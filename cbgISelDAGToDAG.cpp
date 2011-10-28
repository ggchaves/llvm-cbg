//===-- cbgISelDAGToDAG.cpp - A dag to dag inst selector for cbg ------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines an instruction selector for the SPARC target.
//
//===----------------------------------------------------------------------===//

#include "cbgTargetMachine.h"
#include "llvm/Intrinsics.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

//===----------------------------------------------------------------------===//
// Instruction Selector Implementation
//===----------------------------------------------------------------------===//

//===--------------------------------------------------------------------===//
/// cbgDAGToDAGISel - CBG specific code to select CBG machine
/// instructions for SelectionDAG operations.
///
namespace {
class cbgDAGToDAGISel : public SelectionDAGISel {
  /// Subtarget - Keep a pointer to the Sparc Subtarget around so that we can
  /// make the right decision when generating code for different targets.
  const cbgSubtarget &Subtarget;
  cbgTargetMachine& TM;
public:
  explicit cbgDAGToDAGISel(cbgTargetMachine &tm)
    : SelectionDAGISel(tm),
      Subtarget(tm.getSubtarget<cbgSubtarget>()),
      TM(tm) {
  }

  SDNode *Select(SDNode *N);

  // Complex Pattern Selectors.
  bool SelectADDRrr(SDValue N, SDValue &R1, SDValue &R2);
  bool SelectADDRri(SDValue N, SDValue &Base, SDValue &Offset);

  /// SelectInlineAsmMemoryOperand - Implement addressing mode selection for
  /// inline asm expressions.
  virtual bool SelectInlineAsmMemoryOperand(const SDValue &Op,
                                            char ConstraintCode,
                                            std::vector<SDValue> &OutOps);

  virtual const char *getPassName() const {
    return "CBG DAG->DAG Pattern Instruction Selection";
  }

  // Include the pieces autogenerated from the target description.
#include "cbgGenDAGISel.inc"

private:
  SDNode* getGlobalBaseReg();
};
}  // end anonymous namespace

SDNode* cbgDAGToDAGISel::getGlobalBaseReg() {
  unsigned GlobalBaseReg = TM.getInstrInfo()->getGlobalBaseReg(MF);
  return CurDAG->getRegister(GlobalBaseReg, TLI.getPointerTy()).getNode();
}

bool cbgDAGToDAGISel::SelectADDRri(SDValue Addr,
                                     SDValue &Base, SDValue &Offset) {
  if (FrameIndexSDNode *FIN = dyn_cast<FrameIndexSDNode>(Addr)) {
    Base = CurDAG->getTargetFrameIndex(FIN->getIndex(), MVT::i32);
    Offset = CurDAG->getTargetConstant(0, MVT::i32);
    return true;
  }
  if (Addr.getOpcode() == ISD::TargetExternalSymbol ||
      Addr.getOpcode() == ISD::TargetGlobalAddress)
    return false;  // direct calls.

  if (Addr.getOpcode() == ISD::ADD) {
    if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(Addr.getOperand(1))) {
      if (isInt<13>(CN->getSExtValue())) {
        if (FrameIndexSDNode *FIN =
                dyn_cast<FrameIndexSDNode>(Addr.getOperand(0))) {
          // Constant offset from frame ref.
          Base = CurDAG->getTargetFrameIndex(FIN->getIndex(), MVT::i32);
        } else {
          Base = Addr.getOperand(0);
        }
        Offset = CurDAG->getTargetConstant(CN->getZExtValue(), MVT::i32);
        return true;
      }
    }
    if (Addr.getOperand(0).getOpcode() == SPISD::Lo) {
      Base = Addr.getOperand(1);
      Offset = Addr.getOperand(0).getOperand(0);
      return true;
    }
    if (Addr.getOperand(1).getOpcode() == SPISD::Lo) {
      Base = Addr.getOperand(0);
      Offset = Addr.getOperand(1).getOperand(0);
      return true;
    }
  }
  Base = Addr;
  Offset = CurDAG->getTargetConstant(0, MVT::i32);
  return true;
}

bool cbgDAGToDAGISel::SelectADDRrr(SDValue Addr, SDValue &R1, SDValue &R2) {
  if (Addr.getOpcode() == ISD::FrameIndex) return false;
  if (Addr.getOpcode() == ISD::TargetExternalSymbol ||
      Addr.getOpcode() == ISD::TargetGlobalAddress)
    return false;  // direct calls.

  if (Addr.getOpcode() == ISD::ADD) {
    if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(Addr.getOperand(1)))
      if (isInt<13>(CN->getSExtValue()))
        return false;  // Let the reg+imm pattern catch this!
    if (Addr.getOperand(0).getOpcode() == SPISD::Lo ||
        Addr.getOperand(1).getOpcode() == SPISD::Lo)
      return false;  // Let the reg+imm pattern catch this!
    R1 = Addr.getOperand(0);
    R2 = Addr.getOperand(1);
    return true;
  }

  R1 = Addr;
  R2 = CurDAG->getRegister(CBG::G0, MVT::i32);
  return true;
}

SDNode *cbgDAGToDAGISel::Select(SDNode *N) {
  DebugLoc dl = N->getDebugLoc();
  if (N->isMachineOpcode())
    return NULL;   // Already selected.

  switch (N->getOpcode()) {
  default: break;
  case SPISD::GLOBAL_BASE_REG:
    return getGlobalBaseReg();

  case ISD::SDIV:
  case ISD::UDIV: {
    // FIXME: should use a custom expander to expose the SRA to the dag.
    SDValue DivLHS = N->getOperand(0);
    SDValue DivRHS = N->getOperand(1);

    // Set the Y register to the high-part.
    SDValue TopPart;
    if (N->getOpcode() == ISD::SDIV) {
      TopPart = SDValue(CurDAG->getMachineNode(CBG::SRAri, dl, MVT::i32, DivLHS,
                                   CurDAG->getTargetConstant(31, MVT::i32)), 0);
    } else {
      TopPart = CurDAG->getRegister(CBG::G0, MVT::i32);
    }
    TopPart = SDValue(CurDAG->getMachineNode(CBG::WRYrr, dl, MVT::Glue, TopPart,
                                     CurDAG->getRegister(CBG::G0, MVT::i32)), 0);

    // FIXME: Handle div by immediate.
    unsigned Opcode = N->getOpcode() == ISD::SDIV ? CBG::SDIVrr : CBG::UDIVrr;
    return CurDAG->SelectNodeTo(N, Opcode, MVT::i32, DivLHS, DivRHS,
                                TopPart);
  }
  case ISD::MULHU:
  case ISD::MULHS: {
    // FIXME: Handle mul by immediate.
    SDValue MulLHS = N->getOperand(0);
    SDValue MulRHS = N->getOperand(1);
    unsigned Opcode = N->getOpcode() == ISD::MULHU ? CBG::UMULrr : CBG::SMULrr;
    SDNode *Mul = CurDAG->getMachineNode(Opcode, dl, MVT::i32, MVT::Glue,
                                         MulLHS, MulRHS);
    // The high part is in the Y register.
    return CurDAG->SelectNodeTo(N, CBG::RDY, MVT::i32, SDValue(Mul, 1));
    return NULL;
  }
  }

  return SelectCode(N);
}


/// SelectInlineAsmMemoryOperand - Implement addressing mode selection for
/// inline asm expressions.
bool
cbgDAGToDAGISel::SelectInlineAsmMemoryOperand(const SDValue &Op,
                                                char ConstraintCode,
                                                std::vector<SDValue> &OutOps) {
  SDValue Op0, Op1;
  switch (ConstraintCode) {
  default: return true;
  case 'm':   // memory
   if (!SelectADDRrr(Op, Op0, Op1))
     SelectADDRri(Op, Op0, Op1);
   break;
  }

  OutOps.push_back(Op0);
  OutOps.push_back(Op1);
  return false;
}

/// createSparcISelDag - This pass converts a legalized DAG into a
/// SPARC-specific DAG, ready for instruction scheduling.
///
FunctionPass *llvm::createcbgISelDag(cbgTargetMachine &TM) {
  return new cbgDAGToDAGISel(TM);
}
