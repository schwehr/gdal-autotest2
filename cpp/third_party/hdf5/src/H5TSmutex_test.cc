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
// Tests internal mutex init, trylock, and destroy operations
// (third_party/hdf5/src/H5TSmutex.c). Built on POSIX pthreads in google3.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5TSmutex.c

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

#include <dlfcn.h>
#include <errno.h>

#include <atomic>
#include <thread>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/h5ts_c_shim.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(leak_sanitizer) || \
    __has_feature(hwaddress_sanitizer) || __has_feature(memory_sanitizer) || \
    __has_feature(thread_sanitizer)
#define H5TS_MUTEX_TEST_HAS_SANITIZER 1
#endif
#endif
#if !defined(H5TS_MUTEX_TEST_HAS_SANITIZER) &&                              \
    (defined(ADDRESS_SANITIZER) || defined(__SANITIZE_ADDRESS__) ||         \
     defined(LEAK_SANITIZER) || defined(HWADDRESS_SANITIZER) ||             \
     defined(__SANITIZE_HWADDRESS__) || defined(MEMORY_SANITIZER) ||        \
     defined(THREAD_SANITIZER) || defined(__SANITIZE_THREAD__))
#define H5TS_MUTEX_TEST_HAS_SANITIZER 1
#endif

#if !defined(H5TS_MUTEX_TEST_HAS_SANITIZER)
namespace {

enum class MutexFaultTarget {
  kNone = 0,
  kAttrInit,
  kAttrSetTypeRecursive,
  kAttrSetTypeNormal,
  kMutexInit,
  kAttrDestroy,
  kMutexTrylock,
  kMutexDestroy,
};

std::atomic<MutexFaultTarget> g_fault_target{MutexFaultTarget::kNone};
std::atomic<bool> g_fault_has_tid{false};
pthread_t g_fault_tid{};
std::atomic<pthread_mutex_t*> g_fault_mutex{nullptr};

bool IsTargetThread() {
  return g_fault_has_tid.load(std::memory_order_acquire) &&
         pthread_equal(pthread_self(), g_fault_tid) != 0;
}

template <typename Fn>
Fn ResolveNext(std::atomic<Fn>* slot, const char* name) {
  Fn fn = slot->load(std::memory_order_acquire);
  if (fn == nullptr) {
    fn = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, name));
    slot->store(fn, std::memory_order_release);
  }
  return fn;
}

}  // namespace

// Glibc's pthread_mutexattr_* and normal pthread_mutex_init never return errors
// for valid inputs; inject targeted single-call errors in non-sanitizer runs.
extern "C" int pthread_mutexattr_init(pthread_mutexattr_t* attr) __THROW {
  using Fn = int (*)(pthread_mutexattr_t*);
  static std::atomic<Fn> real_fn{nullptr};
  Fn fn = ResolveNext(&real_fn, "pthread_mutexattr_init");
  if (g_fault_target.load(std::memory_order_acquire) ==
          MutexFaultTarget::kAttrInit &&
      IsTargetThread()) {
    g_fault_target.store(MutexFaultTarget::kNone, std::memory_order_release);
    return ENOMEM;
  }
  return fn(attr);
}

extern "C" int pthread_mutexattr_settype(pthread_mutexattr_t* attr,
                                         int kind) __THROW {
  using Fn = int (*)(pthread_mutexattr_t*, int);
  static std::atomic<Fn> real_fn{nullptr};
  Fn fn = ResolveNext(&real_fn, "pthread_mutexattr_settype");
  const MutexFaultTarget target =
      g_fault_target.load(std::memory_order_acquire);
  if (IsTargetThread() &&
      ((target == MutexFaultTarget::kAttrSetTypeRecursive &&
        kind == PTHREAD_MUTEX_RECURSIVE) ||
       (target == MutexFaultTarget::kAttrSetTypeNormal &&
        kind == PTHREAD_MUTEX_NORMAL))) {
    g_fault_target.store(MutexFaultTarget::kNone, std::memory_order_release);
    return EINVAL;
  }
  return fn(attr, kind);
}

extern "C" int pthread_mutexattr_destroy(pthread_mutexattr_t* attr) __THROW {
  using Fn = int (*)(pthread_mutexattr_t*);
  static std::atomic<Fn> real_fn{nullptr};
  Fn fn = ResolveNext(&real_fn, "pthread_mutexattr_destroy");
  if (g_fault_target.load(std::memory_order_acquire) ==
          MutexFaultTarget::kAttrDestroy &&
      IsTargetThread()) {
    g_fault_target.store(MutexFaultTarget::kNone, std::memory_order_release);
    return EINVAL;
  }
  return fn(attr);
}

extern "C" int pthread_mutex_init(pthread_mutex_t* mutex,
                                  const pthread_mutexattr_t* attr) __THROW {
  using Fn = int (*)(pthread_mutex_t*, const pthread_mutexattr_t*);
  static std::atomic<Fn> real_fn{nullptr};
  Fn fn = ResolveNext(&real_fn, "pthread_mutex_init");
  pthread_mutex_t* target = g_fault_mutex.load(std::memory_order_acquire);
  if (g_fault_target.load(std::memory_order_acquire) ==
          MutexFaultTarget::kMutexInit &&
      target != nullptr && mutex == target) {
    g_fault_target.store(MutexFaultTarget::kNone, std::memory_order_release);
    return ENOMEM;
  }
  return fn(mutex, attr);
}

extern "C" int pthread_mutex_trylock(pthread_mutex_t* mutex) __THROWNL {
  using Fn = int (*)(pthread_mutex_t*);
  static std::atomic<Fn> real_fn{nullptr};
  Fn fn = ResolveNext(&real_fn, "pthread_mutex_trylock");
  pthread_mutex_t* target = g_fault_mutex.load(std::memory_order_acquire);
  if (g_fault_target.load(std::memory_order_acquire) ==
          MutexFaultTarget::kMutexTrylock &&
      target != nullptr && mutex == target) {
    g_fault_target.store(MutexFaultTarget::kNone, std::memory_order_release);
    return EINVAL;
  }
  return fn(mutex);
}

extern "C" int pthread_mutex_destroy(pthread_mutex_t* mutex) __THROW {
  using Fn = int (*)(pthread_mutex_t*);
  static std::atomic<Fn> real_fn{nullptr};
  Fn fn = ResolveNext(&real_fn, "pthread_mutex_destroy");
  pthread_mutex_t* target = g_fault_mutex.load(std::memory_order_acquire);
  if (g_fault_target.load(std::memory_order_acquire) ==
          MutexFaultTarget::kMutexDestroy &&
      target != nullptr && mutex == target) {
    g_fault_target.store(MutexFaultTarget::kNone, std::memory_order_release);
    return EBUSY;
  }
  return fn(mutex);
}
#endif

namespace hdf5 {
namespace {

herr_t TryLockNoThreadSafetyAnalysis(H5TS_mutex_t* mutex, bool* acquired)
    __attribute__((no_thread_safety_analysis)) {
  return H5TS_mutex_trylock(mutex, acquired);
}

TEST(H5TSmutexTest, PlainMutexInitTrylockContentionAndDestroy) {
  H5TS_mutex_t mutex{};
  ASSERT_GE(H5TS_mutex_init(&mutex, H5TS_MUTEX_TYPE_PLAIN), 0);
  bool destroyed = false;
  absl::Cleanup cleanup = [&] {
    if (!destroyed) {
      H5TS_mutex_destroy(&mutex);
    }
  };

  bool acquired = false;
  ASSERT_GE(TryLockNoThreadSafetyAnalysis(&mutex, &acquired), 0);
  ASSERT_TRUE(acquired);

  // Another thread observes EBUSY -> acquired == false while lock is held.
  std::thread contender([&mutex]() {
    bool other_acquired = true;
    EXPECT_GE(TryLockNoThreadSafetyAnalysis(&mutex, &other_acquired), 0);
    EXPECT_FALSE(other_acquired);
  });
  contender.join();

  ASSERT_GE(H5TS_shim_mutex_unlock(&mutex), 0);

  int shared_counter = 0;
  constexpr int kThreads = 4;
  constexpr int kIters = 200;
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&mutex, &shared_counter]() {
      for (int i = 0; i < kIters; ++i) {
        ASSERT_GE(H5TS_shim_mutex_lock(&mutex), 0);
        ++shared_counter;
        ASSERT_GE(H5TS_shim_mutex_unlock(&mutex), 0);
      }
    });
  }
  for (auto& w : workers) {
    w.join();
  }
  EXPECT_EQ(shared_counter, kThreads * kIters);

  EXPECT_GE(H5TS_mutex_destroy(&mutex), 0);
  destroyed = true;
}

TEST(H5TSmutexTest, RecursiveMutexAllowsNestedLockAndTrylock) {
  H5TS_mutex_t mutex{};
  ASSERT_GE(H5TS_mutex_init(&mutex, H5TS_MUTEX_TYPE_RECURSIVE), 0);
  bool destroyed = false;
  absl::Cleanup cleanup = [&] {
    if (!destroyed) {
      H5TS_mutex_destroy(&mutex);
    }
  };

  ASSERT_GE(H5TS_shim_mutex_lock(&mutex), 0);
  ASSERT_GE(H5TS_shim_mutex_lock(&mutex), 0);

  bool acquired = false;
  ASSERT_GE(TryLockNoThreadSafetyAnalysis(&mutex, &acquired), 0);
  EXPECT_TRUE(acquired);

  // Another thread still cannot acquire the recursively held mutex.
  std::thread contender([&mutex]() {
    bool other_acquired = true;
    EXPECT_GE(TryLockNoThreadSafetyAnalysis(&mutex, &other_acquired), 0);
    EXPECT_FALSE(other_acquired);
  });
  contender.join();

  EXPECT_GE(H5TS_shim_mutex_unlock(&mutex), 0);
  EXPECT_GE(H5TS_shim_mutex_unlock(&mutex), 0);
  EXPECT_GE(H5TS_shim_mutex_unlock(&mutex), 0);

  // After all recursive locks are released, another thread can trylock it.
  std::thread successor([&mutex]() {
    bool other_acquired = false;
    ASSERT_GE(TryLockNoThreadSafetyAnalysis(&mutex, &other_acquired), 0);
    EXPECT_TRUE(other_acquired);
    EXPECT_GE(H5TS_shim_mutex_unlock(&mutex), 0);
  });
  successor.join();

  EXPECT_GE(H5TS_mutex_destroy(&mutex), 0);
  destroyed = true;
}

TEST(H5TSmutexTest, NonRecursiveTypeFallsBackToNormalMutex) {
  H5TS_mutex_t mutex{};
  ASSERT_GE(H5TS_mutex_init(&mutex, /*type=*/42), 0);
  absl::Cleanup cleanup = [&] { H5TS_mutex_destroy(&mutex); };

  ASSERT_GE(H5TS_shim_mutex_lock(&mutex), 0);
  std::thread contender([&mutex]() {
    bool other_acquired = true;
    EXPECT_GE(TryLockNoThreadSafetyAnalysis(&mutex, &other_acquired), 0);
    EXPECT_FALSE(other_acquired);
  });
  contender.join();
  EXPECT_GE(H5TS_shim_mutex_unlock(&mutex), 0);
}

#if !defined(H5TS_MUTEX_TEST_HAS_SANITIZER)
TEST(H5TSmutexTest, PropagatesPthreadInitTrylockAndDestroyFailures) {
  H5TS_mutex_t mutex{};
  g_fault_tid = pthread_self();
  g_fault_has_tid.store(true, std::memory_order_release);
  g_fault_mutex.store(&mutex, std::memory_order_release);
  absl::Cleanup clear_fault = [] {
    g_fault_target.store(MutexFaultTarget::kNone, std::memory_order_release);
    g_fault_has_tid.store(false, std::memory_order_release);
    g_fault_mutex.store(nullptr, std::memory_order_release);
  };

  g_fault_target.store(MutexFaultTarget::kAttrInit, std::memory_order_release);
  EXPECT_LT(H5TS_mutex_init(&mutex, H5TS_MUTEX_TYPE_PLAIN), 0);

  g_fault_target.store(MutexFaultTarget::kAttrSetTypeRecursive,
                       std::memory_order_release);
  EXPECT_LT(H5TS_mutex_init(&mutex, H5TS_MUTEX_TYPE_RECURSIVE), 0);

  g_fault_target.store(MutexFaultTarget::kAttrSetTypeNormal,
                       std::memory_order_release);
  EXPECT_LT(H5TS_mutex_init(&mutex, H5TS_MUTEX_TYPE_PLAIN), 0);

  g_fault_target.store(MutexFaultTarget::kMutexInit, std::memory_order_release);
  EXPECT_LT(H5TS_mutex_init(&mutex, H5TS_MUTEX_TYPE_PLAIN), 0);

  g_fault_target.store(MutexFaultTarget::kAttrDestroy,
                       std::memory_order_release);
  EXPECT_LT(H5TS_mutex_init(&mutex, H5TS_MUTEX_TYPE_PLAIN), 0);
  // mutex was initialized before pthread_mutexattr_destroy failed; destroy it.
  EXPECT_GE(H5TS_mutex_destroy(&mutex), 0);

  ASSERT_GE(H5TS_mutex_init(&mutex, H5TS_MUTEX_TYPE_PLAIN), 0);
  bool acquired = false;
  g_fault_target.store(MutexFaultTarget::kMutexTrylock,
                       std::memory_order_release);
  EXPECT_LT(TryLockNoThreadSafetyAnalysis(&mutex, &acquired), 0);

  g_fault_target.store(MutexFaultTarget::kMutexDestroy,
                       std::memory_order_release);
  EXPECT_LT(H5TS_mutex_destroy(&mutex), 0);

  // Actually destroy the mutex cleanly now that fault target cleared.
  EXPECT_GE(H5TS_mutex_destroy(&mutex), 0);
}
#endif

}  // namespace
}  // namespace hdf5
