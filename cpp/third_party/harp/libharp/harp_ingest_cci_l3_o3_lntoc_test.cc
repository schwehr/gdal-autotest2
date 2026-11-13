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
// Unit tests for ESA CCI L3 O3 limb-nadir tropospheric column ingestion
// (libharp/harp-ingest-cci_l3_o3_lntoc.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-cci_l3_o3_lntoc.c

#include <cmath>
#include <cstdint>
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

using ::testing::DoubleEq;
using ::testing::ElementsAre;

constexpr double kMolPerM2ToDu = 2241.15;

void PopulateValidLntocProduct(CodaMemProduct& prod) {
  constexpr long kTimeDims[] = {2};
  constexpr double kTimeVals[] = {100.0, 200.0};
  ASSERT_TRUE(prod.AddField(prod.root(), "time",
                            prod.MakeDoubleArray(kTimeDims, kTimeVals)));

  // Upstream read_datetime() calls coda_cursor_goto_next_array_element() at the
  // end of every iteration (including i == num_times - 1), so providing
  // num_times + 1 elements allows all num_times timestamps to be read cleanly.
  // Include one invalid timestamp to cover the coda_nan fallback branch.
  constexpr long kStrTimeDims[] = {3};
  constexpr std::string_view kStrTimes[] = {
      "20200515T120000Z", "99999999T999999Z", "20200516T120000Z"};
  ASSERT_TRUE(prod.AddField(prod.root(), "string_time",
                            prod.MakeStringArray(kStrTimeDims, kStrTimes)));

  constexpr double kLatVals[] = {-10.5, 25.5};
  constexpr double kLonVals[] = {15.0, -120.0};
  constexpr double kTropAltVals[] = {12.3, 14.7};
  constexpr double kTotO3Vals[] = {0.13, 0.14};
  constexpr double kTotO3ErrVals[] = {0.01, 0.02};
  constexpr double kStratO3Vals[] = {0.10, 0.11};
  constexpr double kStratO3ErrVals[] = {0.005, 0.006};
  constexpr double kTropO3Vals[] = {0.03, 0.03};
  constexpr double kTropO3ErrVals[] = {0.004, 0.005};
  constexpr double kCloudVals[] = {4.5, 6.2};
  constexpr double kSzaVals[] = {35.0, 48.5};

  ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                            prod.MakeDoubleArray(kTimeDims, kLatVals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                            prod.MakeDoubleArray(kTimeDims, kLonVals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "tropopause_altitude",
                            prod.MakeDoubleArray(kTimeDims, kTropAltVals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "total_ozone_column",
                            prod.MakeDoubleArray(kTimeDims, kTotO3Vals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "total_ozone_column_standard_error",
                            prod.MakeDoubleArray(kTimeDims, kTotO3ErrVals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "stratospheric_ozone_column",
                            prod.MakeDoubleArray(kTimeDims, kStratO3Vals)));
  ASSERT_TRUE(prod.AddField(prod.root(),
                            "stratospheric_ozone_column_standard_error",
                            prod.MakeDoubleArray(kTimeDims, kStratO3ErrVals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "tropospheric_ozone_column",
                            prod.MakeDoubleArray(kTimeDims, kTropO3Vals)));
  ASSERT_TRUE(prod.AddField(prod.root(),
                            "tropospheric_ozone_column_standard_error",
                            prod.MakeDoubleArray(kTimeDims, kTropO3ErrVals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "cloud_height",
                            prod.MakeDoubleArray(kTimeDims, kCloudVals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "sza_tanpnt",
                            prod.MakeDoubleArray(kTimeDims, kSzaVals)));
}

TEST(HarpIngestCciL3O3LntocTest, IngestSuccessAllVariables) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod =
      FindIngestionModule("ESACCI_OZONE_L3_LNTOC");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateValidLntocProduct(prod);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(2L, res.dimensions[harp_dimension_time]);

  double expected_dt0 = 0.0;
  ASSERT_EQ(0, coda_time_string_to_double("yyyyMMdd'T'HHmmss'Z'",
                                          "20200515T120000Z", &expected_dt0));
  ASSERT_EQ(2u, res.variables["datetime"].double_values.size());
  EXPECT_THAT(res.variables["datetime"].double_values[0],
              DoubleEq(expected_dt0));
  EXPECT_TRUE(std::isnan(res.variables["datetime"].double_values[1]));

  EXPECT_THAT(res.variables["latitude"].double_values,
              ElementsAre(-10.5, 25.5));
  EXPECT_THAT(res.variables["longitude"].double_values,
              ElementsAre(15.0, -120.0));
  EXPECT_THAT(res.variables["tropopause_altitude"].double_values,
              ElementsAre(12.3, 14.7));
  EXPECT_THAT(res.variables["O3_column_number_density"].double_values,
              ElementsAre(DoubleEq(0.13 * kMolPerM2ToDu),
                          DoubleEq(0.14 * kMolPerM2ToDu)));
  EXPECT_THAT(
      res.variables["O3_column_number_density_uncertainty"].double_values,
      ElementsAre(DoubleEq(0.01 * kMolPerM2ToDu),
                  DoubleEq(0.02 * kMolPerM2ToDu)));
  EXPECT_THAT(
      res.variables["stratospheric_O3_column_number_density"].double_values,
      ElementsAre(DoubleEq(0.10 * kMolPerM2ToDu),
                  DoubleEq(0.11 * kMolPerM2ToDu)));
  EXPECT_THAT(
      res.variables["stratospheric_O3_column_number_density_uncertainty"]
          .double_values,
      ElementsAre(DoubleEq(0.005 * kMolPerM2ToDu),
                  DoubleEq(0.006 * kMolPerM2ToDu)));
  EXPECT_THAT(
      res.variables["tropospheric_O3_column_number_density"].double_values,
      ElementsAre(DoubleEq(0.03 * kMolPerM2ToDu),
                  DoubleEq(0.03 * kMolPerM2ToDu)));
  EXPECT_THAT(res.variables["tropospheric_O3_column_number_density_uncertainty"]
                  .double_values,
              ElementsAre(DoubleEq(0.004 * kMolPerM2ToDu),
                          DoubleEq(0.005 * kMolPerM2ToDu)));
  EXPECT_THAT(res.variables["cloud_top_height"].double_values,
              ElementsAre(4.5, 6.2));
  EXPECT_THAT(res.variables["solar_zenith_angle"].double_values,
              ElementsAre(35.0, 48.5));
}

TEST(HarpIngestCciL3O3LntocTest, InitDimensionErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod =
      FindIngestionModule("ESACCI_OZONE_L3_LNTOC");
  ASSERT_NE(nullptr, mod);

  // 1. Missing /time.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 2. Scalar /time (not an array).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "time", prod.MakeDouble(1.0)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 3. 2D /time (expected 1D).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {1, 2};
    constexpr double kVals[] = {1.0, 2.0};
    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kDims, kVals)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }
}

TEST(HarpIngestCciL3O3LntocTest, ReadDatetimeAndDatasetErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod =
      FindIngestionModule("ESACCI_OZONE_L3_LNTOC");
  ASSERT_NE(nullptr, mod);

  // 1. Missing /string_time and missing /latitude.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {2};
    constexpr double kTimes[] = {1.0, 2.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "time",
                              prod.MakeDoubleArray(kDims, kTimes)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 2. Empty /string_time triggers goto_first_array_element failure; wrong-size
  // /latitude triggers element-count mismatch; string /longitude triggers
  // read_double_array failure.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kTimeDims[] = {2};
    constexpr double kTimes[] = {1.0, 2.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "time",
                              prod.MakeDoubleArray(kTimeDims, kTimes)));
    constexpr long kZeroDims[] = {0};
    ASSERT_TRUE(prod.AddField(prod.root(), "string_time",
                              prod.MakeStringArray(kZeroDims, {})));
    constexpr long kWrongDims[] = {1};
    constexpr double kWrongLat[] = {10.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                              prod.MakeDoubleArray(kWrongDims, kWrongLat)));
    constexpr std::string_view kBadLon[] = {"a", "b"};
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                              prod.MakeStringArray(kTimeDims, kBadLon)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 3. Non-string /string_time triggers coda_cursor_read_string failure.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {2};
    constexpr double kTimes[] = {1.0, 2.0};
    constexpr int32_t kIntTimes[] = {1, 2};
    ASSERT_TRUE(prod.AddField(prod.root(), "time",
                              prod.MakeDoubleArray(kDims, kTimes)));
    ASSERT_TRUE(prod.AddField(prod.root(), "string_time",
                              prod.MakeInt32Array(kDims, kIntTimes)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    EXPECT_EQ("datetime", res.failed_variable);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 4. Exact num_times elements in /string_time fails on last
  // coda_cursor_goto_next_array_element() (upstream missing loop-end guard).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {2};
    constexpr double kTimes[] = {1.0, 2.0};
    constexpr std::string_view kStrTimes[] = {"20200515T120000Z",
                                              "20200516T120000Z"};
    ASSERT_TRUE(prod.AddField(prod.root(), "time",
                              prod.MakeDoubleArray(kDims, kTimes)));
    ASSERT_TRUE(prod.AddField(prod.root(), "string_time",
                              prod.MakeStringArray(kDims, kStrTimes)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    EXPECT_EQ("datetime", res.failed_variable);
    harp_set_error(HARP_SUCCESS, nullptr);
  }
}

}  // namespace
}  // namespace stcorp_harp
