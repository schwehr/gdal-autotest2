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
// Unit tests for libharp collocation filtering
// (libharp/harp-filter-collocation.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-filter-collocation.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-filter-collocation.h

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/oom_fault_inject.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/google/util/temp_file.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::ElementsAre;

TEST(HarpFilterCollocationTest, MaskDeleteAndImportErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  harp_collocation_mask_delete(nullptr);
  auto* empty_mask = static_cast<harp_collocation_mask*>(
      std::malloc(sizeof(harp_collocation_mask)));
  ASSERT_NE(nullptr, empty_mask);
  empty_mask->num_index_pairs = 0;
  empty_mask->index_pair = nullptr;
  harp_collocation_mask_delete(empty_mask);

  harp_collocation_mask* raw_mask = nullptr;
  EXPECT_EQ(-1, harp_collocation_mask_import(nullptr, harp_collocation_left, -1,
                                             -1, "a.nc", &raw_mask));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  TempFile missing("missing_colloc.csv");
  EXPECT_EQ(-1,
            harp_collocation_mask_import(missing.c_str(), harp_collocation_left,
                                         -1, -1, "a.nc", &raw_mask));
  EXPECT_EQ(HARP_ERROR_FILE_OPEN, harp_errno);

  // Invalid unit on datetime_diff fails unit converter creation cleanly.
  TempFile bad_unit("bad_unit_colloc.csv");
  ASSERT_TRUE(WriteFileText(
      bad_unit.path(),
      "collocation_index,source_product_a,index_a,source_product_b,index_b,"
      "datetime_diff [not_a_unit]\n0,a.nc,1,b.nc,2,5.0\n"));
  EXPECT_EQ(
      -1, harp_collocation_mask_import(bad_unit.c_str(), harp_collocation_left,
                                       -1, -1, "a.nc", &raw_mask));
  EXPECT_EQ(HARP_ERROR_UNIT_CONVERSION, harp_errno);
}

TEST(HarpFilterCollocationTest, MaskImportLeftRightBlockGrowthAndUnits) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  // 1. CSV with datetime_diff [s] (no unit converter allocation needed) and
  // > 1024 entries for a.nc to exercise COLLOCATION_MASK_BLOCK_SIZE realloc.
  TempFile csv_sec("colloc_sec.csv");
  std::string content =
      "collocation_index,source_product_a,index_a,source_product_b,index_b,"
      "datetime_diff [s]\n";
  for (int i = 0; i < 1025; ++i) {
    content += std::to_string(i) + ",a.nc," + std::to_string(i * 2) + ",b.nc," +
               std::to_string(i * 3) + ",12.5\n";
  }
  ASSERT_TRUE(WriteFileText(csv_sec.path(), content));

  // Left filter negates datetime_diff.
  harp_collocation_mask* raw_left = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_mask_import(csv_sec.c_str(), harp_collocation_left,
                                         -1, -1, "a.nc", &raw_left));
  ScopedHarpCollocationMask left_mask(raw_left);
  EXPECT_EQ(1025L, left_mask->num_index_pairs);
  EXPECT_EQ(0L, left_mask->index_pair[0].collocation_index);
  EXPECT_EQ(2048L, left_mask->index_pair[1024].index);
  EXPECT_DOUBLE_EQ(-12.5, left_mask->index_pair[0].datetime_diff);

  // Right filter keeps positive datetime_diff.
  harp_collocation_mask* raw_right = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_collocation_mask_import(
                              csv_sec.c_str(), harp_collocation_right, 0, 1,
                              "b.nc", &raw_right));
  ScopedHarpCollocationMask right_mask(raw_right);
  EXPECT_EQ(2L, right_mask->num_index_pairs);
  EXPECT_EQ(3L, right_mask->index_pair[1].index);
  EXPECT_DOUBLE_EQ(12.5, right_mask->index_pair[1].datetime_diff);

  // Non-matching source_product yields an empty mask for both left and right.
  harp_collocation_mask* raw_none = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_mask_import(csv_sec.c_str(), harp_collocation_left,
                                         -1, -1, "other.nc", &raw_none));
  ScopedHarpCollocationMask none_mask(raw_none);
  EXPECT_EQ(0L, none_mask->num_index_pairs);

  harp_collocation_mask* raw_none_right = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_collocation_mask_import(
                              csv_sec.c_str(), harp_collocation_right, -1, -1,
                              "other.nc", &raw_none_right));
  ScopedHarpCollocationMask none_right_mask(raw_none_right);
  EXPECT_EQ(0L, none_right_mask->num_index_pairs);

  // 2. CSV with non-datetime_diff column sets datetime_diff to NaN.
  TempFile csv_nodiff("colloc_nodiff.csv");
  ASSERT_TRUE(WriteFileText(
      csv_nodiff.path(),
      "collocation_index,source_product_a,index_a,source_product_b,index_b,"
      "point_distance [km]\n"
      "5,a.nc,1,b.nc,4,12.0\n"));
  harp_collocation_mask* raw_nodiff = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_collocation_mask_import(
                              csv_nodiff.c_str(), harp_collocation_right, -1,
                              -1, "b.nc", &raw_nodiff));
  ScopedHarpCollocationMask nodiff_mask(raw_nodiff);
  ASSERT_EQ(1L, nodiff_mask->num_index_pairs);
  EXPECT_TRUE(std::isnan(nodiff_mask->index_pair[0].datetime_diff));

  // Note: datetime_diff unit conversion is not exercised here because upstream
  // does not free `unit_converter` on that path in harp-filter-collocation.c.
}

TEST(HarpFilterCollocationTest, ApplyMaskErrorPathsAndEmptyProduct) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  harp_collocation_mask mask{};
  mask.num_index_pairs = 0;
  mask.index_pair = nullptr;

  // 1. NULL arguments.
  EXPECT_EQ(-1, harp_product_apply_collocation_mask(prod.get(), nullptr));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_EQ(-1, harp_product_apply_collocation_mask(nullptr, &mask));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  harp_set_error(HARP_SUCCESS, nullptr);

  // 2. Empty product (dimension[time] == 0) succeeds as a no-op.
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_apply_collocation_mask(prod.get(), &mask));

  // 3. Invalid 'collocation_index' variable properties.
  const harp_dimension_type lat_dim = harp_dimension_latitude;
  const harp_dimension_type time_dim = harp_dimension_time;
  const harp_dimension_type dims2d[2] = {harp_dimension_time,
                                         harp_dimension_latitude};
  const long len1 = 2;
  const long lens2d[2] = {2, 2};

  // Add a time variable so dimension[time] == 2.
  {
    harp_variable* raw_t = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_variable_new("datetime", harp_type_double, 1,
                                              &time_dim, &len1, &raw_t));
    ScopedHarpVariable t(raw_t);
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), t.get()));
    t.release();
  }

  // Missing both 'collocation_index' and 'index'.
  EXPECT_EQ(-1, harp_product_apply_collocation_mask(prod.get(), &mask));
  EXPECT_EQ(HARP_ERROR_VARIABLE_NOT_FOUND, harp_errno);
  harp_set_error(HARP_SUCCESS, nullptr);

  // Wrong data type for 'collocation_index'.
  {
    harp_variable* raw_ci = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_new("collocation_index", harp_type_double, 1,
                                &time_dim, &len1, &raw_ci));
    ScopedHarpVariable ci(raw_ci);
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), ci.get()));
    ci.release();
    EXPECT_EQ(-1, harp_product_apply_collocation_mask(prod.get(), &mask));
    EXPECT_EQ(HARP_ERROR_OPERATION, harp_errno);
    ASSERT_EQ(HARP_SUCCESS, harp_product_remove_variable_by_name(
                                prod.get(), "collocation_index"));
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // Wrong num_dimensions for 'collocation_index'.
  {
    harp_variable* raw_ci = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_new("collocation_index", harp_type_int32, 2, dims2d,
                                lens2d, &raw_ci));
    ScopedHarpVariable ci(raw_ci);
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), ci.get()));
    ci.release();
    EXPECT_EQ(-1, harp_product_apply_collocation_mask(prod.get(), &mask));
    EXPECT_EQ(HARP_ERROR_OPERATION, harp_errno);
    ASSERT_EQ(HARP_SUCCESS, harp_product_remove_variable_by_name(
                                prod.get(), "collocation_index"));
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // Wrong dimension_type for 'collocation_index'.
  {
    harp_variable* raw_ci = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_new("collocation_index", harp_type_int32, 1,
                                &lat_dim, &len1, &raw_ci));
    ScopedHarpVariable ci(raw_ci);
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), ci.get()));
    ci.release();
    EXPECT_EQ(-1, harp_product_apply_collocation_mask(prod.get(), &mask));
    EXPECT_EQ(HARP_ERROR_OPERATION, harp_errno);
    ASSERT_EQ(HARP_SUCCESS, harp_product_remove_variable_by_name(
                                prod.get(), "collocation_index"));
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // Wrong data type / num_dimensions / dimension_type for 'index'.
  {
    harp_variable* raw_idx = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_variable_new("index", harp_type_double, 1,
                                              &time_dim, &len1, &raw_idx));
    ScopedHarpVariable idx(raw_idx);
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), idx.get()));
    idx.release();
    EXPECT_EQ(-1, harp_product_apply_collocation_mask(prod.get(), &mask));
    EXPECT_EQ(HARP_ERROR_OPERATION, harp_errno);
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_remove_variable_by_name(prod.get(), "index"));
    harp_set_error(HARP_SUCCESS, nullptr);
  }
  {
    harp_variable* raw_idx = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_variable_new("index", harp_type_int32, 2,
                                              dims2d, lens2d, &raw_idx));
    ScopedHarpVariable idx(raw_idx);
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), idx.get()));
    idx.release();
    EXPECT_EQ(-1, harp_product_apply_collocation_mask(prod.get(), &mask));
    EXPECT_EQ(HARP_ERROR_OPERATION, harp_errno);
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_remove_variable_by_name(prod.get(), "index"));
    harp_set_error(HARP_SUCCESS, nullptr);
  }
  {
    harp_variable* raw_idx = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_variable_new("index", harp_type_int32, 1,
                                              &lat_dim, &len1, &raw_idx));
    ScopedHarpVariable idx(raw_idx);
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), idx.get()));
    idx.release();
    EXPECT_EQ(-1, harp_product_apply_collocation_mask(prod.get(), &mask));
    EXPECT_EQ(HARP_ERROR_OPERATION, harp_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }
}

TEST(HarpFilterCollocationTest, ApplyMaskWithIndexAndCollocationDatetime) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const int prev_opt = harp_get_option_create_collocation_datetime();
  auto restore_opt = absl::MakeCleanup([prev_opt] {
    harp_set_option_create_collocation_datetime(prev_opt);
    harp_set_error(HARP_SUCCESS, nullptr);
  });

  auto make_product = [](bool with_datetime) {
    harp_product* raw = nullptr;
    EXPECT_EQ(HARP_SUCCESS, harp_product_new(&raw));
    ScopedHarpProduct prod(raw);
    const harp_dimension_type time_dim = harp_dimension_time;
    const long len = 4;

    harp_variable* raw_idx = nullptr;
    EXPECT_EQ(HARP_SUCCESS, harp_variable_new("index", harp_type_int32, 1,
                                              &time_dim, &len, &raw_idx));
    ScopedHarpVariable idx(raw_idx);
    for (int i = 0; i < 4; ++i) {
      idx->data.int32_data[i] = i * 10;  // 0, 10, 20, 30
    }
    EXPECT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), idx.get()));
    idx.release();

    if (with_datetime) {
      harp_variable* raw_dt = nullptr;
      EXPECT_EQ(HARP_SUCCESS, harp_variable_new("datetime", harp_type_double, 1,
                                                &time_dim, &len, &raw_dt));
      ScopedHarpVariable dt(raw_dt);
      EXPECT_EQ(HARP_SUCCESS,
                harp_variable_set_unit(dt.get(), HARP_UNIT_DATETIME));
      for (int i = 0; i < 4; ++i) {
        dt->data.double_data[i] = 1000.0 + i * 100.0;
      }
      EXPECT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), dt.get()));
      dt.release();
    }
    return prod;
  };

  // Mask has indices -5 (absent), 10 (matched twice + duplicate pair for
  // tie-breaker), 25 (absent), 30, 99.
  harp_collocation_index_pair pairs[6] = {
      {.collocation_index = 4, .index = 30, .datetime_diff = 5.0},
      {.collocation_index = 1, .index = -5, .datetime_diff = 1.0},
      {.collocation_index = 3, .index = 10, .datetime_diff = -2.0},
      {.collocation_index = 2, .index = 10, .datetime_diff = 3.0},
      {.collocation_index = 5, .index = 99, .datetime_diff = 0.0},
      {.collocation_index = 4, .index = 30, .datetime_diff = 5.0},
  };
  harp_collocation_mask mask{};
  mask.num_index_pairs = 6;
  mask.index_pair = pairs;

  // 1. With create_collocation_datetime == 1 and datetime present.
  ASSERT_EQ(HARP_SUCCESS, harp_set_option_create_collocation_datetime(1));
  {
    ScopedHarpProduct prod = make_product(/*with_datetime=*/true);
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_apply_collocation_mask(prod.get(), &mask));
    EXPECT_EQ(4L, prod->dimension[harp_dimension_time]);

    harp_variable* ci = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                                prod.get(), "collocation_index", &ci));
    EXPECT_THAT(
        std::vector<int32_t>(ci->data.int32_data, ci->data.int32_data + 4),
        ElementsAre(2, 3, 4, 4));

    harp_variable* cdt = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                                prod.get(), "collocation_datetime", &cdt));
    EXPECT_DOUBLE_EQ(1103.0, cdt->data.double_data[0]);
    EXPECT_DOUBLE_EQ(1098.0, cdt->data.double_data[1]);
    EXPECT_DOUBLE_EQ(1305.0, cdt->data.double_data[2]);
    EXPECT_DOUBLE_EQ(1305.0, cdt->data.double_data[3]);
  }

  // 1b. Rearrange failure path (variable with 0 elements) frees temporary
  // buffers cleanly.
  {
    ScopedHarpProduct prod = make_product(/*with_datetime=*/true);
    const harp_dimension_type empty_dims[2] = {harp_dimension_time,
                                               harp_dimension_latitude};
    const long empty_lens[2] = {4, 0};
    harp_variable* raw_empty = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_new("empty", harp_type_double, 2, empty_dims,
                                empty_lens, &raw_empty));
    ScopedHarpVariable empty(raw_empty);
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), empty.get()));
    empty.release();
    EXPECT_EQ(-1, harp_product_apply_collocation_mask(prod.get(), &mask));
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 2. create_collocation_datetime == 1 without datetime fails cleanly.
  {
    ScopedHarpProduct prod = make_product(/*with_datetime=*/false);
    EXPECT_EQ(-1, harp_product_apply_collocation_mask(prod.get(), &mask));
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 3. No matching index empties the product.
  {
    ScopedHarpProduct prod = make_product(/*with_datetime=*/true);
    harp_collocation_index_pair nomatch = {
        .collocation_index = 1, .index = 999, .datetime_diff = 0.0};
    harp_collocation_mask empty_match{};
    empty_match.num_index_pairs = 1;
    empty_match.index_pair = &nomatch;
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_apply_collocation_mask(prod.get(), &empty_match));
    EXPECT_EQ(1, harp_product_is_empty(prod.get()));
  }
}

TEST(HarpFilterCollocationTest,
     ApplyMaskWithExistingCollocationIndexAndFailureCleanup) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  harp_product* raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw));
  ScopedHarpProduct prod(raw);
  const harp_dimension_type time_dim = harp_dimension_time;
  const long len = 3;
  harp_variable* raw_ci = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_new("collocation_index", harp_type_int32, 1,
                              &time_dim, &len, &raw_ci));
  ScopedHarpVariable ci(raw_ci);
  ci->data.int32_data[0] = 10;
  ci->data.int32_data[1] = 20;
  ci->data.int32_data[2] = 30;
  ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), ci.get()));
  ci.release();

  // Adding a 0-element variable forces harp_product_filter_dimension to fail
  // after filter_collocation_index runs, exercising the cleanup path at
  // harp-filter-collocation.c:434 without triggering the upstream success-path
  // dimension_mask leak at line 436.
  const harp_dimension_type empty_dims[2] = {harp_dimension_time,
                                             harp_dimension_latitude};
  const long empty_lens[2] = {3, 0};
  harp_variable* raw_empty = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_new("empty", harp_type_double, 2, empty_dims,
                              empty_lens, &raw_empty));
  ScopedHarpVariable empty(raw_empty);
  ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), empty.get()));
  empty.release();

  harp_collocation_index_pair pairs[3] = {
      {.collocation_index = 30, .index = 2, .datetime_diff = 0.0},
      {.collocation_index = 10, .index = 0, .datetime_diff = 0.0},
      {.collocation_index = 30, .index = 2, .datetime_diff = 0.0},
  };
  harp_collocation_mask mask{};
  mask.num_index_pairs = 3;
  mask.index_pair = pairs;

  EXPECT_EQ(-1, harp_product_apply_collocation_mask(prod.get(), &mask));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
}

TEST(HarpFilterCollocationTest, GetFilteredProductBEndToEndAndErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  // Synthesize a HARP NetCDF file for dataset B.
  TempFile nc_b("dataset_b.nc");
  {
    harp_product* raw_b = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_b));
    ScopedHarpProduct b(raw_b);
    ASSERT_EQ(HARP_SUCCESS, harp_product_set_source_product(b.get(), "b.nc"));

    const harp_dimension_type time_dim = harp_dimension_time;
    const long len = 3;
    harp_variable* raw_idx = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_variable_new("index", harp_type_int32, 1,
                                              &time_dim, &len, &raw_idx));
    ScopedHarpVariable idx(raw_idx);
    idx->data.int32_data[0] = 0;
    idx->data.int32_data[1] = 1;
    idx->data.int32_data[2] = 2;
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(b.get(), idx.get()));
    idx.release();

    harp_variable* raw_val = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_variable_new("temperature", harp_type_double,
                                              1, &time_dim, &len, &raw_val));
    ScopedHarpVariable val(raw_val);
    val->data.double_data[0] = 280.0;
    val->data.double_data[1] = 290.0;
    val->data.double_data[2] = 300.0;
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(b.get(), val.get()));
    val.release();

    ASSERT_EQ(HARP_SUCCESS, harp_export(nc_b.c_str(), "netcdf", b.get()));
  }

  harp_collocation_result* raw_res = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_new(&raw_res, 0, nullptr, nullptr));
  ScopedHarpCollocationResult res(raw_res);
  ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_add_pair(
                              res.get(), 10, "a.nc", 0, "b.nc", 2, 0, nullptr));
  ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_add_pair(
                              res.get(), 20, "a.nc", 1, "b.nc", 0, 0, nullptr));

  // 1. Unknown source product B fails.
  harp_product* out_prod = nullptr;
  EXPECT_EQ(-1, harp_collocation_result_get_filtered_product_b(
                    res.get(), "unknown.nc", &out_prod));
  harp_set_error(HARP_SUCCESS, nullptr);

  // 2. Missing metadata in dataset_b fails with HARP_ERROR_INVALID_ARGUMENT.
  EXPECT_EQ(-1, harp_collocation_result_get_filtered_product_b(
                    res.get(), "b.nc", &out_prod));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  harp_set_error(HARP_SUCCESS, nullptr);

  // 3. Populate metadata with non-existent file -> HARP_ERROR_IMPORT.
  harp_product_metadata* raw_meta = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&raw_meta));
  ScopedHarpProductMetadata meta_guard(raw_meta);
  raw_meta->filename = strdup("/nonexistent_b_file.nc");
  ASSERT_NE(nullptr, raw_meta->filename);
  raw_meta->source_product = strdup("b.nc");
  ASSERT_NE(nullptr, raw_meta->source_product);
  ASSERT_EQ(HARP_SUCCESS,
            harp_dataset_add_product(res->dataset_b, "b.nc", meta_guard.get()));
  meta_guard.release();

  EXPECT_EQ(-1, harp_collocation_result_get_filtered_product_b(
                    res.get(), "b.nc", &out_prod));
  EXPECT_EQ(HARP_ERROR_IMPORT, harp_errno);
  harp_set_error(HARP_SUCCESS, nullptr);

  // 4. Point metadata->filename at synthesized NetCDF file -> succeeds.
  std::free(res->dataset_b->metadata[0]->filename);
  res->dataset_b->metadata[0]->filename = strdup(nc_b.c_str());
  ASSERT_NE(nullptr, res->dataset_b->metadata[0]->filename);

  ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_get_filtered_product_b(
                              res.get(), "b.nc", &out_prod));
  ScopedHarpProduct filtered_b(out_prod);
  ASSERT_NE(nullptr, filtered_b.get());
  EXPECT_EQ(2L, filtered_b->dimension[harp_dimension_time]);

  harp_variable* temp_var = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                              filtered_b.get(), "temperature", &temp_var));
  // Sorted by index (0 then 2) during collocation mask application.
  EXPECT_DOUBLE_EQ(280.0, temp_var->data.double_data[0]);
  EXPECT_DOUBLE_EQ(300.0, temp_var->data.double_data[1]);

  // 5. Product present in dataset_b with 0 matching pairs returns NULL product.
  ASSERT_EQ(HARP_SUCCESS,
            harp_dataset_add_product(res->dataset_b, "empty_b.nc", nullptr));
  harp_product* empty_out = reinterpret_cast<harp_product*>(0x1);
  ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_get_filtered_product_b(
                              res.get(), "empty_b.nc", &empty_out));
  EXPECT_EQ(nullptr, empty_out);
}

TEST(HarpFilterCollocationTest, OutOfMemoryPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const int prev_opt = harp_get_option_create_collocation_datetime();
  const absl::Cleanup cleanup = [prev_opt] {
    harp_set_option_create_collocation_datetime(prev_opt);
    harp_set_error(HARP_SUCCESS, nullptr);
  };

  // 1. OOM inside harp_collocation_mask_import (using point_distance so
  // collocation_mask_from_result skips the leaking unit_converter path at
  // harp-filter-collocation.c:248).
  TempFile csv("colloc_mask_oom.csv");
  ASSERT_TRUE(WriteFileText(
      csv.path(),
      "collocation_index,source_product_a,index_a,source_product_b,index_b,"
      "point_distance [km]\n"
      "0,a.nc,0,b.nc,0,1.0\n"
      "1,a.nc,1,b.nc,1,2.0\n"));
  EXPECT_GT(
      SweepOomUntilSuccess([&]() {
        harp_collocation_mask* mask = nullptr;
        if (harp_collocation_mask_import(csv.c_str(), harp_collocation_left, -1,
                                         -1, "a.nc", &mask) == HARP_SUCCESS) {
          ScopedSuppressOom suppress;
          harp_collocation_mask_delete(mask);
        }
      }),
      0);

  // 2. OOM inside harp_product_apply_collocation_mask (index +
  // create_collocation_datetime branch; leak-clean on every failure path).
  ASSERT_EQ(HARP_SUCCESS, harp_set_option_create_collocation_datetime(1));
  harp_collocation_index_pair pair = {
      .collocation_index = 4, .index = 0, .datetime_diff = 1.0};
  harp_collocation_mask mask{};
  mask.num_index_pairs = 1;
  mask.index_pair = &pair;

  EXPECT_GT(
      SweepOomUntilSuccess([&]() {
        harp_product* prod = nullptr;
        {
          ScopedSuppressOom suppress;
          if (harp_product_new(&prod) != HARP_SUCCESS) {
            return;
          }
          const harp_dimension_type time_dim = harp_dimension_time;
          const long len = 2;
          harp_variable* idx = nullptr;
          harp_variable* dt = nullptr;
          if (harp_variable_new("index", harp_type_int32, 1, &time_dim, &len,
                                &idx) != HARP_SUCCESS ||
              harp_product_add_variable(prod, idx) != HARP_SUCCESS) {
            harp_variable_delete(idx);
            harp_product_delete(prod);
            return;
          }
          idx->data.int32_data[0] = 0;
          idx->data.int32_data[1] = 1;
          if (harp_variable_new("datetime", harp_type_double, 1, &time_dim,
                                &len, &dt) != HARP_SUCCESS ||
              harp_variable_set_unit(dt, HARP_UNIT_DATETIME) != HARP_SUCCESS ||
              harp_product_add_variable(prod, dt) != HARP_SUCCESS) {
            harp_variable_delete(dt);
            harp_product_delete(prod);
            return;
          }
          dt->data.double_data[0] = 100.0;
          dt->data.double_data[1] = 200.0;
        }
        harp_product_apply_collocation_mask(prod, &mask);
        ScopedSuppressOom suppress;
        harp_product_delete(prod);
      }),
      0);
}

}  // namespace
}  // namespace stcorp_harp
