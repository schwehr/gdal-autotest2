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
// Tests for "vector grid invdist" subcommand
// (apps/gdalalg_vector_grid_invdist.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_grid_invdist.cpp

#include "third_party/gdal/apps/gdalalg_vector_grid_invdist.h"

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
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_grid_invdist_test";
constexpr char kTmpDir[] = "/vsimem/gdalalg_vector_grid_invdist_tmp";

class GdalalgVectorGridInvdistTest : public ::testing::Test {
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

TEST_F(GdalalgVectorGridInvdistTest, ValidationErrors) {
  const std::string in_json = std::string(kTestDir) + "/points.geojson";
  const std::string out_tif = std::string(kTestDir) + "/out.tif";
  ASSERT_NO_FATAL_FAILURE(WriteTestPointsGeoJson(in_json));

  // 1. min-points without radius/radius1/radius2 rejected.
  {
    GDALVectorGridInvdistAlgorithmStandalone alg;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"--min-points", "2", in_json, out_tif}));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("'radius' or 'radius1' and 'radius2'"));
  }

  // 2. max-points without radius/radius1/radius2 rejected.
  {
    GDALVectorGridInvdistAlgorithmStandalone alg;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"--max-points", "5", in_json, out_tif}));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("'radius' or 'radius1' and 'radius2'"));
  }

  // 3. radius and radius1 mutually exclusive.
  {
    GDALVectorGridInvdistAlgorithmStandalone alg;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"--radius", "5", "--radius1", "5",
                                       "--radius2", "3", in_json, out_tif}));
  }
}

TEST_F(GdalalgVectorGridInvdistTest, RadiusAndQuadrantOptions) {
  const std::string in_json = std::string(kTestDir) + "/points.geojson";
  const std::string out_tif = std::string(kTestDir) + "/radius_out.tif";
  ASSERT_NO_FATAL_FAILURE(WriteTestPointsGeoJson(in_json));

  GDALVectorGridInvdistAlgorithmStandalone alg;
  EXPECT_EQ("invdist", alg.GetName());
  ASSERT_TRUE(alg.ParseCommandLineArguments({"--power",
                                             "3",
                                             "--smoothing",
                                             "0.5",
                                             "--radius",
                                             "20",
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
                                             "--extent",
                                             "0,0,10,10",
                                             "--size",
                                             "5,5",
                                             "--zfield",
                                             "z",
                                             "--overwrite",
                                             in_json,
                                             out_tif}));

  const std::string alg_str = alg.GetGridAlgorithm();
  EXPECT_THAT(alg_str, HasSubstr("invdist:power=3:smoothing=0.5"));
  EXPECT_THAT(alg_str, HasSubstr(":radius=20"));
  EXPECT_THAT(alg_str, HasSubstr(":min_points=1"));
  EXPECT_THAT(alg_str, HasSubstr(":max_points=4"));
  EXPECT_THAT(alg_str, HasSubstr(":max_points_per_quadrant=2"));
  EXPECT_THAT(alg_str, HasSubstr(":nodata=-9999"));

  ASSERT_TRUE(alg.Run());
  ASSERT_TRUE(alg.Finalize());

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_THAT(out_ds, NotNull());
  EXPECT_EQ(5, out_ds->GetRasterXSize());
  EXPECT_EQ(5, out_ds->GetRasterYSize());

  std::vector<double> vals(25);
  ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                         GF_Read, 0, 0, 5, 5, vals.data(), 5, 5, GDT_Float64, 0,
                         0, nullptr));
  EXPECT_GT(vals[12], 10.0);
  EXPECT_LT(vals[12], 40.0);
}

TEST_F(GdalalgVectorGridInvdistTest, EllipticalRadiusAndMinPointsPerQuadrant) {
  const std::string in_json = std::string(kTestDir) + "/points.geojson";
  const std::string ellipse_tif = std::string(kTestDir) + "/ellipse_out.tif";
  const std::string quadrant_tif = std::string(kTestDir) + "/quadrant_out.tif";
  ASSERT_NO_FATAL_FAILURE(WriteTestPointsGeoJson(in_json));

  // 1. Elliptical radius with rotation angle (without quadrant limits).
  {
    GDALVectorGridInvdistAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--radius1", "15", "--radius2", "10", "--angle", "30", "--extent",
         "0,0,10,10", "--size", "4,4", "--overwrite", in_json, ellipse_tif}));

    const std::string alg_str = alg.GetGridAlgorithm();
    EXPECT_THAT(alg_str, HasSubstr(":angle=30"));
    EXPECT_THAT(alg_str, HasSubstr(":radius1=15"));
    EXPECT_THAT(alg_str, HasSubstr(":radius2=10"));

    ASSERT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> out_ds(GDALDataset::Open(
        ellipse_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
        nullptr, nullptr));
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(4, out_ds->GetRasterXSize());
    EXPECT_EQ(4, out_ds->GetRasterYSize());
  }

  // 2. min-points-per-quadrant with circular radius succeeds.
  {
    GDALVectorGridInvdistAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--radius", "15", "--min-points-per-quadrant", "1", "--extent",
         "0,0,10,10", "--size", "4,4", "--overwrite", in_json, quadrant_tif}));
    EXPECT_THAT(alg.GetGridAlgorithm(),
                HasSubstr(":min_points_per_quadrant=1"));
    ASSERT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> out_ds(GDALDataset::Open(
        quadrant_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
        nullptr, nullptr));
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(4, out_ds->GetRasterXSize());
    EXPECT_EQ(4, out_ds->GetRasterYSize());
  }

  // 3. Combining radius1 != radius2 with min-points-per-quadrant fails at
  // runtime.
  {
    GDALVectorGridInvdistAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--radius1", "15", "--radius2", "10", "--min-points-per-quadrant", "1",
         "--extent", "0,0,10,10", "--size", "4,4", "--overwrite", in_json,
         ellipse_tif}));
    std::string captured_errors;
    auto capture_handler = [](CPLErr, CPLErrorNum, const char* msg) {
      auto* errors = static_cast<std::string*>(CPLGetErrorHandlerUserData());
      if (msg != nullptr) {
        *errors += msg;
        *errors += '\n';
      }
    };
    CPLErrorHandlerPusher pusher(capture_handler, &captured_errors);
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(captured_errors, HasSubstr("radius1 != radius2 not supported"));
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
