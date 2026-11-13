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
// Unit tests for IASI-NG L2 ingestion modules
// (libharp/harp-ingest-iasi-ng_l2.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-iasi-ng_l2.c

#include <cmath>
#include <cstdint>
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

using ::testing::ElementsAre;
using ::testing::FloatNear;

constexpr long kNumLines = 1;
constexpr long kNumFor = 2;
constexpr long kNumFov = 16;
constexpr long kNumIfov = kNumLines * kNumFor * kNumFov;  // 32
constexpr long kNumLevels = 2;

std::vector<double> MakeSeqDouble(long count, double start, double step) {
  std::vector<double> out(static_cast<size_t>(count));
  for (long i = 0; i < count; ++i) {
    out[static_cast<size_t>(i)] = start + static_cast<double>(i) * step;
  }
  return out;
}

std::vector<float> MakeSeqFloat(long count, float start, float step) {
  std::vector<float> out(static_cast<size_t>(count));
  for (long i = 0; i < count; ++i) {
    out[static_cast<size_t>(i)] = start + static_cast<float>(i) * step;
  }
  return out;
}

// Populates /data, dimensions, /data/geolocation_information, and optional
// /data/surface_info for IASI-NG L2 products.
CodaMemNode* PopulateIasiNgBase(CodaMemProduct& prod,
                                std::string_view coda_prod_type,
                                const char* level_dim_name,
                                bool use_orbit_fallback, bool include_surface) {
  EXPECT_TRUE(prod.SetProductInfo("EPS_SG", coda_prod_type, 1));

  if (use_orbit_fallback) {
    constexpr long kOrbitDims[] = {1};
    constexpr uint32_t kOrbit[] = {54321u};
    EXPECT_TRUE(prod.AddAttribute(prod.root(), "orbit",
                                  prod.MakeUint32Array(kOrbitDims, kOrbit)));
  } else {
    EXPECT_TRUE(
        prod.AddAttribute(prod.root(), "orbit_start", prod.MakeInt32(12345)));
  }

  CodaMemNode* data = prod.MakeRecord();
  EXPECT_TRUE(prod.AddField(prod.root(), "data", data));

  // Place dimension arrays inside /data (and one inside a 1-element array of
  // records to exercise find_dimension_length_recursive's coda_array_class
  // traversal).
  const std::vector<int32_t> lines_vec(kNumLines, 0);
  const std::vector<int32_t> for_vec(kNumFor, 0);
  const std::vector<int32_t> fov_vec(kNumFov, 0);
  constexpr long kLinesDims[] = {kNumLines};
  constexpr long kForDims[] = {kNumFor};
  constexpr long kFovDims[] = {kNumFov};

  CodaMemNode* nested_dim_rec = prod.MakeRecord();
  EXPECT_TRUE(prod.AddField(nested_dim_rec, "n_lines",
                            prod.MakeInt32Array(kLinesDims, lines_vec)));
  CodaMemNode* nested_arr[] = {nested_dim_rec};
  constexpr long kOneDim[] = {1};
  EXPECT_TRUE(prod.AddField(data, "dim_holder",
                            prod.MakeNodeArray(kOneDim, nested_arr)));
  EXPECT_TRUE(
      prod.AddField(data, "n_for", prod.MakeInt32Array(kForDims, for_vec)));
  EXPECT_TRUE(
      prod.AddField(data, "n_fov", prod.MakeInt32Array(kFovDims, fov_vec)));

  if (level_dim_name != nullptr) {
    const std::vector<int32_t> lvl_vec(kNumLevels, 0);
    constexpr long kLvlDims[] = {kNumLevels};
    EXPECT_TRUE(prod.AddField(data, level_dim_name,
                              prod.MakeInt32Array(kLvlDims, lvl_vec)));
  }

  // Geolocation group with 4x4 grid coordinates per FOR (skewed slightly so
  // great-circle diagonals and edges intersect cleanly).
  CodaMemNode* geo = prod.MakeRecord();
  EXPECT_TRUE(prod.AddField(data, "geolocation_information", geo));

  constexpr long kUtcDims[] = {kNumLines, kNumFor};
  constexpr double kUtc[] = {1000.0, 2000.0};
  EXPECT_TRUE(
      prod.AddField(geo, "onboard_utc", prod.MakeDoubleArray(kUtcDims, kUtc)));

  std::vector<double> lat(kNumIfov);
  std::vector<double> lon(kNumIfov);
  for (long f = 0; f < kNumLines * kNumFor; ++f) {
    const double base_lat = 10.0 + static_cast<double>(f) * 5.0;
    const double base_lon = 20.0 + static_cast<double>(f) * 5.0;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        const long idx = f * 16 + r * 4 + c;
        lat[static_cast<size_t>(idx)] = base_lat + (3 - r) * 0.5 + c * 0.02;
        lon[static_cast<size_t>(idx)] = base_lon + (3 - c) * 0.5 + r * 0.02;
      }
    }
  }

  constexpr long kIfovDims[] = {kNumIfov};
  EXPECT_TRUE(prod.AddField(geo, "sounder_pixel_latitude",
                            prod.MakeDoubleArray(kIfovDims, lat)));
  EXPECT_TRUE(prod.AddField(geo, "sounder_pixel_longitude",
                            prod.MakeDoubleArray(kIfovDims, lon)));
  EXPECT_TRUE(prod.AddField(
      geo, "sounder_pixel_sun_azimuth",
      prod.MakeDoubleArray(kIfovDims, MakeSeqDouble(kNumIfov, 10.0, 1.0))));
  EXPECT_TRUE(prod.AddField(
      geo, "sounder_pixel_sun_zenith",
      prod.MakeDoubleArray(kIfovDims, MakeSeqDouble(kNumIfov, 20.0, 0.5))));
  EXPECT_TRUE(prod.AddField(
      geo, "sounder_pixel_azimuth",
      prod.MakeDoubleArray(kIfovDims, MakeSeqDouble(kNumIfov, 30.0, 1.0))));
  EXPECT_TRUE(prod.AddField(
      geo, "sounder_pixel_zenith",
      prod.MakeDoubleArray(kIfovDims, MakeSeqDouble(kNumIfov, 5.0, 0.25))));

  if (include_surface) {
    CodaMemNode* sfc = prod.MakeRecord();
    EXPECT_TRUE(prod.AddField(data, "surface_info", sfc));
    std::vector<float> ice_pct(kNumIfov, 50.0f);
    ice_pct[0] = -1.0f;  // Outside [0, 100]: remains unchanged.
    std::vector<float> land_pct(kNumIfov, 80.0f);
    land_pct[1] = 150.0f;  // Outside [0, 100]: remains unchanged.
    EXPECT_TRUE(prod.AddField(sfc, "ice_fraction",
                              prod.MakeFloatArray(kIfovDims, ice_pct)));
    EXPECT_TRUE(prod.AddField(sfc, "land_fraction",
                              prod.MakeFloatArray(kIfovDims, land_pct)));
    EXPECT_TRUE(prod.AddField(
        sfc, "height",
        prod.MakeFloatArray(kIfovDims, MakeSeqFloat(kNumIfov, 100.0f, 1.0f))));
    EXPECT_TRUE(prod.AddField(
        sfc, "height_std",
        prod.MakeFloatArray(kIfovDims, MakeSeqFloat(kNumIfov, 1.0f, 0.1f))));
  }

  return data;
}

void VerifyCommonVariables(IngestExerciseResult& res, int32_t expected_orbit) {
  EXPECT_EQ(kNumIfov, res.dimensions[harp_dimension_time]);
  EXPECT_THAT(res.variables["orbit_index"].int32_values,
              ElementsAre(expected_orbit));
  ASSERT_EQ(kNumIfov, res.variables["datetime"].double_values.size());
  EXPECT_DOUBLE_EQ(1000.0, res.variables["datetime"].double_values[0]);
  EXPECT_DOUBLE_EQ(1000.0, res.variables["datetime"].double_values[15]);
  EXPECT_DOUBLE_EQ(2000.0, res.variables["datetime"].double_values[16]);
  EXPECT_DOUBLE_EQ(2000.0, res.variables["datetime"].double_values[31]);
  ASSERT_EQ(kNumIfov * 4,
            res.variables["latitude_bounds"].double_values.size());
  ASSERT_EQ(kNumIfov * 4,
            res.variables["longitude_bounds"].double_values.size());
  for (double v : res.variables["latitude_bounds"].double_values) {
    EXPECT_TRUE(std::isfinite(v));
  }
  for (double v : res.variables["longitude_bounds"].double_values) {
    EXPECT_TRUE(std::isfinite(v));
  }
}

TEST(HarpIngestIasiNgL2Test, IngestCoNacO3SuccessSignedAndUnsigned) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  constexpr long kIfovDims[] = {kNumIfov};

  // 1. IAS_02_CO with unsigned uint8/uint32 flags + /@orbit fallback.
  {
    const harp_ingestion_module* mod = FindIngestionModule("IAS_02_CO");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* data = PopulateIasiNgBase(prod, "IAS_02_CO_", nullptr,
                                           /*use_orbit_fallback=*/true, false);

    std::vector<uint8_t> qflag(kNumIfov, 2u);
    std::vector<uint32_t> bdiv(kNumIfov, 7u);
    ASSERT_TRUE(prod.AddField(
        data, "surface_z",
        prod.MakeFloatArray(kIfovDims, MakeSeqFloat(kNumIfov, 10.0f, 1.0f))));
    ASSERT_TRUE(
        prod.AddField(data, "co_qflag", prod.MakeUint8Array(kIfovDims, qflag)));
    ASSERT_TRUE(prod.AddField(
        data, "atmosphere_mass_content_of_carbon_monoxide",
        prod.MakeFloatArray(kIfovDims, MakeSeqFloat(kNumIfov, 0.5f, 0.01f))));
    ASSERT_TRUE(
        prod.AddField(data, "co_bdiv", prod.MakeUint32Array(kIfovDims, bdiv)));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    VerifyCommonVariables(res, 54321);
    EXPECT_EQ(2, res.variables["validity"].int8_values[0]);
    EXPECT_EQ(7, res.variables["CO_column_density_validity"].int32_values[0]);
    EXPECT_THAT(res.variables["CO_column_density"].float_values[0],
                FloatNear(0.5f, 1e-6f));
  }

  // 2. IAS_02_NAC with signed int8/int32 flags.
  {
    const harp_ingestion_module* mod = FindIngestionModule("IAS_02_NAC");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* data =
        PopulateIasiNgBase(prod, "IAS_02_NAC", nullptr, false, false);

    std::vector<int8_t> qflag(kNumIfov, 1);
    std::vector<int32_t> bdiv(kNumIfov, 9);
    ASSERT_TRUE(prod.AddField(
        data, "surface_z",
        prod.MakeFloatArray(kIfovDims, MakeSeqFloat(kNumIfov, 20.0f, 1.0f))));
    ASSERT_TRUE(prod.AddField(data, "hno3_qflag",
                              prod.MakeInt8Array(kIfovDims, qflag)));
    ASSERT_TRUE(prod.AddField(
        data, "atmosphere_mass_content_of_nitric_acid",
        prod.MakeFloatArray(kIfovDims, MakeSeqFloat(kNumIfov, 0.2f, 0.01f))));
    ASSERT_TRUE(
        prod.AddField(data, "hno3_bdiv", prod.MakeInt32Array(kIfovDims, bdiv)));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    VerifyCommonVariables(res, 12345);
    EXPECT_EQ(1, res.variables["validity"].int8_values[0]);
    EXPECT_EQ(9, res.variables["HNO3_column_density_validity"].int32_values[0]);
    EXPECT_THAT(res.variables["HNO3_column_density"].float_values[0],
                FloatNear(0.2f, 1e-6f));
  }

  // 3. IAS_02_O3.
  {
    const harp_ingestion_module* mod = FindIngestionModule("IAS_02_O3");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* data =
        PopulateIasiNgBase(prod, "IAS_02_O3_", nullptr, false, false);

    std::vector<uint8_t> qflag(kNumIfov, 3u);
    std::vector<uint32_t> bdiv(kNumIfov, 11u);
    ASSERT_TRUE(prod.AddField(
        data, "surface_z",
        prod.MakeFloatArray(kIfovDims, MakeSeqFloat(kNumIfov, 30.0f, 1.0f))));
    ASSERT_TRUE(
        prod.AddField(data, "o3_qflag", prod.MakeUint8Array(kIfovDims, qflag)));
    ASSERT_TRUE(prod.AddField(
        data, "atmosphere_mass_content_of_ozone",
        prod.MakeFloatArray(kIfovDims, MakeSeqFloat(kNumIfov, 0.3f, 0.01f))));
    ASSERT_TRUE(
        prod.AddField(data, "o3_bdiv", prod.MakeUint32Array(kIfovDims, bdiv)));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    VerifyCommonVariables(res, 12345);
    EXPECT_EQ(3, res.variables["validity"].int8_values[0]);
    EXPECT_EQ(11, res.variables["O3_column_density_validity"].int32_values[0]);
  }
}

TEST(HarpIngestIasiNgL2Test, IngestSo2AllColumnOptions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("IAS_02_SO2");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  CodaMemNode* data =
      PopulateIasiNgBase(prod, "IAS_02_SO2", nullptr, false, false);

  constexpr long kIfovDims[] = {kNumIfov};
  constexpr long kSo2AltDims[] = {kNumIfov, 5};
  std::vector<uint8_t> qflag(kNumIfov, 1u);
  ASSERT_TRUE(prod.AddField(
      data, "so2_col",
      prod.MakeFloatArray(kIfovDims, MakeSeqFloat(kNumIfov, 1.5f, 0.1f))));
  ASSERT_TRUE(
      prod.AddField(data, "so2_col_at_altitudes",
                    prod.MakeFloatArray(
                        kSo2AltDims, MakeSeqFloat(kNumIfov * 5, 10.0f, 1.0f))));
  ASSERT_TRUE(prod.AddField(
      data, "so2_altitude",
      prod.MakeFloatArray(kIfovDims, MakeSeqFloat(kNumIfov, 5000.0f, 10.0f))));
  ASSERT_TRUE(
      prod.AddField(data, "so2_qflag", prod.MakeUint8Array(kIfovDims, qflag)));

  struct So2Case {
    const char* opt;
    float expected_col0;
  };
  constexpr So2Case kCases[] = {
      {.opt = nullptr, .expected_col0 = 1.5f},
      {.opt = "so2_column=7km", .expected_col0 = 10.0f},
      {.opt = "so2_column=10km", .expected_col0 = 11.0f},
      {.opt = "so2_column=13km", .expected_col0 = 12.0f},
      {.opt = "so2_column=16km", .expected_col0 = 13.0f},
      {.opt = "so2_column=25km", .expected_col0 = 14.0f},
  };

  for (const auto& tc : kCases) {
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), tc.opt);
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["SO2_column_number_density"].float_values[0],
                FloatNear(tc.expected_col0, 1e-5f));
  }
}

TEST(HarpIngestIasiNgL2Test, IngestCldGhgSfcTwvSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  constexpr long kIfovDims[] = {kNumIfov};
  constexpr long kPairDims[] = {kNumIfov, 2};
  constexpr long kProfDims[] = {kNumIfov, kNumLevels};

  // 1. IAS_02_CLD.
  {
    const harp_ingestion_module* mod = FindIngestionModule("IAS_02_CLD");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* data =
        PopulateIasiNgBase(prod, "IAS_02_CLD", "n_clevels", false, true);

    std::vector<uint8_t> phase(kNumIfov * 2, 2u);
    ASSERT_TRUE(prod.AddField(
        data, "air_pressure_at_cloud_top",
        prod.MakeFloatArray(kPairDims,
                            MakeSeqFloat(kNumIfov * 2, 50000.0f, 10.0f))));
    ASSERT_TRUE(prod.AddField(
        data, "air_temperature_at_cloud_top",
        prod.MakeFloatArray(kPairDims,
                            MakeSeqFloat(kNumIfov * 2, 250.0f, 0.5f))));
    ASSERT_TRUE(
        prod.AddField(data, "effective_cloud_fraction",
                      prod.MakeFloatArray(
                          kPairDims, MakeSeqFloat(kNumIfov * 2, 0.1f, 0.01f))));
    ASSERT_TRUE(prod.AddField(
        data, "atmosphere_mass_content_of_cloud_ice",
        prod.MakeFloatArray(kIfovDims, MakeSeqFloat(kNumIfov, 1.0f, 0.1f))));
    ASSERT_TRUE(prod.AddField(
        data, "atmosphere_mass_content_of_cloud_liquid",
        prod.MakeFloatArray(kIfovDims, MakeSeqFloat(kNumIfov, 2.0f, 0.1f))));
    ASSERT_TRUE(prod.AddField(
        data, "thermodynamic_phase_of_cloud_water_particles_at_cloud_top",
        prod.MakeUint8Array(kPairDims, phase)));
    ASSERT_TRUE(prod.AddField(
        data,
        "effective_radius_of_cloud_condensed_water_particles_at_cloud_top",
        prod.MakeFloatArray(kIfovDims, MakeSeqFloat(kNumIfov, 1e-5f, 1e-6f))));
    ASSERT_TRUE(prod.AddField(
        data, "dust_indicator",
        prod.MakeFloatArray(kIfovDims, MakeSeqFloat(kNumIfov, 0.2f, 0.01f))));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(kNumLevels, res.dimensions[harp_dimension_vertical]);
    EXPECT_THAT(res.variables["ice_fraction"].float_values[0],
                FloatNear(-1.0f, 1e-6f));
    EXPECT_THAT(res.variables["ice_fraction"].float_values[1],
                FloatNear(0.5f, 1e-6f));
    EXPECT_THAT(res.variables["land_fraction"].float_values[0],
                FloatNear(0.8f, 1e-6f));
    EXPECT_THAT(res.variables["land_fraction"].float_values[1],
                FloatNear(150.0f, 1e-6f));
    EXPECT_EQ(2, res.variables["cloud_phase_type"].int8_values[0]);
  }

  // 2. IAS_02_GHG.
  {
    const harp_ingestion_module* mod = FindIngestionModule("IAS_02_GHG");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* data =
        PopulateIasiNgBase(prod, "IAS_02_GHG", "n_n2o", false, true);

    ASSERT_TRUE(prod.AddField(
        data, "atmosphere_mass_content_of_nitrous_oxide",
        prod.MakeFloatArray(kProfDims,
                            MakeSeqFloat(kNumIfov * kNumLevels, 0.1f, 0.01f))));
    ASSERT_TRUE(prod.AddField(
        data, "atmosphere_mass_content_of_methane",
        prod.MakeFloatArray(kProfDims,
                            MakeSeqFloat(kNumIfov * kNumLevels, 0.2f, 0.01f))));
    ASSERT_TRUE(prod.AddField(
        data, "atmosphere_mass_content_of_carbon_dioxide",
        prod.MakeFloatArray(kProfDims,
                            MakeSeqFloat(kNumIfov * kNumLevels, 0.3f, 0.01f))));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(kNumLevels, res.dimensions[harp_dimension_vertical]);
    EXPECT_EQ(kNumIfov * kNumLevels,
              res.variables["CO2_column_density"].float_values.size());
  }

  // 3. IAS_02_SFC.
  {
    const harp_ingestion_module* mod = FindIngestionModule("IAS_02_SFC");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* data =
        PopulateIasiNgBase(prod, "IAS_02_SFC", nullptr, false, true);

    CodaMemNode* l2p = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(data, "l2p_sst", l2p));
    ASSERT_TRUE(prod.AddField(
        l2p, "dust_indicator",
        prod.MakeFloatArray(kIfovDims, MakeSeqFloat(kNumIfov, 0.4f, 0.01f))));
    ASSERT_TRUE(prod.AddField(
        l2p, "wind_speed",
        prod.MakeFloatArray(kIfovDims, MakeSeqFloat(kNumIfov, 5.0f, 0.1f))));

    CodaMemNode* stat = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(data, "statistical_retrieval", stat));
    ASSERT_TRUE(prod.AddField(
        stat, "surface_temperature",
        prod.MakeFloatArray(kIfovDims, MakeSeqFloat(kNumIfov, 290.0f, 0.1f))));
    ASSERT_TRUE(prod.AddField(
        stat, "surface_air_pressure",
        prod.MakeFloatArray(kIfovDims, MakeSeqFloat(kNumIfov, 1013.0f, 0.1f))));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["wind_speed"].float_values[0],
                FloatNear(5.0f, 1e-6f));
    EXPECT_THAT(res.variables["surface_pressure"].float_values[0],
                FloatNear(1013.0f, 1e-6f));
  }

  // 4. IAS_02_TWV + direct read_corner_latitude on fresh init so
  // info->corner_latitude == NULL branch is exercised.
  {
    const harp_ingestion_module* mod = FindIngestionModule("IAS_02_TWV");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* data =
        PopulateIasiNgBase(prod, "IAS_02_TWV", "n_levels", false, true);

    CodaMemNode* oe = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(data, "optimal_estimation", oe));
    ASSERT_TRUE(prod.AddField(
        oe, "atmosphere_mass_content_of_water",
        prod.MakeFloatArray(kIfovDims, MakeSeqFloat(kNumIfov, 25.0f, 0.5f))));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(kNumLevels, res.dimensions[harp_dimension_vertical]);
    EXPECT_THAT(res.variables["water_vapor_column_density"].float_values[0],
                FloatNear(25.0f, 1e-6f));

    // Fresh init to call latitude_bounds before longitude_bounds.
    harp_ingestion_options* raw_opts = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_ingestion_options_new(&raw_opts));
    ScopedHarpIngestionOptions opts(raw_opts);
    harp_product_definition* prod_def = nullptr;
    void* user_data = nullptr;
    ASSERT_EQ(HARP_SUCCESS, mod->ingestion_init(mod, prod.product(), opts.get(),
                                                &prod_def, &user_data));
    auto done_guard =
        absl::MakeCleanup([&] { mod->ingestion_done(user_data); });

    const harp_variable_definition* lat_bnds_def = nullptr;
    for (int i = 0; i < prod_def->num_variable_definitions; ++i) {
      if (std::string_view(prod_def->variable_definition[i]->name) ==
          "latitude_bounds") {
        lat_bnds_def = prod_def->variable_definition[i];
        break;
      }
    }
    ASSERT_NE(nullptr, lat_bnds_def);
    double corners[4] = {};
    harp_array arr{};
    arr.double_data = corners;
    ASSERT_EQ(HARP_SUCCESS, lat_bnds_def->read_block(user_data, 0, arr));
    for (double v : corners) {
      EXPECT_TRUE(std::isfinite(v));
    }
  }
}

TEST(HarpIngestIasiNgL2Test, InitAndDimensionErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod_co = FindIngestionModule("IAS_02_CO");
  const harp_ingestion_module* mod_cld = FindIngestionModule("IAS_02_CLD");
  ASSERT_NE(nullptr, mod_co);
  ASSERT_NE(nullptr, mod_cld);

  // 1. Unsupported product type.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("EPS_SG", "UNKNOWN_TYPE", 1));
    IngestExerciseResult res = ExerciseIngestionModule(mod_co, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 3. Missing /data.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("EPS_SG", "IAS_02_CO_", 1));
    IngestExerciseResult res = ExerciseIngestionModule(mod_co, prod.product());
    EXPECT_EQ(-1, res.status);
  }

  // 4. Missing /data/geolocation_information.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("EPS_SG", "IAS_02_CO_", 1));
    ASSERT_TRUE(prod.AddField(prod.root(), "data", prod.MakeRecord()));
    IngestExerciseResult res = ExerciseIngestionModule(mod_co, prod.product());
    EXPECT_EQ(-1, res.status);
  }

  // 5. Missing n_lines, non-1D n_lines, scalar n_lines, missing n_for, missing
  // n_fov, missing n_clevels.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("EPS_SG", "IAS_02_CO_", 1));
    CodaMemNode* data = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(prod.root(), "data", data));
    ASSERT_TRUE(
        prod.AddField(data, "geolocation_information", prod.MakeRecord()));
    // Scalar n_lines -> coda_cursor_get_array_dim fails inside recursion.
    ASSERT_TRUE(prod.AddField(data, "n_lines", prod.MakeInt32(1)));
    IngestExerciseResult res = ExerciseIngestionModule(mod_co, prod.product());
    EXPECT_EQ(-1, res.status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("EPS_SG", "IAS_02_CO_", 1));
    CodaMemNode* data = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(prod.root(), "data", data));
    ASSERT_TRUE(
        prod.AddField(data, "geolocation_information", prod.MakeRecord()));
    constexpr long kTwoDims[] = {1, 2};
    constexpr int32_t kVals[] = {0, 0};
    ASSERT_TRUE(
        prod.AddField(data, "n_lines", prod.MakeInt32Array(kTwoDims, kVals)));
    IngestExerciseResult res = ExerciseIngestionModule(mod_co, prod.product());
    EXPECT_EQ(-1, res.status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("EPS_SG", "IAS_02_CO_", 1));
    CodaMemNode* data = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(prod.root(), "data", data));
    ASSERT_TRUE(
        prod.AddField(data, "geolocation_information", prod.MakeRecord()));
    constexpr long kOneDim[] = {1};
    constexpr int32_t kOneVal[] = {0};
    ASSERT_TRUE(
        prod.AddField(data, "n_lines", prod.MakeInt32Array(kOneDim, kOneVal)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_co, prod.product()).status);

    ASSERT_TRUE(
        prod.AddField(data, "n_for", prod.MakeInt32Array(kOneDim, kOneVal)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_co, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("EPS_SG", "IAS_02_CLD", 1));
    CodaMemNode* data = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(prod.root(), "data", data));
    ASSERT_TRUE(
        prod.AddField(data, "geolocation_information", prod.MakeRecord()));
    constexpr long kOneDim[] = {1};
    constexpr int32_t kOneVal[] = {0};
    ASSERT_TRUE(
        prod.AddField(data, "n_lines", prod.MakeInt32Array(kOneDim, kOneVal)));
    ASSERT_TRUE(
        prod.AddField(data, "n_for", prod.MakeInt32Array(kOneDim, kOneVal)));
    ASSERT_TRUE(
        prod.AddField(data, "n_fov", prod.MakeInt32Array(kOneDim, kOneVal)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_cld, prod.product()).status);
  }
}

TEST(HarpIngestIasiNgL2Test, ReadDatasetAndCornerErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod_co = FindIngestionModule("IAS_02_CO");
  const harp_ingestion_module* mod_cld = FindIngestionModule("IAS_02_CLD");
  ASSERT_NE(nullptr, mod_co);
  ASSERT_NE(nullptr, mod_cld);

  // 1. Wrong sizes, wrong types, missing fields on CO product.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("EPS_SG", "IAS_02_CO_", 1));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "orbit_start", prod.MakeString("bad")));

    CodaMemNode* data = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(prod.root(), "data", data));
    const std::vector<int32_t> lines_vec(kNumLines, 0);
    const std::vector<int32_t> for_vec(kNumFor, 0);
    const std::vector<int32_t> fov_vec(kNumFov, 0);
    constexpr long kLinesDims[] = {kNumLines};
    constexpr long kForDims[] = {kNumFor};
    constexpr long kFovDims[] = {kNumFov};
    ASSERT_TRUE(prod.AddField(data, "n_lines",
                              prod.MakeInt32Array(kLinesDims, lines_vec)));
    ASSERT_TRUE(
        prod.AddField(data, "n_for", prod.MakeInt32Array(kForDims, for_vec)));
    ASSERT_TRUE(
        prod.AddField(data, "n_fov", prod.MakeInt32Array(kFovDims, fov_vec)));

    CodaMemNode* geo = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(data, "geolocation_information", geo));
    constexpr long kBadDims[] = {1};
    constexpr long kIfovDims[] = {kNumIfov};
    constexpr double kBadDouble[] = {1.0};
    constexpr std::string_view kBadStrs[kNumIfov] = {};
    // Provide valid lat/lon so corner bounds succeed cleanly without leaving
    // uninitialized corner buffers, while onboard_utc and data arrays fail.
    ASSERT_TRUE(prod.AddField(
        geo, "sounder_pixel_latitude",
        prod.MakeDoubleArray(kIfovDims, MakeSeqDouble(kNumIfov, 10.0, 0.1))));
    ASSERT_TRUE(prod.AddField(
        geo, "sounder_pixel_longitude",
        prod.MakeDoubleArray(kIfovDims, MakeSeqDouble(kNumIfov, 20.0, 0.1))));
    ASSERT_TRUE(prod.AddField(geo, "onboard_utc",
                              prod.MakeDoubleArray(kBadDims, kBadDouble)));

    // Wrong types for int8, int32, float datasets.
    ASSERT_TRUE(prod.AddField(data, "co_qflag",
                              prod.MakeStringArray(kIfovDims, kBadStrs)));
    ASSERT_TRUE(prod.AddField(data, "co_bdiv",
                              prod.MakeStringArray(kIfovDims, kBadStrs)));
    ASSERT_TRUE(prod.AddField(data, "surface_z",
                              prod.MakeStringArray(kIfovDims, kBadStrs)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod_co, prod.product(), nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["surface_altitude"].read_status);
    EXPECT_EQ(-1, res.variables["validity"].read_status);
    EXPECT_EQ(-1, res.variables["CO_column_density_validity"].read_status);
    EXPECT_EQ(-1, res.variables["CO_column_density"].read_status);
  }

  // 2. Missing orbit attribute and slice/percentage failures on CLD.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("EPS_SG", "IAS_02_CLD", 1));
    CodaMemNode* data = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(prod.root(), "data", data));
    const std::vector<int32_t> lines_vec(kNumLines, 0);
    const std::vector<int32_t> for_vec(kNumFor, 0);
    const std::vector<int32_t> fov_vec(kNumFov, 0);
    const std::vector<int32_t> lvl_vec(kNumLevels, 0);
    constexpr long kLinesDims[] = {kNumLines};
    constexpr long kForDims[] = {kNumFor};
    constexpr long kFovDims[] = {kNumFov};
    constexpr long kLvlDims[] = {kNumLevels};
    ASSERT_TRUE(prod.AddField(data, "n_lines",
                              prod.MakeInt32Array(kLinesDims, lines_vec)));
    ASSERT_TRUE(
        prod.AddField(data, "n_for", prod.MakeInt32Array(kForDims, for_vec)));
    ASSERT_TRUE(
        prod.AddField(data, "n_fov", prod.MakeInt32Array(kFovDims, fov_vec)));
    ASSERT_TRUE(prod.AddField(data, "n_clevels",
                              prod.MakeInt32Array(kLvlDims, lvl_vec)));

    CodaMemNode* geo = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(data, "geolocation_information", geo));
    constexpr long kIfovDims[] = {kNumIfov};
    ASSERT_TRUE(prod.AddField(
        geo, "sounder_pixel_latitude",
        prod.MakeDoubleArray(kIfovDims, MakeSeqDouble(kNumIfov, 10.0, 0.1))));
    ASSERT_TRUE(prod.AddField(
        geo, "sounder_pixel_longitude",
        prod.MakeDoubleArray(kIfovDims, MakeSeqDouble(kNumIfov, 20.0, 0.1))));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod_cld, prod.product(), nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["ice_fraction"].read_status);
    EXPECT_EQ(-1, res.variables["cloud_top_pressure"].read_status);
    EXPECT_EQ(-1, res.variables["cloud_phase_type"].read_status);
  }

  // 3. Dedicated get_corner_coordinates error paths (missing latitude for
  // longitude_bounds, and missing longitude for latitude_bounds).
  for (bool missing_lat : {true, false}) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("EPS_SG", "IAS_02_CO_", 1));
    CodaMemNode* data = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(prod.root(), "data", data));
    const std::vector<int32_t> lines_vec(kNumLines, 0);
    const std::vector<int32_t> for_vec(kNumFor, 0);
    const std::vector<int32_t> fov_vec(kNumFov, 0);
    constexpr long kLinesDims[] = {kNumLines};
    constexpr long kForDims[] = {kNumFor};
    constexpr long kFovDims[] = {kNumFov};
    ASSERT_TRUE(prod.AddField(data, "n_lines",
                              prod.MakeInt32Array(kLinesDims, lines_vec)));
    ASSERT_TRUE(
        prod.AddField(data, "n_for", prod.MakeInt32Array(kForDims, for_vec)));
    ASSERT_TRUE(
        prod.AddField(data, "n_fov", prod.MakeInt32Array(kFovDims, fov_vec)));

    CodaMemNode* geo = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(data, "geolocation_information", geo));
    constexpr long kIfovDims[] = {kNumIfov};
    if (!missing_lat) {
      ASSERT_TRUE(prod.AddField(
          geo, "sounder_pixel_latitude",
          prod.MakeDoubleArray(kIfovDims, MakeSeqDouble(kNumIfov, 10.0, 0.1))));
    }

    harp_ingestion_options* raw_opts = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_ingestion_options_new(&raw_opts));
    ScopedHarpIngestionOptions opts(raw_opts);
    harp_product_definition* prod_def = nullptr;
    void* user_data = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              mod_co->ingestion_init(mod_co, prod.product(), opts.get(),
                                     &prod_def, &user_data));
    auto done_guard =
        absl::MakeCleanup([&] { mod_co->ingestion_done(user_data); });

    const char* target_var =
        missing_lat ? "longitude_bounds" : "latitude_bounds";
    const harp_variable_definition* var_def = nullptr;
    for (int i = 0; i < prod_def->num_variable_definitions; ++i) {
      if (std::string_view(prod_def->variable_definition[i]->name) ==
          target_var) {
        var_def = prod_def->variable_definition[i];
        break;
      }
    }
    ASSERT_NE(nullptr, var_def);
    double corners[4] = {};
    harp_array arr{};
    arr.double_data = corners;
    ASSERT_EQ(-1, var_def->read_block(user_data, 0, arr));
  }

  // 4. Empty /@orbit_start array and wrong-type double dataset (onboard_utc).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("EPS_SG", "IAS_02_CO_", 1));
    constexpr long kZeroDim[] = {0};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "orbit_start",
                                  prod.MakeInt32Array(kZeroDim, {})));
    CodaMemNode* data = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(prod.root(), "data", data));
    const std::vector<int32_t> lines_vec(kNumLines, 0);
    const std::vector<int32_t> for_vec(kNumFor, 0);
    const std::vector<int32_t> fov_vec(kNumFov, 0);
    constexpr long kLinesDims[] = {kNumLines};
    constexpr long kForDims[] = {kNumFor};
    constexpr long kFovDims[] = {kNumFov};
    ASSERT_TRUE(prod.AddField(data, "n_lines",
                              prod.MakeInt32Array(kLinesDims, lines_vec)));
    ASSERT_TRUE(
        prod.AddField(data, "n_for", prod.MakeInt32Array(kForDims, for_vec)));
    ASSERT_TRUE(
        prod.AddField(data, "n_fov", prod.MakeInt32Array(kFovDims, fov_vec)));

    CodaMemNode* geo = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(data, "geolocation_information", geo));
    constexpr long kUtcDims[] = {kNumLines, kNumFor};
    constexpr std::string_view kBadUtc[] = {"a", "b"};
    ASSERT_TRUE(prod.AddField(geo, "onboard_utc",
                              prod.MakeStringArray(kUtcDims, kBadUtc)));
    constexpr long kIfovDims[] = {kNumIfov};
    ASSERT_TRUE(prod.AddField(
        geo, "sounder_pixel_latitude",
        prod.MakeDoubleArray(kIfovDims, MakeSeqDouble(kNumIfov, 10.0, 0.1))));
    ASSERT_TRUE(prod.AddField(
        geo, "sounder_pixel_longitude",
        prod.MakeDoubleArray(kIfovDims, MakeSeqDouble(kNumIfov, 20.0, 0.1))));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod_co, prod.product(), nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
  }

  // 5. Scalar co_qflag/co_bdiv when num_ifov == 1 -> coda_num_elements == 1,
  // but coda_cursor_goto_first_array_element fails in read_dataset.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("EPS_SG", "IAS_02_CO_", 1));
    CodaMemNode* data = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(prod.root(), "data", data));
    constexpr long kOneDim[] = {1};
    constexpr int32_t kZero[] = {0};
    ASSERT_TRUE(
        prod.AddField(data, "n_lines", prod.MakeInt32Array(kOneDim, kZero)));
    ASSERT_TRUE(
        prod.AddField(data, "n_for", prod.MakeInt32Array(kOneDim, kZero)));
    ASSERT_TRUE(
        prod.AddField(data, "n_fov", prod.MakeInt32Array(kOneDim, kZero)));
    ASSERT_TRUE(
        prod.AddField(data, "geolocation_information", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(data, "co_qflag", prod.MakeInt8(1)));
    ASSERT_TRUE(prod.AddField(data, "co_bdiv", prod.MakeInt32(1)));

    // Call validity and CO_column_density_validity directly without triggering
    // get_corner_coordinates (which assumes num_fov == 16).
    harp_ingestion_options* raw_opts = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_ingestion_options_new(&raw_opts));
    ScopedHarpIngestionOptions opts(raw_opts);
    harp_product_definition* prod_def = nullptr;
    void* user_data = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              mod_co->ingestion_init(mod_co, prod.product(), opts.get(),
                                     &prod_def, &user_data));
    auto done_guard =
        absl::MakeCleanup([&] { mod_co->ingestion_done(user_data); });

    int8_t qflag_val = 0;
    int32_t bdiv_val = 0;
    for (int i = 0; i < prod_def->num_variable_definitions; ++i) {
      const harp_variable_definition* vd = prod_def->variable_definition[i];
      if (std::string_view(vd->name) == "validity") {
        harp_array arr{};
        arr.int8_data = &qflag_val;
        EXPECT_EQ(-1, vd->read_all(user_data, arr));
      } else if (std::string_view(vd->name) == "CO_column_density_validity") {
        harp_array arr{};
        arr.int32_data = &bdiv_val;
        EXPECT_EQ(-1, vd->read_all(user_data, arr));
      }
    }
  }
}

}  // namespace
}  // namespace stcorp_harp
