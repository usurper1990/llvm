//===-- TGSIAsmPrinter.cpp - TGSI LLVM assembly writer ------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains a printer that converts from our internal representation
// of machine-dependent LLVM code to GAS-format TGSI assembly language.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "asm-printer"
#include "TGSI.h"
#include "TGSIInstrInfo.h"
#include "TGSITargetMachine.h"
#include "MCTargetDesc/TGSITargetStreamer.h"

#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/IR/Mangler.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetLowering.h"
#include "llvm/Target/TargetLoweringObjectFile.h"

using namespace llvm;

namespace {
   class TGSIAsmPrinter : public AsmPrinter {
      unsigned cpi;
      unsigned instructionCount;
   public:
      explicit TGSIAsmPrinter(TargetMachine &TM,
                              std::unique_ptr<MCStreamer> Streamer)
         : AsmPrinter(TM, std::move(Streamer)), cpi(0),
           instructionCount(0) {}

      virtual StringRef getPassName() const {
         return "TGSI Assembly Printer";
      }

      virtual void EmitInstruction(const MachineInstr *mi);
      virtual void EmitFunctionHeader() override;
      virtual void EmitFunctionBodyStart();
      virtual void EmitFunctionBodyEnd();
      virtual void EmitConstantPool() override;
      virtual void EmitStartOfAsmFile(Module &) override;
      virtual void EmitBasicBlockStart(const MachineBasicBlock &MBB) const override;
      virtual void emitImplicitDef(const MachineInstr *MI) const override;

      void printOperand(const MachineInstr *MI, int opNum, raw_ostream &OS);
      void printInstruction(const MachineInstr *MI, raw_ostream &OS);
      static const char *getRegisterName(unsigned RegNo);
   private:
      void EmitMCInstruction(const MCInst &mci);
      void AddFunctionStartAddressMetadata();
   };
}

static MCSymbol *
GetSymbolFromOperand(const MachineOperand &mo, AsmPrinter &ap) {
   SmallString<128> name;
   const TargetMachine &TM = ap.TM;
   Mangler &Mang = TM.getObjFileLowering()->getMangler();

   if (mo.isGlobal()) {
      const GlobalValue *gv = mo.getGlobal();
      TM.getNameWithPrefix(name, gv, Mang);
   } else {
      assert(mo.isSymbol() && "Isn't a symbol reference");
      Mangler::getNameWithPrefix(name, mo.getSymbolName(), ap.getDataLayout());
   }

   return ap.OutContext.getOrCreateSymbol(name);
}

static MCOperand
GetSymbolRef(const MachineOperand &mo, const MCSymbol *sym,
             AsmPrinter &printer) {
   MCContext &ctx = printer.OutContext;
   const MCExpr *Expr = MCSymbolRefExpr::create(sym, MCSymbolRefExpr::VK_None,
                                                ctx);

   if (mo.getOffset())
      Expr = MCBinaryExpr::createAdd(Expr,
                                     MCConstantExpr::create(mo.getOffset(), ctx),
                                     ctx);

   return MCOperand::createExpr(Expr);
}

static void LowerMachineInstrToMCInst(const MachineInstr *mi, MCInst &mci,
                                      AsmPrinter &ap) {
   mci.setOpcode(mi->getOpcode());

   for (unsigned i = 0; i != mi->getNumOperands(); ++i) {
      const MachineOperand &mo = mi->getOperand(i);
      MCOperand mco;

      switch (mo.getType()) {
         case MachineOperand::MO_Register:
            mco = MCOperand::createReg(mo.getReg());
            break;
         case MachineOperand::MO_Immediate:
            mco = MCOperand::createImm(mo.getImm());
            break;
/*          mco = MCOperand::createExpr(MCSymbolRefExpr::create(
                     ap.GetCPISymbol(mo.getImm()), ap.OutContext));
         case MachineOperand::MO_FPImmediate:
            mco = MCOperand::createFPImm(mo.getFPImm()->getValueAPF()
                                         .convertToFloat());
            break; */
         case MachineOperand::MO_MachineBasicBlock:
            mco = MCOperand::createExpr(MCSymbolRefExpr::create
                                        (mo.getMBB()->getSymbol(), ap.OutContext));
            break;
         case MachineOperand::MO_GlobalAddress:
         case MachineOperand::MO_ExternalSymbol:
            mco = GetSymbolRef(mo, GetSymbolFromOperand(mo, ap), ap);
            break;
         case MachineOperand::MO_ConstantPoolIndex:
            mco = GetSymbolRef(mo, ap.GetCPISymbol(mo.getIndex()), ap);
            break;
         case MachineOperand::MO_BlockAddress:
            mco = GetSymbolRef(mo, ap.GetBlockAddressSymbol(mo.getBlockAddress()),
                               ap);
            break;
         default:
            assert(0);
            break;
      }

      mci.addOperand(mco);
   }
}

void TGSIAsmPrinter::EmitMCInstruction(const MCInst &mci) {
   MCTargetStreamer &TS = *OutStreamer->getTargetStreamer();
   TGSITargetStreamer &TTS = static_cast<TGSITargetStreamer &>(TS);

   TTS.EmitInstructionLabel(instructionCount);
   OutStreamer->EmitInstruction(mci, getSubtargetInfo());
   instructionCount++;
}

void TGSIAsmPrinter::AddFunctionStartAddressMetadata() {
   Function *f = const_cast<Function *>(MF->getFunction());
   LLVMContext &ctx = f->getContext();
   auto *start = ConstantAsMetadata::get(
                    ConstantInt::get(Type::getInt32Ty(ctx), instructionCount));
   f->addMetadata("tgsi_kernel_start", *MDNode::get(ctx, { start }));
}

void TGSIAsmPrinter::EmitInstruction(const MachineInstr *mi) {
   MCInst mci;

   LowerMachineInstrToMCInst(mi, mci, *this);
   EmitMCInstruction(mci);
}

void TGSIAsmPrinter::EmitStartOfAsmFile(Module &M)
{
   MCTargetStreamer &TS = *OutStreamer->getTargetStreamer();
   TGSITargetStreamer &TTS = static_cast<TGSITargetStreamer &>(TS);
   TTS.EmitStartOfAsmFile();
}

void TGSIAsmPrinter::EmitFunctionHeader() {
   EmitConstantPool();
   OutStreamer->AddBlankLine();
}

void TGSIAsmPrinter::EmitFunctionBodyStart() {
   MCInst mci;

   mci.setOpcode(TGSI::BGNSUB);
   AddFunctionStartAddressMetadata();
   EmitMCInstruction(mci);
}

void TGSIAsmPrinter::EmitFunctionBodyEnd() {
   MCInst mci;
   mci.setOpcode(TGSI::ENDSUB);
   EmitMCInstruction(mci);
}

void TGSIAsmPrinter::EmitConstantPool() {
   TGSITargetMachine &TTM = static_cast<TGSITargetMachine &>(TM);
   const std::vector<MachineConstantPoolEntry> &CP = TTM.MCP.getConstants();

   if (CP.empty()) return;

   MCTargetStreamer &TS = *OutStreamer->getTargetStreamer();
   TGSITargetStreamer &TTS = static_cast<TGSITargetStreamer &>(TS);

   while (cpi < CP.size())
      TTS.EmitConstantPoolEntry(CP[cpi++]);
}

void TGSIAsmPrinter::EmitBasicBlockStart(const MachineBasicBlock &MBB) const {
   // Do nothing so as to avoid emitting labels.
   return;
}

void TGSIAsmPrinter::emitImplicitDef(const MachineInstr *MI) const {
   // Do nothing so as to avoid emitting labels.
   return;
}

extern "C" void LLVMInitializeTGSIAsmPrinter() {
   RegisterAsmPrinter<TGSIAsmPrinter> X(TheTGSITarget);
}
