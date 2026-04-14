// Control flow helpers for parser expression management.
// Copyright © 2025-2026 Paul Manias

#include "parser/parse_control_flow.h"
#include <kotuku/main.h>
#include "parser/parse_internal.h"

//********************************************************************************************************************

ControlFlowEdge ControlFlowGraph::make_edge(ControlFlowEdgeKind Kind, BCPos Head)
{
   EdgeEntry entry;
   entry.head = Head;
   entry.kind = Kind;
   this->edges.push_back(entry);
   size_t index = this->edges.size() - 1;
   this->trace_edge_creation(Kind, Head, index);
   return ControlFlowEdge(this, index);
}

//********************************************************************************************************************

void ControlFlowGraph::append_edge(size_t Index, BCPos Head)
{
   if (Index >= this->edges.size()) return;
   if (Head.raw() IS NO_JMP) return;

   EdgeEntry& entry = this->edges[Index];
   if (entry.head.raw() IS NO_JMP) {
      entry.head = Head;
      this->trace_edge_append(Index, Head);
      return;
   }

   entry.head = BCPos(JumpListView(this->func_state, entry.head.raw()).append(Head.raw()));
   this->trace_edge_append(Index, Head);
}

//********************************************************************************************************************

void ControlFlowGraph::patch_edge(size_t Index, BCPos Target)
{
   if (Index >= this->edges.size()) return;
   EdgeEntry &entry = this->edges[Index];
   if (entry.resolved) return;
   if (entry.head.raw() IS NO_JMP) {
      this->mark_resolved(Index);
      return;
   }
   this->trace_edge_patch(Index, Target);
   JumpListView(this->func_state, entry.head.raw()).patch_to(Target.raw());
   this->mark_resolved(Index);
}

//********************************************************************************************************************

void ControlFlowGraph::patch_edge_head(size_t Index, BCPos Destination)
{
   if (Index >= this->edges.size()) return;
   EdgeEntry &entry = this->edges[Index];
   if (entry.head.raw() IS NO_JMP) return;
   JumpListView(this->func_state, entry.head.raw()).patch_head(Destination.raw());
   this->mark_resolved(Index);
}

//********************************************************************************************************************

void ControlFlowGraph::patch_edge_with_value(size_t Index, BCPos ValueTarget, BCReg Register, BCPos DefaultTarget)
{
   if (Index >= this->edges.size()) return;
   EdgeEntry &entry = this->edges[Index];
   if (entry.head.raw() IS NO_JMP) {
      this->mark_resolved(Index);
      return;
   }

   BCPos list = entry.head;
   while (not(list.raw() IS NO_JMP)) {
      BCPos next_pc = next_in_chain(this->func_state, list);
      if (this->patch_test_register(list, Register)) this->patch_instruction(list, ValueTarget);
      else this->patch_instruction(list, DefaultTarget);

      list = next_pc;
   }
   this->mark_resolved(Index);
}

//********************************************************************************************************************

bool ControlFlowGraph::edge_produces_values(size_t Index) const
{
   if (Index >= this->edges.size()) return false;
   const EdgeEntry &entry = this->edges[Index];
   if (entry.head.raw() IS NO_JMP) return false;

   for (BCPos list = entry.head; not(list.raw() IS NO_JMP); list = next_in_chain(this->func_state, list)) {
      BCIns prior = this->func_state->bcbase[list.raw() >= 1 ? list.raw() - 1 : list.raw()].ins;
      if (not(bc_op(prior) IS BC_ISTC or bc_op(prior) IS BC_ISFC or bc_a(prior) IS NO_REG)) {
         return true;
      }
   }
   return false;
}

//********************************************************************************************************************

void ControlFlowGraph::drop_edge_values(size_t Index)
{
   if (Index >= this->edges.size()) return;
   EdgeEntry& entry = this->edges[Index];
   if (entry.head.raw() IS NO_JMP) return;

   for (BCPos list = entry.head; not(list.raw() IS NO_JMP); list = next_in_chain(this->func_state, list)) {
      (void)this->patch_test_register(list, BCReg(NO_REG));
   }
}

//********************************************************************************************************************

BCPos ControlFlowGraph::next_in_chain(FuncState* State, BCPos Position)
{
   ptrdiff_t delta = bc_j(State->bcbase[Position.raw()].ins);
   if (BCPOS(delta) IS NO_JMP) return BCPos(NO_JMP);
   return BCPos(BCPOS((ptrdiff_t(Position.raw()) + 1) + delta));
}

//********************************************************************************************************************

bool ControlFlowGraph::patch_test_register(BCPos Position, BCReg Register) const
{
   FuncState *fs = this->func_state;
   BCInsLine *line = &fs->bcbase[Position.raw() >= 1 ? Position.raw() - 1 : Position.raw()];
   BCOp op = bc_op(line->ins);

   if (op IS BC_ISTC or op IS BC_ISFC) {
      if (Register.raw() != NO_REG and Register.raw() != bc_d(line->ins)) {
         setbc_a(&line->ins, Register.raw());
      }
      else {
         setbc_op(&line->ins, op + (BC_IST - BC_ISTC));
         setbc_a(&line->ins, 0);
      }
   }
   else if (bc_a(line->ins) IS NO_REG) {
      if (Register.raw() IS NO_REG) line->ins = BCINS_AJ(BC_JMP, bc_a(fs->bcbase[Position.raw()].ins), 0);
      else {
         setbc_a(&line->ins, Register.raw());
         if (Register.raw() >= bc_a(line[1].ins)) setbc_a(&line[1].ins, Register.raw() + 1);
      }
   }
   else return false;
   return true;
}

void ControlFlowGraph::patch_instruction(BCPos Position, BCPos Destination) const
{
   FuncState* fs = this->func_state;
   BCIns* instruction = &fs->bcbase[Position.raw()].ins;
   BCPOS offset = Destination.raw() - (Position.raw() + 1) + BCBIAS_J;
   fs_check_assert(fs,not(Destination.raw() IS NO_JMP), "uninitialized jump target");
   if (offset > BCMAX_D) fs->ls->err_syntax(ErrMsg::XJUMP);
   setbc_d(instruction, offset);
}

//********************************************************************************************************************

void ControlFlowGraph::finalize() const
{
   if (GetResource(RES::LOG_LEVEL) >= 4) {
      for (size_t i = 0; i < this->edges.size(); ++i) {
         if (not this->edges[i].resolved and not(this->edges[i].head.raw() IS NO_JMP)) {
            pf::Log("Parser").error("Unresolved control-flow edge kind=%d head=%d", int(this->edges[i].kind), int(this->edges[i].head.raw()));
         }
      }
   }
}

//********************************************************************************************************************
// Debug tracing methods

void ControlFlowGraph::trace_edge_creation(ControlFlowEdgeKind Kind, BCPos Head, size_t Index) const
{
   auto prv = (prvTiri *)this->func_state->L->script->ChildPrivate;
   if ((prv->JitOptions & JOF::TRACE_CFG) != JOF::NIL) {
      CSTRING kind_name = "unknown";
      switch (Kind) {
         case ControlFlowEdgeKind::Unconditional: kind_name = "unconditional"; break;
         case ControlFlowEdgeKind::True: kind_name = "true"; break;
         case ControlFlowEdgeKind::False: kind_name = "false"; break;
         case ControlFlowEdgeKind::Break: kind_name = "break"; break;
         case ControlFlowEdgeKind::Continue: kind_name = "continue"; break;
      }
      pf::Log("Parser").msg("[%d] cfg: create edge #%" PRId64 " kind=%s head=%d", this->func_state->ls->linenumber.lineNumber(), Index, kind_name, int(Head.raw()));
   }
}

//********************************************************************************************************************

void ControlFlowGraph::trace_edge_patch(size_t Index, BCPos Target) const
{
   auto prv = (prvTiri *)this->func_state->L->script->ChildPrivate;
   if ((prv->JitOptions & JOF::TRACE_CFG) != JOF::NIL) {
      pf::Log("Parser").msg("[%d] cfg: patch edge #%" PRId64 " to target=%d", this->func_state->ls->linenumber.lineNumber(), Index, int(Target.raw()));
   }
}

//********************************************************************************************************************

void ControlFlowGraph::trace_edge_append(size_t Index, BCPos Head) const
{
   auto prv = (prvTiri *)this->func_state->L->script->ChildPrivate;
   if ((prv->JitOptions & JOF::TRACE_CFG) != JOF::NIL) {
      pf::Log("Parser").msg("[%d] cfg: append to edge #%" PRId64 " head=%d", this->func_state->ls->linenumber.lineNumber(), Index, int(Head.raw()));
   }
}
