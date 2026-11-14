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
// Tests for "vector rasterize" subcommand (apps/gdalalg_vector_rasterize.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_rasterize.cpp

#include "third_party/gdal/apps/gdalalg_vector_rasterize.h"

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
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_rasterize_test";
constexpr char kTmpDir[] = "/vsimem/gdalalg_vector_rasterize_tmp";

class GdalalgVectorRasterizeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
    ASSERT_EQ(0, VSIMkdir(kTmpDir, 0755));
    CPLSetConfigOption("CPL_TMPDIR", kTmpDir);
    GDALRegister_MEM();
    GDALRegister_GTiff();
    RegisterOGRGeoJSON();
    RegisterOGRGeoPackage();
  }

  void TearDown() override {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(kTestDir);
    VSIRmdirRecursive(kTmpDir);
    CPLErrorReset();
  }
};

void CreateTestVectorGpkg(const std::string& path) {
  GDALDriver* gpkg_drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_THAT(gpkg_drv, NotNull());
  GDALDatasetUniquePtr ds(
      gpkg_drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_THAT(ds, NotNull());

  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  OGRLayer* lyr = ds->CreateLayer("polys", &srs, wkbPolygon25D, nullptr);
  ASSERT_THAT(lyr, NotNull());
  OGRFieldDefn fd("val", OFTReal);
  ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&fd));

  auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
  feat->SetField("val", 42.0);
  auto poly = std::make_unique<OGRPolygon>();
  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(2.0, 2.0, 99.0);
  ring->addPoint(8.0, 2.0, 99.0);
  ring->addPoint(8.0, 8.0, 99.0);
  ring->addPoint(2.0, 8.0, 99.0);
  ring->addPoint(2.0, 2.0, 99.0);
  poly->addRingDirectly(ring.release());
  feat->SetGeometryDirectly(poly.release());
  ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
}

TEST_F(GdalalgVectorRasterizeTest, MissingResolutionAndSizeFails) {
  const std::string in_gpkg = std::string(kTestDir) + "/in.gpkg";
  const std::string out_tif = std::string(kTestDir) + "/err.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTestVectorGpkg(in_gpkg));

  GDALVectorRasterizeAlgorithmStandalone alg;
  ASSERT_TRUE(alg.ParseCommandLineArguments(
      {"--burn", "1", "--overwrite", in_gpkg, out_tif}));
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_FALSE(alg.Run());
  EXPECT_THAT(
      CPLGetLastErrorMsg(),
      HasSubstr("Must specify output resolution (--resolution) or size"));
  CPLErrorReset();
}

TEST_F(GdalalgVectorRasterizeTest, MutuallyExclusive3DAndBurnFails) {
  const std::string in_gpkg = std::string(kTestDir) + "/in.gpkg";
  const std::string out_tif = std::string(kTestDir) + "/err_3d_burn.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTestVectorGpkg(in_gpkg));

  GDALVectorRasterizeAlgorithmStandalone alg;
  ASSERT_TRUE(alg.ParseCommandLineArguments({"--3d", "--burn", "0", "--extent",
                                             "0,0,10,10", "--size", "10,10",
                                             "--overwrite", in_gpkg, out_tif}));
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_FALSE(alg.Run());
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Argument '-3d' not allowed with '-burn <value>'"));
  CPLErrorReset();
}

TEST_F(GdalalgVectorRasterizeTest, BurnSizeAllTouchedInvertMultiBandAnd3D) {
  const std::string in_gpkg = std::string(kTestDir) + "/in.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateTestVectorGpkg(in_gpkg));

  // 1. Multi-band burn with --size, --extent, --init, --nodata, --crs,
  // --datatype, --all-touched, --invert, --transformer-option, --co,
  // --optimization RASTER, and progress callback.
  const std::string out_multi = std::string(kTestDir) + "/multi.tif";
  {
    GDALVectorRasterizeAlgorithmStandalone alg;
    EXPECT_EQ("rasterize", alg.GetName());
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--burn",
                                               "10,20",
                                               "--init",
                                               "1,2",
                                               "--nodata",
                                               "-1",
                                               "--crs",
                                               "EPSG:4326",
                                               "--transformer-option",
                                               "GCPS_OK=FALSE",
                                               "--extent",
                                               "0,0,10,10",
                                               "--size",
                                               "10,10",
                                               "--datatype",
                                               "Float32",
                                               "--all-touched",
                                               "--invert",
                                               "--optimization",
                                               "RASTER",
                                               "-f",
                                               "GTiff",
                                               "--co",
                                               "COMPRESS=NONE",
                                               "--overwrite",
                                               in_gpkg,
                                               out_multi}));
    bool progress_called = false;
    auto progress_cb = [](double, const char*, void* user_data) -> int {
      *static_cast<bool*>(user_data) = true;
      return 1;
    };
    ASSERT_TRUE(alg.Run(progress_cb, &progress_called));
    ASSERT_TRUE(alg.Finalize());
    EXPECT_TRUE(progress_called);

    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_multi.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(2, out_ds->GetRasterCount());
    EXPECT_EQ(10, out_ds->GetRasterXSize());
    EXPECT_EQ(GDT_Float32, out_ds->GetRasterBand(1)->GetRasterDataType());
    float corner = 0.0f;
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 1, 1, &corner, 1, 1, GDT_Float32, 0,
                           0, nullptr));
    EXPECT_FLOAT_EQ(10.0f, corner);
  }

  // 2. --3d extraction (mutually exclusive with --burn).
  const std::string out_3d = std::string(kTestDir) + "/out_3d.tif";
  {
    GDALVectorRasterizeAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--3d", "--extent", "0,0,10,10",
                                               "--size", "10,10", "--overwrite",
                                               in_gpkg, out_3d}));
    ASSERT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_3d.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_THAT(out_ds, NotNull());
    double center = 0.0;
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 5, 5, 1, 1, &center, 1, 1, GDT_Float64, 0,
                           0, nullptr));
    EXPECT_DOUBLE_EQ(99.0, center);
  }
}

TEST_F(GdalalgVectorRasterizeTest, ResolutionTapLayerWhereSqlAndAdd) {
  const std::string in_gpkg = std::string(kTestDir) + "/in.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateTestVectorGpkg(in_gpkg));

  // 1. --resolution, --tap, --attribute-name, --input-layer, --where,
  // --dialect.
  const std::string out_attr = std::string(kTestDir) + "/attr.tif";
  {
    GDALVectorRasterizeAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--attribute-name", "val", "--input-layer", "polys", "--where",
         "val > 10", "--dialect", "OGRSQL", "--extent", "0.2,0.2,9.8,9.8",
         "--resolution", "1,1", "--tap", "--optimization", "VECTOR",
         "--overwrite", in_gpkg, out_attr}));
    ASSERT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_attr.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(10, out_ds->GetRasterXSize());
    EXPECT_EQ(10, out_ds->GetRasterYSize());
    double center = 0.0;
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 5, 5, 1, 1, &center, 1, 1, GDT_Float64, 0,
                           0, nullptr));
    EXPECT_DOUBLE_EQ(42.0, center);
  }

  // 2. --add with --band onto existing raster created above (adds 8.0 -> 50.0).
  {
    GDALVectorRasterizeAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--add", "--band", "1", "--burn", "8", in_gpkg, out_attr}));
    ASSERT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_attr.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_THAT(out_ds, NotNull());
    double center = 0.0;
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 5, 5, 1, 1, &center, 1, 1, GDT_Float64, 0,
                           0, nullptr));
    EXPECT_DOUBLE_EQ(50.0, center);
  }

  // 3. --sql query.
  const std::string out_sql = std::string(kTestDir) + "/sql.tif";
  {
    GDALVectorRasterizeAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--sql", "SELECT * FROM polys", "--burn", "7", "--extent", "0,0,10,10",
         "--size", "10,10", "--overwrite", in_gpkg, out_sql}));
    ASSERT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());
  }
}

TEST_F(GdalalgVectorRasterizeTest, PipelineNonStandaloneStep) {
  const std::string in_gpkg = std::string(kTestDir) + "/in_pipe.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateTestVectorGpkg(in_gpkg));

  // Verify non-standalone temp output behavior for pipeline step execution.
  std::unique_ptr<GDALDataset> src_ds(
      GDALDataset::Open(in_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_THAT(src_ds, NotNull());

  GDALVectorRasterizeAlgorithm alg(/*standaloneStep=*/false);
  alg.GetInputDatasets().emplace_back(src_ds.get());
  ASSERT_TRUE(alg.ParseCommandLineArguments(
      {"--burn", "13", "--extent", "0,0,10,10", "--size", "10,10"}));
  ASSERT_TRUE(alg.Run());

  GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
  ASSERT_THAT(out_ds, NotNull());
  double center = 0.0;
  ASSERT_EQ(CE_None,
            out_ds->GetRasterBand(1)->RasterIO(GF_Read, 5, 5, 1, 1, &center, 1,
                                               1, GDT_Float64, 0, 0, nullptr));
  EXPECT_DOUBLE_EQ(13.0, center);
}

}  // namespace
}  // namespace autotest2
