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
// Tests non-recursive read/write lock initialization, destruction, and read,
// write, and try-write lock operations (third_party/hdf5/src/H5TSrwlock.c).
//
// Google3 build caveats: H5_HAVE_THREADS=1 on Linux uses pthread_rwlock_t;
// H5_HAVE_C11_THREADS, H5_HAVE_WIN_THREADS, and __MACH__ branches are not
// compiled. Because glibc's pthread_rwlock_init(..., NULL) and
// pthread_rwlock_destroy(...) unconditionally return 0, non-TSan builds
// interpose those two symbols to inject targeted errors on a single test-owned
// H5TS_rwlock_t pointer.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5TSrwlock.c
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5TSrwlock.h

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/h5ts_c_shim.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG; match its preprocessor state across
// hdf5_internal_prelude.h and H5TSprivate.h.
#ifndef NDEBUG
#define H5TS_RWLOCK_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#include "third_party/hdf5/src/H5TSprivate.h"
}  // extern "C"
#ifdef H5TS_RWLOCK_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5TS_RWLOCK_TEST_RESTORE_UNDEF_NDEBUG
#endif

#if defined(__SANITIZE_THREAD__)
#define H5TSRWLOCK_HAVE_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define H5TSRWLOCK_HAVE_TSAN 1
#endif
#endif

#ifndef H5TSRWLOCK_HAVE_TSAN
namespace {

std::atomic<const pthread_rwlock_t*> g_fail_rwlock_init_target{nullptr};
std::atomic<const pthread_rwlock_t*> g_fail_rwlock_destroy_target{nullptr};

using RwlockInitFn = int (*)(pthread_rwlock_t*, const pthread_rwlockattr_t*);
using RwlockDestroyFn = int (*)(pthread_rwlock_t*);

RwlockInitFn ResolveRealRwlockInit() {
  static std::atomic<RwlockInitFn> cached{nullptr};
  RwlockInitFn fn = cached.load(std::memory_order_acquire);
  if (fn != nullptr) return fn;
  void* sym = dlvsym(RTLD_NEXT, "pthread_rwlock_init", "GLIBC_2.34");
  if (sym == nullptr) {
    sym = dlsym(RTLD_NEXT, "pthread_rwlock_init");
  }
  fn = reinterpret_cast<RwlockInitFn>(sym);
  cached.store(fn, std::memory_order_release);
  return fn;
}

RwlockDestroyFn ResolveRealRwlockDestroy() {
  static std::atomic<RwlockDestroyFn> cached{nullptr};
  RwlockDestroyFn fn = cached.load(std::memory_order_acquire);
  if (fn != nullptr) return fn;
  void* sym = dlvsym(RTLD_NEXT, "pthread_rwlock_destroy", "GLIBC_2.34");
  if (sym == nullptr) {
    sym = dlsym(RTLD_NEXT, "pthread_rwlock_destroy");
  }
  fn = reinterpret_cast<RwlockDestroyFn>(sym);
  cached.store(fn, std::memory_order_release);
  return fn;
}

}  // namespace

extern "C" int pthread_rwlock_init(pthread_rwlock_t* rwlock,
                                   const pthread_rwlockattr_t* attr) __THROW {
  const pthread_rwlock_t* fail_target =
      g_fail_rwlock_init_target.load(std::memory_order_acquire);
  if (fail_target != nullptr && rwlock == fail_target) {
    return EINVAL;
  }
  std::memset(rwlock, 0, sizeof(*rwlock));
  RwlockInitFn real_fn = ResolveRealRwlockInit();
  return real_fn != nullptr ? real_fn(rwlock, attr) : ENOSYS;
}

extern "C" int pthread_rwlock_destroy(pthread_rwlock_t* rwlock) __THROW {
  const pthread_rwlock_t* fail_target =
      g_fail_rwlock_destroy_target.load(std::memory_order_acquire);
  if (fail_target != nullptr && rwlock == fail_target) {
    return EBUSY;
  }
  RwlockDestroyFn real_fn = ResolveRealRwlockDestroy();
  return real_fn != nullptr ? real_fn(rwlock) : ENOSYS;
}
#endif  // !H5TSRWLOCK_HAVE_TSAN

namespace hdf5 {
namespace {

TEST(H5TSrwlockTest, NullArgumentsReturnFailure) {
  EXPECT_LT(H5TS_rwlock_init(nullptr), 0);
  EXPECT_LT(H5TS_rwlock_destroy(nullptr), 0);

  bool acquired = true;
  H5TS_rwlock_t lock{};
  ASSERT_GE(H5TS_rwlock_init(&lock), 0);
  absl::Cleanup destroy = [&] { H5TS_rwlock_destroy(&lock); };

  EXPECT_LT(H5TS_shim_rwlock_rdlock(nullptr), 0);
  EXPECT_LT(H5TS_shim_rwlock_rdunlock(nullptr), 0);
  EXPECT_LT(H5TS_shim_rwlock_wrlock(nullptr), 0);
  EXPECT_LT(H5TS_shim_rwlock_trywrlock(nullptr, &acquired), 0);
  EXPECT_LT(H5TS_shim_rwlock_trywrlock(&lock, nullptr), 0);
  EXPECT_LT(H5TS_shim_rwlock_wrunlock(nullptr), 0);
}

TEST(H5TSrwlockTest, InitAndDestroyLifecycle) {
  H5TS_rwlock_t lock{};
  ASSERT_GE(H5TS_rwlock_init(&lock), 0);
  EXPECT_GE(H5TS_rwlock_destroy(&lock), 0);
}

#ifndef H5TSRWLOCK_HAVE_TSAN
TEST(H5TSrwlockTest, InitAndDestroyPropagatePthreadFailures) {
  H5TS_rwlock_t lock{};

  // 1. Inject pthread_rwlock_init() failure for &lock only.
  g_fail_rwlock_init_target.store(&lock, std::memory_order_release);
  {
    absl::Cleanup clear_init = [] {
      g_fail_rwlock_init_target.store(nullptr, std::memory_order_release);
    };
    EXPECT_LT(H5TS_rwlock_init(&lock), 0);
  }

  // 2. Initialize cleanly, then inject pthread_rwlock_destroy() failure.
  ASSERT_GE(H5TS_rwlock_init(&lock), 0);
  g_fail_rwlock_destroy_target.store(&lock, std::memory_order_release);
  {
    absl::Cleanup clear_destroy = [] {
      g_fail_rwlock_destroy_target.store(nullptr, std::memory_order_release);
    };
    EXPECT_LT(H5TS_rwlock_destroy(&lock), 0);
  }
  EXPECT_GE(H5TS_rwlock_destroy(&lock), 0);
}
#endif  // !H5TSRWLOCK_HAVE_TSAN

TEST(H5TSrwlockTest, TryWriteLockUncontendedAndContended) {
  H5TS_rwlock_t lock{};
  ASSERT_GE(H5TS_rwlock_init(&lock), 0);
  absl::Cleanup destroy = [&] { H5TS_rwlock_destroy(&lock); };

  // 1. Uncontended trywrlock acquires the write lock.
  bool acquired = false;
  ASSERT_GE(H5TS_shim_rwlock_trywrlock(&lock, &acquired), 0);
  EXPECT_TRUE(acquired);

  // 2. Contended trywrlock from a second thread reports acquired == false.
  bool worker_acquired = true;
  std::thread contender([&]() {
    EXPECT_GE(H5TS_shim_rwlock_trywrlock(&lock, &worker_acquired), 0);
  });
  contender.join();
  EXPECT_FALSE(worker_acquired);
  ASSERT_GE(H5TS_shim_rwlock_wrunlock(&lock), 0);

  // 3. While another thread holds a read lock, trywrlock returns false.
  std::mutex mu;
  std::condition_variable cv;
  bool reader_locked = false;
  bool reader_release = false;

  std::thread reader([&]() {
    ASSERT_GE(H5TS_shim_rwlock_rdlock(&lock), 0);
    {
      std::lock_guard<std::mutex> g(mu);
      reader_locked = true;
    }
    cv.notify_all();
    {
      std::unique_lock<std::mutex> ul(mu);
      cv.wait(ul, [&] { return reader_release; });
    }
    EXPECT_GE(H5TS_shim_rwlock_rdunlock(&lock), 0);
  });

  {
    std::unique_lock<std::mutex> ul(mu);
    cv.wait(ul, [&] { return reader_locked; });
  }
  EXPECT_GE(H5TS_shim_rwlock_trywrlock(&lock, &acquired), 0);
  EXPECT_FALSE(acquired);

  {
    std::lock_guard<std::mutex> g(mu);
    reader_release = true;
  }
  cv.notify_all();
  reader.join();

  // 4. After reader releases, trywrlock succeeds again.
  ASSERT_GE(H5TS_shim_rwlock_trywrlock(&lock, &acquired), 0);
  EXPECT_TRUE(acquired);
  EXPECT_GE(H5TS_shim_rwlock_wrunlock(&lock), 0);
}

TEST(H5TSrwlockTest, ConcurrentReadersAndExclusiveWriters) {
  H5TS_rwlock_t lock{};
  ASSERT_GE(H5TS_rwlock_init(&lock), 0);
  absl::Cleanup destroy = [&] { H5TS_rwlock_destroy(&lock); };

  int64_t a = 0;
  int64_t b = 0;
  std::atomic<int> mismatch_count{0};
  constexpr int kWriters = 2;
  constexpr int kReaders = 4;
  constexpr int kIterations = 100;

  std::vector<std::thread> threads;
  threads.reserve(kWriters + kReaders);

  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&]() {
      for (int i = 0; i < kIterations; ++i) {
        ASSERT_GE(H5TS_shim_rwlock_wrlock(&lock), 0);
        ++a;
        --b;
        EXPECT_GE(H5TS_shim_rwlock_wrunlock(&lock), 0);
      }
    });
  }

  for (int r = 0; r < kReaders; ++r) {
    threads.emplace_back([&]() {
      for (int i = 0; i < kIterations; ++i) {
        ASSERT_GE(H5TS_shim_rwlock_rdlock(&lock), 0);
        if (a + b != 0) {
          mismatch_count.fetch_add(1, std::memory_order_relaxed);
        }
        EXPECT_GE(H5TS_shim_rwlock_rdunlock(&lock), 0);
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(mismatch_count.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(a, kWriters * kIterations);
  EXPECT_EQ(b, -(kWriters * kIterations));
}

}  // namespace
}  // namespace hdf5
