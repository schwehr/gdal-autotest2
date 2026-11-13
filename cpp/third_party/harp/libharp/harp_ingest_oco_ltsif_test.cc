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
// Unit tests for OCO L2 Lite SIF ingestion modules
// (libharp/harp-ingest-oco-ltsif.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-oco-ltsif.c

#include <cmath>
#include <cstdint>
#include <span>
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

constexpr long kNumTime = 2;
constexpr double kFillValue = -9999.0;

CodaMemNode* MakeDoubleDatasetWithMissing(CodaMemProduct& prod,
                                          std::span<const long> dims,
                                          std::span<const double> vals) {
  CodaMemNode* node = prod.MakeDoubleArray(dims, vals);
  constexpr long kFillDims[] = {1};
  constexpr double kFill[] = {kFillValue};
  if (!prod.AddAttribute(node, "missing_value",
                         prod.MakeDoubleArray(kFillDims, kFill))) {
    return nullptr;
  }
  return node;
}

void PopulateValidLtSif(CodaMemProduct& prod) {
  constexpr long kTimeDims[] = {kNumTime};
  constexpr long kCornerDims[] = {kNumTime, 4};

  constexpr double kTime[] = {100.0, 200.0};
  constexpr double kLat[] = {-15.0, 25.0};
  constexpr double kLon[] = {35.0, 45.0};
  constexpr double kLatCorners[] = {-16.0, -14.0, -14.0, -16.0,
                                    24.0,  26.0,  26.0,  24.0};
  constexpr double kLonCorners[] = {34.0, 34.0, 36.0, 36.0,
                                    44.0, 44.0, 46.0, 46.0};
  constexpr double kSaz[] = {10.0, 11.0};
  constexpr double kSza[] = {20.0, 21.0};
  constexpr double kVaz[] = {30.0, 31.0};
  constexpr double kVza[] = {40.0, 41.0};
  constexpr int16_t kQf[] = {0, 1};

  ASSERT_TRUE(
      prod.AddField(prod.root(), "Delta_Time",
                    MakeDoubleDatasetWithMissing(prod, kTimeDims, kTime)));
  ASSERT_TRUE(
      prod.AddField(prod.root(), "Latitude",
                    MakeDoubleDatasetWithMissing(prod, kTimeDims, kLat)));
  ASSERT_TRUE(
      prod.AddField(prod.root(), "Longitude",
                    MakeDoubleDatasetWithMissing(prod, kTimeDims, kLon)));
  ASSERT_TRUE(prod.AddField(
      prod.root(), "Latitude_Corners",
      MakeDoubleDatasetWithMissing(prod, kCornerDims, kLatCorners)));
  ASSERT_TRUE(prod.AddField(
      prod.root(), "Longitude_Corners",
      MakeDoubleDatasetWithMissing(prod, kCornerDims, kLonCorners)));
  ASSERT_TRUE(prod.AddField(
      prod.root(), "SAz", MakeDoubleDatasetWithMissing(prod, kTimeDims, kSaz)));
  ASSERT_TRUE(prod.AddField(
      prod.root(), "SZA", MakeDoubleDatasetWithMissing(prod, kTimeDims, kSza)));
  ASSERT_TRUE(prod.AddField(
      prod.root(), "VAz", MakeDoubleDatasetWithMissing(prod, kTimeDims, kVaz)));
  ASSERT_TRUE(prod.AddField(
      prod.root(), "VZA", MakeDoubleDatasetWithMissing(prod, kTimeDims, kVza)));
  ASSERT_TRUE(prod.AddField(prod.root(), "quality_flag",
                            prod.MakeInt16Array(kTimeDims, kQf)));

  constexpr double kDaily740[] = {1.1, kFillValue};
  constexpr double kDaily757[] = {2.1, 2.2};
  constexpr double kDaily771[] = {3.1, 3.2};
  constexpr double kInst740[] = {4.1, 4.2};
  constexpr double kInst757[] = {5.1, 5.2};
  constexpr double kInst771[] = {6.1, 6.2};
  constexpr double kUnc740[] = {0.1, 0.2};
  constexpr double kUnc757[] = {0.3, 0.4};
  constexpr double kUnc771[] = {0.5, 0.6};

  ASSERT_TRUE(
      prod.AddField(prod.root(), "Daily_SIF_740nm",
                    MakeDoubleDatasetWithMissing(prod, kTimeDims, kDaily740)));
  ASSERT_TRUE(
      prod.AddField(prod.root(), "Daily_SIF_757nm",
                    MakeDoubleDatasetWithMissing(prod, kTimeDims, kDaily757)));
  ASSERT_TRUE(
      prod.AddField(prod.root(), "Daily_SIF_771nm",
                    MakeDoubleDatasetWithMissing(prod, kTimeDims, kDaily771)));
  ASSERT_TRUE(
      prod.AddField(prod.root(), "SIF_740nm",
                    MakeDoubleDatasetWithMissing(prod, kTimeDims, kInst740)));
  ASSERT_TRUE(
      prod.AddField(prod.root(), "SIF_Uncertainty_740nm",
                    MakeDoubleDatasetWithMissing(prod, kTimeDims, kUnc740)));

  CodaMemNode* science = prod.MakeRecord();
  ASSERT_TRUE(
      prod.AddField(science, "SIF_757nm",
                    MakeDoubleDatasetWithMissing(prod, kTimeDims, kInst757)));
  ASSERT_TRUE(
      prod.AddField(science, "SIF_771nm",
                    MakeDoubleDatasetWithMissing(prod, kTimeDims, kInst771)));
  ASSERT_TRUE(
      prod.AddField(science, "SIF_Uncertainty_757nm",
                    MakeDoubleDatasetWithMissing(prod, kTimeDims, kUnc757)));
  ASSERT_TRUE(
      prod.AddField(science, "SIF_Uncertainty_771nm",
                    MakeDoubleDatasetWithMissing(prod, kTimeDims, kUnc771)));
  ASSERT_TRUE(prod.AddField(prod.root(), "Science", science));
}

TEST(HarpIngestOcoLtsifTest, IngestAllWavelengthAndCorrectionOptions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateValidLtSif(prod);

  struct OptionCase {
    const char* module_name;
    const char* options;
    double expected_wavelength;
    bool expect_uncertainty;
    double expected_sif0;
    bool expect_sif1_nan;
    double expected_sif1;
    double expected_unc0;
  };
  constexpr OptionCase kCases[] = {
      {.module_name = "OCO_OCO2_LtSIF",
       .options = nullptr,
       .expected_wavelength = 740.0,
       .expect_uncertainty = true,
       .expected_sif0 = 1.1,
       .expect_sif1_nan = true,
       .expected_sif1 = 0.0,
       .expected_unc0 = 0.1},
      {.module_name = "OCO_OCO2_LtSIF",
       .options = "sif_wavelength=757",
       .expected_wavelength = 757.0,
       .expect_uncertainty = true,
       .expected_sif0 = 2.1,
       .expect_sif1_nan = false,
       .expected_sif1 = 2.2,
       .expected_unc0 = 0.3},
      {.module_name = "OCO_OCO2_LtSIF",
       .options = "sif_wavelength=771",
       .expected_wavelength = 771.0,
       .expect_uncertainty = true,
       .expected_sif0 = 3.1,
       .expect_sif1_nan = false,
       .expected_sif1 = 3.2,
       .expected_unc0 = 0.5},
      {.module_name = "OCO_OCO3_LtSIF",
       .options = "daily_correction=applied",
       .expected_wavelength = 740.0,
       .expect_uncertainty = false,
       .expected_sif0 = 4.1,
       .expect_sif1_nan = false,
       .expected_sif1 = 4.2,
       .expected_unc0 = 0.0},
      {.module_name = "OCO_OCO3_LtSIF",
       .options = "sif_wavelength=757;daily_correction=applied",
       .expected_wavelength = 757.0,
       .expect_uncertainty = false,
       .expected_sif0 = 5.1,
       .expect_sif1_nan = false,
       .expected_sif1 = 5.2,
       .expected_unc0 = 0.0},
      {.module_name = "OCO_OCO3_LtSIF",
       .options = "sif_wavelength=771;daily_correction=applied",
       .expected_wavelength = 771.0,
       .expect_uncertainty = false,
       .expected_sif0 = 6.1,
       .expect_sif1_nan = false,
       .expected_sif1 = 6.2,
       .expected_unc0 = 0.0},
  };

  for (const auto& tc : kCases) {
    const harp_ingestion_module* mod = FindIngestionModule(tc.module_name);
    ASSERT_NE(nullptr, mod);
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), tc.options);
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(kNumTime, res.dimensions[harp_dimension_time]);

    EXPECT_THAT(res.variables["wavelength"].double_values,
                ElementsAre(tc.expected_wavelength));
    const auto& sif = res.variables["solar_induced_fluorescence"].double_values;
    ASSERT_EQ(2u, sif.size());
    EXPECT_DOUBLE_EQ(tc.expected_sif0, sif[0]);
    if (tc.expect_sif1_nan) {
      EXPECT_TRUE(std::isnan(sif[1]));
    } else {
      EXPECT_DOUBLE_EQ(tc.expected_sif1, sif[1]);
    }

    const auto& unc = res.variables["solar_induced_fluorescence_uncertainty"];
    EXPECT_EQ(tc.expect_uncertainty, unc.included);
    if (tc.expect_uncertainty) {
      EXPECT_DOUBLE_EQ(tc.expected_unc0, unc.double_values[0]);
    }
  }

  // Verify remaining geolocation, angles, and quality_flag.
  const harp_ingestion_module* mod = FindIngestionModule("OCO_OCO2_LtSIF");
  ASSERT_NE(nullptr, mod);
  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_THAT(res.variables["datetime"].double_values,
              ElementsAre(100.0, 200.0));
  EXPECT_THAT(res.variables["latitude"].double_values,
              ElementsAre(-15.0, 25.0));
  EXPECT_THAT(res.variables["longitude"].double_values,
              ElementsAre(35.0, 45.0));
  EXPECT_THAT(res.variables["latitude_bounds"].double_values,
              ElementsAre(-16.0, -14.0, -14.0, -16.0, 24.0, 26.0, 26.0, 24.0));
  EXPECT_THAT(res.variables["longitude_bounds"].double_values,
              ElementsAre(34.0, 34.0, 36.0, 36.0, 44.0, 44.0, 46.0, 46.0));
  EXPECT_THAT(res.variables["solar_azimuth_angle"].double_values,
              ElementsAre(10.0, 11.0));
  EXPECT_THAT(res.variables["solar_zenith_angle"].double_values,
              ElementsAre(20.0, 21.0));
  EXPECT_THAT(res.variables["viewing_azimuth_angle"].double_values,
              ElementsAre(30.0, 31.0));
  EXPECT_THAT(res.variables["viewing_zenith_angle"].double_values,
              ElementsAre(40.0, 41.0));
  EXPECT_THAT(res.variables["validity"].int16_values, ElementsAre(0, 1));
}

TEST(HarpIngestOcoLtsifTest, InitAndReadErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("OCO_OCO2_LtSIF");
  ASSERT_NE(nullptr, mod);

  // 1. Missing /Delta_Time.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. /Delta_Time is scalar (not an array).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "Delta_Time", prod.MakeDouble(1.0)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. /Delta_Time is 2D -> expected 1 dimension.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {1, 2};
    constexpr double kVals[] = {1.0, 2.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "Delta_Time",
                              prod.MakeDoubleArray(kDims, kVals)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 4. Read dataset error paths: missing field, wrong element count, wrong
  // type for double/int16, missing @missing_value[0], bad @missing_value[0].
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims1[] = {1};
    constexpr double kVal1[] = {1.0};
    // Delta_Time has no @missing_value -> triggers missing @missing_value[0].
    ASSERT_TRUE(prod.AddField(prod.root(), "Delta_Time",
                              prod.MakeDoubleArray(kDims1, kVal1)));

    // Latitude has wrong element count (2 != 1).
    constexpr long kDims2[] = {2};
    constexpr double kVal2[] = {1.0, 2.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "Latitude",
                              prod.MakeDoubleArray(kDims2, kVal2)));

    // Longitude is string array -> coda_cursor_read_double_array fails.
    constexpr std::string_view kBadStr[] = {"bad"};
    ASSERT_TRUE(prod.AddField(prod.root(), "Longitude",
                              prod.MakeStringArray(kDims1, kBadStr)));

    // SAz has string @missing_value -> coda_cursor_read_double fails.
    CodaMemNode* saz = prod.MakeDoubleArray(kDims1, kVal1);
    ASSERT_TRUE(prod.AddAttribute(saz, "missing_value",
                                  prod.MakeStringArray(kDims1, kBadStr)));
    ASSERT_TRUE(prod.AddField(prod.root(), "SAz", saz));

    // quality_flag is string array -> coda_cursor_read_int16_array fails.
    ASSERT_TRUE(prod.AddField(prod.root(), "quality_flag",
                              prod.MakeStringArray(kDims1, kBadStr)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["solar_azimuth_angle"].read_status);
    EXPECT_EQ(-1, res.variables["solar_zenith_angle"].read_status);
    EXPECT_EQ(-1, res.variables["validity"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
