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

#include "tensorflow/core/platform/cloud/file_block_cache.h"
#include <cstring>
#include "tensorflow/core/lib/core/status_test_util.h"
#include "tensorflow/core/platform/cloud/now_seconds_env.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/test.h"

namespace tensorflow {
namespace {

TEST(FileBlockCacheTest, PassThrough) {
  const string want_filename = "foo/bar";
  const size_t want_offset = 42;
  const size_t want_n = 1024;
  int calls = 0;
  auto fetcher = [&calls, want_filename, want_offset, want_n](
                     const string& got_filename, size_t got_offset,
                     size_t got_n, std::vector<char>* out) {
    EXPECT_EQ(got_filename, want_filename);
    EXPECT_EQ(got_offset, want_offset);
    EXPECT_EQ(got_n, want_n);
    calls++;
    out->resize(got_n, 'x');
    return Status::OK();
  };
  // If block_size, max_bytes, or both are zero, the cache is a pass-through.
  FileBlockCache cache1(1, 0, 0, fetcher);
  FileBlockCache cache2(0, 1, 0, fetcher);
  FileBlockCache cache3(0, 0, 0, fetcher);
  std::vector<char> out;
  TF_EXPECT_OK(cache1.Read(want_filename, want_offset, want_n, &out));
  EXPECT_EQ(calls, 1);
  TF_EXPECT_OK(cache2.Read(want_filename, want_offset, want_n, &out));
  EXPECT_EQ(calls, 2);
  TF_EXPECT_OK(cache3.Read(want_filename, want_offset, want_n, &out));
  EXPECT_EQ(calls, 3);
}

TEST(FileBlockCacheTest, BlockAlignment) {
  // Initialize a 256-byte buffer.  This is the file underlying the reads we'll
  // do in this test.
  const size_t size = 256;
  std::vector<char> buf;
  for (int i = 0; i < size; i++) {
    buf.push_back(i);
  }
  // The fetcher just fetches slices of the buffer.
  auto fetcher = [&buf](const string& filename, size_t offset, size_t n,
                        std::vector<char>* out) {
    if (offset < buf.size()) {
      if (offset + n > buf.size()) {
        out->insert(out->end(), buf.begin() + offset, buf.end());
      } else {
        out->insert(out->end(), buf.begin() + offset, buf.begin() + offset + n);
      }
    }
    return Status::OK();
  };
  for (size_t block_size = 2; block_size <= 4; block_size++) {
    // Make a cache of N-byte block size (1 block) and verify that reads of
    // varying offsets and lengths return correct data.
    FileBlockCache cache(block_size, block_size, 0, fetcher);
    for (size_t offset = 0; offset < 10; offset++) {
      for (size_t n = block_size - 2; n <= block_size + 2; n++) {
        std::vector<char> got;
        TF_EXPECT_OK(cache.Read("", offset, n, &got));
        // Verify the size of the read.
        if (offset + n <= size) {
          // Expect a full read.
          EXPECT_EQ(got.size(), n) << "block size = " << block_size
                                   << ", offset = " << offset << ", n = " << n;
        } else {
          // Expect a partial read.
          EXPECT_EQ(got.size(), size - offset)
              << "block size = " << block_size << ", offset = " << offset
              << ", n = " << n;
        }
        // Verify the contents of the read.
        std::vector<char>::const_iterator begin = buf.begin() + offset;
        std::vector<char>::const_iterator end =
            offset + n > buf.size() ? buf.end() : begin + n;
        std::vector<char> want(begin, end);
        EXPECT_EQ(got, want) << "block size = " << block_size
                             << ", offset = " << offset << ", n = " << n;
      }
    }
  }
}

TEST(FileBlockCacheTest, CacheHits) {
  const size_t block_size = 16;
  std::set<size_t> calls;
  auto fetcher = [&calls, block_size](const string& filename, size_t offset,
                                      size_t n, std::vector<char>* out) {
    EXPECT_EQ(n, block_size);
    EXPECT_EQ(offset % block_size, 0);
    EXPECT_EQ(calls.find(offset), calls.end()) << "at offset " << offset;
    calls.insert(offset);
    out->resize(n, 'x');
    return Status::OK();
  };
  const uint32 block_count = 256;
  FileBlockCache cache(block_size, block_count * block_size, 0, fetcher);
  std::vector<char> out;
  // The cache has space for `block_count` blocks. The loop with i = 0 should
  // fill the cache, and the loop with i = 1 should be all cache hits. The
  // fetcher checks that it is called once and only once for each offset (to
  // fetch the corresponding block).
  for (int i = 0; i < 2; i++) {
    for (int j = 0; j < block_count; j++) {
      TF_EXPECT_OK(cache.Read("", block_size * j, block_size, &out));
    }
  }
}

TEST(FileBlockCacheTest, OutOfRange) {
  // Tests reads of a 24-byte file with block size 16.
  const size_t block_size = 16;
  const size_t file_size = 24;
  bool first_block = false;
  bool second_block = false;
  auto fetcher = [block_size, file_size, &first_block, &second_block](
                     const string& filename, size_t offset, size_t n,
                     std::vector<char>* out) {
    EXPECT_EQ(n, block_size);
    EXPECT_EQ(offset % block_size, 0);
    if (offset == 0) {
      // The first block (16 bytes) of the file.
      out->resize(n, 'x');
      first_block = true;
    } else if (offset == block_size) {
      // The second block (8 bytes) of the file.
      out->resize(file_size - block_size, 'x');
      second_block = true;
    }
    return Status::OK();
  };
  FileBlockCache cache(block_size, block_size, 0, fetcher);
  std::vector<char> out;
  // Reading the first 16 bytes should be fine.
  TF_EXPECT_OK(cache.Read("", 0, block_size, &out));
  EXPECT_TRUE(first_block);
  EXPECT_EQ(out.size(), block_size);
  // Reading at offset file_size + 4 will read the second block (since the read
  // at file_size + 4 = 28 will be aligned to an offset of 16) but will return
  // OutOfRange because the offset is past the end of the 24-byte file.
  Status status = cache.Read("", file_size + 4, 4, &out);
  EXPECT_EQ(status.code(), error::OUT_OF_RANGE);
  EXPECT_TRUE(second_block);
  EXPECT_EQ(out.size(), 0);
  // Reading the second full block will return 8 bytes, from a cache hit.
  second_block = false;
  TF_EXPECT_OK(cache.Read("", block_size, block_size, &out));
  EXPECT_FALSE(second_block);
  EXPECT_EQ(out.size(), file_size - block_size);
}

TEST(FileBlockCacheTest, Inconsistent) {
  // Tests the detection of interrupted reads leading to partially filled blocks
  // where we expected complete blocks.
  const size_t block_size = 16;
  // This fetcher returns OK but only fills in one byte for any offset.
  auto fetcher = [block_size](const string& filename, size_t offset, size_t n,
                              std::vector<char>* out) {
    EXPECT_EQ(n, block_size);
    EXPECT_EQ(offset % block_size, 0);
    out->resize(1, 'x');
    return Status::OK();
  };
  FileBlockCache cache(block_size, 2 * block_size, 0, fetcher);
  std::vector<char> out;
  // Read the second block; this should yield an OK status and a single byte.
  TF_EXPECT_OK(cache.Read("", block_size, block_size, &out));
  EXPECT_EQ(out.size(), 1);
  // Now read the first block; this should yield an INTERNAL error because we
  // had already cached a partial block at a later position.
  Status status = cache.Read("", 0, block_size, &out);
  EXPECT_EQ(status.code(), error::INTERNAL);
}

TEST(FileBlockCacheTest, LRU) {
  const size_t block_size = 16;
  std::list<size_t> calls;
  auto fetcher = [&calls, block_size](const string& filename, size_t offset,
                                      size_t n, std::vector<char>* out) {
    EXPECT_EQ(n, block_size);
    EXPECT_FALSE(calls.empty()) << "at offset = " << offset;
    if (!calls.empty()) {
      EXPECT_EQ(offset, calls.front());
      calls.pop_front();
    }
    out->resize(n, 'x');
    return Status::OK();
  };
  const uint32 block_count = 2;
  FileBlockCache cache(block_size, block_count * block_size, 0, fetcher);
  std::vector<char> out;
  // Read blocks from the cache, and verify the LRU behavior based on the
  // fetcher calls that the cache makes.
  calls.push_back(0);
  // Cache miss - drains an element from `calls`.
  TF_EXPECT_OK(cache.Read("", 0, 1, &out));
  // Cache hit - does not drain an element from `calls`.
  TF_EXPECT_OK(cache.Read("", 0, 1, &out));
  calls.push_back(block_size);
  // Cache miss followed by cache hit.
  TF_EXPECT_OK(cache.Read("", block_size, 1, &out));
  TF_EXPECT_OK(cache.Read("", block_size, 1, &out));
  calls.push_back(2 * block_size);
  // Cache miss followed by cache hit.  Causes eviction of LRU element.
  TF_EXPECT_OK(cache.Read("", 2 * block_size, 1, &out));
  TF_EXPECT_OK(cache.Read("", 2 * block_size, 1, &out));
  // LRU element was at offset 0.  Cache miss.
  calls.push_back(0);
  TF_EXPECT_OK(cache.Read("", 0, 1, &out));
  // Element at 2 * block_size is still in cache, and this read should update
  // its position in the LRU list so it doesn't get evicted by the next read.
  TF_EXPECT_OK(cache.Read("", 2 * block_size, 1, &out));
  // Element at block_size was evicted.  Reading this element will also cause
  // the LRU element (at 0) to be evicted.
  calls.push_back(block_size);
  TF_EXPECT_OK(cache.Read("", block_size, 1, &out));
  // Element at 0 was evicted again.
  calls.push_back(0);
  TF_EXPECT_OK(cache.Read("", 0, 1, &out));
}

TEST(FileBlockCacheTest, MaxStaleness) {
  int calls = 0;
  auto fetcher = [&calls](const string& filename, size_t offset, size_t n,
                          std::vector<char>* out) {
    calls++;
    out->resize(n, 'x');
    return Status::OK();
  };
  std::vector<char> out;
  std::unique_ptr<NowSecondsEnv> env(new NowSecondsEnv);
  // Create a cache with max staleness of 2 seconds, and verify that it works as
  // expected.
  FileBlockCache cache1(8, 16, 2 /* max staleness */, fetcher, env.get());
  // Execute the first read to load the block.
  TF_EXPECT_OK(cache1.Read("", 0, 1, &out));
  EXPECT_EQ(calls, 1);
  // Now advance the clock one second at a time and redo the read. The call
  // count should advance every 3 seconds (i.e. every time the staleness is
  // greater than 2).
  for (int i = 1; i <= 10; i++) {
    env->SetNowSeconds(i + 1);
    TF_EXPECT_OK(cache1.Read("", 0, 1, &out));
    EXPECT_EQ(calls, 1 + i / 3);
  }
  // Now create a cache with max staleness of 0, and verify that it also works
  // as expected.
  calls = 0;
  env->SetNowSeconds(0);
  FileBlockCache cache2(8, 16, 0 /* max staleness */, fetcher, env.get());
  // Execute the first read to load the block.
  TF_EXPECT_OK(cache2.Read("", 0, 1, &out));
  EXPECT_EQ(calls, 1);
  // Advance the clock by a huge amount and verify that the cached block is
  // used to satisfy the read.
  env->SetNowSeconds(365 * 24 * 60 * 60);  // ~1 year, just for fun.
  TF_EXPECT_OK(cache2.Read("", 0, 1, &out));
  EXPECT_EQ(calls, 1);
}

TEST(FileBlockCacheTest, RemoveFile) {
  int calls = 0;
  auto fetcher = [&calls](const string& filename, size_t offset, size_t n,
                          std::vector<char>* out) {
    calls++;
    char c = (filename == "a") ? 'a' : (filename == "b") ? 'b' : 'x';
    if (offset > 0) {
      // The first block is lower case and all subsequent blocks are upper case.
      c = toupper(c);
    }
    out->clear();
    out->resize(n, c);
    return Status::OK();
  };
  // This cache has space for 4 blocks; we'll read from two files.
  const size_t n = 3;
  FileBlockCache cache(8, 32, 0, fetcher);
  std::vector<char> out;
  std::vector<char> a(n, 'a');
  std::vector<char> b(n, 'b');
  std::vector<char> A(n, 'A');
  std::vector<char> B(n, 'B');
  // Fill the cache.
  TF_EXPECT_OK(cache.Read("a", 0, n, &out));
  EXPECT_EQ(out, a);
  EXPECT_EQ(calls, 1);
  TF_EXPECT_OK(cache.Read("a", 8, n, &out));
  EXPECT_EQ(out, A);
  EXPECT_EQ(calls, 2);
  TF_EXPECT_OK(cache.Read("b", 0, n, &out));
  EXPECT_EQ(out, b);
  EXPECT_EQ(calls, 3);
  TF_EXPECT_OK(cache.Read("b", 8, n, &out));
  EXPECT_EQ(out, B);
  EXPECT_EQ(calls, 4);
  // All four blocks should be in the cache now.
  TF_EXPECT_OK(cache.Read("a", 0, n, &out));
  EXPECT_EQ(out, a);
  TF_EXPECT_OK(cache.Read("a", 8, n, &out));
  EXPECT_EQ(out, A);
  TF_EXPECT_OK(cache.Read("b", 0, n, &out));
  EXPECT_EQ(out, b);
  TF_EXPECT_OK(cache.Read("b", 8, n, &out));
  EXPECT_EQ(out, B);
  EXPECT_EQ(calls, 4);
  // Remove the blocks from "a".
  cache.RemoveFile("a");
  // Both blocks from "b" should still be there.
  TF_EXPECT_OK(cache.Read("b", 0, n, &out));
  EXPECT_EQ(out, b);
  TF_EXPECT_OK(cache.Read("b", 8, n, &out));
  EXPECT_EQ(out, B);
  EXPECT_EQ(calls, 4);
  // The blocks from "a" should not be there.
  TF_EXPECT_OK(cache.Read("a", 0, n, &out));
  EXPECT_EQ(out, a);
  EXPECT_EQ(calls, 5);
  TF_EXPECT_OK(cache.Read("a", 8, n, &out));
  EXPECT_EQ(out, A);
  EXPECT_EQ(calls, 6);
}

TEST(FileBlockCacheTest, Prune) {
  int calls = 0;
  auto fetcher = [&calls](const string& filename, size_t offset, size_t n,
                          std::vector<char>* out) {
    calls++;
    out->clear();
    out->resize(n, 'x');
    return Status::OK();
  };
  std::vector<char> out;
  // Our fake environment is initialized with the current timestamp.
  std::unique_ptr<NowSecondsEnv> env(new NowSecondsEnv);
  uint64 now = Env::Default()->NowSeconds();
  env->SetNowSeconds(now);
  FileBlockCache cache(8, 32, 1 /* max staleness */, fetcher, env.get());
  // Read three blocks into the cache, and advance the timestamp by one second
  // with each read. Start with a block of "a" at the current timestamp `now`.
  TF_EXPECT_OK(cache.Read("a", 0, 1, &out));
  // Now load a block of a different file "b" at timestamp `now` + 1
  env->SetNowSeconds(now + 1);
  TF_EXPECT_OK(cache.Read("b", 0, 1, &out));
  // Now load a different block of file "a" at timestamp `now` + 1. When the
  // first block of "a" expires, this block should also be removed because it
  // also belongs to file "a".
  TF_EXPECT_OK(cache.Read("a", 8, 1, &out));
  // Ensure that all blocks are in the cache (i.e. reads are cache hits).
  EXPECT_EQ(cache.CacheSize(), 24);
  EXPECT_EQ(calls, 3);
  TF_EXPECT_OK(cache.Read("a", 0, 1, &out));
  TF_EXPECT_OK(cache.Read("b", 0, 1, &out));
  TF_EXPECT_OK(cache.Read("a", 8, 1, &out));
  EXPECT_EQ(calls, 3);
  // Advance the fake timestamp so that "a" becomes stale via its first block.
  env->SetNowSeconds(now + 2);
  // The pruning thread periodically compares env->NowSeconds() with the oldest
  // block's timestamp to see if it should evict any files. At the current fake
  // timestamp of `now` + 2, file "a" is stale because its first block is stale,
  // but file "b" is not stale yet. Thus, once the pruning thread wakes up (in
  // one second of wall time), it should remove "a" and leave "b" alone.
  uint64 start = Env::Default()->NowSeconds();
  do {
    Env::Default()->SleepForMicroseconds(100000);
  } while (cache.CacheSize() == 24 && Env::Default()->NowSeconds() - start < 3);
  // There should be one block left in the cache, and it should be the first
  // block of "b".
  EXPECT_EQ(cache.CacheSize(), 8);
  TF_EXPECT_OK(cache.Read("b", 0, 1, &out));
  EXPECT_EQ(calls, 3);
  // Advance the fake time to `now` + 3, at which point "b" becomes stale.
  env->SetNowSeconds(now + 3);
  // Wait for the pruner to remove "b".
  start = Env::Default()->NowSeconds();
  do {
    Env::Default()->SleepForMicroseconds(100000);
  } while (cache.CacheSize() == 8 && Env::Default()->NowSeconds() - start < 3);
  // The cache should now be empty.
  EXPECT_EQ(cache.CacheSize(), 0);
}

}  // namespace
}  // namespace tensorflow
