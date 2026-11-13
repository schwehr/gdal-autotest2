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
// Unit tests for EarthCARE JAXA ingestion modules
// (libharp/harp-ingest-earthcare_jaxa.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-earthcare_jaxa.c

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

using ::testing::DoubleNear;
using ::testing::ElementsAre;

// Helper to attach
// /HeaderData/VariableProductHeader/MainProductHeader/orbitNumber.
void AddOrbitNumber(CodaMemProduct& prod, uint32_t orbit) {
  CodaMemNode* header = prod.MakeRecord();
  CodaMemNode* var_hdr = prod.MakeRecord();
  CodaMemNode* main_hdr = prod.MakeRecord();
  constexpr long kOne[] = {1};
  const uint32_t kOrbit[] = {orbit};
  ASSERT_TRUE(prod.AddField(main_hdr, "orbitNumber",
                            prod.MakeUint32Array(kOne, kOrbit)));
  ASSERT_TRUE(prod.AddField(var_hdr, "MainProductHeader", main_hdr));
  ASSERT_TRUE(prod.AddField(header, "VariableProductHeader", var_hdr));
  ASSERT_TRUE(prod.AddField(prod.root(), "HeaderData", header));
}

// Helper to create /ScienceData containing Geo and Data sub-records.
void AddScienceData(CodaMemProduct& prod, CodaMemNode* geo, CodaMemNode* data) {
  CodaMemNode* sci = prod.MakeRecord();
  ASSERT_TRUE(prod.AddField(sci, "Geo", geo));
  if (data != nullptr) {
    ASSERT_TRUE(prod.AddField(sci, "Data", data));
  }
  ASSERT_TRUE(prod.AddField(prod.root(), "ScienceData", sci));
}

TEST(HarpIngestEarthcareJaxaTest, IngestCprNom1b) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ECA_CPR_NOM_1B");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  AddOrbitNumber(prod, 1234);

  CodaMemNode* geo = prod.MakeRecord();
  CodaMemNode* data = prod.MakeRecord();

  constexpr long kTimeDims[] = {2};
  constexpr long kProfDims[] = {2, 2};
  constexpr double kTime[] = {100.0, 101.0};
  constexpr double kLat[] = {10.0, 20.0};
  constexpr double kLon[] = {-5.0, 5.0};
  constexpr double kBinHeight[] = {2000.0, 1000.0, 2200.0, 1200.0};
  constexpr double kSza[] = {30.0, 31.0};
  constexpr double kSea[] = {60.0, 61.0};
  constexpr double kSurf[] = {100.0, 200.0};
  constexpr double kDoppler[] = {1.5, 2.5, 3.5, 4.5};
  constexpr double kRefl[] = {10.0, 20.0, 30.0, 40.0};

  ASSERT_TRUE(prod.AddField(geo, "profileTime",
                            prod.MakeDoubleArray(kTimeDims, kTime)));
  ASSERT_TRUE(
      prod.AddField(geo, "latitude", prod.MakeDoubleArray(kTimeDims, kLat)));
  ASSERT_TRUE(
      prod.AddField(geo, "longitude", prod.MakeDoubleArray(kTimeDims, kLon)));
  ASSERT_TRUE(prod.AddField(geo, "binHeight",
                            prod.MakeDoubleArray(kProfDims, kBinHeight)));
  ASSERT_TRUE(prod.AddField(geo, "solarAzimuthAngle",
                            prod.MakeDoubleArray(kTimeDims, kSza)));
  ASSERT_TRUE(prod.AddField(geo, "solarElevationAngle",
                            prod.MakeDoubleArray(kTimeDims, kSea)));
  ASSERT_TRUE(prod.AddField(geo, "surfaceElevation",
                            prod.MakeDoubleArray(kTimeDims, kSurf)));

  ASSERT_TRUE(prod.AddField(data, "dopplerVelocity",
                            prod.MakeDoubleArray(kProfDims, kDoppler)));
  ASSERT_TRUE(prod.AddField(data, "radarReflectivityFactor",
                            prod.MakeDoubleArray(kProfDims, kRefl)));
  AddScienceData(prod, geo, data);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(2, res.dimensions[harp_dimension_vertical]);
  EXPECT_THAT(res.variables["orbit_index"].int32_values, ElementsAre(1234));
  EXPECT_THAT(res.variables["datetime"].double_values,
              ElementsAre(100.0, 101.0));
  // Vertical axis inverted from [2000, 1000] to [1000, 2000].
  EXPECT_THAT(res.variables["altitude"].double_values,
              ElementsAre(1000.0, 2000.0, 1200.0, 2200.0));
  EXPECT_THAT(res.variables["doppler_velocity"].double_values,
              ElementsAre(2.5, 1.5, 4.5, 3.5));
  EXPECT_THAT(res.variables["radar_reflectivity_factor"].double_values,
              ElementsAre(20.0, 10.0, 40.0, 30.0));
}

TEST(HarpIngestEarthcareJaxaTest, IngestAtlCla2aWithUint8QualityFlag) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ECA_ATL_CLA_2A");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  AddOrbitNumber(prod, 200);

  CodaMemNode* geo = prod.MakeRecord();
  CodaMemNode* data = prod.MakeRecord();

  constexpr long kTimeDims[] = {2};
  constexpr long kProfDims[] = {2, 2};
  constexpr double kTime[] = {10.0, 20.0};
  constexpr double kLat[] = {1.0, 2.0};
  constexpr double kLon[] = {3.0, 4.0};
  constexpr double kHeight[] = {5000.0, 1000.0, 6000.0, 2000.0};
  constexpr uint8_t kQual[] = {1, 2, 3, 4};
  constexpr double kVal[] = {0.1, 0.2, 0.3, 0.4};

  ASSERT_TRUE(
      prod.AddField(geo, "time", prod.MakeDoubleArray(kTimeDims, kTime)));
  ASSERT_TRUE(
      prod.AddField(geo, "latitude", prod.MakeDoubleArray(kTimeDims, kLat)));
  ASSERT_TRUE(
      prod.AddField(geo, "longitude", prod.MakeDoubleArray(kTimeDims, kLon)));
  ASSERT_TRUE(
      prod.AddField(geo, "height", prod.MakeDoubleArray(kProfDims, kHeight)));

  ASSERT_TRUE(prod.AddField(data, "quality_flag_10km",
                            prod.MakeUint8Array(kProfDims, kQual)));
  ASSERT_TRUE(prod.AddField(data, "aerosol_backscatter_10km",
                            prod.MakeDoubleArray(kProfDims, kVal)));
  ASSERT_TRUE(prod.AddField(data, "aerosol_extinction_10km",
                            prod.MakeDoubleArray(kProfDims, kVal)));
  ASSERT_TRUE(prod.AddField(data, "aerosol_lidar_ratio_10km",
                            prod.MakeDoubleArray(kProfDims, kVal)));
  ASSERT_TRUE(prod.AddField(data, "cloud_backscatter_10km",
                            prod.MakeDoubleArray(kProfDims, kVal)));
  ASSERT_TRUE(prod.AddField(data, "cloud_extinction_10km",
                            prod.MakeDoubleArray(kProfDims, kVal)));
  ASSERT_TRUE(prod.AddField(data, "cloud_lidar_ratio_10km",
                            prod.MakeDoubleArray(kProfDims, kVal)));
  AddScienceData(prod, geo, data);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_THAT(res.variables["altitude"].double_values,
              ElementsAre(1000.0, 5000.0, 2000.0, 6000.0));
  EXPECT_THAT(res.variables["validity"].int8_values, ElementsAre(2, 1, 4, 3));
  EXPECT_THAT(res.variables["aerosol_backscatter_coefficient"].double_values,
              ElementsAre(0.2, 0.1, 0.4, 0.3));
}

TEST(HarpIngestEarthcareJaxaTest, IngestCprClp2aWithRelativeUncertainties) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ECA_CPR_CLP_2A");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  AddOrbitNumber(prod, 300);

  CodaMemNode* geo = prod.MakeRecord();
  CodaMemNode* data = prod.MakeRecord();

  constexpr long kTimeDims[] = {1};
  constexpr long kProfDims[] = {1, 2};
  constexpr double kTime[] = {50.0};
  constexpr double kLat[] = {0.0};
  constexpr double kLon[] = {0.0};
  constexpr double kHeight[] = {3000.0, 1000.0};
  constexpr double kAirVel[] = {1.0, 2.0};
  constexpr double kIce[] = {20.0, 40.0};
  constexpr double kIceUnc[] = {10.0, 25.0};  // 10%, 25%
  constexpr double kIceReff[] = {50.0, 100.0};
  constexpr double kIceReffUnc[] = {20.0, 5.0};
  constexpr double kWater[] = {10.0, 30.0};
  constexpr double kWaterUnc[] = {50.0, 10.0};
  constexpr double kWaterReff[] = {8.0, 12.0};
  constexpr double kWaterReffUnc[] = {25.0, 50.0};
  constexpr double kTau[] = {2.5};

  ASSERT_TRUE(
      prod.AddField(geo, "time", prod.MakeDoubleArray(kTimeDims, kTime)));
  ASSERT_TRUE(
      prod.AddField(geo, "latitude", prod.MakeDoubleArray(kTimeDims, kLat)));
  ASSERT_TRUE(
      prod.AddField(geo, "longitude", prod.MakeDoubleArray(kTimeDims, kLon)));
  ASSERT_TRUE(
      prod.AddField(geo, "height", prod.MakeDoubleArray(kProfDims, kHeight)));

  ASSERT_TRUE(prod.AddField(data, "cloud_air_velocity_10km",
                            prod.MakeDoubleArray(kProfDims, kAirVel)));
  ASSERT_TRUE(prod.AddField(data, "cloud_ice_content_10km",
                            prod.MakeDoubleArray(kProfDims, kIce)));
  ASSERT_TRUE(prod.AddField(data, "cloud_ice_content_10km_uncertainty",
                            prod.MakeDoubleArray(kProfDims, kIceUnc)));
  ASSERT_TRUE(prod.AddField(data, "cloud_ice_effective_radius_10km",
                            prod.MakeDoubleArray(kProfDims, kIceReff)));
  ASSERT_TRUE(prod.AddField(data, "cloud_ice_effective_radius_10km_uncertainty",
                            prod.MakeDoubleArray(kProfDims, kIceReffUnc)));
  ASSERT_TRUE(prod.AddField(data, "cloud_water_content_10km",
                            prod.MakeDoubleArray(kProfDims, kWater)));
  ASSERT_TRUE(prod.AddField(data, "cloud_water_content_10km_uncertainty",
                            prod.MakeDoubleArray(kProfDims, kWaterUnc)));
  ASSERT_TRUE(prod.AddField(data, "cloud_water_effective_radius_10km",
                            prod.MakeDoubleArray(kProfDims, kWaterReff)));
  ASSERT_TRUE(prod.AddField(data,
                            "cloud_water_effective_radius_10km_uncertainty",
                            prod.MakeDoubleArray(kProfDims, kWaterReffUnc)));
  ASSERT_TRUE(prod.AddField(data, "optical_thickness_10km",
                            prod.MakeDoubleArray(kTimeDims, kTau)));
  AddScienceData(prod, geo, data);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  // Inverted and converted from % to absolute.
  EXPECT_THAT(res.variables["ice_water_density_uncertainty"].double_values,
              ElementsAre(DoubleNear(10.0, 1e-12), DoubleNear(2.0, 1e-12)));
  EXPECT_THAT(
      res.variables["ice_water_effective_radius_uncertainty"].double_values,
      ElementsAre(DoubleNear(5.0, 1e-12), DoubleNear(10.0, 1e-12)));
  EXPECT_THAT(res.variables["liquid_water_density_uncertainty"].double_values,
              ElementsAre(DoubleNear(3.0, 1e-12), DoubleNear(5.0, 1e-12)));
  EXPECT_THAT(
      res.variables["cloud_water_effective_radius_uncertainty"].double_values,
      ElementsAre(DoubleNear(6.0, 1e-12), DoubleNear(2.0, 1e-12)));
  EXPECT_THAT(res.variables["optical_depth"].double_values, ElementsAre(2.5));
}

TEST(HarpIngestEarthcareJaxaTest, IngestCprEco2aDefaultAndBiasCorrected) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ECA_CPR_ECO_2A");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  AddOrbitNumber(prod, 400);

  CodaMemNode* geo = prod.MakeRecord();
  CodaMemNode* data = prod.MakeRecord();

  constexpr long kTimeDims[] = {1};
  constexpr long kProfDims[] = {1, 2};
  constexpr double kTime[] = {60.0};
  constexpr double kLat[] = {12.0};
  constexpr double kLon[] = {34.0};
  constexpr double kBinHeight[] = {4000.0, 2000.0};
  constexpr double kVel[] = {1.0, 2.0};
  constexpr double kVelBias[] = {1.1, 2.2};
  constexpr int32_t kFlag[] = {0, 1};
  constexpr uint32_t kFlagBias[] = {2, 3};
  constexpr double kRefl[] = {5.0, 6.0};

  ASSERT_TRUE(
      prod.AddField(geo, "time", prod.MakeDoubleArray(kTimeDims, kTime)));
  ASSERT_TRUE(
      prod.AddField(geo, "latitude", prod.MakeDoubleArray(kTimeDims, kLat)));
  ASSERT_TRUE(
      prod.AddField(geo, "longitude", prod.MakeDoubleArray(kTimeDims, kLon)));
  ASSERT_TRUE(prod.AddField(geo, "bin_height",
                            prod.MakeDoubleArray(kProfDims, kBinHeight)));

  ASSERT_TRUE(prod.AddField(data, "integrated_doppler_velocity_10km",
                            prod.MakeDoubleArray(kProfDims, kVel)));
  ASSERT_TRUE(prod.AddField(data, "integrated_doppler_velocity_10km_bias_corr",
                            prod.MakeDoubleArray(kProfDims, kVelBias)));
  ASSERT_TRUE(prod.AddField(data, "doppler_velocity_quality_flag_10km",
                            prod.MakeInt32Array(kProfDims, kFlag)));
  ASSERT_TRUE(prod.AddField(data,
                            "doppler_velocity_quality_flag_10km_bias_corr",
                            prod.MakeUint32Array(kProfDims, kFlagBias)));
  ASSERT_TRUE(prod.AddField(data, "integrated_radar_reflectivity_10km",
                            prod.MakeDoubleArray(kProfDims, kRefl)));
  AddScienceData(prod, geo, data);

  IngestExerciseResult def = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, def.status);
  EXPECT_THAT(def.variables["altitude"].double_values,
              ElementsAre(2000.0, 4000.0));
  EXPECT_THAT(def.variables["doppler_velocity"].double_values,
              ElementsAre(2.0, 1.0));
  EXPECT_THAT(def.variables["doppler_velocity_validity"].int32_values,
              ElementsAre(1, 0));

  IngestExerciseResult bc =
      ExerciseIngestionModule(mod, prod.product(), "bias_corrected=true");
  ASSERT_EQ(HARP_SUCCESS, bc.status);
  EXPECT_THAT(bc.variables["doppler_velocity"].double_values,
              ElementsAre(2.2, 1.1));
  EXPECT_THAT(bc.variables["doppler_velocity_validity"].int32_values,
              ElementsAre(3, 2));
}

TEST(HarpIngestEarthcareJaxaTest, IngestMsiClp2aSwath2DReplicationAndInt8) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ECA_MSI_CLP_2A");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  AddOrbitNumber(prod, 500);

  CodaMemNode* geo = prod.MakeRecord();
  CodaMemNode* data = prod.MakeRecord();

  // 2 along-track x 2 across-track = 4 time elements; /Geo/time has 2 elements.
  constexpr long kAlongDims[] = {2};
  constexpr long kSwathDims[] = {2, 2};
  constexpr double kTime[] = {100.0, 200.0};
  constexpr double kLat[] = {1.0, 2.0, 3.0, 4.0};
  constexpr double kLon[] = {5.0, 6.0, 7.0, 8.0};
  constexpr int8_t kQual[] = {0, 1, 2, 3};
  constexpr double kTau[] = {0.5, 1.0, 1.5, 2.0};
  constexpr double kCth[] = {1000.0, 2000.0, 3000.0, 4000.0};
  constexpr double kCtp[] = {900.0, 800.0, 700.0, 600.0};
  constexpr double kCtt[] = {270.0, 260.0, 250.0, 240.0};

  ASSERT_TRUE(
      prod.AddField(geo, "time", prod.MakeDoubleArray(kAlongDims, kTime)));
  ASSERT_TRUE(
      prod.AddField(geo, "latitude", prod.MakeDoubleArray(kSwathDims, kLat)));
  ASSERT_TRUE(
      prod.AddField(geo, "longitude", prod.MakeDoubleArray(kSwathDims, kLon)));

  ASSERT_TRUE(prod.AddField(data, "quality_flag",
                            prod.MakeInt8Array(kSwathDims, kQual)));
  ASSERT_TRUE(prod.AddField(data, "cloud_optical_thickness",
                            prod.MakeDoubleArray(kSwathDims, kTau)));
  ASSERT_TRUE(prod.AddField(data, "cloud_top_height",
                            prod.MakeDoubleArray(kSwathDims, kCth)));
  ASSERT_TRUE(prod.AddField(data, "cloud_top_pressure",
                            prod.MakeDoubleArray(kSwathDims, kCtp)));
  ASSERT_TRUE(prod.AddField(data, "cloud_top_temperature",
                            prod.MakeDoubleArray(kSwathDims, kCtt)));
  AddScienceData(prod, geo, data);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(4, res.dimensions[harp_dimension_time]);
  EXPECT_THAT(res.variables["datetime"].double_values,
              ElementsAre(100.0, 100.0, 200.0, 200.0));
  EXPECT_THAT(res.variables["validity"].int8_values, ElementsAre(0, 1, 2, 3));
  EXPECT_THAT(res.variables["cloud_optical_depth"].double_values,
              ElementsAre(0.5, 1.0, 1.5, 2.0));
  EXPECT_THAT(res.variables["cloud_top_height"].double_values,
              ElementsAre(1000.0, 2000.0, 3000.0, 4000.0));
  EXPECT_THAT(res.variables["cloud_top_pressure"].double_values,
              ElementsAre(900.0, 800.0, 700.0, 600.0));
  EXPECT_THAT(res.variables["cloud_top_temperature"].double_values,
              ElementsAre(270.0, 260.0, 250.0, 240.0));
}

TEST(HarpIngestEarthcareJaxaTest, ErrorPathsMissingAndInvalidFields) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* cpr = FindIngestionModule("ECA_CPR_NOM_1B");
  const harp_ingestion_module* clp = FindIngestionModule("ECA_CPR_CLP_2A");
  const harp_ingestion_module* eco = FindIngestionModule("ECA_CPR_ECO_2A");
  const harp_ingestion_module* msi = FindIngestionModule("ECA_MSI_CLP_2A");
  ASSERT_NE(nullptr, cpr);
  ASSERT_NE(nullptr, clp);
  ASSERT_NE(nullptr, eco);
  ASSERT_NE(nullptr, msi);

  // 1. Missing ScienceData, Geo, latitude, and scalar latitude.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    EXPECT_EQ(-1, ExerciseIngestionModule(cpr, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "ScienceData", prod.MakeRecord()));
    EXPECT_EQ(-1, ExerciseIngestionModule(cpr, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    AddScienceData(prod, prod.MakeRecord(), nullptr);
    EXPECT_EQ(-1, ExerciseIngestionModule(cpr, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* geo = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(geo, "latitude", prod.MakeDouble(1.0)));
    AddScienceData(prod, geo, nullptr);
    EXPECT_EQ(-1, ExerciseIngestionModule(cpr, prod.product()).status);
  }

  // 2. Variable read failures: missing fields, element count mismatch, wrong
  // types (including scalar int8/int32 and string orbitNumber).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* header = prod.MakeRecord();
    CodaMemNode* var_hdr = prod.MakeRecord();
    CodaMemNode* main_hdr = prod.MakeRecord();
    constexpr long kOne[] = {1};
    constexpr std::string_view kBadOrbit[] = {"bad"};
    ASSERT_TRUE(prod.AddField(main_hdr, "orbitNumber",
                              prod.MakeStringArray(kOne, kBadOrbit)));
    ASSERT_TRUE(prod.AddField(var_hdr, "MainProductHeader", main_hdr));
    ASSERT_TRUE(prod.AddField(header, "VariableProductHeader", var_hdr));
    ASSERT_TRUE(prod.AddField(prod.root(), "HeaderData", header));

    CodaMemNode* geo = prod.MakeRecord();
    CodaMemNode* data = prod.MakeRecord();
    constexpr double kLat[] = {10.0};
    constexpr double kH[] = {100.0};
    constexpr std::string_view kBadStr[] = {"bad"};
    ASSERT_TRUE(
        prod.AddField(geo, "latitude", prod.MakeDoubleArray(kOne, kLat)));
    ASSERT_TRUE(
        prod.AddField(geo, "bin_height", prod.MakeDoubleArray(kOne, kH)));
    ASSERT_TRUE(
        prod.AddField(geo, "time", prod.MakeStringArray(kOne, kBadStr)));
    // Missing uncertainty field vs present base field and vice versa.
    ASSERT_TRUE(prod.AddField(data, "cloud_ice_content_10km",
                              prod.MakeDoubleArray(kOne, kLat)));
    ASSERT_TRUE(prod.AddField(data, "doppler_velocity_quality_flag_10km",
                              prod.MakeInt32(0)));
    ASSERT_TRUE(prod.AddField(data, "quality_flag", prod.MakeInt8(0)));
    AddScienceData(prod, geo, data);

    IngestExerciseResult eco_res = ExerciseIngestionModule(
        eco, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, eco_res.status);
    EXPECT_EQ(-1, eco_res.variables["datetime"].read_status);
    EXPECT_EQ(-1, eco_res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, eco_res.variables["doppler_velocity_validity"].read_status);

    IngestExerciseResult clp_res = ExerciseIngestionModule(
        clp, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, clp_res.status);
    EXPECT_EQ(-1,
              clp_res.variables["ice_water_density_uncertainty"].read_status);

    IngestExerciseResult msi_res = ExerciseIngestionModule(
        msi, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, msi_res.status);
    EXPECT_EQ(-1, msi_res.variables["validity"].read_status);
  }

  // 3. Mismatched element counts and missing /HeaderData orbitNumber.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* geo = prod.MakeRecord();
    CodaMemNode* data = prod.MakeRecord();
    constexpr long kOne[] = {1};
    constexpr long kTwo[] = {2};
    constexpr double kLat[] = {10.0};
    constexpr double kTwoD[] = {1.0, 2.0};
    constexpr std::string_view kBad1[] = {"x"};
    ASSERT_TRUE(
        prod.AddField(geo, "latitude", prod.MakeDoubleArray(kOne, kLat)));
    ASSERT_TRUE(
        prod.AddField(geo, "bin_height", prod.MakeDoubleArray(kOne, kLat)));
    ASSERT_TRUE(prod.AddField(geo, "time", prod.MakeDoubleArray(kTwo, kTwoD)));
    ASSERT_TRUE(
        prod.AddField(data, "quality_flag", prod.MakeStringArray(kOne, kBad1)));
    ASSERT_TRUE(prod.AddField(data, "doppler_velocity_quality_flag_10km",
                              prod.MakeStringArray(kOne, kBad1)));
    AddScienceData(prod, geo, data);

    IngestExerciseResult eco_res = ExerciseIngestionModule(
        eco, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, eco_res.status);
    EXPECT_EQ(-1, eco_res.variables["datetime"].read_status);
    EXPECT_EQ(-1, eco_res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, eco_res.variables["doppler_velocity_validity"].read_status);

    IngestExerciseResult msi_res = ExerciseIngestionModule(
        msi, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, msi_res.status);
    EXPECT_EQ(-1, msi_res.variables["validity"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
