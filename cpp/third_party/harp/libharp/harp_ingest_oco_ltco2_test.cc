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
// Unit tests for OCO / ACOS L2 Lite CO2 ingestion modules
// (libharp/harp-ingest-oco-ltco2.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-oco-ltco2.c

#include <cmath>
#include <cstdint>
#include <span>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/stcorp_harp/google/util/coda_mem_product.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/ingest_test_util.h"
#include "third_party/stcorp_harp/google/util/oom_fault_inject.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::ElementsAre;

constexpr long kNumTime = 2;
constexpr long kNumLevels = 3;
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

void PopulateValidLtCo2(CodaMemProduct& prod, std::string_view product_type,
                        int version, bool has_corners, bool use_uint8_flags) {
  ASSERT_TRUE(prod.SetProductInfo("OCO", product_type, version));

  constexpr long kTimeDims[] = {kNumTime};
  constexpr long kLevelDims[] = {kNumLevels};
  constexpr long kCornerDims[] = {kNumTime, 4};
  constexpr long kProfileDims[] = {kNumTime, kNumLevels};

  constexpr double kTime[] = {1000.0, 2000.0};
  constexpr double kLevels[] = {1.0, 2.0, 3.0};
  constexpr double kLat[] = {-10.0, 20.0};
  constexpr double kLon[] = {30.0, 40.0};
  constexpr double kAlt[] = {100.0, 200.0};
  constexpr double kPsurf[] = {1010.0, 990.0};
  constexpr double kPlevels[] = {1000.0, 500.0, 100.0, 990.0, 490.0, 90.0};
  constexpr double kSaz[] = {10.0, 20.0};
  constexpr double kSza[] = {30.0, 40.0};
  constexpr double kSolAz[] = {50.0, 60.0};
  constexpr double kSolZa[] = {70.0, 80.0};
  constexpr double kXco2[] = {410.0, kFillValue};
  constexpr double kXco2Unc[] = {1.2, 1.4};
  constexpr double kXco2Apriori[] = {405.0, 406.0};
  constexpr double kAvk[] = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6};
  constexpr double kProfApriori[] = {401.0, 402.0, 403.0, 404.0, 405.0, 406.0};

  ASSERT_TRUE(
      prod.AddField(prod.root(), "time",
                    MakeDoubleDatasetWithMissing(prod, kTimeDims, kTime)));
  ASSERT_TRUE(
      prod.AddField(prod.root(), "levels",
                    MakeDoubleDatasetWithMissing(prod, kLevelDims, kLevels)));
  ASSERT_TRUE(
      prod.AddField(prod.root(), "latitude",
                    MakeDoubleDatasetWithMissing(prod, kTimeDims, kLat)));
  ASSERT_TRUE(
      prod.AddField(prod.root(), "longitude",
                    MakeDoubleDatasetWithMissing(prod, kTimeDims, kLon)));

  if (has_corners) {
    constexpr double kVLat[] = {-11.0, -9.0, -9.0, -11.0,
                                19.0,  21.0, 21.0, 19.0};
    constexpr double kVLon[] = {29.0, 29.0, 31.0, 31.0, 39.0, 39.0, 41.0, 41.0};
    ASSERT_TRUE(
        prod.AddField(prod.root(), "vertex_latitude",
                      MakeDoubleDatasetWithMissing(prod, kCornerDims, kVLat)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "vertex_longitude",
                      MakeDoubleDatasetWithMissing(prod, kCornerDims, kVLon)));
  }

  CodaMemNode* sounding = prod.MakeRecord();
  ASSERT_TRUE(
      prod.AddField(sounding, "altitude",
                    MakeDoubleDatasetWithMissing(prod, kTimeDims, kAlt)));
  ASSERT_TRUE(
      prod.AddField(sounding, "sensor_azimuth_angle",
                    MakeDoubleDatasetWithMissing(prod, kTimeDims, kSaz)));
  ASSERT_TRUE(
      prod.AddField(sounding, "solar_azimuth_angle",
                    MakeDoubleDatasetWithMissing(prod, kTimeDims, kSolAz)));
  ASSERT_TRUE(prod.AddField(prod.root(), "Sounding", sounding));

  CodaMemNode* retrieval = prod.MakeRecord();
  ASSERT_TRUE(
      prod.AddField(retrieval, "psurf",
                    MakeDoubleDatasetWithMissing(prod, kTimeDims, kPsurf)));
  ASSERT_TRUE(prod.AddField(prod.root(), "Retrieval", retrieval));

  ASSERT_TRUE(prod.AddField(
      prod.root(), "pressure_levels",
      MakeDoubleDatasetWithMissing(prod, kProfileDims, kPlevels)));
  ASSERT_TRUE(
      prod.AddField(prod.root(), "sensor_zenith_angle",
                    MakeDoubleDatasetWithMissing(prod, kTimeDims, kSza)));
  ASSERT_TRUE(
      prod.AddField(prod.root(), "solar_zenith_angle",
                    MakeDoubleDatasetWithMissing(prod, kTimeDims, kSolZa)));
  ASSERT_TRUE(
      prod.AddField(prod.root(), "xco2",
                    MakeDoubleDatasetWithMissing(prod, kTimeDims, kXco2)));
  ASSERT_TRUE(
      prod.AddField(prod.root(), "xco2_uncertainty",
                    MakeDoubleDatasetWithMissing(prod, kTimeDims, kXco2Unc)));
  ASSERT_TRUE(prod.AddField(
      prod.root(), "xco2_apriori",
      MakeDoubleDatasetWithMissing(prod, kTimeDims, kXco2Apriori)));
  ASSERT_TRUE(
      prod.AddField(prod.root(), "xco2_averaging_kernel",
                    MakeDoubleDatasetWithMissing(prod, kProfileDims, kAvk)));
  ASSERT_TRUE(prod.AddField(
      prod.root(), "co2_profile_apriori",
      MakeDoubleDatasetWithMissing(prod, kProfileDims, kProfApriori)));

  if (use_uint8_flags) {
    constexpr uint8_t kU8Flags[] = {0, 1};
    ASSERT_TRUE(prod.AddField(prod.root(), "xco2_quality_flag",
                              prod.MakeUint8Array(kTimeDims, kU8Flags)));
    ASSERT_TRUE(prod.AddField(prod.root(), "xco2_qf_simple_bitflag",
                              prod.MakeUint8Array(kTimeDims, kU8Flags)));
  } else {
    constexpr int8_t kI8Flags[] = {1, 2};
    ASSERT_TRUE(prod.AddField(prod.root(), "xco2_quality_flag",
                              prod.MakeInt8Array(kTimeDims, kI8Flags)));
    ASSERT_TRUE(prod.AddField(prod.root(), "xco2_qf_simple_bitflag",
                              prod.MakeInt8Array(kTimeDims, kI8Flags)));
  }
}

TEST(HarpIngestOcoLtco2Test, IngestOco2V10WithCornersAndBitflag) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("OCO_OCO2_LtCO2");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateValidLtCo2(prod, "oco2_LtCO2", /*version=*/10, /*has_corners=*/true,
                     /*use_uint8_flags=*/true);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(kNumTime, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(kNumLevels, res.dimensions[harp_dimension_vertical]);

  EXPECT_THAT(res.variables["datetime"].double_values,
              ElementsAre(1000.0, 2000.0));
  EXPECT_THAT(res.variables["latitude"].double_values,
              ElementsAre(-10.0, 20.0));
  EXPECT_THAT(res.variables["longitude"].double_values,
              ElementsAre(30.0, 40.0));
  EXPECT_THAT(res.variables["latitude_bounds"].double_values,
              ElementsAre(-11.0, -9.0, -9.0, -11.0, 19.0, 21.0, 21.0, 19.0));
  EXPECT_THAT(res.variables["longitude_bounds"].double_values,
              ElementsAre(29.0, 29.0, 31.0, 31.0, 39.0, 39.0, 41.0, 41.0));
  EXPECT_THAT(res.variables["surface_altitude"].double_values,
              ElementsAre(100.0, 200.0));
  EXPECT_THAT(res.variables["surface_pressure"].double_values,
              ElementsAre(1010.0, 990.0));
  // Vertical profiles are inverted along dim 1.
  EXPECT_THAT(res.variables["pressure"].double_values,
              ElementsAre(100.0, 500.0, 1000.0, 90.0, 490.0, 990.0));
  EXPECT_THAT(
      res.variables["CO2_column_volume_mixing_ratio_dry_avk"].double_values,
      ElementsAre(0.3, 0.2, 0.1, 0.6, 0.5, 0.4));
  EXPECT_THAT(
      res.variables["CO2_volume_mixing_ratio_dry_air_apriori"].double_values,
      ElementsAre(403.0, 402.0, 401.0, 406.0, 405.0, 404.0));
  EXPECT_THAT(res.variables["sensor_azimuth_angle"].double_values,
              ElementsAre(10.0, 20.0));
  EXPECT_THAT(res.variables["sensor_zenith_angle"].double_values,
              ElementsAre(30.0, 40.0));
  EXPECT_THAT(res.variables["solar_azimuth_angle"].double_values,
              ElementsAre(50.0, 60.0));
  EXPECT_THAT(res.variables["solar_zenith_angle"].double_values,
              ElementsAre(70.0, 80.0));

  const auto& xco2 =
      res.variables["CO2_column_volume_mixing_ratio_dry_air"].double_values;
  ASSERT_EQ(2u, xco2.size());
  EXPECT_DOUBLE_EQ(410.0, xco2[0]);
  EXPECT_TRUE(std::isnan(xco2[1]));

  EXPECT_THAT(
      res.variables["CO2_column_volume_mixing_ratio_dry_air_uncertainty"]
          .double_values,
      ElementsAre(1.2, 1.4));
  EXPECT_THAT(res.variables["CO2_column_volume_mixing_ratio_dry_air_apriori"]
                  .double_values,
              ElementsAre(405.0, 406.0));
  EXPECT_TRUE(res.variables["CO2_column_volume_mixing_ratio_dry_air_validity"]
                  .included);
  EXPECT_THAT(res.variables["CO2_column_volume_mixing_ratio_dry_air_validity"]
                  .int8_values,
              ElementsAre(0, 1));
  EXPECT_THAT(res.variables["validity"].int8_values, ElementsAre(0, 1));
}

TEST(HarpIngestOcoLtco2Test, IngestAcosAndOco3V9Int8Flags) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  // 1. ACOS has no corner bounds, version <= 9 excludes bitflag, int8 flags.
  {
    const harp_ingestion_module* mod = FindIngestionModule("OCO_ACOS_LtCO2");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateValidLtCo2(prod, "acos_LtCO2", /*version=*/9, /*has_corners=*/false,
                       /*use_uint8_flags=*/false);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_FALSE(
        res.variables["CO2_column_volume_mixing_ratio_dry_air_validity"]
            .included);
    EXPECT_FALSE(res.variables.contains("latitude_bounds"));
    EXPECT_THAT(res.variables["validity"].int8_values, ElementsAre(1, 2));
  }

  // 2. OCO-3 with version 10 and int8 flags exercises int8 branch on bitflag.
  {
    const harp_ingestion_module* mod = FindIngestionModule("OCO_OCO3_LtCO2");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateValidLtCo2(prod, "oco3_LtCO2", /*version=*/10, /*has_corners=*/true,
                       /*use_uint8_flags=*/false);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_TRUE(res.variables["CO2_column_volume_mixing_ratio_dry_air_validity"]
                    .included);
    EXPECT_THAT(res.variables["CO2_column_volume_mixing_ratio_dry_air_validity"]
                    .int8_values,
                ElementsAre(1, 2));
  }
}

TEST(HarpIngestOcoLtco2Test, InitAndReadErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("OCO_OCO2_LtCO2");
  ASSERT_NE(nullptr, mod);

  // 1. Null product passed to ingestion_init -> coda_get_product_version fails.
  {
    harp_ingestion_options* raw_opts = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_ingestion_options_new(&raw_opts));
    ScopedHarpIngestionOptions opts(raw_opts);
    harp_product_definition* prod_def = nullptr;
    void* user_data = nullptr;
    EXPECT_EQ(-1, mod->ingestion_init(mod, nullptr, opts.get(), &prod_def,
                                      &user_data));
    EXPECT_EQ(HARP_ERROR_CODA, harp_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 2. Missing /time.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("OCO", "oco2_LtCO2", 10));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. /time is scalar (coda_cursor_get_array_dim fails).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("OCO", "oco2_LtCO2", 10));
    ASSERT_TRUE(prod.AddField(prod.root(), "time", prod.MakeDouble(0.0)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 4. /time is 2D -> expected 1 dimension.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("OCO", "oco2_LtCO2", 10));
    constexpr long kDims[] = {1, 2};
    constexpr double kVals[] = {1.0, 2.0};
    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kDims, kVals)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 5. Missing /levels.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("OCO", "oco2_LtCO2", 10));
    constexpr long kDims[] = {1};
    constexpr double kVals[] = {1.0};
    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kDims, kVals)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 6. /levels is scalar (coda_cursor_get_array_dim fails).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("OCO", "oco2_LtCO2", 10));
    constexpr long kDims[] = {1};
    constexpr double kVals[] = {1.0};
    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kDims, kVals)));
    ASSERT_TRUE(prod.AddField(prod.root(), "levels", prod.MakeDouble(1.0)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 7. /levels is 2D -> expected 1 dimension.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("OCO", "oco2_LtCO2", 10));
    constexpr long kTimeDims[] = {1};
    constexpr double kTimeVals[] = {1.0};
    constexpr long kLevDims[] = {1, 2};
    constexpr double kLevVals[] = {1.0, 2.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "time",
                              prod.MakeDoubleArray(kTimeDims, kTimeVals)));
    ASSERT_TRUE(prod.AddField(prod.root(), "levels",
                              prod.MakeDoubleArray(kLevDims, kLevVals)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 8. Dataset read errors: missing dataset, wrong element count, wrong type,
  // missing @missing_value[0], wrong @missing_value[0] type, and non-array
  // int8 field.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("OCO", "oco2_LtCO2", 10));
    constexpr long kTimeDims[] = {1};
    constexpr double kTimeVals[] = {1.0};
    constexpr long kLevDims[] = {1};
    constexpr double kLevVals[] = {1.0};
    // time has no @missing_value -> triggers missing @missing_value[0].
    ASSERT_TRUE(prod.AddField(prod.root(), "time",
                              prod.MakeDoubleArray(kTimeDims, kTimeVals)));
    ASSERT_TRUE(prod.AddField(prod.root(), "levels",
                              prod.MakeDoubleArray(kLevDims, kLevVals)));

    // latitude has wrong element count (2 != 1).
    constexpr long kTwoDims[] = {2};
    constexpr double kTwoVals[] = {1.0, 2.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                              prod.MakeDoubleArray(kTwoDims, kTwoVals)));

    // longitude is a string array -> coda_cursor_read_double_array fails.
    constexpr std::string_view kBadStr[] = {"bad"};
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                              prod.MakeStringArray(kTimeDims, kBadStr)));

    // xco2 has string @missing_value -> coda_cursor_read_double fails.
    CodaMemNode* xco2 = prod.MakeDoubleArray(kTimeDims, kTimeVals);
    ASSERT_TRUE(prod.AddAttribute(xco2, "missing_value",
                                  prod.MakeStringArray(kTimeDims, kBadStr)));
    ASSERT_TRUE(prod.AddField(prod.root(), "xco2", xco2));

    // xco2_quality_flag is scalar int8 (not array) ->
    // coda_cursor_goto_first_array_element fails.
    ASSERT_TRUE(
        prod.AddField(prod.root(), "xco2_quality_flag", prod.MakeInt8(1)));

    // xco2_qf_simple_bitflag is string array -> read_int8_array fails.
    ASSERT_TRUE(prod.AddField(prod.root(), "xco2_qf_simple_bitflag",
                              prod.MakeStringArray(kTimeDims, kBadStr)));

    // pressure_levels is missing -> read_vertical_profile_dataset fails.
    // xco2_apriori is MakeNoData() -> coda_cursor_get_num_elements fails.
    ASSERT_TRUE(prod.AddField(prod.root(), "xco2_apriori", prod.MakeNoData()));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(
        -1,
        res.variables["CO2_column_volume_mixing_ratio_dry_air"].read_status);
    EXPECT_EQ(-1,
              res.variables["CO2_column_volume_mixing_ratio_dry_air_apriori"]
                  .read_status);
    EXPECT_EQ(-1, res.variables["validity"].read_status);
    EXPECT_EQ(-1,
              res.variables["CO2_column_volume_mixing_ratio_dry_air_validity"]
                  .read_status);
    EXPECT_EQ(-1, res.variables["pressure"].read_status);
  }

  // 9. Array of record elements on xco2_quality_flag ->
  // coda_cursor_get_read_type fails.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("OCO", "oco2_LtCO2", 10));
    constexpr long kDims[] = {1};
    constexpr double kVals[] = {1.0};
    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kDims, kVals)));
    ASSERT_TRUE(prod.AddField(prod.root(), "levels",
                              prod.MakeDoubleArray(kDims, kVals)));
    CodaMemNode* rec_elems[] = {prod.MakeRecord()};
    ASSERT_TRUE(prod.AddField(prod.root(), "xco2_quality_flag",
                              prod.MakeNodeArray(kDims, rec_elems)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["validity"].read_status);
  }

  // 10. ASCII-format uint8 array containing non-digit byte (255) ->
  // coda_cursor_get_read_type returns uint8, then coda_cursor_read_uint8_array
  // fails cleanly.
  {
    CodaMemProduct prod(coda_format_ascii);
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("OCO", "oco2_LtCO2", 10));
    constexpr long kDims[] = {1};
    constexpr double kVals[] = {1.0};
    constexpr uint8_t kNonDigit[] = {255};
    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kDims, kVals)));
    ASSERT_TRUE(prod.AddField(prod.root(), "levels",
                              prod.MakeDoubleArray(kDims, kVals)));
    ASSERT_TRUE(prod.AddField(prod.root(), "xco2_quality_flag",
                              prod.MakeUint8Array(kDims, kNonDigit)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["validity"].read_status);
  }
  // 11. coda_format_hdf4 fails coda_cursor_set_product inside init_dimensions
  // (HDF4 is disabled in google3).
  {
    CodaMemProduct prod(coda_format_hdf4);
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("OCO", "oco2_LtCO2", 10));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
}

TEST(HarpIngestOcoLtco2Test, OutOfMemoryInIngestionInit) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("OCO_OCO2_LtCO2");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateValidLtCo2(prod, "oco2_LtCO2", /*version=*/10, /*has_corners=*/false,
                     /*use_uint8_flags=*/true);

  harp_ingestion_options* raw_opts = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_ingestion_options_new(&raw_opts));
  ScopedHarpIngestionOptions opts(raw_opts);

  const int swept = SweepOomUntilSuccess([&]() {
    harp_product_definition* prod_def = nullptr;
    void* user_data = nullptr;
    if (mod->ingestion_init(mod, prod.product(), opts.get(), &prod_def,
                            &user_data) == HARP_SUCCESS) {
      ScopedSuppressOom suppress;
      if (mod->ingestion_done != nullptr) {
        mod->ingestion_done(user_data);
      }
    }
  });
  EXPECT_GT(swept, 0);
}

}  // namespace
}  // namespace stcorp_harp
