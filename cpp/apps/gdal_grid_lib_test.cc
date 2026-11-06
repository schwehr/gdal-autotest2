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
// Tests for GDALGrid (apps/gdal_grid_lib.cpp).
//
// Caveats:
//   google3 builds GDAL without QHull (HAVE_QHULL) or libcurl, so linear
//   gridding fails with CE_Failure.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdal_grid_lib.cpp
//   https://gdal.org/programs/gdal_grid.html

#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/gdal_utils.h"
#include "third_party/gdal/apps/gdal_utils_priv.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

class GdalGridTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    GDALRegister_GTiff();
    GDALRegister_MEM();
    RegisterOGRGeoJSON();
  }

  static void TearDownTestSuite() {
    // Keep registered drivers for tests.
  }
};

std::unique_ptr<GDALDataset> CreatePointsDataset(
    const char* pszPath, bool b3D, bool bWithField,
    const std::vector<std::tuple<double, double, double>>& points) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!poDriver) return nullptr;

  auto poDS = std::unique_ptr<GDALDataset>(
      poDriver->Create(pszPath, 0, 0, 0, GDT_Unknown, nullptr));
  if (!poDS) return nullptr;

  OGRSpatialReference oSRS;
  oSRS.SetWellKnownGeogCS("WGS84");

  OGRLayer* poLayer =
      poDS->CreateLayer("points", &oSRS, b3D ? wkbPoint25D : wkbPoint, nullptr);
  if (!poLayer) return nullptr;

  if (bWithField) {
    OGRFieldDefn oField("z", OFTReal);
    CPL_IGNORE_RET_VAL(poLayer->CreateField(&oField));
  }

  for (const auto& [x, y, z] : points) {
    OGRFeature oFeat(poLayer->GetLayerDefn());
    if (std::isnan(x) && std::isnan(y)) {
      // Feature without geometry
      oFeat.SetGeometry(nullptr);
    } else {
      auto poPt = std::make_unique<OGRPoint>(x, y);
      if (b3D) {
        poPt->setZ(z);
      }
      oFeat.SetGeometry(poPt.get());
    }

    if (bWithField) {
      if (!std::isnan(z)) {
        oFeat.SetField("z", z);
      }
    }
    CPL_IGNORE_RET_VAL(poLayer->CreateFeature(&oFeat));
  }

  return poDS;
}

std::unique_ptr<GDALDataset> CreatePolygonClipDataset(const char* pszPath,
                                                      bool bMultiPolygon) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!poDriver) return nullptr;

  auto poDS = std::unique_ptr<GDALDataset>(
      poDriver->Create(pszPath, 0, 0, 0, GDT_Unknown, nullptr));
  if (!poDS) return nullptr;

  OGRSpatialReference oSRS;
  oSRS.SetWellKnownGeogCS("WGS84");

  OGRLayer* poLayer =
      poDS->CreateLayer("clip_layer", &oSRS,
                        bMultiPolygon ? wkbMultiPolygon : wkbPolygon, nullptr);
  if (!poLayer) return nullptr;

  OGRFieldDefn oField("id", OFTInteger);
  CPL_IGNORE_RET_VAL(poLayer->CreateField(&oField));

  OGRFeature oFeat(poLayer->GetLayerDefn());
  oFeat.SetField("id", 1);

  if (bMultiPolygon) {
    auto poMP = std::make_unique<OGRMultiPolygon>();
    auto poPoly = std::make_unique<OGRPolygon>();
    OGRLinearRing oRing;
    oRing.addPoint(1.0, 1.0);
    oRing.addPoint(1.0, 9.0);
    oRing.addPoint(9.0, 9.0);
    oRing.addPoint(9.0, 1.0);
    oRing.addPoint(1.0, 1.0);
    poPoly->addRing(&oRing);
    poMP->addGeometry(poPoly.get());
    oFeat.SetGeometry(poMP.get());
  } else {
    auto poPoly = std::make_unique<OGRPolygon>();
    OGRLinearRing oRing;
    oRing.addPoint(2.0, 2.0);
    oRing.addPoint(2.0, 8.0);
    oRing.addPoint(8.0, 8.0);
    oRing.addPoint(8.0, 2.0);
    oRing.addPoint(2.0, 2.0);
    poPoly->addRing(&oRing);
    oFeat.SetGeometry(poPoly.get());
  }

  CPL_IGNORE_RET_VAL(poLayer->CreateFeature(&oFeat));
  return poDS;
}

TEST_F(GdalGridTest, ParserUsage) {
  const std::string usage = GDALGridGetParserUsage();
  EXPECT_FALSE(usage.empty());
  EXPECT_THAT(usage, HasSubstr("gdal_grid"));
  EXPECT_THAT(usage, HasSubstr("algorithm"));
}

TEST_F(GdalGridTest, OptionsBasicsAndLifecycle) {
  GDALGridOptions* psOptions = GDALGridOptionsNew(nullptr, nullptr);
  ASSERT_NE(nullptr, psOptions);

  GDALGridOptionsSetProgress(psOptions, nullptr, nullptr);
  GDALGridOptionsSetProgress(psOptions, GDALDummyProgress, nullptr);
  GDALGridOptionsSetProgress(psOptions, GDALTermProgress, nullptr);

  GDALGridOptionsFree(psOptions);
}

TEST_F(GdalGridTest, OptionsParserValidArguments) {
  constexpr const char* apszArgv[] = {
      "-of",
      "GTiff",
      "-ot",
      "Float32",
      "-co",
      "TILED=YES",
      "-zfield",
      "elevation",
      "-z_increase",
      "5.0",
      "-z_multiply",
      "2.0",
      "-where",
      "z > 0",
      "-l",
      "points_layer",
      "-sql",
      "SELECT * FROM points",
      "-spat",
      "0",
      "0",
      "10",
      "10",
      "-clipsrc",
      "0",
      "0",
      "10",
      "10",
      "-clipsrcsql",
      "SELECT * FROM clip",
      "-clipsrclayer",
      "clip_layer",
      "-clipsrcwhere",
      "id = 1",
      "-a_srs",
      "EPSG:4326",
      "-a",
      "invdist:power=3.0:nodata=-9999",
      "-txe",
      "0",
      "100",
      "-tye",
      "0",
      "100",
      "-outsize",
      "50",
      "50",
      nullptr,
  };

  GDALGridOptions* psOptions =
      GDALGridOptionsNew(const_cast<char**>(apszArgv), nullptr);
  ASSERT_NE(nullptr, psOptions);
  GDALGridOptionsFree(psOptions);
}

TEST_F(GdalGridTest, OptionsParserBinaryOptions) {
  constexpr const char* apszArgv[] = {
      "-q", "-oo", "NAME=VALUE", "-of", "GTiff", "src.shp", "dst.tif", nullptr,
  };

  GDALGridOptionsForBinary sOptionsForBinary;
  GDALGridOptions* psOptions =
      GDALGridOptionsNew(const_cast<char**>(apszArgv), &sOptionsForBinary);
  ASSERT_NE(nullptr, psOptions);
  EXPECT_TRUE(sOptionsForBinary.bQuiet);
  EXPECT_EQ("src.shp", sOptionsForBinary.osSource);
  EXPECT_EQ("dst.tif", sOptionsForBinary.osDest);
  GDALGridOptionsFree(psOptions);
}

TEST_F(GdalGridTest, OptionsParserErrors) {
  WithQuietHandler quiet;

  // Unknown argument
  {
    constexpr const char* argv[] = {"-invalid_flag", nullptr};
    EXPECT_EQ(nullptr, GDALGridOptionsNew(const_cast<char**>(argv), nullptr));
  }

  // Duplicate -clipsrc
  {
    constexpr const char* argv[] = {"-clipsrc", "0",        "0",    "1",
                                    "1",        "-clipsrc", "0",    "0",
                                    "1",        "1",        nullptr};
    EXPECT_EQ(nullptr, GDALGridOptionsNew(const_cast<char**>(argv), nullptr));
  }

  // Not enough arguments for -clipsrc
  {
    constexpr const char* argv[] = {"-clipsrc", nullptr};
    EXPECT_EQ(nullptr, GDALGridOptionsNew(const_cast<char**>(argv), nullptr));
  }

  // Invalid -tr parameters
  {
    constexpr const char* argv[] = {"-txe", "0",   "10", "-tye", "0",
                                    "10",   "-tr", "0",  "1",    nullptr};
    EXPECT_EQ(nullptr, GDALGridOptionsNew(const_cast<char**>(argv), nullptr));
  }
  {
    constexpr const char* argv[] = {"-txe", "0",   "10", "-tye", "0",
                                    "10",   "-tr", "-1", "2",    nullptr};
    EXPECT_EQ(nullptr, GDALGridOptionsNew(const_cast<char**>(argv), nullptr));
  }

  // -tr without -txe and -tye
  {
    constexpr const char* argv[] = {"-tr", "1", "1", nullptr};
    EXPECT_EQ(nullptr, GDALGridOptionsNew(const_cast<char**>(argv), nullptr));
  }

  // Invalid WKT in -clipsrc
  {
    constexpr const char* argv[] = {"-clipsrc", "POLYGON((not a valid wkt))",
                                    nullptr};
    EXPECT_EQ(nullptr, GDALGridOptionsNew(const_cast<char**>(argv), nullptr));
  }

  // -clipsrc spat_extent without -spat
  {
    constexpr const char* argv[] = {"-clipsrc", "spat_extent", nullptr};
    EXPECT_EQ(nullptr, GDALGridOptionsNew(const_cast<char**>(argv), nullptr));
  }

  // -clipsrc non-existent datasource
  {
    constexpr const char* argv[] = {"-clipsrc", "/vsimem/does_not_exist.shp",
                                    nullptr};
    EXPECT_EQ(nullptr, GDALGridOptionsNew(const_cast<char**>(argv), nullptr));
  }

  // Invalid -a_srs
  {
    constexpr const char* argv[] = {
        "-a_srs", "INVALID_SRS_NAME_DEFINITELY_NOT_EXISTING", nullptr};
    EXPECT_EQ(nullptr, GDALGridOptionsNew(const_cast<char**>(argv), nullptr));
  }

  // Invalid algorithm
  {
    constexpr const char* argv[] = {"-a", "invalid_algorithm_name", nullptr};
    EXPECT_EQ(nullptr, GDALGridOptionsNew(const_cast<char**>(argv), nullptr));
  }
}

TEST_F(GdalGridTest, OptionsClipSrcCombinations) {
  // -clipsrc with 4 coordinates
  {
    constexpr const char* argv[] = {"-clipsrc", "1", "2", "3", "4", nullptr};
    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);
    GDALGridOptionsFree(psOpt);
  }

  // -clipsrc with valid WKT polygon
  {
    constexpr const char* argv[] = {
        "-clipsrc", "POLYGON((0 0, 0 10, 10 10, 10 0, 0 0))", nullptr};
    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);
    GDALGridOptionsFree(psOpt);
  }

  // -clipsrc spat_extent with -spat
  {
    constexpr const char* argv[] = {
        "-spat", "0", "0", "10", "10", "-clipsrc", "spat_extent", nullptr};
    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);
    GDALGridOptionsFree(psOpt);
  }

  // -spat and -clipsrc bounding box intersection
  {
    constexpr const char* argv[] = {"-spat", "0",        "0",    "10",
                                    "10",    "-clipsrc", "5",    "5",
                                    "15",    "15",       nullptr};
    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);
    GDALGridOptionsFree(psOpt);
  }
}

TEST_F(GdalGridTest, UsageErrorsAtExecution) {
  constexpr char kTestDir[] = "/vsimem/gdal_grid_test/usage_errors";
  absl::Cleanup cleanup_dir = [kTestDir] { VSIRmdirRecursive(kTestDir); };

  const std::string dst_tif = std::string(kTestDir) + "/out.tif";

  const std::vector<std::tuple<double, double, double>> points = {
      {1.0, 1.0, 10.0}, {5.0, 5.0, 20.0}, {9.0, 9.0, 30.0}};
  auto poDS = CreatePointsDataset("points_mem", /*b3D=*/true,
                                  /*bWithField=*/false, points);
  ASSERT_NE(nullptr, poDS);

  WithQuietHandler quiet;
  int bUsageError = 0;

  // Null source dataset
  bUsageError = 0;
  EXPECT_EQ(nullptr, GDALGrid(dst_tif.c_str(), nullptr, nullptr, &bUsageError));
  EXPECT_EQ(1, bUsageError);

  // Null target dataset path
  bUsageError = 0;
  EXPECT_EQ(nullptr, GDALGrid(nullptr, GDALDataset::ToHandle(poDS.get()),
                              nullptr, &bUsageError));
  EXPECT_EQ(1, bUsageError);

  // Multi-layer dataset without -l or -sql
  {
    auto poMultiDS = std::unique_ptr<GDALDataset>(
        GetGDALDriverManager()->GetDriverByName("MEM")->Create(
            "multi_layer", 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, poMultiDS);
    poMultiDS->CreateLayer("lyr1", nullptr, wkbPoint);
    poMultiDS->CreateLayer("lyr2", nullptr, wkbPoint);

    bUsageError = 0;
    EXPECT_EQ(nullptr,
              GDALGrid(dst_tif.c_str(), GDALDataset::ToHandle(poMultiDS.get()),
                       nullptr, &bUsageError));
    EXPECT_EQ(1, bUsageError);
  }

  // Conflicting -outsize and -tr
  {
    constexpr const char* argv[] = {"-txe", "0",   "10",   "-tye", "0",
                                    "10",   "-tr", "1",    "1",    "-outsize",
                                    "10",   "10",  nullptr};
    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    EXPECT_EQ(nullptr,
              GDALGrid(dst_tif.c_str(), GDALDataset::ToHandle(poDS.get()),
                       psOpt, nullptr));
    GDALGridOptionsFree(psOpt);
  }

  // Unrecognised output driver
  {
    constexpr const char* argv[] = {"-of", "NON_EXISTENT_FORMAT_DRIVER",
                                    nullptr};
    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    EXPECT_EQ(nullptr,
              GDALGrid(dst_tif.c_str(), GDALDataset::ToHandle(poDS.get()),
                       psOpt, nullptr));
    GDALGridOptionsFree(psOpt);
  }

  // Unrecognized raster driver when destination path has vector extension
  // (.geojson)
  {
    const std::string dst_vector_ext = std::string(kTestDir) + "/out.geojson";
    EXPECT_EQ(nullptr,
              GDALGrid(dst_vector_ext.c_str(),
                       GDALDataset::ToHandle(poDS.get()), nullptr, nullptr));
  }

  // Invalid output size from -tr (e.g. resolution larger than extent => size <
  // 1)
  {
    bUsageError = 0;
    constexpr const char* argv[] = {"-txe", "0",   "1",  "-tye", "0",
                                    "1",    "-tr", "10", "10",   nullptr};
    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    EXPECT_EQ(nullptr,
              GDALGrid(dst_tif.c_str(), GDALDataset::ToHandle(poDS.get()),
                       psOpt, &bUsageError));
    EXPECT_EQ(1, bUsageError);
    GDALGridOptionsFree(psOpt);
  }

  // Failure to create output dataset on unwriteable path (not a directory)
  {
    constexpr char kNotADir[] = "/vsimem/gdal_grid_test/not_a_dir";
    VsiMemTempWrapper not_a_dir(kNotADir, "x");
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr,
              GDALGrid("/vsimem/gdal_grid_test/not_a_dir/out.tif",
                       GDALDataset::ToHandle(poDS.get()), nullptr, nullptr));
  }
}

TEST_F(GdalGridTest, AllAlgorithmsWithQuietFalse) {
  constexpr char kTestDir[] = "/vsimem/gdal_grid_test/algorithms";
  absl::Cleanup cleanup_dir = [kTestDir] { VSIRmdirRecursive(kTestDir); };

  const std::vector<std::tuple<double, double, double>> points = {
      {0.0, 0.0, 10.0},   {10.0, 0.0, 20.0}, {0.0, 10.0, 30.0},
      {10.0, 10.0, 40.0}, {5.0, 5.0, 25.0},  {2.0, 7.0, 15.0},
  };
  auto poDS = CreatePointsDataset("points_alg", /*b3D=*/true,
                                  /*bWithField=*/false, points);
  ASSERT_NE(nullptr, poDS);

  struct AlgTestCase {
    const char* pszName;
    const char* pszAlgSpec;
  };

  constexpr AlgTestCase kCases[] = {
      {.pszName = "invdist_default", .pszAlgSpec = "invdist"},
      {.pszName = "invdist_full",
       .pszAlgSpec =
           "invdist:power=2.5:smoothing=0.5:radius1=8:radius2=8:angle=30:max_"
           "points=8:min_points=1:nodata=-9999"},
      {.pszName = "invdistnn",
       .pszAlgSpec =
           "invdistnn:power=2.0:smoothing=0.2:radius=8:max_points=8:min_points="
           "1:min_points_per_quadrant=1:max_points_per_quadrant=3:nodata=-"
           "9999"},
      {.pszName = "average",
       .pszAlgSpec =
           "average:radius1=8:radius2=8:angle=0:min_points=1:max_points=8:min_"
           "points_per_quadrant=1:max_points_per_quadrant=3:nodata=-9999"},
      {.pszName = "nearest",
       .pszAlgSpec = "nearest:radius1=8:radius2=8:angle=0:nodata=-9999"},
      {.pszName = "linear", .pszAlgSpec = "linear:radius=15:nodata=-9999"},
      {.pszName = "minimum",
       .pszAlgSpec =
           "minimum:radius1=8:radius2=8:angle=0:min_points=1:min_points_per_"
           "quadrant=1:max_points_per_quadrant=3:nodata=-9999"},
      {.pszName = "maximum",
       .pszAlgSpec =
           "maximum:radius1=8:radius2=8:angle=0:min_points=1:nodata=-9999"},
      {.pszName = "range",
       .pszAlgSpec =
           "range:radius1=8:radius2=8:angle=0:min_points=1:nodata=-9999"},
      {.pszName = "count",
       .pszAlgSpec =
           "count:radius1=8:radius2=8:angle=0:min_points=1:nodata=-9999"},
      {.pszName = "average_distance",
       .pszAlgSpec =
           "average_distance:radius1=8:radius2=8:angle=0:min_points=1:nodata=-"
           "9999"},
      {.pszName = "average_distance_pts",
       .pszAlgSpec =
           "average_distance_pts:radius1=8:radius2=8:angle=0:min_points=1:"
           "nodata=-9999"},
  };

  for (const auto& tc : kCases) {
    const std::string dst_tif =
        std::string(kTestDir) + "/" + tc.pszName + ".tif";

    const char* argv[] = {
        "-of", "GTiff",    "-txe", "-1", "11", "-tye",        "-1",
        "11",  "-outsize", "10",   "10", "-a", tc.pszAlgSpec, nullptr,
    };

    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    // Calling GDALGridOptionsSetProgress with GDALTermProgress sets bQuiet =
    // false, which triggers PrintAlgorithmAndOptions and verbose printing.
    GDALGridOptionsSetProgress(psOpt, GDALTermProgress, nullptr);

    GDALDatasetH hOutDS = nullptr;
    if (std::string(tc.pszName) == "linear") {
      // Linear gridding requires QHull which is disabled in this build.
      // PrintAlgorithmAndOptions is still exercised before context creation
      // fails.
      WithQuietHandler quiet;
      hOutDS = GDALGrid(dst_tif.c_str(), GDALDataset::ToHandle(poDS.get()),
                        psOpt, nullptr);
      EXPECT_EQ(nullptr, hOutDS);
      GDALGridOptionsFree(psOpt);
      continue;
    }

    hOutDS = GDALGrid(dst_tif.c_str(), GDALDataset::ToHandle(poDS.get()), psOpt,
                      nullptr);
    GDALGridOptionsFree(psOpt);

    ASSERT_NE(nullptr, hOutDS);
    auto poOutDS =
        std::unique_ptr<GDALDataset>(GDALDataset::FromHandle(hOutDS));

    EXPECT_EQ(10, poOutDS->GetRasterXSize());
    EXPECT_EQ(10, poOutDS->GetRasterYSize());
    EXPECT_EQ(1, poOutDS->GetRasterCount());

    GDALRasterBand* poBand = poOutDS->GetRasterBand(1);
    ASSERT_NE(nullptr, poBand);

    int bHasNoData = 0;
    const double dfNoData = poBand->GetNoDataValue(&bHasNoData);
    if (std::string(tc.pszAlgSpec).find("nodata=-9999") != std::string::npos) {
      EXPECT_TRUE(bHasNoData);
      EXPECT_DOUBLE_EQ(-9999.0, dfNoData);
    }

    std::vector<double> scanline(10);
    EXPECT_EQ(CE_None, poBand->RasterIO(GF_Read, 0, 5, 10, 1, scanline.data(),
                                        10, 1, GDT_Float64, 0, 0, nullptr));
  }
}

TEST_F(GdalGridTest, BurnAttributeAndModifiers) {
  constexpr char kTestDir[] = "/vsimem/gdal_grid_test/burn_attribute";
  absl::Cleanup cleanup_dir = [kTestDir] { VSIRmdirRecursive(kTestDir); };

  // Points with 2D coordinates and attribute "z", including:
  // - valid point
  // - point with NaN coordinates (no geometry)
  // - point with NaN Z value
  const std::vector<std::tuple<double, double, double>> points = {
      {1.0, 1.0, 10.0},
      {9.0, 9.0, 40.0},
      {5.0, 5.0, std::numeric_limits<double>::quiet_NaN()},
      {std::numeric_limits<double>::quiet_NaN(),
       std::numeric_limits<double>::quiet_NaN(), 20.0},
  };

  auto poDS = CreatePointsDataset("points_burn", /*b3D=*/false,
                                  /*bWithField=*/true, points);
  ASSERT_NE(nullptr, poDS);

  // Test successful burn with -zfield, -z_increase and -z_multiply
  {
    const std::string dst_tif = std::string(kTestDir) + "/burn_modified.tif";
    constexpr const char* argv[] = {
        "-of",         "GTiff", "-zfield",  "z",  "-z_increase", "5.0",
        "-z_multiply", "2.0",   "-outsize", "10", "10",          "-txe",
        "0",           "10",    "-tye",     "0",  "10",          nullptr,
    };

    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hOutDS = GDALGrid(
        dst_tif.c_str(), GDALDataset::ToHandle(poDS.get()), psOpt, nullptr);
    GDALGridOptionsFree(psOpt);

    ASSERT_NE(nullptr, hOutDS);
    GDALClose(hOutDS);
  }

  // Test 3D points with NaN Z when iBurnField < 0
  {
    const std::vector<std::tuple<double, double, double>> nan_z_points = {
        {1.0, 1.0, std::numeric_limits<double>::quiet_NaN()},
        {5.0, 5.0, 20.0},
    };
    auto po3DDS = CreatePointsDataset("points_nan_z", /*b3D=*/true,
                                      /*bWithField=*/false, nan_z_points);
    ASSERT_NE(nullptr, po3DDS);

    const std::string dst_tif = std::string(kTestDir) + "/3d_nan.tif";
    constexpr const char* argv[] = {
        "-of", "GTiff", "-outsize", "10", "10", "-txe",
        "0",   "10",    "-tye",     "0",  "10", nullptr,
    };

    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hOutDS = GDALGrid(
        dst_tif.c_str(), GDALDataset::ToHandle(po3DDS.get()), psOpt, nullptr);
    GDALGridOptionsFree(psOpt);

    ASSERT_NE(nullptr, hOutDS);
    GDALClose(hOutDS);
  }

  // Non-existent zfield error
  {
    WithQuietHandler quiet;
    const std::string dst_tif = std::string(kTestDir) + "/fail_field.tif";
    constexpr const char* argv[] = {
        "-of", "GTiff", "-zfield", "non_existent_field", nullptr,
    };
    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    EXPECT_EQ(nullptr,
              GDALGrid(dst_tif.c_str(), GDALDataset::ToHandle(poDS.get()),
                       psOpt, nullptr));
    GDALGridOptionsFree(psOpt);
  }
}

TEST_F(GdalGridTest, LayerSelectionFilteringAndSQL) {
  constexpr char kTestDir[] = "/vsimem/gdal_grid_test/layer_sql";
  absl::Cleanup cleanup_dir = [kTestDir] { VSIRmdirRecursive(kTestDir); };

  // Create a multi-layer dataset
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, poDriver);

  auto poMultiDS = std::unique_ptr<GDALDataset>(
      poDriver->Create("multi_layer_test", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, poMultiDS);

  OGRSpatialReference oSRS;
  oSRS.SetWellKnownGeogCS("WGS84");

  OGRLayer* poLyr1 = poMultiDS->CreateLayer("layer1", &oSRS, wkbPoint25D);
  ASSERT_NE(nullptr, poLyr1);
  OGRFieldDefn oFieldZ("z", OFTReal);
  CPL_IGNORE_RET_VAL(poLyr1->CreateField(&oFieldZ));

  {
    OGRFeature oFeat(poLyr1->GetLayerDefn());
    auto poPt = std::make_unique<OGRPoint>(2.0, 2.0, 10.0);
    oFeat.SetGeometry(poPt.get());
    oFeat.SetField("z", 10.0);
    CPL_IGNORE_RET_VAL(poLyr1->CreateFeature(&oFeat));
  }
  {
    OGRFeature oFeat(poLyr1->GetLayerDefn());
    auto poPt = std::make_unique<OGRPoint>(8.0, 8.0, 30.0);
    oFeat.SetGeometry(poPt.get());
    oFeat.SetField("z", 30.0);
    CPL_IGNORE_RET_VAL(poLyr1->CreateFeature(&oFeat));
  }

  OGRLayer* poLyr2 = poMultiDS->CreateLayer("layer2", &oSRS, wkbPoint25D);
  ASSERT_NE(nullptr, poLyr2);
  CPL_IGNORE_RET_VAL(poLyr2->CreateField(&oFieldZ));
  {
    OGRFeature oFeat(poLyr2->GetLayerDefn());
    auto poPt = std::make_unique<OGRPoint>(5.0, 5.0, 20.0);
    oFeat.SetGeometry(poPt.get());
    oFeat.SetField("z", 20.0);
    CPL_IGNORE_RET_VAL(poLyr2->CreateFeature(&oFeat));
  }

  // Gridding with multiple -l arguments produces multi-band output
  {
    const std::string dst_tif = std::string(kTestDir) + "/two_layers.tif";
    constexpr const char* argv[] = {
        "-of", "GTiff", "-l", "layer1", "-l",   "layer2", "-outsize", "10",
        "10",  "-txe",  "0",  "10",     "-tye", "0",      "10",       nullptr,
    };

    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hOutDS =
        GDALGrid(dst_tif.c_str(), GDALDataset::ToHandle(poMultiDS.get()), psOpt,
                 nullptr);
    GDALGridOptionsFree(psOpt);

    ASSERT_NE(nullptr, hOutDS);
    auto poOutDS =
        std::unique_ptr<GDALDataset>(GDALDataset::FromHandle(hOutDS));
    EXPECT_EQ(2, poOutDS->GetRasterCount());
  }

  // Gridding with -sql statement
  {
    const std::string dst_tif = std::string(kTestDir) + "/sql.tif";
    constexpr const char* argv[] = {
        "-of",      "GTiff", "-sql", "SELECT * FROM layer1 WHERE z > 15",
        "-outsize", "10",    "10",   "-txe",
        "0",        "10",    "-tye", "0",
        "10",       nullptr,
    };

    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hOutDS =
        GDALGrid(dst_tif.c_str(), GDALDataset::ToHandle(poMultiDS.get()), psOpt,
                 nullptr);
    GDALGridOptionsFree(psOpt);

    ASSERT_NE(nullptr, hOutDS);
    GDALClose(hOutDS);
  }

  // Gridding with both -sql and -l
  {
    const std::string dst_tif = std::string(kTestDir) + "/sql_and_l.tif";
    constexpr const char* argv[] = {
        "-of",  "GTiff",  "-sql",     "SELECT * FROM layer1",
        "-l",   "layer2", "-outsize", "10",
        "10",   "-txe",   "0",        "10",
        "-tye", "0",      "10",       nullptr,
    };

    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hOutDS =
        GDALGrid(dst_tif.c_str(), GDALDataset::ToHandle(poMultiDS.get()), psOpt,
                 nullptr);
    GDALGridOptionsFree(psOpt);

    ASSERT_NE(nullptr, hOutDS);
    auto poOutDS =
        std::unique_ptr<GDALDataset>(GDALDataset::FromHandle(hOutDS));
    EXPECT_EQ(2, poOutDS->GetRasterCount());
  }

  // -where attribute filtering
  {
    const std::string dst_tif = std::string(kTestDir) + "/where.tif";
    constexpr const char* argv[] = {
        "-of", "GTiff", "-l", "layer1", "-where", "z < 25", "-outsize", "10",
        "10",  "-txe",  "0",  "10",     "-tye",   "0",      "10",       nullptr,
    };

    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hOutDS =
        GDALGrid(dst_tif.c_str(), GDALDataset::ToHandle(poMultiDS.get()), psOpt,
                 nullptr);
    GDALGridOptionsFree(psOpt);

    ASSERT_NE(nullptr, hOutDS);
    GDALClose(hOutDS);
  }

  // Error cases: invalid layer name
  {
    WithQuietHandler quiet;
    const std::string dst_tif = std::string(kTestDir) + "/invalid_lyr.tif";
    constexpr const char* argv[] = {
        "-of", "GTiff", "-l", "non_existent_layer", nullptr,
    };
    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    EXPECT_EQ(nullptr,
              GDALGrid(dst_tif.c_str(), GDALDataset::ToHandle(poMultiDS.get()),
                       psOpt, nullptr));
    GDALGridOptionsFree(psOpt);
  }

  // Error cases: invalid WHERE clause
  {
    WithQuietHandler quiet;
    const std::string dst_tif = std::string(kTestDir) + "/invalid_where.tif";
    constexpr const char* argv[] = {
        "-of",    "GTiff",  "-l",
        "layer1", "-where", "SYNTAX ERROR IN WHERE CLAUSE",
        nullptr,
    };
    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    EXPECT_EQ(nullptr,
              GDALGrid(dst_tif.c_str(), GDALDataset::ToHandle(poMultiDS.get()),
                       psOpt, nullptr));
    GDALGridOptionsFree(psOpt);
  }

  // Error cases: invalid SQL statement
  {
    WithQuietHandler quiet;
    const std::string dst_tif = std::string(kTestDir) + "/invalid_sql.tif";
    constexpr const char* argv[] = {
        "-of", "GTiff", "-sql", "SELECT * FROM non_existent_table", nullptr,
    };
    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    EXPECT_EQ(nullptr,
              GDALGrid(dst_tif.c_str(), GDALDataset::ToHandle(poMultiDS.get()),
                       psOpt, nullptr));
    GDALGridOptionsFree(psOpt);
  }

  // Layer with no point geometries
  {
    OGRLayer* poEmptyLyr =
        poMultiDS->CreateLayer("empty_lyr", &oSRS, wkbPolygon);
    ASSERT_NE(nullptr, poEmptyLyr);

    const std::string dst_tif = std::string(kTestDir) + "/empty_lyr.tif";
    constexpr const char* argv[] = {
        "-of", "GTiff", "-l", "empty_lyr", nullptr,
    };
    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    // ProcessLayer returns CE_None when no points found, but output dataset is
    // created.
    GDALDatasetH hOutDS =
        GDALGrid(dst_tif.c_str(), GDALDataset::ToHandle(poMultiDS.get()), psOpt,
                 nullptr);
    GDALGridOptionsFree(psOpt);

    ASSERT_NE(nullptr, hOutDS);
    GDALClose(hOutDS);
  }
}

TEST_F(GdalGridTest, ExtentsResolutionAndAutomaticSizing) {
  constexpr char kTestDir[] = "/vsimem/gdal_grid_test/extents_resolution";
  absl::Cleanup cleanup_dir = [kTestDir] { VSIRmdirRecursive(kTestDir); };

  const std::vector<std::tuple<double, double, double>> points = {
      {10.0, 20.0, 100.0},
      {30.0, 40.0, 200.0},
      {20.0, 30.0, 150.0},
  };
  auto poDS = CreatePointsDataset("points_extent", /*b3D=*/true,
                                  /*bWithField=*/false, points);
  ASSERT_NE(nullptr, poDS);

  // Automatic extent computation and default 256x256 size
  {
    const std::string dst_tif = std::string(kTestDir) + "/auto_extent.tif";
    constexpr const char* argv[] = {"-of", "GTiff", nullptr};

    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hOutDS = GDALGrid(
        dst_tif.c_str(), GDALDataset::ToHandle(poDS.get()), psOpt, nullptr);
    GDALGridOptionsFree(psOpt);

    ASSERT_NE(nullptr, hOutDS);
    auto poOutDS =
        std::unique_ptr<GDALDataset>(GDALDataset::FromHandle(hOutDS));
    EXPECT_EQ(256, poOutDS->GetRasterXSize());
    EXPECT_EQ(256, poOutDS->GetRasterYSize());

    double adfGeoTransform[6];
    EXPECT_EQ(CE_None, poOutDS->GetGeoTransform(adfGeoTransform));
    EXPECT_DOUBLE_EQ(10.0, adfGeoTransform[0]);
    // Swapped to produce north-up images (dfYMin and dfYMax swap)
    EXPECT_DOUBLE_EQ(40.0, adfGeoTransform[3]);
    EXPECT_LT(adfGeoTransform[5], 0.0);
  }

  // Target resolution with -tr
  {
    const std::string dst_tif = std::string(kTestDir) + "/res_tr.tif";
    constexpr const char* argv[] = {
        "-of", "GTiff", "-txe", "10",  "30",  "-tye",
        "20",  "40",    "-tr",  "2.0", "2.0", nullptr,
    };

    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hOutDS = GDALGrid(
        dst_tif.c_str(), GDALDataset::ToHandle(poDS.get()), psOpt, nullptr);
    GDALGridOptionsFree(psOpt);

    ASSERT_NE(nullptr, hOutDS);
    auto poOutDS =
        std::unique_ptr<GDALDataset>(GDALDataset::FromHandle(hOutDS));
    EXPECT_EQ(10, poOutDS->GetRasterXSize());
    EXPECT_EQ(10, poOutDS->GetRasterYSize());
  }

  // Explicit SRS with -a_srs
  {
    const std::string dst_tif = std::string(kTestDir) + "/custom_srs.tif";
    constexpr const char* argv[] = {
        "-of",  "GTiff", "-a_srs", "EPSG:3857", "-outsize", "10",   "10",
        "-txe", "0",     "1000",   "-tye",      "0",        "1000", nullptr,
    };

    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hOutDS = GDALGrid(
        dst_tif.c_str(), GDALDataset::ToHandle(poDS.get()), psOpt, nullptr);
    GDALGridOptionsFree(psOpt);

    ASSERT_NE(nullptr, hOutDS);
    auto poOutDS =
        std::unique_ptr<GDALDataset>(GDALDataset::FromHandle(hOutDS));
    const char* pszProj = poOutDS->GetProjectionRef();
    ASSERT_NE(nullptr, pszProj);
    EXPECT_THAT(pszProj, HasSubstr("3857"));
  }
}

TEST_F(GdalGridTest, SpatialFilterAndClipSrcDatasource) {
  constexpr char kTestDir[] = "/vsimem/gdal_grid_test/spatial_clip";
  absl::Cleanup cleanup_dir = [kTestDir] { VSIRmdirRecursive(kTestDir); };

  const std::vector<std::tuple<double, double, double>> points = {
      {1.0, 1.0, 10.0},
      {3.0, 3.0, 20.0},
      {7.0, 7.0, 30.0},
      {9.0, 9.0, 40.0},
  };
  auto poDS = CreatePointsDataset("points_spat", /*b3D=*/true,
                                  /*bWithField=*/false, points);
  ASSERT_NE(nullptr, poDS);

  // Create clip polygon dataset
  const std::string clip_poly_ds = std::string(kTestDir) + "/clip_poly.json";
  {
    auto poClipDS = CreatePolygonClipDataset(clip_poly_ds.c_str(),
                                             /*bMultiPolygon=*/false);
    ASSERT_NE(nullptr, poClipDS);
    auto poJsonDriver = GetGDALDriverManager()->GetDriverByName("GeoJSON");
    ASSERT_NE(nullptr, poJsonDriver);
    auto poExport = std::unique_ptr<GDALDataset>(
        poJsonDriver->CreateCopy(clip_poly_ds.c_str(), poClipDS.get(), FALSE,
                                 nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, poExport);
  }

  // -clipsrc with datasource file and default layer (no -clipsrclayer or
  // -clipsrcsql)
  {
    const std::string dst_tif = std::string(kTestDir) + "/clip_default_lyr.tif";
    const char* argv[] = {
        "-of",      "GTiff", "-clipsrc", clip_poly_ds.c_str(),
        "-outsize", "10",    "10",       "-txe",
        "0",        "10",    "-tye",     "0",
        "10",       nullptr,
    };

    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hOutDS = GDALGrid(
        dst_tif.c_str(), GDALDataset::ToHandle(poDS.get()), psOpt, nullptr);
    GDALGridOptionsFree(psOpt);

    ASSERT_NE(nullptr, hOutDS);
    GDALClose(hOutDS);
  }

  // -clipsrc with datasource file and -clipsrclayer, -clipsrcwhere
  {
    const std::string dst_tif = std::string(kTestDir) + "/clip_ds.tif";
    const char* argv[] = {
        "-of",
        "GTiff",
        "-clipsrc",
        clip_poly_ds.c_str(),
        "-clipsrclayer",
        "clip_layer",
        "-clipsrcwhere",
        "id = 1",
        "-outsize",
        "10",
        "10",
        "-txe",
        "0",
        "10",
        "-tye",
        "0",
        "10",
        nullptr,
    };

    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hOutDS = GDALGrid(
        dst_tif.c_str(), GDALDataset::ToHandle(poDS.get()), psOpt, nullptr);
    GDALGridOptionsFree(psOpt);

    ASSERT_NE(nullptr, hOutDS);
    GDALClose(hOutDS);
  }

  // Create multipolygon clip dataset
  const std::string clip_mp_ds = std::string(kTestDir) + "/clip_mp.json";
  {
    auto poClipDS = CreatePolygonClipDataset(clip_mp_ds.c_str(),
                                             /*bMultiPolygon=*/true);
    ASSERT_NE(nullptr, poClipDS);
    auto poJsonDriver = GetGDALDriverManager()->GetDriverByName("GeoJSON");
    ASSERT_NE(nullptr, poJsonDriver);
    auto poExport = std::unique_ptr<GDALDataset>(poJsonDriver->CreateCopy(
        clip_mp_ds.c_str(), poClipDS.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, poExport);
  }

  // -clipsrc with multipolygon datasource and -clipsrcsql
  {
    const std::string dst_tif = std::string(kTestDir) + "/clip_mp.tif";
    const char* argv[] = {
        "-of",         "GTiff",
        "-clipsrc",    clip_mp_ds.c_str(),
        "-clipsrcsql", "SELECT * FROM clip_layer",
        "-outsize",    "10",
        "10",          "-txe",
        "0",           "10",
        "-tye",        "0",
        "10",          nullptr,
    };

    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hOutDS = GDALGrid(
        dst_tif.c_str(), GDALDataset::ToHandle(poDS.get()), psOpt, nullptr);
    GDALGridOptionsFree(psOpt);

    ASSERT_NE(nullptr, hOutDS);
    GDALClose(hOutDS);
  }

  // Error case: -clipsrc datasource with non-polygon geometry (points) and SQL
  {
    WithQuietHandler quiet;
    const std::string clip_points_ds = std::string(kTestDir) + "/clip_pts.json";
    auto poJsonDriver = GetGDALDriverManager()->GetDriverByName("GeoJSON");
    ASSERT_NE(nullptr, poJsonDriver);
    auto poExport = std::unique_ptr<GDALDataset>(poJsonDriver->CreateCopy(
        clip_points_ds.c_str(), poDS.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, poExport);

    const char* argv[] = {
        "-clipsrc",    clip_points_ds.c_str(),
        "-clipsrcsql", "SELECT * FROM points_spat",
        nullptr,
    };
    EXPECT_EQ(nullptr, GDALGridOptionsNew(const_cast<char**>(argv), nullptr));
  }

  // Error case: -clipsrc datasource with invalid layer name
  {
    WithQuietHandler quiet;
    const char* argv[] = {
        "-clipsrc",      clip_poly_ds.c_str(),
        "-clipsrclayer", "non_existent_clip_layer",
        nullptr,
    };
    EXPECT_EQ(nullptr, GDALGridOptionsNew(const_cast<char**>(argv), nullptr));
  }
}

TEST_F(GdalGridTest, BufferResizingAndProgressCancellation) {
  constexpr char kTestDir[] = "/vsimem/gdal_grid_test/resizing_progress";
  absl::Cleanup cleanup_dir = [kTestDir] { VSIRmdirRecursive(kTestDir); };

  const std::vector<std::tuple<double, double, double>> points = {
      {0.0, 0.0, 10.0},     {100.0, 0.0, 20.0}, {0.0, 100.0, 30.0},
      {100.0, 100.0, 40.0}, {50.0, 50.0, 25.0},
  };
  auto poDS = CreatePointsDataset("points_buf", /*b3D=*/true,
                                  /*bWithField=*/false, points);
  ASSERT_NE(nullptr, poDS);

  // Test buffer resizing branch: small block size with larger grid size
  {
    const std::string dst_tif = std::string(kTestDir) + "/tiled_resize.tif";
    constexpr const char* argv[] = {
        "-of", "GTiff",         "-co",      "TILED=YES", "-co", "BLOCKXSIZE=16",
        "-co", "BLOCKYSIZE=16", "-outsize", "250",       "250", "-txe",
        "0",   "100",           "-tye",     "0",         "100", nullptr,
    };

    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hOutDS = GDALGrid(
        dst_tif.c_str(), GDALDataset::ToHandle(poDS.get()), psOpt, nullptr);
    GDALGridOptionsFree(psOpt);

    ASSERT_NE(nullptr, hOutDS);
    GDALClose(hOutDS);
  }

  // Progress cancellation: callback returns FALSE
  {
    // Force single-threaded execution to avoid upstream GDAL data race in
    // GDALGridContextProcess where bStop is read/written without a lock during
    // multi-threaded cancellation.
    const CPLConfigOptionSetter num_threads("GDAL_NUM_THREADS", "1", false);
    const std::string dst_tif = std::string(kTestDir) + "/cancelled.tif";
    constexpr const char* argv[] = {
        "-of", "GTiff", "-outsize", "64", "64",  "-txe",
        "0",   "100",   "-tye",     "0",  "100", nullptr,
    };

    GDALGridOptions* psOpt =
        GDALGridOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    auto CancelProgress = [](double /*dfComplete*/, const char* /*pszMsg*/,
                             void* /*pProgressArg*/) -> int {
      return FALSE;  // Abort
    };
    GDALGridOptionsSetProgress(psOpt, CancelProgress, nullptr);

    WithQuietHandler quiet;
    GDALDatasetH hOutDS = GDALGrid(
        dst_tif.c_str(), GDALDataset::ToHandle(poDS.get()), psOpt, nullptr);
    GDALGridOptionsFree(psOpt);

    EXPECT_EQ(nullptr, hOutDS);
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover remaining unreachable/defensive branches in
// apps/gdal_grid_lib.cpp.
