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
// Unit tests for GOME Level 1 ingestion (libharp/harp-ingest-gome_l1.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-gome_l1.c

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

using ::testing::DoubleEq;
using ::testing::ElementsAre;

CodaMemNode* MakeSingleNodeArray(CodaMemProduct& prod, CodaMemNode* elem) {
  constexpr long kOne[] = {1};
  CodaMemNode* elems[] = {elem};
  return prod.MakeNodeArray(kOne, elems);
}

struct EdrSpec {
  int32_t flag;
  double abs_radiance;
  double abs_rad_err;
  double wavelength;
};

struct BrdaSpec {
  std::string_view band_id;
  double integration_time;
  std::vector<EdrSpec> edr;
};

struct EgpSpec {
  int32_t sub_counter;
  double groundpixel_end;
  double corner_lats[5];
  double corner_lons[5];
  double solar_zenith;
  double solar_azimuth;
  double los_zenith;
  double los_azimuth;
  std::vector<BrdaSpec> brda;
};

CodaMemNode* MakeCoordRecord(CodaMemProduct& prod, double lat, double lon) {
  CodaMemNode* rec = prod.MakeRecord();
  if (rec == nullptr || !prod.AddField(rec, "latitude", prod.MakeDouble(lat)) ||
      !prod.AddField(rec, "longitude", prod.MakeDouble(lon))) {
    return nullptr;
  }
  return rec;
}

CodaMemNode* MakeEdrRecord(CodaMemProduct& prod, const EdrSpec& spec) {
  CodaMemNode* rec = prod.MakeRecord();
  if (rec == nullptr ||
      !prod.AddField(rec, "flag", prod.MakeInt32(spec.flag)) ||
      !prod.AddField(rec, "abs_radiance", prod.MakeDouble(spec.abs_radiance)) ||
      !prod.AddField(rec, "abs_rad_err", prod.MakeDouble(spec.abs_rad_err)) ||
      !prod.AddField(rec, "wavelength", prod.MakeDouble(spec.wavelength))) {
    return nullptr;
  }
  return rec;
}

CodaMemNode* MakeBrdaRecord(CodaMemProduct& prod, const BrdaSpec& spec) {
  CodaMemNode* rec = prod.MakeRecord();
  if (rec == nullptr ||
      !prod.AddField(rec, "band_id", prod.MakeString(spec.band_id)) ||
      !prod.AddField(rec, "integration_time",
                     prod.MakeDouble(spec.integration_time))) {
    return nullptr;
  }
  std::vector<CodaMemNode*> edr_nodes;
  edr_nodes.reserve(spec.edr.size());
  for (const auto& e : spec.edr) {
    edr_nodes.push_back(MakeEdrRecord(prod, e));
  }
  const long dims[] = {static_cast<long>(edr_nodes.size())};
  CodaMemNode* sample =
      edr_nodes.empty() ? MakeEdrRecord(prod, {0, 0.0, 0.0, 0.0}) : nullptr;
  if (!prod.AddField(rec, "edr", prod.MakeNodeArray(dims, edr_nodes, sample))) {
    return nullptr;
  }
  return rec;
}

CodaMemNode* MakeEgpRecord(CodaMemProduct& prod, const EgpSpec& spec) {
  CodaMemNode* rec = prod.MakeRecord();
  if (rec == nullptr ||
      !prod.AddField(rec, "sub_counter", prod.MakeInt32(spec.sub_counter))) {
    return nullptr;
  }

  CodaMemNode* agi = prod.MakeRecord();
  if (agi == nullptr || !prod.AddField(agi, "groundpixel_end",
                                       prod.MakeDouble(spec.groundpixel_end))) {
    return nullptr;
  }
  std::vector<CodaMemNode*> coord_nodes;
  for (int i = 0; i < 5; ++i) {
    coord_nodes.push_back(
        MakeCoordRecord(prod, spec.corner_lats[i], spec.corner_lons[i]));
  }
  const long coord_dims[] = {5};
  if (!prod.AddField(agi, "coords",
                     prod.MakeNodeArray(coord_dims, coord_nodes))) {
    return nullptr;
  }

  CodaMemNode* solar = prod.MakeRecord();
  if (solar == nullptr ||
      !prod.AddField(solar, "zenith_b", prod.MakeDouble(spec.solar_zenith)) ||
      !prod.AddField(solar, "azimuth_b", prod.MakeDouble(spec.solar_azimuth)) ||
      !prod.AddField(agi, "solar_angles_spacecraft", solar)) {
    return nullptr;
  }

  CodaMemNode* los = prod.MakeRecord();
  if (los == nullptr ||
      !prod.AddField(los, "zenith_b", prod.MakeDouble(spec.los_zenith)) ||
      !prod.AddField(los, "azimuth_b", prod.MakeDouble(spec.los_azimuth)) ||
      !prod.AddField(agi, "los_spacecraft", los)) {
    return nullptr;
  }

  if (!prod.AddField(rec, "agi", agi)) {
    return nullptr;
  }

  std::vector<CodaMemNode*> brda_nodes;
  brda_nodes.reserve(spec.brda.size());
  for (const auto& b : spec.brda) {
    brda_nodes.push_back(MakeBrdaRecord(prod, b));
  }
  const long brda_dims[] = {static_cast<long>(brda_nodes.size())};
  if (!prod.AddField(rec, "brda", prod.MakeNodeArray(brda_dims, brda_nodes))) {
    return nullptr;
  }
  return rec;
}

void PopulateNominalGomeL1(CodaMemProduct& prod, int32_t orbit,
                           const std::vector<EgpSpec>& egps) {
  ASSERT_TRUE(prod.SetProductInfo("ERS_GOME", "GOM_LVL13_EXTRACTED", 1));
  CodaMemNode* pir = prod.MakeRecord();
  ASSERT_NE(nullptr, pir);
  ASSERT_TRUE(prod.AddField(pir, "start_orbit", prod.MakeInt32(orbit)));
  ASSERT_TRUE(prod.AddField(prod.root(), "pir", pir));

  std::vector<CodaMemNode*> egp_nodes;
  egp_nodes.reserve(egps.size());
  for (const auto& egp : egps) {
    egp_nodes.push_back(MakeEgpRecord(prod, egp));
  }
  const long egp_dims[] = {static_cast<long>(egp_nodes.size())};
  ASSERT_TRUE(prod.AddField(prod.root(), "egp",
                            prod.MakeNodeArray(egp_dims, egp_nodes)));
}

struct SdrSpec {
  double abs_irr;
  double abs_irr_err;
  double wavelength;
};

void PopulateSunReferenceGomeL1(CodaMemProduct& prod, int32_t orbit,
                                double utc_solar_spectrum,
                                const std::vector<std::vector<SdrSpec>>& cdrs) {
  ASSERT_TRUE(prod.SetProductInfo("ERS_GOME", "GOM_LVL13_EXTRACTED", 1));
  CodaMemNode* pir = prod.MakeRecord();
  ASSERT_NE(nullptr, pir);
  ASSERT_TRUE(prod.AddField(pir, "start_orbit", prod.MakeInt32(orbit)));
  ASSERT_TRUE(prod.AddField(prod.root(), "pir", pir));

  CodaMemNode* sfs = prod.MakeRecord();
  ASSERT_NE(nullptr, sfs);
  ASSERT_TRUE(prod.AddField(sfs, "utc_solar_spectrum",
                            prod.MakeDouble(utc_solar_spectrum)));
  ASSERT_TRUE(prod.AddField(prod.root(), "sfs", sfs));

  std::vector<CodaMemNode*> cdr_nodes;
  cdr_nodes.reserve(cdrs.size());
  for (const auto& ch : cdrs) {
    CodaMemNode* cdr = prod.MakeRecord();
    ASSERT_NE(nullptr, cdr);
    std::vector<CodaMemNode*> sdr_nodes;
    sdr_nodes.reserve(ch.size());
    for (const auto& s : ch) {
      CodaMemNode* sdr = prod.MakeRecord();
      ASSERT_NE(nullptr, sdr);
      ASSERT_TRUE(prod.AddField(sdr, "abs_irr", prod.MakeDouble(s.abs_irr)));
      ASSERT_TRUE(
          prod.AddField(sdr, "abs_irr_err", prod.MakeDouble(s.abs_irr_err)));
      ASSERT_TRUE(
          prod.AddField(sdr, "wavelength", prod.MakeDouble(s.wavelength)));
      sdr_nodes.push_back(sdr);
    }
    const long sdr_dims[] = {static_cast<long>(sdr_nodes.size())};
    ASSERT_TRUE(
        prod.AddField(cdr, "sdr", prod.MakeNodeArray(sdr_dims, sdr_nodes)));
    cdr_nodes.push_back(cdr);
  }
  const long cdr_dims[] = {static_cast<long>(cdr_nodes.size())};
  ASSERT_TRUE(prod.AddField(prod.root(), "cdr",
                            prod.MakeNodeArray(cdr_dims, cdr_nodes)));
}

TEST(HarpIngestGomeL1Test, IngestNominalAllBandsAndIntegrationTimeCopy) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GOME_L1_EXTRACTED");
  ASSERT_NE(nullptr, mod);

  // Build 2 EGPs:
  // - EGP 0 has Band 1a (2 pixels, integration_time=1.5), Band 2a (1 pixel),
  //   plus an unrecognized band name ("Ignored") to cover skipping unknown
  //   bands.
  // - EGP 1 has integration_time=3.0 on Band 1a (copy_previous_values = 1),
  //   overwriting EGP 0's Band 1a radiance/rad_err/wavelength/integration_time.
  //   Pixel 1 of EGP 1 has flag=1 so radiance & rad_err remain NaN there.
  std::vector<EgpSpec> egps = {
      {
          .sub_counter = 1,
          .groundpixel_end = 100.0,
          .corner_lats = {1.0, 2.0, 3.0, 4.0, 2.5},
          .corner_lons = {10.0, 20.0, 30.0, 40.0, 25.0},
          .solar_zenith = 35.0,
          .solar_azimuth = 120.0,
          .los_zenith = 10.0,
          .los_azimuth = 240.0,
          .brda =
              {
                  {.band_id = "UnknownBand",
                   .integration_time = 1.5,
                   .edr = {{0, 999.0, 9.0, 999.0}}},
                  {.band_id = "Band 1a",
                   .integration_time = 1.5,
                   .edr = {{0, 11.0, 1.1, 240.0}, {0, 12.0, 1.2, 241.0}}},
                  {.band_id = "Band 2a",
                   .integration_time = 1.5,
                   .edr = {{0, 21.0, 2.1, 310.0}}},
              },
      },
      {
          .sub_counter = 3,
          .groundpixel_end = 101.5,
          .corner_lats = {5.0, 6.0, 7.0, 8.0, 6.5},
          .corner_lons = {50.0, 60.0, 70.0, 80.0, 65.0},
          .solar_zenith = 36.0,
          .solar_azimuth = 121.0,
          .los_zenith = 11.0,
          .los_azimuth = 241.0,
          .brda =
              {
                  {.band_id = "Band 1a",
                   .integration_time = 3.0,
                   .edr = {{0, 110.0, 11.0, 240.5}, {1, 999.0, 9.9, 241.5}}},
                  {.band_id = "Band 2a",
                   .integration_time = 1.5,
                   .edr = {{0, 22.0, 2.2, 311.0}}},
              },
      },
  };

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateNominalGomeL1(prod, 4242, egps);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(4096, res.dimensions[harp_dimension_spectral]);

  EXPECT_THAT(res.variables["datetime_stop"].double_values,
              ElementsAre(100.0, 101.5));
  EXPECT_THAT(res.variables["datetime_length"].double_values,
              ElementsAre(1.5, 1.5));
  EXPECT_THAT(res.variables["orbit_index"].int32_values, ElementsAre(4242));
  EXPECT_THAT(res.variables["latitude"].double_values, ElementsAre(2.5, 6.5));
  EXPECT_THAT(res.variables["longitude"].double_values,
              ElementsAre(25.0, 65.0));
  // Corners reordered as 1, 3, 2, 0.
  EXPECT_THAT(res.variables["latitude_bounds"].double_values,
              ElementsAre(2.0, 4.0, 3.0, 1.0, 6.0, 8.0, 7.0, 5.0));
  EXPECT_THAT(res.variables["longitude_bounds"].double_values,
              ElementsAre(20.0, 40.0, 30.0, 10.0, 60.0, 80.0, 70.0, 50.0));
  EXPECT_THAT(res.variables["scan_subindex"].int8_values, ElementsAre(1, 3));
  EXPECT_THAT(res.variables["scan_direction_type"].int8_values,
              ElementsAre(0, 1));
  EXPECT_THAT(res.variables["solar_zenith_angle"].double_values,
              ElementsAre(35.0, 36.0));
  EXPECT_THAT(res.variables["solar_azimuth_angle"].double_values,
              ElementsAre(120.0, 121.0));
  EXPECT_THAT(res.variables["viewing_zenith_angle"].double_values,
              ElementsAre(10.0, 11.0));
  EXPECT_THAT(res.variables["viewing_azimuth_angle"].double_values,
              ElementsAre(240.0, 241.0));

  const auto& rad = res.variables["wavelength_photon_radiance"].double_values;
  const auto& rad_err =
      res.variables["wavelength_photon_radiance_uncertainty"].double_values;
  const auto& wl = res.variables["wavelength"].double_values;
  const auto& itime = res.variables["integration_time"].double_values;

  // Band 1a occupies spectral indices [0, 1]; Band 2a occupies [2] (offset=2).
  // EGP 0 pixel 0 copied from EGP 1 pixel 0 due to integration_time=3.0.
  EXPECT_THAT(rad[0], DoubleEq(110.0));
  EXPECT_THAT(rad[1], DoubleEq(12.0));
  EXPECT_THAT(rad[2], DoubleEq(21.0));
  EXPECT_TRUE(std::isnan(rad[3]));

  EXPECT_THAT(rad[4096 + 0], DoubleEq(110.0));
  EXPECT_TRUE(std::isnan(rad[4096 + 1]));  // flag == 1 skipped
  EXPECT_THAT(rad[4096 + 2], DoubleEq(22.0));

  EXPECT_THAT(rad_err[0], DoubleEq(11.0));
  EXPECT_TRUE(std::isnan(rad_err[4096 + 1]));

  EXPECT_THAT(wl[0], DoubleEq(240.5));
  EXPECT_THAT(wl[1], DoubleEq(241.5));
  EXPECT_THAT(wl[2], DoubleEq(310.0));

  EXPECT_THAT(itime[0], DoubleEq(3.0));
  EXPECT_THAT(itime[1], DoubleEq(3.0));
  EXPECT_THAT(itime[2], DoubleEq(1.5));
  EXPECT_THAT(itime[4096 + 0], DoubleEq(3.0));
}

TEST(HarpIngestGomeL1Test, IngestNominalSingleBandOptionAndChangedEdrCount) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GOME_L1_EXTRACTED");
  ASSERT_NE(nullptr, mod);

  // EGP 0 has Band 1a and Band 2a (3 EDRs); EGP 1 has Band 2a with 2 EDRs so
  // init_dimensions hits the num_edr_records changed branch (reducing from 3
  // to 2 so subsequent spectral reads remain within bounds).
  std::vector<EgpSpec> egps = {
      {
          .sub_counter = 0,
          .groundpixel_end = 10.0,
          .corner_lats = {0, 0, 0, 0, 0},
          .corner_lons = {0, 0, 0, 0, 0},
          .solar_zenith = 20.0,
          .solar_azimuth = 30.0,
          .los_zenith = 5.0,
          .los_azimuth = 15.0,
          .brda =
              {
                  {.band_id = "Band 1a",
                   .integration_time = 1.5,
                   .edr = {{0, 1.0, 0.1, 250.0}}},
                  {.band_id = "Band 2a",
                   .integration_time = 1.5,
                   .edr = {{0, 50.0, 5.0, 300.0},
                           {0, 51.0, 5.1, 301.0},
                           {0, 52.0, 5.2, 302.0}}},
              },
      },
      {
          .sub_counter = 2,
          .groundpixel_end = 11.5,
          .corner_lats = {1, 1, 1, 1, 1},
          .corner_lons = {2, 2, 2, 2, 2},
          .solar_zenith = 21.0,
          .solar_azimuth = 31.0,
          .los_zenith = 6.0,
          .los_azimuth = 16.0,
          .brda =
              {
                  {.band_id = "Band 2a",
                   .integration_time = 1.5,
                   .edr = {{0, 60.0, 6.0, 300.5}, {0, 61.0, 6.1, 301.5}}},
              },
      },
  };

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateNominalGomeL1(prod, 77, egps);

  IngestExerciseResult res =
      ExerciseIngestionModule(mod, prod.product(), "band=band-2a");
  harp_set_error(HARP_SUCCESS, nullptr);
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(2, res.dimensions[harp_dimension_spectral]);
  EXPECT_THAT(res.variables["wavelength_photon_radiance"].double_values,
              ElementsAre(50.0, 51.0, 60.0, 61.0));
  EXPECT_THAT(res.variables["wavelength"].double_values,
              ElementsAre(300.0, 301.0, 300.5, 301.5));
}

TEST(HarpIngestGomeL1Test, IngestSunReferenceSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GOME_L1_EXTRACTED");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateSunReferenceGomeL1(prod, 8888, 12345.0,
                             {
                                 {{10.0, 0.5, 240.0}, {11.0, 0.6, 241.0}},
                                 {{20.0, 1.0, 310.0}},
                             });

  IngestExerciseResult res =
      ExerciseIngestionModule(mod, prod.product(), "data=sun_reference");
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(1, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(4096, res.dimensions[harp_dimension_spectral]);
  EXPECT_THAT(res.variables["datetime"].double_values, ElementsAre(12345.0));
  EXPECT_THAT(res.variables["orbit_index"].int32_values, ElementsAre(8888));

  const auto& irr = res.variables["wavelength_photon_irradiance"].double_values;
  const auto& irr_err =
      res.variables["wavelength_photon_irradiance_uncertainty"].double_values;
  const auto& wl = res.variables["wavelength"].double_values;
  EXPECT_THAT(irr[0], DoubleEq(10.0));
  EXPECT_THAT(irr[1], DoubleEq(11.0));
  EXPECT_THAT(irr[2], DoubleEq(20.0));
  EXPECT_TRUE(std::isnan(irr[3]));
  EXPECT_THAT(irr_err[0], DoubleEq(0.5));
  EXPECT_THAT(irr_err[1], DoubleEq(0.6));
  EXPECT_THAT(irr_err[2], DoubleEq(1.0));
  EXPECT_THAT(wl[0], DoubleEq(240.0));
  EXPECT_THAT(wl[1], DoubleEq(241.0));
  EXPECT_THAT(wl[2], DoubleEq(310.0));
}

TEST(HarpIngestGomeL1Test, ErrorPathsMissingAndInvalidFields) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GOME_L1_EXTRACTED");
  ASSERT_NE(nullptr, mod);

  // 1. Missing egp in init_dimensions.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. Empty egp array fails coda_cursor_goto_first_array_element.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const long zero_dims[] = {0};
    EgpSpec sample_spec = {.sub_counter = 0,
                           .groundpixel_end = 0.0,
                           .corner_lats = {},
                           .corner_lons = {},
                           .solar_zenith = 0.0,
                           .solar_azimuth = 0.0,
                           .los_zenith = 0.0,
                           .los_azimuth = 0.0,
                           .brda = {{.band_id = "Band 1a",
                                     .integration_time = 1.5,
                                     .edr = {{0, 1.0, 0.1, 250.0}}}}};
    ASSERT_TRUE(prod.AddField(
        prod.root(), "egp",
        prod.MakeNodeArray(zero_dims, {}, MakeEgpRecord(prod, sample_spec))));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
  }

  // 3. Missing brda / empty brda / missing band_id / wrong type band_id /
  // missing edr in init_dimensions.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* egp = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(prod.root(), "egp", MakeSingleNodeArray(prod, egp)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* egp = prod.MakeRecord();
    const long zero_dims[] = {0};
    CodaMemNode* sample_brda =
        MakeBrdaRecord(prod, {"Band 1a", 1.5, {{0, 1.0, 0.1, 250.0}}});
    ASSERT_TRUE(prod.AddField(egp, "brda",
                              prod.MakeNodeArray(zero_dims, {}, sample_brda)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "egp", MakeSingleNodeArray(prod, egp)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* brda = prod.MakeRecord();
    CodaMemNode* egp = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(egp, "brda", MakeSingleNodeArray(prod, brda)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "egp", MakeSingleNodeArray(prod, egp)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* brda = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(brda, "band_id", prod.MakeInt32(1)));
    CodaMemNode* egp = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(egp, "brda", MakeSingleNodeArray(prod, brda)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "egp", MakeSingleNodeArray(prod, egp)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* brda = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(brda, "band_id", prod.MakeString("Band 1a")));
    CodaMemNode* egp = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(egp, "brda", MakeSingleNodeArray(prod, brda)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "egp", MakeSingleNodeArray(prod, egp)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }

  // 4. Minimal egp with valid brda/edr but missing/invalid agi, pir,
  // sub_counter.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* brda =
        MakeBrdaRecord(prod, {"Band 1a", 1.5, {{0, 1.0, 0.1, 250.0}}});
    CodaMemNode* egp = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(egp, "brda", MakeSingleNodeArray(prod, brda)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "egp", MakeSingleNodeArray(prod, egp)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime_stop"].read_status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["scan_subindex"].read_status);
    EXPECT_EQ(-1, res.variables["scan_direction_type"].read_status);
  }

  // 5. Wrong field types in agi, pir, sub_counter, integration_time, flag, edr.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* pir = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(pir, "start_orbit", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "pir", pir));

    CodaMemNode* agi = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(agi, "groundpixel_end", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(
        agi, "coords",
        MakeSingleNodeArray(prod, MakeCoordRecord(prod, 1.0, 2.0))));

    CodaMemNode* brda = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(brda, "band_id", prod.MakeString("Band 1a")));
    ASSERT_TRUE(
        prod.AddField(brda, "integration_time", prod.MakeString("bad")));
    CodaMemNode* edr_rec = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(edr_rec, "flag", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(brda, "edr", MakeSingleNodeArray(prod, edr_rec)));

    CodaMemNode* egp = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(egp, "sub_counter", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(egp, "agi", agi));
    ASSERT_TRUE(prod.AddField(egp, "brda", MakeSingleNodeArray(prod, brda)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "egp", MakeSingleNodeArray(prod, egp)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime_stop"].read_status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["wavelength_photon_radiance"].read_status);
    EXPECT_EQ(-1, res.variables["scan_subindex"].read_status);
    EXPECT_EQ(-1, res.variables["scan_direction_type"].read_status);
  }

  // 6. Sun reference error paths: missing cdr, empty cdr, missing sdr,
  // missing sfs, wrong types.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    EXPECT_EQ(-1,
              ExerciseIngestionModule(mod, prod.product(), "data=sun_reference")
                  .status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const long zero_dims[] = {0};
    CodaMemNode* sample_cdr = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(prod.root(), "cdr",
                              prod.MakeNodeArray(zero_dims, {}, sample_cdr)));
    EXPECT_EQ(-1,
              ExerciseIngestionModule(mod, prod.product(), "data=sun_reference")
                  .status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* cdr = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(prod.root(), "cdr", MakeSingleNodeArray(prod, cdr)));
    EXPECT_EQ(-1,
              ExerciseIngestionModule(mod, prod.product(), "data=sun_reference")
                  .status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const long zero_dims[] = {0};
    CodaMemNode* sdr_sample = prod.MakeRecord();
    CodaMemNode* cdr = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(cdr, "sdr",
                              prod.MakeNodeArray(zero_dims, {}, sdr_sample)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "cdr", MakeSingleNodeArray(prod, cdr)));
    EXPECT_EQ(-1,
              ExerciseIngestionModule(mod, prod.product(), "data=sun_reference")
                  .status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sdr = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sdr, "abs_irr", prod.MakeString("bad")));
    CodaMemNode* cdr = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(cdr, "sdr", MakeSingleNodeArray(prod, sdr)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "cdr", MakeSingleNodeArray(prod, cdr)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "data=sun_reference", /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["wavelength_photon_irradiance"].read_status);
    EXPECT_EQ(-1, res.variables["wavelength"].read_status);
  }

  // 7. Additional get_main_data, get_spectral_data_per_band, and
  // read_sun_reference_datetime error paths.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* agi = prod.MakeRecord();
    // 5 coords: index 0 has wrong type latitude, index 4 is empty record.
    const long five_dims[] = {5};
    CodaMemNode* c0 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(c0, "latitude", prod.MakeString("bad")));
    CodaMemNode* c4 = prod.MakeRecord();
    CodaMemNode* coords[] = {c0, MakeCoordRecord(prod, 1.0, 2.0),
                             MakeCoordRecord(prod, 1.0, 2.0),
                             MakeCoordRecord(prod, 1.0, 2.0), c4};
    ASSERT_TRUE(
        prod.AddField(agi, "coords", prod.MakeNodeArray(five_dims, coords)));

    CodaMemNode* brda = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(brda, "band_id", prod.MakeString("Band 1a")));
    // Missing integration_time -> get_spectral_data_per_band fails at line 229.
    const long zero_dims[] = {0};
    CodaMemNode* sample_edr = MakeEdrRecord(prod, {0, 1.0, 0.1, 250.0});
    ASSERT_TRUE(prod.AddField(brda, "edr",
                              prod.MakeNodeArray(zero_dims, {}, sample_edr)));

    CodaMemNode* egp = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(egp, "agi", agi));
    ASSERT_TRUE(prod.AddField(egp, "brda", MakeSingleNodeArray(prod, brda)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "egp", MakeSingleNodeArray(prod, egp)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime_stop"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["wavelength_photon_radiance"].read_status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* agi = prod.MakeRecord();
    const long five_dims[] = {5};
    CodaMemNode* c0 = prod.MakeRecord();
    CodaMemNode* c4 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(c4, "latitude", prod.MakeString("bad")));
    CodaMemNode* coords[] = {c0, MakeCoordRecord(prod, 1.0, 2.0),
                             MakeCoordRecord(prod, 1.0, 2.0),
                             MakeCoordRecord(prod, 1.0, 2.0), c4};
    ASSERT_TRUE(
        prod.AddField(agi, "coords", prod.MakeNodeArray(five_dims, coords)));

    CodaMemNode* brda = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(brda, "band_id", prod.MakeString("Band 1a")));
    ASSERT_TRUE(prod.AddField(brda, "integration_time", prod.MakeDouble(1.5)));
    const long zero_dims[] = {0};
    CodaMemNode* sample_edr = MakeEdrRecord(prod, {0, 1.0, 0.1, 250.0});
    ASSERT_TRUE(prod.AddField(brda, "edr",
                              prod.MakeNodeArray(zero_dims, {}, sample_edr)));

    CodaMemNode* egp = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(egp, "agi", agi));
    ASSERT_TRUE(prod.AddField(egp, "brda", MakeSingleNodeArray(prod, brda)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "egp", MakeSingleNodeArray(prod, egp)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["wavelength_photon_radiance"].read_status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    // Valid integration_time, flag is string -> 277-278; plus missing
    // abs_radiance in second test, and wrong type abs_radiance in third.
    CodaMemNode* edr_bad_flag = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(edr_bad_flag, "flag", prod.MakeString("bad")));
    CodaMemNode* brda1 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(brda1, "band_id", prod.MakeString("Band 1a")));
    ASSERT_TRUE(prod.AddField(brda1, "integration_time", prod.MakeDouble(1.5)));
    ASSERT_TRUE(
        prod.AddField(brda1, "edr", MakeSingleNodeArray(prod, edr_bad_flag)));

    CodaMemNode* edr_no_rad = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(edr_no_rad, "flag", prod.MakeInt32(0)));
    CodaMemNode* brda2 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(brda2, "band_id", prod.MakeString("Band 2a")));
    ASSERT_TRUE(prod.AddField(brda2, "integration_time", prod.MakeDouble(1.5)));
    ASSERT_TRUE(
        prod.AddField(brda2, "edr", MakeSingleNodeArray(prod, edr_no_rad)));

    const long two_dims[] = {2};
    CodaMemNode* brdas[] = {brda1, brda2};
    CodaMemNode* egp = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(egp, "brda", prod.MakeNodeArray(two_dims, brdas)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "egp", MakeSingleNodeArray(prod, egp)));

    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product(), nullptr, false)
                      .variables["wavelength_photon_radiance"]
                      .read_status);
    EXPECT_EQ(
        -1, ExerciseIngestionModule(mod, prod.product(), "band=band-2a", false)
                .variables["wavelength_photon_radiance"]
                .read_status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* edr_no_flag = prod.MakeRecord();
    CodaMemNode* brda = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(brda, "band_id", prod.MakeString("Band 1a")));
    ASSERT_TRUE(prod.AddField(brda, "integration_time", prod.MakeDouble(1.5)));
    ASSERT_TRUE(
        prod.AddField(brda, "edr", MakeSingleNodeArray(prod, edr_no_flag)));
    CodaMemNode* egp = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(egp, "brda", MakeSingleNodeArray(prod, brda)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "egp", MakeSingleNodeArray(prod, egp)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product(), nullptr, false)
                      .variables["wavelength_photon_radiance"]
                      .read_status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* edr_bad_rad = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(edr_bad_rad, "flag", prod.MakeInt32(0)));
    ASSERT_TRUE(
        prod.AddField(edr_bad_rad, "abs_radiance", prod.MakeString("bad")));
    CodaMemNode* brda = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(brda, "band_id", prod.MakeString("Band 1a")));
    ASSERT_TRUE(prod.AddField(brda, "integration_time", prod.MakeDouble(1.5)));
    ASSERT_TRUE(
        prod.AddField(brda, "edr", MakeSingleNodeArray(prod, edr_bad_rad)));
    CodaMemNode* egp = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(egp, "brda", MakeSingleNodeArray(prod, brda)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "egp", MakeSingleNodeArray(prod, egp)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product(), nullptr, false)
                      .variables["wavelength_photon_radiance"]
                      .read_status);
  }
  {
    // EGP 0 has 1 EDR for Band 1a, EGP 1 has 2 EDRs for Band 1a -> init updates
    // num_edr_records[0] to 2, so reading EGP 0 fails goto_next_array_element.
    std::vector<EgpSpec> egps = {
        {.brda = {{.band_id = "Band 1a",
                   .integration_time = 1.5,
                   .edr = {{0, 1.0, 0.1, 250.0}}}}},
        {.brda = {{.band_id = "Band 1a",
                   .integration_time = 1.5,
                   .edr = {{0, 1.0, 0.1, 250.0}, {0, 2.0, 0.2, 251.0}}}}},
    };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateNominalGomeL1(prod, 10, egps);
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), nullptr, false);
    harp_set_error(HARP_SUCCESS, nullptr);
    EXPECT_EQ(-1, res.variables["wavelength_photon_radiance"].read_status);
  }
  {
    // Sun reference: sfs/utc_solar_spectrum wrong type (line 715-716).
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sfs = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(sfs, "utc_solar_spectrum", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "sfs", sfs));
    CodaMemNode* sdr = prod.MakeRecord();
    CodaMemNode* cdr = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(cdr, "sdr", MakeSingleNodeArray(prod, sdr)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "cdr", MakeSingleNodeArray(prod, cdr)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "data=sun_reference", /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
