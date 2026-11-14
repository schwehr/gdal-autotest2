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
// Tests for GDALVectorGridLinearAlgorithm
// (apps/gdalalg_vector_grid_linear.cpp).
//
// Caveats:
//   In Google3 GDAL builds, QHull is not compiled in, so
//   GDALTriangulationCreateDelaunay() is unavailable and linear gridding
//   fails with CPLE_NotSupported.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_grid_linear.cpp

#include "third_party/gdal/apps/gdalalg_vector_grid_linear.h"

#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_grid_linear_test";

constexpr char kPointsGeoJson[] = R"json({
  "type": "FeatureCollection",
  "features": [
    {
      "type": "Feature",
      "geometry": { "type": "Point", "coordinates": [0.0, 0.0, 0.0] },
      "properties": { "z": 0.0 }
    },
    {
      "type": "Feature",
      "geometry": { "type": "Point", "coordinates": [10.0, 0.0, 10.0] },
      "properties": { "z": 10.0 }
    },
    {
      "type": "Feature",
      "geometry": { "type": "Point", "coordinates": [0.0, 10.0, 10.0] },
      "properties": { "z": 10.0 }
    },
    {
      "type": "Feature",
      "geometry": { "type": "Point", "coordinates": [10.0, 10.0, 20.0] },
      "properties": { "z": 20.0 }
    }
  ]
})json";

class GdalalgVectorGridLinearTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    GDALRegister_MEM();
    RegisterOGRGeoJSON();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
    CPLSetConfigOption("CPL_TMPDIR", kTestDir);
  }

  void TearDown() override {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(kTestDir);
    CPLErrorReset();
  }
};

TEST_F(GdalalgVectorGridLinearTest, PropertiesAndGetGridAlgorithm) {
  GDALVectorGridLinearAlgorithm non_standalone;
  EXPECT_EQ("linear", non_standalone.GetName());
  EXPECT_THAT(non_standalone.GetGridAlgorithm(), HasSubstr("linear:radius="));
  EXPECT_THAT(non_standalone.GetGridAlgorithm(), HasSubstr(":nodata=0"));

  const std::string pts_json = std::string(kTestDir) + "/prop_pts.geojson";
  const std::string out_tif = std::string(kTestDir) + "/prop_out.tif";
  VsiMemTempWrapper pts_wrapper(pts_json, kPointsGeoJson);

  GDALVectorGridLinearAlgorithmStandalone alg;
  EXPECT_EQ("linear", alg.GetName());
  ASSERT_TRUE(alg.ParseCommandLineArguments(
      {"--radius", "5.5", "--nodata", "-999", pts_json, out_tif}));
  EXPECT_EQ("linear:radius=5.5:nodata=-999", alg.GetGridAlgorithm());
}

TEST_F(GdalalgVectorGridLinearTest, RunStandaloneAndPipeline) {
  EXPECT_FALSE(GDALHasTriangulation());

  const std::string pts_json = std::string(kTestDir) + "/pts.geojson";
  const std::string out_tif = std::string(kTestDir) + "/out.tif";
  const std::string pipe_tif = std::string(kTestDir) + "/pipe_out.tif";
  VsiMemTempWrapper pts_wrapper(pts_json, kPointsGeoJson);

  // 1. Standalone execution fails cleanly without QHull support.
  {
    GDALVectorGridLinearAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-i", pts_json, "-o", out_tif, "-f", "GTiff", "--extent", "0,0,10,10",
         "--size", "2,2", "--zfield", "z", "--radius", "20", "--nodata",
         "-1"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("without QHull support"));
    CPLErrorReset();
  }

  // 2. Pipeline execution writes its temp file to CPL_TMPDIR (/vsimem/) and
  // fails cleanly without QHull support.
  {
    auto pipeline =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("pipeline");
    ASSERT_NE(nullptr, pipeline);
    ASSERT_TRUE(pipeline->ParseCommandLineArguments(
        {"read", pts_json, "!", "grid", "linear", "--extent", "0,0,10,10",
         "--size", "2,2", "!", "write", pipe_tif, "-f", "GTiff",
         "--overwrite"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(pipeline->Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("without QHull support"));
    CPLErrorReset();
  }
}

TEST_F(GdalalgVectorGridLinearTest, InvalidRadiusRejected) {
  GDALVectorGridLinearAlgorithmStandalone alg;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_FALSE(alg.ParseCommandLineArguments(
      {"--radius", "-1", "in.geojson", "out.tif"}));
  CPLErrorReset();
}

}  // namespace
}  // namespace autotest2
