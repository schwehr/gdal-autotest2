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
// Tests for GDALTileIndex (apps/gdaltindex_lib.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdaltindex_lib.cpp
//   https://gdal.org/programs/gdaltindex.html

#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/gdal_utils.h"
#include "third_party/gdal/apps/gdal_utils_priv.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_progress.h"
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
  void operator()(GDALTileIndexOptions* opts) const {
    if (opts != nullptr) {
      GDALTileIndexOptionsFree(opts);
    }
  }
};
using OptionsUniquePtr = std::unique_ptr<GDALTileIndexOptions, OptionsDeleter>;

struct FeatureDeleter {
  void operator()(OGRFeatureH feat) const {
    if (feat != nullptr) {
      OGR_F_Destroy(feat);
    }
  }
};
using FeatureUniquePtr =
    std::unique_ptr<std::remove_pointer_t<OGRFeatureH>, FeatureDeleter>;

class GdalTileIndexLibTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    GDALRegister_GTiff();
    GDALRegister_MEM();
    RegisterOGRShape();
    RegisterOGRGeoPackage();
  }
};

TEST_F(GdalTileIndexLibTest, OptionsNewDefault) {
  OptionsUniquePtr opts(GDALTileIndexOptionsNew(nullptr, nullptr));
  ASSERT_NE(nullptr, opts);
}

TEST_F(GdalTileIndexLibTest, OptionsNewFlagsAndBinaryOptions) {
  std::vector<const char*> argv = {
      "-overwrite",
      "-recursive",
      "-filename_filter",
      "*.tif",
      "-min_pixel_size",
      "0.5",
      "-max_pixel_size",
      "100.0",
      "-of",
      "GPKG",
      "-tileindex",
      "custom_location",
      "-write_absolute_path",
      "-skip_different_projection",
      "-t_srs",
      "EPSG:4326",
      "-src_srs_name",
      "src_srs",
      "-src_srs_format",
      "WKT",
      "-lyr_name",
      "tiles",
      "-lco",
      "SPATIAL_INDEX=NO",
      "-gti_filename",
      "index.gti.xml",
      "-tr",
      "1.0",
      "1.0",
      "-te",
      "0.0",
      "0.0",
      "10.0",
      "10.0",
      "-ot",
      "Byte",
      "-bandcount",
      "1",
      "-nodata",
      "0",
      "-colorinterp",
      "gray",
      "-mask",
      "-mo",
      "TILES_AUTHOR=Google",
      "-fetch_md",
      "TIFFTAG_DATETIME",
      "datetime",
      "String",
      "-fetch_md",
      "INT_KEY",
      "int_field",
      "Integer",
      "-fetch_md",
      "INT64_KEY",
      "int64_field",
      "Integer64",
      "-fetch_md",
      "REAL_KEY",
      "real_field",
      "Real",
      "-fetch_md",
      "DATE_KEY",
      "date_field",
      "Date",
      "-fetch_md",
      "DATETIME_KEY",
      "dt_field",
      "DateTime",
      "--invoked-from-gdal-raster-index",
      "-skip_errors",
      "-q",
      "/vsimem/index.gpkg",
      "/vsimem/tile1.tif",
      "/vsimem/tile2.tif",
      nullptr,
  };
  GDALTileIndexOptionsForBinary bin_opts;
  OptionsUniquePtr opts(
      GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), &bin_opts));
  ASSERT_NE(nullptr, opts);
  EXPECT_TRUE(bin_opts.bQuiet);
  EXPECT_EQ("/vsimem/index.gpkg", bin_opts.osDest);
}

TEST_F(GdalTileIndexLibTest, OptionsNewErrors) {
  WithQuietHandler quiet_handler;

  // Invalid -t_srs
  std::vector<const char*> argv_bad_srs = {
      "-t_srs",
      "INVALID_UNKNOWN_SRS_XYZ",
      nullptr,
  };
  OptionsUniquePtr opts_bad_srs(GDALTileIndexOptionsNew(
      const_cast<char**>(argv_bad_srs.data()), nullptr));
  EXPECT_EQ(nullptr, opts_bad_srs);

  // Invalid -fetch_md type
  std::vector<const char*> argv_bad_type = {
      "-fetch_md", "KEY", "FIELD", "InvalidType", nullptr,
  };
  OptionsUniquePtr opts_bad_type(GDALTileIndexOptionsNew(
      const_cast<char**>(argv_bad_type.data()), nullptr));
  EXPECT_EQ(nullptr, opts_bad_type);

  // Unrecognized argument
  std::vector<const char*> argv_bad_arg = {
      "--non-existent-flag",
      nullptr,
  };
  OptionsUniquePtr opts_bad_arg(GDALTileIndexOptionsNew(
      const_cast<char**>(argv_bad_arg.data()), nullptr));
  EXPECT_EQ(nullptr, opts_bad_arg);
}

TEST_F(GdalTileIndexLibTest, ParserUsageAndSetProgress) {
  const std::string usage = GDALTileIndexAppGetParserUsage();
  EXPECT_FALSE(usage.empty());
  EXPECT_NE(std::string::npos, usage.find("gdaltindex"));

  OptionsUniquePtr opts(GDALTileIndexOptionsNew(nullptr, nullptr));
  ASSERT_NE(nullptr, opts);

  GDALTileIndexOptionsSetProgress(
      opts.get(), [](double, const char*, void*) -> int { return 1; }, nullptr);
}

TEST_F(GdalTileIndexLibTest, BasicShapefileTileIndex) {
  const std::string test_dir = "/vsimem/tindex_test/basic_shp";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  // Create tile 1: extent [0, 0] to [10, 10]
  const std::string tile1_path = test_dir + "/tile1.tif";
  {
    DatasetUniquePtr ds(GDALCreate(gtiff_driver, tile1_path.c_str(), 10, 10, 1,
                                   GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {0.0, 1.0, 0.0, 10.0, 0.0, -1.0};
    GDALSetGeoTransform(ds.get(), gt);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRSetWellKnownGeogCS(srs, "WGS84");
    GDALSetSpatialRef(ds.get(), srs);
    OSRDestroySpatialReference(srs);
  }

  // Create tile 2: extent [10, 0] to [20, 10]
  const std::string tile2_path = test_dir + "/tile2.tif";
  {
    DatasetUniquePtr ds(GDALCreate(gtiff_driver, tile2_path.c_str(), 10, 10, 1,
                                   GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {10.0, 1.0, 0.0, 10.0, 0.0, -1.0};
    GDALSetGeoTransform(ds.get(), gt);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRSetWellKnownGeogCS(srs, "WGS84");
    GDALSetSpatialRef(ds.get(), srs);
    OSRDestroySpatialReference(srs);
  }

  const std::string shp_path = test_dir + "/tiles.shp";
  const char* src_files[] = {tile1_path.c_str(), tile2_path.c_str()};

  DatasetUniquePtr index_ds(
      GDALTileIndex(shp_path.c_str(), 2, src_files, nullptr, nullptr));
  ASSERT_NE(nullptr, index_ds);

  OGRLayerH layer = GDALDatasetGetLayer(index_ds.get(), 0);
  ASSERT_NE(nullptr, layer);
  EXPECT_EQ(2, OGR_L_GetFeatureCount(layer, TRUE));

  // Verify feature 1
  FeatureUniquePtr feat1(OGR_L_GetNextFeature(layer));
  ASSERT_NE(nullptr, feat1);
  EXPECT_STREQ(tile1_path.c_str(),
               OGR_F_GetFieldAsString(
                   feat1.get(), OGR_F_GetFieldIndex(feat1.get(), "location")));
  OGRGeometryH geom1 = OGR_F_GetGeometryRef(feat1.get());
  ASSERT_NE(nullptr, geom1);
  OGREnvelope env1;
  OGR_G_GetEnvelope(geom1, &env1);
  EXPECT_DOUBLE_EQ(0.0, env1.MinX);
  EXPECT_DOUBLE_EQ(10.0, env1.MaxX);
  EXPECT_DOUBLE_EQ(0.0, env1.MinY);
  EXPECT_DOUBLE_EQ(10.0, env1.MaxY);

  // Verify feature 2
  FeatureUniquePtr feat2(OGR_L_GetNextFeature(layer));
  ASSERT_NE(nullptr, feat2);
  EXPECT_STREQ(tile2_path.c_str(),
               OGR_F_GetFieldAsString(
                   feat2.get(), OGR_F_GetFieldIndex(feat2.get(), "location")));
  OGRGeometryH geom2 = OGR_F_GetGeometryRef(feat2.get());
  ASSERT_NE(nullptr, geom2);
  OGREnvelope env2;
  OGR_G_GetEnvelope(geom2, &env2);
  EXPECT_DOUBLE_EQ(10.0, env2.MinX);
  EXPECT_DOUBLE_EQ(20.0, env2.MaxX);
}

TEST_F(GdalTileIndexLibTest, GeoPackageWithCustomLayerAndLocationField) {
  const std::string test_dir = "/vsimem/tindex_test/gpkg_custom";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  const std::string tile_path = test_dir + "/tile.tif";
  {
    DatasetUniquePtr ds(GDALCreate(gtiff_driver, tile_path.c_str(), 5, 5, 1,
                                   GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {0.0, 1.0, 0.0, 5.0, 0.0, -1.0};
    GDALSetGeoTransform(ds.get(), gt);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRSetWellKnownGeogCS(srs, "WGS84");
    GDALSetSpatialRef(ds.get(), srs);
    OSRDestroySpatialReference(srs);
  }

  const std::string gpkg_path = test_dir + "/index.gpkg";
  const char* src_files[] = {tile_path.c_str()};

  std::vector<const char*> argv = {
      "-of",
      "GPKG",
      "-lyr_name",
      "my_tiles",
      "-tileindex",
      "filepath",
      "-write_absolute_path",
      nullptr,
  };
  OptionsUniquePtr opts(
      GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, opts);

  DatasetUniquePtr index_ds(
      GDALTileIndex(gpkg_path.c_str(), 1, src_files, opts.get(), nullptr));
  ASSERT_NE(nullptr, index_ds);

  OGRLayerH layer = GDALDatasetGetLayerByName(index_ds.get(), "my_tiles");
  ASSERT_NE(nullptr, layer);
  EXPECT_EQ(1, OGR_L_GetFeatureCount(layer, TRUE));

  FeatureUniquePtr feat(OGR_L_GetNextFeature(layer));
  ASSERT_NE(nullptr, feat);
  EXPECT_NE(-1, OGR_F_GetFieldIndex(feat.get(), "filepath"));
  EXPECT_STREQ(tile_path.c_str(),
               OGR_F_GetFieldAsString(
                   feat.get(), OGR_F_GetFieldIndex(feat.get(), "filepath")));
}

TEST_F(GdalTileIndexLibTest, TargetSRSAndReprojection) {
  const std::string test_dir = "/vsimem/tindex_test/reproject";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  // Input tile in UTM zone 31N (EPSG:32631)
  const std::string tile_path = test_dir + "/utm_tile.tif";
  {
    DatasetUniquePtr ds(GDALCreate(gtiff_driver, tile_path.c_str(), 10, 10, 1,
                                   GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {500000.0, 10.0, 0.0, 4500000.0, 0.0, -10.0};
    GDALSetGeoTransform(ds.get(), gt);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 32631);
    GDALSetSpatialRef(ds.get(), srs);
    OSRDestroySpatialReference(srs);
  }

  const std::string shp_path = test_dir + "/reproj.shp";
  const char* src_files[] = {tile_path.c_str()};

  std::vector<const char*> argv = {
      "-t_srs",          "EPSG:4326", "-src_srs_name", "src_srs",
      "-src_srs_format", "EPSG",      nullptr,
  };
  OptionsUniquePtr opts(
      GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, opts);

  DatasetUniquePtr index_ds(
      GDALTileIndex(shp_path.c_str(), 1, src_files, opts.get(), nullptr));
  ASSERT_NE(nullptr, index_ds);

  OGRLayerH layer = GDALDatasetGetLayer(index_ds.get(), 0);
  ASSERT_NE(nullptr, layer);

  FeatureUniquePtr feat(OGR_L_GetNextFeature(layer));
  ASSERT_NE(nullptr, feat);
  EXPECT_NE(-1, OGR_F_GetFieldIndex(feat.get(), "src_srs"));
  EXPECT_STREQ("EPSG:32631",
               OGR_F_GetFieldAsString(
                   feat.get(), OGR_F_GetFieldIndex(feat.get(), "src_srs")));

  OGRGeometryH geom = OGR_F_GetGeometryRef(feat.get());
  ASSERT_NE(nullptr, geom);
  OGREnvelope env;
  OGR_G_GetEnvelope(geom, &env);
  // Extent in EPSG:4326 should be longitudes near 3 degrees East, latitudes
  // near 40 degrees North
  EXPECT_GT(env.MinX, 0.0);
  EXPECT_LT(env.MaxX, 10.0);
  EXPECT_GT(env.MinY, 35.0);
  EXPECT_LT(env.MaxY, 45.0);
}

TEST_F(GdalTileIndexLibTest, FetchMetadataOption) {
  const std::string test_dir = "/vsimem/tindex_test/fetch_md";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  const std::string tile_path = test_dir + "/tile_md.tif";
  {
    DatasetUniquePtr ds(GDALCreate(gtiff_driver, tile_path.c_str(), 5, 5, 1,
                                   GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {0.0, 1.0, 0.0, 5.0, 0.0, -1.0};
    GDALSetGeoTransform(ds.get(), gt);
    GDALSetMetadataItem(ds.get(), "CUSTOM_SENSOR", "SENTINEL", nullptr);
  }

  const std::string shp_path = test_dir + "/fetch.shp";
  const char* src_files[] = {tile_path.c_str()};

  std::vector<const char*> argv = {
      "-fetch_md", "CUSTOM_SENSOR", "sensor", "String", nullptr,
  };
  OptionsUniquePtr opts(
      GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, opts);

  DatasetUniquePtr index_ds(
      GDALTileIndex(shp_path.c_str(), 1, src_files, opts.get(), nullptr));
  ASSERT_NE(nullptr, index_ds);

  OGRLayerH layer = GDALDatasetGetLayer(index_ds.get(), 0);
  ASSERT_NE(nullptr, layer);

  FeatureUniquePtr feat(OGR_L_GetNextFeature(layer));
  ASSERT_NE(nullptr, feat);
  EXPECT_NE(-1, OGR_F_GetFieldIndex(feat.get(), "sensor"));
  EXPECT_STREQ("SENTINEL",
               OGR_F_GetFieldAsString(
                   feat.get(), OGR_F_GetFieldIndex(feat.get(), "sensor")));
}

TEST_F(GdalTileIndexLibTest, FilteringConstraints) {
  const std::string test_dir = "/vsimem/tindex_test/filters";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  // Tile 1: 1 band, Byte, resolution 1.0, extent [0, 0] to [5, 5]
  const std::string tile1 = test_dir + "/tile1.tif";
  {
    DatasetUniquePtr ds(
        GDALCreate(gtiff_driver, tile1.c_str(), 5, 5, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {0.0, 1.0, 0.0, 5.0, 0.0, -1.0};
    GDALSetGeoTransform(ds.get(), gt);
  }

  // Tile 2: 3 bands, Byte, resolution 2.0, extent [10, 10] to [20, 20]
  const std::string tile2 = test_dir + "/tile2.tif";
  {
    DatasetUniquePtr ds(
        GDALCreate(gtiff_driver, tile2.c_str(), 5, 5, 3, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {10.0, 2.0, 0.0, 20.0, 0.0, -2.0};
    GDALSetGeoTransform(ds.get(), gt);
  }

  const char* src_files[] = {tile1.c_str(), tile2.c_str()};

  // 1. Set band count metadata
  {
    std::vector<const char*> argv = {
        "-bandcount",
        "1",
        "-overwrite",
        nullptr,
    };
    OptionsUniquePtr opts(
        GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, opts);

    DatasetUniquePtr index_ds(
        GDALTileIndex((test_dir + "/filter_band.shp").c_str(), 2, src_files,
                      opts.get(), nullptr));
    ASSERT_NE(nullptr, index_ds);
    OGRLayerH layer = GDALDatasetGetLayer(index_ds.get(), 0);
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(2, OGR_L_GetFeatureCount(layer, TRUE));
    EXPECT_STREQ("1", GDALGetMetadataItem(layer, "BAND_COUNT", nullptr));
  }

  // 2. Filter by pixel size: min_pixel_size 1.5 -> tile1 (res 1.0) is skipped
  {
    std::vector<const char*> argv = {
        "-min_pixel_size",
        "1.5",
        nullptr,
    };
    OptionsUniquePtr opts(
        GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, opts);

    DatasetUniquePtr index_ds(
        GDALTileIndex((test_dir + "/filter_res.shp").c_str(), 2, src_files,
                      opts.get(), nullptr));
    ASSERT_NE(nullptr, index_ds);
    OGRLayerH layer = GDALDatasetGetLayer(index_ds.get(), 0);
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(1, OGR_L_GetFeatureCount(layer, TRUE));
  }

  // 3. Set target extent metadata (-te)
  {
    std::vector<const char*> argv = {
        "-te", "0.0", "0.0", "8.0", "8.0", nullptr,
    };
    OptionsUniquePtr opts(
        GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, opts);

    DatasetUniquePtr index_ds(
        GDALTileIndex((test_dir + "/filter_te.shp").c_str(), 2, src_files,
                      opts.get(), nullptr));
    ASSERT_NE(nullptr, index_ds);
    OGRLayerH layer = GDALDatasetGetLayer(index_ds.get(), 0);
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(2, OGR_L_GetFeatureCount(layer, TRUE));
    EXPECT_STREQ("0", GDALGetMetadataItem(layer, "MINX", nullptr));
  }
}

TEST_F(GdalTileIndexLibTest, InternalApiPreCreatedDataset) {
  const std::string test_dir = "/vsimem/tindex_test/internal";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH mem_driver = GDALGetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  const std::string tile = test_dir + "/tile.tif";
  {
    DatasetUniquePtr ds(
        GDALCreate(gtiff_driver, tile.c_str(), 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
    GDALSetGeoTransform(ds.get(), gt);
  }

  // Destination vector dataset pre-created in memory
  DatasetUniquePtr dst(
      GDALCreate(mem_driver, "mem_index", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, dst);
  OGRLayerH layer =
      GDALDatasetCreateLayer(dst.get(), "tiles", nullptr, wkbPolygon, nullptr);
  ASSERT_NE(nullptr, layer);
  OGRFieldDefnH fld_loc = OGR_Fld_Create("location", OFTString);
  OGR_L_CreateField(layer, fld_loc, FALSE);
  OGR_Fld_Destroy(fld_loc);

  const char* src_files[] = {tile.c_str()};
  GDALDatasetH res = GDALTileIndexInternal(nullptr, dst.get(), layer, 1,
                                           src_files, nullptr, nullptr);
  EXPECT_EQ(dst.get(), res);
  EXPECT_EQ(1, OGR_L_GetFeatureCount(layer, TRUE));
}

TEST_F(GdalTileIndexLibTest, ErrorHandling) {
  WithQuietHandler quiet_handler;

  // 1. Zero source count
  int usage_error = FALSE;
  GDALDatasetH ds =
      GDALTileIndex("/vsimem/out.shp", 0, nullptr, nullptr, &usage_error);
  EXPECT_EQ(nullptr, ds);
  EXPECT_TRUE(usage_error);

  // 2. Missing source file when fail_on_errors is true
  const char* missing_files[] = {"/vsimem/non_existing_tile.tif"};
  std::vector<const char*> argv = {
      "--invoked-from-gdal-raster-index",
      nullptr,
  };
  OptionsUniquePtr opts(
      GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, opts);

  GDALDatasetH ds_fail =
      GDALTileIndex("/vsimem/fail.shp", 1, missing_files, opts.get(), nullptr);
  EXPECT_EQ(nullptr, ds_fail);
}

TEST_F(GdalTileIndexLibTest, DirectoryRecursionAndFilenameFilter) {
  const std::string test_dir = "/vsimem/tindex_test/recursion";
  VSIMkdir(test_dir.c_str(), 0755);
  const std::string sub_dir = test_dir + "/sub";
  VSIMkdir(sub_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  // 1. Tile in top directory matching filter
  const std::string t1 = test_dir + "/tile1.tif";
  {
    DatasetUniquePtr ds(
        GDALCreate(gtiff_driver, t1.c_str(), 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
    GDALSetGeoTransform(ds.get(), gt);
  }

  // 2. Non-tif file in top directory (should be ignored by filter)
  const std::string txt = test_dir + "/notes.txt";
  VSILFILE* f = VSIFOpenL(txt.c_str(), "wb");
  if (f) VSIFCloseL(f);

  // 3. Tile in subdirectory matching filter
  const std::string t2 = sub_dir + "/tile2.tif";
  {
    DatasetUniquePtr ds(
        GDALCreate(gtiff_driver, t2.c_str(), 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {4.0, 1.0, 0.0, 4.0, 0.0, -1.0};
    GDALSetGeoTransform(ds.get(), gt);
  }

  const char* src_dirs[] = {test_dir.c_str()};

  std::vector<const char*> argv = {
      "-recursive",
      "-filename_filter",
      "*.tif",
      nullptr,
  };
  OptionsUniquePtr opts(
      GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, opts);

  const std::string out_shp = test_dir + "/rec.shp";
  DatasetUniquePtr index_ds(
      GDALTileIndex(out_shp.c_str(), 1, src_dirs, opts.get(), nullptr));
  ASSERT_NE(nullptr, index_ds);

  OGRLayerH layer = GDALDatasetGetLayer(index_ds.get(), 0);
  ASSERT_NE(nullptr, layer);
  EXPECT_EQ(2, OGR_L_GetFeatureCount(layer, TRUE));
}

TEST_F(GdalTileIndexLibTest, GTIXmlOutputAndLayerMetadata) {
  const std::string test_dir = "/vsimem/tindex_test/gti";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  const std::string tile = test_dir + "/tile.tif";
  {
    DatasetUniquePtr ds(
        GDALCreate(gtiff_driver, tile.c_str(), 4, 4, 3, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
    GDALSetGeoTransform(ds.get(), gt);
  }

  const char* src_files[] = {tile.c_str()};
  const std::string gti_xml = test_dir + "/index.gti.xml";
  const std::string out_shp = test_dir + "/tiles.shp";

  std::vector<const char*> argv = {
      "-gti_filename", gti_xml.c_str(),
      "-tr",           "1.0",
      "1.0",           "-te",
      "0.0",           "0.0",
      "10.0",          "10.0",
      "-bandcount",    "3",
      "-ot",           "Byte",
      "-nodata",       "0",
      "-colorinterp",  "red,green,blue",
      "-mask",         nullptr,
  };
  OptionsUniquePtr opts(
      GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, opts);

  DatasetUniquePtr index_ds(
      GDALTileIndex(out_shp.c_str(), 1, src_files, opts.get(), nullptr));
  ASSERT_NE(nullptr, index_ds);

  // Check GTI XML file exists and contains metadata tags
  VSIStatBufL stat_buf;
  EXPECT_EQ(0, VSIStatL(gti_xml.c_str(), &stat_buf));

  // Also test error when -mo is used with -gti_filename
  WithQuietHandler quiet_handler;
  std::vector<const char*> argv_err = {
      "-gti_filename", gti_xml.c_str(), "-mo", "FOO=BAR", nullptr,
  };
  OptionsUniquePtr opts_err(
      GDALTileIndexOptionsNew(const_cast<char**>(argv_err.data()), nullptr));
  ASSERT_NE(nullptr, opts_err);
  GDALDatasetH ds_fail =
      GDALTileIndex(out_shp.c_str(), 1, src_files, opts_err.get(), nullptr);
  EXPECT_EQ(nullptr, ds_fail);
}

TEST_F(GdalTileIndexLibTest, LayerMetadataNonGTIContext) {
  const std::string test_dir = "/vsimem/tindex_test/layer_md";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  const std::string tile = test_dir + "/tile.tif";
  {
    DatasetUniquePtr ds(
        GDALCreate(gtiff_driver, tile.c_str(), 4, 4, 3, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
    GDALSetGeoTransform(ds.get(), gt);
  }

  const char* src_files[] = {tile.c_str()};
  const std::string out_shp = test_dir + "/tiles.shp";

  std::vector<const char*> argv = {
      "-tr",        "2.0", "2.0",           "-te",
      "0.0",        "0.0", "20.0",          "20.0",
      "-bandcount", "3",   "-ot",           "Byte",
      "-nodata",    "255", "-colorinterp",  "red,green,blue",
      "-mask",      "-mo", "AUTHOR=Google", nullptr,
  };
  OptionsUniquePtr opts(
      GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, opts);

  DatasetUniquePtr index_ds(
      GDALTileIndex(out_shp.c_str(), 1, src_files, opts.get(), nullptr));
  ASSERT_NE(nullptr, index_ds);

  OGRLayerH layer = GDALDatasetGetLayer(index_ds.get(), 0);
  ASSERT_NE(nullptr, layer);
  EXPECT_STREQ("2", GDALGetMetadataItem(layer, "RESX", nullptr));
  EXPECT_STREQ("2", GDALGetMetadataItem(layer, "RESY", nullptr));
  EXPECT_STREQ("0", GDALGetMetadataItem(layer, "MINX", nullptr));
  EXPECT_STREQ("20", GDALGetMetadataItem(layer, "MAXX", nullptr));
  EXPECT_STREQ("3", GDALGetMetadataItem(layer, "BAND_COUNT", nullptr));
  EXPECT_STREQ("Byte", GDALGetMetadataItem(layer, "DATA_TYPE", nullptr));
  EXPECT_STREQ("255", GDALGetMetadataItem(layer, "NODATA", nullptr));
  EXPECT_STREQ("YES", GDALGetMetadataItem(layer, "MASK_BAND", nullptr));
  EXPECT_STREQ("Google", GDALGetMetadataItem(layer, "AUTHOR", nullptr));
}

TEST_F(GdalTileIndexLibTest, DuplicateFileAndProjectionDifferences) {
  const std::string test_dir = "/vsimem/tindex_test/dups_projs";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  // Tile 1 in EPSG:4326
  const std::string t1 = test_dir + "/t1.tif";
  {
    DatasetUniquePtr ds(
        GDALCreate(gtiff_driver, t1.c_str(), 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
    GDALSetGeoTransform(ds.get(), gt);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRSetWellKnownGeogCS(srs, "WGS84");
    GDALSetSpatialRef(ds.get(), srs);
    OSRDestroySpatialReference(srs);
  }

  // Tile 2 in EPSG:3857
  const std::string t2 = test_dir + "/t2.tif";
  {
    DatasetUniquePtr ds(
        GDALCreate(gtiff_driver, t2.c_str(), 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
    GDALSetGeoTransform(ds.get(), gt);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 3857);
    GDALSetSpatialRef(ds.get(), srs);
    OSRDestroySpatialReference(srs);
  }

  // 1. Duplicate tile passed: when indexing an existing index, should warn and
  // skip duplicate
  {
    WithQuietHandler quiet_handler;
    const char* src_files[] = {t1.c_str()};
    DatasetUniquePtr index_ds(GDALTileIndex((test_dir + "/dups.shp").c_str(), 1,
                                            src_files, nullptr, nullptr));
    ASSERT_NE(nullptr, index_ds);
  }
  {
    WithQuietHandler quiet_handler;
    const char* src_files[] = {t1.c_str()};
    DatasetUniquePtr index_ds(GDALTileIndex((test_dir + "/dups.shp").c_str(), 1,
                                            src_files, nullptr, nullptr));
    ASSERT_NE(nullptr, index_ds);
    OGRLayerH layer = GDALDatasetGetLayer(index_ds.get(), 0);
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(1, OGR_L_GetFeatureCount(layer, TRUE));
  }

  // 2. Different projection with -skip_different_projection: should warn and
  // skip t2
  {
    WithQuietHandler quiet_handler;
    const char* src_files[] = {t1.c_str(), t2.c_str()};
    std::vector<const char*> argv = {
        "-skip_different_projection",
        nullptr,
    };
    OptionsUniquePtr opts(
        GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, opts);

    DatasetUniquePtr index_ds(
        GDALTileIndex((test_dir + "/diff_skip.shp").c_str(), 2, src_files,
                      opts.get(), nullptr));
    ASSERT_NE(nullptr, index_ds);
    OGRLayerH layer = GDALDatasetGetLayer(index_ds.get(), 0);
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(1, OGR_L_GetFeatureCount(layer, TRUE));
  }

  // 3. Different projection without -skip_different_projection: warns but
  // inserts both
  {
    WithQuietHandler quiet_handler;
    const char* src_files[] = {t1.c_str(), t2.c_str()};
    DatasetUniquePtr index_ds(GDALTileIndex(
        (test_dir + "/diff_keep.shp").c_str(), 2, src_files, nullptr, nullptr));
    ASSERT_NE(nullptr, index_ds);
    OGRLayerH layer = GDALDatasetGetLayer(index_ds.get(), 0);
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(2, OGR_L_GetFeatureCount(layer, TRUE));
  }
}

TEST_F(GdalTileIndexLibTest, SourceSRSFormatsPROJAndWKTAndAUTO) {
  const std::string test_dir = "/vsimem/tindex_test/srs_formats";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  const std::string tile = test_dir + "/tile.tif";
  {
    DatasetUniquePtr ds(
        GDALCreate(gtiff_driver, tile.c_str(), 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
    GDALSetGeoTransform(ds.get(), gt);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 4326);
    GDALSetSpatialRef(ds.get(), srs);
    OSRDestroySpatialReference(srs);
  }

  const char* src_files[] = {tile.c_str()};

  // 1. PROJ format
  {
    std::vector<const char*> argv = {
        "-src_srs_name", "srs_fld", "-src_srs_format", "PROJ", nullptr,
    };
    OptionsUniquePtr opts(
        GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, opts);

    DatasetUniquePtr index_ds(GDALTileIndex((test_dir + "/proj.shp").c_str(), 1,
                                            src_files, opts.get(), nullptr));
    ASSERT_NE(nullptr, index_ds);
    OGRLayerH layer = GDALDatasetGetLayer(index_ds.get(), 0);
    ASSERT_NE(nullptr, layer);
    FeatureUniquePtr feat(OGR_L_GetNextFeature(layer));
    ASSERT_NE(nullptr, feat);
    EXPECT_NE(-1, OGR_F_GetFieldIndex(feat.get(), "srs_fld"));
  }

  // 2. WKT format
  {
    std::vector<const char*> argv = {
        "-src_srs_name", "srs_fld", "-src_srs_format", "WKT", nullptr,
    };
    OptionsUniquePtr opts(
        GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, opts);

    DatasetUniquePtr index_ds(GDALTileIndex((test_dir + "/wkt.shp").c_str(), 1,
                                            src_files, opts.get(), nullptr));
    ASSERT_NE(nullptr, index_ds);
    OGRLayerH layer = GDALDatasetGetLayer(index_ds.get(), 0);
    ASSERT_NE(nullptr, layer);
    FeatureUniquePtr feat(OGR_L_GetNextFeature(layer));
    ASSERT_NE(nullptr, feat);
    EXPECT_NE(-1, OGR_F_GetFieldIndex(feat.get(), "srs_fld"));
  }

  // 3. AUTO format
  {
    std::vector<const char*> argv = {
        "-src_srs_name", "srs_fld", "-src_srs_format", "AUTO", nullptr,
    };
    OptionsUniquePtr opts(
        GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, opts);

    DatasetUniquePtr index_ds(GDALTileIndex((test_dir + "/auto.shp").c_str(), 1,
                                            src_files, opts.get(), nullptr));
    ASSERT_NE(nullptr, index_ds);
    OGRLayerH layer = GDALDatasetGetLayer(index_ds.get(), 0);
    ASSERT_NE(nullptr, layer);
    FeatureUniquePtr feat(OGR_L_GetNextFeature(layer));
    ASSERT_NE(nullptr, feat);
    EXPECT_NE(-1, OGR_F_GetFieldIndex(feat.get(), "srs_fld"));
  }
}

TEST_F(GdalTileIndexLibTest, MultiLayerGeoPackageAndOverwrite) {
  const std::string test_dir = "/vsimem/tindex_test/multilayer";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  const std::string tile = test_dir + "/tile.tif";
  {
    DatasetUniquePtr ds(
        GDALCreate(gtiff_driver, tile.c_str(), 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
    GDALSetGeoTransform(ds.get(), gt);
  }

  const char* src_files[] = {tile.c_str()};
  const std::string gpkg_path = test_dir + "/multi.gpkg";

  // Create a GPKG with two pre-existing layers
  {
    GDALDriverH gpkg_driver = GDALGetDriverByName("GPKG");
    ASSERT_NE(nullptr, gpkg_driver);
    DatasetUniquePtr ds(GDALCreate(gpkg_driver, gpkg_path.c_str(), 0, 0, 0,
                                   GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
    OGRLayerH l1 = GDALDatasetCreateLayer(ds.get(), "layer1", nullptr,
                                          wkbPolygon, nullptr);
    ASSERT_NE(nullptr, l1);
    OGRFieldDefnH fld1 = OGR_Fld_Create("location", OFTString);
    OGR_L_CreateField(l1, fld1, TRUE);
    OGR_Fld_Destroy(fld1);

    OGRLayerH l2 = GDALDatasetCreateLayer(ds.get(), "layer2", nullptr,
                                          wkbPolygon, nullptr);
    ASSERT_NE(nullptr, l2);
    OGRFieldDefnH fld2 = OGR_Fld_Create("location", OFTString);
    OGR_L_CreateField(l2, fld2, TRUE);
    OGR_Fld_Destroy(fld2);
  }

  // Opening existing GPKG with multiple layers without -lyr_name should fail
  {
    WithQuietHandler quiet_handler;
    int usage_err = FALSE;
    DatasetUniquePtr ds(
        GDALTileIndex(gpkg_path.c_str(), 1, src_files, nullptr, &usage_err));
    EXPECT_EQ(nullptr, ds);
    EXPECT_TRUE(usage_err);
  }

  // Specifying -lyr_name with multiple layers should succeed
  {
    std::vector<const char*> argv = {
        "-of", "GPKG", "-lyr_name", "layer1", nullptr,
    };
    OptionsUniquePtr opts(
        GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, opts);
    DatasetUniquePtr ds(
        GDALTileIndex(gpkg_path.c_str(), 1, src_files, opts.get(), nullptr));
    ASSERT_NE(nullptr, ds);
  }

  // Overwriting existing layer2
  {
    std::vector<const char*> argv = {
        "-of", "GPKG", "-lyr_name", "layer2", "-overwrite", nullptr,
    };
    OptionsUniquePtr opts(
        GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, opts);
    DatasetUniquePtr ds(
        GDALTileIndex(gpkg_path.c_str(), 1, src_files, opts.get(), nullptr));
    ASSERT_NE(nullptr, ds);
  }
}

TEST_F(GdalTileIndexLibTest, GTIBandOptions) {
  const std::string test_dir = "/vsimem/tindex_test/gti_bands";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  const std::string tile = test_dir + "/tile.tif";
  {
    DatasetUniquePtr ds(
        GDALCreate(gtiff_driver, tile.c_str(), 4, 4, 2, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
    GDALSetGeoTransform(ds.get(), gt);
  }

  const char* src_files[] = {tile.c_str()};
  const std::string gti_path = test_dir + "/index.gti.xml";
  const std::string shp_path = test_dir + "/index.shp";

  // 1. Multiple tokens without explicit -bandcount
  {
    std::vector<const char*> argv = {
        "-gti_filename", gti_path.c_str(), "-ot",          "Byte,UInt16",
        "-nodata",       "0,65535",        "-colorinterp", "Red,Green",
        "-mask",         nullptr,
    };
    OptionsUniquePtr opts(
        GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, opts);

    DatasetUniquePtr ds(
        GDALTileIndex(shp_path.c_str(), 1, src_files, opts.get(), nullptr));
    ASSERT_NE(nullptr, ds);

    CPLXMLTreeCloser xml(CPLParseXMLFile(gti_path.c_str()));
    ASSERT_NE(nullptr, xml.get());
    CPLXMLNode* psMask = CPLSearchXMLNode(xml.get(), "=MaskBand");
    ASSERT_NE(nullptr, psMask);
    EXPECT_STREQ("true", CPLGetXMLValue(psMask, nullptr, ""));
  }

  // 2. Single token with -bandcount 2
  const std::string gti_path2 = test_dir + "/index2.gti.xml";
  const std::string shp_path2 = test_dir + "/index2.shp";
  {
    std::vector<const char*> argv = {
        "-gti_filename", gti_path2.c_str(),
        "-bandcount",    "2",
        "-ot",           "Byte",
        "-nodata",       "255",
        "-colorinterp",  "Gray",
        nullptr,
    };
    OptionsUniquePtr opts(
        GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, opts);

    DatasetUniquePtr ds(
        GDALTileIndex(shp_path2.c_str(), 1, src_files, opts.get(), nullptr));
    ASSERT_NE(nullptr, ds);

    CPLXMLTreeCloser xml(CPLParseXMLFile(gti_path2.c_str()));
    ASSERT_NE(nullptr, xml.get());
    CPLXMLNode* psBand = CPLSearchXMLNode(xml.get(), "=Band");
    ASSERT_NE(nullptr, psBand);
    EXPECT_STREQ("Byte", CPLGetXMLValue(psBand, "dataType", ""));
  }
}

TEST_F(GdalTileIndexLibTest, FetchMetadata) {
  const std::string test_dir = "/vsimem/tindex_test/fetch_md";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  const std::string tile = test_dir + "/tile.tif";
  {
    DatasetUniquePtr ds(
        GDALCreate(gtiff_driver, tile.c_str(), 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
    GDALSetGeoTransform(ds.get(), gt);
    GDALSetMetadataItem(ds.get(), "TIFFTAG_DATETIME", "2026:11:05 10:20:30",
                        nullptr);
    GDALSetMetadataItem(ds.get(), "CUSTOM_STR", "hello", nullptr);
    GDALSetMetadataItem(ds.get(), "CUSTOM_INT", "42", nullptr);
    GDALSetMetadataItem(ds.get(), "CUSTOM_INT64", "9876543210", nullptr);
    GDALSetMetadataItem(ds.get(), "CUSTOM_DATE", "2026/11/05", nullptr);
  }

  const char* src_files[] = {tile.c_str()};

  // 1. Valid -fetch_md arguments
  {
    std::vector<const char*> argv = {
        "-fetch_md", "{PIXEL_SIZE}",     "res",       "Real",
        "-fetch_md", "TIFFTAG_DATETIME", "dt",        "DateTime",
        "-fetch_md", "CUSTOM_STR",       "str_fld",   "String",
        "-fetch_md", "CUSTOM_INT",       "int_fld",   "Integer",
        "-fetch_md", "CUSTOM_INT64",     "int64_fld", "Integer64",
        "-fetch_md", "CUSTOM_DATE",      "date_fld",  "Date",
        nullptr,
    };
    OptionsUniquePtr opts(
        GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, opts);

    const std::string shp_path = test_dir + "/index.shp";
    DatasetUniquePtr ds(
        GDALTileIndex(shp_path.c_str(), 1, src_files, opts.get(), nullptr));
    ASSERT_NE(nullptr, ds);

    OGRLayerH layer = GDALDatasetGetLayer(ds.get(), 0);
    ASSERT_NE(nullptr, layer);
    FeatureUniquePtr feat(OGR_L_GetNextFeature(layer));
    ASSERT_NE(nullptr, feat);

    int idx_res = OGR_F_GetFieldIndex(feat.get(), "res");
    ASSERT_NE(-1, idx_res);
    EXPECT_DOUBLE_EQ(1.0, OGR_F_GetFieldAsDouble(feat.get(), idx_res));

    int idx_dt = OGR_F_GetFieldIndex(feat.get(), "dt");
    ASSERT_NE(-1, idx_dt);
    EXPECT_STREQ("2026/11/05 10:20:30",
                 OGR_F_GetFieldAsString(feat.get(), idx_dt));

    int idx_str = OGR_F_GetFieldIndex(feat.get(), "str_fld");
    ASSERT_NE(-1, idx_str);
    EXPECT_STREQ("hello", OGR_F_GetFieldAsString(feat.get(), idx_str));
  }

  // 2. Invalid field type in -fetch_md
  {
    WithQuietHandler quiet_handler;
    std::vector<const char*> argv = {
        "-fetch_md", "FOO", "bar", "InvalidType", nullptr,
    };
    OptionsUniquePtr opts(
        GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
    EXPECT_EQ(nullptr, opts);
  }
}

TEST_F(GdalTileIndexLibTest, ProgressCallback) {
  const std::string test_dir = "/vsimem/tindex_test/progress";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  const std::string tile = test_dir + "/tile.tif";
  {
    DatasetUniquePtr ds(
        GDALCreate(gtiff_driver, tile.c_str(), 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
    GDALSetGeoTransform(ds.get(), gt);
  }

  const char* src_files[] = {tile.c_str()};

  // 1. Progress function returns FALSE -> aborts
  {
    OptionsUniquePtr opts(GDALTileIndexOptionsNew(nullptr, nullptr));
    ASSERT_NE(nullptr, opts);
    GDALProgressFunc cancel_cb = [](double, const char*, void*) -> int {
      return FALSE;
    };
    GDALTileIndexOptionsSetProgress(opts.get(), cancel_cb, nullptr);

    WithQuietHandler quiet_handler;
    DatasetUniquePtr ds(GDALTileIndex((test_dir + "/cancel.shp").c_str(), 1,
                                      src_files, opts.get(), nullptr));
    EXPECT_EQ(nullptr, ds);
  }

  // 2. Progress function returns TRUE -> succeeds
  {
    OptionsUniquePtr opts(GDALTileIndexOptionsNew(nullptr, nullptr));
    ASSERT_NE(nullptr, opts);
    int call_count = 0;
    GDALProgressFunc count_cb = [](double, const char*, void* data) -> int {
      int* cnt = static_cast<int*>(data);
      (*cnt)++;
      return TRUE;
    };
    GDALTileIndexOptionsSetProgress(opts.get(), count_cb, &call_count);

    DatasetUniquePtr ds(GDALTileIndex((test_dir + "/success.shp").c_str(), 1,
                                      src_files, opts.get(), nullptr));
    ASSERT_NE(nullptr, ds);
    EXPECT_GT(call_count, 0);
  }
}

TEST_F(GdalTileIndexLibTest, SkippingAndWarnings) {
  const std::string test_dir = "/vsimem/tindex_test/skipping";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  // Normal tile with pixel size 10.0
  const std::string big_res_tile = test_dir + "/big_res.tif";
  {
    DatasetUniquePtr ds(GDALCreate(gtiff_driver, big_res_tile.c_str(), 4, 4, 1,
                                   GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {0.0, 10.0, 0.0, 40.0, 0.0, -10.0};
    GDALSetGeoTransform(ds.get(), gt);
  }

  // Tile with no geotransform
  const std::string no_gt_tile = test_dir + "/no_gt.tif";
  {
    DatasetUniquePtr ds(GDALCreate(gtiff_driver, no_gt_tile.c_str(), 4, 4, 1,
                                   GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
  }

  // 1. -max_pixel_size 5.0 should skip big_res_tile
  {
    WithQuietHandler quiet_handler;
    const char* src_files[] = {big_res_tile.c_str()};
    std::vector<const char*> argv = {
        "-max_pixel_size",
        "5.0",
        nullptr,
    };
    OptionsUniquePtr opts(
        GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, opts);

    DatasetUniquePtr ds(GDALTileIndex((test_dir + "/max_pix.shp").c_str(), 1,
                                      src_files, opts.get(), nullptr));
    ASSERT_NE(nullptr, ds);
    OGRLayerH layer = GDALDatasetGetLayer(ds.get(), 0);
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(0, OGR_L_GetFeatureCount(layer, TRUE));
  }

  // 2. Tile without geotransform should be skipped
  {
    WithQuietHandler quiet_handler;
    const char* src_files[] = {no_gt_tile.c_str()};
    DatasetUniquePtr ds(GDALTileIndex((test_dir + "/no_gt.shp").c_str(), 1,
                                      src_files, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    OGRLayerH layer = GDALDatasetGetLayer(ds.get(), 0);
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(0, OGR_L_GetFeatureCount(layer, TRUE));
  }

  // 3. First tile non-existent, second tile valid
  {
    WithQuietHandler quiet_handler;
    const std::string valid_tile = test_dir + "/valid.tif";
    {
      DatasetUniquePtr ds(GDALCreate(gtiff_driver, valid_tile.c_str(), 4, 4, 1,
                                     GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds);
      double gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
      GDALSetGeoTransform(ds.get(), gt);
    }
    const std::string missing_tile = test_dir + "/missing.tif";
    const char* src_files[] = {missing_tile.c_str(), valid_tile.c_str()};
    DatasetUniquePtr ds(GDALTileIndex((test_dir + "/skip_first.shp").c_str(), 2,
                                      src_files, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    OGRLayerH layer = GDALDatasetGetLayer(ds.get(), 0);
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(1, OGR_L_GetFeatureCount(layer, TRUE));
  }

  // 4. Empty directory with -recursive: cannot find any tile
  {
    WithQuietHandler quiet_handler;
    const std::string empty_dir = test_dir + "/empty_dir";
    VSIMkdir(empty_dir.c_str(), 0755);
    const char* src_files[] = {empty_dir.c_str()};
    std::vector<const char*> argv = {
        "-recursive",
        nullptr,
    };
    OptionsUniquePtr opts(
        GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, opts);

    DatasetUniquePtr ds(GDALTileIndex((test_dir + "/empty.shp").c_str(), 1,
                                      src_files, opts.get(), nullptr));
    EXPECT_EQ(nullptr, ds);
  }
}

TEST_F(GdalTileIndexLibTest, SrcSRSFormatEPSG) {
  const std::string test_dir = "/vsimem/tindex_test/epsg_format";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  const std::string tile = test_dir + "/tile.tif";
  {
    DatasetUniquePtr ds(
        GDALCreate(gtiff_driver, tile.c_str(), 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
    GDALSetGeoTransform(ds.get(), gt);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 3857);
    GDALSetSpatialRef(ds.get(), srs);
    OSRDestroySpatialReference(srs);
  }

  const char* src_files[] = {tile.c_str()};
  std::vector<const char*> argv = {
      "-src_srs_name", "srs_code", "-src_srs_format", "EPSG", nullptr,
  };
  OptionsUniquePtr opts(
      GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, opts);

  DatasetUniquePtr ds(GDALTileIndex((test_dir + "/epsg.shp").c_str(), 1,
                                    src_files, opts.get(), nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayerH layer = GDALDatasetGetLayer(ds.get(), 0);
  ASSERT_NE(nullptr, layer);
  FeatureUniquePtr feat(OGR_L_GetNextFeature(layer));
  ASSERT_NE(nullptr, feat);
  int idx = OGR_F_GetFieldIndex(feat.get(), "srs_code");
  ASSERT_NE(-1, idx);
  EXPECT_STREQ("EPSG:3857", OGR_F_GetFieldAsString(feat.get(), idx));
}

TEST_F(GdalTileIndexLibTest, AdditionalEdgeCases) {
  const std::string test_dir = "/vsimem/tindex_test/edge_cases";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  const std::string tile1 = test_dir + "/tile1.tif";
  {
    DatasetUniquePtr ds(
        GDALCreate(gtiff_driver, tile1.c_str(), 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
    GDALSetGeoTransform(ds.get(), gt);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 4326);
    GDALSetSpatialRef(ds.get(), srs);
    OSRDestroySpatialReference(srs);
  }

  const std::string tile2 = test_dir + "/tile2.tif";
  {
    DatasetUniquePtr ds(
        GDALCreate(gtiff_driver, tile2.c_str(), 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {10.0, 1.0, 0.0, 14.0, 0.0, -1.0};
    GDALSetGeoTransform(ds.get(), gt);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 3857);
    GDALSetSpatialRef(ds.get(), srs);
    OSRDestroySpatialReference(srs);
  }

  // 1. In GTI context, different projections without -t_srs returns failure
  {
    WithQuietHandler quiet_handler;
    const std::string gti_xml = test_dir + "/gti.xml";
    const char* src_files[] = {tile1.c_str(), tile2.c_str()};
    std::vector<const char*> argv = {
        "-gti_filename",
        gti_xml.c_str(),
        nullptr,
    };
    OptionsUniquePtr opts(
        GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, opts);

    DatasetUniquePtr ds(GDALTileIndex((test_dir + "/gti_diff_srs.shp").c_str(),
                                      2, src_files, opts.get(), nullptr));
    EXPECT_EQ(nullptr, ds);
  }

  // 2. Existing layer missing location field or missing src_srs field
  {
    const std::string missing_fld_shp = test_dir + "/missing_fld.shp";
    GDALDriverH shp_driver = GDALGetDriverByName("ESRI Shapefile");
    ASSERT_NE(nullptr, shp_driver);
    {
      DatasetUniquePtr ds(GDALCreate(shp_driver, missing_fld_shp.c_str(), 0, 0,
                                     0, GDT_Unknown, nullptr));
      ASSERT_NE(nullptr, ds);
      GDALDatasetCreateLayer(ds.get(), "missing_fld", nullptr, wkbPolygon,
                             nullptr);
    }
    WithQuietHandler quiet_handler;
    const char* src_files[] = {tile1.c_str()};
    DatasetUniquePtr ds(
        GDALTileIndex(missing_fld_shp.c_str(), 1, src_files, nullptr, nullptr));
    EXPECT_EQ(nullptr, ds);

    // Existing layer has location field, but -src_srs_name missing
    {
      DatasetUniquePtr ds_shp(GDALOpenEx(missing_fld_shp.c_str(),
                                         GDAL_OF_VECTOR | GDAL_OF_UPDATE,
                                         nullptr, nullptr, nullptr));
      ASSERT_NE(nullptr, ds_shp);
      OGRLayerH lyr = GDALDatasetGetLayer(ds_shp.get(), 0);
      OGRFieldDefnH fld = OGR_Fld_Create("location", OFTString);
      OGR_L_CreateField(lyr, fld, TRUE);
      OGR_Fld_Destroy(fld);
    }
    std::vector<const char*> argv = {
        "-src_srs_name",
        "nonexistent_srs_field",
        nullptr,
    };
    OptionsUniquePtr opts(
        GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, opts);

    DatasetUniquePtr ds2(GDALTileIndex(missing_fld_shp.c_str(), 1, src_files,
                                       opts.get(), nullptr));
    EXPECT_EQ(nullptr, ds2);
  }

  // 3. Tile with 0 width or height (VRT with 0 dimensions)
  {
    const std::string zero_vrt = test_dir + "/zero.vrt";
    const char* vrt_xml = "<VRTDataset rasterXSize=\"0\" rasterYSize=\"0\"/>";
    VSIFCloseL(VSIFileFromMemBuffer(
        zero_vrt.c_str(), reinterpret_cast<GByte*>(const_cast<char*>(vrt_xml)),
        strlen(vrt_xml), FALSE));
    WithQuietHandler quiet_handler;
    const char* src_files[] = {tile1.c_str(), zero_vrt.c_str()};
    DatasetUniquePtr ds(GDALTileIndex((test_dir + "/zero.shp").c_str(), 2,
                                      src_files, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    OGRLayerH layer = GDALDatasetGetLayer(ds.get(), 0);
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(1, OGR_L_GetFeatureCount(layer, TRUE));
  }

  // 4. Corrupt/unopenable file in multi-tile list skipped
  {
    const std::string corrupt_file = test_dir + "/corrupt.tif";
    const char* data = "invalid data";
    VSIFCloseL(VSIFileFromMemBuffer(
        corrupt_file.c_str(), reinterpret_cast<GByte*>(const_cast<char*>(data)),
        strlen(data), FALSE));
    WithQuietHandler quiet_handler;
    const char* src_files[] = {tile1.c_str(), corrupt_file.c_str()};
    DatasetUniquePtr ds(GDALTileIndex((test_dir + "/corrupt.shp").c_str(), 2,
                                      src_files, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    OGRLayerH layer = GDALDatasetGetLayer(ds.get(), 0);
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(1, OGR_L_GetFeatureCount(layer, TRUE));
  }

  // 5. Driver errors: cannot guess driver, or unknown driver
  {
    WithQuietHandler quiet_handler;
    const char* src_files[] = {tile1.c_str()};
    DatasetUniquePtr ds(GDALTileIndex((test_dir + "/unknown.foo_bar").c_str(),
                                      1, src_files, nullptr, nullptr));
    EXPECT_EQ(nullptr, ds);

    std::vector<const char*> argv = {
        "-of",
        "NONEXISTENT_DRIVER",
        nullptr,
    };
    OptionsUniquePtr opts(
        GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, opts);
    DatasetUniquePtr ds2(GDALTileIndex((test_dir + "/out.shp").c_str(), 1,
                                       src_files, opts.get(), nullptr));
    EXPECT_EQ(nullptr, ds2);
  }

  // 6. -lyr_name required for non-shapefile new file (e.g. GPKG)
  {
    WithQuietHandler quiet_handler;
    int usage_err = FALSE;
    const char* src_files[] = {tile1.c_str()};
    std::vector<const char*> argv = {
        "-of",
        "GPKG",
        nullptr,
    };
    OptionsUniquePtr opts(
        GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, opts);
    DatasetUniquePtr ds(GDALTileIndex((test_dir + "/no_lyr.gpkg").c_str(), 1,
                                      src_files, opts.get(), &usage_err));
    EXPECT_EQ(nullptr, ds);
    EXPECT_TRUE(usage_err);
  }

  // 7. Custom SRS without EPSG code with FORMAT_AUTO on Shapefile
  {
    const std::string custom_srs_tile = test_dir + "/custom_srs.tif";
    {
      DatasetUniquePtr ds(GDALCreate(gtiff_driver, custom_srs_tile.c_str(), 4,
                                     4, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds);
      double gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
      GDALSetGeoTransform(ds.get(), gt);
      OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
      OSRSetFromUserInput(srs,
                          "+proj=laea +lat_0=52 +lon_0=10 +x_0=4321000 "
                          "+y_0=3210000 +ellps=GRS80 +units=m +no_defs");
      GDALSetSpatialRef(ds.get(), srs);
      OSRDestroySpatialReference(srs);
    }
    const char* src_files[] = {custom_srs_tile.c_str()};
    std::vector<const char*> argv = {
        "-src_srs_name", "srs", "-src_srs_format", "AUTO", nullptr,
    };
    OptionsUniquePtr opts(
        GDALTileIndexOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, opts);

    DatasetUniquePtr ds(GDALTileIndex((test_dir + "/custom_srs.shp").c_str(), 1,
                                      src_files, opts.get(), nullptr));
    ASSERT_NE(nullptr, ds);
    OGRLayerH layer = GDALDatasetGetLayer(ds.get(), 0);
    ASSERT_NE(nullptr, layer);
    FeatureUniquePtr feat(OGR_L_GetNextFeature(layer));
    ASSERT_NE(nullptr, feat);
    int idx = OGR_F_GetFieldIndex(feat.get(), "srs");
    ASSERT_NE(-1, idx);
    EXPECT_STRNE("", OGR_F_GetFieldAsString(feat.get(), idx));
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover remaining unreachable/defensive branches in
// apps/gdaltindex_lib.cpp.
