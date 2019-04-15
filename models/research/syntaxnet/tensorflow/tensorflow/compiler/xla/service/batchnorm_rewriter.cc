/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/service/batchnorm_rewriter.h"

#include <algorithm>
#include <memory>
#include <numeric>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "tensorflow/compiler/xla/layout_util.h"
#include "tensorflow/compiler/xla/literal_util.h"
#include "tensorflow/compiler/xla/service/dfs_hlo_visitor_with_default.h"
#include "tensorflow/compiler/xla/service/hlo_computation.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_opcode.h"
#include "tensorflow/compiler/xla/service/hlo_query.h"
#include "tensorflow/compiler/xla/service/shape_inference.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/types.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/compiler/xla/window_util.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/gtl/array_slice.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/types.h"

namespace xla {

// BatchNormRewriterVisitor traverses the HLO computation and rewrites BatchNorm
// operations into smaller operations.
class BatchNormRewriterVisitor : public DfsHloVisitorWithDefault {
 public:
  // Default visitor action is to do nothing and return OK.
  Status DefaultAction(HloInstruction* /*hlo_instruction*/) override {
    return Status::OK();
  }

  Status HandleBatchNormTraining(HloInstruction* batch_norm) override;

  Status HandleBatchNormGrad(HloInstruction* batch_norm) override;

  // Runs the visitor on a computation.
  static bool Run(HloComputation* computation, bool rewrite_training_op,
                  bool rewrite_grad_op, bool use_fusion);

  // Returns whether any batch norm ops were rewritten.
  const bool changed() const { return changed_; }

  ~BatchNormRewriterVisitor() override = default;

 private:
  explicit BatchNormRewriterVisitor(HloComputation* computation,
                                    bool rewrite_training_op,
                                    bool rewrite_grad_op, bool use_fusion)
      : computation_(computation),
        rewrite_training_op_(rewrite_training_op),
        rewrite_grad_op_(rewrite_grad_op),
        use_fusion_(use_fusion) {}

  HloComputation* GetScalarBinaryComputation(PrimitiveType primitive_type,
                                             HloOpcode opcode) {
    HloComputation::Builder b("scalar computation");
    auto scalar_lhs = b.AddInstruction(HloInstruction::CreateParameter(
        0, ShapeUtil::MakeShape(F32, {}), "scalar lhs"));
    auto scalar_rhs = b.AddInstruction(HloInstruction::CreateParameter(
        1, ShapeUtil::MakeShape(F32, {}), "scalar rhs"));
    auto scalar_op = b.AddInstruction(
        HloInstruction::CreateBinary(ShapeUtil::MakeShape(primitive_type, {}),
                                     opcode, scalar_lhs, scalar_rhs));
    return computation_->parent()->AddEmbeddedComputation(b.Build(scalar_op));
  }

  // Current HloComputation instance the BatchNormRewriter is
  // traversing.
  HloComputation* computation_;

  bool rewrite_training_op_;
  bool rewrite_grad_op_;
  bool use_fusion_;

  // Whether rewrite has occurred.
  bool changed_ = false;

  // Replaces the existing HLO instruction old_instruction, with
  // new_instruction, and marks the optimizer status as changed.
  // Returns the Status representing the result of the replace operation.
  Status ReplaceWithNewInstruction(
      HloInstruction* old_instruction,
      std::unique_ptr<HloInstruction> new_instruction) {
    TF_RETURN_IF_ERROR(computation_->ReplaceWithNewInstruction(
        old_instruction, std::move(new_instruction)));
    changed_ = true;
    return Status::OK();
  }

  // Replaces the existing HLO instruction old_instruction, with
  // new_instruction, and marks the optimizer status as changed.
  // Returns the Status representing the result of the replace operation.
  Status ReplaceInstruction(HloInstruction* old_instruction,
                            HloInstruction* new_instruction) {
    TF_RETURN_IF_ERROR(
        computation_->ReplaceInstruction(old_instruction, new_instruction));
    changed_ = true;
    return Status::OK();
  }
};

bool BatchNormRewriterVisitor::Run(HloComputation* computation,
                                   bool rewrite_training_op,
                                   bool rewrite_grad_op, bool use_fusion) {
  BatchNormRewriterVisitor visitor(computation,
                                   /*rewrite_training_op=*/rewrite_training_op,
                                   /*rewrite_grad_op=*/rewrite_grad_op,
                                   /*use_fusion=*/use_fusion);
  TF_CHECK_OK(computation->Accept(&visitor));
  return visitor.changed_;
}

Status BatchNormRewriterVisitor::HandleBatchNormTraining(
    HloInstruction* batch_norm) {
  if (!rewrite_training_op_) {
    return Status::OK();
  }
  // Expand batch norm training into smaller HLO ops.
  HloInstruction* operand = batch_norm->mutable_operand(0);
  const Shape operand_shape = operand->shape();
  int64 feature_index = batch_norm->feature_index();
  const int64 feature_count = operand_shape.dimensions(feature_index);
  const int64 size_in_elements = ShapeUtil::ElementsIn(operand_shape);
  auto elements_per_feature =
      computation_->AddInstruction(HloInstruction::CreateConstant(
          Literal::CreateR0<float>(size_in_elements / feature_count)));

  HloInstruction* scale = batch_norm->mutable_operand(1);
  HloInstruction* offset = batch_norm->mutable_operand(2);
  const Shape feature_shape = scale->shape();

  auto zero = computation_->AddInstruction(
      HloInstruction::CreateConstant(Literal::CreateR0(0.0f)));

  auto epsilon = computation_->AddInstruction(
      HloInstruction::CreateConstant(Literal::CreateR0(batch_norm->epsilon())));

  std::vector<int64> dimensions_without_feature;

  for (int64 i = 0; i < ShapeUtil::Rank(operand_shape); ++i) {
    if (i != feature_index) {
      dimensions_without_feature.push_back(i);
    }
  }

  auto scale_broadcasted = computation_->AddInstruction(
      HloInstruction::CreateBroadcast(operand_shape, scale, {feature_index}));

  auto offset_broadcasted = computation_->AddInstruction(
      HloInstruction::CreateBroadcast(operand_shape, offset, {feature_index}));

  HloComputation* add_reduce_computation =
      GetScalarBinaryComputation(F32, HloOpcode::kAdd);

  // X^2.
  auto operand_squared =
      computation_->AddInstruction(HloInstruction::CreateBinary(
          operand_shape, HloOpcode::kMultiply, operand, operand));
  // Sum[X].
  auto sum = computation_->AddInstruction(HloInstruction::CreateReduce(
      feature_shape, operand, zero, dimensions_without_feature,
      add_reduce_computation));

  // Sum[X^2].
  auto squared_sum = computation_->AddInstruction(HloInstruction::CreateReduce(
      feature_shape, operand_squared, zero, dimensions_without_feature,
      add_reduce_computation));

  // Fuse two parallel reduces together to improve performance.
  if (use_fusion_) {
    auto tuple = computation_->AddInstruction(
        HloInstruction::CreateTuple({sum, squared_sum}));

    auto fused = computation_->CreateFusionInstruction(
        {tuple, sum, squared_sum, operand_squared},
        HloInstruction::FusionKind::kInput);

    sum = computation_->AddInstruction(
        HloInstruction::CreateGetTupleElement(feature_shape, fused, 0));

    squared_sum = computation_->AddInstruction(
        HloInstruction::CreateGetTupleElement(feature_shape, fused, 1));
  }

  // E[X].
  auto mean = computation_->AddInstruction(HloInstruction::CreateBinary(
      feature_shape, HloOpcode::kDivide, sum, elements_per_feature));

  auto mean_broadcasted = computation_->AddInstruction(
      HloInstruction::CreateBroadcast(operand_shape, mean, {feature_index}));

  // E[X^2].
  auto square_mean = computation_->AddInstruction(HloInstruction::CreateBinary(
      feature_shape, HloOpcode::kDivide, squared_sum, elements_per_feature));

  // E^2[X].
  auto mean_square = computation_->AddInstruction(HloInstruction::CreateBinary(
      feature_shape, HloOpcode::kMultiply, mean, mean));

  // Var[X].
  auto var = computation_->AddInstruction(HloInstruction::CreateBinary(
      feature_shape, HloOpcode::kSubtract, square_mean, mean_square));

  auto var_broadcasted = computation_->AddInstruction(
      HloInstruction::CreateBroadcast(operand_shape, var, {feature_index}));

  // Var[X] + epsilon.
  auto var_add_epsilon =
      computation_->AddInstruction(HloInstruction::CreateBinary(
          operand_shape, HloOpcode::kAdd, var_broadcasted, epsilon));

  auto neg_half = computation_->AddInstruction(
      HloInstruction::CreateConstant(Literal::CreateR0(-0.5f)));

  // 1 / Sqrt[Var[X] + epsilon].
  auto rsqrt_var_add_epsilon =
      computation_->AddInstruction(HloInstruction::CreateBinary(
          operand_shape, HloOpcode::kPower, var_add_epsilon, neg_half));

  // X - E[X].
  auto operand_minus_mean =
      computation_->AddInstruction(HloInstruction::CreateBinary(
          operand_shape, HloOpcode::kSubtract, operand, mean_broadcasted));

  // (X - E[X]) / Sqrt[Var[X] + epsilon].
  auto normalized = computation_->AddInstruction(
      HloInstruction::CreateBinary(operand_shape, HloOpcode::kMultiply,
                                   operand_minus_mean, rsqrt_var_add_epsilon));

  // (X - E[X]) / Sqrt[Var[X] + epsilon] * scale.
  auto scaled_normalized =
      computation_->AddInstruction(HloInstruction::CreateBinary(
          operand_shape, HloOpcode::kMultiply, normalized, scale_broadcasted));

  // (X - E[X]) / Sqrt[Var[X] + epsilon] * scale + offset.
  auto shifted_normalized = computation_->AddInstruction(
      HloInstruction::CreateBinary(operand_shape, HloOpcode::kAdd,
                                   scaled_normalized, offset_broadcasted));

  TF_CHECK_OK(ReplaceWithNewInstruction(
      batch_norm,
      HloInstruction::CreateTuple({shifted_normalized, mean, var})));
  return Status::OK();
}

Status BatchNormRewriterVisitor::HandleBatchNormGrad(
    HloInstruction* batch_norm) {
  // Use the following formulas to calculate gradients:
  // scale_grad =
  //   sum(output_grad * (activation - mean(activation))) * rsqrt(var + epsilon)
  //
  // offset_grad =
  //   sum(output_grad)
  //
  // activation_grad =
  //   1/N * scale * rsqrt(var + epsilon) *
  //   (N * output_grad - sum(output_grad) - (activation - mean(activation)) *
  //   sum(output_grad * (activation - mean(activation))) / (variance +
  //   epsilon))
  if (!rewrite_grad_op_) {
    return Status::OK();
  }

  HloInstruction* activation = batch_norm->mutable_operand(0);
  const Shape activation_shape = activation->shape();
  HloInstruction* scale = batch_norm->mutable_operand(1);
  const Shape feature_shape = scale->shape();
  HloInstruction* mean = batch_norm->mutable_operand(2);
  HloInstruction* variance = batch_norm->mutable_operand(3);
  HloInstruction* grad_output = batch_norm->mutable_operand(4);

  int64 feature_index = batch_norm->feature_index();

  const int64 size_in_elements = ShapeUtil::ElementsIn(activation_shape);
  const int64 feature_count = activation_shape.dimensions(feature_index);
  auto elements_per_feature =
      computation_->AddInstruction(HloInstruction::CreateConstant(
          Literal::CreateR0<float>(size_in_elements / feature_count)));

  auto zero = computation_->AddInstruction(
      HloInstruction::CreateConstant(Literal::CreateR0(0.0f)));

  auto neg_half = computation_->AddInstruction(
      HloInstruction::CreateConstant(Literal::CreateR0(-0.5f)));

  auto epsilon = computation_->AddInstruction(
      HloInstruction::CreateConstant(Literal::CreateR0(batch_norm->epsilon())));

  std::vector<int64> dimensions_without_feature;

  for (int64 i = 0; i < ShapeUtil::Rank(activation_shape); ++i) {
    if (i != feature_index) {
      dimensions_without_feature.push_back(i);
    }
  }

  auto scale_broadcasted =
      computation_->AddInstruction(HloInstruction::CreateBroadcast(
          activation_shape, scale, {feature_index}));
  auto variance_broadcasted =
      computation_->AddInstruction(HloInstruction::CreateBroadcast(
          activation_shape, variance, {feature_index}));

  // E[X].
  auto mean_broadcasted = computation_->AddInstruction(
      HloInstruction::CreateBroadcast(activation_shape, mean, {feature_index}));

  // rsqrt[Var[X] + epsilon].
  auto rsqrt_var_add_epsilon_broadcasted =
      computation_->AddInstruction(HloInstruction::CreateBinary(
          activation_shape, HloOpcode::kPower,
          computation_->AddInstruction(
              HloInstruction::CreateBinary(activation_shape, HloOpcode::kAdd,
                                           variance_broadcasted, epsilon)),
          neg_half));

  auto rsqrt_var_add_epsilon =
      computation_->AddInstruction(HloInstruction::CreateBinary(
          feature_shape, HloOpcode::kPower,
          computation_->AddInstruction(HloInstruction::CreateBinary(
              feature_shape, HloOpcode::kAdd, variance, epsilon)),
          neg_half));

  // X - E[X].
  auto activation_minus_mean = computation_->AddInstruction(
      HloInstruction::CreateBinary(activation_shape, HloOpcode::kSubtract,
                                   activation, mean_broadcasted));

  // Grad[Y] * (X - E[X]).
  auto grad_output_times_activiation_minus_mean = computation_->AddInstruction(
      HloInstruction::CreateBinary(activation_shape, HloOpcode::kMultiply,
                                   grad_output, activation_minus_mean));

  HloComputation* add_reduce_computation =
      GetScalarBinaryComputation(F32, HloOpcode::kAdd);

  // sum(Grad[Y] * (X - E[X])).
  auto sum_grad_output_times_activiation_minus_mean =
      computation_->AddInstruction(HloInstruction::CreateReduce(
          feature_shape, grad_output_times_activiation_minus_mean, zero,
          dimensions_without_feature, add_reduce_computation));

  // Grad[beta] = Sum(Grad[Y]).
  auto grad_beta = computation_->AddInstruction(HloInstruction::CreateReduce(
      feature_shape, grad_output, zero, dimensions_without_feature,
      add_reduce_computation));

  if (use_fusion_) {
    auto tuple = computation_->AddInstruction(HloInstruction::CreateTuple(
        {sum_grad_output_times_activiation_minus_mean, grad_beta}));

    auto fused = computation_->CreateFusionInstruction(
        {tuple, sum_grad_output_times_activiation_minus_mean, grad_beta},
        HloInstruction::FusionKind::kInput);

    sum_grad_output_times_activiation_minus_mean = computation_->AddInstruction(
        HloInstruction::CreateGetTupleElement(feature_shape, fused, 0));

    grad_beta = computation_->AddInstruction(
        HloInstruction::CreateGetTupleElement(feature_shape, fused, 1));
  }

  // Grad[scale] = Sum(Grad[Y] * (X - E[X]) * rsqrt[Var[X] + epsilon]).
  auto grad_scale = computation_->AddInstruction(HloInstruction::CreateBinary(
      feature_shape, HloOpcode::kMultiply,
      sum_grad_output_times_activiation_minus_mean, rsqrt_var_add_epsilon));

  // I2 = Sum(Grad[Y])
  auto I2 = computation_->AddInstruction(HloInstruction::CreateBroadcast(
      activation_shape, grad_beta, {feature_index}));

  // I3 = Sum(Grad[Y] * (X - E[X]))
  auto I3 = computation_->AddInstruction(HloInstruction::CreateBroadcast(
      activation_shape, sum_grad_output_times_activiation_minus_mean,
      {feature_index}));

  // I4 = (X - E[X]) * I3
  auto I4 = computation_->AddInstruction(HloInstruction::CreateBinary(
      activation_shape, HloOpcode::kMultiply, I3, activation_minus_mean));

  // I5 = I4 / (Var[X] + epsilon)
  auto I5 = computation_->AddInstruction(HloInstruction::CreateBinary(
      activation_shape, HloOpcode::kDivide, I4,
      computation_->AddInstruction(HloInstruction::CreateBinary(
          activation_shape, HloOpcode::kAdd, variance_broadcasted, epsilon))));

  // scale * rsqrt[Var[X] + epsilon] * 1/N
  auto scale_times_rsqrt_var_add_epsilon =
      computation_->AddInstruction(HloInstruction::CreateBinary(
          activation_shape, HloOpcode::kMultiply, scale_broadcasted,
          rsqrt_var_add_epsilon_broadcasted));

  scale_times_rsqrt_var_add_epsilon =
      computation_->AddInstruction(HloInstruction::CreateBinary(
          activation_shape, HloOpcode::kDivide,
          scale_times_rsqrt_var_add_epsilon, elements_per_feature));

  auto I1 = computation_->AddInstruction(
      HloInstruction::CreateBinary(activation_shape, HloOpcode::kMultiply,
                                   grad_output, elements_per_feature));

  // I6 = I1 - I2 - I5
  auto I6 = computation_->AddInstruction(HloInstruction::CreateBinary(
      activation_shape, HloOpcode::kSubtract,
      computation_->AddInstruction(HloInstruction::CreateBinary(
          activation_shape, HloOpcode::kSubtract, I1, I2)),
      I5));

  // Grad[X] = scale * rsqrt[Var[X] + epsilon] * 1/N * I6.
  auto grad_activation = computation_->AddInstruction(
      HloInstruction::CreateBinary(activation_shape, HloOpcode::kMultiply,
                                   scale_times_rsqrt_var_add_epsilon, I6));

  TF_CHECK_OK(ReplaceWithNewInstruction(
      batch_norm,
      HloInstruction::CreateTuple({grad_activation, grad_scale, grad_beta})));

  return Status::OK();
}

StatusOr<bool> BatchNormRewriter::Run(HloModule* module) {
  XLA_VLOG_LINES(2, "BatchNormRewriter::Run(), before:\n" + module->ToString());
  bool changed = false;
  // Make a copy of the computations because we may add computations to the
  // module, invalidating iteration.
  std::vector<HloComputation*> computations;
  for (auto& comp : module->computations()) {
    if (comp->IsFusionComputation()) {
      continue;
    }
    computations.push_back(comp.get());
  }
  for (auto& comp : computations) {
    if (BatchNormRewriterVisitor::Run(comp, rewrite_training_op_,
                                      rewrite_grad_op_, use_fusion_)) {
      changed = true;
    }
  }
  XLA_VLOG_LINES(2, "BatchNormRewriter::Run(), after:\n" + module->ToString());
  return changed;
}

}  // namespace xla
