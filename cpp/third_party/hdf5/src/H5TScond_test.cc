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
// Tests internal condition variable initialization, waiting, signaling,
// broadcasting, failure propagation, and destruction
// (third_party/hdf5/src/H5TScond.c and H5TScond.h).
//
// Build caveat: google3 compiles with pthreads (H5_HAVE_C11_THREADS and
// H5_HAVE_WIN_THREADS undefined), so H5TScond.c compiles the pthread backend.
// Because glibc's pthread_cond_init(..., NULL) and pthread_cond_destroy(...)
// unconditionally return 0, non-TSan builds interpose those two symbols to
// inject targeted errors on a single test-owned H5TS_cond_t pointer.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5TScond.c

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>

#include <array>
#include <atomic>
#include <cstring>
#include <thread>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/h5ts_c_shim.h"
#include "third_party/hdf5/src/hdf5.h"

#if defined(__SANITIZE_THREAD__)
#define H5TSCOND_HAVE_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define H5TSCOND_HAVE_TSAN 1
#endif
#endif

#ifndef H5TSCOND_HAVE_TSAN
namespace {

std::atomic<const pthread_cond_t*> g_fail_cond_init_target{nullptr};
std::atomic<const pthread_cond_t*> g_fail_cond_destroy_target{nullptr};

using CondInitFn = int (*)(pthread_cond_t*, const pthread_condattr_t*);
using CondDestroyFn = int (*)(pthread_cond_t*);

CondInitFn ResolveRealCondInit() {
  static std::atomic<CondInitFn> cached{nullptr};
  CondInitFn fn = cached.load(std::memory_order_acquire);
  if (fn != nullptr) return fn;
  // Prefer GLIBC_2.3.2 on x86_64 glibc < 2.36 so dlsym does not resolve the
  // legacy GLIBC_2.2.5 2.0 compat wrapper; fall back on arm64/newer glibc.
  void* sym = dlvsym(RTLD_NEXT, "pthread_cond_init", "GLIBC_2.3.2");
  if (sym == nullptr) {
    sym = dlsym(RTLD_NEXT, "pthread_cond_init");
  }
  fn = reinterpret_cast<CondInitFn>(sym);
  cached.store(fn, std::memory_order_release);
  return fn;
}

CondDestroyFn ResolveRealCondDestroy() {
  static std::atomic<CondDestroyFn> cached{nullptr};
  CondDestroyFn fn = cached.load(std::memory_order_acquire);
  if (fn != nullptr) return fn;
  void* sym = dlvsym(RTLD_NEXT, "pthread_cond_destroy", "GLIBC_2.3.2");
  if (sym == nullptr) {
    sym = dlsym(RTLD_NEXT, "pthread_cond_destroy");
  }
  fn = reinterpret_cast<CondDestroyFn>(sym);
  cached.store(fn, std::memory_order_release);
  return fn;
}

}  // namespace

extern "C" int pthread_cond_init(pthread_cond_t* cond,
                                 const pthread_condattr_t* attr) __THROW {
  const pthread_cond_t* fail_target =
      g_fail_cond_init_target.load(std::memory_order_acquire);
  if (fail_target != nullptr && cond == fail_target) {
    return EINVAL;
  }
  std::memset(cond, 0, sizeof(*cond));
  CondInitFn real_fn = ResolveRealCondInit();
  return real_fn != nullptr ? real_fn(cond, attr) : ENOSYS;
}

extern "C" int pthread_cond_destroy(pthread_cond_t* cond) __THROW {
  const pthread_cond_t* fail_target =
      g_fail_cond_destroy_target.load(std::memory_order_acquire);
  if (fail_target != nullptr && cond == fail_target) {
    return EBUSY;
  }
  CondDestroyFn real_fn = ResolveRealCondDestroy();
  return real_fn != nullptr ? real_fn(cond) : ENOSYS;
}
#endif  // !H5TSCOND_HAVE_TSAN

namespace hdf5 {
namespace {

TEST(H5TScondTest, InitAndDestroySucceed) {
  H5TS_cond_t cond{};
  ASSERT_GE(H5TS_cond_init(&cond), 0);
  EXPECT_GE(H5TS_cond_destroy(&cond), 0);
}

#ifndef H5TSCOND_HAVE_TSAN
TEST(H5TScondTest, InitAndDestroyPropagatePthreadFailures) {
  H5TS_cond_t cond{};

  g_fail_cond_init_target.store(&cond, std::memory_order_release);
  {
    absl::Cleanup clear_init = [] {
      g_fail_cond_init_target.store(nullptr, std::memory_order_release);
    };
    EXPECT_LT(H5TS_cond_init(&cond), 0);
  }

  ASSERT_GE(H5TS_cond_init(&cond), 0);
  g_fail_cond_destroy_target.store(&cond, std::memory_order_release);
  {
    absl::Cleanup clear_destroy = [] {
      g_fail_cond_destroy_target.store(nullptr, std::memory_order_release);
    };
    EXPECT_LT(H5TS_cond_destroy(&cond), 0);
  }
  EXPECT_GE(H5TS_cond_destroy(&cond), 0);
}
#endif  // !H5TSCOND_HAVE_TSAN

TEST(H5TScondTest, SignalWakesWaitingThread) {
  H5TS_mutex_t mutex{};
  H5TS_cond_t cond{};
  ASSERT_GE(H5TS_mutex_init(&mutex, H5TS_MUTEX_TYPE_PLAIN), 0);
  absl::Cleanup destroy_mutex = [&] {
    EXPECT_GE(H5TS_mutex_destroy(&mutex), 0);
  };

  ASSERT_GE(H5TS_cond_init(&cond), 0);
  absl::Cleanup destroy_cond = [&] { EXPECT_GE(H5TS_cond_destroy(&cond), 0); };

  bool ready = false;
  bool observed_ready = false;

  std::thread waiter([&]() {
    if (H5TS_shim_mutex_lock(&mutex) < 0) return;
    while (!ready) {
      if (H5TS_shim_cond_wait(&cond, &mutex) < 0) {
        H5TS_shim_mutex_unlock(&mutex);
        return;
      }
    }
    observed_ready = ready;
    H5TS_shim_mutex_unlock(&mutex);
  });

  ASSERT_GE(H5TS_shim_mutex_lock(&mutex), 0);
  ready = true;
  EXPECT_GE(H5TS_shim_cond_signal(&cond), 0);
  ASSERT_GE(H5TS_shim_mutex_unlock(&mutex), 0);

  waiter.join();
  EXPECT_TRUE(observed_ready);
}

TEST(H5TScondTest, BroadcastWakesAllWaitingThreads) {
  constexpr int kWaiters = 4;

  H5TS_mutex_t mutex{};
  H5TS_cond_t cond{};
  ASSERT_GE(H5TS_mutex_init(&mutex, H5TS_MUTEX_TYPE_PLAIN), 0);
  absl::Cleanup destroy_mutex = [&] {
    EXPECT_GE(H5TS_mutex_destroy(&mutex), 0);
  };

  ASSERT_GE(H5TS_cond_init(&cond), 0);
  absl::Cleanup destroy_cond = [&] { EXPECT_GE(H5TS_cond_destroy(&cond), 0); };

  int parked = 0;
  bool go = false;
  int awakened = 0;

  std::array<std::thread, kWaiters> threads;
  for (int i = 0; i < kWaiters; ++i) {
    threads[i] = std::thread([&]() {
      if (H5TS_shim_mutex_lock(&mutex) < 0) return;
      ++parked;
      while (!go) {
        if (H5TS_shim_cond_wait(&cond, &mutex) < 0) {
          H5TS_shim_mutex_unlock(&mutex);
          return;
        }
      }
      ++awakened;
      H5TS_shim_mutex_unlock(&mutex);
    });
  }

  while (true) {
    ASSERT_GE(H5TS_shim_mutex_lock(&mutex), 0);
    const bool all_parked = (parked == kWaiters);
    if (all_parked) {
      go = true;
      EXPECT_GE(H5TS_shim_cond_broadcast(&cond), 0);
      ASSERT_GE(H5TS_shim_mutex_unlock(&mutex), 0);
      break;
    }
    ASSERT_GE(H5TS_shim_mutex_unlock(&mutex), 0);
    std::this_thread::yield();
  }

  for (auto& t : threads) {
    t.join();
  }
  EXPECT_EQ(awakened, kWaiters);
}

}  // namespace
}  // namespace hdf5
