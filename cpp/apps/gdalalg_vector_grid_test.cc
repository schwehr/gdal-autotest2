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
// Tests for GDALVectorGridAlgorithm and GDALVectorGridAbstractAlgorithm
// (apps/gdalalg_vector_grid.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_grid.cpp

#include "third_party/gdal/apps/gdalalg_vector_grid.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_abstract_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
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

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_grid_test";

class GdalalgVectorGridTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CPLSetConfigOption("CPL_TMPDIR", kTestDir);
    GDALRegister_MEM();
    GDALRegister_GTiff();
    RegisterOGRGeoPackage();
    VSIRmdirRecursive(kTestDir);
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(kTestDir);
  }
};

void CreateScatteredPointsGpkg(const std::string& path, bool is_3d) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_THAT(drv, NotNull());
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_THAT(ds, NotNull());

  OGRSpatialReference srs;
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  OGRLayer* lyr =
      ds->CreateLayer("pts", &srs, is_3d ? wkbPoint25D : wkbPoint, nullptr);
  ASSERT_THAT(lyr, NotNull());
  OGRFieldDefn z_field("zval", OFTReal);
  ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&z_field));

  struct SamplePt {
    double x, y, z;
  };
  constexpr SamplePt kPts[] = {
      {1.0, 1.0, 10.0},
      {9.0, 1.0, 20.0},
      {1.0, 9.0, 30.0},
      {9.0, 9.0, 40.0},
  };
  for (const auto& p : kPts) {
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    feat->SetField("zval", p.z);
    auto geom = is_3d ? std::make_unique<OGRPoint>(p.x, p.y, p.z)
                      : std::make_unique<OGRPoint>(p.x, p.y);
    feat->SetGeometryDirectly(geom.release());
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  }
}

TEST_F(GdalalgVectorGridTest, StandaloneInvdistWithResolutionBboxAndZField) {
  const std::string in_gpkg = std::string(kTestDir) + "/pts.gpkg";
  const std::string out_tif = std::string(kTestDir) + "/invdist.tif";
  ASSERT_NO_FATAL_FAILURE(CreateScatteredPointsGpkg(in_gpkg, /*is_3d=*/true));

  GDALVectorGridAlgorithmStandalone alg;
  EXPECT_EQ("grid", alg.GetName());
  EXPECT_EQ(GDAL_OF_VECTOR, alg.GetInputType());
  EXPECT_EQ(GDAL_OF_RASTER, alg.GetOutputType());

  ASSERT_TRUE(alg.ParseCommandLineArguments({
      "invdist",
      "-f",
      "GTiff",
      "--co",
      "COMPRESS=NONE",
      "--extent",
      "0,0,10,10",
      "--resolution",
      "2.5,2.5",
      "--bbox",
      "0,0,10,10",
      "--datatype",
      "Float32",
      "--crs",
      "EPSG:4326",
      "--layer",
      "pts",
      "--zfield",
      "zval",
      "--zoffset",
      "5.0",
      "--zmultiply",
      "2.0",
      "--radius1",
      "20.0",
      "--radius2",
      "20.0",
      "--angle",
      "0.0",
      "--min-points",
      "1",
      "--max-points",
      "4",
      "--min-points-per-quadrant",
      "0",
      "--max-points-per-quadrant",
      "2",
      "--nodata",
      "-9999",
      "--overwrite",
      in_gpkg,
      out_tif,
  }));

  int progress_calls = 0;
  auto progress_cb = [](double, const char*, void* data) -> int {
    ++(*static_cast<int*>(data));
    return 1;
  };
  EXPECT_TRUE(alg.Run(progress_cb, &progress_calls));
  EXPECT_GT(progress_calls, 0);
  EXPECT_TRUE(alg.Finalize());

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_THAT(out_ds, NotNull());
  EXPECT_EQ(4, out_ds->GetRasterXSize());
  EXPECT_EQ(4, out_ds->GetRasterYSize());
  EXPECT_EQ(GDT_Float32, out_ds->GetRasterBand(1)->GetRasterDataType());
}

TEST_F(GdalalgVectorGridTest, SqlSizeAndTwoDWarning) {
  const std::string in_gpkg = std::string(kTestDir) + "/pts2d.gpkg";
  const std::string out_sql_tif = std::string(kTestDir) + "/sql.tif";
  const std::string out_2d_tif = std::string(kTestDir) + "/warn2d.tif";
  ASSERT_NO_FATAL_FAILURE(CreateScatteredPointsGpkg(in_gpkg, /*is_3d=*/false));

  // 1. SQL query + --size + --radius.
  {
    GDALVectorGridAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({
        "average",
        "--size",
        "4,4",
        "--sql",
        "SELECT zval, geom FROM pts",
        "--zfield",
        "zval",
        "--radius",
        "20.0",
        "--overwrite",
        in_gpkg,
        out_sql_tif,
    }));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> out_ds(GDALDataset::Open(
        out_sql_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(4, out_ds->GetRasterXSize());
    EXPECT_EQ(4, out_ds->GetRasterYSize());
  }

  // 2. 2D layer without --zfield emits missing-Z warning (with empty layers
  //    and with explicit --layer).
  for (const bool with_layer : {false, true}) {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorGridAlgorithmStandalone alg;
    std::vector<std::string> args = {"nearest", "--size", "2,2", "--overwrite"};
    if (with_layer) {
      args.push_back("--layer");
      args.push_back("pts");
    }
    args.push_back(in_gpkg);
    args.push_back(out_2d_tif);
    ASSERT_TRUE(alg.ParseCommandLineArguments(args));
    EXPECT_TRUE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("lacks a Z component"));
    EXPECT_TRUE(alg.Finalize());
    CPLErrorReset();
  }
}

TEST_F(GdalalgVectorGridTest, NonStandaloneStepExecution) {
  const std::string in_gpkg = std::string(kTestDir) + "/non_standalone.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateScatteredPointsGpkg(in_gpkg, /*is_3d=*/true));
  GDALDatasetUniquePtr src_ds(
      GDALDataset::Open(in_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_THAT(src_ds, NotNull());

  {
    GDALVectorGridAlgorithm alg(/*standaloneStep=*/false);
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"nearest", "--size", "4,4", "--extent", "0,0,10,10"}));
    auto* step =
        cpl::down_cast<GDALPipelineStepAlgorithm*>(&alg.GetActualAlgorithm());
    ASSERT_THAT(step, NotNull());
    step->GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.Run());

    GDALDataset* out_ds = step->GetOutputDataset().GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(4, out_ds->GetRasterXSize());
    EXPECT_EQ(4, out_ds->GetRasterYSize());
  }

  // "linear" relies on QHull, which is disabled in google3 builds.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorGridAlgorithm alg(/*standaloneStep=*/false);
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"linear", "--size", "4,4", "--extent", "0,0,10,10"}));
    auto* step =
        cpl::down_cast<GDALPipelineStepAlgorithm*>(&alg.GetActualAlgorithm());
    ASSERT_THAT(step, NotNull());
    step->GetInputDatasets().emplace_back(src_ds.get());
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("without QHull support"));
    CPLErrorReset();
  }
}

TEST_F(GdalalgVectorGridTest, AllSubAlgorithmsInstantiateAndRun) {
  const std::string in_gpkg = std::string(kTestDir) + "/subalgs.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateScatteredPointsGpkg(in_gpkg, /*is_3d=*/true));

  for (const char* subalg :
       {"invdistnn", "minimum", "maximum", "range", "count", "average-distance",
        "average-distance-points"}) {
    const std::string out_tif = std::string(kTestDir) + "/" + subalg + ".tif";
    GDALVectorGridAlgorithmStandalone alg;
    std::vector<std::string> args = {subalg,     "--size",    "2,2",
                                     "--extent", "0,0,10,10", "--overwrite"};
    // invdistnn requires a strictly positive --radius (and upstream
    // GDALGridParseAlgorithmAndOptions leaks *ppOptions when radius <= 0).
    if (std::string_view(subalg) == "invdistnn") {
      args.push_back("--radius");
      args.push_back("20.0");
    }
    args.push_back(in_gpkg);
    args.push_back(out_tif);
    ASSERT_TRUE(alg.ParseCommandLineArguments(args)) << subalg;
    EXPECT_TRUE(alg.Run()) << subalg;
    EXPECT_TRUE(alg.Finalize()) << subalg;
  }
}

TEST_F(GdalalgVectorGridTest, ErrorAndValidationPaths) {
  const std::string in_gpkg = std::string(kTestDir) + "/err.gpkg";
  const std::string out_tif = std::string(kTestDir) + "/err.tif";
  ASSERT_NO_FATAL_FAILURE(CreateScatteredPointsGpkg(in_gpkg, /*is_3d=*/true));

  // 1. Direct Run() on parent "grid" fails.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorGridAlgorithmStandalone alg;
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("should not be called directly"));
  }

  // 2. --resolution without --extent fails validation.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorGridAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"average", "--resolution", "1,1", in_gpkg, out_tif}));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("'resolution' should be defined when 'extent' is."));
  }

  // 3. --radius1 without --radius2 fails validation.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorGridAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"average", "--radius1", "5", in_gpkg, out_tif}));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("'radius2' should be defined when 'radius1' is."));
  }

  // 4. --radius2 without --radius1 fails validation.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorGridAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"average", "--radius2", "5", in_gpkg, out_tif}));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("'radius1' should be defined when 'radius2' is."));
  }
  CPLErrorReset();
}

}  // namespace
}  // namespace autotest2
