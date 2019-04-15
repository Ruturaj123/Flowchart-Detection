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

#define EIGEN_USE_THREADS

#include "tensorflow/core/grappler/optimizers/constant_folding.h"
#include "tensorflow/core/framework/allocator.h"
#include "tensorflow/core/framework/attr_value.pb.h"
#include "tensorflow/core/framework/function.pb.h"
#include "tensorflow/core/framework/node_def.pb.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_def.pb.h"
#include "tensorflow/core/framework/tensor.pb.h"
#include "tensorflow/core/framework/tensor_shape.pb.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/framework/versions.pb.h"
#include "tensorflow/core/grappler/clusters/cluster.h"
#include "tensorflow/core/grappler/costs/graph_properties.h"
#include "tensorflow/core/grappler/grappler_item.h"
#include "tensorflow/core/grappler/op_types.h"
#include "tensorflow/core/lib/gtl/cleanup.h"
#include "tensorflow/core/lib/gtl/inlined_vector.h"
#include "tensorflow/core/lib/strings/numbers.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/public/version.h"

namespace tensorflow {
namespace grappler {
using TensorVector = gtl::InlinedVector<TensorValue, 4>;

namespace {
class EigenThreadPoolWrapper : public Eigen::ThreadPoolInterface {
 public:
  explicit EigenThreadPoolWrapper(thread::ThreadPool* pool) : pool_(pool) {}
  ~EigenThreadPoolWrapper() override {}
  void Schedule(std::function<void()> fn) override {
    pool_->Schedule(std::move(fn));
  }
  int NumThreads() const override { return pool_->NumThreads(); }
  int CurrentThreadId() const override { return pool_->CurrentThreadId(); }

 private:
  thread::ThreadPool* pool_ = nullptr;
};

class DeviceSimple : public DeviceBase {
 public:
  DeviceSimple() : DeviceBase(Env::Default()) {
    eigen_worker_threads_.num_threads = port::NumSchedulableCPUs();
    eigen_worker_threads_.workers = new thread::ThreadPool(
        Env::Default(), "constant_folding", eigen_worker_threads_.num_threads);
    eigen_threadpool_wrapper_.reset(
        new EigenThreadPoolWrapper(eigen_worker_threads_.workers));
    eigen_device_.reset(new Eigen::ThreadPoolDevice(
        eigen_threadpool_wrapper_.get(), eigen_worker_threads_.num_threads));
    set_tensorflow_cpu_worker_threads(&eigen_worker_threads_);
    set_eigen_cpu_device(eigen_device_.get());
  }
  ~DeviceSimple() override {
    eigen_threadpool_wrapper_.reset();
    eigen_device_.reset();
    delete eigen_worker_threads_.workers;
  }
  Status MakeTensorFromProto(const TensorProto& tensor_proto,
                             const AllocatorAttributes alloc_attrs,
                             Tensor* tensor) override {
    Tensor parsed(tensor_proto.dtype());
    if (!parsed.FromProto(cpu_allocator(), tensor_proto)) {
      return errors::InvalidArgument("Cannot parse tensor from tensor_proto.");
    }
    *tensor = parsed;
    return Status::OK();
  }
  Allocator* GetAllocator(AllocatorAttributes attr) override {
    return cpu_allocator();
  }

 private:
  DeviceBase::CpuWorkerThreads eigen_worker_threads_;
  std::unique_ptr<Eigen::ThreadPoolInterface> eigen_threadpool_wrapper_;
  std::unique_ptr<Eigen::ThreadPoolDevice> eigen_device_;
};

string AsControlDependency(const NodeDef& node) {
  return strings::StrCat("^", node.name());
}

}  // namespace

ConstantFolding::ConstantFolding() {
  resource_mgr_.reset(new ResourceMgr());
}

string ConstantFolding::AddControlDependency(const string& input_name) {
  const NodeDef* node = node_map_->GetNode(input_name);
  if (!IsSwitch(*node)) {
    return AsControlDependency(*node);
  } else {
    // We can't anchor control dependencies directly on the switch node: unlike
    // other nodes only one of the outputs of the switch node will be generated
    // when the switch node is executed, and we need to make sure the control
    // dependency is only triggered when the corresponding output is triggered.
    // We start by looking for an identity node connected to the output of the
    // switch node, and use it to anchor the control dependency.
    auto outputs = node_map_->GetOutputs(node->name());
    for (const NodeDef* node : outputs) {
      if (IsIdentity(*node)) {
        CHECK_EQ(1, node->input_size());
        if (IsSameInput(node->input(0), input_name)) {
          return AsControlDependency(*node);
        }
      }
    }
    // We haven't found an existing node where we can anchor the control
    // dependency: add a new identity node.
    int position = 0;
    string ctrl_dep_name = ParseNodeName(input_name, &position);
    strings::StrAppend(&ctrl_dep_name, "_", position);
    ctrl_dep_name = AddPrefixToNodeName(ctrl_dep_name, kConstantFoldingCtrl);
    const DataType output_type = node->attr().at("T").type();

    NodeDef* added_node = graph_.add_node();
    added_node->set_name(ctrl_dep_name);
    added_node->set_op("Identity");
    added_node->set_device(node->device());

    (*added_node->mutable_attr())["T"].set_type(output_type);
    *added_node->add_input() = input_name;
    node_map_->AddNode(added_node->name(), added_node);
    node_map_->AddOutput(node->name(), added_node->name());
    return AsControlDependency(*added_node);
  }
}

Status ConstantFolding::MaterializeShapes(const GrapplerItem& item,
                                          const GraphProperties& properties) {
  // We may add some nodes to the graph to encode control dependencies: there is
  // no need to process these, so only iterate over the nodes of the input
  // graph.
  const int node_count = graph_.node_size();
  for (int i = 0; i < node_count; ++i) {
    NodeDef& node = *graph_.mutable_node(i);
    const string op = node.op();
    if (op != "Shape" && op != "Size" && op != "Rank") {
      continue;
    }
    std::vector<OpInfo::TensorProperties> output =
        properties.GetOutputProperties(node.name());
    CHECK_EQ(1, output.size());
    const DataType type = output[0].dtype();
    CHECK(type == DT_INT32 || type == DT_INT64);

    std::vector<OpInfo::TensorProperties> input =
        properties.GetInputProperties(node.name());
    CHECK_EQ(1, input.size());

    const TensorShapeProto shape = input[0].shape();
    // Materialize the shapes using constants whenever possible.
    PartialTensorShape shp(shape);
    if (shp.IsFullyDefined() || (!shp.unknown_rank() && op == "Rank")) {
      bool valid = true;
      Tensor value(type);
      if (op == "Shape") {
        value = Tensor(type, TensorShape({shp.dims()}));
        for (int i = 0; i < shp.dims(); ++i) {
          if (type == DT_INT32) {
            if (shp.dim_size(i) >= INT_MAX) {
              valid = false;
              break;
            }
            value.flat<int32>()(i) = shp.dim_size(i);
          } else {
            value.flat<int64>()(i) = shp.dim_size(i);
          }
        }
      } else if (op == "Size") {
        int64 size = 1;
        for (int i = 0; i < shp.dims(); ++i) {
          size *= shp.dim_size(i);
        }
        value = Tensor(type, TensorShape({}));
        if (type == DT_INT32) {
          if (size >= INT_MAX) {
            valid = false;
          } else {
            value.flat<int32>()(0) = size;
          }
        } else {
          value.flat<int64>()(0) = size;
        }
      } else {
        value = Tensor(type, TensorShape({}));
        if (type == DT_INT32) {
          if (shp.dims() >= INT_MAX) {
            valid = false;
          } else {
            value.flat<int32>()(0) = shp.dims();
          }
        } else {
          value.flat<int64>()(0) = shp.dims();
        }
      }

      if (valid) {
        // Replace the node with the corresponding constant.
        node.set_op("Const");
        node.clear_attr();
        (*node.mutable_attr())["dtype"].set_type(type);
        value.AsProtoTensorContent(
            (*node.mutable_attr())["value"].mutable_tensor());

        // Turn the data input into a control dependency: this is needed to
        // ensure that the constant value will only be generated in the cases
        // where the shape/rank/size would have been generated in the original
        // graph. Additional inputs are extra control dependencies that we
        // preserve.
        CHECK_LE(1, node.input_size());
        string ctrl_dep = AddControlDependency(node.input(0));
        node.set_input(0, ctrl_dep);
      }
    }
  }
  return Status::OK();
}

bool ConstantFolding::IsFoldable(const NodeDef& node) const {
  // Folding not applicable to ops with no inputs.
  if (node.input().empty()) {
    return false;
  }

  // Skips nodes that must be preserved, and op_types that don't benefit from
  // folding
  if (nodes_to_preserve_.find(node.name()) != nodes_to_preserve_.end()) {
    return false;
  }

  const string& op = node.op();
  // Skip constants, they're already folded
  if (op == "Const") {
    return false;
  }
  // Skip constrol flow nodes, they can't be folded
  if (op == "Enter" || op == "RefEnter" || op == "Exit" || op == "RefExit" ||
      op == "NextIteration" || op == "RefNextIteration") {
    return false;
  }
  if (op.find("Placeholder") == 0) {
    return false;
  }
  if (op.find("Save") != string::npos || op.find("Restore") != string::npos ||
      op.find("Reader") != string::npos) {
    return false;
  }
  if (op.find("Quantized") != string::npos || op.find("Sparse") == 0) {
    return false;
  }
  if (node.attr().count("_XlaCompile") > 0) {
    return false;
  }

  // Don't fold stateful ops such as TruncatedNormal.
  const OpDef* op_def = nullptr;
  Status status = OpRegistry::Global()->LookUpOpDef(node.op(), &op_def);
  if (!status.ok()) {
    return false;
  }
  if (op_def->is_stateful()) {
    return false;
  }

  if (op_def->output_arg_size() == 0) {
    return false;
  }

  // No need to (and don't) fold nodes that have no outgoing edges. Such nodes
  // could be introduced by an earlier constant folding pass and are preserved
  // in case users want to fetch their values; re-processing them would
  // lead to an error of adding a duplicated node to graph.
  auto outputs = node_map_->GetOutputs(node.name());
  if (outputs.empty()) {
    return false;
  }

  // We can only fold nodes if all their inputs are known statically, except in
  // the case of a merge node that propagate the first inputs that becomes
  // available, and therefore only requires a single constant input to be
  // foldable.
  bool has_constant_input = false;
  const bool is_merge = IsMerge(node);
  for (const auto& input : node.input()) {
    if (IsControlInput(input)) {
      continue;
    }
    const NodeDef* input_node = node_map_->GetNode(input);
    if (!input_node) {
      return false;
    }
    bool is_const = IsConstant(*input_node);
    if (!is_const && !is_merge) {
      return false;
    }
    // Don't fold strings constants for now since this causes problems with
    // checkpointing.
    if (is_const && input_node->attr().at("dtype").type() == DT_STRING) {
      return false;
    }
    has_constant_input |= is_const;
  }
  if (is_merge) {
    return has_constant_input;
  }

  return true;
}

NodeDef ConstantFolding::CreateNodeDef(const string& name,
                                       const TensorValue& tensor) {
  NodeDef node;
  node.set_name(name);
  node.set_op("Const");

  AttrValue attr_type;
  attr_type.set_type(tensor->dtype());
  node.mutable_attr()->insert({"dtype", attr_type});

  AttrValue attr_tensor;
  TensorProto* t = attr_tensor.mutable_tensor();
  bool optimized = false;
  // Use the packed representation whenever possible to avoid generating large
  // graphdefs. Moreover, avoid repeating the last values if they're equal.
  if (tensor->NumElements() > 4) {
#define POPULATE_TENSOR_PROTO(tensor, t, TYPE, NAME)         \
  optimized = true;                                          \
  TYPE last = tensor->flat<TYPE>()(0);                       \
  int last_index = 0;                                        \
  for (int i = 0; i < tensor->NumElements(); ++i) {          \
    TYPE cur = tensor->flat<TYPE>()(i);                      \
    t->add_##NAME##_val(cur);                                \
    if (cur != last) {                                       \
      last = cur;                                            \
      last_index = i;                                        \
    }                                                        \
  }                                                          \
  /* Remove all identical trailing values to save memory. */ \
  t->mutable_##NAME##_val()->Truncate(last_index + 1);

    if (tensor->dtype() == DT_FLOAT) {
      POPULATE_TENSOR_PROTO(tensor, t, float, float)
    } else if (tensor->dtype() == DT_DOUBLE) {
      POPULATE_TENSOR_PROTO(tensor, t, double, double)
    } else if (tensor->dtype() == DT_INT64) {
      POPULATE_TENSOR_PROTO(tensor, t, int64, int64)
    } else if (tensor->dtype() == DT_INT32) {
      POPULATE_TENSOR_PROTO(tensor, t, int32, int)
    }
  }
  if (optimized) {
    // Also specify type and shape.
    t->set_dtype(tensor->dtype());
    tensor->shape().AsProto(t->mutable_tensor_shape());
  } else {
    tensor->AsProtoTensorContent(t);
  }
  node.mutable_attr()->insert({"value", attr_tensor});
  return node;
}

Status ConstantFolding::EvaluateNode(const NodeDef& node,
                                     const TensorVector& inputs,
                                     TensorVector* output) const {
  Status status;
  auto op_kernel =
      CreateOpKernel("CPU", device_.get(), device_->GetAllocator({}), node,
                     TF_GRAPH_DEF_VERSION, &status);
  TF_RETURN_IF_ERROR(status);
  OpKernelContext::Params params;
  params.device = device_.get();
  params.frame_iter = FrameAndIter(0, 0);
  params.inputs = &inputs;
  params.op_kernel = op_kernel.get();
  params.resource_manager = resource_mgr_.get();

  gtl::InlinedVector<AllocatorAttributes, 4> output_attrs;
  const int num_outputs = op_kernel->num_outputs();
  for (int i = 0; i < num_outputs; i++) {
    AllocatorAttributes attr;
    attr.set_on_host(true);
    output_attrs.push_back(attr);
  }
  params.output_attr_array = output_attrs.data();

  OpKernelContext op_context(&params);
  op_kernel->Compute(&op_context);
  for (int i = 0; i < num_outputs; i++) {
    output->push_back(op_context.release_output(i));
  }
  return op_context.status();
}

Status ConstantFolding::EvaluateOneFoldable(const NodeDef& node,
                                            std::vector<NodeDef>* outputs) {
  TensorVector inputs;
  auto inputs_cleanup = gtl::MakeCleanup([&inputs] {
    for (const auto& input : inputs) {
      delete input.tensor;
    }
  });

  for (const auto& input : node.input()) {
    int position = 0;
    ParseNodeName(input, &position);
    if (position < 0) {
      // Control dependency
      break;
    }
    const NodeDef* input_node = node_map_->GetNode(input);
    if (!IsConstant(*input_node)) {
      return Status(error::INVALID_ARGUMENT,
                    strings::StrCat("Can't fold ", node.name(), ", its ", input,
                                    " isn't constant"));
    }
    const TensorProto& raw_val = input_node->attr().at("value").tensor();
    Tensor* value = new Tensor(raw_val.dtype(), raw_val.tensor_shape());
    CHECK(value->FromProto(raw_val));
    inputs.emplace_back(value);
  }

  TensorVector output_tensors;
  TF_RETURN_IF_ERROR(EvaluateNode(node, inputs, &output_tensors));
  if (output_tensors.empty()) {
    return Status(error::INVALID_ARGUMENT, "Expected at least one output.");
  }

  for (size_t i = 0; i < output_tensors.size(); i++) {
    string node_name = AddPrefixToNodeName(node.name(), kConstantFoldingConst);
    if (output_tensors.size() > 1) {
      node_name = strings::StrCat(node_name, "-", i);
    }
    if (output_tensors[i].tensor) {
      outputs->push_back(CreateNodeDef(node_name, output_tensors[i]));
      delete output_tensors[i].tensor;
    } else {
      // Create an empty NodeDef to identify dead outputs (e.g. the output of a
      // switch that's not selected by the switch predicate).
      outputs->push_back(NodeDef());
    }
  }
  return Status::OK();
}

Status ConstantFolding::FoldNode(NodeDef* node, GraphDef* output) {
  if (IsMerge(*node)) {
    // Merge nodes are special, in the sense that they execute as soon as one of
    // their input is ready. We can therefore fold a merge node iff it has at
    // least one constant input without control dependency.
    // We still need to ensure that the nodes in the fanin of the merge node are
    // scheduled. We'll therefore add a control dependency from the merge node
    // to the folded constant. We end up with:
    //  * the merge node and its inputs are preserved as is
    //  * a new constant node C1, driven by the merge node through a control
    //  dependency, initialized to the value of the folded input
    //  * a new constant node C2, driven by the merge node through a control
    //  dependency, initialized to the index of the folded input
    //  * the fanout of the merge nodes is rewired to be driven by either C1 or
    //  C2.
    for (int input_index = 0; input_index < node->input_size(); ++input_index) {
      const auto& input = node->input(input_index);
      if (IsControlInput(input)) {
        // Try the next input.
        continue;
      }
      NodeDef* input_node = node_map_->GetNode(input);
      if (!IsConstant(*input_node)) {
        continue;
      }
      bool valid_input = true;
      for (const string& fanin_of_input : input_node->input()) {
        if (IsControlInput(fanin_of_input)) {
          valid_input = false;
          break;
        }
      }
      if (!valid_input) {
        // Try the next input
        continue;
      }

      string const_out_name =
          AddPrefixToNodeName(node->name(), kConstantFoldingConst);
      string const_index_name = AddPrefixToNodeName(
          strings::StrCat(node->name(), "_index"), kConstantFoldingConst);
      if (node_map_->GetNode(const_out_name) ||
          node_map_->GetNode(const_index_name)) {
        // Intended name already exists.
        return errors::AlreadyExists(
            strings::StrCat(const_out_name, " or ", const_index_name,
                            "already present in the graph"));
      }

      NodeDef* const_out = output->add_node();
      *const_out = *input_node;
      const_out->set_name(const_out_name);
      const_out->set_device(node->device());
      *const_out->add_input() = AsControlDependency(*node);
      node_map_->AddNode(const_out->name(), const_out);

      NodeDef* const_index = output->add_node();
      const_index->set_op("Const");
      Tensor index(DT_INT32, TensorShape({}));
      index.flat<int32>()(0) = input_index;
      (*const_index->mutable_attr())["dtype"].set_type(DT_INT32);
      index.AsProtoTensorContent(
          (*const_index->mutable_attr())["value"].mutable_tensor());
      const_index->set_name(const_index_name);
      const_index->set_device(node->device());
      *const_index->add_input() = AsControlDependency(*node);
      node_map_->AddNode(const_index->name(), const_index);

      auto outputs = node_map_->GetOutputs(node->name());
      for (auto& output : outputs) {
        for (int i = 0; i < output->input_size(); i++) {
          int position;
          string node_name = ParseNodeName(output->input(i), &position);
          if (node_name == node->name()) {
            if (position == 0) {
              *output->mutable_input(i) = const_out->name();
            } else if (position == 1) {
              *output->mutable_input(i) = const_index->name();
            } else {
              // This is a control dependency (or an invalid edge since the
              // merge node has only 2 inputs): preserve them.
            }
          }
        }
      }
      return Status::OK();
    }
    return Status::OK();
  }

  std::vector<NodeDef> const_nodes;
  TF_RETURN_IF_ERROR(EvaluateOneFoldable(*node, &const_nodes));
  NodeDef* constant_output = nullptr;
  for (int i = 0; i < const_nodes.size(); i++) {
    NodeDef* const_node = &const_nodes[i];
    if (const_node->name().empty()) {
      // Dead output: we can't create a constant to encode its value, so we'll
      // just skip it. We'll preserve the edges that originate from that
      // output below to preserve the overall behavior of the graph wrt dead
      // edges.
      continue;
    }

    for (const auto& input : node->input()) {
      if (IsControlInput(input)) {
        *const_node->add_input() = input;
      } else {
        NodeDef* input_node = node_map_->GetNode(input);
        for (const auto& fanin_of_input : input_node->input()) {
          if (IsControlInput(fanin_of_input)) {
            *const_node->add_input() = fanin_of_input;
          }
        }
      }
    }

    // We rewrite the existing node if it only has a single output, and
    // create new nodes otherwise.
    if (const_nodes.size() == 1) {
      node->set_op("Const");
      node->clear_input();
      *node->mutable_input() = const_node->input();
      *node->mutable_attr() = const_node->attr();
      break;
    } else {
      if (node_map_->GetNode(const_node->name())) {
        // Intended name already exists.
        return errors::AlreadyExists(strings::StrCat(
            const_node->name(), "already present in the graph"));
      }
      NodeDef* added_node = output->add_node();
      *added_node = *const_node;
      added_node->set_device(node->device());
      node_map_->AddNode(added_node->name(), added_node);
      // All the constant nodes encoding output values have the same control
      // dependencies (since these are the control dependencies of the node
      // we're trying to fold). Record one such constant node.
      constant_output = added_node;
    }
  }

  if (const_nodes.size() > 1) {
    auto outputs = node_map_->GetOutputs(node->name());
    for (const auto& output : outputs) {
      for (int i = 0; i < output->input_size(); i++) {
        int position;
        string node_name = ParseNodeName(output->input(i), &position);
        if (node_name == node->name()) {
          if (position < 0) {
            // Propagate control dependencies if possible. If not, we'll just
            // preserve the existing control dependencies.
            if (constant_output != nullptr) {
              *output->mutable_input(i) = AsControlDependency(*constant_output);
            }
          } else if (position < const_nodes.size() &&
                     !const_nodes[position].name().empty()) {
            // Replace alive outputs with the corresponding constant.
            *output->mutable_input(i) = const_nodes[position].name();
          } else {
            // Leave this edge alone.
            VLOG(1) << "Preserving edge from " << node->name() << ":"
                    << position << "[" << node->op() << "] to "
                    << output->name() << ":" << i << "[" << output->op() << "]";
          }
        }
      }
    }
  }
  return Status::OK();
}

Status ConstantFolding::FoldGraph(GraphDef* output) {
  std::unordered_set<string> processed_nodes;
  std::deque<NodeDef*> queue;
  for (int i = 0; i < graph_.node_size(); i++) {
    auto node = graph_.mutable_node(i);
    if (IsFoldable(*node)) {
      queue.push_back(node);
    }
  }
  while (!queue.empty()) {
    NodeDef* node = queue.front();
    queue.pop_front();
    if (processed_nodes.count(node->name())) {
      continue;
    }
    Status s = FoldNode(node, output);
    processed_nodes.insert(node->name());
    if (!s.ok()) {
      VLOG(1) << "Failed to fold node " << node->name() << ": " << s;
    } else {
      auto outputs = node_map_->GetOutputs(node->name());
      for (auto& output : outputs) {
        if (IsFoldable(*output)) {
          queue.push_back(output);
        }
      }
    }
  }

  // Build the graph after constant folding. Note that we keep all processed
  // nodes in the graph in case users need to fetch their values.
  for (const auto& node : graph_.node()) {
    auto added_node = output->add_node();
    *added_node = node;
  }
  return Status::OK();
}

// Returns true iff this reduction can be reduced to an identity (i.e if the set
// of dimensions to reduce along is empty). This happens often in the gradient
// graphs.
bool ConstantFolding::IsSimplifiableReduction(const NodeDef& node) const {
  if (IsReduction(node)) {
    CHECK_LE(2, node.input_size());
    const NodeDef* reductions_indices = node_map_->GetNode(node.input(1));
    if (IsConstant(*reductions_indices)) {
      TensorVector output;
      Status s = EvaluateNode(*reductions_indices, TensorVector(), &output);
      if (!s.ok()) {
        return false;
      }
      CHECK_EQ(1, output.size());
      int output_size = output[0]->NumElements();
      delete output[0].tensor;
      if (output_size == 0) {
        return true;
      }
    }
  }
  return false;
}

bool ConstantFolding::IsSimplifiableReshape(
    const NodeDef& node, const GraphProperties& properties) const {
  if (!IsReshape(node)) {
    return false;
  }
  CHECK_LE(2, node.input_size());
  const NodeDef* new_shape = node_map_->GetNode(node.input(1));
  if (!IsConstant(*new_shape)) {
    return false;
  }
  TensorVector outputs;
  auto outputs_cleanup = gtl::MakeCleanup([&outputs] {
    for (const auto& output : outputs) {
      delete output.tensor;
    }
  });

  Status s = EvaluateNode(*new_shape, TensorVector(), &outputs);
  if (!s.ok()) {
    return false;
  }
  CHECK_EQ(1, outputs.size());

  const std::vector<OpInfo::TensorProperties>& props =
      properties.GetInputProperties(node.name());
  if (props.empty()) {
    return false;
  }
  const OpInfo::TensorProperties& prop = props[0];
  if (prop.dtype() == DT_INVALID) {
    return false;
  }
  const PartialTensorShape shape(prop.shape());
  if (!shape.IsFullyDefined()) {
    return false;
  }

  PartialTensorShape new_dims;
  if (outputs[0]->dtype() == DT_INT32) {
    std::vector<int32> shp;
    for (int i = 0; i < outputs[0]->NumElements(); ++i) {
      int32 dim = outputs[0]->flat<int32>()(i);
      shp.push_back(dim);
    }
    TF_CHECK_OK(TensorShapeUtils::MakeShape(shp, &new_dims));
  } else {
    std::vector<int64> shp;
    for (int i = 0; i < outputs[0]->NumElements(); ++i) {
      int64 dim = outputs[0]->flat<int64>()(i);
      shp.push_back(dim);
    }
    TF_CHECK_OK(TensorShapeUtils::MakeShape(shp, &new_dims));
  }

  return shape.IsCompatibleWith(new_dims);
}

Status ConstantFolding::SimplifyGraph(GraphDef* output,
                                      const GraphProperties& properties) {
  for (auto& node : *output->mutable_node()) {
    if (IsSimplifiableReduction(node)) {
      // Replace the reduction node with an identity node, that can be further
      // optimized by the model pruner.
      const NodeDef* reductions_indices = node_map_->GetNode(node.input(1));
      DataType output_type;
      if (node.attr().count("T") > 0) {
        output_type = node.attr().at("T").type();
      } else {
        // This is an 'any' or 'all' reduction. The output is always boolean.
        output_type = DT_BOOL;
      }
      node.set_op("Identity");
      node.clear_attr();
      (*node.mutable_attr())["T"].set_type(output_type);
      if (node.input_size() > 2) {
        node.mutable_input()->SwapElements(1, node.input_size() - 1);
      }
      node.mutable_input()->RemoveLast();
      for (const auto& input : reductions_indices->input()) {
        DCHECK(IsControlInput(input));
        *node.add_input() = input;
      }
    }
    // It's possible to feed a placeholder with a tensor that doesn't have the
    // proper shape, and reshape this tensor later on. Therefore only remove
    // reshapes in graphs that don't have placeholders.
    if (IsSimplifiableReshape(node, properties)) {
      const NodeDef* new_shape = node_map_->GetNode(node.input(1));
      DataType output_type = node.attr().at("T").type();
      node.set_op("Identity");
      node.clear_attr();
      (*node.mutable_attr())["T"].set_type(output_type);
      if (node.input_size() > 2) {
        node.mutable_input()->SwapElements(1, node.input_size() - 1);
      }
      node.mutable_input()->RemoveLast();
      for (const auto& input : new_shape->input()) {
        DCHECK(IsControlInput(input));
        *node.add_input() = input;
      }
    }
  }
  return Status::OK();
}

Status ConstantFolding::Optimize(Cluster* cluster, const GrapplerItem& item,
                                 GraphDef* output) {
  graph_ = item.graph;
  node_map_.reset(new NodeMap(&graph_));
  for (const auto& node : item.fetch) {
    nodes_to_preserve_.insert(NodeName(node));
  }
  for (const auto& node : item.feed) {
    nodes_to_preserve_.insert(NodeName(node.first));
  }
  device_.reset(new DeviceSimple());
  *output = GraphDef();

  bool has_feed = !item.feed.empty();

  GraphProperties properties(item);
  if (!has_feed) {
    // Only use static shape information when there is no feed in the
    // graph. That's because it's possible to feed a placeholder with a tensor
    // of any shape, which could make the static information inconsistent with
    // the shapes actually fed.
    Status s = properties.InferStatically();
    if (!s.ok()) {
      VLOG(1) << "Failed to infer graph shapes: " << s;
    } else {
      TF_RETURN_IF_ERROR(MaterializeShapes(item, properties));
    }
  }

  TF_RETURN_IF_ERROR(FoldGraph(output));

  if (!has_feed) {
    TF_RETURN_IF_ERROR(SimplifyGraph(output, properties));
  }

  *output->mutable_library() = item.graph.library();
  *output->mutable_versions() = item.graph.versions();

  return Status::OK();
}

void ConstantFolding::Feedback(Cluster* cluster, const GrapplerItem& item,
                               const GraphDef& optimize_output, double result) {
  // Nothing to do for ConstantFolding.
}

}  // end namespace grappler
}  // end namespace tensorflow
