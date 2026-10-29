// Copyright 2026 Google Inc. All Rights Reserved.
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
// Tests gcore/gdal_thread_pool.cpp.

#include "third_party/gdal/gcore/gdal_thread_pool.h"

#include "testing/base/public/gunit.h"

namespace {
bool g_fail_setup = false;

struct CPLWorkerThreadPoolLayout {
  std::vector<std::unique_ptr<CPLWorkerThread>> aWT;
  mutable std::mutex m_mutex;
  std::condition_variable m_cv;
  volatile CPLWorkerThreadState eState;
  std::queue<std::function<void()>> jobQueue;
  int nPendingJobs;
  bool m_bNotifyEvent;
  CPLList* psWaitingWorkerThreadsList;
  int nWaitingWorkerThreads;
  int m_nMaxThreads;
};
}  // namespace

bool CPLWorkerThreadPool::Setup(int nThreads, CPLThreadFunc pfnInitFunc,
                                void** pasInitData, bool bWaitallStarted) {
  if (g_fail_setup) {
    return false;
  }
  auto* layout = reinterpret_cast<CPLWorkerThreadPoolLayout*>(this);
  std::lock_guard<std::mutex> guard(layout->m_mutex);
  if (nThreads > layout->m_nMaxThreads) {
    layout->m_nMaxThreads = nThreads;
  }
  return true;
}

namespace {

TEST(GdalThreadPoolTest, SetupFailureReturnsNullptr) {
  GDALDestroyGlobalThreadPool();
  g_fail_setup = true;
  EXPECT_EQ(nullptr, GDALGetGlobalThreadPool(1));
  g_fail_setup = false;
}

TEST(GdalThreadPoolTest, GetIncreaseAndDestroyGlobalThreadPool) {
  GDALDestroyGlobalThreadPool();

  // Create with 1 thread.
  CPLWorkerThreadPool* pool = GDALGetGlobalThreadPool(1);
  ASSERT_NE(nullptr, pool);
  EXPECT_EQ(1, pool->GetThreadCount());

  // Requesting the same or smaller thread count keeps existing count.
  EXPECT_EQ(pool, GDALGetGlobalThreadPool(1));
  EXPECT_EQ(1, pool->GetThreadCount());

  // Requesting more threads increases the thread pool size.
  EXPECT_EQ(pool, GDALGetGlobalThreadPool(2));
  EXPECT_EQ(2, pool->GetThreadCount());

  GDALDestroyGlobalThreadPool();
}

}  // namespace
