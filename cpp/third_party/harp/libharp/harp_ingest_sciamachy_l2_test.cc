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
// Unit tests for the SCIAMACHY Level-2 ingestion module
// (libharp/harp-ingest-sciamachy_l2.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-sciamachy_l2.c

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/stcorp_coda/libcoda/coda.h"
#include "third_party/stcorp_harp/google/util/coda_mem_product.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/ingest_test_util.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::DoubleNear;
using ::testing::ElementsAre;
using ::testing::IsNan;

struct NadirDatasetCase {
  std::string_view option_name;
  std::string_view species_var_prefix;
};

constexpr NadirDatasetCase kNadirDatasetCases[] = {
    {.option_name = "nad_uv0_o3", .species_var_prefix = "O3"},
    {.option_name = "nad_uv1_no2", .species_var_prefix = "NO2"},
    {.option_name = "nad_uv3_bro", .species_var_prefix = "BrO"},
    {.option_name = "nad_uv4_h2co", .species_var_prefix = "HCHO"},
    {.option_name = "nad_uv5_so2", .species_var_prefix = "SO2"},
    {.option_name = "nad_uv6_oclo", .species_var_prefix = "OClO"},
    {.option_name = "nad_uv7_so2", .species_var_prefix = "SO2"},
    {.option_name = "nad_uv8_h2o", .species_var_prefix = "H2O"},
    {.option_name = "nad_uv9_chocho", .species_var_prefix = "C2H2O2"},
    {.option_name = "nad_ir0_h2o", .species_var_prefix = "H2O"},
    {.option_name = "nad_ir1_ch4", .species_var_prefix = "CH4"},
    {.option_name = "nad_ir2_n2o", .species_var_prefix = "N2O"},
    {.option_name = "nad_ir3_co", .species_var_prefix = "CO"},
    {.option_name = "nad_ir4_co2", .species_var_prefix = "CO2"},
};

struct LimbDatasetCase {
  std::string_view option_name;
  std::string_view species_var_prefix;
};

constexpr LimbDatasetCase kLimbDatasetCases[] = {
    {.option_name = "lim_uv0_o3", .species_var_prefix = "O3"},
    {.option_name = "lim_uv1_no2", .species_var_prefix = "NO2"},
    {.option_name = "lim_uv3_bro", .species_var_prefix = "BrO"},
};

CodaMemNode* MakeCoordRecord(CodaMemProduct& prod, double lat, double lon) {
  CodaMemNode* rec = prod.MakeRecord();
  if (rec == nullptr || !prod.AddField(rec, "latitude", prod.MakeDouble(lat)) ||
      !prod.AddField(rec, "longitude", prod.MakeDouble(lon))) {
    return nullptr;
  }
  return rec;
}

CodaMemNode* MakeNadirGeoRecord(CodaMemProduct& prod, double dsr_time,
                                double integr_time, double cen_lat,
                                double cen_lon, const double corner_lats[4],
                                const double corner_lons[4], double sza_base,
                                double vza_base, double raa_base) {
  CodaMemNode* rec = prod.MakeRecord();
  if (rec == nullptr) {
    return nullptr;
  }
  if (!prod.AddField(rec, "dsr_time", prod.MakeDouble(dsr_time)) ||
      !prod.AddField(rec, "integr_time", prod.MakeDouble(integr_time)) ||
      !prod.AddField(rec, "cen_coor_nad",
                     MakeCoordRecord(prod, cen_lat, cen_lon))) {
    return nullptr;
  }
  CodaMemNode* corners[4] = {
      MakeCoordRecord(prod, corner_lats[0], corner_lons[0]),
      MakeCoordRecord(prod, corner_lats[1], corner_lons[1]),
      MakeCoordRecord(prod, corner_lats[2], corner_lons[2]),
      MakeCoordRecord(prod, corner_lats[3], corner_lons[3]),
  };
  constexpr long kCornerDims[] = {4};
  if (!prod.AddField(rec, "cor_coor_nad",
                     prod.MakeNodeArray(kCornerDims, corners))) {
    return nullptr;
  }
  constexpr long kAngleDims[] = {3};
  const double sza[3] = {sza_base, sza_base + 1.0, sza_base + 2.0};
  const double vza[3] = {vza_base, vza_base + 1.0, vza_base + 2.0};
  const double raa[3] = {raa_base, raa_base + 1.0, raa_base + 2.0};
  if (!prod.AddField(rec, "sol_zen_angle_toa",
                     prod.MakeDoubleArray(kAngleDims, sza)) ||
      !prod.AddField(rec, "los_zen_angle_toa",
                     prod.MakeDoubleArray(kAngleDims, vza)) ||
      !prod.AddField(rec, "rel_azi_angle_toa",
                     prod.MakeDoubleArray(kAngleDims, raa))) {
    return nullptr;
  }
  return rec;
}

CodaMemNode* MakeNadirMdsRecord(CodaMemProduct& prod, double dsr_time,
                                double integr_time, double vcd, double vcd_err,
                                int32_t flag) {
  CodaMemNode* rec = prod.MakeRecord();
  if (rec == nullptr) {
    return nullptr;
  }
  constexpr long kScalarArrDims[] = {1};
  const double vcd_arr[1] = {vcd};
  const double err_arr[1] = {vcd_err};
  if (!prod.AddField(rec, "dsr_time", prod.MakeDouble(dsr_time)) ||
      !prod.AddField(rec, "integr_time", prod.MakeDouble(integr_time)) ||
      !prod.AddField(rec, "vcd",
                     prod.MakeDoubleArray(kScalarArrDims, vcd_arr)) ||
      !prod.AddField(rec, "vcd_err",
                     prod.MakeDoubleArray(kScalarArrDims, err_arr)) ||
      !prod.AddField(rec, "flag_vcd_flags", prod.MakeInt32(flag))) {
    return nullptr;
  }
  return rec;
}

CodaMemNode* MakeCloudsAerosolRecord(CodaMemProduct& prod, double dsr_time,
                                     double integr_time, double cl_frac,
                                     double cl_top_pres, double cl_top_height,
                                     double aero_abso_ind) {
  CodaMemNode* rec = prod.MakeRecord();
  if (rec == nullptr) {
    return nullptr;
  }
  if (!prod.AddField(rec, "dsr_time", prod.MakeDouble(dsr_time)) ||
      !prod.AddField(rec, "integr_time", prod.MakeDouble(integr_time)) ||
      !prod.AddField(rec, "cl_frac", prod.MakeDouble(cl_frac)) ||
      !prod.AddField(rec, "cl_top_pres", prod.MakeDouble(cl_top_pres)) ||
      !prod.AddField(rec, "cl_top_height", prod.MakeDouble(cl_top_height)) ||
      !prod.AddField(rec, "aero_abso_ind", prod.MakeDouble(aero_abso_ind))) {
    return nullptr;
  }
  return rec;
}

void AddMphAbsOrbit(CodaMemProduct& prod, int32_t abs_orbit) {
  CodaMemNode* mph = prod.MakeRecord();
  ASSERT_NE(nullptr, mph);
  ASSERT_TRUE(prod.AddField(mph, "abs_orbit", prod.MakeInt32(abs_orbit)));
  ASSERT_TRUE(prod.AddField(prod.root(), "mph", mph));
}

// Populates a minimal valid nadir product with 1 MDS record (coadding_factor=1)
// and 1 geolocation_nadir / clouds_aerosol record.
void PopulateSimpleNadirProduct(CodaMemProduct& prod, int version,
                                std::string_view dataset_name,
                                double mds_time = 100.0,
                                double geo_time = 100.0) {
  ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", version));
  AddMphAbsOrbit(prod, 4242);

  constexpr double kLat[4] = {0.0, 0.0, 10.0, 10.0};
  constexpr double kLon[4] = {0.0, 10.0, 0.0, 10.0};
  CodaMemNode* geo[1] = {MakeNadirGeoRecord(prod, geo_time, 0.25, 5.0, 5.0,
                                            kLat, kLon, 30.0, 10.0, 40.0)};
  constexpr long kOneDim[] = {1};
  ASSERT_TRUE(prod.AddField(prod.root(), "geolocation_nadir",
                            prod.MakeNodeArray(kOneDim, geo)));

  CodaMemNode* ca[1] = {
      MakeCloudsAerosolRecord(prod, geo_time, 0.25, 0.35, 500.0, 5.5, 1.25)};
  ASSERT_TRUE(prod.AddField(prod.root(), "clouds_aerosol",
                            prod.MakeNodeArray(kOneDim, ca)));

  if (dataset_name != "clouds_aerosol") {
    CodaMemNode* mds[1] = {
        MakeNadirMdsRecord(prod, mds_time, 0.25, 8.0e18, 5.0, 3)};
    ASSERT_TRUE(prod.AddField(prod.root(), dataset_name,
                              prod.MakeNodeArray(kOneDim, mds)));
  }
}

CodaMemNode* MakeLimbGeoRecord(CodaMemProduct& prod, double dsr_time,
                               double lat, double lon, double sza, double vza,
                               double raa) {
  CodaMemNode* rec = prod.MakeRecord();
  if (rec == nullptr ||
      !prod.AddField(rec, "dsr_time", prod.MakeDouble(dsr_time))) {
    return nullptr;
  }
  CodaMemNode* tang[3] = {
      MakeCoordRecord(prod, lat - 1.0, lon - 1.0),
      MakeCoordRecord(prod, lat, lon),
      MakeCoordRecord(prod, lat + 1.0, lon + 1.0),
  };
  constexpr long kThreeDim[] = {3};
  if (!prod.AddField(rec, "tangent_coord",
                     prod.MakeNodeArray(kThreeDim, tang))) {
    return nullptr;
  }
  const double sza_arr[3] = {sza - 1.0, sza, sza + 1.0};
  const double vza_arr[3] = {vza - 1.0, vza, vza + 1.0};
  const double raa_arr[3] = {raa - 1.0, raa, raa + 1.0};
  if (!prod.AddField(rec, "sol_zen_angle_toa",
                     prod.MakeDoubleArray(kThreeDim, sza_arr)) ||
      !prod.AddField(rec, "los_zen_angle_toa",
                     prod.MakeDoubleArray(kThreeDim, vza_arr)) ||
      !prod.AddField(rec, "rel_azi_angle_toa",
                     prod.MakeDoubleArray(kThreeDim, raa_arr))) {
    return nullptr;
  }
  return rec;
}

// Builds one limb MDS record using packed binary arrays for main_species
// (required because get_profile_vmr_data uses 2D
// coda_cursor_goto_array_element).
CodaMemNode* MakeLimbMdsRecord(
    CodaMemProduct& prod, uint8_t n_main, uint16_t n_stvec, uint16_t n1,
    uint8_t n_meas, const std::vector<double>& meas_times, double integr_time,
    const std::vector<double>& heights, const std::vector<double>& pressures,
    const std::vector<double>& temps, const std::vector<double>& vmr_packed,
    const std::vector<double>& add_diag) {
  CodaMemNode* rec = prod.MakeRecord();
  if (rec == nullptr) {
    return nullptr;
  }
  if (!prod.AddField(rec, "n_main", prod.MakeUint8(n_main)) ||
      !prod.AddField(rec, "n_state_vec", prod.MakeUint16(n_stvec)) ||
      !prod.AddField(rec, "n1", prod.MakeUint16(n1)) ||
      !prod.AddField(rec, "n_meas", prod.MakeUint8(n_meas)) ||
      !prod.AddField(rec, "integr_time", prod.MakeDouble(integr_time))) {
    return nullptr;
  }

  std::vector<CodaMemNode*> mg_recs;
  mg_recs.reserve(meas_times.size());
  for (double t : meas_times) {
    CodaMemNode* mg = prod.MakeRecord();
    if (mg == nullptr || !prod.AddField(mg, "dsr_time", prod.MakeDouble(t))) {
      return nullptr;
    }
    mg_recs.push_back(mg);
  }
  const long mg_dims[1] = {static_cast<long>(mg_recs.size())};
  if (!prod.AddField(rec, "measurement_grid",
                     prod.MakeNodeArray(mg_dims, mg_recs))) {
    return nullptr;
  }

  const long vert_dims[1] = {static_cast<long>(n_main)};
  if (!prod.AddField(rec, "tangent_height",
                     prod.MakeDoubleArray(vert_dims, heights)) ||
      !prod.AddField(rec, "tangent_pressure",
                     prod.MakeDoubleArray(vert_dims, pressures)) ||
      !prod.AddField(rec, "tangent_temp",
                     prod.MakeDoubleArray(vert_dims, temps))) {
    return nullptr;
  }

  const long ms_dims[2] = {static_cast<long>(n_main), static_cast<long>(n1)};
  constexpr std::string_view kSpeciesFields[] = {"tang_vmr", "err_tang_vmr"};
  if (!prod.AddField(rec, "main_species",
                     prod.MakeBinaryDoubleRecordArray(ms_dims, kSpeciesFields,
                                                      vmr_packed))) {
    return nullptr;
  }

  const long diag_dims[1] = {static_cast<long>(add_diag.size())};
  if (!prod.AddField(rec, "add_diag",
                     prod.MakeDoubleArray(diag_dims, add_diag))) {
    return nullptr;
  }
  return rec;
}

TEST(HarpIngestSciamachyL2Test, AllNadirDatasetsRoundTrip) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("SCIAMACHY_L2");
  ASSERT_NE(nullptr, mod);

  for (const auto& tc : kNadirDatasetCases) {
    SCOPED_TRACE(tc.option_name);
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    // format_version=4 ensures all datasets (including 7, 8, 9) are populated.
    PopulateSimpleNadirProduct(prod, /*version=*/4, tc.option_name);

    const std::string opt = "dataset=" + std::string(tc.option_name);
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), opt.c_str());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(1, res.dimensions[harp_dimension_time]);
    EXPECT_THAT(res.variables["orbit_index"].int32_values, ElementsAre(4242));
    EXPECT_THAT(res.variables["datetime_start"].double_values,
                ElementsAre(100.0));
    EXPECT_THAT(res.variables["datetime_length"].double_values,
                ElementsAre(0.25));
    EXPECT_THAT(res.variables["latitude"].double_values, ElementsAre(5.0));
    EXPECT_THAT(res.variables["longitude"].double_values, ElementsAre(5.0));
    // Corners rearranged 0, 2, 3, 1.
    EXPECT_THAT(res.variables["latitude_bounds"].double_values,
                ElementsAre(0.0, 10.0, 10.0, 0.0));
    EXPECT_THAT(res.variables["longitude_bounds"].double_values,
                ElementsAre(0.0, 0.0, 10.0, 10.0));
    EXPECT_THAT(res.variables["solar_zenith_angle"].double_values,
                ElementsAre(31.0));
    EXPECT_THAT(res.variables["viewing_zenith_angle"].double_values,
                ElementsAre(11.0));
    EXPECT_THAT(res.variables["relative_azimuth_angle"].double_values,
                ElementsAre(41.0));
    EXPECT_THAT(res.variables["cloud_fraction"].double_values,
                ElementsAre(0.35));
    EXPECT_THAT(res.variables["scan_direction_type"].int8_values,
                ElementsAre(1));

    const std::string col_var =
        std::string(tc.species_var_prefix) + "_column_number_density";
    const std::string err_var = col_var + "_uncertainty";
    const std::string val_var = col_var + "_validity";
    EXPECT_THAT(res.variables[col_var].double_values, ElementsAre(8.0e18));
    // format_version >= 2: vcd_err is already relative fraction (5.0 * 8e18).
    EXPECT_THAT(res.variables[err_var].double_values, ElementsAre(4.0e19));
    EXPECT_THAT(res.variables[val_var].int32_values, ElementsAre(3));
  }
}

TEST(HarpIngestSciamachyL2Test, DefaultDatasetAndCoaddingModes) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("SCIAMACHY_L2");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod(coda_format_binary);
  ASSERT_TRUE(prod.ok());
  // format_version=1 exercises vcd_err percentage-to-fraction scaling (/
  // 100.0).
  ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", 1));
  AddMphAbsOrbit(prod, 777);

  // Build 11 geolocation/cloud DSRs:
  //   geo[0]: t=50 (skipped while searching for mds[0] at t=100)
  //   geo[1]: t=100, coadding=1, forward scan (lat/lon swapped at corners 1 &
  //   2) geo[2..5]: t=200..200.75, coadding=4, integr_time=1.0 (<= 1.0 branch)
  //   geo[6..10]: t=300..302.0, coadding=5, integr_time=2.5 (> 1.0 mixed
  //   branch)
  constexpr double kFwdLat[4] = {0.0, 10.0, 0.0, 10.0};
  constexpr double kFwdLon[4] = {0.0, 0.0, 10.0, 10.0};
  std::vector<CodaMemNode*> geo_recs;
  std::vector<CodaMemNode*> ca_recs;

  geo_recs.push_back(MakeNadirGeoRecord(prod, 50.0, 0.25, 0.0, 0.0, kFwdLat,
                                        kFwdLon, 10.0, 5.0, 15.0));
  ca_recs.push_back(
      MakeCloudsAerosolRecord(prod, 50.0, 0.25, 0.9, 100.0, 1.0, 0.1));

  geo_recs.push_back(MakeNadirGeoRecord(prod, 100.0, 0.25, 4.0, 6.0, kFwdLat,
                                        kFwdLon, 20.0, 12.0, 50.0));
  ca_recs.push_back(
      MakeCloudsAerosolRecord(prod, 100.0, 0.25, 0.2, 200.0, 2.0, 0.2));

  for (int k = 0; k < 4; ++k) {
    const double lats[4] = {1.0 + k, 2.0 + k, 3.0 + k, 4.0 + k};
    const double lons[4] = {10.0 + k, 11.0 + k, 12.0 + k, 13.0 + k};
    geo_recs.push_back(MakeNadirGeoRecord(
        prod, 200.0 + k * 0.25, 0.25, 20.0 + k, 30.0 + k, lats, lons,
        40.0 + k * 10.0, 20.0 + k * 5.0, 60.0 + k * 2.0));
    ca_recs.push_back(MakeCloudsAerosolRecord(prod, 200.0 + k * 0.25, 0.25,
                                              0.1 * (k + 1), 300.0, 3.0, 0.3));
  }

  for (int k = 0; k < 5; ++k) {
    const double lats[4] = {-5.0 + k, -4.0 + k, -3.0 + k, -2.0 + k};
    const double lons[4] = {50.0 + k, 51.0 + k, 52.0 + k, 53.0 + k};
    geo_recs.push_back(MakeNadirGeoRecord(prod, 300.0 + k * 0.5, 0.5, -10.0 + k,
                                          70.0 + k, lats, lons, 10.0 * (k + 1),
                                          5.0 * (k + 1), 3.0 * (k + 1)));
    ca_recs.push_back(MakeCloudsAerosolRecord(prod, 300.0 + k * 0.5, 0.5,
                                              0.2 * k, 400.0, 4.0, 0.4));
  }

  const long geo_dims[1] = {static_cast<long>(geo_recs.size())};
  ASSERT_TRUE(prod.AddField(prod.root(), "geolocation_nadir",
                            prod.MakeNodeArray(geo_dims, geo_recs)));
  ASSERT_TRUE(prod.AddField(prod.root(), "clouds_aerosol",
                            prod.MakeNodeArray(geo_dims, ca_recs)));

  CodaMemNode* mds[3] = {
      MakeNadirMdsRecord(prod, 100.0, 0.25, 500.0, 10.0, 1),
      MakeNadirMdsRecord(prod, 200.0, 1.0, 600.0, 20.0, 2),
      MakeNadirMdsRecord(prod, 300.0, 2.5, 700.0, 30.0, 3),
  };
  constexpr long kMdsDims[] = {3};
  ASSERT_TRUE(prod.AddField(prod.root(), "nad_uv0_o3",
                            prod.MakeNodeArray(kMdsDims, mds)));

  // Omit options to test default dataset ("nad_uv0_o3").
  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(3, res.dimensions[harp_dimension_time]);
  EXPECT_THAT(res.variables["scan_direction_type"].int8_values,
              ElementsAre(0, 1, 2));
  EXPECT_THAT(res.variables["O3_column_number_density"].double_values,
              ElementsAre(500.0, 600.0, 700.0));
  // format_version=1 divides percentage error by 100 before multiplying vcd.
  EXPECT_THAT(
      res.variables["O3_column_number_density_uncertainty"].double_values,
      ElementsAre(50.0, 120.0, 210.0));

  // Cloud fraction averages: [0.2, (0.1+0.2+0.3+0.4)/4=0.25,
  // (0+0.2+0.4+0.6+0.8)/5=0.4].
  ASSERT_EQ(3u, res.variables["cloud_fraction"].double_values.size());
  EXPECT_DOUBLE_EQ(0.2, res.variables["cloud_fraction"].double_values[0]);
  EXPECT_NEAR(0.25, res.variables["cloud_fraction"].double_values[1], 1e-12);
  EXPECT_NEAR(0.4, res.variables["cloud_fraction"].double_values[2], 1e-12);

  // Solar zenith angle:
  //   index 0 (coadd=1): geo[1].sza[1] = 21.0
  //   index 1 (coadd=4, int<=1): geo[2 + 4/2 - 1 = 3].sza[2] = 52.0
  //   index 2 (coadd=5, int>1): avg(geo[6 + 1 = 7].sza[2]=22.0, geo[6 + 4 =
  //   10].sza[1]=51.0) = 36.5
  EXPECT_THAT(res.variables["solar_zenith_angle"].double_values,
              ElementsAre(21.0, 52.0, 36.5));

  // Verify coadded latitude/longitude and bounds are finite and populated.
  ASSERT_EQ(3u, res.variables["latitude"].double_values.size());
  ASSERT_EQ(3u, res.variables["longitude"].double_values.size());
  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(std::isfinite(res.variables["latitude"].double_values[i]));
    EXPECT_TRUE(std::isfinite(res.variables["longitude"].double_values[i]));
  }
  EXPECT_THAT(
      res.variables["latitude_bounds"].double_values,
      ElementsAre(
          0.0, 0.0, 10.0, 10.0,  // index 0
          1.0, 6.0, 7.0, 2.0,    // index 1 (geo[2] 0,1 & geo[5] 2,3)
          -5.0, 0.0, 0.0,
          2.0));  // index 2 (geo[6][0], geo[9][2], geo[10][1], geo[10][3])
  EXPECT_THAT(res.variables["longitude_bounds"].double_values,
              ElementsAre(0.0, 10.0, 10.0, 0.0, 10.0, 15.0, 16.0, 11.0, 50.0,
                          55.0, 55.0, 57.0));
}

TEST(HarpIngestSciamachyL2Test, CloudsAerosolFormatVersions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("SCIAMACHY_L2");
  ASSERT_NE(nullptr, mod);

  // format_version < 2 includes cloud_top_pressure and excludes
  // cloud_top_height.
  {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    PopulateSimpleNadirProduct(prod, /*version=*/1, "clouds_aerosol");

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "dataset=clouds_aerosol");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_TRUE(res.variables["cloud_top_pressure"].included);
    EXPECT_FALSE(res.variables["cloud_top_height"].included);
    EXPECT_THAT(res.variables["cloud_top_pressure"].double_values,
                ElementsAre(500.0));
    EXPECT_THAT(res.variables["absorbing_aerosol_index"].double_values,
                ElementsAre(1.25));
  }

  // format_version >= 2 includes cloud_top_height and excludes
  // cloud_top_pressure.
  {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    PopulateSimpleNadirProduct(prod, /*version=*/2, "clouds_aerosol");

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "dataset=clouds_aerosol");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_FALSE(res.variables["cloud_top_pressure"].included);
    EXPECT_TRUE(res.variables["cloud_top_height"].included);
    EXPECT_THAT(res.variables["cloud_top_height"].double_values,
                ElementsAre(5.5));
  }
}

TEST(HarpIngestSciamachyL2Test, LegacyVersionsTreatNewerBandsAsEmpty) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("SCIAMACHY_L2");
  ASSERT_NE(nullptr, mod);

  struct VersionEmptyCase {
    int version;
    const char* opt;
  };
  constexpr VersionEmptyCase kCases[] = {
      {.version = 3, .opt = "dataset=nad_uv9_chocho"},
      {.version = 2, .opt = "dataset=nad_uv7_so2"},
      {.version = 2, .opt = "dataset=nad_uv8_h2o"},
  };

  for (const auto& tc : kCases) {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", tc.version));
    AddMphAbsOrbit(prod, 99);
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), tc.opt);
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(0, res.dimensions[harp_dimension_time]);
    EXPECT_THAT(res.variables["orbit_index"].int32_values, ElementsAre(99));
  }
}

TEST(HarpIngestSciamachyL2Test, NadirUnavailableAndEmptyDatasets) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("SCIAMACHY_L2");
  ASSERT_NE(nullptr, mod);

  // Unavailable nad_uv0_o3 field yields 0 time elements.
  {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", 4));
    AddMphAbsOrbit(prod, 11);
    ASSERT_TRUE(prod.AddUnavailableField(prod.root(), "nad_uv0_o3"));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(0, res.dimensions[harp_dimension_time]);
  }

  // Zero-length nad_uv0_o3 array yields 0 time elements.
  {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", 4));
    AddMphAbsOrbit(prod, 12);
    CodaMemNode* sample = MakeNadirMdsRecord(prod, 0.0, 0.25, 1.0, 0.1, 0);
    constexpr long kZeroDims[] = {0};
    ASSERT_TRUE(prod.AddField(prod.root(), "nad_uv0_o3",
                              prod.MakeNodeArray(kZeroDims, {}, sample)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(0, res.dimensions[harp_dimension_time]);
  }
}

TEST(HarpIngestSciamachyL2Test, AllLimbDatasetsWithExtendedDiagAndPadding) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("SCIAMACHY_L2");
  ASSERT_NE(nullptr, mod);

  for (const auto& tc : kLimbDatasetCases) {
    SCOPED_TRACE(tc.option_name);
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", 4));
    AddMphAbsOrbit(prod, 555);

    // 4 geolocation_limb records so we can test both forward and backward
    // geo_dsr_id search loops:
    //   geo[0]: t=10.0 (skipped for MDS 0)
    //   geo[1]: t=20.0 (matched by MDS 1 via geo_dsr_id-- from geo[3]!)
    //   geo[2]: t=30.0 (matched by MDS 0 via geo_dsr_id++)
    //   geo[3]: t=40.0 (initial candidate for MDS 1, > MDS 1's t=20.0)
    CodaMemNode* geo[4] = {
        MakeLimbGeoRecord(prod, 10.0, 10.0, 20.0, 30.0, 80.0, 90.0),
        MakeLimbGeoRecord(prod, 20.0, 15.0, 25.0, 35.0, 85.0, 95.0),
        MakeLimbGeoRecord(prod, 30.0, 12.0, 22.0, 32.0, 82.0, 92.0),
        MakeLimbGeoRecord(prod, 40.0, 18.0, 28.0, 38.0, 88.0, 98.0),
    };
    constexpr long kGeoDims[] = {4};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation_limb",
                              prod.MakeNodeArray(kGeoDims, geo)));

    // MDS 0: n_main=2, n_stvec=2, n1=2, n_meas=3 (middle index 3/2 = 1 ->
    // t=30.0). Required add_diag len = 2 + 2 + 2*2*2 + 2*2 + 2*2*2 = 24. Layout
    // of add_diag (24 elements):
    //   [0..3]: header + stvec
    //   [4..5]: nd = {1.0e12, 2.0e12}
    //   [6..7]: extra n_main
    //   [8..9]: nd_apriori = {0.8e12, 1.6e12}
    //   [10..11]: extra n_main
    //   [12..13]: conv_mix = {2.0, 4.0}
    //   [14..15]: conv_nd = {3.0, 6.0}
    //   [16..19]: avk_pc (2x2 row-major) = {0.5, 0.2, 0.4, 0.8}
    //   [20..23]: remaining
    std::vector<double> diag0(24, 0.0);
    diag0[4] = 1.0e12;
    diag0[5] = 2.0e12;
    diag0[8] = 0.8e12;
    diag0[9] = 1.6e12;
    diag0[12] = 2.0;
    diag0[13] = 4.0;
    diag0[14] = 3.0;
    diag0[15] = 6.0;
    diag0[16] = 0.5;
    diag0[17] = 0.2;
    diag0[18] = 0.4;
    diag0[19] = 0.8;

    // main_species dims={2, 2}, packed row-major:
    //   (0,0): tang_vmr=1e-6, err=10%
    //   (0,1): unused
    //   (1,0): tang_vmr=3e-6, err=20%
    //   (1,1): unused
    const std::vector<double> vmr0 = {1.0e-6, 10.0, 9.0, 9.0,
                                      3.0e-6, 20.0, 9.0, 9.0};
    CodaMemNode* mds0 = MakeLimbMdsRecord(
        prod, /*n_main=*/2, /*n_stvec=*/2, /*n1=*/2, /*n_meas=*/3,
        /*meas_times=*/{29.0, 30.0, 31.0}, /*integr_time=*/1.5,
        /*heights=*/{50.0, 20.0}, /*pressures=*/{1.0, 50.0},
        /*temps=*/{250.0, 220.0}, vmr0, diag0);

    // MDS 1: n_main=3 (> MDS 0's 2 -> max_num_vertical=3), n_stvec=2, n1=2,
    // n_meas=2 (middle index 2/2 = 1 -> t=20.0).
    // Required add_diag len = 2 + 2 + 2*3*2 + 2*3 + 2*3*3 = 42.
    std::vector<double> diag1(42, 0.0);
    diag1[4] = 1.0e11;
    diag1[5] = 2.0e11;
    diag1[6] = 3.0e11;
    diag1[10] = 0.5e11;
    diag1[11] = 1.5e11;
    diag1[12] = 2.5e11;
    diag1[16] = 1.0;
    diag1[17] = 2.0;
    diag1[18] = 4.0;
    diag1[19] = 1.0;
    diag1[20] = 2.0;
    diag1[21] = 4.0;
    for (int k = 0; k < 9; ++k) {
      diag1[22 + k] = 0.1 * (k + 1);
    }
    const std::vector<double> vmr1 = {
        2.0e-6, 5.0,  0.0, 0.0,  // (0,0), (0,1)
        4.0e-6, 10.0, 0.0, 0.0,  // (1,0), (1,1)
        6.0e-6, 15.0, 0.0, 0.0,  // (2,0), (2,1)
    };
    CodaMemNode* mds1 = MakeLimbMdsRecord(
        prod, /*n_main=*/3, /*n_stvec=*/2, /*n1=*/2, /*n_meas=*/2,
        /*meas_times=*/{19.0, 20.0}, /*integr_time=*/2.0,
        /*heights=*/{60.0, 40.0, 10.0}, /*pressures=*/{0.5, 5.0, 100.0},
        /*temps=*/{240.0, 230.0, 280.0}, vmr1, diag1);

    CodaMemNode* mds[2] = {mds0, mds1};
    constexpr long kMdsDims[] = {2};
    ASSERT_TRUE(prod.AddField(prod.root(), tc.option_name,
                              prod.MakeNodeArray(kMdsDims, mds)));

    const std::string opt = "dataset=" + std::string(tc.option_name);
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), opt.c_str());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(3, res.dimensions[harp_dimension_vertical]);
    EXPECT_THAT(res.variables["datetime_start"].double_values,
                ElementsAre(30.0, 20.0));
    EXPECT_THAT(res.variables["datetime_length"].double_values,
                ElementsAre(1.5, 2.0));
    EXPECT_THAT(res.variables["latitude"].double_values,
                ElementsAre(12.0, 15.0));
    EXPECT_THAT(res.variables["longitude"].double_values,
                ElementsAre(22.0, 25.0));
    EXPECT_THAT(res.variables["solar_zenith_angle"].double_values,
                ElementsAre(32.0, 35.0));
    EXPECT_THAT(res.variables["viewing_zenith_angle"].double_values,
                ElementsAre(82.0, 85.0));
    EXPECT_THAT(res.variables["relative_azimuth_angle"].double_values,
                ElementsAre(92.0, 95.0));

    EXPECT_THAT(res.variables["temperature"].double_values,
                ElementsAre(220.0, 250.0, IsNan(), 280.0, 230.0, 240.0));
    // Upstream read_altitude_bounds/read_pressure_bounds convert lower bounds
    // in place with i from num_vertical-1 down to 0, so at i=1 data[2*1]
    // overwrites data[i+1]=data[2] before reading data[2*1+1]=data[2] (pinning
    // 40 and 5).
    EXPECT_THAT(res.variables["altitude_bounds"].double_values,
                ElementsAre(20.0, 50.0, 50.0, 100.0, IsNan(), IsNan(), 10.0,
                            40.0, 40.0, 40.0, 60.0, 100.0));
    EXPECT_THAT(res.variables["pressure_bounds"].double_values,
                ElementsAre(50.0, 1.0, 1.0, 3.2e-4, IsNan(), IsNan(), 100.0,
                            5.0, 5.0, 5.0, 0.5, 3.2e-4));

    const std::string pfx(tc.species_var_prefix);
    const std::string vmr_var = pfx + "_volume_mixing_ratio";
    const std::string vmr_err_var = vmr_var + "_uncertainty";
    const std::string vmr_avk_var = vmr_var + "_avk";
    const std::string nd_var = pfx + "_number_density";
    const std::string nd_err_var = nd_var + "_uncertainty";
    const std::string nd_ap_var = nd_var + "_apriori";
    const std::string nd_avk_var = nd_var + "_avk";

    EXPECT_TRUE(res.variables[vmr_avk_var].included);
    EXPECT_TRUE(res.variables[nd_var].included);
    EXPECT_THAT(
        res.variables[vmr_var].double_values,
        ElementsAre(DoubleNear(3.0e-6, 1e-18), DoubleNear(1.0e-6, 1e-18),
                    IsNan(), DoubleNear(6.0e-6, 1e-18),
                    DoubleNear(4.0e-6, 1e-18), DoubleNear(2.0e-6, 1e-18)));
    EXPECT_THAT(
        res.variables[vmr_err_var].double_values,
        ElementsAre(DoubleNear(6.0e-7, 1e-18), DoubleNear(1.0e-7, 1e-18),
                    IsNan(), DoubleNear(9.0e-7, 1e-18),
                    DoubleNear(4.0e-7, 1e-18), DoubleNear(1.0e-7, 1e-18)));

    // Upstream read_nd/read_nd_apriori pad with reversed_i starting at
    // max_num_vertical - num_vertical - 1, overwriting index 0 with NaN when
    // num_vertical(2) < max_num_vertical(3) and leaving index 2 unchanged (0).
    EXPECT_THAT(res.variables[nd_var].double_values,
                ElementsAre(IsNan(), 1.0e12, 0.0, 3.0e11, 2.0e11, 1.0e11));
    EXPECT_THAT(res.variables[nd_ap_var].double_values,
                ElementsAre(IsNan(), 0.8e12, 0.0, 2.5e11, 1.5e11, 0.5e11));
    EXPECT_THAT(res.variables[nd_err_var].double_values,
                ElementsAre(IsNan(), DoubleNear(1.0e11, 1.0), IsNan(),
                            DoubleNear(4.5e10, 1.0), DoubleNear(2.0e10, 1.0),
                            DoubleNear(5.0e9, 1.0)));

    // For MDS 0, max_num_vertical=3 and num_vertical=2:
    // reversed_i=0 and reversed_j=0 are padded with NaN, while [1..2, 1..2]
    // hold scaled reversed AKM entries: conv[1]/conv[1]*avk[1,1]=0.8, etc.
    const auto& vmr_avk = res.variables[vmr_avk_var].double_values;
    ASSERT_EQ(18u, vmr_avk.size());
    EXPECT_TRUE(std::isnan(vmr_avk[0]));
    EXPECT_TRUE(std::isnan(vmr_avk[1]));
    EXPECT_TRUE(std::isnan(vmr_avk[3]));
    EXPECT_DOUBLE_EQ(0.8, vmr_avk[4]);
    EXPECT_DOUBLE_EQ(4.0 / 2.0 * 0.4, vmr_avk[5]);
    EXPECT_TRUE(std::isnan(vmr_avk[6]));
    EXPECT_DOUBLE_EQ(2.0 / 4.0 * 0.2, vmr_avk[7]);
    EXPECT_DOUBLE_EQ(0.5, vmr_avk[8]);

    const auto& nd_avk = res.variables[nd_avk_var].double_values;
    ASSERT_EQ(18u, nd_avk.size());
    EXPECT_DOUBLE_EQ(0.8, nd_avk[4]);
    EXPECT_DOUBLE_EQ(6.0 / 3.0 * 0.4, nd_avk[5]);
  }
}

TEST(HarpIngestSciamachyL2Test, LimbWithoutExtendedDiagAndEmptyLimb) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("SCIAMACHY_L2");
  ASSERT_NE(nullptr, mod);

  // 1. Short add_diag disables extended diagnostics.
  {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", 4));
    AddMphAbsOrbit(prod, 101);

    CodaMemNode* geo[1] = {
        MakeLimbGeoRecord(prod, 10.0, 1.0, 2.0, 3.0, 4.0, 5.0)};
    constexpr long kOneDim[] = {1};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation_limb",
                              prod.MakeNodeArray(kOneDim, geo)));

    const std::vector<double> short_diag = {1.0, 2.0};
    CodaMemNode* mds[1] = {MakeLimbMdsRecord(prod, 1, 2, 1, 1, {10.0}, 1.0,
                                             {30.0}, {10.0}, {240.0},
                                             {1e-6, 5.0}, short_diag)};
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_uv0_o3",
                              prod.MakeNodeArray(kOneDim, mds)));

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "dataset=lim_uv0_o3");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_FALSE(res.variables["O3_volume_mixing_ratio_avk"].included);
    EXPECT_FALSE(res.variables["O3_number_density"].included);
    EXPECT_FALSE(res.variables["O3_number_density_uncertainty"].included);
    EXPECT_FALSE(res.variables["O3_number_density_apriori"].included);
    EXPECT_FALSE(res.variables["O3_number_density_avk"].included);
  }

  // 2. Empty limb dataset (0 elements).
  {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", 4));
    AddMphAbsOrbit(prod, 102);
    CodaMemNode* sample =
        MakeLimbMdsRecord(prod, 1, 1, 1, 1, {0.0}, 1.0, {10.0}, {100.0},
                          {250.0}, {1e-6, 1.0}, {0.0});
    constexpr long kZeroDims[] = {0};
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_uv0_o3",
                              prod.MakeNodeArray(kZeroDims, {}, sample)));

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "dataset=lim_uv0_o3");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(0, res.dimensions[harp_dimension_time]);
  }
}

TEST(HarpIngestSciamachyL2Test, InitErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("SCIAMACHY_L2");
  ASSERT_NE(nullptr, mod);

  // 1. Missing product version (SetProductInfo not called).
  {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. Invalid dataset option value.
  {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "dataset=bad_dataset");
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INVALID_INGESTION_OPTION_VALUE, res.failed_errno);
  }

  // 3. Missing nad_uv0_o3 field.
  {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", 4));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 4. Scalar instead of array for nad_uv0_o3 (goto_first_array_element fails).
  {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", 4));
    ASSERT_TRUE(prod.AddField(prod.root(), "nad_uv0_o3", prod.MakeInt32(1)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 5. Missing geolocation_nadir / missing clouds_aerosol / mismatched counts.
  {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", 4));
    CodaMemNode* mds[1] = {MakeNadirMdsRecord(prod, 10.0, 0.25, 1.0, 1.0, 0)};
    constexpr long kOneDim[] = {1};
    ASSERT_TRUE(prod.AddField(prod.root(), "nad_uv0_o3",
                              prod.MakeNodeArray(kOneDim, mds)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", 4));
    CodaMemNode* mds[1] = {MakeNadirMdsRecord(prod, 10.0, 0.25, 1.0, 1.0, 0)};
    constexpr double kZero[4] = {0.0, 0.0, 0.0, 0.0};
    CodaMemNode* geo[1] = {MakeNadirGeoRecord(prod, 10.0, 0.25, 0.0, 0.0, kZero,
                                              kZero, 0.0, 0.0, 0.0)};
    constexpr long kOneDim[] = {1};
    ASSERT_TRUE(prod.AddField(prod.root(), "nad_uv0_o3",
                              prod.MakeNodeArray(kOneDim, mds)));
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation_nadir",
                              prod.MakeNodeArray(kOneDim, geo)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", 4));
    CodaMemNode* mds[1] = {MakeNadirMdsRecord(prod, 10.0, 0.25, 1.0, 1.0, 0)};
    constexpr double kZero[4] = {0.0, 0.0, 0.0, 0.0};
    CodaMemNode* geo[1] = {MakeNadirGeoRecord(prod, 10.0, 0.25, 0.0, 0.0, kZero,
                                              kZero, 0.0, 0.0, 0.0)};
    CodaMemNode* ca[2] = {
        MakeCloudsAerosolRecord(prod, 10.0, 0.25, 0.1, 100.0, 1.0, 0.0),
        MakeCloudsAerosolRecord(prod, 10.25, 0.25, 0.1, 100.0, 1.0, 0.0),
    };
    constexpr long kOneDim[] = {1};
    constexpr long kTwoDim[] = {2};
    ASSERT_TRUE(prod.AddField(prod.root(), "nad_uv0_o3",
                              prod.MakeNodeArray(kOneDim, mds)));
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation_nadir",
                              prod.MakeNodeArray(kOneDim, geo)));
    ASSERT_TRUE(prod.AddField(prod.root(), "clouds_aerosol",
                              prod.MakeNodeArray(kTwoDim, ca)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 6. Nadir MDS/geo timestamp mismatch (geo_time > mds_time).
  {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    PopulateSimpleNadirProduct(prod, /*version=*/4, "nad_uv0_o3",
                               /*mds_time=*/100.0, /*geo_time=*/200.0);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 7. Nadir MDS/geo missing or wrong-type dsr_time / integr_time.
  //    Variants 0..3 test wrong type; variants 4..7 test missing fields.
  for (int variant = 0; variant < 8; ++variant) {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", 4));
    CodaMemNode* mds_rec = prod.MakeRecord();
    if (variant == 0) {
      ASSERT_TRUE(prod.AddField(mds_rec, "dsr_time", prod.MakeString("bad")));
    } else if (variant != 4) {
      ASSERT_TRUE(prod.AddField(mds_rec, "dsr_time", prod.MakeDouble(100.0)));
    }
    if (variant == 1) {
      ASSERT_TRUE(
          prod.AddField(mds_rec, "integr_time", prod.MakeString("bad")));
    } else if (variant > 1 && variant != 5) {
      ASSERT_TRUE(prod.AddField(mds_rec, "integr_time", prod.MakeDouble(0.25)));
    }
    CodaMemNode* mds[1] = {mds_rec};
    constexpr long kOneDim[] = {1};
    ASSERT_TRUE(prod.AddField(prod.root(), "nad_uv0_o3",
                              prod.MakeNodeArray(kOneDim, mds)));

    CodaMemNode* geo_rec = prod.MakeRecord();
    if (variant == 2) {
      ASSERT_TRUE(prod.AddField(geo_rec, "dsr_time", prod.MakeString("bad")));
    } else if (variant != 6) {
      ASSERT_TRUE(prod.AddField(geo_rec, "dsr_time", prod.MakeDouble(100.0)));
    }
    if (variant == 3) {
      ASSERT_TRUE(
          prod.AddField(geo_rec, "integr_time", prod.MakeString("bad")));
    }
    CodaMemNode* geo[1] = {geo_rec};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation_nadir",
                              prod.MakeNodeArray(kOneDim, geo)));
    CodaMemNode* ca[1] = {
        MakeCloudsAerosolRecord(prod, 100.0, 0.25, 0.1, 100.0, 1.0, 0.0)};
    ASSERT_TRUE(prod.AddField(prod.root(), "clouds_aerosol",
                              prod.MakeNodeArray(kOneDim, ca)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
}

TEST(HarpIngestSciamachyL2Test, LimbInitErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("SCIAMACHY_L2");
  ASSERT_NE(nullptr, mod);

  // 1. Missing geolocation_limb.
  {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", 4));
    CodaMemNode* mds[1] = {MakeLimbMdsRecord(prod, 1, 1, 1, 1, {10.0}, 1.0,
                                             {10.0}, {100.0}, {250.0},
                                             {1e-6, 1.0}, {0.0})};
    constexpr long kOneDim[] = {1};
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_uv0_o3",
                              prod.MakeNodeArray(kOneDim, mds)));
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "dataset=lim_uv0_o3");
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. Limb timestamp mismatch: geo_time > mds_time (only 1 geo record at t=50,
  //    mds at t=10 -> avoids incrementing past num_geo_elements).
  {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", 4));
    CodaMemNode* geo[1] = {
        MakeLimbGeoRecord(prod, 50.0, 1.0, 2.0, 3.0, 4.0, 5.0)};
    CodaMemNode* mds[1] = {MakeLimbMdsRecord(prod, 1, 1, 1, 1, {10.0}, 1.0,
                                             {10.0}, {100.0}, {250.0},
                                             {1e-6, 1.0}, {0.0})};
    constexpr long kOneDim[] = {1};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation_limb",
                              prod.MakeNodeArray(kOneDim, geo)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_uv0_o3",
                              prod.MakeNodeArray(kOneDim, mds)));
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "dataset=lim_uv0_o3");
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 3. Missing or non-array add_diag field on MDS[0].
  for (int diag_err = 0; diag_err < 2; ++diag_err) {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", 4));
    CodaMemNode* geo[1] = {
        MakeLimbGeoRecord(prod, 10.0, 1.0, 2.0, 3.0, 4.0, 5.0)};
    CodaMemNode* mds_rec = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mds_rec, "n_main", prod.MakeUint8(1)));
    ASSERT_TRUE(prod.AddField(mds_rec, "n_state_vec", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(mds_rec, "n1", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(mds_rec, "n_meas", prod.MakeUint8(1)));
    CodaMemNode* mg = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mg, "dsr_time", prod.MakeDouble(10.0)));
    CodaMemNode* mg_arr[1] = {mg};
    constexpr long kOneDim[] = {1};
    ASSERT_TRUE(prod.AddField(mds_rec, "measurement_grid",
                              prod.MakeNodeArray(kOneDim, mg_arr)));
    ASSERT_TRUE(prod.AddField(mds_rec, "integr_time", prod.MakeDouble(1.0)));
    if (diag_err == 1) {
      ASSERT_TRUE(prod.AddField(mds_rec, "add_diag", prod.MakeInt32(0)));
    }
    CodaMemNode* mds[1] = {mds_rec};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation_limb",
                              prod.MakeNodeArray(kOneDim, geo)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_uv0_o3",
                              prod.MakeNodeArray(kOneDim, mds)));
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "dataset=lim_uv0_o3");
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 4. Missing and malformed scalar/grid fields inside limb MDS record.
  for (int bad_step = 0; bad_step < 14; ++bad_step) {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", 4));
    CodaMemNode* mds_rec = prod.MakeRecord();
    if (bad_step == 0) {
      ASSERT_TRUE(prod.AddField(mds_rec, "n_main", prod.MakeString("bad")));
    } else if (bad_step != 7) {
      ASSERT_TRUE(prod.AddField(mds_rec, "n_main", prod.MakeUint8(1)));
    }
    if (bad_step == 1) {
      ASSERT_TRUE(
          prod.AddField(mds_rec, "n_state_vec", prod.MakeString("bad")));
    } else if (bad_step > 1 && bad_step != 8) {
      ASSERT_TRUE(prod.AddField(mds_rec, "n_state_vec", prod.MakeUint16(1)));
    }
    if (bad_step == 2) {
      ASSERT_TRUE(prod.AddField(mds_rec, "n1", prod.MakeString("bad")));
    } else if (bad_step > 2 && bad_step != 9) {
      ASSERT_TRUE(prod.AddField(mds_rec, "n1", prod.MakeUint16(1)));
    }
    if (bad_step == 3) {
      ASSERT_TRUE(prod.AddField(mds_rec, "n_meas", prod.MakeString("bad")));
    } else if (bad_step > 3 && bad_step != 10) {
      ASSERT_TRUE(prod.AddField(mds_rec, "n_meas", prod.MakeUint8(1)));
    }
    if (bad_step == 4) {
      ASSERT_TRUE(
          prod.AddField(mds_rec, "measurement_grid", prod.MakeInt32(0)));
    } else if (bad_step > 4 && bad_step != 11) {
      CodaMemNode* mg = prod.MakeRecord();
      if (bad_step == 5) {
        ASSERT_TRUE(prod.AddField(mg, "dsr_time", prod.MakeString("bad")));
      } else if (bad_step != 12) {
        ASSERT_TRUE(prod.AddField(mg, "dsr_time", prod.MakeDouble(10.0)));
      }
      CodaMemNode* mg_arr[1] = {mg};
      constexpr long kOneDim[] = {1};
      ASSERT_TRUE(prod.AddField(mds_rec, "measurement_grid",
                                prod.MakeNodeArray(kOneDim, mg_arr)));
    }
    if (bad_step == 6) {
      ASSERT_TRUE(
          prod.AddField(mds_rec, "integr_time", prod.MakeString("bad")));
    }
    CodaMemNode* mds[1] = {mds_rec};
    constexpr long kOneDim[] = {1};
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_uv0_o3",
                              prod.MakeNodeArray(kOneDim, mds)));
    CodaMemNode* geo[1] = {
        MakeLimbGeoRecord(prod, 10.0, 1.0, 2.0, 3.0, 4.0, 5.0)};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation_limb",
                              prod.MakeNodeArray(kOneDim, geo)));

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "dataset=lim_uv0_o3");
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 5. Missing/bad dsr_time in geolocation_limb (initial, decrement,
  // increment).
  for (int geo_err = 0; geo_err < 3; ++geo_err) {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", 4));
    std::vector<CodaMemNode*> geo_recs;
    std::vector<CodaMemNode*> mds_recs;
    if (geo_err == 0) {
      // geo[0] missing dsr_time -> fails initial geo_time read.
      geo_recs.push_back(prod.MakeRecord());
      mds_recs.push_back(MakeLimbMdsRecord(prod, 1, 1, 1, 1, {10.0}, 1.0,
                                           {10.0}, {100.0}, {250.0},
                                           {1e-6, 1.0}, {0.0}));
    } else if (geo_err == 1) {
      // geo[0]=t20, geo[1]=bad_str -> MDS 0 matches t20 (geo_dsr_id=0),
      // MDS 1 starts at geo_dsr_id=1 and fails reading geo[1].dsr_time.
      geo_recs.push_back(
          MakeLimbGeoRecord(prod, 20.0, 1.0, 2.0, 3.0, 4.0, 5.0));
      CodaMemNode* bad = prod.MakeRecord();
      ASSERT_TRUE(prod.AddField(bad, "dsr_time", prod.MakeString("bad")));
      geo_recs.push_back(bad);
      mds_recs.push_back(MakeLimbMdsRecord(prod, 1, 1, 1, 1, {20.0}, 1.0,
                                           {10.0}, {100.0}, {250.0},
                                           {1e-6, 1.0}, {0.0}));
      mds_recs.push_back(MakeLimbMdsRecord(prod, 1, 1, 1, 1, {10.0}, 1.0,
                                           {10.0}, {100.0}, {250.0},
                                           {1e-6, 1.0}, {0.0}));
    } else {
      // geo[0]=t5 (t5 < t10 triggers geo_time < mds_time increment loop),
      // geo[1]=bad_str -> fails during increment loop.
      geo_recs.push_back(MakeLimbGeoRecord(prod, 5.0, 1.0, 2.0, 3.0, 4.0, 5.0));
      CodaMemNode* bad = prod.MakeRecord();
      ASSERT_TRUE(prod.AddField(bad, "dsr_time", prod.MakeString("bad")));
      geo_recs.push_back(bad);
      mds_recs.push_back(MakeLimbMdsRecord(prod, 1, 1, 1, 1, {10.0}, 1.0,
                                           {10.0}, {100.0}, {250.0},
                                           {1e-6, 1.0}, {0.0}));
    }
    const long geo_dims[1] = {static_cast<long>(geo_recs.size())};
    const long mds_dims[1] = {static_cast<long>(mds_recs.size())};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation_limb",
                              prod.MakeNodeArray(geo_dims, geo_recs)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_uv0_o3",
                              prod.MakeNodeArray(mds_dims, mds_recs)));
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "dataset=lim_uv0_o3");
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
}

TEST(HarpIngestSciamachyL2Test, VariableReadErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("SCIAMACHY_L2");
  ASSERT_NE(nullptr, mod);

  // 1. Nadir product missing /mph, vcd, vcd_err, flag_vcd_flags, cl_frac,
  //    cen_coor_nad, cor_coor_nad, and angles -> exercise read errors with
  //    stop_on_error=false.
  {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", 4));

    CodaMemNode* mds_rec = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mds_rec, "dsr_time", prod.MakeDouble(100.0)));
    ASSERT_TRUE(prod.AddField(mds_rec, "integr_time", prod.MakeDouble(0.25)));
    CodaMemNode* mds[1] = {mds_rec};
    constexpr long kOneDim[] = {1};
    ASSERT_TRUE(prod.AddField(prod.root(), "nad_uv0_o3",
                              prod.MakeNodeArray(kOneDim, mds)));

    CodaMemNode* geo_rec = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(geo_rec, "dsr_time", prod.MakeDouble(100.0)));
    ASSERT_TRUE(prod.AddField(geo_rec, "integr_time", prod.MakeDouble(0.25)));
    CodaMemNode* geo[1] = {geo_rec};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation_nadir",
                              prod.MakeNodeArray(kOneDim, geo)));

    CodaMemNode* ca_rec = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(ca_rec, "dsr_time", prod.MakeDouble(100.0)));
    CodaMemNode* ca[1] = {ca_rec};
    ASSERT_TRUE(prod.AddField(prod.root(), "clouds_aerosol",
                              prod.MakeNodeArray(kOneDim, ca)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "dataset=nad_uv0_o3", /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["longitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["solar_zenith_angle"].read_status);
    EXPECT_EQ(-1, res.variables["viewing_zenith_angle"].read_status);
    EXPECT_EQ(-1, res.variables["relative_azimuth_angle"].read_status);
    EXPECT_EQ(-1, res.variables["scan_direction_type"].read_status);
    EXPECT_EQ(-1, res.variables["cloud_fraction"].read_status);
    EXPECT_EQ(-1, res.variables["O3_column_number_density"].read_status);
    EXPECT_EQ(
        -1, res.variables["O3_column_number_density_uncertainty"].read_status);
    EXPECT_EQ(-1,
              res.variables["O3_column_number_density_validity"].read_status);
  }

  // 2. Limb product where MDS[0] enables extended_diag (len=9 for n_main=1,
  //    n_stvec=2, n1=1) and MDS[1] has too-small add_diag (len=2) with
  //    identical record schema, exercising add_diag too-small branches.
  {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", 4));
    AddMphAbsOrbit(prod, 1);

    CodaMemNode* geo[2] = {
        MakeLimbGeoRecord(prod, 10.0, 1.0, 2.0, 3.0, 4.0, 5.0),
        MakeLimbGeoRecord(prod, 20.0, 1.0, 2.0, 3.0, 4.0, 5.0),
    };
    constexpr long kTwoDim[] = {2};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation_limb",
                              prod.MakeNodeArray(kTwoDim, geo)));

    const std::vector<double> diag0(9, 1.0);
    CodaMemNode* mds0 = MakeLimbMdsRecord(prod, 1, 2, 1, 1, {10.0}, 1.0, {30.0},
                                          {10.0}, {240.0}, {1e-6, 5.0}, diag0);

    const std::vector<double> tiny_diag = {1.0, 2.0};
    CodaMemNode* mds1 = MakeLimbMdsRecord(
        prod, 2, 2, 1, 1, {20.0}, 1.0, {40.0, 20.0}, {5.0, 50.0},
        {230.0, 250.0}, {1e-6, 5.0, 2e-6, 10.0}, tiny_diag);

    CodaMemNode* mds[2] = {mds0, mds1};
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_uv0_o3",
                              prod.MakeNodeArray(kTwoDim, mds)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "dataset=lim_uv0_o3", /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["O3_volume_mixing_ratio_avk"].read_status);
    EXPECT_EQ(-1, res.variables["O3_number_density"].read_status);
    EXPECT_EQ(-1, res.variables["O3_number_density_uncertainty"].read_status);
    EXPECT_EQ(-1, res.variables["O3_number_density_apriori"].read_status);
    EXPECT_EQ(-1, res.variables["O3_number_density_avk"].read_status);
  }

  // 3. Limb product missing profile & geo subfields (using a single MDS/geo
  //    record so schema matches).
  {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", 4));

    CodaMemNode* bad_geo = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(bad_geo, "dsr_time", prod.MakeDouble(10.0)));
    CodaMemNode* geo[1] = {bad_geo};
    constexpr long kOneDim[] = {1};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation_limb",
                              prod.MakeNodeArray(kOneDim, geo)));

    CodaMemNode* mds_rec = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mds_rec, "n_main", prod.MakeUint8(1)));
    ASSERT_TRUE(prod.AddField(mds_rec, "n_state_vec", prod.MakeUint16(2)));
    ASSERT_TRUE(prod.AddField(mds_rec, "n1", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(mds_rec, "n_meas", prod.MakeUint8(1)));
    CodaMemNode* mg = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mg, "dsr_time", prod.MakeDouble(10.0)));
    CodaMemNode* mg_arr[1] = {mg};
    ASSERT_TRUE(prod.AddField(mds_rec, "measurement_grid",
                              prod.MakeNodeArray(kOneDim, mg_arr)));
    ASSERT_TRUE(prod.AddField(mds_rec, "integr_time", prod.MakeDouble(1.0)));
    const double ok_diag[9] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
    constexpr long kNineDim[] = {9};
    ASSERT_TRUE(prod.AddField(mds_rec, "add_diag",
                              prod.MakeDoubleArray(kNineDim, ok_diag)));
    CodaMemNode* mds[1] = {mds_rec};
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_uv0_o3",
                              prod.MakeNodeArray(kOneDim, mds)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "dataset=lim_uv0_o3", /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["altitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["pressure_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["temperature"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["solar_zenith_angle"].read_status);
    EXPECT_EQ(-1, res.variables["viewing_zenith_angle"].read_status);
    EXPECT_EQ(-1, res.variables["relative_azimuth_angle"].read_status);
    EXPECT_EQ(-1, res.variables["O3_volume_mixing_ratio"].read_status);
    EXPECT_EQ(-1,
              res.variables["O3_volume_mixing_ratio_uncertainty"].read_status);
  }
}

TEST(HarpIngestSciamachyL2Test, CoaddingModeErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("SCIAMACHY_L2");
  ASSERT_NE(nullptr, mod);

  // Test coadding=4 (integr_time=1.0) and coadding=5 (integr_time=2.5) with
  // missing/truncated/wrong-type geolocation and cloud fields.
  struct CoaddCase {
    double mds_integr;
    double geo_integr;
    long num_geo;
  };
  constexpr CoaddCase kModes[] = {
      {.mds_integr = 0.25, .geo_integr = 0.25, .num_geo = 1},
      {.mds_integr = 1.0, .geo_integr = 0.25, .num_geo = 4},
      {.mds_integr = 2.5, .geo_integr = 0.5, .num_geo = 5},
  };

  for (const auto& mode : kModes) {
    // Subcase A: geo records only have dsr_time & integr_time (missing
    // cen_coor_nad, cor_coor_nad, and angle fields).
    {
      CodaMemProduct prod(coda_format_binary);
      ASSERT_TRUE(prod.ok());
      ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", 4));
      CodaMemNode* mds[1] = {
          MakeNadirMdsRecord(prod, 100.0, mode.mds_integr, 1.0, 1.0, 0)};
      constexpr long kOneDim[] = {1};
      ASSERT_TRUE(prod.AddField(prod.root(), "nad_uv0_o3",
                                prod.MakeNodeArray(kOneDim, mds)));

      std::vector<CodaMemNode*> geo_recs;
      std::vector<CodaMemNode*> ca_recs;
      for (long k = 0; k < mode.num_geo; ++k) {
        CodaMemNode* g = prod.MakeRecord();
        ASSERT_TRUE(prod.AddField(
            g, "dsr_time", prod.MakeDouble(100.0 + k * mode.geo_integr)));
        ASSERT_TRUE(
            prod.AddField(g, "integr_time", prod.MakeDouble(mode.geo_integr)));
        geo_recs.push_back(g);
        CodaMemNode* c = prod.MakeRecord();
        ASSERT_TRUE(prod.AddField(c, "cl_frac", prod.MakeString("bad")));
        ca_recs.push_back(c);
      }
      const long geo_dims[1] = {mode.num_geo};
      ASSERT_TRUE(prod.AddField(prod.root(), "geolocation_nadir",
                                prod.MakeNodeArray(geo_dims, geo_recs)));
      ASSERT_TRUE(prod.AddField(prod.root(), "clouds_aerosol",
                                prod.MakeNodeArray(geo_dims, ca_recs)));

      IngestExerciseResult res = ExerciseIngestionModule(
          mod, prod.product(), "dataset=nad_uv0_o3", /*stop_on_error=*/false);
      EXPECT_EQ(-1, res.status);
      EXPECT_EQ(-1, res.variables["latitude"].read_status);
      EXPECT_EQ(-1, res.variables["longitude"].read_status);
      EXPECT_EQ(-1, res.variables["latitude_bounds"].read_status);
      EXPECT_EQ(-1, res.variables["longitude_bounds"].read_status);
      EXPECT_EQ(-1, res.variables["solar_zenith_angle"].read_status);
      EXPECT_EQ(-1, res.variables["viewing_zenith_angle"].read_status);
      EXPECT_EQ(-1, res.variables["relative_azimuth_angle"].read_status);
      EXPECT_EQ(-1, res.variables["cloud_fraction"].read_status);
    }

    // Subcase B: geo records have 0-length / string angle arrays and truncated
    // cor_coor_nad (only 1 element of empty/string coords) + empty
    // cen_coor_nad.
    for (int sub = 0; sub < 3; ++sub) {
      CodaMemProduct prod(coda_format_binary);
      ASSERT_TRUE(prod.ok());
      ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", 4));
      CodaMemNode* mds[1] = {
          MakeNadirMdsRecord(prod, 100.0, mode.mds_integr, 1.0, 1.0, 0)};
      constexpr long kOneDim[] = {1};
      ASSERT_TRUE(prod.AddField(prod.root(), "nad_uv0_o3",
                                prod.MakeNodeArray(kOneDim, mds)));

      std::vector<CodaMemNode*> geo_recs;
      std::vector<CodaMemNode*> ca_recs;
      for (long k = 0; k < mode.num_geo; ++k) {
        CodaMemNode* g = prod.MakeRecord();
        ASSERT_TRUE(prod.AddField(
            g, "dsr_time", prod.MakeDouble(100.0 + k * mode.geo_integr)));
        ASSERT_TRUE(
            prod.AddField(g, "integr_time", prod.MakeDouble(mode.geo_integr)));
        CodaMemNode* cen = prod.MakeRecord();
        if (sub == 1) {
          ASSERT_TRUE(prod.AddField(cen, "latitude", prod.MakeDouble(1.0)));
        } else if (sub == 2) {
          ASSERT_TRUE(prod.AddField(cen, "latitude", prod.MakeString("bad")));
          ASSERT_TRUE(prod.AddField(cen, "longitude", prod.MakeString("bad")));
        }
        ASSERT_TRUE(prod.AddField(g, "cen_coor_nad", cen));

        // Build cor_coor_nad with (sub == 0 ? 1 : (sub == 1 ? 2 : 3)) elements
        // to trigger array index failures across bounds/scan_direction_type.
        std::vector<CodaMemNode*> corners;
        const int num_corners = sub + 1;
        for (int c = 0; c < num_corners; ++c) {
          CodaMemNode* cr = prod.MakeRecord();
          if (sub == 1) {
            ASSERT_TRUE(prod.AddField(cr, "latitude", prod.MakeDouble(1.0)));
          } else if (sub == 2) {
            ASSERT_TRUE(prod.AddField(cr, "latitude", prod.MakeString("bad")));
            ASSERT_TRUE(prod.AddField(cr, "longitude", prod.MakeString("bad")));
          }
          corners.push_back(cr);
        }
        const long c_dims[1] = {num_corners};
        ASSERT_TRUE(prod.AddField(g, "cor_coor_nad",
                                  prod.MakeNodeArray(c_dims, corners)));

        if (sub == 0) {
          constexpr double kShort[1] = {10.0};
          ASSERT_TRUE(prod.AddField(g, "sol_zen_angle_toa",
                                    prod.MakeDoubleArray(kOneDim, kShort)));
          ASSERT_TRUE(prod.AddField(g, "los_zen_angle_toa",
                                    prod.MakeDoubleArray(kOneDim, kShort)));
          ASSERT_TRUE(prod.AddField(g, "rel_azi_angle_toa",
                                    prod.MakeDoubleArray(kOneDim, kShort)));
        } else {
          constexpr long kThreeDim[] = {3};
          constexpr std::string_view kBadStrs[3] = {"a", "b", "c"};
          ASSERT_TRUE(prod.AddField(g, "sol_zen_angle_toa",
                                    prod.MakeStringArray(kThreeDim, kBadStrs)));
          ASSERT_TRUE(prod.AddField(g, "los_zen_angle_toa",
                                    prod.MakeStringArray(kThreeDim, kBadStrs)));
          ASSERT_TRUE(prod.AddField(g, "rel_azi_angle_toa",
                                    prod.MakeStringArray(kThreeDim, kBadStrs)));
        }
        geo_recs.push_back(g);
        ca_recs.push_back(MakeCloudsAerosolRecord(prod, 100.0, mode.geo_integr,
                                                  0.2, 100.0, 1.0, 0.0));
      }
      const long geo_dims[1] = {mode.num_geo};
      ASSERT_TRUE(prod.AddField(prod.root(), "geolocation_nadir",
                                prod.MakeNodeArray(geo_dims, geo_recs)));
      ASSERT_TRUE(prod.AddField(prod.root(), "clouds_aerosol",
                                prod.MakeNodeArray(geo_dims, ca_recs)));

      IngestExerciseResult res = ExerciseIngestionModule(
          mod, prod.product(), "dataset=nad_uv0_o3", /*stop_on_error=*/false);
      EXPECT_EQ(-1, res.status);
      if (mode.num_geo == 1 && sub == 1) {
        EXPECT_EQ(0, res.variables["latitude"].read_status);
      } else {
        EXPECT_EQ(-1, res.variables["latitude"].read_status);
      }
      EXPECT_EQ(-1, res.variables["longitude"].read_status);
      EXPECT_EQ(-1, res.variables["latitude_bounds"].read_status);
      EXPECT_EQ(-1, res.variables["longitude_bounds"].read_status);
      EXPECT_EQ(-1, res.variables["solar_zenith_angle"].read_status);
    }
  }
}

TEST(HarpIngestSciamachyL2Test, AdditionalScalarProfileAndDiagReadErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("SCIAMACHY_L2");
  ASSERT_NE(nullptr, mod);

  // 1. Nadir wrong-type /mph/abs_orbit, vcd (0-len / string), vcd_err present
  //    with missing vcd, flag_vcd_flags as string, and clouds_aerosol errors.
  {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", 4));
    CodaMemNode* mph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mph, "abs_orbit", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "mph", mph));

    CodaMemNode* mds_rec = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mds_rec, "dsr_time", prod.MakeDouble(100.0)));
    ASSERT_TRUE(prod.AddField(mds_rec, "integr_time", prod.MakeDouble(0.25)));
    constexpr long kZeroDim[] = {0};
    ASSERT_TRUE(
        prod.AddField(mds_rec, "vcd", prod.MakeDoubleArray(kZeroDim, {})));
    constexpr long kOneDim[] = {1};
    const double err_arr[1] = {2.0};
    ASSERT_TRUE(prod.AddField(mds_rec, "vcd_err",
                              prod.MakeDoubleArray(kOneDim, err_arr)));
    ASSERT_TRUE(
        prod.AddField(mds_rec, "flag_vcd_flags", prod.MakeString("bad")));
    CodaMemNode* mds[1] = {mds_rec};
    ASSERT_TRUE(prod.AddField(prod.root(), "nad_uv0_o3",
                              prod.MakeNodeArray(kOneDim, mds)));

    constexpr double kZero[4] = {0.0, 0.0, 0.0, 0.0};
    CodaMemNode* geo[1] = {MakeNadirGeoRecord(prod, 100.0, 0.25, 0.0, 0.0,
                                              kZero, kZero, 0.0, 0.0, 0.0)};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation_nadir",
                              prod.MakeNodeArray(kOneDim, geo)));
    CodaMemNode* ca[1] = {
        MakeCloudsAerosolRecord(prod, 100.0, 0.25, 0.1, 100.0, 1.0, 0.0)};
    ASSERT_TRUE(prod.AddField(prod.root(), "clouds_aerosol",
                              prod.MakeNodeArray(kOneDim, ca)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "dataset=nad_uv0_o3", /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["O3_column_number_density"].read_status);
    EXPECT_EQ(
        -1, res.variables["O3_column_number_density_uncertainty"].read_status);
    EXPECT_EQ(-1,
              res.variables["O3_column_number_density_validity"].read_status);
  }
  {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", 4));
    AddMphAbsOrbit(prod, 1);

    constexpr double kZero[4] = {0.0, 0.0, 0.0, 0.0};
    CodaMemNode* geo[1] = {MakeNadirGeoRecord(prod, 100.0, 0.25, 0.0, 0.0,
                                              kZero, kZero, 0.0, 0.0, 0.0)};
    constexpr long kOneDim[] = {1};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation_nadir",
                              prod.MakeNodeArray(kOneDim, geo)));
    CodaMemNode* ca_rec = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(ca_rec, "dsr_time", prod.MakeDouble(100.0)));
    ASSERT_TRUE(prod.AddField(ca_rec, "integr_time", prod.MakeDouble(0.25)));
    ASSERT_TRUE(prod.AddField(ca_rec, "cl_frac", prod.MakeDouble(0.5)));
    ASSERT_TRUE(prod.AddField(ca_rec, "cl_top_height", prod.MakeString("bad")));
    CodaMemNode* ca[1] = {ca_rec};
    ASSERT_TRUE(prod.AddField(prod.root(), "clouds_aerosol",
                              prod.MakeNodeArray(kOneDim, ca)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "dataset=clouds_aerosol", /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["cloud_top_height"].read_status);
    EXPECT_EQ(-1, res.variables["absorbing_aerosol_index"].read_status);
  }

  // 2. Limb string arrays for tangent_height/pressure/temp, wrong-dim
  //    main_species, short/string geo arrays, and string add_diag on MDS[1].
  {
    CodaMemProduct prod(coda_format_binary);
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_SCIAMACHY", "SCI_OL__2P", 4));
    AddMphAbsOrbit(prod, 1);

    CodaMemNode* bad_geo = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(bad_geo, "dsr_time", prod.MakeDouble(10.0)));
    CodaMemNode* bad_tc[1] = {MakeCoordRecord(prod, 1.0, 2.0)};
    constexpr long kOneDim[] = {1};
    ASSERT_TRUE(prod.AddField(bad_geo, "tangent_coord",
                              prod.MakeNodeArray(kOneDim, bad_tc)));
    const double one_angle[1] = {10.0};
    ASSERT_TRUE(prod.AddField(bad_geo, "sol_zen_angle_toa",
                              prod.MakeDoubleArray(kOneDim, one_angle)));
    constexpr long kThreeDim[] = {3};
    constexpr std::string_view kBadStrs[3] = {"a", "b", "c"};
    ASSERT_TRUE(prod.AddField(bad_geo, "los_zen_angle_toa",
                              prod.MakeStringArray(kThreeDim, kBadStrs)));
    ASSERT_TRUE(prod.AddField(bad_geo, "rel_azi_angle_toa",
                              prod.MakeStringArray(kThreeDim, kBadStrs)));
    CodaMemNode* geo[2] = {
        bad_geo,
        MakeLimbGeoRecord(prod, 20.0, 1.0, 2.0, 3.0, 4.0, 5.0),
    };
    constexpr long kTwoDim[] = {2};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation_limb",
                              prod.MakeNodeArray(kTwoDim, geo)));

    // MDS 0: string arrays for tangent_height/pressure/temp, 1D main_species,
    // valid 9-element double add_diag.
    CodaMemNode* mds0 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mds0, "n_main", prod.MakeUint8(1)));
    ASSERT_TRUE(prod.AddField(mds0, "n_state_vec", prod.MakeUint16(2)));
    ASSERT_TRUE(prod.AddField(mds0, "n1", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(mds0, "n_meas", prod.MakeUint8(1)));
    CodaMemNode* mg0 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mg0, "dsr_time", prod.MakeDouble(10.0)));
    CodaMemNode* mg0_arr[1] = {mg0};
    ASSERT_TRUE(prod.AddField(mds0, "measurement_grid",
                              prod.MakeNodeArray(kOneDim, mg0_arr)));
    ASSERT_TRUE(prod.AddField(mds0, "integr_time", prod.MakeDouble(1.0)));
    constexpr std::string_view kBad1[1] = {"bad"};
    ASSERT_TRUE(prod.AddField(mds0, "tangent_height",
                              prod.MakeStringArray(kOneDim, kBad1)));
    ASSERT_TRUE(prod.AddField(mds0, "tangent_pressure",
                              prod.MakeStringArray(kOneDim, kBad1)));
    ASSERT_TRUE(prod.AddField(mds0, "tangent_temp",
                              prod.MakeStringArray(kOneDim, kBad1)));
    // 1D array fails goto_array_element(2).
    ASSERT_TRUE(prod.AddField(mds0, "main_species",
                              prod.MakeDoubleArray(kOneDim, one_angle)));
    const double diag0_vals[9] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
    constexpr long kNineDim[] = {9};
    ASSERT_TRUE(prod.AddField(mds0, "add_diag",
                              prod.MakeDoubleArray(kNineDim, diag0_vals)));

    // MDS 1: same field order, but string array for add_diag (length 9 ->
    // num_elements >= 9 passes size check, then read_double_array fails) and
    // 2D main_species where tang_vmr is string while err_tang_vmr is double.
    CodaMemNode* mds1 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mds1, "n_main", prod.MakeUint8(1)));
    ASSERT_TRUE(prod.AddField(mds1, "n_state_vec", prod.MakeUint16(2)));
    ASSERT_TRUE(prod.AddField(mds1, "n1", prod.MakeUint16(1)));
    ASSERT_TRUE(prod.AddField(mds1, "n_meas", prod.MakeUint8(1)));
    CodaMemNode* mg1 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mg1, "dsr_time", prod.MakeDouble(20.0)));
    CodaMemNode* mg1_arr[1] = {mg1};
    ASSERT_TRUE(prod.AddField(mds1, "measurement_grid",
                              prod.MakeNodeArray(kOneDim, mg1_arr)));
    ASSERT_TRUE(prod.AddField(mds1, "integr_time", prod.MakeDouble(1.0)));
    ASSERT_TRUE(prod.AddField(mds1, "tangent_height",
                              prod.MakeStringArray(kOneDim, kBad1)));
    ASSERT_TRUE(prod.AddField(mds1, "tangent_pressure",
                              prod.MakeStringArray(kOneDim, kBad1)));
    ASSERT_TRUE(prod.AddField(mds1, "tangent_temp",
                              prod.MakeStringArray(kOneDim, kBad1)));
    ASSERT_TRUE(prod.AddField(mds1, "main_species",
                              prod.MakeDoubleArray(kOneDim, one_angle)));
    constexpr std::string_view kBad9[9] = {"a", "b", "c", "d", "e",
                                           "f", "g", "h", "i"};
    ASSERT_TRUE(
        prod.AddField(mds1, "add_diag", prod.MakeStringArray(kNineDim, kBad9)));

    CodaMemNode* mds[2] = {mds0, mds1};
    ASSERT_TRUE(prod.AddField(prod.root(), "lim_uv0_o3",
                              prod.MakeNodeArray(kTwoDim, mds)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "dataset=lim_uv0_o3", /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["altitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["pressure_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["temperature"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["solar_zenith_angle"].read_status);
    EXPECT_EQ(-1, res.variables["viewing_zenith_angle"].read_status);
    EXPECT_EQ(-1, res.variables["relative_azimuth_angle"].read_status);
    EXPECT_EQ(-1, res.variables["O3_volume_mixing_ratio"].read_status);
    EXPECT_EQ(-1, res.variables["O3_volume_mixing_ratio_avk"].read_status);
    EXPECT_EQ(-1, res.variables["O3_number_density"].read_status);
    EXPECT_EQ(-1, res.variables["O3_number_density_apriori"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
