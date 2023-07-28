#pragma once

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

//===- MachineUniformityAnalysis.h ---------------------------*- C++ -*----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// \brief Uniformity info and uniformity-aware uniform info for Machine IR
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_MACHINEUNIFORMITYANALYSIS_H
#define LLVM_CODEGEN_MACHINEUNIFORMITYANALYSIS_H

#include "llvm/ADT/GenericUniformityInfo.h"
#include "llvm/CodeGen/MachineCycleAnalysis.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineSSAContext.h"

namespace llvm {

extern template class GenericUniformityInfo<MachineSSAContext>;
using MachineUniformityInfo = GenericUniformityInfo<MachineSSAContext>;

/// \brief Compute the uniform information of a Machine IR function.
MachineUniformityInfo
computeMachineUniformityInfo(MachineFunction &F,
                             const MachineCycleInfo &cycleInfo,
                             const MachineDomTree &domTree);

} // namespace llvm

#endif // LLVM_CODEGEN_MACHINEUNIFORMITYANALYSIS_H

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
