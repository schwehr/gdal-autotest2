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
// Tests atomic configuration and concurrent atomic lock-attempt accounting
// associated with third_party/hdf5/src/H5TSatomic.c.
//
// Note: LF=0 in default google3 non-MPI Linux build because
// H5_HAVE_STDATOMIC_H=1 is defined in H5pubconf.h, so the mutex-emulated
// atomic helpers in H5TSatomic.c are compiled out in favor of C11 <stdatomic.h>
// macros in H5TSprivate.h.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5TSatomic.c

#include "third_party/hdf5/src/hdf5.h"

#include <array>
#include <thread>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/h5ts_c_shim.h"

namespace hdf5 {
namespace {

TEST(H5TSatomicTest, DefaultBuildUsesNativeC11AtomicsNotEmulation) {
#ifndef H5_HAVE_THREADS
  ADD_FAILURE() << "Expected H5_HAVE_THREADS to be enabled in google3.";
#endif
#ifndef H5_HAVE_STDATOMIC_H
  ADD_FAILURE() << "Expected H5_HAVE_STDATOMIC_H=1 in default google3 build.";
#endif

  bool is_threadsafe = false;
  ASSERT_GE(H5is_library_threadsafe(&is_threadsafe), 0);
  EXPECT_TRUE(is_threadsafe);
}

TEST(H5TSatomicTest, AttemptLockCounterIncrementsAtomicallyAcrossThreads) {
  ASSERT_GE(H5open(), 0);

  unsigned before = 0;
  ASSERT_GE(H5TSmutex_get_attempt_count(&before), 0);
  EXPECT_EQ(before, H5TS_shim_api_info_attempt_count());

  constexpr int kNumThreads = 4;
  constexpr int kCallsPerThread = 25;
  std::array<std::thread, kNumThreads> threads;
  std::array<bool, kNumThreads> all_ok{};

  for (int i = 0; i < kNumThreads; ++i) {
    threads[i] = std::thread([&ok = all_ok[i]]() {
      ok = true;
      for (int iter = 0; iter < kCallsPerThread; ++iter) {
        unsigned maj = 0;
        unsigned min = 0;
        unsigned rel = 0;
        if (H5get_libversion(&maj, &min, &rel) < 0) {
          ok = false;
        }
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  for (bool ok : all_ok) {
    EXPECT_TRUE(ok);
  }

  unsigned after = 0;
  ASSERT_GE(H5TSmutex_get_attempt_count(&after), 0);
  EXPECT_GE(after, before + static_cast<unsigned>(kNumThreads * kCallsPerThread));
  EXPECT_EQ(after, H5TS_shim_api_info_attempt_count());
}

}  // namespace
}  // namespace hdf5
