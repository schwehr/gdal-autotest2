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
// Unit tests for ESA CCI L3 O3 nadir profile ingestion
// (libharp/harp-ingest-cci_l3_o3_np.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-cci_l3_o3_np.c

#include <cmath>
#include <string_view>

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

using ::testing::DoubleEq;
using ::testing::ElementsAre;
using ::testing::FloatEq;

void PopulateValidNpProduct(CodaMemProduct& prod) {
  ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_start",
                                prod.MakeString("20200515T000000Z")));
  ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_end",
                                prod.MakeString("20200515T235959Z")));

  // [num_lat=2, num_lon=2, num_vertical=3].
  constexpr long kLonDims[] = {2};
  constexpr float kLons[] = {-60.0f, 60.0f};
  constexpr long kLatDims[] = {2};
  constexpr float kLats[] = {-30.0f, 30.0f};
  constexpr long kVertDims[] = {3};
  constexpr float kAirPress[] = {1000.0f, 400.0f, 100.0f};
  constexpr long kSurfDims[] = {2, 2};
  constexpr float kSurfPress[] = {900.0f, 810.0f, 1024.0f, 961.0f};

  ASSERT_TRUE(
      prod.AddField(prod.root(), "lon", prod.MakeFloatArray(kLonDims, kLons)));
  ASSERT_TRUE(
      prod.AddField(prod.root(), "lat", prod.MakeFloatArray(kLatDims, kLats)));
  ASSERT_TRUE(prod.AddField(prod.root(), "air_pressure",
                            prod.MakeFloatArray(kVertDims, kAirPress)));
  ASSERT_TRUE(prod.AddField(prod.root(), "surface_pressure",
                            prod.MakeFloatArray(kSurfDims, kSurfPress)));

  // 3D shape: [num_vertical=3, num_lat=2, num_lon=2] -> reordered {2, 0, 1}
  // to [num_lat=2, num_lon=2, num_vertical=3].
  constexpr long kDataDims[] = {3, 2, 2};
  constexpr float kNdens[] = {
      -999.0f, 2.0f,  3.0f,  4.0f,  // v=0
      5.0f,    6.0f,  7.0f,  8.0f,  // v=1
      9.0f,    10.0f, 11.0f, 12.0f  // v=2
  };
  constexpr float kNdensErr[] = {
      0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 1.1f, 1.2f,
  };
  constexpr float kVmr[] = {
      1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f, 11.0f, 12.0f,
  };
  constexpr float kVmrErr[] = {
      0.01f, 0.02f, 0.03f, 0.04f, 0.05f, 0.06f,
      0.07f, 0.08f, 0.09f, 0.10f, 0.11f, 0.12f,
  };

  CodaMemNode* ndens_node = prod.MakeFloatArray(kDataDims, kNdens);
  ASSERT_NE(nullptr, ndens_node);
  constexpr long kFillDims[] = {1};
  constexpr float kFillVals[] = {-999.0f};
  ASSERT_TRUE(prod.AddAttribute(ndens_node, "FillValue",
                                prod.MakeFloatArray(kFillDims, kFillVals)));
  ASSERT_TRUE(prod.AddField(prod.root(), "O3_ndens", ndens_node));

  ASSERT_TRUE(prod.AddField(prod.root(), "O3e_ndens",
                            prod.MakeFloatArray(kDataDims, kNdensErr)));
  ASSERT_TRUE(prod.AddField(prod.root(), "O3_vmr",
                            prod.MakeFloatArray(kDataDims, kVmr)));
  ASSERT_TRUE(prod.AddField(prod.root(), "O3e_vmr",
                            prod.MakeFloatArray(kDataDims, kVmrErr)));
}

TEST(HarpIngestCciL3O3NpTest, IngestSuccessAllVariables) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("ESACCI_OZONE_L3_NP");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateValidNpProduct(prod);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(1L, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(2L, res.dimensions[harp_dimension_latitude]);
  EXPECT_EQ(2L, res.dimensions[harp_dimension_longitude]);
  EXPECT_EQ(3L, res.dimensions[harp_dimension_vertical]);

  double expected_start = 0.0;
  double expected_stop = 0.0;
  ASSERT_EQ(0, coda_time_string_to_double("yyyyMMdd'T'HHmmss'Z'",
                                          "20200515T000000Z", &expected_start));
  ASSERT_EQ(0, coda_time_string_to_double("yyyyMMdd'T'HHmmss'Z'",
                                          "20200515T235959Z", &expected_stop));
  EXPECT_THAT(res.variables["datetime_start"].double_values,
              ElementsAre(DoubleEq(expected_start)));
  EXPECT_THAT(res.variables["datetime_stop"].double_values,
              ElementsAre(DoubleEq(expected_stop)));
  EXPECT_THAT(res.variables["longitude"].float_values,
              ElementsAre(-60.0f, 60.0f));
  EXPECT_THAT(res.variables["latitude"].float_values,
              ElementsAre(-30.0f, 30.0f));

  // Level 0 replaced with surface_pressure; levels 1..2 from air_pressure.
  EXPECT_THAT(res.variables["pressure"].float_values,
              ElementsAre(900.0f, 400.0f, 100.0f, 810.0f, 400.0f, 100.0f,
                          1024.0f, 400.0f, 100.0f, 961.0f, 400.0f, 100.0f));

  // Log-linear pressure_bounds:
  // profile 0: [900, 600, 600, 200, 200, 100]
  // profile 1: [810, sqrt(810*400), sqrt(810*400), 200, 200, 100]
  // profile 2: [1024, 640, 640, 200, 200, 100]
  // profile 3: [961, 620, 620, 200, 200, 100]
  EXPECT_THAT(res.variables["pressure_bounds"].float_values,
              ElementsAre(FloatEq(900.0f), FloatEq(600.0f), FloatEq(600.0f),
                          FloatEq(200.0f), FloatEq(200.0f), FloatEq(100.0f),
                          FloatEq(810.0f),
                          FloatEq(static_cast<float>(std::sqrt(810.0 * 400.0))),
                          FloatEq(static_cast<float>(std::sqrt(810.0 * 400.0))),
                          FloatEq(200.0f), FloatEq(200.0f), FloatEq(100.0f),
                          FloatEq(1024.0f), FloatEq(640.0f), FloatEq(640.0f),
                          FloatEq(200.0f), FloatEq(200.0f), FloatEq(100.0f),
                          FloatEq(961.0f), FloatEq(620.0f), FloatEq(620.0f),
                          FloatEq(200.0f), FloatEq(200.0f), FloatEq(100.0f)));

  // Upstream read_and_reorder_dataset_3d passes order={2, 0, 1} to
  // harp_array_transpose (where order maps dst_dim -> src_dim), producing
  // [lon, vertical, lat] ordering rather than [lat, lon, vertical].
  // O3_ndens has @FillValue=-999.0f at (v=0, lat=0, lon=0) -> NaN at index 0.
  const auto& ndens = res.variables["O3_number_density"].float_values;
  ASSERT_EQ(12u, ndens.size());
  EXPECT_TRUE(std::isnan(ndens[0]));
  EXPECT_EQ(3.0f, ndens[1]);
  EXPECT_EQ(5.0f, ndens[2]);
  EXPECT_EQ(7.0f, ndens[3]);
  EXPECT_EQ(9.0f, ndens[4]);
  EXPECT_EQ(11.0f, ndens[5]);
  EXPECT_EQ(2.0f, ndens[6]);

  EXPECT_THAT(res.variables["O3_number_density_uncertainty"].float_values,
              ElementsAre(0.1f, 0.3f, 0.5f, 0.7f, 0.9f, 1.1f, 0.2f, 0.4f, 0.6f,
                          0.8f, 1.0f, 1.2f));
  EXPECT_THAT(res.variables["O3_volume_mixing_ratio"].float_values,
              ElementsAre(1.0f, 3.0f, 5.0f, 7.0f, 9.0f, 11.0f, 2.0f, 4.0f, 6.0f,
                          8.0f, 10.0f, 12.0f));
  EXPECT_THAT(res.variables["O3_volume_mixing_ratio_uncertainty"].float_values,
              ElementsAre(0.01f, 0.03f, 0.05f, 0.07f, 0.09f, 0.11f, 0.02f,
                          0.04f, 0.06f, 0.08f, 0.10f, 0.12f));
}

TEST(HarpIngestCciL3O3NpTest, InitDimensionErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ESACCI_OZONE_L3_NP");
  ASSERT_NE(nullptr, mod);

  constexpr long kDims1D[] = {1};
  constexpr float kVals1D[] = {1.0f};
  constexpr long kDims2D[] = {1, 2};
  constexpr float kVals2D[] = {1.0f, 2.0f};

  // 1. Missing /lon.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 2. Scalar /lon.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "lon", prod.MakeFloat(1.0f)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 3. 2D /lon.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "lon",
                              prod.MakeFloatArray(kDims2D, kVals2D)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 4. Missing /lat.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "lon",
                              prod.MakeFloatArray(kDims1D, kVals1D)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 5. Scalar /lat.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "lon",
                              prod.MakeFloatArray(kDims1D, kVals1D)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lat", prod.MakeFloat(1.0f)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 6. 2D /lat.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "lon",
                              prod.MakeFloatArray(kDims1D, kVals1D)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lat",
                              prod.MakeFloatArray(kDims2D, kVals2D)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 7. Missing /air_pressure.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "lon",
                              prod.MakeFloatArray(kDims1D, kVals1D)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lat",
                              prod.MakeFloatArray(kDims1D, kVals1D)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 8. Scalar /air_pressure.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "lon",
                              prod.MakeFloatArray(kDims1D, kVals1D)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lat",
                              prod.MakeFloatArray(kDims1D, kVals1D)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "air_pressure", prod.MakeFloat(1000.0f)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 9. 2D /air_pressure.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "lon",
                              prod.MakeFloatArray(kDims1D, kVals1D)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lat",
                              prod.MakeFloatArray(kDims1D, kVals1D)));
    ASSERT_TRUE(prod.AddField(prod.root(), "air_pressure",
                              prod.MakeFloatArray(kDims2D, kVals2D)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }
}

TEST(HarpIngestCciL3O3NpTest, ReadDatetimeAndDatasetErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ESACCI_OZONE_L3_NP");
  ASSERT_NE(nullptr, mod);

  // 1. Missing @time_coverage_start, non-string @time_coverage_end, missing
  // /surface_pressure, invalid @FillValue on /O3_ndens, wrong element count on
  // /O3e_ndens, and wrong element type on /O3_vmr.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims1D[] = {1};
    constexpr float kVals1D[] = {1.0f};
    constexpr long kDims3D[] = {1, 1, 1};
    constexpr std::string_view kBadStr[] = {"bad"};
    constexpr long kWrongDims[] = {2};
    constexpr float kWrongVals[] = {1.0f, 2.0f};

    ASSERT_TRUE(prod.AddField(prod.root(), "lon",
                              prod.MakeFloatArray(kDims1D, kVals1D)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lat",
                              prod.MakeFloatArray(kDims1D, kVals1D)));
    ASSERT_TRUE(prod.AddField(prod.root(), "air_pressure",
                              prod.MakeFloatArray(kDims1D, kVals1D)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_end",
                                  prod.MakeInt32(42)));

    CodaMemNode* ndens = prod.MakeFloatArray(kDims3D, kVals1D);
    ASSERT_NE(nullptr, ndens);
    ASSERT_TRUE(prod.AddAttribute(ndens, "FillValue",
                                  prod.MakeStringArray(kDims1D, kBadStr)));
    ASSERT_TRUE(prod.AddField(prod.root(), "O3_ndens", ndens));
    ASSERT_TRUE(prod.AddField(prod.root(), "O3e_ndens",
                              prod.MakeFloatArray(kWrongDims, kWrongVals)));
    ASSERT_TRUE(prod.AddField(prod.root(), "O3_vmr",
                              prod.MakeStringArray(kDims3D, kBadStr)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_stop"].read_status);
    EXPECT_EQ(-1, res.variables["pressure"].read_status);
    EXPECT_EQ(-1, res.variables["pressure_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["O3_number_density"].read_status);
    EXPECT_EQ(-1, res.variables["O3_number_density_uncertainty"].read_status);
    EXPECT_EQ(-1, res.variables["O3_volume_mixing_ratio"].read_status);
    EXPECT_EQ(-1,
              res.variables["O3_volume_mixing_ratio_uncertainty"].read_status);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 2. Wrong datetime length and invalid datetime format; plus non-float
  // /air_pressure failing inside read_pressure.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims1D[] = {1};
    constexpr float kVals1D[] = {1.0f};
    constexpr std::string_view kBadPress[] = {"bad"};
    ASSERT_TRUE(prod.AddField(prod.root(), "lon",
                              prod.MakeFloatArray(kDims1D, kVals1D)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lat",
                              prod.MakeFloatArray(kDims1D, kVals1D)));
    ASSERT_TRUE(prod.AddField(prod.root(), "air_pressure",
                              prod.MakeStringArray(kDims1D, kBadPress)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_start",
                                  prod.MakeString("20200515")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_end",
                                  prod.MakeString("99999999T999999Z")));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_stop"].read_status);
    EXPECT_EQ(-1, res.variables["pressure"].read_status);
    harp_set_error(HARP_SUCCESS, nullptr);
  }
}

}  // namespace
}  // namespace stcorp_harp
