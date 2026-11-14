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
// Tests for GDALVectorGridNearestAlgorithm
// (apps/gdalalg_vector_grid_nearest.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_grid_nearest.cpp
//   https://gdal.org/programs/gdal_vector_grid.html

#include "third_party/gdal/apps/gdalalg_vector_grid_nearest.h"

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_grid_nearest_test";

void CreatePointsGpkg(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer = ds->CreateLayer("points", nullptr, wkbPoint25D, nullptr);
  ASSERT_NE(nullptr, layer);
  OGRFieldDefn field("z", OFTReal);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&field));

  struct Pt {
    double x;
    double y;
    double z;
  };
  const Pt pts[] = {
      {0.5, 0.5, 10.0},
      {3.5, 0.5, 20.0},
      {0.5, 3.5, 30.0},
      {3.5, 3.5, 40.0},
  };
  for (const auto& p : pts) {
    auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
    feat->SetField("z", p.z);
    feat->SetGeometryDirectly(new OGRPoint(p.x, p.y, p.z));
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }
}

class GdalalgVectorGridNearestTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    RegisterOGRGeoPackage();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgVectorGridNearestTest, BasicPropertiesAndGetGridAlgorithm) {
  GDALVectorGridNearestAlgorithmStandalone alg;
  EXPECT_EQ("nearest", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_EQ("/programs/gdal_vector_grid.html", alg.GetHelpURL());

  const std::string default_algo = alg.GetGridAlgorithm();
  EXPECT_THAT(default_algo, HasSubstr("nearest:angle=0:nodata=0"));
  EXPECT_EQ(std::string::npos, default_algo.find("radius"));
}

TEST_F(GdalalgVectorGridNearestTest, RadiusAndEllipticalValidationErrors) {
  const std::string in_gpkg = std::string(kTestDir) + "/val.gpkg";
  const std::string out_tif = std::string(kTestDir) + "/val.tif";
  ASSERT_NO_FATAL_FAILURE(CreatePointsGpkg(in_gpkg));

  // Mutual exclusion between --radius and --radius1.
  {
    GDALVectorGridNearestAlgorithmStandalone alg;
    WithQuietHandler quiet;
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"--radius", "2", "--radius1", "2",
                                       "--radius2", "2", in_gpkg, out_tif}));
  }

  // --radius1 without --radius2.
  {
    GDALVectorGridNearestAlgorithmStandalone alg;
    WithQuietHandler quiet;
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"--radius1", "2", in_gpkg, out_tif}));
  }
}

TEST_F(GdalalgVectorGridNearestTest, RunWithRadiusAndEllipticalRadii) {
  const std::string in_gpkg = std::string(kTestDir) + "/pts.gpkg";
  const std::string out_radius = std::string(kTestDir) + "/out_radius.tif";
  const std::string out_ellipse = std::string(kTestDir) + "/out_ellipse.tif";
  ASSERT_NO_FATAL_FAILURE(CreatePointsGpkg(in_gpkg));

  // 1. Circular search radius.
  {
    GDALVectorGridNearestAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--extent", "0,0,4,4", "--size", "4,4", "--radius", "1.0", "--nodata",
         "-999", "--zfield", "z", "--overwrite", in_gpkg, out_radius}));
    EXPECT_THAT(alg.GetGridAlgorithm(),
                HasSubstr("nearest:angle=0:nodata=-999:radius=1"));
    ASSERT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_radius.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(4, ds->GetRasterXSize());
    EXPECT_EQ(4, ds->GetRasterYSize());
    std::vector<double> buf(16, 0.0);
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 4, 4, buf.data(), 4,
                                             4, GDT_Float64, 0, 0, nullptr));
    // Raster is top-down (ymax=4 at row 0):
    //   (0.5, 3.5) -> buf[0] = 30.0, (3.5, 3.5) -> buf[3] = 40.0
    //   (0.5, 0.5) -> buf[12] = 10.0, (3.5, 0.5) -> buf[15] = 20.0.
    EXPECT_DOUBLE_EQ(30.0, buf[0]);
    EXPECT_DOUBLE_EQ(40.0, buf[3]);
    EXPECT_DOUBLE_EQ(10.0, buf[12]);
    EXPECT_DOUBLE_EQ(20.0, buf[15]);
  }

  // 2. Elliptical search radii with angle and nodata.
  {
    GDALVectorGridNearestAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--extent", "0,0,4,4", "--size", "4,4", "--radius1", "5.0",
         "--radius2", "3.0", "--angle", "45", "--nodata", "-1", "--overwrite",
         in_gpkg, out_ellipse}));
    EXPECT_THAT(alg.GetGridAlgorithm(),
                HasSubstr("nearest:angle=45:nodata=-1:radius1=5:radius2=3"));
    ASSERT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  {
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        out_ellipse.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
        nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(4, ds->GetRasterXSize());
    EXPECT_EQ(4, ds->GetRasterYSize());
  }
}

}  // namespace
}  // namespace autotest2
