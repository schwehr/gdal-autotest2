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
// Tests internal HDF5 threadsafety runtime: global API locking, per-thread
// state/IDs/error stacks/API contexts, and package lifecycle
// (third_party/hdf5/src/H5TSint.c). Built with H5_HAVE_THREADSAFE in google3.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5TSint.c

#ifndef NDEBUG
#define H5TS_INT_TEST_DEFINED_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#ifdef H5TS_INT_TEST_DEFINED_NDEBUG
#undef NDEBUG
#undef H5TS_INT_TEST_DEFINED_NDEBUG
#endif

#include <dlfcn.h>
#include <errno.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/h5ts_c_shim.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(leak_sanitizer) || \
    __has_feature(hwaddress_sanitizer) || __has_feature(memory_sanitizer) || \
    __has_feature(thread_sanitizer)
#define H5TS_INT_TEST_HAS_SANITIZER 1
#endif
#endif
#if !defined(H5TS_INT_TEST_HAS_SANITIZER) &&                              \
    (defined(ADDRESS_SANITIZER) || defined(__SANITIZE_ADDRESS__) ||       \
     defined(LEAK_SANITIZER) || defined(HWADDRESS_SANITIZER) ||           \
     defined(__SANITIZE_HWADDRESS__) || defined(MEMORY_SANITIZER) ||      \
     defined(THREAD_SANITIZER) || defined(__SANITIZE_THREAD__))
#define H5TS_INT_TEST_HAS_SANITIZER 1
#endif

#ifndef NDEBUG
#define H5TS_INT_TEST_DEFINED_NDEBUG_PKG 1
#define NDEBUG 1
#endif
extern "C" {
#define H5E_FRIEND
#define H5TS_FRIEND
#include "third_party/hdf5/src/H5Epkg.h"
#include "third_party/hdf5/src/H5TSpkg.h"
}  // extern "C"
#ifdef H5TS_INT_TEST_DEFINED_NDEBUG_PKG
#undef NDEBUG
#undef H5TS_INT_TEST_DEFINED_NDEBUG_PKG
#endif

#if !defined(H5TS_INT_TEST_HAS_SANITIZER)
namespace {

enum class IntFaultTarget {
  kNone = 0,
  kOnce,
  kApiMutexInit,
  kApiMutexTrylock,
  kAnyMutexLock,
  kAnyMutexUnlock,
  kAnyMutexDestroy,
};

std::atomic<IntFaultTarget> g_int_fault{IntFaultTarget::kNone};
std::atomic<bool> g_int_has_tid{false};
pthread_t g_int_fault_tid{};

bool IsIntFaultThread() {
  return g_int_has_tid.load(std::memory_order_acquire) &&
         pthread_equal(pthread_self(), g_int_fault_tid) != 0;
}

template <typename Fn>
Fn ResolveIntNext(std::atomic<Fn>* slot, const char* name) {
  Fn fn = slot->load(std::memory_order_acquire);
  if (fn == nullptr) {
    fn = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, name));
    slot->store(fn, std::memory_order_release);
  }
  return fn;
}

}  // namespace

extern "C" int pthread_once(pthread_once_t* once_control,
                            void (*init_routine)(void)) {
  using Fn = int (*)(pthread_once_t*, void (*)(void));
  static std::atomic<Fn> real_fn{nullptr};
  Fn fn = ResolveIntNext(&real_fn, "pthread_once");
  if (g_int_fault.load(std::memory_order_acquire) == IntFaultTarget::kOnce &&
      IsIntFaultThread()) {
    g_int_fault.store(IntFaultTarget::kNone, std::memory_order_release);
    return EINVAL;
  }
  return fn(once_control, init_routine);
}

extern "C" int pthread_mutex_init(pthread_mutex_t* mutex,
                                  const pthread_mutexattr_t* attr) __THROW {
  using Fn = int (*)(pthread_mutex_t*, const pthread_mutexattr_t*);
  static std::atomic<Fn> real_fn{nullptr};
  Fn fn = ResolveIntNext(&real_fn, "pthread_mutex_init");
  if (g_int_fault.load(std::memory_order_acquire) ==
          IntFaultTarget::kApiMutexInit &&
      IsIntFaultThread() &&
      mutex ==
          reinterpret_cast<pthread_mutex_t*>(&H5TS_api_info_p.api_mutex)) {
    g_int_fault.store(IntFaultTarget::kNone, std::memory_order_release);
    return ENOMEM;
  }
  return fn(mutex, attr);
}

extern "C" int pthread_mutex_trylock(pthread_mutex_t* mutex) __THROWNL {
  using Fn = int (*)(pthread_mutex_t*);
  static std::atomic<Fn> real_fn{nullptr};
  Fn fn = ResolveIntNext(&real_fn, "pthread_mutex_trylock");
  if (g_int_fault.load(std::memory_order_acquire) ==
          IntFaultTarget::kApiMutexTrylock &&
      IsIntFaultThread() &&
      mutex ==
          reinterpret_cast<pthread_mutex_t*>(&H5TS_api_info_p.api_mutex)) {
    g_int_fault.store(IntFaultTarget::kNone, std::memory_order_release);
    return EINVAL;
  }
  return fn(mutex);
}

extern "C" int pthread_mutex_lock(pthread_mutex_t* mutex) __THROWNL {
  using Fn = int (*)(pthread_mutex_t*);
  static std::atomic<Fn> real_fn{nullptr};
  Fn fn = ResolveIntNext(&real_fn, "pthread_mutex_lock");
  if (g_int_fault.load(std::memory_order_acquire) ==
          IntFaultTarget::kAnyMutexLock &&
      IsIntFaultThread()) {
    g_int_fault.store(IntFaultTarget::kNone, std::memory_order_release);
    return EINVAL;
  }
  return fn(mutex);
}

extern "C" int pthread_mutex_unlock(pthread_mutex_t* mutex) __THROWNL {
  using Fn = int (*)(pthread_mutex_t*);
  static std::atomic<Fn> real_fn{nullptr};
  Fn fn = ResolveIntNext(&real_fn, "pthread_mutex_unlock");
  if (g_int_fault.load(std::memory_order_acquire) ==
          IntFaultTarget::kAnyMutexUnlock &&
      IsIntFaultThread()) {
    fn(mutex);
    g_int_fault.store(IntFaultTarget::kNone, std::memory_order_release);
    return EINVAL;
  }
  return fn(mutex);
}

extern "C" int pthread_mutex_destroy(pthread_mutex_t* mutex) __THROW {
  using Fn = int (*)(pthread_mutex_t*);
  static std::atomic<Fn> real_fn{nullptr};
  Fn fn = ResolveIntNext(&real_fn, "pthread_mutex_destroy");
  if (g_int_fault.load(std::memory_order_acquire) ==
          IntFaultTarget::kAnyMutexDestroy &&
      IsIntFaultThread()) {
    g_int_fault.store(IntFaultTarget::kNone, std::memory_order_release);
    return EBUSY;
  }
  return fn(mutex);
}
#endif

namespace hdf5 {
namespace {

TEST(H5TSintTest, ApiLockAndUnlockWhenLibraryClosedAndOpen) {
  // Closing the library clears H5_INIT_GLOBAL so H5TS_api_lock enters the
  // `!H5_INIT_GLOBAL` H5TS_once check before locking.
  ASSERT_GE(H5close(), 0);
  absl::Cleanup reopen = [] { H5open(); };

  const unsigned before_attempts = H5TS_shim_api_info_attempt_count();
  ASSERT_GE(H5TS_api_lock(), 0);
  EXPECT_GE(H5TS_shim_api_info_attempt_count(), before_attempts + 1);
  ASSERT_GE(H5TS_api_unlock(), 0);

  ASSERT_GE(H5open(), 0);
  const unsigned mid_attempts = H5TS_shim_api_info_attempt_count();
  ASSERT_GE(H5TS_api_lock(), 0);
  ASSERT_GE(H5TS_api_lock(), 0);
  EXPECT_GE(H5TS_shim_api_info_attempt_count(), mid_attempts + 2);
  EXPECT_GE(H5TS_api_unlock(), 0);
  EXPECT_GE(H5TS_api_unlock(), 0);
}

TEST(H5TSintTest, ApiMutexReleaseAndAcquireRecursiveAndContended) {
  ASSERT_GE(H5open(), 0);

  // Release with zero lock depth is a safe no-op returning 0.
  unsigned zero_count = 99;
  ASSERT_GE(H5TS__api_mutex_release(&zero_count), 0);
  EXPECT_EQ(zero_count, 0u);

  // Lock recursively (depth 2), release all, and re-acquire depth 2.
  ASSERT_GE(H5TS_api_lock(), 0);
  ASSERT_GE(H5TS_api_lock(), 0);

  unsigned released_depth = 0;
  ASSERT_GE(H5TS__api_mutex_release(&released_depth), 0);
  EXPECT_EQ(released_depth, 2u);

  bool acquired = false;
  ASSERT_GE(H5TS__api_mutex_acquire(released_depth, &acquired), 0);
  EXPECT_TRUE(acquired);

  // While this thread holds the global API lock, another thread's non-blocking
  // acquire must report acquired == false.
  std::thread contender([]() {
    bool other_acquired = true;
    EXPECT_GE(H5TS__api_mutex_acquire(1, &other_acquired), 0);
    EXPECT_FALSE(other_acquired);
  });
  contender.join();

  // Release both re-acquired locks cleanly.
  unsigned final_depth = 0;
  ASSERT_GE(H5TS__api_mutex_release(&final_depth), 0);
  EXPECT_EQ(final_depth, 2u);
}

TEST(H5TSintTest, ThreadIdNullCheckUniquenessAndNodeReuse) {
  ASSERT_GE(H5open(), 0);

  EXPECT_LT(H5TS_thread_id(nullptr), 0);

  uint64_t main_id1 = 0;
  uint64_t main_id2 = 0;
  ASSERT_GE(H5TS_thread_id(&main_id1), 0);
  ASSERT_GE(H5TS_thread_id(&main_id2), 0);
  EXPECT_GE(main_id1, 1u);
  EXPECT_EQ(main_id1, main_id2);

  // First worker allocates a new tinfo node and puts it on the free list upon
  // exit via H5TS__tinfo_destroy.
  uint64_t worker1_id = 0;
  uint64_t worker1_repeat_id = 0;
  std::thread t1([&]() {
    ASSERT_GE(H5TS_thread_id(&worker1_id), 0);
    ASSERT_GE(H5TS_thread_id(&worker1_repeat_id), 0);
  });
  t1.join();
  EXPECT_GE(worker1_id, 1u);
  EXPECT_EQ(worker1_id, worker1_repeat_id);
  EXPECT_NE(worker1_id, main_id1);

  // Second sequential worker reuses the freed tinfo node while still getting a
  // strictly fresh unique ID.
  uint64_t worker2_id = 0;
  std::thread t2([&]() { ASSERT_GE(H5TS_thread_id(&worker2_id), 0); });
  t2.join();
  EXPECT_GT(worker2_id, worker1_id);
  EXPECT_NE(worker2_id, main_id1);

  // Concurrent workers each observe distinct non-zero IDs.
  constexpr int kConcurrent = 4;
  std::vector<uint64_t> ids(kConcurrent, 0);
  std::vector<std::thread> workers;
  workers.reserve(kConcurrent);
  for (int i = 0; i < kConcurrent; ++i) {
    workers.emplace_back([&ids, i]() { EXPECT_GE(H5TS_thread_id(&ids[i]), 0); });
  }
  for (auto& w : workers) {
    w.join();
  }
  std::set<uint64_t> unique_ids(ids.begin(), ids.end());
  EXPECT_EQ(unique_ids.size(), static_cast<size_t>(kConcurrent));
  EXPECT_FALSE(unique_ids.contains(0));
  EXPECT_FALSE(unique_ids.contains(main_id1));
}

TEST(H5TSintTest, GetApiCtxPtrAndGetErrStackCreateAndReusePerThread) {
  ASSERT_GE(H5open(), 0);

  // Thread starting with H5TS_get_api_ctx_ptr triggers lazy tinfo creation.
  H5CX_node_t** ctx_a1 = nullptr;
  H5CX_node_t** ctx_a2 = nullptr;
  H5E_stack_t* err_a = nullptr;
  std::thread t_ctx([&]() {
    ctx_a1 = H5TS_get_api_ctx_ptr();
    ctx_a2 = H5TS_get_api_ctx_ptr();
    err_a = H5TS_get_err_stack();
  });
  t_ctx.join();
  EXPECT_NE(ctx_a1, nullptr);
  EXPECT_EQ(ctx_a1, ctx_a2);
  EXPECT_NE(err_a, nullptr);

  // Thread starting with H5TS_get_err_stack triggers lazy tinfo creation.
  H5E_stack_t* err_b1 = nullptr;
  H5E_stack_t* err_b2 = nullptr;
  H5CX_node_t** ctx_b = nullptr;
  std::thread t_err([&]() {
    err_b1 = H5TS_get_err_stack();
    err_b2 = H5TS_get_err_stack();
    ctx_b = H5TS_get_api_ctx_ptr();
  });
  t_err.join();
  EXPECT_NE(err_b1, nullptr);
  EXPECT_EQ(err_b1, err_b2);
  EXPECT_NE(ctx_b, nullptr);
}

TEST(H5TSintTest, TinfoDestroyNullIsNoOp) { H5TS__tinfo_destroy(nullptr); }

TEST(H5TSintTest, TopTermAndTermPackageCleanupAndReinit) {
  ASSERT_GE(H5open(), 0);

  // Ensure the tinfo free list contains multiple nodes before termination.
  std::thread w1([]() {
    uint64_t id = 0;
    EXPECT_GE(H5TS_thread_id(&id), 0);
  });
  std::thread w2([]() {
    uint64_t id = 0;
    EXPECT_GE(H5TS_thread_id(&id), 0);
  });
  w1.join();
  w2.join();

  ASSERT_GE(H5close(), 0);

  // Return the main thread's own tinfo node to the free list before deleting
  // H5TS_thrd_info_key_g so H5TS__tinfo_term frees every allocated node.
  void* main_tinfo = nullptr;
  ASSERT_GE(H5TS_shim_key_get_value(H5TS_thrd_info_key_g, &main_tinfo), 0);
  if (main_tinfo != nullptr) {
    ASSERT_GE(H5TS_shim_key_set_value(H5TS_thrd_info_key_g, nullptr), 0);
    H5TS__tinfo_destroy(main_tinfo);
  }

  EXPECT_GE(H5TS_top_term_package(), 0);
  H5TS_term_package();

  // Restore H5TS and HDF5 global state for subsequent tests and atexit.
  ASSERT_GE(H5TS__init_package(), 0);
  ASSERT_GE(H5open(), 0);

  uint64_t post_reinit_id = 0;
  EXPECT_GE(H5TS_thread_id(&post_reinit_id), 0);
  EXPECT_GE(post_reinit_id, 1u);
}

TEST(H5TSintTest, InvalidThreadInfoKeyFailsCreateAndTermSafely) {
  ASSERT_GE(H5open(), 0);

  // Temporarily point H5TS_thrd_info_key_g at an invalid key index so
  // H5TS_key_set_value fails inside H5TS__tinfo_create() and returns the
  // allocated node back to the free list via H5TS__tinfo_destroy().
  const H5TS_key_t saved_key = H5TS_thrd_info_key_g;
  H5TS_thrd_info_key_g = static_cast<H5TS_key_t>(-1);
  absl::Cleanup restore_key = [&] { H5TS_thrd_info_key_g = saved_key; };

  uint64_t id = 0;
  EXPECT_LT(H5TS_thread_id(&id), 0);
  EXPECT_EQ(H5TS_get_api_ctx_ptr(), nullptr);
  EXPECT_EQ(H5TS_get_err_stack(), nullptr);

  // Restore key before checking H5TS__tinfo_init key exhaustion and
  // H5TS__tinfo_term key_delete failure.
  H5TS_thrd_info_key_g = saved_key;
  ASSERT_GE(H5close(), 0);

  void* main_tinfo = nullptr;
  ASSERT_GE(H5TS_shim_key_get_value(H5TS_thrd_info_key_g, &main_tinfo), 0);
  if (main_tinfo != nullptr) {
    ASSERT_GE(H5TS_shim_key_set_value(H5TS_thrd_info_key_g, nullptr), 0);
    H5TS__tinfo_destroy(main_tinfo);
  }
  ASSERT_GE(H5TS_top_term_package(), 0);
  H5TS_term_package();

  // Exhaust all pthread keys so H5TS__tinfo_init() -> H5TS_key_create() fails
  // inside H5TS__init_package().
  std::vector<H5TS_key_t> keys;
  absl::Cleanup cleanup_and_restore = [&] {
    for (H5TS_key_t k : keys) {
      H5TS_key_delete(k);
    }
    H5TS__init_package();
    H5open();
  };
  for (int i = 0; i < 4096; ++i) {
    H5TS_key_t k{};
    if (H5TS_key_create(&k, nullptr) < 0) {
      break;
    }
    keys.push_back(k);
  }
  EXPECT_LT(H5TS__init_package(), 0);

  // Clean up the mutexes initialized before H5TS_key_create failed, while also
  // exercising H5TS__tinfo_term()'s H5TS_key_delete failure branch.
  H5TS_thrd_info_key_g = static_cast<H5TS_key_t>(-1);
  EXPECT_LT(H5TS__tinfo_term(), 0);
  H5TS_term_package();
}

#if !defined(H5TS_INT_TEST_HAS_SANITIZER)
TEST(H5TSintTest, PropagatesMutexAndOnceFailures) {
  const std::string prev_style = GTEST_FLAG_GET(death_test_style);
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  absl::Cleanup restore_style = [&] {
    GTEST_FLAG_SET(death_test_style, prev_style);
  };

  auto run_int_faults = []() {
    g_int_fault_tid = pthread_self();
    g_int_has_tid.store(true, std::memory_order_release);

    // 1. H5TS_api_lock: H5TS_once failure when !H5_INIT_GLOBAL, then
    // H5TS_mutex_lock failure.
    if (H5close() < 0) std::exit(1);
    g_int_fault.store(IntFaultTarget::kOnce, std::memory_order_release);
    if (H5TS_api_lock() >= 0) std::exit(2);

    g_int_fault.store(IntFaultTarget::kAnyMutexLock, std::memory_order_release);
    if (H5TS_api_lock() >= 0) std::exit(3);

    // 2. H5TS__api_mutex_acquire: trylock failure and recursive lock failure.
    if (H5open() < 0) std::exit(4);
    bool acquired = false;
    g_int_fault.store(IntFaultTarget::kApiMutexTrylock,
                      std::memory_order_release);
    if (H5TS__api_mutex_acquire(1, &acquired) >= 0) std::exit(5);

    acquired = false;
    g_int_fault.store(IntFaultTarget::kAnyMutexLock, std::memory_order_release);
    if (H5TS__api_mutex_acquire(2, &acquired) >= 0) std::exit(6);
    if (H5TS_shim_mutex_unlock(&H5TS_api_info_p.api_mutex) < 0) std::exit(7);

    // 3. H5TS_api_unlock: mutex_unlock failure.
    if (H5TS_api_lock() < 0) std::exit(8);
    g_int_fault.store(IntFaultTarget::kAnyMutexUnlock,
                      std::memory_order_release);
    if (H5TS_api_unlock() >= 0) std::exit(9);

    // 4. H5TS__tinfo_create: mutex_lock and mutex_unlock failures.
    void* main_tinfo = nullptr;
    if (H5TS_shim_key_get_value(H5TS_thrd_info_key_g, &main_tinfo) < 0) {
      std::exit(10);
    }
    if (main_tinfo != nullptr) {
      if (H5TS_shim_key_set_value(H5TS_thrd_info_key_g, nullptr) < 0) {
        std::exit(11);
      }
      H5TS__tinfo_destroy(main_tinfo);
    }
    uint64_t id = 0;
    g_int_fault.store(IntFaultTarget::kAnyMutexLock, std::memory_order_release);
    if (H5TS_thread_id(&id) >= 0) std::exit(12);

    g_int_fault.store(IntFaultTarget::kAnyMutexUnlock,
                      std::memory_order_release);
    if (H5TS_thread_id(&id) >= 0) std::exit(13);

    // 5. H5TS__tinfo_term: mutex_lock, mutex_unlock, and mutex_destroy
    // failures.
    if (H5close() < 0) std::exit(14);
    g_int_fault.store(IntFaultTarget::kAnyMutexLock, std::memory_order_release);
    if (H5TS__tinfo_term() >= 0) std::exit(15);

    g_int_fault.store(IntFaultTarget::kAnyMutexUnlock,
                      std::memory_order_release);
    if (H5TS__tinfo_term() >= 0) std::exit(16);

    g_int_fault.store(IntFaultTarget::kAnyMutexDestroy,
                      std::memory_order_release);
    if (H5TS__tinfo_term() >= 0) std::exit(17);

    if (H5TS__tinfo_term() < 0) std::exit(18);
    H5TS_term_package();

    // 6. H5TS__init_package: api_mutex init failure.
    g_int_fault.store(IntFaultTarget::kApiMutexInit, std::memory_order_release);
    if (H5TS__init_package() >= 0) std::exit(19);

    // Restore package globals before atexit runs.
    if (H5TS__init_package() < 0) std::exit(20);
    if (H5open() < 0) std::exit(21);
    std::exit(0);
  };

  EXPECT_EXIT(run_int_faults(), ::testing::ExitedWithCode(0), "");
}
#endif

}  // namespace
}  // namespace hdf5
