// Tests mutex synchronization.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_multiproc.h
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_multiproc.cpp
//
// Copyright 2015 Google Inc. All Rights Reserved.
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

// Most of these tests are unable to assert much of anything and count on
// being run with MSAN, ASAN, TSAN, Valgrind, deadlock detectors, etc.

#include "third_party/gdal/port/cpl_multiproc.h"

#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

// TLS slots that no GDAL module owns (cpl_multiproc.h marks 17 as unused and
// nothing is assigned just below CTLS_MAX), so the probes below cannot
// interfere with CPLReadLine() or other CPL internals that keep state in TLS.
constexpr int kUnusedTlsSlotA = 17;
constexpr int kUnusedTlsSlotB = CTLS_MAX - 1;
static_assert(kUnusedTlsSlotA < CTLS_MAX, "TLS slot out of range");

// Tests creation, locking, unlocking, and destroying a single mutex.
TEST(CplMutexTest, CplCreateMutexSimple) {
  CPLMutex* mutex = CPLCreateMutex();
  ASSERT_NE(nullptr, mutex);
  CPLAcquireMutex(mutex, 1);
  CPLReleaseMutex(mutex);
  CPLDestroyMutex(mutex);
  SUCCEED();
}

// Tests for possible interactions between two mutexes.
TEST(CplMutexTest, CplCreateTwoMutexes) {
  CPLMutex* mutex1 = CPLCreateMutex();
  ASSERT_NE(nullptr, mutex1);
  CPLMutex* mutex2 = CPLCreateMutex();
  ASSERT_NE(nullptr, mutex2);

  CPLAcquireMutex(mutex1, 1);
  CPLReleaseMutex(mutex1);

  CPLAcquireMutex(mutex1, 1);
  CPLAcquireMutex(mutex2, 1);
  CPLReleaseMutex(mutex2);
  CPLReleaseMutex(mutex1);

  // These absolutely should not trigger a deadlock.
  CPLAcquireMutex(mutex1, 1);
  CPLAcquireMutex(mutex2, 1);
  CPLReleaseMutex(mutex1);
  CPLReleaseMutex(mutex2);

  CPLAcquireMutex(mutex2, 1);
  CPLReleaseMutex(mutex2);

  CPLDestroyMutex(mutex1);
  CPLDestroyMutex(mutex2);
  // TODO(schwehr): Why does omitting destroy does not cause a heapcheck fail?
  SUCCEED();
}

TEST(CplMutexTest, CplCreateOrAcquireMutex) {
  CPLMutex* mutex = nullptr;
  ASSERT_TRUE(CPLCreateOrAcquireMutex(&mutex, 1));
  ASSERT_NE(nullptr, mutex);

  CPLMutex* mutex_check = mutex;
  ASSERT_TRUE(CPLCreateOrAcquireMutex(&mutex, 1));
  ASSERT_EQ(mutex_check, mutex);

  CPLAcquireMutex(mutex, 1);
  CPLReleaseMutex(mutex);

  CPLDestroyMutex(mutex);
  SUCCEED();
}

TEST(CplConditionTest, Simple) {
  CPLCond* condition = CPLCreateCond();
  ASSERT_NE(nullptr, condition);
  CPLCondSignal(condition);
  CPLCondBroadcast(condition);
  CPLDestroyCond(condition);
  SUCCEED();
}

TEST(CplThreading, Model) { ASSERT_STREQ("pthread", CPLGetThreadingModel()); }

TEST(CplThreading, NumCpus) {
  const int cpus = CPLGetNumCPUs();
  ASSERT_GE(cpus, 1);
  ASSERT_LE(cpus, 100000);
}

TEST(CplMutexTest, MutexOptionsHoldersAndTimeout) {
  CPLSetConfigOption("CPL_DEBUG", "ON");
  absl::Cleanup cleanup_cfg = [] { CPLSetConfigOption("CPL_DEBUG", nullptr); };

  // Test CPLMutexHolder with nullptr and existing mutex
  {
    CPLMutexHolder null_ptr_holder(static_cast<CPLMutex**>(nullptr));
    CPLMutexHolder null_val_holder(static_cast<CPLMutex*>(nullptr));
  }

  CPLMutex* m_holder = nullptr;
  {
    CPLMutexHolder holder1(&m_holder, 1.0, __FILE__, __LINE__,
                           CPL_MUTEX_RECURSIVE);
    ASSERT_NE(nullptr, m_holder);
    {
      CPLMutexHolder holder2(m_holder, 1.0, __FILE__, __LINE__);
    }
  }
  CPLDestroyMutex(m_holder);

  // Test CPLCreateMutexEx options: REGULAR, ADAPTIVE, RECURSIVE, invalid
  CPLMutex* m_reg = CPLCreateMutexEx(CPL_MUTEX_REGULAR);
  ASSERT_NE(nullptr, m_reg);
  CPLReleaseMutex(m_reg);

  // Spawn a thread that acquires and releases the regular mutex.
  struct TimeoutArg {
    CPLMutex* mutex;
    int acquired;
  } arg = {m_reg, 0};
  CPLJoinableThread* th = CPLCreateJoinableThread(
      [](void* p) {
        auto* a = static_cast<TimeoutArg*>(p);
        a->acquired = CPLAcquireMutex(a->mutex, 0.01);
        if (a->acquired) {
          CPLReleaseMutex(a->mutex);
        }
      },
      &arg);
  CPLJoinThread(th);
  EXPECT_TRUE(arg.acquired);

  // Acquire with wait > 0 when uncontended
  EXPECT_TRUE(CPLAcquireMutex(m_reg, 0.5));
  CPLReleaseMutex(m_reg);
  CPLDestroyMutex(m_reg);

  CPLMutex* m_adap = CPLCreateMutexEx(CPL_MUTEX_ADAPTIVE);
  ASSERT_NE(nullptr, m_adap);
  CPLReleaseMutex(m_adap);

  CPLMutex* m_invalid = CPLCreateMutexEx(-999);
  ASSERT_NE(nullptr, m_invalid);
  CPLReleaseMutex(m_invalid);

  CPLDestroyMutex(m_adap);
  CPLDestroyMutex(m_invalid);

  // Under CPL_MULTIPROC_PTHREAD (this build) CPLCleanupMasterMutex() is a
  // no-op; just make sure it is safe to call repeatedly.
  CPLCleanupMasterMutex();
  CPLCleanupMasterMutex();
}

struct CondThreadState {
  CPLMutex* mutex;
  CPLCond* cond;
  bool ready;
};

TEST(CplConditionTest, WaitAndTimedWait) {
  CPLMutex* mutex = CPLCreateMutex();
  CPLCond* cond = CPLCreateCond();
  ASSERT_NE(nullptr, mutex);
  ASSERT_NE(nullptr, cond);

  // 1. TimedWait timeout
  EXPECT_EQ(COND_TIMED_WAIT_TIME_OUT, CPLCondTimedWait(cond, mutex, 0.005));

  // 2. TimedWait signaled by worker thread
  CondThreadState state = {mutex, cond, false};
  CPLJoinableThread* t1 = CPLCreateJoinableThread(
      [](void* p) {
        auto* s = static_cast<CondThreadState*>(p);
        CPLAcquireMutex(s->mutex, 10.0);
        s->ready = true;
        CPLCondSignal(s->cond);
        CPLReleaseMutex(s->mutex);
      },
      &state);
  while (!state.ready) {
    EXPECT_NE(COND_TIMED_WAIT_OTHER, CPLCondTimedWait(cond, mutex, 5.0));
  }
  CPLReleaseMutex(mutex);
  CPLJoinThread(t1);

  // 3. Unbounded CPLCondWait signaled by worker thread
  CPLAcquireMutex(mutex, 10.0);
  state.ready = false;
  CPLJoinableThread* t2 = CPLCreateJoinableThread(
      [](void* p) {
        auto* s = static_cast<CondThreadState*>(p);
        CPLSleep(0.01);
        CPLAcquireMutex(s->mutex, 10.0);
        s->ready = true;
        CPLCondBroadcast(s->cond);
        CPLReleaseMutex(s->mutex);
      },
      &state);
  while (!state.ready) {
    CPLCondWait(cond, mutex);
  }
  CPLReleaseMutex(mutex);
  CPLJoinThread(t2);

  CPLDestroyCond(cond);
  CPLDestroyMutex(mutex);
}

TEST(CplThreading, ThreadsPidSleepAndTLS) {
  EXPECT_GT(CPLGetPID(), 0);
  EXPECT_GT(CPLGetCurrentProcessID(), 0);
  EXPECT_GE(CPLGetCurrentThreadCount(), 1);
  CPLSleep(0.001);

  CPLJoinThread(nullptr);

  // Test detached thread via CPLCreateThread + TLS with free func +
  // CPLCleanupTLS.
  static bool custom_tls_freed = false;
  struct DetachedSync {
    CPLMutex* mutex;
    CPLCond* cond;
    bool done;
  };
  CPLMutex* m = CPLCreateMutex();
  CPLCond* c = CPLCreateCond();
  DetachedSync sync = {m, c, false};

  int rc = CPLCreateThread(
      [](void* p) {
        // Set TLS with CPLMalloc'd memory (bFreeOnExit=TRUE) and with a custom
        // free function.
        CPLSetTLS(kUnusedTlsSlotA, CPLMalloc(16), TRUE);
        int mem_err = 0;
        CPLSetTLSWithFreeFuncEx(
            kUnusedTlsSlotB, reinterpret_cast<void*>(0x1),
            [](void* /*data*/) { custom_tls_freed = true; }, &mem_err);
        EXPECT_EQ(0, mem_err);
        EXPECT_EQ(reinterpret_cast<void*>(0x1), CPLGetTLS(kUnusedTlsSlotB));
        CPLCleanupTLS();

        auto* s = static_cast<DetachedSync*>(p);
        CPLAcquireMutex(s->mutex, 10.0);
        s->done = true;
        CPLCondSignal(s->cond);
        CPLReleaseMutex(s->mutex);
      },
      &sync);
  EXPECT_EQ(1, rc);
  while (!sync.done) {
    CPLCondWait(c, m);
  }
  CPLReleaseMutex(m);
  EXPECT_TRUE(custom_tls_freed);
  CPLDestroyCond(c);
  CPLDestroyMutex(m);
}

TEST(CplMultiprocTest, LockFileAndLocks) {
  WithQuietHandler quiet;

  CPLUnlockFile(nullptr);
  // Failing lock in non-existent directory
  EXPECT_EQ(nullptr, CPLLockFile("/nonexistent_dir_xyz/file.txt", 0.001));

  const std::string target_file = ::testing::TempDir() + "/cpl_lock_test.dat";
  // CPLLockFile() creates "<path>.lock" (never <path> itself); remove it even
  // if an assertion below fails before CPLUnlockFile() runs.
  const std::string lock_file = target_file + ".lock";
  absl::Cleanup cleanup = [&] { VSIUnlink(lock_file.c_str()); };

  void* lock1 = CPLLockFile(target_file.c_str(), 0.01);
  ASSERT_NE(nullptr, lock1);
  VSIStatBufL lock_stat = {};
  EXPECT_EQ(0, VSIStatL(lock_file.c_str(), &lock_stat));
  // Second lock on same file fails after waiting
  EXPECT_EQ(nullptr, CPLLockFile(target_file.c_str(), 0.01));
  CPLUnlockFile(lock1);
  EXPECT_NE(0, VSIStatL(lock_file.c_str(), &lock_stat));

  // Test CPLLock APIs for RECURSIVE_MUTEX, ADAPTIVE_MUTEX, and SPIN
  for (CPLLockType type :
       {LOCK_RECURSIVE_MUTEX, LOCK_ADAPTIVE_MUTEX, LOCK_SPIN}) {
    CPLLock* lock = nullptr;
    {
      CPLLockHolder null_opt_holder(static_cast<CPLLock*>(nullptr));
      CPLLockHolder holder(&lock, type, __FILE__, __LINE__);
      ASSERT_NE(nullptr, lock);
      CPLLockSetDebugPerf(lock, TRUE);
      CPLLockSetDebugPerf(lock, FALSE);
    }
    {
      CPLLockHolder opt_holder(lock, __FILE__, __LINE__);
    }
    EXPECT_TRUE(CPLCreateOrAcquireLock(&lock, type));
    CPLReleaseLock(lock);
    EXPECT_TRUE(CPLAcquireLock(lock));
    CPLReleaseLock(lock);
    CPLDestroyLock(lock);
  }
}

}  // namespace
}  // namespace autotest2
