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
// Tests dataspace selection operations, queries, adjustments, projections,
// fills, subtractions, serialization/deserialization, and selection iterators
// (third_party/hdf5/src/H5Sselect.c) in Google3's default non-MPI Linux build
// (-DNDEBUG).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Sselect.c

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug/stats fields from
// internal cache/API-context structs pulled by H5private.h -> H5CXprivate.h ->
// H5ACprivate.h -> H5Cprivate.h. Define NDEBUG around both the prelude and
// internal headers so struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5SSELECT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5S_FRIEND
#define H5S_TESTING
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Spkg.h"
}  // extern "C"
#ifdef H5SSELECT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5SSELECT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

// RAII wrapper for pushing/popping a thread API context configured with the
// default file access property list so internal H5S serialization routines can
// query libver bounds via H5CX_get_libver_bounds().
class ScopedApiContext {
 public:
  ScopedApiContext() {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      hid_t fapl_id = H5P_DEFAULT;
      if (H5CX_set_apl(&fapl_id, H5P_CLS_FACC, H5I_INVALID_HID, false) >= 0) {
        ready_ = true;
      }
    }
  }

  ~ScopedApiContext() {
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

  bool ready() const { return pushed_ && ready_; }

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
  bool ready_ = false;
};

H5S_t* RawSpace(hid_t space_id) {
  return static_cast<H5S_t*>(H5I_object_verify(space_id, H5I_DATASPACE));
}

TEST(H5SselectTest, OffsetSimpleAndInternalSelectOffset) {
  constexpr std::array<hsize_t, 2> kDims = {10, 12};
  ScopedSpace space(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  constexpr std::array<hsize_t, 2> kStart = {2, 3};
  constexpr std::array<hsize_t, 2> kCount = {3, 4};
  ASSERT_GE(H5Sselect_hyperslab(space, H5S_SELECT_SET, kStart.data(), nullptr,
                                kCount.data(), nullptr),
            0);

  constexpr std::array<hssize_t, 2> kOffset = {2, -1};
  ASSERT_GE(H5Soffset_simple(space, kOffset.data()), 0);

  std::array<hsize_t, 2> lo{};
  std::array<hsize_t, 2> hi{};
  ASSERT_GE(H5Sget_select_bounds(space, lo.data(), hi.data()), 0);
  EXPECT_THAT(lo, ElementsAre(4, 2));
  EXPECT_THAT(hi, ElementsAre(6, 5));

  // Passing nullptr resets offsets back to zero across all dimensions.
  ASSERT_GE(H5Soffset_simple(space, nullptr), 0);
  ASSERT_GE(H5Sget_select_bounds(space, lo.data(), hi.data()), 0);
  EXPECT_THAT(lo, ElementsAre(2, 3));
  EXPECT_THAT(hi, ElementsAre(4, 6));

  // Internal H5S_select_offset handles both explicit and NULL offsets.
  H5S_t* raw = RawSpace(space);
  ASSERT_NE(raw, nullptr);
  constexpr std::array<hssize_t, 2> kInternalOffset = {1, 2};
  EXPECT_GE(H5S_select_offset(raw, kInternalOffset.data()), 0);
  EXPECT_GE(H5S__verify_offsets(space, kInternalOffset.data()), 0);
  EXPECT_GE(H5S_select_offset(raw, nullptr), 0);
  constexpr std::array<hssize_t, 2> kZeros = {0, 0};
  EXPECT_GE(H5S__verify_offsets(space, kZeros.data()), 0);

  // Negative checks: invalid ID, scalar, and null dataspaces.
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ScopedSpace null_space(H5Screate(H5S_NULL));
  ASSERT_TRUE(scalar.valid());
  ASSERT_TRUE(null_space.valid());

  ScopedErrorSilence silence;
  EXPECT_LT(H5Soffset_simple(H5I_INVALID_HID, kOffset.data()), 0);
  EXPECT_LT(H5Soffset_simple(H5T_NATIVE_INT32, kOffset.data()), 0);
  EXPECT_LT(H5Soffset_simple(scalar, kOffset.data()), 0);
  EXPECT_LT(H5Soffset_simple(null_space, kOffset.data()), 0);
}

TEST(H5SselectTest, SelectCopyAcrossAllSelectionTypesAndShareFlag) {
  constexpr std::array<hsize_t, 2> kDims = {8, 8};
  ScopedSpace dst(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(dst.valid());

  // 1. Copy NONE selection.
  {
    ScopedSpace src(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(src.valid());
    ASSERT_GE(H5Sselect_none(src), 0);
    ASSERT_GE(H5Sselect_copy(dst, src), 0);
    EXPECT_EQ(H5Sget_select_type(dst), H5S_SEL_NONE);
    EXPECT_EQ(H5Sget_select_npoints(dst), 0);
  }

  // 2. Copy ALL selection.
  {
    ScopedSpace src(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(src.valid());
    ASSERT_GE(H5Sselect_copy(dst, src), 0);
    EXPECT_EQ(H5Sget_select_type(dst), H5S_SEL_ALL);
    EXPECT_EQ(H5Sget_select_npoints(dst), 64);
  }

  // 3. Copy POINTS selection.
  {
    ScopedSpace src(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(src.valid());
    constexpr std::array<hsize_t, 4> kCoords = {1, 2, 5, 6};
    ASSERT_GE(H5Sselect_elements(src, H5S_SELECT_SET, 2, kCoords.data()), 0);
    constexpr std::array<hssize_t, 2> kOffset = {1, 0};
    ASSERT_GE(H5Soffset_simple(src, kOffset.data()), 0);

    ASSERT_GE(H5Sselect_copy(dst, src), 0);
    EXPECT_EQ(H5Sget_select_type(dst), H5S_SEL_POINTS);
    EXPECT_EQ(H5Sget_select_npoints(dst), 2);
    EXPECT_GE(H5S__verify_offsets(dst, kOffset.data()), 0);
  }

  // 4. Copy irregular HYPERSLAB selection via public and shared internal copy.
  {
    ScopedSpace src(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(src.valid());
    constexpr std::array<hsize_t, 2> kStart1 = {0, 0};
    constexpr std::array<hsize_t, 2> kCount1 = {2, 3};
    ASSERT_GE(H5Sselect_hyperslab(src, H5S_SELECT_SET, kStart1.data(), nullptr,
                                  kCount1.data(), nullptr),
              0);
    constexpr std::array<hsize_t, 2> kStart2 = {4, 4};
    constexpr std::array<hsize_t, 2> kCount2 = {2, 2};
    ASSERT_GE(H5Sselect_hyperslab(src, H5S_SELECT_OR, kStart2.data(), nullptr,
                                  kCount2.data(), nullptr),
              0);

    ASSERT_GE(H5Sselect_copy(dst, src), 0);
    EXPECT_EQ(H5Sget_select_type(dst), H5S_SEL_HYPERSLABS);
    EXPECT_EQ(H5Sget_select_npoints(dst), 10);

    ScopedSpace shared_dst(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(shared_dst.valid());
    H5S_t* raw_dst = RawSpace(shared_dst);
    H5S_t* raw_src = RawSpace(src);
    ASSERT_NE(raw_dst, nullptr);
    ASSERT_NE(raw_src, nullptr);
    EXPECT_GE(H5S_select_copy(raw_dst, raw_src, /*share_selection=*/true), 0);
    EXPECT_EQ(H5S_get_select_type(raw_dst), H5S_SEL_HYPERSLABS);
    EXPECT_EQ(H5S_get_select_npoints(raw_dst), 10);
  }

  // 5. Negative ID checks for H5Sselect_copy.
  ScopedErrorSilence silence;
  EXPECT_LT(H5Sselect_copy(H5I_INVALID_HID, dst), 0);
  EXPECT_LT(H5Sselect_copy(dst, H5I_INVALID_HID), 0);
  EXPECT_LT(H5Sselect_copy(H5T_NATIVE_INT32, dst), 0);
  EXPECT_LT(H5Sselect_copy(dst, H5T_NATIVE_INT32), 0);
}

TEST(H5SselectTest, QueriesNpointsTypeValidAndBounds) {
  constexpr std::array<hsize_t, 2> kDims = {10, 10};
  ScopedSpace space(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  EXPECT_EQ(H5Sget_select_type(space), H5S_SEL_ALL);
  EXPECT_EQ(H5Sget_select_npoints(space), 100);
  EXPECT_GT(H5Sselect_valid(space), 0);

  std::array<hsize_t, 2> lo{};
  std::array<hsize_t, 2> hi{};
  ASSERT_GE(H5Sget_select_bounds(space, lo.data(), hi.data()), 0);
  EXPECT_THAT(lo, ElementsAre(0, 0));
  EXPECT_THAT(hi, ElementsAre(9, 9));

  // Shift hyperslab out of bounds with positive and negative offsets.
  constexpr std::array<hsize_t, 2> kStart = {2, 2};
  constexpr std::array<hsize_t, 2> kCount = {3, 4};
  ASSERT_GE(H5Sselect_hyperslab(space, H5S_SELECT_SET, kStart.data(), nullptr,
                                kCount.data(), nullptr),
            0);
  EXPECT_EQ(H5Sget_select_type(space), H5S_SEL_HYPERSLABS);
  EXPECT_EQ(H5Sget_select_npoints(space), 12);
  EXPECT_GT(H5Sselect_valid(space), 0);

  constexpr std::array<hssize_t, 2> kOutPos = {6, 0};
  ASSERT_GE(H5Soffset_simple(space, kOutPos.data()), 0);
  EXPECT_EQ(H5Sselect_valid(space), 0);

  constexpr std::array<hssize_t, 2> kOutNeg = {-3, 0};
  ASSERT_GE(H5Soffset_simple(space, kOutNeg.data()), 0);
  EXPECT_EQ(H5Sselect_valid(space), 0);

  ASSERT_GE(H5Sselect_none(space), 0);
  EXPECT_EQ(H5Sget_select_type(space), H5S_SEL_NONE);
  EXPECT_EQ(H5Sget_select_npoints(space), 0);
  EXPECT_GT(H5Sselect_valid(space), 0);

  // Direct internal query calls on H5S_t*.
  H5S_t* raw = RawSpace(space);
  ASSERT_NE(raw, nullptr);
  EXPECT_EQ(H5S_get_select_type(raw), H5S_SEL_NONE);
  EXPECT_EQ(H5S_get_select_npoints(raw), 0);
  EXPECT_GT(H5S_select_valid(raw), 0);

  ScopedErrorSilence silence;
  EXPECT_EQ(H5Sget_select_type(H5I_INVALID_HID), H5S_SEL_ERROR);
  EXPECT_LT(H5Sget_select_npoints(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Sselect_valid(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Sget_select_bounds(H5I_INVALID_HID, lo.data(), hi.data()), 0);
  EXPECT_LT(H5Sget_select_bounds(space, nullptr, hi.data()), 0);
  EXPECT_LT(H5Sget_select_bounds(space, lo.data(), nullptr), 0);
  // NONE selection has no bounding box.
  EXPECT_LT(H5Sget_select_bounds(space, lo.data(), hi.data()), 0);
  EXPECT_LT(H5S_get_select_bounds(raw, lo.data(), hi.data()), 0);
}

TEST(H5SselectTest, InternalPredicatesOffsetUnlimAndNonUnlimQueries) {
  constexpr std::array<hsize_t, 2> kDims = {4, 6};
  constexpr std::array<hsize_t, 2> kMaxDims = {H5S_UNLIMITED, 6};
  ScopedSpace space(H5Screate_simple(2, kDims.data(), kMaxDims.data()));
  ASSERT_TRUE(space.valid());
  H5S_t* raw = RawSpace(space);
  ASSERT_NE(raw, nullptr);

  // 1. ALL selection.
  hsize_t lin_off = 999;
  EXPECT_GE(H5S_get_select_offset(raw, &lin_off), 0);
  EXPECT_EQ(lin_off, 0);
  EXPECT_EQ(H5S_get_select_unlim_dim(raw), -1);
  EXPECT_GT(H5S_select_is_contiguous(raw), 0);
  EXPECT_GT(H5S_select_is_single(raw), 0);
  EXPECT_GT(H5S_select_is_regular(raw), 0);
  {
    ScopedErrorSilence silence;
    hsize_t slice_elems = 0;
    EXPECT_LT(H5S_get_select_num_elem_non_unlim(raw, &slice_elems), 0);
  }

  // 2. NONE selection.
  ASSERT_GE(H5Sselect_none(space), 0);
  EXPECT_EQ(H5S_get_select_unlim_dim(raw), -1);
  EXPECT_EQ(H5S_select_is_contiguous(raw), 0);
  EXPECT_EQ(H5S_select_is_single(raw), 0);
  EXPECT_GT(H5S_select_is_regular(raw), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5S_get_select_offset(raw, &lin_off), 0);
    hsize_t slice_elems = 0;
    EXPECT_LT(H5S_get_select_num_elem_non_unlim(raw, &slice_elems), 0);
  }

  // 3. Finite strided hyperslab.
  constexpr std::array<hsize_t, 2> kStart = {1, 2};
  constexpr std::array<hsize_t, 2> kStride = {2, 2};
  constexpr std::array<hsize_t, 2> kCount = {2, 2};
  ASSERT_GE(H5Sselect_hyperslab(space, H5S_SELECT_SET, kStart.data(),
                                kStride.data(), kCount.data(), nullptr),
            0);
  EXPECT_GE(H5S_get_select_offset(raw, &lin_off), 0);
  EXPECT_EQ(lin_off, 1 * 6 + 2);
  EXPECT_EQ(H5S_get_select_unlim_dim(raw), -1);
  EXPECT_EQ(H5S_select_is_contiguous(raw), 0);
  EXPECT_EQ(H5S_select_is_single(raw), 0);
  EXPECT_GT(H5S_select_is_regular(raw), 0);
  {
    ScopedErrorSilence silence;
    hsize_t slice_elems = 0;
    // Finite hyperslab has no unlimited dimension, so callback fails.
    EXPECT_LT(H5S_get_select_num_elem_non_unlim(raw, &slice_elems), 0);
  }

  // 4. Unlimited hyperslab along dimension 0.
  constexpr std::array<hsize_t, 2> kUnlimStart = {1, 1};
  constexpr std::array<hsize_t, 2> kUnlimCount = {H5S_UNLIMITED, 3};
  ASSERT_GE(H5Sselect_hyperslab(space, H5S_SELECT_SET, kUnlimStart.data(),
                                nullptr, kUnlimCount.data(), nullptr),
            0);
  EXPECT_EQ(H5S_get_select_unlim_dim(raw), 0);
  hsize_t non_unlim = 0;
  EXPECT_GE(H5S_get_select_num_elem_non_unlim(raw, &non_unlim), 0);
  EXPECT_EQ(non_unlim, 3);
}

TEST(H5SselectTest, SerializeAndDeserializeAllSelectionTypesAndErrors) {
  ASSERT_GE(H5open(), 0);
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.ready());

  constexpr std::array<hsize_t, 2> kDims = {8, 8};

  // Round-trip helper through H5S_select_serial_size / H5S_select_serialize /
  // H5S_select_deserialize into both a pre-created dataspace and a newly
  // allocated dataspace (*space == nullptr). Note: H5S_SEL_ALL does not
  // serialize extent dimensions, so allocating a fresh H5S_SIMPLE space
  // produces a 0-rank placeholder with 0 selected points. Also avoid SIZE_MAX
  // for p_size because H5Sselect.c computes `*p + p_size - 1`, which triggers
  // UBSan pointer-overflow.
  auto verify_roundtrip = [&](hid_t src_id, H5S_sel_type expected_type,
                              hssize_t expected_npts,
                              hssize_t expected_alloc_npts) {
    H5S_t* raw_src = RawSpace(src_id);
    ASSERT_NE(raw_src, nullptr);

    const hssize_t ssize = H5S_select_serial_size(raw_src);
    ASSERT_GE(ssize, 0);
    std::vector<uint8_t> buf(static_cast<size_t>(ssize));
    uint8_t* write_ptr = buf.data();
    ASSERT_GE(H5S_select_serialize(raw_src, &write_ptr), 0);
    EXPECT_EQ(write_ptr, buf.data() + ssize);

    // Deserialize into a caller-provided dataspace using exact buffer size.
    ScopedSpace dst(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(dst.valid());
    H5S_t* raw_dst = RawSpace(dst);
    const uint8_t* read_ptr = buf.data();
    ASSERT_GE(H5S_select_deserialize(&raw_dst, &read_ptr, buf.size()), 0);
    EXPECT_EQ(H5Sget_select_type(dst), expected_type);
    EXPECT_EQ(H5Sget_select_npoints(dst), expected_npts);

    // Deserialize allocating a new H5S_t* using bounded buffer size.
    H5S_t* alloc_dst = nullptr;
    absl::Cleanup close_alloc = [&] {
      if (alloc_dst != nullptr) {
        H5S_close(alloc_dst);
      }
    };
    read_ptr = buf.data();
    ASSERT_GE(H5S_select_deserialize(&alloc_dst, &read_ptr, buf.size()), 0);
    ASSERT_NE(alloc_dst, nullptr);
    EXPECT_EQ(H5S_get_select_type(alloc_dst), expected_type);
    EXPECT_EQ(static_cast<hssize_t>(H5S_get_select_npoints(alloc_dst)),
              expected_alloc_npts);
  };

  // 1. ALL selection.
  {
    ScopedSpace s(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(s.valid());
    verify_roundtrip(s, H5S_SEL_ALL, 64, 0);
  }

  // 2. NONE selection.
  {
    ScopedSpace s(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(s.valid());
    ASSERT_GE(H5Sselect_none(s), 0);
    verify_roundtrip(s, H5S_SEL_NONE, 0, 0);
  }

  // 3. POINTS selection.
  {
    ScopedSpace s(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(s.valid());
    constexpr std::array<hsize_t, 6> kPts = {0, 1, 2, 3, 7, 7};
    ASSERT_GE(H5Sselect_elements(s, H5S_SELECT_SET, 3, kPts.data()), 0);
    verify_roundtrip(s, H5S_SEL_POINTS, 3, 3);
  }

  // 4. HYPERSLAB selection.
  {
    ScopedSpace s(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(s.valid());
    constexpr std::array<hsize_t, 2> kStart = {1, 2};
    constexpr std::array<hsize_t, 2> kCount = {3, 4};
    ASSERT_GE(H5Sselect_hyperslab(s, H5S_SELECT_SET, kStart.data(), nullptr,
                                  kCount.data(), nullptr),
              0);
    verify_roundtrip(s, H5S_SEL_HYPERSLABS, 12, 12);
  }

  // 5. Deserialization error paths: short buffer (< 4 bytes), unknown sel_type,
  //    and truncated payload after valid sel_type.
  ScopedErrorSilence silence;
  ScopedSpace target(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(target.valid());
  H5S_t* raw_target = RawSpace(target);

  std::array<uint8_t, 16> bad_buf{};
  const uint8_t* p = bad_buf.data();
  EXPECT_LT(H5S_select_deserialize(&raw_target, &p, 2), 0);

  // Little-endian sel_type = 99 (invalid).
  bad_buf[0] = 99;
  p = bad_buf.data();
  EXPECT_LT(H5S_select_deserialize(&raw_target, &p, bad_buf.size()), 0);

  // Valid sel_type = H5S_SEL_POINTS (1), but truncated (only 4 bytes total).
  bad_buf[0] = static_cast<uint8_t>(H5S_SEL_POINTS);
  p = bad_buf.data();
  EXPECT_LT(H5S_select_deserialize(&raw_target, &p, 4), 0);
}

TEST(H5SselectTest, AdjustPublicSignedAndInternalUnsigned) {
  constexpr std::array<hsize_t, 2> kDims = {10, 10};
  ScopedSpace space(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  // 1. Adjust hyperslab with positive and negative signed offsets.
  constexpr std::array<hsize_t, 2> kStart = {4, 5};
  constexpr std::array<hsize_t, 2> kCount = {2, 3};
  ASSERT_GE(H5Sselect_hyperslab(space, H5S_SELECT_SET, kStart.data(), nullptr,
                                kCount.data(), nullptr),
            0);

  constexpr std::array<hssize_t, 2> kShiftPos = {2, 3};
  ASSERT_GE(H5Sselect_adjust(space, kShiftPos.data()), 0);

  std::array<hsize_t, 2> lo{};
  std::array<hsize_t, 2> hi{};
  ASSERT_GE(H5Sget_select_bounds(space, lo.data(), hi.data()), 0);
  EXPECT_THAT(lo, ElementsAre(2, 2));
  EXPECT_THAT(hi, ElementsAre(3, 4));

  // Negative signed adjustment moves selection up.
  constexpr std::array<hssize_t, 2> kShiftNeg = {-1, -2};
  ASSERT_GE(H5Sselect_adjust(space, kShiftNeg.data()), 0);
  ASSERT_GE(H5Sget_select_bounds(space, lo.data(), hi.data()), 0);
  EXPECT_THAT(lo, ElementsAre(3, 4));
  EXPECT_THAT(hi, ElementsAre(4, 6));

  // Internal H5S_select_adjust_u subtracts unsigned offset on points too.
  constexpr std::array<hsize_t, 4> kPts = {3, 4, 6, 7};
  ASSERT_GE(H5Sselect_elements(space, H5S_SELECT_SET, 2, kPts.data()), 0);
  H5S_t* raw = RawSpace(space);
  ASSERT_NE(raw, nullptr);
  constexpr std::array<hsize_t, 2> kShiftU = {1, 2};
  EXPECT_GE(H5S_select_adjust_u(raw, kShiftU.data()), 0);
  ASSERT_GE(H5Sget_select_bounds(space, lo.data(), hi.data()), 0);
  EXPECT_THAT(lo, ElementsAre(2, 2));
  EXPECT_THAT(hi, ElementsAre(5, 5));

  // 2. Negative tests for H5Sselect_adjust.
  ScopedErrorSilence silence;
  EXPECT_LT(H5Sselect_adjust(H5I_INVALID_HID, kShiftPos.data()), 0);
  EXPECT_LT(H5Sselect_adjust(H5T_NATIVE_INT32, kShiftPos.data()), 0);
  EXPECT_LT(H5Sselect_adjust(space, nullptr), 0);

  // Moving selection below zero fails.
  constexpr std::array<hssize_t, 2> kTooLarge = {10, 0};
  EXPECT_LT(H5Sselect_adjust(space, kTooLarge.data()), 0);

  // H5S_SELECT_BOUNDS failure (NONE selection, or negative H5Soffset_simple).
  ASSERT_GE(H5Sselect_none(space), 0);
  EXPECT_LT(H5Sselect_adjust(space, kShiftPos.data()), 0);

  ASSERT_GE(H5Sselect_elements(space, H5S_SELECT_SET, 2, kPts.data()), 0);
  constexpr std::array<hssize_t, 2> kNegBaseOffset = {-10, 0};
  ASSERT_GE(H5Soffset_simple(space, kNegBaseOffset.data()), 0);
  EXPECT_LT(H5Sselect_adjust(space, kShiftPos.data()), 0);
}

TEST(H5SselectTest, SelectShapeSameCoversEqualDifferentRanksAndMixedTypes) {
  // 1. Scalar vs scalar (same npoints vs different npoints).
  ScopedSpace scalar1(H5Screate(H5S_SCALAR));
  ScopedSpace scalar2(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar1.valid());
  ASSERT_TRUE(scalar2.valid());
  EXPECT_GT(H5Sselect_shape_same(scalar1, scalar2), 0);
  ASSERT_GE(H5Sselect_none(scalar2), 0);
  EXPECT_EQ(H5Sselect_shape_same(scalar1, scalar2), 0);

  // 2. Same-type NONE selections of different ranks have the same shape.
  constexpr std::array<hsize_t, 2> kDims2D = {6, 8};
  constexpr std::array<hsize_t, 3> kDims3D = {4, 6, 8};
  ScopedSpace none2d(H5Screate_simple(2, kDims2D.data(), nullptr));
  ScopedSpace none3d(H5Screate_simple(3, kDims3D.data(), nullptr));
  ASSERT_TRUE(none2d.valid());
  ASSERT_TRUE(none3d.valid());
  ASSERT_GE(H5Sselect_none(none2d), 0);
  ASSERT_GE(H5Sselect_none(none3d), 0);
  EXPECT_GT(H5Sselect_shape_same(none2d, none3d), 0);

  // 3. Different ranks where leading extra dim is NOT flat -> false.
  ScopedSpace s3d(H5Screate_simple(3, kDims3D.data(), nullptr));
  ScopedSpace s2d(H5Screate_simple(2, kDims2D.data(), nullptr));
  ASSERT_TRUE(s3d.valid());
  ASSERT_TRUE(s2d.valid());
  constexpr std::array<hsize_t, 3> kStart3D = {0, 0, 0};
  constexpr std::array<hsize_t, 3> kCountNonFlat3D = {2, 2, 2};
  constexpr std::array<hsize_t, 2> kStart2D = {0, 0};
  constexpr std::array<hsize_t, 2> kCount2D = {2, 4};
  ASSERT_GE(H5Sselect_hyperslab(s3d, H5S_SELECT_SET, kStart3D.data(), nullptr,
                                kCountNonFlat3D.data(), nullptr),
            0);
  ASSERT_GE(H5Sselect_hyperslab(s2d, H5S_SELECT_SET, kStart2D.data(), nullptr,
                                kCount2D.data(), nullptr),
            0);
  EXPECT_EQ(H5Sselect_shape_same(s3d, s2d), 0);
  EXPECT_EQ(H5Sselect_shape_same(s2d, s3d), 0);

  // 4. Different ranks where leading dim IS flat (thickness 1) -> true (both
  //    argument orders) when single block, and bounds range mismatch -> false.
  constexpr std::array<hsize_t, 3> kCountFlat3D = {1, 2, 4};
  ASSERT_GE(H5Sselect_hyperslab(s3d, H5S_SELECT_SET, kStart3D.data(), nullptr,
                                kCountFlat3D.data(), nullptr),
            0);
  EXPECT_GT(H5Sselect_shape_same(s3d, s2d), 0);
  EXPECT_GT(H5Sselect_shape_same(s2d, s3d), 0);

  // Same npoints=8, different bounding box: 4x2 vs 2x4 -> false.
  constexpr std::array<hsize_t, 2> kCount2DAlt = {4, 2};
  ASSERT_GE(H5Sselect_hyperslab(s2d, H5S_SELECT_SET, kStart2D.data(), nullptr,
                                kCount2DAlt.data(), nullptr),
            0);
  EXPECT_EQ(H5Sselect_shape_same(s3d, s2d), 0);

  // 5. Both single-block with different selection types (1x1 hyperslab vs
  //    1-point selection) -> true via H5S_SELECT_IS_SINGLE fast path.
  constexpr std::array<hsize_t, 2> kOne = {1, 1};
  ASSERT_GE(H5Sselect_hyperslab(s2d, H5S_SELECT_SET, kStart2D.data(), nullptr,
                                kOne.data(), nullptr),
            0);
  ScopedSpace pts2d(H5Screate_simple(2, kDims2D.data(), nullptr));
  ASSERT_TRUE(pts2d.valid());
  constexpr std::array<hsize_t, 2> kSinglePt = {3, 4};
  ASSERT_GE(H5Sselect_elements(pts2d, H5S_SELECT_SET, 1, kSinglePt.data()), 0);
  EXPECT_GT(H5Sselect_shape_same(s2d, pts2d), 0);

  // 6. Mixed selection types (multi-block hyperslab vs multi-point selection)
  //    to exercise the block-by-block fallback loop (lines 1733-1855):
  //    6a. Matching 2-block strided hyperslab (1x1 blocks at (1,1),(1,3)) vs
  //        shifted points ((2,2),(2,4)) in 3D flat-first-dim vs 2D -> true.
  constexpr std::array<hsize_t, 3> kHypMatchStart = {2, 1, 1};
  constexpr std::array<hsize_t, 3> kHypMatchStride = {1, 1, 2};
  constexpr std::array<hsize_t, 3> kHypMatchCount = {1, 1, 2};
  ASSERT_GE(H5Sselect_hyperslab(s3d, H5S_SELECT_SET, kHypMatchStart.data(),
                                kHypMatchStride.data(), kHypMatchCount.data(),
                                nullptr),
            0);
  constexpr std::array<hsize_t, 4> kPtsMatch = {2, 2, 2, 4};
  ASSERT_GE(H5Sselect_elements(pts2d, H5S_SELECT_SET, 2, kPtsMatch.data()), 0);
  EXPECT_GT(H5Sselect_shape_same(s3d, pts2d), 0);

  //    6b. First block size mismatch: hyperslab of two 1x2 blocks vs 4 points
  //        sharing exact same bounds [0,0]..[2,1] and npoints=4 -> false.
  ScopedSpace hyp2d(H5Screate_simple(2, kDims2D.data(), nullptr));
  ASSERT_TRUE(hyp2d.valid());
  constexpr std::array<hsize_t, 2> kStride2x2 = {2, 1};
  constexpr std::array<hsize_t, 2> kCount2x1 = {2, 1};
  constexpr std::array<hsize_t, 2> kBlock1x2 = {1, 2};
  ASSERT_GE(H5Sselect_hyperslab(hyp2d, H5S_SELECT_SET, kStart2D.data(),
                                kStride2x2.data(), kCount2x1.data(),
                                kBlock1x2.data()),
            0);
  constexpr std::array<hsize_t, 8> kFourPts = {0, 0, 0, 1, 2, 0, 2, 1};
  ASSERT_GE(H5Sselect_elements(pts2d, H5S_SELECT_SET, 4, kFourPts.data()), 0);
  EXPECT_EQ(H5Sselect_shape_same(hyp2d, pts2d), 0);

  //    6c. Second block offset mismatch: 3 strided 1x1 blocks at col 0, 2, 4
  //        vs 3 points at col 0, 1, 4 (same npoints=3, same bounds) -> false.
  constexpr std::array<hsize_t, 2> kStride1x2 = {1, 2};
  constexpr std::array<hsize_t, 2> kCount1x3 = {1, 3};
  ASSERT_GE(H5Sselect_hyperslab(hyp2d, H5S_SELECT_SET, kStart2D.data(),
                                kStride1x2.data(), kCount1x3.data(), nullptr),
            0);
  constexpr std::array<hsize_t, 6> kThreePtsShift = {0, 0, 0, 1, 0, 4};
  ASSERT_GE(H5Sselect_elements(pts2d, H5S_SELECT_SET, 3, kThreePtsShift.data()),
            0);
  EXPECT_EQ(H5Sselect_shape_same(hyp2d, pts2d), 0);

  //    6d. Second block size mismatch: L-shaped 3-element hyperslab with block0
  //        {(0,0)} (1x1) and block1 {(1,0),(1,1)} (1x2) vs 3 points
  //        (0,0),(1,0), (1,1) (same npoints=3, same bounds [0,0]..[1,1]) ->
  //        false.
  ASSERT_GE(H5Sselect_hyperslab(hyp2d, H5S_SELECT_SET, kStart2D.data(), nullptr,
                                kOne.data(), nullptr),
            0);
  constexpr std::array<hsize_t, 2> kRow1Start = {1, 0};
  constexpr std::array<hsize_t, 2> kRow1Count = {1, 2};
  ASSERT_GE(H5Sselect_hyperslab(hyp2d, H5S_SELECT_OR, kRow1Start.data(),
                                nullptr, kRow1Count.data(), nullptr),
            0);
  constexpr std::array<hsize_t, 6> kLPts = {0, 0, 1, 0, 1, 1};
  ASSERT_GE(H5Sselect_elements(pts2d, H5S_SELECT_SET, 3, kLPts.data()), 0);
  EXPECT_EQ(H5Sselect_shape_same(hyp2d, pts2d), 0);

  // 7. Error checks: invalid IDs and bounds failure via negative offset.
  ScopedErrorSilence silence;
  EXPECT_LT(H5Sselect_shape_same(H5I_INVALID_HID, hyp2d), 0);
  EXPECT_LT(H5Sselect_shape_same(hyp2d, H5I_INVALID_HID), 0);

  constexpr std::array<hsize_t, 2> kOffStart = {1, 1};
  constexpr std::array<hsize_t, 2> kOffCount = {2, 2};
  ASSERT_GE(H5Sselect_hyperslab(hyp2d, H5S_SELECT_SET, kOffStart.data(),
                                nullptr, kOffCount.data(), nullptr),
            0);
  ASSERT_GE(H5Sselect_hyperslab(s2d, H5S_SELECT_SET, kOffStart.data(), nullptr,
                                kOffCount.data(), nullptr),
            0);
  constexpr std::array<hssize_t, 2> kBadOffset = {-5, 0};
  ASSERT_GE(H5Soffset_simple(hyp2d, kBadOffset.data()), 0);
  EXPECT_LT(H5Sselect_shape_same(hyp2d, s2d), 0);
  EXPECT_LT(H5Sselect_shape_same(s2d, hyp2d), 0);
}

TEST(H5SselectTest, SelectIntersectBlockAllTypesAndErrors) {
  constexpr std::array<hsize_t, 2> kDims = {10, 10};
  ScopedSpace space(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  // 1. ALL selection intersects block within extent.
  constexpr std::array<hsize_t, 2> kBlkStart = {2, 2};
  constexpr std::array<hsize_t, 2> kBlkEnd = {4, 4};
  EXPECT_GT(H5Sselect_intersect_block(space, kBlkStart.data(), kBlkEnd.data()),
            0);

  // 2. Strided hyperslab:
  //    - Block outside selection bounding box -> early false.
  //    - Block inside bounding box in gap between strides -> false.
  //    - Block hitting selected element -> true.
  constexpr std::array<hsize_t, 2> kStart = {2, 2};
  constexpr std::array<hsize_t, 2> kStride = {4, 4};
  constexpr std::array<hsize_t, 2> kCount = {2, 2};
  ASSERT_GE(H5Sselect_hyperslab(space, H5S_SELECT_SET, kStart.data(),
                                kStride.data(), kCount.data(), nullptr),
            0);

  constexpr std::array<hsize_t, 2> kOutsideStart = {0, 0};
  constexpr std::array<hsize_t, 2> kOutsideEnd = {1, 1};
  EXPECT_EQ(H5Sselect_intersect_block(space, kOutsideStart.data(),
                                      kOutsideEnd.data()),
            0);

  constexpr std::array<hsize_t, 2> kGapStart = {3, 3};
  constexpr std::array<hsize_t, 2> kGapEnd = {5, 5};
  EXPECT_EQ(H5Sselect_intersect_block(space, kGapStart.data(), kGapEnd.data()),
            0);

  constexpr std::array<hsize_t, 2> kHitStart = {5, 5};
  constexpr std::array<hsize_t, 2> kHitEnd = {6, 6};
  EXPECT_GT(H5Sselect_intersect_block(space, kHitStart.data(), kHitEnd.data()),
            0);

  // 3. NONE selection never intersects.
  ASSERT_GE(H5Sselect_none(space), 0);
  EXPECT_EQ(H5Sselect_intersect_block(space, kHitStart.data(), kHitEnd.data()),
            0);

  // 4. Error checks: invalid ID, NULL pointers, start > end, negative offset.
  ScopedErrorSilence silence;
  EXPECT_LT(H5Sselect_intersect_block(H5I_INVALID_HID, kBlkStart.data(),
                                      kBlkEnd.data()),
            0);
  EXPECT_LT(H5Sselect_intersect_block(space, nullptr, kBlkEnd.data()), 0);
  EXPECT_LT(H5Sselect_intersect_block(space, kBlkStart.data(), nullptr), 0);

  constexpr std::array<hsize_t, 2> kInvStart = {5, 2};
  constexpr std::array<hsize_t, 2> kInvEnd = {3, 4};
  EXPECT_LT(H5Sselect_intersect_block(space, kInvStart.data(), kInvEnd.data()),
            0);

  ASSERT_GE(H5Sselect_hyperslab(space, H5S_SELECT_SET, kStart.data(),
                                kStride.data(), kCount.data(), nullptr),
            0);
  constexpr std::array<hssize_t, 2> kNegOffset = {-5, 0};
  ASSERT_GE(H5Soffset_simple(space, kNegOffset.data()), 0);
  EXPECT_LT(H5Sselect_intersect_block(space, kHitStart.data(), kHitEnd.data()),
            0);
}

TEST(H5SselectTest, ConstructProjectionUpDownScalarAndWithOffset) {
  ASSERT_GE(H5open(), 0);

  auto close_raw = [](H5S_t*& ptr) {
    if (ptr != nullptr) {
      H5S_close(ptr);
      ptr = nullptr;
    }
  };

  // 1. Project 2D single-element selection DOWN to scalar (rank 0), both with
  //    and without buf_adj, and 2D NONE selection DOWN to scalar.
  {
    constexpr std::array<hsize_t, 2> kDims = {4, 5};
    ScopedSpace base(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(base.valid());
    constexpr std::array<hsize_t, 2> kPt = {2, 3};
    ASSERT_GE(H5Sselect_elements(base, H5S_SELECT_SET, 1, kPt.data()), 0);

    H5S_t* proj = nullptr;
    absl::Cleanup free_proj = [&] { close_raw(proj); };
    ptrdiff_t buf_adj = -1;
    ASSERT_GE(
        H5S_select_construct_projection(RawSpace(base), &proj, 0, 4, &buf_adj),
        0);
    ASSERT_NE(proj, nullptr);
    EXPECT_EQ(H5S_get_select_type(proj), H5S_SEL_ALL);
    EXPECT_EQ(buf_adj, static_cast<ptrdiff_t>((2 * 5 + 3) * 4));
    close_raw(proj);

    // NULL buf_adj is explicitly permitted.
    ASSERT_GE(
        H5S_select_construct_projection(RawSpace(base), &proj, 0, 4, nullptr),
        0);
    ASSERT_NE(proj, nullptr);
    close_raw(proj);

    // NONE selection projected to scalar yields NONE selection.
    ASSERT_GE(H5Sselect_none(base), 0);
    ASSERT_GE(
        H5S_select_construct_projection(RawSpace(base), &proj, 0, 4, &buf_adj),
        0);
    ASSERT_NE(proj, nullptr);
    EXPECT_EQ(H5S_get_select_type(proj), H5S_SEL_NONE);
    EXPECT_EQ(buf_adj, 0);
  }

  // 2. Project 1D hyperslab UP to 3D with non-zero offset_changed.
  {
    constexpr std::array<hsize_t, 1> kDims1D = {10};
    ScopedSpace base(H5Screate_simple(1, kDims1D.data(), nullptr));
    ASSERT_TRUE(base.valid());
    constexpr std::array<hsize_t, 1> kStart1D = {1};
    constexpr std::array<hsize_t, 1> kCount1D = {3};
    ASSERT_GE(H5Sselect_hyperslab(base, H5S_SELECT_SET, kStart1D.data(),
                                  nullptr, kCount1D.data(), nullptr),
              0);
    constexpr std::array<hssize_t, 1> kOff1D = {2};
    ASSERT_GE(H5Soffset_simple(base, kOff1D.data()), 0);

    H5S_t* proj = nullptr;
    absl::Cleanup free_proj = [&] { close_raw(proj); };
    ptrdiff_t buf_adj = -1;
    ASSERT_GE(
        H5S_select_construct_projection(RawSpace(base), &proj, 3, 8, &buf_adj),
        0);
    ASSERT_NE(proj, nullptr);
    EXPECT_EQ(buf_adj, 0);
    EXPECT_EQ(H5S_get_simple_extent_ndims(proj), 3);
    EXPECT_GT(H5S_select_shape_same(RawSpace(base), proj), 0);
    std::array<hsize_t, 3> lo{};
    std::array<hsize_t, 3> hi{};
    ASSERT_GE(H5S_get_select_bounds(proj, lo.data(), hi.data()), 0);
    EXPECT_THAT(lo, ElementsAre(0, 0, 3));
    EXPECT_THAT(hi, ElementsAre(0, 0, 5));
  }

  // 3. Project 3D hyperslab (flat first dim at index 2) DOWN to 2D with offset.
  {
    constexpr std::array<hsize_t, 3> kDims3D = {4, 5, 6};
    ScopedSpace base(H5Screate_simple(3, kDims3D.data(), nullptr));
    ASSERT_TRUE(base.valid());
    constexpr std::array<hsize_t, 3> kStart3D = {1, 1, 2};
    constexpr std::array<hsize_t, 3> kCount3D = {1, 2, 3};
    ASSERT_GE(H5Sselect_hyperslab(base, H5S_SELECT_SET, kStart3D.data(),
                                  nullptr, kCount3D.data(), nullptr),
              0);
    constexpr std::array<hssize_t, 3> kOff3D = {1, 0, 1};
    ASSERT_GE(H5Soffset_simple(base, kOff3D.data()), 0);

    H5S_t* proj = nullptr;
    absl::Cleanup free_proj = [&] { close_raw(proj); };
    ptrdiff_t buf_adj = -1;
    ASSERT_GE(
        H5S_select_construct_projection(RawSpace(base), &proj, 2, 4, &buf_adj),
        0);
    ASSERT_NE(proj, nullptr);
    EXPECT_EQ(H5S_get_simple_extent_ndims(proj), 2);
    // Down-projection uses diminfo[0].start (1), not start + offset.
    EXPECT_EQ(buf_adj, static_cast<ptrdiff_t>(1 * 5 * 6 * 4));
    EXPECT_GT(H5S_select_shape_same(RawSpace(base), proj), 0);
  }
}

TEST(H5SselectTest, SelectIterateAndFillCoverAppLibEarlyStopAndErrors) {
  constexpr std::array<hsize_t, 2> kDims = {4, 5};
  ScopedSpace space(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  // 1. H5S_select_fill via both public H5Dfill and direct H5S_select_fill.
  constexpr std::array<hsize_t, 2> kStart = {1, 1};
  constexpr std::array<hsize_t, 2> kStride = {2, 2};
  constexpr std::array<hsize_t, 2> kCount = {2, 2};
  ASSERT_GE(H5Sselect_hyperslab(space, H5S_SELECT_SET, kStart.data(),
                                kStride.data(), kCount.data(), nullptr),
            0);

  std::array<int32_t, 20> buf{};
  buf.fill(-1);
  constexpr int32_t kFillVal = 42;
  ASSERT_GE(
      H5S_select_fill(&kFillVal, sizeof(kFillVal), RawSpace(space), buf.data()),
      0);
  // Selected coords are (1,1)=6, (1,3)=8, (3,1)=16, (3,3)=18.
  for (size_t idx = 0; idx < buf.size(); ++idx) {
    if (idx == 6 || idx == 8 || idx == 16 || idx == 18) {
      EXPECT_EQ(buf[idx], 42);
    } else {
      EXPECT_EQ(buf[idx], -1);
    }
  }

  // 2. H5Diterate (H5S_SEL_ITER_OP_APP): full traversal, early positive stop,
  //    and negative error stop.
  struct IterLog {
    std::vector<int32_t> values;
    std::vector<hsize_t> coords_flat;
    herr_t stop_rc = 0;
    size_t stop_after = 100;
  } log;

  auto app_cb = [](void* elem, hid_t /*type_id*/, unsigned ndim,
                   const hsize_t* point, void* operator_data) -> herr_t {
    auto* st = static_cast<IterLog*>(operator_data);
    st->values.push_back(*static_cast<const int32_t*>(elem));
    for (unsigned d = 0; d < ndim; ++d) {
      st->coords_flat.push_back(point[d]);
    }
    if (st->values.size() >= st->stop_after) {
      return st->stop_rc;
    }
    return 0;
  };

  std::iota(buf.begin(), buf.end(), 0);
  ASSERT_EQ(H5Diterate(buf.data(), H5T_NATIVE_INT32, space, app_cb, &log), 0);
  EXPECT_THAT(log.values, ElementsAre(6, 8, 16, 18));
  EXPECT_THAT(log.coords_flat, ElementsAre(1, 1, 1, 3, 3, 1, 3, 3));

  log = IterLog{.stop_rc = 7, .stop_after = 2};
  EXPECT_EQ(H5Diterate(buf.data(), H5T_NATIVE_INT32, space, app_cb, &log), 7);
  EXPECT_THAT(log.values, ElementsAre(6, 8));

  {
    ScopedErrorSilence silence;
    log = IterLog{.stop_rc = -1, .stop_after = 2};
    EXPECT_LT(H5Diterate(buf.data(), H5T_NATIVE_INT32, space, app_cb, &log), 0);
  }

  // 3. H5S_select_iterate with H5S_SEL_ITER_OP_LIB on scalar (ndims=0) and
  //    simple dataspace, plus unsupported op_type error.
  const H5T_t* native_i32 = static_cast<const H5T_t*>(
      H5I_object_verify(H5T_NATIVE_INT32, H5I_DATATYPE));
  ASSERT_NE(native_i32, nullptr);

  auto lib_cb = [](void* elem, const H5T_t* /*type*/, unsigned ndim,
                   const hsize_t* point, void* operator_data) -> herr_t {
    auto* st = static_cast<IterLog*>(operator_data);
    st->values.push_back(*static_cast<const int32_t*>(elem));
    for (unsigned d = 0; d < ndim; ++d) {
      st->coords_flat.push_back(point[d]);
    }
    return 0;
  };

  log = IterLog{};
  H5S_sel_iter_op_t lib_op{};
  lib_op.op_type = H5S_SEL_ITER_OP_LIB;
  lib_op.u.lib_op = lib_cb;
  EXPECT_EQ(H5S_select_iterate(buf.data(), native_i32, RawSpace(space), &lib_op,
                               &log),
            0);
  EXPECT_THAT(log.values, ElementsAre(6, 8, 16, 18));

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  int32_t scalar_val = 99;
  log = IterLog{};
  EXPECT_EQ(H5S_select_iterate(&scalar_val, native_i32, RawSpace(scalar),
                               &lib_op, &log),
            0);
  EXPECT_THAT(log.values, ElementsAre(99));

  // NONE selection executes zero iterations and returns 0.
  ASSERT_GE(H5Sselect_none(space), 0);
  log = IterLog{};
  EXPECT_EQ(H5S_select_iterate(buf.data(), native_i32, RawSpace(space), &lib_op,
                               &log),
            0);
  EXPECT_TRUE(log.values.empty());

  // Unsupported op_type returns < 0.
  ASSERT_GE(H5Sselect_all(space), 0);
  ScopedErrorSilence silence;
  H5S_sel_iter_op_t bad_op{};
  bad_op.op_type = static_cast<H5S_sel_iter_op_type_t>(99);
  EXPECT_LT(H5S_select_iterate(buf.data(), native_i32, RawSpace(space), &bad_op,
                               &log),
            0);
}

TEST(H5SselectTest, ProjectIntersectionCoversAllScalarPointsHyperslabErrors) {
  constexpr std::array<hsize_t, 2> kDims = {8, 8};

  // 1. ALL intersect space copies dst selection.
  {
    ScopedSpace src(H5Screate_simple(2, kDims.data(), nullptr));
    ScopedSpace dst(H5Screate_simple(2, kDims.data(), nullptr));
    ScopedSpace sis(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(src.valid());
    ASSERT_TRUE(dst.valid());
    ASSERT_TRUE(sis.valid());

    constexpr std::array<hsize_t, 2> kDstStart = {1, 2};
    constexpr std::array<hsize_t, 2> kDstCount = {2, 3};
    ASSERT_GE(H5Sselect_hyperslab(dst, H5S_SELECT_SET, kDstStart.data(),
                                  nullptr, kDstCount.data(), nullptr),
              0);
    constexpr std::array<hsize_t, 2> kSrcStart = {0, 0};
    constexpr std::array<hsize_t, 2> kSrcCount = {2, 3};
    ASSERT_GE(H5Sselect_hyperslab(src, H5S_SELECT_SET, kSrcStart.data(),
                                  nullptr, kSrcCount.data(), nullptr),
              0);

    ScopedSpace proj(H5Sselect_project_intersection(src, dst, sis));
    ASSERT_TRUE(proj.valid());
    EXPECT_EQ(H5Sget_select_npoints(proj), 6);
    std::array<hsize_t, 2> lo{};
    std::array<hsize_t, 2> hi{};
    ASSERT_GE(H5Sget_select_bounds(proj, lo.data(), hi.data()), 0);
    EXPECT_THAT(lo, ElementsAre(1, 2));
    EXPECT_THAT(hi, ElementsAre(2, 4));
  }

  // 2. Empty src_intersect_space yields NONE selection.
  {
    ScopedSpace src(H5Screate_simple(2, kDims.data(), nullptr));
    ScopedSpace dst(H5Screate_simple(2, kDims.data(), nullptr));
    ScopedSpace sis(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(src.valid());
    ASSERT_TRUE(dst.valid());
    ASSERT_TRUE(sis.valid());
    ASSERT_GE(H5Sselect_none(sis), 0);

    ScopedSpace proj(H5Sselect_project_intersection(src, dst, sis));
    ASSERT_TRUE(proj.valid());
    EXPECT_EQ(H5Sget_select_type(proj), H5S_SEL_NONE);
  }

  // 3. Scalar dst_space: intersecting vs non-intersecting single point.
  {
    ScopedSpace src(H5Screate_simple(2, kDims.data(), nullptr));
    ScopedSpace dst(H5Screate(H5S_SCALAR));
    ScopedSpace sis(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(src.valid());
    ASSERT_TRUE(dst.valid());
    ASSERT_TRUE(sis.valid());

    constexpr std::array<hsize_t, 2> kPt = {3, 4};
    ASSERT_GE(H5Sselect_elements(src, H5S_SELECT_SET, 1, kPt.data()), 0);
    constexpr std::array<hsize_t, 2> kHitStart = {2, 2};
    constexpr std::array<hsize_t, 2> kHitCount = {3, 3};
    ASSERT_GE(H5Sselect_hyperslab(sis, H5S_SELECT_SET, kHitStart.data(),
                                  nullptr, kHitCount.data(), nullptr),
              0);

    ScopedSpace proj_hit(H5Sselect_project_intersection(src, dst, sis));
    ASSERT_TRUE(proj_hit.valid());
    EXPECT_EQ(H5Sget_select_type(proj_hit), H5S_SEL_ALL);

    constexpr std::array<hsize_t, 2> kMissStart = {0, 0};
    constexpr std::array<hsize_t, 2> kMissCount = {2, 2};
    ASSERT_GE(H5Sselect_hyperslab(sis, H5S_SELECT_SET, kMissStart.data(),
                                  nullptr, kMissCount.data(), nullptr),
              0);
    ScopedSpace proj_miss(H5Sselect_project_intersection(src, dst, sis));
    ASSERT_TRUE(proj_miss.valid());
    EXPECT_EQ(H5Sget_select_type(proj_miss), H5S_SEL_NONE);

    // Negative offset on src triggers bounds failure in scalar dst path.
    ScopedErrorSilence silence;
    constexpr std::array<hssize_t, 2> kBadOffset = {-5, 0};
    ASSERT_GE(H5Soffset_simple(src, kBadOffset.data()), 0);
    EXPECT_EQ(H5Sselect_project_intersection(src, dst, sis), H5I_INVALID_HID);
  }

  // 4. Point src_intersect_space + point src/dst combinations +
  // hyperslab->hyperslab.
  {
    ScopedSpace src(H5Screate_simple(2, kDims.data(), nullptr));
    ScopedSpace dst(H5Screate_simple(2, kDims.data(), nullptr));
    ScopedSpace sis(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(src.valid());
    ASSERT_TRUE(dst.valid());
    ASSERT_TRUE(sis.valid());

    constexpr std::array<hsize_t, 6> kSrcPts = {0, 0, 1, 1, 2, 2};
    constexpr std::array<hsize_t, 6> kDstPts = {5, 5, 6, 6, 7, 7};
    constexpr std::array<hsize_t, 4> kSisPts = {0, 0, 2, 2};
    ASSERT_GE(H5Sselect_elements(src, H5S_SELECT_SET, 3, kSrcPts.data()), 0);
    ASSERT_GE(H5Sselect_elements(dst, H5S_SELECT_SET, 3, kDstPts.data()), 0);
    ASSERT_GE(H5Sselect_elements(sis, H5S_SELECT_SET, 2, kSisPts.data()), 0);

    ScopedSpace proj_pts(H5Sselect_project_intersection(src, dst, sis));
    ASSERT_TRUE(proj_pts.valid());
    EXPECT_EQ(H5Sget_select_type(proj_pts), H5S_SEL_POINTS);
    EXPECT_EQ(H5Sget_select_npoints(proj_pts), 2);
    std::array<hsize_t, 4> out_pts{};
    ASSERT_GE(H5Sget_select_elem_pointlist(proj_pts, 0, 2, out_pts.data()), 0);
    EXPECT_THAT(out_pts, ElementsAre(5, 5, 7, 7));

    // Hyperslab src & dst with point sis -> H5S__hyper_project_intersection.
    constexpr std::array<hsize_t, 2> kStart = {0, 0};
    constexpr std::array<hsize_t, 2> kCount = {2, 2};
    constexpr std::array<hsize_t, 2> kDstStart = {4, 4};
    ASSERT_GE(H5Sselect_hyperslab(src, H5S_SELECT_SET, kStart.data(), nullptr,
                                  kCount.data(), nullptr),
              0);
    ASSERT_GE(H5Sselect_hyperslab(dst, H5S_SELECT_SET, kDstStart.data(),
                                  nullptr, kCount.data(), nullptr),
              0);
    ScopedSpace proj_hyp(H5Sselect_project_intersection(src, dst, sis));
    ASSERT_TRUE(proj_hyp.valid());
    EXPECT_EQ(H5Sget_select_type(proj_hyp), H5S_SEL_HYPERSLABS);
    EXPECT_EQ(H5Sget_select_npoints(proj_hyp), 1);

    // Negative public argument checks.
    ScopedSpace sis_1d(
        H5Screate_simple(1, std::array<hsize_t, 1>{8}.data(), nullptr));
    ASSERT_TRUE(sis_1d.valid());
    ScopedErrorSilence silence;
    EXPECT_EQ(H5Sselect_project_intersection(H5I_INVALID_HID, dst, sis),
              H5I_INVALID_HID);
    EXPECT_EQ(H5Sselect_project_intersection(src, H5I_INVALID_HID, sis),
              H5I_INVALID_HID);
    EXPECT_EQ(H5Sselect_project_intersection(src, dst, H5I_INVALID_HID),
              H5I_INVALID_HID);
    // Rank mismatch between src (2D) and sis (1D).
    EXPECT_EQ(H5Sselect_project_intersection(src, dst, sis_1d),
              H5I_INVALID_HID);
    // Point count mismatch between src (4) and dst (1).
    EXPECT_EQ(H5Sselect_project_intersection(src, proj_hyp, sis),
              H5I_INVALID_HID);
  }
}

TEST(H5SselectTest, SelectSubtractCoversAllNoneHyperslabAndPointErrors) {
  constexpr std::array<hsize_t, 2> kDims = {6, 6};

  // 1. Subtracting NONE is a no-op; subtracting from NONE is a no-op.
  {
    ScopedSpace a(H5Screate_simple(2, kDims.data(), nullptr));
    ScopedSpace b(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(a.valid());
    ASSERT_TRUE(b.valid());
    ASSERT_GE(H5Sselect_none(b), 0);
    EXPECT_GE(H5S_select_subtract(RawSpace(a), RawSpace(b)), 0);
    EXPECT_EQ(H5Sget_select_type(a), H5S_SEL_ALL);

    EXPECT_GE(H5S_select_subtract(RawSpace(b), RawSpace(a)), 0);
    EXPECT_EQ(H5Sget_select_type(b), H5S_SEL_NONE);
  }

  // 2. Subtracting ALL yields NONE.
  {
    ScopedSpace a(H5Screate_simple(2, kDims.data(), nullptr));
    ScopedSpace b(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(a.valid());
    ASSERT_TRUE(b.valid());
    EXPECT_GE(H5S_select_subtract(RawSpace(a), RawSpace(b)), 0);
    EXPECT_EQ(H5Sget_select_type(a), H5S_SEL_NONE);
  }

  // 3. Subtracting hyperslab from ALL converts ALL to hyperslab and subtracts.
  {
    ScopedSpace a(H5Screate_simple(2, kDims.data(), nullptr));
    ScopedSpace b(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(a.valid());
    ASSERT_TRUE(b.valid());
    constexpr std::array<hsize_t, 2> kSubStart = {1, 1};
    constexpr std::array<hsize_t, 2> kSubCount = {2, 3};
    ASSERT_GE(H5Sselect_hyperslab(b, H5S_SELECT_SET, kSubStart.data(), nullptr,
                                  kSubCount.data(), nullptr),
              0);
    EXPECT_GE(H5S_select_subtract(RawSpace(a), RawSpace(b)), 0);
    EXPECT_EQ(H5Sget_select_type(a), H5S_SEL_HYPERSLABS);
    EXPECT_EQ(H5Sget_select_npoints(a), 36 - 6);

    // Subtracting hyperslab from hyperslab.
    constexpr std::array<hsize_t, 2> kSub2Start = {0, 0};
    constexpr std::array<hsize_t, 2> kSub2Count = {1, 6};
    ASSERT_GE(H5Sselect_hyperslab(b, H5S_SELECT_SET, kSub2Start.data(), nullptr,
                                  kSub2Count.data(), nullptr),
              0);
    EXPECT_GE(H5S_select_subtract(RawSpace(a), RawSpace(b)), 0);
    EXPECT_EQ(H5Sget_select_npoints(a), 30 - 6);
  }

  // 4. Point selections in either operand return H5E_UNSUPPORTED (< 0) when
  //    neither operand short-circuits via NONE or subtract_space == ALL.
  {
    ScopedSpace hslab(H5Screate_simple(2, kDims.data(), nullptr));
    ScopedSpace pts(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(hslab.valid());
    ASSERT_TRUE(pts.valid());
    constexpr std::array<hsize_t, 2> kStart = {0, 0};
    constexpr std::array<hsize_t, 2> kCount = {3, 3};
    ASSERT_GE(H5Sselect_hyperslab(hslab, H5S_SELECT_SET, kStart.data(), nullptr,
                                  kCount.data(), nullptr),
              0);
    constexpr std::array<hsize_t, 2> kPt = {1, 1};
    ASSERT_GE(H5Sselect_elements(pts, H5S_SELECT_SET, 1, kPt.data()), 0);

    ScopedErrorSilence silence;
    EXPECT_LT(H5S_select_subtract(RawSpace(hslab), RawSpace(pts)), 0);
    EXPECT_LT(H5S_select_subtract(RawSpace(pts), RawSpace(hslab)), 0);
  }
}

TEST(H5SselectTest, SelectionIteratorPublicAndInternalLifecycleAndContig) {
  constexpr std::array<hsize_t, 2> kDims = {6, 8};
  ScopedSpace space(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  // 1. Sorted point iterator via public H5Ssel_iter_* APIs.
  constexpr std::array<hsize_t, 6> kPts = {
      3, 4,  // linear 28
      0, 2,  // linear 2
      1, 1,  // linear 9
  };
  ASSERT_GE(H5Sselect_elements(space, H5S_SELECT_SET, 3, kPts.data()), 0);

  hid_t iter_id = H5Ssel_iter_create(
      space, sizeof(uint32_t),
      H5S_SEL_ITER_GET_SEQ_LIST_SORTED | H5S_SEL_ITER_SHARE_WITH_DATASPACE);
  ASSERT_NE(iter_id, H5I_INVALID_HID);
  absl::Cleanup close_iter = [&] {
    if (iter_id != H5I_INVALID_HID) {
      H5Ssel_iter_close(iter_id);
    }
  };

  size_t nseq = 0;
  size_t nelmts = 0;
  std::array<hsize_t, 4> off{};
  std::array<size_t, 4> len{};

  // Zero maxseq / maxelmts returns 0 sequences and 0 elements cleanly.
  ASSERT_GE(H5Ssel_iter_get_seq_list(iter_id, 0, 4, &nseq, &nelmts, off.data(),
                                     len.data()),
            0);
  EXPECT_EQ(nseq, 0);
  EXPECT_EQ(nelmts, 0);
  ASSERT_GE(H5Ssel_iter_get_seq_list(iter_id, 4, 0, &nseq, &nelmts, off.data(),
                                     len.data()),
            0);
  EXPECT_EQ(nseq, 0);
  EXPECT_EQ(nelmts, 0);

  // With H5S_SEL_ITER_GET_SEQ_LIST_SORTED on unsorted points {(3,4)=28,
  // (0,2)=2, (1,1)=9}, the first batch breaks before the offset decreases
  // (returning 28*4=112), and the second batch returns the remaining monotonic
  // offsets (2*4=8, 9*4=36).
  ASSERT_GE(H5Ssel_iter_get_seq_list(iter_id, 4, 10, &nseq, &nelmts, off.data(),
                                     len.data()),
            0);
  EXPECT_EQ(nseq, 1);
  EXPECT_EQ(nelmts, 1);
  EXPECT_EQ(off[0], 112);

  ASSERT_GE(H5Ssel_iter_get_seq_list(iter_id, 4, 10, &nseq, &nelmts, off.data(),
                                     len.data()),
            0);
  EXPECT_EQ(nseq, 2);
  EXPECT_EQ(nelmts, 2);
  EXPECT_EQ(off[0], 8);
  EXPECT_EQ(off[1], 36);

  // Exhausted iterator returns 0 without advancing.
  ASSERT_GE(H5Ssel_iter_get_seq_list(iter_id, 4, 10, &nseq, &nelmts, off.data(),
                                     len.data()),
            0);
  EXPECT_EQ(nseq, 0);
  EXPECT_EQ(nelmts, 0);

  // Reset iterator onto a contiguous hyperslab and re-read sequences.
  constexpr std::array<hsize_t, 2> kHypStart = {1, 0};
  constexpr std::array<hsize_t, 2> kHypCount = {2, 8};
  ASSERT_GE(H5Sselect_hyperslab(space, H5S_SELECT_SET, kHypStart.data(),
                                nullptr, kHypCount.data(), nullptr),
            0);
  ASSERT_GE(H5Ssel_iter_reset(iter_id, space), 0);
  ASSERT_GE(H5Ssel_iter_get_seq_list(iter_id, 4, 32, &nseq, &nelmts, off.data(),
                                     len.data()),
            0);
  EXPECT_EQ(nseq, 1);
  EXPECT_EQ(nelmts, 16);
  EXPECT_EQ(off[0], 8 * sizeof(uint32_t));
  EXPECT_EQ(len[0], 16 * sizeof(uint32_t));

  ASSERT_GE(H5Ssel_iter_close(iter_id), 0);
  iter_id = H5I_INVALID_HID;

  // 2. Internal iterator helpers: init, coords, nelmts, next, get_seq_list,
  //    release on scalar and simple spaces, plus H5S_select_contig_block.
  {
    H5S_sel_iter_t raw_iter{};
    bool raw_init = false;
    absl::Cleanup release_raw = [&] {
      if (raw_init) {
        H5S_select_iter_release(&raw_iter);
      }
    };
    ASSERT_GE(
        H5S_select_iter_init(&raw_iter, RawSpace(space), sizeof(uint32_t), 0),
        0);
    raw_init = true;
    EXPECT_EQ(H5S_select_iter_nelmts(&raw_iter), 16);
    std::array<hsize_t, 2> coords{};
    ASSERT_GE(H5S_select_iter_coords(&raw_iter, coords.data()), 0);
    EXPECT_THAT(coords, ElementsAre(1, 0));
    ASSERT_GE(H5S_select_iter_next(&raw_iter, 3), 0);
    EXPECT_EQ(H5S_select_iter_nelmts(&raw_iter), 13);
    ASSERT_GE(H5S_select_iter_coords(&raw_iter, coords.data()), 0);
    EXPECT_THAT(coords, ElementsAre(1, 3));
    ASSERT_GE(H5S_select_iter_get_seq_list(&raw_iter, 4, 13, &nseq, &nelmts,
                                           off.data(), len.data()),
              0);
    EXPECT_EQ(nelmts, 13);

    ASSERT_GE(H5S_select_iter_release(&raw_iter), 0);
    raw_init = false;

    // Scalar space (rank == 0) skips dims/sel_off copy cleanly.
    ScopedSpace scalar(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(scalar.valid());
    ASSERT_GE(
        H5S_select_iter_init(&raw_iter, RawSpace(scalar), sizeof(uint32_t), 0),
        0);
    raw_init = true;
    EXPECT_EQ(H5S_select_iter_nelmts(&raw_iter), 1);
  }

  // H5S_select_contig_block: contiguous hyperslab vs strided hyperslab.
  {
    bool is_contig = false;
    hsize_t contig_off = 999;
    size_t contig_len = 999;
    EXPECT_GE(H5S_select_contig_block(RawSpace(space), &is_contig, &contig_off,
                                      &contig_len),
              0);
    EXPECT_TRUE(is_contig);
    EXPECT_EQ(contig_off, 8);
    EXPECT_EQ(contig_len, 16);

    // NULL outputs on contiguous selection are valid.
    EXPECT_GE(
        H5S_select_contig_block(RawSpace(space), nullptr, nullptr, nullptr), 0);

    // Strided hyperslab is non-contiguous.
    constexpr std::array<hsize_t, 2> kStride = {2, 2};
    constexpr std::array<hsize_t, 2> kCount = {2, 2};
    ASSERT_GE(H5Sselect_hyperslab(space, H5S_SELECT_SET, kHypStart.data(),
                                  kStride.data(), kCount.data(), nullptr),
              0);
    is_contig = true;
    EXPECT_GE(H5S_select_contig_block(RawSpace(space), &is_contig, &contig_off,
                                      &contig_len),
              0);
    EXPECT_FALSE(is_contig);
  }

  // 3. Negative checks for H5Ssel_iter_* public APIs.
  ScopedErrorSilence silence;
  EXPECT_EQ(H5Ssel_iter_create(H5I_INVALID_HID, 4, 0), H5I_INVALID_HID);
  EXPECT_EQ(H5Ssel_iter_create(space, 0, 0), H5I_INVALID_HID);
  EXPECT_EQ(H5Ssel_iter_create(space, 4, 0x8000u), H5I_INVALID_HID);

  hid_t tmp_iter = H5Ssel_iter_create(space, 4, 0);
  ASSERT_NE(tmp_iter, H5I_INVALID_HID);
  absl::Cleanup close_tmp = [&] { H5Ssel_iter_close(tmp_iter); };
  EXPECT_LT(H5Ssel_iter_get_seq_list(H5I_INVALID_HID, 4, 4, &nseq, &nelmts,
                                     off.data(), len.data()),
            0);
  EXPECT_LT(H5Ssel_iter_get_seq_list(tmp_iter, 4, 4, nullptr, &nelmts,
                                     off.data(), len.data()),
            0);
  EXPECT_LT(H5Ssel_iter_get_seq_list(tmp_iter, 4, 4, &nseq, nullptr, off.data(),
                                     len.data()),
            0);
  EXPECT_LT(H5Ssel_iter_get_seq_list(tmp_iter, 4, 4, &nseq, &nelmts, nullptr,
                                     len.data()),
            0);
  EXPECT_LT(H5Ssel_iter_get_seq_list(tmp_iter, 4, 4, &nseq, &nelmts, off.data(),
                                     nullptr),
            0);
  EXPECT_LT(H5Ssel_iter_reset(H5I_INVALID_HID, space), 0);
  EXPECT_LT(H5Ssel_iter_reset(tmp_iter, H5I_INVALID_HID), 0);
  EXPECT_LT(H5Ssel_iter_close(H5I_INVALID_HID), 0);
}

TEST(H5SselectTest, DirectProjectScalarAndSimpleAcrossAllSelectionTypes) {
  constexpr std::array<hsize_t, 2> kDims1x1 = {1, 1};
  constexpr std::array<hsize_t, 2> kDims2D = {4, 5};
  constexpr std::array<hsize_t, 1> kDims1D = {5};
  constexpr std::array<hsize_t, 3> kDims3D = {1, 4, 5};

  // 1. H5S_select_project_scalar across ALL, POINTS, HYPERSLABS, and NONE.
  {
    ScopedSpace all_1x1(H5Screate_simple(2, kDims1x1.data(), nullptr));
    ScopedSpace space(H5Screate_simple(2, kDims2D.data(), nullptr));
    ASSERT_TRUE(all_1x1.valid());
    ASSERT_TRUE(space.valid());

    hsize_t offset = 999;
    EXPECT_GE(H5S_select_project_scalar(RawSpace(all_1x1), &offset), 0);
    EXPECT_EQ(offset, 0);

    // 1-point selection projects to its linearized offset.
    constexpr std::array<hsize_t, 2> kPt = {2, 3};
    ASSERT_GE(H5Sselect_elements(space, H5S_SELECT_SET, 1, kPt.data()), 0);
    EXPECT_GE(H5S_select_project_scalar(RawSpace(space), &offset), 0);
    EXPECT_EQ(offset, 2 * 5 + 3);

    // 1x1 hyperslab projects to its linearized offset.
    constexpr std::array<hsize_t, 2> kStart = {1, 4};
    constexpr std::array<hsize_t, 2> kOne = {1, 1};
    ASSERT_GE(H5Sselect_hyperslab(space, H5S_SELECT_SET, kStart.data(), nullptr,
                                  kOne.data(), nullptr),
              0);
    EXPECT_GE(H5S_select_project_scalar(RawSpace(space), &offset), 0);
    EXPECT_EQ(offset, 1 * 5 + 4);

    // NONE selection fails scalar projection cleanly.
    ScopedErrorSilence silence;
    ASSERT_GE(H5Sselect_none(space), 0);
    EXPECT_LT(H5S_select_project_scalar(RawSpace(space), &offset), 0);
  }

  // 2. H5S_select_project_simple across ALL, NONE, POINTS, and HYPERSLABS.
  {
    ScopedSpace src(H5Screate_simple(2, kDims2D.data(), nullptr));
    ScopedSpace dst_up(H5Screate_simple(3, kDims3D.data(), nullptr));
    ScopedSpace dst_down(H5Screate_simple(1, kDims1D.data(), nullptr));
    ASSERT_TRUE(src.valid());
    ASSERT_TRUE(dst_up.valid());
    ASSERT_TRUE(dst_down.valid());

    hsize_t offset = 0;

    // ALL selection projected up.
    EXPECT_GE(
        H5S_select_project_simple(RawSpace(src), RawSpace(dst_up), &offset), 0);
    EXPECT_EQ(H5Sget_select_type(dst_up), H5S_SEL_ALL);

    // NONE selection projected up.
    ASSERT_GE(H5Sselect_none(src), 0);
    EXPECT_GE(
        H5S_select_project_simple(RawSpace(src), RawSpace(dst_up), &offset), 0);
    EXPECT_EQ(H5Sget_select_type(dst_up), H5S_SEL_NONE);

    // POINTS selection projected up and down.
    constexpr std::array<hsize_t, 4> kPts = {2, 1, 2, 4};
    ASSERT_GE(H5Sselect_elements(src, H5S_SELECT_SET, 2, kPts.data()), 0);
    EXPECT_GE(
        H5S_select_project_simple(RawSpace(src), RawSpace(dst_up), &offset), 0);
    EXPECT_EQ(H5Sget_select_npoints(dst_up), 2);
    EXPECT_GE(
        H5S_select_project_simple(RawSpace(src), RawSpace(dst_down), &offset),
        0);
    EXPECT_EQ(H5Sget_select_npoints(dst_down), 2);
    EXPECT_EQ(offset, 2 * 5);

    // HYPERSLAB selection projected down.
    constexpr std::array<hsize_t, 2> kStart = {3, 1};
    constexpr std::array<hsize_t, 2> kCount = {1, 3};
    ASSERT_GE(H5Sselect_hyperslab(src, H5S_SELECT_SET, kStart.data(), nullptr,
                                  kCount.data(), nullptr),
              0);
    EXPECT_GE(
        H5S_select_project_simple(RawSpace(src), RawSpace(dst_down), &offset),
        0);
    EXPECT_EQ(H5Sget_select_npoints(dst_down), 3);
  }
}

TEST(H5SselectTest, ShapeSameNonFlatLeadingDimAndIntersectSubtractEdgeErrors) {
  // 1. ShapeSame: higher-rank space_a matches trailing dimension range
  //    (1 - 0 == 1) and total npoints (2 == 2), but leading extra dimension
  //    in space_a is non-flat (low_a[0]=0 != high_a[0]=1) -> returns false.
  {
    constexpr std::array<hsize_t, 2> kDims2D = {4, 4};
    constexpr std::array<hsize_t, 1> kDims1D = {2};
    ScopedSpace pts2d(H5Screate_simple(2, kDims2D.data(), nullptr));
    ScopedSpace all1d(H5Screate_simple(1, kDims1D.data(), nullptr));
    ASSERT_TRUE(pts2d.valid());
    ASSERT_TRUE(all1d.valid());
    constexpr std::array<hsize_t, 4> kPts = {0, 0, 1, 1};
    ASSERT_GE(H5Sselect_elements(pts2d, H5S_SELECT_SET, 2, kPts.data()), 0);
    EXPECT_EQ(H5Sselect_shape_same(pts2d, all1d), 0);
  }

  constexpr std::array<hsize_t, 2> kDims = {8, 8};

  // 2. Project intersection with negative-offset hyperslab sis fails
  //    H5S_SELECT_INTERSECT_BLOCK in both scalar-dst and points-loop paths.
  {
    ScopedSpace src(H5Screate_simple(2, kDims.data(), nullptr));
    ScopedSpace scalar_dst(H5Screate(H5S_SCALAR));
    ScopedSpace pts_dst(H5Screate_simple(2, kDims.data(), nullptr));
    ScopedSpace sis(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(src.valid());
    ASSERT_TRUE(scalar_dst.valid());
    ASSERT_TRUE(pts_dst.valid());
    ASSERT_TRUE(sis.valid());

    constexpr std::array<hsize_t, 2> kOnePt = {3, 4};
    ASSERT_GE(H5Sselect_elements(src, H5S_SELECT_SET, 1, kOnePt.data()), 0);
    constexpr std::array<hsize_t, 2> kSisStart = {2, 2};
    constexpr std::array<hsize_t, 2> kSisCount = {3, 3};
    ASSERT_GE(H5Sselect_hyperslab(sis, H5S_SELECT_SET, kSisStart.data(),
                                  nullptr, kSisCount.data(), nullptr),
              0);
    constexpr std::array<hssize_t, 2> kBadOffset = {-5, 0};
    ASSERT_GE(H5Soffset_simple(sis, kBadOffset.data()), 0);

    ScopedErrorSilence silence;
    // Scalar dst fails inside H5S_SELECT_INTERSECT_BLOCK(sis, ...).
    EXPECT_EQ(H5Sselect_project_intersection(src, scalar_dst, sis),
              H5I_INVALID_HID);

    // Simple points dst/src fail in point-loop H5S_SELECT_INTERSECT_BLOCK.
    constexpr std::array<hsize_t, 4> kTwoPts = {1, 1, 2, 2};
    ASSERT_GE(H5Sselect_elements(src, H5S_SELECT_SET, 2, kTwoPts.data()), 0);
    ASSERT_GE(H5Sselect_elements(pts_dst, H5S_SELECT_SET, 2, kTwoPts.data()),
              0);
    EXPECT_EQ(H5Sselect_project_intersection(src, pts_dst, sis),
              H5I_INVALID_HID);
  }

  // 3. Subtract fails H5S__modify_select when space is an unlimited hyperslab
  //    (no span tree can be generated). Note: passing an unlimited selection
  //    to H5S__hyper_project_intersection hits an uninitialized udata bug in
  //    upstream H5Shyper.c, so it is intentionally avoided here.
  {
    constexpr std::array<hsize_t, 2> kMaxDims = {H5S_UNLIMITED, 8};
    ScopedSpace unlim_space(H5Screate_simple(2, kDims.data(), kMaxDims.data()));
    ScopedSpace sub_space(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(unlim_space.valid());
    ASSERT_TRUE(sub_space.valid());

    constexpr std::array<hsize_t, 2> kStart = {0, 0};
    constexpr std::array<hsize_t, 2> kCount = {2, 2};
    constexpr std::array<hsize_t, 2> kUnlimCount = {H5S_UNLIMITED, 2};
    ASSERT_GE(H5Sselect_hyperslab(unlim_space, H5S_SELECT_SET, kStart.data(),
                                  nullptr, kUnlimCount.data(), nullptr),
              0);
    ASSERT_GE(H5Sselect_hyperslab(sub_space, H5S_SELECT_SET, kStart.data(),
                                  nullptr, kCount.data(), nullptr),
              0);

    ScopedErrorSilence silence;
    EXPECT_LT(H5S_select_subtract(RawSpace(unlim_space), RawSpace(sub_space)),
              0);
  }
}

herr_t StubFailCopy(H5S_t* /*dst*/, const H5S_t* /*src*/, bool /*share*/) {
  return FAIL;
}

herr_t StubFailRelease(H5S_t* /*space*/) { return FAIL; }

herr_t StubFailAdjustS(H5S_t* /*space*/, const hssize_t* /*offset*/) {
  return FAIL;
}

htri_t StubFailIntersectBlock(H5S_t* /*space*/, const hsize_t* /*start*/,
                              const hsize_t* /*end*/) {
  return FAIL;
}

herr_t StubFailProjectScalar(const H5S_t* /*space*/, hsize_t* /*offset*/) {
  return FAIL;
}

herr_t StubFailProjectSimple(const H5S_t* /*space*/, H5S_t* /*new_space*/,
                             hsize_t* /*offset*/) {
  return FAIL;
}

// Per-hook state for scoped selection iterator callback failure injection.
struct IterHookState {
  const H5S_select_class_t* base_sel_cls = nullptr;
  const H5S_sel_iter_class_t* base_iter_cls = nullptr;
  H5S_sel_iter_class_t override_iter_cls{};
  bool fail_iter_init = false;
  bool fail_coords = false;
  bool fail_block = false;
  bool fail_has_next_block = false;
  bool fail_next = false;
  bool fail_next_block = false;
  bool fail_get_seq_list = false;
  bool fail_release = false;
};

IterHookState g_hook_a;
IterHookState g_hook_b;

herr_t HookIterCoords(IterHookState* st, const H5S_sel_iter_t* iter,
                      hsize_t* coords) {
  if (st->fail_coords) {
    return FAIL;
  }
  return (*st->base_iter_cls->iter_coords)(iter, coords);
}

herr_t HookIterBlock(IterHookState* st, const H5S_sel_iter_t* iter,
                     hsize_t* start, hsize_t* end) {
  if (st->fail_block) {
    return FAIL;
  }
  return (*st->base_iter_cls->iter_block)(iter, start, end);
}

hsize_t HookIterNelmts(IterHookState* st, const H5S_sel_iter_t* iter) {
  return (*st->base_iter_cls->iter_nelmts)(iter);
}

htri_t HookIterHasNextBlock(IterHookState* st, const H5S_sel_iter_t* iter) {
  if (st->fail_has_next_block) {
    return FAIL;
  }
  return (*st->base_iter_cls->iter_has_next_block)(iter);
}

herr_t HookIterNext(IterHookState* st, H5S_sel_iter_t* iter, size_t nelem) {
  if (st->fail_next) {
    return FAIL;
  }
  return (*st->base_iter_cls->iter_next)(iter, nelem);
}

herr_t HookIterNextBlock(IterHookState* st, H5S_sel_iter_t* iter) {
  if (st->fail_next_block) {
    return FAIL;
  }
  return (*st->base_iter_cls->iter_next_block)(iter);
}

herr_t HookIterGetSeqList(IterHookState* st, H5S_sel_iter_t* iter,
                          size_t maxseq, size_t maxbytes, size_t* nseq,
                          size_t* nbytes, hsize_t* off, size_t* len) {
  if (st->fail_get_seq_list) {
    return FAIL;
  }
  return (*st->base_iter_cls->iter_get_seq_list)(iter, maxseq, maxbytes, nseq,
                                                 nbytes, off, len);
}

herr_t HookIterRelease(IterHookState* st, H5S_sel_iter_t* iter) {
  herr_t rc = SUCCEED;
  if (st->base_iter_cls != nullptr &&
      st->base_iter_cls->iter_release != nullptr) {
    rc = (*st->base_iter_cls->iter_release)(iter);
  }
  if (st->fail_release) {
    return FAIL;
  }
  return rc;
}

herr_t HookIterInitCommon(IterHookState* st, H5S_t* space,
                          H5S_sel_iter_t* sel_iter,
                          H5S_sel_iter_class_t* ops_table) {
  herr_t rc = (*st->base_sel_cls->iter_init)(space, sel_iter);
  if (rc < 0) {
    return rc;
  }
  st->base_iter_cls = sel_iter->type;
  if (st->fail_iter_init) {
    if (sel_iter->type != nullptr && sel_iter->type->iter_release != nullptr) {
      (*sel_iter->type->iter_release)(sel_iter);
    }
    return FAIL;
  }
  ops_table->type = st->base_iter_cls->type;
  sel_iter->type = ops_table;
  return SUCCEED;
}

herr_t HookIterCoordsA(const H5S_sel_iter_t* it, hsize_t* c) {
  return HookIterCoords(&g_hook_a, it, c);
}
herr_t HookIterBlockA(const H5S_sel_iter_t* it, hsize_t* s, hsize_t* e) {
  return HookIterBlock(&g_hook_a, it, s, e);
}
hsize_t HookIterNelmtsA(const H5S_sel_iter_t* it) {
  return HookIterNelmts(&g_hook_a, it);
}
htri_t HookIterHasNextBlockA(const H5S_sel_iter_t* it) {
  return HookIterHasNextBlock(&g_hook_a, it);
}
herr_t HookIterNextA(H5S_sel_iter_t* it, size_t n) {
  return HookIterNext(&g_hook_a, it, n);
}
herr_t HookIterNextBlockA(H5S_sel_iter_t* it) {
  return HookIterNextBlock(&g_hook_a, it);
}
herr_t HookIterGetSeqListA(H5S_sel_iter_t* it, size_t ms, size_t mb, size_t* ns,
                           size_t* nb, hsize_t* o, size_t* l) {
  return HookIterGetSeqList(&g_hook_a, it, ms, mb, ns, nb, o, l);
}
herr_t HookIterReleaseA(H5S_sel_iter_t* it) {
  return HookIterRelease(&g_hook_a, it);
}

herr_t HookIterCoordsB(const H5S_sel_iter_t* it, hsize_t* c) {
  return HookIterCoords(&g_hook_b, it, c);
}
herr_t HookIterBlockB(const H5S_sel_iter_t* it, hsize_t* s, hsize_t* e) {
  return HookIterBlock(&g_hook_b, it, s, e);
}
hsize_t HookIterNelmtsB(const H5S_sel_iter_t* it) {
  return HookIterNelmts(&g_hook_b, it);
}
htri_t HookIterHasNextBlockB(const H5S_sel_iter_t* it) {
  return HookIterHasNextBlock(&g_hook_b, it);
}
herr_t HookIterNextB(H5S_sel_iter_t* it, size_t n) {
  return HookIterNext(&g_hook_b, it, n);
}
herr_t HookIterNextBlockB(H5S_sel_iter_t* it) {
  return HookIterNextBlock(&g_hook_b, it);
}
herr_t HookIterGetSeqListB(H5S_sel_iter_t* it, size_t ms, size_t mb, size_t* ns,
                           size_t* nb, hsize_t* o, size_t* l) {
  return HookIterGetSeqList(&g_hook_b, it, ms, mb, ns, nb, o, l);
}
herr_t HookIterReleaseB(H5S_sel_iter_t* it) {
  return HookIterRelease(&g_hook_b, it);
}

herr_t HookIterInitA(H5S_t* space, H5S_sel_iter_t* sel_iter) {
  g_hook_a.override_iter_cls = {
      H5S_SEL_NONE,       HookIterCoordsA,       HookIterBlockA,
      HookIterNelmtsA,    HookIterHasNextBlockA, HookIterNextA,
      HookIterNextBlockA, HookIterGetSeqListA,   HookIterReleaseA,
  };
  return HookIterInitCommon(&g_hook_a, space, sel_iter,
                            &g_hook_a.override_iter_cls);
}

herr_t HookIterInitB(H5S_t* space, H5S_sel_iter_t* sel_iter) {
  g_hook_b.override_iter_cls = {
      H5S_SEL_NONE,       HookIterCoordsB,       HookIterBlockB,
      HookIterNelmtsB,    HookIterHasNextBlockB, HookIterNextB,
      HookIterNextBlockB, HookIterGetSeqListB,   HookIterReleaseB,
  };
  return HookIterInitCommon(&g_hook_b, space, sel_iter,
                            &g_hook_b.override_iter_cls);
}

TEST(H5SselectTest, SelectionClassCallbackErrorPropagation) {
  constexpr std::array<hsize_t, 2> kDims = {8, 8};
  ScopedErrorSilence silence;

  // 1. Failing select.type->copy in H5Sselect_copy and
  //    H5S_select_project_intersection (ALL intersect space path).
  {
    ScopedSpace src(H5Screate_simple(2, kDims.data(), nullptr));
    ScopedSpace dst(H5Screate_simple(2, kDims.data(), nullptr));
    ScopedSpace sis(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(src.valid());
    ASSERT_TRUE(dst.valid());
    ASSERT_TRUE(sis.valid());

    H5S_t* raw_src = RawSpace(src);
    ASSERT_NE(raw_src, nullptr);
    const H5S_select_class_t* const orig_type = raw_src->select.type;
    H5S_select_class_t fail_copy_cls = *orig_type;
    fail_copy_cls.copy = StubFailCopy;
    absl::Cleanup restore = [&] { raw_src->select.type = orig_type; };
    raw_src->select.type = &fail_copy_cls;

    EXPECT_LT(H5Sselect_copy(dst, src), 0);
    H5S_t* proj = nullptr;
    EXPECT_LT(H5S_select_project_intersection(RawSpace(dst), raw_src,
                                              RawSpace(sis), &proj, false),
              0);
  }

  // 2. Failing select.type->release during H5S_select_copy (dst release fails
  //    while tmp_space release succeeds) and H5S_select_release.
  //    Note: failing tmp_space's release in H5S_select_copy triggers an
  //    upstream infinite loop (HGOTO_ERROR -> goto done inside done:).
  {
    ScopedSpace src(H5Screate_simple(2, kDims.data(), nullptr));
    ScopedSpace dst(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(src.valid());
    ASSERT_TRUE(dst.valid());

    H5S_t* raw_dst = RawSpace(dst);
    ASSERT_NE(raw_dst, nullptr);

    const H5S_select_class_t* const orig_dst = raw_dst->select.type;
    H5S_select_class_t fail_rel_cls = H5S_sel_all[0];
    fail_rel_cls.release = StubFailRelease;
    absl::Cleanup restore = [&] { raw_dst->select.type = orig_dst; };

    raw_dst->select.type = &fail_rel_cls;
    EXPECT_LT(H5S_select_release(raw_dst), 0);
    EXPECT_LT(H5Sselect_copy(dst, src), 0);
  }

  // 3. Failing select.type->adjust_s and intersect_block.
  {
    ScopedSpace space(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(space.valid());
    H5S_t* raw = RawSpace(space);
    ASSERT_NE(raw, nullptr);
    const H5S_select_class_t* const orig_type = raw->select.type;
    H5S_select_class_t custom = H5S_sel_all[0];
    custom.adjust_s = StubFailAdjustS;
    custom.intersect_block = StubFailIntersectBlock;
    absl::Cleanup restore = [&] { raw->select.type = orig_type; };
    raw->select.type = &custom;

    constexpr std::array<hssize_t, 2> kZeroOff = {0, 0};
    EXPECT_LT(H5Sselect_adjust(space, kZeroOff.data()), 0);

    constexpr std::array<hsize_t, 2> kStart = {1, 1};
    constexpr std::array<hsize_t, 2> kEnd = {2, 2};
    EXPECT_LT(H5Sselect_intersect_block(space, kStart.data(), kEnd.data()), 0);
  }

  // 4. Failing select.type->project_scalar and project_simple in
  //    H5S_select_construct_projection (triggers new_space cleanup via
  //    H5S_close).
  {
    constexpr std::array<hsize_t, 1> kDims1D = {4};
    ScopedSpace one_pt(H5Screate_simple(1, kDims1D.data(), nullptr));
    ScopedSpace all_1d(H5Screate_simple(1, kDims1D.data(), nullptr));
    ASSERT_TRUE(one_pt.valid());
    ASSERT_TRUE(all_1d.valid());
    constexpr std::array<hsize_t, 1> kPt = {1};
    ASSERT_GE(H5Sselect_elements(one_pt, H5S_SELECT_SET, 1, kPt.data()), 0);

    H5S_t* raw_pt = RawSpace(one_pt);
    H5S_t* raw_all = RawSpace(all_1d);
    const H5S_select_class_t* const orig_pt = raw_pt->select.type;
    const H5S_select_class_t* const orig_all = raw_all->select.type;
    H5S_select_class_t fail_pt_cls = *orig_pt;
    fail_pt_cls.project_scalar = StubFailProjectScalar;
    H5S_select_class_t fail_all_cls = *orig_all;
    fail_all_cls.project_simple = StubFailProjectSimple;
    absl::Cleanup restore = [&] {
      raw_pt->select.type = orig_pt;
      raw_all->select.type = orig_all;
    };
    raw_pt->select.type = &fail_pt_cls;
    raw_all->select.type = &fail_all_cls;

    H5S_t* proj = nullptr;
    ptrdiff_t buf_adj = 0;
    EXPECT_LT(H5S_select_construct_projection(raw_pt, &proj, 0, 4, &buf_adj),
              0);
    EXPECT_EQ(proj, nullptr);
    EXPECT_LT(H5S_select_construct_projection(raw_all, &proj, 2, 4, &buf_adj),
              0);
    EXPECT_EQ(proj, nullptr);
  }
}

TEST(H5SselectTest, IteratorCallbackErrorPropagation) {
  constexpr std::array<hsize_t, 2> kDims = {6, 6};
  ScopedErrorSilence silence;

  // 1. Mixed-type H5S_select_shape_same iterator callback failures.
  {
    ScopedSpace hyp(H5Screate_simple(2, kDims.data(), nullptr));
    ScopedSpace pts(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(hyp.valid());
    ASSERT_TRUE(pts.valid());
    constexpr std::array<hsize_t, 2> kStart = {0, 0};
    constexpr std::array<hsize_t, 2> kStride = {1, 2};
    constexpr std::array<hsize_t, 2> kCount = {1, 2};
    ASSERT_GE(H5Sselect_hyperslab(hyp, H5S_SELECT_SET, kStart.data(),
                                  kStride.data(), kCount.data(), nullptr),
              0);
    constexpr std::array<hsize_t, 4> kPts = {0, 0, 0, 2};
    ASSERT_GE(H5Sselect_elements(pts, H5S_SELECT_SET, 2, kPts.data()), 0);

    H5S_t* raw_hyp = RawSpace(hyp);
    H5S_t* raw_pts = RawSpace(pts);
    const H5S_select_class_t* const orig_hyp = raw_hyp->select.type;
    const H5S_select_class_t* const orig_pts = raw_pts->select.type;

    H5S_select_class_t cls_a = *orig_hyp;
    H5S_select_class_t cls_b = *orig_pts;
    cls_a.iter_init = HookIterInitA;
    cls_b.iter_init = HookIterInitB;

    g_hook_a = IterHookState{.base_sel_cls = orig_hyp};
    g_hook_b = IterHookState{.base_sel_cls = orig_pts};
    absl::Cleanup restore = [&] {
      raw_hyp->select.type = orig_hyp;
      raw_pts->select.type = orig_pts;
      g_hook_a = IterHookState{};
      g_hook_b = IterHookState{};
    };
    raw_hyp->select.type = &cls_a;
    raw_pts->select.type = &cls_b;

    auto reset_flags = [] {
      g_hook_a.fail_iter_init = false;
      g_hook_a.fail_block = false;
      g_hook_a.fail_has_next_block = false;
      g_hook_a.fail_next_block = false;
      g_hook_a.fail_release = false;
      g_hook_b.fail_iter_init = false;
      g_hook_b.fail_block = false;
      g_hook_b.fail_has_next_block = false;
      g_hook_b.fail_next_block = false;
      g_hook_b.fail_release = false;
    };

    reset_flags();
    g_hook_a.fail_iter_init = true;
    EXPECT_LT(H5Sselect_shape_same(hyp, pts), 0);

    reset_flags();
    g_hook_b.fail_iter_init = true;
    EXPECT_LT(H5Sselect_shape_same(hyp, pts), 0);

    reset_flags();
    g_hook_a.fail_block = true;
    EXPECT_LT(H5Sselect_shape_same(hyp, pts), 0);

    reset_flags();
    g_hook_b.fail_block = true;
    EXPECT_LT(H5Sselect_shape_same(hyp, pts), 0);

    reset_flags();
    g_hook_a.fail_has_next_block = true;
    EXPECT_LT(H5Sselect_shape_same(hyp, pts), 0);

    reset_flags();
    g_hook_b.fail_has_next_block = true;
    EXPECT_LT(H5Sselect_shape_same(hyp, pts), 0);

    reset_flags();
    g_hook_a.fail_next_block = true;
    EXPECT_LT(H5Sselect_shape_same(hyp, pts), 0);

    reset_flags();
    g_hook_b.fail_next_block = true;
    EXPECT_LT(H5Sselect_shape_same(hyp, pts), 0);

    // Failing iter_release during cleanup exercises both HDONE_ERROR branches
    // for iter_a and iter_b while still delegating the underlying release.
    reset_flags();
    g_hook_a.fail_release = true;
    g_hook_b.fail_release = true;
    EXPECT_LT(H5Sselect_shape_same(hyp, pts), 0);
  }

  // 2. H5S_select_project_intersection point-loop iterator callback failures.
  {
    ScopedSpace src(H5Screate_simple(2, kDims.data(), nullptr));
    ScopedSpace dst(H5Screate_simple(2, kDims.data(), nullptr));
    ScopedSpace sis(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(src.valid());
    ASSERT_TRUE(dst.valid());
    ASSERT_TRUE(sis.valid());

    constexpr std::array<hsize_t, 4> kSrcPts = {0, 0, 1, 1};
    constexpr std::array<hsize_t, 4> kDstPts = {2, 2, 3, 3};
    ASSERT_GE(H5Sselect_elements(src, H5S_SELECT_SET, 2, kSrcPts.data()), 0);
    ASSERT_GE(H5Sselect_elements(dst, H5S_SELECT_SET, 2, kDstPts.data()), 0);
    constexpr std::array<hsize_t, 2> kSisStart = {0, 0};
    constexpr std::array<hsize_t, 2> kSisCount = {4, 4};
    ASSERT_GE(H5Sselect_hyperslab(sis, H5S_SELECT_SET, kSisStart.data(),
                                  nullptr, kSisCount.data(), nullptr),
              0);

    H5S_t* raw_src = RawSpace(src);
    H5S_t* raw_dst = RawSpace(dst);
    const H5S_select_class_t* const orig_src = raw_src->select.type;
    const H5S_select_class_t* const orig_dst = raw_dst->select.type;

    H5S_select_class_t cls_a = *orig_src;
    H5S_select_class_t cls_b = *orig_dst;
    cls_a.iter_init = HookIterInitA;
    cls_b.iter_init = HookIterInitB;

    g_hook_a = IterHookState{.base_sel_cls = orig_src};
    g_hook_b = IterHookState{.base_sel_cls = orig_dst};
    absl::Cleanup restore = [&] {
      raw_src->select.type = orig_src;
      raw_dst->select.type = orig_dst;
      g_hook_a = IterHookState{};
      g_hook_b = IterHookState{};
    };
    raw_src->select.type = &cls_a;
    raw_dst->select.type = &cls_b;

    auto reset_flags = [] {
      g_hook_a.fail_iter_init = false;
      g_hook_a.fail_coords = false;
      g_hook_a.fail_next = false;
      g_hook_a.fail_release = false;
      g_hook_b.fail_iter_init = false;
      g_hook_b.fail_coords = false;
      g_hook_b.fail_next = false;
      g_hook_b.fail_release = false;
    };

    reset_flags();
    g_hook_a.fail_iter_init = true;
    EXPECT_EQ(H5Sselect_project_intersection(src, dst, sis), H5I_INVALID_HID);

    reset_flags();
    g_hook_b.fail_iter_init = true;
    EXPECT_EQ(H5Sselect_project_intersection(src, dst, sis), H5I_INVALID_HID);

    reset_flags();
    g_hook_a.fail_coords = true;
    EXPECT_EQ(H5Sselect_project_intersection(src, dst, sis), H5I_INVALID_HID);

    reset_flags();
    g_hook_b.fail_coords = true;
    EXPECT_EQ(H5Sselect_project_intersection(src, dst, sis), H5I_INVALID_HID);

    reset_flags();
    g_hook_a.fail_next = true;
    EXPECT_EQ(H5Sselect_project_intersection(src, dst, sis), H5I_INVALID_HID);

    reset_flags();
    g_hook_b.fail_next = true;
    EXPECT_EQ(H5Sselect_project_intersection(src, dst, sis), H5I_INVALID_HID);

    reset_flags();
    g_hook_a.fail_release = true;
    g_hook_b.fail_release = true;
    EXPECT_EQ(H5Sselect_project_intersection(src, dst, sis), H5I_INVALID_HID);
  }

  // 3. Iterator failures across H5S_select_iterate, H5S_select_fill,
  //    H5S_select_contig_block, H5S_select_iter_get_seq_list, and public
  //    H5Ssel_iter_{get_seq_list,reset,close}.
  {
    ScopedSpace all_space(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(all_space.valid());
    H5S_t* raw_all = RawSpace(all_space);
    const H5S_select_class_t* const orig_all = raw_all->select.type;

    H5S_select_class_t cls_a = *orig_all;
    cls_a.iter_init = HookIterInitA;
    g_hook_a = IterHookState{.base_sel_cls = orig_all};
    absl::Cleanup restore_space = [&] {
      raw_all->select.type = orig_all;
      g_hook_a = IterHookState{};
    };

    const H5T_t* native_i32 = static_cast<const H5T_t*>(
        H5I_object_verify(H5T_NATIVE_INT32, H5I_DATATYPE));
    ASSERT_NE(native_i32, nullptr);
    std::array<int32_t, 36> buf{};
    constexpr int32_t kFill = 7;
    H5S_sel_iter_op_t lib_op{};
    lib_op.op_type = H5S_SEL_ITER_OP_LIB;
    lib_op.u.lib_op = [](void*, const H5T_t*, unsigned, const hsize_t*,
                         void*) -> herr_t { return 0; };
    bool contig = false;
    hsize_t off = 0;
    size_t len = 0;

    // 3a. Create public iterator on normal space BEFORE installing failing hook
    //     on raw_all, so we never hit H5Ssel_iter_create's unfreed error path.
    hid_t iter_id = H5Ssel_iter_create(all_space, sizeof(int32_t), 0);
    ASSERT_NE(iter_id, H5I_INVALID_HID);
    H5S_sel_iter_t* raw_iter = static_cast<H5S_sel_iter_t*>(
        H5I_object_verify(iter_id, H5I_SPACE_SEL_ITER));
    ASSERT_NE(raw_iter, nullptr);
    const H5S_sel_iter_class_t* const orig_iter_cls = raw_iter->type;
    absl::Cleanup close_iter = [&] {
      if (iter_id != H5I_INVALID_HID) {
        H5S_sel_iter_t* obj = static_cast<H5S_sel_iter_t*>(
            H5I_object_verify(iter_id, H5I_SPACE_SEL_ITER));
        if (obj != nullptr) {
          obj->type = orig_iter_cls;
        }
        H5Ssel_iter_close(iter_id);
      }
    };

    // Fail iter_release in H5Ssel_iter_reset and H5Ssel_iter_close / _cb.
    g_hook_a.base_iter_cls = orig_iter_cls;
    H5S_sel_iter_class_t custom_iter_cls = *orig_iter_cls;
    custom_iter_cls.iter_release = HookIterReleaseA;
    custom_iter_cls.iter_get_seq_list = HookIterGetSeqListA;
    raw_iter->type = &custom_iter_cls;

    g_hook_a.fail_release = true;
    EXPECT_LT(H5Ssel_iter_close(iter_id), 0);
    EXPECT_LT(H5Ssel_iter_reset(iter_id, all_space), 0);

    size_t nseq = 0;
    size_t nelmts = 0;
    g_hook_a.fail_get_seq_list = true;
    EXPECT_LT(
        H5Ssel_iter_get_seq_list(iter_id, 4, 4, &nseq, &nelmts, &off, &len), 0);
    EXPECT_LT(H5S_select_iter_get_seq_list(raw_iter, 4, 4, &nseq, &nelmts, &off,
                                           &len),
              0);

    // Restore iter->type before testing fail_iter_init via H5Ssel_iter_reset.
    raw_iter->type = orig_iter_cls;
    raw_all->select.type = &cls_a;

    // 3b. iter_init failures.
    g_hook_a.fail_iter_init = true;
    EXPECT_LT(
        H5S_select_iterate(buf.data(), native_i32, raw_all, &lib_op, nullptr),
        0);
    EXPECT_LT(H5S_select_fill(&kFill, sizeof(kFill), raw_all, buf.data()), 0);
    EXPECT_LT(H5S_select_contig_block(raw_all, &contig, &off, &len), 0);
    EXPECT_LT(H5Ssel_iter_reset(iter_id, all_space), 0);
    g_hook_a.fail_iter_init = false;

    // 3c. iter_get_seq_list failures (+ HDONE_ERROR release checks).
    g_hook_a.fail_get_seq_list = true;
    g_hook_a.fail_release = true;
    EXPECT_LT(
        H5S_select_iterate(buf.data(), native_i32, raw_all, &lib_op, nullptr),
        0);
    EXPECT_LT(H5S_select_fill(&kFill, sizeof(kFill), raw_all, buf.data()), 0);
    EXPECT_LT(H5S_select_contig_block(raw_all, &contig, &off, &len), 0);
  }
}

}  // namespace
}  // namespace hdf5
