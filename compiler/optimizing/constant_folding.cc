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

#include "constant_folding.h"

#include <algorithm>

#include "optimizing/data_type.h"
#include "optimizing/nodes.h"

namespace art {

// This visitor tries to simplify instructions that can be evaluated
// as constants.
class HConstantFoldingVisitor : public HGraphDelegateVisitor {
 public:
  explicit HConstantFoldingVisitor(HGraph* graph, OptimizingCompilerStats* stats)
      : HGraphDelegateVisitor(graph, stats) {}

 private:
  void VisitBasicBlock(HBasicBlock* block) override;

  void VisitUnaryOperation(HUnaryOperation* inst) override;
  void VisitBinaryOperation(HBinaryOperation* inst) override;

  void VisitTypeConversion(HTypeConversion* inst) override;
  void VisitDivZeroCheck(HDivZeroCheck* inst) override;
  void VisitIf(HIf* inst) override;

  void PropagateValue(HBasicBlock* starting_block, HInstruction* variable, HConstant* constant);

  DISALLOW_COPY_AND_ASSIGN(HConstantFoldingVisitor);
};

// This visitor tries to simplify operations with an absorbing input,
// yielding a constant. For example `input * 0` is replaced by a
// null constant.
class InstructionWithAbsorbingInputSimplifier : public HGraphVisitor {
 public:
  explicit InstructionWithAbsorbingInputSimplifier(HGraph* graph) : HGraphVisitor(graph) {}

 private:
  void VisitShift(HBinaryOperation* shift);

  void VisitEqual(HEqual* instruction) override;
  void VisitNotEqual(HNotEqual* instruction) override;

  void VisitAbove(HAbove* instruction) override;
  void VisitAboveOrEqual(HAboveOrEqual* instruction) override;
  void VisitBelow(HBelow* instruction) override;
  void VisitBelowOrEqual(HBelowOrEqual* instruction) override;

  void VisitAnd(HAnd* instruction) override;
  void VisitCompare(HCompare* instruction) override;
  void VisitMul(HMul* instruction) override;
  void VisitOr(HOr* instruction) override;
  void VisitRem(HRem* instruction) override;
  void VisitShl(HShl* instruction) override;
  void VisitShr(HShr* instruction) override;
  void VisitSub(HSub* instruction) override;
  void VisitUShr(HUShr* instruction) override;
  void VisitXor(HXor* instruction) override;
};


bool HConstantFolding::Run() {
  HConstantFoldingVisitor visitor(graph_, stats_);
  // Process basic blocks in reverse post-order in the dominator tree,
  // so that an instruction turned into a constant, used as input of
  // another instruction, may possibly be used to turn that second
  // instruction into a constant as well.
  visitor.VisitReversePostOrder();
  return true;
}


void HConstantFoldingVisitor::VisitBasicBlock(HBasicBlock* block) {
  // Traverse this block's instructions (phis don't need to be
  // processed) in (forward) order and replace the ones that can be
  // statically evaluated by a compile-time counterpart.
  for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
    it.Current()->Accept(this);
  }
}

void HConstantFoldingVisitor::VisitUnaryOperation(HUnaryOperation* inst) {
  // Constant folding: replace `op(a)' with a constant at compile
  // time if `a' is a constant.
  HConstant* constant = inst->TryStaticEvaluation();
  if (constant != nullptr) {
    inst->ReplaceWith(constant);
    inst->GetBlock()->RemoveInstruction(inst);
  }
}

void HConstantFoldingVisitor::VisitBinaryOperation(HBinaryOperation* inst) {
  // Constant folding: replace `op(a, b)' with a constant at
  // compile time if `a' and `b' are both constants.
  HConstant* constant = inst->TryStaticEvaluation();
  if (constant != nullptr) {
    inst->ReplaceWith(constant);
    inst->GetBlock()->RemoveInstruction(inst);
  } else {
    InstructionWithAbsorbingInputSimplifier simplifier(GetGraph());
    inst->Accept(&simplifier);
  }
}

void HConstantFoldingVisitor::VisitTypeConversion(HTypeConversion* inst) {
  // Constant folding: replace `TypeConversion(a)' with a constant at
  // compile time if `a' is a constant.
  HConstant* constant = inst->TryStaticEvaluation();
  if (constant != nullptr) {
    inst->ReplaceWith(constant);
    inst->GetBlock()->RemoveInstruction(inst);
  }
}

void HConstantFoldingVisitor::VisitDivZeroCheck(HDivZeroCheck* inst) {
  // We can safely remove the check if the input is a non-null constant.
  HInstruction* check_input = inst->InputAt(0);
  if (check_input->IsConstant() && !check_input->AsConstant()->IsArithmeticZero()) {
    inst->ReplaceWith(check_input);
    inst->GetBlock()->RemoveInstruction(inst);
  }
}

void HConstantFoldingVisitor::PropagateValue(HBasicBlock* starting_block,
                                             HInstruction* variable,
                                             HConstant* constant) {
  const bool recording_stats = stats_ != nullptr;
  size_t uses_before = 0;
  size_t uses_after = 0;
  if (recording_stats) {
    uses_before = variable->GetUses().SizeSlow();
  }

  variable->ReplaceUsesDominatedBy(
      starting_block->GetFirstInstruction(), constant, /* strictly_dominated= */ false);

  if (recording_stats) {
    uses_after = variable->GetUses().SizeSlow();
    DCHECK_GE(uses_after, 1u) << "we must at least have the use in the if clause.";
    DCHECK_GE(uses_before, uses_after);
    MaybeRecordStat(stats_, MethodCompilationStat::kPropagatedIfValue, uses_before - uses_after);
  }
}

void HConstantFoldingVisitor::VisitIf(HIf* inst) {
  // Consistency check: the true and false successors do not dominate each other.
  DCHECK(!inst->IfTrueSuccessor()->Dominates(inst->IfFalseSuccessor()) &&
         !inst->IfFalseSuccessor()->Dominates(inst->IfTrueSuccessor()));

  HInstruction* if_input = inst->InputAt(0);

  // Already a constant.
  if (if_input->IsConstant()) {
    return;
  }

  // if (variable) {
  //   SSA `variable` guaranteed to be true
  // } else {
  //   and here false
  // }
  PropagateValue(inst->IfTrueSuccessor(), if_input, GetGraph()->GetIntConstant(1));
  PropagateValue(inst->IfFalseSuccessor(), if_input, GetGraph()->GetIntConstant(0));

  // If the input is a condition, we can propagate the information of the condition itself.
  if (!if_input->IsCondition()) {
    return;
  }
  HCondition* condition = if_input->AsCondition();

  // We want either `==` or `!=`, since we cannot make assumptions for other conditions e.g. `>`
  if (!condition->IsEqual() && !condition->IsNotEqual()) {
    return;
  }

  HInstruction* left = condition->GetLeft();
  HInstruction* right = condition->GetRight();

  // We want one of them to be a constant and not the other.
  if (left->IsConstant() == right->IsConstant()) {
    return;
  }

  // At this point we have something like:
  // if (variable == constant) {
  //   SSA `variable` guaranteed to be equal to constant here
  // } else {
  //   No guarantees can be made here (usually, see boolean case below).
  // }
  // Similarly with variable != constant, except that we can make guarantees in the else case.

  HConstant* constant = left->IsConstant() ? left->AsConstant() : right->AsConstant();
  HInstruction* variable = left->IsConstant() ? right : left;

  // Don't deal with floats/doubles since they bring a lot of edge cases e.g.
  // if (f == 0.0f) {
  //   // f is not really guaranteed to be 0.0f. It could be -0.0f, for example
  // }
  if (DataType::IsFloatingPointType(variable->GetType())) {
    return;
  }
  DCHECK(!DataType::IsFloatingPointType(constant->GetType()));

  // Sometimes we have an HCompare flowing into an Equals/NonEquals, which can act as a proxy. For
  // example: `Equals(Compare(var, constant), 0)`. This is common for long, float, and double.
  if (variable->IsCompare()) {
    // We only care about equality comparisons so we skip if it is a less or greater comparison.
    if (!constant->IsArithmeticZero()) {
      return;
    }

    // Update left and right to be the ones from the HCompare.
    left = variable->AsCompare()->GetLeft();
    right = variable->AsCompare()->GetRight();

    // Re-check that one of them to be a constant and not the other.
    if (left->IsConstant() == right->IsConstant()) {
      return;
    }

    constant = left->IsConstant() ? left->AsConstant() : right->AsConstant();
    variable = left->IsConstant() ? right : left;

    // Re-check floating point values.
    if (DataType::IsFloatingPointType(variable->GetType())) {
      return;
    }
    DCHECK(!DataType::IsFloatingPointType(constant->GetType()));
  }

  // From this block forward we want to replace the SSA value. We use `starting_block` and not the
  // `if` block as we want to update one of the branches but not the other.
  HBasicBlock* starting_block =
      condition->IsEqual() ? inst->IfTrueSuccessor() : inst->IfFalseSuccessor();

  PropagateValue(starting_block, variable, constant);

  // Special case for booleans since they have only two values so we know what to propagate in the
  // other branch. However, sometimes our boolean values are not compared to 0 or 1. In those cases
  // we cannot make an assumption for the `else` branch.
  if (variable->GetType() == DataType::Type::kBool &&
      constant->IsIntConstant() &&
      (constant->AsIntConstant()->IsTrue() || constant->AsIntConstant()->IsFalse())) {
    HBasicBlock* other_starting_block =
        condition->IsEqual() ? inst->IfFalseSuccessor() : inst->IfTrueSuccessor();
    DCHECK_NE(other_starting_block, starting_block);

    HConstant* other_constant = constant->AsIntConstant()->IsTrue() ?
                                    GetGraph()->GetIntConstant(0) :
                                    GetGraph()->GetIntConstant(1);
    DCHECK_NE(other_constant, constant);
    PropagateValue(other_starting_block, variable, other_constant);
  }
}

void InstructionWithAbsorbingInputSimplifier::VisitShift(HBinaryOperation* instruction) {
  DCHECK(instruction->IsShl() || instruction->IsShr() || instruction->IsUShr());
  HInstruction* left = instruction->GetLeft();
  if (left->IsConstant() && left->AsConstant()->IsArithmeticZero()) {
    // Replace code looking like
    //    SHL dst, 0, shift_amount
    // with
    //    CONSTANT 0
    instruction->ReplaceWith(left);
    instruction->GetBlock()->RemoveInstruction(instruction);
  }
}

void InstructionWithAbsorbingInputSimplifier::VisitEqual(HEqual* instruction) {
  if ((instruction->GetLeft()->IsNullConstant() && !instruction->GetRight()->CanBeNull()) ||
      (instruction->GetRight()->IsNullConstant() && !instruction->GetLeft()->CanBeNull())) {
    // Replace code looking like
    //    EQUAL lhs, null
    // where lhs cannot be null with
    //    CONSTANT false
    instruction->ReplaceWith(GetGraph()->GetConstant(DataType::Type::kBool, 0));
    instruction->GetBlock()->RemoveInstruction(instruction);
  }
}

void InstructionWithAbsorbingInputSimplifier::VisitNotEqual(HNotEqual* instruction) {
  if ((instruction->GetLeft()->IsNullConstant() && !instruction->GetRight()->CanBeNull()) ||
      (instruction->GetRight()->IsNullConstant() && !instruction->GetLeft()->CanBeNull())) {
    // Replace code looking like
    //    NOT_EQUAL lhs, null
    // where lhs cannot be null with
    //    CONSTANT true
    instruction->ReplaceWith(GetGraph()->GetConstant(DataType::Type::kBool, 1));
    instruction->GetBlock()->RemoveInstruction(instruction);
  }
}

void InstructionWithAbsorbingInputSimplifier::VisitAbove(HAbove* instruction) {
  if (instruction->GetLeft()->IsConstant() &&
      instruction->GetLeft()->AsConstant()->IsArithmeticZero()) {
    // Replace code looking like
    //    ABOVE dst, 0, src  // unsigned 0 > src is always false
    // with
    //    CONSTANT false
    instruction->ReplaceWith(GetGraph()->GetConstant(DataType::Type::kBool, 0));
    instruction->GetBlock()->RemoveInstruction(instruction);
  }
}

void InstructionWithAbsorbingInputSimplifier::VisitAboveOrEqual(HAboveOrEqual* instruction) {
  if (instruction->GetRight()->IsConstant() &&
      instruction->GetRight()->AsConstant()->IsArithmeticZero()) {
    // Replace code looking like
    //    ABOVE_OR_EQUAL dst, src, 0  // unsigned src >= 0 is always true
    // with
    //    CONSTANT true
    instruction->ReplaceWith(GetGraph()->GetConstant(DataType::Type::kBool, 1));
    instruction->GetBlock()->RemoveInstruction(instruction);
  }
}

void InstructionWithAbsorbingInputSimplifier::VisitBelow(HBelow* instruction) {
  if (instruction->GetRight()->IsConstant() &&
      instruction->GetRight()->AsConstant()->IsArithmeticZero()) {
    // Replace code looking like
    //    BELOW dst, src, 0  // unsigned src < 0 is always false
    // with
    //    CONSTANT false
    instruction->ReplaceWith(GetGraph()->GetConstant(DataType::Type::kBool, 0));
    instruction->GetBlock()->RemoveInstruction(instruction);
  }
}

void InstructionWithAbsorbingInputSimplifier::VisitBelowOrEqual(HBelowOrEqual* instruction) {
  if (instruction->GetLeft()->IsConstant() &&
      instruction->GetLeft()->AsConstant()->IsArithmeticZero()) {
    // Replace code looking like
    //    BELOW_OR_EQUAL dst, 0, src  // unsigned 0 <= src is always true
    // with
    //    CONSTANT true
    instruction->ReplaceWith(GetGraph()->GetConstant(DataType::Type::kBool, 1));
    instruction->GetBlock()->RemoveInstruction(instruction);
  }
}

void InstructionWithAbsorbingInputSimplifier::VisitAnd(HAnd* instruction) {
  DataType::Type type = instruction->GetType();
  HConstant* input_cst = instruction->GetConstantRight();
  if ((input_cst != nullptr) && input_cst->IsZeroBitPattern()) {
    // Replace code looking like
    //    AND dst, src, 0
    // with
    //    CONSTANT 0
    instruction->ReplaceWith(input_cst);
    instruction->GetBlock()->RemoveInstruction(instruction);
  }

  HInstruction* left = instruction->GetLeft();
  HInstruction* right = instruction->GetRight();

  if (left->IsNot() ^ right->IsNot()) {
    // Replace code looking like
    //    NOT notsrc, src
    //    AND dst, notsrc, src
    // with
    //    CONSTANT 0
    HInstruction* hnot = (left->IsNot() ? left : right);
    HInstruction* hother = (left->IsNot() ? right : left);
    HInstruction* src = hnot->AsNot()->GetInput();

    if (src == hother) {
      instruction->ReplaceWith(GetGraph()->GetConstant(type, 0));
      instruction->GetBlock()->RemoveInstruction(instruction);
    }
  }
}

void InstructionWithAbsorbingInputSimplifier::VisitCompare(HCompare* instruction) {
  HConstant* input_cst = instruction->GetConstantRight();
  if (input_cst != nullptr) {
    HInstruction* input_value = instruction->GetLeastConstantLeft();
    if (DataType::IsFloatingPointType(input_value->GetType()) &&
        ((input_cst->IsFloatConstant() && input_cst->AsFloatConstant()->IsNaN()) ||
         (input_cst->IsDoubleConstant() && input_cst->AsDoubleConstant()->IsNaN()))) {
      // Replace code looking like
      //    CMP{G,L}-{FLOAT,DOUBLE} dst, src, NaN
      // with
      //    CONSTANT +1 (gt bias)
      // or
      //    CONSTANT -1 (lt bias)
      instruction->ReplaceWith(GetGraph()->GetConstant(DataType::Type::kInt32,
                                                       (instruction->IsGtBias() ? 1 : -1)));
      instruction->GetBlock()->RemoveInstruction(instruction);
    }
  }
}

void InstructionWithAbsorbingInputSimplifier::VisitMul(HMul* instruction) {
  HConstant* input_cst = instruction->GetConstantRight();
  DataType::Type type = instruction->GetType();
  if (DataType::IsIntOrLongType(type) &&
      (input_cst != nullptr) && input_cst->IsArithmeticZero()) {
    // Replace code looking like
    //    MUL dst, src, 0
    // with
    //    CONSTANT 0
    // Integral multiplication by zero always yields zero, but floating-point
    // multiplication by zero does not always do. For example `Infinity * 0.0`
    // should yield a NaN.
    instruction->ReplaceWith(input_cst);
    instruction->GetBlock()->RemoveInstruction(instruction);
  }
}

void InstructionWithAbsorbingInputSimplifier::VisitOr(HOr* instruction) {
  HConstant* input_cst = instruction->GetConstantRight();

  if (input_cst == nullptr) {
    return;
  }

  if (Int64FromConstant(input_cst) == -1) {
    // Replace code looking like
    //    OR dst, src, 0xFFF...FF
    // with
    //    CONSTANT 0xFFF...FF
    instruction->ReplaceWith(input_cst);
    instruction->GetBlock()->RemoveInstruction(instruction);
  }
}

void InstructionWithAbsorbingInputSimplifier::VisitRem(HRem* instruction) {
  DataType::Type type = instruction->GetType();

  if (!DataType::IsIntegralType(type)) {
    return;
  }

  HBasicBlock* block = instruction->GetBlock();

  if (instruction->GetLeft()->IsConstant() &&
      instruction->GetLeft()->AsConstant()->IsArithmeticZero()) {
    // Replace code looking like
    //    REM dst, 0, src
    // with
    //    CONSTANT 0
    instruction->ReplaceWith(instruction->GetLeft());
    block->RemoveInstruction(instruction);
  }

  HConstant* cst_right = instruction->GetRight()->AsConstant();
  if (((cst_right != nullptr) &&
       (cst_right->IsOne() || cst_right->IsMinusOne())) ||
      (instruction->GetLeft() == instruction->GetRight())) {
    // Replace code looking like
    //    REM dst, src, 1
    // or
    //    REM dst, src, -1
    // or
    //    REM dst, src, src
    // with
    //    CONSTANT 0
    instruction->ReplaceWith(GetGraph()->GetConstant(type, 0));
    block->RemoveInstruction(instruction);
  }
}

void InstructionWithAbsorbingInputSimplifier::VisitShl(HShl* instruction) {
  VisitShift(instruction);
}

void InstructionWithAbsorbingInputSimplifier::VisitShr(HShr* instruction) {
  VisitShift(instruction);
}

void InstructionWithAbsorbingInputSimplifier::VisitSub(HSub* instruction) {
  DataType::Type type = instruction->GetType();

  if (!DataType::IsIntegralType(type)) {
    return;
  }

  HBasicBlock* block = instruction->GetBlock();

  // We assume that GVN has run before, so we only perform a pointer
  // comparison.  If for some reason the values are equal but the pointers are
  // different, we are still correct and only miss an optimization
  // opportunity.
  if (instruction->GetLeft() == instruction->GetRight()) {
    // Replace code looking like
    //    SUB dst, src, src
    // with
    //    CONSTANT 0
    // Note that we cannot optimize `x - x` to `0` for floating-point. It does
    // not work when `x` is an infinity.
    instruction->ReplaceWith(GetGraph()->GetConstant(type, 0));
    block->RemoveInstruction(instruction);
  }
}

void InstructionWithAbsorbingInputSimplifier::VisitUShr(HUShr* instruction) {
  VisitShift(instruction);
}

void InstructionWithAbsorbingInputSimplifier::VisitXor(HXor* instruction) {
  if (instruction->GetLeft() == instruction->GetRight()) {
    // Replace code looking like
    //    XOR dst, src, src
    // with
    //    CONSTANT 0
    DataType::Type type = instruction->GetType();
    HBasicBlock* block = instruction->GetBlock();
    instruction->ReplaceWith(GetGraph()->GetConstant(type, 0));
    block->RemoveInstruction(instruction);
  }
}

}  // namespace art
