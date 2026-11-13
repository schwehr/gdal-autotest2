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
// Unit tests for libharp dimension masks (libharp/harp-dimension-mask.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-dimension-mask.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-dimension-mask.h

#include <array>
#include <cstdint>
#include <cstdlib>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/oom_fault_inject.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::ElementsAreArray;

TEST(HarpDimensionMaskTest, NewAndDeleteZeroOneTwoDimensions) {
  // 1. Delete NULL is a safe no-op, and deleting a struct with NULL mask works.
  harp_dimension_mask_delete(nullptr);
  auto* manual = static_cast<harp_dimension_mask*>(
      std::malloc(sizeof(harp_dimension_mask)));
  ASSERT_NE(nullptr, manual);
  manual->num_dimensions = 0;
  manual->num_elements = 0;
  manual->masked_dimension_length = 0;
  manual->mask = nullptr;
  harp_dimension_mask_delete(manual);

  // 2. 0-D mask has 1 element initialized to 1.
  harp_dimension_mask* raw_0d = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_new(0, nullptr, &raw_0d));
  ScopedHarpDimensionMask mask_0d(raw_0d);
  EXPECT_EQ(0, mask_0d->num_dimensions);
  EXPECT_EQ(1L, mask_0d->num_elements);
  EXPECT_EQ(1L, mask_0d->masked_dimension_length);
  EXPECT_EQ(1, mask_0d->mask[0]);

  // 3. 1-D mask initializes all elements to 1.
  constexpr long kDims1D[] = {4};
  harp_dimension_mask* raw_1d = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_new(1, kDims1D, &raw_1d));
  ScopedHarpDimensionMask mask_1d(raw_1d);
  EXPECT_EQ(1, mask_1d->num_dimensions);
  EXPECT_EQ(4L, mask_1d->dimension[0]);
  EXPECT_EQ(4L, mask_1d->num_elements);
  EXPECT_EQ(4L, mask_1d->masked_dimension_length);
  for (long i = 0; i < 4; ++i) {
    EXPECT_EQ(1, mask_1d->mask[i]);
  }

  // 4. 2-D mask initializes all elements to 1 and sets masked_dimension_length
  // to the innermost dimension.
  constexpr long kDims2D[] = {3, 5};
  harp_dimension_mask* raw_2d = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_new(2, kDims2D, &raw_2d));
  ScopedHarpDimensionMask mask_2d(raw_2d);
  EXPECT_EQ(2, mask_2d->num_dimensions);
  EXPECT_EQ(3L, mask_2d->dimension[0]);
  EXPECT_EQ(5L, mask_2d->dimension[1]);
  EXPECT_EQ(15L, mask_2d->num_elements);
  EXPECT_EQ(5L, mask_2d->masked_dimension_length);
  for (long i = 0; i < 15; ++i) {
    EXPECT_EQ(1, mask_2d->mask[i]);
  }

  // 5. Zero-length dimension allocates 0 elements cleanly.
  constexpr long kDimsEmpty[] = {0};
  harp_dimension_mask* raw_empty = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_new(1, kDimsEmpty, &raw_empty));
  ScopedHarpDimensionMask mask_empty(raw_empty);
  EXPECT_EQ(0L, mask_empty->num_elements);
  EXPECT_EQ(0L, mask_empty->masked_dimension_length);
}

TEST(HarpDimensionMaskTest, CopyPreservesDimensionsAndIndependence) {
  constexpr long kDims[] = {2, 3};
  harp_dimension_mask* raw_src = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_new(2, kDims, &raw_src));
  ScopedHarpDimensionMask src(raw_src);
  src->mask[1] = 0;
  src->mask[4] = 0;
  src->mask[5] = 0;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_update_masked_length(src.get()));
  EXPECT_EQ(2L, src->masked_dimension_length);

  harp_dimension_mask* raw_copy = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_copy(src.get(), &raw_copy));
  ScopedHarpDimensionMask copy(raw_copy);
  EXPECT_EQ(2, copy->num_dimensions);
  EXPECT_EQ(2L, copy->dimension[0]);
  EXPECT_EQ(3L, copy->dimension[1]);
  EXPECT_EQ(6L, copy->num_elements);
  EXPECT_EQ(2L, copy->masked_dimension_length);
  EXPECT_THAT(std::vector<uint8_t>(copy->mask, copy->mask + 6),
              ElementsAreArray({1, 0, 1, 1, 0, 0}));

  // Mutating copy does not affect src.
  copy->mask[0] = 0;
  EXPECT_EQ(1, src->mask[0]);
}

TEST(HarpDimensionMaskTest, SetNewAndDelete) {
  harp_dimension_mask_set_delete(nullptr);

  harp_dimension_mask_set* raw_set = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_set_new(&raw_set));
  ScopedHarpDimensionMaskSet mask_set(raw_set);
  for (int i = 0; i < HARP_NUM_DIM_TYPES; ++i) {
    EXPECT_EQ(nullptr, mask_set.get()[i]);
  }

  constexpr long kDims[] = {3};
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_new(
                              1, kDims, &mask_set.get()[harp_dimension_time]));
  ASSERT_NE(nullptr, mask_set.get()[harp_dimension_time]);
}

TEST(HarpDimensionMaskTest, FillTrueAndFillFalse) {
  constexpr long kDims[] = {2, 3};
  harp_dimension_mask* raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_new(2, kDims, &raw));
  ScopedHarpDimensionMask mask(raw);

  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_fill_false(mask.get()));
  EXPECT_EQ(0L, mask->masked_dimension_length);
  EXPECT_THAT(std::vector<uint8_t>(mask->mask, mask->mask + 6),
              ElementsAreArray({0, 0, 0, 0, 0, 0}));

  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_fill_true(mask.get()));
  EXPECT_EQ(3L, mask->masked_dimension_length);
  EXPECT_THAT(std::vector<uint8_t>(mask->mask, mask->mask + 6),
              ElementsAreArray({1, 1, 1, 1, 1, 1}));
}

TEST(HarpDimensionMaskTest, UpdateMaskedLengthZeroOneTwoDims) {
  // 0-D true and false.
  harp_dimension_mask* raw_0d = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_new(0, nullptr, &raw_0d));
  ScopedHarpDimensionMask mask_0d(raw_0d);
  ASSERT_EQ(HARP_SUCCESS,
            harp_dimension_mask_update_masked_length(mask_0d.get()));
  EXPECT_EQ(1L, mask_0d->masked_dimension_length);
  mask_0d->mask[0] = 0;
  ASSERT_EQ(HARP_SUCCESS,
            harp_dimension_mask_update_masked_length(mask_0d.get()));
  EXPECT_EQ(0L, mask_0d->masked_dimension_length);

  // 1-D mixed.
  constexpr long kDims1D[] = {5};
  harp_dimension_mask* raw_1d = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_new(1, kDims1D, &raw_1d));
  ScopedHarpDimensionMask mask_1d(raw_1d);
  mask_1d->mask[1] = 0;
  mask_1d->mask[3] = 0;
  ASSERT_EQ(HARP_SUCCESS,
            harp_dimension_mask_update_masked_length(mask_1d.get()));
  EXPECT_EQ(3L, mask_1d->masked_dimension_length);

  // 2-D picks maximum active count across outer blocks.
  constexpr long kDims2D[] = {3, 4};
  harp_dimension_mask* raw_2d = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_new(2, kDims2D, &raw_2d));
  ScopedHarpDimensionMask mask_2d(raw_2d);
  // Row 0: 1 active, Row 1: 3 active, Row 2: 2 active -> max = 3.
  const std::array<uint8_t, 12> pattern = {
      1, 0, 0, 0, 1, 0, 1, 1, 0, 1, 0, 1,
  };
  for (size_t i = 0; i < pattern.size(); ++i) {
    mask_2d->mask[i] = pattern[i];
  }
  ASSERT_EQ(HARP_SUCCESS,
            harp_dimension_mask_update_masked_length(mask_2d.get()));
  EXPECT_EQ(3L, mask_2d->masked_dimension_length);

  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_fill_false(mask_2d.get()));
  ASSERT_EQ(HARP_SUCCESS,
            harp_dimension_mask_update_masked_length(mask_2d.get()));
  EXPECT_EQ(0L, mask_2d->masked_dimension_length);
}

TEST(HarpDimensionMaskTest, OuterProductActiveAndAllFalseRows) {
  constexpr long kRowDims[] = {3};
  constexpr long kColDims[] = {4};
  harp_dimension_mask* raw_row = nullptr;
  harp_dimension_mask* raw_col = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_new(1, kRowDims, &raw_row));
  ScopedHarpDimensionMask row_mask(raw_row);
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_new(1, kColDims, &raw_col));
  ScopedHarpDimensionMask col_mask(raw_col);

  row_mask->mask[1] = 0;
  ASSERT_EQ(HARP_SUCCESS,
            harp_dimension_mask_update_masked_length(row_mask.get()));
  col_mask->mask[0] = 0;
  col_mask->mask[2] = 0;
  ASSERT_EQ(HARP_SUCCESS,
            harp_dimension_mask_update_masked_length(col_mask.get()));

  harp_dimension_mask* raw_out = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_outer_product(
                              row_mask.get(), col_mask.get(), &raw_out));
  ScopedHarpDimensionMask out(raw_out);
  EXPECT_EQ(2, out->num_dimensions);
  EXPECT_EQ(3L, out->dimension[0]);
  EXPECT_EQ(4L, out->dimension[1]);
  EXPECT_EQ(12L, out->num_elements);
  EXPECT_EQ(2L, out->masked_dimension_length);
  EXPECT_THAT(std::vector<uint8_t>(out->mask, out->mask + 12),
              ElementsAreArray({0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1}));

  // All-false row_mask exercises the row_mask->masked_dimension_length == 0
  // branch; update_masked_length initializes masked_dimension_length
  // afterwards.
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_fill_false(row_mask.get()));
  harp_dimension_mask* raw_zero_out = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_outer_product(
                              row_mask.get(), col_mask.get(), &raw_zero_out));
  ScopedHarpDimensionMask zero_out(raw_zero_out);
  EXPECT_THAT(std::vector<uint8_t>(zero_out->mask, zero_out->mask + 12),
              ElementsAreArray({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
  ASSERT_EQ(HARP_SUCCESS,
            harp_dimension_mask_update_masked_length(zero_out.get()));
  EXPECT_EQ(0L, zero_out->masked_dimension_length);
}

TEST(HarpDimensionMaskTest, PrependAndAppendDimension) {
  // 1. Prepend onto 0-D mask -> 1-D.
  harp_dimension_mask* raw_0d = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_new(0, nullptr, &raw_0d));
  ScopedHarpDimensionMask mask_0d(raw_0d);
  ASSERT_EQ(HARP_SUCCESS,
            harp_dimension_mask_prepend_dimension(mask_0d.get(), 3));
  EXPECT_EQ(1, mask_0d->num_dimensions);
  EXPECT_EQ(3L, mask_0d->dimension[0]);
  EXPECT_EQ(3L, mask_0d->num_elements);
  EXPECT_EQ(1L, mask_0d->masked_dimension_length);
  EXPECT_THAT(std::vector<uint8_t>(mask_0d->mask, mask_0d->mask + 3),
              ElementsAreArray({1, 1, 1}));

  // 2. Prepend onto 1-D mask -> 2-D, repeating mask along outer dimension.
  constexpr long kDims[] = {3};
  harp_dimension_mask* raw_1d = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_new(1, kDims, &raw_1d));
  ScopedHarpDimensionMask mask_1d(raw_1d);
  mask_1d->mask[1] = 0;
  ASSERT_EQ(HARP_SUCCESS,
            harp_dimension_mask_update_masked_length(mask_1d.get()));
  ASSERT_EQ(HARP_SUCCESS,
            harp_dimension_mask_prepend_dimension(mask_1d.get(), 2));
  EXPECT_EQ(2, mask_1d->num_dimensions);
  EXPECT_EQ(2L, mask_1d->dimension[0]);
  EXPECT_EQ(3L, mask_1d->dimension[1]);
  EXPECT_EQ(6L, mask_1d->num_elements);
  EXPECT_EQ(2L, mask_1d->masked_dimension_length);
  EXPECT_THAT(std::vector<uint8_t>(mask_1d->mask, mask_1d->mask + 6),
              ElementsAreArray({1, 0, 1, 1, 0, 1}));

  // 3. Append onto 0-D mask -> 1-D.
  harp_dimension_mask* raw_app0 = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_new(0, nullptr, &raw_app0));
  ScopedHarpDimensionMask app0(raw_app0);
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_append_dimension(app0.get(), 4));
  EXPECT_EQ(1, app0->num_dimensions);
  EXPECT_EQ(4L, app0->dimension[0]);
  EXPECT_EQ(4L, app0->masked_dimension_length);

  // 4. Append onto 1-D non-zero mask repeats each element `length` times.
  harp_dimension_mask* raw_app1 = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_new(1, kDims, &raw_app1));
  ScopedHarpDimensionMask app1(raw_app1);
  app1->mask[1] = 0;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_update_masked_length(app1.get()));
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_append_dimension(app1.get(), 2));
  EXPECT_EQ(2, app1->num_dimensions);
  EXPECT_EQ(3L, app1->dimension[0]);
  EXPECT_EQ(2L, app1->dimension[1]);
  EXPECT_EQ(2L, app1->masked_dimension_length);
  EXPECT_THAT(std::vector<uint8_t>(app1->mask, app1->mask + 6),
              ElementsAreArray({1, 1, 0, 0, 1, 1}));

  // 5. Append onto all-false 1-D mask keeps masked_dimension_length == 0.
  harp_dimension_mask* raw_app_false = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_new(1, kDims, &raw_app_false));
  ScopedHarpDimensionMask app_false(raw_app_false);
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_fill_false(app_false.get()));
  ASSERT_EQ(HARP_SUCCESS,
            harp_dimension_mask_append_dimension(app_false.get(), 3));
  EXPECT_EQ(0L, app_false->masked_dimension_length);
  EXPECT_EQ(9L, app_false->num_elements);
}

TEST(HarpDimensionMaskTest, ReduceAlongEachDimension) {
  constexpr long kDims[] = {3, 4};
  harp_dimension_mask* raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_new(2, kDims, &raw));
  ScopedHarpDimensionMask mask(raw);

  // Row 0: [0, 1, 0, 0] -> true
  // Row 1: [0, 0, 0, 0] -> false
  // Row 2: [1, 0, 0, 0] -> true
  // Cols: [1, 1, 0, 0]
  const std::array<uint8_t, 12> pattern = {
      0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0,
  };
  for (size_t i = 0; i < pattern.size(); ++i) {
    mask->mask[i] = pattern[i];
  }

  harp_dimension_mask* raw_r0 = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_reduce(mask.get(), 0, &raw_r0));
  ScopedHarpDimensionMask r0(raw_r0);
  EXPECT_EQ(1, r0->num_dimensions);
  EXPECT_EQ(3L, r0->num_elements);
  EXPECT_EQ(2L, r0->masked_dimension_length);
  EXPECT_THAT(std::vector<uint8_t>(r0->mask, r0->mask + 3),
              ElementsAreArray({1, 0, 1}));

  harp_dimension_mask* raw_r1 = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_reduce(mask.get(), 1, &raw_r1));
  ScopedHarpDimensionMask r1(raw_r1);
  EXPECT_EQ(1, r1->num_dimensions);
  EXPECT_EQ(4L, r1->num_elements);
  EXPECT_EQ(2L, r1->masked_dimension_length);
  EXPECT_THAT(std::vector<uint8_t>(r1->mask, r1->mask + 4),
              ElementsAreArray({1, 1, 0, 0}));
}

TEST(HarpDimensionMaskTest, MergeSameAndBroadcastDimensions) {
  // 1. Merge 1-D into 1-D.
  constexpr long kDims1D[] = {4};
  harp_dimension_mask* raw_a1 = nullptr;
  harp_dimension_mask* raw_b1 = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_new(1, kDims1D, &raw_a1));
  ScopedHarpDimensionMask a1(raw_a1);
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_new(1, kDims1D, &raw_b1));
  ScopedHarpDimensionMask b1(raw_b1);
  a1->mask[0] = 0;
  a1->mask[2] = 0;
  b1->mask[1] = 0;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_merge(a1.get(), -1, b1.get()));
  EXPECT_EQ(1L, b1->masked_dimension_length);
  EXPECT_THAT(std::vector<uint8_t>(b1->mask, b1->mask + 4),
              ElementsAreArray({0, 0, 0, 1}));

  // 2. Merge 1-D into 2-D along dim_index 0 and dim_index 1.
  constexpr long kDims2D[] = {3, 4};
  harp_dimension_mask* raw_2d = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_new(2, kDims2D, &raw_2d));
  ScopedHarpDimensionMask m2d(raw_2d);

  constexpr long kRowDims[] = {3};
  harp_dimension_mask* raw_row = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_new(1, kRowDims, &raw_row));
  ScopedHarpDimensionMask row_mask(raw_row);
  row_mask->mask[1] = 0;  // Zeroes row 1.
  ASSERT_EQ(HARP_SUCCESS,
            harp_dimension_mask_merge(row_mask.get(), 0, m2d.get()));
  EXPECT_EQ(4L, m2d->masked_dimension_length);

  // Merge a1 ([0, 1, 0, 1]) along dim_index 1 -> zeroes cols 0 and 2.
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_merge(a1.get(), 1, m2d.get()));
  EXPECT_EQ(2L, m2d->masked_dimension_length);
  EXPECT_THAT(std::vector<uint8_t>(m2d->mask, m2d->mask + 12),
              ElementsAreArray({0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1}));
}

TEST(HarpDimensionMaskTest, SetSimplifyPropagatesBetweenPrimaryAndSecondary) {
  // 1. Empty mask set is a no-op.
  harp_dimension_mask_set* raw_empty = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_set_new(&raw_empty));
  ScopedHarpDimensionMaskSet empty_set(raw_empty);
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_set_simplify(empty_set.get()));

  // 2. 2-D latitude mask with NULL time mask creates time mask, reduces,
  // propagates back, and strips masks that are all true.
  harp_dimension_mask_set* raw_set = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_set_new(&raw_set));
  ScopedHarpDimensionMaskSet set1(raw_set);

  constexpr long kAllTrueDims[] = {3};
  ASSERT_EQ(HARP_SUCCESS,
            harp_dimension_mask_new(1, kAllTrueDims,
                                    &set1.get()[harp_dimension_spectral]));

  constexpr long kLatDims[] = {3, 2};
  ASSERT_EQ(HARP_SUCCESS,
            harp_dimension_mask_new(2, kLatDims,
                                    &set1.get()[harp_dimension_latitude]));
  // Time 1 has no valid latitude elements -> time[1] becomes 0.
  set1.get()[harp_dimension_latitude]->mask[2] = 0;
  set1.get()[harp_dimension_latitude]->mask[3] = 0;

  // All-true 2-D longitude mask will receive time[1]=0 during second pass.
  ASSERT_EQ(HARP_SUCCESS,
            harp_dimension_mask_new(2, kLatDims,
                                    &set1.get()[harp_dimension_longitude]));

  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_set_simplify(set1.get()));
  EXPECT_EQ(nullptr, set1.get()[harp_dimension_spectral]);
  ASSERT_NE(nullptr, set1.get()[harp_dimension_time]);
  EXPECT_THAT(std::vector<uint8_t>(set1.get()[harp_dimension_time]->mask,
                                   set1.get()[harp_dimension_time]->mask + 3),
              ElementsAreArray({1, 0, 1}));
  EXPECT_THAT(
      std::vector<uint8_t>(set1.get()[harp_dimension_longitude]->mask,
                           set1.get()[harp_dimension_longitude]->mask + 6),
      ElementsAreArray({1, 1, 0, 0, 1, 1}));

  // 3. When time mask already has 0 at index 0, simplify zeroes row 0 of 2-D
  // secondary mask too; if all masks simplify to all-true, they are removed.
  set1.get()[harp_dimension_time]->mask[0] = 0;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_set_simplify(set1.get()));
  EXPECT_THAT(
      std::vector<uint8_t>(set1.get()[harp_dimension_latitude]->mask,
                           set1.get()[harp_dimension_latitude]->mask + 6),
      ElementsAreArray({0, 0, 0, 0, 1, 1}));
}

TEST(HarpDimensionMaskTest, OutOfMemoryPaths) {
  const absl::Cleanup reset_err = [] { harp_set_error(HARP_SUCCESS, nullptr); };
  constexpr long kDims2D[] = {3, 2};
  constexpr long kRowDims[] = {3};
  EXPECT_GT(SweepOomUntilSuccess([&]() {
              harp_dimension_mask* raw = nullptr;
              if (harp_dimension_mask_new(2, kDims2D, &raw) == HARP_SUCCESS) {
                ScopedSuppressOom suppress;
                harp_dimension_mask_delete(raw);
              }
            }),
            0);

  harp_dimension_mask* raw_src = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_new(2, kDims2D, &raw_src));
  ScopedHarpDimensionMask src(raw_src);
  src->mask[2] = 0;
  src->mask[3] = 0;

  // harp_dimension_mask_copy leaks `dimension_mask` on its 2nd malloc (mask)
  // upstream (harp-dimension-mask.c:124), so only inject OOM on allocation 1.
  {
    harp_set_error(HARP_SUCCESS, nullptr);
    ScopedOomCountdown guard(1);
    harp_dimension_mask* copy = nullptr;
    EXPECT_EQ(-1, harp_dimension_mask_copy(src.get(), &copy));
    EXPECT_TRUE(guard.triggered());
  }

  EXPECT_GT(SweepOomUntilSuccess([&]() {
              harp_dimension_mask_set* set = nullptr;
              if (harp_dimension_mask_set_new(&set) == HARP_SUCCESS) {
                ScopedSuppressOom suppress;
                harp_dimension_mask_set_delete(set);
              }
            }),
            0);

  harp_dimension_mask* raw_row = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_new(1, kRowDims, &raw_row));
  ScopedHarpDimensionMask row(raw_row);
  EXPECT_GT(SweepOomUntilSuccess([&]() {
              harp_dimension_mask* out = nullptr;
              if (harp_dimension_mask_outer_product(row.get(), row.get(),
                                                    &out) == HARP_SUCCESS) {
                ScopedSuppressOom suppress;
                harp_dimension_mask_delete(out);
              }
            }),
            0);

  EXPECT_GT(
      SweepOomUntilSuccess([&]() {
        harp_dimension_mask* work = nullptr;
        {
          ScopedSuppressOom suppress;
          if (harp_dimension_mask_copy(row.get(), &work) != HARP_SUCCESS) {
            return;
          }
        }
        harp_dimension_mask_prepend_dimension(work, 2);
        ScopedSuppressOom suppress;
        harp_dimension_mask_delete(work);
      }),
      0);

  EXPECT_GT(
      SweepOomUntilSuccess([&]() {
        harp_dimension_mask* work = nullptr;
        {
          ScopedSuppressOom suppress;
          if (harp_dimension_mask_copy(row.get(), &work) != HARP_SUCCESS) {
            return;
          }
        }
        harp_dimension_mask_append_dimension(work, 2);
        ScopedSuppressOom suppress;
        harp_dimension_mask_delete(work);
      }),
      0);

  EXPECT_GT(SweepOomUntilSuccess([&]() {
              harp_dimension_mask* reduced = nullptr;
              if (harp_dimension_mask_reduce(src.get(), 0, &reduced) ==
                  HARP_SUCCESS) {
                ScopedSuppressOom suppress;
                harp_dimension_mask_delete(reduced);
              }
            }),
            0);

  EXPECT_GT(
      SweepOomUntilSuccess([&]() {
        harp_dimension_mask_set* set = nullptr;
        {
          ScopedSuppressOom suppress;
          if (harp_dimension_mask_set_new(&set) != HARP_SUCCESS) {
            return;
          }
          if (harp_dimension_mask_copy(
                  src.get(), &set[harp_dimension_latitude]) != HARP_SUCCESS) {
            harp_dimension_mask_set_delete(set);
            return;
          }
        }
        harp_dimension_mask_set_simplify(set);
        ScopedSuppressOom suppress;
        harp_dimension_mask_set_delete(set);
      }),
      0);
}

}  // namespace
}  // namespace stcorp_harp
