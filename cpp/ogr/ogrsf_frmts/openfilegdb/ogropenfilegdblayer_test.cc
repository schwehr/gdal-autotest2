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
// Tests Open FileGDB layer implementation (ogropenfilegdblayer.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/ogropenfilegdblayer.cpp
//   https://gdal.org/drivers/vector/openfilegdb.html

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_swq.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/filegdbtable.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/filegdbtable_priv.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/ogr_openfilegdb.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

extern "C" void RegisterOGROpenFileGDB();

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr const char* kTestDir = "/vsimem/ogropenfilegdblayer_test";

inline CPLErr CloseDS(GDALDataset& ds) { return ds.Close(); }

inline CPLErr CloseDS(GDALDataset* ds) {
  return ds != nullptr ? ds->Close() : CE_None;
}

class OgrOpenFileGdbLayerTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    GDALAllRegister();
    RegisterOGROpenFileGDB();
  }

  static void TearDownTestSuite() {
    GDALDestroyDriverManager();
    OSRCleanup();
  }

  void SetUp() override { VSIMkdirRecursive(kTestDir, 0755); }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

std::unique_ptr<OGROpenFileGDBDataSource> OpenGDB(
    const std::string& path, int nOpenFlags = GDAL_OF_VECTOR | GDAL_OF_READONLY,
    CSLConstList open_options = nullptr) {
  GDALOpenInfo oOpenInfo(path.c_str(), nOpenFlags);
  oOpenInfo.papszOpenOptions = const_cast<char**>(open_options);
  auto ds = std::make_unique<OGROpenFileGDBDataSource>();
  bool bRetry = false;
  if (!ds->Open(&oOpenInfo, bRetry)) return nullptr;
  return ds;
}

// -----------------------------------------------------------------------------
// Test 1: BuildGeometryColumnGDBv10AndMultiPatch
// -----------------------------------------------------------------------------
TEST_F(OgrOpenFileGdbLayerTest, BuildGeometryColumnGDBv10AndMultiPatch) {
  OGROpenFileGDBDataSource ds;

  // 1. Invalid XML definition
  {
    WithQuietHandler quiet;
    OGROpenFileGDBLayer lyr(&ds, "/vsimem/dummy.gdbtable", "lyr_bad_xml",
                            "<unclosed_xml", "", false);
    EXPECT_TRUE(lyr.GetXMLDefinition().empty());
    EXPECT_EQ(wkbUnknown, lyr.GetGeomType());
  }

  // 2. XML without DEFeatureClassInfo or DETableInfo
  {
    WithQuietHandler quiet;
    OGROpenFileGDBLayer lyr(&ds, "/vsimem/dummy.gdbtable", "lyr_no_info",
                            "<root><other/></root>", "", false);
    EXPECT_TRUE(lyr.GetXMLDefinition().empty());
    EXPECT_EQ(wkbUnknown, lyr.GetGeomType());
  }

  // 3. DETableInfo without ShapeType -> wkbNone
  {
    OGROpenFileGDBLayer lyr(&ds, "/vsimem/dummy.gdbtable", "lyr_table",
                            "<DETableInfo><Name>lyr_table</Name></DETableInfo>",
                            "", false);
    EXPECT_EQ(wkbNone, lyr.GetGeomType());
  }

  // 4. DEFeatureClassInfo with AliasName, UTC, Z, M, Nullable, GridSettings,
  // Parent SRS
  {
    const char* pszParentDef =
        "<DEFeatureDataset><Name>fds</Name>"
        "<SpatialReference><WKID>4326</WKID></SpatialReference></"
        "DEFeatureDataset>";

    const char* pszDef =
        "<DEFeatureClassInfo>"
        "<AliasName>LyrAlias</AliasName>"
        "<IsTimeInUTC>true</IsTimeInUTC>"
        "<HasZ>true</HasZ>"
        "<HasM>true</HasM>"
        "<ShapeType>esriGeometryPoint</ShapeType>"
        "<ShapeFieldName>SHAPE</ShapeFieldName>"
        "<GPFieldInfoExs>"
        "  "
        "<GPFieldInfoEx><Name>SHAPE</Name><IsNullable>false</IsNullable></"
        "GPFieldInfoEx>"
        "</GPFieldInfoExs>"
        "<SpatialReference>"
        "  <WKID>3857</WKID>"
        "  "
        "<XOrigin>-20000000</XOrigin><YOrigin>-20000000</"
        "YOrigin><XYScale>10000</XYScale>"
        "  <ZOrigin>-100000</ZOrigin><ZScale>1000</ZScale>"
        "  <MOrigin>-100000</MOrigin><MScale>1000</MScale>"
        "  "
        "<XYTolerance>0.001</XYTolerance><ZTolerance>0.001</"
        "ZTolerance><MTolerance>0.001</MTolerance>"
        "</SpatialReference>"
        "</DEFeatureClassInfo>";

    OGROpenFileGDBLayer lyr(&ds, "/vsimem/dummy.gdbtable", "lyr_pt", pszDef,
                            "doc", false, wkbUnknown, pszParentDef);

    EXPECT_STREQ("LyrAlias", lyr.GetMetadataItem("ALIAS_NAME"));
    EXPECT_EQ(wkbPointZM, lyr.GetGeomType());
    EXPECT_STREQ("doc", lyr.GetXMLDocumentation().c_str());

    // Coordinate precision and parent SRS (EPSG 4326 overridden)
    auto* poGeomField = lyr.GetLayerDefn()->GetGeomFieldDefn(0);
    ASSERT_NE(nullptr, poGeomField);
    EXPECT_STREQ("SHAPE", poGeomField->GetNameRef());
    EXPECT_FALSE(poGeomField->IsNullable());
    auto* srs = poGeomField->GetSpatialRef();
    ASSERT_NE(nullptr, srs);
    EXPECT_THAT(srs->GetAuthorityCode(nullptr), ::testing::StrEq("4326"));
  }

  // 5. Parent definition with invalid XML / no DEFeatureDataset
  {
    WithQuietHandler quiet;
    const char* pszDef =
        "<DEFeatureClassInfo>"
        "<ShapeType>esriGeometryPoint</ShapeType>"
        "<ShapeFieldName>SHAPE</ShapeFieldName>"
        "<SpatialReference><WKID>4326</WKID></SpatialReference>"
        "</DEFeatureClassInfo>";

    OGROpenFileGDBLayer lyr(&ds, "/vsimem/dummy.gdbtable", "lyr_pt2", pszDef,
                            "", false, wkbUnknown, "<invalid_parent");
    EXPECT_EQ(wkbPoint, lyr.GetGeomType());
  }

  // 6. MultiPatch detection in BuildGeometryColumnGDBv10
  {
    // A: MultiPatch on non-existent table
    const char* pszMPDef =
        "<DEFeatureClassInfo>"
        "<ShapeType>esriGeometryMultiPatch</ShapeType>"
        "<ShapeFieldName>SHAPE</ShapeFieldName>"
        "</DEFeatureClassInfo>";

    {
      WithQuietHandler quiet;
      OGROpenFileGDBLayer lyr_nonexist(&ds, "/vsimem/nonexistent.gdbtable",
                                       "mp_nonexist", pszMPDef, "", false);
      EXPECT_EQ(wkbUnknown, lyr_nonexist.GetGeomType());
    }

    // B: MultiPatch on valid empty table
    const std::string mp_gdb = std::string(kTestDir) + "/mp_test.gdb";
    {
      OGROpenFileGDBDataSource mp_ds;
      ASSERT_TRUE(mp_ds.Create(mp_gdb.c_str()));
      OGRGeomFieldDefn geom_field("SHAPE", wkbMultiPolygon25D);
      const char* const mp_opts[] = {"CREATE_MULTIPATCH=YES", nullptr};
      auto* mp_lyr = mp_ds.ICreateLayer("mp", &geom_field, mp_opts);
      ASSERT_NE(nullptr, mp_lyr);
      CloseDS(mp_ds);
    }
    const std::string mp_tbl_path = mp_gdb + "/a00000009.gdbtable";

    // Empty MultiPatch table -> remains wkbUnknown
    {
      OGROpenFileGDBLayer lyr_mp(&ds, mp_tbl_path.c_str(), "mp", pszMPDef, "",
                                 false);
      EXPECT_EQ(wkbUnknown, lyr_mp.GetGeomType());
    }

    // C: MultiPatch table with 2 features having matching geometry types + 1
    // deleted
    {
      auto mp_ds = OpenGDB(mp_gdb, GDAL_OF_VECTOR | GDAL_OF_UPDATE);
      ASSERT_NE(nullptr, mp_ds);
      auto* mp_lyr = mp_ds->GetLayerByName("mp");
      ASSERT_NE(nullptr, mp_lyr);

      for (int i = 0; i < 3; i++) {
        auto f = std::make_unique<OGRFeature>(mp_lyr->GetLayerDefn());
        OGRGeometry* geom = nullptr;
        OGRGeometryFactory::createFromWkt(
            "TIN Z (((0 0 0, 0 1 0, 1 1 0, 0 0 0)))", nullptr, &geom);
        ASSERT_NE(nullptr, geom);
        f->SetGeometryDirectly(geom);
        EXPECT_EQ(OGRERR_NONE, mp_lyr->CreateFeature(f.get()));
      }
      // Delete the 3rd feature so TryToDetectMultiPatchKind hits
      // SeekAndReadValidityBit == 0
      EXPECT_EQ(OGRERR_NONE, mp_lyr->DeleteFeature(3));
      CloseDS(*mp_ds);
    }

    {
      OGROpenFileGDBLayer lyr_mp_poly(&ds, mp_tbl_path.c_str(), "mp", pszMPDef,
                                      "", false);
      EXPECT_NE(wkbUnknown, lyr_mp_poly.GetGeomType());
    }
  }
}

// -----------------------------------------------------------------------------
// Test 2: BuildLayerDefinitionV9AndFieldTypesAndDefaults
// -----------------------------------------------------------------------------
TEST_F(OgrOpenFileGdbLayerTest,
       BuildLayerDefinitionV9AndFieldTypesAndDefaults) {
  OGROpenFileGDBDataSource ds;

  // 1. BuildLayerDefinition failure checks
  {
    WithQuietHandler quiet;
    OGROpenFileGDBLayer lyr(&ds, "/vsimem/nonexistent.gdbtable", "bad", "", "",
                            false);
    EXPECT_STREQ("", lyr.GetFIDColumn());
    EXPECT_EQ(OGRERR_FAILURE, lyr.ISetSpatialFilter(0, nullptr));
    EXPECT_EQ(OGRERR_FAILURE, lyr.SetAttributeFilter("1=1"));
    EXPECT_EQ(nullptr, lyr.GetNextFeature());
    EXPECT_EQ(nullptr, lyr.GetFeature(1));
    EXPECT_EQ(OGRERR_FAILURE, lyr.SetNextByIndex(0));
    OGREnvelope env;
    EXPECT_EQ(OGRERR_FAILURE, lyr.IGetExtent(0, &env, false));
    OGREnvelope3D env3d;
    EXPECT_EQ(OGRERR_FAILURE, lyr.IGetExtent3D(0, &env3d, false));
    EXPECT_EQ(0, lyr.GetFeatureCount(false));
    EXPECT_FALSE(lyr.TestCapability(OLCRandomRead));
    EXPECT_FALSE(lyr.HasIndexForField("foo"));
    EXPECT_EQ(nullptr, lyr.BuildIndex("foo", 1, -1, nullptr));
    int out_type = 0;
    OGRFieldDefn dummy_fld("dummy", OFTInteger);
    EXPECT_EQ(nullptr, lyr.GetMinMaxValue(&dummy_fld, 1, out_type));
    double dmin, dmax, dsum;
    int ncnt;
    EXPECT_FALSE(lyr.GetMinMaxSumCount(&dummy_fld, dmin, dmax, dsum, ncnt));
    EXPECT_EQ(&ds, lyr.GetDataset());
    EXPECT_STREQ("bad", lyr.GetName());
    EXPECT_STREQ("/vsimem/nonexistent.gdbtable", lyr.GetFilename().c_str());
  }

  // 2. bEditable = true retries in read-only mode when write fails
  {
    const std::string zip_file = std::string(kTestDir) + "/ro_table.zip";
    void* h_zip = CPLCreateZip(zip_file.c_str(), nullptr);
    ASSERT_NE(nullptr, h_zip);

    // Create a dummy valid gdbtable in memory and write into zip
    std::string tbl_tmp = std::string(kTestDir) + "/t.gdbtable";
    {
      OpenFileGDB::FileGDBTable t;
      ASSERT_TRUE(
          t.Create(tbl_tmp.c_str(), 4, OpenFileGDB::FGTGT_NONE, false, false));
      ASSERT_TRUE(t.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "OBJECTID", "", OpenFileGDB::FGFT_OBJECTID, false, false, true, 0,
          OpenFileGDB::FileGDBField::UNSET_FIELD)));
      t.Sync();
      t.Close();
    }
    CPLAddFileInZip(h_zip, "t.gdbtable", tbl_tmp.c_str(), nullptr, nullptr,
                    nullptr, nullptr);
    CPLCloseZip(h_zip);

    std::string ro_path = "/vsizip/" + zip_file + "/t.gdbtable";
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      // Open in editable mode inside zip -> write open fails, falls back to
      // read-only
      OGROpenFileGDBLayer lyr(&ds, ro_path.c_str(), "t", "", "", true);
      EXPECT_TRUE(lyr.IsValidLayerDefn());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot open"));
    }
  }

  // 3. FileGDB v9 mode (osDefinition is empty) geometry types
  {
    struct GeomTestCase {
      OpenFileGDB::FileGDBTableGeometryType gdb_geom_type;
      OGRwkbGeometryType expected_ogr_type;
    };
    const GeomTestCase cases[] = {
        {OpenFileGDB::FGTGT_POINT, wkbPoint},
        {OpenFileGDB::FGTGT_MULTIPOINT, wkbMultiPoint},
        {OpenFileGDB::FGTGT_LINE, wkbMultiLineString},
        {OpenFileGDB::FGTGT_POLYGON, wkbMultiPolygon},
        {OpenFileGDB::FGTGT_MULTIPATCH, wkbUnknown},
        {OpenFileGDB::FGTGT_NONE, wkbNone},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      std::string path =
          std::string(kTestDir) + "/v9_geom_" + std::to_string(i) + ".gdbtable";
      {
        OpenFileGDB::FileGDBTable t;
        ASSERT_TRUE(
            t.Create(path.c_str(), 4, cases[i].gdb_geom_type, true, true));
        ASSERT_TRUE(t.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
            "OBJECTID", "", OpenFileGDB::FGFT_OBJECTID, false, false, true, 0,
            OpenFileGDB::FileGDBField::UNSET_FIELD)));
        if (cases[i].gdb_geom_type != OpenFileGDB::FGTGT_NONE) {
          auto geom_fld = std::make_unique<OpenFileGDB::FileGDBGeomField>(
              "SHAPE", "", true,
              "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS "
              "84\",6378137,298.257223563]],"
              "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]",
              -400.0, -400.0, 1000000000.0, 0.000000008983153,
              std::vector<double>{1000.0});
          geom_fld->SetZOriginScaleTolerance(-100000.0, 10000.0, 0.001);
          geom_fld->SetMOriginScaleTolerance(-100000.0, 10000.0, 0.001);
          ASSERT_TRUE(t.CreateField(std::move(geom_fld)));
        }
        t.Sync();
        t.Close();
      }

      OGROpenFileGDBLayer lyr(&ds, path.c_str(), "v9_lyr", "", "", false);
      if (cases[i].gdb_geom_type != OpenFileGDB::FGTGT_NONE) {
        EXPECT_EQ(wkbSetM(wkbSetZ(cases[i].expected_ogr_type)),
                  lyr.GetGeomType());
        auto* defn = lyr.GetLayerDefn();
        ASSERT_NE(nullptr, defn);
        EXPECT_EQ(1, defn->GetGeomFieldCount());
        EXPECT_NE(nullptr, defn->GetGeomFieldDefn(0));
      } else {
        EXPECT_EQ(wkbNone, lyr.GetGeomType());
        auto* defn = lyr.GetLayerDefn();
        ASSERT_NE(nullptr, defn);
        EXPECT_EQ(0, defn->GetGeomFieldCount());
      }
    }
  }

  // 4. Inconsistent geometry type warning in v9
  {
    std::string path = std::string(kTestDir) + "/v9_inconsistent.gdbtable";
    {
      OpenFileGDB::FileGDBTable t;
      ASSERT_TRUE(
          t.Create(path.c_str(), 4, OpenFileGDB::FGTGT_POINT, false, false));
      ASSERT_TRUE(t.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "OBJECTID", "", OpenFileGDB::FGFT_OBJECTID, false, false, true, 0,
          OpenFileGDB::FileGDBField::UNSET_FIELD)));
      auto geom_fld = std::make_unique<OpenFileGDB::FileGDBGeomField>(
          "SHAPE", "", true, "", -400.0, -400.0, 1000000000.0,
          0.000000008983153, std::vector<double>{1000.0});
      ASSERT_TRUE(t.CreateField(std::move(geom_fld)));
      t.Sync();
      t.Close();
    }
    WithQuietHandler quiet;
    CPLErrorReset();
    OGROpenFileGDBLayer lyr(&ds, path.c_str(), "v9_incon", "", "", false,
                            wkbPolygon);
    EXPECT_EQ(wkbPoint, lyr.GetGeomType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Inconsistency for layer geometry type"));
  }

  // 5. All Field Types and Defaults in BuildLayerDefinition
  {
    const std::string gdb = std::string(kTestDir) + "/fields_test.gdb";
    OGROpenFileGDBDataSource create_ds;
    ASSERT_TRUE(create_ds.Create(gdb.c_str()));

    OGRGeomFieldDefn geom_field("SHAPE", wkbPolygon);
    const char* const create_opts[] = {
        "TARGET_ARCGIS_VERSION=ARCGIS_PRO_3_2_OR_LATER",
        "CREATE_SHAPE_AREA_AND_LENGTH_FIELDS=YES", nullptr};
    auto* lyr = create_ds.ICreateLayer("all_fields", &geom_field, create_opts);
    ASSERT_NE(nullptr, lyr);

    // Create fields of each type
    OGRFieldDefn f_i16("f_i16", OFTInteger);
    f_i16.SetSubType(OFSTInt16);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_i16));

    OGRFieldDefn f_i32("f_i32", OFTInteger);
    f_i32.SetDefault("42");
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_i32));

    OGRFieldDefn f_f32("f_f32", OFTReal);
    f_f32.SetSubType(OFSTFloat32);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_f32));

    OGRFieldDefn f_f64("f_f64", OFTReal);
    f_f64.SetDefault("3.1415");
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_f64));

    OGRFieldDefn f_str("f_str", OFTString);
    f_str.SetWidth(50);
    f_str.SetDefault("'hello'");
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_str));

    OGRFieldDefn f_i64("f_i64", OFTInteger64);
    f_i64.SetDefault("1234567890123");
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_i64));

    OGRFieldDefn f_date("f_date", OFTDate);
    f_date.SetDefault("'2023/05/10'");
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_date));

    OGRFieldDefn f_time("f_time", OFTTime);
    f_time.SetDefault("'14:30:00'");
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_time));

    OGRFieldDefn f_dt("f_dt", OFTDateTime);
    f_dt.SetDefault("'2023/05/10 14:30:00'");
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_dt));

    OGRFieldDefn f_bin("f_bin", OFTBinary);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_bin));

    // Legacy datetime formatting in v10 without ArcGIS Pro 3.2+
    {
      OGRGeomFieldDefn g_fld("SHAPE", wkbPoint);
      auto* lyr_legacy = create_ds.ICreateLayer("legacy_dt", &g_fld, nullptr);
      ASSERT_NE(nullptr, lyr_legacy);
      OGRFieldDefn f_dt_legacy("f_dt_leg", OFTDateTime);
      f_dt_legacy.SetDefault("'2023/05/10 14:30:00'");
      EXPECT_EQ(OGRERR_NONE, lyr_legacy->CreateField(&f_dt_legacy));
    }

    CloseDS(create_ds);

    // Open read-only and verify fields and defaults
    auto ds_ro = OpenGDB(gdb);
    ASSERT_NE(nullptr, ds_ro);
    auto* ro_lyr = ds_ro->GetLayerByName("all_fields");
    ASSERT_NE(nullptr, ro_lyr);

    auto* defn = ro_lyr->GetLayerDefn();
    ASSERT_NE(nullptr, defn);

    int idx_i32 = defn->GetFieldIndex("f_i32");
    EXPECT_GE(idx_i32, 0);
    EXPECT_STREQ("42", defn->GetFieldDefn(idx_i32)->GetDefault());

    int idx_str = defn->GetFieldIndex("f_str");
    EXPECT_GE(idx_str, 0);
    EXPECT_EQ(50, defn->GetFieldDefn(idx_str)->GetWidth());
    EXPECT_STREQ("'hello'", defn->GetFieldDefn(idx_str)->GetDefault());

    int idx_area = defn->GetFieldIndex("Shape_Area");
    EXPECT_GE(idx_area, 0);
    EXPECT_STREQ("FILEGEODATABASE_SHAPE_AREA",
                 defn->GetFieldDefn(idx_area)->GetDefault());

    int idx_len = defn->GetFieldIndex("Shape_Length");
    EXPECT_GE(idx_len, 0);
    EXPECT_STREQ("FILEGEODATABASE_SHAPE_LENGTH",
                 defn->GetFieldDefn(idx_len)->GetDefault());

    auto* leg_lyr = ds_ro->GetLayerByName("legacy_dt");
    ASSERT_NE(nullptr, leg_lyr);
    auto* leg_defn = leg_lyr->GetLayerDefn();
    ASSERT_NE(nullptr, leg_defn);
    int idx_leg = leg_defn->GetFieldIndex("f_dt_leg");
    EXPECT_GE(idx_leg, 0);
    EXPECT_STREQ("'2023/05/10 14:30:00'",
                 leg_defn->GetFieldDefn(idx_leg)->GetDefault());
  }

  // 6. Special field types: XML, GUID, GLOBALID, GDB_UserMetadata Xml
  // binary-as-string, high-precision datetime
  {
    std::string raw_path = std::string(kTestDir) + "/raw_fields.gdbtable";
    {
      OpenFileGDB::FileGDBTable t;
      ASSERT_TRUE(
          t.Create(raw_path.c_str(), 4, OpenFileGDB::FGTGT_NONE, false, false));
      ASSERT_TRUE(t.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "OBJECTID", "", OpenFileGDB::FGFT_OBJECTID, false, false, true, 0,
          OpenFileGDB::FileGDBField::UNSET_FIELD)));
      ASSERT_TRUE(t.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "guid_f", "", OpenFileGDB::FGFT_GUID, true, false, true, 0,
          OpenFileGDB::FileGDBField::UNSET_FIELD)));
      ASSERT_TRUE(t.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "gid_f", "", OpenFileGDB::FGFT_GLOBALID, true, false, true, 0,
          OpenFileGDB::FileGDBField::UNSET_FIELD)));
      ASSERT_TRUE(t.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "xml_f", "", OpenFileGDB::FGFT_XML, true, false, true, 0,
          OpenFileGDB::FileGDBField::UNSET_FIELD)));
      ASSERT_TRUE(t.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "Xml", "", OpenFileGDB::FGFT_BINARY, true, false, true, 0,
          OpenFileGDB::FileGDBField::UNSET_FIELD)));
      ASSERT_TRUE(t.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "dt_hp", "", OpenFileGDB::FGFT_DATETIME, true, false, true, 0,
          OpenFileGDB::FileGDBField::UNSET_FIELD)));

      OGRField def_int;
      def_int.Integer = 10;
      ASSERT_TRUE(t.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "int_def", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
          def_int)));

      OGRField def_real;
      def_real.Real = 1.25;
      ASSERT_TRUE(t.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "real_def", "", OpenFileGDB::FGFT_FLOAT64, true, false, true, 0,
          def_real)));

      OGRField def_i64;
      def_i64.Integer64 = 100;
      ASSERT_TRUE(t.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "i64_def", "", OpenFileGDB::FGFT_INT64, true, false, true, 0,
          def_i64)));

      // Write 1 feature
      std::vector<OGRField> f(t.GetFieldCount(),
                              OpenFileGDB::FileGDBField::UNSET_FIELD);
      const char szXmlData[] = "<metadata/>";
      f[4].Binary.nCount = static_cast<int>(sizeof(szXmlData));
      f[4].Binary.paData =
          reinterpret_cast<GByte*>(const_cast<char*>(szXmlData));
      ASSERT_TRUE(t.CreateFeature(f, nullptr));
      t.Sync();
      t.Close();
    }

    CPLSetThreadLocalConfigOption("OPENFILEGDB_LIST_DELETED_FEATURES", "YES");
    absl::Cleanup reset_del = [] {
      CPLSetThreadLocalConfigOption("OPENFILEGDB_LIST_DELETED_FEATURES",
                                    nullptr);
    };

    const char* pszRawDef =
        "<DETableInfo>"
        "<Name>raw_fields</Name>"
        "<GPFieldInfoExs>"
        "  "
        "<GPFieldInfoEx><Name>dt_hp</Name><HighPrecision>true</"
        "HighPrecision><DomainName>my_dom</DomainName></GPFieldInfoEx>"
        "  "
        "<GPFieldInfoEx><Name>int_def</Name><DefaultValueNumeric>99</"
        "DefaultValueNumeric></GPFieldInfoEx>"
        "  "
        "<GPFieldInfoEx><Name>real_def</Name><DefaultValueNumeric>9.75</"
        "DefaultValueNumeric></GPFieldInfoEx>"
        "  "
        "<GPFieldInfoEx><Name>i64_def</Name><DefaultValueInteger>999</"
        "DefaultValueInteger></GPFieldInfoEx>"
        "</GPFieldInfoExs>"
        "</DETableInfo>";

    OGROpenFileGDBLayer raw_lyr(&ds, raw_path.c_str(), "raw_fields", pszRawDef,
                                "", false);
    auto* raw_defn = raw_lyr.GetLayerDefn();
    ASSERT_NE(nullptr, raw_defn);
    EXPECT_EQ(
        OFTString,
        raw_defn->GetFieldDefn(raw_defn->GetFieldIndex("guid_f"))->GetType());
    EXPECT_EQ(
        OFTString,
        raw_defn->GetFieldDefn(raw_defn->GetFieldIndex("gid_f"))->GetType());
    EXPECT_EQ(
        OFTString,
        raw_defn->GetFieldDefn(raw_defn->GetFieldIndex("xml_f"))->GetType());
    EXPECT_EQ(
        OFTString,
        raw_defn->GetFieldDefn(raw_defn->GetFieldIndex("Xml"))->GetType());
    EXPECT_EQ("my_dom", raw_defn->GetFieldDefn(raw_defn->GetFieldIndex("dt_hp"))
                            ->GetDomainName());

    auto* f_read = raw_lyr.GetNextFeature();
    ASSERT_NE(nullptr, f_read);
    EXPECT_STREQ("<metadata/>", f_read->GetFieldAsString("Xml"));
    delete f_read;
  }
}

// -----------------------------------------------------------------------------
// Test 3: AttributeFiltersAndIndexIterators
// -----------------------------------------------------------------------------
TEST_F(OgrOpenFileGdbLayerTest, AttributeFiltersAndIndexIterators) {
  const std::string gdb = std::string(kTestDir) + "/idx_filter.gdb";
  {
    OGROpenFileGDBDataSource ds;
    ASSERT_TRUE(ds.Create(gdb.c_str()));
    OGRGeomFieldDefn geom_field("SHAPE", wkbPoint);
    const char* const idx_opts[] = {
        "TARGET_ARCGIS_VERSION=ARCGIS_PRO_3_2_OR_LATER", nullptr};
    auto* lyr = ds.ICreateLayer("idx_lyr", &geom_field, idx_opts);
    ASSERT_NE(nullptr, lyr);

    OGRFieldDefn f_int("int_col", OFTInteger);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_int));
    OGRFieldDefn f_i64("i64_col", OFTInteger64);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_i64));
    OGRFieldDefn f_real("real_col", OFTReal);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_real));
    OGRFieldDefn f_str("str_col", OFTString);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_str));
    OGRFieldDefn f_str_ci("str_ci_col", OFTString);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_str_ci));
    OGRFieldDefn f_dt("dt_col", OFTDateTime);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_dt));
    OGRFieldDefn f_date("date_col", OFTDate);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_date));
    OGRFieldDefn f_time("time_col", OFTTime);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_time));
    OGRFieldDefn f_unidx("unidx_col", OFTInteger);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_unidx));

    // Populate features:
    // Feat 1: 10, 100, 1.5, "alpha", "ALPHA", 2023/01/01 10:00:00, 1
    // Feat 2: 20, 200, 2.5, "beta", "BETA", 2023/01/02 10:00:00, 2
    // Feat 3: 30, 300, 3.5, "gamma", "GAMMA", 2023/01/03 10:00:00, 3
    // Feat 4: 40, 400, 4.5, "delta", "DELTA", 2023/01/04 10:00:00, 4
    // Feat 5: NULLs
    auto add_feat = [&](int i, GIntBig i64, double r, const char* s,
                        const char* sci, const char* dt, int u,
                        bool is_null = false) {
      OGRFeature feat(lyr->GetLayerDefn());
      if (!is_null) {
        feat.SetField("int_col", i);
        feat.SetField("i64_col", i64);
        feat.SetField("real_col", r);
        feat.SetField("str_col", s);
        feat.SetField("str_ci_col", sci);
        feat.SetField("dt_col", dt);
        feat.SetField("date_col", "2023/01/01");
        feat.SetField("time_col", "10:00:00");
        feat.SetField("unidx_col", u);
      }
      OGRPoint pt(static_cast<double>(i), static_cast<double>(i));
      feat.SetGeometry(&pt);
      EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(&feat));
    };

    add_feat(10, 100, 1.5, "alpha", "ALPHA", "2023/01/01 10:00:00", 1);
    add_feat(20, 200, 2.5, "beta", "BETA", "2023/01/02 10:00:00", 2);
    add_feat(30, 300, 3.5, "gamma", "GAMMA", "2023/01/03 10:00:00", 3);
    add_feat(40, 400, 4.5, "delta", "DELTA", "2023/01/04 10:00:00", 4);
    add_feat(0, 0, 0, "", "", "", 0, /*is_null=*/true);

    // Create indices
    auto* ofgdb_lyr = cpl::down_cast<OGROpenFileGDBLayer*>(lyr);
    ofgdb_lyr->CreateIndex("idx_int", "int_col");
    ofgdb_lyr->CreateIndex("idx_i64", "i64_col");
    ofgdb_lyr->CreateIndex("idx_real", "real_col");
    ofgdb_lyr->CreateIndex("idx_str", "str_col");
    ofgdb_lyr->CreateIndex("idx_str_ci", "LOWER(str_ci_col)");
    ofgdb_lyr->CreateIndex("idx_dt", "dt_col");
    ofgdb_lyr->CreateIndex("idx_date", "date_col");
    ofgdb_lyr->CreateIndex("idx_time", "time_col");

    CloseDS(ds);
  }

  // Open read-only and test all attribute filter branches
  auto ds = OpenGDB(gdb);
  ASSERT_NE(nullptr, ds);
  auto* lyr =
      cpl::down_cast<OGROpenFileGDBLayer*>(ds->GetLayerByName("idx_lyr"));
  ASSERT_NE(nullptr, lyr);

  // 1. Basic comparisons: =, !=, <, <=, >, >=
  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("int_col = 20"));
  EXPECT_EQ(1, lyr->GetFeatureCount(true));
  EXPECT_EQ(2, lyr->GetAttrIndexUse());  // index sufficient

  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("int_col != 20"));
  EXPECT_EQ(4, lyr->GetFeatureCount(true));

  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("int_col < 30"));
  EXPECT_EQ(2, lyr->GetFeatureCount(true));

  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("int_col <= 30"));
  EXPECT_EQ(3, lyr->GetFeatureCount(true));

  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("int_col > 20"));
  EXPECT_EQ(2, lyr->GetFeatureCount(true));

  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("int_col >= 20"));
  EXPECT_EQ(3, lyr->GetFeatureCount(true));

  // Reverse constant op column
  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("20 = int_col"));
  EXPECT_EQ(1, lyr->GetFeatureCount(true));
  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("20 != int_col"));
  EXPECT_EQ(4, lyr->GetFeatureCount(true));
  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("30 > int_col"));
  EXPECT_EQ(2, lyr->GetFeatureCount(true));
  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("30 >= int_col"));
  EXPECT_EQ(3, lyr->GetFeatureCount(true));
  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("20 < int_col"));
  EXPECT_EQ(2, lyr->GetFeatureCount(true));
  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("20 <= int_col"));
  EXPECT_EQ(3, lyr->GetFeatureCount(true));

  // Float constant on integer / int64
  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("int_col = 20.0"));
  EXPECT_EQ(1, lyr->GetFeatureCount(true));
  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("i64_col = 200.0"));
  EXPECT_EQ(1, lyr->GetFeatureCount(true));

  // String equality & LOWER index ILIKE
  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("str_ci_col = 'beta'"));
  EXPECT_EQ(1, lyr->GetFeatureCount(true));

  // String >= and <
  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("str_col >= 'gamma'"));
  EXPECT_EQ(1, lyr->GetFeatureCount(true));  // "gamma"

  // Date and Time constants
  EXPECT_EQ(OGRERR_NONE,
            lyr->SetAttributeFilter("dt_col = '2023/01/02 10:00:00'"));
  EXPECT_EQ(1, lyr->GetFeatureCount(true));

  // 2. AND combinations & BETWEEN
  EXPECT_EQ(OGRERR_NONE,
            lyr->SetAttributeFilter("int_col >= 20 AND int_col <= 30"));
  EXPECT_EQ(2, lyr->GetFeatureCount(true));

  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("int_col BETWEEN 20 AND 30"));
  EXPECT_EQ(2, lyr->GetFeatureCount(true));

  // Left indexed, right unindexed
  EXPECT_EQ(OGRERR_NONE,
            lyr->SetAttributeFilter("int_col >= 20 AND unidx_col = 2"));
  EXPECT_EQ(1, lyr->GetFeatureCount(true));
  EXPECT_EQ(1, lyr->GetAttrIndexUse());  // index not sufficient, partial filter

  // Right indexed, left unindexed
  EXPECT_EQ(OGRERR_NONE,
            lyr->SetAttributeFilter("unidx_col = 2 AND int_col >= 20"));
  EXPECT_EQ(1, lyr->GetFeatureCount(true));

  // 3. OR combinations (AreExprExclusive pairs)
  // SWQ_EQ, SWQ_EQ
  EXPECT_EQ(OGRERR_NONE,
            lyr->SetAttributeFilter("int_col = 10 OR int_col = 30"));
  EXPECT_EQ(2, lyr->GetFeatureCount(true));

  // SWQ_LT, SWQ_GT
  EXPECT_EQ(OGRERR_NONE,
            lyr->SetAttributeFilter("int_col < 20 OR int_col > 30"));
  EXPECT_EQ(2, lyr->GetFeatureCount(true));

  // SWQ_GT, SWQ_LT
  EXPECT_EQ(OGRERR_NONE,
            lyr->SetAttributeFilter("int_col > 30 OR int_col < 20"));
  EXPECT_EQ(2, lyr->GetFeatureCount(true));

  // SWQ_LT, SWQ_GE
  EXPECT_EQ(OGRERR_NONE,
            lyr->SetAttributeFilter("int_col < 20 OR int_col >= 30"));
  EXPECT_EQ(3, lyr->GetFeatureCount(true));

  // SWQ_LE, SWQ_GE
  EXPECT_EQ(OGRERR_NONE,
            lyr->SetAttributeFilter("int_col <= 20 OR int_col >= 40"));
  EXPECT_EQ(3, lyr->GetFeatureCount(true));

  // SWQ_LE, SWQ_GT
  EXPECT_EQ(OGRERR_NONE,
            lyr->SetAttributeFilter("int_col <= 20 OR int_col > 30"));
  EXPECT_EQ(3, lyr->GetFeatureCount(true));

  // SWQ_GE, SWQ_LE
  EXPECT_EQ(OGRERR_NONE,
            lyr->SetAttributeFilter("int_col >= 40 OR int_col <= 20"));
  EXPECT_EQ(3, lyr->GetFeatureCount(true));

  // SWQ_GE, SWQ_LT
  EXPECT_EQ(OGRERR_NONE,
            lyr->SetAttributeFilter("int_col >= 30 OR int_col < 20"));
  EXPECT_EQ(3, lyr->GetFeatureCount(true));

  // SWQ_GT, SWQ_LE
  EXPECT_EQ(OGRERR_NONE,
            lyr->SetAttributeFilter("int_col > 30 OR int_col <= 20"));
  EXPECT_EQ(3, lyr->GetFeatureCount(true));

  // IS NULL with comparison
  EXPECT_EQ(OGRERR_NONE,
            lyr->SetAttributeFilter("int_col IS NULL OR int_col = 20"));
  EXPECT_EQ(2, lyr->GetFeatureCount(true));
  EXPECT_EQ(OGRERR_NONE,
            lyr->SetAttributeFilter("int_col = 20 OR int_col IS NULL"));
  EXPECT_EQ(2, lyr->GetFeatureCount(true));

  // OR with one branch non-indexed -> index disabled
  EXPECT_EQ(OGRERR_NONE,
            lyr->SetAttributeFilter("int_col = 20 OR unidx_col = 1"));
  EXPECT_EQ(2, lyr->GetFeatureCount(true));
  EXPECT_EQ(0, lyr->GetAttrIndexUse());

  // 4. IS NULL and IS NOT NULL
  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("int_col IS NULL"));
  EXPECT_EQ(1, lyr->GetFeatureCount(true));

  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("int_col IS NOT NULL"));
  EXPECT_EQ(4, lyr->GetFeatureCount(true));

  // 5. IN (...)
  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("int_col IN (10, 30, 40)"));
  EXPECT_EQ(3, lyr->GetFeatureCount(true));

  EXPECT_EQ(OGRERR_NONE,
            lyr->SetAttributeFilter("str_col IN ('alpha', 'gamma')"));
  EXPECT_EQ(2, lyr->GetFeatureCount(true));

  // IN with non-constant
  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("int_col IN (10, unidx_col)"));
  EXPECT_EQ(1, lyr->GetFeatureCount(true));

  // 6. NOT (...)
  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("NOT (int_col = 20)"));
  EXPECT_EQ(4, lyr->GetFeatureCount(true));

  // NOT on partial expression disables index
  EXPECT_EQ(OGRERR_NONE,
            lyr->SetAttributeFilter("NOT (int_col = 20 AND unidx_col = 1)"));
  EXPECT_GE(lyr->GetFeatureCount(true), 0);

  // 7. Direct helper calls on OGROpenFileGDBLayer
  EXPECT_TRUE(lyr->HasIndexForField("int_col"));
  EXPECT_FALSE(lyr->HasIndexForField("unidx_col"));
  EXPECT_FALSE(lyr->HasIndexForField("nonexistent"));

  // BuildIndex
  auto* iter_not_null = lyr->BuildIndex("int_col", 1, -1, nullptr);
  ASSERT_NE(nullptr, iter_not_null);
  delete iter_not_null;

  EXPECT_EQ(nullptr, lyr->BuildIndex("nonexistent", 1, -1, nullptr));

  // GetMinMaxValue
  int out_type = 0;
  auto* fdefn_int = lyr->GetLayerDefn()->GetFieldDefn(
      lyr->GetLayerDefn()->GetFieldIndex("int_col"));
  const OGRField* min_val = lyr->GetMinMaxValue(fdefn_int, 1, out_type);
  ASSERT_NE(nullptr, min_val);
  EXPECT_EQ(10, min_val->Integer);

  const OGRField* max_val = lyr->GetMinMaxValue(fdefn_int, 0, out_type);
  ASSERT_NE(nullptr, max_val);
  EXPECT_EQ(40, max_val->Integer);

  // GetMinMaxSumCount
  double dmin = 0, dmax = 0, dsum = 0;
  int ncnt = 0;
  EXPECT_TRUE(lyr->GetMinMaxSumCount(fdefn_int, dmin, dmax, dsum, ncnt));
  EXPECT_DOUBLE_EQ(10.0, dmin);
  EXPECT_DOUBLE_EQ(40.0, dmax);
  EXPECT_DOUBLE_EQ(100.0, dsum);
  EXPECT_EQ(4, ncnt);

  // 8. Additional coverage for i64, date, time, long string, and SWQ ops
  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("i64_col = 200"));
  EXPECT_EQ(1, lyr->GetFeatureCount(true));
  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("i64_col = 200.0"));
  EXPECT_EQ(1, lyr->GetFeatureCount(true));
  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("date_col = '2023/01/01'"));
  EXPECT_EQ(4, lyr->GetFeatureCount(true));
  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("time_col = '10:00:00'"));
  EXPECT_EQ(4, lyr->GetFeatureCount(true));

  // CompValues via OR (AreExprExclusive)
  EXPECT_EQ(OGRERR_NONE,
            lyr->SetAttributeFilter("int_col = 10.0 OR int_col = 10.0"));
  EXPECT_EQ(1, lyr->GetFeatureCount(true));
  EXPECT_EQ(OGRERR_NONE,
            lyr->SetAttributeFilter("real_col = 1.5 OR real_col = 2.5"));
  EXPECT_EQ(2, lyr->GetFeatureCount(true));
  EXPECT_EQ(OGRERR_NONE,
            lyr->SetAttributeFilter("real_col = 1.5 OR real_col = 1.5"));
  EXPECT_EQ(1, lyr->GetFeatureCount(true));
  EXPECT_EQ(OGRERR_NONE,
            lyr->SetAttributeFilter("real_col = 2.5 OR real_col = 1.5"));
  EXPECT_EQ(2, lyr->GetFeatureCount(true));
  EXPECT_EQ(OGRERR_NONE,
            lyr->SetAttributeFilter("str_col = 'alpha' OR str_col = 'beta'"));
  EXPECT_EQ(2, lyr->GetFeatureCount(true));
  EXPECT_EQ(
      OGRERR_NONE,
      lyr->SetAttributeFilter(
          "dt_col = '2023/01/01 10:00:00' OR dt_col = '2023/01/02 10:00:00'"));
  EXPECT_EQ(2, lyr->GetFeatureCount(true));
  EXPECT_EQ(OGRERR_NONE,
            lyr->SetAttributeFilter("i64_col = 100 OR i64_col = 200"));
  EXPECT_EQ(2, lyr->GetFeatureCount(true));

  // Long string (> 80 chars) truncation in index lookup and IN (...)
  std::string long_s(100, 'a');
  EXPECT_EQ(OGRERR_NONE,
            lyr->SetAttributeFilter(("str_col = '" + long_s + "'").c_str()));
  EXPECT_EQ(0, lyr->GetFeatureCount(true));
  EXPECT_EQ(OGRERR_NONE,
            lyr->SetAttributeFilter(
                ("str_col IN ('" + long_s + "', 'alpha')").c_str()));
  EXPECT_EQ(1, lyr->GetFeatureCount(true));

  // SetNextByIndex when attribute iterator is active (line 2021)
  EXPECT_EQ(OGRERR_NONE, lyr->SetNextByIndex(0));

  // BuildIndex with all SWQ ops (lines 2410-2440)
  swq_expr_node val_node(20);
  for (swq_op op : {SWQ_LE, SWQ_LT, SWQ_EQ, SWQ_GE, SWQ_GT}) {
    auto* it = lyr->BuildIndex("int_col", 1, op, &val_node);
    ASSERT_NE(nullptr, it);
    delete it;
  }
  EXPECT_EQ(nullptr, lyr->BuildIndex("int_col", 1, SWQ_NE, &val_node));

  // Clear attribute filter
  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter(nullptr));
  EXPECT_EQ(0, lyr->GetAttrIndexUse());
}

// -----------------------------------------------------------------------------
// Test 4: SpatialFilterQuadTreeAndSpatialIndex
// -----------------------------------------------------------------------------
TEST_F(OgrOpenFileGdbLayerTest, SpatialFilterQuadTreeAndSpatialIndex) {
  const std::string gdb = std::string(kTestDir) + "/spatial_test.gdb";
  {
    OGROpenFileGDBDataSource ds;
    ASSERT_TRUE(ds.Create(gdb.c_str()));
    OGRGeomFieldDefn geom_field("SHAPE", wkbPolygon);
    auto* lyr = ds.ICreateLayer("poly_lyr", &geom_field, nullptr);
    ASSERT_NE(nullptr, lyr);

    OGRFieldDefn fld("id", OFTInteger);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&fld));

    // Create 4 polygon features in a grid:
    // Box 1: (0,0) - (2,2)
    // Box 2: (5,0) - (7,2)
    // Box 3: (0,5) - (2,7)
    // Box 4: (5,5) - (7,7)
    auto add_box = [&](int id, double minx, double miny, double maxx,
                       double maxy) {
      OGRFeature feat(lyr->GetLayerDefn());
      feat.SetField("id", id);
      OGRLinearRing ring;
      ring.addPoint(minx, miny);
      ring.addPoint(maxx, miny);
      ring.addPoint(maxx, maxy);
      ring.addPoint(minx, maxy);
      ring.addPoint(minx, miny);
      OGRPolygon poly;
      poly.addRing(&ring);
      feat.SetGeometry(&poly);
      EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(&feat));
    };

    add_box(1, 0, 0, 2, 2);
    add_box(2, 5, 0, 7, 2);
    add_box(3, 0, 5, 2, 7);
    add_box(4, 5, 5, 7, 7);

    // Create spatial index
    cpl::down_cast<OGROpenFileGDBLayer*>(lyr)->CreateSpatialIndex();
    CloseDS(ds);
  }

  // 1. OPENFILEGDB_USE_SPATIAL_INDEX = YES (native FileGDBSpatialIndexIterator)
  {
    CPLSetThreadLocalConfigOption("OPENFILEGDB_USE_SPATIAL_INDEX", "YES");
    absl::Cleanup reset_opt = [] {
      CPLSetThreadLocalConfigOption("OPENFILEGDB_USE_SPATIAL_INDEX", nullptr);
    };

    auto ds = OpenGDB(gdb);
    ASSERT_NE(nullptr, ds);
    auto* lyr =
        cpl::down_cast<OGROpenFileGDBLayer*>(ds->GetLayerByName("poly_lyr"));
    ASSERT_NE(nullptr, lyr);

    // Spatial filter covering entire layer extent -> disables filter
    lyr->SetSpatialFilterRect(-1, -1, 10, 10);
    EXPECT_EQ(4, lyr->GetFeatureCount(true));

    // Spatial filter covering only Box 1 (0,0)-(2,2)
    lyr->SetSpatialFilterRect(0, 0, 3, 3);
    EXPECT_EQ(1, lyr->GetFeatureCount(true));

    // GetNextFeature with spatial index iterator
    lyr->ResetReading();
    auto* feat = lyr->GetNextFeature();
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(1, feat->GetFieldAsInteger("id"));
    delete feat;
    EXPECT_EQ(nullptr, lyr->GetNextFeature());

    // Update filter envelope while iterator is active
    lyr->SetSpatialFilterRect(4, 0, 8, 3);
    feat = lyr->GetNextFeature();
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(2, feat->GetFieldAsInteger("id"));
    delete feat;

    // Combine with attribute filter
    EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("id = 2"));
    lyr->ResetReading();
    feat = lyr->GetNextFeature();
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(2, feat->GetFieldAsInteger("id"));
    delete feat;

    // Clear filters
    lyr->SetSpatialFilter(nullptr);
    EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter(nullptr));
    EXPECT_EQ(4, lyr->GetFeatureCount(true));
  }

  // 2. OPENFILEGDB_USE_SPATIAL_INDEX = NO and OPENFILEGDB_IN_MEMORY_SPI = YES
  // (QuadTree fallback)
  {
    CPLSetThreadLocalConfigOption("OPENFILEGDB_USE_SPATIAL_INDEX", "NO");
    CPLSetThreadLocalConfigOption("OPENFILEGDB_IN_MEMORY_SPI", "YES");
    absl::Cleanup reset_opt = [] {
      CPLSetThreadLocalConfigOption("OPENFILEGDB_USE_SPATIAL_INDEX", nullptr);
      CPLSetThreadLocalConfigOption("OPENFILEGDB_IN_MEMORY_SPI", nullptr);
    };

    auto ds = OpenGDB(gdb);
    ASSERT_NE(nullptr, ds);
    auto* lyr =
        cpl::down_cast<OGROpenFileGDBLayer*>(ds->GetLayerByName("poly_lyr"));
    ASSERT_NE(nullptr, lyr);

    EXPECT_EQ(SPI_IN_BUILDING, lyr->GetSpatialIndexState());

    // Set spatial filter and call GetFeatureCount() -> builds quadtree and
    // transitions to SPI_COMPLETED
    lyr->SetSpatialFilterRect(0, 0, 3, 3);
    EXPECT_EQ(1, lyr->GetFeatureCount(true));
    EXPECT_EQ(SPI_COMPLETED, lyr->GetSpatialIndexState());

    // Update filter when SPI_COMPLETED -> searches quadtree and sorts features
    lyr->SetSpatialFilterRect(0, 0, 8, 3);
    EXPECT_EQ(2, lyr->GetFeatureCount(true));

    auto* feat = lyr->GetNextFeature();
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(1, feat->GetFieldAsInteger("id"));
    delete feat;

    feat = lyr->GetNextFeature();
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(2, feat->GetFieldAsInteger("id"));
    delete feat;
    EXPECT_EQ(nullptr, lyr->GetNextFeature());

    // SetNextByIndex with quadtree filtered features
    EXPECT_EQ(OGRERR_NONE, lyr->SetNextByIndex(0));
    feat = lyr->GetNextFeature();
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(1, feat->GetFieldAsInteger("id"));
    delete feat;

    EXPECT_EQ(OGRERR_NON_EXISTING_FEATURE, lyr->SetNextByIndex(-1));
    EXPECT_EQ(OGRERR_NON_EXISTING_FEATURE, lyr->SetNextByIndex(999));

    // Reset reading when SPI_COMPLETED
    lyr->ResetReading();
    feat = lyr->GetNextFeature();
    ASSERT_NE(nullptr, feat);
    delete feat;
  }

  // 3. QuadTree spatial filter iteration via GetNextFeature
  {
    CPLSetThreadLocalConfigOption("OPENFILEGDB_USE_SPATIAL_INDEX", "NO");
    CPLSetThreadLocalConfigOption("OPENFILEGDB_IN_MEMORY_SPI", "YES");
    absl::Cleanup reset_opt = [] {
      CPLSetThreadLocalConfigOption("OPENFILEGDB_USE_SPATIAL_INDEX", nullptr);
      CPLSetThreadLocalConfigOption("OPENFILEGDB_IN_MEMORY_SPI", nullptr);
    };

    auto ds_iter = OpenGDB(gdb);
    ASSERT_NE(nullptr, ds_iter);
    auto* lyr_iter = cpl::down_cast<OGROpenFileGDBLayer*>(
        ds_iter->GetLayerByName("poly_lyr"));
    ASSERT_NE(nullptr, lyr_iter);
    EXPECT_TRUE(lyr_iter->TestCapability(OLCFastSetNextByIndex));
    EXPECT_TRUE(lyr_iter->TestCapability(OLCFastGetExtent3D));
    OGREnvelope3D env3d_2d;
    EXPECT_EQ(OGRERR_NONE, lyr_iter->IGetExtent3D(0, &env3d_2d, false));

    lyr_iter->SetSpatialFilterRect(0, 0, 3, 3);
    int cnt = 0;
    while (auto* f = lyr_iter->GetNextFeature()) {
      cnt++;
      delete f;
    }
    EXPECT_EQ(1, cnt);
    EXPECT_EQ(SPI_COMPLETED, lyr_iter->GetSpatialIndexState());
  }

  // 4. SetNextByIndex without filter on unmodified layer
  {
    auto ds_nbi = OpenGDB(gdb);
    ASSERT_NE(nullptr, ds_nbi);
    auto* lyr_nbi = cpl::down_cast<OGROpenFileGDBLayer*>(
        ds_nbi->GetLayerByName("poly_lyr"));
    ASSERT_NE(nullptr, lyr_nbi);
    EXPECT_EQ(OGRERR_NONE, lyr_nbi->SetNextByIndex(1));
    EXPECT_EQ(OGRERR_NON_EXISTING_FEATURE, lyr_nbi->SetNextByIndex(-1));
    EXPECT_EQ(OGRERR_NON_EXISTING_FEATURE, lyr_nbi->SetNextByIndex(999));
  }
}

// -----------------------------------------------------------------------------
// Test 5: FeatureReadingGeometriesIgnoredFieldsAndDeletedRows
// -----------------------------------------------------------------------------
TEST_F(OgrOpenFileGdbLayerTest,
       FeatureReadingGeometriesIgnoredFieldsAndDeletedRows) {
  const std::string gdb = std::string(kTestDir) + "/geom_reading.gdb";
  {
    OGROpenFileGDBDataSource ds;
    ASSERT_TRUE(ds.Create(gdb.c_str()));

    // Create 3D Layer with Polygon
    OGRGeomFieldDefn geom_field("SHAPE", wkbPolygon25D);
    auto* lyr = ds.ICreateLayer("geom_lyr", &geom_field, nullptr);
    ASSERT_NE(nullptr, lyr);

    OGRFieldDefn f_name("name", OFTString);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_name));
    OGRFieldDefn f_val("val", OFTInteger);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_val));

    // Feature 1: Valid Polygon with Z
    {
      OGRFeature feat(lyr->GetLayerDefn());
      feat.SetField("name", "poly1");
      feat.SetField("val", 100);
      OGRLinearRing ring;
      ring.addPoint(0, 0, 10);
      ring.addPoint(2, 0, 10);
      ring.addPoint(2, 2, 10);
      ring.addPoint(0, 2, 10);
      ring.addPoint(0, 0, 10);
      OGRPolygon poly;
      poly.addRing(&ring);
      feat.SetGeometry(&poly);
      EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(&feat));
    }

    // Feature 2: Polygon with Z=20
    {
      OGRFeature feat(lyr->GetLayerDefn());
      feat.SetField("name", "poly2");
      feat.SetField("val", 200);
      OGRLinearRing ring;
      ring.addPoint(5, 5, 20);
      ring.addPoint(7, 5, 20);
      ring.addPoint(7, 7, 20);
      ring.addPoint(5, 7, 20);
      ring.addPoint(5, 5, 20);
      OGRPolygon poly;
      poly.addRing(&ring);
      feat.SetGeometry(&poly);
      EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(&feat));
    }

    // Feature 3: To delete
    {
      OGRFeature feat(lyr->GetLayerDefn());
      feat.SetField("name", "poly3");
      feat.SetField("val", 300);
      OGRLinearRing ring;
      ring.addPoint(10, 10, 30);
      ring.addPoint(12, 10, 30);
      ring.addPoint(12, 12, 30);
      ring.addPoint(10, 12, 30);
      ring.addPoint(10, 10, 30);
      OGRPolygon poly;
      poly.addRing(&ring);
      feat.SetGeometry(&poly);
      EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(&feat));
    }

    // Layer with CurvePolygon
    {
      OGRGeomFieldDefn g_cp("SHAPE", wkbCurvePolygon);
      auto* lyr_cp = ds.ICreateLayer("curve_lyr", &g_cp, nullptr);
      ASSERT_NE(nullptr, lyr_cp);
      OGRFeature feat(lyr_cp->GetLayerDefn());
      OGRGeometry* geom = nullptr;
      OGRGeometryFactory::createFromWkt(
          "CURVEPOLYGON (CIRCULARSTRING (0 0, 1 1, 2 0, 1 -1, 0 0))", nullptr,
          &geom);
      ASSERT_NE(nullptr, geom);
      feat.SetGeometryDirectly(geom);
      EXPECT_EQ(OGRERR_NONE, lyr_cp->CreateFeature(&feat));
    }

    // Layer with CompoundCurve / LineString
    {
      OGRGeomFieldDefn g_mc("SHAPE", wkbCompoundCurve);
      auto* lyr_mc = ds.ICreateLayer("line_lyr", &g_mc, nullptr);
      ASSERT_NE(nullptr, lyr_mc);
      {
        OGRFeature feat(lyr_mc->GetLayerDefn());
        OGRGeometry* geom = nullptr;
        OGRGeometryFactory::createFromWkt("LINESTRING (0 0, 1 1)", nullptr,
                                          &geom);
        ASSERT_NE(nullptr, geom);
        feat.SetGeometryDirectly(geom);
        EXPECT_EQ(OGRERR_NONE, lyr_mc->CreateFeature(&feat));
      }
      {
        OGRFeature feat(lyr_mc->GetLayerDefn());
        OGRGeometry* geom = nullptr;
        OGRGeometryFactory::createFromWkt(
            "COMPOUNDCURVE (CIRCULARSTRING (0 0, 1 1, 2 0), (2 0, 3 0))",
            nullptr, &geom);
        ASSERT_NE(nullptr, geom);
        feat.SetGeometryDirectly(geom);
        EXPECT_EQ(OGRERR_NONE, lyr_mc->CreateFeature(&feat));
      }
    }

    CloseDS(ds);
  }

  // Open in update mode to delete Feature 3
  {
    auto ds_up = OpenGDB(gdb, GDAL_OF_VECTOR | GDAL_OF_UPDATE);
    ASSERT_NE(nullptr, ds_up);
    auto* lyr = ds_up->GetLayerByName("geom_lyr");
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(OGRERR_NONE, lyr->DeleteFeature(3));
    CloseDS(ds_up.get());
  }

  // Re-open read-only
  auto ds = OpenGDB(gdb);
  ASSERT_NE(nullptr, ds);
  auto* lyr =
      cpl::down_cast<OGROpenFileGDBLayer*>(ds->GetLayerByName("geom_lyr"));
  ASSERT_NE(nullptr, lyr);

  // 1. GetExtent & GetExtent3D
  OGREnvelope env;
  EXPECT_EQ(OGRERR_NONE, lyr->GetExtent(&env, false));
  EXPECT_DOUBLE_EQ(0.0, env.MinX);
  EXPECT_DOUBLE_EQ(12.0, env.MaxX);

  OGREnvelope3D env3d;
  EXPECT_EQ(OGRERR_NONE, lyr->GetExtent3D(0, &env3d, false));
  EXPECT_DOUBLE_EQ(0.0, env3d.MinX);
  EXPECT_DOUBLE_EQ(10.0, env3d.MinZ);
  EXPECT_DOUBLE_EQ(30.0, env3d.MaxZ);

  // 2. Random read GetFeature
  auto* f1 = lyr->GetFeature(1);
  ASSERT_NE(nullptr, f1);
  EXPECT_EQ(1, f1->GetFID());
  EXPECT_STREQ("poly1", f1->GetFieldAsString("name"));
  ASSERT_NE(nullptr, f1->GetGeometryRef());
  // Flattens to MultiPolygon in GetCurrentFeature
  EXPECT_EQ(wkbMultiPolygon25D, f1->GetGeometryRef()->getGeometryType());
  delete f1;

  // Deleted feature returns nullptr
  EXPECT_EQ(nullptr, lyr->GetFeature(3));
  // Out of range feature returns nullptr
  EXPECT_EQ(nullptr, lyr->GetFeature(0));
  EXPECT_EQ(nullptr, lyr->GetFeature(999));

  // 3. SetNextByIndex when deleted rows exist (GetValidRecordCount !=
  // GetTotalRecordCount)
  EXPECT_EQ(2, lyr->GetFeatureCount(true));
  EXPECT_EQ(OGRERR_NONE, lyr->SetNextByIndex(1));
  auto* f2 = lyr->GetNextFeature();
  ASSERT_NE(nullptr, f2);
  EXPECT_EQ(2, f2->GetFID());
  delete f2;

  // 4. Ignored Fields (SetIgnoredFields)
  const char* const ignored[] = {"name", "SHAPE", nullptr};
  EXPECT_EQ(OGRERR_NONE, lyr->SetIgnoredFields(ignored));
  lyr->ResetReading();
  f1 = lyr->GetNextFeature();
  ASSERT_NE(nullptr, f1);
  EXPECT_EQ(nullptr, f1->GetGeometryRef());   // ignored geom
  EXPECT_FALSE(f1->IsFieldSetAndNotNull(0));  // ignored field
  EXPECT_TRUE(f1->IsFieldSetAndNotNull(1));
  delete f1;

  // Clear ignored fields
  EXPECT_EQ(OGRERR_NONE, lyr->SetIgnoredFields(nullptr));

  // Read curve and compound curve layers
  {
    auto* cp_lyr = ds->GetLayerByName("curve_lyr");
    ASSERT_NE(nullptr, cp_lyr);
    auto* f_cp = cp_lyr->GetNextFeature();
    ASSERT_NE(nullptr, f_cp);
    EXPECT_EQ(wkbMultiSurface, f_cp->GetGeometryRef()->getGeometryType());
    delete f_cp;

    auto* mc_lyr = ds->GetLayerByName("line_lyr");
    ASSERT_NE(nullptr, mc_lyr);
    auto* f_mc1 = mc_lyr->GetNextFeature();
    ASSERT_NE(nullptr, f_mc1);
    delete f_mc1;
    auto* f_mc2 = mc_lyr->GetNextFeature();
    ASSERT_NE(nullptr, f_mc2);
    EXPECT_EQ(wkbMultiCurve, f_mc2->GetGeometryRef()->getGeometryType());
    delete f_mc2;
  }

  // 5. TestCapability checks
  EXPECT_TRUE(lyr->TestCapability(OLCRandomRead));
  EXPECT_TRUE(lyr->TestCapability(OLCFastGetExtent));
  EXPECT_TRUE(lyr->TestCapability(OLCFastGetExtent3D));
  EXPECT_TRUE(lyr->TestCapability(OLCIgnoreFields));
  EXPECT_TRUE(lyr->TestCapability(OLCStringsAsUTF8));
  EXPECT_TRUE(lyr->TestCapability(OLCMeasuredGeometries));
  EXPECT_TRUE(lyr->TestCapability(OLCCurveGeometries));
  EXPECT_TRUE(lyr->TestCapability(OLCZGeometries));
  EXPECT_TRUE(lyr->TestCapability(OLCFastFeatureCount));
  EXPECT_FALSE(lyr->TestCapability(OLCSequentialWrite));
  EXPECT_FALSE(lyr->TestCapability("nonexistent"));
}

}  // namespace
}  // namespace autotest2
