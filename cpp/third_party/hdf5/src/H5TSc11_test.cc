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
// Tests C11-threads build configuration expectations and pthread fallback
// typedefs/initialization related to third_party/hdf5/src/H5TSc11.c.
//
// Note: LF=0 in default google3 non-MPI Linux build because
// H5_HAVE_C11_THREADS is undefined in H5pubconf.h, so H5TSc11.c is compiled
// out and HDF5 uses POSIX pthreads (H5TSpthread.c) instead.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5TSc11.c

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

#include <pthread.h>

#include <array>
#include <thread>
#include <type_traits>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

TEST(H5TSc11Test, DefaultBuildUsesPthreadsNotC11Threads) {
#ifdef H5_HAVE_C11_THREADS
  ADD_FAILURE() << "Expected H5_HAVE_C11_THREADS to be undefined.";
#endif
#ifndef H5_HAVE_PTHREAD_H
  ADD_FAILURE() << "Expected H5_HAVE_PTHREAD_H to be defined.";
#endif
#ifndef H5_HAVE_THREADSAFE
  ADD_FAILURE() << "Expected H5_HAVE_THREADSAFE to be defined.";
#endif

  static_assert(std::is_same_v<H5TS_thread_t, pthread_t>);
  static_assert(std::is_same_v<H5TS_mutex_t, pthread_mutex_t>);
  static_assert(std::is_same_v<H5TS_cond_t, pthread_cond_t>);
  static_assert(std::is_same_v<H5TS_key_t, pthread_key_t>);
  static_assert(std::is_same_v<H5TS_once_t, pthread_once_t>);

  EXPECT_EQ(H5TS_MUTEX_TYPE_PLAIN, 0);
  EXPECT_EQ(H5TS_MUTEX_TYPE_RECURSIVE, 1);

  bool is_threadsafe = false;
  ASSERT_GE(H5is_library_threadsafe(&is_threadsafe), 0);
  EXPECT_TRUE(is_threadsafe);
}

TEST(H5TSc11Test, FirstThreadInitAndThreadIdsSucceedAcrossThreads) {
  ASSERT_GE(H5open(), 0);

  constexpr int kThreads = 4;
  std::array<uint64_t, kThreads> ids{};
  std::array<std::thread, kThreads> threads;

  for (int i = 0; i < kThreads; ++i) {
    threads[i] = std::thread([&id = ids[i]]() {
      ASSERT_GE(H5TS_thread_id(&id), 0);
    });
  }
  for (auto& t : threads) {
    t.join();
  }

  for (int i = 0; i < kThreads; ++i) {
    EXPECT_NE(ids[i], 0U);
    for (int j = i + 1; j < kThreads; ++j) {
      EXPECT_NE(ids[i], ids[j]);
    }
  }
}

}  // namespace
}  // namespace hdf5
