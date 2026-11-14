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
// Tests for vector grid data metrics algorithms
// (apps/gdalalg_vector_grid_data_metrics.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_grid_data_metrics.cpp

#include "third_party/gdal/apps/gdalalg_vector_grid_data_metrics.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_grid_data_metrics_test";

constexpr char kPointsGeoJson[] = R"json({
  "type": "FeatureCollection",
  "features": [
    {
      "type": "Feature",
      "geometry": { "type": "Point", "coordinates": [1.0, 1.0, 10.0] },
      "properties": { "z": 10.0 }
    },
    {
      "type": "Feature",
      "geometry": { "type": "Point", "coordinates": [2.0, 2.0, 20.0] },
      "properties": { "z": 20.0 }
    },
    {
      "type": "Feature",
      "geometry": { "type": "Point", "coordinates": [1.0, 9.0, 15.0] },
      "properties": { "z": 15.0 }
    },
    {
      "type": "Feature",
      "geometry": { "type": "Point", "coordinates": [9.0, 1.0, 25.0] },
      "properties": { "z": 25.0 }
    },
    {
      "type": "Feature",
      "geometry": { "type": "Point", "coordinates": [8.0, 8.0, 30.0] },
      "properties": { "z": 30.0 }
    },
    {
      "type": "Feature",
      "geometry": { "type": "Point", "coordinates": [9.0, 9.0, 40.0] },
      "properties": { "z": 40.0 }
    }
  ]
})json";

class GdalalgVectorGridDataMetricsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    GDALRegister_MEM();
    RegisterOGRGeoJSON();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgVectorGridDataMetricsTest, ConstructNonStandaloneSubAlgorithms) {
  GDALVectorGridMinimumAlgorithm min_alg;
  GDALVectorGridMaximumAlgorithm max_alg;
  GDALVectorGridRangeAlgorithm range_alg;
  GDALVectorGridCountAlgorithm count_alg;
  GDALVectorGridAverageDistanceAlgorithm avg_dist_alg;
  GDALVectorGridAverageDistancePointsAlgorithm avg_dist_pts_alg;

  EXPECT_EQ("minimum", min_alg.GetName());
  EXPECT_EQ("maximum", max_alg.GetName());
  EXPECT_EQ("range", range_alg.GetName());
  EXPECT_EQ("count", count_alg.GetName());
  EXPECT_EQ("average-distance", avg_dist_alg.GetName());
  EXPECT_EQ("average-distance-points", avg_dist_pts_alg.GetName());
}

TEST_F(GdalalgVectorGridDataMetricsTest, PolymorphicDestructionOfAllMetrics) {
  std::vector<std::unique_ptr<GDALAlgorithm>> algs;
  algs.push_back(std::make_unique<GDALVectorGridMinimumAlgorithm>());
  algs.push_back(std::make_unique<GDALVectorGridMaximumAlgorithm>());
  algs.push_back(std::make_unique<GDALVectorGridRangeAlgorithm>());
  algs.push_back(std::make_unique<GDALVectorGridCountAlgorithm>());
  algs.push_back(std::make_unique<GDALVectorGridAverageDistanceAlgorithm>());
  algs.push_back(
      std::make_unique<GDALVectorGridAverageDistancePointsAlgorithm>());
  algs.push_back(std::make_unique<GDALVectorGridMinimumAlgorithmStandalone>());
  algs.push_back(std::make_unique<GDALVectorGridMaximumAlgorithmStandalone>());
  algs.push_back(std::make_unique<GDALVectorGridRangeAlgorithmStandalone>());
  algs.push_back(std::make_unique<GDALVectorGridCountAlgorithmStandalone>());
  algs.push_back(
      std::make_unique<GDALVectorGridAverageDistanceAlgorithmStandalone>());
  algs.push_back(std::make_unique<
                 GDALVectorGridAverageDistancePointsAlgorithmStandalone>());

  constexpr std::array<const char*, 12> kExpectedNames = {
      "minimum", "maximum",          "range",
      "count",   "average-distance", "average-distance-points",
      "minimum", "maximum",          "range",
      "count",   "average-distance", "average-distance-points",
  };
  ASSERT_EQ(kExpectedNames.size(), algs.size());
  for (size_t i = 0; i < algs.size(); ++i) {
    EXPECT_EQ(kExpectedNames[i], algs[i]->GetName());
  }
}

TEST_F(GdalalgVectorGridDataMetricsTest,
       AllMetricsWithRadiusAndEllipseOptions) {
  const std::string pts_json = std::string(kTestDir) + "/pts.geojson";
  VsiMemTempWrapper pts_wrapper(pts_json, kPointsGeoJson);

  auto run_and_read = [&](GDALAlgorithm& alg, const std::string& out_tif,
                          const std::vector<std::string>& extra_args) {
    std::vector<std::string> args = {
        "-i",        pts_json, "-o",  out_tif,    "-f", "GTiff",    "--extent",
        "0,0,10,10", "--size", "2,2", "--zfield", "z",  "--nodata", "-999"};
    args.insert(args.end(), extra_args.begin(), extra_args.end());
    ASSERT_TRUE(alg.ParseCommandLineArguments(args));
    EXPECT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());
  };

  // 1. Minimum with --radius, --min-points, --min-points-per-quadrant,
  // --max-points-per-quadrant.
  {
    const std::string out_tif = std::string(kTestDir) + "/min.tif";
    GDALVectorGridMinimumAlgorithmStandalone alg;
    ASSERT_NO_FATAL_FAILURE(run_and_read(
        alg, out_tif,
        {"--radius", "20", "--min-points", "1", "--min-points-per-quadrant",
         "0", "--max-points-per-quadrant", "10"}));
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    std::array<double, 4> vals{};
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, vals.data(),
                                             2, 2, GDT_Float64, 0, 0, nullptr));
    EXPECT_DOUBLE_EQ(10.0, vals[0]);
  }

  // 2. Maximum with --radius1, --radius2, --min-points-per-quadrant > 0
  // (per-quadrant parameters require angle == 0).
  {
    const std::string out_tif = std::string(kTestDir) + "/max.tif";
    GDALVectorGridMaximumAlgorithmStandalone alg;
    ASSERT_NO_FATAL_FAILURE(
        run_and_read(alg, out_tif,
                     {"--radius1", "20", "--radius2", "20", "--angle", "0",
                      "--min-points-per-quadrant", "1"}));
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    std::array<double, 4> vals{};
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, vals.data(),
                                             2, 2, GDT_Float64, 0, 0, nullptr));
    EXPECT_DOUBLE_EQ(40.0, vals[0]);
  }

  // 2b. Maximum with rotated ellipse (--radius1, --radius2, --angle != 0).
  {
    const std::string out_tif = std::string(kTestDir) + "/max_rot.tif";
    GDALVectorGridMaximumAlgorithmStandalone alg;
    ASSERT_NO_FATAL_FAILURE(run_and_read(
        alg, out_tif, {"--radius1", "20", "--radius2", "20", "--angle", "15"}));
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    std::array<double, 4> vals{};
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, vals.data(),
                                             2, 2, GDT_Float64, 0, 0, nullptr));
    EXPECT_DOUBLE_EQ(40.0, vals[0]);
  }

  // 3. Range.
  {
    const std::string out_tif = std::string(kTestDir) + "/range.tif";
    GDALVectorGridRangeAlgorithmStandalone alg;
    ASSERT_NO_FATAL_FAILURE(run_and_read(alg, out_tif, {"--radius", "20"}));
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    std::array<double, 4> vals{};
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, vals.data(),
                                             2, 2, GDT_Float64, 0, 0, nullptr));
    EXPECT_DOUBLE_EQ(30.0, vals[0]);
  }

  // 4. Count.
  {
    const std::string out_tif = std::string(kTestDir) + "/count.tif";
    GDALVectorGridCountAlgorithmStandalone alg;
    ASSERT_NO_FATAL_FAILURE(run_and_read(alg, out_tif, {"--radius", "20"}));
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    std::array<double, 4> vals{};
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, vals.data(),
                                             2, 2, GDT_Float64, 0, 0, nullptr));
    EXPECT_DOUBLE_EQ(6.0, vals[0]);
  }

  // 5. Average distance.
  {
    const std::string out_tif = std::string(kTestDir) + "/avg_dist.tif";
    GDALVectorGridAverageDistanceAlgorithmStandalone alg;
    ASSERT_NO_FATAL_FAILURE(run_and_read(alg, out_tif, {"--radius", "20"}));
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    std::array<double, 4> vals{};
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, vals.data(),
                                             2, 2, GDT_Float64, 0, 0, nullptr));
    EXPECT_GT(vals[0], 0.0);
  }

  // 6. Average distance between points.
  {
    const std::string out_tif = std::string(kTestDir) + "/avg_dist_pts.tif";
    GDALVectorGridAverageDistancePointsAlgorithmStandalone alg;
    ASSERT_NO_FATAL_FAILURE(run_and_read(alg, out_tif, {"--radius", "20"}));
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    std::array<double, 4> vals{};
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, vals.data(),
                                             2, 2, GDT_Float64, 0, 0, nullptr));
    EXPECT_GT(vals[0], 0.0);
  }
}

TEST_F(GdalalgVectorGridDataMetricsTest, PipelineUsage) {
  // Non-standalone grid step writes a temporary GeoTIFF via
  // CPLGenerateTempFilenameSafe; point CPL_TMPDIR to /vsimem/ since Forge
  // CWD is read-only.
  CPLSetConfigOption("CPL_TMPDIR", kTestDir);
  absl::Cleanup restore_tmpdir = [] {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
  };

  const std::string pts_json = std::string(kTestDir) + "/pipe_pts.geojson";
  const std::string out_tif = std::string(kTestDir) + "/pipe_out.tif";
  VsiMemTempWrapper pts_wrapper(pts_json, kPointsGeoJson);

  auto pipeline =
      GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("pipeline");
  ASSERT_NE(nullptr, pipeline);
  ASSERT_TRUE(pipeline->ParseCommandLineArguments(
      {"read", pts_json, "!", "grid", "minimum", "--extent", "0,0,10,10",
       "--size", "2,2", "--radius", "20", "!", "write", out_tif, "-f", "GTiff",
       "--overwrite"}));
  EXPECT_TRUE(pipeline->Run());
  ASSERT_TRUE(pipeline->Finalize());

  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(2, ds->GetRasterXSize());
}

}  // namespace
}  // namespace autotest2
