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
// Unit tests for OMI L2 ingestion modules (libharp/harp-ingest-omi_l2.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-omi_l2.c

#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
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
using ::testing::IsEmpty;

constexpr double kSecondsFrom1993To2000 = 220838400.0 + 5.0;
constexpr double kMissingDouble = -1.2676506e+30;

// Builds the standard
// /HDFEOS/SWATHS/<swath_name>/{Geolocation_Fields,Data_Fields} hierarchy inside
// an in-memory CODA product.
struct OmiSwathBuilder {
  CodaMemProduct prod;
  CodaMemNode* geo = nullptr;
  CodaMemNode* data = nullptr;

  explicit OmiSwathBuilder(std::string_view swath_name) {
    if (!prod.ok()) {
      return;
    }
    CodaMemNode* hdfeos = prod.MakeRecord();
    CodaMemNode* swaths = prod.MakeRecord();
    CodaMemNode* swath = prod.MakeRecord();
    geo = prod.MakeRecord();
    data = prod.MakeRecord();
    if (!prod.AddField(swath, "Geolocation_Fields", geo) ||
        !prod.AddField(swath, "Data_Fields", data) ||
        !prod.AddField(swaths, swath_name, swath) ||
        !prod.AddField(hdfeos, "SWATHS", swaths) ||
        !prod.AddField(prod.root(), "HDFEOS", hdfeos)) {
      geo = nullptr;
      data = nullptr;
    }
  }

  bool ok() const { return prod.ok() && geo != nullptr && data != nullptr; }

  bool AddDoubleField(CodaMemNode* parent, std::string_view name,
                      std::span<const long> dims,
                      std::span<const double> values,
                      double missing_value = kMissingDouble,
                      bool include_scale = false, double scale_factor = 1.0,
                      bool include_offset = false, double offset = 0.0) {
    CodaMemNode* node = prod.MakeDoubleArray(dims, values);
    if (node == nullptr) {
      return false;
    }
    constexpr long kAttrDims[] = {1};
    const double missing_arr[] = {missing_value};
    if (!prod.AddAttribute(node, "MissingValue",
                           prod.MakeDoubleArray(kAttrDims, missing_arr))) {
      return false;
    }
    if (include_scale) {
      const double scale_arr[] = {scale_factor};
      if (!prod.AddAttribute(node, "ScaleFactor",
                             prod.MakeDoubleArray(kAttrDims, scale_arr))) {
        return false;
      }
    }
    if (include_offset) {
      const double offset_arr[] = {offset};
      if (!prod.AddAttribute(node, "Offset",
                             prod.MakeDoubleArray(kAttrDims, offset_arr))) {
        return false;
      }
    }
    return prod.AddField(parent, name, node);
  }

  bool AddInt32Field(CodaMemNode* parent, std::string_view name,
                     std::span<const long> dims,
                     std::span<const int32_t> values) {
    return prod.AddField(parent, name, prod.MakeInt32Array(dims, values));
  }

  bool AddUint8Field(CodaMemNode* parent, std::string_view name,
                     std::span<const long> dims,
                     std::span<const uint8_t> values) {
    return prod.AddField(parent, name, prod.MakeUint8Array(dims, values));
  }

  // Populates Time, Longitude, Latitude for a 2x2 swath. When `lon_offset` is
  // +176.0 or -180.0, extrapolated footprint corners cross +/-180 deg.
  bool AddBaseGeo2x2(double lon_base = 10.0, double lat_base = 20.0) {
    constexpr long kTimeDims[] = {2};
    const double time_vals[] = {kSecondsFrom1993To2000 + 100.0,
                                kSecondsFrom1993To2000 + 200.0};
    constexpr long kGridDims[] = {2, 2};
    const double lon_vals[] = {lon_base, lon_base + 2.0, lon_base,
                               lon_base + 2.0};
    const double lat_vals[] = {lat_base, lat_base, lat_base + 2.0,
                               lat_base + 2.0};
    return AddDoubleField(geo, "Time", kTimeDims, time_vals) &&
           AddDoubleField(geo, "Longitude", kGridDims, lon_vals) &&
           AddDoubleField(geo, "Latitude", kGridDims, lat_vals);
  }

  bool AddAngles2x2() {
    constexpr long kGridDims[] = {2, 2};
    constexpr double kSza[] = {30.0, 31.0, 32.0, 33.0};
    constexpr double kSaa[] = {120.0, 121.0, 122.0, 123.0};
    constexpr double kVza[] = {5.0, 6.0, 7.0, 8.0};
    constexpr double kVaa[] = {210.0, 211.0, 212.0, 213.0};
    return AddDoubleField(geo, "SolarZenithAngle", kGridDims, kSza) &&
           AddDoubleField(geo, "SolarAzimuthAngle", kGridDims, kSaa) &&
           AddDoubleField(geo, "ViewingZenithAngle", kGridDims, kVza) &&
           AddDoubleField(geo, "ViewingAzimuthAngle", kGridDims, kVaa);
  }

  bool AddSpacecraft2() {
    constexpr long kTimeDims[] = {2};
    constexpr double kAlt[] = {705000.0, 705100.0};
    constexpr double kLat[] = {20.0, 22.0};
    constexpr double kLon[] = {10.0, 10.5};
    return AddDoubleField(geo, "SpacecraftAltitude", kTimeDims, kAlt) &&
           AddDoubleField(geo, "SpacecraftLatitude", kTimeDims, kLat) &&
           AddDoubleField(geo, "SpacecraftLongitude", kTimeDims, kLon);
  }
};

// Reads a single variable through a module definition's read_block/read_all
// directly (used to test read_latitude_bounds before read_longitude_bounds).
int ReadSingleVariableDirect(const harp_ingestion_module* mod,
                             coda_product* cprod, std::string_view var_name,
                             std::vector<double>* out) {
  ScopedHarpIngestionOptions opts;
  harp_ingestion_options* raw_opts = nullptr;
  if (harp_ingestion_options_new(&raw_opts) != 0) {
    return -1;
  }
  opts.reset(raw_opts);

  harp_product_definition* prod_def = nullptr;
  void* user_data = nullptr;
  if (mod->ingestion_init(mod, cprod, opts.get(), &prod_def, &user_data) != 0) {
    return -1;
  }
  auto done_guard = absl::MakeCleanup([&] {
    if (mod->ingestion_done != nullptr && user_data != nullptr) {
      mod->ingestion_done(user_data);
    }
  });

  long dims[HARP_NUM_DIM_TYPES] = {};
  if (prod_def->read_dimensions(user_data, dims) != 0) {
    return -1;
  }

  for (int i = 0; i < prod_def->num_variable_definitions; ++i) {
    const harp_variable_definition* var_def = prod_def->variable_definition[i];
    if (var_name != var_def->name) {
      continue;
    }
    long stride = 1;
    for (int d = 1; d < var_def->num_dimensions; ++d) {
      stride *= (var_def->dimension_type[d] == harp_dimension_independent)
                    ? var_def->dimension[d]
                    : dims[var_def->dimension_type[d]];
    }
    out->assign(static_cast<size_t>(dims[var_def->dimension_type[0]] * stride),
                0.0);
    for (long blk = 0; blk < dims[var_def->dimension_type[0]]; ++blk) {
      harp_array slice{};
      slice.double_data = out->data() + blk * stride;
      if (var_def->read_block(user_data, blk, slice) != 0) {
        return -1;
      }
    }
    return 0;
  }
  return -1;
}

TEST(HarpIngestOmiL2Test, IngestOmaeruvSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("OMI_L2_OMAERUV");
  ASSERT_NE(nullptr, mod);

  OmiSwathBuilder b("Aerosol_NearUV_Swath");
  ASSERT_TRUE(b.ok());
  // Use lon_base=177 so extrapolated corner longitudes exceed 180 and wrap.
  ASSERT_TRUE(b.AddBaseGeo2x2(/*lon_base=*/177.0, /*lat_base=*/10.0));
  constexpr long kGridDims[] = {2, 2};
  constexpr double kPressure[] = {1013.0, 1010.0, 1005.0, 1000.0};
  ASSERT_TRUE(b.AddDoubleField(b.geo, "TerrainPressure", kGridDims, kPressure));

  constexpr long kSpecDims[] = {2, 2, 3};
  // Include a missing value and non-trivial ScaleFactor/Offset to cover
  // transform_array_double and get_variable_attributes.
  const double kAod[] = {
      0.1, kMissingDouble, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0, 1.1, 1.2};
  constexpr double kAaod[] = {0.01, 0.02, 0.03, 0.04, 0.05, 0.06,
                              0.07, 0.08, 0.09, 0.10, 0.11, 0.12};
  constexpr double kUvai[] = {1.0, 2.0, 3.0, 4.0};
  ASSERT_TRUE(b.AddDoubleField(b.data, "FinalAerosolOpticalDepth", kSpecDims,
                               kAod, kMissingDouble, /*include_scale=*/true,
                               2.0, /*include_offset=*/true, 0.5));
  ASSERT_TRUE(b.AddDoubleField(b.data, "FinalAerosolAbsOpticalDepth", kSpecDims,
                               kAaod));
  ASSERT_TRUE(b.AddDoubleField(b.data, "UVAerosolIndex", kGridDims, kUvai));

  IngestExerciseResult res = ExerciseIngestionModule(mod, b.prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(4L, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(3L, res.dimensions[harp_dimension_spectral]);
  EXPECT_THAT(res.variables["datetime"].double_values,
              ElementsAre(100.0, 100.0, 200.0, 200.0));
  EXPECT_THAT(res.variables["wavelength"].double_values,
              ElementsAre(354.0, 388.0, 500.0));
  EXPECT_THAT(res.variables["surface_pressure"].double_values,
              ElementsAre(1013.0, 1010.0, 1005.0, 1000.0));
  EXPECT_THAT(res.variables["uv_aerosol_index"].double_values,
              ElementsAre(1.0, 2.0, 3.0, 4.0));

  const auto& aod = res.variables["aerosol_optical_depth"].double_values;
  ASSERT_EQ(12u, aod.size());
  EXPECT_DOUBLE_EQ(0.7, aod[0]);
  EXPECT_TRUE(std::isnan(aod[1]));
  EXPECT_DOUBLE_EQ(2.9, aod[11]);

  // Verify wrapped corner longitudes lie in [-180, 180].
  for (double lon : res.variables["longitude_bounds"].double_values) {
    EXPECT_GE(lon, -180.0);
    EXPECT_LE(lon, 180.0);
  }
}

TEST(HarpIngestOmiL2Test, IngestOmbroSuccessAndDestriped) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("OMI_L2_OMBRO");
  ASSERT_NE(nullptr, mod);

  OmiSwathBuilder b("OMI_Total_Column_Amount_BRO");
  ASSERT_TRUE(b.ok());
  ASSERT_TRUE(b.AddBaseGeo2x2());
  ASSERT_TRUE(b.AddSpacecraft2());
  constexpr long kGridDims[] = {2, 2};
  constexpr double kHeight[] = {100.0, 200.0, 300.0, 400.0};
  constexpr double kCol[] = {1.0e13, 2.0e13, 3.0e13, 4.0e13};
  constexpr double kDestr[] = {1.5e13, 2.5e13, 3.5e13, 4.5e13};
  constexpr double kUnc[] = {0.1e13, 0.2e13, 0.3e13, 0.4e13};
  ASSERT_TRUE(b.AddDoubleField(b.geo, "TerrainHeight", kGridDims, kHeight));
  ASSERT_TRUE(b.AddDoubleField(b.data, "ColumnAmount", kGridDims, kCol));
  ASSERT_TRUE(
      b.AddDoubleField(b.data, "ColumnAmountDestriped", kGridDims, kDestr));
  ASSERT_TRUE(b.AddDoubleField(b.data, "ColumnUncertainty", kGridDims, kUnc));

  IngestExerciseResult def_res = ExerciseIngestionModule(mod, b.prod.product());
  ASSERT_EQ(HARP_SUCCESS, def_res.status);
  EXPECT_THAT(def_res.variables["BrO_column_number_density"].double_values,
              ElementsAre(1.0e13, 2.0e13, 3.0e13, 4.0e13));
  EXPECT_TRUE(
      def_res.variables["BrO_column_number_density_uncertainty"].included);
  EXPECT_THAT(
      def_res.variables["BrO_column_number_density_uncertainty"].double_values,
      ElementsAre(0.1e13, 0.2e13, 0.3e13, 0.4e13));
  EXPECT_THAT(def_res.variables["sensor_altitude"].double_values,
              ElementsAre(705000.0, 705000.0, 705100.0, 705100.0));
  EXPECT_THAT(def_res.variables["surface_altitude"].double_values,
              ElementsAre(100.0, 200.0, 300.0, 400.0));

  IngestExerciseResult destr_res =
      ExerciseIngestionModule(mod, b.prod.product(), "destriped=true");
  ASSERT_EQ(HARP_SUCCESS, destr_res.status);
  EXPECT_THAT(destr_res.variables["BrO_column_number_density"].double_values,
              ElementsAre(1.5e13, 2.5e13, 3.5e13, 4.5e13));
  EXPECT_FALSE(
      destr_res.variables["BrO_column_number_density_uncertainty"].included);
}

TEST(HarpIngestOmiL2Test, IngestOmchochoSuccessAndDestriped) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("OMI_L2_OMCHOCHO");
  ASSERT_NE(nullptr, mod);

  OmiSwathBuilder b("OMI_Total_Column_Amount_CHOCHO");
  ASSERT_TRUE(b.ok());
  // Use lon_base=-179 so extrapolated corner longitudes go < -180 and wrap.
  ASSERT_TRUE(b.AddBaseGeo2x2(/*lon_base=*/-179.0, /*lat_base=*/-10.0));
  constexpr long kGridDims[] = {2, 2};
  constexpr double kCol[] = {1.0e14, 2.0e14, 3.0e14, 4.0e14};
  constexpr double kDestr[] = {1.1e14, 2.1e14, 3.1e14, 4.1e14};
  constexpr double kUnc[] = {0.2e14, 0.3e14, 0.4e14, 0.5e14};
  ASSERT_TRUE(b.AddDoubleField(b.data, "ColumnAmount", kGridDims, kCol));
  ASSERT_TRUE(
      b.AddDoubleField(b.data, "ColumnAmountDestriped", kGridDims, kDestr));
  ASSERT_TRUE(b.AddDoubleField(b.data, "ColumnUncertainty", kGridDims, kUnc));

  IngestExerciseResult def_res = ExerciseIngestionModule(mod, b.prod.product());
  ASSERT_EQ(HARP_SUCCESS, def_res.status);
  EXPECT_THAT(def_res.variables["C2H2O2_column_number_density"].double_values,
              ElementsAre(1.0e14, 2.0e14, 3.0e14, 4.0e14));
  EXPECT_TRUE(
      def_res.variables["C2H2O2_column_number_density_uncertainty"].included);
  for (double lon : def_res.variables["longitude_bounds"].double_values) {
    EXPECT_GE(lon, -180.0);
    EXPECT_LE(lon, 180.0);
  }

  IngestExerciseResult destr_res =
      ExerciseIngestionModule(mod, b.prod.product(), "destriped=true");
  ASSERT_EQ(HARP_SUCCESS, destr_res.status);
  EXPECT_THAT(destr_res.variables["C2H2O2_column_number_density"].double_values,
              ElementsAre(1.1e14, 2.1e14, 3.1e14, 4.1e14));
  EXPECT_FALSE(
      destr_res.variables["C2H2O2_column_number_density_uncertainty"].included);
}

TEST(HarpIngestOmiL2Test, IngestOmcldo2SuccessAndNotClipped) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("OMI_L2_OMCLDO2");
  ASSERT_NE(nullptr, mod);

  OmiSwathBuilder b("CloudFractionAndPressure");
  ASSERT_TRUE(b.ok());
  ASSERT_TRUE(b.AddBaseGeo2x2());
  ASSERT_TRUE(b.AddAngles2x2());
  ASSERT_TRUE(b.AddSpacecraft2());
  constexpr long kGridDims[] = {2, 2};
  constexpr double kVals[] = {10.0, 20.0, 30.0, 40.0};
  constexpr double kCf[] = {0.0, 0.5, 0.8, 1.0};
  constexpr double kCfNotClipped[] = {-0.1, 0.5, 0.8, 1.2};
  constexpr double kCfPrec[] = {0.05, 0.05, 0.05, 0.05};
  constexpr double kCp[] = {500.0, 600.0, 700.0, 800.0};
  constexpr double kCpPrec[] = {20.0, 25.0, 30.0, 35.0};
  constexpr int32_t kFlags[] = {0, 1, 2, 3};
  ASSERT_TRUE(b.AddDoubleField(b.geo, "TerrainHeight", kGridDims, kVals));
  ASSERT_TRUE(b.AddDoubleField(b.data, "TerrainPressure", kGridDims, kCp));
  ASSERT_TRUE(b.AddDoubleField(b.data, "CloudFraction", kGridDims, kCf));
  ASSERT_TRUE(b.AddDoubleField(b.data, "CloudFractionNotClipped", kGridDims,
                               kCfNotClipped));
  ASSERT_TRUE(
      b.AddDoubleField(b.data, "CloudFractionPrecision", kGridDims, kCfPrec));
  ASSERT_TRUE(b.AddDoubleField(b.data, "CloudPressure", kGridDims, kCp));
  ASSERT_TRUE(
      b.AddDoubleField(b.data, "CloudPressurePrecision", kGridDims, kCpPrec));
  ASSERT_TRUE(
      b.AddInt32Field(b.data, "ProcessingQualityFlags", kGridDims, kFlags));

  IngestExerciseResult def_res = ExerciseIngestionModule(mod, b.prod.product());
  ASSERT_EQ(HARP_SUCCESS, def_res.status);
  EXPECT_THAT(def_res.variables["cloud_fraction"].double_values,
              ElementsAre(0.0, 0.5, 0.8, 1.0));
  EXPECT_THAT(def_res.variables["validity"].int32_values,
              ElementsAre(0, 1, 2, 3));
  EXPECT_THAT(def_res.variables["sensor_latitude"].double_values,
              ElementsAre(20.0, 20.0, 22.0, 22.0));
  EXPECT_THAT(def_res.variables["sensor_longitude"].double_values,
              ElementsAre(10.0, 10.0, 10.5, 10.5));

  IngestExerciseResult raw_res = ExerciseIngestionModule(
      mod, b.prod.product(), "clipped_cloud_fraction=false");
  ASSERT_EQ(HARP_SUCCESS, raw_res.status);
  EXPECT_THAT(raw_res.variables["cloud_fraction"].double_values,
              ElementsAre(-0.1, 0.5, 0.8, 1.2));
}

TEST(HarpIngestOmiL2Test, IngestOmcldrrSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("OMI_L2_OMCLDRR");
  ASSERT_NE(nullptr, mod);

  OmiSwathBuilder b("Cloud_Product");
  ASSERT_TRUE(b.ok());
  ASSERT_TRUE(b.AddBaseGeo2x2());
  constexpr long kGridDims[] = {2, 2};
  constexpr double kSza[] = {25.0, 26.0, 27.0, 28.0};
  constexpr double kVza[] = {10.0, 11.0, 12.0, 13.0};
  constexpr double kRaa[] = {90.0, 91.0, 92.0, 93.0};
  constexpr double kTh[] = {50.0, 60.0, 70.0, 80.0};
  constexpr double kTp[] = {1000.0, 990.0, 980.0, 970.0};
  constexpr double kCf[] = {0.2, 0.3, 0.4, 0.5};
  constexpr double kCp[] = {600.0, 610.0, 620.0, 630.0};
  constexpr int32_t kFlags[] = {4, 5, 6, 7};
  ASSERT_TRUE(b.AddDoubleField(b.geo, "SolarZenithAngle", kGridDims, kSza));
  ASSERT_TRUE(b.AddDoubleField(b.geo, "ViewingZenithAngle", kGridDims, kVza));
  ASSERT_TRUE(b.AddDoubleField(b.geo, "RelativeAzimuthAngle", kGridDims, kRaa));
  ASSERT_TRUE(b.AddDoubleField(b.geo, "TerrainHeight", kGridDims, kTh));
  ASSERT_TRUE(b.AddDoubleField(b.data, "TerrainPressure", kGridDims, kTp));
  ASSERT_TRUE(b.AddDoubleField(b.data, "CloudFractionforO3", kGridDims, kCf));
  ASSERT_TRUE(b.AddDoubleField(b.data, "CloudPressureforO3", kGridDims, kCp));
  ASSERT_TRUE(b.AddInt32Field(b.data, "ProcessingQualityFlagsforO3", kGridDims,
                              kFlags));

  IngestExerciseResult res = ExerciseIngestionModule(mod, b.prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_THAT(res.variables["relative_azimuth_angle"].double_values,
              ElementsAre(90.0, 91.0, 92.0, 93.0));
  EXPECT_THAT(res.variables["cloud_fraction"].double_values,
              ElementsAre(0.2, 0.3, 0.4, 0.5));
  EXPECT_THAT(res.variables["cloud_pressure"].double_values,
              ElementsAre(600.0, 610.0, 620.0, 630.0));
  EXPECT_THAT(res.variables["validity"].int32_values, ElementsAre(4, 5, 6, 7));
}

TEST(HarpIngestOmiL2Test, IngestOmdoao3Success) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("OMI_L2_OMDOAO3");
  ASSERT_NE(nullptr, mod);

  OmiSwathBuilder b("ColumnAmountO3");
  ASSERT_TRUE(b.ok());
  ASSERT_TRUE(b.AddBaseGeo2x2());
  ASSERT_TRUE(b.AddAngles2x2());
  ASSERT_TRUE(b.AddSpacecraft2());
  constexpr long kGridDims[] = {2, 2};
  constexpr double kVals[] = {300.0, 310.0, 320.0, 330.0};
  constexpr double kUnc[] = {3.0, 4.0, 5.0, 6.0};
  constexpr double kCf[] = {0.1, 0.2, 0.3, 0.4};
  constexpr double kCp[] = {700.0, 710.0, 720.0, 730.0};
  constexpr double kCpUnc[] = {10.0, 11.0, 12.0, 13.0};
  constexpr int32_t kFlags[] = {0, 0, 1, 2};
  ASSERT_TRUE(b.AddDoubleField(b.geo, "TerrainHeight", kGridDims, kVals));
  ASSERT_TRUE(b.AddDoubleField(b.data, "TerrainPressure", kGridDims, kCp));
  ASSERT_TRUE(b.AddDoubleField(b.data, "ColumnAmountO3", kGridDims, kVals));
  ASSERT_TRUE(
      b.AddDoubleField(b.data, "ColumnAmountO3Precision", kGridDims, kUnc));
  ASSERT_TRUE(
      b.AddInt32Field(b.data, "ProcessingQualityFlags", kGridDims, kFlags));
  ASSERT_TRUE(b.AddDoubleField(b.data, "CloudFraction", kGridDims, kCf));
  ASSERT_TRUE(b.AddDoubleField(b.data, "CloudPressure", kGridDims, kCp));
  ASSERT_TRUE(
      b.AddDoubleField(b.data, "CloudPressurePrecision", kGridDims, kCpUnc));

  IngestExerciseResult res = ExerciseIngestionModule(mod, b.prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_THAT(res.variables["O3_column_number_density"].double_values,
              ElementsAre(300.0, 310.0, 320.0, 330.0));
  EXPECT_THAT(
      res.variables["O3_column_number_density_uncertainty"].double_values,
      ElementsAre(3.0, 4.0, 5.0, 6.0));
  EXPECT_THAT(res.variables["O3_column_number_density_validity"].int32_values,
              ElementsAre(0, 0, 1, 2));
}

TEST(HarpIngestOmiL2Test, IngestOmdominoSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("OMI_L2_OMDOMINO");
  ASSERT_NE(nullptr, mod);

  OmiSwathBuilder b("DominoNO2");
  ASSERT_TRUE(b.ok());
  ASSERT_TRUE(b.AddBaseGeo2x2());
  ASSERT_TRUE(b.AddAngles2x2());
  constexpr long kCornerDims[] = {4, 2, 2};
  // Layout [4, 2, 2]: corner 0..3 for 4 pixels (0..3).
  constexpr double kLonCorners[] = {
      1.0,  2.0,  3.0,  4.0,   // corner a
      11.0, 12.0, 13.0, 14.0,  // corner b
      21.0, 22.0, 23.0, 24.0,  // corner c
      31.0, 32.0, 33.0, 34.0,  // corner d
  };
  constexpr double kLatCorners[] = {
      -1.0,  -2.0,  -3.0,  -4.0,   // corner a
      -11.0, -12.0, -13.0, -14.0,  // corner b
      -21.0, -22.0, -23.0, -24.0,  // corner c
      -31.0, -32.0, -33.0, -34.0,  // corner d
  };
  ASSERT_TRUE(b.AddDoubleField(b.geo, "LongitudeCornerpoints", kCornerDims,
                               kLonCorners));
  ASSERT_TRUE(b.AddDoubleField(b.geo, "LatitudeCornerpoints", kCornerDims,
                               kLatCorners));

  constexpr long kGridDims[] = {2, 2};
  constexpr double kNo2[] = {1.0e15, 2.0e15, 3.0e15, 4.0e15};
  constexpr double kNo2Err[] = {0.1e15, 0.2e15, 0.3e15, 0.4e15};
  constexpr uint8_t kFlag[] = {0, 1, 255, 2};
  constexpr double kCf[] = {0.1, 0.2, 0.3, 0.4};
  constexpr double kCfStd[] = {0.01, 0.02, 0.03, 0.04};
  constexpr double kCp[] = {500.0, 510.0, 520.0, 530.0};
  constexpr double kCpStd[] = {15.0, 16.0, 17.0, 18.0};
  ASSERT_TRUE(b.AddDoubleField(b.data, "TotalVerticalColumn", kGridDims, kNo2));
  ASSERT_TRUE(
      b.AddDoubleField(b.data, "TotalVerticalColumnError", kGridDims, kNo2Err));
  ASSERT_TRUE(
      b.AddDoubleField(b.data, "TroposphericVerticalColumn", kGridDims, kNo2));
  ASSERT_TRUE(b.AddDoubleField(b.data, "TroposphericVerticalColumnError",
                               kGridDims, kNo2Err));
  ASSERT_TRUE(
      b.AddUint8Field(b.data, "TroposphericColumnFlag", kGridDims, kFlag));
  ASSERT_TRUE(b.AddDoubleField(b.data, "CloudFraction", kGridDims, kCf));
  ASSERT_TRUE(b.AddDoubleField(b.data, "CloudFractionStd", kGridDims, kCfStd));
  ASSERT_TRUE(b.AddDoubleField(b.data, "CloudPressure", kGridDims, kCp));
  ASSERT_TRUE(b.AddDoubleField(b.data, "CloudPressureStd", kGridDims, kCpStd));

  IngestExerciseResult res = ExerciseIngestionModule(mod, b.prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  // Corners are reordered from {a,b,c,d} to {d,b,a,c}.
  EXPECT_THAT(res.variables["longitude_bounds"].double_values,
              ElementsAre(31.0, 11.0, 1.0, 21.0, 32.0, 12.0, 2.0, 22.0, 33.0,
                          13.0, 3.0, 23.0, 34.0, 14.0, 4.0, 24.0));
  EXPECT_THAT(
      res.variables["latitude_bounds"].double_values,
      ElementsAre(-31.0, -11.0, -1.0, -21.0, -32.0, -12.0, -2.0, -22.0, -33.0,
                  -13.0, -3.0, -23.0, -34.0, -14.0, -4.0, -24.0));
  EXPECT_THAT(res.variables["tropospheric_NO2_column_number_density_validity"]
                  .int16_values,
              ElementsAre(0, 1, 255, 2));
}

TEST(HarpIngestOmiL2Test, IngestOmhchoSuccessAndDestriped) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("OMI_L2_OMHCHO");
  ASSERT_NE(nullptr, mod);

  OmiSwathBuilder b("OMI_Total_Column_Amount_HCHO");
  ASSERT_TRUE(b.ok());
  ASSERT_TRUE(b.AddBaseGeo2x2());
  ASSERT_TRUE(b.AddSpacecraft2());
  constexpr long kGridDims[] = {2, 2};
  constexpr double kVals[] = {1.0e16, 2.0e16, 3.0e16, 4.0e16};
  constexpr double kDestr[] = {1.2e16, 2.2e16, 3.2e16, 4.2e16};
  constexpr double kUnc[] = {0.1e16, 0.2e16, 0.3e16, 0.4e16};
  ASSERT_TRUE(b.AddDoubleField(b.geo, "TerrainHeight", kGridDims, kVals));
  ASSERT_TRUE(b.AddDoubleField(b.data, "ColumnAmount", kGridDims, kVals));
  ASSERT_TRUE(
      b.AddDoubleField(b.data, "ColumnAmountDestriped", kGridDims, kDestr));
  ASSERT_TRUE(b.AddDoubleField(b.data, "ColumnUncertainty", kGridDims, kUnc));

  IngestExerciseResult def_res = ExerciseIngestionModule(mod, b.prod.product());
  ASSERT_EQ(HARP_SUCCESS, def_res.status);
  EXPECT_THAT(def_res.variables["HCHO_column_number_density"].double_values,
              ElementsAre(1.0e16, 2.0e16, 3.0e16, 4.0e16));
  EXPECT_TRUE(
      def_res.variables["HCHO_column_number_density_uncertainty"].included);

  IngestExerciseResult destr_res =
      ExerciseIngestionModule(mod, b.prod.product(), "destriped=true");
  ASSERT_EQ(HARP_SUCCESS, destr_res.status);
  EXPECT_THAT(destr_res.variables["HCHO_column_number_density"].double_values,
              ElementsAre(1.2e16, 2.2e16, 3.2e16, 4.2e16));
  EXPECT_FALSE(
      destr_res.variables["HCHO_column_number_density_uncertainty"].included);
}

TEST(HarpIngestOmiL2Test, IngestOmno2FullAndOptionalOmitted) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("OMI_L2_OMNO2");
  ASSERT_NE(nullptr, mod);

  // 1. Minimal product without optional fields (exercises include_* == 0).
  {
    OmiSwathBuilder b("ColumnAmountNO2");
    ASSERT_TRUE(b.ok());
    ASSERT_TRUE(b.AddBaseGeo2x2());
    ASSERT_TRUE(b.AddAngles2x2());
    ASSERT_TRUE(b.AddSpacecraft2());
    constexpr long kGridDims[] = {2, 2};
    constexpr double kVals[] = {1.0, 2.0, 3.0, 4.0};
    ASSERT_TRUE(b.AddDoubleField(b.data, "TerrainHeight", kGridDims, kVals));
    ASSERT_TRUE(b.AddDoubleField(b.data, "TerrainPressure", kGridDims, kVals));
    ASSERT_TRUE(b.AddDoubleField(b.data, "ColumnAmountNO2", kGridDims, kVals));
    ASSERT_TRUE(
        b.AddDoubleField(b.data, "ColumnAmountNO2Std", kGridDims, kVals));
    ASSERT_TRUE(
        b.AddDoubleField(b.data, "ColumnAmountNO2Trop", kGridDims, kVals));
    ASSERT_TRUE(
        b.AddDoubleField(b.data, "ColumnAmountNO2TropStd", kGridDims, kVals));
    ASSERT_TRUE(
        b.AddDoubleField(b.data, "SlantColumnAmountNO2", kGridDims, kVals));
    ASSERT_TRUE(
        b.AddDoubleField(b.data, "SlantColumnAmountNO2Std", kGridDims, kVals));
    ASSERT_TRUE(b.AddDoubleField(b.data, "CloudFraction", kGridDims, kVals));
    ASSERT_TRUE(b.AddDoubleField(b.data, "CloudFractionStd", kGridDims, kVals));
    ASSERT_TRUE(b.AddDoubleField(b.data, "CloudPressure", kGridDims, kVals));
    ASSERT_TRUE(b.AddDoubleField(b.data, "CloudPressureStd", kGridDims, kVals));

    IngestExerciseResult res = ExerciseIngestionModule(mod, b.prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_FALSE(
        res.variables["tropospheric_NO2_column_number_density_amf"].included);
    EXPECT_FALSE(res.variables["tropospheric_NO2_column_number_density_apriori"]
                     .included);
    EXPECT_FALSE(
        res.variables["stratospheric_NO2_column_number_density"].included);
    EXPECT_FALSE(
        res.variables["stratospheric_NO2_column_number_density_amf"].included);
    EXPECT_FALSE(
        res.variables["stratospheric_NO2_column_number_density_apriori"]
            .included);
    EXPECT_FALSE(res.variables["validity"].included);
    EXPECT_FALSE(res.variables["tropopause_pressure"].included);
  }

  // 2. Full product with all optional fields and destriped=true.
  {
    OmiSwathBuilder b("ColumnAmountNO2");
    ASSERT_TRUE(b.ok());
    ASSERT_TRUE(b.AddBaseGeo2x2());
    ASSERT_TRUE(b.AddAngles2x2());
    ASSERT_TRUE(b.AddSpacecraft2());
    constexpr long kGridDims[] = {2, 2};
    constexpr double kVals[] = {10.0, 20.0, 30.0, 40.0};
    constexpr int32_t kFlags[] = {0, 1, 0, 1};
    ASSERT_TRUE(b.AddDoubleField(b.data, "TerrainHeight", kGridDims, kVals));
    ASSERT_TRUE(b.AddDoubleField(b.data, "TerrainPressure", kGridDims, kVals));
    ASSERT_TRUE(b.AddDoubleField(b.data, "ColumnAmountNO2", kGridDims, kVals));
    ASSERT_TRUE(
        b.AddDoubleField(b.data, "ColumnAmountNO2Std", kGridDims, kVals));
    ASSERT_TRUE(
        b.AddDoubleField(b.data, "ColumnAmountNO2Trop", kGridDims, kVals));
    ASSERT_TRUE(
        b.AddDoubleField(b.data, "ColumnAmountNO2TropStd", kGridDims, kVals));
    ASSERT_TRUE(b.AddDoubleField(b.data, "AmfTrop", kGridDims, kVals));
    ASSERT_TRUE(b.AddDoubleField(b.data, "VcdApTrop", kGridDims, kVals));
    ASSERT_TRUE(
        b.AddDoubleField(b.data, "ColumnAmountNO2Strat", kGridDims, kVals));
    ASSERT_TRUE(
        b.AddDoubleField(b.data, "ColumnAmountNO2StratStd", kGridDims, kVals));
    ASSERT_TRUE(b.AddDoubleField(b.data, "AmfStrat", kGridDims, kVals));
    ASSERT_TRUE(b.AddDoubleField(b.data, "VcdApStrat", kGridDims, kVals));
    ASSERT_TRUE(
        b.AddDoubleField(b.data, "SlantColumnAmountNO2", kGridDims, kVals));
    ASSERT_TRUE(b.AddDoubleField(b.data, "SlantColumnAmountNO2Destriped",
                                 kGridDims, kVals));
    ASSERT_TRUE(
        b.AddDoubleField(b.data, "SlantColumnAmountNO2Std", kGridDims, kVals));
    ASSERT_TRUE(b.AddInt32Field(b.data, "VcdQualityFlags", kGridDims, kFlags));
    ASSERT_TRUE(
        b.AddDoubleField(b.data, "TropopausePressure", kGridDims, kVals));
    ASSERT_TRUE(b.AddDoubleField(b.data, "CloudFraction", kGridDims, kVals));
    ASSERT_TRUE(b.AddDoubleField(b.data, "CloudFractionStd", kGridDims, kVals));
    ASSERT_TRUE(b.AddDoubleField(b.data, "CloudPressure", kGridDims, kVals));
    ASSERT_TRUE(b.AddDoubleField(b.data, "CloudPressureStd", kGridDims, kVals));

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, b.prod.product(), "destriped=true");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_TRUE(
        res.variables["stratospheric_NO2_column_number_density"].included);
    EXPECT_THAT(
        res.variables["stratospheric_NO2_column_number_density"].double_values,
        ElementsAre(10.0, 20.0, 30.0, 40.0));
    EXPECT_THAT(res.variables["validity"].int32_values,
                ElementsAre(0, 1, 0, 1));
    EXPECT_THAT(res.variables["tropopause_pressure"].double_values,
                ElementsAre(10.0, 20.0, 30.0, 40.0));
  }
}

TEST(HarpIngestOmiL2Test, IngestOmo3prSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("OMI_L2_OMO3PR");
  ASSERT_NE(nullptr, mod);

  OmiSwathBuilder b("O3Profile");
  ASSERT_TRUE(b.ok());
  ASSERT_TRUE(b.AddBaseGeo2x2());
  ASSERT_TRUE(b.AddAngles2x2());
  ASSERT_TRUE(b.AddSpacecraft2());
  constexpr long kGridDims[] = {2, 2};
  constexpr double kTh[] = {10.0, 20.0, 30.0, 40.0};
  ASSERT_TRUE(b.AddDoubleField(b.geo, "TerrainHeight", kGridDims, kTh));

  // 2 layers -> Pressure has 3 levels (dim[2] = num_layers + 1).
  constexpr long kPressureDims[] = {2, 2, 3};
  constexpr double kPressure[] = {
      1000.0, 500.0, 100.0, 1001.0, 501.0, 101.0,
      1002.0, 502.0, 102.0, 1003.0, 503.0, 103.0,
  };
  ASSERT_TRUE(b.AddDoubleField(b.geo, "Pressure", kPressureDims, kPressure));

  constexpr long kO3Dims[] = {2, 2, 2};
  constexpr double kO3[] = {10.0, 20.0, 11.0, 21.0, 12.0, 22.0, 13.0, 23.0};
  constexpr double kO3Unc[] = {1.0, 2.0, 1.1, 2.1, 1.2, 2.2, 1.3, 2.3};
  constexpr int32_t kFlags[] = {0, 1, 2, 3};
  ASSERT_TRUE(b.AddDoubleField(b.data, "O3", kO3Dims, kO3));
  ASSERT_TRUE(b.AddDoubleField(b.data, "O3Precision", kO3Dims, kO3Unc));
  ASSERT_TRUE(
      b.AddInt32Field(b.data, "ProcessingQualityFlags", kGridDims, kFlags));

  IngestExerciseResult res = ExerciseIngestionModule(mod, b.prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(4L, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(2L, res.dimensions[harp_dimension_vertical]);
  EXPECT_THAT(
      res.variables["pressure"].double_values,
      ElementsAre(1000.0, 500.0, 1001.0, 501.0, 1002.0, 502.0, 1003.0, 503.0));
  EXPECT_THAT(res.variables["O3_number_density"].double_values,
              ElementsAre(10.0, 20.0, 11.0, 21.0, 12.0, 22.0, 13.0, 23.0));
  EXPECT_THAT(res.variables["O3_number_density_uncertainty"].double_values,
              ElementsAre(1.0, 2.0, 1.1, 2.1, 1.2, 2.2, 1.3, 2.3));
  EXPECT_THAT(res.variables["O3_number_density_validity"].int32_values,
              ElementsAre(0, 1, 2, 3));
}

TEST(HarpIngestOmiL2Test, IngestOmocloSuccessAndDestriped) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("OMI_L2_OMOCLO");
  ASSERT_NE(nullptr, mod);

  OmiSwathBuilder b("OMI_Total_Column_Amount_OClO");
  ASSERT_TRUE(b.ok());
  ASSERT_TRUE(b.AddBaseGeo2x2());
  ASSERT_TRUE(b.AddSpacecraft2());
  constexpr long kGridDims[] = {2, 2};
  constexpr double kVals[] = {1.0e13, 2.0e13, 3.0e13, 4.0e13};
  constexpr double kDestr[] = {1.3e13, 2.3e13, 3.3e13, 4.3e13};
  constexpr double kUnc[] = {0.1e13, 0.2e13, 0.3e13, 0.4e13};
  ASSERT_TRUE(b.AddDoubleField(b.geo, "TerrainHeight", kGridDims, kVals));
  ASSERT_TRUE(b.AddDoubleField(b.data, "ColumnAmount", kGridDims, kVals));
  ASSERT_TRUE(
      b.AddDoubleField(b.data, "ColumnAmountDestriped", kGridDims, kDestr));
  ASSERT_TRUE(b.AddDoubleField(b.data, "ColumnUncertainty", kGridDims, kUnc));

  IngestExerciseResult def_res = ExerciseIngestionModule(mod, b.prod.product());
  ASSERT_EQ(HARP_SUCCESS, def_res.status);
  EXPECT_THAT(def_res.variables["OClO_column_number_density"].double_values,
              ElementsAre(1.0e13, 2.0e13, 3.0e13, 4.0e13));
  EXPECT_TRUE(
      def_res.variables["OClO_column_number_density_uncertainty"].included);

  IngestExerciseResult destr_res =
      ExerciseIngestionModule(mod, b.prod.product(), "destriped=true");
  ASSERT_EQ(HARP_SUCCESS, destr_res.status);
  EXPECT_THAT(destr_res.variables["OClO_column_number_density"].double_values,
              ElementsAre(1.3e13, 2.3e13, 3.3e13, 4.3e13));
  EXPECT_FALSE(
      destr_res.variables["OClO_column_number_density_uncertainty"].included);
}

TEST(HarpIngestOmiL2Test, IngestOmso2V2AndV3Options) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("OMI_L2_OMSO2");
  ASSERT_NE(nullptr, mod);

  // 1. V2 product (SO2ColumnAmountPBL + CloudTopPressure).
  {
    OmiSwathBuilder b("OMI_Total_Column_Amount_SO2");
    ASSERT_TRUE(b.ok());
    ASSERT_TRUE(b.AddBaseGeo2x2());
    ASSERT_TRUE(b.AddAngles2x2());
    ASSERT_TRUE(b.AddSpacecraft2());
    constexpr long kGridDims[] = {2, 2};
    constexpr double kPbl[] = {1.0, 2.0, 3.0, 4.0};
    constexpr double k5km[] = {5.0, 6.0, 7.0, 8.0};
    constexpr double k15km[] = {9.0, 10.0, 11.0, 12.0};
    constexpr double kMisc[] = {0.2, 0.3, 0.4, 0.5};
    ASSERT_TRUE(b.AddDoubleField(b.geo, "TerrainHeight", kGridDims, kMisc));
    ASSERT_TRUE(b.AddDoubleField(b.data, "TerrainPressure", kGridDims, kMisc));
    ASSERT_TRUE(
        b.AddDoubleField(b.data, "SO2ColumnAmountPBL", kGridDims, kPbl));
    ASSERT_TRUE(
        b.AddDoubleField(b.data, "SO2ColumnAmount05KM", kGridDims, k5km));
    ASSERT_TRUE(
        b.AddDoubleField(b.data, "SO2ColumnAmount15KM", kGridDims, k15km));
    ASSERT_TRUE(b.AddDoubleField(b.data, "CloudFraction", kGridDims, kMisc));
    ASSERT_TRUE(b.AddDoubleField(b.data, "CloudTopPressure", kGridDims, kMisc));

    struct V2Case {
      const char* opt;
      std::array<double, 4> expected;
    };
    constexpr V2Case kV2Cases[] = {
        {nullptr, {1.0, 2.0, 3.0, 4.0}},
        {"so2_column_variant=pbl", {1.0, 2.0, 3.0, 4.0}},
        {"so2_column_variant=5km", {5.0, 6.0, 7.0, 8.0}},
        {"so2_column_variant=15km", {9.0, 10.0, 11.0, 12.0}},
    };
    for (const auto& tc : kV2Cases) {
      IngestExerciseResult res =
          ExerciseIngestionModule(mod, b.prod.product(), tc.opt);
      ASSERT_EQ(HARP_SUCCESS, res.status);
      EXPECT_THAT(res.variables["SO2_column_number_density"].double_values,
                  ElementsAre(tc.expected[0], tc.expected[1], tc.expected[2],
                              tc.expected[3]));
      EXPECT_TRUE(res.variables["cloud_top_pressure"].included);
      EXPECT_FALSE(res.variables["cloud_pressure"].included);
    }

    // Invalid V3 option on V2 product.
    IngestExerciseResult bad_res = ExerciseIngestionModule(
        mod, b.prod.product(), "so2_column_variant=trl");
    EXPECT_EQ(-1, bad_res.status);
    EXPECT_EQ(HARP_ERROR_INVALID_INGESTION_OPTION_VALUE, bad_res.failed_errno);
  }

  // 2. V3 product (ColumnAmountSO2_* + CloudPressure).
  {
    OmiSwathBuilder b("OMI_Total_Column_Amount_SO2");
    ASSERT_TRUE(b.ok());
    ASSERT_TRUE(b.AddBaseGeo2x2());
    ASSERT_TRUE(b.AddAngles2x2());
    ASSERT_TRUE(b.AddSpacecraft2());
    constexpr long kGridDims[] = {2, 2};
    constexpr double kPbl[] = {11.0, 12.0, 13.0, 14.0};
    constexpr double kTrl[] = {21.0, 22.0, 23.0, 24.0};
    constexpr double kTrm[] = {31.0, 32.0, 33.0, 34.0};
    constexpr double kStl[] = {41.0, 42.0, 43.0, 44.0};
    constexpr double kMisc[] = {0.2, 0.3, 0.4, 0.5};
    ASSERT_TRUE(b.AddDoubleField(b.geo, "TerrainHeight", kGridDims, kMisc));
    ASSERT_TRUE(b.AddDoubleField(b.data, "TerrainPressure", kGridDims, kMisc));
    ASSERT_TRUE(
        b.AddDoubleField(b.data, "ColumnAmountSO2_PBL", kGridDims, kPbl));
    ASSERT_TRUE(
        b.AddDoubleField(b.data, "ColumnAmountSO2_TRL", kGridDims, kTrl));
    ASSERT_TRUE(
        b.AddDoubleField(b.data, "ColumnAmountSO2_TRM", kGridDims, kTrm));
    ASSERT_TRUE(
        b.AddDoubleField(b.data, "ColumnAmountSO2_STL", kGridDims, kStl));
    ASSERT_TRUE(b.AddDoubleField(b.data, "CloudFraction", kGridDims, kMisc));
    ASSERT_TRUE(b.AddDoubleField(b.data, "CloudPressure", kGridDims, kMisc));

    struct V3Case {
      const char* opt;
      std::array<double, 4> expected;
    };
    constexpr V3Case kV3Cases[] = {
        {nullptr, {11.0, 12.0, 13.0, 14.0}},
        {"so2_column_variant=pbl", {11.0, 12.0, 13.0, 14.0}},
        {"so2_column_variant=trl", {21.0, 22.0, 23.0, 24.0}},
        {"so2_column_variant=trm", {31.0, 32.0, 33.0, 34.0}},
        {"so2_column_variant=stl", {41.0, 42.0, 43.0, 44.0}},
    };
    for (const auto& tc : kV3Cases) {
      IngestExerciseResult res =
          ExerciseIngestionModule(mod, b.prod.product(), tc.opt);
      ASSERT_EQ(HARP_SUCCESS, res.status);
      EXPECT_THAT(res.variables["SO2_column_number_density"].double_values,
                  ElementsAre(tc.expected[0], tc.expected[1], tc.expected[2],
                              tc.expected[3]));
      EXPECT_TRUE(res.variables["cloud_pressure"].included);
      EXPECT_FALSE(res.variables["cloud_top_pressure"].included);
    }

    // Invalid V2 option on V3 product.
    IngestExerciseResult bad_res = ExerciseIngestionModule(
        mod, b.prod.product(), "so2_column_variant=5km");
    EXPECT_EQ(-1, bad_res.status);
    EXPECT_EQ(HARP_ERROR_INVALID_INGESTION_OPTION_VALUE, bad_res.failed_errno);
  }
}

TEST(HarpIngestOmiL2Test, IngestOmto3V2AndV3Options) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("OMI_L2_OMTO3");
  ASSERT_NE(nullptr, mod);

  // 1. V2 product (CloudFraction + CloudTopPressure, no
  // RadiativeCloudFraction).
  {
    OmiSwathBuilder b("OMI_Column_Amount_O3");
    ASSERT_TRUE(b.ok());
    ASSERT_TRUE(b.AddBaseGeo2x2());
    ASSERT_TRUE(b.AddAngles2x2());
    ASSERT_TRUE(b.AddSpacecraft2());
    constexpr long kGridDims[] = {2, 2};
    constexpr double kO3[] = {290.0, 295.0, 300.0, 305.0};
    constexpr int32_t kFlags[] = {0, 0, 1, 1};
    constexpr double kCf[] = {0.25, 0.35, 0.45, 0.55};
    constexpr double kCtp[] = {400.0, 410.0, 420.0, 430.0};
    ASSERT_TRUE(b.AddDoubleField(b.data, "ColumnAmountO3", kGridDims, kO3));
    ASSERT_TRUE(b.AddInt32Field(b.data, "QualityFlags", kGridDims, kFlags));
    ASSERT_TRUE(b.AddDoubleField(b.data, "CloudFraction", kGridDims, kCf));
    ASSERT_TRUE(b.AddDoubleField(b.data, "CloudTopPressure", kGridDims, kCtp));

    IngestExerciseResult eff_res =
        ExerciseIngestionModule(mod, b.prod.product());
    ASSERT_EQ(HARP_SUCCESS, eff_res.status);
    EXPECT_TRUE(eff_res.variables["cloud_fraction"].included);
    EXPECT_THAT(eff_res.variables["cloud_fraction"].double_values,
                ElementsAre(0.25, 0.35, 0.45, 0.55));
    EXPECT_TRUE(eff_res.variables["cloud_top_pressure"].included);
    EXPECT_FALSE(eff_res.variables["cloud_pressure"].included);

    IngestExerciseResult rad_res = ExerciseIngestionModule(
        mod, b.prod.product(), "cloud_fraction_variant=radiative");
    ASSERT_EQ(HARP_SUCCESS, rad_res.status);
    EXPECT_FALSE(rad_res.variables["cloud_fraction"].included);
  }

  // 2. V3 product (fc + RadiativeCloudFraction + CloudPressure).
  {
    OmiSwathBuilder b("OMI_Column_Amount_O3");
    ASSERT_TRUE(b.ok());
    ASSERT_TRUE(b.AddBaseGeo2x2());
    ASSERT_TRUE(b.AddAngles2x2());
    ASSERT_TRUE(b.AddSpacecraft2());
    constexpr long kGridDims[] = {2, 2};
    constexpr double kO3[] = {310.0, 315.0, 320.0, 325.0};
    constexpr int32_t kFlags[] = {1, 1, 2, 2};
    constexpr double kFc[] = {0.1, 0.2, 0.3, 0.4};
    constexpr double kRadCf[] = {0.15, 0.25, 0.35, 0.45};
    constexpr double kCp[] = {650.0, 660.0, 670.0, 680.0};
    ASSERT_TRUE(b.AddDoubleField(b.data, "ColumnAmountO3", kGridDims, kO3));
    ASSERT_TRUE(b.AddInt32Field(b.data, "QualityFlags", kGridDims, kFlags));
    ASSERT_TRUE(b.AddDoubleField(b.data, "fc", kGridDims, kFc));
    ASSERT_TRUE(
        b.AddDoubleField(b.data, "RadiativeCloudFraction", kGridDims, kRadCf));
    ASSERT_TRUE(b.AddDoubleField(b.data, "CloudPressure", kGridDims, kCp));

    IngestExerciseResult eff_res = ExerciseIngestionModule(
        mod, b.prod.product(), "cloud_fraction_variant=effective");
    ASSERT_EQ(HARP_SUCCESS, eff_res.status);
    EXPECT_THAT(eff_res.variables["cloud_fraction"].double_values,
                ElementsAre(0.1, 0.2, 0.3, 0.4));
    EXPECT_TRUE(eff_res.variables["cloud_pressure"].included);
    EXPECT_FALSE(eff_res.variables["cloud_top_pressure"].included);

    IngestExerciseResult rad_res = ExerciseIngestionModule(
        mod, b.prod.product(), "cloud_fraction_variant=radiative");
    ASSERT_EQ(HARP_SUCCESS, rad_res.status);
    EXPECT_TRUE(rad_res.variables["cloud_fraction"].included);
    EXPECT_THAT(rad_res.variables["cloud_fraction"].double_values,
                ElementsAre(0.15, 0.25, 0.35, 0.45));
  }
}

TEST(HarpIngestOmiL2Test, IngestOmuvbAllWavelengthsAndClearSky) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("OMI_L2_OMUVB");
  ASSERT_NE(nullptr, mod);

  OmiSwathBuilder b("UVB");
  ASSERT_TRUE(b.ok());
  ASSERT_TRUE(b.AddBaseGeo2x2());
  constexpr long kGridDims[] = {2, 2};
  constexpr double kSza[] = {40.0, 41.0, 42.0, 43.0};
  constexpr double kTh[] = {100.0, 110.0, 120.0, 130.0};
  constexpr double k305[] = {1.0, 2.0, 3.0, 4.0};
  constexpr double k310[] = {11.0, 12.0, 13.0, 14.0};
  constexpr double k324[] = {21.0, 22.0, 23.0, 24.0};
  constexpr double k380[] = {31.0, 32.0, 33.0, 34.0};
  constexpr double kCs305[] = {101.0, 102.0, 103.0, 104.0};
  constexpr double kCs310[] = {111.0, 112.0, 113.0, 114.0};
  constexpr double kCs324[] = {121.0, 122.0, 123.0, 124.0};
  constexpr double kCs380[] = {131.0, 132.0, 133.0, 134.0};
  ASSERT_TRUE(b.AddDoubleField(b.geo, "SolarZenithAngle", kGridDims, kSza));
  ASSERT_TRUE(b.AddDoubleField(b.geo, "TerrainHeight", kGridDims, kTh));
  ASSERT_TRUE(b.AddDoubleField(b.data, "Irradiance305", kGridDims, k305));
  ASSERT_TRUE(b.AddDoubleField(b.data, "Irradiance310", kGridDims, k310));
  ASSERT_TRUE(b.AddDoubleField(b.data, "Irradiance324", kGridDims, k324));
  ASSERT_TRUE(b.AddDoubleField(b.data, "Irradiance380", kGridDims, k380));
  ASSERT_TRUE(b.AddDoubleField(b.data, "CSIrradiance305", kGridDims, kCs305));
  ASSERT_TRUE(b.AddDoubleField(b.data, "CSIrradiance310", kGridDims, kCs310));
  ASSERT_TRUE(b.AddDoubleField(b.data, "CSIrradiance324", kGridDims, kCs324));
  ASSERT_TRUE(b.AddDoubleField(b.data, "CSIrradiance380", kGridDims, kCs380));

  struct UvbCase {
    const char* opt;
    std::array<double, 4> expected;
  };
  constexpr UvbCase kCases[] = {
      {nullptr, {1.0, 2.0, 3.0, 4.0}},
      {"wavelength=305nm", {1.0, 2.0, 3.0, 4.0}},
      {"wavelength=310nm", {11.0, 12.0, 13.0, 14.0}},
      {"wavelength=324nm", {21.0, 22.0, 23.0, 24.0}},
      {"wavelength=380nm", {31.0, 32.0, 33.0, 34.0}},
      {"clear_sky=true", {101.0, 102.0, 103.0, 104.0}},
      {"clear_sky=true;wavelength=305nm", {101.0, 102.0, 103.0, 104.0}},
      {"clear_sky=true;wavelength=310nm", {111.0, 112.0, 113.0, 114.0}},
      {"clear_sky=true;wavelength=324nm", {121.0, 122.0, 123.0, 124.0}},
      {"clear_sky=true;wavelength=380nm", {131.0, 132.0, 133.0, 134.0}},
  };
  for (const auto& tc : kCases) {
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, b.prod.product(), tc.opt);
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["surface_irradiance"].double_values,
                ElementsAre(tc.expected[0], tc.expected[1], tc.expected[2],
                            tc.expected[3]));
  }
}

TEST(HarpIngestOmiL2Test, ReadLatitudeBoundsDirectlyCoversLazyInit) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("OMI_L2_OMBRO");
  ASSERT_NE(nullptr, mod);

  // 1. Direct read of latitude_bounds without reading longitude_bounds first.
  {
    OmiSwathBuilder b("OMI_Total_Column_Amount_BRO");
    ASSERT_TRUE(b.ok());
    ASSERT_TRUE(b.AddBaseGeo2x2());
    std::vector<double> lat_bounds;
    ASSERT_EQ(0, ReadSingleVariableDirect(mod, b.prod.product(),
                                          "latitude_bounds", &lat_bounds));
    ASSERT_EQ(16u, lat_bounds.size());
    EXPECT_THAT(lat_bounds[0], DoubleNear(19.0, 0.5));
  }

  // 2. Direct read of latitude_bounds fails when Longitude is missing.
  {
    OmiSwathBuilder b("OMI_Total_Column_Amount_BRO");
    ASSERT_TRUE(b.ok());
    constexpr long kGridDims[] = {2, 2};
    constexpr double kLat[] = {20.0, 20.0, 22.0, 22.0};
    ASSERT_TRUE(b.AddDoubleField(b.geo, "Latitude", kGridDims, kLat));
    std::vector<double> lat_bounds;
    EXPECT_EQ(-1, ReadSingleVariableDirect(mod, b.prod.product(),
                                           "latitude_bounds", &lat_bounds));
  }
}

TEST(HarpIngestOmiL2Test, InitCursorErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("OMI_L2_OMBRO");
  ASSERT_NE(nullptr, mod);

  // 1. Missing HDFEOS.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. Missing SWATHS.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "HDFEOS", prod.MakeRecord()));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. Empty SWATHS (no first record field).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* hdfeos = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(hdfeos, "SWATHS", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(prod.root(), "HDFEOS", hdfeos));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 4. Missing Data_Fields.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* hdfeos = prod.MakeRecord();
    CodaMemNode* swaths = prod.MakeRecord();
    CodaMemNode* swath = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(swath, "Geolocation_Fields", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(swaths, "Swath", swath));
    ASSERT_TRUE(prod.AddField(hdfeos, "SWATHS", swaths));
    ASSERT_TRUE(prod.AddField(prod.root(), "HDFEOS", hdfeos));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 5. Missing Geolocation_Fields.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* hdfeos = prod.MakeRecord();
    CodaMemNode* swaths = prod.MakeRecord();
    CodaMemNode* swath = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(swath, "Data_Fields", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(swaths, "Swath", swath));
    ASSERT_TRUE(prod.AddField(hdfeos, "SWATHS", swaths));
    ASSERT_TRUE(prod.AddField(prod.root(), "HDFEOS", hdfeos));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
}

TEST(HarpIngestOmiL2Test, DimensionAndInitErrorsAcrossModules) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* ombro = FindIngestionModule("OMI_L2_OMBRO");
  const harp_ingestion_module* omaeruv = FindIngestionModule("OMI_L2_OMAERUV");
  const harp_ingestion_module* omo3pr = FindIngestionModule("OMI_L2_OMO3PR");
  const harp_ingestion_module* omdomino =
      FindIngestionModule("OMI_L2_OMDOMINO");
  ASSERT_NE(nullptr, ombro);
  ASSERT_NE(nullptr, omaeruv);
  ASSERT_NE(nullptr, omo3pr);
  ASSERT_NE(nullptr, omdomino);

  // 1. Standard init_dimensions / verify_dimensions failures.
  {
    OmiSwathBuilder b("S");
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(-1, ExerciseIngestionModule(ombro, b.prod.product()).status);
  }
  {
    OmiSwathBuilder b("S");
    ASSERT_TRUE(b.ok());
    ASSERT_TRUE(b.prod.AddField(b.geo, "Latitude", b.prod.MakeDouble(1.0)));
    EXPECT_EQ(-1, ExerciseIngestionModule(ombro, b.prod.product()).status);
  }
  {
    OmiSwathBuilder b("S");
    ASSERT_TRUE(b.ok());
    constexpr long kDims1D[] = {2};
    constexpr double kVals[] = {1.0, 2.0};
    ASSERT_TRUE(b.AddDoubleField(b.geo, "Latitude", kDims1D, kVals));
    IngestExerciseResult res = ExerciseIngestionModule(ombro, b.prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    OmiSwathBuilder b("S");
    ASSERT_TRUE(b.ok());
    constexpr long kTime1[] = {1, 2};
    constexpr double kVals[] = {1.0, 2.0};
    ASSERT_TRUE(b.AddDoubleField(b.geo, "Latitude", kTime1, kVals));
    IngestExerciseResult res = ExerciseIngestionModule(ombro, b.prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    OmiSwathBuilder b("S");
    ASSERT_TRUE(b.ok());
    constexpr long kXtrack1[] = {2, 1};
    constexpr double kVals[] = {1.0, 2.0};
    ASSERT_TRUE(b.AddDoubleField(b.geo, "Latitude", kXtrack1, kVals));
    IngestExerciseResult res = ExerciseIngestionModule(ombro, b.prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 2. OMDOMINO init failures (bad cursors and time==1).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    EXPECT_EQ(-1, ExerciseIngestionModule(omdomino, prod.product()).status);
  }
  {
    OmiSwathBuilder b("DominoNO2");
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(-1, ExerciseIngestionModule(omdomino, b.prod.product()).status);
  }
  {
    OmiSwathBuilder b("DominoNO2");
    ASSERT_TRUE(b.ok());
    constexpr long kTime1[] = {1, 2};
    constexpr double kVals[] = {1.0, 2.0};
    ASSERT_TRUE(b.AddDoubleField(b.geo, "Latitude", kTime1, kVals));
    EXPECT_EQ(-1, ExerciseIngestionModule(omdomino, b.prod.product()).status);
  }

  // 3. OMAERUV init failures.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    EXPECT_EQ(-1, ExerciseIngestionModule(omaeruv, prod.product()).status);
  }
  {
    OmiSwathBuilder b("Aerosol_NearUV_Swath");
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(-1, ExerciseIngestionModule(omaeruv, b.prod.product()).status);
  }
  {
    OmiSwathBuilder b("Aerosol_NearUV_Swath");
    ASSERT_TRUE(b.ok());
    ASSERT_TRUE(b.prod.AddField(b.data, "FinalAerosolOpticalDepth",
                                b.prod.MakeDouble(1.0)));
    EXPECT_EQ(-1, ExerciseIngestionModule(omaeruv, b.prod.product()).status);
  }
  {
    OmiSwathBuilder b("Aerosol_NearUV_Swath");
    ASSERT_TRUE(b.ok());
    constexpr long kDims2D[] = {2, 2};
    constexpr double kVals[] = {1.0, 2.0, 3.0, 4.0};
    ASSERT_TRUE(
        b.AddDoubleField(b.data, "FinalAerosolOpticalDepth", kDims2D, kVals));
    EXPECT_EQ(-1, ExerciseIngestionModule(omaeruv, b.prod.product()).status);
  }
  {
    OmiSwathBuilder b("Aerosol_NearUV_Swath");
    ASSERT_TRUE(b.ok());
    constexpr long kBadTime[] = {1, 2, 3};
    constexpr double kVals[] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
    ASSERT_TRUE(
        b.AddDoubleField(b.data, "FinalAerosolOpticalDepth", kBadTime, kVals));
    EXPECT_EQ(-1, ExerciseIngestionModule(omaeruv, b.prod.product()).status);
  }
  {
    OmiSwathBuilder b("Aerosol_NearUV_Swath");
    ASSERT_TRUE(b.ok());
    constexpr long kBadSpec[] = {2, 2, 2};
    constexpr double kVals[] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
    ASSERT_TRUE(
        b.AddDoubleField(b.data, "FinalAerosolOpticalDepth", kBadSpec, kVals));
    EXPECT_EQ(-1, ExerciseIngestionModule(omaeruv, b.prod.product()).status);
  }
  {
    // Missing FinalAerosolAbsOpticalDepth fails variable_descriptor_init.
    OmiSwathBuilder b("Aerosol_NearUV_Swath");
    ASSERT_TRUE(b.ok());
    constexpr long kDims[] = {2, 2, 3};
    constexpr double kVals[12] = {};
    ASSERT_TRUE(
        b.AddDoubleField(b.data, "FinalAerosolOpticalDepth", kDims, kVals));
    EXPECT_EQ(-1, ExerciseIngestionModule(omaeruv, b.prod.product()).status);
  }
  {
    // FinalAerosolOpticalDepth missing MissingValue fails
    // variable_descriptor_init.
    OmiSwathBuilder b("Aerosol_NearUV_Swath");
    ASSERT_TRUE(b.ok());
    constexpr long kDims[] = {2, 2, 3};
    constexpr double kVals[12] = {};
    ASSERT_TRUE(b.prod.AddField(b.data, "FinalAerosolOpticalDepth",
                                b.prod.MakeDoubleArray(kDims, kVals)));
    EXPECT_EQ(-1, ExerciseIngestionModule(omaeruv, b.prod.product()).status);
  }
  {
    // FinalAerosolAbsOpticalDepth wrong dim fails variable_descriptor_init.
    OmiSwathBuilder b("Aerosol_NearUV_Swath");
    ASSERT_TRUE(b.ok());
    constexpr long kDims[] = {2, 2, 3};
    constexpr long kBadDims[] = {2, 2, 2};
    constexpr double kVals[12] = {};
    ASSERT_TRUE(
        b.AddDoubleField(b.data, "FinalAerosolOpticalDepth", kDims, kVals));
    ASSERT_TRUE(b.AddDoubleField(b.data, "FinalAerosolAbsOpticalDepth",
                                 kBadDims, std::span<const double>(kVals, 8)));
    EXPECT_EQ(-1, ExerciseIngestionModule(omaeruv, b.prod.product()).status);
  }

  // 4. OMO3PR init failures.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    EXPECT_EQ(-1, ExerciseIngestionModule(omo3pr, prod.product()).status);
  }
  {
    OmiSwathBuilder b("O3Profile");
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(-1, ExerciseIngestionModule(omo3pr, b.prod.product()).status);
  }
  {
    OmiSwathBuilder b("O3Profile");
    ASSERT_TRUE(b.ok());
    ASSERT_TRUE(b.prod.AddField(b.geo, "Pressure", b.prod.MakeDouble(1.0)));
    EXPECT_EQ(-1, ExerciseIngestionModule(omo3pr, b.prod.product()).status);
  }
  {
    OmiSwathBuilder b("O3Profile");
    ASSERT_TRUE(b.ok());
    constexpr long kDims2D[] = {2, 2};
    constexpr double kVals[] = {1.0, 2.0, 3.0, 4.0};
    ASSERT_TRUE(b.AddDoubleField(b.geo, "Pressure", kDims2D, kVals));
    EXPECT_EQ(-1, ExerciseIngestionModule(omo3pr, b.prod.product()).status);
  }
  {
    OmiSwathBuilder b("O3Profile");
    ASSERT_TRUE(b.ok());
    constexpr long kTime1[] = {1, 2, 3};
    constexpr double kVals[6] = {};
    ASSERT_TRUE(b.AddDoubleField(b.geo, "Pressure", kTime1, kVals));
    EXPECT_EQ(-1, ExerciseIngestionModule(omo3pr, b.prod.product()).status);
  }
  {
    OmiSwathBuilder b("O3Profile");
    ASSERT_TRUE(b.ok());
    constexpr long kVert0[] = {2, 2, 1};
    constexpr double kVals[4] = {};
    ASSERT_TRUE(b.AddDoubleField(b.geo, "Pressure", kVert0, kVals));
    IngestExerciseResult res =
        ExerciseIngestionModule(omo3pr, b.prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    // Pressure without MissingValue fails variable_descriptor_init.
    OmiSwathBuilder b("O3Profile");
    ASSERT_TRUE(b.ok());
    constexpr long kPdims[] = {2, 2, 3};
    constexpr double kPvals[12] = {};
    ASSERT_TRUE(b.prod.AddField(b.geo, "Pressure",
                                b.prod.MakeDoubleArray(kPdims, kPvals)));
    EXPECT_EQ(-1, ExerciseIngestionModule(omo3pr, b.prod.product()).status);
  }
  {
    // Missing O3 or O3Precision fails variable_descriptor_init.
    OmiSwathBuilder b("O3Profile");
    ASSERT_TRUE(b.ok());
    constexpr long kPdims[] = {2, 2, 3};
    constexpr double kPvals[12] = {};
    ASSERT_TRUE(b.AddDoubleField(b.geo, "Pressure", kPdims, kPvals));
    EXPECT_EQ(-1, ExerciseIngestionModule(omo3pr, b.prod.product()).status);

    constexpr long kO3dims[] = {2, 2, 2};
    ASSERT_TRUE(b.AddDoubleField(b.data, "O3", kO3dims,
                                 std::span<const double>(kPvals, 8)));
    EXPECT_EQ(-1, ExerciseIngestionModule(omo3pr, b.prod.product()).status);
  }
}

TEST(HarpIngestOmiL2Test, VariableReadAndAttributeErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* omdoao3 = FindIngestionModule("OMI_L2_OMDOAO3");
  const harp_ingestion_module* omdomino =
      FindIngestionModule("OMI_L2_OMDOMINO");
  const harp_ingestion_module* omaeruv = FindIngestionModule("OMI_L2_OMAERUV");
  ASSERT_NE(nullptr, omdoao3);
  ASSERT_NE(nullptr, omdomino);
  ASSERT_NE(nullptr, omaeruv);

  // 1. Missing fields + attribute failures + wrong types/ranks in OMDOAO3.
  {
    OmiSwathBuilder b("ColumnAmountO3");
    ASSERT_TRUE(b.ok());
    constexpr long kGridDims[] = {2, 2};
    constexpr std::string_view kBadStrs[] = {"a", "b", "c", "d"};
    // Latitude has string elements so init_dimensions succeeds, then
    // init_geolocation fails when reading Latitude.
    CodaMemNode* bad_lat = b.prod.MakeStringArray(kGridDims, kBadStrs);
    constexpr long kAttrDims[] = {1};
    constexpr double kMissing[] = {kMissingDouble};
    ASSERT_TRUE(b.prod.AddAttribute(
        bad_lat, "MissingValue", b.prod.MakeDoubleArray(kAttrDims, kMissing)));
    ASSERT_TRUE(b.prod.AddField(b.geo, "Latitude", bad_lat));

    // Longitude without MissingValue fails get_variable_attributes.
    constexpr double kVals[] = {1.0, 2.0, 3.0, 4.0};
    ASSERT_TRUE(b.prod.AddField(b.geo, "Longitude",
                                b.prod.MakeDoubleArray(kGridDims, kVals)));

    // Time as scalar MissingValue fails coda_cursor_goto_first_array_element.
    CodaMemNode* time_node = b.prod.MakeDoubleArray(
        std::to_array<long>({2}), std::to_array<double>({1.0, 2.0}));
    ASSERT_TRUE(b.prod.AddAttribute(time_node, "MissingValue",
                                    b.prod.MakeDouble(kMissingDouble)));
    ASSERT_TRUE(b.prod.AddField(b.geo, "Time", time_node));

    // ColumnAmountO3 has string MissingValue attribute.
    CodaMemNode* col_node = b.prod.MakeDoubleArray(kGridDims, kVals);
    ASSERT_TRUE(b.prod.AddAttribute(
        col_node, "MissingValue",
        b.prod.MakeStringArray(kAttrDims,
                               std::to_array<std::string_view>({"x"}))));
    ASSERT_TRUE(b.prod.AddField(b.data, "ColumnAmountO3", col_node));

    // ColumnAmountO3Precision has scalar ScaleFactor attribute.
    CodaMemNode* prec_node = b.prod.MakeDoubleArray(kGridDims, kVals);
    ASSERT_TRUE(
        b.prod.AddAttribute(prec_node, "MissingValue",
                            b.prod.MakeDoubleArray(kAttrDims, kMissing)));
    ASSERT_TRUE(
        b.prod.AddAttribute(prec_node, "ScaleFactor", b.prod.MakeDouble(1.0)));
    ASSERT_TRUE(b.prod.AddField(b.data, "ColumnAmountO3Precision", prec_node));

    // CloudFraction has string ScaleFactor attribute.
    CodaMemNode* cf_node = b.prod.MakeDoubleArray(kGridDims, kVals);
    ASSERT_TRUE(b.prod.AddAttribute(
        cf_node, "MissingValue", b.prod.MakeDoubleArray(kAttrDims, kMissing)));
    ASSERT_TRUE(b.prod.AddAttribute(
        cf_node, "ScaleFactor",
        b.prod.MakeStringArray(kAttrDims,
                               std::to_array<std::string_view>({"x"}))));
    ASSERT_TRUE(b.prod.AddField(b.data, "CloudFraction", cf_node));

    // CloudPressure has scalar Offset attribute.
    CodaMemNode* cp_node = b.prod.MakeDoubleArray(kGridDims, kVals);
    ASSERT_TRUE(b.prod.AddAttribute(
        cp_node, "MissingValue", b.prod.MakeDoubleArray(kAttrDims, kMissing)));
    ASSERT_TRUE(b.prod.AddAttribute(cp_node, "Offset", b.prod.MakeDouble(0.0)));
    ASSERT_TRUE(b.prod.AddField(b.data, "CloudPressure", cp_node));

    // CloudPressurePrecision has string Offset attribute.
    CodaMemNode* cpp_node = b.prod.MakeDoubleArray(kGridDims, kVals);
    ASSERT_TRUE(b.prod.AddAttribute(
        cpp_node, "MissingValue", b.prod.MakeDoubleArray(kAttrDims, kMissing)));
    ASSERT_TRUE(b.prod.AddAttribute(
        cpp_node, "Offset",
        b.prod.MakeStringArray(kAttrDims,
                               std::to_array<std::string_view>({"x"}))));
    ASSERT_TRUE(b.prod.AddField(b.data, "CloudPressurePrecision", cpp_node));

    // ProcessingQualityFlags as string array fails read_variable_int32;
    // TerrainHeight with wrong dims fails verify_variable_dimensions.
    ASSERT_TRUE(b.prod.AddField(b.data, "ProcessingQualityFlags",
                                b.prod.MakeStringArray(kGridDims, kBadStrs)));
    ASSERT_TRUE(b.AddDoubleField(b.geo, "TerrainHeight",
                                 std::to_array<long>({2, 3}),
                                 std::to_array<double>({1, 2, 3, 4, 5, 6})));
    // SolarZenithAngle as 1D array fails rank check in
    // verify_variable_dimensions.
    ASSERT_TRUE(b.AddDoubleField(b.geo, "SolarZenithAngle",
                                 std::to_array<long>({4}), kVals));
    // ViewingZenithAngle as scalar fails coda_cursor_get_array_dim.
    ASSERT_TRUE(
        b.prod.AddField(b.geo, "ViewingZenithAngle", b.prod.MakeDouble(1.0)));

    IngestExerciseResult res = ExerciseIngestionModule(
        omdoao3, b.prod.product(), /*options_str=*/nullptr,
        /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["longitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_altitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_latitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_longitude"].read_status);
    EXPECT_EQ(-1, res.variables["surface_altitude"].read_status);
    EXPECT_EQ(-1, res.variables["solar_zenith_angle"].read_status);
    EXPECT_EQ(-1, res.variables["viewing_zenith_angle"].read_status);
    EXPECT_EQ(-1, res.variables["O3_column_number_density"].read_status);
    EXPECT_EQ(
        -1, res.variables["O3_column_number_density_uncertainty"].read_status);
    EXPECT_EQ(-1,
              res.variables["O3_column_number_density_validity"].read_status);
    EXPECT_EQ(-1, res.variables["cloud_fraction"].read_status);
    EXPECT_EQ(-1, res.variables["cloud_pressure"].read_status);
    EXPECT_EQ(-1, res.variables["cloud_pressure_uncertainty"].read_status);
  }

  // 2. read_latitude failure in init_geolocation after valid Longitude.
  {
    OmiSwathBuilder b("ColumnAmountO3");
    ASSERT_TRUE(b.ok());
    constexpr long kGridDims[] = {2, 2};
    constexpr double kVals[] = {1.0, 2.0, 3.0, 4.0};
    ASSERT_TRUE(b.AddDoubleField(b.geo, "Longitude", kGridDims, kVals));
    ASSERT_TRUE(b.prod.AddField(b.geo, "Latitude",
                                b.prod.MakeDoubleArray(kGridDims, kVals)));
    std::vector<double> bounds;
    EXPECT_EQ(-1, ReadSingleVariableDirect(omdoao3, b.prod.product(),
                                           "longitude_bounds", &bounds));
  }

  // 3. OMDOMINO int16 error paths (missing, wrong dims, wrong type) + missing
  // cornerpoints.
  {
    OmiSwathBuilder b("DominoNO2");
    ASSERT_TRUE(b.ok());
    ASSERT_TRUE(b.AddBaseGeo2x2());
    constexpr long kGridDims[] = {2, 2};
    constexpr std::string_view kBadStrs[] = {"a", "b", "c", "d"};
    ASSERT_TRUE(b.prod.AddField(b.data, "TroposphericColumnFlag",
                                b.prod.MakeStringArray(kGridDims, kBadStrs)));
    IngestExerciseResult res = ExerciseIngestionModule(
        omdomino, b.prod.product(), /*options_str=*/nullptr,
        /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["longitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1,
              res.variables["tropospheric_NO2_column_number_density_validity"]
                  .read_status);
  }
  {
    OmiSwathBuilder b("DominoNO2");
    ASSERT_TRUE(b.ok());
    ASSERT_TRUE(b.AddBaseGeo2x2());
    constexpr long kBadDims[] = {2, 3};
    constexpr int16_t kBadFlags[] = {0, 1, 2, 3, 4, 5};
    ASSERT_TRUE(b.prod.AddField(b.data, "TroposphericColumnFlag",
                                b.prod.MakeInt16Array(kBadDims, kBadFlags)));
    IngestExerciseResult res = ExerciseIngestionModule(
        omdomino, b.prod.product(), /*options_str=*/nullptr,
        /*stop_on_error=*/false);
    EXPECT_EQ(-1,
              res.variables["tropospheric_NO2_column_number_density_validity"]
                  .read_status);
  }
  {
    // Missing TroposphericColumnFlag and ProcessingQualityFlags.
    OmiSwathBuilder b("DominoNO2");
    ASSERT_TRUE(b.ok());
    ASSERT_TRUE(b.AddBaseGeo2x2());
    IngestExerciseResult res = ExerciseIngestionModule(
        omdomino, b.prod.product(), /*options_str=*/nullptr,
        /*stop_on_error=*/false);
    EXPECT_EQ(-1,
              res.variables["tropospheric_NO2_column_number_density_validity"]
                  .read_status);
  }

  // 4. read_variable_partial_double failure on string array with attributes.
  {
    OmiSwathBuilder b("Aerosol_NearUV_Swath");
    ASSERT_TRUE(b.ok());
    ASSERT_TRUE(b.AddBaseGeo2x2());
    constexpr long kSpecDims[] = {2, 2, 3};
    constexpr std::string_view kBadStrs[12] = {"a", "b", "c", "d", "e", "f",
                                               "g", "h", "i", "j", "k", "l"};
    CodaMemNode* bad_aod = b.prod.MakeStringArray(kSpecDims, kBadStrs);
    constexpr long kAttrDims[] = {1};
    constexpr double kMissing[] = {kMissingDouble};
    ASSERT_TRUE(b.prod.AddAttribute(
        bad_aod, "MissingValue", b.prod.MakeDoubleArray(kAttrDims, kMissing)));
    ASSERT_TRUE(b.prod.AddField(b.data, "FinalAerosolOpticalDepth", bad_aod));
    constexpr double kAaod[12] = {};
    ASSERT_TRUE(b.AddDoubleField(b.data, "FinalAerosolAbsOpticalDepth",
                                 kSpecDims, kAaod));
    IngestExerciseResult res = ExerciseIngestionModule(
        omaeruv, b.prod.product(), /*options_str=*/nullptr,
        /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["aerosol_optical_depth"].read_status);
  }

  // Note: upstream ingestion_init_no2 returns -1 without freeing `info` when
  // destriped=true and SlantColumnAmountNO2Destriped is absent, so we do not
  // exercise that error path under HeapLeakChecker/LSan.
}

}  // namespace
}  // namespace stcorp_harp
