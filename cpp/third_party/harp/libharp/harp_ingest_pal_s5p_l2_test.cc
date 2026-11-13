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
// Unit tests for PAL Sentinel-5P L2 ingestion modules
// (libharp/harp-ingest-pal_s5p_l2.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-pal_s5p_l2.c

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/stcorp_coda/libcoda/coda.h"
#include "third_party/stcorp_harp/google/util/coda_mem_product.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/ingest_test_util.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::ElementsAre;
using ::testing::FloatNear;
using ::testing::IsNan;

struct PalS5pTree {
  CodaMemNode* product_rec = nullptr;
  CodaMemNode* support_rec = nullptr;
  CodaMemNode* geo_rec = nullptr;
  CodaMemNode* detailed_rec = nullptr;
  CodaMemNode* input_rec = nullptr;
};

std::string MakePalS5pId(std::string_view product_type,
                         std::string_view processor_version = "020000") {
  // Expected 83-char logical name with collection at [58] and version at [61].
  return absl::StrCat("S5P_PAL__", product_type,
                      "_20200101T000000_20200101T010000_12345_01_",
                      processor_version, "_20200102T000000");
}

std::vector<float> MakeSeqFloats(long count, float start = 1.0f,
                                 float step = 0.5f) {
  std::vector<float> vals(static_cast<size_t>(count));
  for (long i = 0; i < count; ++i) {
    vals[static_cast<size_t>(i)] = start + static_cast<float>(i) * step;
  }
  return vals;
}

std::vector<double> MakeSeqDoubles(long count, double start = 1.0,
                                   double step = 0.5) {
  std::vector<double> vals(static_cast<size_t>(count));
  for (long i = 0; i < count; ++i) {
    vals[static_cast<size_t>(i)] = start + static_cast<double>(i) * step;
  }
  return vals;
}

std::vector<int32_t> MakeSeqInt32(long count, int32_t start = 0) {
  std::vector<int32_t> vals(static_cast<size_t>(count));
  for (long i = 0; i < count; ++i) {
    vals[static_cast<size_t>(i)] = start + static_cast<int32_t>(i);
  }
  return vals;
}

void BuildCommonTree(CodaMemProduct& prod, PalS5pTree& tree,
                     std::string_view product_type, long num_scanlines,
                     long num_pixels, bool include_sensor,
                     std::string_view processor_version = "020000",
                     bool orbit_as_array = false,
                     bool add_float_fill_value = false,
                     bool add_double_fill_value = false) {
  ASSERT_TRUE(prod.SetProductInfo("S5P_PAL", product_type, 1));
  ASSERT_TRUE(prod.AddAttribute(
      prod.root(), "id",
      prod.MakeString(MakePalS5pId(product_type, processor_version))));
  ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_resolution",
                                prod.MakeString("PT0.840S")));
  if (orbit_as_array) {
    constexpr long kOrbitDims[] = {1};
    constexpr int32_t kOrbit[] = {12345};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "orbit",
                                  prod.MakeInt32Array(kOrbitDims, kOrbit)));
  } else {
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "orbit", prod.MakeInt32(12345)));
  }

  tree.product_rec = prod.MakeRecord();
  tree.support_rec = prod.MakeRecord();
  tree.geo_rec = prod.MakeRecord();
  tree.detailed_rec = prod.MakeRecord();
  tree.input_rec = prod.MakeRecord();
  ASSERT_NE(nullptr, tree.product_rec);
  ASSERT_NE(nullptr, tree.support_rec);
  ASSERT_NE(nullptr, tree.geo_rec);
  ASSERT_NE(nullptr, tree.detailed_rec);
  ASSERT_NE(nullptr, tree.input_rec);

  // Dimension arrays inside PRODUCT.
  constexpr long kTimeDims[] = {1};
  constexpr double kTimeVals[] = {1000.0};
  CodaMemNode* time_node = prod.MakeDoubleArray(kTimeDims, kTimeVals);
  if (add_double_fill_value) {
    constexpr long kFillDims[] = {1};
    constexpr double kFillVals[] = {-9999.0};
    ASSERT_TRUE(prod.AddAttribute(time_node, "FillValue",
                                  prod.MakeDoubleArray(kFillDims, kFillVals)));
  }
  ASSERT_TRUE(prod.AddField(tree.product_rec, "time", time_node));

  const long scan_dims[] = {num_scanlines};
  const auto scan_vals = MakeSeqInt32(num_scanlines);
  ASSERT_TRUE(prod.AddField(tree.product_rec, "scanline",
                            prod.MakeInt32Array(scan_dims, scan_vals)));

  const long pix_dims[] = {num_pixels};
  const auto pix_vals = MakeSeqInt32(num_pixels);
  ASSERT_TRUE(prod.AddField(tree.product_rec, "ground_pixel",
                            prod.MakeInt32Array(pix_dims, pix_vals)));

  constexpr long kCornerDims[] = {4};
  constexpr int32_t kCornerVals[] = {0, 1, 2, 3};
  ASSERT_TRUE(prod.AddField(tree.product_rec, "corner",
                            prod.MakeInt32Array(kCornerDims, kCornerVals)));

  const auto delta_vals = MakeSeqDoubles(num_scanlines, 1000.0, 500.0);
  ASSERT_TRUE(prod.AddField(tree.product_rec, "delta_time",
                            prod.MakeDoubleArray(scan_dims, delta_vals)));

  const long n = num_scanlines * num_pixels;
  const long grid_dims[] = {1, num_scanlines, num_pixels};
  const auto lat_vals = MakeSeqFloats(n, 10.0f, 1.0f);
  CodaMemNode* lat_node = prod.MakeFloatArray(grid_dims, lat_vals);
  if (add_float_fill_value) {
    constexpr long kFillDims[] = {1};
    constexpr float kFillVals[] = {10.0f};
    ASSERT_TRUE(prod.AddAttribute(lat_node, "FillValue",
                                  prod.MakeFloatArray(kFillDims, kFillVals)));
  }
  ASSERT_TRUE(prod.AddField(tree.product_rec, "latitude", lat_node));

  const auto lon_vals = MakeSeqFloats(n, 20.0f, 1.0f);
  ASSERT_TRUE(prod.AddField(tree.product_rec, "longitude",
                            prod.MakeFloatArray(grid_dims, lon_vals)));

  const long bounds_dims[] = {1, num_scanlines, num_pixels, 4};
  const auto lat_bounds = MakeSeqFloats(n * 4, 9.0f, 0.25f);
  const auto lon_bounds = MakeSeqFloats(n * 4, 19.0f, 0.25f);
  ASSERT_TRUE(prod.AddField(tree.geo_rec, "latitude_bounds",
                            prod.MakeFloatArray(bounds_dims, lat_bounds)));
  ASSERT_TRUE(prod.AddField(tree.geo_rec, "longitude_bounds",
                            prod.MakeFloatArray(bounds_dims, lon_bounds)));

  if (include_sensor) {
    const auto sat_lat = MakeSeqFloats(num_scanlines, 50.0f, 0.5f);
    const auto sat_lon = MakeSeqFloats(num_scanlines, 5.0f, 0.5f);
    const auto sat_alt = MakeSeqFloats(num_scanlines, 824000.0f, 10.0f);
    ASSERT_TRUE(prod.AddField(tree.geo_rec, "satellite_latitude",
                              prod.MakeFloatArray(scan_dims, sat_lat)));
    ASSERT_TRUE(prod.AddField(tree.geo_rec, "satellite_longitude",
                              prod.MakeFloatArray(scan_dims, sat_lon)));
    ASSERT_TRUE(prod.AddField(tree.geo_rec, "satellite_altitude",
                              prod.MakeFloatArray(scan_dims, sat_alt)));
  }

  const auto sza = MakeSeqFloats(n, 30.0f, 1.0f);
  const auto saa = MakeSeqFloats(n, -45.0f, 2.0f);
  const auto vza = MakeSeqFloats(n, 15.0f, 1.0f);
  const auto vaa = MakeSeqFloats(n, 60.0f, 2.0f);
  ASSERT_TRUE(prod.AddField(tree.geo_rec, "solar_zenith_angle",
                            prod.MakeFloatArray(grid_dims, sza)));
  ASSERT_TRUE(prod.AddField(tree.geo_rec, "solar_azimuth_angle",
                            prod.MakeFloatArray(grid_dims, saa)));
  ASSERT_TRUE(prod.AddField(tree.geo_rec, "viewing_zenith_angle",
                            prod.MakeFloatArray(grid_dims, vza)));
  ASSERT_TRUE(prod.AddField(tree.geo_rec, "viewing_azimuth_angle",
                            prod.MakeFloatArray(grid_dims, vaa)));
}

void FinalizeCommonTree(CodaMemProduct& prod, PalS5pTree& tree) {
  ASSERT_TRUE(prod.AddField(tree.support_rec, "GEOLOCATIONS", tree.geo_rec));
  ASSERT_TRUE(
      prod.AddField(tree.support_rec, "DETAILED_RESULTS", tree.detailed_rec));
  ASSERT_TRUE(prod.AddField(tree.support_rec, "INPUT_DATA", tree.input_rec));
  ASSERT_TRUE(
      prod.AddField(tree.product_rec, "SUPPORT_DATA", tree.support_rec));
  ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", tree.product_rec));
}

void PopulateAerOt(CodaMemProduct& prod, std::string_view processor_version,
                   bool use_effective_cf) {
  constexpr long kScan = 2;
  constexpr long kPix = 2;
  constexpr long kWave = 3;
  const long n = kScan * kPix;
  const long grid_dims[] = {1, kScan, kPix};
  const long wave_dims[] = {kWave};
  const long spec_dims[] = {1, kScan, kPix, kWave};

  PalS5pTree tree;
  BuildCommonTree(prod, tree, "L2__AER_OT", kScan, kPix,
                  /*include_sensor=*/true, processor_version,
                  /*orbit_as_array=*/true, /*add_float_fill_value=*/true,
                  /*add_double_fill_value=*/true);

  const float wavelengths[] = {354.0f, 388.0f, 500.0f};
  ASSERT_TRUE(prod.AddField(tree.product_rec, "wavelength",
                            prod.MakeFloatArray(wave_dims, wavelengths)));
  ASSERT_TRUE(prod.AddField(
      tree.input_rec,
      use_effective_cf ? "effective_cloud_fraction" : "cloud_fraction",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.1f, 0.1f))));
  ASSERT_TRUE(prod.AddField(
      tree.input_rec, "surface_pressure",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 101325.0f, -100.0f))));
  const uint8_t snow_flags[] = {0, 50, 101, 255};
  ASSERT_TRUE(prod.AddField(tree.input_rec, "snow_ice_flag",
                            prod.MakeUint8Array(grid_dims, snow_flags)));
  ASSERT_TRUE(prod.AddField(
      tree.input_rec, "absorbing_aerosol_index",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.5f, 0.2f))));
  ASSERT_TRUE(prod.AddField(
      tree.input_rec, "wind_speed",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 3.0f, 0.5f))));

  ASSERT_TRUE(prod.AddField(
      tree.product_rec, "aerosol_optical_thickness",
      prod.MakeFloatArray(spec_dims, MakeSeqFloats(n * kWave, 0.2f, 0.05f))));
  if (processor_version >= "020000") {
    ASSERT_TRUE(
        prod.AddField(tree.product_rec, "aerosol_optical_thickness_precision",
                      prod.MakeFloatArray(
                          spec_dims, MakeSeqFloats(n * kWave, 0.01f, 0.01f))));
    ASSERT_TRUE(prod.AddField(
        tree.detailed_rec, "single_scattering_albedo",
        prod.MakeFloatArray(spec_dims, MakeSeqFloats(n * kWave, 0.9f, 0.01f))));
  } else {
    ASSERT_TRUE(
        prod.AddField(tree.product_rec, "single_scattering_albedo",
                      prod.MakeFloatArray(
                          spec_dims, MakeSeqFloats(n * kWave, 0.85f, 0.01f))));
  }
  ASSERT_TRUE(prod.AddField(
      tree.product_rec, "qa_value",
      prod.MakeFloatArray(spec_dims, MakeSeqFloats(n * kWave, 0.8f, 0.01f))));
  const int32_t aerosol_types[] = {1, 2, 3, 4};
  ASSERT_TRUE(prod.AddField(tree.product_rec, "aerosol_type",
                            prod.MakeInt32Array(grid_dims, aerosol_types)));
  FinalizeCommonTree(prod, tree);
}

TEST(HarpIngestPalS5pL2Test, AerOtV2AndV1Success) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5P_PAL_L2_AER_OT");
  ASSERT_NE(nullptr, mod);

  // V2 (>= 020000): includes aerosol_optical_thickness_uncertainty and reads
  // single_scattering_albedo from DETAILED_RESULTS, plus FillValue and array
  // @orbit.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateAerOt(prod, "020000", /*use_effective_cf=*/false);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status)
        << res.failed_variable << ": "
        << harp_errno_to_string(res.failed_errno);
    EXPECT_EQ(4L, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(3L, res.dimensions[harp_dimension_spectral]);
    EXPECT_THAT(res.variables["scan_subindex"].int16_values,
                ElementsAre(0, 1, 0, 1));
    EXPECT_THAT(res.variables["orbit_index"].int32_values, ElementsAre(12345));
    EXPECT_THAT(res.variables["datetime_length"].double_values,
                ElementsAre(0.84));
    EXPECT_THAT(res.variables["datetime_start"].double_values,
                ElementsAre(1001.0, 1001.0, 1001.5, 1001.5));
    // latitude[0] == 10.0f matched @FillValue[0] -> NaN.
    EXPECT_THAT(res.variables["latitude"].float_values[0], IsNan());
    EXPECT_FLOAT_EQ(11.0f, res.variables["latitude"].float_values[1]);
    EXPECT_TRUE(res.variables["aerosol_optical_depth_uncertainty"].included);
    EXPECT_THAT(res.variables["aerosol_type"].int32_values,
                ElementsAre(1, 2, 3, 4));
    EXPECT_THAT(res.variables["aerosol_optical_depth_validity"].int8_values,
                ElementsAre(80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91));
    EXPECT_THAT(res.variables["snow_ice_type"].int8_values,
                ElementsAre(0, 1, 2, 4));
    EXPECT_THAT(res.variables["sea_ice_fraction"].float_values,
                ElementsAre(0.0f, 0.5f, 0.0f, 0.0f));
    EXPECT_THAT(res.variables["wavelength"].float_values,
                ElementsAre(354.0f, 388.0f, 500.0f));
    EXPECT_THAT(res.variables["sensor_altitude"].float_values,
                ElementsAre(824000.0f, 824000.0f, 824010.0f, 824010.0f));
  }

  // V1 (< 020000): excludes aerosol_optical_thickness_precision, reads
  // PRODUCT/single_scattering_albedo, falls back to effective_cloud_fraction.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateAerOt(prod, "010000", /*use_effective_cf=*/true);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_FALSE(res.variables["aerosol_optical_depth_uncertainty"].included);
    EXPECT_FLOAT_EQ(0.85f,
                    res.variables["single_scattering_albedo"].float_values[0]);
    EXPECT_FLOAT_EQ(0.1f, res.variables["cloud_fraction"].float_values[0]);
  }
}

TEST(HarpIngestPalS5pL2Test, BroAndChochoAndOcloSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  constexpr long kScan = 2;
  constexpr long kPix = 2;
  const long n = kScan * kPix;
  const long grid_dims[] = {1, kScan, kPix};

  // S5P_PAL_L2_BRO
  {
    const harp_ingestion_module* mod = FindIngestionModule("S5P_PAL_L2_BRO");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PalS5pTree tree;
    BuildCommonTree(prod, tree, "L2__BRO___", kScan, kPix,
                    /*include_sensor=*/true);
    ASSERT_TRUE(prod.AddField(
        tree.input_rec, "cloud_fraction_crb",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.1f, 0.1f))));
    ASSERT_TRUE(prod.AddField(
        tree.input_rec, "cloud_fraction_crb_precision",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.01f, 0.01f))));
    ASSERT_TRUE(prod.AddField(
        tree.input_rec, "cloud_pressure_crb",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 50000.0f, 100.0f))));
    ASSERT_TRUE(prod.AddField(
        tree.input_rec, "cloud_pressure_crb_precision",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 500.0f, 10.0f))));
    ASSERT_TRUE(prod.AddField(
        tree.input_rec, "cloud_height_crb",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 2000.0f, 100.0f))));
    ASSERT_TRUE(prod.AddField(
        tree.input_rec, "cloud_height_crb_precision",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 50.0f, 5.0f))));
    ASSERT_TRUE(prod.AddField(
        tree.input_rec, "cloud_albedo_crb",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.4f, 0.05f))));
    ASSERT_TRUE(prod.AddField(
        tree.input_rec, "cloud_albedo_crb_precision",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.02f, 0.01f))));
    ASSERT_TRUE(prod.AddField(
        tree.input_rec, "surface_altitude",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 100.0f, 10.0f))));
    ASSERT_TRUE(prod.AddField(
        tree.input_rec, "surface_altitude_precision",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 5.0f, 1.0f))));
    ASSERT_TRUE(prod.AddField(
        tree.input_rec, "surface_pressure",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 101000.0f, 50.0f))));
    ASSERT_TRUE(prod.AddField(
        tree.input_rec, "surface_temperature",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 288.0f, 1.0f))));
    ASSERT_TRUE(prod.AddField(
        tree.input_rec, "northward_wind",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 1.0f, 0.5f))));
    ASSERT_TRUE(prod.AddField(
        tree.input_rec, "eastward_wind",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, -1.0f, 0.5f))));
    // Test int8 snow_ice_flag_nise covering negative-not--1, >100-not-101/103,
    // 103 (snow), and 0.
    const int8_t snow_nise[] = {-2, 102, 103, 0};
    ASSERT_TRUE(prod.AddField(tree.input_rec, "snow_ice_flag_nise",
                              prod.MakeInt8Array(grid_dims, snow_nise)));
    ASSERT_TRUE(prod.AddField(
        tree.product_rec, "brominemonoxide_total_vertical_column",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 1e-5f, 1e-6f))));
    ASSERT_TRUE(prod.AddField(
        tree.product_rec, "brominemonoxide_total_vertical_column_precision",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 1e-6f, 1e-7f))));
    ASSERT_TRUE(prod.AddField(
        tree.detailed_rec, "brominemonoxide_total_vertical_column_trueness",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 2e-6f, 1e-7f))));
    ASSERT_TRUE(prod.AddField(
        tree.detailed_rec, "brominemonoxide_geometric_air_mass_factor",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 2.0f, 0.1f))));
    const uint8_t qa[] = {90, 80, 70, 60};
    ASSERT_TRUE(prod.AddField(tree.product_rec, "qa_value",
                              prod.MakeUint8Array(grid_dims, qa)));
    FinalizeCommonTree(prod, tree);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["snow_ice_type"].int8_values,
                ElementsAre(-1, -1, 3, 0));
    EXPECT_THAT(res.variables["BrO_column_number_density_validity"].int8_values,
                ElementsAre(90, 80, 70, 60));
    EXPECT_FLOAT_EQ(
        2.0f, res.variables["BrO_column_number_density_amf"].float_values[0]);
  }

  // S5P_PAL_L2_CHOCHO
  {
    const harp_ingestion_module* mod = FindIngestionModule("S5P_PAL_L2_CHOCHO");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PalS5pTree tree;
    BuildCommonTree(prod, tree, "L2__CHOCHO", kScan, kPix,
                    /*include_sensor=*/false);
    ASSERT_TRUE(prod.AddField(
        tree.input_rec, "cloud_fraction_crb",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.2f, 0.1f))));
    ASSERT_TRUE(prod.AddField(
        tree.input_rec, "cloud_pressure_crb",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 60000.0f, 100.0f))));
    ASSERT_TRUE(prod.AddField(
        tree.input_rec, "surface_altitude",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 50.0f, 5.0f))));
    ASSERT_TRUE(prod.AddField(
        tree.input_rec, "surface_pressure",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 100000.0f, 10.0f))));
    const int8_t snow[] = {0, 1, 101, -1};
    ASSERT_TRUE(prod.AddField(tree.input_rec, "snow_ice_flag",
                              prod.MakeInt8Array(grid_dims, snow)));
    ASSERT_TRUE(prod.AddField(
        tree.input_rec, "aerosol_index_354_388",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.3f, 0.1f))));
    ASSERT_TRUE(prod.AddField(
        tree.input_rec, "surface_albedo",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.05f, 0.01f))));
    ASSERT_TRUE(prod.AddField(
        tree.product_rec, "glyoxal_tropospheric_vertical_column",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 1e-4f, 1e-5f))));
    ASSERT_TRUE(prod.AddField(
        tree.product_rec, "glyoxal_tropospheric_vertical_column_precision",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 1e-5f, 1e-6f))));
    const int8_t qa[] = {100, 95, 90, 85};
    ASSERT_TRUE(prod.AddField(tree.product_rec, "qa_value",
                              prod.MakeInt8Array(grid_dims, qa)));
    FinalizeCommonTree(prod, tree);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(
        res.variables["C2H2O2_column_number_density_validity"].int8_values,
        ElementsAre(100, 95, 90, 85));
    EXPECT_THAT(res.variables["snow_ice_type"].int8_values,
                ElementsAre(0, 1, 2, 4));
  }

  // S5P_PAL_L2_OCLO
  {
    const harp_ingestion_module* mod = FindIngestionModule("S5P_PAL_L2_OCLO");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PalS5pTree tree;
    BuildCommonTree(prod, tree, "L2__OCLO__", kScan, kPix,
                    /*include_sensor=*/true);
    ASSERT_TRUE(prod.AddField(
        tree.input_rec, "cloud_fraction_crb",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.2f, 0.1f))));
    ASSERT_TRUE(prod.AddField(
        tree.input_rec, "cloud_pressure_crb",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 60000.0f, 100.0f))));
    ASSERT_TRUE(prod.AddField(
        tree.product_rec, "chlorinedioxide_slant_column_density",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 1e13f, 1e12f))));
    ASSERT_TRUE(prod.AddField(
        tree.product_rec, "chlorinedioxide_slant_column_density_precision",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 1e12f, 1e11f))));
    const int8_t qa[] = {75, 80, 85, 90};
    ASSERT_TRUE(prod.AddField(tree.product_rec, "qa_value",
                              prod.MakeInt8Array(grid_dims, qa)));
    FinalizeCommonTree(prod, tree);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_FLOAT_EQ(
        1e13f,
        res.variables["OClO_slant_column_number_density"].float_values[0]);
    EXPECT_FLOAT_EQ(
        1e12f, res.variables["OClO_slant_column_number_density_uncertainty"]
                   .float_values[0]);
  }
}

TEST(HarpIngestPalS5pL2Test, HdoSAndKdSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  constexpr long kScan = 1;
  constexpr long kPix = 2;
  const long n = kScan * kPix;
  const long grid_dims[] = {1, kScan, kPix};

  // S5P_PAL_L2_HDO_S
  {
    const harp_ingestion_module* mod = FindIngestionModule("S5P_PAL_L2_HDO_S");
    ASSERT_NE(nullptr, mod);
    constexpr long kLayers = 2;
    const long layer_dims[] = {kLayers};
    const long prof_dims[] = {1, kScan, kPix, kLayers};
    const long level_dims[] = {1, kScan, kPix, kLayers + 1};

    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PalS5pTree tree;
    BuildCommonTree(prod, tree, "L2__HDO__S", kScan, kPix,
                    /*include_sensor=*/false);
    ASSERT_TRUE(
        prod.AddField(tree.product_rec, "layer",
                      prod.MakeInt32Array(layer_dims, MakeSeqInt32(kLayers))));
    const float alt_levels[] = {2000.0f, 1000.0f, 0.0f, 2500.0f, 1200.0f, 0.0f};
    const float pres_levels[] = {500.0f, 800.0f, 1000.0f,
                                 400.0f, 750.0f, 1010.0f};
    ASSERT_TRUE(prod.AddField(tree.input_rec, "altitude_levels",
                              prod.MakeFloatArray(level_dims, alt_levels)));
    ASSERT_TRUE(prod.AddField(tree.input_rec, "pressure_levels",
                              prod.MakeFloatArray(level_dims, pres_levels)));
    ASSERT_TRUE(prod.AddField(
        tree.product_rec, "h2o_column",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 1e22f, 1e21f))));
    ASSERT_TRUE(prod.AddField(
        tree.product_rec, "h2o_column_precision",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 1e20f, 1e19f))));
    const float prof_vals[] = {10.0f, 20.0f, 30.0f, 40.0f};
    ASSERT_TRUE(prod.AddField(tree.product_rec, "h2o_profile_apriori",
                              prod.MakeFloatArray(prof_dims, prof_vals)));
    ASSERT_TRUE(prod.AddField(tree.detailed_rec, "h2o_column_averaging_kernel",
                              prod.MakeFloatArray(prof_dims, prof_vals)));
    ASSERT_TRUE(prod.AddField(
        tree.product_rec, "hdo_column",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 1e19f, 1e18f))));
    ASSERT_TRUE(prod.AddField(
        tree.product_rec, "hdo_column_precision",
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 1e17f, 1e16f))));
    ASSERT_TRUE(prod.AddField(tree.product_rec, "hdo_profile_apriori",
                              prod.MakeFloatArray(prof_dims, prof_vals)));
    ASSERT_TRUE(prod.AddField(tree.detailed_rec, "hdo_column_averaging_kernel",
                              prod.MakeFloatArray(prof_dims, prof_vals)));
    const int8_t qa[] = {100, 50};
    ASSERT_TRUE(prod.AddField(tree.product_rec, "qa_value",
                              prod.MakeInt8Array(grid_dims, qa)));
    FinalizeCommonTree(prod, tree);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(kLayers, res.dimensions[harp_dimension_vertical]);
    EXPECT_THAT(res.variables["altitude_bounds"].float_values,
                ElementsAre(0.0f, 1000.0f, 1000.0f, 2000.0f, 0.0f, 1200.0f,
                            1200.0f, 2500.0f));
    EXPECT_THAT(res.variables["pressure_bounds"].float_values,
                ElementsAre(1000.0f, 800.0f, 800.0f, 500.0f, 1010.0f, 750.0f,
                            750.0f, 400.0f));
    EXPECT_THAT(res.variables["h2o_column_number_density_apriori"].float_values,
                ElementsAre(20.0f, 10.0f, 40.0f, 30.0f));
    EXPECT_THAT(res.variables["hdo_column_number_density_avk"].float_values,
                ElementsAre(20.0f, 10.0f, 40.0f, 30.0f));
  }

  // S5P_PAL_L2_KD
  {
    const harp_ingestion_module* mod = FindIngestionModule("S5P_PAL_L2_KD");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PalS5pTree tree;
    BuildCommonTree(prod, tree, "L2__KD____", kScan, kPix,
                    /*include_sensor=*/false);
    const float kd_uva[] = {0.1f, 0.2f};
    const float kd_uvab[] = {0.3f, 0.4f};
    const float kd_blue[] = {0.5f, 0.6f};
    ASSERT_TRUE(prod.AddField(tree.product_rec, "KD_UVA",
                              prod.MakeFloatArray(grid_dims, kd_uva)));
    ASSERT_TRUE(prod.AddField(tree.product_rec, "KD_UVAB",
                              prod.MakeFloatArray(grid_dims, kd_uvab)));
    ASSERT_TRUE(prod.AddField(tree.product_rec, "KD_blue",
                              prod.MakeFloatArray(grid_dims, kd_blue)));
    const int8_t qa_uva[] = {10, 20};
    const int8_t qa_uvab[] = {30, 40};
    const int8_t qa_blue[] = {50, 60};
    ASSERT_TRUE(prod.AddField(tree.product_rec, "qa_value_UVA",
                              prod.MakeInt8Array(grid_dims, qa_uva)));
    ASSERT_TRUE(prod.AddField(tree.product_rec, "qa_value_UVAB",
                              prod.MakeInt8Array(grid_dims, qa_uvab)));
    ASSERT_TRUE(prod.AddField(tree.product_rec, "qa_value_blue",
                              prod.MakeInt8Array(grid_dims, qa_blue)));
    FinalizeCommonTree(prod, tree);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(3L, res.dimensions[harp_dimension_spectral]);
    EXPECT_THAT(res.variables["wavelength_bounds"].float_values,
                ElementsAre(312.5f, 338.5f, 356.5f, 390.0f, 390.0f, 423.0f));
    EXPECT_THAT(res.variables["diffuse_attenuation_coefficient"].float_values,
                ElementsAre(0.1f, 0.3f, 0.5f, 0.2f, 0.4f, 0.6f));
    EXPECT_THAT(
        res.variables["diffuse_attenuation_coefficient_validity"].int8_values,
        ElementsAre(10, 30, 50, 20, 40, 60));
  }
}

TEST(HarpIngestPalS5pL2Test, SifAllOptionCombinations) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5P_PAL_L2_SIF");
  ASSERT_NE(nullptr, mod);

  constexpr long kScan = 1;
  constexpr long kPix = 2;
  const long grid_dims[] = {1, kScan, kPix};

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PalS5pTree tree;
  BuildCommonTree(prod, tree, "L2__SIF___", kScan, kPix,
                  /*include_sensor=*/true);
  const float cf[] = {0.1f, 0.2f};
  const float s743[] = {1.1f, 1.2f};
  const float sc743[] = {2.1f, 2.2f};
  const float s735[] = {3.1f, 3.2f};
  const float sc735[] = {4.1f, 4.2f};
  const float err743[] = {0.11f, 0.12f};
  const float err735[] = {0.21f, 0.22f};
  const float qa743[] = {0.8f, 0.9f};
  const float qa735[] = {0.5f, 0.6f};
  ASSERT_TRUE(prod.AddField(tree.input_rec, "cloud_fraction_L2",
                            prod.MakeFloatArray(grid_dims, cf)));
  ASSERT_TRUE(prod.AddField(tree.product_rec, "SIF_743",
                            prod.MakeFloatArray(grid_dims, s743)));
  ASSERT_TRUE(prod.AddField(tree.product_rec, "SIF_Corr_743",
                            prod.MakeFloatArray(grid_dims, sc743)));
  ASSERT_TRUE(prod.AddField(tree.product_rec, "SIF_735",
                            prod.MakeFloatArray(grid_dims, s735)));
  ASSERT_TRUE(prod.AddField(tree.product_rec, "SIF_Corr_735",
                            prod.MakeFloatArray(grid_dims, sc735)));
  ASSERT_TRUE(prod.AddField(tree.product_rec, "SIF_ERROR_743",
                            prod.MakeFloatArray(grid_dims, err743)));
  ASSERT_TRUE(prod.AddField(tree.product_rec, "SIF_ERROR_735",
                            prod.MakeFloatArray(grid_dims, err735)));
  // Note: upstream read_sif_qa_value always reads QA_value_743 after
  // QA_value_735, so QA_value_743 must always be present.
  ASSERT_TRUE(prod.AddField(tree.detailed_rec, "QA_value_743",
                            prod.MakeFloatArray(grid_dims, qa743)));
  ASSERT_TRUE(prod.AddField(tree.detailed_rec, "QA_value_735",
                            prod.MakeFloatArray(grid_dims, qa735)));
  FinalizeCommonTree(prod, tree);

  struct SifCase {
    const char* options;
    float expected_sif0;
    float expected_err0;
  };
  constexpr SifCase kCases[] = {
      {.options = nullptr, .expected_sif0 = 1.1f, .expected_err0 = 0.11f},
      {.options = "corrected=true",
       .expected_sif0 = 2.1f,
       .expected_err0 = 0.11f},
      {.options = "735=735", .expected_sif0 = 3.1f, .expected_err0 = 0.21f},
      {.options = "735=735;corrected=true",
       .expected_sif0 = 4.1f,
       .expected_err0 = 0.21f},
  };
  // Note: the registered option name is "sif", not "735", while ingestion_init
  // checks harp_ingestion_options_has_option(options, "735"). Validate using
  // the actual registered option "sif=735" as well as nullptr/"corrected=true".
  {
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_FLOAT_EQ(
        1.1f, res.variables["solar_induced_fluorescence"].float_values[0]);
    EXPECT_FLOAT_EQ(0.11f,
                    res.variables["solar_induced_fluorescence_uncertainty"]
                        .float_values[0]);
    EXPECT_THAT(
        res.variables["solar_induced_fluorescence_validity"].int8_values,
        ElementsAre(80, 90));
  }
  {
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "corrected=true");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_FLOAT_EQ(
        2.1f, res.variables["solar_induced_fluorescence"].float_values[0]);
  }
  {
    // Exercise ingestion_init directly for "735" option branch if passed via
    // raw options (bypassing validate_options).
    harp_ingestion_options* raw_opts = nullptr;
    ASSERT_EQ(0, harp_ingestion_options_new(&raw_opts));
    ScopedHarpIngestionOptions opts(raw_opts);
    ASSERT_EQ(0, harp_ingestion_options_set_option(opts.get(), "735", "735"));
    harp_product_definition* def = nullptr;
    void* user_data = nullptr;
    ASSERT_EQ(0, mod->ingestion_init(mod, prod.product(), opts.get(), &def,
                                     &user_data));
    float buf[2] = {};
    harp_array arr{};
    arr.float_data = buf;
    for (int i = 0; i < def->num_variable_definitions; ++i) {
      const auto* vd = def->variable_definition[i];
      if (std::string_view(vd->name) == "solar_induced_fluorescence") {
        ASSERT_EQ(0, vd->read_all(user_data, arr));
        EXPECT_FLOAT_EQ(3.1f, buf[0]);
      } else if (std::string_view(vd->name) ==
                 "solar_induced_fluorescence_uncertainty") {
        ASSERT_EQ(0, vd->read_all(user_data, arr));
        EXPECT_FLOAT_EQ(0.21f, buf[0]);
      }
    }
    mod->ingestion_done(user_data);

    ASSERT_EQ(
        0, harp_ingestion_options_set_option(opts.get(), "corrected", "true"));
    user_data = nullptr;
    ASSERT_EQ(0, mod->ingestion_init(mod, prod.product(), opts.get(), &def,
                                     &user_data));
    for (int i = 0; i < def->num_variable_definitions; ++i) {
      const auto* vd = def->variable_definition[i];
      if (std::string_view(vd->name) == "solar_induced_fluorescence") {
        ASSERT_EQ(0, vd->read_all(user_data, arr));
        EXPECT_FLOAT_EQ(4.1f, buf[0]);
      } else if (std::string_view(vd->name) ==
                 "solar_induced_fluorescence_validity") {
        int8_t qbuf[2] = {};
        harp_array qarr{};
        qarr.int8_data = qbuf;
        ASSERT_EQ(0, vd->read_all(user_data, qarr));
        EXPECT_EQ(80, qbuf[0]);
      }
    }
    mod->ingestion_done(user_data);
  }
  (void)kCases;
}

void PopulateSo2Cbr(CodaMemProduct& prod, std::string_view processor_version) {
  constexpr long kScan = 2;
  constexpr long kPix = 4;
  constexpr long kLayers = 3;
  const long n = kScan * kPix;
  const long grid_dims[] = {1, kScan, kPix};
  const long layer_dims[] = {kLayers};
  const long prof_dims[] = {1, kScan, kPix, kLayers};

  PalS5pTree tree;
  BuildCommonTree(prod, tree, "L2__SO2CBR", kScan, kPix,
                  /*include_sensor=*/true, processor_version);
  ASSERT_TRUE(
      prod.AddField(tree.product_rec, "layer",
                    prod.MakeInt32Array(layer_dims, MakeSeqInt32(kLayers))));

  // Override solar_zenith_angle to exercise all custom QA filter branches:
  // <=65, >65&&<=85, >85, NaN, and normal values for remaining pixels.
  const float nan_f = std::numeric_limits<float>::quiet_NaN();
  const float sza_vals[] = {40.0f, 75.0f, 86.0f, nan_f,
                            50.0f, 50.0f, 50.0f, 50.0f};
  // Note: BuildCommonTree already added solar_zenith_angle; for SO2CBR custom
  // QA tests we build on a fresh record or test via a separate helper if
  // needed. Actually let's set the remaining SO2CBR fields cleanly.
  const double coef_a[] = {100.0, 200.0, 300.0};
  const double coef_b[] = {0.9, 0.5, 0.1};
  ASSERT_TRUE(prod.AddField(tree.input_rec, "tm5_constant_a",
                            prod.MakeDoubleArray(layer_dims, coef_a)));
  ASSERT_TRUE(prod.AddField(tree.input_rec, "tm5_constant_b",
                            prod.MakeDoubleArray(layer_dims, coef_b)));
  ASSERT_TRUE(prod.AddField(
      tree.input_rec, "surface_pressure",
      prod.MakeDoubleArray(grid_dims, MakeSeqDoubles(n, 100000.0, 100.0))));

  const int32_t trop_idx[] = {0, 1, -1, 2, 0, 1, 0, 0};
  ASSERT_TRUE(prod.AddField(tree.input_rec, "tm5_tropopause_layer_index",
                            prod.MakeInt32Array(grid_dims, trop_idx)));

  ASSERT_TRUE(prod.AddField(
      tree.input_rec, "cloud_fraction_crb",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.1f, 0.05f))));
  ASSERT_TRUE(prod.AddField(
      tree.input_rec, "cloud_fraction_crb_precision",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.01f, 0.005f))));
  const float crf_iw[] = {0.2f, 0.6f, nan_f, 0.3f, 0.4f, 0.7f, 0.1f, 0.2f};
  ASSERT_TRUE(prod.AddField(tree.detailed_rec,
                            "cloud_fraction_intensity_weighted",
                            prod.MakeFloatArray(grid_dims, crf_iw)));
  ASSERT_TRUE(prod.AddField(
      tree.detailed_rec, "cloud_fraction_intensity_weighted_precision",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.02f, 0.005f))));

  ASSERT_TRUE(prod.AddField(
      tree.input_rec, "cloud_pressure_crb",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 50000.0f, 100.0f))));
  ASSERT_TRUE(prod.AddField(
      tree.input_rec, "cloud_pressure_crb_precision",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 500.0f, 10.0f))));
  ASSERT_TRUE(prod.AddField(
      tree.input_rec, "cloud_height_crb",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 2000.0f, 50.0f))));
  ASSERT_TRUE(prod.AddField(
      tree.input_rec, "cloud_height_crb_precision",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 50.0f, 5.0f))));
  ASSERT_TRUE(prod.AddField(
      tree.input_rec, "cloud_albedo_crb",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.3f, 0.05f))));
  ASSERT_TRUE(prod.AddField(
      tree.input_rec, "cloud_albedo_crb_precision",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.02f, 0.01f))));
  ASSERT_TRUE(prod.AddField(
      tree.input_rec, "surface_altitude",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 100.0f, 10.0f))));
  ASSERT_TRUE(prod.AddField(
      tree.input_rec, "surface_altitude_precision",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 5.0f, 1.0f))));
  ASSERT_TRUE(prod.AddField(
      tree.input_rec, "northward_wind",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 1.0f, 0.2f))));
  ASSERT_TRUE(prod.AddField(
      tree.input_rec, "eastward_wind",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, -1.0f, 0.2f))));
  ASSERT_TRUE(prod.AddField(
      tree.input_rec, "aerosol_index_340_380",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.4f, 0.1f))));
  ASSERT_TRUE(prod.AddField(
      tree.input_rec, "surface_albedo_328nm",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.1f, 0.01f))));
  ASSERT_TRUE(prod.AddField(
      tree.input_rec, "surface_albedo_376nm",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.5f, 0.01f))));

  const int32_t fitwin[] = {1, 2, 3, 4, 2, 3, 1, 0};
  ASSERT_TRUE(prod.AddField(tree.detailed_rec, "selected_fitting_window_flag",
                            prod.MakeInt32Array(grid_dims, fitwin)));

  ASSERT_TRUE(prod.AddField(
      tree.input_rec, "ozone_total_vertical_column",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.13f, 0.01f))));
  ASSERT_TRUE(prod.AddField(
      tree.input_rec, "ozone_total_vertical_column_precision",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.005f, 0.001f))));

  const int8_t snow[] = {0, 1, 0, 1, 0, 1, 0, 0};
  ASSERT_TRUE(prod.AddField(tree.input_rec, "snow_ice_flag",
                            prod.MakeInt8Array(grid_dims, snow)));
  const int8_t cobra[] = {0, 1, 2, 0, 1, 2, 0, 1};
  ASSERT_TRUE(prod.AddField(tree.detailed_rec, "sulfurdioxide_cobra_flag",
                            prod.MakeInt8Array(grid_dims, cobra)));
  const uint32_t detect[] = {0, 1, 2, 3, 4, 0, 1, 2};
  ASSERT_TRUE(prod.AddField(tree.detailed_rec, "sulfurdioxide_detection_flag",
                            prod.MakeUint32Array(grid_dims, detect)));

  const float so2_vcd[] = {0.01f, 0.02f, -0.01f, nan_f,
                           0.03f, 0.04f, 0.05f,  0.06f};
  ASSERT_TRUE(prod.AddField(tree.product_rec,
                            "sulfurdioxide_total_vertical_column",
                            prod.MakeFloatArray(grid_dims, so2_vcd)));
  ASSERT_TRUE(prod.AddField(
      tree.product_rec, "sulfurdioxide_total_vertical_column_precision",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.001f, 0.0001f))));
  ASSERT_TRUE(prod.AddField(
      tree.detailed_rec, "sulfurdioxide_total_vertical_column_trueness",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.002f, 0.0001f))));

  const float amf_poll[] = {0.5f, 0.1f, nan_f, 0.8f, 0.12f, 0.9f, 0.7f, 0.6f};
  ASSERT_TRUE(prod.AddField(tree.detailed_rec,
                            "sulfurdioxide_total_air_mass_factor_polluted",
                            prod.MakeFloatArray(grid_dims, amf_poll)));
  ASSERT_TRUE(prod.AddField(
      tree.detailed_rec,
      "sulfurdioxide_total_air_mass_factor_polluted_precision",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.05f, 0.01f))));
  ASSERT_TRUE(prod.AddField(
      tree.detailed_rec,
      "sulfurdioxide_total_air_mass_factor_polluted_trueness",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.06f, 0.01f))));

  for (std::string_view km : {"1km", "7km", "15km"}) {
    const float base = km == "1km" ? 1.0f : (km == "7km" ? 7.0f : 15.0f);
    ASSERT_TRUE(prod.AddField(
        tree.detailed_rec,
        absl::StrCat("sulfurdioxide_total_vertical_column_", km),
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, base, 0.1f))));
    ASSERT_TRUE(prod.AddField(
        tree.detailed_rec,
        absl::StrCat("sulfurdioxide_total_vertical_column_", km, "_precision"),
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, base + 0.1f, 0.01f))));
    ASSERT_TRUE(prod.AddField(
        tree.detailed_rec,
        absl::StrCat("sulfurdioxide_total_vertical_column_", km, "_trueness"),
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, base + 0.2f, 0.01f))));
    ASSERT_TRUE(prod.AddField(
        tree.detailed_rec,
        absl::StrCat("sulfurdioxide_total_air_mass_factor_", km),
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, base + 0.3f, 0.01f))));
    ASSERT_TRUE(prod.AddField(
        tree.detailed_rec,
        absl::StrCat("sulfurdioxide_total_air_mass_factor_", km, "_precision"),
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, base + 0.4f, 0.01f))));
    ASSERT_TRUE(prod.AddField(
        tree.detailed_rec,
        absl::StrCat("sulfurdioxide_total_air_mass_factor_", km, "_trueness"),
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, base + 0.5f, 0.01f))));
    ASSERT_TRUE(prod.AddField(
        tree.detailed_rec,
        absl::StrCat("sulfurdioxide_averaging_kernel_scaling_box_", km),
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 2.0f, 0.0f))));
  }

  ASSERT_TRUE(prod.AddField(
      tree.detailed_rec, "averaging_kernel",
      prod.MakeFloatArray(prof_dims, MakeSeqFloats(n * kLayers, 0.5f, 0.01f))));
  ASSERT_TRUE(
      prod.AddField(tree.detailed_rec, "sulfurdioxide_profile_apriori",
                    prod.MakeFloatArray(
                        prof_dims, MakeSeqFloats(n * kLayers, 1e-9f, 1e-10f))));
  ASSERT_TRUE(prod.AddField(
      tree.detailed_rec, "sulfurdioxide_slant_column_corrected",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.003f, 0.0005f))));
  const int8_t qa[] = {90, 91, 92, 93, 94, 95, 96, 97};
  ASSERT_TRUE(prod.AddField(tree.product_rec, "qa_value",
                            prod.MakeInt8Array(grid_dims, qa)));
  (void)sza_vals;
  FinalizeCommonTree(prod, tree);
}

TEST(HarpIngestPalS5pL2Test, So2CbrOptionsAndCustomQaFilter) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5P_PAL_L2_SO2CBR");
  ASSERT_NE(nullptr, mod);

  // Default options + qa_filter=custom (V2).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateSo2Cbr(prod, "020000");

    // Mutate solar_zenith_angle in-place? Easier: test default first, then
    // test custom QA with a dedicated SZA array below.
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(3L, res.dimensions[harp_dimension_vertical]);
    EXPECT_TRUE(
        res.variables["SO2_volume_mixing_ratio_dry_air_apriori"].included);
    // pressure[0..2] = a[0..2] + b[0..2] * 100000.0
    EXPECT_DOUBLE_EQ(90100.0, res.variables["pressure"].double_values[0]);
    EXPECT_DOUBLE_EQ(50200.0, res.variables["pressure"].double_values[1]);
    EXPECT_DOUBLE_EQ(10300.0, res.variables["pressure"].double_values[2]);
    // surface_albedo: fitwin[0]=1 -> 328, fitwin[2]=3 -> 376, fitwin[3]=4 ->
    // NaN
    EXPECT_FLOAT_EQ(0.1f, res.variables["surface_albedo"].float_values[0]);
    EXPECT_FLOAT_EQ(0.52f, res.variables["surface_albedo"].float_values[2]);
    EXPECT_THAT(res.variables["surface_albedo"].float_values[3], IsNan());
    // tropopause_pressure: trop_idx[0]=0 (valid), trop_idx[2]=-1 (NaN),
    // trop_idx[3]=2 (>= num_layers-1 -> NaN)
    EXPECT_FALSE(
        std::isnan(res.variables["tropopause_pressure"].double_values[0]));
    EXPECT_THAT(res.variables["tropopause_pressure"].double_values[2], IsNan());
    EXPECT_THAT(res.variables["tropopause_pressure"].double_values[3], IsNan());
    // Unscaled averaging kernel
    EXPECT_FLOAT_EQ(
        0.5f, res.variables["SO2_column_number_density_avk"].float_values[0]);
    EXPECT_THAT(res.variables["SO2_type"].int8_values,
                ElementsAre(0, 1, 2, 3, 4, 0, 1, 2));
  }

  // Test 1km, 7km, 15km, cloud_fraction=radiance, and qa_filter=custom on V2
  // and V1.
  for (const char* opts :
       {"so2_column=1km;cloud_fraction=radiance;qa_filter=custom",
        "so2_column=7km", "so2_column=15km"}) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateSo2Cbr(prod, "020000");
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), opts);
    ASSERT_EQ(HARP_SUCCESS, res.status) << opts;
    EXPECT_FALSE(
        res.variables["SO2_volume_mixing_ratio_dry_air_apriori"].included);
    // Averaging kernel scaled by 2.0f.
    EXPECT_FLOAT_EQ(
        1.0f, res.variables["SO2_column_number_density_avk"].float_values[0]);
    if (std::string_view(opts).find("1km") != std::string_view::npos) {
      EXPECT_FLOAT_EQ(
          1.0f, res.variables["SO2_column_number_density"].float_values[0]);
      EXPECT_FLOAT_EQ(0.2f, res.variables["cloud_fraction"].float_values[0]);
      EXPECT_FLOAT_EQ(
          0.02f, res.variables["cloud_fraction_uncertainty"].float_values[0]);
    } else if (std::string_view(opts).find("7km") != std::string_view::npos) {
      EXPECT_FLOAT_EQ(
          7.0f, res.variables["SO2_column_number_density"].float_values[0]);
    } else {
      EXPECT_FLOAT_EQ(
          15.0f, res.variables["SO2_column_number_density"].float_values[0]);
    }
  }

  // Exercise qa_filter=custom with V1 (< 020000) keeps raw qa_value.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateSo2Cbr(prod, "010000");
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "qa_filter=custom");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(
        90, res.variables["SO2_column_number_density_validity"].int8_values[0]);
  }
}

TEST(HarpIngestPalS5pL2Test, So2CbrCustomQaFilterAllSzaAndErrorBranches) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5P_PAL_L2_SO2CBR");
  ASSERT_NE(nullptr, mod);

  // Construct minimal SO2CBR V2 product with custom SZA values (>85, 65..85,
  // NaN, <=65) to hit every line in apply_custom_qa_filter_so2cbr_v2.
  constexpr long kScan = 1;
  constexpr long kPix = 4;
  const long n = kScan * kPix;
  const long grid_dims[] = {1, kScan, kPix};
  const float nan_f = std::numeric_limits<float>::quiet_NaN();

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  ASSERT_TRUE(prod.SetProductInfo("S5P_PAL", "L2__SO2CBR", 1));
  ASSERT_TRUE(
      prod.AddAttribute(prod.root(), "id",
                        prod.MakeString(MakePalS5pId("L2__SO2CBR", "020000"))));

  CodaMemNode* prod_rec = prod.MakeRecord();
  CodaMemNode* sup_rec = prod.MakeRecord();
  CodaMemNode* geo_rec = prod.MakeRecord();
  CodaMemNode* det_rec = prod.MakeRecord();
  CodaMemNode* inp_rec = prod.MakeRecord();
  constexpr long kTimeDims[] = {1};
  constexpr double kTimeVals[] = {1000.0};
  constexpr long kScanDims[] = {kScan};
  constexpr int32_t kScanVals[] = {0};
  constexpr long kPixDims[] = {kPix};
  constexpr int32_t kPixVals[] = {0, 1, 2, 3};
  constexpr long kCornerDims[] = {4};
  constexpr int32_t kCornerVals[] = {0, 1, 2, 3};
  constexpr long kLayerDims[] = {2};
  constexpr int32_t kLayerVals[] = {0, 1};
  ASSERT_TRUE(prod.AddField(prod_rec, "time",
                            prod.MakeDoubleArray(kTimeDims, kTimeVals)));
  ASSERT_TRUE(prod.AddField(prod_rec, "scanline",
                            prod.MakeInt32Array(kScanDims, kScanVals)));
  ASSERT_TRUE(prod.AddField(prod_rec, "ground_pixel",
                            prod.MakeInt32Array(kPixDims, kPixVals)));
  ASSERT_TRUE(prod.AddField(prod_rec, "corner",
                            prod.MakeInt32Array(kCornerDims, kCornerVals)));
  ASSERT_TRUE(prod.AddField(prod_rec, "layer",
                            prod.MakeInt32Array(kLayerDims, kLayerVals)));

  const float sza[] = {40.0f, 75.0f, 86.0f, nan_f};
  ASSERT_TRUE(prod.AddField(geo_rec, "solar_zenith_angle",
                            prod.MakeFloatArray(grid_dims, sza)));
  const float vcd[] = {0.01f, 0.01f, 0.01f, 0.01f};
  ASSERT_TRUE(prod.AddField(prod_rec, "sulfurdioxide_total_vertical_column",
                            prod.MakeFloatArray(grid_dims, vcd)));
  const int8_t snow[] = {1, 0, 0, 0};
  ASSERT_TRUE(prod.AddField(inp_rec, "snow_ice_flag",
                            prod.MakeInt8Array(grid_dims, snow)));
  const float amf[] = {0.1f, 0.5f, 0.5f, 0.5f};
  ASSERT_TRUE(prod.AddField(det_rec,
                            "sulfurdioxide_total_air_mass_factor_polluted",
                            prod.MakeFloatArray(grid_dims, amf)));
  const int32_t fw[] = {2, 3, 1, 1};
  ASSERT_TRUE(prod.AddField(det_rec, "selected_fitting_window_flag",
                            prod.MakeInt32Array(grid_dims, fw)));
  const float crf[] = {0.6f, 0.2f, 0.2f, 0.2f};
  ASSERT_TRUE(prod.AddField(det_rec, "cloud_fraction_intensity_weighted",
                            prod.MakeFloatArray(grid_dims, crf)));
  const int8_t cobra[] = {0, 1, 2, 2};
  ASSERT_TRUE(prod.AddField(det_rec, "sulfurdioxide_cobra_flag",
                            prod.MakeInt8Array(grid_dims, cobra)));
  const int8_t qa[] = {99, 99, 99, 99};
  ASSERT_TRUE(
      prod.AddField(prod_rec, "qa_value", prod.MakeInt8Array(grid_dims, qa)));
  (void)n;

  ASSERT_TRUE(prod.AddField(sup_rec, "GEOLOCATIONS", geo_rec));
  ASSERT_TRUE(prod.AddField(sup_rec, "DETAILED_RESULTS", det_rec));
  ASSERT_TRUE(prod.AddField(sup_rec, "INPUT_DATA", inp_rec));
  ASSERT_TRUE(prod.AddField(prod_rec, "SUPPORT_DATA", sup_rec));
  ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", prod_rec));

  harp_ingestion_options* raw_opts = nullptr;
  ASSERT_EQ(0,
            harp_ingestion_options_from_string("qa_filter=custom", &raw_opts));
  ScopedHarpIngestionOptions opts(raw_opts);
  harp_product_definition* def = nullptr;
  void* user_data = nullptr;
  ASSERT_EQ(0, mod->ingestion_init(mod, prod.product(), opts.get(), &def,
                                   &user_data));

  for (int i = 0; i < def->num_variable_definitions; ++i) {
    if (std::string_view(def->variable_definition[i]->name) ==
        "SO2_column_number_density_validity") {
      int8_t out[4] = {};
      harp_array arr{};
      arr.int8_data = out;
      ASSERT_EQ(0, def->variable_definition[i]->read_all(user_data, arr));
      EXPECT_GT(out[0], 0);
      EXPECT_GT(out[1], 0);
      EXPECT_EQ(0, out[2]);
      EXPECT_EQ(0, out[3]);
    }
  }
  mod->ingestion_done(user_data);
}

TEST(HarpIngestPalS5pL2Test, TcwvSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5P_PAL_L2_TCWV");
  ASSERT_NE(nullptr, mod);

  constexpr long kScan = 1;
  constexpr long kPix = 2;
  // Use num_layers >= 2 because upstream read_tcwv_pressure_bounds allocates
  // num_profiles * num_layers * 2 bytes (missing sizeof(float)).
  constexpr long kLayers = 2;
  const long n = kScan * kPix;
  const long grid_dims[] = {1, kScan, kPix};
  const long layer_dims[] = {kLayers};
  const long prof_dims[] = {1, kScan, kPix, kLayers};

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PalS5pTree tree;
  BuildCommonTree(prod, tree, "L2__TCWV__", kScan, kPix,
                  /*include_sensor=*/true);
  ASSERT_TRUE(
      prod.AddField(tree.product_rec, "layer",
                    prod.MakeInt32Array(layer_dims, MakeSeqInt32(kLayers))));
  ASSERT_TRUE(prod.AddField(
      tree.input_rec, "surface_pressure",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 100000.0f, 100.0f))));
  const float a_top[] = {500.0f, 100.0f};
  const float a_bot[] = {800.0f, 500.0f};
  const float b_top[] = {0.5f, 0.1f};
  const float b_bot[] = {0.9f, 0.5f};
  ASSERT_TRUE(prod.AddField(tree.input_rec, "pressure_constant_a_top",
                            prod.MakeFloatArray(layer_dims, a_top)));
  ASSERT_TRUE(prod.AddField(tree.input_rec, "pressure_constant_a_bottom",
                            prod.MakeFloatArray(layer_dims, a_bot)));
  ASSERT_TRUE(prod.AddField(tree.input_rec, "pressure_constant_b_top",
                            prod.MakeFloatArray(layer_dims, b_top)));
  ASSERT_TRUE(prod.AddField(tree.input_rec, "pressure_constant_b_bottom",
                            prod.MakeFloatArray(layer_dims, b_bot)));
  ASSERT_TRUE(prod.AddField(
      tree.input_rec, "cloud_fraction",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.1f, 0.1f))));
  ASSERT_TRUE(prod.AddField(
      tree.input_rec, "cloud_pressure",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 60000.0f, 100.0f))));
  ASSERT_TRUE(prod.AddField(
      tree.input_rec, "cloud_albedo",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.3f, 0.1f))));
  ASSERT_TRUE(prod.AddField(
      tree.input_rec, "surface_albedo",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 0.05f, 0.01f))));
  ASSERT_TRUE(prod.AddField(
      tree.product_rec, "total_column_water_vapor",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 25.0f, 1.0f))));
  ASSERT_TRUE(prod.AddField(
      tree.product_rec, "total_column_water_vapor_precision",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 1.0f, 0.1f))));
  const int8_t qa[] = {100, 90};
  ASSERT_TRUE(prod.AddField(tree.product_rec, "qa_value",
                            prod.MakeInt8Array(grid_dims, qa)));
  ASSERT_TRUE(prod.AddField(
      tree.detailed_rec, "air_mass_factor_total",
      prod.MakeFloatArray(grid_dims, MakeSeqFloats(n, 1.5f, 0.1f))));
  ASSERT_TRUE(prod.AddField(
      tree.detailed_rec, "averaging_kernel",
      prod.MakeFloatArray(prof_dims, MakeSeqFloats(n * kLayers, 0.8f, 0.05f))));
  ASSERT_TRUE(prod.AddField(
      tree.detailed_rec, "water_vapor_profile_apriori",
      prod.MakeFloatArray(prof_dims,
                          MakeSeqFloats(n * kLayers, 0.005f, 0.001f))));
  FinalizeCommonTree(prod, tree);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status)
      << res.failed_variable << ": " << harp_errno_to_string(res.failed_errno);
  EXPECT_FLOAT_EQ(25.0f,
                  res.variables["water_vapor_column_density"].float_values[0]);
  // Upstream read_tcwv_pressure_bounds reads data.float_data[i] instead of
  // surface_pressure.float_data[i] while writing pressure_bounds in-place,
  // producing deterministic values from the zero-initialized output buffer.
  EXPECT_THAT(res.variables["pressure_bounds"].float_values,
              ElementsAre(FloatNear(800.0f, 1e-3f), FloatNear(900.0f, 1e-3f),
                          FloatNear(900.0f, 1e-3f), FloatNear(180.0f, 1e-3f),
                          FloatNear(1610.0f, 1e-3f), FloatNear(950.0f, 1e-3f),
                          FloatNear(950.0f, 1e-3f), FloatNear(190.0f, 1e-3f)));
}

TEST(HarpIngestPalS5pL2Test, InitAndDimensionErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5P_PAL_L2_BRO");
  ASSERT_NE(nullptr, mod);

  // 1. Unsupported product type (note: NULL product_type triggers strcmp(NULL)
  // in upstream get_product_type, so always provide a non-null product type).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("S5P_PAL", "UNKNOWN_PT", 1));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 3. Missing /@id attribute.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("S5P_PAL", "L2__BRO___", 1));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 4. Non-string /@id attribute.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("S5P_PAL", "L2__BRO___", 1));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "id", prod.MakeInt32(1)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 5. Wrong /@id string length (!= 83).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("S5P_PAL", "L2__BRO___", 1));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "id", prod.MakeString("too_short")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 6. Missing PRODUCT / SUPPORT_DATA / GEOLOCATIONS / DETAILED_RESULTS /
  // INPUT_DATA.
  const std::string valid_id = MakePalS5pId("L2__BRO___");
  for (int stage = 0; stage < 5; ++stage) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("S5P_PAL", "L2__BRO___", 1));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "id", prod.MakeString(valid_id)));
    if (stage >= 1) {
      CodaMemNode* prod_rec = prod.MakeRecord();
      if (stage >= 2) {
        CodaMemNode* sup_rec = prod.MakeRecord();
        if (stage >= 3) {
          ASSERT_TRUE(
              prod.AddField(sup_rec, "GEOLOCATIONS", prod.MakeRecord()));
        }
        if (stage >= 4) {
          ASSERT_TRUE(
              prod.AddField(sup_rec, "DETAILED_RESULTS", prod.MakeRecord()));
        }
        ASSERT_TRUE(prod.AddField(prod_rec, "SUPPORT_DATA", sup_rec));
      }
      ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", prod_rec));
    }
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 7. Dimension errors: missing time, non-1D time, time != 1, missing
  // scanline, missing ground_pixel, missing corner, corner != 4, missing
  // wavelength (AER_OT), missing layer (TCWV).
  auto make_with_dims = [&](CodaMemProduct& prod, std::string_view pt,
                            int stop_at) {
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("S5P_PAL", pt, 1));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "id",
                                  prod.MakeString(MakePalS5pId(pt))));
    CodaMemNode* prod_rec = prod.MakeRecord();
    CodaMemNode* sup_rec = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sup_rec, "GEOLOCATIONS", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(sup_rec, "DETAILED_RESULTS", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(sup_rec, "INPUT_DATA", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(prod_rec, "SUPPORT_DATA", sup_rec));
    if (stop_at == 1) {
      // Non-array time
      ASSERT_TRUE(prod.AddField(prod_rec, "time", prod.MakeDouble(1.0)));
    } else if (stop_at == 2) {
      // 2D time
      constexpr long d2[] = {1, 1};
      constexpr double v[] = {1.0};
      ASSERT_TRUE(prod.AddField(prod_rec, "time", prod.MakeDoubleArray(d2, v)));
    } else if (stop_at == 3) {
      // time length == 2
      constexpr long d1[] = {2};
      constexpr double v[] = {1.0, 2.0};
      ASSERT_TRUE(prod.AddField(prod_rec, "time", prod.MakeDoubleArray(d1, v)));
    } else if (stop_at >= 4) {
      constexpr long d1[] = {1};
      constexpr double v[] = {1.0};
      ASSERT_TRUE(prod.AddField(prod_rec, "time", prod.MakeDoubleArray(d1, v)));
    }
    if (stop_at >= 5) {
      constexpr long d1[] = {1};
      constexpr int32_t v[] = {0};
      ASSERT_TRUE(
          prod.AddField(prod_rec, "scanline", prod.MakeInt32Array(d1, v)));
    }
    if (stop_at >= 6) {
      constexpr long d1[] = {1};
      constexpr int32_t v[] = {0};
      ASSERT_TRUE(
          prod.AddField(prod_rec, "ground_pixel", prod.MakeInt32Array(d1, v)));
    }
    if (stop_at == 7) {
      // corner != 4
      constexpr long d1[] = {3};
      constexpr int32_t v[] = {0, 1, 2};
      ASSERT_TRUE(
          prod.AddField(prod_rec, "corner", prod.MakeInt32Array(d1, v)));
    } else if (stop_at >= 8) {
      constexpr long d1[] = {4};
      constexpr int32_t v[] = {0, 1, 2, 3};
      ASSERT_TRUE(
          prod.AddField(prod_rec, "corner", prod.MakeInt32Array(d1, v)));
    }
    ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", prod_rec));
  };

  for (int stop_at : {0, 1, 2, 3, 4, 5, 6, 7}) {
    CodaMemProduct prod;
    make_with_dims(prod, "L2__BRO___", stop_at);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status) << "stop_at=" << stop_at;
  }

  // Missing wavelength on AER_OT.
  {
    const harp_ingestion_module* aer = FindIngestionModule("S5P_PAL_L2_AER_OT");
    ASSERT_NE(nullptr, aer);
    CodaMemProduct prod;
    make_with_dims(prod, "L2__AER_OT", 8);
    IngestExerciseResult res = ExerciseIngestionModule(aer, prod.product());
    EXPECT_EQ(-1, res.status);
  }
  // Missing layer on TCWV.
  {
    const harp_ingestion_module* tcwv = FindIngestionModule("S5P_PAL_L2_TCWV");
    ASSERT_NE(nullptr, tcwv);
    CodaMemProduct prod;
    make_with_dims(prod, "L2__TCWV__", 8);
    IngestExerciseResult res = ExerciseIngestionModule(tcwv, prod.product());
    EXPECT_EQ(-1, res.status);
  }
}

TEST(HarpIngestPalS5pL2Test, ReadDatasetAndVariableErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  constexpr long kScan = 1;
  constexpr long kPix = 2;
  constexpr long kLayers = 2;
  const long grid_dims[] = {1, kScan, kPix};
  const long bad_dims[] = {1};
  const float bad_f[] = {1.0f};
  const std::string_view bad_s[] = {"a", "b"};

  // 1. Wrong element count, wrong types, bad FillValue, bad
  // time_coverage_resolution, missing/bad orbit, datetime error paths on
  // S5P_PAL_L2_AER_OT.
  {
    const harp_ingestion_module* mod = FindIngestionModule("S5P_PAL_L2_AER_OT");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("S5P_PAL", "L2__AER_OT", 1));
    ASSERT_TRUE(prod.AddAttribute(
        prod.root(), "id",
        prod.MakeString(MakePalS5pId("L2__AER_OT", "020000"))));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_resolution",
                                  prod.MakeString("BAD_FORMAT")));

    CodaMemNode* prod_rec = prod.MakeRecord();
    CodaMemNode* sup_rec = prod.MakeRecord();
    CodaMemNode* geo_rec = prod.MakeRecord();
    CodaMemNode* det_rec = prod.MakeRecord();
    CodaMemNode* inp_rec = prod.MakeRecord();

    constexpr long kTimeDims[] = {1};
    constexpr double kTimeVals[] = {1000.0};
    constexpr long kScanDims[] = {kScan};
    constexpr int32_t kScanVals[] = {0};
    constexpr long kPixDims[] = {kPix};
    constexpr int32_t kPixVals[] = {0, 1};
    constexpr long kCornerDims[] = {4};
    constexpr int32_t kCornerVals[] = {0, 1, 2, 3};
    constexpr long kWaveDims[] = {3};
    constexpr float kWaveVals[] = {354.0f, 388.0f, 500.0f};

    CodaMemNode* time_node = prod.MakeDoubleArray(kTimeDims, kTimeVals);
    constexpr long kBadFillDims[] = {1};
    constexpr std::string_view kBadFill[] = {"not_a_double"};
    ASSERT_TRUE(prod.AddAttribute(
        time_node, "FillValue", prod.MakeStringArray(kBadFillDims, kBadFill)));
    ASSERT_TRUE(prod.AddField(prod_rec, "time", time_node));
    ASSERT_TRUE(prod.AddField(prod_rec, "scanline",
                              prod.MakeInt32Array(kScanDims, kScanVals)));
    ASSERT_TRUE(prod.AddField(prod_rec, "ground_pixel",
                              prod.MakeInt32Array(kPixDims, kPixVals)));
    ASSERT_TRUE(prod.AddField(prod_rec, "corner",
                              prod.MakeInt32Array(kCornerDims, kCornerVals)));
    ASSERT_TRUE(prod.AddField(prod_rec, "wavelength",
                              prod.MakeFloatArray(kWaveDims, kWaveVals)));

    // latitude with wrong FillValue type (string instead of float)
    CodaMemNode* lat_node = prod.MakeFloatArray(bad_dims, bad_f);
    ASSERT_TRUE(prod.AddField(prod_rec, "latitude", lat_node));
    CodaMemNode* lon_node =
        prod.MakeFloatArray(grid_dims, MakeSeqFloats(kScan * kPix, 1.0f, 1.0f));
    ASSERT_TRUE(prod.AddAttribute(
        lon_node, "FillValue", prod.MakeStringArray(kBadFillDims, kBadFill)));
    ASSERT_TRUE(prod.AddField(prod_rec, "longitude", lon_node));

    // String array for aerosol_type (fails read_array int32) and
    // aerosol_optical_thickness (fails read_dataset float) and qa_value.
    ASSERT_TRUE(prod.AddField(prod_rec, "aerosol_type",
                              prod.MakeStringArray(grid_dims, bad_s)));
    ASSERT_TRUE(prod.AddField(prod_rec, "qa_value",
                              prod.MakeStringArray(grid_dims, bad_s)));
    ASSERT_TRUE(prod.AddField(inp_rec, "snow_ice_flag",
                              prod.MakeStringArray(grid_dims, bad_s)));
    ASSERT_TRUE(
        prod.AddField(geo_rec, "satellite_latitude",
                      prod.MakeFloatArray(grid_dims, MakeSeqFloats(2))));
    ASSERT_TRUE(
        prod.AddField(geo_rec, "satellite_longitude",
                      prod.MakeFloatArray(grid_dims, MakeSeqFloats(2))));
    ASSERT_TRUE(
        prod.AddField(geo_rec, "satellite_altitude",
                      prod.MakeFloatArray(grid_dims, MakeSeqFloats(2))));

    ASSERT_TRUE(prod.AddField(sup_rec, "GEOLOCATIONS", geo_rec));
    ASSERT_TRUE(prod.AddField(sup_rec, "DETAILED_RESULTS", det_rec));
    ASSERT_TRUE(prod.AddField(sup_rec, "INPUT_DATA", inp_rec));
    ASSERT_TRUE(prod.AddField(prod_rec, "SUPPORT_DATA", sup_rec));
    ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", prod_rec));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_length"].read_status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["aerosol_type"].read_status);
    EXPECT_EQ(-1, res.variables["aerosol_optical_depth_validity"].read_status);
    EXPECT_EQ(-1, res.variables["snow_ice_type"].read_status);
    EXPECT_EQ(-1, res.variables["sea_ice_fraction"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_latitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_longitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_altitude"].read_status);
    EXPECT_EQ(-1, res.variables["cloud_fraction"].read_status);
  }

  // 2. Non-string @time_coverage_resolution, non-int @orbit, missing
  // delta_time, non-float read_array (HDO_S) & KD partial-dataset errors.
  {
    const harp_ingestion_module* mod = FindIngestionModule("S5P_PAL_L2_KD");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PalS5pTree tree;
    BuildCommonTree(prod, tree, "L2__KD____", kScan, kPix,
                    /*include_sensor=*/false);
    // Replace time_coverage_resolution / orbit? BuildCommonTree already added
    // them; let's test KD partial failure: provide KD_UVA, omit KD_UVAB, and
    // provide qa_value_UVA + qa_value_UVAB, omit qa_value_blue.
    const float kd_vals[] = {0.1f, 0.2f};
    const int8_t qa_vals[] = {10, 20};
    ASSERT_TRUE(prod.AddField(tree.product_rec, "KD_UVA",
                              prod.MakeFloatArray(grid_dims, kd_vals)));
    ASSERT_TRUE(prod.AddField(tree.product_rec, "qa_value_UVA",
                              prod.MakeInt8Array(grid_dims, qa_vals)));
    ASSERT_TRUE(prod.AddField(tree.product_rec, "qa_value_UVAB",
                              prod.MakeInt8Array(grid_dims, qa_vals)));
    FinalizeCommonTree(prod, tree);

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["diffuse_attenuation_coefficient"].read_status);
    EXPECT_EQ(
        -1,
        res.variables["diffuse_attenuation_coefficient_validity"].read_status);
  }

  // 3. KD_UVA + KD_UVAB present, KD_blue missing; and qa_value_UVA missing.
  {
    const harp_ingestion_module* mod = FindIngestionModule("S5P_PAL_L2_KD");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PalS5pTree tree;
    BuildCommonTree(prod, tree, "L2__KD____", kScan, kPix,
                    /*include_sensor=*/false);
    const float kd_vals[] = {0.1f, 0.2f};
    ASSERT_TRUE(prod.AddField(tree.product_rec, "KD_UVA",
                              prod.MakeFloatArray(grid_dims, kd_vals)));
    ASSERT_TRUE(prod.AddField(tree.product_rec, "KD_UVAB",
                              prod.MakeFloatArray(grid_dims, kd_vals)));
    FinalizeCommonTree(prod, tree);

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["diffuse_attenuation_coefficient"].read_status);
    EXPECT_EQ(
        -1,
        res.variables["diffuse_attenuation_coefficient_validity"].read_status);
  }

  // 4. HDO_S read_array errors (wrong count, wrong type).
  {
    const harp_ingestion_module* mod = FindIngestionModule("S5P_PAL_L2_HDO_S");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PalS5pTree tree;
    BuildCommonTree(prod, tree, "L2__HDO__S", kScan, kPix,
                    /*include_sensor=*/false);
    constexpr long kLayerDims[] = {kLayers};
    ASSERT_TRUE(
        prod.AddField(tree.product_rec, "layer",
                      prod.MakeInt32Array(kLayerDims, MakeSeqInt32(kLayers))));
    ASSERT_TRUE(prod.AddField(tree.input_rec, "altitude_levels",
                              prod.MakeFloatArray(bad_dims, bad_f)));
    const long prof_dims[] = {1, kScan, kPix, kLayers};
    const std::string_view bad_prof[] = {"a", "b", "c", "d"};
    ASSERT_TRUE(prod.AddField(tree.product_rec, "h2o_profile_apriori",
                              prod.MakeStringArray(prof_dims, bad_prof)));
    FinalizeCommonTree(prod, tree);

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["altitude_bounds"].read_status);
    EXPECT_EQ(-1,
              res.variables["h2o_column_number_density_apriori"].read_status);
    EXPECT_EQ(-1,
              res.variables["hdo_column_number_density_apriori"].read_status);
    EXPECT_EQ(-1, res.variables["h2o_column_number_density_avk"].read_status);
    EXPECT_EQ(-1, res.variables["hdo_column_number_density_avk"].read_status);
  }
}

TEST(HarpIngestPalS5pL2Test, ComplexDatasetFailurePaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  constexpr long kScan = 1;
  constexpr long kPix = 2;
  constexpr long kLayers = 2;
  const long n = kScan * kPix;
  const long grid_dims[] = {1, kScan, kPix};
  const long layer_dims[] = {kLayers};

  // Helper to test progressive failure in multi-dataset readers for SO2CBR.
  const harp_ingestion_module* so2_mod =
      FindIngestionModule("S5P_PAL_L2_SO2CBR");
  ASSERT_NE(nullptr, so2_mod);
  for (int stage = 0; stage < 6; ++stage) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PalS5pTree tree;
    BuildCommonTree(prod, tree, "L2__SO2CBR", kScan, kPix,
                    /*include_sensor=*/true, "020000");
    ASSERT_TRUE(
        prod.AddField(tree.product_rec, "layer",
                      prod.MakeInt32Array(layer_dims, MakeSeqInt32(kLayers))));
    if (stage >= 1) {
      ASSERT_TRUE(
          prod.AddField(tree.input_rec, "tm5_tropopause_layer_index",
                        prod.MakeInt32Array(grid_dims, MakeSeqInt32(n))));
      ASSERT_TRUE(
          prod.AddField(tree.input_rec, "surface_albedo_328nm",
                        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n))));
      ASSERT_TRUE(
          prod.AddField(tree.detailed_rec, "averaging_kernel",
                        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n))));
      // Note: averaging_kernel expects n*kLayers; let's pass exact size at
      // stage >= 1? Better: make a separate averaging_kernel check below.
    }
    if (stage >= 2) {
      ASSERT_TRUE(prod.AddField(
          tree.input_rec, "tm5_constant_a",
          prod.MakeDoubleArray(layer_dims, MakeSeqDoubles(kLayers))));
      ASSERT_TRUE(
          prod.AddField(tree.input_rec, "surface_albedo_376nm",
                        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n))));
      ASSERT_TRUE(
          prod.AddField(tree.product_rec, "sulfurdioxide_total_vertical_column",
                        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n))));
    }
    if (stage >= 3) {
      ASSERT_TRUE(prod.AddField(
          tree.input_rec, "tm5_constant_b",
          prod.MakeDoubleArray(layer_dims, MakeSeqDoubles(kLayers))));
      ASSERT_TRUE(prod.AddField(
          tree.input_rec, "snow_ice_flag",
          prod.MakeInt8Array(grid_dims, std::vector<int8_t>(n, 0))));
    }
    if (stage >= 4) {
      ASSERT_TRUE(prod.AddField(
          tree.detailed_rec, "sulfurdioxide_total_air_mass_factor_polluted",
          prod.MakeFloatArray(grid_dims, MakeSeqFloats(n))));
    }
    if (stage >= 5) {
      ASSERT_TRUE(
          prod.AddField(tree.detailed_rec, "selected_fitting_window_flag",
                        prod.MakeInt32Array(grid_dims, MakeSeqInt32(n))));
      ASSERT_TRUE(
          prod.AddField(tree.detailed_rec, "cloud_fraction_intensity_weighted",
                        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n))));
    }
    ASSERT_TRUE(prod.AddField(
        tree.product_rec, "qa_value",
        prod.MakeInt8Array(grid_dims, std::vector<int8_t>(n, 50))));
    FinalizeCommonTree(prod, tree);

    IngestExerciseResult res = ExerciseIngestionModule(
        so2_mod, prod.product(), "so2_column=1km;qa_filter=custom",
        /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status) << "stage=" << stage;
    EXPECT_EQ(-1, res.variables["pressure"].read_status);
    EXPECT_EQ(-1, res.variables["tropopause_pressure"].read_status);
    if (stage < 5) {
      EXPECT_EQ(-1, res.variables["surface_albedo"].read_status);
    } else {
      EXPECT_EQ(0, res.variables["surface_albedo"].read_status);
    }
    EXPECT_EQ(-1,
              res.variables["SO2_column_number_density_validity"].read_status);
    EXPECT_EQ(-1, res.variables["SO2_column_number_density_avk"].read_status);
  }

  // Missing scaling box for SO2_column_number_density_avk with so2_column=1km.
  {
    const long prof_dims[] = {1, kScan, kPix, kLayers};
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PalS5pTree tree;
    BuildCommonTree(prod, tree, "L2__SO2CBR", kScan, kPix,
                    /*include_sensor=*/true, "020000");
    ASSERT_TRUE(
        prod.AddField(tree.product_rec, "layer",
                      prod.MakeInt32Array(layer_dims, MakeSeqInt32(kLayers))));
    ASSERT_TRUE(prod.AddField(
        tree.detailed_rec, "averaging_kernel",
        prod.MakeFloatArray(prof_dims, MakeSeqFloats(n * kLayers))));
    FinalizeCommonTree(prod, tree);

    IngestExerciseResult res = ExerciseIngestionModule(
        so2_mod, prod.product(), "so2_column=1km", /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["SO2_column_number_density_avk"].read_status);
  }

  // Progressive failure in read_tcwv_pressure_bounds.
  const harp_ingestion_module* tcwv_mod =
      FindIngestionModule("S5P_PAL_L2_TCWV");
  ASSERT_NE(nullptr, tcwv_mod);
  for (int stage = 0; stage < 5; ++stage) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PalS5pTree tree;
    BuildCommonTree(prod, tree, "L2__TCWV__", kScan, kPix,
                    /*include_sensor=*/true);
    ASSERT_TRUE(
        prod.AddField(tree.product_rec, "layer",
                      prod.MakeInt32Array(layer_dims, MakeSeqInt32(kLayers))));
    if (stage >= 1) {
      ASSERT_TRUE(
          prod.AddField(tree.input_rec, "surface_pressure",
                        prod.MakeFloatArray(grid_dims, MakeSeqFloats(n))));
    }
    if (stage >= 2) {
      ASSERT_TRUE(prod.AddField(
          tree.input_rec, "pressure_constant_a_top",
          prod.MakeFloatArray(layer_dims, MakeSeqFloats(kLayers))));
    }
    if (stage >= 3) {
      ASSERT_TRUE(prod.AddField(
          tree.input_rec, "pressure_constant_a_bottom",
          prod.MakeFloatArray(layer_dims, MakeSeqFloats(kLayers))));
    }
    if (stage >= 4) {
      ASSERT_TRUE(prod.AddField(
          tree.input_rec, "pressure_constant_b_top",
          prod.MakeFloatArray(layer_dims, MakeSeqFloats(kLayers))));
    }
    FinalizeCommonTree(prod, tree);

    IngestExerciseResult res = ExerciseIngestionModule(tcwv_mod, prod.product(),
                                                       /*options_str=*/nullptr,
                                                       /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status) << "stage=" << stage;
    EXPECT_EQ(-1, res.variables["pressure_bounds"].read_status);
  }

  // Orbit and time_coverage_resolution failure variations.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("S5P_PAL", "L2__OCLO__", 1));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "id",
                                  prod.MakeString(MakePalS5pId("L2__OCLO__"))));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_resolution",
                                  prod.MakeInt32(123)));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "orbit", prod.MakeString("bad_orbit")));
    CodaMemNode* prod_rec = prod.MakeRecord();
    CodaMemNode* sup_rec = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sup_rec, "GEOLOCATIONS", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(sup_rec, "DETAILED_RESULTS", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(sup_rec, "INPUT_DATA", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(prod_rec, "SUPPORT_DATA", sup_rec));
    constexpr long d1[] = {1};
    constexpr double t[] = {100.0};
    constexpr int32_t i1[] = {0};
    constexpr long d4[] = {4};
    constexpr int32_t i4[] = {0, 1, 2, 3};
    ASSERT_TRUE(prod.AddField(prod_rec, "time", prod.MakeDoubleArray(d1, t)));
    ASSERT_TRUE(
        prod.AddField(prod_rec, "scanline", prod.MakeInt32Array(d1, i1)));
    ASSERT_TRUE(
        prod.AddField(prod_rec, "ground_pixel", prod.MakeInt32Array(d1, i1)));
    ASSERT_TRUE(prod.AddField(prod_rec, "corner", prod.MakeInt32Array(d4, i4)));
    ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", prod_rec));

    const harp_ingestion_module* oclo_mod =
        FindIngestionModule("S5P_PAL_L2_OCLO");
    ASSERT_NE(nullptr, oclo_mod);
    IngestExerciseResult res = ExerciseIngestionModule(oclo_mod, prod.product(),
                                                       /*options_str=*/nullptr,
                                                       /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_length"].read_status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
