#pragma once

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

//===- MCSectionDXContainer.h - DXContainer MC Sections ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the MCSectionDXContainer class.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_MC_MCSECTIONDXCONTAINER_H
#define LLVM_MC_MCSECTIONDXCONTAINER_H

#include "llvm/MC/MCSection.h"
#include "llvm/MC/SectionKind.h"

namespace llvm {

class MCSymbol;

class MCSectionDXContainer final : public MCSection {
  friend class MCContext;

  MCSectionDXContainer(StringRef Name, SectionKind K, MCSymbol *Begin)
      : MCSection(SV_DXContainer, Name, K, Begin) {}

public:
  void printSwitchToSection(const MCAsmInfo &, const Triple &, raw_ostream &,
                            const MCExpr *) const override;
  bool useCodeAlign() const override { return false; }
  bool isVirtualSection() const override { return false; }
};

} // end namespace llvm

#endif // LLVM_MC_MCSECTIONDXCONTAINER_H

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
