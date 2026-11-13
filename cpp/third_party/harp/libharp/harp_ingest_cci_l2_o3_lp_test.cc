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
// Unit tests for ESA CCI L2 Ozone Limb Profile ingestion module
// (libharp/harp-ingest-cci_l2_o3_lp.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-cci_l2_o3_lp.c

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

TEST(HarpIngestCciL2O3LpTest, IngestSuccessWithNumericAndStringNaNFillValues) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ESACCI_OZONE_L2_LP");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  constexpr long kTimeDims[] = {2};
  constexpr long kVertDims[] = {3};
  constexpr long kGridDims[] = {2, 3};

  constexpr double kTime[] = {44000.0, 44001.0};
  constexpr double kLon[] = {10.0, 20.0};
  constexpr double kLat[] = {-30.0, 30.0};
  constexpr double kAlt[] = {10.0, 20.0, 30.0, 11.0, 21.0, 31.0};
  constexpr double kPress[] = {200.0, 50.0, 10.0};
  constexpr double kTemp[] = {220.0, -999.0, 240.0, 221.0, 231.0, 241.0};
  constexpr double kO3[] = {1e12, 2e12, 3e12, 4e12, 5e12, 6e12};
  constexpr double kO3Err[] = {1e10, 2e10, 3e10, 4e10, 5e10, 6e10};

  ASSERT_TRUE(prod.AddField(prod.root(), "time",
                            prod.MakeDoubleArray(kTimeDims, kTime)));
  ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                            prod.MakeDoubleArray(kTimeDims, kLon)));
  ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                            prod.MakeDoubleArray(kTimeDims, kLat)));
  ASSERT_TRUE(prod.AddField(prod.root(), "altitude",
                            prod.MakeDoubleArray(kGridDims, kAlt)));
  ASSERT_TRUE(prod.AddField(prod.root(), "air_pressure",
                            prod.MakeDoubleArray(kVertDims, kPress)));

  // air_temperature uses numeric @FillValue = -999.0.
  CodaMemNode* temp_node = prod.MakeDoubleArray(kGridDims, kTemp);
  ASSERT_TRUE(
      prod.AddAttribute(temp_node, "FillValue", prod.MakeDouble(-999.0)));
  ASSERT_TRUE(prod.AddField(prod.root(), "air_temperature", temp_node));

  // mole_concentration_of_ozone_in_air uses string @FillValue = "NaN".
  CodaMemNode* o3_node = prod.MakeDoubleArray(kGridDims, kO3);
  ASSERT_TRUE(prod.AddAttribute(o3_node, "FillValue", prod.MakeString("NaN")));
  ASSERT_TRUE(prod.AddField(prod.root(), "mole_concentration_of_ozone_in_air",
                            o3_node));

  // mole_concentration_of_ozone_in_air_standard_error omits @FillValue.
  ASSERT_TRUE(prod.AddField(prod.root(),
                            "mole_concentration_of_ozone_in_air_standard_error",
                            prod.MakeDoubleArray(kGridDims, kO3Err)));

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(3, res.dimensions[harp_dimension_vertical]);
  EXPECT_THAT(res.variables["datetime"].double_values,
              ElementsAre(44000.0, 44001.0));
  EXPECT_THAT(res.variables["longitude"].double_values,
              ElementsAre(10.0, 20.0));
  EXPECT_THAT(res.variables["latitude"].double_values,
              ElementsAre(-30.0, 30.0));
  EXPECT_THAT(res.variables["altitude"].double_values,
              ElementsAre(10.0, 20.0, 30.0, 11.0, 21.0, 31.0));
  EXPECT_THAT(res.variables["pressure"].double_values,
              ElementsAre(200.0, 50.0, 10.0));
  EXPECT_DOUBLE_EQ(220.0, res.variables["temperature"].double_values[0]);
  EXPECT_TRUE(std::isnan(res.variables["temperature"].double_values[1]));
  EXPECT_DOUBLE_EQ(240.0, res.variables["temperature"].double_values[2]);
  EXPECT_THAT(res.variables["O3_number_density"].double_values,
              ElementsAre(1e12, 2e12, 3e12, 4e12, 5e12, 6e12));
  EXPECT_THAT(res.variables["O3_number_density_uncertainty"].double_values,
              ElementsAre(1e10, 2e10, 3e10, 4e10, 5e10, 6e10));
}

TEST(HarpIngestCciL2O3LpTest, ErrorPathsInitAndDatasetReadFailures) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ESACCI_OZONE_L2_LP");
  ASSERT_NE(nullptr, mod);

  // 1. Missing /mole_concentration_of_ozone_in_air.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. Non-array /mole_concentration_of_ozone_in_air.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "mole_concentration_of_ozone_in_air",
                              prod.MakeDouble(1.0)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. 1D /mole_concentration_of_ozone_in_air (expected 2D).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims1d[] = {2};
    constexpr double kVals[] = {1.0, 2.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "mole_concentration_of_ozone_in_air",
                              prod.MakeDoubleArray(kDims1d, kVals)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 4. Dataset read failures: missing field, wrong element count, non-double
  // array, invalid short @FillValue string ("bad"), and non-string/too-long
  // @FillValue ("too_long_str").
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kGridDims[] = {2, 2};
    constexpr long kTimeDims[] = {2};
    constexpr long kBadTimeDims[] = {1};
    constexpr double kO3[] = {1.0, 2.0, 3.0, 4.0};
    constexpr double kWrongLen[] = {1.0};
    constexpr std::string_view kBadStrings[] = {"a", "b"};

    ASSERT_TRUE(prod.AddField(prod.root(), "mole_concentration_of_ozone_in_air",
                              prod.MakeDoubleArray(kGridDims, kO3)));
    // Missing time -> CODA error.
    // Wrong-length longitude -> INGESTION error.
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                              prod.MakeDoubleArray(kBadTimeDims, kWrongLen)));
    // String-typed latitude -> CODA read_double_array error.
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                              prod.MakeStringArray(kTimeDims, kBadStrings)));
    // air_pressure with invalid string @FillValue "bad" -> INGESTION error.
    constexpr double kBadPress[] = {200.0, 100.0};
    CodaMemNode* press_node = prod.MakeDoubleArray(kTimeDims, kBadPress);
    ASSERT_TRUE(
        prod.AddAttribute(press_node, "FillValue", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "air_pressure", press_node));
    // air_temperature with @FillValue string longer than sizeof(str)=4 ->
    // CODA error in coda_cursor_read_string.
    CodaMemNode* temp_node = prod.MakeDoubleArray(kGridDims, kO3);
    ASSERT_TRUE(
        prod.AddAttribute(temp_node, "FillValue", prod.MakeString("too_long")));
    ASSERT_TRUE(prod.AddField(prod.root(), "air_temperature", temp_node));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["pressure"].read_status);
    EXPECT_EQ(-1, res.variables["temperature"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
