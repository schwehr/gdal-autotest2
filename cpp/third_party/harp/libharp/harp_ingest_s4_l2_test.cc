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
// Unit tests for Sentinel-4 L2 ingestion modules (libharp/harp-ingest-s4_l2.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-s4_l2.c

#include <cmath>
#include <cstdint>
#include <span>
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

using ::testing::ElementsAre;

enum class LayerLocation {
  kNone,
  kProduct,
  kSupportData,
  kSubcolumn,
};

constexpr float kFloatFill = -999.0f;
constexpr double kDoubleFill = -999.0;

// Attaches `product_type` (which in Sentinel-4 contains hyphens such as
// "UVN-2-ALH") to `prod`.
bool SetS4ProductInfo(CodaMemProduct& prod, std::string_view product_type) {
  return prod.SetProductInfo("Sentinel4", product_type, 0);
}

// Helper to construct in-memory Sentinel-4 L2 CODA product trees.
class S4ProductBuilder {
 public:
  S4ProductBuilder(CodaMemProduct& prod, std::string_view product_type,
                   bool use_nir = false, long num_scanlines = 2,
                   long num_pixels = 2, long num_layers = 0,
                   LayerLocation layer_loc = LayerLocation::kNone)
      : prod_(prod),
        num_scanlines_(num_scanlines),
        num_pixels_(num_pixels),
        num_layers_(num_layers) {
    EXPECT_TRUE(SetS4ProductInfo(prod_, product_type));

    constexpr long kScalarArrDims[] = {1};
    constexpr double kTimeRef[] = {20000.0};
    constexpr std::string_view kCycle[] = {"7"};
    EXPECT_TRUE(
        prod_.AddAttribute(prod_.root(), "time_reference_days_since_1950",
                           prod_.MakeDoubleArray(kScalarArrDims, kTimeRef)));
    EXPECT_TRUE(
        prod_.AddAttribute(prod_.root(), "repeat_cycle_in_day",
                           prod_.MakeStringArray(kScalarArrDims, kCycle)));

    product_rec_ = prod_.MakeRecord();
    support_data_rec_ = prod_.MakeRecord();
    geolocations_rec_ = prod_.MakeRecord();
    detailed_results_rec_ = prod_.MakeRecord();
    input_data_rec_ = prod_.MakeRecord();

    EXPECT_TRUE(prod_.AddField(
        prod_.root(), use_nir ? "PRODUCT_NIR" : "PRODUCT", product_rec_));
    EXPECT_TRUE(
        prod_.AddField(product_rec_, "SUPPORT_DATA", support_data_rec_));
    EXPECT_TRUE(
        prod_.AddField(support_data_rec_, "GEOLOCATIONS", geolocations_rec_));
    EXPECT_TRUE(prod_.AddField(support_data_rec_, "DETAILED_RESULTS",
                               detailed_results_rec_));
    EXPECT_TRUE(
        prod_.AddField(support_data_rec_, "INPUT_DATA", input_data_rec_));

    if (num_scanlines_ > 0) {
      std::vector<int32_t> scanline_vals(static_cast<size_t>(num_scanlines_),
                                         0);
      const long scan_dims[] = {num_scanlines_};
      EXPECT_TRUE(
          prod_.AddField(product_rec_, "scanline",
                         prod_.MakeInt32Array(scan_dims, scanline_vals)));
    }
    if (num_pixels_ > 0) {
      std::vector<int32_t> pixel_vals(static_cast<size_t>(num_pixels_), 0);
      const long pix_dims[] = {num_pixels_};
      EXPECT_TRUE(prod_.AddField(product_rec_, "ground_pixel",
                                 prod_.MakeInt32Array(pix_dims, pixel_vals)));
    }
    if (num_layers_ > 0) {
      std::vector<int32_t> layer_vals(static_cast<size_t>(num_layers_), 0);
      const long layer_dims[] = {num_layers_};
      switch (layer_loc) {
        case LayerLocation::kProduct:
          EXPECT_TRUE(
              prod_.AddField(product_rec_, "layer",
                             prod_.MakeInt32Array(layer_dims, layer_vals)));
          break;
        case LayerLocation::kSupportData:
          EXPECT_TRUE(
              prod_.AddField(support_data_rec_, "layer",
                             prod_.MakeInt32Array(layer_dims, layer_vals)));
          break;
        case LayerLocation::kSubcolumn:
          EXPECT_TRUE(
              prod_.AddField(product_rec_, "subcolumn",
                             prod_.MakeInt32Array(layer_dims, layer_vals)));
          break;
        case LayerLocation::kNone:
          break;
      }
    }
  }

  CodaMemNode* product_rec() const { return product_rec_; }
  CodaMemNode* support_data_rec() const { return support_data_rec_; }
  CodaMemNode* geolocations_rec() const { return geolocations_rec_; }
  CodaMemNode* detailed_results_rec() const { return detailed_results_rec_; }
  CodaMemNode* input_data_rec() const { return input_data_rec_; }

  long num_times() const { return num_scanlines_ * num_pixels_; }

  void AddFloatField(CodaMemNode* rec, std::string_view name,
                     std::span<const long> dims, std::span<const float> vals,
                     float fill_val = kFloatFill) {
    CodaMemNode* arr = prod_.MakeFloatArray(dims, vals);
    ASSERT_NE(nullptr, arr);
    constexpr long kOne[] = {1};
    const float fill[] = {fill_val};
    ASSERT_TRUE(
        prod_.AddAttribute(arr, "FillValue", prod_.MakeFloatArray(kOne, fill)));
    ASSERT_TRUE(prod_.AddField(rec, name, arr));
  }

  void AddSeqFloatField(CodaMemNode* rec, std::string_view name, float base,
                        float step = 1.0f, bool include_fill = false) {
    const long n = num_times();
    std::vector<float> vals(static_cast<size_t>(n));
    for (long i = 0; i < n; ++i) {
      vals[static_cast<size_t>(i)] =
          (include_fill && i == n - 1) ? kFloatFill : (base + step * i);
    }
    const long dims[] = {num_scanlines_, num_pixels_};
    AddFloatField(rec, name, dims, vals);
  }

  void AddDoubleField(CodaMemNode* rec, std::string_view name,
                      std::span<const long> dims, std::span<const double> vals,
                      double fill_val = kDoubleFill) {
    CodaMemNode* arr = prod_.MakeDoubleArray(dims, vals);
    ASSERT_NE(nullptr, arr);
    constexpr long kOne[] = {1};
    const double fill[] = {fill_val};
    ASSERT_TRUE(prod_.AddAttribute(arr, "FillValue",
                                   prod_.MakeDoubleArray(kOne, fill)));
    ASSERT_TRUE(prod_.AddField(rec, name, arr));
  }

  void AddInt8Field(CodaMemNode* rec, std::string_view name,
                    std::span<const long> dims, std::span<const int8_t> vals) {
    ASSERT_TRUE(prod_.AddField(rec, name, prod_.MakeInt8Array(dims, vals)));
  }

  void AddUint8Field(CodaMemNode* rec, std::string_view name,
                     std::span<const long> dims,
                     std::span<const uint8_t> vals) {
    ASSERT_TRUE(prod_.AddField(rec, name, prod_.MakeUint8Array(dims, vals)));
  }

  void AddInt32Field(CodaMemNode* rec, std::string_view name,
                     std::span<const long> dims,
                     std::span<const int32_t> vals) {
    ASSERT_TRUE(prod_.AddField(rec, name, prod_.MakeInt32Array(dims, vals)));
  }

  void AddUint32Field(CodaMemNode* rec, std::string_view name,
                      std::span<const long> dims,
                      std::span<const uint32_t> vals) {
    ASSERT_TRUE(prod_.AddField(rec, name, prod_.MakeUint32Array(dims, vals)));
  }

  void PopulateCoreFields(bool include_qa_value, bool use_uint8 = false) {
    const long n = num_times();
    const long dims2[] = {num_scanlines_, num_pixels_};
    const long dims3[] = {num_scanlines_, num_pixels_, 4};

    std::vector<double> delta_t(static_cast<size_t>(n));
    for (long i = 0; i < n; ++i) {
      delta_t[static_cast<size_t>(i)] = 100.0 + 10.0 * i;
    }
    AddDoubleField(product_rec_, "delta_time", dims2, delta_t);

    AddSeqFloatField(product_rec_, "latitude", 40.0f, 1.0f);
    AddSeqFloatField(product_rec_, "longitude", 10.0f, 1.0f);

    std::vector<float> lat_b(static_cast<size_t>(n * 4), 40.0f);
    std::vector<float> lon_b(static_cast<size_t>(n * 4), 10.0f);
    AddFloatField(geolocations_rec_, "latitude_bounds", dims3, lat_b);
    AddFloatField(geolocations_rec_, "longitude_bounds", dims3, lon_b);

    if (include_qa_value) {
      if (use_uint8) {
        std::vector<uint8_t> qa(static_cast<size_t>(n), 95);
        AddUint8Field(product_rec_, "qa_value", dims2, qa);
      } else {
        std::vector<int8_t> qa(static_cast<size_t>(n), 90);
        AddInt8Field(product_rec_, "qa_value", dims2, qa);
      }
    }
  }

  void PopulateAdditionalGeolocationFields() {
    AddSeqFloatField(geolocations_rec_, "solar_zenith_angle", 30.0f);
    AddSeqFloatField(geolocations_rec_, "solar_azimuth_angle", 20.0f);
    AddSeqFloatField(geolocations_rec_, "viewing_zenith_angle", 15.0f);
    AddSeqFloatField(geolocations_rec_, "viewing_azimuth_angle", 45.0f);
  }

  void PopulateSurfaceFields(bool use_uint32 = false) {
    const long n = num_times();
    const long dims2[] = {num_scanlines_, num_pixels_};
    AddSeqFloatField(input_data_rec_, "surface_altitude", 100.0f, 10.0f);
    AddSeqFloatField(input_data_rec_, "surface_altitude_precision", 5.0f, 0.5f);
    AddSeqFloatField(input_data_rec_, "surface_pressure", 101325.0f, -100.0f);
    if (use_uint32) {
      std::vector<uint32_t> sclass(static_cast<size_t>(n), 2);
      AddUint32Field(input_data_rec_, "surface_classification", dims2, sclass);
    } else {
      std::vector<int32_t> sclass(static_cast<size_t>(n), 1);
      AddInt32Field(input_data_rec_, "surface_classification", dims2, sclass);
    }
  }

  void PopulateSnowIceFlag(bool use_uint8 = false) {
    const long n = num_times();
    const long dims2[] = {num_scanlines_, num_pixels_};
    if (use_uint8) {
      std::vector<uint8_t> flag(static_cast<size_t>(n), 1);
      AddUint8Field(input_data_rec_, "snow_ice_flag", dims2, flag);
    } else {
      std::vector<int8_t> flag(static_cast<size_t>(n), 0);
      AddInt8Field(input_data_rec_, "snow_ice_flag", dims2, flag);
    }
  }

  void PopulateStandardInputCloudFields() {
    AddSeqFloatField(input_data_rec_, "cloud_fraction", 0.2f, 0.1f);
    AddSeqFloatField(input_data_rec_, "cloud_fraction_precision", 0.02f, 0.01f);
    AddSeqFloatField(input_data_rec_, "cloud_pressure", 80000.0f, -1000.0f);
    AddSeqFloatField(input_data_rec_, "cloud_pressure_precision", 500.0f,
                     10.0f);
    AddSeqFloatField(input_data_rec_, "cloud_height", 2000.0f, 100.0f);
    AddSeqFloatField(input_data_rec_, "cloud_height_precision", 50.0f, 5.0f);
    AddSeqFloatField(input_data_rec_, "cloud_albedo", 0.5f, 0.05f);
    AddSeqFloatField(input_data_rec_, "cloud_albedo_precision", 0.05f, 0.005f);
  }

  void PopulatePressureBoundsCoefs(bool clamp_toa = true) {
    const long dims1[] = {num_layers_ + 1};
    std::vector<float> coef_a(static_cast<size_t>(num_layers_ + 1));
    std::vector<float> coef_b(static_cast<size_t>(num_layers_ + 1));
    for (long k = 0; k <= num_layers_; ++k) {
      coef_a[static_cast<size_t>(k)] = 100.0f * (num_layers_ - k);
      coef_b[static_cast<size_t>(k)] =
          static_cast<float>(num_layers_ - k) / static_cast<float>(num_layers_);
    }
    if (clamp_toa) {
      // Force top-of-atmosphere boundary < 1e-3f to exercise clamping.
      coef_a[static_cast<size_t>(num_layers_)] = 0.0f;
      coef_b[static_cast<size_t>(num_layers_)] = 0.0f;
    }
    AddFloatField(input_data_rec_, "constant_a_interface", dims1, coef_a);
    AddFloatField(input_data_rec_, "constant_b_interface", dims1, coef_b);
  }

  void AddProfileFloatField(CodaMemNode* rec, std::string_view name,
                            float base) {
    const long n = num_times() * num_layers_;
    const long dims3[] = {num_scanlines_, num_pixels_, num_layers_};
    std::vector<float> vals(static_cast<size_t>(n));
    for (long i = 0; i < n; ++i) {
      vals[static_cast<size_t>(i)] = base + 0.1f * i;
    }
    AddFloatField(rec, name, dims3, vals);
  }

 private:
  CodaMemProduct& prod_;
  long num_scanlines_;
  long num_pixels_;
  long num_layers_;
  CodaMemNode* product_rec_ = nullptr;
  CodaMemNode* support_data_rec_ = nullptr;
  CodaMemNode* geolocations_rec_ = nullptr;
  CodaMemNode* detailed_results_rec_ = nullptr;
  CodaMemNode* input_data_rec_ = nullptr;
};

TEST(HarpIngestS4L2Test, IngestAlhDefaultAnd770SurfaceAlbedo) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S4-L2-ALH");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  S4ProductBuilder b(prod, "UVN-2-ALH");
  b.PopulateCoreFields(/*include_qa_value=*/true, /*use_uint8=*/true);
  b.PopulateAdditionalGeolocationFields();
  b.PopulateSurfaceFields(/*use_uint32=*/true);
  b.PopulateSnowIceFlag(/*use_uint8=*/true);
  b.PopulateStandardInputCloudFields();

  b.AddSeqFloatField(b.product_rec(), "aerosol_mid_height", 1500.0f, 100.0f,
                     /*include_fill=*/true);
  b.AddSeqFloatField(b.product_rec(), "aerosol_mid_height_precision", 50.0f);
  b.AddSeqFloatField(b.product_rec(), "aerosol_mid_pressure", 85000.0f);
  b.AddSeqFloatField(b.product_rec(), "aerosol_mid_pressure_precision", 500.0f);
  b.AddSeqFloatField(b.detailed_results_rec(), "aerosol_optical_thickness",
                     0.3f);
  b.AddSeqFloatField(b.detailed_results_rec(),
                     "aerosol_optical_thickness_precision", 0.03f);
  constexpr long kAlbDims[] = {2, 2, 2};
  constexpr float kAlbVals[] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
  b.AddFloatField(b.detailed_results_rec(), "surface_albedo", kAlbDims,
                  kAlbVals);

  IngestExerciseResult res_default =
      ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res_default.status);
  EXPECT_EQ(4L, res_default.dimensions[harp_dimension_time]);
  EXPECT_THAT(res_default.variables["scan_subindex"].int16_values,
              ElementsAre(0, 1, 0, 1));
  EXPECT_THAT(res_default.variables["cycle_index"].int32_values,
              ElementsAre(7));
  EXPECT_THAT(res_default.variables["datetime_length"].double_values,
              ElementsAre(20.0));
  EXPECT_THAT(res_default.variables["validity"].int8_values,
              ElementsAre(95, 95, 95, 95));
  EXPECT_THAT(res_default.variables["surface_type"].int32_values,
              ElementsAre(2, 2, 2, 2));
  EXPECT_THAT(res_default.variables["snow_ice_type"].int8_values,
              ElementsAre(1, 1, 1, 1));
  EXPECT_THAT(res_default.variables["surface_albedo"].float_values,
              ElementsAre(0.1f, 0.3f, 0.5f, 0.7f));
  EXPECT_TRUE(
      std::isnan(res_default.variables["aerosol_height"].float_values[3]));

  IngestExerciseResult res_770 =
      ExerciseIngestionModule(mod, prod.product(), "surface_albedo=770");
  ASSERT_EQ(HARP_SUCCESS, res_770.status);
  EXPECT_THAT(res_770.variables["surface_albedo"].float_values,
              ElementsAre(0.2f, 0.4f, 0.6f, 0.8f));
}

TEST(HarpIngestS4L2Test, IngestAuiBothWavelengthRatios) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S4-L2-AUI");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  S4ProductBuilder b(prod, "UVN-2-AUI");
  b.PopulateCoreFields(/*include_qa_value=*/true);
  b.PopulateAdditionalGeolocationFields();
  b.PopulateSurfaceFields();
  b.PopulateSnowIceFlag();

  b.AddSeqFloatField(b.product_rec(), "aerosol_index_354_388", 1.1f);
  b.AddSeqFloatField(b.product_rec(), "aerosol_index_354_388_precision", 0.1f);
  b.AddSeqFloatField(b.detailed_results_rec(), "scene_albedo_388", 0.25f);
  b.AddSeqFloatField(b.product_rec(), "aerosol_index_340_380", 2.1f);
  b.AddSeqFloatField(b.product_rec(), "aerosol_index_340_380_precision", 0.2f);
  b.AddSeqFloatField(b.detailed_results_rec(), "scene_albedo_380", 0.35f);

  for (const char* opt :
       {"wavelength_ratio=354_388nm", "wavelength_ratio=340_380nm"}) {
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), opt);
    ASSERT_EQ(HARP_SUCCESS, res.status) << opt;
    const float expected_ai =
        std::string_view(opt) == "wavelength_ratio=354_388nm" ? 1.1f : 2.1f;
    EXPECT_FLOAT_EQ(expected_ai,
                    res.variables["absorbing_aerosol_index"].float_values[0]);
  }
}

TEST(HarpIngestS4L2Test, IngestCldAllModelsAndBands) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S4-L2-CLD");
  ASSERT_NE(nullptr, mod);

  for (bool use_nir : {false, true}) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S4ProductBuilder b(prod, "UVN-2-CLD", use_nir);
    b.PopulateCoreFields(/*include_qa_value=*/true, /*use_uint8=*/use_nir);
    b.PopulateAdditionalGeolocationFields();
    b.PopulateSurfaceFields();
    b.PopulateSnowIceFlag();

    b.AddSeqFloatField(b.product_rec(), "cloud_base_height", 1000.0f);
    b.AddSeqFloatField(b.product_rec(), "cloud_base_height_precision", 50.0f);
    b.AddSeqFloatField(b.product_rec(), "cloud_base_pressure", 90000.0f);
    b.AddSeqFloatField(b.product_rec(), "cloud_base_pressure_precision",
                       200.0f);
    b.AddSeqFloatField(b.product_rec(), "cloud_fraction", 0.4f);
    b.AddSeqFloatField(b.product_rec(), "cloud_fraction_precision", 0.04f);
    b.AddSeqFloatField(b.product_rec(), "cloud_optical_thickness", 12.0f);
    b.AddSeqFloatField(b.product_rec(), "cloud_optical_thickness_precision",
                       1.2f);
    b.AddSeqFloatField(b.product_rec(), "cloud_top_height", 3000.0f);
    b.AddSeqFloatField(b.product_rec(), "cloud_top_height_precision", 100.0f);
    b.AddSeqFloatField(b.product_rec(), "cloud_top_pressure", 70000.0f);
    b.AddSeqFloatField(b.product_rec(), "cloud_top_pressure_precision", 300.0f);

    b.AddSeqFloatField(b.input_data_rec(), "surface_temperature", 285.0f);
    b.AddSeqFloatField(b.input_data_rec(), "cma_cloud_fraction_psf", 0.45f);
    b.AddSeqFloatField(b.input_data_rec(), "oca_total_cot_psf", 14.0f);
    b.AddSeqFloatField(b.input_data_rec(), "oca_total_ctp_psf", 68000.0f);
    b.AddSeqFloatField(b.input_data_rec(), "oca_liquid_cot_psf", 8.0f);
    b.AddSeqFloatField(b.input_data_rec(), "oca_liquid_ctp_psf", 75000.0f);
    b.AddSeqFloatField(b.input_data_rec(), "oca_ice_cot_psf", 6.0f);
    b.AddSeqFloatField(b.input_data_rec(), "oca_ice_ctp_psf", 45000.0f);
    b.AddSeqFloatField(b.input_data_rec(), "oca_desert_dust_cot_psf", 0.5f);
    b.AddSeqFloatField(b.input_data_rec(), "oca_desert_dust_ctp_psf", 85000.0f);

    b.AddSeqFloatField(b.detailed_results_rec(), "surface_albedo", 0.15f);
    b.AddSeqFloatField(b.detailed_results_rec(), "surface_albedo_precision",
                       0.02f);
    b.AddSeqFloatField(b.detailed_results_rec(), "cloud_top_temperature",
                       260.0f);
    constexpr long kDims2[] = {2, 2};
    constexpr int8_t kPhase[] = {0, 1, 2, 1};
    constexpr uint8_t kQaCrb[] = {80, 81, 82, 83};
    constexpr int8_t kErr[] = {0, 0, 1, 0};
    b.AddInt8Field(b.detailed_results_rec(), "cloud_phase", kDims2, kPhase);
    b.AddInt8Field(b.detailed_results_rec(), "processing_errors", kDims2, kErr);
    b.AddSeqFloatField(b.detailed_results_rec(), "cloud_height_crb", 2500.0f);
    b.AddSeqFloatField(b.detailed_results_rec(), "cloud_height_crb_precision",
                       90.0f);
    b.AddSeqFloatField(b.detailed_results_rec(), "cloud_albedo_crb", 0.6f);
    b.AddSeqFloatField(b.detailed_results_rec(), "cloud_albedo_crb_precision",
                       0.06f);
    b.AddUint8Field(b.detailed_results_rec(), "qa_value_crb", kDims2, kQaCrb);
    b.AddSeqFloatField(b.detailed_results_rec(), "surface_albedo_crb", 0.16f);
    b.AddSeqFloatField(b.detailed_results_rec(), "surface_albedo_crb_precision",
                       0.03f);
    b.AddInt8Field(b.detailed_results_rec(), "processing_errors_crb", kDims2,
                   kErr);
    b.AddSeqFloatField(b.detailed_results_rec(), "cloud_fraction_crb_precision",
                       0.05f);
    b.AddSeqFloatField(b.detailed_results_rec(), "cloud_pressure_crb",
                       72000.0f);
    b.AddSeqFloatField(b.detailed_results_rec(), "cloud_pressure_crb_precision",
                       350.0f);

    const std::string band_prefix = use_nir ? "band=NIR;" : "";
    IngestExerciseResult res_cal = ExerciseIngestionModule(
        mod, prod.product(), use_nir ? "band=NIR" : nullptr);
    ASSERT_EQ(HARP_SUCCESS, res_cal.status);
    EXPECT_FLOAT_EQ(70000.0f,
                    res_cal.variables["cloud_top_pressure"].float_values[0]);

    const std::string crb_opts = band_prefix + "model=CRB";
    IngestExerciseResult res_crb =
        ExerciseIngestionModule(mod, prod.product(), crb_opts.c_str());
    ASSERT_EQ(HARP_SUCCESS, res_crb.status);
    EXPECT_FLOAT_EQ(72000.0f,
                    res_crb.variables["cloud_top_pressure"].float_values[0]);
    EXPECT_FLOAT_EQ(
        0.05f, res_crb.variables["cloud_fraction_uncertainty"].float_values[0]);

    const std::string cma_opts = band_prefix + "model=CMA+OCA";
    IngestExerciseResult res_cma =
        ExerciseIngestionModule(mod, prod.product(), cma_opts.c_str());
    ASSERT_EQ(HARP_SUCCESS, res_cma.status);
    EXPECT_FLOAT_EQ(0.45f, res_cma.variables["cloud_fraction"].float_values[0]);
    EXPECT_FLOAT_EQ(14.0f,
                    res_cma.variables["cloud_optical_depth"].float_values[0]);
    EXPECT_FLOAT_EQ(68000.0f,
                    res_cma.variables["cloud_top_pressure"].float_values[0]);
  }
}

TEST(HarpIngestS4L2Test, IngestFcsCmaDefaultAndNir) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S4-L2-FCS-CMA");
  ASSERT_NE(nullptr, mod);

  for (bool use_nir : {false, true}) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S4ProductBuilder b(prod, "UVN-2-FCS-CMA", use_nir);
    b.PopulateCoreFields(/*include_qa_value=*/true);
    b.PopulateAdditionalGeolocationFields();
    b.AddSeqFloatField(b.product_rec(), "cma_cloud_free_psf_mean", 0.5f);
    b.AddSeqFloatField(b.product_rec(), "cma_cloud_contaminated_psf_mean",
                       0.2f);
    b.AddSeqFloatField(b.product_rec(), "cma_cloud_filled_psf_mean", 0.1f);
    b.AddSeqFloatField(b.product_rec(), "cma_desert_dust_psf_mean", 0.05f);
    b.AddSeqFloatField(b.product_rec(), "cma_snow_ice_psf_mean", 0.1f);
    b.AddSeqFloatField(b.product_rec(), "cma_volcanic_ash_psf_mean", 0.05f);

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), use_nir ? "band=NIR" : nullptr);
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_FLOAT_EQ(0.5f, res.variables["cloud_free_fraction"].float_values[0]);
  }
}

TEST(HarpIngestS4L2Test, IngestFdySuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S4-L2-HCH");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  S4ProductBuilder b(prod, "UVN-2-FDY", /*use_nir=*/false, 2, 2, 3,
                     LayerLocation::kSupportData);
  b.PopulateCoreFields(/*include_qa_value=*/true);
  b.PopulateAdditionalGeolocationFields();
  b.PopulateSurfaceFields();
  b.PopulateSnowIceFlag();
  b.PopulateStandardInputCloudFields();
  b.PopulatePressureBoundsCoefs(/*clamp_toa=*/true);

  b.AddSeqFloatField(b.input_data_rec(), "surface_albedo", 0.08f);
  b.AddSeqFloatField(b.product_rec(), "formaldehyde_tropospheric_column",
                     0.001f);
  b.AddSeqFloatField(b.product_rec(),
                     "formaldehyde_tropospheric_column_precision", 0.0001f);
  b.AddSeqFloatField(b.product_rec(),
                     "formaldehyde_tropospheric_column_trueness", 0.0002f);
  b.AddSeqFloatField(b.detailed_results_rec(),
                     "formaldehyde_tropospheric_air_mass_factor", 1.5f);
  b.AddSeqFloatField(b.detailed_results_rec(),
                     "formaldehyde_tropospheric_air_mass_factor_precision",
                     0.15f);
  b.AddSeqFloatField(b.detailed_results_rec(),
                     "formaldehyde_tropospheric_air_mass_factor_trueness",
                     0.2f);
  b.AddProfileFloatField(b.detailed_results_rec(),
                         "formaldehyde_tropospheric_column_averaging_kernel",
                         0.5f);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(3L, res.dimensions[harp_dimension_vertical]);
  // Top-of-atmosphere bound (index 2*3 - 1 = 5) is clamped to 1e-3f.
  EXPECT_FLOAT_EQ(1e-3f, res.variables["pressure_bounds"].float_values[5]);
}

TEST(HarpIngestS4L2Test, IngestGlySuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S4-L2-CHO");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  S4ProductBuilder b(prod, "UVN-2-GLY");
  b.PopulateCoreFields(/*include_qa_value=*/true);
  b.PopulateAdditionalGeolocationFields();
  b.PopulateSurfaceFields();
  b.PopulateSnowIceFlag();
  b.PopulateStandardInputCloudFields();

  b.AddSeqFloatField(b.product_rec(), "glyoxal_tropospheric_column", 0.0005f);
  b.AddSeqFloatField(b.product_rec(), "glyoxal_tropospheric_column_precision",
                     0.00005f);
  b.AddSeqFloatField(b.product_rec(), "glyoxal_tropospheric_column_trueness",
                     0.00006f);
  b.AddSeqFloatField(b.detailed_results_rec(),
                     "glyoxal_tropospheric_column_air_mass_factor", 1.2f);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_FLOAT_EQ(0.0005f,
                  res.variables["tropospheric_C2H2O2_column_number_density"]
                      .float_values[0]);
}

TEST(HarpIngestS4L2Test, IngestNo2DefaultAndTotalColumnOptions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S4-L2-NO2");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  S4ProductBuilder b(prod, "UVN-2-NO2", /*use_nir=*/false, 2, 2, 2,
                     LayerLocation::kProduct);
  b.PopulateCoreFields(/*include_qa_value=*/true);
  b.PopulateAdditionalGeolocationFields();
  b.PopulateSurfaceFields();
  b.PopulateSnowIceFlag();
  b.PopulateStandardInputCloudFields();
  b.PopulatePressureBoundsCoefs(/*clamp_toa=*/false);

  b.AddSeqFloatField(b.product_rec(), "nitrogen_dioxide_doas_total_column",
                     0.002f);
  b.AddSeqFloatField(b.product_rec(),
                     "nitrogen_dioxide_doas_total_column_precision", 0.0002f);
  b.AddSeqFloatField(b.product_rec(), "nitrogen_dioxide_stratospheric_column",
                     0.001f);
  b.AddSeqFloatField(b.product_rec(), "nitrogen_dioxide_tropospheric_column",
                     0.001f);
  b.AddSeqFloatField(b.product_rec(),
                     "nitrogen_dioxide_tropospheric_column_precision", 0.0001f);
  b.AddProfileFloatField(b.detailed_results_rec(), "averaging_kernel", 0.4f);
  b.AddSeqFloatField(b.detailed_results_rec(),
                     "nitrogen_dioxide_stratospheric_air_mass_factor", 2.0f);
  b.AddSeqFloatField(b.detailed_results_rec(),
                     "nitrogen_dioxide_tropospheric_air_mass_factor", 1.3f);

  for (const char* opt : {"total_column=summed", "total_column=total"}) {
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), opt);
    ASSERT_EQ(HARP_SUCCESS, res.status) << opt;
    EXPECT_FLOAT_EQ(0.002f,
                    res.variables["NO2_column_number_density"].float_values[0]);
  }
}

TEST(HarpIngestS4L2Test, IngestO3AndO3TscSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  // 1. S4-L2-OTO (UVN-2-O3) with TOA pressure clamping.
  {
    const harp_ingestion_module* mod = FindIngestionModule("S4-L2-OTO");
    ASSERT_NE(nullptr, mod);

    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S4ProductBuilder b(prod, "UVN-2-O3", /*use_nir=*/false, 2, 2, 2,
                       LayerLocation::kProduct);
    b.PopulateCoreFields(/*include_qa_value=*/true);
    b.PopulateAdditionalGeolocationFields();
    b.PopulateSurfaceFields();
    b.PopulateSnowIceFlag();
    b.PopulatePressureBoundsCoefs(/*clamp_toa=*/true);

    b.AddSeqFloatField(b.input_data_rec(), "cloud_fraction", 0.3f);
    b.AddSeqFloatField(b.input_data_rec(), "cloud_fraction_precision", 0.03f);
    b.AddSeqFloatField(b.input_data_rec(), "cloud_top_pressure", 75000.0f);
    b.AddSeqFloatField(b.input_data_rec(), "cloud_top_pressure_precision",
                       400.0f);
    b.AddSeqFloatField(b.input_data_rec(), "cloud_top_height", 2500.0f);
    b.AddSeqFloatField(b.input_data_rec(), "cloud_top_height_precision", 80.0f);
    b.AddSeqFloatField(b.input_data_rec(), "cloud_base_height", 1500.0f);
    b.AddSeqFloatField(b.input_data_rec(), "cloud_base_height_precision",
                       60.0f);
    b.AddSeqFloatField(b.input_data_rec(), "cloud_base_pressure", 85000.0f);
    b.AddSeqFloatField(b.input_data_rec(), "cloud_base_pressure_precision",
                       300.0f);
    b.AddSeqFloatField(b.input_data_rec(), "surface_albedo", 0.12f);

    b.AddSeqFloatField(b.product_rec(), "ozone_total_column", 0.14f);
    b.AddSeqFloatField(b.product_rec(), "ozone_total_column_precision", 0.01f);
    b.AddSeqFloatField(b.product_rec(), "ozone_total_column_trueness", 0.02f);
    b.AddSeqFloatField(b.detailed_results_rec(), "ozone_total_air_mass_factor",
                       2.5f);
    b.AddSeqFloatField(b.detailed_results_rec(), "ozone_effective_temperature",
                       230.0f);
    b.AddProfileFloatField(b.detailed_results_rec(), "averaging_kernel", 0.6f);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_FLOAT_EQ(1e-3f, res.variables["pressure_bounds"].float_values[3]);
    EXPECT_FLOAT_EQ(0.14f,
                    res.variables["O3_column_number_density"].float_values[0]);
  }

  // 2. S4-L2-OTR (UVN-2-O3-TSC) with transposed subcolumn_bounds.
  {
    const harp_ingestion_module* mod = FindIngestionModule("S4-L2-OTR");
    ASSERT_NE(nullptr, mod);

    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S4ProductBuilder b(prod, "UVN-2-O3-TSC", /*use_nir=*/false, 2, 2, 3,
                       LayerLocation::kSubcolumn);
    b.PopulateCoreFields(/*include_qa_value=*/true);
    b.PopulateAdditionalGeolocationFields();
    b.PopulateSurfaceFields();
    b.PopulateSnowIceFlag();

    b.AddSeqFloatField(b.input_data_rec(), "cloud_fraction", 0.2f);
    b.AddSeqFloatField(b.input_data_rec(), "cloud_top_pressure", 70000.0f);
    b.AddSeqFloatField(b.input_data_rec(), "surface_albedo", 0.1f);

    // subcolumn_bounds has shape {2, 3}, transposed to {3, 2}.
    constexpr long kBoundsDims[] = {2, 3};
    constexpr float kBoundsVals[] = {1000.0f, 800.0f, 600.0f,
                                     800.0f,  600.0f, 400.0f};
    b.AddFloatField(b.product_rec(), "subcolumn_bounds", kBoundsDims,
                    kBoundsVals);
    b.AddProfileFloatField(b.product_rec(), "ozone_subcolumn", 0.05f);
    b.AddProfileFloatField(b.product_rec(), "ozone_subcolumn_precision",
                           0.005f);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["pressure_bounds"].float_values,
                ElementsAre(1000.0f, 800.0f, 800.0f, 600.0f, 600.0f, 400.0f));
  }
}

TEST(HarpIngestS4L2Test, IngestSo2AllColumnAndAlbedoOptions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S4-L2-SO2");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  S4ProductBuilder b(prod, "UVN-2-SO2", /*use_nir=*/false, 2, 2, 2,
                     LayerLocation::kSupportData);
  b.PopulateCoreFields(/*include_qa_value=*/true);
  b.PopulateAdditionalGeolocationFields();
  b.PopulateSurfaceFields();
  b.PopulateSnowIceFlag();
  b.PopulateStandardInputCloudFields();
  b.PopulatePressureBoundsCoefs(/*clamp_toa=*/false);

  b.AddSeqFloatField(b.input_data_rec(), "surface_albedo_328", 0.05f);
  b.AddSeqFloatField(b.input_data_rec(), "surface_albedo_376", 0.07f);
  b.AddProfileFloatField(b.detailed_results_rec(), "averaging_kernel", 0.3f);

  b.AddSeqFloatField(b.product_rec(), "sulfur_dioxide_total_column_polluted",
                     1.0f);
  b.AddSeqFloatField(b.product_rec(),
                     "sulfur_dioxide_total_column_polluted_precision", 0.1f);
  b.AddSeqFloatField(b.product_rec(),
                     "sulfur_dioxide_total_column_polluted_trueness", 0.2f);
  b.AddSeqFloatField(b.detailed_results_rec(),
                     "sulfur_dioxide_total_air_mass_factor_polluted", 1.1f);
  b.AddSeqFloatField(b.detailed_results_rec(),
                     "sulfur_dioxide_total_air_mass_factor_polluted_precision",
                     0.11f);
  b.AddSeqFloatField(b.detailed_results_rec(),
                     "sulfur_dioxide_total_air_mass_factor_polluted_trueness",
                     0.12f);

  for (std::string_view suffix : {"1km", "7km", "15km"}) {
    const float base = suffix == "1km" ? 2.0f : (suffix == "7km" ? 3.0f : 4.0f);
    b.AddSeqFloatField(
        b.detailed_results_rec(),
        std::string("sulfur_dioxide_total_column_") + std::string(suffix),
        base);
    b.AddSeqFloatField(b.detailed_results_rec(),
                       std::string("sulfur_dioxide_total_column_") +
                           std::string(suffix) + "_precision",
                       base * 0.1f);
    b.AddSeqFloatField(b.detailed_results_rec(),
                       std::string("sulfur_dioxide_total_column_") +
                           std::string(suffix) + "_trueness",
                       base * 0.2f);
    b.AddSeqFloatField(b.detailed_results_rec(),
                       std::string("sulfur_dioxide_total_air_mass_factor_") +
                           std::string(suffix),
                       base * 0.5f);
    b.AddSeqFloatField(b.detailed_results_rec(),
                       std::string("sulfur_dioxide_total_air_mass_factor_") +
                           std::string(suffix) + "_precision",
                       0.05f);
    b.AddSeqFloatField(b.detailed_results_rec(),
                       std::string("sulfur_dioxide_total_air_mass_factor_") +
                           std::string(suffix) + "_trueness",
                       0.06f);
  }

  struct So2Case {
    const char* opts;
    float expected_so2;
    float expected_albedo;
  };
  constexpr So2Case kCases[] = {
      {.opts = nullptr, .expected_so2 = 1.0f, .expected_albedo = 0.05f},
      {.opts = "so2_column=1km;surface_albedo=376",
       .expected_so2 = 2.0f,
       .expected_albedo = 0.07f},
      {.opts = "so2_column=7km",
       .expected_so2 = 3.0f,
       .expected_albedo = 0.05f},
      {.opts = "so2_column=15km",
       .expected_so2 = 4.0f,
       .expected_albedo = 0.05f},
  };
  for (const auto& tc : kCases) {
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), tc.opts);
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_FLOAT_EQ(tc.expected_so2,
                    res.variables["SO2_column_number_density"].float_values[0]);
    EXPECT_FLOAT_EQ(tc.expected_albedo,
                    res.variables["surface_albedo"].float_values[0]);
  }
}

TEST(HarpIngestS4L2Test, InitAndDimensionErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod_alh = FindIngestionModule("S4-L2-ALH");
  const harp_ingestion_module* mod_cld = FindIngestionModule("S4-L2-CLD");
  const harp_ingestion_module* mod_fdy = FindIngestionModule("S4-L2-HCH");
  const harp_ingestion_module* mod_no2 = FindIngestionModule("S4-L2-NO2");
  const harp_ingestion_module* mod_otr = FindIngestionModule("S4-L2-OTR");
  ASSERT_NE(nullptr, mod_alh);
  ASSERT_NE(nullptr, mod_cld);
  ASSERT_NE(nullptr, mod_fdy);
  ASSERT_NE(nullptr, mod_no2);
  ASSERT_NE(nullptr, mod_otr);

  // 1. Unsupported product type.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(SetS4ProductInfo(prod, "UNKNOWN-TYPE"));
    IngestExerciseResult res = ExerciseIngestionModule(mod_alh, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 2. Missing PRODUCT / PRODUCT_NIR / SUPPORT_DATA / GEOLOCATIONS /
  // DETAILED_RESULTS / INPUT_DATA.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(SetS4ProductInfo(prod, "UVN-2-CLD"));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_cld, prod.product()).status);
    EXPECT_EQ(
        -1,
        ExerciseIngestionModule(mod_cld, prod.product(), "band=NIR").status);

    CodaMemNode* p = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", p));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_cld, prod.product()).status);

    CodaMemNode* sd = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(p, "SUPPORT_DATA", sd));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_cld, prod.product()).status);

    ASSERT_TRUE(prod.AddField(sd, "GEOLOCATIONS", prod.MakeRecord()));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_cld, prod.product()).status);

    ASSERT_TRUE(prod.AddField(sd, "DETAILED_RESULTS", prod.MakeRecord()));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_cld, prod.product()).status);

    // With all groups present, missing dimension arrays yield 0-sized dims
    // (cycle_index/datetime_length are 0D so they still fail without attrs).
    ASSERT_TRUE(prod.AddField(sd, "INPUT_DATA", prod.MakeRecord()));
    IngestExerciseResult empty_res =
        ExerciseIngestionModule(mod_cld, prod.product());
    EXPECT_EQ(-1, empty_res.status);
    EXPECT_EQ(0L, empty_res.dimensions[harp_dimension_time]);
  }

  // 4. Non-array and multi-dimensional dimension variables.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S4ProductBuilder b(prod, "UVN-2-ALH", /*use_nir=*/false, 0, 0, 0);
    // Scalar scanline triggers coda_cursor_get_array_dim failure.
    ASSERT_TRUE(prod.AddField(b.product_rec(), "scanline", prod.MakeInt32(1)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_alh, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S4ProductBuilder b(prod, "UVN-2-ALH", /*use_nir=*/false, 0, 0, 0);
    constexpr long kDims2D[] = {1, 2};
    constexpr int32_t kVals[] = {0, 1};
    ASSERT_TRUE(prod.AddField(b.product_rec(), "scanline",
                              prod.MakeInt32Array(kDims2D, kVals)));
    IngestExerciseResult res = ExerciseIngestionModule(mod_alh, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S4ProductBuilder b(prod, "UVN-2-ALH", /*use_nir=*/false, 2, 0, 0);
    ASSERT_TRUE(
        prod.AddField(b.product_rec(), "ground_pixel", prod.MakeInt32(1)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_alh, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S4ProductBuilder b(prod, "UVN-2-O3-TSC", /*use_nir=*/false, 2, 2, 0);
    ASSERT_TRUE(prod.AddField(b.product_rec(), "subcolumn", prod.MakeInt32(1)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_otr, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S4ProductBuilder b(prod, "UVN-2-FDY", /*use_nir=*/false, 2, 2, 0);
    ASSERT_TRUE(
        prod.AddField(b.support_data_rec(), "layer", prod.MakeInt32(1)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_fdy, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S4ProductBuilder b(prod, "UVN-2-NO2", /*use_nir=*/false, 2, 2, 0);
    ASSERT_TRUE(prod.AddField(b.product_rec(), "layer", prod.MakeInt32(1)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_no2, prod.product()).status);
  }
}

TEST(HarpIngestS4L2Test, ReadDatasetAndVariableErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod_alh = FindIngestionModule("S4-L2-ALH");
  const harp_ingestion_module* mod_fdy = FindIngestionModule("S4-L2-HCH");
  const harp_ingestion_module* mod_oto = FindIngestionModule("S4-L2-OTO");
  const harp_ingestion_module* mod_otr = FindIngestionModule("S4-L2-OTR");
  ASSERT_NE(nullptr, mod_alh);
  ASSERT_NE(nullptr, mod_fdy);
  ASSERT_NE(nullptr, mod_oto);
  ASSERT_NE(nullptr, mod_otr);

  // 1. Missing fields and wrong element counts / types on ALH (num_times=1
  //    so scalar vs 1-element array paths can be triggered cleanly).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(SetS4ProductInfo(prod, "UVN-2-ALH"));

    // Add invalid attribute types for time_reference and repeat_cycle_in_day.
    constexpr long kOne[] = {1};
    constexpr std::string_view kNonNum[] = {"abc"};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_reference_days_since_1950",
                                  prod.MakeStringArray(kOne, kNonNum)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "repeat_cycle_in_day",
                                  prod.MakeStringArray(kOne, kNonNum)));

    CodaMemNode* p = prod.MakeRecord();
    CodaMemNode* sd = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    CodaMemNode* dr = prod.MakeRecord();
    CodaMemNode* in = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", p));
    ASSERT_TRUE(prod.AddField(p, "SUPPORT_DATA", sd));
    ASSERT_TRUE(prod.AddField(sd, "GEOLOCATIONS", geo));
    ASSERT_TRUE(prod.AddField(sd, "DETAILED_RESULTS", dr));
    ASSERT_TRUE(prod.AddField(sd, "INPUT_DATA", in));

    constexpr int32_t kDim1[] = {0};
    ASSERT_TRUE(prod.AddField(p, "scanline", prod.MakeInt32Array(kOne, kDim1)));
    ASSERT_TRUE(
        prod.AddField(p, "ground_pixel", prod.MakeInt32Array(kOne, kDim1)));

    // Scalar int8 and scalar int32 trigger goto_first_array_element failure.
    ASSERT_TRUE(prod.AddField(p, "qa_value", prod.MakeInt8(1)));
    ASSERT_TRUE(prod.AddField(in, "surface_classification", prod.MakeInt32(1)));
    // Wrong element count for latitude (2 instead of 1).
    constexpr long kTwo[] = {2};
    constexpr float kTwoFloats[] = {1.0f, 2.0f};
    ASSERT_TRUE(
        prod.AddField(p, "latitude", prod.MakeFloatArray(kTwo, kTwoFloats)));
    // Wrong type for longitude (string array -> read_float_array fails).
    ASSERT_TRUE(
        prod.AddField(p, "longitude", prod.MakeStringArray(kOne, kNonNum)));
    // Float array without @FillValue.
    constexpr float kOneFloat[] = {1.0f};
    ASSERT_TRUE(prod.AddField(geo, "solar_zenith_angle",
                              prod.MakeFloatArray(kOne, kOneFloat)));
    // Float array with non-float @FillValue.
    CodaMemNode* bad_fill_arr = prod.MakeFloatArray(kOne, kOneFloat);
    ASSERT_TRUE(prod.AddAttribute(bad_fill_arr, "FillValue",
                                  prod.MakeStringArray(kOne, kNonNum)));
    ASSERT_TRUE(prod.AddField(geo, "solar_azimuth_angle", bad_fill_arr));

    // Double array (delta_time) with non-double element type.
    ASSERT_TRUE(
        prod.AddField(p, "delta_time", prod.MakeStringArray(kOne, kNonNum)));
    // Scalar snow_ice_flag vs wrong-type surface_albedo.
    constexpr std::string_view kBadAlhAlb[] = {"a", "b"};
    ASSERT_TRUE(prod.AddField(in, "snow_ice_flag",
                              prod.MakeStringArray(kOne, kNonNum)));
    ASSERT_TRUE(prod.AddField(dr, "surface_albedo",
                              prod.MakeStringArray(kTwo, kBadAlhAlb)));

    IngestExerciseResult res = ExerciseIngestionModule(mod_alh, prod.product(),
                                                       /*options_str=*/nullptr,
                                                       /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["cycle_index"].read_status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_length"].read_status);
    EXPECT_EQ(-1, res.variables["validity"].read_status);
    EXPECT_EQ(-1, res.variables["surface_type"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["solar_zenith_angle"].read_status);
    EXPECT_EQ(-1, res.variables["solar_azimuth_angle"].read_status);
    EXPECT_EQ(-1, res.variables["snow_ice_type"].read_status);
    EXPECT_EQ(-1, res.variables["surface_albedo"].read_status);
  }

  // 2. Missing attributes, non-string repeat_cycle, and double @FillValue
  // errors.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(SetS4ProductInfo(prod, "UVN-2-ALH"));
    constexpr long kOne[] = {1};
    constexpr int32_t kInt[] = {7};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "repeat_cycle_in_day",
                                  prod.MakeInt32Array(kOne, kInt)));

    CodaMemNode* p = prod.MakeRecord();
    CodaMemNode* sd = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", p));
    ASSERT_TRUE(prod.AddField(p, "SUPPORT_DATA", sd));
    ASSERT_TRUE(prod.AddField(sd, "GEOLOCATIONS", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(sd, "DETAILED_RESULTS", prod.MakeRecord()));
    CodaMemNode* in = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sd, "INPUT_DATA", in));
    ASSERT_TRUE(prod.AddField(p, "scanline", prod.MakeInt32Array(kOne, kInt)));
    ASSERT_TRUE(
        prod.AddField(p, "ground_pixel", prod.MakeInt32Array(kOne, kInt)));

    // Scalar delta_time -> datetime_length fails at goto_first_array_element.
    ASSERT_TRUE(prod.AddField(p, "delta_time", prod.MakeDouble(1.0)));
    // Non-int32 array for surface_classification -> read_int32_array fails.
    constexpr std::string_view kBadStr[] = {"bad"};
    ASSERT_TRUE(prod.AddField(in, "surface_classification",
                              prod.MakeStringArray(kOne, kBadStr)));

    IngestExerciseResult res = ExerciseIngestionModule(mod_alh, prod.product(),
                                                       /*options_str=*/nullptr,
                                                       /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_length"].read_status);
    EXPECT_EQ(-1, res.variables["cycle_index"].read_status);
    EXPECT_EQ(-1, res.variables["surface_type"].read_status);
  }

  // 3. Double @FillValue missing / wrong type and delta_time length too short.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(SetS4ProductInfo(prod, "UVN-2-ALH"));
    constexpr long kOne[] = {1};
    constexpr double kRef[] = {20000.0};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_reference_days_since_1950",
                                  prod.MakeDoubleArray(kOne, kRef)));
    CodaMemNode* p = prod.MakeRecord();
    CodaMemNode* sd = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", p));
    ASSERT_TRUE(prod.AddField(p, "SUPPORT_DATA", sd));
    ASSERT_TRUE(prod.AddField(sd, "GEOLOCATIONS", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(sd, "DETAILED_RESULTS", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(sd, "INPUT_DATA", prod.MakeRecord()));
    constexpr int32_t kInt[] = {0};
    ASSERT_TRUE(prod.AddField(p, "scanline", prod.MakeInt32Array(kOne, kInt)));
    ASSERT_TRUE(
        prod.AddField(p, "ground_pixel", prod.MakeInt32Array(kOne, kInt)));
    // 1-element delta_time without @FillValue: datetime fails at @FillValue,
    // datetime_length fails at goto_array_element_by_index(1).
    ASSERT_TRUE(
        prod.AddField(p, "delta_time", prod.MakeDoubleArray(kOne, kRef)));

    IngestExerciseResult res = ExerciseIngestionModule(mod_alh, prod.product(),
                                                       /*options_str=*/nullptr,
                                                       /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_length"].read_status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(SetS4ProductInfo(prod, "UVN-2-ALH"));
    constexpr long kOne[] = {1};
    constexpr double kRef[] = {20000.0};
    constexpr std::string_view kBadFill[] = {"bad"};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_reference_days_since_1950",
                                  prod.MakeDoubleArray(kOne, kRef)));
    CodaMemNode* p = prod.MakeRecord();
    CodaMemNode* sd = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", p));
    ASSERT_TRUE(prod.AddField(p, "SUPPORT_DATA", sd));
    ASSERT_TRUE(prod.AddField(sd, "GEOLOCATIONS", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(sd, "DETAILED_RESULTS", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(sd, "INPUT_DATA", prod.MakeRecord()));
    constexpr int32_t kInt[] = {0};
    ASSERT_TRUE(prod.AddField(p, "scanline", prod.MakeInt32Array(kOne, kInt)));
    ASSERT_TRUE(
        prod.AddField(p, "ground_pixel", prod.MakeInt32Array(kOne, kInt)));

    CodaMemNode* dt = prod.MakeDoubleArray(kOne, kRef);
    ASSERT_TRUE(prod.AddAttribute(dt, "FillValue",
                                  prod.MakeStringArray(kOne, kBadFill)));
    ASSERT_TRUE(prod.AddField(p, "delta_time", dt));

    IngestExerciseResult res = ExerciseIngestionModule(mod_alh, prod.product(),
                                                       /*options_str=*/nullptr,
                                                       /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
  }

  // 4. Pressure bounds & subcolumn bounds error paths (missing coef_a,
  //    coef_b, surface_pressure, subcolumn_bounds).
  for (int stage = 0; stage < 3; ++stage) {
    CodaMemProduct prod_fdy;
    ASSERT_TRUE(prod_fdy.ok());
    S4ProductBuilder bf(prod_fdy, "UVN-2-FDY", false, 2, 2, 2,
                        LayerLocation::kSupportData);
    if (stage >= 1) {
      constexpr long kC[] = {3};
      constexpr float kV[] = {0.0f, 0.0f, 0.0f};
      bf.AddFloatField(bf.input_data_rec(), "constant_a_interface", kC, kV);
    }
    if (stage >= 2) {
      constexpr long kC[] = {3};
      constexpr float kV[] = {1.0f, 0.5f, 0.0f};
      bf.AddFloatField(bf.input_data_rec(), "constant_b_interface", kC, kV);
    }
    IngestExerciseResult rf = ExerciseIngestionModule(
        mod_fdy, prod_fdy.product(), nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, rf.variables["pressure_bounds"].read_status);

    CodaMemProduct prod_oto;
    ASSERT_TRUE(prod_oto.ok());
    S4ProductBuilder bo(prod_oto, "UVN-2-O3", false, 2, 2, 2,
                        LayerLocation::kProduct);
    if (stage >= 1) {
      constexpr long kC[] = {3};
      constexpr float kV[] = {0.0f, 0.0f, 0.0f};
      bo.AddFloatField(bo.input_data_rec(), "constant_a_interface", kC, kV);
    }
    if (stage >= 2) {
      constexpr long kC[] = {3};
      constexpr float kV[] = {1.0f, 0.5f, 0.0f};
      bo.AddFloatField(bo.input_data_rec(), "constant_b_interface", kC, kV);
    }
    IngestExerciseResult ro = ExerciseIngestionModule(
        mod_oto, prod_oto.product(), nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, ro.variables["pressure_bounds"].read_status);
  }
  {
    CodaMemProduct prod_otr;
    ASSERT_TRUE(prod_otr.ok());
    S4ProductBuilder bt(prod_otr, "UVN-2-O3-TSC", false, 2, 2, 2,
                        LayerLocation::kSubcolumn);
    IngestExerciseResult rt = ExerciseIngestionModule(
        mod_otr, prod_otr.product(), nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, rt.variables["pressure_bounds"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
