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
// Unit tests for libharp derived variable conversions
// (libharp/harp-derived-variable-list.c).
//
// Google3 build notes: HDF4 is disabled (#undef HAVE_HDF4); udunits2 XML is
// embedded via //third_party/udunits.
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-derived-variable-list.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-derived-variable.c

#include <cmath>
#include <cstddef>
#include <span>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/oom_fault_inject.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::AssertionFailure;
using ::testing::AssertionResult;
using ::testing::AssertionSuccess;

AssertionResult AddDoubleVariable(
    harp_product* product, const char* name, const char* unit,
    std::span<const harp_dimension_type> dim_types, std::span<const long> dims,
    std::span<const double> values) {
  if (dim_types.size() != dims.size()) {
    return AssertionFailure() << "dim_types and dims size mismatch";
  }
  harp_variable* raw = nullptr;
  if (harp_variable_new(name, harp_type_double,
                        static_cast<int>(dim_types.size()), dim_types.data(),
                        dims.data(), &raw) != HARP_SUCCESS) {
    return AssertionFailure() << "harp_variable_new failed for " << name << ": "
                              << harp_errno_to_string(harp_errno);
  }
  ScopedHarpVariable var(raw);
  if (var->num_elements != static_cast<long>(values.size())) {
    return AssertionFailure()
           << "expected " << var->num_elements << " values for " << name
           << ", got " << values.size();
  }
  if (unit != nullptr) {
    if (harp_variable_set_unit(var.get(), unit) != HARP_SUCCESS) {
      return AssertionFailure() << "harp_variable_set_unit failed for " << name
                                << ": " << harp_errno_to_string(harp_errno);
    }
  }
  for (long i = 0; i < var->num_elements; ++i) {
    var->data.double_data[i] = values[static_cast<size_t>(i)];
  }
  if (harp_product_add_variable(product, var.get()) != HARP_SUCCESS) {
    return AssertionFailure() << "harp_product_add_variable failed for " << name
                              << ": " << harp_errno_to_string(harp_errno);
  }
  var.release();
  return AssertionSuccess();
}

AssertionResult CreateProduct(ScopedHarpProduct* out) {
  harp_product* raw = nullptr;
  if (harp_product_new(&raw) != HARP_SUCCESS) {
    return AssertionFailure()
           << "harp_product_new failed: " << harp_errno_to_string(harp_errno);
  }
  out->reset(raw);
  return AssertionSuccess();
}

AssertionResult DeriveDoubleVariable(
    const harp_product* product, const char* name, const char* unit,
    std::span<const harp_dimension_type> dim_types,
    ScopedHarpVariable* out_var) {
  constexpr harp_data_type kDouble = harp_type_double;
  harp_variable* raw = nullptr;
  if (harp_product_get_derived_variable(
          product, name, &kDouble, unit, static_cast<int>(dim_types.size()),
          dim_types.data(), &raw) != HARP_SUCCESS) {
    return AssertionFailure()
           << "harp_product_get_derived_variable failed for " << name << ": "
           << harp_errno_to_string(harp_errno);
  }
  out_var->reset(raw);
  return AssertionSuccess();
}

TEST(HarpDerivedVariableListTest, DatetimeAndBoundsConversions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  constexpr harp_dimension_type kTimeDim[] = {harp_dimension_time};
  constexpr harp_dimension_type kTimeBoundsDims[] = {
      harp_dimension_time, harp_dimension_independent};
  constexpr long kDims1[] = {2};
  constexpr long kDimsBounds[] = {2, 2};

  // 1. datetime_start/stop -> datetime, datetime_length, datetime_bounds.
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr double kStarts[] = {100.0, 200.0};
    constexpr double kStops[] = {110.0, 220.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "datetime_start",
                                  HARP_UNIT_DATETIME, kTimeDim, kDims1,
                                  kStarts));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "datetime_stop",
                                  HARP_UNIT_DATETIME, kTimeDim, kDims1,
                                  kStops));

    ScopedHarpVariable dt;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "datetime", HARP_UNIT_DATETIME,
                                     kTimeDim, &dt));
    EXPECT_DOUBLE_EQ(105.0, dt->data.double_data[0]);
    EXPECT_DOUBLE_EQ(210.0, dt->data.double_data[1]);

    ScopedHarpVariable len;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "datetime_length",
                                     HARP_UNIT_TIME, kTimeDim, &len));
    EXPECT_DOUBLE_EQ(10.0, len->data.double_data[0]);
    EXPECT_DOUBLE_EQ(20.0, len->data.double_data[1]);

    ScopedHarpVariable bounds;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "datetime_bounds",
                                     HARP_UNIT_DATETIME, kTimeBoundsDims,
                                     &bounds));
    EXPECT_DOUBLE_EQ(100.0, bounds->data.double_data[0]);
    EXPECT_DOUBLE_EQ(110.0, bounds->data.double_data[1]);
    EXPECT_DOUBLE_EQ(200.0, bounds->data.double_data[2]);
    EXPECT_DOUBLE_EQ(220.0, bounds->data.double_data[3]);
  }

  // 2. datetime_bounds -> datetime_start, datetime_stop.
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr double kBounds[] = {10.0, 20.0, 30.0, 50.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "datetime_bounds",
                                  HARP_UNIT_DATETIME, kTimeBoundsDims,
                                  kDimsBounds, kBounds));

    ScopedHarpVariable start;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "datetime_start",
                                     HARP_UNIT_DATETIME, kTimeDim, &start));
    EXPECT_DOUBLE_EQ(10.0, start->data.double_data[0]);
    EXPECT_DOUBLE_EQ(30.0, start->data.double_data[1]);

    ScopedHarpVariable stop;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "datetime_stop",
                                     HARP_UNIT_DATETIME, kTimeDim, &stop));
    EXPECT_DOUBLE_EQ(20.0, stop->data.double_data[0]);
    EXPECT_DOUBLE_EQ(50.0, stop->data.double_data[1]);
  }

  // 3. datetime + datetime_length -> datetime_start, datetime_stop.
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr double kMid[] = {100.0, 200.0};
    constexpr double kLen[] = {10.0, 20.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "datetime", HARP_UNIT_DATETIME,
                                  kTimeDim, kDims1, kMid));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "datetime_length", HARP_UNIT_TIME,
                                  kTimeDim, kDims1, kLen));

    ScopedHarpVariable start;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "datetime_start",
                                     HARP_UNIT_DATETIME, kTimeDim, &start));
    EXPECT_DOUBLE_EQ(95.0, start->data.double_data[0]);
    EXPECT_DOUBLE_EQ(190.0, start->data.double_data[1]);

    ScopedHarpVariable stop;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "datetime_stop",
                                     HARP_UNIT_DATETIME, kTimeDim, &stop));
    EXPECT_DOUBLE_EQ(105.0, stop->data.double_data[0]);
    EXPECT_DOUBLE_EQ(210.0, stop->data.double_data[1]);
  }

  // 4. datetime_stop + datetime_length -> datetime_start.
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr double kStop[] = {110.0, 220.0};
    constexpr double kLen[] = {10.0, 20.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "datetime_stop",
                                  HARP_UNIT_DATETIME, kTimeDim, kDims1, kStop));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "datetime_length", HARP_UNIT_TIME,
                                  kTimeDim, kDims1, kLen));

    ScopedHarpVariable start;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "datetime_start",
                                     HARP_UNIT_DATETIME, kTimeDim, &start));
    EXPECT_DOUBLE_EQ(100.0, start->data.double_data[0]);
    EXPECT_DOUBLE_EQ(200.0, start->data.double_data[1]);
  }

  // 5. datetime_start + datetime_length -> datetime_stop.
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr double kStart[] = {100.0, 200.0};
    constexpr double kLen[] = {10.0, 20.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "datetime_start",
                                  HARP_UNIT_DATETIME, kTimeDim, kDims1,
                                  kStart));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "datetime_length", HARP_UNIT_TIME,
                                  kTimeDim, kDims1, kLen));

    ScopedHarpVariable stop;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "datetime_stop",
                                     HARP_UNIT_DATETIME, kTimeDim, &stop));
    EXPECT_DOUBLE_EQ(110.0, stop->data.double_data[0]);
    EXPECT_DOUBLE_EQ(220.0, stop->data.double_data[1]);
  }
}

TEST(HarpDerivedVariableListTest, IndexYearMonthAndErrorBranches) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  constexpr harp_dimension_type kTimeDim[] = {harp_dimension_time};
  constexpr long kDims[] = {2};

  // Valid datetime -> index, month, year.
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    // 0.0 is 2000-01-01T00:00:00; 86400*40 is 2000-02-10.
    constexpr double kDt[] = {0.0, 86400.0 * 40.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "datetime", HARP_UNIT_DATETIME,
                                  kTimeDim, kDims, kDt));

    constexpr harp_data_type kInt32 = harp_type_int32;
    harp_variable* raw_idx = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_get_derived_variable(
                                prod.get(), "index", &kInt32, nullptr, 1,
                                kTimeDim, &raw_idx));
    ScopedHarpVariable idx(raw_idx);
    EXPECT_EQ(0, idx->data.int32_data[0]);
    EXPECT_EQ(1, idx->data.int32_data[1]);

    constexpr harp_data_type kInt8 = harp_type_int8;
    harp_variable* raw_month = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_get_derived_variable(
                                prod.get(), "month", &kInt8, nullptr, 1,
                                kTimeDim, &raw_month));
    ScopedHarpVariable month(raw_month);
    EXPECT_EQ(12, month->num_enum_values);
    EXPECT_EQ(0, month->data.int8_data[0]);  // January
    EXPECT_EQ(1, month->data.int8_data[1]);  // February

    constexpr harp_data_type kInt16 = harp_type_int16;
    harp_variable* raw_year = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_get_derived_variable(
                                prod.get(), "year", &kInt16, nullptr, 1,
                                kTimeDim, &raw_year));
    ScopedHarpVariable year(raw_year);
    EXPECT_EQ(2000, year->data.int16_data[0]);
    EXPECT_EQ(2000, year->data.int16_data[1]);
  }

  // Invalid datetime (NaN) -> coda_time_double_to_parts error in month and
  // year.
  {
    harp_set_error(HARP_SUCCESS, nullptr);
    auto reset_err =
        absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kSingle[] = {1};
    const double kNan[] = {harp_nan()};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "datetime", HARP_UNIT_DATETIME,
                                  kTimeDim, kSingle, kNan));

    constexpr harp_data_type kInt8 = harp_type_int8;
    harp_variable* var = nullptr;
    EXPECT_EQ(-1, harp_product_get_derived_variable(
                      prod.get(), "month", &kInt8, nullptr, 1, kTimeDim, &var));
    EXPECT_EQ(nullptr, var);

    constexpr harp_data_type kInt16 = harp_type_int16;
    EXPECT_EQ(-1, harp_product_get_derived_variable(
                      prod.get(), "year", &kInt16, nullptr, 1, kTimeDim, &var));
    EXPECT_EQ(nullptr, var);
  }
}

TEST(HarpDerivedVariableListTest, GeometryAreaLatLonAndBoundsWrapping) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  constexpr harp_dimension_type kTimeDim[] = {harp_dimension_time};
  constexpr harp_dimension_type kTimeIndep[] = {harp_dimension_time,
                                                harp_dimension_independent};
  constexpr harp_dimension_type kLatDim[] = {harp_dimension_latitude};
  constexpr harp_dimension_type kLonDim[] = {harp_dimension_longitude};
  constexpr harp_dimension_type kLatLonDim[] = {harp_dimension_latitude,
                                                harp_dimension_longitude};
  constexpr harp_dimension_type kLatBoundsDim[] = {harp_dimension_latitude,
                                                   harp_dimension_independent};
  constexpr harp_dimension_type kLonBoundsDim[] = {harp_dimension_longitude,
                                                   harp_dimension_independent};

  // 1. area, latitude, longitude from polygon bounds (time-dependent).
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kBoundsDims[] = {1, 4};
    constexpr double kLatBounds[] = {0.0, 0.0, 1.0, 1.0};
    constexpr double kLonBounds[] = {0.0, 1.0, 1.0, 0.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "latitude_bounds",
                                  HARP_UNIT_LATITUDE, kTimeIndep, kBoundsDims,
                                  kLatBounds));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "longitude_bounds",
                                  HARP_UNIT_LONGITUDE, kTimeIndep, kBoundsDims,
                                  kLonBounds));

    ScopedHarpVariable area;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "area", HARP_UNIT_AREA,
                                     kTimeDim, &area));
    EXPECT_GT(area->data.double_data[0], 0.0);

    ScopedHarpVariable lat;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "latitude", HARP_UNIT_LATITUDE,
                                     kTimeDim, &lat));
    EXPECT_NEAR(0.5, lat->data.double_data[0], 0.05);

    ScopedHarpVariable lon;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "longitude",
                                     HARP_UNIT_LONGITUDE, kTimeDim, &lon));
    EXPECT_NEAR(0.5, lon->data.double_data[0], 0.05);
  }

  // 2. Gridded area from latitude_bounds {latitude,2} + longitude_bounds
  // {longitude,2} + midpoint from bounds.
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kGridBounds[] = {2, 2};
    constexpr double kLatBounds[] = {0.0, 1.0, 1.0, 2.0};
    constexpr double kLonBounds[] = {10.0, 11.0, 11.0, 12.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "latitude_bounds",
                                  HARP_UNIT_LATITUDE, kLatBoundsDim,
                                  kGridBounds, kLatBounds));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "longitude_bounds",
                                  HARP_UNIT_LONGITUDE, kLonBoundsDim,
                                  kGridBounds, kLonBounds));

    ScopedHarpVariable area;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "area", HARP_UNIT_AREA,
                                     kLatLonDim, &area));
    EXPECT_EQ(4, area->num_elements);
    EXPECT_GT(area->data.double_data[0], 0.0);

    ScopedHarpVariable lat;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "latitude", HARP_UNIT_LATITUDE,
                                     kLatDim, &lat));
    EXPECT_DOUBLE_EQ(0.5, lat->data.double_data[0]);
    EXPECT_DOUBLE_EQ(1.5, lat->data.double_data[1]);
  }

  // 3. Mismatched independent dimension lengths and invalid polygon errors.
  {
    harp_set_error(HARP_SUCCESS, nullptr);
    auto reset_err =
        absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDims4[] = {1, 4};
    constexpr long kDims3[] = {1, 3};
    constexpr double kLatB[] = {0.0, 0.0, 1.0, 1.0};
    constexpr double kLonB[] = {0.0, 1.0, 1.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "latitude_bounds",
                                  HARP_UNIT_LATITUDE, kTimeIndep, kDims4,
                                  kLatB));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "longitude_bounds",
                                  HARP_UNIT_LONGITUDE, kTimeIndep, kDims3,
                                  kLonB));

    ScopedHarpVariable var;
    EXPECT_FALSE(DeriveDoubleVariable(prod.get(), "area", HARP_UNIT_AREA,
                                      kTimeDim, &var));
    EXPECT_EQ(HARP_ERROR_INVALID_VARIABLE, harp_errno);
    EXPECT_FALSE(DeriveDoubleVariable(prod.get(), "latitude",
                                      HARP_UNIT_LATITUDE, kTimeDim, &var));
    EXPECT_EQ(HARP_ERROR_INVALID_VARIABLE, harp_errno);
    EXPECT_FALSE(DeriveDoubleVariable(prod.get(), "longitude",
                                      HARP_UNIT_LONGITUDE, kTimeDim, &var));
    EXPECT_EQ(HARP_ERROR_INVALID_VARIABLE, harp_errno);
  }
  {
    harp_set_error(HARP_SUCCESS, nullptr);
    auto reset_err =
        absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    // A 1-vertex polygon has begin == end, so num_points drops to 0 and
    // harp_spherical_polygon_from_latitude_longitude_bounds returns -1.
    constexpr long kDims1[] = {1, 1};
    constexpr double kZero[] = {0.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "latitude_bounds",
                                  HARP_UNIT_LATITUDE, kTimeIndep, kDims1,
                                  kZero));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "longitude_bounds",
                                  HARP_UNIT_LONGITUDE, kTimeIndep, kDims1,
                                  kZero));

    ScopedHarpVariable var;
    EXPECT_FALSE(DeriveDoubleVariable(prod.get(), "area", HARP_UNIT_AREA,
                                      kTimeDim, &var));
    EXPECT_FALSE(DeriveDoubleVariable(prod.get(), "latitude",
                                      HARP_UNIT_LATITUDE, kTimeDim, &var));
    EXPECT_FALSE(DeriveDoubleVariable(prod.get(), "longitude",
                                      HARP_UNIT_LONGITUDE, kTimeDim, &var));
  }
  {
    harp_set_error(HARP_SUCCESS, nullptr);
    auto reset_err =
        absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    // Identical min/max latitude bounds trigger overlapping line segments (-1).
    constexpr long kDimsBad[] = {1, 2};
    constexpr double kSameLat[] = {10.0, 10.0};
    constexpr double kLon[] = {0.0, 1.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "latitude_bounds",
                                  HARP_UNIT_LATITUDE, kLatBoundsDim, kDimsBad,
                                  kSameLat));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "longitude_bounds",
                                  HARP_UNIT_LONGITUDE, kLonBoundsDim, kDimsBad,
                                  kLon));

    ScopedHarpVariable var;
    EXPECT_FALSE(DeriveDoubleVariable(prod.get(), "area", HARP_UNIT_AREA,
                                      kLatLonDim, &var));
  }

  // 4. latitude_bounds clamping and longitude_bounds wrapping/NaN +
  // regrid_out_of_bounds option toggle.
  {
    const int saved_oob = harp_get_option_regrid_out_of_bounds();
    auto restore_oob = absl::MakeCleanup(
        [&] { harp_set_option_regrid_out_of_bounds(saved_oob); });

    for (int oob_mode : {0, 1}) {
      ASSERT_EQ(HARP_SUCCESS, harp_set_option_regrid_out_of_bounds(oob_mode));

      ScopedHarpProduct prod;
      ASSERT_TRUE(CreateProduct(&prod));
      constexpr long kLatN[] = {2};
      constexpr double kLats[] = {-90.0, 90.0};
      ASSERT_TRUE(AddDoubleVariable(prod.get(), "latitude", HARP_UNIT_LATITUDE,
                                    kLatDim, kLatN, kLats));

      ScopedHarpVariable lat_b;
      ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "latitude_bounds",
                                       HARP_UNIT_LATITUDE, kLatBoundsDim,
                                       &lat_b));
      EXPECT_GE(lat_b->data.double_data[0], -90.0);
      EXPECT_LE(lat_b->data.double_data[3], 90.0);
    }

    ASSERT_EQ(HARP_SUCCESS, harp_set_option_regrid_out_of_bounds(2));
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    // Trigger all longitude_bounds branches:
    //   1) < -1e4 -> NaN
    //   2) -180 - 1e-12 (< EPSILON) -> clamped to -180
    //   3) < -180 (by > EPSILON) -> += 360
    //   4) > 180 (by > EPSILON) -> -= 360
    //   5) 180 + 1e-12 (< EPSILON) -> clamped to 180
    //   6) > 1e4 -> NaN
    constexpr long kLonN[] = {2};
    constexpr double kLons1[] = {-20000.0, 20000.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "longitude", HARP_UNIT_LONGITUDE,
                                  kLonDim, kLonN, kLons1));
    ScopedHarpVariable lon_b1;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "longitude_bounds",
                                     HARP_UNIT_LONGITUDE, kLonBoundsDim,
                                     &lon_b1));
    EXPECT_TRUE(std::isnan(lon_b1->data.double_data[0]));
    EXPECT_TRUE(std::isnan(lon_b1->data.double_data[3]));

    // Test near-boundary wrapping around -180 and +180.
    ScopedHarpProduct prod2;
    ASSERT_TRUE(CreateProduct(&prod2));
    constexpr long kLonN2[] = {4};
    // Midpoints [-180 - 0.5e-11, -180 + 0.5e-11, 180 - 0.5e-11, 180 + 0.5e-11]
    // produce extrapolated bounds slightly below -180 and slightly above +180
    // (within EPSILON) plus values wrapping by 360 when grid step is large.
    constexpr double kLons2[] = {-180.0 - 0.5e-11, -180.0 + 0.5e-11,
                                 180.0 - 0.5e-11, 180.0 + 0.5e-11};
    ASSERT_TRUE(AddDoubleVariable(prod2.get(), "longitude", HARP_UNIT_LONGITUDE,
                                  kLonDim, kLonN2, kLons2));
    ScopedHarpVariable lon_b2;
    ASSERT_TRUE(DeriveDoubleVariable(prod2.get(), "longitude_bounds",
                                     HARP_UNIT_LONGITUDE, kLonBoundsDim,
                                     &lon_b2));
    EXPECT_DOUBLE_EQ(-180.0, lon_b2->data.double_data[0]);
    EXPECT_DOUBLE_EQ(180.0, lon_b2->data.double_data[7]);

    ScopedHarpProduct prod3;
    ASSERT_TRUE(CreateProduct(&prod3));
    constexpr double kLons3[] = {-175.0, -165.0, 165.0, 175.0};
    ASSERT_TRUE(AddDoubleVariable(prod3.get(), "longitude", HARP_UNIT_LONGITUDE,
                                  kLonDim, kLonN2, kLons3));
    ScopedHarpVariable lon_b3;
    ASSERT_TRUE(DeriveDoubleVariable(prod3.get(), "longitude_bounds",
                                     HARP_UNIT_LONGITUDE, kLonBoundsDim,
                                     &lon_b3));
    for (long i = 0; i < lon_b3->num_elements; ++i) {
      EXPECT_GE(lon_b3->data.double_data[i], -180.0);
      EXPECT_LE(lon_b3->data.double_data[i], 180.0);
    }

    // Wrap by >360 degrees (-190 -> 170, 190 -> -170).
    ScopedHarpProduct prod4;
    ASSERT_TRUE(CreateProduct(&prod4));
    constexpr double kLons4[] = {-170.0, -130.0};
    ASSERT_TRUE(AddDoubleVariable(prod4.get(), "longitude", HARP_UNIT_LONGITUDE,
                                  kLonDim, kLonN, kLons4));
    ScopedHarpVariable lon_b4;
    ASSERT_TRUE(DeriveDoubleVariable(prod4.get(), "longitude_bounds",
                                     HARP_UNIT_LONGITUDE, kLonBoundsDim,
                                     &lon_b4));
    EXPECT_DOUBLE_EQ(170.0, lon_b4->data.double_data[0]);

    ScopedHarpProduct prod5;
    ASSERT_TRUE(CreateProduct(&prod5));
    constexpr double kLons5[] = {130.0, 170.0};
    ASSERT_TRUE(AddDoubleVariable(prod5.get(), "longitude", HARP_UNIT_LONGITUDE,
                                  kLonDim, kLonN, kLons5));
    ScopedHarpVariable lon_b5;
    ASSERT_TRUE(DeriveDoubleVariable(prod5.get(), "longitude_bounds",
                                     HARP_UNIT_LONGITUDE, kLonBoundsDim,
                                     &lon_b5));
    EXPECT_DOUBLE_EQ(-170.0, lon_b5->data.double_data[3]);
  }
}

TEST(HarpDerivedVariableListTest, DimensionExpansionAndVerticalMidPoint) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  constexpr harp_dimension_type kTimeDim[] = {harp_dimension_time};
  constexpr harp_dimension_type kLatDim[] = {harp_dimension_latitude};
  constexpr harp_dimension_type kLonDim[] = {harp_dimension_longitude};
  constexpr harp_dimension_type kLatLonDim[] = {harp_dimension_latitude,
                                                harp_dimension_longitude};
  constexpr harp_dimension_type kTimeVertDim[] = {harp_dimension_time,
                                                  harp_dimension_vertical};

  // 1. Sensor lat/lon/alt copy and vertical_mid_point with trailing NaN.
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDims1[] = {1};
    constexpr double kLat[] = {52.0};
    constexpr double kLon[] = {4.0};
    constexpr double kAlt[] = {800000.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "sensor_latitude",
                                  HARP_UNIT_LATITUDE, kTimeDim, kDims1, kLat));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "sensor_longitude",
                                  HARP_UNIT_LONGITUDE, kTimeDim, kDims1, kLon));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "sensor_altitude",
                                  HARP_UNIT_LENGTH, kTimeDim, kDims1, kAlt));

    ScopedHarpVariable lat;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "latitude", HARP_UNIT_LATITUDE,
                                     kTimeDim, &lat));
    EXPECT_DOUBLE_EQ(52.0, lat->data.double_data[0]);

    ScopedHarpVariable lon;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "longitude",
                                     HARP_UNIT_LONGITUDE, kTimeDim, &lon));
    EXPECT_DOUBLE_EQ(4.0, lon->data.double_data[0]);

    ScopedHarpVariable alt;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "altitude", HARP_UNIT_LENGTH,
                                     kTimeDim, &alt));
    EXPECT_DOUBLE_EQ(800000.0, alt->data.double_data[0]);
  }

  // 2. get_vertical_mid_point ({time} from {time, vertical}) with trailing NaN.
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDimsTV[] = {1, 4};
    const double kLats[] = {10.0, 20.0, 30.0, harp_nan()};
    const double kLons[] = {1.0, 2.0, 3.0, harp_nan()};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "latitude", HARP_UNIT_LATITUDE,
                                  kTimeVertDim, kDimsTV, kLats));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "longitude", HARP_UNIT_LONGITUDE,
                                  kTimeVertDim, kDimsTV, kLons));

    ScopedHarpVariable lat;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "latitude", HARP_UNIT_LATITUDE,
                                     kTimeDim, &lat));
    EXPECT_DOUBLE_EQ(20.0, lat->data.double_data[0]);

    ScopedHarpVariable lon;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "longitude",
                                     HARP_UNIT_LONGITUDE, kTimeDim, &lon));
    EXPECT_DOUBLE_EQ(2.0, lon->data.double_data[0]);
  }

  // 3. get_expanded_dimension: scalar -> {time}, {lat}+{lon} -> {lat, lon}.
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr double kScalar[] = {45.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "latitude", HARP_UNIT_LATITUDE,
                                  {}, {}, kScalar));

    ScopedHarpVariable lat_t;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "latitude", HARP_UNIT_LATITUDE,
                                     kTimeDim, &lat_t));
    EXPECT_EQ(1, lat_t->dimension[0]);
    EXPECT_DOUBLE_EQ(45.0, lat_t->data.double_data[0]);
  }
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDims2[] = {2};
    constexpr double kLats[] = {10.0, 20.0};
    constexpr double kLons[] = {100.0, 110.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "latitude", HARP_UNIT_LATITUDE,
                                  kLatDim, kDims2, kLats));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "longitude", HARP_UNIT_LONGITUDE,
                                  kLonDim, kDims2, kLons));

    ScopedHarpVariable lat_2d;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "latitude", HARP_UNIT_LATITUDE,
                                     kLatLonDim, &lat_2d));
    EXPECT_EQ(4, lat_2d->num_elements);

    ScopedHarpVariable lon_2d;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "longitude",
                                     HARP_UNIT_LONGITUDE, kLatLonDim, &lon_2d));
    EXPECT_EQ(4, lon_2d->num_elements);
  }
}

TEST(HarpDerivedVariableListTest, SpectralAndAngleConversions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  constexpr harp_dimension_type kTimeDim[] = {harp_dimension_time};
  constexpr harp_dimension_type kSpecDim[] = {harp_dimension_spectral};
  constexpr harp_dimension_type kTimeSpecDim[] = {harp_dimension_time,
                                                  harp_dimension_spectral};

  // 1. Wavelength <-> frequency <-> wavenumber ({time, spectral}).
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDims[] = {1, 2};
    constexpr double kWl[] = {500e-9, 1000e-9};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "wavelength",
                                  HARP_UNIT_WAVELENGTH, kTimeSpecDim, kDims,
                                  kWl));

    ScopedHarpVariable freq;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "frequency",
                                     HARP_UNIT_FREQUENCY, kTimeSpecDim, &freq));
    EXPECT_GT(freq->data.double_data[0], 0.0);

    ScopedHarpVariable wn;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "wavenumber",
                                     HARP_UNIT_WAVENUMBER, kTimeSpecDim, &wn));
    EXPECT_NEAR(2e6, wn->data.double_data[0], 1e-3);
  }
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDims[] = {1, 1};
    constexpr double kFreq[] = {3e14};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "frequency", HARP_UNIT_FREQUENCY,
                                  kTimeSpecDim, kDims, kFreq));
    ScopedHarpVariable wl;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "wavelength",
                                     HARP_UNIT_WAVELENGTH, kTimeSpecDim, &wl));
    EXPECT_GT(wl->data.double_data[0], 0.0);
    ScopedHarpVariable wn;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "wavenumber",
                                     HARP_UNIT_WAVENUMBER, kTimeSpecDim, &wn));
    EXPECT_GT(wn->data.double_data[0], 0.0);
  }
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDims[] = {1, 1};
    constexpr double kWn[] = {1e6};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "wavenumber",
                                  HARP_UNIT_WAVENUMBER, kTimeSpecDim, kDims,
                                  kWn));
    ScopedHarpVariable freq;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "frequency",
                                     HARP_UNIT_FREQUENCY, kTimeSpecDim, &freq));
    EXPECT_GT(freq->data.double_data[0], 0.0);
    ScopedHarpVariable wl;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "wavelength",
                                     HARP_UNIT_WAVELENGTH, kTimeSpecDim, &wl));
    EXPECT_NEAR(1e-6, wl->data.double_data[0], 1e-12);
  }

  // 2. Angstrom exponent from wavelength + AOD.
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kSpecN[] = {2};
    constexpr long kTimeSpecN[] = {1, 2};
    constexpr double kWl[] = {440e-9, 870e-9};
    constexpr double kAod[] = {0.4, 0.2};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "wavelength",
                                  HARP_UNIT_WAVELENGTH, kSpecDim, kSpecN, kWl));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "aerosol_optical_depth",
                                  HARP_UNIT_DIMENSIONLESS, kTimeSpecDim,
                                  kTimeSpecN, kAod));

    ScopedHarpVariable ang;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "angstrom_exponent",
                                     HARP_UNIT_DIMENSIONLESS, kTimeDim, &ang));
    EXPECT_GT(ang->data.double_data[0], 0.0);
  }

  // 3. Reflectance from radiance, irradiance, solar_zenith_angle.
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDims[] = {1};
    constexpr double kRad[] = {10.0};
    constexpr double kIrr[] = {100.0};
    constexpr double kSza[] = {30.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "radiance", HARP_UNIT_RADIANCE,
                                  kTimeDim, kDims, kRad));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "irradiance",
                                  HARP_UNIT_IRRADIANCE, kTimeDim, kDims, kIrr));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "solar_zenith_angle",
                                  HARP_UNIT_ANGLE, kTimeDim, kDims, kSza));

    ScopedHarpVariable refl;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "reflectance",
                                     HARP_UNIT_DIMENSIONLESS, kTimeDim, &refl));
    EXPECT_GT(refl->data.double_data[0], 0.0);
  }

  // 4. Sensor, viewing, solar, relative azimuth, and scattering angles.
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDims[] = {1};
    constexpr double kDt[] = {86400.0 * 172.0};  // Summer solstice-ish
    constexpr double kLat[] = {45.0};
    constexpr double kLon[] = {5.0};
    constexpr double kViewZen[] = {20.0};
    constexpr double kViewAz[] = {90.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "datetime", HARP_UNIT_DATETIME,
                                  kTimeDim, kDims, kDt));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "latitude", HARP_UNIT_LATITUDE,
                                  kTimeDim, kDims, kLat));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "longitude", HARP_UNIT_LONGITUDE,
                                  kTimeDim, kDims, kLon));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "viewing_zenith_angle",
                                  HARP_UNIT_ANGLE, kTimeDim, kDims, kViewZen));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "viewing_azimuth_angle",
                                  HARP_UNIT_ANGLE, kTimeDim, kDims, kViewAz));

    // Derive sensor_zenith/azimuth -> sensor_elevation, solar_declination,
    // solar_hour, solar_zenith, solar_azimuth, relative_azimuth, scattering.
    ScopedHarpVariable sensor_el;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "sensor_elevation_angle",
                                     HARP_UNIT_ANGLE, kTimeDim, &sensor_el));
    EXPECT_TRUE(std::isfinite(sensor_el->data.double_data[0]));

    ScopedHarpVariable solar_az;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "solar_azimuth_angle",
                                     HARP_UNIT_ANGLE, kTimeDim, &solar_az));
    EXPECT_TRUE(std::isfinite(solar_az->data.double_data[0]));

    ScopedHarpVariable scat;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "scattering_angle",
                                     HARP_UNIT_ANGLE, kTimeDim, &scat));
    EXPECT_TRUE(std::isfinite(scat->data.double_data[0]));

    ScopedHarpVariable solar_el;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "solar_elevation_angle",
                                     HARP_UNIT_ANGLE, kTimeDim, &solar_el));
    EXPECT_TRUE(std::isfinite(solar_el->data.double_data[0]));
  }
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDims[] = {1};
    constexpr double kSensEl[] = {70.0};
    constexpr double kSensAz[] = {120.0};
    constexpr double kSolarEl[] = {50.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "sensor_elevation_angle",
                                  HARP_UNIT_ANGLE, kTimeDim, kDims, kSensEl));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "sensor_azimuth_angle",
                                  HARP_UNIT_ANGLE, kTimeDim, kDims, kSensAz));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "solar_elevation_angle",
                                  HARP_UNIT_ANGLE, kTimeDim, kDims, kSolarEl));

    ScopedHarpVariable view_az;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "viewing_azimuth_angle",
                                     HARP_UNIT_ANGLE, kTimeDim, &view_az));
    EXPECT_TRUE(std::isfinite(view_az->data.double_data[0]));

    ScopedHarpVariable sza;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "solar_zenith_angle",
                                     HARP_UNIT_ANGLE, kTimeDim, &sza));
    EXPECT_DOUBLE_EQ(40.0, sza->data.double_data[0]);

    ScopedHarpVariable sens_za;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "sensor_zenith_angle",
                                     HARP_UNIT_ANGLE, kTimeDim, &sens_za));
    EXPECT_DOUBLE_EQ(20.0, sens_za->data.double_data[0]);
  }
}

TEST(HarpDerivedVariableListTest, ThermodynamicsWindAndGravityConversions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  constexpr harp_dimension_type kTimeDim[] = {harp_dimension_time};
  constexpr harp_dimension_type kTimeVertDim[] = {harp_dimension_time,
                                                  harp_dimension_vertical};
  constexpr harp_dimension_type kTimeVertBoundsDim[] = {
      harp_dimension_time, harp_dimension_vertical, harp_dimension_independent};

  // 1. Dry/wet air, H2O, molar_mass, density, number_density, pressure,
  // temperature, virtual_temperature, cloud_height.
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDims[] = {1};
    constexpr double kDryCol[] = {2.0e29};
    constexpr double kH2oCol[] = {5.0e27};
    constexpr double kDryDens[] = {1.18};
    constexpr double kH2oDens[] = {0.02};
    constexpr double kDryNd[] = {2.4e25};
    constexpr double kH2oNd[] = {5.0e23};
    constexpr double kCloudBase[] = {1000.0};
    constexpr double kCloudTop[] = {3000.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "dry_air_column_density",
                                  HARP_UNIT_COLUMN_MASS_DENSITY, kTimeDim,
                                  kDims, kDryCol));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "H2O_column_density",
                                  HARP_UNIT_COLUMN_MASS_DENSITY, kTimeDim,
                                  kDims, kH2oCol));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "dry_air_density",
                                  HARP_UNIT_MASS_DENSITY, kTimeDim, kDims,
                                  kDryDens));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "H2O_density",
                                  HARP_UNIT_MASS_DENSITY, kTimeDim, kDims,
                                  kH2oDens));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "dry_air_number_density",
                                  HARP_UNIT_NUMBER_DENSITY, kTimeDim, kDims,
                                  kDryNd));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "H2O_number_density",
                                  HARP_UNIT_NUMBER_DENSITY, kTimeDim, kDims,
                                  kH2oNd));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "cloud_base_height",
                                  HARP_UNIT_LENGTH, kTimeDim, kDims,
                                  kCloudBase));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "cloud_top_height",
                                  HARP_UNIT_LENGTH, kTimeDim, kDims,
                                  kCloudTop));

    ScopedHarpVariable col_dens;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "column_density",
                                     HARP_UNIT_COLUMN_MASS_DENSITY, kTimeDim,
                                     &col_dens));
    EXPECT_DOUBLE_EQ(2.05e29, col_dens->data.double_data[0]);

    ScopedHarpVariable dens;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "density",
                                     HARP_UNIT_MASS_DENSITY, kTimeDim, &dens));
    EXPECT_DOUBLE_EQ(1.20, dens->data.double_data[0]);

    ScopedHarpVariable nd;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "number_density",
                                     HARP_UNIT_NUMBER_DENSITY, kTimeDim, &nd));
    EXPECT_DOUBLE_EQ(2.45e25, nd->data.double_data[0]);

    ScopedHarpVariable mm;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "molar_mass",
                                     HARP_UNIT_MOLAR_MASS, kTimeDim, &mm));
    EXPECT_GT(mm->data.double_data[0], 0.0);

    ScopedHarpVariable ch;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "cloud_height",
                                     HARP_UNIT_LENGTH, kTimeDim, &ch));
    EXPECT_DOUBLE_EQ(2000.0, ch->data.double_data[0]);
  }

  // 2. Inverse dry_air and H2O derivations, plus molar_mass from H2O_vmr/mmr.
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDims[] = {1};
    constexpr double kDens[] = {1.2};
    constexpr double kNd[] = {2.5e25};
    constexpr double kTemp[] = {288.15};
    constexpr double kH2oVmr[] = {0.01};
    constexpr double kH2oMmr[] = {0.006};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "density", HARP_UNIT_MASS_DENSITY,
                                  kTimeDim, kDims, kDens));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "number_density",
                                  HARP_UNIT_NUMBER_DENSITY, kTimeDim, kDims,
                                  kNd));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "temperature",
                                  HARP_UNIT_TEMPERATURE, kTimeDim, kDims,
                                  kTemp));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "H2O_volume_mixing_ratio",
                                  HARP_UNIT_VOLUME_MIXING_RATIO, kTimeDim,
                                  kDims, kH2oVmr));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "H2O_mass_mixing_ratio",
                                  HARP_UNIT_MASS_MIXING_RATIO, kTimeDim, kDims,
                                  kH2oMmr));

    ScopedHarpVariable pres;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "pressure", HARP_UNIT_PRESSURE,
                                     kTimeDim, &pres));
    EXPECT_GT(pres->data.double_data[0], 0.0);

    ScopedHarpVariable vtemp;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "virtual_temperature",
                                     HARP_UNIT_TEMPERATURE, kTimeDim, &vtemp));
    EXPECT_GT(vtemp->data.double_data[0], 288.0);

    ScopedHarpVariable dry_vmr;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "dry_air_volume_mixing_ratio",
                                     HARP_UNIT_VOLUME_MIXING_RATIO, kTimeDim,
                                     &dry_vmr));
    EXPECT_DOUBLE_EQ(0.99, dry_vmr->data.double_data[0]);

    ScopedHarpVariable dry_mmr;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "dry_air_mass_mixing_ratio",
                                     HARP_UNIT_MASS_MIXING_RATIO, kTimeDim,
                                     &dry_mmr));
    EXPECT_DOUBLE_EQ(0.994, dry_mmr->data.double_data[0]);
  }
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDims[] = {1};
    constexpr double kPres[] = {101325.0};
    constexpr double kNd[] = {2.5e25};
    constexpr double kVtemp[] = {290.0};
    constexpr double kH2oVmrDry[] = {0.01};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "pressure", HARP_UNIT_PRESSURE,
                                  kTimeDim, kDims, kPres));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "number_density",
                                  HARP_UNIT_NUMBER_DENSITY, kTimeDim, kDims,
                                  kNd));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "virtual_temperature",
                                  HARP_UNIT_TEMPERATURE, kTimeDim, kDims,
                                  kVtemp));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "H2O_volume_mixing_ratio_dry_air",
                                  HARP_UNIT_VOLUME_MIXING_RATIO, kTimeDim,
                                  kDims, kH2oVmrDry));

    ScopedHarpVariable temp;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "temperature",
                                     HARP_UNIT_TEMPERATURE, kTimeDim, &temp));
    EXPECT_GT(temp->data.double_data[0], 200.0);

    ScopedHarpVariable dry_vmr;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "dry_air_volume_mixing_ratio",
                                     HARP_UNIT_VOLUME_MIXING_RATIO, kTimeDim,
                                     &dry_vmr));
    EXPECT_NEAR(1.0 / 1.01, dry_vmr->data.double_data[0], 1e-12);
  }
  {
    // get_temperature_from_virtual_temperature directly + get_h2o_mixing_ratio
    // + get_molar_mass_from_h2o_mmr + get_nd_from_pressure_and_temperature.
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDims[] = {1};
    constexpr double kVtemp[] = {290.0};
    constexpr double kPres[] = {101325.0};
    constexpr double kH2oMmr[] = {0.006};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "virtual_temperature",
                                  HARP_UNIT_TEMPERATURE, kTimeDim, kDims,
                                  kVtemp));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "pressure", HARP_UNIT_PRESSURE,
                                  kTimeDim, kDims, kPres));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "H2O_mass_mixing_ratio",
                                  HARP_UNIT_MASS_MIXING_RATIO, kTimeDim, kDims,
                                  kH2oMmr));

    ScopedHarpVariable temp;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "temperature",
                                     HARP_UNIT_TEMPERATURE, kTimeDim, &temp));
    EXPECT_GT(temp->data.double_data[0], 200.0);

    ScopedHarpVariable nd;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "number_density",
                                     HARP_UNIT_NUMBER_DENSITY, kTimeDim, &nd));
    EXPECT_GT(nd->data.double_data[0], 0.0);
  }
  {
    // get_molar_mass_from_h2o_vmr +
    // get_h2o_mixing_ratio_from_dry_air_mixing_ratio.
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDims[] = {1};
    constexpr double kDryVmr[] = {0.99};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "dry_air_volume_mixing_ratio",
                                  HARP_UNIT_VOLUME_MIXING_RATIO, kTimeDim,
                                  kDims, kDryVmr));

    ScopedHarpVariable mm;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "molar_mass",
                                     HARP_UNIT_MOLAR_MASS, kTimeDim, &mm));
    EXPECT_GT(mm->data.double_data[0], 28.0);
  }
  {
    // get_air_nd_from_species_nd_and_vmr (column_density and column_nd) and
    // get_dry_air_from_air_and_h2o + get_h2o_from_air_and_dry_air.
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDims[] = {1};
    constexpr double kColNd[] = {1.0e29};
    constexpr double kDryColNd[] = {0.98e29};
    constexpr double kO3ColNd[] = {8.0e22};
    constexpr double kO3ColVmr[] = {8.0e-7};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "column_number_density",
                                  HARP_UNIT_COLUMN_NUMBER_DENSITY, kTimeDim,
                                  kDims, kColNd));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "dry_air_column_number_density",
                                  HARP_UNIT_COLUMN_NUMBER_DENSITY, kTimeDim,
                                  kDims, kDryColNd));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "O3_column_number_density",
                                  HARP_UNIT_COLUMN_NUMBER_DENSITY, kTimeDim,
                                  kDims, kO3ColNd));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "O3_column_volume_mixing_ratio",
                                  HARP_UNIT_VOLUME_MIXING_RATIO, kTimeDim,
                                  kDims, kO3ColVmr));

    ScopedHarpVariable h2o_col;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "H2O_column_number_density",
                                     HARP_UNIT_COLUMN_NUMBER_DENSITY, kTimeDim,
                                     &h2o_col));
    EXPECT_NEAR(0.02e29, h2o_col->data.double_data[0], 1e24);
  }

  // 3. Wind direction/speed <-> U/V (including angle > 180 wrapping branch).
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDims[] = {2};
    // (u=1, v=1) -> angle = 135 <= 180; (u=-1, v=1) -> angle = 225 > 180 ->
    // -135.
    constexpr double kU[] = {1.0, -1.0};
    constexpr double kV[] = {1.0, 1.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "zonal_wind_velocity",
                                  HARP_UNIT_SPEED, kTimeDim, kDims, kU));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "meridional_wind_velocity",
                                  HARP_UNIT_SPEED, kTimeDim, kDims, kV));

    ScopedHarpVariable wdir;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "wind_direction",
                                     HARP_UNIT_ANGLE, kTimeDim, &wdir));
    EXPECT_NEAR(135.0, wdir->data.double_data[0], 1e-9);
    EXPECT_NEAR(-135.0, wdir->data.double_data[1], 1e-9);

    ScopedHarpVariable wspd;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "wind_speed", HARP_UNIT_SPEED,
                                     kTimeDim, &wspd));
    EXPECT_NEAR(std::sqrt(2.0), wspd->data.double_data[0], 1e-12);
  }
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDims[] = {1};
    constexpr double kSpd[] = {10.0};
    constexpr double kDir[] = {90.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "wind_speed", HARP_UNIT_SPEED,
                                  kTimeDim, kDims, kSpd));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "wind_direction", HARP_UNIT_ANGLE,
                                  kTimeDim, kDims, kDir));

    ScopedHarpVariable u;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "zonal_wind_velocity",
                                     HARP_UNIT_SPEED, kTimeDim, &u));
    EXPECT_NEAR(10.0, u->data.double_data[0], 1e-9);

    ScopedHarpVariable v;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "meridional_wind_velocity",
                                     HARP_UNIT_SPEED, kTimeDim, &v));
    EXPECT_NEAR(0.0, v->data.double_data[0], 1e-9);
  }

  // 4. Gravity, surface_gravity, geopotential <-> gph <-> altitude,
  // pressure <-> altitude/gph profiles, and column_mass_density from surface_p.
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDimsT[] = {1};
    constexpr long kDimsTV[] = {1, 3};
    constexpr long kDimsTVB[] = {1, 3, 2};
    constexpr double kLat[] = {45.0};
    constexpr double kSurfAlt[] = {0.0};
    constexpr double kSurfPres[] = {101325.0};
    constexpr double kPres[] = {90000.0, 50000.0, 20000.0};
    constexpr double kPresB[] = {100000.0, 80000.0, 80000.0,
                                 40000.0,  40000.0, 10000.0};
    constexpr double kTemp[] = {280.0, 250.0, 220.0};
    constexpr double kMm[] = {28.96, 28.96, 28.96};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "latitude", HARP_UNIT_LATITUDE,
                                  kTimeDim, kDimsT, kLat));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "surface_altitude",
                                  HARP_UNIT_LENGTH, kTimeDim, kDimsT,
                                  kSurfAlt));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "surface_pressure",
                                  HARP_UNIT_PRESSURE, kTimeDim, kDimsT,
                                  kSurfPres));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "pressure", HARP_UNIT_PRESSURE,
                                  kTimeVertDim, kDimsTV, kPres));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "pressure_bounds",
                                  HARP_UNIT_PRESSURE, kTimeVertBoundsDim,
                                  kDimsTVB, kPresB));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "temperature",
                                  HARP_UNIT_TEMPERATURE, kTimeVertDim, kDimsTV,
                                  kTemp));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "molar_mass",
                                  HARP_UNIT_MOLAR_MASS, kTimeVertDim, kDimsTV,
                                  kMm));

    ScopedHarpVariable alt;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "altitude", HARP_UNIT_LENGTH,
                                     kTimeVertDim, &alt));
    EXPECT_GT(alt->data.double_data[2], alt->data.double_data[0]);

    ScopedHarpVariable gph;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "geopotential_height",
                                     HARP_UNIT_LENGTH, kTimeVertDim, &gph));
    EXPECT_GT(gph->data.double_data[2], gph->data.double_data[0]);

    ScopedHarpVariable grav_t;
    ASSERT_TRUE(DeriveDoubleVariable(
        prod.get(), "gravity", HARP_UNIT_ACCELERATION, kTimeDim, &grav_t));
    EXPECT_NEAR(9.806, grav_t->data.double_data[0], 0.01);

    ScopedHarpVariable sgrav;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "surface_gravity",
                                     HARP_UNIT_ACCELERATION, kTimeDim, &sgrav));
    EXPECT_NEAR(9.806, sgrav->data.double_data[0], 0.01);

    ScopedHarpVariable grav_tv;
    ASSERT_TRUE(DeriveDoubleVariable(
        prod.get(), "gravity", HARP_UNIT_ACCELERATION, kTimeVertDim, &grav_tv));
    EXPECT_LT(grav_tv->data.double_data[2], grav_tv->data.double_data[0]);

    ScopedHarpVariable col_dens;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "column_density",
                                     HARP_UNIT_COLUMN_MASS_DENSITY, kTimeDim,
                                     &col_dens));
    EXPECT_GT(col_dens->data.double_data[0], 0.0);
  }
  {
    // Inverse: pressure from altitude and geopotential from gph.
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDimsT[] = {1};
    constexpr long kDimsTV[] = {1, 3};
    constexpr double kLat[] = {45.0};
    constexpr double kSurfAlt[] = {0.0};
    constexpr double kSurfPres[] = {101325.0};
    constexpr double kAlt[] = {1000.0, 5000.0, 10000.0};
    constexpr double kTemp[] = {280.0, 250.0, 220.0};
    constexpr double kMm[] = {28.96, 28.96, 28.96};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "latitude", HARP_UNIT_LATITUDE,
                                  kTimeDim, kDimsT, kLat));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "surface_altitude",
                                  HARP_UNIT_LENGTH, kTimeDim, kDimsT,
                                  kSurfAlt));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "surface_pressure",
                                  HARP_UNIT_PRESSURE, kTimeDim, kDimsT,
                                  kSurfPres));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "altitude", HARP_UNIT_LENGTH,
                                  kTimeVertDim, kDimsTV, kAlt));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "temperature",
                                  HARP_UNIT_TEMPERATURE, kTimeVertDim, kDimsTV,
                                  kTemp));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "molar_mass",
                                  HARP_UNIT_MOLAR_MASS, kTimeVertDim, kDimsTV,
                                  kMm));

    ScopedHarpVariable pres;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "pressure", HARP_UNIT_PRESSURE,
                                     kTimeVertDim, &pres));
    EXPECT_LT(pres->data.double_data[2], pres->data.double_data[0]);

    ScopedHarpVariable gp;
    ASSERT_TRUE(DeriveDoubleVariable(
        prod.get(), "geopotential", HARP_UNIT_GEOPOTENTIAL, kTimeVertDim, &gp));
    EXPECT_GT(gp->data.double_data[2], gp->data.double_data[0]);
  }
  {
    // pressure from geopotential_height, and gph from geopotential.
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDimsT[] = {1};
    constexpr long kDimsTV[] = {1, 3};
    constexpr double kSurfAlt[] = {0.0};
    constexpr double kSurfPres[] = {101325.0};
    constexpr double kGph[] = {1000.0, 5000.0, 10000.0};
    constexpr double kTemp[] = {280.0, 250.0, 220.0};
    constexpr double kMm[] = {28.96, 28.96, 28.96};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "surface_altitude",
                                  HARP_UNIT_LENGTH, kTimeDim, kDimsT,
                                  kSurfAlt));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "surface_pressure",
                                  HARP_UNIT_PRESSURE, kTimeDim, kDimsT,
                                  kSurfPres));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "geopotential_height",
                                  HARP_UNIT_LENGTH, kTimeVertDim, kDimsTV,
                                  kGph));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "temperature",
                                  HARP_UNIT_TEMPERATURE, kTimeVertDim, kDimsTV,
                                  kTemp));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "molar_mass",
                                  HARP_UNIT_MOLAR_MASS, kTimeVertDim, kDimsTV,
                                  kMm));

    ScopedHarpVariable pres;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "pressure", HARP_UNIT_PRESSURE,
                                     kTimeVertDim, &pres));
    EXPECT_LT(pres->data.double_data[2], pres->data.double_data[0]);
  }
  {
    // gph from geopotential, and altitude from gph + latitude.
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDims[] = {1};
    constexpr double kGp[] = {9806.65};
    constexpr double kLat[] = {45.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "geopotential",
                                  HARP_UNIT_GEOPOTENTIAL, kTimeDim, kDims,
                                  kGp));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "latitude", HARP_UNIT_LATITUDE,
                                  kTimeDim, kDims, kLat));

    ScopedHarpVariable alt;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "altitude", HARP_UNIT_LENGTH,
                                     kTimeDim, &alt));
    EXPECT_NEAR(1000.0, alt->data.double_data[0], 5.0);
  }
}

TEST(HarpDerivedVariableListTest,
     SpeciesProfilesAvkDfsTropopauseAndUncertainty) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  constexpr harp_dimension_type kTimeDim[] = {harp_dimension_time};
  constexpr harp_dimension_type kTimeVertDim[] = {harp_dimension_time,
                                                  harp_dimension_vertical};
  constexpr harp_dimension_type kTimeVertBoundsDim[] = {
      harp_dimension_time, harp_dimension_vertical, harp_dimension_independent};
  constexpr harp_dimension_type kTimeAvkDim[] = {
      harp_dimension_time, harp_dimension_vertical, harp_dimension_vertical};

  // 1. Species conversions: partial column, density, vmr, mmr, dry_air,
  // partial pressure, AVK, DFS, covariance/uncertainty, tropopause &
  // stratospheric/tropospheric columns.
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDimsT[] = {1};
    constexpr long kDimsTV[] = {1, 6};
    constexpr long kDimsTVB[] = {1, 6, 2};
    constexpr long kDimsAvk[] = {1, 6, 6};
    constexpr double kLat[] = {45.0};
    constexpr double kAlt[] = {0.0, 4000.0, 8000.0, 12000.0, 16000.0, 22000.0};
    constexpr double kPres[] = {100000.0, 60000.0, 40000.0,
                                20000.0,  10000.0, 3000.0};
    constexpr double kPresB[] = {110000.0, 80000.0, 80000.0, 50000.0,
                                 50000.0,  30000.0, 30000.0, 15000.0,
                                 15000.0,  5000.0,  5000.0,  1000.0};
    // Tropopause occurs at index 3 (alt=12000m, pres=20000Pa).
    constexpr double kTemp[] = {288.0, 260.0, 230.0, 210.0, 210.0, 215.0};
    constexpr double kNd[] = {2.5e25, 1.7e25, 1.2e25, 7.0e24, 3.5e24, 1.0e24};
    constexpr double kMm[] = {28.96, 28.96, 28.96, 28.96, 28.96, 28.96};
    constexpr double kDryAirVmr[] = {0.99, 0.995, 0.999, 1.0, 1.0, 1.0};
    constexpr double kO3Vmr[] = {4e-8, 5e-8, 8e-8, 5e-7, 2e-6, 5e-6};
    constexpr double kO3Sys[] = {1e-9, 1e-9, 1e-9, 1e-9, 1e-9, 1e-9};
    constexpr double kO3Rnd[] = {2e-9, 2e-9, 2e-9, 2e-9, 2e-9, 2e-9};
    double kVmrAvk[36] = {};
    double kCov[36] = {};
    for (int i = 0; i < 6; ++i) {
      kVmrAvk[i * 6 + i] = 0.5;
      kCov[i * 6 + i] = 4.0e-18;
    }

    ASSERT_TRUE(AddDoubleVariable(prod.get(), "latitude", HARP_UNIT_LATITUDE,
                                  kTimeDim, kDimsT, kLat));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "altitude", HARP_UNIT_LENGTH,
                                  kTimeVertDim, kDimsTV, kAlt));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "pressure", HARP_UNIT_PRESSURE,
                                  kTimeVertDim, kDimsTV, kPres));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "pressure_bounds",
                                  HARP_UNIT_PRESSURE, kTimeVertBoundsDim,
                                  kDimsTVB, kPresB));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "temperature",
                                  HARP_UNIT_TEMPERATURE, kTimeVertDim, kDimsTV,
                                  kTemp));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "number_density",
                                  HARP_UNIT_NUMBER_DENSITY, kTimeVertDim,
                                  kDimsTV, kNd));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "molar_mass",
                                  HARP_UNIT_MOLAR_MASS, kTimeVertDim, kDimsTV,
                                  kMm));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "dry_air_volume_mixing_ratio",
                                  HARP_UNIT_VOLUME_MIXING_RATIO, kTimeVertDim,
                                  kDimsTV, kDryAirVmr));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "O3_volume_mixing_ratio",
                                  HARP_UNIT_VOLUME_MIXING_RATIO, kTimeVertDim,
                                  kDimsTV, kO3Vmr));
    ASSERT_TRUE(AddDoubleVariable(
        prod.get(), "O3_volume_mixing_ratio_uncertainty_systematic",
        HARP_UNIT_VOLUME_MIXING_RATIO, kTimeVertDim, kDimsTV, kO3Sys));
    ASSERT_TRUE(AddDoubleVariable(
        prod.get(), "O3_volume_mixing_ratio_uncertainty_random",
        HARP_UNIT_VOLUME_MIXING_RATIO, kTimeVertDim, kDimsTV, kO3Rnd));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "O3_volume_mixing_ratio_avk",
                                  HARP_UNIT_DIMENSIONLESS, kTimeAvkDim,
                                  kDimsAvk, kVmrAvk));
    ASSERT_TRUE(AddDoubleVariable(
        prod.get(), "O3_volume_mixing_ratio_covariance",
        HARP_UNIT_VOLUME_MIXING_RATIO_SQUARED, kTimeAvkDim, kDimsAvk, kCov));

    ScopedHarpVariable tropo_alt;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "tropopause_altitude",
                                     HARP_UNIT_LENGTH, kTimeDim, &tropo_alt));
    EXPECT_DOUBLE_EQ(12000.0, tropo_alt->data.double_data[0]);

    ScopedHarpVariable tropo_pres;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "tropopause_pressure",
                                     HARP_UNIT_PRESSURE, kTimeDim,
                                     &tropo_pres));
    EXPECT_DOUBLE_EQ(20000.0, tropo_pres->data.double_data[0]);

    ScopedHarpVariable o3_col;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "O3_column_number_density",
                                     HARP_UNIT_COLUMN_NUMBER_DENSITY, kTimeDim,
                                     &o3_col));
    EXPECT_GT(o3_col->data.double_data[0], 0.0);

    ScopedHarpVariable o3_col_dens;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "O3_column_density",
                                     HARP_UNIT_COLUMN_MASS_DENSITY, kTimeDim,
                                     &o3_col_dens));
    EXPECT_GT(o3_col_dens->data.double_data[0], 0.0);

    ScopedHarpVariable o3_strato;
    ASSERT_TRUE(DeriveDoubleVariable(
        prod.get(), "stratospheric_O3_column_number_density",
        HARP_UNIT_COLUMN_NUMBER_DENSITY, kTimeDim, &o3_strato));
    EXPECT_GT(o3_strato->data.double_data[0], 0.0);

    ScopedHarpVariable o3_tropo;
    ASSERT_TRUE(DeriveDoubleVariable(
        prod.get(), "tropospheric_O3_column_number_density",
        HARP_UNIT_COLUMN_NUMBER_DENSITY, kTimeDim, &o3_tropo));
    EXPECT_GT(o3_tropo->data.double_data[0], 0.0);

    ScopedHarpVariable o3_nd_avk;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "O3_number_density_avk",
                                     HARP_UNIT_DIMENSIONLESS, kTimeAvkDim,
                                     &o3_nd_avk));
    EXPECT_DOUBLE_EQ(0.5, o3_nd_avk->data.double_data[0]);

    ScopedHarpVariable o3_col_avk_2d;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "O3_column_number_density_avk",
                                     HARP_UNIT_DIMENSIONLESS, kTimeAvkDim,
                                     &o3_col_avk_2d));
    EXPECT_GT(o3_col_avk_2d->data.double_data[0], 0.0);

    ScopedHarpVariable o3_col_avk_1d;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "O3_column_number_density_avk",
                                     HARP_UNIT_DIMENSIONLESS, kTimeVertDim,
                                     &o3_col_avk_1d));
    EXPECT_GT(o3_col_avk_1d->data.double_data[0], 0.0);

    ScopedHarpVariable o3_strato_avk;
    ASSERT_TRUE(DeriveDoubleVariable(
        prod.get(), "stratospheric_O3_column_number_density_avk",
        HARP_UNIT_DIMENSIONLESS, kTimeVertDim, &o3_strato_avk));
    EXPECT_DOUBLE_EQ(0.0, o3_strato_avk->data.double_data[0]);

    ScopedHarpVariable o3_tropo_avk;
    ASSERT_TRUE(DeriveDoubleVariable(
        prod.get(), "tropospheric_O3_column_number_density_avk",
        HARP_UNIT_DIMENSIONLESS, kTimeVertDim, &o3_tropo_avk));
    EXPECT_GT(o3_tropo_avk->data.double_data[0], 0.0);

    ScopedHarpVariable o3_dfs;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "O3_volume_mixing_ratio_dfs",
                                     HARP_UNIT_DIMENSIONLESS, kTimeDim,
                                     &o3_dfs));
    EXPECT_NEAR(3.0, o3_dfs->data.double_data[0], 1e-12);

    ScopedHarpVariable o3_unc;
    ASSERT_TRUE(DeriveDoubleVariable(
        prod.get(), "O3_volume_mixing_ratio_uncertainty",
        HARP_UNIT_VOLUME_MIXING_RATIO, kTimeVertDim, &o3_unc));
    EXPECT_GT(o3_unc->data.double_data[0], 0.0);

    ScopedHarpVariable o3_pp;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "O3_partial_pressure",
                                     HARP_UNIT_PRESSURE, kTimeVertDim, &o3_pp));
    EXPECT_GT(o3_pp->data.double_data[0], 0.0);

    ScopedHarpVariable o3_mmr;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "O3_mass_mixing_ratio",
                                     HARP_UNIT_MASS_MIXING_RATIO, kTimeVertDim,
                                     &o3_mmr));
    EXPECT_GT(o3_mmr->data.double_data[0], 0.0);

    ScopedHarpVariable o3_vmr_dry;
    ASSERT_TRUE(DeriveDoubleVariable(
        prod.get(), "O3_volume_mixing_ratio_dry_air",
        HARP_UNIT_VOLUME_MIXING_RATIO, kTimeVertDim, &o3_vmr_dry));
    EXPECT_GT(o3_vmr_dry->data.double_data[0], 0.0);
  }

  // 2. Inverse species conversions:
  //    - get_vmr_from_nd, get_vmr_from_mmr, get_vmr_from_mmr_dry,
  //      get_vmr_from_partial_pressure_and_pressure, get_mr_from_mr_dry,
  //      get_mmr_from_vmr_dry, get_mmr_from_density, get_nd_from_density,
  //      get_vmr_avk_from_nd_avk, get_density_avk_from_partial_column_avk,
  //      get_sqrt_trace_from_matrix, get_strato/tropo_column_from_pressure.
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDimsTV[] = {1, 2};
    constexpr long kDimsTVB[] = {1, 2, 2};
    constexpr long kDimsAvk[] = {1, 2, 2};
    constexpr double kAltB[] = {0.0, 5000.0, 5000.0, 10000.0};
    constexpr double kNd[] = {2.5e25, 1.5e25};
    constexpr double kO3ColNd[] = {5.0e21, 5.0e21};
    constexpr double kO3MmrDry[] = {1.0e-6, 2.0e-6};
    constexpr double kO3ColAvk[] = {1.0, 0.0, 0.0, 1.0};
    constexpr double kO3NdAvk[] = {1.0, 0.0, 0.0, 1.0};
    constexpr double kO3Cov[] = {4.0, 0.0, 0.0, 9.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "altitude_bounds",
                                  HARP_UNIT_LENGTH, kTimeVertBoundsDim,
                                  kDimsTVB, kAltB));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "number_density",
                                  HARP_UNIT_NUMBER_DENSITY, kTimeVertDim,
                                  kDimsTV, kNd));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "O3_column_number_density",
                                  HARP_UNIT_COLUMN_NUMBER_DENSITY, kTimeVertDim,
                                  kDimsTV, kO3ColNd));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "O3_mass_mixing_ratio_dry_air",
                                  HARP_UNIT_MASS_MIXING_RATIO, kTimeVertDim,
                                  kDimsTV, kO3MmrDry));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "O3_column_number_density_avk",
                                  HARP_UNIT_DIMENSIONLESS, kTimeAvkDim,
                                  kDimsAvk, kO3ColAvk));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "O3_number_density_avk",
                                  HARP_UNIT_DIMENSIONLESS, kTimeAvkDim,
                                  kDimsAvk, kO3NdAvk));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "O3_number_density_covariance",
                                  HARP_UNIT_NUMBER_DENSITY_SQUARED, kTimeAvkDim,
                                  kDimsAvk, kO3Cov));

    ScopedHarpVariable o3_vmr_dry;
    ASSERT_TRUE(DeriveDoubleVariable(
        prod.get(), "O3_volume_mixing_ratio_dry_air",
        HARP_UNIT_VOLUME_MIXING_RATIO, kTimeVertDim, &o3_vmr_dry));
    EXPECT_GT(o3_vmr_dry->data.double_data[0], 0.0);

    ScopedHarpVariable o3_vmr;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "O3_volume_mixing_ratio",
                                     HARP_UNIT_VOLUME_MIXING_RATIO,
                                     kTimeVertDim, &o3_vmr));
    EXPECT_GT(o3_vmr->data.double_data[0], 0.0);

    ScopedHarpVariable o3_nd_unc;
    ASSERT_TRUE(DeriveDoubleVariable(
        prod.get(), "O3_number_density_uncertainty", HARP_UNIT_NUMBER_DENSITY,
        kTimeVertDim, &o3_nd_unc));
    EXPECT_DOUBLE_EQ(2.0, o3_nd_unc->data.double_data[0]);
    EXPECT_DOUBLE_EQ(3.0, o3_nd_unc->data.double_data[1]);

    ScopedHarpVariable o3_vmr_avk;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "O3_volume_mixing_ratio_avk",
                                     HARP_UNIT_DIMENSIONLESS, kTimeAvkDim,
                                     &o3_vmr_avk));
    EXPECT_DOUBLE_EQ(1.0, o3_vmr_avk->data.double_data[0]);
  }
  {
    // get_density_avk_from_partial_column_avk_and_alt_bounds +
    // get_strato/tropo_column_from_partial_column_and_pressure +
    // get_vmr_from_partial_pressure_and_pressure + get_vmr_from_mmr.
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDimsT[] = {1};
    constexpr long kDimsTV[] = {1, 2};
    constexpr long kDimsTVB[] = {1, 2, 2};
    constexpr long kDimsAvk[] = {1, 2, 2};
    constexpr double kAltB[] = {0.0, 5000.0, 5000.0, 10000.0};
    constexpr double kPresB[] = {100000.0, 50000.0, 50000.0, 10000.0};
    constexpr double kPres[] = {75000.0, 30000.0};
    constexpr double kTropPres[] = {40000.0};
    constexpr double kO3ColNd[] = {5.0e21, 5.0e21};
    constexpr double kO3ColAvk[] = {1.0, 0.0, 0.0, 1.0};
    constexpr double kO3Pp[] = {0.005, 0.01};
    constexpr double kO3Density[] = {1.0e-4, 5.0e-5};
    constexpr double kAirDensity[] = {1.0, 0.5};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "altitude_bounds",
                                  HARP_UNIT_LENGTH, kTimeVertBoundsDim,
                                  kDimsTVB, kAltB));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "pressure_bounds",
                                  HARP_UNIT_PRESSURE, kTimeVertBoundsDim,
                                  kDimsTVB, kPresB));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "pressure", HARP_UNIT_PRESSURE,
                                  kTimeVertDim, kDimsTV, kPres));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "tropopause_pressure",
                                  HARP_UNIT_PRESSURE, kTimeDim, kDimsT,
                                  kTropPres));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "O3_column_number_density",
                                  HARP_UNIT_COLUMN_NUMBER_DENSITY, kTimeVertDim,
                                  kDimsTV, kO3ColNd));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "O3_column_number_density_avk",
                                  HARP_UNIT_DIMENSIONLESS, kTimeAvkDim,
                                  kDimsAvk, kO3ColAvk));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "O3_partial_pressure",
                                  HARP_UNIT_PRESSURE, kTimeVertDim, kDimsTV,
                                  kO3Pp));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "O3_density",
                                  HARP_UNIT_MASS_DENSITY, kTimeVertDim, kDimsTV,
                                  kO3Density));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "density", HARP_UNIT_MASS_DENSITY,
                                  kTimeVertDim, kDimsTV, kAirDensity));

    ScopedHarpVariable o3_nd_avk;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "O3_number_density_avk",
                                     HARP_UNIT_DIMENSIONLESS, kTimeAvkDim,
                                     &o3_nd_avk));
    EXPECT_DOUBLE_EQ(1.0, o3_nd_avk->data.double_data[0]);

    ScopedHarpVariable strato;
    ASSERT_TRUE(DeriveDoubleVariable(
        prod.get(), "stratospheric_O3_column_number_density",
        HARP_UNIT_COLUMN_NUMBER_DENSITY, kTimeDim, &strato));
    EXPECT_GT(strato->data.double_data[0], 0.0);

    ScopedHarpVariable tropo;
    ASSERT_TRUE(DeriveDoubleVariable(
        prod.get(), "tropospheric_O3_column_number_density",
        HARP_UNIT_COLUMN_NUMBER_DENSITY, kTimeDim, &tropo));
    EXPECT_GT(tropo->data.double_data[0], 0.0);

    ScopedHarpVariable o3_vmr;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "O3_volume_mixing_ratio",
                                     HARP_UNIT_VOLUME_MIXING_RATIO,
                                     kTimeVertDim, &o3_vmr));
    EXPECT_GT(o3_vmr->data.double_data[0], 0.0);

    ScopedHarpVariable o3_mmr;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "O3_mass_mixing_ratio",
                                     HARP_UNIT_MASS_MIXING_RATIO, kTimeVertDim,
                                     &o3_mmr));
    EXPECT_DOUBLE_EQ(1.0e-4, o3_mmr->data.double_data[0]);

    ScopedHarpVariable o3_nd;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "O3_number_density",
                                     HARP_UNIT_NUMBER_DENSITY, kTimeVertDim,
                                     &o3_nd));
    EXPECT_GT(o3_nd->data.double_data[0], 0.0);
  }
  {
    // get_copy for O3_column_volume_mixing_ratio {time,vertical} <->
    // O3_volume_mixing_ratio {time,vertical}, plus get_mmr_from_vmr_dry and
    // get_vmr_from_mmr.
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDimsTV[] = {1, 2};
    constexpr double kO3VmrDry[] = {1.0e-6, 2.0e-6};
    constexpr double kO3Mmr[] = {1.5e-6, 3.0e-6};
    constexpr double kMm[] = {28.96, 28.96};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "O3_volume_mixing_ratio_dry_air",
                                  HARP_UNIT_VOLUME_MIXING_RATIO, kTimeVertDim,
                                  kDimsTV, kO3VmrDry));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "O3_mass_mixing_ratio",
                                  HARP_UNIT_MASS_MIXING_RATIO, kTimeVertDim,
                                  kDimsTV, kO3Mmr));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "molar_mass",
                                  HARP_UNIT_MOLAR_MASS, kTimeVertDim, kDimsTV,
                                  kMm));

    ScopedHarpVariable o3_mmr_dry;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "O3_mass_mixing_ratio_dry_air",
                                     HARP_UNIT_MASS_MIXING_RATIO, kTimeVertDim,
                                     &o3_mmr_dry));
    EXPECT_GT(o3_mmr_dry->data.double_data[0], 0.0);

    ScopedHarpVariable o3_vmr;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "O3_volume_mixing_ratio",
                                     HARP_UNIT_VOLUME_MIXING_RATIO,
                                     kTimeVertDim, &o3_vmr));
    EXPECT_GT(o3_vmr->data.double_data[0], 0.0);

    ScopedHarpVariable o3_col_vmr;
    ASSERT_TRUE(DeriveDoubleVariable(
        prod.get(), "O3_column_volume_mixing_ratio",
        HARP_UNIT_VOLUME_MIXING_RATIO, kTimeVertDim, &o3_col_vmr));
    EXPECT_DOUBLE_EQ(o3_vmr->data.double_data[0],
                     o3_col_vmr->data.double_data[0]);
  }
  {
    // Tropopause not found (constant temperature -> index < 0 -> NaN).
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDimsTV[] = {1, 4};
    constexpr double kAlt[] = {0.0, 5000.0, 10000.0, 15000.0};
    constexpr double kPres[] = {100000.0, 50000.0, 25000.0, 12000.0};
    constexpr double kConstTemp[] = {250.0, 250.0, 250.0, 250.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "altitude", HARP_UNIT_LENGTH,
                                  kTimeVertDim, kDimsTV, kAlt));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "pressure", HARP_UNIT_PRESSURE,
                                  kTimeVertDim, kDimsTV, kPres));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "temperature",
                                  HARP_UNIT_TEMPERATURE, kTimeVertDim, kDimsTV,
                                  kConstTemp));

    ScopedHarpVariable tropo_alt;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "tropopause_altitude",
                                     HARP_UNIT_LENGTH, kTimeDim, &tropo_alt));
    EXPECT_TRUE(std::isnan(tropo_alt->data.double_data[0]));

    ScopedHarpVariable tropo_pres;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "tropopause_pressure",
                                     HARP_UNIT_PRESSURE, kTimeDim,
                                     &tropo_pres));
    EXPECT_TRUE(std::isnan(tropo_pres->data.double_data[0]));
  }
}

TEST(HarpDerivedVariableListTest, AuxiliaryClimatologyAfgl86AndUsstd76) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const int saved_afgl86 = harp_get_option_enable_aux_afgl86();
  const int saved_usstd76 = harp_get_option_enable_aux_usstd76();
  auto restore_opts = absl::MakeCleanup([&] {
    harp_set_option_enable_aux_afgl86(saved_afgl86);
    harp_set_option_enable_aux_usstd76(saved_usstd76);
  });

  constexpr harp_dimension_type kTimeDim[] = {harp_dimension_time};
  constexpr harp_dimension_type kTimeVertDim[] = {harp_dimension_time,
                                                  harp_dimension_vertical};

  // 1. AFGL86 climatology.
  {
    ASSERT_EQ(HARP_SUCCESS, harp_set_option_enable_aux_afgl86(1));
    ASSERT_EQ(HARP_SUCCESS, harp_set_option_enable_aux_usstd76(0));

    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDimsT[] = {3};
    constexpr long kDimsTV[] = {3, 2};
    constexpr double kDt[] = {0.0, 86400.0 * 180.0, 86400.0 * 180.0};
    constexpr double kLat[] = {10.0, 45.0, -85.0};
    constexpr double kAlt[] = {0.0, 10000.0, 0.0, 10000.0, 0.0, 10000.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "datetime", HARP_UNIT_DATETIME,
                                  kTimeDim, kDimsT, kDt));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "latitude", HARP_UNIT_LATITUDE,
                                  kTimeDim, kDimsT, kLat));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "altitude", HARP_UNIT_LENGTH,
                                  kTimeVertDim, kDimsTV, kAlt));

    ScopedHarpVariable pres;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "pressure", HARP_UNIT_PRESSURE,
                                     kTimeVertDim, &pres));
    EXPECT_GT(pres->data.double_data[0], 90000.0);

    ScopedHarpVariable temp;
    ASSERT_TRUE(DeriveDoubleVariable(
        prod.get(), "temperature", HARP_UNIT_TEMPERATURE, kTimeVertDim, &temp));
    EXPECT_GT(temp->data.double_data[0], 200.0);

    ScopedHarpVariable o3_nd;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "O3_number_density",
                                     HARP_UNIT_NUMBER_DENSITY, kTimeVertDim,
                                     &o3_nd));
    EXPECT_GT(o3_nd->data.double_data[0], 0.0);
  }

  // 2. US Standard 1976 climatology.
  {
    ASSERT_EQ(HARP_SUCCESS, harp_set_option_enable_aux_afgl86(0));
    ASSERT_EQ(HARP_SUCCESS, harp_set_option_enable_aux_usstd76(1));

    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDimsTV[] = {1, 2};
    constexpr double kAlt[] = {0.0, 10000.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "altitude", HARP_UNIT_LENGTH,
                                  kTimeVertDim, kDimsTV, kAlt));

    ScopedHarpVariable pres;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "pressure", HARP_UNIT_PRESSURE,
                                     kTimeVertDim, &pres));
    EXPECT_NEAR(101300.0, pres->data.double_data[0], 50.0);

    ScopedHarpVariable temp;
    ASSERT_TRUE(DeriveDoubleVariable(
        prod.get(), "temperature", HARP_UNIT_TEMPERATURE, kTimeVertDim, &temp));
    EXPECT_NEAR(288.15, temp->data.double_data[0], 1.0);

    ScopedHarpVariable nd;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "number_density",
                                     HARP_UNIT_NUMBER_DENSITY, kTimeVertDim,
                                     &nd));
    EXPECT_GT(nd->data.double_data[0], 0.0);
  }
}

TEST(HarpDerivedVariableListTest, RemainingConversionFunctions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  constexpr harp_dimension_type kTimeDim[] = {harp_dimension_time};
  constexpr harp_dimension_type kTimeVertDim[] = {harp_dimension_time,
                                                  harp_dimension_vertical};
  constexpr harp_dimension_type kTimeVertBoundsDim[] = {
      harp_dimension_time, harp_dimension_vertical, harp_dimension_independent};

  // 1. get_air_nd_from_species_nd_and_vmr.
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDims[] = {1};
    constexpr double kO3Col[] = {8.0e22};
    constexpr double kO3Vmr[] = {8.0e-7};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "O3_column_number_density",
                                  HARP_UNIT_COLUMN_NUMBER_DENSITY, kTimeDim,
                                  kDims, kO3Col));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "O3_column_volume_mixing_ratio",
                                  HARP_UNIT_VOLUME_MIXING_RATIO, kTimeDim,
                                  kDims, kO3Vmr));

    ScopedHarpVariable col_nd;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "column_number_density",
                                     HARP_UNIT_COLUMN_NUMBER_DENSITY, kTimeDim,
                                     &col_nd));
    EXPECT_NEAR(1.0e29, col_nd->data.double_data[0], 1e24);
  }

  // 2. get_bounds_from_midpoints_log and get_midpoint_from_bounds_log.
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDimsTV[] = {1, 3};
    constexpr double kPres[] = {100000.0, 50000.0, 25000.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "pressure", HARP_UNIT_PRESSURE,
                                  kTimeVertDim, kDimsTV, kPres));

    ScopedHarpVariable pb;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "pressure_bounds",
                                     HARP_UNIT_PRESSURE, kTimeVertBoundsDim,
                                     &pb));
    EXPECT_GT(pb->data.double_data[0], pb->data.double_data[1]);
  }
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDimsTVB[] = {1, 2, 2};
    constexpr double kPresB[] = {100000.0, 50000.0, 50000.0, 25000.0};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "pressure_bounds",
                                  HARP_UNIT_PRESSURE, kTimeVertBoundsDim,
                                  kDimsTVB, kPresB));

    ScopedHarpVariable pres;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "pressure", HARP_UNIT_PRESSURE,
                                     kTimeVertDim, &pres));
    EXPECT_NEAR(std::sqrt(100000.0 * 50000.0), pres->data.double_data[0], 1e-6);
  }

  // 3. get_density_from_nd_for_air and get_nd_from_density_for_air.
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDims[] = {1};
    constexpr double kNd[] = {2.5e25};
    constexpr double kMm[] = {28.96};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "number_density",
                                  HARP_UNIT_NUMBER_DENSITY, kTimeDim, kDims,
                                  kNd));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "molar_mass",
                                  HARP_UNIT_MOLAR_MASS, kTimeDim, kDims, kMm));

    ScopedHarpVariable dens;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "density",
                                     HARP_UNIT_MASS_DENSITY, kTimeDim, &dens));
    EXPECT_GT(dens->data.double_data[0], 1.0);
  }
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDims[] = {1};
    constexpr double kDens[] = {1.2};
    constexpr double kMm[] = {28.96};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "density", HARP_UNIT_MASS_DENSITY,
                                  kTimeDim, kDims, kDens));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "molar_mass",
                                  HARP_UNIT_MOLAR_MASS, kTimeDim, kDims, kMm));

    ScopedHarpVariable nd;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "number_density",
                                     HARP_UNIT_NUMBER_DENSITY, kTimeDim, &nd));
    EXPECT_GT(nd->data.double_data[0], 1e25);
  }

  // 4. get_dry_air_from_air_and_h2o.
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDims[] = {1};
    constexpr double kNd[] = {2.5e25};
    constexpr double kH2oNd[] = {5.0e23};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "number_density",
                                  HARP_UNIT_NUMBER_DENSITY, kTimeDim, kDims,
                                  kNd));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "H2O_number_density",
                                  HARP_UNIT_NUMBER_DENSITY, kTimeDim, kDims,
                                  kH2oNd));

    ScopedHarpVariable dry_nd;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "dry_air_number_density",
                                     HARP_UNIT_NUMBER_DENSITY, kTimeDim,
                                     &dry_nd));
    EXPECT_DOUBLE_EQ(2.45e25, dry_nd->data.double_data[0]);
  }

  // 5. get_mr_from_mr_dry_and_dry_air_mr and get_nd_from_vmr.
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDims[] = {1};
    constexpr double kO3VmrDry[] = {1.0e-6};
    constexpr double kDryVmr[] = {0.99};
    constexpr double kNd[] = {2.5e25};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "O3_volume_mixing_ratio_dry_air",
                                  HARP_UNIT_VOLUME_MIXING_RATIO, kTimeDim,
                                  kDims, kO3VmrDry));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "dry_air_volume_mixing_ratio",
                                  HARP_UNIT_VOLUME_MIXING_RATIO, kTimeDim,
                                  kDims, kDryVmr));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "number_density",
                                  HARP_UNIT_NUMBER_DENSITY, kTimeDim, kDims,
                                  kNd));

    ScopedHarpVariable o3_vmr;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "O3_volume_mixing_ratio",
                                     HARP_UNIT_VOLUME_MIXING_RATIO, kTimeDim,
                                     &o3_vmr));
    EXPECT_DOUBLE_EQ(0.99e-6, o3_vmr->data.double_data[0]);

    ScopedHarpVariable o3_nd;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "O3_number_density",
                                     HARP_UNIT_NUMBER_DENSITY, kTimeDim,
                                     &o3_nd));
    EXPECT_NEAR(0.99e-6 * 2.5e25, o3_nd->data.double_data[0], 1e12);
  }

  // 6. get_partial_column_from_density_and_alt_bounds,
  //    get_strato_column_from_partial_column_and_altitude, and
  //    get_tropo_column_from_partial_column_and_altitude.
  {
    ScopedHarpProduct prod;
    ASSERT_TRUE(CreateProduct(&prod));
    constexpr long kDimsT[] = {1};
    constexpr long kDimsTV[] = {1, 3};
    constexpr long kDimsTVB[] = {1, 3, 2};
    constexpr double kAltB[] = {0.0, 5000.0, 5000.0, 12000.0, 12000.0, 20000.0};
    constexpr double kTropAlt[] = {10000.0};
    constexpr double kO3Nd[] = {1.0e18, 2.0e18, 3.0e18};
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "altitude_bounds",
                                  HARP_UNIT_LENGTH, kTimeVertBoundsDim,
                                  kDimsTVB, kAltB));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "tropopause_altitude",
                                  HARP_UNIT_LENGTH, kTimeDim, kDimsT,
                                  kTropAlt));
    ASSERT_TRUE(AddDoubleVariable(prod.get(), "O3_number_density",
                                  HARP_UNIT_NUMBER_DENSITY, kTimeVertDim,
                                  kDimsTV, kO3Nd));

    ScopedHarpVariable pcol;
    ASSERT_TRUE(DeriveDoubleVariable(prod.get(), "O3_column_number_density",
                                     HARP_UNIT_COLUMN_NUMBER_DENSITY,
                                     kTimeVertDim, &pcol));
    EXPECT_DOUBLE_EQ(5000.0 * 1.0e18, pcol->data.double_data[0]);

    ScopedHarpVariable strato;
    ASSERT_TRUE(DeriveDoubleVariable(
        prod.get(), "stratospheric_O3_column_number_density",
        HARP_UNIT_COLUMN_NUMBER_DENSITY, kTimeDim, &strato));
    EXPECT_GT(strato->data.double_data[0], 0.0);

    ScopedHarpVariable tropo;
    ASSERT_TRUE(DeriveDoubleVariable(
        prod.get(), "tropospheric_O3_column_number_density",
        HARP_UNIT_COLUMN_NUMBER_DENSITY, kTimeDim, &tropo));
    EXPECT_GT(tropo->data.double_data[0], 0.0);
  }
}

TEST(HarpDerivedVariableListTest, OutOfMemoryInDerivedVariableListInit) {
  // Ensure harp_init_counter starts at 0 so harp_derived_variable_conversions
  // is NULL before exercising OOM cleanup across registration paths.
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  harp_derived_variable_list_done();

  const absl::Cleanup restore = [] {
    harp_set_error(HARP_SUCCESS, nullptr);
    if (harp_derived_variable_conversions == nullptr) {
      harp_derived_variable_list_init();
    }
  };

  int oom_triggered = 0;
  // Sweep early hashtable/list allocations, then sample later conversion paths.
  // Because init_conversions performs thousands of small allocations, sweeping
  // every 7th step after step 60 covers every OOM guard variant in
  // register_* and harp_variable_conversion_new in < 0.5s.
  for (int n = 1; n <= 60; ++n) {
    harp_set_error(HARP_SUCCESS, nullptr);
    ScopedOomCountdown guard(n);
    if (harp_derived_variable_list_init() == HARP_SUCCESS) {
      ScopedSuppressOom suppress;
      harp_derived_variable_list_done();
      break;
    }
    EXPECT_EQ(HARP_ERROR_OUT_OF_MEMORY, harp_errno);
    if (!guard.triggered()) {
      break;
    }
    ++oom_triggered;
  }
  for (int n = 61; n <= 12000; n += 7) {
    harp_set_error(HARP_SUCCESS, nullptr);
    ScopedOomCountdown guard(n);
    if (harp_derived_variable_list_init() == HARP_SUCCESS) {
      ScopedSuppressOom suppress;
      harp_derived_variable_list_done();
      break;
    }
    if (!guard.triggered()) {
      break;
    }
    ++oom_triggered;
  }
  EXPECT_GT(oom_triggered, 1000);
}

}  // namespace
}  // namespace stcorp_harp
