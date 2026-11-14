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
// Tests for GDALRasterPolygonizeAlgorithm (apps/gdalalg_raster_polygonize.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_polygonize.cpp

#include "third_party/gdal/apps/gdalalg_raster_polygonize.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_main.h"
#include "third_party/gdal/apps/gdalalg_vector_write.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_polygonize_test";

class GdalalgRasterPolygonizeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CPLSetConfigOption("CPL_TMPDIR", kTestDir);
    GDALRegister_MEM();
    GDALRegister_GTiff();
    RegisterOGRGeoPackage();
    RegisterOGRShape();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(kTestDir);
  }
};

void CreateIntRaster(const std::string& path, GDALDataType dt = GDT_Byte) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 4, 4, 1, dt, nullptr));
  ASSERT_NE(nullptr, ds);
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  ASSERT_EQ(CE_None, ds->SetSpatialRef(&srs));
  GDALGeoTransform gt(0.0, 1.0, 0.0, 4.0, 0.0, -1.0);
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

  if (dt == GDT_Byte) {
    // Diagonal 2s on a background of 1s tests 4- vs 8-connected behavior.
    const std::vector<uint8_t> pixels = {
        2, 1, 1, 1,  //
        1, 2, 1, 1,  //
        1, 1, 1, 1,  //
        1, 1, 1, 1,  //
    };
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(
                  GF_Write, 0, 0, 4, 4, const_cast<uint8_t*>(pixels.data()), 4,
                  4, GDT_Byte, 0, 0, nullptr));
  } else if (dt == GDT_Int64) {
    const std::vector<int64_t> pixels(16, 10000000000LL);
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(
                  GF_Write, 0, 0, 4, 4, const_cast<int64_t*>(pixels.data()), 4,
                  4, GDT_Int64, 0, 0, nullptr));
  } else if (dt == GDT_Float32) {
    const std::vector<float> pixels(16, 3.5f);
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 4,
                                             const_cast<float*>(pixels.data()),
                                             4, 4, GDT_Float32, 0, 0, nullptr));
  }
}

TEST_F(GdalalgRasterPolygonizeTest, BasicPropertiesAndCanHandleNextStep) {
  GDALRasterPolygonizeAlgorithm alg(false);
  EXPECT_EQ("polygonize", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_EQ("/programs/gdal_raster_polygonize.html", alg.GetHelpURL());
  EXPECT_FALSE(alg.IsNativelyStreamingCompatible());
  EXPECT_EQ(GDAL_OF_RASTER, alg.GetInputType());
  EXPECT_EQ(GDAL_OF_VECTOR, alg.GetOutputType());

  GDALVectorWriteAlgorithm vec_write;
  EXPECT_TRUE(alg.CanHandleNextStep(&vec_write));

  GDALVectorWriteAlgorithm vec_write_stream;
  ASSERT_TRUE(
      vec_write_stream.ParseCommandLineArguments({"-f", "stream", "ignored"}));
  EXPECT_FALSE(alg.CanHandleNextStep(&vec_write_stream));

  GDALRasterPolygonizeAlgorithm non_write_step(false);
  EXPECT_FALSE(alg.CanHandleNextStep(&non_write_step));
}

TEST_F(GdalalgRasterPolygonizeTest, StandaloneByteConnectDiagonalAndCommit) {
  const std::string in_tif = std::string(kTestDir) + "/byte.tif";
  const std::string out_gpkg = std::string(kTestDir) + "/byte_out.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateIntRaster(in_tif, GDT_Byte));

  // 1. Default 4-connected polygonize.
  {
    GDALRasterPolygonizeAlgorithmStandalone alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({in_tif, out_gpkg, "--attribute-name",
                                       "val", "--commit-interval", "2"}));
    ASSERT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());
  }

  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayerByName("polygonize");
    ASSERT_NE(nullptr, lyr);
    EXPECT_GE(lyr->GetLayerDefn()->GetFieldIndex("val"), 0);
    // Two separate diagonal 2-pixels + background 1-pixels = 3 features.
    EXPECT_EQ(3, lyr->GetFeatureCount());
  }

  // 2. Overwrite layer with -c (8-connected) merges diagonal 2-pixels.
  {
    GDALRasterPolygonizeAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({in_tif, out_gpkg, "--update",
                                               "--overwrite-layer", "-c",
                                               "--attribute-name", "val"}));
    ASSERT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());
  }

  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayerByName("polygonize");
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(2, lyr->GetFeatureCount());
  }

  // 3. Append to existing layer doubles feature count.
  {
    GDALRasterPolygonizeAlgorithmStandalone alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({in_tif, out_gpkg, "--update", "--append",
                                       "-c", "--attribute-name", "val"}));
    ASSERT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());
  }

  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayerByName("polygonize");
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(4, lyr->GetFeatureCount());
  }
}

TEST_F(GdalalgRasterPolygonizeTest, FloatAndInt64AndShapefileAndPipeline) {
  // 1. Float32 exercises GDALFPolygonize and OFTReal field creation.
  const std::string f_tif = std::string(kTestDir) + "/float.tif";
  const std::string f_gpkg = std::string(kTestDir) + "/float.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateIntRaster(f_tif, GDT_Float32));
  {
    GDALRasterPolygonizeAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({f_tif, f_gpkg}));
    ASSERT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(f_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(OFTReal, lyr->GetLayerDefn()->GetFieldDefn(0)->GetType());
  }

  // 2. Int64 exercises OFTInteger64 field creation.
  const std::string i64_tif = std::string(kTestDir) + "/int64.tif";
  const std::string i64_gpkg = std::string(kTestDir) + "/int64.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateIntRaster(i64_tif, GDT_Int64));
  {
    GDALRasterPolygonizeAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({i64_tif, i64_gpkg}));
    ASSERT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(i64_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(OFTInteger64, lyr->GetLayerDefn()->GetFieldDefn(0)->GetType());
  }

  // 3. ESRI Shapefile output normalizes layer name to basename.
  const std::string shp_out = std::string(kTestDir) + "/poly_shape.shp";
  {
    GDALRasterPolygonizeAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({f_tif, shp_out}));
    ASSERT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(shp_out.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    EXPECT_NE(nullptr, ds->GetLayerByName("poly_shape"));
  }

  // 4. Pipeline mode with write step and without write step (temp GPKG).
  const std::string pipe_gpkg = std::string(kTestDir) + "/pipe.gpkg";
  {
    GDALMainAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"pipeline", "read", f_tif, "!",
                                               "polygonize", "!", "write",
                                               pipe_gpkg, "--overwrite"}));
    ASSERT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());
  }
  {
    GDALMainAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"pipeline", "read", f_tif, "!", "polygonize", "!", "info"}));
    ASSERT_TRUE(alg.Run());
    const auto* out_str_arg =
        alg.GetActualAlgorithm().GetArg(GDAL_ARG_NAME_OUTPUT_STRING);
    ASSERT_NE(nullptr, out_str_arg);
    const std::string& out = out_str_arg->Get<std::string>();
    EXPECT_THAT(out, HasSubstr("polygonize"));
  }
}

TEST_F(GdalalgRasterPolygonizeTest, ErrorPaths) {
  const std::string in_tif = std::string(kTestDir) + "/err.tif";
  const std::string out_gpkg = std::string(kTestDir) + "/err.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateIntRaster(in_tif, GDT_Byte));

  // Create initial GPKG with layer "polygonize" and field "DN".
  {
    GDALRasterPolygonizeAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({in_tif, out_gpkg}));
    ASSERT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());
  }

  // 1. Existing layer without --overwrite-layer or --append fails.
  {
    GDALRasterPolygonizeAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({in_tif, out_gpkg, "--update"}));
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.Run());
  }

  // 2. Append to non-existent layer fails.
  {
    GDALRasterPolygonizeAlgorithmStandalone alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({in_tif, out_gpkg, "--update", "--append",
                                       "--output-layer", "missing_layer"}));
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.Run());
  }

  // 3. Append with missing attribute field fails.
  {
    GDALRasterPolygonizeAlgorithmStandalone alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({in_tif, out_gpkg, "--update", "--append",
                                       "--attribute-name", "no_such_field"}));
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.Run());
  }

  // 4. Cannot guess driver for unknown extension.
  {
    GDALRasterPolygonizeAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {in_tif, std::string(kTestDir) + "/out.unknown_ext_xyz"}));
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.Run());
  }

  // 5. Unknown explicit driver fails validation.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterPolygonizeAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {in_tif, std::string(kTestDir) + "/out.vec", "-f", "MEM"}));
    EXPECT_FALSE(alg.GetArg(GDAL_ARG_NAME_OUTPUT_FORMAT)->Set("NO_SUCH_DRV"));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("NO_SUCH_DRV"));
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
