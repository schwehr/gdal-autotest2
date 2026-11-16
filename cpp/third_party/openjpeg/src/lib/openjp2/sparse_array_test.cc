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
// Tests internal OpenJPEG sparse int32 array lifecycle, bounds validation,
// block-tiled read/write, and strided buffer fast paths
// (src/lib/openjp2/sparse_array.c).
//
// Caveats:
// - Exercises sparse_array APIs without accessing opj_sparse_array_int32_t
//   internals; relies on 32-bit OPJ_UINT32 overflow math for block counts.
//
// See also:
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/sparse_array.c
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/sparse_array.h

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/openjpeg2/google/util/opj_internal.h"

namespace openjpeg2 {
namespace {

using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;

static_assert(OPJ_TRUE == 1 && OPJ_FALSE == 0);
static_assert(sizeof(OPJ_INT32) == 4);

constexpr OPJ_INT32 kDestSentinel = -2000000001;
constexpr OPJ_INT32 kSrcPoison = 2000000001;

struct SparseArrayDeleter {
  void operator()(opj_sparse_array_int32_t* sa) const {
    opj_sparse_array_int32_free(sa);
  }
};
using SparseArrayPtr =
    std::unique_ptr<opj_sparse_array_int32_t, SparseArrayDeleter>;

constexpr OPJ_INT32 SampleValue(OPJ_UINT32 x, OPJ_UINT32 y,
                                OPJ_INT32 bias = 0) {
  const int64_t mag =
      static_cast<int64_t>(y) * 1009 + static_cast<int64_t>(x) * 37 + 1 + bias;
  return static_cast<OPJ_INT32>((((x ^ y) & 1u) == 0u) ? mag : -mag);
}

void WriteRegionStrided(opj_sparse_array_int32_t* sa, OPJ_UINT32 x0,
                        OPJ_UINT32 y0, OPJ_UINT32 x1, OPJ_UINT32 y1,
                        OPJ_UINT32 col_stride, OPJ_UINT32 line_stride,
                        std::vector<OPJ_INT32>& grid, OPJ_UINT32 grid_width,
                        OPJ_INT32 bias = 0) {
  ASSERT_GT(x1, x0);
  ASSERT_GT(y1, y0);
  ASSERT_GE(col_stride, 1u);
  const size_t region_w = static_cast<size_t>(x1 - x0);
  const size_t region_h = static_cast<size_t>(y1 - y0);
  const size_t min_line_elems =
      (region_w - 1) * static_cast<size_t>(col_stride) + 1;
  ASSERT_GE(static_cast<size_t>(line_stride), min_line_elems);

  std::vector<OPJ_INT32> src(region_h * static_cast<size_t>(line_stride),
                             kSrcPoison);
  for (size_t r = 0; r < region_h; ++r) {
    for (size_t c = 0; c < region_w; ++c) {
      const OPJ_UINT32 x = x0 + static_cast<OPJ_UINT32>(c);
      const OPJ_UINT32 y = y0 + static_cast<OPJ_UINT32>(r);
      const OPJ_INT32 val = SampleValue(x, y, bias);
      src[r * static_cast<size_t>(line_stride) +
          c * static_cast<size_t>(col_stride)] = val;
      grid[static_cast<size_t>(y) * grid_width + x] = val;
    }
  }
  ASSERT_EQ(OPJ_TRUE,
            opj_sparse_array_int32_write(sa, x0, y0, x1, y1, src.data(),
                                         col_stride, line_stride, OPJ_FALSE));
}

void VerifyReadStrided(const opj_sparse_array_int32_t* sa, OPJ_UINT32 x0,
                       OPJ_UINT32 y0, OPJ_UINT32 x1, OPJ_UINT32 y1,
                       OPJ_UINT32 col_stride, OPJ_UINT32 line_stride,
                       std::span<const OPJ_INT32> grid, OPJ_UINT32 grid_width) {
  ASSERT_GT(x1, x0);
  ASSERT_GT(y1, y0);
  ASSERT_GE(col_stride, 1u);
  const size_t region_w = static_cast<size_t>(x1 - x0);
  const size_t region_h = static_cast<size_t>(y1 - y0);
  const size_t min_line_elems =
      (region_w - 1) * static_cast<size_t>(col_stride) + 1;
  ASSERT_GE(static_cast<size_t>(line_stride), min_line_elems);

  const size_t buf_size = region_h * static_cast<size_t>(line_stride);
  std::vector<OPJ_INT32> actual(buf_size, kDestSentinel);
  std::vector<OPJ_INT32> expected(buf_size, kDestSentinel);
  for (size_t r = 0; r < region_h; ++r) {
    for (size_t c = 0; c < region_w; ++c) {
      const OPJ_UINT32 x = x0 + static_cast<OPJ_UINT32>(c);
      const OPJ_UINT32 y = y0 + static_cast<OPJ_UINT32>(r);
      expected[r * static_cast<size_t>(line_stride) +
               c * static_cast<size_t>(col_stride)] =
          grid[static_cast<size_t>(y) * grid_width + x];
    }
  }
  ASSERT_EQ(OPJ_TRUE,
            opj_sparse_array_int32_read(sa, x0, y0, x1, y1, actual.data(),
                                        col_stride, line_stride, OPJ_FALSE));
  EXPECT_THAT(actual, ElementsAreArray(expected));
}

TEST(SparseArrayTest, FreeNullIsNoOp) { opj_sparse_array_int32_free(nullptr); }

TEST(SparseArrayTest, CreateZeroOrOverflowingDimensionsReturnsNull) {
  struct InvalidCreateCase {
    std::string_view name;
    OPJ_UINT32 width;
    OPJ_UINT32 height;
    OPJ_UINT32 block_width;
    OPJ_UINT32 block_height;
  };
  constexpr OPJ_UINT32 kMaxU32 = std::numeric_limits<OPJ_UINT32>::max();
  constexpr InvalidCreateCase kCases[] = {
      {.name = "zero_width",
       .width = 0,
       .height = 4,
       .block_width = 2,
       .block_height = 2},
      {.name = "zero_height",
       .width = 4,
       .height = 0,
       .block_width = 2,
       .block_height = 2},
      {.name = "zero_block_width",
       .width = 4,
       .height = 4,
       .block_width = 0,
       .block_height = 2},
      {.name = "zero_block_height",
       .width = 4,
       .height = 4,
       .block_width = 2,
       .block_height = 0},
      {.name = "all_zero",
       .width = 0,
       .height = 0,
       .block_width = 0,
       .block_height = 0},
      // block_width * block_height * sizeof(OPJ_INT32) overflows OPJ_UINT32.
      {.name = "block_bytes_overflow_wide",
       .width = 1,
       .height = 1,
       .block_width = 0x40000000U,
       .block_height = 1},
      {.name = "block_bytes_overflow_tall",
       .width = 1,
       .height = 1,
       .block_width = 1,
       .block_height = 0x40000000U},
      {.name = "block_bytes_overflow_square",
       .width = 1,
       .height = 1,
       .block_width = 65536,
       .block_height = 16384},
      {.name = "block_bytes_overflow_max",
       .width = 1,
       .height = 1,
       .block_width = kMaxU32,
       .block_height = kMaxU32},
      // block_count_hor * block_count_ver overflows OPJ_UINT32.
      {.name = "block_count_overflow_square",
       .width = 65536,
       .height = 65536,
       .block_width = 1,
       .block_height = 1},
      {.name = "block_count_overflow_max_width",
       .width = kMaxU32,
       .height = 2,
       .block_width = 1,
       .block_height = 1},
      {.name = "block_count_overflow_max_height",
       .width = 2,
       .height = kMaxU32,
       .block_width = 1,
       .block_height = 1},
      {.name = "block_count_overflow_tiled",
       .width = 1048576,
       .height = 1048576,
       .block_width = 16,
       .block_height = 16},
      {.name = "block_count_overflow_tiled_max",
       .width = kMaxU32,
       .height = kMaxU32,
       .block_width = 16384,
       .block_height = 16384},
  };

  for (const InvalidCreateCase& tc : kCases) {
    SCOPED_TRACE(std::string(tc.name));
    SparseArrayPtr sa(opj_sparse_array_int32_create(
        tc.width, tc.height, tc.block_width, tc.block_height));
    EXPECT_EQ(nullptr, sa.get());
  }
}

TEST(SparseArrayTest, CreateBoundaryAndLargeSparseArraySucceeds) {
  // 1. Minimal 1x1 array with 1x1 block.
  {
    SparseArrayPtr sa(opj_sparse_array_int32_create(1, 1, 1, 1));
    ASSERT_NE(nullptr, sa);
    OPJ_INT32 val = -42;
    ASSERT_EQ(OPJ_TRUE, opj_sparse_array_int32_write(sa.get(), 0, 0, 1, 1, &val,
                                                     1, 1, OPJ_FALSE));
    OPJ_INT32 out = kDestSentinel;
    ASSERT_EQ(OPJ_TRUE, opj_sparse_array_int32_read(sa.get(), 0, 0, 1, 1, &out,
                                                    1, 1, OPJ_FALSE));
    EXPECT_EQ(-42, out);
  }

  // 2. Block dimension exactly at the OPJ_UINT32 byte-size guard
  // (0xFFFFFFFFU / 1 / 4 == 0x3FFFFFFFU) succeeds without allocating blocks
  // until written.
  {
    SparseArrayPtr sa(opj_sparse_array_int32_create(1, 1, 0x3FFFFFFFU, 1));
    EXPECT_NE(nullptr, sa);
  }

  // 3. Block dimensions larger than array dimensions.
  {
    SparseArrayPtr sa(opj_sparse_array_int32_create(3, 2, 16, 8));
    ASSERT_NE(nullptr, sa);
    std::vector<OPJ_INT32> grid(3 * 2, 0);
    ASSERT_NO_FATAL_FAILURE(
        WriteRegionStrided(sa.get(), 1, 0, 3, 2, 1, 2, grid, 3));
    ASSERT_NO_FATAL_FAILURE(
        VerifyReadStrided(sa.get(), 0, 0, 3, 2, 1, 3, grid, 3));
  }

  // 4. Large sparse array where only one corner block is allocated.
  {
    constexpr OPJ_UINT32 kLargeDim = 16384;
    SparseArrayPtr sa(
        opj_sparse_array_int32_create(kLargeDim, kLargeDim, 64, 64));
    ASSERT_NE(nullptr, sa);

    constexpr OPJ_INT32 kPatch[] = {11, 22, 33, 44, 55, 66};
    ASSERT_EQ(OPJ_TRUE,
              opj_sparse_array_int32_write(sa.get(), 16380, 16381, 16383, 16383,
                                           kPatch, 1, 3, OPJ_FALSE));

    std::vector<OPJ_INT32> patch_out(6, kDestSentinel);
    ASSERT_EQ(OPJ_TRUE,
              opj_sparse_array_int32_read(sa.get(), 16380, 16381, 16383, 16383,
                                          patch_out.data(), 1, 3, OPJ_FALSE));
    EXPECT_THAT(patch_out, ElementsAre(11, 22, 33, 44, 55, 66));

    std::vector<OPJ_INT32> unset_out(16, kDestSentinel);
    ASSERT_EQ(OPJ_TRUE,
              opj_sparse_array_int32_read(sa.get(), 0, 0, 4, 4,
                                          unset_out.data(), 1, 4, OPJ_FALSE));
    EXPECT_THAT(unset_out, Each(0));
  }
}

TEST(SparseArrayTest, IsRegionValidChecksAllBounds) {
  constexpr OPJ_UINT32 kWidth = 10;
  constexpr OPJ_UINT32 kHeight = 8;
  SparseArrayPtr sa(opj_sparse_array_int32_create(kWidth, kHeight, 4, 3));
  ASSERT_NE(nullptr, sa);

  struct RegionValidCase {
    std::string_view name;
    OPJ_UINT32 x0;
    OPJ_UINT32 y0;
    OPJ_UINT32 x1;
    OPJ_UINT32 y1;
    OPJ_BOOL expected;
  };
  constexpr OPJ_UINT32 kMaxU32 = std::numeric_limits<OPJ_UINT32>::max();
  constexpr RegionValidCase kCases[] = {
      {.name = "full_array",
       .x0 = 0,
       .y0 = 0,
       .x1 = kWidth,
       .y1 = kHeight,
       .expected = OPJ_TRUE},
      {.name = "top_left_pixel",
       .x0 = 0,
       .y0 = 0,
       .x1 = 1,
       .y1 = 1,
       .expected = OPJ_TRUE},
      {.name = "bottom_right_pixel",
       .x0 = 9,
       .y0 = 7,
       .x1 = 10,
       .y1 = 8,
       .expected = OPJ_TRUE},
      {.name = "interior",
       .x0 = 3,
       .y0 = 2,
       .x1 = 7,
       .y1 = 6,
       .expected = OPJ_TRUE},
      {.name = "x0_at_width",
       .x0 = kWidth,
       .y0 = 0,
       .x1 = kWidth + 1,
       .y1 = 1,
       .expected = OPJ_FALSE},
      {.name = "x0_beyond_width",
       .x0 = 15,
       .y0 = 0,
       .x1 = 20,
       .y1 = 1,
       .expected = OPJ_FALSE},
      {.name = "x1_equals_x0_zero",
       .x0 = 0,
       .y0 = 0,
       .x1 = 0,
       .y1 = 4,
       .expected = OPJ_FALSE},
      {.name = "x1_equals_x0_nonzero",
       .x0 = 5,
       .y0 = 1,
       .x1 = 5,
       .y1 = 4,
       .expected = OPJ_FALSE},
      {.name = "x1_less_than_x0",
       .x0 = 6,
       .y0 = 0,
       .x1 = 4,
       .y1 = 4,
       .expected = OPJ_FALSE},
      {.name = "x1_beyond_width",
       .x0 = 0,
       .y0 = 0,
       .x1 = kWidth + 1,
       .y1 = 4,
       .expected = OPJ_FALSE},
      {.name = "y0_at_height",
       .x0 = 0,
       .y0 = kHeight,
       .x1 = 2,
       .y1 = kHeight + 1,
       .expected = OPJ_FALSE},
      {.name = "y0_beyond_height",
       .x0 = 0,
       .y0 = 12,
       .x1 = 2,
       .y1 = 15,
       .expected = OPJ_FALSE},
      {.name = "y1_equals_y0_zero",
       .x0 = 0,
       .y0 = 0,
       .x1 = 4,
       .y1 = 0,
       .expected = OPJ_FALSE},
      {.name = "y1_equals_y0_nonzero",
       .x0 = 0,
       .y0 = 3,
       .x1 = 4,
       .y1 = 3,
       .expected = OPJ_FALSE},
      {.name = "y1_less_than_y0",
       .x0 = 0,
       .y0 = 5,
       .x1 = 4,
       .y1 = 2,
       .expected = OPJ_FALSE},
      {.name = "y1_beyond_height",
       .x0 = 0,
       .y0 = 0,
       .x1 = 4,
       .y1 = kHeight + 1,
       .expected = OPJ_FALSE},
      {.name = "all_zero_empty",
       .x0 = 0,
       .y0 = 0,
       .x1 = 0,
       .y1 = 0,
       .expected = OPJ_FALSE},
      {.name = "x1_max_uint32",
       .x0 = 0,
       .y0 = 0,
       .x1 = kMaxU32,
       .y1 = 1,
       .expected = OPJ_FALSE},
  };

  for (const RegionValidCase& tc : kCases) {
    SCOPED_TRACE(std::string(tc.name));
    EXPECT_EQ(tc.expected, opj_sparse_array_is_region_valid(
                               sa.get(), tc.x0, tc.y0, tc.x1, tc.y1));
  }
}

TEST(SparseArrayTest, InvalidRegionsRespectForgivingFlag) {
  constexpr OPJ_UINT32 kWidth = 8;
  constexpr OPJ_UINT32 kHeight = 6;
  SparseArrayPtr sa(opj_sparse_array_int32_create(kWidth, kHeight, 4, 3));
  ASSERT_NE(nullptr, sa);

  std::vector<OPJ_INT32> grid(kWidth * kHeight, 0);
  ASSERT_NO_FATAL_FAILURE(
      WriteRegionStrided(sa.get(), 1, 1, 2, 2, 1, 1, grid, kWidth));

  struct InvalidRegionCase {
    std::string_view name;
    OPJ_UINT32 x0;
    OPJ_UINT32 y0;
    OPJ_UINT32 x1;
    OPJ_UINT32 y1;
  };
  constexpr InvalidRegionCase kCases[] = {
      {.name = "empty_x", .x0 = 2, .y0 = 1, .x1 = 2, .y1 = 3},
      {.name = "empty_y", .x0 = 1, .y0 = 3, .x1 = 4, .y1 = 3},
      {.name = "empty_origin", .x0 = 0, .y0 = 0, .x1 = 0, .y1 = 0},
      {.name = "inverted_x", .x0 = 4, .y0 = 1, .x1 = 2, .y1 = 3},
      {.name = "inverted_y", .x0 = 1, .y0 = 4, .x1 = 3, .y1 = 2},
      {.name = "oob_x1", .x0 = 4, .y0 = 0, .x1 = kWidth + 1, .y1 = 3},
      {.name = "oob_y1", .x0 = 0, .y0 = 3, .x1 = 4, .y1 = kHeight + 1},
      {.name = "oob_x0", .x0 = kWidth, .y0 = 0, .x1 = kWidth + 2, .y1 = 2},
      {.name = "oob_y0", .x0 = 0, .y0 = kHeight, .x1 = 2, .y1 = kHeight + 2},
  };

  for (const InvalidRegionCase& tc : kCases) {
    SCOPED_TRACE(std::string(tc.name));
    std::vector<OPJ_INT32> buf(16, kDestSentinel);

    // Invalid writes return `forgiving` without modifying the array.
    EXPECT_EQ(OPJ_FALSE,
              opj_sparse_array_int32_write(sa.get(), tc.x0, tc.y0, tc.x1, tc.y1,
                                           buf.data(), 1, 4, OPJ_FALSE));
    EXPECT_EQ(OPJ_TRUE,
              opj_sparse_array_int32_write(sa.get(), tc.x0, tc.y0, tc.x1, tc.y1,
                                           buf.data(), 1, 4, OPJ_TRUE));
    EXPECT_EQ(OPJ_FALSE,
              opj_sparse_array_int32_write(sa.get(), tc.x0, tc.y0, tc.x1, tc.y1,
                                           buf.data(), 0, 0, OPJ_FALSE));
    EXPECT_EQ(OPJ_TRUE,
              opj_sparse_array_int32_write(sa.get(), tc.x0, tc.y0, tc.x1, tc.y1,
                                           buf.data(), 0, 0, OPJ_TRUE));
    EXPECT_EQ(OPJ_FALSE,
              opj_sparse_array_int32_write(sa.get(), tc.x0, tc.y0, tc.x1, tc.y1,
                                           nullptr, 1, 1, OPJ_FALSE));
    EXPECT_EQ(OPJ_TRUE,
              opj_sparse_array_int32_write(sa.get(), tc.x0, tc.y0, tc.x1, tc.y1,
                                           nullptr, 1, 1, OPJ_TRUE));

    // Invalid reads return `forgiving` without modifying `buf`.
    EXPECT_EQ(OPJ_FALSE,
              opj_sparse_array_int32_read(sa.get(), tc.x0, tc.y0, tc.x1, tc.y1,
                                          buf.data(), 1, 4, OPJ_FALSE));
    EXPECT_THAT(buf, Each(kDestSentinel));

    EXPECT_EQ(OPJ_TRUE,
              opj_sparse_array_int32_read(sa.get(), tc.x0, tc.y0, tc.x1, tc.y1,
                                          buf.data(), 1, 4, OPJ_TRUE));
    EXPECT_THAT(buf, Each(kDestSentinel));

    EXPECT_EQ(OPJ_FALSE,
              opj_sparse_array_int32_read(sa.get(), tc.x0, tc.y0, tc.x1, tc.y1,
                                          buf.data(), 0, 0, OPJ_FALSE));
    EXPECT_THAT(buf, Each(kDestSentinel));

    EXPECT_EQ(OPJ_TRUE,
              opj_sparse_array_int32_read(sa.get(), tc.x0, tc.y0, tc.x1, tc.y1,
                                          buf.data(), 0, 0, OPJ_TRUE));
    EXPECT_THAT(buf, Each(kDestSentinel));

    EXPECT_EQ(OPJ_FALSE,
              opj_sparse_array_int32_read(sa.get(), tc.x0, tc.y0, tc.x1, tc.y1,
                                          nullptr, 1, 1, OPJ_FALSE));
    EXPECT_EQ(OPJ_TRUE,
              opj_sparse_array_int32_read(sa.get(), tc.x0, tc.y0, tc.x1, tc.y1,
                                          nullptr, 1, 1, OPJ_TRUE));
  }

  // Confirm none of the invalid writes altered the existing array state.
  ASSERT_NO_FATAL_FAILURE(VerifyReadStrided(sa.get(), 0, 0, kWidth, kHeight, 1,
                                            kWidth, grid, kWidth));
}

TEST(SparseArrayTest, ReadUnsetArrayZeroFillsAcrossStrideModes) {
  constexpr OPJ_UINT32 kWidth = 10;
  constexpr OPJ_UINT32 kHeight = 7;
  SparseArrayPtr sa(opj_sparse_array_int32_create(kWidth, kHeight, 4, 3));
  ASSERT_NE(nullptr, sa);

  const std::vector<OPJ_INT32> zero_grid(kWidth * kHeight, 0);

  // col_stride == 1 (memset path) with contiguous and padded row strides.
  ASSERT_NO_FATAL_FAILURE(VerifyReadStrided(sa.get(), 0, 0, kWidth, kHeight, 1,
                                            kWidth, zero_grid, kWidth));
  ASSERT_NO_FATAL_FAILURE(
      VerifyReadStrided(sa.get(), 1, 1, 9, 6, 1, 11, zero_grid, kWidth));

  // col_stride > 1 (element-wise zero-fill path) with gap & padding checks.
  ASSERT_NO_FATAL_FAILURE(
      VerifyReadStrided(sa.get(), 1, 1, 9, 6, 2, 18, zero_grid, kWidth));
  ASSERT_NO_FATAL_FAILURE(VerifyReadStrided(sa.get(), 0, 0, kWidth, kHeight, 3,
                                            31, zero_grid, kWidth));

  // col_stride == 0 (writes zeros at line_stride intervals only).
  std::vector<OPJ_INT32> zero_col_stride_buf(5 * 2, kDestSentinel);
  ASSERT_EQ(OPJ_TRUE, opj_sparse_array_int32_read(sa.get(), 1, 1, 9, 6,
                                                  zero_col_stride_buf.data(), 0,
                                                  2, OPJ_FALSE));
  EXPECT_THAT(zero_col_stride_buf,
              ElementsAre(0, kDestSentinel, 0, kDestSentinel, 0, kDestSentinel,
                          0, kDestSentinel, 0, kDestSentinel));
}

TEST(SparseArrayTest, ContiguousWriteAndReadFullAndPartialBlocks) {
  constexpr OPJ_UINT32 kWidth = 12;
  constexpr OPJ_UINT32 kHeight = 7;
  constexpr OPJ_UINT32 kBlockW = 4;
  constexpr OPJ_UINT32 kBlockH = 3;

  // 1. Full-array write/read where every block slice has x_incr == 4.
  {
    SparseArrayPtr sa(
        opj_sparse_array_int32_create(kWidth, kHeight, kBlockW, kBlockH));
    ASSERT_NE(nullptr, sa);
    std::vector<OPJ_INT32> grid(kWidth * kHeight, 0);
    ASSERT_NO_FATAL_FAILURE(WriteRegionStrided(sa.get(), 0, 0, kWidth, kHeight,
                                               1, kWidth + 3, grid, kWidth));
    ASSERT_NO_FATAL_FAILURE(VerifyReadStrided(sa.get(), 0, 0, kWidth, kHeight,
                                              1, kWidth, grid, kWidth));
    ASSERT_NO_FATAL_FAILURE(VerifyReadStrided(sa.get(), 0, 0, kWidth, kHeight,
                                              1, kWidth + 4, grid, kWidth));
  }

  // 2. Unaligned multi-block partial write/read exercising x_incr == 4 (middle
  // block) and x_incr != 4 (leading/trailing partial blocks), plus untouched
  // border samples within allocated blocks remaining 0.
  {
    SparseArrayPtr sa(
        opj_sparse_array_int32_create(kWidth, kHeight, kBlockW, kBlockH));
    ASSERT_NE(nullptr, sa);
    std::vector<OPJ_INT32> grid(kWidth * kHeight, 0);
    ASSERT_NO_FATAL_FAILURE(
        WriteRegionStrided(sa.get(), 1, 1, 11, 7, 1, 12, grid, kWidth));
    ASSERT_NO_FATAL_FAILURE(VerifyReadStrided(sa.get(), 0, 0, kWidth, kHeight,
                                              1, kWidth, grid, kWidth));
    ASSERT_NO_FATAL_FAILURE(
        VerifyReadStrided(sa.get(), 1, 1, 11, 7, 1, 10, grid, kWidth));
    ASSERT_NO_FATAL_FAILURE(
        VerifyReadStrided(sa.get(), 2, 2, 10, 5, 1, 9, grid, kWidth));
  }

  // 3. Disjoint and overlapping partial writes within the same single block.
  {
    SparseArrayPtr sa(opj_sparse_array_int32_create(6, 5, 6, 5));
    ASSERT_NE(nullptr, sa);
    std::vector<OPJ_INT32> grid(6 * 5, 0);

    ASSERT_NO_FATAL_FAILURE(
        WriteRegionStrided(sa.get(), 1, 1, 3, 3, 1, 2, grid, 6, /*bias=*/0));
    ASSERT_NO_FATAL_FAILURE(
        VerifyReadStrided(sa.get(), 0, 0, 6, 5, 1, 6, grid, 6));

    ASSERT_NO_FATAL_FAILURE(WriteRegionStrided(sa.get(), 2, 1, 5, 4, 1, 4, grid,
                                               6, /*bias=*/500000));
    ASSERT_NO_FATAL_FAILURE(
        VerifyReadStrided(sa.get(), 0, 0, 6, 5, 1, 6, grid, 6));
  }
}

TEST(SparseArrayTest, PartialSparseBlocksMixAllocatedAndUnallocated) {
  constexpr OPJ_UINT32 kWidth = 12;
  constexpr OPJ_UINT32 kHeight = 9;
  SparseArrayPtr sa(opj_sparse_array_int32_create(kWidth, kHeight, 4, 3));
  ASSERT_NE(nullptr, sa);
  std::vector<OPJ_INT32> grid(kWidth * kHeight, 0);

  // Allocate only blocks (0,0), (2,0), (1,1), and (0,2); leave the other 5
  // blocks NULL.
  ASSERT_NO_FATAL_FAILURE(
      WriteRegionStrided(sa.get(), 0, 0, 4, 3, 1, 4, grid, kWidth));
  ASSERT_NO_FATAL_FAILURE(
      WriteRegionStrided(sa.get(), 9, 1, 11, 3, 1, 2, grid, kWidth));
  ASSERT_NO_FATAL_FAILURE(
      WriteRegionStrided(sa.get(), 4, 4, 8, 5, 1, 4, grid, kWidth));
  ASSERT_NO_FATAL_FAILURE(
      WriteRegionStrided(sa.get(), 1, 6, 3, 9, 1, 2, grid, kWidth));

  // Overwrite an interior slice of already-allocated block (1,1).
  ASSERT_NO_FATAL_FAILURE(WriteRegionStrided(sa.get(), 5, 4, 7, 5, 1, 2, grid,
                                             kWidth,
                                             /*bias=*/500000));

  // Read across mixed allocated and unallocated blocks with contiguous and
  // strided buffers.
  ASSERT_NO_FATAL_FAILURE(VerifyReadStrided(sa.get(), 0, 0, kWidth, kHeight, 1,
                                            kWidth, grid, kWidth));
  ASSERT_NO_FATAL_FAILURE(
      VerifyReadStrided(sa.get(), 2, 1, 10, 8, 1, 9, grid, kWidth));
  ASSERT_NO_FATAL_FAILURE(
      VerifyReadStrided(sa.get(), 2, 1, 10, 8, 3, 25, grid, kWidth));
}

TEST(SparseArrayTest, StridedWriteAndReadSingleColumnAndGeneralStrides) {
  constexpr OPJ_UINT32 kWidth = 10;
  constexpr OPJ_UINT32 kHeight = 8;
  SparseArrayPtr sa(opj_sparse_array_int32_create(kWidth, kHeight, 4, 3));
  ASSERT_NE(nullptr, sa);
  std::vector<OPJ_INT32> grid(kWidth * kHeight, 0);

  // 1. x_incr == 1 with buf_col_stride != 1 (single-column multi-block).
  ASSERT_NO_FATAL_FAILURE(
      WriteRegionStrided(sa.get(), 3, 0, 4, 7, 3, 5, grid, kWidth));
  ASSERT_NO_FATAL_FAILURE(
      VerifyReadStrided(sa.get(), 3, 0, 4, 7, 3, 5, grid, kWidth));

  // 2. Multi-block x=[3,9) with col_stride == 2: hits x_incr == 1 on leading
  // [3,4) and trailing [8,9) slices, and general strided path on middle [4,8)
  // slice (y_incr > 1).
  ASSERT_NO_FATAL_FAILURE(WriteRegionStrided(sa.get(), 3, 1, 9, 7, 2, 13, grid,
                                             kWidth,
                                             /*bias=*/100000));
  ASSERT_NO_FATAL_FAILURE(
      VerifyReadStrided(sa.get(), 3, 1, 9, 7, 2, 13, grid, kWidth));

  // 3. General strided path with col_stride == 3 across multiple blocks.
  ASSERT_NO_FATAL_FAILURE(WriteRegionStrided(sa.get(), 1, 0, 9, 5, 3, 25, grid,
                                             kWidth,
                                             /*bias=*/200000));
  ASSERT_NO_FATAL_FAILURE(
      VerifyReadStrided(sa.get(), 1, 0, 9, 5, 3, 25, grid, kWidth));

  // 4. col_stride == 8 with x_incr < 8 (5 < 8), verifying fallback from the
  // x_incr >= 8 && col_stride == 8 fast path to the general loop.
  ASSERT_NO_FATAL_FAILURE(
      VerifyReadStrided(sa.get(), 1, 1, 6, 4, 8, 42, grid, kWidth));
  ASSERT_NO_FATAL_FAILURE(VerifyReadStrided(sa.get(), 0, 0, kWidth, kHeight, 1,
                                            kWidth, grid, kWidth));
}

TEST(SparseArrayTest, ReadSingleRowColStrideTwoFastPath) {
  // 1. Single-block fast path `y_incr == 1 && buf_col_stride == 2` across
  // sub-4, exact-4, and 4*n + rem widths.
  {
    constexpr OPJ_UINT32 kWidth = 16;
    constexpr OPJ_UINT32 kHeight = 4;
    SparseArrayPtr sa(opj_sparse_array_int32_create(kWidth, kHeight, 16, 4));
    ASSERT_NE(nullptr, sa);
    std::vector<OPJ_INT32> grid(kWidth * kHeight, 0);
    ASSERT_NO_FATAL_FAILURE(WriteRegionStrided(sa.get(), 0, 0, kWidth, kHeight,
                                               1, kWidth, grid, kWidth));

    // x_incr == 2 (tail only)
    ASSERT_NO_FATAL_FAILURE(
        VerifyReadStrided(sa.get(), 0, 0, 2, 1, 2, 4, grid, kWidth));
    // x_incr == 3 (tail only)
    ASSERT_NO_FATAL_FAILURE(
        VerifyReadStrided(sa.get(), 1, 1, 4, 2, 2, 7, grid, kWidth));
    // x_incr == 4 (unrolled loop only)
    ASSERT_NO_FATAL_FAILURE(
        VerifyReadStrided(sa.get(), 0, 2, 4, 3, 2, 8, grid, kWidth));
    // x_incr == 7 (unrolled loop + 3-element tail)
    ASSERT_NO_FATAL_FAILURE(
        VerifyReadStrided(sa.get(), 2, 0, 9, 1, 2, 15, grid, kWidth));
    // x_incr == 10 (two unrolled iterations + 2-element tail)
    ASSERT_NO_FATAL_FAILURE(
        VerifyReadStrided(sa.get(), 3, 3, 13, 4, 2, 21, grid, kWidth));

    // Multi-row read (y_incr == 2) with col_stride == 2 uses the general loop.
    ASSERT_NO_FATAL_FAILURE(
        VerifyReadStrided(sa.get(), 1, 1, 10, 3, 2, 19, grid, kWidth));
  }

  // 2. Block height == 1 forces y_incr == 1 across multiple horizontal blocks.
  {
    constexpr OPJ_UINT32 kWidth = 14;
    constexpr OPJ_UINT32 kHeight = 3;
    SparseArrayPtr sa(opj_sparse_array_int32_create(kWidth, kHeight, 6, 1));
    ASSERT_NE(nullptr, sa);
    std::vector<OPJ_INT32> grid(kWidth * kHeight, 0);
    ASSERT_NO_FATAL_FAILURE(WriteRegionStrided(sa.get(), 0, 0, kWidth, kHeight,
                                               1, kWidth, grid, kWidth));

    // Slices: [1,6) -> x_incr=5, [6,12) -> x_incr=6, [12,13) -> x_incr=1.
    ASSERT_NO_FATAL_FAILURE(
        VerifyReadStrided(sa.get(), 1, 1, 13, 2, 2, 25, grid, kWidth));
  }
}

TEST(SparseArrayTest, StridedEightFastPathWriteAndRead) {
  constexpr OPJ_UINT32 kWidth = 24;
  constexpr OPJ_UINT32 kHeight = 6;
  SparseArrayPtr sa(opj_sparse_array_int32_create(kWidth, kHeight, 16, 4));
  ASSERT_NE(nullptr, sa);
  std::vector<OPJ_INT32> grid(kWidth * kHeight, 0);

  // 1. Exact multiple of 4 (x_incr == 8, y_incr == 3).
  ASSERT_NO_FATAL_FAILURE(
      WriteRegionStrided(sa.get(), 0, 0, 8, 3, 8, 65, grid, kWidth));
  ASSERT_NO_FATAL_FAILURE(
      VerifyReadStrided(sa.get(), 0, 0, 8, 3, 8, 65, grid, kWidth));

  // 2. Non-multiple of 4 (x_incr == 11, exercises k = 8..10 tail loop).
  ASSERT_NO_FATAL_FAILURE(WriteRegionStrided(sa.get(), 0, 0, 11, 4, 8, 90, grid,
                                             kWidth,
                                             /*bias=*/100000));
  ASSERT_NO_FATAL_FAILURE(
      VerifyReadStrided(sa.get(), 0, 0, 11, 4, 8, 90, grid, kWidth));

  // 3. Multi-block x=[4,24): x_incr == 12 then x_incr == 8, y_incr == 3 then 2.
  ASSERT_NO_FATAL_FAILURE(WriteRegionStrided(sa.get(), 4, 1, 24, 6, 8, 160,
                                             grid, kWidth,
                                             /*bias=*/200000));
  ASSERT_NO_FATAL_FAILURE(
      VerifyReadStrided(sa.get(), 4, 1, 24, 6, 8, 160, grid, kWidth));

  // 4. Multi-block x=[5,21): x_incr == 11 (fast path + tail) then x_incr == 5
  // (falls back to general loop on the second block).
  ASSERT_NO_FATAL_FAILURE(WriteRegionStrided(sa.get(), 5, 0, 21, 2, 8, 129,
                                             grid, kWidth,
                                             /*bias=*/300000));
  ASSERT_NO_FATAL_FAILURE(
      VerifyReadStrided(sa.get(), 5, 0, 21, 2, 8, 129, grid, kWidth));

  // Verify the entire array contiguously as well.
  ASSERT_NO_FATAL_FAILURE(VerifyReadStrided(sa.get(), 0, 0, kWidth, kHeight, 1,
                                            kWidth, grid, kWidth));
}

TEST(SparseArrayTest, ZeroStrideWriteBroadcastAndRead) {
  constexpr OPJ_UINT32 kWidth = 10;
  constexpr OPJ_UINT32 kHeight = 7;
  SparseArrayPtr sa(opj_sparse_array_int32_create(kWidth, kHeight, 4, 3));
  ASSERT_NE(nullptr, sa);
  std::vector<OPJ_INT32> grid(kWidth * kHeight, 0);

  // 1. Scalar broadcast (src_col_stride == 0, src_line_stride == 0) across
  // multiple blocks, also exercising forgiving == OPJ_TRUE on a valid call.
  constexpr OPJ_INT32 kFillValue = 42;
  ASSERT_EQ(OPJ_TRUE, opj_sparse_array_int32_write(
                          sa.get(), 1, 1, 9, 6, &kFillValue, 0, 0, OPJ_TRUE));
  for (OPJ_UINT32 y = 1; y < 6; ++y) {
    for (OPJ_UINT32 x = 1; x < 9; ++x) {
      grid[static_cast<size_t>(y) * kWidth + x] = kFillValue;
    }
  }
  ASSERT_NO_FATAL_FAILURE(VerifyReadStrided(sa.get(), 0, 0, kWidth, kHeight, 1,
                                            kWidth, grid, kWidth));

  // 2. Per-row broadcast (src_col_stride == 0, src_line_stride == 2) starting
  // at x0 == 3 so the first slice has x_incr == 1 and the last slice has x_incr
  // == 1.
  constexpr OPJ_INT32 kRowValues[] = {10, kSrcPoison, 20, kSrcPoison,
                                      30, kSrcPoison, 40, kSrcPoison};
  ASSERT_EQ(OPJ_TRUE, opj_sparse_array_int32_write(
                          sa.get(), 3, 2, 9, 6, kRowValues, 0, 2, OPJ_FALSE));
  for (OPJ_UINT32 y = 2; y < 6; ++y) {
    for (OPJ_UINT32 x = 3; x < 9; ++x) {
      grid[static_cast<size_t>(y) * kWidth + x] = kRowValues[(y - 2) * 2];
    }
  }
  ASSERT_NO_FATAL_FAILURE(VerifyReadStrided(sa.get(), 0, 0, kWidth, kHeight, 1,
                                            kWidth, grid, kWidth));

  // 3. Single-row vertical broadcast (src_col_stride == 1, src_line_stride ==
  // 0).
  constexpr OPJ_INT32 kColPattern[] = {101, 102, 103, 104, 105};
  ASSERT_EQ(OPJ_TRUE, opj_sparse_array_int32_write(
                          sa.get(), 2, 0, 7, 4, kColPattern, 1, 0, OPJ_FALSE));
  for (OPJ_UINT32 y = 0; y < 4; ++y) {
    for (OPJ_UINT32 x = 2; x < 7; ++x) {
      grid[static_cast<size_t>(y) * kWidth + x] = kColPattern[x - 2];
    }
  }
  ASSERT_NO_FATAL_FAILURE(VerifyReadStrided(sa.get(), 0, 0, kWidth, kHeight, 1,
                                            kWidth, grid, kWidth));

  // 4. Zero col_stride read over populated blocks writes each row's rightmost
  // sample at line_stride spacing without touching adjacent buffer elements.
  ASSERT_NO_FATAL_FAILURE(WriteRegionStrided(sa.get(), 0, 0, kWidth, kHeight, 1,
                                             kWidth, grid, kWidth));

  std::vector<OPJ_INT32> zero_col_read(4 * 2, kDestSentinel);
  // Ends at x == 9 (final slice [8,9) hits x_incr == 1).
  ASSERT_EQ(OPJ_TRUE,
            opj_sparse_array_int32_read(sa.get(), 1, 0, 9, 4,
                                        zero_col_read.data(), 0, 2, OPJ_TRUE));
  EXPECT_THAT(zero_col_read,
              ElementsAre(SampleValue(8, 0), kDestSentinel, SampleValue(8, 1),
                          kDestSentinel, SampleValue(8, 2), kDestSentinel,
                          SampleValue(8, 3), kDestSentinel));

  // Ends at x == 7 (final slice [4,7) hits general case x_incr == 3).
  std::fill(zero_col_read.begin(), zero_col_read.end(), kDestSentinel);
  ASSERT_EQ(OPJ_TRUE,
            opj_sparse_array_int32_read(sa.get(), 1, 0, 7, 4,
                                        zero_col_read.data(), 0, 2, OPJ_FALSE));
  EXPECT_THAT(zero_col_read,
              ElementsAre(SampleValue(6, 0), kDestSentinel, SampleValue(6, 1),
                          kDestSentinel, SampleValue(6, 2), kDestSentinel,
                          SampleValue(6, 3), kDestSentinel));
}

TEST(SparseArrayTest, ExtremeInt32ValuesRoundTrip) {
  constexpr OPJ_UINT32 kWidth = 5;
  constexpr OPJ_UINT32 kHeight = 4;
  SparseArrayPtr sa(opj_sparse_array_int32_create(kWidth, kHeight, 3, 2));
  ASSERT_NE(nullptr, sa);

  constexpr OPJ_INT32 kMin = std::numeric_limits<OPJ_INT32>::min();
  constexpr OPJ_INT32 kMax = std::numeric_limits<OPJ_INT32>::max();
  constexpr std::array<OPJ_INT32, 4 * 3> kValues = {
      kMin, kMin + 1, -1, 0,        1,  kMax - 1,
      kMax, -123456,  42, kMin + 7, -2, kMax - 7,
  };

  // Pack with col_stride == 2 to test negative/extreme values under striding.
  std::vector<OPJ_INT32> src(4 * 6, kSrcPoison);
  for (size_t r = 0; r < 4; ++r) {
    for (size_t c = 0; c < 3; ++c) {
      src[r * 6 + c * 2] = kValues[r * 3 + c];
    }
  }
  ASSERT_EQ(OPJ_TRUE, opj_sparse_array_int32_write(
                          sa.get(), 1, 0, 4, 4, src.data(), 2, 6, OPJ_FALSE));

  std::vector<OPJ_INT32> actual(4 * 3, kDestSentinel);
  ASSERT_EQ(OPJ_TRUE,
            opj_sparse_array_int32_read(sa.get(), 1, 0, 4, 4, actual.data(), 1,
                                        3, OPJ_FALSE));
  EXPECT_THAT(actual, ElementsAreArray(kValues));
}

TEST(SparseArrayTest, OneByOneBlockGeometryExercisesEveryBlockStep) {
  constexpr OPJ_UINT32 kWidth = 5;
  constexpr OPJ_UINT32 kHeight = 4;
  SparseArrayPtr sa(opj_sparse_array_int32_create(kWidth, kHeight, 1, 1));
  ASSERT_NE(nullptr, sa);
  std::vector<OPJ_INT32> grid(kWidth * kHeight, 0);

  ASSERT_NO_FATAL_FAILURE(
      WriteRegionStrided(sa.get(), 1, 1, 4, 4, 1, 3, grid, kWidth));
  ASSERT_NO_FATAL_FAILURE(WriteRegionStrided(sa.get(), 2, 2, 3, 3, 2, 2, grid,
                                             kWidth,
                                             /*bias=*/10000));

  ASSERT_NO_FATAL_FAILURE(VerifyReadStrided(sa.get(), 0, 0, kWidth, kHeight, 1,
                                            kWidth, grid, kWidth));
  ASSERT_NO_FATAL_FAILURE(VerifyReadStrided(sa.get(), 0, 0, kWidth, kHeight, 2,
                                            kWidth * 2, grid, kWidth));
  ASSERT_NO_FATAL_FAILURE(
      VerifyReadStrided(sa.get(), 1, 1, 4, 4, 8, 25, grid, kWidth));
}

// TODO(schwehr): Cover remaining OOM allocation-failure return paths
// (`sa->data_blocks == NULL` in opj_sparse_array_int32_create() and
// `src_block == NULL` in opj_sparse_array_int32_read_or_write()) via
// :openjpeg2_internal_no_malloc and ScopedOpjAllocFailCountdown.

}  // namespace
}  // namespace openjpeg2
