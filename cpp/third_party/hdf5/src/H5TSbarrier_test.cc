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
// Tests internal thread barrier initialization, waiting, reuse, validation,
// and destruction (third_party/hdf5/src/H5TSbarrier.c and H5TSbarrier.h).
//
// Note: H5_HAVE_PTHREAD_BARRIER is undefined in google3 H5pubconf.h, so HDF5
// compiles the portable atomic-counter barrier implementation. All barrier
// accesses go through h5ts_c_shim.h to avoid C11 _Atomic ABI differences in C++.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5TSbarrier.c

#include "third_party/hdf5/src/hdf5.h"

#include <array>
#include <atomic>
#include <memory>
#include <thread>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/h5ts_c_shim.h"

namespace hdf5 {
namespace {

struct BarrierDeleter {
  void operator()(H5TS_shim_barrier_t* b) const {
    if (b != nullptr) {
      H5TS_shim_barrier_free(b);
    }
  }
};

using ScopedBarrier = std::unique_ptr<H5TS_shim_barrier_t, BarrierDeleter>;

TEST(H5TSbarrierTest, NullAndZeroCountArgumentsFail) {
#ifdef H5_HAVE_PTHREAD_BARRIER
  ADD_FAILURE() << "Expected H5_HAVE_PTHREAD_BARRIER to be undefined.";
#endif

  EXPECT_LT(H5TS_shim_barrier_init_null(2), 0);
  EXPECT_LT(H5TS_shim_barrier_wait_null(), 0);
  EXPECT_LT(H5TS_shim_barrier_destroy_null(), 0);

  ScopedBarrier barrier(H5TS_shim_barrier_alloc());
  ASSERT_NE(barrier, nullptr);
  EXPECT_LT(H5TS_shim_barrier_init(barrier.get(), 0), 0);
}

TEST(H5TSbarrierTest, SingleThreadBarrierCanBeWaitedRepeatedly) {
  ScopedBarrier barrier(H5TS_shim_barrier_alloc());
  ASSERT_NE(barrier, nullptr);

  ASSERT_GE(H5TS_shim_barrier_init(barrier.get(), 1), 0);
  absl::Cleanup destroy = [&] {
    EXPECT_GE(H5TS_shim_barrier_destroy(barrier.get()), 0);
  };

  for (int i = 0; i < 4; ++i) {
    EXPECT_GE(H5TS_shim_barrier_wait(barrier.get()), 0);
  }
}

TEST(H5TSbarrierTest, MultiThreadBarrierSynchronizesAcrossGenerations) {
  constexpr unsigned kThreads = 4;
  constexpr int kRounds = 4;

  ScopedBarrier barrier(H5TS_shim_barrier_alloc());
  ASSERT_NE(barrier, nullptr);
  ASSERT_GE(H5TS_shim_barrier_init(barrier.get(), kThreads), 0);
  absl::Cleanup destroy = [&] {
    EXPECT_GE(H5TS_shim_barrier_destroy(barrier.get()), 0);
  };

  std::array<std::atomic<int>, kThreads> phase_arrivals{};
  std::array<std::thread, kThreads> threads;
  std::array<bool, kThreads> thread_ok{};

  for (unsigned tid = 0; tid < kThreads; ++tid) {
    threads[tid] = std::thread([&, tid]() {
      bool ok = true;
      for (int r = 1; r <= kRounds; ++r) {
        phase_arrivals[tid].store(r, std::memory_order_relaxed);
        if (H5TS_shim_barrier_wait(barrier.get()) < 0) {
          ok = false;
          break;
        }
        for (unsigned other = 0; other < kThreads; ++other) {
          if (phase_arrivals[other].load(std::memory_order_relaxed) != r) {
            ok = false;
          }
        }
        if (H5TS_shim_barrier_wait(barrier.get()) < 0) {
          ok = false;
          break;
        }
      }
      thread_ok[tid] = ok;
    });
  }

  for (auto& t : threads) {
    t.join();
  }
  for (bool ok : thread_ok) {
    EXPECT_TRUE(ok);
  }
}

}  // namespace
}  // namespace hdf5
