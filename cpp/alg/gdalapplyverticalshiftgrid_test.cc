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
// Tests GDALApplyVerticalShiftGrid and GDALOpenVerticalShiftGrid
// (alg/gdalapplyverticalshiftgrid.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/gdalapplyverticalshiftgrid.cpp

#define CPL_WARN_DEPRECATED_GDALApplyVerticalShiftGrid(x)
#define CPL_WARN_DEPRECATED_GDALOpenVerticalShiftGrid(x)

#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr std::array<double, 6> kDefaultGT = {10.0, 1.0, 0.0, 40.0, 0.0, -1.0};

GDALDatasetUniquePtr CreateDataset(const std::string& path, int width,
                                   int height, GDALDataType data_type,
                                   float fill_value,
                                   const std::array<double, 6>& gt,
                                   const char* srs_def = "EPSG:4326",
                                   double nodata = -99999.0,
                                   bool has_nodata = false) {
  GDALDriver* driver = path.empty()
                           ? GetGDALDriverManager()->GetDriverByName("MEM")
                           : GetGDALDriverManager()->GetDriverByName("GTiff");
  if (!driver) return nullptr;

  GDALDatasetUniquePtr ds(
      driver->Create(path.c_str(), width, height, 1, data_type, nullptr));
  if (!ds) return nullptr;

  ds->SetGeoTransform(const_cast<double*>(gt.data()));

  if (srs_def && srs_def[0] != '\0') {
    OGRSpatialReference srs;
    srs.SetFromUserInput(srs_def);
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    ds->SetSpatialRef(&srs);
  }

  GDALRasterBand* band = ds->GetRasterBand(1);
  if (band) {
    if (has_nodata) {
      band->SetNoDataValue(nodata);
    }
    std::vector<float> data(width * height, fill_value);
    CPLErr err = band->RasterIO(GF_Write, 0, 0, width, height, data.data(),
                                width, height, GDT_Float32, 0, 0, nullptr);
    if (err != CE_None) return nullptr;
  }
  return ds;
}

class GdalApplyVerticalShiftGridTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    GDALRegister_VRT();
  }
};

TEST_F(GdalApplyVerticalShiftGridTest, NullPointers) {
  GDALDatasetUniquePtr ds =
      CreateDataset("", 5, 5, GDT_Float32, 10.0f, kDefaultGT);
  ASSERT_NE(nullptr, ds);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(nullptr, GDALApplyVerticalShiftGrid(nullptr, ds.get(), FALSE, 1.0,
                                                1.0, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  EXPECT_EQ(nullptr, GDALApplyVerticalShiftGrid(ds.get(), nullptr, FALSE, 1.0,
                                                1.0, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST_F(GdalApplyVerticalShiftGridTest, SourceDatasetValidation) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);

  GDALDatasetUniquePtr grid =
      CreateDataset("", 5, 5, GDT_Float32, 10.0f, kDefaultGT);
  ASSERT_NE(nullptr, grid);

  // 1. Source has no geotransform.
  {
    GDALDatasetUniquePtr src(
        mem_drv->Create("", 5, 5, 1, GDT_Float32, nullptr));
    OGRSpatialReference srs;
    srs.SetFromUserInput("EPSG:4326");
    src->SetSpatialRef(&srs);

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALApplyVerticalShiftGrid(src.get(), grid.get(), FALSE,
                                                  1.0, 1.0, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Source dataset has no geotransform"));
    CPLErrorReset();
  }

  // 2. Source has no projection.
  {
    GDALDatasetUniquePtr src(
        mem_drv->Create("", 5, 5, 1, GDT_Float32, nullptr));
    src->SetGeoTransform(const_cast<double*>(kDefaultGT.data()));

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALApplyVerticalShiftGrid(src.get(), grid.get(), FALSE,
                                                  1.0, 1.0, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Source dataset has no projection"));
    CPLErrorReset();
  }

  // 3. Source has multiple bands.
  {
    GDALDatasetUniquePtr src(
        mem_drv->Create("", 5, 5, 2, GDT_Float32, nullptr));
    src->SetGeoTransform(const_cast<double*>(kDefaultGT.data()));
    OGRSpatialReference srs;
    srs.SetFromUserInput("EPSG:4326");
    src->SetSpatialRef(&srs);

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALApplyVerticalShiftGrid(src.get(), grid.get(), FALSE,
                                                  1.0, 1.0, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Only single band source dataset is supported"));
    CPLErrorReset();
  }
}

TEST_F(GdalApplyVerticalShiftGridTest, GridDatasetValidation) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);

  GDALDatasetUniquePtr src =
      CreateDataset("", 5, 5, GDT_Float32, 10.0f, kDefaultGT);
  ASSERT_NE(nullptr, src);

  // 1. Grid has no geotransform.
  {
    GDALDatasetUniquePtr grid(
        mem_drv->Create("", 5, 5, 1, GDT_Float32, nullptr));
    OGRSpatialReference srs;
    srs.SetFromUserInput("EPSG:4326");
    grid->SetSpatialRef(&srs);

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALApplyVerticalShiftGrid(src.get(), grid.get(), FALSE,
                                                  1.0, 1.0, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Grid dataset has no geotransform"));
    CPLErrorReset();
  }

  // 2. Grid has no projection.
  {
    GDALDatasetUniquePtr grid(
        mem_drv->Create("", 5, 5, 1, GDT_Float32, nullptr));
    grid->SetGeoTransform(const_cast<double*>(kDefaultGT.data()));

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALApplyVerticalShiftGrid(src.get(), grid.get(), FALSE,
                                                  1.0, 1.0, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Grid dataset has no projection"));
    CPLErrorReset();
  }

  // 3. Grid has multiple bands.
  {
    GDALDatasetUniquePtr grid(
        mem_drv->Create("", 5, 5, 2, GDT_Float32, nullptr));
    grid->SetGeoTransform(const_cast<double*>(kDefaultGT.data()));
    OGRSpatialReference srs;
    srs.SetFromUserInput("EPSG:4326");
    grid->SetSpatialRef(&srs);

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALApplyVerticalShiftGrid(src.get(), grid.get(), FALSE,
                                                  1.0, 1.0, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Only single band grid dataset is supported"));
    CPLErrorReset();
  }
}

TEST_F(GdalApplyVerticalShiftGridTest, InvalidDataType) {
  GDALDatasetUniquePtr src =
      CreateDataset("", 5, 5, GDT_Float32, 10.0f, kDefaultGT);
  ASSERT_NE(nullptr, src);
  GDALDatasetUniquePtr grid =
      CreateDataset("", 5, 5, GDT_Float32, 5.0f, kDefaultGT);
  ASSERT_NE(nullptr, grid);

  CPLStringList options;
  options.SetNameValue("DATATYPE", "InvalidDataType");

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(nullptr, GDALApplyVerticalShiftGrid(src.get(), grid.get(), FALSE,
                                                1.0, 1.0, options.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid DATATYPE="));
  CPLErrorReset();
}

TEST_F(GdalApplyVerticalShiftGridTest, ForwardAndInverseShift) {
  constexpr int kWidth = 5;
  constexpr int kHeight = 5;
  GDALDatasetUniquePtr src =
      CreateDataset("", kWidth, kHeight, GDT_Float32, 100.0f, kDefaultGT);
  ASSERT_NE(nullptr, src);
  src->SetDescription("TestSourceDataset");
  GDALDatasetUniquePtr grid =
      CreateDataset("", kWidth, kHeight, GDT_Float32, 15.0f, kDefaultGT);
  ASSERT_NE(nullptr, grid);

  // 1. Forward shift (src + grid = 100 + 15 = 115)
  {
    GDALDatasetH hDst = GDALApplyVerticalShiftGrid(src.get(), grid.get(), FALSE,
                                                   1.0, 1.0, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(GDALDataset::FromHandle(hDst));

    EXPECT_STREQ("TestSourceDataset", dst->GetDescription());

    GDALGeoTransform dst_gt;
    EXPECT_EQ(CE_None, dst->GetGeoTransform(dst_gt));
    EXPECT_DOUBLE_EQ(kDefaultGT[0], dst_gt[0]);
    EXPECT_DOUBLE_EQ(kDefaultGT[1], dst_gt[1]);

    const OGRSpatialReference* srs = dst->GetSpatialRef();
    ASSERT_NE(nullptr, srs);

    int success = 0;
    dst->GetRasterBand(1)->GetNoDataValue(&success);
    EXPECT_EQ(FALSE, success);

    std::vector<float> values(kWidth * kHeight, 0.0f);
    EXPECT_EQ(CE_None, dst->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, kWidth, kHeight, values.data(),
                           kWidth, kHeight, GDT_Float32, 0, 0, nullptr));
    for (float val : values) {
      EXPECT_NEAR(115.0f, val, 1e-4f);
    }
  }

  // 2. Inverse shift (src - grid = 100 - 15 = 85)
  {
    GDALDatasetH hDst = GDALApplyVerticalShiftGrid(src.get(), grid.get(), TRUE,
                                                   1.0, 1.0, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(GDALDataset::FromHandle(hDst));

    std::vector<float> values(kWidth * kHeight, 0.0f);
    EXPECT_EQ(CE_None, dst->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, kWidth, kHeight, values.data(),
                           kWidth, kHeight, GDT_Float32, 0, 0, nullptr));
    for (float val : values) {
      EXPECT_NEAR(85.0f, val, 1e-4f);
    }
  }
}

TEST_F(GdalApplyVerticalShiftGridTest, UnitConversions) {
  constexpr int kWidth = 4;
  constexpr int kHeight = 4;
  GDALDatasetUniquePtr src =
      CreateDataset("", kWidth, kHeight, GDT_Float32, 50.0f, kDefaultGT);
  ASSERT_NE(nullptr, src);
  GDALDatasetUniquePtr grid =
      CreateDataset("", kWidth, kHeight, GDT_Float32, 10.0f, kDefaultGT);
  ASSERT_NE(nullptr, grid);

  // dfSrcUnitToMeter = 2.0, dfDstUnitToMeter = 0.5
  // Forward: (50 * 2.0 + 10) / 0.5 = 110 / 0.5 = 220
  {
    GDALDatasetH hDst = GDALApplyVerticalShiftGrid(src.get(), grid.get(), FALSE,
                                                   2.0, 0.5, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(GDALDataset::FromHandle(hDst));

    std::vector<float> values(kWidth * kHeight, 0.0f);
    EXPECT_EQ(CE_None, dst->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, kWidth, kHeight, values.data(),
                           kWidth, kHeight, GDT_Float32, 0, 0, nullptr));
    for (float val : values) {
      EXPECT_NEAR(220.0f, val, 1e-3f);
    }
  }

  // Inverse: (50 * 2.0 - 10) / 0.5 = 90 / 0.5 = 180
  {
    GDALDatasetH hDst = GDALApplyVerticalShiftGrid(src.get(), grid.get(), TRUE,
                                                   2.0, 0.5, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(GDALDataset::FromHandle(hDst));

    std::vector<float> values(kWidth * kHeight, 0.0f);
    EXPECT_EQ(CE_None, dst->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, kWidth, kHeight, values.data(),
                           kWidth, kHeight, GDT_Float32, 0, 0, nullptr));
    for (float val : values) {
      EXPECT_NEAR(180.0f, val, 1e-3f);
    }
  }
}

TEST_F(GdalApplyVerticalShiftGridTest, BlockSizeAndPartialBlocks) {
  constexpr int kWidth = 25;
  constexpr int kHeight = 25;
  GDALDatasetUniquePtr src =
      CreateDataset("", kWidth, kHeight, GDT_Float32, 100.0f, kDefaultGT);
  ASSERT_NE(nullptr, src);
  GDALDatasetUniquePtr grid =
      CreateDataset("", kWidth, kHeight, GDT_Float32, 20.0f, kDefaultGT);
  ASSERT_NE(nullptr, grid);

  CPLStringList options;
  options.SetNameValue("BLOCKSIZE", "16");

  GDALDatasetH hDst = GDALApplyVerticalShiftGrid(src.get(), grid.get(), FALSE,
                                                 1.0, 1.0, options.List());
  ASSERT_NE(nullptr, hDst);
  GDALDatasetUniquePtr dst(GDALDataset::FromHandle(hDst));

  std::vector<float> values(kWidth * kHeight, 0.0f);
  EXPECT_EQ(CE_None, dst->GetRasterBand(1)->RasterIO(
                         GF_Read, 0, 0, kWidth, kHeight, values.data(), kWidth,
                         kHeight, GDT_Float32, 0, 0, nullptr));
  for (float val : values) {
    EXPECT_NEAR(120.0f, val, 1e-4f);
  }
}

TEST_F(GdalApplyVerticalShiftGridTest, SourceNoDataHandling) {
  constexpr int kWidth = 4;
  constexpr int kHeight = 4;
  constexpr double kNoData = -9999.0;
  GDALDatasetUniquePtr src =
      CreateDataset("", kWidth, kHeight, GDT_Float32, 100.0f, kDefaultGT,
                    "EPSG:4326", kNoData, /*has_nodata=*/true);
  ASSERT_NE(nullptr, src);

  // Set pixel (0, 0) to nodata
  float nodata_val = static_cast<float>(kNoData);
  EXPECT_EQ(CE_None,
            src->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 1, 1, &nodata_val,
                                            1, 1, GDT_Float32, 0, 0, nullptr));

  GDALDatasetUniquePtr grid =
      CreateDataset("", kWidth, kHeight, GDT_Float32, 10.0f, kDefaultGT);
  ASSERT_NE(nullptr, grid);

  GDALDatasetH hDst = GDALApplyVerticalShiftGrid(src.get(), grid.get(), FALSE,
                                                 1.0, 1.0, nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALDatasetUniquePtr dst(GDALDataset::FromHandle(hDst));

  int success = 0;
  EXPECT_DOUBLE_EQ(kNoData, dst->GetRasterBand(1)->GetNoDataValue(&success));
  EXPECT_TRUE(success);

  std::vector<float> values(kWidth * kHeight, 0.0f);
  EXPECT_EQ(CE_None, dst->GetRasterBand(1)->RasterIO(
                         GF_Read, 0, 0, kWidth, kHeight, values.data(), kWidth,
                         kHeight, GDT_Float32, 0, 0, nullptr));

  // Pixel (0, 0) should remain nodata
  EXPECT_FLOAT_EQ(nodata_val, values[0]);
  // Other pixels should be shifted: 100 + 10 = 110
  for (size_t i = 1; i < values.size(); ++i) {
    EXPECT_NEAR(110.0f, values[i], 1e-4f);
  }
}

TEST_F(GdalApplyVerticalShiftGridTest, GridNoDataAndErrorOnMissingVertShift) {
  constexpr int kWidth = 6;
  constexpr int kHeight = 6;
  // Grid covers only 3x3 (half the source extent), leaving part of the source
  // un-covered.
  std::array<double, 6> grid_gt = {10.0, 1.0, 0.0, 40.0, 0.0, -1.0};
  constexpr double kGridNoData = -8888.0;
  GDALDatasetUniquePtr grid =
      CreateDataset("", 3, 3, GDT_Float32, 10.0f, grid_gt, "EPSG:4326",
                    kGridNoData, /*has_nodata=*/true);
  ASSERT_NE(nullptr, grid);

  GDALDatasetUniquePtr src =
      CreateDataset("", kWidth, kHeight, GDT_Float32, 100.0f, kDefaultGT);
  ASSERT_NE(nullptr, src);

  // 1. With ERROR_ON_MISSING_VERT_SHIFT=YES: missing coverage triggers failure.
  {
    CPLStringList options;
    options.SetNameValue("ERROR_ON_MISSING_VERT_SHIFT", "YES");

    GDALDatasetH hDst = GDALApplyVerticalShiftGrid(src.get(), grid.get(), FALSE,
                                                   1.0, 1.0, options.List());
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(GDALDataset::FromHandle(hDst));

    std::vector<float> values(kWidth * kHeight, 0.0f);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, dst->GetRasterBand(1)->RasterIO(
                              GF_Read, 0, 0, kWidth, kHeight, values.data(),
                              kWidth, kHeight, GDT_Float32, 0, 0, nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Missing vertical grid value"));
    CPLErrorReset();
  }

  // 2. With ERROR_ON_MISSING_VERT_SHIFT=NO: missing coverage defaults to 0
  // shift.
  {
    CPLStringList options;
    options.SetNameValue("ERROR_ON_MISSING_VERT_SHIFT", "NO");

    GDALDatasetH hDst = GDALApplyVerticalShiftGrid(src.get(), grid.get(), FALSE,
                                                   1.0, 1.0, options.List());
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(GDALDataset::FromHandle(hDst));

    std::vector<float> values(kWidth * kHeight, 0.0f);
    EXPECT_EQ(CE_None, dst->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, kWidth, kHeight, values.data(),
                           kWidth, kHeight, GDT_Float32, 0, 0, nullptr));
    // Covered region has 100 + 10 = 110, un-covered has 100 + 0 = 100
    EXPECT_NEAR(110.0f, values[0], 1e-4f);
    EXPECT_NEAR(100.0f, values[kWidth * kHeight - 1], 1e-4f);
  }
}

TEST_F(GdalApplyVerticalShiftGridTest,
       OptionsResamplingAndMaxErrorAndDataType) {
  constexpr int kWidth = 4;
  constexpr int kHeight = 4;
  GDALDatasetUniquePtr src =
      CreateDataset("", kWidth, kHeight, GDT_Float32, 100.0f, kDefaultGT);
  ASSERT_NE(nullptr, src);
  GDALDatasetUniquePtr grid =
      CreateDataset("", kWidth, kHeight, GDT_Float32, 10.0f, kDefaultGT);
  ASSERT_NE(nullptr, grid);

  // Test NEAREST, BILINEAR, CUBIC resampling, MAX_ERROR=0.0 (exact),
  // DATATYPE=Float64
  const char* const resamplings[] = {"NEAREST", "BILINEAR", "CUBIC"};
  for (const char* resampling : resamplings) {
    CPLStringList options;
    options.SetNameValue("RESAMPLING", resampling);
    options.SetNameValue("MAX_ERROR", "0.0");
    options.SetNameValue("DATATYPE", "Float64");

    GDALDatasetH hDst = GDALApplyVerticalShiftGrid(src.get(), grid.get(), FALSE,
                                                   1.0, 1.0, options.List());
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(GDALDataset::FromHandle(hDst));

    EXPECT_EQ(GDT_Float64, dst->GetRasterBand(1)->GetRasterDataType());

    std::vector<double> values(kWidth * kHeight, 0.0);
    EXPECT_EQ(CE_None, dst->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, kWidth, kHeight, values.data(),
                           kWidth, kHeight, GDT_Float64, 0, 0, nullptr));
    EXPECT_NEAR(110.0, values[0], 1e-3);
  }
}

TEST_F(GdalApplyVerticalShiftGridTest, SrcSrsOptionAndCompoundSrs) {
  constexpr int kWidth = 4;
  constexpr int kHeight = 4;
  GDALDatasetUniquePtr src =
      CreateDataset("", kWidth, kHeight, GDT_Float32, 100.0f, kDefaultGT);
  ASSERT_NE(nullptr, src);
  GDALDatasetUniquePtr grid =
      CreateDataset("", kWidth, kHeight, GDT_Float32, 10.0f, kDefaultGT);
  ASSERT_NE(nullptr, grid);

  // 1. SRC_SRS option override
  {
    CPLStringList options;
    options.SetNameValue("SRC_SRS", "EPSG:4326");

    GDALDatasetH hDst = GDALApplyVerticalShiftGrid(src.get(), grid.get(), FALSE,
                                                   1.0, 1.0, options.List());
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(GDALDataset::FromHandle(hDst));
    ASSERT_NE(nullptr, dst->GetSpatialRef());
  }

  // 2. Compound SRS (exercises StripVertical())
  {
    OGRSpatialReference compound_srs;
    ASSERT_EQ(OGRERR_NONE, compound_srs.SetFromUserInput("EPSG:4326+5773"));
    compound_srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    src->SetSpatialRef(&compound_srs);
    EXPECT_TRUE(src->GetSpatialRef()->IsCompound());

    GDALDatasetH hDst = GDALApplyVerticalShiftGrid(src.get(), grid.get(), FALSE,
                                                   1.0, 1.0, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(GDALDataset::FromHandle(hDst));
    ASSERT_NE(nullptr, dst->GetSpatialRef());
    // Output dataset preserves source dataset's compound SRS.
    EXPECT_TRUE(dst->GetSpatialRef()->IsCompound());
  }
}

TEST_F(GdalApplyVerticalShiftGridTest, OpenVerticalShiftGridSingleGrid) {
  constexpr char kTestDir[] =
      "/vsimem/gdalapplyverticalshiftgrid_test/open_single";
  absl::Cleanup cleanup_dir = [kTestDir] { VSIRmdirRecursive(kTestDir); };

  const std::string grid_path = std::string(kTestDir) + "/grid.tif";
  {
    GDALDatasetUniquePtr ds =
        CreateDataset(grid_path, 4, 4, GDT_Float32, 10.0f, kDefaultGT);
    ASSERT_NE(nullptr, ds);
  }

  // 1. Valid single grid at absolute path.
  {
    int error = -1;
    GDALDatasetH hDS = GDALOpenVerticalShiftGrid(grid_path.c_str(), &error);
    ASSERT_NE(nullptr, hDS);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(hDS));
    EXPECT_EQ(0, error);
  }

  // 2. Valid single grid with nullptr error pointer.
  {
    GDALDatasetH hDS = GDALOpenVerticalShiftGrid(grid_path.c_str(), nullptr);
    ASSERT_NE(nullptr, hDS);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(hDS));
  }

  // 3. Nonexistent relative grid without leading dot -> exercises
  // GetProj4Filename -> proj_grid_info.
  {
    int error = 0;
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetH hDS =
        GDALOpenVerticalShiftGrid("nonexistent_grid.gtx", &error);
    EXPECT_EQ(nullptr, hDS);
    EXPECT_EQ(1, error);
    CPLErrorReset();
  }

  // 4. Nonexistent relative grid with '@' prefix -> missing is OK.
  {
    int error = -1;
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetH hDS =
        GDALOpenVerticalShiftGrid("@nonexistent_grid.gtx", &error);
    EXPECT_EQ(nullptr, hDS);
    EXPECT_EQ(0, error);
    CPLErrorReset();
  }

  // 5. Nonexistent relative grid starting with './'.
  {
    int error = 0;
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetH hDS =
        GDALOpenVerticalShiftGrid("./nonexistent_grid.gtx", &error);
    EXPECT_EQ(nullptr, hDS);
    EXPECT_EQ(1, error);
    CPLErrorReset();
  }

  // 6. Nonexistent absolute path without '@'.
  {
    int error = 0;
    const std::string missing_path = std::string(kTestDir) + "/missing.tif";
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetH hDS = GDALOpenVerticalShiftGrid(missing_path.c_str(), &error);
    EXPECT_EQ(nullptr, hDS);
    EXPECT_EQ(1, error);
    CPLErrorReset();
  }

  // 7. Nonexistent absolute path with '@'.
  {
    int error = -1;
    const std::string missing_path =
        std::string("@") + kTestDir + "/missing.tif";
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetH hDS = GDALOpenVerticalShiftGrid(missing_path.c_str(), &error);
    EXPECT_EQ(nullptr, hDS);
    EXPECT_EQ(0, error);
    CPLErrorReset();
  }
}

TEST_F(GdalApplyVerticalShiftGridTest, OpenVerticalShiftGridMultipleGrids) {
  constexpr char kTestDir[] =
      "/vsimem/gdalapplyverticalshiftgrid_test/open_multi";
  absl::Cleanup cleanup_dir = [kTestDir] { VSIRmdirRecursive(kTestDir); };

  const std::string grid1_path = std::string(kTestDir) + "/grid1.tif";
  const std::string grid2_path = std::string(kTestDir) + "/grid2.tif";
  {
    GDALDatasetUniquePtr ds1 =
        CreateDataset(grid1_path, 4, 4, GDT_Float32, 10.0f, kDefaultGT);
    ASSERT_NE(nullptr, ds1);
    GDALDatasetUniquePtr ds2 =
        CreateDataset(grid2_path, 4, 4, GDT_Float32, 20.0f, kDefaultGT);
    ASSERT_NE(nullptr, ds2);
  }

  // 1. Multiple existing grids -> builds VRT.
  {
    int error = 0;
    const std::string spec = grid1_path + "," + grid2_path;
    GDALDatasetH hDS = GDALOpenVerticalShiftGrid(spec.c_str(), &error);
    ASSERT_NE(nullptr, hDS);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(hDS));
    EXPECT_EQ(1, error);  // Upstream sets *pbError = hDS != nullptr.
  }

  // 2. Multiple existing grids with nullptr error pointer.
  {
    const std::string spec = grid1_path + "," + grid2_path;
    GDALDatasetH hDS = GDALOpenVerticalShiftGrid(spec.c_str(), nullptr);
    ASSERT_NE(nullptr, hDS);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(hDS));
  }

  // 3. Multiple grids with one missing without '@' -> failure.
  {
    int error = 0;
    const std::string spec = grid1_path + ",nonexistent_grid.gtx";
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetH hDS = GDALOpenVerticalShiftGrid(spec.c_str(), &error);
    EXPECT_EQ(nullptr, hDS);
    EXPECT_EQ(1, error);
    CPLErrorReset();
  }

  // 4. Multiple grids with one missing with '@' -> missing is skipped, VRT
  // built from remainder.
  {
    int error = 0;
    const std::string spec = grid1_path + ",@nonexistent_grid.gtx";
    GDALDatasetH hDS = GDALOpenVerticalShiftGrid(spec.c_str(), &error);
    ASSERT_NE(nullptr, hDS);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(hDS));
    EXPECT_EQ(1, error);
  }

  // 5. Multiple grids where all are missing with '@' -> returns nullptr, error
  // is false.
  {
    int error = -1;
    const std::string spec = "@nonexistent1.gtx,@nonexistent2.gtx";
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetH hDS = GDALOpenVerticalShiftGrid(spec.c_str(), &error);
    EXPECT_EQ(nullptr, hDS);
    EXPECT_EQ(0, error);
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Vertical shift grid composite hierarchy and multi-band
// shift grids.
