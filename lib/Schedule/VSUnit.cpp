//===------------ VSUnit.cpp - Translate LLVM IR to VSUnit  -----*- C++ -*-===//
//
// Copyright: 2011 by SYSU EDA Group. all rights reserved.
// IMPORTANT: This software is supplied to you by Hongbin Zheng in consideration
// of your agreement to the following terms, and your use, installation,
// modification or redistribution of this software constitutes acceptance
// of these terms.  If you do not agree with these terms, please do not use,
// install, modify or redistribute this software. You may not redistribute,
// install copy or modify this software without written permission from
// Hongbin Zheng.
//
//===----------------------------------------------------------------------===//
//
// This file implement the VSUnit class, which represent the basic atom
// operation in hardware.
//
//===----------------------------------------------------------------------===//

#include "VSUnit.h"
#include "ScheduleDOT.h"

#include "vtm/VerilogBackendMCTargetDesc.h"
#include "vtm/SynSettings.h"
#include "vtm/VFInfo.h"

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "vtm-sunit"
#include "llvm/Support/Debug.h"

using namespace llvm;

static cl::opt<bool>
ScheduleDataPathALAP("vtm-schedule-datapath-alap",
                     cl::desc("Schedule datapath operaton As Last As Possible "
                              "to allow more effecient resource sharing"),
                     cl::init(true)),
// FIXME: When multi-cycles chain is disabled, also set the chaining threshold
// of all functional units to 0 to avoid chaining.
DisableMultiCyclesChain("vtm-disable-multi-cycles-chain",
                        cl::desc("Disable multi-cycles chaining "
                                 "(manually setting all chaining threshold to "
                                 "0 is need)"),
                        cl::init(false));

//===----------------------------------------------------------------------===//
void VSchedGraph::print(raw_ostream &OS) const {
  getEntryBB()->dump();
}

void VSchedGraph::dump() const {
  print(dbgs());
}

bool VSchedGraph::trySetLoopOp(MachineInstr *MI) {
  assert(MI->getDesc().isTerminator() && "Bad instruction!");

  if (!VInstrInfo::isBrCndLike(MI->getOpcode())) return false;

  if (MI->getOperand(1).getMBB() != getEntryBB()) return false;

  // Ok, remember this instruction as self enable.
  LoopOp.setPointer(MI);
  return true;
}

bool VSchedGraph::isLoopPHIMove(MachineInstr *MI) {
  assert(MI->getOpcode() == VTM::VOpMvPhi && "Bad opcode!");

  return MI->getOperand(2).getMBB() == getEntryBB() && enablePipeLine();
}

void VSchedGraph::verify() const {
  if (getEntryRoot()->num_deps())
    llvm_unreachable("Entry root should not have any dependence!");
  if (getExitRoot()->num_uses())
    llvm_unreachable("Exit root should not have any use!");

  for (const_iterator I = cp_begin(), E = cp_end(); I != E; ++I)
    verifySU(*I);

  //for (const_iterator I = dp_begin(), E = dp_end(); I != E; ++I)
  //  verifySU(*I);
}

void VSchedGraph::verifySU(const VSUnit *SU) const {
  typedef VSUnit::const_dep_iterator dep_it;

  bool IsBBEntry = SU->getRepresentativePtr().isMBB();
  MachineBasicBlock *ParentMBB = SU->getParentBB();
  bool AnyDepFromTheSameParent = IsBBEntry;

  for (dep_it DI = SU->dep_begin(), DE = SU->dep_end(); DI != DE; ++DI) {
    const VSUnit *Dep = *DI;
    assert((DI.getEdgeType() == VDEdge::MemDep
            || SU->getIdx() > Dep->getIdx())
           && "Bad value dependent edge!");
    assert((!IsBBEntry || (Dep->getRepresentativePtr()->isTerminator()
                           && DI.getLatency() == 0))
           && "Bad inter BB dependent edge.");
    AnyDepFromTheSameParent |= DI->getParentBB() == ParentMBB;
  }

  assert((SU->isScheduled() || !SU->use_empty() || SU == getExitRoot())
          && "Unexpected deteched SU!");
  assert((SU->isScheduled() || SU->hasFixedTiming() || AnyDepFromTheSameParent)
         && "Find SU not constrained by the BB entry!");
}

VSUnit *VSchedGraph::createVSUnit(InstPtrTy Ptr, unsigned fuid) {
  VSUnit *SU = new VSUnit(NextSUIdx++, fuid);

  MachineInstr *MI = Ptr;
  bool mapped = mapMI2SU(Ptr, SU, MI ? DLInfo.getStepsToFinish(MI) : 0);
  (void) mapped;
  assert(mapped && "Cannot add SU to the inst2su map!");

  if (SU->isDatapath()) DPSUs.push_back(SU);
  else                  CPSUs.push_back(SU);
  return SU;
}

// Sort the schedule to place all control schedule unit at the beginning of the
// AllSU vector.
static inline bool sort_by_type(const VSUnit* LHS, const VSUnit* RHS) {
  if (LHS->isControl() != RHS->isControl())
    return LHS->isControl();

  return LHS->getIdx() < RHS->getIdx();
}

VSchedGraph::iterator
VSchedGraph::mergeSUsInSubGraph(VSchedGraph &SubGraph) {
  // 1. Merge the MI2SU map.
  // Prevent the virtual exit root from being inserted to the current MI2SU map.
  SubGraph.InstToSUnits.erase(SubGraph.getExitRoot()->getRepresentativePtr());

  InstToSUnits.insert(SubGraph.InstToSUnits.begin(),
                      SubGraph.InstToSUnits.end());

  // 2. Add the SUs in subgraph to the SU list of current SU list.
  unsigned OldCPStart = num_cps();
  unsigned NewIdxBase = NextSUIdx;
  assert(num_sus() == NextSUIdx - FirstSUIdx && "Index mis-matched!");
  VSUnit *Terminator = SubGraph.lookUpTerminator(SubGraph.getEntryBB());
  assert(Terminator && "Terminator SU not found!");
  // We need to clear the dependencies of the terminator before we adding
  // local schedule constraint.
  Terminator->cleanDepAndUse();
  unsigned TerminatorSlot = Terminator->getSlot();

  for (iterator I = SubGraph.cp_begin(), E = SubGraph.cp_end(); I != E; ++I) {
    VSUnit *U = *I;

    // Ignore the virtual exit root.
    if (U == SubGraph.getExitRoot()) continue;

    // Update the index of the scheduled SU.
    CPSUs.push_back(U->updateIdx(NewIdxBase + U->getIdx() - FirstSUIdx));
    ++NextSUIdx;

    // We may need to build a fixed timing dependencies according to the
    // schedule of U.
    unsigned USlot = U->getSlot();
    U->resetSchedule();

    // All scheduled control SU has fixed a timing constraint.
    U->setHasFixedTiming();

    // Do not add loop.
    if (U == Terminator) continue;

    assert(USlot && "Unexpected unscheduled SU!");
    unsigned ScheduleOffset = TerminatorSlot - USlot;
    // We need to build the new dependencies.
    U->cleanDepAndUse();
    // A dependencies to constrain the SU with local schedule.
    Terminator->addDep(U, VDEdge::CreateFixTimingConstraint(ScheduleOffset));
  }

  for (iterator I = SubGraph.dp_begin(), E = SubGraph.dp_end(); I != E; ++I) {
    VSUnit *U = *I;

    // Update the index of the scheduled SU.
    DPSUs.push_back(U->updateIdx(NewIdxBase + U->getIdx() - FirstSUIdx));
    ++NextSUIdx;
  }

  SubGraph.CPSUs.clear();
  // Leave the exit root of the SubGraph in its CPSU list, so it will be deleted.
  SubGraph.CPSUs.push_back(getExitRoot());
  SubGraph.DPSUs.clear();

  // 3. Merge the II Map.
  IIMap.insert(SubGraph.IIMap.begin(), SubGraph.IIMap.end());

  //4. Merge the terminator map.
  Terminators.insert(SubGraph.Terminators.begin(), SubGraph.Terminators.end());

  assert(NextSUIdx == Terminator->getIdx() + 1 && "Index mis-matched!");
  // Return the iterator point to the first SU of the subgraph, and
  // we need to skip the entry node of the block, we need add 1 after OldCPStart.
  return CPSUs.begin() + OldCPStart + 1;
}

void VSchedGraph::topologicalSortCPSUs() {
  unsigned Idx = 0;
  VSUnit *Exit = getExitRoot();
  typedef po_iterator<VSUnit*, SmallPtrSet<VSUnit*, 64>, false,
                      GraphTraits<Inverse<VSUnit*> > >
          top_it;

  for (top_it I = top_it::begin(Exit), E = top_it::end(Exit); I != E; ++I)
    CPSUs[Idx++] = *I;

  assert(Idx == num_cps() && "Bad topological sort!");
}

void VSchedGraph::prepareForDatapathSched() {
  for (iterator I = cp_begin(), E = cp_end(); I != E; ++I) {
    VSUnit *U = *I;
    assert(U->isControl() && "Unexpected datapath op in to schedule list!");
    U->cleanDepAndUse();
  }

  // Temporary Hack: Merge the data-path SU vector into the control-path SU
  // vector.
  CPSUs.insert(CPSUs.end(), DPSUs.begin(), DPSUs.end());
}

void VSchedGraph::resetCPSchedule(unsigned MII) {
  for (iterator I = cp_begin(), E = cp_end(); I != E; ++I) {
    VSUnit *U = *I;
    U->resetSchedule();
  }

  getEntryRoot()->scheduledTo(EntrySlot);
  // Also schedule the LoopOp to MII step.
  if (MII) {
    assert(hasLoopOp() && "MII provided but LoopOp not exist!");
    getLoopOp()->scheduledTo(EntrySlot + MII);
  }

  // Make sure the PHI copy emit before the BB jump to other BBs.
  typedef VSUnit::dep_iterator dep_it;
  VSUnit *ExitRoot = getExitRoot();
  for (dep_it I = ExitRoot->dep_begin(), E = ExitRoot->dep_end(); I != E; ++I)
    if (I->isPHI()) I.getEdge().setLatency(MII);
}

void VSchedGraph::resetDPSchedule() {
  for (iterator I = dp_begin(), E = dp_end(); I != E; ++I) {
    VSUnit *U = *I;
    U->resetSchedule();
  }
}

void VSchedGraph::scheduleLoop() {
  MachineBasicBlock *MBB = getEntryBB();
  MachineFunction *F = MBB->getParent();
  DEBUG(dbgs() << "Try to pipeline MBB#" << MBB->getNumber()
               << " MF#" << F->getFunctionNumber() << '\n');
  IterativeModuloScheduling Scheduler(*this);
  // Ensure us can schedule the critical path.
  while (!Scheduler.scheduleCriticalPath())
    Scheduler.lengthenCriticalPath();

  // computeMII may return a very big II if we cannot compute the RecII.
  Scheduler.computeMII();
  DEBUG(dbgs() << "Pipelining BB# " << MBB->getNumber()
               << " in function " << MBB->getParent()->getFunction()->getName()
               << " #" << MBB->getParent()->getFunctionNumber() << '\n');

  DEBUG(dbgs() << "MII: " << Scheduler.getMII() << "...");
  while (!Scheduler.scheduleCriticalPath()) {
    // Make sure MII smaller than the critical path length.
    if (2 * Scheduler.getMII() < Scheduler.getCriticalPathLength())
      Scheduler.increaseMII();
    else
      Scheduler.lengthenCriticalPath();
  }

  assert(Scheduler.getMII() <= Scheduler.getCriticalPathLength()
         && "MII bigger then Critical path length!");

  while (!Scheduler.scheduleState()) {
    // Make sure MII smaller than the critical path length.
    if (Scheduler.getMII() < Scheduler.getCriticalPathLength())
      Scheduler.increaseMII();
    else
      Scheduler.lengthenCriticalPath();
  }

  DEBUG(dbgs() << "SchedII: " << Scheduler.getMII()
               << " Latency: " << getTotalSlot(MBB) << '\n');
  assert(getLoopOp()->getSlot() - EntrySlot == Scheduler.getMII()
         && "LoopOp was not scheduled to the right slot!");
  assert(getLoopOp()->getSlot() <= getEndSlot(MBB)
         && "Expect MII is not bigger then critical path length!");

  bool succ = IIMap.insert(std::make_pair(MBB, Scheduler.getMII())).second;
  assert(succ && "Cannot remember II!");
  (void) succ;

  fixPHISchedules(cp_begin(), cp_end());
}

void VSchedGraph::viewGraph() {
  ViewGraph(this, this->getEntryBB()->getName());
}

void VSchedGraph::fixChainedDatapathRC(VSUnit *U) {
  assert(U->isDatapath() && "Expected datapath operation!");
  assert(U->num_instrs() == 1 && "Unexpected datapath operation merged!");

  MachineInstr *MI = U->getRepresentativePtr();
  MachineBasicBlock *ParentMBB = MI->getParent();
  const DetialLatencyInfo::DepLatInfoTy *DepLatInfo = DLInfo.getDepLatInfo(MI);
  assert(DepLatInfo && "dependence latency information not available?");

  typedef DetialLatencyInfo::DepLatInfoTy::const_iterator dep_it;
  // If multi-cycle chain is disable, a copy is always need.
  bool NeedCopyToReg = DisableMultiCyclesChain;

  for (dep_it I = DepLatInfo->begin(), E = DepLatInfo->end(); I != E; ++I) {
    const MachineInstr *SrcMI = I->first;

    // Ignore the entry root.
    if (SrcMI == 0 || SrcMI->getParent() != ParentMBB)
      continue;

    unsigned SrcOpC = SrcMI->getOpcode();
    // Ignore the operations without interesting function unit.
    if (VInstrInfo::hasTrivialFU(SrcOpC)) continue;

    VSUnit *SrcSU = lookupSUnit(SrcMI);
    assert(SrcSU && "Source schedule unit not exist?");
    unsigned SrcCopySlot =
      SrcSU->getFinSlot() + VInstrInfo::isWriteUntilFinish(SrcOpC);
    // Is the datapath operation chained with its depending control operation?
    if (SrcCopySlot > U->getSlot()) {
      NeedCopyToReg = true;
      // FIXME: Also compute the optimize copy slot.
      break;
    }
  }

  if (!NeedCopyToReg) {
    assert(MI->getDesc().getNumDefs() == 1
           && "Expect datapath operation have only 1 define!");

    unsigned Reg = MI->getOperand(0).getReg();
    DLInfo.MRI.setRegClass(Reg, &VTM::WireRegClass);
  }
}

void VSchedGraph::fixPHISchedules(iterator su_begin, iterator su_end) {
  // Fix the schedule of PHI's so we can emit the incoming copies at a right
  // slot;
  for (iterator I = su_begin, E = su_end; I != E; ++I) {
    VSUnit *U = *I;
    if (!U->isPHI()) continue;

    MachineBasicBlock *MBB = U->getParentBB();
    // Schedule the SU to the slot of the PHI Move.
    U->scheduledTo(U->getSlot() + getII(MBB));
    assert(U->getSlot() <= getEndSlot(MBB) && "Bad PHI schedule!");
  }
}

void VSchedGraph::clearDanglingFlagForTree(VSUnit *Root) {
  // Perform depth first search to find node that reachable from Root.
  std::vector<std::pair<VSUnit*, VSUnit::dep_iterator> > WorkStack;
  WorkStack.push_back(std::make_pair(Root, Root->dep_begin()));
  Root->setIsDangling(false);
  MachineBasicBlock *RootBB = Root->getParentBB();

  while (!WorkStack.empty()) {
    VSUnit *U = WorkStack.back().first;
    VSUnit::dep_iterator ChildIt = WorkStack.back().second;

    if (ChildIt == U->dep_end()) {
      WorkStack.pop_back();
      continue;
    }

    VSUnit *ChildNode = *ChildIt;
    ++WorkStack.back().second;

    if (!ChildNode->isDangling() || ChildNode->getParentBB() != RootBB) continue;

    // If the node is reachable from exit, then it is not dangling.
    ChildNode->setIsDangling(false);
    WorkStack.push_back(std::make_pair(ChildNode, ChildNode->dep_begin()));
  }
}

void VSchedGraph::addSoftConstraintsToBreakChains(SDCSchedulingBase &S) {
  for (iterator I = dp_begin(), E = dp_end(); I != E; ++I) {
    VSUnit *U = *I;

    // Add soft constraints to prevent CtrlOps from being chained by Data-path Ops.
    assert(U->num_instrs() == 1 && "Data-path SU with more than 1 MIs!");
    MachineInstr *MI = U->getRepresentativePtr();
    assert(MI && "Bad data-path SU without underlying MI!");
    // Ignore the trivial data-paths.
    if (getStepsToFinish(MI) == 0) continue;

    MachineBasicBlock *ParentMBB = U->getParentBB();
    const DepLatInfoTy *Deps = getDepLatInfo(MI);

    typedef DetialLatencyInfo::DepLatInfoTy::const_iterator dep_it;

    for (dep_it I = Deps->begin(), E = Deps->end(); I != E; ++I) {
      const MachineInstr *SrcMI = I->first;

      // Ignore the entry root.
      if (SrcMI == 0 || SrcMI->getParent() != ParentMBB)
        continue;

      unsigned SrcOpC = SrcMI->getOpcode();
      // Ignore the operations without interesting function unit.
      if (VInstrInfo::hasTrivialFU(SrcOpC)) continue;

      VSUnit *SrcSU = lookupSUnit(SrcMI);
      assert(SrcSU && "Source schedule unit not exist?");
      unsigned StepsBeforeCopy = SrcSU->getLatency()
                                 + VInstrInfo::isWriteUntilFinish(SrcOpC);

      float LengthOfChain =
        DetialLatencyInfo::getLatency(*I) + DLInfo.getMaxLatency(MI);

      // The chain do not extend the live-interval of the underlying FU of the
      // dependence.
      if (StepsBeforeCopy >=  ceil(LengthOfChain)) continue;

      // Try avoid chaining by schedule the data-path operation 1 steps after.
      S.addSoftConstraint(SrcSU, U, StepsBeforeCopy + 1, 100.0);
    }
  }
}

void VSchedGraph::scheduleControlPath() {
  SDCScheduler<true> Scheduler(*this);

  Scheduler.buildTimeFrameAndResetSchedule(true);
  BasicLinearOrderGenerator::addLinOrdEdge(Scheduler);
  // Build the step variables, and no need to schedule at all if all SUs have
  // been scheduled.
  if (!Scheduler.createLPAndVariables()) return;

  Scheduler.buildASAPObject(1.0);
  //Scheduler.buildOptSlackObject(0.0);

  bool success = Scheduler.schedule();
  assert(success && "SDCScheduler fail!");
  (void) success;

  Scheduler.fixInterBBLatency(*this);
}

void VSchedGraph::scheduleDatapath() {
  SDCScheduler<false> Scheduler(*this);

  if (Scheduler.createLPAndVariables()) {
    // Add soft constraints to break the chain.
    //addSoftConstraintsToBreakChains(Scheduler);
    //Scheduler.addSoftConstraintsPenalties(1.0);

    // Schedule them ALAP.
    Scheduler.buildASAPObject(-1.0);
    bool succ = Scheduler.schedule();
    assert(succ && "Cannot schedule the data-path!");
    (void) succ;
  }

  // Break the multi-cycles chains to expose more FU sharing opportunities.
  for (iterator I = cp_begin(), E = cp_end(); I != E; ++I)
    if ((*I)->isControl()) clearDanglingFlagForTree(*I);

  for (iterator I = dp_begin(), E = dp_end(); I != E; ++I) {
    VSUnit *U = *I;

    if (U->isDangling()) U->scheduledTo(getEndSlot(U->getParentBB()));

    fixChainedDatapathRC(U);
  }
}

//===----------------------------------------------------------------------===//

void VSUnit::dump() const {
  print(dbgs());
  dbgs() << '\n';
}

void VDEdge::print(raw_ostream &OS) const {}

void VSUnit::EdgeBundle::addEdge(VDEdge NewEdge) {
  VDEdge &CurEdge = Edges.front();

  if (CurEdge == NewEdge) return;

  assert(NewEdge.getEdgeType() != VDEdge::FixedTiming
        && CurEdge.getEdgeType() != VDEdge::FixedTiming
        && "Cannot override fixed timing dependencies!");
  // If the new dependency constraint tighter?
  assert((NewEdge.getDistance() == 0 || CurEdge.getDistance() == 0
          || CurEdge.getDistance() == NewEdge.getDistance())
         && "Unexpected multiple loop carried dependencies!");
  if (NewEdge.getDistance() <= CurEdge.getDistance()
      && NewEdge.getLatency() > CurEdge.getLatency()) {
    CurEdge = NewEdge;
  }
}

// TODO: Implement edge bundle, calculate the edge for
void VSUnit::addDep(VSUnit *Src, VDEdge NewE) {
  assert(Src != this && "Cannot add self-loop!");
  edge_iterator at = Deps.find(Src);

  if (at == Deps.end()) {
    bool IsCrossBB = Src->getParentBB() != getParentBB();
    Deps.insert(std::make_pair(Src, EdgeBundle(NewE, IsCrossBB)));
    Src->addToUseList(this);
    return;
  }

  at->second.addEdge(NewE);
}

VSUnit::VSUnit(unsigned short Idx, uint16_t FUNum)
  : SchedSlot(0), IsDangling(true), HasFixedTiming(false), InstIdx(Idx),
    FUNum(FUNum) {
  assert(Idx > VSchedGraph::NullSUIdx && "Bad index!");
}

VSUnit::VSUnit(MachineBasicBlock *MBB, uint16_t Idx)
  : SchedSlot(0), IsDangling(true), HasFixedTiming(false), InstIdx(Idx),
    FUNum(0) {
  assert(Idx > VSchedGraph::NullSUIdx && "Bad index!");
  Instrs.push_back(MBB);
  latencies.push_back(0);
}

VSUnit *VSUnit::updateIdx(unsigned short Idx) {
  assert(Idx > VSchedGraph::NullSUIdx && "Bad index!");

  InstIdx = Idx;
  return this;
}

unsigned VSUnit::countValDeps() const {
  unsigned DepCounter = 0;

  for(const_dep_iterator I = dep_begin(), E = dep_end(); I != E; ++I) {
    if(I.getEdgeType() != VDEdge::ValDep) continue;

    ++DepCounter;
  }

  return DepCounter;
}

unsigned VSUnit::countValUses() const {
  unsigned DepCounter = 0;

  for(const_use_iterator I = use_begin(), E = use_end(); I != E; ++I){
    const VSUnit* V =*I;
    if(V->getEdgeFrom(this).getEdgeType() != VDEdge::ValDep) continue;

    ++DepCounter;
  }

  return DepCounter;
}

unsigned VSUnit::getOpcode() const {
  if (MachineInstr *I = getRepresentativePtr())
    return I->getOpcode();

  return VTM::INSTRUCTION_LIST_END;
}

void VSUnit::scheduledTo(unsigned slot) {
  assert(slot && "Can not schedule to slot 0!");
  SchedSlot = slot;
  // TODO: Assert the schedule is not locked?
}

VFUs::FUTypes VSUnit::getFUType() const {
  if (MachineInstr *Instr = getRepresentativePtr())
    return VInstrInfo::getFUType(Instr->getOpcode());

  return VFUs::Trivial;
}

bool VSUnit::isDatapath() const {
  if (MachineInstr *Instr = getRepresentativePtr())
    return VInstrInfo::isDatapath(Instr->getOpcode());

  return false;
}

int8_t VSUnit::getLatencyFor(MachineInstr *MI) const {
  const_instr_iterator at = std::find(instr_begin(), instr_end(), MI);
  assert(at != instr_end() && "Instruction not exist!");
  return getLatencyAt(at - instr_begin());
}

int VSUnit::getLatencyFrom(MachineInstr *SrcMI, int SrcLatency) const{
  int Latency = SrcLatency;
  if (SrcMI != getRepresentativePtr()) {
    Latency += getLatencyFor(SrcMI);
  }

  return Latency;
}

void VSUnit::print(raw_ostream &OS) const {
  OS << "[" << getIdx() << "] MBB#" << getParentBB()->getNumber() << ' ';

  for (unsigned i = 0, e = num_instrs(); i < e; ++i) {
    InstPtrTy Ptr = getPtrAt(i);
    if (MachineInstr *Instr = Ptr.dyn_cast_mi()) {
      const TargetInstrInfo *TII = Instr->getParent()->getParent()->getTarget()
                                         .getInstrInfo();
      OS << TII->getName(Instr->getDesc().getOpcode()) << ' ';
      if (!Instr->isPseudo())
        OS << *VInstrInfo::getTraceOperand(Instr);
      if (i) OS << ' ' << int(getLatencyAt(i));
      OS << '\n';
      DEBUG(OS << *Instr << '\n');
    }
  }

  OS << getFUId() << "\nAt slot: " << getSlot();
  if (isDangling()) OS << " <Dangling>";
}
