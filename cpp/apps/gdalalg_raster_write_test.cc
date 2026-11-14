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
// Tests for GDALRasterWriteAlgorithm (apps/gdalalg_raster_write.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_write.cpp

#include "third_party/gdal/apps/gdalalg_raster_write.h"

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
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_write_test";

class GdalalgRasterWriteTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    GDALRegister_PNG();
    RegisterOGRGeoPackage();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

void CreateTinyTiff(const std::string& path, uint8_t value) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  ASSERT_EQ(CE_None, ds->SetSpatialRef(&srs));
  GDALGeoTransform gt(0.0, 0.01, 0.0, 1.0, 0.0, -0.01);
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));
  std::vector<uint8_t> pixels(16, value);
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 4, pixels.data(),
                                           4, 4, GDT_Byte, 0, 0, nullptr));
}

TEST_F(GdalalgRasterWriteTest, BasicProperties) {
  GDALRasterWriteAlgorithm alg;
  EXPECT_EQ("write", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_EQ("/programs/gdal_raster_pipeline.html", alg.GetHelpURL());
  EXPECT_TRUE(alg.CanBeLastStep());
  EXPECT_TRUE(alg.GeneratesFilesFromUserInput());
  EXPECT_FALSE(alg.IsNativelyStreamingCompatible());
}

TEST_F(GdalalgRasterWriteTest, WriteFormatOptionsOverwriteAndProgress) {
  const std::string in_tif = std::string(kTestDir) + "/in.tif";
  const std::string out_tif = std::string(kTestDir) + "/out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(in_tif, 42));

  // 1. Write new GTiff with explicit format, creation options, and progress.
  {
    GDALRasterPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", in_tif, "!", "write", "-f", "GTiff", "--co", "COMPRESS=NONE",
         "--co", "TILED=NO", out_tif}));
    int progress_calls = 0;
    auto progress = [](double, const char*, void* data) -> int {
      ++(*static_cast<int*>(data));
      return 1;
    };
    ASSERT_TRUE(pipeline.Run(progress, &progress_calls));
    EXPECT_GT(progress_calls, 0);
    ASSERT_TRUE(pipeline.Finalize());
  }

  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    uint8_t val = 0;
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &val, 1, 1,
                                             GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(42, val);
  }

  // 2. Writing again without --overwrite fails during argument validation.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterPipelineAlgorithm pipeline;
    EXPECT_FALSE(pipeline.ParseCommandLineArguments(
        {"read", in_tif, "!", "write", out_tif}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("already exists"));
    CPLErrorReset();
  }

  // 3. Writing again with --overwrite succeeds.
  {
    GDALRasterPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", in_tif, "!", "write", "--overwrite", out_tif}));
    ASSERT_TRUE(pipeline.Run());
    ASSERT_TRUE(pipeline.Finalize());
  }
}

TEST_F(GdalalgRasterWriteTest, StreamFormatAndAppendRaster) {
  const std::string in_tif = std::string(kTestDir) + "/stream_in.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(in_tif, 77));

  // 1. Stream format returns the input dataset without writing to disk.
  {
    GDALRasterPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", in_tif, "!", "write", "-f", "stream", "ignored"}));
    ASSERT_TRUE(pipeline.Run());
    GDALDataset* out_ds = pipeline.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(4, out_ds->GetRasterXSize());
  }

  // 2. Append raster subdataset to an existing GeoPackage.
  const std::string out_gpkg = std::string(kTestDir) + "/multi.gpkg";
  {
    GDALRasterPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", in_tif, "!", "write", "--co", "RASTER_TABLE=t1", out_gpkg}));
    ASSERT_TRUE(pipeline.Run());
    ASSERT_TRUE(pipeline.Finalize());
  }
  {
    GDALRasterPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", in_tif, "!", "write", "--append", "--co", "RASTER_TABLE=t2",
         out_gpkg}));
    ASSERT_TRUE(pipeline.Run());
    ASSERT_TRUE(pipeline.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    CSLConstList subdatasets = ds->GetMetadata("SUBDATASETS");
    EXPECT_NE(nullptr, subdatasets);
  }
}

}  // namespace
}  // namespace autotest2
