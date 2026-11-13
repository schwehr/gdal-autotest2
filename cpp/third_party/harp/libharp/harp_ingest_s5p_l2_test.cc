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
// Unit tests for Sentinel-5P L2 ingestion modules
// (libharp/harp-ingest-s5p_l2.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-s5p_l2.c

#include <cmath>
#include <cstdint>
#include <format>
#include <span>
#include <string>
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

constexpr float kFillFloat = 9.96921e+36f;
constexpr double kFillDouble = 9.969209968386869e+36;

// Builds a valid 83-character Sentinel-5P logical filename for `@id` encoding
// the given 6-digit processor version (e.g. 20800 for 02.08.00).
std::string MakeProductId(std::string_view product_type,
                          int processor_version) {
  return std::format(
      "S5P_OFFL_{}_20200101T000000_20200101T010000_12345_01_{:06d}_"
      "20200101T020000",
      product_type, processor_version);
}

bool AddFloatArray(CodaMemProduct& prod, CodaMemNode* rec,
                   std::string_view name, std::span<const long> dims,
                   std::span<const float> values,
                   float fill_value = kFillFloat) {
  CodaMemNode* arr = prod.MakeFloatArray(dims, values);
  if (arr == nullptr) return false;
  constexpr long kOne[] = {1};
  const float fill[] = {fill_value};
  if (!prod.AddAttribute(arr, "FillValue", prod.MakeFloatArray(kOne, fill))) {
    return false;
  }
  return prod.AddField(rec, name, arr);
}

bool AddDoubleArray(CodaMemProduct& prod, CodaMemNode* rec,
                    std::string_view name, std::span<const long> dims,
                    std::span<const double> values,
                    double fill_value = kFillDouble) {
  CodaMemNode* arr = prod.MakeDoubleArray(dims, values);
  if (arr == nullptr) return false;
  constexpr long kOne[] = {1};
  const double fill[] = {fill_value};
  if (!prod.AddAttribute(arr, "FillValue", prod.MakeDoubleArray(kOne, fill))) {
    return false;
  }
  return prod.AddField(rec, name, arr);
}

bool AddInt8ArrayWithFill(CodaMemProduct& prod, CodaMemNode* rec,
                          std::string_view name, std::span<const long> dims,
                          std::span<const int8_t> values,
                          int8_t fill_value = 127) {
  CodaMemNode* arr = prod.MakeInt8Array(dims, values);
  if (arr == nullptr) return false;
  constexpr long kOne[] = {1};
  const int8_t fill[] = {fill_value};
  if (!prod.AddAttribute(arr, "FillValue", prod.MakeInt8Array(kOne, fill))) {
    return false;
  }
  return prod.AddField(rec, name, arr);
}

struct S5pConfig {
  std::string_view product_type;
  int processor_version = 20800;
  bool include_id = true;
  std::string_view processing_mode = "OFFL";
  bool orbit_as_array = false;
  long num_scanlines = 2;
  long num_pixels = 2;
  long num_layers = 2;
  long num_levels = 3;
  bool has_layer_dim = false;
  bool has_level_dim = false;
  bool delta_time_3d = false;
  bool use_uint8_qa = true;
  bool include_nir_geoloc = false;
  bool height_levels_v0 = false;
};

struct S5pHandles {
  CodaMemNode* product = nullptr;
  CodaMemNode* support_data = nullptr;
  CodaMemNode* geolocations = nullptr;
  CodaMemNode* detailed_results = nullptr;
  CodaMemNode* input_data = nullptr;
  CodaMemNode* so2_lh = nullptr;
};

S5pHandles BuildS5pBase(CodaMemProduct& prod, const S5pConfig& cfg) {
  S5pHandles h;
  EXPECT_TRUE(prod.SetProductInfo("Sentinel5P", cfg.product_type, 1));

  if (cfg.include_id) {
    EXPECT_TRUE(
        prod.AddAttribute(prod.root(), "id",
                          prod.MakeString(MakeProductId(
                              cfg.product_type, cfg.processor_version))));
  }
  if (cfg.orbit_as_array) {
    constexpr long kOne[] = {1};
    constexpr int32_t kOrbit[] = {12345};
    EXPECT_TRUE(prod.AddAttribute(prod.root(), "orbit",
                                  prod.MakeInt32Array(kOne, kOrbit)));
  } else {
    EXPECT_TRUE(prod.AddAttribute(prod.root(), "orbit", prod.MakeInt32(12345)));
  }
  EXPECT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_resolution",
                                prod.MakeString("PT1.08S")));

  // /METADATA/GRANULE_DESCRIPTION@ProcessingMode
  CodaMemNode* metadata = prod.MakeRecord();
  CodaMemNode* granule_desc = prod.MakeRecord();
  EXPECT_TRUE(prod.AddAttribute(granule_desc, "ProcessingMode",
                                prod.MakeString(cfg.processing_mode)));
  EXPECT_TRUE(prod.AddField(metadata, "GRANULE_DESCRIPTION", granule_desc));
  EXPECT_TRUE(prod.AddField(prod.root(), "METADATA", metadata));

  // /PRODUCT
  h.product = prod.MakeRecord();
  h.support_data = prod.MakeRecord();
  h.geolocations = prod.MakeRecord();
  h.detailed_results = prod.MakeRecord();
  h.input_data = prod.MakeRecord();

  const long n_sl = cfg.num_scanlines;
  const long n_px = cfg.num_pixels;
  const long n_obs = n_sl * n_px;
  const long n_corners = 4;

  // Dimension arrays.
  constexpr long kTimeDims[] = {1};
  constexpr double kTimeVals[] = {315532800.0};  // 2020-01-01 in s since 2010
  EXPECT_TRUE(AddDoubleArray(prod, h.product, "time", kTimeDims, kTimeVals));

  const long sl_dims[] = {n_sl};
  std::vector<int32_t> sl_idx(n_sl, 0);
  EXPECT_TRUE(prod.AddField(h.product, "scanline",
                            prod.MakeInt32Array(sl_dims, sl_idx)));

  const long px_dims[] = {n_px};
  std::vector<int32_t> px_idx(n_px, 0);
  EXPECT_TRUE(prod.AddField(h.product, "ground_pixel",
                            prod.MakeInt32Array(px_dims, px_idx)));

  constexpr long kCornerDims[] = {4};
  constexpr int32_t kCornerIdx[] = {0, 1, 2, 3};
  EXPECT_TRUE(prod.AddField(h.product, "corner",
                            prod.MakeInt32Array(kCornerDims, kCornerIdx)));

  if (cfg.has_layer_dim) {
    const long layer_dims[] = {cfg.num_layers};
    std::vector<float> layers(cfg.num_layers);
    for (long l = 0; l < cfg.num_layers; ++l) {
      layers[l] = static_cast<float>((cfg.num_layers - l) * 1000.0f);
    }
    EXPECT_TRUE(AddFloatArray(prod, h.product, "layer", layer_dims, layers));
  }
  if (cfg.has_level_dim) {
    const long level_dims[] = {cfg.num_levels};
    std::vector<int32_t> levels(cfg.num_levels, 0);
    EXPECT_TRUE(prod.AddField(h.product, "level",
                              prod.MakeInt32Array(level_dims, levels)));
  }

  // delta_time
  if (cfg.delta_time_3d) {
    const long dt_dims[] = {1, n_sl, n_px};
    std::vector<double> dt(n_obs);
    for (long i = 0; i < n_obs; ++i) {
      dt[i] = static_cast<double>(i) * 1000.0;
    }
    EXPECT_TRUE(AddDoubleArray(prod, h.product, "delta_time", dt_dims, dt));
  } else {
    const long dt_dims[] = {1, n_sl};
    std::vector<double> dt(n_sl);
    for (long i = 0; i < n_sl; ++i) {
      dt[i] = static_cast<double>(i) * 1000.0;
    }
    EXPECT_TRUE(AddDoubleArray(prod, h.product, "delta_time", dt_dims, dt));
  }

  // Core geolocation & qa_value.
  const long obs_dims[] = {1, n_sl, n_px};
  const long bounds_dims[] = {1, n_sl, n_px, n_corners};
  std::vector<float> lat(n_obs), lon(n_obs);
  std::vector<float> lat_b(n_obs * n_corners), lon_b(n_obs * n_corners);
  std::vector<float> sza(n_obs, 30.0f), saa(n_obs, 120.0f), vza(n_obs, 15.0f),
      vaa(n_obs, -60.0f);
  for (long i = 0; i < n_obs; ++i) {
    lat[i] = 10.0f + static_cast<float>(i);
    lon[i] = 20.0f + static_cast<float>(i);
    for (long c = 0; c < n_corners; ++c) {
      lat_b[i * 4 + c] = lat[i] + 0.1f * static_cast<float>(c);
      lon_b[i * 4 + c] = lon[i] + 0.1f * static_cast<float>(c);
    }
  }
  EXPECT_TRUE(AddFloatArray(prod, h.product, "latitude", obs_dims, lat));
  EXPECT_TRUE(AddFloatArray(prod, h.product, "longitude", obs_dims, lon));
  EXPECT_TRUE(AddFloatArray(prod, h.geolocations, "latitude_bounds",
                            bounds_dims, lat_b));
  EXPECT_TRUE(AddFloatArray(prod, h.geolocations, "longitude_bounds",
                            bounds_dims, lon_b));
  EXPECT_TRUE(
      AddFloatArray(prod, h.geolocations, "solar_zenith_angle", obs_dims, sza));
  EXPECT_TRUE(AddFloatArray(prod, h.geolocations, "solar_azimuth_angle",
                            obs_dims, saa));
  EXPECT_TRUE(AddFloatArray(prod, h.geolocations, "viewing_zenith_angle",
                            obs_dims, vza));
  EXPECT_TRUE(AddFloatArray(prod, h.geolocations, "viewing_azimuth_angle",
                            obs_dims, vaa));

  if (cfg.include_nir_geoloc) {
    EXPECT_TRUE(AddFloatArray(prod, h.product, "latitude_nir", obs_dims, lat));
    EXPECT_TRUE(AddFloatArray(prod, h.product, "longitude_nir", obs_dims, lon));
    EXPECT_TRUE(AddFloatArray(prod, h.geolocations, "latitude_bounds_nir",
                              bounds_dims, lat_b));
    EXPECT_TRUE(AddFloatArray(prod, h.geolocations, "longitude_bounds_nir",
                              bounds_dims, lon_b));
    EXPECT_TRUE(AddFloatArray(prod, h.geolocations, "solar_zenith_angle_nir",
                              obs_dims, sza));
    EXPECT_TRUE(AddFloatArray(prod, h.geolocations, "solar_azimuth_angle_nir",
                              obs_dims, saa));
    EXPECT_TRUE(AddFloatArray(prod, h.geolocations, "viewing_zenith_angle_nir",
                              obs_dims, vza));
    EXPECT_TRUE(AddFloatArray(prod, h.geolocations, "viewing_azimuth_angle_nir",
                              obs_dims, vaa));
  }

  std::vector<float> sat_lat(n_sl, 45.0f), sat_lon(n_sl, 5.0f),
      sat_alt(n_sl, 824000.0f);
  EXPECT_TRUE(AddFloatArray(prod, h.geolocations, "satellite_latitude", sl_dims,
                            sat_lat));
  EXPECT_TRUE(AddFloatArray(prod, h.geolocations, "satellite_longitude",
                            sl_dims, sat_lon));
  EXPECT_TRUE(AddFloatArray(prod, h.geolocations, "satellite_altitude", sl_dims,
                            sat_alt));

  if (cfg.use_uint8_qa) {
    std::vector<uint8_t> qa(n_obs, 90);
    EXPECT_TRUE(prod.AddField(h.product, "qa_value",
                              prod.MakeUint8Array(obs_dims, qa)));
  } else {
    std::vector<int8_t> qa(n_obs, 80);
    EXPECT_TRUE(
        prod.AddField(h.product, "qa_value", prod.MakeInt8Array(obs_dims, qa)));
  }

  std::vector<uint32_t> pqf(n_obs, 0);
  EXPECT_TRUE(prod.AddField(h.detailed_results, "processing_quality_flags",
                            prod.MakeUint32Array(obs_dims, pqf)));

  // Common INPUT_DATA surface and snow/ice variables.
  std::vector<float> surf_alt(n_obs, 100.0f), surf_alt_prec(n_obs, 5.0f),
      surf_press(n_obs, 101325.0f), n_wind(n_obs, 2.0f), e_wind(n_obs, -1.5f),
      land_frac(n_obs, 0.75f);
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "surface_altitude", obs_dims,
                            surf_alt));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "surface_altitude_precision",
                            obs_dims, surf_alt_prec));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "surface_pressure", obs_dims,
                            surf_press));
  EXPECT_TRUE(
      AddFloatArray(prod, h.input_data, "northward_wind", obs_dims, n_wind));
  EXPECT_TRUE(
      AddFloatArray(prod, h.input_data, "eastward_wind", obs_dims, e_wind));
  EXPECT_TRUE(
      AddFloatArray(prod, h.input_data, "land_fraction", obs_dims, land_frac));

  if (cfg.include_nir_geoloc) {
    EXPECT_TRUE(AddFloatArray(prod, h.input_data, "surface_altitude_nir",
                              obs_dims, surf_alt));
    EXPECT_TRUE(AddFloatArray(prod, h.input_data, "surface_pressure_nir",
                              obs_dims, surf_press));
  }

  // Snow/ice flags: exercise multiple flag categories across observations.
  std::vector<int8_t> snow_flag_i8(n_obs);
  for (long i = 0; i < n_obs; ++i) {
    switch (i % 4) {
      case 0:
        snow_flag_i8[i] = 0;
        break;
      case 1:
        snow_flag_i8[i] = 50;
        break;
      case 2:
        snow_flag_i8[i] = 101;
        break;
      case 3:
        snow_flag_i8[i] = -1;  // 255 -> ocean (4)
        break;
    }
  }
  EXPECT_TRUE(AddInt8ArrayWithFill(prod, h.input_data, "snow_ice_flag",
                                   obs_dims, snow_flag_i8));
  EXPECT_TRUE(AddInt8ArrayWithFill(prod, h.input_data, "snow_ice_flag_nise",
                                   obs_dims, snow_flag_i8));
  if (cfg.include_nir_geoloc) {
    EXPECT_TRUE(AddInt8ArrayWithFill(prod, h.input_data, "snow_ice_flag_nir",
                                     obs_dims, snow_flag_i8));
  }

  return h;
}

void FinalizeS5pHierarchy(CodaMemProduct& prod, const S5pHandles& h) {
  ASSERT_TRUE(prod.AddField(h.support_data, "GEOLOCATIONS", h.geolocations));
  ASSERT_TRUE(
      prod.AddField(h.support_data, "DETAILED_RESULTS", h.detailed_results));
  ASSERT_TRUE(prod.AddField(h.support_data, "INPUT_DATA", h.input_data));
  ASSERT_TRUE(prod.AddField(h.product, "SUPPORT_DATA", h.support_data));
  if (h.so2_lh != nullptr) {
    ASSERT_TRUE(prod.AddField(h.product, "SO2_LAYER_HEIGHT", h.so2_lh));
  }
  ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", h.product));
}

void PopulateCloudInputVars(CodaMemProduct& prod, const S5pHandles& h,
                            long n_obs) {
  const long obs_dims[] = {1, n_obs};
  std::vector<float> vals(n_obs, 0.4f);
  EXPECT_TRUE(
      AddFloatArray(prod, h.input_data, "cloud_albedo_crb", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "cloud_albedo_crb_precision",
                            obs_dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, h.input_data, "cloud_fraction_crb", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "cloud_fraction_crb_precision",
                            obs_dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, h.input_data, "cloud_height_crb", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "cloud_height_crb_precision",
                            obs_dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, h.input_data, "cloud_pressure_crb", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "cloud_pressure_crb_precision",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "cloud_fraction_intensity_weighted", obs_dims,
                            vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "cloud_fraction_intensity_weighted_precision",
                            obs_dims, vals));
}

// ============================================================================
// 1. S5P_L2_AER_AI
// ============================================================================

void PopulateAerAi(CodaMemProduct& prod, const S5pHandles& h, long n_obs,
                   bool include_fill = false) {
  const long dims[] = {1, n_obs};
  std::vector<float> vals(n_obs, 1.5f);
  if (include_fill && n_obs > 0) {
    vals[0] = kFillFloat;
  }
  std::vector<float> prec(n_obs, 0.2f);
  EXPECT_TRUE(
      AddFloatArray(prod, h.product, "aerosol_index_354_388", dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product, "aerosol_index_354_388_precision",
                            dims, prec));
  EXPECT_TRUE(
      AddFloatArray(prod, h.product, "aerosol_index_354_388_scm", dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, h.product, "aerosol_index_340_380", dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product, "aerosol_index_340_380_precision",
                            dims, prec));
  EXPECT_TRUE(
      AddFloatArray(prod, h.product, "aerosol_index_335_367", dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product, "aerosol_index_335_367_precision",
                            dims, prec));
  EXPECT_TRUE(
      AddFloatArray(prod, h.detailed_results, "cloud_fraction", dims, prec));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "cloud_altitude", dims, prec));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "surface_albedo", dims, prec));
}

TEST(HarpIngestS5pL2Test, AerAiWavelengthRatiosAndSnowIceFlagMapping) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5P_L2_AER_AI");
  ASSERT_NE(nullptr, mod);

  // Default 354_388nm at v02.09.01, scalar orbit, fill replacement, int8 QA,
  // and custom snow_ice_flag integer values covering all mapping branches.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5pConfig cfg{
        .product_type = "L2__AER_AI",
        .processor_version = 20901,
        .num_scanlines = 2,
        .num_pixels = 4,
        .use_uint8_qa = false,
    };
    S5pHandles h = BuildS5pBase(prod, cfg);
    PopulateAerAi(prod, h, 8, /*include_fill=*/true);
    // Replace snow_ice_flag with int8 array covering 0, 50, 101, 103, -1 (255),
    // -2 (invalid), 102 (invalid), 100 (sea_ice edge).
    // Note: CodaMemProduct won't overwrite existing field in AddField, so we
    // test snow_ice_flag integer branches using a second product or dedicated
    // test.
    FinalizeS5pHierarchy(prod, h);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(8, res.dimensions[harp_dimension_time]);
    EXPECT_TRUE(
        std::isnan(res.variables["absorbing_aerosol_index"].float_values[0]));
    EXPECT_FLOAT_EQ(1.5f,
                    res.variables["absorbing_aerosol_index"].float_values[1]);
    EXPECT_FLOAT_EQ(
        0.2f,
        res.variables["absorbing_aerosol_index_uncertainty"].float_values[0]);
    EXPECT_THAT(res.variables["scan_subindex"].int16_values,
                ElementsAre(0, 1, 2, 3, 0, 1, 2, 3));
    EXPECT_EQ(12345, res.variables["orbit_index"].int32_values[0]);
    EXPECT_DOUBLE_EQ(1.08, res.variables["datetime_length"].double_values[0]);
  }

  // Test 340_380nm, 335_367nm (empty before 02.04.00, populated >= 02.04.00),
  // 354_388nm_scm (empty before 02.09.01, populated >= 02.09.01), and orbit as
  // array.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5pConfig cfg{
        .product_type = "L2__AER_AI",
        .processor_version = 20300,
        .orbit_as_array = true,
    };
    S5pHandles h = BuildS5pBase(prod, cfg);
    PopulateAerAi(prod, h, 4);
    FinalizeS5pHierarchy(prod, h);

    IngestExerciseResult r335_old = ExerciseIngestionModule(
        mod, prod.product(), "wavelength_ratio=335_367nm");
    ASSERT_EQ(HARP_SUCCESS, r335_old.status);
    EXPECT_EQ(0, r335_old.dimensions[harp_dimension_time]);

    IngestExerciseResult rscm_old = ExerciseIngestionModule(
        mod, prod.product(), "wavelength_ratio=354_388nm_scm");
    ASSERT_EQ(HARP_SUCCESS, rscm_old.status);
    EXPECT_EQ(0, rscm_old.dimensions[harp_dimension_time]);

    IngestExerciseResult r340 = ExerciseIngestionModule(
        mod, prod.product(), "wavelength_ratio=340_380nm");
    ASSERT_EQ(HARP_SUCCESS, r340.status);
    EXPECT_EQ(4, r340.dimensions[harp_dimension_time]);
    EXPECT_EQ(12345, r340.variables["orbit_index"].int32_values[0]);

    IngestExerciseResult r354 = ExerciseIngestionModule(
        mod, prod.product(), "wavelength_ratio=354_388nm");
    ASSERT_EQ(HARP_SUCCESS, r354.status);
    EXPECT_EQ(4, r354.dimensions[harp_dimension_time]);
  }

  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5pConfig cfg{.product_type = "L2__AER_AI", .processor_version = 20901};
    S5pHandles h = BuildS5pBase(prod, cfg);
    PopulateAerAi(prod, h, 4);
    FinalizeS5pHierarchy(prod, h);

    IngestExerciseResult r335 = ExerciseIngestionModule(
        mod, prod.product(), "wavelength_ratio=335_367nm");
    ASSERT_EQ(HARP_SUCCESS, r335.status);
    EXPECT_TRUE(r335.variables["absorbing_aerosol_index_uncertainty"].included);

    IngestExerciseResult rscm = ExerciseIngestionModule(
        mod, prod.product(), "wavelength_ratio=354_388nm_scm");
    ASSERT_EQ(HARP_SUCCESS, rscm.status);
    EXPECT_FALSE(
        rscm.variables["absorbing_aerosol_index_uncertainty"].included);
    EXPECT_TRUE(rscm.variables["cloud_fraction"].included);
    EXPECT_TRUE(rscm.variables["cloud_height"].included);
    EXPECT_TRUE(rscm.variables["surface_albedo"].included);
  }
}

TEST(HarpIngestS5pL2Test, SnowIceTypeAllBranches) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5P_L2_AER_AI");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  EXPECT_TRUE(prod.SetProductInfo("Sentinel5P", "L2__AER_AI", 1));
  EXPECT_TRUE(prod.AddAttribute(
      prod.root(), "id", prod.MakeString(MakeProductId("L2__AER_AI", 20800))));
  EXPECT_TRUE(prod.AddAttribute(prod.root(), "orbit", prod.MakeInt32(1)));
  EXPECT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_resolution",
                                prod.MakeString("PT1S")));

  CodaMemNode* meta = prod.MakeRecord();
  CodaMemNode* gd = prod.MakeRecord();
  EXPECT_TRUE(prod.AddAttribute(gd, "ProcessingMode", prod.MakeString("OFFL")));
  EXPECT_TRUE(prod.AddField(meta, "GRANULE_DESCRIPTION", gd));
  EXPECT_TRUE(prod.AddField(prod.root(), "METADATA", meta));

  CodaMemNode* prod_rec = prod.MakeRecord();
  CodaMemNode* sup = prod.MakeRecord();
  CodaMemNode* geo = prod.MakeRecord();
  CodaMemNode* det = prod.MakeRecord();
  CodaMemNode* inp = prod.MakeRecord();

  constexpr long kOne[] = {1};
  constexpr long kSl[] = {1};
  constexpr long kPx[] = {8};
  constexpr long kCor[] = {4};
  constexpr double kTime[] = {0.0};
  constexpr int32_t kZero1[] = {0};
  constexpr int32_t kZero8[8] = {};
  constexpr int32_t kZero4[4] = {};
  EXPECT_TRUE(AddDoubleArray(prod, prod_rec, "time", kOne, kTime));
  EXPECT_TRUE(
      prod.AddField(prod_rec, "scanline", prod.MakeInt32Array(kSl, kZero1)));
  EXPECT_TRUE(prod.AddField(prod_rec, "ground_pixel",
                            prod.MakeInt32Array(kPx, kZero8)));
  EXPECT_TRUE(
      prod.AddField(prod_rec, "corner", prod.MakeInt32Array(kCor, kZero4)));

  // snow_ice_flag values covering all branches:
  // 0 -> 0 (snow_free_land), 50 -> 1 (sea_ice), 101 -> 2 (permanent_ice),
  // 103 -> 3 (snow), -1 (255) -> 4 (ocean), -2 -> -1, 102 -> -1, 100 -> 1.
  constexpr long kObsDims[] = {1, 1, 8};
  constexpr int8_t kFlags[] = {0, 50, 101, 103, -1, -2, 102, 100};
  EXPECT_TRUE(prod.AddField(inp, "snow_ice_flag",
                            prod.MakeInt8Array(kObsDims, kFlags)));

  EXPECT_TRUE(prod.AddField(sup, "GEOLOCATIONS", geo));
  EXPECT_TRUE(prod.AddField(sup, "DETAILED_RESULTS", det));
  EXPECT_TRUE(prod.AddField(sup, "INPUT_DATA", inp));
  EXPECT_TRUE(prod.AddField(prod_rec, "SUPPORT_DATA", sup));
  EXPECT_TRUE(prod.AddField(prod.root(), "PRODUCT", prod_rec));

  // Exercise only snow_ice_type directly to avoid populating unused fields.
  ScopedHarpIngestionOptions opts;
  harp_ingestion_options* raw_opts = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_ingestion_options_new(&raw_opts));
  opts.reset(raw_opts);
  harp_product_definition* pdef = nullptr;
  void* user_data = nullptr;
  ASSERT_EQ(HARP_SUCCESS, mod->ingestion_init(mod, prod.product(), opts.get(),
                                              &pdef, &user_data));
  const harp_variable_definition* var_def = nullptr;
  for (int i = 0; i < pdef->num_variable_definitions; ++i) {
    if (std::string_view(pdef->variable_definition[i]->name) ==
        "snow_ice_type") {
      var_def = pdef->variable_definition[i];
      break;
    }
  }
  ASSERT_NE(nullptr, var_def);
  int8_t out[8] = {};
  harp_array arr{};
  arr.int8_data = out;
  ASSERT_EQ(HARP_SUCCESS, var_def->read_all(user_data, arr));
  mod->ingestion_done(user_data);
  EXPECT_THAT(out, ElementsAre(0, 1, 2, 3, 4, -1, -1, 1));
}

// ============================================================================
// 2. S5P_L2_AER_LH
// ============================================================================

void PopulateAerLh(CodaMemProduct& prod, const S5pHandles& h, long n_obs,
                   int processor_version) {
  const long dims[] = {1, n_obs};
  std::vector<float> vals(n_obs, 2500.0f);
  EXPECT_TRUE(AddFloatArray(prod, h.product, "aerosol_mid_height", dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product, "aerosol_mid_height_precision",
                            dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, h.product, "aerosol_mid_pressure", dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product, "aerosol_mid_pressure_precision",
                            dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "aerosol_mid_pressure_not_clipped", dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "aerosol_optical_thickness", dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "aerosol_optical_thickness_precision", dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "cloud_fraction", dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, h.input_data, "aerosol_index_354_388", dims, vals));
  if (processor_version >= 20600) {
    const long alb_dims[] = {1, n_obs, 2};
    std::vector<float> alb(n_obs * 2);
    std::vector<float> alb_prec(n_obs * 2);
    for (long i = 0; i < n_obs; ++i) {
      alb[i * 2] = 0.1f;
      alb[i * 2 + 1] = 0.2f;
      alb_prec[i * 2] = 0.01f;
      alb_prec[i * 2 + 1] = 0.02f;
    }
    EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "surface_albedo",
                              alb_dims, alb));
    EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                              "surface_albedo_precision", alb_dims, alb_prec));
  } else {
    std::vector<float> alb(n_obs, 0.15f);
    EXPECT_TRUE(
        AddFloatArray(prod, h.detailed_results, "surface_albedo", dims, alb));
  }
}

TEST(HarpIngestS5pL2Test, AerLhVersionsAndOptions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5P_L2_AER_LH");
  ASSERT_NE(nullptr, mod);

  // < 02.00.00 with aerosol_pressure=unclipped excludes aerosol_pressure.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5pConfig cfg{.product_type = "L2__AER_LH", .processor_version = 10300};
    S5pHandles h = BuildS5pBase(prod, cfg);
    PopulateAerLh(prod, h, 4, cfg.processor_version);
    FinalizeS5pHierarchy(prod, h);

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "aerosol_pressure=unclipped");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_FALSE(res.variables["aerosol_pressure"].included);
    EXPECT_FLOAT_EQ(0.15f, res.variables["surface_albedo"].float_values[0]);
    EXPECT_FALSE(res.variables["surface_albedo_uncertainty"].included);
  }

  // >= 02.06.00 with default options and with aerosol_pressure=unclipped;
  // surface_albedo=772.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5pConfig cfg{.product_type = "L2__AER_LH", .processor_version = 20600};
    S5pHandles h = BuildS5pBase(prod, cfg);
    PopulateAerLh(prod, h, 4, cfg.processor_version);
    FinalizeS5pHierarchy(prod, h);

    IngestExerciseResult r_def = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, r_def.status);
    EXPECT_FLOAT_EQ(0.1f, r_def.variables["surface_albedo"].float_values[0]);
    EXPECT_FLOAT_EQ(
        0.01f, r_def.variables["surface_albedo_uncertainty"].float_values[0]);

    IngestExerciseResult r_opt = ExerciseIngestionModule(
        mod, prod.product(), "aerosol_pressure=unclipped;surface_albedo=772");
    ASSERT_EQ(HARP_SUCCESS, r_opt.status);
    EXPECT_TRUE(r_opt.variables["aerosol_pressure"].included);
    EXPECT_FLOAT_EQ(0.2f, r_opt.variables["surface_albedo"].float_values[0]);
    EXPECT_FLOAT_EQ(
        0.02f, r_opt.variables["surface_albedo_uncertainty"].float_values[0]);
  }
}

// ============================================================================
// 3. S5P_L2_CH4
// ============================================================================

void PopulateCh4(CodaMemProduct& prod, const S5pHandles& h, long n_obs,
                 long n_layers, long n_levels, int processor_version) {
  const long obs_dims[] = {1, n_obs};
  const long layer_dims[] = {1, n_obs, n_layers};
  const long level_dims[] = {1, n_obs, n_levels};
  std::vector<float> vals(n_obs, 1850.0f);
  std::vector<double> dvals(n_obs, 5000.0);
  std::vector<float> avk(n_obs * n_layers);
  std::vector<float> h_levels(n_obs * n_levels);
  for (long i = 0; i < n_obs; ++i) {
    for (long l = 0; l < n_layers; ++l) {
      avk[i * n_layers + l] = static_cast<float>(l + 1);
    }
    for (long l = 0; l < n_levels; ++l) {
      h_levels[i * n_levels + l] = static_cast<float>((n_levels - l) * 1000.0);
    }
  }
  EXPECT_TRUE(
      AddFloatArray(prod, h.product, "methane_mixing_ratio", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(
      prod, h.product, "methane_mixing_ratio_bias_corrected", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product,
                            "methane_mixing_ratio_bias_corrected_destriped",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product, "methane_mixing_ratio_precision",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "column_averaging_kernel",
                            layer_dims, avk));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "methane_profile_apriori",
                            layer_dims, avk));
  EXPECT_TRUE(
      AddFloatArray(prod, h.input_data, "dry_air_subcolumns", layer_dims, avk));
  EXPECT_TRUE(
      AddDoubleArray(prod, h.input_data, "pressure_interval", obs_dims, dvals));
  EXPECT_TRUE(AddFloatArray(
      prod, h.input_data,
      processor_version < 10000 ? "height_levels" : "altitude_levels",
      level_dims, h_levels));

  std::vector<float> swir(n_obs, 0.25f), nir(n_obs, 0.35f);
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "water_total_column",
                            obs_dims, swir));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "water_total_column_precision", obs_dims, swir));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data,
                            "cloud_fraction_VIIRS_SWIR_IFOV", obs_dims, swir));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "cloud_fraction_VIIRS_NIR_IFOV",
                            obs_dims, nir));
  EXPECT_TRUE(AddFloatArray(
      prod, h.detailed_results,
      processor_version < 10000 ? "aerosol_mid_height" : "aerosol_mid_altitude",
      obs_dims, swir));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "aerosol_optical_thickness_SWIR", obs_dims, swir));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "aerosol_optical_thickness_NIR", obs_dims, nir));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "surface_albedo_SWIR",
                            obs_dims, swir));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "surface_albedo_NIR",
                            obs_dims, nir));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "surface_albedo_SWIR_precision", obs_dims, swir));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "surface_albedo_NIR_precision", obs_dims, nir));
}

TEST(HarpIngestS5pL2Test, Ch4VersionsAndOptions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5P_L2_CH4");
  ASSERT_NE(nullptr, mod);

  // < 01.00.00 (no @id -> processor_version = -1) uses height_levels &
  // aerosol_mid_height, and ch4=corrected returns empty product.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5pConfig cfg{
        .product_type = "L2__CH4___",
        .processor_version = 900,
        .include_id = false,
        .has_layer_dim = true,
        .has_level_dim = true,
    };
    S5pHandles h = BuildS5pBase(prod, cfg);
    PopulateCh4(prod, h, 4, cfg.num_layers, cfg.num_levels, 900);
    FinalizeS5pHierarchy(prod, h);

    IngestExerciseResult r_empty =
        ExerciseIngestionModule(mod, prod.product(), "ch4=corrected");
    ASSERT_EQ(HARP_SUCCESS, r_empty.status);
    EXPECT_EQ(0, r_empty.dimensions[harp_dimension_time]);

    IngestExerciseResult r_bias =
        ExerciseIngestionModule(mod, prod.product(), "ch4=bias_corrected");
    ASSERT_EQ(HARP_SUCCESS, r_bias.status);
    EXPECT_EQ(4, r_bias.dimensions[harp_dimension_time]);
    EXPECT_EQ(2, r_bias.dimensions[harp_dimension_vertical]);
    // altitude_bounds inverted from {3000, 2000, 1000} -> {1000, 2000, 3000}.
    EXPECT_FLOAT_EQ(1000.0f,
                    r_bias.variables["altitude_bounds"].float_values[0]);
    EXPECT_FLOAT_EQ(2000.0f,
                    r_bias.variables["altitude_bounds"].float_values[1]);
    EXPECT_FLOAT_EQ(0.25f, r_bias.variables["surface_albedo"].float_values[0]);
  }

  // >= 02.07.00 with ch4=corrected;band=NIR.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5pConfig cfg{
        .product_type = "L2__CH4___",
        .processor_version = 20700,
        .has_layer_dim = true,
        .has_level_dim = true,
    };
    S5pHandles h = BuildS5pBase(prod, cfg);
    PopulateCh4(prod, h, 4, cfg.num_layers, cfg.num_levels, 20700);
    FinalizeS5pHierarchy(prod, h);

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "ch4=corrected;band=NIR");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_FLOAT_EQ(0.35f, res.variables["surface_albedo"].float_values[0]);
    EXPECT_FLOAT_EQ(0.35f, res.variables["cloud_fraction"].float_values[0]);
  }
}

// ============================================================================
// 4. S5P_L2_CO
// ============================================================================

void PopulateCo(CodaMemProduct& prod, const S5pHandles& h, long n_obs,
                long n_layers, bool nan_top_pressure) {
  const long obs_dims[] = {1, n_obs};
  const long layer_dims[] = {1, n_obs, n_layers};
  std::vector<float> vals(n_obs, 0.03f);
  std::vector<float> p_levels(n_obs * n_layers);
  std::vector<float> avk(n_obs * n_layers);
  for (long i = 0; i < n_obs; ++i) {
    for (long l = 0; l < n_layers; ++l) {
      p_levels[i * n_layers + l] = (l == 0 && nan_top_pressure)
                                       ? kFillFloat
                                       : static_cast<float>((l + 1) * 20000.0);
      avk[i * n_layers + l] = 1.0f;
    }
  }
  EXPECT_TRUE(AddFloatArray(prod, h.product, "carbonmonoxide_total_column",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product,
                            "carbonmonoxide_total_column_corrected", obs_dims,
                            vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product,
                            "carbonmonoxide_total_column_precision", obs_dims,
                            vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "pressure_levels",
                            layer_dims, p_levels));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "column_averaging_kernel",
                            layer_dims, avk));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data,
                            "carbonmonoxide_profile_apriori", layer_dims, avk));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "water_total_column",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "water_total_column_precision", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "height_scattering_layer",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "scattering_optical_thickness_SWIR", obs_dims,
                            vals));
}

TEST(HarpIngestS5pL2Test, CoVersionsAndOptions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5P_L2_CO");
  ASSERT_NE(nullptr, mod);

  // < 02.01.00: co=corrected returns empty product; default avk divides by 1e3.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5pConfig cfg{
        .product_type = "L2__CO____",
        .processor_version = 20000,
        .has_layer_dim = true,
    };
    S5pHandles h = BuildS5pBase(prod, cfg);
    PopulateCo(prod, h, 4, cfg.num_layers, /*nan_top_pressure=*/true);
    FinalizeS5pHierarchy(prod, h);

    IngestExerciseResult r_empty =
        ExerciseIngestionModule(mod, prod.product(), "co=corrected");
    ASSERT_EQ(HARP_SUCCESS, r_empty.status);
    EXPECT_EQ(0, r_empty.dimensions[harp_dimension_time]);

    IngestExerciseResult r_def = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, r_def.status);
    EXPECT_FLOAT_EQ(
        0.001f,
        r_def.variables["CO_column_number_density_avk"].float_values[0]);
    EXPECT_FALSE(r_def.variables["CO_column_number_density_apriori"].included);
    // Top pressure was fill -> NaN upper bound.
    EXPECT_TRUE(std::isnan(r_def.variables["pressure_bounds"].float_values[3]));
  }

  // >= 02.04.00 with co=corrected;co_avk=number_density.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5pConfig cfg{
        .product_type = "L2__CO____",
        .processor_version = 20400,
        .has_layer_dim = true,
    };
    S5pHandles h = BuildS5pBase(prod, cfg);
    PopulateCo(prod, h, 4, cfg.num_layers, /*nan_top_pressure=*/false);
    FinalizeS5pHierarchy(prod, h);

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "co=corrected;co_avk=number_density");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_TRUE(res.variables["CO_number_density_avk"].included);
    EXPECT_FALSE(res.variables["CO_column_number_density_avk"].included);
    EXPECT_FLOAT_EQ(1000.0f,
                    res.variables["CO_number_density_avk"].float_values[0]);
    EXPECT_TRUE(res.variables["CO_column_number_density_apriori"].included);
    EXPECT_FLOAT_EQ(1e-3f, res.variables["pressure_bounds"].float_values[3]);
  }
}

// ============================================================================
// 5. S5P_L2_HCHO
// ============================================================================

void PopulateHcho(CodaMemProduct& prod, const S5pHandles& h, long n_obs,
                  long n_layers) {
  const long obs_dims[] = {1, n_obs};
  const long layer_dims[] = {1, n_obs, n_layers};
  const long coef_dims[] = {n_layers};
  std::vector<float> vals(n_obs, 0.001f);
  std::vector<float> amf(n_obs, 2.0f), amf_clr(n_obs, 4.0f);
  std::vector<float> avk(n_obs * n_layers, 1.0f);
  std::vector<double> tm5_a(n_layers, 100.0), tm5_b(n_layers, 0.5);
  std::vector<int32_t> trop_idx(n_obs, 1);
  if (n_obs > 1) {
    trop_idx[1] = -1;  // Out-of-range triggers NaN branch.
  }
  EXPECT_TRUE(AddFloatArray(prod, h.product,
                            "formaldehyde_tropospheric_vertical_column",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(
      prod, h.product, "formaldehyde_tropospheric_vertical_column_precision",
      obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(
      prod, h.detailed_results,
      "formaldehyde_tropospheric_vertical_column_trueness", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "averaging_kernel",
                            layer_dims, avk));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "formaldehyde_profile_apriori", layer_dims, avk));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "formaldehyde_tropospheric_air_mass_factor",
                            obs_dims, amf));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "formaldehyde_clear_air_mass_factor", obs_dims,
                            amf_clr));
  EXPECT_TRUE(AddFloatArray(
      prod, h.detailed_results,
      "formaldehyde_tropospheric_air_mass_factor_precision", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(
      prod, h.detailed_results,
      "formaldehyde_tropospheric_air_mass_factor_trueness", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "formaldehyde_slant_column_corrected", obs_dims,
                            vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "formaldehyde_slant_column_corrected_trueness",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "aerosol_index_340_380",
                            obs_dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, h.input_data, "surface_albedo", obs_dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, h.input_data, "effective_albedo", obs_dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, h.input_data, "scene_pressure", obs_dims, vals));
  EXPECT_TRUE(
      AddDoubleArray(prod, h.input_data, "tm5_constant_a", coef_dims, tm5_a));
  EXPECT_TRUE(
      AddDoubleArray(prod, h.input_data, "tm5_constant_b", coef_dims, tm5_b));
  EXPECT_TRUE(prod.AddField(h.input_data, "tm5_tropopause_layer_index",
                            prod.MakeInt32Array(obs_dims, trop_idx)));
  PopulateCloudInputVars(prod, h, n_obs);
}

TEST(HarpIngestS5pL2Test, HchoVersionsAndOptions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5P_L2_HCHO");
  ASSERT_NE(nullptr, mod);

  // < 02.00.00 unmasked avk.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5pConfig cfg{
        .product_type = "L2__HCHO__",
        .processor_version = 10300,
        .has_layer_dim = true,
        .delta_time_3d = true,
    };
    S5pHandles h = BuildS5pBase(prod, cfg);
    PopulateHcho(prod, h, 4, cfg.num_layers);
    FinalizeS5pHierarchy(prod, h);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_FLOAT_EQ(1.0f,
                    res.variables["tropospheric_HCHO_column_number_density_avk"]
                        .float_values[3]);
    EXPECT_FALSE(res.variables["tropopause_pressure"].included);
  }

  // >= 02.08.00 OFFL with default options and with
  // amf=clear_sky;cloud_fraction=radiance.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5pConfig cfg{
        .product_type = "L2__HCHO__",
        .processor_version = 20800,
        .has_layer_dim = true,
        .delta_time_3d = true,
    };
    S5pHandles h = BuildS5pBase(prod, cfg);
    PopulateHcho(prod, h, 4, cfg.num_layers);
    FinalizeS5pHierarchy(prod, h);

    IngestExerciseResult r_def = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, r_def.status);
    EXPECT_TRUE(r_def.variables["scene_albedo"].included);
    EXPECT_TRUE(r_def.variables["scene_pressure"].included);
    EXPECT_TRUE(r_def.variables["tropopause_pressure"].included);
    EXPECT_TRUE(
        std::isnan(r_def.variables["tropopause_pressure"].double_values[1]));
    EXPECT_TRUE(std::isnan(
        r_def.variables["tropospheric_HCHO_column_number_density_avk"]
            .float_values[2]));

    IngestExerciseResult r_opt = ExerciseIngestionModule(
        mod, prod.product(), "amf=clear_sky;cloud_fraction=radiance");
    ASSERT_EQ(HARP_SUCCESS, r_opt.status);
    EXPECT_FALSE(r_opt.variables["tropospheric_HCHO_column_number_density_avk"]
                     .included);
    // 0.001 * 2.0 / 4.0 = 0.0005
    EXPECT_FLOAT_EQ(0.0005f,
                    r_opt.variables["tropospheric_HCHO_column_number_density"]
                        .float_values[0]);
    EXPECT_FLOAT_EQ(
        4.0f, r_opt.variables["tropospheric_HCHO_column_number_density_amf"]
                  .float_values[0]);
  }
}

// ============================================================================
// 6. S5P_L2_O3
// ============================================================================

void PopulateO3(CodaMemProduct& prod, const S5pHandles& h, long n_obs,
                long n_layers, long n_levels, bool trigger_surface_removal,
                int processor_version) {
  const long obs_dims[] = {1, n_obs};
  const long layer_dims[] = {1, n_obs, n_layers};
  const long level_dims[] = {1, n_obs, n_levels};
  std::vector<float> col(n_obs, 0.3f), prec(n_obs, 0.01f), temp(n_obs, 220.0f),
      ring(n_obs, 0.05f), eff_alb(n_obs, 0.3f), rms(n_obs, 0.005f);
  if (n_obs >= 4) {
    // Make pixel 1 fail col, pixel 2 fail temp, pixel 3 fail ring/rms.
    col[1] = 0.6f;
    temp[2] = 300.0f;
    ring[3] = 0.5f;
    rms[3] = 0.05f;
  }
  std::vector<float> p_grid(n_obs * n_levels);
  std::vector<float> prof(n_obs * n_layers, 0.1f);
  for (long i = 0; i < n_obs; ++i) {
    for (long l = 0; l < n_levels; ++l) {
      p_grid[i * n_levels + l] = static_cast<float>(100000 - l * 20000);
    }
    if (trigger_surface_removal && i == 0) {
      if (processor_version < 10104) {
        p_grid[i * n_levels + n_levels - 1] = kFillFloat;
      } else {
        p_grid[i * n_levels + 1] = p_grid[i * n_levels];
      }
    }
  }
  EXPECT_TRUE(AddFloatArray(prod, h.product, "ozone_total_vertical_column",
                            obs_dims, col));
  EXPECT_TRUE(AddFloatArray(prod, h.product,
                            "ozone_total_vertical_column_precision", obs_dims,
                            prec));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "pressure_grid",
                            level_dims, p_grid));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "ozone_profile_apriori",
                            layer_dims, prof));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "averaging_kernel",
                            layer_dims, prof));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "ozone_total_air_mass_factor", obs_dims, prec));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "ozone_total_air_mass_factor_trueness", obs_dims,
                            prec));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "degrees_of_freedom",
                            obs_dims, prec));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "shannon_information_content", obs_dims, prec));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "ozone_slant_column_ring_corrected", obs_dims,
                            prec));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "ozone_effective_temperature", obs_dims, temp));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "ring_scale_factor",
                            obs_dims, ring));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "effective_albedo",
                            obs_dims, eff_alb));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "scene_pressure",
                            obs_dims, prec));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "fitted_root_mean_square",
                            obs_dims, rms));
  EXPECT_TRUE(
      AddFloatArray(prod, h.input_data, "surface_albedo", obs_dims, eff_alb));
  // Cloud variables (both NRTI and OFFL sets).
  EXPECT_TRUE(
      AddFloatArray(prod, h.input_data, "cloud_fraction", obs_dims, prec));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "cloud_fraction_precision",
                            obs_dims, prec));
  EXPECT_TRUE(
      AddFloatArray(prod, h.input_data, "cloud_base_height", obs_dims, prec));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "cloud_base_height_precision",
                            obs_dims, prec));
  EXPECT_TRUE(
      AddFloatArray(prod, h.input_data, "cloud_base_pressure", obs_dims, prec));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "cloud_base_pressure_precision",
                            obs_dims, prec));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "cloud_optical_thickness",
                            obs_dims, prec));
  EXPECT_TRUE(AddFloatArray(
      prod, h.input_data, "cloud_optical_thickness_precision", obs_dims, prec));
  EXPECT_TRUE(
      AddFloatArray(prod, h.input_data, "cloud_top_height", obs_dims, prec));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "cloud_top_height_precision",
                            obs_dims, prec));
  EXPECT_TRUE(
      AddFloatArray(prod, h.input_data, "cloud_top_pressure", obs_dims, prec));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "cloud_top_pressure_precision",
                            obs_dims, prec));
  PopulateCloudInputVars(prod, h, n_obs);
}

TEST(HarpIngestS5pL2Test, O3VersionsModesAndCustomQaFilter) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5P_L2_O3");
  ASSERT_NE(nullptr, mod);

  // < 01.01.04 OFFL with NaN top pressure.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5pConfig cfg{
        .product_type = "L2__O3____",
        .processor_version = 10100,
        .has_layer_dim = true,
        .has_level_dim = true,
        .delta_time_3d = true,
    };
    S5pHandles h = BuildS5pBase(prod, cfg);
    PopulateO3(prod, h, 4, cfg.num_layers, cfg.num_levels, true, 10100);
    FinalizeS5pHierarchy(prod, h);

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "qa_filter=custom");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["O3_column_number_density_validity"].int8_values,
                ElementsAre(100, 0, 0, 0));
    EXPECT_TRUE(std::isnan(res.variables["pressure_bounds"].float_values[2]));
  }

  // >= 01.01.04 NRTI with qa_filter=custom & surface layer removal.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5pConfig cfg{
        .product_type = "L2__O3____",
        .processor_version = 10200,
        .processing_mode = "Near-realtime",
        .has_layer_dim = true,
        .has_level_dim = true,
        .delta_time_3d = true,
    };
    S5pHandles h = BuildS5pBase(prod, cfg);
    PopulateO3(prod, h, 4, cfg.num_layers, cfg.num_levels, true, 10200);
    FinalizeS5pHierarchy(prod, h);

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "qa_filter=custom");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_TRUE(res.variables["O3_column_number_density_amf"].included);
    EXPECT_FALSE(res.variables["O3_column_number_density_dfs"].included);
    EXPECT_THAT(res.variables["O3_column_number_density_validity"].int8_values,
                ElementsAre(100, 0, 0, 0));
    EXPECT_TRUE(std::isnan(res.variables["pressure_bounds"].float_values[2]));
    EXPECT_TRUE(std::isnan(
        res.variables["O3_column_number_density_apriori"].float_values[1]));
    EXPECT_TRUE(std::isnan(
        res.variables["O3_column_number_density_avk"].float_values[1]));
  }

  // >= 02.00.00 with qa_filter=custom leaves raw qa_value unchanged.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5pConfig cfg{
        .product_type = "L2__O3____",
        .processor_version = 20000,
        .has_layer_dim = true,
        .has_level_dim = true,
        .delta_time_3d = true,
    };
    S5pHandles h = BuildS5pBase(prod, cfg);
    PopulateO3(prod, h, 4, cfg.num_layers, cfg.num_levels, false, 20000);
    FinalizeS5pHierarchy(prod, h);

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "qa_filter=custom");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(
        90, res.variables["O3_column_number_density_validity"].int8_values[0]);
  }
}

// ============================================================================
// 7. S5P_L2_O3_PR
// ============================================================================

void PopulateO3Pr(CodaMemProduct& prod, const S5pHandles& h, long n_obs,
                  long n_levels, int processor_version) {
  const long obs_dims[] = {1, n_obs};
  const long level_dims[] = {1, n_obs, n_levels};
  const long cov_dims[] = {1, n_obs, n_levels, n_levels};
  const long spec_dims[] = {2};
  const long spec_obs_dims[] = {1, n_obs, 2};
  std::vector<float> vals(n_obs, 300.0f);
  std::vector<float> prof(n_obs * n_levels, 1e-4f);
  std::vector<float> cov(n_obs * n_levels * n_levels, 1e-8f);
  std::vector<float> alt(n_obs * n_levels);
  for (long i = 0; i < n_obs; ++i) {
    for (long l = 0; l < n_levels; ++l) {
      alt[i * n_levels + l] = static_cast<float>(l * 5000.0f);
    }
  }
  constexpr float kWave[] = {310.0f, 330.0f};
  std::vector<float> alb(n_obs * 2, 0.25f);

  EXPECT_TRUE(AddFloatArray(prod, h.product, "dimension_surface_albedo",
                            spec_dims, kWave));
  EXPECT_TRUE(AddFloatArray(prod, h.product, "dimension_cloud_albedo",
                            spec_dims, kWave));

  CodaMemNode* loc_node = processor_version < 20100 ? h.input_data : h.product;
  EXPECT_TRUE(AddFloatArray(prod, loc_node, "pressure", level_dims, alt));
  EXPECT_TRUE(AddFloatArray(prod, loc_node, "altitude", level_dims, alt));

  CodaMemNode* cf_loc =
      processor_version < 20100 ? h.detailed_results : h.input_data;
  EXPECT_TRUE(
      AddFloatArray(prod, cf_loc, "cloud_fraction_crb", obs_dims, vals));

  EXPECT_TRUE(
      AddFloatArray(prod, h.product, "ozone_profile", level_dims, prof));
  EXPECT_TRUE(AddFloatArray(prod, h.product, "ozone_profile_precision",
                            level_dims, prof));
  EXPECT_TRUE(
      AddFloatArray(prod, h.product, "ozone_total_column", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product, "ozone_total_column_precision",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product, "ozone_tropospheric_column",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(
      prod, h.product, "ozone_tropospheric_column_precision", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "averaging_kernel",
                            cov_dims, cov));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "ozone_profile_error_covariance_matrix", cov_dims,
                            cov));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "cloud_albedo_crb",
                            spec_obs_dims, alb));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "surface_albedo",
                            spec_obs_dims, alb));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "ozone_profile_apriori",
                            level_dims, prof));

  CodaMemNode* ap_prec = prod.MakeFloatArray(level_dims, prof);
  ASSERT_NE(nullptr, ap_prec);
  constexpr long kOne[] = {1};
  constexpr float kFillArr[] = {kFillFloat};
  constexpr double kCorr[] = {6000.0};
  EXPECT_TRUE(prod.AddAttribute(ap_prec, "FillValue",
                                prod.MakeFloatArray(kOne, kFillArr)));
  EXPECT_TRUE(prod.AddAttribute(ap_prec, "correlation_length",
                                prod.MakeDoubleArray(kOne, kCorr)));
  EXPECT_TRUE(
      prod.AddField(h.input_data, "ozone_profile_apriori_precision", ap_prec));

  EXPECT_TRUE(
      AddFloatArray(prod, h.input_data, "cloud_pressure_crb", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "pressure_at_tropopause",
                            obs_dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, h.input_data, "temperature", level_dims, alt));
}

TEST(HarpIngestS5pL2Test, O3PrVersions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5P_L2_O3_PR");
  ASSERT_NE(nullptr, mod);

  for (int ver : {20000, 20100}) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5pConfig cfg{
        .product_type = "L2__O3__PR",
        .processor_version = ver,
        .has_level_dim = true,
    };
    S5pHandles h = BuildS5pBase(prod, cfg);
    PopulateO3Pr(prod, h, 4, cfg.num_levels, ver);
    FinalizeS5pHierarchy(prod, h);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(4, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(3, res.dimensions[harp_dimension_vertical]);
    EXPECT_EQ(2, res.dimensions[harp_dimension_spectral]);
    EXPECT_THAT(res.variables["wavelength"].float_values,
                ElementsAre(310.0f, 330.0f));
  }
}

// ============================================================================
// 8. S5P_L2_O3_TCL
// ============================================================================

void PopulateO3Tcl(CodaMemProduct& prod, int processor_version, bool is_csa) {
  ASSERT_TRUE(prod.SetProductInfo("Sentinel5P", "L2__O3_TCL", 1));
  ASSERT_TRUE(prod.AddAttribute(
      prod.root(), "id",
      prod.MakeString(MakeProductId("L2__O3_TCL", processor_version))));
  ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_start",
                                prod.MakeString("2020-01-01T00:00:00")));
  ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_end",
                                prod.MakeString("2020-01-31T23:59:59")));

  CodaMemNode* product_rec = prod.MakeRecord();
  CodaMemNode* sup = prod.MakeRecord();
  CodaMemNode* det = prod.MakeRecord();

  constexpr long kTimeDims[] = {1};
  constexpr double kTimeVals[] = {0.0};
  ASSERT_TRUE(AddDoubleArray(prod, product_rec, "time", kTimeDims, kTimeVals));

  constexpr long kLatDims[] = {2};
  constexpr long kLonDims[] = {3};
  constexpr float kLat[] = {-10.0f, 10.0f};
  constexpr float kLon[] = {-60.0f, 0.0f, 60.0f};
  const char* lat_name =
      is_csa ? (processor_version < 10100 ? "lat" : "latitude_csa")
             : (processor_version < 10100 ? "latitude" : "latitude_ccd");
  const char* lon_name =
      is_csa ? (processor_version < 10100 ? "lon" : "longitude_csa")
             : (processor_version < 10100 ? "longitude" : "longitude_ccd");
  ASSERT_TRUE(AddFloatArray(prod, product_rec, lat_name, kLatDims, kLat));
  ASSERT_TRUE(AddFloatArray(prod, product_rec, lon_name, kLonDims, kLon));

  constexpr long kGridDims[] = {1, 2, 3};
  constexpr float kGridVals[6] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
  constexpr int32_t kQaVals[6] = {90, 90, 90, 90, 90, 90};
  constexpr float kStratRef[2] = {0.12f, 0.15f};
  constexpr long kStratRefDims[] = {1, 2};

  ASSERT_TRUE(AddFloatArray(prod, product_rec,
                            is_csa ? "ozone_upper_tropospheric_mixing_ratio"
                                   : "ozone_tropospheric_mixing_ratio",
                            kGridDims, kGridVals));
  ASSERT_TRUE(
      AddFloatArray(prod, product_rec,
                    is_csa ? "ozone_upper_tropospheric_mixing_ratio_precision"
                           : "ozone_tropospheric_mixing_ratio_precision",
                    kGridDims, kGridVals));

  if (is_csa && processor_version < 30000) {
    ASSERT_TRUE(prod.AddField(product_rec,
                              "ozone_upper_tropospheric_mixing_ratio_flag",
                              prod.MakeInt32Array(kGridDims, kQaVals)));
  } else if (processor_version >= 30000) {
    ASSERT_TRUE(prod.AddField(product_rec,
                              is_csa ? "qa_value_csa" : "qa_value_ccd",
                              prod.MakeInt32Array(kGridDims, kQaVals)));
  } else {
    ASSERT_TRUE(prod.AddField(product_rec, "qa_value",
                              prod.MakeInt32Array(kGridDims, kQaVals)));
  }

  ASSERT_TRUE(AddFloatArray(prod, product_rec,
                            "ozone_tropospheric_vertical_column", kGridDims,
                            kGridVals));
  ASSERT_TRUE(AddFloatArray(prod, product_rec,
                            "ozone_tropospheric_vertical_column_precision",
                            kGridDims, kGridVals));
  ASSERT_TRUE(AddFloatArray(prod, det, "ozone_stratospheric_vertical_column",
                            kGridDims, kGridVals));
  ASSERT_TRUE(AddFloatArray(prod, det,
                            "ozone_stratospheric_vertical_column_precision",
                            kGridDims, kGridVals));
  ASSERT_TRUE(AddFloatArray(prod, det,
                            "ozone_stratospheric_vertical_column_reference",
                            kStratRefDims, kStratRef));
  ASSERT_TRUE(AddFloatArray(
      prod, det, "ozone_stratospheric_vertical_column_reference_precision",
      kStratRefDims, kStratRef));
  ASSERT_TRUE(AddFloatArray(prod, det, "ozone_total_vertical_column", kGridDims,
                            kGridVals));
  ASSERT_TRUE(AddFloatArray(prod, det, "ozone_total_vertical_column_precision",
                            kGridDims, kGridVals));
  ASSERT_TRUE(AddFloatArray(prod, det, "surface_albedo", kGridDims, kGridVals));
  ASSERT_TRUE(
      AddFloatArray(prod, det, "surface_altitude", kGridDims, kGridVals));
  ASSERT_TRUE(
      AddFloatArray(prod, det, "surface_pressure", kGridDims, kGridVals));
  ASSERT_TRUE(AddFloatArray(
      prod, det, "number_of_observations_ozone_upper_tropospheric_mixing_ratio",
      kGridDims, kGridVals));
  ASSERT_TRUE(
      AddFloatArray(prod, det, "cloud_top_pressure_max", kGridDims, kGridVals));
  ASSERT_TRUE(
      AddFloatArray(prod, det, "cloud_top_pressure_min", kGridDims, kGridVals));

  ASSERT_TRUE(prod.AddField(sup, "DETAILED_RESULTS", det));
  ASSERT_TRUE(prod.AddField(product_rec, "SUPPORT_DATA", sup));
  ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", product_rec));
}

TEST(HarpIngestS5pL2Test, O3TclCcdAndCsaVersions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5P_L2_O3_TCL");
  ASSERT_NE(nullptr, mod);

  // CCD < 01.00.00: qa_value & surface_pressure excluded.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateO3Tcl(prod, 900, /*is_csa=*/false);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(1, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(2, res.dimensions[harp_dimension_latitude]);
    EXPECT_EQ(3, res.dimensions[harp_dimension_longitude]);
    EXPECT_FALSE(
        res.variables
            ["tropospheric_O3_column_volume_mixing_ratio_dry_air_validity"]
                .included);
    EXPECT_FALSE(res.variables["surface_pressure"].included);
  }

  // CCD 02.00.00 with o3_strat=reference.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateO3Tcl(prod, 20000, /*is_csa=*/false);
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "o3_strat=reference");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_TRUE(res.variables["surface_pressure"].included);
    EXPECT_THAT(
        res.variables["stratospheric_O3_column_number_density"].float_values,
        ElementsAre(0.12f, 0.12f, 0.12f, 0.15f, 0.15f, 0.15f));
    EXPECT_THAT(
        res.variables["stratospheric_O3_column_number_density_uncertainty"]
            .float_values,
        ElementsAre(0.12f, 0.12f, 0.12f, 0.15f, 0.15f, 0.15f));
  }

  // CCD 03.00.00 uses qa_value_ccd.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateO3Tcl(prod, 30000, /*is_csa=*/false);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(
        90, res.variables
                ["tropospheric_O3_column_volume_mixing_ratio_dry_air_validity"]
                    .int32_values[0]);
  }

  // CSA < 01.01.00, 01.01.00, and >= 03.00.00.
  for (int ver : {10000, 10100, 30000}) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateO3Tcl(prod, ver, /*is_csa=*/true);
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "o3=csa");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_TRUE(res.variables["pressure_bounds"].included);
    EXPECT_TRUE(res.variables
                    ["tropospheric_O3_column_volume_mixing_ratio_dry_air_count"]
                        .included);
  }
}

// ============================================================================
// 9. S5P_L2_NO2
// ============================================================================

void PopulateNo2(CodaMemProduct& prod, const S5pHandles& h, long n_obs,
                 long n_layers) {
  const long obs_dims[] = {1, n_obs};
  const long layer_dims[] = {1, n_obs, n_layers};
  const long coef_dims[] = {n_layers, 2};
  std::vector<float> vals(n_obs, 0.05f), amf_tot(n_obs, 2.0f),
      amf_trop(n_obs, 1.0f), amf_strat(n_obs, 3.0f);
  std::vector<float> avk(n_obs * n_layers, 1.0f);
  std::vector<double> tm5_a(n_layers * 2, 0.0), tm5_b(n_layers * 2, 0.0);
  // Make top boundary < 1e-3 to trigger clamping to 1e-3.
  tm5_a[0] = 100.0;
  tm5_b[0] = 0.9;
  tm5_a[1] = 50.0;
  tm5_b[1] = 0.5;
  tm5_a[2] = 50.0;
  tm5_b[2] = 0.5;
  tm5_a[3] = 0.0;
  tm5_b[3] = 0.0;
  std::vector<int32_t> trop_idx(n_obs, 1);
  if (n_obs > 1) {
    trop_idx[1] = 99;  // Out-of-range -> NaN branch.
  }

  EXPECT_TRUE(AddFloatArray(
      prod, h.product, "nitrogendioxide_tropospheric_column", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product,
                            "nitrogendioxide_tropospheric_column_precision",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product, "air_mass_factor_troposphere",
                            obs_dims, amf_trop));
  EXPECT_TRUE(AddFloatArray(prod, h.product, "air_mass_factor_total", obs_dims,
                            amf_tot));
  EXPECT_TRUE(
      AddFloatArray(prod, h.product, "averaging_kernel", layer_dims, avk));
  EXPECT_TRUE(
      AddDoubleArray(prod, h.product, "tm5_constant_a", coef_dims, tm5_a));
  EXPECT_TRUE(
      AddDoubleArray(prod, h.product, "tm5_constant_b", coef_dims, tm5_b));
  EXPECT_TRUE(prod.AddField(h.product, "tm5_tropopause_layer_index",
                            prod.MakeInt32Array(obs_dims, trop_idx)));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "nitrogendioxide_summed_total_column", obs_dims,
                            vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "nitrogendioxide_total_column", obs_dims, amf_tot));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "nitrogendioxide_summed_total_column_precision",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "nitrogendioxide_total_column_precision", obs_dims,
                            amf_tot));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "nitrogendioxide_stratospheric_column", obs_dims,
                            vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "nitrogendioxide_stratospheric_column_precision",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "air_mass_factor_stratosphere", obs_dims,
                            amf_strat));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "nitrogendioxide_slant_column_density", obs_dims,
                            vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "nitrogendioxide_slant_column_density_precision",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "cloud_fraction_crb_nitrogendioxide_window",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "cloud_radiance_fraction_nitrogendioxide_window",
                            obs_dims, amf_tot));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "aerosol_index_354_388",
                            obs_dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, h.input_data, "cloud_albedo_crb", obs_dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, h.input_data, "cloud_pressure_crb", obs_dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, h.input_data, "scene_albedo", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "apparent_scene_pressure",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data,
                            "surface_albedo_nitrogendioxide_window", obs_dims,
                            vals));

  // O22CLD sub-group.
  CodaMemNode* o22 = prod.MakeRecord();
  EXPECT_TRUE(
      AddFloatArray(prod, o22, "o22cld_cloud_fraction_crb", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, o22, "o22cld_cloud_fraction_crb_precision",
                            obs_dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, o22, "o22cld_cloud_pressure_crb", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, o22, "o22cld_cloud_pressure_crb_precision",
                            obs_dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, o22, "o22cld_cloud_height_crb", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, o22, "o22cld_cloud_height_crb_precision",
                            obs_dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, o22, "o22cld_cloud_albedo_crb", obs_dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, o22, "o22cld_surface_albedo", obs_dims, vals));
  EXPECT_TRUE(prod.AddField(h.detailed_results, "O22CLD", o22));
}

TEST(HarpIngestS5pL2Test, No2AndO22CldOptions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5P_L2_NO2");
  ASSERT_NE(nullptr, mod);

  // < 02.02.00 with data=o22cld returns empty product; default NO2 succeeds.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5pConfig cfg{
        .product_type = "L2__NO2___",
        .processor_version = 20100,
        .has_layer_dim = true,
    };
    S5pHandles h = BuildS5pBase(prod, cfg);
    PopulateNo2(prod, h, 4, cfg.num_layers);
    FinalizeS5pHierarchy(prod, h);

    IngestExerciseResult r_empty =
        ExerciseIngestionModule(mod, prod.product(), "data=o22cld");
    ASSERT_EQ(HARP_SUCCESS, r_empty.status);
    EXPECT_EQ(0, r_empty.dimensions[harp_dimension_time]);

    IngestExerciseResult r_summed =
        ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, r_summed.status);
    EXPECT_FLOAT_EQ(
        0.05f, r_summed.variables["NO2_column_number_density"].float_values[0]);
    EXPECT_DOUBLE_EQ(1e-3,
                     r_summed.variables["pressure_bounds"].double_values[3]);
    EXPECT_TRUE(
        std::isnan(r_summed.variables["tropopause_pressure"].double_values[1]));
  }

  // >= 02.02.00 with total_column=total;cloud_fraction=radiance and
  // data=o22cld.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5pConfig cfg{
        .product_type = "L2__NO2___",
        .processor_version = 20400,
        .has_layer_dim = true,
    };
    S5pHandles h = BuildS5pBase(prod, cfg);
    PopulateNo2(prod, h, 4, cfg.num_layers);
    FinalizeS5pHierarchy(prod, h);

    IngestExerciseResult r_tot = ExerciseIngestionModule(
        mod, prod.product(), "total_column=total;cloud_fraction=radiance");
    ASSERT_EQ(HARP_SUCCESS, r_tot.status);
    EXPECT_FLOAT_EQ(
        2.0f, r_tot.variables["NO2_column_number_density"].float_values[0]);
    EXPECT_FLOAT_EQ(2.0f, r_tot.variables["cloud_fraction"].float_values[0]);

    IngestExerciseResult r_o22 =
        ExerciseIngestionModule(mod, prod.product(), "data=o22cld");
    ASSERT_EQ(HARP_SUCCESS, r_o22.status);
    EXPECT_EQ(4, r_o22.dimensions[harp_dimension_time]);
    EXPECT_FLOAT_EQ(0.05f, r_o22.variables["cloud_fraction"].float_values[0]);
  }
}

// ============================================================================
// 10. S5P_L2_SO2
// ============================================================================

void PopulateSo2(CodaMemProduct& prod, S5pHandles& h, long n_obs, long n_layers,
                 int processor_version) {
  const long obs_dims[] = {1, n_obs};
  const long layer_dims[] = {1, n_obs, n_layers};
  const long coef_dims[] = {n_layers};
  std::vector<float> vals(n_obs, 0.02f), alb328(n_obs, 0.1f),
      alb376(n_obs, 0.2f);
  std::vector<float> avk(n_obs * n_layers, 1.0f);
  std::vector<double> tm5_a(n_layers, 100.0), tm5_b(n_layers, 0.5);
  std::vector<int32_t> trop_idx(n_obs, 0);
  std::vector<int32_t> det_flag(n_obs, 1);
  std::vector<int32_t> win_flag(n_obs, 1);
  if (n_obs >= 4) {
    win_flag[0] = 1;
    win_flag[1] = 2;
    win_flag[2] = 3;
    win_flag[3] = 4;  // -> NaN
  }

  EXPECT_TRUE(AddFloatArray(
      prod, h.product, "sulfurdioxide_total_vertical_column", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product,
                            "sulfurdioxide_total_vertical_column_precision",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "sulfurdioxide_total_vertical_column_trueness",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "sulfurdioxide_total_air_mass_factor_polluted",
                            obs_dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, h.detailed_results,
                    "sulfurdioxide_total_air_mass_factor_polluted_precision",
                    obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(
      prod, h.detailed_results,
      "sulfurdioxide_total_air_mass_factor_polluted_trueness", obs_dims, vals));

  for (const char* suffix : {"1km", "3km", "7km", "15km"}) {
    EXPECT_TRUE(AddFloatArray(
        prod, h.detailed_results,
        std::string("sulfurdioxide_total_vertical_column_") + suffix, obs_dims,
        vals));
    EXPECT_TRUE(
        AddFloatArray(prod, h.detailed_results,
                      std::string("sulfurdioxide_total_vertical_column_") +
                          suffix + "_precision",
                      obs_dims, vals));
    EXPECT_TRUE(
        AddFloatArray(prod, h.detailed_results,
                      std::string("sulfurdioxide_total_vertical_column_") +
                          suffix + "_trueness",
                      obs_dims, vals));
    EXPECT_TRUE(AddFloatArray(
        prod, h.detailed_results,
        std::string("sulfurdioxide_total_air_mass_factor_") + suffix, obs_dims,
        vals));
    EXPECT_TRUE(
        AddFloatArray(prod, h.detailed_results,
                      std::string("sulfurdioxide_total_air_mass_factor_") +
                          suffix + "_precision",
                      obs_dims, vals));
    EXPECT_TRUE(
        AddFloatArray(prod, h.detailed_results,
                      std::string("sulfurdioxide_total_air_mass_factor_") +
                          suffix + "_trueness",
                      obs_dims, vals));
    EXPECT_TRUE(AddFloatArray(
        prod, h.detailed_results,
        std::string("sulfurdioxide_averaging_kernel_scaling_box_") + suffix,
        obs_dims, vals));
  }

  if (processor_version >= 20500) {
    h.so2_lh = prod.MakeRecord();
    EXPECT_TRUE(AddFloatArray(
        prod, h.so2_lh, "sulfurdioxide_total_vertical_column_layer_height",
        obs_dims, vals));
    EXPECT_TRUE(AddFloatArray(
        prod, h.so2_lh,
        "sulfurdioxide_total_vertical_column_layer_height_precision", obs_dims,
        vals));
    EXPECT_TRUE(AddFloatArray(
        prod, h.so2_lh,
        "sulfurdioxide_total_vertical_column_layer_height_trueness", obs_dims,
        vals));
    EXPECT_TRUE(AddFloatArray(
        prod, h.so2_lh, "sulfurdioxide_total_air_mass_factor_layer_height",
        obs_dims, vals));
    EXPECT_TRUE(AddFloatArray(
        prod, h.so2_lh,
        "sulfurdioxide_total_air_mass_factor_layer_height_precision", obs_dims,
        vals));
    EXPECT_TRUE(AddFloatArray(
        prod, h.so2_lh,
        "sulfurdioxide_total_air_mass_factor_layer_height_trueness", obs_dims,
        vals));
    EXPECT_TRUE(
        AddFloatArray(prod, h.so2_lh,
                      "sulfurdioxide_averaging_kernel_scaling_box_layer_height",
                      obs_dims, vals));
    EXPECT_TRUE(AddFloatArray(prod, h.so2_lh, "sulfurdioxide_layer_height",
                              obs_dims, vals));
    EXPECT_TRUE(AddFloatArray(prod, h.so2_lh,
                              "sulfurdioxide_layer_height_precision", obs_dims,
                              vals));
    EXPECT_TRUE(AddFloatArray(prod, h.so2_lh, "sulfurdioxide_layer_pressure",
                              obs_dims, vals));
    std::vector<uint8_t> lh_qa(n_obs, 95);
    EXPECT_TRUE(prod.AddField(h.so2_lh, "qa_value_layer_height",
                              prod.MakeUint8Array(obs_dims, lh_qa)));
  }

  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "averaging_kernel",
                            layer_dims, avk));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "sulfurdioxide_profile_apriori", layer_dims, avk));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "sulfurdioxide_slant_column_corrected", obs_dims,
                            vals));
  EXPECT_TRUE(prod.AddField(h.detailed_results, "sulfurdioxide_detection_flag",
                            prod.MakeInt32Array(obs_dims, det_flag)));
  EXPECT_TRUE(prod.AddField(h.detailed_results, "selected_fitting_window_flag",
                            prod.MakeInt32Array(obs_dims, win_flag)));

  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "ozone_total_vertical_column",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data,
                            "ozone_total_vertical_column_precision", obs_dims,
                            vals));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "aerosol_index_340_380",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "surface_albedo_328nm",
                            obs_dims, alb328));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "surface_albedo_376nm",
                            obs_dims, alb376));
  EXPECT_TRUE(
      AddFloatArray(prod, h.input_data, "effective_albedo", obs_dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, h.input_data, "scene_pressure", obs_dims, vals));
  EXPECT_TRUE(
      AddDoubleArray(prod, h.input_data, "tm5_constant_a", coef_dims, tm5_a));
  EXPECT_TRUE(
      AddDoubleArray(prod, h.input_data, "tm5_constant_b", coef_dims, tm5_b));
  EXPECT_TRUE(prod.AddField(h.input_data, "tm5_tropopause_layer_index",
                            prod.MakeInt32Array(obs_dims, trop_idx)));
  PopulateCloudInputVars(prod, h, n_obs);
}

TEST(HarpIngestS5pL2Test, So2VersionsModesAndColumnOptions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5P_L2_SO2");
  ASSERT_NE(nullptr, mod);

  // OFFL 01.00.00: so2_column=1km returns empty; default PBL succeeds.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5pConfig cfg{
        .product_type = "L2__SO2___",
        .processor_version = 10000,
        .has_layer_dim = true,
        .delta_time_3d = true,
    };
    S5pHandles h = BuildS5pBase(prod, cfg);
    PopulateSo2(prod, h, 4, cfg.num_layers, cfg.processor_version);
    FinalizeS5pHierarchy(prod, h);

    IngestExerciseResult r_empty =
        ExerciseIngestionModule(mod, prod.product(), "so2_column=1km");
    ASSERT_EQ(HARP_SUCCESS, r_empty.status);
    EXPECT_EQ(0, r_empty.dimensions[harp_dimension_time]);

    IngestExerciseResult r_def = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, r_def.status);
    EXPECT_FLOAT_EQ(0.1f, r_def.variables["surface_albedo"].float_values[0]);
    EXPECT_FLOAT_EQ(0.1f, r_def.variables["surface_albedo"].float_values[1]);
    EXPECT_FLOAT_EQ(0.2f, r_def.variables["surface_albedo"].float_values[2]);
    EXPECT_TRUE(std::isnan(r_def.variables["surface_albedo"].float_values[3]));
  }

  // NRTI 01.00.00: so2_column=1km succeeds, lh/3km return empty.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5pConfig cfg{
        .product_type = "L2__SO2___",
        .processor_version = 10000,
        .processing_mode = "NRTI",
        .has_layer_dim = true,
        .delta_time_3d = true,
    };
    S5pHandles h = BuildS5pBase(prod, cfg);
    PopulateSo2(prod, h, 4, cfg.num_layers, cfg.processor_version);
    FinalizeS5pHierarchy(prod, h);

    IngestExerciseResult r_1km =
        ExerciseIngestionModule(mod, prod.product(), "so2_column=1km");
    ASSERT_EQ(HARP_SUCCESS, r_1km.status);
    EXPECT_EQ(4, r_1km.dimensions[harp_dimension_time]);

    IngestExerciseResult r_lh =
        ExerciseIngestionModule(mod, prod.product(), "so2_column=lh");
    ASSERT_EQ(HARP_SUCCESS, r_lh.status);
    EXPECT_EQ(0, r_lh.dimensions[harp_dimension_time]);

    IngestExerciseResult r_3km =
        ExerciseIngestionModule(mod, prod.product(), "so2_column=3km");
    ASSERT_EQ(HARP_SUCCESS, r_3km.status);
    EXPECT_EQ(0, r_3km.dimensions[harp_dimension_time]);
  }

  // 02.05.00: so2_column=lh, 7km, 15km.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5pConfig cfg{
        .product_type = "L2__SO2___",
        .processor_version = 20500,
        .has_layer_dim = true,
        .delta_time_3d = true,
    };
    S5pHandles h = BuildS5pBase(prod, cfg);
    PopulateSo2(prod, h, 4, cfg.num_layers, cfg.processor_version);
    FinalizeS5pHierarchy(prod, h);

    for (const char* opt :
         {"so2_column=7km", "so2_column=15km", "so2_column=lh"}) {
      IngestExerciseResult res =
          ExerciseIngestionModule(mod, prod.product(), opt);
      ASSERT_EQ(HARP_SUCCESS, res.status) << opt;
      EXPECT_TRUE(
          res.variables["SO2_column_number_density_amf_uncertainty_random"]
              .included);
      EXPECT_TRUE(res.variables["SO2_layer_height"].included);
    }
  }

  // >= 02.08.00: so2_column=3km succeeds, amf_uncertainty_random excluded.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5pConfig cfg{
        .product_type = "L2__SO2___",
        .processor_version = 20800,
        .has_layer_dim = true,
        .delta_time_3d = true,
    };
    S5pHandles h = BuildS5pBase(prod, cfg);
    PopulateSo2(prod, h, 4, cfg.num_layers, cfg.processor_version);
    FinalizeS5pHierarchy(prod, h);

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "so2_column=3km");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_FALSE(
        res.variables["SO2_column_number_density_amf_uncertainty_random"]
            .included);
  }
}

// ============================================================================
// 11. S5P_L2_CLOUD
// ============================================================================

void PopulateCloud(CodaMemProduct& prod, const S5pHandles& h, long n_obs) {
  const long obs_dims[] = {1, n_obs};
  std::vector<float> vals(n_obs, 0.5f);
  std::vector<uint8_t> phase(n_obs, 1), qa_crb(n_obs, 88);
  if (n_obs > 1) {
    phase[1] = 255;  // maps to -1 as int8
  }
  EXPECT_TRUE(AddFloatArray(prod, h.product, "cloud_fraction", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product, "cloud_fraction_precision",
                            obs_dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, h.product, "cloud_base_pressure", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product, "cloud_base_pressure_precision",
                            obs_dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, h.product, "cloud_base_height", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product, "cloud_base_height_precision",
                            obs_dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, h.product, "cloud_top_pressure", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product, "cloud_top_pressure_precision",
                            obs_dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, h.product, "cloud_top_height", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product, "cloud_top_height_precision",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product, "cloud_optical_thickness",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(
      prod, h.product, "cloud_optical_thickness_precision", obs_dims, vals));

  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "cloud_fraction_apriori",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "cloud_top_temperature",
                            obs_dims, vals));
  EXPECT_TRUE(prod.AddField(h.detailed_results, "cloud_phase",
                            prod.MakeUint8Array(obs_dims, phase)));
  EXPECT_TRUE(prod.AddField(h.detailed_results, "qa_value_crb",
                            prod.MakeUint8Array(obs_dims, qa_crb)));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "surface_albedo_fitted",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "surface_albedo_fitted_precision", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "cloud_fraction_crb",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "cloud_fraction_crb_precision", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "cloud_pressure_crb",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "cloud_pressure_crb_precision", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "cloud_height_crb",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "cloud_height_crb_precision", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "cloud_albedo_crb",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "cloud_albedo_crb_precision", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "surface_albedo_fitted_crb", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "surface_albedo_fitted_crb_precision", obs_dims,
                            vals));

  // NIR detailed results.
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "cloud_fraction_nir",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "cloud_fraction_precision_nir", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "cloud_fraction_apriori_nir", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "cloud_top_height_nir",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "cloud_top_height_precision_nir", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "cloud_optical_thickness_nir", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "cloud_optical_thickness_precision_nir", obs_dims,
                            vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "surface_albedo_fitted_nir", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "surface_albedo_fitted_precision_nir", obs_dims,
                            vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "cloud_fraction_crb_nir",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "cloud_fraction_crb_precision_nir", obs_dims,
                            vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "cloud_height_crb_nir",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "cloud_height_crb_precision_nir", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results, "cloud_albedo_crb_nir",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "cloud_albedo_crb_precision_nir", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "surface_albedo_fitted_crb_nir", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.detailed_results,
                            "surface_albedo_fitted_crb_precision_nir", obs_dims,
                            vals));
}

TEST(HarpIngestS5pL2Test, CloudModelsAndBands) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5P_L2_CLOUD");
  ASSERT_NE(nullptr, mod);

  // < 02.01.03 with band=NIR returns empty product.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5pConfig cfg{
        .product_type = "L2__CLOUD_",
        .processor_version = 20100,
        .delta_time_3d = true,
        .include_nir_geoloc = true,
    };
    S5pHandles h = BuildS5pBase(prod, cfg);
    PopulateCloud(prod, h, 4);
    FinalizeS5pHierarchy(prod, h);

    IngestExerciseResult r_nir_old =
        ExerciseIngestionModule(mod, prod.product(), "band=NIR");
    ASSERT_EQ(HARP_SUCCESS, r_nir_old.status);
    EXPECT_EQ(0, r_nir_old.dimensions[harp_dimension_time]);

    IngestExerciseResult r_cal =
        ExerciseIngestionModule(mod, prod.product(), "model=CAL;band=UVVIS");
    ASSERT_EQ(HARP_SUCCESS, r_cal.status);
    EXPECT_THAT(r_cal.variables["cloud_type"].int8_values,
                ElementsAre(1, -1, 1, 1));
  }

  // >= 02.01.03 exercises all 4 model/band combinations.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5pConfig cfg{
        .product_type = "L2__CLOUD_",
        .processor_version = 20103,
        .delta_time_3d = true,
        .include_nir_geoloc = true,
    };
    S5pHandles h = BuildS5pBase(prod, cfg);
    PopulateCloud(prod, h, 4);
    FinalizeS5pHierarchy(prod, h);

    for (const char* opt : {"model=CAL;band=UVVIS", "model=CAL;band=NIR",
                            "model=CRB;band=UVVIS", "model=CRB;band=NIR"}) {
      IngestExerciseResult res =
          ExerciseIngestionModule(mod, prod.product(), opt);
      ASSERT_EQ(HARP_SUCCESS, res.status) << opt;
      EXPECT_EQ(4, res.dimensions[harp_dimension_time]);
      EXPECT_FLOAT_EQ(0.5f, res.variables["cloud_fraction"].float_values[0]);
    }
  }
}

// ============================================================================
// 12. S5P_L2_FRESCO
// ============================================================================

void PopulateFresco(CodaMemProduct& prod, const S5pHandles& h, long n_obs) {
  const long obs_dims[] = {1, n_obs};
  std::vector<float> vals(n_obs, 0.7f);
  EXPECT_TRUE(
      AddFloatArray(prod, h.product, "cloud_fraction_crb", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product, "cloud_fraction_crb_precision",
                            obs_dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, h.product, "cloud_pressure_crb", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product, "cloud_pressure_crb_precision",
                            obs_dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, h.product, "cloud_height_crb", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product, "cloud_height_crb_precision",
                            obs_dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, h.product, "cloud_albedo_crb", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product, "cloud_albedo_crb_precision",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product, "scene_albedo", obs_dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, h.product, "scene_albedo_precision", obs_dims, vals));
  EXPECT_TRUE(
      AddFloatArray(prod, h.product, "apparent_scene_height", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product, "apparent_scene_height_precision",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.product, "apparent_scene_pressure",
                            obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(
      prod, h.product, "apparent_scene_pressure_precision", obs_dims, vals));
  EXPECT_TRUE(AddFloatArray(prod, h.input_data, "surface_albedo_assumed",
                            obs_dims, vals));
}

TEST(HarpIngestS5pL2Test, FrescoVersions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5P_L2_FRESCO");
  ASSERT_NE(nullptr, mod);

  // < 01.00.00 excludes surface_pressure and scene_height.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5pConfig cfg{.product_type = "L2__FRESCO", .processor_version = 900};
    S5pHandles h = BuildS5pBase(prod, cfg);
    PopulateFresco(prod, h, 4);
    FinalizeS5pHierarchy(prod, h);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_FALSE(res.variables["surface_pressure"].included);
    EXPECT_FALSE(res.variables["scene_height"].included);
  }

  // >= 02.09.00 includes surface_pressure, winds, land_fraction, scene_height.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5pConfig cfg{.product_type = "L2__FRESCO", .processor_version = 20900};
    S5pHandles h = BuildS5pBase(prod, cfg);
    PopulateFresco(prod, h, 4);
    FinalizeS5pHierarchy(prod, h);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_TRUE(res.variables["surface_pressure"].included);
    EXPECT_TRUE(res.variables["land_fraction"].included);
    EXPECT_TRUE(res.variables["scene_height"].included);
    EXPECT_TRUE(res.variables["scene_height_uncertainty"].included);
    EXPECT_FLOAT_EQ(0.7f, res.variables["scene_height"].float_values[0]);
  }
}

// ============================================================================
// Error Paths
// ============================================================================

TEST(HarpIngestS5pL2Test, InitAndDimensionErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* aer_mod = FindIngestionModule("S5P_L2_AER_AI");
  const harp_ingestion_module* no2_mod = FindIngestionModule("S5P_L2_NO2");
  const harp_ingestion_module* o3_mod = FindIngestionModule("S5P_L2_O3");
  const harp_ingestion_module* o3pr_mod = FindIngestionModule("S5P_L2_O3_PR");
  const harp_ingestion_module* o3tcl_mod = FindIngestionModule("S5P_L2_O3_TCL");
  const harp_ingestion_module* so2_mod = FindIngestionModule("S5P_L2_SO2");
  ASSERT_NE(nullptr, aer_mod);
  ASSERT_NE(nullptr, no2_mod);
  ASSERT_NE(nullptr, o3_mod);
  ASSERT_NE(nullptr, o3pr_mod);
  ASSERT_NE(nullptr, o3tcl_mod);
  ASSERT_NE(nullptr, so2_mod);

  // 1. NULL coda_product -> coda_get_product_type fails -> HARP_ERROR_CODA.
  {
    ScopedHarpIngestionOptions opts;
    harp_ingestion_options* raw_opts = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_ingestion_options_new(&raw_opts));
    opts.reset(raw_opts);
    harp_product_definition* pdef = nullptr;
    void* user_data = nullptr;
    EXPECT_EQ(-1, aer_mod->ingestion_init(aer_mod, nullptr, opts.get(), &pdef,
                                          &user_data));
    EXPECT_EQ(HARP_ERROR_CODA, harp_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 2. Unsupported product type -> HARP_ERROR_INGESTION.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5P", "L2__BOGUS_", 1));
    IngestExerciseResult res = ExerciseIngestionModule(aer_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 3. Invalid @id type (int32 instead of string) and short @id string.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5P", "L2__AER_AI", 1));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "id", prod.MakeInt32(42)));
    IngestExerciseResult res = ExerciseIngestionModule(aer_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5P", "L2__AER_AI", 1));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "id", prod.MakeString("short_id")));
    // Fails at missing METADATA/GRANULE_DESCRIPTION@ProcessingMode.
    IngestExerciseResult res = ExerciseIngestionModule(aer_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 4. ProcessingMode wrong type (int32 instead of string).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5P", "L2__AER_AI", 1));
    CodaMemNode* meta = prod.MakeRecord();
    CodaMemNode* gd = prod.MakeRecord();
    ASSERT_TRUE(prod.AddAttribute(gd, "ProcessingMode", prod.MakeInt32(1)));
    ASSERT_TRUE(prod.AddField(meta, "GRANULE_DESCRIPTION", gd));
    ASSERT_TRUE(prod.AddField(prod.root(), "METADATA", meta));
    IngestExerciseResult res = ExerciseIngestionModule(aer_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 5. Missing sub-groups in init_cursors (PRODUCT, SO2_LAYER_HEIGHT,
  // SUPPORT_DATA, GEOLOCATIONS, DETAILED_RESULTS, INPUT_DATA).
  // Missing PRODUCT.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5P", "L2__AER_AI", 1));
    CodaMemNode* meta = prod.MakeRecord();
    CodaMemNode* gd = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddAttribute(gd, "ProcessingMode", prod.MakeString("OFFL")));
    ASSERT_TRUE(prod.AddField(meta, "GRANULE_DESCRIPTION", gd));
    ASSERT_TRUE(prod.AddField(prod.root(), "METADATA", meta));
    EXPECT_EQ(-1, ExerciseIngestionModule(aer_mod, prod.product()).status);
  }

  // Missing SO2_LAYER_HEIGHT for SO2 >= 02.05.00.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5P", "L2__SO2___", 1));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "id",
                          prod.MakeString(MakeProductId("L2__SO2___", 20500))));
    CodaMemNode* meta = prod.MakeRecord();
    CodaMemNode* gd = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddAttribute(gd, "ProcessingMode", prod.MakeString("OFFL")));
    ASSERT_TRUE(prod.AddField(meta, "GRANULE_DESCRIPTION", gd));
    ASSERT_TRUE(prod.AddField(prod.root(), "METADATA", meta));
    ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", prod.MakeRecord()));
    EXPECT_EQ(-1, ExerciseIngestionModule(so2_mod, prod.product()).status);
  }

  // Missing SUPPORT_DATA, GEOLOCATIONS, DETAILED_RESULTS, INPUT_DATA.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5P", "L2__AER_AI", 1));
    CodaMemNode* meta = prod.MakeRecord();
    CodaMemNode* gd = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddAttribute(gd, "ProcessingMode", prod.MakeString("OFFL")));
    ASSERT_TRUE(prod.AddField(meta, "GRANULE_DESCRIPTION", gd));
    ASSERT_TRUE(prod.AddField(prod.root(), "METADATA", meta));
    ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", prod.MakeRecord()));
    EXPECT_EQ(-1, ExerciseIngestionModule(aer_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5P", "L2__AER_AI", 1));
    CodaMemNode* meta = prod.MakeRecord();
    CodaMemNode* gd = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddAttribute(gd, "ProcessingMode", prod.MakeString("OFFL")));
    ASSERT_TRUE(prod.AddField(meta, "GRANULE_DESCRIPTION", gd));
    ASSERT_TRUE(prod.AddField(prod.root(), "METADATA", meta));
    CodaMemNode* p = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(p, "SUPPORT_DATA", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", p));
    EXPECT_EQ(-1, ExerciseIngestionModule(aer_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5P", "L2__AER_AI", 1));
    CodaMemNode* meta = prod.MakeRecord();
    CodaMemNode* gd = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddAttribute(gd, "ProcessingMode", prod.MakeString("OFFL")));
    ASSERT_TRUE(prod.AddField(meta, "GRANULE_DESCRIPTION", gd));
    ASSERT_TRUE(prod.AddField(prod.root(), "METADATA", meta));
    CodaMemNode* p = prod.MakeRecord();
    CodaMemNode* sup = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sup, "GEOLOCATIONS", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(p, "SUPPORT_DATA", sup));
    ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", p));
    EXPECT_EQ(-1, ExerciseIngestionModule(aer_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5P", "L2__AER_AI", 1));
    CodaMemNode* meta = prod.MakeRecord();
    CodaMemNode* gd = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddAttribute(gd, "ProcessingMode", prod.MakeString("OFFL")));
    ASSERT_TRUE(prod.AddField(meta, "GRANULE_DESCRIPTION", gd));
    ASSERT_TRUE(prod.AddField(prod.root(), "METADATA", meta));
    CodaMemNode* p = prod.MakeRecord();
    CodaMemNode* sup = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sup, "GEOLOCATIONS", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(sup, "DETAILED_RESULTS", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(p, "SUPPORT_DATA", sup));
    ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", p));
    EXPECT_EQ(-1, ExerciseIngestionModule(aer_mod, prod.product()).status);
  }

  // 6. Dimension validation errors:
  // - 2D `time` array (num_coda_dims != 1)
  // - `time` length != 1
  // - `corner` length != 4
  // - `o3` `level != layer + 1`
  // - `o3_pr` `level < 2`
  {
    CodaMemProduct prod;
    S5pConfig cfg{.product_type = "L2__AER_AI"};
    S5pHandles h = BuildS5pBase(prod, cfg);
    // Use O3_TCL to test 2D time without conflict on AER_AI.
    FinalizeS5pHierarchy(prod, h);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5P", "L2__O3_TCL", 1));
    CodaMemNode* p = prod.MakeRecord();
    CodaMemNode* sup = prod.MakeRecord();
    constexpr long k2D[] = {1, 2};
    constexpr double kT2[] = {0.0, 1.0};
    ASSERT_TRUE(prod.AddField(p, "time", prod.MakeDoubleArray(k2D, kT2)));
    ASSERT_TRUE(prod.AddField(sup, "DETAILED_RESULTS", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(p, "SUPPORT_DATA", sup));
    ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", p));
    IngestExerciseResult res =
        ExerciseIngestionModule(o3tcl_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5P", "L2__O3_TCL", 1));
    CodaMemNode* p = prod.MakeRecord();
    CodaMemNode* sup = prod.MakeRecord();
    constexpr long kTwo[] = {2};
    constexpr double kT2[] = {0.0, 1.0};
    ASSERT_TRUE(prod.AddField(p, "time", prod.MakeDoubleArray(kTwo, kT2)));
    ASSERT_TRUE(prod.AddField(sup, "DETAILED_RESULTS", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(p, "SUPPORT_DATA", sup));
    ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", p));
    IngestExerciseResult res =
        ExerciseIngestionModule(o3tcl_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  // Missing longitude in O3_TCL.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5P", "L2__O3_TCL", 1));
    CodaMemNode* p = prod.MakeRecord();
    CodaMemNode* sup = prod.MakeRecord();
    constexpr long kOne[] = {1};
    constexpr double kT1[] = {0.0};
    constexpr float kLat[] = {0.0f};
    ASSERT_TRUE(prod.AddField(p, "time", prod.MakeDoubleArray(kOne, kT1)));
    ASSERT_TRUE(prod.AddField(p, "latitude", prod.MakeFloatArray(kOne, kLat)));
    ASSERT_TRUE(prod.AddField(sup, "DETAILED_RESULTS", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(p, "SUPPORT_DATA", sup));
    ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", p));
    EXPECT_EQ(-1, ExerciseIngestionModule(o3tcl_mod, prod.product()).status);
  }
  // O3_PR level < 2.
  {
    CodaMemProduct prod;
    S5pConfig cfg{
        .product_type = "L2__O3__PR",
        .num_levels = 1,
        .has_level_dim = true,
    };
    S5pHandles h = BuildS5pBase(prod, cfg);
    FinalizeS5pHierarchy(prod, h);
    IngestExerciseResult res =
        ExerciseIngestionModule(o3pr_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  // O3 level != layer + 1.
  {
    CodaMemProduct prod;
    S5pConfig cfg{
        .product_type = "L2__O3____",
        .num_layers = 2,
        .num_levels = 4,
        .has_layer_dim = true,
        .has_level_dim = true,
    };
    S5pHandles h = BuildS5pBase(prod, cfg);
    FinalizeS5pHierarchy(prod, h);
    IngestExerciseResult res = ExerciseIngestionModule(o3_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  // Corner != 4.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5P", "L2__NO2___", 1));
    CodaMemNode* meta = prod.MakeRecord();
    CodaMemNode* gd = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddAttribute(gd, "ProcessingMode", prod.MakeString("OFFL")));
    ASSERT_TRUE(prod.AddField(meta, "GRANULE_DESCRIPTION", gd));
    ASSERT_TRUE(prod.AddField(prod.root(), "METADATA", meta));
    CodaMemNode* p = prod.MakeRecord();
    CodaMemNode* sup = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sup, "GEOLOCATIONS", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(sup, "DETAILED_RESULTS", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(sup, "INPUT_DATA", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(p, "SUPPORT_DATA", sup));
    constexpr long kOne[] = {1};
    constexpr long kTwo[] = {2};
    constexpr double kT1[] = {0.0};
    constexpr int32_t kI1[] = {0};
    constexpr int32_t kI2[] = {0, 1};
    ASSERT_TRUE(prod.AddField(p, "time", prod.MakeDoubleArray(kOne, kT1)));
    ASSERT_TRUE(prod.AddField(p, "scanline", prod.MakeInt32Array(kOne, kI1)));
    ASSERT_TRUE(
        prod.AddField(p, "ground_pixel", prod.MakeInt32Array(kOne, kI1)));
    ASSERT_TRUE(prod.AddField(p, "corner", prod.MakeInt32Array(kTwo, kI2)));
    ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", p));
    IngestExerciseResult res = ExerciseIngestionModule(no2_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
}

TEST(HarpIngestS5pL2Test, VariableReaderErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  // 1. O3_PR mismatched wavelength values & missing correlation_length.
  {
    const harp_ingestion_module* mod = FindIngestionModule("S5P_L2_O3_PR");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    S5pConfig cfg{
        .product_type = "L2__O3__PR",
        .processor_version = 20100,
        .has_level_dim = true,
    };
    S5pHandles h = BuildS5pBase(prod, cfg);
    // Add mismatched wavelength axes.
    constexpr long kSpec[] = {2};
    constexpr float kW1[] = {310.0f, 330.0f};
    constexpr float kW2[] = {310.0f, 340.0f};
    ASSERT_TRUE(
        AddFloatArray(prod, h.product, "dimension_surface_albedo", kSpec, kW1));
    ASSERT_TRUE(
        AddFloatArray(prod, h.product, "dimension_cloud_albedo", kSpec, kW2));
    FinalizeS5pHierarchy(prod, h);

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["wavelength"].read_status);
    EXPECT_EQ(
        -1, res.variables["O3_number_density_apriori_covariance"].read_status);
  }

  // 2. Bad time_coverage_resolution, missing/bad orbit, missing/wrong-size
  // dataset, missing @FillValue.
  {
    const harp_ingestion_module* mod = FindIngestionModule("S5P_L2_AER_AI");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5P", "L2__AER_AI", 1));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_resolution",
                                  prod.MakeString("INVALID")));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "orbit", prod.MakeString("not_an_int")));
    CodaMemNode* meta = prod.MakeRecord();
    CodaMemNode* gd = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddAttribute(gd, "ProcessingMode", prod.MakeString("OFFL")));
    ASSERT_TRUE(prod.AddField(meta, "GRANULE_DESCRIPTION", gd));
    ASSERT_TRUE(prod.AddField(prod.root(), "METADATA", meta));

    CodaMemNode* p = prod.MakeRecord();
    CodaMemNode* sup = prod.MakeRecord();
    CodaMemNode* det = prod.MakeRecord();
    constexpr long kOne[] = {1};
    constexpr long kTwo[] = {2};
    constexpr long kFour[] = {4};
    constexpr double kT1[] = {0.0};
    constexpr int32_t kI1[] = {0};
    constexpr int32_t kI4[4] = {};
    // Note: time lacks FillValue; latitude has wrong element count (2 != 1).
    ASSERT_TRUE(prod.AddField(p, "time", prod.MakeDoubleArray(kOne, kT1)));
    ASSERT_TRUE(prod.AddField(p, "scanline", prod.MakeInt32Array(kOne, kI1)));
    ASSERT_TRUE(
        prod.AddField(p, "ground_pixel", prod.MakeInt32Array(kOne, kI1)));
    ASSERT_TRUE(prod.AddField(p, "corner", prod.MakeInt32Array(kFour, kI4)));
    constexpr float kLat2[] = {1.0f, 2.0f};
    ASSERT_TRUE(AddFloatArray(prod, p, "latitude", kTwo, kLat2));
    // processing_quality_flags wrong element count.
    constexpr uint32_t kPqf2[] = {0, 1};
    ASSERT_TRUE(prod.AddField(det, "processing_quality_flags",
                              prod.MakeUint32Array(kTwo, kPqf2)));
    ASSERT_TRUE(prod.AddField(sup, "GEOLOCATIONS", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(sup, "DETAILED_RESULTS", det));
    ASSERT_TRUE(prod.AddField(sup, "INPUT_DATA", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(p, "SUPPORT_DATA", sup));
    ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", p));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_length"].read_status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["validity"].read_status);
  }

  // 3. O3_TCL bad time_coverage_start string format.
  {
    const harp_ingestion_module* mod = FindIngestionModule("S5P_L2_O3_TCL");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5P", "L2__O3_TCL", 1));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_start",
                                  prod.MakeString("not-a-timestamp")));
    CodaMemNode* p = prod.MakeRecord();
    CodaMemNode* sup = prod.MakeRecord();
    constexpr long kOne[] = {1};
    constexpr double kT1[] = {0.0};
    constexpr float kF1[] = {0.0f};
    ASSERT_TRUE(prod.AddField(p, "time", prod.MakeDoubleArray(kOne, kT1)));
    ASSERT_TRUE(prod.AddField(p, "latitude", prod.MakeFloatArray(kOne, kF1)));
    ASSERT_TRUE(prod.AddField(p, "longitude", prod.MakeFloatArray(kOne, kF1)));
    ASSERT_TRUE(prod.AddField(sup, "DETAILED_RESULTS", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(p, "SUPPORT_DATA", sup));
    ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", p));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_stop"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
