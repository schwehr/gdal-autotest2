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
// Unit tests for GEOMS FTIR TCCON ingestion module
// (libharp/harp-ingest-geoms-tccon.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-geoms-tccon.c

#include <cmath>
#include <limits>
#include <span>
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

using ::testing::ElementsAre;
using ::testing::FloatEq;

constexpr char kModuleName[] = "GEOMS-TE-FTIR-TCCON";
constexpr float kFillValue = -9999.0f;

CodaMemNode* MakeFloatArrayWithFill(CodaMemProduct& prod,
                                    std::span<const long> dims,
                                    std::span<const float> values,
                                    float fill_value) {
  CodaMemNode* node = prod.MakeFloatArray(dims, values);
  if (node == nullptr) {
    return nullptr;
  }
  if (!prod.AddAttribute(node, "VAR_FILL_VALUE", prod.MakeFloat(fill_value))) {
    return nullptr;
  }
  return node;
}

CodaMemNode* MakeFloatScalarArrayWithFill(CodaMemProduct& prod, float value,
                                          float fill_value) {
  const long dims[] = {1};
  const float vals[] = {value};
  return MakeFloatArrayWithFill(prod, dims, vals, fill_value);
}

std::vector<float> MakeSequence(long count, float base, float step) {
  std::vector<float> out(static_cast<size_t>(count));
  for (long i = 0; i < count; ++i) {
    out[static_cast<size_t>(i)] = base + static_cast<float>(i) * step;
  }
  return out;
}

void PopulateTcconProduct(CodaMemProduct& prod, std::string_view template_name,
                          long num_time, long num_vertical, bool altitude_2d,
                          float fill_value) {
  ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_TEMPLATE",
                                prod.MakeString(template_name)));
  ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_SOURCE",
                                prod.MakeString("FTIR.TCCON_001")));
  ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_LOCATION",
                                prod.MakeString("ParkFalls")));

  const long time_dims[] = {num_time};
  std::vector<double> datetime_vals(static_cast<size_t>(num_time));
  for (long t = 0; t < num_time; ++t) {
    datetime_vals[static_cast<size_t>(t)] = 5000.0 + static_cast<double>(t);
  }
  ASSERT_TRUE(prod.AddField(prod.root(), "DATETIME",
                            prod.MakeDoubleArray(time_dims, datetime_vals)));

  ASSERT_TRUE(
      prod.AddField(prod.root(), "LATITUDE_INSTRUMENT",
                    MakeFloatScalarArrayWithFill(prod, 45.945f, fill_value)));
  ASSERT_TRUE(
      prod.AddField(prod.root(), "LONGITUDE_INSTRUMENT",
                    MakeFloatScalarArrayWithFill(prod, -90.273f, fill_value)));
  ASSERT_TRUE(
      prod.AddField(prod.root(), "ALTITUDE_INSTRUMENT",
                    MakeFloatScalarArrayWithFill(prod, 0.442f, fill_value)));

  if (altitude_2d) {
    const long alt_dims[] = {num_time, num_vertical};
    auto alt_vals = MakeSequence(num_time * num_vertical, 0.5f, 1.0f);
    ASSERT_TRUE(prod.AddField(
        prod.root(), "ALTITUDE",
        MakeFloatArrayWithFill(prod, alt_dims, alt_vals, fill_value)));
  } else {
    const long alt_dims[] = {num_vertical};
    auto alt_vals = MakeSequence(num_vertical, 0.5f, 1.0f);
    if (!std::isnan(fill_value)) {
      alt_vals[0] = fill_value;
    }
    ASSERT_TRUE(prod.AddField(
        prod.root(), "ALTITUDE",
        MakeFloatArrayWithFill(prod, alt_dims, alt_vals, fill_value)));
  }

  const long prof_dims[] = {num_time, num_vertical};
  const long prof_count = num_time * num_vertical;

  struct FieldSpec {
    const char* name;
    bool is_profile;
    float base;
  };
  constexpr FieldSpec kCommonFields[] = {
      {"SURFACE_PRESSURE_INDEPENDENT", false, 980.0f},
      {"SURFACE_TEMPERATURE_INDEPENDENT", false, 285.0f},
      {"PRESSURE_INDEPENDENT", true, 900.0f},
      {"TEMPERATURE_INDEPENDENT", true, 270.0f},
      {"N2O_COLUMN_MIXING_RATIO_VOLUME_DRY_ABSORPTION_SOLAR", false, 320.0f},
      {"N2O_COLUMN_MIXING_RATIO_VOLUME_DRY_ABSORPTION_SOLAR_UNCERTAINTY_RANDOM_"
       "STANDARD",
       false, 2.0f},
      {"N2O_MIXING_RATIO_VOLUME_APRIORI", true, 315.0f},
      {"N2O_COLUMN_ABSORPTION_SOLAR_AVK", true, 0.9f},
      {"HF_COLUMN_MIXING_RATIO_VOLUME_DRY_ABSORPTION_SOLAR", false, 150.0f},
      {"HF_COLUMN_MIXING_RATIO_VOLUME_DRY_ABSORPTION_SOLAR_UNCERTAINTY_RANDOM_"
       "STANDARD",
       false, 3.0f},
      {"HF_MIXING_RATIO_VOLUME_APRIORI", true, 140.0f},
      {"HF_COLUMN_ABSORPTION_SOLAR_AVK", true, 0.8f},
      {"HDO_COLUMN_MIXING_RATIO_VOLUME_DRY_ABSORPTION_SOLAR", false, 200.0f},
      {"HDO_COLUMN_MIXING_RATIO_VOLUME_DRY_ABSORPTION_SOLAR_UNCERTAINTY_RANDOM_"
       "STANDARD",
       false, 4.0f},
      {"HDO_MIXING_RATIO_VOLUME_APRIORI", true, 190.0f},
      {"HDO_COLUMN_ABSORPTION_SOLAR_AVK", true, 0.7f},
      {"H2O_COLUMN_MIXING_RATIO_VOLUME_DRY_ABSORPTION_SOLAR", false, 1500.0f},
      {"H2O_COLUMN_MIXING_RATIO_VOLUME_DRY_ABSORPTION_SOLAR_UNCERTAINTY_RANDOM_"
       "STANDARD",
       false, 10.0f},
      {"H2O_MIXING_RATIO_VOLUME_APRIORI", true, 1400.0f},
      {"H2O_COLUMN_ABSORPTION_SOLAR_AVK", true, 0.85f},
      {"CO_COLUMN_MIXING_RATIO_VOLUME_DRY_ABSORPTION_SOLAR", false, 95.0f},
      {"CO_COLUMN_MIXING_RATIO_VOLUME_DRY_ABSORPTION_SOLAR_UNCERTAINTY_RANDOM_"
       "STANDARD",
       false, 1.5f},
      {"CO_MIXING_RATIO_VOLUME_APRIORI", true, 90.0f},
      {"CO_COLUMN_ABSORPTION_SOLAR_AVK", true, 0.95f},
      {"CO2_COLUMN_MIXING_RATIO_VOLUME_DRY_ABSORPTION_SOLAR", false, 410.0f},
      {"CO2_COLUMN_MIXING_RATIO_VOLUME_DRY_ABSORPTION_SOLAR_UNCERTAINTY_RANDOM_"
       "STANDARD",
       false, 0.5f},
      {"CO2_MIXING_RATIO_VOLUME_APRIORI", true, 405.0f},
      {"CO2_COLUMN_ABSORPTION_SOLAR_AVK", true, 1.0f},
      {"CH4_COLUMN_MIXING_RATIO_VOLUME_DRY_ABSORPTION_SOLAR", false, 1.85f},
      {"CH4_COLUMN_MIXING_RATIO_VOLUME_DRY_ABSORPTION_SOLAR_UNCERTAINTY_RANDOM_"
       "STANDARD",
       false, 0.02f},
      {"CH4_MIXING_RATIO_VOLUME_APRIORI", true, 1.80f},
      {"CH4_COLUMN_ABSORPTION_SOLAR_AVK", true, 0.98f},
      {"ANGLE_SOLAR_AZIMUTH", false, 150.0f},
      {"ANGLE_SOLAR_ZENITH_ASTRONOMICAL", false, 35.0f},
      {"GRAVITY_INDEPENDENT", true, 9.8f},
      {"WIND_SPEED_SURFACE_INDEPENDENT", false, 4.5f},
      {"WIND_DIRECTION_SURFACE_INDEPENDENT", false, 210.0f},
      {"HUMIDITY_RELATIVE_SURFACE_INDEPENDENT", false, 55.0f},
      {"NUMBER_DENSITY_INDEPENDENT", true, 2.5e19f},
  };

  for (const auto& f : kCommonFields) {
    if (f.is_profile) {
      auto vals = MakeSequence(prof_count, f.base, 0.1f);
      ASSERT_TRUE(prod.AddField(
          prod.root(), f.name,
          MakeFloatArrayWithFill(prod, prof_dims, vals, fill_value)));
    } else {
      auto vals = MakeSequence(num_time, f.base, 0.5f);
      ASSERT_TRUE(prod.AddField(
          prod.root(), f.name,
          MakeFloatArrayWithFill(prod, time_dims, vals, fill_value)));
    }
  }

  if (template_name.ends_with("006") || template_name.ends_with("007")) {
    auto amf_vals = MakeSequence(num_time, 1.2f, 0.1f);
    ASSERT_TRUE(prod.AddField(
        prod.root(), "O2_COLUMN_ABSORPTION_SOLAR_AMF",
        MakeFloatArrayWithFill(prod, time_dims, amf_vals, fill_value)));
  }
  if (template_name.ends_with("006")) {
    auto o2_ap = MakeSequence(prof_count, 0.2095f, 0.0001f);
    auto o2_avk = MakeSequence(prof_count, 1.0f, 0.01f);
    ASSERT_TRUE(prod.AddField(
        prod.root(), "O2_MIXING_RATIO_VOLUME_APRIORI",
        MakeFloatArrayWithFill(prod, prof_dims, o2_ap, fill_value)));
    ASSERT_TRUE(prod.AddField(
        prod.root(), "O2_COLUMN_ABSORPTION_SOLAR_AVK",
        MakeFloatArrayWithFill(prod, prof_dims, o2_avk, fill_value)));
  } else if (template_name.ends_with("007")) {
    auto o2_col = MakeSequence(num_time, 4.5e24f, 1e22f);
    auto o2_unc = MakeSequence(num_time, 1e22f, 1e20f);
    auto o2_ap = MakeSequence(num_time, 4.4e24f, 1e22f);
    ASSERT_TRUE(prod.AddField(
        prod.root(), "O2_COLUMN_ABSORPTION_SOLAR",
        MakeFloatArrayWithFill(prod, time_dims, o2_col, fill_value)));
    ASSERT_TRUE(prod.AddField(
        prod.root(), "O2_COLUMN_ABSORPTION_SOLAR_UNCERTAINTY_RANDOM_STANDARD",
        MakeFloatArrayWithFill(prod, time_dims, o2_unc, fill_value)));
    ASSERT_TRUE(prod.AddField(
        prod.root(), "O2_COLUMN_ABSORPTION_SOLAR_APRIORI",
        MakeFloatArrayWithFill(prod, time_dims, o2_ap, fill_value)));
  }
}

TEST(HarpIngestGeomsTcconTest, IngestVersion005With1DAltitudeAndFillValue) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule(kModuleName);
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateTcconProduct(prod, "GEOMS-TE-FTIR-TCCON-005", /*num_time=*/2,
                       /*num_vertical=*/3, /*altitude_2d=*/false, kFillValue);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(2L, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(3L, res.dimensions[harp_dimension_vertical]);
  EXPECT_THAT(res.variables["sensor_name"].string_values,
              ElementsAre("FTIR.TCCON_001"));
  EXPECT_THAT(res.variables["location_name"].string_values,
              ElementsAre("ParkFalls"));
  EXPECT_THAT(res.variables["datetime"].double_values,
              ElementsAre(5000.0, 5001.0));
  EXPECT_THAT(res.variables["sensor_latitude"].float_values,
              ElementsAre(FloatEq(45.945f)));

  // 1D ALTITUDE is replicated across time, and fill_value -> NaN.
  const auto& alt = res.variables["altitude"].float_values;
  ASSERT_EQ(6u, alt.size());
  EXPECT_TRUE(std::isnan(alt[0]));
  EXPECT_FLOAT_EQ(1.5f, alt[1]);
  EXPECT_FLOAT_EQ(2.5f, alt[2]);
  EXPECT_TRUE(std::isnan(alt[3]));
  EXPECT_FLOAT_EQ(1.5f, alt[4]);
  EXPECT_FLOAT_EQ(2.5f, alt[5]);
}

TEST(HarpIngestGeomsTcconTest, IngestVersion006And007With2DAltitudeAndNanFill) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule(kModuleName);
  ASSERT_NE(nullptr, mod);

  const float nan_fill = std::numeric_limits<float>::quiet_NaN();

  // Version 006 (2D altitude, NaN fill value).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateTcconProduct(prod, "GEOMS-TE-FTIR-TCCON-006", /*num_time=*/2,
                         /*num_vertical=*/2, /*altitude_2d=*/true, nan_fill);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["altitude"].float_values,
                ElementsAre(0.5f, 1.5f, 2.5f, 3.5f));
    EXPECT_TRUE(res.variables.contains("O2_volume_mixing_ratio_apriori"));
    EXPECT_TRUE(res.variables.contains("O2_column_number_density_avk"));
    EXPECT_TRUE(res.variables.contains("O2_column_number_density_amf"));
  }

  // Version 007 (1 time step, 2D altitude).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateTcconProduct(prod, "GEOMS-TE-FTIR-TCCON-007", /*num_time=*/1,
                         /*num_vertical=*/2, /*altitude_2d=*/true, kFillValue);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(1L, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(2L, res.dimensions[harp_dimension_vertical]);
    EXPECT_TRUE(res.variables.contains("O2_column_number_density"));
    EXPECT_TRUE(res.variables.contains("O2_column_number_density_uncertainty"));
    EXPECT_TRUE(res.variables.contains("O2_column_number_density_apriori"));
    EXPECT_TRUE(res.variables.contains("O2_column_number_density_amf"));
  }
}

TEST(HarpIngestGeomsTcconTest, InitErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule(kModuleName);
  ASSERT_NE(nullptr, mod);

  // 1. Missing @DATA_TEMPLATE.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 2. Non-string @DATA_TEMPLATE triggers get_string_length failure.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "DATA_TEMPLATE", prod.MakeInt32(123)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. Invalid length for @DATA_TEMPLATE.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_TEMPLATE",
                                  prod.MakeString("SHORT")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 4. Length 24 for @DATA_TEMPLATE is truncated to 23 chars by read_string.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_TEMPLATE",
                                  prod.MakeString("GEOMS-TE-FTIR-TCCON-001x")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 5. Unsupported template version of valid length 23.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_TEMPLATE",
                                  prod.MakeString("GEOMS-TE-FTIR-TCCON-001")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 6. Missing /DATETIME.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_TEMPLATE",
                                  prod.MakeString("GEOMS-TE-FTIR-TCCON-005")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 7. Wrong type for /DATETIME with num_time > 1.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_TEMPLATE",
                                  prod.MakeString("GEOMS-TE-FTIR-TCCON-005")));
    constexpr long kDims[] = {2};
    constexpr std::string_view kBadTimes[] = {"a", "b"};
    ASSERT_TRUE(prod.AddField(prod.root(), "DATETIME",
                              prod.MakeStringArray(kDims, kBadTimes)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 8. Non-chronological /DATETIME.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_TEMPLATE",
                                  prod.MakeString("GEOMS-TE-FTIR-TCCON-005")));
    constexpr long kDims[] = {2};
    constexpr double kTimes[] = {10.0, 5.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "DATETIME",
                              prod.MakeDoubleArray(kDims, kTimes)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 9. Missing /ALTITUDE.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_TEMPLATE",
                                  prod.MakeString("GEOMS-TE-FTIR-TCCON-005")));
    constexpr long kDims[] = {1};
    constexpr double kTimes[] = {10.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "DATETIME",
                              prod.MakeDoubleArray(kDims, kTimes)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 10. Scalar /ALTITUDE fails coda_cursor_get_array_dim.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_TEMPLATE",
                                  prod.MakeString("GEOMS-TE-FTIR-TCCON-005")));
    constexpr long kDims[] = {1};
    constexpr double kTimes[] = {10.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "DATETIME",
                              prod.MakeDoubleArray(kDims, kTimes)));
    ASSERT_TRUE(prod.AddField(prod.root(), "ALTITUDE", prod.MakeFloat(1.0f)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 11. 3D /ALTITUDE fails dimensionality check.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_TEMPLATE",
                                  prod.MakeString("GEOMS-TE-FTIR-TCCON-005")));
    constexpr long kDims[] = {1};
    constexpr double kTimes[] = {10.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "DATETIME",
                              prod.MakeDoubleArray(kDims, kTimes)));
    constexpr long kAltDims[] = {1, 1, 1};
    constexpr float kAltVals[] = {1.0f};
    ASSERT_TRUE(prod.AddField(prod.root(), "ALTITUDE",
                              prod.MakeFloatArray(kAltDims, kAltVals)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
}

TEST(HarpIngestGeomsTcconTest, VariableReadErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule(kModuleName);
  ASSERT_NE(nullptr, mod);

  // 1. Missing attributes, wrong-type attributes, wrong-type DATETIME (1 elem),
  // missing/wrong-type/wrong-size float fields, and missing/wrong-type fill
  // values.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_TEMPLATE",
                                  prod.MakeString("GEOMS-TE-FTIR-TCCON-005")));
    // Non-string DATA_SOURCE -> get_string_length error; missing DATA_LOCATION.
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "DATA_SOURCE", prod.MakeInt32(42)));
    // String DATETIME of 1 element -> read_double_array error.
    constexpr long kTimeDims[] = {1};
    constexpr std::string_view kBadTime[] = {"bad"};
    ASSERT_TRUE(prod.AddField(prod.root(), "DATETIME",
                              prod.MakeStringArray(kTimeDims, kBadTime)));
    // Valid 1D ALTITUDE without @VAR_FILL_VALUE -> goto @VAR_FILL_VALUE error.
    constexpr long kAltDims[] = {2};
    constexpr float kAltVals[] = {1.0f, 2.0f};
    ASSERT_TRUE(prod.AddField(prod.root(), "ALTITUDE",
                              prod.MakeFloatArray(kAltDims, kAltVals)));
    // LATITUDE_INSTRUMENT with wrong element count (2 instead of 1).
    ASSERT_TRUE(prod.AddField(prod.root(), "LATITUDE_INSTRUMENT",
                              prod.MakeFloatArray(kAltDims, kAltVals)));
    // LONGITUDE_INSTRUMENT with wrong type (string).
    ASSERT_TRUE(prod.AddField(prod.root(), "LONGITUDE_INSTRUMENT",
                              prod.MakeString("x")));
    // ALTITUDE_INSTRUMENT with wrong @VAR_FILL_VALUE type (string).
    constexpr float kAltInst[] = {0.5f};
    CodaMemNode* alt_inst = prod.MakeFloatArray(kTimeDims, kAltInst);
    ASSERT_TRUE(
        prod.AddAttribute(alt_inst, "VAR_FILL_VALUE", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "ALTITUDE_INSTRUMENT", alt_inst));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["sensor_name"].read_status);
    EXPECT_EQ(-1, res.variables["location_name"].read_status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["altitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_latitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_longitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_altitude"].read_status);
    EXPECT_EQ(-1, res.variables["surface_pressure"].read_status);
  }

  // 2. read_variable_float_opt_time_dep failure when num_time > 1 and 1D
  // ALTITUDE is missing @VAR_FILL_VALUE.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_TEMPLATE",
                                  prod.MakeString("GEOMS-TE-FTIR-TCCON-005")));
    constexpr long kTimeDims[] = {2};
    constexpr double kTimes[] = {1.0, 2.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "DATETIME",
                              prod.MakeDoubleArray(kTimeDims, kTimes)));
    constexpr long kAltDims[] = {2};
    constexpr float kAltVals[] = {1.0f, 2.0f};
    ASSERT_TRUE(prod.AddField(prod.root(), "ALTITUDE",
                              prod.MakeFloatArray(kAltDims, kAltVals)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["altitude"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
