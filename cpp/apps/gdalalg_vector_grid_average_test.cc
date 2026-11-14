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
// Tests for GDALVectorGridAverageAlgorithm
// (apps/gdalalg_vector_grid_average.cpp and
// apps/gdalalg_vector_grid_average.h).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_grid_average.cpp

#include "third_party/gdal/apps/gdalalg_vector_grid_average.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_grid_average_test";

void WriteScatteredPointsGeoJson(const std::string& path) {
  constexpr std::string_view kGeoJson = R"json({
  "type": "FeatureCollection",
  "features": [
    { "type": "Feature", "geometry": { "type": "Point", "coordinates": [0.5, 0.5, 10.0] }, "properties": {} },
    { "type": "Feature", "geometry": { "type": "Point", "coordinates": [3.5, 0.5, 20.0] }, "properties": {} },
    { "type": "Feature", "geometry": { "type": "Point", "coordinates": [0.5, 3.5, 30.0] }, "properties": {} },
    { "type": "Feature", "geometry": { "type": "Point", "coordinates": [3.5, 3.5, 40.0] }, "properties": {} }
  ]
})json";
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  ASSERT_EQ(kGeoJson.size(),
            VSIFWriteL(kGeoJson.data(), 1, kGeoJson.size(), fp));
  ASSERT_EQ(0, VSIFCloseL(fp));
}

class GdalalgVectorGridAverageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    RegisterOGRGeoJSON();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
    CPLSetConfigOption("CPL_TMPDIR", kTestDir);
  }

  void TearDown() override {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(kTestDir);
  }
};

TEST_F(GdalalgVectorGridAverageTest, PropertiesAndGetGridAlgorithmFormatting) {
  {
    GDALVectorGridAverageAlgorithm alg;
    EXPECT_EQ("average", alg.GetName());
    EXPECT_FALSE(alg.GetDescription().empty());
    EXPECT_STREQ("/programs/gdal_vector_grid.html", alg.GetHelpURL().c_str());
    EXPECT_EQ("average:angle=0:nodata=0", alg.GetGridAlgorithm());
  }

  {
    GDALVectorGridAverageAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--radius", "5"}));
    EXPECT_EQ("average:angle=0:nodata=0:radius=5", alg.GetGridAlgorithm());
  }

  {
    GDALVectorGridAverageAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--radius1", "4", "--radius2", "2", "--angle", "45", "--nodata",
         "-9999", "--min-points", "1", "--max-points", "8",
         "--min-points-per-quadrant", "1", "--max-points-per-quadrant", "2"}));
    EXPECT_EQ(
        "average:angle=45:nodata=-9999:radius1=4:radius2=2:min_points=1:"
        "max_points=8:min_points_per_quadrant=1:max_points_per_quadrant=2",
        alg.GetGridAlgorithm());
  }
}

TEST_F(GdalalgVectorGridAverageTest, ValidationMaxPointsRequiresQuadrantArg) {
  CPLErrorReset();
  WithQuietHandler quiet;
  GDALVectorGridAverageAlgorithm alg;
  EXPECT_FALSE(alg.ParseCommandLineArguments({"--max-points", "4"}));
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("min-points-per-quadrant"));
  CPLErrorReset();

  // Providing min-points-per-quadrant or max-points-per-quadrant satisfies it.
  {
    GDALVectorGridAverageAlgorithm ok1;
    EXPECT_TRUE(ok1.ParseCommandLineArguments(
        {"--max-points", "4", "--min-points-per-quadrant", "1"}));
  }
  {
    GDALVectorGridAverageAlgorithm ok2;
    EXPECT_TRUE(ok2.ParseCommandLineArguments(
        {"--max-points", "4", "--max-points-per-quadrant", "2"}));
  }
}

TEST_F(GdalalgVectorGridAverageTest, StandaloneAndStepExecution) {
  const std::string in_geojson = std::string(kTestDir) + "/pts.geojson";
  const std::string out_tif = std::string(kTestDir) + "/grid.tif";
  ASSERT_NO_FATAL_FAILURE(WriteScatteredPointsGeoJson(in_geojson));

  // Standalone execution writing a 2x2 GTiff grid.
  {
    GDALVectorGridAverageAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--extent", "0,0,4,4", "--size", "2,2", "--radius", "5", "--overwrite",
         in_geojson, out_tif}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> out(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(2, out->GetRasterXSize());
    EXPECT_EQ(2, out->GetRasterYSize());

    std::vector<double> vals(4, 0.0);
    ASSERT_EQ(CE_None, out->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 2, 2, vals.data(), 2, 2, GDT_Float64,
                           0, 0, nullptr));
    // All 4 points are within radius 5 of every pixel center, average is 25.0.
    for (double v : vals) {
      EXPECT_DOUBLE_EQ(25.0, v);
    }
  }

  // Non-standalone step execution with radius1/radius2.
  {
    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_NE(nullptr, mem_drv);
    std::unique_ptr<GDALDataset> src(
        mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, src);
    OGRLayer* lyr = src->CreateLayer("pts", nullptr, wkbPoint25D, nullptr);
    ASSERT_NE(nullptr, lyr);
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    auto pt = std::make_unique<OGRPoint>(1.0, 1.0, 5.0);
    ASSERT_EQ(OGRERR_NONE, feat->SetGeometryDirectly(pt.release()));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));

    GDALVectorGridAverageAlgorithm step;
    step.GetInputDatasets().emplace_back(src.get());
    ASSERT_TRUE(step.ParseCommandLineArguments(
        {"--extent", "0,0,2,2", "--size", "2,2", "--radius1", "3", "--radius2",
         "3", "--nodata", "-1"}));
    EXPECT_TRUE(step.Run());
    GDALDataset* out = step.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(2, out->GetRasterXSize());
    EXPECT_EQ(2, out->GetRasterYSize());
  }
}

}  // namespace
}  // namespace autotest2
