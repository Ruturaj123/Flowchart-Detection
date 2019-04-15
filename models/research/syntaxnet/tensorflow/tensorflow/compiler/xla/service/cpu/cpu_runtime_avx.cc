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

#include "tensorflow/compiler/xla/service/cpu/cpu_runtime_avx.h"

#define EIGEN_USE_THREADS

#include "third_party/eigen3/Eigen/Core"

#ifdef __AVX__
xla::cpu::runtime::V8F32 __xla_cpu_runtime_ExpV8F32(
    xla::cpu::runtime::V8F32 x) {
  return Eigen::internal::pexp(x);
}

xla::cpu::runtime::V8F32 __xla_cpu_runtime_LogV8F32(
    xla::cpu::runtime::V8F32 x) {
  return Eigen::internal::plog(x);
}
#endif  // __AVX__

namespace xla {
namespace cpu {
namespace runtime {

const char *const kExpV8F32SymbolName = "__xla_cpu_runtime_ExpV8F32";
const char *const kLogV8F32SymbolName = "__xla_cpu_runtime_LogV8F32";

}  // namespace runtime
}  // namespace cpu
}  // namespace xla
