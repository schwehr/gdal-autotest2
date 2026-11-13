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
// Unit tests for GEOMS UVVIS Brewer ingestion module
// (libharp/harp-ingest-geoms-uvvis-brewer.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-geoms-uvvis-brewer.c

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

using ::testing::DoubleEq;
using ::testing::ElementsAre;
using ::testing::FloatEq;

constexpr char kModuleName[] = "GEOMS-TE-UVVIS-BREWER-TOTALCOL";
constexpr float kFloatFill = -9999.0f;
constexpr double kDoubleFill = -9999.0;

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

CodaMemNode* MakeDoubleArrayWithFill(CodaMemProduct& prod,
                                     std::span<const long> dims,
                                     std::span<const double> values,
                                     double fill_value) {
  CodaMemNode* node = prod.MakeDoubleArray(dims, values);
  if (node == nullptr) {
    return nullptr;
  }
  if (!prod.AddAttribute(node, "VAR_FILL_VALUE", prod.MakeDouble(fill_value))) {
    return nullptr;
  }
  return node;
}

void PopulateBrewerProduct(CodaMemProduct& prod, long num_time,
                           bool with_effective_temperature, bool use_nan_fill) {
  ASSERT_TRUE(
      prod.SetProductInfo("GEOMS", "GEOMS_TE_UVVIS_BREWER_TOTALCOL", 1));
  ASSERT_TRUE(
      prod.AddAttribute(prod.root(), "DATA_TEMPLATE",
                        prod.MakeString("GEOMS-TE-UVVIS-BREWER-TOTALCOL-001")));
  ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_SOURCE",
                                prod.MakeString("UVVIS.BREWER_017")));
  ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_LOCATION",
                                prod.MakeString("DeBilt")));

  const double d_fill =
      use_nan_fill ? std::numeric_limits<double>::quiet_NaN() : kDoubleFill;
  const float f_fill =
      use_nan_fill ? std::numeric_limits<float>::quiet_NaN() : kFloatFill;

  const long time_dims[] = {num_time};
  std::vector<double> datetime_vals(static_cast<size_t>(num_time));
  for (long i = 0; i < num_time; ++i) {
    datetime_vals[static_cast<size_t>(i)] = 6000.0 + static_cast<double>(i);
  }
  if (!use_nan_fill && num_time > 2) {
    datetime_vals[2] = d_fill;
  }
  ASSERT_TRUE(prod.AddField(
      prod.root(), "DATETIME",
      MakeDoubleArrayWithFill(prod, time_dims, datetime_vals, d_fill)));

  ASSERT_TRUE(
      prod.AddField(prod.root(), "LATITUDE_INSTRUMENT",
                    MakeFloatScalarArrayWithFill(prod, 52.10f, f_fill)));
  ASSERT_TRUE(prod.AddField(prod.root(), "LONGITUDE_INSTRUMENT",
                            MakeFloatScalarArrayWithFill(prod, 5.18f, f_fill)));
  ASSERT_TRUE(prod.AddField(prod.root(), "ALTITUDE_INSTRUMENT",
                            MakeFloatScalarArrayWithFill(prod, 10.0f, f_fill)));

  struct FieldSpec {
    const char* name;
    float base;
  };
  constexpr FieldSpec kFields[] = {
      {"ANGLE_SOLAR_AZIMUTH", 180.0f},
      {"ANGLE_SOLAR_ZENITH", 40.0f},
      {"O3_COLUMN_ABSORPTION_SOLAR", 310.0f},
      {"O3_COLUMN_ABSORPTION_SOLAR_UNCERTAINTY_COMBINED_STANDARD", 5.0f},
      {"O3_COLUMN_ABSORPTION_SOLAR_UNCERTAINTY_RANDOM_STANDARD", 3.0f},
      {"O3_COLUMN_ABSORPTION_SOLAR_UNCERTAINTY_SYSTEMATIC_STANDARD", 4.0f},
      {"O3_COLUMN_ABSORPTION_SOLAR_AMF", 1.3f},
  };
  for (const auto& f : kFields) {
    std::vector<float> vals(static_cast<size_t>(num_time));
    for (long i = 0; i < num_time; ++i) {
      vals[static_cast<size_t>(i)] = f.base + static_cast<float>(i);
    }
    if (!use_nan_fill && num_time > 1) {
      vals[1] = f_fill;
    }
    ASSERT_TRUE(
        prod.AddField(prod.root(), f.name,
                      MakeFloatArrayWithFill(prod, time_dims, vals, f_fill)));
  }

  if (with_effective_temperature) {
    std::vector<float> temp_vals(static_cast<size_t>(num_time), 225.0f);
    ASSERT_TRUE(prod.AddField(
        prod.root(), "TEMPERATURE_EFFECTIVE_O3",
        MakeFloatArrayWithFill(prod, time_dims, temp_vals, f_fill)));
  }
}

TEST(HarpIngestGeomsUvvisBrewerTest, IngestSuccessWithEffectiveTemperature) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule(kModuleName);
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateBrewerProduct(prod, /*num_time=*/3,
                        /*with_effective_temperature=*/true,
                        /*use_nan_fill=*/false);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(3L, res.dimensions[harp_dimension_time]);
  EXPECT_THAT(res.variables["sensor_name"].string_values,
              ElementsAre("UVVIS.BREWER_017"));
  EXPECT_THAT(res.variables["location_name"].string_values,
              ElementsAre("DeBilt"));
  const auto& dt = res.variables["datetime"].double_values;
  ASSERT_EQ(3u, dt.size());
  EXPECT_THAT(dt[0], DoubleEq(6000.0));
  EXPECT_THAT(dt[1], DoubleEq(6001.0));
  EXPECT_TRUE(std::isnan(dt[2]));
  EXPECT_THAT(res.variables["sensor_latitude"].float_values,
              ElementsAre(FloatEq(52.10f)));

  const auto& o3 = res.variables["O3_column_number_density"].float_values;
  ASSERT_EQ(3u, o3.size());
  EXPECT_FLOAT_EQ(310.0f, o3[0]);
  EXPECT_TRUE(std::isnan(o3[1]));
  EXPECT_FLOAT_EQ(312.0f, o3[2]);

  EXPECT_TRUE(res.variables["O3_effective_temperature"].included);
  EXPECT_THAT(res.variables["O3_effective_temperature"].float_values,
              ElementsAre(FloatEq(225.0f), FloatEq(225.0f), FloatEq(225.0f)));
}

TEST(HarpIngestGeomsUvvisBrewerTest, IngestSuccessWithoutEffectiveTempNanFill) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule(kModuleName);
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateBrewerProduct(prod, /*num_time=*/1,
                        /*with_effective_temperature=*/false,
                        /*use_nan_fill=*/true);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(1L, res.dimensions[harp_dimension_time]);
  EXPECT_FALSE(res.variables["O3_effective_temperature"].included);
}

TEST(HarpIngestGeomsUvvisBrewerTest, InitErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule(kModuleName);
  ASSERT_NE(nullptr, mod);

  // 1. Null product triggers coda_get_product_version failure.
  {
    ScopedHarpIngestionOptions opts;
    harp_ingestion_options* raw_opts = nullptr;
    ASSERT_EQ(0, harp_ingestion_options_new(&raw_opts));
    opts.reset(raw_opts);
    harp_product_definition* prod_def = nullptr;
    void* user_data = nullptr;
    EXPECT_EQ(-1, mod->ingestion_init(mod, nullptr, opts.get(), &prod_def,
                                      &user_data));
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 2. HDF4 format product triggers coda_cursor_set_product failure (HDF4
  // support is disabled in google3).
  {
    CodaMemProduct prod(coda_format_hdf4);
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. Missing @DATA_TEMPLATE.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.SetProductInfo("GEOMS", "GEOMS_TE_UVVIS_BREWER_TOTALCOL", 1));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 3. Non-string @DATA_TEMPLATE.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.SetProductInfo("GEOMS", "GEOMS_TE_UVVIS_BREWER_TOTALCOL", 1));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "DATA_TEMPLATE", prod.MakeInt32(1)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 4. Invalid @DATA_TEMPLATE length.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.SetProductInfo("GEOMS", "GEOMS_TE_UVVIS_BREWER_TOTALCOL", 1));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_TEMPLATE",
                                  prod.MakeString("BAD")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 5. Invalid @DATA_TEMPLATE prefix (length 34).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.SetProductInfo("GEOMS", "GEOMS_TE_UVVIS_BREWER_TOTALCOL", 1));
    ASSERT_TRUE(prod.AddAttribute(
        prod.root(), "DATA_TEMPLATE",
        prod.MakeString("GEOMS-TE-UVVIS-XXXXXX-TOTALCOL-001")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 6. Missing /@DATA_SOURCE.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.SetProductInfo("GEOMS", "GEOMS_TE_UVVIS_BREWER_TOTALCOL", 1));
    ASSERT_TRUE(prod.AddAttribute(
        prod.root(), "DATA_TEMPLATE",
        prod.MakeString("GEOMS-TE-UVVIS-BREWER-TOTALCOL-001")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 7. Non-string /@DATA_SOURCE triggers coda_cursor_read_string error.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.SetProductInfo("GEOMS", "GEOMS_TE_UVVIS_BREWER_TOTALCOL", 1));
    ASSERT_TRUE(prod.AddAttribute(
        prod.root(), "DATA_TEMPLATE",
        prod.MakeString("GEOMS-TE-UVVIS-BREWER-TOTALCOL-001")));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "DATA_SOURCE", prod.MakeInt32(9)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 8. Invalid /@DATA_SOURCE prefix.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.SetProductInfo("GEOMS", "GEOMS_TE_UVVIS_BREWER_TOTALCOL", 1));
    ASSERT_TRUE(prod.AddAttribute(
        prod.root(), "DATA_TEMPLATE",
        prod.MakeString("GEOMS-TE-UVVIS-BREWER-TOTALCOL-001")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_SOURCE",
                                  prod.MakeString("UVVIS.OTHER_001")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 9. Unsupported template version (e.g. -002).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.SetProductInfo("GEOMS", "GEOMS_TE_UVVIS_BREWER_TOTALCOL", 2));
    ASSERT_TRUE(prod.AddAttribute(
        prod.root(), "DATA_TEMPLATE",
        prod.MakeString("GEOMS-TE-UVVIS-BREWER-TOTALCOL-002")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_SOURCE",
                                  prod.MakeString("UVVIS.BREWER_017")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 10. Missing /DATETIME.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.SetProductInfo("GEOMS", "GEOMS_TE_UVVIS_BREWER_TOTALCOL", 1));
    ASSERT_TRUE(prod.AddAttribute(
        prod.root(), "DATA_TEMPLATE",
        prod.MakeString("GEOMS-TE-UVVIS-BREWER-TOTALCOL-001")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_SOURCE",
                                  prod.MakeString("UVVIS.BREWER_017")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 11. Wrong type for /DATETIME with num_time > 1.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.SetProductInfo("GEOMS", "GEOMS_TE_UVVIS_BREWER_TOTALCOL", 1));
    ASSERT_TRUE(prod.AddAttribute(
        prod.root(), "DATA_TEMPLATE",
        prod.MakeString("GEOMS-TE-UVVIS-BREWER-TOTALCOL-001")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_SOURCE",
                                  prod.MakeString("UVVIS.BREWER_017")));
    constexpr long kDims[] = {2};
    constexpr std::string_view kBadTimes[] = {"a", "b"};
    ASSERT_TRUE(prod.AddField(prod.root(), "DATETIME",
                              prod.MakeStringArray(kDims, kBadTimes)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 12. Non-chronological /DATETIME.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.SetProductInfo("GEOMS", "GEOMS_TE_UVVIS_BREWER_TOTALCOL", 1));
    ASSERT_TRUE(prod.AddAttribute(
        prod.root(), "DATA_TEMPLATE",
        prod.MakeString("GEOMS-TE-UVVIS-BREWER-TOTALCOL-001")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_SOURCE",
                                  prod.MakeString("UVVIS.BREWER_017")));
    constexpr long kDims[] = {2};
    constexpr double kTimes[] = {20.0, 10.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "DATETIME",
                              prod.MakeDoubleArray(kDims, kTimes)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
}

TEST(HarpIngestGeomsUvvisBrewerTest, VariableReadErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule(kModuleName);
  ASSERT_NE(nullptr, mod);

  // 1. Wrong type for 1-element DATETIME + missing/non-string DATA_LOCATION +
  // missing/wrong-size/wrong-type/missing-fill/wrong-fill float fields.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.SetProductInfo("GEOMS", "GEOMS_TE_UVVIS_BREWER_TOTALCOL", 1));
    ASSERT_TRUE(prod.AddAttribute(
        prod.root(), "DATA_TEMPLATE",
        prod.MakeString("GEOMS-TE-UVVIS-BREWER-TOTALCOL-001")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_SOURCE",
                                  prod.MakeString("UVVIS.BREWER_017")));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "DATA_LOCATION", prod.MakeInt32(7)));

    constexpr long kTimeDims[] = {1};
    constexpr std::string_view kBadTime[] = {"bad"};
    ASSERT_TRUE(prod.AddField(prod.root(), "DATETIME",
                              prod.MakeStringArray(kTimeDims, kBadTime)));

    constexpr long kTwoDims[] = {2};
    constexpr float kTwoFloats[] = {1.0f, 2.0f};
    ASSERT_TRUE(prod.AddField(prod.root(), "LATITUDE_INSTRUMENT",
                              prod.MakeFloatArray(kTwoDims, kTwoFloats)));
    ASSERT_TRUE(prod.AddField(prod.root(), "LONGITUDE_INSTRUMENT",
                              prod.MakeString("x")));
    constexpr float kOneFloat[] = {40.0f};
    ASSERT_TRUE(prod.AddField(prod.root(), "ALTITUDE_INSTRUMENT",
                              prod.MakeFloatArray(kTimeDims, kOneFloat)));
    CodaMemNode* sza = prod.MakeFloatArray(kTimeDims, kOneFloat);
    ASSERT_TRUE(
        prod.AddAttribute(sza, "VAR_FILL_VALUE", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "ANGLE_SOLAR_ZENITH", sza));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["location_name"].read_status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_latitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_longitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_altitude"].read_status);
    EXPECT_EQ(-1, res.variables["solar_zenith_angle"].read_status);
    EXPECT_EQ(-1, res.variables["solar_azimuth_angle"].read_status);
  }

  // 2. DATETIME missing @VAR_FILL_VALUE and wrong @VAR_FILL_VALUE type.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.SetProductInfo("GEOMS", "GEOMS_TE_UVVIS_BREWER_TOTALCOL", 1));
    ASSERT_TRUE(prod.AddAttribute(
        prod.root(), "DATA_TEMPLATE",
        prod.MakeString("GEOMS-TE-UVVIS-BREWER-TOTALCOL-001")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_SOURCE",
                                  prod.MakeString("UVVIS.BREWER_017")));
    constexpr long kTimeDims[] = {1};
    constexpr double kTimes[] = {6000.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "DATETIME",
                              prod.MakeDoubleArray(kTimeDims, kTimes)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["location_name"].read_status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.SetProductInfo("GEOMS", "GEOMS_TE_UVVIS_BREWER_TOTALCOL", 1));
    ASSERT_TRUE(prod.AddAttribute(
        prod.root(), "DATA_TEMPLATE",
        prod.MakeString("GEOMS-TE-UVVIS-BREWER-TOTALCOL-001")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_SOURCE",
                                  prod.MakeString("UVVIS.BREWER_017")));
    constexpr long kTimeDims[] = {1};
    constexpr double kTimes[] = {6000.0};
    CodaMemNode* dt = prod.MakeDoubleArray(kTimeDims, kTimes);
    ASSERT_TRUE(
        prod.AddAttribute(dt, "VAR_FILL_VALUE", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "DATETIME", dt));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
