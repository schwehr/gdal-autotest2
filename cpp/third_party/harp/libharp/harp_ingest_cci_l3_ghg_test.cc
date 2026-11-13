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
// Unit tests for ESA CCI L3 greenhouse gas ingestion module
// (libharp/harp-ingest-cci_l3_ghg.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-cci_l3_ghg.c

#include <cmath>
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

constexpr char kModuleName[] = "ESACCI_GHG_L3_Obs4MIPs";

void AddGhgCoordinates(CodaMemProduct& prod) {
  // 3652 days since 1990-01-01 = 2000-01-01 = 0.0 s since 2000-01-01.
  constexpr long kTimeDims[] = {2};
  constexpr double kTime[] = {3652.0, 3653.0};
  constexpr long kLatLonDims[] = {2};
  constexpr double kLat[] = {-20.0, 20.0};
  constexpr double kLon[] = {10.0, 30.0};

  ASSERT_TRUE(prod.AddField(prod.root(), "time",
                            prod.MakeDoubleArray(kTimeDims, kTime)));
  ASSERT_TRUE(prod.AddField(prod.root(), "lat",
                            prod.MakeDoubleArray(kLatLonDims, kLat)));
  ASSERT_TRUE(prod.AddField(prod.root(), "lon",
                            prod.MakeDoubleArray(kLatLonDims, kLon)));
}

TEST(HarpIngestCciL3GhgTest, IngestCh4AndCo2WithFillValues) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule(kModuleName);
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  AddGhgCoordinates(prod);

  constexpr long kGridDims[] = {2, 2, 2};
  constexpr double kXch4[] = {1.8e-6, 1.0e20, 1.9e-6, 2.0e-6,
                              2.1e-6, 2.2e-6, 2.3e-6, 2.4e-6};
  constexpr double kXch4Std[] = {1.0e-8, 2.0e-8, 3.0e-8, 4.0e-8,
                                 5.0e-8, 6.0e-8, 7.0e-8, 8.0e-8};
  constexpr double kXco2[] = {4.0e-4, 4.1e-4, 1.0e20, 4.2e-4,
                              4.3e-4, 4.4e-4, 4.5e-4, 4.6e-4};
  constexpr double kXco2Std[] = {1.0e-6, 2.0e-6, 3.0e-6, 4.0e-6,
                                 5.0e-6, 6.0e-6, 7.0e-6, 8.0e-6};
  ASSERT_TRUE(prod.AddField(prod.root(), "xch4",
                            prod.MakeDoubleArray(kGridDims, kXch4)));
  ASSERT_TRUE(prod.AddField(prod.root(), "xch4_stddev",
                            prod.MakeDoubleArray(kGridDims, kXch4Std)));
  ASSERT_TRUE(prod.AddField(prod.root(), "xco2",
                            prod.MakeDoubleArray(kGridDims, kXco2)));
  ASSERT_TRUE(prod.AddField(prod.root(), "xco2_stddev",
                            prod.MakeDoubleArray(kGridDims, kXco2Std)));

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(2L, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(2L, res.dimensions[harp_dimension_latitude]);
  EXPECT_EQ(2L, res.dimensions[harp_dimension_longitude]);

  EXPECT_THAT(res.variables["datetime"].double_values,
              ElementsAre(0.0, 86400.0));
  EXPECT_THAT(res.variables["latitude"].double_values,
              ElementsAre(-20.0, 20.0));
  EXPECT_THAT(res.variables["longitude"].double_values,
              ElementsAre(10.0, 30.0));

  EXPECT_TRUE(res.variables["CH4_column_volume_mixing_ratio"].included);
  EXPECT_NEAR(1.8,
              res.variables["CH4_column_volume_mixing_ratio"].double_values[0],
              1e-12);
  EXPECT_TRUE(std::isnan(
      res.variables["CH4_column_volume_mixing_ratio"].double_values[1]));
  EXPECT_NEAR(0.01,
              res.variables["CH4_column_volume_mixing_ratio_uncertainty"]
                  .double_values[0],
              1e-12);

  EXPECT_TRUE(res.variables["CO2_column_volume_mixing_ratio"].included);
  EXPECT_NEAR(400.0,
              res.variables["CO2_column_volume_mixing_ratio"].double_values[0],
              1e-9);
  EXPECT_TRUE(std::isnan(
      res.variables["CO2_column_volume_mixing_ratio"].double_values[2]));
  EXPECT_NEAR(1.0,
              res.variables["CO2_column_volume_mixing_ratio_uncertainty"]
                  .double_values[0],
              1e-12);
}

TEST(HarpIngestCciL3GhgTest, OptionalGasInclusion) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule(kModuleName);
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  AddGhgCoordinates(prod);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_FALSE(res.variables["CH4_column_volume_mixing_ratio"].included);
  EXPECT_FALSE(
      res.variables["CH4_column_volume_mixing_ratio_uncertainty"].included);
  EXPECT_FALSE(res.variables["CO2_column_volume_mixing_ratio"].included);
  EXPECT_FALSE(
      res.variables["CO2_column_volume_mixing_ratio_uncertainty"].included);
}

TEST(HarpIngestCciL3GhgTest, DimensionAndReadErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule(kModuleName);
  ASSERT_NE(nullptr, mod);

  // 1. Missing time.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. Scalar time (get_array_dim fails) and 2D time (num_coda_dims != 1).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "time", prod.MakeDouble(1.0)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDim2D[] = {1, 1};
    constexpr double kOne[] = {1.0};
    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kDim2D, kOne)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 3. Missing lat, scalar lat, 2D lat.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDim1D[] = {1};
    constexpr double kOne[] = {1.0};
    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kDim1D, kOne)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);

    ASSERT_TRUE(prod.AddField(prod.root(), "lat", prod.MakeDouble(1.0)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDim1D[] = {1};
    constexpr long kDim2D[] = {1, 1};
    constexpr double kOne[] = {1.0};
    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kDim1D, kOne)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lat", prod.MakeDoubleArray(kDim2D, kOne)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 4. Missing lon, scalar lon, 2D lon.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDim1D[] = {1};
    constexpr double kOne[] = {1.0};
    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kDim1D, kOne)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lat", prod.MakeDoubleArray(kDim1D, kOne)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);

    ASSERT_TRUE(prod.AddField(prod.root(), "lon", prod.MakeDouble(1.0)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDim1D[] = {1};
    constexpr long kDim2D[] = {1, 1};
    constexpr double kOne[] = {1.0};
    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kDim1D, kOne)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lat", prod.MakeDoubleArray(kDim1D, kOne)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lon", prod.MakeDoubleArray(kDim2D, kOne)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 5. read_dataset errors: wrong size, wrong type, missing stddev fields.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDim1D[] = {2};
    constexpr std::string_view kBadStrs[] = {"a", "b"};
    constexpr double kGood[] = {1.0, 2.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "time",
                              prod.MakeStringArray(kDim1D, kBadStrs)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lat", prod.MakeDoubleArray(kDim1D, kGood)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lon", prod.MakeDoubleArray(kDim1D, kGood)));
    // Wrong element count (1 instead of 8) for xch4, missing xch4_stddev,
    // missing xco2_stddev.
    constexpr long kBadGrid[] = {1};
    constexpr double kBadVal[] = {1.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "xch4",
                              prod.MakeDoubleArray(kBadGrid, kBadVal)));
    ASSERT_TRUE(prod.AddField(prod.root(), "xco2",
                              prod.MakeDoubleArray(kBadGrid, kBadVal)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["CH4_column_volume_mixing_ratio"].read_status);
    EXPECT_EQ(-1, res.variables["CH4_column_volume_mixing_ratio_uncertainty"]
                      .read_status);
    EXPECT_EQ(-1, res.variables["CO2_column_volume_mixing_ratio"].read_status);
    EXPECT_EQ(-1, res.variables["CO2_column_volume_mixing_ratio_uncertainty"]
                      .read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
