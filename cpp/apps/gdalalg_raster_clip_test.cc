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
// Tests for GDALRasterClipAlgorithm (apps/gdalalg_raster_clip.cpp and
// apps/gdalalg_raster_clip.h).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_clip.cpp

#include "third_party/gdal/apps/gdalalg_raster_clip.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_clip_test";

std::unique_ptr<GDALDataset> CreateMemRaster(int width, int height,
                                             bool with_gt = true,
                                             int epsg = 4326,
                                             bool bottom_up = false,
                                             bool rotated = false) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!drv) return nullptr;
  std::unique_ptr<GDALDataset> ds(
      drv->Create("mem_raster", width, height, 1, GDT_Byte, nullptr));
  if (!ds) return nullptr;

  if (epsg > 0) {
    OGRSpatialReference srs;
    if (srs.importFromEPSG(epsg) != OGRERR_NONE) return nullptr;
    ds->SetSpatialRef(&srs);
  }
  if (with_gt) {
    double gt[6] = {0.0, 1.0, 0.0, static_cast<double>(height), 0.0, -1.0};
    if (bottom_up) {
      gt[3] = 0.0;
      gt[5] = 1.0;
    }
    if (rotated) {
      gt[2] = 0.5;
      gt[4] = 0.5;
    }
    if (ds->SetGeoTransform(gt) != CE_None) return nullptr;
  }

  std::vector<uint8_t> pixels(width * height);
  for (int i = 0; i < width * height; ++i) {
    pixels[i] = static_cast<uint8_t>(i + 1);
  }
  if (ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, width, height,
                                     pixels.data(), width, height, GDT_Byte, 0,
                                     0, nullptr) != CE_None) {
    return nullptr;
  }
  return ds;
}

class GdalalgRasterClipTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    GDALRegister_VRT();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgRasterClipTest, PropertiesAndWindowValidation) {
  GDALRasterClipAlgorithm alg;
  EXPECT_EQ("clip", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_STREQ("/programs/gdal_raster_clip.html", alg.GetHelpURL().c_str());

  // Window with non-positive width or height fails validation.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterClipAlgorithm bad;
    EXPECT_FALSE(bad.ParseCommandLineArguments({"--window", "0,0,0,4"}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("width > 0 and height > 0"));
    CPLErrorReset();
  }
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterClipAlgorithm bad;
    EXPECT_FALSE(bad.ParseCommandLineArguments({"--window", "0,0,4,-1"}));
    CPLErrorReset();
  }
}

TEST_F(GdalalgRasterClipTest, WindowClippingAndErrors) {
  auto src = CreateMemRaster(6, 6);
  ASSERT_NE(nullptr, src);

  // --window + --add-alpha is not supported.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterClipAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src.get());
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--window", "1,1,2,2", "--add-alpha"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("'alpha' argument is not supported with 'window'"));
    CPLErrorReset();
  }

  // Valid --window clips accurately.
  {
    GDALRasterClipAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--window", "1,1,2,3"}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(2, out->GetRasterXSize());
    EXPECT_EQ(3, out->GetRasterYSize());
  }

  // Outside window fails by default, succeeds with --allow-bbox-outside-source.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterClipAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--window", "4,4,4,4"}));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }
  {
    GDALRasterClipAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--window", "4,4,4,4", "--allow-bbox-outside-source"}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(4, out->GetRasterXSize());
  }
}

TEST_F(GdalalgRasterClipTest, GeotransformAndMissingGeometryErrors) {
  // Raster without geotransform fails.
  {
    auto no_gt = CreateMemRaster(4, 4, /*with_gt=*/false);
    ASSERT_NE(nullptr, no_gt);
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterClipAlgorithm alg;
    alg.GetInputDatasets().emplace_back(no_gt.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--bbox", "0,0,2,2"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("without a geotransform"));
    CPLErrorReset();
  }

  // Raster with rotated geotransform fails.
  {
    auto rot = CreateMemRaster(4, 4, /*with_gt=*/true, 4326, false,
                               /*rotated=*/true);
    ASSERT_NE(nullptr, rot);
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterClipAlgorithm alg;
    alg.GetInputDatasets().emplace_back(rot.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--bbox", "0,0,2,2"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("rotation terms"));
    CPLErrorReset();
  }

  // Missing clip geometry fails.
  {
    auto src = CreateMemRaster(4, 4);
    ASSERT_NE(nullptr, src);
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterClipAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src.get());
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("must be specified"));
    CPLErrorReset();
  }
}

TEST_F(GdalalgRasterClipTest, BboxProjwinAndOutsideSourceOptions) {
  auto src = CreateMemRaster(10, 10, true, 4326);
  ASSERT_NE(nullptr, src);

  // Fast-path rectangle clip with --bbox and --bbox-crs.
  {
    GDALRasterClipAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--bbox", "2,2,6,6", "--bbox-crs", "EPSG:4326"}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(4, out->GetRasterXSize());
    EXPECT_EQ(4, out->GetRasterYSize());
  }

  // Outside bbox fails without --allow-bbox-outside-source, succeeds with it.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterClipAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--bbox", "8,8,15,15"}));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }
  {
    GDALRasterClipAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--bbox", "8,8,15,15", "--allow-bbox-outside-source"}));
    EXPECT_TRUE(alg.Run());
    ASSERT_NE(nullptr, alg.GetOutputDataset().GetDatasetRef());
  }
}

TEST_F(GdalalgRasterClipTest, GeometryCutlineAddAlphaOnlyBboxAndBottomUp) {
  auto src = CreateMemRaster(10, 10, true, 4326);
  ASSERT_NE(nullptr, src);

  // Non-rectangular geometry uses GDALWarp cutline; --add-alpha adds alpha.
  {
    GDALRasterClipAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--geometry", "POLYGON ((1 1, 8 1, 4 8, 1 1))", "--geometry-crs",
         "EPSG:4326", "--add-alpha"}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(2, out->GetRasterCount());
    EXPECT_EQ(GCI_AlphaBand, out->GetRasterBand(2)->GetColorInterpretation());
  }

  // --only-bbox turns triangle into rectangle fast path.
  {
    GDALRasterClipAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--geometry", "POLYGON ((1 1, 8 1, 4 8, 1 1))", "--only-bbox"}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(1, out->GetRasterCount());
    EXPECT_EQ(7, out->GetRasterXSize());
    EXPECT_EQ(7, out->GetRasterYSize());
  }

  // Geometry outside extent fails unless --allow-bbox-outside-source is set.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterClipAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--geometry", "POLYGON ((5 5, 15 5, 10 15, 5 5))"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("partially or totally outside"));
    CPLErrorReset();
  }
  {
    GDALRasterClipAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--geometry", "POLYGON ((5 5, 15 5, 10 15, 5 5))",
         "--allow-bbox-outside-source"}));
    EXPECT_TRUE(alg.Run());
    ASSERT_NE(nullptr, alg.GetOutputDataset().GetDatasetRef());
  }

  // Bottom-up raster (gt[5] > 0) uses warp path even for rectangle bbox.
  {
    auto bottom_up = CreateMemRaster(10, 10, true, 4326, /*bottom_up=*/true);
    ASSERT_NE(nullptr, bottom_up);
    GDALRasterClipAlgorithm alg;
    alg.GetInputDatasets().emplace_back(bottom_up.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--bbox", "2,2,6,6"}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(4, out->GetRasterXSize());
    EXPECT_EQ(4, out->GetRasterYSize());
  }
}

TEST_F(GdalalgRasterClipTest, LikeDatasetNoCrsErrorAndStandaloneRun) {
  auto src = CreateMemRaster(10, 10, true, 4326);
  auto like_no_crs = CreateMemRaster(4, 4, true, /*epsg=*/0);
  ASSERT_NE(nullptr, src);
  ASSERT_NE(nullptr, like_no_crs);

  // --like raster without CRS fails.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterClipAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src.get());
    alg.GetArg("like")->Get<GDALArgDatasetValue>().Set(like_no_crs.get());
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("has no CRS"));
    CPLErrorReset();
  }

  // Standalone execution reading/writing GTiff files.
  const std::string in_tif = std::string(kTestDir) + "/in.tif";
  const std::string out_tif = std::string(kTestDir) + "/out.tif";
  GDALDriver* gtiff = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff);
  std::unique_ptr<GDALDataset> created(gtiff->CreateCopy(
      in_tif.c_str(), src.get(), FALSE, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, created);
  created.reset();

  GDALRasterClipAlgorithmStandalone standalone;
  ASSERT_TRUE(standalone.ParseCommandLineArguments(
      {"--bbox", "1,1,5,5", "--overwrite", in_tif, out_tif}));
  EXPECT_TRUE(standalone.Run());
  EXPECT_TRUE(standalone.Finalize());

  std::unique_ptr<GDALDataset> reopened(
      GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, reopened);
  EXPECT_EQ(4, reopened->GetRasterXSize());
  EXPECT_EQ(4, reopened->GetRasterYSize());
}

}  // namespace
}  // namespace autotest2
