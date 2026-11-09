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
// Tests internal H5F unit-test helpers for SOHM message counts, cached root
// symbol table verification, maxaddr/sbe_addr inspection, shared-file equality,
// and file-locking environment variable reparsing
// (third_party/hdf5/src/H5Ftest.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Ftest.c

#include <cstdlib>
#include <optional>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, which omits #ifndef NDEBUG fields from
// H5C_cache_entry_t (H5AC_info_t) inside H5F_super_t. Define NDEBUG around
// hdf5_internal_prelude.h and internal headers so struct offsets match `:hdf5`.
#ifndef NDEBUG
#define H5FTEST_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#ifdef try
#undef try
#endif
#define try hdf5_try_param
extern "C" {
#define H5F_FRIEND
#define H5F_TESTING
#define H5G_FRIEND
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5Gpkg.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5FTEST_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FTEST_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

TEST(H5FtestTest, GetSohmMesgCountTestHandlesValidSharedAndErrorCases) {
  // 1. Invalid file ID fails argument validation.
  {
    ScopedErrorSilence silence;
    size_t count = 99;
    EXPECT_LT(H5F__get_sohm_mesg_count_test(H5I_INVALID_HID,
                                            H5O_SHMESG_ATTR_FLAG, &count),
              0);
  }

  // 2. File without SOHM indexes succeeds and reports count == 0.
  {
    ScopedFile plain = CreateInMemoryFile("no_sohm.h5");
    ASSERT_TRUE(plain.valid());
    size_t count = 99;
    EXPECT_GE(H5F__get_sohm_mesg_count_test(plain.get(), H5O_ATTR_ID, &count),
              0);
    EXPECT_EQ(count, 0U);
  }

  // 3. File configured with a shared attribute message index tracks counts.
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 1), 0);
  ASSERT_GE(
      H5Pset_shared_mesg_index(fcpl.get(), 0, H5O_SHMESG_ATTR_FLAG, /*min=*/1),
      0);

  ScopedFile file = CreateInMemoryFile("with_sohm.h5", H5F_ACC_TRUNC,
                                       fcpl.get());
  ASSERT_TRUE(file.valid());

  size_t count = 999;
  EXPECT_GE(H5F__get_sohm_mesg_count_test(file.get(), H5O_ATTR_ID, &count), 0);
  EXPECT_EQ(count, 0U);

  // Write two groups sharing identical attributes so SOHM stores an entry.
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  const int32_t attr_val = 42;
  for (const char* grp_name : {"/g1", "/g2"}) {
    ScopedGroup grp(
        H5Gcreate2(file.get(), grp_name, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    ScopedAttr attr(H5Acreate2(grp.get(), "shared_attr", H5T_NATIVE_INT32,
                               space.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(attr.valid());
    ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT32, &attr_val), 0);
  }

  count = 0;
  EXPECT_GE(H5F__get_sohm_mesg_count_test(file.get(), H5O_ATTR_ID, &count), 0);
  EXPECT_GT(count, 0U);

  // Querying a message type not in any index fails cleanly.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5F__get_sohm_mesg_count_test(file.get(), H5O_DTYPE_ID, &count),
              0);
  }
}

TEST(H5FtestTest, CheckCachedStabTestVerifiesRootStabAndDetectsMismatch) {
  // 1. Invalid file ID fails argument validation.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5F__check_cached_stab_test(H5I_INVALID_HID), 0);
  }

  // 2. Earliest-format (v0 superblock) file populates sblock->root_ent.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_V18),
      0);

  ScopedFile file(H5Fcreate("cached_stab.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                            fapl.get()));
  ASSERT_TRUE(file.valid());

  EXPECT_GE(H5F__check_cached_stab_test(file.get()), 0);

  // 3. Corrupting root_ent->type triggers H5G__verify_cached_stab_test failure.
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);
  ASSERT_NE(f->shared->sblock, nullptr);
  ASSERT_NE(f->shared->sblock->root_ent, nullptr);

  const H5G_cache_type_t orig_type = f->shared->sblock->root_ent->type;
  absl::Cleanup restore_type = [&]() {
    f->shared->sblock->root_ent->type = orig_type;
  };
  f->shared->sblock->root_ent->type = H5G_NOTHING_CACHED;

  ScopedErrorSilence silence;
  EXPECT_LT(H5F__check_cached_stab_test(file.get()), 0);
}

TEST(H5FtestTest, GetMaxaddrAndSbeAddrTestReportExpectedValues) {
  {
    ScopedErrorSilence silence;
    haddr_t addr = 0;
    EXPECT_LT(H5F__get_maxaddr_test(H5I_INVALID_HID, &addr), 0);
    EXPECT_LT(H5F__get_sbe_addr_test(H5I_INVALID_HID, &addr), 0);
  }

  // Earliest superblock has no extension (ext_addr == HADDR_UNDEF).
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST,
                                   H5F_LIBVER_V18),
              0);
    ScopedFile v0_file(
        H5Fcreate("v0_sbe.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(v0_file.valid());

    haddr_t maxaddr = 0;
    haddr_t sbe_addr = 0;
    EXPECT_GE(H5F__get_maxaddr_test(v0_file.get(), &maxaddr), 0);
    EXPECT_GT(maxaddr, 0U);
    EXPECT_NE(maxaddr, HADDR_UNDEF);

    EXPECT_GE(H5F__get_sbe_addr_test(v0_file.get(), &sbe_addr), 0);
    EXPECT_EQ(sbe_addr, HADDR_UNDEF);
  }

  // File with non-default free-space strategy creates a superblock extension.
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_PAGE,
                                         /*persist=*/false, /*threshold=*/1),
              0);
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_V110), 0);

    ScopedFile ext_file(
        H5Fcreate("v3_sbe.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    ASSERT_TRUE(ext_file.valid());

    haddr_t sbe_addr = HADDR_UNDEF;
    EXPECT_GE(H5F__get_sbe_addr_test(ext_file.get(), &sbe_addr), 0);
    EXPECT_NE(sbe_addr, HADDR_UNDEF);
  }
}

TEST(H5FtestTest, SameFileTestComparesSharedStructsAndValidatesArgs) {
  ScopedFile file1 = CreateInMemoryFile("same1.h5");
  ASSERT_TRUE(file1.valid());
  ScopedFile reopened(H5Freopen(file1.get()));
  ASSERT_TRUE(reopened.valid());
  ScopedFile file2 = CreateInMemoryFile("same2.h5");
  ASSERT_TRUE(file2.valid());

  EXPECT_GT(H5F__same_file_test(file1.get(), file1.get()), 0);
  EXPECT_GT(H5F__same_file_test(file1.get(), reopened.get()), 0);
  EXPECT_EQ(H5F__same_file_test(file1.get(), file2.get()), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5F__same_file_test(H5I_INVALID_HID, file2.get()), 0);
  EXPECT_LT(H5F__same_file_test(file1.get(), H5I_INVALID_HID), 0);
}

TEST(H5FtestTest, ReparseFileLockVariableTestParsesAllEnvStatesAndRestores) {
  std::optional<std::string> orig_env;
  if (const char* current = std::getenv("HDF5_USE_FILE_LOCKING")) {
    orig_env = current;
  }
  absl::Cleanup restore_env = [&]() {
    if (orig_env.has_value()) {
      setenv("HDF5_USE_FILE_LOCKING", orig_env->c_str(), 1);
    } else {
      unsetenv("HDF5_USE_FILE_LOCKING");
    }
    H5F__reparse_file_lock_variable_test();
  };

  struct LockCase {
    const char* env_value;
    htri_t expected_use_locks;
    htri_t expected_ignore_disabled;
  };
  constexpr LockCase kCases[] = {
      {"FALSE", 0, -1},
      {"0", 0, -1},
      {"BEST_EFFORT", 1, 1},
      {"TRUE", 1, 0},
      {"1", 1, 0},
      {"INVALID", -1, -1},
      {nullptr, -1, -1},
  };

  for (const auto& tc : kCases) {
    if (tc.env_value != nullptr) {
      ASSERT_EQ(setenv("HDF5_USE_FILE_LOCKING", tc.env_value, 1), 0);
    } else {
      ASSERT_EQ(unsetenv("HDF5_USE_FILE_LOCKING"), 0);
    }
    EXPECT_GE(H5F__reparse_file_lock_variable_test(), 0);
    EXPECT_EQ(use_locks_env_g, tc.expected_use_locks);
    EXPECT_EQ(ignore_disabled_locks_g, tc.expected_ignore_disabled);
  }
}

}  // namespace
}  // namespace hdf5
