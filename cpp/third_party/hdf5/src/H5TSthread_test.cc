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
// Tests thread creation, joining, detaching, and yielding
// (third_party/hdf5/src/H5TSthread.c).
//
// Google3 build caveats: H5_HAVE_THREADS=1 on Linux uses POSIX pthreads;
// H5_HAVE_C11_THREADS and H5_HAVE_WIN_THREADS branches are not compiled.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5TSthread.c

#include <pthread.h>

#include <atomic>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG; match its preprocessor state across
// hdf5_internal_prelude.h and H5TSprivate.h.
#ifndef NDEBUG
#define H5TS_THREAD_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#include "third_party/hdf5/src/H5TSprivate.h"
}  // extern "C"
#ifdef H5TS_THREAD_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5TS_THREAD_TEST_RESTORE_UNDEF_NDEBUG
#endif

#if defined(__SANITIZE_THREAD__)
#define H5TS_THREAD_HAVE_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define H5TS_THREAD_HAVE_TSAN 1
#endif
#endif

namespace hdf5 {
namespace {

struct JoinContext {
  int input = 0;
  int output = 0;
};

void* JoinWorker(void* udata) {
  auto* ctx = static_cast<JoinContext*>(udata);
  ctx->output = ctx->input * 3 + 1;
  return &ctx->output;
}

#ifndef H5TS_THREAD_HAVE_TSAN
struct JoinSelfContext {
  herr_t rc = 0;
};

void* JoinSelfWorker(void* udata) {
  auto* ctx = static_cast<JoinSelfContext*>(udata);
  // 1. Joining self from a pthread_create'd thread returns EDEADLK cleanly
  // under glibc, ASan (BeforeJoin tracks created threads), and MSan.
  ctx->rc = H5TS_thread_join(H5TS_thread_self(), nullptr);
  return nullptr;
}
#endif  // !H5TS_THREAD_HAVE_TSAN

struct DetachSync {
  pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
  bool detached = false;
  bool worker_done = false;
};

void* DetachWorker(void* udata) {
  auto* sync = static_cast<DetachSync*>(udata);
  pthread_mutex_lock(&sync->mu);
  // Wait until parent completes H5TS_thread_detach() so the pthread_t remains
  // valid during the detach call.
  while (!sync->detached) {
    pthread_cond_wait(&sync->cv, &sync->mu);
  }
  sync->worker_done = true;
  pthread_cond_signal(&sync->cv);
  pthread_mutex_unlock(&sync->mu);
  return nullptr;
}

struct YieldContext {
  std::atomic<int> turn{0};
  int observed = 0;
};

void* YieldWorker(void* udata) {
  auto* ctx = static_cast<YieldContext*>(udata);
  while (ctx->turn.load(std::memory_order_acquire) == 0) {
    H5TS_thread_yield();
  }
  ctx->observed = 99;
  ctx->turn.store(2, std::memory_order_release);
  return nullptr;
}

TEST(H5TSthreadTest, CreateAndJoinReturnsWorkerResultAndSupportsNullRet) {
  JoinContext ctx1{.input = 7};
  H5TS_thread_t t1{};
  ASSERT_GE(H5TS_thread_create(&t1, JoinWorker, &ctx1), 0);

  H5TS_thread_ret_t ret_val = nullptr;
  ASSERT_GE(H5TS_thread_join(t1, &ret_val), 0);
  EXPECT_EQ(ret_val, &ctx1.output);
  EXPECT_EQ(ctx1.output, 22);

  // Joining with ret_val == nullptr discards the return value cleanly.
  JoinContext ctx2{.input = 10};
  H5TS_thread_t t2{};
  ASSERT_GE(H5TS_thread_create(&t2, JoinWorker, &ctx2), 0);
  ASSERT_GE(H5TS_thread_join(t2, nullptr), 0);
  EXPECT_EQ(ctx2.output, 31);
}

#ifndef H5TS_THREAD_HAVE_TSAN
TEST(H5TSthreadTest, JoinSelfReturnsFailure) {
  // TSan's pthread_join interceptor consumes the thread ID before checking the
  // underlying return code, so EDEADLK self-join is skipped under TSan.
  JoinSelfContext ctx{};
  H5TS_thread_t thread{};
  ASSERT_GE(H5TS_thread_create(&thread, JoinSelfWorker, &ctx), 0);
  ASSERT_GE(H5TS_thread_join(thread, nullptr), 0);
  EXPECT_LT(ctx.rc, 0);
}
#endif  // !H5TS_THREAD_HAVE_TSAN

TEST(H5TSthreadTest, DetachRunsWorkerToCompletionWithoutLeak) {
  DetachSync sync;
  absl::Cleanup destroy_sync = [&] {
    pthread_cond_destroy(&sync.cv);
    pthread_mutex_destroy(&sync.mu);
  };

  H5TS_thread_t thread{};
  ASSERT_GE(H5TS_thread_create(&thread, DetachWorker, &sync), 0);
  ASSERT_GE(H5TS_thread_detach(thread), 0);

  pthread_mutex_lock(&sync.mu);
  sync.detached = true;
  pthread_cond_signal(&sync.cv);
  while (!sync.worker_done) {
    pthread_cond_wait(&sync.cv, &sync.mu);
  }
  pthread_mutex_unlock(&sync.mu);
}

TEST(H5TSthreadTest, YieldAllowsProgressBetweenThreads) {
  YieldContext ctx;
  H5TS_thread_t thread{};
  ASSERT_GE(H5TS_thread_create(&thread, YieldWorker, &ctx), 0);

  H5TS_thread_yield();
  ctx.turn.store(1, std::memory_order_release);
  while (ctx.turn.load(std::memory_order_acquire) != 2) {
    H5TS_thread_yield();
  }
  ASSERT_GE(H5TS_thread_join(thread, nullptr), 0);
  EXPECT_EQ(ctx.observed, 99);
}

}  // namespace
}  // namespace hdf5
