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
// Tests internal recursive read/write locks: initialization, destruction,
// recursive read and write locking, reader/writer contention and handoff, and
// invalid state / pthread error paths (third_party/hdf5/src/H5TSrec_rwlock.c).
//
// Google3 build caveats: H5_HAVE_THREADS=1 (POSIX pthreads),
// H5_HAVE_WIN_THREADS is undefined, and H5TS_ENABLE_REC_RWLOCK_STATS=0
// because H5TS_DEBUG is not defined in :hdf5. H5TS__rec_rwlock_rdlock allocates
// a thread-local counter freed by pthread key destructors on thread exit, so
// tests acquire read locks on joined worker threads for zero ASan/LSan leaks.
// Because glibc's pthread_cond_* routines do not fail under normal inputs,
// non-TSan builds interpose specific target addresses via RTLD_NEXT to verify
// condition-variable error propagation.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5TSrec_rwlock.c

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>

#include <atomic>
#include <chrono>
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
// hdf5_internal_prelude.h and H5TSpkg.h so internal struct layouts match.
#ifndef NDEBUG
#define H5TS_REC_RWLOCK_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5TS_FRIEND
#include "third_party/hdf5/src/H5TSpkg.h"
}  // extern "C"
#ifdef H5TS_REC_RWLOCK_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5TS_REC_RWLOCK_TEST_RESTORE_UNDEF_NDEBUG
#endif

#if defined(__SANITIZE_THREAD__)
#define H5TS_REC_RWLOCK_HAVE_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define H5TS_REC_RWLOCK_HAVE_TSAN 1
#endif
#endif

#ifndef H5TS_REC_RWLOCK_HAVE_TSAN
namespace {

std::atomic<const pthread_cond_t*> g_fail_cond_init_target{nullptr};
std::atomic<const pthread_cond_t*> g_fail_cond_destroy_target{nullptr};
std::atomic<const pthread_cond_t*> g_fail_cond_wait_target{nullptr};
std::atomic<const pthread_cond_t*> g_fail_cond_signal_target{nullptr};
std::atomic<const pthread_cond_t*> g_fail_cond_broadcast_target{nullptr};

template <typename FnPtr>
FnPtr ResolveCondSym(std::atomic<FnPtr>& cache, const char* name) {
  FnPtr fn = cache.load(std::memory_order_acquire);
  if (fn != nullptr) return fn;
  // Prefer GLIBC_2.3.2 on x86_64 glibc so dlsym does not resolve the legacy
  // GLIBC_2.2.5 condvar compat symbol; fall back on arm64/newer glibc.
  void* sym = dlvsym(RTLD_NEXT, name, "GLIBC_2.3.2");
  if (sym == nullptr) {
    sym = dlsym(RTLD_NEXT, name);
  }
  fn = reinterpret_cast<FnPtr>(sym);
  cache.store(fn, std::memory_order_release);
  return fn;
}

}  // namespace

extern "C" int pthread_cond_init(pthread_cond_t* cond,
                                 const pthread_condattr_t* attr) __THROW {
  if (g_fail_cond_init_target.load(std::memory_order_acquire) == cond) {
    return EINVAL;
  }
  std::memset(cond, 0, sizeof(*cond));
  using Fn = int (*)(pthread_cond_t*, const pthread_condattr_t*);
  static std::atomic<Fn> cache{nullptr};
  Fn real_fn = ResolveCondSym(cache, "pthread_cond_init");
  return real_fn != nullptr ? real_fn(cond, attr) : ENOSYS;
}

extern "C" int pthread_cond_destroy(pthread_cond_t* cond) __THROW {
  if (g_fail_cond_destroy_target.load(std::memory_order_acquire) == cond) {
    return EBUSY;
  }
  using Fn = int (*)(pthread_cond_t*);
  static std::atomic<Fn> cache{nullptr};
  Fn real_fn = ResolveCondSym(cache, "pthread_cond_destroy");
  return real_fn != nullptr ? real_fn(cond) : ENOSYS;
}

extern "C" int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
  if (g_fail_cond_wait_target.load(std::memory_order_acquire) == cond) {
    return EINVAL;
  }
  using Fn = int (*)(pthread_cond_t*, pthread_mutex_t*);
  static std::atomic<Fn> cache{nullptr};
  Fn real_fn = ResolveCondSym(cache, "pthread_cond_wait");
  return real_fn != nullptr ? real_fn(cond, mutex) : ENOSYS;
}

extern "C" int pthread_cond_signal(pthread_cond_t* cond) __THROW {
  if (g_fail_cond_signal_target.load(std::memory_order_acquire) == cond) {
    return EINVAL;
  }
  using Fn = int (*)(pthread_cond_t*);
  static std::atomic<Fn> cache{nullptr};
  Fn real_fn = ResolveCondSym(cache, "pthread_cond_signal");
  return real_fn != nullptr ? real_fn(cond) : ENOSYS;
}

extern "C" int pthread_cond_broadcast(pthread_cond_t* cond) __THROW {
  if (g_fail_cond_broadcast_target.load(std::memory_order_acquire) == cond) {
    return EINVAL;
  }
  using Fn = int (*)(pthread_cond_t*);
  static std::atomic<Fn> cache{nullptr};
  Fn real_fn = ResolveCondSym(cache, "pthread_cond_broadcast");
  return real_fn != nullptr ? real_fn(cond) : ENOSYS;
}
#endif  // !H5TS_REC_RWLOCK_HAVE_TSAN

namespace hdf5 {
namespace {

// Synchronizes under lock->mutex until waiting_writers_count reaches target,
// avoiding sleep guesswork and keeping TSan happens-before intact.
bool WaitForWaitingWriters(H5TS_rec_rwlock_t* lock, int32_t target) {
  for (;;) {
    if (H5TS_shim_mutex_lock(&lock->mutex) < 0) {
      return false;
    }
    const int32_t count = lock->waiting_writers_count;
    if (H5TS_shim_mutex_unlock(&lock->mutex) < 0) {
      return false;
    }
    if (count >= target) {
      return true;
    }
    std::this_thread::yield();
  }
}

TEST(H5TSrecRwlockTest, NullArgumentsReturnFailure) {
  EXPECT_LT(H5TS__rec_rwlock_init(nullptr), 0);
  EXPECT_LT(H5TS__rec_rwlock_destroy(nullptr), 0);
  EXPECT_LT(H5TS__rec_rwlock_rdlock(nullptr), 0);
  EXPECT_LT(H5TS__rec_rwlock_wrlock(nullptr), 0);
  EXPECT_LT(H5TS__rec_rwlock_rdunlock(nullptr), 0);
  EXPECT_LT(H5TS__rec_rwlock_wrunlock(nullptr), 0);
}

TEST(H5TSrecRwlockTest, InitAndDestroyUnusedLockWithoutReaderKey) {
  H5TS_rec_rwlock_t lock{};
  ASSERT_GE(H5TS__rec_rwlock_init(&lock), 0);
  EXPECT_EQ(lock.lock_type, H5TS_REC_RWLOCK_UNUSED);
  EXPECT_FALSE(lock.is_key_registered);
  EXPECT_EQ(lock.rec_write_lock_count, 0);
  EXPECT_EQ(lock.reader_thread_count, 0);
  EXPECT_GE(H5TS__rec_rwlock_destroy(&lock), 0);
}

TEST(H5TSrecRwlockTest, UnlockUnusedLockFails) {
  H5TS_rec_rwlock_t lock{};
  ASSERT_GE(H5TS__rec_rwlock_init(&lock), 0);
  absl::Cleanup destroy = [&] { H5TS__rec_rwlock_destroy(&lock); };

  EXPECT_LT(H5TS__rec_rwlock_rdunlock(&lock), 0);
  EXPECT_LT(H5TS__rec_rwlock_wrunlock(&lock), 0);
}

TEST(H5TSrecRwlockTest, RecursiveWriteLockAndInvalidModeTransitions) {
  H5TS_rec_rwlock_t lock{};
  ASSERT_GE(H5TS__rec_rwlock_init(&lock), 0);
  absl::Cleanup destroy = [&] { H5TS__rec_rwlock_destroy(&lock); };

  // 1. Acquire write lock recursively three times on the same thread.
  ASSERT_GE(H5TS__rec_rwlock_wrlock(&lock), 0);
  EXPECT_EQ(lock.lock_type, H5TS_REC_RWLOCK_WRITE);
  EXPECT_EQ(lock.rec_write_lock_count, 1);

  ASSERT_GE(H5TS__rec_rwlock_wrlock(&lock), 0);
  ASSERT_GE(H5TS__rec_rwlock_wrlock(&lock), 0);
  EXPECT_EQ(lock.rec_write_lock_count, 3);

  // 2. Same thread holding a write lock cannot acquire a read lock or rdunlock.
  EXPECT_LT(H5TS__rec_rwlock_rdlock(&lock), 0);
  EXPECT_LT(H5TS__rec_rwlock_rdunlock(&lock), 0);

  // 3. Release two recursive levels; lock remains write-held until count == 0.
  EXPECT_GE(H5TS__rec_rwlock_wrunlock(&lock), 0);
  EXPECT_EQ(lock.lock_type, H5TS_REC_RWLOCK_WRITE);
  EXPECT_EQ(lock.rec_write_lock_count, 2);

  EXPECT_GE(H5TS__rec_rwlock_wrunlock(&lock), 0);
  EXPECT_EQ(lock.lock_type, H5TS_REC_RWLOCK_WRITE);
  EXPECT_EQ(lock.rec_write_lock_count, 1);

  EXPECT_GE(H5TS__rec_rwlock_wrunlock(&lock), 0);
  EXPECT_EQ(lock.lock_type, H5TS_REC_RWLOCK_UNUSED);
  EXPECT_EQ(lock.rec_write_lock_count, 0);

  // 4. Extra write unlock on unused lock fails.
  EXPECT_LT(H5TS__rec_rwlock_wrunlock(&lock), 0);
}

TEST(H5TSrecRwlockTest, RecursiveReadLockReentryAndErrorsOnWorkerThread) {
  H5TS_rec_rwlock_t lock{};
  ASSERT_GE(H5TS__rec_rwlock_init(&lock), 0);
  absl::Cleanup destroy = [&] { H5TS__rec_rwlock_destroy(&lock); };

  std::thread reader([&]() {
    // 1. Initial read lock registers the TLS key and allocates count (= 1).
    ASSERT_GE(H5TS__rec_rwlock_rdlock(&lock), 0);
    EXPECT_TRUE(lock.is_key_registered);
    EXPECT_EQ(lock.lock_type, H5TS_REC_RWLOCK_READ);
    EXPECT_EQ(lock.reader_thread_count, 1);

    // 2. Recursive read locks increment only the TLS depth counter.
    ASSERT_GE(H5TS__rec_rwlock_rdlock(&lock), 0);
    ASSERT_GE(H5TS__rec_rwlock_rdlock(&lock), 0);
    EXPECT_EQ(lock.reader_thread_count, 1);

    // 3. Thread holding a read lock cannot upgrade to write or call wrunlock.
    EXPECT_LT(H5TS__rec_rwlock_wrlock(&lock), 0);
    EXPECT_LT(H5TS__rec_rwlock_wrunlock(&lock), 0);

    // 4. Release all three recursive read locks.
    EXPECT_GE(H5TS__rec_rwlock_rdunlock(&lock), 0);
    EXPECT_EQ(lock.lock_type, H5TS_REC_RWLOCK_READ);
    EXPECT_GE(H5TS__rec_rwlock_rdunlock(&lock), 0);
    EXPECT_EQ(lock.lock_type, H5TS_REC_RWLOCK_READ);
    EXPECT_GE(H5TS__rec_rwlock_rdunlock(&lock), 0);
    EXPECT_EQ(lock.lock_type, H5TS_REC_RWLOCK_UNUSED);
    EXPECT_EQ(lock.reader_thread_count, 0);

    // 5. Re-acquiring on the same thread reuses the existing TLS slot (*count
    // == 0).
    ASSERT_GE(H5TS__rec_rwlock_rdlock(&lock), 0);
    EXPECT_EQ(lock.lock_type, H5TS_REC_RWLOCK_READ);
    EXPECT_GE(H5TS__rec_rwlock_rdunlock(&lock), 0);
  });
  reader.join();

  // Destroy verifies H5TS_key_delete() succeeds when is_key_registered is true.
  EXPECT_GE(H5TS__rec_rwlock_destroy(&lock), 0);
  ASSERT_GE(H5TS__rec_rwlock_init(&lock), 0);
}

TEST(H5TSrecRwlockTest, ConcurrentReadersAndNonReaderRdunlockFails) {
  H5TS_rec_rwlock_t lock{};
  ASSERT_GE(H5TS__rec_rwlock_init(&lock), 0);
  absl::Cleanup destroy = [&] { H5TS__rec_rwlock_destroy(&lock); };

  std::mutex mu;
  std::condition_variable cv;
  bool r1_locked = false;
  bool r2_locked = false;
  bool nonreader_checked = false;
  bool r1_release = false;

  // 1. Reader 1 acquires the first read lock and registers the TLS key.
  std::thread reader1([&]() {
    ASSERT_GE(H5TS__rec_rwlock_rdlock(&lock), 0);
    {
      std::lock_guard<std::mutex> g(mu);
      r1_locked = true;
    }
    cv.notify_all();

    {
      std::unique_lock<std::mutex> ul(mu);
      cv.wait(ul, [&] { return r1_release; });
    }
    EXPECT_GE(H5TS__rec_rwlock_rdunlock(&lock), 0);
  });

  // 2. Reader 2 concurrently acquires a read lock after key registration.
  std::thread reader2([&]() {
    {
      std::unique_lock<std::mutex> ul(mu);
      cv.wait(ul, [&] { return r1_locked; });
    }
    ASSERT_GE(H5TS__rec_rwlock_rdlock(&lock), 0);
    {
      std::lock_guard<std::mutex> g(mu);
      r2_locked = true;
    }
    cv.notify_all();

    {
      std::unique_lock<std::mutex> ul(mu);
      cv.wait(ul, [&] { return nonreader_checked; });
    }
    EXPECT_GE(H5TS__rec_rwlock_rdunlock(&lock), 0);
  });

  {
    std::unique_lock<std::mutex> ul(mu);
    cv.wait(ul, [&] { return r1_locked && r2_locked; });
  }

  // 3. A thread without a TLS read-lock allocation (count == NULL) must fail
  // rdunlock even while lock_type == H5TS_REC_RWLOCK_READ.
  std::thread non_reader(
      [&]() { EXPECT_LT(H5TS__rec_rwlock_rdunlock(&lock), 0); });
  non_reader.join();

  // 4. Release reader 1 before reader 2 so reader_thread_count steps 2 -> 1 ->
  // 0.
  {
    std::lock_guard<std::mutex> g(mu);
    r1_release = true;
  }
  cv.notify_all();
  reader1.join();

  {
    std::lock_guard<std::mutex> g(mu);
    nonreader_checked = true;
  }
  cv.notify_all();
  reader2.join();

  EXPECT_EQ(lock.lock_type, H5TS_REC_RWLOCK_UNUSED);
}

TEST(H5TSrecRwlockTest, WaitingWriterWakesWhenLastReaderUnlocks) {
  H5TS_rec_rwlock_t lock{};
  ASSERT_GE(H5TS__rec_rwlock_init(&lock), 0);
  absl::Cleanup destroy = [&] { H5TS__rec_rwlock_destroy(&lock); };

  std::mutex mu;
  std::condition_variable cv;
  bool reader_acquired = false;
  bool writer_starting = false;
  int shared_value = 0;

  std::thread reader([&]() {
    ASSERT_GE(H5TS__rec_rwlock_rdlock(&lock), 0);
    EXPECT_EQ(shared_value, 0);
    {
      std::lock_guard<std::mutex> g(mu);
      reader_acquired = true;
    }
    cv.notify_all();

    {
      std::unique_lock<std::mutex> ul(mu);
      cv.wait(ul, [&] { return writer_starting; });
    }
    // Wait until writer is blocked inside cond_wait(&lock.writers_cv).
    ASSERT_TRUE(WaitForWaitingWriters(&lock, 1));
    EXPECT_GE(H5TS__rec_rwlock_rdunlock(&lock), 0);
  });

  std::thread writer([&]() {
    {
      std::unique_lock<std::mutex> ul(mu);
      cv.wait(ul, [&] { return reader_acquired; });
      writer_starting = true;
    }
    cv.notify_all();

    ASSERT_GE(H5TS__rec_rwlock_wrlock(&lock), 0);
    shared_value = 42;
    EXPECT_GE(H5TS__rec_rwlock_wrunlock(&lock), 0);
  });

  reader.join();
  writer.join();
  EXPECT_EQ(shared_value, 42);
}

TEST(H5TSrecRwlockTest, WriterContentionAndReaderBroadcastOnWriteUnlock) {
  H5TS_rec_rwlock_t lock{};
  ASSERT_GE(H5TS__rec_rwlock_init(&lock), 0);
  absl::Cleanup destroy = [&] { H5TS__rec_rwlock_destroy(&lock); };

  std::atomic<bool> w2_started{false};
  std::atomic<bool> readers_started{false};
  int shared_value = 0;

  // 1. Hold write lock on main thread while a second writer queues on
  // writers_cv.
  ASSERT_GE(H5TS__rec_rwlock_wrlock(&lock), 0);
  shared_value = 10;

  std::thread writer2([&]() {
    w2_started.store(true, std::memory_order_release);
    ASSERT_GE(H5TS__rec_rwlock_wrlock(&lock), 0);
    shared_value += 20;

    while (!readers_started.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_GE(H5TS__rec_rwlock_wrunlock(&lock), 0);
  });

  while (!w2_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  ASSERT_TRUE(WaitForWaitingWriters(&lock, 1));

  // 2. Spawn readers that encounter lock_type == WRITE and wait on readers_cv.
  std::vector<int> observed(2, 0);
  std::vector<std::thread> readers;
  readers.reserve(2);
  std::atomic<int> r_entered{0};
  for (int i = 0; i < 2; ++i) {
    readers.emplace_back([&, i]() {
      r_entered.fetch_add(1, std::memory_order_release);
      ASSERT_GE(H5TS__rec_rwlock_rdlock(&lock), 0);
      observed[i] = shared_value;
      EXPECT_GE(H5TS__rec_rwlock_rdunlock(&lock), 0);
    });
  }

  while (r_entered.load(std::memory_order_acquire) < 2) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  readers_started.store(true, std::memory_order_release);

  // 3. Main writer unlocks -> signals writer2 -> writer2 unlocks -> broadcasts
  // readers.
  ASSERT_GE(H5TS__rec_rwlock_wrunlock(&lock), 0);

  writer2.join();
  for (auto& r : readers) {
    r.join();
  }
  EXPECT_EQ(observed[0], 30);
  EXPECT_EQ(observed[1], 30);
}

TEST(H5TSrecRwlockTest, ExhaustingPthreadKeysCausesRdlockFailure) {
  H5TS_rec_rwlock_t lock{};
  ASSERT_GE(H5TS__rec_rwlock_init(&lock), 0);
  absl::Cleanup destroy = [&] { H5TS__rec_rwlock_destroy(&lock); };

  // 1. Temporarily exhaust all available pthread TLS keys and restore them on
  // scope exit so subsequent tests are unaffected.
  std::vector<pthread_key_t> keys;
  keys.reserve(PTHREAD_KEYS_MAX);
  absl::Cleanup release_keys = [&keys] {
    for (pthread_key_t key : keys) {
      pthread_key_delete(key);
    }
  };

  bool exhausted = false;
  for (int i = 0; i <= PTHREAD_KEYS_MAX; ++i) {
    pthread_key_t key{};
    if (pthread_key_create(&key, nullptr) != 0) {
      exhausted = true;
      break;
    }
    keys.push_back(key);
  }
  ASSERT_TRUE(exhausted);

  // 2. First rdlock() cannot register its TLS key and fails cleanly.
  EXPECT_LT(H5TS__rec_rwlock_rdlock(&lock), 0);
  EXPECT_FALSE(lock.is_key_registered);
  EXPECT_EQ(lock.lock_type, H5TS_REC_RWLOCK_UNUSED);
}

#ifndef H5TS_REC_RWLOCK_HAVE_TSAN
TEST(H5TSrecRwlockTest, InitAndDestroyPropagateCondFailures) {
  H5TS_rec_rwlock_t lock{};

  // 1. Failure during H5TS_cond_init(&lock.writers_cv); clean up mutex.
  g_fail_cond_init_target.store(&lock.writers_cv, std::memory_order_release);
  {
    absl::Cleanup clear = [] {
      g_fail_cond_init_target.store(nullptr, std::memory_order_release);
    };
    EXPECT_LT(H5TS__rec_rwlock_init(&lock), 0);
  }
  EXPECT_GE(H5TS_mutex_destroy(&lock.mutex), 0);

  // 2. Failure during H5TS_cond_init(&lock.readers_cv); clean up mutex + cv.
  g_fail_cond_init_target.store(&lock.readers_cv, std::memory_order_release);
  {
    absl::Cleanup clear = [] {
      g_fail_cond_init_target.store(nullptr, std::memory_order_release);
    };
    EXPECT_LT(H5TS__rec_rwlock_init(&lock), 0);
  }
  EXPECT_GE(H5TS_cond_destroy(&lock.writers_cv), 0);
  EXPECT_GE(H5TS_mutex_destroy(&lock.mutex), 0);

  // 3. Inject H5TS_cond_destroy(&lock.readers_cv) failure.
  ASSERT_GE(H5TS__rec_rwlock_init(&lock), 0);
  g_fail_cond_destroy_target.store(&lock.readers_cv, std::memory_order_release);
  {
    absl::Cleanup clear = [] {
      g_fail_cond_destroy_target.store(nullptr, std::memory_order_release);
    };
    EXPECT_LT(H5TS__rec_rwlock_destroy(&lock), 0);
  }
  EXPECT_GE(H5TS_cond_destroy(&lock.readers_cv), 0);

  // 4. Inject H5TS_cond_destroy(&lock.writers_cv) failure.
  ASSERT_GE(H5TS__rec_rwlock_init(&lock), 0);
  g_fail_cond_destroy_target.store(&lock.writers_cv, std::memory_order_release);
  {
    absl::Cleanup clear = [] {
      g_fail_cond_destroy_target.store(nullptr, std::memory_order_release);
    };
    EXPECT_LT(H5TS__rec_rwlock_destroy(&lock), 0);
  }
  EXPECT_GE(H5TS_cond_destroy(&lock.writers_cv), 0);
}

TEST(H5TSrecRwlockTest, LockAndUnlockPropagateCondFailures) {
  H5TS_rec_rwlock_t lock{};
  ASSERT_GE(H5TS__rec_rwlock_init(&lock), 0);
  absl::Cleanup destroy = [&] { H5TS__rec_rwlock_destroy(&lock); };

  // 1. While main thread holds wrlock, fail cond_wait on readers_cv and
  // writers_cv in worker threads so waiting loops propagate failure cleanly.
  ASSERT_GE(H5TS__rec_rwlock_wrlock(&lock), 0);

  g_fail_cond_wait_target.store(&lock.readers_cv, std::memory_order_release);
  std::thread failing_reader(
      [&]() { EXPECT_LT(H5TS__rec_rwlock_rdlock(&lock), 0); });
  failing_reader.join();
  g_fail_cond_wait_target.store(nullptr, std::memory_order_release);

  g_fail_cond_wait_target.store(&lock.writers_cv, std::memory_order_release);
  std::thread failing_writer(
      [&]() { EXPECT_LT(H5TS__rec_rwlock_wrlock(&lock), 0); });
  failing_writer.join();
  g_fail_cond_wait_target.store(nullptr, std::memory_order_release);

  // 2. Fail cond_broadcast(&lock.readers_cv) during wrunlock().
  g_fail_cond_broadcast_target.store(&lock.readers_cv,
                                     std::memory_order_release);
  {
    absl::Cleanup clear = [] {
      g_fail_cond_broadcast_target.store(nullptr, std::memory_order_release);
    };
    EXPECT_LT(H5TS__rec_rwlock_wrunlock(&lock), 0);
  }
  EXPECT_EQ(lock.lock_type, H5TS_REC_RWLOCK_UNUSED);

  // 3. Fail cond_signal(&lock.writers_cv) during wrunlock() with a waiting
  // writer, then wake the waiting writer cleanly.
  ASSERT_GE(H5TS__rec_rwlock_wrlock(&lock), 0);
  std::atomic<bool> w_started{false};
  std::thread queued_writer1([&]() {
    w_started.store(true, std::memory_order_release);
    ASSERT_GE(H5TS__rec_rwlock_wrlock(&lock), 0);
    EXPECT_GE(H5TS__rec_rwlock_wrunlock(&lock), 0);
  });
  while (!w_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  ASSERT_TRUE(WaitForWaitingWriters(&lock, 1));

  g_fail_cond_signal_target.store(&lock.writers_cv, std::memory_order_release);
  EXPECT_LT(H5TS__rec_rwlock_wrunlock(&lock), 0);
  g_fail_cond_signal_target.store(nullptr, std::memory_order_release);
  ASSERT_GE(H5TS_shim_mutex_lock(&lock.mutex), 0);
  EXPECT_GE(H5TS_shim_cond_signal(&lock.writers_cv), 0);
  ASSERT_GE(H5TS_shim_mutex_unlock(&lock.mutex), 0);
  queued_writer1.join();

  // 4. Fail cond_broadcast(&lock.readers_cv) during rdunlock().
  std::thread failing_rdunlock([&]() {
    ASSERT_GE(H5TS__rec_rwlock_rdlock(&lock), 0);
    g_fail_cond_broadcast_target.store(&lock.readers_cv,
                                       std::memory_order_release);
    absl::Cleanup clear = [] {
      g_fail_cond_broadcast_target.store(nullptr, std::memory_order_release);
    };
    EXPECT_LT(H5TS__rec_rwlock_rdunlock(&lock), 0);
  });
  failing_rdunlock.join();

  // 5. Fail cond_signal(&lock.writers_cv) during rdunlock() with a waiting
  // writer, then wake the waiting writer cleanly.
  std::mutex mu;
  std::condition_variable cv;
  bool r_locked = false;
  bool w_starting = false;
  std::thread reader_holder([&]() {
    ASSERT_GE(H5TS__rec_rwlock_rdlock(&lock), 0);
    {
      std::lock_guard<std::mutex> g(mu);
      r_locked = true;
    }
    cv.notify_all();
    {
      std::unique_lock<std::mutex> ul(mu);
      cv.wait(ul, [&] { return w_starting; });
    }
    ASSERT_TRUE(WaitForWaitingWriters(&lock, 1));
    g_fail_cond_signal_target.store(&lock.writers_cv,
                                    std::memory_order_release);
    EXPECT_LT(H5TS__rec_rwlock_rdunlock(&lock), 0);
    g_fail_cond_signal_target.store(nullptr, std::memory_order_release);
    ASSERT_GE(H5TS_shim_mutex_lock(&lock.mutex), 0);
    EXPECT_GE(H5TS_shim_cond_signal(&lock.writers_cv), 0);
    ASSERT_GE(H5TS_shim_mutex_unlock(&lock.mutex), 0);
  });

  std::thread queued_writer2([&]() {
    {
      std::unique_lock<std::mutex> ul(mu);
      cv.wait(ul, [&] { return r_locked; });
      w_starting = true;
    }
    cv.notify_all();
    ASSERT_GE(H5TS__rec_rwlock_wrlock(&lock), 0);
    EXPECT_GE(H5TS__rec_rwlock_wrunlock(&lock), 0);
  });

  reader_holder.join();
  queued_writer2.join();
}
#endif  // !H5TS_REC_RWLOCK_HAVE_TSAN

TEST(H5TSrecRwlockTest, MultiThreadMutualExclusionStress) {
  H5TS_rec_rwlock_t lock{};
  ASSERT_GE(H5TS__rec_rwlock_init(&lock), 0);
  absl::Cleanup destroy = [&] { H5TS__rec_rwlock_destroy(&lock); };

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
        ASSERT_GE(H5TS__rec_rwlock_wrlock(&lock), 0);
        ASSERT_GE(H5TS__rec_rwlock_wrlock(&lock), 0);
        ++a;
        --b;
        EXPECT_GE(H5TS__rec_rwlock_wrunlock(&lock), 0);
        EXPECT_GE(H5TS__rec_rwlock_wrunlock(&lock), 0);
      }
    });
  }

  for (int r = 0; r < kReaders; ++r) {
    threads.emplace_back([&]() {
      for (int i = 0; i < kIterations; ++i) {
        ASSERT_GE(H5TS__rec_rwlock_rdlock(&lock), 0);
        ASSERT_GE(H5TS__rec_rwlock_rdlock(&lock), 0);
        if (a + b != 0) {
          mismatch_count.fetch_add(1, std::memory_order_relaxed);
        }
        EXPECT_GE(H5TS__rec_rwlock_rdunlock(&lock), 0);
        EXPECT_GE(H5TS__rec_rwlock_rdunlock(&lock), 0);
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
