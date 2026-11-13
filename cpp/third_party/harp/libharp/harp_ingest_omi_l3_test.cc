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
// Unit tests for OMI L3 ingestion modules (libharp/harp-ingest-omi_l3.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-omi_l3.c

#include <cmath>
#include <cstdint>
#include <span>
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

using ::testing::DoubleNear;
using ::testing::ElementsAre;

constexpr int32_t kNumLat = 2;
constexpr int32_t kNumLon = 4;
constexpr long kGridSize = kNumLat * kNumLon;
constexpr double kFillVal = -9999.0;

CodaMemNode* MakeGridDataset(CodaMemProduct& prod, std::span<const double> vals,
                             bool include_scale_and_offset) {
  constexpr long kGridDims[] = {kNumLat, kNumLon};
  constexpr long kAttrDims[] = {1};
  constexpr double kFill[] = {kFillVal};
  CodaMemNode* ds = prod.MakeDoubleArray(kGridDims, vals);
  if (!prod.AddAttribute(ds, "MissingValue",
                         prod.MakeDoubleArray(kAttrDims, kFill))) {
    return nullptr;
  }
  if (include_scale_and_offset) {
    constexpr double kScale[] = {2.0};
    constexpr double kOffset[] = {10.0};
    if (!prod.AddAttribute(ds, "ScaleFactor",
                           prod.MakeDoubleArray(kAttrDims, kScale)) ||
        !prod.AddAttribute(ds, "Offset",
                           prod.MakeDoubleArray(kAttrDims, kOffset))) {
      return nullptr;
    }
  }
  return ds;
}

// Builds `/HDFEOS/ADDITIONAL/FILE_ATTRIBUTES@TAI93At0zOfGranule` and
// `/HDFEOS/GRIDS/<group_name>/Data_Fields` with global grid attributes.
CodaMemNode* BuildHdfeosGrid(CodaMemProduct& prod,
                             std::string_view group_name) {
  CodaMemNode* hdfeos = prod.MakeRecord();
  CodaMemNode* additional = prod.MakeRecord();
  CodaMemNode* file_attrs = prod.MakeRecord();
  constexpr long kAttrDims[] = {1};
  // 220838405.0 is 2000-01-01T00:00:00Z in TAI93 (SECONDS_FROM_1993_TO_2000).
  constexpr double kTai93[] = {220838405.0 + 100.0};
  EXPECT_TRUE(prod.AddAttribute(file_attrs, "TAI93At0zOfGranule",
                                prod.MakeDoubleArray(kAttrDims, kTai93)));
  EXPECT_TRUE(prod.AddField(additional, "FILE_ATTRIBUTES", file_attrs));
  EXPECT_TRUE(prod.AddField(hdfeos, "ADDITIONAL", additional));

  CodaMemNode* grids = prod.MakeRecord();
  CodaMemNode* group = prod.MakeRecord();
  EXPECT_TRUE(
      prod.AddAttribute(group, "GridSpacing", prod.MakeString("(90,90)")));
  constexpr int32_t kLons[] = {kNumLon};
  constexpr int32_t kLats[] = {kNumLat};
  EXPECT_TRUE(prod.AddAttribute(group, "NumberOfLongitudesInGrid",
                                prod.MakeInt32Array(kAttrDims, kLons)));
  EXPECT_TRUE(prod.AddAttribute(group, "NumberOfLatitudesInGrid",
                                prod.MakeInt32Array(kAttrDims, kLats)));

  CodaMemNode* data_fields = prod.MakeRecord();
  EXPECT_TRUE(prod.AddField(group, "Data_Fields", data_fields));
  EXPECT_TRUE(prod.AddField(grids, group_name, group));
  EXPECT_TRUE(prod.AddField(hdfeos, "GRIDS", grids));
  EXPECT_TRUE(prod.AddField(prod.root(), "HDFEOS", hdfeos));
  return data_fields;
}

TEST(HarpIngestOmiL3Test, IngestOmdoao3eSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("OMI_L3_OMDOAO3e");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  CodaMemNode* df = BuildHdfeosGrid(prod, "ColumnAmountO3");

  constexpr double kVals0[] = {1.0, kFillVal, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
  constexpr double kVals1[] = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
  // Include ScaleFactor & Offset on CloudFraction, omit them on others.
  ASSERT_TRUE(
      prod.AddField(df, "CloudFraction", MakeGridDataset(prod, kVals0, true)));
  ASSERT_TRUE(prod.AddField(df, "CloudFractionPrecision",
                            MakeGridDataset(prod, kVals1, false)));
  ASSERT_TRUE(
      prod.AddField(df, "CloudPressure", MakeGridDataset(prod, kVals1, false)));
  ASSERT_TRUE(prod.AddField(df, "CloudPressurePrecision",
                            MakeGridDataset(prod, kVals1, false)));
  ASSERT_TRUE(prod.AddField(df, "ColumnAmountO3",
                            MakeGridDataset(prod, kVals1, false)));
  ASSERT_TRUE(prod.AddField(df, "ColumnAmountO3Precision",
                            MakeGridDataset(prod, kVals1, false)));

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(1, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(kNumLat, res.dimensions[harp_dimension_latitude]);
  EXPECT_EQ(kNumLon, res.dimensions[harp_dimension_longitude]);

  EXPECT_THAT(res.variables["datetime_start"].double_values,
              ElementsAre(DoubleNear(100.0, 1e-6)));
  EXPECT_THAT(res.variables["datetime_length"].double_values, ElementsAre(1.0));
  EXPECT_THAT(res.variables["latitude"].double_values,
              ElementsAre(-45.0, 45.0));
  EXPECT_THAT(res.variables["longitude"].double_values,
              ElementsAre(-135.0, -45.0, 45.0, 135.0));

  const auto& cf = res.variables["cloud_fraction"].double_values;
  ASSERT_EQ(static_cast<size_t>(kGridSize), cf.size());
  // offset=10.0, scale=2.0 -> 10 + 2*1.0 = 12.0; kFillVal -> NaN.
  EXPECT_DOUBLE_EQ(12.0, cf[0]);
  EXPECT_TRUE(std::isnan(cf[1]));
  EXPECT_DOUBLE_EQ(26.0, cf[7]);
  EXPECT_DOUBLE_EQ(0.1,
                   res.variables["O3_column_number_density"].double_values[0]);
}

TEST(HarpIngestOmiL3Test, IngestOmhchodOmno2dOmso2eOmto3dOmto3e) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  // 1. OMI_L3_OMHCHOd
  {
    const harp_ingestion_module* mod = FindIngestionModule("OMI_L3_OMHCHOd");
    ASSERT_NE(nullptr, mod);

    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "RangeBeginningDate",
                                  prod.MakeString("2000-01-01")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "RangeEndingDate",
                                  prod.MakeString("2000-01-02")));

    constexpr long kLatDims[] = {kNumLat};
    constexpr double kLat[] = {-45.0, 45.0};
    constexpr long kLonDims[] = {kNumLon};
    constexpr double kLon[] = {-135.0, -45.0, 45.0, 135.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                              prod.MakeDoubleArray(kLatDims, kLat)));
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                              prod.MakeDoubleArray(kLonDims, kLon)));

    constexpr long kGridDims[] = {kNumLat, kNumLon};
    constexpr double kHcho[] = {1e15, 2e15, 3e15, 4e15, 5e15, 6e15, 7e15, 8e15};
    constexpr double kUnc[] = {1e14, 2e14, 3e14, 4e14, 5e14, 6e14, 7e14, 8e14};
    CodaMemNode* ksd = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(ksd, "column_amount",
                              prod.MakeDoubleArray(kGridDims, kHcho)));
    ASSERT_TRUE(prod.AddField(ksd, "column_uncertainty",
                              prod.MakeDoubleArray(kGridDims, kUnc)));
    ASSERT_TRUE(prod.AddField(prod.root(), "key_science_data", ksd));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["datetime_start"].double_values,
                ElementsAre(0.0));
    EXPECT_THAT(res.variables["datetime_stop"].double_values,
                ElementsAre(86400.0));
    EXPECT_THAT(res.variables["latitude"].double_values,
                ElementsAre(-45.0, 45.0));
    EXPECT_THAT(res.variables["longitude"].double_values,
                ElementsAre(-135.0, -45.0, 45.0, 135.0));
    EXPECT_DOUBLE_EQ(
        1e15, res.variables["HCHO_column_number_density"].double_values[0]);
    EXPECT_DOUBLE_EQ(1e14,
                     res.variables["HCHO_column_number_density_uncertainty"]
                         .double_values[0]);
  }

  // 2. OMI_L3_OMNO2d (both default and no2=cloud_screened)
  {
    const harp_ingestion_module* mod = FindIngestionModule("OMI_L3_OMNO2d");
    ASSERT_NE(nullptr, mod);

    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* df = BuildHdfeosGrid(prod, "ColumnAmountNO2");
    constexpr double kNo2[] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
    constexpr double kNo2Trop[] = {0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5};
    constexpr double kNo2Cs[] = {10.0, 20.0, 30.0, 40.0,
                                 50.0, 60.0, 70.0, 80.0};
    constexpr double kNo2TropCs[] = {5.0,  15.0, 25.0, 35.0,
                                     45.0, 55.0, 65.0, 75.0};
    ASSERT_TRUE(prod.AddField(df, "ColumnAmountNO2",
                              MakeGridDataset(prod, kNo2, false)));
    ASSERT_TRUE(prod.AddField(df, "ColumnAmountNO2Trop",
                              MakeGridDataset(prod, kNo2Trop, false)));
    ASSERT_TRUE(prod.AddField(df, "ColumnAmountNO2CloudScreened",
                              MakeGridDataset(prod, kNo2Cs, false)));
    ASSERT_TRUE(prod.AddField(df, "ColumnAmountNO2TropCloudScreened",
                              MakeGridDataset(prod, kNo2TropCs, false)));

    IngestExerciseResult res_def = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res_def.status);
    EXPECT_DOUBLE_EQ(
        1.0, res_def.variables["NO2_column_number_density"].double_values[0]);
    EXPECT_DOUBLE_EQ(0.5,
                     res_def.variables["tropospheric_NO2_column_number_density"]
                         .double_values[0]);

    // Upstream registers option "no2" while ingestion_init_omno2 checks
    // "qa_filter", so "no2=cloud_screened" validates cleanly and still reads
    // the unfiltered datasets.
    IngestExerciseResult res_cs =
        ExerciseIngestionModule(mod, prod.product(), "no2=cloud_screened");
    ASSERT_EQ(HARP_SUCCESS, res_cs.status);
    EXPECT_DOUBLE_EQ(
        1.0, res_cs.variables["NO2_column_number_density"].double_values[0]);
    EXPECT_DOUBLE_EQ(0.5,
                     res_cs.variables["tropospheric_NO2_column_number_density"]
                         .double_values[0]);
  }

  // 3. OMI_L3_OMSO2e
  {
    const harp_ingestion_module* mod = FindIngestionModule("OMI_L3_OMSO2e");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* df = BuildHdfeosGrid(prod, "OMI_Total_Column_Amount_SO2");
    constexpr double kVals[] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
    ASSERT_TRUE(prod.AddField(df, "CloudRadianceFraction",
                              MakeGridDataset(prod, kVals, false)));
    ASSERT_TRUE(prod.AddField(df, "ColumnAmountSO2",
                              MakeGridDataset(prod, kVals, false)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_DOUBLE_EQ(
        1.0, res.variables["radiative_cloud_fraction"].double_values[0]);
    EXPECT_DOUBLE_EQ(
        1.0, res.variables["SO2_column_number_density"].double_values[0]);
  }

  // 4. OMI_L3_OMTO3d and OMI_L3_OMTO3e
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* df = BuildHdfeosGrid(prod, "OMI_Column_Amount_O3");
    constexpr double kVals[] = {300.0, 310.0, 320.0, 330.0,
                                340.0, 350.0, 360.0, 370.0};
    ASSERT_TRUE(prod.AddField(df, "ColumnAmountO3",
                              MakeGridDataset(prod, kVals, false)));
    ASSERT_TRUE(prod.AddField(df, "RadiativeCloudFraction",
                              MakeGridDataset(prod, kVals, false)));
    ASSERT_TRUE(prod.AddField(df, "UVAerosolIndex",
                              MakeGridDataset(prod, kVals, false)));

    const harp_ingestion_module* mod3d = FindIngestionModule("OMI_L3_OMTO3d");
    ASSERT_NE(nullptr, mod3d);
    IngestExerciseResult res3d = ExerciseIngestionModule(mod3d, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res3d.status);
    EXPECT_DOUBLE_EQ(300.0,
                     res3d.variables["uv_aerosol_index"].double_values[0]);

    const harp_ingestion_module* mod3e = FindIngestionModule("OMI_L3_OMTO3e");
    ASSERT_NE(nullptr, mod3e);
    IngestExerciseResult res3e = ExerciseIngestionModule(mod3e, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res3e.status);
    EXPECT_DOUBLE_EQ(
        300.0, res3e.variables["O3_column_number_density"].double_values[0]);
  }
}

TEST(HarpIngestOmiL3Test, GridInitErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("OMI_L3_OMDOAO3e");
  ASSERT_NE(nullptr, mod);

  // 1. Missing /HDFEOS/ADDITIONAL/FILE_ATTRIBUTES@TAI93At0zOfGranule.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. Wrong array length (!= 1) for TAI93At0zOfGranule.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* hdfeos = prod.MakeRecord();
    CodaMemNode* additional = prod.MakeRecord();
    CodaMemNode* file_attrs = prod.MakeRecord();
    constexpr long kDims[] = {2};
    constexpr double kVals[] = {1.0, 2.0};
    ASSERT_TRUE(prod.AddAttribute(file_attrs, "TAI93At0zOfGranule",
                                  prod.MakeDoubleArray(kDims, kVals)));
    ASSERT_TRUE(prod.AddField(additional, "FILE_ATTRIBUTES", file_attrs));
    ASSERT_TRUE(prod.AddField(hdfeos, "ADDITIONAL", additional));
    ASSERT_TRUE(prod.AddField(prod.root(), "HDFEOS", hdfeos));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 3. TAI93At0zOfGranule wrong element type (string array).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* hdfeos = prod.MakeRecord();
    CodaMemNode* additional = prod.MakeRecord();
    CodaMemNode* file_attrs = prod.MakeRecord();
    constexpr long kDims[] = {1};
    constexpr std::string_view kBad[] = {"bad"};
    ASSERT_TRUE(prod.AddAttribute(file_attrs, "TAI93At0zOfGranule",
                                  prod.MakeStringArray(kDims, kBad)));
    ASSERT_TRUE(prod.AddField(additional, "FILE_ATTRIBUTES", file_attrs));
    ASSERT_TRUE(prod.AddField(hdfeos, "ADDITIONAL", additional));
    ASSERT_TRUE(prod.AddField(prod.root(), "HDFEOS", hdfeos));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 4. Missing /HDFEOS/GRIDS, missing data_group_name, missing Data_Fields,
  // missing/bad GridSpacing, missing/bad NumberOfLongitudesInGrid, missing/bad
  // NumberOfLatitudesInGrid, and non-global grid coverage.
  auto make_tai_root = [](CodaMemProduct& prod) -> CodaMemNode* {
    CodaMemNode* hdfeos = prod.MakeRecord();
    CodaMemNode* additional = prod.MakeRecord();
    CodaMemNode* file_attrs = prod.MakeRecord();
    constexpr long kDims[] = {1};
    constexpr double kTai[] = {220838405.0};
    EXPECT_TRUE(prod.AddAttribute(file_attrs, "TAI93At0zOfGranule",
                                  prod.MakeDoubleArray(kDims, kTai)));
    EXPECT_TRUE(prod.AddField(additional, "FILE_ATTRIBUTES", file_attrs));
    EXPECT_TRUE(prod.AddField(hdfeos, "ADDITIONAL", additional));
    EXPECT_TRUE(prod.AddField(prod.root(), "HDFEOS", hdfeos));
    return hdfeos;
  };

  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_tai_root(prod);
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* hdfeos = make_tai_root(prod);
    ASSERT_TRUE(prod.AddField(hdfeos, "GRIDS", prod.MakeRecord()));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* hdfeos = make_tai_root(prod);
    CodaMemNode* grids = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(grids, "ColumnAmountO3", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(hdfeos, "GRIDS", grids));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* hdfeos = make_tai_root(prod);
    CodaMemNode* grids = prod.MakeRecord();
    CodaMemNode* group = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(group, "Data_Fields", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(grids, "ColumnAmountO3", group));
    ASSERT_TRUE(prod.AddField(hdfeos, "GRIDS", grids));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* hdfeos = make_tai_root(prod);
    CodaMemNode* grids = prod.MakeRecord();
    CodaMemNode* group = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(group, "Data_Fields", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddAttribute(group, "GridSpacing",
                                  prod.MakeString("invalid-spacing")));
    ASSERT_TRUE(prod.AddField(grids, "ColumnAmountO3", group));
    ASSERT_TRUE(prod.AddField(hdfeos, "GRIDS", grids));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* hdfeos = make_tai_root(prod);
    CodaMemNode* grids = prod.MakeRecord();
    CodaMemNode* group = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(group, "Data_Fields", prod.MakeRecord()));
    ASSERT_TRUE(
        prod.AddAttribute(group, "GridSpacing", prod.MakeString("(90,90)")));
    constexpr long kTwo[] = {2};
    constexpr int32_t kBadLons[] = {4, 4};
    ASSERT_TRUE(prod.AddAttribute(group, "NumberOfLongitudesInGrid",
                                  prod.MakeInt32Array(kTwo, kBadLons)));
    ASSERT_TRUE(prod.AddField(grids, "ColumnAmountO3", group));
    ASSERT_TRUE(prod.AddField(hdfeos, "GRIDS", grids));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    // GridSpacing >= 64 chars (exceeds str_buffer).
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* hdfeos = make_tai_root(prod);
    CodaMemNode* grids = prod.MakeRecord();
    CodaMemNode* group = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(group, "Data_Fields", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddAttribute(
        group, "GridSpacing",
        prod.MakeString("01234567890123456789012345678901234567890123456789"
                        "01234567890123456")));
    ASSERT_TRUE(prod.AddField(grids, "ColumnAmountO3", group));
    ASSERT_TRUE(prod.AddField(hdfeos, "GRIDS", grids));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    // Missing NumberOfLongitudesInGrid.
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* hdfeos = make_tai_root(prod);
    CodaMemNode* grids = prod.MakeRecord();
    CodaMemNode* group = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(group, "Data_Fields", prod.MakeRecord()));
    ASSERT_TRUE(
        prod.AddAttribute(group, "GridSpacing", prod.MakeString("(90,90)")));
    ASSERT_TRUE(prod.AddField(grids, "ColumnAmountO3", group));
    ASSERT_TRUE(prod.AddField(hdfeos, "GRIDS", grids));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    // NumberOfLongitudesInGrid wrong type (string array).
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* hdfeos = make_tai_root(prod);
    CodaMemNode* grids = prod.MakeRecord();
    CodaMemNode* group = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(group, "Data_Fields", prod.MakeRecord()));
    ASSERT_TRUE(
        prod.AddAttribute(group, "GridSpacing", prod.MakeString("(90,90)")));
    constexpr long kOne[] = {1};
    constexpr std::string_view kBadStr[] = {"bad"};
    ASSERT_TRUE(prod.AddAttribute(group, "NumberOfLongitudesInGrid",
                                  prod.MakeStringArray(kOne, kBadStr)));
    ASSERT_TRUE(prod.AddField(grids, "ColumnAmountO3", group));
    ASSERT_TRUE(prod.AddField(hdfeos, "GRIDS", grids));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    // Missing NumberOfLatitudesInGrid.
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* hdfeos = make_tai_root(prod);
    CodaMemNode* grids = prod.MakeRecord();
    CodaMemNode* group = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(group, "Data_Fields", prod.MakeRecord()));
    ASSERT_TRUE(
        prod.AddAttribute(group, "GridSpacing", prod.MakeString("(90,90)")));
    constexpr long kOne[] = {1};
    constexpr int32_t kDim[] = {4};
    ASSERT_TRUE(prod.AddAttribute(group, "NumberOfLongitudesInGrid",
                                  prod.MakeInt32Array(kOne, kDim)));
    ASSERT_TRUE(prod.AddField(grids, "ColumnAmountO3", group));
    ASSERT_TRUE(prod.AddField(hdfeos, "GRIDS", grids));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    // NumberOfLatitudesInGrid wrong length (!= 1).
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* hdfeos = make_tai_root(prod);
    CodaMemNode* grids = prod.MakeRecord();
    CodaMemNode* group = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(group, "Data_Fields", prod.MakeRecord()));
    ASSERT_TRUE(
        prod.AddAttribute(group, "GridSpacing", prod.MakeString("(90,90)")));
    constexpr long kOne[] = {1};
    constexpr long kTwo[] = {2};
    constexpr int32_t kLon[] = {4};
    constexpr int32_t kBadLat[] = {2, 2};
    ASSERT_TRUE(prod.AddAttribute(group, "NumberOfLongitudesInGrid",
                                  prod.MakeInt32Array(kOne, kLon)));
    ASSERT_TRUE(prod.AddAttribute(group, "NumberOfLatitudesInGrid",
                                  prod.MakeInt32Array(kTwo, kBadLat)));
    ASSERT_TRUE(prod.AddField(grids, "ColumnAmountO3", group));
    ASSERT_TRUE(prod.AddField(hdfeos, "GRIDS", grids));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    // NumberOfLatitudesInGrid wrong type (string array).
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* hdfeos = make_tai_root(prod);
    CodaMemNode* grids = prod.MakeRecord();
    CodaMemNode* group = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(group, "Data_Fields", prod.MakeRecord()));
    ASSERT_TRUE(
        prod.AddAttribute(group, "GridSpacing", prod.MakeString("(90,90)")));
    constexpr long kOne[] = {1};
    constexpr int32_t kLon[] = {4};
    constexpr std::string_view kBadStr[] = {"bad"};
    ASSERT_TRUE(prod.AddAttribute(group, "NumberOfLongitudesInGrid",
                                  prod.MakeInt32Array(kOne, kLon)));
    ASSERT_TRUE(prod.AddAttribute(group, "NumberOfLatitudesInGrid",
                                  prod.MakeStringArray(kOne, kBadStr)));
    ASSERT_TRUE(prod.AddField(grids, "ColumnAmountO3", group));
    ASSERT_TRUE(prod.AddField(hdfeos, "GRIDS", grids));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    // Non-global coverage (2*10 != 180).
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* hdfeos = make_tai_root(prod);
    CodaMemNode* grids = prod.MakeRecord();
    CodaMemNode* group = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(group, "Data_Fields", prod.MakeRecord()));
    ASSERT_TRUE(
        prod.AddAttribute(group, "GridSpacing", prod.MakeString("(10,10)")));
    constexpr long kOne[] = {1};
    constexpr int32_t kDim[] = {2};
    ASSERT_TRUE(prod.AddAttribute(group, "NumberOfLongitudesInGrid",
                                  prod.MakeInt32Array(kOne, kDim)));
    ASSERT_TRUE(prod.AddAttribute(group, "NumberOfLatitudesInGrid",
                                  prod.MakeInt32Array(kOne, kDim)));
    ASSERT_TRUE(prod.AddField(grids, "ColumnAmountO3", group));
    ASSERT_TRUE(prod.AddField(hdfeos, "GRIDS", grids));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  // Init failures on OMI_L3_OMNO2d, OMI_L3_OMSO2e, and OMI_L3_OMTO3d.
  for (const char* mname :
       {"OMI_L3_OMNO2d", "OMI_L3_OMSO2e", "OMI_L3_OMTO3d"}) {
    const harp_ingestion_module* m = FindIngestionModule(mname);
    ASSERT_NE(nullptr, m);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    EXPECT_EQ(-1, ExerciseIngestionModule(m, prod.product()).status);
  }
}

TEST(HarpIngestOmiL3Test, DatasetAndHchoReadErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  // 1. OMI_L3_OMDOAO3e dataset & attribute read errors.
  {
    const harp_ingestion_module* mod = FindIngestionModule("OMI_L3_OMDOAO3e");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* df = BuildHdfeosGrid(prod, "ColumnAmountO3");

    constexpr long kBadDims[] = {1};
    constexpr double kBadVal[] = {1.0};
    constexpr long kGridDims[] = {kNumLat, kNumLon};
    constexpr double kVals[] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
    constexpr std::string_view kBadStr[] = {"x"};

    // CloudFraction: missing field in Data_Fields (triggers goto_record_field).
    // CloudFractionPrecision: wrong grid size.
    ASSERT_TRUE(prod.AddField(df, "CloudFractionPrecision",
                              prod.MakeDoubleArray(kBadDims, kBadVal)));

    // CloudPressure: MissingValue is scalar (not array).
    CodaMemNode* cp = prod.MakeDoubleArray(kGridDims, kVals);
    ASSERT_TRUE(prod.AddAttribute(cp, "MissingValue", prod.MakeDouble(-999.0)));
    ASSERT_TRUE(prod.AddField(df, "CloudPressure", cp));

    // CloudPressurePrecision: MissingValue is string array -> read_double
    // fails.
    CodaMemNode* cpp = prod.MakeDoubleArray(kGridDims, kVals);
    ASSERT_TRUE(prod.AddAttribute(cpp, "MissingValue",
                                  prod.MakeStringArray(kBadDims, kBadStr)));
    ASSERT_TRUE(prod.AddField(df, "CloudPressurePrecision", cpp));

    // ColumnAmountO3: ScaleFactor is scalar -> goto_first_array_element fails.
    CodaMemNode* o3 = prod.MakeDoubleArray(kGridDims, kVals);
    ASSERT_TRUE(prod.AddAttribute(o3, "MissingValue",
                                  prod.MakeDoubleArray(kBadDims, kBadVal)));
    ASSERT_TRUE(prod.AddAttribute(o3, "ScaleFactor", prod.MakeDouble(1.0)));
    ASSERT_TRUE(prod.AddField(df, "ColumnAmountO3", o3));

    // ColumnAmountO3Precision: Offset is scalar -> goto_first_array_element
    // fails.
    CodaMemNode* o3p = prod.MakeDoubleArray(kGridDims, kVals);
    ASSERT_TRUE(prod.AddAttribute(o3p, "MissingValue",
                                  prod.MakeDoubleArray(kBadDims, kBadVal)));
    ASSERT_TRUE(prod.AddAttribute(o3p, "Offset", prod.MakeDouble(0.0)));
    ASSERT_TRUE(prod.AddField(df, "ColumnAmountO3Precision", o3p));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["cloud_fraction"].read_status);
    EXPECT_EQ(-1, res.variables["cloud_fraction_uncertainty"].read_status);
    EXPECT_EQ(-1, res.variables["cloud_pressure"].read_status);
    EXPECT_EQ(-1, res.variables["cloud_pressure_uncertainty"].read_status);
    EXPECT_EQ(-1, res.variables["O3_column_number_density"].read_status);
    EXPECT_EQ(
        -1, res.variables["O3_column_number_density_uncertainty"].read_status);
  }

  // 1b. Missing MissingValue attribute, wrong data array type, and
  // non-array ScaleFactor/Offset error paths.
  {
    const harp_ingestion_module* mod = FindIngestionModule("OMI_L3_OMDOAO3e");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* df = BuildHdfeosGrid(prod, "ColumnAmountO3");
    constexpr long kBadDims[] = {1};
    constexpr double kBadVal[] = {1.0};
    constexpr long kGridDims[] = {kNumLat, kNumLon};
    constexpr double kVals[] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
    constexpr std::string_view kBadStr[] = {"x"};
    constexpr std::string_view kBadGridStr[] = {"a", "b", "c", "d",
                                                "e", "f", "g", "h"};

    // Missing MissingValue attribute.
    ASSERT_TRUE(prod.AddField(df, "CloudFraction",
                              prod.MakeDoubleArray(kGridDims, kVals)));
    // Non-double data array with valid MissingValue.
    CodaMemNode* cfp = prod.MakeStringArray(kGridDims, kBadGridStr);
    ASSERT_TRUE(prod.AddAttribute(cfp, "MissingValue",
                                  prod.MakeDoubleArray(kBadDims, kBadVal)));
    ASSERT_TRUE(prod.AddField(df, "CloudFractionPrecision", cfp));
    // ScaleFactor is string array.
    CodaMemNode* o3 = prod.MakeDoubleArray(kGridDims, kVals);
    ASSERT_TRUE(prod.AddAttribute(o3, "MissingValue",
                                  prod.MakeDoubleArray(kBadDims, kBadVal)));
    ASSERT_TRUE(prod.AddAttribute(o3, "ScaleFactor",
                                  prod.MakeStringArray(kBadDims, kBadStr)));
    ASSERT_TRUE(prod.AddField(df, "ColumnAmountO3", o3));
    // Offset is string array.
    CodaMemNode* o3p = prod.MakeDoubleArray(kGridDims, kVals);
    ASSERT_TRUE(prod.AddAttribute(o3p, "MissingValue",
                                  prod.MakeDoubleArray(kBadDims, kBadVal)));
    ASSERT_TRUE(prod.AddAttribute(o3p, "Offset",
                                  prod.MakeStringArray(kBadDims, kBadStr)));
    ASSERT_TRUE(prod.AddField(df, "ColumnAmountO3Precision", o3p));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["cloud_fraction"].read_status);
    EXPECT_EQ(-1, res.variables["cloud_fraction_uncertainty"].read_status);
    EXPECT_EQ(-1, res.variables["O3_column_number_density"].read_status);
    EXPECT_EQ(
        -1, res.variables["O3_column_number_density_uncertainty"].read_status);
  }

  // 1c. Missing datasets on OMI_L3_OMSO2e and OMI_L3_OMTO3d.
  {
    const harp_ingestion_module* mod_so2 = FindIngestionModule("OMI_L3_OMSO2e");
    ASSERT_NE(nullptr, mod_so2);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    BuildHdfeosGrid(prod, "OMI_Total_Column_Amount_SO2");
    IngestExerciseResult res = ExerciseIngestionModule(mod_so2, prod.product(),
                                                       /*options_str=*/nullptr,
                                                       /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["radiative_cloud_fraction"].read_status);
    EXPECT_EQ(-1, res.variables["SO2_column_number_density"].read_status);
  }
  {
    const harp_ingestion_module* mod_o3d = FindIngestionModule("OMI_L3_OMTO3d");
    ASSERT_NE(nullptr, mod_o3d);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    BuildHdfeosGrid(prod, "OMI_Column_Amount_O3");
    IngestExerciseResult res = ExerciseIngestionModule(mod_o3d, prod.product(),
                                                       /*options_str=*/nullptr,
                                                       /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["cloud_fraction"].read_status);
    EXPECT_EQ(-1, res.variables["uv_aerosol_index"].read_status);
  }

  // 2. OMI_L3_OMHCHOd variable read errors (missing attributes, wrong types,
  // bad dates, missing/bad column_amount).
  {
    const harp_ingestion_module* mod = FindIngestionModule("OMI_L3_OMHCHOd");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "RangeBeginningDate",
                                  prod.MakeInt32(1)));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "RangeEndingDate", prod.MakeInt32(2)));
    constexpr long kLatDims[] = {kNumLat};
    constexpr double kLat[] = {-45.0, 45.0};
    constexpr long kLonDims[] = {kNumLon};
    constexpr double kLon[] = {-135.0, -45.0, 45.0, 135.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                              prod.MakeDoubleArray(kLatDims, kLat)));
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                              prod.MakeDoubleArray(kLonDims, kLon)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_stop"].read_status);
    EXPECT_EQ(-1, res.variables["HCHO_column_number_density"].read_status);
  }
  {
    const harp_ingestion_module* mod = FindIngestionModule("OMI_L3_OMHCHOd");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "RangeBeginningDate",
                                  prod.MakeString("bad-date")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "RangeEndingDate",
                                  prod.MakeString("bad-date")));
    constexpr long kLatDims[] = {kNumLat};
    constexpr double kLat[] = {-45.0, 45.0};
    constexpr long kLonDims[] = {kNumLon};
    constexpr double kLon[] = {-135.0, -45.0, 45.0, 135.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                              prod.MakeDoubleArray(kLatDims, kLat)));
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                              prod.MakeDoubleArray(kLonDims, kLon)));

    CodaMemNode* ksd = prod.MakeRecord();
    constexpr long kBadDims[] = {1};
    constexpr double kBadVal[] = {1.0};
    ASSERT_TRUE(prod.AddField(ksd, "column_amount",
                              prod.MakeDoubleArray(kBadDims, kBadVal)));
    constexpr long kGridDims[] = {kNumLat, kNumLon};
    constexpr std::string_view kBadStr[] = {"a", "b", "c", "d",
                                            "e", "f", "g", "h"};
    ASSERT_TRUE(prod.AddField(ksd, "column_uncertainty",
                              prod.MakeStringArray(kGridDims, kBadStr)));
    ASSERT_TRUE(prod.AddField(prod.root(), "key_science_data", ksd));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_stop"].read_status);
    EXPECT_EQ(-1, res.variables["HCHO_column_number_density"].read_status);
    EXPECT_EQ(
        -1,
        res.variables["HCHO_column_number_density_uncertainty"].read_status);
  }
  {
    // Missing RangeBeginningDate and RangeEndingDate.
    const harp_ingestion_module* mod = FindIngestionModule("OMI_L3_OMHCHOd");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kLatDims[] = {kNumLat};
    constexpr double kLat[] = {-45.0, 45.0};
    constexpr long kLonDims[] = {kNumLon};
    constexpr double kLon[] = {-135.0, -45.0, 45.0, 135.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude",
                              prod.MakeDoubleArray(kLatDims, kLat)));
    ASSERT_TRUE(prod.AddField(prod.root(), "longitude",
                              prod.MakeDoubleArray(kLonDims, kLon)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_stop"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
