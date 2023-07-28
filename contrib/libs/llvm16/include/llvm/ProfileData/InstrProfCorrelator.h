#pragma once

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

//===- InstrProfCorrelator.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// This file defines InstrProfCorrelator used to generate PGO profiles from
// raw profile data and debug info.
//===----------------------------------------------------------------------===//

#ifndef LLVM_PROFILEDATA_INSTRPROFCORRELATOR_H
#define LLVM_PROFILEDATA_INSTRPROFCORRELATOR_H

#include "llvm/ADT/DenseSet.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/YAMLTraits.h"
#include <optional>
#include <vector>

namespace llvm {
class DWARFContext;
class DWARFDie;
namespace object {
class ObjectFile;
}

/// InstrProfCorrelator - A base class used to create raw instrumentation data
/// to their functions.
class InstrProfCorrelator {
public:
  static llvm::Expected<std::unique_ptr<InstrProfCorrelator>>
  get(StringRef DebugInfoFilename);

  /// Construct a ProfileData vector used to correlate raw instrumentation data
  /// to their functions.
  virtual Error correlateProfileData() = 0;

  /// Process debug info and dump the correlation data.
  virtual Error dumpYaml(raw_ostream &OS) = 0;

  /// Return the number of ProfileData elements.
  std::optional<size_t> getDataSize() const;

  /// Return a pointer to the names string that this class constructs.
  const char *getNamesPointer() const { return Names.c_str(); }

  /// Return the number of bytes in the names string.
  size_t getNamesSize() const { return Names.size(); }

  /// Return the size of the counters section in bytes.
  uint64_t getCountersSectionSize() const {
    return Ctx->CountersSectionEnd - Ctx->CountersSectionStart;
  }

  static const char *FunctionNameAttributeName;
  static const char *CFGHashAttributeName;
  static const char *NumCountersAttributeName;

  enum InstrProfCorrelatorKind { CK_32Bit, CK_64Bit };
  InstrProfCorrelatorKind getKind() const { return Kind; }
  virtual ~InstrProfCorrelator() = default;

protected:
  struct Context {
    static llvm::Expected<std::unique_ptr<Context>>
    get(std::unique_ptr<MemoryBuffer> Buffer, const object::ObjectFile &Obj);
    std::unique_ptr<MemoryBuffer> Buffer;
    /// The address range of the __llvm_prf_cnts section.
    uint64_t CountersSectionStart;
    uint64_t CountersSectionEnd;
    /// True if target and host have different endian orders.
    bool ShouldSwapBytes;
  };
  const std::unique_ptr<Context> Ctx;

  InstrProfCorrelator(InstrProfCorrelatorKind K, std::unique_ptr<Context> Ctx)
      : Ctx(std::move(Ctx)), Kind(K) {}

  std::string Names;
  std::vector<std::string> NamesVec;

  struct Probe {
    std::string FunctionName;
    std::optional<std::string> LinkageName;
    yaml::Hex64 CFGHash;
    yaml::Hex64 CounterOffset;
    uint32_t NumCounters;
    std::optional<std::string> FilePath;
    std::optional<int> LineNumber;
  };

  struct CorrelationData {
    std::vector<Probe> Probes;
  };

  friend struct yaml::MappingTraits<Probe>;
  friend struct yaml::SequenceElementTraits<Probe>;
  friend struct yaml::MappingTraits<CorrelationData>;

private:
  static llvm::Expected<std::unique_ptr<InstrProfCorrelator>>
  get(std::unique_ptr<MemoryBuffer> Buffer);

  const InstrProfCorrelatorKind Kind;
};

/// InstrProfCorrelatorImpl - A child of InstrProfCorrelator with a template
/// pointer type so that the ProfileData vector can be materialized.
template <class IntPtrT>
class InstrProfCorrelatorImpl : public InstrProfCorrelator {
public:
  InstrProfCorrelatorImpl(std::unique_ptr<InstrProfCorrelator::Context> Ctx);
  static bool classof(const InstrProfCorrelator *C);

  /// Return a pointer to the underlying ProfileData vector that this class
  /// constructs.
  const RawInstrProf::ProfileData<IntPtrT> *getDataPointer() const {
    return Data.empty() ? nullptr : Data.data();
  }

  /// Return the number of ProfileData elements.
  size_t getDataSize() const { return Data.size(); }

  static llvm::Expected<std::unique_ptr<InstrProfCorrelatorImpl<IntPtrT>>>
  get(std::unique_ptr<InstrProfCorrelator::Context> Ctx,
      const object::ObjectFile &Obj);

protected:
  std::vector<RawInstrProf::ProfileData<IntPtrT>> Data;

  Error correlateProfileData() override;
  virtual void correlateProfileDataImpl(
      InstrProfCorrelator::CorrelationData *Data = nullptr) = 0;

  Error dumpYaml(raw_ostream &OS) override;

  void addProbe(StringRef FunctionName, uint64_t CFGHash, IntPtrT CounterOffset,
                IntPtrT FunctionPtr, uint32_t NumCounters);

private:
  InstrProfCorrelatorImpl(InstrProfCorrelatorKind Kind,
                          std::unique_ptr<InstrProfCorrelator::Context> Ctx)
      : InstrProfCorrelator(Kind, std::move(Ctx)){};
  llvm::DenseSet<IntPtrT> CounterOffsets;

  // Byte-swap the value if necessary.
  template <class T> T maybeSwap(T Value) const {
    return Ctx->ShouldSwapBytes ? sys::getSwappedBytes(Value) : Value;
  }
};

/// DwarfInstrProfCorrelator - A child of InstrProfCorrelatorImpl that takes
/// DWARF debug info as input to correlate profiles.
template <class IntPtrT>
class DwarfInstrProfCorrelator : public InstrProfCorrelatorImpl<IntPtrT> {
public:
  DwarfInstrProfCorrelator(std::unique_ptr<DWARFContext> DICtx,
                           std::unique_ptr<InstrProfCorrelator::Context> Ctx)
      : InstrProfCorrelatorImpl<IntPtrT>(std::move(Ctx)),
        DICtx(std::move(DICtx)) {}

private:
  std::unique_ptr<DWARFContext> DICtx;

  /// Return the address of the object that the provided DIE symbolizes.
  std::optional<uint64_t> getLocation(const DWARFDie &Die) const;

  /// Returns true if the provided DIE symbolizes an instrumentation probe
  /// symbol.
  static bool isDIEOfProbe(const DWARFDie &Die);

  /// Iterate over DWARF DIEs to find those that symbolize instrumentation
  /// probes and construct the ProfileData vector and Names string.
  ///
  /// Here is some example DWARF for an instrumentation probe we are looking
  /// for:
  /// \code
  ///   DW_TAG_subprogram
  ///   DW_AT_low_pc	(0x0000000000000000)
  ///   DW_AT_high_pc	(0x0000000000000014)
  ///   DW_AT_name	("foo")
  ///     DW_TAG_variable
  ///       DW_AT_name	("__profc_foo")
  ///       DW_AT_location	(DW_OP_addr 0x0)
  ///       DW_TAG_LLVM_annotation
  ///         DW_AT_name	("Function Name")
  ///         DW_AT_const_value	("foo")
  ///       DW_TAG_LLVM_annotation
  ///         DW_AT_name	("CFG Hash")
  ///         DW_AT_const_value	(12345678)
  ///       DW_TAG_LLVM_annotation
  ///         DW_AT_name	("Num Counters")
  ///         DW_AT_const_value	(2)
  ///       NULL
  ///     NULL
  /// \endcode
  void correlateProfileDataImpl(
      InstrProfCorrelator::CorrelationData *Data = nullptr) override;
};

} // end namespace llvm

#endif // LLVM_PROFILEDATA_INSTRPROFCORRELATOR_H

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
