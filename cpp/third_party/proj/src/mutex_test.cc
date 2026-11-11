// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Unit tests for PROJ's recursive core mutex acquire/release helpers
// (proj/src/mutex.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/mutex.cpp

#include <latch>   // NOLINT(build/c++20)
#include <thread>  // NOLINT
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

// RAII guard so every pj_acquire_lock() pairs with pj_release_lock() even if
// an assertion aborts early.
class ScopedCoreLock {
 public:
  ScopedCoreLock() { pj_acquire_lock(); }
  ~ScopedCoreLock() { pj_release_lock(); }

  ScopedCoreLock(const ScopedCoreLock&) = delete;
  ScopedCoreLock& operator=(const ScopedCoreLock&) = delete;
};

TEST(MutexTest, AcquireAndReleaseAreReentrantOnSameThread) {
  int value = 0;

  // 1. Sequential lock/unlock cycles succeed without deadlock.
  for (int i = 0; i < 4; ++i) {
    ScopedCoreLock lock;
    ++value;
  }
  EXPECT_EQ(4, value);

  // 2. Nested acquisitions on the same thread succeed because core_lock is a
  // std::recursive_mutex.
  {
    ScopedCoreLock depth1;
    ++value;
    {
      ScopedCoreLock depth2;
      ++value;
      {
        ScopedCoreLock depth3;
        ++value;
        {
          ScopedCoreLock depth4;
          ++value;
        }
      }
    }
  }
  EXPECT_EQ(8, value);
}

TEST(MutexTest, SerializesConcurrentThreadsAndAllowsPerThreadRecursion) {
  constexpr int kNumThreads = 6;
  constexpr int kIterationsPerThread = 500;

  // Protected strictly by core_lock (non-atomic so TSan verifies
  // happens-before).
  int in_critical = 0;
  int shared_counter = 0;
  bool overlap_detected = false;

  std::latch start_gate(kNumThreads);
  {
    std::vector<std::jthread> workers;
    workers.reserve(kNumThreads);
    for (int t = 0; t < kNumThreads; ++t) {
      workers.emplace_back([&]() {
        start_gate.arrive_and_wait();
        for (int i = 0; i < kIterationsPerThread; ++i) {
          // Exercise recursive acquisition from concurrent worker threads.
          ScopedCoreLock outer;
          ScopedCoreLock inner;
          if (in_critical != 0) {
            overlap_detected = true;
          }
          ++in_critical;
          const int prev = shared_counter;
          shared_counter = prev + 1;
          --in_critical;
        }
      });
    }
  }

  EXPECT_FALSE(overlap_detected);
  EXPECT_EQ(0, in_critical);
  EXPECT_EQ(kNumThreads * kIterationsPerThread, shared_counter);
}

}  // namespace
}  // namespace proj
