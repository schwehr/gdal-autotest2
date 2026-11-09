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
// Tests internal one-time initialization wrapper H5TS_once
// (third_party/hdf5/src/H5TSonce.c). Built on POSIX pthreads in google3.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5TSonce.c

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

#include <dlfcn.h>
#include <errno.h>

#include <atomic>
#include <thread>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(leak_sanitizer) || \
    __has_feature(hwaddress_sanitizer) || __has_feature(memory_sanitizer) || \
    __has_feature(thread_sanitizer)
#define H5TS_ONCE_TEST_HAS_SANITIZER 1
#endif
#endif
#if !defined(H5TS_ONCE_TEST_HAS_SANITIZER) &&                               \
    (defined(ADDRESS_SANITIZER) || defined(__SANITIZE_ADDRESS__) ||         \
     defined(LEAK_SANITIZER) || defined(HWADDRESS_SANITIZER) ||             \
     defined(__SANITIZE_HWADDRESS__) || defined(MEMORY_SANITIZER) ||        \
     defined(THREAD_SANITIZER) || defined(__SANITIZE_THREAD__))
#define H5TS_ONCE_TEST_HAS_SANITIZER 1
#endif

#if !defined(H5TS_ONCE_TEST_HAS_SANITIZER)
namespace {
std::atomic<pthread_once_t*> g_fail_once_target{nullptr};
}  // namespace

// Glibc's pthread_once never returns an error code; inject EINVAL for a
// single targeted H5TS_once_t* when no sanitizer interceptors are active.
extern "C" int pthread_once(pthread_once_t* once_control,
                            void (*init_routine)(void)) {
  using PthreadOnceFn = int (*)(pthread_once_t*, void (*)(void));
  static std::atomic<PthreadOnceFn> real_fn{nullptr};
  PthreadOnceFn fn = real_fn.load(std::memory_order_acquire);
  if (fn == nullptr) {
    fn = reinterpret_cast<PthreadOnceFn>(dlsym(RTLD_NEXT, "pthread_once"));
    real_fn.store(fn, std::memory_order_release);
  }
  pthread_once_t* target = g_fail_once_target.load(std::memory_order_acquire);
  if (target != nullptr && once_control == target) {
    return EINVAL;
  }
  return fn(once_control, init_routine);
}
#endif

namespace hdf5 {
namespace {

std::atomic<int> g_single_init_count{0};
std::atomic<int> g_concurrent_init_count{0};

void IncrementSingleCounter() {
  g_single_init_count.fetch_add(1, std::memory_order_relaxed);
}

void IncrementConcurrentCounter() {
  std::this_thread::yield();
  g_concurrent_init_count.fetch_add(1, std::memory_order_relaxed);
}

TEST(H5TSonceTest, NullArgumentsFail) {
  H5TS_once_t once = H5TS_ONCE_INITIALIZER;
  EXPECT_LT(H5TS_once(nullptr, IncrementSingleCounter), 0);
  EXPECT_LT(H5TS_once(&once, nullptr), 0);
  EXPECT_LT(H5TS_once(nullptr, nullptr), 0);
}

TEST(H5TSonceTest, ExecutesCallbackExactlyOnceOnRepeatedCalls) {
  H5TS_once_t once = H5TS_ONCE_INITIALIZER;
  g_single_init_count.store(0, std::memory_order_relaxed);

  EXPECT_GE(H5TS_once(&once, IncrementSingleCounter), 0);
  EXPECT_EQ(g_single_init_count.load(std::memory_order_relaxed), 1);

  EXPECT_GE(H5TS_once(&once, IncrementSingleCounter), 0);
  EXPECT_GE(H5TS_once(&once, IncrementSingleCounter), 0);
  EXPECT_EQ(g_single_init_count.load(std::memory_order_relaxed), 1);
}

TEST(H5TSonceTest, ConcurrentCallsExecuteCallbackOnce) {
  constexpr int kNumThreads = 8;
  H5TS_once_t once = H5TS_ONCE_INITIALIZER;
  g_concurrent_init_count.store(0, std::memory_order_relaxed);

  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);
  for (int i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([&once]() {
      EXPECT_GE(H5TS_once(&once, IncrementConcurrentCounter), 0);
      EXPECT_GE(H5TS_once(&once, IncrementConcurrentCounter), 0);
    });
  }
  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(g_concurrent_init_count.load(std::memory_order_relaxed), 1);
}

#if !defined(H5TS_ONCE_TEST_HAS_SANITIZER)
TEST(H5TSonceTest, PropagatesPthreadOnceFailure) {
  H5TS_once_t once = H5TS_ONCE_INITIALIZER;
  g_single_init_count.store(0, std::memory_order_relaxed);
  g_fail_once_target.store(&once, std::memory_order_release);
  absl::Cleanup clear_target = [] {
    g_fail_once_target.store(nullptr, std::memory_order_release);
  };

  EXPECT_LT(H5TS_once(&once, IncrementSingleCounter), 0);
  EXPECT_EQ(g_single_init_count.load(std::memory_order_relaxed), 0);
}
#endif

}  // namespace
}  // namespace hdf5
