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
// Tests POSIX pthread first-thread package initialization for HDF5
// thread-safety (third_party/hdf5/src/H5TSpthread.c).
//
// Google3 build caveats: H5_HAVE_PTHREAD_H=1, H5_HAVE_THREADS=1, and
// H5_HAVE_THREADSAFE=1 (so H5_HAVE_THREADSAFE_API is enabled).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5TSpthread.c

#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/h5ts_c_shim.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG; match its preprocessor state across
// hdf5_internal_prelude.h and H5TSpkg.h so internal struct layouts match.
#ifndef NDEBUG
#define H5TS_PTHREAD_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5TS_FRIEND
#include "third_party/hdf5/src/H5TSpkg.h"
}  // extern "C"
#ifdef H5TS_PTHREAD_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5TS_PTHREAD_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

// Exercises H5TS__pthread_first_thread_init() directly in an isolated forked
// child before the parent process triggers one-time H5TS initialization.
class PreInitEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    direct_init_ok_ = RunChild([]() {
      // 1. Initialize H5TS directly via the pthread first-thread hook.
      H5TS__pthread_first_thread_init();

      if (H5TS_shim_api_info_attempt_count() != 0) {
        return 1;
      }

      // 2. Verify the global API mutex can be locked and unlocked via C shim.
      if (H5TS_shim_mutex_lock(&H5TS_api_info_p.api_mutex) < 0) {
        return 2;
      }
      if (H5TS_shim_mutex_unlock(&H5TS_api_info_p.api_mutex) < 0) {
        return 3;
      }

      // 3. Tear down global H5TS state before exiting (without spawning threads
      // after a multi-threaded fork, which TSan explicitly prohibits).
      if (H5TS_top_term_package() < 0) {
        return 4;
      }
      H5TS_term_package();
      return 0;
    });
  }

  static bool direct_init_ok() { return direct_init_ok_; }

 private:
  template <typename Fn>
  static bool RunChild(Fn&& fn) {
    const pid_t pid = fork();
    if (pid < 0) {
      return false;
    }
    if (pid == 0) {
      const int rc = fn();
      std::exit(rc);
    }
    int status = 0;
    pid_t waited = -1;
    do {
      waited = waitpid(pid, &status, 0);
    } while (waited == -1 && errno == EINTR);
    return waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }

  static inline bool direct_init_ok_ = false;
};

[[maybe_unused]] const ::testing::Environment* const kPreInitEnv =
    ::testing::AddGlobalTestEnvironment(new PreInitEnvironment);

TEST(H5TSpthreadTest, DirectFirstThreadInitWorksInPreInitChild) {
  EXPECT_TRUE(PreInitEnvironment::direct_init_ok());
}

TEST(H5TSpthreadTest, PublicApiInitializesThreadsafeRuntimeAcrossThreads) {
  // Initialize HDF5 once before spawning worker threads: upstream H5TS_api_lock
  // checks non-atomic H5_INIT_GLOBAL before acquiring the API mutex, which
  // otherwise triggers a TSan data race against H5_init_library(). Calling
  // H5open() here executes H5TS_once(..., H5TS__pthread_first_thread_init) and
  // establishes a clean happens-before edge for all spawned threads.
  ASSERT_GE(H5open(), 0);

  constexpr int kNumThreads = 4;
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::atomic<int> failures{0};

  // 1. Exercise initialized thread-safe public HDF5 APIs concurrently.
  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);
  for (int i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([&]() {
      ready.fetch_add(1, std::memory_order_release);
      while (!go.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }

      if (H5open() < 0) {
        failures.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      bool is_ts = false;
      if (H5is_library_threadsafe(&is_ts) < 0 || !is_ts) {
        failures.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      unsigned maj = 0;
      unsigned min = 0;
      unsigned rel = 0;
      if (H5get_libversion(&maj, &min, &rel) < 0) {
        failures.fetch_add(1, std::memory_order_relaxed);
        return;
      }

      uint64_t tid = 0;
      if (H5TS_thread_id(&tid) < 0 || tid == 0) {
        failures.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      if (H5TS_get_err_stack() == nullptr ||
          H5TS_get_api_ctx_ptr() == nullptr) {
        failures.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  while (ready.load(std::memory_order_acquire) < kNumThreads) {
    std::this_thread::yield();
  }
  go.store(true, std::memory_order_release);

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(failures.load(std::memory_order_relaxed), 0);

  // 2. Confirm global lock attempt counters were initialized and incremented.
  unsigned count = 0;
  ASSERT_GE(H5TSmutex_get_attempt_count(&count), 0);
  EXPECT_GT(count, 0U);
  EXPECT_EQ(H5TS_shim_api_info_attempt_count(), count);
}

}  // namespace
}  // namespace hdf5
