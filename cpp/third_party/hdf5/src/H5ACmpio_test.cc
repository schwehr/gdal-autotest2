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
// Tests parallel metadata cache configuration constants, defaults, and
// validation paths related to third_party/hdf5/src/H5ACmpio.c.
// (LF=0 in default google3 non-MPI Linux build: all implementations in
// H5ACmpio.c are gated inside #ifdef H5_HAVE_PARALLEL.)
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5ACmpio.c

#include <cstring>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, which omits debug/stats fields from
// H5C_cache_entry_t inside H5ACprivate.h. Match that macro around internal
// headers so struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5ACMPIO_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5AC_FRIEND
#include "third_party/hdf5/src/H5ACpkg.h"
}  // extern "C"
#ifdef H5ACMPIO_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5ACMPIO_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

TEST(H5ACmpioTest, DefaultBuildHasParallelDisabledAndExportsConstants) {
#ifdef H5_HAVE_PARALLEL
  ADD_FAILURE() << "Expected H5_HAVE_PARALLEL to be disabled in default build.";
#endif

  EXPECT_EQ(H5AC__CURR_CACHE_CONFIG_VERSION, 1);
  EXPECT_EQ(H5AC__MAX_TRACE_FILE_NAME_LEN, 1024);
  EXPECT_EQ(H5AC_METADATA_WRITE_STRATEGY__PROCESS_0_ONLY, 0);
  EXPECT_EQ(H5AC_METADATA_WRITE_STRATEGY__DISTRIBUTED, 1);
  EXPECT_EQ(H5AC__DEFAULT_METADATA_WRITE_STRATEGY,
            H5AC_METADATA_WRITE_STRATEGY__DISTRIBUTED);

  const H5AC_cache_config_t def_cfg = H5AC__DEFAULT_CACHE_CONFIG;
  EXPECT_EQ(def_cfg.version, H5AC__CURR_CACHE_CONFIG_VERSION);
  EXPECT_EQ(def_cfg.dirty_bytes_threshold, H5AC__DEFAULT_DIRTY_BYTES_THRESHOLD);
  EXPECT_EQ(def_cfg.metadata_write_strategy,
            H5AC__DEFAULT_METADATA_WRITE_STRATEGY);
  EXPECT_GT(H5AC__MAX_DIRTY_BYTES_THRESHOLD, H5AC__MIN_DIRTY_BYTES_THRESHOLD);
}

TEST(H5ACmpioTest, ParallelCacheConfigFieldsRoundTripAndValidate) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());

  H5AC_cache_config_t cfg{};
  cfg.version = H5AC__CURR_CACHE_CONFIG_VERSION;
  ASSERT_GE(H5Pget_mdc_config(fapl.get(), &cfg), 0);
  EXPECT_EQ(cfg.dirty_bytes_threshold,
            static_cast<size_t>(H5AC__DEFAULT_DIRTY_BYTES_THRESHOLD));
  EXPECT_EQ(cfg.metadata_write_strategy, H5AC__DEFAULT_METADATA_WRITE_STRATEGY);

  cfg.dirty_bytes_threshold = H5AC__MIN_DIRTY_BYTES_THRESHOLD;
  cfg.metadata_write_strategy = H5AC_METADATA_WRITE_STRATEGY__PROCESS_0_ONLY;
  ASSERT_GE(H5Pset_mdc_config(fapl.get(), &cfg), 0);

  // FAPL preserves parallel fields before file open.
  H5AC_cache_config_t fapl_cfg{};
  fapl_cfg.version = H5AC__CURR_CACHE_CONFIG_VERSION;
  ASSERT_GE(H5Pget_mdc_config(fapl.get(), &fapl_cfg), 0);
  EXPECT_EQ(fapl_cfg.dirty_bytes_threshold,
            static_cast<size_t>(H5AC__MIN_DIRTY_BYTES_THRESHOLD));
  EXPECT_EQ(fapl_cfg.metadata_write_strategy,
            H5AC_METADATA_WRITE_STRATEGY__PROCESS_0_ONLY);

  ScopedFile file(
      H5Fcreate("h5ac_mpio_cfg.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  // Without H5_HAVE_PARALLEL, an open file's cache has no parallel aux_ptr, so
  // H5Fget_mdc_config reports the serial defaults while H5Fset_mdc_config still
  // validates parallel field bounds.
  H5AC_cache_config_t file_cfg{};
  file_cfg.version = H5AC__CURR_CACHE_CONFIG_VERSION;
  ASSERT_GE(H5Fget_mdc_config(file.get(), &file_cfg), 0);
  EXPECT_EQ(file_cfg.dirty_bytes_threshold,
            static_cast<size_t>(H5AC__DEFAULT_DIRTY_BYTES_THRESHOLD));
  EXPECT_EQ(file_cfg.metadata_write_strategy,
            H5AC__DEFAULT_METADATA_WRITE_STRATEGY);

  file_cfg.dirty_bytes_threshold = H5AC__MAX_DIRTY_BYTES_THRESHOLD;
  file_cfg.metadata_write_strategy =
      H5AC_METADATA_WRITE_STRATEGY__PROCESS_0_ONLY;
  ASSERT_GE(H5Fset_mdc_config(file.get(), &file_cfg), 0);

  std::memset(&file_cfg, 0, sizeof(file_cfg));
  file_cfg.version = H5AC__CURR_CACHE_CONFIG_VERSION;
  ASSERT_GE(H5Fget_mdc_config(file.get(), &file_cfg), 0);
  EXPECT_EQ(file_cfg.dirty_bytes_threshold,
            static_cast<size_t>(H5AC__DEFAULT_DIRTY_BYTES_THRESHOLD));
  EXPECT_EQ(file_cfg.metadata_write_strategy,
            H5AC__DEFAULT_METADATA_WRITE_STRATEGY);

  ScopedErrorSilence silence;
  H5AC_cache_config_t invalid = file_cfg;
  invalid.metadata_write_strategy = 99;
  EXPECT_LT(H5Fset_mdc_config(file.get(), &invalid), 0);
  EXPECT_LT(H5Pset_mdc_config(fapl.get(), &invalid), 0);

  invalid = file_cfg;
  invalid.dirty_bytes_threshold = H5AC__MIN_DIRTY_BYTES_THRESHOLD - 1;
  EXPECT_LT(H5Fset_mdc_config(file.get(), &invalid), 0);

  invalid = file_cfg;
  invalid.dirty_bytes_threshold = H5AC__MAX_DIRTY_BYTES_THRESHOLD + 1;
  EXPECT_LT(H5Fset_mdc_config(file.get(), &invalid), 0);
}

}  // namespace
}  // namespace hdf5
