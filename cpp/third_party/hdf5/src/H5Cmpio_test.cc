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
// Tests parallel cache support constants, ring definitions, disabled MPI VFD
// sentinel, and MDC parallel config field handling related to
// third_party/hdf5/src/H5Cmpio.c.
// (LF=0 in default google3 non-MPI Linux build: all implementations in
// H5Cmpio.c are gated inside #ifdef H5_HAVE_PARALLEL.)
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Cmpio.c

#ifndef NDEBUG
#define H5CMPIO_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#define H5C_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5Cpkg.h"
}  // extern "C"

#ifdef H5CMPIO_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5CMPIO_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

TEST(H5CmpioTest, DefaultBuildDisablesParallelAndExportsCacheConstants) {
#ifdef H5_HAVE_PARALLEL
  ADD_FAILURE() << "Expected H5_HAVE_PARALLEL to be disabled in default build.";
#endif

  EXPECT_EQ(H5FD_MPIO, H5I_INVALID_HID);
  EXPECT_EQ(H5C_RING_UNDEFINED, 0);
  EXPECT_EQ(H5C_RING_USER, 1);
  EXPECT_EQ(H5C_RING_RDFSM, 2);
  EXPECT_EQ(H5C_RING_MDFSM, 3);
  EXPECT_EQ(H5C_RING_SBE, 4);
  EXPECT_EQ(H5C_RING_SB, 5);
  EXPECT_EQ(H5C_RING_NTYPES, 6);

  EXPECT_EQ(H5AC_METADATA_WRITE_STRATEGY__PROCESS_0_ONLY, 0);
  EXPECT_EQ(H5AC_METADATA_WRITE_STRATEGY__DISTRIBUTED, 1);
  EXPECT_EQ(H5AC__DEFAULT_METADATA_WRITE_STRATEGY,
            H5AC_METADATA_WRITE_STRATEGY__DISTRIBUTED);
}

TEST(H5CmpioTest, SerialCacheConfigAcceptsAndValidatesWriteStrategy) {
  ScopedFile file = CreateInMemoryFile("h5c_mpio_serial.h5");
  ASSERT_TRUE(file.valid());

  H5AC_cache_config_t cfg{};
  cfg.version = H5AC__CURR_CACHE_CONFIG_VERSION;
  ASSERT_GE(H5Fget_mdc_config(file.get(), &cfg), 0);
  EXPECT_EQ(cfg.metadata_write_strategy, H5AC__DEFAULT_METADATA_WRITE_STRATEGY);
  EXPECT_GT(cfg.dirty_bytes_threshold, 0U);

  cfg.metadata_write_strategy = H5AC_METADATA_WRITE_STRATEGY__PROCESS_0_ONLY;
  ASSERT_GE(H5Fset_mdc_config(file.get(), &cfg), 0);

  ScopedErrorSilence silence;
  cfg.metadata_write_strategy = -1;
  EXPECT_LT(H5Fset_mdc_config(file.get(), &cfg), 0);
}

}  // namespace
}  // namespace hdf5
