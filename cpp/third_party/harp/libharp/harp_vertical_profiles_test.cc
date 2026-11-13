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
// Unit tests for vertical profile conversions and smoothing
// (libharp/harp-vertical-profiles.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-vertical-profiles.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-vertical-profiles.h
//   https://github.com/stcorp/harp/blob/master/libharp/harp.h.in

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/google/util/temp_file.h"
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

void AddInt32Var(harp_product* product, const char* name,
                 std::initializer_list<harp_dimension_type> dim_types,
                 std::initializer_list<long> dims,
                 std::initializer_list<int32_t> values) {
  ScopedHarpVariable var =
      MakeVar(name, harp_type_int32, dim_types, dims, nullptr);
  ASSERT_NE(nullptr, var);
  ASSERT_EQ(static_cast<long>(values.size()), var->num_elements);
  long idx = 0;
  for (int32_t v : values) {
    var->data.int32_data[idx++] = v;
  }
  ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(product, var.get()));
  var.release();
}

// Builds a sample product with a 2-level vertical profile on time=2.
ScopedHarpProduct MakeSourceProfileProduct(
    const char* source_name, bool time_dependent_grid,
    std::initializer_list<int32_t> cidx) {
  harp_product* raw = nullptr;
  EXPECT_EQ(HARP_SUCCESS, harp_product_new(&raw));
  ScopedHarpProduct prod(raw);
  if (!prod) {
    return nullptr;
  }
  if (source_name != nullptr) {
    EXPECT_EQ(HARP_SUCCESS,
              harp_product_set_source_product(prod.get(), source_name));
  }
  if (cidx.size() > 0) {
    AddInt32Var(prod.get(), "collocation_index", {harp_dimension_time},
                {static_cast<long>(cidx.size())}, cidx);
  }
  if (time_dependent_grid) {
    AddDoubleVar(prod.get(), "altitude",
                 {harp_dimension_time, harp_dimension_vertical}, {2, 2}, "m",
                 {500.0, 1500.0, 500.0, 1500.0});
    AddDoubleVar(prod.get(), "altitude_bounds",
                 {harp_dimension_time, harp_dimension_vertical,
                  harp_dimension_independent},
                 {2, 2, 2}, "m",
                 {0.0, 1000.0, 1000.0, 2000.0, 0.0, 1000.0, 1000.0, 2000.0});
  } else {
    AddDoubleVar(prod.get(), "altitude", {harp_dimension_vertical}, {2}, "m",
                 {500.0, 1500.0});
    AddDoubleVar(prod.get(), "altitude_bounds",
                 {harp_dimension_vertical, harp_dimension_independent}, {2, 2},
                 "m", {0.0, 1000.0, 1000.0, 2000.0});
  }
  AddDoubleVar(prod.get(), "O3_number_density",
               {harp_dimension_time, harp_dimension_vertical}, {2, 2},
               "molec/m3", {1.0e18, 2.0e18, 3.0e18, 4.0e18});
  AddDoubleVar(prod.get(), "O3_column_number_density",
               {harp_dimension_time, harp_dimension_vertical}, {2, 2},
               "molec/m2", {10.0, 20.0, 30.0, 40.0});
  return prod;
}

// Builds a collocated product with index/collocation_index, altitude grid,
// AVKs, and optional apriori profiles.
ScopedHarpProduct MakeCollocatedProduct(
    const char* source_name, bool with_apriori, bool with_index_var,
    std::initializer_list<int32_t> indices) {
  harp_product* raw = nullptr;
  EXPECT_EQ(HARP_SUCCESS, harp_product_new(&raw));
  ScopedHarpProduct prod(raw);
  if (!prod) {
    return nullptr;
  }
  if (source_name != nullptr) {
    EXPECT_EQ(HARP_SUCCESS,
              harp_product_set_source_product(prod.get(), source_name));
  }
  const long num_times = static_cast<long>(indices.size());
  if (with_index_var) {
    AddInt32Var(prod.get(), "index", {harp_dimension_time}, {num_times},
                indices);
  } else {
    AddInt32Var(prod.get(), "collocation_index", {harp_dimension_time},
                {num_times}, indices);
  }

  std::vector<double> alt_vals;
  std::vector<double> bounds_vals;
  std::vector<double> avk_2d_vals;
  std::vector<double> avk_1d_vals;
  std::vector<double> nd_apriori_vals;
  std::vector<double> col_apriori_vals;
  for (long t = 0; t < num_times; ++t) {
    alt_vals.push_back(500.0);
    alt_vals.push_back(1500.0);
    bounds_vals.insert(bounds_vals.end(), {0.0, 1000.0, 1000.0, 2000.0});
    // Identity-like 2x2 AVK with slight mixing.
    avk_2d_vals.insert(avk_2d_vals.end(), {0.8, 0.2, 0.1, 0.9});
    avk_1d_vals.insert(avk_1d_vals.end(), {0.5, 0.5});
    nd_apriori_vals.insert(nd_apriori_vals.end(), {0.5e18, 1.0e18});
    col_apriori_vals.insert(col_apriori_vals.end(), {2.0, 4.0});
  }

  auto add_vec = [&](const char* name,
                     std::initializer_list<harp_dimension_type> dt,
                     std::initializer_list<long> d, const char* unit,
                     const std::vector<double>& vals) {
    ScopedHarpVariable v = MakeVar(name, harp_type_double, dt, d, unit);
    EXPECT_NE(nullptr, v);
    if (!v) return;
    for (long i = 0; i < v->num_elements; ++i) {
      v->data.double_data[i] = vals[i];
    }
    EXPECT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), v.get()));
    v.release();
  };

  add_vec("altitude", {harp_dimension_time, harp_dimension_vertical},
          {num_times, 2}, "m", alt_vals);
  add_vec("altitude_bounds",
          {harp_dimension_time, harp_dimension_vertical,
           harp_dimension_independent},
          {num_times, 2, 2}, "m", bounds_vals);
  add_vec(
      "O3_number_density_avk",
      {harp_dimension_time, harp_dimension_vertical, harp_dimension_vertical},
      {num_times, 2, 2}, "", avk_2d_vals);
  add_vec("O3_column_number_density_avk",
          {harp_dimension_time, harp_dimension_vertical}, {num_times, 2}, "",
          avk_1d_vals);
  if (with_apriori) {
    add_vec("O3_number_density_apriori",
            {harp_dimension_time, harp_dimension_vertical}, {num_times, 2},
            "molec/m3", nd_apriori_vals);
    add_vec("O3_column_number_density_apriori",
            {harp_dimension_time, harp_dimension_vertical}, {num_times, 2},
            "molec/m2", col_apriori_vals);
  }
  return prod;
}

TEST(HarpVerticalProfilesTest, GeopotentialAltitudeAndColumnMassDensity) {
  constexpr double kLat = 45.0;
  constexpr double kAlt = 5000.0;

  const double gph = harp_gph_from_altitude_and_latitude(kAlt, kLat);
  EXPECT_GT(gph, 4900.0);
  EXPECT_LT(gph, 5100.0);
  EXPECT_THAT(harp_altitude_from_gph_and_latitude(gph, kLat),
              DoubleNear(kAlt, 1e-6));

  const double geop = harp_geopotential_from_gph(gph);
  EXPECT_THAT(harp_gph_from_geopotential(geop), DoubleNear(gph, 1e-9));

  constexpr double kPressureBounds[] = {101325.0, 50000.0, 50000.0, 10000.0};
  constexpr double kAltProfile[] = {2500.0, 10000.0};
  const double cmd = harp_column_mass_density_from_surface_pressure_and_profile(
      101325.0, 2, kPressureBounds, kAltProfile, kLat);
  EXPECT_GT(cmd, 10000.0);
  EXPECT_LT(cmd, 10600.0);
}

TEST(HarpVerticalProfilesTest, TropopauseIndexFromAltitudeAndTemperature) {
  // 1. All levels > 50000 Pa -> -1.
  {
    constexpr double kAlt[] = {0.0, 1000.0, 2000.0};
    constexpr double kPres[] = {100000.0, 80000.0, 60000.0};
    constexpr double kTemp[] = {290.0, 283.0, 276.0};
    EXPECT_EQ(-1L, harp_tropopause_index_from_altitude_and_temperature(
                       3, kAlt, kPres, kTemp));
  }

  // 2. Non-increasing altitude before initial level -> -1.
  {
    constexpr double kAlt[] = {2000.0, 1000.0, 3000.0};
    constexpr double kPres[] = {100000.0, 40000.0, 20000.0};
    constexpr double kTemp[] = {290.0, 250.0, 240.0};
    EXPECT_EQ(-1L, harp_tropopause_index_from_altitude_and_temperature(
                       3, kAlt, kPres, kTemp));
  }

  // 3. Zero-height step at initial level (lapse_below = NaN) and non-increasing
  //    altitude inside the loop -> -1.
  {
    constexpr double kAlt[] = {5000.0, 5000.0, 4000.0};
    constexpr double kPres[] = {55000.0, 45000.0, 30000.0};
    constexpr double kTemp[] = {260.0, 255.0, 250.0};
    EXPECT_EQ(-1L, harp_tropopause_index_from_altitude_and_temperature(
                       3, kAlt, kPres, kTemp));
  }

  // 4. False tropopause (2km average lapse > 0.002) followed by a thin layer
  //    (height < EPSILON), then true tropopause.
  {
    // Level 0->1: p[1]=45000, dz=2000, dT=-14 -> lapse_below = 0.007 > 0.002
    // Level 1->2: dz=500, dT=-0.5 -> lapse_above = 0.001 <= 0.002 (candidate
    // i=1)
    //   Within 2km above alt[1]=6000: alt[3]=7500 has dz=1000, dT=-8 ->
    //   lapse=0.008 Avg lapse = 0.008 > 0.002 -> rejected!
    // Level 2->3: lapse_below=0.001 (<=0.002), alt[3]=7500 -> lapse=0.008
    // Level 3->4: dz=0 (thin layer skipped), lapse_above=0.008
    // Level 4->5: dz=1000, dT=-1 -> lapse_above=0.001 <= 0.002 (candidate i=4)
    //   Within 2km above alt[4]=7500: alt[6]=8500 (lapse=0.001) & alt[7]=9500
    //   (lapse=0.0) Avg lapse <= 0.002 -> returns i=4!
    constexpr double kAlt[] = {4000.0, 6000.0, 6500.0, 7500.0,
                               7500.0, 8500.0, 9500.0, 12000.0};
    constexpr double kPres[] = {60000.0, 45000.0, 40000.0, 30000.0,
                                28000.0, 20000.0, 15000.0, 4000.0};
    constexpr double kTemp[] = {270.0, 256.0, 255.5, 247.5,
                                247.5, 246.5, 246.5, 246.5};
    EXPECT_EQ(4L, harp_tropopause_index_from_altitude_and_temperature(
                      8, kAlt, kPres, kTemp));
  }

  // 5. Tropopause found with count == 0 (next level > 2km above candidate).
  {
    constexpr double kAlt[] = {5000.0, 7000.0, 10000.0, 13000.0};
    constexpr double kPres[] = {55000.0, 40000.0, 20000.0, 4000.0};
    constexpr double kTemp[] = {265.0, 250.0, 250.0, 250.0};
    EXPECT_EQ(1L, harp_tropopause_index_from_altitude_and_temperature(
                      4, kAlt, kPres, kTemp));
  }

  // 6. Steep lapse all the way above 5000 Pa -> -1.
  {
    constexpr double kAlt[] = {5000.0, 7000.0, 9000.0, 11000.0};
    constexpr double kPres[] = {55000.0, 40000.0, 20000.0, 4000.0};
    constexpr double kTemp[] = {270.0, 250.0, 230.0, 210.0};
    EXPECT_EQ(-1L, harp_tropopause_index_from_altitude_and_temperature(
                       4, kAlt, kPres, kTemp));
  }
}

TEST(HarpVerticalProfilesTest, ProfileConversionsPressureAltitudeGph) {
  constexpr long kLevels = 3;
  constexpr double kTemp[] = {288.0, 275.0, 260.0};
  constexpr double kMolar[] = {28.9644, 28.9644, 28.9644};
  constexpr double kSurfP = 101325.0;
  constexpr double kSurfZ = 0.0;
  constexpr double kLat = 45.0;

  // 1. Decreasing pressure (surface -> TOA) and round-trip via altitude.
  {
    constexpr double kPres[] = {90000.0, 70000.0, 50000.0};
    double alt[3] = {};
    double pres_back[3] = {};
    harp_profile_altitude_from_pressure(kLevels, kPres, kTemp, kMolar, kSurfP,
                                        kSurfZ, kLat, alt);
    EXPECT_LT(alt[0], alt[1]);
    EXPECT_LT(alt[1], alt[2]);
    harp_profile_pressure_from_altitude(kLevels, alt, kTemp, kMolar, kSurfP,
                                        kSurfZ, kLat, pres_back);
    for (int i = 0; i < kLevels; ++i) {
      EXPECT_THAT(pres_back[i], DoubleNear(kPres[i], 50.0));
    }
  }

  // 2. Increasing pressure (TOA -> surface) and decreasing altitude.
  {
    constexpr double kPresRev[] = {50000.0, 70000.0, 90000.0};
    constexpr double kTempRev[] = {260.0, 275.0, 288.0};
    double alt[3] = {};
    double pres_back[3] = {};
    harp_profile_altitude_from_pressure(kLevels, kPresRev, kTempRev, kMolar,
                                        kSurfP, kSurfZ, kLat, alt);
    EXPECT_GT(alt[0], alt[1]);
    EXPECT_GT(alt[1], alt[2]);
    harp_profile_pressure_from_altitude(kLevels, alt, kTempRev, kMolar, kSurfP,
                                        kSurfZ, kLat, pres_back);
    for (int i = 0; i < kLevels; ++i) {
      EXPECT_THAT(pres_back[i], DoubleNear(kPresRev[i], 50.0));
    }
  }

  // 3. GPH <-> Pressure in both surface-to-TOA and TOA-to-surface order.
  {
    constexpr double kPres[] = {90000.0, 70000.0, 50000.0};
    double gph[3] = {};
    double pres_back[3] = {};
    harp_profile_gph_from_pressure(kLevels, kPres, kTemp, kMolar, kSurfP,
                                   kSurfZ, gph);
    EXPECT_LT(gph[0], gph[1]);
    harp_profile_pressure_from_gph(kLevels, gph, kTemp, kMolar, kSurfP, kSurfZ,
                                   pres_back);
    for (int i = 0; i < kLevels; ++i) {
      EXPECT_THAT(pres_back[i], DoubleNear(kPres[i], 1e-6));
    }

    constexpr double kPresRev[] = {50000.0, 70000.0, 90000.0};
    constexpr double kTempRev[] = {260.0, 275.0, 288.0};
    harp_profile_gph_from_pressure(kLevels, kPresRev, kTempRev, kMolar, kSurfP,
                                   kSurfZ, gph);
    EXPECT_GT(gph[0], gph[1]);
    harp_profile_pressure_from_gph(kLevels, gph, kTempRev, kMolar, kSurfP,
                                   kSurfZ, pres_back);
    for (int i = 0; i < kLevels; ++i) {
      EXPECT_THAT(pres_back[i], DoubleNear(kPresRev[i], 1e-6));
    }
  }
}

TEST(HarpVerticalProfilesTest, ColumnIntegrationsFromPartialColumn) {
  const double nan = harp_nan();
  const double all_nan[] = {nan, nan, nan};
  const double partcol[] = {10.0, 20.0, nan, 30.0};
  constexpr double kAltBounds[] = {0.0,     5000.0,  5000.0,  15000.0,
                                   15000.0, 20000.0, 20000.0, 30000.0};
  constexpr double kPresBounds[] = {100000.0, 50000.0, 50000.0, 10000.0,
                                    10000.0,  5000.0,  5000.0,  1000.0};

  // Total column.
  EXPECT_DOUBLE_EQ(60.0, harp_profile_column_from_partial_column(4, partcol));
  EXPECT_EQ(1, harp_isnan(harp_profile_column_from_partial_column(3, all_nan)));

  // Tropospheric & stratospheric by altitude (tropopause = 10000 m splits level
  // [5000, 15000] in half).
  EXPECT_DOUBLE_EQ(20.0,
                   harp_profile_tropo_column_from_partial_column_and_altitude(
                       4, partcol, kAltBounds, 10000.0));
  EXPECT_EQ(
      1, harp_isnan(harp_profile_tropo_column_from_partial_column_and_altitude(
             3, all_nan, kAltBounds, 10000.0)));

  EXPECT_DOUBLE_EQ(40.0,
                   harp_profile_strato_column_from_partial_column_and_altitude(
                       4, partcol, kAltBounds, 10000.0));
  EXPECT_EQ(
      1, harp_isnan(harp_profile_strato_column_from_partial_column_and_altitude(
             3, all_nan, kAltBounds, 10000.0)));

  // Tropospheric & stratospheric by pressure (tropopause = sqrt(50000*10000)
  // splits level [50000, 10000] evenly in log-pressure).
  const double p_tropo = std::sqrt(50000.0 * 10000.0);
  EXPECT_THAT(harp_profile_tropo_column_from_partial_column_and_pressure(
                  4, partcol, kPresBounds, p_tropo),
              DoubleNear(20.0, 1e-9));
  EXPECT_EQ(
      1, harp_isnan(harp_profile_tropo_column_from_partial_column_and_pressure(
             3, all_nan, kPresBounds, p_tropo)));

  EXPECT_THAT(harp_profile_strato_column_from_partial_column_and_pressure(
                  4, partcol, kPresBounds, p_tropo),
              DoubleNear(40.0, 1e-9));
  EXPECT_EQ(
      1, harp_isnan(harp_profile_strato_column_from_partial_column_and_pressure(
             3, all_nan, kPresBounds, p_tropo)));
}

TEST(HarpVerticalProfilesTest, AveragingKernelConversions) {
  constexpr long kLevels = 3;
  constexpr double kAvk2D[] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0};
  double avk_1d[3] = {};
  harp_profile_column_avk_from_partial_column_avk(kLevels, kAvk2D, avk_1d);
  EXPECT_DOUBLE_EQ(12.0, avk_1d[0]);
  EXPECT_DOUBLE_EQ(15.0, avk_1d[1]);
  EXPECT_DOUBLE_EQ(18.0, avk_1d[2]);

  // Tropospheric / stratospheric column AVK split at 10000 m.
  constexpr double kAltBounds[] = {0.0,     5000.0,  5000.0,
                                   15000.0, 15000.0, 15000.0};
  double tropo_avk[3] = {};
  double strato_avk[3] = {};
  harp_profile_tropospheric_column_avk_from_column_avk(
      kLevels, avk_1d, kAltBounds, 10000.0, tropo_avk);
  EXPECT_DOUBLE_EQ(12.0, tropo_avk[0]);
  EXPECT_DOUBLE_EQ(15.0, tropo_avk[1]);
  EXPECT_DOUBLE_EQ(0.0, tropo_avk[2]);

  harp_profile_stratospheric_column_avk_from_column_avk(
      kLevels, avk_1d, kAltBounds, 10000.0, strato_avk);
  EXPECT_DOUBLE_EQ(0.0, strato_avk[0]);
  EXPECT_DOUBLE_EQ(15.0, strato_avk[1]);
  EXPECT_DOUBLE_EQ(18.0, strato_avk[2]);

  // Density AVK <-> Partial column AVK (level 2 has zero height -> zeroed).
  double density_avk[9] = {};
  double partcol_avk_back[9] = {};
  harp_density_avk_from_partial_column_avk_and_altitude_bounds(
      kLevels, kAvk2D, kAltBounds, density_avk);
  // Row 0 (h=5000), col 1 (h=10000): 2.0 / 5000 * 10000 = 4.0
  EXPECT_DOUBLE_EQ(4.0, density_avk[1]);
  EXPECT_DOUBLE_EQ(0.0, density_avk[2 * 3 + 0]);

  harp_partial_column_avk_from_density_avk_and_altitude_bounds(
      kLevels, density_avk, kAltBounds, partcol_avk_back);
  EXPECT_DOUBLE_EQ(2.0, partcol_avk_back[1]);
  EXPECT_DOUBLE_EQ(0.0, partcol_avk_back[0 * 3 + 2]);

  // Number density AVK <-> VMR AVK (level 2 has zero air density -> zeroed).
  constexpr double kAirNd[] = {2.0, 4.0, 0.0};
  double nd_avk[9] = {};
  double vmr_avk_back[9] = {};
  harp_number_density_avk_from_volume_mixing_ratio_avk(kLevels, kAvk2D, kAirNd,
                                                       nd_avk);
  EXPECT_DOUBLE_EQ(1.0, nd_avk[0]);
  EXPECT_DOUBLE_EQ(1.0, nd_avk[1]);  // 2.0 * 2.0 / 4.0
  EXPECT_DOUBLE_EQ(0.0, nd_avk[2]);

  harp_volume_mixing_ratio_avk_from_number_density_avk(kLevels, nd_avk, kAirNd,
                                                       vmr_avk_back);
  EXPECT_DOUBLE_EQ(1.0, vmr_avk_back[0]);
  EXPECT_DOUBLE_EQ(2.0, vmr_avk_back[1]);
  EXPECT_DOUBLE_EQ(0.0, vmr_avk_back[2 * 3 + 0]);
}

TEST(HarpVerticalProfilesTest, VariableSmoothVerticalValidationAndBehavior) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  ScopedHarpVariable valid_var =
      MakeVar("x", harp_type_double,
              {harp_dimension_time, harp_dimension_vertical}, {2, 2}, "1");
  ScopedHarpVariable valid_avk = MakeVar(
      "x_avk", harp_type_double,
      {harp_dimension_time, harp_dimension_vertical, harp_dimension_vertical},
      {2, 2, 2}, "");
  ScopedHarpVariable valid_apriori =
      MakeVar("x_apriori", harp_type_double,
              {harp_dimension_time, harp_dimension_vertical}, {2, 2}, "1");
  ScopedHarpVariable valid_axis =
      MakeVar("altitude", harp_type_double,
              {harp_dimension_time, harp_dimension_vertical}, {2, 2}, "m");

  // 1. Validation error paths.
  EXPECT_EQ(-1, harp_variable_smooth_vertical(nullptr, nullptr, valid_avk.get(),
                                              nullptr));
  EXPECT_EQ(-1, harp_variable_smooth_vertical(valid_var.get(), nullptr, nullptr,
                                              nullptr));

  ScopedHarpVariable int_var =
      MakeVar("xi", harp_type_int32,
              {harp_dimension_time, harp_dimension_vertical}, {2, 2}, "1");
  EXPECT_EQ(-1, harp_variable_smooth_vertical(int_var.get(), nullptr,
                                              valid_avk.get(), nullptr));

  ScopedHarpVariable bad_dim_var =
      MakeVar("x1", harp_type_double, {harp_dimension_time}, {2}, "1");
  EXPECT_EQ(-1, harp_variable_smooth_vertical(bad_dim_var.get(), nullptr,
                                              valid_avk.get(), nullptr));

  ScopedHarpVariable int_avk = MakeVar(
      "ai", harp_type_int32,
      {harp_dimension_time, harp_dimension_vertical, harp_dimension_vertical},
      {2, 2, 2}, "");
  EXPECT_EQ(-1, harp_variable_smooth_vertical(valid_var.get(), nullptr,
                                              int_avk.get(), nullptr));

  ScopedHarpVariable bad_dim_avk =
      MakeVar("a2", harp_type_double,
              {harp_dimension_time, harp_dimension_vertical}, {2, 2}, "");
  EXPECT_EQ(-1, harp_variable_smooth_vertical(valid_var.get(), nullptr,
                                              bad_dim_avk.get(), nullptr));

  ScopedHarpVariable rect_avk = MakeVar(
      "ar", harp_type_double,
      {harp_dimension_time, harp_dimension_vertical, harp_dimension_vertical},
      {2, 2, 2}, "");
  ASSERT_NE(nullptr, rect_avk);
  rect_avk->dimension[2] = 3;
  EXPECT_EQ(-1, harp_variable_smooth_vertical(valid_var.get(), nullptr,
                                              rect_avk.get(), nullptr));
  rect_avk->dimension[2] = 2;

  ScopedHarpVariable mismatch_avk = MakeVar(
      "am", harp_type_double,
      {harp_dimension_time, harp_dimension_vertical, harp_dimension_vertical},
      {1, 2, 2}, "");
  EXPECT_EQ(-1, harp_variable_smooth_vertical(valid_var.get(), nullptr,
                                              mismatch_avk.get(), nullptr));

  ScopedHarpVariable int_apriori =
      MakeVar("pi", harp_type_int32,
              {harp_dimension_time, harp_dimension_vertical}, {2, 2}, "1");
  EXPECT_EQ(-1,
            harp_variable_smooth_vertical(valid_var.get(), nullptr,
                                          valid_avk.get(), int_apriori.get()));

  ScopedHarpVariable bad_dim_apriori =
      MakeVar("p1", harp_type_double, {harp_dimension_time}, {2}, "1");
  EXPECT_EQ(-1, harp_variable_smooth_vertical(valid_var.get(), nullptr,
                                              valid_avk.get(),
                                              bad_dim_apriori.get()));

  ScopedHarpVariable mismatch_apriori =
      MakeVar("pm", harp_type_double,
              {harp_dimension_time, harp_dimension_vertical}, {2, 3}, "1");
  EXPECT_EQ(-1, harp_variable_smooth_vertical(valid_var.get(), nullptr,
                                              valid_avk.get(),
                                              mismatch_apriori.get()));

  ScopedHarpVariable int_axis =
      MakeVar("axi", harp_type_int32,
              {harp_dimension_time, harp_dimension_vertical}, {2, 2}, "m");
  EXPECT_EQ(-1, harp_variable_smooth_vertical(valid_var.get(), int_axis.get(),
                                              valid_avk.get(), nullptr));

  ScopedHarpVariable bad_dim_axis =
      MakeVar("ax1", harp_type_double, {harp_dimension_vertical}, {2}, "m");
  EXPECT_EQ(-1,
            harp_variable_smooth_vertical(valid_var.get(), bad_dim_axis.get(),
                                          valid_avk.get(), nullptr));

  ScopedHarpVariable mismatch_axis =
      MakeVar("axm", harp_type_double,
              {harp_dimension_time, harp_dimension_vertical}, {2, 3}, "m");
  EXPECT_EQ(-1,
            harp_variable_smooth_vertical(valid_var.get(), mismatch_axis.get(),
                                          valid_avk.get(), nullptr));
  harp_set_error(HARP_SUCCESS, nullptr);

  // 2. Smooth with apriori and NaN-padded vertical_axis (time 0 has 1 valid
  //    level; time 1 has all-NaN vertical_axis -> uses full length 2).
  valid_axis->data.double_data[0] = 500.0;
  valid_axis->data.double_data[1] = harp_nan();
  valid_axis->data.double_data[2] = harp_nan();
  valid_axis->data.double_data[3] = harp_nan();

  valid_var->data.double_data[0] = 10.0;
  valid_var->data.double_data[1] = 20.0;
  valid_var->data.double_data[2] = 30.0;
  valid_var->data.double_data[3] = 40.0;

  valid_apriori->data.double_data[0] = 2.0;
  valid_apriori->data.double_data[1] = 4.0;
  valid_apriori->data.double_data[2] = 6.0;
  valid_apriori->data.double_data[3] = 8.0;

  const double avk_vals[] = {0.5, 0.5, 0.25, 0.75, 0.5, 0.5, 0.25, 0.75};
  for (int i = 0; i < 8; ++i) {
    valid_avk->data.double_data[i] = avk_vals[i];
  }

  ASSERT_EQ(HARP_SUCCESS, harp_variable_smooth_vertical(
                              valid_var.get(), valid_axis.get(),
                              valid_avk.get(), valid_apriori.get()));
  // Time 0: unpadded length = 1 -> x_s[0] = 2 + 0.5*(10-2) = 6; x[1] untouched.
  EXPECT_DOUBLE_EQ(6.0, valid_var->data.double_data[0]);
  EXPECT_DOUBLE_EQ(20.0, valid_var->data.double_data[1]);
  // Time 1: unpadded length = 2 -> x_s[0] = 6 + 0.5*(24) + 0.5*(32) = 34.
  EXPECT_DOUBLE_EQ(34.0, valid_var->data.double_data[2]);

  // 3. Smooth multi-block {time, spectral, vertical} without apriori/axis.
  ScopedHarpVariable spec_var = MakeVar(
      "xs", harp_type_double,
      {harp_dimension_time, harp_dimension_spectral, harp_dimension_vertical},
      {2, 2, 2}, "1");
  spec_var->data.double_data[0] = 10.0;
  spec_var->data.double_data[1] = harp_nan();
  spec_var->data.double_data[2] = 10.0;
  spec_var->data.double_data[3] = 20.0;
  spec_var->data.double_data[4] = 10.0;
  spec_var->data.double_data[5] = 20.0;
  spec_var->data.double_data[6] = 10.0;
  spec_var->data.double_data[7] = 20.0;
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_smooth_vertical(spec_var.get(), nullptr,
                                          valid_avk.get(), nullptr));
  EXPECT_DOUBLE_EQ(5.0, spec_var->data.double_data[0]);
  EXPECT_EQ(1, harp_isnan(spec_var->data.double_data[1]));
  EXPECT_DOUBLE_EQ(15.0, spec_var->data.double_data[2]);
}

TEST(HarpVerticalProfilesTest, SmoothVerticalWithCollocatedProduct) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const char* smooth_vars[] = {"O3_number_density"};

  // 1. Error: product has no vertical dimension.
  {
    harp_product* raw = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw));
    ScopedHarpProduct no_vert(raw);
    ScopedHarpProduct colloc = MakeCollocatedProduct("b", true, false, {0, 1});
    EXPECT_EQ(
        -1, harp_product_smooth_vertical_with_collocated_product(
                no_vert.get(), 1, smooth_vars, "altitude", "m", colloc.get()));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 2. Error: smooth variable missing from product.
  {
    ScopedHarpProduct prod = MakeSourceProfileProduct("a", false, {0, 1});
    ScopedHarpProduct colloc = MakeCollocatedProduct("b", true, false, {0, 1});
    const char* missing[] = {"no_such_var"};
    EXPECT_EQ(-1, harp_product_smooth_vertical_with_collocated_product(
                      prod.get(), 1, missing, "altitude", "m", colloc.get()));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 3. Error: collocated_product missing collocation_index / altitude /
  //    altitude_bounds / avk.
  {
    ScopedHarpProduct prod = MakeSourceProfileProduct("a", false, {0, 1});
    harp_product* raw_c = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_c));
    ScopedHarpProduct empty_colloc(raw_c);
    EXPECT_EQ(-1, harp_product_smooth_vertical_with_collocated_product(
                      prod.get(), 1, smooth_vars, "altitude", "m",
                      empty_colloc.get()));

    AddInt32Var(empty_colloc.get(), "collocation_index", {harp_dimension_time},
                {2}, {0, 1});
    EXPECT_EQ(-1, harp_product_smooth_vertical_with_collocated_product(
                      prod.get(), 1, smooth_vars, "altitude", "m",
                      empty_colloc.get()));

    // 1-level altitude without bounds cannot derive altitude_bounds.
    {
      harp_product* raw_1l = nullptr;
      ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_1l));
      ScopedHarpProduct colloc_1l(raw_1l);
      AddInt32Var(colloc_1l.get(), "collocation_index", {harp_dimension_time},
                  {2}, {0, 1});
      AddDoubleVar(colloc_1l.get(), "altitude",
                   {harp_dimension_time, harp_dimension_vertical}, {2, 1}, "m",
                   {500.0, 500.0});
      EXPECT_EQ(-1, harp_product_smooth_vertical_with_collocated_product(
                        prod.get(), 1, smooth_vars, "altitude", "m",
                        colloc_1l.get()));
    }

    AddDoubleVar(empty_colloc.get(), "altitude",
                 {harp_dimension_time, harp_dimension_vertical}, {2, 2}, "m",
                 {500.0, 1500.0, 500.0, 1500.0});
    EXPECT_EQ(-1, harp_product_smooth_vertical_with_collocated_product(
                      prod.get(), 1, smooth_vars, "altitude", "m",
                      empty_colloc.get()));

    AddDoubleVar(empty_colloc.get(), "altitude_bounds",
                 {harp_dimension_time, harp_dimension_vertical,
                  harp_dimension_independent},
                 {2, 2, 2}, "m",
                 {0.0, 1000.0, 1000.0, 2000.0, 0.0, 1000.0, 1000.0, 2000.0});
    EXPECT_EQ(-1, harp_product_smooth_vertical_with_collocated_product(
                      prod.get(), 1, smooth_vars, "altitude", "m",
                      empty_colloc.get()));

    // filter_by_index failure, regrid failure (independent dim != 2), and
    // smooth failure.
    ScopedHarpProduct colloc = MakeCollocatedProduct("b", true, false, {0, 1});
    ScopedHarpProduct prod_mismatch =
        MakeSourceProfileProduct("a", false, {0, 99});
    EXPECT_EQ(-1, harp_product_smooth_vertical_with_collocated_product(
                      prod_mismatch.get(), 1, smooth_vars, "altitude", "m",
                      colloc.get()));

    ScopedHarpProduct colloc_bad_bounds =
        MakeCollocatedProduct("b", true, false, {0, 1});
    ASSERT_EQ(HARP_SUCCESS, harp_product_remove_variable_by_name(
                                colloc_bad_bounds.get(), "altitude_bounds"));
    AddDoubleVar(colloc_bad_bounds.get(), "altitude_bounds",
                 {harp_dimension_time, harp_dimension_vertical,
                  harp_dimension_independent},
                 {2, 2, 3}, "m", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
    EXPECT_EQ(-1, harp_product_smooth_vertical_with_collocated_product(
                      prod.get(), 1, smooth_vars, "altitude", "m",
                      colloc_bad_bounds.get()));

    ScopedHarpProduct prod_bad_layout =
        MakeSourceProfileProduct("a", false, {0, 1});
    AddDoubleVar(prod_bad_layout.get(), "bad_layout",
                 {harp_dimension_time, harp_dimension_vertical,
                  harp_dimension_independent},
                 {2, 2, 2}, "molec/m3",
                 {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0});
    AddDoubleVar(
        colloc.get(), "bad_layout_avk",
        {harp_dimension_time, harp_dimension_vertical, harp_dimension_vertical},
        {2, 2, 2}, "", {1, 0, 0, 1, 1, 0, 0, 1});
    const char* bad_vars[] = {"bad_layout"};
    EXPECT_EQ(-1, harp_product_smooth_vertical_with_collocated_product(
                      prod_bad_layout.get(), 1, bad_vars, "altitude", "m",
                      colloc.get()));
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 4. Success with apriori and reversed collocation_index order.
  {
    ScopedHarpProduct prod = MakeSourceProfileProduct("a", false, {1, 0});
    ScopedHarpProduct colloc = MakeCollocatedProduct("b", true, false, {0, 1});
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_smooth_vertical_with_collocated_product(
                  prod.get(), 1, smooth_vars, "altitude", "m", colloc.get()));
    harp_variable* o3 = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                                prod.get(), "O3_number_density", &o3));
    EXPECT_TRUE(std::isfinite(o3->data.double_data[0]));
  }

  // 5. Success without apriori.
  {
    ScopedHarpProduct prod = MakeSourceProfileProduct("a", true, {0, 1});
    ScopedHarpProduct colloc = MakeCollocatedProduct("b", false, false, {0, 1});
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_smooth_vertical_with_collocated_product(
                  prod.get(), 1, smooth_vars, "altitude", "m", colloc.get()));
    harp_variable* o3 = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                                prod.get(), "O3_number_density", &o3));
    EXPECT_DOUBLE_EQ(1.2e18, o3->data.double_data[0]);
  }
}

TEST(HarpVerticalProfilesTest, ProductGetSmoothedColumn) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  ScopedHarpVariable grid =
      MakeVar("altitude", harp_type_double,
              {harp_dimension_time, harp_dimension_vertical}, {2, 2}, "m");
  grid->data.double_data[0] = 500.0;
  grid->data.double_data[1] = 1500.0;
  grid->data.double_data[2] = 500.0;
  grid->data.double_data[3] = 1500.0;

  ScopedHarpVariable bounds =
      MakeVar("altitude_bounds", harp_type_double,
              {harp_dimension_time, harp_dimension_vertical,
               harp_dimension_independent},
              {2, 2, 2}, "m");
  const double bvals[] = {0.0, 1000.0, 1000.0, 2000.0,
                          0.0, 1000.0, 1000.0, 2000.0};
  for (int i = 0; i < 8; ++i) {
    bounds->data.double_data[i] = bvals[i];
  }

  ScopedHarpVariable avk =
      MakeVar("O3_column_number_density_avk", harp_type_double,
              {harp_dimension_time, harp_dimension_vertical}, {2, 2}, "");
  for (int i = 0; i < 4; ++i) {
    avk->data.double_data[i] = 0.5;
  }

  ScopedHarpVariable apriori = MakeVar(
      "O3_column_number_density_apriori", harp_type_double,
      {harp_dimension_time, harp_dimension_vertical}, {2, 2}, "molec/m2");
  for (int i = 0; i < 4; ++i) {
    apriori->data.double_data[i] = 2.0;
  }

  // 1. Validation errors.
  {
    ScopedHarpProduct prod = MakeSourceProfileProduct("a", false, {0, 1});
    harp_variable* out = nullptr;

    harp_product* raw_nv = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_nv));
    ScopedHarpProduct no_vert(raw_nv);
    EXPECT_EQ(-1, harp_product_get_smoothed_column(
                      no_vert.get(), "O3_column_number_density", "molec/m2",
                      grid.get(), bounds.get(), avk.get(), nullptr, &out));

    ScopedHarpVariable bad_grid =
        MakeVar("altitude", harp_type_double, {harp_dimension_time}, {2}, "m");
    EXPECT_EQ(-1, harp_product_get_smoothed_column(
                      prod.get(), "O3_column_number_density", "molec/m2",
                      bad_grid.get(), bounds.get(), avk.get(), nullptr, &out));

    ScopedHarpVariable int_grid =
        MakeVar("altitude", harp_type_int32,
                {harp_dimension_time, harp_dimension_vertical}, {2, 2}, "m");
    EXPECT_EQ(-1, harp_product_get_smoothed_column(
                      prod.get(), "O3_column_number_density", "molec/m2",
                      int_grid.get(), bounds.get(), avk.get(), nullptr, &out));

    ScopedHarpVariable bad_avk =
        MakeVar("avk", harp_type_double, {harp_dimension_time}, {2}, "");
    EXPECT_EQ(-1, harp_product_get_smoothed_column(
                      prod.get(), "O3_column_number_density", "molec/m2",
                      grid.get(), bounds.get(), bad_avk.get(), nullptr, &out));

    ScopedHarpVariable mismatch_avk =
        MakeVar("avk", harp_type_double,
                {harp_dimension_time, harp_dimension_vertical}, {2, 3}, "");
    EXPECT_EQ(
        -1, harp_product_get_smoothed_column(
                prod.get(), "O3_column_number_density", "molec/m2", grid.get(),
                bounds.get(), mismatch_avk.get(), nullptr, &out));

    ScopedHarpVariable int_avk =
        MakeVar("avk", harp_type_int32,
                {harp_dimension_time, harp_dimension_vertical}, {2, 2}, "");
    EXPECT_EQ(-1, harp_product_get_smoothed_column(
                      prod.get(), "O3_column_number_density", "molec/m2",
                      grid.get(), bounds.get(), int_avk.get(), nullptr, &out));

    ScopedHarpVariable int_apriori = MakeVar(
        "ap", harp_type_int32, {harp_dimension_time, harp_dimension_vertical},
        {2, 2}, "molec/m2");
    EXPECT_EQ(
        -1, harp_product_get_smoothed_column(
                prod.get(), "O3_column_number_density", "molec/m2", grid.get(),
                bounds.get(), avk.get(), int_apriori.get(), &out));

    ScopedHarpVariable dim_apriori = MakeVar(
        "ap", harp_type_double, {harp_dimension_vertical}, {2}, "molec/m2");
    EXPECT_EQ(
        -1, harp_product_get_smoothed_column(
                prod.get(), "O3_column_number_density", "molec/m2", grid.get(),
                bounds.get(), avk.get(), dim_apriori.get(), &out));

    ScopedHarpVariable mismatch_apriori = MakeVar(
        "ap", harp_type_double, {harp_dimension_time, harp_dimension_vertical},
        {1, 2}, "molec/m2");
    EXPECT_EQ(
        -1, harp_product_get_smoothed_column(
                prod.get(), "O3_column_number_density", "molec/m2", grid.get(),
                bounds.get(), avk.get(), mismatch_apriori.get(), &out));

    EXPECT_EQ(-1, harp_product_get_smoothed_column(
                      prod.get(), "nonexistent_species", "molec/m2", grid.get(),
                      bounds.get(), avk.get(), nullptr, &out));

    ScopedHarpVariable unknown_grid = MakeVar(
        "geopotential", harp_type_double,
        {harp_dimension_time, harp_dimension_vertical}, {2, 2}, "m2/s2");
    EXPECT_EQ(-1,
              harp_product_get_smoothed_column(
                  prod.get(), "O3_column_number_density", "molec/m2",
                  unknown_grid.get(), bounds.get(), avk.get(), nullptr, &out));

    // Invalid bounds independent dim (!= 2) triggers regrid failure inside
    // get_smoothed_column.
    ScopedHarpVariable bad_bounds =
        MakeVar("altitude_bounds", harp_type_double,
                {harp_dimension_time, harp_dimension_vertical,
                 harp_dimension_independent},
                {2, 2, 3}, "m");
    EXPECT_EQ(-1, harp_product_get_smoothed_column(
                      prod.get(), "O3_column_number_density", "molec/m2",
                      grid.get(), bad_bounds.get(), avk.get(), nullptr, &out));

    // Product with single-level vertical grid (no bounds) triggers
    // get_derived_bounds_for_grid failure? Note: 1-level grid succeeds in
    // get_derived_bounds_for_grid only if bounds exist, so use non-numeric
    // grid or pressure=0? Actually 1-level altitude without bounds fails in
    // harp_product_get_derived_bounds_for_grid only when num_levels==1!
    // Wait: let's check; wrap out just in case.
    harp_product* raw_1lev = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_1lev));
    ScopedHarpProduct prod_1lev(raw_1lev);
    AddDoubleVar(prod_1lev.get(), "altitude", {harp_dimension_vertical}, {1},
                 "m", {500.0});
    AddDoubleVar(prod_1lev.get(), "O3_column_number_density",
                 {harp_dimension_time, harp_dimension_vertical}, {2, 1},
                 "molec/m2", {10.0, 20.0});
    harp_variable* raw_out_1lev = nullptr;
    EXPECT_EQ(-1,
              harp_product_get_smoothed_column(
                  prod_1lev.get(), "O3_column_number_density", "molec/m2",
                  grid.get(), bounds.get(), avk.get(), nullptr, &raw_out_1lev));
    ScopedHarpVariable guard_1lev(raw_out_1lev);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 2. Success with time-independent source grid + bounds + apriori.
  {
    ScopedHarpProduct prod = MakeSourceProfileProduct("a", false, {0, 1});
    harp_variable* raw_out = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_get_smoothed_column(
                                prod.get(), "O3_column_number_density",
                                "molec/m2", grid.get(), bounds.get(), avk.get(),
                                apriori.get(), &raw_out));
    ScopedHarpVariable out(raw_out);
    // Time 0: (10-2)*0.5 + (20-2)*0.5 + 2 + 2 = 17.
    EXPECT_DOUBLE_EQ(17.0, out->data.double_data[0]);
    // Time 1: (30-2)*0.5 + (40-2)*0.5 + 2 + 2 = 37.
    EXPECT_DOUBLE_EQ(37.0, out->data.double_data[1]);
  }

  // 3. Success with time-dependent source grid, nullptr bounds/apriori, and
  //    all-NaN profile at time 1 -> output NaN.
  {
    ScopedHarpProduct prod = MakeSourceProfileProduct("a", true, {0, 1});
    harp_variable* partcol = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_get_variable_by_name(
                  prod.get(), "O3_column_number_density", &partcol));
    partcol->data.double_data[2] = harp_nan();
    partcol->data.double_data[3] = harp_nan();

    harp_variable* raw_out = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_get_smoothed_column(
                  prod.get(), "O3_column_number_density", "molec/m2",
                  grid.get(), nullptr, avk.get(), nullptr, &raw_out));
    ScopedHarpVariable out(raw_out);
    EXPECT_DOUBLE_EQ(15.0, out->data.double_data[0]);
    EXPECT_EQ(1, harp_isnan(out->data.double_data[1]));
  }
}

TEST(HarpVerticalProfilesTest, GetSmoothedColumnUsingCollocatedProduct) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_dimension_type dim_time[] = {harp_dimension_time};

  // 1. Validation errors.
  {
    ScopedHarpProduct prod = MakeSourceProfileProduct("a", false, {0, 1});
    ScopedHarpProduct colloc = MakeCollocatedProduct("b", true, false, {0, 1});
    harp_variable* out = nullptr;

    const harp_dimension_type bad_dim[] = {harp_dimension_vertical};
    EXPECT_EQ(-1, harp_product_get_smoothed_column_using_collocated_product(
                      prod.get(), "O3_column_number_density", "molec/m2", 0,
                      dim_time, "altitude", "m", colloc.get(), &out));
    EXPECT_EQ(-1, harp_product_get_smoothed_column_using_collocated_product(
                      prod.get(), "O3_column_number_density", "molec/m2", 1,
                      bad_dim, "altitude", "m", colloc.get(), &out));
    EXPECT_EQ(-1, harp_product_get_smoothed_column_using_collocated_product(
                      prod.get(), "O3_column_number_density", "molec/m2",
                      HARP_NUM_DIM_TYPES, dim_time, "altitude", "m",
                      colloc.get(), &out));

    harp_product* raw_nv = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_nv));
    ScopedHarpProduct no_vert(raw_nv);
    EXPECT_EQ(-1, harp_product_get_smoothed_column_using_collocated_product(
                      no_vert.get(), "O3_column_number_density", "molec/m2", 1,
                      dim_time, "altitude", "m", colloc.get(), &out));

    harp_product* raw_c = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_c));
    ScopedHarpProduct empty_colloc(raw_c);
    EXPECT_EQ(-1, harp_product_get_smoothed_column_using_collocated_product(
                      prod.get(), "O3_column_number_density", "molec/m2", 1,
                      dim_time, "altitude", "m", empty_colloc.get(), &out));

    AddInt32Var(empty_colloc.get(), "collocation_index", {harp_dimension_time},
                {2}, {0, 1});
    EXPECT_EQ(-1, harp_product_get_smoothed_column_using_collocated_product(
                      prod.get(), "O3_column_number_density", "molec/m2", 1,
                      dim_time, "altitude", "m", empty_colloc.get(), &out));

    // 1-level altitude without bounds cannot derive altitude_bounds.
    {
      harp_product* raw_1l = nullptr;
      ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_1l));
      ScopedHarpProduct colloc_1l(raw_1l);
      AddInt32Var(colloc_1l.get(), "collocation_index", {harp_dimension_time},
                  {2}, {0, 1});
      AddDoubleVar(colloc_1l.get(), "altitude",
                   {harp_dimension_time, harp_dimension_vertical}, {2, 1}, "m",
                   {500.0, 500.0});
      EXPECT_EQ(-1, harp_product_get_smoothed_column_using_collocated_product(
                        prod.get(), "O3_column_number_density", "molec/m2", 1,
                        dim_time, "altitude", "m", colloc_1l.get(), &out));
    }

    AddDoubleVar(empty_colloc.get(), "altitude",
                 {harp_dimension_time, harp_dimension_vertical}, {2, 2}, "m",
                 {500.0, 1500.0, 500.0, 1500.0});
    EXPECT_EQ(-1, harp_product_get_smoothed_column_using_collocated_product(
                      prod.get(), "O3_column_number_density", "molec/m2", 1,
                      dim_time, "altitude", "m", empty_colloc.get(), &out));

    AddDoubleVar(empty_colloc.get(), "altitude_bounds",
                 {harp_dimension_time, harp_dimension_vertical,
                  harp_dimension_independent},
                 {2, 2, 2}, "m",
                 {0.0, 1000.0, 1000.0, 2000.0, 0.0, 1000.0, 1000.0, 2000.0});
    EXPECT_EQ(-1, harp_product_get_smoothed_column_using_collocated_product(
                      prod.get(), "O3_column_number_density", "molec/m2", 1,
                      dim_time, "altitude", "m", empty_colloc.get(), &out));

    // filter_by_index failure and get_smoothed_column failure.
    ScopedHarpProduct prod_mismatch =
        MakeSourceProfileProduct("a", false, {0, 99});
    EXPECT_EQ(
        -1, harp_product_get_smoothed_column_using_collocated_product(
                prod_mismatch.get(), "O3_column_number_density", "molec/m2", 1,
                dim_time, "altitude", "m", colloc.get(), &out));
    AddDoubleVar(colloc.get(), "CO_column_number_density_avk",
                 {harp_dimension_time, harp_dimension_vertical}, {2, 2}, "",
                 {0.5, 0.5, 0.5, 0.5});
    EXPECT_EQ(-1, harp_product_get_smoothed_column_using_collocated_product(
                      prod.get(), "CO_column_number_density", "molec/m2", 1,
                      dim_time, "altitude", "m", colloc.get(), &out));
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 2. Success with and without apriori.
  for (bool with_apriori : {true, false}) {
    ScopedHarpProduct prod = MakeSourceProfileProduct("a", false, {0, 1});
    ScopedHarpProduct colloc =
        MakeCollocatedProduct("b", with_apriori, false, {0, 1});
    harp_variable* raw_out = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_get_smoothed_column_using_collocated_product(
                  prod.get(), "O3_column_number_density", "molec/m2", 1,
                  dim_time, "altitude", "m", colloc.get(), &raw_out));
    ScopedHarpVariable out(raw_out);
    EXPECT_DOUBLE_EQ(with_apriori ? 18.0 : 15.0, out->data.double_data[0]);
  }
}

TEST(HarpVerticalProfilesTest, SmoothAndSmoothedColumnUsingCollocatedDataset) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  TempFile b1_file("vp_b1.nc");
  TempFile b2_file("vp_b2.nc");
  TempFile b3_file("vp_b3.nc");
  TempFile b_min_file("vp_b_min.nc");

  // Write netCDF files to disk for dataset_b:
  // - b1 has 1 sample (index 0) with apriori + bad_layout_avk +
  // CO_column_..._avk
  // - b2 has 1 sample (index 0) unused in matched pairs (exercises NULL skip)
  // - b3 has 1 sample (index 0) with apriori (exercises harp_product_append)
  {
    ScopedHarpProduct b1 = MakeCollocatedProduct("b1", true, true, {0});
    AddDoubleVar(
        b1.get(), "bad_layout_avk",
        {harp_dimension_time, harp_dimension_vertical, harp_dimension_vertical},
        {1, 2, 2}, "", {1, 0, 0, 1});
    AddDoubleVar(b1.get(), "CO_column_number_density_avk",
                 {harp_dimension_time, harp_dimension_vertical}, {1, 2}, "",
                 {0.5, 0.5});
    ASSERT_EQ(HARP_SUCCESS, harp_export(b1_file.c_str(), "netcdf", b1.get()));
    ScopedHarpProduct b2 = MakeCollocatedProduct("b2", false, true, {0});
    ASSERT_EQ(HARP_SUCCESS, harp_export(b2_file.c_str(), "netcdf", b2.get()));
    ScopedHarpProduct b3 = MakeCollocatedProduct("b3", true, true, {0});
    AddDoubleVar(
        b3.get(), "bad_layout_avk",
        {harp_dimension_time, harp_dimension_vertical, harp_dimension_vertical},
        {1, 2, 2}, "", {1, 0, 0, 1});
    AddDoubleVar(b3.get(), "CO_column_number_density_avk",
                 {harp_dimension_time, harp_dimension_vertical}, {1, 2}, "",
                 {0.5, 0.5});
    ASSERT_EQ(HARP_SUCCESS, harp_export(b3_file.c_str(), "netcdf", b3.get()));

    // Minimal product with only index for missing-axis/bounds/avk error paths.
    harp_product* raw_min = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_min));
    ScopedHarpProduct b_min(raw_min);
    ASSERT_EQ(HARP_SUCCESS, harp_product_set_source_product(b_min.get(), "bm"));
    AddInt32Var(b_min.get(), "index", {harp_dimension_time}, {1}, {0});
    ASSERT_EQ(HARP_SUCCESS,
              harp_export(b_min_file.c_str(), "netcdf", b_min.get()));
  }

  harp_collocation_result* raw_colloc = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_new(&raw_colloc, 0, nullptr, nullptr));
  ScopedHarpCollocationResult colloc(raw_colloc);

  ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_add_pair(
                              colloc.get(), 0, "a", 0, "b1", 0, 0, nullptr));
  ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_add_pair(
                              colloc.get(), 1, "a", 1, "b3", 0, 0, nullptr));
  // Ensure b1, b2, and b3 have metadata pointing to their netCDF files on disk.
  ASSERT_EQ(HARP_SUCCESS,
            harp_dataset_import(colloc->dataset_b, b1_file.c_str(), nullptr));
  ASSERT_EQ(HARP_SUCCESS,
            harp_dataset_import(colloc->dataset_b, b2_file.c_str(), nullptr));
  ASSERT_EQ(HARP_SUCCESS,
            harp_dataset_import(colloc->dataset_b, b3_file.c_str(), nullptr));

  const char* smooth_vars[] = {"O3_number_density"};
  const harp_dimension_type dim_time[] = {harp_dimension_time};

  // 1. Validation errors for both dataset functions.
  {
    harp_product* raw_nv = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_nv));
    ScopedHarpProduct no_vert(raw_nv);
    EXPECT_EQ(
        -1, harp_product_smooth_vertical_with_collocated_dataset(
                no_vert.get(), 1, smooth_vars, "altitude", "m", colloc.get()));

    harp_variable* out = nullptr;
    EXPECT_EQ(-1, harp_product_get_smoothed_column_using_collocated_dataset(
                      no_vert.get(), "O3_column_number_density", "molec/m2", 0,
                      dim_time, "altitude", "m", colloc.get(), &out));
    EXPECT_EQ(-1, harp_product_get_smoothed_column_using_collocated_dataset(
                      no_vert.get(), "O3_column_number_density", "molec/m2",
                      HARP_NUM_DIM_TYPES, dim_time, "altitude", "m",
                      colloc.get(), &out));
    EXPECT_EQ(-1, harp_product_get_smoothed_column_using_collocated_dataset(
                      no_vert.get(), "O3_column_number_density", "molec/m2", 1,
                      dim_time, "altitude", "m", colloc.get(), &out));

    // Product missing smooth variable.
    ScopedHarpProduct prod_no_var =
        MakeSourceProfileProduct("a", false, {0, 1});
    const char* missing[] = {"missing_var"};
    EXPECT_EQ(
        -1, harp_product_smooth_vertical_with_collocated_dataset(
                prod_no_var.get(), 1, missing, "altitude", "m", colloc.get()));

    // Product missing collocation_index (safe here: no temp_product leaked).
    ScopedHarpProduct prod_no_cidx = MakeSourceProfileProduct("a", false, {});
    EXPECT_EQ(-1, harp_product_smooth_vertical_with_collocated_dataset(
                      prod_no_cidx.get(), 1, smooth_vars, "altitude", "m",
                      colloc.get()));
    EXPECT_EQ(-1,
              harp_product_get_smoothed_column_using_collocated_dataset(
                  prod_no_cidx.get(), "O3_column_number_density", "molec/m2", 1,
                  dim_time, "altitude", "m", colloc.get(), &out));

    // Inconsistent collocation_index count vs collocation_result pairs.
    ScopedHarpProduct prod_mismatch =
        MakeSourceProfileProduct("a", false, {0, 99});
    EXPECT_EQ(-1, harp_product_smooth_vertical_with_collocated_dataset(
                      prod_mismatch.get(), 1, smooth_vars, "altitude", "m",
                      colloc.get()));
    EXPECT_EQ(
        -1, harp_product_get_smoothed_column_using_collocated_dataset(
                prod_mismatch.get(), "O3_column_number_density", "molec/m2", 1,
                dim_time, "altitude", "m", colloc.get(), &out));

    // Regrid failure via invalid bounds independent dimension (!= 2) on
    // b_ov_file.
    {
      TempFile b_ov_file("vp_b_ov.nc");
      ScopedHarpProduct bm_overlap =
          MakeCollocatedProduct("bm", false, true, {0});
      ASSERT_EQ(HARP_SUCCESS, harp_product_remove_variable_by_name(
                                  bm_overlap.get(), "altitude_bounds"));
      AddDoubleVar(bm_overlap.get(), "altitude_bounds",
                   {harp_dimension_time, harp_dimension_vertical,
                    harp_dimension_independent},
                   {1, 2, 3}, "m", {0.0, 1.0, 2.0, 3.0, 4.0, 5.0});
      ASSERT_EQ(HARP_SUCCESS,
                harp_export(b_ov_file.c_str(), "netcdf", bm_overlap.get()));
      harp_collocation_result* raw_ov = nullptr;
      ASSERT_EQ(HARP_SUCCESS,
                harp_collocation_result_new(&raw_ov, 0, nullptr, nullptr));
      ScopedHarpCollocationResult colloc_ov(raw_ov);
      ASSERT_EQ(HARP_SUCCESS,
                harp_collocation_result_add_pair(colloc_ov.get(), 0, "a", 0,
                                                 "bm", 0, 0, nullptr));
      ASSERT_EQ(HARP_SUCCESS,
                harp_collocation_result_add_pair(colloc_ov.get(), 1, "a", 1,
                                                 "bm", 0, 0, nullptr));
      ASSERT_EQ(HARP_SUCCESS, harp_dataset_import(colloc_ov->dataset_b,
                                                  b_ov_file.c_str(), nullptr));
      ScopedHarpProduct prod_ov = MakeSourceProfileProduct("a", false, {0, 1});
      EXPECT_EQ(-1, harp_product_smooth_vertical_with_collocated_dataset(
                        prod_ov.get(), 1, smooth_vars, "altitude", "m",
                        colloc_ov.get()));
      harp_variable* out_ov = nullptr;
      EXPECT_EQ(-1,
                harp_product_get_smoothed_column_using_collocated_dataset(
                    prod_ov.get(), "O3_column_number_density", "molec/m2", 1,
                    dim_time, "altitude", "m", colloc_ov.get(), &out_ov));
      ScopedHarpVariable guard_ov(out_ov);
    }

    // Variable smoothing failure inside dataset function + get_smoothed_column
    // failure inside dataset function.
    ScopedHarpProduct prod_bad = MakeSourceProfileProduct("a", false, {0, 1});
    AddDoubleVar(prod_bad.get(), "bad_layout",
                 {harp_dimension_time, harp_dimension_vertical,
                  harp_dimension_independent},
                 {2, 2, 2}, "molec/m3",
                 {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0});
    const char* bad_vars[] = {"bad_layout"};
    EXPECT_EQ(-1,
              harp_product_smooth_vertical_with_collocated_dataset(
                  prod_bad.get(), 1, bad_vars, "altitude", "m", colloc.get()));
    EXPECT_EQ(-1, harp_product_get_smoothed_column_using_collocated_dataset(
                      prod_bad.get(), "CO_column_number_density", "molec/m2", 1,
                      dim_time, "altitude", "m", colloc.get(), &out));

    // Missing metadata in dataset_b -> get_filtered_product_b failure.
    harp_collocation_result* raw_nometa = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_collocation_result_new(&raw_nometa, 0, nullptr, nullptr));
    ScopedHarpCollocationResult colloc_nometa(raw_nometa);
    ASSERT_EQ(HARP_SUCCESS,
              harp_collocation_result_add_pair(colloc_nometa.get(), 0, "a", 0,
                                               "missing_b", 0, 0, nullptr));
    ASSERT_EQ(HARP_SUCCESS,
              harp_collocation_result_add_pair(colloc_nometa.get(), 1, "a", 1,
                                               "missing_b", 0, 0, nullptr));
    ScopedHarpProduct prod1 = MakeSourceProfileProduct("a", false, {0, 1});
    EXPECT_EQ(-1, harp_product_smooth_vertical_with_collocated_dataset(
                      prod1.get(), 1, smooth_vars, "altitude", "m",
                      colloc_nometa.get()));
    EXPECT_EQ(-1, harp_product_get_smoothed_column_using_collocated_dataset(
                      prod1.get(), "O3_column_number_density", "molec/m2", 1,
                      dim_time, "altitude", "m", colloc_nometa.get(), &out));

    // Minimal collocated file on disk missing altitude / bounds / avk.
    harp_collocation_result* raw_min_c2 = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_collocation_result_new(&raw_min_c2, 0, nullptr, nullptr));
    ScopedHarpCollocationResult colloc_min2(raw_min_c2);
    ASSERT_EQ(HARP_SUCCESS,
              harp_collocation_result_add_pair(colloc_min2.get(), 0, "a", 0,
                                               "bm", 0, 0, nullptr));
    ASSERT_EQ(HARP_SUCCESS,
              harp_collocation_result_add_pair(colloc_min2.get(), 1, "a", 1,
                                               "bm", 0, 0, nullptr));
    ASSERT_EQ(HARP_SUCCESS, harp_dataset_import(colloc_min2->dataset_b,
                                                b_min_file.c_str(), nullptr));
    EXPECT_EQ(-1, harp_product_smooth_vertical_with_collocated_dataset(
                      prod1.get(), 1, smooth_vars, "altitude", "m",
                      colloc_min2.get()));
    EXPECT_EQ(-1, harp_product_get_smoothed_column_using_collocated_dataset(
                      prod1.get(), "O3_column_number_density", "molec/m2", 1,
                      dim_time, "altitude", "m", colloc_min2.get(), &out));
    // Re-export b_min with 1-level altitude (cannot derive altitude_bounds),
    // then 2-level altitude+bounds (missing avk), plus mismatched append.
    {
      harp_product* raw_bm1 = nullptr;
      ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_bm1));
      ScopedHarpProduct bm1(raw_bm1);
      ASSERT_EQ(HARP_SUCCESS, harp_product_set_source_product(bm1.get(), "bm"));
      AddInt32Var(bm1.get(), "index", {harp_dimension_time}, {1}, {0});
      AddDoubleVar(bm1.get(), "altitude",
                   {harp_dimension_time, harp_dimension_vertical}, {1, 1}, "m",
                   {500.0});
      ASSERT_EQ(HARP_SUCCESS,
                harp_export(b_min_file.c_str(), "netcdf", bm1.get()));
      EXPECT_EQ(-1, harp_product_smooth_vertical_with_collocated_dataset(
                        prod1.get(), 1, smooth_vars, "altitude", "m",
                        colloc_min2.get()));
      EXPECT_EQ(-1, harp_product_get_smoothed_column_using_collocated_dataset(
                        prod1.get(), "O3_column_number_density", "molec/m2", 1,
                        dim_time, "altitude", "m", colloc_min2.get(), &out));

      harp_product* raw_bm = nullptr;
      ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_bm));
      ScopedHarpProduct bm(raw_bm);
      ASSERT_EQ(HARP_SUCCESS, harp_product_set_source_product(bm.get(), "bm"));
      AddInt32Var(bm.get(), "index", {harp_dimension_time}, {1}, {0});
      AddDoubleVar(bm.get(), "altitude",
                   {harp_dimension_time, harp_dimension_vertical}, {1, 2}, "m",
                   {500.0, 1500.0});
      AddDoubleVar(bm.get(), "altitude_bounds",
                   {harp_dimension_time, harp_dimension_vertical,
                    harp_dimension_independent},
                   {1, 2, 2}, "m", {0.0, 1000.0, 1000.0, 2000.0});
      ASSERT_EQ(HARP_SUCCESS,
                harp_export(b_min_file.c_str(), "netcdf", bm.get()));
      EXPECT_EQ(-1, harp_product_smooth_vertical_with_collocated_dataset(
                        prod1.get(), 1, smooth_vars, "altitude", "m",
                        colloc_min2.get()));
      EXPECT_EQ(-1, harp_product_get_smoothed_column_using_collocated_dataset(
                        prod1.get(), "O3_column_number_density", "molec/m2", 1,
                        dim_time, "altitude", "m", colloc_min2.get(), &out));

      // Mismatched schemas between b1 (with apriori) and b2 (without apriori)
      // trigger harp_product_append failure.
      harp_collocation_result* raw_app = nullptr;
      ASSERT_EQ(HARP_SUCCESS,
                harp_collocation_result_new(&raw_app, 0, nullptr, nullptr));
      ScopedHarpCollocationResult colloc_app(raw_app);
      ASSERT_EQ(HARP_SUCCESS,
                harp_collocation_result_add_pair(colloc_app.get(), 0, "a", 0,
                                                 "b1", 0, 0, nullptr));
      ASSERT_EQ(HARP_SUCCESS,
                harp_collocation_result_add_pair(colloc_app.get(), 1, "a", 1,
                                                 "b2", 0, 0, nullptr));
      ASSERT_EQ(HARP_SUCCESS, harp_dataset_import(colloc_app->dataset_b,
                                                  b1_file.c_str(), nullptr));
      ASSERT_EQ(HARP_SUCCESS, harp_dataset_import(colloc_app->dataset_b,
                                                  b2_file.c_str(), nullptr));
      EXPECT_EQ(-1, harp_product_smooth_vertical_with_collocated_dataset(
                        prod1.get(), 1, smooth_vars, "altitude", "m",
                        colloc_app.get()));
      EXPECT_EQ(-1, harp_product_get_smoothed_column_using_collocated_dataset(
                        prod1.get(), "O3_column_number_density", "molec/m2", 1,
                        dim_time, "altitude", "m", colloc_app.get(), &out));
    }
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 2. Success for harp_product_smooth_vertical_with_collocated_dataset.
  {
    ScopedHarpProduct prod = MakeSourceProfileProduct("a", false, {0, 1});
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_smooth_vertical_with_collocated_dataset(
                  prod.get(), 1, smooth_vars, "altitude", "m", colloc.get()));
    harp_variable* o3 = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                                prod.get(), "O3_number_density", &o3));
    EXPECT_TRUE(std::isfinite(o3->data.double_data[0]));
  }

  // 3. Success for harp_product_get_smoothed_column_using_collocated_dataset.
  {
    ScopedHarpProduct prod = MakeSourceProfileProduct("a", false, {0, 1});
    harp_variable* raw_out = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_get_smoothed_column_using_collocated_dataset(
                  prod.get(), "O3_column_number_density", "molec/m2", 1,
                  dim_time, "altitude", "m", colloc.get(), &raw_out));
    ScopedHarpVariable out(raw_out);
    EXPECT_DOUBLE_EQ(18.0, out->data.double_data[0]);
    EXPECT_DOUBLE_EQ(38.0, out->data.double_data[1]);
  }
}

}  // namespace
}  // namespace stcorp_harp
