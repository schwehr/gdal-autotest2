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
// Unit tests for libharp array/variable/product filtering
// (libharp/harp-filter.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-filter.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-filter.h

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::ElementsAre;

TEST(HarpFilterTest, ArrayFilterScalarAndNullMask) {
  // 1. Scalar numeric out-of-place.
  int32_t src_val = 42;
  int32_t dst_val = 0;
  harp_array src{};
  harp_array dst{};
  src.int32_data = &src_val;
  dst.int32_data = &dst_val;
  harp_array_filter(harp_type_int32, 0, nullptr, nullptr, src, nullptr, dst);
  EXPECT_EQ(42, dst_val);

  // 2. Scalar string out-of-place replaces existing string and nulls source.
  char* src_str = strdup("new");
  char* dst_str = strdup("old");
  auto free_scalars = absl::MakeCleanup([&] {
    std::free(src_str);
    std::free(dst_str);
  });
  src.string_data = &src_str;
  dst.string_data = &dst_str;
  harp_array_filter(harp_type_string, 0, nullptr, nullptr, src, nullptr, dst);
  EXPECT_EQ(nullptr, src_str);
  ASSERT_NE(nullptr, dst_str);
  EXPECT_STREQ("new", dst_str);

  // 3. 1-D with NULL mask (in-place and out-of-place for strings).
  constexpr long kLen = 2;
  const uint8_t* null_mask[1] = {nullptr};
  char* s_arr[2] = {strdup("a"), strdup("b")};
  char* d_arr[2] = {strdup("x"), strdup("y")};
  auto free_arrays = absl::MakeCleanup([&] {
    for (int i = 0; i < 2; ++i) {
      std::free(s_arr[i]);
      std::free(d_arr[i]);
    }
  });
  src.string_data = s_arr;
  dst.string_data = d_arr;
  harp_array_filter(harp_type_string, 1, &kLen, null_mask, src, &kLen, dst);
  EXPECT_EQ(nullptr, s_arr[0]);
  EXPECT_EQ(nullptr, s_arr[1]);
  EXPECT_STREQ("a", d_arr[0]);
  EXPECT_STREQ("b", d_arr[1]);
  // In-place with NULL mask is a no-op.
  harp_array_filter(harp_type_string, 1, &kLen, null_mask, dst, &kLen, dst);
  EXPECT_STREQ("a", d_arr[0]);
  EXPECT_STREQ("b", d_arr[1]);
}

TEST(HarpFilterTest, ArrayFilterOneDimAllDataTypesWithPadding) {
  constexpr long kSrcLen = 4;
  constexpr long kDstLen = 3;  // Larger than 2 true entries to test padding.
  const uint8_t mask_bytes[4] = {0, 1, 0, 1};
  const uint8_t* mask[1] = {mask_bytes};

  // 1. int8
  {
    int8_t src_buf[4] = {10, 20, 30, 40};
    int8_t dst_buf[3] = {99, 99, 99};
    harp_array src{};
    harp_array dst{};
    src.int8_data = src_buf;
    dst.int8_data = dst_buf;
    harp_array_filter(harp_type_int8, 1, &kSrcLen, mask, src, &kDstLen, dst);
    EXPECT_THAT(dst_buf, ElementsAre(20, 40, 0));
  }

  // 2. int16
  {
    int16_t src_buf[4] = {100, 200, 300, 400};
    int16_t dst_buf[3] = {99, 99, 99};
    harp_array src{};
    harp_array dst{};
    src.int16_data = src_buf;
    dst.int16_data = dst_buf;
    harp_array_filter(harp_type_int16, 1, &kSrcLen, mask, src, &kDstLen, dst);
    EXPECT_THAT(dst_buf, ElementsAre(200, 400, 0));
  }

  // 3. int32
  {
    int32_t src_buf[4] = {1000, 2000, 3000, 4000};
    int32_t dst_buf[3] = {99, 99, 99};
    harp_array src{};
    harp_array dst{};
    src.int32_data = src_buf;
    dst.int32_data = dst_buf;
    harp_array_filter(harp_type_int32, 1, &kSrcLen, mask, src, &kDstLen, dst);
    EXPECT_THAT(dst_buf, ElementsAre(2000, 4000, 0));
  }

  // 4. float (remaining target element set to NaN)
  {
    float src_buf[4] = {1.5f, 2.5f, 3.5f, 4.5f};
    float dst_buf[3] = {0.0f, 0.0f, 0.0f};
    harp_array src{};
    harp_array dst{};
    src.float_data = src_buf;
    dst.float_data = dst_buf;
    harp_array_filter(harp_type_float, 1, &kSrcLen, mask, src, &kDstLen, dst);
    EXPECT_FLOAT_EQ(2.5f, dst_buf[0]);
    EXPECT_FLOAT_EQ(4.5f, dst_buf[1]);
    EXPECT_TRUE(std::isnan(dst_buf[2]));
  }

  // 5. double (remaining target element set to NaN)
  {
    double src_buf[4] = {1.25, 2.25, 3.25, 4.25};
    double dst_buf[3] = {0.0, 0.0, 0.0};
    harp_array src{};
    harp_array dst{};
    src.double_data = src_buf;
    dst.double_data = dst_buf;
    harp_array_filter(harp_type_double, 1, &kSrcLen, mask, src, &kDstLen, dst);
    EXPECT_DOUBLE_EQ(2.25, dst_buf[0]);
    EXPECT_DOUBLE_EQ(4.25, dst_buf[1]);
    EXPECT_TRUE(std::isnan(dst_buf[2]));
  }

  // 6. string out-of-place and in-place with holes
  {
    char* src_buf[4] = {strdup("s0"), strdup("s1"), strdup("s2"), strdup("s3")};
    char* dst_buf[3] = {strdup("d0"), strdup("d1"), strdup("d2")};
    auto cleanup_strs = absl::MakeCleanup([&] {
      for (int i = 0; i < 4; ++i) {
        std::free(src_buf[i]);
      }
      for (int i = 0; i < 3; ++i) {
        std::free(dst_buf[i]);
      }
    });
    harp_array src{};
    harp_array dst{};
    src.string_data = src_buf;
    dst.string_data = dst_buf;
    harp_array_filter(harp_type_string, 1, &kSrcLen, mask, src, &kDstLen, dst);
    EXPECT_STREQ("s1", dst_buf[0]);
    EXPECT_STREQ("s3", dst_buf[1]);
    EXPECT_EQ(nullptr, dst_buf[2]);

    // In-place filtering with kept first element followed by skipped element.
    char* inplace[3] = {strdup("keep0"), strdup("drop1"), strdup("keep2")};
    auto cleanup_inplace = absl::MakeCleanup([&] {
      for (int i = 0; i < 3; ++i) {
        std::free(inplace[i]);
      }
    });
    const uint8_t inplace_mask_bytes[3] = {1, 0, 1};
    const uint8_t* inplace_mask[1] = {inplace_mask_bytes};
    constexpr long kInplaceSrcLen = 3;
    constexpr long kInplaceDstLen = 2;
    harp_array arr{};
    arr.string_data = inplace;
    harp_array_filter(harp_type_string, 1, &kInplaceSrcLen, inplace_mask, arr,
                      &kInplaceDstLen, arr);
    EXPECT_STREQ("keep0", inplace[0]);
    EXPECT_STREQ("keep2", inplace[1]);
    EXPECT_EQ(nullptr, inplace[2]);
  }
}

TEST(HarpFilterTest, ArrayFilterMultiDimWithPaddingAndNullMasks) {
  // 3-D [2, 3, 2] -> target [2, 3, 2] where dim 0 keeps only index 1,
  // dim 1 keeps only index 1 (with target dim 1 == 3 to exercise trailing
  // harp_array_null blocks), and dim 2 has NULL mask.
  constexpr long kSrcDims[3] = {2, 3, 2};
  constexpr long kDstDims[3] = {2, 3, 2};
  const uint8_t mask0[2] = {0, 1};
  const uint8_t mask1[3] = {0, 1, 0};
  const uint8_t* masks[3] = {mask0, mask1, nullptr};

  std::vector<int32_t> data(12);
  for (int i = 0; i < 12; ++i) {
    data[i] = i + 1;
  }
  harp_array arr{};
  arr.int32_data = data.data();
  harp_array_filter(harp_type_int32, 3, kSrcDims, masks, arr, kDstDims, arr);

  // Kept block is [1, 1, *] -> original values 9, 10; remaining slots nulled
  // to 0.
  EXPECT_THAT(data, ElementsAre(9, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
}

TEST(HarpFilterTest, VariableFilterNullScalarAndUnmasked) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  // 1. NULL mask set is a no-op.
  harp_variable* raw_scalar = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_new("s", harp_type_double, 0, nullptr,
                                            nullptr, &raw_scalar));
  ScopedHarpVariable scalar(raw_scalar);
  scalar->data.double_data[0] = 3.14;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_filter(scalar.get(), nullptr));
  EXPECT_DOUBLE_EQ(3.14, scalar->data.double_data[0]);

  // 2. Non-NULL mask set on scalar is a no-op.
  harp_dimension_mask_set* raw_set = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_set_new(&raw_set));
  ScopedHarpDimensionMaskSet mask_set(raw_set);
  constexpr long kTimeDims[] = {3};
  ASSERT_EQ(HARP_SUCCESS,
            harp_dimension_mask_new(1, kTimeDims,
                                    &mask_set.get()[harp_dimension_time]));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_filter(scalar.get(), mask_set.get()));

  // 3. Variable with independent & unmasked dimensions returns without changes.
  const harp_dimension_type dim_types[2] = {harp_dimension_independent,
                                            harp_dimension_latitude};
  const long dims[2] = {2, 2};
  harp_variable* raw_var = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_new("v", harp_type_int32, 2, dim_types,
                                            dims, &raw_var));
  ScopedHarpVariable var(raw_var);
  var->data.int32_data[0] = 7;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_filter(var.get(), mask_set.get()));
  EXPECT_EQ(4L, var->num_elements);
  EXPECT_EQ(7, var->data.int32_data[0]);
}

TEST(HarpFilterTest, VariableFilterOneDimAndTwoDimMasks) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  harp_dimension_mask_set* raw_set = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_set_new(&raw_set));
  ScopedHarpDimensionMaskSet mask_set(raw_set);

  // 1-D time mask [1, 0, 1] -> length 2.
  constexpr long kTimeDims[] = {3};
  ASSERT_EQ(HARP_SUCCESS,
            harp_dimension_mask_new(1, kTimeDims,
                                    &mask_set.get()[harp_dimension_time]));
  mask_set.get()[harp_dimension_time]->mask[1] = 0;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_update_masked_length(
                              mask_set.get()[harp_dimension_time]));

  // 1. 1-D string variable shrinks and frees dropped strings.
  const harp_dimension_type time_dim = harp_dimension_time;
  harp_variable* raw_str_var = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_new("id", harp_type_string, 1, &time_dim, kTimeDims,
                              &raw_str_var));
  ScopedHarpVariable str_var(raw_str_var);
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(str_var.get(), 0, "first"));
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(str_var.get(), 1, "second"));
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(str_var.get(), 2, "third"));

  ASSERT_EQ(HARP_SUCCESS, harp_variable_filter(str_var.get(), mask_set.get()));
  EXPECT_EQ(2L, str_var->num_elements);
  EXPECT_EQ(2L, str_var->dimension[0]);
  EXPECT_STREQ("first", str_var->data.string_data[0]);
  EXPECT_STREQ("third", str_var->data.string_data[1]);

  // 2. All-true mask preserves size without realloc.
  constexpr long kLatDims[] = {2};
  ASSERT_EQ(HARP_SUCCESS,
            harp_dimension_mask_new(1, kLatDims,
                                    &mask_set.get()[harp_dimension_latitude]));
  const harp_dimension_type lat_dim = harp_dimension_latitude;
  harp_variable* raw_lat_var = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_new("lat", harp_type_double, 1,
                                            &lat_dim, kLatDims, &raw_lat_var));
  ScopedHarpVariable lat_var(raw_lat_var);
  lat_var->data.double_data[0] = 10.0;
  lat_var->data.double_data[1] = 20.0;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_filter(lat_var.get(), mask_set.get()));
  EXPECT_EQ(2L, lat_var->num_elements);
  EXPECT_DOUBLE_EQ(10.0, lat_var->data.double_data[0]);
  EXPECT_DOUBLE_EQ(20.0, lat_var->data.double_data[1]);

  // 3. 2-D vertical mask [3, 4] combined with 1-D time mask [1, 0, 1] and
  // unmasked trailing independent dimension [2].
  constexpr long kVertDims[] = {3, 4};
  ASSERT_EQ(HARP_SUCCESS,
            harp_dimension_mask_new(2, kVertDims,
                                    &mask_set.get()[harp_dimension_vertical]));
  const uint8_t vert_bytes[12] = {
      1, 0, 1, 0,  // time 0: 2 entries
      1, 1, 1, 0,  // time 1 (masked out by time mask): 3 entries
      0, 1, 0, 0,  // time 2: 1 entry (second slot gets NaN)
  };
  std::memcpy(mask_set.get()[harp_dimension_vertical]->mask, vert_bytes, 12);
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_update_masked_length(
                              mask_set.get()[harp_dimension_vertical]));
  EXPECT_EQ(3L,
            mask_set.get()[harp_dimension_vertical]->masked_dimension_length);

  // Set time 1 vertical mask to fewer active entries so max is 2 (from time 0).
  mask_set.get()[harp_dimension_vertical]->mask[4] = 0;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_update_masked_length(
                              mask_set.get()[harp_dimension_vertical]));

  const harp_dimension_type var3_dims[3] = {
      harp_dimension_time, harp_dimension_vertical, harp_dimension_independent};
  const long var3_lens[3] = {3, 4, 2};
  harp_variable* raw_var3 = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_new("profile", harp_type_double, 3,
                                            var3_dims, var3_lens, &raw_var3));
  ScopedHarpVariable var3(raw_var3);
  for (long i = 0; i < 24; ++i) {
    var3->data.double_data[i] = static_cast<double>(i);
  }
  ASSERT_EQ(HARP_SUCCESS, harp_variable_filter(var3.get(), mask_set.get()));
  EXPECT_EQ(2L, var3->dimension[0]);
  EXPECT_EQ(2L, var3->dimension[1]);
  EXPECT_EQ(2L, var3->dimension[2]);
  EXPECT_EQ(8L, var3->num_elements);
  // Time 0 keeps vertical indices 0 (0,1) and 2 (4,5).
  EXPECT_DOUBLE_EQ(0.0, var3->data.double_data[0]);
  EXPECT_DOUBLE_EQ(1.0, var3->data.double_data[1]);
  EXPECT_DOUBLE_EQ(4.0, var3->data.double_data[2]);
  EXPECT_DOUBLE_EQ(5.0, var3->data.double_data[3]);
  // Time 2 keeps vertical index 1 (18,19) and pads slot 1 with NaN.
  EXPECT_DOUBLE_EQ(18.0, var3->data.double_data[4]);
  EXPECT_DOUBLE_EQ(19.0, var3->data.double_data[5]);
  EXPECT_TRUE(std::isnan(var3->data.double_data[6]));
  EXPECT_TRUE(std::isnan(var3->data.double_data[7]));
}

TEST(HarpFilterTest, ProductFilterNullEmptyAndTwoDimPromotion) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  // Add 1-D time variable, 1-D vertical variable (no time dimension!), and
  // 2-D {time, vertical} variable.
  const harp_dimension_type time_dim = harp_dimension_time;
  const long time_len = 2;
  harp_variable* raw_time = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_new("datetime", harp_type_double, 1,
                                            &time_dim, &time_len, &raw_time));
  ScopedHarpVariable time_var(raw_time);
  time_var->data.double_data[0] = 100.0;
  time_var->data.double_data[1] = 200.0;
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_add_variable(prod.get(), time_var.get()));
  time_var.release();

  const harp_dimension_type vert_dim = harp_dimension_vertical;
  const long vert_len = 3;
  harp_variable* raw_vert = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_new("altitude", harp_type_double, 1,
                                            &vert_dim, &vert_len, &raw_vert));
  ScopedHarpVariable vert_var(raw_vert);
  vert_var->data.double_data[0] = 1.0;
  vert_var->data.double_data[1] = 2.0;
  vert_var->data.double_data[2] = 3.0;
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_add_variable(prod.get(), vert_var.get()));
  vert_var.release();

  const harp_dimension_type tv_dims[2] = {harp_dimension_time,
                                          harp_dimension_vertical};
  const long tv_lens[2] = {2, 3};
  harp_variable* raw_tv = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_new("O3_volume_mixing_ratio", harp_type_double, 2,
                              tv_dims, tv_lens, &raw_tv));
  ScopedHarpVariable tv_var(raw_tv);
  for (int i = 0; i < 6; ++i) {
    tv_var->data.double_data[i] = 10.0 + i;
  }
  ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), tv_var.get()));
  tv_var.release();

  // 1. NULL mask set leaves product unchanged.
  ASSERT_EQ(HARP_SUCCESS, harp_product_filter(prod.get(), nullptr));
  EXPECT_EQ(2L, prod->dimension[harp_dimension_time]);
  EXPECT_EQ(3L, prod->dimension[harp_dimension_vertical]);

  // 2. 2-D vertical mask promotes time-less 'altitude' variable to 2-D
  // {time, vertical} before filtering.
  harp_dimension_mask_set* raw_set = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_set_new(&raw_set));
  ScopedHarpDimensionMaskSet mask_set(raw_set);
  ASSERT_EQ(HARP_SUCCESS,
            harp_dimension_mask_new(2, tv_lens,
                                    &mask_set.get()[harp_dimension_vertical]));
  const uint8_t vmask[6] = {1, 0, 1, 0, 1, 0};
  std::memcpy(mask_set.get()[harp_dimension_vertical]->mask, vmask, 6);
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_update_masked_length(
                              mask_set.get()[harp_dimension_vertical]));

  ASSERT_EQ(HARP_SUCCESS, harp_product_filter(prod.get(), mask_set.get()));
  EXPECT_EQ(2L, prod->dimension[harp_dimension_time]);
  EXPECT_EQ(2L, prod->dimension[harp_dimension_vertical]);

  harp_variable* alt_out = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                              prod.get(), "altitude", &alt_out));
  EXPECT_EQ(2, alt_out->num_dimensions);
  EXPECT_EQ(harp_dimension_time, alt_out->dimension_type[0]);
  EXPECT_EQ(harp_dimension_vertical, alt_out->dimension_type[1]);
  EXPECT_DOUBLE_EQ(1.0, alt_out->data.double_data[0]);
  EXPECT_DOUBLE_EQ(3.0, alt_out->data.double_data[1]);
  EXPECT_DOUBLE_EQ(2.0, alt_out->data.double_data[2]);
  EXPECT_TRUE(std::isnan(alt_out->data.double_data[3]));

  // 3. Mask with masked_dimension_length == 0 empties the entire product.
  ASSERT_EQ(HARP_SUCCESS, harp_dimension_mask_fill_false(
                              mask_set.get()[harp_dimension_vertical]));
  ASSERT_EQ(HARP_SUCCESS, harp_product_filter(prod.get(), mask_set.get()));
  EXPECT_EQ(1, harp_product_is_empty(prod.get()));
}

}  // namespace
}  // namespace stcorp_harp
