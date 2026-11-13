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
// Unit tests for Cloudnet L2 classification ingestion module
// (libharp/harp-ingest-cloudnet.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-cloudnet.c

#include <cmath>
#include <cstdint>
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

void PopulateCloudnetProduct(CodaMemProduct& prod, bool use_amsl,
                             bool scalar_geo, bool int32_classifications,
                             bool custom_fill_attr) {
  constexpr long kTimeDims[] = {2};
  constexpr double kTimes[] = {1.0, 2.5};
  CodaMemNode* time_node = prod.MakeDoubleArray(kTimeDims, kTimes);
  ASSERT_NE(nullptr, time_node);
  ASSERT_TRUE(prod.AddAttribute(
      time_node, "units", prod.MakeString("hours since 2020-05-15 00:00:00")));
  ASSERT_TRUE(prod.AddField(prod.root(), "time", time_node));

  constexpr long kOneDim[] = {1};
  if (scalar_geo) {
    CodaMemNode* lat = prod.MakeFloat(custom_fill_attr ? -999.0f : 51.97f);
    if (custom_fill_attr) {
      constexpr float kLatFill[] = {-999.0f};
      ASSERT_TRUE(prod.AddAttribute(lat, "FillValue",
                                    prod.MakeFloatArray(kOneDim, kLatFill)));
    }
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude", lat));
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude", prod.MakeFloat(4.93f)));
    ASSERT_TRUE(prod.AddField(prod.root(), "altitude",
                              prod.MakeFloat(9.96920996839e+36f)));
  } else {
    constexpr long kGeoDims[] = {3};
    constexpr float kLats[] = {51.0f, 52.0f, 53.0f};
    constexpr float kLons[] = {4.0f, 5.0f, 6.0f};
    constexpr float kAlts[] = {10.0f, 20.0f, 30.0f};
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                              prod.MakeFloatArray(kGeoDims, kLats)));
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                              prod.MakeFloatArray(kGeoDims, kLons)));
    ASSERT_TRUE(prod.AddField(prod.root(), "altitude",
                              prod.MakeFloatArray(kGeoDims, kAlts)));
  }

  constexpr long kHeightDims[] = {3};
  constexpr float kHeights[] = {100.0f, -999.0f, 300.0f};
  CodaMemNode* height_node = prod.MakeFloatArray(kHeightDims, kHeights);
  if (custom_fill_attr) {
    constexpr float kHeightFill[] = {-999.0f};
    ASSERT_TRUE(prod.AddAttribute(height_node, "FillValue",
                                  prod.MakeFloatArray(kOneDim, kHeightFill)));
  }
  ASSERT_TRUE(prod.AddField(prod.root(), "height", height_node));

  constexpr float kCloudBase[] = {500.0f, 9.96920996839e+36f};
  constexpr float kCloudTop[] = {1500.0f, 1800.0f};
  ASSERT_TRUE(prod.AddField(
      prod.root(), use_amsl ? "cloud_base_height_amsl" : "cloud_base_height",
      prod.MakeFloatArray(kTimeDims, kCloudBase)));
  ASSERT_TRUE(prod.AddField(
      prod.root(), use_amsl ? "cloud_top_height_amsl" : "cloud_top_height",
      prod.MakeFloatArray(kTimeDims, kCloudTop)));

  constexpr long kGridDims[] = {2, 3};
  if (int32_classifications) {
    constexpr int32_t kClasses[] = {0, 1, 2, 3, 4, 5};
    constexpr int32_t kStatus[] = {1, 2, 3, 1, 2, 3};
    ASSERT_TRUE(prod.AddField(prod.root(), "target_classification",
                              prod.MakeInt32Array(kGridDims, kClasses)));
    ASSERT_TRUE(prod.AddField(prod.root(), "detection_status",
                              prod.MakeInt32Array(kGridDims, kStatus)));
  } else {
    constexpr int8_t kClasses[] = {0, 1, 2, 3, 4, 5};
    constexpr int8_t kStatus[] = {1, 2, 3, 1, 2, 3};
    ASSERT_TRUE(prod.AddField(prod.root(), "target_classification",
                              prod.MakeInt8Array(kGridDims, kClasses)));
    ASSERT_TRUE(prod.AddField(prod.root(), "detection_status",
                              prod.MakeInt8Array(kGridDims, kStatus)));
  }
}

TEST(HarpIngestCloudnetTest, IngestLegacyScalarGeoInt32AndCustomFill) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod =
      FindIngestionModule("CLOUDNET_L2_classification");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateCloudnetProduct(prod, /*use_amsl=*/false, /*scalar_geo=*/true,
                          /*int32_classifications=*/true,
                          /*custom_fill_attr=*/true);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(3, res.dimensions[harp_dimension_vertical]);

  double expected_day = 0.0;
  ASSERT_EQ(0,
            coda_time_string_to_double("yyyy-MM-dd HH:mm:ss",
                                       "2020-05-15 00:00:00", &expected_day));
  EXPECT_THAT(res.variables["datetime"].double_values,
              ElementsAre(expected_day + 3600.0, expected_day + 9000.0));

  EXPECT_TRUE(std::isnan(res.variables["sensor_latitude"].float_values[0]));
  EXPECT_FLOAT_EQ(4.93f, res.variables["sensor_longitude"].float_values[0]);
  EXPECT_TRUE(std::isnan(res.variables["sensor_altitude"].float_values[0]));

  const auto& alt = res.variables["altitude"].float_values;
  ASSERT_EQ(3u, alt.size());
  EXPECT_FLOAT_EQ(100.0f, alt[0]);
  EXPECT_TRUE(std::isnan(alt[1]));
  EXPECT_FLOAT_EQ(300.0f, alt[2]);

  const auto& base = res.variables["cloud_base_height"].float_values;
  EXPECT_FLOAT_EQ(500.0f, base[0]);
  EXPECT_TRUE(std::isnan(base[1]));
  EXPECT_THAT(res.variables["cloud_top_height"].float_values,
              ElementsAre(1500.0f, 1800.0f));
  EXPECT_THAT(res.variables["cloud_type"].int8_values,
              ElementsAre(0, 1, 2, 3, 4, 5));
  EXPECT_THAT(res.variables["cloud_type_validity"].int8_values,
              ElementsAre(1, 2, 3, 1, 2, 3));
}

TEST(HarpIngestCloudnetTest, IngestAmslArrayGeoAndInt8Classifications) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod =
      FindIngestionModule("CLOUDNET_L2_classification");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateCloudnetProduct(prod, /*use_amsl=*/true, /*scalar_geo=*/false,
                          /*int32_classifications=*/false,
                          /*custom_fill_attr=*/false);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_FLOAT_EQ(52.0f, res.variables["sensor_latitude"].float_values[0]);
  EXPECT_FLOAT_EQ(5.0f, res.variables["sensor_longitude"].float_values[0]);
  EXPECT_FLOAT_EQ(20.0f, res.variables["sensor_altitude"].float_values[0]);
  EXPECT_FLOAT_EQ(500.0f, res.variables["cloud_base_height"].float_values[0]);
  EXPECT_TRUE(std::isnan(res.variables["cloud_base_height"].float_values[1]));
  EXPECT_THAT(res.variables["cloud_type"].int8_values,
              ElementsAre(0, 1, 2, 3, 4, 5));
}

TEST(HarpIngestCloudnetTest, EmptyDimensionsAndMissingCloudBaseHeight) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod =
      FindIngestionModule("CLOUDNET_L2_classification");
  ASSERT_NE(nullptr, mod);

  // 1. Missing time and missing cloud_base_height -> HARP_ERROR_INGESTION.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 2. Missing time with cloud_base_height and scalar geo -> num_times = 0
  // succeeds.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {0};
    ASSERT_TRUE(prod.AddField(prod.root(), "cloud_base_height",
                              prod.MakeFloatArray(kDims, {})));
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude", prod.MakeFloat(1.0f)));
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude", prod.MakeFloat(2.0f)));
    ASSERT_TRUE(prod.AddField(prod.root(), "altitude", prod.MakeFloat(3.0f)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(0, res.dimensions[harp_dimension_time]);
  }

  // 3. Scalar time and missing height -> num_times = 0, num_altitudes = 0.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "time", prod.MakeDouble(0.0)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "cloud_base_height", prod.MakeFloat(0.0f)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(0, res.dimensions[harp_dimension_time]);
  }

  // 4. Valid time, missing height -> num_altitudes = 0.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kTimeDims[] = {1};
    constexpr double kTimes[] = {1.0};
    constexpr float kOneFloat[] = {100.0f};
    CodaMemNode* time_node = prod.MakeDoubleArray(kTimeDims, kTimes);
    ASSERT_TRUE(
        prod.AddAttribute(time_node, "units",
                          prod.MakeString("hours since 2020-05-15 00:00:00")));
    ASSERT_TRUE(prod.AddField(prod.root(), "time", time_node));
    ASSERT_TRUE(prod.AddField(prod.root(), "cloud_base_height",
                              prod.MakeFloatArray(kTimeDims, kOneFloat)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(0, res.dimensions[harp_dimension_vertical]);
  }

  // 5. Valid time, scalar height -> num_altitudes = 0.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kTimeDims[] = {1};
    constexpr double kTimes[] = {1.0};
    constexpr float kOneFloat[] = {100.0f};
    CodaMemNode* time_node = prod.MakeDoubleArray(kTimeDims, kTimes);
    ASSERT_TRUE(
        prod.AddAttribute(time_node, "units",
                          prod.MakeString("hours since 2020-05-15 00:00:00")));
    ASSERT_TRUE(prod.AddField(prod.root(), "time", time_node));
    ASSERT_TRUE(prod.AddField(prod.root(), "height", prod.MakeFloat(100.0f)));
    ASSERT_TRUE(prod.AddField(prod.root(), "cloud_base_height",
                              prod.MakeFloatArray(kTimeDims, kOneFloat)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(0, res.dimensions[harp_dimension_vertical]);
  }
}

TEST(HarpIngestCloudnetTest, ReadVariableErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod =
      FindIngestionModule("CLOUDNET_L2_classification");
  ASSERT_NE(nullptr, mod);

  // 1. Missing @units on time, wrong-type latitude, empty-array longitude,
  // wrong-type @FillValue on altitude, wrong element count on
  // cloud_base_height, wrong-type float array on cloud_top_height, wrong-type
  // int array on target_classification, scalar int on detection_status.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kTimeDims[] = {2};
    constexpr double kTimes[] = {1.0, 2.0};
    constexpr long kHeightDims[] = {2};
    constexpr float kHeights[] = {100.0f, 200.0f};
    constexpr long kOneDim[] = {1};
    constexpr float kOneFloat[] = {10.0f};
    constexpr std::string_view kBadStrs[] = {"bad", "val"};

    ASSERT_TRUE(prod.AddField(prod.root(), "time",
                              prod.MakeDoubleArray(kTimeDims, kTimes)));
    ASSERT_TRUE(prod.AddField(prod.root(), "height",
                              prod.MakeFloatArray(kHeightDims, kHeights)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "latitude", prod.MakeString("not_a_float")));
    constexpr long kZeroDims[] = {0};
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                              prod.MakeFloatArray(kZeroDims, {})));
    CodaMemNode* alt = prod.MakeFloat(10.0f);
    constexpr std::string_view kOneBadStr[] = {"bad"};
    ASSERT_TRUE(prod.AddAttribute(alt, "FillValue",
                                  prod.MakeStringArray(kOneDim, kOneBadStr)));
    ASSERT_TRUE(prod.AddField(prod.root(), "altitude", alt));

    ASSERT_TRUE(prod.AddField(prod.root(), "cloud_base_height",
                              prod.MakeFloatArray(kOneDim, kOneFloat)));
    ASSERT_TRUE(prod.AddField(prod.root(), "cloud_top_height",
                              prod.MakeStringArray(kTimeDims, kBadStrs)));
    constexpr long kGridDims[] = {2, 2};
    constexpr std::string_view kBadGrid[] = {"a", "b", "c", "d"};
    ASSERT_TRUE(prod.AddField(prod.root(), "target_classification",
                              prod.MakeStringArray(kGridDims, kBadGrid)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "detection_status", prod.MakeInt8(1)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_latitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_longitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_altitude"].read_status);
    EXPECT_EQ(-1, res.variables["cloud_base_height"].read_status);
    EXPECT_EQ(-1, res.variables["cloud_top_height"].read_status);
    EXPECT_EQ(-1, res.variables["cloud_type"].read_status);
    EXPECT_EQ(-1, res.variables["cloud_type_validity"].read_status);
  }

  // 2. Wrong-type @FillValue on float array + invalid datetime string + wrong
  // type time array + wrong-type @units.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kTimeDims[] = {2};
    constexpr std::string_view kBadTimes[] = {"a", "b"};
    constexpr long kHeightDims[] = {2};
    constexpr float kHeights[] = {100.0f, 200.0f};
    constexpr long kOneDim[] = {1};
    constexpr std::string_view kBadFill[] = {"bad"};

    ASSERT_TRUE(prod.AddField(prod.root(), "time",
                              prod.MakeStringArray(kTimeDims, kBadTimes)));
    CodaMemNode* h = prod.MakeFloatArray(kHeightDims, kHeights);
    ASSERT_TRUE(prod.AddAttribute(h, "FillValue",
                                  prod.MakeStringArray(kOneDim, kBadFill)));
    ASSERT_TRUE(prod.AddField(prod.root(), "height", h));
    ASSERT_TRUE(prod.AddField(prod.root(), "cloud_base_height",
                              prod.MakeFloatArray(kTimeDims, kHeights)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["altitude"].read_status);
  }

  // 3. Invalid @units date string (>=31 chars so str+12 is initialized) and
  // missing units attribute field.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kTimeDims[] = {1};
    constexpr double kTimes[] = {1.0};
    constexpr float kOneFloat[] = {100.0f};
    CodaMemNode* time_node = prod.MakeDoubleArray(kTimeDims, kTimes);
    ASSERT_TRUE(
        prod.AddAttribute(time_node, "units",
                          prod.MakeString("hours since XXXX-XX-XX XX:XX:XX")));
    ASSERT_TRUE(prod.AddField(prod.root(), "time", time_node));
    ASSERT_TRUE(prod.AddField(prod.root(), "cloud_base_height",
                              prod.MakeFloatArray(kTimeDims, kOneFloat)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 4. Non-string @units attribute.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kTimeDims[] = {1};
    constexpr double kTimes[] = {1.0};
    constexpr float kOneFloat[] = {100.0f};
    CodaMemNode* time_node = prod.MakeDoubleArray(kTimeDims, kTimes);
    ASSERT_TRUE(prod.AddAttribute(time_node, "units", prod.MakeInt32(42)));
    ASSERT_TRUE(prod.AddField(prod.root(), "time", time_node));
    ASSERT_TRUE(prod.AddField(prod.root(), "cloud_base_height",
                              prod.MakeFloatArray(kTimeDims, kOneFloat)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 5. Scalar int8 target_classification (1x1 dims) fails
  // coda_cursor_goto_first_array_element.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kOneDim[] = {1};
    constexpr double kTimes[] = {1.0};
    constexpr float kHeights[] = {100.0f};
    CodaMemNode* time_node = prod.MakeDoubleArray(kOneDim, kTimes);
    ASSERT_TRUE(
        prod.AddAttribute(time_node, "units",
                          prod.MakeString("hours since 2020-05-15 00:00:00")));
    ASSERT_TRUE(prod.AddField(prod.root(), "time", time_node));
    ASSERT_TRUE(prod.AddField(prod.root(), "height",
                              prod.MakeFloatArray(kOneDim, kHeights)));
    ASSERT_TRUE(prod.AddField(prod.root(), "cloud_base_height",
                              prod.MakeFloatArray(kOneDim, kHeights)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "target_classification", prod.MakeInt8(0)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["cloud_type"].read_status);
  }

  // 6. Direct ingestion_init with NULL coda_product fails
  // coda_cursor_set_product; direct datetime read_all with missing time fails.
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

    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {0};
    ASSERT_TRUE(prod.AddField(prod.root(), "cloud_base_height",
                              prod.MakeFloatArray(kDims, {})));
    ASSERT_EQ(0, mod->ingestion_init(mod, prod.product(), opts.get(), &prod_def,
                                     &user_data));
    double dummy = 0.0;
    harp_array data{};
    data.double_data = &dummy;
    EXPECT_EQ(-1, prod_def->variable_definition[0]->read_all(user_data, data));
    EXPECT_EQ(HARP_ERROR_CODA, harp_errno);
    mod->ingestion_done(user_data);
  }
}

}  // namespace
}  // namespace stcorp_harp
