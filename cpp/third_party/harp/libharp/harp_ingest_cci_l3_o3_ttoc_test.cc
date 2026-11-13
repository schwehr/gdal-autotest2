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
// Unit tests for ESA CCI L3 O3 tropical tropospheric ozone column ingestion
// (libharp/harp-ingest-cci_l3_o3_ttoc.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-cci_l3_o3_ttoc.c

#include <cmath>
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

using ::testing::DoubleEq;
using ::testing::ElementsAre;

constexpr double kMolPerM2ToDu = 2241.15;
constexpr double kMToKm = 0.001;

void PopulateValidTtocProduct(CodaMemProduct& prod) {
  constexpr long kLatDims[] = {2};
  constexpr double kLats[] = {-15.0, 15.0};
  constexpr long kLonDims[] = {2};
  constexpr double kLons[] = {-60.0, 60.0};
  constexpr long kGridDims[] = {2, 2};

  ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                            prod.MakeDoubleArray(kLatDims, kLats)));
  ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                            prod.MakeDoubleArray(kLonDims, kLons)));

  CodaMemNode* product_rec = prod.MakeRecord();
  CodaMemNode* support_rec = prod.MakeRecord();
  CodaMemNode* detailed_rec = prod.MakeRecord();
  CodaMemNode* total_o3_rec = prod.MakeRecord();
  CodaMemNode* strat_o3_rec = prod.MakeRecord();
  CodaMemNode* surf_rec = prod.MakeRecord();
  CodaMemNode* cloud_rec = prod.MakeRecord();

  // Include -999.0 fill value in both scaled and unscaled datasets.
  constexpr double kTotO3[] = {-999.0, 0.12, 0.13, 0.14};
  constexpr double kTotO3Err[] = {0.01, 0.02, 0.03, 0.04};
  ASSERT_TRUE(prod.AddField(total_o3_rec, "total_ozone_column",
                            prod.MakeDoubleArray(kGridDims, kTotO3)));
  ASSERT_TRUE(prod.AddField(total_o3_rec, "total_ozone_column_standard_error",
                            prod.MakeDoubleArray(kGridDims, kTotO3Err)));

  constexpr double kStratO3[] = {0.10, 0.11, 0.12, 0.13};
  constexpr double kStratO3Err[] = {0.005, 0.006, 0.007, 0.008};
  ASSERT_TRUE(prod.AddField(strat_o3_rec, "stratospheric_ozone_column",
                            prod.MakeDoubleArray(kGridDims, kStratO3)));
  ASSERT_TRUE(prod.AddField(strat_o3_rec,
                            "stratospheric_ozone_column_standard_error",
                            prod.MakeDoubleArray(kGridDims, kStratO3Err)));

  constexpr double kSurfAlbedo[] = {-999.0, 0.2, 0.3, 0.4};
  constexpr double kSurfAlt[] = {100.0, 200.0, 300.0, 400.0};
  ASSERT_TRUE(prod.AddField(surf_rec, "surface_albedo",
                            prod.MakeDoubleArray(kGridDims, kSurfAlbedo)));
  ASSERT_TRUE(prod.AddField(surf_rec, "surface_altitude",
                            prod.MakeDoubleArray(kGridDims, kSurfAlt)));

  constexpr double kCloudAlbedo[] = {0.5, 0.6, 0.7, 0.8};
  constexpr double kCloudAlbedoErr[] = {0.05, 0.06, 0.07, 0.08};
  constexpr double kCloudTopAlt[] = {5000.0, 6000.0, 7000.0, 8000.0};
  constexpr double kCloudTopAltErr[] = {100.0, 200.0, 300.0, 400.0};
  ASSERT_TRUE(prod.AddField(cloud_rec, "cloud_albedo",
                            prod.MakeDoubleArray(kGridDims, kCloudAlbedo)));
  ASSERT_TRUE(prod.AddField(cloud_rec, "cloud_albedo_standard_error",
                            prod.MakeDoubleArray(kGridDims, kCloudAlbedoErr)));
  ASSERT_TRUE(prod.AddField(cloud_rec, "cloud_top_altitude",
                            prod.MakeDoubleArray(kGridDims, kCloudTopAlt)));
  ASSERT_TRUE(prod.AddField(cloud_rec, "cloud_top_altitude_standard_error",
                            prod.MakeDoubleArray(kGridDims, kCloudTopAltErr)));

  ASSERT_TRUE(prod.AddField(detailed_rec, "TOTAL_OZONE", total_o3_rec));
  ASSERT_TRUE(prod.AddField(detailed_rec, "STRATOSPHERIC_OZONE", strat_o3_rec));
  ASSERT_TRUE(prod.AddField(detailed_rec, "SURFACE_PROPERTIES", surf_rec));
  ASSERT_TRUE(prod.AddField(detailed_rec, "CLOUD_PARAMETERS", cloud_rec));
  ASSERT_TRUE(prod.AddField(support_rec, "DETAILED_RESULTS", detailed_rec));
  ASSERT_TRUE(prod.AddField(product_rec, "SUPPORT_DATA", support_rec));

  constexpr double kTropO3[] = {0.02, 0.03, 0.04, 0.05};
  constexpr double kTropO3Err[] = {0.002, 0.003, 0.004, 0.005};
  constexpr double kTropVmr[] = {40.0, 45.0, 50.0, 55.0};
  constexpr double kTropVmrErr[] = {4.0, 4.5, 5.0, 5.5};
  ASSERT_TRUE(prod.AddField(product_rec, "tropospheric_ozone_column",
                            prod.MakeDoubleArray(kGridDims, kTropO3)));
  ASSERT_TRUE(prod.AddField(product_rec,
                            "tropospheric_ozone_column_standard_error",
                            prod.MakeDoubleArray(kGridDims, kTropO3Err)));
  ASSERT_TRUE(prod.AddField(product_rec, "tropospheric_ozone_mixing_ratio",
                            prod.MakeDoubleArray(kGridDims, kTropVmr)));
  ASSERT_TRUE(prod.AddField(product_rec,
                            "tropospheric_ozone_mixing_ratio_standard_error",
                            prod.MakeDoubleArray(kGridDims, kTropVmrErr)));

  ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", product_rec));
}

TEST(HarpIngestCciL3O3TtocTest, IngestSuccessAllVariables) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod =
      FindIngestionModule("ESACCI_OZONE_L3_TTOC");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateValidTtocProduct(prod);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(2L, res.dimensions[harp_dimension_latitude]);
  EXPECT_EQ(2L, res.dimensions[harp_dimension_longitude]);

  EXPECT_THAT(res.variables["latitude"].double_values,
              ElementsAre(-15.0, 15.0));
  EXPECT_THAT(res.variables["longitude"].double_values,
              ElementsAre(-60.0, 60.0));

  const auto& tot_o3 = res.variables["O3_column_number_density"].double_values;
  ASSERT_EQ(4u, tot_o3.size());
  EXPECT_TRUE(std::isnan(tot_o3[0]));
  EXPECT_THAT(tot_o3[1], DoubleEq(0.12 * kMolPerM2ToDu));
  EXPECT_THAT(tot_o3[2], DoubleEq(0.13 * kMolPerM2ToDu));
  EXPECT_THAT(tot_o3[3], DoubleEq(0.14 * kMolPerM2ToDu));

  EXPECT_THAT(
      res.variables["O3_column_number_density_uncertainty"].double_values,
      ElementsAre(
          DoubleEq(0.01 * kMolPerM2ToDu), DoubleEq(0.02 * kMolPerM2ToDu),
          DoubleEq(0.03 * kMolPerM2ToDu), DoubleEq(0.04 * kMolPerM2ToDu)));
  EXPECT_THAT(
      res.variables["stratospheric_O3_column_number_density"].double_values,
      ElementsAre(
          DoubleEq(0.10 * kMolPerM2ToDu), DoubleEq(0.11 * kMolPerM2ToDu),
          DoubleEq(0.12 * kMolPerM2ToDu), DoubleEq(0.13 * kMolPerM2ToDu)));
  EXPECT_THAT(
      res.variables["stratospheric_O3_column_number_density_uncertainty"]
          .double_values,
      ElementsAre(
          DoubleEq(0.005 * kMolPerM2ToDu), DoubleEq(0.006 * kMolPerM2ToDu),
          DoubleEq(0.007 * kMolPerM2ToDu), DoubleEq(0.008 * kMolPerM2ToDu)));
  EXPECT_THAT(
      res.variables["tropospheric_O3_column_number_density"].double_values,
      ElementsAre(
          DoubleEq(0.02 * kMolPerM2ToDu), DoubleEq(0.03 * kMolPerM2ToDu),
          DoubleEq(0.04 * kMolPerM2ToDu), DoubleEq(0.05 * kMolPerM2ToDu)));
  EXPECT_THAT(res.variables["tropospheric_O3_column_number_density_uncertainty"]
                  .double_values,
              ElementsAre(DoubleEq(0.002 * kMolPerM2ToDu),
                          DoubleEq(0.003 * kMolPerM2ToDu),
                          DoubleEq(0.004 * kMolPerM2ToDu),
                          DoubleEq(0.005 * kMolPerM2ToDu)));
  EXPECT_THAT(
      res.variables["tropospheric_O3_volume_mixing_ratio"].double_values,
      ElementsAre(40.0, 45.0, 50.0, 55.0));
  EXPECT_THAT(res.variables["tropospheric_O3_volume_mixing_ratio_uncertainty"]
                  .double_values,
              ElementsAre(4.0, 4.5, 5.0, 5.5));

  const auto& surf_alb = res.variables["surface_albedo"].double_values;
  ASSERT_EQ(4u, surf_alb.size());
  EXPECT_TRUE(std::isnan(surf_alb[0]));
  EXPECT_EQ(0.2, surf_alb[1]);
  EXPECT_EQ(0.3, surf_alb[2]);
  EXPECT_EQ(0.4, surf_alb[3]);

  EXPECT_THAT(res.variables["surface_altitude"].double_values,
              ElementsAre(100.0, 200.0, 300.0, 400.0));
  EXPECT_THAT(res.variables["cloud_albedo"].double_values,
              ElementsAre(0.5, 0.6, 0.7, 0.8));
  EXPECT_THAT(res.variables["cloud_albedo_uncertainty"].double_values,
              ElementsAre(0.05, 0.06, 0.07, 0.08));
  EXPECT_THAT(
      res.variables["cloud_top_height"].double_values,
      ElementsAre(DoubleEq(5000.0 * kMToKm), DoubleEq(6000.0 * kMToKm),
                  DoubleEq(7000.0 * kMToKm), DoubleEq(8000.0 * kMToKm)));
  EXPECT_THAT(res.variables["cloud_top_height_uncertainty"].double_values,
              ElementsAre(DoubleEq(100.0 * kMToKm), DoubleEq(200.0 * kMToKm),
                          DoubleEq(300.0 * kMToKm), DoubleEq(400.0 * kMToKm)));
}

TEST(HarpIngestCciL3O3TtocTest, InitDimensionErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod =
      FindIngestionModule("ESACCI_OZONE_L3_TTOC");
  ASSERT_NE(nullptr, mod);

  constexpr long kDims1D[] = {1};
  constexpr double kVals1D[] = {1.0};
  constexpr long kDims2D[] = {1, 2};
  constexpr double kVals2D[] = {1.0, 2.0};

  // 1. Missing /latitude.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 2. Scalar /latitude.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude", prod.MakeDouble(1.0)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 3. 2D /latitude.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                              prod.MakeDoubleArray(kDims2D, kVals2D)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 4. Missing /longitude.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                              prod.MakeDoubleArray(kDims1D, kVals1D)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 5. Scalar /longitude.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                              prod.MakeDoubleArray(kDims1D, kVals1D)));
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude", prod.MakeDouble(1.0)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 6. 2D /longitude.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                              prod.MakeDoubleArray(kDims1D, kVals1D)));
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                              prod.MakeDoubleArray(kDims2D, kVals2D)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }
}

TEST(HarpIngestCciL3O3TtocTest, ReadDatasetErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod =
      FindIngestionModule("ESACCI_OZONE_L3_TTOC");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  constexpr long kDims1D[] = {1};
  constexpr double kVals1D[] = {1.0};
  ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                            prod.MakeDoubleArray(kDims1D, kVals1D)));
  ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                            prod.MakeDoubleArray(kDims1D, kVals1D)));

  CodaMemNode* product_rec = prod.MakeRecord();
  constexpr long kWrongDims[] = {2};
  constexpr double kWrongVals[] = {1.0, 2.0};
  ASSERT_TRUE(prod.AddField(product_rec, "tropospheric_ozone_column",
                            prod.MakeDoubleArray(kWrongDims, kWrongVals)));
  constexpr long kGridDims[] = {1, 1};
  constexpr std::string_view kBadStr[] = {"bad"};
  ASSERT_TRUE(prod.AddField(product_rec, "tropospheric_ozone_mixing_ratio",
                            prod.MakeStringArray(kGridDims, kBadStr)));
  ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT", product_rec));

  IngestExerciseResult res = ExerciseIngestionModule(
      mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
  EXPECT_EQ(-1, res.status);
  EXPECT_EQ(-1, res.variables["O3_column_number_density"].read_status);
  EXPECT_EQ(-1,
            res.variables["tropospheric_O3_column_number_density"].read_status);
  EXPECT_EQ(-1,
            res.variables["tropospheric_O3_volume_mixing_ratio"].read_status);
  harp_set_error(HARP_SUCCESS, nullptr);
}

}  // namespace
}  // namespace stcorp_harp
