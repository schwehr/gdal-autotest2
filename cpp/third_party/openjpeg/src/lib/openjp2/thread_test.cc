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
// Tests internal OpenJPEG threading, synchronization primitives, TLS, and
// thread-pool APIs (src/lib/openjp2/thread.c).
//
// Caveats for google3:
// - Compiled with -DMUTEX_pthread=1 on Linux; MUTEX_win32 and the no-thread
//   #else stub sections are compiled out.
// - Several C helpers assert or dereference pointers without NULL checks
//   (opj_mutex_lock, opj_mutex_unlock, opj_cond_wait, opj_cond_signal,
//   opj_thread_create with null thread_fn, opj_thread_join, opj_tls_get/set
//   with null tls, opj_thread_pool_submit_job/wait_completion/get_thread_count
//   with null tp), so tests respect those preconditions.
//
// See also:
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/thread.c
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/thread.h
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/tls_keys.h

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/openjpeg2/google/util/alloc_fault_injector.h"
#include "third_party/openjpeg2/google/util/opj_internal_test_util.h"

namespace openjpeg2 {
namespace {

using ::testing::ElementsAreArray;

static_assert(OPJ_TRUE == 1 && OPJ_FALSE == 0);
static_assert(OPJ_TLS_KEY_T1 == 0);

struct OpjThreadDeleter {
  void operator()(opj_thread_t* thread) const { opj_thread_join(thread); }
};
using OpjThreadPtr = std::unique_ptr<opj_thread_t, OpjThreadDeleter>;

// Performs pure register work and folds into an atomic sink so jobs keep
// workers briefly busy under both -O0 and -O2 without blocking or sleeping.
void BurnCpuCycles(uint32_t seed, int iterations, std::atomic<uint32_t>* sink) {
  uint32_t acc = seed | 1u;
  for (int i = 0; i < iterations; ++i) {
    acc ^= acc << 13;
    acc ^= acc >> 17;
    acc ^= acc << 5;
  }
  sink->fetch_xor(acc, std::memory_order_relaxed);
}

struct MutexSharedState {
  opj_mutex_t* mutex = nullptr;
  int counter = 0;
  std::vector<int> trace;
};

struct MutexWorkerArg {
  MutexSharedState* shared = nullptr;
  int worker_id = 0;
  int iterations = 0;
};

void MutexIncrementWorker(void* user_data) {
  auto* arg = static_cast<MutexWorkerArg*>(user_data);
  for (int i = 0; i < arg->iterations; ++i) {
    opj_mutex_lock(arg->shared->mutex);
    ++arg->shared->counter;
    arg->shared->trace.push_back(arg->worker_id);
    opj_mutex_unlock(arg->shared->mutex);
  }
}

struct ProducerConsumerState {
  opj_mutex_t* mutex = nullptr;
  opj_cond_t* can_produce = nullptr;
  opj_cond_t* can_consume = nullptr;
  bool worker_ready = false;
  bool go = false;
  bool has_item = false;
  int item = 0;
  int count = 0;
};

void ProducerWorker(void* user_data) {
  auto* state = static_cast<ProducerConsumerState*>(user_data);

  // 1. Handshake ensuring the producer waits on `go` before producing.
  opj_mutex_lock(state->mutex);
  state->worker_ready = true;
  opj_cond_signal(state->can_produce);
  while (!state->go) {
    opj_cond_wait(state->can_consume, state->mutex);
  }
  opj_mutex_unlock(state->mutex);

  // 2. Produce `count` sequential items into the single-slot buffer.
  for (int i = 0; i < state->count; ++i) {
    opj_mutex_lock(state->mutex);
    while (state->has_item) {
      opj_cond_wait(state->can_produce, state->mutex);
    }
    state->item = i * 10 + 3;
    state->has_item = true;
    opj_cond_signal(state->can_consume);
    opj_mutex_unlock(state->mutex);
  }
}

void NoOpThread(void* /*user_data*/) {}

struct ThreadComputeArg {
  int input = 0;
  int output = 0;
};

void ComputeThread(void* user_data) {
  auto* arg = static_cast<ThreadComputeArg*>(user_data);
  arg->output = arg->input * arg->input + 7;
}

struct TlsSlot {
  int tag = 0;
  std::atomic<int> free_count{0};
};

void RecordTlsFree(void* value) {
  if (value != nullptr) {
    static_cast<TlsSlot*>(value)->free_count.fetch_add(
        1, std::memory_order_acq_rel);
  }
}

struct DummyTlsJobState {
  std::thread::id caller_tid;
  std::array<TlsSlot, 8> slots{};
  bool ran = false;
};

void DummyPoolTlsJob(void* user_data, opj_tls_t* tls) {
  auto* st = static_cast<DummyTlsJobState*>(user_data);
  st->ran = true;
  EXPECT_EQ(st->caller_tid, std::this_thread::get_id());
  ASSERT_NE(nullptr, tls);

  for (size_t i = 0; i < st->slots.size(); ++i) {
    st->slots[i].tag = static_cast<int>(i);
  }

  EXPECT_EQ(nullptr, opj_tls_get(tls, OPJ_TLS_KEY_T1));

  // Each new key triggers opj_realloc by +1 slot; 6 distinct keys exercise
  // repeated reallocs, linear lookup, and mixed null/non-null free callbacks.
  constexpr std::array<int, 6> kKeys = {OPJ_TLS_KEY_T1, 1, 2, 3, 4, 100};
  const std::array<opj_tls_free_func, 6> kFreeFuncs = {
      &RecordTlsFree, &RecordTlsFree, nullptr,
      &RecordTlsFree, nullptr,        &RecordTlsFree,
  };
  for (size_t i = 0; i < kKeys.size(); ++i) {
    EXPECT_EQ(OPJ_TRUE,
              opj_tls_set(tls, kKeys[i], &st->slots[i], kFreeFuncs[i]))
        << "i=" << i;
  }
  for (size_t i = 0; i < kKeys.size(); ++i) {
    EXPECT_EQ(&st->slots[i], opj_tls_get(tls, kKeys[i])) << "i=" << i;
  }
  EXPECT_EQ(nullptr, opj_tls_get(tls, 999));

  // Overwrite key 0 (which has &RecordTlsFree): old slot[0] must be freed.
  EXPECT_EQ(OPJ_TRUE,
            opj_tls_set(tls, kKeys[0], &st->slots[6], &RecordTlsFree));
  EXPECT_EQ(&st->slots[6], opj_tls_get(tls, kKeys[0]));
  EXPECT_EQ(1, st->slots[0].free_count.load(std::memory_order_acquire));

  // Overwrite key 2 (which had nullptr free_func): slot[2] must not be freed.
  EXPECT_EQ(OPJ_TRUE,
            opj_tls_set(tls, kKeys[2], &st->slots[7], &RecordTlsFree));
  EXPECT_EQ(&st->slots[7], opj_tls_get(tls, kKeys[2]));
  EXPECT_EQ(0, st->slots[2].free_count.load(std::memory_order_acquire));

  // Overwrite key 3 with nullptr value and nullptr free_func: slot[3] freed.
  EXPECT_EQ(OPJ_TRUE, opj_tls_set(tls, kKeys[3], nullptr, nullptr));
  EXPECT_EQ(nullptr, opj_tls_get(tls, kKeys[3]));
  EXPECT_EQ(1, st->slots[3].free_count.load(std::memory_order_acquire));

  // Insert a new key with nullptr value and non-null free_func.
  EXPECT_EQ(OPJ_TRUE, opj_tls_set(tls, 200, nullptr, &RecordTlsFree));
  EXPECT_EQ(nullptr, opj_tls_get(tls, 200));
}

struct WorkerTlsPersistState {
  std::thread::id caller_tid;
  std::array<TlsSlot, 9> slots{};
  bool job1_ran = false;
  bool job2_ran = false;
  void* heap_ptr = nullptr;
};

void WorkerTlsJob1(void* user_data, opj_tls_t* tls) {
  auto* st = static_cast<WorkerTlsPersistState*>(user_data);
  st->job1_ran = true;
  EXPECT_NE(st->caller_tid, std::this_thread::get_id());
  ASSERT_NE(nullptr, tls);

  for (size_t i = 0; i < st->slots.size(); ++i) {
    st->slots[i].tag = static_cast<int>(i);
  }

  EXPECT_EQ(nullptr, opj_tls_get(tls, OPJ_TLS_KEY_T1));
  EXPECT_EQ(nullptr, opj_tls_get(tls, 42));

  // Set 7 distinct keys so opj_realloc grows the TLS key/value array (+1/key).
  EXPECT_EQ(OPJ_TRUE,
            opj_tls_set(tls, OPJ_TLS_KEY_T1, &st->slots[0], &RecordTlsFree));
  EXPECT_EQ(OPJ_TRUE, opj_tls_set(tls, 1, &st->slots[1], nullptr));

  void* heap_buf = opj_malloc(32);
  ASSERT_NE(nullptr, heap_buf);
  absl::Cleanup free_if_unset = [&heap_buf] { opj_free(heap_buf); };
  std::fill_n(static_cast<uint8_t*>(heap_buf), 32, uint8_t{0xA5});
  if (opj_tls_set(tls, 2, heap_buf, &opj_free) == OPJ_TRUE) {
    std::move(free_if_unset).Cancel();
    st->heap_ptr = heap_buf;
  } else {
    ADD_FAILURE() << "opj_tls_set failed for heap key 2";
  }

  EXPECT_EQ(OPJ_TRUE, opj_tls_set(tls, 3, &st->slots[3], &RecordTlsFree));
  EXPECT_EQ(OPJ_TRUE, opj_tls_set(tls, 4, &st->slots[4], &RecordTlsFree));
  EXPECT_EQ(OPJ_TRUE, opj_tls_set(tls, 5, &st->slots[5], &RecordTlsFree));
  EXPECT_EQ(OPJ_TRUE, opj_tls_set(tls, 6, &st->slots[6], nullptr));
}

void WorkerTlsJob2(void* user_data, opj_tls_t* tls) {
  auto* st = static_cast<WorkerTlsPersistState*>(user_data);
  st->job2_ran = true;
  EXPECT_NE(st->caller_tid, std::this_thread::get_id());
  ASSERT_NE(nullptr, tls);

  EXPECT_EQ(&st->slots[0], opj_tls_get(tls, OPJ_TLS_KEY_T1));
  EXPECT_EQ(&st->slots[1], opj_tls_get(tls, 1));
  EXPECT_EQ(&st->slots[3], opj_tls_get(tls, 3));
  EXPECT_EQ(&st->slots[4], opj_tls_get(tls, 4));
  EXPECT_EQ(&st->slots[5], opj_tls_get(tls, 5));
  EXPECT_EQ(&st->slots[6], opj_tls_get(tls, 6));
  EXPECT_NE(nullptr, st->heap_ptr);
  EXPECT_EQ(st->heap_ptr, opj_tls_get(tls, 2));
  EXPECT_EQ(nullptr, opj_tls_get(tls, -1));
  EXPECT_EQ(nullptr, opj_tls_get(tls, 999));

  // Overwrite OPJ_TLS_KEY_T1: invokes RecordTlsFree on slots[0].
  EXPECT_EQ(OPJ_TRUE,
            opj_tls_set(tls, OPJ_TLS_KEY_T1, &st->slots[7], &RecordTlsFree));
  EXPECT_EQ(&st->slots[7], opj_tls_get(tls, OPJ_TLS_KEY_T1));
  EXPECT_EQ(1, st->slots[0].free_count.load(std::memory_order_acquire));

  // Overwrite key 1 (had nullptr free_func): slots[1] stays unfreed.
  EXPECT_EQ(OPJ_TRUE, opj_tls_set(tls, 1, &st->slots[8], &RecordTlsFree));
  EXPECT_EQ(&st->slots[8], opj_tls_get(tls, 1));
  EXPECT_EQ(0, st->slots[1].free_count.load(std::memory_order_acquire));

  // Overwrite key 5 with nullptr value and nullptr free_func: frees slots[5].
  EXPECT_EQ(OPJ_TRUE, opj_tls_set(tls, 5, nullptr, nullptr));
  EXPECT_EQ(nullptr, opj_tls_get(tls, 5));
  EXPECT_EQ(1, st->slots[5].free_count.load(std::memory_order_acquire));
}

struct WorkerSlot {
  int local_jobs_run = 0;
  std::atomic<int> free_count{0};
};

void RecordWorkerSlotFree(void* value) {
  if (value != nullptr) {
    static_cast<WorkerSlot*>(value)->free_count.fetch_add(
        1, std::memory_order_acq_rel);
  }
}

struct MultiWorkerPoolState {
  static constexpr int kMaxWorkers = 4;
  static constexpr int kNumJobs = 32;
  std::atomic<int> next_slot{0};
  std::array<WorkerSlot, kMaxWorkers> slots{};
  std::array<std::atomic<int>, kNumJobs> job_ran{};
  std::atomic<bool> tls_set_failed{false};
};

struct MultiWorkerJobArg {
  MultiWorkerPoolState* shared = nullptr;
  int job_index = 0;
};

void MultiWorkerPoolJob(void* user_data, opj_tls_t* tls) {
  auto* arg = static_cast<MultiWorkerJobArg*>(user_data);
  MultiWorkerPoolState* shared = arg->shared;

  auto* slot = static_cast<WorkerSlot*>(opj_tls_get(tls, OPJ_TLS_KEY_T1));
  if (slot == nullptr) {
    const int idx = shared->next_slot.fetch_add(1, std::memory_order_relaxed);
    if (idx < static_cast<int>(shared->slots.size())) {
      slot = &shared->slots[idx];
      if (opj_tls_set(tls, OPJ_TLS_KEY_T1, slot, &RecordWorkerSlotFree) !=
          OPJ_TRUE) {
        shared->tls_set_failed.store(true, std::memory_order_relaxed);
        slot = nullptr;
      }
    }
  }
  if (slot != nullptr) {
    ++slot->local_jobs_run;
  }

  shared->job_ran[arg->job_index].fetch_add(1, std::memory_order_relaxed);
}

struct BatchPoolState {
  std::atomic<int> completed{0};
  std::atomic<uint32_t> sink{0};
  std::vector<std::atomic<int>> job_ran;

  explicit BatchPoolState(size_t count) : job_ran(count) {}
};

struct BatchJobArg {
  BatchPoolState* shared = nullptr;
  size_t index = 0;
  int work_iterations = 0;
};

void BatchPoolJob(void* user_data, opj_tls_t* /*tls*/) {
  auto* arg = static_cast<BatchJobArg*>(user_data);
  if (arg->work_iterations > 0) {
    BurnCpuCycles(static_cast<uint32_t>(arg->index + 1), arg->work_iterations,
                  &arg->shared->sink);
  }
  arg->shared->job_ran[arg->index].fetch_add(1, std::memory_order_relaxed);
  arg->shared->completed.fetch_add(1, std::memory_order_release);
}

TEST(ThreadTest, HasThreadSupportAndNumCpus) {
  EXPECT_EQ(OPJ_TRUE, opj_has_thread_support());

  // Pins that opj_get_num_cpus() queries _SC_NPROCESSORS_ONLN (online CPUs,
  // not _SC_NPROCESSORS_CONF).
  const int num_cpus = opj_get_num_cpus();
  EXPECT_GE(num_cpus, 1);
#ifdef _SC_NPROCESSORS_ONLN
  const long online_cpus = sysconf(_SC_NPROCESSORS_ONLN);
  ASSERT_GE(online_cpus, 1);
  EXPECT_EQ(static_cast<int>(online_cpus), num_cpus);
#endif
}

TEST(ThreadTest, MutexCreateDestroyAndMutualExclusion) {
  opj_mutex_destroy(nullptr);

  OpjMutexPtr mutex(opj_mutex_create());
  ASSERT_NE(nullptr, mutex);

  // Single-thread lock/unlock smoke check before concurrent contention.
  opj_mutex_lock(mutex.get());
  opj_mutex_unlock(mutex.get());

  constexpr int kNumThreads = 4;
  constexpr int kItersPerThread = 500;

  MutexSharedState shared;
  shared.mutex = mutex.get();
  shared.trace.reserve(kNumThreads * kItersPerThread);

  std::array<MutexWorkerArg, kNumThreads> args{};
  std::array<OpjThreadPtr, kNumThreads> threads{};
  for (int i = 0; i < kNumThreads; ++i) {
    args[i] = MutexWorkerArg{
        .shared = &shared,
        .worker_id = i,
        .iterations = kItersPerThread,
    };
    threads[i].reset(opj_thread_create(&MutexIncrementWorker, &args[i]));
    ASSERT_NE(nullptr, threads[i]);
  }

  for (OpjThreadPtr& thread : threads) {
    thread.reset();
  }

  EXPECT_EQ(kNumThreads * kItersPerThread, shared.counter);
  EXPECT_EQ(static_cast<size_t>(kNumThreads * kItersPerThread),
            shared.trace.size());

  mutex.reset();
}

TEST(ThreadTest, CondCreateDestroyAndProducerConsumerSynchronization) {
  opj_cond_destroy(nullptr);

  OpjMutexPtr mutex(opj_mutex_create());
  OpjCondPtr can_produce(opj_cond_create());
  OpjCondPtr can_consume(opj_cond_create());
  ASSERT_NE(nullptr, mutex);
  ASSERT_NE(nullptr, can_produce);
  ASSERT_NE(nullptr, can_consume);

  // Signaling with no waiters is a valid no-op.
  opj_cond_signal(can_produce.get());

  constexpr int kItemCount = 12;
  ProducerConsumerState state{
      .mutex = mutex.get(),
      .can_produce = can_produce.get(),
      .can_consume = can_consume.get(),
      .count = kItemCount,
  };

  OpjThreadPtr producer(opj_thread_create(&ProducerWorker, &state));
  ASSERT_NE(nullptr, producer);

  // 1. Complete startup handshake so opj_cond_wait runs deterministically.
  opj_mutex_lock(mutex.get());
  while (!state.worker_ready) {
    opj_cond_wait(can_produce.get(), mutex.get());
  }
  state.go = true;
  opj_cond_signal(can_consume.get());
  opj_mutex_unlock(mutex.get());

  // 2. Consume all items produced across the bounded 1-slot buffer.
  std::vector<int> received;
  received.reserve(kItemCount);
  for (int i = 0; i < kItemCount; ++i) {
    opj_mutex_lock(mutex.get());
    while (!state.has_item) {
      opj_cond_wait(can_consume.get(), mutex.get());
    }
    received.push_back(state.item);
    state.has_item = false;
    opj_cond_signal(can_produce.get());
    opj_mutex_unlock(mutex.get());
  }

  producer.reset();

  std::vector<int> expected;
  expected.reserve(kItemCount);
  for (int i = 0; i < kItemCount; ++i) {
    expected.push_back(i * 10 + 3);
  }
  EXPECT_THAT(received, ElementsAreArray(expected));
}

TEST(ThreadTest, ThreadCreateWithNullAndNonNullUserDataAndJoin) {
  // 1. thread.h explicitly permits user_data == nullptr.
  OpjThreadPtr null_arg_thread(opj_thread_create(&NoOpThread, nullptr));
  ASSERT_NE(nullptr, null_arg_thread);
  null_arg_thread.reset();

  // 2. Distinct non-null user_data pointers across concurrent threads.
  std::array<ThreadComputeArg, 4> args = {{
      {.input = 2, .output = 0},
      {.input = -5, .output = 0},
      {.input = 9, .output = 0},
      {.input = 0, .output = 0},
  }};
  std::array<OpjThreadPtr, 4> threads{};
  for (size_t i = 0; i < args.size(); ++i) {
    threads[i].reset(opj_thread_create(&ComputeThread, &args[i]));
    ASSERT_NE(nullptr, threads[i]);
  }
  for (OpjThreadPtr& thread : threads) {
    thread.reset();
  }

  EXPECT_EQ(11, args[0].output);
  EXPECT_EQ(32, args[1].output);
  EXPECT_EQ(88, args[2].output);
  EXPECT_EQ(7, args[3].output);
}

TEST(ThreadTest, DummyThreadPoolExecutesSynchronouslyAndManagesTls) {
  opj_thread_pool_destroy(nullptr);

  struct DummyPoolCase {
    std::string_view name;
    int num_threads;
  };
  constexpr DummyPoolCase kCases[] = {
      {.name = "zero_threads", .num_threads = 0},
      {.name = "minus_one_threads", .num_threads = -1},
      {.name = "negative_eight_threads", .num_threads = -8},
  };

  for (const DummyPoolCase& tc : kCases) {
    SCOPED_TRACE(tc.name);

    DummyTlsJobState state;
    state.caller_tid = std::this_thread::get_id();

    OpjThreadPoolPtr tp(opj_thread_pool_create(tc.num_threads));
    ASSERT_NE(nullptr, tp);
    EXPECT_EQ(0, opj_thread_pool_get_thread_count(tp.get()));

    // Wait completion on a dummy synchronous pool is a no-op
    // (tp->mutex == NULL).
    opj_thread_pool_wait_completion(tp.get(), 0);
    opj_thread_pool_wait_completion(tp.get(), -1);

    ASSERT_EQ(OPJ_TRUE,
              opj_thread_pool_submit_job(tp.get(), &DummyPoolTlsJob, &state));
    // Dummy pool must have executed the job synchronously before returning.
    EXPECT_TRUE(state.ran);

    // Remaining active TLS entries are freed only when destroying the pool.
    EXPECT_EQ(0, state.slots[1].free_count.load());
    EXPECT_EQ(0, state.slots[2].free_count.load());
    EXPECT_EQ(0, state.slots[4].free_count.load());
    EXPECT_EQ(0, state.slots[5].free_count.load());
    EXPECT_EQ(0, state.slots[6].free_count.load());
    EXPECT_EQ(0, state.slots[7].free_count.load());

    tp.reset();

    EXPECT_EQ(1, state.slots[0].free_count.load());
    EXPECT_EQ(1, state.slots[1].free_count.load());
    EXPECT_EQ(0, state.slots[2].free_count.load());
    EXPECT_EQ(1, state.slots[3].free_count.load());
    EXPECT_EQ(0, state.slots[4].free_count.load());
    EXPECT_EQ(1, state.slots[5].free_count.load());
    EXPECT_EQ(1, state.slots[6].free_count.load());
    EXPECT_EQ(1, state.slots[7].free_count.load());
  }
}

TEST(ThreadTest, SingleWorkerPoolPersistsTlsAcrossJobsAndFreesOnDestroy) {
  WorkerTlsPersistState state;
  state.caller_tid = std::this_thread::get_id();

  OpjThreadPoolPtr tp(opj_thread_pool_create(1));
  ASSERT_NE(nullptr, tp);
  EXPECT_EQ(1, opj_thread_pool_get_thread_count(tp.get()));

  // 1. Job 1 populates 7 distinct TLS keys on the sole worker thread.
  ASSERT_EQ(OPJ_TRUE,
            opj_thread_pool_submit_job(tp.get(), &WorkerTlsJob1, &state));
  opj_thread_pool_wait_completion(tp.get(), 0);
  EXPECT_TRUE(state.job1_ran);

  // 2. Job 2 runs on the same worker thread and verifies/mutates persisted TLS.
  ASSERT_EQ(OPJ_TRUE,
            opj_thread_pool_submit_job(tp.get(), &WorkerTlsJob2, &state));
  opj_thread_pool_wait_completion(tp.get(), 0);
  EXPECT_TRUE(state.job2_ran);

  // Remaining slots stay live until the worker thread exits on pool destroy.
  EXPECT_EQ(0, state.slots[3].free_count.load());
  EXPECT_EQ(0, state.slots[4].free_count.load());
  EXPECT_EQ(0, state.slots[6].free_count.load());
  EXPECT_EQ(0, state.slots[7].free_count.load());
  EXPECT_EQ(0, state.slots[8].free_count.load());

  tp.reset();

  EXPECT_EQ(1, state.slots[0].free_count.load());
  EXPECT_EQ(0, state.slots[1].free_count.load());
  EXPECT_EQ(1, state.slots[3].free_count.load());
  EXPECT_EQ(1, state.slots[4].free_count.load());
  EXPECT_EQ(1, state.slots[5].free_count.load());
  EXPECT_EQ(0, state.slots[6].free_count.load());
  EXPECT_EQ(1, state.slots[7].free_count.load());
  EXPECT_EQ(1, state.slots[8].free_count.load());
}

TEST(ThreadTest, MultiWorkerPoolExecutesAllJobsAndIsolatesWorkerTls) {
  constexpr std::array<int, 2> kWorkerCounts = {2, 4};

  for (const int num_workers : kWorkerCounts) {
    SCOPED_TRACE(num_workers);

    MultiWorkerPoolState state;
    std::array<MultiWorkerJobArg, MultiWorkerPoolState::kNumJobs> args{};
    for (int i = 0; i < MultiWorkerPoolState::kNumJobs; ++i) {
      args[i] = MultiWorkerJobArg{.shared = &state, .job_index = i};
    }

    OpjThreadPoolPtr tp(opj_thread_pool_create(num_workers));
    ASSERT_NE(nullptr, tp);
    EXPECT_EQ(num_workers, opj_thread_pool_get_thread_count(tp.get()));

    for (auto& arg : args) {
      ASSERT_EQ(OPJ_TRUE, opj_thread_pool_submit_job(
                              tp.get(), &MultiWorkerPoolJob, &arg));
    }

    // Negative max_remaining_jobs clamps to 0 in wait_completion.
    opj_thread_pool_wait_completion(tp.get(), -3);

    EXPECT_FALSE(state.tls_set_failed.load());
    for (int i = 0; i < MultiWorkerPoolState::kNumJobs; ++i) {
      EXPECT_EQ(1, state.job_ran[i].load()) << "job " << i;
    }

    const int active_workers = state.next_slot.load();
    EXPECT_GE(active_workers, 1);
    ASSERT_LE(active_workers, num_workers);

    int total_from_tls = 0;
    for (int w = 0; w < active_workers; ++w) {
      total_from_tls += state.slots[w].local_jobs_run;
    }
    EXPECT_EQ(MultiWorkerPoolState::kNumJobs, total_from_tls);

    tp.reset();

    for (int w = 0; w < num_workers; ++w) {
      const int expected_frees = (w < active_workers) ? 1 : 0;
      EXPECT_EQ(expected_frees, state.slots[w].free_count.load())
          << "worker " << w;
    }
  }
}

TEST(ThreadTest, WaitCompletionPartialThresholdAndDrainsPendingJobsOnDestroy) {
  // 1. Wait with max_remaining_jobs > 0, then wait for full completion.
  {
    constexpr int kJobCount = 16;
    BatchPoolState state(kJobCount);
    std::vector<BatchJobArg> args(kJobCount);
    for (int i = 0; i < kJobCount; ++i) {
      args[i] = BatchJobArg{
          .shared = &state,
          .index = static_cast<size_t>(i),
          .work_iterations = 50000,
      };
    }

    OpjThreadPoolPtr tp(opj_thread_pool_create(1));
    ASSERT_NE(nullptr, tp);
    for (auto& arg : args) {
      ASSERT_EQ(OPJ_TRUE,
                opj_thread_pool_submit_job(tp.get(), &BatchPoolJob, &arg));
    }

    opj_thread_pool_wait_completion(tp.get(), 2);
    const int completed_after_partial =
        state.completed.load(std::memory_order_acquire);
    EXPECT_GE(completed_after_partial, kJobCount - 2);
    EXPECT_LE(completed_after_partial, kJobCount);

    opj_thread_pool_wait_completion(tp.get(), 0);
    EXPECT_EQ(kJobCount, state.completed.load());
    for (int i = 0; i < kJobCount; ++i) {
      EXPECT_EQ(1, state.job_ran[i].load()) << "job " << i;
    }
  }

  // 2. Destroying a pool while jobs are still pending drains all jobs cleanly.
  {
    constexpr int kJobCount = 24;
    BatchPoolState state(kJobCount);
    std::vector<BatchJobArg> args(kJobCount);
    for (int i = 0; i < kJobCount; ++i) {
      args[i] = BatchJobArg{
          .shared = &state,
          .index = static_cast<size_t>(i),
          .work_iterations = 50000,
      };
    }

    OpjThreadPoolPtr tp(opj_thread_pool_create(2));
    ASSERT_NE(nullptr, tp);
    for (auto& arg : args) {
      ASSERT_EQ(OPJ_TRUE,
                opj_thread_pool_submit_job(tp.get(), &BatchPoolJob, &arg));
    }

    tp.reset();

    EXPECT_EQ(kJobCount, state.completed.load());
    for (int i = 0; i < kJobCount; ++i) {
      EXPECT_EQ(1, state.job_ran[i].load()) << "job " << i;
    }
  }
}

TEST(ThreadTest, SubmitBeyondSignalingThresholdThrottlesAndCompletesAllJobs) {
  // Pins upstream `100 * worker_threads_count` pending-job throttling
  // threshold. Submitting 150 jobs on a 1-thread pool where each job runs
  // brief pure-register work (well within size="small" under TSan/MSan -O0)
  // triggers both submit-side cond_wait and busy-worker submission without
  // deadlock.
  constexpr int kJobCount = 150;
  BatchPoolState state(kJobCount);
  std::vector<BatchJobArg> args(kJobCount);
  for (int i = 0; i < kJobCount; ++i) {
    args[i] = BatchJobArg{
        .shared = &state,
        .index = static_cast<size_t>(i),
        .work_iterations = 20000,
    };
  }

  OpjThreadPoolPtr tp(opj_thread_pool_create(1));
  ASSERT_NE(nullptr, tp);

  for (auto& arg : args) {
    ASSERT_EQ(OPJ_TRUE,
              opj_thread_pool_submit_job(tp.get(), &BatchPoolJob, &arg));
  }

  opj_thread_pool_wait_completion(tp.get(), 0);
  EXPECT_EQ(kJobCount, state.completed.load());
  for (int i = 0; i < kJobCount; ++i) {
    EXPECT_EQ(1, state.job_ran[i].load()) << "job " << i;
  }
}

TEST(ThreadTest, AllocationFailurePathsCleanUpWithoutLeaks) {
  // 1. Primitive creation OOM checks.
  {
    ScopedOpjAllocFailCountdown fail_next(0);
    EXPECT_EQ(nullptr, opj_mutex_create());
  }
  {
    ScopedOpjAllocFailCountdown fail_next(0);
    EXPECT_EQ(nullptr, opj_cond_create());
  }
  {
    ScopedOpjAllocFailCountdown fail_next(0);
    EXPECT_EQ(nullptr, opj_thread_create(&NoOpThread, nullptr));
  }

  // 2. TLS realloc OOM inside a zero-worker synchronous thread pool preserves
  //    existing TLS entries without leaking. ScopedOpjAllocFailCountdown is
  //    thread_local, so TLS OOM must run on the caller thread in a 0-worker
  //    pool.
  {
    OpjThreadPoolPtr tp(opj_thread_pool_create(0));
    ASSERT_NE(nullptr, tp);

    struct TlsFailArg {
      TlsSlot existing;
      TlsSlot rejected;
      OPJ_BOOL first_set = OPJ_FALSE;
      OPJ_BOOL second_set = OPJ_TRUE;
    } tls_arg;

    auto job = [](void* user_data, opj_tls_t* tls) {
      auto* arg = static_cast<TlsFailArg*>(user_data);
      arg->first_set = opj_tls_set(tls, 1, &arg->existing, &RecordTlsFree);
      {
        ScopedOpjAllocFailCountdown fail_realloc(0);
        arg->second_set = opj_tls_set(tls, 2, &arg->rejected, &RecordTlsFree);
      }
      EXPECT_EQ(&arg->existing, opj_tls_get(tls, 1));
      EXPECT_EQ(nullptr, opj_tls_get(tls, 2));
    };

    ASSERT_EQ(OPJ_TRUE, opj_thread_pool_submit_job(tp.get(), +job, &tls_arg));
    EXPECT_EQ(OPJ_TRUE, tls_arg.first_set);
    EXPECT_EQ(OPJ_FALSE, tls_arg.second_set);
    tp.reset();
    EXPECT_EQ(1, tls_arg.existing.free_count.load());
    EXPECT_EQ(0, tls_arg.rejected.free_count.load());
  }

  // 3. Thread-pool creation OOM failure points (self-calibrating sweeps
  // counting
  //    opj_* calls on the current thread):
  //    create(0): 0=tp calloc, 1=tp->tls_list calloc.
  //    create(1): 0=tp calloc (covered above), 1=tp->mutex, 2=tp->cond,
  //    3=worker_threads calloc, 4=worker[0].mutex, 5=worker[0].cond,
  //    6=opj_thread_create malloc.
  for (int countdown : {0, 1}) {
    ScopedOpjAllocFailCountdown fail_at(countdown);
    EXPECT_EQ(nullptr, opj_thread_pool_create(0)) << "countdown=" << countdown;
  }
  for (int countdown = 1;; ++countdown) {
    ScopedOpjAllocFailCountdown fail_at(countdown);
    OpjThreadPoolPtr created(opj_thread_pool_create(1));
    if (created != nullptr) {
      EXPECT_EQ(7, countdown);
      break;
    }
  }

  // 4. Job submission OOM failure points leave an active pool usable:
  //    0=opj_job_t malloc, 1=job list node opj_calloc.
  {
    OpjThreadPoolPtr tp(opj_thread_pool_create(1));
    ASSERT_NE(nullptr, tp);
    std::atomic<int> ran{0};
    auto inc_job = [](void* user_data, opj_tls_t* /*tls*/) {
      static_cast<std::atomic<int>*>(user_data)->fetch_add(
          1, std::memory_order_acq_rel);
    };

    for (int countdown = 0;; ++countdown) {
      ScopedOpjAllocFailCountdown fail_at(countdown);
      if (opj_thread_pool_submit_job(tp.get(), +inc_job, &ran) == OPJ_TRUE) {
        EXPECT_EQ(2, countdown);
        break;
      }
    }
    opj_thread_pool_wait_completion(tp.get(), 0);
    EXPECT_EQ(1, ran.load());
  }
}

// TODO(schwehr): Cover remaining pthread syscall failure branches, the
// unreachable INT_MAX TLS key limit, and the false branch of `if (job->job_fn)`
// in thread.c.

}  // namespace
}  // namespace openjpeg2
