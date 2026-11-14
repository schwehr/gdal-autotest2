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
// Tests for GDALRasterReadAlgorithm (apps/gdalalg_raster_read.cpp and
// apps/gdalalg_raster_read.h).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_read.cpp

#include "third_party/gdal/apps/gdalalg_raster_read.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/gdalalg_raster_pipeline.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_read_test";

void CreateTinyTiff(const std::string& path, uint8_t value) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> pixels(16, value);
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 4, pixels.data(),
                                           4, 4, GDT_Byte, 0, 0, nullptr));
}

TEST(GdalalgRasterReadTest, StandaloneAndMixedMode) {
  GDALRegister_GTiff();
  GDALRegister_PNG();
  RegisterOGRGeoPackage();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  const std::string in_tif = std::string(kTestDir) + "/in.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(in_tif, 55));

  {
    GDALRasterReadAlgorithm alg;
    EXPECT_EQ("read", alg.GetName());
    EXPECT_FALSE(alg.GetDescription().empty());
    EXPECT_FALSE(alg.GetHelpURL().empty());

    ASSERT_TRUE(alg.ParseCommandLineArguments({in_tif}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(4, out_ds->GetRasterXSize());
    EXPECT_EQ(4, out_ds->GetRasterYSize());
  }

  // Mixed raster/vector mode opens GTiff without --input-format, rejects
  // --input-format=GTiff (missing DCAP_VECTOR), and accepts GPKG.
  {
    GDALRasterReadAlgorithm mixed_alg(/*openForMixedRasterVector=*/true);
    ASSERT_TRUE(mixed_alg.ParseCommandLineArguments({in_tif}));
    EXPECT_TRUE(mixed_alg.Run());
    EXPECT_NE(nullptr, mixed_alg.GetOutputDataset().GetDatasetRef());
  }
  {
    GDALRasterReadAlgorithm mixed_alg(/*openForMixedRasterVector=*/true);
    CPLErrorStateBackuper error_backuper(CPLQuietErrorHandler);
    CPLErrorReset();
    EXPECT_FALSE(
        mixed_alg.ParseCommandLineArguments({"--input-format=GTiff", in_tif}));
    EXPECT_NE(std::string_view::npos,
              std::string_view(CPLGetLastErrorMsg()).find("DCAP_VECTOR"));
    CPLErrorReset();
  }
  {
    const std::string in_gpkg = std::string(kTestDir) + "/in.gpkg";
    GDALDriver* gpkg_drv = GetGDALDriverManager()->GetDriverByName("GPKG");
    ASSERT_NE(nullptr, gpkg_drv);
    {
      GDALDatasetUniquePtr gpkg_ds(
          gpkg_drv->Create(in_gpkg.c_str(), 4, 4, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, gpkg_ds);
      GDALGeoTransform gt{0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
      ASSERT_EQ(CE_None, gpkg_ds->SetGeoTransform(gt));
      std::vector<uint8_t> pixels(16, 77);
      ASSERT_EQ(CE_None, gpkg_ds->GetRasterBand(1)->RasterIO(
                             GF_Write, 0, 0, 4, 4, pixels.data(), 4, 4,
                             GDT_Byte, 0, 0, nullptr));
    }
    GDALRasterReadAlgorithm mixed_alg(/*openForMixedRasterVector=*/true);
    ASSERT_TRUE(
        mixed_alg.ParseCommandLineArguments({"--input-format=GPKG", in_gpkg}));
    EXPECT_TRUE(mixed_alg.Run());
    EXPECT_NE(nullptr, mixed_alg.GetOutputDataset().GetDatasetRef());
  }
}

TEST(GdalalgRasterReadTest, InsideRasterPipeline) {
  GDALRegister_GTiff();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  const std::string in_tif = std::string(kTestDir) + "/pipe_in.tif";
  const std::string out_tif = std::string(kTestDir) + "/pipe_out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(in_tif, 88));

  {
    GDALRasterPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", "--input-format=GTiff", in_tif, "!", "write", out_tif,
         "--overwrite"}));
    EXPECT_TRUE(pipeline.Run());
    EXPECT_TRUE(pipeline.Finalize());
  }

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, out_ds);
  std::vector<uint8_t> pixels(16, 0);
  ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                         GF_Read, 0, 0, 4, 4, pixels.data(), 4, 4, GDT_Byte, 0,
                         0, nullptr));
  EXPECT_EQ(88, pixels[0]);
}

}  // namespace
}  // namespace autotest2
