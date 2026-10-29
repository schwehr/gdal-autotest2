// Test CPLWorkerThreadPool related code
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_worker_thread_pool.h
//
// Copyright 2024 Google Inc. All Rights Reserved.
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

#include "third_party/gdal/port/cpl_worker_thread_pool.h"

#include <atomic>
#include <memory>
#include <numeric>
#include <thread>  // NOLINT(build/c++11)
#include <vector>

#include "testing/base/public/gunit.h"

namespace autotest2 {
namespace {

TEST(CplWorkerThreadPoolTest, SubmitJobWorksAsExpected) {
  CPLWorkerThreadPool pool;
  ASSERT_TRUE(pool.Setup(2, nullptr, nullptr, false));
  EXPECT_EQ(pool.GetThreadCount(), 2);

  const auto job = [](void* data) { ++(*static_cast<int*>(data)); };

  const int kNumJobs = 1000;
  std::vector<int> data(kNumJobs);
  std::vector<int> data_expected(kNumJobs);
  for (int i = 0; i < kNumJobs; ++i) {
    data[i] = i;
    data_expected[i] = i + 1;
    pool.SubmitJob(job, &data[i]);
  }

  pool.WaitCompletion();
  EXPECT_EQ(data, data_expected);
}

TEST(CplWorkerThreadPoolTest, SubmitJobsWorksAsExpected) {
  CPLWorkerThreadPool pool;
  ASSERT_TRUE(pool.Setup(2, nullptr, nullptr));
  EXPECT_EQ(pool.GetThreadCount(), 2);

  const auto job = [](void* data) { ++(*static_cast<int*>(data)); };

  const int kNumJobs = 1000;
  std::vector<int> data(kNumJobs);
  std::vector<void*> data_pointers(kNumJobs);
  std::vector<int> data_expected(kNumJobs);

  for (int i = 0; i < kNumJobs; ++i) {
    data[i] = 0;
    data_expected[i] = 1;
    data_pointers[i] = &data[i];
  }

  pool.SubmitJobs(job, data_pointers);

  pool.WaitEvent();
  EXPECT_GE(std::reduce(data.begin(), data.end()), 1);

  pool.WaitCompletion();
  EXPECT_EQ(data, data_expected);
}

TEST(CplWorkerThreadPoolTest, DestructorWaitsForJobsToComplete) {
  const int kNumJobs = 1000;
  std::vector<int> data(kNumJobs);
  std::vector<int> data_expected(kNumJobs);

  for (int i = 0; i < kNumJobs; ++i) {
    data[i] = i;
    data_expected[i] = i + 1;
  }

  {
    CPLWorkerThreadPool pool;
    ASSERT_TRUE(pool.Setup(4, nullptr, nullptr));
    EXPECT_EQ(pool.GetThreadCount(), 4);

    const auto job = [](void* data) { ++(*static_cast<int*>(data)); };

    for (int i = 0; i < kNumJobs; ++i) {
      pool.SubmitJob(job, &data[i]);
    }
  }

  EXPECT_EQ(data, data_expected);
}

TEST(CplWorkerThreadPoolTest, JobQueuesWorkAsExpected) {
  CPLWorkerThreadPool pool;
  ASSERT_TRUE(pool.Setup(4, nullptr, nullptr));
  EXPECT_EQ(pool.GetThreadCount(), 4);

  const auto job = [](void* data) { ++(*static_cast<int*>(data)); };

  const int kNumJobs = 1000;
  std::vector<int> data(kNumJobs);
  std::vector<int> data_expected(kNumJobs);

  for (int i = 0; i < kNumJobs; ++i) {
    data[i] = i;
    data_expected[i] = i + 1;
  }

  std::unique_ptr<CPLJobQueue> q1 = pool.CreateJobQueue();
  std::unique_ptr<CPLJobQueue> q2 = pool.CreateJobQueue();

  for (int i = 0; i < kNumJobs; ++i) {
    if (i % 2) {
      q1->SubmitJob(job, &data[i]);
    } else {
      q2->SubmitJob(job, &data[i]);
    }
  }

  q1->WaitCompletion();
  q2->WaitCompletion();

  EXPECT_EQ(data, data_expected);
}

TEST(CplWorkerThreadPoolTest, ConstructorWithThreadCountAndInitFunc) {
  CPLWorkerThreadPool pool1(2);
  EXPECT_EQ(pool1.GetThreadCount(), 2);

  std::atomic<int> init_sum{0};
  int init_val1 = 10;
  int init_val2 = 20;
  struct InitCtx {
    std::atomic<int>* sum;
    int val;
  };
  InitCtx ctx1{&init_sum, init_val1};
  InitCtx ctx2{&init_sum, init_val2};
  void* ctx_ptrs[2] = {&ctx1, &ctx2};

  CPLWorkerThreadPool pool2;
  ASSERT_TRUE(pool2.Setup(
      2,
      [](void* data) {
        auto* ctx = static_cast<InitCtx*>(data);
        ctx->sum->fetch_add(ctx->val);
      },
      ctx_ptrs));
  EXPECT_EQ(init_sum.load(), 30);
}

TEST(CplWorkerThreadPoolTest, LazyThreadCreationInSubmitJobsAndEmptyVector) {
  CPLWorkerThreadPool pool;
  ASSERT_TRUE(pool.Setup(2, nullptr, nullptr, false));

  const auto job = [](void* data) { ++(*static_cast<int*>(data)); };
  EXPECT_FALSE(pool.SubmitJobs(job, {}));

  int a = 1;
  int b = 2;
  std::vector<void*> ptrs = {&a, &b};
  EXPECT_TRUE(pool.SubmitJobs(job, ptrs));
  pool.WaitCompletion(-1);
  EXPECT_EQ(a, 2);
  EXPECT_EQ(b, 3);

  // WaitEvent when no jobs are pending returns immediately.
  pool.WaitEvent();
}

TEST(CplWorkerThreadPoolTest, NestedSubmitFromWorkerThreadAndWakeUp) {
  // Single-thread pool: nested SubmitJob must execute synchronously to avoid
  // deadlock, and nested SubmitJobs also executes synchronously.
  {
    CPLWorkerThreadPool pool1(1);
    std::atomic<int> count{0};
    pool1.SubmitJob([&]() {
      pool1.SubmitJob([&]() { count.fetch_add(1); });
      int x = 0;
      std::vector<void*> ptrs = {&x};
      pool1.SubmitJobs([](void* p) { ++(*static_cast<int*>(p)); }, ptrs);
      count.fetch_add(x);
    });
    pool1.WaitCompletion();
    EXPECT_EQ(count.load(), 2);
  }

  // Multi-thread pool with idle threads: nested SubmitJob can queue
  // asynchronously when another worker thread is waiting.
  {
    CPLWorkerThreadPool pool2;
    ASSERT_TRUE(pool2.Setup(3, nullptr, nullptr, true));
    std::atomic<int> count{0};
    pool2.SubmitJob([&]() { pool2.SubmitJob([&]() { count.fetch_add(10); }); });
    pool2.WaitCompletion();
    EXPECT_EQ(count.load(), 10);
  }

  // WakeUpWaitEvent() must release a pending WaitEvent() even though no job
  // has completed.  The single worker is parked on `release`, so if the
  // wake-up were ignored WaitEvent() would block until the test times out.
  {
    CPLWorkerThreadPool pool3(1);
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    pool3.SubmitJob([&]() {
      started = true;
      while (!release) std::this_thread::yield();
    });
    while (!started) std::this_thread::yield();

    pool3.WakeUpWaitEvent();
    pool3.WaitEvent();
    // The job is still running: WaitEvent() returned because of the wake-up.
    EXPECT_FALSE(release.load());

    release = true;
    pool3.WaitCompletion();
  }
}

TEST(CplWorkerThreadPoolTest, JobQueueWaitEvent) {
  CPLWorkerThreadPool pool(2);
  std::unique_ptr<CPLJobQueue> q = pool.CreateJobQueue();
  EXPECT_EQ(q->GetPool(), &pool);

  // When 0 jobs pending, WaitEvent returns false immediately.
  EXPECT_FALSE(q->WaitEvent());

  std::atomic<int> done{0};
  for (int i = 0; i < 10; ++i) {
    q->SubmitJob([&]() { done.fetch_add(1); });
  }
  while (q->WaitEvent()) {
  }
  EXPECT_EQ(done.load(), 10);
}

}  // namespace
}  // namespace autotest2
