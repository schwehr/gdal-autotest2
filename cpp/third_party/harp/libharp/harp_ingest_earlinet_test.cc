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
// Unit tests for EARLINET ingestion module (libharp/harp-ingest-earlinet.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-earlinet.c

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

void PopulateEarlinetProduct(CodaMemProduct& prod, bool include_backscatter,
                             bool include_extinction, bool with_fill_attrs) {
  constexpr long kTimeDims[] = {2};
  constexpr double kTime[] = {1000.0, 2000.0};
  ASSERT_TRUE(prod.AddField(prod.root(), "time",
                            prod.MakeDoubleArray(kTimeDims, kTime)));

  constexpr long kTimeBoundDims[] = {2, 2};
  constexpr double kTimeBounds[] = {900.0, 1100.0, 1900.0, 2100.0};
  ASSERT_TRUE(prod.AddField(prod.root(), "time_bounds",
                            prod.MakeDoubleArray(kTimeBoundDims, kTimeBounds)));

  constexpr long kOneDim[] = {1};
  constexpr float kLat[] = {40.63f};
  constexpr float kLon[] = {15.72f};
  constexpr float kStationAlt[] = {760.0f};
  constexpr float kZenith[] = {0.0f};
  CodaMemNode* lat_node = prod.MakeFloatArray(kOneDim, kLat);
  if (with_fill_attrs) {
    constexpr float kFill[] = {40.63f};
    ASSERT_TRUE(prod.AddAttribute(lat_node, "FillValue",
                                  prod.MakeFloatArray(kOneDim, kFill)));
  }
  ASSERT_TRUE(prod.AddField(prod.root(), "latitude", lat_node));
  ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                            prod.MakeFloatArray(kOneDim, kLon)));
  ASSERT_TRUE(prod.AddField(prod.root(), "station_altitude",
                            prod.MakeFloatArray(kOneDim, kStationAlt)));
  ASSERT_TRUE(prod.AddField(prod.root(), "zenith_angle",
                            prod.MakeFloatArray(kOneDim, kZenith)));

  constexpr long kWaveDims[] = {2};
  constexpr float kWavelengths[] = {355.0f, 532.0f};
  ASSERT_TRUE(prod.AddField(prod.root(), "wavelength",
                            prod.MakeFloatArray(kWaveDims, kWavelengths)));

  constexpr long kAltDims[] = {3};
  constexpr double kAlts[] = {500.0, -999.0, 1500.0};
  CodaMemNode* alt_node = prod.MakeDoubleArray(kAltDims, kAlts);
  if (with_fill_attrs) {
    constexpr double kAltFill[] = {-999.0};
    ASSERT_TRUE(prod.AddAttribute(alt_node, "FillValue",
                                  prod.MakeDoubleArray(kOneDim, kAltFill)));
  }
  ASSERT_TRUE(prod.AddField(prod.root(), "altitude", alt_node));

  // Source shape is [wavelength(2), time(2), altitude(3)].
  // Transposed to [time(2), wavelength(2), altitude(3)].
  constexpr long kDataDims[] = {2, 2, 3};
  constexpr double kBack[] = {1.0, 2.0, 3.0, 4.0,  5.0,  6.0,
                              7.0, 8.0, 9.0, 10.0, 11.0, 12.0};
  constexpr double kErrBack[] = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6,
                                 0.7, 0.8, 0.9, 1.0, 1.1, 1.2};
  if (include_backscatter) {
    ASSERT_TRUE(prod.AddField(prod.root(), "backscatter",
                              prod.MakeDoubleArray(kDataDims, kBack)));
    ASSERT_TRUE(prod.AddField(prod.root(), "error_backscatter",
                              prod.MakeDoubleArray(kDataDims, kErrBack)));
  }
  if (include_extinction) {
    ASSERT_TRUE(prod.AddField(prod.root(), "extinction",
                              prod.MakeDoubleArray(kDataDims, kBack)));
    ASSERT_TRUE(prod.AddField(prod.root(), "error_extinction",
                              prod.MakeDoubleArray(kDataDims, kErrBack)));
  }
}

TEST(HarpIngestEarlinetTest, IngestFullProductWithFillAndTranspose) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("EARLINET");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateEarlinetProduct(prod, /*include_backscatter=*/true,
                          /*include_extinction=*/true,
                          /*with_fill_attrs=*/true);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(3, res.dimensions[harp_dimension_vertical]);
  EXPECT_EQ(2, res.dimensions[harp_dimension_spectral]);

  EXPECT_THAT(res.variables["datetime_start"].double_values,
              ElementsAre(900.0, 1900.0));
  EXPECT_THAT(res.variables["datetime_stop"].double_values,
              ElementsAre(1100.0, 2100.0));
  EXPECT_TRUE(std::isnan(res.variables["latitude"].float_values[0]));
  EXPECT_THAT(res.variables["longitude"].float_values, ElementsAre(15.72f));
  EXPECT_THAT(res.variables["sensor_altitude"].float_values,
              ElementsAre(760.0f));
  EXPECT_THAT(res.variables["viewing_zenith_angle"].float_values,
              ElementsAre(0.0f));
  EXPECT_THAT(res.variables["wavelength"].float_values,
              ElementsAre(355.0f, 532.0f));

  const auto& alt = res.variables["altitude"].double_values;
  ASSERT_EQ(3u, alt.size());
  EXPECT_DOUBLE_EQ(500.0, alt[0]);
  EXPECT_TRUE(std::isnan(alt[1]));
  EXPECT_DOUBLE_EQ(1500.0, alt[2]);

  // Transposed [t=0, w=0, a=0..2] = 1,2,3; [t=0, w=1, a=0..2] = 7,8,9;
  // [t=1, w=0, a=0..2] = 4,5,6; [t=1, w=1, a=0..2] = 10,11,12.
  EXPECT_THAT(res.variables["backscatter_coefficient"].double_values,
              ElementsAre(1.0, 2.0, 3.0, 7.0, 8.0, 9.0, 4.0, 5.0, 6.0, 10.0,
                          11.0, 12.0));
  EXPECT_THAT(
      res.variables["backscatter_coefficient_uncertainty"].double_values,
      ElementsAre(0.1, 0.2, 0.3, 0.7, 0.8, 0.9, 0.4, 0.5, 0.6, 1.0, 1.1, 1.2));
  EXPECT_THAT(res.variables["extinction_coefficient"].double_values,
              ElementsAre(1.0, 2.0, 3.0, 7.0, 8.0, 9.0, 4.0, 5.0, 6.0, 10.0,
                          11.0, 12.0));
  EXPECT_THAT(
      res.variables["extinction_coefficient_uncertainty"].double_values,
      ElementsAre(0.1, 0.2, 0.3, 0.7, 0.8, 0.9, 0.4, 0.5, 0.6, 1.0, 1.1, 1.2));
}

TEST(HarpIngestEarlinetTest, OptionalBackscatterAndExtinctionOmitted) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("EARLINET");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateEarlinetProduct(prod, /*include_backscatter=*/false,
                          /*include_extinction=*/false,
                          /*with_fill_attrs=*/false);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_FALSE(res.variables["backscatter_coefficient"].included);
  EXPECT_FALSE(res.variables["backscatter_coefficient_uncertainty"].included);
  EXPECT_FALSE(res.variables["extinction_coefficient"].included);
  EXPECT_FALSE(res.variables["extinction_coefficient_uncertainty"].included);
  EXPECT_FLOAT_EQ(40.63f, res.variables["latitude"].float_values[0]);
}

TEST(HarpIngestEarlinetTest, ErrorPathsDimensionsAndVariables) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("EARLINET");
  ASSERT_NE(nullptr, mod);

  // 1. Missing time, missing altitude, missing wavelength.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);

    constexpr long kDims[] = {2};
    constexpr double kVals[] = {1.0, 2.0};
    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kDims, kVals)));
    res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);

    ASSERT_TRUE(prod.AddField(prod.root(), "altitude",
                              prod.MakeDoubleArray(kDims, kVals)));
    res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. Variable read errors: missing fields, wrong element counts, wrong types,
  // and wrong-type @FillValue attributes.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kTimeDims[] = {2};
    constexpr double kTime[] = {1.0, 2.0};
    constexpr long kAltDims[] = {2};
    constexpr double kAlt[] = {10.0, 20.0};
    constexpr long kWaveDims[] = {2};
    constexpr float kWave[] = {355.0f, 532.0f};
    constexpr long kOneDim[] = {1};
    constexpr float kOneFloat[] = {10.0f};
    constexpr std::string_view kBadStr[] = {"bad"};

    ASSERT_TRUE(prod.AddField(prod.root(), "time",
                              prod.MakeDoubleArray(kTimeDims, kTime)));
    CodaMemNode* alt_node = prod.MakeDoubleArray(kAltDims, kAlt);
    ASSERT_TRUE(prod.AddAttribute(alt_node, "FillValue",
                                  prod.MakeStringArray(kOneDim, kBadStr)));
    ASSERT_TRUE(prod.AddField(prod.root(), "altitude", alt_node));
    ASSERT_TRUE(prod.AddField(prod.root(), "wavelength",
                              prod.MakeFloatArray(kWaveDims, kWave)));
    // Wrong-type latitude (string array) and wrong-type FillValue on longitude.
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                              prod.MakeStringArray(kOneDim, kBadStr)));
    CodaMemNode* lon_node = prod.MakeFloatArray(kOneDim, kOneFloat);
    ASSERT_TRUE(prod.AddAttribute(lon_node, "FillValue",
                                  prod.MakeStringArray(kOneDim, kBadStr)));
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude", lon_node));
    // Wrong element count for station_altitude (2 instead of 1).
    ASSERT_TRUE(prod.AddField(prod.root(), "station_altitude",
                              prod.MakeFloatArray(kWaveDims, kWave)));
    // Wrong-type time_bounds (string array of size 4).
    constexpr long kTbDims[] = {2, 2};
    constexpr std::string_view kBadTb[] = {"a", "b", "c", "d"};
    ASSERT_TRUE(prod.AddField(prod.root(), "time_bounds",
                              prod.MakeStringArray(kTbDims, kBadTb)));
    // Present backscatter/extinction with missing error_* and wrong counts.
    ASSERT_TRUE(prod.AddField(prod.root(), "backscatter",
                              prod.MakeDoubleArray(kTimeDims, kTime)));
    ASSERT_TRUE(prod.AddField(prod.root(), "extinction",
                              prod.MakeDoubleArray(kTimeDims, kTime)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_stop"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_altitude"].read_status);
    EXPECT_EQ(-1, res.variables["viewing_zenith_angle"].read_status);
    EXPECT_EQ(-1, res.variables["altitude"].read_status);
    EXPECT_EQ(-1, res.variables["backscatter_coefficient"].read_status);
    EXPECT_EQ(-1,
              res.variables["backscatter_coefficient_uncertainty"].read_status);
    EXPECT_EQ(-1, res.variables["extinction_coefficient"].read_status);
    EXPECT_EQ(-1,
              res.variables["extinction_coefficient_uncertainty"].read_status);
  }

  // 3. Direct ingestion_init with NULL coda_product fails
  // coda_cursor_set_product.
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
}

}  // namespace
}  // namespace stcorp_harp
