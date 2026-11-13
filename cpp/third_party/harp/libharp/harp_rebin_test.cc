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
// Unit tests for libharp interval rebinning
// (libharp/harp-rebin.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-rebin.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp.h.in

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::ElementsAre;

// Creates a double variable, sets optional unit, and adds it to `product`.
void AddDoubleVar(harp_product* product, const char* name, const char* unit,
                  std::span<const harp_dimension_type> dim_types,
                  std::span<const long> dims, std::span<const double> values) {
  harp_variable* raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_new(name, harp_type_double,
                              static_cast<int>(dim_types.size()),
                              dim_types.data(), dims.data(), &raw));
  ScopedHarpVariable var(raw);
  if (unit != nullptr) {
    ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(var.get(), unit));
  }
  ASSERT_EQ(static_cast<long>(values.size()), var->num_elements);
  for (long i = 0; i < var->num_elements; ++i) {
    var->data.double_data[i] = values[static_cast<size_t>(i)];
  }
  ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(product, var.get()));
  var.release();
}

// Creates a float variable, sets optional unit, and adds it to `product`.
void AddFloatVar(harp_product* product, const char* name, const char* unit,
                 std::span<const harp_dimension_type> dim_types,
                 std::span<const long> dims, std::span<const float> values) {
  harp_variable* raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_new(name, harp_type_float,
                              static_cast<int>(dim_types.size()),
                              dim_types.data(), dims.data(), &raw));
  ScopedHarpVariable var(raw);
  if (unit != nullptr) {
    ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(var.get(), unit));
  }
  ASSERT_EQ(static_cast<long>(values.size()), var->num_elements);
  for (long i = 0; i < var->num_elements; ++i) {
    var->data.float_data[i] = values[static_cast<size_t>(i)];
  }
  ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(product, var.get()));
  var.release();
}

ScopedHarpVariable MakeDoubleBoundsVar(
    const char* name, const char* unit,
    std::span<const harp_dimension_type> dim_types, std::span<const long> dims,
    std::span<const double> values) {
  harp_variable* raw = nullptr;
  EXPECT_EQ(HARP_SUCCESS,
            harp_variable_new(name, harp_type_double,
                              static_cast<int>(dim_types.size()),
                              dim_types.data(), dims.data(), &raw));
  ScopedHarpVariable var(raw);
  if (var != nullptr) {
    if (unit != nullptr) {
      EXPECT_EQ(HARP_SUCCESS, harp_variable_set_unit(var.get(), unit));
    }
    for (long i = 0; i < var->num_elements; ++i) {
      var->data.double_data[i] = values[static_cast<size_t>(i)];
    }
  }
  return var;
}

TEST(HarpRebinTest, InvalidTargetBoundsValidationErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  // 1. Name does not end in "_bounds".
  {
    constexpr auto kDimTypes = std::to_array<harp_dimension_type>(
        {harp_dimension_vertical, harp_dimension_independent});
    constexpr auto kDims = std::to_array<long>({2, 2});
    constexpr auto kVals = std::to_array<double>({0.0, 1.0, 1.0, 2.0});
    ScopedHarpVariable tb =
        MakeDoubleBoundsVar("altitude", "m", kDimTypes, kDims, kVals);
    ASSERT_NE(nullptr, tb);
    EXPECT_EQ(
        -1, harp_product_rebin_with_axis_bounds_variable(prod.get(), tb.get()));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  }

  // 2. Non-double data type.
  {
    constexpr auto kDimTypes = std::to_array<harp_dimension_type>(
        {harp_dimension_vertical, harp_dimension_independent});
    constexpr auto kDims = std::to_array<long>({2, 2});
    harp_variable* raw_tb = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_new("altitude_bounds", harp_type_float, 2,
                                kDimTypes.data(), kDims.data(), &raw_tb));
    ScopedHarpVariable tb(raw_tb);
    EXPECT_EQ(
        -1, harp_product_rebin_with_axis_bounds_variable(prod.get(), tb.get()));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  }

  // 3. Invalid num_dimensions (1D and 4D).
  {
    constexpr auto kDimTypes1 =
        std::to_array<harp_dimension_type>({harp_dimension_vertical});
    constexpr auto kDims1 = std::to_array<long>({2});
    constexpr auto kVals1 = std::to_array<double>({0.0, 1.0});
    ScopedHarpVariable tb1 =
        MakeDoubleBoundsVar("altitude_bounds", "m", kDimTypes1, kDims1, kVals1);
    EXPECT_EQ(-1, harp_product_rebin_with_axis_bounds_variable(prod.get(),
                                                               tb1.get()));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

    constexpr auto kDimTypes4 = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_latitude, harp_dimension_vertical,
         harp_dimension_independent});
    constexpr auto kDims4 = std::to_array<long>({1, 1, 1, 2});
    constexpr auto kVals4 = std::to_array<double>({0.0, 1.0});
    ScopedHarpVariable tb4 =
        MakeDoubleBoundsVar("altitude_bounds", "m", kDimTypes4, kDims4, kVals4);
    EXPECT_EQ(-1, harp_product_rebin_with_axis_bounds_variable(prod.get(),
                                                               tb4.get()));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  }

  // 4. Independent second-to-last dimension.
  {
    constexpr auto kDimTypes = std::to_array<harp_dimension_type>(
        {harp_dimension_independent, harp_dimension_independent});
    constexpr auto kDims = std::to_array<long>({1, 2});
    constexpr auto kVals = std::to_array<double>({0.0, 1.0});
    ScopedHarpVariable tb =
        MakeDoubleBoundsVar("altitude_bounds", "m", kDimTypes, kDims, kVals);
    EXPECT_EQ(
        -1, harp_product_rebin_with_axis_bounds_variable(prod.get(), tb.get()));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  }

  // 5. 3D target_bounds: first dim != time, second dim == time, or time length
  // mismatch with product.
  {
    constexpr auto kVals = std::to_array<double>({0.0, 1.0});
    constexpr auto kDims = std::to_array<long>({1, 1, 2});
    constexpr auto kBadFirst = std::to_array<harp_dimension_type>(
        {harp_dimension_latitude, harp_dimension_vertical,
         harp_dimension_independent});
    ScopedHarpVariable tb_bad_first =
        MakeDoubleBoundsVar("altitude_bounds", "m", kBadFirst, kDims, kVals);
    EXPECT_EQ(-1, harp_product_rebin_with_axis_bounds_variable(
                      prod.get(), tb_bad_first.get()));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

    constexpr auto kGood3D = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_vertical,
         harp_dimension_independent});
    ScopedHarpVariable tb_bad_second =
        MakeDoubleBoundsVar("altitude_bounds", "m", kGood3D, kDims, kVals);
    ASSERT_NE(nullptr, tb_bad_second);
    tb_bad_second->dimension_type[1] = harp_dimension_time;
    EXPECT_EQ(-1, harp_product_rebin_with_axis_bounds_variable(
                      prod.get(), tb_bad_second.get()));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

    ScopedHarpVariable tb_time_mismatch =
        MakeDoubleBoundsVar("altitude_bounds", "m", kGood3D, kDims, kVals);
    ASSERT_NE(nullptr, tb_time_mismatch);
    EXPECT_EQ(-1, harp_product_rebin_with_axis_bounds_variable(
                      prod.get(), tb_time_mismatch.get()));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  }

  // 6. Invalid trailing independent dimension (wrong type or length != 2).
  {
    constexpr auto kBadLastType = std::to_array<harp_dimension_type>(
        {harp_dimension_vertical, harp_dimension_latitude});
    constexpr auto kDims2 = std::to_array<long>({1, 2});
    constexpr auto kVals2 = std::to_array<double>({0.0, 1.0});
    ScopedHarpVariable tb_type = MakeDoubleBoundsVar(
        "altitude_bounds", "m", kBadLastType, kDims2, kVals2);
    EXPECT_EQ(-1, harp_product_rebin_with_axis_bounds_variable(prod.get(),
                                                               tb_type.get()));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

    constexpr auto kGoodTypes = std::to_array<harp_dimension_type>(
        {harp_dimension_vertical, harp_dimension_independent});
    constexpr auto kBadLen = std::to_array<long>({1, 3});
    constexpr auto kVals3 = std::to_array<double>({0.0, 1.0, 2.0});
    ScopedHarpVariable tb_len = MakeDoubleBoundsVar(
        "altitude_bounds", "m", kGoodTypes, kBadLen, kVals3);
    EXPECT_EQ(-1, harp_product_rebin_with_axis_bounds_variable(prod.get(),
                                                               tb_len.get()));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  }

  // 7. Missing source bounds for time and non-time dimensions.
  {
    constexpr auto kTimeTypes = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_independent});
    constexpr auto kDims = std::to_array<long>({1, 2});
    constexpr auto kVals = std::to_array<double>({0.0, 10.0});
    ScopedHarpVariable tb_time = MakeDoubleBoundsVar(
        "datetime_bounds", HARP_UNIT_DATETIME, kTimeTypes, kDims, kVals);
    EXPECT_EQ(-1, harp_product_rebin_with_axis_bounds_variable(prod.get(),
                                                               tb_time.get()));

    constexpr auto kVertTypes = std::to_array<harp_dimension_type>(
        {harp_dimension_vertical, harp_dimension_independent});
    ScopedHarpVariable tb_vert =
        MakeDoubleBoundsVar("altitude_bounds", "m", kVertTypes, kDims, kVals);
    EXPECT_EQ(-1, harp_product_rebin_with_axis_bounds_variable(prod.get(),
                                                               tb_vert.get()));
  }
}

TEST(HarpRebinTest, VerticalRebinAverageSumAngleWeightsAndRemoval) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const double nan = std::numeric_limits<double>::quiet_NaN();

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  // Build product with time=2, vertical=3, spectral=2.
  // Source altitude_bounds: [0, 10], [10, 20], [20, 30].
  {
    constexpr auto kDims = std::to_array<harp_dimension_type>(
        {harp_dimension_vertical, harp_dimension_independent});
    constexpr auto kLen = std::to_array<long>({3, 2});
    constexpr auto kBounds =
        std::to_array<double>({0.0, 10.0, 10.0, 20.0, 20.0, 30.0});
    AddDoubleVar(prod.get(), "altitude_bounds", "m", kDims, kLen, kBounds);
  }

  // Variable without vertical dim -> binning_skip.
  {
    constexpr auto kDims =
        std::to_array<harp_dimension_type>({harp_dimension_time});
    constexpr auto kLen = std::to_array<long>({2});
    constexpr auto kVals = std::to_array<double>({100.0, 200.0});
    AddDoubleVar(prod.get(), "datetime", "s", kDims, kLen, kVals);
  }

  // Variable with 2 vertical dims -> binning_remove.
  {
    constexpr auto kDims = std::to_array<harp_dimension_type>(
        {harp_dimension_vertical, harp_dimension_vertical});
    constexpr auto kLen = std::to_array<long>({3, 3});
    std::array<double, 9> vals{};
    vals.fill(1.0);
    AddDoubleVar(prod.get(), "matrix", "1", kDims, kLen, vals);
  }

  // *count, string, unitless, enum, and *_avk variables -> binning_remove.
  {
    constexpr auto kDims =
        std::to_array<harp_dimension_type>({harp_dimension_vertical});
    constexpr auto kLen = std::to_array<long>({3});
    constexpr auto kVals = std::to_array<double>({1.0, 2.0, 3.0});
    AddDoubleVar(prod.get(), "sample_count", "1", kDims, kLen, kVals);
    AddDoubleVar(prod.get(), "unitless_var", nullptr, kDims, kLen, kVals);
    AddDoubleVar(prod.get(), "O3_volume_mixing_ratio_avk", "1", kDims, kLen,
                 kVals);

    harp_variable* raw_str = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_new("label", harp_type_string, 1, kDims.data(),
                                kLen.data(), &raw_str));
    ScopedHarpVariable str_var(raw_str);
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_set_string_data_element(str_var.get(), 0, "a"));
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_set_string_data_element(str_var.get(), 1, "b"));
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_set_string_data_element(str_var.get(), 2, "c"));
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_add_variable(prod.get(), str_var.get()));
    str_var.release();

    harp_variable* raw_enum = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_new("quality_flag", harp_type_int8, 1, kDims.data(),
                                kLen.data(), &raw_enum));
    ScopedHarpVariable enum_var(raw_enum);
    ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(enum_var.get(), "1"));
    const char* enum_names[] = {"good", "bad"};
    ASSERT_EQ(HARP_SUCCESS, harp_variable_set_enumeration_values(
                                enum_var.get(), 2, enum_names));
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_add_variable(prod.get(), enum_var.get()));
    enum_var.release();
  }

  // Invalid weight variables -> binning_remove.
  {
    // Weight with unit != NULL -> removed.
    constexpr auto kDims = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_vertical});
    constexpr auto kLen = std::to_array<long>({2, 3});
    constexpr auto kVals =
        std::to_array<float>({1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f});
    AddFloatVar(prod.get(), "bad_unit_weight", "1", kDims, kLen, kVals);

    // Weight with mismatched dimensions vs target variable -> removed via
    // get_weight_variable_for_variable.
    constexpr auto kBadDims = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_vertical,
         harp_dimension_spectral});
    constexpr auto kBadLen = std::to_array<long>({2, 3, 2});
    std::array<float, 12> bad_w{};
    bad_w.fill(1.0f);
    AddFloatVar(prod.get(), "temperature_weight", nullptr, kBadDims, kBadLen,
                bad_w);
  }

  // Generic `weight` {time, vertical} and `temperature` {time, vertical} (float
  // to cover convert_data_type).
  {
    constexpr auto kDims = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_vertical});
    constexpr auto kLen = std::to_array<long>({2, 3});
    constexpr float fnan = std::numeric_limits<float>::quiet_NaN();
    const auto kWeight =
        std::to_array<float>({1.0f, 3.0f, 1.0f, 1.0f, fnan, 1.0f});
    AddFloatVar(prod.get(), "weight", nullptr, kDims, kLen, kWeight);

    constexpr auto kTemp =
        std::to_array<float>({10.0f, 20.0f, 30.0f, 100.0f, 200.0f, 300.0f});
    AddFloatVar(prod.get(), "temperature", "K", kDims, kLen, kTemp);
  }

  // Variable with time, vertical, and spectral dims (`num_blocks > 1` and
  // `num_elements > 1`) using generic `weight` broadcast over `spectral`.
  {
    constexpr auto kDims = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_vertical,
         harp_dimension_spectral});
    constexpr auto kLen = std::to_array<long>({2, 3, 2});
    const auto kSpec = std::to_array<double>(
        {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 10.0, 20.0, nan, 40.0, 50.0, 60.0});
    AddDoubleVar(prod.get(), "radiance", "W/m2", kDims, kLen, kSpec);
  }

  // Partial column integrated quantities -> binning_sum.
  {
    constexpr auto kDims = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_vertical});
    constexpr auto kLen = std::to_array<long>({2, 3});
    constexpr auto kCol =
        std::to_array<double>({10.0, 20.0, 30.0, 1.0, 2.0, 3.0});
    AddDoubleVar(prod.get(), "O3_column_number_density", "molec/m2", kDims,
                 kLen, kCol);
    AddDoubleVar(prod.get(), "O3_column_number_density_apriori", "molec/m2",
                 kDims, kLen, kCol);
    AddDoubleVar(prod.get(), "O3_column_number_density_avk", "1", kDims, kLen,
                 kCol);
    AddDoubleVar(prod.get(), "O3_column_number_density_dfs", "1", kDims, kLen,
                 kCol);
    // Column variable not ending in density/apriori/avk/dfs falls through to
    // binning_average.
    AddDoubleVar(prod.get(), "O3_column_number_density_uncertainty", "molec/m2",
                 kDims, kLen, kCol);

    // `<var>_weight` with same num_dimensions as variable but mismatched dim
    // type hits get_weight_variable_for_variable mismatch branch.
    constexpr auto kMisDims = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_spectral});
    constexpr auto kMisLen = std::to_array<long>({2, 2});
    constexpr auto kMisW = std::to_array<float>({1.0f, 1.0f, 1.0f, 1.0f});
    AddFloatVar(prod.get(), "O3_column_number_density_apriori_weight", nullptr,
                kMisDims, kMisLen, kMisW);
    // `<var>_weight` with unit != NULL -> bintype == binning_remove branch.
    AddFloatVar(prod.get(), "O3_column_number_density_dfs_weight", "1", kDims,
                kLen,
                std::to_array<float>({1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f}));
  }

  // Angle variables: one with `<name>_weight` (covering norm==0 and NaN) and
  // one without `<name>_weight` (covering NaN angle).
  {
    constexpr auto kDims = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_vertical});
    constexpr auto kLen = std::to_array<long>({2, 3});
    const auto kAzimuth =
        std::to_array<double>({350.0, 10.0, nan, 90.0, 90.0, 90.0});
    AddDoubleVar(prod.get(), "wind_direction", "degree", kDims, kLen, kAzimuth);
    constexpr auto kAzWeight =
        std::to_array<float>({1.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f});
    AddFloatVar(prod.get(), "wind_direction_weight", nullptr, kDims, kLen,
                kAzWeight);

    const auto kAngle =
        std::to_array<double>({0.0, 90.0, nan, 180.0, 180.0, 180.0});
    AddDoubleVar(prod.get(), "sensor_zenith_angle", "degree", kDims, kLen,
                 kAngle);
  }

  // Target bounds: 4 bins (grows vertical from 3 -> 4), including an uncovered
  // bin [40, 50] -> NaN and weight=0.
  constexpr auto kTargetDimTypes = std::to_array<harp_dimension_type>(
      {harp_dimension_vertical, harp_dimension_independent});
  constexpr auto kTargetDims = std::to_array<long>({4, 2});
  constexpr auto kTargetVals =
      std::to_array<double>({0.0, 20.0, 20.0, 30.0, 5.0, 15.0, 40.0, 50.0});
  ScopedHarpVariable target_bounds = MakeDoubleBoundsVar(
      "altitude_bounds", "m", kTargetDimTypes, kTargetDims, kTargetVals);
  ASSERT_NE(nullptr, target_bounds);

  ASSERT_EQ(HARP_SUCCESS, harp_product_rebin_with_axis_bounds_variable(
                              prod.get(), target_bounds.get()));
  EXPECT_EQ(4, prod->dimension[harp_dimension_vertical]);

  // Verify removed variables are gone.
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "matrix"));
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "sample_count"));
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "unitless_var"));
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "label"));
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "quality_flag"));
  EXPECT_EQ(
      0, harp_product_has_variable(prod.get(), "O3_volume_mixing_ratio_avk"));
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "bad_unit_weight"));
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "temperature_weight"));
  EXPECT_EQ(1, harp_product_has_variable(prod.get(), "datetime"));

  // Check temperature: for t=0, bin 0 ([0,20]) has weights 1*10 + 3*20 / (1+3)
  // = 17.5; bin 3 ([40,50]) is NaN.
  harp_variable* temp_out = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                              prod.get(), "temperature", &temp_out));
  EXPECT_EQ(harp_type_double, temp_out->data_type);
  EXPECT_NEAR(17.5, temp_out->data.double_data[0], 1e-12);
  EXPECT_NEAR(30.0, temp_out->data.double_data[1], 1e-12);
  EXPECT_NEAR(17.5, temp_out->data.double_data[2], 1e-12);
  EXPECT_EQ(1, harp_isnan(temp_out->data.double_data[3]));

  // Check wind_direction: bin 0 averages 350 and 10 deg -> ~0 deg.
  harp_variable* wind_out = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                              prod.get(), "wind_direction", &wind_out));
  EXPECT_NEAR(0.0, wind_out->data.double_data[0], 1e-10);
  EXPECT_EQ(1, harp_isnan(wind_out->data.double_data[1]));
  EXPECT_EQ(1, harp_isnan(wind_out->data.double_data[3]));

  // Check O3_column_number_density: weighted sum, bin 0 ([0,20]) has
  // weights 1 and 3 on [0,10]=10 and [10,20]=20 -> 1*10 + 3*20 = 70.
  harp_variable* o3_out = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                              prod.get(), "O3_column_number_density", &o3_out));
  EXPECT_NEAR(70.0, o3_out->data.double_data[0], 1e-12);
}

TEST(HarpRebinTest, TimeDimensionDescendingPressureAndMultiOverlapRealloc) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  // 1. Rebin along time dimension (derives datetime_bounds from
  // datetime_start/datetime_stop, removes time axis variables, shrinks dim).
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);

    constexpr auto kT =
        std::to_array<harp_dimension_type>({harp_dimension_time});
    constexpr auto kTLen = std::to_array<long>({4});
    constexpr auto kStart = std::to_array<double>({0.0, 10.0, 20.0, 30.0});
    constexpr auto kStop = std::to_array<double>({10.0, 20.0, 30.0, 40.0});
    constexpr auto kVal = std::to_array<double>({1.0, 2.0, 3.0, 4.0});
    AddDoubleVar(prod.get(), "datetime_start", HARP_UNIT_DATETIME, kT, kTLen,
                 kStart);
    AddDoubleVar(prod.get(), "datetime_stop", HARP_UNIT_DATETIME, kT, kTLen,
                 kStop);
    AddDoubleVar(prod.get(), "datetime", HARP_UNIT_DATETIME, kT, kTLen, kStart);
    AddDoubleVar(prod.get(), "datetime_length", "s", kT, kTLen, kStart);
    AddDoubleVar(prod.get(), "temperature", "K", kT, kTLen, kVal);

    constexpr auto kTbTypes = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_independent});
    constexpr auto kTbDims = std::to_array<long>({2, 2});
    constexpr auto kTbVals = std::to_array<double>({0.0, 20.0, 20.0, 40.0});
    ScopedHarpVariable tb = MakeDoubleBoundsVar(
        "datetime_bounds", HARP_UNIT_DATETIME, kTbTypes, kTbDims, kTbVals);
    ASSERT_EQ(HARP_SUCCESS, harp_product_rebin_with_axis_bounds_variable(
                                prod.get(), tb.get()));
    EXPECT_EQ(2, prod->dimension[harp_dimension_time]);
    EXPECT_EQ(0, harp_product_has_variable(prod.get(), "datetime"));
    EXPECT_EQ(0, harp_product_has_variable(prod.get(), "datetime_start"));
    EXPECT_EQ(0, harp_product_has_variable(prod.get(), "datetime_stop"));
    EXPECT_EQ(0, harp_product_has_variable(prod.get(), "datetime_length"));
    harp_variable* temp = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                                prod.get(), "temperature", &temp));
    EXPECT_NEAR(1.5, temp->data.double_data[0], 1e-12);
    EXPECT_NEAR(3.5, temp->data.double_data[1], 1e-12);
  }

  // 2. Vertical pressure_bounds (log axis), descending source & target bounds,
  // 3D time-dependent bounds promotion, and > BLOCK_SIZE (16) overlaps to
  // cover add_source_index realloc.
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);

    // 20 vertical layers so a single wide target bin overlaps 20 > 16
    // intervals.
    constexpr long kNumSrc = 20;
    std::vector<double> src_p(static_cast<size_t>(kNumSrc) * 2);
    std::vector<double> src_t(static_cast<size_t>(kNumSrc), 250.0);
    for (long k = 0; k < kNumSrc; ++k) {
      // Descending pressure bounds: [1000..950], [950..900], ...
      src_p[static_cast<size_t>(2 * k)] = 1000.0 - 40.0 * k;
      src_p[static_cast<size_t>(2 * k + 1)] = 1000.0 - 40.0 * (k + 1);
    }
    constexpr auto kPDims = std::to_array<harp_dimension_type>(
        {harp_dimension_vertical, harp_dimension_independent});
    constexpr auto kPLen = std::to_array<long>({kNumSrc, 2});
    AddDoubleVar(prod.get(), "pressure_bounds", "hPa", kPDims, kPLen, src_p);

    // Also include time-independent altitude_gph, pressure, and temperature.
    constexpr auto kVDims =
        std::to_array<harp_dimension_type>({harp_dimension_vertical});
    constexpr auto kVLen = std::to_array<long>({kNumSrc});
    AddDoubleVar(prod.get(), "pressure", "hPa", kVDims, kVLen, src_t);
    AddDoubleVar(prod.get(), "altitude_gph", "m", kVDims, kVLen, src_t);
    AddDoubleVar(prod.get(), "temperature", "K", kVDims, kVLen, src_t);

    // Add time dimension = 2 and test 3D target_bounds promoting time-indep
    // temperature to {time, vertical}.
    constexpr auto kTDims =
        std::to_array<harp_dimension_type>({harp_dimension_time});
    constexpr auto kTLen = std::to_array<long>({2});
    constexpr auto kDt = std::to_array<double>({0.0, 1.0});
    AddDoubleVar(prod.get(), "datetime", "s", kTDims, kTLen, kDt);

    constexpr auto kTargetDimTypes = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_vertical,
         harp_dimension_independent});
    constexpr auto kTargetDims = std::to_array<long>({2, 1, 2});
    constexpr auto kTargetVals =
        std::to_array<double>({1000.0, 200.0, 1000.0, 200.0});
    ScopedHarpVariable tb = MakeDoubleBoundsVar(
        "pressure_bounds", "hPa", kTargetDimTypes, kTargetDims, kTargetVals);

    ASSERT_EQ(HARP_SUCCESS, harp_product_rebin_with_axis_bounds_variable(
                                prod.get(), tb.get()));
    EXPECT_EQ(1, prod->dimension[harp_dimension_vertical]);
    EXPECT_EQ(0, harp_product_has_variable(prod.get(), "pressure"));
    EXPECT_EQ(0, harp_product_has_variable(prod.get(), "altitude_gph"));

    harp_variable* temp = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                                prod.get(), "temperature", &temp));
    EXPECT_EQ(2, temp->num_dimensions);
    EXPECT_NEAR(250.0, temp->data.double_data[0], 1e-10);
  }

  // 3. Latitude, longitude, and spectral axis removal branches, plus invalid
  // angle unit error path.
  {
    struct AxisCase {
      harp_dimension_type dim_type;
      const char* bounds_name;
      const char* axis_name;
      const char* unit;
    };
    constexpr AxisCase kAxisCases[] = {
        {.dim_type = harp_dimension_latitude,
         .bounds_name = "latitude_bounds",
         .axis_name = "latitude",
         .unit = "degree_north"},
        {.dim_type = harp_dimension_longitude,
         .bounds_name = "longitude_bounds",
         .axis_name = "longitude",
         .unit = "degree_east"},
        {.dim_type = harp_dimension_spectral,
         .bounds_name = "wavelength_bounds",
         .axis_name = "wavelength",
         .unit = "m"},
    };
    for (const auto& ac : kAxisCases) {
      harp_product* raw_prod = nullptr;
      ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
      ScopedHarpProduct prod(raw_prod);

      constexpr auto kBTypes = std::to_array<harp_dimension_type>(
          {harp_dimension_time, harp_dimension_independent});
      const auto b_types = std::to_array<harp_dimension_type>(
          {ac.dim_type, harp_dimension_independent});
      (void)kBTypes;
      constexpr auto kBLen = std::to_array<long>({2, 2});
      constexpr auto kBVals = std::to_array<double>({1.0, 2.0, 2.0, 3.0});
      AddDoubleVar(prod.get(), ac.bounds_name, ac.unit, b_types, kBLen, kBVals);

      const auto a_types = std::to_array<harp_dimension_type>({ac.dim_type});
      constexpr auto kALen = std::to_array<long>({2});
      constexpr auto kAVals = std::to_array<double>({1.5, 2.5});
      AddDoubleVar(prod.get(), ac.axis_name, ac.unit, a_types, kALen, kAVals);
      if (ac.dim_type == harp_dimension_spectral) {
        AddDoubleVar(prod.get(), "wavenumber", "1/m", a_types, kALen, kAVals);
        AddDoubleVar(prod.get(), "frequency", "Hz", a_types, kALen, kAVals);
      }
      AddDoubleVar(prod.get(), "val", "1", a_types, kALen, kAVals);

      constexpr auto kTbLen = std::to_array<long>({1, 2});
      constexpr auto kTbVals = std::to_array<double>({1.0, 3.0});
      ScopedHarpVariable tb = MakeDoubleBoundsVar(ac.bounds_name, ac.unit,
                                                  b_types, kTbLen, kTbVals);
      ASSERT_EQ(HARP_SUCCESS, harp_product_rebin_with_axis_bounds_variable(
                                  prod.get(), tb.get()));
      EXPECT_EQ(0, harp_product_has_variable(prod.get(), ac.axis_name));
    }

    // Angle variable with non-radian-convertible unit triggers error.
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);
    constexpr auto kBTypes = std::to_array<harp_dimension_type>(
        {harp_dimension_vertical, harp_dimension_independent});
    constexpr auto kBLen = std::to_array<long>({2, 2});
    constexpr auto kBVals = std::to_array<double>({0.0, 10.0, 10.0, 20.0});
    AddDoubleVar(prod.get(), "altitude_bounds", "m", kBTypes, kBLen, kBVals);
    constexpr auto kVTypes =
        std::to_array<harp_dimension_type>({harp_dimension_vertical});
    constexpr auto kVLen = std::to_array<long>({2});
    constexpr auto kVVals = std::to_array<double>({10.0, 20.0});
    AddDoubleVar(prod.get(), "viewing_angle", "m", kVTypes, kVLen, kVVals);

    constexpr auto kTbLen = std::to_array<long>({1, 2});
    constexpr auto kTbVals = std::to_array<double>({0.0, 20.0});
    ScopedHarpVariable tb =
        MakeDoubleBoundsVar("altitude_bounds", "m", kBTypes, kTbLen, kTbVals);
    EXPECT_EQ(
        -1, harp_product_rebin_with_axis_bounds_variable(prod.get(), tb.get()));
  }

  // 4. 3D time-dependent source_bounds with 2D target_bounds, 3D weight
  // variable (dimensions after rebinned dim), and invalid generic `weight`
  // fallback paths.
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);

    constexpr auto kSBTypes = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_vertical,
         harp_dimension_independent});
    constexpr auto kSBLen = std::to_array<long>({2, 2, 2});
    constexpr auto kSBVals =
        std::to_array<double>({0.0, 10.0, 10.0, 20.0, 0.0, 10.0, 10.0, 20.0});
    AddDoubleVar(prod.get(), "altitude_bounds", "m", kSBTypes, kSBLen, kSBVals);

    constexpr auto kTVSTypes = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_vertical,
         harp_dimension_spectral});
    constexpr auto kTVSLen = std::to_array<long>({2, 2, 2});
    constexpr auto kTVSVals =
        std::to_array<double>({1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0});
    AddDoubleVar(prod.get(), "radiance", "W/m2", kTVSTypes, kTVSLen, kTVSVals);
    constexpr auto kWVals =
        std::to_array<float>({1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f});
    AddFloatVar(prod.get(), "weight", nullptr, kTVSTypes, kTVSLen, kWVals);

    // 2D target_bounds against 3D source_bounds.
    constexpr auto kTBTypes = std::to_array<harp_dimension_type>(
        {harp_dimension_vertical, harp_dimension_independent});
    constexpr auto kTBLen = std::to_array<long>({1, 2});
    constexpr auto kTBVals = std::to_array<double>({0.0, 20.0});
    ScopedHarpVariable tb =
        MakeDoubleBoundsVar("altitude_bounds", "m", kTBTypes, kTBLen, kTBVals);
    ASSERT_EQ(HARP_SUCCESS, harp_product_rebin_with_axis_bounds_variable(
                                prod.get(), tb.get()));
    EXPECT_EQ(1, prod->dimension[harp_dimension_vertical]);
  }
  {
    // 3D target_bounds + 3D weight variable exercises lines 988-989 &
    // 1002-1003, plus a 2D {time, vertical} variable whose generic `weight` has
    // extra dimensions (rejected by get_weight_for_variable).
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);

    constexpr auto kSBTypes = std::to_array<harp_dimension_type>(
        {harp_dimension_vertical, harp_dimension_independent});
    constexpr auto kSBLen = std::to_array<long>({2, 2});
    constexpr auto kSBVals = std::to_array<double>({0.0, 10.0, 10.0, 20.0});
    AddDoubleVar(prod.get(), "altitude_bounds", "m", kSBTypes, kSBLen, kSBVals);

    constexpr auto kTVTypes = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_vertical});
    constexpr auto kTVLen = std::to_array<long>({2, 2});
    constexpr auto kTemp = std::to_array<double>({10.0, 20.0, 30.0, 40.0});
    AddDoubleVar(prod.get(), "temperature", "K", kTVTypes, kTVLen, kTemp);

    constexpr auto kTVSTypes = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_vertical,
         harp_dimension_spectral});
    constexpr auto kTVSLen = std::to_array<long>({2, 2, 2});
    constexpr auto kWVals =
        std::to_array<float>({1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f});
    AddFloatVar(prod.get(), "weight", nullptr, kTVSTypes, kTVSLen, kWVals);

    constexpr auto kTBTypes = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_vertical,
         harp_dimension_independent});
    constexpr auto kTBLen = std::to_array<long>({2, 1, 2});
    constexpr auto kTBVals = std::to_array<double>({0.0, 20.0, 0.0, 20.0});
    ScopedHarpVariable tb =
        MakeDoubleBoundsVar("altitude_bounds", "m", kTBTypes, kTBLen, kTBVals);
    ASSERT_EQ(HARP_SUCCESS, harp_product_rebin_with_axis_bounds_variable(
                                prod.get(), tb.get()));
    EXPECT_EQ(1, prod->dimension[harp_dimension_vertical]);
  }
}

}  // namespace
}  // namespace stcorp_harp
