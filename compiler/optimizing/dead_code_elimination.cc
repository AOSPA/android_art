/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "dead_code_elimination.h"

#include "base/array_ref.h"
#include "base/bit_vector-inl.h"
#include "base/scoped_arena_allocator.h"
#include "base/scoped_arena_containers.h"
#include "base/stl_util.h"
#include "ssa_phi_elimination.h"

namespace art {

static void MarkReachableBlocks(HGraph* graph, ArenaBitVector* visited) {
  // Use local allocator for allocating memory.
  ScopedArenaAllocator allocator(graph->GetArenaStack());

  ScopedArenaVector<HBasicBlock*> worklist(allocator.Adapter(kArenaAllocDCE));
  constexpr size_t kDefaultWorlistSize = 8;
  worklist.reserve(kDefaultWorlistSize);
  visited->SetBit(graph->GetEntryBlock()->GetBlockId());
  worklist.push_back(graph->GetEntryBlock());

  while (!worklist.empty()) {
    HBasicBlock* block = worklist.back();
    worklist.pop_back();
    int block_id = block->GetBlockId();
    DCHECK(visited->IsBitSet(block_id));

    ArrayRef<HBasicBlock* const> live_successors(block->GetSuccessors());
    HInstruction* last_instruction = block->GetLastInstruction();
    if (last_instruction->IsIf()) {
      HIf* if_instruction = last_instruction->AsIf();
      HInstruction* condition = if_instruction->InputAt(0);
      if (condition->IsIntConstant()) {
        if (condition->AsIntConstant()->IsTrue()) {
          live_successors = live_successors.SubArray(0u, 1u);
          DCHECK_EQ(live_successors[0], if_instruction->IfTrueSuccessor());
        } else {
          DCHECK(condition->AsIntConstant()->IsFalse()) << condition->AsIntConstant()->GetValue();
          live_successors = live_successors.SubArray(1u, 1u);
          DCHECK_EQ(live_successors[0], if_instruction->IfFalseSuccessor());
        }
      }
    } else if (last_instruction->IsPackedSwitch()) {
      HPackedSwitch* switch_instruction = last_instruction->AsPackedSwitch();
      HInstruction* switch_input = switch_instruction->InputAt(0);
      if (switch_input->IsIntConstant()) {
        int32_t switch_value = switch_input->AsIntConstant()->GetValue();
        int32_t start_value = switch_instruction->GetStartValue();
        // Note: Though the spec forbids packed-switch values to wrap around, we leave
        // that task to the verifier and use unsigned arithmetic with it's "modulo 2^32"
        // semantics to check if the value is in range, wrapped or not.
        uint32_t switch_index =
            static_cast<uint32_t>(switch_value) - static_cast<uint32_t>(start_value);
        if (switch_index < switch_instruction->GetNumEntries()) {
          live_successors = live_successors.SubArray(switch_index, 1u);
          DCHECK_EQ(live_successors[0], block->GetSuccessors()[switch_index]);
        } else {
          live_successors = live_successors.SubArray(switch_instruction->GetNumEntries(), 1u);
          DCHECK_EQ(live_successors[0], switch_instruction->GetDefaultBlock());
        }
      }
    }

    for (HBasicBlock* successor : live_successors) {
      // Add only those successors that have not been visited yet.
      if (!visited->IsBitSet(successor->GetBlockId())) {
        visited->SetBit(successor->GetBlockId());
        worklist.push_back(successor);
      }
    }
  }
}

void HDeadCodeElimination::MaybeRecordDeadBlock(HBasicBlock* block) {
  if (stats_ != nullptr) {
    stats_->RecordStat(MethodCompilationStat::kRemovedDeadInstruction,
                       block->GetPhis().CountSize() + block->GetInstructions().CountSize());
  }
}

void HDeadCodeElimination::MaybeRecordSimplifyIf() {
  if (stats_ != nullptr) {
    stats_->RecordStat(MethodCompilationStat::kSimplifyIf);
  }
}

static bool HasInput(HCondition* instruction, HInstruction* input) {
  return (instruction->InputAt(0) == input) ||
         (instruction->InputAt(1) == input);
}

static bool HasEquality(IfCondition condition) {
  switch (condition) {
    case kCondEQ:
    case kCondLE:
    case kCondGE:
    case kCondBE:
    case kCondAE:
      return true;
    case kCondNE:
    case kCondLT:
    case kCondGT:
    case kCondB:
    case kCondA:
      return false;
  }
}

static HConstant* Evaluate(HCondition* condition, HInstruction* left, HInstruction* right) {
  if (left == right && !DataType::IsFloatingPointType(left->GetType())) {
    return condition->GetBlock()->GetGraph()->GetIntConstant(
        HasEquality(condition->GetCondition()) ? 1 : 0);
  }

  if (!left->IsConstant() || !right->IsConstant()) {
    return nullptr;
  }

  if (left->IsIntConstant()) {
    return condition->Evaluate(left->AsIntConstant(), right->AsIntConstant());
  } else if (left->IsNullConstant()) {
    return condition->Evaluate(left->AsNullConstant(), right->AsNullConstant());
  } else if (left->IsLongConstant()) {
    return condition->Evaluate(left->AsLongConstant(), right->AsLongConstant());
  } else if (left->IsFloatConstant()) {
    return condition->Evaluate(left->AsFloatConstant(), right->AsFloatConstant());
  } else {
    DCHECK(left->IsDoubleConstant());
    return condition->Evaluate(left->AsDoubleConstant(), right->AsDoubleConstant());
  }
}

static bool RemoveNonNullControlDependences(HBasicBlock* block, HBasicBlock* throws) {
  // Test for an if as last statement.
  if (!block->EndsWithIf()) {
    return false;
  }
  HIf* ifs = block->GetLastInstruction()->AsIf();
  // Find either:
  //   if obj == null
  //     throws
  //   else
  //     not_throws
  // or:
  //   if obj != null
  //     not_throws
  //   else
  //     throws
  HInstruction* cond = ifs->InputAt(0);
  HBasicBlock* not_throws = nullptr;
  if (throws == ifs->IfTrueSuccessor() && cond->IsEqual()) {
    not_throws = ifs->IfFalseSuccessor();
  } else if (throws == ifs->IfFalseSuccessor() && cond->IsNotEqual()) {
    not_throws = ifs->IfTrueSuccessor();
  } else {
    return false;
  }
  DCHECK(cond->IsEqual() || cond->IsNotEqual());
  HInstruction* obj = cond->InputAt(1);
  if (obj->IsNullConstant()) {
    obj = cond->InputAt(0);
  } else if (!cond->InputAt(0)->IsNullConstant()) {
    return false;
  }
  // Scan all uses of obj and find null check under control dependence.
  HBoundType* bound = nullptr;
  const HUseList<HInstruction*>& uses = obj->GetUses();
  for (auto it = uses.begin(), end = uses.end(); it != end;) {
    HInstruction* user = it->GetUser();
    ++it;  // increment before possibly replacing
    if (user->IsNullCheck()) {
      HBasicBlock* user_block = user->GetBlock();
      if (user_block != block &&
          user_block != throws &&
          block->Dominates(user_block)) {
        if (bound == nullptr) {
          ReferenceTypeInfo ti = obj->GetReferenceTypeInfo();
          bound = new (obj->GetBlock()->GetGraph()->GetAllocator()) HBoundType(obj);
          bound->SetUpperBound(ti, /*can_be_null*/ false);
          bound->SetReferenceTypeInfo(ti);
          bound->SetCanBeNull(false);
          not_throws->InsertInstructionBefore(bound, not_throws->GetFirstInstruction());
        }
        user->ReplaceWith(bound);
        user_block->RemoveInstruction(user);
      }
    }
  }
  return bound != nullptr;
}

// Simplify the pattern:
//
//           B1
//          /  \
//          |   instr_1
//          |   ...
//          |   instr_n
//          |   foo()  // always throws
//          \   goto B2
//           \ /
//            B2
//
// Into:
//
//           B1
//          /  \
//          |  instr_1
//          |  ...
//          |  instr_n
//          |  foo()
//          |  goto Exit
//          |   |
//         B2  Exit
//
// Rationale:
// Removal of the never taken edge to B2 may expose
// other optimization opportunities, such as code sinking.
bool HDeadCodeElimination::SimplifyAlwaysThrows() {
  HBasicBlock* exit = graph_->GetExitBlock();
  if (exit == nullptr) {
    return false;
  }

  bool rerun_dominance_and_loop_analysis = false;

  // Order does not matter, just pick one.
  for (HBasicBlock* block : graph_->GetReversePostOrder()) {
    if (block->GetTryCatchInformation() != nullptr) {
      // We don't want to perform the simplify always throws optimizations for throws inside of
      // tries since those throws might not go to the exit block. We do that by checking the
      // TryCatchInformation of the blocks.
      //
      // As a special case the `catch_block` is the first block of the catch and it has
      // TryCatchInformation. Other blocks in the catch don't have try catch information (as long as
      // they are not part of an outer try). Knowing if a `catch_block` is part of an outer try is
      // possible by checking its successors, but other restrictions of the simplify always throws
      // optimization will block `catch_block` nevertheless (e.g. only one predecessor) so it is not
      // worth the effort.

      // TODO(solanes): Maybe we can do a `goto catch` if inside of a try catch instead of going to
      // the exit. If we do so, we have to take into account that we should go to the nearest valid
      // catch i.e. one that would accept our exception type.
      continue;
    }

    HInstruction* last = block->GetLastInstruction();
    HInstruction* prev = last->GetPrevious();
    if (prev == nullptr) {
      DCHECK_EQ(block->GetFirstInstruction(), block->GetLastInstruction());
      continue;
    }

    if (prev->AlwaysThrows() &&
        last->IsGoto() &&
        block->GetPhis().IsEmpty() &&
        block->GetPredecessors().size() == 1u) {
      HBasicBlock* pred = block->GetSinglePredecessor();
      HBasicBlock* succ = block->GetSingleSuccessor();
      // Ensure no computations are merged through throwing block.
      // This does not prevent the optimization per se, but would
      // require an elaborate clean up of the SSA graph.
      if (succ != exit &&
          !block->Dominates(pred) &&
          pred->Dominates(succ) &&
          succ->GetPredecessors().size() > 1u &&
          succ->GetPhis().IsEmpty()) {
        block->ReplaceSuccessor(succ, exit);
        rerun_dominance_and_loop_analysis = true;
        MaybeRecordStat(stats_, MethodCompilationStat::kSimplifyThrowingInvoke);
        // Perform a quick follow up optimization on object != null control dependences
        // that is much cheaper to perform now than in a later phase.
        if (RemoveNonNullControlDependences(pred, block)) {
          MaybeRecordStat(stats_, MethodCompilationStat::kRemovedNullCheck);
        }
      }
    }
  }

  // We need to re-analyze the graph in order to run DCE afterwards.
  if (rerun_dominance_and_loop_analysis) {
    graph_->ClearLoopInformation();
    graph_->ClearDominanceInformation();
    graph_->BuildDominatorTree();
    return true;
  }
  return false;
}

bool HDeadCodeElimination::SimplifyIfs() {
  bool simplified_one_or_more_ifs = false;
  bool rerun_dominance_and_loop_analysis = false;

  // Iterating in PostOrder it's better for MaybeAddPhi as it can add a Phi for multiple If
  // instructions in a chain without updating the dominator chain. The branch redirection itself can
  // work in PostOrder or ReversePostOrder without issues.
  for (HBasicBlock* block : graph_->GetPostOrder()) {
    if (block->IsCatchBlock()) {
      // This simplification cannot be applied to catch blocks, because exception handler edges do
      // not represent normal control flow. Though in theory this could still apply to normal
      // control flow going directly to a catch block, we cannot support it at the moment because
      // the catch Phi's inputs do not correspond to the catch block's predecessors, so we cannot
      // identify which predecessor corresponds to a given statically evaluated input.
      continue;
    }

    HInstruction* last = block->GetLastInstruction();
    if (!last->IsIf()) {
      continue;
    }

    if (block->IsLoopHeader()) {
      // We do not apply this optimization to loop headers as this could create irreducible loops.
      continue;
    }

    // We will add a Phi which allows the simplification to take place in cases where it wouldn't.
    MaybeAddPhi(block);

    // TODO(solanes): Investigate support for multiple phis in `block`. We can potentially "push
    // downwards" existing Phis into the true/false branches. For example, let's say we have another
    // Phi: Phi(x1,x2,x3,x4,x5,x6). This could turn into Phi(x1,x2) in the true branch, Phi(x3,x4)
    // in the false branch, and remain as Phi(x5,x6) in `block` (for edges that we couldn't
    // redirect). We might even be able to remove some phis altogether as they will have only one
    // value.
    if (block->HasSinglePhi() &&
        block->GetFirstPhi()->HasOnlyOneNonEnvironmentUse()) {
      HInstruction* first = block->GetFirstInstruction();
      bool has_only_phi_and_if = (last == first) && (last->InputAt(0) == block->GetFirstPhi());
      bool has_only_phi_condition_and_if =
          !has_only_phi_and_if &&
          first->IsCondition() &&
          HasInput(first->AsCondition(), block->GetFirstPhi()) &&
          (first->GetNext() == last) &&
          (last->InputAt(0) == first) &&
          first->HasOnlyOneNonEnvironmentUse();

      if (has_only_phi_and_if || has_only_phi_condition_and_if) {
        HPhi* phi = block->GetFirstPhi()->AsPhi();
        bool phi_input_is_left = (first->InputAt(0) == phi);

        // Walk over all inputs of the phis and update the control flow of
        // predecessors feeding constants to the phi.
        // Note that phi->InputCount() may change inside the loop.
        for (size_t i = 0; i < phi->InputCount();) {
          HInstruction* input = phi->InputAt(i);
          HInstruction* value_to_check = nullptr;
          if (has_only_phi_and_if) {
            if (input->IsIntConstant()) {
              value_to_check = input;
            }
          } else {
            DCHECK(has_only_phi_condition_and_if);
            if (phi_input_is_left) {
              value_to_check = Evaluate(first->AsCondition(), input, first->InputAt(1));
            } else {
              value_to_check = Evaluate(first->AsCondition(), first->InputAt(0), input);
            }
          }
          if (value_to_check == nullptr) {
            // Could not evaluate to a constant, continue iterating over the inputs.
            ++i;
          } else {
            HBasicBlock* predecessor_to_update = block->GetPredecessors()[i];
            HBasicBlock* successor_to_update = nullptr;
            if (value_to_check->AsIntConstant()->IsTrue()) {
              successor_to_update = last->AsIf()->IfTrueSuccessor();
            } else {
              DCHECK(value_to_check->AsIntConstant()->IsFalse())
                  << value_to_check->AsIntConstant()->GetValue();
              successor_to_update = last->AsIf()->IfFalseSuccessor();
            }
            predecessor_to_update->ReplaceSuccessor(block, successor_to_update);
            phi->RemoveInputAt(i);
            simplified_one_or_more_ifs = true;
            if (block->IsInLoop()) {
              rerun_dominance_and_loop_analysis = true;
            }
            // For simplicity, don't create a dead block, let the dead code elimination
            // pass deal with it.
            if (phi->InputCount() == 1) {
              break;
            }
          }
        }
        if (block->GetPredecessors().size() == 1) {
          phi->ReplaceWith(phi->InputAt(0));
          block->RemovePhi(phi);
          if (has_only_phi_condition_and_if) {
            // Evaluate here (and not wait for a constant folding pass) to open
            // more opportunities for DCE.
            HInstruction* result = first->AsCondition()->TryStaticEvaluation();
            if (result != nullptr) {
              first->ReplaceWith(result);
              block->RemoveInstruction(first);
            }
          }
        }
        if (simplified_one_or_more_ifs) {
          MaybeRecordSimplifyIf();
        }
      }
    }
  }
  // We need to re-analyze the graph in order to run DCE afterwards.
  if (simplified_one_or_more_ifs) {
    if (rerun_dominance_and_loop_analysis) {
      graph_->ClearLoopInformation();
      graph_->ClearDominanceInformation();
      graph_->BuildDominatorTree();
    } else {
      graph_->ClearDominanceInformation();
      // We have introduced critical edges, remove them.
      graph_->SimplifyCFG();
      graph_->ComputeDominanceInformation();
      graph_->ComputeTryBlockInformation();
    }
  }

  return simplified_one_or_more_ifs;
}

void HDeadCodeElimination::MaybeAddPhi(HBasicBlock* block) {
  DCHECK(block->GetLastInstruction()->IsIf());
  HIf* if_instruction = block->GetLastInstruction()->AsIf();
  if (if_instruction->InputAt(0)->IsConstant()) {
    // Constant values are handled in RemoveDeadBlocks.
    return;
  }

  if (block->GetNumberOfPredecessors() < 2u) {
    // Nothing to redirect.
    return;
  }

  if (!block->GetPhis().IsEmpty()) {
    // SimplifyIf doesn't currently work with multiple phis. Adding a phi here won't help that
    // optimization.
    return;
  }

  HBasicBlock* dominator = block->GetDominator();
  if (!dominator->EndsWithIf()) {
    return;
  }

  HInstruction* input = if_instruction->InputAt(0);
  HInstruction* dominator_input = dominator->GetLastInstruction()->AsIf()->InputAt(0);
  const bool same_input = dominator_input == input;
  if (!same_input) {
    // Try to see if the dominator has the opposite input (e.g. if(cond) and if(!cond)). If that's
    // the case, we can perform the optimization with the false and true branches reversed.
    if (!dominator_input->IsCondition() || !input->IsCondition()) {
      return;
    }

    HCondition* block_cond = input->AsCondition();
    HCondition* dominator_cond = dominator_input->AsCondition();

    if (block_cond->GetLeft() != dominator_cond->GetLeft() ||
        block_cond->GetRight() != dominator_cond->GetRight() ||
        block_cond->GetOppositeCondition() != dominator_cond->GetCondition()) {
      return;
    }
  }

  if (kIsDebugBuild) {
    // `block`'s successors should have only one predecessor. Otherwise, we have a critical edge in
    // the graph.
    for (HBasicBlock* succ : block->GetSuccessors()) {
      DCHECK_EQ(succ->GetNumberOfPredecessors(), 1u);
    }
  }

  const size_t pred_size = block->GetNumberOfPredecessors();
  HPhi* new_phi = new (graph_->GetAllocator())
      HPhi(graph_->GetAllocator(), kNoRegNumber, pred_size, DataType::Type::kInt32);

  for (size_t index = 0; index < pred_size; index++) {
    HBasicBlock* pred = block->GetPredecessors()[index];
    const bool dominated_by_true =
        dominator->GetLastInstruction()->AsIf()->IfTrueSuccessor()->Dominates(pred);
    const bool dominated_by_false =
        dominator->GetLastInstruction()->AsIf()->IfFalseSuccessor()->Dominates(pred);
    if (dominated_by_true == dominated_by_false) {
      // In this case, we can't know if we are coming from the true branch, or the false branch. It
      // happens in cases like:
      //      1 (outer if)
      //     / \
      //    2   3 (inner if)
      //    |  / \
      //    | 4  5
      //     \/  |
      //      6  |
      //       \ |
      //         7 (has the same if(cond) as 1)
      //         |
      //         8
      // `7` (which would be `block` in this example), and `6` will come from both the true path and
      // the false path of `1`. We bumped into something similar in SelectGenerator. See
      // HSelectGenerator::TryFixupDoubleDiamondPattern.
      // TODO(solanes): Figure out if we can fix up the graph into a double diamond in a generic way
      // so that DeadCodeElimination and SelectGenerator can take advantage of it.

      if (!same_input) {
        // `1` and `7` having the opposite condition is a case we are missing. We could potentially
        // add a BooleanNot instruction to be able to add the Phi, but it seems like overkill since
        // this case is not that common.
        return;
      }

      // The Phi will have `0`, `1`, and `cond` as inputs. If SimplifyIf redirects 0s and 1s, we
      // will end up with Phi(cond,...,cond) which will be replaced by `cond`. Effectively, we will
      // redirect edges that we are able to redirect and the rest will remain as before (i.e. we
      // won't have an extra Phi).
      new_phi->SetRawInputAt(index, input);
    } else {
      // Redirect to either the true branch (1), or the false branch (0).
      // Given that `dominated_by_true` is the exact opposite of `dominated_by_false`,
      // `(same_input && dominated_by_true) || (!same_input && dominated_by_false)` is equivalent to
      // `same_input == dominated_by_true`.
      new_phi->SetRawInputAt(
          index,
          same_input == dominated_by_true ? graph_->GetIntConstant(1) : graph_->GetIntConstant(0));
    }
  }

  block->AddPhi(new_phi);
  if_instruction->ReplaceInput(new_phi, 0);

  // Remove the old input now, if possible. This allows the branch redirection in SimplifyIf to
  // work without waiting for another pass of DCE.
  if (input->IsDeadAndRemovable()) {
    DCHECK(!same_input)
        << " if both blocks have the same condition, it shouldn't be dead and removable since the "
        << "dominator block's If instruction would be using that condition.";
    input->GetBlock()->RemoveInstruction(input);
  }
  MaybeRecordStat(stats_, MethodCompilationStat::kSimplifyIfAddedPhi);
}

void HDeadCodeElimination::ConnectSuccessiveBlocks() {
  // Order does not matter. Skip the entry block by starting at index 1 in reverse post order.
  for (size_t i = 1u, size = graph_->GetReversePostOrder().size(); i != size; ++i) {
    HBasicBlock* block  = graph_->GetReversePostOrder()[i];
    DCHECK(!block->IsEntryBlock());
    while (block->GetLastInstruction()->IsGoto()) {
      HBasicBlock* successor = block->GetSingleSuccessor();
      if (successor->IsExitBlock() || successor->GetPredecessors().size() != 1u) {
        break;
      }
      DCHECK_LT(i, IndexOfElement(graph_->GetReversePostOrder(), successor));
      block->MergeWith(successor);
      --size;
      DCHECK_EQ(size, graph_->GetReversePostOrder().size());
      DCHECK_EQ(block, graph_->GetReversePostOrder()[i]);
      // Reiterate on this block in case it can be merged with its new successor.
    }
  }
}

struct HDeadCodeElimination::TryBelongingInformation {
  TryBelongingInformation(ScopedArenaAllocator* allocator)
      : blocks_in_try(allocator->Adapter(kArenaAllocDCE)),
        coalesced_try_entries(allocator->Adapter(kArenaAllocDCE)) {}

  // Which blocks belong in the try.
  ScopedArenaSet<HBasicBlock*> blocks_in_try;
  // Which other try entries are referencing this same try.
  ScopedArenaSet<HBasicBlock*> coalesced_try_entries;
};

bool HDeadCodeElimination::CanPerformTryRemoval(const TryBelongingInformation& try_belonging_info) {
  for (HBasicBlock* block : try_belonging_info.blocks_in_try) {
    for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
      if (it.Current()->CanThrow()) {
        return false;
      }
    }
  }
  return true;
}

void HDeadCodeElimination::DisconnectHandlersAndUpdateTryBoundary(
    HBasicBlock* block,
    /* out */ bool* any_handler_in_loop) {
  // Disconnect the handlers.
  while (block->GetSuccessors().size() > 1) {
    HBasicBlock* handler = block->GetSuccessors()[1];
    DCHECK(handler->IsCatchBlock());
    block->RemoveSuccessor(handler);
    handler->RemovePredecessor(block);
    if (handler->IsInLoop()) {
      *any_handler_in_loop = true;
    }
  }

  // Change TryBoundary to Goto.
  DCHECK(block->EndsWithTryBoundary());
  HInstruction* last = block->GetLastInstruction();
  block->RemoveInstruction(last);
  block->AddInstruction(new (graph_->GetAllocator()) HGoto(last->GetDexPc()));
  DCHECK_EQ(block->GetSuccessors().size(), 1u);
}

void HDeadCodeElimination::RemoveTry(HBasicBlock* try_entry,
                                     const TryBelongingInformation& try_belonging_info,
                                     /* out */ bool* any_handler_in_loop) {
  // Update all try entries.
  DCHECK(try_entry->EndsWithTryBoundary());
  DCHECK(try_entry->GetLastInstruction()->AsTryBoundary()->IsEntry());
  DisconnectHandlersAndUpdateTryBoundary(try_entry, any_handler_in_loop);

  for (HBasicBlock* other_try_entry : try_belonging_info.coalesced_try_entries) {
    DCHECK(other_try_entry->EndsWithTryBoundary());
    DCHECK(other_try_entry->GetLastInstruction()->AsTryBoundary()->IsEntry());
    DisconnectHandlersAndUpdateTryBoundary(other_try_entry, any_handler_in_loop);
  }

  // Update the blocks in the try.
  for (HBasicBlock* block : try_belonging_info.blocks_in_try) {
    // Update the try catch information since now the try doesn't exist.
    block->SetTryCatchInformation(nullptr);

    if (block->EndsWithTryBoundary()) {
      // Try exits.
      DCHECK(!block->GetLastInstruction()->AsTryBoundary()->IsEntry());
      DisconnectHandlersAndUpdateTryBoundary(block, any_handler_in_loop);

      if (block->GetSingleSuccessor()->IsExitBlock()) {
        // `predecessor` used to be a single exit TryBoundary that got turned into a Goto. It
        // is now pointing to the exit which we don't allow. To fix it, we disconnect
        // `predecessor` from its predecessor and RemoveDeadBlocks will remove it from the
        // graph.
        DCHECK(block->IsSingleGoto());
        HBasicBlock* predecessor = block->GetSinglePredecessor();
        predecessor->ReplaceSuccessor(block, graph_->GetExitBlock());
      }
    }
  }
}

bool HDeadCodeElimination::RemoveUnneededTries() {
  if (!graph_->HasTryCatch()) {
    return false;
  }

  // Use local allocator for allocating memory.
  ScopedArenaAllocator allocator(graph_->GetArenaStack());

  // Collect which blocks are part of which try.
  std::unordered_map<HBasicBlock*, TryBelongingInformation> tries;
  for (HBasicBlock* block : graph_->GetReversePostOrderSkipEntryBlock()) {
    if (block->IsTryBlock()) {
      HBasicBlock* key = block->GetTryCatchInformation()->GetTryEntry().GetBlock();
      auto it = tries.find(key);
      if (it == tries.end()) {
        it = tries.insert({key, TryBelongingInformation(&allocator)}).first;
      }
      it->second.blocks_in_try.insert(block);
    }
  }

  // Deduplicate the tries which have different try entries but they are really the same try.
  for (auto it = tries.begin(); it != tries.end(); it++) {
    DCHECK(it->first->EndsWithTryBoundary());
    HTryBoundary* try_boundary = it->first->GetLastInstruction()->AsTryBoundary();
    for (auto other_it = next(it); other_it != tries.end(); /*other_it++ in the loop*/) {
      DCHECK(other_it->first->EndsWithTryBoundary());
      HTryBoundary* other_try_boundary = other_it->first->GetLastInstruction()->AsTryBoundary();
      if (try_boundary->HasSameExceptionHandlersAs(*other_try_boundary)) {
        // Merge the entries as they are really the same one.
        // Block merging.
        it->second.blocks_in_try.insert(other_it->second.blocks_in_try.begin(),
                                        other_it->second.blocks_in_try.end());

        // Add the coalesced try entry to update it too.
        it->second.coalesced_try_entries.insert(other_it->first);

        // Erase the other entry.
        other_it = tries.erase(other_it);
      } else {
        other_it++;
      }
    }
  }

  const size_t total_tries = tries.size();
  size_t removed_tries = 0;
  bool any_handler_in_loop = false;

  // Check which tries contain throwing instructions.
  for (const auto& entry : tries) {
    if (CanPerformTryRemoval(entry.second)) {
      ++removed_tries;
      RemoveTry(entry.first, entry.second, &any_handler_in_loop);
    }
  }

  if (removed_tries == total_tries) {
    graph_->SetHasTryCatch(false);
  }

  if (removed_tries != 0) {
    // We want to:
    //   1) Update the dominance information
    //   2) Remove catch block subtrees, if they are now unreachable.
    // If we run the dominance recomputation without removing the code, those catch blocks will
    // not be part of the post order and won't be removed. If we don't run the dominance
    // recomputation, we risk RemoveDeadBlocks not running it and leaving the graph in an
    // inconsistent state. So, what we can do is run RemoveDeadBlocks and if it didn't remove any
    // block we trigger a recomputation.
    // Note that we are not guaranteed to remove a catch block if we have nested try blocks:
    //
    //   try {
    //     ... nothing can throw. TryBoundary A ...
    //     try {
    //       ... can throw. TryBoundary B...
    //     } catch (Error e) {}
    //   } catch (Exception e) {}
    //
    // In the example above, we can remove the TryBoundary A but the Exception catch cannot be
    // removed as the TryBoundary B might still throw into that catch. TryBoundary A and B don't get
    // coalesced since they have different catch handlers.

    if (!RemoveDeadBlocks()) {
      // If the catches that we modified were in a loop, we have to update the loop information.
      if (any_handler_in_loop) {
        graph_->ClearLoopInformation();
        graph_->ClearDominanceInformation();
        graph_->BuildDominatorTree();
      } else {
        graph_->ClearDominanceInformation();
        graph_->ComputeDominanceInformation();
        graph_->ComputeTryBlockInformation();
      }
    }
    MaybeRecordStat(stats_, MethodCompilationStat::kRemovedTry, removed_tries);
    return true;
  } else {
    return false;
  }
}

bool HDeadCodeElimination::RemoveDeadBlocks() {
  // Use local allocator for allocating memory.
  ScopedArenaAllocator allocator(graph_->GetArenaStack());

  // Classify blocks as reachable/unreachable.
  ArenaBitVector live_blocks(&allocator, graph_->GetBlocks().size(), false, kArenaAllocDCE);
  live_blocks.ClearAllBits();

  MarkReachableBlocks(graph_, &live_blocks);
  bool removed_one_or_more_blocks = false;
  bool rerun_dominance_and_loop_analysis = false;

  // Remove all dead blocks. Iterate in post order because removal needs the
  // block's chain of dominators and nested loops need to be updated from the
  // inside out.
  for (HBasicBlock* block : graph_->GetPostOrder()) {
    int id = block->GetBlockId();
    if (!live_blocks.IsBitSet(id)) {
      MaybeRecordDeadBlock(block);
      block->DisconnectAndDelete();
      removed_one_or_more_blocks = true;
      if (block->IsInLoop()) {
        rerun_dominance_and_loop_analysis = true;
      }
    }
  }

  // If we removed at least one block, we need to recompute the full
  // dominator tree and try block membership.
  if (removed_one_or_more_blocks) {
    if (rerun_dominance_and_loop_analysis) {
      graph_->ClearLoopInformation();
      graph_->ClearDominanceInformation();
      graph_->BuildDominatorTree();
    } else {
      graph_->ClearDominanceInformation();
      graph_->ComputeDominanceInformation();
      graph_->ComputeTryBlockInformation();
    }
  }
  return removed_one_or_more_blocks;
}

void HDeadCodeElimination::RemoveDeadInstructions() {
  // Process basic blocks in post-order in the dominator tree, so that
  // a dead instruction depending on another dead instruction is removed.
  for (HBasicBlock* block : graph_->GetPostOrder()) {
    // Traverse this block's instructions in backward order and remove
    // the unused ones.
    HBackwardInstructionIterator i(block->GetInstructions());
    // Skip the first iteration, as the last instruction of a block is
    // a branching instruction.
    DCHECK(i.Current()->IsControlFlow());
    for (i.Advance(); !i.Done(); i.Advance()) {
      HInstruction* inst = i.Current();
      DCHECK(!inst->IsControlFlow());
      if (inst->IsDeadAndRemovable()) {
        block->RemoveInstruction(inst);
        MaybeRecordStat(stats_, MethodCompilationStat::kRemovedDeadInstruction);
      }
    }
  }
}

bool HDeadCodeElimination::Run() {
  // Do not eliminate dead blocks if the graph has irreducible loops. We could
  // support it, but that would require changes in our loop representation to handle
  // multiple entry points. We decided it was not worth the complexity.
  if (!graph_->HasIrreducibleLoops()) {
    // Simplify graph to generate more dead block patterns.
    ConnectSuccessiveBlocks();
    bool did_any_simplification = false;
    did_any_simplification |= SimplifyAlwaysThrows();
    did_any_simplification |= SimplifyIfs();
    did_any_simplification |= RemoveDeadBlocks();
    // We call RemoveDeadBlocks before RemoveUnneededTries to remove the dead blocks from the
    // previous optimizations. Otherwise, we might detect that a try has throwing instructions but
    // they are actually dead code. RemoveUnneededTryBoundary will call RemoveDeadBlocks again if
    // needed.
    did_any_simplification |= RemoveUnneededTries();
    if (did_any_simplification) {
      // Connect successive blocks created by dead branches.
      ConnectSuccessiveBlocks();
    }
  }
  SsaRedundantPhiElimination(graph_).Run();
  RemoveDeadInstructions();
  return true;
}

}  // namespace art
