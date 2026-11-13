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
// Unit tests for CCI L4 O3 nadir profile ingestion
// (libharp/harp-ingest-cci_l4_o3_np.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-cci_l4_o3_np.c

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
using ::testing::FloatEq;
using ::testing::IsNan;

// Populates a valid ESACCI_OZONE_L4_NP product with dimensions
// time=1, layers=2, lat=1, lon=2 (plus optional FillValue attributes).
void PopulateCciL4O3NpProduct(CodaMemProduct& prod, bool with_fill_values) {
  ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_start",
                                prod.MakeString("20200515T000000Z")));

  constexpr long kTimeDims[] = {1};
  constexpr double kTimeVals[] = {12.0};
  CodaMemNode* time_node = prod.MakeDoubleArray(kTimeDims, kTimeVals);
  if (with_fill_values) {
    ASSERT_TRUE(
        prod.AddAttribute(time_node, "FillValue", prod.MakeDouble(-999.0)));
  }
  ASSERT_TRUE(prod.AddField(prod.root(), "time", time_node));

  constexpr long kLonDims[] = {2};
  constexpr float kLonVals[] = {10.0f, -999.0f};
  CodaMemNode* lon_node = prod.MakeFloatArray(kLonDims, kLonVals);
  if (with_fill_values) {
    ASSERT_TRUE(
        prod.AddAttribute(lon_node, "FillValue", prod.MakeFloat(-999.0f)));
  }
  ASSERT_TRUE(prod.AddField(prod.root(), "lon", lon_node));

  constexpr long kLatDims[] = {1};
  constexpr float kLatVals[] = {45.0f};
  ASSERT_TRUE(prod.AddField(prod.root(), "lat",
                            prod.MakeFloatArray(kLatDims, kLatVals)));

  constexpr long kLayersDims[] = {2};
  constexpr float kLayersVals[] = {1.0f, 2.0f};
  ASSERT_TRUE(prod.AddField(prod.root(), "layers",
                            prod.MakeFloatArray(kLayersDims, kLayersVals)));

  // 4D layout in file: [num_time=1, num_vertical=2, num_lat=1, num_lon=2].
  // Transposed in HARP to [num_time=1, num_lat=1, num_lon=2, num_vertical=2].
  constexpr long k4dDims[] = {1, 2, 1, 2};
  constexpr float kGphVals[] = {100.0f, 200.0f, 1500.0f, 2500.0f};
  constexpr float kTempVals[] = {280.0f, 290.0f, 250.0f, 260.0f};
  constexpr float kO3DensVals[] = {1.0e18f, 2.0e18f, 3.0e18f, 4.0e18f};
  constexpr float kO3sDensVals[] = {1.0e17f, 2.0e17f, 3.0e17f, 4.0e17f};
  constexpr float kO3VmrVals[] = {1e-6f, 2e-6f, 3e-6f, 4e-6f};
  constexpr float kO3sVmrVals[] = {1e-7f, 2e-7f, 3e-7f, 4e-7f};
  ASSERT_TRUE(prod.AddField(prod.root(), "Gph",
                            prod.MakeFloatArray(k4dDims, kGphVals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "Temperature",
                            prod.MakeFloatArray(k4dDims, kTempVals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "O3_dens",
                            prod.MakeFloatArray(k4dDims, kO3DensVals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "O3s_dens",
                            prod.MakeFloatArray(k4dDims, kO3sDensVals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "O3_vmr",
                            prod.MakeFloatArray(k4dDims, kO3VmrVals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "O3s_vmr",
                            prod.MakeFloatArray(k4dDims, kO3sVmrVals)));

  constexpr long kCoefDims[] = {2};
  constexpr float kFa[] = {100.0f, 200.0f};
  constexpr float kFb[] = {0.5f, 0.25f};
  ASSERT_TRUE(prod.AddField(prod.root(), "Hybride_coef_fa",
                            prod.MakeFloatArray(kCoefDims, kFa)));
  ASSERT_TRUE(prod.AddField(prod.root(), "Hybride_coef_fb",
                            prod.MakeFloatArray(kCoefDims, kFb)));

  constexpr long kLevelCoefDims[] = {3};
  constexpr float kA[] = {0.0f, 100.0f, 200.0f};
  constexpr float kB[] = {1.0f, 0.5f, 0.0f};
  ASSERT_TRUE(prod.AddField(prod.root(), "Hybride_coef_a",
                            prod.MakeFloatArray(kLevelCoefDims, kA)));
  ASSERT_TRUE(prod.AddField(prod.root(), "Hybride_coef_b",
                            prod.MakeFloatArray(kLevelCoefDims, kB)));

  constexpr long kPsurfDims[] = {1, 1, 2};
  constexpr float kPsurf[] = {1000.0f, 800.0f};
  ASSERT_TRUE(prod.AddField(prod.root(), "Psurf",
                            prod.MakeFloatArray(kPsurfDims, kPsurf)));
}

void AddBaseDimensions(CodaMemProduct& prod) {
  constexpr long kOne[] = {1};
  constexpr double kD[] = {0.0};
  constexpr float kF[] = {0.0f};
  ASSERT_TRUE(
      prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kOne, kD)));
  ASSERT_TRUE(prod.AddField(prod.root(), "lon", prod.MakeFloatArray(kOne, kF)));
  ASSERT_TRUE(prod.AddField(prod.root(), "lat", prod.MakeFloatArray(kOne, kF)));
  ASSERT_TRUE(
      prod.AddField(prod.root(), "layers", prod.MakeFloatArray(kOne, kF)));
}

TEST(HarpIngestCciL4O3NpTest, IngestFullProductAndFillValues) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("ESACCI_OZONE_L4_NP");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateCciL4O3NpProduct(prod, /*with_fill_values=*/true);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(1, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(1, res.dimensions[harp_dimension_latitude]);
  EXPECT_EQ(2, res.dimensions[harp_dimension_longitude]);
  EXPECT_EQ(2, res.dimensions[harp_dimension_vertical]);

  double expected_epoch = 0.0;
  ASSERT_EQ(0, coda_time_string_to_double("yyyyMMdd'T'HHmmss'Z'",
                                          "20200515T000000Z", &expected_epoch));
  EXPECT_THAT(res.variables["datetime"].double_values,
              ElementsAre(expected_epoch + 12.0 * 3600.0));

  EXPECT_THAT(res.variables["longitude"].float_values,
              ElementsAre(FloatEq(10.0f), IsNan()));
  EXPECT_THAT(res.variables["latitude"].float_values, ElementsAre(45.0f));

  // Verify 4D transposition [1, 2, 1, 2] -> [1, 1, 2, 2].
  EXPECT_THAT(res.variables["geopotential_height"].float_values,
              ElementsAre(100.0f, 1500.0f, 200.0f, 2500.0f));
  EXPECT_THAT(res.variables["temperature"].float_values,
              ElementsAre(280.0f, 250.0f, 290.0f, 260.0f));
  EXPECT_THAT(res.variables["O3_column_number_density"].float_values,
              ElementsAre(1.0e18f, 3.0e18f, 2.0e18f, 4.0e18f));
  EXPECT_THAT(
      res.variables["O3_column_number_density_uncertainty"].float_values,
      ElementsAre(1.0e17f, 3.0e17f, 2.0e17f, 4.0e17f));
  EXPECT_THAT(res.variables["O3_volume_mixing_ratio"].float_values,
              ElementsAre(1e-6f, 3e-6f, 2e-6f, 4e-6f));
  EXPECT_THAT(res.variables["O3_volume_mixing_ratio_uncertainty"].float_values,
              ElementsAre(1e-7f, 3e-7f, 2e-7f, 4e-7f));

  // Verify hybrid pressure interpolation and pressure bounds.
  EXPECT_THAT(res.variables["pressure"].float_values,
              ElementsAre(600.0f, 450.0f, 500.0f, 400.0f));
  EXPECT_THAT(res.variables["pressure_bounds"].float_values,
              ElementsAre(1000.0f, 600.0f, 600.0f, 200.0f, 800.0f, 500.0f,
                          500.0f, 200.0f));
}

TEST(HarpIngestCciL4O3NpTest, InitDimensionsErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ESACCI_OZONE_L4_NP");
  ASSERT_NE(nullptr, mod);

  constexpr long kOne[] = {1};
  constexpr long kTwoD[] = {1, 1};
  constexpr float kF[] = {0.0f};
  constexpr double kD[] = {0.0};

  // 1. Missing /time, scalar /time, and 2D /time.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "time", prod.MakeDouble(0.0)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kTwoD, kD)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 2. Missing /lon, scalar /lon, and 2D /lon.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kOne, kD)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kOne, kD)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lon", prod.MakeFloat(0.0f)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kOne, kD)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lon", prod.MakeFloatArray(kTwoD, kF)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 3. Missing /lat, scalar /lat, and 2D /lat.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kOne, kD)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lon", prod.MakeFloatArray(kOne, kF)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kOne, kD)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lon", prod.MakeFloatArray(kOne, kF)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lat", prod.MakeFloat(0.0f)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kOne, kD)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lon", prod.MakeFloatArray(kOne, kF)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lat", prod.MakeFloatArray(kTwoD, kF)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 4. Missing /layers, scalar /layers, and 2D /layers.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kOne, kD)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lon", prod.MakeFloatArray(kOne, kF)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lat", prod.MakeFloatArray(kOne, kF)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kOne, kD)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lon", prod.MakeFloatArray(kOne, kF)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lat", prod.MakeFloatArray(kOne, kF)));
    ASSERT_TRUE(prod.AddField(prod.root(), "layers", prod.MakeFloat(0.0f)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.AddField(prod.root(), "time", prod.MakeDoubleArray(kOne, kD)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lon", prod.MakeFloatArray(kOne, kF)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lat", prod.MakeFloatArray(kOne, kF)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "layers", prod.MakeFloatArray(kTwoD, kF)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
}

TEST(HarpIngestCciL4O3NpTest, DateAndDatasetErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ESACCI_OZONE_L4_NP");
  ASSERT_NE(nullptr, mod);

  // 1. Missing @time_coverage_start, non-string, wrong length, non-zero time,
  // and unparseable date string (also covers missing /Hybride_coef_fa and
  // /Hybride_coef_a in read_pressure / read_pressure_bounds).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    AddBaseDimensions(prod);
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["pressure"].read_status);
    EXPECT_EQ(-1, res.variables["pressure_bounds"].read_status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    AddBaseDimensions(prod);
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_start",
                                  prod.MakeInt32(20200515)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  for (std::string_view bad_date :
       {"20200515", "20200515T120000Z", "20209999T000000Z"}) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    AddBaseDimensions(prod);
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_start",
                                  prod.MakeString(bad_date)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }

  // 2. Wrong element count, wrong types, and invalid @FillValue attributes.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_start",
                                  prod.MakeString("20200515T000000Z")));
    constexpr long kOne[] = {1};
    constexpr long kTwo[] = {2};
    constexpr std::string_view kStr1[] = {"bad"};
    constexpr double kD[] = {0.0};
    constexpr float kF[] = {0.0f};
    constexpr float kF2[] = {0.0f, 1.0f};

    CodaMemNode* time_node = prod.MakeStringArray(kOne, kStr1);
    ASSERT_TRUE(prod.AddField(prod.root(), "time", time_node));

    CodaMemNode* lon_node = prod.MakeFloatArray(kOne, kF);
    ASSERT_TRUE(
        prod.AddAttribute(lon_node, "FillValue", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "lon", lon_node));

    CodaMemNode* lat_node = prod.MakeStringArray(kOne, kStr1);
    ASSERT_TRUE(prod.AddField(prod.root(), "lat", lat_node));

    ASSERT_TRUE(
        prod.AddField(prod.root(), "layers", prod.MakeFloatArray(kOne, kF)));
    // Mismatched element count on /Gph (2 instead of 1).
    ASSERT_TRUE(
        prod.AddField(prod.root(), "Gph", prod.MakeFloatArray(kTwo, kF2)));
    // Missing /Psurf and partial /Hybride_coef_* triggers pressure errors.
    ASSERT_TRUE(prod.AddField(prod.root(), "Hybride_coef_fa",
                              prod.MakeFloatArray(kOne, kF)));
    ASSERT_TRUE(prod.AddField(prod.root(), "Hybride_coef_a",
                              prod.MakeFloatArray(kTwo, kF2)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["geopotential_height"].read_status);
    EXPECT_EQ(-1, res.variables["pressure"].read_status);
    EXPECT_EQ(-1, res.variables["pressure_bounds"].read_status);
    (void)kD;
  }

  // 3. Invalid @FillValue on double /time and missing /Psurf with both coefs.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_start",
                                  prod.MakeString("20200515T000000Z")));
    constexpr long kOne[] = {1};
    constexpr long kTwo[] = {2};
    constexpr double kD[] = {0.0};
    constexpr float kF[] = {0.0f};
    constexpr float kF2[] = {0.0f, 1.0f};

    CodaMemNode* time_node = prod.MakeDoubleArray(kOne, kD);
    ASSERT_TRUE(
        prod.AddAttribute(time_node, "FillValue", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "time", time_node));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lon", prod.MakeFloatArray(kOne, kF)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lat", prod.MakeFloatArray(kOne, kF)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "layers", prod.MakeFloatArray(kOne, kF)));
    ASSERT_TRUE(prod.AddField(prod.root(), "Hybride_coef_fa",
                              prod.MakeFloatArray(kOne, kF)));
    ASSERT_TRUE(prod.AddField(prod.root(), "Hybride_coef_fb",
                              prod.MakeFloatArray(kOne, kF)));
    ASSERT_TRUE(prod.AddField(prod.root(), "Hybride_coef_a",
                              prod.MakeFloatArray(kTwo, kF2)));
    ASSERT_TRUE(prod.AddField(prod.root(), "Hybride_coef_b",
                              prod.MakeFloatArray(kTwo, kF2)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["pressure"].read_status);
    EXPECT_EQ(-1, res.variables["pressure_bounds"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
