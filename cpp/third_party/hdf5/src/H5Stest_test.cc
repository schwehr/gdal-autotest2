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
// Tests internal dataspace selection testing routines in
// third_party/hdf5/src/H5Stest.c (diminfo status, rebuild status, internal
// span/point consistency checks, and selection offset verification) in
// Google3's default non-MPI Linux build (-DNDEBUG).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Stest.c

#include <array>
#include <cstdint>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug/stats fields from
// internal cache/API-context structs pulled by H5private.h -> H5CXprivate.h ->
// H5ACprivate.h -> H5Cprivate.h. Define NDEBUG around both the prelude and
// internal headers so struct offsets match `:hdf5`.
#ifndef NDEBUG
#define H5STEST_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5S_FRIEND
#define H5S_TESTING
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Spkg.h"
}  // extern "C"
#ifdef H5STEST_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5STEST_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

TEST(H5StestTest, InvalidIdsFailAllTestHelpers) {
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedPlist plist(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(plist.valid());

  ScopedErrorSilence silence;

  H5S_diminfo_valid_t s1 = H5S_DIMINFO_VALID_NO;
  H5S_diminfo_valid_t s2 = H5S_DIMINFO_VALID_NO;
  EXPECT_LT(H5S__get_rebuild_status_test(H5I_INVALID_HID, &s1, &s2), 0);
  EXPECT_LT(H5S__get_rebuild_status_test(plist, &s1, &s2), 0);
  EXPECT_LT(H5S__get_rebuild_status_test(H5T_NATIVE_INT32, &s1, &s2), 0);

  EXPECT_LT(H5S__get_diminfo_status_test(H5I_INVALID_HID, &s1), 0);
  EXPECT_LT(H5S__get_diminfo_status_test(plist, &s1), 0);

  EXPECT_LT(H5S__internal_consistency_test(H5I_INVALID_HID), 0);
  EXPECT_LT(H5S__internal_consistency_test(plist), 0);

  constexpr std::array<hssize_t, 2> kOffsets = {0, 0};
  EXPECT_LT(H5S__verify_offsets(H5I_INVALID_HID, kOffsets.data()), 0);
  EXPECT_LT(H5S__verify_offsets(plist, kOffsets.data()), 0);
}

TEST(H5StestTest, GetDiminfoAndRebuildStatusTransitions) {
  constexpr std::array<hsize_t, 2> kDims = {10, 10};
  ScopedSpace space(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  // 1. Regular hyperslab starts with H5S_DIMINFO_VALID_YES; rebuild keeps YES.
  constexpr std::array<hsize_t, 2> kStart1 = {0, 0};
  constexpr std::array<hsize_t, 2> kBlock1 = {4, 6};
  ASSERT_GE(H5Sselect_hyperslab(space, H5S_SELECT_SET, kStart1.data(), nullptr,
                                kBlock1.data(), nullptr),
            0);

  H5S_diminfo_valid_t status = H5S_DIMINFO_VALID_IMPOSSIBLE;
  ASSERT_GE(H5S__get_diminfo_status_test(space, &status), 0);
  EXPECT_EQ(status, H5S_DIMINFO_VALID_YES);

  H5S_diminfo_valid_t before = H5S_DIMINFO_VALID_IMPOSSIBLE;
  H5S_diminfo_valid_t after = H5S_DIMINFO_VALID_IMPOSSIBLE;
  ASSERT_GE(H5S__get_rebuild_status_test(space, &before, &after), 0);
  EXPECT_EQ(before, H5S_DIMINFO_VALID_YES);
  EXPECT_EQ(after, H5S_DIMINFO_VALID_YES);

  // 2. Strided H5S_SELECT_AND drops diminfo_valid to NO, and rebuild recovers
  //    YES when the intersection is still a single regular hyperslab.
  constexpr std::array<hsize_t, 2> kStartAnd = {0, 0};
  constexpr std::array<hsize_t, 2> kStrideAnd = {2, 2};
  constexpr std::array<hsize_t, 2> kCountAnd = {2, 2};
  constexpr std::array<hsize_t, 2> kBlockAnd = {1, 1};
  ASSERT_GE(H5Sselect_hyperslab(space, H5S_SELECT_AND, kStartAnd.data(),
                                kStrideAnd.data(), kCountAnd.data(),
                                kBlockAnd.data()),
            0);
  ASSERT_GE(H5S__get_diminfo_status_test(space, &status), 0);
  EXPECT_EQ(status, H5S_DIMINFO_VALID_NO);
  EXPECT_GT(H5S__internal_consistency_test(space), 0);

  ASSERT_GE(H5S__get_rebuild_status_test(space, &before, &after), 0);
  EXPECT_EQ(before, H5S_DIMINFO_VALID_NO);
  EXPECT_EQ(after, H5S_DIMINFO_VALID_YES);
  EXPECT_GT(H5S__internal_consistency_test(space), 0);

  // 3. Union with an L-shaped block produces an irregular hyperslab whose
  //    diminfo_valid transitions from NO -> IMPOSSIBLE upon rebuild.
  constexpr std::array<hsize_t, 2> kStartOr = {5, 6};
  constexpr std::array<hsize_t, 2> kBlockOr = {3, 2};
  ASSERT_GE(H5Sselect_hyperslab(space, H5S_SELECT_OR, kStartOr.data(), nullptr,
                                kBlockOr.data(), nullptr),
            0);
  ASSERT_GE(H5S__get_diminfo_status_test(space, &status), 0);
  EXPECT_EQ(status, H5S_DIMINFO_VALID_NO);

  ASSERT_GE(H5S__get_rebuild_status_test(space, &before, &after), 0);
  EXPECT_EQ(before, H5S_DIMINFO_VALID_NO);
  EXPECT_EQ(after, H5S_DIMINFO_VALID_IMPOSSIBLE);
  EXPECT_GT(H5S__internal_consistency_test(space), 0);

  // 4. Subsequent rebuild check on IMPOSSIBLE leaves it IMPOSSIBLE.
  ASSERT_GE(H5S__get_rebuild_status_test(space, &before, &after), 0);
  EXPECT_EQ(before, H5S_DIMINFO_VALID_IMPOSSIBLE);
  EXPECT_EQ(after, H5S_DIMINFO_VALID_IMPOSSIBLE);
}

TEST(H5StestTest, InternalConsistencyAcrossAllSelectionTypesAndOffsets) {
  constexpr std::array<hsize_t, 3> kDims = {6, 8, 10};
  ScopedSpace space(H5Screate_simple(3, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  // 1. ALL selection (default).
  EXPECT_GT(H5S__internal_consistency_test(space), 0);

  // 2. NONE selection.
  ASSERT_GE(H5Sselect_none(space), 0);
  EXPECT_GT(H5S__internal_consistency_test(space), 0);

  // 3. POINTS selection with multiple coordinates.
  constexpr std::array<hsize_t, 9> kCoords = {
      1, 2, 3, 4, 5, 6, 0, 1, 2,
  };
  ASSERT_GE(H5Sselect_elements(space, H5S_SELECT_SET, 3, kCoords.data()), 0);
  EXPECT_GT(H5S__internal_consistency_test(space), 0);

  // 4. 3D irregular hyperslab selection (exercises recursive span tail check).
  constexpr std::array<hsize_t, 3> kStart1 = {1, 1, 1};
  constexpr std::array<hsize_t, 3> kCount1 = {2, 2, 2};
  ASSERT_GE(H5Sselect_hyperslab(space, H5S_SELECT_SET, kStart1.data(), nullptr,
                                kCount1.data(), nullptr),
            0);
  constexpr std::array<hsize_t, 3> kStart2 = {3, 4, 5};
  constexpr std::array<hsize_t, 3> kCount2 = {2, 2, 2};
  ASSERT_GE(H5Sselect_hyperslab(space, H5S_SELECT_OR, kStart2.data(), nullptr,
                                kCount2.data(), nullptr),
            0);
  constexpr std::array<hssize_t, 3> kOffset = {1, -1, 2};
  ASSERT_GE(H5Soffset_simple(space, kOffset.data()), 0);
  EXPECT_GT(H5S__internal_consistency_test(space), 0);

  // 5. Negative offset moving selection below zero makes bounds query fail.
  constexpr std::array<hssize_t, 3> kBadOffset = {-5, 0, 0};
  ASSERT_GE(H5Soffset_simple(space, kBadOffset.data()), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5S__internal_consistency_test(space), 0);
  }

  // 6. Scalar and null dataspaces with ALL/NONE selections.
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  EXPECT_GT(H5S__internal_consistency_test(scalar), 0);
  ASSERT_GE(H5Sselect_none(scalar), 0);
  EXPECT_GT(H5S__internal_consistency_test(scalar), 0);

  ScopedSpace null_space(H5Screate(H5S_NULL));
  ASSERT_TRUE(null_space.valid());
  EXPECT_GT(H5S__internal_consistency_test(null_space), 0);
}

TEST(H5StestTest, InternalConsistencyDetectsCorruptedTailPointers) {
  constexpr std::array<hsize_t, 2> kDims = {10, 10};

  // 1. Corrupt irregular hyperslab span tail pointers (root and nested dim).
  {
    ScopedSpace space(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(space.valid());
    constexpr std::array<hsize_t, 2> kStart1 = {1, 1};
    constexpr std::array<hsize_t, 2> kCount1 = {2, 3};
    ASSERT_GE(H5Sselect_hyperslab(space, H5S_SELECT_SET, kStart1.data(),
                                  nullptr, kCount1.data(), nullptr),
              0);
    constexpr std::array<hsize_t, 2> kStart2 = {5, 6};
    constexpr std::array<hsize_t, 2> kCount2 = {2, 2};
    ASSERT_GE(H5Sselect_hyperslab(space, H5S_SELECT_OR, kStart2.data(), nullptr,
                                  kCount2.data(), nullptr),
              0);

    H5S_t* raw = static_cast<H5S_t*>(H5I_object_verify(space, H5I_DATASPACE));
    ASSERT_NE(raw, nullptr);
    H5S_hyper_span_info_t* root_spans = raw->select.sel_info.hslab->span_lst;
    ASSERT_NE(root_spans, nullptr);
    ASSERT_NE(root_spans->head, nullptr);
    ASSERT_NE(root_spans->head->down, nullptr);

    ScopedErrorSilence silence;

    // Corrupt root tail pointer.
    H5S_hyper_span_t* saved_root_tail = root_spans->tail;
    {
      absl::Cleanup restore = [&] { root_spans->tail = saved_root_tail; };
      root_spans->tail = nullptr;
      EXPECT_LT(H5S__internal_consistency_test(space), 0);
    }

    // Corrupt nested dimension tail pointer to trigger recursive propagation.
    H5S_hyper_span_info_t* down_spans = root_spans->head->down;
    H5S_hyper_span_t* saved_down_tail = down_spans->tail;
    {
      absl::Cleanup restore = [&] { down_spans->tail = saved_down_tail; };
      down_spans->tail = nullptr;
      EXPECT_LT(H5S__internal_consistency_test(space), 0);
    }

    EXPECT_GT(H5S__internal_consistency_test(space), 0);
  }

  // 3. Corrupt point selection tail pointer.
  {
    ScopedSpace space(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(space.valid());
    constexpr std::array<hsize_t, 4> kCoords = {1, 2, 5, 6};
    ASSERT_GE(H5Sselect_elements(space, H5S_SELECT_SET, 2, kCoords.data()), 0);

    H5S_t* raw = static_cast<H5S_t*>(H5I_object_verify(space, H5I_DATASPACE));
    ASSERT_NE(raw, nullptr);
    H5S_pnt_list_t* pnt_lst = raw->select.sel_info.pnt_lst;
    ASSERT_NE(pnt_lst, nullptr);

    ScopedErrorSilence silence;
    H5S_pnt_node_t* saved_tail = pnt_lst->tail;
    {
      absl::Cleanup restore = [&] { pnt_lst->tail = saved_tail; };
      pnt_lst->tail = pnt_lst->head;
      EXPECT_LT(H5S__internal_consistency_test(space), 0);
    }
    EXPECT_GT(H5S__internal_consistency_test(space), 0);
  }
}

TEST(H5StestTest, VerifyOffsetsChecksRankTypeAndValues) {
  // 1. Scalar and null dataspaces fail offset verification.
  {
    ScopedErrorSilence silence;
    constexpr std::array<hssize_t, 1> kZero = {0};

    ScopedSpace scalar(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(scalar.valid());
    EXPECT_LT(H5S__verify_offsets(scalar, kZero.data()), 0);

    ScopedSpace null_space(H5Screate(H5S_NULL));
    ASSERT_TRUE(null_space.valid());
    EXPECT_LT(H5S__verify_offsets(null_space, kZero.data()), 0);
  }

  // 2. Simple dataspace verifies initial, explicit, and reset offsets.
  constexpr std::array<hsize_t, 2> kDims = {10, 12};
  ScopedSpace space(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  constexpr std::array<hssize_t, 2> kZeros = {0, 0};
  EXPECT_GE(H5S__verify_offsets(space, kZeros.data()), 0);

  constexpr std::array<hssize_t, 2> kCustom = {3, -2};
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5S__verify_offsets(space, kCustom.data()), 0);
  }

  ASSERT_GE(H5Soffset_simple(space, kCustom.data()), 0);
  EXPECT_GE(H5S__verify_offsets(space, kCustom.data()), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5S__verify_offsets(space, kZeros.data()), 0);
  }

  ASSERT_GE(H5Soffset_simple(space, nullptr), 0);
  EXPECT_GE(H5S__verify_offsets(space, kZeros.data()), 0);
}

}  // namespace
}  // namespace hdf5
