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
// Unit tests for OCO-2 L3 CO2 ingestion module
// (libharp/harp-ingest-oco-l3co2.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-oco-l3co2.c

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

using ::testing::DoubleNear;
using ::testing::ElementsAre;

constexpr long kNumLat = 2;
constexpr long kNumLon = 3;
constexpr long kGridSize = kNumLat * kNumLon;

void PopulateValidOcoL3Co2(CodaMemProduct& prod, bool with_missing_value) {
  constexpr long kTimeDims[] = {1};
  constexpr double kTime[] = {0.0};
  ASSERT_TRUE(prod.AddField(prod.root(), "time",
                            prod.MakeDoubleArray(kTimeDims, kTime)));

  constexpr long kLatDims[] = {kNumLat};
  constexpr double kLat[] = {-30.0, 30.0};
  ASSERT_TRUE(
      prod.AddField(prod.root(), "lat", prod.MakeDoubleArray(kLatDims, kLat)));

  constexpr long kLonDims[] = {kNumLon};
  constexpr double kLon[] = {-120.0, 0.0, 120.0};
  ASSERT_TRUE(
      prod.AddField(prod.root(), "lon", prod.MakeDoubleArray(kLonDims, kLon)));

  ASSERT_TRUE(prod.AddAttribute(prod.root(), "RangeBeginningDate",
                                prod.MakeString("2000-01-01")));
  ASSERT_TRUE(prod.AddAttribute(prod.root(), "RangeBeginningTime",
                                prod.MakeString("00:00:00.000000")));

  constexpr long kGridDims[] = {1, kNumLat, kNumLon};
  constexpr double kXco2[] = {400.0, -9999.0, 402.0, 403.0, 404.0, 405.0};
  constexpr double kXco2Prec[] = {1.0, 1.5, -9999.0, 2.0, 2.5, 3.0};
  CodaMemNode* xco2 = prod.MakeDoubleArray(kGridDims, kXco2);
  CodaMemNode* prec = prod.MakeDoubleArray(kGridDims, kXco2Prec);
  if (with_missing_value) {
    constexpr long kFillDims[] = {1};
    constexpr double kFillVal[] = {-9999.0};
    ASSERT_TRUE(prod.AddAttribute(xco2, "missing_value",
                                  prod.MakeDoubleArray(kFillDims, kFillVal)));
    ASSERT_TRUE(prod.AddAttribute(prec, "missing_value",
                                  prod.MakeDoubleArray(kFillDims, kFillVal)));
  }
  ASSERT_TRUE(prod.AddField(prod.root(), "XCO2", xco2));
  ASSERT_TRUE(prod.AddField(prod.root(), "XCO2PREC", prec));
}

TEST(HarpIngestOcoL3co2Test, IngestSuccessWithAndWithoutMissingValue) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("OCO_OCO2_L3CO2");
  ASSERT_NE(nullptr, mod);

  // 1. With @missing_value attribute: fill values (-9999.0) become NaN.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateValidOcoL3Co2(prod, /*with_missing_value=*/true);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(1, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(kNumLat, res.dimensions[harp_dimension_latitude]);
    EXPECT_EQ(kNumLon, res.dimensions[harp_dimension_longitude]);

    EXPECT_THAT(res.variables["datetime_start"].double_values,
                ElementsAre(DoubleNear(0.0, 1e-6)));
    EXPECT_THAT(res.variables["datetime_length"].double_values,
                ElementsAre(86400.0));
    EXPECT_THAT(res.variables["latitude"].double_values,
                ElementsAre(-30.0, 30.0));
    EXPECT_THAT(res.variables["longitude"].double_values,
                ElementsAre(-120.0, 0.0, 120.0));

    const auto& xco2 =
        res.variables["CO2_column_volume_mixing_ratio_dry_air"].double_values;
    ASSERT_EQ(static_cast<size_t>(kGridSize), xco2.size());
    EXPECT_DOUBLE_EQ(400.0, xco2[0]);
    EXPECT_TRUE(std::isnan(xco2[1]));
    EXPECT_DOUBLE_EQ(405.0, xco2[5]);

    const auto& unc =
        res.variables["CO2_column_volume_mixing_ratio_dry_air_uncertainty"]
            .double_values;
    ASSERT_EQ(static_cast<size_t>(kGridSize), unc.size());
    EXPECT_DOUBLE_EQ(1.0, unc[0]);
    EXPECT_TRUE(std::isnan(unc[2]));
    EXPECT_DOUBLE_EQ(3.0, unc[5]);
  }

  // 2. Without @missing_value attribute: values are preserved verbatim.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateValidOcoL3Co2(prod, /*with_missing_value=*/false);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(
        res.variables["CO2_column_volume_mixing_ratio_dry_air"].double_values,
        ElementsAre(400.0, -9999.0, 402.0, 403.0, 404.0, 405.0));
  }
}

TEST(HarpIngestOcoL3co2Test, DimensionInitErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("OCO_OCO2_L3CO2");
  ASSERT_NE(nullptr, mod);

  // 1. Missing /time.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. /time is scalar (not an array) -> coda_cursor_get_array_dim fails.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "time", prod.MakeDouble(0.0)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. /time is 2D -> expected 1 dimension.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {1, 1};
    constexpr double kVals[] = {0.0};
    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kDims, kVals)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 4. /time has length 2 -> expected length 1 for daily grids.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {2};
    constexpr double kVals[] = {0.0, 1.0};
    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kDims, kVals)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 5. Missing /lat.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kTimeDims[] = {1};
    constexpr double kTime[] = {0.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "time",
                              prod.MakeDoubleArray(kTimeDims, kTime)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 6. /lat is scalar -> coda_cursor_get_array_dim fails.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kTimeDims[] = {1};
    constexpr double kTime[] = {0.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "time",
                              prod.MakeDoubleArray(kTimeDims, kTime)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lat", prod.MakeDouble(0.0)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 7. /lat is 2D -> expected 1 dimension.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kTimeDims[] = {1};
    constexpr double kTime[] = {0.0};
    constexpr long kLatDims[] = {1, 2};
    constexpr double kLat[] = {-10.0, 10.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "time",
                              prod.MakeDoubleArray(kTimeDims, kTime)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lat",
                              prod.MakeDoubleArray(kLatDims, kLat)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 8. Missing /lon.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kTimeDims[] = {1};
    constexpr double kTime[] = {0.0};
    constexpr long kLatDims[] = {2};
    constexpr double kLat[] = {-10.0, 10.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "time",
                              prod.MakeDoubleArray(kTimeDims, kTime)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lat",
                              prod.MakeDoubleArray(kLatDims, kLat)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 9. /lon is scalar -> coda_cursor_get_array_dim fails.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kTimeDims[] = {1};
    constexpr double kTime[] = {0.0};
    constexpr long kLatDims[] = {2};
    constexpr double kLat[] = {-10.0, 10.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "time",
                              prod.MakeDoubleArray(kTimeDims, kTime)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lat",
                              prod.MakeDoubleArray(kLatDims, kLat)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lon", prod.MakeDouble(0.0)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 10. /lon is 2D -> expected 1 dimension.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kTimeDims[] = {1};
    constexpr double kTime[] = {0.0};
    constexpr long kLatDims[] = {2};
    constexpr double kLat[] = {-10.0, 10.0};
    constexpr long kLonDims[] = {1, 2};
    constexpr double kLon[] = {-10.0, 10.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "time",
                              prod.MakeDoubleArray(kTimeDims, kTime)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lat",
                              prod.MakeDoubleArray(kLatDims, kLat)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lon",
                              prod.MakeDoubleArray(kLonDims, kLon)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
}

TEST(HarpIngestOcoL3co2Test, VariableReadErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("OCO_OCO2_L3CO2");
  ASSERT_NE(nullptr, mod);

  // 1. Missing RangeBeginningDate and missing datasets.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims1[] = {1};
    constexpr double kVal1[] = {0.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "time",
                              prod.MakeDoubleArray(kDims1, kVal1)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lat", prod.MakeDoubleArray(kDims1, kVal1)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lon", prod.MakeDoubleArray(kDims1, kVal1)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(0, res.variables["datetime_length"].read_status);
    EXPECT_EQ(
        -1,
        res.variables["CO2_column_volume_mixing_ratio_dry_air"].read_status);
  }

  // 2. Wrong type for @RangeBeginningDate.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims1[] = {1};
    constexpr double kVal1[] = {0.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "time",
                              prod.MakeDoubleArray(kDims1, kVal1)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lat", prod.MakeDoubleArray(kDims1, kVal1)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lon", prod.MakeDoubleArray(kDims1, kVal1)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "RangeBeginningDate",
                                  prod.MakeInt32(2020)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ("datetime_start", res.failed_variable);
  }

  // 3. Missing @RangeBeginningTime.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims1[] = {1};
    constexpr double kVal1[] = {0.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "time",
                              prod.MakeDoubleArray(kDims1, kVal1)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lat", prod.MakeDoubleArray(kDims1, kVal1)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lon", prod.MakeDoubleArray(kDims1, kVal1)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "RangeBeginningDate",
                                  prod.MakeString("2020-01-01")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ("datetime_start", res.failed_variable);
  }

  // 4. Wrong type for @RangeBeginningTime.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims1[] = {1};
    constexpr double kVal1[] = {0.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "time",
                              prod.MakeDoubleArray(kDims1, kVal1)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lat", prod.MakeDoubleArray(kDims1, kVal1)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lon", prod.MakeDoubleArray(kDims1, kVal1)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "RangeBeginningDate",
                                  prod.MakeString("2020-01-01")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "RangeBeginningTime",
                                  prod.MakeInt32(12)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ("datetime_start", res.failed_variable);
  }

  // 5. Invalid time string format for coda_time_string_to_double.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims1[] = {1};
    constexpr double kVal1[] = {0.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "time",
                              prod.MakeDoubleArray(kDims1, kVal1)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lat", prod.MakeDoubleArray(kDims1, kVal1)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lon", prod.MakeDoubleArray(kDims1, kVal1)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "RangeBeginningDate",
                                  prod.MakeString("not-a-date")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "RangeBeginningTime",
                                  prod.MakeString("00:00:00.000000")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ("datetime_start", res.failed_variable);
  }

  // 6. Mismatched element count in XCO2, non-double XCO2PREC, and non-double
  // @missing_value[0] on lat.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kTimeDims[] = {1};
    constexpr double kTime[] = {0.0};
    constexpr long kLatDims[] = {kNumLat};
    constexpr double kLat[] = {-30.0, 30.0};
    constexpr long kLonDims[] = {kNumLon};
    constexpr double kLon[] = {-120.0, 0.0, 120.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "time",
                              prod.MakeDoubleArray(kTimeDims, kTime)));

    CodaMemNode* lat = prod.MakeDoubleArray(kLatDims, kLat);
    constexpr long kBadDims[] = {1};
    constexpr std::string_view kBadFill[] = {"bad"};
    ASSERT_TRUE(prod.AddAttribute(lat, "missing_value",
                                  prod.MakeStringArray(kBadDims, kBadFill)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lat", lat));
    ASSERT_TRUE(prod.AddField(prod.root(), "lon",
                              prod.MakeDoubleArray(kLonDims, kLon)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "RangeBeginningDate",
                                  prod.MakeString("2020-01-01")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "RangeBeginningTime",
                                  prod.MakeString("00:00:00.000000")));

    // XCO2 with wrong element count (1 != 6).
    constexpr double kBadVals[] = {400.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "XCO2",
                              prod.MakeDoubleArray(kBadDims, kBadVals)));

    // XCO2PREC as string array (read_double_array fails).
    constexpr long kGridDims[] = {1, kNumLat, kNumLon};
    constexpr std::string_view kBadStrs[] = {"a", "b", "c", "d", "e", "f"};
    ASSERT_TRUE(prod.AddField(prod.root(), "XCO2PREC",
                              prod.MakeStringArray(kGridDims, kBadStrs)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(
        -1,
        res.variables["CO2_column_volume_mixing_ratio_dry_air"].read_status);
    EXPECT_EQ(
        -1, res.variables["CO2_column_volume_mixing_ratio_dry_air_uncertainty"]
                .read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
