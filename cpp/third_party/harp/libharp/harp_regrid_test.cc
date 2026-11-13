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
// Unit tests for libharp regridding and dimension clamping
// (libharp/harp-regrid.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-regrid.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-internal.h

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/google/util/temp_file.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::ElementsAre;

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

void AddInt32Var(harp_product* product, const char* name, const char* unit,
                 std::span<const harp_dimension_type> dim_types,
                 std::span<const long> dims, std::span<const int32_t> values) {
  harp_variable* raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_new(name, harp_type_int32,
                              static_cast<int>(dim_types.size()),
                              dim_types.data(), dims.data(), &raw));
  ScopedHarpVariable var(raw);
  if (unit != nullptr) {
    ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(var.get(), unit));
  }
  ASSERT_EQ(static_cast<long>(values.size()), var->num_elements);
  for (long i = 0; i < var->num_elements; ++i) {
    var->data.int32_data[i] = values[static_cast<size_t>(i)];
  }
  ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(product, var.get()));
  var.release();
}

ScopedHarpVariable MakeDoubleVar(const char* name, const char* unit,
                                 std::span<const harp_dimension_type> dim_types,
                                 std::span<const long> dims,
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

TEST(HarpRegridTest, GetDerivedBoundsForGrid) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  constexpr auto kTimeTypes =
      std::to_array<harp_dimension_type>({harp_dimension_time});
  constexpr auto kTimeDims = std::to_array<long>({2});
  constexpr auto kStart = std::to_array<double>({0.0, 10.0});
  constexpr auto kStop = std::to_array<double>({5.0, 15.0});
  AddDoubleVar(prod.get(), "datetime_start", "s", kTimeTypes, kTimeDims,
               kStart);
  AddDoubleVar(prod.get(), "datetime_stop", "s", kTimeTypes, kTimeDims, kStop);

  constexpr auto kMid = std::to_array<double>({2.5, 12.5});
  ScopedHarpVariable dt_grid =
      MakeDoubleVar("datetime", "s", kTimeTypes, kTimeDims, kMid);
  harp_variable* raw_bounds = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_derived_bounds_for_grid(
                              prod.get(), dt_grid.get(), &raw_bounds));
  ScopedHarpVariable dt_bounds(raw_bounds);
  EXPECT_EQ(2, dt_bounds->num_dimensions);
  EXPECT_DOUBLE_EQ(0.0, dt_bounds->data.double_data[0]);
  EXPECT_DOUBLE_EQ(5.0, dt_bounds->data.double_data[1]);
  EXPECT_DOUBLE_EQ(10.0, dt_bounds->data.double_data[2]);
  EXPECT_DOUBLE_EQ(15.0, dt_bounds->data.double_data[3]);

  // Missing datetime_stop triggers error.
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_remove_variable_by_name(prod.get(), "datetime_stop"));
  EXPECT_EQ(-1, harp_product_get_derived_bounds_for_grid(
                    prod.get(), dt_grid.get(), &raw_bounds));

  // Missing datetime_start triggers error.
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_remove_variable_by_name(prod.get(), "datetime_start"));
  EXPECT_EQ(-1, harp_product_get_derived_bounds_for_grid(
                    prod.get(), dt_grid.get(), &raw_bounds));

  // Non-time grid derives <name>_bounds from midpoints.
  constexpr auto kVertTypes =
      std::to_array<harp_dimension_type>({harp_dimension_vertical});
  constexpr auto kVertDims = std::to_array<long>({2});
  constexpr auto kAlt = std::to_array<double>({1000.0, 2000.0});
  AddDoubleVar(prod.get(), "altitude", "m", kVertTypes, kVertDims, kAlt);
  ScopedHarpVariable alt_grid =
      MakeDoubleVar("altitude", "m", kVertTypes, kVertDims, kAlt);
  raw_bounds = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_derived_bounds_for_grid(
                              prod.get(), alt_grid.get(), &raw_bounds));
  ScopedHarpVariable alt_bounds(raw_bounds);
  EXPECT_EQ(2, alt_bounds->num_dimensions);
  EXPECT_EQ(4, alt_bounds->num_elements);

  // Missing altitude triggers error.
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_remove_variable_by_name(prod.get(), "altitude"));
  EXPECT_EQ(-1, harp_product_get_derived_bounds_for_grid(
                    prod.get(), alt_grid.get(), &raw_bounds));
}

TEST(HarpRegridTest, RegridWithAxisVariableValidationErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  // 1. Wrong target_grid data type.
  {
    constexpr auto kTypes =
        std::to_array<harp_dimension_type>({harp_dimension_vertical});
    constexpr auto kDims = std::to_array<long>({2});
    harp_variable* raw = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_new("altitude", harp_type_float, 1, kTypes.data(),
                                kDims.data(), &raw));
    ScopedHarpVariable tg(raw);
    EXPECT_EQ(-1, harp_product_regrid_with_axis_variable(prod.get(), tg.get(),
                                                         nullptr));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  }

  // 2. Invalid target_grid num_dimensions (3D) or independent last dim.
  {
    constexpr auto kTypes3 = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_latitude,
         harp_dimension_vertical});
    constexpr auto kDims3 = std::to_array<long>({1, 1, 2});
    constexpr auto kVals = std::to_array<double>({1.0, 2.0});
    ScopedHarpVariable tg3 =
        MakeDoubleVar("altitude", "m", kTypes3, kDims3, kVals);
    EXPECT_EQ(-1, harp_product_regrid_with_axis_variable(prod.get(), tg3.get(),
                                                         nullptr));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

    constexpr auto kTypesIndep =
        std::to_array<harp_dimension_type>({harp_dimension_independent});
    constexpr auto kDims1 = std::to_array<long>({2});
    ScopedHarpVariable tg_indep =
        MakeDoubleVar("altitude", "m", kTypesIndep, kDims1, kVals);
    EXPECT_EQ(-1, harp_product_regrid_with_axis_variable(
                      prod.get(), tg_indep.get(), nullptr));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  }

  // 3. 2D target_grid errors: first dim != time, second dim == time, or time
  // length mismatch.
  {
    constexpr auto kDims = std::to_array<long>({1, 2});
    constexpr auto kVals = std::to_array<double>({1.0, 2.0});
    constexpr auto kBadFirst = std::to_array<harp_dimension_type>(
        {harp_dimension_latitude, harp_dimension_vertical});
    ScopedHarpVariable tg1 =
        MakeDoubleVar("altitude", "m", kBadFirst, kDims, kVals);
    EXPECT_EQ(-1, harp_product_regrid_with_axis_variable(prod.get(), tg1.get(),
                                                         nullptr));

    constexpr auto kGood2D = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_vertical});
    ScopedHarpVariable tg2 =
        MakeDoubleVar("datetime", "s", kGood2D, kDims, kVals);
    ASSERT_NE(nullptr, tg2);
    tg2->dimension_type[1] = harp_dimension_time;
    EXPECT_EQ(-1, harp_product_regrid_with_axis_variable(prod.get(), tg2.get(),
                                                         nullptr));

    ScopedHarpVariable tg3 =
        MakeDoubleVar("altitude", "m", kGood2D, kDims, kVals);
    ASSERT_NE(nullptr, tg3);
    EXPECT_EQ(-1, harp_product_regrid_with_axis_variable(prod.get(), tg3.get(),
                                                         nullptr));
  }

  // 4. target_bounds validation errors.
  {
    constexpr auto kTypes =
        std::to_array<harp_dimension_type>({harp_dimension_vertical});
    constexpr auto kDims = std::to_array<long>({2});
    constexpr auto kVals = std::to_array<double>({1.0, 2.0});
    ScopedHarpVariable tg =
        MakeDoubleVar("altitude", "m", kTypes, kDims, kVals);

    // Wrong data_type.
    constexpr auto kBTypes = std::to_array<harp_dimension_type>(
        {harp_dimension_vertical, harp_dimension_independent});
    constexpr auto kBDims = std::to_array<long>({2, 2});
    harp_variable* raw_tb = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_new("altitude_bounds", harp_type_float, 2,
                                kBTypes.data(), kBDims.data(), &raw_tb));
    ScopedHarpVariable tb_float(raw_tb);
    EXPECT_EQ(-1, harp_product_regrid_with_axis_variable(prod.get(), tg.get(),
                                                         tb_float.get()));

    // Wrong num_dimensions (1D instead of 2D).
    ScopedHarpVariable tb_1d =
        MakeDoubleVar("altitude_bounds", "m", kTypes, kDims, kVals);
    EXPECT_EQ(-1, harp_product_regrid_with_axis_variable(prod.get(), tg.get(),
                                                         tb_1d.get()));

    // Mismatched dim0 length.
    constexpr auto kBDimsMismatch = std::to_array<long>({1, 2});
    ScopedHarpVariable tb_dim0 =
        MakeDoubleVar("altitude_bounds", "m", kBTypes, kBDimsMismatch, kVals);
    EXPECT_EQ(-1, harp_product_regrid_with_axis_variable(prod.get(), tg.get(),
                                                         tb_dim0.get()));

    // Invalid last dimension (length 3 instead of 2).
    constexpr auto kBDims3 = std::to_array<long>({2, 3});
    constexpr auto kVals6 = std::to_array<double>({0, 1, 2, 3, 4, 5});
    ScopedHarpVariable tb_last =
        MakeDoubleVar("altitude_bounds", "m", kBTypes, kBDims3, kVals6);
    EXPECT_EQ(-1, harp_product_regrid_with_axis_variable(prod.get(), tg.get(),
                                                         tb_last.get()));

    // 2D target_grid with mismatched dim1 in target_bounds.
    constexpr auto kTDims =
        std::to_array<harp_dimension_type>({harp_dimension_time});
    constexpr auto kTLen = std::to_array<long>({1});
    constexpr auto kTVal = std::to_array<double>({0.0});
    AddDoubleVar(prod.get(), "datetime", "s", kTDims, kTLen, kTVal);

    constexpr auto kTg2Types = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_vertical});
    constexpr auto kTg2Dims = std::to_array<long>({1, 2});
    ScopedHarpVariable tg2 =
        MakeDoubleVar("altitude", "m", kTg2Types, kTg2Dims, kVals);

    constexpr auto kTb3Types = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_vertical,
         harp_dimension_independent});
    constexpr auto kTb3Dims = std::to_array<long>({1, 1, 2});
    ScopedHarpVariable tb_dim1 =
        MakeDoubleVar("altitude_bounds", "m", kTb3Types, kTb3Dims, kVals);
    EXPECT_EQ(-1, harp_product_regrid_with_axis_variable(prod.get(), tg2.get(),
                                                         tb_dim1.get()));
  }

  // 5. Missing source datetime grid on time regrid, and source length <= 1.
  {
    constexpr auto kT =
        std::to_array<harp_dimension_type>({harp_dimension_time});
    constexpr auto kTLen2 = std::to_array<long>({2});
    constexpr auto kVals2 = std::to_array<double>({0.0, 10.0});
    ScopedHarpVariable dt_tg =
        MakeDoubleVar("datetime", "s", kT, kTLen2, kVals2);
    // prod currently has datetime of length 1 -> triggers length <= 1 check
    // (and removes datetime before failing).
    AddDoubleVar(prod.get(), "temperature", "K", kT, std::to_array<long>({1}),
                 std::to_array<double>({280.0}));
    EXPECT_EQ(-1, harp_product_regrid_with_axis_variable(prod.get(),
                                                         dt_tg.get(), nullptr));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
    EXPECT_EQ(0, harp_product_has_variable(prod.get(), "datetime"));

    EXPECT_EQ(-1, harp_product_regrid_with_axis_variable(prod.get(),
                                                         dt_tg.get(), nullptr));
  }
}

TEST(HarpRegridTest, LinearIntervalPressureLogLogAndOutOfBounds) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const int prev_oob = harp_get_option_regrid_out_of_bounds();
  auto restore_oob = absl::MakeCleanup(
      [prev_oob] { harp_set_option_regrid_out_of_bounds(prev_oob); });

  // 1. Vertical pressure grid with interval + linear + removed variables.
  {
    ASSERT_EQ(HARP_SUCCESS, harp_set_option_regrid_out_of_bounds(1));
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);

    constexpr auto kT =
        std::to_array<harp_dimension_type>({harp_dimension_time});
    constexpr auto kTLen = std::to_array<long>({2});
    constexpr auto kDt = std::to_array<double>({0.0, 10.0});
    AddDoubleVar(prod.get(), "datetime", "s", kT, kTLen, kDt);

    // 2D time-dependent pressure grid with NaN padding on second time step.
    constexpr auto kTV = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_vertical});
    constexpr auto kTVLen = std::to_array<long>({2, 3});
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    const auto kSrcP =
        std::to_array<double>({1000.0, 500.0, 100.0, 1000.0, 200.0, nan});
    AddDoubleVar(prod.get(), "pressure", "hPa", kTV, kTVLen, kSrcP);

    // Time-independent temperature (will be promoted to {time, vertical}) and
    // O3_column_number_density (uses interval interpolation + auto bounds).
    constexpr auto kV =
        std::to_array<harp_dimension_type>({harp_dimension_vertical});
    constexpr auto kVLen = std::to_array<long>({3});
    constexpr auto kTemp = std::to_array<double>({300.0, 250.0, 200.0});
    AddDoubleVar(prod.get(), "temperature", "K", kV, kVLen, kTemp);
    // Add float variable to exercise convert_data_type.
    {
      harp_variable* raw = nullptr;
      ASSERT_EQ(HARP_SUCCESS,
                harp_variable_new("pressure_alt", harp_type_float, 1, kV.data(),
                                  kVLen.data(), &raw));
      ScopedHarpVariable v(raw);
      ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(v.get(), "m"));
      v->data.float_data[0] = 0.0f;
      v->data.float_data[1] = 5000.0f;
      v->data.float_data[2] = 15000.0f;
      ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), v.get()));
      v.release();
    }

    constexpr auto kO3 =
        std::to_array<double>({10.0, 20.0, 30.0, 40.0, 50.0, nan});
    AddDoubleVar(prod.get(), "O3_column_number_density", "molec/m2", kTV,
                 kTVLen, kO3);
    AddDoubleVar(prod.get(), "O3_column_number_density_uncertainty", "molec/m2",
                 kTV, kTVLen, kO3);

    // Multi-dim after vertical: {time, vertical, spectral} -> num_elements > 1.
    constexpr auto kTVS = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_vertical,
         harp_dimension_spectral});
    constexpr auto kTVSLen = std::to_array<long>({2, 3, 2});
    std::array<double, 12> tvs_vals{};
    tvs_vals.fill(5.0);
    AddDoubleVar(prod.get(), "radiance", "W/m2", kTVS, kTVSLen, tvs_vals);

    // 2D AVK {vertical, vertical} -> removed.
    constexpr auto kVV = std::to_array<harp_dimension_type>(
        {harp_dimension_vertical, harp_dimension_vertical});
    constexpr auto kVVLen = std::to_array<long>({3, 3});
    std::array<double, 9> vv_vals{};
    vv_vals.fill(1.0);
    AddDoubleVar(prod.get(), "O3_avk", "1", kVV, kVVLen, vv_vals);

    // Target 1D pressure grid (length 4 > 3 so product grows, then with NaN
    // padding on target_grid).
    constexpr auto kTgtLen = std::to_array<long>({4});
    const auto kTgtP = std::to_array<double>({1000.0, 500.0, 200.0, nan});
    ScopedHarpVariable tgt_p =
        MakeDoubleVar("pressure", "hPa", kV, kTgtLen, kTgtP);

    ASSERT_EQ(HARP_SUCCESS, harp_product_regrid_with_axis_variable(
                                prod.get(), tgt_p.get(), nullptr));
    EXPECT_EQ(4, prod->dimension[harp_dimension_vertical]);
    EXPECT_EQ(0, harp_product_has_variable(
                     prod.get(), "O3_column_number_density_uncertainty"));
    EXPECT_EQ(0, harp_product_has_variable(prod.get(), "O3_avk"));
    EXPECT_EQ(1, harp_product_has_variable(prod.get(), "pressure_bounds"));

    harp_variable* temp_out = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                                prod.get(), "temperature", &temp_out));
    EXPECT_NEAR(300.0, temp_out->data.double_data[0], 1e-10);
    EXPECT_NEAR(250.0, temp_out->data.double_data[1], 1e-10);
    EXPECT_EQ(1, harp_isnan(temp_out->data.double_data[3]));
  }

  // 2. Spectral regrid exercises log-log interpolation for AOD and extinction,
  // plus shrinking target dimension (< source).
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);

    constexpr auto kS =
        std::to_array<harp_dimension_type>({harp_dimension_spectral});
    constexpr auto kSLen = std::to_array<long>({3});
    constexpr auto kWvl = std::to_array<double>({400e-9, 500e-9, 800e-9});
    const auto kAod = std::to_array<double>({
        0.5 * std::pow(400.0 / 500.0, -2.0),
        0.5,
        0.5 * std::pow(800.0 / 500.0, -2.0),
    });
    AddDoubleVar(prod.get(), "wavelength", "m", kS, kSLen, kWvl);
    AddDoubleVar(prod.get(), "aerosol_optical_depth", "1", kS, kSLen, kAod);
    AddDoubleVar(prod.get(), "aerosol_extinction_coefficient", "1/m", kS, kSLen,
                 kAod);

    constexpr auto kTgtLen = std::to_array<long>({2});
    constexpr auto kTgtWvl = std::to_array<double>({550e-9, 600e-9});
    ScopedHarpVariable tgt_wvl =
        MakeDoubleVar("wavelength", "m", kS, kTgtLen, kTgtWvl);

    ASSERT_EQ(HARP_SUCCESS, harp_product_regrid_with_axis_variable(
                                prod.get(), tgt_wvl.get(), nullptr));
    EXPECT_EQ(2, prod->dimension[harp_dimension_spectral]);
    harp_variable* aod_out = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                                prod.get(), "aerosol_optical_depth", &aod_out));
    EXPECT_NEAR(0.5 * std::pow(600.0 / 500.0, -2.0),
                aod_out->data.double_data[1], 1e-12);
  }

  // 3. Time dimension regrid removes datetime_start/stop/length and handles
  // string/unitless variable removal.
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);

    constexpr auto kT =
        std::to_array<harp_dimension_type>({harp_dimension_time});
    constexpr auto kTLen = std::to_array<long>({3});
    constexpr auto kDt = std::to_array<double>({0.0, 10.0, 20.0});
    constexpr auto kVals = std::to_array<double>({100.0, 200.0, 300.0});
    AddDoubleVar(prod.get(), "datetime", "s", kT, kTLen, kDt);
    AddDoubleVar(prod.get(), "datetime_start", "s", kT, kTLen, kDt);
    AddDoubleVar(prod.get(), "datetime_stop", "s", kT, kTLen, kDt);
    AddDoubleVar(prod.get(), "datetime_length", "s", kT, kTLen, kDt);
    AddDoubleVar(prod.get(), "index", nullptr, kT, kTLen, kVals);
    AddDoubleVar(prod.get(), "temperature", "K", kT, kTLen, kVals);

    harp_variable* raw_str = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_new("note", harp_type_string, 1, kT.data(),
                                kTLen.data(), &raw_str));
    ScopedHarpVariable str_var(raw_str);
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_add_variable(prod.get(), str_var.get()));
    str_var.release();

    constexpr auto kTgtLen = std::to_array<long>({2});
    constexpr auto kTgtDt = std::to_array<double>({5.0, 15.0});
    ScopedHarpVariable tgt_dt =
        MakeDoubleVar("datetime", "s", kT, kTgtLen, kTgtDt);

    ASSERT_EQ(HARP_SUCCESS, harp_product_regrid_with_axis_variable(
                                prod.get(), tgt_dt.get(), nullptr));
    EXPECT_EQ(0, harp_product_has_variable(prod.get(), "datetime_start"));
    EXPECT_EQ(0, harp_product_has_variable(prod.get(), "index"));
    EXPECT_EQ(0, harp_product_has_variable(prod.get(), "note"));
    harp_variable* temp = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                                prod.get(), "temperature", &temp));
    EXPECT_NEAR(150.0, temp->data.double_data[0], 1e-12);
    EXPECT_NEAR(250.0, temp->data.double_data[1], 1e-12);
  }
  // 3.5. Existing source_bounds in product, explicit target_bounds, and all
  // _column_* suffix variants (_apriori, _avk, _dfs, plus non-interval
  // _column_other).
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);

    constexpr auto kV =
        std::to_array<harp_dimension_type>({harp_dimension_vertical});
    constexpr auto kVLen = std::to_array<long>({2});
    constexpr auto kVBTypes = std::to_array<harp_dimension_type>(
        {harp_dimension_vertical, harp_dimension_independent});
    constexpr auto kVBLen = std::to_array<long>({2, 2});
    constexpr auto kAlt = std::to_array<double>({500.0, 1500.0});
    constexpr auto kAltB = std::to_array<double>({0.0, 1000.0, 1000.0, 2000.0});
    constexpr auto kCol = std::to_array<double>({10.0, 20.0});

    AddDoubleVar(prod.get(), "altitude", "m", kV, kVLen, kAlt);
    AddDoubleVar(prod.get(), "altitude_bounds", "m", kVBTypes, kVBLen, kAltB);
    AddDoubleVar(prod.get(), "O3_column_number_density_apriori", "molec/m2", kV,
                 kVLen, kCol);
    AddDoubleVar(prod.get(), "O3_column_number_density_avk", "1", kV, kVLen,
                 kCol);
    AddDoubleVar(prod.get(), "O3_column_number_density_dfs", "1", kV, kVLen,
                 kCol);
    AddDoubleVar(prod.get(), "O3_column_other", "1", kV, kVLen, kCol);

    constexpr auto kTgtAlt = std::to_array<double>({1000.0, 1500.0});
    constexpr auto kTgtAltB =
        std::to_array<double>({0.0, 2000.0, 1000.0, 2000.0});
    ScopedHarpVariable tgt_alt =
        MakeDoubleVar("altitude", "m", kV, kVLen, kTgtAlt);
    ScopedHarpVariable tgt_alt_b =
        MakeDoubleVar("altitude_bounds", "m", kVBTypes, kVBLen, kTgtAltB);
    ASSERT_EQ(HARP_SUCCESS, harp_product_regrid_with_axis_variable(
                                prod.get(), tgt_alt.get(), tgt_alt_b.get()));
    EXPECT_EQ(1, harp_product_has_variable(prod.get(), "altitude_bounds"));
  }

  // 3.6. Un-derivable bounds for custom axis with interval variable exercises
  // get_bounds_for_grid_from_variable and source/target bounds error cleanup.
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);

    constexpr auto kV =
        std::to_array<harp_dimension_type>({harp_dimension_vertical});
    constexpr auto kVLen = std::to_array<long>({2});
    constexpr auto kVBTypes = std::to_array<harp_dimension_type>(
        {harp_dimension_vertical, harp_dimension_independent});
    constexpr auto kVBLen = std::to_array<long>({2, 2});
    constexpr auto kFoo = std::to_array<double>({1.0, 2.0});
    constexpr auto kFooB = std::to_array<double>({0.5, 1.5, 1.5, 2.5});
    AddDoubleVar(prod.get(), "foo", "m", kV, kVLen, kFoo);
    AddDoubleVar(prod.get(), "O3_column_number_density", "molec/m2", kV, kVLen,
                 kFoo);

    ScopedHarpVariable tgt_foo = MakeDoubleVar("foo", "m", kV, kVLen, kFoo);
    EXPECT_EQ(-1, harp_product_regrid_with_axis_variable(
                      prod.get(), tgt_foo.get(), nullptr));

    ScopedHarpVariable tgt_foo_b =
        MakeDoubleVar("foo_bounds", "m", kVBTypes, kVBLen, kFooB);
    EXPECT_EQ(-1, harp_product_regrid_with_axis_variable(
                      prod.get(), tgt_foo.get(), tgt_foo_b.get()));
  }
}

TEST(HarpRegridTest, MultiDimSourceAxisAndClampDimension) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  // 1. Product with no variables depending on vertical -> returns 0 as-is.
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);

    constexpr auto kT =
        std::to_array<harp_dimension_type>({harp_dimension_time});
    constexpr auto kTLen = std::to_array<long>({2});
    constexpr auto kDt = std::to_array<double>({0.0, 10.0});
    AddDoubleVar(prod.get(), "datetime", "s", kT, kTLen, kDt);

    constexpr auto kV =
        std::to_array<harp_dimension_type>({harp_dimension_vertical});
    constexpr auto kVLen = std::to_array<long>({2});
    constexpr auto kAlt = std::to_array<double>({100.0, 200.0});
    ScopedHarpVariable tgt_alt =
        MakeDoubleVar("altitude", "m", kV, kVLen, kAlt);
    ASSERT_EQ(HARP_SUCCESS, harp_product_regrid_with_axis_variable(
                                prod.get(), tgt_alt.get(), nullptr));
  }

  // 2. 3D source axis {time, latitude, vertical} triggers
  // regrid_with_multi_dim_source_axis flattening/unflattening.
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);

    constexpr auto kTLV = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_latitude,
         harp_dimension_vertical});
    constexpr auto kTLVLen = std::to_array<long>({2, 2, 3});
    constexpr auto kAlt3D =
        std::to_array<double>({0.0, 100.0, 200.0, 0.0, 100.0, 200.0, 0.0, 100.0,
                               200.0, 0.0, 100.0, 200.0});
    constexpr auto kTemp3D =
        std::to_array<double>({290.0, 280.0, 270.0, 290.0, 280.0, 270.0, 290.0,
                               280.0, 270.0, 290.0, 280.0, 270.0});
    AddDoubleVar(prod.get(), "altitude", "m", kTLV, kTLVLen, kAlt3D);
    AddDoubleVar(prod.get(), "temperature", "K", kTLV, kTLVLen, kTemp3D);

    // Add a unitless variable (removed via resample_remove), a variable with
    // incompatible leading dims {vertical} (removed by same_grid check), and a
    // 4D variable with trailing time dim (removed by trailing-time check).
    constexpr auto kV =
        std::to_array<harp_dimension_type>({harp_dimension_vertical});
    constexpr auto kVLen = std::to_array<long>({3});
    constexpr auto kVVals = std::to_array<double>({1.0, 2.0, 3.0});
    AddDoubleVar(prod.get(), "unitless_3d", nullptr, kTLV, kTLVLen, kAlt3D);
    AddDoubleVar(prod.get(), "bad_grid_var", "1", kV, kVLen, kVVals);

    constexpr auto kTLVL = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_latitude, harp_dimension_vertical,
         harp_dimension_spectral, harp_dimension_longitude});
    constexpr auto kTLVLLen = std::to_array<long>({2, 2, 3, 1, 1});
    AddDoubleVar(prod.get(), "trailing_time_var", "1", kTLVL, kTLVLLen, kAlt3D);
    harp_variable* trailing_var = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_get_variable_by_name(prod.get(), "trailing_time_var",
                                                &trailing_var));
    trailing_var->dimension_type[4] = harp_dimension_time;
    prod->dimension[harp_dimension_longitude] = 0;

    // 5D variable without trailing time finishes the trailing-time loop
    // cleanly.
    constexpr auto kTLVSI = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_latitude, harp_dimension_vertical,
         harp_dimension_spectral, harp_dimension_independent});
    AddDoubleVar(prod.get(), "valid_5d_var", "1", kTLVSI, kTLVLLen, kTemp3D);

    constexpr auto kTgtLen = std::to_array<long>({2});
    constexpr auto kTgtAlt = std::to_array<double>({50.0, 150.0});
    ScopedHarpVariable tgt_alt =
        MakeDoubleVar("altitude", "m", kV, kTgtLen, kTgtAlt);

    ASSERT_EQ(HARP_SUCCESS, harp_product_regrid_with_axis_variable(
                                prod.get(), tgt_alt.get(), nullptr));
    EXPECT_EQ(2, prod->dimension[harp_dimension_vertical]);
    EXPECT_EQ(0, harp_product_has_variable(prod.get(), "bad_grid_var"));

    harp_variable* temp = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                                prod.get(), "temperature", &temp));
    EXPECT_EQ(3, temp->num_dimensions);
    EXPECT_NEAR(285.0, temp->data.double_data[0], 1e-12);
    EXPECT_NEAR(275.0, temp->data.double_data[1], 1e-12);
  }

  // 3. Error when no axis exists at all.
  // Note: regrid_with_multi_dim_source_axis() leaks `source_grid` upstream when
  // `axis_dimension_type[j] == harp_dimension_time` for `j >= 1`
  // (harp-regrid.c:650-655 returns -1 without deleting `source_grid`), which
  // triggers HeapLeakChecker/LSan, so that branch is omitted here.
  {
    // Missing axis variable altogether.
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);
    constexpr auto kV =
        std::to_array<harp_dimension_type>({harp_dimension_vertical});
    constexpr auto kVLen = std::to_array<long>({2});
    constexpr auto kAlt = std::to_array<double>({0.0, 100.0});
    ScopedHarpVariable tgt_alt =
        MakeDoubleVar("altitude", "m", kV, kVLen, kAlt);
    AddDoubleVar(prod.get(), "temperature", "K", kV, kVLen, kAlt);
    EXPECT_EQ(-1, harp_product_regrid_with_axis_variable(
                      prod.get(), tgt_alt.get(), nullptr));
  }
  {
    // 3D source axis with vertical length 1 fails inside
    // regrid_with_multi_dim_source_axis and exercises its error cleanup.
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);
    constexpr auto kTLV = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_latitude,
         harp_dimension_vertical});
    constexpr auto kTLVLen = std::to_array<long>({1, 1, 1});
    constexpr auto kVal = std::to_array<double>({100.0});
    AddDoubleVar(prod.get(), "altitude", "m", kTLV, kTLVLen, kVal);
    AddDoubleVar(prod.get(), "temperature", "K", kTLV, kTLVLen, kVal);
    constexpr auto kV =
        std::to_array<harp_dimension_type>({harp_dimension_vertical});
    constexpr auto kVLen2 = std::to_array<long>({2});
    constexpr auto kTgt = std::to_array<double>({50.0, 150.0});
    ScopedHarpVariable tgt_alt =
        MakeDoubleVar("altitude", "m", kV, kVLen2, kTgt);
    EXPECT_EQ(-1, harp_product_regrid_with_axis_variable(
                      prod.get(), tgt_alt.get(), nullptr));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  }
  {
    // 3D source axis {time(2), lat(2), vertical(3)} with 2D target_grid
    // {time(2), vertical(2)} fails inside virtual_product because flattened
    // time length (4) != 2, exercising virtual_product variable cleanup.
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);
    constexpr auto kTLV = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_latitude,
         harp_dimension_vertical});
    constexpr auto kTLVLen = std::to_array<long>({2, 2, 3});
    constexpr auto kVals =
        std::to_array<double>({0.0, 100.0, 200.0, 0.0, 100.0, 200.0, 0.0, 100.0,
                               200.0, 0.0, 100.0, 200.0});
    AddDoubleVar(prod.get(), "altitude", "m", kTLV, kTLVLen, kVals);
    AddDoubleVar(prod.get(), "temperature", "K", kTLV, kTLVLen, kVals);

    constexpr auto kTV = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_vertical});
    constexpr auto kTVLen = std::to_array<long>({2, 2});
    constexpr auto kTgt = std::to_array<double>({50.0, 150.0, 50.0, 150.0});
    ScopedHarpVariable tgt_alt =
        MakeDoubleVar("altitude", "m", kTV, kTVLen, kTgt);
    EXPECT_EQ(-1, harp_product_regrid_with_axis_variable(
                      prod.get(), tgt_alt.get(), nullptr));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  }

  // 4. harp_product_clamp_dimension: ascending altitude (swapped bounds),
  // descending pressure (2D with empty row + NaN), and clamping to empty.
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);

    constexpr auto kV =
        std::to_array<harp_dimension_type>({harp_dimension_vertical});
    constexpr auto kVLen = std::to_array<long>({4});
    constexpr auto kAlt =
        std::to_array<double>({500.0, 1500.0, 2500.0, 3500.0});
    constexpr auto kTemp = std::to_array<double>({290.0, 280.0, 270.0, 260.0});
    AddDoubleVar(prod.get(), "altitude", "m", kV, kVLen, kAlt);
    AddDoubleVar(prod.get(), "temperature", "K", kV, kVLen, kTemp);

    // Pass lower_bound > upper_bound so swap is exercised; clamp [1200, 2800].
    // Midpoint bounds are [500,1000], [1000,2000], [2000,3000], [3000,3500];
    // clamping [1200,2800] drops the first and last bins, leaving 2 bins.
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_clamp_dimension(prod.get(), harp_dimension_vertical,
                                           "altitude", "m", 2800.0, 1200.0));
    EXPECT_EQ(2, prod->dimension[harp_dimension_vertical]);
    harp_variable* bounds = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                                prod.get(), "altitude_bounds", &bounds));
    EXPECT_DOUBLE_EQ(1200.0, bounds->data.double_data[0]);
    EXPECT_DOUBLE_EQ(2800.0, bounds->data.double_data[3]);

    // Clamping a 1-element vertical dimension fails during regridding.
    harp_product* raw_one = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_one));
    ScopedHarpProduct prod_one(raw_one);
    constexpr auto kVLen1 = std::to_array<long>({1});
    AddDoubleVar(prod_one.get(), "altitude", "m", kV, kVLen1,
                 std::to_array<double>({500.0}));
    AddDoubleVar(prod_one.get(), "temperature", "K", kV, kVLen1,
                 std::to_array<double>({280.0}));
    EXPECT_EQ(-1, harp_product_clamp_dimension(prod_one.get(),
                                               harp_dimension_vertical,
                                               "altitude", "m", 0.0, 1000.0));
  }

  // 5. Descending 2D pressure clamp with infinite bounds, NaN row, and rows
  // clamped empty from below/above.
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);

    constexpr auto kT =
        std::to_array<harp_dimension_type>({harp_dimension_time});
    constexpr auto kTLen = std::to_array<long>({4});
    constexpr auto kDtStart = std::to_array<double>({0.0, 10.0, 20.0, 30.0});
    constexpr auto kDtStop = std::to_array<double>({5.0, 15.0, 25.0, 35.0});
    AddDoubleVar(prod.get(), "datetime_start", HARP_UNIT_DATETIME, kT, kTLen,
                 kDtStart);
    AddDoubleVar(prod.get(), "datetime_stop", HARP_UNIT_DATETIME, kT, kTLen,
                 kDtStop);

    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    constexpr auto kTV = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_vertical});
    constexpr auto kTVLen = std::to_array<long>({4, 4});
    const auto kPress = std::to_array<double>({
        1000.0,
        500.0,
        200.0,
        50.0,
        nan,
        nan,
        nan,
        nan,
        50.0,
        20.0,
        nan,
        nan,
        2000.0,
        1500.0,
        nan,
        nan,
    });
    const auto kTemp = std::to_array<double>({
        300.0,
        260.0,
        220.0,
        200.0,
        nan,
        nan,
        nan,
        nan,
        210.0,
        200.0,
        nan,
        nan,
        310.0,
        305.0,
        nan,
        nan,
    });
    AddDoubleVar(prod.get(), "pressure", "hPa", kTV, kTVLen, kPress);
    AddDoubleVar(prod.get(), "temperature", "K", kTV, kTVLen, kTemp);

    ASSERT_EQ(HARP_SUCCESS,
              harp_product_clamp_dimension(prod.get(), harp_dimension_vertical,
                                           "pressure", "hPa", 150.0, 800.0));
    EXPECT_EQ(3, prod->dimension[harp_dimension_vertical]);

    // Clamping time dimension succeeds with infinite bounds when
    // datetime_start/datetime_stop exist, and fails once removed.
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_clamp_dimension(
                  prod.get(), harp_dimension_time, "datetime",
                  HARP_UNIT_DATETIME, -std::numeric_limits<double>::infinity(),
                  std::numeric_limits<double>::infinity()));
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_remove_variable_by_name(prod.get(), "datetime"));
    EXPECT_EQ(-1, harp_product_clamp_dimension(prod.get(), harp_dimension_time,
                                               "datetime", HARP_UNIT_DATETIME,
                                               0.0, 10.0));
    EXPECT_EQ(-1,
              harp_product_clamp_dimension(prod.get(), harp_dimension_vertical,
                                           "altitude", "m", 0.0, 10.0));
    // Re-add datetime without datetime_start/datetime_stop to trigger
    // get_derived_bounds_for_grid failure inside clamp_dimension.
    AddDoubleVar(prod.get(), "datetime", HARP_UNIT_DATETIME, kT, kTLen,
                 kDtStart);
    EXPECT_EQ(-1, harp_product_clamp_dimension(prod.get(), harp_dimension_time,
                                               "datetime", HARP_UNIT_DATETIME,
                                               0.0, 10.0));
  }
}

TEST(HarpRegridTest, RegridWithCollocatedProductAndDataset) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  // 1. Source product A with time=2, vertical=3, collocation_index={10, 20}.
  harp_product* raw_a = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_a));
  ScopedHarpProduct prod_a(raw_a);
  constexpr auto kT = std::to_array<harp_dimension_type>({harp_dimension_time});
  constexpr auto kTLen2 = std::to_array<long>({2});
  constexpr auto kIdxA = std::to_array<int32_t>({10, 20});
  AddInt32Var(prod_a.get(), "collocation_index", nullptr, kT, kTLen2, kIdxA);
  AddInt32Var(prod_a.get(), "index", nullptr, kT, kTLen2,
              std::to_array<int32_t>({0, 1}));

  constexpr auto kV =
      std::to_array<harp_dimension_type>({harp_dimension_vertical});
  constexpr auto kVLen3 = std::to_array<long>({3});
  constexpr auto kAltA = std::to_array<double>({0.0, 1000.0, 2000.0});
  AddDoubleVar(prod_a.get(), "altitude", "m", kV, kVLen3, kAltA);

  constexpr auto kTV = std::to_array<harp_dimension_type>(
      {harp_dimension_time, harp_dimension_vertical});
  constexpr auto kTVLenA = std::to_array<long>({2, 3});
  constexpr auto kTempA =
      std::to_array<double>({300.0, 290.0, 280.0, 310.0, 300.0, 290.0});
  AddDoubleVar(prod_a.get(), "temperature", "K", kTV, kTVLenA, kTempA);

  // Error checks for harp_product_regrid_with_collocated_product:
  // time dimension fails, missing vertical dimension fails.
  {
    harp_product* raw_empty = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_empty));
    ScopedHarpProduct empty_prod(raw_empty);
    EXPECT_EQ(-1, harp_product_regrid_with_collocated_product(
                      empty_prod.get(), harp_dimension_time, "datetime", "s",
                      prod_a.get()));
    EXPECT_EQ(-1, harp_product_regrid_with_collocated_product(
                      empty_prod.get(), harp_dimension_vertical, "altitude",
                      "m", prod_a.get()));

    EXPECT_EQ(-1, harp_product_regrid_with_collocated_product(
                      prod_a.get(), harp_dimension_vertical, "altitude", "m",
                      empty_prod.get()));
  }

  // 2. Collocated product B with dimension[vertical] == 0 (1D altitude {time})
  // and reversed collocation_index={20, 10, 30}.
  {
    harp_product* raw_b = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_b));
    ScopedHarpProduct prod_b(raw_b);
    constexpr auto kTLen3 = std::to_array<long>({3});
    AddInt32Var(prod_b.get(), "collocation_index", nullptr, kT, kTLen3,
                std::to_array<int32_t>({20, 10, 30}));
    AddDoubleVar(prod_b.get(), "altitude", "m", kT, kTLen3,
                 std::to_array<double>({1500.0, 500.0, 999.0}));

    harp_product* raw_a_copy = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_copy(prod_a.get(), &raw_a_copy));
    ScopedHarpProduct prod_a_copy(raw_a_copy);

    ASSERT_EQ(HARP_SUCCESS, harp_product_regrid_with_collocated_product(
                                prod_a_copy.get(), harp_dimension_vertical,
                                "altitude", "m", prod_b.get()));
    EXPECT_EQ(1, prod_a_copy->dimension[harp_dimension_vertical]);
    harp_variable* temp = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                                prod_a_copy.get(), "temperature", &temp));
    // collocation_index 10 -> altitude 500 -> 295 K; 20 -> altitude 1500 ->
    // 295 K.
    EXPECT_NEAR(295.0, temp->data.double_data[0], 1e-12);
    EXPECT_NEAR(295.0, temp->data.double_data[1], 1e-12);
  }

  // 2.5. Collocated product B with 2D altitude {time, vertical} +
  // altitude_bounds, plus error paths for collocated_product.
  {
    harp_product* raw_b2d = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_b2d));
    ScopedHarpProduct prod_b2d(raw_b2d);
    constexpr auto kTLen3 = std::to_array<long>({3});
    AddInt32Var(prod_b2d.get(), "collocation_index", nullptr, kT, kTLen3,
                std::to_array<int32_t>({20, 10, 30}));
    constexpr auto kTVLen3 = std::to_array<long>({3, 2});
    AddDoubleVar(
        prod_b2d.get(), "altitude", "m", kTV, kTVLen3,
        std::to_array<double>({500.0, 1500.0, 250.0, 1250.0, 100.0, 200.0}));
    constexpr auto kTVBTypes = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_vertical,
         harp_dimension_independent});
    constexpr auto kTVBLen3 = std::to_array<long>({3, 2, 2});
    AddDoubleVar(
        prod_b2d.get(), "altitude_bounds", "m", kTVBTypes, kTVBLen3,
        std::to_array<double>({0.0, 1000.0, 1000.0, 2000.0, 0.0, 500.0, 500.0,
                               2000.0, 0.0, 150.0, 150.0, 300.0}));

    harp_product* raw_a_copy = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_copy(prod_a.get(), &raw_a_copy));
    ScopedHarpProduct prod_a_copy(raw_a_copy);
    ASSERT_EQ(HARP_SUCCESS, harp_product_regrid_with_collocated_product(
                                prod_a_copy.get(), harp_dimension_vertical,
                                "altitude", "m", prod_b2d.get()));
    EXPECT_EQ(2, prod_a_copy->dimension[harp_dimension_vertical]);
    EXPECT_EQ(1,
              harp_product_has_variable(prod_a_copy.get(), "altitude_bounds"));

    // Error: collocated_product missing axis when dim == 0 and when dim > 0.
    harp_product* raw_no_axis = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_no_axis));
    ScopedHarpProduct no_axis(raw_no_axis);
    AddInt32Var(no_axis.get(), "collocation_index", nullptr, kT, kTLen2, kIdxA);
    EXPECT_EQ(-1, harp_product_regrid_with_collocated_product(
                      prod_a.get(), harp_dimension_vertical, "altitude", "m",
                      no_axis.get()));
    AddDoubleVar(no_axis.get(), "other", "1", kV, kVLen3, kAltA);
    EXPECT_EQ(-1, harp_product_regrid_with_collocated_product(
                      prod_a.get(), harp_dimension_vertical, "altitude", "m",
                      no_axis.get()));

    // Error: collocation_index mismatch during filter_by_index.
    harp_product* raw_mismatch = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_mismatch));
    ScopedHarpProduct mismatch(raw_mismatch);
    AddInt32Var(mismatch.get(), "collocation_index", nullptr, kT, kTLen2,
                std::to_array<int32_t>({99, 100}));
    AddDoubleVar(mismatch.get(), "altitude", "m", kT, kTLen2,
                 std::to_array<double>({500.0, 1500.0}));
    EXPECT_EQ(-1, harp_product_regrid_with_collocated_product(
                      prod_a.get(), harp_dimension_vertical, "altitude", "m",
                      mismatch.get()));

    // Error: regrid_with_axis_variable failure (source product has 1 level).
    harp_product* raw_a1 = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_a1));
    ScopedHarpProduct prod_a1(raw_a1);
    AddInt32Var(prod_a1.get(), "collocation_index", nullptr, kT, kTLen2, kIdxA);
    constexpr auto kVLen1 = std::to_array<long>({1});
    AddDoubleVar(prod_a1.get(), "altitude", "m", kV, kVLen1,
                 std::to_array<double>({500.0}));
    constexpr auto kTVLen1 = std::to_array<long>({2, 1});
    AddDoubleVar(prod_a1.get(), "temperature", "K", kTV, kTVLen1,
                 std::to_array<double>({300.0, 310.0}));
    EXPECT_EQ(-1, harp_product_regrid_with_collocated_product(
                      prod_a1.get(), harp_dimension_vertical, "altitude", "m",
                      prod_b2d.get()));
  }

  // 3. Collocated product B with 2D altitude {time, vertical} + altitude_bounds
  // exported as netCDF files inside TempDir for
  // harp_product_regrid_with_collocated_dataset.
  TempDir dir("harp_regrid_colloc");
  const auto path_b1 = dir.child("b1.nc");
  const auto path_b2 = dir.child("b2.nc");

  {
    harp_product* raw_b1 = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_b1));
    ScopedHarpProduct b1(raw_b1);
    ASSERT_EQ(HARP_SUCCESS, harp_product_set_source_product(b1.get(), "b1"));
    constexpr auto kTLen1 = std::to_array<long>({1});
    AddInt32Var(b1.get(), "index", nullptr, kT, kTLen1,
                std::to_array<int32_t>({0}));
    constexpr auto kTVLenB = std::to_array<long>({1, 2});
    AddDoubleVar(b1.get(), "altitude", "m", kTV, kTVLenB,
                 std::to_array<double>({500.0, 1500.0}));
    constexpr auto kTVBLen = std::to_array<long>({1, 2, 2});
    constexpr auto kTVBTypes = std::to_array<harp_dimension_type>(
        {harp_dimension_time, harp_dimension_vertical,
         harp_dimension_independent});
    AddDoubleVar(b1.get(), "altitude_bounds", "m", kTVBTypes, kTVBLen,
                 std::to_array<double>({0.0, 1000.0, 1000.0, 2000.0}));
    ASSERT_EQ(HARP_SUCCESS, harp_export(path_b1.c_str(), "netcdf", b1.get()));
  }
  {
    harp_product* raw_b2 = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_b2));
    ScopedHarpProduct b2(raw_b2);
    ASSERT_EQ(HARP_SUCCESS, harp_product_set_source_product(b2.get(), "b2"));
    constexpr auto kTLen1 = std::to_array<long>({1});
    AddInt32Var(b2.get(), "index", nullptr, kT, kTLen1,
                std::to_array<int32_t>({0}));
    AddDoubleVar(b2.get(), "altitude", "m", kT, kTLen1,
                 std::to_array<double>({1000.0}));
    ASSERT_EQ(HARP_SUCCESS, harp_export(path_b2.c_str(), "netcdf", b2.get()));
  }

  harp_collocation_result* raw_cr = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_new(&raw_cr, 0, nullptr, nullptr));
  ScopedHarpCollocationResult cr(raw_cr);
  ASSERT_EQ(HARP_SUCCESS,
            harp_dataset_import(cr->dataset_b, dir.c_str(), nullptr));
  ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_add_pair(
                              cr.get(), 10, "a1", 0, "b1", 0, 0, nullptr));
  ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_add_pair(
                              cr.get(), 20, "a1", 1, "b2", 0, 0, nullptr));

  // First test error when collocation_index is not found in collocation result.
  {
    harp_product* raw_a_bad = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_a_bad));
    ScopedHarpProduct a_bad(raw_a_bad);
    constexpr auto kTLen1 = std::to_array<long>({1});
    AddInt32Var(a_bad.get(), "collocation_index", nullptr, kT, kTLen1,
                std::to_array<int32_t>({999}));
    AddDoubleVar(a_bad.get(), "altitude", "m", kV, kVLen3, kAltA);
    EXPECT_EQ(-1, harp_product_regrid_with_collocated_dataset(
                      a_bad.get(), harp_dimension_vertical, "altitude", "m",
                      cr.get()));
  }

  // Test error when product has no vertical dimension, when time dimension
  // is passed with missing collocation_index, and when dataset_b has unused
  // products or error conditions.
  {
    harp_product* raw_empty = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_empty));
    ScopedHarpProduct empty_prod(raw_empty);
    EXPECT_EQ(-1, harp_product_regrid_with_collocated_dataset(
                      empty_prod.get(), harp_dimension_vertical, "altitude",
                      "m", cr.get()));

    AddInt32Var(empty_prod.get(), "index", nullptr, kT,
                std::to_array<long>({1}), std::to_array<int32_t>({0}));
    EXPECT_EQ(-1, harp_product_regrid_with_collocated_dataset(
                      empty_prod.get(), harp_dimension_time, "datetime",
                      HARP_UNIT_DATETIME, cr.get()));
  }

  // Error paths for collocated dataset:
  // (a) Unused product in dataset_b (collocated_product == NULL) is skipped
  //     cleanly; missing metadata fails.
  {
    harp_collocation_result* raw_cr2 = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_collocation_result_new(&raw_cr2, 0, nullptr, nullptr));
    ScopedHarpCollocationResult cr2(raw_cr2);
    ASSERT_EQ(HARP_SUCCESS,
              harp_dataset_add_product(cr2->dataset_b, "b_unused", nullptr));
    ASSERT_EQ(HARP_SUCCESS,
              harp_dataset_add_product(cr2->dataset_b, "b_nometa", nullptr));
    ASSERT_EQ(HARP_SUCCESS,
              harp_collocation_result_add_pair(cr2.get(), 10, "a1", 0,
                                               "b_nometa", 0, 0, nullptr));
    harp_product* raw_a1 = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_a1));
    ScopedHarpProduct a1(raw_a1);
    constexpr auto kTLen1 = std::to_array<long>({1});
    AddInt32Var(a1.get(), "collocation_index", nullptr, kT, kTLen1,
                std::to_array<int32_t>({10}));
    AddDoubleVar(a1.get(), "altitude", "m", kV, kVLen3, kAltA);
    EXPECT_EQ(
        -1, harp_product_regrid_with_collocated_dataset(
                a1.get(), harp_dimension_vertical, "altitude", "m", cr2.get()));
  }

  // (b) Collocated product in dataset_b missing axis variable (dim == 0 and
  // dim > 0). Note: an empty `collocated_product` (no matching sample index)
  // leaks upstream at harp-regrid.c:1581-1584 (`continue` without delete), so
  // that branch is omitted here.
  {
    TempDir err_dir("harp_regrid_colloc_err");
    const auto path_no_alt_0 = err_dir.child("b_noalt0.nc");
    const auto path_no_alt_1 = err_dir.child("b_noalt1.nc");
    {
      harp_product* raw_p = nullptr;
      ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_p));
      ScopedHarpProduct p(raw_p);
      ASSERT_EQ(HARP_SUCCESS,
                harp_product_set_source_product(p.get(), "b_noalt0"));
      AddInt32Var(p.get(), "index", nullptr, kT, std::to_array<long>({1}),
                  std::to_array<int32_t>({0}));
      ASSERT_EQ(HARP_SUCCESS,
                harp_export(path_no_alt_0.c_str(), "netcdf", p.get()));
    }
    {
      harp_product* raw_p = nullptr;
      ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_p));
      ScopedHarpProduct p(raw_p);
      ASSERT_EQ(HARP_SUCCESS,
                harp_product_set_source_product(p.get(), "b_noalt1"));
      AddInt32Var(p.get(), "index", nullptr, kT, std::to_array<long>({1}),
                  std::to_array<int32_t>({0}));
      AddDoubleVar(p.get(), "other", "1", kV, std::to_array<long>({1}),
                   std::to_array<double>({1.0}));
      ASSERT_EQ(HARP_SUCCESS,
                harp_export(path_no_alt_1.c_str(), "netcdf", p.get()));
    }

    auto run_err_case = [&](const char* b_name) {
      harp_collocation_result* raw_cre = nullptr;
      ASSERT_EQ(HARP_SUCCESS,
                harp_collocation_result_new(&raw_cre, 0, nullptr, nullptr));
      ScopedHarpCollocationResult cre(raw_cre);
      ASSERT_EQ(HARP_SUCCESS,
                harp_dataset_import(cre->dataset_b, err_dir.c_str(), nullptr));
      ASSERT_EQ(HARP_SUCCESS,
                harp_collocation_result_add_pair(cre.get(), 10, "a1", 0, b_name,
                                                 0, 0, nullptr));
      harp_product* raw_a1 = nullptr;
      ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_a1));
      ScopedHarpProduct a1(raw_a1);
      AddInt32Var(a1.get(), "collocation_index", nullptr, kT,
                  std::to_array<long>({1}), std::to_array<int32_t>({10}));
      AddDoubleVar(a1.get(), "altitude", "m", kV, kVLen3, kAltA);
      EXPECT_EQ(-1, harp_product_regrid_with_collocated_dataset(
                        a1.get(), harp_dimension_vertical, "altitude", "m",
                        cre.get()));
    };
    run_err_case("b_noalt0");
    run_err_case("b_noalt1");
  }

  // (c) Error when regrid_with_axis_variable fails on source product with
  // 1 vertical level.
  {
    harp_product* raw_a1 = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_a1));
    ScopedHarpProduct a1(raw_a1);
    AddInt32Var(a1.get(), "collocation_index", nullptr, kT, kTLen2, kIdxA);
    AddDoubleVar(a1.get(), "altitude", "m", kV, std::to_array<long>({1}),
                 std::to_array<double>({500.0}));
    AddDoubleVar(a1.get(), "temperature", "K", kTV, std::to_array<long>({2, 1}),
                 std::to_array<double>({300.0, 310.0}));
    EXPECT_EQ(
        -1, harp_product_regrid_with_collocated_dataset(
                a1.get(), harp_dimension_vertical, "altitude", "m", cr.get()));
  }

  // (d) Error when harp_product_append fails across dataset_b products
  //     (incompatible altitude_bounds dims), when filter_by_index fails on
  //     merged_product (partial match missing sample 5), and when
  //     merged_product is NULL (0 collocation indices).
  {
    TempDir err_dir("harp_regrid_colloc_err2");
    const auto path_b_3d = err_dir.child("b_3d.nc");
    const auto path_b_2d = err_dir.child("b_2d.nc");
    const auto path_b_part = err_dir.child("b_part.nc");
    {
      harp_product* raw_p = nullptr;
      ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_p));
      ScopedHarpProduct p(raw_p);
      ASSERT_EQ(HARP_SUCCESS, harp_product_set_source_product(p.get(), "b_3d"));
      AddInt32Var(p.get(), "index", nullptr, kT, std::to_array<long>({1}),
                  std::to_array<int32_t>({0}));
      AddDoubleVar(p.get(), "foo", "m", kTV, std::to_array<long>({1, 2}),
                   std::to_array<double>({500.0, 1500.0}));
      constexpr auto kTVBTypes = std::to_array<harp_dimension_type>(
          {harp_dimension_time, harp_dimension_vertical,
           harp_dimension_independent});
      AddDoubleVar(p.get(), "foo_bounds", "m", kTVBTypes,
                   std::to_array<long>({1, 2, 2}),
                   std::to_array<double>({0.0, 1000.0, 1000.0, 2000.0}));
      ASSERT_EQ(HARP_SUCCESS,
                harp_export(path_b_3d.c_str(), "netcdf", p.get()));
    }
    {
      harp_product* raw_p = nullptr;
      ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_p));
      ScopedHarpProduct p(raw_p);
      ASSERT_EQ(HARP_SUCCESS, harp_product_set_source_product(p.get(), "b_2d"));
      AddInt32Var(p.get(), "index", nullptr, kT, std::to_array<long>({1}),
                  std::to_array<int32_t>({0}));
      AddDoubleVar(p.get(), "foo", "m", kTV, std::to_array<long>({1, 2}),
                   std::to_array<double>({600.0, 1600.0}));
      ASSERT_EQ(HARP_SUCCESS,
                harp_export(path_b_2d.c_str(), "netcdf", p.get()));
    }
    {
      harp_product* raw_p = nullptr;
      ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_p));
      ScopedHarpProduct p(raw_p);
      ASSERT_EQ(HARP_SUCCESS,
                harp_product_set_source_product(p.get(), "b_part"));
      AddInt32Var(p.get(), "index", nullptr, kT, std::to_array<long>({1}),
                  std::to_array<int32_t>({0}));
      AddDoubleVar(p.get(), "altitude", "m", kT, std::to_array<long>({1}),
                   std::to_array<double>({1000.0}));
      ASSERT_EQ(HARP_SUCCESS,
                harp_export(path_b_part.c_str(), "netcdf", p.get()));
    }

    // 1) Incompatible altitude_bounds dimensions -> harp_product_append fails.
    {
      harp_collocation_result* raw_cra = nullptr;
      ASSERT_EQ(HARP_SUCCESS,
                harp_collocation_result_new(&raw_cra, 0, nullptr, nullptr));
      ScopedHarpCollocationResult cra(raw_cra);
      ASSERT_EQ(HARP_SUCCESS,
                harp_dataset_import(cra->dataset_b, err_dir.c_str(), nullptr));
      ASSERT_EQ(HARP_SUCCESS,
                harp_collocation_result_add_pair(cra.get(), 10, "a1", 0, "b_3d",
                                                 0, 0, nullptr));
      ASSERT_EQ(HARP_SUCCESS,
                harp_collocation_result_add_pair(cra.get(), 20, "a1", 1, "b_2d",
                                                 0, 0, nullptr));
      harp_product* raw_a2 = nullptr;
      ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_a2));
      ScopedHarpProduct a2(raw_a2);
      AddInt32Var(a2.get(), "collocation_index", nullptr, kT, kTLen2, kIdxA);
      AddDoubleVar(a2.get(), "foo", "m", kV, kVLen3, kAltA);
      EXPECT_EQ(-1,
                harp_product_regrid_with_collocated_dataset(
                    a2.get(), harp_dimension_vertical, "foo", "m", cra.get()));
    }

    // 2) Partial match on b_part (sample 0 exists, sample 5 missing) ->
    //    filter_by_index on merged_product fails.
    {
      harp_collocation_result* raw_crf = nullptr;
      ASSERT_EQ(HARP_SUCCESS,
                harp_collocation_result_new(&raw_crf, 0, nullptr, nullptr));
      ScopedHarpCollocationResult crf(raw_crf);
      ASSERT_EQ(HARP_SUCCESS,
                harp_dataset_import(crf->dataset_b, err_dir.c_str(), nullptr));
      ASSERT_EQ(HARP_SUCCESS,
                harp_collocation_result_add_pair(crf.get(), 10, "a1", 0,
                                                 "b_part", 0, 0, nullptr));
      ASSERT_EQ(HARP_SUCCESS,
                harp_collocation_result_add_pair(crf.get(), 20, "a1", 1,
                                                 "b_part", 5, 0, nullptr));
      harp_product* raw_a2 = nullptr;
      ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_a2));
      ScopedHarpProduct a2(raw_a2);
      AddInt32Var(a2.get(), "collocation_index", nullptr, kT, kTLen2, kIdxA);
      AddDoubleVar(a2.get(), "altitude", "m", kV, kVLen3, kAltA);
      EXPECT_EQ(-1, harp_product_regrid_with_collocated_dataset(
                        a2.get(), harp_dimension_vertical, "altitude", "m",
                        crf.get()));
    }

    // 3) Zero-length collocation_index -> merged_product == NULL.
    {
      harp_product* raw_a0 = nullptr;
      ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_a0));
      ScopedHarpProduct a0(raw_a0);
      harp_variable* raw_ci = nullptr;
      const long zero = 0;
      ASSERT_EQ(HARP_SUCCESS,
                harp_variable_new("collocation_index", harp_type_int32, 1,
                                  kT.data(), &zero, &raw_ci));
      ScopedHarpVariable ci(raw_ci);
      ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(a0.get(), ci.get()));
      ci.release();
      AddDoubleVar(a0.get(), "altitude", "m", kV, kVLen3, kAltA);
      EXPECT_EQ(-1, harp_product_regrid_with_collocated_dataset(
                        a0.get(), harp_dimension_vertical, "altitude", "m",
                        cr.get()));
      EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
    }
  }

  // Valid collocated dataset regrid across b1 (2D altitude) + b2 (1D altitude).
  ASSERT_EQ(HARP_SUCCESS, harp_product_regrid_with_collocated_dataset(
                              prod_a.get(), harp_dimension_vertical, "altitude",
                              "m", cr.get()));
  EXPECT_EQ(2, prod_a->dimension[harp_dimension_vertical]);
  harp_variable* temp_out = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                              prod_a.get(), "temperature", &temp_out));
  // t=0 (b1): alt {500, 1500} -> {295, 285}.
  EXPECT_NEAR(295.0, temp_out->data.double_data[0], 1e-12);
  EXPECT_NEAR(285.0, temp_out->data.double_data[1], 1e-12);
  // t=1 (b2): alt {1000, NaN} -> {300, NaN}.
  EXPECT_NEAR(300.0, temp_out->data.double_data[2], 1e-12);
  EXPECT_EQ(1, harp_isnan(temp_out->data.double_data[3]));
}

}  // namespace
}  // namespace stcorp_harp
