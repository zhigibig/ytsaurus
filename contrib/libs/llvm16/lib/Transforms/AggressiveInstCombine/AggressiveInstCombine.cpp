//===- AggressiveInstCombine.cpp ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the aggressive expression pattern combiner classes.
// Currently, it handles expression patterns for:
//  * Truncate instruction
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/AggressiveInstCombine/AggressiveInstCombine.h"
#include "AggressiveInstCombineInternal.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Transforms/Utils/BuildLibCalls.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;
using namespace PatternMatch;

#define DEBUG_TYPE "aggressive-instcombine"

STATISTIC(NumAnyOrAllBitsSet, "Number of any/all-bits-set patterns folded");
STATISTIC(NumGuardedRotates,
          "Number of guarded rotates transformed into funnel shifts");
STATISTIC(NumGuardedFunnelShifts,
          "Number of guarded funnel shifts transformed into funnel shifts");
STATISTIC(NumPopCountRecognized, "Number of popcount idioms recognized");

static cl::opt<unsigned> MaxInstrsToScan(
    "aggressive-instcombine-max-scan-instrs", cl::init(64), cl::Hidden,
    cl::desc("Max number of instructions to scan for aggressive instcombine."));

/// Match a pattern for a bitwise funnel/rotate operation that partially guards
/// against undefined behavior by branching around the funnel-shift/rotation
/// when the shift amount is 0.
static bool foldGuardedFunnelShift(Instruction &I, const DominatorTree &DT) {
  if (I.getOpcode() != Instruction::PHI || I.getNumOperands() != 2)
    return false;

  // As with the one-use checks below, this is not strictly necessary, but we
  // are being cautious to avoid potential perf regressions on targets that
  // do not actually have a funnel/rotate instruction (where the funnel shift
  // would be expanded back into math/shift/logic ops).
  if (!isPowerOf2_32(I.getType()->getScalarSizeInBits()))
    return false;

  // Match V to funnel shift left/right and capture the source operands and
  // shift amount.
  auto matchFunnelShift = [](Value *V, Value *&ShVal0, Value *&ShVal1,
                             Value *&ShAmt) {
    Value *SubAmt;
    unsigned Width = V->getType()->getScalarSizeInBits();

    // fshl(ShVal0, ShVal1, ShAmt)
    //  == (ShVal0 << ShAmt) | (ShVal1 >> (Width -ShAmt))
    if (match(V, m_OneUse(m_c_Or(
                     m_Shl(m_Value(ShVal0), m_Value(ShAmt)),
                     m_LShr(m_Value(ShVal1),
                            m_Sub(m_SpecificInt(Width), m_Value(SubAmt))))))) {
      if (ShAmt == SubAmt) // TODO: Use m_Specific
        return Intrinsic::fshl;
    }

    // fshr(ShVal0, ShVal1, ShAmt)
    //  == (ShVal0 >> ShAmt) | (ShVal1 << (Width - ShAmt))
    if (match(V,
              m_OneUse(m_c_Or(m_Shl(m_Value(ShVal0), m_Sub(m_SpecificInt(Width),
                                                           m_Value(SubAmt))),
                              m_LShr(m_Value(ShVal1), m_Value(ShAmt)))))) {
      if (ShAmt == SubAmt) // TODO: Use m_Specific
        return Intrinsic::fshr;
    }

    return Intrinsic::not_intrinsic;
  };

  // One phi operand must be a funnel/rotate operation, and the other phi
  // operand must be the source value of that funnel/rotate operation:
  // phi [ rotate(RotSrc, ShAmt), FunnelBB ], [ RotSrc, GuardBB ]
  // phi [ fshl(ShVal0, ShVal1, ShAmt), FunnelBB ], [ ShVal0, GuardBB ]
  // phi [ fshr(ShVal0, ShVal1, ShAmt), FunnelBB ], [ ShVal1, GuardBB ]
  PHINode &Phi = cast<PHINode>(I);
  unsigned FunnelOp = 0, GuardOp = 1;
  Value *P0 = Phi.getOperand(0), *P1 = Phi.getOperand(1);
  Value *ShVal0, *ShVal1, *ShAmt;
  Intrinsic::ID IID = matchFunnelShift(P0, ShVal0, ShVal1, ShAmt);
  if (IID == Intrinsic::not_intrinsic ||
      (IID == Intrinsic::fshl && ShVal0 != P1) ||
      (IID == Intrinsic::fshr && ShVal1 != P1)) {
    IID = matchFunnelShift(P1, ShVal0, ShVal1, ShAmt);
    if (IID == Intrinsic::not_intrinsic ||
        (IID == Intrinsic::fshl && ShVal0 != P0) ||
        (IID == Intrinsic::fshr && ShVal1 != P0))
      return false;
    assert((IID == Intrinsic::fshl || IID == Intrinsic::fshr) &&
           "Pattern must match funnel shift left or right");
    std::swap(FunnelOp, GuardOp);
  }

  // The incoming block with our source operand must be the "guard" block.
  // That must contain a cmp+branch to avoid the funnel/rotate when the shift
  // amount is equal to 0. The other incoming block is the block with the
  // funnel/rotate.
  BasicBlock *GuardBB = Phi.getIncomingBlock(GuardOp);
  BasicBlock *FunnelBB = Phi.getIncomingBlock(FunnelOp);
  Instruction *TermI = GuardBB->getTerminator();

  // Ensure that the shift values dominate each block.
  if (!DT.dominates(ShVal0, TermI) || !DT.dominates(ShVal1, TermI))
    return false;

  ICmpInst::Predicate Pred;
  BasicBlock *PhiBB = Phi.getParent();
  if (!match(TermI, m_Br(m_ICmp(Pred, m_Specific(ShAmt), m_ZeroInt()),
                         m_SpecificBB(PhiBB), m_SpecificBB(FunnelBB))))
    return false;

  if (Pred != CmpInst::ICMP_EQ)
    return false;

  IRBuilder<> Builder(PhiBB, PhiBB->getFirstInsertionPt());

  if (ShVal0 == ShVal1)
    ++NumGuardedRotates;
  else
    ++NumGuardedFunnelShifts;

  // If this is not a rotate then the select was blocking poison from the
  // 'shift-by-zero' non-TVal, but a funnel shift won't - so freeze it.
  bool IsFshl = IID == Intrinsic::fshl;
  if (ShVal0 != ShVal1) {
    if (IsFshl && !llvm::isGuaranteedNotToBePoison(ShVal1))
      ShVal1 = Builder.CreateFreeze(ShVal1);
    else if (!IsFshl && !llvm::isGuaranteedNotToBePoison(ShVal0))
      ShVal0 = Builder.CreateFreeze(ShVal0);
  }

  // We matched a variation of this IR pattern:
  // GuardBB:
  //   %cmp = icmp eq i32 %ShAmt, 0
  //   br i1 %cmp, label %PhiBB, label %FunnelBB
  // FunnelBB:
  //   %sub = sub i32 32, %ShAmt
  //   %shr = lshr i32 %ShVal1, %sub
  //   %shl = shl i32 %ShVal0, %ShAmt
  //   %fsh = or i32 %shr, %shl
  //   br label %PhiBB
  // PhiBB:
  //   %cond = phi i32 [ %fsh, %FunnelBB ], [ %ShVal0, %GuardBB ]
  // -->
  // llvm.fshl.i32(i32 %ShVal0, i32 %ShVal1, i32 %ShAmt)
  Function *F = Intrinsic::getDeclaration(Phi.getModule(), IID, Phi.getType());
  Phi.replaceAllUsesWith(Builder.CreateCall(F, {ShVal0, ShVal1, ShAmt}));
  return true;
}

/// This is used by foldAnyOrAllBitsSet() to capture a source value (Root) and
/// the bit indexes (Mask) needed by a masked compare. If we're matching a chain
/// of 'and' ops, then we also need to capture the fact that we saw an
/// "and X, 1", so that's an extra return value for that case.
struct MaskOps {
  Value *Root = nullptr;
  APInt Mask;
  bool MatchAndChain;
  bool FoundAnd1 = false;

  MaskOps(unsigned BitWidth, bool MatchAnds)
      : Mask(APInt::getZero(BitWidth)), MatchAndChain(MatchAnds) {}
};

/// This is a recursive helper for foldAnyOrAllBitsSet() that walks through a
/// chain of 'and' or 'or' instructions looking for shift ops of a common source
/// value. Examples:
///   or (or (or X, (X >> 3)), (X >> 5)), (X >> 8)
/// returns { X, 0x129 }
///   and (and (X >> 1), 1), (X >> 4)
/// returns { X, 0x12 }
static bool matchAndOrChain(Value *V, MaskOps &MOps) {
  Value *Op0, *Op1;
  if (MOps.MatchAndChain) {
    // Recurse through a chain of 'and' operands. This requires an extra check
    // vs. the 'or' matcher: we must find an "and X, 1" instruction somewhere
    // in the chain to know that all of the high bits are cleared.
    if (match(V, m_And(m_Value(Op0), m_One()))) {
      MOps.FoundAnd1 = true;
      return matchAndOrChain(Op0, MOps);
    }
    if (match(V, m_And(m_Value(Op0), m_Value(Op1))))
      return matchAndOrChain(Op0, MOps) && matchAndOrChain(Op1, MOps);
  } else {
    // Recurse through a chain of 'or' operands.
    if (match(V, m_Or(m_Value(Op0), m_Value(Op1))))
      return matchAndOrChain(Op0, MOps) && matchAndOrChain(Op1, MOps);
  }

  // We need a shift-right or a bare value representing a compare of bit 0 of
  // the original source operand.
  Value *Candidate;
  const APInt *BitIndex = nullptr;
  if (!match(V, m_LShr(m_Value(Candidate), m_APInt(BitIndex))))
    Candidate = V;

  // Initialize result source operand.
  if (!MOps.Root)
    MOps.Root = Candidate;

  // The shift constant is out-of-range? This code hasn't been simplified.
  if (BitIndex && BitIndex->uge(MOps.Mask.getBitWidth()))
    return false;

  // Fill in the mask bit derived from the shift constant.
  MOps.Mask.setBit(BitIndex ? BitIndex->getZExtValue() : 0);
  return MOps.Root == Candidate;
}

/// Match patterns that correspond to "any-bits-set" and "all-bits-set".
/// These will include a chain of 'or' or 'and'-shifted bits from a
/// common source value:
/// and (or  (lshr X, C), ...), 1 --> (X & CMask) != 0
/// and (and (lshr X, C), ...), 1 --> (X & CMask) == CMask
/// Note: "any-bits-clear" and "all-bits-clear" are variations of these patterns
/// that differ only with a final 'not' of the result. We expect that final
/// 'not' to be folded with the compare that we create here (invert predicate).
static bool foldAnyOrAllBitsSet(Instruction &I) {
  // The 'any-bits-set' ('or' chain) pattern is simpler to match because the
  // final "and X, 1" instruction must be the final op in the sequence.
  bool MatchAllBitsSet;
  if (match(&I, m_c_And(m_OneUse(m_And(m_Value(), m_Value())), m_Value())))
    MatchAllBitsSet = true;
  else if (match(&I, m_And(m_OneUse(m_Or(m_Value(), m_Value())), m_One())))
    MatchAllBitsSet = false;
  else
    return false;

  MaskOps MOps(I.getType()->getScalarSizeInBits(), MatchAllBitsSet);
  if (MatchAllBitsSet) {
    if (!matchAndOrChain(cast<BinaryOperator>(&I), MOps) || !MOps.FoundAnd1)
      return false;
  } else {
    if (!matchAndOrChain(cast<BinaryOperator>(&I)->getOperand(0), MOps))
      return false;
  }

  // The pattern was found. Create a masked compare that replaces all of the
  // shift and logic ops.
  IRBuilder<> Builder(&I);
  Constant *Mask = ConstantInt::get(I.getType(), MOps.Mask);
  Value *And = Builder.CreateAnd(MOps.Root, Mask);
  Value *Cmp = MatchAllBitsSet ? Builder.CreateICmpEQ(And, Mask)
                               : Builder.CreateIsNotNull(And);
  Value *Zext = Builder.CreateZExt(Cmp, I.getType());
  I.replaceAllUsesWith(Zext);
  ++NumAnyOrAllBitsSet;
  return true;
}

// Try to recognize below function as popcount intrinsic.
// This is the "best" algorithm from
// http://graphics.stanford.edu/~seander/bithacks.html#CountBitsSetParallel
// Also used in TargetLowering::expandCTPOP().
//
// int popcount(unsigned int i) {
//   i = i - ((i >> 1) & 0x55555555);
//   i = (i & 0x33333333) + ((i >> 2) & 0x33333333);
//   i = ((i + (i >> 4)) & 0x0F0F0F0F);
//   return (i * 0x01010101) >> 24;
// }
static bool tryToRecognizePopCount(Instruction &I) {
  if (I.getOpcode() != Instruction::LShr)
    return false;

  Type *Ty = I.getType();
  if (!Ty->isIntOrIntVectorTy())
    return false;

  unsigned Len = Ty->getScalarSizeInBits();
  // FIXME: fix Len == 8 and other irregular type lengths.
  if (!(Len <= 128 && Len > 8 && Len % 8 == 0))
    return false;

  APInt Mask55 = APInt::getSplat(Len, APInt(8, 0x55));
  APInt Mask33 = APInt::getSplat(Len, APInt(8, 0x33));
  APInt Mask0F = APInt::getSplat(Len, APInt(8, 0x0F));
  APInt Mask01 = APInt::getSplat(Len, APInt(8, 0x01));
  APInt MaskShift = APInt(Len, Len - 8);

  Value *Op0 = I.getOperand(0);
  Value *Op1 = I.getOperand(1);
  Value *MulOp0;
  // Matching "(i * 0x01010101...) >> 24".
  if ((match(Op0, m_Mul(m_Value(MulOp0), m_SpecificInt(Mask01)))) &&
       match(Op1, m_SpecificInt(MaskShift))) {
    Value *ShiftOp0;
    // Matching "((i + (i >> 4)) & 0x0F0F0F0F...)".
    if (match(MulOp0, m_And(m_c_Add(m_LShr(m_Value(ShiftOp0), m_SpecificInt(4)),
                                    m_Deferred(ShiftOp0)),
                            m_SpecificInt(Mask0F)))) {
      Value *AndOp0;
      // Matching "(i & 0x33333333...) + ((i >> 2) & 0x33333333...)".
      if (match(ShiftOp0,
                m_c_Add(m_And(m_Value(AndOp0), m_SpecificInt(Mask33)),
                        m_And(m_LShr(m_Deferred(AndOp0), m_SpecificInt(2)),
                              m_SpecificInt(Mask33))))) {
        Value *Root, *SubOp1;
        // Matching "i - ((i >> 1) & 0x55555555...)".
        if (match(AndOp0, m_Sub(m_Value(Root), m_Value(SubOp1))) &&
            match(SubOp1, m_And(m_LShr(m_Specific(Root), m_SpecificInt(1)),
                                m_SpecificInt(Mask55)))) {
          LLVM_DEBUG(dbgs() << "Recognized popcount intrinsic\n");
          IRBuilder<> Builder(&I);
          Function *Func = Intrinsic::getDeclaration(
              I.getModule(), Intrinsic::ctpop, I.getType());
          I.replaceAllUsesWith(Builder.CreateCall(Func, {Root}));
          ++NumPopCountRecognized;
          return true;
        }
      }
    }
  }

  return false;
}

/// Fold smin(smax(fptosi(x), C1), C2) to llvm.fptosi.sat(x), providing C1 and
/// C2 saturate the value of the fp conversion. The transform is not reversable
/// as the fptosi.sat is more defined than the input - all values produce a
/// valid value for the fptosi.sat, where as some produce poison for original
/// that were out of range of the integer conversion. The reversed pattern may
/// use fmax and fmin instead. As we cannot directly reverse the transform, and
/// it is not always profitable, we make it conditional on the cost being
/// reported as lower by TTI.
static bool tryToFPToSat(Instruction &I, TargetTransformInfo &TTI) {
  // Look for min(max(fptosi, converting to fptosi_sat.
  Value *In;
  const APInt *MinC, *MaxC;
  if (!match(&I, m_SMax(m_OneUse(m_SMin(m_OneUse(m_FPToSI(m_Value(In))),
                                        m_APInt(MinC))),
                        m_APInt(MaxC))) &&
      !match(&I, m_SMin(m_OneUse(m_SMax(m_OneUse(m_FPToSI(m_Value(In))),
                                        m_APInt(MaxC))),
                        m_APInt(MinC))))
    return false;

  // Check that the constants clamp a saturate.
  if (!(*MinC + 1).isPowerOf2() || -*MaxC != *MinC + 1)
    return false;

  Type *IntTy = I.getType();
  Type *FpTy = In->getType();
  Type *SatTy =
      IntegerType::get(IntTy->getContext(), (*MinC + 1).exactLogBase2() + 1);
  if (auto *VecTy = dyn_cast<VectorType>(IntTy))
    SatTy = VectorType::get(SatTy, VecTy->getElementCount());

  // Get the cost of the intrinsic, and check that against the cost of
  // fptosi+smin+smax
  InstructionCost SatCost = TTI.getIntrinsicInstrCost(
      IntrinsicCostAttributes(Intrinsic::fptosi_sat, SatTy, {In}, {FpTy}),
      TTI::TCK_RecipThroughput);
  SatCost += TTI.getCastInstrCost(Instruction::SExt, SatTy, IntTy,
                                  TTI::CastContextHint::None,
                                  TTI::TCK_RecipThroughput);

  InstructionCost MinMaxCost = TTI.getCastInstrCost(
      Instruction::FPToSI, IntTy, FpTy, TTI::CastContextHint::None,
      TTI::TCK_RecipThroughput);
  MinMaxCost += TTI.getIntrinsicInstrCost(
      IntrinsicCostAttributes(Intrinsic::smin, IntTy, {IntTy}),
      TTI::TCK_RecipThroughput);
  MinMaxCost += TTI.getIntrinsicInstrCost(
      IntrinsicCostAttributes(Intrinsic::smax, IntTy, {IntTy}),
      TTI::TCK_RecipThroughput);

  if (SatCost >= MinMaxCost)
    return false;

  IRBuilder<> Builder(&I);
  Function *Fn = Intrinsic::getDeclaration(I.getModule(), Intrinsic::fptosi_sat,
                                           {SatTy, FpTy});
  Value *Sat = Builder.CreateCall(Fn, In);
  I.replaceAllUsesWith(Builder.CreateSExt(Sat, IntTy));
  return true;
}

/// Try to replace a mathlib call to sqrt with the LLVM intrinsic. This avoids
/// pessimistic codegen that has to account for setting errno and can enable
/// vectorization.
static bool
foldSqrt(Instruction &I, TargetTransformInfo &TTI, TargetLibraryInfo &TLI) {
  // Match a call to sqrt mathlib function.
  auto *Call = dyn_cast<CallInst>(&I);
  if (!Call)
    return false;

  Module *M = Call->getModule();
  LibFunc Func;
  if (!TLI.getLibFunc(*Call, Func) || !isLibFuncEmittable(M, &TLI, Func))
    return false;

  if (Func != LibFunc_sqrt && Func != LibFunc_sqrtf && Func != LibFunc_sqrtl)
    return false;

  // If (1) this is a sqrt libcall, (2) we can assume that NAN is not created
  // (because NNAN or the operand arg must not be less than -0.0) and (2) we
  // would not end up lowering to a libcall anyway (which could change the value
  // of errno), then:
  // (1) errno won't be set.
  // (2) it is safe to convert this to an intrinsic call.
  Type *Ty = Call->getType();
  Value *Arg = Call->getArgOperand(0);
  if (TTI.haveFastSqrt(Ty) &&
      (Call->hasNoNaNs() || CannotBeOrderedLessThanZero(Arg, &TLI))) {
    IRBuilder<> Builder(&I);
    IRBuilderBase::FastMathFlagGuard Guard(Builder);
    Builder.setFastMathFlags(Call->getFastMathFlags());

    Function *Sqrt = Intrinsic::getDeclaration(M, Intrinsic::sqrt, Ty);
    Value *NewSqrt = Builder.CreateCall(Sqrt, Arg, "sqrt");
    I.replaceAllUsesWith(NewSqrt);

    // Explicitly erase the old call because a call with side effects is not
    // trivially dead.
    I.eraseFromParent();
    return true;
  }

  return false;
}

// Check if this array of constants represents a cttz table.
// Iterate over the elements from \p Table by trying to find/match all
// the numbers from 0 to \p InputBits that should represent cttz results.
static bool isCTTZTable(const ConstantDataArray &Table, uint64_t Mul,
                        uint64_t Shift, uint64_t InputBits) {
  unsigned Length = Table.getNumElements();
  if (Length < InputBits || Length > InputBits * 2)
    return false;

  APInt Mask = APInt::getBitsSetFrom(InputBits, Shift);
  unsigned Matched = 0;

  for (unsigned i = 0; i < Length; i++) {
    uint64_t Element = Table.getElementAsInteger(i);
    if (Element >= InputBits)
      continue;

    // Check if \p Element matches a concrete answer. It could fail for some
    // elements that are never accessed, so we keep iterating over each element
    // from the table. The number of matched elements should be equal to the
    // number of potential right answers which is \p InputBits actually.
    if ((((Mul << Element) & Mask.getZExtValue()) >> Shift) == i)
      Matched++;
  }

  return Matched == InputBits;
}

// Try to recognize table-based ctz implementation.
// E.g., an example in C (for more cases please see the llvm/tests):
// int f(unsigned x) {
//    static const char table[32] =
//      {0, 1, 28, 2, 29, 14, 24, 3, 30,
//       22, 20, 15, 25, 17, 4, 8, 31, 27,
//       13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9};
//    return table[((unsigned)((x & -x) * 0x077CB531U)) >> 27];
// }
// this can be lowered to `cttz` instruction.
// There is also a special case when the element is 0.
//
// Here are some examples or LLVM IR for a 64-bit target:
//
// CASE 1:
// %sub = sub i32 0, %x
// %and = and i32 %sub, %x
// %mul = mul i32 %and, 125613361
// %shr = lshr i32 %mul, 27
// %idxprom = zext i32 %shr to i64
// %arrayidx = getelementptr inbounds [32 x i8], [32 x i8]* @ctz1.table, i64 0,
// i64 %idxprom %0 = load i8, i8* %arrayidx, align 1, !tbaa !8
//
// CASE 2:
// %sub = sub i32 0, %x
// %and = and i32 %sub, %x
// %mul = mul i32 %and, 72416175
// %shr = lshr i32 %mul, 26
// %idxprom = zext i32 %shr to i64
// %arrayidx = getelementptr inbounds [64 x i16], [64 x i16]* @ctz2.table, i64
// 0, i64 %idxprom %0 = load i16, i16* %arrayidx, align 2, !tbaa !8
//
// CASE 3:
// %sub = sub i32 0, %x
// %and = and i32 %sub, %x
// %mul = mul i32 %and, 81224991
// %shr = lshr i32 %mul, 27
// %idxprom = zext i32 %shr to i64
// %arrayidx = getelementptr inbounds [32 x i32], [32 x i32]* @ctz3.table, i64
// 0, i64 %idxprom %0 = load i32, i32* %arrayidx, align 4, !tbaa !8
//
// CASE 4:
// %sub = sub i64 0, %x
// %and = and i64 %sub, %x
// %mul = mul i64 %and, 283881067100198605
// %shr = lshr i64 %mul, 58
// %arrayidx = getelementptr inbounds [64 x i8], [64 x i8]* @table, i64 0, i64
// %shr %0 = load i8, i8* %arrayidx, align 1, !tbaa !8
//
// All this can be lowered to @llvm.cttz.i32/64 intrinsic.
static bool tryToRecognizeTableBasedCttz(Instruction &I) {
  LoadInst *LI = dyn_cast<LoadInst>(&I);
  if (!LI)
    return false;

  Type *AccessType = LI->getType();
  if (!AccessType->isIntegerTy())
    return false;

  GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(LI->getPointerOperand());
  if (!GEP || !GEP->isInBounds() || GEP->getNumIndices() != 2)
    return false;

  if (!GEP->getSourceElementType()->isArrayTy())
    return false;

  uint64_t ArraySize = GEP->getSourceElementType()->getArrayNumElements();
  if (ArraySize != 32 && ArraySize != 64)
    return false;

  GlobalVariable *GVTable = dyn_cast<GlobalVariable>(GEP->getPointerOperand());
  if (!GVTable || !GVTable->hasInitializer() || !GVTable->isConstant())
    return false;

  ConstantDataArray *ConstData =
      dyn_cast<ConstantDataArray>(GVTable->getInitializer());
  if (!ConstData)
    return false;

  if (!match(GEP->idx_begin()->get(), m_ZeroInt()))
    return false;

  Value *Idx2 = std::next(GEP->idx_begin())->get();
  Value *X1;
  uint64_t MulConst, ShiftConst;
  // FIXME: 64-bit targets have `i64` type for the GEP index, so this match will
  // probably fail for other (e.g. 32-bit) targets.
  if (!match(Idx2, m_ZExtOrSelf(
                       m_LShr(m_Mul(m_c_And(m_Neg(m_Value(X1)), m_Deferred(X1)),
                                    m_ConstantInt(MulConst)),
                              m_ConstantInt(ShiftConst)))))
    return false;

  unsigned InputBits = X1->getType()->getScalarSizeInBits();
  if (InputBits != 32 && InputBits != 64)
    return false;

  // Shift should extract top 5..7 bits.
  if (InputBits - Log2_32(InputBits) != ShiftConst &&
      InputBits - Log2_32(InputBits) - 1 != ShiftConst)
    return false;

  if (!isCTTZTable(*ConstData, MulConst, ShiftConst, InputBits))
    return false;

  auto ZeroTableElem = ConstData->getElementAsInteger(0);
  bool DefinedForZero = ZeroTableElem == InputBits;

  IRBuilder<> B(LI);
  ConstantInt *BoolConst = B.getInt1(!DefinedForZero);
  Type *XType = X1->getType();
  auto Cttz = B.CreateIntrinsic(Intrinsic::cttz, {XType}, {X1, BoolConst});
  Value *ZExtOrTrunc = nullptr;

  if (DefinedForZero) {
    ZExtOrTrunc = B.CreateZExtOrTrunc(Cttz, AccessType);
  } else {
    // If the value in elem 0 isn't the same as InputBits, we still want to
    // produce the value from the table.
    auto Cmp = B.CreateICmpEQ(X1, ConstantInt::get(XType, 0));
    auto Select =
        B.CreateSelect(Cmp, ConstantInt::get(XType, ZeroTableElem), Cttz);

    // NOTE: If the table[0] is 0, but the cttz(0) is defined by the Target
    // it should be handled as: `cttz(x) & (typeSize - 1)`.

    ZExtOrTrunc = B.CreateZExtOrTrunc(Select, AccessType);
  }

  LI->replaceAllUsesWith(ZExtOrTrunc);

  return true;
}

/// This is used by foldLoadsRecursive() to capture a Root Load node which is
/// of type or(load, load) and recursively build the wide load. Also capture the
/// shift amount, zero extend type and loadSize.
struct LoadOps {
  LoadInst *Root = nullptr;
  LoadInst *RootInsert = nullptr;
  bool FoundRoot = false;
  uint64_t LoadSize = 0;
  Value *Shift = nullptr;
  Type *ZextType;
  AAMDNodes AATags;
};

// Identify and Merge consecutive loads recursively which is of the form
// (ZExt(L1) << shift1) | (ZExt(L2) << shift2) -> ZExt(L3) << shift1
// (ZExt(L1) << shift1) | ZExt(L2) -> ZExt(L3)
static bool foldLoadsRecursive(Value *V, LoadOps &LOps, const DataLayout &DL,
                               AliasAnalysis &AA) {
  Value *ShAmt2 = nullptr;
  Value *X;
  Instruction *L1, *L2;

  // Go to the last node with loads.
  if (match(V, m_OneUse(m_c_Or(
                   m_Value(X),
                   m_OneUse(m_Shl(m_OneUse(m_ZExt(m_OneUse(m_Instruction(L2)))),
                                  m_Value(ShAmt2)))))) ||
      match(V, m_OneUse(m_Or(m_Value(X),
                             m_OneUse(m_ZExt(m_OneUse(m_Instruction(L2)))))))) {
    if (!foldLoadsRecursive(X, LOps, DL, AA) && LOps.FoundRoot)
      // Avoid Partial chain merge.
      return false;
  } else
    return false;

  // Check if the pattern has loads
  LoadInst *LI1 = LOps.Root;
  Value *ShAmt1 = LOps.Shift;
  if (LOps.FoundRoot == false &&
      (match(X, m_OneUse(m_ZExt(m_Instruction(L1)))) ||
       match(X, m_OneUse(m_Shl(m_OneUse(m_ZExt(m_OneUse(m_Instruction(L1)))),
                               m_Value(ShAmt1)))))) {
    LI1 = dyn_cast<LoadInst>(L1);
  }
  LoadInst *LI2 = dyn_cast<LoadInst>(L2);

  // Check if loads are same, atomic, volatile and having same address space.
  if (LI1 == LI2 || !LI1 || !LI2 || !LI1->isSimple() || !LI2->isSimple() ||
      LI1->getPointerAddressSpace() != LI2->getPointerAddressSpace())
    return false;

  // Check if Loads come from same BB.
  if (LI1->getParent() != LI2->getParent())
    return false;

  // Find the data layout
  bool IsBigEndian = DL.isBigEndian();

  // Check if loads are consecutive and same size.
  Value *Load1Ptr = LI1->getPointerOperand();
  APInt Offset1(DL.getIndexTypeSizeInBits(Load1Ptr->getType()), 0);
  Load1Ptr =
      Load1Ptr->stripAndAccumulateConstantOffsets(DL, Offset1,
                                                  /* AllowNonInbounds */ true);

  Value *Load2Ptr = LI2->getPointerOperand();
  APInt Offset2(DL.getIndexTypeSizeInBits(Load2Ptr->getType()), 0);
  Load2Ptr =
      Load2Ptr->stripAndAccumulateConstantOffsets(DL, Offset2,
                                                  /* AllowNonInbounds */ true);

  // Verify if both loads have same base pointers and load sizes are same.
  uint64_t LoadSize1 = LI1->getType()->getPrimitiveSizeInBits();
  uint64_t LoadSize2 = LI2->getType()->getPrimitiveSizeInBits();
  if (Load1Ptr != Load2Ptr || LoadSize1 != LoadSize2)
    return false;

  // Support Loadsizes greater or equal to 8bits and only power of 2.
  if (LoadSize1 < 8 || !isPowerOf2_64(LoadSize1))
    return false;

  // Alias Analysis to check for stores b/w the loads.
  LoadInst *Start = LOps.FoundRoot ? LOps.RootInsert : LI1, *End = LI2;
  MemoryLocation Loc;
  if (!Start->comesBefore(End)) {
    std::swap(Start, End);
    Loc = MemoryLocation::get(End);
    if (LOps.FoundRoot)
      Loc = Loc.getWithNewSize(LOps.LoadSize);
  } else
    Loc = MemoryLocation::get(End);
  unsigned NumScanned = 0;
  for (Instruction &Inst :
       make_range(Start->getIterator(), End->getIterator())) {
    if (Inst.mayWriteToMemory() && isModSet(AA.getModRefInfo(&Inst, Loc)))
      return false;
    if (++NumScanned > MaxInstrsToScan)
      return false;
  }

  // Make sure Load with lower Offset is at LI1
  bool Reverse = false;
  if (Offset2.slt(Offset1)) {
    std::swap(LI1, LI2);
    std::swap(ShAmt1, ShAmt2);
    std::swap(Offset1, Offset2);
    std::swap(Load1Ptr, Load2Ptr);
    std::swap(LoadSize1, LoadSize2);
    Reverse = true;
  }

  // Big endian swap the shifts
  if (IsBigEndian)
    std::swap(ShAmt1, ShAmt2);

  // Find Shifts values.
  const APInt *Temp;
  uint64_t Shift1 = 0, Shift2 = 0;
  if (ShAmt1 && match(ShAmt1, m_APInt(Temp)))
    Shift1 = Temp->getZExtValue();
  if (ShAmt2 && match(ShAmt2, m_APInt(Temp)))
    Shift2 = Temp->getZExtValue();

  // First load is always LI1. This is where we put the new load.
  // Use the merged load size available from LI1 for forward loads.
  if (LOps.FoundRoot) {
    if (!Reverse)
      LoadSize1 = LOps.LoadSize;
    else
      LoadSize2 = LOps.LoadSize;
  }

  // Verify if shift amount and load index aligns and verifies that loads
  // are consecutive.
  uint64_t ShiftDiff = IsBigEndian ? LoadSize2 : LoadSize1;
  uint64_t PrevSize =
      DL.getTypeStoreSize(IntegerType::get(LI1->getContext(), LoadSize1));
  if ((Shift2 - Shift1) != ShiftDiff || (Offset2 - Offset1) != PrevSize)
    return false;

  // Update LOps
  AAMDNodes AATags1 = LOps.AATags;
  AAMDNodes AATags2 = LI2->getAAMetadata();
  if (LOps.FoundRoot == false) {
    LOps.FoundRoot = true;
    AATags1 = LI1->getAAMetadata();
  }
  LOps.LoadSize = LoadSize1 + LoadSize2;
  LOps.RootInsert = Start;

  // Concatenate the AATags of the Merged Loads.
  LOps.AATags = AATags1.concat(AATags2);

  LOps.Root = LI1;
  LOps.Shift = ShAmt1;
  LOps.ZextType = X->getType();
  return true;
}

// For a given BB instruction, evaluate all loads in the chain that form a
// pattern which suggests that the loads can be combined. The one and only use
// of the loads is to form a wider load.
static bool foldConsecutiveLoads(Instruction &I, const DataLayout &DL,
                                 TargetTransformInfo &TTI, AliasAnalysis &AA) {
  // Only consider load chains of scalar values.
  if (isa<VectorType>(I.getType()))
    return false;

  LoadOps LOps;
  if (!foldLoadsRecursive(&I, LOps, DL, AA) || !LOps.FoundRoot)
    return false;

  IRBuilder<> Builder(&I);
  LoadInst *NewLoad = nullptr, *LI1 = LOps.Root;

  IntegerType *WiderType = IntegerType::get(I.getContext(), LOps.LoadSize);
  // TTI based checks if we want to proceed with wider load
  bool Allowed = TTI.isTypeLegal(WiderType);
  if (!Allowed)
    return false;

  unsigned AS = LI1->getPointerAddressSpace();
  unsigned Fast = 0;
  Allowed = TTI.allowsMisalignedMemoryAccesses(I.getContext(), LOps.LoadSize,
                                               AS, LI1->getAlign(), &Fast);
  if (!Allowed || !Fast)
    return false;

  // Make sure the Load pointer of type GEP/non-GEP is above insert point
  Instruction *Inst = dyn_cast<Instruction>(LI1->getPointerOperand());
  if (Inst && Inst->getParent() == LI1->getParent() &&
      !Inst->comesBefore(LOps.RootInsert))
    Inst->moveBefore(LOps.RootInsert);

  // New load can be generated
  Value *Load1Ptr = LI1->getPointerOperand();
  Builder.SetInsertPoint(LOps.RootInsert);
  Value *NewPtr = Builder.CreateBitCast(Load1Ptr, WiderType->getPointerTo(AS));
  NewLoad = Builder.CreateAlignedLoad(WiderType, NewPtr, LI1->getAlign(),
                                      LI1->isVolatile(), "");
  NewLoad->takeName(LI1);
  // Set the New Load AATags Metadata.
  if (LOps.AATags)
    NewLoad->setAAMetadata(LOps.AATags);

  Value *NewOp = NewLoad;
  // Check if zero extend needed.
  if (LOps.ZextType)
    NewOp = Builder.CreateZExt(NewOp, LOps.ZextType);

  // Check if shift needed. We need to shift with the amount of load1
  // shift if not zero.
  if (LOps.Shift)
    NewOp = Builder.CreateShl(NewOp, LOps.Shift);
  I.replaceAllUsesWith(NewOp);

  return true;
}

/// This is the entry point for folds that could be implemented in regular
/// InstCombine, but they are separated because they are not expected to
/// occur frequently and/or have more than a constant-length pattern match.
static bool foldUnusualPatterns(Function &F, DominatorTree &DT,
                                TargetTransformInfo &TTI,
                                TargetLibraryInfo &TLI, AliasAnalysis &AA) {
  bool MadeChange = false;
  for (BasicBlock &BB : F) {
    // Ignore unreachable basic blocks.
    if (!DT.isReachableFromEntry(&BB))
      continue;

    const DataLayout &DL = F.getParent()->getDataLayout();

    // Walk the block backwards for efficiency. We're matching a chain of
    // use->defs, so we're more likely to succeed by starting from the bottom.
    // Also, we want to avoid matching partial patterns.
    // TODO: It would be more efficient if we removed dead instructions
    // iteratively in this loop rather than waiting until the end.
    for (Instruction &I : make_early_inc_range(llvm::reverse(BB))) {
      MadeChange |= foldAnyOrAllBitsSet(I);
      MadeChange |= foldGuardedFunnelShift(I, DT);
      MadeChange |= tryToRecognizePopCount(I);
      MadeChange |= tryToFPToSat(I, TTI);
      MadeChange |= tryToRecognizeTableBasedCttz(I);
      MadeChange |= foldConsecutiveLoads(I, DL, TTI, AA);
      // NOTE: This function introduces erasing of the instruction `I`, so it
      // needs to be called at the end of this sequence, otherwise we may make
      // bugs.
      MadeChange |= foldSqrt(I, TTI, TLI);
    }
  }

  // We're done with transforms, so remove dead instructions.
  if (MadeChange)
    for (BasicBlock &BB : F)
      SimplifyInstructionsInBlock(&BB);

  return MadeChange;
}

/// This is the entry point for all transforms. Pass manager differences are
/// handled in the callers of this function.
static bool runImpl(Function &F, AssumptionCache &AC, TargetTransformInfo &TTI,
                    TargetLibraryInfo &TLI, DominatorTree &DT,
                    AliasAnalysis &AA) {
  bool MadeChange = false;
  const DataLayout &DL = F.getParent()->getDataLayout();
  TruncInstCombine TIC(AC, TLI, DL, DT);
  MadeChange |= TIC.run(F);
  MadeChange |= foldUnusualPatterns(F, DT, TTI, TLI, AA);
  return MadeChange;
}

PreservedAnalyses AggressiveInstCombinePass::run(Function &F,
                                                 FunctionAnalysisManager &AM) {
  auto &AC = AM.getResult<AssumptionAnalysis>(F);
  auto &TLI = AM.getResult<TargetLibraryAnalysis>(F);
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  auto &TTI = AM.getResult<TargetIRAnalysis>(F);
  auto &AA = AM.getResult<AAManager>(F);
  if (!runImpl(F, AC, TTI, TLI, DT, AA)) {
    // No changes, all analyses are preserved.
    return PreservedAnalyses::all();
  }
  // Mark all the analyses that instcombine updates as preserved.
  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
