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
// Unit tests for spatial binning (libharp/harp-bin-spatial.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-bin-spatial.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp.h.in

#include <cmath>
#include <cstdint>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/oom_fault_inject.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::DoubleNear;

ScopedHarpVariable MakeVar(const char* name, harp_data_type data_type,
                           std::initializer_list<harp_dimension_type> dim_types,
                           std::initializer_list<long> dims, const char* unit) {
  std::vector<harp_dimension_type> dt(dim_types);
  std::vector<long> d(dims);
  harp_variable* raw = nullptr;
  if (harp_variable_new(name, data_type, static_cast<int>(dt.size()),
                        dt.empty() ? nullptr : dt.data(),
                        d.empty() ? nullptr : d.data(), &raw) != HARP_SUCCESS) {
    return nullptr;
  }
  ScopedHarpVariable var(raw);
  if (unit != nullptr) {
    if (harp_variable_set_unit(var.get(), unit) != HARP_SUCCESS) {
      return nullptr;
    }
  }
  return var;
}

void AddDoubleVar(harp_product* product, const char* name,
                  std::initializer_list<harp_dimension_type> dim_types,
                  std::initializer_list<long> dims, const char* unit,
                  std::initializer_list<double> values) {
  ScopedHarpVariable var =
      MakeVar(name, harp_type_double, dim_types, dims, unit);
  ASSERT_NE(nullptr, var);
  ASSERT_EQ(static_cast<long>(values.size()), var->num_elements);
  long idx = 0;
  for (double v : values) {
    var->data.double_data[idx++] = v;
  }
  ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(product, var.get()));
  var.release();
}

TEST(HarpBinSpatialTest, ValidationErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  double lat_edges[] = {-90.0, 0.0, 90.0};
  double lon_edges[] = {-180.0, 0.0, 180.0};
  long time_bins[] = {0, 0};

  // 1. Product already has a latitude dimension.
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);
    AddDoubleVar(prod.get(), "latitude", {harp_dimension_latitude}, {2},
                 "degree_north", {-45.0, 45.0});
    EXPECT_EQ(-1, harp_product_bin_spatial(prod.get(), 1, 0, nullptr, 3,
                                           lat_edges, 3, lon_edges));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 2. Product already has a longitude dimension.
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);
    AddDoubleVar(prod.get(), "longitude", {harp_dimension_longitude}, {2},
                 "degree_east", {-90.0, 90.0});
    EXPECT_EQ(-1, harp_product_bin_spatial(prod.get(), 1, 0, nullptr, 3,
                                           lat_edges, 3, lon_edges));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // Build a valid minimal time-indexed product for subsequent error checks.
  auto make_base_product = []() {
    harp_product* raw_prod = nullptr;
    EXPECT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);
    if (prod) {
      AddDoubleVar(prod.get(), "latitude", {harp_dimension_time}, {2},
                   "degree_north", {10.0, 20.0});
      AddDoubleVar(prod.get(), "longitude", {harp_dimension_time}, {2},
                   "degree_east", {10.0, 20.0});
    }
    return prod;
  };

  // 3. num_time_elements mismatch.
  {
    ScopedHarpProduct prod = make_base_product();
    ASSERT_NE(nullptr, prod);
    EXPECT_EQ(-1, harp_product_bin_spatial(prod.get(), 1, 3, time_bins, 3,
                                           lat_edges, 3, lon_edges));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 4. Out-of-range time_bin_index (< 0 and >= num_time_bins).
  {
    ScopedHarpProduct prod = make_base_product();
    long bad_neg[] = {-1, 0};
    EXPECT_EQ(-1, harp_product_bin_spatial(prod.get(), 1, 2, bad_neg, 3,
                                           lat_edges, 3, lon_edges));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

    long bad_hi[] = {0, 1};
    EXPECT_EQ(-1, harp_product_bin_spatial(prod.get(), 1, 2, bad_hi, 3,
                                           lat_edges, 3, lon_edges));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 5. Too few latitude / longitude edges (< 2).
  {
    ScopedHarpProduct prod = make_base_product();
    EXPECT_EQ(-1, harp_product_bin_spatial(prod.get(), 1, 2, time_bins, 1,
                                           lat_edges, 3, lon_edges));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

    EXPECT_EQ(-1, harp_product_bin_spatial(prod.get(), 1, 2, time_bins, 3,
                                           lat_edges, 1, lon_edges));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 6. Latitude edges out of [-90, 90] and non-ascending edges.
  {
    ScopedHarpProduct prod = make_base_product();
    double bad_lat_low[] = {-91.0, 0.0, 90.0};
    EXPECT_EQ(-1, harp_product_bin_spatial(prod.get(), 1, 2, time_bins, 3,
                                           bad_lat_low, 3, lon_edges));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

    double bad_lat_high[] = {-90.0, 0.0, 91.0};
    EXPECT_EQ(-1, harp_product_bin_spatial(prod.get(), 1, 2, time_bins, 3,
                                           bad_lat_high, 3, lon_edges));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

    double bad_lat_order[] = {0.0, 0.0, 90.0};
    EXPECT_EQ(-1, harp_product_bin_spatial(prod.get(), 1, 2, time_bins, 3,
                                           bad_lat_order, 3, lon_edges));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

    double bad_lon_order[] = {-180.0, -180.0, 180.0};
    EXPECT_EQ(-1, harp_product_bin_spatial(prod.get(), 1, 2, time_bins, 3,
                                           lat_edges, 3, bad_lon_order));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

    double bad_lon_span[] = {-180.0, 0.0, 200.0};
    EXPECT_EQ(-1, harp_product_bin_spatial(prod.get(), 1, 2, time_bins, 3,
                                           lat_edges, 3, bad_lon_span));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 7. Missing latitude variable (note: missing longitude after derived
  //    latitude leaks `latitude` in upstream harp-bin-spatial.c:1331).
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);
    AddDoubleVar(prod.get(), "O3_column_number_density", {harp_dimension_time},
                 {2}, "molec/m2", {1.0, 2.0});
    EXPECT_EQ(-1, harp_product_bin_spatial(prod.get(), 1, 2, time_bins, 3,
                                           lat_edges, 3, lon_edges));
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 8. Mismatched independent dimension length between latitude_bounds and
  // longitude_bounds.
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);
    AddDoubleVar(prod.get(), "latitude_bounds",
                 {harp_dimension_time, harp_dimension_independent}, {2, 3},
                 "degree_north", {0.0, 1.0, 2.0, 3.0, 4.0, 5.0});
    AddDoubleVar(prod.get(), "longitude_bounds",
                 {harp_dimension_time, harp_dimension_independent}, {2, 4},
                 "degree_east", {0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0});
    EXPECT_EQ(-1, harp_product_bin_spatial(prod.get(), 1, 2, time_bins, 3,
                                           lat_edges, 3, lon_edges));
    EXPECT_EQ(HARP_ERROR_INVALID_VARIABLE, harp_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 9. Variable with too many dimensions (num_dimensions + 2 >= 8, i.e. 6).
  {
    ScopedHarpProduct prod = make_base_product();
    ScopedHarpVariable var =
        MakeVar("too_many_dims", harp_type_double,
                {harp_dimension_time, harp_dimension_independent,
                 harp_dimension_independent, harp_dimension_independent,
                 harp_dimension_independent, harp_dimension_independent},
                {2, 1, 1, 1, 1, 1}, "1");
    ASSERT_NE(nullptr, var);
    var->data.double_data[0] = 1.0;
    var->data.double_data[1] = 2.0;
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), var.get()));
    var.release();
    EXPECT_EQ(-1, harp_product_bin_spatial(prod.get(), 1, 2, time_bins, 3,
                                           lat_edges, 3, lon_edges));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 10. Angle variable with invalid unit (cannot convert to rad) and 8D angle
  //     variable exceeding HARP_MAX_NUM_DIMS when vector-expanded, plus an
  //     area-binning error after latlon_weight allocation.
  {
    ScopedHarpProduct prod = make_base_product();
    AddDoubleVar(prod.get(), "bad_angle", {harp_dimension_time}, {2}, "m",
                 {1.0, 2.0});
    EXPECT_EQ(-1, harp_product_bin_spatial(prod.get(), 1, 2, time_bins, 3,
                                           lat_edges, 3, lon_edges));
    EXPECT_EQ(HARP_ERROR_UNIT_CONVERSION, harp_errno);

    harp_product* raw_area = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_area));
    ScopedHarpProduct area_err(raw_area);
    AddDoubleVar(area_err.get(), "latitude_bounds",
                 {harp_dimension_time, harp_dimension_independent}, {2, 2},
                 "degree_north", {-10.0, 10.0, -10.0, 10.0});
    AddDoubleVar(area_err.get(), "longitude_bounds",
                 {harp_dimension_time, harp_dimension_independent}, {2, 2},
                 "degree_east", {-10.0, 10.0, -10.0, 10.0});
    AddDoubleVar(area_err.get(), "bad_angle", {harp_dimension_time}, {2}, "m",
                 {1.0, 2.0});
    EXPECT_EQ(-1, harp_product_bin_spatial(area_err.get(), 1, 2, time_bins, 3,
                                           lat_edges, 3, lon_edges));
    EXPECT_EQ(HARP_ERROR_UNIT_CONVERSION, harp_errno);

    ScopedHarpProduct prod8 = make_base_product();
    ScopedHarpVariable angle8 =
        MakeVar("too_many_angle", harp_type_double,
                {harp_dimension_time, harp_dimension_independent,
                 harp_dimension_independent, harp_dimension_independent,
                 harp_dimension_independent, harp_dimension_independent,
                 harp_dimension_independent, harp_dimension_independent},
                {2, 1, 1, 1, 1, 1, 1, 1}, "degree");
    ASSERT_NE(nullptr, angle8);
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_add_variable(prod8.get(), angle8.get()));
    angle8.release();
    EXPECT_EQ(-1, harp_product_bin_spatial(prod8.get(), 1, 2, time_bins, 3,
                                           lat_edges, 3, lon_edges));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }
}

TEST(HarpBinSpatialTest, PointBinningAllVariableTypesAndBinTypes) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  // 5 time samples:
  // - Sample 0: lat=-15, lon=-90 -> cell (0,0), bin 0
  // - Sample 1: lat=-15, lon=-90 -> cell (0,0), bin 0
  // - Sample 2: lat=+15, lon=-90 -> cell (1,0), bin 0
  // - Sample 3: lat=+75, lon=-90 -> out of lat grid [-60, 60] (ignored)
  // - Sample 4: lat=+15, lon=+90 -> out of lon grid [-180, 0] (ignored)
  // Plus time bin 1 has 0 valid samples -> empty bin sets datetime to NaN.
  AddDoubleVar(prod.get(), "latitude", {harp_dimension_time}, {5},
               "degree_north", {-15.0, -15.0, 15.0, 75.0, 15.0});
  AddDoubleVar(prod.get(), "longitude", {harp_dimension_time}, {5},
               "degree_east", {-90.0, -90.0, -90.0, -90.0, 90.0});

  // Time axis variables (datetime covers binning_time_average; 2D
  // datetime_length below covers the invalid-dimension removal branch).
  AddDoubleVar(prod.get(), "datetime", {harp_dimension_time}, {5},
               "s since 2000-01-01", {10.0, 20.0, 30.0, 100.0, 200.0});
  AddDoubleVar(prod.get(), "datetime_start", {harp_dimension_time}, {5},
               "s since 2000-01-01", {15.0, 5.0, 25.0, 1.0, 1.0});
  AddDoubleVar(prod.get(), "datetime_stop", {harp_dimension_time}, {5},
               "s since 2000-01-01", {16.0, 26.0, 36.0, 999.0, 999.0});

  // Pre-existing count and weight variables to exercise replacement branches.
  {
    ScopedHarpVariable v =
        MakeVar("count", harp_type_int32, {harp_dimension_time}, {5}, nullptr);
    ASSERT_NE(nullptr, v);
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), v.get()));
    v.release();

    ScopedHarpVariable w =
        MakeVar("weight", harp_type_float, {harp_dimension_time}, {5}, "1");
    ASSERT_NE(nullptr, w);
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), w.get()));
    w.release();

    ScopedHarpVariable vc = MakeVar("O3_count", harp_type_int32,
                                    {harp_dimension_time}, {5}, nullptr);
    ASSERT_NE(nullptr, vc);
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), vc.get()));
    vc.release();

    ScopedHarpVariable vw =
        MakeVar("O3_weight", harp_type_float, {harp_dimension_time}, {5}, "1");
    ASSERT_NE(nullptr, vw);
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), vw.get()));
    vw.release();
  }

  // Variables that should be skipped or removed.
  AddDoubleVar(prod.get(), "altitude", {harp_dimension_vertical}, {2}, "m",
               {100.0, 200.0});
  {
    // 2D datetime_length -> invalid shape, should be removed.
    AddDoubleVar(prod.get(), "datetime_length",
                 {harp_dimension_time, harp_dimension_independent}, {5, 1}, "s",
                 {1.0, 2.0, 3.0, 4.0, 5.0});

    // Enumeration variable -> removed.
    ScopedHarpVariable ev = MakeVar("validity", harp_type_int8,
                                    {harp_dimension_time}, {5}, nullptr);
    ASSERT_NE(nullptr, ev);
    const char* enum_names[] = {"bad", "good"};
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_set_enumeration_values(ev.get(), 2, enum_names));
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), ev.get()));
    ev.release();

    // String variable -> removed.
    ScopedHarpVariable sv =
        MakeVar("note", harp_type_string, {harp_dimension_time}, {5}, nullptr);
    ASSERT_NE(nullptr, sv);
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_set_string_data_element(sv.get(), 0, "a"));
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), sv.get()));
    sv.release();

    // Unitless variable -> removed.
    ScopedHarpVariable uv = MakeVar("unitless", harp_type_double,
                                    {harp_dimension_time}, {5}, nullptr);
    ASSERT_NE(nullptr, uv);
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), uv.get()));
    uv.release();

    // AVK variable -> removed.
    AddDoubleVar(prod.get(), "O3_avk",
                 {harp_dimension_time, harp_dimension_vertical}, {5, 2}, "1",
                 {1, 1, 1, 1, 1, 1, 1, 1, 1, 1});
  }

  // Float variable with a NaN value in sample 1 (triggers per-variable weight).
  {
    ScopedHarpVariable fv =
        MakeVar("O3", harp_type_float, {harp_dimension_time}, {5}, "DU");
    ASSERT_NE(nullptr, fv);
    fv->data.float_data[0] = 300.0f;
    fv->data.float_data[1] = static_cast<float>(harp_nan());
    fv->data.float_data[2] = 320.0f;
    fv->data.float_data[3] = 999.0f;
    fv->data.float_data[4] = 999.0f;
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), fv.get()));
    fv.release();
  }

  // Multi-dimensional {time, vertical} variable.
  AddDoubleVar(prod.get(), "O3_profile",
               {harp_dimension_time, harp_dimension_vertical}, {5, 2}, "ppv",
               {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 9.0, 9.0, 9.0, 9.0});

  // Systematic and random uncertainty variables.
  AddDoubleVar(prod.get(), "O3_uncertainty", {harp_dimension_time}, {5}, "DU",
               {10.0, 20.0, 5.0, 99.0, 99.0});
  AddDoubleVar(prod.get(), "O3_uncertainty_random", {harp_dimension_time}, {5},
               "DU", {3.0, 4.0, 5.0, 99.0, 99.0});

  // Angle / direction variables (350 deg & 10 deg average to 0 deg).
  AddDoubleVar(prod.get(), "wind_direction", {harp_dimension_time}, {5},
               "degree", {350.0, 10.0, 90.0, 0.0, 0.0});

  double lat_edges[] = {-60.0, 0.0, 60.0};
  double lon_edges[] = {-180.0, 0.0};  // 2x1 spatial grid
  long time_bins[] = {0, 0, 0, 0, 1};
  ASSERT_EQ(HARP_SUCCESS, harp_product_bin_spatial(prod.get(), 2, 5, time_bins,
                                                   3, lat_edges, 2, lon_edges));

  // Verify removed variables.
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "latitude"));
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "longitude"));
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "O3_count"));
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "validity"));
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "note"));
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "unitless"));
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "O3_avk"));

  // Non-time variable retained untouched.
  EXPECT_EQ(1, harp_product_has_variable(prod.get(), "altitude"));

  // Check count and weight.
  harp_variable* var = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_get_variable_by_name(prod.get(), "count", &var));
  EXPECT_EQ(3, var->data.int32_data[0]);
  EXPECT_EQ(0, var->data.int32_data[1]);

  ASSERT_EQ(HARP_SUCCESS,
            harp_product_get_variable_by_name(prod.get(), "weight", &var));
  // Bin 0, cell (0,0): weight 2; cell (1,0): weight 1; Bin 1: 0, 0.
  EXPECT_FLOAT_EQ(2.0f, var->data.float_data[0]);
  EXPECT_FLOAT_EQ(1.0f, var->data.float_data[1]);
  EXPECT_FLOAT_EQ(0.0f, var->data.float_data[2]);
  EXPECT_FLOAT_EQ(0.0f, var->data.float_data[3]);

  // Check datetime variables.
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_get_variable_by_name(prod.get(), "datetime", &var));
  EXPECT_DOUBLE_EQ(20.0, var->data.double_data[0]);
  EXPECT_EQ(1, harp_isnan(var->data.double_data[1]));

  ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                              prod.get(), "datetime_start", &var));
  EXPECT_DOUBLE_EQ(5.0, var->data.double_data[0]);
  EXPECT_EQ(1, harp_isnan(var->data.double_data[1]));

  ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                              prod.get(), "datetime_stop", &var));
  EXPECT_DOUBLE_EQ(36.0, var->data.double_data[0]);
  EXPECT_EQ(1, harp_isnan(var->data.double_data[1]));

  // Check O3 (sample 1 was NaN, so cell (0,0) has weight 1 and value 300.0).
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_get_variable_by_name(prod.get(), "O3", &var));
  EXPECT_DOUBLE_EQ(300.0, var->data.double_data[0]);
  EXPECT_DOUBLE_EQ(320.0, var->data.double_data[1]);
  EXPECT_EQ(1, harp_isnan(var->data.double_data[2]));
  EXPECT_EQ(1, harp_product_has_variable(prod.get(), "O3_weight"));

  // Check O3_uncertainty (average) and O3_uncertainty_random (sqrt(9+16)/2
  // = 2.5).
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                              prod.get(), "O3_uncertainty", &var));
  EXPECT_DOUBLE_EQ(15.0, var->data.double_data[0]);
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                              prod.get(), "O3_uncertainty_random", &var));
  EXPECT_DOUBLE_EQ(2.5, var->data.double_data[0]);

  // Check O3_profile (2D {time, vertical} -> 4D {time, lat, lon, vertical}).
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_get_variable_by_name(prod.get(), "O3_profile", &var));
  EXPECT_EQ(4, var->num_dimensions);
  EXPECT_DOUBLE_EQ(2.0, var->data.double_data[0]);
  EXPECT_DOUBLE_EQ(3.0, var->data.double_data[1]);

  // Check wind_direction (350 & 10 -> 0 deg) and wind_direction_weight.
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                              prod.get(), "wind_direction", &var));
  EXPECT_THAT(var->data.double_data[0], DoubleNear(0.0, 1e-9));
  EXPECT_THAT(var->data.double_data[1], DoubleNear(90.0, 1e-9));
  EXPECT_EQ(1, harp_product_has_variable(prod.get(), "wind_direction_weight"));

  // Check latitude_bounds and longitude_bounds.
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                              prod.get(), "latitude_bounds", &var));
  EXPECT_DOUBLE_EQ(-60.0, var->data.double_data[0]);
  EXPECT_DOUBLE_EQ(0.0, var->data.double_data[1]);
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                              prod.get(), "longitude_bounds", &var));
  EXPECT_DOUBLE_EQ(-180.0, var->data.double_data[0]);
  EXPECT_DOUBLE_EQ(0.0, var->data.double_data[1]);
}

TEST(HarpBinSpatialTest, AreaBinningPolygonsAndEdgeCases) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  // 4x4 grid: lat {-90, -30, 0, 30, 90}, lon {-180, -60, 0, 60, 180}
  double lat_edges[] = {-90.0, -30.0, 0.0, 30.0, 90.0};
  double lon_edges[] = {-180.0, -60.0, 0.0, 60.0, 180.0};

  // 10 polygons with up to 5 vertices (NaN-padded):
  // 0: 2-vertex bounding box covering [-30, 30] x [-90, 90] (2x2 center cells)
  // 1: 4-vertex polygon spanning 3x3 cells (covers interior cell at (1,1),
  //    plus NaN padding)
  // 2: Closed 4-vertex polygon (5th vertex equals 1st) with diagonal edges
  // 3: <2 valid vertices (1 valid + NaNs) -> skipped
  // 4: Polygon completely outside lat range (grid is [-90,90], wait: lat=95 ->
  //    or use narrower grid [-60, 60]) -> tested in next sample with grid
  // 5: Antimeridian wrap polygon [135, 225] x [0, 30] (wraps via loop=1)
  // 6: North pole polygon (lon -180,-90,0,90; lat 60,60,60,60)
  // 7: South pole polygon (lon -180,-90,0,90; lat -60,-60,-60,-60)
  // 8: Equator-crossing 360-deg polygon (lon -180,-90,0,90; lat -10,10,-10,10)
  // -> skipped 9: Excessive longitude angles (>1e4 and <-1e4, lon[0]=600)
  // triggering
  //    harp_wrap and min_lon >= ref + 180 loops
  const double nan = harp_nan();
  const long num_samples = 10;
  AddDoubleVar(
      prod.get(), "latitude_bounds",
      {harp_dimension_time, harp_dimension_independent}, {num_samples, 5},
      "degree_north",
      {
          // 0: 2-vertex bounding rect
          -30.0,
          30.0,
          nan,
          nan,
          nan,
          // 1: Large diamond/box spanning lat [-45, 45], lon [-135, 135]
          //    (covers interior cells not touched by boundary)
          -45.0,
          -45.0,
          45.0,
          45.0,
          nan,
          // 2: Closed polygon (5th == 1st) going up-right, up-left, down-left,
          // down-right
          -40.0,
          0.0,
          40.0,
          0.0,
          -40.0,
          // 3: 1 valid vertex -> skipped
          10.0,
          nan,
          nan,
          nan,
          nan,
          // 4: 4-vertex polygon partially outside lat/lon bounds (clips top &
          // bottom)
          -70.0,
          -70.0,
          70.0,
          70.0,
          nan,
          // 5: Antimeridian wrap polygon [135, 225]
          0.0,
          0.0,
          30.0,
          30.0,
          nan,
          // 6: North pole covering polygon
          60.0,
          60.0,
          60.0,
          60.0,
          nan,
          // 7: South pole covering polygon
          -60.0,
          -60.0,
          -60.0,
          -60.0,
          nan,
          // 8: Equator-crossing 360-deg polygon -> skipped
          -10.0,
          10.0,
          -10.0,
          10.0,
          nan,
          // 9: Large longitude values (>1e4 / <-1e4) + lon[0]=600
          5.0,
          5.0,
          20.0,
          20.0,
          nan,
      });
  AddDoubleVar(prod.get(), "longitude_bounds",
               {harp_dimension_time, harp_dimension_independent},
               {num_samples, 5}, "degree_east",
               {
                   // 0
                   -90.0,
                   90.0,
                   nan,
                   nan,
                   nan,
                   // 1: Large box spanning lat [-45, 45], lon [-80, 80]
                   //    (covers interior cells not touched by boundary)
                   -80.0,
                   80.0,
                   80.0,
                   -80.0,
                   nan,
                   // 2
                   0.0,
                   120.0,
                   0.0,
                   -120.0,
                   0.0,
                   // 3
                   10.0,
                   nan,
                   nan,
                   nan,
                   nan,
                   // 4
                   -45.0,
                   45.0,
                   45.0,
                   -45.0,
                   nan,
                   // 5
                   135.0,
                   225.0,
                   225.0,
                   135.0,
                   nan,
                   // 6
                   -180.0,
                   -90.0,
                   0.0,
                   90.0,
                   nan,
                   // 7
                   -180.0,
                   -90.0,
                   0.0,
                   90.0,
                   nan,
                   // 8
                   -180.0,
                   -90.0,
                   0.0,
                   90.0,
                   nan,
                   // 9: lon[0]=600 triggers min_lon >= ref + 180; 20000 /
                   // -20000 trigger harp_wrap
                   600.0,
                   20000.0,
                   -20000.0,
                   20020.0,
                   nan,
               });

  AddDoubleVar(prod.get(), "O3", {harp_dimension_time}, {num_samples}, "DU",
               {100, 200, 300, 400, 500, 600, 700, 800, 900, 1000});
  AddDoubleVar(prod.get(), "O3_uncertainty_random", {harp_dimension_time},
               {num_samples}, "DU", {1, 2, 3, 4, 5, 6, 7, 8, 9, 10});

  std::vector<long> time_bins(num_samples, 0);
  ASSERT_EQ(HARP_SUCCESS, harp_product_bin_spatial(prod.get(), 1, num_samples,
                                                   time_bins.data(), 5,
                                                   lat_edges, 5, lon_edges));

  harp_variable* o3 = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_get_variable_by_name(prod.get(), "O3", &o3));
  EXPECT_EQ(3, o3->num_dimensions);
  EXPECT_EQ(1, o3->dimension[0]);
  EXPECT_EQ(4, o3->dimension[1]);
  EXPECT_EQ(4, o3->dimension[2]);

  // Verify interior cell (row 1, col 1: lat [-30,0], lon [-90,0]) and polar
  // cells (row 3: lat [30,90], row 0: lat [-90,-30]) received finite weights.
  harp_variable* weight = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_get_variable_by_name(prod.get(), "weight", &weight));
  EXPECT_GT(weight->data.float_data[1 * 4 + 1], 0.0f);
  EXPECT_GT(weight->data.float_data[3 * 4 + 0], 0.0f);
  EXPECT_GT(weight->data.float_data[0 * 4 + 0], 0.0f);
}

TEST(HarpBinSpatialTest, AreaBinningOutOfLatGridAndWrapLeft) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  // Use a restricted lat/lon grid so polygons can be outside lat_max/lat_min,
  // wrap leftward, step across -180/+180, exceed top lat edge, and clip to 0.
  double lat_edges[] = {-30.0, 0.0, 30.0};
  double lon_edges[] = {0.0, 90.0, 180.0};

  // Use a 64-vertex polygon slot so sample 5 can step +175 deg 64 times to
  // trigger harp_wrap (>1e4) on the closing vertex.
  const double nan = harp_nan();
  constexpr long kNumSamples = 9;
  constexpr long kMaxVerts = 64;
  std::vector<double> lat_vals(kNumSamples * kMaxVerts, nan);
  std::vector<double> lon_vals(kNumSamples * kMaxVerts, nan);
  auto set_poly = [&](long idx, std::initializer_list<double> lats,
                      std::initializer_list<double> lons) {
    long k = 0;
    for (double v : lats) lat_vals[idx * kMaxVerts + k++] = v;
    k = 0;
    for (double v : lons) lon_vals[idx * kMaxVerts + k++] = v;
  };
  // 0: Completely north of grid (> 30) -> skipped
  set_poly(0, {40.0, 40.0, 50.0, 50.0}, {10.0, 20.0, 20.0, 10.0});
  // 1: Completely south of grid (< -30) -> skipped
  set_poly(1, {-50.0, -50.0, -40.0, -40.0}, {10.0, 20.0, 20.0, 10.0});
  // 2: Wraps leftward past reference_longitude - 360 (-360) and lat drops
  set_poly(2, {10.0, -10.0, 10.0, 10.0}, {-150.0, -270.0, -390.0, -270.0});
  // 3: lon[0] < ref - 180 (-250), lon[1] < lon[0] - 180 (-250 -> -500),
  //    lat >= lat_edges[last] (35) at vertex 0 and vertex 2.
  set_poly(3, {35.0, -10.0, 35.0, 10.0}, {-250.0, -500.0, -250.0, -300.0});
  // 4: Steps of -140 so closing lon (10 -> -350) < min_lon (-270).
  set_poly(4, {15.0, 5.0, 15.0}, {10.0, -130.0, -270.0});
  // 5: 64 vertices stepping +175 deg each so |lon[0] - lon[63]| > 1e4.
  for (long k = 0; k < kMaxVerts; ++k) {
    lat_vals[5 * kMaxVerts + k] = (k % 2 == 0) ? 5.0 : 15.0;
    lon_vals[5 * kMaxVerts + k] = 10.0 + 175.0 * k;
  }
  // 6: Diagonal triangle touching (0, 90) from (-10, 100) -> (10, 80) -> (-10,
  // 80)
  //    enters cell (1,1) only at corner (0, 90) so lon-clamp leaves <3 points.
  set_poly(6, {-10.0, -10.0, 10.0}, {80.0, 100.0, 80.0});
  // 7: Triangle touching lat=0 at a single vertex (0, 100) from (-10, 80) ->
  //    (0, 100) -> (-10, 120) enters cell (1,1) so lat-clamp leaves <3 points.
  set_poly(7, {-10.0, 0.0, -10.0}, {80.0, 100.0, 120.0});
  // 8: Steps of -150 so closing lon > lon[last] + 180.
  set_poly(8, {15.0, 5.0, 15.0, 25.0}, {100.0, -50.0, -200.0, -350.0});

  ScopedHarpVariable lat_b =
      MakeVar("latitude_bounds", harp_type_double,
              {harp_dimension_time, harp_dimension_independent},
              {kNumSamples, kMaxVerts}, "degree_north");
  ScopedHarpVariable lon_b =
      MakeVar("longitude_bounds", harp_type_double,
              {harp_dimension_time, harp_dimension_independent},
              {kNumSamples, kMaxVerts}, "degree_east");
  ASSERT_NE(nullptr, lat_b);
  ASSERT_NE(nullptr, lon_b);
  for (long idx = 0; idx < kNumSamples * kMaxVerts; ++idx) {
    lat_b->data.double_data[idx] = lat_vals[idx];
    lon_b->data.double_data[idx] = lon_vals[idx];
  }
  ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), lat_b.get()));
  lat_b.release();
  ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), lon_b.get()));
  lon_b.release();

  AddDoubleVar(prod.get(), "O3", {harp_dimension_time}, {kNumSamples}, "DU",
               {10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0, 90.0});
  long time_bins[kNumSamples] = {0, 0, 0, 0, 0, 0, 0, 0, 0};

  ASSERT_EQ(HARP_SUCCESS,
            harp_product_bin_spatial(prod.get(), 1, kNumSamples, time_bins, 3,
                                     lat_edges, 3, lon_edges));
  harp_variable* count = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_get_variable_by_name(prod.get(), "count", &count));
  EXPECT_GE(count->data.int32_data[0], 1);
}

TEST(HarpBinSpatialTest, LargeReallocForAreaAndPointBinning) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  // 1. Area binning touching > 1024 (LATLON_BLOCK_SIZE) cells via a 40x40 grid.
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);

    std::vector<double> lat_edges(41);
    std::vector<double> lon_edges(41);
    for (int i = 0; i <= 40; ++i) {
      lat_edges[i] = -80.0 + 4.0 * i;
      lon_edges[i] = -80.0 + 4.0 * i;
    }
    AddDoubleVar(prod.get(), "latitude_bounds",
                 {harp_dimension_time, harp_dimension_independent}, {1, 4},
                 "degree_north", {-75.0, -75.0, 75.0, 75.0});
    AddDoubleVar(prod.get(), "longitude_bounds",
                 {harp_dimension_time, harp_dimension_independent}, {1, 4},
                 "degree_east", {-75.0, 75.0, 75.0, -75.0});
    AddDoubleVar(prod.get(), "O3", {harp_dimension_time}, {1}, "DU", {300.0});
    long time_bins[] = {0};

    ASSERT_EQ(HARP_SUCCESS,
              harp_product_bin_spatial(prod.get(), 1, 1, time_bins, 41,
                                       lat_edges.data(), 41, lon_edges.data()));
    harp_variable* o3 = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_get_variable_by_name(prod.get(), "O3", &o3));
    EXPECT_EQ(1600L, o3->num_elements);
    EXPECT_DOUBLE_EQ(300.0, o3->data.double_data[20 * 40 + 20]);
  }

  // 2. Point binning with > 1024 samples (triggers realloc in
  //    find_matching_cells_for_points and num_time_bins * spatial_block <= N).
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);

    constexpr long kNumSamples = 1050;
    ScopedHarpVariable lat =
        MakeVar("latitude", harp_type_double, {harp_dimension_time},
                {kNumSamples}, "degree_north");
    ScopedHarpVariable lon =
        MakeVar("longitude", harp_type_double, {harp_dimension_time},
                {kNumSamples}, "degree_east");
    ScopedHarpVariable o3 = MakeVar("O3", harp_type_double,
                                    {harp_dimension_time}, {kNumSamples}, "DU");
    ASSERT_NE(nullptr, lat);
    ASSERT_NE(nullptr, lon);
    ASSERT_NE(nullptr, o3);
    for (long i = 0; i < kNumSamples; ++i) {
      lat->data.double_data[i] = 10.0;
      lon->data.double_data[i] = 20.0;
      o3->data.double_data[i] = 310.0;
    }
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), lat.get()));
    lat.release();
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), lon.get()));
    lon.release();
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), o3.get()));
    o3.release();

    double lat_edges[] = {-90.0, 0.0, 90.0};
    double lon_edges[] = {-180.0, 0.0, 180.0};
    std::vector<long> time_bins(kNumSamples, 0);
    ASSERT_EQ(HARP_SUCCESS, harp_product_bin_spatial(prod.get(), 1, kNumSamples,
                                                     time_bins.data(), 3,
                                                     lat_edges, 3, lon_edges));
    harp_variable* count = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_get_variable_by_name(prod.get(), "count", &count));
    EXPECT_EQ(kNumSamples, count->data.int32_data[0]);
  }
}

TEST(HarpBinSpatialTest, BinSpatialFullEmptyAndSuccessAndError) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  double lat_edges[] = {-90.0, 0.0, 90.0};
  double lon_edges[] = {-180.0, 0.0, 180.0};

  // 1. Empty product (num_elements == 0) returns 0 immediately.
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);
    EXPECT_EQ(HARP_SUCCESS, harp_product_bin_spatial_full(
                                prod.get(), 3, lat_edges, 3, lon_edges));
  }

  // 2. Non-empty product bins into a single time bin.
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);
    AddDoubleVar(prod.get(), "latitude", {harp_dimension_time}, {2},
                 "degree_north", {10.0, 20.0});
    AddDoubleVar(prod.get(), "longitude", {harp_dimension_time}, {2},
                 "degree_east", {10.0, 20.0});
    AddDoubleVar(prod.get(), "O3", {harp_dimension_time}, {2}, "DU",
                 {300.0, 320.0});
    ASSERT_EQ(HARP_SUCCESS, harp_product_bin_spatial_full(
                                prod.get(), 3, lat_edges, 3, lon_edges));
    EXPECT_EQ(1L, prod->dimension[harp_dimension_time]);
    harp_variable* o3 = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_get_variable_by_name(prod.get(), "O3", &o3));
    EXPECT_DOUBLE_EQ(310.0, o3->data.double_data[1 * 2 + 1]);
  }

  // 3. Error forwarding from harp_product_bin_spatial.
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);
    AddDoubleVar(prod.get(), "O3", {harp_dimension_time}, {1}, "DU", {300.0});
    EXPECT_EQ(-1, harp_product_bin_spatial_full(prod.get(), 3, lat_edges, 3,
                                                lon_edges));
    harp_set_error(HARP_SUCCESS, nullptr);
  }
}

TEST(HarpBinSpatialTest, OutOfMemoryInAreaBinning) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  double lat_edges[] = {-90.0, 0.0, 90.0};
  double lon_edges[] = {-180.0, 0.0, 180.0};
  long time_bins[] = {0, 0};

  EXPECT_GT(SweepOomUntilSuccess([&]() {
              harp_product* prod = nullptr;
              {
                ScopedSuppressOom suppress;
                if (harp_product_new(&prod) != HARP_SUCCESS) {
                  return;
                }
                AddDoubleVar(prod, "latitude_bounds",
                             {harp_dimension_time, harp_dimension_independent},
                             {2, 2}, "degree_north", {-10.0, 10.0, -5.0, 5.0});
                AddDoubleVar(prod, "longitude_bounds",
                             {harp_dimension_time, harp_dimension_independent},
                             {2, 2}, "degree_east", {-10.0, 10.0, -5.0, 5.0});
                AddDoubleVar(prod, "datetime", {harp_dimension_time}, {2}, "s",
                             {10.0, 20.0});
                AddDoubleVar(prod, "sensor_azimuth_angle",
                             {harp_dimension_time}, {2}, "degree",
                             {45.0, 90.0});
                AddDoubleVar(prod, "O3", {harp_dimension_time}, {2}, "DU",
                             {300.0, 320.0});
              }
              harp_product_bin_spatial(prod, 1, 2, time_bins, 3, lat_edges, 3,
                                       lon_edges);
              ScopedSuppressOom suppress;
              harp_product_delete(prod);
            }),
            0);
}

}  // namespace
}  // namespace stcorp_harp
