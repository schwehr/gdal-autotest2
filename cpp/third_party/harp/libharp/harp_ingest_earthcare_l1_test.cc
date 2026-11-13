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
// Unit tests for EarthCARE L1 ingestion modules
// (libharp/harp-ingest-earthcare_l1.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-earthcare_l1.c

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
using ::testing::FloatEq;

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

TEST(HarpIngestEarthcareL1Test, IngestAtlNom1bAllDataOptions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ECA_ATL_NOM_1B");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  AddOrbitNumber(prod, 77);

  CodaMemNode* sci = prod.MakeRecord();
  constexpr long kTimeDims[] = {2};
  constexpr long kProfDims[] = {2, 2};
  constexpr double kTime[] = {10.0, 20.0};
  constexpr double kLat[] = {1.0, 2.0, 3.0, 4.0};
  constexpr double kLon[] = {5.0, 6.0, 7.0, 8.0};
  constexpr float kAlt[] = {2000.0f, 1000.0f, 2100.0f, 1100.0f};
  constexpr float kRay[] = {1.0f, 2.0f, 3.0f, 4.0f};
  constexpr float kMie[] = {5.0f, 6.0f, 7.0f, 8.0f};
  constexpr float kCross[] = {9.0f, 10.0f, 11.0f, 12.0f};

  ASSERT_TRUE(prod.AddField(sci, "along_track",
                            prod.MakeDoubleArray(kTimeDims, kTime)));
  ASSERT_TRUE(
      prod.AddField(sci, "height", prod.MakeDoubleArray(kTimeDims, kTime)));
  ASSERT_TRUE(
      prod.AddField(sci, "time", prod.MakeDoubleArray(kTimeDims, kTime)));
  ASSERT_TRUE(prod.AddField(sci, "sample_latitude",
                            prod.MakeDoubleArray(kProfDims, kLat)));
  ASSERT_TRUE(prod.AddField(sci, "sample_longitude",
                            prod.MakeDoubleArray(kProfDims, kLon)));
  ASSERT_TRUE(prod.AddField(sci, "sample_altitude",
                            prod.MakeFloatArray(kProfDims, kAlt)));
  ASSERT_TRUE(prod.AddField(sci, "rayleigh_attenuated_backscatter",
                            prod.MakeFloatArray(kProfDims, kRay)));
  ASSERT_TRUE(prod.AddField(sci, "mie_attenuated_backscatter",
                            prod.MakeFloatArray(kProfDims, kMie)));
  ASSERT_TRUE(prod.AddField(sci, "crosspolar_attenuated_backscatter",
                            prod.MakeFloatArray(kProfDims, kCross)));
  ASSERT_TRUE(prod.AddField(prod.root(), "ScienceData", sci));

  // Default (rayleigh), explicit rayleigh, mie, and crosspolar.
  IngestExerciseResult def = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, def.status);
  EXPECT_EQ(2, def.dimensions[harp_dimension_time]);
  EXPECT_EQ(2, def.dimensions[harp_dimension_vertical]);
  EXPECT_THAT(def.variables["altitude"].float_values,
              ElementsAre(1000.0f, 2000.0f, 1100.0f, 2100.0f));
  EXPECT_THAT(def.variables["backscatter_coefficient"].float_values,
              ElementsAre(2.0f, 1.0f, 4.0f, 3.0f));

  IngestExerciseResult ray =
      ExerciseIngestionModule(mod, prod.product(), "data=rayleigh");
  ASSERT_EQ(HARP_SUCCESS, ray.status);
  EXPECT_THAT(ray.variables["backscatter_coefficient"].float_values,
              ElementsAre(2.0f, 1.0f, 4.0f, 3.0f));

  IngestExerciseResult mie =
      ExerciseIngestionModule(mod, prod.product(), "data=mie");
  ASSERT_EQ(HARP_SUCCESS, mie.status);
  EXPECT_THAT(mie.variables["backscatter_coefficient"].float_values,
              ElementsAre(6.0f, 5.0f, 8.0f, 7.0f));

  IngestExerciseResult cross =
      ExerciseIngestionModule(mod, prod.product(), "data=crosspolar");
  ASSERT_EQ(HARP_SUCCESS, cross.status);
  EXPECT_THAT(cross.variables["backscatter_coefficient"].float_values,
              ElementsAre(10.0f, 9.0f, 12.0f, 11.0f));
}

// Populates one BBR resolution group ("standard", "small", or "full") with
// distinct values indexed by direction (0=aft, 1=nadir, 2=fore) and
// band (0=SW, 1=LW) for num_time=2.
CodaMemNode* MakeBbrResolutionGroup(CodaMemProduct& prod, double base_offset) {
  CodaMemNode* grp = prod.MakeRecord();
  constexpr long kTimeDims[] = {2};
  constexpr long kEdgeDims[] = {2, 4};
  constexpr long kDirDims[] = {3, 2};
  constexpr long kDirBndDims[] = {3, 2, 2};

  const double kLat[] = {10.0 + base_offset, 11.0 + base_offset};
  const double kLon[] = {20.0 + base_offset, 21.0 + base_offset};
  const double kEdgeLat[] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
  const double kEdgeLon[] = {11.0, 12.0, 13.0, 14.0, 15.0, 16.0, 17.0, 18.0};
  std::vector<double> dir_vals(6);
  for (int i = 0; i < 6; ++i) {
    dir_vals[i] = base_offset + static_cast<double>(i);
  }
  std::vector<double> dir_bnd_vals(12);
  for (int i = 0; i < 12; ++i) {
    dir_bnd_vals[i] = base_offset + 100.0 + static_cast<double>(i);
  }

  EXPECT_TRUE(prod.AddField(grp, "barycentre_latitude",
                            prod.MakeDoubleArray(kTimeDims, kLat)));
  EXPECT_TRUE(prod.AddField(grp, "barycentre_longitude",
                            prod.MakeDoubleArray(kTimeDims, kLon)));
  EXPECT_TRUE(prod.AddField(grp, "zero_weight_edge_latitude",
                            prod.MakeDoubleArray(kEdgeDims, kEdgeLat)));
  EXPECT_TRUE(prod.AddField(grp, "zero_weight_edge_longitude",
                            prod.MakeDoubleArray(kEdgeDims, kEdgeLon)));
  EXPECT_TRUE(prod.AddField(grp, "solar_azimuth_angle",
                            prod.MakeDoubleArray(kDirDims, dir_vals)));
  EXPECT_TRUE(prod.AddField(grp, "solar_elevation_angle",
                            prod.MakeDoubleArray(kDirDims, dir_vals)));
  EXPECT_TRUE(prod.AddField(grp, "sensor_azimuth_angle",
                            prod.MakeDoubleArray(kDirDims, dir_vals)));
  EXPECT_TRUE(prod.AddField(grp, "sensor_elevation_angle",
                            prod.MakeDoubleArray(kDirDims, dir_vals)));
  EXPECT_TRUE(prod.AddField(grp, "time_barycentre",
                            prod.MakeDoubleArray(kDirBndDims, dir_bnd_vals)));
  EXPECT_TRUE(prod.AddField(grp, "radiance",
                            prod.MakeDoubleArray(kDirBndDims, dir_bnd_vals)));
  EXPECT_TRUE(prod.AddField(grp, "radiance_error",
                            prod.MakeDoubleArray(kDirBndDims, dir_bnd_vals)));
  return grp;
}

TEST(HarpIngestEarthcareL1Test, IngestBbrNom1bOptionsAndResolutions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ECA_BBR_NOM_1B");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  AddOrbitNumber(prod, 88);

  CodaMemNode* sci = prod.MakeRecord();
  constexpr long kTimeDims[] = {2};
  constexpr double kAlong[] = {0.0, 1.0};
  ASSERT_TRUE(prod.AddField(sci, "along_track",
                            prod.MakeDoubleArray(kTimeDims, kAlong)));
  ASSERT_TRUE(
      prod.AddField(sci, "standard", MakeBbrResolutionGroup(prod, 0.0)));
  ASSERT_TRUE(
      prod.AddField(sci, "small", MakeBbrResolutionGroup(prod, 1000.0)));
  ASSERT_TRUE(prod.AddField(sci, "full", MakeBbrResolutionGroup(prod, 2000.0)));
  ASSERT_TRUE(prod.AddField(prod.root(), "ScienceData", sci));

  // Note: upstream `ingestion_init` in harp-ingest-earthcare_l1.c leaves
  // `info->bbr_band` uninitialized unless `band=LW` is passed; always pass
  // `band=LW` for BBR modules to avoid reading uninitialized heap memory.

  // Standard resolution, nadir (dir=1), LW (band=1) -> slice (1*2+1)*2 = 6.
  IngestExerciseResult def =
      ExerciseIngestionModule(mod, prod.product(), "band=LW");
  ASSERT_EQ(HARP_SUCCESS, def.status);
  EXPECT_THAT(def.variables["latitude"].double_values, ElementsAre(10.0, 11.0));
  EXPECT_THAT(def.variables["solar_azimuth_angle"].double_values,
              ElementsAre(2.0, 3.0));
  EXPECT_THAT(def.variables["radiance"].double_values,
              ElementsAre(106.0, 107.0));

  // Fore (dir=2) + LW (band=1) + resolution=full -> offset (2*2+1)*2 = 10.
  IngestExerciseResult full_fore_lw = ExerciseIngestionModule(
      mod, prod.product(), "direction=fore;band=LW;resolution=full");
  ASSERT_EQ(HARP_SUCCESS, full_fore_lw.status);
  EXPECT_THAT(full_fore_lw.variables["latitude"].double_values,
              ElementsAre(2010.0, 2011.0));
  EXPECT_THAT(full_fore_lw.variables["solar_azimuth_angle"].double_values,
              ElementsAre(2004.0, 2005.0));
  EXPECT_THAT(full_fore_lw.variables["radiance"].double_values,
              ElementsAre(2110.0, 2111.0));

  // Aft (dir=0) + LW (band=1) + resolution=small -> offset (0*2+1)*2 = 2.
  IngestExerciseResult small_aft = ExerciseIngestionModule(
      mod, prod.product(), "direction=aft;band=LW;resolution=small");
  ASSERT_EQ(HARP_SUCCESS, small_aft.status);
  EXPECT_THAT(small_aft.variables["latitude"].double_values,
              ElementsAre(1010.0, 1011.0));
  EXPECT_THAT(small_aft.variables["radiance"].double_values,
              ElementsAre(1102.0, 1103.0));

  // Upstream quirk in harp-ingest-earthcare_l1.c line 786:
  // `edge_coordinate=one_weight` sets `bbr_resolution = 1` ("small") instead of
  // `bbr_edge_coordinate = 1`.
  IngestExerciseResult edge_opt = ExerciseIngestionModule(
      mod, prod.product(), "edge_coordinate=one_weight;band=LW");
  ASSERT_EQ(HARP_SUCCESS, edge_opt.status);
  EXPECT_THAT(edge_opt.variables["latitude"].double_values,
              ElementsAre(1010.0, 1011.0));
  EXPECT_THAT(edge_opt.variables["latitude_bounds"].double_values,
              ElementsAre(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0));
}

TEST(HarpIngestEarthcareL1Test, IngestBbrSng1bAcrossTrackReplication) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ECA_BBR_SNG_1B");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  AddOrbitNumber(prod, 99);

  CodaMemNode* sci = prod.MakeRecord();
  constexpr long kAlongDims[] = {2};
  constexpr long kAcrossDims[] = {2};
  constexpr long kTimeDims[] = {3, 2, 2};
  constexpr long kFieldDims[] = {3, 2, 2, 2};
  constexpr double kAlong[] = {0.0, 1.0};
  constexpr double kAcross[] = {0.0, 1.0};

  std::vector<double> time_vals(12);
  std::vector<int8_t> flag_vals(12);
  for (int i = 0; i < 12; ++i) {
    time_vals[i] = 100.0 + static_cast<double>(i);
    flag_vals[i] = static_cast<int8_t>(i);
  }
  std::vector<double> field_vals(24);
  for (int i = 0; i < 24; ++i) {
    field_vals[i] = static_cast<double>(i);
  }

  ASSERT_TRUE(prod.AddField(sci, "along_track",
                            prod.MakeDoubleArray(kAlongDims, kAlong)));
  ASSERT_TRUE(prod.AddField(sci, "across_track",
                            prod.MakeDoubleArray(kAcrossDims, kAcross)));
  ASSERT_TRUE(
      prod.AddField(sci, "time", prod.MakeDoubleArray(kTimeDims, time_vals)));
  ASSERT_TRUE(prod.AddField(sci, "invalid_flag",
                            prod.MakeInt8Array(kTimeDims, flag_vals)));
  for (std::string_view name :
       {"latitude", "longitude", "solar_azimuth_angle", "solar_elevation_angle",
        "sensor_azimuth_angle", "sensor_elevation_angle", "radiance",
        "radiance_error"}) {
    ASSERT_TRUE(
        prod.AddField(sci, name, prod.MakeDoubleArray(kFieldDims, field_vals)));
  }
  ASSERT_TRUE(prod.AddField(prod.root(), "ScienceData", sci));

  // Nadir (dir=1), LW (band=1) -> slice (1*2+1)*2 = [6, 7], replicated
  // across=2.
  IngestExerciseResult def =
      ExerciseIngestionModule(mod, prod.product(), "band=LW");
  ASSERT_EQ(HARP_SUCCESS, def.status);
  EXPECT_EQ(4, def.dimensions[harp_dimension_time]);
  EXPECT_THAT(def.variables["datetime"].double_values,
              ElementsAre(106.0, 106.0, 107.0, 107.0));
  EXPECT_THAT(def.variables["validity"].int8_values, ElementsAre(6, 6, 7, 7));
  EXPECT_THAT(def.variables["radiance"].double_values,
              ElementsAre(12.0, 13.0, 14.0, 15.0));
}

TEST(HarpIngestEarthcareL1Test, IngestMsiNom1bAllBandOptions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ECA_MSI_NOM_1B");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  AddOrbitNumber(prod, 101);

  CodaMemNode* sci = prod.MakeRecord();
  constexpr long kAlongDims[] = {2};
  constexpr long kAcrossDims[] = {2};
  constexpr long kFieldDims[] = {7, 2, 2};
  constexpr long kIrradDims[] = {4, 2};
  constexpr double kTime[] = {10.0, 20.0};
  constexpr double kAcross[] = {0.0, 1.0};

  std::vector<double> field_vals(7 * 4);
  std::vector<int8_t> qual_vals(7 * 4);
  for (int i = 0; i < 28; ++i) {
    field_vals[i] = static_cast<double>(i);
    qual_vals[i] = static_cast<int8_t>(i);
  }
  std::vector<double> irrad_vals(4 * 2);
  for (int i = 0; i < 8; ++i) {
    irrad_vals[i] = 100.0 + static_cast<double>(i);
  }

  ASSERT_TRUE(prod.AddField(sci, "along_track",
                            prod.MakeDoubleArray(kAlongDims, kTime)));
  ASSERT_TRUE(prod.AddField(sci, "across_track",
                            prod.MakeDoubleArray(kAcrossDims, kAcross)));
  ASSERT_TRUE(
      prod.AddField(sci, "time", prod.MakeDoubleArray(kAlongDims, kTime)));
  ASSERT_TRUE(prod.AddField(sci, "solar_spectral_irradiance",
                            prod.MakeDoubleArray(kIrradDims, irrad_vals)));
  ASSERT_TRUE(prod.AddField(sci, "pixel_quality_status",
                            prod.MakeInt8Array(kFieldDims, qual_vals)));
  for (std::string_view name :
       {"latitude", "longitude", "solar_azimuth_angle", "solar_elevation_angle",
        "sensor_azimuth_angle", "sensor_elevation_angle", "pixel_values"}) {
    ASSERT_TRUE(
        prod.AddField(sci, name, prod.MakeDoubleArray(kFieldDims, field_vals)));
  }
  ASSERT_TRUE(prod.AddField(prod.root(), "ScienceData", sci));

  struct BandCase {
    const char* option;
    int band_index;
    bool is_thermal;
  };
  constexpr BandCase kBands[] = {
      {.option = nullptr, .band_index = 0, .is_thermal = false},
      {.option = "band=VNIR", .band_index = 1, .is_thermal = false},
      {.option = "band=SWIR1", .band_index = 2, .is_thermal = false},
      {.option = "band=SWIR2", .band_index = 3, .is_thermal = false},
      {.option = "band=TIR1", .band_index = 4, .is_thermal = true},
      {.option = "band=TIR2", .band_index = 5, .is_thermal = true},
      {.option = "band=TIR3", .band_index = 6, .is_thermal = true},
  };

  for (const auto& tc : kBands) {
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), tc.option);
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(4, res.dimensions[harp_dimension_time]);
    EXPECT_THAT(res.variables["datetime"].double_values,
                ElementsAre(10.0, 10.0, 20.0, 20.0));
    const double base = static_cast<double>(tc.band_index * 4);
    EXPECT_THAT(res.variables["latitude"].double_values,
                ElementsAre(base, base + 1.0, base + 2.0, base + 3.0));
    if (tc.is_thermal) {
      EXPECT_FALSE(res.variables["wavelength_radiance"].included);
      EXPECT_FALSE(res.variables["wavelength_irradiance"].included);
      EXPECT_TRUE(res.variables["brightness_temperature"].included);
      EXPECT_THAT(res.variables["brightness_temperature"].double_values,
                  ElementsAre(base, base + 1.0, base + 2.0, base + 3.0));
    } else {
      EXPECT_TRUE(res.variables["wavelength_radiance"].included);
      EXPECT_TRUE(res.variables["wavelength_irradiance"].included);
      EXPECT_FALSE(res.variables["brightness_temperature"].included);
      const double irr = 100.0 + static_cast<double>(tc.band_index * 2);
      EXPECT_THAT(res.variables["wavelength_irradiance"].double_values,
                  ElementsAre(irr, irr + 1.0, irr, irr + 1.0));
    }
  }
}

TEST(HarpIngestEarthcareL1Test, ErrorPathsMissingAndMismatchedFields) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* atl = FindIngestionModule("ECA_ATL_NOM_1B");
  const harp_ingestion_module* bbr = FindIngestionModule("ECA_BBR_NOM_1B");
  const harp_ingestion_module* sng = FindIngestionModule("ECA_BBR_SNG_1B");
  const harp_ingestion_module* msi = FindIngestionModule("ECA_MSI_NOM_1B");
  ASSERT_NE(nullptr, atl);
  ASSERT_NE(nullptr, bbr);
  ASSERT_NE(nullptr, sng);
  ASSERT_NE(nullptr, msi);

  // 1. Missing /ScienceData and missing /ScienceData/along_track.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    EXPECT_EQ(-1, ExerciseIngestionModule(atl, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "ScienceData", prod.MakeRecord()));
    EXPECT_EQ(-1, ExerciseIngestionModule(atl, prod.product()).status);
  }

  // 2. Partial/mismatched fields across ATL, BBR_NOM, BBR_SNG, MSI_NOM.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* header = prod.MakeRecord();
    CodaMemNode* var_hdr = prod.MakeRecord();
    CodaMemNode* main_hdr = prod.MakeRecord();
    constexpr long kOne[] = {1};
    constexpr std::string_view kBadStr[] = {"bad"};
    ASSERT_TRUE(prod.AddField(main_hdr, "orbitNumber",
                              prod.MakeStringArray(kOne, kBadStr)));
    ASSERT_TRUE(prod.AddField(var_hdr, "MainProductHeader", main_hdr));
    ASSERT_TRUE(prod.AddField(header, "VariableProductHeader", var_hdr));
    ASSERT_TRUE(prod.AddField(prod.root(), "HeaderData", header));

    CodaMemNode* sci = prod.MakeRecord();
    constexpr long kTwo[] = {2};
    constexpr double kOneD[] = {1.0};
    constexpr double kTwoD[] = {1.0, 2.0};
    ASSERT_TRUE(
        prod.AddField(sci, "along_track", prod.MakeDoubleArray(kOne, kOneD)));
    ASSERT_TRUE(
        prod.AddField(sci, "height", prod.MakeDoubleArray(kOne, kOneD)));
    ASSERT_TRUE(prod.AddField(sci, "time", prod.MakeDoubleArray(kTwo, kTwoD)));
    ASSERT_TRUE(prod.AddField(sci, "sample_altitude",
                              prod.MakeStringArray(kOne, kBadStr)));
    ASSERT_TRUE(prod.AddField(sci, "invalid_flag",
                              prod.MakeStringArray(kOne, kBadStr)));
    ASSERT_TRUE(prod.AddField(sci, "solar_spectral_irradiance",
                              prod.MakeStringArray(kOne, kBadStr)));
    ASSERT_TRUE(prod.AddField(prod.root(), "ScienceData", sci));

    IngestExerciseResult atl_res = ExerciseIngestionModule(
        atl, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, atl_res.status);
    EXPECT_EQ(-1, atl_res.variables["datetime"].read_status);
    EXPECT_EQ(-1, atl_res.variables["altitude"].read_status);
    EXPECT_EQ(-1, atl_res.variables["orbit_index"].read_status);

    // Missing "standard" group in BBR_NOM triggers resolution-lookup failures.
    IngestExerciseResult bbr_res = ExerciseIngestionModule(
        bbr, prod.product(), "band=LW", /*stop_on_error=*/false);
    EXPECT_EQ(-1, bbr_res.status);
    EXPECT_EQ(-1, bbr_res.variables["datetime"].read_status);
    EXPECT_EQ(-1, bbr_res.variables["latitude"].read_status);
    EXPECT_EQ(-1, bbr_res.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1, bbr_res.variables["longitude_bounds"].read_status);
    EXPECT_EQ(-1, bbr_res.variables["solar_azimuth_angle"].read_status);

    IngestExerciseResult sng_res = ExerciseIngestionModule(
        sng, prod.product(), "band=LW", /*stop_on_error=*/false);
    EXPECT_EQ(-1, sng_res.status);
    EXPECT_EQ(-1, sng_res.variables["datetime"].read_status);
    EXPECT_EQ(-1, sng_res.variables["validity"].read_status);

    IngestExerciseResult msi_res = ExerciseIngestionModule(
        msi, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, msi_res.status);
    EXPECT_EQ(-1, msi_res.variables["wavelength_irradiance"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
