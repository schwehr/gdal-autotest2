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
// Unit tests for SCIAMACHY Level 1c ingestion module
// (libharp/harp-ingest-sciamachy_l1.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-sciamachy_l1.c

#include <array>
#include <cstdint>
#include <span>
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

constexpr int kMaxPixels = 8192;

CodaMemNode* MakeCoord(CodaMemProduct& prod, double lat, double lon) {
  CodaMemNode* rec = prod.MakeRecord();
  if (rec == nullptr || !prod.AddField(rec, "latitude", prod.MakeDouble(lat)) ||
      !prod.AddField(rec, "longitude", prod.MakeDouble(lon))) {
    return nullptr;
  }
  return rec;
}

// Builds a single `geo` record with optional forward/backward corner winding.
CodaMemNode* MakeGeoRecord(CodaMemProduct& prod, double base_val,
                           bool backward_corners) {
  CodaMemNode* geo = prod.MakeRecord();
  if (geo == nullptr) return nullptr;

  constexpr long kTwoDims[] = {2};
  const double tan_h[2] = {base_val + 1.0, base_val + 2.0};
  if (!prod.AddField(geo, "tan_h", prod.MakeDoubleArray(kTwoDims, tan_h))) {
    return nullptr;
  }
  if (!prod.AddField(geo, "center_coord",
                     MakeCoord(prod, base_val + 0.1, base_val + 0.2))) {
    return nullptr;
  }

  CodaMemNode* tang_elems[] = {
      MakeCoord(prod, base_val + 10.0, base_val + 20.0),
      MakeCoord(prod, base_val + 11.0, base_val + 21.0),
  };
  if (!prod.AddField(geo, "tang_ground_point",
                     prod.MakeNodeArray(kTwoDims, tang_elems))) {
    return nullptr;
  }

  // Corners chosen so z = -sin(qx)*sin(ry):
  //   backward_corners=true  -> r_lat=+10 -> z < 0 -> scan_direction_type = 1
  //   backward_corners=false -> r_lat=-10 -> z > 0 -> scan_direction_type = 0
  const double r_lat = backward_corners ? 10.0 : -10.0;
  constexpr long kCornerDims[] = {4};
  CodaMemNode* corners[] = {
      MakeCoord(prod, 0.0, 0.0),
      MakeCoord(prod, 0.0, 10.0),
      MakeCoord(prod, r_lat, 0.0),
      MakeCoord(prod, base_val + 3.0, base_val + 4.0),
  };
  if (!prod.AddField(geo, "corner_coord",
                     prod.MakeNodeArray(kCornerDims, corners))) {
    return nullptr;
  }

  if (!prod.AddField(geo, "sat_h", prod.MakeDouble(790.0 + base_val)) ||
      !prod.AddField(geo, "sub_sat_point",
                     MakeCoord(prod, base_val + 5.0, base_val + 6.0))) {
    return nullptr;
  }

  const double sol_zen[2] = {30.0 + base_val, 31.0 + base_val};
  const double sol_azi[2] = {120.0 + base_val, 121.0 + base_val};
  const double los_zen[2] = {5.0 + base_val, 6.0 + base_val};
  const double los_azi[2] = {90.0 + base_val, 91.0 + base_val};
  if (!prod.AddField(geo, "sol_zen_ang",
                     prod.MakeDoubleArray(kTwoDims, sol_zen)) ||
      !prod.AddField(geo, "sol_azi_ang",
                     prod.MakeDoubleArray(kTwoDims, sol_azi)) ||
      !prod.AddField(geo, "los_zen_ang",
                     prod.MakeDoubleArray(kTwoDims, los_zen)) ||
      !prod.AddField(geo, "los_azi_ang",
                     prod.MakeDoubleArray(kTwoDims, los_azi))) {
    return nullptr;
  }
  return geo;
}

CodaMemNode* MakeClusConfig(CodaMemProduct& prod, uint16_t clus_len,
                            double intgr_time) {
  CodaMemNode* rec = prod.MakeRecord();
  if (rec == nullptr ||
      !prod.AddField(rec, "clus_len", prod.MakeUint16(clus_len)) ||
      !prod.AddField(rec, "intgr_time", prod.MakeDouble(intgr_time))) {
    return nullptr;
  }
  return rec;
}

CodaMemNode* MakeStateRecord(CodaMemProduct& prod, uint8_t mds_type,
                             double dsr_time, uint16_t num_rep_geo,
                             std::span<CodaMemNode* const> clus_configs) {
  CodaMemNode* rec = prod.MakeRecord();
  const long clus_dims[] = {static_cast<long>(clus_configs.size())};
  if (rec == nullptr ||
      !prod.AddField(rec, "mds_type", prod.MakeUint8(mds_type)) ||
      !prod.AddField(rec, "dsr_time", prod.MakeDouble(dsr_time)) ||
      !prod.AddField(rec, "num_rep_geo", prod.MakeUint16(num_rep_geo)) ||
      !prod.AddField(
          rec, "num_clus",
          prod.MakeUint16(static_cast<uint16_t>(clus_configs.size()))) ||
      !prod.AddField(rec, "clus_config",
                     prod.MakeNodeArray(clus_dims, clus_configs))) {
    return nullptr;
  }
  return rec;
}

CodaMemNode* MakeMdsRecord(CodaMemProduct& prod, int8_t rad_units_flag,
                           uint16_t num_obs, uint16_t num_pixels,
                           std::span<const double> obs_vals,
                           std::span<const double> wvl_vals,
                           std::span<CodaMemNode* const> geo_elems) {
  CodaMemNode* rec = prod.MakeRecord();
  const long obs_dims[] = {num_obs, num_pixels};
  const long wvl_dims[] = {num_pixels};
  const long geo_dims[] = {static_cast<long>(geo_elems.size())};
  if (rec == nullptr ||
      !prod.AddField(rec, "rad_units_flag", prod.MakeInt8(rad_units_flag)) ||
      !prod.AddField(rec, "num_obs", prod.MakeUint16(num_obs)) ||
      !prod.AddField(rec, "num_pixels", prod.MakeUint16(num_pixels)) ||
      !prod.AddField(rec, "observations",
                     prod.MakeDoubleArray(obs_dims, obs_vals)) ||
      !prod.AddField(rec, "pixel_wavelength",
                     prod.MakeDoubleArray(wvl_dims, wvl_vals)) ||
      !prod.AddField(rec, "geo", prod.MakeNodeArray(geo_dims, geo_elems))) {
    return nullptr;
  }
  return rec;
}

// Builds a complete SCIAMACHY L1c product with 2 matching states separated by
// a non-matching state, exercising cluster filtering, observation copying,
// forward/backward/mixed scan directions, and multi-state wavelength reading.
void PopulateSciamachyProduct(CodaMemProduct& prod, std::string_view ds_name,
                              uint8_t mds_type_target) {
  ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_NLC_1P", 1));

  CodaMemNode* mph = prod.MakeRecord();
  ASSERT_TRUE(prod.AddField(mph, "abs_orbit", prod.MakeInt32(12345)));
  ASSERT_TRUE(prod.AddField(prod.root(), "mph", mph));

  // Cluster filter: clusters 0 and 1 active (-1), cluster 2+ filtered (0).
  std::array<int8_t, 64> cluster_filter{};
  cluster_filter[0] = -1;
  cluster_filter[1] = -1;
  constexpr long kFilterDims[] = {64};
  CodaMemNode* cal_rec = prod.MakeRecord();
  ASSERT_TRUE(prod.AddField(cal_rec, "nadir_cluster_flag",
                            prod.MakeInt8Array(kFilterDims, cluster_filter)));
  ASSERT_TRUE(prod.AddField(cal_rec, "limb_cluster_flag",
                            prod.MakeInt8Array(kFilterDims, cluster_filter)));
  ASSERT_TRUE(prod.AddField(cal_rec, "occ_cluster_flag",
                            prod.MakeInt8Array(kFilterDims, cluster_filter)));
  constexpr long kOneDim[] = {1};
  CodaMemNode* cal_arr[] = {cal_rec};
  ASSERT_TRUE(prod.AddField(prod.root(), "cal_options",
                            prod.MakeNodeArray(kOneDim, cal_arr)));

  // Three states: matching, non-matching, matching (with min_integr_time > 1.01
  // on the second matching state to exercise mixed scan_direction_type == 2).
  const uint8_t other_mds = (mds_type_target == 1) ? 2 : 1;
  CodaMemNode* s0_clus[] = {
      MakeClusConfig(prod, 2, 0.5),
      MakeClusConfig(prod, 1, 0.25),
  };
  CodaMemNode* s1_clus[] = {
      MakeClusConfig(prod, 1, 0.1),
  };
  CodaMemNode* s2_clus[] = {
      MakeClusConfig(prod, 2, 1.5),
      MakeClusConfig(prod, 1, 2.0),
      MakeClusConfig(prod, 1, 0.1),  // Filtered out by cluster_filter[2] == 0.
  };
  CodaMemNode* states[] = {
      MakeStateRecord(prod, mds_type_target, 100.0, 2, s0_clus),
      MakeStateRecord(prod, other_mds, 150.0, 1, s1_clus),
      MakeStateRecord(prod, mds_type_target, 200.0, 1, s2_clus),
  };
  constexpr long kStateDims[] = {3};
  ASSERT_TRUE(prod.AddField(prod.root(), "states",
                            prod.MakeNodeArray(kStateDims, states)));

  // Datasource MDS records. Include a trailing dummy record because upstream's
  // cursor loop advances on `i < info->num_states_current_datasource`.
  const double obs0[] = {10.0, 11.0};
  const double wvl0[] = {300.0, 301.0};
  CodaMemNode* geo0[] = {MakeGeoRecord(prod, 1.0, false)};

  const double obs1[] = {20.0, 21.0};
  const double wvl1[] = {400.0};
  CodaMemNode* geo1[] = {
      MakeGeoRecord(prod, 2.0, false),  // forward (0)
      MakeGeoRecord(prod, 3.0, true),   // backward (1)
  };

  const double obs2[] = {30.0, 31.0};
  const double wvl2[] = {310.0, 311.0};
  CodaMemNode* geo2[] = {MakeGeoRecord(prod, 4.0, false)};

  const double obs3[] = {40.0};
  const double wvl3[] = {410.0};
  CodaMemNode* geo3[] = {MakeGeoRecord(prod, 5.0, false)};

  const double obs_dummy[] = {0.0};
  const double wvl_dummy[] = {0.0};
  CodaMemNode* geo_dummy[] = {MakeGeoRecord(prod, 0.0, false)};

  CodaMemNode* mds_recs[] = {
      MakeMdsRecord(prod, -1, 1, 2, obs0, wvl0, geo0),
      MakeMdsRecord(prod, -1, 2, 1, obs1, wvl1, geo1),
      MakeMdsRecord(prod, -1, 1, 2, obs2, wvl2, geo2),
      MakeMdsRecord(prod, -1, 1, 1, obs3, wvl3, geo3),
      MakeMdsRecord(prod, -1, 1, 1, obs_dummy, wvl_dummy, geo_dummy),
  };
  constexpr long kMdsDims[] = {5};
  ASSERT_TRUE(prod.AddField(prod.root(), ds_name,
                            prod.MakeNodeArray(kMdsDims, mds_recs)));
}

CodaMemNode* MakeSunRefRecord(CodaMemProduct& prod, std::string_view id,
                              double irr_base, double unc_scale,
                              double wvl_base) {
  CodaMemNode* rec = prod.MakeRecord();
  std::vector<double> mean_ref(kMaxPixels);
  std::vector<double> rad_pre(kMaxPixels);
  std::vector<double> wvlen(kMaxPixels);
  for (int i = 0; i < kMaxPixels; ++i) {
    mean_ref[i] = irr_base + static_cast<double>(i);
    rad_pre[i] = unc_scale;
    wvlen[i] = wvl_base + static_cast<double>(i) * 0.1;
  }
  constexpr long kPixDims[] = {kMaxPixels};
  if (rec == nullptr ||
      !prod.AddField(rec, "sun_spect_id", prod.MakeString(id)) ||
      !prod.AddField(rec, "mean_ref_spec",
                     prod.MakeDoubleArray(kPixDims, mean_ref)) ||
      !prod.AddField(rec, "rad_pre_mean_sun_ref_spec",
                     prod.MakeDoubleArray(kPixDims, rad_pre)) ||
      !prod.AddField(rec, "wvlen_sun_meas",
                     prod.MakeDoubleArray(kPixDims, wvlen))) {
    return nullptr;
  }
  return rec;
}

TEST(HarpIngestSciamachyL1Test, IngestNadirDefaultAndExplicitOption) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("SCIAMACHY_L1c");
  ASSERT_NE(nullptr, mod);

  for (const char* opt : {static_cast<const char*>(nullptr), "data=nadir"}) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateSciamachyProduct(prod, "nadir", 1);

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), opt);
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(3, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(3, res.dimensions[harp_dimension_spectral]);
    EXPECT_FALSE(res.variables["altitude"].included);
    EXPECT_TRUE(res.variables["latitude_bounds"].included);
    EXPECT_TRUE(res.variables["longitude_bounds"].included);
    EXPECT_TRUE(res.variables["scan_direction_type"].included);

    EXPECT_THAT(res.variables["orbit_index"].int32_values, ElementsAre(12345));
    EXPECT_THAT(res.variables["datetime_start"].double_values,
                ElementsAre(100.0, 100.25, 200.0));
    EXPECT_THAT(res.variables["datetime_length"].double_values,
                ElementsAre(0.25, 0.25, 1.5));
    // Geo comes from max_obs record (elem 1 in state 0, elem 2 in state 2).
    EXPECT_THAT(res.variables["latitude"].double_values,
                ElementsAre(2.1, 3.1, 4.1));
    EXPECT_THAT(res.variables["longitude"].double_values,
                ElementsAre(2.2, 3.2, 4.2));
    // Corners rearranged 0, 2, 3, 1.
    EXPECT_THAT(res.variables["latitude_bounds"].double_values,
                ElementsAre(0.0, -10.0, 5.0, 0.0, 0.0, 10.0, 6.0, 0.0, 0.0,
                            -10.0, 7.0, 0.0));
    EXPECT_THAT(res.variables["longitude_bounds"].double_values,
                ElementsAre(0.0, 0.0, 6.0, 10.0, 0.0, 0.0, 7.0, 10.0, 0.0, 0.0,
                            8.0, 10.0));
    EXPECT_THAT(res.variables["sensor_altitude"].double_values,
                ElementsAre(792.0, 793.0, 794.0));
    EXPECT_THAT(res.variables["sensor_latitude"].double_values,
                ElementsAre(7.0, 8.0, 9.0));
    EXPECT_THAT(res.variables["sensor_longitude"].double_values,
                ElementsAre(8.0, 9.0, 10.0));
    EXPECT_THAT(res.variables["solar_zenith_angle"].double_values,
                ElementsAre(33.0, 34.0, 35.0));
    EXPECT_THAT(res.variables["solar_azimuth_angle"].double_values,
                ElementsAre(123.0, 124.0, 125.0));
    EXPECT_THAT(res.variables["viewing_zenith_angle"].double_values,
                ElementsAre(8.0, 9.0, 10.0));
    EXPECT_THAT(res.variables["viewing_azimuth_angle"].double_values,
                ElementsAre(93.0, 94.0, 95.0));
    EXPECT_THAT(res.variables["scan_direction_type"].int8_values,
                ElementsAre(0, 1, 2));
    EXPECT_THAT(
        res.variables["wavelength_photon_radiance"].double_values,
        ElementsAre(10.0, 11.0, 20.0, 10.0, 11.0, 21.0, 30.0, 31.0, 40.0));
    EXPECT_THAT(res.variables["wavelength"].double_values,
                ElementsAre(300.0, 301.0, 400.0, 300.0, 301.0, 400.0, 310.0,
                            311.0, 410.0));
    EXPECT_THAT(res.variables["integration_time"].double_values,
                ElementsAre(0.5, 0.5, 0.25, 0.5, 0.5, 0.25, 1.5, 1.5, 2.0));
  }
}

TEST(HarpIngestSciamachyL1Test, IngestLimbAndOccultationSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("SCIAMACHY_L1c");
  ASSERT_NE(nullptr, mod);

  struct ModeCase {
    const char* option;
    std::string_view ds_name;
    uint8_t mds_type;
  };
  constexpr ModeCase kCases[] = {
      {.option = "data=limb", .ds_name = "limb", .mds_type = 2},
      {.option = "data=occultation", .ds_name = "occultation", .mds_type = 3},
  };

  for (const auto& tc : kCases) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateSciamachyProduct(prod, tc.ds_name, tc.mds_type);

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), tc.option);
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(3, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(3, res.dimensions[harp_dimension_spectral]);
    EXPECT_TRUE(res.variables["altitude"].included);
    EXPECT_FALSE(res.variables["latitude_bounds"].included);
    EXPECT_FALSE(res.variables["longitude_bounds"].included);
    EXPECT_FALSE(res.variables["scan_direction_type"].included);

    EXPECT_THAT(res.variables["altitude"].double_values,
                ElementsAre(3.0, 4.0, 5.0));
    // Limb/occultation latitude/longitude come from tang_ground_point[1].
    EXPECT_THAT(res.variables["latitude"].double_values,
                ElementsAre(13.0, 14.0, 15.0));
    EXPECT_THAT(res.variables["longitude"].double_values,
                ElementsAre(23.0, 24.0, 25.0));
  }
}

TEST(HarpIngestSciamachyL1Test, IngestSunReferenceSuccessAndNoDiffuser) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("SCIAMACHY_L1c");
  ASSERT_NE(nullptr, mod);

  // 1. First record is non-'D' ("A0"), second is 'D' ("D1"), third is ignored.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_NLC_1P", 1));
    CodaMemNode* sun_recs[] = {
        MakeSunRefRecord(prod, "A0", 1.0, 0.1, 200.0),
        MakeSunRefRecord(prod, "D1", 100.0, 0.05, 250.0),
        MakeSunRefRecord(prod, "D2", 999.0, 0.5, 900.0),
    };
    constexpr long kSunDims[] = {3};
    ASSERT_TRUE(prod.AddField(prod.root(), "sun_reference",
                              prod.MakeNodeArray(kSunDims, sun_recs)));

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "data=sun_reference");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(1, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(kMaxPixels, res.dimensions[harp_dimension_spectral]);
    EXPECT_NEAR(100.0,
                res.variables["wavelength_photon_irradiance"].double_values[0],
                1e-9);
    EXPECT_NEAR(5.0,
                res.variables["wavelength_photon_irradiance_uncertainty"]
                    .double_values[0],
                1e-9);
    EXPECT_NEAR(250.0, res.variables["wavelength"].double_values[0], 1e-9);
    EXPECT_NEAR(250.0 + 0.1 * (kMaxPixels - 1),
                res.variables["wavelength"].double_values[kMaxPixels - 1],
                1e-9);
  }

  // 2. No 'D' sun_spect_id -> 0 observations.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_NLC_1P", 1));
    CodaMemNode* sun_recs[] = {
        MakeSunRefRecord(prod, "A0", 1.0, 0.1, 200.0),
    };
    constexpr long kSunDims[] = {1};
    ASSERT_TRUE(prod.AddField(prod.root(), "sun_reference",
                              prod.MakeNodeArray(kSunDims, sun_recs)));

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "data=sun_reference");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(0, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(kMaxPixels, res.dimensions[harp_dimension_spectral]);
  }
}

TEST(HarpIngestSciamachyL1Test, InitErrorsNadirLimbAndSunReference) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("SCIAMACHY_L1c");
  ASSERT_NE(nullptr, mod);

  // 1. Missing nadir field.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_NLC_1P", 1));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
  }

  // 3. Unavailable nadir field.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_NLC_1P", 1));
    ASSERT_TRUE(prod.AddUnavailableField(prod.root(), "nadir"));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 4. Empty nadir array.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_NLC_1P", 1));
    constexpr long kZeroDim[] = {0};
    CodaMemNode* sample = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(prod.root(), "nadir",
                              prod.MakeNodeArray(kZeroDim, {}, sample)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
  }

  // 5. Missing rad_units_flag.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_NLC_1P", 1));
    constexpr long kOneDim[] = {1};
    CodaMemNode* elems[] = {prod.MakeRecord()};
    ASSERT_TRUE(prod.AddField(prod.root(), "nadir",
                              prod.MakeNodeArray(kOneDim, elems)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
  }

  // 6. Wrong type for rad_units_flag and binary units (rad_units_flag != -1).
  for (bool wrong_type : {true, false}) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_NLC_1P", 1));
    constexpr long kOneDim[] = {1};
    CodaMemNode* rec = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(rec, "rad_units_flag",
                      wrong_type ? prod.MakeString("bad") : prod.MakeInt8(0)));
    CodaMemNode* elems[] = {rec};
    ASSERT_TRUE(prod.AddField(prod.root(), "nadir",
                              prod.MakeNodeArray(kOneDim, elems)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(wrong_type ? HARP_ERROR_CODA : HARP_ERROR_INGESTION,
              res.failed_errno);
  }

  // 7. cal_options missing, empty, missing flag, wrong flag type.
  for (int stage = 0; stage < 4; ++stage) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_NLC_1P", 1));
    constexpr long kOneDim[] = {1};
    CodaMemNode* rec = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(rec, "rad_units_flag", prod.MakeInt8(-1)));
    CodaMemNode* elems[] = {rec};
    ASSERT_TRUE(prod.AddField(prod.root(), "nadir",
                              prod.MakeNodeArray(kOneDim, elems)));
    if (stage == 1) {
      constexpr long kZeroDim[] = {0};
      ASSERT_TRUE(
          prod.AddField(prod.root(), "cal_options",
                        prod.MakeNodeArray(kZeroDim, {}, prod.MakeRecord())));
    } else if (stage == 2) {
      CodaMemNode* cal[] = {prod.MakeRecord()};
      ASSERT_TRUE(prod.AddField(prod.root(), "cal_options",
                                prod.MakeNodeArray(kOneDim, cal)));
    } else if (stage == 3) {
      CodaMemNode* cal_rec = prod.MakeRecord();
      ASSERT_TRUE(
          prod.AddField(cal_rec, "nadir_cluster_flag", prod.MakeString("bad")));
      CodaMemNode* cal[] = {cal_rec};
      ASSERT_TRUE(prod.AddField(prod.root(), "cal_options",
                                prod.MakeNodeArray(kOneDim, cal)));
    }
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
  }

  // 8. states errors: missing states, empty states, missing mds_type, wrong
  // mds_type, missing num_clus, wrong num_clus, missing clus_config, empty
  // clus_config, missing intgr_time, wrong intgr_time, short clus_config.
  for (int stage = 0; stage <= 10; ++stage) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_NLC_1P", 1));
    constexpr long kOneDim[] = {1};
    CodaMemNode* mds = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mds, "rad_units_flag", prod.MakeInt8(-1)));
    CodaMemNode* elems[] = {mds};
    ASSERT_TRUE(prod.AddField(prod.root(), "nadir",
                              prod.MakeNodeArray(kOneDim, elems)));

    std::array<int8_t, 64> filter{};
    filter[0] = -1;
    filter[1] = -1;
    constexpr long kFilterDims[] = {64};
    CodaMemNode* cal_rec = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(cal_rec, "nadir_cluster_flag",
                              prod.MakeInt8Array(kFilterDims, filter)));
    CodaMemNode* cal[] = {cal_rec};
    ASSERT_TRUE(prod.AddField(prod.root(), "cal_options",
                              prod.MakeNodeArray(kOneDim, cal)));

    if (stage == 1) {
      constexpr long kZeroDim[] = {0};
      ASSERT_TRUE(
          prod.AddField(prod.root(), "states",
                        prod.MakeNodeArray(kZeroDim, {}, prod.MakeRecord())));
    } else if (stage >= 2) {
      CodaMemNode* st = prod.MakeRecord();
      if (stage == 3) {
        ASSERT_TRUE(prod.AddField(st, "mds_type", prod.MakeString("bad")));
      } else if (stage >= 4) {
        ASSERT_TRUE(prod.AddField(st, "mds_type", prod.MakeUint8(1)));
      }
      if (stage == 5) {
        ASSERT_TRUE(prod.AddField(st, "num_clus", prod.MakeString("bad")));
      } else if (stage >= 6) {
        ASSERT_TRUE(prod.AddField(st, "num_clus",
                                  prod.MakeUint16(stage == 10 ? 2 : 1)));
      }
      if (stage == 7) {
        constexpr long kZeroDim[] = {0};
        ASSERT_TRUE(
            prod.AddField(st, "clus_config",
                          prod.MakeNodeArray(kZeroDim, {}, prod.MakeRecord())));
      } else if (stage >= 8) {
        CodaMemNode* cc = prod.MakeRecord();
        if (stage == 9) {
          ASSERT_TRUE(prod.AddField(cc, "intgr_time", prod.MakeString("bad")));
        } else if (stage >= 10) {
          ASSERT_TRUE(prod.AddField(cc, "intgr_time", prod.MakeDouble(0.5)));
        }
        CodaMemNode* cc_arr[] = {cc};
        ASSERT_TRUE(prod.AddField(st, "clus_config",
                                  prod.MakeNodeArray(kOneDim, cc_arr)));
      }
      CodaMemNode* st_arr[] = {st};
      ASSERT_TRUE(prod.AddField(prod.root(), "states",
                                prod.MakeNodeArray(kOneDim, st_arr)));
    }
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
  }

  // 9. Second nadir loop errors: missing num_obs, wrong num_obs, missing
  // num_pixels, wrong num_pixels, short nadir array.
  for (int stage = 0; stage < 5; ++stage) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_NLC_1P", 1));
    constexpr long kOneDim[] = {1};

    std::array<int8_t, 64> filter{};
    filter[0] = -1;
    constexpr long kFilterDims[] = {64};
    CodaMemNode* cal_rec = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(cal_rec, "nadir_cluster_flag",
                              prod.MakeInt8Array(kFilterDims, filter)));
    CodaMemNode* cal[] = {cal_rec};
    ASSERT_TRUE(prod.AddField(prod.root(), "cal_options",
                              prod.MakeNodeArray(kOneDim, cal)));

    CodaMemNode* cc_arr[] = {MakeClusConfig(prod, 1, 0.5)};
    CodaMemNode* st_arr[] = {MakeStateRecord(prod, 1, 10.0, 1, cc_arr)};
    ASSERT_TRUE(prod.AddField(prod.root(), "states",
                              prod.MakeNodeArray(kOneDim, st_arr)));

    CodaMemNode* mds = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mds, "rad_units_flag", prod.MakeInt8(-1)));
    if (stage == 1) {
      ASSERT_TRUE(prod.AddField(mds, "num_obs", prod.MakeString("bad")));
    } else if (stage >= 2) {
      ASSERT_TRUE(prod.AddField(mds, "num_obs", prod.MakeUint16(1)));
    }
    if (stage == 3) {
      ASSERT_TRUE(prod.AddField(mds, "num_pixels", prod.MakeString("bad")));
    } else if (stage == 4) {
      ASSERT_TRUE(prod.AddField(mds, "num_pixels", prod.MakeUint16(1)));
    }
    CodaMemNode* mds_arr[] = {mds};
    ASSERT_TRUE(prod.AddField(prod.root(), "nadir",
                              prod.MakeNodeArray(kOneDim, mds_arr)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
  }

  // 10. Sun reference init errors: missing, unavailable, empty, missing
  // sun_spect_id, wrong type sun_spect_id.
  for (int stage = 0; stage < 5; ++stage) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_NLC_1P", 1));
    constexpr long kOneDim[] = {1};
    if (stage == 1) {
      ASSERT_TRUE(prod.AddUnavailableField(prod.root(), "sun_reference"));
    } else if (stage == 2) {
      constexpr long kZeroDim[] = {0};
      ASSERT_TRUE(
          prod.AddField(prod.root(), "sun_reference",
                        prod.MakeNodeArray(kZeroDim, {}, prod.MakeRecord())));
    } else if (stage == 3 || stage == 4) {
      CodaMemNode* srec = prod.MakeRecord();
      if (stage == 4) {
        ASSERT_TRUE(prod.AddField(srec, "sun_spect_id", prod.MakeInt32(1)));
      }
      CodaMemNode* s_arr[] = {srec};
      ASSERT_TRUE(prod.AddField(prod.root(), "sun_reference",
                                prod.MakeNodeArray(kOneDim, s_arr)));
    }
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "data=sun_reference");
    EXPECT_EQ(-1, res.status);
  }
}

TEST(HarpIngestSciamachyL1Test, VariableReaderErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("SCIAMACHY_L1c");
  ASSERT_NE(nullptr, mod);

  // 1. Product passes init_nadir_limb_occultation_dimensions, but lacks mph,
  // geo, observations, pixel_wavelength, dsr_time, num_rep_geo, clus_len.
  // Setting rad_units_flag = 0 on element 1 also covers get_spectral_data's
  // mixed-units check after element 0 passed init.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_NLC_1P", 1));
    constexpr long kOneDim[] = {1};

    std::array<int8_t, 64> filter{};
    filter[0] = -1;
    constexpr long kFilterDims[] = {64};
    CodaMemNode* cal_rec = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(cal_rec, "nadir_cluster_flag",
                              prod.MakeInt8Array(kFilterDims, filter)));
    CodaMemNode* cal[] = {cal_rec};
    ASSERT_TRUE(prod.AddField(prod.root(), "cal_options",
                              prod.MakeNodeArray(kOneDim, cal)));

    // Two states so get_datetime_start_data/get_datetime_length_data/
    // get_integration_time fail mid-loop and/or skip non-matching state.
    CodaMemNode* cc_rec = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(cc_rec, "intgr_time", prod.MakeDouble(0.5)));
    CodaMemNode* cc_arr[] = {cc_rec};
    CodaMemNode* st0 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(st0, "mds_type", prod.MakeUint8(1)));
    ASSERT_TRUE(prod.AddField(st0, "num_clus", prod.MakeUint16(1)));
    ASSERT_TRUE(
        prod.AddField(st0, "clus_config", prod.MakeNodeArray(kOneDim, cc_arr)));
    CodaMemNode* st_arr[] = {st0};
    ASSERT_TRUE(prod.AddField(prod.root(), "states",
                              prod.MakeNodeArray(kOneDim, st_arr)));

    CodaMemNode* m0 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(m0, "rad_units_flag", prod.MakeInt8(-1)));
    ASSERT_TRUE(prod.AddField(m0, "num_obs", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(m0, "num_pixels", prod.MakeUint16(1)));
    CodaMemNode* m1 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(m1, "rad_units_flag", prod.MakeInt8(0)));
    ASSERT_TRUE(prod.AddField(m1, "num_obs", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(m1, "num_pixels", prod.MakeUint16(1)));
    CodaMemNode* mds_arr[] = {m0, m1};
    constexpr long kTwoDim[] = {2};
    ASSERT_TRUE(prod.AddField(prod.root(), "nadir",
                              prod.MakeNodeArray(kTwoDim, mds_arr)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "data=nadir", /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_length"].read_status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["longitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["wavelength_photon_radiance"].read_status);
    EXPECT_EQ(-1, res.variables["wavelength"].read_status);
    EXPECT_EQ(-1, res.variables["integration_time"].read_status);
    EXPECT_EQ(-1, res.variables["scan_direction_type"].read_status);
  }

  // 2. get_spectral_data mixed rad_units_flag != -1 error on cluster 0 after
  // init succeeded (not possible via init except on 2nd cluster, so test with
  // 2 clusters where cluster 0 has rad_units_flag = -1 and cluster 1 has 0).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_NLC_1P", 1));
    constexpr long kOneDim[] = {1};

    std::array<int8_t, 64> filter{};
    filter[0] = -1;
    filter[1] = -1;
    constexpr long kFilterDims[] = {64};
    CodaMemNode* cal_rec = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(cal_rec, "nadir_cluster_flag",
                              prod.MakeInt8Array(kFilterDims, filter)));
    CodaMemNode* cal[] = {cal_rec};
    ASSERT_TRUE(prod.AddField(prod.root(), "cal_options",
                              prod.MakeNodeArray(kOneDim, cal)));

    CodaMemNode* cc_arr[] = {MakeClusConfig(prod, 1, 0.5),
                             MakeClusConfig(prod, 1, 0.5)};
    CodaMemNode* st_arr[] = {MakeStateRecord(prod, 1, 10.0, 1, cc_arr)};
    ASSERT_TRUE(prod.AddField(prod.root(), "states",
                              prod.MakeNodeArray(kOneDim, st_arr)));

    const double obs[] = {1.0};
    const double wvl[] = {300.0};
    CodaMemNode* geo0[] = {MakeGeoRecord(prod, 1.0, false)};
    CodaMemNode* geo1[] = {MakeGeoRecord(prod, 1.0, false)};
    CodaMemNode* geo2[] = {MakeGeoRecord(prod, 1.0, false)};
    CodaMemNode* mds_arr[] = {
        MakeMdsRecord(prod, -1, 1, 1, obs, wvl, geo0),
        MakeMdsRecord(prod, 0, 1, 1, obs, wvl, geo1),
        MakeMdsRecord(prod, -1, 1, 1, obs, wvl, geo2),
    };
    constexpr long kThreeDim[] = {3};
    ASSERT_TRUE(prod.AddField(prod.root(), "nadir",
                              prod.MakeNodeArray(kThreeDim, mds_arr)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "data=nadir", /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["wavelength_photon_radiance"].read_status);
  }

  // 3. read_scan_direction_type out-of-range block index and missing
  // corner_coord fields.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateSciamachyProduct(prod, "nadir", 1);

    harp_ingestion_options* raw_opts = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_ingestion_options_new(&raw_opts));
    ScopedHarpIngestionOptions opts(raw_opts);
    harp_product_definition* prod_def = nullptr;
    void* user_data = nullptr;
    ASSERT_EQ(HARP_SUCCESS, mod->ingestion_init(mod, prod.product(), opts.get(),
                                                &prod_def, &user_data));
    auto done_guard =
        absl::MakeCleanup([&] { mod->ingestion_done(user_data); });

    const harp_variable_definition* scan_def = nullptr;
    for (int i = 0; i < prod_def->num_variable_definitions; ++i) {
      if (std::string_view(prod_def->variable_definition[i]->name) ==
          "scan_direction_type") {
        scan_def = prod_def->variable_definition[i];
        break;
      }
    }
    ASSERT_NE(nullptr, scan_def);
    int8_t val = -1;
    harp_array data{};
    data.int8_data = &val;
    EXPECT_EQ(-1, scan_def->read_block(user_data, 999, data));
    EXPECT_EQ(HARP_ERROR_INGESTION, harp_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 4. Sun reference missing data arrays -> read_wavelength_photon_irradiance,
  // uncertainty, and wavelength fail.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_NLC_1P", 1));
    CodaMemNode* srec = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(srec, "sun_spect_id", prod.MakeString("D1")));
    constexpr long kOneDim[] = {1};
    CodaMemNode* s_arr[] = {srec};
    ASSERT_TRUE(prod.AddField(prod.root(), "sun_reference",
                              prod.MakeNodeArray(kOneDim, s_arr)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "data=sun_reference", /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["wavelength_photon_irradiance"].read_status);
    EXPECT_EQ(
        -1,
        res.variables["wavelength_photon_irradiance_uncertainty"].read_status);
    EXPECT_EQ(-1, res.variables["wavelength"].read_status);
  }

  // 5. Sun reference with mean_ref_spec present but missing
  // rad_pre_mean_sun_ref_spec, plus wrong-type mean_ref_spec.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_NLC_1P", 1));
    CodaMemNode* srec = prod.MakeRecord();
    std::vector<double> mean_ref(kMaxPixels, 1.0);
    constexpr long kPixDims[] = {kMaxPixels};
    ASSERT_TRUE(prod.AddField(srec, "sun_spect_id", prod.MakeString("D1")));
    ASSERT_TRUE(prod.AddField(srec, "mean_ref_spec",
                              prod.MakeDoubleArray(kPixDims, mean_ref)));
    constexpr long kOneDim[] = {1};
    CodaMemNode* s_arr[] = {srec};
    ASSERT_TRUE(prod.AddField(prod.root(), "sun_reference",
                              prod.MakeNodeArray(kOneDim, s_arr)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "data=sun_reference", /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_SUCCESS,
              res.variables["wavelength_photon_irradiance"].read_status);
    EXPECT_EQ(
        -1,
        res.variables["wavelength_photon_irradiance_uncertainty"].read_status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_NLC_1P", 1));
    CodaMemNode* srec = prod.MakeRecord();
    constexpr long kOneDim[] = {1};
    constexpr std::string_view kBadStr[] = {"bad"};
    ASSERT_TRUE(prod.AddField(srec, "sun_spect_id", prod.MakeString("D1")));
    ASSERT_TRUE(prod.AddField(srec, "mean_ref_spec",
                              prod.MakeStringArray(kOneDim, kBadStr)));
    CodaMemNode* s_arr[] = {srec};
    ASSERT_TRUE(prod.AddField(prod.root(), "sun_reference",
                              prod.MakeNodeArray(kOneDim, s_arr)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "data=sun_reference", /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["wavelength_photon_irradiance"].read_status);
  }

  // 6. Detailed reader errors: wrong types for dsr_time, num_rep_geo, clus_len,
  // abs_orbit, observations (scalar and string array), pixel_wavelength (string
  // array), unavailable rad_units_flag on 2nd cluster, and get_main_data
  // sub-field errors (empty geo, missing/short/wrong-type geo fields).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_NLC_1P", 1));
    constexpr long kOneDim[] = {1};
    constexpr long kTwoDim[] = {2};

    CodaMemNode* mph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mph, "abs_orbit", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "mph", mph));

    std::array<int8_t, 64> filter{};
    filter[0] = -1;
    filter[1] = -1;
    constexpr long kFilterDims[] = {64};
    CodaMemNode* cal_rec = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(cal_rec, "nadir_cluster_flag",
                              prod.MakeInt8Array(kFilterDims, filter)));
    CodaMemNode* cal[] = {cal_rec};
    ASSERT_TRUE(prod.AddField(prod.root(), "cal_options",
                              prod.MakeNodeArray(kOneDim, cal)));

    CodaMemNode* cc0 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(cc0, "intgr_time", prod.MakeDouble(0.5)));
    ASSERT_TRUE(prod.AddField(cc0, "clus_len", prod.MakeString("bad")));
    CodaMemNode* cc1 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(cc1, "intgr_time", prod.MakeDouble(0.5)));
    ASSERT_TRUE(prod.AddField(cc1, "clus_len", prod.MakeString("bad")));
    CodaMemNode* cc_arr[] = {cc0, cc1};

    CodaMemNode* st0 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(st0, "mds_type", prod.MakeUint8(1)));
    ASSERT_TRUE(prod.AddField(st0, "num_clus", prod.MakeUint16(2)));
    ASSERT_TRUE(prod.AddField(st0, "dsr_time", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(st0, "num_rep_geo", prod.MakeString("bad")));
    ASSERT_TRUE(
        prod.AddField(st0, "clus_config", prod.MakeNodeArray(kTwoDim, cc_arr)));
    CodaMemNode* st_arr[] = {st0};
    ASSERT_TRUE(prod.AddField(prod.root(), "states",
                              prod.MakeNodeArray(kOneDim, st_arr)));

    // Empty geo array -> get_main_data fails at goto_first_array_element.
    constexpr long kZeroDim[] = {0};
    constexpr std::string_view kBadStr[] = {"bad"};
    CodaMemNode* m0 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(m0, "rad_units_flag", prod.MakeInt8(-1)));
    ASSERT_TRUE(prod.AddField(m0, "num_obs", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(m0, "num_pixels", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(m0, "observations", prod.MakeDouble(1.0)));
    ASSERT_TRUE(prod.AddField(m0, "pixel_wavelength",
                              prod.MakeStringArray(kOneDim, kBadStr)));
    ASSERT_TRUE(prod.AddField(
        m0, "geo", prod.MakeNodeArray(kZeroDim, {}, prod.MakeRecord())));

    CodaMemNode* m1 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddUnavailableField(m1, "rad_units_flag"));
    ASSERT_TRUE(prod.AddField(m1, "num_obs", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(m1, "num_pixels", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(m1, "observations", prod.MakeDouble(1.0)));
    ASSERT_TRUE(prod.AddField(m1, "pixel_wavelength",
                              prod.MakeStringArray(kOneDim, kBadStr)));
    ASSERT_TRUE(prod.AddField(
        m1, "geo", prod.MakeNodeArray(kZeroDim, {}, prod.MakeRecord())));

    CodaMemNode* m2 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(m2, "rad_units_flag", prod.MakeInt8(-1)));
    ASSERT_TRUE(prod.AddField(m2, "num_obs", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(m2, "num_pixels", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(m2, "observations", prod.MakeDouble(1.0)));
    ASSERT_TRUE(prod.AddField(m2, "pixel_wavelength",
                              prod.MakeStringArray(kOneDim, kBadStr)));
    ASSERT_TRUE(prod.AddField(
        m2, "geo", prod.MakeNodeArray(kZeroDim, {}, prod.MakeRecord())));

    CodaMemNode* mds_arr[] = {m0, m1, m2};
    constexpr long kThreeDim[] = {3};
    ASSERT_TRUE(prod.AddField(prod.root(), "nadir",
                              prod.MakeNodeArray(kThreeDim, mds_arr)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "data=nadir", /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_length"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["wavelength_photon_radiance"].read_status);
    EXPECT_EQ(-1, res.variables["wavelength"].read_status);
    EXPECT_EQ(-1, res.variables["integration_time"].read_status);
    EXPECT_EQ(-1, res.variables["scan_direction_type"].read_status);
  }

  // 7. get_datetime_start_data missing num_rep_geo (with valid dsr_time),
  // get_spectral_data 2D string observations, get_main_data missing
  // sub-records / short arrays / wrong scalar types, and scan_direction_type
  // corner_coord error paths.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_NLC_1P", 1));
    constexpr long kOneDim[] = {1};

    std::array<int8_t, 64> filter{};
    filter[0] = -1;
    constexpr long kFilterDims[] = {64};
    CodaMemNode* cal_rec = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(cal_rec, "nadir_cluster_flag",
                              prod.MakeInt8Array(kFilterDims, filter)));
    CodaMemNode* cal[] = {cal_rec};
    ASSERT_TRUE(prod.AddField(prod.root(), "cal_options",
                              prod.MakeNodeArray(kOneDim, cal)));

    CodaMemNode* cc_arr[] = {MakeClusConfig(prod, 1, 0.5)};
    CodaMemNode* st0 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(st0, "mds_type", prod.MakeUint8(1)));
    ASSERT_TRUE(prod.AddField(st0, "num_clus", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(st0, "dsr_time", prod.MakeDouble(10.0)));
    ASSERT_TRUE(
        prod.AddField(st0, "clus_config", prod.MakeNodeArray(kOneDim, cc_arr)));
    CodaMemNode* st_arr[] = {st0};
    ASSERT_TRUE(prod.AddField(prod.root(), "states",
                              prod.MakeNodeArray(kOneDim, st_arr)));

    // geo[0] has wrong scalar type for sat_h, 1-element sol_zen_ang (missing
    // index 1), 1-element corner_coord (so corner_coord[0..3] fails in both
    // get_main_data and read_scan_direction_type), missing center_coord.
    CodaMemNode* geo0 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(geo0, "sat_h", prod.MakeString("bad")));
    const double one_ang[] = {30.0};
    ASSERT_TRUE(prod.AddField(geo0, "sol_zen_ang",
                              prod.MakeDoubleArray(kOneDim, one_ang)));
    CodaMemNode* one_corner[] = {MakeCoord(prod, 0.0, 0.0)};
    ASSERT_TRUE(prod.AddField(geo0, "corner_coord",
                              prod.MakeNodeArray(kOneDim, one_corner)));
    CodaMemNode* geo_arr0[] = {geo0};
    CodaMemNode* geo_arr1[] = {prod.MakeRecord()};

    constexpr long kObsDims[] = {1, 1};
    constexpr std::string_view kBadObs[] = {"bad"};
    CodaMemNode* m0 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(m0, "rad_units_flag", prod.MakeInt8(-1)));
    ASSERT_TRUE(prod.AddField(m0, "num_obs", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(m0, "num_pixels", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(m0, "observations",
                              prod.MakeStringArray(kObsDims, kBadObs)));
    ASSERT_TRUE(
        prod.AddField(m0, "geo", prod.MakeNodeArray(kOneDim, geo_arr0)));

    CodaMemNode* m1 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(m1, "rad_units_flag", prod.MakeInt8(-1)));
    ASSERT_TRUE(prod.AddField(m1, "num_obs", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(m1, "num_pixels", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(m1, "observations",
                              prod.MakeStringArray(kObsDims, kBadObs)));
    ASSERT_TRUE(
        prod.AddField(m1, "geo", prod.MakeNodeArray(kOneDim, geo_arr1)));

    CodaMemNode* mds_arr[] = {m0, m1};
    constexpr long kTwoDim[] = {2};
    ASSERT_TRUE(prod.AddField(prod.root(), "nadir",
                              prod.MakeNodeArray(kTwoDim, mds_arr)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "data=nadir", /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_altitude"].read_status);
    EXPECT_EQ(-1, res.variables["solar_zenith_angle"].read_status);
    EXPECT_EQ(-1, res.variables["solar_azimuth_angle"].read_status);
    EXPECT_EQ(-1, res.variables["wavelength_photon_radiance"].read_status);
    EXPECT_EQ(-1, res.variables["scan_direction_type"].read_status);
  }

  // 7b. get_datetime_start_data wrong-type num_rep_geo (with valid dsr_time)
  // and get_spectral_data unavailable rad_units_flag on 2nd cluster (with
  // valid 2D observations on 1st cluster).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_NLC_1P", 1));
    constexpr long kOneDim[] = {1};
    constexpr long kTwoDim[] = {2};

    std::array<int8_t, 64> filter{};
    filter[0] = -1;
    filter[1] = -1;
    constexpr long kFilterDims[] = {64};
    CodaMemNode* cal_rec = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(cal_rec, "nadir_cluster_flag",
                              prod.MakeInt8Array(kFilterDims, filter)));
    CodaMemNode* cal[] = {cal_rec};
    ASSERT_TRUE(prod.AddField(prod.root(), "cal_options",
                              prod.MakeNodeArray(kOneDim, cal)));

    CodaMemNode* cc_arr[] = {MakeClusConfig(prod, 1, 0.5),
                             MakeClusConfig(prod, 1, 0.5)};
    CodaMemNode* st0 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(st0, "mds_type", prod.MakeUint8(1)));
    ASSERT_TRUE(prod.AddField(st0, "num_clus", prod.MakeUint16(2)));
    ASSERT_TRUE(prod.AddField(st0, "dsr_time", prod.MakeDouble(10.0)));
    ASSERT_TRUE(st0 != nullptr);
    ASSERT_TRUE(prod.AddField(st0, "num_rep_geo", prod.MakeString("bad")));
    ASSERT_TRUE(
        prod.AddField(st0, "clus_config", prod.MakeNodeArray(kTwoDim, cc_arr)));
    CodaMemNode* st_arr[] = {st0};
    ASSERT_TRUE(prod.AddField(prod.root(), "states",
                              prod.MakeNodeArray(kOneDim, st_arr)));

    constexpr long kObsDims[] = {1, 1};
    const double obs_vals[] = {1.0};
    CodaMemNode* m0 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(m0, "rad_units_flag", prod.MakeInt8(-1)));
    ASSERT_TRUE(prod.AddField(m0, "num_obs", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(m0, "num_pixels", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(m0, "observations",
                              prod.MakeDoubleArray(kObsDims, obs_vals)));

    CodaMemNode* m1 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddUnavailableField(m1, "rad_units_flag"));
    ASSERT_TRUE(prod.AddField(m1, "num_obs", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(m1, "num_pixels", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(m1, "observations",
                              prod.MakeDoubleArray(kObsDims, obs_vals)));

    CodaMemNode* m2 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(m2, "rad_units_flag", prod.MakeInt8(-1)));
    ASSERT_TRUE(prod.AddField(m2, "num_obs", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(m2, "num_pixels", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(m2, "observations",
                              prod.MakeDoubleArray(kObsDims, obs_vals)));

    CodaMemNode* mds_arr[] = {m0, m1, m2};
    constexpr long kThreeDim[] = {3};
    ASSERT_TRUE(prod.AddField(prod.root(), "nadir",
                              prod.MakeNodeArray(kThreeDim, mds_arr)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "data=nadir", /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(-1, res.variables["wavelength_photon_radiance"].read_status);
  }

  // 8. read_scan_direction_type missing corner_coord, empty corner_coord,
  // missing/wrong-type latitude and longitude on corner_coord[0].
  for (int stage = 0; stage < 6; ++stage) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_NLC_1P", 1));
    constexpr long kOneDim[] = {1};

    std::array<int8_t, 64> filter{};
    filter[0] = -1;
    constexpr long kFilterDims[] = {64};
    CodaMemNode* cal_rec = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(cal_rec, "nadir_cluster_flag",
                              prod.MakeInt8Array(kFilterDims, filter)));
    CodaMemNode* cal[] = {cal_rec};
    ASSERT_TRUE(prod.AddField(prod.root(), "cal_options",
                              prod.MakeNodeArray(kOneDim, cal)));

    CodaMemNode* cc_arr[] = {MakeClusConfig(prod, 1, 0.5)};
    CodaMemNode* st_arr[] = {MakeStateRecord(prod, 1, 10.0, 1, cc_arr)};
    ASSERT_TRUE(prod.AddField(prod.root(), "states",
                              prod.MakeNodeArray(kOneDim, st_arr)));

    CodaMemNode* geo0 = prod.MakeRecord();
    if (stage == 1) {
      constexpr long kZeroDim[] = {0};
      ASSERT_TRUE(
          prod.AddField(geo0, "corner_coord",
                        prod.MakeNodeArray(kZeroDim, {}, prod.MakeRecord())));
    } else if (stage >= 2) {
      CodaMemNode* c0 = prod.MakeRecord();
      if (stage == 3) {
        ASSERT_TRUE(prod.AddField(c0, "latitude", prod.MakeString("bad")));
      } else if (stage >= 4) {
        ASSERT_TRUE(prod.AddField(c0, "latitude", prod.MakeDouble(0.0)));
      }
      if (stage == 5) {
        ASSERT_TRUE(prod.AddField(c0, "longitude", prod.MakeString("bad")));
      }
      CodaMemNode* corners[] = {c0};
      ASSERT_TRUE(prod.AddField(geo0, "corner_coord",
                                prod.MakeNodeArray(kOneDim, corners)));
    }
    CodaMemNode* geo_arr[] = {geo0};

    CodaMemNode* m0 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(m0, "rad_units_flag", prod.MakeInt8(-1)));
    ASSERT_TRUE(prod.AddField(m0, "num_obs", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(m0, "num_pixels", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(m0, "geo", prod.MakeNodeArray(kOneDim, geo_arr)));
    CodaMemNode* m1 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(m1, "rad_units_flag", prod.MakeInt8(-1)));
    ASSERT_TRUE(prod.AddField(m1, "num_obs", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(m1, "num_pixels", prod.MakeUint16(1)));
    CodaMemNode* mds_arr[] = {m0, m1};
    constexpr long kTwoDim[] = {2};
    ASSERT_TRUE(prod.AddField(prod.root(), "nadir",
                              prod.MakeNodeArray(kTwoDim, mds_arr)));

    harp_ingestion_options* raw_opts = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_ingestion_options_new(&raw_opts));
    ScopedHarpIngestionOptions opts(raw_opts);
    harp_product_definition* prod_def = nullptr;
    void* user_data = nullptr;
    ASSERT_EQ(HARP_SUCCESS, mod->ingestion_init(mod, prod.product(), opts.get(),
                                                &prod_def, &user_data));
    auto done_guard =
        absl::MakeCleanup([&] { mod->ingestion_done(user_data); });

    const harp_variable_definition* scan_def = nullptr;
    for (int i = 0; i < prod_def->num_variable_definitions; ++i) {
      if (std::string_view(prod_def->variable_definition[i]->name) ==
          "scan_direction_type") {
        scan_def = prod_def->variable_definition[i];
        break;
      }
    }
    ASSERT_NE(nullptr, scan_def);
    int8_t val = -1;
    harp_array data{};
    data.int8_data = &val;
    EXPECT_EQ(-1, scan_def->read_block(user_data, 0, data));
  }
}

}  // namespace
}  // namespace stcorp_harp
