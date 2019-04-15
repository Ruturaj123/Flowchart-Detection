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

#include "tensorflow/compiler/xla/service/service.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "tensorflow/compiler/xla/execution_options_util.h"
#include "tensorflow/compiler/xla/layout_util.h"
#include "tensorflow/compiler/xla/legacy_flags/debug_options_flags.h"
#include "tensorflow/compiler/xla/ptr_util.h"
#include "tensorflow/compiler/xla/service/compiler.h"
#include "tensorflow/compiler/xla/service/computation_layout.h"
#include "tensorflow/compiler/xla/service/device_memory_allocator.h"
#include "tensorflow/compiler/xla/service/executable.h"
#include "tensorflow/compiler/xla/service/hlo_computation.h"
#include "tensorflow/compiler/xla/service/hlo_cost_analysis.h"
#include "tensorflow/compiler/xla/service/hlo_evaluator.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_module.h"
#include "tensorflow/compiler/xla/service/hlo_module_config.h"
#include "tensorflow/compiler/xla/service/platform_util.h"
#include "tensorflow/compiler/xla/service/session.pb.h"
#include "tensorflow/compiler/xla/service/transfer_manager.h"
#include "tensorflow/compiler/xla/shape_layout.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/types.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/core/lib/gtl/cleanup.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/lib/strings/stringprintf.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/protobuf.h"
#include "tensorflow/core/platform/stream_executor_no_cuda.h"
#include "tensorflow/core/platform/types.h"

namespace se = ::perftools::gputools;

using ::tensorflow::strings::Printf;
using ::tensorflow::strings::StrCat;

namespace xla {

namespace {

// Copies the contents of an Allocation into a Literal proto.
tensorflow::Status LiteralFromAllocation(const Allocation* allocation,
                                         const Shape& literal_shape,
                                         Literal* literal) {
  TF_ASSIGN_OR_RETURN(
      se::StreamExecutor * executor,
      allocation->backend()->stream_executor(allocation->device_ordinal()));
  return allocation->backend()->transfer_manager()->TransferLiteralFromDevice(
      executor, allocation->device_memory(), allocation->shape(), literal_shape,
      literal);
}

// Records the arguments used to invoke a computation in a SessionModule
// proto.
tensorflow::Status RecordArguments(
    const tensorflow::gtl::ArraySlice<const Allocation*> arg_allocations,
    SessionModule* module) {
  module->clear_arguments();
  for (const Allocation* allocation : arg_allocations) {
    Literal argument;
    TF_RETURN_IF_ERROR(
        LiteralFromAllocation(allocation, allocation->shape(), &argument));
    *module->add_arguments() = argument.ToProto();
  }
  return tensorflow::Status::OK();
}

// Records the result of a computation in a SessionModule proto.
tensorflow::Status RecordResult(const Allocation* result_allocation,
                                SessionModule* module) {
  module->clear_result();
  Literal result;
  TF_RETURN_IF_ERROR(LiteralFromAllocation(
      result_allocation, result_allocation->shape(), &result));
  *module->mutable_result() = result.ToProto();
  return tensorflow::Status::OK();
}

}  // namespace

ServiceOptions& ServiceOptions::set_platform(
    perftools::gputools::Platform* platform) {
  platform_ = platform;
  return *this;
}

perftools::gputools::Platform* ServiceOptions::platform() const {
  return platform_;
}

ServiceOptions& ServiceOptions::set_number_of_replicas(int number_of_replicas) {
  number_of_replicas_ = number_of_replicas;
  return *this;
}

int ServiceOptions::number_of_replicas() const { return number_of_replicas_; }

ServiceOptions& ServiceOptions::set_intra_op_parallelism_threads(
    int num_threads) {
  intra_op_parallelism_threads_ = num_threads;
  return *this;
}

int ServiceOptions::intra_op_parallelism_threads() const {
  return intra_op_parallelism_threads_;
}

/* static */ StatusOr<std::unique_ptr<Service>> Service::NewService(
    perftools::gputools::Platform* platform) {
  ServiceOptions default_options;
  default_options.set_platform(platform);
  return NewService(default_options);
}

/* static */ StatusOr<std::unique_ptr<Service>> Service::NewService(
    const ServiceOptions& options) {
  perftools::gputools::Platform* platform = options.platform();
  std::unique_ptr<Backend> execute_backend;
  if (platform == nullptr) {
    TF_ASSIGN_OR_RETURN(platform, PlatformUtil::GetDefaultPlatform());
  }
  BackendOptions backend_options;
  backend_options.set_platform(platform);
  TF_ASSIGN_OR_RETURN(execute_backend, Backend::CreateBackend(backend_options));

  std::unique_ptr<Service> service(
      new Service(options, std::move(execute_backend)));
  return std::move(service);
}

Service::Service(const ServiceOptions& options,
                 std::unique_ptr<Backend> execute_backend)
    : options_(options), execute_backend_(std::move(execute_backend)) {
  CHECK(options_.number_of_replicas() > 0);
  if (execute_backend_) {
    if (execute_backend_->device_count() > 0) {
      CHECK_GE(execute_backend_->device_count(), options_.number_of_replicas())
          << "Requested more replicas than there are devices.";
    }
    LOG(INFO) << Printf(
        "XLA service %p executing computations on platform %s. Devices:", this,
        execute_backend_->platform()->Name().c_str());
    for (int i = 0; i < execute_backend_->device_count(); ++i) {
      if (execute_backend_->device_ordinal_supported(i)) {
        se::StreamExecutor* executor =
            execute_backend_->stream_executor(i).ValueOrDie();
        const auto& description = executor->GetDeviceDescription();
        LOG(INFO) << Printf("  StreamExecutor device (%d): %s, %s", i,
                            description.name().c_str(),
                            description.platform_version().c_str());
      } else {
        LOG(INFO) << Printf("  StreamExecutor device (%d) not supported", i);
      }
    }
  } else {
    VLOG(1) << "XLA compile-only service constructed";
  }
}

tensorflow::Status Service::Computation(const ComputationRequest* arg,
                                        ComputationResponse* result) {
  if (arg->name().empty()) {
    return InvalidArgument("computation request needs a name");
  }

  *result->mutable_computation() =
      computation_tracker_.NewComputation(arg->name());
  VLOG(1) << Printf("Created new computation %s on service %p",
                    result->computation().ShortDebugString().c_str(), this);
  return tensorflow::Status::OK();
}

tensorflow::Status Service::CreateChannelHandle(
    const CreateChannelHandleRequest* arg,
    CreateChannelHandleResponse* result) {
  *result->mutable_channel() = channel_tracker_.NewChannel();
  return tensorflow::Status::OK();
}

tensorflow::Status Service::Unregister(const UnregisterRequest* arg,
                                       UnregisterResponse* result) {
  return allocation_tracker_.Unregister(arg->data());
}

// Deconstructs a previously-allocated global handle.
tensorflow::Status Service::DeconstructTuple(const DeconstructTupleRequest* arg,
                                             DeconstructTupleResponse* result) {
  TF_ASSIGN_OR_RETURN(
      std::vector<GlobalDataHandle> elements,
      allocation_tracker_.DeconstructTuple(arg->tuple_handle()));

  for (auto& element : elements) {
    *result->add_element_handles() = element;
  }
  return tensorflow::Status::OK();
}

tensorflow::Status Service::ValidateResultShapeWithLayout(
    const Shape& shape_with_layout, const Shape& result_shape) const {
  if (!ShapeUtil::Compatible(shape_with_layout, result_shape)) {
    return InvalidArgument(
        "Shape used to set computation result layout %s is not compatible "
        "with result shape %s",
        ShapeUtil::HumanStringWithLayout(shape_with_layout).c_str(),
        ShapeUtil::HumanString(result_shape).c_str());
  }
  if (!LayoutUtil::HasLayout(shape_with_layout)) {
    return InvalidArgument(
        "Shape used to set computation result layout %s does not have layout",
        ShapeUtil::HumanStringWithLayout(shape_with_layout).c_str());
  }
  return ShapeUtil::ValidateShape(shape_with_layout);
}

StatusOr<std::vector<const Allocation*>> Service::ResolveAndValidateArguments(
    tensorflow::gtl::ArraySlice<const GlobalDataHandle*> arguments,
    const Backend* backend, int device_ordinal) {
  std::vector<const Allocation*> allocations;
  for (size_t i = 0; i < arguments.size(); ++i) {
    auto allocation_status = allocation_tracker_.Resolve(*arguments[i]);
    if (!allocation_status.ok()) {
      return Status(allocation_status.status().code(),
                    StrCat(allocation_status.status().error_message(), ", ",
                           "failed to resolve allocation for parameter ", i));
    }
    const Allocation* allocation = allocation_status.ValueOrDie();

    // Verify allocation is same platform and device as the execution.
    if (allocation->backend() != backend ||
        allocation->device_ordinal() != device_ordinal) {
      return InvalidArgument(
          "argument %lu is on device %s but computation will be executed "
          "on device %s",
          i,
          allocation->backend()
              ->device_name(allocation->device_ordinal())
              .c_str(),
          backend->device_name(device_ordinal).c_str());
    }

    allocations.push_back(allocation);
  }
  return allocations;
}

StatusOr<std::unique_ptr<HloModuleConfig>> Service::CreateModuleConfig(
    const ProgramShape& program_shape,
    tensorflow::gtl::ArraySlice<const Shape*> argument_shapes,
    const ExecutionOptions* execution_options, bool has_hybrid_result) {
  auto config = MakeUnique<HloModuleConfig>(program_shape);
  auto* computation_layout = config->mutable_entry_computation_layout();

  if (program_shape.parameters_size() != argument_shapes.size()) {
    return InvalidArgument("computation takes %d parameters, but %zu given",
                           program_shape.parameters_size(),
                           argument_shapes.size());
  }
  for (int i = 0; i < argument_shapes.size(); ++i) {
    // Verify that shape of arguments matches the shape of the arguments in the
    // ProgramShape.
    if (!ShapeUtil::Compatible(*argument_shapes[i],
                               program_shape.parameters(i))) {
      return InvalidArgument(
          "computation expects parameter %d to have shape %s, given shape %s",
          i, ShapeUtil::HumanString(program_shape.parameters(i)).c_str(),
          ShapeUtil::HumanString(*argument_shapes[i]).c_str());
    }
    TF_RETURN_IF_ERROR(
        computation_layout->mutable_parameter_layout(i)->CopyLayoutFromShape(
            *argument_shapes[i]));
  }
  if (execution_options != nullptr &&
      execution_options->has_shape_with_output_layout()) {
    const auto& shape_with_output_layout =
        execution_options->shape_with_output_layout();
    TF_RETURN_IF_ERROR(ValidateResultShapeWithLayout(shape_with_output_layout,
                                                     program_shape.result()));
    TF_RETURN_IF_ERROR(
        computation_layout->mutable_result_layout()->CopyLayoutFromShape(
            shape_with_output_layout));
  } else {
    computation_layout->mutable_result_layout()->Clear();
  }

  config->set_replica_count(options_.number_of_replicas());
  config->set_has_hybrid_result(has_hybrid_result);
  if (execution_options != nullptr) {
    config->set_seed(execution_options->seed());
    config->set_debug_options(execution_options->debug_options());
    config->enable_hlo_profiling(
        execution_options->debug_options().xla_hlo_profile());
  } else {
    config->set_debug_options(legacy_flags::GetDebugOptionsFromFlags());
  }

  if (execute_backend_ != nullptr &&
      execute_backend_->eigen_intra_op_thread_pool() != nullptr) {
    config->set_intra_op_parallelism_threads(
        execute_backend_->eigen_intra_op_thread_pool()->NumThreads());
  }
  return std::move(config);
}

StatusOr<std::unique_ptr<HloModuleConfig>> Service::CreateModuleConfig(
    const ProgramShape& program_shape,
    tensorflow::gtl::ArraySlice<const Allocation*> arguments,
    const ExecutionOptions& execution_options) {
  std::vector<const Shape*> argument_shapes;
  for (const auto* arg : arguments) {
    argument_shapes.push_back(&arg->shape());
  }
  return CreateModuleConfig(program_shape, argument_shapes, &execution_options);
}

StatusOr<std::vector<std::unique_ptr<Executable>>> Service::BuildExecutables(
    std::vector<VersionedComputationHandle> versioned_handles,
    std::vector<std::unique_ptr<HloModuleConfig>> module_configs,
    Backend* backend,
    std::vector<perftools::gputools::StreamExecutor*> executors) {
  VLOG(1) << Printf("BuildExecutable on service %p", this);

  // Dump computation proto state if flag is set.
  std::vector<std::unique_ptr<SessionModule>> session_modules;
  for (int64 i = 0; i < versioned_handles.size(); ++i) {
    const string& directory_path =
        module_configs[i]->debug_options().xla_dump_computations_to();
    const string& other_directory_path =
        module_configs[i]->debug_options().xla_dump_executions_to();
    if (directory_path.empty() && other_directory_path.empty()) {
      continue;
    }
    TF_ASSIGN_OR_RETURN(
        std::unique_ptr<SessionModule> session_module,
        computation_tracker_.SnapshotComputation(versioned_handles[i].handle));
    if (!directory_path.empty()) {
      string filename = Printf("computation_%lld__%s__version_%lld",
                               versioned_handles[i].handle.handle(),
                               session_module->entry().name().c_str(),
                               versioned_handles[i].version);
      TF_RETURN_IF_ERROR(Executable::DumpToDirectory(directory_path, filename,
                                                     *session_module));
      session_modules.push_back(std::move(session_module));
    }
  }

  VLOG(1) << "Computation handles:";
  for (const VersionedComputationHandle& versioned_handle : versioned_handles) {
    VLOG(1) << versioned_handle;
  }

  CHECK_EQ(versioned_handles.size(), module_configs.size());
  std::vector<std::unique_ptr<HloModule>> modules;
  for (int64 i = 0; i < versioned_handles.size(); ++i) {
    const VersionedComputationHandle& versioned_handle = versioned_handles[i];
    const HloModuleConfig& config = *module_configs[i];
    TF_ASSIGN_OR_RETURN(auto module,
                        computation_tracker_.BuildHloModule(
                            versioned_handle, config,
                            /*include_unreachable_instructions=*/true));
    modules.push_back(std::move(module));
  }

  TF_ASSIGN_OR_RETURN(
      std::vector<std::unique_ptr<Executable>> executables,
      backend->compiler()->Compile(std::move(modules), std::move(executors)));

  for (size_t i = 0; i < versioned_handles.size(); ++i) {
    if (!module_configs[i]->debug_options().xla_dump_executions_to().empty()) {
      executables[i]->set_session_module(std::move(session_modules[i]));
    }
  }

  return std::move(executables);
}

StatusOr<std::unique_ptr<Executable>> Service::BuildExecutable(
    const VersionedComputationHandle& versioned_handle,
    std::unique_ptr<HloModuleConfig> module_config,
    const tensorflow::gtl::ArraySlice<perftools::gputools::DeviceMemoryBase>
        arguments,
    Backend* backend, se::StreamExecutor* executor) {
  VLOG(1) << Printf("BuildExecutable on service %p with handle %s", this,
                    versioned_handle.ToString().c_str());

  // Dump computation proto state if flag is set.
  std::unique_ptr<SessionModule> session_module;
  const string& directory_path =
      module_config->debug_options().xla_dump_computations_to();
  const string& other_directory_path =
      module_config->debug_options().xla_dump_executions_to();
  if (!directory_path.empty() || !other_directory_path.empty()) {
    TF_ASSIGN_OR_RETURN(
        session_module,
        computation_tracker_.SnapshotComputation(versioned_handle.handle));
    if (!directory_path.empty()) {
      string filename = Printf("computation_%lld__%s__version_%lld",
                               versioned_handle.handle.handle(),
                               session_module->entry().name().c_str(),
                               versioned_handle.version);
      TF_RETURN_IF_ERROR(Executable::DumpToDirectory(directory_path, filename,
                                                     *session_module));
    }
  }

  TF_ASSIGN_OR_RETURN(
      std::unique_ptr<HloModule> module,
      computation_tracker_.BuildHloModule(versioned_handle, *module_config,
                                          /*include_unreachable_instructions=*/
                                          true));

  TF_ASSIGN_OR_RETURN(
      std::unique_ptr<Executable> executable,
      backend->compiler()->Compile(std::move(module), executor));

  if (!other_directory_path.empty()) {
    executable->set_session_module(std::move(session_module));
  }

  return std::move(executable);
}

StatusOr<std::shared_ptr<Executable>> Service::BuildAndCacheExecutable(
    const VersionedComputationHandle& versioned_handle,
    std::unique_ptr<HloModuleConfig> module_config,
    const tensorflow::gtl::ArraySlice<perftools::gputools::DeviceMemoryBase>
        arguments,
    Backend* backend, perftools::gputools::StreamExecutor* executor,
    ExecutionProfile* profile) {
  std::shared_ptr<Executable> executable =
      compilation_cache_.LookUp(versioned_handle, *module_config);

  if (executable != nullptr) {
    // Executable found in the computation cache.
    if (profile != nullptr) {
      profile->set_compilation_cache_hit(true);
    }
    return executable;
  }

  uint64 start_micros =
      // Avoid reading the clock if we don't want timing info
      (profile != nullptr) ? tensorflow::Env::Default()->NowMicros() : 0;

  // Take a copy of the module config, as compilation introduces layouts where
  // layouts were optional before.
  HloModuleConfig original_module_config = *module_config;
  TF_ASSIGN_OR_RETURN(
      std::unique_ptr<Executable> executable_unique_ptr,
      BuildExecutable(versioned_handle, std::move(module_config), arguments,
                      backend, executor));

  if (profile != nullptr) {
    uint64 end_micros = tensorflow::Env::Default()->NowMicros();
    uint64 milliseconds = (end_micros - start_micros) / 1000;
    profile->set_compilation_cache_hit(false);
    profile->set_compile_time_ms(milliseconds);
  }

  // Insert executable into the cache.
  return compilation_cache_.Insert(std::move(executable_unique_ptr),
                                   original_module_config);
}

StatusOr<std::vector<GlobalDataHandle>>
Service::ExecuteParallelAndRegisterResult(
    tensorflow::gtl::ArraySlice<Executable*> executables,
    tensorflow::gtl::ArraySlice<
        std::vector<perftools::gputools::DeviceMemoryBase>>
        arguments,
    Backend* backend, tensorflow::gtl::ArraySlice<DeviceHandle> device_handles,
    tensorflow::gtl::ArraySlice<string> result_tags) {
  // Streams where the computation are launched, so we can wait on the streams
  // to complete.
  std::vector<Pool<se::Stream>::SmartPtr> streams;

  // Global data handles for the computation results, one for each computation.
  std::vector<GlobalDataHandle> result_handles;

  TF_ASSIGN_OR_RETURN(DeviceAssignment device_assignment,
                      backend->computation_placer()->AssignDevices(
                          options_.number_of_replicas(), executables.size()));

  for (int64 i = 0; i < executables.size(); i++) {
    // Stream executors for the replicas of the current computation.
    TF_ASSIGN_OR_RETURN(auto replicas, Replicas(*backend, device_handles[i]));
    for (int64 replica = 0; replica < replicas.size(); ++replica) {
      TF_ASSIGN_OR_RETURN(Pool<se::Stream>::SmartPtr stream,
                          backend->BorrowStream(replicas[replica]));
      streams.push_back(std::move(stream));

      // Set up run options.
      ExecutableRunOptions options;
      options.set_stream(streams.back().get());
      options.set_allocator(backend->memory_allocator());
      options.set_inter_op_thread_pool(backend->inter_op_thread_pool());
      options.set_intra_op_thread_pool(
          backend->eigen_intra_op_thread_pool_device());
      options.set_device_assignment(&device_assignment);
      ServiceExecutableRunOptions run_options(options,
                                              backend->StreamBorrower());

      // Asynchronously launch the computation.
      TF_ASSIGN_OR_RETURN(
          perftools::gputools::DeviceMemoryBase result,
          executables[i]->ExecuteAsyncOnStream(&run_options, arguments[i]));

      // All replicas share the same device address for the result allocation,
      // so only one of the replicas need to register the result handle.
      if (replica == 0) {
        result_handles.push_back(allocation_tracker_.Register(
            backend, replicas[0]->device_ordinal(), result,
            executables[i]->result_shape(), result_tags[i]));
      }
    }
  }

  // Wait for all executions to complete.
  for (int64 i = 0; i < streams.size(); ++i) {
    if (!streams[i]->BlockHostUntilDone()) {
      return InternalError("failed to complete execution for stream %lld", i);
    }
  }

  return result_handles;
}

StatusOr<GlobalDataHandle> Service::ExecuteAndRegisterResult(
    Executable* executable,
    const tensorflow::gtl::ArraySlice<perftools::gputools::DeviceMemoryBase>
        arguments,
    Backend* backend, perftools::gputools::StreamExecutor* executor,
    const string& result_tag, ExecutionProfile* profile) {
  // Set up streams.
  std::vector<Pool<se::Stream>::SmartPtr> streams;

  TF_ASSIGN_OR_RETURN(auto replicas,
                      Replicas(*backend, SingleComputationDeviceHandle()));
  TF_RET_CHECK(!replicas.empty());
  for (se::StreamExecutor* executor : replicas) {
    TF_ASSIGN_OR_RETURN(Pool<se::Stream>::SmartPtr stream,
                        backend->BorrowStream(executor));
    streams.push_back(std::move(stream));
  }

  TF_ASSIGN_OR_RETURN(DeviceAssignment device_assignment,
                      backend->computation_placer()->AssignDevices(
                          options_.number_of_replicas(),
                          /*computation_count=*/1));

  // Set up run options.
  std::vector<ServiceExecutableRunOptions> run_options;
  for (const Pool<se::Stream>::SmartPtr& stream : streams) {
    ExecutableRunOptions options;
    options.set_stream(stream.get());
    options.set_allocator(backend->memory_allocator());
    options.set_inter_op_thread_pool(backend->inter_op_thread_pool());
    options.set_intra_op_thread_pool(
        backend->eigen_intra_op_thread_pool_device());
    options.set_device_assignment(&device_assignment);
    run_options.emplace_back(options, backend->StreamBorrower(),
                             backend->inter_op_thread_pool());
  }

  perftools::gputools::DeviceMemoryBase result;
  if (options_.number_of_replicas() == 1) {
    TF_ASSIGN_OR_RETURN(
        result, executable->ExecuteOnStreamWrapper<se::DeviceMemoryBase>(
                    &run_options[0], profile, arguments));
  } else {
    std::vector<
        tensorflow::gtl::ArraySlice<perftools::gputools::DeviceMemoryBase>>
        repeated_arguments(options_.number_of_replicas(), arguments);

    TF_ASSIGN_OR_RETURN(auto results, executable->ExecuteOnStreams(
                                          run_options, repeated_arguments));
    TF_RET_CHECK(!results.empty());
    result = results[0];
  }
  return allocation_tracker_.Register(backend, executor->device_ordinal(),
                                      result, executable->result_shape(),
                                      result_tag);
}

tensorflow::Status Service::SetReturnValue(const SetReturnValueRequest* arg,
                                           SetReturnValueResponse* results) {
  TF_ASSIGN_OR_RETURN(UserComputation * computation,
                      computation_tracker_.Resolve(arg->computation()));
  return computation->SetReturnValue(arg->operand());
}

tensorflow::Status Service::ExecuteParallel(const ExecuteParallelRequest* arg,
                                            ExecuteParallelResponse* result) {
  VLOG(1) << "running execute-parallel request: " << arg->ShortDebugString();

  std::vector<std::vector<se::DeviceMemoryBase>> all_arguments;
  std::vector<perftools::gputools::StreamExecutor*> executors;
  std::vector<VersionedComputationHandle> versioned_handles;
  std::vector<std::unique_ptr<HloModuleConfig>> module_configs;
  std::vector<string> computation_names;
  std::vector<DeviceHandle> device_handles;

  if (arg->requests_size() * options_.number_of_replicas() >
      execute_backend_->device_count()) {
    return FailedPrecondition(
        "there are not enough stream executors to execute %d computations",
        arg->requests_size());
  }

  for (int64 i = 0; i < arg->requests_size(); ++i) {
    // Get the stream executor for the i'th computation. This stream executor
    // is one of the executors to run the replicated computation.
    if (!arg->requests(i).has_device_handle()) {
      return FailedPrecondition(
          "device handles must be given to execute parallel computations");
    }
    TF_ASSIGN_OR_RETURN(
        auto replicas,
        Replicas(*execute_backend_, arg->requests(i).device_handle()));
    se::StreamExecutor* executor = replicas[0];
    CHECK(executor != nullptr);

    // Resolve the UserComputation object associated with the requested
    // computation and compute the program shape.
    const ExecuteRequest& request = arg->requests(i);
    TF_ASSIGN_OR_RETURN(UserComputation * user_computation,
                        computation_tracker_.Resolve(request.computation()));
    VersionedComputationHandle versioned_handle =
        user_computation->GetVersionedHandle();
    if (user_computation->request_count(versioned_handle.version) == 0) {
      return InvalidArgument("computations may not be empty");
    }

    TF_ASSIGN_OR_RETURN(
        std::shared_ptr<const ProgramShape> program_shape,
        user_computation->ComputeProgramShape(versioned_handle.version));

    // Resolve the allocations for the arguments of the computation, and create
    // a vector of device memory offsets for the arguments from the allocations.
    TF_ASSIGN_OR_RETURN(
        std::vector<const Allocation*> arg_allocations,
        ResolveAndValidateArguments(request.arguments(), execute_backend_.get(),
                                    executor->device_ordinal()));
    std::vector<se::DeviceMemoryBase> arguments;
    arguments.reserve(arg_allocations.size());
    for (const Allocation* allocation : arg_allocations) {
      arguments.push_back(allocation->device_memory());
    }

    // Create an HloModuleConfig object for the computation, given the shape of
    // the program and the argument allocations.
    TF_ASSIGN_OR_RETURN(std::unique_ptr<HloModuleConfig> module_config,
                        CreateModuleConfig(*program_shape, arg_allocations,
                                           request.execution_options()));
    VLOG(3) << "ExecuteParallel created HloModuleConfig computation layout: "
            << module_config->entry_computation_layout().ToString();

    // Adds to the vectors to build and execute the computations after the loop.
    all_arguments.push_back(arguments);
    versioned_handles.push_back(versioned_handle);
    module_configs.push_back(std::move(module_config));
    computation_names.push_back(user_computation->name());
    executors.push_back(executor);
    device_handles.push_back(arg->requests(i).device_handle());
  }

  // Build the user computations into HloModules and compile to generate the
  // executables.
  TF_ASSIGN_OR_RETURN(
      std::vector<std::unique_ptr<Executable>> executables,
      BuildExecutables(versioned_handles, std::move(module_configs),
                       execute_backend_.get(), executors));
  std::vector<Executable*> executable_ptrs;
  executable_ptrs.reserve(executables.size());
  for (const auto& executable : executables) {
    executable_ptrs.push_back(executable.get());
  }

  // Execute the generated executables in parallel and return the device
  // handles for each computation's output.
  TF_ASSIGN_OR_RETURN(
      std::vector<GlobalDataHandle> outputs,
      ExecuteParallelAndRegisterResult(executable_ptrs, all_arguments,
                                       execute_backend_.get(), device_handles,
                                       computation_names));
  for (const GlobalDataHandle& output : outputs) {
    ExecuteResponse response;
    *response.mutable_output() = output;
    *result->add_responses() = response;
  }

  VLOG(1) << "successfully completed 'execute-parallel' request";
  return tensorflow::Status::OK();
}

tensorflow::Status Service::GetDeviceHandles(const GetDeviceHandlesRequest* arg,
                                             GetDeviceHandlesResponse* result) {
  const int64 available_device_count = execute_backend_->device_count();
  const int64 replica_count = options_.number_of_replicas();
  if (replica_count <= 0) {
    return FailedPrecondition("Replica count must be a positive integer");
  }
  if (available_device_count < arg->device_count() * replica_count) {
    return ResourceExhausted(
        "Requested device count (%lld) exceeds the number of available devices "
        "on the target (%lld)",
        arg->device_count(), available_device_count);
  }

  for (int64 i = 0; i < arg->device_count(); ++i) {
    DeviceHandle device_handle;
    device_handle.set_handle(i);
    device_handle.set_device_count(arg->device_count());
    *result->add_device_handles() = device_handle;
  }

  return tensorflow::Status::OK();
}

tensorflow::Status Service::Execute(const ExecuteRequest* arg,
                                    ExecuteResponse* result) {
  VLOG(1) << "running execute request: " << arg->ShortDebugString();

  TF_ASSIGN_OR_RETURN(UserComputation * user_computation,
                      computation_tracker_.Resolve(arg->computation()));

  VersionedComputationHandle versioned_handle =
      user_computation->GetVersionedHandle();

  if (user_computation->request_count(versioned_handle.version) == 0) {
    return InvalidArgument("computations may not be empty");
  }

  TF_ASSIGN_OR_RETURN(
      std::shared_ptr<const ProgramShape> program_shape,
      user_computation->ComputeProgramShape(versioned_handle.version));

  TF_ASSIGN_OR_RETURN(
      std::vector<const Allocation*> arg_allocations,
      ResolveAndValidateArguments(arg->arguments(), execute_backend_.get(),
                                  execute_backend_->default_device_ordinal()));

  TF_ASSIGN_OR_RETURN(std::unique_ptr<HloModuleConfig> module_config,
                      CreateModuleConfig(*program_shape, arg_allocations,
                                         arg->execution_options()));

  VLOG(3) << "Execute created HloModuleConfig computation layout: "
          << module_config->entry_computation_layout().ToString();

  std::vector<se::DeviceMemoryBase> arguments;
  arguments.reserve(arg_allocations.size());
  for (const Allocation* allocation : arg_allocations) {
    arguments.push_back(allocation->device_memory());
  }

  TF_ASSIGN_OR_RETURN(
      std::shared_ptr<Executable> executable,
      BuildAndCacheExecutable(versioned_handle, std::move(module_config),
                              arguments, execute_backend_.get(),
                              execute_backend_->default_stream_executor(),
                              result->mutable_profile()));

  if (executable->dumping()) {
    executable->session_module()->set_execution_platform(
        execute_backend_->platform()->Name());
    TF_RETURN_IF_ERROR(
        RecordArguments(arg_allocations, executable->session_module()));
  }

  TF_ASSIGN_OR_RETURN(
      *result->mutable_output(),
      ExecuteAndRegisterResult(
          executable.get(), arguments, execute_backend_.get(),
          execute_backend_->default_stream_executor(),
          "result of " + user_computation->name(), result->mutable_profile()));

  if (executable->dumping()) {
    TF_ASSIGN_OR_RETURN(const Allocation* result_allocation,
                        allocation_tracker_.Resolve(result->output()));
    TF_RETURN_IF_ERROR(
        RecordResult(result_allocation, executable->session_module()));
    TF_RETURN_IF_ERROR(executable->DumpSessionModule());
  }

  VLOG(1) << "successfully completed 'execute' request";
  return tensorflow::Status::OK();
}

tensorflow::Status Service::ExecuteAsync(const ExecuteAsyncRequest* arg,
                                         ExecuteAsyncResponse* result) {
  VLOG(1) << "running execute-async request: " << arg->ShortDebugString();

  TF_ASSIGN_OR_RETURN(UserComputation * user_computation,
                      computation_tracker_.Resolve(arg->computation()));

  VersionedComputationHandle versioned_handle =
      user_computation->GetVersionedHandle();
  if (user_computation->request_count(versioned_handle.version) == 0) {
    return InvalidArgument("computations may not be empty");
  }

  TF_ASSIGN_OR_RETURN(
      std::shared_ptr<const ProgramShape> program_shape,
      user_computation->ComputeProgramShape(versioned_handle.version));

  TF_ASSIGN_OR_RETURN(
      std::vector<const Allocation*> arg_allocations,
      ResolveAndValidateArguments(arg->arguments(), execute_backend_.get(),
                                  execute_backend_->default_device_ordinal()));

  TF_ASSIGN_OR_RETURN(std::unique_ptr<HloModuleConfig> module_config,
                      CreateModuleConfig(*program_shape, arg_allocations,
                                         arg->execution_options()));

  VLOG(3) << "ExecuteAsync created HloModuleConfig computation layout: "
          << module_config->entry_computation_layout().ToString();

  std::vector<se::DeviceMemoryBase> arguments;
  arguments.reserve(arg_allocations.size());
  for (const Allocation* allocation : arg_allocations) {
    arguments.push_back(allocation->device_memory());
  }

  ExecutionProfile profile;

  TF_ASSIGN_OR_RETURN(
      std::shared_ptr<Executable> executable,
      BuildAndCacheExecutable(versioned_handle, std::move(module_config),
                              arguments, execute_backend_.get(),
                              execute_backend_->default_stream_executor(),
                              &profile));

  TF_ASSIGN_OR_RETURN(auto replicas, Replicas(*execute_backend_,
                                              SingleComputationDeviceHandle()));
  TF_RET_CHECK(!replicas.empty());

  // Set up streams.
  std::vector<Pool<se::Stream>::SmartPtr> streams;

  for (se::StreamExecutor* executor : replicas) {
    TF_ASSIGN_OR_RETURN(Pool<se::Stream>::SmartPtr stream,
                        execute_backend_->BorrowStream(executor));
    streams.push_back(std::move(stream));
  }

  perftools::gputools::DeviceMemoryBase result_data;
  for (const Pool<se::Stream>::SmartPtr& stream : streams) {
    ExecutableRunOptions options;
    options.set_stream(stream.get());
    options.set_allocator(execute_backend_->memory_allocator());
    options.set_inter_op_thread_pool(execute_backend_->inter_op_thread_pool());
    options.set_intra_op_thread_pool(
        execute_backend_->eigen_intra_op_thread_pool_device());

    ServiceExecutableRunOptions service_options(
        options, execute_backend_->StreamBorrower());

    TF_ASSIGN_OR_RETURN(
        perftools::gputools::DeviceMemoryBase this_result_data,
        executable->ExecuteAsyncOnStream(&service_options, arguments));

    // Take the first result.
    if (result_data == nullptr) {
      result_data = this_result_data;
    }
  }

  auto output = allocation_tracker_.Register(
      execute_backend_.get(), execute_backend_->default_device_ordinal(),
      result_data, executable->result_shape(),
      "result of " + user_computation->name());

  *result->mutable_execution() = execution_tracker_.Register(
      execute_backend_.get(), std::move(streams), profile, output);
  streams.clear();

  VLOG(1) << "successfully completed 'execute-async' request";
  return tensorflow::Status::OK();
}

tensorflow::Status Service::WaitForExecution(const WaitForExecutionRequest* arg,
                                             WaitForExecutionResponse* result) {
  TF_ASSIGN_OR_RETURN(const auto execution,
                      execution_tracker_.Resolve(arg->execution()));

  TF_RETURN_IF_ERROR(execution->BlockUntilDone());

  *result->mutable_output() = execution->result();
  *result->mutable_profile() = execution->profile();

  TF_RETURN_IF_ERROR(execution_tracker_.Unregister(arg->execution()));
  VLOG(1) << "successfully completed 'wait-for-execution' request";
  return tensorflow::Status::OK();
}

tensorflow::Status Service::TransferToClient(const TransferToClientRequest* arg,
                                             TransferToClientResponse* result) {
  TF_ASSIGN_OR_RETURN(const Allocation* allocation,
                      allocation_tracker_.Resolve(arg->data()));

  const Shape* literal_shape;
  if (arg->has_shape_with_layout()) {
    if (!LayoutUtil::HasLayout(arg->shape_with_layout())) {
      return InvalidArgument("shape_with_layout must have layout if present.");
    }
    literal_shape = &arg->shape_with_layout();
  } else {
    literal_shape = &allocation->shape();
  }

  Literal literal;
  auto status = LiteralFromAllocation(allocation, *literal_shape, &literal);
  *result->mutable_literal() = literal.ToProto();
  return status;
}

tensorflow::Status Service::TransferToServer(const TransferToServerRequest* arg,
                                             TransferToServerResponse* result) {
  Literal literal = Literal(arg->literal());
  const Shape& shape = literal.shape();

  if (ShapeUtil::IsTuple(shape) && options_.number_of_replicas() > 1) {
    // TODO(b/32990684): Tuple transfers to host end up allocating further
    // buffers - implement that correctly.
    return Unimplemented(
        "Tuple transfers to the device not supported with replication.");
  }

  std::vector<se::StreamExecutor*> replicas;
  if (arg->has_device_handle()) {
    TF_ASSIGN_OR_RETURN(replicas,
                        Replicas(*execute_backend_, arg->device_handle()));
  } else {
    TF_ASSIGN_OR_RETURN(
        replicas, Replicas(*execute_backend_, SingleComputationDeviceHandle()));
  }

  // Allocate memory on the device, using the stream executor. The size of the
  // allocation is obtained by examining the shape of the literal passed from
  // the client. An allocation handle is returned in the response.
  int64 allocation_size =
      execute_backend_->transfer_manager()->GetByteSizeRequirement(shape);

  TF_ASSIGN_OR_RETURN(se::DeviceMemoryBase allocation,
                      execute_backend_->memory_allocator()->Allocate(
                          replicas[0]->device_ordinal(), allocation_size));

  *result->mutable_data() = allocation_tracker_.Register(
      execute_backend_.get(), replicas[0]->device_ordinal(), allocation, shape,
      StrCat("TransferToServer literal of size ", allocation_size));

  for (se::StreamExecutor* executor : replicas) {
    TF_RETURN_IF_ERROR(
        execute_backend_->transfer_manager()->TransferLiteralToDevice(
            executor, literal, &allocation));
  }
  return tensorflow::Status::OK();
}

tensorflow::Status Service::TransferToInfeed(const TransferToInfeedRequest* arg,
                                             TransferToInfeedResponse* result) {
  const int64 replica_count = options_.number_of_replicas();
  if (arg->replica_id() < 0 || arg->replica_id() >= replica_count) {
    return FailedPrecondition(
        "%s",
        StrCat("The replica_id=", arg->replica_id(),
               " on TransferToInfeedRequest not in range [0, replica_count=",
               replica_count, ").")
            .c_str());
  }

  se::StreamExecutor* executor;
  if (arg->has_device_handle()) {
    TF_ASSIGN_OR_RETURN(auto replicas,
                        Replicas(*execute_backend_, arg->device_handle()));
    executor = replicas[arg->replica_id()];
  } else {
    TF_ASSIGN_OR_RETURN(
        auto replicas,
        Replicas(*execute_backend_, SingleComputationDeviceHandle()));
    executor = replicas[arg->replica_id()];
  }

  return execute_backend_->transfer_manager()->TransferLiteralToInfeed(
      executor, Literal(arg->literal()));
}

tensorflow::Status Service::TransferFromOutfeed(
    const TransferFromOutfeedRequest* arg,
    TransferFromOutfeedResponse* result) {
  const int64 replica_count = options_.number_of_replicas();
  if (arg->replica_id() < 0 || arg->replica_id() >= replica_count) {
    return FailedPrecondition(
        "The replica_id=%lld on TransferFromOutfeedRequest not in range [0, "
        "%lld)",
        arg->replica_id(), replica_count);
  }

  se::StreamExecutor* executor;
  if (arg->has_device_handle()) {
    TF_ASSIGN_OR_RETURN(auto replicas,
                        Replicas(*execute_backend_, arg->device_handle()));
    executor = replicas[arg->replica_id()];
  } else {
    TF_ASSIGN_OR_RETURN(
        auto replicas,
        Replicas(*execute_backend_, SingleComputationDeviceHandle()));
    executor = replicas[arg->replica_id()];
  }

  Literal literal;
  TF_RETURN_IF_ERROR(
      execute_backend_->transfer_manager()->TransferLiteralFromOutfeed(
          executor, arg->shape_with_layout(), &literal));
  *result->mutable_literal() = literal.ToProto();
  return tensorflow::Status::OK();
}

tensorflow::Status Service::ResetDevice(const ResetDeviceRequest* arg,
                                        ResetDeviceResponse* result) {
  return execute_backend_->ResetDevices();
}

tensorflow::Status Service::IsConstant(const IsConstantRequest* arg,
                                       IsConstantResponse* result) {
  TF_ASSIGN_OR_RETURN(UserComputation * user_computation,
                      computation_tracker_.Resolve(arg->computation()));

  VersionedComputationHandle versioned_handle =
      user_computation->GetVersionedHandleAtOperation(arg->operand());

  if (user_computation->request_count(versioned_handle.version) == 0) {
    return InvalidArgument("computations may not be empty");
  }

  TF_ASSIGN_OR_RETURN(bool is_constant,
                      user_computation->IsConstant(arg->operand()));

  result->set_is_constant(is_constant);
  return tensorflow::Status::OK();
}

tensorflow::Status Service::ComputeConstant(const ComputeConstantRequest* arg,
                                            ComputeConstantResponse* result) {
  TF_ASSIGN_OR_RETURN(UserComputation * user_computation,
                      computation_tracker_.Resolve(arg->computation()));

  VersionedComputationHandle versioned_handle =
      user_computation->GetVersionedHandleAtOperation(arg->operand());

  if (user_computation->request_count(versioned_handle.version) == 0) {
    return InvalidArgument("computations may not be empty");
  }

  TF_ASSIGN_OR_RETURN(bool is_constant,
                      user_computation->IsConstant(arg->operand()));
  if (!is_constant) {
    return InvalidArgument("Operand to ComputeConstant depends on parameter.");
  }

  // We can't use ComputeProgramShape because it checks that all parameter
  // instructions are present and contiguous. Instead construct ProgramShape
  // directly.
  ProgramShape program_shape;
  TF_ASSIGN_OR_RETURN(*program_shape.mutable_result(),
                      user_computation->GetShape(arg->operand()));

  TF_DCHECK_OK(ShapeUtil::ValidateShape(program_shape.result()));

  ExecutionOptions execution_options = xla::CreateDefaultExecutionOptions();
  execution_options.mutable_debug_options()->set_xla_enable_fast_math(false);
  execution_options.mutable_debug_options()
      ->set_xla_eliminate_hlo_implicit_broadcast(true);
  *execution_options.mutable_shape_with_output_layout() =
      program_shape.result();

  Shape shape_with_output_layout(program_shape.result());
  if (arg->has_output_layout()) {
    TF_RETURN_IF_ERROR(LayoutUtil::ValidateLayoutForShape(
        arg->output_layout(), execution_options.shape_with_output_layout()));
    *execution_options.mutable_shape_with_output_layout()->mutable_layout() =
        arg->output_layout();
  }

  TF_ASSIGN_OR_RETURN(std::unique_ptr<HloModuleConfig> module_config,
                      CreateModuleConfig(program_shape, {}, execution_options));

  // Exclude dead parameter instructions for the purpose of computing constants.
  TF_ASSIGN_OR_RETURN(
      std::unique_ptr<HloModule> module,
      computation_tracker_.BuildHloModule(versioned_handle, *module_config,
                                          /*include_unreachable_instructions=*/
                                          false));

  HloEvaluator evaluator;
  TF_ASSIGN_OR_RETURN(auto result_literal, evaluator.Evaluate(*module, {}));
  // Since the shape_with_output_layout option in ExecutionOption is
  // non-effective to the Evaluator results, explicit relayout here.
  if (arg->has_output_layout()) {
    result_literal = result_literal->Relayout(arg->output_layout());
  }
  *result->mutable_literal() = result_literal->ToProto();

  return tensorflow::Status::OK();
}

tensorflow::Status Service::GetShape(const GetShapeRequest* arg,
                                     GetShapeResponse* result) {
  TF_ASSIGN_OR_RETURN(const Allocation* allocation,
                      allocation_tracker_.Resolve(arg->data()));
  *result->mutable_shape() = allocation->shape();
  return tensorflow::Status::OK();
}

tensorflow::Status Service::GetComputationShape(
    const GetComputationShapeRequest* arg,
    GetComputationShapeResponse* result) {
  TF_ASSIGN_OR_RETURN(UserComputation * computation,
                      computation_tracker_.Resolve(arg->computation()));

  VersionedComputationHandle versioned_handle =
      computation->GetVersionedHandle();

  TF_ASSIGN_OR_RETURN(auto program_shape, computation->ComputeProgramShape(
                                              versioned_handle.version));
  *result->mutable_program_shape() = *program_shape;
  return tensorflow::Status::OK();
}

tensorflow::Status Service::GetLocalShape(const GetLocalShapeRequest* arg,
                                          GetLocalShapeResponse* result) {
  TF_ASSIGN_OR_RETURN(UserComputation * computation,
                      computation_tracker_.Resolve(arg->computation()));

  TF_ASSIGN_OR_RETURN(*result->mutable_shape(),
                      computation->GetShape(arg->operand()));
  return tensorflow::Status::OK();
}

tensorflow::Status Service::GetComputationStats(
    const ComputationStatsRequest* arg, ComputationStatsResponse* result) {
  TF_ASSIGN_OR_RETURN(UserComputation * user_computation,
                      computation_tracker_.Resolve(arg->computation()));

  VersionedComputationHandle versioned_handle =
      user_computation->GetVersionedHandle();

  HloModuleConfig config;
  config.set_debug_options(arg->debug_options());
  TF_ASSIGN_OR_RETURN(
      std::unique_ptr<HloModule> module,
      computation_tracker_.BuildHloModule(versioned_handle, config));

  hlo_graph_dumper::MaybeDumpHloModule(*module,
                                       "computation statistics subject");

  // Run HLO analysis to get the computation statistics.
  HloCostAnalysis analysis(
      execute_backend_->compiler()->ShapeSizeBytesFunction());

  TF_RETURN_IF_ERROR(
      module->entry_computation()->root_instruction()->Accept(&analysis));

  ComputationStats stats;
  stats.set_flop_count(analysis.flop_count());
  stats.set_transcendental_count(analysis.transcendental_count());
  *result->mutable_stats() = stats;
  return tensorflow::Status::OK();
}

template <typename RequestT, typename ResponseT>
tensorflow::Status Service::AddInstruction(
    const RequestT* arg, ResponseT* result,
    const std::function<StatusOr<ComputationDataHandle>(UserComputation*)>&
        adder) {
  TF_ASSIGN_OR_RETURN(UserComputation * computation,
                      computation_tracker_.Resolve(arg->computation()));

  TF_ASSIGN_OR_RETURN(*result->mutable_output(), adder(computation));
  return tensorflow::Status::OK();
}

tensorflow::Status Service::Op(const OpRequest* arg, OpResponse* result) {
  TF_ASSIGN_OR_RETURN(UserComputation * computation,
                      computation_tracker_.Resolve(arg->computation()));
  StatusOr<ComputationDataHandle> handle_status;

  switch (arg->op_case()) {
    case OpRequest::kBatchNormTrainingRequest:
      handle_status = computation->AddBatchNormTrainingInstruction(
          arg->batch_norm_training_request());
      break;
    case OpRequest::kBatchNormGradRequest:
      handle_status = computation->AddBatchNormGradInstruction(
          arg->batch_norm_grad_request());
      break;
    case OpRequest::kBinaryOpRequest:
      handle_status =
          computation->AddBinaryInstruction(arg->binary_op_request());
      break;
    case OpRequest::kBroadcastRequest:
      handle_status =
          computation->AddBroadcastInstruction(arg->broadcast_request());
      break;
    case OpRequest::kCallRequest: {
      TF_ASSIGN_OR_RETURN(
          UserComputation * to_apply,
          computation_tracker_.Resolve(arg->call_request().to_apply()));
      handle_status =
          computation->AddCallInstruction(arg->call_request(), *to_apply);
      break;
    }
    case OpRequest::kConcatenateRequest:
      handle_status =
          computation->AddConcatenateInstruction(arg->concatenate_request());
      break;
    case OpRequest::kConstantRequest:
      handle_status =
          computation->AddConstantInstruction(arg->constant_request());
      break;
    case OpRequest::kConvertRequest:
      handle_status =
          computation->AddConvertInstruction(arg->convert_request());
      break;
    case OpRequest::kConvolveRequest:
      handle_status =
          computation->AddConvolveInstruction(arg->convolve_request());
      break;
    case OpRequest::kCrossReplicaSumRequest:
      handle_status = computation->AddCrossReplicaSumInstruction(
          arg->cross_replica_sum_request());
      break;
    case OpRequest::kCustomCallRequest:
      handle_status =
          computation->AddCustomCallInstruction(arg->custom_call_request());
      break;
    case OpRequest::kDynamicSliceRequest:
      handle_status =
          computation->AddDynamicSliceInstruction(arg->dynamic_slice_request());
      break;
    case OpRequest::kDynamicUpdateSliceRequest:
      handle_status = computation->AddDynamicUpdateSliceInstruction(
          arg->dynamic_update_slice_request());
      break;
    case OpRequest::kGetTupleElementRequest:
      handle_status = computation->AddGetTupleElementInstruction(
          arg->get_tuple_element_request());
      break;
    case OpRequest::kInfeedRequest:
      handle_status = computation->AddInfeedInstruction(arg->infeed_request());
      break;
    case OpRequest::kOutfeedRequest:
      TF_RETURN_IF_ERROR(
          computation->AddOutfeedInstruction(arg->outfeed_request()));
      return tensorflow::Status::OK();
    case OpRequest::kMapRequest: {
      TF_ASSIGN_OR_RETURN(
          UserComputation * to_apply,
          computation_tracker_.Resolve(arg->map_request().to_apply()));
      handle_status =
          computation->AddMapInstruction(arg->map_request(), *to_apply);
      break;
    }
    case OpRequest::kPadRequest:
      handle_status = computation->AddPadInstruction(arg->pad_request());
      break;
    case OpRequest::kParameterRequest:
      handle_status =
          computation->AddParameterInstruction(arg->parameter_request());
      break;
    case OpRequest::kReduceRequest: {
      TF_ASSIGN_OR_RETURN(
          UserComputation * to_apply,
          computation_tracker_.Resolve(arg->reduce_request().to_apply()));
      handle_status =
          computation->AddReduceInstruction(arg->reduce_request(), *to_apply);
      break;
    }
    case OpRequest::kReducePrecisionRequest: {
      handle_status = computation->AddReducePrecisionInstruction(
          arg->reduce_precision_request());
      break;
    }
    case OpRequest::kReduceWindowRequest: {
      TF_ASSIGN_OR_RETURN(UserComputation * to_apply,
                          computation_tracker_.Resolve(
                              arg->reduce_window_request().to_apply()));
      handle_status = computation->AddReduceWindowInstruction(
          arg->reduce_window_request(), *to_apply);
      break;
    }
    case OpRequest::kReshapeRequest:
      handle_status =
          computation->AddReshapeInstruction(arg->reshape_request());
      break;
    case OpRequest::kReverseRequest:
      handle_status =
          computation->AddReverseInstruction(arg->reverse_request());
      break;
    case OpRequest::kRngRequest:
      handle_status = computation->AddRngInstruction(arg->rng_request());
      break;
    case OpRequest::kSelectAndScatterRequest: {
      TF_ASSIGN_OR_RETURN(UserComputation * select,
                          computation_tracker_.Resolve(
                              arg->select_and_scatter_request().select()));
      TF_ASSIGN_OR_RETURN(UserComputation * scatter,
                          computation_tracker_.Resolve(
                              arg->select_and_scatter_request().scatter()));
      handle_status = computation->AddSelectAndScatterInstruction(
          arg->select_and_scatter_request(), *select, *scatter);
      break;
    }
    case OpRequest::kSliceRequest:
      handle_status = computation->AddSliceInstruction(arg->slice_request());
      break;
    case OpRequest::kTernaryOpRequest:
      handle_status =
          computation->AddTernaryInstruction(arg->ternary_op_request());
      break;
    case OpRequest::kTraceRequest:
      return computation->AddTraceInstruction(arg->trace_request());
    case OpRequest::kTransposeRequest:
      handle_status =
          computation->AddTransposeInstruction(arg->transpose_request());
      break;
    case OpRequest::kUnaryOpRequest:
      handle_status = computation->AddUnaryInstruction(arg->unary_op_request());
      break;
    case OpRequest::kVariadicOpRequest:
      handle_status =
          computation->AddVariadicInstruction(arg->variadic_op_request());
      break;
    case OpRequest::kWhileRequest: {
      TF_ASSIGN_OR_RETURN(
          UserComputation * condition,
          computation_tracker_.Resolve(arg->while_request().condition()));
      TF_ASSIGN_OR_RETURN(
          UserComputation * body,
          computation_tracker_.Resolve(arg->while_request().body()));
      handle_status = computation->AddWhileInstruction(arg->while_request(),
                                                       *condition, *body);
      break;
    }
    case OpRequest::kSendRequest: {
      TF_RETURN_IF_ERROR(
          channel_tracker_.RegisterSend(arg->send_request().channel_handle()));
      TF_RETURN_IF_ERROR(computation->AddSendInstruction(arg->send_request()));
      return tensorflow::Status::OK();
    }
    case OpRequest::kRecvRequest: {
      TF_RETURN_IF_ERROR(
          channel_tracker_.RegisterRecv(arg->recv_request().channel_handle()));
      handle_status = computation->AddRecvInstruction(arg->recv_request());
      break;
    }
    default:
      return InvalidArgument("Unsupported operation");
  }
  TF_ASSIGN_OR_RETURN(*result->mutable_output(), handle_status);

  // We set the debug metadata here, because we slice off part of the OpRequest
  // proto in the above switch statement.
  TF_ASSIGN_OR_RETURN(ComputationDataHandle handle, handle_status);
  TF_RETURN_IF_ERROR(computation->SetOpMetadata(handle, arg->metadata()));

  return tensorflow::Status::OK();
}

tensorflow::Status Service::SnapshotComputation(
    const SnapshotComputationRequest* arg,
    SnapshotComputationResponse* result) {
  TF_ASSIGN_OR_RETURN(
      std::unique_ptr<SessionModule> module,
      computation_tracker_.SnapshotComputation(arg->computation()));

  result->set_allocated_module(module.release());

  return tensorflow::Status::OK();
}

tensorflow::Status Service::LoadComputationSnapshot(
    const LoadComputationSnapshotRequest* arg,
    LoadComputationSnapshotResponse* result) {
  TF_ASSIGN_OR_RETURN(*result->mutable_computation(),
                      computation_tracker_.LoadSessionModule(arg->module()));
  return tensorflow::Status::OK();
}

DeviceHandle Service::SingleComputationDeviceHandle() const {
  DeviceHandle device_handle;
  device_handle.set_handle(0);
  device_handle.set_device_count(1);
  return device_handle;
}

StatusOr<std::vector<perftools::gputools::StreamExecutor*>> Service::Replicas(
    const Backend& backend, const DeviceHandle& device_handle) const {
  std::vector<perftools::gputools::StreamExecutor*> replicas;
  for (int replica = 0; replica < options_.number_of_replicas(); ++replica) {
    // From the computation placer, find out the device ids of the replicas for
    // the given device handle.
    TF_ASSIGN_OR_RETURN(
        int device_ordinal,
        backend.computation_placer()->DeviceId(replica, device_handle.handle(),
                                               options_.number_of_replicas(),
                                               device_handle.device_count()));
    TF_ASSIGN_OR_RETURN(auto executor, backend.stream_executor(device_ordinal));
    replicas.push_back(executor);
  }
  return replicas;
}

}  // namespace xla
