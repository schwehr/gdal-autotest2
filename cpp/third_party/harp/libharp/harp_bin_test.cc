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
// Unit tests for HARP product time binning (libharp/harp-bin.c).
//
// Google3 build notes: HDF4 is disabled (#undef HAVE_HDF4); udunits2 XML is
// embedded via //third_party/udunits.
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-bin.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp.h.in

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/oom_fault_inject.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::DoubleNear;
using ::testing::ElementsAre;
using ::testing::FloatNear;
using ::testing::HasSubstr;

harp_variable* AddVariableOrFail(harp_product* product, const char* name,
                                 harp_data_type data_type, int num_dimensions,
                                 const harp_dimension_type* dimension_type,
                                 const long* dimension,
                                 const char* unit = nullptr) {
  harp_variable* raw = nullptr;
  if (harp_variable_new(name, data_type, num_dimensions, dimension_type,
                        dimension, &raw) != HARP_SUCCESS) {
    ADD_FAILURE() << "harp_variable_new failed for " << name << ": "
                  << harp_errno_to_string(harp_errno);
    return nullptr;
  }
  ScopedHarpVariable guard(raw);
  if (unit != nullptr) {
    if (harp_variable_set_unit(raw, unit) != HARP_SUCCESS) {
      ADD_FAILURE() << "harp_variable_set_unit failed for " << name << ": "
                    << harp_errno_to_string(harp_errno);
      return nullptr;
    }
  }
  if (harp_product_add_variable(product, raw) != HARP_SUCCESS) {
    ADD_FAILURE() << "harp_product_add_variable failed for " << name << ": "
                  << harp_errno_to_string(harp_errno);
    return nullptr;
  }
  guard.release();
  return raw;
}

TEST(HarpBinTest, BinInvalidArgumentsReturnError) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  constexpr harp_dimension_type kDims[] = {harp_dimension_time};
  constexpr long kLen[] = {3};
  harp_variable* var = AddVariableOrFail(prod.get(), "temperature",
                                         harp_type_double, 1, kDims, kLen, "K");
  ASSERT_NE(nullptr, var);
  var->data.double_data[0] = 280.0;
  var->data.double_data[1] = 290.0;
  var->data.double_data[2] = 300.0;

  // 1. Mismatched num_elements vs time dimension length.
  long bad_len_bins[] = {0, 0};
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_product_bin(prod.get(), 1, 2, bad_len_bins));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_THAT(harp_errno_to_string(harp_errno),
              HasSubstr("does not match time dimension length"));

  // 2. Negative bin_index.
  long negative_bins[] = {0, -1, 0};
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_product_bin(prod.get(), 1, 3, negative_bins));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  // 3. bin_index >= num_bins.
  long overflow_bins[] = {0, 1, 2};
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_product_bin(prod.get(), 2, 3, overflow_bins));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  harp_set_error(HARP_SUCCESS, nullptr);
}

TEST(HarpBinTest, BinClassifiesRemovesSkipsAndAveragesVariables) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  constexpr harp_dimension_type kTimeDim[] = {harp_dimension_time};
  constexpr long kTimeLen[] = {4};
  constexpr harp_dimension_type kVertDim[] = {harp_dimension_vertical};
  constexpr long kVertLen[] = {2};
  constexpr harp_dimension_type kAreaDim[] = {harp_dimension_time,
                                              harp_dimension_independent};
  constexpr long kAreaLen[] = {4, 4};
  constexpr harp_dimension_type kEdgeDim[] = {harp_dimension_time,
                                              harp_dimension_independent};
  constexpr long kEdgeLen[] = {4, 2};
  constexpr harp_dimension_type kTimeVertDim[] = {harp_dimension_time,
                                                  harp_dimension_vertical};
  constexpr long kTimeVertLen[] = {4, 2};

  // Scalar and non-time variables -> binning_skip (preserved unchanged).
  harp_variable* scalar = AddVariableOrFail(
      prod.get(), "scalar_meta", harp_type_double, 0, nullptr, nullptr, "m");
  ASSERT_NE(nullptr, scalar);
  scalar->data.double_data[0] = 42.0;

  harp_variable* vert_axis = AddVariableOrFail(
      prod.get(), "altitude", harp_type_double, 1, kVertDim, kVertLen, "m");
  ASSERT_NE(nullptr, vert_axis);
  vert_axis->data.double_data[0] = 100.0;
  vert_axis->data.double_data[1] = 200.0;

  // Variables that must be removed during binning:
  // - enum variable
  harp_variable* enum_var = AddVariableOrFail(
      prod.get(), "quality_flag", harp_type_int8, 1, kTimeDim, kTimeLen, "1");
  ASSERT_NE(nullptr, enum_var);
  const char* enum_names[] = {"good", "bad"};
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_enumeration_values(enum_var, 2, enum_names));

  // - string variable
  harp_variable* str_var = AddVariableOrFail(
      prod.get(), "source_id", harp_type_string, 1, kTimeDim, kTimeLen);
  ASSERT_NE(nullptr, str_var);
  for (long i = 0; i < 4; ++i) {
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_set_string_data_element(str_var, i, "s"));
  }

  // - unitless non-count/non-weight time variable
  ASSERT_NE(nullptr, AddVariableOrFail(prod.get(), "unitless_index",
                                       harp_type_int32, 1, kTimeDim, kTimeLen));

  // - averaging kernel (_avk)
  ASSERT_NE(nullptr, AddVariableOrFail(prod.get(), "O3_avk", harp_type_double,
                                       1, kTimeDim, kTimeLen, "1"));

  // - area bounds (last dim independent with length > 2)
  ASSERT_NE(nullptr,
            AddVariableOrFail(prod.get(), "latitude_bounds", harp_type_double,
                              2, kAreaDim, kAreaLen, "degree_north"));
  ASSERT_NE(nullptr,
            AddVariableOrFail(prod.get(), "longitude_bounds", harp_type_double,
                              2, kAreaDim, kAreaLen, "degree_east"));

  // - invalid *_count (wrong type, has unit, non-1D "count")
  ASSERT_NE(nullptr,
            AddVariableOrFail(prod.get(), "bad_type_count", harp_type_double, 1,
                              kTimeDim, kTimeLen));
  ASSERT_NE(nullptr,
            AddVariableOrFail(prod.get(), "bad_unit_count", harp_type_int32, 1,
                              kTimeDim, kTimeLen, "1"));
  ASSERT_NE(nullptr, AddVariableOrFail(prod.get(), "count", harp_type_int32, 2,
                                       kTimeVertDim, kTimeVertLen));

  // - invalid *_weight (wrong type, has unit)
  ASSERT_NE(nullptr,
            AddVariableOrFail(prod.get(), "bad_type_weight", harp_type_double,
                              1, kTimeDim, kTimeLen));
  ASSERT_NE(nullptr,
            AddVariableOrFail(prod.get(), "bad_unit_weight", harp_type_float, 1,
                              kTimeDim, kTimeLen, "1"));

  // - variable with zero-length independent sub-dimension (hits
  //   variable->dimension[i] == harp_dimension_time == 0 in get_binning_type,
  //   without violating product dimension invariants on removal).
  constexpr long kZeroIndepLen[] = {4, 0};
  ASSERT_NE(nullptr,
            AddVariableOrFail(prod.get(), "zero_subdim", harp_type_double, 2,
                              kEdgeDim, kZeroIndepLen, "1"));

  // Valid edge bounds (last dim length 2) -> kept & binned as angle!
  harp_variable* lon_edge =
      AddVariableOrFail(prod.get(), "sensor_longitude_bounds", harp_type_double,
                        2, kEdgeDim, kEdgeLen, "degree_east");
  ASSERT_NE(nullptr, lon_edge);
  // Also test actual "longitude_bounds" with independent length 2 (not > 2).
  harp_variable* bounds_2 =
      AddVariableOrFail(prod.get(), "longitude_bounds_edge", harp_type_double,
                        2, kEdgeDim, kEdgeLen, "degree_east");
  ASSERT_NE(nullptr, bounds_2);
  for (long i = 0; i < 8; ++i) {
    lon_edge->data.double_data[i] = 10.0;
    bounds_2->data.double_data[i] = 20.0;
  }

  // datetime_start (binning_time_min) & datetime_stop (binning_time_max)
  // including NaNs at both target_index and non-target_index.
  const double nan = std::numeric_limits<double>::quiet_NaN();
  harp_variable* dt_start =
      AddVariableOrFail(prod.get(), "datetime_start", harp_type_double, 1,
                        kTimeDim, kTimeLen, "s");
  ASSERT_NE(nullptr, dt_start);
  dt_start->data.double_data[0] = nan;
  dt_start->data.double_data[1] = 15.0;
  dt_start->data.double_data[2] = 5.0;
  dt_start->data.double_data[3] = nan;

  harp_variable* dt_stop =
      AddVariableOrFail(prod.get(), "datetime_stop", harp_type_double, 1,
                        kTimeDim, kTimeLen, "s");
  ASSERT_NE(nullptr, dt_stop);
  dt_stop->data.double_data[0] = nan;
  dt_stop->data.double_data[1] = 25.0;
  dt_stop->data.double_data[2] = 35.0;
  dt_stop->data.double_data[3] = nan;

  // Regular int16 variable converted to double and averaged.
  harp_variable* temp = AddVariableOrFail(
      prod.get(), "temperature", harp_type_int16, 1, kTimeDim, kTimeLen, "K");
  ASSERT_NE(nullptr, temp);
  temp->data.int16_data[0] = 200;
  temp->data.int16_data[1] = 220;
  temp->data.int16_data[2] = 240;
  temp->data.int16_data[3] = 280;

  // 2D angle variable ("direction") with wrap-around across 0/360 degrees.
  harp_variable* wind_dir =
      AddVariableOrFail(prod.get(), "wind_direction", harp_type_double, 1,
                        kTimeDim, kTimeLen, "degree");
  ASSERT_NE(nullptr, wind_dir);
  wind_dir->data.double_data[0] = 350.0;
  wind_dir->data.double_data[1] = 10.0;
  wind_dir->data.double_data[2] = 90.0;
  wind_dir->data.double_data[3] = nan;

  // Bin 4 samples into 4 bins where bins 1 and 3 are empty: {0, 0, 0, 2}.
  // Using num_bins == num_elements ensures empty-bin clearing works cleanly
  // (in upstream harp-bin.c, num_bins < num_elements truncates
  // num_elements/num_elements to 0 and num_bins > num_elements over-indexes).
  long bin_index[] = {0, 0, 0, 2};
  ASSERT_EQ(HARP_SUCCESS, harp_product_bin(prod.get(), 4, 4, bin_index));

  EXPECT_EQ(4L, prod->dimension[harp_dimension_time]);
  EXPECT_EQ(1, harp_product_has_variable(prod.get(), "scalar_meta"));
  EXPECT_EQ(1, harp_product_has_variable(prod.get(), "altitude"));
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "quality_flag"));
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "source_id"));
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "unitless_index"));
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "O3_avk"));
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "latitude_bounds"));
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "longitude_bounds"));
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "bad_type_count"));
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "bad_unit_count"));
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "bad_type_weight"));
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "bad_unit_weight"));
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "zero_subdim"));

  // Invalid 2D "count" should have been replaced by a valid 1D int32 "count".
  harp_variable* out_count = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_get_variable_by_name(prod.get(), "count", &out_count));
  EXPECT_EQ(harp_type_int32, out_count->data_type);
  EXPECT_EQ(1, out_count->num_dimensions);
  EXPECT_EQ(3, out_count->data.int32_data[0]);
  EXPECT_EQ(0, out_count->data.int32_data[1]);
  EXPECT_EQ(1, out_count->data.int32_data[2]);

  // datetime_start / datetime_stop: bin 0 = min/max of {nan, 15, 5}, bin 1 =
  // nan, bin 2 = nan.
  EXPECT_DOUBLE_EQ(5.0, dt_start->data.double_data[0]);
  EXPECT_TRUE(std::isnan(dt_start->data.double_data[1]));
  EXPECT_TRUE(std::isnan(dt_start->data.double_data[2]));

  EXPECT_DOUBLE_EQ(35.0, dt_stop->data.double_data[0]);
  EXPECT_TRUE(std::isnan(dt_stop->data.double_data[1]));
  EXPECT_TRUE(std::isnan(dt_stop->data.double_data[2]));

  // temperature: bin 0 = (200+220+240)/3 = 220, bin 1 = nan, bin 2 = 280.
  EXPECT_EQ(harp_type_double, temp->data_type);
  EXPECT_DOUBLE_EQ(220.0, temp->data.double_data[0]);
  EXPECT_TRUE(std::isnan(temp->data.double_data[1]));
  EXPECT_DOUBLE_EQ(280.0, temp->data.double_data[2]);

  // wind_direction: bin 0 has 350, 10, 90 -> angle weight created; bin 1 is
  // empty (weight 0, NaN); bin 2 had only NaN so angle is NaN and weight is 0.
  harp_variable* wind_w = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                              prod.get(), "wind_direction_weight", &wind_w));
  EXPECT_EQ(harp_type_float, wind_w->data_type);
  EXPECT_GT(wind_w->data.float_data[0], 0.0f);
  EXPECT_FLOAT_EQ(0.0f, wind_w->data.float_data[1]);
  EXPECT_FLOAT_EQ(0.0f, wind_w->data.float_data[2]);
  EXPECT_FLOAT_EQ(0.0f, wind_w->data.float_data[3]);
  EXPECT_TRUE(std::isnan(wind_dir->data.double_data[1]));
  EXPECT_TRUE(std::isnan(wind_dir->data.double_data[2]));
  EXPECT_TRUE(std::isnan(wind_dir->data.double_data[3]));
}

TEST(HarpBinTest, BinEdgeBoundsLatitudeAndLongitudeKept) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  constexpr harp_dimension_type kEdgeDim[] = {harp_dimension_time,
                                              harp_dimension_independent};
  constexpr long kEdgeLen[] = {2, 2};
  harp_variable* lat_bounds =
      AddVariableOrFail(prod.get(), "latitude_bounds", harp_type_double, 2,
                        kEdgeDim, kEdgeLen, "degree_north");
  ASSERT_NE(nullptr, lat_bounds);
  lat_bounds->data.double_data[0] = -10.0;
  lat_bounds->data.double_data[1] = 10.0;
  lat_bounds->data.double_data[2] = 10.0;
  lat_bounds->data.double_data[3] = 30.0;

  harp_variable* lon_bounds =
      AddVariableOrFail(prod.get(), "longitude_bounds", harp_type_double, 2,
                        kEdgeDim, kEdgeLen, "degree_east");
  ASSERT_NE(nullptr, lon_bounds);
  lon_bounds->data.double_data[0] = 350.0;
  lon_bounds->data.double_data[1] = 10.0;
  lon_bounds->data.double_data[2] = 10.0;
  lon_bounds->data.double_data[3] = 350.0;

  long bin_index[] = {0, 0};
  ASSERT_EQ(HARP_SUCCESS, harp_product_bin(prod.get(), 1, 2, bin_index));
  EXPECT_EQ(1, harp_product_has_variable(prod.get(), "latitude_bounds"));
  EXPECT_EQ(1, harp_product_has_variable(prod.get(), "longitude_bounds"));
  EXPECT_NEAR(0.0, lat_bounds->data.double_data[0], 1e-9);
  EXPECT_NEAR(20.0, lat_bounds->data.double_data[1], 1e-9);
  EXPECT_NEAR(0.0, lon_bounds->data.double_data[0], 1e-9);
  EXPECT_NEAR(0.0, lon_bounds->data.double_data[1], 1e-9);
}

TEST(HarpBinTest, BinUncertaintiesRespectSystematicRandomAndOption) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const int prev_prop = harp_get_option_propagate_uncertainty();
  auto restore_prop = absl::MakeCleanup(
      [&] { harp_set_option_propagate_uncertainty(prev_prop); });

  for (int prop_opt : {0, 1}) {
    ASSERT_EQ(HARP_SUCCESS, harp_set_option_propagate_uncertainty(prop_opt));
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);

    constexpr harp_dimension_type kDims[] = {harp_dimension_time};
    constexpr long kLen[] = {2};
    harp_variable* sys_u =
        AddVariableOrFail(prod.get(), "O3_uncertainty_systematic",
                          harp_type_double, 1, kDims, kLen, "DU");
    harp_variable* rand_u =
        AddVariableOrFail(prod.get(), "O3_uncertainty_random", harp_type_double,
                          1, kDims, kLen, "DU");
    harp_variable* tot_u = AddVariableOrFail(
        prod.get(), "O3_uncertainty", harp_type_double, 1, kDims, kLen, "DU");
    ASSERT_NE(nullptr, sys_u);
    ASSERT_NE(nullptr, rand_u);
    ASSERT_NE(nullptr, tot_u);

    for (auto* v : {sys_u, rand_u, tot_u}) {
      v->data.double_data[0] = 3.0;
      v->data.double_data[1] = 4.0;
    }

    long bin_index[] = {0, 0};
    ASSERT_EQ(HARP_SUCCESS, harp_product_bin(prod.get(), 1, 2, bin_index));

    // Systematic always correlates fully: (3 + 4) / 2 = 3.5
    EXPECT_THAT(sys_u->data.double_data[0], DoubleNear(3.5, 1e-12));
    // Random always propagates uncorrelated: sqrt(3^2 + 4^2) / 2 = 2.5
    EXPECT_THAT(rand_u->data.double_data[0], DoubleNear(2.5, 1e-12));
    // Total depends on option: 0 -> 2.5, 1 -> 3.5
    EXPECT_THAT(tot_u->data.double_data[0],
                DoubleNear(prop_opt == 1 ? 3.5 : 2.5, 1e-12));
  }
}

TEST(HarpBinTest, BinWithExistingCountsWeightsNansAndExpandingBins) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  constexpr harp_dimension_type kTimeDim[] = {harp_dimension_time};
  constexpr long kTimeLen[] = {3};
  constexpr harp_dimension_type kTimeVertDim[] = {harp_dimension_time,
                                                  harp_dimension_vertical};
  constexpr long kTimeVertLen[] = {3, 2};
  constexpr harp_dimension_type k3DDim[] = {
      harp_dimension_time, harp_dimension_vertical, harp_dimension_spectral};
  constexpr long k3DLen[] = {3, 2, 1};

  // Pre-existing valid 1D "count" and 1D "weight" (exercises broadcasting).
  harp_variable* g_count = AddVariableOrFail(
      prod.get(), "count", harp_type_int32, 1, kTimeDim, kTimeLen);
  ASSERT_NE(nullptr, g_count);
  g_count->data.int32_data[0] = 2;
  g_count->data.int32_data[1] = 4;
  g_count->data.int32_data[2] = 1;

  harp_variable* g_weight = AddVariableOrFail(
      prod.get(), "weight", harp_type_float, 1, kTimeDim, kTimeLen);
  ASSERT_NE(nullptr, g_weight);
  g_weight->data.float_data[0] = 1.0f;
  g_weight->data.float_data[1] = 3.0f;
  g_weight->data.float_data[2] = 0.0f;

  // 2D variable using global weight (weight takes preference over count).
  harp_variable* o3 =
      AddVariableOrFail(prod.get(), "O3_density", harp_type_double, 2,
                        kTimeVertDim, kTimeVertLen, "DU");
  ASSERT_NE(nullptr, o3);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  // Sample 0 (bin 0): [nan, 10] with w=1; Sample 1 (bin 0): [20, nan] with w=3;
  // Sample 2 (bin 2): [50, 60] with w=0.
  o3->data.double_data[0] = nan;
  o3->data.double_data[1] = 10.0;
  o3->data.double_data[2] = 20.0;
  o3->data.double_data[3] = nan;
  o3->data.double_data[4] = 50.0;
  o3->data.double_data[5] = 60.0;

  // Variable with valid variable-specific <name>_count (and NaN at all-NaN
  // bin 2, with matching count=0 at NaN indices; plus a separate product
  // without global weight to exercise get_count_variable_for_variable removal).
  harp_variable* co = AddVariableOrFail(
      prod.get(), "CO_density", harp_type_double, 1, kTimeDim, kTimeLen, "DU");
  ASSERT_NE(nullptr, co);
  co->data.double_data[0] = nan;
  co->data.double_data[1] = 8.0;
  co->data.double_data[2] = nan;
  harp_variable* co_count = AddVariableOrFail(
      prod.get(), "CO_density_count", harp_type_int32, 1, kTimeDim, kTimeLen);
  ASSERT_NE(nullptr, co_count);
  co_count->data.int32_data[0] = 0;
  co_count->data.int32_data[1] = 4;
  co_count->data.int32_data[2] = 0;

  // 2D variable with mismatched <name>_weight (dim type/len) -> removed.
  constexpr harp_dimension_type kTimeIndepDim[] = {harp_dimension_time,
                                                   harp_dimension_independent};
  constexpr long kMismatchLen[] = {3, 1};
  constexpr long kIndep2Len[] = {3, 2};
  harp_variable* indep_v =
      AddVariableOrFail(prod.get(), "indep_val", harp_type_double, 2,
                        kTimeIndepDim, kIndep2Len, "1");
  ASSERT_NE(nullptr, indep_v);
  for (int i = 0; i < 6; ++i) {
    indep_v->data.double_data[i] = 1.0;
  }
  ASSERT_NE(nullptr,
            AddVariableOrFail(prod.get(), "indep_val_weight", harp_type_float,
                              2, kTimeIndepDim, kMismatchLen));
  ASSERT_NE(nullptr,
            AddVariableOrFail(prod.get(), "NO2_density", harp_type_double, 2,
                              kTimeVertDim, kTimeVertLen, "DU"));
  // Mismatched num_dimensions on NO2_density_weight.
  ASSERT_NE(nullptr, AddVariableOrFail(prod.get(), "NO2_density_weight",
                                       harp_type_float, 1, kTimeDim, kTimeLen));

  // Pre-existing <angle>_weight with a zero norm sample.
  harp_variable* angle =
      AddVariableOrFail(prod.get(), "solar_zenith_angle", harp_type_double, 1,
                        kTimeDim, kTimeLen, "degree");
  ASSERT_NE(nullptr, angle);
  angle->data.double_data[0] = 30.0;
  angle->data.double_data[1] = 30.0;
  angle->data.double_data[2] = 60.0;
  harp_variable* angle_w =
      AddVariableOrFail(prod.get(), "solar_zenith_angle_weight",
                        harp_type_float, 1, kTimeDim, kTimeLen);
  ASSERT_NE(nullptr, angle_w);
  angle_w->data.float_data[0] = 0.0f;
  angle_w->data.float_data[1] = 2.0f;
  angle_w->data.float_data[2] = 1.0f;

  // 3D variable to ensure global 1D "weight" works for 3D as well.
  harp_variable* v3d = AddVariableOrFail(
      prod.get(), "aerosol", harp_type_double, 3, k3DDim, k3DLen, "1");
  ASSERT_NE(nullptr, v3d);
  v3d->data.double_data[0] = 1.0;
  v3d->data.double_data[1] = 2.0;
  v3d->data.double_data[2] = 3.0;
  v3d->data.double_data[3] = 4.0;
  v3d->data.double_data[4] = 5.0;
  v3d->data.double_data[5] = 6.0;

  // Bin 3 samples into 3 bins where bin 1 is empty: {0, 0, 2}.
  long bin_index[] = {0, 0, 2};
  ASSERT_EQ(HARP_SUCCESS, harp_product_bin(prod.get(), 3, 3, bin_index));

  EXPECT_EQ(3L, prod->dimension[harp_dimension_time]);
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "indep_val_weight"));
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "NO2_density_weight"));

  // Global count summed: bin 0 = 2+4 = 6, bin 1 = 0, bin 2 = 1.
  EXPECT_EQ(6, g_count->data.int32_data[0]);
  EXPECT_EQ(0, g_count->data.int32_data[1]);
  EXPECT_EQ(1, g_count->data.int32_data[2]);

  // O3_density: bin 0 col 0 has only sample 1 (20*3/3 = 20), col 1 has only
  // sample 0 (10*1/1 = 10). Bin 2 has weight 0 -> NaN.
  EXPECT_THAT(o3->data.double_data[0], DoubleNear(20.0, 1e-12));
  EXPECT_THAT(o3->data.double_data[1], DoubleNear(10.0, 1e-12));
  EXPECT_TRUE(std::isnan(o3->data.double_data[4]));
  EXPECT_TRUE(std::isnan(o3->data.double_data[5]));

  // O3_density_weight was created because O3_density had NaNs with weights.
  harp_variable* o3_w = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                              prod.get(), "O3_density_weight", &o3_w));
  EXPECT_THAT(o3_w->data.float_data[0], FloatNear(3.0f, 1e-6f));
  EXPECT_THAT(o3_w->data.float_data[1], FloatNear(1.0f, 1e-6f));

  // CO_density: bin 0 = 8.0, bin 2 = NaN (all-NaN sample -> count zeroed).
  EXPECT_THAT(co->data.double_data[0], DoubleNear(8.0, 1e-12));
  EXPECT_TRUE(std::isnan(co->data.double_data[2]));
  EXPECT_EQ(4, co_count->data.int32_data[0]);
  EXPECT_EQ(0, co_count->data.int32_data[2]);

  // solar_zenith_angle: bin 0 had weight (0, 2) at 30 deg -> 30 deg, norm 2.
  EXPECT_THAT(angle->data.double_data[0], DoubleNear(30.0, 1e-9));
  EXPECT_THAT(angle_w->data.float_data[0], FloatNear(2.0f, 1e-6f));
  EXPECT_THAT(angle->data.double_data[2], DoubleNear(60.0, 1e-9));
}

TEST(HarpBinTest, BinGlobalWeightMismatchAndCountBroadcast) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  constexpr harp_dimension_type kTimeDim[] = {harp_dimension_time};
  constexpr long kTimeLen[] = {2};
  constexpr harp_dimension_type kTimeVertDim[] = {harp_dimension_time,
                                                  harp_dimension_vertical};
  constexpr long kTimeVertLen[] = {2, 2};

  // Global 2D "weight" [time, vertical] will not match a 1D [time] variable
  // (weight_variable->num_dimensions > variable->num_dimensions) nor a
  // [time, spectral] variable (dimension_type mismatch).
  harp_variable* g_weight = AddVariableOrFail(
      prod.get(), "weight", harp_type_float, 2, kTimeVertDim, kTimeVertLen);
  ASSERT_NE(nullptr, g_weight);
  for (int i = 0; i < 4; ++i) {
    g_weight->data.float_data[i] = 2.0f;
  }

  // Global 1D "count" [time] broadcasts across 2D [time, spectral].
  harp_variable* g_count = AddVariableOrFail(
      prod.get(), "count", harp_type_int32, 1, kTimeDim, kTimeLen);
  ASSERT_NE(nullptr, g_count);
  g_count->data.int32_data[0] = 1;
  g_count->data.int32_data[1] = 3;

  constexpr harp_dimension_type kTimeSpecDim[] = {harp_dimension_time,
                                                  harp_dimension_spectral};
  constexpr long kTimeSpecLen[] = {2, 2};
  harp_variable* spec_var =
      AddVariableOrFail(prod.get(), "radiance", harp_type_double, 2,
                        kTimeSpecDim, kTimeSpecLen, "1");
  ASSERT_NE(nullptr, spec_var);
  spec_var->data.double_data[0] = 10.0;
  spec_var->data.double_data[1] = 20.0;
  spec_var->data.double_data[2] = 30.0;
  spec_var->data.double_data[3] = 40.0;

  harp_variable* t_var = AddVariableOrFail(
      prod.get(), "pressure", harp_type_double, 1, kTimeDim, kTimeLen, "Pa");
  ASSERT_NE(nullptr, t_var);
  t_var->data.double_data[0] = 100.0;
  t_var->data.double_data[1] = 200.0;

  // Variable with NO matching count/weight (uses implicit count=1) and a NaN
  // at non-first sample to trigger <var>_count creation with existing count.
  harp_variable* no_cw = AddVariableOrFail(
      prod.get(), "humidity", harp_type_double, 1, kTimeDim, kTimeLen, "1");
  ASSERT_NE(nullptr, no_cw);
  no_cw->data.double_data[0] = 0.5;
  no_cw->data.double_data[1] = std::numeric_limits<double>::quiet_NaN();

  long bin_index[] = {0, 0};
  ASSERT_EQ(HARP_SUCCESS, harp_product_bin(prod.get(), 1, 2, bin_index));

  // Global count weighted: radiance = (10*1 + 30*3)/(1+3) = 25,
  // (20*1 + 40*3)/4 = 35; pressure = (100*1 + 200*3)/4 = 175.
  EXPECT_THAT(spec_var->data.double_data[0], DoubleNear(25.0, 1e-12));
  EXPECT_THAT(spec_var->data.double_data[1], DoubleNear(35.0, 1e-12));
  EXPECT_THAT(t_var->data.double_data[0], DoubleNear(175.0, 1e-12));
  // Note: humidity uses global count (1, 3), sample 1 is NaN so count[1] -> 0
  // and humidity_count is created with [1].
  EXPECT_THAT(no_cw->data.double_data[0], DoubleNear(0.5, 1e-12));
  harp_variable* hum_c = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                              prod.get(), "humidity_count", &hum_c));
  EXPECT_EQ(1, hum_c->data.int32_data[0]);
}

TEST(HarpBinTest, BinFullEmptyAndPopulated) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  // Empty product (time dim == 0) is a no-op.
  EXPECT_EQ(HARP_SUCCESS, harp_product_bin_full(prod.get()));

  constexpr harp_dimension_type kDims[] = {harp_dimension_time};
  constexpr long kLen[] = {3};
  harp_variable* v = AddVariableOrFail(prod.get(), "O3_column_number_density",
                                       harp_type_double, 1, kDims, kLen, "DU");
  ASSERT_NE(nullptr, v);
  v->data.double_data[0] = 300.0;
  v->data.double_data[1] = 310.0;
  v->data.double_data[2] = 320.0;

  ASSERT_EQ(HARP_SUCCESS, harp_product_bin_full(prod.get()));
  EXPECT_EQ(1L, prod->dimension[harp_dimension_time]);
  EXPECT_THAT(v->data.double_data[0], DoubleNear(310.0, 1e-12));

  // Angle with incompatible unit ("m" -> "rad" fails) propagates error from
  // harp_product_bin through harp_product_bin_full.
  harp_product* raw_bad = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_bad));
  ScopedHarpProduct bad_prod(raw_bad);
  constexpr long kBadLen[] = {2};
  harp_variable* bad_angle =
      AddVariableOrFail(bad_prod.get(), "viewing_angle", harp_type_double, 1,
                        kDims, kBadLen, "m");
  ASSERT_NE(nullptr, bad_angle);
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_product_bin_full(bad_prod.get()));
  EXPECT_EQ(HARP_ERROR_UNIT_CONVERSION, harp_errno);
  harp_set_error(HARP_SUCCESS, nullptr);
}

TEST(HarpBinTest, BinWithCollocatedDatasetSuccessAndErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  harp_collocation_result* raw_col = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_new(&raw_col, 0, nullptr, nullptr));
  ScopedHarpCollocationResult col(raw_col);

  // 1. Missing collocation_index variable.
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            harp_product_bin_with_collocated_dataset(prod.get(), col.get()));
  EXPECT_EQ(HARP_ERROR_VARIABLE_NOT_FOUND, harp_errno);

  // Add collocation_index and value variable to product.
  constexpr harp_dimension_type kDims[] = {harp_dimension_time};
  constexpr long kLen[] = {3};
  harp_variable* col_idx = AddVariableOrFail(prod.get(), "collocation_index",
                                             harp_type_int32, 1, kDims, kLen);
  ASSERT_NE(nullptr, col_idx);
  col_idx->data.int32_data[0] = 10;
  col_idx->data.int32_data[1] = 11;
  col_idx->data.int32_data[2] = 12;

  harp_variable* val = AddVariableOrFail(prod.get(), "temperature",
                                         harp_type_double, 1, kDims, kLen, "K");
  ASSERT_NE(nullptr, val);
  val->data.double_data[0] = 280.0;
  val->data.double_data[1] = 290.0;
  val->data.double_data[2] = 300.0;

  // 2. Mismatch between collocation_result pairs and product collocation_index.
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_add_pair(col.get(), 10, "prodA", 0, "prodB",
                                             0, 0, nullptr));
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            harp_product_bin_with_collocated_dataset(prod.get(), col.get()));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  harp_set_error(HARP_SUCCESS, nullptr);

  // 3. Matching 3 pairs: pair 10 & 11 share (prodB, 0); pair 12 has (prodB, 1).
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_add_pair(col.get(), 11, "prodA", 1, "prodB",
                                             0, 0, nullptr));
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_add_pair(col.get(), 12, "prodA", 2, "prodB",
                                             1, 0, nullptr));

  ASSERT_EQ(HARP_SUCCESS,
            harp_product_bin_with_collocated_dataset(prod.get(), col.get()));
  EXPECT_EQ(2L, prod->dimension[harp_dimension_time]);
  EXPECT_THAT(val->data.double_data[0], DoubleNear(285.0, 1e-12));
  EXPECT_THAT(val->data.double_data[1], DoubleNear(300.0, 1e-12));

  harp_variable* out_col_idx = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                              prod.get(), "collocation_index", &out_col_idx));
  EXPECT_EQ(2L, out_col_idx->num_elements);
  EXPECT_EQ(10, out_col_idx->data.int32_data[0]);
  EXPECT_EQ(12, out_col_idx->data.int32_data[1]);
}

TEST(HarpBinTest, BinWithCollocatedDatasetPropagatesBinError) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  constexpr harp_dimension_type kDims[] = {harp_dimension_time};
  constexpr long kLen[] = {2};
  harp_variable* col_idx = AddVariableOrFail(prod.get(), "collocation_index",
                                             harp_type_int32, 1, kDims, kLen);
  ASSERT_NE(nullptr, col_idx);
  col_idx->data.int32_data[0] = 1;
  col_idx->data.int32_data[1] = 2;

  // Incompatible unit on angle variable causes harp_product_bin to fail.
  ASSERT_NE(nullptr, AddVariableOrFail(prod.get(), "sensor_angle",
                                       harp_type_double, 1, kDims, kLen, "m"));

  harp_collocation_result* raw_col = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_new(&raw_col, 0, nullptr, nullptr));
  ScopedHarpCollocationResult col(raw_col);
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_add_pair(col.get(), 1, "prodA", 0, "prodB",
                                             0, 0, nullptr));
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_add_pair(col.get(), 2, "prodA", 1, "prodB",
                                             0, 0, nullptr));

  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            harp_product_bin_with_collocated_dataset(prod.get(), col.get()));
  EXPECT_EQ(HARP_ERROR_UNIT_CONVERSION, harp_errno);
  harp_set_error(HARP_SUCCESS, nullptr);
}

TEST(HarpBinTest, BinWithVariableValidationErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  constexpr harp_dimension_type kVertDim[] = {harp_dimension_vertical};
  constexpr long kVertLen[] = {2};
  ASSERT_NE(nullptr, AddVariableOrFail(prod.get(), "altitude", harp_type_double,
                                       1, kVertDim, kVertLen, "m"));

  const char* vars[] = {"altitude"};
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_product_bin_with_variable(prod.get(), 0, vars));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  const char* missing[] = {"does_not_exist"};
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_product_bin_with_variable(prod.get(), 1, missing));
  EXPECT_EQ(HARP_ERROR_VARIABLE_NOT_FOUND, harp_errno);

  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_product_bin_with_variable(prod.get(), 1, vars));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  harp_set_error(HARP_SUCCESS, nullptr);
}

TEST(HarpBinTest, BinWithVariableAllKeyTypesAndMultiKey) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  constexpr harp_dimension_type kDims[] = {harp_dimension_time};
  constexpr long kLen[] = {4};

  harp_variable* k_i8 =
      AddVariableOrFail(prod.get(), "k_i8", harp_type_int8, 1, kDims, kLen);
  harp_variable* k_i16 =
      AddVariableOrFail(prod.get(), "k_i16", harp_type_int16, 1, kDims, kLen);
  // k_i32 has unit "1", so get_binning_type != binning_remove (kept via
  // binning, not variable_copy).
  harp_variable* k_i32 = AddVariableOrFail(prod.get(), "k_i32", harp_type_int32,
                                           1, kDims, kLen, "1");
  harp_variable* k_str =
      AddVariableOrFail(prod.get(), "k_str", harp_type_string, 1, kDims, kLen);
  harp_variable* val = AddVariableOrFail(prod.get(), "value", harp_type_double,
                                         1, kDims, kLen, "m");
  ASSERT_NE(nullptr, k_i8);
  ASSERT_NE(nullptr, k_i16);
  ASSERT_NE(nullptr, k_i32);
  ASSERT_NE(nullptr, k_str);
  ASSERT_NE(nullptr, val);

  // Groups:
  // 0 & 1: (i8=1, i16=10, i32=100, str="a") -> value avg (10+20)/2 = 15
  // 2:     (i8=1, i16=10, i32=100, str="b") -> value 30
  // 3:     (i8=2, i16=10, i32=100, str="a") -> value 40
  k_i8->data.int8_data[0] = 1;
  k_i8->data.int8_data[1] = 1;
  k_i8->data.int8_data[2] = 1;
  k_i8->data.int8_data[3] = 2;

  k_i16->data.int16_data[0] = 10;
  k_i16->data.int16_data[1] = 10;
  k_i16->data.int16_data[2] = 10;
  k_i16->data.int16_data[3] = 10;

  k_i32->data.int32_data[0] = 100;
  k_i32->data.int32_data[1] = 100;
  k_i32->data.int32_data[2] = 100;
  k_i32->data.int32_data[3] = 100;

  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_string_data_element(k_str, 0, "a"));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_string_data_element(k_str, 1, "a"));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_string_data_element(k_str, 2, "b"));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_string_data_element(k_str, 3, "a"));

  val->data.double_data[0] = 10.0;
  val->data.double_data[1] = 20.0;
  val->data.double_data[2] = 30.0;
  val->data.double_data[3] = 40.0;

  const char* keys[] = {"k_i8", "k_i16", "k_i32", "k_str"};
  ASSERT_EQ(HARP_SUCCESS, harp_product_bin_with_variable(prod.get(), 4, keys));
  EXPECT_EQ(3L, prod->dimension[harp_dimension_time]);
  EXPECT_THAT(val->data.double_data[0], DoubleNear(15.0, 1e-12));
  EXPECT_THAT(val->data.double_data[1], DoubleNear(30.0, 1e-12));
  EXPECT_THAT(val->data.double_data[2], DoubleNear(40.0, 1e-12));
}

TEST(HarpBinTest, BinWithVariableFloatAndDoubleKeysWithNan) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  constexpr harp_dimension_type kDims[] = {harp_dimension_time};
  constexpr long kLen4[] = {4};
  const float nan_f = std::numeric_limits<float>::quiet_NaN();
  const double nan_d = std::numeric_limits<double>::quiet_NaN();

  // Pin upstream behavior in harp-bin.c (if (check_nan) evaluates the array
  // pointer rather than check_nan[k]): when sample 0 is non-NaN, each non-NaN
  // float/double sample starts a new bin, while NaN samples match each other
  // into the same bin.
  harp_product* raw_prod4 = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod4));
  ScopedHarpProduct prod4(raw_prod4);
  harp_variable* kf =
      AddVariableOrFail(prod4.get(), "kf", harp_type_float, 1, kDims, kLen4);
  harp_variable* kd =
      AddVariableOrFail(prod4.get(), "kd", harp_type_double, 1, kDims, kLen4);
  harp_variable* val = AddVariableOrFail(prod4.get(), "val", harp_type_double,
                                         1, kDims, kLen4, "1");
  ASSERT_NE(nullptr, kf);
  ASSERT_NE(nullptr, kd);
  ASSERT_NE(nullptr, val);

  kf->data.float_data[0] = 1.0f;
  kf->data.float_data[1] = 2.0f;
  kf->data.float_data[2] = nan_f;
  kf->data.float_data[3] = nan_f;

  kd->data.double_data[0] = 1.0;
  kd->data.double_data[1] = 2.0;
  kd->data.double_data[2] = nan_d;
  kd->data.double_data[3] = nan_d;

  val->data.double_data[0] = 10.0;
  val->data.double_data[1] = 20.0;
  val->data.double_data[2] = 30.0;
  val->data.double_data[3] = 40.0;

  const char* keys[] = {"kf", "kd"};
  ASSERT_EQ(HARP_SUCCESS, harp_product_bin_with_variable(prod4.get(), 2, keys));
  ASSERT_EQ(3L, prod4->dimension[harp_dimension_time]);
  EXPECT_THAT(val->data.double_data[0], DoubleNear(10.0, 1e-12));
  EXPECT_THAT(val->data.double_data[1], DoubleNear(20.0, 1e-12));
  EXPECT_THAT(val->data.double_data[2], DoubleNear(35.0, 1e-12));
}

TEST(HarpBinTest, BinWithVariablePropagatesBinError) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  constexpr harp_dimension_type kDims[] = {harp_dimension_time};
  constexpr long kLen[] = {2};
  harp_variable* key =
      AddVariableOrFail(prod.get(), "key", harp_type_int32, 1, kDims, kLen);
  ASSERT_NE(nullptr, key);
  key->data.int32_data[0] = 1;
  key->data.int32_data[1] = 1;
  ASSERT_NE(nullptr, AddVariableOrFail(prod.get(), "sensor_angle",
                                       harp_type_double, 1, kDims, kLen, "m"));

  const char* keys[] = {"key"};
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_product_bin_with_variable(prod.get(), 1, keys));
  EXPECT_EQ(HARP_ERROR_UNIT_CONVERSION, harp_errno);
  harp_set_error(HARP_SUCCESS, nullptr);
}

TEST(HarpBinTest, BinCountAndWeightEdgeBranchesAndExpand) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  constexpr harp_dimension_type kTimeDim[] = {harp_dimension_time};
  constexpr long kTimeLen[] = {3};
  constexpr harp_dimension_type kTVDim[] = {harp_dimension_time,
                                            harp_dimension_vertical};
  constexpr long kTVLen[] = {3, 2};
  constexpr harp_dimension_type kTSDim[] = {harp_dimension_time,
                                            harp_dimension_spectral};
  constexpr long kTSLen[] = {3, 2};
  constexpr harp_dimension_type kTIDim[] = {harp_dimension_time,
                                            harp_dimension_independent};
  constexpr long kTILen2[] = {3, 2};
  constexpr long kTILen1[] = {3, 1};
  const double nan = std::numeric_limits<double>::quiet_NaN();

  // 1. Product without "weight" or "count": exercises count_variable mismatch
  //    paths, NaN at target_index == i creating <var>_count, and missing
  //    global "weight" (2D variable).
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);

    // v1 + invalid v1_count (double -> binning_remove).
    harp_variable* v1 = AddVariableOrFail(prod.get(), "v1", harp_type_double, 1,
                                          kTimeDim, kTimeLen, "1");
    ASSERT_NE(nullptr, v1);
    v1->data.double_data[0] = 1.0;
    v1->data.double_data[1] = 2.0;
    v1->data.double_data[2] = 3.0;
    ASSERT_NE(nullptr,
              AddVariableOrFail(prod.get(), "v1_count", harp_type_double, 1,
                                kTimeDim, kTimeLen));

    // v2 {time, vertical} + v2_count {time} (num_dimensions mismatch).
    harp_variable* v2 = AddVariableOrFail(prod.get(), "v2", harp_type_double, 2,
                                          kTVDim, kTVLen, "1");
    ASSERT_NE(nullptr, v2);
    for (int i = 0; i < 6; ++i) {
      v2->data.double_data[i] = 1.0;
    }
    ASSERT_NE(nullptr,
              AddVariableOrFail(prod.get(), "v2_count", harp_type_int32, 1,
                                kTimeDim, kTimeLen));

    // v3 {time, independent(2)} + v3_count {time, independent(1)}.
    harp_variable* v3 = AddVariableOrFail(prod.get(), "v3", harp_type_double, 2,
                                          kTIDim, kTILen2, "1");
    ASSERT_NE(nullptr, v3);
    for (int i = 0; i < 6; ++i) {
      v3->data.double_data[i] = 1.0;
    }
    ASSERT_NE(nullptr, AddVariableOrFail(prod.get(), "v3_count",
                                         harp_type_int32, 2, kTIDim, kTILen1));

    // v4 {time, vertical} + v4_count {time, spectral} (dimension_type
    // mismatch).
    harp_variable* v4 = AddVariableOrFail(prod.get(), "v4", harp_type_double, 2,
                                          kTVDim, kTVLen, "1");
    ASSERT_NE(nullptr, v4);
    for (int i = 0; i < 6; ++i) {
      v4->data.double_data[i] = 1.0;
    }
    ASSERT_NE(nullptr, AddVariableOrFail(prod.get(), "v4_count",
                                         harp_type_int32, 2, kTSDim, kTSLen));

    // v5 {time} with NaN at sample 0 (target_index == i) -> creates v5_count.
    harp_variable* v5 = AddVariableOrFail(prod.get(), "v5", harp_type_double, 1,
                                          kTimeDim, kTimeLen, "1");
    ASSERT_NE(nullptr, v5);
    v5->data.double_data[0] = nan;
    v5->data.double_data[1] = 10.0;
    v5->data.double_data[2] = 20.0;

    long bin_index[] = {0, 0, 1};
    ASSERT_EQ(HARP_SUCCESS, harp_product_bin(prod.get(), 2, 3, bin_index));
    EXPECT_EQ(0, harp_product_has_variable(prod.get(), "v1_count"));
    EXPECT_EQ(0, harp_product_has_variable(prod.get(), "v2_count"));
    EXPECT_EQ(0, harp_product_has_variable(prod.get(), "v3_count"));
    EXPECT_EQ(0, harp_product_has_variable(prod.get(), "v4_count"));
    EXPECT_THAT(v5->data.double_data[0], DoubleNear(10.0, 1e-12));
    harp_variable* v5_c = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_get_variable_by_name(prod.get(), "v5_count", &v5_c));
    EXPECT_EQ(1, v5_c->data.int32_data[0]);
  }

  // 2. Invalid global "weight" (double -> binning_remove) ignored by 2D var.
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);
    ASSERT_NE(nullptr, AddVariableOrFail(prod.get(), "weight", harp_type_double,
                                         1, kTimeDim, kTimeLen));
    harp_variable* v = AddVariableOrFail(prod.get(), "v", harp_type_double, 2,
                                         kTVDim, kTVLen, "1");
    ASSERT_NE(nullptr, v);
    for (int i = 0; i < 6; ++i) {
      v->data.double_data[i] = 2.0;
    }
    long bin_index[] = {0, 0, 0};
    ASSERT_EQ(HARP_SUCCESS, harp_product_bin(prod.get(), 1, 3, bin_index));
    EXPECT_EQ(0, harp_product_has_variable(prod.get(), "weight"));
    EXPECT_THAT(v->data.double_data[0], DoubleNear(2.0, 1e-12));
  }

  // 3. 3D global "weight" rejected by 2D var (num_dimensions mismatch).
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);
    constexpr harp_dimension_type k3DDim[] = {
        harp_dimension_time, harp_dimension_vertical, harp_dimension_spectral};
    constexpr long k3DLen[] = {3, 2, 1};
    harp_variable* w3 = AddVariableOrFail(prod.get(), "weight", harp_type_float,
                                          3, k3DDim, k3DLen);
    ASSERT_NE(nullptr, w3);
    for (int i = 0; i < 6; ++i) {
      w3->data.float_data[i] = 5.0f;
    }
    harp_variable* v = AddVariableOrFail(prod.get(), "v", harp_type_double, 2,
                                         kTVDim, kTVLen, "1");
    ASSERT_NE(nullptr, v);
    for (int i = 0; i < 6; ++i) {
      v->data.double_data[i] = 3.0;
    }
    long bin_index[] = {0, 0, 0};
    ASSERT_EQ(HARP_SUCCESS, harp_product_bin(prod.get(), 1, 3, bin_index));
    EXPECT_THAT(v->data.double_data[0], DoubleNear(3.0, 1e-12));
  }

  // 4. NaN at target_index == i with use_weight_variable == 1 and pre-existing
  //    invalid v_weight (double) replaced by add_weight_variable.
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);
    harp_variable* w = AddVariableOrFail(prod.get(), "weight", harp_type_float,
                                         1, kTimeDim, kTimeLen);
    ASSERT_NE(nullptr, w);
    w->data.float_data[0] = 2.0f;
    w->data.float_data[1] = 3.0f;
    w->data.float_data[2] = 1.0f;
    harp_variable* v = AddVariableOrFail(prod.get(), "v", harp_type_double, 2,
                                         kTVDim, kTVLen, "1");
    ASSERT_NE(nullptr, v);
    v->data.double_data[0] = nan;
    v->data.double_data[1] = 4.0;
    v->data.double_data[2] = 10.0;
    v->data.double_data[3] = 6.0;
    v->data.double_data[4] = 20.0;
    v->data.double_data[5] = 8.0;
    ASSERT_NE(nullptr, AddVariableOrFail(prod.get(), "v_weight",
                                         harp_type_double, 2, kTVDim, kTVLen));
    long bin_index[] = {0, 0, 1};
    ASSERT_EQ(HARP_SUCCESS, harp_product_bin(prod.get(), 2, 3, bin_index));
    EXPECT_THAT(v->data.double_data[0], DoubleNear(10.0, 1e-12));
    harp_variable* vw = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_get_variable_by_name(prod.get(), "v_weight", &vw));
    EXPECT_EQ(harp_type_float, vw->data_type);
    EXPECT_THAT(vw->data.float_data[0], FloatNear(3.0f, 1e-6f));
  }

  // 5. num_bins > num_elements with 1D variable (safe because
  //    variable->num_elements / num_elements == 3 / 2 == 1 and index < 3).
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);
    constexpr long kLen2[] = {2};
    harp_variable* v = AddVariableOrFail(
        prod.get(), "temperature", harp_type_double, 1, kTimeDim, kLen2, "K");
    ASSERT_NE(nullptr, v);
    v->data.double_data[0] = 280.0;
    v->data.double_data[1] = 300.0;
    long bin_index[] = {0, 2};
    ASSERT_EQ(HARP_SUCCESS, harp_product_bin(prod.get(), 3, 2, bin_index));
    EXPECT_EQ(3L, prod->dimension[harp_dimension_time]);
    EXPECT_THAT(v->data.double_data[0], DoubleNear(280.0, 1e-12));
    EXPECT_TRUE(std::isnan(v->data.double_data[1]));
    EXPECT_THAT(v->data.double_data[2], DoubleNear(300.0, 1e-12));
  }

  // 6. Angle variable already at HARP_MAX_NUM_DIMS (8) -> adding the 2D vector
  //    dimension fails cleanly in harp_product_bin.
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);
    constexpr harp_dimension_type kMaxDims[HARP_MAX_NUM_DIMS] = {
        harp_dimension_time,        harp_dimension_independent,
        harp_dimension_independent, harp_dimension_independent,
        harp_dimension_independent, harp_dimension_independent,
        harp_dimension_independent, harp_dimension_independent};
    constexpr long kMaxLens[HARP_MAX_NUM_DIMS] = {2, 1, 1, 1, 1, 1, 1, 1};
    ASSERT_NE(nullptr,
              AddVariableOrFail(prod.get(), "solar_angle", harp_type_double,
                                HARP_MAX_NUM_DIMS, kMaxDims, kMaxLens, "rad"));
    long bin_index[] = {0, 0};
    harp_set_error(HARP_SUCCESS, nullptr);
    EXPECT_EQ(-1, harp_product_bin(prod.get(), 1, 2, bin_index));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }
}

TEST(HarpBinTest, OutOfMemoryPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const absl::Cleanup reset_err = [] { harp_set_error(HARP_SUCCESS, nullptr); };

  constexpr harp_dimension_type kTimeDim[] = {harp_dimension_time};
  constexpr long kTimeLen[] = {2};
  long bin_index[] = {0, 0};

  EXPECT_GT(SweepOomUntilSuccess([&]() {
              harp_product* prod = nullptr;
              {
                ScopedSuppressOom suppress;
                if (harp_product_new(&prod) != HARP_SUCCESS) {
                  return;
                }
                harp_variable* w = AddVariableOrFail(
                    prod, "weight", harp_type_float, 1, kTimeDim, kTimeLen);
                if (w != nullptr) {
                  w->data.float_data[0] = 1.0f;
                  w->data.float_data[1] = 2.0f;
                }
                harp_variable* dt0 =
                    AddVariableOrFail(prod, "datetime_start", harp_type_double,
                                      1, kTimeDim, kTimeLen, "s");
                if (dt0 != nullptr) {
                  dt0->data.double_data[0] = 10.0;
                  dt0->data.double_data[1] = 20.0;
                }
                harp_variable* ang =
                    AddVariableOrFail(prod, "wind_direction", harp_type_double,
                                      1, kTimeDim, kTimeLen, "degree");
                if (ang != nullptr) {
                  ang->data.double_data[0] = 350.0;
                  ang->data.double_data[1] = 10.0;
                }
                harp_variable* o3 = AddVariableOrFail(
                    prod, "O3", harp_type_int16, 1, kTimeDim, kTimeLen, "1");
                if (o3 != nullptr) {
                  o3->data.int16_data[0] = 300;
                  o3->data.int16_data[1] = 310;
                }
              }
              harp_product_bin(prod, 1, 2, bin_index);
              ScopedSuppressOom suppress;
              harp_product_delete(prod);
            }),
            0);
}

}  // namespace
}  // namespace stcorp_harp
