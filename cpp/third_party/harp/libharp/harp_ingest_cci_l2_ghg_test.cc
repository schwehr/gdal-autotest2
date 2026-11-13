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
// Unit tests for ESA CCI L2 GHG ingestion modules
// (libharp/harp-ingest-cci_l2_ghg.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-cci_l2_ghg.c

#include <cmath>
#include <cstdint>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/stcorp_harp/google/util/coda_mem_product.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/ingest_test_util.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::ElementsAre;

TEST(HarpIngestCciL2GhgTest, IngestEmmaWithOptionalFieldsPresentAndAbsent) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ESACCI_GHG_L2_EMMA");
  ASSERT_NE(nullptr, mod);

  // 1. All optional fields present using viewing_zenith_angle fallback and
  // -999.0 fill value.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {2};
    constexpr double kTime[] = {946684800.0, 946684900.0};
    constexpr double kLat[] = {50.0, -999.0};
    constexpr double kLon[] = {10.0, 11.0};
    constexpr double kVza[] = {12.0, 13.0};
    constexpr double kSza[] = {35.0, 36.0};
    constexpr double kXch4[] = {1800.0, 1810.0};
    constexpr double kXch4Unc[] = {5.0, 6.0};
    constexpr double kXco2[] = {400.0, 405.0};
    constexpr double kXco2Unc[] = {1.5, 2.5};

    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kDims, kTime)));
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                              prod.MakeDoubleArray(kDims, kLat)));
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                              prod.MakeDoubleArray(kDims, kLon)));
    ASSERT_TRUE(prod.AddField(prod.root(), "viewing_zenith_angle",
                              prod.MakeDoubleArray(kDims, kVza)));
    ASSERT_TRUE(prod.AddField(prod.root(), "solar_zenith_angle",
                              prod.MakeDoubleArray(kDims, kSza)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "xch4", prod.MakeDoubleArray(kDims, kXch4)));
    ASSERT_TRUE(prod.AddField(prod.root(), "xch4_uncertainty",
                              prod.MakeDoubleArray(kDims, kXch4Unc)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "xco2", prod.MakeDoubleArray(kDims, kXco2)));
    ASSERT_TRUE(prod.AddField(prod.root(), "xco2_uncertainty",
                              prod.MakeDoubleArray(kDims, kXco2Unc)));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
    EXPECT_THAT(res.variables["datetime"].double_values,
                ElementsAre(0.0, 100.0));
    EXPECT_DOUBLE_EQ(50.0, res.variables["latitude"].double_values[0]);
    EXPECT_TRUE(std::isnan(res.variables["latitude"].double_values[1]));
    EXPECT_THAT(res.variables["sensor_zenith_angle"].double_values,
                ElementsAre(12.0, 13.0));
    EXPECT_THAT(res.variables["solar_zenith_angle"].double_values,
                ElementsAre(35.0, 36.0));
    EXPECT_THAT(res.variables["CH4_column_volume_mixing_ratio"].double_values,
                ElementsAre(1800.0, 1810.0));
    EXPECT_THAT(res.variables["CH4_column_volume_mixing_ratio_uncertainty"]
                    .double_values,
                ElementsAre(5.0, 6.0));
    EXPECT_THAT(res.variables["CO2_column_volume_mixing_ratio"].double_values,
                ElementsAre(400.0, 405.0));
    EXPECT_THAT(res.variables["CO2_column_volume_mixing_ratio_uncertainty"]
                    .double_values,
                ElementsAre(1.5, 2.5));
  }

  // 2. Optional fields omitted: include_* predicates return 0.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {2};
    constexpr double kTime[] = {946684800.0, 946684900.0};
    constexpr double kLatCentre[] = {-10.0, 10.0};
    constexpr double kLonCentre[] = {-20.0, 20.0};
    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kDims, kTime)));
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude_centre",
                              prod.MakeDoubleArray(kDims, kLatCentre)));
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude_centre",
                              prod.MakeDoubleArray(kDims, kLonCentre)));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["latitude"].double_values,
                ElementsAre(-10.0, 10.0));
    EXPECT_THAT(res.variables["longitude"].double_values,
                ElementsAre(-20.0, 20.0));
    EXPECT_FALSE(res.variables["sensor_zenith_angle"].included);
    EXPECT_FALSE(res.variables["solar_zenith_angle"].included);
    EXPECT_FALSE(res.variables["CH4_column_volume_mixing_ratio"].included);
    EXPECT_FALSE(res.variables["CO2_column_volume_mixing_ratio"].included);
  }
}

TEST(HarpIngestCciL2GhgTest, IngestGosatSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ESACCI_GHG_L2_GOSAT");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  constexpr long kDims[] = {2};
  constexpr double kTime[] = {946684800.0, 946684860.0};
  constexpr double kAlt[] = {100.0, 200.0};
  constexpr double kLat[] = {1.0, 2.0};
  constexpr double kLon[] = {3.0, 4.0};
  constexpr double kZenith[] = {15.0, 16.0};
  constexpr double kSza[] = {25.0, 26.0};
  constexpr double kXch4[] = {1750.0, 1760.0};
  constexpr double kXch4Unc[] = {4.0, 4.5};
  constexpr double kXco2[] = {395.0, 396.0};
  constexpr double kXco2Unc[] = {1.1, 1.2};

  ASSERT_TRUE(
      prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kDims, kTime)));
  ASSERT_TRUE(prod.AddField(prod.root(), "surface_altitude",
                            prod.MakeDoubleArray(kDims, kAlt)));
  ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                            prod.MakeDoubleArray(kDims, kLat)));
  ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                            prod.MakeDoubleArray(kDims, kLon)));
  ASSERT_TRUE(prod.AddField(prod.root(), "sensor_zenith_angle",
                            prod.MakeDoubleArray(kDims, kZenith)));
  ASSERT_TRUE(prod.AddField(prod.root(), "solar_zenith_angle",
                            prod.MakeDoubleArray(kDims, kSza)));
  ASSERT_TRUE(
      prod.AddField(prod.root(), "xch4", prod.MakeDoubleArray(kDims, kXch4)));
  ASSERT_TRUE(prod.AddField(prod.root(), "xch4_uncertainty",
                            prod.MakeDoubleArray(kDims, kXch4Unc)));
  ASSERT_TRUE(
      prod.AddField(prod.root(), "xco2", prod.MakeDoubleArray(kDims, kXco2)));
  ASSERT_TRUE(prod.AddField(prod.root(), "xco2_uncertainty",
                            prod.MakeDoubleArray(kDims, kXco2Unc)));

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_THAT(res.variables["surface_altitude"].double_values,
              ElementsAre(100.0, 200.0));
  EXPECT_THAT(res.variables["sensor_zenith_angle"].double_values,
              ElementsAre(15.0, 16.0));
}

TEST(HarpIngestCciL2GhgTest, IngestSciamachyBdcaReorderingAndOptionalBounds) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod =
      FindIngestionModule("ESACCI_GHG_L2_SCIAMACHY");
  ASSERT_NE(nullptr, mod);

  // 1. With altitude and corners (ABCD -> BDCA reordering).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {2};
    constexpr long kBoundsDims[] = {2, 4};
    constexpr double kTime[] = {946684800.0, 946684810.0};
    constexpr double kAlt[] = {50.0, 60.0};
    constexpr double kLat[] = {10.0, 20.0};
    constexpr double kLon[] = {30.0, 40.0};
    constexpr double kLatCorners[] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
    constexpr double kLonCorners[] = {11.0, 12.0, 13.0, 14.0,
                                      15.0, 16.0, 17.0, 18.0};
    constexpr double kZenith[] = {5.0, 6.0};
    constexpr double kSza[] = {45.0, 46.0};

    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kDims, kTime)));
    ASSERT_TRUE(prod.AddField(prod.root(), "altitude",
                              prod.MakeDoubleArray(kDims, kAlt)));
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                              prod.MakeDoubleArray(kDims, kLat)));
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                              prod.MakeDoubleArray(kDims, kLon)));
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude_corners",
                              prod.MakeDoubleArray(kBoundsDims, kLatCorners)));
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude_corners",
                              prod.MakeDoubleArray(kBoundsDims, kLonCorners)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sensor_zenith_angle",
                              prod.MakeDoubleArray(kDims, kZenith)));
    ASSERT_TRUE(prod.AddField(prod.root(), "solar_zenith_angle",
                              prod.MakeDoubleArray(kDims, kSza)));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["surface_altitude"].double_values,
                ElementsAre(50.0, 60.0));
    EXPECT_THAT(res.variables["latitude_bounds"].double_values,
                ElementsAre(2.0, 4.0, 3.0, 1.0, 6.0, 8.0, 7.0, 5.0));
    EXPECT_THAT(res.variables["longitude_bounds"].double_values,
                ElementsAre(12.0, 14.0, 13.0, 11.0, 16.0, 18.0, 17.0, 15.0));
  }

  // 2. Without optional altitude and corners.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {1};
    constexpr double kTime[] = {946684800.0};
    constexpr double kLat[] = {10.0};
    constexpr double kLon[] = {30.0};
    constexpr double kZenith[] = {5.0};
    constexpr double kSza[] = {45.0};

    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kDims, kTime)));
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                              prod.MakeDoubleArray(kDims, kLat)));
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                              prod.MakeDoubleArray(kDims, kLon)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sensor_zenith_angle",
                              prod.MakeDoubleArray(kDims, kZenith)));
    ASSERT_TRUE(prod.AddField(prod.root(), "solar_zenith_angle",
                              prod.MakeDoubleArray(kDims, kSza)));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_FALSE(res.variables["surface_altitude"].included);
    EXPECT_FALSE(res.variables["latitude_bounds"].included);
    EXPECT_FALSE(res.variables["longitude_bounds"].included);
  }
}

TEST(HarpIngestCciL2GhgTest, IngestTropomiSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod =
      FindIngestionModule("ESACCI_GHG_L2_TROPOMI");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  constexpr long kDims[] = {2};
  constexpr long kBoundsDims[] = {2, 4};
  constexpr double kTime[] = {946684800.0, 946684805.0};
  constexpr double kLat[] = {10.0, 20.0};
  constexpr double kLon[] = {30.0, 40.0};
  constexpr double kLatCorners[] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
  constexpr double kLonCorners[] = {11.0, 12.0, 13.0, 14.0,
                                    15.0, 16.0, 17.0, 18.0};
  constexpr double kZenith[] = {5.0, 6.0};
  constexpr double kSza[] = {45.0, 46.0};
  constexpr double kXch4[] = {1820.0, 1830.0};
  constexpr double kXch4Unc[] = {6.0, 7.0};
  constexpr double kXco[] = {90.0, 95.0};
  constexpr double kXcoUnc[] = {2.0, 3.0};

  ASSERT_TRUE(
      prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kDims, kTime)));
  ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                            prod.MakeDoubleArray(kDims, kLat)));
  ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                            prod.MakeDoubleArray(kDims, kLon)));
  ASSERT_TRUE(prod.AddField(prod.root(), "latitude_corners",
                            prod.MakeDoubleArray(kBoundsDims, kLatCorners)));
  ASSERT_TRUE(prod.AddField(prod.root(), "longitude_corners",
                            prod.MakeDoubleArray(kBoundsDims, kLonCorners)));
  ASSERT_TRUE(prod.AddField(prod.root(), "sensor_zenith_angle",
                            prod.MakeDoubleArray(kDims, kZenith)));
  ASSERT_TRUE(prod.AddField(prod.root(), "solar_zenith_angle",
                            prod.MakeDoubleArray(kDims, kSza)));
  ASSERT_TRUE(
      prod.AddField(prod.root(), "xch4", prod.MakeDoubleArray(kDims, kXch4)));
  ASSERT_TRUE(prod.AddField(prod.root(), "xch4_uncertainty",
                            prod.MakeDoubleArray(kDims, kXch4Unc)));
  ASSERT_TRUE(
      prod.AddField(prod.root(), "xco", prod.MakeDoubleArray(kDims, kXco)));
  ASSERT_TRUE(prod.AddField(prod.root(), "xco_uncertainty",
                            prod.MakeDoubleArray(kDims, kXcoUnc)));

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_THAT(res.variables["latitude_bounds"].double_values,
              ElementsAre(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0));
  EXPECT_THAT(res.variables["longitude_bounds"].double_values,
              ElementsAre(11.0, 12.0, 13.0, 14.0, 15.0, 16.0, 17.0, 18.0));
  EXPECT_THAT(res.variables["CO_column_volume_mixing_ratio"].double_values,
              ElementsAre(90.0, 95.0));
  EXPECT_THAT(
      res.variables["CO_column_volume_mixing_ratio_uncertainty"].double_values,
      ElementsAre(2.0, 3.0));
}

TEST(HarpIngestCciL2GhgTest, ErrorPathsInitAndReadFailures) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* sciamachy =
      FindIngestionModule("ESACCI_GHG_L2_SCIAMACHY");
  const harp_ingestion_module* gosat =
      FindIngestionModule("ESACCI_GHG_L2_GOSAT");
  ASSERT_NE(nullptr, sciamachy);
  ASSERT_NE(nullptr, gosat);

  // 1. Missing /time.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res =
        ExerciseIngestionModule(sciamachy, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. Non-array /time.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "time", prod.MakeDouble(1.0)));
    IngestExerciseResult res =
        ExerciseIngestionModule(sciamachy, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. 2D /time.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims2d[] = {2, 2};
    constexpr double kVals[] = {1.0, 2.0, 3.0, 4.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "time",
                              prod.MakeDoubleArray(kDims2d, kVals)));
    IngestExerciseResult res =
        ExerciseIngestionModule(sciamachy, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 4. Read failures: string /time, missing surface_altitude (GOSAT),
  // wrong-length bounds in SCIAMACHY BDCA readers, missing latitude/longitude,
  // missing uncertainties when xch4/xco2 are present.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {2};
    constexpr long kBadDims[] = {3};
    constexpr std::string_view kBadTime[] = {"t1", "t2"};
    constexpr double kBadBounds[] = {1.0, 2.0, 3.0};
    constexpr double kGas[] = {1.0, 2.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "time",
                              prod.MakeStringArray(kDims, kBadTime)));
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude_corners",
                              prod.MakeDoubleArray(kBadDims, kBadBounds)));
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude_corners",
                              prod.MakeDoubleArray(kBadDims, kBadBounds)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "xch4", prod.MakeDoubleArray(kDims, kGas)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "xco2", prod.MakeDoubleArray(kDims, kGas)));

    IngestExerciseResult res_scia =
        ExerciseIngestionModule(sciamachy, prod.product(),
                                /*options_str=*/nullptr,
                                /*stop_on_error=*/false);
    EXPECT_EQ(-1, res_scia.variables["datetime"].read_status);
    EXPECT_EQ(-1, res_scia.variables["latitude"].read_status);
    EXPECT_EQ(-1, res_scia.variables["longitude"].read_status);
    EXPECT_EQ(-1, res_scia.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1, res_scia.variables["longitude_bounds"].read_status);
    EXPECT_EQ(-1, res_scia.variables["solar_zenith_angle"].read_status);
    EXPECT_EQ(-1,
              res_scia.variables["CH4_column_volume_mixing_ratio_uncertainty"]
                  .read_status);
    EXPECT_EQ(-1,
              res_scia.variables["CO2_column_volume_mixing_ratio_uncertainty"]
                  .read_status);

    IngestExerciseResult res_gosat =
        ExerciseIngestionModule(gosat, prod.product(), /*options_str=*/nullptr,
                                /*stop_on_error=*/false);
    EXPECT_EQ(-1, res_gosat.variables["surface_altitude"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
