/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include <tuple>

#include "load_store_analysis.h"
#include "load_store_elimination.h"
#include "nodes.h"
#include "optimizing_unit_test.h"

#include "gtest/gtest.h"

namespace art {

class LoadStoreEliminationTest : public ImprovedOptimizingUnitTest {
 public:
  void PerformLSE() {
    graph_->BuildDominatorTree();
    LoadStoreElimination lse(graph_, /*stats=*/ nullptr);
    lse.Run();
    EXPECT_TRUE(CheckGraphSkipRefTypeInfoChecks());
  }

  // Create instructions shared among tests.
  void CreateEntryBlockInstructions() {
    HInstruction* c1 = graph_->GetIntConstant(1);
    HInstruction* c4 = graph_->GetIntConstant(4);
    i_add1_ = new (GetAllocator()) HAdd(DataType::Type::kInt32, i_, c1);
    i_add4_ = new (GetAllocator()) HAdd(DataType::Type::kInt32, i_, c4);
    entry_block_->AddInstruction(i_add1_);
    entry_block_->AddInstruction(i_add4_);
    entry_block_->AddInstruction(new (GetAllocator()) HGoto());
  }

  // Create the major CFG used by tests:
  //    entry
  //      |
  //  pre_header
  //      |
  //    loop[]
  //      |
  //   return
  //      |
  //     exit
  void CreateTestControlFlowGraph() {
    pre_header_ = new (GetAllocator()) HBasicBlock(graph_);
    loop_ = new (GetAllocator()) HBasicBlock(graph_);

    graph_->AddBlock(pre_header_);
    graph_->AddBlock(loop_);

    entry_block_->ReplaceSuccessor(return_block_, pre_header_);
    pre_header_->AddSuccessor(loop_);
    loop_->AddSuccessor(loop_);
    loop_->AddSuccessor(return_block_);

    HInstruction* c0 = graph_->GetIntConstant(0);
    HInstruction* c1 = graph_->GetIntConstant(1);
    HInstruction* c128 = graph_->GetIntConstant(128);

    CreateEntryBlockInstructions();

    // pre_header block
    //   phi = 0;
    phi_ = new (GetAllocator()) HPhi(GetAllocator(), 0, 0, DataType::Type::kInt32);
    loop_->AddPhi(phi_);
    pre_header_->AddInstruction(new (GetAllocator()) HGoto());
    phi_->AddInput(c0);

    // loop block:
    //   suspend_check
    //   phi++;
    //   if (phi >= 128)
    suspend_check_ = new (GetAllocator()) HSuspendCheck();
    HInstruction* inc_phi = new (GetAllocator()) HAdd(DataType::Type::kInt32, phi_, c1);
    HInstruction* cmp = new (GetAllocator()) HGreaterThanOrEqual(phi_, c128);
    HInstruction* hif = new (GetAllocator()) HIf(cmp);
    loop_->AddInstruction(suspend_check_);
    loop_->AddInstruction(inc_phi);
    loop_->AddInstruction(cmp);
    loop_->AddInstruction(hif);
    phi_->AddInput(inc_phi);

    CreateEnvForSuspendCheck();
  }

  void CreateEnvForSuspendCheck() {
    ArenaVector<HInstruction*> current_locals({array_, i_, j_},
                                              GetAllocator()->Adapter(kArenaAllocInstruction));
    ManuallyBuildEnvFor(suspend_check_, &current_locals);
  }

  // Create the diamond-shaped CFG:
  //      upper
  //      /   \
  //    left  right
  //      \   /
  //      down
  //
  // Return: the basic blocks forming the CFG in the following order {upper, left, right, down}.
  std::tuple<HBasicBlock*, HBasicBlock*, HBasicBlock*, HBasicBlock*> CreateDiamondShapedCFG() {
    CreateEntryBlockInstructions();

    HBasicBlock* upper = new (GetAllocator()) HBasicBlock(graph_);
    HBasicBlock* left = new (GetAllocator()) HBasicBlock(graph_);
    HBasicBlock* right = new (GetAllocator()) HBasicBlock(graph_);

    graph_->AddBlock(upper);
    graph_->AddBlock(left);
    graph_->AddBlock(right);

    entry_block_->ReplaceSuccessor(return_block_, upper);
    upper->AddSuccessor(left);
    upper->AddSuccessor(right);
    left->AddSuccessor(return_block_);
    right->AddSuccessor(return_block_);

    HInstruction* cmp = new (GetAllocator()) HGreaterThanOrEqual(i_, j_);
    HInstruction* hif = new (GetAllocator()) HIf(cmp);
    upper->AddInstruction(cmp);
    upper->AddInstruction(hif);

    left->AddInstruction(new (GetAllocator()) HGoto());
    right->AddInstruction(new (GetAllocator()) HGoto());

    return std::make_tuple(upper, left, right, return_block_);
  }

  // Add a HVecLoad instruction to the end of the provided basic block.
  //
  // Return: the created HVecLoad instruction.
  HInstruction* AddVecLoad(HBasicBlock* block, HInstruction* array, HInstruction* index) {
    DCHECK(block != nullptr);
    DCHECK(array != nullptr);
    DCHECK(index != nullptr);
    HInstruction* vload = new (GetAllocator()) HVecLoad(
        GetAllocator(),
        array,
        index,
        DataType::Type::kInt32,
        SideEffects::ArrayReadOfType(DataType::Type::kInt32),
        4,
        /*is_string_char_at*/ false,
        kNoDexPc);
    block->InsertInstructionBefore(vload, block->GetLastInstruction());
    return vload;
  }

  // Add a HVecStore instruction to the end of the provided basic block.
  // If no vdata is specified, generate HVecStore: array[index] = [1,1,1,1].
  //
  // Return: the created HVecStore instruction.
  HInstruction* AddVecStore(HBasicBlock* block,
                            HInstruction* array,
                            HInstruction* index,
                            HInstruction* vdata = nullptr) {
    DCHECK(block != nullptr);
    DCHECK(array != nullptr);
    DCHECK(index != nullptr);
    if (vdata == nullptr) {
      HInstruction* c1 = graph_->GetIntConstant(1);
      vdata = new (GetAllocator()) HVecReplicateScalar(GetAllocator(),
                                                       c1,
                                                       DataType::Type::kInt32,
                                                       4,
                                                       kNoDexPc);
      block->InsertInstructionBefore(vdata, block->GetLastInstruction());
    }
    HInstruction* vstore = new (GetAllocator()) HVecStore(
        GetAllocator(),
        array,
        index,
        vdata,
        DataType::Type::kInt32,
        SideEffects::ArrayWriteOfType(DataType::Type::kInt32),
        4,
        kNoDexPc);
    block->InsertInstructionBefore(vstore, block->GetLastInstruction());
    return vstore;
  }

  // Add a HArrayGet instruction to the end of the provided basic block.
  //
  // Return: the created HArrayGet instruction.
  HInstruction* AddArrayGet(HBasicBlock* block, HInstruction* array, HInstruction* index) {
    DCHECK(block != nullptr);
    DCHECK(array != nullptr);
    DCHECK(index != nullptr);
    HInstruction* get = new (GetAllocator()) HArrayGet(array, index, DataType::Type::kInt32, 0);
    block->InsertInstructionBefore(get, block->GetLastInstruction());
    return get;
  }

  // Add a HArraySet instruction to the end of the provided basic block.
  // If no data is specified, generate HArraySet: array[index] = 1.
  //
  // Return: the created HArraySet instruction.
  HInstruction* AddArraySet(HBasicBlock* block,
                            HInstruction* array,
                            HInstruction* index,
                            HInstruction* data = nullptr) {
    DCHECK(block != nullptr);
    DCHECK(array != nullptr);
    DCHECK(index != nullptr);
    if (data == nullptr) {
      data = graph_->GetIntConstant(1);
    }
    HInstruction* store = new (GetAllocator()) HArraySet(array,
                                                         index,
                                                         data,
                                                         DataType::Type::kInt32,
                                                         0);
    block->InsertInstructionBefore(store, block->GetLastInstruction());
    return store;
  }

  void CreateParameters() override {
    parameters_.push_back(new (GetAllocator()) HParameterValue(graph_->GetDexFile(),
                                                               dex::TypeIndex(0),
                                                               0,
                                                               DataType::Type::kInt32));
    array_ = parameters_.back();
    parameters_.push_back(new (GetAllocator()) HParameterValue(graph_->GetDexFile(),
                                                               dex::TypeIndex(1),
                                                               1,
                                                               DataType::Type::kInt32));
    i_ = parameters_.back();
    parameters_.push_back(new (GetAllocator()) HParameterValue(graph_->GetDexFile(),
                                                               dex::TypeIndex(1),
                                                               2,
                                                               DataType::Type::kInt32));
    j_ = parameters_.back();
  }

  HBasicBlock* pre_header_;
  HBasicBlock* loop_;

  HInstruction* array_;
  HInstruction* i_;
  HInstruction* j_;
  HInstruction* i_add1_;
  HInstruction* i_add4_;
  HInstruction* suspend_check_;

  HPhi* phi_;
};

TEST_F(LoadStoreEliminationTest, ArrayGetSetElimination) {
  InitGraph();
  CreateTestControlFlowGraph();

  HInstruction* c1 = graph_->GetIntConstant(1);
  HInstruction* c2 = graph_->GetIntConstant(2);
  HInstruction* c3 = graph_->GetIntConstant(3);

  // array[1] = 1;
  // x = array[1];  <--- Remove.
  // y = array[2];
  // array[1] = 1;  <--- Remove, since it stores same value.
  // array[i] = 3;  <--- MAY alias.
  // array[1] = 1;  <--- Cannot remove, even if it stores the same value.
  AddArraySet(entry_block_, array_, c1, c1);
  HInstruction* load1 = AddArrayGet(entry_block_, array_, c1);
  HInstruction* load2 = AddArrayGet(entry_block_, array_, c2);
  HInstruction* store1 = AddArraySet(entry_block_, array_, c1, c1);
  AddArraySet(entry_block_, array_, i_, c3);
  HInstruction* store2 = AddArraySet(entry_block_, array_, c1, c1);

  PerformLSE();

  ASSERT_TRUE(IsRemoved(load1));
  ASSERT_FALSE(IsRemoved(load2));
  ASSERT_TRUE(IsRemoved(store1));
  ASSERT_FALSE(IsRemoved(store2));
}

TEST_F(LoadStoreEliminationTest, SameHeapValue1) {
  InitGraph();
  CreateTestControlFlowGraph();

  HInstruction* c1 = graph_->GetIntConstant(1);
  HInstruction* c2 = graph_->GetIntConstant(2);

  // Test LSE handling same value stores on array.
  // array[1] = 1;
  // array[2] = 1;
  // array[1] = 1;  <--- Can remove.
  // array[1] = 2;  <--- Can NOT remove.
  AddArraySet(entry_block_, array_, c1, c1);
  AddArraySet(entry_block_, array_, c2, c1);
  HInstruction* store1 = AddArraySet(entry_block_, array_, c1, c1);
  HInstruction* store2 = AddArraySet(entry_block_, array_, c1, c2);

  PerformLSE();

  ASSERT_TRUE(IsRemoved(store1));
  ASSERT_FALSE(IsRemoved(store2));
}

TEST_F(LoadStoreEliminationTest, SameHeapValue2) {
  InitGraph();
  CreateTestControlFlowGraph();

  // Test LSE handling same value stores on vector.
  // vdata = [0x1, 0x2, 0x3, 0x4, ...]
  // VecStore array[i...] = vdata;
  // VecStore array[j...] = vdata;  <--- MAY ALIAS.
  // VecStore array[i...] = vdata;  <--- Cannot Remove, even if it's same value.
  AddVecStore(entry_block_, array_, i_);
  AddVecStore(entry_block_, array_, j_);
  HInstruction* vstore = AddVecStore(entry_block_, array_, i_);

  PerformLSE();

  ASSERT_FALSE(IsRemoved(vstore));
}

TEST_F(LoadStoreEliminationTest, SameHeapValue3) {
  InitGraph();
  CreateTestControlFlowGraph();

  // VecStore array[i...] = vdata;
  // VecStore array[i+1...] = vdata;  <--- MAY alias due to partial overlap.
  // VecStore array[i...] = vdata;    <--- Cannot remove, even if it's same value.
  AddVecStore(entry_block_, array_, i_);
  AddVecStore(entry_block_, array_, i_add1_);
  HInstruction* vstore = AddVecStore(entry_block_, array_, i_);

  PerformLSE();

  ASSERT_FALSE(IsRemoved(vstore));
}

TEST_F(LoadStoreEliminationTest, OverlappingLoadStore) {
  InitGraph();
  CreateTestControlFlowGraph();

  HInstruction* c1 = graph_->GetIntConstant(1);

  // Test LSE handling array LSE when there is vector store in between.
  // a[i] = 1;
  // .. = a[i];                <-- Remove.
  // a[i,i+1,i+2,i+3] = data;  <-- PARTIAL OVERLAP !
  // .. = a[i];                <-- Cannot remove.
  AddArraySet(entry_block_, array_, i_, c1);
  HInstruction* load1 = AddArrayGet(entry_block_, array_, i_);
  AddVecStore(entry_block_, array_, i_);
  HInstruction* load2 = AddArrayGet(entry_block_, array_, i_);

  // Test LSE handling vector load/store partial overlap.
  // a[i,i+1,i+2,i+3] = data;
  // a[i+4,i+5,i+6,i+7] = data;
  // .. = a[i,i+1,i+2,i+3];
  // .. = a[i+4,i+5,i+6,i+7];
  // a[i+1,i+2,i+3,i+4] = data;  <-- PARTIAL OVERLAP !
  // .. = a[i,i+1,i+2,i+3];
  // .. = a[i+4,i+5,i+6,i+7];
  AddVecStore(entry_block_, array_, i_);
  AddVecStore(entry_block_, array_, i_add4_);
  HInstruction* vload1 = AddVecLoad(entry_block_, array_, i_);
  HInstruction* vload2 = AddVecLoad(entry_block_, array_, i_add4_);
  AddVecStore(entry_block_, array_, i_add1_);
  HInstruction* vload3 = AddVecLoad(entry_block_, array_, i_);
  HInstruction* vload4 = AddVecLoad(entry_block_, array_, i_add4_);

  // Test LSE handling vector LSE when there is array store in between.
  // a[i,i+1,i+2,i+3] = data;
  // a[i+1] = 1;                 <-- PARTIAL OVERLAP !
  // .. = a[i,i+1,i+2,i+3];
  AddVecStore(entry_block_, array_, i_);
  AddArraySet(entry_block_, array_, i_, c1);
  HInstruction* vload5 = AddVecLoad(entry_block_, array_, i_);

  PerformLSE();

  ASSERT_TRUE(IsRemoved(load1));
  ASSERT_FALSE(IsRemoved(load2));

  ASSERT_TRUE(IsRemoved(vload1));
  ASSERT_TRUE(IsRemoved(vload2));
  ASSERT_FALSE(IsRemoved(vload3));
  ASSERT_FALSE(IsRemoved(vload4));

  ASSERT_FALSE(IsRemoved(vload5));
}
// function (int[] a, int j) {
// a[j] = 1;
// for (int i=0; i<128; i++) {
//    /* doesn't do any write */
// }
// a[j] = 1;
TEST_F(LoadStoreEliminationTest, StoreAfterLoopWithoutSideEffects) {
  InitGraph();
  CreateTestControlFlowGraph();

  HInstruction* c1 = graph_->GetIntConstant(1);

  // a[j] = 1
  AddArraySet(pre_header_, array_, j_, c1);

  // LOOP BODY:
  // .. = a[i,i+1,i+2,i+3];
  AddVecLoad(loop_, array_, phi_);

  // a[j] = 1;
  HInstruction* array_set = AddArraySet(return_block_, array_, j_, c1);

  PerformLSE();

  ASSERT_TRUE(IsRemoved(array_set));
}

// function (int[] a, int j) {
//   int[] b = new int[128];
//   a[j] = 0;
//   for (int phi=0; phi<128; phi++) {
//     a[phi,phi+1,phi+2,phi+3] = [1,1,1,1];
//     b[phi,phi+1,phi+2,phi+3] = a[phi,phi+1,phi+2,phi+3];
//   }
//   a[j] = 0;
// }
TEST_F(LoadStoreEliminationTest, StoreAfterSIMDLoopWithSideEffects) {
  InitGraph();
  CreateTestControlFlowGraph();

  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c128 = graph_->GetIntConstant(128);

  HInstruction* array_b = new (GetAllocator()) HNewArray(c0, c128, 0, 0);
  pre_header_->InsertInstructionBefore(array_b, pre_header_->GetLastInstruction());
  array_b->CopyEnvironmentFrom(suspend_check_->GetEnvironment());

  // a[j] = 0;
  AddArraySet(pre_header_, array_, j_, c0);

  // LOOP BODY:
  // a[phi,phi+1,phi+2,phi+3] = [1,1,1,1];
  // b[phi,phi+1,phi+2,phi+3] = a[phi,phi+1,phi+2,phi+3];
  AddVecStore(loop_, array_, phi_);
  HInstruction* vload = AddVecLoad(loop_, array_, phi_);
  AddVecStore(loop_, array_b, phi_, vload->AsVecLoad());

  // a[j] = 0;
  HInstruction* a_set = AddArraySet(return_block_, array_, j_, c0);

  PerformLSE();

  ASSERT_TRUE(IsRemoved(vload));
  ASSERT_FALSE(IsRemoved(a_set));  // Cannot remove due to write side-effect in the loop.
}

// function (int[] a, int j) {
//   int[] b = new int[128];
//   a[j] = 0;
//   for (int phi=0; phi<128; phi++) {
//     a[phi,phi+1,phi+2,phi+3] = [1,1,1,1];
//     b[phi,phi+1,phi+2,phi+3] = a[phi,phi+1,phi+2,phi+3];
//   }
//   x = a[j];
// }
TEST_F(LoadStoreEliminationTest, LoadAfterSIMDLoopWithSideEffects) {
  InitGraph();
  CreateTestControlFlowGraph();

  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c128 = graph_->GetIntConstant(128);

  HInstruction* array_b = new (GetAllocator()) HNewArray(c0, c128, 0, 0);
  pre_header_->InsertInstructionBefore(array_b, pre_header_->GetLastInstruction());
  array_b->CopyEnvironmentFrom(suspend_check_->GetEnvironment());

  // a[j] = 0;
  AddArraySet(pre_header_, array_, j_, c0);

  // LOOP BODY:
  // a[phi,phi+1,phi+2,phi+3] = [1,1,1,1];
  // b[phi,phi+1,phi+2,phi+3] = a[phi,phi+1,phi+2,phi+3];
  AddVecStore(loop_, array_, phi_);
  HInstruction* vload = AddVecLoad(loop_, array_, phi_);
  AddVecStore(loop_, array_b, phi_, vload->AsVecLoad());

  // x = a[j];
  HInstruction* load = AddArrayGet(return_block_, array_, j_);

  PerformLSE();

  ASSERT_TRUE(IsRemoved(vload));
  ASSERT_FALSE(IsRemoved(load));  // Cannot remove due to write side-effect in the loop.
}

// Check that merging works correctly when there are VecStors in predecessors.
//
//                  vstore1: a[i,... i + 3] = [1,...1]
//                       /          \
//                      /            \
// vstore2: a[i,... i + 3] = [1,...1]  vstore3: a[i+1, ... i + 4] = [1, ... 1]
//                     \              /
//                      \            /
//                  vstore4: a[i,... i + 3] = [1,...1]
//
// Expected:
//   'vstore2' is removed.
//   'vstore3' is not removed.
//   'vstore4' is not removed. Such cases are not supported at the moment.
TEST_F(LoadStoreEliminationTest, MergePredecessorVecStores) {
  InitGraph();

  HBasicBlock* upper;
  HBasicBlock* left;
  HBasicBlock* right;
  HBasicBlock* down;
  std::tie(upper, left, right, down) = CreateDiamondShapedCFG();

  // upper: a[i,... i + 3] = [1,...1]
  HInstruction* vstore1 = AddVecStore(upper, array_, i_);
  HInstruction* vdata = vstore1->InputAt(2);

  // left: a[i,... i + 3] = [1,...1]
  HInstruction* vstore2 = AddVecStore(left, array_, i_, vdata);

  // right: a[i+1, ... i + 4] = [1, ... 1]
  HInstruction* vstore3 = AddVecStore(right, array_, i_add1_, vdata);

  // down: a[i,... i + 3] = [1,...1]
  HInstruction* vstore4 = AddVecStore(down, array_, i_, vdata);

  PerformLSE();

  ASSERT_TRUE(IsRemoved(vstore2));
  ASSERT_FALSE(IsRemoved(vstore3));
  ASSERT_FALSE(IsRemoved(vstore4));
}

// Check that merging works correctly when there are ArraySets in predecessors.
//
//          a[i] = 1
//        /          \
//       /            \
// store1: a[i] = 1  store2: a[i+1] = 1
//       \            /
//        \          /
//          store3: a[i] = 1
//
// Expected:
//   'store1' is removed.
//   'store2' is not removed.
//   'store3' is removed.
TEST_F(LoadStoreEliminationTest, MergePredecessorStores) {
  InitGraph();

  HBasicBlock* upper;
  HBasicBlock* left;
  HBasicBlock* right;
  HBasicBlock* down;
  std::tie(upper, left, right, down) = CreateDiamondShapedCFG();

  // upper: a[i,... i + 3] = [1,...1]
  AddArraySet(upper, array_, i_);

  // left: a[i,... i + 3] = [1,...1]
  HInstruction* store1 = AddArraySet(left, array_, i_);

  // right: a[i+1, ... i + 4] = [1, ... 1]
  HInstruction* store2 = AddArraySet(right, array_, i_add1_);

  // down: a[i,... i + 3] = [1,...1]
  HInstruction* store3 = AddArraySet(down, array_, i_);

  PerformLSE();

  ASSERT_TRUE(IsRemoved(store1));
  ASSERT_FALSE(IsRemoved(store2));
  ASSERT_TRUE(IsRemoved(store3));
}

// Check that redundant VStore/VLoad are removed from a SIMD loop.
//
//  LOOP BODY
//     vstore1: a[i,... i + 3] = [1,...1]
//     vload:   x = a[i,... i + 3]
//     vstore2: b[i,... i + 3] = x
//     vstore3: a[i,... i + 3] = [1,...1]
//
// Return 'a' from the method to make it escape.
//
// Expected:
//   'vstore1' is not removed.
//   'vload' is removed.
//   'vstore2' is removed because 'b' does not escape.
//   'vstore3' is removed.
TEST_F(LoadStoreEliminationTest, RedundantVStoreVLoadInLoop) {
  InitGraph();
  CreateTestControlFlowGraph();

  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c128 = graph_->GetIntConstant(128);

  HInstruction* array_a = new (GetAllocator()) HNewArray(c0, c128, 0, 0);
  pre_header_->InsertInstructionBefore(array_a, pre_header_->GetLastInstruction());
  array_a->CopyEnvironmentFrom(suspend_check_->GetEnvironment());

  ASSERT_TRUE(return_block_->GetLastInstruction()->IsReturnVoid());
  HInstruction* ret = new (GetAllocator()) HReturn(array_a);
  return_block_->ReplaceAndRemoveInstructionWith(return_block_->GetLastInstruction(), ret);

  HInstruction* array_b = new (GetAllocator()) HNewArray(c0, c128, 0, 0);
  pre_header_->InsertInstructionBefore(array_b, pre_header_->GetLastInstruction());
  array_b->CopyEnvironmentFrom(suspend_check_->GetEnvironment());

  // LOOP BODY:
  //    a[i,... i + 3] = [1,...1]
  //    x = a[i,... i + 3]
  //    b[i,... i + 3] = x
  //    a[i,... i + 3] = [1,...1]
  HInstruction* vstore1 = AddVecStore(loop_, array_a, phi_);
  HInstruction* vload = AddVecLoad(loop_, array_a, phi_);
  HInstruction* vstore2 = AddVecStore(loop_, array_b, phi_, vload->AsVecLoad());
  HInstruction* vstore3 = AddVecStore(loop_, array_a, phi_, vstore1->InputAt(2));

  PerformLSE();

  ASSERT_FALSE(IsRemoved(vstore1));
  ASSERT_TRUE(IsRemoved(vload));
  ASSERT_TRUE(IsRemoved(vstore2));
  ASSERT_TRUE(IsRemoved(vstore3));
}

// Loop writes invalidate only possibly aliased heap locations.
TEST_F(LoadStoreEliminationTest, StoreAfterLoopWithSideEffects) {
  InitGraph();
  CreateTestControlFlowGraph();

  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c2 = graph_->GetIntConstant(2);
  HInstruction* c128 = graph_->GetIntConstant(128);

  // array[0] = 2;
  // loop:
  //   b[i] = array[i]
  // array[0] = 2
  HInstruction* store1 = AddArraySet(entry_block_, array_, c0, c2);

  HInstruction* array_b = new (GetAllocator()) HNewArray(c0, c128, 0, 0);
  pre_header_->InsertInstructionBefore(array_b, pre_header_->GetLastInstruction());
  array_b->CopyEnvironmentFrom(suspend_check_->GetEnvironment());

  HInstruction* load = AddArrayGet(loop_, array_, phi_);
  HInstruction* store2 = AddArraySet(loop_, array_b, phi_, load);

  HInstruction* store3 = AddArraySet(return_block_, array_, c0, c2);

  PerformLSE();

  ASSERT_FALSE(IsRemoved(store1));
  ASSERT_TRUE(IsRemoved(store2));
  ASSERT_TRUE(IsRemoved(store3));
}

// Loop writes invalidate only possibly aliased heap locations.
TEST_F(LoadStoreEliminationTest, StoreAfterLoopWithSideEffects2) {
  CreateTestControlFlowGraph();

  // Add another array parameter that may alias with `array_`.
  // Note: We're not adding it to the suspend check environment.
  AddParameter(new (GetAllocator()) HParameterValue(graph_->GetDexFile(),
                                                    dex::TypeIndex(0),
                                                    3,
                                                    DataType::Type::kInt32));
  HInstruction* array2 = parameters_.back();

  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c2 = graph_->GetIntConstant(2);

  // array[0] = 2;
  // loop:
  //   array2[i] = array[i]
  // array[0] = 2
  HInstruction* store1 = AddArraySet(entry_block_, array_, c0, c2);

  HInstruction* load = AddArrayGet(loop_, array_, phi_);
  HInstruction* store2 = AddArraySet(loop_, array2, phi_, load);

  HInstruction* store3 = AddArraySet(return_block_, array_, c0, c2);

  PerformLSE();

  ASSERT_FALSE(IsRemoved(store1));
  ASSERT_FALSE(IsRemoved(store2));
  ASSERT_FALSE(IsRemoved(store3));
}

// As it is not allowed to use defaults for VecLoads, check if there is a new created array
// a VecLoad used in a loop and after it is not replaced with a default.
TEST_F(LoadStoreEliminationTest, VLoadDefaultValueInLoopWithoutWriteSideEffects) {
  InitGraph();
  CreateTestControlFlowGraph();

  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c128 = graph_->GetIntConstant(128);

  HInstruction* array_a = new (GetAllocator()) HNewArray(c0, c128, 0, 0);
  pre_header_->InsertInstructionBefore(array_a, pre_header_->GetLastInstruction());
  array_a->CopyEnvironmentFrom(suspend_check_->GetEnvironment());

  // LOOP BODY:
  //    v = a[i,... i + 3]
  // array[0,... 3] = v
  HInstruction* vload = AddVecLoad(loop_, array_a, phi_);
  HInstruction* vstore = AddVecStore(return_block_, array_, c0, vload->AsVecLoad());

  PerformLSE();

  ASSERT_FALSE(IsRemoved(vload));
  ASSERT_FALSE(IsRemoved(vstore));
}

// As it is not allowed to use defaults for VecLoads, check if there is a new created array
// a VecLoad is not replaced with a default.
TEST_F(LoadStoreEliminationTest, VLoadDefaultValue) {
  InitGraph();
  CreateTestControlFlowGraph();

  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c128 = graph_->GetIntConstant(128);

  HInstruction* array_a = new (GetAllocator()) HNewArray(c0, c128, 0, 0);
  pre_header_->InsertInstructionBefore(array_a, pre_header_->GetLastInstruction());
  array_a->CopyEnvironmentFrom(suspend_check_->GetEnvironment());

  // v = a[0,... 3]
  // array[0,... 3] = v
  HInstruction* vload = AddVecLoad(pre_header_, array_a, c0);
  HInstruction* vstore = AddVecStore(return_block_, array_, c0, vload->AsVecLoad());

  PerformLSE();

  ASSERT_FALSE(IsRemoved(vload));
  ASSERT_FALSE(IsRemoved(vstore));
}

// As it is allowed to use defaults for ordinary loads, check if there is a new created array
// a load used in a loop and after it is replaced with a default.
TEST_F(LoadStoreEliminationTest, LoadDefaultValueInLoopWithoutWriteSideEffects) {
  InitGraph();
  CreateTestControlFlowGraph();

  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c128 = graph_->GetIntConstant(128);

  HInstruction* array_a = new (GetAllocator()) HNewArray(c0, c128, 0, 0);
  pre_header_->InsertInstructionBefore(array_a, pre_header_->GetLastInstruction());
  array_a->CopyEnvironmentFrom(suspend_check_->GetEnvironment());

  // LOOP BODY:
  //    v = a[i]
  // array[0] = v
  HInstruction* load = AddArrayGet(loop_, array_a, phi_);
  HInstruction* store = AddArraySet(return_block_, array_, c0, load);

  PerformLSE();

  ASSERT_TRUE(IsRemoved(load));
  ASSERT_FALSE(IsRemoved(store));
}

// As it is allowed to use defaults for ordinary loads, check if there is a new created array
// a load is replaced with a default.
TEST_F(LoadStoreEliminationTest, LoadDefaultValue) {
  InitGraph();
  CreateTestControlFlowGraph();

  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c128 = graph_->GetIntConstant(128);

  HInstruction* array_a = new (GetAllocator()) HNewArray(c0, c128, 0, 0);
  pre_header_->InsertInstructionBefore(array_a, pre_header_->GetLastInstruction());
  array_a->CopyEnvironmentFrom(suspend_check_->GetEnvironment());

  // v = a[0]
  // array[0] = v
  HInstruction* load = AddArrayGet(pre_header_, array_a, c0);
  HInstruction* store = AddArraySet(return_block_, array_, c0, load);

  PerformLSE();

  ASSERT_TRUE(IsRemoved(load));
  ASSERT_FALSE(IsRemoved(store));
}

// As it is not allowed to use defaults for VecLoads but allowed for regular loads,
// check if there is a new created array, a VecLoad and a load used in a loop and after it,
// VecLoad is not replaced with a default but the load is.
TEST_F(LoadStoreEliminationTest, VLoadAndLoadDefaultValueInLoopWithoutWriteSideEffects) {
  InitGraph();
  CreateTestControlFlowGraph();

  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c128 = graph_->GetIntConstant(128);

  HInstruction* array_a = new (GetAllocator()) HNewArray(c0, c128, 0, 0);
  pre_header_->InsertInstructionBefore(array_a, pre_header_->GetLastInstruction());
  array_a->CopyEnvironmentFrom(suspend_check_->GetEnvironment());

  // LOOP BODY:
  //    v = a[i,... i + 3]
  //    v1 = a[i]
  // array[0,... 3] = v
  // array[0] = v1
  HInstruction* vload = AddVecLoad(loop_, array_a, phi_);
  HInstruction* load = AddArrayGet(loop_, array_a, phi_);
  HInstruction* vstore = AddVecStore(return_block_, array_, c0, vload->AsVecLoad());
  HInstruction* store = AddArraySet(return_block_, array_, c0, load);

  PerformLSE();

  ASSERT_FALSE(IsRemoved(vload));
  ASSERT_TRUE(IsRemoved(load));
  ASSERT_FALSE(IsRemoved(vstore));
  ASSERT_FALSE(IsRemoved(store));
}

// As it is not allowed to use defaults for VecLoads but allowed for regular loads,
// check if there is a new created array, a VecLoad and a load,
// VecLoad is not replaced with a default but the load is.
TEST_F(LoadStoreEliminationTest, VLoadAndLoadDefaultValue) {
  InitGraph();
  CreateTestControlFlowGraph();

  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c128 = graph_->GetIntConstant(128);

  HInstruction* array_a = new (GetAllocator()) HNewArray(c0, c128, 0, 0);
  pre_header_->InsertInstructionBefore(array_a, pre_header_->GetLastInstruction());
  array_a->CopyEnvironmentFrom(suspend_check_->GetEnvironment());

  // v = a[0,... 3]
  // v1 = a[0]
  // array[0,... 3] = v
  // array[0] = v1
  HInstruction* vload = AddVecLoad(pre_header_, array_a, c0);
  HInstruction* load = AddArrayGet(pre_header_, array_a, c0);
  HInstruction* vstore = AddVecStore(return_block_, array_, c0, vload->AsVecLoad());
  HInstruction* store = AddArraySet(return_block_, array_, c0, load);

  PerformLSE();

  ASSERT_FALSE(IsRemoved(vload));
  ASSERT_TRUE(IsRemoved(load));
  ASSERT_FALSE(IsRemoved(vstore));
  ASSERT_FALSE(IsRemoved(store));
}

// It is not allowed to use defaults for VecLoads. However it should not prevent from removing
// loads getting the same value.
// Check a load getting a known value is eliminated (a loop test case).
TEST_F(LoadStoreEliminationTest, VLoadDefaultValueAndVLoadInLoopWithoutWriteSideEffects) {
  InitGraph();
  CreateTestControlFlowGraph();

  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c128 = graph_->GetIntConstant(128);

  HInstruction* array_a = new (GetAllocator()) HNewArray(c0, c128, 0, 0);
  pre_header_->InsertInstructionBefore(array_a, pre_header_->GetLastInstruction());
  array_a->CopyEnvironmentFrom(suspend_check_->GetEnvironment());

  // LOOP BODY:
  //    v = a[i,... i + 3]
  //    v1 = a[i,... i + 3]
  // array[0,... 3] = v
  // array[128,... 131] = v1
  HInstruction* vload1 = AddVecLoad(loop_, array_a, phi_);
  HInstruction* vload2 = AddVecLoad(loop_, array_a, phi_);
  HInstruction* vstore1 = AddVecStore(return_block_, array_, c0, vload1->AsVecLoad());
  HInstruction* vstore2 = AddVecStore(return_block_, array_, c128, vload2->AsVecLoad());

  PerformLSE();

  ASSERT_FALSE(IsRemoved(vload1));
  ASSERT_TRUE(IsRemoved(vload2));
  ASSERT_FALSE(IsRemoved(vstore1));
  ASSERT_FALSE(IsRemoved(vstore2));
}

// It is not allowed to use defaults for VecLoads. However it should not prevent from removing
// loads getting the same value.
// Check a load getting a known value is eliminated.
TEST_F(LoadStoreEliminationTest, VLoadDefaultValueAndVLoad) {
  InitGraph();
  CreateTestControlFlowGraph();

  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c128 = graph_->GetIntConstant(128);

  HInstruction* array_a = new (GetAllocator()) HNewArray(c0, c128, 0, 0);
  pre_header_->InsertInstructionBefore(array_a, pre_header_->GetLastInstruction());
  array_a->CopyEnvironmentFrom(suspend_check_->GetEnvironment());

  // v = a[0,... 3]
  // v1 = a[0,... 3]
  // array[0,... 3] = v
  // array[128,... 131] = v1
  HInstruction* vload1 = AddVecLoad(pre_header_, array_a, c0);
  HInstruction* vload2 = AddVecLoad(pre_header_, array_a, c0);
  HInstruction* vstore1 = AddVecStore(return_block_, array_, c0, vload1->AsVecLoad());
  HInstruction* vstore2 = AddVecStore(return_block_, array_, c128, vload2->AsVecLoad());

  PerformLSE();

  ASSERT_FALSE(IsRemoved(vload1));
  ASSERT_TRUE(IsRemoved(vload2));
  ASSERT_FALSE(IsRemoved(vstore1));
  ASSERT_FALSE(IsRemoved(vstore2));
}

// void DO_CAL() {
//   int i = 1;
//   int[] w = new int[80];
//   int t = 0;
//   while (i < 80) {
//     w[i] = PLEASE_INTERLEAVE(w[i - 1], 1)
//     t = PLEASE_SELECT(w[i], t);
//     i++;
//   }
//   return t;
// }
TEST_F(LoadStoreEliminationTest, ArrayLoopOverlap) {
  CreateGraph();
  AdjacencyListGraph blocks(graph_,
                            GetAllocator(),
                            "entry",
                            "exit",
                            { { "entry", "loop_pre_header" },
                              { "loop_pre_header", "loop_entry" },
                              { "loop_entry", "loop_body" },
                              { "loop_entry", "loop_post" },
                              { "loop_body", "loop_entry" },
                              { "loop_post", "exit" } });
#define GET_BLOCK(name) HBasicBlock* name = blocks.Get(#name)
  GET_BLOCK(entry);
  GET_BLOCK(loop_pre_header);
  GET_BLOCK(loop_entry);
  GET_BLOCK(loop_body);
  GET_BLOCK(loop_post);
  GET_BLOCK(exit);
#undef GET_BLOCK

  HInstruction* zero_const = graph_->GetConstant(DataType::Type::kInt32, 0);
  HInstruction* one_const = graph_->GetConstant(DataType::Type::kInt32, 1);
  HInstruction* eighty_const = graph_->GetConstant(DataType::Type::kInt32, 80);
  HInstruction* entry_goto = new (GetAllocator()) HGoto();
  entry->AddInstruction(entry_goto);

  HInstruction* alloc_w = new (GetAllocator()) HNewArray(zero_const, eighty_const, 0, 0);
  HInstruction* pre_header_goto = new (GetAllocator()) HGoto();
  loop_pre_header->AddInstruction(alloc_w);
  loop_pre_header->AddInstruction(pre_header_goto);
  // environment
  ArenaVector<HInstruction*> alloc_locals({}, GetAllocator()->Adapter(kArenaAllocInstruction));
  ManuallyBuildEnvFor(alloc_w, &alloc_locals);

  // loop-start
  HPhi* i_phi = new (GetAllocator()) HPhi(GetAllocator(), 0, 0, DataType::Type::kInt32);
  HPhi* t_phi = new (GetAllocator()) HPhi(GetAllocator(), 1, 0, DataType::Type::kInt32);
  HInstruction* suspend = new (GetAllocator()) HSuspendCheck();
  HInstruction* i_cmp_top = new (GetAllocator()) HGreaterThanOrEqual(i_phi, eighty_const);
  HInstruction* loop_start_branch = new (GetAllocator()) HIf(i_cmp_top);
  loop_entry->AddPhi(i_phi);
  loop_entry->AddPhi(t_phi);
  loop_entry->AddInstruction(suspend);
  loop_entry->AddInstruction(i_cmp_top);
  loop_entry->AddInstruction(loop_start_branch);
  CHECK_EQ(loop_entry->GetSuccessors().size(), 2u);
  if (loop_entry->GetNormalSuccessors()[1] != loop_body) {
    loop_entry->SwapSuccessors();
  }
  CHECK_EQ(loop_entry->GetPredecessors().size(), 2u);
  if (loop_entry->GetPredecessors()[0] != loop_pre_header) {
    loop_entry->SwapPredecessors();
  }
  i_phi->AddInput(one_const);
  t_phi->AddInput(zero_const);

  // environment
  ArenaVector<HInstruction*> suspend_locals({ alloc_w, i_phi, t_phi },
                                            GetAllocator()->Adapter(kArenaAllocInstruction));
  ManuallyBuildEnvFor(suspend, &suspend_locals);

  // BODY
  HInstruction* last_i = new (GetAllocator()) HSub(DataType::Type::kInt32, i_phi, one_const);
  HInstruction* last_get =
      new (GetAllocator()) HArrayGet(alloc_w, last_i, DataType::Type::kInt32, 0);
  HInvoke* body_value = new (GetAllocator())
      HInvokeStaticOrDirect(GetAllocator(),
                            2,
                            DataType::Type::kInt32,
                            0,
                            { nullptr, 0 },
                            nullptr,
                            {},
                            InvokeType::kStatic,
                            { nullptr, 0 },
                            HInvokeStaticOrDirect::ClinitCheckRequirement::kNone);
  body_value->SetRawInputAt(0, last_get);
  body_value->SetRawInputAt(1, one_const);
  HInstruction* body_set =
      new (GetAllocator()) HArraySet(alloc_w, i_phi, body_value, DataType::Type::kInt32, 0);
  HInstruction* body_get =
      new (GetAllocator()) HArrayGet(alloc_w, i_phi, DataType::Type::kInt32, 0);
  HInvoke* t_next = new (GetAllocator())
      HInvokeStaticOrDirect(GetAllocator(),
                            2,
                            DataType::Type::kInt32,
                            0,
                            { nullptr, 0 },
                            nullptr,
                            {},
                            InvokeType::kStatic,
                            { nullptr, 0 },
                            HInvokeStaticOrDirect::ClinitCheckRequirement::kNone);
  t_next->SetRawInputAt(0, body_get);
  t_next->SetRawInputAt(1, t_phi);
  HInstruction* i_next = new (GetAllocator()) HAdd(DataType::Type::kInt32, i_phi, one_const);
  HInstruction* body_goto = new (GetAllocator()) HGoto();
  loop_body->AddInstruction(last_i);
  loop_body->AddInstruction(last_get);
  loop_body->AddInstruction(body_value);
  loop_body->AddInstruction(body_set);
  loop_body->AddInstruction(body_get);
  loop_body->AddInstruction(t_next);
  loop_body->AddInstruction(i_next);
  loop_body->AddInstruction(body_goto);
  body_value->CopyEnvironmentFrom(suspend->GetEnvironment());

  i_phi->AddInput(i_next);
  t_phi->AddInput(t_next);
  t_next->CopyEnvironmentFrom(suspend->GetEnvironment());

  // loop-post
  HInstruction* return_inst = new (GetAllocator()) HReturn(t_phi);
  loop_post->AddInstruction(return_inst);

  // exit
  HInstruction* exit_inst = new (GetAllocator()) HExit();
  exit->AddInstruction(exit_inst);

  graph_->ClearDominanceInformation();
  graph_->ClearLoopInformation();
  PerformLSE();

  // TODO Technically this is optimizable. LSE just needs to add phis to keep
  // track of the last `N` values set where `N` is how many locations we can go
  // back into the array.
  if (IsRemoved(last_get)) {
    // If we were able to remove the previous read the entire array should be removable.
    EXPECT_TRUE(IsRemoved(body_set));
    EXPECT_TRUE(IsRemoved(alloc_w));
  } else {
    // This is the branch we actually take for now. If we rely on being able to
    // read the array we'd better remember to write to it as well.
    EXPECT_FALSE(IsRemoved(body_set));
  }
  // The last 'get' should always be removable.
  EXPECT_TRUE(IsRemoved(body_get));
}


// void DO_CAL2() {
//   int i = 1;
//   int[] w = new int[80];
//   int t = 0;
//   while (i < 80) {
//     w[i] = PLEASE_INTERLEAVE(w[i - 1], 1) // <-- removed
//     t = PLEASE_SELECT(w[i], t);
//     w[i] = PLEASE_INTERLEAVE(w[i - 1], 1) // <-- removed
//     t = PLEASE_SELECT(w[i], t);
//     w[i] = PLEASE_INTERLEAVE(w[i - 1], 1) // <-- kept
//     t = PLEASE_SELECT(w[i], t);
//     i++;
//   }
//   return t;
// }
TEST_F(LoadStoreEliminationTest, ArrayLoopOverlap2) {
  CreateGraph();
  AdjacencyListGraph blocks(graph_,
                            GetAllocator(),
                            "entry",
                            "exit",
                            { { "entry", "loop_pre_header" },
                              { "loop_pre_header", "loop_entry" },
                              { "loop_entry", "loop_body" },
                              { "loop_entry", "loop_post" },
                              { "loop_body", "loop_entry" },
                              { "loop_post", "exit" } });
#define GET_BLOCK(name) HBasicBlock* name = blocks.Get(#name)
  GET_BLOCK(entry);
  GET_BLOCK(loop_pre_header);
  GET_BLOCK(loop_entry);
  GET_BLOCK(loop_body);
  GET_BLOCK(loop_post);
  GET_BLOCK(exit);
#undef GET_BLOCK

  HInstruction* zero_const = graph_->GetConstant(DataType::Type::kInt32, 0);
  HInstruction* one_const = graph_->GetConstant(DataType::Type::kInt32, 1);
  HInstruction* eighty_const = graph_->GetConstant(DataType::Type::kInt32, 80);
  HInstruction* entry_goto = new (GetAllocator()) HGoto();
  entry->AddInstruction(entry_goto);

  HInstruction* alloc_w = new (GetAllocator()) HNewArray(zero_const, eighty_const, 0, 0);
  HInstruction* pre_header_goto = new (GetAllocator()) HGoto();
  loop_pre_header->AddInstruction(alloc_w);
  loop_pre_header->AddInstruction(pre_header_goto);
  // environment
  ArenaVector<HInstruction*> alloc_locals({}, GetAllocator()->Adapter(kArenaAllocInstruction));
  ManuallyBuildEnvFor(alloc_w, &alloc_locals);

  // loop-start
  HPhi* i_phi = new (GetAllocator()) HPhi(GetAllocator(), 0, 0, DataType::Type::kInt32);
  HPhi* t_phi = new (GetAllocator()) HPhi(GetAllocator(), 1, 0, DataType::Type::kInt32);
  HInstruction* suspend = new (GetAllocator()) HSuspendCheck();
  HInstruction* i_cmp_top = new (GetAllocator()) HGreaterThanOrEqual(i_phi, eighty_const);
  HInstruction* loop_start_branch = new (GetAllocator()) HIf(i_cmp_top);
  loop_entry->AddPhi(i_phi);
  loop_entry->AddPhi(t_phi);
  loop_entry->AddInstruction(suspend);
  loop_entry->AddInstruction(i_cmp_top);
  loop_entry->AddInstruction(loop_start_branch);
  CHECK_EQ(loop_entry->GetSuccessors().size(), 2u);
  if (loop_entry->GetNormalSuccessors()[1] != loop_body) {
    loop_entry->SwapSuccessors();
  }
  CHECK_EQ(loop_entry->GetPredecessors().size(), 2u);
  if (loop_entry->GetPredecessors()[0] != loop_pre_header) {
    loop_entry->SwapPredecessors();
  }
  i_phi->AddInput(one_const);
  t_phi->AddInput(zero_const);

  // environment
  ArenaVector<HInstruction*> suspend_locals({ alloc_w, i_phi, t_phi },
                                            GetAllocator()->Adapter(kArenaAllocInstruction));
  ManuallyBuildEnvFor(suspend, &suspend_locals);

  // BODY
  HInstruction* last_i = new (GetAllocator()) HSub(DataType::Type::kInt32, i_phi, one_const);
  HInstruction* last_get_1, *last_get_2, *last_get_3;
  HInstruction* body_value_1, *body_value_2, *body_value_3;
  HInstruction* body_set_1, *body_set_2, *body_set_3;
  HInstruction* body_get_1, *body_get_2, *body_get_3;
  HInstruction* t_next_1, *t_next_2, *t_next_3;
  auto make_instructions = [&](HInstruction* last_t_value) {
    HInstruction* last_get =
        new (GetAllocator()) HArrayGet(alloc_w, last_i, DataType::Type::kInt32, 0);
    HInvoke* body_value = new (GetAllocator())
        HInvokeStaticOrDirect(GetAllocator(),
                              2,
                              DataType::Type::kInt32,
                              0,
                              { nullptr, 0 },
                              nullptr,
                              {},
                              InvokeType::kStatic,
                              { nullptr, 0 },
                              HInvokeStaticOrDirect::ClinitCheckRequirement::kNone);
    body_value->SetRawInputAt(0, last_get);
    body_value->SetRawInputAt(1, one_const);
    HInstruction* body_set =
        new (GetAllocator()) HArraySet(alloc_w, i_phi, body_value, DataType::Type::kInt32, 0);
    HInstruction* body_get =
        new (GetAllocator()) HArrayGet(alloc_w, i_phi, DataType::Type::kInt32, 0);
    HInvoke* t_next = new (GetAllocator())
        HInvokeStaticOrDirect(GetAllocator(),
                              2,
                              DataType::Type::kInt32,
                              0,
                              { nullptr, 0 },
                              nullptr,
                              {},
                              InvokeType::kStatic,
                              { nullptr, 0 },
                              HInvokeStaticOrDirect::ClinitCheckRequirement::kNone);
    t_next->SetRawInputAt(0, body_get);
    t_next->SetRawInputAt(1, last_t_value);
    loop_body->AddInstruction(last_get);
    loop_body->AddInstruction(body_value);
    loop_body->AddInstruction(body_set);
    loop_body->AddInstruction(body_get);
    loop_body->AddInstruction(t_next);
    return std::make_tuple(last_get, body_value, body_set, body_get, t_next);
  };
  std::tie(last_get_1, body_value_1, body_set_1, body_get_1, t_next_1) = make_instructions(t_phi);
  std::tie(last_get_2, body_value_2, body_set_2, body_get_2, t_next_2) =
      make_instructions(t_next_1);
  std::tie(last_get_3, body_value_3, body_set_3, body_get_3, t_next_3) =
      make_instructions(t_next_2);
  HInstruction* i_next = new (GetAllocator()) HAdd(DataType::Type::kInt32, i_phi, one_const);
  HInstruction* body_goto = new (GetAllocator()) HGoto();
  loop_body->InsertInstructionBefore(last_i, last_get_1);
  loop_body->AddInstruction(i_next);
  loop_body->AddInstruction(body_goto);
  body_value_1->CopyEnvironmentFrom(suspend->GetEnvironment());
  body_value_2->CopyEnvironmentFrom(suspend->GetEnvironment());
  body_value_3->CopyEnvironmentFrom(suspend->GetEnvironment());

  i_phi->AddInput(i_next);
  t_phi->AddInput(t_next_3);
  t_next_1->CopyEnvironmentFrom(suspend->GetEnvironment());
  t_next_2->CopyEnvironmentFrom(suspend->GetEnvironment());
  t_next_3->CopyEnvironmentFrom(suspend->GetEnvironment());

  // loop-post
  HInstruction* return_inst = new (GetAllocator()) HReturn(t_phi);
  loop_post->AddInstruction(return_inst);

  // exit
  HInstruction* exit_inst = new (GetAllocator()) HExit();
  exit->AddInstruction(exit_inst);

  graph_->ClearDominanceInformation();
  graph_->ClearLoopInformation();
  PerformLSE();

  // TODO Technically this is optimizable. LSE just needs to add phis to keep
  // track of the last `N` values set where `N` is how many locations we can go
  // back into the array.
  if (IsRemoved(last_get_1)) {
    // If we were able to remove the previous read the entire array should be removable.
    EXPECT_TRUE(IsRemoved(body_set_1));
    EXPECT_TRUE(IsRemoved(body_set_2));
    EXPECT_TRUE(IsRemoved(body_set_3));
    EXPECT_TRUE(IsRemoved(last_get_1));
    EXPECT_TRUE(IsRemoved(last_get_2));
    EXPECT_TRUE(IsRemoved(alloc_w));
  } else {
    // This is the branch we actually take for now. If we rely on being able to
    // read the array we'd better remember to write to it as well.
    EXPECT_FALSE(IsRemoved(body_set_3));
  }
  // The last 'get' should always be removable.
  EXPECT_TRUE(IsRemoved(body_get_1));
  EXPECT_TRUE(IsRemoved(body_get_2));
  EXPECT_TRUE(IsRemoved(body_get_3));
  // shadowed writes should always be removed
  EXPECT_TRUE(IsRemoved(body_set_1));
  EXPECT_TRUE(IsRemoved(body_set_2));
}

TEST_F(LoadStoreEliminationTest, ArrayNonLoopPhi) {
  CreateGraph();
  AdjacencyListGraph blocks(graph_,
                            GetAllocator(),
                            "entry",
                            "exit",
                            { { "entry", "start" },
                              { "start", "left" },
                              { "start", "right" },
                              { "left", "ret" },
                              { "right", "ret" },
                              { "ret", "exit" } });
#define GET_BLOCK(name) HBasicBlock* name = blocks.Get(#name)
  GET_BLOCK(entry);
  GET_BLOCK(start);
  GET_BLOCK(left);
  GET_BLOCK(right);
  GET_BLOCK(ret);
  GET_BLOCK(exit);
#undef GET_BLOCK

  HInstruction* zero_const = graph_->GetConstant(DataType::Type::kInt32, 0);
  HInstruction* one_const = graph_->GetConstant(DataType::Type::kInt32, 1);
  HInstruction* two_const = graph_->GetConstant(DataType::Type::kInt32, 2);
  HInstruction* param = new (GetAllocator())
      HParameterValue(graph_->GetDexFile(), dex::TypeIndex(1), 0, DataType::Type::kBool);
  HInstruction* entry_goto = new (GetAllocator()) HGoto();
  entry->AddInstruction(param);
  entry->AddInstruction(entry_goto);

  HInstruction* alloc_w = new (GetAllocator()) HNewArray(zero_const, two_const, 0, 0);
  HInstruction* branch = new (GetAllocator()) HIf(param);
  start->AddInstruction(alloc_w);
  start->AddInstruction(branch);
  // environment
  ArenaVector<HInstruction*> alloc_locals({}, GetAllocator()->Adapter(kArenaAllocInstruction));
  ManuallyBuildEnvFor(alloc_w, &alloc_locals);

  // left
  HInvoke* left_value = new (GetAllocator())
      HInvokeStaticOrDirect(GetAllocator(),
                            1,
                            DataType::Type::kInt32,
                            0,
                            { nullptr, 0 },
                            nullptr,
                            {},
                            InvokeType::kStatic,
                            { nullptr, 0 },
                            HInvokeStaticOrDirect::ClinitCheckRequirement::kNone);
  left_value->SetRawInputAt(0, zero_const);
  HInstruction* left_set_1 =
      new (GetAllocator()) HArraySet(alloc_w, zero_const, left_value, DataType::Type::kInt32, 0);
  HInstruction* left_set_2 =
      new (GetAllocator()) HArraySet(alloc_w, one_const, zero_const, DataType::Type::kInt32, 0);
  HInstruction* left_goto = new (GetAllocator()) HGoto();
  left->AddInstruction(left_value);
  left->AddInstruction(left_set_1);
  left->AddInstruction(left_set_2);
  left->AddInstruction(left_goto);
  ArenaVector<HInstruction*> left_locals({ alloc_w },
                                         GetAllocator()->Adapter(kArenaAllocInstruction));
  ManuallyBuildEnvFor(left_value, &alloc_locals);

  // right
  HInvoke* right_value = new (GetAllocator())
      HInvokeStaticOrDirect(GetAllocator(),
                            1,
                            DataType::Type::kInt32,
                            0,
                            { nullptr, 0 },
                            nullptr,
                            {},
                            InvokeType::kStatic,
                            { nullptr, 0 },
                            HInvokeStaticOrDirect::ClinitCheckRequirement::kNone);
  right_value->SetRawInputAt(0, one_const);
  HInstruction* right_set_1 =
      new (GetAllocator()) HArraySet(alloc_w, zero_const, right_value, DataType::Type::kInt32, 0);
  HInstruction* right_set_2 =
      new (GetAllocator()) HArraySet(alloc_w, one_const, zero_const, DataType::Type::kInt32, 0);
  HInstruction* right_goto = new (GetAllocator()) HGoto();
  right->AddInstruction(right_value);
  right->AddInstruction(right_set_1);
  right->AddInstruction(right_set_2);
  right->AddInstruction(right_goto);
  ArenaVector<HInstruction*> right_locals({ alloc_w },
                                          GetAllocator()->Adapter(kArenaAllocInstruction));
  ManuallyBuildEnvFor(right_value, &alloc_locals);

  // ret
  HInstruction* read_1 =
      new (GetAllocator()) HArrayGet(alloc_w, zero_const, DataType::Type::kInt32, 0);
  HInstruction* read_2 =
      new (GetAllocator()) HArrayGet(alloc_w, one_const, DataType::Type::kInt32, 0);
  HInstruction* add = new (GetAllocator()) HAdd(DataType::Type::kInt32, read_1, read_2);
  HInstruction* return_inst = new (GetAllocator()) HReturn(add);
  ret->AddInstruction(read_1);
  ret->AddInstruction(read_2);
  ret->AddInstruction(add);
  ret->AddInstruction(return_inst);

  // exit
  HInstruction* exit_inst = new (GetAllocator()) HExit();
  exit->AddInstruction(exit_inst);

  graph_->ClearDominanceInformation();
  graph_->ClearLoopInformation();
  PerformLSE();

  EXPECT_TRUE(IsRemoved(read_1));
  EXPECT_TRUE(IsRemoved(read_2));
  EXPECT_TRUE(IsRemoved(left_set_1));
  EXPECT_TRUE(IsRemoved(left_set_2));
  EXPECT_TRUE(IsRemoved(right_set_1));
  EXPECT_TRUE(IsRemoved(right_set_2));
  EXPECT_TRUE(IsRemoved(alloc_w));

  EXPECT_FALSE(IsRemoved(left_value));
  EXPECT_FALSE(IsRemoved(right_value));
}

TEST_F(LoadStoreEliminationTest, ArrayMergeDefault) {
  CreateGraph();
  AdjacencyListGraph blocks(graph_,
                            GetAllocator(),
                            "entry",
                            "exit",
                            { { "entry", "start" },
                              { "start", "left" },
                              { "start", "right" },
                              { "left", "ret" },
                              { "right", "ret" },
                              { "ret", "exit" } });
#define GET_BLOCK(name) HBasicBlock* name = blocks.Get(#name)
  GET_BLOCK(entry);
  GET_BLOCK(start);
  GET_BLOCK(left);
  GET_BLOCK(right);
  GET_BLOCK(ret);
  GET_BLOCK(exit);
#undef GET_BLOCK

  HInstruction* zero_const = graph_->GetConstant(DataType::Type::kInt32, 0);
  HInstruction* one_const = graph_->GetConstant(DataType::Type::kInt32, 1);
  HInstruction* two_const = graph_->GetConstant(DataType::Type::kInt32, 2);
  HInstruction* param = new (GetAllocator())
      HParameterValue(graph_->GetDexFile(), dex::TypeIndex(1), 0, DataType::Type::kBool);
  HInstruction* entry_goto = new (GetAllocator()) HGoto();
  entry->AddInstruction(param);
  entry->AddInstruction(entry_goto);

  HInstruction* alloc_w = new (GetAllocator()) HNewArray(zero_const, two_const, 0, 0);
  HInstruction* branch = new (GetAllocator()) HIf(param);
  start->AddInstruction(alloc_w);
  start->AddInstruction(branch);
  // environment
  ArenaVector<HInstruction*> alloc_locals({}, GetAllocator()->Adapter(kArenaAllocInstruction));
  ManuallyBuildEnvFor(alloc_w, &alloc_locals);

  // left
  HInstruction* left_set_1 =
      new (GetAllocator()) HArraySet(alloc_w, zero_const, one_const, DataType::Type::kInt32, 0);
  HInstruction* left_set_2 =
      new (GetAllocator()) HArraySet(alloc_w, zero_const, zero_const, DataType::Type::kInt32, 0);
  HInstruction* left_goto = new (GetAllocator()) HGoto();
  left->AddInstruction(left_set_1);
  left->AddInstruction(left_set_2);
  left->AddInstruction(left_goto);

  // right
  HInstruction* right_set_1 =
      new (GetAllocator()) HArraySet(alloc_w, one_const, one_const, DataType::Type::kInt32, 0);
  HInstruction* right_set_2 =
      new (GetAllocator()) HArraySet(alloc_w, one_const, zero_const, DataType::Type::kInt32, 0);
  HInstruction* right_goto = new (GetAllocator()) HGoto();
  right->AddInstruction(right_set_1);
  right->AddInstruction(right_set_2);
  right->AddInstruction(right_goto);

  // ret
  HInstruction* read_1 =
      new (GetAllocator()) HArrayGet(alloc_w, zero_const, DataType::Type::kInt32, 0);
  HInstruction* read_2 =
      new (GetAllocator()) HArrayGet(alloc_w, one_const, DataType::Type::kInt32, 0);
  HInstruction* add = new (GetAllocator()) HAdd(DataType::Type::kInt32, read_1, read_2);
  HInstruction* return_inst = new (GetAllocator()) HReturn(add);
  ret->AddInstruction(read_1);
  ret->AddInstruction(read_2);
  ret->AddInstruction(add);
  ret->AddInstruction(return_inst);

  // exit
  HInstruction* exit_inst = new (GetAllocator()) HExit();
  exit->AddInstruction(exit_inst);

  graph_->ClearDominanceInformation();
  graph_->ClearLoopInformation();
  PerformLSE();

  EXPECT_TRUE(IsRemoved(read_1));
  EXPECT_TRUE(IsRemoved(read_2));
  EXPECT_TRUE(IsRemoved(left_set_1));
  EXPECT_TRUE(IsRemoved(left_set_2));
  EXPECT_TRUE(IsRemoved(right_set_1));
  EXPECT_TRUE(IsRemoved(right_set_2));
  EXPECT_TRUE(IsRemoved(alloc_w));
}

}  // namespace art
