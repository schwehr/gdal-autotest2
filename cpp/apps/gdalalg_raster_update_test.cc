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
// Tests for GDALRasterUpdateAlgorithm (apps/gdalalg_raster_update.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_update.cpp

#include "third_party/gdal/apps/gdalalg_raster_update.h"

#include <array>
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
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_update_test";

class GdalalgRasterUpdateTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

void CreateGeoTiff(const std::string& path, int size, double xmin, double ymax,
                   double res, uint8_t fill_val, bool with_srs,
                   bool with_overviews) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), size, size, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALGeoTransform gt(xmin, res, 0.0, ymax, 0.0, -res);
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));
  if (with_srs) {
    OGRSpatialReference srs;
    ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    ASSERT_EQ(CE_None, ds->SetSpatialRef(&srs));
  }
  std::vector<uint8_t> pixels(size * size, fill_val);
  ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, size, size,
                                                    pixels.data(), size, size,
                                                    GDT_Byte, 0, 0, nullptr));
  if (with_overviews) {
    constexpr std::array<int, 1> kLevels = {2};
    ASSERT_EQ(CE_None,
              ds->BuildOverviews("NEAREST", kLevels.size(), kLevels.data(), 0,
                                 nullptr, nullptr, nullptr, nullptr));
  }
}

TEST_F(GdalalgRasterUpdateTest, BasicProperties) {
  GDALRasterUpdateAlgorithm alg;
  EXPECT_EQ("update", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_EQ("/programs/gdal_raster_update.html", alg.GetHelpURL());
}

TEST_F(GdalalgRasterUpdateTest, UpdateWithOverviewsAndProgress) {
  const std::string src_tif = std::string(kTestDir) + "/src.tif";
  const std::string dst_tif = std::string(kTestDir) + "/dst.tif";
  ASSERT_NO_FATAL_FAILURE(CreateGeoTiff(src_tif, 4, 2.0, 6.0, 1.0, 99,
                                        /*with_srs=*/true,
                                        /*with_overviews=*/false));
  ASSERT_NO_FATAL_FAILURE(CreateGeoTiff(dst_tif, 8, 0.0, 8.0, 1.0, 10,
                                        /*with_srs=*/true,
                                        /*with_overviews=*/true));

  {
    GDALRasterUpdateAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {src_tif, dst_tif, "--resampling", "nearest", "--wo", "INIT_DEST=0",
         "--to", "SAMPLE_GRID=NO", "--et", "0.1"}));
    int progress_calls = 0;
    auto progress = [](double, const char*, void* data) -> int {
      ++(*static_cast<int*>(data));
      return 1;
    };
    ASSERT_TRUE(alg.Run(progress, &progress_calls));
    EXPECT_GT(progress_calls, 0);
    ASSERT_TRUE(alg.Finalize());
  }

  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(dst_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  uint8_t val = 0;
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Read, 3, 3, 1, 1, &val, 1, 1,
                                           GDT_Byte, 0, 0, nullptr));
  EXPECT_EQ(99, val);

  // Overview band at (1, 1) corresponds to full-res (2..3, 2..3) -> 99.
  GDALRasterBand* ovr = ds->GetRasterBand(1)->GetOverview(0);
  ASSERT_NE(nullptr, ovr);
  uint8_t ovr_val = 0;
  ASSERT_EQ(CE_None, ovr->RasterIO(GF_Read, 1, 1, 1, 1, &ovr_val, 1, 1,
                                   GDT_Byte, 0, 0, nullptr));
  EXPECT_EQ(99, ovr_val);
}

TEST_F(GdalalgRasterUpdateTest, CutlineGeometryAndNoUpdateOverviews) {
  const std::string src_tif = std::string(kTestDir) + "/src_clip.tif";
  const std::string dst_tif = std::string(kTestDir) + "/dst_clip.tif";
  ASSERT_NO_FATAL_FAILURE(CreateGeoTiff(src_tif, 8, 0.0, 8.0, 1.0, 77,
                                        /*with_srs=*/true,
                                        /*with_overviews=*/false));
  ASSERT_NO_FATAL_FAILURE(CreateGeoTiff(dst_tif, 8, 0.0, 8.0, 1.0, 10,
                                        /*with_srs=*/true,
                                        /*with_overviews=*/true));

  {
    GDALRasterUpdateAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {src_tif, dst_tif, "--geometry", "POLYGON ((0 4,4 4,4 8,0 8,0 4))",
         "--geometry-crs", "EPSG:4326", "--no-update-overviews"}));
    ASSERT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());
  }

  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(dst_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  uint8_t inside = 0;
  uint8_t outside = 0;
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Read, 1, 1, 1, 1, &inside, 1, 1,
                                           GDT_Byte, 0, 0, nullptr));
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Read, 6, 6, 1, 1, &outside, 1, 1,
                                           GDT_Byte, 0, 0, nullptr));
  EXPECT_EQ(77, inside);
  EXPECT_EQ(10, outside);

  // Overview was NOT updated due to --no-update-overviews.
  GDALRasterBand* ovr = ds->GetRasterBand(1)->GetOverview(0);
  ASSERT_NE(nullptr, ovr);
  uint8_t ovr_val = 0;
  ASSERT_EQ(CE_None, ovr->RasterIO(GF_Read, 0, 0, 1, 1, &ovr_val, 1, 1,
                                   GDT_Byte, 0, 0, nullptr));
  EXPECT_EQ(10, ovr_val);
}

TEST_F(GdalalgRasterUpdateTest, BothNoCrsAndMismatchedCrsOverviewWarning) {
  const std::string src_nocrs = std::string(kTestDir) + "/src_nocrs.tif";
  const std::string dst_nocrs = std::string(kTestDir) + "/dst_nocrs.tif";
  ASSERT_NO_FATAL_FAILURE(CreateGeoTiff(src_nocrs, 4, 0.0, 4.0, 1.0, 55,
                                        /*with_srs=*/false,
                                        /*with_overviews=*/false));
  ASSERT_NO_FATAL_FAILURE(CreateGeoTiff(dst_nocrs, 4, 0.0, 4.0, 1.0, 10,
                                        /*with_srs=*/false,
                                        /*with_overviews=*/true));

  // 1. Both have no CRS: overviews still refreshed cleanly.
  {
    GDALRasterUpdateAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({src_nocrs, dst_nocrs}));
    ASSERT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());
  }

  // 2. Source has CRS, destination does not: emits warning and still updates.
  const std::string src_crs = std::string(kTestDir) + "/src_crs.tif";
  ASSERT_NO_FATAL_FAILURE(CreateGeoTiff(src_crs, 4, 0.0, 4.0, 1.0, 66,
                                        /*with_srs=*/true,
                                        /*with_overviews=*/false));
  {
    GDALRasterUpdateAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({src_crs, dst_nocrs}));
    struct CapturedWarning {
      bool saw_warning = false;
      std::string messages;
    } captured;
    auto capture_handler = [](CPLErr err_class, CPLErrorNum, const char* msg) {
      auto* state = static_cast<CapturedWarning*>(CPLGetErrorHandlerUserData());
      if (err_class == CE_Warning && msg != nullptr) {
        state->saw_warning = true;
        state->messages += msg;
        state->messages += '\n';
      }
    };
    CPLErrorHandlerPusher pusher(capture_handler, &captured);
    CPLErrorReset();
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(captured.saw_warning);
    EXPECT_THAT(captured.messages, HasSubstr("Overviews can not be updated"));
    EXPECT_TRUE(alg.Finalize());
    CPLErrorReset();
  }
}

TEST_F(GdalalgRasterUpdateTest, ErrorPaths) {
  const std::string tif = std::string(kTestDir) + "/same.tif";
  ASSERT_NO_FATAL_FAILURE(CreateGeoTiff(tif, 4, 0.0, 4.0, 1.0, 10,
                                        /*with_srs=*/true,
                                        /*with_overviews=*/false));

  // 1. Same source and destination filename fails.
  {
    GDALRasterUpdateAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({tif, tif}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("must be different"));
  }

  // 2. Same dataset object fails.
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(tif.c_str(), GDAL_OF_RASTER | GDAL_OF_UPDATE, nullptr,
                          nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    GDALRasterUpdateAlgorithm alg;
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(ds.get()));
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_OUTPUT)->Set(ds.get()));
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.Run());
  }

  // 3. Invalid geometry fails.
  const std::string tif2 = std::string(kTestDir) + "/other.tif";
  ASSERT_NO_FATAL_FAILURE(CreateGeoTiff(tif2, 4, 0.0, 4.0, 1.0, 20,
                                        /*with_srs=*/true,
                                        /*with_overviews=*/false));
  {
    GDALRasterUpdateAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {tif, tif2, "--geometry", "NOT_VALID_WKT"}));
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.Run());
  }
}

}  // namespace
}  // namespace autotest2
