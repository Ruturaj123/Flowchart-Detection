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

// This header declares functions which may be called by the generated code on
// the CPU. Calls to these functions must be resolved explicitly in the JIT in
// xla::cpu::SimpleResolver.  It also defines a per-CpuExecutable context
// which is used to cache expensive state and resources utilized by the
// aforementioned functions.

#ifndef TENSORFLOW_COMPILER_XLA_SERVICE_CPU_CPU_RUNTIME_AVX_H_
#define TENSORFLOW_COMPILER_XLA_SERVICE_CPU_CPU_RUNTIME_AVX_H_

#include "tensorflow/core/platform/macros.h"

namespace xla {
namespace cpu {
namespace runtime {

extern const char *const kExpV8F32SymbolName;
extern const char *const kLogV8F32SymbolName;
extern const char *const kTanhV8F32SymbolName;

typedef float V8F32 __attribute__((__vector_size__(32)));
}  // namespace runtime
}  // namespace cpu
}  // namespace xla

extern "C" {

// The following functions are vectorized versions of a selection of libm
// library functions.
// References to these functions are created by the LLVM vectorizer.
xla::cpu::runtime::V8F32 __xla_cpu_runtime_ExpV8F32(xla::cpu::runtime::V8F32 x)
    TF_ATTRIBUTE_WEAK;

xla::cpu::runtime::V8F32 __xla_cpu_runtime_LogV8F32(xla::cpu::runtime::V8F32 x)
    TF_ATTRIBUTE_WEAK;

}

#endif  // TENSORFLOW_COMPILER_XLA_SERVICE_CPU_CPU_RUNTIME_AVX_H_
