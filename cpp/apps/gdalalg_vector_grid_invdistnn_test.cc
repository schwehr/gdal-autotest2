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
// Tests for "vector grid invdistnn" subcommand
// (apps/gdalalg_vector_grid_invdistnn.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_grid_invdistnn.cpp

#include "third_party/gdal/apps/gdalalg_vector_grid_invdistnn.h"

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_grid_invdistnn_test";
constexpr char kTmpDir[] = "/vsimem/gdalalg_vector_grid_invdistnn_tmp";

class GdalalgVectorGridInvdistNNTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
    ASSERT_EQ(0, VSIMkdir(kTmpDir, 0755));
    CPLSetConfigOption("CPL_TMPDIR", kTmpDir);
    GDALRegister_MEM();
    GDALRegister_GTiff();
    RegisterOGRGeoJSON();
  }

  void TearDown() override {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(kTestDir);
    VSIRmdirRecursive(kTmpDir);
    CPLErrorReset();
  }
};

void WriteTestPointsGeoJson(const std::string& path) {
  const std::string json = R"json({
  "type": "FeatureCollection",
  "features": [
    {
      "type": "Feature",
      "geometry": { "type": "Point", "coordinates": [1.0, 1.0, 10.0] },
      "properties": { "z": 10.0 }
    },
    {
      "type": "Feature",
      "geometry": { "type": "Point", "coordinates": [9.0, 1.0, 20.0] },
      "properties": { "z": 20.0 }
    },
    {
      "type": "Feature",
      "geometry": { "type": "Point", "coordinates": [1.0, 9.0, 30.0] },
      "properties": { "z": 30.0 }
    },
    {
      "type": "Feature",
      "geometry": { "type": "Point", "coordinates": [9.0, 9.0, 40.0] },
      "properties": { "z": 40.0 }
    }
  ]
})json";
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb");
  ASSERT_THAT(fp, NotNull());
  EXPECT_EQ(json.size(), VSIFWriteL(json.data(), 1, json.size(), fp));
  EXPECT_EQ(0, VSIFCloseL(fp));
}

TEST_F(GdalalgVectorGridInvdistNNTest, DefaultAndCustomOptions) {
  const std::string in_json = std::string(kTestDir) + "/points.geojson";
  const std::string out_default = std::string(kTestDir) + "/default.tif";
  const std::string out_custom = std::string(kTestDir) + "/custom.tif";
  ASSERT_NO_FATAL_FAILURE(WriteTestPointsGeoJson(in_json));

  // 1. Default parameters with radius.
  {
    GDALVectorGridInvdistNNAlgorithmStandalone alg;
    EXPECT_EQ("invdistnn", alg.GetName());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--radius", "20", "--extent", "0,0,10,10", "--size", "4,4", "--zfield",
         "z", "--overwrite", in_json, out_default}));
    const std::string alg_str = alg.GetGridAlgorithm();
    EXPECT_THAT(alg_str, HasSubstr("invdistnn:power=2:smoothing=0:nodata=0"));
    EXPECT_THAT(alg_str, HasSubstr(":radius=20"));
    EXPECT_THAT(alg_str, HasSubstr(":max_points=12"));

    ASSERT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        out_default.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
        nullptr, nullptr));
    ASSERT_THAT(ds, NotNull());
    EXPECT_EQ(4, ds->GetRasterXSize());
    EXPECT_EQ(4, ds->GetRasterYSize());
  }

  // 2. All optional parameters specified.
  {
    GDALVectorGridInvdistNNAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--power",
                                               "3",
                                               "--smoothing",
                                               "0.25",
                                               "--radius",
                                               "15",
                                               "--min-points",
                                               "1",
                                               "--max-points",
                                               "6",
                                               "--min-points-per-quadrant",
                                               "1",
                                               "--max-points-per-quadrant",
                                               "3",
                                               "--nodata",
                                               "-999",
                                               "--extent",
                                               "0,0,10,10",
                                               "--size",
                                               "5,5",
                                               "--overwrite",
                                               in_json,
                                               out_custom}));
    const std::string alg_str = alg.GetGridAlgorithm();
    EXPECT_THAT(alg_str, HasSubstr("invdistnn:power=3:smoothing=0.25"));
    EXPECT_THAT(alg_str, HasSubstr(":nodata=-999"));
    EXPECT_THAT(alg_str, HasSubstr(":radius=15"));
    EXPECT_THAT(alg_str, HasSubstr(":min_points=1"));
    EXPECT_THAT(alg_str, HasSubstr(":max_points=6"));
    EXPECT_THAT(alg_str, HasSubstr(":min_points_per_quadrant=1"));
    EXPECT_THAT(alg_str, HasSubstr(":max_points_per_quadrant=3"));

    ASSERT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_custom.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_THAT(ds, NotNull());
    std::vector<double> vals(25);
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 5, 5, vals.data(),
                                             5, 5, GDT_Float64, 0, 0, nullptr));
    EXPECT_GT(vals[12], 10.0);
    EXPECT_LT(vals[12], 40.0);
  }
}

}  // namespace
}  // namespace autotest2
