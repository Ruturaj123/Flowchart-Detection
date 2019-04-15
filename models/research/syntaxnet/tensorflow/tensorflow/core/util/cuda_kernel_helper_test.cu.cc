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

#if GOOGLE_CUDA
#define EIGEN_USE_GPU

#include <numeric>
#include "tensorflow/core/platform/test.h"
#include "tensorflow/core/util/cuda_kernel_helper.h"

#define CUDA_EXPECT_SUCCESS                                 \
  {                                                         \
    cudaDeviceSynchronize();                                \
    cudaError_t err = cudaGetLastError();                   \
    EXPECT_EQ(cudaSuccess, err) << cudaGetErrorString(err); \
  }

#define CUDA_ASSERT_SUCCESS                                 \
  {                                                         \
    cudaDeviceSynchronize();                                \
    cudaError_t err = cudaGetLastError();                   \
    ASSERT_EQ(cudaSuccess, err) << cudaGetErrorString(err); \
  }

namespace tensorflow {

namespace {

__global__ void SetOutbufZero(CudaLaunchConfig config, int* outbuf) {
  CUDA_1D_KERNEL_LOOP(x, config.virtual_thread_count) { outbuf[x] = 0; }
}

// counting number of jobs by using atomic +1
__global__ void Count1D(CudaLaunchConfig config, int bufsize, int* outbuf) {
  CUDA_1D_KERNEL_LOOP(x, config.virtual_thread_count) {
    if (x < 0) {  // x might overflow when testing extreme case
      break;
    }
    atomicAdd(&outbuf[x % bufsize], 1);
  }
}
__global__ void Count2D(Cuda2DLaunchConfig config, int bufsize, int* outbuf) {
  CUDA_AXIS_KERNEL_LOOP(x, config.virtual_thread_count, x) {
    if (x < 0) {  // x might overflow when testing extreme case
      break;
    }
    CUDA_AXIS_KERNEL_LOOP(y, config.virtual_thread_count, y) {
      if (y < 0) {  // y might overflow when testing extreme case
        break;
      }
      int idx = x * config.virtual_thread_count.y + y;
      atomicAdd(&outbuf[idx % bufsize], 1);
    }
  }
}
__global__ void Count3D(Cuda3DLaunchConfig config, int bufsize, int* outbuf) {
  CUDA_AXIS_KERNEL_LOOP(x, config.virtual_thread_count, x) {
    if (x < 0) {  // x might overflow when testing extreme case
      break;
    }
    CUDA_AXIS_KERNEL_LOOP(y, config.virtual_thread_count, y) {
      if (y < 0) {  // y might overflow when testing extreme case
        break;
      }
      CUDA_AXIS_KERNEL_LOOP(z, config.virtual_thread_count, z) {
        if (z < 0) {  // z might overflow when testing extreme case
          break;
        }
        int idx =
            x * config.virtual_thread_count.y * config.virtual_thread_count.z +
            y * config.virtual_thread_count.z + z;
        atomicAdd(&outbuf[idx % bufsize], 1);
      }
    }
  }
}

}  // namespace

class CudaLaunchConfigTest : public ::testing::Test {
 protected:
  const int bufsize = 1024;
  int* outbuf = nullptr;
  Eigen::CudaStreamDevice stream;
  GPUDevice d = GPUDevice(&stream);

  virtual void SetUp() {
    cudaError_t err = cudaMallocManaged(&outbuf, sizeof(int) * bufsize);
    ASSERT_EQ(cudaSuccess, err) << cudaGetErrorString(err);
  }

  virtual void TearDown() {
    cudaDeviceSynchronize();
    cudaFree(outbuf);
    outbuf = nullptr;
  }
};

TEST_F(CudaLaunchConfigTest, GetCudaLaunchConfig) {
  CudaLaunchConfig cfg;

  // test invalid inputs
  CudaLaunchConfig default_value;
  cfg = GetCudaLaunchConfig(0, d);
  EXPECT_EQ(default_value.virtual_thread_count, cfg.virtual_thread_count);
  EXPECT_EQ(default_value.block_count, cfg.block_count);
  EXPECT_EQ(default_value.thread_per_block, cfg.thread_per_block);

  cfg = GetCudaLaunchConfig(-1, d);
  EXPECT_EQ(default_value.virtual_thread_count, cfg.virtual_thread_count);
  EXPECT_EQ(default_value.block_count, cfg.block_count);
  EXPECT_EQ(default_value.thread_per_block, cfg.thread_per_block);

  cfg = GetCudaLaunchConfig(0, d, Count1D, 0, 0);
  EXPECT_EQ(default_value.virtual_thread_count, cfg.virtual_thread_count);
  EXPECT_EQ(default_value.block_count, cfg.block_count);
  EXPECT_EQ(default_value.thread_per_block, cfg.thread_per_block);

  cfg = GetCudaLaunchConfig(-1, d, Count1D, 0, 0);
  EXPECT_EQ(default_value.virtual_thread_count, cfg.virtual_thread_count);
  EXPECT_EQ(default_value.block_count, cfg.block_count);
  EXPECT_EQ(default_value.thread_per_block, cfg.thread_per_block);

  // test valid inputs
  #define TEST_LAUNCH_PARAMETER(work_element_count)                             \
    cfg = GetCudaLaunchConfig(bufsize, d);                                      \
    SetOutbufZero<<<cfg.block_count, cfg.thread_per_block, 0, d.stream()>>>     \
                                                                (cfg, outbuf);  \
    CUDA_ASSERT_SUCCESS                                                         \
    cfg = GetCudaLaunchConfig(work_element_count, d);                           \
    Count1D<<<cfg.block_count, cfg.thread_per_block, 0, d.stream()>>> (         \
        cfg, bufsize, outbuf);                                                  \
    CUDA_EXPECT_SUCCESS                                                         \
    EXPECT_EQ(work_element_count, std::accumulate(outbuf, outbuf + bufsize, 0));\
                                                                                \
    cfg = GetCudaLaunchConfig(bufsize, d, SetOutbufZero, 0, 0);                 \
    SetOutbufZero<<<cfg.block_count, cfg.thread_per_block, 0, d.stream()>>>     \
                                                                (cfg, outbuf);  \
    CUDA_ASSERT_SUCCESS                                                         \
    cfg = GetCudaLaunchConfig(work_element_count, d, Count1D, 0, 0);            \
    Count1D<<<cfg.block_count, cfg.thread_per_block, 0, d.stream()>>> (         \
        cfg, bufsize, outbuf);                                                  \
    CUDA_EXPECT_SUCCESS                                                         \
    EXPECT_EQ(work_element_count, std::accumulate(outbuf, outbuf + bufsize, 0))

  TEST_LAUNCH_PARAMETER(128);
  TEST_LAUNCH_PARAMETER(129);
  TEST_LAUNCH_PARAMETER(511);
  TEST_LAUNCH_PARAMETER(512);
  TEST_LAUNCH_PARAMETER(2048);
  TEST_LAUNCH_PARAMETER(2049);
  TEST_LAUNCH_PARAMETER(8191);
  TEST_LAUNCH_PARAMETER(8192);
  TEST_LAUNCH_PARAMETER(123456);
  TEST_LAUNCH_PARAMETER(1 << 30);
  #undef TEST_LAUNCH_PARAMETER
}

bool operator==(const Cuda2DLaunchConfig& a, const Cuda2DLaunchConfig& b) {
  return a.thread_per_block.x == b.thread_per_block.x &&
         a.thread_per_block.y == b.thread_per_block.y &&
         a.thread_per_block.z == b.thread_per_block.z &&
         a.block_count.x == b.block_count.x &&
         a.block_count.y == b.block_count.y &&
         a.block_count.z == b.block_count.z &&
         a.thread_per_block.x == b.thread_per_block.x &&
         a.thread_per_block.y == b.thread_per_block.y &&
         a.thread_per_block.z == b.thread_per_block.z;
}

TEST_F(CudaLaunchConfigTest, GetCuda2DLaunchConfig) {
  Cuda2DLaunchConfig cfg;
  CudaLaunchConfig cfg1d;

  // test invalid inputs
  Cuda2DLaunchConfig default_value;
  cfg = GetCuda2DLaunchConfig(1, 0, d);
  EXPECT_EQ(default_value, cfg);
  cfg = GetCuda2DLaunchConfig(1, -1, d);
  EXPECT_EQ(default_value, cfg);
  cfg = GetCuda2DLaunchConfig(-1, 1, d);
  EXPECT_EQ(default_value, cfg);
  cfg = GetCuda2DLaunchConfig(-1, 1, d);
  EXPECT_EQ(default_value, cfg);
  cfg = GetCuda2DLaunchConfig(0, -1, d);
  EXPECT_EQ(default_value, cfg);
  cfg = GetCuda2DLaunchConfig(0, 0, d);
  EXPECT_EQ(default_value, cfg);

  cfg = GetCuda2DLaunchConfig(1, 0, d, Count2D, 0, 0);
  EXPECT_EQ(default_value, cfg);
  cfg = GetCuda2DLaunchConfig(1, -1, d, Count2D, 0, 0);
  EXPECT_EQ(default_value, cfg);
  cfg = GetCuda2DLaunchConfig(-1, 1, d, Count2D, 0, 0);
  EXPECT_EQ(default_value, cfg);
  cfg = GetCuda2DLaunchConfig(-1, 1, d, Count2D, 0, 0);
  EXPECT_EQ(default_value, cfg);
  cfg = GetCuda2DLaunchConfig(0, -1, d, Count2D, 0, 0);
  EXPECT_EQ(default_value, cfg);
  cfg = GetCuda2DLaunchConfig(0, 0, d, Count2D, 0, 0);
  EXPECT_EQ(default_value, cfg);

  // test valid inputs
  #define TEST_LAUNCH_PARAMETER(dimx, dimy)                                     \
    cfg1d = GetCudaLaunchConfig(bufsize, d);                                    \
    SetOutbufZero<<<cfg1d.block_count, cfg1d.thread_per_block, 0, d.stream()>>> \
                                                                (cfg1d, outbuf);\
    CUDA_ASSERT_SUCCESS                                                         \
    cfg = GetCuda2DLaunchConfig(dimx, dimy, d);                                 \
    Count2D<<<cfg.block_count, cfg.thread_per_block, 0, d.stream()>>> (         \
        cfg, bufsize, outbuf);                                                  \
    CUDA_EXPECT_SUCCESS                                                         \
    EXPECT_EQ(dimx * dimy, std::accumulate(outbuf, outbuf + bufsize, 0));       \
                                                                                \
    cfg1d = GetCudaLaunchConfig(bufsize, d, SetOutbufZero, 0, 0);               \
    SetOutbufZero<<<cfg1d.block_count, cfg1d.thread_per_block, 0, d.stream()>>> \
                                                                (cfg1d, outbuf);\
    CUDA_ASSERT_SUCCESS                                                         \
    cfg = GetCuda2DLaunchConfig(dimx, dimy, d, Count2D, 0, 0);                  \
    Count2D<<<cfg.block_count, cfg.thread_per_block, 0, d.stream()>>> (         \
        cfg, bufsize, outbuf);                                                  \
    CUDA_EXPECT_SUCCESS                                                         \
    EXPECT_EQ(dimx * dimy, std::accumulate(outbuf, outbuf + bufsize, 0))

  TEST_LAUNCH_PARAMETER(128, 128);
  TEST_LAUNCH_PARAMETER(129, 64);
  TEST_LAUNCH_PARAMETER(511, 2048);
  TEST_LAUNCH_PARAMETER(512, 512);
  TEST_LAUNCH_PARAMETER(2048, 1024);
  TEST_LAUNCH_PARAMETER(2049, 32);
  TEST_LAUNCH_PARAMETER(8191, 1);
  TEST_LAUNCH_PARAMETER(8192, 10);
  TEST_LAUNCH_PARAMETER(123456, 12);
  TEST_LAUNCH_PARAMETER(1, 1 << 30);
  TEST_LAUNCH_PARAMETER(1 << 30, 1);
  #undef TEST_LAUNCH_PARAMETER
}

TEST_F(CudaLaunchConfigTest, GetCuda3DLaunchConfig) {
  Cuda3DLaunchConfig cfg;
  CudaLaunchConfig cfg1d;

  // test invalid inputs
  Cuda3DLaunchConfig default_value;
  cfg = GetCuda3DLaunchConfig(0, 1, 1, d, Count3D, 0, 0);
  EXPECT_EQ(default_value, cfg);
  cfg = GetCuda3DLaunchConfig(-1, 1, 1, d, Count3D, 0, 0);
  EXPECT_EQ(default_value, cfg);
  cfg = GetCuda3DLaunchConfig(1, 0, 1, d, Count3D, 0, 0);
  EXPECT_EQ(default_value, cfg);
  cfg = GetCuda3DLaunchConfig(1, -1, 1, d, Count3D, 0, 0);
  EXPECT_EQ(default_value, cfg);
  cfg = GetCuda3DLaunchConfig(1, 1, 0, d, Count3D, 0, 0);
  EXPECT_EQ(default_value, cfg);
  cfg = GetCuda3DLaunchConfig(1, 1, -1, d, Count3D, 0, 0);
  EXPECT_EQ(default_value, cfg);
  cfg = GetCuda3DLaunchConfig(0, 0, 0, d, Count3D, 0, 0);
  EXPECT_EQ(default_value, cfg);
  cfg = GetCuda3DLaunchConfig(-1, -1, -1, d, Count3D, 0, 0);
  EXPECT_EQ(default_value, cfg);

  // test valid inputs
  #define TEST_LAUNCH_PARAMETER(dimx, dimy, dimz)                               \
    cfg1d = GetCudaLaunchConfig(bufsize, d, SetOutbufZero, 0, 0);               \
    SetOutbufZero<<<cfg1d.block_count, cfg1d.thread_per_block, 0, d.stream()>>> \
                                                                (cfg1d, outbuf);\
    CUDA_ASSERT_SUCCESS                                                         \
    cfg = GetCuda3DLaunchConfig(dimx, dimy, dimz, d, Count3D, 0, 0);            \
    Count3D<<<cfg.block_count, cfg.thread_per_block, 0, d.stream()>>> (         \
        cfg, bufsize, outbuf);                                                  \
    CUDA_EXPECT_SUCCESS                                                         \
    EXPECT_EQ(dimx * dimy * dimz, std::accumulate(outbuf, outbuf + bufsize, 0))

  TEST_LAUNCH_PARAMETER(128, 128, 128);
  TEST_LAUNCH_PARAMETER(129, 64, 1024);
  TEST_LAUNCH_PARAMETER(511, 2048, 128);
  TEST_LAUNCH_PARAMETER(512, 512, 64);
  TEST_LAUNCH_PARAMETER(2048, 1024, 128);
  TEST_LAUNCH_PARAMETER(2049, 32, 1024);
  TEST_LAUNCH_PARAMETER(8191, 1, 1024);
  TEST_LAUNCH_PARAMETER(8192, 10, 32);
  TEST_LAUNCH_PARAMETER(123456, 12, 21);
  TEST_LAUNCH_PARAMETER(1, 1, 1 << 30);
  TEST_LAUNCH_PARAMETER(1, 1 << 30, 1);
  TEST_LAUNCH_PARAMETER(1 << 30, 1, 1);
  #undef TEST_LAUNCH_PARAMETER
}

}  // namespace tensorflow

#endif  // GOOGLE_CUDA
