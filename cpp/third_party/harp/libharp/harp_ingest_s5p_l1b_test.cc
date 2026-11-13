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
// Unit tests for Sentinel-5P L1b ingestion modules
// (libharp/harp-ingest-s5p_l1b.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-s5p_l1b.c

#include <cmath>
#include <cstdint>
#include <span>
#include <string>
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
using ::testing::FloatNear;

constexpr long kNumScanlines = 2;
constexpr long kNumPixels = 2;
constexpr long kNumChannels = 2;
constexpr float kFillFloat = -9999.0f;
constexpr double kFillDouble = -9999.0;

CodaMemNode* MakeFloatDataset(CodaMemProduct& prod, std::span<const long> dims,
                              std::span<const float> vals) {
  CodaMemNode* ds = prod.MakeFloatArray(dims, vals);
  constexpr long kFillDims[] = {1};
  constexpr float kFill[] = {kFillFloat};
  if (!prod.AddAttribute(ds, "FillValue",
                         prod.MakeFloatArray(kFillDims, kFill))) {
    return nullptr;
  }
  return ds;
}

CodaMemNode* MakeDoubleDataset(CodaMemProduct& prod, std::span<const long> dims,
                               std::span<const double> vals) {
  CodaMemNode* ds = prod.MakeDoubleArray(dims, vals);
  constexpr long kFillDims[] = {1};
  constexpr double kFill[] = {kFillDouble};
  if (!prod.AddAttribute(ds, "FillValue",
                         prod.MakeDoubleArray(kFillDims, kFill))) {
    return nullptr;
  }
  return ds;
}

void PopulateS5pGroup(CodaMemProduct& prod, std::string_view group_name,
                      bool is_irradiance, bool array_orbit,
                      bool fill_time_ref) {
  if (array_orbit) {
    constexpr long kOrbitDims[] = {1};
    constexpr int32_t kOrbit[] = {12345};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "orbit",
                                  prod.MakeInt32Array(kOrbitDims, kOrbit)));
  } else {
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "orbit", prod.MakeInt32(12345)));
  }

  CodaMemNode* group = prod.MakeRecord();
  CodaMemNode* mode = prod.MakeRecord();
  CodaMemNode* geodata = prod.MakeRecord();
  CodaMemNode* observations = prod.MakeRecord();
  CodaMemNode* instrument = prod.MakeRecord();

  constexpr long kObsDims[] = {1, kNumScanlines, kNumPixels, kNumChannels};
  constexpr long kWaveDims[] = {kNumPixels, kNumChannels};
  constexpr long kScanDims[] = {kNumScanlines};
  constexpr long kPixDims[] = {kNumScanlines, kNumPixels};
  constexpr long kBoundsDims[] = {kNumScanlines, kNumPixels, 4};
  constexpr long kOneDim[] = {1};

  // OBSERVATIONS
  const double time_ref = fill_time_ref ? -2147483647.0 : 1000.0;
  const double time_arr[] = {time_ref};
  constexpr double kDeltaTime[] = {500.0, kFillDouble};
  ASSERT_TRUE(prod.AddField(observations, "time",
                            prod.MakeDoubleArray(kOneDim, time_arr)));
  ASSERT_TRUE(prod.AddField(observations, "delta_time",
                            MakeDoubleDataset(prod, kScanDims, kDeltaTime)));

  constexpr float kObs[] = {10.0f, 20.0f, 30.0f,      40.0f,
                            50.0f, 60.0f, kFillFloat, 80.0f};
  // Negative error/noise values yield NaN; non-negative yield
  // |10^(val*0.1)*obs|.
  constexpr float kErr[] = {0.0f, -1.0f, 10.0f, 0.0f, 0.0f, -2.0f, 0.0f, 10.0f};
  constexpr float kNoise[] = {10.0f, 0.0f,  -1.0f, 0.0f,
                              0.0f,  10.0f, 0.0f,  -1.0f};
  constexpr float kWave[] = {300.0f, 301.0f, 302.0f, kFillFloat};

  if (is_irradiance) {
    ASSERT_TRUE(prod.AddField(observations, "irradiance",
                              MakeFloatDataset(prod, kObsDims, kObs)));
    ASSERT_TRUE(prod.AddField(observations, "irradiance_error",
                              MakeFloatDataset(prod, kObsDims, kErr)));
    ASSERT_TRUE(prod.AddField(observations, "irradiance_noise",
                              MakeFloatDataset(prod, kObsDims, kNoise)));
    ASSERT_TRUE(prod.AddField(instrument, "calibrated_wavelength",
                              MakeFloatDataset(prod, kWaveDims, kWave)));
  } else {
    ASSERT_TRUE(prod.AddField(observations, "radiance",
                              MakeFloatDataset(prod, kObsDims, kObs)));
    ASSERT_TRUE(prod.AddField(observations, "radiance_error",
                              MakeFloatDataset(prod, kObsDims, kErr)));
    ASSERT_TRUE(prod.AddField(observations, "radiance_noise",
                              MakeFloatDataset(prod, kObsDims, kNoise)));
    ASSERT_TRUE(prod.AddField(instrument, "nominal_wavelength",
                              MakeFloatDataset(prod, kWaveDims, kWave)));

    // GEODATA (required for radiance products)
    constexpr float kLat[] = {1.0f, 2.0f, 3.0f, 4.0f};
    constexpr float kLon[] = {10.0f, 20.0f, 30.0f, 40.0f};
    constexpr float kLatB[] = {1.0f, 1.1f, 1.2f, 1.3f, 2.0f, 2.1f, 2.2f, 2.3f,
                               3.0f, 3.1f, 3.2f, 3.3f, 4.0f, 4.1f, 4.2f, 4.3f};
    constexpr float kLonB[] = {10.0f, 10.1f, 10.2f, 10.3f, 20.0f, 20.1f,
                               20.2f, 20.3f, 30.0f, 30.1f, 30.2f, 30.3f,
                               40.0f, 40.1f, 40.2f, 40.3f};
    constexpr float kSatLat[] = {5.0f, 6.0f};
    constexpr float kSatLon[] = {15.0f, 16.0f};
    constexpr float kSatAlt[] = {800000.0f, 800100.0f};
    constexpr float kSza[] = {30.0f, 31.0f, 32.0f, 33.0f};
    constexpr float kSaz[] = {40.0f, 41.0f, 42.0f, 43.0f};
    constexpr float kVza[] = {10.0f, 11.0f, 12.0f, 13.0f};
    constexpr float kVaz[] = {20.0f, 21.0f, 22.0f, 23.0f};

    ASSERT_TRUE(prod.AddField(geodata, "latitude",
                              MakeFloatDataset(prod, kPixDims, kLat)));
    ASSERT_TRUE(prod.AddField(geodata, "longitude",
                              MakeFloatDataset(prod, kPixDims, kLon)));
    ASSERT_TRUE(prod.AddField(geodata, "latitude_bounds",
                              MakeFloatDataset(prod, kBoundsDims, kLatB)));
    ASSERT_TRUE(prod.AddField(geodata, "longitude_bounds",
                              MakeFloatDataset(prod, kBoundsDims, kLonB)));
    ASSERT_TRUE(prod.AddField(geodata, "satellite_latitude",
                              MakeFloatDataset(prod, kScanDims, kSatLat)));
    ASSERT_TRUE(prod.AddField(geodata, "satellite_longitude",
                              MakeFloatDataset(prod, kScanDims, kSatLon)));
    ASSERT_TRUE(prod.AddField(geodata, "satellite_altitude",
                              MakeFloatDataset(prod, kScanDims, kSatAlt)));
    ASSERT_TRUE(prod.AddField(geodata, "solar_zenith_angle",
                              MakeFloatDataset(prod, kPixDims, kSza)));
    ASSERT_TRUE(prod.AddField(geodata, "solar_azimuth_angle",
                              MakeFloatDataset(prod, kPixDims, kSaz)));
    ASSERT_TRUE(prod.AddField(geodata, "viewing_zenith_angle",
                              MakeFloatDataset(prod, kPixDims, kVza)));
    ASSERT_TRUE(prod.AddField(geodata, "viewing_azimuth_angle",
                              MakeFloatDataset(prod, kPixDims, kVaz)));
  }

  ASSERT_TRUE(prod.AddField(mode, "GEODATA", geodata));
  ASSERT_TRUE(prod.AddField(mode, "OBSERVATIONS", observations));
  ASSERT_TRUE(prod.AddField(mode, "INSTRUMENT", instrument));
  ASSERT_TRUE(prod.AddField(group, "STANDARD_MODE", mode));
  ASSERT_TRUE(prod.AddField(prod.root(), group_name, group));
}

TEST(HarpIngestS5pL1bTest, IngestIrradianceUvnAndSirAllBands) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  // 1. S5P_L1B_IR_UVN with default band (1) and explicit bands 2..6.
  const harp_ingestion_module* uvn_mod = FindIngestionModule("S5P_L1B_IR_UVN");
  ASSERT_NE(nullptr, uvn_mod);
  for (int b = 1; b <= 6; ++b) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const std::string group = "BAND" + std::to_string(b) + "_IRRADIANCE";
    PopulateS5pGroup(prod, group, /*is_irradiance=*/true,
                     /*array_orbit=*/(b % 2 == 0),
                     /*fill_time_ref=*/(b == 2));
    const std::string opt = "band=" + std::to_string(b);
    IngestExerciseResult res = ExerciseIngestionModule(
        uvn_mod, prod.product(), b == 1 ? nullptr : opt.c_str());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(kNumScanlines * kNumPixels, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(kNumChannels, res.dimensions[harp_dimension_spectral]);
    EXPECT_THAT(res.variables["orbit_index"].int32_values, ElementsAre(12345));
    EXPECT_THAT(res.variables["scan_subindex"].int16_values,
                ElementsAre(0, 1, 0, 1));
    if (b == 1) {
      const auto& dt = res.variables["datetime"].double_values;
      ASSERT_EQ(4u, dt.size());
      EXPECT_DOUBLE_EQ(1000.5, dt[0]);
      EXPECT_DOUBLE_EQ(1000.5, dt[1]);
      EXPECT_TRUE(std::isnan(dt[2]));
      EXPECT_TRUE(std::isnan(dt[3]));

      const auto& wave = res.variables["wavelength"].float_values;
      ASSERT_EQ(8u, wave.size());
      EXPECT_FLOAT_EQ(300.0f, wave[0]);
      EXPECT_TRUE(std::isnan(wave[3]));
      EXPECT_FLOAT_EQ(300.0f, wave[4]);

      const auto& irr = res.variables["photon_irradiance"].float_values;
      EXPECT_FLOAT_EQ(10.0f, irr[0]);
      EXPECT_TRUE(std::isnan(irr[6]));

      const auto& sys_err =
          res.variables["photon_irradiance_uncertainty_systematic"]
              .float_values;
      EXPECT_FLOAT_EQ(10.0f, sys_err[0]);
      EXPECT_TRUE(std::isnan(sys_err[1]));
      EXPECT_THAT(sys_err[2], FloatNear(300.0f, 1e-3f));

      const auto& rand_err =
          res.variables["photon_irradiance_uncertainty_random"].float_values;
      EXPECT_THAT(rand_err[0], FloatNear(100.0f, 1e-3f));
      EXPECT_TRUE(std::isnan(rand_err[2]));
    }
    if (b == 2) {
      for (double v : res.variables["datetime"].double_values) {
        EXPECT_TRUE(std::isnan(v));
      }
    }
  }

  // 2. S5P_L1B_IR_SIR bands 7 and 8.
  const harp_ingestion_module* sir_mod = FindIngestionModule("S5P_L1B_IR_SIR");
  ASSERT_NE(nullptr, sir_mod);
  for (int b : {7, 8}) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const std::string group = "BAND" + std::to_string(b) + "_IRRADIANCE";
    PopulateS5pGroup(prod, group, /*is_irradiance=*/true,
                     /*array_orbit=*/false, /*fill_time_ref=*/false);
    const std::string opt = "band=" + std::to_string(b);
    IngestExerciseResult res =
        ExerciseIngestionModule(sir_mod, prod.product(), opt.c_str());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["orbit_index"].int32_values, ElementsAre(12345));
  }
}

TEST(HarpIngestS5pL1bTest, IngestRadianceAllBands) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  for (int b = 1; b <= 8; ++b) {
    const std::string mod_name = "S5P_L1B_RA_BD" + std::to_string(b);
    const harp_ingestion_module* mod = FindIngestionModule(mod_name);
    ASSERT_NE(nullptr, mod);

    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const std::string group = "BAND" + std::to_string(b) + "_RADIANCE";
    PopulateS5pGroup(prod, group, /*is_irradiance=*/false,
                     /*array_orbit=*/(b == 1), /*fill_time_ref=*/false);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    if (b == 1) {
      EXPECT_THAT(res.variables["latitude"].float_values,
                  ElementsAre(1.0f, 2.0f, 3.0f, 4.0f));
      EXPECT_THAT(res.variables["longitude"].float_values,
                  ElementsAre(10.0f, 20.0f, 30.0f, 40.0f));
      EXPECT_THAT(res.variables["sensor_latitude"].float_values,
                  ElementsAre(5.0f, 5.0f, 6.0f, 6.0f));
      EXPECT_THAT(res.variables["sensor_longitude"].float_values,
                  ElementsAre(15.0f, 15.0f, 16.0f, 16.0f));
      EXPECT_THAT(res.variables["sensor_altitude"].float_values,
                  ElementsAre(800000.0f, 800000.0f, 800100.0f, 800100.0f));
      EXPECT_THAT(res.variables["solar_zenith_angle"].float_values,
                  ElementsAre(30.0f, 31.0f, 32.0f, 33.0f));
      EXPECT_THAT(res.variables["solar_azimuth_angle"].float_values,
                  ElementsAre(40.0f, 41.0f, 42.0f, 43.0f));
      EXPECT_THAT(res.variables["sensor_zenith_angle"].float_values,
                  ElementsAre(10.0f, 11.0f, 12.0f, 13.0f));
      EXPECT_THAT(res.variables["sensor_azimuth_angle"].float_values,
                  ElementsAre(20.0f, 21.0f, 22.0f, 23.0f));
      EXPECT_EQ(16u, res.variables["latitude_bounds"].float_values.size());
      EXPECT_EQ(16u, res.variables["longitude_bounds"].float_values.size());
      EXPECT_FLOAT_EQ(10.0f, res.variables["photon_radiance"].float_values[0]);
      EXPECT_THAT(
          res.variables["photon_radiance_uncertainty_random"].float_values[0],
          FloatNear(100.0f, 1e-3f));
    }
  }
}

TEST(HarpIngestS5pL1bTest, InitAndReadErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* ir_mod = FindIngestionModule("S5P_L1B_IR_UVN");
  const harp_ingestion_module* ra_mod = FindIngestionModule("S5P_L1B_RA_BD1");
  ASSERT_NE(nullptr, ir_mod);
  ASSERT_NE(nullptr, ra_mod);

  // 1. IR: non-record root fails get_product_group_availability; empty record
  // fails with "no data for band '1'".
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetRootNode(prod.MakeDouble(0.0)));
    IngestExerciseResult res = ExerciseIngestionModule(ir_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(ir_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 2. RA: empty root -> init_cursors fails on goto_first_record_field.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(ra_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. Missing STANDARD_MODE / GEODATA / OBSERVATIONS / INSTRUMENT.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.AddField(prod.root(), "BAND1_IRRADIANCE", prod.MakeRecord()));
    EXPECT_EQ(-1, ExerciseIngestionModule(ir_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* group = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(group, "STANDARD_MODE", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(prod.root(), "BAND1_IRRADIANCE", group));
    EXPECT_EQ(-1, ExerciseIngestionModule(ir_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* group = prod.MakeRecord();
    CodaMemNode* mode = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mode, "GEODATA", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(group, "STANDARD_MODE", mode));
    ASSERT_TRUE(prod.AddField(prod.root(), "BAND1_IRRADIANCE", group));
    EXPECT_EQ(-1, ExerciseIngestionModule(ir_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* group = prod.MakeRecord();
    CodaMemNode* mode = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mode, "GEODATA", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(mode, "OBSERVATIONS", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(group, "STANDARD_MODE", mode));
    ASSERT_TRUE(prod.AddField(prod.root(), "BAND1_IRRADIANCE", group));
    EXPECT_EQ(-1, ExerciseIngestionModule(ir_mod, prod.product()).status);
  }

  // 4. init_dimensions errors: wrong num_dims (3 != 4) and outermost dim != 1.
  auto make_s5p_skeleton = [](CodaMemProduct& prod, std::string_view grp,
                              CodaMemNode*& obs_out, CodaMemNode*& inst_out,
                              CodaMemNode*& geo_out) {
    CodaMemNode* group = prod.MakeRecord();
    CodaMemNode* mode = prod.MakeRecord();
    geo_out = prod.MakeRecord();
    obs_out = prod.MakeRecord();
    inst_out = prod.MakeRecord();
    EXPECT_TRUE(prod.AddField(mode, "GEODATA", geo_out));
    EXPECT_TRUE(prod.AddField(mode, "OBSERVATIONS", obs_out));
    EXPECT_TRUE(prod.AddField(mode, "INSTRUMENT", inst_out));
    EXPECT_TRUE(prod.AddField(group, "STANDARD_MODE", mode));
    EXPECT_TRUE(prod.AddField(prod.root(), grp, group));
  };

  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* obs = nullptr;
    CodaMemNode* inst = nullptr;
    CodaMemNode* geo = nullptr;
    make_s5p_skeleton(prod, "BAND1_IRRADIANCE", obs, inst, geo);
    constexpr long kDims3[] = {1, 2, 2};
    constexpr float kVals3[] = {1.0f, 2.0f, 3.0f, 4.0f};
    ASSERT_TRUE(
        prod.AddField(obs, "irradiance", prod.MakeFloatArray(kDims3, kVals3)));
    IngestExerciseResult res = ExerciseIngestionModule(ir_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* obs = nullptr;
    CodaMemNode* inst = nullptr;
    CodaMemNode* geo = nullptr;
    make_s5p_skeleton(prod, "BAND1_IRRADIANCE", obs, inst, geo);
    constexpr long kDims4[] = {2, 1, 1, 1};
    constexpr float kVals4[] = {1.0f, 2.0f};
    ASSERT_TRUE(
        prod.AddField(obs, "irradiance", prod.MakeFloatArray(kDims4, kVals4)));
    IngestExerciseResult res = ExerciseIngestionModule(ir_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 5. init_dataset errors on IR and RA (using fresh CodaMemProduct per case).
  constexpr long kObsDims1[] = {1, 1, 1, 1};
  constexpr long kWaveDims1[] = {1, 1};
  constexpr float kVal1[] = {1.0f};
  {
    // IR: missing calibrated_wavelength.
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* obs = nullptr;
    CodaMemNode* inst = nullptr;
    CodaMemNode* geo = nullptr;
    make_s5p_skeleton(prod, "BAND1_IRRADIANCE", obs, inst, geo);
    ASSERT_TRUE(prod.AddField(obs, "irradiance",
                              MakeFloatDataset(prod, kObsDims1, kVal1)));
    EXPECT_EQ(-1, ExerciseIngestionModule(ir_mod, prod.product()).status);
  }
  {
    // IR: calibrated_wavelength wrong element count.
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* obs = nullptr;
    CodaMemNode* inst = nullptr;
    CodaMemNode* geo = nullptr;
    make_s5p_skeleton(prod, "BAND1_IRRADIANCE", obs, inst, geo);
    ASSERT_TRUE(prod.AddField(obs, "irradiance",
                              MakeFloatDataset(prod, kObsDims1, kVal1)));
    constexpr long kBadDims[] = {2};
    constexpr float kBadVals[] = {300.0f, 301.0f};
    ASSERT_TRUE(prod.AddField(inst, "calibrated_wavelength",
                              prod.MakeFloatArray(kBadDims, kBadVals)));
    IngestExerciseResult res = ExerciseIngestionModule(ir_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    // IR: calibrated_wavelength missing @FillValue[0].
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* obs = nullptr;
    CodaMemNode* inst = nullptr;
    CodaMemNode* geo = nullptr;
    make_s5p_skeleton(prod, "BAND1_IRRADIANCE", obs, inst, geo);
    ASSERT_TRUE(prod.AddField(obs, "irradiance",
                              MakeFloatDataset(prod, kObsDims1, kVal1)));
    ASSERT_TRUE(prod.AddField(inst, "calibrated_wavelength",
                              prod.MakeFloatArray(kWaveDims1, kVal1)));
    EXPECT_EQ(-1, ExerciseIngestionModule(ir_mod, prod.product()).status);
  }
  {
    // IR: calibrated_wavelength @FillValue[0] wrong type.
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* obs = nullptr;
    CodaMemNode* inst = nullptr;
    CodaMemNode* geo = nullptr;
    make_s5p_skeleton(prod, "BAND1_IRRADIANCE", obs, inst, geo);
    ASSERT_TRUE(prod.AddField(obs, "irradiance",
                              MakeFloatDataset(prod, kObsDims1, kVal1)));
    CodaMemNode* wl = prod.MakeFloatArray(kWaveDims1, kVal1);
    constexpr long kOne[] = {1};
    constexpr std::string_view kBadStr[] = {"bad"};
    ASSERT_TRUE(prod.AddAttribute(wl, "FillValue",
                                  prod.MakeStringArray(kOne, kBadStr)));
    ASSERT_TRUE(prod.AddField(inst, "calibrated_wavelength", wl));
    EXPECT_EQ(-1, ExerciseIngestionModule(ir_mod, prod.product()).status);
  }
  {
    // IR: irradiance missing @FillValue[0].
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* obs = nullptr;
    CodaMemNode* inst = nullptr;
    CodaMemNode* geo = nullptr;
    make_s5p_skeleton(prod, "BAND1_IRRADIANCE", obs, inst, geo);
    ASSERT_TRUE(prod.AddField(inst, "calibrated_wavelength",
                              MakeFloatDataset(prod, kWaveDims1, kVal1)));
    ASSERT_TRUE(prod.AddField(obs, "irradiance",
                              prod.MakeFloatArray(kObsDims1, kVal1)));
    EXPECT_EQ(-1, ExerciseIngestionModule(ir_mod, prod.product()).status);
  }
  {
    // IR: irradiance_error missing.
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* obs = nullptr;
    CodaMemNode* inst = nullptr;
    CodaMemNode* geo = nullptr;
    make_s5p_skeleton(prod, "BAND1_IRRADIANCE", obs, inst, geo);
    ASSERT_TRUE(prod.AddField(inst, "calibrated_wavelength",
                              MakeFloatDataset(prod, kWaveDims1, kVal1)));
    ASSERT_TRUE(prod.AddField(obs, "irradiance",
                              MakeFloatDataset(prod, kObsDims1, kVal1)));
    EXPECT_EQ(-1, ExerciseIngestionModule(ir_mod, prod.product()).status);
  }
  {
    // IR: irradiance_error valid, irradiance_noise is MakeNoData().
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* obs = nullptr;
    CodaMemNode* inst = nullptr;
    CodaMemNode* geo = nullptr;
    make_s5p_skeleton(prod, "BAND1_IRRADIANCE", obs, inst, geo);
    ASSERT_TRUE(prod.AddField(inst, "calibrated_wavelength",
                              MakeFloatDataset(prod, kWaveDims1, kVal1)));
    ASSERT_TRUE(prod.AddField(obs, "irradiance",
                              MakeFloatDataset(prod, kObsDims1, kVal1)));
    ASSERT_TRUE(prod.AddField(obs, "irradiance_error",
                              MakeFloatDataset(prod, kObsDims1, kVal1)));
    ASSERT_TRUE(prod.AddField(obs, "irradiance_noise", prod.MakeNoData()));
    EXPECT_EQ(-1, ExerciseIngestionModule(ir_mod, prod.product()).status);
  }
  {
    // RA: missing radiance -> init_dimensions fails.
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* obs = nullptr;
    CodaMemNode* inst = nullptr;
    CodaMemNode* geo = nullptr;
    make_s5p_skeleton(prod, "BAND1_RADIANCE", obs, inst, geo);
    EXPECT_EQ(-1, ExerciseIngestionModule(ra_mod, prod.product()).status);
  }
  {
    // RA: missing nominal_wavelength.
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* obs = nullptr;
    CodaMemNode* inst = nullptr;
    CodaMemNode* geo = nullptr;
    make_s5p_skeleton(prod, "BAND1_RADIANCE", obs, inst, geo);
    ASSERT_TRUE(prod.AddField(obs, "radiance",
                              MakeFloatDataset(prod, kObsDims1, kVal1)));
    EXPECT_EQ(-1, ExerciseIngestionModule(ra_mod, prod.product()).status);
  }
  {
    // RA: radiance missing @FillValue[0].
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* obs = nullptr;
    CodaMemNode* inst = nullptr;
    CodaMemNode* geo = nullptr;
    make_s5p_skeleton(prod, "BAND1_RADIANCE", obs, inst, geo);
    ASSERT_TRUE(
        prod.AddField(obs, "radiance", prod.MakeFloatArray(kObsDims1, kVal1)));
    ASSERT_TRUE(prod.AddField(inst, "nominal_wavelength",
                              MakeFloatDataset(prod, kWaveDims1, kVal1)));
    EXPECT_EQ(-1, ExerciseIngestionModule(ra_mod, prod.product()).status);
  }
  {
    // RA: missing radiance_error.
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* obs = nullptr;
    CodaMemNode* inst = nullptr;
    CodaMemNode* geo = nullptr;
    make_s5p_skeleton(prod, "BAND1_RADIANCE", obs, inst, geo);
    ASSERT_TRUE(prod.AddField(obs, "radiance",
                              MakeFloatDataset(prod, kObsDims1, kVal1)));
    ASSERT_TRUE(prod.AddField(inst, "nominal_wavelength",
                              MakeFloatDataset(prod, kWaveDims1, kVal1)));
    EXPECT_EQ(-1, ExerciseIngestionModule(ra_mod, prod.product()).status);
  }
  {
    // RA: missing radiance_noise.
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* obs = nullptr;
    CodaMemNode* inst = nullptr;
    CodaMemNode* geo = nullptr;
    make_s5p_skeleton(prod, "BAND1_RADIANCE", obs, inst, geo);
    ASSERT_TRUE(prod.AddField(obs, "radiance",
                              MakeFloatDataset(prod, kObsDims1, kVal1)));
    ASSERT_TRUE(prod.AddField(obs, "radiance_error",
                              MakeFloatDataset(prod, kObsDims1, kVal1)));
    ASSERT_TRUE(prod.AddField(inst, "nominal_wavelength",
                              MakeFloatDataset(prod, kWaveDims1, kVal1)));
    EXPECT_EQ(-1, ExerciseIngestionModule(ra_mod, prod.product()).status);
  }

  // 6. Variable read errors on valid RA init: missing/bad time, delta_time,
  // orbit, GEODATA, and partial array reads.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* obs = nullptr;
    CodaMemNode* inst = nullptr;
    CodaMemNode* geo = nullptr;
    make_s5p_skeleton(prod, "BAND1_RADIANCE", obs, inst, geo);
    ASSERT_TRUE(prod.AddField(obs, "radiance",
                              MakeFloatDataset(prod, kObsDims1, kVal1)));
    ASSERT_TRUE(prod.AddField(obs, "radiance_error",
                              MakeFloatDataset(prod, kObsDims1, kVal1)));
    ASSERT_TRUE(prod.AddField(obs, "radiance_noise",
                              MakeFloatDataset(prod, kObsDims1, kVal1)));
    ASSERT_TRUE(prod.AddField(inst, "nominal_wavelength",
                              MakeFloatDataset(prod, kWaveDims1, kVal1)));

    // time wrong count (2 != 1).
    constexpr long kTwo[] = {2};
    constexpr double kTwoD[] = {1.0, 2.0};
    ASSERT_TRUE(prod.AddField(obs, "time", prod.MakeDoubleArray(kTwo, kTwoD)));
    // latitude wrong count (2 != 1).
    constexpr float kTwoF[] = {1.0f, 2.0f};
    ASSERT_TRUE(
        prod.AddField(geo, "latitude", MakeFloatDataset(prod, kTwo, kTwoF)));
    // longitude missing @FillValue[0].
    constexpr long kOne[] = {1};
    ASSERT_TRUE(
        prod.AddField(geo, "longitude", prod.MakeFloatArray(kOne, kVal1)));
    // latitude_bounds is MakeNoData() -> coda_cursor_get_num_elements fails.
    ASSERT_TRUE(prod.AddField(geo, "latitude_bounds", prod.MakeNoData()));
    // longitude_bounds has string @FillValue[0] -> read_float fails.
    CodaMemNode* lb = prod.MakeFloatArray(kOne, kVal1);
    constexpr std::string_view kBadStr[] = {"bad"};
    ASSERT_TRUE(prod.AddAttribute(lb, "FillValue",
                                  prod.MakeStringArray(kOne, kBadStr)));
    ASSERT_TRUE(prod.AddField(geo, "longitude_bounds", lb));
    // satellite_latitude wrong type (string array).
    ASSERT_TRUE(prod.AddField(geo, "satellite_latitude",
                              prod.MakeStringArray(kOne, kBadStr)));
    // orbit is string -> coda_cursor_read_int32 fails.
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "orbit", prod.MakeString("bad")));

    IngestExerciseResult res =
        ExerciseIngestionModule(ra_mod, prod.product(), /*options_str=*/nullptr,
                                /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["longitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_latitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_longitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_altitude"].read_status);
  }

  // 7. read_datetime error paths across time & delta_time variations.
  auto check_datetime_failure = [&](auto setup_obs) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* obs = nullptr;
    CodaMemNode* inst = nullptr;
    CodaMemNode* geo = nullptr;
    make_s5p_skeleton(prod, "BAND1_IRRADIANCE", obs, inst, geo);
    ASSERT_TRUE(prod.AddField(obs, "irradiance",
                              MakeFloatDataset(prod, kObsDims1, kVal1)));
    ASSERT_TRUE(prod.AddField(obs, "irradiance_error",
                              MakeFloatDataset(prod, kObsDims1, kVal1)));
    ASSERT_TRUE(prod.AddField(obs, "irradiance_noise",
                              MakeFloatDataset(prod, kObsDims1, kVal1)));
    ASSERT_TRUE(prod.AddField(inst, "calibrated_wavelength",
                              MakeFloatDataset(prod, kWaveDims1, kVal1)));
    setup_obs(prod, obs);
    IngestExerciseResult res =
        ExerciseIngestionModule(ir_mod, prod.product(), /*options_str=*/nullptr,
                                /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
  };
  constexpr long kOne[] = {1};
  constexpr double kOneD[] = {100.0};
  constexpr std::string_view kBadStr[] = {"bad"};
  // Missing time.
  check_datetime_failure([](CodaMemProduct&, CodaMemNode*) {});
  // time is MakeNoData().
  check_datetime_failure([](CodaMemProduct& p, CodaMemNode* o) {
    ASSERT_TRUE(p.AddField(o, "time", p.MakeNoData()));
  });
  // time is scalar (goto_first_array_element fails).
  check_datetime_failure([](CodaMemProduct& p, CodaMemNode* o) {
    ASSERT_TRUE(p.AddField(o, "time", p.MakeDouble(100.0)));
  });
  // time is 1-element string array (read_double fails).
  check_datetime_failure([&](CodaMemProduct& p, CodaMemNode* o) {
    ASSERT_TRUE(p.AddField(o, "time", p.MakeStringArray(kOne, kBadStr)));
  });
  // delta_time missing.
  check_datetime_failure([&](CodaMemProduct& p, CodaMemNode* o) {
    ASSERT_TRUE(p.AddField(o, "time", p.MakeDoubleArray(kOne, kOneD)));
  });
  // delta_time is MakeNoData().
  check_datetime_failure([&](CodaMemProduct& p, CodaMemNode* o) {
    ASSERT_TRUE(p.AddField(o, "time", p.MakeDoubleArray(kOne, kOneD)));
    ASSERT_TRUE(p.AddField(o, "delta_time", p.MakeNoData()));
  });
  // delta_time wrong count (2 != 1).
  check_datetime_failure([&](CodaMemProduct& p, CodaMemNode* o) {
    constexpr long kTwo[] = {2};
    constexpr double kTwoD[] = {1.0, 2.0};
    ASSERT_TRUE(p.AddField(o, "time", p.MakeDoubleArray(kOne, kOneD)));
    ASSERT_TRUE(p.AddField(o, "delta_time", p.MakeDoubleArray(kTwo, kTwoD)));
  });
  // delta_time string array (read_double_array fails).
  check_datetime_failure([&](CodaMemProduct& p, CodaMemNode* o) {
    ASSERT_TRUE(p.AddField(o, "time", p.MakeDoubleArray(kOne, kOneD)));
    ASSERT_TRUE(p.AddField(o, "delta_time", p.MakeStringArray(kOne, kBadStr)));
  });
  // delta_time missing @FillValue[0].
  check_datetime_failure([&](CodaMemProduct& p, CodaMemNode* o) {
    ASSERT_TRUE(p.AddField(o, "time", p.MakeDoubleArray(kOne, kOneD)));
    ASSERT_TRUE(p.AddField(o, "delta_time", p.MakeDoubleArray(kOne, kOneD)));
  });
  // delta_time string @FillValue[0].
  check_datetime_failure([&](CodaMemProduct& p, CodaMemNode* o) {
    ASSERT_TRUE(p.AddField(o, "time", p.MakeDoubleArray(kOne, kOneD)));
    CodaMemNode* dt = p.MakeDoubleArray(kOne, kOneD);
    ASSERT_TRUE(
        p.AddAttribute(dt, "FillValue", p.MakeStringArray(kOne, kBadStr)));
    ASSERT_TRUE(p.AddField(o, "delta_time", dt));
  });

  // 8. Partial array read failures in read_observable_error and
  // read_observable_noise (error/noise string arrays, and radiance string
  // array).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* obs = nullptr;
    CodaMemNode* inst = nullptr;
    CodaMemNode* geo = nullptr;
    make_s5p_skeleton(prod, "BAND1_RADIANCE", obs, inst, geo);
    CodaMemNode* rad_err = prod.MakeStringArray(kObsDims1, kBadStr);
    CodaMemNode* rad_noise = prod.MakeStringArray(kObsDims1, kBadStr);
    ASSERT_TRUE(prod.AddAttribute(rad_err, "FillValue",
                                  prod.MakeFloatArray(kOne, kVal1)));
    ASSERT_TRUE(prod.AddAttribute(rad_noise, "FillValue",
                                  prod.MakeFloatArray(kOne, kVal1)));
    ASSERT_TRUE(prod.AddField(obs, "radiance",
                              MakeFloatDataset(prod, kObsDims1, kVal1)));
    ASSERT_TRUE(prod.AddField(obs, "radiance_error", rad_err));
    ASSERT_TRUE(prod.AddField(obs, "radiance_noise", rad_noise));
    ASSERT_TRUE(prod.AddField(inst, "nominal_wavelength",
                              MakeFloatDataset(prod, kWaveDims1, kVal1)));

    IngestExerciseResult res =
        ExerciseIngestionModule(ra_mod, prod.product(), /*options_str=*/nullptr,
                                /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(
        -1,
        res.variables["photon_radiance_uncertainty_systematic"].read_status);
    EXPECT_EQ(-1,
              res.variables["photon_radiance_uncertainty_random"].read_status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* obs = nullptr;
    CodaMemNode* inst = nullptr;
    CodaMemNode* geo = nullptr;
    make_s5p_skeleton(prod, "BAND1_RADIANCE", obs, inst, geo);
    CodaMemNode* rad = prod.MakeStringArray(kObsDims1, kBadStr);
    ASSERT_TRUE(
        prod.AddAttribute(rad, "FillValue", prod.MakeFloatArray(kOne, kVal1)));
    ASSERT_TRUE(prod.AddField(obs, "radiance", rad));
    ASSERT_TRUE(prod.AddField(obs, "radiance_error",
                              MakeFloatDataset(prod, kObsDims1, kVal1)));
    ASSERT_TRUE(prod.AddField(obs, "radiance_noise",
                              MakeFloatDataset(prod, kObsDims1, kVal1)));
    ASSERT_TRUE(prod.AddField(inst, "nominal_wavelength",
                              MakeFloatDataset(prod, kWaveDims1, kVal1)));

    IngestExerciseResult res =
        ExerciseIngestionModule(ra_mod, prod.product(), /*options_str=*/nullptr,
                                /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["photon_radiance"].read_status);
    EXPECT_EQ(
        -1,
        res.variables["photon_radiance_uncertainty_systematic"].read_status);
    EXPECT_EQ(-1,
              res.variables["photon_radiance_uncertainty_random"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
