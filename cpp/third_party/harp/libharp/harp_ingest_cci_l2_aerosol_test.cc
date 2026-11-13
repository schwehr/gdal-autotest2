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
// Unit tests for ESA CCI L2 Aerosol ingestion modules
// (libharp/harp-ingest-cci_l2_aerosol.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-cci_l2_aerosol.c

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/stcorp_coda/libcoda/coda.h"
#include "third_party/stcorp_harp/google/util/coda_mem_product.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/ingest_test_util.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::ElementsAre;

void PopulateCommonFields(CodaMemProduct& prod, long num_time,
                          bool include_corners) {
  const long dims[] = {num_time};
  // Include -999.0 fill value in latitude to exercise fill-value replacement.
  constexpr double kTime[] = {10957.5, 10958.0};  // days since 1970-01-01
  constexpr double kLat[] = {52.0, -999.0};
  constexpr double kLon[] = {4.0, 5.0};
  ASSERT_TRUE(
      prod.AddField(prod.root(), "time", prod.MakeDoubleArray(dims, kTime)));
  ASSERT_TRUE(
      prod.AddField(prod.root(), "latitude", prod.MakeDoubleArray(dims, kLat)));
  ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                            prod.MakeDoubleArray(dims, kLon)));

  if (include_corners) {
    constexpr double kCornerLat1[] = {51.5, 52.5};
    constexpr double kCornerLat2[] = {51.6, 52.6};
    constexpr double kCornerLat3[] = {52.6, 53.6};
    constexpr double kCornerLat4[] = {52.5, 53.5};
    constexpr double kCornerLon1[] = {3.5, 4.5};
    constexpr double kCornerLon2[] = {4.5, 5.5};
    constexpr double kCornerLon3[] = {4.6, 5.6};
    constexpr double kCornerLon4[] = {3.6, 4.6};
    ASSERT_TRUE(prod.AddField(prod.root(), "pixel_corner_latitude1",
                              prod.MakeDoubleArray(dims, kCornerLat1)));
    ASSERT_TRUE(prod.AddField(prod.root(), "pixel_corner_latitude2",
                              prod.MakeDoubleArray(dims, kCornerLat2)));
    ASSERT_TRUE(prod.AddField(prod.root(), "pixel_corner_latitude3",
                              prod.MakeDoubleArray(dims, kCornerLat3)));
    ASSERT_TRUE(prod.AddField(prod.root(), "pixel_corner_latitude4",
                              prod.MakeDoubleArray(dims, kCornerLat4)));
    ASSERT_TRUE(prod.AddField(prod.root(), "pixel_corner_longitude1",
                              prod.MakeDoubleArray(dims, kCornerLon1)));
    ASSERT_TRUE(prod.AddField(prod.root(), "pixel_corner_longitude2",
                              prod.MakeDoubleArray(dims, kCornerLon2)));
    ASSERT_TRUE(prod.AddField(prod.root(), "pixel_corner_longitude3",
                              prod.MakeDoubleArray(dims, kCornerLon3)));
    ASSERT_TRUE(prod.AddField(prod.root(), "pixel_corner_longitude4",
                              prod.MakeDoubleArray(dims, kCornerLon4)));
  }
}

TEST(HarpIngestCciL2AerosolTest, IngestAatsrAndAtsr2Success) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  constexpr std::string_view kModuleNames[] = {
      "ESACCI_AEROSOL_L2_AATSR",
      "ESACCI_AEROSOL_L2_ATSR2",
  };
  for (std::string_view mod_name : kModuleNames) {
    const harp_ingestion_module* mod = FindIngestionModule(mod_name);
    ASSERT_NE(nullptr, mod);

    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kNumTime = 2;
    PopulateCommonFields(prod, kNumTime, /*include_corners=*/true);

    constexpr long kDims[] = {kNumTime};
    constexpr double kAod550[] = {0.1, 0.2};
    constexpr double kAod670[] = {0.3, 0.4};
    constexpr double kAod870[] = {0.5, 0.6};
    constexpr double kAod1600[] = {0.7, 0.8};
    constexpr double kUnc550[] = {0.01, 0.02};
    // Omit AOD670_uncertainty and AOD1600_uncertainty to exercise
    // CODA_ERROR_INVALID_NAME -> NaN fallback.
    constexpr double kUnc870[] = {0.05, 0.06};
    constexpr double kSatZenith[] = {10.0, 20.0};
    constexpr double kSunZenith[] = {30.0, 40.0};

    ASSERT_TRUE(prod.AddField(prod.root(), "AOD550",
                              prod.MakeDoubleArray(kDims, kAod550)));
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD670",
                              prod.MakeDoubleArray(kDims, kAod670)));
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD870",
                              prod.MakeDoubleArray(kDims, kAod870)));
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD1600",
                              prod.MakeDoubleArray(kDims, kAod1600)));
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD550_uncertainty",
                              prod.MakeDoubleArray(kDims, kUnc550)));
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD870_uncertainty",
                              prod.MakeDoubleArray(kDims, kUnc870)));
    ASSERT_TRUE(prod.AddField(prod.root(), "satellite_zenith_at_center",
                              prod.MakeDoubleArray(kDims, kSatZenith)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sun_zenith_at_center",
                              prod.MakeDoubleArray(kDims, kSunZenith)));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(4, res.dimensions[harp_dimension_spectral]);
    // 10957.5 * 86400 - 946684800 = 43200.0
    EXPECT_THAT(res.variables["datetime"].double_values,
                ElementsAre(43200.0, 86400.0));
    EXPECT_DOUBLE_EQ(52.0, res.variables["latitude"].double_values[0]);
    EXPECT_TRUE(std::isnan(res.variables["latitude"].double_values[1]));
    EXPECT_THAT(res.variables["longitude"].double_values,
                ElementsAre(4.0, 5.0));
    EXPECT_THAT(res.variables["latitude_bounds"].double_values,
                ElementsAre(51.5, 51.6, 52.6, 52.5, 52.5, 52.6, 53.6, 53.5));
    EXPECT_THAT(res.variables["longitude_bounds"].double_values,
                ElementsAre(3.5, 4.5, 4.6, 3.6, 4.5, 5.5, 5.6, 4.6));
    EXPECT_THAT(res.variables["aerosol_optical_depth"].double_values,
                ElementsAre(0.1, 0.3, 0.5, 0.7, 0.2, 0.4, 0.6, 0.8));
    EXPECT_DOUBLE_EQ(
        0.01,
        res.variables["aerosol_optical_depth_uncertainty"].double_values[0]);
    EXPECT_TRUE(std::isnan(
        res.variables["aerosol_optical_depth_uncertainty"].double_values[1]));
    EXPECT_DOUBLE_EQ(
        0.05,
        res.variables["aerosol_optical_depth_uncertainty"].double_values[2]);
    EXPECT_TRUE(std::isnan(
        res.variables["aerosol_optical_depth_uncertainty"].double_values[3]));
    EXPECT_THAT(
        res.variables["wavelength"].double_values,
        ElementsAre(550.0, 670.0, 870.0, 1600.0, 550.0, 670.0, 870.0, 1600.0));
    EXPECT_TRUE(res.variables["sensor_zenith_angle"].included);
    EXPECT_THAT(res.variables["sensor_zenith_angle"].double_values,
                ElementsAre(10.0, 20.0));
    EXPECT_TRUE(res.variables["solar_zenith_angle"].included);
    EXPECT_THAT(res.variables["solar_zenith_angle"].double_values,
                ElementsAre(30.0, 40.0));
  }
}

TEST(HarpIngestCciL2AerosolTest,
     AatsrZenithAnglesExcludedWhenMissingOrWrongLen) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod =
      FindIngestionModule("ESACCI_AEROSOL_L2_AATSR");
  ASSERT_NE(nullptr, mod);

  // 1. Missing satellite_zenith_at_center excludes sensor/solar zenith angles.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateCommonFields(prod, 2, /*include_corners=*/true);
    constexpr long kDims[] = {2};
    constexpr double kVals[] = {0.1, 0.2};
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD550",
                              prod.MakeDoubleArray(kDims, kVals)));
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD670",
                              prod.MakeDoubleArray(kDims, kVals)));
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD870",
                              prod.MakeDoubleArray(kDims, kVals)));
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD1600",
                              prod.MakeDoubleArray(kDims, kVals)));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_FALSE(res.variables["sensor_zenith_angle"].included);
    EXPECT_FALSE(res.variables["solar_zenith_angle"].included);
  }

  // 2. Wrong element count on satellite_zenith_at_center excludes zenith
  // angles.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateCommonFields(prod, 2, /*include_corners=*/true);
    constexpr long kDims[] = {2};
    constexpr long kBadDims[] = {1};
    constexpr double kVals[] = {0.1, 0.2};
    constexpr double kBadZenith[] = {10.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD550",
                              prod.MakeDoubleArray(kDims, kVals)));
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD670",
                              prod.MakeDoubleArray(kDims, kVals)));
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD870",
                              prod.MakeDoubleArray(kDims, kVals)));
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD1600",
                              prod.MakeDoubleArray(kDims, kVals)));
    ASSERT_TRUE(prod.AddField(prod.root(), "satellite_zenith_at_center",
                              prod.MakeDoubleArray(kBadDims, kBadZenith)));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_FALSE(res.variables["sensor_zenith_angle"].included);
    EXPECT_FALSE(res.variables["solar_zenith_angle"].included);
  }
}

TEST(HarpIngestCciL2AerosolTest, IngestMerisAlamoSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod =
      FindIngestionModule("ESACCI_AEROSOL_L2_MERIS_ALAMO");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateCommonFields(prod, 2, /*include_corners=*/true);
  constexpr long kDims[] = {2};
  constexpr double kAod550[] = {0.15, 0.25};
  constexpr double kAod865[] = {0.35, 0.45};
  constexpr double kStd550[] = {0.01, 0.02};
  constexpr double kStd865[] = {0.03, 0.04};
  ASSERT_TRUE(prod.AddField(prod.root(), "AOD550",
                            prod.MakeDoubleArray(kDims, kAod550)));
  ASSERT_TRUE(prod.AddField(prod.root(), "AOD865",
                            prod.MakeDoubleArray(kDims, kAod865)));
  ASSERT_TRUE(prod.AddField(prod.root(), "AOD550_std",
                            prod.MakeDoubleArray(kDims, kStd550)));
  ASSERT_TRUE(prod.AddField(prod.root(), "AOD865_std",
                            prod.MakeDoubleArray(kDims, kStd865)));

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(2, res.dimensions[harp_dimension_spectral]);
  EXPECT_THAT(res.variables["aerosol_optical_depth"].double_values,
              ElementsAre(0.15, 0.35, 0.25, 0.45));
  EXPECT_THAT(res.variables["aerosol_optical_depth_uncertainty"].double_values,
              ElementsAre(0.01, 0.03, 0.02, 0.04));
  EXPECT_THAT(res.variables["wavelength"].double_values,
              ElementsAre(550.0, 865.0, 550.0, 865.0));
}

TEST(HarpIngestCciL2AerosolTest, IngestIasiDlrAndLmdVariantsSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod =
      FindIngestionModule("ESACCI_AEROSOL_L2_IASI");
  ASSERT_NE(nullptr, mod);

  // 1. DLR/ULB variant with D_AOD* and satellite_zenith_at_center.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateCommonFields(prod, 2, /*include_corners=*/false);
    constexpr long kDims[] = {2};
    constexpr double k550[] = {0.1, 0.2};
    constexpr double k10000[] = {0.3, 0.4};
    constexpr double k11000[] = {0.5, 0.6};
    constexpr double kUnc[] = {0.02, 0.03};
    constexpr double kSat[] = {15.0, 25.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "D_AOD550",
                              prod.MakeDoubleArray(kDims, k550)));
    ASSERT_TRUE(prod.AddField(prod.root(), "D_AOD10000",
                              prod.MakeDoubleArray(kDims, k10000)));
    ASSERT_TRUE(prod.AddField(prod.root(), "D_AOD11000",
                              prod.MakeDoubleArray(kDims, k11000)));
    ASSERT_TRUE(prod.AddField(prod.root(), "D_AOD10000_uncertainty",
                              prod.MakeDoubleArray(kDims, kUnc)));
    ASSERT_TRUE(prod.AddField(prod.root(), "D_AOD11000_uncertainty",
                              prod.MakeDoubleArray(kDims, kUnc)));
    ASSERT_TRUE(prod.AddField(prod.root(), "satellite_zenith_at_center",
                              prod.MakeDoubleArray(kDims, kSat)));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(3, res.dimensions[harp_dimension_spectral]);
    EXPECT_THAT(res.variables["dust_aerosol_optical_depth"].double_values,
                ElementsAre(0.1, 0.3, 0.5, 0.2, 0.4, 0.6));
    EXPECT_THAT(res.variables["wavelength"].double_values,
                ElementsAre(550.0, 10000.0, 11000.0, 550.0, 10000.0, 11000.0));
    EXPECT_THAT(res.variables["sensor_zenith_angle"].double_values,
                ElementsAre(15.0, 25.0));
  }

  // 2. LMD variant with Daod* and satellite_zenith fallback.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateCommonFields(prod, 2, /*include_corners=*/false);
    constexpr long kDims[] = {2};
    constexpr double k550[] = {1.1, 1.2};
    constexpr double k10000[] = {1.3, 1.4};
    constexpr double k11000[] = {1.5, 1.6};
    constexpr double kUnc[] = {0.1, 0.2};
    constexpr double kSat[] = {5.0, 6.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "Daod550",
                              prod.MakeDoubleArray(kDims, k550)));
    ASSERT_TRUE(prod.AddField(prod.root(), "Daod10000",
                              prod.MakeDoubleArray(kDims, k10000)));
    ASSERT_TRUE(prod.AddField(prod.root(), "Daod11000",
                              prod.MakeDoubleArray(kDims, k11000)));
    ASSERT_TRUE(prod.AddField(prod.root(), "Daod10000_uncertainty",
                              prod.MakeDoubleArray(kDims, kUnc)));
    ASSERT_TRUE(prod.AddField(prod.root(), "satellite_zenith",
                              prod.MakeDoubleArray(kDims, kSat)));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["dust_aerosol_optical_depth"].double_values,
                ElementsAre(1.1, 1.3, 1.5, 1.2, 1.4, 1.6));
    EXPECT_THAT(res.variables["sensor_zenith_angle"].double_values,
                ElementsAre(5.0, 6.0));
  }
}

TEST(HarpIngestCciL2AerosolTest, ErrorPathsReadFailures) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* aatsr =
      FindIngestionModule("ESACCI_AEROSOL_L2_AATSR");
  ASSERT_NE(nullptr, aatsr);

  // Note: We intentionally do not trigger init_dimensions() failures (missing,
  // non-array, or 2D /latitude) because upstream ingestion_init_*() allocates
  // `ingest_info` with malloc() and calls ingestion_done(info) on
  // init_dimensions() failure before initializing values_buffer, aod_fieldname,
  // and aod_uncertainty_name, causing free() on uninitialized pointers.

  // 1. Wrong element count, wrong element type, and missing fields during read.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims2[] = {2};
    constexpr long kDims1[] = {1};
    constexpr double kLat[] = {10.0, 20.0};
    constexpr double kWrongLen[] = {1.0};
    constexpr std::string_view kStrings[] = {"bad1", "bad2"};
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                              prod.MakeDoubleArray(kDims2, kLat)));
    // Missing time -> CODA error; wrong-length longitude -> INGESTION error;
    // string AOD550 -> CODA read_double_array error;
    // string AOD550_uncertainty -> CODA error (not INVALID_NAME) in
    // read_aerosol_optical_depth_uncertainty.
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                              prod.MakeDoubleArray(kDims1, kWrongLen)));
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD550",
                              prod.MakeStringArray(kDims2, kStrings)));
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD550_uncertainty",
                              prod.MakeStringArray(kDims2, kStrings)));
    // Present satellite_zenith_at_center of right length so solar_zenith_angle
    // is included and fails on missing /sun_zenith_at_center.
    ASSERT_TRUE(prod.AddField(prod.root(), "satellite_zenith_at_center",
                              prod.MakeDoubleArray(kDims2, kLat)));

    IngestExerciseResult res =
        ExerciseIngestionModule(aatsr, prod.product(), /*options_str=*/nullptr,
                                /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(HARP_SUCCESS, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["longitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["aerosol_optical_depth"].read_status);
    EXPECT_EQ(-1,
              res.variables["aerosol_optical_depth_uncertainty"].read_status);
    EXPECT_EQ(-1, res.variables["solar_zenith_angle"].read_status);
  }

  // 2. Partial pixel_corner_latitude* / pixel_corner_longitude* failures (2, 3,
  // 4).
  for (int missing_corner = 2; missing_corner <= 4; ++missing_corner) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateCommonFields(prod, 2, /*include_corners=*/false);
    constexpr long kDims[] = {2};
    constexpr double kVals[] = {1.0, 2.0};
    for (int c = 1; c < missing_corner; ++c) {
      std::string lat_name = "pixel_corner_latitude" + std::to_string(c);
      std::string lon_name = "pixel_corner_longitude" + std::to_string(c);
      ASSERT_TRUE(prod.AddField(prod.root(), lat_name,
                                prod.MakeDoubleArray(kDims, kVals)));
      ASSERT_TRUE(prod.AddField(prod.root(), lon_name,
                                prod.MakeDoubleArray(kDims, kVals)));
    }
    IngestExerciseResult res =
        ExerciseIngestionModule(aatsr, prod.product(), /*options_str=*/nullptr,
                                /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["longitude_bounds"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
