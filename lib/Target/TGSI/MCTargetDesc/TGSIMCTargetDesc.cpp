//===-- TGSIMCTargetDesc.cpp - TGSI Target Descriptions --------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file provides TGSI specific target descriptions.
//
//===----------------------------------------------------------------------===//

#include "TGSIASMStreamer.h"
#include "TGSIMCTargetDesc.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/Support/TargetRegistry.h"

#define GET_INSTRINFO_MC_DESC
#include "TGSIGenInstrInfo.inc"

#define GET_SUBTARGETINFO_MC_DESC
#include "TGSIGenSubtargetInfo.inc"

#define GET_REGINFO_MC_DESC
#include "TGSIGenRegisterInfo.inc"

using namespace llvm;

namespace {
   struct TGSIMCAsmInfo : public MCAsmInfo {
      explicit TGSIMCAsmInfo(const Triple &TT) {
         IsLittleEndian = true;
         HasDotTypeDotSizeDirective = false;
         HasSingleParameterDotFile = false;
      }
   };
}

static MCInstrInfo *createTGSIMCInstrInfo() {
   MCInstrInfo *X = new MCInstrInfo();
   InitTGSIMCInstrInfo(X);
   return X;
}

static MCRegisterInfo *createTGSIMCRegisterInfo(const Triple &TT) {
   MCRegisterInfo *X = new MCRegisterInfo();
   InitTGSIMCRegisterInfo(X, TGSI::ADDR0);
   return X;
}

static MCSubtargetInfo *createTGSIMCSubtargetInfo(const Triple &TT,
                                                  StringRef CPU,
                                                  StringRef FS) {
   return createTGSIMCSubtargetInfoImpl(TT, CPU, FS);
}

extern "C" void LLVMInitializeTGSITargetMC() {
   RegisterMCAsmInfo<TGSIMCAsmInfo> regMCAsmInfo(TheTGSITarget);

   TargetRegistry::RegisterMCInstrInfo(TheTGSITarget, createTGSIMCInstrInfo);
   TargetRegistry::RegisterMCRegInfo(TheTGSITarget, createTGSIMCRegisterInfo);
   TargetRegistry::RegisterMCSubtargetInfo(TheTGSITarget,
                                           createTGSIMCSubtargetInfo);
   TargetRegistry::RegisterMCInstPrinter(TheTGSITarget,
                                         createTGSIMCInstPrinter);

   TargetRegistry::RegisterAsmTargetStreamer(TheTGSITarget,
                                             createTGSITargetAsmStreamer);
   TargetRegistry::RegisterNullTargetStreamer(TheTGSITarget,
                                              createTGSINullTargetStreamer);
}
