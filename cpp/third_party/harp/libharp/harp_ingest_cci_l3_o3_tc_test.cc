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
// Unit tests for ESA CCI L3 O3 total column ingestion
// (libharp/harp-ingest-cci_l3_o3_tc.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-cci_l3_o3_tc.c

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

TEST(HarpIngestCciL3O3TcTest, IngestSuccessAllVariables) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("ESACCI_OZONE_L3_TC");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());

  ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_start",
                                prod.MakeString("20200515")));
  ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_end",
                                prod.MakeString("20200516")));

  constexpr long kLonDims[] = {3};
  constexpr double kLons[] = {-120.0, 0.0, 120.0};
  constexpr long kLatDims[] = {2};
  constexpr double kLats[] = {-45.0, 45.0};
  constexpr long kGridDims[] = {2, 3};
  constexpr double kO3[] = {280.0, 290.0, 300.0, 310.0, 320.0, 330.0};

  ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                            prod.MakeDoubleArray(kLonDims, kLons)));
  ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                            prod.MakeDoubleArray(kLatDims, kLats)));
  ASSERT_TRUE(prod.AddField(prod.root(), "atmosphere_mole_content_of_ozone",
                            prod.MakeDoubleArray(kGridDims, kO3)));

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(1L, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(2L, res.dimensions[harp_dimension_latitude]);
  EXPECT_EQ(3L, res.dimensions[harp_dimension_longitude]);

  double expected_start = 0.0;
  double expected_stop = 0.0;
  ASSERT_EQ(
      0, coda_time_string_to_double("yyyyMMdd", "20200515", &expected_start));
  ASSERT_EQ(0,
            coda_time_string_to_double("yyyyMMdd", "20200516", &expected_stop));
  EXPECT_THAT(res.variables["datetime_start"].double_values,
              ElementsAre(DoubleEq(expected_start)));
  EXPECT_THAT(res.variables["datetime_stop"].double_values,
              ElementsAre(DoubleEq(expected_stop)));
  EXPECT_THAT(res.variables["longitude"].double_values,
              ElementsAre(-120.0, 0.0, 120.0));
  EXPECT_THAT(res.variables["latitude"].double_values,
              ElementsAre(-45.0, 45.0));
  EXPECT_THAT(res.variables["O3_column_number_density"].double_values,
              ElementsAre(280.0, 290.0, 300.0, 310.0, 320.0, 330.0));
}

TEST(HarpIngestCciL3O3TcTest, InitDimensionErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ESACCI_OZONE_L3_TC");
  ASSERT_NE(nullptr, mod);

  constexpr long kDims1D[] = {1};
  constexpr double kVals1D[] = {1.0};
  constexpr long kDims2D[] = {1, 2};
  constexpr double kVals2D[] = {1.0, 2.0};

  // 1. Missing /longitude.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 2. Scalar /longitude.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude", prod.MakeDouble(1.0)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 3. 2D /longitude.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                              prod.MakeDoubleArray(kDims2D, kVals2D)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 4. Missing /latitude.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                              prod.MakeDoubleArray(kDims1D, kVals1D)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 5. Scalar /latitude.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                              prod.MakeDoubleArray(kDims1D, kVals1D)));
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude", prod.MakeDouble(1.0)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 6. 2D /latitude.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                              prod.MakeDoubleArray(kDims1D, kVals1D)));
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                              prod.MakeDoubleArray(kDims2D, kVals2D)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }
}

TEST(HarpIngestCciL3O3TcTest, ReadDatetimeAndDatasetErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ESACCI_OZONE_L3_TC");
  ASSERT_NE(nullptr, mod);

  constexpr long kDims1D[] = {1};
  constexpr double kVals1D[] = {1.0};

  // 1. Missing @time_coverage_start, non-string @time_coverage_end, missing
  // /atmosphere_mole_content_of_ozone.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                              prod.MakeDoubleArray(kDims1D, kVals1D)));
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                              prod.MakeDoubleArray(kDims1D, kVals1D)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_end",
                                  prod.MakeInt32(20200516)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_stop"].read_status);
    EXPECT_EQ(-1, res.variables["O3_column_number_density"].read_status);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 2. Wrong length @time_coverage_start, invalid date @time_coverage_end,
  // wrong element count for /atmosphere_mole_content_of_ozone.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                              prod.MakeDoubleArray(kDims1D, kVals1D)));
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                              prod.MakeDoubleArray(kDims1D, kVals1D)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_start",
                                  prod.MakeString("202005")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_end",
                                  prod.MakeString("99999999")));
    constexpr long kWrongDims[] = {2};
    constexpr double kWrongVals[] = {1.0, 2.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "atmosphere_mole_content_of_ozone",
                              prod.MakeDoubleArray(kWrongDims, kWrongVals)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_stop"].read_status);
    EXPECT_EQ(-1, res.variables["O3_column_number_density"].read_status);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 3. Wrong element type in /atmosphere_mole_content_of_ozone.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                              prod.MakeDoubleArray(kDims1D, kVals1D)));
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                              prod.MakeDoubleArray(kDims1D, kVals1D)));
    constexpr long kGridDims[] = {1, 1};
    constexpr std::string_view kBadStr[] = {"bad"};
    ASSERT_TRUE(prod.AddField(prod.root(), "atmosphere_mole_content_of_ozone",
                              prod.MakeStringArray(kGridDims, kBadStr)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["O3_column_number_density"].read_status);
    harp_set_error(HARP_SUCCESS, nullptr);
  }
}

}  // namespace
}  // namespace stcorp_harp
