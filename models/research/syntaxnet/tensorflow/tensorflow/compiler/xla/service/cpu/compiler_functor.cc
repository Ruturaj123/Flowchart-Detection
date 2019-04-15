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

#include "tensorflow/compiler/xla/service/cpu/compiler_functor.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/ExecutionEngine/ObjectMemoryBuffer.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/MCContext.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "tensorflow/compiler/xla/ptr_util.h"
#include "tensorflow/compiler/xla/service/cpu/cpu_runtime.h"
#include "tensorflow/compiler/xla/service/cpu/cpu_runtime_avx.h"
#include "tensorflow/compiler/xla/service/cpu/cpu_runtime_sse4_1.h"
#include "tensorflow/compiler/xla/service/cpu/llvm_ir_runtime.h"
#include "tensorflow/compiler/xla/service/llvm_ir/llvm_util.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/compiler/xla/types.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/core/platform/logging.h"

namespace xla {
namespace cpu {

/* static */ CompilerFunctor::VectorIntrinsics
CompilerFunctor::AllIntrinsics() {
  VectorIntrinsics intrinsics;
  intrinsics.sse_intrinsics = true;
  intrinsics.avx_intrinsics = true;
  return intrinsics;
}

llvm::object::OwningBinary<llvm::object::ObjectFile> CompilerFunctor::
operator()(llvm::Module& module) const {
  llvm::legacy::PassManager module_passes;
  llvm::legacy::FunctionPassManager function_passes(&module);

  VLOG(2) << "IR before optimizations";
  XLA_VLOG_LINES(2, llvm_ir::DumpModuleToString(module));

  if (pre_optimization_hook_) {
    TF_CHECK_OK(pre_optimization_hook_(module));
  }

  // Add the appropriate TargetLibraryInfo and TargetTransformInfo.
  AddTargetInfoPasses(&module_passes);

  // Build up optimization pipeline.
  if (optimize_for_size_) {
    // Optimizing for size turns on -O2 level optimizations.
    //
    // TODO(b/64153864): Although the code generator supports size_level = 2 to
    // turn on more aggressive code size optimizations than size_level = 1, we
    // pass size_level = 1 because in many cases a size_level of 2 does
    // worse. Investigate why.
    AddOptimizationPasses(&module_passes, &function_passes, /*opt_level=*/2,
                          /*size_level=*/1);
  } else {
    AddOptimizationPasses(&module_passes, &function_passes,
                          /*opt_level=*/opt_level_, /*size_level=*/0);
  }

  // Run optimization passes on module.
  function_passes.doInitialization();

  CHECK(!llvm::verifyModule(module, &llvm::dbgs()));

  for (auto func = module.begin(); func != module.end(); ++func) {
    function_passes.run(*func);
  }
  function_passes.doFinalization();
  module_passes.run(module);

  CHECK(!llvm::verifyModule(module, &llvm::dbgs()));

  runtime::RewriteIRRuntimeFunctions(&module, enable_fast_math_);

  // Buffer for holding machine code prior to constructing the ObjectFile.
  llvm::SmallVector<char, 0> stream_buffer;
  llvm::raw_svector_ostream ostream(stream_buffer);

  VLOG(2) << "IR after optimizations";
  XLA_VLOG_LINES(2, llvm_ir::DumpModuleToString(module));

  if (post_optimization_hook_) {
    TF_CHECK_OK(post_optimization_hook_(module));
  }

  // Generate code.
  llvm::MCContext* mc_context;
  llvm::legacy::PassManager codegen_passes;
  target_machine_->addPassesToEmitMC(codegen_passes, mc_context, ostream);
  codegen_passes.run(module);

  // Construct ObjectFile from machine code buffer.
  std::unique_ptr<llvm::MemoryBuffer> memory_buffer(
      new llvm::ObjectMemoryBuffer(std::move(stream_buffer)));
  llvm::Expected<std::unique_ptr<llvm::object::ObjectFile>>
      object_file_or_error = llvm::object::ObjectFile::createObjectFile(
          memory_buffer->getMemBufferRef());
  CHECK(object_file_or_error);

  std::unique_ptr<llvm::object::ObjectFile> object_file =
      std::move(object_file_or_error.get());
  if (VLOG_IS_ON(2)) {
    StatusOr<DisassemblerResult> disassembly_status =
        disassembler_->DisassembleObjectFile(*object_file);
    if (disassembly_status.ok()) {
      auto result = disassembly_status.ValueOrDie();
      XLA_VLOG_LINES(2, result.text);
      VLOG(2) << "compiled code size: " << result.code_size_bytes << " bytes";
    }
  }

  return llvm::object::OwningBinary<llvm::object::ObjectFile>(
      std::move(object_file), std::move(memory_buffer));
}

namespace {
// Returns the set of vectorized library functions supported for the target.
std::vector<llvm::VecDesc> VectorFunctionsForTargetLibraryInfoImpl(
    llvm::Triple::ArchType arch, llvm::StringRef feature_string,
    CompilerFunctor::VectorIntrinsics const& available_intrinsics) {
  std::vector<llvm::VecDesc> vector_functions;

  const llvm::VecDesc four_wide_vector_functions[] = {
      {"expf", runtime::kExpV4F32SymbolName, 4},
      {"llvm.exp.f32", runtime::kExpV4F32SymbolName, 4},

      {"logf", runtime::kLogV4F32SymbolName, 4},
      {"llvm.log.f32", runtime::kLogV4F32SymbolName, 4},
  };

  const llvm::VecDesc eight_wide_vector_functions[] = {
      {"expf", runtime::kExpV8F32SymbolName, 8},
      {"llvm.exp.f32", runtime::kExpV8F32SymbolName, 8},

      {"logf", runtime::kLogV8F32SymbolName, 8},
      {"llvm.log.f32", runtime::kLogV8F32SymbolName, 8},
  };

  // These functions are generated by XLA as LLVM IR, so they're always
  // available.
  const llvm::VecDesc ir_vector_functions[] = {
      {"tanhf", runtime::kTanhV4F32SymbolName, 4},
      {"llvm.tanh.f32", runtime::kTanhV4F32SymbolName, 4},

      {"tanhf", runtime::kTanhV8F32SymbolName, 8},
      {"llvm.tanh.f32", runtime::kTanhV8F32SymbolName, 8},
  };

  if (arch == llvm::Triple::x86 || llvm::Triple::x86_64) {
    llvm::SmallVector<llvm::StringRef, 32> features;
    feature_string.split(features, ',', -1, /*KeepEmpty=*/false);
    if (std::find(features.begin(), features.end(), "+sse4.1") !=
            features.end() &&
        available_intrinsics.sse_intrinsics) {
      vector_functions.insert(vector_functions.end(),
                              std::begin(four_wide_vector_functions),
                              std::end(four_wide_vector_functions));
    }
    if (std::find(features.begin(), features.end(), "+avx") != features.end() &&
        available_intrinsics.avx_intrinsics) {
      vector_functions.insert(vector_functions.end(),
                              std::begin(eight_wide_vector_functions),
                              std::end(eight_wide_vector_functions));
    }
  }

  vector_functions.insert(vector_functions.end(),
                          std::begin(ir_vector_functions),
                          std::end(ir_vector_functions));
  return vector_functions;
}
}  // namespace

void CompilerFunctor::AddTargetInfoPasses(
    llvm::legacy::PassManagerBase* passes) const {
  llvm::Triple target_triple(target_machine_->getTargetTriple());
  auto target_library_info_impl =
      MakeUnique<llvm::TargetLibraryInfoImpl>(target_triple);
  target_library_info_impl->addVectorizableFunctions(
      VectorFunctionsForTargetLibraryInfoImpl(
          target_triple.getArch(), target_machine_->getTargetFeatureString(),
          available_intrinsics_));
  passes->add(
      new llvm::TargetLibraryInfoWrapperPass(*target_library_info_impl));
  passes->add(createTargetTransformInfoWrapperPass(
      target_machine_->getTargetIRAnalysis()));
}

void CompilerFunctor::AddOptimizationPasses(
    llvm::legacy::PassManagerBase* module_passes,
    llvm::legacy::FunctionPassManager* function_passes, unsigned opt_level,
    unsigned size_level) const {
  llvm::PassManagerBuilder builder;
  builder.OptLevel = opt_level;
  builder.SizeLevel = size_level;

  if (opt_level > 1) {
    builder.Inliner = llvm::createFunctionInliningPass();
  } else {
    // Only inline functions marked with "alwaysinline".
    builder.Inliner = llvm::createAlwaysInlinerLegacyPass();
  }

  builder.DisableUnitAtATime = false;
  builder.DisableUnrollLoops = opt_level == 0;
  builder.LoopVectorize = opt_level > 0 && size_level == 0;
  builder.SLPVectorize = opt_level > 1 && size_level == 0;

  builder.populateFunctionPassManager(*function_passes);
  builder.populateModulePassManager(*module_passes);
}

}  // namespace cpu
}  // namespace xla
