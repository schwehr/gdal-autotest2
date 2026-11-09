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
// Tests Windows threadsafety build configuration, disabled Win32 thread
// sentinels, and active POSIX pthreads counterparts for
// third_party/hdf5/src/H5TSwin.c.
// (LF=0 in default google3 non-MPI Linux build: all implementations in
// H5TSwin.c are gated behind #ifdef H5_HAVE_WIN_THREADS.)
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5TSwin.c

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

#include <pthread.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <type_traits>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/h5ts_c_shim.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

struct TlsPayload {
  std::atomic<int> destructor_calls{0};
  int tag = 0x48355453;
};

struct TlsWorkerCtx {
  H5TS_key_t key;
  TlsPayload* payload;
  herr_t set_rc = -1;
  herr_t get_rc = -1;
  void* observed = nullptr;
};

struct OnceWorkerCtx {
  H5TS_once_t* once;
  herr_t rc = -1;
};

std::atomic<int> g_once_calls{0};

extern "C" {

static void TestOnceCallback(void) {
  g_once_calls.fetch_add(1, std::memory_order_relaxed);
}

static void* OnceWorker(void* arg) {
  auto* ctx = static_cast<OnceWorkerCtx*>(arg);
  ctx->rc = H5TS_once(ctx->once, &TestOnceCallback);
  return nullptr;
}

static void TestKeyDestructor(void* ptr) {
  auto* payload = static_cast<TlsPayload*>(ptr);
  if (payload != nullptr) {
    payload->destructor_calls.fetch_add(1, std::memory_order_release);
  }
}

static void* TlsKeyWorker(void* arg) {
  auto* ctx = static_cast<TlsWorkerCtx*>(arg);
  ctx->set_rc = H5TS_shim_key_set_value(ctx->key, ctx->payload);
  ctx->get_rc = H5TS_shim_key_get_value(ctx->key, &ctx->observed);
  return ctx->observed;
}

}  // extern "C"

TEST(H5TSwinTest, DefaultBuildDisablesWinThreadsAndSelectsPthreads) {
#ifdef H5_HAVE_WIN_THREADS
  ADD_FAILURE() << "Expected H5_HAVE_WIN_THREADS to be disabled in google3.";
#endif
#ifdef H5_HAVE_WIN32_API
  ADD_FAILURE() << "Expected H5_HAVE_WIN32_API to be disabled in google3.";
#endif
#ifdef H5_HAVE_WINDOWS
  ADD_FAILURE() << "Expected H5_HAVE_WINDOWS to be disabled in google3.";
#endif
#ifdef H5_BUILT_AS_DYNAMIC_LIB
  ADD_FAILURE()
      << "Expected H5_BUILT_AS_DYNAMIC_LIB to be disabled in google3.";
#endif
#ifdef H5_HAVE_C11_THREADS
  ADD_FAILURE() << "Expected H5_HAVE_C11_THREADS to be disabled in google3.";
#endif
#ifdef H5_HAVE_CONCURRENCY
  ADD_FAILURE() << "Expected H5_HAVE_CONCURRENCY to be disabled in google3.";
#endif
#ifndef H5_HAVE_THREADS
  ADD_FAILURE() << "Expected H5_HAVE_THREADS to be enabled in google3.";
#endif
#ifndef H5_HAVE_THREADSAFE
  ADD_FAILURE() << "Expected H5_HAVE_THREADSAFE to be enabled in google3.";
#endif
#ifndef H5_HAVE_PTHREAD_H
  ADD_FAILURE() << "Expected H5_HAVE_PTHREAD_H to be enabled in google3.";
#endif
#ifndef H5_HAVE_THREADSAFE_API
  ADD_FAILURE() << "Expected H5_HAVE_THREADSAFE_API to be enabled in google3.";
#endif

  // Verify H5TS portability types resolve to POSIX pthreads rather than Win32
  // handles/critical sections/INIT_ONCE.
  static_assert(std::is_same_v<H5TS_thread_t, pthread_t>);
  static_assert(std::is_same_v<H5TS_key_t, pthread_key_t>);
  static_assert(std::is_same_v<H5TS_mutex_t, pthread_mutex_t>);
  static_assert(std::is_same_v<H5TS_cond_t, pthread_cond_t>);
  static_assert(std::is_same_v<H5TS_once_t, pthread_once_t>);
  static_assert(H5TS_MUTEX_TYPE_PLAIN == 0);
  static_assert(H5TS_MUTEX_TYPE_RECURSIVE == 1);

  ASSERT_GE(H5open(), 0);
  bool is_threadsafe = false;
  ASSERT_GE(H5is_library_threadsafe(&is_threadsafe), 0);
  EXPECT_TRUE(is_threadsafe);

  EXPECT_NE(H5TS_thread_equal(H5TS_thread_self(), pthread_self()), 0);
}

TEST(H5TSwinTest, OnceAndApiLockCounterpartsToWin32ProcessEnter) {
  // 1. On Windows, H5TS__win32_process_enter runs via InitOnceExecuteOnce; on
  // Linux, H5TS_once delegates to pthread_once and rejects null arguments.
  static H5TS_once_t once = H5TS_ONCE_INITIALIZER;
  EXPECT_LT(H5TS_once(nullptr, &TestOnceCallback), 0);
  EXPECT_LT(H5TS_once(&once, nullptr), 0);

  std::array<OnceWorkerCtx, 2> results{{{&once}, {&once}}};
  std::array<H5TS_thread_t, 2> threads{};
  std::array<bool, 2> created{};
  std::array<bool, 2> joined{};
  absl::Cleanup join_all = [&] {
    for (size_t i = 0; i < threads.size(); ++i) {
      if (created[i] && !joined[i]) {
        H5TS_thread_join(threads[i], nullptr);
      }
    }
  };
  for (size_t i = 0; i < threads.size(); ++i) {
    ASSERT_GE(H5TS_thread_create(&threads[i], &OnceWorker, &results[i]), 0);
    created[i] = true;
  }
  for (size_t i = 0; i < threads.size(); ++i) {
    ASSERT_GE(H5TS_thread_join(threads[i], nullptr), 0);
    joined[i] = true;
    EXPECT_GE(results[i].rc, 0);
  }
  EXPECT_EQ(g_once_calls.load(std::memory_order_acquire), 1);

  // 2. Verify HDF5's first-thread init (H5TS__pthread_first_thread_init instead
  // of H5TS__win32_process_enter) initialized the global API lock.
  ASSERT_GE(H5open(), 0);
  unsigned before = 0;
  ASSERT_GE(H5TSmutex_get_attempt_count(&before), 0);
  EXPECT_EQ(before, H5TS_shim_api_info_attempt_count());

  unsigned maj = 0;
  unsigned min = 0;
  unsigned rel = 0;
  ASSERT_GE(H5get_libversion(&maj, &min, &rel), 0);

  unsigned after = 0;
  ASSERT_GE(H5TSmutex_get_attempt_count(&after), 0);
  EXPECT_GT(after, before);
  EXPECT_EQ(after, H5TS_shim_api_info_attempt_count());
}

TEST(H5TSwinTest, TlsDestructorAndThreadInfoCleanupEliminateWin32DllMain) {
  // 1. Unlike H5_HAVE_WIN_THREADS (where H5TS_key_create rejects non-null
  // destructors and DllMain -> H5TS__win32_thread_exit must clean up TLS),
  // pthreads accepts a key destructor and runs it automatically on thread exit.
  EXPECT_LT(H5TS_key_create(nullptr, nullptr), 0);

  TlsPayload payload{};
  H5TS_key_t key{};
  ASSERT_GE(H5TS_key_create(&key, &TestKeyDestructor), 0);
  absl::Cleanup delete_key = [key] { EXPECT_GE(H5TS_key_delete(key), 0); };

  TlsWorkerCtx ctx{key, &payload};
  H5TS_thread_t thread{};
  ASSERT_GE(H5TS_thread_create(&thread, &TlsKeyWorker, &ctx), 0);
  bool joined = false;
  absl::Cleanup join_thread = [&] {
    if (!joined) {
      H5TS_thread_join(thread, nullptr);
    }
  };
  H5TS_thread_ret_t thread_ret = nullptr;
  ASSERT_GE(H5TS_thread_join(thread, &thread_ret), 0);
  joined = true;

  EXPECT_GE(ctx.set_rc, 0);
  EXPECT_GE(ctx.get_rc, 0);
  EXPECT_EQ(ctx.observed, &payload);
  EXPECT_EQ(thread_ret, &payload);
  EXPECT_EQ(payload.destructor_calls.load(std::memory_order_acquire), 1);

  // 2. Verify HDF5's per-thread info key (registered with H5TS__tinfo_destroy)
  // assigns distinct IDs across sequentially joined threads and recycles
  // per-thread state automatically on thread exit.
  ASSERT_GE(H5open(), 0);
  EXPECT_LT(H5TS_thread_id(nullptr), 0);

  uint64_t id1 = 0;
  herr_t rc1 = -1;
  {
    std::thread t1([&]() { rc1 = H5TS_thread_id(&id1); });
    t1.join();
  }
  EXPECT_GE(rc1, 0);
  EXPECT_GE(id1, 1U);

  uint64_t id2 = 0;
  herr_t rc2 = -1;
  {
    std::thread t2([&]() { rc2 = H5TS_thread_id(&id2); });
    t2.join();
  }
  EXPECT_GE(rc2, 0);
  EXPECT_GE(id2, 1U);
  EXPECT_NE(id1, id2);
}

}  // namespace
}  // namespace hdf5
