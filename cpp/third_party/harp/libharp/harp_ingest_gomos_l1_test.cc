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
// Unit tests for GOMOS Level 1 ingestion (libharp/harp-ingest-gomos_l1.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-gomos_l1.c

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/coda_mem_product.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/ingest_test_util.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/google/util/temp_file.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::ElementsAre;

CodaMemNode* MakeSingleNodeArray(CodaMemProduct& prod, CodaMemNode* elem) {
  constexpr long kOne[] = {1};
  CodaMemNode* elems[] = {elem};
  return prod.MakeNodeArray(kOne, elems);
}

void AddMphAndSph(CodaMemProduct& prod, int32_t abs_orbit,
                  double samp_duration) {
  CodaMemNode* mph = prod.MakeRecord();
  ASSERT_NE(nullptr, mph);
  ASSERT_TRUE(prod.AddField(mph, "abs_orbit", prod.MakeInt32(abs_orbit)));
  ASSERT_TRUE(prod.AddField(prod.root(), "mph", mph));

  CodaMemNode* sph = prod.MakeRecord();
  ASSERT_NE(nullptr, sph);
  ASSERT_TRUE(
      prod.AddField(sph, "samp_duration", prod.MakeDouble(samp_duration)));
  ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));
}

void PopulateGomosLimb(CodaMemProduct& prod, int version, int32_t illum_cond) {
  ASSERT_TRUE(prod.SetProductInfo("ENVISAT_GOMOS", "GOM_LIM_1P", version));
  AddMphAndSph(prod, 5555, 0.5);

  const double nom_wl[] = {250.0, 400.0, 650.0};
  const long wl_dims[] = {3};
  CodaMemNode* wav_rec = prod.MakeRecord();
  ASSERT_TRUE(
      prod.AddField(wav_rec, "nom_wl", prod.MakeDoubleArray(wl_dims, nom_wl)));
  ASSERT_TRUE(prod.AddField(prod.root(), "lim_nom_wav_assignment",
                            MakeSingleNodeArray(prod, wav_rec)));

  const double curve_wl[] = {300.0, 500.0, 600.0};
  const double curve_val[] = {1.0, 2.0, 4.0};
  const long three_dims[] = {3};
  CodaMemNode* occ_rec = prod.MakeRecord();
  ASSERT_TRUE(
      prod.AddField(occ_rec, "size_rad_sens_curve_limb", prod.MakeUint8(3)));
  ASSERT_TRUE(prod.AddField(occ_rec, "abs_rad_sens_curve_limb",
                            prod.MakeDoubleArray(three_dims, curve_wl)));
  ASSERT_TRUE(prod.AddField(occ_rec, "rad_sens_curve_limb",
                            prod.MakeDoubleArray(three_dims, curve_val)));
  ASSERT_TRUE(prod.AddField(prod.root(), "lim_occultation_data",
                            MakeSingleNodeArray(prod, occ_rec)));

  const long two_dims[] = {2};
  std::vector<CodaMemNode*> ads_nodes;
  std::vector<CodaMemNode*> mds_nodes;
  for (int i = 0; i < 2; ++i) {
    CodaMemNode* ads = prod.MakeRecord();
    const double tlat[] = {10.0 + i, 11.0 + i};
    const double tlon[] = {20.0 + i, 21.0 + i};
    const double talt[] = {30000.0 + i * 1000.0, 31000.0 + i * 1000.0};
    ASSERT_TRUE(prod.AddField(ads, "tangent_lat",
                              prod.MakeDoubleArray(two_dims, tlat)));
    ASSERT_TRUE(prod.AddField(ads, "tangent_long",
                              prod.MakeDoubleArray(two_dims, tlon)));
    ASSERT_TRUE(prod.AddField(ads, "tangent_alt",
                              prod.MakeDoubleArray(two_dims, talt)));
    ASSERT_TRUE(prod.AddField(ads, "off_back", prod.MakeDouble(10.0)));
    ASSERT_TRUE(prod.AddField(ads, "gain_back", prod.MakeDouble(2.0)));
    ASSERT_TRUE(prod.AddField(ads, "lat", prod.MakeDouble(40.0 + i)));
    ASSERT_TRUE(prod.AddField(ads, "longit", prod.MakeDouble(50.0 + i)));
    ASSERT_TRUE(prod.AddField(ads, "alt", prod.MakeDouble(800000.0 + i)));
    ads_nodes.push_back(ads);

    CodaMemNode* mds = prod.MakeRecord();
    const double back_vals[] = {100.0, 200.0, 300.0, 400.0, 500.0, 600.0};
    const double err_vals[] = {5.0, 10.0, 15.0, 20.0, 25.0, 30.0};
    const long six_dims[] = {6};
    ASSERT_TRUE(prod.AddField(mds, "dsr_time", prod.MakeDouble(1000.0 + i)));
    ASSERT_TRUE(prod.AddField(mds, "up_low_back_corr",
                              prod.MakeDoubleArray(six_dims, back_vals)));
    ASSERT_TRUE(prod.AddField(mds, "up_low_back_no_corr",
                              prod.MakeDoubleArray(six_dims, back_vals)));
    ASSERT_TRUE(prod.AddField(mds, "err_up_low_back_corr",
                              prod.MakeDoubleArray(six_dims, err_vals)));
    mds_nodes.push_back(mds);
  }
  ASSERT_TRUE(prod.AddField(prod.root(), "lim_ads",
                            prod.MakeNodeArray(two_dims, ads_nodes)));
  ASSERT_TRUE(prod.AddField(prod.root(), "lim_mds",
                            prod.MakeNodeArray(two_dims, mds_nodes)));

  CodaMemNode* qual = prod.MakeRecord();
  ASSERT_TRUE(prod.AddField(qual, version == 0 ? "limb_flag" : "obs_illum_cond",
                            prod.MakeInt32(illum_cond)));
  ASSERT_TRUE(prod.AddField(prod.root(), "lim_summary_quality",
                            MakeSingleNodeArray(prod, qual)));
}

void PopulateGomosTra(CodaMemProduct& prod, int version, int32_t illum_cond) {
  ASSERT_TRUE(prod.SetProductInfo("ENVISAT_GOMOS", "GOM_TRA_1P", version));
  AddMphAndSph(prod, 6666, 0.25);

  const double nom_wl[] = {250.0, 400.0, 650.0};
  const long three_dims[] = {3};
  CodaMemNode* wav_rec = prod.MakeRecord();
  ASSERT_TRUE(prod.AddField(wav_rec, "nom_wl",
                            prod.MakeDoubleArray(three_dims, nom_wl)));
  ASSERT_TRUE(prod.AddField(prod.root(), "tra_nom_wav_assignment",
                            MakeSingleNodeArray(prod, wav_rec)));

  const long two_dims[] = {2};
  std::vector<CodaMemNode*> geo_nodes;
  std::vector<CodaMemNode*> tra_nodes;
  for (int i = 0; i < 2; ++i) {
    CodaMemNode* geo = prod.MakeRecord();
    const double tlat[] = {1.0 + i, 2.0 + i};
    const double tlon[] = {3.0 + i, 4.0 + i};
    const double talt[] = {25000.0 + i * 500.0, 26000.0 + i * 500.0};
    const double slat[] = {10.0 + i, 11.0 + i};
    const double slon[] = {20.0 + i, 21.0 + i};
    const double salt[] = {790000.0 + i, 790500.0 + i};
    ASSERT_TRUE(prod.AddField(geo, "tangent_lat",
                              prod.MakeDoubleArray(two_dims, tlat)));
    ASSERT_TRUE(prod.AddField(geo, "tangent_long",
                              prod.MakeDoubleArray(two_dims, tlon)));
    ASSERT_TRUE(prod.AddField(geo, "tangent_alt",
                              prod.MakeDoubleArray(two_dims, talt)));
    ASSERT_TRUE(
        prod.AddField(geo, "lat", prod.MakeDoubleArray(two_dims, slat)));
    ASSERT_TRUE(
        prod.AddField(geo, "longit", prod.MakeDoubleArray(two_dims, slon)));
    ASSERT_TRUE(
        prod.AddField(geo, "alt", prod.MakeDoubleArray(two_dims, salt)));
    geo_nodes.push_back(geo);

    CodaMemNode* tra = prod.MakeRecord();
    const double trans_vals[] = {0.8, 0.9, 1.0};
    const double cov_vals[] = {0.01, 0.04, 0.09};
    ASSERT_TRUE(prod.AddField(tra, "dsr_time", prod.MakeDouble(2000.0 + i)));
    ASSERT_TRUE(prod.AddField(tra, "trans_spectra",
                              prod.MakeDoubleArray(three_dims, trans_vals)));
    ASSERT_TRUE(
        prod.AddField(tra, "cov", prod.MakeDoubleArray(three_dims, cov_vals)));
    tra_nodes.push_back(tra);
  }
  ASSERT_TRUE(prod.AddField(prod.root(), "tra_geolocation",
                            prod.MakeNodeArray(two_dims, geo_nodes)));
  ASSERT_TRUE(prod.AddField(prod.root(), "tra_transmission",
                            prod.MakeNodeArray(two_dims, tra_nodes)));

  CodaMemNode* qual = prod.MakeRecord();
  ASSERT_TRUE(prod.AddField(qual, version == 0 ? "limb_flag" : "obs_illum_cond",
                            prod.MakeInt32(illum_cond)));
  ASSERT_TRUE(prod.AddField(prod.root(), "tra_summary_quality",
                            MakeSingleNodeArray(prod, qual)));
}

TEST(HarpIngestGomosL1Test, IngestLimbV0UpperAndV1LowerUncorrected) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GOMOS_L1_LIMB");
  ASSERT_NE(nullptr, mod);

  // Version 0, default options (spectra=upper, corrected=true).
  // Note: get_spectral_data calls coda_cursor_goto_next_array_element past the
  // last element of lim_mds in CodaMemProduct (upstream quirk masked on binary
  // files when boundary checks are disabled), so stop_on_error=false exercises
  // all non-spectral variables alongside the spectral entry path.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGomosLimb(prod, /*version=*/0, /*illum_cond=*/2);

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(3, res.dimensions[harp_dimension_spectral]);
    EXPECT_THAT(res.variables["datetime_start"].double_values,
                ElementsAre(1000.0, 1001.0));
    EXPECT_THAT(res.variables["datetime_length"].double_values,
                ElementsAre(0.5));
    EXPECT_THAT(res.variables["orbit_index"].int32_values, ElementsAre(5555));
    EXPECT_THAT(res.variables["latitude"].double_values,
                ElementsAre(11.0, 12.0));
    EXPECT_THAT(res.variables["longitude"].double_values,
                ElementsAre(21.0, 22.0));
    EXPECT_THAT(res.variables["altitude"].double_values,
                ElementsAre(31000.0, 32000.0));
    EXPECT_THAT(res.variables["wavelength"].double_values,
                ElementsAre(250.0, 400.0, 650.0));
    EXPECT_THAT(res.variables["sensor_latitude"].double_values,
                ElementsAre(40.0, 41.0));
    EXPECT_THAT(res.variables["sensor_longitude"].double_values,
                ElementsAre(50.0, 51.0));
    EXPECT_THAT(res.variables["sensor_altitude"].double_values,
                ElementsAre(800000.0, 800001.0));
    EXPECT_THAT(res.variables["scene_type"].int8_values, ElementsAre(2));
    EXPECT_EQ(-1, res.variables["wavelength_photon_radiance"].read_status);
    EXPECT_EQ(
        -1,
        res.variables["wavelength_photon_radiance_uncertainty"].read_status);
  }

  // Version 1, spectra=lower and corrected=false.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGomosLimb(prod, /*version=*/1, /*illum_cond=*/4);

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "spectra=lower;corrected=false",
        /*stop_on_error=*/false);
    EXPECT_THAT(res.variables["latitude"].double_values,
                ElementsAre(10.0, 11.0));
    EXPECT_THAT(res.variables["longitude"].double_values,
                ElementsAre(20.0, 21.0));
    EXPECT_THAT(res.variables["altitude"].double_values,
                ElementsAre(30000.0, 31000.0));
    EXPECT_THAT(res.variables["scene_type"].int8_values, ElementsAre(4));
  }
}

TEST(HarpIngestGomosL1Test, IngestTransmissionV0AndV1) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod =
      FindIngestionModule("GOMOS_L1_TRANSMISSION");
  ASSERT_NE(nullptr, mod);

  for (int version : {0, 1}) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGomosTra(prod, version, /*illum_cond=*/version + 1);

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(3, res.dimensions[harp_dimension_spectral]);
    EXPECT_THAT(res.variables["datetime_start"].double_values,
                ElementsAre(2000.0, 2001.0));
    EXPECT_THAT(res.variables["datetime_length"].double_values,
                ElementsAre(0.25));
    EXPECT_THAT(res.variables["orbit_index"].int32_values, ElementsAre(6666));
    EXPECT_THAT(res.variables["latitude"].double_values, ElementsAre(2.0, 3.0));
    EXPECT_THAT(res.variables["longitude"].double_values,
                ElementsAre(4.0, 5.0));
    EXPECT_THAT(res.variables["altitude"].double_values,
                ElementsAre(26000.0, 26500.0));
    EXPECT_THAT(res.variables["wavelength"].double_values,
                ElementsAre(250.0, 400.0, 650.0));
    EXPECT_THAT(res.variables["sensor_latitude"].double_values,
                ElementsAre(11.0, 12.0));
    EXPECT_THAT(res.variables["sensor_longitude"].double_values,
                ElementsAre(21.0, 22.0));
    EXPECT_THAT(res.variables["sensor_altitude"].double_values,
                ElementsAre(790500.0, 790501.0));
    EXPECT_THAT(res.variables["scene_type"].int8_values,
                ElementsAre(version + 1));
  }
}

TEST(HarpIngestGomosL1Test, ErrorPathsLimbAndTransmission) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* limb_mod = FindIngestionModule("GOMOS_L1_LIMB");
  const harp_ingestion_module* tra_mod =
      FindIngestionModule("GOMOS_L1_TRANSMISSION");
  ASSERT_NE(nullptr, limb_mod);
  ASSERT_NE(nullptr, tra_mod);

  // 1. Limb init failures: missing lim_mds, missing lim_nom_wav_assignment,
  // empty lim_nom_wav_assignment, missing nom_wl, bad nom_wl type, missing
  // lim_occultation_data, empty lim_occultation_data, missing/bad sensitivity
  // curve fields.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    EXPECT_EQ(-1, ExerciseIngestionModule(limb_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_mds",
                              MakeSingleNodeArray(prod, prod.MakeRecord())));
    EXPECT_EQ(-1, ExerciseIngestionModule(limb_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const long zero_dims[] = {0};
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_mds",
                              MakeSingleNodeArray(prod, prod.MakeRecord())));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lim_nom_wav_assignment",
                      prod.MakeNodeArray(zero_dims, {}, prod.MakeRecord())));
    EXPECT_EQ(-1, ExerciseIngestionModule(limb_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_mds",
                              MakeSingleNodeArray(prod, prod.MakeRecord())));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_nom_wav_assignment",
                              MakeSingleNodeArray(prod, prod.MakeRecord())));
    EXPECT_EQ(-1, ExerciseIngestionModule(limb_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const long one_dim[] = {1};
    const std::string_view bad_wl[] = {"bad"};
    CodaMemNode* wav = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(wav, "nom_wl", prod.MakeStringArray(one_dim, bad_wl)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_mds",
                              MakeSingleNodeArray(prod, prod.MakeRecord())));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_nom_wav_assignment",
                              MakeSingleNodeArray(prod, wav)));
    EXPECT_EQ(-1, ExerciseIngestionModule(limb_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const long one_dim[] = {1};
    const double good_wl[] = {400.0};
    CodaMemNode* wav = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(wav, "nom_wl", prod.MakeDoubleArray(one_dim, good_wl)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_mds",
                              MakeSingleNodeArray(prod, prod.MakeRecord())));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_nom_wav_assignment",
                              MakeSingleNodeArray(prod, wav)));
    // Missing lim_occultation_data.
    EXPECT_EQ(-1, ExerciseIngestionModule(limb_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const long one_dim[] = {1};
    const long zero_dims[] = {0};
    const double good_wl[] = {400.0};
    CodaMemNode* wav = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(wav, "nom_wl", prod.MakeDoubleArray(one_dim, good_wl)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_mds",
                              MakeSingleNodeArray(prod, prod.MakeRecord())));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_nom_wav_assignment",
                              MakeSingleNodeArray(prod, wav)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lim_occultation_data",
                      prod.MakeNodeArray(zero_dims, {}, prod.MakeRecord())));
    EXPECT_EQ(-1, ExerciseIngestionModule(limb_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const long one_dim[] = {1};
    const double good_wl[] = {400.0};
    CodaMemNode* wav = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(wav, "nom_wl", prod.MakeDoubleArray(one_dim, good_wl)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_mds",
                              MakeSingleNodeArray(prod, prod.MakeRecord())));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_nom_wav_assignment",
                              MakeSingleNodeArray(prod, wav)));
    CodaMemNode* occ = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(occ, "size_rad_sens_curve_limb", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_occultation_data",
                              MakeSingleNodeArray(prod, occ)));
    EXPECT_EQ(-1, ExerciseIngestionModule(limb_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const long one_dim[] = {1};
    const double good_wl[] = {400.0};
    CodaMemNode* wav = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(wav, "nom_wl", prod.MakeDoubleArray(one_dim, good_wl)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_mds",
                              MakeSingleNodeArray(prod, prod.MakeRecord())));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_nom_wav_assignment",
                              MakeSingleNodeArray(prod, wav)));
    CodaMemNode* occ = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(occ, "size_rad_sens_curve_limb", prod.MakeUint8(1)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_occultation_data",
                              MakeSingleNodeArray(prod, occ)));
    EXPECT_EQ(-1, ExerciseIngestionModule(limb_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const long one_dim[] = {1};
    const double good_wl[] = {400.0};
    const std::string_view bad_str[] = {"x"};
    CodaMemNode* wav = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(wav, "nom_wl", prod.MakeDoubleArray(one_dim, good_wl)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_mds",
                              MakeSingleNodeArray(prod, prod.MakeRecord())));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_nom_wav_assignment",
                              MakeSingleNodeArray(prod, wav)));
    CodaMemNode* occ = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(occ, "size_rad_sens_curve_limb", prod.MakeUint8(1)));
    ASSERT_TRUE(prod.AddField(occ, "abs_rad_sens_curve_limb",
                              prod.MakeStringArray(one_dim, bad_str)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_occultation_data",
                              MakeSingleNodeArray(prod, occ)));
    EXPECT_EQ(-1, ExerciseIngestionModule(limb_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const long one_dim[] = {1};
    const double good_wl[] = {400.0};
    CodaMemNode* wav = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(wav, "nom_wl", prod.MakeDoubleArray(one_dim, good_wl)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_mds",
                              MakeSingleNodeArray(prod, prod.MakeRecord())));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_nom_wav_assignment",
                              MakeSingleNodeArray(prod, wav)));
    CodaMemNode* occ = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(occ, "size_rad_sens_curve_limb", prod.MakeUint8(1)));
    ASSERT_TRUE(prod.AddField(occ, "abs_rad_sens_curve_limb",
                              prod.MakeDoubleArray(one_dim, good_wl)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_occultation_data",
                              MakeSingleNodeArray(prod, occ)));
    EXPECT_EQ(-1, ExerciseIngestionModule(limb_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const long one_dim[] = {1};
    const double good_wl[] = {400.0};
    const std::string_view bad_str[] = {"x"};
    CodaMemNode* wav = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(wav, "nom_wl", prod.MakeDoubleArray(one_dim, good_wl)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_mds",
                              MakeSingleNodeArray(prod, prod.MakeRecord())));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_nom_wav_assignment",
                              MakeSingleNodeArray(prod, wav)));
    CodaMemNode* occ = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(occ, "size_rad_sens_curve_limb", prod.MakeUint8(1)));
    ASSERT_TRUE(prod.AddField(occ, "abs_rad_sens_curve_limb",
                              prod.MakeDoubleArray(one_dim, good_wl)));
    ASSERT_TRUE(prod.AddField(occ, "rad_sens_curve_limb",
                              prod.MakeStringArray(one_dim, bad_str)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_occultation_data",
                              MakeSingleNodeArray(prod, occ)));
    EXPECT_EQ(-1, ExerciseIngestionModule(limb_mod, prod.product()).status);
  }

  // 2. Limb variable read errors (valid init, missing/bad variable datasets).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_GOMOS", "GOM_LIM_1P", 1));
    const long one_dim[] = {1};
    const double good_wl[] = {400.0};
    CodaMemNode* wav = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(wav, "nom_wl", prod.MakeDoubleArray(one_dim, good_wl)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_mds",
                              MakeSingleNodeArray(prod, prod.MakeRecord())));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_nom_wav_assignment",
                              MakeSingleNodeArray(prod, wav)));
    CodaMemNode* occ = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(occ, "size_rad_sens_curve_limb", prod.MakeUint8(1)));
    ASSERT_TRUE(prod.AddField(occ, "abs_rad_sens_curve_limb",
                              prod.MakeDoubleArray(one_dim, good_wl)));
    ASSERT_TRUE(prod.AddField(occ, "rad_sens_curve_limb",
                              prod.MakeDoubleArray(one_dim, good_wl)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_occultation_data",
                              MakeSingleNodeArray(prod, occ)));

    IngestExerciseResult res = ExerciseIngestionModule(limb_mod, prod.product(),
                                                       /*options_str=*/nullptr,
                                                       /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_length"].read_status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["wavelength_photon_radiance"].read_status);
    EXPECT_EQ(-1, res.variables["scene_type"].read_status);
  }

  // 3. Limb wrong scalar/array element types (version 0 and 1).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_GOMOS", "GOM_LIM_1P", 0));
    const long one_dim[] = {1};
    const long zero_dims[] = {0};
    const double good_wl[] = {400.0};
    CodaMemNode* wav = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(wav, "nom_wl", prod.MakeDoubleArray(one_dim, good_wl)));
    CodaMemNode* mds = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mds, "dsr_time", prod.MakeString("bad")));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lim_mds", MakeSingleNodeArray(prod, mds)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_nom_wav_assignment",
                              MakeSingleNodeArray(prod, wav)));
    CodaMemNode* occ = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(occ, "size_rad_sens_curve_limb", prod.MakeUint8(1)));
    ASSERT_TRUE(prod.AddField(occ, "abs_rad_sens_curve_limb",
                              prod.MakeDoubleArray(one_dim, good_wl)));
    ASSERT_TRUE(prod.AddField(occ, "rad_sens_curve_limb",
                              prod.MakeDoubleArray(one_dim, good_wl)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_occultation_data",
                              MakeSingleNodeArray(prod, occ)));
    CodaMemNode* mph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mph, "abs_orbit", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "mph", mph));
    CodaMemNode* sph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sph, "samp_duration", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lim_ads",
                      prod.MakeNodeArray(zero_dims, {}, prod.MakeRecord())));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lim_summary_quality",
                      prod.MakeNodeArray(zero_dims, {}, prod.MakeRecord())));

    IngestExerciseResult res = ExerciseIngestionModule(limb_mod, prod.product(),
                                                       /*options_str=*/nullptr,
                                                       /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_length"].read_status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["scene_type"].read_status);
  }

  // 4. Transmission init and read errors.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    EXPECT_EQ(-1, ExerciseIngestionModule(tra_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "tra_transmission",
                              MakeSingleNodeArray(prod, prod.MakeRecord())));
    EXPECT_EQ(-1, ExerciseIngestionModule(tra_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const long zero_dims[] = {0};
    ASSERT_TRUE(prod.AddField(prod.root(), "tra_transmission",
                              MakeSingleNodeArray(prod, prod.MakeRecord())));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "tra_nom_wav_assignment",
                      prod.MakeNodeArray(zero_dims, {}, prod.MakeRecord())));
    EXPECT_EQ(-1, ExerciseIngestionModule(tra_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "tra_transmission",
                              MakeSingleNodeArray(prod, prod.MakeRecord())));
    ASSERT_TRUE(prod.AddField(prod.root(), "tra_nom_wav_assignment",
                              MakeSingleNodeArray(prod, prod.MakeRecord())));
    EXPECT_EQ(-1, ExerciseIngestionModule(tra_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_GOMOS", "GOM_TRA_1P", 1));
    const long one_dim[] = {1};
    const double good_wl[] = {400.0};
    CodaMemNode* wav = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(wav, "nom_wl", prod.MakeDoubleArray(one_dim, good_wl)));
    CodaMemNode* tra = prod.MakeRecord();
    const std::string_view bad_str[] = {"bad"};
    ASSERT_TRUE(prod.AddField(tra, "trans_spectra",
                              prod.MakeStringArray(one_dim, bad_str)));
    ASSERT_TRUE(prod.AddField(prod.root(), "tra_transmission",
                              MakeSingleNodeArray(prod, tra)));
    ASSERT_TRUE(prod.AddField(prod.root(), "tra_nom_wav_assignment",
                              MakeSingleNodeArray(prod, wav)));
    CodaMemNode* geo = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(geo, "tangent_lat",
                              prod.MakeDoubleArray(one_dim, good_wl)));
    ASSERT_TRUE(prod.AddField(prod.root(), "tra_geolocation",
                              MakeSingleNodeArray(prod, geo)));
    CodaMemNode* qual = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(qual, "obs_illum_cond", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "tra_summary_quality",
                              MakeSingleNodeArray(prod, qual)));

    IngestExerciseResult res = ExerciseIngestionModule(tra_mod, prod.product(),
                                                       /*options_str=*/nullptr,
                                                       /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["wavelength_photon_transmittance"].read_status);
    EXPECT_EQ(-1, res.variables["scene_type"].read_status);
  }

  // 5. Additional error branches: empty sph, short lim_ads, short
  // tangent_lat, missing limb_flag/obs_illum_cond/size_rad_sens_curve_limb,
  // short up_low_back_corr with spectra=lower, and empty lim_mds read_all.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_GOMOS", "GOM_LIM_1P", 1));
    const long one_dim[] = {1};
    const double good_wl[] = {400.0};
    CodaMemNode* wav = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(wav, "nom_wl", prod.MakeDoubleArray(one_dim, good_wl)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_nom_wav_assignment",
                              MakeSingleNodeArray(prod, wav)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_mds",
                              MakeSingleNodeArray(prod, prod.MakeRecord())));
    CodaMemNode* occ = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_occultation_data",
                              MakeSingleNodeArray(prod, occ)));
    EXPECT_EQ(-1, ExerciseIngestionModule(limb_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_GOMOS", "GOM_LIM_1P", 1));
    const long one_dim[] = {1};
    const long two_dims[] = {2};
    const double good_wl[] = {400.0};
    CodaMemNode* wav = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(wav, "nom_wl", prod.MakeDoubleArray(one_dim, good_wl)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_nom_wav_assignment",
                              MakeSingleNodeArray(prod, wav)));
    CodaMemNode* occ = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(occ, "size_rad_sens_curve_limb", prod.MakeUint8(1)));
    ASSERT_TRUE(prod.AddField(occ, "abs_rad_sens_curve_limb",
                              prod.MakeDoubleArray(one_dim, good_wl)));
    ASSERT_TRUE(prod.AddField(occ, "rad_sens_curve_limb",
                              prod.MakeDoubleArray(one_dim, good_wl)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_occultation_data",
                              MakeSingleNodeArray(prod, occ)));

    CodaMemNode* mds0 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mds0, "up_low_back_corr",
                              prod.MakeDoubleArray(one_dim, good_wl)));
    CodaMemNode* mds1 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mds1, "up_low_back_corr",
                              prod.MakeDoubleArray(one_dim, good_wl)));
    CodaMemNode* mds_arr[] = {mds0, mds1};
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_mds",
                              prod.MakeNodeArray(two_dims, mds_arr)));

    CodaMemNode* ads = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(ads, "tangent_lat",
                              prod.MakeDoubleArray(one_dim, good_wl)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lim_ads", MakeSingleNodeArray(prod, ads)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_summary_quality",
                              MakeSingleNodeArray(prod, prod.MakeRecord())));

    IngestExerciseResult res = ExerciseIngestionModule(
        limb_mod, prod.product(), "spectra=lower", /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime_length"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["wavelength_photon_radiance"].read_status);
    EXPECT_EQ(-1, res.variables["scene_type"].read_status);
  }
  {
    // Version 0 missing limb_flag, plus tra_geolocation with 1-element
    // tangent_lat (hits USE_ARRAY_INDEX_1 failure at line 180-181).
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_GOMOS", "GOM_TRA_1P", 0));
    const long one_dim[] = {1};
    const double good_wl[] = {400.0};
    CodaMemNode* wav = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(wav, "nom_wl", prod.MakeDoubleArray(one_dim, good_wl)));
    ASSERT_TRUE(prod.AddField(prod.root(), "tra_nom_wav_assignment",
                              MakeSingleNodeArray(prod, wav)));
    ASSERT_TRUE(prod.AddField(prod.root(), "tra_transmission",
                              MakeSingleNodeArray(prod, prod.MakeRecord())));
    CodaMemNode* geo = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(geo, "tangent_lat",
                              prod.MakeDoubleArray(one_dim, good_wl)));
    ASSERT_TRUE(prod.AddField(prod.root(), "tra_geolocation",
                              MakeSingleNodeArray(prod, geo)));
    ASSERT_TRUE(prod.AddField(prod.root(), "tra_summary_quality",
                              MakeSingleNodeArray(prod, prod.MakeRecord())));

    IngestExerciseResult res = ExerciseIngestionModule(tra_mod, prod.product(),
                                                       /*options_str=*/nullptr,
                                                       /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["scene_type"].read_status);
  }
  {
    // Empty lim_mds calls read_all directly to hit line 270-271
    // (coda_cursor_goto_first_array_element failure in get_spectral_data).
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_GOMOS", "GOM_LIM_1P", 1));
    const long one_dim[] = {1};
    const long zero_dims[] = {0};
    const double good_wl[] = {400.0};
    CodaMemNode* wav = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(wav, "nom_wl", prod.MakeDoubleArray(one_dim, good_wl)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_nom_wav_assignment",
                              MakeSingleNodeArray(prod, wav)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lim_mds",
                      prod.MakeNodeArray(zero_dims, {}, prod.MakeRecord())));
    CodaMemNode* occ = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(occ, "size_rad_sens_curve_limb", prod.MakeUint8(1)));
    ASSERT_TRUE(prod.AddField(occ, "abs_rad_sens_curve_limb",
                              prod.MakeDoubleArray(one_dim, good_wl)));
    ASSERT_TRUE(prod.AddField(occ, "rad_sens_curve_limb",
                              prod.MakeDoubleArray(one_dim, good_wl)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_occultation_data",
                              MakeSingleNodeArray(prod, occ)));

    harp_ingestion_options* opts = nullptr;
    ASSERT_EQ(0, harp_ingestion_options_new(&opts));
    ScopedHarpIngestionOptions opts_guard(opts);
    harp_product_definition* prod_def = nullptr;
    void* user_data = nullptr;
    ASSERT_EQ(0, limb_mod->ingestion_init(limb_mod, prod.product(), opts,
                                          &prod_def, &user_data));
    auto done_guard =
        absl::MakeCleanup([&] { limb_mod->ingestion_done(user_data); });
    for (int i = 0; i < prod_def->num_variable_definitions; ++i) {
      if (std::string_view(prod_def->variable_definition[i]->name) ==
          "wavelength_photon_radiance") {
        double dummy = 0.0;
        harp_array data{};
        data.double_data = &dummy;
        EXPECT_EQ(-1,
                  prod_def->variable_definition[i]->read_all(user_data, data));
      }
    }
  }
}

uint32_t Crc32(std::string_view data) {
  uint32_t crc = 0xFFFFFFFFu;
  for (uint8_t byte : data) {
    crc ^= byte;
    for (int b = 0; b < 8; ++b) {
      crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

void AppendLe16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFFu));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

void AppendLe32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFFu));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

template <typename T>
void AppendPod(std::vector<uint8_t>& out, const T& v) {
  const size_t pos = out.size();
  out.resize(pos + sizeof(T));
  std::memcpy(out.data() + pos, &v, sizeof(T));
}

std::vector<uint8_t> BuildZipArchive(
    const std::vector<std::pair<std::string_view, std::string_view>>& files) {
  std::vector<uint8_t> out;
  struct EntryMeta {
    std::string_view name;
    uint32_t crc;
    uint32_t size;
    uint32_t offset;
  };
  std::vector<EntryMeta> meta;
  meta.reserve(files.size());
  for (const auto& [name, content] : files) {
    const uint32_t offset = static_cast<uint32_t>(out.size());
    const uint32_t crc = Crc32(content);
    const uint32_t sz = static_cast<uint32_t>(content.size());
    meta.push_back({name, crc, sz, offset});
    AppendLe32(out, 0x04034B50u);
    AppendLe16(out, 20);
    AppendLe16(out, 0);
    AppendLe16(out, 0);
    AppendLe16(out, 0);
    AppendLe16(out, 0);
    AppendLe32(out, crc);
    AppendLe32(out, sz);
    AppendLe32(out, sz);
    AppendLe16(out, static_cast<uint16_t>(name.size()));
    AppendLe16(out, 0);
    out.insert(out.end(), name.begin(), name.end());
    out.insert(out.end(), content.begin(), content.end());
  }
  const uint32_t cd_offset = static_cast<uint32_t>(out.size());
  for (const auto& m : meta) {
    AppendLe32(out, 0x02014B50u);
    AppendLe16(out, 20);
    AppendLe16(out, 20);
    AppendLe16(out, 0);
    AppendLe16(out, 0);
    AppendLe16(out, 0);
    AppendLe16(out, 0);
    AppendLe32(out, m.crc);
    AppendLe32(out, m.size);
    AppendLe32(out, m.size);
    AppendLe16(out, static_cast<uint16_t>(m.name.size()));
    AppendLe16(out, 0);
    AppendLe16(out, 0);
    AppendLe16(out, 0);
    AppendLe16(out, 0);
    AppendLe32(out, 0);
    AppendLe32(out, m.offset);
    out.insert(out.end(), m.name.begin(), m.name.end());
  }
  const uint32_t cd_size = static_cast<uint32_t>(out.size()) - cd_offset;
  AppendLe32(out, 0x06054B50u);
  AppendLe16(out, 0);
  AppendLe16(out, 0);
  AppendLe16(out, static_cast<uint16_t>(meta.size()));
  AppendLe16(out, static_cast<uint16_t>(meta.size()));
  AppendLe32(out, cd_size);
  AppendLe32(out, cd_offset);
  AppendLe16(out, 0);
  return out;
}

std::string MakeDoubleXml(std::string_view endian_tag) {
  return std::string(
             "<cd:Float><cd:BitSize>64</cd:BitSize>"
             "<cd:NativeType>double</cd:NativeType>") +
         std::string(endian_tag) + "</cd:Float>";
}

std::string MakeInt32Xml(std::string_view endian_tag) {
  return std::string(
             "<cd:Integer><cd:BitSize>32</cd:BitSize>"
             "<cd:NativeType>int32</cd:NativeType>") +
         std::string(endian_tag) + "</cd:Integer>";
}

TEST(HarpIngestGomosL1Test, IngestBinaryProductsViaCodadefCoversSpectralMath) {
  // Synthesize a minimal binary .codadef in TempDir so harp_ingest opens
  // binary GOM_LIM_1P / GOM_TRA_1P files with CODA boundary checks disabled
  // (matching production ENVISAT ingestion, avoiding CodaMemProduct's
  // unconditional coda_mem_array bounds error at get_spectral_data tail).
  TempDir def_dir("gomos_l1_codadef");
  const std::string endian_tag =
      std::endian::native == std::endian::little ? "<cd:LittleEndian/>" : "";
  const std::string d_xml = MakeDoubleXml(endian_tag);
  const std::string i32_xml = MakeInt32Xml(endian_tag);

  constexpr std::string_view kIndexXml =
      R"(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="ENVISAT_GOMOS">
  <cd:ProductType name="GOM_LIM_1P">
    <cd:ProductDefinition id="GOM_LIM_1P" format="binary" version="1">
      <cd:DetectionRule><cd:MatchData offset="0">GOMLIM1P</cd:MatchData></cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="GOM_TRA_1P">
    <cd:ProductDefinition id="GOM_TRA_1P" format="binary" version="1">
      <cd:DetectionRule><cd:MatchData offset="0">GOMTRA1P</cd:MatchData></cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
</cd:ProductClass>)";

  const std::string limb_xml = std::string(R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="GOM_LIM_1P" format="binary">
  <cd:Record>
    <cd:Field name="magic"><cd:Raw><cd:BitSize>64</cd:BitSize></cd:Raw></cd:Field>
    <cd:Field name="mph"><cd:Record>
      <cd:Field name="abs_orbit">)") +
                               i32_xml + R"(</cd:Field>
    </cd:Record></cd:Field>
    <cd:Field name="sph"><cd:Record>
      <cd:Field name="samp_duration">)" +
                               d_xml + R"(</cd:Field>
    </cd:Record></cd:Field>
    <cd:Field name="lim_nom_wav_assignment"><cd:Array><cd:Dimension>1</cd:Dimension><cd:Record>
      <cd:Field name="nom_wl"><cd:Array><cd:Dimension>4</cd:Dimension>)" +
                               d_xml + R"(</cd:Array></cd:Field>
    </cd:Record></cd:Array></cd:Field>
    <cd:Field name="lim_occultation_data"><cd:Array><cd:Dimension>1</cd:Dimension><cd:Record>
      <cd:Field name="size_rad_sens_curve_limb"><cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>uint8</cd:NativeType></cd:Integer></cd:Field>
      <cd:Field name="abs_rad_sens_curve_limb"><cd:Array><cd:Dimension>3</cd:Dimension>)" +
                               d_xml + R"(</cd:Array></cd:Field>
      <cd:Field name="rad_sens_curve_limb"><cd:Array><cd:Dimension>3</cd:Dimension>)" +
                               d_xml + R"(</cd:Array></cd:Field>
    </cd:Record></cd:Array></cd:Field>
    <cd:Field name="lim_ads"><cd:Array><cd:Dimension>2</cd:Dimension><cd:Record>
      <cd:Field name="tangent_lat"><cd:Array><cd:Dimension>2</cd:Dimension>)" +
                               d_xml + R"(</cd:Array></cd:Field>
      <cd:Field name="tangent_long"><cd:Array><cd:Dimension>2</cd:Dimension>)" +
                               d_xml + R"(</cd:Array></cd:Field>
      <cd:Field name="tangent_alt"><cd:Array><cd:Dimension>2</cd:Dimension>)" +
                               d_xml + R"(</cd:Array></cd:Field>
      <cd:Field name="off_back">)" +
                               d_xml + R"(</cd:Field>
      <cd:Field name="gain_back">)" +
                               d_xml + R"(</cd:Field>
      <cd:Field name="lat">)" + d_xml +
                               R"(</cd:Field>
      <cd:Field name="longit">)" +
                               d_xml + R"(</cd:Field>
      <cd:Field name="alt">)" + d_xml +
                               R"(</cd:Field>
    </cd:Record></cd:Array></cd:Field>
    <cd:Field name="lim_mds"><cd:Array><cd:Dimension>2</cd:Dimension><cd:Record>
      <cd:Field name="dsr_time">)" +
                               d_xml + R"(</cd:Field>
      <cd:Field name="up_low_back_corr"><cd:Array><cd:Dimension>8</cd:Dimension>)" +
                               d_xml + R"(</cd:Array></cd:Field>
      <cd:Field name="up_low_back_no_corr"><cd:Array><cd:Dimension>8</cd:Dimension>)" +
                               d_xml + R"(</cd:Array></cd:Field>
      <cd:Field name="err_up_low_back_corr"><cd:Array><cd:Dimension>8</cd:Dimension>)" +
                               d_xml + R"(</cd:Array></cd:Field>
    </cd:Record></cd:Array></cd:Field>
    <cd:Field name="lim_summary_quality"><cd:Array><cd:Dimension>1</cd:Dimension><cd:Record>
      <cd:Field name="obs_illum_cond">)" +
                               i32_xml + R"(</cd:Field>
    </cd:Record></cd:Array></cd:Field>
  </cd:Record>
</cd:ProductDefinition>)";

  const std::string tra_xml = std::string(R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="GOM_TRA_1P" format="binary">
  <cd:Record>
    <cd:Field name="magic"><cd:Raw><cd:BitSize>64</cd:BitSize></cd:Raw></cd:Field>
    <cd:Field name="mph"><cd:Record>
      <cd:Field name="abs_orbit">)") +
                              i32_xml + R"(</cd:Field>
    </cd:Record></cd:Field>
    <cd:Field name="sph"><cd:Record>
      <cd:Field name="samp_duration">)" +
                              d_xml + R"(</cd:Field>
    </cd:Record></cd:Field>
    <cd:Field name="tra_nom_wav_assignment"><cd:Array><cd:Dimension>1</cd:Dimension><cd:Record>
      <cd:Field name="nom_wl"><cd:Array><cd:Dimension>3</cd:Dimension>)" +
                              d_xml + R"(</cd:Array></cd:Field>
    </cd:Record></cd:Array></cd:Field>
    <cd:Field name="tra_geolocation"><cd:Array><cd:Dimension>2</cd:Dimension><cd:Record>
      <cd:Field name="tangent_lat"><cd:Array><cd:Dimension>2</cd:Dimension>)" +
                              d_xml + R"(</cd:Array></cd:Field>
      <cd:Field name="tangent_long"><cd:Array><cd:Dimension>2</cd:Dimension>)" +
                              d_xml + R"(</cd:Array></cd:Field>
      <cd:Field name="tangent_alt"><cd:Array><cd:Dimension>2</cd:Dimension>)" +
                              d_xml + R"(</cd:Array></cd:Field>
      <cd:Field name="lat"><cd:Array><cd:Dimension>2</cd:Dimension>)" +
                              d_xml + R"(</cd:Array></cd:Field>
      <cd:Field name="longit"><cd:Array><cd:Dimension>2</cd:Dimension>)" +
                              d_xml + R"(</cd:Array></cd:Field>
      <cd:Field name="alt"><cd:Array><cd:Dimension>2</cd:Dimension>)" +
                              d_xml + R"(</cd:Array></cd:Field>
    </cd:Record></cd:Array></cd:Field>
    <cd:Field name="tra_transmission"><cd:Array><cd:Dimension>2</cd:Dimension><cd:Record>
      <cd:Field name="dsr_time">)" +
                              d_xml + R"(</cd:Field>
      <cd:Field name="trans_spectra"><cd:Array><cd:Dimension>3</cd:Dimension>)" +
                              d_xml + R"(</cd:Array></cd:Field>
      <cd:Field name="cov"><cd:Array><cd:Dimension>3</cd:Dimension>)" +
                              d_xml + R"(</cd:Array></cd:Field>
    </cd:Record></cd:Array></cd:Field>
    <cd:Field name="tra_summary_quality"><cd:Array><cd:Dimension>1</cd:Dimension><cd:Record>
      <cd:Field name="obs_illum_cond">)" +
                              i32_xml + R"(</cd:Field>
    </cd:Record></cd:Array></cd:Field>
  </cd:Record>
</cd:ProductDefinition>)";

  const std::vector<uint8_t> zip_bytes = BuildZipArchive({
      {"index.xml", kIndexXml},
      {"products/GOM_LIM_1P.xml", limb_xml},
      {"products/GOM_TRA_1P.xml", tra_xml},
  });
  ASSERT_TRUE(WriteFileBytes(def_dir.child("gomos.codadef"), zip_bytes));

  // Build binary GOM_LIM_1P file. Wavelengths [250, 400, 500, 650] against
  // sensitivity curve knots [300, 500, 600] hit: < min, linear interp,
  // exact knot (NaN), and > max in spectral_conversion_factor.
  const auto limb_path = def_dir.child("limb.dat");
  {
    std::vector<uint8_t> buf;
    const char magic[8] = {'G', 'O', 'M', 'L', 'I', 'M', '1', 'P'};
    buf.insert(buf.end(), magic, magic + 8);
    AppendPod<int32_t>(buf, 7777);
    AppendPod<double>(buf, 0.5);
    for (double w : {250.0, 400.0, 500.0, 650.0}) {
      AppendPod<double>(buf, w);
    }
    AppendPod<uint8_t>(buf, 3);
    for (double w : {300.0, 500.0, 600.0}) {
      AppendPod<double>(buf, w);
    }
    for (double v : {1.0, 2.0, 4.0}) {
      AppendPod<double>(buf, v);
    }
    for (int i = 0; i < 2; ++i) {
      for (double v :
           {10.0 + i, 11.0 + i, 20.0 + i, 21.0 + i, 30000.0 + i * 100.0,
            31000.0 + i * 100.0, 10.0, 2.0, 40.0 + i, 50.0 + i, 800000.0 + i}) {
        AppendPod<double>(buf, v);
      }
    }
    for (int i = 0; i < 2; ++i) {
      AppendPod<double>(buf, 1000.0 + i);
      for (int j = 0; j < 8; ++j) {
        AppendPod<double>(buf, 20.0 * (j + 1));
      }
      for (int j = 0; j < 8; ++j) {
        AppendPod<double>(buf, 10.0 * (j + 1));
      }
      for (int j = 0; j < 8; ++j) {
        AppendPod<double>(buf, 5.0);
      }
    }
    AppendPod<int32_t>(buf, 2);
    ASSERT_TRUE(WriteFileBytes(limb_path, buf));
  }

  // Build binary GOM_TRA_1P file.
  const auto tra_path = def_dir.child("tra.dat");
  {
    std::vector<uint8_t> buf;
    const char magic[8] = {'G', 'O', 'M', 'T', 'R', 'A', '1', 'P'};
    buf.insert(buf.end(), magic, magic + 8);
    AppendPod<int32_t>(buf, 8888);
    AppendPod<double>(buf, 0.25);
    for (double w : {250.0, 400.0, 650.0}) {
      AppendPod<double>(buf, w);
    }
    for (int i = 0; i < 2; ++i) {
      for (int k = 0; k < 12; ++k) {
        AppendPod<double>(buf, 1.0 + i + k);
      }
    }
    for (int i = 0; i < 2; ++i) {
      AppendPod<double>(buf, 2000.0 + i);
      for (double v : {0.5, 0.6, 0.7}) {
        AppendPod<double>(buf, v);
      }
      for (double v : {0.01, 0.04, 0.09}) {
        AppendPod<double>(buf, v);
      }
    }
    AppendPod<int32_t>(buf, 1);
    ASSERT_TRUE(WriteFileBytes(tra_path, buf));
  }

  ASSERT_EQ(HARP_SUCCESS, harp_set_coda_definition_path(def_dir.c_str()));
  auto reset_coda_path =
      absl::MakeCleanup([] { harp_set_coda_definition_path(nullptr); });
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_ingest(limb_path.c_str(), nullptr, nullptr, &raw_prod));
    ScopedHarpProduct prod(raw_prod);
    harp_variable* rad_var = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_get_variable_by_name(
                  prod.get(), "wavelength_photon_radiance", &rad_var));
    // First spectrum: off=10, gain=2, raw=[20, 40, 60, 80] -> electrons=[20,
    // 30, 40, 50]. Sensitivity factors: <300->1.0, 400->1.5, 500->NaN,
    // >600->4.0.
    EXPECT_DOUBLE_EQ(20.0, rad_var->data.double_data[0]);
    EXPECT_DOUBLE_EQ(45.0, rad_var->data.double_data[1]);
    EXPECT_TRUE(std::isnan(rad_var->data.double_data[2]));
    EXPECT_DOUBLE_EQ(200.0, rad_var->data.double_data[3]);
  }
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_ingest(tra_path.c_str(), nullptr, nullptr, &raw_prod));
    ScopedHarpProduct prod(raw_prod);
    harp_variable* unc_var = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_get_variable_by_name(
                  prod.get(), "wavelength_photon_transmittance_uncertainty",
                  &unc_var));
    EXPECT_DOUBLE_EQ(0.1, unc_var->data.double_data[0]);
    EXPECT_DOUBLE_EQ(0.2, unc_var->data.double_data[1]);
    EXPECT_DOUBLE_EQ(0.3, unc_var->data.double_data[2]);
  }
}

}  // namespace
}  // namespace stcorp_harp
