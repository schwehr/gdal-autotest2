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
// Tests for GDALVectorTranslate / ogr2ogr (apps/ogr2ogr_lib.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/ogr2ogr_lib.cpp
//   https://gdal.org/programs/ogr2ogr.html

#include <algorithm>
#include <filesystem>  // NOLINT
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/gdal_utils.h"
#include "third_party/gdal/apps/gdal_utils_priv.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_relationship.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geomcoordinateprecision.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/generic/ogrlayerdecorator.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/ogr2ogr_lib_test";

struct VectorTranslateOptionsDeleter {
  void operator()(GDALVectorTranslateOptions* psOptions) const {
    GDALVectorTranslateOptionsFree(psOptions);
  }
};
using VectorTranslateOptionsUniquePtr =
    std::unique_ptr<GDALVectorTranslateOptions, VectorTranslateOptionsDeleter>;

VectorTranslateOptionsUniquePtr ParseOptions(
    const std::vector<std::string>& args,
    GDALVectorTranslateOptionsForBinary* psOptionsForBinary = nullptr) {
  CPLStringList csl;
  for (const auto& arg : args) {
    csl.AddString(arg.c_str());
  }
  return VectorTranslateOptionsUniquePtr(
      GDALVectorTranslateOptionsNew(csl.List(), psOptionsForBinary));
}

GDALDatasetUniquePtr RunTranslate(
    const char* dest, GDALDataset* src,
    const GDALVectorTranslateOptions* options = nullptr,
    int* usage_error = nullptr) {
  GDALDatasetH hSrc = GDALDataset::ToHandle(src);
  GDALDatasetH hOut =
      GDALVectorTranslate(dest, nullptr, 1, &hSrc, options, usage_error);
  return GDALDatasetUniquePtr(GDALDataset::FromHandle(hOut));
}

void WriteVsimemFile(const std::string& path, const std::string& content) {
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
  ASSERT_EQ(content.size(), VSIFWriteL(content.data(), 1, content.size(), fp));
}

GDALDatasetUniquePtr CreateMemPointDataset(const char* layer_name = "pts",
                                           int epsg = 4326) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (drv == nullptr) return nullptr;
  GDALDatasetUniquePtr ds(drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  if (ds == nullptr) return nullptr;

  OGRSpatialReference srs;
  if (epsg > 0) {
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    if (srs.importFromEPSG(epsg) != OGRERR_NONE) return nullptr;
  }

  OGRLayer* lyr =
      ds->CreateLayer(layer_name, epsg > 0 ? &srs : nullptr, wkbPoint, nullptr);
  if (lyr == nullptr) return nullptr;

  OGRFieldDefn f_id("id", OFTInteger);
  OGRFieldDefn f_name("name", OFTString);
  OGRFieldDefn f_val("val", OFTReal);
  if (lyr->CreateField(&f_id) != OGRERR_NONE ||
      lyr->CreateField(&f_name) != OGRERR_NONE ||
      lyr->CreateField(&f_val) != OGRERR_NONE) {
    return nullptr;
  }

  struct PtData {
    int id;
    const char* name;
    double val;
    double x;
    double y;
  };
  constexpr PtData kPts[] = {
      {.id = 1, .name = "alpha", .val = 10.5, .x = 1.0, .y = 2.0},
      {.id = 2, .name = "beta", .val = 20.5, .x = 5.0, .y = 6.0},
      {.id = 3, .name = "gamma", .val = 30.5, .x = 15.0, .y = 16.0},
      {.id = 4, .name = "", .val = 40.5, .x = 25.0, .y = 26.0},
  };
  for (const auto& p : kPts) {
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    feat->SetFID(p.id);
    feat->SetField("id", p.id);
    feat->SetField("name", p.name);
    feat->SetField("val", p.val);
    auto pt = std::make_unique<OGRPoint>(p.x, p.y);
    feat->SetGeometryDirectly(pt.release());
    if (lyr->CreateFeature(feat.get()) != OGRERR_NONE) return nullptr;
  }
  return ds;
}

GDALDatasetUniquePtr CreateMemPolygonDataset(const char* layer_name = "polys") {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (drv == nullptr) return nullptr;
  GDALDatasetUniquePtr ds(drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  if (ds == nullptr) return nullptr;

  OGRSpatialReference srs;
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  if (srs.importFromEPSG(4326) != OGRERR_NONE) return nullptr;

  OGRLayer* lyr = ds->CreateLayer(layer_name, &srs, wkbPolygon, nullptr);
  if (lyr == nullptr) return nullptr;

  OGRFieldDefn f_id("id", OFTInteger);
  if (lyr->CreateField(&f_id) != OGRERR_NONE) return nullptr;

  auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
  feat->SetField("id", 1);
  auto poly = std::make_unique<OGRPolygon>(0.0, 0.0, 10.0, 10.0);
  feat->SetGeometryDirectly(poly.release());
  if (lyr->CreateFeature(feat.get()) != OGRERR_NONE) return nullptr;

  return ds;
}

class Ogr2ogrLibTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    RegisterOGRGeoPackage();
    RegisterOGRGeoJSON();
    RegisterOGRSQLite();
    RegisterOGRCSV();
    RegisterOGRShape();
    RegisterOGRVRT();
    RegisterOGRGPX();
    RegisterOGRGML();
    RegisterOGROSM();
    RegisterOGROpenFileGDB();
    RegisterOGRGeoJSONSeq();
    VSIRmdirRecursive(kTestDir);
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(Ogr2ogrLibTest, OptionsDefaultFreeNullAndParserUsage) {
  VectorTranslateOptionsUniquePtr opts(
      GDALVectorTranslateOptionsNew(nullptr, nullptr));
  ASSERT_NE(nullptr, opts);
  GDALVectorTranslateOptionsFree(nullptr);

  const std::string usage = GDALVectorTranslateGetParserUsage();
  EXPECT_FALSE(usage.empty());
  EXPECT_THAT(usage, HasSubstr("ogr2ogr"));
  EXPECT_THAT(usage, HasSubstr("-where"));
  EXPECT_THAT(usage, HasSubstr("-t_srs"));
}

TEST_F(Ogr2ogrLibTest, OptionsParsingValidFlagsAndBinary) {
  EXPECT_NE(nullptr, ParseOptions({"-f", "GPKG", "-dsco", "VERSION=1.2", "-lco",
                                   "SPATIAL_INDEX=NO", "-overwrite"}));
  EXPECT_NE(nullptr, ParseOptions({"-append", "-update"}));
  EXPECT_NE(nullptr, ParseOptions({"-upsert", "-addfields"}));
  EXPECT_NE(nullptr,
            ParseOptions({"-sql", "SELECT * FROM pts", "-dialect", "SQLITE",
                          "-where", "id > 0", "-select", "id,name", "-nln",
                          "out_lyr", "-relaxedFieldNameMatch"}));
  EXPECT_NE(nullptr, ParseOptions({"-nlt", "POINT", "-nlt", "PROMOTE_TO_MULTI",
                                   "-nlt", "CONVERT_TO_LINEAR"}));
  EXPECT_NE(nullptr, ParseOptions({"-nlt", "POLYGON25D"}));
  EXPECT_NE(nullptr, ParseOptions({"-nlt", "LINESTRINGZ"}));
  EXPECT_NE(nullptr, ParseOptions({"-nlt", "NONE"}));
  EXPECT_NE(nullptr, ParseOptions({"-nlt", "GEOMETRY"}));
  EXPECT_NE(nullptr, ParseOptions({"-nlt", "CONVERT_TO_CURVE"}));
  EXPECT_NE(nullptr, ParseOptions({"-s_srs", "EPSG:4326", "-t_srs", "EPSG:3857",
                                   "-s_coord_epoch", "2020.0", "-t_coord_epoch",
                                   "2021.0", "-ct_opt", "ALLOW_BALLPARK=YES"}));
  EXPECT_NE(nullptr,
            ParseOptions({"-a_srs", "NONE", "-a_coord_epoch", "2020.5"}));
  EXPECT_NE(nullptr, ParseOptions({"-ct", "+proj=noop"}));
  EXPECT_NE(nullptr, ParseOptions({"-spat", "0", "0", "10", "10", "-spat_srs",
                                   "EPSG:4326", "-geomfield", "geom"}));
  EXPECT_NE(nullptr,
            ParseOptions({"-fieldTypeToString", "All", "-unsetFieldWidth",
                          "-emptyStrAsNull", "-forceNullable", "-unsetDefault",
                          "-resolveDomains", "-noNativeData"}));
  EXPECT_NE(nullptr, ParseOptions({"-fieldTypeToString", "Integer,Real"}));
  EXPECT_NE(nullptr,
            ParseOptions({"-mapFieldType", "Integer=Real,All=String"}));
  EXPECT_NE(nullptr, ParseOptions({"-fieldmap", "0,1", "-splitlistfields",
                                   "-maxsubfields", "3"}));
  EXPECT_NE(nullptr, ParseOptions({"-dateTimeTo", "UTC"}));
  EXPECT_NE(nullptr, ParseOptions({"-dateTimeTo", "UTC+02"}));
  EXPECT_NE(nullptr, ParseOptions({"-dateTimeTo", "UTC-05:30"}));
  EXPECT_NE(nullptr, ParseOptions({"-dim", "layer_dim"}));
  EXPECT_NE(nullptr, ParseOptions({"-dim", "XY"}));
  EXPECT_NE(nullptr, ParseOptions({"-dim", "XYZ"}));
  EXPECT_NE(nullptr, ParseOptions({"-dim", "XYM"}));
  EXPECT_NE(nullptr, ParseOptions({"-dim", "XYZM"}));
  EXPECT_NE(nullptr,
            ParseOptions({"-segmentize", "1.0", "-makevalid", "-skipinvalid",
                          "-wrapdateline", "-datelineoffset", "15"}));
  EXPECT_NE(nullptr, ParseOptions({"-simplify", "0.5", "-explodecollections",
                                   "-zfield", "val"}));
  EXPECT_NE(nullptr, ParseOptions({"-clipsrc", "0", "0", "10", "10",
                                   "-clipsrcsql", "SELECT 1", "-clipsrclayer",
                                   "l", "-clipsrcwhere", "id=1"}));
  EXPECT_NE(nullptr,
            ParseOptions({"-clipsrc", "POLYGON((0 0,0 1,1 1,1 0,0 0))"}));
  EXPECT_NE(nullptr, ParseOptions({"-clipsrc", "spat_extent"}));
  EXPECT_NE(nullptr, ParseOptions({"-clipdst", "0", "0", "10", "10",
                                   "-clipdstsql", "SELECT 1", "-clipdstlayer",
                                   "l", "-clipdstwhere", "id=1"}));
  EXPECT_NE(nullptr,
            ParseOptions({"-clipdst", "POLYGON((0 0,0 1,1 1,1 0,0 0))"}));
  EXPECT_NE(nullptr, ParseOptions({"-gcp", "0", "0", "10", "20", "-gcp", "1",
                                   "1", "11", "21", "5", "-tps"}));
  EXPECT_NE(nullptr,
            ParseOptions({"-order", "1", "-xyRes", "0.01 m", "-zRes", "1 mm",
                          "-mRes", "0.1", "-unsetCoordPrecision"}));
  EXPECT_NE(nullptr, ParseOptions({"-xyRes", "0.001 deg"}));
  EXPECT_NE(nullptr,
            ParseOptions({"-q", "-progress", "-doo", "OPT=VAL", "-fid", "1",
                          "-preserve_fid", "-unsetFid", "-skipfailures",
                          "-limit", "10", "-ds_transaction", "-lyr_transaction",
                          "-mo", "KEY=VAL", "-nomd", "--no-overwrite",
                          "--invoked-from-gdal-vector-convert"}));
  EXPECT_NE(nullptr, ParseOptions({"-gt", "unlimited"}));
  EXPECT_NE(nullptr, ParseOptions({"-gt", "500"}));

  // OptionsForBinary and @file syntax
  const std::string sql_file = std::string(kTestDir) + "/q.sql";
  const std::string where_file = std::string(kTestDir) + "/w.txt";
  ASSERT_NO_FATAL_FAILURE(
      WriteVsimemFile(sql_file, "-- comment\nSELECT * FROM pts"));
  ASSERT_NO_FATAL_FAILURE(WriteVsimemFile(where_file, "id = 1"));

  GDALVectorTranslateOptionsForBinary bin_opts;
  auto opts = ParseOptions({"-f", "GPKG", "-append", "-sql", "@" + sql_file,
                            "-where", "@" + where_file, "-if", "MEM", "-oo",
                            "FOO=BAR", "out.gpkg", "in.mem", "pts"},
                           &bin_opts);
  ASSERT_NE(nullptr, opts);
  EXPECT_EQ(ACCESS_APPEND, bin_opts.eAccessMode);
  EXPECT_EQ("GPKG", bin_opts.osFormat);
  EXPECT_EQ("out.gpkg", bin_opts.osDestDataSource);
  EXPECT_EQ("in.mem", bin_opts.osDataSource);
  EXPECT_EQ(1, bin_opts.aosAllowInputDrivers.size());
}

TEST_F(Ogr2ogrLibTest, OptionsParsingErrors) {
  const std::vector<std::vector<std::string>> usage_error_cases = {
      {"--unknown-option-xyz"},
      {"-nlt", "INVALID_GEOM_TYPE"},
      {"-nlt", "POINT", "-nlt", "POLYGON"},
      {"-nlt", "CONVERT_TO_CURVE", "-nlt", "CONVERT_TO_LINEAR"},
      {"-fieldTypeToString", "BadType"},
      {"-mapFieldType", "BadSrc=String"},
      {"-dateTimeTo", "UTC+99"},
      {"-dateTimeTo", "INVALID"},
      {"-dim", "INVALID"},
      {"-xyRes", "1.0badunit"},
      {"-zRes", "1.0badunit"},
  };

  for (const auto& args : usage_error_cases) {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorTranslateOptionsForBinary bin_opts;
    EXPECT_EQ(nullptr, ParseOptions(args, &bin_opts));
    EXPECT_TRUE(bin_opts.bShowUsageIfError);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Errors detected during pre-processing or post-parse validation return
  // nullptr with CE_Failure without setting bShowUsageIfError.
  const std::vector<std::vector<std::string>> other_error_cases = {
      {"-gcp", "0", "0"},
      {"-clipsrc"},
      {"-clipdst"},
      {"-clipsrc", "POLYGON((0 0,0 1,1 1,1 0,0 0))", "-clipsrc",
       "POLYGON((0 0,0 1,1 1,1 0,0 0))"},
      {"-clipdst", "POLYGON((0 0,0 1,1 1,1 0,0 0))", "-clipdst",
       "POLYGON((0 0,0 1,1 1,1 0,0 0))"},
      {"-clipsrc", "POLYGON(INVALID)", "dst", "src"},
      {"-clipdst", "POLYGON(INVALID)", "dst", "src"},
  };

  for (const auto& args : other_error_cases) {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorTranslateOptionsForBinary bin_opts;
    EXPECT_EQ(nullptr, ParseOptions(args, &bin_opts));
    EXPECT_FALSE(bin_opts.bShowUsageIfError);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST_F(Ogr2ogrLibTest, FormatConversionsGpkgGeoJsonSqliteCsvMem) {
  auto src = CreateMemPointDataset("pts", 4326);
  ASSERT_NE(nullptr, src);

  // 1. MEM -> GPKG
  const std::string gpkg_path = std::string(kTestDir) + "/pts.gpkg";
  {
    auto opts = ParseOptions({"-f", "GPKG", "-overwrite"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate(gpkg_path.c_str(), src.get(), opts.get());
    ASSERT_NE(nullptr, out);
    out.reset();

    std::unique_ptr<GDALDataset> reopened(
        GDALDataset::Open(gpkg_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, reopened);
    ASSERT_EQ(1, reopened->GetLayerCount());
    EXPECT_EQ(4, reopened->GetLayer(0)->GetFeatureCount());
  }

  // 2. GPKG -> GeoJSON
  const std::string geojson_path = std::string(kTestDir) + "/pts.geojson";
  {
    std::unique_ptr<GDALDataset> gpkg_in(
        GDALDataset::Open(gpkg_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, gpkg_in);
    auto opts = ParseOptions({"-f", "GeoJSON"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate(geojson_path.c_str(), gpkg_in.get(), opts.get());
    ASSERT_NE(nullptr, out);
    out.reset();

    std::unique_ptr<GDALDataset> reopened(GDALDataset::Open(
        geojson_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
        nullptr, nullptr));
    ASSERT_NE(nullptr, reopened);
    EXPECT_EQ(4, reopened->GetLayer(0)->GetFeatureCount());
  }

  // 3. GeoJSON -> SQLite
  const std::string sqlite_path = std::string(kTestDir) + "/pts.sqlite";
  {
    std::unique_ptr<GDALDataset> gj_in(GDALDataset::Open(
        geojson_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
        nullptr, nullptr));
    ASSERT_NE(nullptr, gj_in);
    auto opts = ParseOptions({"-f", "SQLite"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate(sqlite_path.c_str(), gj_in.get(), opts.get());
    ASSERT_NE(nullptr, out);
    out.reset();

    std::unique_ptr<GDALDataset> reopened(GDALDataset::Open(
        sqlite_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
        nullptr, nullptr));
    ASSERT_NE(nullptr, reopened);
    EXPECT_EQ(4, reopened->GetLayer(0)->GetFeatureCount());
  }

  // 4. MEM -> CSV
  const std::string csv_path = std::string(kTestDir) + "/pts.csv";
  {
    auto opts = ParseOptions({"-f", "CSV", "-lco", "GEOMETRY=AS_WKT"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate(csv_path.c_str(), src.get(), opts.get());
    ASSERT_NE(nullptr, out);
    out.reset();

    std::unique_ptr<GDALDataset> reopened(
        GDALDataset::Open(csv_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, reopened);
    EXPECT_EQ(4, reopened->GetLayer(0)->GetFeatureCount());
  }

  // 5. MEM -> MEM and existing hDstDS
  {
    auto opts = ParseOptions({"-f", "MEM"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", src.get(), opts.get());
    ASSERT_NE(nullptr, out);
    ASSERT_EQ(1, out->GetLayerCount());
    EXPECT_EQ(4, out->GetLayer(0)->GetFeatureCount());

    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_NE(nullptr, mem_drv);
    GDALDatasetUniquePtr existing_dst(
        mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, existing_dst);
    auto opts_nln = ParseOptions({"-nln", "copied_pts"});
    ASSERT_NE(nullptr, opts_nln);
    GDALDatasetH hSrc = GDALDataset::ToHandle(src.get());
    GDALDatasetH hRet =
        GDALVectorTranslate(nullptr, GDALDataset::ToHandle(existing_dst.get()),
                            1, &hSrc, opts_nln.get(), nullptr);
    EXPECT_EQ(GDALDataset::ToHandle(existing_dst.get()), hRet);
    EXPECT_NE(nullptr, existing_dst->GetLayerByName("copied_pts"));
  }
}

TEST_F(Ogr2ogrLibTest, FilteringSelectSqlSpatFidAndLimit) {
  auto src = CreateMemPointDataset("pts", 4326);
  ASSERT_NE(nullptr, src);

  // 1. -where and -select
  {
    auto opts = ParseOptions(
        {"-f", "MEM", "-where", "id >= 2 AND id <= 3", "-select", "name,val"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", src.get(), opts.get());
    ASSERT_NE(nullptr, out);
    OGRLayer* lyr = out->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(2, lyr->GetFeatureCount());
    EXPECT_EQ(2, lyr->GetLayerDefn()->GetFieldCount());
    EXPECT_LT(lyr->GetLayerDefn()->GetFieldIndex("id"), 0);
  }

  // 2. -sql with OGRSQL and SQLITE dialects
  {
    auto opts = ParseOptions({"-f", "MEM", "-sql",
                              "SELECT id, name FROM pts WHERE id = 2",
                              "-dialect", "OGRSQL", "-nln", "filtered"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", src.get(), opts.get());
    ASSERT_NE(nullptr, out);
    OGRLayer* lyr = out->GetLayerByName("filtered");
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(1, lyr->GetFeatureCount());
  }

  {
    auto opts =
        ParseOptions({"-f", "MEM", "-sql",
                      "SELECT id, val FROM pts ORDER BY id DESC LIMIT 2",
                      "-dialect", "SQLITE"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", src.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(2, out->GetLayer(0)->GetFeatureCount());
  }

  // 3. -spat and -spat_srs
  {
    auto opts = ParseOptions({"-f", "MEM", "-spat", "0", "0", "10", "10"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", src.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(2, out->GetLayer(0)->GetFeatureCount());
  }

  {
    // EPSG:3857 bounding box covering roughly (0..10, 0..10) in EPSG:4326.
    auto opts = ParseOptions({"-f", "MEM", "-spat", "0", "0", "1113200",
                              "1118890", "-spat_srs", "EPSG:3857"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", src.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(2, out->GetLayer(0)->GetFeatureCount());
  }

  // 4. -fid and -limit
  {
    auto opts = ParseOptions({"-f", "MEM", "-fid", "3"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", src.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(1, out->GetLayer(0)->GetFeatureCount());
  }

  {
    auto opts = ParseOptions({"-f", "MEM", "-limit", "2"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", src.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(2, out->GetLayer(0)->GetFeatureCount());
  }
}

TEST_F(Ogr2ogrLibTest, ReprojectionAndClipAndGeometryOps) {
  auto src = CreateMemPointDataset("pts", 4326);
  ASSERT_NE(nullptr, src);

  // 1. -t_srs EPSG:3857
  {
    auto opts = ParseOptions({"-f", "MEM", "-t_srs", "EPSG:3857"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", src.get(), opts.get());
    ASSERT_NE(nullptr, out);
    OGRLayer* lyr = out->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    ASSERT_NE(nullptr, lyr->GetSpatialRef());
    EXPECT_STREQ("3857", lyr->GetSpatialRef()->GetAuthorityCode(nullptr));
    lyr->ResetReading();
    OGRFeatureUniquePtr feat(lyr->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    const OGRPoint* pt = feat->GetGeometryRef()->toPoint();
    EXPECT_GT(pt->getX(), 100000.0);
    EXPECT_GT(pt->getY(), 200000.0);
  }

  // 2. -a_srs NONE
  {
    auto opts = ParseOptions({"-f", "MEM", "-a_srs", "NONE"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", src.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(nullptr, out->GetLayer(0)->GetSpatialRef());
  }

  // 3. -clipsrc bbox and -clipdst WKT on polygons
  auto poly_ds = CreateMemPolygonDataset("polys");
  ASSERT_NE(nullptr, poly_ds);
  {
    auto opts = ParseOptions({"-f", "MEM", "-clipsrc", "2", "2", "6", "6"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", poly_ds.get(), opts.get());
    ASSERT_NE(nullptr, out);
    OGRLayer* lyr = out->GetLayer(0);
    lyr->ResetReading();
    OGRFeatureUniquePtr feat(lyr->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    OGREnvelope env;
    feat->GetGeometryRef()->getEnvelope(&env);
    EXPECT_DOUBLE_EQ(2.0, env.MinX);
    EXPECT_DOUBLE_EQ(6.0, env.MaxX);
  }

  {
    auto opts = ParseOptions(
        {"-f", "MEM", "-clipdst", "POLYGON((1 1,1 5,5 5,5 1,1 1))"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", poly_ds.get(), opts.get());
    ASSERT_NE(nullptr, out);
    OGRLayer* lyr = out->GetLayer(0);
    lyr->ResetReading();
    OGRFeatureUniquePtr feat(lyr->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    OGREnvelope env;
    feat->GetGeometryRef()->getEnvelope(&env);
    EXPECT_DOUBLE_EQ(1.0, env.MinX);
    EXPECT_DOUBLE_EQ(5.0, env.MaxX);
  }

  // 4. -clipsrc from dataset file
  const std::string clip_path = std::string(kTestDir) + "/clip.geojson";
  {
    auto opts_save = ParseOptions({"-f", "GeoJSON"});
    ASSERT_NE(nullptr, opts_save);
    auto saved =
        RunTranslate(clip_path.c_str(), poly_ds.get(), opts_save.get());
    ASSERT_NE(nullptr, saved);
    saved.reset();

    auto opts = ParseOptions({"-f", "MEM", "-clipsrc", clip_path});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", src.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(2, out->GetLayer(0)->GetFeatureCount());
  }

  // 5. -nlt PROMOTE_TO_MULTI, -dim 3, -zfield, -segmentize, -simplify
  {
    auto opts = ParseOptions({"-f", "MEM", "-nlt", "PROMOTE_TO_MULTI", "-dim",
                              "3", "-zfield", "val"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", src.get(), opts.get());
    ASSERT_NE(nullptr, out);
    OGRLayer* lyr = out->GetLayer(0);
    EXPECT_EQ(wkbMultiPoint25D, lyr->GetGeomType());
    lyr->ResetReading();
    OGRFeatureUniquePtr feat(lyr->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    const OGRMultiPoint* mp = feat->GetGeometryRef()->toMultiPoint();
    ASSERT_EQ(1, mp->getNumGeometries());
    EXPECT_DOUBLE_EQ(10.5, mp->getGeometryRef(0)->toPoint()->getZ());
  }

  {
    auto opts =
        ParseOptions({"-f", "MEM", "-segmentize", "2.0", "-simplify", "0.1"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", poly_ds.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(1, out->GetLayer(0)->GetFeatureCount());
  }

  // 6. -explodecollections
  {
    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    GDALDatasetUniquePtr mp_ds(
        mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* lyr = mp_ds->CreateLayer("mp", nullptr, wkbMultiPoint, nullptr);
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    OGRMultiPoint mp;
    OGRPoint p1(1.0, 2.0);
    OGRPoint p2(3.0, 4.0);
    mp.addGeometry(&p1);
    mp.addGeometry(&p2);
    feat->SetGeometry(&mp);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));

    auto opts = ParseOptions({"-f", "MEM", "-explodecollections"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", mp_ds.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(2, out->GetLayer(0)->GetFeatureCount());
  }

  // 7. -gcp + -order 1
  {
    auto opts = ParseOptions({"-f", "MEM", "-gcp", "0", "0", "10", "20", "-gcp",
                              "10", "0", "20", "20", "-gcp", "0", "10", "10",
                              "30", "-order", "1"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", src.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(4, out->GetLayer(0)->GetFeatureCount());
  }
}

TEST_F(Ogr2ogrLibTest, OverwriteAppendUpdateAndUpsertModes) {
  auto src = CreateMemPointDataset("pts", 4326);
  ASSERT_NE(nullptr, src);
  const std::string gpkg_path = std::string(kTestDir) + "/modes.gpkg";

  // 1. Create initial GPKG
  {
    auto opts = ParseOptions({"-f", "GPKG"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate(gpkg_path.c_str(), src.get(), opts.get());
    ASSERT_NE(nullptr, out);
  }

  // 2. -append
  {
    auto opts = ParseOptions({"-append", "-fieldmap", "identity"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate(gpkg_path.c_str(), src.get(), opts.get());
    ASSERT_NE(nullptr, out);
    out.reset();

    std::unique_ptr<GDALDataset> reopened(
        GDALDataset::Open(gpkg_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, reopened);
    EXPECT_EQ(8, reopened->GetLayer(0)->GetFeatureCount());
  }

  // 3. -overwrite
  {
    auto opts = ParseOptions({"-overwrite"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate(gpkg_path.c_str(), src.get(), opts.get());
    ASSERT_NE(nullptr, out);
    out.reset();

    std::unique_ptr<GDALDataset> reopened(
        GDALDataset::Open(gpkg_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, reopened);
    EXPECT_EQ(4, reopened->GetLayer(0)->GetFeatureCount());
  }

  // 4. -update adding second layer with -nln
  {
    auto opts = ParseOptions({"-update", "-nln", "pts_second"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate(gpkg_path.c_str(), src.get(), opts.get());
    ASSERT_NE(nullptr, out);
    out.reset();

    std::unique_ptr<GDALDataset> reopened(
        GDALDataset::Open(gpkg_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, reopened);
    EXPECT_EQ(2, reopened->GetLayerCount());
    EXPECT_NE(nullptr, reopened->GetLayerByName("pts_second"));
  }

  // 5. -upsert on GPKG
  {
    auto opts = ParseOptions({"-upsert", "-preserve_fid"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate(gpkg_path.c_str(), src.get(), opts.get());
    ASSERT_NE(nullptr, out);
    out.reset();

    std::unique_ptr<GDALDataset> reopened(
        GDALDataset::Open(gpkg_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, reopened);
    EXPECT_EQ(4, reopened->GetLayerByName("pts")->GetFeatureCount());
  }
}

TEST_F(Ogr2ogrLibTest, FieldConversionsSplitListAndDomains) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  std::vector<OGRCodedValue> codes;
  codes.push_back({CPLStrdup("1"), CPLStrdup("One")});
  codes.push_back({CPLStrdup("2"), CPLStrdup("Two")});
  codes.push_back({nullptr, nullptr});
  std::string failure_reason;
  ASSERT_TRUE(ds->AddFieldDomain(
      std::make_unique<OGRCodedFieldDomain>("status_dom", "Status", OFTInteger,
                                            OFSTNone, std::move(codes)),
      failure_reason));

  OGRLayer* lyr = ds->CreateLayer("items", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, lyr);

  OGRFieldDefn f_status("status", OFTInteger);
  f_status.SetDomainName("status_dom");
  OGRFieldDefn f_ints("ints", OFTIntegerList);
  OGRFieldDefn f_strs("strs", OFTStringList);
  OGRFieldDefn f_empty("empty_str", OFTString);
  OGRFieldDefn f_dt("dt", OFTDateTime);
  ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_status));
  ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_ints));
  ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_strs));
  ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_empty));
  ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_dt));

  auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
  feat->SetField("status", 1);
  const int int_vals[] = {10, 20};
  feat->SetField("ints", 2, const_cast<int*>(int_vals));
  const char* const str_vals[] = {"a", "bb", nullptr};
  feat->SetField("strs", const_cast<char**>(str_vals));
  feat->SetField("empty_str", "");
  feat->SetField("dt", 2025, 6, 15, 12, 0, 0.0f, 100);
  ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));

  // 1. -splitlistfields -resolveDomains -emptyStrAsNull -dateTimeTo UTC+02
  {
    auto opts =
        ParseOptions({"-f", "MEM", "-splitlistfields", "-resolveDomains",
                      "-emptyStrAsNull", "-dateTimeTo", "UTC+02"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", ds.get(), opts.get());
    ASSERT_NE(nullptr, out);
    OGRLayer* out_lyr = out->GetLayer(0);
    ASSERT_NE(nullptr, out_lyr);
    EXPECT_GE(out_lyr->GetLayerDefn()->GetFieldIndex("status_resolved"), 0);
    EXPECT_GE(out_lyr->GetLayerDefn()->GetFieldIndex("ints1"), 0);
    EXPECT_GE(out_lyr->GetLayerDefn()->GetFieldIndex("ints2"), 0);
    EXPECT_GE(out_lyr->GetLayerDefn()->GetFieldIndex("strs1"), 0);
    out_lyr->ResetReading();
    OGRFeatureUniquePtr out_feat(out_lyr->GetNextFeature());
    ASSERT_NE(nullptr, out_feat);
    EXPECT_STREQ("One", out_feat->GetFieldAsString("status_resolved"));
    EXPECT_TRUE(out_feat->IsFieldNull(
        out_lyr->GetLayerDefn()->GetFieldIndex("empty_str")));
    int yr = 0, mo = 0, da = 0, hr = 0, mi = 0, tz = 0;
    float sc = 0.0f;
    EXPECT_TRUE(out_feat->GetFieldAsDateTime(
        out_lyr->GetLayerDefn()->GetFieldIndex("dt"), &yr, &mo, &da, &hr, &mi,
        &sc, &tz));
    EXPECT_EQ(14, hr);
  }

  // 2. -fieldTypeToString Integer and -mapFieldType
  {
    auto opts = ParseOptions({"-f", "MEM", "-select", "status,empty_str",
                              "-fieldTypeToString", "Integer"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", ds.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(OFTString,
              out->GetLayer(0)->GetLayerDefn()->GetFieldDefn(0)->GetType());
  }

  {
    auto opts = ParseOptions(
        {"-f", "MEM", "-select", "status", "-mapFieldType", "Integer=Real"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", ds.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(OFTReal,
              out->GetLayer(0)->GetLayerDefn()->GetFieldDefn(0)->GetType());
  }
}

TEST_F(Ogr2ogrLibTest, ProgressCallbackArrowApiAndMetadata) {
  auto src = CreateMemPointDataset("pts", 4326);
  ASSERT_NE(nullptr, src);
  const std::string gpkg1 = std::string(kTestDir) + "/arrow_in.gpkg";
  const std::string gpkg2 = std::string(kTestDir) + "/arrow_out.gpkg";

  // 1. Progress callback recording completion
  int progress_calls = 0;
  auto progress_cb = [](double /*complete*/, const char* /*msg*/, void* data) {
    ++(*static_cast<int*>(data));
    return TRUE;
  };
  {
    auto opts = ParseOptions({"-f", "GPKG", "-progress", "-mo", "AUTHOR=TEST"});
    ASSERT_NE(nullptr, opts);
    GDALVectorTranslateOptionsSetProgress(opts.get(), progress_cb,
                                          &progress_calls);
    auto out = RunTranslate(gpkg1.c_str(), src.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_GT(progress_calls, 0);
    EXPECT_STREQ("TEST", out->GetMetadataItem("AUTHOR"));
  }

  // 2. Progress cancellation returns nullptr
  {
    auto cancel_cb = [](double, const char*, void*) { return FALSE; };
    auto opts = ParseOptions({"-f", "MEM", "-progress"});
    ASSERT_NE(nullptr, opts);
    GDALVectorTranslateOptionsSetProgress(opts.get(), cancel_cb, nullptr);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto out = RunTranslate("", src.get(), opts.get());
    EXPECT_EQ(nullptr, out);
    CPLErrorReset();
  }

  // 3. Arrow batch path with GPKG -> GPKG reprojection
  {
    CPLConfigOptionSetter arrow_opt("OGR2OGR_USE_ARROW_API", "YES", false);
    CPLConfigOptionSetter thr_opt("OGR2OGR_MIN_FEATURES_FOR_THREADED_REPROJ",
                                  "1", false);
    std::unique_ptr<GDALDataset> gpkg_in(
        GDALDataset::Open(gpkg1.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, gpkg_in);

    auto opts = ParseOptions({"-f", "GPKG", "-t_srs", "EPSG:3857"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate(gpkg2.c_str(), gpkg_in.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(4, out->GetLayer(0)->GetFeatureCount());
  }
}

TEST_F(Ogr2ogrLibTest, ValidationAndRuntimeErrorBranches) {
  auto src = CreateMemPointDataset("pts", 4326);
  ASSERT_NE(nullptr, src);
  GDALDatasetH hSrc = GDALDataset::ToHandle(src.get());

  // 1. Null dest + null hDstDS
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    int usage_err = 0;
    EXPECT_EQ(nullptr, GDALVectorTranslate(nullptr, nullptr, 1, &hSrc, nullptr,
                                           &usage_err));
    EXPECT_EQ(TRUE, usage_err);
    CPLErrorReset();
  }

  // 2. nSrcCount != 1 and null srcDS
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    int usage_err = 0;
    EXPECT_EQ(nullptr,
              GDALVectorTranslate("", nullptr, 0, &hSrc, nullptr, &usage_err));
    EXPECT_EQ(TRUE, usage_err);
    GDALDatasetH hNull = nullptr;
    usage_err = 0;
    EXPECT_EQ(nullptr,
              GDALVectorTranslate("", nullptr, 1, &hNull, nullptr, &usage_err));
    EXPECT_EQ(TRUE, usage_err);
    CPLErrorReset();
  }

  // 3. Mutually exclusive / usage error combinations in GDALVectorTranslate
  const std::vector<std::vector<std::string>> usage_err_opts = {
      {"-preserve_fid", "-explodecollections"},
      {"-fieldmap", "0,1"},
      {"-fieldmap", "0,1", "-addfields"},
      {"-select", "id", "-append"},
      {"-fieldTypeToString", "Integer", "-mapFieldType", "Real=String"},
      {"-s_srs", "EPSG:4326"},
      {"-clipsrc", "spat_extent"},
  };
  for (const auto& args : usage_err_opts) {
    auto opts = ParseOptions(args);
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    int usage_err = 0;
    EXPECT_EQ(nullptr, RunTranslate("", src.get(), opts.get(), &usage_err));
    EXPECT_EQ(TRUE, usage_err);
    CPLErrorReset();
  }

  // 4. --no-overwrite when output already exists
  const std::string existing_gpkg = std::string(kTestDir) + "/existing.gpkg";
  {
    auto opts = ParseOptions({"-f", "GPKG"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate(existing_gpkg.c_str(), src.get(), opts.get());
    ASSERT_NE(nullptr, out);
    out.reset();

    auto no_ow_opts = ParseOptions({"-f", "GPKG", "--no-overwrite"});
    ASSERT_NE(nullptr, no_ow_opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              RunTranslate(existing_gpkg.c_str(), src.get(), no_ow_opts.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 5. Unknown output driver and raster-only output driver
  {
    auto opts = ParseOptions({"-f", "NON_EXISTENT_DRIVER"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, RunTranslate("/vsimem/ogr2ogr_lib_test/bad.out",
                                    src.get(), opts.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  {
    auto opts = ParseOptions({"-f", "GTiff"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, RunTranslate("/vsimem/ogr2ogr_lib_test/bad.tif",
                                    src.get(), opts.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 6. Invalid -spat_srs and -spat_srs with -sql
  {
    auto opts =
        ParseOptions({"-f", "MEM", "-spat", "0", "0", "1", "1", "-spat_srs",
                      "EPSG:4326", "-sql", "SELECT * FROM pts"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, RunTranslate("", src.get(), opts.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  {
    auto opts = ParseOptions({"-f", "MEM", "-spat", "0", "0", "1", "1",
                              "-spat_srs", "INVALID_SRS_DEF"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, RunTranslate("", src.get(), opts.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST_F(Ogr2ogrLibTest, OptionsParsingSubtypesAliasesAndErrors) {
  // 1. Field subtype mapping, -gt (upstream registered hidden alias as "tg"
  // without leading '-' so "-tg" is rejected), and -gcp 4 vs 5 args.
  EXPECT_NE(nullptr, ParseOptions({"-mapFieldType",
                                   "Integer=Integer(Int16),Real=Real(Float32)",
                                   "-gt", "10"}));
  {
    GDALVectorTranslateOptionsForBinary bin_opts;
    auto opts = ParseOptions({"-gcp", "0", "0", "10", "20", "-gcp", "1", "1",
                              "11", "21", "5", "out.gpkg", "in.gpkg"},
                             &bin_opts);
    ASSERT_NE(nullptr, opts);
    EXPECT_EQ("out.gpkg", bin_opts.osDestDataSource);
    EXPECT_EQ("in.gpkg", bin_opts.osDataSource);
  }
  {
    GDALVectorTranslateOptionsForBinary bin_opts;
    auto opts = ParseOptions(
        {"-gcp", "0", "0", "10", "20", "out.gpkg", "in.gpkg"}, &bin_opts);
    ASSERT_NE(nullptr, opts);
    EXPECT_EQ("out.gpkg", bin_opts.osDestDataSource);
  }

  // 2. Additional parser error cases.
  const std::vector<std::vector<std::string>> usage_err_cases = {
      {"-mapFieldType", "Integer=Integer(BadSubtype)"},
      {"-dateTimeTo", "UTC+02:11"},
      {"-xyRes", "1.0foo"},
      {"-zRes", "1.0foo"},
  };
  for (const auto& args : usage_err_cases) {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorTranslateOptionsForBinary bin_opts;
    EXPECT_EQ(nullptr, ParseOptions(args, &bin_opts));
    EXPECT_TRUE(bin_opts.bShowUsageIfError);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST_F(Ogr2ogrLibTest, ClipSrcAndClipDstVariants) {
  auto src_pts = CreateMemPointDataset("pts", 4326);
  ASSERT_NE(nullptr, src_pts);

  // Build clip datasets in /vsimem.
  const std::string clip_multi = std::string(kTestDir) + "/clip_multi.gpkg";
  {
    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    GDALDatasetUniquePtr mds(
        mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRSpatialReference srs;
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
    OGRLayer* la = mds->CreateLayer("layer_a", &srs, wkbPolygon, nullptr);
    OGRLayer* lb = mds->CreateLayer("layer_b", &srs, wkbPolygon, nullptr);
    OGRFieldDefn fid_fld("cid", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, la->CreateField(&fid_fld));
    ASSERT_EQ(OGRERR_NONE, lb->CreateField(&fid_fld));

    auto add_poly = [](OGRLayer* lyr, int cid, double x0, double y0, double x1,
                       double y1) {
      auto f = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
      f->SetField("cid", cid);
      auto p = std::make_unique<OGRPolygon>(x0, y0, x1, y1);
      f->SetGeometryDirectly(p.release());
      EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(f.get()));
    };
    add_poly(la, 1, 0.0, 0.0, 10.0, 10.0);
    add_poly(la, 2, 20.0, 20.0, 30.0, 30.0);
    add_poly(lb, 1, 0.0, 0.0, 6.0, 6.0);

    auto save_opts = ParseOptions({"-f", "GPKG"});
    ASSERT_NE(nullptr, save_opts);
    auto saved = RunTranslate(clip_multi.c_str(), mds.get(), save_opts.get());
    ASSERT_NE(nullptr, saved);
  }

  // 1. -clipsrcsql, -clipsrclayer, -clipsrcwhere
  {
    auto opts =
        ParseOptions({"-f", "MEM", "-clipsrc", clip_multi, "-clipsrcsql",
                      "SELECT * FROM layer_a WHERE cid = 1"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", src_pts.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(2, out->GetLayer(0)->GetFeatureCount());
  }
  {
    auto opts = ParseOptions(
        {"-f", "MEM", "-clipsrc", clip_multi, "-clipsrclayer", "layer_b"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", src_pts.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(2, out->GetLayer(0)->GetFeatureCount());
  }
  {
    auto opts =
        ParseOptions({"-f", "MEM", "-clipsrc", clip_multi, "-clipsrclayer",
                      "layer_a", "-clipsrcwhere", "cid = 2"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", src_pts.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(1, out->GetLayer(0)->GetFeatureCount());
  }

  // 2. -clipdstsql, -clipdstlayer, -clipdstwhere
  {
    auto opts =
        ParseOptions({"-f", "MEM", "-clipdst", clip_multi, "-clipdstsql",
                      "SELECT * FROM layer_a WHERE cid = 1"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", src_pts.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(2, out->GetLayer(0)->GetFeatureCount());
  }
  {
    auto opts =
        ParseOptions({"-f", "MEM", "-clipdst", clip_multi, "-clipdstlayer",
                      "layer_a", "-clipdstwhere", "cid = 1"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", src_pts.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(2, out->GetLayer(0)->GetFeatureCount());
  }

  // 3. -clipsrc spat_extent with -spat and -spat_srs
  {
    auto opts =
        ParseOptions({"-f", "MEM", "-spat", "0", "0", "1113200", "1118890",
                      "-spat_srs", "EPSG:3857", "-clipsrc", "spat_extent"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", src_pts.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(2, out->GetLayer(0)->GetFeatureCount());
  }

  // 4. Invalid WKT in -clipsrc and -clipdst: fails without -makevalid, succeeds
  // with it.
  constexpr char kBowtieWkt[] = "POLYGON((0 0,10 10,10 0,0 10,0 0))";
  for (const char* flag : {"-clipsrc", "-clipdst"}) {
    {
      auto opts = ParseOptions({"-f", "MEM", flag, kBowtieWkt});
      ASSERT_NE(nullptr, opts);
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(nullptr, RunTranslate("", src_pts.get(), opts.get()));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      CPLErrorReset();
    }
    {
      auto opts = ParseOptions({"-f", "MEM", flag, kBowtieWkt, "-makevalid"});
      ASSERT_NE(nullptr, opts);
      WithQuietHandler quiet;
      CPLErrorReset();
      auto out = RunTranslate("", src_pts.get(), opts.get());
      EXPECT_NE(nullptr, out);
      CPLErrorReset();
    }
  }

  // 5. Invalid geometry inside clip datasource (with and without -makevalid).
  const std::string bad_clip_gj = std::string(kTestDir) + "/bad_clip.geojson";
  ASSERT_NO_FATAL_FAILURE(
      WriteVsimemFile(bad_clip_gj,
                      R"json({"type":"FeatureCollection","features":[
        {"type":"Feature","properties":{"id":1},
         "geometry":{"type":"Polygon","coordinates":[[[0,0],[10,10],[10,0],[0,10],[0,0]]]}}
      ]})json"));
  {
    auto opts = ParseOptions({"-f", "MEM", "-clipsrc", bad_clip_gj});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, RunTranslate("", src_pts.get(), opts.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
  {
    auto opts =
        ParseOptions({"-f", "MEM", "-clipsrc", bad_clip_gj, "-makevalid"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto out = RunTranslate("", src_pts.get(), opts.get());
    EXPECT_NE(nullptr, out);
    CPLErrorReset();
  }

  // 6. Missing clip datasource / missing clip layer / empty clip result.
  const std::vector<std::vector<std::string>> bad_clip_cases = {
      {"-f", "MEM", "-clipsrc", std::string(kTestDir) + "/no_such.geojson"},
      {"-f", "MEM", "-clipdst", std::string(kTestDir) + "/no_such.geojson"},
      {"-f", "MEM", "-clipsrc", clip_multi, "-clipsrclayer", "no_layer"},
      {"-f", "MEM", "-clipsrc", clip_multi, "-clipsrcwhere", "cid = 999"},
  };
  for (const auto& args : bad_clip_cases) {
    auto opts = ParseOptions(args);
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, RunTranslate("", src_pts.get(), opts.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 7. Reprojected clip datasource (EPSG:3857) + un-SRS'd clip datasource
  // warning.
  const std::string clip_3857 = std::string(kTestDir) + "/clip_3857.gpkg";
  {
    auto opts_proj = ParseOptions({"-f", "GPKG", "-t_srs", "EPSG:3857"});
    ASSERT_NE(nullptr, opts_proj);
    auto poly_ds = CreateMemPolygonDataset("polys");
    auto saved =
        RunTranslate(clip_3857.c_str(), poly_ds.get(), opts_proj.get());
    ASSERT_NE(nullptr, saved);
  }
  {
    auto opts = ParseOptions({"-f", "MEM", "-clipsrc", clip_3857});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", src_pts.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(2, out->GetLayer(0)->GetFeatureCount());
  }
  {
    auto opts = ParseOptions({"-f", "MEM", "-clipdst", clip_3857});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", src_pts.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(2, out->GetLayer(0)->GetFeatureCount());
  }
  const std::string clip_nosrs = std::string(kTestDir) + "/clip_nosrs.shp";
  {
    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    GDALDatasetUniquePtr mds(
        mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* lyr = mds->CreateLayer("c", nullptr, wkbPolygon, nullptr);
    auto f = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    auto p = std::make_unique<OGRPolygon>(0.0, 0.0, 10.0, 10.0);
    f->SetGeometryDirectly(p.release());
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(f.get()));
    auto save_opts = ParseOptions({"-f", "ESRI Shapefile"});
    ASSERT_NE(nullptr, save_opts);
    auto saved = RunTranslate(clip_nosrs.c_str(), mds.get(), save_opts.get());
    ASSERT_NE(nullptr, saved);
  }
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto opts_src = ParseOptions({"-f", "MEM", "-clipsrc", clip_nosrs});
    ASSERT_NE(nullptr, opts_src);
    EXPECT_NE(nullptr, RunTranslate("", src_pts.get(), opts_src.get()));
    auto opts_dst = ParseOptions({"-f", "MEM", "-clipdst", clip_nosrs});
    ASSERT_NE(nullptr, opts_dst);
    EXPECT_NE(nullptr, RunTranslate("", src_pts.get(), opts_dst.get()));
    CPLErrorReset();
  }

  // 8. Empty geometry, dimension reduction in clipsrc/clipdst, and -nlt NONE
  // with -clipsrc.
  {
    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    GDALDatasetUniquePtr mds(
        mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* lyr = mds->CreateLayer("polys", nullptr, wkbPolygon, nullptr);
    // Feature 0: empty polygon.
    auto f0 = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    f0->SetGeometryDirectly(new OGRPolygon());
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(f0.get()));
    // Feature 1: polygon touching (0,0)-(5,5) only along edge x=5 -> dim 1 < 2.
    auto f1 = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    f1->SetGeometryDirectly(new OGRPolygon(5.0, 0.0, 8.0, 5.0));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(f1.get()));
    // Feature 2: completely disjoint polygon.
    auto f2 = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    f2->SetGeometryDirectly(new OGRPolygon(50.0, 50.0, 60.0, 60.0));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(f2.get()));

    auto opts_s = ParseOptions({"-f", "MEM", "-clipsrc", "0", "0", "5", "5"});
    ASSERT_NE(nullptr, opts_s);
    auto out_s = RunTranslate("", mds.get(), opts_s.get());
    ASSERT_NE(nullptr, out_s);
    EXPECT_EQ(0, out_s->GetLayer(0)->GetFeatureCount());

    auto opts_d = ParseOptions({"-f", "MEM", "-clipdst", "0", "0", "5", "5"});
    ASSERT_NE(nullptr, opts_d);
    auto out_d = RunTranslate("", mds.get(), opts_d.get());
    ASSERT_NE(nullptr, out_d);
    EXPECT_EQ(0, out_d->GetLayer(0)->GetFeatureCount());
  }
  {
    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    GDALDatasetUniquePtr mds(
        mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* lyr = mds->CreateLayer("pts", nullptr, wkbPoint, nullptr);
    auto f0 = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    f0->SetGeometryDirectly(new OGRPoint());
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(f0.get()));
    auto f1 = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    f1->SetGeometryDirectly(new OGRPoint(2.0, 2.0));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(f1.get()));
    auto f2 = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    f2->SetGeometryDirectly(new OGRPoint(50.0, 50.0));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(f2.get()));

    auto opts = ParseOptions(
        {"-f", "MEM", "-nlt", "NONE", "-clipsrc", "0", "0", "10", "10"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", mds.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(1, out->GetLayer(0)->GetFeatureCount());
  }
}

TEST_F(Ogr2ogrLibTest, SplitListFieldsExtended) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);

  // 1. With OFTInteger64List + -maxsubfields 1.
  {
    GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* lyr = ds->CreateLayer("items", nullptr, wkbPoint, nullptr);
    OGRFieldDefn f_i("il", OFTIntegerList);
    OGRFieldDefn f_i64("i64l", OFTInteger64List);
    OGRFieldDefn f_r("rl", OFTRealList);
    OGRFieldDefn f_s("sl", OFTStringList);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_i));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_i64));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_r));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_s));

    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    feat->SetFID(1);
    const int iv[] = {1, 2};
    const GIntBig i64v[] = {10000000000LL, 20000000000LL};
    const double rv[] = {1.25, 2.5};
    const char* const sv[] = {"x", "yy", nullptr};
    feat->SetField("il", 2, const_cast<int*>(iv));
    feat->SetField("i64l", 2, i64v);
    feat->SetField("rl", 2, const_cast<double*>(rv));
    feat->SetField("sl", const_cast<char**>(sv));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));

    auto opts =
        ParseOptions({"-f", "MEM", "-splitlistfields", "-maxsubfields", "1"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", ds.get(), opts.get());
    ASSERT_NE(nullptr, out);
    OGRLayer* out_lyr = out->GetLayer(0);
    EXPECT_EQ(OFTInteger64,
              out_lyr->GetLayerDefn()->GetFieldDefn(1)->GetType());
    out_lyr->ResetReading();
    OGRFeatureUniquePtr out_f(out_lyr->GetNextFeature());
    ASSERT_NE(nullptr, out_f);
    EXPECT_EQ(10000000000LL, out_f->GetFieldAsInteger64("i64l"));
    EXPECT_DOUBLE_EQ(1.25, out_f->GetFieldAsDouble("rl"));
  }

  // 2. IntegerList/RealList/StringList with -maxsubfields 2, -progress, -fid,
  // -sql.
  {
    GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* lyr = ds->CreateLayer("items", nullptr, wkbPoint, nullptr);
    OGRFieldDefn f_i("il", OFTIntegerList);
    OGRFieldDefn f_r("rl", OFTRealList);
    OGRFieldDefn f_s("sl", OFTStringList);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_i));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_r));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_s));

    for (int id = 1; id <= 2; ++id) {
      auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
      feat->SetFID(id);
      const int iv[] = {1, 2, 3};
      const double rv[] = {1.5, 2.5, 3.5};
      const char* const sv[] = {"a", "bbb", "c", nullptr};
      feat->SetField("il", 3, const_cast<int*>(iv));
      feat->SetField("rl", 3, const_cast<double*>(rv));
      feat->SetField("sl", const_cast<char**>(sv));
      ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
    }

    int prog_calls = 0;
    auto cb = [](double, const char*, void* data) {
      ++(*static_cast<int*>(data));
      return TRUE;
    };
    auto opts = ParseOptions(
        {"-f", "MEM", "-splitlistfields", "-maxsubfields", "2", "-progress"});
    ASSERT_NE(nullptr, opts);
    GDALVectorTranslateOptionsSetProgress(opts.get(), cb, &prog_calls);
    auto out = RunTranslate("", ds.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_GT(prog_calls, 0);
    EXPECT_GE(out->GetLayer(0)->GetLayerDefn()->GetFieldIndex("rl2"), 0);
    EXPECT_LT(out->GetLayer(0)->GetLayerDefn()->GetFieldIndex("rl3"), 0);

    // -fid exercises OGRSplitListFieldLayer::GetFeature()
    auto opts_fid =
        ParseOptions({"-f", "MEM", "-splitlistfields", "-fid", "1"});
    ASSERT_NE(nullptr, opts_fid);
    auto out_fid = RunTranslate("", ds.get(), opts_fid.get());
    ASSERT_NE(nullptr, out_fid);
    EXPECT_EQ(1, out_fid->GetLayer(0)->GetFeatureCount());

    // -sql + -splitlistfields
    auto opts_sql = ParseOptions(
        {"-f", "MEM", "-splitlistfields", "-sql", "SELECT * FROM items"});
    ASSERT_NE(nullptr, opts_sql);
    auto out_sql = RunTranslate("", ds.get(), opts_sql.get());
    ASSERT_NE(nullptr, out_sql);
    EXPECT_EQ(2, out_sql->GetLayer(0)->GetFeatureCount());
  }

  // 3. -splitlistfields on layer with no list fields (with and without -sql).
  {
    auto pts = CreateMemPointDataset("pts", 4326);
    auto opts1 = ParseOptions({"-f", "MEM", "-splitlistfields"});
    ASSERT_NE(nullptr, opts1);
    EXPECT_NE(nullptr, RunTranslate("", pts.get(), opts1.get()));

    auto opts2 = ParseOptions(
        {"-f", "MEM", "-splitlistfields", "-sql", "SELECT * FROM pts"});
    ASSERT_NE(nullptr, opts2);
    EXPECT_NE(nullptr, RunTranslate("", pts.get(), opts2.get()));
  }
}

TEST_F(Ogr2ogrLibTest, GeometryTransformsDimensionsCurvesAndPrecision) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);

  // 1. -dim 2, 3, XYM, XYZM, layer_dim.
  {
    GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* lyr = ds->CreateLayer("pts", nullptr, wkbPointZM, nullptr);
    auto f = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    f->SetGeometryDirectly(new OGRPoint(1.0, 2.0, 3.0, 4.0));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(f.get()));

    for (const char* dim : {"2", "3", "XYM", "XYZM", "layer_dim"}) {
      auto opts = ParseOptions({"-f", "MEM", "-dim", dim});
      ASSERT_NE(nullptr, opts);
      auto out = RunTranslate("", ds.get(), opts.get());
      ASSERT_NE(nullptr, out);
      EXPECT_EQ(1, out->GetLayer(0)->GetFeatureCount());
    }
  }

  // 2. Curves, CONVERT_TO_LINEAR, CONVERT_TO_CURVE, PROMOTE_TO_MULTI on
  // Triangle, and reprojecting valid CurvePolygon into GPKG.
  {
    OGRSpatialReference srs;
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
    GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* lyr = ds->CreateLayer("curves", &srs, wkbCurvePolygon, nullptr);
    auto [geom, err] = OGRGeometryFactory::createFromWkt(
        "CURVEPOLYGON(CIRCULARSTRING(0 0, 1 1, 2 0, 1 -1, 0 0))");
    ASSERT_EQ(OGRERR_NONE, err);
    auto f = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    f->SetGeometryDirectly(geom.release());
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(f.get()));

    auto opts_lin =
        ParseOptions({"-f", "MEM", "-nlt", "PROMOTE_TO_MULTI", "-nlt",
                      "CONVERT_TO_LINEAR", "-t_srs", "EPSG:3857"});
    ASSERT_NE(nullptr, opts_lin);
    auto out_lin = RunTranslate("", ds.get(), opts_lin.get());
    ASSERT_NE(nullptr, out_lin);
    EXPECT_EQ(wkbMultiPolygon, out_lin->GetLayer(0)->GetGeomType());

    const std::string curve_gpkg = std::string(kTestDir) + "/curve_3857.gpkg";
    auto opts_reproj = ParseOptions({"-f", "GPKG", "-t_srs", "EPSG:3857"});
    ASSERT_NE(nullptr, opts_reproj);
    auto out_gpkg =
        RunTranslate(curve_gpkg.c_str(), ds.get(), opts_reproj.get());
    ASSERT_NE(nullptr, out_gpkg);
    EXPECT_EQ(1, out_gpkg->GetLayer(0)->GetFeatureCount());
  }
  {
    auto poly_ds = CreateMemPolygonDataset("polys");
    auto opts_curve = ParseOptions({"-f", "MEM", "-nlt", "CONVERT_TO_CURVE"});
    ASSERT_NE(nullptr, opts_curve);
    auto out_curve = RunTranslate("", poly_ds.get(), opts_curve.get());
    ASSERT_NE(nullptr, out_curve);
    EXPECT_EQ(wkbCurvePolygon, out_curve->GetLayer(0)->GetGeomType());
  }
  {
    GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* lyr = ds->CreateLayer("tri", nullptr, wkbTriangle, nullptr);
    auto [geom, err] =
        OGRGeometryFactory::createFromWkt("TRIANGLE((0 0, 1 0, 0 1, 0 0))");
    ASSERT_EQ(OGRERR_NONE, err);
    auto f = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    f->SetGeometryDirectly(geom.release());
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(f.get()));

    auto opts = ParseOptions({"-f", "MEM", "-nlt", "PROMOTE_TO_MULTI"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", ds.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(wkbMultiPolygon, out->GetLayer(0)->GetGeomType());
  }

  // 3. -explodecollections on empty Multi* / Curve / Surface /
  // GeometryCollection.
  const std::vector<std::pair<OGRwkbGeometryType, const char*>> empty_colls = {
      {wkbMultiPoint, "MULTIPOINT EMPTY"},
      {wkbMultiLineString, "MULTILINESTRING EMPTY"},
      {wkbMultiPolygon, "MULTIPOLYGON EMPTY"},
      {wkbMultiCurve, "MULTICURVE EMPTY"},
      {wkbMultiSurface, "MULTISURFACE EMPTY"},
      {wkbGeometryCollection, "GEOMETRYCOLLECTION EMPTY"},
  };
  for (const auto& [gtype, wkt] : empty_colls) {
    GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* lyr = ds->CreateLayer("c", nullptr, gtype, nullptr);
    auto [geom, err] = OGRGeometryFactory::createFromWkt(wkt);
    ASSERT_EQ(OGRERR_NONE, err);
    auto f = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    f->SetGeometryDirectly(geom.release());
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(f.get()));

    auto opts = ParseOptions({"-f", "MEM", "-explodecollections"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", ds.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(1, out->GetLayer(0)->GetFeatureCount());
  }

  // 4. -zfield missing warning, -wrapdateline, -makevalid, -skipinvalid.
  {
    auto pts = CreateMemPointDataset("pts", 4326);
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      auto opts = ParseOptions({"-f", "MEM", "-zfield", "missing_field"});
      ASSERT_NE(nullptr, opts);
      EXPECT_NE(nullptr, RunTranslate("", pts.get(), opts.get()));
      CPLErrorReset();
    }
    // Geographic -> Geographic with -wrapdateline -datelineoffset
    {
      auto opts =
          ParseOptions({"-f", "MEM", "-wrapdateline", "-datelineoffset", "15"});
      ASSERT_NE(nullptr, opts);
      EXPECT_NE(nullptr, RunTranslate("", pts.get(), opts.get()));
    }
    // Projected -> Geographic with -wrapdateline
    {
      auto opts_3857 = ParseOptions({"-f", "MEM", "-t_srs", "EPSG:3857"});
      auto ds_3857 = RunTranslate("", pts.get(), opts_3857.get());
      ASSERT_NE(nullptr, ds_3857);
      auto opts_wrap = ParseOptions({"-f", "MEM", "-t_srs", "EPSG:4326",
                                     "-wrapdateline", "-datelineoffset", "12"});
      ASSERT_NE(nullptr, opts_wrap);
      EXPECT_NE(nullptr, RunTranslate("", ds_3857.get(), opts_wrap.get()));
    }
    // Geographic -> Projected with -wrapdateline triggers CPLErrorOnce
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      auto opts =
          ParseOptions({"-f", "MEM", "-t_srs", "EPSG:3857", "-wrapdateline"});
      ASSERT_NE(nullptr, opts);
      EXPECT_NE(nullptr, RunTranslate("", pts.get(), opts.get()));
      CPLErrorReset();
    }
  }
  {
    GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* lyr = ds->CreateLayer("badpoly", nullptr, wkbPolygon, nullptr);
    auto [geom, err] =
        OGRGeometryFactory::createFromWkt("POLYGON((0 0,10 10,10 0,0 10,0 0))");
    ASSERT_EQ(OGRERR_NONE, err);
    auto f = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    f->SetGeometryDirectly(geom.release());
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(f.get()));

    auto opts_mv = ParseOptions({"-f", "MEM", "-makevalid"});
    ASSERT_NE(nullptr, opts_mv);
    auto out_mv = RunTranslate("", ds.get(), opts_mv.get());
    ASSERT_NE(nullptr, out_mv);
    EXPECT_EQ(1, out_mv->GetLayer(0)->GetFeatureCount());

    auto opts_si = ParseOptions({"-f", "MEM", "-skipinvalid"});
    ASSERT_NE(nullptr, opts_si);
    auto out_si = RunTranslate("", ds.get(), opts_si.get());
    ASSERT_NE(nullptr, out_si);
    EXPECT_EQ(0, out_si->GetLayer(0)->GetFeatureCount());
  }

  // 5. Coordinate precision (-xyRes, -zRes, -mRes, -unsetCoordPrecision).
  {
    auto pts = CreateMemPointDataset("pts", 4326);
    const std::string res_gpkg = std::string(kTestDir) + "/res.gpkg";
    {
      auto opts = ParseOptions({"-f", "GPKG", "-xyRes", "0.001 deg", "-zRes",
                                "1 mm", "-mRes", "0.1"});
      ASSERT_NE(nullptr, opts);
      auto out = RunTranslate(res_gpkg.c_str(), pts.get(), opts.get());
      ASSERT_NE(nullptr, out);
    }
    {
      auto opts = ParseOptions({"-f", "GPKG", "-t_srs", "EPSG:3857", "-xyRes",
                                "10 mm", "-zRes", "0.01 m", "-overwrite"});
      ASSERT_NE(nullptr, opts);
      auto out = RunTranslate(res_gpkg.c_str(), pts.get(), opts.get());
      ASSERT_NE(nullptr, out);
    }
    {
      // MEM does not advertise DCAP_HONOR_GEOM_COORDINATE_PRECISION -> warning.
      WithQuietHandler quiet;
      CPLErrorReset();
      auto opts = ParseOptions(
          {"-f", "MEM", "-xyRes", "1 m", "-zRes", "1 m", "-mRes", "0.1"});
      ASSERT_NE(nullptr, opts);
      EXPECT_NE(nullptr, RunTranslate("", pts.get(), opts.get()));
      CPLErrorReset();
    }
    {
      // OGR_APPLY_GEOM_SET_PRECISION=NO disables SetPrecision() call.
      CPLConfigOptionSetter prec_opt("OGR_APPLY_GEOM_SET_PRECISION", "NO",
                                     false);
      auto opts =
          ParseOptions({"-f", "GPKG", "-xyRes", "0.01 deg", "-overwrite"});
      ASSERT_NE(nullptr, opts);
      EXPECT_NE(nullptr, RunTranslate(res_gpkg.c_str(), pts.get(), opts.get()));
    }
    {
      // -unsetCoordPrecision drops source coordinate precision.
      // Create a default geometry column (empty name) because MEM's
      // CreateGeomFieldAfterCreateLayer path in ogr2ogr_lib.cpp does not
      // clear precision when a named geometry column is recreated.
      GDALDatasetUniquePtr prec_ds(
          mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
      OGRLayer* prec_lyr =
          prec_ds->CreateLayer("p", nullptr, wkbPoint, nullptr);
      ASSERT_NE(nullptr, prec_lyr);
      OGRGeomCoordinatePrecision prec;
      prec.dfXYResolution = 0.01;
      prec_lyr->GetLayerDefn()->GetGeomFieldDefn(0)->SetCoordinatePrecision(
          prec);
      auto opts = ParseOptions({"-f", "MEM", "-unsetCoordPrecision"});
      ASSERT_NE(nullptr, opts);
      auto out = RunTranslate("", prec_ds.get(), opts.get());
      ASSERT_NE(nullptr, out);
      EXPECT_EQ(OGRGeomCoordinatePrecision::UNKNOWN,
                out->GetLayer(0)
                    ->GetLayerDefn()
                    ->GetGeomFieldDefn(0)
                    ->GetCoordinatePrecision()
                    .dfXYResolution);

      // Unit suffix without SRS fails for -xyRes and -zRes.
      auto nosrs = CreateMemPointDataset("nosrs", 0);
      for (const char* flag : {"-xyRes", "-zRes"}) {
        WithQuietHandler quiet;
        CPLErrorReset();
        auto bad = ParseOptions({"-f", "MEM", flag, "1 mm"});
        ASSERT_NE(nullptr, bad);
        EXPECT_EQ(nullptr, RunTranslate("", nosrs.get(), bad.get()));
        EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
        CPLErrorReset();
      }
    }
  }

  // 6. -gcp + -tps + -t_srs (CompositeCT), -ct, -ct_opt, -s_srs/-t_srs errors.
  {
    auto pts = CreateMemPointDataset("pts", 4326);
    {
      auto opts = ParseOptions({"-f", "MEM", "-gcp", "0",      "0",
                                "0",  "0",   "-gcp", "10",     "0",
                                "10", "0",   "-gcp", "0",      "10",
                                "0",  "10",  "-tps", "-t_srs", "EPSG:3857"});
      ASSERT_NE(nullptr, opts);
      auto out = RunTranslate("", pts.get(), opts.get());
      ASSERT_NE(nullptr, out);
      EXPECT_EQ(4, out->GetLayer(0)->GetFeatureCount());
    }
    {
      // Only 1 GCP with -order 2 fails GCPCoordTransformation::IsValid().
      WithQuietHandler quiet;
      CPLErrorReset();
      auto opts = ParseOptions(
          {"-f", "MEM", "-gcp", "0", "0", "1", "1", "-order", "2"});
      ASSERT_NE(nullptr, opts);
      EXPECT_EQ(nullptr, RunTranslate("", pts.get(), opts.get()));
      CPLErrorReset();
    }
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      auto opts = ParseOptions(
          {"-f", "MEM", "-ct", "+proj=pipeline +step +proj=axisswap +order=2,1",
           "-ct_opt", "ALLOW_BALLPARK=YES", "-ct_opt", "ONLY_BEST=NO",
           "-ct_opt", "WARN_ABOUT_DIFFERENT_COORD_OP=NO", "-ct_opt",
           "UNKNOWN_OPT=YES"});
      ASSERT_NE(nullptr, opts);
      auto out = RunTranslate("", pts.get(), opts.get());
      EXPECT_NE(nullptr, out);
      CPLErrorReset();
    }
    {
      auto opts = ParseOptions({"-f", "MEM", "-s_srs", "EPSG:4326",
                                "-s_coord_epoch", "2020.0", "-t_srs",
                                "EPSG:3857", "-t_coord_epoch", "2021.0"});
      ASSERT_NE(nullptr, opts);
      EXPECT_NE(nullptr, RunTranslate("", pts.get(), opts.get()));
    }
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"-f", "MEM", "-t_srs", "INVALID_SRS_XYZ"},
             {"-f", "MEM", "-s_srs", "INVALID_SRS_XYZ", "-t_srs", "EPSG:4326"},
             {"-f", "MEM", "-s_srs", "LOCAL_CS[\"foo\"]", "-t_srs",
              "EPSG:4326"},
         }) {
      WithQuietHandler quiet;
      CPLErrorReset();
      auto opts = ParseOptions(args);
      ASSERT_NE(nullptr, opts);
      EXPECT_EQ(nullptr, RunTranslate("", pts.get(), opts.get()));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      CPLErrorReset();
    }
  }

  // 7. Per-feature SRS, missing source SRS error, and
  // AxisMappingCoordinateTransformation.
  {
    OGRSpatialReference srs4326, srs4269;
    srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    srs4269.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    ASSERT_EQ(OGRERR_NONE, srs4326.importFromEPSG(4326));
    ASSERT_EQ(OGRERR_NONE, srs4269.importFromEPSG(4269));
    GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* lyr = ds->CreateLayer("per_feat", nullptr, wkbPoint, nullptr);

    auto f1 = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    auto p1 = std::make_unique<OGRPoint>(1.0, 2.0);
    p1->assignSpatialReference(&srs4326);
    f1->SetGeometryDirectly(p1.release());
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(f1.get()));

    auto f2 = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    auto p2 = std::make_unique<OGRPoint>(-75.0, 40.0);
    p2->assignSpatialReference(&srs4269);
    f2->SetGeometryDirectly(p2.release());
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(f2.get()));

    auto opts = ParseOptions({"-f", "MEM", "-t_srs", "EPSG:3857"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", ds.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(2, out->GetLayer(0)->GetFeatureCount());
  }
  {
    auto nosrs = CreateMemPointDataset("nosrs", 0);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto opts = ParseOptions({"-f", "MEM", "-t_srs", "EPSG:3857"});
    ASSERT_NE(nullptr, opts);
    EXPECT_EQ(nullptr, RunTranslate("", nosrs.get(), opts.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
  {
    // AxisMappingCoordinateTransformation: authority-compliant (2,1) -> MEM
    // output (OAMS_TRADITIONAL_GIS_ORDER 1,2). Set SRS on GeomFieldDefn after
    // CreateLayer because MEMDataset::ICreateLayer forces
    // TRADITIONAL_GIS_ORDER.
    std::unique_ptr<OGRSpatialReference, OGRSpatialReferenceReleaser> srs_auth(
        new OGRSpatialReference());
    ASSERT_EQ(OGRERR_NONE, srs_auth->importFromEPSG(4326));
    srs_auth->SetAxisMappingStrategy(OAMS_AUTHORITY_COMPLIANT);
    GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* lyr = ds->CreateLayer("axis", nullptr, wkbPoint, nullptr);
    whileUnsealing(lyr->GetLayerDefn()->GetGeomFieldDefn(0))
        ->SetSpatialRef(srs_auth.get());
    auto f = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    f->SetGeometryDirectly(new OGRPoint(45.0, -75.0));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(f.get()));

    auto opts = ParseOptions({"-f", "MEM"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", ds.get(), opts.get());
    ASSERT_NE(nullptr, out);
    OGRLayer* out_lyr = out->GetLayer(0);
    out_lyr->ResetReading();
    OGRFeatureUniquePtr out_f(out_lyr->GetNextFeature());
    ASSERT_NE(nullptr, out_f);
    EXPECT_DOUBLE_EQ(-75.0, out_f->GetGeometryRef()->toPoint()->getX());
    EXPECT_DOUBLE_EQ(45.0, out_f->GetGeometryRef()->toPoint()->getY());
  }

  // 8. Reprojection failure (out of domain) with and without -skipfailures.
  {
    GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRSpatialReference srs4326;
    srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    ASSERT_EQ(OGRERR_NONE, srs4326.importFromEPSG(4326));
    OGRLayer* lyr = ds->CreateLayer("oom", &srs4326, wkbPoint, nullptr);
    auto f = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    f->SetGeometryDirectly(
        new OGRPoint(std::numeric_limits<double>::quiet_NaN(), 0.0));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(f.get()));

    {
      WithQuietHandler quiet;
      CPLErrorReset();
      auto opts = ParseOptions({"-f", "MEM", "-t_srs", "EPSG:3857"});
      ASSERT_NE(nullptr, opts);
      EXPECT_EQ(nullptr, RunTranslate("", ds.get(), opts.get()));
      CPLErrorReset();
    }
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      auto opts =
          ParseOptions({"-f", "MEM", "-t_srs", "EPSG:3857", "-skipfailures"});
      ASSERT_NE(nullptr, opts);
      EXPECT_NE(nullptr, RunTranslate("", ds.get(), opts.get()));
      CPLErrorReset();
    }
  }
}

TEST_F(Ogr2ogrLibTest, FieldMappingDomainsWidthsAndFids) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);

  // 1. -mapFieldType All=String, Integer=Integer(Int16), -unsetFieldWidth,
  // -forceNullable, -unsetDefault.
  {
    GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* lyr = ds->CreateLayer("flds", nullptr, wkbPoint, nullptr);
    OGRFieldDefn f_int("i", OFTInteger);
    f_int.SetWidth(8);
    f_int.SetNullable(FALSE);
    f_int.SetDefault("42");
    OGRFieldDefn f_real("r", OFTReal);
    f_real.SetWidth(12);
    f_real.SetPrecision(3);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_int));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_real));
    auto f = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    f->SetField("i", 7);
    f->SetField("r", 3.14);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(f.get()));

    auto opts = ParseOptions(
        {"-f", "MEM", "-mapFieldType", "Integer=Integer(Int16),All=String",
         "-unsetFieldWidth", "-forceNullable", "-unsetDefault"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", ds.get(), opts.get());
    ASSERT_NE(nullptr, out);
    const OGRFeatureDefn* fd = out->GetLayer(0)->GetLayerDefn();
    EXPECT_EQ(OFSTInt16, fd->GetFieldDefn(0)->GetSubType());
    EXPECT_EQ(0, fd->GetFieldDefn(0)->GetWidth());
    EXPECT_EQ(TRUE, fd->GetFieldDefn(0)->IsNullable());
    EXPECT_EQ(nullptr, fd->GetFieldDefn(0)->GetDefault());
    EXPECT_EQ(OFTString, fd->GetFieldDefn(1)->GetType());
    EXPECT_EQ(0, fd->GetFieldDefn(1)->GetPrecision());
  }

  // 2. Unsupported field types: list -> GPKG JSON, Int64/DateTime -> Shapefile,
  // Binary -> CSV.
  {
    GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* lyr = ds->CreateLayer("types", nullptr, wkbPoint, nullptr);
    OGRFieldDefn f_il("il", OFTIntegerList);
    OGRFieldDefn f_i64("i64", OFTInteger64);
    OGRFieldDefn f_dt("dt", OFTDateTime);
    OGRFieldDefn f_bin("bin", OFTBinary);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_il));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_i64));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_dt));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_bin));
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    const int iv[] = {1, 2};
    feat->SetField("il", 2, const_cast<int*>(iv));
    feat->SetField("i64", static_cast<GIntBig>(123456789012LL));
    feat->SetField("dt", 2026, 1, 2, 3, 4, 5.0f, 100);
    const GByte bin_data[] = {1, 2, 3};
    feat->SetField(feat->GetFieldIndex("bin"), 3, const_cast<GByte*>(bin_data));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));

    WithQuietHandler quiet;
    CPLErrorReset();
    auto gpkg_opts = ParseOptions({"-f", "GPKG"});
    EXPECT_NE(nullptr,
              RunTranslate((std::string(kTestDir) + "/types.gpkg").c_str(),
                           ds.get(), gpkg_opts.get()));

    auto shp_opts = ParseOptions({"-f", "ESRI Shapefile"});
    const std::string shp_path = std::string(kTestDir) + "/types.shp";
    auto shp_out = RunTranslate(shp_path.c_str(), ds.get(), shp_opts.get());
    ASSERT_NE(nullptr, shp_out);
    shp_out.reset();

    // Roundtrip Shapefile -> GPKG -> Shapefile for numeric field width delta.
    std::unique_ptr<GDALDataset> shp_in(
        GDALDataset::Open(shp_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, shp_in);
    auto gpkg2_opts = ParseOptions({"-f", "GPKG"});
    EXPECT_NE(nullptr,
              RunTranslate((std::string(kTestDir) + "/from_shp.gpkg").c_str(),
                           shp_in.get(), gpkg2_opts.get()));

    auto csv_opts = ParseOptions({"-f", "CSV"});
    EXPECT_NE(nullptr,
              RunTranslate((std::string(kTestDir) + "/types.csv").c_str(),
                           ds.get(), csv_opts.get()));
    CPLErrorReset();
  }

  // 3. -select with OLCIgnoreFields (GPKG), -where, -zfield, multi-geom,
  // missing field.
  {
    auto pts = CreateMemPointDataset("pts", 4326);
    const std::string gpkg_src = std::string(kTestDir) + "/sel_src.gpkg";
    auto save_opts = ParseOptions({"-f", "GPKG"});
    ASSERT_NE(nullptr,
              RunTranslate(gpkg_src.c_str(), pts.get(), save_opts.get()));

    std::unique_ptr<GDALDataset> in(
        GDALDataset::Open(gpkg_src.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, in);
    auto opts = ParseOptions({"-f", "MEM", "-select", "name", "-where",
                              "id >= 2", "-zfield", "val"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate("", in.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(3, out->GetLayer(0)->GetFeatureCount());
  }
  {
    GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* lyr = ds->CreateLayer("mg", nullptr, wkbNone, nullptr);
    OGRSpatialReference srs;
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
    OGRGeomFieldDefn gf1("geom1", wkbPoint);
    gf1.SetSpatialRef(&srs);
    gf1.SetNullable(FALSE);
    OGRGeomFieldDefn gf2("geom2", wkbPoint);
    gf2.SetSpatialRef(&srs);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateGeomField(&gf1));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateGeomField(&gf2));
    OGRFieldDefn f_id("id", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_id));
    auto f = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    f->SetField("id", 1);
    f->SetGeomFieldDirectly(0, new OGRPoint(1.0, 2.0));
    f->SetGeomFieldDirectly(1, new OGRPoint(3.0, 4.0));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(f.get()));

    // All geom fields into MEM (ODsCCreateGeomFieldAfterCreateLayer).
    auto opts_all = ParseOptions({"-f", "MEM"});
    auto out_all = RunTranslate("", ds.get(), opts_all.get());
    ASSERT_NE(nullptr, out_all);
    EXPECT_EQ(2, out_all->GetLayer(0)->GetLayerDefn()->GetGeomFieldCount());

    // Selecting one geom field by name.
    auto opts_one = ParseOptions({"-f", "MEM", "-select", "geom2,id"});
    auto out_one = RunTranslate("", ds.get(), opts_one.get());
    ASSERT_NE(nullptr, out_one);
    EXPECT_EQ(1, out_one->GetLayer(0)->GetLayerDefn()->GetGeomFieldCount());

    // Selecting two geom fields into GeoJSON fails (no multiple geom fields).
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      auto opts_two = ParseOptions({"-f", "GeoJSON", "-select", "geom1,geom2"});
      EXPECT_EQ(
          nullptr,
          RunTranslate((std::string(kTestDir) + "/bad_mg.geojson").c_str(),
                       ds.get(), opts_two.get()));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      CPLErrorReset();
    }
    // Missing field in -select: fails without -skipfailures, succeeds with it.
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      auto opts_bad = ParseOptions({"-f", "MEM", "-select", "no_field"});
      EXPECT_EQ(nullptr, RunTranslate("", ds.get(), opts_bad.get()));
      auto opts_skip =
          ParseOptions({"-f", "MEM", "-select", "no_field", "-skipfailures"});
      EXPECT_NE(nullptr, RunTranslate("", ds.get(), opts_skip.get()));
      CPLErrorReset();
    }
  }

  // 4. Duplicate field names in source layer get renamed.
  {
    GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* lyr = ds->CreateLayer("dups", nullptr, wkbPoint, nullptr);
    OGRFieldDefn f1("dup", OFTInteger);
    OGRFieldDefn f2("dup", OFTInteger);
    OGRFieldDefn f3("dup2", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f1));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f2));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f3));
    WithQuietHandler quiet;
    CPLErrorReset();
    auto opts = ParseOptions({"-f", "MEM"});
    auto out = RunTranslate("", ds.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(3, out->GetLayer(0)->GetLayerDefn()->GetFieldCount());
    CPLErrorReset();
  }

  // 5. String "fid" -> GPKG, Integer "fid" -> GPKG, GPKG -> GPKG (-unsetFid /
  // -explodecollections).
  {
    GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* lyr = ds->CreateLayer("str_fid", nullptr, wkbPoint, nullptr);
    OGRFieldDefn f_fid("fid", OFTString);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_fid));
    auto f = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    f->SetField("fid", "abc");
    f->SetGeometryDirectly(new OGRPoint(1.0, 2.0));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(f.get()));

    const std::string gpkg_str_fid = std::string(kTestDir) + "/str_fid.gpkg";
    auto opts = ParseOptions({"-f", "GPKG"});
    auto out = RunTranslate(gpkg_str_fid.c_str(), ds.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_STREQ("gpkg_fid", out->GetLayer(0)->GetFIDColumn());
  }
  {
    GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* lyr = ds->CreateLayer("int_fid", nullptr, wkbMultiPoint, nullptr);
    OGRFieldDefn f_fid("fid", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_fid));
    auto f = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    f->SetField("fid", 77);
    auto mp = std::make_unique<OGRMultiPoint>();
    OGRPoint p1(1.0, 2.0);
    mp->addGeometry(&p1);
    f->SetGeometryDirectly(mp.release());
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(f.get()));

    const std::string gpkg_int_fid = std::string(kTestDir) + "/int_fid.gpkg";
    auto opts = ParseOptions({"-f", "GPKG"});
    auto out = RunTranslate(gpkg_int_fid.c_str(), ds.get(), opts.get());
    ASSERT_NE(nullptr, out);
    out.reset();

    std::unique_ptr<GDALDataset> in_gpkg(GDALDataset::Open(
        gpkg_int_fid.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
        nullptr, nullptr));
    ASSERT_NE(nullptr, in_gpkg);

    auto opts_exp = ParseOptions({"-f", "GPKG", "-explodecollections"});
    EXPECT_NE(nullptr,
              RunTranslate((std::string(kTestDir) + "/exp_fid.gpkg").c_str(),
                           in_gpkg.get(), opts_exp.get()));

    auto opts_unset = ParseOptions({"-f", "GPKG", "-unsetFid"});
    EXPECT_NE(nullptr,
              RunTranslate((std::string(kTestDir) + "/unset_fid.gpkg").c_str(),
                           in_gpkg.get(), opts_unset.get()));
  }

  // 6. -append with -fieldmap (valid and invalid), -addfields + -select,
  // -relaxedFieldNameMatch.
  {
    auto src = CreateMemPointDataset("pts", 4326);
    const std::string fm_gpkg = std::string(kTestDir) + "/fieldmap.gpkg";
    auto opts_init = ParseOptions({"-f", "GPKG"});
    ASSERT_NE(nullptr,
              RunTranslate(fm_gpkg.c_str(), src.get(), opts_init.get()));

    {
      auto opts = ParseOptions({"-append", "-fieldmap", "0,1,2"});
      EXPECT_NE(nullptr, RunTranslate(fm_gpkg.c_str(), src.get(), opts.get()));
    }
    for (const char* bad_fm : {"0", "99,0,1"}) {
      WithQuietHandler quiet;
      CPLErrorReset();
      auto opts = ParseOptions({"-append", "-fieldmap", bad_fm});
      EXPECT_EQ(nullptr, RunTranslate(fm_gpkg.c_str(), src.get(), opts.get()));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      CPLErrorReset();
    }
    {
      auto opts = ParseOptions({"-append", "-addfields", "-select", "id,name"});
      EXPECT_NE(nullptr, RunTranslate(fm_gpkg.c_str(), src.get(), opts.get()));
    }

    // Relaxed vs exact field name matching when case differs.
    GDALDatasetUniquePtr ds_upper(
        mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* lu = ds_upper->CreateLayer("pts", nullptr, wkbPoint, nullptr);
    OGRFieldDefn fn("NAME", OFTString);
    ASSERT_EQ(OGRERR_NONE, lu->CreateField(&fn));
    auto fu = std::make_unique<OGRFeature>(lu->GetLayerDefn());
    fu->SetField("NAME", "upper");
    ASSERT_EQ(OGRERR_NONE, lu->CreateFeature(fu.get()));

    {
      WithQuietHandler quiet;
      CPLErrorReset();
      auto opts_exact = ParseOptions({"-append"});
      EXPECT_NE(nullptr, RunTranslate(fm_gpkg.c_str(), ds_upper.get(),
                                      opts_exact.get()));
      CPLErrorReset();
    }
    {
      auto opts_relaxed = ParseOptions({"-append", "-relaxedFieldNameMatch"});
      EXPECT_NE(nullptr, RunTranslate(fm_gpkg.c_str(), ds_upper.get(),
                                      opts_relaxed.get()));
    }
  }

  // 7. -nomd, -noNativeData on GeoJSON, -dateTimeTo edge cases, and OpenFileGDB
  // Shape_Area.
  {
    const std::string gj_in_path = std::string(kTestDir) + "/native.geojson";
    ASSERT_NO_FATAL_FAILURE(WriteVsimemFile(
        gj_in_path,
        R"json({"type":"FeatureCollection","customKey":"val","features":[
          {"type":"Feature","properties":{"a":1},"geometry":{"type":"Point","coordinates":[1,2]}}
        ]})json"));
    const char* const open_opts[] = {"NATIVE_DATA=YES", nullptr};
    std::unique_ptr<GDALDataset> gj_in(
        GDALDataset::Open(gj_in_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, open_opts, nullptr));
    ASSERT_NE(nullptr, gj_in);
    auto opts_native = ParseOptions({"-f", "GeoJSON"});
    EXPECT_NE(
        nullptr,
        RunTranslate((std::string(kTestDir) + "/native_keep.geojson").c_str(),
                     gj_in.get(), opts_native.get()));
    auto opts_nonative = ParseOptions({"-f", "GeoJSON", "-noNativeData"});
    EXPECT_NE(
        nullptr,
        RunTranslate((std::string(kTestDir) + "/native_drop.geojson").c_str(),
                     gj_in.get(), opts_nonative.get()));
  }
  {
    GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    ds->SetMetadataItem("SRC_MD", "1");
    OGRLayer* lyr = ds->CreateLayer("dt_lyr", nullptr, wkbPolygon, nullptr);
    lyr->SetMetadataItem("LYR_MD", "1");
    OGRFieldDefn f_dt("dt", OFTDateTime);
    OGRFieldDefn f_sa("Shape_Area", OFTReal);
    f_sa.SetDefault("FILEGEODATABASE_SHAPE_AREA");
    OGRFieldDefn f_sl("Shape_Length", OFTReal);
    f_sl.SetDefault("FILEGEODATABASE_SHAPE_LENGTH");
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_dt));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_sa));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_sl));

    // Feature 0: null dt; Feature 1: local TZFlag=0; Feature 2: UTC TZFlag=100.
    auto f0 = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    f0->SetGeometryDirectly(new OGRPolygon(0.0, 0.0, 1.0, 1.0));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(f0.get()));

    auto f1 = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    f1->SetField("dt", 2026, 1, 1, 10, 0, 0.0f, 0);
    f1->SetGeometryDirectly(new OGRPolygon(0.0, 0.0, 1.0, 1.0));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(f1.get()));

    auto f2 = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    f2->SetField("dt", 2026, 1, 1, 10, 0, 0.0f, 100);
    f2->SetGeometryDirectly(new OGRPolygon(0.0, 0.0, 1.0, 1.0));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(f2.get()));

    auto opts_nomd =
        ParseOptions({"-f", "GPKG", "-nomd", "-dateTimeTo", "UTC"});
    auto out_nomd = RunTranslate((std::string(kTestDir) + "/nomd.gpkg").c_str(),
                                 ds.get(), opts_nomd.get());
    ASSERT_NE(nullptr, out_nomd);
    EXPECT_EQ(nullptr, out_nomd->GetMetadataItem("SRC_MD"));

    auto opts_gdb = ParseOptions({"-f", "OpenFileGDB"});
    EXPECT_NE(nullptr,
              RunTranslate((std::string(kTestDir) + "/out.gdb").c_str(),
                           ds.get(), opts_gdb.get()));
  }
}

TEST_F(Ogr2ogrLibTest, SpatSrsLayersTransactionsSqlOsmGpxAndRelationships) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);

  // 1. ApplySpatialFilter: geographic -spat_srs on projected layer, no-SRS
  // warning, -geomfield valid/missing.
  {
    auto pts = CreateMemPointDataset("pts", 4326);
    auto opts_3857 = ParseOptions({"-f", "MEM", "-t_srs", "EPSG:3857"});
    auto ds_3857 = RunTranslate("", pts.get(), opts_3857.get());
    ASSERT_NE(nullptr, ds_3857);

    auto opts_geo_spat = ParseOptions(
        {"-f", "MEM", "-spat", "0", "0", "10", "10", "-spat_srs", "EPSG:4326"});
    auto out = RunTranslate("", ds_3857.get(), opts_geo_spat.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(2, out->GetLayer(0)->GetFeatureCount());

    auto nosrs = CreateMemPointDataset("nosrs", 0);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_NE(nullptr, RunTranslate("", nosrs.get(), opts_geo_spat.get()));
    auto opts_gf = ParseOptions({"-f", "MEM", "-spat", "0", "0", "10", "10",
                                 "-geomfield", "no_such_geom"});
    EXPECT_NE(nullptr, RunTranslate("", pts.get(), opts_gf.get()));
    CPLErrorReset();
  }

  // 2. -sql warnings (-where, layers), -geomfield, CSV -sql -progress, invalid
  // -sql, -sql into existing single .shp, -sql on same GPKG with -overwrite
  // -nln.
  {
    auto pts = CreateMemPointDataset("pts", 4326);
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      auto opts = ParseOptions({"-f", "MEM", "-sql", "SELECT * FROM pts",
                                "-where", "id > 0", "-geomfield", "bad_gf",
                                "-spat", "0", "0", "10", "10", "pts"});
      EXPECT_NE(nullptr, RunTranslate("", pts.get(), opts.get()));
      CPLErrorReset();
    }
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      auto opts = ParseOptions({"-f", "MEM", "-sql", "SELECT * FROM bad_tbl"});
      EXPECT_EQ(nullptr, RunTranslate("", pts.get(), opts.get()));
      CPLErrorReset();
    }

    // CSV lacks OLCFastFeatureCount -> progress warning in -sql and normal
    // modes.
    const std::string csv_in_path = std::string(kTestDir) + "/in_nofast.csv";
    ASSERT_NO_FATAL_FAILURE(
        WriteVsimemFile(csv_in_path, "id,name\n1,a\n2,b\n"));
    std::unique_ptr<GDALDataset> csv_in(GDALDataset::Open(
        csv_in_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
        nullptr, nullptr));
    ASSERT_NE(nullptr, csv_in);
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      auto o1 = ParseOptions(
          {"-f", "MEM", "-progress", "-sql", "SELECT * FROM in_nofast"});
      EXPECT_NE(nullptr, RunTranslate("", csv_in.get(), o1.get()));
      auto o2 = ParseOptions({"-f", "MEM", "-progress"});
      EXPECT_NE(nullptr, RunTranslate("", csv_in.get(), o2.get()));
      auto o3 = ParseOptions(
          {"-f", "MEM", "-progress", "--invoked-from-gdal-vector-convert"});
      EXPECT_NE(nullptr, RunTranslate("", csv_in.get(), o3.get()));
      CPLErrorReset();
    }

    // Existing single .shp with and without -sql.
    const std::string single_shp = std::string(kTestDir) + "/single.shp";
    auto save_shp = ParseOptions({"-f", "ESRI Shapefile"});
    ASSERT_NE(nullptr,
              RunTranslate(single_shp.c_str(), pts.get(), save_shp.get()));
    {
      auto ow_sql = ParseOptions(
          {"-f", "ESRI Shapefile", "-overwrite", "-sql", "SELECT * FROM pts"});
      EXPECT_NE(nullptr,
                RunTranslate(single_shp.c_str(), pts.get(), ow_sql.get()));
      auto ow_plain = ParseOptions({"-f", "ESRI Shapefile", "-overwrite"});
      EXPECT_NE(nullptr,
                RunTranslate(single_shp.c_str(), pts.get(), ow_plain.get()));
    }

    // Same GPKG source & dest with -sql -overwrite -nln.
    const std::string same_gpkg = std::string(kTestDir) + "/same.gpkg";
    auto save_gpkg = ParseOptions({"-f", "GPKG"});
    ASSERT_NE(nullptr,
              RunTranslate(same_gpkg.c_str(), pts.get(), save_gpkg.get()));
    {
      std::unique_ptr<GDALDataset> rw(
          GDALDataset::Open(same_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
                            nullptr, nullptr, nullptr));
      ASSERT_NE(nullptr, rw);
      auto o = ParseOptions(
          {"-overwrite", "-nln", "pts_copy", "-sql", "SELECT * FROM pts"});
      GDALDatasetH hRW = GDALDataset::ToHandle(rw.get());
      EXPECT_EQ(hRW,
                GDALVectorTranslate(nullptr, hRW, 1, &hRW, o.get(), nullptr));
    }
    // Same GPKG source & dest errors in update / non-update modes.
    {
      std::unique_ptr<GDALDataset> ro(GDALDataset::Open(
          same_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
          nullptr, nullptr));
      ASSERT_NE(nullptr, ro);
      for (const auto& args : std::vector<std::vector<std::string>>{
               {"-append"},
               {"-overwrite", "-nln", "pts", "pts"},
               {"-overwrite", "-nln", "pts"},
               {"-f", "GPKG"},
           }) {
        WithQuietHandler quiet;
        CPLErrorReset();
        auto o = ParseOptions(args);
        EXPECT_EQ(nullptr, RunTranslate(same_gpkg.c_str(), ro.get(), o.get()));
        EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
        CPLErrorReset();
      }
    }
  }

  // 3. Multi-layer Shapefile dir creation, explicit layers, invalid -where,
  // transactions, driver guessing, -f VRT, -doo, -dsco/-lco warnings, -upsert
  // on .shp, Create fail.
  {
    GDALDatasetUniquePtr mds(
        mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* l1 = mds->CreateLayer("l1", nullptr, wkbPoint, nullptr);
    OGRLayer* l2 = mds->CreateLayer("l2", nullptr, wkbPoint, nullptr);
    OGRFieldDefn fi("id", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, l1->CreateField(&fi));
    ASSERT_EQ(OGRERR_NONE, l2->CreateField(&fi));
    for (OGRLayer* lyr : {l1, l2}) {
      auto f = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
      f->SetField("id", 1);
      f->SetGeometryDirectly(new OGRPoint(1.0, 2.0));
      ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(f.get()));
    }

    // Multi-layer into .shp creates directory.
    const std::string multi_shp = std::string(kTestDir) + "/multi_dir.shp";
    auto o_shp = ParseOptions({"-f", "ESRI Shapefile"});
    auto out_shp = RunTranslate(multi_shp.c_str(), mds.get(), o_shp.get());
    ASSERT_NE(nullptr, out_shp);
    EXPECT_EQ(2, out_shp->GetLayerCount());

    // Explicit layer names + missing layer with/without -skipfailures.
    {
      auto o_l1 = ParseOptions({"-f", "MEM", "l1"});
      auto out_l1 = RunTranslate("", mds.get(), o_l1.get());
      ASSERT_NE(nullptr, out_l1);
      EXPECT_EQ(1, out_l1->GetLayerCount());
    }
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      auto o_miss = ParseOptions({"-f", "MEM", "no_such_layer"});
      EXPECT_EQ(nullptr, RunTranslate("", mds.get(), o_miss.get()));
      auto o_skip =
          ParseOptions({"-f", "MEM", "-skipfailures", "no_such_layer", "l1"});
      auto out_skip = RunTranslate("", mds.get(), o_skip.get());
      EXPECT_NE(nullptr, out_skip);
      CPLErrorReset();
    }
    // Invalid -where with/without -skipfailures.
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      auto o_badw = ParseOptions({"-f", "MEM", "-where", "bad_col = 1"});
      EXPECT_EQ(nullptr, RunTranslate("", mds.get(), o_badw.get()));
      auto o_skipw =
          ParseOptions({"-f", "MEM", "-where", "bad_col = 1", "-skipfailures"});
      EXPECT_NE(nullptr, RunTranslate("", mds.get(), o_skipw.get()));
      CPLErrorReset();
    }

    // -gt 2 with -ds_transaction and -lyr_transaction into GPKG.
    const std::string tx_gpkg = std::string(kTestDir) + "/tx.gpkg";
    {
      auto o_ds = ParseOptions({"-f", "GPKG", "-gt", "1", "-ds_transaction"});
      EXPECT_NE(nullptr, RunTranslate(tx_gpkg.c_str(), mds.get(), o_ds.get()));
      auto o_ly = ParseOptions(
          {"-f", "GPKG", "-gt", "1", "-lyr_transaction", "-overwrite"});
      EXPECT_NE(nullptr, RunTranslate(tx_gpkg.c_str(), mds.get(), o_ly.get()));
    }
    // -update without -append on existing layer (both CLI modes) and warnings.
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      auto o_up1 = ParseOptions({"-update", "-dsco", "DUMMY=1", "-lco", "D=1"});
      EXPECT_EQ(nullptr, RunTranslate(tx_gpkg.c_str(), mds.get(), o_up1.get()));
      auto o_up2 =
          ParseOptions({"-update", "--invoked-from-gdal-vector-convert"});
      EXPECT_EQ(nullptr, RunTranslate(tx_gpkg.c_str(), mds.get(), o_up2.get()));
      auto o_app_lco = ParseOptions({"-append", "-lco", "SPATIAL_INDEX=NO"});
      EXPECT_NE(nullptr,
                RunTranslate(tx_gpkg.c_str(), mds.get(), o_app_lco.get()));
      CPLErrorReset();
    }
  }

  // 4. Miscellaneous creation / driver errors.
  {
    auto pts = CreateMemPointDataset("pts", 4326);
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      // -doo ignored on creation
      auto o_doo = ParseOptions({"-f", "MEM", "-doo", "OPT=1"});
      EXPECT_NE(nullptr, RunTranslate("", pts.get(), o_doo.get()));
      // -f VRT -> OGR_VRT doesn't support Create
      auto o_vrt = ParseOptions({"-f", "VRT"});
      EXPECT_EQ(nullptr,
                RunTranslate((std::string(kTestDir) + "/o.vrt").c_str(),
                             pts.get(), o_vrt.get()));
      // Extensionless without -f -> defaults to Shapefile, fails with
      // vector-convert flag
      const std::string noext = std::string(kTestDir) + "/noext_dir";
      auto o_noext = ParseOptions({});
      EXPECT_NE(nullptr, RunTranslate(noext.c_str(), pts.get(), o_noext.get()));
      auto o_vc = ParseOptions({"--invoked-from-gdal-vector-convert"});
      EXPECT_EQ(nullptr,
                RunTranslate((std::string(kTestDir) + "/noext2").c_str(),
                             pts.get(), o_vc.get()));
      // Unknown extension without -f
      EXPECT_EQ(nullptr,
                RunTranslate((std::string(kTestDir) + "/bad.xyzext").c_str(),
                             pts.get(), o_noext.get()));
      // -update on non-existent GPKG
      auto o_up = ParseOptions({"-update"});
      EXPECT_EQ(nullptr,
                RunTranslate((std::string(kTestDir) + "/missing.gpkg").c_str(),
                             pts.get(), o_up.get()));
      // -upsert on Shapefile fails
      const std::string shp_up = std::string(kTestDir) + "/no_upsert.shp";
      auto o_sh = ParseOptions({"-f", "ESRI Shapefile"});
      ASSERT_NE(nullptr, RunTranslate(shp_up.c_str(), pts.get(), o_sh.get()));
      auto o_ups = ParseOptions({"-upsert"});
      EXPECT_EQ(nullptr, RunTranslate(shp_up.c_str(), pts.get(), o_ups.get()));
      // Create failure in missing directory under TempDir
      const std::string bad_dir = (std::filesystem::path(::testing::TempDir()) /
                                   "missing_subdir_xyz" / "out.gpkg")
                                      .string();
      auto o_gpkg = ParseOptions({"-f", "GPKG"});
      EXPECT_EQ(nullptr,
                RunTranslate(bad_dir.c_str(), pts.get(), o_gpkg.get()));
      CPLErrorReset();
    }
    // GeoJSON with -nln sets @NAME
    {
      auto o_gj = ParseOptions({"-f", "GeoJSON", "-nln", "my_gj_layer"});
      EXPECT_NE(nullptr,
                RunTranslate((std::string(kTestDir) + "/named.geojson").c_str(),
                             pts.get(), o_gj.get()));
    }
  }

  // 5. GPX -> GPKG (FID preservation heuristic).
  {
    const std::string gpx_path = std::string(kTestDir) + "/test.gpx";
    ASSERT_NO_FATAL_FAILURE(
        WriteVsimemFile(gpx_path,
                        R"xml(<?xml version="1.0" encoding="UTF-8"?>
<gpx version="1.1" creator="test">
  <wpt lat="1.0" lon="2.0"><name>wp1</name></wpt>
</gpx>)xml"));
    std::unique_ptr<GDALDataset> gpx_in(
        GDALDataset::Open(gpx_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, gpx_in);
    auto opts = ParseOptions({"-f", "GPKG"});
    EXPECT_NE(nullptr,
              RunTranslate((std::string(kTestDir) + "/from_gpx.gpkg").c_str(),
                           gpx_in.get(), opts.get()));
  }

  // 6. OSM random-layer-reading paths.
  {
    const std::string osm_path = std::string(kTestDir) + "/test.osm";
    ASSERT_NO_FATAL_FAILURE(
        WriteVsimemFile(osm_path,
                        R"xml(<?xml version="1.0" encoding="UTF-8"?>
<osm version="0.6" generator="test">
  <node id="1" lat="1.0" lon="2.0" version="1">
    <tag k="name" v="n1"/>
  </node>
  <node id="2" lat="1.1" lon="2.1" version="1"/>
  <way id="10" version="1">
    <nd ref="1"/>
    <nd ref="2"/>
    <tag k="highway" v="residential"/>
    <tag k="name" v="w1"/>
  </way>
</osm>)xml"));
    std::unique_ptr<GDALDataset> osm_in(
        GDALDataset::Open(osm_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, osm_in);

    {
      WithQuietHandler quiet;
      CPLErrorReset();
      auto o_all = ParseOptions({"-f", "GPKG", "-progress"});
      auto out_all =
          RunTranslate((std::string(kTestDir) + "/osm_all.gpkg").c_str(),
                       osm_in.get(), o_all.get());
      EXPECT_NE(nullptr, out_all);
      CPLErrorReset();
    }
    {
      auto o_pts =
          ParseOptions({"-f", "MEM", "-spat", "0", "0", "5", "5", "points"});
      auto out_pts = RunTranslate("", osm_in.get(), o_pts.get());
      ASSERT_NE(nullptr, out_pts);
      EXPECT_EQ(1, out_pts->GetLayerCount());
    }
    {
      // Empty filter result -> deferred empty layer creation.
      auto o_empty = ParseOptions(
          {"-f", "MEM", "-spat", "50", "50", "60", "60", "points"});
      auto out_empty = RunTranslate("", osm_in.get(), o_empty.get());
      ASSERT_NE(nullptr, out_empty);
      EXPECT_EQ(0, out_empty->GetLayer(0)->GetFeatureCount());
    }
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"-f", "MEM", "-splitlistfields"},
             {"-f", "MEM", "no_osm_layer"},
             {"-f", "MEM", "-where", "bad_col = 1", "points"},
         }) {
      WithQuietHandler quiet;
      CPLErrorReset();
      auto o = ParseOptions(args);
      EXPECT_EQ(nullptr, RunTranslate("", osm_in.get(), o.get()));
      CPLErrorReset();
    }
  }

  // 7. CopyRelationships (GPKG -> GPKG, partial layer skip, existing skip).
  {
    const std::string rel_src = std::string(kTestDir) + "/rel_src.gpkg";
    const std::string rel_dst = std::string(kTestDir) + "/rel_dst.gpkg";
    {
      GDALDriver* gpkg_drv = GetGDALDriverManager()->GetDriverByName("GPKG");
      ASSERT_NE(nullptr, gpkg_drv);
      GDALDatasetUniquePtr ds(
          gpkg_drv->Create(rel_src.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
      ASSERT_NE(nullptr, ds);
      OGRLayer* l1 = ds->CreateLayer("left_tbl", nullptr, wkbPoint, nullptr);
      OGRLayer* l2 = ds->CreateLayer("right_tbl", nullptr, wkbPoint, nullptr);
      OGRFieldDefn fk("key_id", OFTInteger);
      ASSERT_EQ(OGRERR_NONE, l1->CreateField(&fk));
      ASSERT_EQ(OGRERR_NONE, l2->CreateField(&fk));
      auto rel = std::make_unique<GDALRelationship>(
          "left_right", "left_tbl", "right_tbl",
          GDALRelationshipCardinality::GRC_MANY_TO_MANY);
      rel->SetLeftTableFields({"key_id"});
      rel->SetRightTableFields({"key_id"});
      std::string reason;
      ASSERT_TRUE(ds->AddRelationship(std::move(rel), reason)) << reason;
    }

    std::unique_ptr<GDALDataset> src_ds(
        GDALDataset::Open(rel_src.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, src_ds);

    // Copy both layers -> relationship copied.
    {
      auto o = ParseOptions({"-f", "GPKG"});
      auto out = RunTranslate(rel_dst.c_str(), src_ds.get(), o.get());
      ASSERT_NE(nullptr, out);
      EXPECT_FALSE(out->GetRelationshipNames().empty());
    }
    // Update existing GPKG -> existing relationship skipped.
    {
      auto o = ParseOptions({"-append"});
      auto out = RunTranslate(rel_dst.c_str(), src_ds.get(), o.get());
      ASSERT_NE(nullptr, out);
    }
    // Copy only left_tbl -> relationship skipped because right_tbl missing.
    {
      auto o = ParseOptions({"-f", "GPKG", "left_tbl"});
      auto out =
          RunTranslate((std::string(kTestDir) + "/rel_part.gpkg").c_str(),
                       src_ds.get(), o.get());
      ASSERT_NE(nullptr, out);
      EXPECT_TRUE(out->GetRelationshipNames().empty());
    }
  }
}

TEST_F(Ogr2ogrLibTest, ArrowBatchExtended) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  const std::string src_gpkg = std::string(kTestDir) + "/arrow_src.gpkg";
  {
    GDALDriver* gpkg_drv = GetGDALDriverManager()->GetDriverByName("GPKG");
    GDALDatasetUniquePtr ds(
        gpkg_drv->Create(src_gpkg.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
    std::vector<OGRCodedValue> codes;
    codes.push_back({CPLStrdup("1"), CPLStrdup("One")});
    codes.push_back({nullptr, nullptr});
    std::string reason;
    ASSERT_TRUE(ds->AddFieldDomain(
        std::make_unique<OGRCodedFieldDomain>("dom1", "Domain", OFTInteger,
                                              OFSTNone, std::move(codes)),
        reason));
    OGRSpatialReference srs;
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
    OGRLayer* lyr = ds->CreateLayer("pts3d", &srs, wkbPoint25D, nullptr);
    OGRFieldDefn fd("d", OFTInteger);
    fd.SetDomainName("dom1");
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&fd));

    // Feature 0: null geometry; Features 1..2: 3D points.
    auto f0 = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    f0->SetField("d", 1);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(f0.get()));
    for (int i = 1; i <= 2; ++i) {
      auto fi = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
      fi->SetField("d", 1);
      fi->SetGeometryDirectly(new OGRPoint(1.0 * i, 2.0 * i, 10.0 * i));
      ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(fi.get()));
    }
  }

  std::unique_ptr<GDALDataset> in(
      GDALDataset::Open(src_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, in);

  CPLConfigOptionSetter arrow_opt("OGR2OGR_USE_ARROW_API", "YES", false);
  CPLConfigOptionSetter thr_opt("OGR2OGR_MIN_FEATURES_FOR_THREADED_REPROJ", "1",
                                false);

  const std::string dst_gpkg = std::string(kTestDir) + "/arrow_dst.gpkg";
  {
    auto opts = ParseOptions(
        {"-f", "GPKG", "-t_srs", "EPSG:3857", "-limit", "2", "-gt", "2"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate(dst_gpkg.c_str(), in.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(2, out->GetLayer(0)->GetFeatureCount());
    EXPECT_NE(nullptr, out->GetFieldDomain("dom1"));
  }
  // -append via Arrow to existing GPKG layer.
  {
    auto opts = ParseOptions({"-append", "-t_srs", "EPSG:3857"});
    ASSERT_NE(nullptr, opts);
    auto out = RunTranslate(dst_gpkg.c_str(), in.get(), opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(5, out->GetLayer(0)->GetFeatureCount());
  }
  // 2-geom-field layer with -t_srs falls back from Arrow cleanly.
  {
    GDALDatasetUniquePtr ds2(
        mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRSpatialReference srs;
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
    OGRLayer* lyr = ds2->CreateLayer("mg", &srs, wkbPoint, nullptr);
    OGRGeomFieldDefn gf2("g2", wkbPoint);
    gf2.SetSpatialRef(&srs);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateGeomField(&gf2));
    auto opts = ParseOptions({"-f", "MEM", "-t_srs", "EPSG:3857"});
    EXPECT_NE(nullptr, RunTranslate("", ds2.get(), opts.get()));
  }
}

// Custom test dataset/layer wrappers to exercise GDALVectorTranslate edge paths
// (hidden layers, GetSupportedSRSList/SetActiveSRS, FID64, MEASURES,
// failing CreateFeature/DeleteLayer/CommitTransaction/FlushCache).
class MockSrcLayer final : public OGRLayerDecorator {
 public:
  MockSrcLayer(OGRLayer* base, bool has_srs_list)
      : OGRLayerDecorator(base, /*bTakeOwnership=*/false) {
    if (has_srs_list) {
      auto s4326 =
          std::unique_ptr<OGRSpatialReference, OGRSpatialReferenceReleaser>(
              new OGRSpatialReference());
      s4326->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
      s4326->importFromEPSG(4326);
      srs_list_.push_back(std::move(s4326));

      auto s3857 =
          std::unique_ptr<OGRSpatialReference, OGRSpatialReferenceReleaser>(
              new OGRSpatialReference());
      s3857->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
      s3857->importFromEPSG(3857);
      srs_list_.push_back(std::move(s3857));
    }
  }

  const GetSupportedSRSListRetType& GetSupportedSRSList(int) override {
    return srs_list_;
  }

  OGRErr SetActiveSRS(int, const OGRSpatialReference*) override {
    set_active_srs_called_ = true;
    return OGRERR_NONE;
  }

  const char* GetMetadataItem(const char* name,
                              const char* domain = "") override {
    if (EQUAL(name, OLMD_FID64)) return "YES";
    if (domain != nullptr && EQUAL(domain, "NATIVE_DATA")) {
      if (EQUAL(name, "NATIVE_DATA")) return "{}";
      if (EQUAL(name, "NATIVE_MEDIA_TYPE")) return "application/json";
    }
    return OGRLayerDecorator::GetMetadataItem(name, domain);
  }

  char** GetMetadata(const char* domain = "") override {
    if (domain != nullptr && EQUAL(domain, "MEASURES")) {
      return measures_md_.List();
    }
    return OGRLayerDecorator::GetMetadata(domain);
  }

  bool set_active_srs_called() const { return set_active_srs_called_; }
  void SetMeasureUnit(const char* val) {
    measures_md_.SetNameValue("UNIT", val);
  }

 private:
  GetSupportedSRSListRetType srs_list_;
  bool set_active_srs_called_ = false;
  CPLStringList measures_md_;
};

class MockSrcDataset final : public GDALDataset {
 public:
  MockSrcDataset(GDALDataset* base, OGRLayer* visible_layer,
                 OGRLayer* hidden_layer)
      : base_(base), visible_(visible_layer), hidden_(hidden_layer) {
    SetDescription("MockSrcDataset");
    poDriver = base->GetDriver();
  }

  int GetLayerCount() const override { return base_->GetLayerCount(); }

  const OGRLayer* GetLayer(int i) const override {
    if (i == 0 && visible_ != nullptr) return visible_;
    return base_->GetLayer(i);
  }

  bool IsLayerPrivate(int i) const override { return i > 0; }

  OGRLayer* GetLayerByName(const char* name) override {
    if (hidden_ != nullptr && EQUAL(name, "hidden_lyr")) return hidden_;
    if (visible_ != nullptr && EQUAL(name, visible_->GetName()))
      return visible_;
    return base_->GetLayerByName(name);
  }

  OGRLayer* ExecuteSQL(const char* stmt, OGRGeometry* filter,
                       const char* dialect) override {
    return base_->ExecuteSQL(stmt, filter, dialect);
  }

  void ReleaseResultSet(OGRLayer* rs) override { base_->ReleaseResultSet(rs); }

 private:
  GDALDataset* base_;
  OGRLayer* visible_;
  OGRLayer* hidden_;
};

class MockDstLayer final : public OGRLayerDecorator {
 public:
  MockDstLayer(OGRLayer* base, bool fail_create_feature)
      : OGRLayerDecorator(base, /*bTakeOwnership=*/false),
        fail_create_feature_(fail_create_feature) {}

  OGRErr ICreateFeature(OGRFeature* f) override {
    if (fail_create_feature_) return OGRERR_FAILURE;
    return OGRLayerDecorator::ICreateFeature(f);
  }

 private:
  bool fail_create_feature_;
};

class MockDstDataset final : public GDALDataset {
 public:
  explicit MockDstDataset(GDALDataset* base, GDALDriver* drv = nullptr)
      : base_(base) {
    SetDescription("MockDstDataset");
    poDriver = drv != nullptr ? drv : base->GetDriver();
  }

  int GetLayerCount() const override {
    return static_cast<int>(wrapped_layers_.size());
  }

  const OGRLayer* GetLayer(int i) const override {
    if (i >= 0 && i < static_cast<int>(wrapped_layers_.size()))
      return wrapped_layers_[i].get();
    return nullptr;
  }

  int TestCapability(const char* cap) const override {
    if (EQUAL(cap, ODsCTransactions)) return TRUE;
    return base_->TestCapability(cap);
  }

  OGRErr StartTransaction(int) override { return OGRERR_NONE; }
  OGRErr CommitTransaction() override {
    return fail_commit_ ? OGRERR_FAILURE : OGRERR_NONE;
  }
  OGRErr RollbackTransaction() override { return OGRERR_NONE; }

  CPLErr FlushCache(bool) override {
    return fail_flush_ ? CE_Failure : CE_None;
  }

  OGRErr DeleteLayer(int i) override {
    if (fail_delete_) return OGRERR_FAILURE;
    OGRErr err = base_->DeleteLayer(i);
    if (err == OGRERR_NONE && i >= 0 &&
        i < static_cast<int>(wrapped_layers_.size())) {
      wrapped_layers_.erase(wrapped_layers_.begin() + i);
    }
    return err;
  }

  void set_fail_create_feature(bool v) { fail_create_feature_ = v; }
  void set_fail_commit(bool v) { fail_commit_ = v; }
  void set_fail_flush(bool v) { fail_flush_ = v; }
  void set_fail_delete(bool v) { fail_delete_ = v; }

 protected:
  OGRLayer* ICreateLayer(const char* name, const OGRGeomFieldDefn* gf,
                         CSLConstList lco) override {
    OGRLayer* base_lyr = base_->CreateLayer(name, gf, lco);
    if (base_lyr == nullptr) return nullptr;
    wrapped_layers_.push_back(
        std::make_unique<MockDstLayer>(base_lyr, fail_create_feature_));
    return wrapped_layers_.back().get();
  }

 private:
  GDALDataset* base_;
  std::vector<std::unique_ptr<MockDstLayer>> wrapped_layers_;
  bool fail_create_feature_ = false;
  bool fail_commit_ = false;
  bool fail_flush_ = false;
  bool fail_delete_ = false;
};

TEST_F(Ogr2ogrLibTest, CustomDriversAndMockLayers) {
  auto pts = CreateMemPointDataset("pts", 4326);
  ASSERT_NE(nullptr, pts);

  // 1. Custom CreateCopy-only driver exercising GDALVectorTranslateCreateCopy
  // and GDALVectorTranslateWrappedDataset / GDALVectorTranslateWrappedLayer.
  auto cc_drv = std::make_unique<GDALDriver>();
  cc_drv->SetDescription("TEST_CC_DRV");
  cc_drv->SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
  cc_drv->pfnCreateCopy = [](const char* dest, GDALDataset* src_ds, int, char**,
                             GDALProgressFunc, void*) -> GDALDataset* {
    if (std::string(dest).find("probe") != std::string::npos) {
      EXPECT_NE(nullptr, src_ds->GetLayerByName("pts"));
      EXPECT_NE(nullptr, src_ds->GetLayerByName("PTS"));
      EXPECT_EQ(nullptr, src_ds->GetLayerByName("missing_lyr"));
      EXPECT_NE(nullptr, src_ds->GetLayerByName("hidden_lyr"));
      EXPECT_NE(nullptr, src_ds->GetLayerByName("HIDDEN_LYR"));
      OGRLayer* lyr = src_ds->GetLayer(0);
      if (lyr != nullptr) {
        EXPECT_NE(nullptr, lyr->GetLayerDefn());
        lyr->ResetReading();
        OGRFeatureUniquePtr f1(lyr->GetNextFeature());
        OGRFeatureUniquePtr f2(lyr->GetFeature(1));
      }
      if (OGRLayer* rs =
              src_ds->ExecuteSQL("SELECT * FROM pts", nullptr, nullptr)) {
        OGRFeatureUniquePtr rf(rs->GetNextFeature());
        src_ds->ReleaseResultSet(rs);
      }
    }
    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    return mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr);
  };
  GetGDALDriverManager()->RegisterDriver(cc_drv.get());
  absl::Cleanup dereg_cc = [&] {
    GetGDALDriverManager()->DeregisterDriver(cc_drv.get());
  };

  // Unsupported flags rejected by GDALVectorTranslateCreateCopy.
  const std::vector<std::vector<std::string>> cc_unsupported = {
      {"-f", "TEST_CC_DRV", "-skipfailures"},
      {"-f", "TEST_CC_DRV", "-ds_transaction"},
      {"-f", "TEST_CC_DRV", "-fid", "1"},
      {"-f", "TEST_CC_DRV", "-lco", "A=B"},
      {"-f", "TEST_CC_DRV", "-addfields"},
      {"-f", "TEST_CC_DRV", "-s_srs", "EPSG:4326", "-t_srs", "EPSG:3857"},
      {"-f", "TEST_CC_DRV", "-relaxedFieldNameMatch"},
      {"-f", "TEST_CC_DRV", "-nln", "newname"},
      {"-f", "TEST_CC_DRV", "-select", "id"},
      {"-f", "TEST_CC_DRV", "-sql", "SELECT * FROM pts"},
      {"-f", "TEST_CC_DRV", "-dialect", "SQLITE"},
      {"-f", "TEST_CC_DRV", "-nlt", "POINT"},
      {"-f", "TEST_CC_DRV", "-fieldTypeToString", "Integer"},
      {"-f", "TEST_CC_DRV", "-mapFieldType", "Integer=Real"},
      {"-f", "TEST_CC_DRV", "-unsetFieldWidth"},
      {"-f", "TEST_CC_DRV", "-wrapdateline"},
      {"-f", "TEST_CC_DRV", "-clipsrc", "0", "0", "1", "1"},
      {"-f", "TEST_CC_DRV", "-clipsrcsql", "SELECT 1"},
      {"-f", "TEST_CC_DRV", "-clipsrclayer", "l"},
      {"-f", "TEST_CC_DRV", "-clipsrcwhere", "id=1"},
      {"-f", "TEST_CC_DRV", "-clipdst", "0", "0", "1", "1"},
      {"-f", "TEST_CC_DRV", "-clipdstsql", "SELECT 1"},
      {"-f", "TEST_CC_DRV", "-clipdstlayer", "l"},
      {"-f", "TEST_CC_DRV", "-clipdstwhere", "id=1"},
      {"-f", "TEST_CC_DRV", "-splitlistfields"},
      {"-f", "TEST_CC_DRV", "-maxsubfields", "2"},
      {"-f", "TEST_CC_DRV", "-explodecollections"},
      {"-f", "TEST_CC_DRV", "-zfield", "val"},
      {"-f", "TEST_CC_DRV", "-gcp", "0", "0", "1", "1"},
      {"-f", "TEST_CC_DRV", "-append", "-fieldmap", "0,1,2"},
      {"-f", "TEST_CC_DRV", "-forceNullable"},
      {"-f", "TEST_CC_DRV", "-resolveDomains"},
      {"-f", "TEST_CC_DRV", "-emptyStrAsNull"},
      {"-f", "TEST_CC_DRV", "-unsetDefault"},
      {"-f", "TEST_CC_DRV", "-unsetFid"},
      {"-f", "TEST_CC_DRV", "-nomd"},
      {"-f", "TEST_CC_DRV", "-noNativeData"},
      {"-f", "TEST_CC_DRV", "-limit", "1"},
      {"-f", "TEST_CC_DRV", "-mo", "K=V"},
      {"-f", "TEST_CC_DRV", "-where", "id=1"},
      {"-f", "TEST_CC_DRV", "pts"},
  };
  for (const auto& args : cc_unsupported) {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto opts = ParseOptions(args);
    ASSERT_NE(nullptr, opts);
    EXPECT_EQ(nullptr, RunTranslate("out.cc", pts.get(), opts.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Valid CreateCopy with WrappedDataset (including hidden layer, -t_srs,
  // -spat, -geomfield).
  {
    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    GDALDatasetUniquePtr hidden_holder(
        mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* hidden_lyr = hidden_holder->CreateLayer(
        "hidden_lyr", pts->GetLayer(0)->GetSpatialRef(), wkbPoint, nullptr);
    MockSrcLayer wrapped_vis(pts->GetLayer(0), /*has_srs_list=*/false);
    MockSrcDataset mock_src(pts.get(), &wrapped_vis, hidden_lyr);

    {
      auto opts = ParseOptions({"-f", "TEST_CC_DRV", "-t_srs", "EPSG:3857",
                                "-spat", "0", "0", "10", "10"});
      EXPECT_NE(nullptr, RunTranslate("probe.cc", &mock_src, opts.get()));
    }
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      auto opts =
          ParseOptions({"-f", "TEST_CC_DRV", "-a_srs", "EPSG:4326", "-spat",
                        "0", "0", "10", "10", "-geomfield", "bad_geom"});
      EXPECT_NE(nullptr, RunTranslate("ok.cc", &mock_src, opts.get()));
      auto opts_badsrs =
          ParseOptions({"-f", "TEST_CC_DRV", "-t_srs", "INVALID_SRS"});
      EXPECT_EQ(nullptr, RunTranslate("bad.cc", &mock_src, opts_badsrs.get()));
      auto nosrs = CreateMemPointDataset("nosrs", 0);
      auto opts_t = ParseOptions({"-f", "TEST_CC_DRV", "-t_srs", "EPSG:3857"});
      EXPECT_EQ(nullptr, RunTranslate("bad2.cc", nosrs.get(), opts_t.get()));
      CPLErrorReset();
    }
  }

  // 2. Custom CanVectorTranslateFrom / VectorTranslateFrom driver.
  {
    auto vt_drv = std::make_unique<GDALDriver>();
    vt_drv->SetDescription("TEST_VT_DRV");
    vt_drv->SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
    vt_drv->pfnCanVectorTranslateFrom =
        [](const char*, GDALDataset*, CSLConstList, char***) { return true; };
    vt_drv->pfnVectorTranslateFrom = [](const char*, GDALDataset*, CSLConstList,
                                        GDALProgressFunc,
                                        void*) -> GDALDataset* {
      GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
      return mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr);
    };
    GetGDALDriverManager()->RegisterDriver(vt_drv.get());
    absl::Cleanup dereg_vt = [&] {
      GetGDALDriverManager()->DeregisterDriver(vt_drv.get());
    };
    auto opts = ParseOptions({"-f", "TEST_VT_DRV"});
    EXPECT_NE(nullptr, RunTranslate("out.vt", pts.get(), opts.get()));
  }

  // 3. GetSupportedSRSList / SetActiveSRS + IsLayerPrivate +
  // FID64/MEASURES/NATIVE_DATA.
  {
    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    GDALDatasetUniquePtr base_multi(
        mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRSpatialReference srs4326;
    srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    ASSERT_EQ(OGRERR_NONE, srs4326.importFromEPSG(4326));
    OGRLayer* vis = base_multi->CreateLayer("vis", &srs4326, wkbPoint, nullptr);
    base_multi->CreateLayer("priv", &srs4326, wkbPoint, nullptr);
    auto f = std::make_unique<OGRFeature>(vis->GetLayerDefn());
    f->SetGeometryDirectly(new OGRPoint(1.0, 2.0));
    ASSERT_EQ(OGRERR_NONE, vis->CreateFeature(f.get()));

    MockSrcLayer mock_vis(vis, /*has_srs_list=*/true);
    mock_vis.SetMeasureUnit("meters");
    MockSrcDataset mock_ds(base_multi.get(), &mock_vis, nullptr);

    auto opts = ParseOptions(
        {"-f", "GPKG", "-t_srs", "EPSG:3857", "-spat", "0", "0", "10", "10"});
    auto out =
        RunTranslate((std::string(kTestDir) + "/active_srs.gpkg").c_str(),
                     &mock_ds, opts.get());
    ASSERT_NE(nullptr, out);
    EXPECT_TRUE(mock_vis.set_active_srs_called());
    EXPECT_EQ(1, out->GetLayerCount());
  }

  // 4. Failing MockDstDataset: CreateFeature fail (+/- skipfailures, tx),
  // CommitTransaction fail, FlushCache fail, DeleteLayer fail, OFTInteger64
  // without GDAL_DMD_CREATIONFIELDDATATYPES.
  {
    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    auto nodt_drv = std::make_unique<GDALDriver>();
    nodt_drv->SetDescription("TEST_NODT_DRV");
    nodt_drv->SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
    nodt_drv->SetMetadataItem(GDAL_DCAP_CREATE, "YES");
    GetGDALDriverManager()->RegisterDriver(nodt_drv.get());
    absl::Cleanup dereg_nodt = [&] {
      GetGDALDriverManager()->DeregisterDriver(nodt_drv.get());
    };

    GDALDatasetUniquePtr src64(
        mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* l64 = src64->CreateLayer("l64", nullptr, wkbPoint, nullptr);
    OGRFieldDefn f64("big", OFTInteger64);
    ASSERT_EQ(OGRERR_NONE, l64->CreateField(&f64));
    auto feat64 = std::make_unique<OGRFeature>(l64->GetLayerDefn());
    feat64->SetField("big", static_cast<GIntBig>(42));
    ASSERT_EQ(OGRERR_NONE, l64->CreateFeature(feat64.get()));

    GDALDatasetUniquePtr mem_dst(
        mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    MockDstDataset dst_nodt(mem_dst.get(), nodt_drv.get());
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetH hSrc64 = GDALDataset::ToHandle(src64.get());
    EXPECT_EQ(GDALDataset::ToHandle(&dst_nodt),
              GDALVectorTranslate(nullptr, GDALDataset::ToHandle(&dst_nodt), 1,
                                  &hSrc64, nullptr, nullptr));
    EXPECT_EQ(OFTReal,
              dst_nodt.GetLayer(0)->GetLayerDefn()->GetFieldDefn(0)->GetType());
    CPLErrorReset();
  }
  {
    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    GDALDatasetH hSrc = GDALDataset::ToHandle(pts.get());
    WithQuietHandler quiet;
    CPLErrorReset();

    // CreateFeature fails without -skipfailures (lyr_transaction rollback).
    {
      GDALDatasetUniquePtr mem_dst(
          mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
      MockDstDataset dst(mem_dst.get());
      dst.set_fail_create_feature(true);
      auto o = ParseOptions({"-lyr_transaction"});
      EXPECT_EQ(nullptr,
                GDALVectorTranslate(nullptr, GDALDataset::ToHandle(&dst), 1,
                                    &hSrc, o.get(), nullptr));
    }
    // CreateFeature fails with -skipfailures (lyr_transaction &
    // ds_transaction).
    {
      GDALDatasetUniquePtr mem_dst(
          mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
      MockDstDataset dst(mem_dst.get());
      dst.set_fail_create_feature(true);
      auto o1 = ParseOptions({"-skipfailures", "-lyr_transaction"});
      EXPECT_NE(nullptr,
                GDALVectorTranslate(nullptr, GDALDataset::ToHandle(&dst), 1,
                                    &hSrc, o1.get(), nullptr));
      auto o2 = ParseOptions({"-skipfailures", "-ds_transaction", "-append"});
      EXPECT_NE(nullptr,
                GDALVectorTranslate(nullptr, GDALDataset::ToHandle(&dst), 1,
                                    &hSrc, o2.get(), nullptr));
    }
    // CommitTransaction fails, FlushCache fails, DeleteLayer fails.
    {
      GDALDatasetUniquePtr mem_dst(
          mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
      MockDstDataset dst(mem_dst.get());
      dst.set_fail_commit(true);
      auto o = ParseOptions({"-ds_transaction"});
      EXPECT_EQ(nullptr,
                GDALVectorTranslate(nullptr, GDALDataset::ToHandle(&dst), 1,
                                    &hSrc, o.get(), nullptr));
    }
    {
      GDALDatasetUniquePtr mem_dst(
          mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
      MockDstDataset dst(mem_dst.get());
      dst.set_fail_flush(true);
      EXPECT_EQ(nullptr,
                GDALVectorTranslate(nullptr, GDALDataset::ToHandle(&dst), 1,
                                    &hSrc, nullptr, nullptr));
    }
    {
      GDALDatasetUniquePtr mem_dst(
          mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
      MockDstDataset dst(mem_dst.get());
      EXPECT_NE(nullptr,
                GDALVectorTranslate(nullptr, GDALDataset::ToHandle(&dst), 1,
                                    &hSrc, nullptr, nullptr));
      dst.set_fail_delete(true);
      auto o_ow = ParseOptions({"-overwrite"});
      EXPECT_EQ(nullptr,
                GDALVectorTranslate(nullptr, GDALDataset::ToHandle(&dst), 1,
                                    &hSrc, o_ow.get(), nullptr));
    }
    CPLErrorReset();
  }
}

// TODO: b/335317901 - ogr2ogr_lib.cpp reaches ~88.6% line coverage (4558/5144
// instrumented lines across 8849 lines). Remaining uncovered lines are
// predominantly defensive OOM/clone failure checks, platform-specific or
// disabled-driver branches (e.g., PostgreSQL/PGDUMP/OpenFileGDB write paths),
// and minor usage/CLI edge branches.

}  // namespace
}  // namespace autotest2
