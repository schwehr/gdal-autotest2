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
// Tests point selection operations, selection iteration, serialization/
// deserialization across libver bounds, shape comparison, block intersection,
// offset adjustment, and rank projection (third_party/hdf5/src/H5Spoint.c).
//
// Google3 build caveats:
//   - Compiles `:hdf5` with `-DNDEBUG` (internal structs omit debug fields);
//     `NDEBUG` is defined around `hdf5_internal_prelude.h` and internal headers
//     to match `:hdf5` ABI layouts.
//   - Parallel MPI (`H5_HAVE_PARALLEL`) is disabled in non-MPI Linux builds.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Spoint.c

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#ifndef NDEBUG
#define H5SPOINT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#pragma push_macro("try")
#undef try
#define try hdf5_try_param
#define H5S_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Spkg.h"
#include "third_party/hdf5/src/H5Sprivate.h"
#undef try
#pragma pop_macro("try")
}  // extern "C"
#ifdef H5SPOINT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5SPOINT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;

// RAII wrapper for HDF5 dataspace selection iterators (`H5Ssel_iter_close`).
using ScopedSelIter = ScopedH5Id<H5Ssel_iter_close>;

// RAII wrapper pushing/popping an HDF5 API context for internal H5S calls.
class ScopedApiContext {
 public:
  explicit ScopedApiContext(hid_t fapl_id = H5P_DEFAULT) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      hid_t apl = fapl_id;
      H5CX_set_apl(&apl, H5P_CLS_FACC, H5I_INVALID_HID, true);
    }
  }

  ~ScopedApiContext() {
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

  bool valid() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

H5S_t* VerifyDataspacePtr(hid_t space_id) {
  return static_cast<H5S_t*>(H5I_object_verify(space_id, H5I_DATASPACE));
}

void AppendUint16LE(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xffU));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xffU));
}

void AppendUint32LE(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xffU));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xffU));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xffU));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xffU));
}

void AppendUint64LE(std::vector<uint8_t>& out, uint64_t v) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<uint8_t>((v >> shift) & 0xffULL));
  }
}

TEST(H5SpointTest, SelectElementsSetAppendPrependAndPointlistCaching) {
  constexpr std::array<hsize_t, 2> kDims = {10, 12};
  ScopedSpace space(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  // 1. APPEND onto an initial non-point (H5S_SEL_ALL) selection allocates a
  //    fresh point list with empty head/tail and inserts at tail.
  constexpr std::array<hsize_t, 2> kFirst = {4, 5};
  ASSERT_GE(H5Sselect_elements(space, H5S_SELECT_APPEND, 1, kFirst.data()), 0);
  EXPECT_EQ(H5Sget_select_type(space), H5S_SEL_POINTS);
  EXPECT_EQ(H5Sget_select_elem_npoints(space), 1);

  // 2. APPEND onto an existing point selection links after tail.
  constexpr std::array<hsize_t, 4> kMore = {6, 7, 2, 3};
  ASSERT_GE(H5Sselect_elements(space, H5S_SELECT_APPEND, 2, kMore.data()), 0);
  EXPECT_EQ(H5Sget_select_elem_npoints(space), 3);

  std::array<hsize_t, 6> out_3{};
  ASSERT_GE(H5Sget_select_elem_pointlist(space, 0, 3, out_3.data()), 0);
  EXPECT_THAT(out_3, ElementsAre(4, 5, 6, 7, 2, 3));

  // 3. PREPEND onto an existing point selection updates head while keeping
  // tail.
  constexpr std::array<hsize_t, 2> kFront = {1, 9};
  ASSERT_GE(H5Sselect_elements(space, H5S_SELECT_PREPEND, 1, kFront.data()), 0);
  EXPECT_EQ(H5Sget_select_elem_npoints(space), 4);
  std::array<hsize_t, 8> out_4{};
  ASSERT_GE(H5Sget_select_elem_pointlist(space, 0, 4, out_4.data()), 0);
  EXPECT_THAT(out_4, ElementsAre(1, 9, 4, 5, 6, 7, 2, 3));

  // 4. PREPEND onto a non-point (H5S_SEL_NONE) selection resets first, then
  // sets
  //    both head and tail on the empty list.
  ASSERT_GE(H5Sselect_none(space), 0);
  EXPECT_EQ(H5Sget_select_type(space), H5S_SEL_NONE);
  ASSERT_GE(H5Sselect_elements(space, H5S_SELECT_PREPEND, 1, kFront.data()), 0);
  EXPECT_EQ(H5Sget_select_elem_npoints(space), 1);

  // 5. SET replaces the selection with 5 unsorted coordinates and exercises
  //    bounding box MIN/MAX tracking plus sequential pointlist caching.
  constexpr std::array<hsize_t, 10> kFive = {5, 8, 1, 2, 7, 4, 3, 9, 2, 1};
  ASSERT_GE(H5Sselect_elements(space, H5S_SELECT_SET, 5, kFive.data()), 0);
  EXPECT_EQ(H5Sget_select_elem_npoints(space), 5);
  EXPECT_EQ(H5Sget_select_npoints(space), 5);

  std::array<hsize_t, 2> start{};
  std::array<hsize_t, 2> end{};
  ASSERT_GE(H5Sget_select_bounds(space, start.data(), end.data()), 0);
  EXPECT_THAT(start, ElementsAre(1, 1));
  EXPECT_THAT(end, ElementsAre(7, 9));

  // Chunk 0..2 populates last_idx = 2, then 2..4 hits the cached pointer.
  std::array<hsize_t, 4> chunk{};
  ASSERT_GE(H5Sget_select_elem_pointlist(space, 0, 2, chunk.data()), 0);
  EXPECT_THAT(chunk, ElementsAre(5, 8, 1, 2));

  chunk.fill(HSIZE_UNDEF);
  ASSERT_GE(H5Sget_select_elem_pointlist(space, 2, 2, chunk.data()), 0);
  EXPECT_THAT(chunk, ElementsAre(7, 4, 3, 9));

  // Non-sequential startpoint misses cache and walks from head.
  chunk.fill(HSIZE_UNDEF);
  ASSERT_GE(H5Sget_select_elem_pointlist(space, 1, 2, chunk.data()), 0);
  EXPECT_THAT(chunk, ElementsAre(1, 2, 7, 4));

  // Requesting past the end stops cleanly without writing beyond valid nodes.
  std::array<hsize_t, 6> tail_buf{};
  tail_buf.fill(HSIZE_UNDEF);
  ASSERT_GE(H5Sget_select_elem_pointlist(space, 3, 3, tail_buf.data()), 0);
  EXPECT_THAT(tail_buf, ElementsAre(3, 9, 2, 1, HSIZE_UNDEF, HSIZE_UNDEF));

  // Startpoint beyond selection size leaves output untouched.
  chunk.fill(HSIZE_UNDEF);
  ASSERT_GE(H5Sget_select_elem_pointlist(space, 10, 2, chunk.data()), 0);
  EXPECT_THAT(chunk,
              ElementsAre(HSIZE_UNDEF, HSIZE_UNDEF, HSIZE_UNDEF, HSIZE_UNDEF));
}

TEST(H5SpointTest, PublicApiErrorPaths) {
  constexpr std::array<hsize_t, 2> kDims = {4, 4};
  constexpr std::array<hsize_t, 2> kCoord = {1, 2};
  ScopedSpace simple(H5Screate_simple(2, kDims.data(), nullptr));
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ScopedSpace null_space(H5Screate(H5S_NULL));
  ASSERT_TRUE(simple.valid());
  ASSERT_TRUE(scalar.valid());
  ASSERT_TRUE(null_space.valid());

  ScopedErrorSilence silence;

  // H5Sselect_elements invalid arguments.
  EXPECT_LT(
      H5Sselect_elements(H5I_INVALID_HID, H5S_SELECT_SET, 1, kCoord.data()), 0);
  EXPECT_LT(
      H5Sselect_elements(H5T_NATIVE_INT, H5S_SELECT_SET, 1, kCoord.data()), 0);
  EXPECT_LT(H5Sselect_elements(scalar, H5S_SELECT_SET, 1, kCoord.data()), 0);
  EXPECT_LT(H5Sselect_elements(null_space, H5S_SELECT_SET, 1, kCoord.data()),
            0);
  EXPECT_LT(H5Sselect_elements(simple, H5S_SELECT_SET, 1, nullptr), 0);
  EXPECT_LT(H5Sselect_elements(simple, H5S_SELECT_SET, 0, kCoord.data()), 0);

  constexpr auto kUnsupportedOps = std::to_array<H5S_seloper_t>({
      H5S_SELECT_NOOP,
      H5S_SELECT_OR,
      H5S_SELECT_AND,
      H5S_SELECT_XOR,
      H5S_SELECT_NOTB,
      H5S_SELECT_NOTA,
      H5S_SELECT_INVALID,
  });
  for (H5S_seloper_t op : kUnsupportedOps) {
    EXPECT_LT(H5Sselect_elements(simple, op, 1, kCoord.data()), 0);
  }

  // H5Sget_select_elem_npoints / H5Sget_select_elem_pointlist errors.
  std::array<hsize_t, 2> buf{};
  EXPECT_LT(H5Sget_select_elem_npoints(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Sget_select_elem_npoints(H5T_NATIVE_INT), 0);
  EXPECT_LT(H5Sget_select_elem_npoints(simple), 0);  // H5S_SEL_ALL

  EXPECT_LT(H5Sget_select_elem_pointlist(simple, 0, 1, nullptr), 0);
  EXPECT_LT(H5Sget_select_elem_pointlist(H5I_INVALID_HID, 0, 1, buf.data()), 0);
  EXPECT_LT(H5Sget_select_elem_pointlist(H5T_NATIVE_INT, 0, 1, buf.data()), 0);
  EXPECT_LT(H5Sget_select_elem_pointlist(simple, 0, 1, buf.data()), 0);

  ASSERT_GE(H5Sselect_none(simple), 0);
  EXPECT_LT(H5Sget_select_elem_npoints(simple), 0);
  EXPECT_LT(H5Sget_select_elem_pointlist(simple, 0, 1, buf.data()), 0);

  constexpr std::array<hsize_t, 2> kStart = {0, 0};
  constexpr std::array<hsize_t, 2> kCount = {2, 2};
  ASSERT_GE(H5Sselect_hyperslab(simple, H5S_SELECT_SET, kStart.data(), nullptr,
                                kCount.data(), nullptr),
            0);
  EXPECT_LT(H5Sget_select_elem_npoints(simple), 0);
  EXPECT_LT(H5Sget_select_elem_pointlist(simple, 0, 1, buf.data()), 0);
}

TEST(H5SpointTest, ValidityBoundsAndSelectionOffsets) {
  constexpr std::array<hsize_t, 2> kDims = {10, 10};
  constexpr std::array<hsize_t, 4> kCoords = {2, 3, 6, 7};
  ScopedSpace space(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());
  ASSERT_GE(H5Sselect_elements(space, H5S_SELECT_SET, 2, kCoords.data()), 0);

  EXPECT_GT(H5Sselect_valid(space), 0);

  // Positive offset keeping selection inside [0, 10).
  constexpr std::array<hssize_t, 2> kPosOk = {3, 2};
  ASSERT_GE(H5Soffset_simple(space, kPosOk.data()), 0);
  EXPECT_GT(H5Sselect_valid(space), 0);
  std::array<hsize_t, 2> start{};
  std::array<hsize_t, 2> end{};
  ASSERT_GE(H5Sget_select_bounds(space, start.data(), end.data()), 0);
  EXPECT_THAT(start, ElementsAre(5, 5));
  EXPECT_THAT(end, ElementsAre(9, 9));

  // Negative offset keeping selection >= 0.
  constexpr std::array<hssize_t, 2> kNegOk = {-2, -1};
  ASSERT_GE(H5Soffset_simple(space, kNegOk.data()), 0);
  EXPECT_GT(H5Sselect_valid(space), 0);
  ASSERT_GE(H5Sget_select_bounds(space, start.data(), end.data()), 0);
  EXPECT_THAT(start, ElementsAre(0, 2));
  EXPECT_THAT(end, ElementsAre(4, 6));

  // Positive offset exceeding extent size.
  constexpr std::array<hssize_t, 2> kPosBad = {5, 0};
  ASSERT_GE(H5Soffset_simple(space, kPosBad.data()), 0);
  EXPECT_EQ(H5Sselect_valid(space), 0);

  // Negative offset moving low bound below 0 triggers bounds error too.
  constexpr std::array<hssize_t, 2> kNegBad = {-3, 0};
  ASSERT_GE(H5Soffset_simple(space, kNegBad.data()), 0);
  EXPECT_EQ(H5Sselect_valid(space), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Sget_select_bounds(space, start.data(), end.data()), 0);
  }

  // NULL resets offset to 0.
  ASSERT_GE(H5Soffset_simple(space, nullptr), 0);
  EXPECT_GT(H5Sselect_valid(space), 0);
}

TEST(H5SpointTest, CopySelectCopyAndReleaseAreIndependent) {
  constexpr std::array<hsize_t, 2> kDims = {8, 8};
  constexpr std::array<hsize_t, 6> kCoords = {0, 1, 2, 3, 4, 5};
  ScopedSpace src(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(src.valid());
  ASSERT_GE(H5Sselect_elements(src, H5S_SELECT_SET, 3, kCoords.data()), 0);

  ScopedSpace copy(H5Scopy(src));
  ASSERT_TRUE(copy.valid());
  ScopedSpace dst(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(dst.valid());
  ASSERT_GE(H5Sselect_copy(dst, src), 0);

  // Mutate and release source; copies must retain original 3 points.
  constexpr std::array<hsize_t, 2> kNew = {7, 7};
  ASSERT_GE(H5Sselect_elements(src, H5S_SELECT_SET, 1, kNew.data()), 0);
  ASSERT_GE(H5Sselect_all(src), 0);

  EXPECT_EQ(H5Sget_select_elem_npoints(copy), 3);
  EXPECT_EQ(H5Sget_select_elem_npoints(dst), 3);
  std::array<hsize_t, 6> pts{};
  ASSERT_GE(H5Sget_select_elem_pointlist(dst, 0, 3, pts.data()), 0);
  EXPECT_THAT(pts, ElementsAreArray(kCoords));
}

TEST(H5SpointTest, ShapeSameMatchesAndMismatches) {
  constexpr std::array<hsize_t, 2> kDims2D = {10, 10};
  constexpr std::array<hsize_t, 3> kDims3D = {6, 10, 10};

  // Same-rank multi-point selections shifted by (2, 1).
  ScopedSpace p1(H5Screate_simple(2, kDims2D.data(), nullptr));
  ScopedSpace p2(H5Screate_simple(2, kDims2D.data(), nullptr));
  ScopedSpace p_diff(H5Screate_simple(2, kDims2D.data(), nullptr));
  ASSERT_TRUE(p1.valid());
  ASSERT_TRUE(p2.valid());
  ASSERT_TRUE(p_diff.valid());

  constexpr std::array<hsize_t, 6> kPts1 = {1, 2, 3, 4, 5, 6};
  constexpr std::array<hsize_t, 6> kPts2 = {3, 3, 5, 5, 7, 7};
  constexpr std::array<hsize_t, 6> kPtsDiff = {3, 3, 5, 5, 7, 8};
  ASSERT_GE(H5Sselect_elements(p1, H5S_SELECT_SET, 3, kPts1.data()), 0);
  ASSERT_GE(H5Sselect_elements(p2, H5S_SELECT_SET, 3, kPts2.data()), 0);
  ASSERT_GE(H5Sselect_elements(p_diff, H5S_SELECT_SET, 3, kPtsDiff.data()), 0);

  EXPECT_GT(H5Sselect_shape_same(p1, p2), 0);
  EXPECT_EQ(H5Sselect_shape_same(p1, p_diff), 0);

  // Same bounds as p2 ([3,3]..[7,7]), but middle point shifted, hitting the
  // interior coordinate comparison in H5S__point_shape_same.
  ScopedSpace p_diff_mid(H5Screate_simple(2, kDims2D.data(), nullptr));
  ASSERT_TRUE(p_diff_mid.valid());
  constexpr std::array<hsize_t, 6> kPtsDiffMid = {3, 3, 5, 6, 7, 7};
  ASSERT_GE(
      H5Sselect_elements(p_diff_mid, H5S_SELECT_SET, 3, kPtsDiffMid.data()), 0);
  EXPECT_EQ(H5Sselect_shape_same(p1, p_diff_mid), 0);

  // Different-rank point selections where the extra dim of 3D space is flat
  // (4).
  ScopedSpace p3d(H5Screate_simple(3, kDims3D.data(), nullptr));
  ASSERT_TRUE(p3d.valid());
  constexpr std::array<hsize_t, 9> kPts3D = {4, 1, 2, 4, 3, 4, 4, 5, 6};
  ASSERT_GE(H5Sselect_elements(p3d, H5S_SELECT_SET, 3, kPts3D.data()), 0);

  EXPECT_GT(H5Sselect_shape_same(p3d, p2), 0);
  EXPECT_GT(H5Sselect_shape_same(p2, p3d), 0);
  EXPECT_EQ(H5Sselect_shape_same(p3d, p_diff), 0);

  // Single point exercises H5S__point_is_single fast path across ranks.
  ScopedSpace single1(H5Screate_simple(3, kDims3D.data(), nullptr));
  ScopedSpace single2(H5Screate_simple(2, kDims2D.data(), nullptr));
  constexpr std::array<hsize_t, 3> kOne3D = {2, 5, 7};
  constexpr std::array<hsize_t, 2> kOne2D = {1, 2};
  ASSERT_GE(H5Sselect_elements(single1, H5S_SELECT_SET, 1, kOne3D.data()), 0);
  ASSERT_GE(H5Sselect_elements(single2, H5S_SELECT_SET, 1, kOne2D.data()), 0);
  EXPECT_GT(H5Sselect_shape_same(single1, single2), 0);

  // Mixed selection types (>1 point vs strided hyperslab of 1x1 blocks) iterate
  // via point_iter_block, point_iter_has_next_block, and point_iter_next_block.
  ScopedSpace p_grid(H5Screate_simple(2, kDims2D.data(), nullptr));
  ScopedSpace p_grid_mismatch(H5Screate_simple(2, kDims2D.data(), nullptr));
  ScopedSpace h_grid(H5Screate_simple(2, kDims2D.data(), nullptr));
  ScopedSpace h_single_block(H5Screate_simple(2, kDims2D.data(), nullptr));
  constexpr std::array<hsize_t, 8> kGridPts = {1, 1, 1, 3, 3, 1, 3, 3};
  constexpr std::array<hsize_t, 8> kGridMismatchPts = {1, 1, 1, 3, 2, 1, 3, 3};
  constexpr std::array<hsize_t, 2> kHStart = {0, 0};
  constexpr std::array<hsize_t, 2> kHStride = {2, 2};
  constexpr std::array<hsize_t, 2> kHCount2x2 = {2, 2};
  constexpr std::array<hsize_t, 2> kHBlock2x2 = {2, 2};
  constexpr std::array<hsize_t, 2> kOne = {1, 1};
  ASSERT_GE(H5Sselect_elements(p_grid, H5S_SELECT_SET, 4, kGridPts.data()), 0);
  ASSERT_GE(H5Sselect_elements(p_grid_mismatch, H5S_SELECT_SET, 4,
                               kGridMismatchPts.data()),
            0);
  ASSERT_GE(
      H5Sselect_hyperslab(h_grid, H5S_SELECT_SET, kHStart.data(),
                          kHStride.data(), kHCount2x2.data(), kOne.data()),
      0);
  ASSERT_GE(H5Sselect_hyperslab(h_single_block, H5S_SELECT_SET, kHStart.data(),
                                nullptr, kOne.data(), kHBlock2x2.data()),
            0);
  EXPECT_GT(H5Sselect_shape_same(p_grid, h_grid), 0);
  EXPECT_EQ(H5Sselect_shape_same(p_grid_mismatch, h_grid), 0);
  EXPECT_EQ(H5Sselect_shape_same(p_grid, h_single_block), 0);

  // Direct internal callback invocation for non-flat leading dim mismatch
  // (bypassing H5S_select_shape_same's bounding-box flatness precheck).
  ScopedSpace p3d_nonflat(H5Screate_simple(3, kDims3D.data(), nullptr));
  constexpr std::array<hsize_t, 9> kPts3DNonFlat = {4, 1, 2, 5, 3, 4, 4, 5, 6};
  ASSERT_GE(
      H5Sselect_elements(p3d_nonflat, H5S_SELECT_SET, 3, kPts3DNonFlat.data()),
      0);
  EXPECT_EQ((*H5S_sel_point->shape_same)(VerifyDataspacePtr(p3d_nonflat),
                                         VerifyDataspacePtr(p2)),
            0);
}

TEST(H5SpointTest, IntersectBlockHitsAndMisses) {
  constexpr std::array<hsize_t, 2> kDims = {20, 20};
  // Include (1, 1), (4, 1), (5, 5), (9, 9) so bounding box is [1,1]..[9,9].
  constexpr std::array<hsize_t, 8> kCoords = {1, 1, 4, 1, 5, 5, 9, 9};
  ScopedSpace space(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());
  ASSERT_GE(H5Sselect_elements(space, H5S_SELECT_SET, 4, kCoords.data()), 0);

  // Intersects (5, 5).
  constexpr std::array<hsize_t, 2> kHitStart = {4, 4};
  constexpr std::array<hsize_t, 2> kHitEnd = {6, 6};
  EXPECT_GT(H5Sselect_intersect_block(space, kHitStart.data(), kHitEnd.data()),
            0);

  // Overlaps bounding box [1,1]..[9,9], checks dim0 match + dim1 miss at (4,1),
  // and returns false after checking all points.
  constexpr std::array<hsize_t, 2> kMissStart = {4, 6};
  constexpr std::array<hsize_t, 2> kMissEnd = {8, 8};
  EXPECT_EQ(
      H5Sselect_intersect_block(space, kMissStart.data(), kMissEnd.data()), 0);

  // Outside bounding box completely.
  constexpr std::array<hsize_t, 2> kFarStart = {12, 12};
  constexpr std::array<hsize_t, 2> kFarEnd = {15, 15};
  EXPECT_EQ(H5Sselect_intersect_block(space, kFarStart.data(), kFarEnd.data()),
            0);
}

TEST(H5SpointTest, SelectionIteratorSeqListSortedCoalescingAndReset) {
  constexpr std::array<hsize_t, 2> kDims = {5, 6};
  // Points in 5x6 row-major layout (4-byte elements):
  //   (2, 3) -> off 15*4 = 60
  //   (0, 1) -> off  1*4 =  4
  //   (0, 2) -> off  2*4 =  8  (contiguous after (0,1)!)
  //   (1, 0) -> off  6*4 = 24
  constexpr std::array<hsize_t, 8> kCoords = {2, 3, 0, 1, 0, 2, 1, 0};
  ScopedSpace space(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());
  ASSERT_GE(H5Sselect_elements(space, H5S_SELECT_SET, 4, kCoords.data()), 0);

  // 1. Default iterator copies the point list, surviving source space edits.
  ScopedSelIter iter_copy(H5Ssel_iter_create(space, sizeof(int32_t), 0));
  ASSERT_TRUE(iter_copy.valid());

  // Mutate original space immediately to verify iterator cloned the point list.
  ASSERT_GE(H5Sselect_none(space), 0);

  std::array<hsize_t, 4> off{};
  std::array<size_t, 4> len{};
  size_t nseq = 0;
  size_t nelmts = 0;

  // Limit maxseq=1 first (stops at curr_seq == maxseq).
  ASSERT_GE(H5Ssel_iter_get_seq_list(iter_copy, 1, 4, &nseq, &nelmts,
                                     off.data(), len.data()),
            0);
  EXPECT_EQ(nseq, 1);
  EXPECT_EQ(nelmts, 1);
  EXPECT_EQ(off[0], 15 * 4);
  EXPECT_EQ(len[0], 4);

  // Next call retrieves remaining 3 elements, coalescing (0,1) and (0,2).
  ASSERT_GE(H5Ssel_iter_get_seq_list(iter_copy, 4, 4, &nseq, &nelmts,
                                     off.data(), len.data()),
            0);
  EXPECT_EQ(nseq, 2);
  EXPECT_EQ(nelmts, 3);
  EXPECT_THAT(std::span(off).first(2), ElementsAre(1 * 4, 6 * 4));
  EXPECT_THAT(std::span(len).first(2), ElementsAre(8, 4));

  // Exhausted iterator returns 0 sequences.
  ASSERT_GE(H5Ssel_iter_get_seq_list(iter_copy, 4, 4, &nseq, &nelmts,
                                     off.data(), len.data()),
            0);
  EXPECT_EQ(nseq, 0);
  EXPECT_EQ(nelmts, 0);

  // 2. Shared + sorted iterator on unsorted points stops before backward jump.
  ASSERT_GE(H5Sselect_elements(space, H5S_SELECT_SET, 4, kCoords.data()), 0);
  constexpr std::array<hssize_t, 2> kSelOff = {1, 0};
  ASSERT_GE(H5Soffset_simple(space, kSelOff.data()), 0);

  ScopedSelIter iter_sorted(H5Ssel_iter_create(
      space, sizeof(int32_t),
      H5S_SEL_ITER_GET_SEQ_LIST_SORTED | H5S_SEL_ITER_SHARE_WITH_DATASPACE));
  ASSERT_TRUE(iter_sorted.valid());

  // With offset (1,0): (3,3)->21*4, then (1,1)->7*4 (< 21*4) triggers sorted
  // break!
  ASSERT_GE(H5Ssel_iter_get_seq_list(iter_sorted, 4, 4, &nseq, &nelmts,
                                     off.data(), len.data()),
            0);
  EXPECT_EQ(nseq, 1);
  EXPECT_EQ(nelmts, 1);
  EXPECT_EQ(off[0], 21 * 4);

  // Remaining points (1,1), (1,2), (2,0) are sorted & (1,1)+(1,2) coalesce.
  ASSERT_GE(H5Ssel_iter_get_seq_list(iter_sorted, 4, 2, &nseq, &nelmts,
                                     off.data(), len.data()),
            0);
  EXPECT_EQ(nseq, 1);
  EXPECT_EQ(nelmts, 2);
  EXPECT_EQ(off[0], 7 * 4);
  EXPECT_EQ(len[0], 8);

  // 3. Reset iterator onto fresh point selection and read remaining element.
  constexpr std::array<hsize_t, 4> kAsc = {0, 0, 0, 2};
  ScopedSpace asc_space(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_GE(H5Sselect_elements(asc_space, H5S_SELECT_SET, 2, kAsc.data()), 0);
  ASSERT_GE(H5Ssel_iter_reset(iter_sorted, asc_space), 0);
  ASSERT_GE(H5Ssel_iter_get_seq_list(iter_sorted, 4, 4, &nseq, &nelmts,
                                     off.data(), len.data()),
            0);
  EXPECT_EQ(nseq, 2);
  EXPECT_EQ(nelmts, 2);
  EXPECT_THAT(std::span(off).first(2), ElementsAre(0, 8));
}

TEST(H5SpointTest, DatasetReadWriteProjectIterateAndFill) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  // 1. Contiguous 2D dataset read/write with rank-up and rank-down projections.
  constexpr std::array<hsize_t, 2> kDims2D = {4, 5};
  ScopedSpace fspace(H5Screate_simple(2, kDims2D.data(), nullptr));
  ASSERT_TRUE(fspace.valid());

  constexpr char kContigName[] = "contig";
  ScopedDataset dset(H5Dcreate2(file, kContigName, H5T_NATIVE_INT32, fspace,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  constexpr std::array<hsize_t, 6> kFilePts = {0, 1, 2, 3, 3, 4};
  ASSERT_GE(H5Sselect_elements(fspace, H5S_SELECT_SET, 3, kFilePts.data()), 0);

  // 3D memory dataspace (flat dim0=1) projects down to 2D file space.
  constexpr std::array<hsize_t, 3> kDims3D = {2, 4, 5};
  ScopedSpace mspace3d(H5Screate_simple(3, kDims3D.data(), nullptr));
  constexpr std::array<hsize_t, 9> kMemPts3D = {1, 0, 0, 1, 1, 2, 1, 3, 1};
  ASSERT_GE(H5Sselect_elements(mspace3d, H5S_SELECT_SET, 3, kMemPts3D.data()),
            0);

  std::vector<int32_t> write_3d(2 * 4 * 5, 0);
  write_3d[1 * 20 + 0 * 5 + 0] = 111;
  write_3d[1 * 20 + 1 * 5 + 2] = 222;
  write_3d[1 * 20 + 3 * 5 + 1] = 333;
  ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, mspace3d, fspace, H5P_DEFAULT,
                     write_3d.data()),
            0);

  // 1D memory dataspace projects up into a flat-row 2D file selection.
  ScopedSpace fspace_flat(H5Screate_simple(2, kDims2D.data(), nullptr));
  constexpr std::array<hsize_t, 4> kFlatFilePts = {2, 3, 2, 1};
  ASSERT_GE(
      H5Sselect_elements(fspace_flat, H5S_SELECT_SET, 2, kFlatFilePts.data()),
      0);

  constexpr std::array<hsize_t, 1> kDims1D = {6};
  ScopedSpace mspace1d(H5Screate_simple(1, kDims1D.data(), nullptr));
  constexpr std::array<hsize_t, 2> kMemPts1D = {4, 1};
  ASSERT_GE(H5Sselect_elements(mspace1d, H5S_SELECT_SET, 2, kMemPts1D.data()),
            0);

  std::array<int32_t, 6> read_1d{};
  read_1d.fill(-1);
  ASSERT_GE(H5Dread(dset, H5T_NATIVE_INT32, mspace1d, fspace_flat, H5P_DEFAULT,
                    read_1d.data()),
            0);
  EXPECT_EQ(read_1d[4], 222);
  EXPECT_EQ(read_1d[1], 0);

  // Also project 1D point selection UP into 2D file space on write.
  read_1d[4] = 444;
  read_1d[1] = 555;
  ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, mspace1d, fspace_flat, H5P_DEFAULT,
                     read_1d.data()),
            0);
  std::vector<int32_t> read_3d(2 * 4 * 5, -1);
  ASSERT_GE(H5Dread(dset, H5T_NATIVE_INT32, mspace3d, fspace, H5P_DEFAULT,
                    read_3d.data()),
            0);
  EXPECT_EQ(read_3d[1 * 20 + 0 * 5 + 0], 111);
  EXPECT_EQ(read_3d[1 * 20 + 1 * 5 + 2], 444);
  EXPECT_EQ(read_3d[1 * 20 + 3 * 5 + 1], 333);

  // 2. Scalar dataset + 1-point simple space exercises project_scalar & offset.
  ScopedSpace scalar_space(H5Screate(H5S_SCALAR));
  constexpr char kScalarName[] = "scalar";
  ScopedDataset scalar_dset(H5Dcreate2(file, kScalarName, H5T_NATIVE_INT32,
                                       scalar_space, H5P_DEFAULT, H5P_DEFAULT,
                                       H5P_DEFAULT));
  ASSERT_TRUE(scalar_dset.valid());
  ScopedSpace one_pt_space(H5Screate_simple(2, kDims2D.data(), nullptr));
  constexpr std::array<hsize_t, 2> kOnePt = {2, 3};
  ASSERT_GE(H5Sselect_elements(one_pt_space, H5S_SELECT_SET, 1, kOnePt.data()),
            0);
  std::array<int32_t, 20> one_buf{};
  one_buf[2 * 5 + 3] = 777;
  ASSERT_GE(H5Dwrite(scalar_dset, H5T_NATIVE_INT32, one_pt_space, H5S_ALL,
                     H5P_DEFAULT, one_buf.data()),
            0);
  int32_t scalar_val = 0;
  ASSERT_GE(H5Dread(scalar_dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                    H5P_DEFAULT, &scalar_val),
            0);
  EXPECT_EQ(scalar_val, 777);

  // 3. Chunked dataset I/O exercises H5S__point_adjust_u and single-point I/O.
  constexpr std::array<hsize_t, 2> kChunkDims = {3, 3};
  constexpr std::array<hsize_t, 2> kChunkedDims = {6, 6};
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_GE(H5Pset_chunk(dcpl, 2, kChunkDims.data()), 0);
  ScopedSpace chunk_fspace(H5Screate_simple(2, kChunkedDims.data(), nullptr));
  constexpr char kChunkedName[] = "chunked";
  ScopedDataset chunk_dset(H5Dcreate2(file, kChunkedName, H5T_NATIVE_INT32,
                                      chunk_fspace, H5P_DEFAULT, dcpl,
                                      H5P_DEFAULT));
  ASSERT_TRUE(chunk_dset.valid());

  constexpr std::array<hsize_t, 4> kChunkPts = {0, 1, 4, 5};
  ASSERT_GE(
      H5Sselect_elements(chunk_fspace, H5S_SELECT_SET, 2, kChunkPts.data()), 0);
  ScopedSpace chunk_mspace(H5Screate_simple(1, kDims1D.data(), nullptr));
  constexpr std::array<hsize_t, 2> kChunkMPts = {0, 3};
  ASSERT_GE(
      H5Sselect_elements(chunk_mspace, H5S_SELECT_SET, 2, kChunkMPts.data()),
      0);
  std::array<int32_t, 6> chunk_in = {91, 0, 0, 92, 0, 0};
  ASSERT_GE(H5Dwrite(chunk_dset, H5T_NATIVE_INT32, chunk_mspace, chunk_fspace,
                     H5P_DEFAULT, chunk_in.data()),
            0);
  std::array<int32_t, 6> chunk_out{};
  ASSERT_GE(H5Dread(chunk_dset, H5T_NATIVE_INT32, chunk_mspace, chunk_fspace,
                    H5P_DEFAULT, chunk_out.data()),
            0);
  EXPECT_EQ(chunk_out[0], 91);
  EXPECT_EQ(chunk_out[3], 92);

  // 4. H5Dfill and H5Diterate on a point selection.
  std::array<int32_t, 6> fill_buf{};
  const int32_t kFillVal = 42;
  ASSERT_GE(H5Dfill(&kFillVal, H5T_NATIVE_INT32, fill_buf.data(),
                    H5T_NATIVE_INT32, chunk_mspace),
            0);
  EXPECT_THAT(fill_buf, ElementsAre(42, 0, 0, 42, 0, 0));

  std::vector<int32_t> visited;
  auto iter_cb = [](void* elem, hid_t /*type_id*/, unsigned /*ndim*/,
                    const hsize_t* /*point*/, void* op_data) -> herr_t {
    auto* out = static_cast<std::vector<int32_t>*>(op_data);
    out->push_back(*static_cast<const int32_t*>(elem));
    return 0;
  };
  ASSERT_GE(H5Diterate(fill_buf.data(), H5T_NATIVE_INT32, chunk_mspace, iter_cb,
                       &visited),
            0);
  EXPECT_THAT(visited, ElementsAre(42, 42));
}

TEST(H5SpointTest, ProjectIntersectionWithPointSelections) {
  constexpr std::array<hsize_t, 2> kDims = {8, 8};
  ScopedSpace src(H5Screate_simple(2, kDims.data(), nullptr));
  ScopedSpace dst(H5Screate_simple(2, kDims.data(), nullptr));
  ScopedSpace sis_pts(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(src.valid());
  ASSERT_TRUE(dst.valid());
  ASSERT_TRUE(sis_pts.valid());

  constexpr std::array<hsize_t, 6> kSrcPts = {1, 1, 2, 2, 5, 5};
  constexpr std::array<hsize_t, 6> kDstPts = {0, 0, 0, 1, 0, 2};
  constexpr std::array<hsize_t, 4> kSisPts = {2, 2, 7, 7};
  ASSERT_GE(H5Sselect_elements(src, H5S_SELECT_SET, 3, kSrcPts.data()), 0);
  ASSERT_GE(H5Sselect_elements(dst, H5S_SELECT_SET, 3, kDstPts.data()), 0);
  ASSERT_GE(H5Sselect_elements(sis_pts, H5S_SELECT_SET, 2, kSisPts.data()), 0);

  ScopedSpace proj(H5Sselect_project_intersection(src, dst, sis_pts));
  ASSERT_TRUE(proj.valid());
  EXPECT_EQ(H5Sget_select_type(proj), H5S_SEL_POINTS);
  EXPECT_EQ(H5Sget_select_elem_npoints(proj), 1);
  std::array<hsize_t, 2> pt{};
  ASSERT_GE(H5Sget_select_elem_pointlist(proj, 0, 1, pt.data()), 0);
  EXPECT_THAT(pt, ElementsAre(0, 1));
}

TEST(H5SpointTest, AdjustSignedAndUnsigned) {
  constexpr std::array<hsize_t, 2> kDims = {20, 20};
  constexpr std::array<hsize_t, 4> kCoords = {5, 6, 10, 12};
  ScopedSpace space(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());
  ASSERT_GE(H5Sselect_elements(space, H5S_SELECT_SET, 2, kCoords.data()), 0);

  // Public signed adjust: zero offset is a no-op.
  constexpr std::array<hssize_t, 2> kZeroS = {0, 0};
  ASSERT_GE(H5Sselect_adjust(space, kZeroS.data()), 0);

  // Positive signed adjust subtracts offset.
  constexpr std::array<hssize_t, 2> kPosS = {2, 3};
  ASSERT_GE(H5Sselect_adjust(space, kPosS.data()), 0);
  std::array<hsize_t, 4> pts{};
  ASSERT_GE(H5Sget_select_elem_pointlist(space, 0, 2, pts.data()), 0);
  EXPECT_THAT(pts, ElementsAre(3, 3, 8, 9));

  // Negative signed adjust shifts back up.
  constexpr std::array<hssize_t, 2> kNegS = {-1, -2};
  ASSERT_GE(H5Sselect_adjust(space, kNegS.data()), 0);
  ASSERT_GE(H5Sget_select_elem_pointlist(space, 0, 2, pts.data()), 0);
  EXPECT_THAT(pts, ElementsAre(4, 5, 9, 11));

  // Adjusting below zero fails cleanly.
  {
    ScopedErrorSilence silence;
    constexpr std::array<hssize_t, 2> kTooLargeS = {5, 0};
    EXPECT_LT(H5Sselect_adjust(space, kTooLargeS.data()), 0);
  }

  // Internal unsigned adjust: zero offset and non-zero offset.
  H5S_t* raw = VerifyDataspacePtr(space);
  ASSERT_NE(raw, nullptr);
  constexpr std::array<hsize_t, 2> kZeroU = {0, 0};
  ASSERT_GE(H5S_select_adjust_u(raw, kZeroU.data()), 0);
  constexpr std::array<hsize_t, 2> kPosU = {1, 2};
  ASSERT_GE(H5S_select_adjust_u(raw, kPosU.data()), 0);

  std::array<hsize_t, 2> start{};
  std::array<hsize_t, 2> end{};
  ASSERT_GE(H5Sget_select_bounds(space, start.data(), end.data()), 0);
  EXPECT_THAT(start, ElementsAre(3, 3));
  EXPECT_THAT(end, ElementsAre(8, 9));
}

TEST(H5SpointTest, EncodeDecodeAcrossLibverAndCoordinateSizes) {
  struct EncodeCase {
    H5F_libver_t low;
    H5F_libver_t high;
    hsize_t dim;
    hsize_t p0;
    hsize_t p1;
    bool use_encode1;
  };
  constexpr auto kCases = std::to_array<EncodeCase>({
      // V1 (4-byte coordinates) via H5Sencode1 and H5Sencode2(EARLIEST..V110).
      {.low = H5F_LIBVER_EARLIEST,
       .high = H5F_LIBVER_LATEST,
       .dim = 100,
       .p0 = 3,
       .p1 = 17,
       .use_encode1 = true},
      {.low = H5F_LIBVER_EARLIEST,
       .high = H5F_LIBVER_V110,
       .dim = 100,
       .p0 = 5,
       .p1 = 42,
       .use_encode1 = false},
      // V2 with 2-byte encoding (coords <= 65535).
      {.low = H5F_LIBVER_V112,
       .high = H5F_LIBVER_V112,
       .dim = 100,
       .p0 = 12,
       .p1 = 99,
       .use_encode1 = false},
      // V2 with 4-byte encoding (65535 < coords <= UINT32_MAX).
      {.low = H5F_LIBVER_V112,
       .high = H5F_LIBVER_LATEST,
       .dim = 100000,
       .p0 = 70000,
       .p1 = 99999,
       .use_encode1 = false},
      // V2 with 8-byte encoding (coords > UINT32_MAX).
      {.low = H5F_LIBVER_V112,
       .high = H5F_LIBVER_LATEST,
       .dim = (1ULL << 34),
       .p0 = (1ULL << 33) + 7,
       .p1 = (1ULL << 33) + 19,
       .use_encode1 = false},
  });

  std::vector<size_t> encoded_sizes;
  for (const EncodeCase& tc : kCases) {
    const std::array<hsize_t, 2> dims = {tc.dim, tc.dim};
    ScopedSpace space(H5Screate_simple(2, dims.data(), nullptr));
    ASSERT_TRUE(space.valid());
    const std::array<hsize_t, 4> pts = {tc.p0, 1, tc.p1, 2};
    ASSERT_GE(H5Sselect_elements(space, H5S_SELECT_SET, 2, pts.data()), 0);

    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_libver_bounds(fapl, tc.low, tc.high), 0);

    size_t buf_size = 0;
    if (tc.use_encode1) {
      ASSERT_GE(H5Sencode1(space, nullptr, &buf_size), 0);
    } else {
      ASSERT_GE(H5Sencode2(space, nullptr, &buf_size, fapl), 0);
    }
    ASSERT_GT(buf_size, 0);

    std::vector<uint8_t> buf(buf_size);
    if (tc.use_encode1) {
      ASSERT_GE(H5Sencode1(space, buf.data(), &buf_size), 0);
    } else {
      ASSERT_GE(H5Sencode2(space, buf.data(), &buf_size, fapl), 0);
    }
    encoded_sizes.push_back(buf_size);

    // Note: H5Sdecode() calls H5S_select_deserialize(..., SIZE_MAX), where
    // `*p + SIZE_MAX - 1` in H5Sselect.c triggers UBSan pointer-overflow under
    // `--config=asan-chlor`. Deserialization across all V1/V2 formats is
    // verified via H5Ropen_region below and H5S_select_deserialize directly.
  }
  ASSERT_EQ(encoded_sizes.size(), 5U);
  // V2 2-byte encoding < V1 4-byte encoding < V2 8-byte encoding.
  EXPECT_LT(encoded_sizes[2], encoded_sizes[1]);
  EXPECT_LT(encoded_sizes[3], encoded_sizes[4]);

  // Public serialize + deserialize round-trip via dataset region reference.
  {
    ScopedPlist core_fapl = MakeCoreFapl();
    ASSERT_TRUE(core_fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(core_fapl, H5F_LIBVER_V112, H5F_LIBVER_LATEST), 0);
    ScopedFile file(
        H5Fcreate("point_ref.h5", H5F_ACC_TRUNC, H5P_DEFAULT, core_fapl));
    ASSERT_TRUE(file.valid());
    constexpr std::array<hsize_t, 2> kDims = {10, 10};
    constexpr std::array<hsize_t, 4> kRefPts = {1, 3, 7, 8};
    ScopedSpace space(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(file, "data", H5T_NATIVE_INT32, space,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    ASSERT_GE(H5Sselect_elements(space, H5S_SELECT_SET, 2, kRefPts.data()), 0);

    H5R_ref_t ref{};
    absl::Cleanup destroy_ref = [&] { H5Rdestroy(&ref); };
    ASSERT_GE(H5Rcreate_region(file, "data", space, H5P_DEFAULT, &ref), 0);

    ScopedSpace decoded(H5Ropen_region(&ref, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(decoded.valid());
    EXPECT_EQ(H5Sget_select_type(decoded), H5S_SEL_POINTS);
    EXPECT_EQ(H5Sget_select_elem_npoints(decoded), 2);
    std::array<hsize_t, 4> out{};
    ASSERT_GE(H5Sget_select_elem_pointlist(decoded, 0, 2, out.data()), 0);
    EXPECT_THAT(out, ElementsAreArray(kRefPts));
  }

  // Encoding coordinates > 2^32 with high_bound=V110 fails with range error.
  constexpr std::array<hsize_t, 1> kHugeDim = {(1ULL << 34)};
  constexpr std::array<hsize_t, 1> kHugePt = {(1ULL << 33) + 5};
  ScopedSpace huge_space(H5Screate_simple(1, kHugeDim.data(), nullptr));
  ASSERT_GE(H5Sselect_elements(huge_space, H5S_SELECT_SET, 1, kHugePt.data()),
            0);
  ScopedPlist fapl_v110(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(
      H5Pset_libver_bounds(fapl_v110, H5F_LIBVER_EARLIEST, H5F_LIBVER_V110), 0);
  {
    ScopedErrorSilence silence;
    size_t sz = 0;
    EXPECT_LT(H5Sencode2(huge_space, nullptr, &sz, fapl_v110), 0);
  }

  // Encoding a selection whose offset pushes low bounds negative fails.
  constexpr std::array<hsize_t, 1> kSmallDim = {10};
  constexpr std::array<hsize_t, 1> kSmallPt = {1};
  constexpr std::array<hssize_t, 1> kNegOff = {-2};
  ScopedSpace oob_space(H5Screate_simple(1, kSmallDim.data(), nullptr));
  ASSERT_GE(H5Sselect_elements(oob_space, H5S_SELECT_SET, 1, kSmallPt.data()),
            0);
  ASSERT_GE(H5Soffset_simple(oob_space, kNegOff.data()), 0);
  {
    ScopedErrorSilence silence;
    size_t sz = 0;
    EXPECT_LT(H5Sencode2(oob_space, nullptr, &sz, fapl_v110), 0);
  }
}

TEST(H5SpointTest, InternalSerializeDeserializeAndErrorPaths) {
  constexpr std::array<hsize_t, 2> kDims = {(1ULL << 34), 100};
  ScopedSpace space_2b(H5Screate_simple(2, kDims.data(), nullptr));
  ScopedSpace space_4b(H5Screate_simple(2, kDims.data(), nullptr));
  ScopedSpace space_8b(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(space_2b.valid());
  ASSERT_TRUE(space_4b.valid());
  ASSERT_TRUE(space_8b.valid());

  constexpr std::array<hsize_t, 4> kSmallPts = {1, 2, 3, 4};
  constexpr std::array<hsize_t, 4> kMedPts = {70000, 2, 90000, 5};
  constexpr std::array<hsize_t, 4> kHugePts = {(1ULL << 33) + 1, 5,
                                               (1ULL << 33) + 9, 8};
  ASSERT_GE(H5Sselect_elements(space_2b, H5S_SELECT_SET, 2, kSmallPts.data()),
            0);
  ASSERT_GE(H5Sselect_elements(space_4b, H5S_SELECT_SET, 2, kMedPts.data()), 0);
  ASSERT_GE(H5Sselect_elements(space_8b, H5S_SELECT_SET, 2, kHugePts.data()),
            0);

  ScopedPlist fapl_v110(H5Pcreate(H5P_FILE_ACCESS));
  ScopedPlist fapl_v112(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(
      H5Pset_libver_bounds(fapl_v110, H5F_LIBVER_EARLIEST, H5F_LIBVER_V110), 0);
  ASSERT_GE(H5Pset_libver_bounds(fapl_v112, H5F_LIBVER_V112, H5F_LIBVER_LATEST),
            0);

  auto round_trip_with_null_out = [&](hid_t fapl_id, H5S_t* raw_space,
                                      std::span<const hsize_t> expected_pts) {
    ScopedApiContext ctx(fapl_id);
    ASSERT_TRUE(ctx.valid());
    const hssize_t ssize = H5S_select_serial_size(raw_space);
    ASSERT_GT(ssize, 0);
    std::vector<uint8_t> buf(static_cast<size_t>(ssize));
    uint8_t* p_enc = buf.data();
    ASSERT_GE(H5S_select_serialize(raw_space, &p_enc), 0);
    ASSERT_EQ(p_enc, buf.data() + buf.size());

    // Passing *out_space == nullptr allocates a new H5S_t and populates rank.
    H5S_t* out_space = nullptr;
    absl::Cleanup close_out = [&out_space] {
      if (out_space != nullptr) {
        H5S_close(out_space);
      }
    };
    const uint8_t* p_dec = buf.data();
    ASSERT_GE(H5S_select_deserialize(&out_space, &p_dec, buf.size()), 0);
    ASSERT_NE(out_space, nullptr);
    EXPECT_EQ(p_dec, buf.data() + buf.size());
    EXPECT_EQ(H5S_get_select_npoints(out_space), 2);
    EXPECT_EQ(out_space->extent.rank, 2U);

    std::vector<hsize_t> decoded_pts;
    for (const H5S_pnt_node_t* node = out_space->select.sel_info.pnt_lst->head;
         node != nullptr; node = node->next) {
      decoded_pts.push_back(node->pnt[0]);
      decoded_pts.push_back(node->pnt[1]);
    }
    EXPECT_THAT(decoded_pts, ElementsAreArray(expected_pts));

    // Check rank mismatch when *out_space != nullptr.
    ScopedSpace space_1d(H5Screate_simple(1, kDims.data(), nullptr));
    H5S_t* raw_1d = VerifyDataspacePtr(space_1d);
    p_dec = buf.data();
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5S_select_deserialize(&raw_1d, &p_dec, buf.size()), 0);
    }

    // Verify truncation fails cleanly at every header/body boundary without
    // leaking tmp_space. Keep len >= 5 so selection type decodes and
    // H5S__point_deserialize handles every bounds check.
    for (size_t trunc = 5; trunc < buf.size(); ++trunc) {
      H5S_t* err_space = nullptr;
      const uint8_t* p_err = buf.data();
      ScopedErrorSilence silence;
      EXPECT_LT(H5S_select_deserialize(&err_space, &p_err, trunc), 0);
      EXPECT_EQ(err_space, nullptr);
    }
  };

  round_trip_with_null_out(fapl_v110, VerifyDataspacePtr(space_2b), kSmallPts);
  round_trip_with_null_out(fapl_v112, VerifyDataspacePtr(space_2b), kSmallPts);
  round_trip_with_null_out(fapl_v112, VerifyDataspacePtr(space_4b), kMedPts);
  round_trip_with_null_out(fapl_v112, VerifyDataspacePtr(space_8b), kHugePts);

  // Exercise num_elem > UINT32_MAX check in H5S__point_get_version_enc_size
  // without allocating 2^32 nodes (serial_size only inspects num_elem).
  {
    H5S_t* raw = VerifyDataspacePtr(space_2b);
    const hsize_t saved_num_elem = raw->select.num_elem;
    absl::Cleanup restore_num = [&] { raw->select.num_elem = saved_num_elem; };
    raw->select.num_elem = (1ULL << 32) + 1;

    {
      ScopedApiContext ctx(fapl_v110);
      ASSERT_TRUE(ctx.valid());
      ScopedErrorSilence silence;
      EXPECT_LT(H5S_select_serial_size(raw), 0);
    }
    {
      ScopedApiContext ctx(fapl_v112);
      ASSERT_TRUE(ctx.valid());
      EXPECT_GT(H5S_select_serial_size(raw), 0);
    }
  }

  // Crafted corrupt serialized point buffers testing every header error branch.
  auto expect_corrupt = [](const std::vector<uint8_t>& buf) {
    H5S_t* out = nullptr;
    const uint8_t* p = buf.data();
    ScopedErrorSilence silence;
    EXPECT_LT(H5S_select_deserialize(&out, &p, buf.size()), 0);
    EXPECT_EQ(out, nullptr);
  };

  // Bad versions (0 and 3).
  {
    std::vector<uint8_t> v0;
    AppendUint32LE(v0, H5S_SEL_POINTS);
    AppendUint32LE(v0, 0);
    expect_corrupt(v0);

    std::vector<uint8_t> v3;
    AppendUint32LE(v3, H5S_SEL_POINTS);
    AppendUint32LE(v3, 3);
    expect_corrupt(v3);
  }
  // Bad enc_size bits (0x01) and invalid enc_size passing mask (0x06).
  {
    std::vector<uint8_t> bad_mask;
    AppendUint32LE(bad_mask, H5S_SEL_POINTS);
    AppendUint32LE(bad_mask, H5S_POINT_VERSION_2);
    bad_mask.push_back(0x01);
    expect_corrupt(bad_mask);

    std::vector<uint8_t> bad_switch;
    AppendUint32LE(bad_switch, H5S_SEL_POINTS);
    AppendUint32LE(bad_switch, H5S_POINT_VERSION_2);
    bad_switch.push_back(0x06);
    AppendUint32LE(bad_switch, 1);  // rank
    expect_corrupt(bad_switch);
  }
  // Invalid ranks (0 and H5S_MAX_RANK + 1).
  {
    for (uint32_t bad_rank : {0U, static_cast<uint32_t>(H5S_MAX_RANK + 1)}) {
      std::vector<uint8_t> bad_r;
      AppendUint32LE(bad_r, H5S_SEL_POINTS);
      AppendUint32LE(bad_r, H5S_POINT_VERSION_2);
      bad_r.push_back(H5S_SELECT_INFO_ENC_SIZE_2);
      AppendUint32LE(bad_r, bad_rank);
      expect_corrupt(bad_r);
    }
  }
  // Truncated num_elem for enc_size=2, 4, 8 and multiplication overflow.
  {
    for (uint8_t esz : {H5S_SELECT_INFO_ENC_SIZE_2, H5S_SELECT_INFO_ENC_SIZE_4,
                        H5S_SELECT_INFO_ENC_SIZE_8}) {
      std::vector<uint8_t> trunc_ne;
      AppendUint32LE(trunc_ne, H5S_SEL_POINTS);
      AppendUint32LE(trunc_ne, H5S_POINT_VERSION_2);
      trunc_ne.push_back(esz);
      AppendUint32LE(trunc_ne, 1);  // rank
      trunc_ne.push_back(0x01);     // 1 byte < 2/4/8
      expect_corrupt(trunc_ne);
    }

    // Multiplication overflow: enc_size=8, rank=2, num_elem = (SIZE_MAX/16)+1.
    std::vector<uint8_t> mul_ovf;
    AppendUint32LE(mul_ovf, H5S_SEL_POINTS);
    AppendUint32LE(mul_ovf, H5S_POINT_VERSION_2);
    mul_ovf.push_back(H5S_SELECT_INFO_ENC_SIZE_8);
    AppendUint32LE(mul_ovf, 2);
    AppendUint64LE(mul_ovf, (std::numeric_limits<size_t>::max() / 16) + 1);
    expect_corrupt(mul_ovf);

    // Valid V2 2-byte header claiming 3 points with only 1 point present.
    std::vector<uint8_t> coord_ovf;
    AppendUint32LE(coord_ovf, H5S_SEL_POINTS);
    AppendUint32LE(coord_ovf, H5S_POINT_VERSION_2);
    coord_ovf.push_back(H5S_SELECT_INFO_ENC_SIZE_2);
    AppendUint32LE(coord_ovf, 1);
    AppendUint16LE(coord_ovf, 3);
    AppendUint16LE(coord_ovf, 7);
    expect_corrupt(coord_ovf);
  }
}

TEST(H5SpointTest, InternalQueriesIteratorsAndProjections) {
  constexpr std::array<hsize_t, 2> kDims = {10, 12};
  ScopedSpace space1(H5Screate_simple(2, kDims.data(), nullptr));
  ScopedSpace space2(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(space1.valid());
  ASSERT_TRUE(space2.valid());

  constexpr std::array<hsize_t, 2> kOnePt = {3, 4};
  constexpr std::array<hsize_t, 6> kThreePts = {1, 2, 3, 4, 5, 6};
  ASSERT_GE(H5Sselect_elements(space1, H5S_SELECT_SET, 1, kOnePt.data()), 0);
  ASSERT_GE(H5Sselect_elements(space2, H5S_SELECT_SET, 3, kThreePts.data()), 0);

  H5S_t* raw1 = VerifyDataspacePtr(space1);
  H5S_t* raw2 = VerifyDataspacePtr(space2);
  ASSERT_NE(raw1, nullptr);
  ASSERT_NE(raw2, nullptr);

  // Predicate callbacks: 1 point vs >1 points.
  EXPECT_EQ(H5S_get_select_unlim_dim(raw1), -1);
  EXPECT_GT(H5S_select_is_contiguous(raw1), 0);
  EXPECT_EQ(H5S_select_is_contiguous(raw2), 0);
  EXPECT_GT(H5S_select_is_single(raw1), 0);
  EXPECT_EQ(H5S_select_is_single(raw2), 0);
  EXPECT_GT(H5S_select_is_regular(raw1), 0);
  EXPECT_EQ(H5S_select_is_regular(raw2), 0);

  // Offset query: valid and out-of-bounds (negative and >= dim_size).
  hsize_t linear_off = 0;
  ASSERT_GE(H5S_get_select_offset(raw1, &linear_off), 0);
  EXPECT_EQ(linear_off, 3 * 12 + 4);

  constexpr std::array<hssize_t, 2> kOobNeg = {-4, 0};
  ASSERT_GE(H5Soffset_simple(space1, kOobNeg.data()), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5S_get_select_offset(raw1, &linear_off), 0);
  }
  constexpr std::array<hssize_t, 2> kOobHigh = {7, 0};
  ASSERT_GE(H5Soffset_simple(space1, kOobHigh.data()), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5S_get_select_offset(raw1, &linear_off), 0);
  }

  // Internal iterator coords, nelmts, next(2), and release.
  H5S_sel_iter_t iter{};
  ASSERT_GE(H5S_select_iter_init(&iter, raw2, sizeof(int32_t), 0), 0);
  absl::Cleanup release_iter = [&] { H5S_select_iter_release(&iter); };

  EXPECT_EQ(H5S_select_iter_nelmts(&iter), 3U);
  std::array<hsize_t, 2> coords{};
  ASSERT_GE(H5S_select_iter_coords(&iter, coords.data()), 0);
  EXPECT_THAT(coords, ElementsAre(1, 2));

  ASSERT_GE(H5S_select_iter_next(&iter, 2), 0);
  EXPECT_EQ(H5S_select_iter_nelmts(&iter), 1U);
  ASSERT_GE(H5S_select_iter_coords(&iter, coords.data()), 0);
  EXPECT_THAT(coords, ElementsAre(5, 6));

  // Scalar projection error when a selection unexpectedly has >1 node.
  {
    ScopedErrorSilence silence;
    hsize_t proj_off = 0;
    EXPECT_LT(H5S_select_project_scalar(raw2, &proj_off), 0);
  }

  // Direct H5S_select_construct_projection down (3D->2D) and up (2D->3D).
  constexpr std::array<hsize_t, 3> kDims3D = {5, 10, 12};
  ScopedSpace space3d(H5Screate_simple(3, kDims3D.data(), nullptr));
  constexpr std::array<hsize_t, 6> kFlat3D = {2, 1, 3, 2, 4, 5};
  ASSERT_GE(H5Sselect_elements(space3d, H5S_SELECT_SET, 2, kFlat3D.data()), 0);
  H5S_t* raw3d = VerifyDataspacePtr(space3d);

  H5S_t* proj_down = nullptr;
  absl::Cleanup close_down = [&] {
    if (proj_down != nullptr) H5S_close(proj_down);
  };
  ptrdiff_t buf_adj = 0;
  ASSERT_GE(H5S_select_construct_projection(raw3d, &proj_down, 2,
                                            sizeof(int32_t), &buf_adj),
            0);
  ASSERT_NE(proj_down, nullptr);
  EXPECT_EQ(buf_adj, static_cast<ptrdiff_t>(2 * 10 * 12 * sizeof(int32_t)));

  H5S_t* proj_up = nullptr;
  absl::Cleanup close_up = [&] {
    if (proj_up != nullptr) H5S_close(proj_up);
  };
  ASSERT_GE(H5S_select_construct_projection(proj_down, &proj_up, 4,
                                            sizeof(int32_t), &buf_adj),
            0);
  ASSERT_NE(proj_up, nullptr);
  EXPECT_EQ(buf_adj, 0);
}

}  // namespace
}  // namespace hdf5
