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
// Tests for GDALVectorInfo (apps/ogrinfo_lib.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/ogrinfo_lib.cpp
//   https://gdal.org/programs/ogrinfo.html

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdal_utils.h"
#include "third_party/gdal/apps/gdal_utils_priv.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_relationship.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

using GdalVectorInfoResult = std::unique_ptr<char, CplFreeDeleter>;
using GdalVectorInfoOptionsPtr =
    std::unique_ptr<GDALVectorInfoOptions,
                    decltype(&GDALVectorInfoOptionsFree)>;

GdalVectorInfoOptionsPtr MakeOptions(const std::vector<std::string>& args) {
  CPLStringList arg_list;
  for (const auto& arg : args) {
    arg_list.AddString(arg.c_str());
  }
  return GdalVectorInfoOptionsPtr(
      GDALVectorInfoOptionsNew(arg_list.List(),
                               /*psOptionsForBinary=*/nullptr),
      GDALVectorInfoOptionsFree);
}

GdalVectorInfoResult RunVectorInfo(
    GDALDatasetH ds, const GDALVectorInfoOptions* options = nullptr) {
  return GdalVectorInfoResult(GDALVectorInfo(ds, options));
}

class FakeRelationshipDataset : public GDALDataset {
 public:
  void AddFakeRelationship(std::unique_ptr<GDALRelationship> rel) {
    relationships_.push_back(std::move(rel));
  }

  std::vector<std::string> GetRelationshipNames(
      CSLConstList = nullptr) const override {
    std::vector<std::string> names;
    for (const auto& rel : relationships_) {
      names.push_back(rel->GetName());
    }
    return names;
  }

  const GDALRelationship* GetRelationship(
      const std::string& name) const override {
    for (const auto& rel : relationships_) {
      if (rel->GetName() == name) {
        return rel.get();
      }
    }
    return nullptr;
  }

 private:
  std::vector<std::unique_ptr<GDALRelationship>> relationships_;
};

class FakePrivateLayerDataset : public GDALDataset {
 public:
  explicit FakePrivateLayerDataset(GDALDataset* base) : base_(base) {}
  int GetLayerCount() const override { return base_->GetLayerCount(); }
  const OGRLayer* GetLayer(int idx) const override {
    return base_->GetLayer(idx);
  }
  bool IsLayerPrivate(int) const override { return true; }

 private:
  GDALDataset* base_;
};

class FakeLayerWithSupportedSRS : public OGRLayer {
 public:
  explicit FakeLayerWithSupportedSRS(OGRLayer* base) : base_(base) {
    auto srs1 = std::make_unique<OGRSpatialReference>();
    srs1->importFromEPSG(4326);
    supported_srs_.emplace_back(srs1.release());

    auto srs2 = std::make_unique<OGRSpatialReference>();
    srs2->SetWellKnownGeogCS("WGS72");
    supported_srs_.emplace_back(srs2.release());
  }

  const char* GetName() const override { return base_->GetName(); }
  const OGRFeatureDefn* GetLayerDefn() const override {
    return base_->GetLayerDefn();
  }
  void ResetReading() override { base_->ResetReading(); }
  OGRFeature* GetNextFeature() override { return base_->GetNextFeature(); }
  int TestCapability(const char* cap) const override {
    return base_->TestCapability(cap);
  }
  const OGRLayer::GetSupportedSRSListRetType& GetSupportedSRSList(
      int) override {
    return supported_srs_;
  }

 private:
  OGRLayer* base_;
  OGRLayer::GetSupportedSRSListRetType supported_srs_;
};

class FakeDatasetWithSupportedSRS : public GDALDataset {
 public:
  explicit FakeDatasetWithSupportedSRS(OGRLayer* layer) : layer_(layer) {}
  int GetLayerCount() const override { return 1; }
  const OGRLayer* GetLayer(int) const override { return layer_; }

 private:
  OGRLayer* layer_;
};

class OgrinfoLibTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    RegisterOGRGeoJSON();
  }
};

TEST_F(OgrinfoLibTest, NullDatasetReturnsNull) {
  EXPECT_EQ(nullptr, RunVectorInfo(nullptr).get());
}

TEST_F(OgrinfoLibTest, ParserUsage) {
  const std::string usage = GDALVectorInfoGetParserUsage();
  EXPECT_THAT(usage, HasSubstr("ogrinfo"));
  EXPECT_THAT(usage, HasSubstr("-json"));
  EXPECT_THAT(usage, HasSubstr("-summary"));
}

TEST_F(OgrinfoLibTest, OptionsParsingValidAndFree) {
  const std::vector<std::string> args = {
      "-json",       "-q",
      "-fid",        "1",
      "-spat",       "0",
      "0",           "10",
      "10",          "-geomfield",
      "geom",        "-where",
      "id > 0",      "-dialect",
      "OGRSQL",      "-sql",
      "SELECT 1",    "-rc",
      "1",           "-al",
      "-so",         "-limit",
      "5",           "-fields",
      "NO",          "-geom",
      "WKT",         "-nomd",
      "-listmdd",    "-mdd",
      "CustomMD",    "-nocount",
      "-noextent",   "-extent3D",
      "-nogeomtype", "-wkt_format",
      "WKT2",        "-fielddomain",
      "dom1",        "-stdout",
      "--cli",       "dummy_file",
      "dummy_layer",
  };
  auto opts = MakeOptions(args);
  ASSERT_NE(nullptr, opts);
}

TEST_F(OgrinfoLibTest, OptionsPreprocessingFieldsAndGeom) {
  auto opts = MakeOptions({"-fields=no", "-geom=wkt"});
  ASSERT_NE(nullptr, opts);
}

TEST_F(OgrinfoLibTest, OptionsParsingFileIngestionForWhereAndSql) {
  constexpr char kWherePath[] = "/vsimem/ogrinfo_lib_test/where.txt";
  constexpr char kSqlPath[] = "/vsimem/ogrinfo_lib_test/sql.txt";
  VsiMemTempWrapper where_file(kWherePath, "id = 42");
  VsiMemTempWrapper sql_file(kSqlPath, "SELECT * FROM test_layer");

  auto opts = MakeOptions({std::string("-where"), std::string("@") + kWherePath,
                           std::string("-sql"), std::string("@") + kSqlPath});
  ASSERT_NE(nullptr, opts);

  // Missing files return null and report failure.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto bad_where = MakeOptions({"-where", "@/vsimem/nonexistent_where.txt"});
    EXPECT_EQ(nullptr, bad_where);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLErrorReset();
    auto bad_sql = MakeOptions({"-sql", "@/vsimem/nonexistent_sql.txt"});
    EXPECT_EQ(nullptr, bad_sql);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(OgrinfoLibTest, OptionsDialectWithWhereWarning) {
  WithQuietHandler quiet;
  CPLErrorReset();
  auto opts = MakeOptions({"-dialect", "OGRSQL", "-where", "x > 1"});
  ASSERT_NE(nullptr, opts);
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("-dialect is ignored with -where"));
}

TEST_F(OgrinfoLibTest, OptionsParsingWithBinaryOptions) {
  GDALVectorInfoOptionsForBinary bin_opts;
  CPLStringList args;
  args.AddString("-ro");
  args.AddString("-update");
  args.AddString("-q");
  args.AddString("-oo");
  args.AddString("FOO=BAR");
  args.AddString("-if");
  args.AddString("Memory");
  args.AddString("my_file");

  auto* opts = GDALVectorInfoOptionsNew(args.List(), &bin_opts);
  ASSERT_NE(nullptr, opts);
  GDALVectorInfoOptionsFree(opts);

  EXPECT_TRUE(bin_opts.bReadOnly);
  EXPECT_TRUE(bin_opts.bUpdate);
  EXPECT_FALSE(bin_opts.bVerbose);
  EXPECT_EQ("my_file", bin_opts.osFilename);
}

TEST_F(OgrinfoLibTest, OptionsParsingInvalidOptionReturnsNull) {
  WithQuietHandler quiet;
  CPLErrorReset();
  CPLStringList args;
  args.AddString("-invalid_nonexistent_flag");
  auto* opts = GDALVectorInfoOptionsNew(args.List(), nullptr);
  EXPECT_EQ(nullptr, opts);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST_F(OgrinfoLibTest, EmptyDatasetTextAndJson) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  // 1. Text mode default.
  {
    auto info = RunVectorInfo(ds.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("using driver `MEM' successful."));
  }

  // 2. JSON mode.
  {
    auto opts = MakeOptions({"-json"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("\"driverShortName\":\"MEM\""));
    EXPECT_THAT(info.get(), HasSubstr("\"layers\":[]"));
  }

  // 3. Super quiet mode.
  {
    auto opts = MakeOptions({"-q"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), Not(HasSubstr("INFO: Open of")));
  }
}

TEST_F(OgrinfoLibTest, LayerSummaryAndDetailsReporting) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  srs.SetCoordinateEpoch(2022.5);

  OGRLayer* layer = ds->CreateLayer("points_layer", &srs, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn field_int("int_col", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&field_int));
  OGRFieldDefn field_str("str_col", OFTString);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&field_str));
  OGRFieldDefn field_real("real_col", OFTReal);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&field_real));

  // Feature 1
  {
    auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
    feat->SetFID(1);
    feat->SetField(0, 100);
    feat->SetField(1, "first_val");
    feat->SetField(2, 1.5);
    auto pt = std::make_unique<OGRPoint>(10.0, 20.0);
    feat->SetGeometry(pt.get());
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }

  // Feature 2
  {
    auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
    feat->SetFID(2);
    feat->SetField(0, 200);
    feat->SetField(1, "second_val");
    feat->SetField(2, 2.5);
    auto pt = std::make_unique<OGRPoint>(30.0, 40.0);
    feat->SetGeometry(pt.get());
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }

  // 1. Text mode default: prints 1-line summary per layer.
  {
    auto info = RunVectorInfo(ds.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("1: points_layer (Point)"));
  }

  // 2. Text mode -al: prints full layer details and dumped features.
  {
    auto opts = MakeOptions({"-al", "-wkt_format", "WKT2"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Layer name: points_layer"));
    EXPECT_THAT(info.get(), HasSubstr("Geometry: Point"));
    EXPECT_THAT(info.get(), HasSubstr("Feature Count: 2"));
    EXPECT_THAT(
        info.get(),
        HasSubstr("Extent: (10.000000, 20.000000) - (30.000000, 40.000000)"));
    EXPECT_THAT(info.get(), HasSubstr("Layer SRS WKT:"));
    EXPECT_THAT(info.get(), HasSubstr("GEOGCRS[\"WGS 84\""));
    EXPECT_THAT(info.get(), HasSubstr("Coordinate epoch: 2022.5"));
    EXPECT_THAT(info.get(), HasSubstr("int_col: Integer"));
    EXPECT_THAT(info.get(), HasSubstr("str_col: String"));
    EXPECT_THAT(info.get(), HasSubstr("real_col: Real"));
    EXPECT_THAT(info.get(), HasSubstr("OGRFeature(points_layer):1"));
    EXPECT_THAT(info.get(), HasSubstr("first_val"));
    EXPECT_THAT(info.get(), HasSubstr("POINT (10 20)"));
    EXPECT_THAT(info.get(), HasSubstr("OGRFeature(points_layer):2"));
    EXPECT_THAT(info.get(), HasSubstr("second_val"));
  }

  // 3. Specific layer requested by name.
  {
    auto opts = MakeOptions({"dummy_file", "points_layer"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Layer name: points_layer"));
  }

  // 4. -so / -summary mode (summary only, no feature dump).
  {
    auto opts = MakeOptions({"-al", "-so"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Layer name: points_layer"));
    EXPECT_THAT(info.get(), Not(HasSubstr("OGRFeature(points_layer):1")));
  }

  // 5. -nocount, -noextent, -nogeomtype.
  {
    auto opts =
        MakeOptions({"-al", "-so", "-nocount", "-noextent", "-nogeomtype"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), Not(HasSubstr("Feature Count:")));
    EXPECT_THAT(info.get(), Not(HasSubstr("Extent:")));
    EXPECT_THAT(info.get(), Not(HasSubstr("Geometry: Point")));
  }

  // 6. -fid 1.
  {
    auto opts = MakeOptions({"-al", "-fid", "1"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("OGRFeature(points_layer):1"));
    EXPECT_THAT(info.get(), Not(HasSubstr("OGRFeature(points_layer):2")));
  }

  // 7. -limit 1.
  {
    auto opts = MakeOptions({"-al", "-limit", "1"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("OGRFeature(points_layer):1"));
    EXPECT_THAT(info.get(), Not(HasSubstr("OGRFeature(points_layer):2")));
  }

  // 8. -where filter.
  {
    auto opts = MakeOptions({"-al", "-where", "int_col = 200"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), Not(HasSubstr("OGRFeature(points_layer):1")));
    EXPECT_THAT(info.get(), HasSubstr("OGRFeature(points_layer):2"));
    layer->SetAttributeFilter(nullptr);
  }

  // 9. -spat spatial filter.
  {
    auto opts = MakeOptions({"-al", "-spat", "0", "0", "15", "25"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("OGRFeature(points_layer):1"));
    EXPECT_THAT(info.get(), Not(HasSubstr("OGRFeature(points_layer):2")));
    layer->SetSpatialFilter(nullptr);
  }

  // 10. -geom formats: NO, SUMMARY, WKT, ISO_WKT.
  {
    auto opts_no = MakeOptions({"-al", "-geom", "NO"});
    auto info_no = RunVectorInfo(ds.get(), opts_no.get());
    ASSERT_NE(nullptr, info_no);
    EXPECT_THAT(info_no.get(), Not(HasSubstr("POINT (10 20)")));

    auto opts_sum = MakeOptions({"-al", "-geom", "SUMMARY"});
    auto info_sum = RunVectorInfo(ds.get(), opts_sum.get());
    ASSERT_NE(nullptr, info_sum);
    EXPECT_THAT(info_sum.get(), HasSubstr("POINT"));

    auto opts_wkt = MakeOptions({"-al", "-geom", "WKT"});
    auto info_wkt = RunVectorInfo(ds.get(), opts_wkt.get());
    ASSERT_NE(nullptr, info_wkt);
    EXPECT_THAT(info_wkt.get(), HasSubstr("POINT (10 20)"));

    auto opts_iso = MakeOptions({"-al", "-geom", "ISO_WKT"});
    auto info_iso = RunVectorInfo(ds.get(), opts_iso.get());
    ASSERT_NE(nullptr, info_iso);
    EXPECT_THAT(info_iso.get(), HasSubstr("POINT (10 20)"));
  }

  // 11. -fields NO.
  {
    auto opts = MakeOptions({"-al", "-fields", "NO"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), Not(HasSubstr("first_val")));
  }

  // 12. JSON mode.
  {
    auto opts = MakeOptions({"-json"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("\"layers\":["));
    EXPECT_THAT(info.get(), HasSubstr("\"name\":\"points_layer\""));
    EXPECT_THAT(info.get(), HasSubstr("\"geometryFields\":["));
    EXPECT_THAT(info.get(), HasSubstr("\"type\":\"Point\""));
    EXPECT_THAT(info.get(), HasSubstr("\"featureCount\":2"));
    EXPECT_THAT(info.get(), HasSubstr("\"extent\":["));
    EXPECT_THAT(info.get(), HasSubstr("\"fields\":["));
    EXPECT_THAT(info.get(), HasSubstr("\"name\":\"int_col\""));
    EXPECT_THAT(info.get(), HasSubstr("\"type\":\"Integer\""));
  }
}

TEST_F(OgrinfoLibTest, AllFieldTypesAndAttributesReporting) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer = ds->CreateLayer("all_types", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, layer);

  // Field definitions
  OGRFieldDefn f_bool("f_bool", OFTInteger);
  f_bool.SetSubType(OFSTBoolean);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_bool));

  OGRFieldDefn f_int16("f_int16", OFTInteger);
  f_int16.SetSubType(OFSTInt16);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_int16));

  OGRFieldDefn f_int64("f_int64", OFTInteger64);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_int64));

  OGRFieldDefn f_float32("f_float32", OFTReal);
  f_float32.SetSubType(OFSTFloat32);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_float32));

  OGRFieldDefn f_json("f_json", OFTString);
  f_json.SetSubType(OFSTJSON);
  f_json.SetAlternativeName("json_alias");
  f_json.SetComment("JSON column comment");
  f_json.SetNullable(FALSE);
  f_json.SetUnique(TRUE);
  f_json.SetDefault("'{}'");
  f_json.SetDomainName("MyDomain");
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_json));

  OGRFieldDefn f_uuid("f_uuid", OFTString);
  f_uuid.SetSubType(OFSTUUID);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_uuid));

  OGRFieldDefn f_bin("f_bin", OFTBinary);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_bin));

  OGRFieldDefn f_date("f_date", OFTDate);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_date));

  OGRFieldDefn f_time("f_time", OFTTime);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_time));

  OGRFieldDefn f_dt("f_dt", OFTDateTime);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_dt));

  OGRFieldDefn f_int_list("f_int_list", OFTIntegerList);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_int_list));

  OGRFieldDefn f_int64_list("f_int64_list", OFTInteger64List);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_int64_list));

  OGRFieldDefn f_real_list("f_real_list", OFTRealList);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_real_list));

  OGRFieldDefn f_str_list("f_str_list", OFTStringList);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_str_list));

  // Add a feature with values populated
  {
    auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
    feat->SetField(0, 1);
    feat->SetField(1, 12);
    feat->SetField(2, static_cast<GIntBig>(9876543210LL));
    feat->SetField(3, 3.14);
    feat->SetField(4, "{\"a\":1}");
    feat->SetField(5, "123e4567-e89b-12d3-a456-426614174000");

    const GByte bin_data[] = {0xAB, 0xCD};
    feat->SetField(6, 2, const_cast<GByte*>(bin_data));

    feat->SetField(7, 2023, 11, 5, 0, 0, 0, 0);
    feat->SetField(8, 0, 0, 0, 14, 30, 0, 0);
    feat->SetField(9, 2023, 11, 5, 14, 30, 0, 0);

    const int int_vals[] = {1, 2, 3};
    feat->SetField(10, 3, int_vals);

    const GIntBig int64_vals[] = {10000000000LL, 20000000000LL};
    feat->SetField(11, 2, int64_vals);

    const double real_vals[] = {1.1, 2.2};
    feat->SetField(12, 2, real_vals);

    const char* str_vals[] = {"alpha", "beta", nullptr};
    feat->SetField(13, const_cast<char**>(str_vals));

    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }

  // 1. Text mode verification.
  {
    auto opts = MakeOptions({"-al"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("f_bool: Integer(Boolean)"));
    EXPECT_THAT(info.get(), HasSubstr("f_int16: Integer(Int16)"));
    EXPECT_THAT(info.get(), HasSubstr("f_int64: Integer64"));
    EXPECT_THAT(info.get(), HasSubstr("f_float32: Real(Float32)"));
    EXPECT_THAT(
        info.get(),
        HasSubstr("f_json: String(JSON) (0.0) UNIQUE NOT NULL DEFAULT '{}'"));
    EXPECT_THAT(info.get(), HasSubstr("alternative name=\"json_alias\""));
    EXPECT_THAT(info.get(), HasSubstr("domain name=MyDomain"));
    EXPECT_THAT(info.get(), HasSubstr("comment=JSON column comment"));
    EXPECT_THAT(info.get(), HasSubstr("f_bin: Binary"));
    EXPECT_THAT(info.get(), HasSubstr("f_date: Date"));
    EXPECT_THAT(info.get(), HasSubstr("f_time: Time"));
    EXPECT_THAT(info.get(), HasSubstr("f_dt: DateTime"));
    EXPECT_THAT(info.get(), HasSubstr("f_int_list: IntegerList"));
    EXPECT_THAT(info.get(), HasSubstr("f_int64_list: Integer64List"));
    EXPECT_THAT(info.get(), HasSubstr("f_real_list: RealList"));
    EXPECT_THAT(info.get(), HasSubstr("f_str_list: StringList"));
    EXPECT_THAT(info.get(), HasSubstr("(3:1,2,3)"));
    EXPECT_THAT(info.get(), HasSubstr("ABCD"));
  }

  // 2. JSON mode verification.
  {
    auto opts = MakeOptions({"-json"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("\"subType\":\"Boolean\""));
    EXPECT_THAT(info.get(), HasSubstr("\"subType\":\"Int16\""));
    EXPECT_THAT(info.get(), HasSubstr("\"subType\":\"JSON\""));
    EXPECT_THAT(info.get(), HasSubstr("\"alias\":\"json_alias\""));
    EXPECT_THAT(info.get(), HasSubstr("\"comment\":\"JSON column comment\""));
    EXPECT_THAT(info.get(), HasSubstr("\"nullable\":false"));
    EXPECT_THAT(info.get(), HasSubstr("\"uniqueConstraint\":true"));
    EXPECT_THAT(info.get(), HasSubstr("\"defaultValue\":\"'{}'\""));
    EXPECT_THAT(info.get(), HasSubstr("\"domainName\":\"MyDomain\""));
  }
}

TEST_F(OgrinfoLibTest, MultipleGeometryFieldsAndExtent3D) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer =
      ds->CreateLayer("multi_geom_layer", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRGeomFieldDefn geom_primary("primary_pt", wkbPoint25D);
  ASSERT_EQ(OGRERR_NONE, layer->CreateGeomField(&geom_primary));

  OGRGeomFieldDefn geom_secondary("secondary_poly", wkbPolygon);
  ASSERT_EQ(OGRERR_NONE, layer->CreateGeomField(&geom_secondary));

  // Add 3D feature
  {
    auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
    auto pt = std::make_unique<OGRPoint>(10.0, 20.0, 30.0);
    feat->SetGeomField(0, pt.get());

    auto poly = std::make_unique<OGRPolygon>();
    auto ring = std::make_unique<OGRLinearRing>();
    ring->addPoint(0, 0);
    ring->addPoint(5, 0);
    ring->addPoint(5, 5);
    ring->addPoint(0, 5);
    ring->addPoint(0, 0);
    poly->addRingDirectly(ring.release());
    feat->SetGeomField(1, poly.get());

    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }

  // 1. Text mode with -extent3D.
  {
    auto opts = MakeOptions({"-al", "-extent3D"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Extent (primary_pt):"));
    EXPECT_THAT(info.get(), HasSubstr("Geometry Column 1 = primary_pt"));
    EXPECT_THAT(info.get(), HasSubstr("Geometry Column 2 = secondary_poly"));
    EXPECT_THAT(info.get(), HasSubstr("POINT Z (10 20 30)"));
    EXPECT_THAT(info.get(), HasSubstr("POLYGON ((0 0,5 0,5 5,0 5,0 0))"));
  }

  // 2. Spatial filter on secondary geometry field.
  {
    auto opts = MakeOptions(
        {"-al", "-spat", "0", "0", "6", "6", "-geomfield", "secondary_poly"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("secondary_poly"));
  }

  // 3. JSON mode with -extent3D.
  {
    auto opts = MakeOptions({"-json", "-extent3D"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("\"geometryFields\":["));
    EXPECT_THAT(info.get(), HasSubstr("\"name\":\"primary_pt\""));
    EXPECT_THAT(info.get(), HasSubstr("\"name\":\"secondary_poly\""));
    EXPECT_THAT(info.get(), HasSubstr("\"extent3D\":["));
  }
}

TEST_F(OgrinfoLibTest, FieldDomainsReporting) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  // 1. Coded Field Domain
  std::vector<OGRCodedValue> coded_values;
  coded_values.push_back({CPLStrdup("1"), CPLStrdup("Red")});
  coded_values.push_back({CPLStrdup("2"), CPLStrdup("Green")});
  coded_values.push_back({CPLStrdup("3"), nullptr});
  coded_values.push_back({nullptr, nullptr});

  auto coded_domain = std::make_unique<OGRCodedFieldDomain>(
      "ColorsDomain", "Color domain description", OFTInteger, OFSTNone,
      std::move(coded_values));
  coded_domain->SetSplitPolicy(OFDSP_DUPLICATE);
  coded_domain->SetMergePolicy(OFDMP_DEFAULT_VALUE);
  std::string failure_reason;
  ASSERT_TRUE(ds->AddFieldDomain(std::move(coded_domain), failure_reason));

  // 2. Range Field Domain
  OGRField min_val;
  min_val.Real = 0.0;
  OGRField max_val;
  max_val.Real = 100.0;
  auto range_domain = std::make_unique<OGRRangeFieldDomain>(
      "PercentDomain", "Percentage range", OFTReal, OFSTNone, min_val,
      /*bMinIsIncluded=*/true, max_val, /*bMaxIsIncluded=*/false);
  range_domain->SetSplitPolicy(OFDSP_GEOMETRY_RATIO);
  range_domain->SetMergePolicy(OFDMP_SUM);
  ASSERT_TRUE(ds->AddFieldDomain(std::move(range_domain), failure_reason));

  // 3. Glob Field Domain
  auto glob_domain = std::make_unique<OGRGlobFieldDomain>(
      "CodeDomain", "Code glob", OFTString, OFSTNone, "CODE-*");
  glob_domain->SetSplitPolicy(OFDSP_DEFAULT_VALUE);
  glob_domain->SetMergePolicy(OFDMP_GEOMETRY_WEIGHTED);
  ASSERT_TRUE(ds->AddFieldDomain(std::move(glob_domain), failure_reason));

  // Text mode -fielddomain ColorsDomain.
  {
    auto opts = MakeOptions({"-fielddomain", "ColorsDomain"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Domain ColorsDomain:"));
    EXPECT_THAT(info.get(), HasSubstr("Description: Color domain description"));
    EXPECT_THAT(info.get(), HasSubstr("Type: coded"));
    EXPECT_THAT(info.get(), HasSubstr("Split policy: duplicate"));
    EXPECT_THAT(info.get(), HasSubstr("Merge policy: default value"));
    EXPECT_THAT(info.get(), HasSubstr("1: Red"));
    EXPECT_THAT(info.get(), HasSubstr("2: Green"));
  }

  // Text mode -fielddomain PercentDomain.
  {
    auto opts = MakeOptions({"-fielddomain", "PercentDomain"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Domain PercentDomain:"));
    EXPECT_THAT(info.get(), HasSubstr("Type: range"));
    EXPECT_THAT(info.get(), HasSubstr("Split policy: geometry ratio"));
    EXPECT_THAT(info.get(), HasSubstr("Merge policy: sum"));
  }

  // Text mode -fielddomain CodeDomain.
  {
    auto opts = MakeOptions({"-fielddomain", "CodeDomain"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Domain CodeDomain:"));
    EXPECT_THAT(info.get(), HasSubstr("Type: glob"));
    EXPECT_THAT(info.get(), HasSubstr("Glob: CODE-*"));
    EXPECT_THAT(info.get(), HasSubstr("Merge policy: geometry weighted"));
  }

  // JSON mode: reports all domains.
  {
    auto opts = MakeOptions({"-json"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("\"domains\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"ColorsDomain\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"type\":\"coded\""));
    EXPECT_THAT(info.get(), HasSubstr("\"codedValues\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"1\":\"Red\""));
    EXPECT_THAT(info.get(), HasSubstr("\"PercentDomain\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"type\":\"range\""));
    EXPECT_THAT(info.get(), HasSubstr("\"CodeDomain\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"type\":\"glob\""));
  }

  // Missing domain error.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto opts = MakeOptions({"-fielddomain", "NonExistentDomain"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    EXPECT_EQ(nullptr, info);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(OgrinfoLibTest, RelationshipsReporting) {
  auto ds = std::make_unique<FakeRelationshipDataset>();

  // 1. OneToMany Composite Relationship
  auto rel1 = std::make_unique<GDALRelationship>("owner_pets", "owners", "pets",
                                                 GRC_ONE_TO_MANY);
  rel1->SetType(GRT_COMPOSITE);
  rel1->SetLeftTableFields({"owner_id"});
  rel1->SetRightTableFields({"pet_owner_id"});
  rel1->SetForwardPathLabel("owns");
  rel1->SetBackwardPathLabel("owned_by");
  rel1->SetRelatedTableType("media");
  ds->AddFakeRelationship(std::move(rel1));

  // 2. ManyToMany Association Relationship
  auto rel2 = std::make_unique<GDALRelationship>("students_classes", "students",
                                                 "classes", GRC_MANY_TO_MANY);
  rel2->SetType(GRT_ASSOCIATION);
  rel2->SetMappingTableName("enrollments");
  rel2->SetLeftTableFields({"student_id"});
  rel2->SetRightTableFields({"class_id"});
  rel2->SetLeftMappingTableFields({"m_student_id"});
  rel2->SetRightMappingTableFields({"m_class_id"});
  ds->AddFakeRelationship(std::move(rel2));

  // 1. Text mode default: 1-line summary per relationship.
  {
    auto info = RunVectorInfo(ds.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(
        info.get(),
        HasSubstr("Relationship: owner_pets (Composite, owners, pets)"));
    EXPECT_THAT(
        info.get(),
        HasSubstr(
            "Relationship: students_classes (Association, students, classes)"));
  }

  // 2. Text mode -al: detailed reporting per relationship.
  {
    auto opts = MakeOptions({"-al"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Relationship: owner_pets"));
    EXPECT_THAT(info.get(), HasSubstr("Type: Composite"));
    EXPECT_THAT(info.get(), HasSubstr("Cardinality: OneToMany"));
    EXPECT_THAT(info.get(), HasSubstr("Left table name: owners"));
    EXPECT_THAT(info.get(), HasSubstr("Right table name: pets"));
    EXPECT_THAT(info.get(), HasSubstr("Left table fields: owner_id"));
    EXPECT_THAT(info.get(), HasSubstr("Right table fields: pet_owner_id"));
    EXPECT_THAT(info.get(), HasSubstr("Forward path label: owns"));
    EXPECT_THAT(info.get(), HasSubstr("Backward path label: owned_by"));
    EXPECT_THAT(info.get(), HasSubstr("Related table type: media"));

    EXPECT_THAT(info.get(), HasSubstr("Relationship: students_classes"));
    EXPECT_THAT(info.get(), HasSubstr("Type: Association"));
    EXPECT_THAT(info.get(), HasSubstr("Cardinality: ManyToMany"));
    EXPECT_THAT(info.get(), HasSubstr("Mapping table name: enrollments"));
    EXPECT_THAT(info.get(),
                HasSubstr("Left mapping table fields: m_student_id"));
    EXPECT_THAT(info.get(),
                HasSubstr("Right mapping table fields: m_class_id"));
  }

  // 3. JSON mode.
  {
    auto opts = MakeOptions({"-json"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("\"relationships\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"owner_pets\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"type\":\"Composite\""));
    EXPECT_THAT(info.get(), HasSubstr("\"cardinality\":\"OneToMany\""));
    EXPECT_THAT(info.get(), HasSubstr("\"left_table_name\":\"owners\""));
    EXPECT_THAT(info.get(), HasSubstr("\"right_table_name\":\"pets\""));
    EXPECT_THAT(info.get(), HasSubstr("\"students_classes\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"cardinality\":\"ManyToMany\""));
    EXPECT_THAT(info.get(),
                HasSubstr("\"mapping_table_name\":\"enrollments\""));
  }
}

TEST_F(OgrinfoLibTest, SQLQueriesAndDialects) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer = ds->CreateLayer("items", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn field_id("item_id", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&field_id));
  OGRFieldDefn field_name("item_name", OFTString);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&field_name));

  for (int i = 1; i <= 5; ++i) {
    auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
    feat->SetField(0, i * 10);
    feat->SetField(1, std::string("name_").append(std::to_string(i)).c_str());
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }

  // 1. Text mode basic SQL.
  {
    auto opts = MakeOptions(
        {"-sql", "SELECT item_name FROM items WHERE item_id >= 40"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("name_4"));
    EXPECT_THAT(info.get(), HasSubstr("name_5"));
    EXPECT_THAT(info.get(), Not(HasSubstr("name_1")));
  }

  // 2. SQL with -dialect OGRSQL and -where.
  {
    auto opts = MakeOptions({"-sql", "SELECT * FROM items", "-dialect",
                             "OGRSQL", "-where", "item_id = 20"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("name_2"));
    EXPECT_THAT(info.get(), Not(HasSubstr("name_3")));
  }

  // 3. JSON mode SQL query.
  {
    auto opts = MakeOptions(
        {"-json", "-sql", "SELECT * FROM items WHERE item_id = 30"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("\"layers\":["));
  }

  // 4. Invalid SQL query returns null and emits error.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto opts =
        MakeOptions({"-sql", "SELECT SYNTAX_ERROR FROM non_existent_table"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    EXPECT_EQ(nullptr, info);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(OgrinfoLibTest, RandomLayerReadingMode) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer1 = ds->CreateLayer("layer1", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, layer1);
  OGRFieldDefn f1("val1", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, layer1->CreateField(&f1));
  {
    auto feat = std::make_unique<OGRFeature>(layer1->GetLayerDefn());
    feat->SetField(0, 111);
    ASSERT_EQ(OGRERR_NONE, layer1->CreateFeature(feat.get()));
  }

  OGRLayer* layer2 = ds->CreateLayer("layer2", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, layer2);
  OGRFieldDefn f2("val2", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, layer2->CreateField(&f2));
  {
    auto feat = std::make_unique<OGRFeature>(layer2->GetLayerDefn());
    feat->SetField(0, 222);
    ASSERT_EQ(OGRERR_NONE, layer2->CreateFeature(feat.get()));
  }

  // 1. Text mode -rl.
  {
    auto opts = MakeOptions({"-rl"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Layer name: layer1"));
    EXPECT_THAT(info.get(), HasSubstr("Layer name: layer2"));
    EXPECT_THAT(info.get(), HasSubstr("Feature Count: 1"));
  }

  // 2. -rl with layer filter.
  {
    ds->ResetReading();
    auto opts = MakeOptions({"-rl", "dummy", "layer1"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Layer name: layer1"));
    EXPECT_THAT(info.get(), Not(HasSubstr("Layer name: layer2")));
  }
}

TEST_F(OgrinfoLibTest, MetadataReporting) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  ds->SetMetadataItem("DATASET_KEY", "DATASET_VAL");
  ds->SetMetadataItem("CUSTOM_KEY", "CUSTOM_VAL", "CustomMD");

  OGRLayer* layer = ds->CreateLayer("layer_md", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, layer);
  layer->SetMetadataItem("LAYER_KEY", "LAYER_VAL");

  // 1. Text mode default.
  {
    auto opts = MakeOptions({"-al"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Metadata:"));
    EXPECT_THAT(info.get(), HasSubstr("DATASET_KEY=DATASET_VAL"));
    EXPECT_THAT(info.get(), HasSubstr("LAYER_KEY=LAYER_VAL"));
  }

  // 2. Text mode -mdd CustomMD.
  {
    auto opts = MakeOptions({"-mdd", "CustomMD"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Metadata (CustomMD):"));
    EXPECT_THAT(info.get(), HasSubstr("CUSTOM_KEY=CUSTOM_VAL"));
  }

  // 3. Text mode -listmdd.
  {
    auto opts = MakeOptions({"-listmdd"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Metadata domains:"));
    EXPECT_THAT(info.get(), HasSubstr("CustomMD"));
  }

  // 4. Text mode -nomd suppresses metadata.
  {
    auto opts = MakeOptions({"-nomd"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), Not(HasSubstr("DATASET_KEY=DATASET_VAL")));
  }

  // 5. JSON mode metadata.
  {
    auto opts = MakeOptions({"-json", "-mdd", "CustomMD", "-listmdd"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("\"metadata\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"DATASET_KEY\":\"DATASET_VAL\""));
    EXPECT_THAT(info.get(), HasSubstr("\"CustomMD\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"CUSTOM_KEY\":\"CUSTOM_VAL\""));
    EXPECT_THAT(info.get(), HasSubstr("\"metadataDomains\":["));
  }
}

TEST_F(OgrinfoLibTest, FieldDomainsComprehensive) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  std::string failure_reason;

  // OFTInteger Range Domain (min included, max excluded)
  OGRField min_int;
  min_int.Integer = 10;
  OGRField max_int;
  max_int.Integer = 100;
  auto int_domain = std::make_unique<OGRRangeFieldDomain>(
      "IntRange", "Integer range", OFTInteger, OFSTNone, min_int,
      /*bMinIsIncluded=*/true, max_int, /*bMaxIsIncluded=*/false);
  ASSERT_TRUE(ds->AddFieldDomain(std::move(int_domain), failure_reason));

  // OFTInteger Range Domain (min unset, max included)
  OGRField unset_field;
  OGR_RawField_SetUnset(&unset_field);
  OGRField max_int2;
  max_int2.Integer = 50;
  auto int_domain_min_unset = std::make_unique<OGRRangeFieldDomain>(
      "IntRangeMinUnset", "Min unset", OFTInteger, OFSTNone, unset_field,
      /*bMinIsIncluded=*/false, max_int2, /*bMaxIsIncluded=*/true);
  ASSERT_TRUE(
      ds->AddFieldDomain(std::move(int_domain_min_unset), failure_reason));

  // OFTInteger64 Range Domain (min excluded, max included)
  OGRField min_int64;
  min_int64.Integer64 = 10000000000LL;
  OGRField max_int64;
  max_int64.Integer64 = 20000000000LL;
  auto int64_domain = std::make_unique<OGRRangeFieldDomain>(
      "Int64Range", "Integer64 range", OFTInteger64, OFSTNone, min_int64,
      /*bMinIsIncluded=*/false, max_int64, /*bMaxIsIncluded=*/true);
  ASSERT_TRUE(ds->AddFieldDomain(std::move(int64_domain), failure_reason));

  // OFTInteger64 Range Domain (max unset)
  OGRField min_int64_2;
  min_int64_2.Integer64 = 500LL;
  auto int64_domain_max_unset = std::make_unique<OGRRangeFieldDomain>(
      "Int64RangeMaxUnset", "Max unset", OFTInteger64, OFSTNone, min_int64_2,
      /*bMinIsIncluded=*/true, unset_field, /*bMaxIsIncluded=*/false);
  ASSERT_TRUE(
      ds->AddFieldDomain(std::move(int64_domain_max_unset), failure_reason));

  // OFTDateTime Range Domain (min included, max excluded)
  OGRField min_dt{};
  min_dt.Date.Year = 2020;
  min_dt.Date.Month = 1;
  min_dt.Date.Day = 1;
  min_dt.Date.Hour = 0;
  min_dt.Date.Minute = 0;
  min_dt.Date.Second = 0;
  OGRField max_dt{};
  max_dt.Date.Year = 2025;
  max_dt.Date.Month = 12;
  max_dt.Date.Day = 31;
  max_dt.Date.Hour = 23;
  max_dt.Date.Minute = 59;
  max_dt.Date.Second = 59;
  auto dt_domain = std::make_unique<OGRRangeFieldDomain>(
      "DateTimeRange", "DateTime range", OFTDateTime, OFSTNone, min_dt,
      /*bMinIsIncluded=*/true, max_dt, /*bMaxIsIncluded=*/false);
  ASSERT_TRUE(ds->AddFieldDomain(std::move(dt_domain), failure_reason));

  // Coded domain with subtype OFSTBoolean
  std::vector<OGRCodedValue> coded_vals;
  coded_vals.push_back({CPLStrdup("0"), CPLStrdup("False")});
  coded_vals.push_back({CPLStrdup("1"), CPLStrdup("True")});
  coded_vals.push_back({nullptr, nullptr});
  auto coded_bool_domain = std::make_unique<OGRCodedFieldDomain>(
      "BoolDomain", "Boolean domain", OFTInteger, OFSTBoolean,
      std::move(coded_vals));
  ASSERT_TRUE(ds->AddFieldDomain(std::move(coded_bool_domain), failure_reason));

  // Text mode testing
  {
    auto opts = MakeOptions({"-fielddomain", "IntRange"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Domain IntRange:"));
    EXPECT_THAT(info.get(), HasSubstr("Minimum value: 10"));
    EXPECT_THAT(info.get(), HasSubstr("Maximum value: 100 (excluded)"));
  }
  {
    auto opts = MakeOptions({"-fielddomain", "Int64Range"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Domain Int64Range:"));
    EXPECT_THAT(info.get(), HasSubstr("Minimum value: 10000000000 (excluded)"));
    EXPECT_THAT(info.get(), HasSubstr("Maximum value: 20000000000"));
  }
  {
    auto opts = MakeOptions({"-fielddomain", "DateTimeRange"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Domain DateTimeRange:"));
    EXPECT_THAT(info.get(), HasSubstr("Minimum value: 2020-01-01T00:00:00"));
    EXPECT_THAT(info.get(),
                HasSubstr("Maximum value: 2025-12-31T23:59:59 (excluded)"));
  }
  {
    auto opts = MakeOptions({"-fielddomain", "BoolDomain"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Domain BoolDomain:"));
    EXPECT_THAT(info.get(), HasSubstr("Field type: Integer(Boolean)"));
  }

  // JSON mode testing
  {
    auto opts = MakeOptions({"-json"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("\"IntRange\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"minValue\":10"));
    EXPECT_THAT(info.get(), HasSubstr("\"minValueIncluded\":true"));
    EXPECT_THAT(info.get(), HasSubstr("\"maxValue\":100"));
    EXPECT_THAT(info.get(), HasSubstr("\"maxValueIncluded\":false"));
    EXPECT_THAT(info.get(), HasSubstr("\"Int64Range\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"minValue\":10000000000"));
    EXPECT_THAT(info.get(), HasSubstr("\"DateTimeRange\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"minValue\":\"2020-01-01T00:00:00\""));
    EXPECT_THAT(info.get(), HasSubstr("\"BoolDomain\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"fieldSubType\":\"Boolean\""));
  }
}

TEST_F(OgrinfoLibTest, JsonFeaturesDumpingComprehensive) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer = ds->CreateLayer("feat_layer", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn f_int("f_int", OFTInteger);
  OGRFieldDefn f_bool("f_bool", OFTInteger);
  f_bool.SetSubType(OFSTBoolean);
  OGRFieldDefn f_int64("f_int64", OFTInteger64);
  OGRFieldDefn f_real("f_real", OFTReal);
  OGRFieldDefn f_str("f_str", OFTString);
  OGRFieldDefn f_json("f_json", OFTString);
  f_json.SetSubType(OFSTJSON);
  OGRFieldDefn f_null("f_null", OFTString);
  OGRFieldDefn f_date("f_date", OFTDate);
  OGRFieldDefn f_time("f_time", OFTTime);
  OGRFieldDefn f_dt("f_dt", OFTDateTime);

  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_int));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_bool));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_int64));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_real));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_str));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_json));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_null));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_date));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_time));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_dt));

  OGRGeomCoordinatePrecision prec;
  prec.dfXYResolution = 0.001;
  prec.dfZResolution = 0.01;
  layer->GetLayerDefn()->GetGeomFieldDefn(0)->SetCoordinatePrecision(prec);

  // Feature 1: all fields set
  {
    auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
    feat->SetField("f_int", 42);
    feat->SetField("f_bool", 1);
    feat->SetField("f_int64", static_cast<GIntBig>(9876543210LL));
    feat->SetField("f_real", 3.14159);
    feat->SetField("f_str", "hello_text");
    feat->SetField("f_json", "{\"key\": \"val\"}");
    feat->SetFieldNull(6);
    feat->SetField("f_date", 2024, 6, 15, 0, 0, 0, 0);
    feat->SetField("f_time", 0, 0, 0, 12, 30, 45, 0);
    feat->SetField("f_dt", 2024, 6, 15, 12, 30, 45, 0);
    auto pt = std::make_unique<OGRPoint>(12.3456, 78.9012);
    feat->SetGeometry(pt.get());
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }

  // Feature 2: null geometry
  {
    auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
    feat->SetField("f_int", 1);
    feat->SetGeometry(nullptr);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }

  auto opts = MakeOptions({"-json", "-features"});
  auto info = RunVectorInfo(ds.get(), opts.get());
  ASSERT_NE(nullptr, info);
  EXPECT_THAT(info.get(), HasSubstr("\"features\":["));
  EXPECT_THAT(info.get(), HasSubstr("\"type\":\"Feature\""));
  EXPECT_THAT(info.get(), HasSubstr("\"properties\":{"));
  EXPECT_THAT(info.get(), HasSubstr("\"f_int\":42"));
  EXPECT_THAT(info.get(), HasSubstr("\"f_bool\":true"));
  EXPECT_THAT(info.get(), HasSubstr("\"f_int64\":9876543210"));
  EXPECT_THAT(info.get(), HasSubstr("\"f_str\":\"hello_text\""));
  EXPECT_THAT(info.get(), HasSubstr("\"f_null\":null"));
  EXPECT_THAT(info.get(), HasSubstr("\"key\":\"val\""));
  EXPECT_THAT(info.get(), HasSubstr("\"geometry\":{"));
  EXPECT_THAT(info.get(), HasSubstr("\"Point\""));
  EXPECT_THAT(info.get(), HasSubstr("\"geometry\":null"));
}

TEST_F(OgrinfoLibTest, JsonFeaturesMultiGeometryAndGeomWkt) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer = ds->CreateLayer("multi_geom", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRGeomFieldDefn g1("geom_pt", wkbPoint);
  OGRGeomFieldDefn g2("geom_poly", wkbPolygon);
  ASSERT_EQ(OGRERR_NONE, layer->CreateGeomField(&g1));
  ASSERT_EQ(OGRERR_NONE, layer->CreateGeomField(&g2));

  {
    auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
    auto pt = std::make_unique<OGRPoint>(1.0, 2.0);
    auto poly = std::make_unique<OGRPolygon>();
    auto ring = std::make_unique<OGRLinearRing>();
    ring->addPoint(0, 0);
    ring->addPoint(1, 0);
    ring->addPoint(1, 1);
    ring->addPoint(0, 1);
    ring->addPoint(0, 0);
    poly->addRingDirectly(ring.release());

    feat->SetGeomFieldDirectly(0, pt.release());
    feat->SetGeomFieldDirectly(1, poly.release());
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }

  auto opts = MakeOptions({"-json", "-features"});
  auto info = RunVectorInfo(ds.get(), opts.get());
  ASSERT_NE(nullptr, info);
  EXPECT_THAT(info.get(), HasSubstr("\"geometries\":["));
  EXPECT_THAT(info.get(), HasSubstr("\"Point\""));
  EXPECT_THAT(info.get(), HasSubstr("\"Polygon\""));
}

TEST_F(OgrinfoLibTest, HierarchicalLayersWithGroups) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->CreateMultiDimensional("", nullptr, nullptr));
  ASSERT_NE(nullptr, ds);

  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);
  auto sub1 = root->CreateGroup("subgroup_one");
  ASSERT_NE(nullptr, sub1);
  auto sub2 = sub1->CreateGroup("nested_group");
  ASSERT_NE(nullptr, sub2);

  // 1. Text mode default.
  {
    auto info = RunVectorInfo(ds.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Group subgroup_one:"));
    EXPECT_THAT(info.get(), HasSubstr("Group   nested_group:"));
  }

  // 2. JSON mode.
  {
    auto opts = MakeOptions({"-json"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("\"rootGroup\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"name\":\"subgroup_one\""));
    EXPECT_THAT(info.get(), HasSubstr("\"name\":\"nested_group\""));
  }
}

TEST_F(OgrinfoLibTest, FieldTimezonesAndFormatting) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer = ds->CreateLayer("tz_layer", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn f_loc("f_loc", OFTDateTime);
  f_loc.SetTZFlag(OGR_TZFLAG_LOCALTIME);
  OGRFieldDefn f_mix("f_mix", OFTDateTime);
  f_mix.SetTZFlag(OGR_TZFLAG_MIXED_TZ);
  OGRFieldDefn f_utc("f_utc", OFTDateTime);
  f_utc.SetTZFlag(OGR_TZFLAG_UTC);
  OGRFieldDefn f_plus("f_plus", OFTDateTime);
  f_plus.SetTZFlag(OGR_TZFLAG_UTC + 8);  // +02:00
  OGRFieldDefn f_minus("f_minus", OFTDateTime);
  f_minus.SetTZFlag(OGR_TZFLAG_UTC - 20);  // -05:00
  OGRFieldDefn f_prec("f_prec", OFTReal);
  f_prec.SetWidth(8);
  f_prec.SetPrecision(3);

  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_loc));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_mix));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_utc));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_plus));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_minus));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_prec));

  // Text mode
  {
    auto opts = MakeOptions({"-al", "-so"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("f_loc: DateTime (localtime)"));
    EXPECT_THAT(info.get(), HasSubstr("f_mix: DateTime (mixed timezones)"));
    EXPECT_THAT(info.get(), HasSubstr("f_utc: DateTime (UTC)"));
    EXPECT_THAT(info.get(), HasSubstr("f_plus: DateTime (+02:00)"));
    EXPECT_THAT(info.get(), HasSubstr("f_minus: DateTime (-05:00)"));
    EXPECT_THAT(info.get(), HasSubstr("f_prec: Real (8.3)"));
  }

  // JSON mode
  {
    auto opts = MakeOptions({"-json"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("\"timezone\":\"localtime\""));
    EXPECT_THAT(info.get(), HasSubstr("\"timezone\":\"mixed timezones\""));
    EXPECT_THAT(info.get(), HasSubstr("\"timezone\":\"UTC\""));
    EXPECT_THAT(info.get(), HasSubstr("\"timezone\":\"+02:00\""));
    EXPECT_THAT(info.get(), HasSubstr("\"timezone\":\"-05:00\""));
    EXPECT_THAT(info.get(), HasSubstr("\"width\":8"));
    EXPECT_THAT(info.get(), HasSubstr("\"precision\":3"));
  }
}

TEST_F(OgrinfoLibTest, XmlAndJsonMetadataDomains) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  const char* const xml_md[] = {"<root><val>123</val></root>", nullptr};
  ds->SetMetadata(const_cast<char**>(xml_md), "xml:my_xml");

  const char* const json_md[] = {"{\"my_key\": 456}", nullptr};
  ds->SetMetadata(const_cast<char**>(json_md), "json:my_json");

  // Text mode
  {
    auto opts = MakeOptions({"-mdd", "xml:my_xml"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("<root><val>123</val></root>"));
  }
  {
    auto opts = MakeOptions({"-mdd", "json:my_json"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("{\"my_key\": 456}"));
  }

  // JSON mode
  {
    auto opts =
        MakeOptions({"-json", "-mdd", "xml:my_xml", "-mdd", "json:my_json"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(),
                HasSubstr("\"xml:my_xml\":\"<root><val>123</val></root>\""));
    EXPECT_THAT(info.get(), HasSubstr("\"json:my_json\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"my_key\":456"));
  }
}

TEST_F(OgrinfoLibTest, SingleLayer3DExtentAndPrivateLayerAndTitle) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer = ds->CreateLayer("single_3d", nullptr, wkbPoint25D, nullptr);
  ASSERT_NE(nullptr, layer);
  layer->SetMetadataItem("TITLE", "Layer Custom Title");

  {
    auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
    auto pt = std::make_unique<OGRPoint>(10.0, 20.0, 30.0);
    feat->SetGeometry(pt.get());
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }

  // Single layer 3D extent in text mode
  {
    auto opts = MakeOptions({"-al", "-extent3D"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(
        info.get(),
        HasSubstr("Extent: (10.000000, 20.000000, 30.000000) - (10.000000, "
                  "20.000000, 30.000000)"));
  }

  // Title in text mode and JSON mode
  {
    auto info = RunVectorInfo(ds.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("(title: Layer Custom Title)"));
  }
  {
    auto opts = MakeOptions({"--cli", "-so", "-json"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("\"title\":\"Layer Custom Title\""));
  }

  // Private layer in text mode
  FakePrivateLayerDataset priv_ds(ds.get());
  {
    auto info = RunVectorInfo(&priv_ds);
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("[private]"));
  }
}

TEST_F(OgrinfoLibTest, BinaryOptionsAndErrorBranches) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);
  OGRLayer* layer = ds->CreateLayer("lyr", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, layer);
  OGRFieldDefn f_num("num", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_num));
  {
    auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
    feat->SetField("num", 1);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }

  // Binary options: -if warning on unknown driver, -oo, filename
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorInfoOptionsForBinary sOptionsForBinary;
    const std::vector<std::string> args = {
        "-if",        "UnknownDriverFoo", "-if", "MEM", "-oo",
        "NAME=VALUE", "my_filename.shp"};
    CPLStringList arg_list;
    for (const auto& arg : args) {
      arg_list.AddString(arg.c_str());
    }
    auto* opts = GDALVectorInfoOptionsNew(arg_list.List(), &sOptionsForBinary);
    ASSERT_NE(nullptr, opts);
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_EQ("my_filename.shp", sOptionsForBinary.osFilename);
    GDALVectorInfoOptionsFree(opts);
  }

  // -stdout option
  {
    auto opts = MakeOptions({"-stdout"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
  }

  // -sql with ignored layer name
  {
    auto opts = MakeOptions(
        {"-sql", "SELECT num FROM lyr", "dummy_file", "ignored_layer"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(),
                HasSubstr("layer names ignored in combination with -sql."));
  }

  // -sql with -where filter on result set
  {
    auto opts =
        MakeOptions({"-sql", "SELECT num FROM lyr", "-where", "num = 1"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("num: Integer"));
  }

  // -sql with invalid SQL syntax returns nullptr
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto opts = MakeOptions({"-sql", "INVALID SYNTAX !@#$"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    EXPECT_EQ(nullptr, info);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // Non-existent layer requested returns nullptr
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto opts = MakeOptions({"dummy_file", "non_existent_layer"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    EXPECT_EQ(nullptr, info);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // Repeat count -rc 2
  {
    auto opts = MakeOptions({"-rc", "2", "-al"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
  }

  // Summary cli with -json
  {
    auto opts = MakeOptions({"--cli", "-so", "-json"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
  }
}

TEST_F(OgrinfoLibTest, SupportedSRSListAndCoordinatePrecision) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayer* base_layer =
      ds->CreateLayer("srs_layer", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, base_layer);

  OGRGeomCoordinatePrecision prec;
  prec.dfXYResolution = 0.001;
  prec.dfZResolution = 0.01;
  prec.dfMResolution = 0.1;
  prec.oFormatSpecificOptions["OpenFileGDB"].SetNameValue("XY_TOLERANCE",
                                                          "0.001");
  prec.oFormatSpecificOptions["OpenFileGDB"].SetNameValue("INT_VAL", "10");
  prec.oFormatSpecificOptions["OpenFileGDB"].SetNameValue("REAL_VAL", "2.5");

  OGRGeomFieldDefn gfld("geom_prec", wkbPoint);
  gfld.SetCoordinatePrecision(prec);
  ASSERT_EQ(OGRERR_NONE, base_layer->CreateGeomField(&gfld));

  FakeLayerWithSupportedSRS fake_layer(base_layer);
  FakeDatasetWithSupportedSRS fake_ds(&fake_layer);

  // 1. Text mode
  {
    auto opts = MakeOptions({"-al", "-so"});
    auto info = RunVectorInfo(&fake_ds, opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Supported SRS: EPSG:4326, EPSG:4322"));
  }

  // 2. JSON mode
  {
    auto opts = MakeOptions({"-json"});
    auto info = RunVectorInfo(&fake_ds, opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("\"supportedSRSList\":["));
    EXPECT_THAT(info.get(), HasSubstr("\"authority\":\"EPSG\""));
    EXPECT_THAT(info.get(), HasSubstr("\"code\":\"4326\""));
    EXPECT_THAT(info.get(), HasSubstr("\"xyCoordinateResolution\":0.001"));
    EXPECT_THAT(info.get(), HasSubstr("\"zCoordinateResolution\":0.01"));
    EXPECT_THAT(info.get(), HasSubstr("\"mCoordinateResolution\":0.1"));
    EXPECT_THAT(info.get(),
                HasSubstr("\"coordinatePrecisionFormatSpecificOptions\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"INT_VAL\":10"));
    EXPECT_THAT(info.get(), HasSubstr("\"REAL_VAL\":2.5"));
  }
}

TEST_F(OgrinfoLibTest, RandomLayerReadingWithSpatialFilter) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer = ds->CreateLayer("rl_layer", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  auto pt = std::make_unique<OGRPoint>(5.0, 5.0);
  feat->SetGeometry(pt.get());
  ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));

  // 1. -rl with -spat
  {
    auto opts = MakeOptions({"-rl", "-spat", "0", "0", "10", "10"});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Layer name: rl_layer"));
  }

  // 2. -rl with -spat and -geomfield
  {
    ds->ResetReading();
    auto opts =
        MakeOptions({"-rl", "-spat", "0", "0", "10", "10", "-geomfield", ""});
    auto info = RunVectorInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Layer name: rl_layer"));
  }
}

TEST_F(OgrinfoLibTest, LayerSummaryWithMultipleGeomFields) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer = ds->CreateLayer("two_geoms", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, layer);
  OGRGeomFieldDefn g1("pt", wkbPoint);
  OGRGeomFieldDefn g2("poly", wkbPolygon);
  ASSERT_EQ(OGRERR_NONE, layer->CreateGeomField(&g1));
  ASSERT_EQ(OGRERR_NONE, layer->CreateGeomField(&g2));

  // Text summary mode (no -al) calls PrintLayerSummary with nGeomFieldCount > 1
  auto info = RunVectorInfo(ds.get());
  ASSERT_NE(nullptr, info);
  EXPECT_THAT(info.get(), HasSubstr("1: two_geoms (Point, Polygon)"));
}

TEST_F(OgrinfoLibTest, JsonFeatureWithCurveGeometryAndScalarJson) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer =
      ds->CreateLayer("curve_layer", nullptr, wkbCompoundCurve, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn f_num_json("f_num_json", OFTString);
  f_num_json.SetSubType(OFSTJSON);
  OGRFieldDefn f_real_json("f_real_json", OFTString);
  f_real_json.SetSubType(OFSTJSON);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_num_json));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_real_json));

  auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  feat->SetField("f_num_json", "123");
  feat->SetField("f_real_json", "45.67");

  auto curve = std::make_unique<OGRCompoundCurve>();
  auto ls = std::make_unique<OGRLineString>();
  ls->addPoint(0, 0);
  ls->addPoint(1, 1);
  curve->addCurveDirectly(ls.release());
  feat->SetGeometryDirectly(curve.release());
  ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));

  auto opts = MakeOptions({"-json", "-features"});
  auto info = RunVectorInfo(ds.get(), opts.get());
  ASSERT_NE(nullptr, info);
  EXPECT_THAT(info.get(), HasSubstr("\"wkt_geometry\":\"COMPOUNDCURVE"));
  EXPECT_THAT(info.get(), HasSubstr("\"f_num_json\":123"));
  EXPECT_THAT(info.get(), HasSubstr("\"f_real_json\":45.67"));
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover remaining unreachable/defensive branches in
// apps/ogrinfo_lib.cpp.
