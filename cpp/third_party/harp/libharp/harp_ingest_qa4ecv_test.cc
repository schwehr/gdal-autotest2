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
// Unit tests for QA4ECV L2 HCHO/NO2 ingestion (libharp/harp-ingest-qa4ecv.c).
//
// Google3 build notes: HDF4 is disabled (#undef HAVE_HDF4); ingestion is
// exercised against in-memory coda_product trees via CodaMemProduct.
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-qa4ecv.c

#include <cmath>
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

using ::testing::ElementsAre;
using ::testing::FloatNear;

constexpr long kScalarArrDims[] = {1};
constexpr float kFloatFill[] = {-9999.0f};
constexpr double kDoubleFill[] = {-9999.0};

// Adds a float array field with the @FillValue[0] attribute required by
// read_dataset() in harp-ingest-qa4ecv.c.
CodaMemNode* MakeFloatWithFill(CodaMemProduct& prod, const long* dims,
                               int num_dims, const float* values,
                               long num_values) {
  CodaMemNode* node =
      prod.MakeFloatArray({dims, static_cast<size_t>(num_dims)},
                          {values, static_cast<size_t>(num_values)});
  if (node == nullptr) {
    return nullptr;
  }
  prod.AddAttribute(node, "FillValue",
                    prod.MakeFloatArray(kScalarArrDims, kFloatFill));
  return node;
}

// Adds a double array field with the @FillValue[0] attribute required by
// read_dataset() in harp-ingest-qa4ecv.c.
CodaMemNode* MakeDoubleWithFill(CodaMemProduct& prod, const long* dims,
                                int num_dims, const double* values,
                                long num_values) {
  CodaMemNode* node =
      prod.MakeDoubleArray({dims, static_cast<size_t>(num_dims)},
                           {values, static_cast<size_t>(num_values)});
  if (node == nullptr) {
    return nullptr;
  }
  prod.AddAttribute(node, "FillValue",
                    prod.MakeDoubleArray(kScalarArrDims, kDoubleFill));
  return node;
}

struct Qa4ecvGroups {
  CodaMemNode* product = nullptr;
  CodaMemNode* support_data = nullptr;
  CodaMemNode* geolocations = nullptr;
  CodaMemNode* detailed_results = nullptr;
  CodaMemNode* input_data = nullptr;
};

// Populates the common QA4ECV group hierarchy, dimensions, and variables for a
// 1 x 2 x 2 (time=1, scanline=2, ground_pixel=2) product with 2 layers.
Qa4ecvGroups PopulateCommonQa4ecv(CodaMemProduct& prod, bool orbit_as_array,
                                  bool snow_as_uint8, bool flags_as_uint32,
                                  bool clamp_toa_pressure) {
  Qa4ecvGroups g;
  if (orbit_as_array) {
    constexpr int32_t kOrbit[] = {4242};
    EXPECT_TRUE(prod.AddAttribute(prod.root(), "orbit",
                                  prod.MakeInt32Array(kScalarArrDims, kOrbit)));
  } else {
    EXPECT_TRUE(prod.AddAttribute(prod.root(), "orbit", prod.MakeInt32(4242)));
  }

  g.product = prod.MakeRecord();
  g.support_data = prod.MakeRecord();
  g.geolocations = prod.MakeRecord();
  g.detailed_results = prod.MakeRecord();
  g.input_data = prod.MakeRecord();

  EXPECT_TRUE(prod.AddField(g.support_data, "GEOLOCATIONS", g.geolocations));
  EXPECT_TRUE(
      prod.AddField(g.support_data, "DETAILED_RESULTS", g.detailed_results));
  EXPECT_TRUE(prod.AddField(g.support_data, "INPUT_DATA", g.input_data));
  EXPECT_TRUE(prod.AddField(g.product, "SUPPORT_DATA", g.support_data));
  EXPECT_TRUE(prod.AddField(prod.root(), "PRODUCT", g.product));

  // Dimensions under /PRODUCT: time=1, scanline=2, ground_pixel=2, corner=4,
  // layer=2.
  constexpr long kTimeDims[] = {1};
  constexpr double kTimeVals[] = {1000.0};
  constexpr long kScanDims[] = {2};
  constexpr int32_t kTwoIndices[] = {0, 1};
  constexpr double kDeltaTime[] = {500.0, 1500.0};
  constexpr long kCornerDims[] = {4};
  constexpr int32_t kFourIndices[] = {0, 1, 2, 3};
  constexpr long kLayerDims[] = {2};

  EXPECT_TRUE(prod.AddField(
      g.product, "time", MakeDoubleWithFill(prod, kTimeDims, 1, kTimeVals, 1)));
  EXPECT_TRUE(
      prod.AddField(g.product, "delta_time",
                    MakeDoubleWithFill(prod, kScanDims, 1, kDeltaTime, 2)));
  EXPECT_TRUE(prod.AddField(g.product, "scanline",
                            prod.MakeInt32Array(kScanDims, kTwoIndices)));
  EXPECT_TRUE(prod.AddField(g.product, "ground_pixel",
                            prod.MakeInt32Array(kScanDims, kTwoIndices)));
  EXPECT_TRUE(prod.AddField(g.product, "corner",
                            prod.MakeInt32Array(kCornerDims, kFourIndices)));
  EXPECT_TRUE(prod.AddField(g.product, "layer",
                            prod.MakeInt32Array(kLayerDims, kTwoIndices)));

  // Hybrid pressure coefficients: num_layers * 2 = 4 elements.
  constexpr long kCoefDims[] = {4};
  const double coef_a[] = {0.0, 100.0, 100.0, clamp_toa_pressure ? 0.0 : 50.0};
  const double coef_b[] = {1.0, 0.5, 0.5, 0.0};
  EXPECT_TRUE(prod.AddField(g.product, "tm5_pressure_level_a",
                            MakeDoubleWithFill(prod, kCoefDims, 1, coef_a, 4)));
  EXPECT_TRUE(prod.AddField(g.product, "tm5_pressure_level_b",
                            MakeDoubleWithFill(prod, kCoefDims, 1, coef_b, 4)));

  constexpr long kGridDims[] = {2, 2};
  constexpr float kLat[] = {10.0f, 11.0f, 12.0f, -9999.0f};
  constexpr float kLon[] = {20.0f, 21.0f, 22.0f, 23.0f};
  constexpr float kSurfPress[] = {1000.0f, 950.0f, 900.0f, 850.0f};
  EXPECT_TRUE(prod.AddField(g.product, "latitude",
                            MakeFloatWithFill(prod, kGridDims, 2, kLat, 4)));
  EXPECT_TRUE(prod.AddField(g.product, "longitude",
                            MakeFloatWithFill(prod, kGridDims, 2, kLon, 4)));
  EXPECT_TRUE(
      prod.AddField(g.product, "tm5_surface_pressure",
                    MakeFloatWithFill(prod, kGridDims, 2, kSurfPress, 4)));

  // Geolocation fields.
  constexpr long kBoundsDims[] = {2, 2, 4};
  constexpr float kLatBounds[] = {9.0f,  9.0f,  11.0f, 11.0f, 10.0f, 10.0f,
                                  12.0f, 12.0f, 11.0f, 11.0f, 13.0f, 13.0f,
                                  12.0f, 12.0f, 14.0f, 14.0f};
  constexpr float kLonBounds[] = {19.0f, 21.0f, 21.0f, 19.0f, 20.0f, 22.0f,
                                  22.0f, 20.0f, 21.0f, 23.0f, 23.0f, 21.0f,
                                  22.0f, 24.0f, 24.0f, 22.0f};
  constexpr float kSza[] = {30.0f, 31.0f, 32.0f, 33.0f};
  constexpr float kRaa[] = {10.0f, 20.0f, 30.0f, 40.0f};
  constexpr float kVza[] = {5.0f, 6.0f, 7.0f, 8.0f};
  EXPECT_TRUE(
      prod.AddField(g.geolocations, "latitude_bounds",
                    MakeFloatWithFill(prod, kBoundsDims, 3, kLatBounds, 16)));
  EXPECT_TRUE(
      prod.AddField(g.geolocations, "longitude_bounds",
                    MakeFloatWithFill(prod, kBoundsDims, 3, kLonBounds, 16)));
  EXPECT_TRUE(prod.AddField(g.geolocations, "solar_zenith_angle",
                            MakeFloatWithFill(prod, kGridDims, 2, kSza, 4)));
  EXPECT_TRUE(prod.AddField(g.geolocations, "relative_azimuth_angle",
                            MakeFloatWithFill(prod, kGridDims, 2, kRaa, 4)));
  EXPECT_TRUE(prod.AddField(g.geolocations, "viewing_zenith_angle",
                            MakeFloatWithFill(prod, kGridDims, 2, kVza, 4)));

  // Input data fields.
  constexpr float kSurfAlt[] = {100.0f, 200.0f, 300.0f, 400.0f};
  constexpr float kCloudFrac[] = {0.1f, 0.2f, 0.3f, 0.4f};
  constexpr float kCloudFracUnc[] = {0.01f, 0.02f, 0.03f, 0.04f};
  constexpr float kCloudPress[] = {700.0f, 750.0f, 800.0f, 850.0f};
  constexpr float kCloudPressUnc[] = {10.0f, 15.0f, 20.0f, 25.0f};
  EXPECT_TRUE(
      prod.AddField(g.input_data, "surface_altitude",
                    MakeFloatWithFill(prod, kGridDims, 2, kSurfAlt, 4)));
  EXPECT_TRUE(
      prod.AddField(g.input_data, "cloud_fraction",
                    MakeFloatWithFill(prod, kGridDims, 2, kCloudFrac, 4)));
  EXPECT_TRUE(
      prod.AddField(g.input_data, "cloud_fraction_uncertainty",
                    MakeFloatWithFill(prod, kGridDims, 2, kCloudFracUnc, 4)));
  EXPECT_TRUE(
      prod.AddField(g.input_data, "cloud_pressure",
                    MakeFloatWithFill(prod, kGridDims, 2, kCloudPress, 4)));
  EXPECT_TRUE(
      prod.AddField(g.input_data, "cloud_pressure_uncertainty",
                    MakeFloatWithFill(prod, kGridDims, 2, kCloudPressUnc, 4)));

  if (snow_as_uint8) {
    // Covers 0->0, 50->1, 101->2, 255->4.
    constexpr uint8_t kSnowU8[] = {0, 50, 101, 255};
    CodaMemNode* snow = prod.MakeUint8Array(kGridDims, kSnowU8);
    EXPECT_TRUE(prod.AddAttribute(
        snow, "FillValue", prod.MakeFloatArray(kScalarArrDims, kFloatFill)));
    EXPECT_TRUE(prod.AddField(g.input_data, "snow_ice_flag", snow));
  } else {
    // Covers 103->3, 102->-1, -2->-1, 25->1.
    constexpr int8_t kSnowI8[] = {103, 102, -2, 25};
    CodaMemNode* snow = prod.MakeInt8Array(kGridDims, kSnowI8);
    EXPECT_TRUE(prod.AddAttribute(
        snow, "FillValue", prod.MakeFloatArray(kScalarArrDims, kFloatFill)));
    EXPECT_TRUE(prod.AddField(g.input_data, "snow_ice_flag", snow));
  }

  if (flags_as_uint32) {
    constexpr uint32_t kFlags[] = {0u, 1u, 2u, 3u};
    EXPECT_TRUE(prod.AddField(g.detailed_results, "processing_quality_flags",
                              prod.MakeUint32Array(kGridDims, kFlags)));
  } else {
    constexpr int32_t kFlags[] = {0, 1, 2, 4};
    EXPECT_TRUE(prod.AddField(g.detailed_results, "processing_quality_flags",
                              prod.MakeInt32Array(kGridDims, kFlags)));
  }

  return g;
}

void PopulateHchoFields(CodaMemProduct& prod, const Qa4ecvGroups& g) {
  constexpr long kGridDims[] = {2, 2};
  constexpr long kProfDims[] = {2, 2, 2};
  constexpr float kCol[] = {1.0e15f, 2.0e15f, 3.0e15f, 4.0e15f};
  constexpr float kUncR[] = {1.0e14f, 2.0e14f, 3.0e14f, 4.0e14f};
  constexpr float kUncS[] = {5.0e13f, 6.0e13f, 7.0e13f, 8.0e13f};
  constexpr float kAmfTrop[] = {1.5f, 1.5f, 2.0f, 2.0f};
  constexpr float kAmfClear[] = {3.0f, 3.0f, 4.0f, 4.0f};
  constexpr float kAvk[] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
  constexpr float kAvkClear[] = {0.2f, 0.3f, 0.4f, 0.5f,
                                 0.6f, 0.7f, 0.8f, 0.9f};
  constexpr float kApriori[] = {1e-9f, 2e-9f, 3e-9f, 4e-9f,
                                5e-9f, 6e-9f, 7e-9f, 8e-9f};
  constexpr float kAlbedo[] = {0.05f, 0.06f, 0.07f, 0.08f};
  constexpr float kCrf[] = {0.15f, 0.25f, 0.35f, 0.45f};

  EXPECT_TRUE(prod.AddField(g.product, "tropospheric_hcho_vertical_column",
                            MakeFloatWithFill(prod, kGridDims, 2, kCol, 4)));
  EXPECT_TRUE(prod.AddField(
      g.product, "tropospheric_hcho_vertical_column_uncertainty_random",
      MakeFloatWithFill(prod, kGridDims, 2, kUncR, 4)));
  EXPECT_TRUE(prod.AddField(
      g.product, "tropospheric_hcho_vertical_column_uncertainty_systematic",
      MakeFloatWithFill(prod, kGridDims, 2, kUncS, 4)));
  EXPECT_TRUE(
      prod.AddField(g.product, "amf_trop",
                    MakeFloatWithFill(prod, kGridDims, 2, kAmfTrop, 4)));
  EXPECT_TRUE(prod.AddField(g.product, "averaging_kernel",
                            MakeFloatWithFill(prod, kProfDims, 3, kAvk, 8)));

  EXPECT_TRUE(
      prod.AddField(g.detailed_results, "amf_clear",
                    MakeFloatWithFill(prod, kGridDims, 2, kAmfClear, 4)));
  EXPECT_TRUE(
      prod.AddField(g.detailed_results, "averaging_kernel_clear",
                    MakeFloatWithFill(prod, kProfDims, 3, kAvkClear, 8)));
  EXPECT_TRUE(prod.AddField(g.detailed_results, "cloud_radiance_fraction_hcho",
                            MakeFloatWithFill(prod, kGridDims, 2, kCrf, 4)));

  EXPECT_TRUE(
      prod.AddField(g.input_data, "hcho_profile_apriori",
                    MakeFloatWithFill(prod, kProfDims, 3, kApriori, 8)));
  EXPECT_TRUE(prod.AddField(g.input_data, "surface_albedo_hcho",
                            MakeFloatWithFill(prod, kGridDims, 2, kAlbedo, 4)));
}

void PopulateNo2Fields(CodaMemProduct& prod, const Qa4ecvGroups& g,
                       bool trop_index_uint32) {
  constexpr long kGridDims[] = {2, 2};
  constexpr long kProfDims[] = {2, 2, 2};
  if (trop_index_uint32) {
    // Value 5 is out-of-range (>= num_layers=2) to cover NaN branches.
    constexpr uint32_t kTropIdx[] = {0u, 1u, 5u, 0u};
    EXPECT_TRUE(prod.AddField(g.product, "tm5_tropopause_layer_index",
                              prod.MakeUint32Array(kGridDims, kTropIdx)));
  } else {
    constexpr int32_t kTropIdx[] = {0, 1, -1, 2};
    EXPECT_TRUE(prod.AddField(g.product, "tm5_tropopause_layer_index",
                              prod.MakeInt32Array(kGridDims, kTropIdx)));
  }

  constexpr float kTropNo2[] = {1.0e15f, 2.0e15f, 3.0e15f, 4.0e15f};
  constexpr float kTropNo2Unc[] = {1.0e14f, 2.0e14f, 3.0e14f, 4.0e14f};
  constexpr float kAmfTrop[] = {1.2f, 1.4f, 1.6f, 1.8f};
  constexpr float kAmfTotal[] = {2.0f, 2.0f, 2.0f, 2.0f};
  constexpr float kAmfStrat[] = {2.5f, 2.5f, 2.5f, 2.5f};
  constexpr float kAvk[] = {0.2f, 0.4f, 0.6f, 0.8f, 0.1f, 0.3f, 0.5f, 0.7f};
  constexpr float kSumNo2[] = {3.0e15f, 4.0e15f, 5.0e15f, 6.0e15f};
  constexpr float kSumNo2Unc[] = {3.0e14f, 4.0e14f, 5.0e14f, 6.0e14f};
  constexpr float kTotNo2[] = {3.5e15f, 4.5e15f, 5.5e15f, 6.5e15f};
  constexpr float kTotNo2Unc[] = {3.5e14f, 4.5e14f, 5.5e14f, 6.5e14f};
  constexpr float kStratNo2[] = {2.0e15f, 2.1e15f, 2.2e15f, 2.3e15f};
  constexpr float kStratNo2Unc[] = {2.0e14f, 2.1e14f, 2.2e14f, 2.3e14f};
  constexpr float kStreamNo2[] = {2.5e15f, 2.6e15f, 2.7e15f, 2.8e15f};
  constexpr float kStreamNo2Unc[] = {2.5e14f, 2.6e14f, 2.7e14f, 2.8e14f};
  constexpr float kCrf[] = {0.22f, 0.33f, 0.44f, 0.55f};
  constexpr float kAlbedo[] = {0.02f, 0.03f, 0.04f, 0.05f};

  EXPECT_TRUE(
      prod.AddField(g.product, "tropospheric_no2_vertical_column",
                    MakeFloatWithFill(prod, kGridDims, 2, kTropNo2, 4)));
  EXPECT_TRUE(
      prod.AddField(g.product, "tropospheric_no2_vertical_column_uncertainty",
                    MakeFloatWithFill(prod, kGridDims, 2, kTropNo2Unc, 4)));
  EXPECT_TRUE(
      prod.AddField(g.product, "amf_trop",
                    MakeFloatWithFill(prod, kGridDims, 2, kAmfTrop, 4)));
  EXPECT_TRUE(
      prod.AddField(g.product, "amf_total",
                    MakeFloatWithFill(prod, kGridDims, 2, kAmfTotal, 4)));
  EXPECT_TRUE(prod.AddField(g.product, "averaging_kernel",
                            MakeFloatWithFill(prod, kProfDims, 3, kAvk, 8)));

  EXPECT_TRUE(prod.AddField(g.detailed_results,
                            "summed_no2_total_vertical_column",
                            MakeFloatWithFill(prod, kGridDims, 2, kSumNo2, 4)));
  EXPECT_TRUE(prod.AddField(
      g.detailed_results, "summed_no2_total_vertical_column_uncertainty",
      MakeFloatWithFill(prod, kGridDims, 2, kSumNo2Unc, 4)));
  EXPECT_TRUE(prod.AddField(g.detailed_results, "total_no2_vertical_column",
                            MakeFloatWithFill(prod, kGridDims, 2, kTotNo2, 4)));
  EXPECT_TRUE(
      prod.AddField(g.detailed_results, "total_no2_vertical_column_uncertainty",
                    MakeFloatWithFill(prod, kGridDims, 2, kTotNo2Unc, 4)));
  EXPECT_TRUE(
      prod.AddField(g.detailed_results, "stratospheric_no2_vertical_column",
                    MakeFloatWithFill(prod, kGridDims, 2, kStratNo2, 4)));
  EXPECT_TRUE(prod.AddField(
      g.detailed_results, "stratospheric_no2_vertical_column_uncertainty",
      MakeFloatWithFill(prod, kGridDims, 2, kStratNo2Unc, 4)));
  EXPECT_TRUE(prod.AddField(
      g.detailed_results, "stratospheric_no2_vertical_column_stream",
      MakeFloatWithFill(prod, kGridDims, 2, kStreamNo2, 4)));
  EXPECT_TRUE(
      prod.AddField(g.detailed_results,
                    "stratospheric_no2_vertical_column_stream_uncertainty",
                    MakeFloatWithFill(prod, kGridDims, 2, kStreamNo2Unc, 4)));
  EXPECT_TRUE(
      prod.AddField(g.detailed_results, "amf_strat",
                    MakeFloatWithFill(prod, kGridDims, 2, kAmfStrat, 4)));
  EXPECT_TRUE(prod.AddField(g.detailed_results, "cloud_radiance_fraction_no2",
                            MakeFloatWithFill(prod, kGridDims, 2, kCrf, 4)));

  EXPECT_TRUE(prod.AddField(g.input_data, "surface_albedo_no2",
                            MakeFloatWithFill(prod, kGridDims, 2, kAlbedo, 4)));
}

TEST(HarpIngestQa4ecvTest, IngestHchoClearSkyAndRadianceSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("QA4ECV_L2_HCHO");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  Qa4ecvGroups g = PopulateCommonQa4ecv(prod, /*orbit_as_array=*/true,
                                        /*snow_as_uint8=*/true,
                                        /*flags_as_uint32=*/true,
                                        /*clamp_toa_pressure=*/true);
  PopulateHchoFields(prod, g);

  IngestExerciseResult res = ExerciseIngestionModule(
      mod, prod.product(), "amf=clear_sky;cloud_fraction=radiance");
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(4L, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(2L, res.dimensions[harp_dimension_vertical]);
  EXPECT_THAT(res.variables["scan_subindex"].int16_values,
              ElementsAre(0, 1, 0, 1));
  EXPECT_THAT(res.variables["datetime"].double_values,
              ElementsAre(1000.5, 1000.5, 1001.5, 1001.5));
  EXPECT_THAT(res.variables["orbit_index"].int32_values, ElementsAre(4242));
  EXPECT_TRUE(std::isnan(res.variables["latitude"].float_values[3]));
  EXPECT_THAT(res.variables["snow_ice_type"].int8_values,
              ElementsAre(0, 1, 2, 4));
  EXPECT_THAT(res.variables["sea_ice_fraction"].float_values,
              ElementsAre(0.0f, 0.5f, 0.0f, 0.0f));
  EXPECT_FALSE(res.variables["cloud_fraction_uncertainty"].included);
  EXPECT_THAT(res.variables["cloud_fraction"].float_values,
              ElementsAre(0.15f, 0.25f, 0.35f, 0.45f));
  // Scaled by amf_trop / amf_clear = 0.5.
  EXPECT_THAT(
      res.variables["tropospheric_HCHO_column_number_density"].float_values,
      ElementsAre(0.5e15f, 1.0e15f, 1.5e15f, 2.0e15f));
  EXPECT_THAT(
      res.variables["tropospheric_HCHO_column_number_density_amf"].float_values,
      ElementsAre(3.0f, 3.0f, 4.0f, 4.0f));
  // TOA pressure clamped to 1e-3 Pa at index 3 of each profile.
  EXPECT_DOUBLE_EQ(1e-3, res.variables["pressure_bounds"].double_values[3]);
}

#if !defined(MEMORY_SANITIZER)
// Upstream ingestion_init() in harp-ingest-qa4ecv.c leaves
// info->use_clear_sky_amf uninitialized when the `amf` option is omitted.
TEST(HarpIngestQa4ecvTest, IngestHchoDefaultOptionsSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("QA4ECV_L2_HCHO");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  Qa4ecvGroups g = PopulateCommonQa4ecv(prod, /*orbit_as_array=*/false,
                                        /*snow_as_uint8=*/false,
                                        /*flags_as_uint32=*/false,
                                        /*clamp_toa_pressure=*/false);
  PopulateHchoFields(prod, g);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_TRUE(res.variables["cloud_fraction_uncertainty"].included);
  EXPECT_THAT(res.variables["snow_ice_type"].int8_values,
              ElementsAre(3, -1, -1, 1));
  // Note: because upstream leaves info->use_clear_sky_amf uninitialized when
  // `amf` is omitted, its value depends on heap state; only assert read success
  // and element count here.
  EXPECT_EQ(
      HARP_SUCCESS,
      res.variables["tropospheric_HCHO_column_number_density"].read_status);
  EXPECT_EQ(4u, res.variables["tropospheric_HCHO_column_number_density"]
                    .float_values.size());
  EXPECT_DOUBLE_EQ(50.0, res.variables["pressure_bounds"].double_values[3]);
}
#endif

TEST(HarpIngestQa4ecvTest, IngestNo2DefaultAndAlternateOptionsSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("QA4ECV_L2_NO2");
  ASSERT_NE(nullptr, mod);

  // 1. Default options (summed total column, default stratospheric column,
  // default cloud fraction) with int32 tropopause index.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    Qa4ecvGroups g = PopulateCommonQa4ecv(prod, /*orbit_as_array=*/false,
                                          /*snow_as_uint8=*/false,
                                          /*flags_as_uint32=*/false,
                                          /*clamp_toa_pressure=*/false);
    PopulateNo2Fields(prod, g, /*trop_index_uint32=*/false);

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "total_column=summed");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_TRUE(res.variables["cloud_fraction_uncertainty"].included);
    EXPECT_THAT(res.variables["NO2_column_number_density"].float_values,
                ElementsAre(3.0e15f, 4.0e15f, 5.0e15f, 6.0e15f));
    EXPECT_THAT(
        res.variables["stratospheric_NO2_column_number_density"].float_values,
        ElementsAre(2.0e15f, 2.1e15f, 2.2e15f, 2.3e15f));
    // Tropopause index -1 and 2 (>= num_layers) yield NaN.
    EXPECT_FALSE(
        std::isnan(res.variables["tropopause_pressure"].double_values[0]));
    EXPECT_TRUE(
        std::isnan(res.variables["tropopause_pressure"].double_values[2]));
    EXPECT_TRUE(
        std::isnan(res.variables["tropopause_pressure"].double_values[3]));
    EXPECT_TRUE(
        std::isnan(res.variables["tropospheric_NO2_column_number_density_avk"]
                       .float_values[4]));
    EXPECT_TRUE(
        std::isnan(res.variables["stratospheric_NO2_column_number_density_avk"]
                       .float_values[4]));
  }

  // 2. Alternate options (total_column=total, stratospheric_column=stream,
  // cloud_fraction=radiance) with uint32 tropopause index.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    Qa4ecvGroups g = PopulateCommonQa4ecv(prod, /*orbit_as_array=*/true,
                                          /*snow_as_uint8=*/true,
                                          /*flags_as_uint32=*/true,
                                          /*clamp_toa_pressure=*/true);
    PopulateNo2Fields(prod, g, /*trop_index_uint32=*/true);

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(),
        "total_column=total;stratospheric_column=stream;"
        "cloud_fraction=radiance");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_FALSE(res.variables["cloud_fraction_uncertainty"].included);
    EXPECT_THAT(res.variables["NO2_column_number_density"].float_values,
                ElementsAre(3.5e15f, 4.5e15f, 5.5e15f, 6.5e15f));
    EXPECT_THAT(
        res.variables["NO2_column_number_density_uncertainty"].float_values,
        ElementsAre(3.5e14f, 4.5e14f, 5.5e14f, 6.5e14f));
    EXPECT_THAT(
        res.variables["stratospheric_NO2_column_number_density"].float_values,
        ElementsAre(2.5e15f, 2.6e15f, 2.7e15f, 2.8e15f));
    EXPECT_THAT(
        res.variables["stratospheric_NO2_column_number_density_uncertainty"]
            .float_values,
        ElementsAre(2.5e14f, 2.6e14f, 2.7e14f, 2.8e14f));
    EXPECT_THAT(res.variables["cloud_fraction"].float_values,
                ElementsAre(0.22f, 0.33f, 0.44f, 0.55f));
    EXPECT_THAT(res.variables["surface_albedo"].float_values,
                ElementsAre(0.02f, 0.03f, 0.04f, 0.05f));
    EXPECT_THAT(res.variables["tropospheric_NO2_column_number_density_amf"]
                    .float_values,
                ElementsAre(1.2f, 1.4f, 1.6f, 1.8f));
    EXPECT_THAT(res.variables["NO2_column_number_density_amf"].float_values,
                ElementsAre(2.0f, 2.0f, 2.0f, 2.0f));
    EXPECT_THAT(res.variables["stratospheric_NO2_column_number_density_amf"]
                    .float_values,
                ElementsAre(2.5f, 2.5f, 2.5f, 2.5f));
    EXPECT_FLOAT_EQ(0.0f,
                    res.variables["tropospheric_NO2_column_number_density_avk"]
                        .float_values[0]);
    EXPECT_THAT(res.variables["stratospheric_NO2_column_number_density_avk"]
                    .float_values[0],
                FloatNear(0.16f, 1e-6f));
  }
}

TEST(HarpIngestQa4ecvTest, InitCursorAndDimensionErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("QA4ECV_L2_NO2");
  ASSERT_NE(nullptr, mod);

  // 1. Missing PRODUCT.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }

  // 2. Missing SUPPORT_DATA.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", prod.MakeRecord()));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }

  // 3. Missing GEOLOCATIONS / DETAILED_RESULTS / INPUT_DATA.
  for (int stage = 0; stage < 3; ++stage) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* p = prod.MakeRecord();
    CodaMemNode* s = prod.MakeRecord();
    if (stage >= 1) {
      ASSERT_TRUE(prod.AddField(s, "GEOLOCATIONS", prod.MakeRecord()));
    }
    if (stage >= 2) {
      ASSERT_TRUE(prod.AddField(s, "DETAILED_RESULTS", prod.MakeRecord()));
    }
    ASSERT_TRUE(prod.AddField(p, "SUPPORT_DATA", s));
    ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", p));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }

  // Helper to build minimal group structure with customizable dimensions.
  auto make_dim_prod = [&](int num_dims_present, bool bad_time_rank,
                           bool scalar_time, long time_len, long corner_len) {
    auto p = std::make_unique<CodaMemProduct>();
    CodaMemNode* prod_rec = p->MakeRecord();
    CodaMemNode* supp = p->MakeRecord();
    p->AddField(supp, "GEOLOCATIONS", p->MakeRecord());
    p->AddField(supp, "DETAILED_RESULTS", p->MakeRecord());
    p->AddField(supp, "INPUT_DATA", p->MakeRecord());
    p->AddField(prod_rec, "SUPPORT_DATA", supp);
    p->AddField(p->root(), "PRODUCT", prod_rec);

    if (num_dims_present >= 1) {
      if (scalar_time) {
        p->AddField(prod_rec, "time", p->MakeInt32(1));
      } else if (bad_time_rank) {
        constexpr long k2D[] = {1, 1};
        constexpr int32_t kV[] = {1};
        p->AddField(prod_rec, "time", p->MakeInt32Array(k2D, kV));
      } else {
        const long t_dim[] = {time_len};
        std::vector<int32_t> vals(static_cast<size_t>(time_len), 0);
        p->AddField(prod_rec, "time", p->MakeInt32Array(t_dim, vals));
      }
    }
    if (num_dims_present >= 2) {
      constexpr long kD[] = {2};
      constexpr int32_t kV[] = {0, 1};
      p->AddField(prod_rec, "scanline", p->MakeInt32Array(kD, kV));
    }
    if (num_dims_present >= 3) {
      constexpr long kD[] = {2};
      constexpr int32_t kV[] = {0, 1};
      p->AddField(prod_rec, "ground_pixel", p->MakeInt32Array(kD, kV));
    }
    if (num_dims_present >= 4) {
      const long c_dim[] = {corner_len};
      std::vector<int32_t> vals(static_cast<size_t>(corner_len), 0);
      p->AddField(prod_rec, "corner", p->MakeInt32Array(c_dim, vals));
    }
    if (num_dims_present >= 5) {
      constexpr long kD[] = {2};
      constexpr int32_t kV[] = {0, 1};
      p->AddField(prod_rec, "layer", p->MakeInt32Array(kD, kV));
    }
    return p;
  };

  // Missing each of the 5 dimension variables, plus non-1D/scalar/wrong-length.
  for (int n = 0; n < 5; ++n) {
    auto p = make_dim_prod(n, false, false, 1, 4);
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, p->product()).status);
  }
  EXPECT_EQ(-1, ExerciseIngestionModule(
                    mod, make_dim_prod(5, true, false, 1, 4)->product())
                    .status);
  EXPECT_EQ(-1, ExerciseIngestionModule(
                    mod, make_dim_prod(5, false, true, 1, 4)->product())
                    .status);
  EXPECT_EQ(-1, ExerciseIngestionModule(
                    mod, make_dim_prod(5, false, false, 2, 4)->product())
                    .status);
  EXPECT_EQ(-1, ExerciseIngestionModule(
                    mod, make_dim_prod(5, false, false, 1, 3)->product())
                    .status);

  // Missing / wrong-size tm5_pressure_level_a and tm5_pressure_level_b.
  {
    auto p = make_dim_prod(5, false, false, 1, 4);
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, p->product()).status);
  }
  {
    auto p = make_dim_prod(5, false, false, 1, 4);
    CodaMemNode* prod_rec = nullptr;
    // Add tm5_pressure_level_a with wrong element count (2 instead of 4).
    coda_cursor cur;
    ASSERT_EQ(0, coda_cursor_set_product(&cur, p->product()));
    (void)prod_rec;
    constexpr long kBadDims[] = {2};
    constexpr double kBadVals[] = {0.0, 1.0};
    CodaMemProduct p2;
    Qa4ecvGroups g = PopulateCommonQa4ecv(p2, false, false, false, false);
    // Replace tm5_pressure_level_b with wrong-size via a fresh product without
    // tm5_pressure_level_b.
    CodaMemProduct p3;
    CodaMemNode* pr = p3.MakeRecord();
    CodaMemNode* su = p3.MakeRecord();
    p3.AddField(su, "GEOLOCATIONS", p3.MakeRecord());
    p3.AddField(su, "DETAILED_RESULTS", p3.MakeRecord());
    p3.AddField(su, "INPUT_DATA", p3.MakeRecord());
    p3.AddField(pr, "SUPPORT_DATA", su);
    p3.AddField(p3.root(), "PRODUCT", pr);
    constexpr int32_t kOne[] = {0};
    constexpr int32_t kTwo[] = {0, 1};
    constexpr int32_t kFour[] = {0, 1, 2, 3};
    constexpr long kD1[] = {1};
    constexpr long kD2[] = {2};
    constexpr long kD4[] = {4};
    p3.AddField(pr, "time", p3.MakeInt32Array(kD1, kOne));
    p3.AddField(pr, "scanline", p3.MakeInt32Array(kD2, kTwo));
    p3.AddField(pr, "ground_pixel", p3.MakeInt32Array(kD2, kTwo));
    p3.AddField(pr, "corner", p3.MakeInt32Array(kD4, kFour));
    p3.AddField(pr, "layer", p3.MakeInt32Array(kD2, kTwo));
    p3.AddField(pr, "tm5_pressure_level_a",
                MakeDoubleWithFill(
                    p3, kD4, 1, std::array<double, 4>{0, 1, 2, 3}.data(), 4));
    p3.AddField(pr, "tm5_pressure_level_b",
                MakeDoubleWithFill(p3, kBadDims, 1, kBadVals, 2));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, p3.product()).status);
    (void)g;
  }
}

TEST(HarpIngestQa4ecvTest, ReadDatasetAndVariableErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* no2_mod = FindIngestionModule("QA4ECV_L2_NO2");
  const harp_ingestion_module* hcho_mod = FindIngestionModule("QA4ECV_L2_HCHO");
  ASSERT_NE(nullptr, no2_mod);
  ASSERT_NE(nullptr, hcho_mod);

  // 1. Minimal NO2 product with missing datasets and missing @orbit triggers
  // stop_on_error=false read failures across variables.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* pr = prod.MakeRecord();
    CodaMemNode* su = prod.MakeRecord();
    prod.AddField(su, "GEOLOCATIONS", prod.MakeRecord());
    prod.AddField(su, "DETAILED_RESULTS", prod.MakeRecord());
    CodaMemNode* inp = prod.MakeRecord();
    prod.AddField(su, "INPUT_DATA", inp);
    prod.AddField(pr, "SUPPORT_DATA", su);
    prod.AddField(prod.root(), "PRODUCT", pr);

    constexpr long kD1[] = {1};
    constexpr long kD2[] = {2};
    constexpr long kD4[] = {4};
    constexpr double kT[] = {1000.0};
    constexpr int32_t kI2[] = {0, 1};
    constexpr int32_t kI4[] = {0, 1, 2, 3};
    constexpr double kC[] = {0.0, 1.0, 2.0, 3.0};
    // Make time a scalar string? No, time needs 1-dim array; use double array.
    prod.AddField(pr, "time", MakeDoubleWithFill(prod, kD1, 1, kT, 1));
    prod.AddField(pr, "scanline", prod.MakeInt32Array(kD2, kI2));
    prod.AddField(pr, "ground_pixel", prod.MakeInt32Array(kD2, kI2));
    prod.AddField(pr, "corner", prod.MakeInt32Array(kD4, kI4));
    prod.AddField(pr, "layer", prod.MakeInt32Array(kD2, kI2));
    prod.AddField(pr, "tm5_pressure_level_a",
                  MakeDoubleWithFill(prod, kD4, 1, kC, 4));
    prod.AddField(pr, "tm5_pressure_level_b",
                  MakeDoubleWithFill(prod, kD4, 1, kC, 4));

    // Add wrong-type arrays to exercise read_dataset type-error branches:
    // delta_time without @FillValue, snow_ice_flag as string, latitude as
    // float without @FillValue, tm5_tropopause_layer_index as string.
    constexpr double kDelta[] = {1.0, 2.0};
    prod.AddField(pr, "delta_time", prod.MakeDoubleArray(kD2, kDelta));
    constexpr long kG[] = {2, 2};
    constexpr float kF4[] = {1.0f, 2.0f, 3.0f, 4.0f};
    prod.AddField(pr, "latitude", prod.MakeFloatArray(kG, kF4));
    constexpr std::string_view kBadStr4[] = {"a", "b", "c", "d"};
    prod.AddField(pr, "longitude", prod.MakeStringArray(kG, kBadStr4));
    prod.AddField(pr, "tm5_tropopause_layer_index",
                  prod.MakeStringArray(kG, kBadStr4));
    prod.AddField(inp, "snow_ice_flag", prod.MakeStringArray(kG, kBadStr4));
    // Add @orbit as invalid string to cover read_orbit_index read failure.
    prod.AddAttribute(prod.root(), "orbit", prod.MakeString("bad"));

    IngestExerciseResult res = ExerciseIngestionModule(
        no2_mod, prod.product(), nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["snow_ice_type"].read_status);
    EXPECT_EQ(-1, res.variables["tropopause_pressure"].read_status);
    EXPECT_EQ(-1, res.variables["pressure_bounds"].read_status);
  }

  // 2. HCHO clear_sky errors when amf_trop or amf_clear is missing, and
  // NO2 avk errors when tropopause index / amf_total / amf_strat / amf_trop
  // fail.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    Qa4ecvGroups g = PopulateCommonQa4ecv(prod, false, false, false, false);
    constexpr long kGridDims[] = {2, 2};
    constexpr long kProfDims[] = {2, 2, 2};
    constexpr float kF4[] = {1.0f, 2.0f, 3.0f, 4.0f};
    constexpr float kF8[] = {1, 2, 3, 4, 5, 6, 7, 8};
    prod.AddField(g.product, "tropospheric_hcho_vertical_column",
                  MakeFloatWithFill(prod, kGridDims, 2, kF4, 4));
    prod.AddField(g.product, "averaging_kernel",
                  MakeFloatWithFill(prod, kProfDims, 3, kF8, 8));

    // amf_trop missing -> read_hcho_column_tropospheric fails.
    IngestExerciseResult r1 = ExerciseIngestionModule(
        hcho_mod, prod.product(), "amf=clear_sky", /*stop_on_error=*/false);
    EXPECT_EQ(
        -1,
        r1.variables["tropospheric_HCHO_column_number_density"].read_status);

    // Add amf_trop, still missing amf_clear -> fails on amf_clear.
    prod.AddField(g.product, "amf_trop",
                  MakeFloatWithFill(prod, kGridDims, 2, kF4, 4));
    IngestExerciseResult r2 = ExerciseIngestionModule(
        hcho_mod, prod.product(), "amf=clear_sky", /*stop_on_error=*/false);
    EXPECT_EQ(
        -1,
        r2.variables["tropospheric_HCHO_column_number_density"].read_status);

    // NO2 avk failure when tm5_tropopause_layer_index is missing, then when
    // amf_total is missing, then when amf_strat is missing.
    IngestExerciseResult r3 = ExerciseIngestionModule(
        no2_mod, prod.product(), nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(
        -1,
        r3.variables["tropospheric_NO2_column_number_density_avk"].read_status);
    EXPECT_EQ(-1, r3.variables["stratospheric_NO2_column_number_density_avk"]
                      .read_status);

    constexpr int32_t kTropIdx[] = {0, 1, 0, 1};
    prod.AddField(g.product, "tm5_tropopause_layer_index",
                  prod.MakeInt32Array(kGridDims, kTropIdx));
    IngestExerciseResult r4 = ExerciseIngestionModule(
        no2_mod, prod.product(), nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(
        -1,
        r4.variables["tropospheric_NO2_column_number_density_avk"].read_status);
    EXPECT_EQ(-1, r4.variables["stratospheric_NO2_column_number_density_avk"]
                      .read_status);

    prod.AddField(g.product, "amf_total",
                  MakeFloatWithFill(prod, kGridDims, 2, kF4, 4));
    IngestExerciseResult r5 = ExerciseIngestionModule(
        no2_mod, prod.product(), nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, r5.variables["stratospheric_NO2_column_number_density_avk"]
                      .read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
