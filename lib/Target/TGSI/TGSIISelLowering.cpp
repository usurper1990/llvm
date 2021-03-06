//===-- TGSIISelLowering.cpp - TGSI DAG Lowering Implementation ---------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the interfaces that TGSI uses to lower LLVM code into a
// selection DAG.
//
//===----------------------------------------------------------------------===//

#include "TGSIISelLowering.h"
#include "TGSIRegisterInfo.h"
#include "TGSITargetMachine.h"
#include "TGSITargetObjectFile.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/ManagedStatic.h"
using namespace llvm;


//===----------------------------------------------------------------------===//
// Calling Convention Implementation
//===----------------------------------------------------------------------===//

#include "TGSIGenCallingConv.inc"

SDValue
TGSITargetLowering::LowerReturn(SDValue Chain,
                                CallingConv::ID CallConv, bool isVarArg,
                                const SmallVectorImpl<ISD::OutputArg> &Outs,
                                const SmallVectorImpl<SDValue> &OutVals,
                                const SDLoc &dl, SelectionDAG &DAG) const {
   SDValue Flag;

   // CCValAssign - represent the assignment of the return value to locations.
   SmallVector<CCValAssign, 16> RVLocs;

   // CCState - Info about the registers and stack slot.
   CCState ccinfo(CallConv, isVarArg, DAG.getMachineFunction(),
                  RVLocs, *DAG.getContext());

   ccinfo.AnalyzeReturn(Outs, RetCC_TGSI);

   for (unsigned i = 0, e = RVLocs.size(); i != e; ++i) {
      CCValAssign& VA = RVLocs[i];

      assert(VA.isRegLoc() && "CCValAssign must be RegLoc");

      unsigned Reg = VA.getLocReg();

      Chain = DAG.getCopyToReg(Chain, dl, Reg, OutVals[i], Flag);

      // Guarantee that all emitted copies are stuck together.
      Flag = Chain.getValue(1);
   }

   if (Flag.getNode())
      return DAG.getNode(TGSIISD::RET, dl, MVT::Other, Chain, Flag);
   else
      return DAG.getNode(TGSIISD::RET, dl, MVT::Other, Chain);
}

SDValue
TGSITargetLowering::LowerFormalArguments(SDValue Chain,
                                         CallingConv::ID CallConv, bool isVarArg,
                                         const SmallVectorImpl<ISD::InputArg> &Ins,
                                         const SDLoc &dl, SelectionDAG &DAG,
                                         SmallVectorImpl<SDValue> &InVals) const {
   MachineFunction &mf = DAG.getMachineFunction();
   MachineRegisterInfo &reginfo = mf.getRegInfo();

   // Assign locations to all of the incoming arguments.
   SmallVector<CCValAssign, 16> ArgLocs;
   CCState ccinfo(CallConv, isVarArg, DAG.getMachineFunction(),
                  ArgLocs, *DAG.getContext());
   CCAssignFn *ccfn = KernCC_TGSI;
   // (isKernelFunction(mf.getFunction()) ? KernCC_TGSI : FunCC_TGSI);

   ccinfo.AnalyzeFormalArguments(Ins, ccfn);

   for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i) {
      CCValAssign &va = ArgLocs[i];
      MVT vt = va.getValVT();
      SDValue arg;

      if (va.isRegLoc()) {
         unsigned vreg = reginfo.createVirtualRegister(getRegClassFor(vt));

         reginfo.addLiveIn(va.getLocReg(), vreg);
         arg = DAG.getCopyFromReg(Chain, dl, vreg, vt);

      } else {
         SDValue ptr = DAG.getConstant(va.getLocMemOffset(), dl, MVT::i32);

         arg = DAG.getNode(TGSIISD::LOAD_INPUT, dl, vt, Chain, ptr);
      }

      InVals.push_back(arg);
   }

   return Chain;
}

SDValue
TGSITargetLowering::LowerCall(TargetLowering::CallLoweringInfo &CLI,
                              SmallVectorImpl<SDValue> &InVals) const {
   SelectionDAG &DAG = CLI.DAG;
   SDLoc dl = CLI.DL;
   SmallVectorImpl<ISD::OutputArg> &Outs = CLI.Outs;
   SmallVectorImpl<SDValue> &OutVals = CLI.OutVals;
   SmallVectorImpl<ISD::InputArg> &Ins = CLI.Ins;
   SDValue Chain = CLI.Chain;
   SDValue Callee = CLI.Callee;
   bool &isTailCall = CLI.IsTailCall;
   CallingConv::ID CallConv = CLI.CallConv;
   bool isVarArg = CLI.IsVarArg;

   SDValue InFlag;
   // Analyze operands of the call, assigning locations to each operand.
   SmallVector<CCValAssign, 16> ArgLocs;
   CCState ccinfo(CallConv, isVarArg, DAG.getMachineFunction(),
                  ArgLocs, *DAG.getContext());
   ccinfo.AnalyzeCallOperands(Outs, FunCC_TGSI);

   // Walk the register assignments, inserting copies.
   for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i) {
      CCValAssign &VA = ArgLocs[i];
      SDValue Arg = OutVals[i];

      if (VA.getLocVT() == MVT::f32)
         Arg = DAG.getNode(ISD::BITCAST, dl, MVT::i32, Arg);

      Chain = DAG.getCopyToReg(Chain, dl, VA.getLocReg(), Arg, InFlag);
      InFlag = Chain.getValue(1);
   }

   // Returns a chain & a flag for retval copy to use
   SDVTList NodeTys = DAG.getVTList(MVT::Other, MVT::Glue);
   SmallVector<SDValue, 8> Ops;
   Ops.push_back(Chain);
   Ops.push_back(Callee);
   for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i) {
      unsigned Reg = ArgLocs[i].getLocReg();

      Ops.push_back(DAG.getRegister(Reg, ArgLocs[i].getLocVT()));
   }
   if (InFlag.getNode())
      Ops.push_back(InFlag);

   Chain = DAG.getNode(TGSIISD::CALL, dl, NodeTys, Ops);
   InFlag = Chain.getValue(1);

   // Assign locations to each value returned by this call.
   SmallVector<CCValAssign, 16> RVLocs;
   CCState RVInfo(CallConv, isVarArg, DAG.getMachineFunction(),
                  RVLocs, *DAG.getContext());

   RVInfo.AnalyzeCallResult(Ins, RetCC_TGSI);

   // Copy all of the result registers out of their specified physreg.
   for (unsigned i = 0; i != RVLocs.size(); ++i) {
      unsigned Reg = RVLocs[i].getLocReg();

      Chain = DAG.getCopyFromReg(Chain, dl, Reg,
                                 RVLocs[i].getValVT(), InFlag).getValue(1);
      InFlag = Chain.getValue(2);
      InVals.push_back(Chain.getValue(0));
   }

   isTailCall = false;
   return Chain;
}

//===----------------------------------------------------------------------===//
// TargetLowering Implementation
//===----------------------------------------------------------------------===//

TGSITargetLowering::TGSITargetLowering(TargetMachine &TM,
                                       const TGSISubtarget &STI)
   : TargetLowering(TM), Subtarget(&STI) {

   // Set up the register classes.
   addRegisterClass(MVT::i32, &TGSI::IRegsRegClass);
   addRegisterClass(MVT::v4i32, &TGSI::IVRegsRegClass);
   addRegisterClass(MVT::f32, &TGSI::FRegsRegClass);
   addRegisterClass(MVT::v4f32, &TGSI::FVRegsRegClass);

   setStackPointerRegisterToSaveRestore(TGSI::TEMP0);
   computeRegisterProperties(Subtarget->getRegisterInfo());

   setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i16, Expand);
   setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i8, Expand);
   setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i1, Expand);
   setOperationAction(ISD::SDIVREM, MVT::i32, Expand);
   setOperationAction(ISD::UDIVREM, MVT::i32, Expand);
   setOperationAction(ISD::UMUL_LOHI, MVT::i32, Expand);
   setOperationAction(ISD::SMUL_LOHI, MVT::i32, Expand);
   setOperationAction(ISD::STACKSAVE, MVT::Other, Expand);
   setOperationAction(ISD::STACKRESTORE, MVT::Other, Expand);
   setOperationAction(ISD::BR_CC, MVT::i32, Expand);
   setOperationAction(ISD::BR_CC, MVT::f32, Expand);
   setOperationAction(ISD::BR_CC, MVT::f64, Expand);
   setOperationAction(ISD::BR_CC, MVT::Other, Expand);
   setOperationAction(ISD::SELECT_CC, MVT::i32, Expand);
   setOperationAction(ISD::SELECT_CC, MVT::f32, Expand);
   setOperationAction(ISD::SELECT_CC, MVT::Other, Expand);

   setOperationAction(ISD::ConstantFP, MVT::f32, Legal);
   setOperationAction(ISD::FGETSIGN, MVT::f32, Legal);
   setOperationAction(ISD::FLOG2, MVT::f32, Legal);
   setOperationAction(ISD::FEXP2, MVT::f32, Legal);

   setOperationAction(ISD::BUILD_VECTOR, MVT::v4i32, Expand);
   setOperationAction(ISD::BUILD_VECTOR, MVT::v4f32, Expand);
   setOperationAction(ISD::EXTRACT_VECTOR_ELT, MVT::v4i32, Expand);
   setOperationAction(ISD::EXTRACT_VECTOR_ELT, MVT::v4f32, Expand);

   setOperationAction(ISD::INTRINSIC_WO_CHAIN, MVT::Other, Custom);
   setOperationAction(ISD::ADDE, MVT::i8, Expand);
   setOperationAction(ISD::ADDE, MVT::i16, Expand);
   setOperationAction(ISD::ADDE, MVT::i32, Expand);
   setOperationAction(ISD::ADDE, MVT::i64, Expand);
   setOperationAction(ISD::SUBE, MVT::i8, Expand);
   setOperationAction(ISD::SUBE, MVT::i16, Expand);
   setOperationAction(ISD::SUBE, MVT::i32, Expand);
   setOperationAction(ISD::SUBE, MVT::i64, Expand);
   setOperationAction(ISD::ADDC, MVT::i8, Expand);
   setOperationAction(ISD::ADDC, MVT::i16, Expand);
   setOperationAction(ISD::ADDC, MVT::i32, Expand);
   setOperationAction(ISD::ADDC, MVT::i64, Expand);
   setOperationAction(ISD::SUBC, MVT::i8, Expand);
   setOperationAction(ISD::SUBC, MVT::i16, Expand);
   setOperationAction(ISD::SUBC, MVT::i32, Expand);
   setOperationAction(ISD::SUBC, MVT::i64, Expand);

   // setOperationAction(ISD::FSUB, MVT::f32, Expand);
   setOperationAction(ISD::ROTL, MVT::i32, Expand);
   setOperationAction(ISD::ROTL, MVT::i64, Expand);
   setOperationAction(ISD::ROTR, MVT::i32, Expand);
   setOperationAction(ISD::ROTR, MVT::i64, Expand);
   
   setCondCodeAction(ISD::SETUO, MVT::f32, Expand);
   setCondCodeAction(ISD::SETUO, MVT::f64, Expand);
   setCondCodeAction(ISD::SETO, MVT::f32, Expand);
   setCondCodeAction(ISD::SETO, MVT::f64, Expand);
   
   // No Integer SETLE and SETGT and the unsigned counterparts.
   setCondCodeAction(ISD::SETLE, MVT::i32, Expand);
   setCondCodeAction(ISD::SETLE, MVT::i64, Expand);
   setCondCodeAction(ISD::SETGT, MVT::i32, Expand);
   setCondCodeAction(ISD::SETGT, MVT::i64, Expand);
   setCondCodeAction(ISD::SETULE, MVT::i32, Expand);
   setCondCodeAction(ISD::SETULE, MVT::i64, Expand);
   setCondCodeAction(ISD::SETUGT, MVT::i32, Expand);
   setCondCodeAction(ISD::SETUGT, MVT::i64, Expand);
   
   setCondCodeAction(ISD::SETUEQ, MVT::f32, Expand);
   setCondCodeAction(ISD::SETUEQ, MVT::f64, Expand);
   setCondCodeAction(ISD::SETUGT, MVT::f32, Expand);
   setCondCodeAction(ISD::SETUGT, MVT::f64, Expand);
   setCondCodeAction(ISD::SETOGT, MVT::f32, Expand);
   setCondCodeAction(ISD::SETOGT, MVT::f64, Expand);
   setCondCodeAction(ISD::SETUGE, MVT::f32, Expand);
   setCondCodeAction(ISD::SETUGE, MVT::f64, Expand);
   setCondCodeAction(ISD::SETULT, MVT::f32, Expand);
   setCondCodeAction(ISD::SETULT, MVT::f64, Expand);
   setCondCodeAction(ISD::SETULE, MVT::f32, Expand);
   setCondCodeAction(ISD::SETULE, MVT::f64, Expand);
   setCondCodeAction(ISD::SETOLE, MVT::f32, Expand);
   setCondCodeAction(ISD::SETOLE, MVT::f64, Expand);
   setCondCodeAction(ISD::SETONE, MVT::f32, Expand);
   setCondCodeAction(ISD::SETONE, MVT::f64, Expand);
   setCondCodeAction(ISD::SETO, MVT::f32, Expand);
   setCondCodeAction(ISD::SETO, MVT::f64, Expand);
   setCondCodeAction(ISD::SETUO, MVT::f32, Expand);
   setCondCodeAction(ISD::SETUO, MVT::f64, Expand);
   
}

const char *TGSITargetLowering::getTargetNodeName(unsigned Opcode) const {
   switch (Opcode) {
      case TGSIISD::LOAD_INPUT:
         return "TGSIISD::LOAD_INPUT";
      case TGSIISD::CALL:
         return "TGSIISD::CALL";
      case TGSIISD::RET:
         return "TGSIISD::RET";
      default:
         llvm_unreachable("Invalid opcode");
   }
}

SDValue TGSITargetLowering::
CreateLiveInRegister(SelectionDAG &DAG, const TargetRegisterClass *RC,
                     unsigned Reg, EVT VT) const {
   MachineFunction &MF = DAG.getMachineFunction();
   MachineRegisterInfo &MRI = MF.getRegInfo();
   unsigned VirtualRegister;
   if (!MRI.isLiveIn(Reg)) {
      VirtualRegister = MRI.createVirtualRegister(RC);
      MRI.addLiveIn(Reg, VirtualRegister);
   } else {
      VirtualRegister = MRI.getLiveInVirtReg(Reg);
   }
   return DAG.getRegister(VirtualRegister, VT);
}

SDValue TGSITargetLowering::
LowerOperation(SDValue op, SelectionDAG &dag) const {
   switch (op.getOpcode()) {
   case ISD::INTRINSIC_WO_CHAIN: {
      unsigned IntrinsicID =
                         cast<ConstantSDNode>(op.getOperand(0))->getZExtValue();
      EVT VT = op.getValueType();
      switch(IntrinsicID) {
      case Intrinsic::tgsi_read_blockid_x:
         return CreateLiveInRegister(dag, &TGSI::IRegsRegClass,
                                     TGSI_BLOCK_ID(x), VT);
      case Intrinsic::tgsi_read_blockid_y:
         return CreateLiveInRegister(dag, &TGSI::IRegsRegClass,
                                     TGSI_BLOCK_ID(y), VT);
      case Intrinsic::tgsi_read_blockid_z:
         return CreateLiveInRegister(dag, &TGSI::IRegsRegClass,
                                     TGSI_BLOCK_ID(z), VT);
      case Intrinsic::tgsi_read_blocksize_x:
         return CreateLiveInRegister(dag, &TGSI::IRegsRegClass,
                                     TGSI_BLOCK_SIZE(x), VT);
      case Intrinsic::tgsi_read_blocksize_y:
         return CreateLiveInRegister(dag, &TGSI::IRegsRegClass,
                                     TGSI_BLOCK_SIZE(y), VT);
      case Intrinsic::tgsi_read_blocksize_z:
         return CreateLiveInRegister(dag, &TGSI::IRegsRegClass,
                                     TGSI_BLOCK_SIZE(z), VT);
      case Intrinsic::tgsi_read_gridsize_x:
         return CreateLiveInRegister(dag, &TGSI::IRegsRegClass,
                                     TGSI_GRID_SIZE(x), VT);
      case Intrinsic::tgsi_read_gridsize_y:
         return CreateLiveInRegister(dag, &TGSI::IRegsRegClass,
                                     TGSI_GRID_SIZE(y), VT);
      case Intrinsic::tgsi_read_gridsize_z:
         return CreateLiveInRegister(dag, &TGSI::IRegsRegClass,
                                     TGSI_GRID_SIZE(z), VT);
      case Intrinsic::tgsi_read_threadid_x:
         return CreateLiveInRegister(dag, &TGSI::IRegsRegClass,
                                     TGSI_THREAD_ID(x), VT);
      case Intrinsic::tgsi_read_threadid_y:
         return CreateLiveInRegister(dag, &TGSI::IRegsRegClass,
                                     TGSI_THREAD_ID(y), VT);
      case Intrinsic::tgsi_read_threadid_z:
         return CreateLiveInRegister(dag, &TGSI::IRegsRegClass,
                                     TGSI_THREAD_ID(z), VT);
      case Intrinsic::tgsi_read_workdim:
         return CreateLiveInRegister(dag, &TGSI::IRegsRegClass,
                                     TGSI_WORK_DIM, VT);
      default:
         llvm_unreachable("Unknown TGSI Intrinsic");
      }
      break; /* Never reached */
      }
   default:
      llvm_unreachable("Should not custom lower this!");
   }
}


// Pin TGSISection's and TGSITargetObjectFile's vtables to this file.
void TGSISection::anchor() {}

TGSITargetObjectFile::~TGSITargetObjectFile() {
  delete static_cast<TGSISection *>(TextSection);
  delete static_cast<TGSISection *>(DataSection);
  delete static_cast<TGSISection *>(BSSSection);
  delete static_cast<TGSISection *>(ReadOnlySection);

  delete static_cast<TGSISection *>(StaticCtorSection);
  delete static_cast<TGSISection *>(StaticDtorSection);
  delete static_cast<TGSISection *>(LSDASection);
  delete static_cast<TGSISection *>(EHFrameSection);
  delete static_cast<TGSISection *>(DwarfAbbrevSection);
  delete static_cast<TGSISection *>(DwarfInfoSection);
  delete static_cast<TGSISection *>(DwarfLineSection);
  delete static_cast<TGSISection *>(DwarfFrameSection);
  delete static_cast<TGSISection *>(DwarfPubTypesSection);
  delete static_cast<const TGSISection *>(DwarfDebugInlineSection);
  delete static_cast<TGSISection *>(DwarfStrSection);
  delete static_cast<TGSISection *>(DwarfLocSection);
  delete static_cast<TGSISection *>(DwarfARangesSection);
  delete static_cast<TGSISection *>(DwarfRangesSection);
}

MCSection *
TGSITargetObjectFile::SelectSectionForGlobal(const GlobalObject *GO,
                                              SectionKind Kind,
                                              const TargetMachine &TM) const {
  return getDataSection();
}
