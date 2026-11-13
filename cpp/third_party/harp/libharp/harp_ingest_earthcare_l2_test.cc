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
// Unit tests for EarthCARE L2 ingestion modules
// (libharp/harp-ingest-earthcare_l2.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-earthcare_l2.c

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

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

using ::testing::ElementsAre;
using ::testing::FloatNear;

constexpr std::array<long, 1> kOrbitDims = {1};
constexpr std::array<long, 1> kDims1D = {2};
constexpr std::array<long, 2> kDims2D = {2, 2};
constexpr std::array<long, 2> kDimsBbrDir = {2, 3};
constexpr std::array<long, 3> kDims2D2 = {2, 2, 2};
constexpr std::array<long, 3> kDims2D3 = {2, 2, 3};
constexpr std::array<long, 3> kDimsBbrBounds = {2, 4, 2};

constexpr std::array<double, 2> kLat1D = {10.0, 20.0};
constexpr std::array<double, 2> kLon1D = {30.0, 40.0};
constexpr std::array<double, 2> kTime1D = {100.0, 101.0};

constexpr std::array<double, 4> kLat2D = {10.0, 11.0, 20.0, 21.0};
constexpr std::array<double, 4> kLon2D = {30.0, 31.0, 40.0, 41.0};

// Vertical profiles are stored top-to-bottom in EarthCARE L2 and inverted by
// read_profile_array() along the vertical axis so HARP stores ascending grids.
constexpr std::array<float, 4> kHeight2D = {2000.0f, 1000.0f, 2100.0f, 1100.0f};
constexpr std::array<float, 2> kGeoid1D = {10.0f, 20.0f};
constexpr std::array<float, 2> kElevation1D = {50.0f, 60.0f};

CodaMemNode* BuildEarthCareSkeleton(CodaMemProduct& prod, uint32_t orbit_num) {
  CodaMemNode* header_data = prod.MakeRecord();
  CodaMemNode* vph = prod.MakeRecord();
  CodaMemNode* mph = prod.MakeRecord();
  const std::array<uint32_t, 1> orbit_vals = {orbit_num};
  if (header_data == nullptr || vph == nullptr || mph == nullptr ||
      !prod.AddField(mph, "orbitNumber",
                     prod.MakeUint32Array(kOrbitDims, orbit_vals)) ||
      !prod.AddField(vph, "MainProductHeader", mph) ||
      !prod.AddField(header_data, "VariableProductHeader", vph) ||
      !prod.AddField(prod.root(), "HeaderData", header_data)) {
    return nullptr;
  }
  CodaMemNode* science_data = prod.MakeRecord();
  if (science_data == nullptr ||
      !prod.AddField(prod.root(), "ScienceData", science_data)) {
    return nullptr;
  }
  return science_data;
}

void AddCommon1D(CodaMemProduct& prod, CodaMemNode* sci) {
  ASSERT_TRUE(
      prod.AddField(sci, "latitude", prod.MakeDoubleArray(kDims1D, kLat1D)));
  ASSERT_TRUE(
      prod.AddField(sci, "longitude", prod.MakeDoubleArray(kDims1D, kLon1D)));
  ASSERT_TRUE(
      prod.AddField(sci, "time", prod.MakeDoubleArray(kDims1D, kTime1D)));
}

void AddCommon2D(CodaMemProduct& prod, CodaMemNode* sci) {
  ASSERT_TRUE(
      prod.AddField(sci, "latitude", prod.MakeDoubleArray(kDims2D, kLat2D)));
  ASSERT_TRUE(
      prod.AddField(sci, "longitude", prod.MakeDoubleArray(kDims2D, kLon2D)));
  ASSERT_TRUE(
      prod.AddField(sci, "time", prod.MakeDoubleArray(kDims1D, kTime1D)));
}

TEST(HarpIngestEarthcareL2Test, EcaAcTc2bAllResolutions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ECA_AC__TC__2B");
  ASSERT_NE(nullptr, mod);

  struct ResolutionCase {
    const char* options;
    std::string_view field_name;
  };
  constexpr ResolutionCase kCases[] = {
      {.options = nullptr, .field_name = "synergetic_target_classification"},
      {.options = "resolution=medium",
       .field_name = "synergetic_target_classification_medium_resolution"},
      {.options = "resolution=low",
       .field_name = "synergetic_target_classification_low_resolution"},
  };

  for (const auto& tc : kCases) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sci = BuildEarthCareSkeleton(prod, 42);
    ASSERT_NE(nullptr, sci);
    AddCommon1D(prod, sci);
    ASSERT_TRUE(
        prod.AddField(sci, "height", prod.MakeFloatArray(kDims2D, kHeight2D)));
    ASSERT_TRUE(prod.AddField(sci, "geoid_offset",
                              prod.MakeFloatArray(kDims1D, kGeoid1D)));
    ASSERT_TRUE(prod.AddField(sci, "elevation",
                              prod.MakeFloatArray(kDims1D, kElevation1D)));
    constexpr std::array<int8_t, 4> kScene = {1, 2, 3, 4};
    ASSERT_TRUE(
        prod.AddField(sci, tc.field_name, prod.MakeInt8Array(kDims2D, kScene)));

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), tc.options);
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(2, res.dimensions[harp_dimension_vertical]);
    EXPECT_THAT(res.variables["orbit_index"].int32_values, ElementsAre(42));
    // Vertical grid is inverted: [1000, 2000] - 10, [1100, 2100] - 20.
    EXPECT_THAT(res.variables["altitude"].float_values,
                ElementsAre(990.0f, 1990.0f, 1080.0f, 2080.0f));
    EXPECT_THAT(res.variables["surface_altitude"].float_values,
                ElementsAre(40.0f, 40.0f));
    EXPECT_THAT(res.variables["scene_type"].int8_values,
                ElementsAre(2, 1, 4, 3));
  }
}

TEST(HarpIngestEarthcareL2Test, EcaAcmCap2bSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ECA_ACM_CAP_2B");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  CodaMemNode* sci = BuildEarthCareSkeleton(prod, 100);
  ASSERT_NE(nullptr, sci);
  AddCommon1D(prod, sci);
  ASSERT_TRUE(
      prod.AddField(sci, "height", prod.MakeFloatArray(kDims2D, kHeight2D)));
  ASSERT_TRUE(prod.AddField(sci, "geoid_offset",
                            prod.MakeFloatArray(kDims1D, kGeoid1D)));

  constexpr std::array<float, 4> kProf = {1.0f, 2.0f, 3.0f, 4.0f};
  constexpr std::array<float, 2> kCol = {0.5f, 0.75f};
  constexpr std::array<int8_t, 2> kQual = {0, 1};
  for (std::string_view name :
       {"liquid_water_content", "liquid_extinction", "liquid_effective_radius",
        "ice_water_content", "ice_effective_radius", "ice_mass_flux",
        "rain_rate", "rain_water_content", "aerosol_number_concentration",
        "aerosol_extinction", "aerosol_mass_content"}) {
    ASSERT_TRUE(prod.AddField(sci, name, prod.MakeFloatArray(kDims2D, kProf)));
  }
  for (std::string_view name : {"ice_water_path", "aerosol_optical_depth"}) {
    ASSERT_TRUE(prod.AddField(sci, name, prod.MakeFloatArray(kDims1D, kCol)));
  }
  ASSERT_TRUE(
      prod.AddField(sci, "quality_status", prod.MakeInt8Array(kDims1D, kQual)));

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_THAT(res.variables["liquid_water_density"].float_values,
              ElementsAre(2.0f, 1.0f, 4.0f, 3.0f));
  EXPECT_THAT(res.variables["ice_water_column_density"].float_values,
              ElementsAre(0.5f, 0.75f));
  EXPECT_THAT(res.variables["aerosol_optical_depth"].float_values,
              ElementsAre(0.5f, 0.75f));
  EXPECT_THAT(res.variables["validity"].int8_values, ElementsAre(0, 1));
}

TEST(HarpIngestEarthcareL2Test, EcaAmAcd2bAngstromVariants) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ECA_AM__ACD_2B");
  ASSERT_NE(nullptr, mod);

  for (const char* opts :
       {static_cast<const char*>(nullptr), "angstrom=670/865"}) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sci = BuildEarthCareSkeleton(prod, 7);
    ASSERT_NE(nullptr, sci);
    AddCommon2D(prod, sci);
    constexpr std::array<long, 1> kSpecDim = {3};
    constexpr std::array<int32_t, 3> kSpecVals = {355, 670, 865};
    ASSERT_TRUE(prod.AddField(sci, "aerosol_optical_thickness_dimension",
                              prod.MakeInt32Array(kSpecDim, kSpecVals)));
    constexpr std::array<float, 12> kAot = {1, 2, 3, 4,  5,  6,
                                            7, 8, 9, 10, 11, 12};
    constexpr std::array<float, 12> kAotErr = {
        0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 1.1f, 1.2f};
    ASSERT_TRUE(prod.AddField(sci, "aerosol_optical_thickness_spectral",
                              prod.MakeFloatArray(kDims2D3, kAot)));
    ASSERT_TRUE(prod.AddField(sci, "aerosol_optical_thickness_spectral_error",
                              prod.MakeFloatArray(kDims2D3, kAotErr)));
    constexpr std::array<float, 8> kAng = {1.1f, 2.1f, 1.2f, 2.2f,
                                           1.3f, 2.3f, 1.4f, 2.4f};
    constexpr std::array<float, 8> kAngErr = {0.1f, 0.2f, 0.1f, 0.2f,
                                              0.1f, 0.2f, 0.1f, 0.2f};
    ASSERT_TRUE(prod.AddField(sci, "aerosol_angstrom_exponent",
                              prod.MakeFloatArray(kDims2D2, kAng)));
    ASSERT_TRUE(prod.AddField(sci, "aerosol_angstrom_exponent_error",
                              prod.MakeFloatArray(kDims2D2, kAngErr)));
    constexpr std::array<int8_t, 4> kI8 = {1, 2, 3, 4};
    ASSERT_TRUE(prod.AddField(sci, "aerosol_dominant_type_ATLID",
                              prod.MakeInt8Array(kDims2D, kI8)));
    ASSERT_TRUE(
        prod.AddField(sci, "quality_status", prod.MakeInt8Array(kDims2D, kI8)));

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), opts);
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(4, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(3, res.dimensions[harp_dimension_spectral]);
    EXPECT_THAT(res.variables["datetime"].double_values,
                ElementsAre(100.0, 100.0, 101.0, 101.0));
    EXPECT_THAT(res.variables["wavelength"].float_values,
                ElementsAre(355.0f, 670.0f, 865.0f));
    EXPECT_EQ(4u, res.variables["latitude_bounds"].double_values.size() / 4);
    EXPECT_EQ(4u, res.variables["longitude_bounds"].double_values.size() / 4);
    if (opts == nullptr) {
      EXPECT_THAT(res.variables["angstrom_exponent"].float_values,
                  ElementsAre(1.1f, 1.2f, 1.3f, 1.4f));
      EXPECT_THAT(res.variables["angstrom_exponent_uncertainty"].float_values,
                  ElementsAre(0.1f, 0.1f, 0.1f, 0.1f));
    } else {
      EXPECT_THAT(res.variables["angstrom_exponent"].float_values,
                  ElementsAre(2.1f, 2.2f, 2.3f, 2.4f));
      EXPECT_THAT(res.variables["angstrom_exponent_uncertainty"].float_values,
                  ElementsAre(0.2f, 0.2f, 0.2f, 0.2f));
    }
  }
}

TEST(HarpIngestEarthcareL2Test, EcaAmCth2bMsiAndAtlidSource) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ECA_AM__CTH_2B");
  ASSERT_NE(nullptr, mod);

  for (const char* opts : {static_cast<const char*>(nullptr), "source=atlid"}) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sci = BuildEarthCareSkeleton(prod, 8);
    ASSERT_NE(nullptr, sci);
    AddCommon2D(prod, sci);
    constexpr std::array<float, 4> kCf = {0.2f, 0.4f, 0.6f, 0.8f};
    constexpr std::array<float, 4> kCth = {5000.0f, 5100.0f, 6000.0f, 6100.0f};
    constexpr std::array<float, 4> kDiff = {100.0f, 200.0f, 300.0f, 400.0f};
    constexpr std::array<int8_t, 4> kQual = {0, 0, 1, 1};
    ASSERT_TRUE(prod.AddField(sci, "cloud_fraction",
                              prod.MakeFloatArray(kDims2D, kCf)));
    ASSERT_TRUE(prod.AddField(sci, "cloud_top_height_MSI",
                              prod.MakeFloatArray(kDims2D, kCth)));
    ASSERT_TRUE(prod.AddField(sci, "geoid_offset",
                              prod.MakeFloatArray(kDims1D, kGeoid1D)));
    ASSERT_TRUE(prod.AddField(sci, "cloud_top_height_difference_ATLID_MSI",
                              prod.MakeFloatArray(kDims2D, kDiff)));
    ASSERT_TRUE(prod.AddField(sci, "quality_status",
                              prod.MakeInt8Array(kDims2D, kQual)));

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), opts);
    ASSERT_EQ(HARP_SUCCESS, res.status);
    if (opts == nullptr) {
      EXPECT_THAT(res.variables["cloud_top_height"].float_values,
                  ElementsAre(4990.0f, 5090.0f, 5980.0f, 6080.0f));
    } else {
      EXPECT_THAT(res.variables["cloud_top_height"].float_values,
                  ElementsAre(5090.0f, 5290.0f, 6280.0f, 6480.0f));
    }
  }
}

TEST(HarpIngestEarthcareL2Test, EcaAtlAer2aAndAtlAld2aSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  // 1. ECA_ATL_AER_2A.
  {
    const harp_ingestion_module* mod = FindIngestionModule("ECA_ATL_AER_2A");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sci = BuildEarthCareSkeleton(prod, 9);
    ASSERT_NE(nullptr, sci);
    AddCommon1D(prod, sci);
    ASSERT_TRUE(
        prod.AddField(sci, "height", prod.MakeFloatArray(kDims2D, kHeight2D)));
    ASSERT_TRUE(prod.AddField(sci, "geoid_offset",
                              prod.MakeFloatArray(kDims1D, kGeoid1D)));
    ASSERT_TRUE(prod.AddField(sci, "elevation",
                              prod.MakeFloatArray(kDims1D, kElevation1D)));
    constexpr std::array<float, 4> kProf = {1.0f, 2.0f, 3.0f, 4.0f};
    constexpr std::array<float, 2> kTrop = {12000.0f, 12500.0f};
    constexpr std::array<int8_t, 4> kI8 = {1, 2, 3, 4};
    for (std::string_view name :
         {"particle_extinction_coefficient_355nm",
          "particle_extinction_coefficient_355nm_error",
          "particle_backscatter_coefficient_355nm",
          "particle_backscatter_coefficient_355nm_error",
          "particle_linear_depol_ratio_355nm",
          "particle_linear_depol_ratio_355nm_error", "lidar_ratio_355nm",
          "lidar_ratio_355nm_error"}) {
      ASSERT_TRUE(
          prod.AddField(sci, name, prod.MakeFloatArray(kDims2D, kProf)));
    }
    ASSERT_TRUE(prod.AddField(sci, "tropopause_height",
                              prod.MakeFloatArray(kDims1D, kTrop)));
    for (std::string_view name :
         {"aerosol_classification", "classification", "quality_status"}) {
      ASSERT_TRUE(prod.AddField(sci, name, prod.MakeInt8Array(kDims2D, kI8)));
    }

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["wavelength"].float_values, ElementsAre(355.0f));
    EXPECT_THAT(res.variables["tropopause_height"].float_values,
                ElementsAre(12000.0f, 12500.0f));
    EXPECT_THAT(res.variables["particle_extinction_coefficient"].float_values,
                ElementsAre(2.0f, 1.0f, 4.0f, 3.0f));
  }

  // 2. ECA_ATL_ALD_2A (uses max_layers for num_vertical).
  {
    const harp_ingestion_module* mod = FindIngestionModule("ECA_ATL_ALD_2A");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sci = BuildEarthCareSkeleton(prod, 10);
    ASSERT_NE(nullptr, sci);
    AddCommon1D(prod, sci);
    constexpr std::array<int32_t, 2> kLayers = {1, 2};
    ASSERT_TRUE(prod.AddField(sci, "max_layers",
                              prod.MakeInt32Array(kDims1D, kLayers)));
    ASSERT_TRUE(prod.AddField(sci, "geoid_offset",
                              prod.MakeFloatArray(kDims1D, kGeoid1D)));
    constexpr std::array<float, 4> kBase = {100.0f, 500.0f, 200.0f, 600.0f};
    constexpr std::array<float, 4> kTop = {300.0f, 700.0f, 400.0f, 800.0f};
    ASSERT_TRUE(prod.AddField(sci, "aerosol_layer_base",
                              prod.MakeFloatArray(kDims2D, kBase)));
    ASSERT_TRUE(prod.AddField(sci, "aerosol_layer_top",
                              prod.MakeFloatArray(kDims2D, kTop)));
    constexpr std::array<float, 4> kProf = {1.0f, 2.0f, 3.0f, 4.0f};
    for (std::string_view name :
         {"aerosol_layer_optical_thickness_355nm",
          "aerosol_layer_optical_thickness_355nm_error",
          "aerosol_layer_mean_extinction_355nm",
          "aerosol_layer_mean_extinction_355nm_error",
          "aerosol_layer_mean_backscatter_355nm",
          "aerosol_layer_mean_backscatter_355nm_error",
          "aerosol_layer_mean_lidar_ratio_355nm",
          "aerosol_layer_mean_lidar_ratio_355nm_error",
          "aerosol_layer_mean_depolarisation_355nm",
          "aerosol_layer_mean_depolarisation_355nm_error"}) {
      ASSERT_TRUE(
          prod.AddField(sci, name, prod.MakeFloatArray(kDims2D, kProf)));
    }
    constexpr std::array<int8_t, 2> kQual = {0, 1};
    ASSERT_TRUE(prod.AddField(sci, "quality_status",
                              prod.MakeInt8Array(kDims1D, kQual)));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    // Inverted vertical axis, then interleaved {base, top} - geoid_offset.
    EXPECT_THAT(res.variables["altitude_bounds"].float_values,
                ElementsAre(490.0f, 690.0f, 90.0f, 290.0f, 580.0f, 780.0f,
                            180.0f, 380.0f));
    EXPECT_THAT(res.variables["wavelength"].float_values, ElementsAre(355.0f));
  }
}

TEST(HarpIngestEarthcareL2Test, EcaAtlCth2aEbd2aAndIce2aSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  // 1. ECA_ATL_CTH_2A.
  {
    const harp_ingestion_module* mod = FindIngestionModule("ECA_ATL_CTH_2A");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sci = BuildEarthCareSkeleton(prod, 11);
    ASSERT_NE(nullptr, sci);
    AddCommon1D(prod, sci);
    constexpr std::array<float, 2> kCth = {8000.0f, 9000.0f};
    constexpr std::array<float, 2> kConf = {0.9f, 0.8f};
    constexpr std::array<int8_t, 2> kI8 = {1, 2};
    ASSERT_TRUE(prod.AddField(sci, "ATLID_cloud_top_height",
                              prod.MakeFloatArray(kDims1D, kCth)));
    ASSERT_TRUE(prod.AddField(sci, "ATLID_cloud_top_height_confidence",
                              prod.MakeFloatArray(kDims1D, kConf)));
    ASSERT_TRUE(prod.AddField(sci, "simplified_uppermost_cloud_classification",
                              prod.MakeInt8Array(kDims1D, kI8)));
    ASSERT_TRUE(
        prod.AddField(sci, "quality_status", prod.MakeInt8Array(kDims1D, kI8)));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["cloud_top_height"].float_values,
                ElementsAre(8000.0f, 9000.0f));
    EXPECT_THAT(res.variables["cloud_type"].int8_values, ElementsAre(1, 2));
  }

  // 2. ECA_ATL_EBD_2A across all 3 resolution options.
  {
    const harp_ingestion_module* mod = FindIngestionModule("ECA_ATL_EBD_2A");
    ASSERT_NE(nullptr, mod);
    struct EbdCase {
      const char* options;
      std::string_view suffix;
    };
    constexpr EbdCase kCases[] = {
        {.options = nullptr, .suffix = ""},
        {.options = "resolution=medium", .suffix = "_medium_resolution"},
        {.options = "resolution=low", .suffix = "_low_resolution"},
    };
    for (const auto& tc : kCases) {
      CodaMemProduct prod;
      ASSERT_TRUE(prod.ok());
      CodaMemNode* sci = BuildEarthCareSkeleton(prod, 12);
      ASSERT_NE(nullptr, sci);
      AddCommon1D(prod, sci);
      ASSERT_TRUE(prod.AddField(sci, "height",
                                prod.MakeFloatArray(kDims2D, kHeight2D)));
      ASSERT_TRUE(prod.AddField(sci, "geoid_offset",
                                prod.MakeFloatArray(kDims1D, kGeoid1D)));
      ASSERT_TRUE(prod.AddField(sci, "elevation",
                                prod.MakeFloatArray(kDims1D, kElevation1D)));
      constexpr std::array<float, 2> kF1 = {1.5f, 2.5f};
      ASSERT_TRUE(prod.AddField(sci, "viewing_elevation_angle",
                                prod.MakeFloatArray(kDims1D, kF1)));
      ASSERT_TRUE(prod.AddField(sci, "tropopause_height",
                                prod.MakeFloatArray(kDims1D, kF1)));
      constexpr std::array<float, 4> kProf = {1.0f, 2.0f, 3.0f, 4.0f};
      for (std::string_view base :
           {"particle_extinction_coefficient_355nm",
            "particle_backscatter_coefficient_355nm", "lidar_ratio_355nm",
            "particle_linear_depol_ratio_355nm",
            "particle_optical_depth_355nm"}) {
        const std::string field = std::string(base) + std::string(tc.suffix);
        const std::string err_field = field + "_error";
        ASSERT_TRUE(
            prod.AddField(sci, field, prod.MakeFloatArray(kDims2D, kProf)));
        ASSERT_TRUE(
            prod.AddField(sci, err_field, prod.MakeFloatArray(kDims2D, kProf)));
      }
      ASSERT_TRUE(prod.AddField(sci, "particle_effective_area_radius",
                                prod.MakeFloatArray(kDims2D, kProf)));
      ASSERT_TRUE(prod.AddField(sci, "particle_effective_area_radius_error",
                                prod.MakeFloatArray(kDims2D, kProf)));
      constexpr std::array<int8_t, 4> kI8 = {1, 2, 3, 4};
      ASSERT_TRUE(prod.AddField(sci, "simple_classification",
                                prod.MakeInt8Array(kDims2D, kI8)));
      ASSERT_TRUE(prod.AddField(sci, "quality_status",
                                prod.MakeInt8Array(kDims2D, kI8)));

      IngestExerciseResult res =
          ExerciseIngestionModule(mod, prod.product(), tc.options);
      ASSERT_EQ(HARP_SUCCESS, res.status);
      EXPECT_THAT(res.variables["extinction_coefficient"].float_values,
                  ElementsAre(2.0f, 1.0f, 4.0f, 3.0f));
      EXPECT_THAT(res.variables["particle_effective_radius"].float_values,
                  ElementsAre(2.0f, 1.0f, 4.0f, 3.0f));
    }
  }

  // 3. ECA_ATL_ICE_2A.
  {
    const harp_ingestion_module* mod = FindIngestionModule("ECA_ATL_ICE_2A");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sci = BuildEarthCareSkeleton(prod, 13);
    ASSERT_NE(nullptr, sci);
    AddCommon1D(prod, sci);
    ASSERT_TRUE(
        prod.AddField(sci, "height", prod.MakeFloatArray(kDims2D, kHeight2D)));
    ASSERT_TRUE(prod.AddField(sci, "geoid_offset",
                              prod.MakeFloatArray(kDims1D, kGeoid1D)));
    ASSERT_TRUE(prod.AddField(sci, "elevation",
                              prod.MakeFloatArray(kDims1D, kElevation1D)));
    constexpr std::array<float, 2> kF1 = {1.5f, 2.5f};
    ASSERT_TRUE(prod.AddField(sci, "viewing_elevation_angle",
                              prod.MakeFloatArray(kDims1D, kF1)));
    ASSERT_TRUE(prod.AddField(sci, "tropopause_height",
                              prod.MakeFloatArray(kDims1D, kF1)));
    constexpr std::array<float, 4> kProf = {1.0f, 2.0f, 3.0f, 4.0f};
    for (std::string_view name :
         {"ice_water_content", "ice_water_content_error",
          "ice_effective_radius", "ice_effective_radius_error"}) {
      ASSERT_TRUE(
          prod.AddField(sci, name, prod.MakeFloatArray(kDims2D, kProf)));
    }
    constexpr std::array<int8_t, 4> kI8 = {0, 1, 2, 3};
    ASSERT_TRUE(
        prod.AddField(sci, "quality_status", prod.MakeInt8Array(kDims2D, kI8)));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["ice_water_density"].float_values,
                ElementsAre(2.0f, 1.0f, 4.0f, 3.0f));
    EXPECT_THAT(res.variables["ice_particle_effective_radius"].float_values,
                ElementsAre(2.0f, 1.0f, 4.0f, 3.0f));
  }
}

TEST(HarpIngestEarthcareL2Test, EcaBmRad2bOptionsAndResolutions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ECA_BM__RAD_2B");
  ASSERT_NE(nullptr, mod);

  struct BmRadCase {
    const char* options;
    std::string_view dataset_name;
    int dir_idx;
    std::string_view rad_field;
    std::string_view rad_err_field;
    std::string_view rad_qual_field;
    bool expect_unfiltered;
  };
  // Note: upstream ingestion_init() lacks an `else` before `resolution=full`,
  // so `resolution=small` falls through to `Assessment` (`bbr_resolution = 3`);
  // `edge_coordinate=one_weight` sets `bbr_resolution = 2` (`Full`) and leaves
  // `bbr_edge_coordinate = 0` so `zero_weight_edge_coordinate_nadir` is read.
  constexpr BmRadCase kCases[] = {
      {.options = nullptr,
       .dataset_name = "Standard",
       .dir_idx = 0,
       .rad_field = "solar_radiance",
       .rad_err_field = "solar_radiance_error",
       .rad_qual_field = "solar_radiance_quality_status",
       .expect_unfiltered = true},
      {.options = "direction=fore;radiance=SW_MSI;resolution=full",
       .dataset_name = "Full",
       .dir_idx = 1,
       .rad_field = "solar_radiance_MSI",
       .rad_err_field = "solar_radiance_MSI_error",
       .rad_qual_field = "solar_radiance_MSI_quality_status",
       .expect_unfiltered = true},
      {.options = "direction=aft;radiance=SW_filtered;resolution=assessment",
       .dataset_name = "Assessment",
       .dir_idx = 2,
       .rad_field = "shortwave_filtered_radiance",
       .rad_err_field = "",
       .rad_qual_field = "",
       .expect_unfiltered = false},
      {.options = "radiance=LW;resolution=small",
       .dataset_name = "Assessment",
       .dir_idx = 0,
       .rad_field = "thermal_radiance",
       .rad_err_field = "thermal_radiance_error",
       .rad_qual_field = "thermal_radiance_quality_status",
       .expect_unfiltered = true},
      {.options = "radiance=LW_filtered;edge_coordinate=one_weight",
       .dataset_name = "Full",
       .dir_idx = 0,
       .rad_field = "longwave_filtered_radiance",
       .rad_err_field = "",
       .rad_qual_field = "",
       .expect_unfiltered = false},
  };

  for (const auto& tc : kCases) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sci = BuildEarthCareSkeleton(prod, 55);
    ASSERT_NE(nullptr, sci);
    CodaMemNode* ds = prod.MakeRecord();
    ASSERT_NE(nullptr, ds);
    ASSERT_TRUE(prod.AddField(sci, tc.dataset_name, ds));

    constexpr std::array<double, 6> kDirD = {10, 11, 12, 20, 21, 22};
    constexpr std::array<int8_t, 6> kDirI8 = {0, 1, 2, 3, 4, 5};
    for (std::string_view name :
         {"latitude", "longitude", "time", "surface_elevation",
          "solar_azimuth_angle", "solar_zenith_angle", "viewing_azimuth_angle",
          "viewing_zenith_angle"}) {
      ASSERT_TRUE(
          prod.AddField(ds, name, prod.MakeDoubleArray(kDimsBbrDir, kDirD)));
    }
    ASSERT_TRUE(prod.AddField(ds, tc.rad_field,
                              prod.MakeDoubleArray(kDimsBbrDir, kDirD)));
    if (tc.expect_unfiltered) {
      ASSERT_TRUE(prod.AddField(ds, tc.rad_err_field,
                                prod.MakeDoubleArray(kDimsBbrDir, kDirD)));
      ASSERT_TRUE(prod.AddField(ds, tc.rad_qual_field,
                                prod.MakeInt8Array(kDimsBbrDir, kDirI8)));
    }
    ASSERT_TRUE(prod.AddField(ds, "quality_status",
                              prod.MakeInt8Array(kDimsBbrDir, kDirI8)));
    // 16 elements = 2 * 4 * 2: first 8 are lat bounds (4 per pixel), next 8
    // are lon bounds (4 per pixel).
    constexpr std::array<double, 16> kEdges = {
        1,  2,  3,  4,  5,  6,  7,  8,   // lat: [corner0_t0, corner0_t1, ...]
        11, 12, 13, 14, 15, 16, 17, 18,  // lon
    };
    ASSERT_TRUE(prod.AddField(ds, "zero_weight_edge_coordinate_nadir",
                              prod.MakeDoubleArray(kDimsBbrBounds, kEdges)));

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), tc.options);
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["radiance"].double_values,
                ElementsAre(10.0 + tc.dir_idx, 20.0 + tc.dir_idx));
    EXPECT_EQ(tc.expect_unfiltered,
              res.variables["radiance_uncertainty"].included);
    EXPECT_EQ(tc.expect_unfiltered,
              res.variables["radiance_validity"].included);
    EXPECT_THAT(res.variables["latitude_bounds"].double_values,
                ElementsAre(1, 3, 5, 7, 2, 4, 6, 8));
    EXPECT_THAT(res.variables["longitude_bounds"].double_values,
                ElementsAre(11, 13, 15, 17, 12, 14, 16, 18));
  }
}

TEST(HarpIngestEarthcareL2Test, EcaBmaFlx2bOptionsAndResolutions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ECA_BMA_FLX_2B");
  ASSERT_NE(nullptr, mod);

  struct FlxCase {
    const char* options;
    std::string_view dataset_name;
    bool combined;
    bool thermal;
    int dir_idx;
  };
  constexpr FlxCase kCases[] = {
      {.options = nullptr,
       .dataset_name = "StandardResolution",
       .combined = true,
       .thermal = false,
       .dir_idx = 0},
      {.options = "direction=nadir;irradiance=thermal;resolution=full",
       .dataset_name = "FullResolution",
       .combined = false,
       .thermal = true,
       .dir_idx = 0},
      {.options = "direction=fore;resolution=assessment",
       .dataset_name = "AssessmentResolution",
       .combined = false,
       .thermal = false,
       .dir_idx = 1},
      {.options = "direction=aft;irradiance=thermal;resolution=small",
       .dataset_name = "AssessmentResolution",
       .combined = false,
       .thermal = true,
       .dir_idx = 2},
      {.options = "irradiance=thermal",
       .dataset_name = "StandardResolution",
       .combined = true,
       .thermal = true,
       .dir_idx = 0},
  };

  for (const auto& tc : kCases) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sci = BuildEarthCareSkeleton(prod, 66);
    ASSERT_NE(nullptr, sci);
    CodaMemNode* ds = prod.MakeRecord();
    ASSERT_NE(nullptr, ds);
    ASSERT_TRUE(prod.AddField(sci, tc.dataset_name, ds));

    AddCommon1D(prod, ds);
    constexpr std::array<int8_t, 2> kQual1D = {1, 2};
    ASSERT_TRUE(prod.AddField(ds, "quality_status",
                              prod.MakeInt8Array(kDims1D, kQual1D)));

    if (tc.combined) {
      constexpr std::array<double, 2> kComb = {300.0, 400.0};
      const std::string prefix = tc.thermal ? "thermal" : "solar";
      ASSERT_TRUE(prod.AddField(ds, prefix + "_combined_top_of_atmosphere_flux",
                                prod.MakeDoubleArray(kDims1D, kComb)));
      ASSERT_TRUE(
          prod.AddField(ds, prefix + "_combined_top_of_atmosphere_flux_error",
                        prod.MakeDoubleArray(kDims1D, kComb)));
      ASSERT_TRUE(prod.AddField(
          ds, prefix + "_combined_top_of_atmosphere_flux_quality_status",
          prod.MakeInt8Array(kDims1D, kQual1D)));
    } else {
      constexpr std::array<double, 6> kDirD = {100, 101, 102, 200, 201, 202};
      constexpr std::array<int8_t, 6> kDirI8 = {0, 1, 2, 3, 4, 5};
      for (std::string_view name :
           {"solar_azimuth_angle", "solar_zenith_angle",
            "viewing_azimuth_angle", "viewing_zenith_angle"}) {
        ASSERT_TRUE(
            prod.AddField(ds, name, prod.MakeDoubleArray(kDimsBbrDir, kDirD)));
      }
      const std::string prefix = tc.thermal ? "thermal" : "solar";
      ASSERT_TRUE(prod.AddField(ds, prefix + "_top_of_atmosphere_flux",
                                prod.MakeDoubleArray(kDimsBbrDir, kDirD)));
      ASSERT_TRUE(prod.AddField(ds, prefix + "_top_of_atmosphere_flux_error",
                                prod.MakeDoubleArray(kDimsBbrDir, kDirD)));
      ASSERT_TRUE(
          prod.AddField(ds, prefix + "_top_of_atmosphere_flux_quality_status",
                        prod.MakeInt8Array(kDimsBbrDir, kDirI8)));
    }

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), tc.options);
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(!tc.combined, res.variables["solar_azimuth_angle"].included);
    if (tc.combined) {
      EXPECT_THAT(res.variables["irradiance"].double_values,
                  ElementsAre(300.0, 400.0));
    } else {
      EXPECT_THAT(res.variables["irradiance"].double_values,
                  ElementsAre(100.0 + tc.dir_idx, 200.0 + tc.dir_idx));
    }
  }
}

TEST(HarpIngestEarthcareL2Test, EcaCprCld2aSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ECA_CPR_CLD_2A");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  CodaMemNode* sci = BuildEarthCareSkeleton(prod, 77);
  ASSERT_NE(nullptr, sci);
  AddCommon1D(prod, sci);
  ASSERT_TRUE(
      prod.AddField(sci, "height", prod.MakeFloatArray(kDims2D, kHeight2D)));
  ASSERT_TRUE(prod.AddField(sci, "geoid_offset",
                            prod.MakeFloatArray(kDims1D, kGeoid1D)));
  ASSERT_TRUE(prod.AddField(sci, "surface_elevation",
                            prod.MakeFloatArray(kDims1D, kElevation1D)));
  constexpr std::array<int8_t, 2> kLand = {0, 1};
  ASSERT_TRUE(
      prod.AddField(sci, "land_flag", prod.MakeInt8Array(kDims1D, kLand)));
  constexpr std::array<float, 2> kPath = {2.0f, 3.0f};
  for (std::string_view name :
       {"ice_water_path", "ice_water_path_error", "rain_water_path",
        "rain_water_path_error", "liquid_water_path",
        "liquid_water_path_error"}) {
    ASSERT_TRUE(prod.AddField(sci, name, prod.MakeFloatArray(kDims1D, kPath)));
  }
  constexpr std::array<float, 4> kLwc = {10.0f, 20.0f, 30.0f, 40.0f};
  constexpr std::array<float, 4> kRelErr = {0.1f, 0.2f, 0.3f, 0.4f};
  ASSERT_TRUE(prod.AddField(sci, "liquid_water_content",
                            prod.MakeFloatArray(kDims2D, kLwc)));
  ASSERT_TRUE(prod.AddField(sci, "liquid_water_content_relative_error",
                            prod.MakeFloatArray(kDims2D, kRelErr)));
  ASSERT_TRUE(prod.AddField(sci, "liquid_effective_radius",
                            prod.MakeFloatArray(kDims2D, kLwc)));
  ASSERT_TRUE(prod.AddField(sci, "liquid_effective_radius_relative_error",
                            prod.MakeFloatArray(kDims2D, kRelErr)));
  constexpr std::array<int8_t, 4> kRet = {0, 1, 2, 3};
  ASSERT_TRUE(prod.AddField(sci, "retrieval_status",
                            prod.MakeInt8Array(kDims2D, kRet)));

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  // Inverted [20*0.2, 10*0.1, 40*0.4, 30*0.3] = [4, 1, 16, 9].
  EXPECT_THAT(res.variables["liquid_water_density_uncertainty"].float_values,
              ElementsAre(FloatNear(4.0f, 1e-5f), FloatNear(1.0f, 1e-5f),
                          FloatNear(16.0f, 1e-5f), FloatNear(9.0f, 1e-5f)));
  EXPECT_THAT(res.variables["liquid_particle_effective_radius_uncertainty"]
                  .float_values,
              ElementsAre(FloatNear(4.0f, 1e-5f), FloatNear(1.0f, 1e-5f),
                          FloatNear(16.0f, 1e-5f), FloatNear(9.0f, 1e-5f)));
  EXPECT_THAT(res.variables["validity"].int8_values, ElementsAre(1, 0, 3, 2));
}

TEST(HarpIngestEarthcareL2Test, EcaMsiProductsSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  // 1. ECA_MSI_AOT_2A (default and aot=865;angstrom=670/865).
  {
    const harp_ingestion_module* mod = FindIngestionModule("ECA_MSI_AOT_2A");
    ASSERT_NE(nullptr, mod);
    for (const char* opts :
         {static_cast<const char*>(nullptr), "aot=865;angstrom=670/865"}) {
      CodaMemProduct prod;
      ASSERT_TRUE(prod.ok());
      CodaMemNode* sci = BuildEarthCareSkeleton(prod, 81);
      ASSERT_NE(nullptr, sci);
      AddCommon2D(prod, sci);
      constexpr std::array<float, 4> kF670 = {0.1f, 0.2f, 0.3f, 0.4f};
      constexpr std::array<float, 4> kF865 = {0.5f, 0.6f, 0.7f, 0.8f};
      constexpr std::array<int8_t, 4> kQual = {0, 1, 0, 1};
      ASSERT_TRUE(prod.AddField(sci, "aerosol_optical_thickness_670nm",
                                prod.MakeFloatArray(kDims2D, kF670)));
      ASSERT_TRUE(prod.AddField(sci, "aerosol_optical_thickness_670nm_error",
                                prod.MakeFloatArray(kDims2D, kF670)));
      ASSERT_TRUE(prod.AddField(sci, "aerosol_optical_thickness_865nm",
                                prod.MakeFloatArray(kDims2D, kF865)));
      ASSERT_TRUE(prod.AddField(sci, "aerosol_optical_thickness_865nm_error",
                                prod.MakeFloatArray(kDims2D, kF865)));
      ASSERT_TRUE(prod.AddField(sci, "angstrom_parameter_355nm_670nm",
                                prod.MakeFloatArray(kDims2D, kF670)));
      ASSERT_TRUE(prod.AddField(sci, "angstrom_parameter_670nm_865nm",
                                prod.MakeFloatArray(kDims2D, kF865)));
      ASSERT_TRUE(prod.AddField(sci, "surface_reflectance_670nm",
                                prod.MakeFloatArray(kDims2D, kF670)));
      ASSERT_TRUE(prod.AddField(sci, "surface_reflectance_670nm_error",
                                prod.MakeFloatArray(kDims2D, kF670)));
      ASSERT_TRUE(prod.AddField(sci, "quality_status",
                                prod.MakeInt8Array(kDims2D, kQual)));

      IngestExerciseResult res =
          ExerciseIngestionModule(mod, prod.product(), opts);
      ASSERT_EQ(HARP_SUCCESS, res.status);
      EXPECT_EQ(opts == nullptr, res.variables["surface_reflectance"].included);
      EXPECT_EQ(opts == nullptr,
                res.variables["surface_reflectance_uncertainty"].included);
      if (opts == nullptr) {
        EXPECT_THAT(res.variables["aerosol_optical_depth"].float_values,
                    ElementsAre(0.1f, 0.2f, 0.3f, 0.4f));
      } else {
        EXPECT_THAT(res.variables["aerosol_optical_depth"].float_values,
                    ElementsAre(0.5f, 0.6f, 0.7f, 0.8f));
      }
    }
  }

  // 2. ECA_MSI_CM__2A (covers cloud_phase > 0 decrement and <= 0 passthrough).
  {
    const harp_ingestion_module* mod = FindIngestionModule("ECA_MSI_CM__2A");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sci = BuildEarthCareSkeleton(prod, 82);
    ASSERT_NE(nullptr, sci);
    AddCommon2D(prod, sci);
    constexpr std::array<int8_t, 4> kPhase = {0, 1, 2, 4};
    constexpr std::array<int8_t, 4> kI8 = {0, 1, 2, 3};
    ASSERT_TRUE(
        prod.AddField(sci, "cloud_phase", prod.MakeInt8Array(kDims2D, kPhase)));
    for (std::string_view name :
         {"cloud_type", "cloud_type_quality_status",
          "cloud_phase_quality_status", "cloud_mask",
          "cloud_mask_quality_status", "quality_status"}) {
      ASSERT_TRUE(prod.AddField(sci, name, prod.MakeInt8Array(kDims2D, kI8)));
    }

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["cloud_phase_type"].int8_values,
                ElementsAre(0, 0, 1, 3));
  }

  // 3. ECA_MSI_COP_2A.
  {
    const harp_ingestion_module* mod = FindIngestionModule("ECA_MSI_COP_2A");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sci = BuildEarthCareSkeleton(prod, 83);
    ASSERT_NE(nullptr, sci);
    AddCommon2D(prod, sci);
    ASSERT_TRUE(prod.AddField(sci, "geoid_offset",
                              prod.MakeFloatArray(kDims1D, kGeoid1D)));
    constexpr std::array<float, 4> kVals = {100.0f, 200.0f, 300.0f, 400.0f};
    for (std::string_view name :
         {"cloud_effective_radius", "cloud_effective_radius_error",
          "cloud_optical_thickness", "cloud_optical_thickness_error",
          "cloud_top_height", "cloud_top_height_error", "cloud_top_pressure",
          "cloud_top_pressure_error", "cloud_top_temperature",
          "cloud_top_temperature_error", "cloud_water_path",
          "cloud_water_path_error"}) {
      ASSERT_TRUE(
          prod.AddField(sci, name, prod.MakeFloatArray(kDims2D, kVals)));
    }
    constexpr std::array<int8_t, 4> kQual = {0, 1, 0, 1};
    ASSERT_TRUE(prod.AddField(sci, "quality_status",
                              prod.MakeInt8Array(kDims2D, kQual)));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["cloud_top_height"].float_values,
                ElementsAre(90.0f, 190.0f, 280.0f, 380.0f));
    EXPECT_THAT(res.variables["cloud_top_pressure"].float_values,
                ElementsAre(100.0f, 200.0f, 300.0f, 400.0f));
  }
}

TEST(HarpIngestEarthcareL2Test, ErrorPathsInitAndReadFailures) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  const harp_ingestion_module* ac_mod = FindIngestionModule("ECA_AC__TC__2B");
  const harp_ingestion_module* ald_mod = FindIngestionModule("ECA_ATL_ALD_2A");
  const harp_ingestion_module* acd_mod = FindIngestionModule("ECA_AM__ACD_2B");
  const harp_ingestion_module* cth_mod = FindIngestionModule("ECA_AM__CTH_2B");
  const harp_ingestion_module* cpr_mod = FindIngestionModule("ECA_CPR_CLD_2A");
  const harp_ingestion_module* rad_mod = FindIngestionModule("ECA_BM__RAD_2B");
  const harp_ingestion_module* flx_mod = FindIngestionModule("ECA_BMA_FLX_2B");
  ASSERT_NE(nullptr, ac_mod);
  ASSERT_NE(nullptr, ald_mod);
  ASSERT_NE(nullptr, acd_mod);
  ASSERT_NE(nullptr, cth_mod);
  ASSERT_NE(nullptr, cpr_mod);
  ASSERT_NE(nullptr, rad_mod);
  ASSERT_NE(nullptr, flx_mod);

  // 1. Missing ScienceData.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(ac_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. Missing latitude inside ScienceData.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "ScienceData", prod.MakeRecord()));
    IngestExerciseResult res = ExerciseIngestionModule(ac_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. Invalid ingestion option rejected by module validation.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res =
        ExerciseIngestionModule(ac_mod, prod.product(), "resolution=invalid");
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INVALID_INGESTION_OPTION_VALUE, res.failed_errno);
  }

  // 4. Wrong element counts and types in read_array / read_orbit_index /
  // derived profile helpers.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sci = prod.MakeRecord();
    ASSERT_NE(nullptr, sci);
    ASSERT_TRUE(prod.AddField(prod.root(), "ScienceData", sci));
    ASSERT_TRUE(
        prod.AddField(sci, "latitude", prod.MakeDoubleArray(kDims1D, kLat1D)));
    constexpr std::array<long, 1> kBadDim = {1};
    constexpr std::array<double, 1> kBadDouble = {1.0};
    ASSERT_TRUE(prod.AddField(sci, "longitude",
                              prod.MakeDoubleArray(kBadDim, kBadDouble)));
    constexpr std::array<std::string_view, 2> kBadStr = {"a", "b"};
    ASSERT_TRUE(
        prod.AddField(sci, "time", prod.MakeStringArray(kDims1D, kBadStr)));
    ASSERT_TRUE(
        prod.AddField(sci, "height", prod.MakeFloatArray(kDims2D, kHeight2D)));
    ASSERT_TRUE(prod.AddField(sci, "elevation",
                              prod.MakeStringArray(kDims1D, kBadStr)));
    constexpr std::array<std::string_view, 4> kBadStr4 = {"a", "b", "c", "d"};
    ASSERT_TRUE(prod.AddField(sci, "synergetic_target_classification",
                              prod.MakeStringArray(kDims2D, kBadStr4)));

    IngestExerciseResult res =
        ExerciseIngestionModule(ac_mod, prod.product(), /*options_str=*/nullptr,
                                /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["altitude"].read_status);
    EXPECT_EQ(-1, res.variables["surface_altitude"].read_status);
    EXPECT_EQ(-1, res.variables["scene_type"].read_status);
  }

  // 5. read_orbit_index wrong type; missing geoid_offset after valid elevation;
  // missing aerosol_layer_top in ECA_ATL_ALD_2A.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sci = prod.MakeRecord();
    ASSERT_NE(nullptr, sci);
    ASSERT_TRUE(prod.AddField(prod.root(), "ScienceData", sci));
    // Put a string at orbitNumber so coda_cursor_read_uint32 fails.
    CodaMemNode* header_data = prod.MakeRecord();
    CodaMemNode* vph = prod.MakeRecord();
    CodaMemNode* mph = prod.MakeRecord();
    constexpr std::array<std::string_view, 1> kBadOrbit = {"not_uint"};
    ASSERT_TRUE(prod.AddField(mph, "orbitNumber",
                              prod.MakeStringArray(kOrbitDims, kBadOrbit)));
    ASSERT_TRUE(prod.AddField(vph, "MainProductHeader", mph));
    ASSERT_TRUE(prod.AddField(header_data, "VariableProductHeader", vph));
    ASSERT_TRUE(prod.AddField(prod.root(), "HeaderData", header_data));

    AddCommon1D(prod, sci);
    constexpr std::array<int32_t, 2> kLayers = {1, 2};
    ASSERT_TRUE(prod.AddField(sci, "max_layers",
                              prod.MakeInt32Array(kDims1D, kLayers)));
    ASSERT_TRUE(prod.AddField(sci, "aerosol_layer_base",
                              prod.MakeFloatArray(kDims2D, kHeight2D)));

    IngestExerciseResult res = ExerciseIngestionModule(ald_mod, prod.product(),
                                                       /*options_str=*/nullptr,
                                                       /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["altitude_bounds"].read_status);
  }

  // 6. read_aerosol_layer_base_top_as_altitude missing geoid_offset after base
  // and top succeed.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sci = BuildEarthCareSkeleton(prod, 1);
    ASSERT_NE(nullptr, sci);
    AddCommon1D(prod, sci);
    constexpr std::array<int32_t, 2> kLayers = {1, 2};
    ASSERT_TRUE(prod.AddField(sci, "max_layers",
                              prod.MakeInt32Array(kDims1D, kLayers)));
    ASSERT_TRUE(prod.AddField(sci, "aerosol_layer_base",
                              prod.MakeFloatArray(kDims2D, kHeight2D)));
    ASSERT_TRUE(prod.AddField(sci, "aerosol_layer_top",
                              prod.MakeFloatArray(kDims2D, kHeight2D)));
    IngestExerciseResult res = ExerciseIngestionModule(ald_mod, prod.product(),
                                                       /*options_str=*/nullptr,
                                                       /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["altitude_bounds"].read_status);
  }

  // 7. 2D swath errors: read_latitude_bounds/read_longitude_bounds failure
  // when longitude is missing; read_aerosol_angstrom_exponent(_error) missing;
  // read_cloud_top_height_AM_as_altitude missing diff when source=atlid.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sci = BuildEarthCareSkeleton(prod, 2);
    ASSERT_NE(nullptr, sci);
    ASSERT_TRUE(
        prod.AddField(sci, "latitude", prod.MakeDoubleArray(kDims2D, kLat2D)));
    IngestExerciseResult res = ExerciseIngestionModule(acd_mod, prod.product(),
                                                       /*options_str=*/nullptr,
                                                       /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["longitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["angstrom_exponent"].read_status);
    EXPECT_EQ(-1, res.variables["angstrom_exponent_uncertainty"].read_status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sci = BuildEarthCareSkeleton(prod, 3);
    ASSERT_NE(nullptr, sci);
    AddCommon2D(prod, sci);
    ASSERT_TRUE(prod.AddField(sci, "cloud_top_height_MSI",
                              prod.MakeFloatArray(kDims2D, kHeight2D)));
    ASSERT_TRUE(prod.AddField(sci, "geoid_offset",
                              prod.MakeFloatArray(kDims1D, kGeoid1D)));
    IngestExerciseResult res = ExerciseIngestionModule(
        cth_mod, prod.product(), "source=atlid", /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["cloud_top_height"].read_status);
  }

  // 8. Relative error functions failing on missing base field +
  // read_cloud_phase error.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sci = BuildEarthCareSkeleton(prod, 4);
    ASSERT_NE(nullptr, sci);
    AddCommon1D(prod, sci);
    ASSERT_TRUE(
        prod.AddField(sci, "height", prod.MakeFloatArray(kDims2D, kHeight2D)));
    ASSERT_TRUE(prod.AddField(sci, "geoid_offset",
                              prod.MakeFloatArray(kDims1D, kGeoid1D)));
    ASSERT_TRUE(prod.AddField(sci, "liquid_water_content_relative_error",
                              prod.MakeFloatArray(kDims2D, kHeight2D)));
    ASSERT_TRUE(prod.AddField(sci, "liquid_effective_radius_relative_error",
                              prod.MakeFloatArray(kDims2D, kHeight2D)));
    IngestExerciseResult res = ExerciseIngestionModule(cpr_mod, prod.product(),
                                                       /*options_str=*/nullptr,
                                                       /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1,
              res.variables["liquid_water_density_uncertainty"].read_status);
    EXPECT_EQ(-1, res.variables["liquid_particle_effective_radius_uncertainty"]
                      .read_status);
  }

  // 9. BBR dataset missing (init falls back to ScienceData/latitude) and
  // BBR edge coordinate size/type errors.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sci = BuildEarthCareSkeleton(prod, 5);
    ASSERT_NE(nullptr, sci);
    AddCommon1D(prod, sci);
    IngestExerciseResult res = ExerciseIngestionModule(rad_mod, prod.product(),
                                                       /*options_str=*/nullptr,
                                                       /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["radiance"].read_status);
    EXPECT_EQ(-1, res.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["longitude_bounds"].read_status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sci = BuildEarthCareSkeleton(prod, 5);
    ASSERT_NE(nullptr, sci);
    AddCommon1D(prod, sci);
    IngestExerciseResult res = ExerciseIngestionModule(flx_mod, prod.product(),
                                                       /*options_str=*/nullptr,
                                                       /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["irradiance"].read_status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sci = BuildEarthCareSkeleton(prod, 6);
    ASSERT_NE(nullptr, sci);
    CodaMemNode* ds = prod.MakeRecord();
    ASSERT_NE(nullptr, ds);
    ASSERT_TRUE(prod.AddField(sci, "Standard", ds));
    constexpr std::array<double, 6> kDirD = {1, 2, 3, 4, 5, 6};
    ASSERT_TRUE(prod.AddField(ds, "latitude",
                              prod.MakeDoubleArray(kDimsBbrDir, kDirD)));
    // Missing zero_weight_edge_coordinate_nadir first.
    IngestExerciseResult res1 = ExerciseIngestionModule(
        rad_mod, prod.product(), /*options_str=*/nullptr,
        /*stop_on_error=*/false);
    EXPECT_EQ(-1, res1.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1, res1.variables["longitude_bounds"].read_status);

    // Wrong element count (2 instead of 16).
    ASSERT_TRUE(prod.AddField(ds, "zero_weight_edge_coordinate_nadir",
                              prod.MakeDoubleArray(kDims1D, kLat1D)));
    IngestExerciseResult res2 = ExerciseIngestionModule(
        rad_mod, prod.product(), /*options_str=*/nullptr,
        /*stop_on_error=*/false);
    EXPECT_EQ(-1, res2.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1, res2.variables["longitude_bounds"].read_status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sci = BuildEarthCareSkeleton(prod, 7);
    ASSERT_NE(nullptr, sci);
    CodaMemNode* ds = prod.MakeRecord();
    ASSERT_NE(nullptr, ds);
    ASSERT_TRUE(prod.AddField(sci, "Standard", ds));
    constexpr std::array<double, 6> kDirD = {1, 2, 3, 4, 5, 6};
    ASSERT_TRUE(prod.AddField(ds, "latitude",
                              prod.MakeDoubleArray(kDimsBbrDir, kDirD)));
    // Wrong element type (strings of length 16) triggers partial_array failure.
    constexpr std::array<std::string_view, 16> kBadEdges = {
        "a", "b", "c", "d", "e", "f", "g", "h",
        "i", "j", "k", "l", "m", "n", "o", "p",
    };
    ASSERT_TRUE(prod.AddField(ds, "zero_weight_edge_coordinate_nadir",
                              prod.MakeStringArray(kDimsBbrBounds, kBadEdges)));
    IngestExerciseResult res = ExerciseIngestionModule(rad_mod, prod.product(),
                                                       /*options_str=*/nullptr,
                                                       /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["longitude_bounds"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
