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
// Unit tests for ESA CCI L3 aerosol ingestion modules
// (libharp/harp-ingest-cci_l3_aerosol.c).
//
// Note: Upstream ingestion_init_* allocates `ingest_info` with malloc() and
// leaves `values_buffer`, `aod_fieldname`, and `aod_uncertainty_name`
// uninitialized before calling init_dimensions(). If init_dimensions() fails,
// ingestion_done() frees those uninitialized pointers. Tests therefore provide
// valid 1D /latitude and /longitude datasets so init_dimensions() succeeds.
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-cci_l3_aerosol.c

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

void AddLatLon(CodaMemProduct& prod) {
  constexpr long kDim[] = {2};
  constexpr double kLat[] = {-45.0, 45.0};
  constexpr double kLon[] = {-90.0, 90.0};
  ASSERT_TRUE(
      prod.AddField(prod.root(), "latitude", prod.MakeDoubleArray(kDim, kLat)));
  ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                            prod.MakeDoubleArray(kDim, kLon)));
}

TEST(HarpIngestCciL3AerosolTest, AatsrAndAtsr2SuccessAndPartialZenith) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  for (std::string_view mod_name :
       {"ESACCI_AEROSOL_L3_AATSR", "ESACCI_AEROSOL_L3_ATSR2"}) {
    const harp_ingestion_module* mod = FindIngestionModule(mod_name);
    ASSERT_NE(nullptr, mod);

    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    AddLatLon(prod);

    constexpr long kGridDims[] = {2, 2};
    constexpr double kAod550[] = {0.1, -999.0, 0.3, 0.4};
    constexpr double kAod870[] = {0.05, 0.06, 0.07, 0.08};
    constexpr double kAod1600[] = {0.01, 0.02, 0.03, 0.04};
    constexpr double kSdev550[] = {0.01, 0.02, -999.0, 0.04};
    // Intentionally omit AOD670_mean and AOD670_sdev to exercise the
    // CODA_ERROR_INVALID_NAME -> NaN fallback path.
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD550_mean",
                              prod.MakeDoubleArray(kGridDims, kAod550)));
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD870_mean",
                              prod.MakeDoubleArray(kGridDims, kAod870)));
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD1600_mean",
                              prod.MakeDoubleArray(kGridDims, kAod1600)));
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD550_sdev",
                              prod.MakeDoubleArray(kGridDims, kSdev550)));
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD870_sdev",
                              prod.MakeDoubleArray(kGridDims, kAod870)));
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD1600_sdev",
                              prod.MakeDoubleArray(kGridDims, kAod1600)));

    constexpr long kZenDims[] = {2, 2, 2};
    constexpr double kSatZen[] = {10.0, 20.0, -999.0, 40.0,
                                  99.0, 99.0, 99.0,   99.0};
    constexpr double kSunZen[] = {25.0, 35.0, 45.0, -999.0,
                                  88.0, 88.0, 88.0, 88.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "satellite_zenith_mean",
                              prod.MakeDoubleArray(kZenDims, kSatZen)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sun_zenith_mean",
                              prod.MakeDoubleArray(kZenDims, kSunZen)));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(4L, res.dimensions[harp_dimension_spectral]);
    EXPECT_EQ(2L, res.dimensions[harp_dimension_latitude]);
    EXPECT_EQ(2L, res.dimensions[harp_dimension_longitude]);
    EXPECT_EQ(1L, res.dimensions[harp_dimension_vertical]);

    EXPECT_THAT(res.variables["wavelength"].double_values,
                ElementsAre(550.0, 670.0, 870.0, 1600.0));
    const auto& aod = res.variables["aerosol_optical_depth"].double_values;
    ASSERT_EQ(16u, aod.size());
    EXPECT_DOUBLE_EQ(0.1, aod[0]);
    EXPECT_TRUE(std::isnan(aod[1]));
    EXPECT_TRUE(std::isnan(aod[4]));
    EXPECT_DOUBLE_EQ(0.05, aod[8]);
    EXPECT_DOUBLE_EQ(0.04, aod[15]);

    const auto& unc =
        res.variables["aerosol_optical_depth_uncertainty"].double_values;
    ASSERT_EQ(16u, unc.size());
    EXPECT_TRUE(std::isnan(unc[2]));
    EXPECT_TRUE(std::isnan(unc[5]));

    EXPECT_TRUE(res.variables["sensor_zenith_angle"].included);
    EXPECT_TRUE(res.variables["solar_zenith_angle"].included);
    EXPECT_DOUBLE_EQ(10.0,
                     res.variables["sensor_zenith_angle"].double_values[0]);
    EXPECT_TRUE(
        std::isnan(res.variables["sensor_zenith_angle"].double_values[2]));
    EXPECT_DOUBLE_EQ(25.0,
                     res.variables["solar_zenith_angle"].double_values[0]);
    EXPECT_TRUE(
        std::isnan(res.variables["solar_zenith_angle"].double_values[3]));
  }
}

TEST(HarpIngestCciL3AerosolTest, AatsrWithoutZenithFieldsAndReadErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod =
      FindIngestionModule("ESACCI_AEROSOL_L3_AATSR");
  ASSERT_NE(nullptr, mod);

  // Without /sun_zenith_mean, zenith variables are excluded.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    AddLatLon(prod);
    // Use non-double arrays for AOD550_mean and AOD550_sdev so
    // coda_cursor_read_double_array sets coda_errno != CODA_ERROR_INVALID_NAME
    // and triggers the error return in read_aerosol_optical_depth*.
    constexpr long kGridDims[] = {2, 2};
    constexpr std::string_view kBadStrs[] = {"a", "b", "c", "d"};
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD550_mean",
                              prod.MakeStringArray(kGridDims, kBadStrs)));
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD550_sdev",
                              prod.MakeStringArray(kGridDims, kBadStrs)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_FALSE(res.variables["sensor_zenith_angle"].included);
    EXPECT_FALSE(res.variables["solar_zenith_angle"].included);
    EXPECT_EQ(-1, res.variables["aerosol_optical_depth"].read_status);
    EXPECT_EQ(-1,
              res.variables["aerosol_optical_depth_uncertainty"].read_status);
  }

  // With /altitude and /sun_zenith_mean present (coda_errno == CODA_SUCCESS),
  // a wrong-size AOD550_mean triggers read_dataset element count error.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    AddLatLon(prod);
    constexpr long kAltDim[] = {1};
    constexpr double kAlt[] = {10.0};
    constexpr double kZen[] = {20.0, 20.0, 20.0, 20.0};
    constexpr long kGridDims[] = {2, 2};
    constexpr long kBadDims[] = {1};
    constexpr double kBadVals[] = {1.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "altitude",
                              prod.MakeDoubleArray(kAltDim, kAlt)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sun_zenith_mean",
                              prod.MakeDoubleArray(kGridDims, kZen)));
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD550_mean",
                              prod.MakeDoubleArray(kBadDims, kBadVals)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["aerosol_optical_depth"].read_status);
  }

  // Error branches in read_partial_dataset (missing satellite_zenith_mean,
  // too-small sun_zenith_mean, and non-double satellite_zenith_mean).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    AddLatLon(prod);
    constexpr long kSmallDims[] = {1};
    constexpr double kSmallVals[] = {1.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "sun_zenith_mean",
                              prod.MakeDoubleArray(kSmallDims, kSmallVals)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_TRUE(res.variables["sensor_zenith_angle"].included);
    EXPECT_EQ(-1, res.variables["sensor_zenith_angle"].read_status);
    EXPECT_EQ(-1, res.variables["solar_zenith_angle"].read_status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    AddLatLon(prod);
    constexpr long kZenDims[] = {2, 2, 2};
    constexpr std::string_view kBadZen[] = {"a", "b", "c", "d",
                                            "e", "f", "g", "h"};
    ASSERT_TRUE(prod.AddField(prod.root(), "sun_zenith_mean",
                              prod.MakeStringArray(kZenDims, kBadZen)));
    ASSERT_TRUE(prod.AddField(prod.root(), "satellite_zenith_mean",
                              prod.MakeStringArray(kZenDims, kBadZen)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["sensor_zenith_angle"].read_status);
    EXPECT_EQ(-1, res.variables["solar_zenith_angle"].read_status);
  }
}

TEST(HarpIngestCciL3AerosolTest, GomosSuccessAndErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod =
      FindIngestionModule("ESACCI_AEROSOL_L3_GOMOS_AERGOM");
  ASSERT_NE(nullptr, mod);

  // 1. Success with 2 altitudes, verifying [alt][lat][lon] ->
  // [wl][lat][lon][alt].
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    AddLatLon(prod);
    constexpr long kAltDim[] = {2};
    constexpr double kAlt[] = {15.0, 20.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "altitude",
                              prod.MakeDoubleArray(kAltDim, kAlt)));

    constexpr long kGridDims[] = {2, 2};
    constexpr double kSaod[] = {0.01, 0.02, 0.03, 0.04};
    constexpr double kSaodUnc[] = {0.001, 0.002, 0.003, 0.004};
    ASSERT_TRUE(prod.AddField(prod.root(), "S_AOD550",
                              prod.MakeDoubleArray(kGridDims, kSaod)));
    ASSERT_TRUE(prod.AddField(prod.root(), "S_AOD550_uncertainty",
                              prod.MakeDoubleArray(kGridDims, kSaodUnc)));

    // [altitude=2][lat=2][lon=2]
    constexpr long kAexDims[] = {2, 2, 2};
    constexpr double kAex[] = {1.0, 2.0, 3.0, 4.0, 10.0, 20.0, 30.0, 40.0};
    constexpr double kAexUnc[] = {0.1, 0.2, 0.3, 0.4, 1.1, 1.2, 1.3, 1.4};
    ASSERT_TRUE(prod.AddField(prod.root(), "AEX550",
                              prod.MakeDoubleArray(kAexDims, kAex)));
    ASSERT_TRUE(prod.AddField(prod.root(), "AEX550_uncertainty",
                              prod.MakeDoubleArray(kAexDims, kAexUnc)));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(1L, res.dimensions[harp_dimension_spectral]);
    EXPECT_EQ(2L, res.dimensions[harp_dimension_vertical]);
    EXPECT_THAT(res.variables["altitude"].double_values,
                ElementsAre(15.0, 20.0));
    EXPECT_THAT(
        res.variables["stratospheric_aerosol_optical_depth"].double_values,
        ElementsAre(0.01, 0.02, 0.03, 0.04));
    EXPECT_THAT(res.variables["stratospheric_aerosol_optical_depth_uncertainty"]
                    .double_values,
                ElementsAre(0.001, 0.002, 0.003, 0.004));
    EXPECT_THAT(res.variables["stratospheric_aerosol_extinction_coefficient"]
                    .double_values,
                ElementsAre(1.0, 10.0, 2.0, 20.0, 3.0, 30.0, 4.0, 40.0));
    EXPECT_THAT(res.variables
                    ["stratospheric_aerosol_extinction_coefficient_uncertainty"]
                        .double_values,
                ElementsAre(0.1, 1.1, 0.2, 1.2, 0.3, 1.3, 0.4, 1.4));
  }

  // 2. Missing AEX550 and AEX550_uncertainty error paths.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    AddLatLon(prod);
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["altitude"].read_status);
    EXPECT_EQ(-1, res.variables["stratospheric_aerosol_extinction_coefficient"]
                      .read_status);
    EXPECT_EQ(-1,
              res.variables
                  ["stratospheric_aerosol_extinction_coefficient_uncertainty"]
                      .read_status);
  }
}

TEST(HarpIngestCciL3AerosolTest, MerisIasiAndMultiSensorSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  // 1. MERIS ALAMO
  {
    const harp_ingestion_module* mod =
        FindIngestionModule("ESACCI_AEROSOL_L3_MERIS_ALAMO");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    AddLatLon(prod);
    constexpr long kGridDims[] = {2, 2};
    constexpr double k550[] = {0.2, 0.3, 0.4, 0.5};
    constexpr double k865[] = {0.1, 0.2, 0.3, 0.4};
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD550",
                              prod.MakeDoubleArray(kGridDims, k550)));
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD865",
                              prod.MakeDoubleArray(kGridDims, k865)));
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD550_std",
                              prod.MakeDoubleArray(kGridDims, k550)));
    ASSERT_TRUE(prod.AddField(prod.root(), "AOD865_std",
                              prod.MakeDoubleArray(kGridDims, k865)));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["wavelength"].double_values,
                ElementsAre(550.0, 865.0));
    EXPECT_THAT(res.variables["aerosol_optical_depth"].double_values,
                ElementsAre(0.2, 0.3, 0.4, 0.5, 0.1, 0.2, 0.3, 0.4));
  }

  // 2. IASI (D_AOD550 branch and Daod550 branch).
  {
    const harp_ingestion_module* mod =
        FindIngestionModule("ESACCI_AEROSOL_L3_IASI");
    ASSERT_NE(nullptr, mod);

    // 2a. DLR naming (/D_AOD550 present) and uncertainty missing -> NaN.
    CodaMemProduct prod_dlr;
    ASSERT_TRUE(prod_dlr.ok());
    AddLatLon(prod_dlr);
    constexpr long kGridDims[] = {2, 2};
    constexpr double kVals[] = {0.1, 0.2, 0.3, 0.4};
    ASSERT_TRUE(prod_dlr.AddField(prod_dlr.root(), "D_AOD550",
                                  prod_dlr.MakeDoubleArray(kGridDims, kVals)));
    ASSERT_TRUE(prod_dlr.AddField(prod_dlr.root(), "D_AOD10000",
                                  prod_dlr.MakeDoubleArray(kGridDims, kVals)));
    ASSERT_TRUE(prod_dlr.AddField(prod_dlr.root(), "D_AOD11000",
                                  prod_dlr.MakeDoubleArray(kGridDims, kVals)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod_dlr.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["wavelength"].double_values,
                ElementsAre(550.0, 10000.0, 11000.0));
    EXPECT_DOUBLE_EQ(
        0.1, res.variables["dust_aerosol_optical_depth"].double_values[0]);
    EXPECT_TRUE(
        std::isnan(res.variables["dust_aerosol_optical_depth_uncertainty"]
                       .double_values[0]));

    // 2b. LMD naming (/D_AOD550 absent -> Daod%ld) with uncertainty present.
    CodaMemProduct prod_lmd;
    ASSERT_TRUE(prod_lmd.ok());
    AddLatLon(prod_lmd);
    ASSERT_TRUE(prod_lmd.AddField(prod_lmd.root(), "Daod550",
                                  prod_lmd.MakeDoubleArray(kGridDims, kVals)));
    ASSERT_TRUE(prod_lmd.AddField(prod_lmd.root(), "Daod10000",
                                  prod_lmd.MakeDoubleArray(kGridDims, kVals)));
    ASSERT_TRUE(prod_lmd.AddField(prod_lmd.root(), "Daod11000",
                                  prod_lmd.MakeDoubleArray(kGridDims, kVals)));
    ASSERT_TRUE(prod_lmd.AddField(prod_lmd.root(), "uncertainty",
                                  prod_lmd.MakeDoubleArray(kGridDims, kVals)));
    IngestExerciseResult res_lmd =
        ExerciseIngestionModule(mod, prod_lmd.product());
    ASSERT_EQ(HARP_SUCCESS, res_lmd.status);
    EXPECT_DOUBLE_EQ(
        0.4, res_lmd.variables["dust_aerosol_optical_depth"].double_values[11]);
    EXPECT_DOUBLE_EQ(0.1,
                     res_lmd.variables["dust_aerosol_optical_depth_uncertainty"]
                         .double_values[0]);
  }

  // 3. Multi Sensor AAI
  {
    const harp_ingestion_module* mod =
        FindIngestionModule("ESACCI_AEROSOL_L3_Multi_Sensor_AAI");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    AddLatLon(prod);
    constexpr long kGridDims[] = {2, 2};
    constexpr double kAai[] = {1.1, 1.2, 1.3, 1.4};
    constexpr double kSza[] = {30.0, 31.0, 32.0, 33.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "absorbing_aerosol_index",
                              prod.MakeDoubleArray(kGridDims, kAai)));
    ASSERT_TRUE(prod.AddField(prod.root(), "solar_zenith_angle",
                              prod.MakeDoubleArray(kGridDims, kSza)));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["absorbing_aerosol_index"].double_values,
                ElementsAre(1.1, 1.2, 1.3, 1.4));
    EXPECT_THAT(res.variables["solar_zenith_angle"].double_values,
                ElementsAre(30.0, 31.0, 32.0, 33.0));
  }
}

}  // namespace
}  // namespace stcorp_harp
