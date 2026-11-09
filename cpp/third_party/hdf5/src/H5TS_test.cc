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
// Tests public HDF5 global API lock release/acquire and attempt-count helpers
// (third_party/hdf5/src/H5TS.c).
//
// Build caveat: google3 compiles HDF5 with H5_HAVE_THREADS=1 and
// H5_HAVE_THREADSAFE=1 (enabling H5_HAVE_THREADSAFE_API), without
// H5_HAVE_CONCURRENCY.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5TS.c

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

#include <condition_variable>
#include <mutex>
#include <thread>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/h5ts_c_shim.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

extern "C" {
#define H5TS_FRIEND
#include "third_party/hdf5/src/H5TSpkg.h"
}  // extern "C"

#ifndef H5TS_HAVE_TSAN
#if defined(__SANITIZE_THREAD__)
#define H5TS_HAVE_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define H5TS_HAVE_TSAN 1
#endif
#endif
#endif

namespace hdf5 {
namespace {

TEST(H5TSTest, AttemptCountTracksApiLockAcquisitions) {
  ASSERT_GE(H5open(), 0);

  unsigned before = 0;
  ASSERT_GE(H5TSmutex_get_attempt_count(&before), 0);
  EXPECT_EQ(before, H5TS_shim_api_info_attempt_count());

  unsigned maj = 0;
  unsigned min = 0;
  unsigned rel = 0;
  ASSERT_GE(H5get_libversion(&maj, &min, &rel), 0);
  ASSERT_GE(H5get_libversion(&maj, &min, &rel), 0);

  unsigned after = 0;
  ASSERT_GE(H5TSmutex_get_attempt_count(&after), 0);
  EXPECT_GE(after, before + 2U);
  EXPECT_EQ(after, H5TS_shim_api_info_attempt_count());
}

TEST(H5TSTest, ReleaseAndAcquireInsideCallbackCoordinateWithWorkerThread) {
  ASSERT_GE(H5open(), 0);

  ScopedFile file = CreateInMemoryFile("h5ts_callback_lock.h5");
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(H5Gcreate2(file.get(), "/child", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  grp.reset();

  struct CallbackContext {
    std::mutex mu;
    std::condition_variable cv;
    bool worker_holds_lock = false;
    bool main_observed_busy = false;
    unsigned released_lock_count = 0;
    bool worker_acquired = false;
    bool worker_libver_ok = false;
    unsigned worker_released_count = 0;
    bool reacquired = false;
    int visits = 0;
  } ctx;

  auto visit_cb = [](hid_t /*group*/, const char* name,
                     const H5L_info2_t* /*info*/, void* op_data) -> herr_t {
    auto* c = static_cast<CallbackContext*>(op_data);
    ++c->visits;
    if (c->visits > 1 || std::string_view(name) == ".") {
      return 0;
    }

    // 1. Temporarily release the global API lock held by H5Lvisit2.
    unsigned lock_count = 99;
    if (H5TSmutex_release(&lock_count) < 0) return -1;
    c->released_lock_count = lock_count;
    if (lock_count == 0) return -1;

    // 2. Spawn a worker thread that acquires the global API lock, exercises an
    //    HDF5 API call, and holds the lock until the main thread tests contention.
    std::thread worker([c]() {
      bool acquired = false;
      if (H5TSmutex_acquire(1, &acquired) >= 0) {
        c->worker_acquired = acquired;
      }
      unsigned maj = 0;
      unsigned min = 0;
      unsigned rel = 0;
      if (c->worker_acquired && H5get_libversion(&maj, &min, &rel) >= 0) {
        c->worker_libver_ok = true;
      }

      {
        std::lock_guard<std::mutex> lk(c->mu);
        c->worker_holds_lock = true;
      }
      c->cv.notify_all();

      {
        std::unique_lock<std::mutex> lk(c->mu);
        c->cv.wait(lk, [c] { return c->main_observed_busy; });
      }

      if (c->worker_acquired) {
        unsigned w_count = 0;
        if (H5TSmutex_release(&w_count) >= 0) {
          c->worker_released_count = w_count;
        }
      }
    });

    // 3. Wait until worker owns the HDF5 API mutex, then confirm non-blocking
    //    H5TSmutex_acquire reports acquired == false without error.
    {
      std::unique_lock<std::mutex> lk(c->mu);
      c->cv.wait(lk, [c] { return c->worker_holds_lock; });
    }

    bool busy_acquired = true;
    const herr_t busy_rc = H5TSmutex_acquire(lock_count, &busy_acquired);

    {
      std::lock_guard<std::mutex> lk(c->mu);
      c->main_observed_busy = true;
    }
    c->cv.notify_all();
    worker.join();

    if (busy_rc < 0 || busy_acquired) return -1;

    // 4. Re-acquire the original recursive lock count before returning.
    bool reacquired = false;
    if (H5TSmutex_acquire(lock_count, &reacquired) < 0) return -1;
    c->reacquired = reacquired;
    return reacquired ? 0 : -1;
  };

  ASSERT_GE(
      H5Lvisit2(file.get(), H5_INDEX_NAME, H5_ITER_INC, visit_cb, &ctx), 0);
  EXPECT_GE(ctx.released_lock_count, 1U);
  EXPECT_TRUE(ctx.worker_acquired);
  EXPECT_TRUE(ctx.worker_libver_ok);
  EXPECT_EQ(ctx.worker_released_count, 1U);
  EXPECT_TRUE(ctx.reacquired);
}

TEST(H5TSTest, RecursiveLockCountRoundTripViaInternalApi) {
  ASSERT_GE(H5open(), 0);

  unsigned locked_depth = 0;
  absl::Cleanup unlock_all = [&locked_depth] {
    while (locked_depth > 0) {
      H5TS_api_unlock();
      --locked_depth;
    }
  };

  constexpr unsigned kDepth = 3;
  for (unsigned i = 0; i < kDepth; ++i) {
    ASSERT_GE(H5TS_api_lock(), 0);
    ++locked_depth;
  }

  unsigned released_count = 0;
  ASSERT_GE(H5TSmutex_release(&released_count), 0);
  EXPECT_EQ(released_count, kDepth);
  locked_depth = 0;

  bool acquired = false;
  ASSERT_GE(H5TSmutex_acquire(released_count, &acquired), 0);
  EXPECT_TRUE(acquired);
  locked_depth = released_count;
}

#ifndef H5TS_HAVE_TSAN
TEST(H5TSTest, ReleaseFailsWhenMutexIsNotOwnedByCallingThread) {
  ASSERT_GE(H5open(), 0);
  ASSERT_EQ(H5TS_api_info_p.lock_count, 0U);

  // Set lock_count without holding api_mutex so H5TS__api_mutex_release calls
  // pthread_mutex_unlock on an unowned PTHREAD_MUTEX_RECURSIVE mutex, which
  // returns EPERM and exercises the H5TSmutex_release failure branch.
  H5TS_api_info_p.lock_count = 1;
  absl::Cleanup reset_count = [] { H5TS_api_info_p.lock_count = 0; };

  unsigned lock_count = 99;
  EXPECT_LT(H5TSmutex_release(&lock_count), 0);
  EXPECT_EQ(lock_count, 1U);
  EXPECT_EQ(H5TS_api_info_p.lock_count, 0U);
}
#endif  // !H5TS_HAVE_TSAN

}  // namespace
}  // namespace hdf5
