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
// Tests for GDALRasterIndexAlgorithm (apps/gdalalg_raster_index.cpp and
// apps/gdalalg_raster_index.h).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_index.cpp

#include "third_party/gdal/apps/gdalalg_raster_index.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_index_test";

void CreateTileTiff(const std::string& path, double origin_x, double origin_y,
                    double pixel_size, int epsg) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(epsg));
  ds->SetSpatialRef(&srs);
  std::array<double, 6> gt = {origin_x, pixel_size, 0.0,
                              origin_y, 0.0,        -pixel_size};
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt.data()));
}

TEST(GdalalgRasterIndexTest, BasicPropertiesAndCreateTileIndex) {
  GDALRegister_GTiff();
  RegisterOGRGeoPackage();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  const std::string tile1 = std::string(kTestDir) + "/t1.tif";
  const std::string tile2 = std::string(kTestDir) + "/t2.tif";
  const std::string out_gpkg = std::string(kTestDir) + "/index.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateTileTiff(tile1, 0.0, 4.0, 1.0, 4326));
  ASSERT_NO_FATAL_FAILURE(CreateTileTiff(tile2, 4.0, 4.0, 1.0, 4326));

  {
    GDALRasterIndexAlgorithm alg;
    EXPECT_EQ("index", alg.GetName());
    EXPECT_FALSE(alg.GetDescription().empty());
    EXPECT_FALSE(alg.GetHelpURL().empty());

    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--output-layer=tiles", "--location-name=loc", "--metadata=FOO=BAR",
         tile1, tile2, out_gpkg}));
    auto progress = [](double, const char*, void*) -> int { return 1; };
    EXPECT_TRUE(alg.Run(progress, nullptr));
    EXPECT_TRUE(alg.Finalize());
  }

  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, ds);
  OGRLayer* lyr = ds->GetLayerByName("tiles");
  ASSERT_NE(nullptr, lyr);
  EXPECT_EQ(2, lyr->GetFeatureCount());
  EXPECT_STREQ("BAR", lyr->GetMetadataItem("FOO"));
  std::unique_ptr<OGRFeature> feat(lyr->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  EXPECT_GE(feat->GetFieldIndex("loc"), 0);
}

TEST(GdalalgRasterIndexTest, RecursiveFiltersPixelSizeCrsAndSkipErrors) {
  GDALRegister_GTiff();
  RegisterOGRGeoPackage();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  const std::string sub_dir = std::string(kTestDir) + "/sub";
  ASSERT_EQ(0, VSIMkdir(sub_dir.c_str(), 0755));

  const std::string fine_tif = sub_dir + "/fine.tif";
  const std::string coarse_tif = sub_dir + "/coarse.tif";
  const std::string skip_tif = sub_dir + "/other.dat";
  ASSERT_NO_FATAL_FAILURE(CreateTileTiff(fine_tif, 0.0, 4.0, 1.0, 4326));
  ASSERT_NO_FATAL_FAILURE(CreateTileTiff(coarse_tif, 10.0, 10.0, 5.0, 4326));
  ASSERT_NO_FATAL_FAILURE(CreateTileTiff(skip_tif, 20.0, 20.0, 1.0, 4326));

  const std::string out_gpkg = std::string(kTestDir) + "/filtered.gpkg";
  {
    GDALRasterIndexAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--recursive", "--filename-filter=*.tif", "--min-pixel-size=0.5",
         "--max-pixel-size=2.0", "--absolute-path", "--dst-crs=EPSG:4326",
         "--source-crs-field-name=src_crs", "--source-crs-format=EPSG",
         "--skip-errors", kTestDir, std::string(kTestDir) + "/nonexistent.tif",
         out_gpkg}));
    WithQuietHandler quiet;
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, ds);
  OGRLayer* lyr = ds->GetLayer(0);
  ASSERT_NE(nullptr, lyr);
  EXPECT_EQ(1, lyr->GetFeatureCount());
  std::unique_ptr<OGRFeature> feat(lyr->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  EXPECT_STREQ("EPSG:4326", feat->GetFieldAsString("src_crs"));
}

TEST(GdalalgRasterIndexTest, ErrorPaths) {
  GDALRegister_GTiff();
  GDALRegister_MEM();
  RegisterOGRGeoPackage();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  const std::string tile = std::string(kTestDir) + "/err_tile.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTileTiff(tile, 0.0, 4.0, 1.0, 4326));

  // 1. Input dataset passed as object rather than name fails.
  {
    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_NE(nullptr, mem_drv);
    GDALDatasetUniquePtr mem_ds(
        mem_drv->Create("", 2, 2, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, mem_ds);

    GDALRasterIndexAlgorithm alg;
    auto* input_arg = alg.GetArg(GDAL_ARG_NAME_INPUT);
    ASSERT_NE(nullptr, input_arg);
    std::vector<GDALArgDatasetValue> inputs;
    inputs.emplace_back(mem_ds.get());
    input_arg->Set(std::move(inputs));
    alg.GetArg(GDAL_ARG_NAME_OUTPUT)
        ->Set(std::string(kTestDir) + "/obj_fail.gpkg");

    CPLErrorReset();
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("provided by name"));
    CPLErrorReset();
  }

  // 2. Invalid metadata KEY=VALUE fails validation.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterIndexAlgorithm alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--metadata=MISSING_EQUALS", tile,
         std::string(kTestDir) + "/bad_md.gpkg"}));
    CPLErrorReset();
  }

  // 3. Unknown output extension fails SetupOutputDataset().
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterIndexAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {tile, std::string(kTestDir) + "/out.unknown_ext"}));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }

  // 4. MEM output without --output-layer fails
  // SetDefaultOutputLayerNameIfNeeded().
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterIndexAlgorithm alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"-f", "MEM", tile, "mem_no_layer"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("layer"));
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
