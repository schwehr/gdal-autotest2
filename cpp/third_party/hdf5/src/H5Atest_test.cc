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
// Tests internal attribute testing routines H5A__is_shared_test and
// H5A__get_shared_rc_test (third_party/hdf5/src/H5Atest.c) in Google3's
// default non-MPI Linux build (-DNDEBUG).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Atest.c

#include <array>
#include <cstdint>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug/stats fields from
// internal cache/object structs pulled by H5private.h -> H5CXprivate.h ->
// H5ACprivate.h -> H5Cprivate.h. Define NDEBUG around the prelude and internal
// headers so struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5A_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5A_FRIEND
#define H5A_TESTING
#pragma push_macro("try")
#undef try
#define try h5_try_param_
#include "third_party/hdf5/src/H5Apkg.h"
#undef try
#pragma pop_macro("try")
}  // extern "C"
#ifdef H5A_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5A_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

ScopedPlist MakeSharedAttrFcpl(unsigned max_list, unsigned min_btree,
                               unsigned min_size = 1) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  if (!fcpl.valid()) {
    return fcpl;
  }
  if (H5Pset_shared_mesg_nindexes(fcpl, 1) < 0 ||
      H5Pset_shared_mesg_index(fcpl, 0, H5O_SHMESG_ATTR_FLAG, min_size) < 0 ||
      H5Pset_shared_mesg_phase_change(fcpl, max_list, min_btree) < 0) {
    fcpl.reset();
  }
  return fcpl;
}

TEST(H5AtestTest, InvalidAndNonAttributeIdsFail) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(
      H5Gcreate2(file, "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());

  ScopedErrorSilence silence;
  EXPECT_LT(H5A__is_shared_test(H5I_INVALID_HID), 0);
  EXPECT_LT(H5A__is_shared_test(file), 0);
  EXPECT_LT(H5A__is_shared_test(grp), 0);
  EXPECT_LT(H5A__is_shared_test(space), 0);

  hsize_t rc = 999;
  EXPECT_LT(H5A__get_shared_rc_test(H5I_INVALID_HID, &rc), 0);
  EXPECT_LT(H5A__get_shared_rc_test(file, &rc), 0);
  EXPECT_LT(H5A__get_shared_rc_test(grp, &rc), 0);
  EXPECT_LT(H5A__get_shared_rc_test(space, &rc), 0);
}

TEST(H5AtestTest, UnsharedAttributeReturnsFalseAndFailsRefcountQuery) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());

  ScopedAttr attr(H5Acreate2(file, "plain_attr", H5T_NATIVE_INT32, space,
                             H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());
  constexpr int32_t kVal = 7;
  ASSERT_GE(H5Awrite(attr, H5T_NATIVE_INT32, &kVal), 0);

  EXPECT_EQ(H5A__is_shared_test(attr), 0);

  // Under `:hdf5`'s `-DNDEBUG` build, H5A__get_shared_rc_test's assert is
  // compiled out and H5SM_get_refcount returns < 0 because SOHM is disabled.
  ScopedErrorSilence silence;
  hsize_t rc = 0;
  EXPECT_LT(H5A__get_shared_rc_test(attr, &rc), 0);
}

TEST(H5AtestTest, SharedAttributesReportStatusAndRefCountsInListIndex) {
  // Keep SOHM index in list format (max_list=10, min_btree=5) with min_size=100
  // so small scalar attributes remain unshared while 128-byte array attributes
  // participate in SOHM sharing.
  ScopedPlist fcpl =
      MakeSharedAttrFcpl(/*max_list=*/10, /*min_btree=*/5, /*min_size=*/100);
  ASSERT_TRUE(fcpl.valid());
  ScopedFile file =
      CreateInMemoryFile("sohm_list.h5", H5F_ACC_TRUNC, fcpl.get());
  ASSERT_TRUE(file.valid());

  ScopedGroup g1(H5Gcreate2(file, "/g1", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedGroup g2(H5Gcreate2(file, "/g2", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedGroup g3(H5Gcreate2(file, "/g3", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(g1.valid());
  ASSERT_TRUE(g2.valid());
  ASSERT_TRUE(g3.valid());

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  constexpr hsize_t kArrLen = 32;
  ScopedSpace arr_space(H5Screate_simple(1, &kArrLen, nullptr));
  ASSERT_TRUE(arr_space.valid());

  std::array<int32_t, kArrLen> shared_vals{};
  shared_vals.fill(42);
  constexpr int32_t kUniqueVal = 99;

  // 1. Small scalar attribute is below SOHM min_size (unshared); large array
  // attribute becomes SOHM-shareable (HERE, refcount 1).
  {
    ScopedAttr small_uniq(H5Acreate2(g1, "unique_small", H5T_NATIVE_INT32,
                                     scalar, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(small_uniq.valid());
    ASSERT_GE(H5Awrite(small_uniq, H5T_NATIVE_INT32, &kUniqueVal), 0);
    EXPECT_EQ(H5A__is_shared_test(small_uniq), 0);

    ScopedAttr a1(H5Acreate2(g1, "shared_a", H5T_NATIVE_INT32, arr_space,
                             H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a1.valid());
    ASSERT_GE(H5Awrite(a1, H5T_NATIVE_INT32, shared_vals.data()), 0);
    EXPECT_GT(H5A__is_shared_test(a1), 0);
    hsize_t rc = 0;
    ASSERT_GE(H5A__get_shared_rc_test(a1, &rc), 0);
    EXPECT_EQ(rc, 1U);
  }

  // 2. Second identical attribute promotes message to SOHM with refcount 2.
  {
    ScopedAttr a2(H5Acreate2(g2, "shared_a", H5T_NATIVE_INT32, arr_space,
                             H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a2.valid());
    ASSERT_GE(H5Awrite(a2, H5T_NATIVE_INT32, shared_vals.data()), 0);
    EXPECT_GT(H5A__is_shared_test(a2), 0);

    hsize_t rc = 0;
    ASSERT_GE(H5A__get_shared_rc_test(a2, &rc), 0);
    EXPECT_EQ(rc, 2U);
  }

  // 3. Reopening /g1/shared_a sees SOHM status; adding /g3 gives refcount 3.
  {
    ScopedAttr a1_reopen(H5Aopen(g1, "shared_a", H5P_DEFAULT));
    ASSERT_TRUE(a1_reopen.valid());
    EXPECT_GT(H5A__is_shared_test(a1_reopen), 0);
    hsize_t rc = 0;
    ASSERT_GE(H5A__get_shared_rc_test(a1_reopen, &rc), 0);
    EXPECT_EQ(rc, 2U);

    ScopedAttr a3(H5Acreate2(g3, "shared_a", H5T_NATIVE_INT32, arr_space,
                             H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a3.valid());
    ASSERT_GE(H5Awrite(a3, H5T_NATIVE_INT32, shared_vals.data()), 0);
    EXPECT_GT(H5A__is_shared_test(a3), 0);
    ASSERT_GE(H5A__get_shared_rc_test(a3, &rc), 0);
    EXPECT_EQ(rc, 3U);
  }

  // 4. Deleting one copy decrements refcount back down to 2.
  ASSERT_GE(H5Adelete(g3, "shared_a"), 0);
  ScopedAttr a2_check(H5Aopen(g2, "shared_a", H5P_DEFAULT));
  ASSERT_TRUE(a2_check.valid());
  EXPECT_GT(H5A__is_shared_test(a2_check), 0);
  hsize_t rc = 0;
  ASSERT_GE(H5A__get_shared_rc_test(a2_check, &rc), 0);
  EXPECT_EQ(rc, 2U);
}

TEST(H5AtestTest, SharedAttributesWorkInBtreeSohmIndex) {
  // Force B-tree SOHM storage (max_list=0, min_btree=0).
  ScopedPlist fcpl = MakeSharedAttrFcpl(/*max_list=*/0, /*min_btree=*/0);
  ASSERT_TRUE(fcpl.valid());
  ScopedFile file =
      CreateInMemoryFile("sohm_btree.h5", H5F_ACC_TRUNC, fcpl.get());
  ASSERT_TRUE(file.valid());

  ScopedGroup g1(H5Gcreate2(file, "/g1", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedGroup g2(H5Gcreate2(file, "/g2", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(g1.valid());
  ASSERT_TRUE(g2.valid());

  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  constexpr int64_t kVal = 123456789LL;

  {
    ScopedAttr a1(H5Acreate2(g1, "tag", H5T_NATIVE_INT64, space, H5P_DEFAULT,
                             H5P_DEFAULT));
    ASSERT_TRUE(a1.valid());
    ASSERT_GE(H5Awrite(a1, H5T_NATIVE_INT64, &kVal), 0);
  }
  {
    ScopedAttr a2(H5Acreate2(g2, "tag", H5T_NATIVE_INT64, space, H5P_DEFAULT,
                             H5P_DEFAULT));
    ASSERT_TRUE(a2.valid());
    ASSERT_GE(H5Awrite(a2, H5T_NATIVE_INT64, &kVal), 0);
    EXPECT_GT(H5A__is_shared_test(a2), 0);

    hsize_t rc = 0;
    ASSERT_GE(H5A__get_shared_rc_test(a2, &rc), 0);
    EXPECT_EQ(rc, 2U);
  }
}

}  // namespace
}  // namespace hdf5
