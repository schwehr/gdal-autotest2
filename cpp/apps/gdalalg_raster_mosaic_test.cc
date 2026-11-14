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
// Tests for GDALRasterMosaicAlgorithm (apps/gdalalg_raster_mosaic.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_mosaic.cpp

#include "third_party/gdal/apps/gdalalg_raster_mosaic.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_raster_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_mosaic_test";

class GdalalgRasterMosaicTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    // Register VRT before constructing GDALRasterMosaicAlgorithm so
    // VRTDerivedRasterBand pixel functions are populated.
    GDALRegister_VRT();
    VSIRmdirRecursive(kTestDir);
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

void CreateTileTiff(const std::string& path, double min_x, double max_y,
                    uint8_t fill) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_THAT(drv, NotNull());
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_THAT(ds, NotNull());
  double gt[6] = {min_x, 1.0, 0.0, max_y, 0.0, -1.0};
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));
  OGRSpatialReference srs;
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  ASSERT_EQ(CE_None, ds->SetSpatialRef(&srs));
  std::vector<uint8_t> pixels(16, fill);
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 4, pixels.data(),
                                           4, 4, GDT_Byte, 0, 0, nullptr));
}

TEST_F(GdalalgRasterMosaicTest, StandaloneBasicMosaic) {
  const std::string tile1 = std::string(kTestDir) + "/tile1.tif";
  const std::string tile2 = std::string(kTestDir) + "/tile2.tif";
  const std::string out_vrt = std::string(kTestDir) + "/mosaic.vrt";
  ASSERT_NO_FATAL_FAILURE(CreateTileTiff(tile1, 0.0, 4.0, 10));
  ASSERT_NO_FATAL_FAILURE(CreateTileTiff(tile2, 4.0, 4.0, 20));

  GDALRasterMosaicAlgorithmStandalone alg;
  EXPECT_EQ("mosaic", alg.GetName());
  ASSERT_TRUE(alg.ParseCommandLineArguments({tile1, tile2, out_vrt}));
  EXPECT_TRUE(alg.Run());
  EXPECT_TRUE(alg.Finalize());

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_vrt.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_THAT(out_ds, NotNull());
  EXPECT_EQ(8, out_ds->GetRasterXSize());
  EXPECT_EQ(4, out_ds->GetRasterYSize());
  EXPECT_EQ(1, out_ds->GetRasterCount());
}

TEST_F(GdalalgRasterMosaicTest, AddAlphaAndPixelFunctionWithArgs) {
  const std::string tile1 = std::string(kTestDir) + "/ov1.tif";
  const std::string tile2 = std::string(kTestDir) + "/ov2.tif";
  const std::string out_tif = std::string(kTestDir) + "/sum.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTileTiff(tile1, 0.0, 4.0, 10));
  ASSERT_NO_FATAL_FAILURE(CreateTileTiff(tile2, 0.0, 4.0, 25));

  // Add-alpha without pixel function.
  {
    const std::string alpha_vrt = std::string(kTestDir) + "/alpha.vrt";
    GDALRasterMosaicAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--add-alpha", tile1, tile2, alpha_vrt}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> out_ds(GDALDataset::Open(
        alpha_vrt.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(2, out_ds->GetRasterCount());
    EXPECT_EQ(GCI_AlphaBand,
              out_ds->GetRasterBand(2)->GetColorInterpretation());
  }

  // Pixel function "sum" with k=5 inside a raster pipeline.
  GDALRasterPipelineAlgorithm pipeline;
  ASSERT_TRUE(pipeline.ParseCommandLineArguments({
      "mosaic",
      "--pixel-function",
      "sum",
      "--pixel-function-arg",
      "k=5",
      tile1,
      tile2,
      "!",
      "write",
      out_tif,
      "--overwrite",
  }));
  EXPECT_TRUE(pipeline.Run());
  EXPECT_TRUE(pipeline.Finalize());

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_THAT(out_ds, NotNull());
  std::vector<uint8_t> buf(16, 0);
  ASSERT_EQ(CE_None,
            out_ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 4, 4, buf.data(),
                                               4, 4, GDT_Byte, 0, 0, nullptr));
  // 10 + 25 + 5 = 40
  EXPECT_EQ(40, buf[0]);
}

TEST_F(GdalalgRasterMosaicTest, MissingInputFileFails) {
  const std::string missing = std::string(kTestDir) + "/does_not_exist.tif";
  const std::string out_vrt = std::string(kTestDir) + "/fail.vrt";

  WithQuietHandler quiet;
  CPLErrorReset();
  GDALRasterMosaicAlgorithmStandalone alg;
  ASSERT_TRUE(alg.ParseCommandLineArguments({missing, out_vrt}));
  EXPECT_FALSE(alg.Run());
  EXPECT_NE(CE_None, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST_F(GdalalgRasterMosaicTest, UsageAndInvalidPixelFunction) {
  GDALRasterMosaicAlgorithmStandalone alg;
  EXPECT_THAT(alg.GetUsageForCLI(false), HasSubstr("add-alpha"));

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_FALSE(alg.ParseCommandLineArguments(
      {"--pixel-function", "no_such_func", "a.tif", "b.vrt"}));
  CPLErrorReset();
}

}  // namespace
}  // namespace autotest2
