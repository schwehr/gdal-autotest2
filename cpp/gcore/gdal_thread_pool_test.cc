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
// Tests global thread pool management (gcore/gdal_thread_pool.cpp).
//
// Note: Lines 36-39 of gcore/gdal_thread_pool.cpp (delete on Setup() returning
// false) are unreachable because CPLWorkerThreadPool::Setup(nThreads, nullptr,
// nullptr, false) takes the lazy-initialization fast path (!bWaitallStarted)
// and unconditionally returns true.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdal_thread_pool.cpp

#include "third_party/gdal/gcore/gdal_thread_pool.h"

#include <atomic>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/port/cpl_worker_thread_pool.h"

namespace autotest2 {
namespace {

TEST(GdalThreadPoolTest, GetIncreaseAndDestroyGlobalThreadPool) {
  GDALDestroyGlobalThreadPool();
  absl::Cleanup cleanup_pool = [] { GDALDestroyGlobalThreadPool(); };

  // Create with 1 thread.
  CPLWorkerThreadPool* pool = GDALGetGlobalThreadPool(1);
  ASSERT_NE(nullptr, pool);
  EXPECT_EQ(1, pool->GetThreadCount());

  // Requesting the same or smaller thread count keeps existing count.
  EXPECT_EQ(pool, GDALGetGlobalThreadPool(1));
  EXPECT_EQ(1, pool->GetThreadCount());

  // Requesting more threads increases the thread pool size.
  EXPECT_EQ(pool, GDALGetGlobalThreadPool(3));
  EXPECT_EQ(3, pool->GetThreadCount());

  GDALDestroyGlobalThreadPool();
  // Destroying when already destroyed is a safe no-op.
  GDALDestroyGlobalThreadPool();
}

TEST(GdalThreadPoolTest, SubmitJobAndWaitCompletion) {
  GDALDestroyGlobalThreadPool();
  absl::Cleanup cleanup_pool = [] { GDALDestroyGlobalThreadPool(); };

  CPLWorkerThreadPool* pool = GDALGetGlobalThreadPool(2);
  ASSERT_NE(nullptr, pool);
  EXPECT_EQ(2, pool->GetThreadCount());

  constexpr int kFirstBatch = 8;
  std::atomic<int> completed{0};
  for (int i = 0; i < kFirstBatch; ++i) {
    EXPECT_TRUE(pool->SubmitJob(
        [&completed] { completed.fetch_add(1, std::memory_order_relaxed); }));
  }
  pool->WaitCompletion();
  EXPECT_EQ(kFirstBatch, completed.load());

  // Growing the global pool retains existing instance and runs further jobs.
  EXPECT_EQ(pool, GDALGetGlobalThreadPool(4));
  EXPECT_EQ(4, pool->GetThreadCount());

  constexpr int kSecondBatch = 8;
  for (int i = 0; i < kSecondBatch; ++i) {
    EXPECT_TRUE(pool->SubmitJob(
        [&completed] { completed.fetch_add(1, std::memory_order_relaxed); }));
  }
  pool->WaitCompletion();
  EXPECT_EQ(kFirstBatch + kSecondBatch, completed.load());
}

}  // namespace
}  // namespace autotest2
