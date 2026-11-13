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
// Unit tests for ESA CCI L2 O3 total column ingestion module
// (libharp/harp-ingest-cci_l2_o3_tc.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-cci_l2_o3_tc.c

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

using ::testing::ElementsAre;

constexpr char kModuleName[] = "ESACCI_OZONE_L2_TC";

// Populates a complete synthetic ESACCI_OZONE_L2_TC product.
// When `transposed` is true, /averaging_kernels has shape
// [num_layers, dim1, dim2] (so num_time = dim1 * dim2) and multi-dimensional
// datasets use the transposed layout expected by the module.
void PopulateCciL2O3TcProduct(CodaMemProduct& prod, bool transposed,
                              bool with_fill_values) {
  constexpr long kNumLayers = 2;
  constexpr long kNumTime = 2;
  const long time_dims[] = {kNumTime};
  const long corner_dims[] = {transposed ? 4 : kNumTime,
                              transposed ? kNumTime : 4};
  const long press_dims[] = {transposed ? (kNumLayers + 1) : kNumTime,
                             transposed ? kNumTime : (kNumLayers + 1)};
  const long avk_dims_2d[] = {kNumTime, kNumLayers};
  const long avk_dims_3d[] = {kNumLayers, 1, kNumTime};

  // Integer pixel_number with optional @FillValue[0].
  const int32_t pixel_vals[] = {with_fill_values ? -999 : 1, 2};
  CodaMemNode* pixel_node = prod.MakeInt32Array(time_dims, pixel_vals);
  if (with_fill_values) {
    constexpr long kFillDims[] = {1};
    constexpr int32_t kFillVal[] = {-999};
    ASSERT_TRUE(prod.AddAttribute(pixel_node, "FillValue",
                                  prod.MakeInt32Array(kFillDims, kFillVal)));
  }
  ASSERT_TRUE(prod.AddField(prod.root(), "pixel_number", pixel_node));

  // Scalar-per-pixel double datasets with optional @FillValue[0] on /time.
  const double time_vals[] = {with_fill_values ? -999.0 : 5000.0, 5001.0};
  CodaMemNode* time_node = prod.MakeDoubleArray(time_dims, time_vals);
  if (with_fill_values) {
    constexpr long kFillDims[] = {1};
    constexpr double kFillVal[] = {-999.0};
    ASSERT_TRUE(prod.AddAttribute(time_node, "FillValue",
                                  prod.MakeDoubleArray(kFillDims, kFillVal)));
  }
  ASSERT_TRUE(prod.AddField(prod.root(), "time", time_node));

  constexpr double kLatVals[] = {51.5, 52.5};
  constexpr double kLonVals[] = {4.5, 5.5};
  constexpr double kSzaVals[] = {30.0, 35.0};
  constexpr double kVzaVals[] = {10.0, 15.0};
  constexpr double kRaaVals[] = {45.0, 50.0};
  constexpr double kO3Vals[] = {300.0, 310.0};
  constexpr double kO3ErrVals[] = {5.0, 6.0};
  constexpr double kTempVals[] = {225.0, 228.0};
  constexpr double kCfVals[] = {0.2, 0.4};
  constexpr double kCtpVals[] = {50000.0, 60000.0};
  constexpr double kCaVals[] = {0.8, 0.7};
  constexpr double kSpVals[] = {70000.0, 75000.0};
  constexpr double kSaVals[] = {0.3, 0.35};
  constexpr double kSurfAlbVals[] = {0.1, 0.15};
  constexpr double kSurfAltVals[] = {12.0, 25.0};

  ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                            prod.MakeDoubleArray(time_dims, kLatVals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                            prod.MakeDoubleArray(time_dims, kLonVals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "solar_zenith_angle",
                            prod.MakeDoubleArray(time_dims, kSzaVals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "viewing_zenith_angle",
                            prod.MakeDoubleArray(time_dims, kVzaVals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "relative_azimuth_angle",
                            prod.MakeDoubleArray(time_dims, kRaaVals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "total_ozone_column",
                            prod.MakeDoubleArray(time_dims, kO3Vals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "total_ozone_column_random_error",
                            prod.MakeDoubleArray(time_dims, kO3ErrVals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "effective_temperature",
                            prod.MakeDoubleArray(time_dims, kTempVals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "cloud_fraction",
                            prod.MakeDoubleArray(time_dims, kCfVals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "cloud_top_pressure",
                            prod.MakeDoubleArray(time_dims, kCtpVals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "cloud_albedo",
                            prod.MakeDoubleArray(time_dims, kCaVals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "effective_scene_pressure",
                            prod.MakeDoubleArray(time_dims, kSpVals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "effective_scene_albedo",
                            prod.MakeDoubleArray(time_dims, kSaVals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "surface_albedo",
                            prod.MakeDoubleArray(time_dims, kSurfAlbVals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "surface_altitude",
                            prod.MakeDoubleArray(time_dims, kSurfAltVals)));

  // Corner bounds: [time, 4] or transposed [4, time].
  constexpr double kLonCorners2D[] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
  constexpr double kLonCorners3D[] = {1.0, 5.0, 2.0, 6.0, 3.0, 7.0, 4.0, 8.0};
  constexpr double kLatCorners2D[] = {10.0, 20.0, 30.0, 40.0,
                                      50.0, 60.0, 70.0, 80.0};
  constexpr double kLatCorners3D[] = {10.0, 50.0, 20.0, 60.0,
                                      30.0, 70.0, 40.0, 80.0};
  ASSERT_TRUE(prod.AddField(
      prod.root(), "longitude_corner",
      prod.MakeDoubleArray(corner_dims,
                           transposed ? kLonCorners3D : kLonCorners2D)));
  ASSERT_TRUE(prod.AddField(
      prod.root(), "latitude_corner",
      prod.MakeDoubleArray(corner_dims,
                           transposed ? kLatCorners3D : kLatCorners2D)));

  // Pressure levels [time, 3] or [3, time]:
  // t0 levels = [1000, 500, 100], t1 levels = [1010, 510, 110].
  constexpr double kPress2D[] = {1000.0, 500.0, 100.0, 1010.0, 510.0, 110.0};
  constexpr double kPress3D[] = {1000.0, 1010.0, 500.0, 510.0, 100.0, 110.0};
  ASSERT_TRUE(prod.AddField(
      prod.root(), "atmosphere_pressure_grid",
      prod.MakeDoubleArray(press_dims, transposed ? kPress3D : kPress2D)));

  // Averaging kernels & a-priori: [time, 2] or [2, 1, time].
  constexpr double kAvk2D[] = {0.1, 0.2, 0.3, 0.4};
  constexpr double kAvk3D[] = {0.1, 0.3, 0.2, 0.4};
  constexpr double kApriori2D[] = {11.0, 12.0, 13.0, 14.0};
  constexpr double kApriori3D[] = {11.0, 13.0, 12.0, 14.0};
  if (transposed) {
    ASSERT_TRUE(prod.AddField(prod.root(), "averaging_kernels",
                              prod.MakeDoubleArray(avk_dims_3d, kAvk3D)));
    ASSERT_TRUE(prod.AddField(prod.root(), "apriori_ozone_profile",
                              prod.MakeDoubleArray(avk_dims_3d, kApriori3D)));
  } else {
    ASSERT_TRUE(prod.AddField(prod.root(), "averaging_kernels",
                              prod.MakeDoubleArray(avk_dims_2d, kAvk2D)));
    ASSERT_TRUE(prod.AddField(prod.root(), "apriori_ozone_profile",
                              prod.MakeDoubleArray(avk_dims_2d, kApriori2D)));
  }
}

TEST(HarpIngestCciL2O3TcTest, IngestNonTransposedWithFillValues) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule(kModuleName);
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateCciL2O3TcProduct(prod, /*transposed=*/false,
                           /*with_fill_values=*/true);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(2L, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(2L, res.dimensions[harp_dimension_vertical]);

  // -999 in int32 is replaced with HARP int32 fill value (INT32_MAX).
  EXPECT_THAT(
      res.variables["scan_subindex"].int32_values,
      ElementsAre(harp_get_fill_value_for_type(harp_type_int32).int32_data, 2));
  ASSERT_EQ(2u, res.variables["datetime"].double_values.size());
  EXPECT_TRUE(std::isnan(res.variables["datetime"].double_values[0]));
  EXPECT_DOUBLE_EQ(5001.0, res.variables["datetime"].double_values[1]);

  EXPECT_THAT(res.variables["longitude"].double_values, ElementsAre(4.5, 5.5));
  EXPECT_THAT(res.variables["latitude"].double_values, ElementsAre(51.5, 52.5));
  EXPECT_THAT(res.variables["longitude_bounds"].double_values,
              ElementsAre(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0));
  EXPECT_THAT(res.variables["latitude_bounds"].double_values,
              ElementsAre(10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0));
  EXPECT_THAT(res.variables["sensor_zenith_angle"].double_values,
              ElementsAre(10.0, 15.0));
  EXPECT_THAT(res.variables["solar_zenith_angle"].double_values,
              ElementsAre(30.0, 35.0));
  EXPECT_THAT(res.variables["relative_azimuth_angle"].double_values,
              ElementsAre(45.0, 50.0));
  EXPECT_THAT(res.variables["O3_column_number_density"].double_values,
              ElementsAre(300.0, 310.0));
  EXPECT_THAT(res.variables["O3_column_number_density_uncertainty_random"]
                  .double_values,
              ElementsAre(5.0, 6.0));
  EXPECT_THAT(res.variables["temperature"].double_values,
              ElementsAre(225.0, 228.0));
  EXPECT_THAT(res.variables["cloud_fraction"].double_values,
              ElementsAre(0.2, 0.4));
  EXPECT_THAT(res.variables["cloud_top_pressure"].double_values,
              ElementsAre(50000.0, 60000.0));
  EXPECT_THAT(res.variables["cloud_top_albedo"].double_values,
              ElementsAre(0.8, 0.7));
  EXPECT_THAT(res.variables["scene_pressure"].double_values,
              ElementsAre(70000.0, 75000.0));
  EXPECT_THAT(res.variables["scene_albedo"].double_values,
              ElementsAre(0.3, 0.35));
  EXPECT_THAT(res.variables["surface_albedo"].double_values,
              ElementsAre(0.1, 0.15));
  EXPECT_THAT(res.variables["surface_altitude"].double_values,
              ElementsAre(12.0, 25.0));
  EXPECT_THAT(
      res.variables["pressure_bounds"].double_values,
      ElementsAre(1000.0, 500.0, 500.0, 100.0, 1010.0, 510.0, 510.0, 110.0));
  EXPECT_THAT(res.variables["O3_column_number_density_avk"].double_values,
              ElementsAre(0.1, 0.2, 0.3, 0.4));
  EXPECT_THAT(res.variables["O3_column_number_density_apriori"].double_values,
              ElementsAre(11.0, 12.0, 13.0, 14.0));
}

TEST(HarpIngestCciL2O3TcTest, IngestTransposed3DProduct) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule(kModuleName);
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateCciL2O3TcProduct(prod, /*transposed=*/true,
                           /*with_fill_values=*/false);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(2L, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(2L, res.dimensions[harp_dimension_vertical]);

  EXPECT_THAT(res.variables["scan_subindex"].int32_values, ElementsAre(1, 2));
  EXPECT_THAT(res.variables["datetime"].double_values,
              ElementsAre(5000.0, 5001.0));
  EXPECT_THAT(res.variables["longitude_bounds"].double_values,
              ElementsAre(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0));
  EXPECT_THAT(res.variables["latitude_bounds"].double_values,
              ElementsAre(10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0));
  EXPECT_THAT(
      res.variables["pressure_bounds"].double_values,
      ElementsAre(1000.0, 500.0, 500.0, 100.0, 1010.0, 510.0, 510.0, 110.0));
  EXPECT_THAT(res.variables["O3_column_number_density_avk"].double_values,
              ElementsAre(0.1, 0.2, 0.3, 0.4));
  EXPECT_THAT(res.variables["O3_column_number_density_apriori"].double_values,
              ElementsAre(11.0, 12.0, 13.0, 14.0));
}

TEST(HarpIngestCciL2O3TcTest, InitDimensionsErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule(kModuleName);
  ASSERT_NE(nullptr, mod);

  // 1. Missing /averaging_kernels.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. Non-array /averaging_kernels (coda_cursor_get_array_dim fails).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.AddField(prod.root(), "averaging_kernels", prod.MakeDouble(1.0)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. 1D /averaging_kernels (num_coda_dims < 2).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims1D[] = {2};
    constexpr double kVals[] = {1.0, 2.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "averaging_kernels",
                              prod.MakeDoubleArray(kDims1D, kVals)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 4. 4D /averaging_kernels (num_coda_dims > 3).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims4D[] = {1, 1, 1, 1};
    constexpr double kVals[] = {1.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "averaging_kernels",
                              prod.MakeDoubleArray(kDims4D, kVals)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
}

TEST(HarpIngestCciL2O3TcTest, ReadDatasetErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule(kModuleName);
  ASSERT_NE(nullptr, mod);

  // 1. Missing fields, wrong element counts, and wrong types in a single pass.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kAvkDims[] = {2, 2};
    constexpr double kAvkVals[] = {0.1, 0.2, 0.3, 0.4};
    ASSERT_TRUE(prod.AddField(prod.root(), "averaging_kernels",
                              prod.MakeDoubleArray(kAvkDims, kAvkVals)));

    // Wrong element count on /time and /longitude_corner and /latitude_corner
    // and /atmosphere_pressure_grid and /apriori_ozone_profile.
    constexpr long kWrongDim[] = {1};
    constexpr double kOneDouble[] = {1.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "time",
                              prod.MakeDoubleArray(kWrongDim, kOneDouble)));
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude_corner",
                              prod.MakeDoubleArray(kWrongDim, kOneDouble)));
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude_corner",
                              prod.MakeDoubleArray(kWrongDim, kOneDouble)));
    ASSERT_TRUE(prod.AddField(prod.root(), "atmosphere_pressure_grid",
                              prod.MakeDoubleArray(kWrongDim, kOneDouble)));
    ASSERT_TRUE(prod.AddField(prod.root(), "apriori_ozone_profile",
                              prod.MakeDoubleArray(kWrongDim, kOneDouble)));

    // Wrong type for int32 (/pixel_number) and double (/latitude).
    constexpr long kTimeDims[] = {2};
    constexpr std::string_view kBadStrs[] = {"a", "b"};
    ASSERT_TRUE(prod.AddField(prod.root(), "pixel_number",
                              prod.MakeStringArray(kTimeDims, kBadStrs)));
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                              prod.MakeStringArray(kTimeDims, kBadStrs)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["scan_subindex"].read_status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["longitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["pressure_bounds"].read_status);
    EXPECT_EQ(-1,
              res.variables["O3_column_number_density_apriori"].read_status);
    EXPECT_EQ(HARP_SUCCESS,
              res.variables["O3_column_number_density_avk"].read_status);
  }

  // 2. Non-numeric @FillValue attribute on int32 and double datasets, plus
  // wrong type for /averaging_kernels.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kAvkDims[] = {1, 1};
    constexpr std::string_view kBadAvk[] = {"bad"};
    ASSERT_TRUE(prod.AddField(prod.root(), "averaging_kernels",
                              prod.MakeStringArray(kAvkDims, kBadAvk)));

    constexpr long kOneDim[] = {1};
    constexpr int32_t kPix[] = {1};
    constexpr double kTime[] = {100.0};
    constexpr std::string_view kBadFill[] = {"bad_fill"};
    CodaMemNode* pix_node = prod.MakeInt32Array(kOneDim, kPix);
    ASSERT_TRUE(prod.AddAttribute(pix_node, "FillValue",
                                  prod.MakeStringArray(kOneDim, kBadFill)));
    ASSERT_TRUE(prod.AddField(prod.root(), "pixel_number", pix_node));

    CodaMemNode* time_node = prod.MakeDoubleArray(kOneDim, kTime);
    ASSERT_TRUE(prod.AddAttribute(time_node, "FillValue",
                                  prod.MakeStringArray(kOneDim, kBadFill)));
    ASSERT_TRUE(prod.AddField(prod.root(), "time", time_node));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["scan_subindex"].read_status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["O3_column_number_density_avk"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
