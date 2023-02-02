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

#ifndef ART_COMPILER_OPTIMIZING_DEAD_CODE_ELIMINATION_H_
#define ART_COMPILER_OPTIMIZING_DEAD_CODE_ELIMINATION_H_

#include "nodes.h"
#include "optimization.h"
#include "optimizing_compiler_stats.h"

namespace art {

/**
 * Optimization pass performing dead code elimination (removal of
 * unused variables/instructions) on the SSA form.
 */
class HDeadCodeElimination : public HOptimization {
 public:
  HDeadCodeElimination(HGraph* graph, OptimizingCompilerStats* stats, const char* name)
      : HOptimization(graph, name, stats) {}

  bool Run() override;

  static constexpr const char* kDeadCodeEliminationPassName = "dead_code_elimination";

 private:
  void MaybeRecordDeadBlock(HBasicBlock* block);
  void MaybeRecordSimplifyIf();
  bool RemoveDeadBlocks();
  void RemoveDeadInstructions();
  bool SimplifyAlwaysThrows();
  // Simplify the pattern:
  //
  //        B1    B2    ...
  //       goto  goto  goto
  //         \    |    /
  //          \   |   /
  //             B3
  //     i1 = phi(input, input)
  //     (i2 = condition on i1)
  //        if i1 (or i2)
  //          /     \
  //         /       \
  //        B4       B5
  //
  // Into:
  //
  //       B1      B2    ...
  //        |      |      |
  //       B4      B5    B?
  //
  // Note that individual edges can be redirected (for example B2->B3
  // can be redirected as B2->B5) without applying this optimization
  // to other incoming edges.
  //
  // Note that we rely on the dead code elimination to get rid of B3.
  bool SimplifyIfs();
  void ConnectSuccessiveBlocks();

  // Helper struct to eliminate tries.
  struct TryBelongingInformation;
  // Disconnects `block`'s handlers and update its `TryBoundary` instruction to a `Goto`.
  // Sets `any_handler_in_loop` to true if any handler is currently a loop to later update the loop
  // information if needed.
  void DisconnectHandlersAndUpdateTryBoundary(HBasicBlock* block,
                                              /* out */ bool* any_handler_in_loop);
  // Returns true iff the try doesn't contain throwing instructions.
  bool CanPerformTryRemoval(const TryBelongingInformation& try_belonging_info);
  // Removes the try by disconnecting all try entries and exits from their handlers. Also updates
  // the graph in the case that a `TryBoundary` instruction of kind `exit` has the Exit block as
  // its successor.
  void RemoveTry(HBasicBlock* try_entry,
                 const TryBelongingInformation& try_belonging_info,
                 bool* any_catch_in_loop);
  // Checks which tries (if any) are currently in the graph, coalesces the different try entries
  // that are referencing the same try, and removes the tries which don't contain any throwing
  // instructions.
  bool RemoveUnneededTries();

  // Adds a phi in `block`, if `block` and its dominator have the same (or opposite) condition.
  // For example it turns:
  // if(cond)
  //   /  \
  //  B1  B2
  //   \ /
  // if(cond)
  //   /  \
  //  B3  B4
  //
  // into:
  // if(cond)
  //   /  \
  //  B1  B2
  //   \ /
  // if(Phi(1, 0))
  //   /  \
  //  B3  B4
  //
  // Following this, SimplifyIfs is able to connect B1->B3 and B2->B4 effectively skipping an if.
  void MaybeAddPhi(HBasicBlock* block);

  DISALLOW_COPY_AND_ASSIGN(HDeadCodeElimination);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_DEAD_CODE_ELIMINATION_H_
