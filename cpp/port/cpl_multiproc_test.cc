// Tests mutex synchronization.
//
//   http://trac.osgeo.org/gdal/browser/trunk/gdal/port/cpl_multiproc.h
//   http://trac.osgeo.org/gdal/browser/trunk/gdal/port/cpl_multiproc.cpp
//
// Copyright 2015 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Most of these tests are unable to assert much of anything and count on
// being run with MSAN, ASAN, TSAN, Valgrind, deadlock detectors, etc.
//
// TODO(schwehr): Create multiple thread tests.

#include "port/cpl_multiproc.h"

#include "gunit.h"

namespace autotest2 {
namespace {

// Tests creation, locking, unlocking, and destroying a single mutex.
TEST(CplMutexTest, CplCreateMutexSimple) {
  CPLMutex *mutex = CPLCreateMutex();
  ASSERT_NE(nullptr, mutex);
  CPLAcquireMutex(mutex, 1);
  CPLReleaseMutex(mutex);
  CPLDestroyMutex(mutex);
  SUCCEED();
}

// Tests for possible interactions between two mutexes.
TEST(CplMutexTest, CplCreateTwoMutexes) {
  CPLMutex *mutex1 = CPLCreateMutex();
  ASSERT_NE(nullptr, mutex1);
  CPLMutex *mutex2 = CPLCreateMutex();
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
  CPLMutex *mutex = nullptr;
  ASSERT_TRUE(CPLCreateOrAcquireMutex(&mutex, 1));
  ASSERT_NE(nullptr, mutex);

  CPLMutex *mutex_check = mutex;
  ASSERT_TRUE(CPLCreateOrAcquireMutex(&mutex, 1));
  ASSERT_EQ(mutex_check, mutex);

  CPLAcquireMutex(mutex, 1);
  CPLReleaseMutex(mutex);

  CPLDestroyMutex(mutex);
  SUCCEED();
}

TEST(CplConditionTest, Simple) {
  CPLCond *condition = CPLCreateCond();
  ASSERT_NE(nullptr, condition);
  CPLCondSignal(condition);
  CPLCondBroadcast(condition);
  CPLDestroyCond(condition);
  SUCCEED();
}

TEST(CplThreading, Model) {
  ASSERT_STREQ("pthread", CPLGetThreadingModel());
}

TEST(CplThreading, NumCpus) {
  const int cpus = CPLGetNumCPUs();
  ASSERT_GE(cpus, 1);
  ASSERT_LE(cpus, 100000);
}

// TODO(schwehr): Test CPLCondWait.
// TODO(schwehr): Test CPL*Thread.
// TODO(schwehr): Test thread local storage (TLS).
// TODO(schwehr): Test the new functions added in GDAL 2.0.0.

}  // namespace
}  // namespace autotest2
