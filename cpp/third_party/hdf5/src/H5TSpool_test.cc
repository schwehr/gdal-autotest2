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
// Tests internal thread pool creation, task queuing, worker execution, and
// teardown (third_party/hdf5/src/H5TSpool.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5TSpool.c

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

#include <dlfcn.h>
#include <errno.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/h5ts_c_shim.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(leak_sanitizer) || \
    __has_feature(hwaddress_sanitizer) || __has_feature(memory_sanitizer) || \
    __has_feature(thread_sanitizer)
#define H5TS_POOL_TEST_HAS_SANITIZER 1
#endif
#endif
#if !defined(H5TS_POOL_TEST_HAS_SANITIZER) &&                              \
    (defined(ADDRESS_SANITIZER) || defined(__SANITIZE_ADDRESS__) ||        \
     defined(LEAK_SANITIZER) || defined(HWADDRESS_SANITIZER) ||            \
     defined(__SANITIZE_HWADDRESS__) || defined(MEMORY_SANITIZER) ||       \
     defined(THREAD_SANITIZER) || defined(__SANITIZE_THREAD__))
#define H5TS_POOL_TEST_HAS_SANITIZER 1
#endif

#if !defined(H5TS_POOL_TEST_HAS_SANITIZER)
namespace {

enum class PoolFaultTarget {
  kNone = 0,
  kMutexInit,
  kSemInit,
  kThreadCreate,
  kSemWait,
  kSemPost,
  kThreadJoin,
  kSemDestroy,
  kMutexDestroy,
  kDestroyMutexLock,
  kDestroyMutexUnlockBoth,
  kWorkerMutexLock,
  kWorkerMutexUnlock,
};

std::atomic<PoolFaultTarget> g_pool_fault{PoolFaultTarget::kNone};
std::atomic<bool> g_has_main_tid{false};
pthread_t g_main_tid{};
std::atomic<int> g_destroy_unlock_remaining{0};

bool IsMainThread() {
  return g_has_main_tid.load(std::memory_order_acquire) &&
         pthread_equal(pthread_self(), g_main_tid) != 0;
}

template <typename Fn>
Fn ResolvePoolNext(std::atomic<Fn>* slot, const char* name) {
  Fn fn = slot->load(std::memory_order_acquire);
  if (fn == nullptr) {
    fn = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, name));
    slot->store(fn, std::memory_order_release);
  }
  return fn;
}

}  // namespace

extern "C" int pthread_mutex_init(pthread_mutex_t* mutex,
                                  const pthread_mutexattr_t* attr) __THROW {
  using Fn = int (*)(pthread_mutex_t*, const pthread_mutexattr_t*);
  static std::atomic<Fn> real_fn{nullptr};
  Fn fn = ResolvePoolNext(&real_fn, "pthread_mutex_init");
  if (g_pool_fault.load(std::memory_order_acquire) ==
      PoolFaultTarget::kMutexInit) {
    g_pool_fault.store(PoolFaultTarget::kNone, std::memory_order_release);
    return ENOMEM;
  }
  return fn(mutex, attr);
}

extern "C" int pthread_mutex_lock(pthread_mutex_t* mutex) __THROWNL {
  using Fn = int (*)(pthread_mutex_t*);
  static std::atomic<Fn> real_fn{nullptr};
  Fn fn = ResolvePoolNext(&real_fn, "pthread_mutex_lock");
  const PoolFaultTarget fault = g_pool_fault.load(std::memory_order_acquire);
  if (fault == PoolFaultTarget::kDestroyMutexLock && IsMainThread()) {
    g_pool_fault.store(PoolFaultTarget::kNone, std::memory_order_release);
    return EINVAL;
  }
  if (fault == PoolFaultTarget::kWorkerMutexLock &&
      g_has_main_tid.load(std::memory_order_acquire) && !IsMainThread()) {
    g_pool_fault.store(PoolFaultTarget::kNone, std::memory_order_release);
    return EINVAL;
  }
  return fn(mutex);
}

extern "C" int pthread_mutex_unlock(pthread_mutex_t* mutex) __THROWNL {
  using Fn = int (*)(pthread_mutex_t*);
  static std::atomic<Fn> real_fn{nullptr};
  Fn fn = ResolvePoolNext(&real_fn, "pthread_mutex_unlock");
  const PoolFaultTarget fault = g_pool_fault.load(std::memory_order_acquire);
  if (fault == PoolFaultTarget::kDestroyMutexUnlockBoth && IsMainThread()) {
    const int prev =
        g_destroy_unlock_remaining.fetch_sub(1, std::memory_order_acq_rel);
    // Release the lock once on the first call so subsequent teardown succeeds
    // without double-unlocking a PTHREAD_MUTEX_NORMAL mutex.
    if (prev == 2) {
      fn(mutex);
    }
    if (prev <= 1) {
      g_pool_fault.store(PoolFaultTarget::kNone, std::memory_order_release);
    }
    return EINVAL;
  }
  if (fault == PoolFaultTarget::kWorkerMutexUnlock &&
      g_has_main_tid.load(std::memory_order_acquire) && !IsMainThread()) {
    fn(mutex);
    g_pool_fault.store(PoolFaultTarget::kNone, std::memory_order_release);
    return EINVAL;
  }
  return fn(mutex);
}

extern "C" int pthread_mutex_destroy(pthread_mutex_t* mutex) __THROW {
  using Fn = int (*)(pthread_mutex_t*);
  static std::atomic<Fn> real_fn{nullptr};
  Fn fn = ResolvePoolNext(&real_fn, "pthread_mutex_destroy");
  if (g_pool_fault.load(std::memory_order_acquire) ==
      PoolFaultTarget::kMutexDestroy) {
    g_pool_fault.store(PoolFaultTarget::kNone, std::memory_order_release);
    return EBUSY;
  }
  return fn(mutex);
}

extern "C" int sem_init(sem_t* sem, int pshared, unsigned int value) __THROW {
  using Fn = int (*)(sem_t*, int, unsigned int);
  static std::atomic<Fn> real_fn{nullptr};
  Fn fn = ResolvePoolNext(&real_fn, "sem_init");
  if (g_pool_fault.load(std::memory_order_acquire) ==
      PoolFaultTarget::kSemInit) {
    g_pool_fault.store(PoolFaultTarget::kNone, std::memory_order_release);
    errno = ENOSPC;
    return -1;
  }
  return fn(sem, pshared, value);
}

extern "C" int sem_wait(sem_t* sem) {
  using Fn = int (*)(sem_t*);
  static std::atomic<Fn> real_fn{nullptr};
  Fn fn = ResolvePoolNext(&real_fn, "sem_wait");
  if (g_pool_fault.load(std::memory_order_acquire) ==
          PoolFaultTarget::kSemWait &&
      g_has_main_tid.load(std::memory_order_acquire) && !IsMainThread()) {
    g_pool_fault.store(PoolFaultTarget::kNone, std::memory_order_release);
    errno = EINVAL;
    return -1;
  }
  return fn(sem);
}

extern "C" int pthread_create(pthread_t* __restrict newthread,
                              const pthread_attr_t* __restrict attr,
                              void* (*start_routine)(void*),
                              void* __restrict arg) __THROWNL {
  using Fn = int (*)(pthread_t* __restrict, const pthread_attr_t* __restrict,
                     void* (*)(void*), void* __restrict);
  static std::atomic<Fn> real_fn{nullptr};
  Fn fn = ResolvePoolNext(&real_fn, "pthread_create");
  if (g_pool_fault.load(std::memory_order_acquire) ==
          PoolFaultTarget::kThreadCreate &&
      IsMainThread()) {
    g_pool_fault.store(PoolFaultTarget::kNone, std::memory_order_release);
    return EAGAIN;
  }
  return fn(newthread, attr, start_routine, arg);
}

extern "C" int sem_post(sem_t* sem) __THROWNL {
  using Fn = int (*)(sem_t*);
  static std::atomic<Fn> real_fn{nullptr};
  Fn fn = ResolvePoolNext(&real_fn, "sem_post");
  if (g_pool_fault.load(std::memory_order_acquire) ==
      PoolFaultTarget::kSemPost) {
    g_pool_fault.store(PoolFaultTarget::kNone, std::memory_order_release);
    errno = EOVERFLOW;
    return -1;
  }
  return fn(sem);
}

extern "C" int sem_destroy(sem_t* sem) __THROW {
  using Fn = int (*)(sem_t*);
  static std::atomic<Fn> real_fn{nullptr};
  Fn fn = ResolvePoolNext(&real_fn, "sem_destroy");
  if (g_pool_fault.load(std::memory_order_acquire) ==
      PoolFaultTarget::kSemDestroy) {
    g_pool_fault.store(PoolFaultTarget::kNone, std::memory_order_release);
    errno = EINVAL;
    return -1;
  }
  return fn(sem);
}

extern "C" int pthread_join(pthread_t thread, void** retval) {
  using Fn = int (*)(pthread_t, void**);
  static std::atomic<Fn> real_fn{nullptr};
  Fn fn = ResolvePoolNext(&real_fn, "pthread_join");
  if (g_pool_fault.load(std::memory_order_acquire) ==
      PoolFaultTarget::kThreadJoin) {
    g_pool_fault.store(PoolFaultTarget::kNone, std::memory_order_release);
    return EINVAL;
  }
  return fn(thread, retval);
}
#endif

namespace hdf5 {
namespace {

struct GateTaskContext {
  std::mutex mu;
  std::condition_variable cv;
  int entered = 0;
  bool release = false;
  std::atomic<int> completed{0};
};

void* GateTask(void* arg) {
  auto* ctx = static_cast<GateTaskContext*>(arg);
  {
    std::unique_lock<std::mutex> lock(ctx->mu);
    ++ctx->entered;
    ctx->cv.notify_all();
    ctx->cv.wait(lock, [ctx]() { return ctx->release; });
  }
  ctx->completed.fetch_add(1, std::memory_order_relaxed);
  return nullptr;
}

void* IncrementTask(void* arg) {
  auto* count = static_cast<std::atomic<int>*>(arg);
  count->fetch_add(1, std::memory_order_relaxed);
  return nullptr;
}

TEST(H5TSpoolTest, InvalidCreateDestroyAndAddTaskArgumentsFail) {
  H5TS_pool_t* pool = nullptr;
  EXPECT_LT(H5TS_pool_create(nullptr, 2), 0);
  EXPECT_LT(H5TS_pool_create(&pool, 0), 0);
  EXPECT_EQ(pool, nullptr);
  EXPECT_LT(H5TS_pool_destroy(nullptr), 0);
  EXPECT_LT(H5TS_shim_pool_add_task(nullptr, IncrementTask, nullptr), 0);
}

TEST(H5TSpoolTest, SingleThreadPoolCreateAndImmediateDestroy) {
  ASSERT_GE(H5open(), 0);
  H5TS_pool_t* pool = nullptr;
  ASSERT_GE(H5TS_pool_create(&pool, 1), 0);
  ASSERT_NE(pool, nullptr);
  absl::Cleanup destroy_pool = [&] {
    if (pool != nullptr) {
      H5TS_pool_destroy(pool);
    }
  };

  EXPECT_LT(H5TS_shim_pool_add_task(pool, nullptr, nullptr), 0);
  EXPECT_GE(H5TS_pool_destroy(pool), 0);
  pool = nullptr;
}

TEST(H5TSpoolTest, QueuedTasksExecuteAcrossWorkersAndDrainOnDestroy) {
  ASSERT_GE(H5open(), 0);
  constexpr unsigned kWorkers = 4;
  constexpr int kExtraTasks = 12;

  H5TS_pool_t* pool = nullptr;
  ASSERT_GE(H5TS_pool_create(&pool, kWorkers), 0);
  ASSERT_NE(pool, nullptr);
  absl::Cleanup destroy_pool = [&] {
    if (pool != nullptr) {
      H5TS_pool_destroy(pool);
    }
  };

  GateTaskContext gate;
  absl::Cleanup release_gate = [&] {
    std::lock_guard<std::mutex> lock(gate.mu);
    gate.release = true;
    gate.cv.notify_all();
  };

  // Pin all workers in GateTask so subsequent tasks form a multi-node queue,
  // exercising both non-null task->next and empty-tail transitions in
  // H5TS__pool_do.
  for (unsigned i = 0; i < kWorkers; ++i) {
    ASSERT_GE(H5TS_shim_pool_add_task(pool, GateTask, &gate), 0);
  }
  {
    std::unique_lock<std::mutex> lock(gate.mu);
    gate.cv.wait(lock, [&gate]() {
      return static_cast<unsigned>(gate.entered) == kWorkers;
    });
  }

  std::atomic<int> extra_completed{0};
  for (int i = 0; i < kExtraTasks; ++i) {
    ASSERT_GE(H5TS_shim_pool_add_task(pool, IncrementTask, &extra_completed),
              0);
  }

  {
    std::lock_guard<std::mutex> lock(gate.mu);
    gate.release = true;
    gate.cv.notify_all();
  }

  // H5TS_pool_destroy drains queued tasks before signaling workers to exit.
  EXPECT_GE(H5TS_pool_destroy(pool), 0);
  pool = nullptr;

  EXPECT_EQ(gate.completed.load(std::memory_order_relaxed),
            static_cast<int>(kWorkers));
  EXPECT_EQ(extra_completed.load(std::memory_order_relaxed), kExtraTasks);
}

#if !defined(H5TS_POOL_TEST_HAS_SANITIZER)
TEST(H5TSpoolTest, PropagatesPosixInitWorkerAndDestroyFailures) {
  const std::string prev_style = GTEST_FLAG_GET(death_test_style);
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  absl::Cleanup restore_style = [&] {
    GTEST_FLAG_SET(death_test_style, prev_style);
  };

  auto run_fault_checks = []() {
    if (H5open() < 0) std::exit(1);
    g_main_tid = pthread_self();
    g_has_main_tid.store(true, std::memory_order_release);

    // 1. H5TS_pool_create: mutex_init failure.
    H5TS_pool_t* pool = nullptr;
    g_pool_fault.store(PoolFaultTarget::kMutexInit, std::memory_order_release);
    if (H5TS_pool_create(&pool, 1) >= 0) std::exit(2);

    // 2. H5TS_pool_create: sem_init failure.
    g_pool_fault.store(PoolFaultTarget::kSemInit, std::memory_order_release);
    if (H5TS_pool_create(&pool, 1) >= 0) std::exit(3);

    // 3. H5TS_pool_create: pthread_create failure at u == 0 cleans up pool.
    g_pool_fault.store(PoolFaultTarget::kThreadCreate,
                       std::memory_order_release);
    if (H5TS_pool_create(&pool, 1) >= 0 || pool != nullptr) std::exit(31);

    // 4. H5TS__pool_do: sem_wait failure exits worker thread; destroy joins it.
    g_pool_fault.store(PoolFaultTarget::kSemWait, std::memory_order_release);
    if (H5TS_pool_create(&pool, 1) < 0) std::exit(4);
    while (g_pool_fault.load(std::memory_order_acquire) !=
           PoolFaultTarget::kNone) {
      H5TS_thread_yield();
    }
    if (H5TS_pool_destroy(pool) < 0) std::exit(5);

    // 4. H5TS_pool_destroy: sem_post failure.
    if (H5TS_pool_create(&pool, 1) < 0) std::exit(6);
    g_pool_fault.store(PoolFaultTarget::kSemPost, std::memory_order_release);
    if (H5TS_pool_destroy(pool) >= 0) std::exit(7);

    // 5. H5TS__pool_free: thread_join failure.
    if (H5TS_pool_create(&pool, 1) < 0) std::exit(8);
    g_pool_fault.store(PoolFaultTarget::kThreadJoin, std::memory_order_release);
    if (H5TS_pool_destroy(pool) >= 0) std::exit(9);

    // 6. H5TS__pool_free: sem_destroy failure.
    if (H5TS_pool_create(&pool, 1) < 0) std::exit(10);
    g_pool_fault.store(PoolFaultTarget::kSemDestroy, std::memory_order_release);
    if (H5TS_pool_destroy(pool) >= 0) std::exit(11);

    // 7. H5TS__pool_free: mutex_destroy failure.
    if (H5TS_pool_create(&pool, 1) < 0) std::exit(12);
    g_pool_fault.store(PoolFaultTarget::kMutexDestroy,
                       std::memory_order_release);
    if (H5TS_pool_destroy(pool) >= 0) std::exit(13);

    // 8. H5TS_pool_destroy: mutex_lock failure.
    if (H5TS_pool_create(&pool, 1) < 0) std::exit(14);
    g_pool_fault.store(PoolFaultTarget::kDestroyMutexLock,
                       std::memory_order_release);
    if (H5TS_pool_destroy(pool) >= 0) std::exit(15);
    if (H5TS_pool_destroy(pool) < 0) std::exit(16);

    // 9. H5TS_pool_destroy: both main-path and cleanup mutex_unlock failures.
    if (H5TS_pool_create(&pool, 1) < 0) std::exit(17);
    g_destroy_unlock_remaining.store(2, std::memory_order_release);
    g_pool_fault.store(PoolFaultTarget::kDestroyMutexUnlockBoth,
                       std::memory_order_release);
    if (H5TS_pool_destroy(pool) >= 0) std::exit(18);
    if (H5TS_pool_destroy(pool) < 0) std::exit(19);

    // 10. H5TS__pool_do: worker mutex_lock failure exits worker cleanly.
    if (H5TS_pool_create(&pool, 1) < 0) std::exit(20);
    std::atomic<int> warmup{0};
    if (H5TS_shim_pool_add_task(pool, IncrementTask, &warmup) < 0) {
      std::exit(21);
    }
    while (warmup.load(std::memory_order_acquire) == 0) {
      H5TS_thread_yield();
    }
    g_pool_fault.store(PoolFaultTarget::kWorkerMutexLock,
                       std::memory_order_release);
    if (H5TS_shim_pool_add_task(pool, IncrementTask, &warmup) < 0) {
      std::exit(22);
    }
    while (g_pool_fault.load(std::memory_order_acquire) !=
           PoolFaultTarget::kNone) {
      H5TS_thread_yield();
    }
    if (H5TS_pool_destroy(pool) < 0) std::exit(23);

    // 11. H5TS__pool_do: worker mutex_unlock failure on non-empty queue.
    if (H5TS_pool_create(&pool, 1) < 0) std::exit(24);
    warmup.store(0, std::memory_order_release);
    if (H5TS_shim_pool_add_task(pool, IncrementTask, &warmup) < 0) {
      std::exit(25);
    }
    while (warmup.load(std::memory_order_acquire) == 0) {
      H5TS_thread_yield();
    }
    g_pool_fault.store(PoolFaultTarget::kWorkerMutexUnlock,
                       std::memory_order_release);
    if (H5TS_shim_pool_add_task(pool, IncrementTask, &warmup) < 0) {
      std::exit(26);
    }
    while (g_pool_fault.load(std::memory_order_acquire) !=
           PoolFaultTarget::kNone) {
      H5TS_thread_yield();
    }
    if (H5TS_pool_destroy(pool) < 0) std::exit(27);

    // 12. H5TS__pool_do: worker mutex_unlock failure on empty shutdown path.
    if (H5TS_pool_create(&pool, 1) < 0) std::exit(28);
    warmup.store(0, std::memory_order_release);
    if (H5TS_shim_pool_add_task(pool, IncrementTask, &warmup) < 0) {
      std::exit(29);
    }
    while (warmup.load(std::memory_order_acquire) == 0) {
      H5TS_thread_yield();
    }
    g_pool_fault.store(PoolFaultTarget::kWorkerMutexUnlock,
                       std::memory_order_release);
    if (H5TS_pool_destroy(pool) < 0) std::exit(30);

    std::exit(0);
  };

  EXPECT_EXIT(run_fault_checks(), ::testing::ExitedWithCode(0), "");
}
#endif

}  // namespace
}  // namespace hdf5
