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
// Unit tests for CALIPSO L2 ingestion modules
// (libharp/harp-ingest-calipso_l2.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-calipso_l2.c

#include <cmath>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/stcorp_harp/google/util/coda_mem_product.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/ingest_test_util.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::DoubleNear;
using ::testing::ElementsAre;

constexpr double kSeconds1993To2000 = 220838400.0;
constexpr double kCelsiusToKelvin = 273.15;

// Populates common time-specific and optical-depth fields for CALIPSO L2.
// When `extra_dim == 1` and `use_1d_time_fields` is true, `/Latitude`,
// `/Longitude`, and `/Profile_Time` are 1D `[num_times]`; otherwise 2D
// `[num_times, extra_dim]`.
void PopulateCommonFields(CodaMemProduct& prod, long num_times, long extra_dim,
                          bool use_1d_time_fields, int wavelength) {
  const std::vector<long> time_dims =
      use_1d_time_fields ? std::vector<long>{num_times}
                         : std::vector<long>{num_times, extra_dim};
  const std::vector<long> t1_dims = {num_times};

  std::vector<double> profile_times;
  std::vector<double> latitudes;
  std::vector<double> longitudes;
  for (long i = 0; i < num_times; ++i) {
    for (long j = 0; j < extra_dim; ++j) {
      profile_times.push_back(kSeconds1993To2000 + 100.0 * i + 10.0 * j);
      latitudes.push_back(10.0 + i + 0.5 * j);
      longitudes.push_back(-20.0 + i + 0.5 * j);
    }
  }
  ASSERT_TRUE(prod.AddField(prod.root(), "Profile_Time",
                            prod.MakeDoubleArray(time_dims, profile_times)));
  ASSERT_TRUE(prod.AddField(prod.root(), "Latitude",
                            prod.MakeDoubleArray(time_dims, latitudes)));
  ASSERT_TRUE(prod.AddField(prod.root(), "Longitude",
                            prod.MakeDoubleArray(time_dims, longitudes)));

  std::vector<double> trop_height(num_times, 12.5);
  trop_height[0] = -9999.0;  // Triggers fill-value replacement -> NaN.
  ASSERT_TRUE(prod.AddField(prod.root(), "Tropopause_Height",
                            prod.MakeDoubleArray(t1_dims, trop_height)));

  const std::string wl = std::to_string(wavelength);
  std::vector<double> od_vals(num_times, 0.25);
  std::vector<double> od_unc_vals(num_times, 0.02);
  ASSERT_TRUE(prod.AddField(prod.root(),
                            "Column_Optical_Depth_Tropospheric_Aerosols_" + wl,
                            prod.MakeDoubleArray(t1_dims, od_vals)));
  ASSERT_TRUE(prod.AddField(
      prod.root(),
      "Column_Optical_Depth_Tropospheric_Aerosols_Uncertainty_" + wl,
      prod.MakeDoubleArray(t1_dims, od_unc_vals)));
  ASSERT_TRUE(prod.AddField(prod.root(),
                            "Column_Optical_Depth_Stratospheric_Aerosols_" + wl,
                            prod.MakeDoubleArray(t1_dims, od_vals)));
  ASSERT_TRUE(prod.AddField(
      prod.root(),
      "Column_Optical_Depth_Stratospheric_Aerosols_Uncertainty_" + wl,
      prod.MakeDoubleArray(t1_dims, od_unc_vals)));

  // Cloud optical depth always uses 532 nm.
  ASSERT_TRUE(prod.AddField(prod.root(), "Column_Optical_Depth_Cloud_532",
                            prod.MakeDoubleArray(t1_dims, od_vals)));
  ASSERT_TRUE(prod.AddField(prod.root(),
                            "Column_Optical_Depth_Cloud_Uncertainty_532",
                            prod.MakeDoubleArray(t1_dims, od_unc_vals)));
}

void PopulateLayerFields(CodaMemProduct& prod, long num_times,
                         long num_altitudes) {
  const std::vector<long> t1_dims = {num_times};
  const std::vector<long> layer_dims = {num_times, num_altitudes};

  std::vector<double> sza(num_times, 30.0);
  std::vector<double> saa(num_times, 120.0);
  std::vector<double> off_nadir(num_times, 0.3);
  std::vector<double> scatter(num_times, 150.0);
  ASSERT_TRUE(prod.AddField(prod.root(), "Solar_Zenith_Angle",
                            prod.MakeDoubleArray(t1_dims, sza)));
  ASSERT_TRUE(prod.AddField(prod.root(), "Solar_Azimuth_Angle",
                            prod.MakeDoubleArray(t1_dims, saa)));
  ASSERT_TRUE(prod.AddField(prod.root(), "Off_Nadir_Angle",
                            prod.MakeDoubleArray(t1_dims, off_nadir)));
  ASSERT_TRUE(prod.AddField(prod.root(), "Scattering_Angle",
                            prod.MakeDoubleArray(t1_dims, scatter)));

  // Row 0 has a -9999.0 slot (replaced with NaN) so the valid layers shift
  // and trailing slots get padded with NaN when reversed.
  ASSERT_EQ(2, num_times);
  ASSERT_EQ(3, num_altitudes);
  const double base_alt[] = {5.0, -9999.0, 1.0, 6.0, 4.0, 2.0};
  const double top_alt[] = {6.0, -9999.0, 2.0, 7.0, 5.0, 3.0};
  const double mid_pres[] = {500.0, -9999.0, 900.0, 400.0, 600.0, 800.0};
  const double base_pres[] = {550.0, -9999.0, 950.0, 450.0, 650.0, 850.0};
  const double top_pres[] = {450.0, -9999.0, 850.0, 350.0, 550.0, 750.0};
  const double mid_temp[] = {-20.0, -9999.0, 10.0, -30.0, -10.0, 5.0};

  ASSERT_TRUE(prod.AddField(prod.root(), "Layer_Base_Altitude",
                            prod.MakeDoubleArray(layer_dims, base_alt)));
  ASSERT_TRUE(prod.AddField(prod.root(), "Layer_Top_Altitude",
                            prod.MakeDoubleArray(layer_dims, top_alt)));
  ASSERT_TRUE(prod.AddField(prod.root(), "Midlayer_Pressure",
                            prod.MakeDoubleArray(layer_dims, mid_pres)));
  ASSERT_TRUE(prod.AddField(prod.root(), "Layer_Base_Pressure",
                            prod.MakeDoubleArray(layer_dims, base_pres)));
  ASSERT_TRUE(prod.AddField(prod.root(), "Layer_Top_Pressure",
                            prod.MakeDoubleArray(layer_dims, top_pres)));
  ASSERT_TRUE(prod.AddField(prod.root(), "Midlayer_Temperature",
                            prod.MakeDoubleArray(layer_dims, mid_temp)));
}

void PopulateProfileFields(CodaMemProduct& prod, long num_times,
                           long num_altitudes, int wavelength) {
  const std::vector<long> alt_dims = {num_altitudes};
  const std::vector<long> prof_dims = {num_times, num_altitudes};

  CodaMemNode* meta = prod.MakeRecord();
  ASSERT_NE(nullptr, meta);
  std::vector<double> lidar_alts(num_altitudes);
  for (long i = 0; i < num_altitudes; ++i) {
    lidar_alts[i] = 1.0 + static_cast<double>(i);
  }
  ASSERT_TRUE(prod.AddField(meta, "Lidar_Data_Altitudes",
                            prod.MakeDoubleArray(alt_dims, lidar_alts)));
  ASSERT_TRUE(prod.AddField(prod.root(), "metadata", meta));

  std::vector<double> pressure(num_times * num_altitudes, 800.0);
  std::vector<double> temperature(num_times * num_altitudes, 15.0);
  ASSERT_TRUE(prod.AddField(prod.root(), "Pressure",
                            prod.MakeDoubleArray(prof_dims, pressure)));
  ASSERT_TRUE(prod.AddField(prod.root(), "Temperature",
                            prod.MakeDoubleArray(prof_dims, temperature)));

  std::vector<double> ext(num_times * num_altitudes, 0.12);
  std::vector<double> ext_unc(num_times * num_altitudes, 0.01);
  std::vector<double> back(num_times * num_altitudes, 0.03);
  std::vector<double> back_unc(num_times * num_altitudes, 0.004);

  // init_dimensions always checks /Extinction_Coefficient_532 for profile size.
  ASSERT_TRUE(prod.AddField(prod.root(), "Extinction_Coefficient_532",
                            prod.MakeDoubleArray(prof_dims, ext)));
  if (wavelength == 1064) {
    ASSERT_TRUE(prod.AddField(prod.root(), "Extinction_Coefficient_1064",
                              prod.MakeDoubleArray(prof_dims, ext)));
    ASSERT_TRUE(prod.AddField(prod.root(),
                              "Extinction_Coefficient_Uncertainty_1064",
                              prod.MakeDoubleArray(prof_dims, ext_unc)));
    ASSERT_TRUE(prod.AddField(prod.root(), "Backscatter_Coefficient_1064",
                              prod.MakeDoubleArray(prof_dims, back)));
    ASSERT_TRUE(prod.AddField(prod.root(),
                              "Backscatter_Coefficient_Uncertainty_1064",
                              prod.MakeDoubleArray(prof_dims, back_unc)));
  } else {
    ASSERT_TRUE(prod.AddField(prod.root(),
                              "Extinction_Coefficient_Uncertainty_532",
                              prod.MakeDoubleArray(prof_dims, ext_unc)));
    ASSERT_TRUE(prod.AddField(prod.root(), "Total_Backscatter_Coefficient_532",
                              prod.MakeDoubleArray(prof_dims, back)));
    ASSERT_TRUE(prod.AddField(prod.root(),
                              "Total_Backscatter_Coefficient_Uncertainty_532",
                              prod.MakeDoubleArray(prof_dims, back_unc)));
  }
}

TEST(HarpIngestCalipsoL2Test, AerosolLayerOddExtraDimDefault532) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod =
      FindIngestionModule("CALIPSO_L2_CAL_LID_ALay");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateCommonFields(prod, /*num_times=*/2, /*extra_dim=*/3,
                       /*use_1d_time_fields=*/false, /*wavelength=*/532);
  PopulateLayerFields(prod, /*num_times=*/2, /*num_altitudes=*/3);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(3, res.dimensions[harp_dimension_vertical]);

  // extra_dim == 3 (odd): median is index 1 (offset +10s), start is 0, stop 20.
  EXPECT_THAT(res.variables["datetime_start"].double_values,
              ElementsAre(0.0, 100.0));
  EXPECT_THAT(res.variables["datetime"].double_values,
              ElementsAre(10.0, 110.0));
  EXPECT_THAT(res.variables["datetime_stop"].double_values,
              ElementsAre(20.0, 120.0));
  EXPECT_THAT(res.variables["latitude"].double_values, ElementsAre(10.5, 11.5));
  EXPECT_THAT(res.variables["longitude"].double_values,
              ElementsAre(-19.5, -18.5));
  EXPECT_TRUE(
      std::isnan(res.variables["tropopause_altitude"].double_values[0]));
  EXPECT_DOUBLE_EQ(12.5, res.variables["tropopause_altitude"].double_values[1]);

  EXPECT_THAT(res.variables["solar_zenith_angle"].double_values,
              ElementsAre(30.0, 30.0));
  EXPECT_THAT(res.variables["solar_azimuth_angle"].double_values,
              ElementsAre(120.0, 120.0));
  EXPECT_THAT(res.variables["viewing_elevation_angle"].double_values,
              ElementsAre(0.3, 0.3));
  EXPECT_THAT(res.variables["scattering_angle"].double_values,
              ElementsAre(150.0, 150.0));

  // Row 0 reversed valid layers: [1..2] -> 1.5, [5..6] -> 5.5, padded NaN.
  const auto& alt = res.variables["altitude"].double_values;
  ASSERT_EQ(6u, alt.size());
  EXPECT_DOUBLE_EQ(1.5, alt[0]);
  EXPECT_DOUBLE_EQ(5.5, alt[1]);
  EXPECT_TRUE(std::isnan(alt[2]));
  EXPECT_DOUBLE_EQ(2.5, alt[3]);
  EXPECT_DOUBLE_EQ(4.5, alt[4]);
  EXPECT_DOUBLE_EQ(6.5, alt[5]);

  const auto& alt_b = res.variables["altitude_bounds"].double_values;
  ASSERT_EQ(12u, alt_b.size());
  EXPECT_DOUBLE_EQ(1.0, alt_b[0]);
  EXPECT_DOUBLE_EQ(2.0, alt_b[1]);
  EXPECT_DOUBLE_EQ(5.0, alt_b[2]);
  EXPECT_DOUBLE_EQ(6.0, alt_b[3]);
  EXPECT_TRUE(std::isnan(alt_b[4]));
  EXPECT_TRUE(std::isnan(alt_b[5]));

  const auto& pres = res.variables["pressure"].double_values;
  EXPECT_DOUBLE_EQ(900.0, pres[0]);
  EXPECT_DOUBLE_EQ(500.0, pres[1]);
  EXPECT_TRUE(std::isnan(pres[2]));

  const auto& pres_b = res.variables["pressure_bounds"].double_values;
  EXPECT_DOUBLE_EQ(950.0, pres_b[0]);
  EXPECT_DOUBLE_EQ(850.0, pres_b[1]);
  EXPECT_TRUE(std::isnan(pres_b[4]));
  EXPECT_TRUE(std::isnan(pres_b[5]));

  const auto& temp = res.variables["temperature"].double_values;
  EXPECT_THAT(temp[0], DoubleNear(10.0 + kCelsiusToKelvin, 1e-9));
  EXPECT_THAT(temp[1], DoubleNear(-20.0 + kCelsiusToKelvin, 1e-9));
  EXPECT_TRUE(std::isnan(temp[2]));
}

TEST(HarpIngestCalipsoL2Test, CloudLayerEvenExtraDimAndExplicit532) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod =
      FindIngestionModule("CALIPSO_L2_CAL_LID_CLay");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateCommonFields(prod, /*num_times=*/2, /*extra_dim=*/2,
                       /*use_1d_time_fields=*/false, /*wavelength=*/532);
  PopulateLayerFields(prod, /*num_times=*/2, /*num_altitudes=*/3);

  IngestExerciseResult res =
      ExerciseIngestionModule(mod, prod.product(), "wavelength=532");
  ASSERT_EQ(HARP_SUCCESS, res.status);
  // extra_dim == 2 (even): median is avg of index 0 and 1 (+5s, +0.25 deg).
  EXPECT_THAT(res.variables["datetime"].double_values, ElementsAre(5.0, 105.0));
  EXPECT_THAT(res.variables["latitude"].double_values,
              ElementsAre(10.25, 11.25));
  EXPECT_THAT(res.variables["longitude"].double_values,
              ElementsAre(-19.75, -18.75));
}

TEST(HarpIngestCalipsoL2Test, AerosolProfile1DTimeAndWavelength1064) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod =
      FindIngestionModule("CALIPSO_L2_CAL_LID_APro");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateCommonFields(prod, /*num_times=*/2, /*extra_dim=*/1,
                       /*use_1d_time_fields=*/true, /*wavelength=*/1064);
  PopulateProfileFields(prod, /*num_times=*/2, /*num_altitudes=*/2,
                        /*wavelength=*/1064);

  IngestExerciseResult res =
      ExerciseIngestionModule(mod, prod.product(), "wavelength=1064");
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(2, res.dimensions[harp_dimension_vertical]);
  EXPECT_THAT(res.variables["altitude"].double_values, ElementsAre(1.0, 2.0));
  EXPECT_THAT(res.variables["pressure"].double_values,
              ElementsAre(800.0, 800.0, 800.0, 800.0));
  EXPECT_THAT(res.variables["temperature"].double_values,
              ElementsAre(15.0 + kCelsiusToKelvin, 15.0 + kCelsiusToKelvin,
                          15.0 + kCelsiusToKelvin, 15.0 + kCelsiusToKelvin));
  EXPECT_THAT(res.variables["aerosol_extinction_coefficient"].double_values,
              ElementsAre(0.12, 0.12, 0.12, 0.12));
  EXPECT_THAT(
      res.variables["aerosol_extinction_coefficient_uncertainty"].double_values,
      ElementsAre(0.01, 0.01, 0.01, 0.01));
  EXPECT_THAT(res.variables["backscatter_coefficient"].double_values,
              ElementsAre(0.03, 0.03, 0.03, 0.03));
  EXPECT_THAT(
      res.variables["backscatter_coefficient_uncertainty"].double_values,
      ElementsAre(0.004, 0.004, 0.004, 0.004));
  EXPECT_THAT(res.variables["tropospheric_aerosol_optical_depth"].double_values,
              ElementsAre(0.25, 0.25));
  EXPECT_THAT(res.variables["cloud_optical_depth"].double_values,
              ElementsAre(0.25, 0.25));
}

TEST(HarpIngestCalipsoL2Test, CloudProfile532) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod =
      FindIngestionModule("CALIPSO_L2_CAL_LID_CPro");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateCommonFields(prod, /*num_times=*/2, /*extra_dim=*/1,
                       /*use_1d_time_fields=*/true, /*wavelength=*/532);
  PopulateProfileFields(prod, /*num_times=*/2, /*num_altitudes=*/2,
                        /*wavelength=*/532);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_THAT(res.variables["extinction_coefficient"].double_values,
              ElementsAre(0.12, 0.12, 0.12, 0.12));
  EXPECT_THAT(res.variables["extinction_coefficient_uncertainty"].double_values,
              ElementsAre(0.01, 0.01, 0.01, 0.01));
  EXPECT_THAT(res.variables["backscatter_coefficient"].double_values,
              ElementsAre(0.03, 0.03, 0.03, 0.03));
  EXPECT_THAT(
      res.variables["backscatter_coefficient_uncertainty"].double_values,
      ElementsAre(0.004, 0.004, 0.004, 0.004));
}

TEST(HarpIngestCalipsoL2Test, InitDimensionAndOptionErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* alay =
      FindIngestionModule("CALIPSO_L2_CAL_LID_ALay");
  const harp_ingestion_module* apro =
      FindIngestionModule("CALIPSO_L2_CAL_LID_APro");
  ASSERT_NE(nullptr, alay);
  ASSERT_NE(nullptr, apro);

  // Invalid wavelength option.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res =
        ExerciseIngestionModule(alay, prod.product(), "wavelength=999");
    EXPECT_EQ(-1, res.status);
  }

  // Missing /Latitude.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(alay, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // Scalar /Latitude fails coda_cursor_get_array_dim.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "Latitude", prod.MakeDouble(1.0)));
    IngestExerciseResult res = ExerciseIngestionModule(alay, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // Missing /Layer_Base_Altitude (layer module) or scalar /Layer_Base_Altitude.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {2};
    constexpr double kLats[] = {1.0, 2.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "Latitude",
                              prod.MakeDoubleArray(kDims, kLats)));
    IngestExerciseResult res = ExerciseIngestionModule(alay, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);

    ASSERT_TRUE(prod.AddField(prod.root(), "Layer_Base_Altitude",
                              prod.MakeDouble(1.0)));
    res = ExerciseIngestionModule(alay, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // Missing /Extinction_Coefficient_532 (profile) or scalar.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {2};
    constexpr double kLats[] = {1.0, 2.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "Latitude",
                              prod.MakeDoubleArray(kDims, kLats)));
    IngestExerciseResult res = ExerciseIngestionModule(apro, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);

    ASSERT_TRUE(prod.AddField(prod.root(), "Extinction_Coefficient_532",
                              prod.MakeDouble(1.0)));
    res = ExerciseIngestionModule(apro, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
}

TEST(HarpIngestCalipsoL2Test, ReadDatasetAndCompoundReaderErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* alay =
      FindIngestionModule("CALIPSO_L2_CAL_LID_ALay");
  const harp_ingestion_module* apro =
      FindIngestionModule("CALIPSO_L2_CAL_LID_APro");
  ASSERT_NE(nullptr, alay);
  ASSERT_NE(nullptr, apro);

  // Layer product with missing / wrong-length / wrong-type datasets.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kTimeDims[] = {2};
    constexpr double kLats[] = {1.0, 2.0};
    constexpr long kLayerDims[] = {2, 2};
    constexpr double kLayerVals[] = {1.0, 2.0, 3.0, 4.0};
    constexpr std::string_view kBadStrs[] = {"a", "b"};

    ASSERT_TRUE(prod.AddField(prod.root(), "Latitude",
                              prod.MakeDoubleArray(kTimeDims, kLats)));
    // Wrong-type /Longitude (string array) -> coda_cursor_read_double_array.
    ASSERT_TRUE(prod.AddField(prod.root(), "Longitude",
                              prod.MakeStringArray(kTimeDims, kBadStrs)));
    // Wrong element count for /Tropopause_Height (1 instead of 2).
    constexpr long kOneDim[] = {1};
    constexpr double kOneVal[] = {10.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "Tropopause_Height",
                              prod.MakeDoubleArray(kOneDim, kOneVal)));
    // Present Layer_Base_Altitude, missing Layer_Top_Altitude, missing
    // Midlayer_Pressure, missing Layer_Base_Pressure, missing
    // Midlayer_Temperature.
    ASSERT_TRUE(prod.AddField(prod.root(), "Layer_Base_Altitude",
                              prod.MakeDoubleArray(kLayerDims, kLayerVals)));

    IngestExerciseResult res = ExerciseIngestionModule(
        alay, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_stop"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["tropopause_altitude"].read_status);
    EXPECT_EQ(-1, res.variables["altitude"].read_status);
    EXPECT_EQ(-1, res.variables["altitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["pressure"].read_status);
    EXPECT_EQ(-1, res.variables["pressure_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["temperature"].read_status);
  }

  // Missing /Layer_Base_Altitude or missing /Layer_Top_Pressure when base
  // succeeds.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kTimeDims[] = {2};
    constexpr double kLats[] = {1.0, 2.0};
    constexpr long kLayerDims[] = {2, 2};
    constexpr double kLayerVals[] = {1.0, 2.0, 3.0, 4.0};
    constexpr long kWrongLayerDims[] = {1};
    constexpr double kWrongLayerVals[] = {1.0};

    ASSERT_TRUE(prod.AddField(prod.root(), "Latitude",
                              prod.MakeDoubleArray(kTimeDims, kLats)));
    ASSERT_TRUE(prod.AddField(prod.root(), "Layer_Base_Altitude",
                              prod.MakeDoubleArray(kLayerDims, kLayerVals)));
    ASSERT_TRUE(prod.AddField(prod.root(), "Layer_Base_Pressure",
                              prod.MakeDoubleArray(kLayerDims, kLayerVals)));
    // Missing Layer_Top_Pressure.
    IngestExerciseResult res = ExerciseIngestionModule(
        alay, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["pressure_bounds"].read_status);

    // Wrong count on Layer_Base_Altitude during read_layer_altitude &
    // read_layer_altitude_bounds, and missing Temperature in profile module.
    CodaMemProduct prod2;
    ASSERT_TRUE(prod2.ok());
    ASSERT_TRUE(prod2.AddField(prod2.root(), "Latitude",
                               prod2.MakeDoubleArray(kTimeDims, kLats)));
    ASSERT_TRUE(prod2.AddField(
        prod2.root(), "Layer_Base_Altitude",
        prod2.MakeDoubleArray(kWrongLayerDims, kWrongLayerVals)));
    // Wait: init_dimensions needs 2D Layer_Base_Altitude, so test wrong
    //  Layer_Base_Pressure count instead.
    CodaMemProduct prod3;
    ASSERT_TRUE(prod3.ok());
    ASSERT_TRUE(prod3.AddField(prod3.root(), "Latitude",
                               prod3.MakeDoubleArray(kTimeDims, kLats)));
    ASSERT_TRUE(prod3.AddField(prod3.root(), "Layer_Base_Altitude",
                               prod3.MakeDoubleArray(kLayerDims, kLayerVals)));
    ASSERT_TRUE(prod3.AddField(
        prod3.root(), "Layer_Base_Pressure",
        prod3.MakeDoubleArray(kWrongLayerDims, kWrongLayerVals)));
    res =
        ExerciseIngestionModule(alay, prod3.product(), /*options_str=*/nullptr,
                                /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["pressure_bounds"].read_status);
  }

  // Profile module missing Temperature -> read_profile_temperature fails.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kTimeDims[] = {2};
    constexpr double kLats[] = {1.0, 2.0};
    constexpr long kProfDims[] = {2, 2};
    constexpr double kProfVals[] = {0.1, 0.2, 0.3, 0.4};
    ASSERT_TRUE(prod.AddField(prod.root(), "Latitude",
                              prod.MakeDoubleArray(kTimeDims, kLats)));
    ASSERT_TRUE(prod.AddField(prod.root(), "Extinction_Coefficient_532",
                              prod.MakeDoubleArray(kProfDims, kProfVals)));
    IngestExerciseResult res = ExerciseIngestionModule(
        apro, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["temperature"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
