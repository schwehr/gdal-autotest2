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
// Tests for GDALContourGenerate (apps/gdal_contour_lib.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdal_contour_lib.cpp
//   https://gdal.org/programs/gdal_contour.html

#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/apps/gdal_utils.h"
#include "third_party/gdal/apps/gdal_utils_priv.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

struct DatasetDeleter {
  void operator()(GDALDatasetH ds) const {
    if (ds != nullptr) {
      GDALClose(ds);
    }
  }
};
using DatasetUniquePtr =
    std::unique_ptr<std::remove_pointer_t<GDALDatasetH>, DatasetDeleter>;

struct OptionsDeleter {
  void operator()(GDALContourOptions* opts) const {
    if (opts != nullptr) {
      GDALContourOptionsFree(opts);
    }
  }
};
using OptionsUniquePtr = std::unique_ptr<GDALContourOptions, OptionsDeleter>;

class GdalContourLibTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    GDALRegister_GTiff();
    GDALRegister_MEM();
    RegisterOGRShape();
    RegisterOGRGeoPackage();
  }
};

TEST_F(GdalContourLibTest, OptionsNewValidationFailures) {
  // Must specify at least one of -i, -fl, or -e.
  std::vector<const char*> argv_empty = {
      "-a",
      "elev",
      nullptr,
  };
  OptionsUniquePtr opts_empty(
      GDALContourOptionsNew(const_cast<char**>(argv_empty.data()), nullptr));
  EXPECT_EQ(nullptr, opts_empty);

  // Invalid argument.
  WithQuietHandler quiet_handler;
  std::vector<const char*> argv_invalid = {
      "-i",
      "10",
      "--unrecognized-flag",
      nullptr,
  };
  OptionsUniquePtr opts_invalid(
      GDALContourOptionsNew(const_cast<char**>(argv_invalid.data()), nullptr));
  EXPECT_EQ(nullptr, opts_invalid);
}

TEST_F(GdalContourLibTest, OptionsNewFlagsAndBinaryOptions) {
  std::vector<const char*> argv = {
      "-b",
      "1",
      "-a",
      "ELEV",
      "-amin",
      "ELEV_MIN",
      "-amax",
      "ELEV_MAX",
      "-3d",
      "-inodata",
      "-snodata",
      "-9999",
      "-i",
      "10.5",
      "-off",
      "2.0",
      "-nln",
      "my_contour",
      "-p",
      "-gt",
      "unlimited",
      "-q",
      "/vsimem/src.tif",
      "/vsistdout/",
      nullptr,
  };
  GDALContourOptionsForBinary bin_opts;
  OptionsUniquePtr opts(
      GDALContourOptionsNew(const_cast<char**>(argv.data()), &bin_opts));
  ASSERT_NE(nullptr, opts);
  EXPECT_TRUE(bin_opts.bQuiet);
  EXPECT_EQ("/vsimem/src.tif", bin_opts.osSrcDataSource);
  EXPECT_EQ("/vsistdout/", bin_opts.osDestDataSource);
}

TEST_F(GdalContourLibTest, OptionsNewFixedLevelsParsing) {
  // 1. Space-separated in single argument, with MIN and MAX
  {
    std::vector<const char*> argv = {
        "-fl",
        "10 20 MIN MAX",
        nullptr,
    };
    OptionsUniquePtr opts(
        GDALContourOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, opts);
  }

  // 2. Multiple arguments after -fl
  {
    std::vector<const char*> argv = {
        "-fl", "10", "20", "MIN", "MAX", nullptr,
    };
    OptionsUniquePtr opts(
        GDALContourOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, opts);
  }

  // 3. Exponential base
  {
    std::vector<const char*> argv = {
        "-e", "2.0", "-gt", "200", nullptr,
    };
    OptionsUniquePtr opts(
        GDALContourOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, opts);
  }
}

TEST_F(GdalContourLibTest, ParserUsageAndSetters) {
  const std::string usage = GDALContourGetParserUsage();
  EXPECT_FALSE(usage.empty());
  EXPECT_NE(std::string::npos, usage.find("gdal_contour"));

  std::vector<const char*> argv = {"-i", "10", nullptr};
  OptionsUniquePtr opts(
      GDALContourOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, opts);

  GDALContourOptionsSetDestDataSource(opts.get(), "/vsimem/test.shp");
  GDALContourOptionsSetProgress(
      opts.get(), [](double, const char*, void*) -> int { return 1; }, nullptr);
  GDALContourOptionsSetProgress(opts.get(), nullptr, nullptr);
}

TEST_F(GdalContourLibTest, ProcessOptionsSourceDatasetErrors) {
  WithQuietHandler quiet_handler;

  std::vector<const char*> argv = {"-i", "10", nullptr};
  OptionsUniquePtr opts(
      GDALContourOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, opts);

  GDALDatasetH hSrcDS = nullptr;
  GDALRasterBandH hBand = nullptr;
  GDALDatasetH hDstDS = nullptr;
  OGRLayerH hLayer = nullptr;
  char** papszStringOptions = nullptr;

  // Non-existing source file
  EXPECT_EQ(CE_Failure,
            GDALContourProcessOptions(opts.get(), &papszStringOptions, &hSrcDS,
                                      &hBand, &hDstDS, &hLayer));

  // Invalid band
  GDALDriverH mem_driver = GDALGetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);
  DatasetUniquePtr src(
      GDALCreate(mem_driver, "src", 5, 5, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src);

  std::vector<const char*> argv_band = {"-i", "10", "-b", "99", nullptr};
  OptionsUniquePtr opts_band(
      GDALContourOptionsNew(const_cast<char**>(argv_band.data()), nullptr));
  ASSERT_NE(nullptr, opts_band);

  hSrcDS = src.get();
  hBand = nullptr;
  EXPECT_EQ(CE_Failure,
            GDALContourProcessOptions(opts_band.get(), &papszStringOptions,
                                      &hSrcDS, &hBand, &hDstDS, &hLayer));
}

TEST_F(GdalContourLibTest, ProcessOptionsLineModeWithShapefile) {
  const std::string test_dir = "/vsimem/contour_test/line_shp";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "/dem.tif";
  const std::string dst_path = test_dir + "/contour.shp";

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  DatasetUniquePtr src_ds(GDALCreate(gtiff_driver, src_path.c_str(), 10, 10, 1,
                                     GDT_Float32, nullptr));
  ASSERT_NE(nullptr, src_ds);

  GDALRasterBandH band = GDALGetRasterBand(src_ds.get(), 1);
  GDALSetRasterNoDataValue(band, -9999.0);

  // Elevation ramp from 0 to 90
  std::vector<float> data(100);
  for (int y = 0; y < 10; ++y) {
    for (int x = 0; x < 10; ++x) {
      data[y * 10 + x] = static_cast<float>(y * 10 + x);
    }
  }
  ASSERT_EQ(CE_None, GDALRasterIO(band, GF_Write, 0, 0, 10, 10, data.data(), 10,
                                  10, GDT_Float32, 0, 0));

  std::vector<const char*> argv = {
      "-i",  "10.0", "-a",   "ELEV",    "-off",
      "5.0", "-3d",  "-nln", "contour", nullptr,
  };
  OptionsUniquePtr opts(
      GDALContourOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, opts);
  GDALContourOptionsSetDestDataSource(opts.get(), dst_path.c_str());

  GDALDatasetH hSrcDS = src_ds.get();
  GDALRasterBandH hBand = band;
  GDALDatasetH hDstDS = nullptr;
  OGRLayerH hLayer = nullptr;
  char** papszStringOptions = nullptr;

  ASSERT_EQ(CE_None,
            GDALContourProcessOptions(opts.get(), &papszStringOptions, &hSrcDS,
                                      &hBand, &hDstDS, &hLayer));
  absl::Cleanup str_opts_cleanup = [&]() { CSLDestroy(papszStringOptions); };
  DatasetUniquePtr dst_cleanup(hDstDS);

  ASSERT_NE(nullptr, hDstDS);
  ASSERT_NE(nullptr, hLayer);
  EXPECT_NE(nullptr, CSLFetchNameValue(papszStringOptions, "LEVEL_INTERVAL"));
  EXPECT_NE(nullptr, CSLFetchNameValue(papszStringOptions, "LEVEL_BASE"));
  EXPECT_NE(nullptr, CSLFetchNameValue(papszStringOptions, "ELEV_FIELD"));

  // Verify layer definition
  OGRFeatureDefnH feat_defn = OGR_L_GetLayerDefn(hLayer);
  ASSERT_NE(nullptr, feat_defn);
  EXPECT_NE(-1, OGR_FD_GetFieldIndex(feat_defn, "ID"));
  EXPECT_NE(-1, OGR_FD_GetFieldIndex(feat_defn, "ELEV"));

  // Generate contours and verify
  EXPECT_EQ(CE_None, GDALContourGenerateEx(hBand, hLayer, papszStringOptions,
                                           nullptr, nullptr));
  EXPECT_GT(OGR_L_GetFeatureCount(hLayer, TRUE), 0);
}

TEST_F(GdalContourLibTest, ProcessOptionsPolygonalModeWithGeoPackage) {
  const std::string test_dir = "/vsimem/contour_test/polygon_gpkg";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "/dem.tif";
  const std::string dst_path = test_dir + "/contour.gpkg";

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  DatasetUniquePtr src_ds(GDALCreate(gtiff_driver, src_path.c_str(), 8, 8, 1,
                                     GDT_Float32, nullptr));
  ASSERT_NE(nullptr, src_ds);

  GDALRasterBandH band = GDALGetRasterBand(src_ds.get(), 1);
  std::vector<float> data(64);
  for (int i = 0; i < 64; ++i) {
    data[i] = static_cast<float>(i * 5);
  }
  ASSERT_EQ(CE_None, GDALRasterIO(band, GF_Write, 0, 0, 8, 8, data.data(), 8, 8,
                                  GDT_Float32, 0, 0));

  WithQuietHandler quiet_handler;
  // Notice -p with -a triggers warning: -a is ignored in polygonal contouring
  // mode
  std::vector<const char*> argv = {
      "-p",       "-i",    "25.0",     "-a",  "IGNORED_ELEV", "-amin",
      "MIN_ELEV", "-amax", "MAX_ELEV", "-gt", "100",          nullptr,
  };
  OptionsUniquePtr opts(
      GDALContourOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, opts);
  GDALContourOptionsSetDestDataSource(opts.get(), dst_path.c_str());

  GDALDatasetH hSrcDS = src_ds.get();
  GDALRasterBandH hBand = band;
  GDALDatasetH hDstDS = nullptr;
  OGRLayerH hLayer = nullptr;
  char** papszStringOptions = nullptr;

  ASSERT_EQ(CE_None,
            GDALContourProcessOptions(opts.get(), &papszStringOptions, &hSrcDS,
                                      &hBand, &hDstDS, &hLayer));
  absl::Cleanup str_opts_cleanup = [&]() { CSLDestroy(papszStringOptions); };
  DatasetUniquePtr dst_cleanup(hDstDS);

  ASSERT_NE(nullptr, hDstDS);
  ASSERT_NE(nullptr, hLayer);
  EXPECT_NE(nullptr, CSLFetchNameValue(papszStringOptions, "POLYGONIZE"));
  EXPECT_NE(nullptr, CSLFetchNameValue(papszStringOptions, "ELEV_FIELD_MIN"));
  EXPECT_NE(nullptr, CSLFetchNameValue(papszStringOptions, "ELEV_FIELD_MAX"));

  // Check layer definition has MIN_ELEV and MAX_ELEV
  OGRFeatureDefnH feat_defn = OGR_L_GetLayerDefn(hLayer);
  ASSERT_NE(nullptr, feat_defn);
  EXPECT_NE(-1, OGR_FD_GetFieldIndex(feat_defn, "MIN_ELEV"));
  EXPECT_NE(-1, OGR_FD_GetFieldIndex(feat_defn, "MAX_ELEV"));
  EXPECT_EQ(-1, OGR_FD_GetFieldIndex(feat_defn, "IGNORED_ELEV"));

  EXPECT_EQ(CE_None, GDALContourGenerateEx(hBand, hLayer, papszStringOptions,
                                           nullptr, nullptr));
  EXPECT_GT(OGR_L_GetFeatureCount(hLayer, TRUE), 0);
}

TEST_F(GdalContourLibTest, LineModeWarningForAminAmax) {
  const std::string test_dir = "/vsimem/contour_test/line_warn";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH mem_driver = GDALGetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  DatasetUniquePtr src(
      GDALCreate(mem_driver, "src", 5, 5, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src);

  WithQuietHandler quiet_handler;
  // In line mode, -amin and -amax emit a warning and are cleared
  std::vector<const char*> argv = {
      "-i", "10", "-amin", "MIN_ELEV", "-amax", "MAX_ELEV", nullptr,
  };
  OptionsUniquePtr opts(
      GDALContourOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, opts);
  GDALContourOptionsSetDestDataSource(opts.get(),
                                      (test_dir + "/c.shp").c_str());

  GDALDatasetH hSrcDS = src.get();
  GDALRasterBandH hBand = GDALGetRasterBand(src.get(), 1);
  GDALDatasetH hDstDS = nullptr;
  OGRLayerH hLayer = nullptr;
  char** papszStringOptions = nullptr;

  EXPECT_EQ(CE_None,
            GDALContourProcessOptions(opts.get(), &papszStringOptions, &hSrcDS,
                                      &hBand, &hDstDS, &hLayer));
  absl::Cleanup str_opts_cleanup = [&]() { CSLDestroy(papszStringOptions); };
  DatasetUniquePtr dst_cleanup(hDstDS);
}

TEST_F(GdalContourLibTest, FixedLevelsAndExpBaseOptions) {
  const std::string test_dir = "/vsimem/contour_test/fl_exp";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH mem_driver = GDALGetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);
  DatasetUniquePtr src(
      GDALCreate(mem_driver, "src", 5, 5, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src);

  // 1. Fixed levels
  {
    std::vector<const char*> argv = {
        "-fl", "5", "10", "15", nullptr,
    };
    OptionsUniquePtr opts(
        GDALContourOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, opts);
    GDALContourOptionsSetDestDataSource(opts.get(),
                                        (test_dir + "/fl.shp").c_str());

    GDALDatasetH hSrcDS = src.get();
    GDALRasterBandH hBand = GDALGetRasterBand(src.get(), 1);
    GDALDatasetH hDstDS = nullptr;
    OGRLayerH hLayer = nullptr;
    char** papszStringOptions = nullptr;

    EXPECT_EQ(CE_None,
              GDALContourProcessOptions(opts.get(), &papszStringOptions,
                                        &hSrcDS, &hBand, &hDstDS, &hLayer));
    absl::Cleanup str_opts_cleanup = [&]() { CSLDestroy(papszStringOptions); };
    DatasetUniquePtr dst_cleanup(hDstDS);

    EXPECT_NE(nullptr, CSLFetchNameValue(papszStringOptions, "FIXED_LEVELS"));
  }

  // 2. Exponential base
  {
    std::vector<const char*> argv = {
        "-e",
        "2.0",
        nullptr,
    };
    OptionsUniquePtr opts(
        GDALContourOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, opts);
    GDALContourOptionsSetDestDataSource(opts.get(),
                                        (test_dir + "/exp.shp").c_str());

    GDALDatasetH hSrcDS = src.get();
    GDALRasterBandH hBand = GDALGetRasterBand(src.get(), 1);
    GDALDatasetH hDstDS = nullptr;
    OGRLayerH hLayer = nullptr;
    char** papszStringOptions = nullptr;

    EXPECT_EQ(CE_None,
              GDALContourProcessOptions(opts.get(), &papszStringOptions,
                                        &hSrcDS, &hBand, &hDstDS, &hLayer));
    absl::Cleanup str_opts_cleanup = [&]() { CSLDestroy(papszStringOptions); };
    DatasetUniquePtr dst_cleanup(hDstDS);

    EXPECT_NE(nullptr, CSLFetchNameValue(papszStringOptions, "LEVEL_EXP_BASE"));
  }
}

TEST_F(GdalContourLibTest, ExistingDstDatasetAndExistingLayer) {
  const std::string test_dir = "/vsimem/contour_test/existing_ds";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH mem_driver = GDALGetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  DatasetUniquePtr src(
      GDALCreate(mem_driver, "src", 5, 5, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src);

  // Destination dataset already exists in memory
  DatasetUniquePtr dst(
      GDALCreate(mem_driver, "dst", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, dst);

  // Pre-create the layer with the "ID" and "ELEV" fields already present
  OGRLayerH existing_layer = GDALDatasetCreateLayer(
      dst.get(), "contour", nullptr, wkbLineString, nullptr);
  ASSERT_NE(nullptr, existing_layer);

  OGRFieldDefnH fld_id = OGR_Fld_Create("ID", OFTInteger);
  OGR_L_CreateField(existing_layer, fld_id, FALSE);
  OGR_Fld_Destroy(fld_id);

  OGRFieldDefnH fld_elev = OGR_Fld_Create("ELEV", OFTReal);
  OGR_L_CreateField(existing_layer, fld_elev, FALSE);
  OGR_Fld_Destroy(fld_elev);

  std::vector<const char*> argv = {
      "-i", "10", "-a", "ELEV", nullptr,
  };
  OptionsUniquePtr opts(
      GDALContourOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, opts);

  GDALDatasetH hSrcDS = src.get();
  GDALRasterBandH hBand = GDALGetRasterBand(src.get(), 1);
  GDALDatasetH hDstDS = dst.get();
  OGRLayerH hLayer = existing_layer;
  char** papszStringOptions = nullptr;

  EXPECT_EQ(CE_None,
            GDALContourProcessOptions(opts.get(), &papszStringOptions, &hSrcDS,
                                      &hBand, &hDstDS, &hLayer));
  absl::Cleanup str_opts_cleanup = [&]() { CSLDestroy(papszStringOptions); };
}

TEST_F(GdalContourLibTest, DriverGuessAndCreationFailures) {
  GDALDriverH mem_driver = GDALGetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);
  DatasetUniquePtr src(
      GDALCreate(mem_driver, "src", 5, 5, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src);

  WithQuietHandler quiet_handler;

  // 1. Cannot guess driver
  {
    std::vector<const char*> argv = {"-i", "10", nullptr};
    OptionsUniquePtr opts(
        GDALContourOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, opts);
    GDALContourOptionsSetDestDataSource(opts.get(), "/vsimem/no_extension");

    GDALDatasetH hSrcDS = src.get();
    GDALRasterBandH hBand = GDALGetRasterBand(src.get(), 1);
    GDALDatasetH hDstDS = nullptr;
    OGRLayerH hLayer = nullptr;
    char** papszStringOptions = nullptr;

    EXPECT_EQ(CE_Failure,
              GDALContourProcessOptions(opts.get(), &papszStringOptions,
                                        &hSrcDS, &hBand, &hDstDS, &hLayer));
  }

  // 2. Unknown format driver
  {
    std::vector<const char*> argv = {
        "-i",
        "10",
        "-f",
        "NON_EXISTING_VECTOR_DRIVER",
        "/vsimem/in.tif",
        "/vsimem/out.shp",
        nullptr,
    };
    GDALContourOptionsForBinary bin_opts;
    OptionsUniquePtr opts(
        GDALContourOptionsNew(const_cast<char**>(argv.data()), &bin_opts));
    ASSERT_NE(nullptr, opts);
    GDALContourOptionsSetDestDataSource(opts.get(), "/vsimem/out.shp");

    GDALDatasetH hSrcDS = src.get();
    GDALRasterBandH hBand = GDALGetRasterBand(src.get(), 1);
    GDALDatasetH hDstDS = nullptr;
    OGRLayerH hLayer = nullptr;
    char** papszStringOptions = nullptr;

    EXPECT_EQ(CE_Failure,
              GDALContourProcessOptions(opts.get(), &papszStringOptions,
                                        &hSrcDS, &hBand, &hDstDS, &hLayer));
  }
}

TEST_F(GdalContourLibTest, ExistingDstDatasetWithNullLayer) {
  const std::string test_dir = "/vsimem/contour_test/null_layer";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH mem_driver = GDALGetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  DatasetUniquePtr src(
      GDALCreate(mem_driver, "src", 5, 5, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src);

  // Existing dst dataset, layer already exists (loaded by GetLayerByName)
  DatasetUniquePtr dst(
      GDALCreate(mem_driver, "dst2", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, dst);
  OGRLayerH lyr = GDALDatasetCreateLayer(dst.get(), "layer_exists", nullptr,
                                         wkbLineString, nullptr);
  ASSERT_NE(nullptr, lyr);

  std::vector<const char*> argv = {"-i", "10", "-nln", "layer_exists", nullptr};
  OptionsUniquePtr opts(
      GDALContourOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, opts);

  GDALDatasetH hSrcDS = src.get();
  GDALRasterBandH hBand = GDALGetRasterBand(src.get(), 1);
  GDALDatasetH hDstDS = dst.get();
  OGRLayerH hLayer = nullptr;
  char** papszStringOptions = nullptr;

  EXPECT_EQ(CE_None,
            GDALContourProcessOptions(opts.get(), &papszStringOptions, &hSrcDS,
                                      &hBand, &hDstDS, &hLayer));
  absl::Cleanup str_opts_cleanup = [&]() { CSLDestroy(papszStringOptions); };
  EXPECT_EQ(lyr, hLayer);
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover remaining unreachable/defensive branches in
// apps/gdal_contour_lib.cpp.
