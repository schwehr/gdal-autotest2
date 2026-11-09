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
// Tests semaphore initialization, destruction, count validation, and
// signal/wait synchronization (third_party/hdf5/src/H5TSsemaphore.c).
//
// Google3 build caveats: Linux defines __unix__ and not __MACH__, so POSIX
// unnamed semaphores (sem_init / sem_destroy / sem_post / sem_wait) are used;
// Windows and emulated mutex+condvar branches are not compiled.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5TSsemaphore.c
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5TSsemaphore.h

#include <limits.h>

#include <limits>
#include <thread>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/h5ts_c_shim.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG; match its preprocessor state across
// hdf5_internal_prelude.h and H5TSprivate.h.
#ifndef NDEBUG
#define H5TS_SEMAPHORE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#include "third_party/hdf5/src/H5TSprivate.h"
}  // extern "C"
#ifdef H5TS_SEMAPHORE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5TS_SEMAPHORE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

TEST(H5TSsemaphoreTest, NullArgumentsReturnFailure) {
  EXPECT_LT(H5TS_semaphore_init(nullptr, 0), 0);
  EXPECT_LT(H5TS_semaphore_destroy(nullptr), 0);
  EXPECT_LT(H5TS_shim_semaphore_signal(nullptr), 0);
  EXPECT_LT(H5TS_shim_semaphore_wait(nullptr), 0);
}

TEST(H5TSsemaphoreTest, InitialCountAboveSemValueMaxFails) {
  H5TS_semaphore_t sem{};
  // POSIX sem_init fails with EINVAL when initial_count > SEM_VALUE_MAX.
  static_assert(std::numeric_limits<unsigned>::max() >
                static_cast<unsigned>(SEM_VALUE_MAX));
  EXPECT_LT(
      H5TS_semaphore_init(&sem, static_cast<unsigned>(SEM_VALUE_MAX) + 1U), 0);
  EXPECT_LT(H5TS_semaphore_init(&sem, std::numeric_limits<unsigned>::max()), 0);
}

TEST(H5TSsemaphoreTest, InitWaitSignalAndDestroyLifecycle) {
  H5TS_semaphore_t sem{};
  ASSERT_GE(H5TS_semaphore_init(&sem, 2), 0);

  // Consume initial count of 2, replenish 1, consume 1, then destroy cleanly.
  EXPECT_GE(H5TS_shim_semaphore_wait(&sem), 0);
  EXPECT_GE(H5TS_shim_semaphore_wait(&sem), 0);
  EXPECT_GE(H5TS_shim_semaphore_signal(&sem), 0);
  EXPECT_GE(H5TS_shim_semaphore_wait(&sem), 0);
  EXPECT_GE(H5TS_semaphore_destroy(&sem), 0);
}

TEST(H5TSsemaphoreTest, ProducerConsumerSynchronizationAcrossThreads) {
  H5TS_semaphore_t ready_sem{};
  H5TS_semaphore_t ack_sem{};
  ASSERT_GE(H5TS_semaphore_init(&ready_sem, 0), 0);
  absl::Cleanup destroy_ready = [&] { H5TS_semaphore_destroy(&ready_sem); };
  ASSERT_GE(H5TS_semaphore_init(&ack_sem, 0), 0);
  absl::Cleanup destroy_ack = [&] { H5TS_semaphore_destroy(&ack_sem); };

  constexpr int kRounds = 8;
  int shared_payload = 0;
  std::vector<int> received;
  received.reserve(kRounds);

  std::thread consumer([&]() {
    for (int i = 0; i < kRounds; ++i) {
      ASSERT_GE(H5TS_shim_semaphore_wait(&ready_sem), 0);
      received.push_back(shared_payload);
      ASSERT_GE(H5TS_shim_semaphore_signal(&ack_sem), 0);
    }
  });

  for (int i = 1; i <= kRounds; ++i) {
    shared_payload = i * 10;
    ASSERT_GE(H5TS_shim_semaphore_signal(&ready_sem), 0);
    ASSERT_GE(H5TS_shim_semaphore_wait(&ack_sem), 0);
  }

  consumer.join();
  EXPECT_THAT(received, ::testing::ElementsAre(10, 20, 30, 40, 50, 60, 70, 80));
}

}  // namespace
}  // namespace hdf5
