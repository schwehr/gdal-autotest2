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
// Unit tests for GOSAT FTS L2 ingestion modules
// (libharp/harp-ingest-gosat_fts_l2.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-gosat_fts_l2.c

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/coda_mem_product.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/ingest_test_util.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::DoubleNear;
using ::testing::ElementsAre;

void PopulateGosatL2Common(CodaMemProduct& prod, CodaMemNode** out_data_rec) {
  CodaMemNode* scan_attr = prod.MakeRecord();
  ASSERT_NE(nullptr, scan_attr);
  const long t_dims[] = {2};
  constexpr std::string_view kTimes[] = {"2015-05-15 12:30:45.123Z",
                                         "2015-05-15 12:30:46.500Z"};
  ASSERT_TRUE(
      prod.AddField(scan_attr, "time", prod.MakeStringArray(t_dims, kTimes)));
  ASSERT_TRUE(prod.AddField(prod.root(), "scanAttribute", scan_attr));

  CodaMemNode* data_rec = prod.MakeRecord();
  ASSERT_NE(nullptr, data_rec);
  CodaMemNode* geo = prod.MakeRecord();
  ASSERT_NE(nullptr, geo);

  const float lat[] = {35.5f, -9999.0f};
  const float lon[] = {135.5f, -9999.0f};
  const float saz[] = {45.0f, -9999.0f};
  const float sza[] = {30.0f, -9999.0f};
  const float sat_az[] = {200.0f, 10.0f};
  const float sat_za[] = {25.0f, 15.0f};
  ASSERT_TRUE(prod.AddField(geo, "latitude", prod.MakeFloatArray(t_dims, lat)));
  ASSERT_TRUE(
      prod.AddField(geo, "longitude", prod.MakeFloatArray(t_dims, lon)));
  ASSERT_TRUE(
      prod.AddField(geo, "solarAzimuth", prod.MakeFloatArray(t_dims, saz)));
  ASSERT_TRUE(
      prod.AddField(geo, "solarZenith", prod.MakeFloatArray(t_dims, sza)));
  ASSERT_TRUE(prod.AddField(geo, "satelliteAzimuth",
                            prod.MakeFloatArray(t_dims, sat_az)));
  ASSERT_TRUE(prod.AddField(geo, "satelliteZenith",
                            prod.MakeFloatArray(t_dims, sat_za)));

  // 2 footprints x 36 points.
  // Footprint 0: fplon[0] = -170 (< -90, < 0) to trigger dateline +360 shift
  // and >= 180 wrap, with varying lat/lon to hit min/max updates.
  // Footprint 1: fplon[36] = 185 (> 90, >= 180) to trigger min_lon/max_lon
  // wrap without < 0 shift.
  std::vector<double> fp_lat(72, 10.0);
  std::vector<double> fp_lon(72, 185.0);
  fp_lat[0] = 35.0;
  fp_lat[1] = 34.0;
  fp_lat[2] = 36.0;
  fp_lon[0] = -170.0;
  fp_lon[1] = 175.0;
  fp_lon[2] = 195.0;
  fp_lat[36] = -10.0;
  fp_lat[37] = -12.0;
  fp_lat[38] = -8.0;
  fp_lon[36] = 185.0;
  fp_lon[37] = 182.0;
  fp_lon[38] = 188.0;

  const long fp_dims[] = {2, 36};
  ASSERT_TRUE(prod.AddField(geo, "footPrintLatitude",
                            prod.MakeDoubleArray(fp_dims, fp_lat)));
  ASSERT_TRUE(prod.AddField(geo, "footPrintLongitude",
                            prod.MakeDoubleArray(fp_dims, fp_lon)));
  ASSERT_TRUE(prod.AddField(data_rec, "geolocation", geo));
  ASSERT_TRUE(prod.AddField(prod.root(), "Data", data_rec));
  *out_data_rec = data_rec;
}

void PopulateCo2Fields(CodaMemProduct& prod, CodaMemNode* data_rec) {
  const long dims[] = {2};
  CodaMemNode* mr = prod.MakeRecord();
  const float xco2[] = {400.0f, -1.0E30f};
  const float e1[] = {1.0f, -1.0E30f};
  const float e2[] = {2.0f, -1.0E30f};
  const float e3[] = {3.0f, -1.0E30f};
  const float e4[] = {4.0f, -1.0E30f};
  ASSERT_TRUE(prod.AddField(mr, "XCO2", prod.MakeFloatArray(dims, xco2)));
  ASSERT_TRUE(
      prod.AddField(mr, "XCO2ExternalError", prod.MakeFloatArray(dims, e1)));
  ASSERT_TRUE(prod.AddField(mr, "XCO2InterferenceError",
                            prod.MakeFloatArray(dims, e2)));
  ASSERT_TRUE(
      prod.AddField(mr, "XCO2RetrievalNoise", prod.MakeFloatArray(dims, e3)));
  ASSERT_TRUE(
      prod.AddField(mr, "XCO2SmoothingError", prod.MakeFloatArray(dims, e4)));
  ASSERT_TRUE(prod.AddField(data_rec, "mixingRatio", mr));

  CodaMemNode* tc = prod.MakeRecord();
  const float col[] = {8.5e21f, -1.0E30f};
  const float c1[] = {0.1f, -1.0E30f};
  const float c2[] = {0.2f, -1.0E30f};
  const float c3[] = {0.3f, -1.0E30f};
  const float c4[] = {0.4f, -1.0E30f};
  ASSERT_TRUE(
      prod.AddField(tc, "CO2TotalColumn", prod.MakeFloatArray(dims, col)));
  ASSERT_TRUE(prod.AddField(tc, "CO2TotalColumnExternalError",
                            prod.MakeFloatArray(dims, c1)));
  ASSERT_TRUE(prod.AddField(tc, "CO2TotalColumnInterferenceError",
                            prod.MakeFloatArray(dims, c2)));
  ASSERT_TRUE(prod.AddField(tc, "CO2TotalColumnRetrievalNoise",
                            prod.MakeFloatArray(dims, c3)));
  ASSERT_TRUE(prod.AddField(tc, "CO2TotalColumnSmoothingError",
                            prod.MakeFloatArray(dims, c4)));
  ASSERT_TRUE(prod.AddField(data_rec, "totalColumn", tc));
}

void PopulateCh4Fields(CodaMemProduct& prod, CodaMemNode* data_rec) {
  const long dims[] = {2};
  CodaMemNode* mr = prod.MakeRecord();
  const float xch4[] = {1.8f, -1.0E30f};
  const float e1[] = {0.01f, -1.0E30f};
  const float e2[] = {0.02f, -1.0E30f};
  const float e3[] = {0.03f, -1.0E30f};
  const float e4[] = {0.04f, -1.0E30f};
  ASSERT_TRUE(prod.AddField(mr, "XCH4", prod.MakeFloatArray(dims, xch4)));
  ASSERT_TRUE(
      prod.AddField(mr, "XCH4ExternalError", prod.MakeFloatArray(dims, e1)));
  ASSERT_TRUE(prod.AddField(mr, "XCH4InterferenceError",
                            prod.MakeFloatArray(dims, e2)));
  ASSERT_TRUE(
      prod.AddField(mr, "XCH4RetrievalNoise", prod.MakeFloatArray(dims, e3)));
  ASSERT_TRUE(
      prod.AddField(mr, "XCH4SmoothingError", prod.MakeFloatArray(dims, e4)));
  ASSERT_TRUE(prod.AddField(data_rec, "mixingRatio", mr));

  CodaMemNode* tc = prod.MakeRecord();
  const float col[] = {3.5e19f, -1.0E30f};
  const float c1[] = {1.0f, -1.0E30f};
  const float c2[] = {2.0f, -1.0E30f};
  const float c3[] = {3.0f, -1.0E30f};
  const float c4[] = {4.0f, -1.0E30f};
  ASSERT_TRUE(
      prod.AddField(tc, "CH4TotalColumn", prod.MakeFloatArray(dims, col)));
  ASSERT_TRUE(prod.AddField(tc, "CH4TotalColumnExternalError",
                            prod.MakeFloatArray(dims, c1)));
  ASSERT_TRUE(prod.AddField(tc, "CH4TotalColumnInterferenceError",
                            prod.MakeFloatArray(dims, c2)));
  ASSERT_TRUE(prod.AddField(tc, "CH4TotalColumnRetrievalNoise",
                            prod.MakeFloatArray(dims, c3)));
  ASSERT_TRUE(prod.AddField(tc, "CH4TotalColumnSmoothingError",
                            prod.MakeFloatArray(dims, c4)));
  ASSERT_TRUE(prod.AddField(data_rec, "totalColumn", tc));
}

TEST(HarpIngestGosatFtsL2Test, IngestCo2Success) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GOSAT_FTS_L2_CO2_TC");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  CodaMemNode* data_rec = nullptr;
  PopulateGosatL2Common(prod, &data_rec);
  PopulateCo2Fields(prod, data_rec);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(2, res.dimensions[harp_dimension_time]);

  double expected_t0 = 0.0;
  ASSERT_EQ(0, coda_string_to_time("2015-05-15 12:30:45.123000", &expected_t0));
  EXPECT_NEAR(expected_t0, res.variables["datetime"].double_values[0], 1e-6);

  EXPECT_DOUBLE_EQ(35.5, res.variables["latitude"].double_values[0]);
  EXPECT_TRUE(std::isnan(res.variables["latitude"].double_values[1]));
  EXPECT_DOUBLE_EQ(135.5, res.variables["longitude"].double_values[0]);
  EXPECT_TRUE(std::isnan(res.variables["longitude"].double_values[1]));

  EXPECT_THAT(std::vector<double>(
                  res.variables["latitude_bounds"].double_values.begin(),
                  res.variables["latitude_bounds"].double_values.begin() + 4),
              ElementsAre(10.0, 10.0, 36.0, 36.0));
  EXPECT_THAT(std::vector<double>(
                  res.variables["longitude_bounds"].double_values.begin() + 4,
                  res.variables["longitude_bounds"].double_values.begin() + 8),
              ElementsAre(-178.0, -172.0, -172.0, -178.0));

  EXPECT_DOUBLE_EQ(45.0, res.variables["solar_azimuth_angle"].double_values[0]);
  EXPECT_TRUE(
      std::isnan(res.variables["solar_azimuth_angle"].double_values[1]));
  EXPECT_DOUBLE_EQ(30.0, res.variables["solar_zenith_angle"].double_values[0]);
  EXPECT_TRUE(std::isnan(res.variables["solar_zenith_angle"].double_values[1]));
  // 200 + 180 = 380 -> wrapped to 20.0; 10 + 180 = 190.
  EXPECT_THAT(res.variables["sensor_azimuth_angle"].double_values,
              ElementsAre(20.0, 190.0));
  EXPECT_THAT(res.variables["sensor_zenith_angle"].double_values,
              ElementsAre(155.0, 165.0));

  EXPECT_DOUBLE_EQ(
      400.0,
      res.variables["CO2_column_volume_mixing_ratio_dry_air"].double_values[0]);
  EXPECT_TRUE(std::isnan(res.variables["CO2_column_volume_mixing_ratio_dry_air"]
                             .double_values[1]));
  EXPECT_THAT(
      res.variables["CO2_column_volume_mixing_ratio_dry_air_uncertainty"]
          .double_values,
      ElementsAre(DoubleNear(10.0, 1e-6), 0.0));
  EXPECT_NEAR(8.5e21,
              res.variables["CO2_column_number_density"].double_values[0],
              1e15);
  EXPECT_TRUE(
      std::isnan(res.variables["CO2_column_number_density"].double_values[1]));
  EXPECT_THAT(
      res.variables["CO2_column_number_density_uncertainty"].double_values,
      ElementsAre(DoubleNear(1.0, 1e-5), 0.0));
}

TEST(HarpIngestGosatFtsL2Test, IngestCh4Success) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GOSAT_FTS_L2_CH4_TC");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  CodaMemNode* data_rec = nullptr;
  PopulateGosatL2Common(prod, &data_rec);
  PopulateCh4Fields(prod, data_rec);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(2, res.dimensions[harp_dimension_time]);

  EXPECT_NEAR(
      1.8,
      res.variables["CH4_column_volume_mixing_ratio_dry_air"].double_values[0],
      1e-5);
  EXPECT_TRUE(std::isnan(res.variables["CH4_column_volume_mixing_ratio_dry_air"]
                             .double_values[1]));
  EXPECT_THAT(
      res.variables["CH4_column_volume_mixing_ratio_dry_air_uncertainty"]
          .double_values,
      ElementsAre(DoubleNear(0.1, 1e-6), 0.0));
  EXPECT_NEAR(3.5e19,
              res.variables["CH4_column_number_density"].double_values[0],
              1e13);
  EXPECT_TRUE(
      std::isnan(res.variables["CH4_column_number_density"].double_values[1]));
  EXPECT_THAT(
      res.variables["CH4_column_number_density_uncertainty"].double_values,
      ElementsAre(DoubleNear(10.0, 1e-6), 0.0));
}

TEST(HarpIngestGosatFtsL2Test, ReadCornerLatitudeDirectlyWhenUninit) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GOSAT_FTS_L2_CO2_TC");
  ASSERT_NE(nullptr, mod);

  // Exercise read_corner_latitude directly before read_corner_longitude runs,
  // both on a valid product and on a missing-footprint product.
  for (bool valid : {true, false}) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* scan_attr = prod.MakeRecord();
    const long t_dims[] = {1};
    constexpr std::string_view kTimes[] = {"2015-05-15 12:30:45.123Z"};
    ASSERT_TRUE(
        prod.AddField(scan_attr, "time", prod.MakeStringArray(t_dims, kTimes)));
    ASSERT_TRUE(prod.AddField(prod.root(), "scanAttribute", scan_attr));
    if (valid) {
      CodaMemNode* data_rec = prod.MakeRecord();
      CodaMemNode* geo = prod.MakeRecord();
      std::vector<double> fp_lat(36, 12.0);
      std::vector<double> fp_lon(36, 20.0);
      const long fp_dims[] = {1, 36};
      ASSERT_TRUE(prod.AddField(geo, "footPrintLatitude",
                                prod.MakeDoubleArray(fp_dims, fp_lat)));
      ASSERT_TRUE(prod.AddField(geo, "footPrintLongitude",
                                prod.MakeDoubleArray(fp_dims, fp_lon)));
      ASSERT_TRUE(prod.AddField(data_rec, "geolocation", geo));
      ASSERT_TRUE(prod.AddField(prod.root(), "Data", data_rec));
    }

    ScopedHarpIngestionOptions opts;
    harp_ingestion_options* raw_opts = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_ingestion_options_new(&raw_opts));
    opts.reset(raw_opts);

    harp_product_definition* prod_def = nullptr;
    void* user_data = nullptr;
    ASSERT_EQ(0, mod->ingestion_init(mod, prod.product(), opts.get(), &prod_def,
                                     &user_data));
    auto done_guard =
        absl::MakeCleanup([&] { mod->ingestion_done(user_data); });

    for (int i = 0; i < prod_def->num_variable_definitions; ++i) {
      const harp_variable_definition* vd = prod_def->variable_definition[i];
      if (std::string_view(vd->name) == "latitude_bounds") {
        double buf[4] = {};
        harp_array arr{};
        arr.double_data = buf;
        int rc = vd->read_block(user_data, 0, arr);
        if (valid) {
          EXPECT_EQ(0, rc);
          EXPECT_THAT(buf, ElementsAre(12.0, 12.0, 12.0, 12.0));
        } else {
          EXPECT_EQ(-1, rc);
        }
      }
    }
  }
}

TEST(HarpIngestGosatFtsL2Test, ErrorPathsInitAndTime) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GOSAT_FTS_L2_CO2_TC");
  ASSERT_NE(nullptr, mod);

  // 1. Missing scanAttribute/time.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. Non-array scanAttribute/time.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* scan_attr = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(scan_attr, "time", prod.MakeInt32(1)));
    ASSERT_TRUE(prod.AddField(prod.root(), "scanAttribute", scan_attr));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. Invalid string in scanAttribute/time & wrong type in time.
  for (bool wrong_type : {false, true}) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* scan_attr = prod.MakeRecord();
    const long dims[] = {1};
    if (wrong_type) {
      const int32_t bad[] = {1};
      ASSERT_TRUE(
          prod.AddField(scan_attr, "time", prod.MakeInt32Array(dims, bad)));
    } else {
      constexpr std::string_view kBad[] = {"not-a-valid-date-string-at-all"};
      ASSERT_TRUE(
          prod.AddField(scan_attr, "time", prod.MakeStringArray(dims, kBad)));
    }
    ASSERT_TRUE(prod.AddField(prod.root(), "scanAttribute", scan_attr));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
  }
}

TEST(HarpIngestGosatFtsL2Test, ErrorPathsFootprintAndDatasets) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* co2_mod =
      FindIngestionModule("GOSAT_FTS_L2_CO2_TC");
  const harp_ingestion_module* ch4_mod =
      FindIngestionModule("GOSAT_FTS_L2_CH4_TC");
  ASSERT_NE(nullptr, co2_mod);
  ASSERT_NE(nullptr, ch4_mod);

  auto make_with_time = [](CodaMemProduct& prod) {
    CodaMemNode* scan_attr = prod.MakeRecord();
    const long t_dims[] = {1};
    constexpr std::string_view kTimes[] = {"2015-05-15 12:30:45.123Z"};
    ASSERT_TRUE(
        prod.AddField(scan_attr, "time", prod.MakeStringArray(t_dims, kTimes)));
    ASSERT_TRUE(prod.AddField(prod.root(), "scanAttribute", scan_attr));
  };

  // 1. footPrintLatitude not an array.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_with_time(prod);
    CodaMemNode* data_rec = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(geo, "footPrintLatitude", prod.MakeDouble(1.0)));
    ASSERT_TRUE(prod.AddField(data_rec, "geolocation", geo));
    ASSERT_TRUE(prod.AddField(prod.root(), "Data", data_rec));
    IngestExerciseResult res = ExerciseIngestionModule(
        co2_mod, prod.product(), nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["longitude_bounds"].read_status);
  }

  // 2. footPrintLatitude element count not divisible by 36.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_with_time(prod);
    CodaMemNode* data_rec = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    const long bad_dims[] = {5};
    const double vals[5] = {1, 2, 3, 4, 5};
    ASSERT_TRUE(prod.AddField(geo, "footPrintLatitude",
                              prod.MakeDoubleArray(bad_dims, vals)));
    ASSERT_TRUE(prod.AddField(data_rec, "geolocation", geo));
    ASSERT_TRUE(prod.AddField(prod.root(), "Data", data_rec));
    IngestExerciseResult res = ExerciseIngestionModule(
        co2_mod, prod.product(), nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["longitude_bounds"].read_status);
  }

  // 3. footPrintLatitude wrong element type (string array of 36).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_with_time(prod);
    CodaMemNode* data_rec = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    const long dims36[] = {36};
    std::vector<std::string_view> strs(36, "bad");
    ASSERT_TRUE(prod.AddField(geo, "footPrintLatitude",
                              prod.MakeStringArray(dims36, strs)));
    ASSERT_TRUE(prod.AddField(data_rec, "geolocation", geo));
    ASSERT_TRUE(prod.AddField(prod.root(), "Data", data_rec));
    IngestExerciseResult res = ExerciseIngestionModule(
        co2_mod, prod.product(), nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["longitude_bounds"].read_status);
  }

  // 4. Missing footPrintLongitude, then wrong type footPrintLongitude.
  for (bool wrong_lon_type : {false, true}) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_with_time(prod);
    CodaMemNode* data_rec = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    const long dims36[] = {36};
    std::vector<double> vals(36, 1.0);
    ASSERT_TRUE(prod.AddField(geo, "footPrintLatitude",
                              prod.MakeDoubleArray(dims36, vals)));
    if (wrong_lon_type) {
      std::vector<std::string_view> strs(36, "bad");
      ASSERT_TRUE(prod.AddField(geo, "footPrintLongitude",
                                prod.MakeStringArray(dims36, strs)));
    }
    ASSERT_TRUE(prod.AddField(data_rec, "geolocation", geo));
    ASSERT_TRUE(prod.AddField(prod.root(), "Data", data_rec));
    IngestExerciseResult res = ExerciseIngestionModule(
        co2_mod, prod.product(), nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["longitude_bounds"].read_status);
  }

  // 5. Missing datasets and wrong element types / empty arrays for CO2 & CH4
  // uncertainty components and sensor angles.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_with_time(prod);
    CodaMemNode* data_rec = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    const long empty_dims[] = {0};
    ASSERT_TRUE(prod.AddField(
        geo, "latitude",
        prod.MakeFloatArray(empty_dims, std::span<const float>{})));
    const long str_dims[] = {1};
    constexpr std::string_view kBadStr[] = {"bad"};
    ASSERT_TRUE(prod.AddField(geo, "longitude",
                              prod.MakeStringArray(str_dims, kBadStr)));
    ASSERT_TRUE(prod.AddField(data_rec, "geolocation", geo));
    ASSERT_TRUE(prod.AddField(prod.root(), "Data", data_rec));

    IngestExerciseResult r1 = ExerciseIngestionModule(
        co2_mod, prod.product(), nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, r1.status);
    EXPECT_EQ(-1, r1.variables["latitude"].read_status);
    EXPECT_EQ(-1, r1.variables["longitude"].read_status);
    EXPECT_EQ(-1, r1.variables["sensor_azimuth_angle"].read_status);
    EXPECT_EQ(-1, r1.variables["sensor_zenith_angle"].read_status);
    EXPECT_EQ(-1,
              r1.variables["CO2_column_volume_mixing_ratio_dry_air_uncertainty"]
                  .read_status);
    EXPECT_EQ(
        -1, r1.variables["CO2_column_number_density_uncertainty"].read_status);

    IngestExerciseResult r2 = ExerciseIngestionModule(
        ch4_mod, prod.product(), nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, r2.status);
    EXPECT_EQ(-1,
              r2.variables["CH4_column_volume_mixing_ratio_dry_air_uncertainty"]
                  .read_status);
    EXPECT_EQ(
        -1, r2.variables["CH4_column_number_density_uncertainty"].read_status);
  }

  // 6. Partial error component presence to hit each component failure branch.
  const char* co2_mr_errs[] = {"XCO2ExternalError", "XCO2InterferenceError",
                               "XCO2RetrievalNoise", "XCO2SmoothingError"};
  const char* co2_tc_errs[] = {
      "CO2TotalColumnExternalError", "CO2TotalColumnInterferenceError",
      "CO2TotalColumnRetrievalNoise", "CO2TotalColumnSmoothingError"};
  const char* ch4_mr_errs[] = {"XCH4ExternalError", "XCH4InterferenceError",
                               "XCH4RetrievalNoise", "XCH4SmoothingError"};
  const char* ch4_tc_errs[] = {
      "CH4TotalColumnExternalError", "CH4TotalColumnInterferenceError",
      "CH4TotalColumnRetrievalNoise", "CH4TotalColumnSmoothingError"};
  for (int skip = 1; skip < 4; ++skip) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_with_time(prod);
    CodaMemNode* data_rec = prod.MakeRecord();
    CodaMemNode* mr = prod.MakeRecord();
    CodaMemNode* tc = prod.MakeRecord();
    const long dims[] = {1};
    const float val[] = {1.0f};
    for (int i = 0; i < skip; ++i) {
      ASSERT_TRUE(
          prod.AddField(mr, co2_mr_errs[i], prod.MakeFloatArray(dims, val)));
      ASSERT_TRUE(
          prod.AddField(tc, co2_tc_errs[i], prod.MakeFloatArray(dims, val)));
      ASSERT_TRUE(
          prod.AddField(mr, ch4_mr_errs[i], prod.MakeFloatArray(dims, val)));
      ASSERT_TRUE(
          prod.AddField(tc, ch4_tc_errs[i], prod.MakeFloatArray(dims, val)));
    }
    ASSERT_TRUE(prod.AddField(data_rec, "mixingRatio", mr));
    ASSERT_TRUE(prod.AddField(data_rec, "totalColumn", tc));
    ASSERT_TRUE(prod.AddField(prod.root(), "Data", data_rec));

    IngestExerciseResult r_co2 = ExerciseIngestionModule(
        co2_mod, prod.product(), nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(
        -1,
        r_co2.variables["CO2_column_volume_mixing_ratio_dry_air_uncertainty"]
            .read_status);
    EXPECT_EQ(
        -1,
        r_co2.variables["CO2_column_number_density_uncertainty"].read_status);

    IngestExerciseResult r_ch4 = ExerciseIngestionModule(
        ch4_mod, prod.product(), nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(
        -1,
        r_ch4.variables["CH4_column_volume_mixing_ratio_dry_air_uncertainty"]
            .read_status);
    EXPECT_EQ(
        -1,
        r_ch4.variables["CH4_column_number_density_uncertainty"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
