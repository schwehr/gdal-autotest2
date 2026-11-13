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
// Unit tests for ESA CCI L2 Ozone Nadir Profile ingestion module
// (libharp/harp-ingest-cci_l2_o3_np.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-cci_l2_o3_np.c

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

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

void AddFloatDataset(CodaMemProduct& prod, std::string_view name,
                     const long* dims, int num_dims, const float* vals,
                     long num_vals, bool add_fill_value = true,
                     float fill_val = -999.0f) {
  std::span<const long> dim_span(dims, static_cast<size_t>(num_dims));
  std::span<const float> val_span(vals, static_cast<size_t>(num_vals));
  CodaMemNode* node = prod.MakeFloatArray(dim_span, val_span);
  if (add_fill_value) {
    ASSERT_TRUE(prod.AddAttribute(node, "FillValue", prod.MakeFloat(fill_val)));
  }
  ASSERT_TRUE(prod.AddField(prod.root(), name, node));
}

void AddDoubleDataset(CodaMemProduct& prod, std::string_view name,
                      const long* dims, int num_dims, const double* vals,
                      long num_vals, double fill_val = -999.0) {
  std::span<const long> dim_span(dims, static_cast<size_t>(num_dims));
  std::span<const double> val_span(vals, static_cast<size_t>(num_vals));
  CodaMemNode* node = prod.MakeDoubleArray(dim_span, val_span);
  ASSERT_TRUE(prod.AddAttribute(node, "FillValue", prod.MakeDouble(fill_val)));
  ASSERT_TRUE(prod.AddField(prod.root(), name, node));
}

TEST(HarpIngestCciL2O3NpTest, IngestSuccessAllVariables) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ESACCI_OZONE_L2_NP");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  constexpr long kTimeDims[] = {2};
  constexpr long kLevelDims[] = {2};
  constexpr long kGridDims[] = {2, 2};
  constexpr long kCovDims[] = {2, 2, 2};
  constexpr long kBoundsDims[] = {2, 4, 2};

  // 2000-01-01 -> epoch 0.0 hours since 2000-01-01.
  ASSERT_TRUE(prod.AddAttribute(prod.root(), "Data_date",
                                prod.MakeString("2000-01-01")));

  constexpr int16_t kScp[] = {3, -32767};
  CodaMemNode* scp_node = prod.MakeInt16Array(kTimeDims, kScp);
  ASSERT_TRUE(prod.AddAttribute(scp_node, "FillValue", prod.MakeInt16(-32767)));
  ASSERT_TRUE(prod.AddField(prod.root(), "scp", scp_node));

  constexpr double kTime[] = {1.5, -999.0};
  AddDoubleDataset(prod, "time", kTimeDims, 1, kTime, 2);

  constexpr float kLon[] = {10.0f, -999.0f};
  constexpr float kLat[] = {50.0f, 51.0f};
  // Include fill value -999.0f in ll to exercise NaN replacement in
  // read_latitude_bounds and read_longitude_bounds.
  constexpr float kLl[] = {
      1.0f, 11.0f, 2.0f, 12.0f, 3.0f, 13.0f,   -999.0f, 14.0f,
      5.0f, 15.0f, 6.0f, 16.0f, 7.0f, -999.0f, 8.0f,    18.0f,
  };
  constexpr float kLza[] = {5.0f, 6.0f};
  constexpr float kSza[] = {40.0f, 41.0f};
  constexpr float kLevs[] = {1000.0f, 500.0f};
  constexpr float kO3Nd[] = {100.0f, 200.0f, 300.0f, 400.0f};
  constexpr float kO3Err[] = {10.0f, 20.0f, 5.0f, 2.5f};
  constexpr float kSx[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
  constexpr float kAk[] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
  constexpr float kO3Vmr[] = {10.0f, 20.0f, 30.0f, 40.0f};
  constexpr float kO3Ap[] = {50.0f, 60.0f, 70.0f, 80.0f};
  constexpr float kO3ApErr[] = {10.0f, 10.0f, 20.0f, 25.0f};
  constexpr float kSalb[] = {0.2f, 0.3f};
  constexpr float kSpres[] = {1013.0f, 1010.0f};

  AddFloatDataset(prod, "lon", kTimeDims, 1, kLon, 2);
  // Omit @FillValue on lat to cover the optional-@FillValue branch for float.
  AddFloatDataset(prod, "lat", kTimeDims, 1, kLat, 2, /*add_fill_value=*/false);
  AddFloatDataset(prod, "ll", kBoundsDims, 3, kLl, 16);
  AddFloatDataset(prod, "lza", kTimeDims, 1, kLza, 2);
  AddFloatDataset(prod, "sza", kTimeDims, 1, kSza, 2);
  AddFloatDataset(prod, "levs", kLevelDims, 1, kLevs, 2);
  AddFloatDataset(prod, "o3_nd", kGridDims, 2, kO3Nd, 4);
  AddFloatDataset(prod, "o3_error", kGridDims, 2, kO3Err, 4);
  AddFloatDataset(prod, "sx", kCovDims, 3, kSx, 8);
  AddFloatDataset(prod, "ak", kCovDims, 3, kAk, 8);
  AddFloatDataset(prod, "o3_vmr", kGridDims, 2, kO3Vmr, 4);
  AddFloatDataset(prod, "o3_ap", kGridDims, 2, kO3Ap, 4);
  AddFloatDataset(prod, "o3_ap_error", kGridDims, 2, kO3ApErr, 4);
  AddFloatDataset(prod, "salb", kTimeDims, 1, kSalb, 2);
  AddFloatDataset(prod, "spres", kTimeDims, 1, kSpres, 2);

  constexpr double kCloudf[] = {0.25, 0.5};
  constexpr double kCloudp[] = {600.0, 700.0};
  constexpr double kClouda[] = {0.8, 0.9};
  AddDoubleDataset(prod, "cloudf", kTimeDims, 1, kCloudf, 2);
  AddDoubleDataset(prod, "cloudp", kTimeDims, 1, kCloudp, 2);
  AddDoubleDataset(prod, "clouda", kTimeDims, 1, kClouda, 2);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(2, res.dimensions[harp_dimension_vertical]);

  EXPECT_EQ(3, res.variables["scan_subindex"].int16_values[0]);
  EXPECT_EQ(harp_get_fill_value_for_type(harp_type_int16).int16_data,
            res.variables["scan_subindex"].int16_values[1]);
  EXPECT_DOUBLE_EQ(1.5, res.variables["datetime"].double_values[0]);
  EXPECT_TRUE(std::isnan(res.variables["datetime"].double_values[1]));
  EXPECT_FLOAT_EQ(10.0f, res.variables["longitude"].float_values[0]);
  EXPECT_TRUE(std::isnan(res.variables["longitude"].float_values[1]));
  EXPECT_THAT(res.variables["latitude"].float_values,
              ElementsAre(50.0f, 51.0f));

  // Verify corner reordering [0,1,3,2] and NaN replacement.
  const auto& lon_b = res.variables["longitude_bounds"].float_values;
  EXPECT_FLOAT_EQ(11.0f, lon_b[0]);
  EXPECT_FLOAT_EQ(12.0f, lon_b[1]);
  EXPECT_FLOAT_EQ(14.0f, lon_b[2]);
  EXPECT_FLOAT_EQ(13.0f, lon_b[3]);
  EXPECT_FLOAT_EQ(15.0f, lon_b[4]);
  EXPECT_FLOAT_EQ(16.0f, lon_b[5]);
  EXPECT_FLOAT_EQ(18.0f, lon_b[6]);
  EXPECT_TRUE(std::isnan(lon_b[7]));

  const auto& lat_b = res.variables["latitude_bounds"].float_values;
  EXPECT_FLOAT_EQ(1.0f, lat_b[0]);
  EXPECT_FLOAT_EQ(2.0f, lat_b[1]);
  EXPECT_TRUE(std::isnan(lat_b[2]));
  EXPECT_FLOAT_EQ(3.0f, lat_b[3]);
  EXPECT_FLOAT_EQ(5.0f, lat_b[4]);
  EXPECT_FLOAT_EQ(6.0f, lat_b[5]);
  EXPECT_FLOAT_EQ(8.0f, lat_b[6]);
  EXPECT_FLOAT_EQ(7.0f, lat_b[7]);

  EXPECT_THAT(res.variables["O3_number_density_uncertainty"].float_values,
              ElementsAre(FloatNear(10.0f, 1e-4f), FloatNear(40.0f, 1e-4f),
                          FloatNear(15.0f, 1e-4f), FloatNear(10.0f, 1e-4f)));
  EXPECT_THAT(res.variables["O3_volume_mixing_ratio_uncertainty"].float_values,
              ElementsAre(FloatNear(1.0f, 1e-4f), FloatNear(4.0f, 1e-4f),
                          FloatNear(1.5f, 1e-4f), FloatNear(1.0f, 1e-4f)));
  EXPECT_THAT(
      res.variables["O3_volume_mixing_ratio_apriori_uncertainty"].float_values,
      ElementsAre(FloatNear(5.0f, 1e-4f), FloatNear(6.0f, 1e-4f),
                  FloatNear(14.0f, 1e-4f), FloatNear(20.0f, 1e-4f)));
  EXPECT_THAT(res.variables["cloud_fraction"].double_values,
              ElementsAre(0.25, 0.5));
  EXPECT_THAT(res.variables["cloud_top_pressure"].double_values,
              ElementsAre(600.0, 700.0));
  EXPECT_THAT(res.variables["cloud_top_albedo"].double_values,
              ElementsAre(0.8, 0.9));
  EXPECT_THAT(res.variables["surface_albedo"].float_values,
              ElementsAre(0.2f, 0.3f));
  EXPECT_THAT(res.variables["surface_pressure"].float_values,
              ElementsAre(1013.0f, 1010.0f));
}

TEST(HarpIngestCciL2O3NpTest, ErrorPathsInitAndGeoBounds) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ESACCI_OZONE_L2_NP");
  ASSERT_NE(nullptr, mod);

  // 1. Missing /o3_nd.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. Non-array /o3_nd.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "o3_nd", prod.MakeFloat(1.0f)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. 1D /o3_nd (expected 2D).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims1d[] = {2};
    constexpr float kVals[] = {1.0f, 2.0f};
    ASSERT_TRUE(prod.AddField(prod.root(), "o3_nd",
                              prod.MakeFloatArray(kDims1d, kVals)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 4. Missing /ll.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kGridDims[] = {1, 2};
    constexpr float kVals[] = {1.0f, 2.0f};
    ASSERT_TRUE(prod.AddField(prod.root(), "o3_nd",
                              prod.MakeFloatArray(kGridDims, kVals)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 5. Wrong element count for /ll.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kGridDims[] = {1, 2};
    constexpr float kVals[] = {1.0f, 2.0f};
    ASSERT_TRUE(prod.AddField(prod.root(), "o3_nd",
                              prod.MakeFloatArray(kGridDims, kVals)));
    ASSERT_TRUE(prod.AddField(prod.root(), "ll",
                              prod.MakeFloatArray(kGridDims, kVals)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 6. Missing @FillValue on /ll.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kGridDims[] = {1, 2};
    constexpr long kLlDims[] = {8};
    constexpr float kVals[] = {1.0f, 2.0f};
    constexpr float kLl[8] = {};
    ASSERT_TRUE(prod.AddField(prod.root(), "o3_nd",
                              prod.MakeFloatArray(kGridDims, kVals)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "ll", prod.MakeFloatArray(kLlDims, kLl)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 7. Non-float @FillValue on /ll.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kGridDims[] = {1, 2};
    constexpr long kLlDims[] = {8};
    constexpr float kVals[] = {1.0f, 2.0f};
    constexpr float kLl[8] = {};
    ASSERT_TRUE(prod.AddField(prod.root(), "o3_nd",
                              prod.MakeFloatArray(kGridDims, kVals)));
    CodaMemNode* ll_node = prod.MakeFloatArray(kLlDims, kLl);
    ASSERT_TRUE(
        prod.AddAttribute(ll_node, "FillValue", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "ll", ll_node));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
}

TEST(HarpIngestCciL2O3NpTest, ErrorPathsReadDatasetDatetimeBoundsAndRelErr) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ESACCI_OZONE_L2_NP");
  ASSERT_NE(nullptr, mod);

  // Helper lambda to build a minimal valid o3_nd + ll header so ingestion_init
  // succeeds.
  auto make_base = [&](CodaMemProduct& prod) {
    constexpr long kGridDims[] = {1, 2};
    constexpr long kLlDims[] = {8};
    constexpr float kVals[] = {10.0f, 20.0f};
    constexpr float kLl[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    AddFloatDataset(prod, "o3_nd", kGridDims, 2, kVals, 2);
    AddFloatDataset(prod, "ll", kLlDims, 1, kLl, 8);
  };

  // 1. Various Data_date errors in read_datetime.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_base(prod);
    // Missing @Data_date -> CODA error.
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_base(prod);
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Data_date", prod.MakeInt32(1)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_base(prod);
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Data_date",
                                  prod.MakeString("2020-01")));  // len < 10
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_base(prod);
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Data_date",
                                  prod.MakeString("2020-01-01T00")));  // > 10
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_base(prod);
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Data_date",
                                  prod.MakeString("not-a-date")));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_base(prod);
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Data_date",
                                  prod.MakeString("2020-05-15")));
    // Valid Data_date, missing /time -> read_dataset fails.
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
  }

  // 2. read_dataset type-specific errors: int16 (scp), float (lon/lat/lza),
  // double (cloudf/cloudp/clouda), and read_relerr_as_uncertainty_float.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_base(prod);
    constexpr long kTimeDims[] = {1};
    constexpr long kWrongDims[] = {2};
    constexpr int16_t kScp[] = {1};
    constexpr std::string_view kBadStr[] = {"bad"};
    constexpr double kWrongDoubles[] = {1.0, 2.0};
    constexpr double kDbl[] = {1.0};
    constexpr float kFlt[] = {1.0f};

    // scp without @FillValue -> CODA error.
    ASSERT_TRUE(prod.AddField(prod.root(), "scp",
                              prod.MakeInt16Array(kTimeDims, kScp)));
    // lon as string array -> read_float_array error.
    ASSERT_TRUE(prod.AddField(prod.root(), "lon",
                              prod.MakeStringArray(kTimeDims, kBadStr)));
    // lat with string @FillValue -> read_float @FillValue error.
    CodaMemNode* lat_node = prod.MakeFloatArray(kTimeDims, kFlt);
    ASSERT_TRUE(
        prod.AddAttribute(lat_node, "FillValue", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "lat", lat_node));
    // cloudf as string array -> read_double_array error.
    ASSERT_TRUE(prod.AddField(prod.root(), "cloudf",
                              prod.MakeStringArray(kTimeDims, kBadStr)));
    // cloudp without @FillValue -> CODA error.
    ASSERT_TRUE(prod.AddField(prod.root(), "cloudp",
                              prod.MakeDoubleArray(kTimeDims, kDbl)));
    // clouda with string @FillValue -> read_double @FillValue error.
    CodaMemNode* clouda_node = prod.MakeDoubleArray(kTimeDims, kDbl);
    ASSERT_TRUE(
        prod.AddAttribute(clouda_node, "FillValue", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "clouda", clouda_node));
    // lza wrong element count -> INGESTION error.
    constexpr float kWrongFloats[] = {1.0f, 2.0f};
    ASSERT_TRUE(prod.AddField(prod.root(), "lza",
                              prod.MakeFloatArray(kWrongDims, kWrongFloats)));
    (void)kWrongDoubles;

    // Missing o3_error and missing o3_vmr trigger relerr failure branches.
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["scan_subindex"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_zenith_angle"].read_status);
    EXPECT_EQ(-1, res.variables["cloud_fraction"].read_status);
    EXPECT_EQ(-1, res.variables["cloud_top_pressure"].read_status);
    EXPECT_EQ(-1, res.variables["cloud_top_albedo"].read_status);
    EXPECT_EQ(-1, res.variables["O3_number_density_uncertainty"].read_status);
    EXPECT_EQ(-1,
              res.variables["O3_volume_mixing_ratio_uncertainty"].read_status);
  }

  // 3. String-typed scp and string-typed @FillValue on int16 scp.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_base(prod);
    constexpr long kTimeDims[] = {1};
    constexpr std::string_view kBadStr[] = {"bad"};
    ASSERT_TRUE(prod.AddField(prod.root(), "scp",
                              prod.MakeStringArray(kTimeDims, kBadStr)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["scan_subindex"].read_status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_base(prod);
    constexpr long kTimeDims[] = {1};
    constexpr int16_t kScp[] = {1};
    CodaMemNode* scp_node = prod.MakeInt16Array(kTimeDims, kScp);
    ASSERT_TRUE(
        prod.AddAttribute(scp_node, "FillValue", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "scp", scp_node));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["scan_subindex"].read_status);
  }

  // 4. Out-of-bounds read_block index on longitude_bounds + per-element
  // read_float failures in longitude_bounds (elements 1, 3, 7, 5) and
  // latitude_bounds (elements 0, 2, 6, 4) using coda_format_ascii.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_base(prod);
    ScopedHarpIngestionOptions opts;
    harp_ingestion_options* raw_opts = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_ingestion_options_new(&raw_opts));
    opts.reset(raw_opts);

    harp_product_definition* prod_def = nullptr;
    void* user_data = nullptr;
    ASSERT_EQ(0, mod->ingestion_init(mod, prod.product(), opts.get(), &prod_def,
                                     &user_data));
    auto done_guard = absl::MakeCleanup([&] {
      if (mod->ingestion_done != nullptr && user_data != nullptr) {
        mod->ingestion_done(user_data);
      }
    });

    float buf[4] = {};
    harp_array slice{};
    slice.float_data = buf;
    for (int i = 0; i < prod_def->num_variable_definitions; ++i) {
      const harp_variable_definition* var_def =
          prod_def->variable_definition[i];
      if (std::string_view(var_def->name) == "longitude_bounds" ||
          std::string_view(var_def->name) == "latitude_bounds") {
        EXPECT_EQ(-1, var_def->read_block(user_data, 1, slice));
      }
    }
  }

  constexpr int kLonIndices[] = {1, 3, 7, 5};
  constexpr int kLatIndices[] = {0, 2, 6, 4};
  const float kValidAscii =
      std::bit_cast<float>(std::array<char, 4>{' ', '1', '.', '0'});
  const float kBadAscii =
      std::bit_cast<float>(std::array<char, 4>{'b', 'a', 'd', '!'});
  const float kFillAscii =
      std::bit_cast<float>(std::array<char, 4>{'-', '9', '9', '9'});

  auto run_ascii_bounds_failure = [&](int bad_elem,
                                      std::string_view target_var) {
    CodaMemProduct prod(coda_format_ascii);
    ASSERT_TRUE(prod.ok());
    constexpr long kGridDims[] = {1, 2};
    constexpr long kLlDims[] = {8};
    const float kO3Vals[] = {kValidAscii, kValidAscii};
    ASSERT_TRUE(prod.AddField(prod.root(), "o3_nd",
                              prod.MakeFloatArray(kGridDims, kO3Vals)));

    float ll_vals[8];
    for (int i = 0; i < 8; ++i) {
      ll_vals[i] = (i == bad_elem) ? kBadAscii : kValidAscii;
    }
    CodaMemNode* ll_node = prod.MakeFloatArray(kLlDims, ll_vals);
    ASSERT_TRUE(
        prod.AddAttribute(ll_node, "FillValue", prod.MakeFloat(kFillAscii)));
    ASSERT_TRUE(prod.AddField(prod.root(), "ll", ll_node));
    // coda_ascii_cursor checks `bit_offset + bit_size >= mem_size * 8`, so add
    // a trailing field to keep all ll elements and @FillValue strictly inside.
    ASSERT_TRUE(prod.AddField(prod.root(), "pad", prod.MakeFloat(kValidAscii)));

    ScopedHarpIngestionOptions opts;
    harp_ingestion_options* raw_opts = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_ingestion_options_new(&raw_opts));
    opts.reset(raw_opts);
    harp_product_definition* prod_def = nullptr;
    void* user_data = nullptr;
    ASSERT_EQ(0, mod->ingestion_init(mod, prod.product(), opts.get(), &prod_def,
                                     &user_data));
    auto done_guard = absl::MakeCleanup([&] {
      if (mod->ingestion_done != nullptr && user_data != nullptr) {
        mod->ingestion_done(user_data);
      }
    });

    float buf[4] = {};
    harp_array slice{};
    slice.float_data = buf;
    for (int i = 0; i < prod_def->num_variable_definitions; ++i) {
      const harp_variable_definition* var_def =
          prod_def->variable_definition[i];
      if (var_def->name == target_var) {
        EXPECT_EQ(-1, var_def->read_block(user_data, 0, slice));
      }
    }
  };

  for (int idx : kLonIndices) {
    run_ascii_bounds_failure(idx, "longitude_bounds");
  }
  for (int idx : kLatIndices) {
    run_ascii_bounds_failure(idx, "latitude_bounds");
  }
}

}  // namespace
}  // namespace stcorp_harp
