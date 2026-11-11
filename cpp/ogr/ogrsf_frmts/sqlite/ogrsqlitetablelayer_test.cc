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
// Tests for OGRSQLiteTableLayer (ogrsqlitetablelayer.cpp).
//
// See also:
//   https://gdal.org/drivers/vector/sqlite.html
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/sqlite/ogrsqlitetablelayer.cpp
//
// google3 build caveats:
//   - Compiled without HAVE_SPATIALITE; uses SQLite >= 3.37 (STRICT tables)
//     and bundled PROJ database.

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_p.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/ogr/ogrsf_frmts/sqlite/ogr_sqlite.h"
#include "third_party/gdal/ogr/ogrsf_frmts/sqlite/ogrsqliteutility.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_time.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/sqlite/sqlite3.h"

namespace autotest2 {
namespace {

using ::testing::Contains;
using ::testing::HasSubstr;

// Appends CPL error and warning messages to the `std::vector<std::string>`
// passed via `CPLErrorHandlerPusher` user data.
void CPL_STDCALL RecordErrorMessage(CPLErr /*error_class*/,
                                    CPLErrorNum /*error_num*/,
                                    const char* msg) {
  auto* error_msgs =
      static_cast<std::vector<std::string>*>(CPLGetErrorHandlerUserData());
  if (error_msgs != nullptr && msg != nullptr) {
    error_msgs->emplace_back(msg);
  }
}

template <typename Tag, typename Tag::type M>
struct AccessPrivate {
  friend typename Tag::type GetPrivate(Tag) { return M; }
};
struct LoadStatsSp4Tag {
  using type = void (OGRSQLiteTableLayer::*)();
  friend type GetPrivate(LoadStatsSp4Tag);
};
template struct AccessPrivate<
    LoadStatsSp4Tag, &OGRSQLiteTableLayer::LoadStatisticsSpatialite4DB>;

constexpr char kTestDir[] = "/vsimem/tablelayer_test";

struct ExposedGDALDataset : public GDALDataset {
  static void SetUpdate(GDALDataset* poDS) {
    static_cast<ExposedGDALDataset*>(poDS)->eAccess = GA_Update;
  }
};

class OgrSqliteTableLayerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRSQLite();
    VSIStatBufL sStat;
    if (VSIStatL(kTestDir, &sStat) == 0) {
      VSIRmdirRecursive(kTestDir);
    }
    VSIMkdir(kTestDir, 0755);
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }

  static void TearDownTestSuite() { OSRCleanup(); }
};

TEST_F(OgrSqliteTableLayerTest, FieldDefnToSqliteFieldDefnAllTypesAndModes) {
  // Test OGRSQLiteFieldDefnToSQliteFieldDefn for both bStrict = false and
  // bStrict = true.
  struct TestCase {
    OGRFieldType type;
    OGRFieldSubType subtype;
    int width;
    bool bStrict;
    bool bInternalUse;
    std::string expected;
  };

  const std::vector<TestCase> cases = {
      // Non-strict mode
      {OFTInteger, OFSTNone, 0, false, false, "INTEGER"},
      {OFTInteger, OFSTBoolean, 0, false, false, "INTEGER_BOOLEAN"},
      {OFTInteger, OFSTInt16, 0, false, false, "INTEGER_INT16"},
      {OFTInteger64, OFSTNone, 0, false, false, "BIGINT"},
      {OFTReal, OFSTNone, 0, false, false, "FLOAT"},
      {OFTReal, OFSTFloat32, 0, false, true, "FLOAT_FLOAT32"},
      {OFTReal, OFSTFloat32, 0, false, false, "FLOAT"},
      {OFTBinary, OFSTNone, 0, false, false, "BLOB"},
      {OFTString, OFSTNone, 0, false, false, "VARCHAR"},
      {OFTString, OFSTNone, 64, false, false, "VARCHAR(64)"},
      {OFTDateTime, OFSTNone, 0, false, false, "TIMESTAMP"},
      {OFTDate, OFSTNone, 0, false, false, "DATE"},
      {OFTTime, OFSTNone, 0, false, false, "TIME"},
      {OFTIntegerList, OFSTNone, 0, false, false, "JSONINTEGERLIST"},
      {OFTInteger64List, OFSTNone, 0, false, false, "JSONINTEGER64LIST"},
      {OFTRealList, OFSTNone, 0, false, false, "JSONREALLIST"},
      {OFTStringList, OFSTNone, 0, false, false, "JSONSTRINGLIST"},
      {OFTWideString, OFSTNone, 0, false, false, "VARCHAR"},

      // Strict mode
      {OFTInteger, OFSTNone, 0, true, false,
       "INTEGER CHECK (\"fld\" BETWEEN -2147483648 AND 2147483647)"},
      {OFTInteger64, OFSTNone, 0, true, false,
       "INTEGER CHECK (\"fld\" BETWEEN -9223372036854775808 AND "
       "9223372036854775807)"},
      {OFTReal, OFSTNone, 0, true, false, "REAL"},
      {OFTBinary, OFSTNone, 0, true, false, "BLOB"},
      {OFTDateTime, OFSTNone, 0, true, false,
       "TEXT CHECK (\"fld\" LIKE "
       "'[0-9][0-9][0-9][0-9]-[0-1][0-9]-[0-3][0-9]T[0-2][0-9]:[0-5][0-9]:[0-6]"
       "[0-9]*')"},
      {OFTDate, OFSTNone, 0, true, false,
       "TEXT CHECK (\"fld\" LIKE "
       "'[0-9][0-9][0-9][0-9]-[0-1][0-9]-[0-3][0-9]T')"},
      {OFTTime, OFSTNone, 0, true, false,
       "TEXT CHECK (\"fld\" LIKE '[0-2][0-9]:[0-5][0-9]:[0-6][0-9]*')"},
      {OFTString, OFSTNone, 0, true, false, "TEXT"},
  };

  for (const auto& c : cases) {
    OGRFieldDefn fld("fld", c.type);
    fld.SetSubType(c.subtype);
    fld.SetWidth(c.width);
    CPLString res =
        OGRSQLiteFieldDefnToSQliteFieldDefn(&fld, c.bInternalUse, c.bStrict);
    EXPECT_EQ(std::string(res.c_str()), c.expected);
  }
}

TEST_F(OgrSqliteTableLayerTest, NonSpatialiteFormatsAndAllFieldTypesCRUD) {
  const std::string dbPath = std::string(kTestDir) + "/non_sp.db";
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("SQLite");
  ASSERT_NE(poDriver, nullptr);

  CPLStringList osDSCO;
  osDSCO.AddNameValue("METADATA", "YES");

  std::unique_ptr<GDALDataset> poDS(
      poDriver->Create(dbPath.c_str(), 0, 0, 0, GDT_Unknown, osDSCO.List()));
  ASSERT_NE(poDS, nullptr);

  CPLStringList osLCO;
  osLCO.AddNameValue("FORMAT", "WKT");
  osLCO.AddNameValue("LAUNDER", "YES");
  osLCO.AddNameValue("FID", "custom_fid");
  osLCO.AddNameValue("GEOMETRY_NAME", "wkt_geom");
  osLCO.AddNameValue("COMPRESS_COLUMNS", "comp_str");

  OGRLayer* poLayer =
      poDS->CreateLayer("test_tbl", nullptr, wkbPoint, osLCO.List());
  ASSERT_NE(poLayer, nullptr);

  auto* poTableLayer = dynamic_cast<OGRSQLiteTableLayer*>(poLayer);
  ASSERT_NE(poTableLayer, nullptr);

  EXPECT_STREQ(poTableLayer->GetName(), "test_tbl");
  EXPECT_TRUE(poTableLayer->IsTableLayer());

  // Test CreateGeomField
  {
    OGRGeomFieldDefn oNoneGeom("none_geom", wkbNone);
    EXPECT_EQ(poTableLayer->CreateGeomField(&oNoneGeom), OGRERR_FAILURE);

    // Empty name becomes GEOMETRY2 (or geometry2 when laundered)
    OGRGeomFieldDefn oEmptyNameGeom("", wkbPolygon);
    auto* poSRS = new OGRSpatialReference();
    EXPECT_EQ(poSRS->importFromEPSG(4326), OGRERR_NONE);
    oEmptyNameGeom.SetSpatialRef(poSRS);
    poSRS->Release();
    oEmptyNameGeom.SetNullable(0);
    EXPECT_EQ(poTableLayer->CreateGeomField(&oEmptyNameGeom), OGRERR_NONE);
    EXPECT_TRUE(
        EQUAL(poTableLayer->GetLayerDefn()->GetGeomFieldDefn(1)->GetNameRef(),
              "GEOMETRY2"));
  }

  // Test CreateField with invalid FID matching type
  {
    OGRFieldDefn oBadFID("custom_fid", OFTString);
    EXPECT_EQ(poTableLayer->CreateField(&oBadFID), OGRERR_FAILURE);

    // Valid FID column matching FID name
    OGRFieldDefn oGoodFID("custom_fid", OFTInteger64);
    EXPECT_EQ(poTableLayer->CreateField(&oGoodFID), OGRERR_NONE);
  }

  // Test CreateField with all supported field types
  OGRFieldDefn fInt("f_int", OFTInteger);
  EXPECT_EQ(poTableLayer->CreateField(&fInt), OGRERR_NONE);

  OGRFieldDefn fBool("f_bool", OFTInteger);
  fBool.SetSubType(OFSTBoolean);
  EXPECT_EQ(poTableLayer->CreateField(&fBool), OGRERR_NONE);

  OGRFieldDefn fInt16("f_int16", OFTInteger);
  fInt16.SetSubType(OFSTInt16);
  EXPECT_EQ(poTableLayer->CreateField(&fInt16), OGRERR_NONE);

  OGRFieldDefn fInt64("f_int64", OFTInteger64);
  EXPECT_EQ(poTableLayer->CreateField(&fInt64), OGRERR_NONE);

  OGRFieldDefn fReal("f_real", OFTReal);
  EXPECT_EQ(poTableLayer->CreateField(&fReal), OGRERR_NONE);

  OGRFieldDefn fBlob("f_blob", OFTBinary);
  EXPECT_EQ(poTableLayer->CreateField(&fBlob), OGRERR_NONE);

  OGRFieldDefn fStr("f_str", OFTString);
  fStr.SetWidth(32);
  EXPECT_EQ(poTableLayer->CreateField(&fStr), OGRERR_NONE);

  OGRFieldDefn fCompStr("comp_str", OFTString);
  EXPECT_EQ(poTableLayer->CreateField(&fCompStr), OGRERR_NONE);

  OGRFieldDefn fDateTime("f_datetime", OFTDateTime);
  EXPECT_EQ(poTableLayer->CreateField(&fDateTime), OGRERR_NONE);

  OGRFieldDefn fDate("f_date", OFTDate);
  EXPECT_EQ(poTableLayer->CreateField(&fDate), OGRERR_NONE);

  OGRFieldDefn fTime("f_time", OFTTime);
  EXPECT_EQ(poTableLayer->CreateField(&fTime), OGRERR_NONE);

  OGRFieldDefn fIntList("f_intlist", OFTIntegerList);
  EXPECT_EQ(poTableLayer->CreateField(&fIntList), OGRERR_NONE);

  OGRFieldDefn fInt64List("f_int64list", OFTInteger64List);
  EXPECT_EQ(poTableLayer->CreateField(&fInt64List), OGRERR_NONE);

  OGRFieldDefn fRealList("f_reallist", OFTRealList);
  EXPECT_EQ(poTableLayer->CreateField(&fRealList), OGRERR_NONE);

  OGRFieldDefn fStrList("f_strlist", OFTStringList);
  EXPECT_EQ(poTableLayer->CreateField(&fStrList), OGRERR_NONE);

  // Field with default value and not nullable
  OGRFieldDefn fDefaultNotNull("f_def_notnull", OFTString);
  fDefaultNotNull.SetDefault("'hello'");
  fDefaultNotNull.SetNullable(0);
  EXPECT_EQ(poTableLayer->CreateField(&fDefaultNotNull), OGRERR_NONE);

  // Field with UNIQUE
  OGRFieldDefn fUnique("f_unique", OFTString);
  fUnique.SetUnique(TRUE);
  EXPECT_EQ(poTableLayer->CreateField(&fUnique), OGRERR_NONE);

  // Test OGR_SQLITE_ENABLE_DATETIME=NO fallback
  {
    CPLSetConfigOption("OGR_SQLITE_ENABLE_DATETIME", "NO");
    OGRFieldDefn fDateFallback("f_date_fallback", OFTDate);
    EXPECT_EQ(poTableLayer->CreateField(&fDateFallback), OGRERR_NONE);
    CPLSetConfigOption("OGR_SQLITE_ENABLE_DATETIME", nullptr);
    int idx = poTableLayer->GetLayerDefn()->GetFieldIndex("f_date_fallback");
    ASSERT_GE(idx, 0);
    EXPECT_EQ(poTableLayer->GetLayerDefn()->GetFieldDefn(idx)->GetType(),
              OFTString);
  }

  // Insert feature 1: all fields set
  {
    OGRFeature oFeat(poTableLayer->GetLayerDefn());
    OGRPoint pt(10.0, 20.0);
    oFeat.SetGeomField(0, &pt);
    OGRPolygon poly;
    OGRLinearRing ring;
    ring.addPoint(0, 0);
    ring.addPoint(0, 1);
    ring.addPoint(1, 1);
    ring.addPoint(1, 0);
    ring.addPoint(0, 0);
    poly.addRing(&ring);
    oFeat.SetGeomField(1, &poly);

    oFeat.SetField("f_int", 42);
    oFeat.SetField("f_bool", 1);
    oFeat.SetField("f_int16", 123);
    oFeat.SetField("f_int64", static_cast<GIntBig>(9876543210LL));
    oFeat.SetField("f_real", 3.14159);
    const GByte abyBlob[] = {0xDE, 0xAD, 0xBE, 0xEF};
    oFeat.SetField(oFeat.GetFieldIndex("f_blob"), sizeof(abyBlob), abyBlob);
    oFeat.SetField("f_str", "test string");
    oFeat.SetField(
        "comp_str",
        "this is a very compressible string repeated repeated repeated");
    oFeat.SetField("f_datetime", 2026, 11, 3, 12, 34, 56, 0);
    oFeat.SetField("f_date", 2026, 11, 3, 0, 0, 0, 0);
    oFeat.SetField("f_time", 0, 0, 0, 10, 20, 30.5f, 0);
    const int anInts[] = {1, 2, 3};
    oFeat.SetField("f_intlist", 3, anInts);
    const GIntBig anInt64s[] = {100LL, 200LL};
    oFeat.SetField("f_int64list", 2, anInt64s);
    const double adfReals[] = {1.1, 2.2};
    oFeat.SetField("f_reallist", 2, adfReals);
    CPLStringList aosStrings;
    aosStrings.AddString("apple");
    aosStrings.AddString("banana");
    oFeat.SetField("f_strlist", aosStrings.List());
    oFeat.SetField("f_def_notnull", "explicit_hello");
    oFeat.SetField("f_unique", "unique_1");

    EXPECT_EQ(poTableLayer->CreateFeature(&oFeat), OGRERR_NONE);
    EXPECT_EQ(oFeat.GetFID(), 1);
  }

  // Insert feature 2: test unset default field (bHasDefaultValue = true) with
  // non-null geom
  {
    OGRFeature oFeat(poTableLayer->GetLayerDefn());
    OGRPoint pt(1.0, 2.0);
    oFeat.SetGeomField(0, &pt);
    OGRPolygon poly;
    OGRLinearRing ring;
    ring.addPoint(0, 0);
    ring.addPoint(0, 1);
    ring.addPoint(1, 1);
    ring.addPoint(0, 0);
    poly.addRing(&ring);
    oFeat.SetGeomField(1, &poly);
    oFeat.SetField("f_time", 0, 0, 0, 10, 20, 30.0f, 0);  // 0 ms branch
    oFeat.SetField("f_unique", "unique_2");

    EXPECT_EQ(poTableLayer->CreateFeature(&oFeat), OGRERR_NONE);
    EXPECT_EQ(oFeat.GetFID(), 2);
  }

  // Insert feature 3: test statement reuse (bReuseStmt = true)
  {
    OGRFeature oFeat(poTableLayer->GetLayerDefn());
    OGRPoint pt(3.0, 4.0);
    oFeat.SetGeomField(0, &pt);
    OGRPolygon poly;
    OGRLinearRing ring;
    ring.addPoint(0, 0);
    ring.addPoint(0, 2);
    ring.addPoint(2, 2);
    ring.addPoint(0, 0);
    poly.addRing(&ring);
    oFeat.SetGeomField(1, &poly);
    oFeat.SetField("f_unique", "unique_3");
    EXPECT_EQ(poTableLayer->CreateFeature(&oFeat), OGRERR_NONE);
    EXPECT_EQ(oFeat.GetFID(), 3);
  }

  // Test OSGF_WKB binding in BindValues and CreateGeomField/CreateField on WKB
  // layer
  {
    OGRLayer* poWkbLayer =
        poDS->CreateLayer("wkb_tbl", nullptr, wkbPoint, nullptr);
    ASSERT_NE(poWkbLayer, nullptr);
    auto* poWkbTable = dynamic_cast<OGRSQLiteTableLayer*>(poWkbLayer);
    ASSERT_NE(poWkbTable, nullptr);

    {
      OGRFeature oFeat(poWkbTable->GetLayerDefn());
      OGRPoint pt(5.0, 6.0);
      oFeat.SetGeometry(&pt);
      EXPECT_EQ(poWkbTable->CreateFeature(&oFeat), OGRERR_NONE);
    }

    OGRFieldDefn oUniqImmediate("u_imm", OFTInteger);
    oUniqImmediate.SetUnique(TRUE);
    CPLErrorReset();
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(poWkbTable->CreateField(&oUniqImmediate), OGRERR_FAILURE);
    OGRGeomFieldDefn oDupGeom("GEOMETRY", wkbPoint);
    EXPECT_EQ(poWkbTable->CreateGeomField(&oDupGeom), OGRERR_FAILURE);
    CPLPopErrorHandler();

    OGRGeomFieldDefn oWkbGeom2("geom2", wkbLineString);
    EXPECT_EQ(poWkbTable->CreateGeomField(&oWkbGeom2), OGRERR_NONE);

    OGRFieldDefn fDef("col_def", OFTString);
    fDef.SetDefault("'def'");
    EXPECT_EQ(poWkbTable->CreateField(&fDef), OGRERR_NONE);
  }

  // Test deferred creation with strftime default (lines 3619-3622) and UNIQUE +
  // DEFAULT
  {
    OGRLayer* poDefLayer =
        poDS->CreateLayer("def_tbl", nullptr, wkbPoint, nullptr);
    ASSERT_NE(poDefLayer, nullptr);
    auto* poDefTable = dynamic_cast<OGRSQLiteTableLayer*>(poDefLayer);
    ASSERT_NE(poDefTable, nullptr);

    OGRFieldDefn fStrf("f_strf", OFTString);
    fStrf.SetDefault("(strftime('%Y','now'))");
    EXPECT_EQ(poDefTable->CreateField(&fStrf), OGRERR_NONE);

    OGRFieldDefn fUniqDef("uniq_def", OFTString);
    fUniqDef.SetUnique(TRUE);
    fUniqDef.SetDefault("'def'");
    EXPECT_EQ(poDefTable->CreateField(&fUniqDef), OGRERR_NONE);

    OGRFeature oFeat(poDefTable->GetLayerDefn());
    OGRPoint pt(1.0, 1.0);
    oFeat.SetGeometry(&pt);
    EXPECT_EQ(poDefTable->CreateFeature(&oFeat), OGRERR_NONE);
  }

  // Test duplicate UNIQUE constraint error in CreateFeature
  {
    OGRFeature oFeat(poTableLayer->GetLayerDefn());
    OGRPoint pt(0, 0);
    oFeat.SetGeomField(0, &pt);
    OGRPolygon poly;
    OGRLinearRing ring;
    ring.addPoint(0, 0);
    ring.addPoint(0, 1);
    ring.addPoint(1, 1);
    ring.addPoint(0, 0);
    poly.addRing(&ring);
    oFeat.SetGeomField(1, &poly);
    oFeat.SetField("f_unique", "unique_1");  // duplicate!
    CPLErrorReset();
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(poTableLayer->CreateFeature(&oFeat), OGRERR_FAILURE);
    CPLPopErrorHandler();
  }

  // Test m_iFIDAsRegularColumnIndex with mismatched value
  {
    OGRFeature oFeat(poTableLayer->GetLayerDefn());
    oFeat.SetFID(10);
    oFeat.SetField("custom_fid", static_cast<GIntBig>(999));  // mismatch!
    CPLErrorReset();
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(poTableLayer->CreateFeature(&oFeat), OGRERR_FAILURE);
    CPLPopErrorHandler();
  }

  // Test m_iFIDAsRegularColumnIndex with FID == OGRNullFID
  {
    OGRFeature oFeat(poTableLayer->GetLayerDefn());
    OGRPoint pt(10.0, 20.0);
    oFeat.SetGeomField(0, &pt);
    OGRPolygon poly;
    OGRLinearRing ring;
    ring.addPoint(0, 0);
    ring.addPoint(0, 1);
    ring.addPoint(1, 1);
    ring.addPoint(1, 0);
    ring.addPoint(0, 0);
    poly.addRing(&ring);
    oFeat.SetGeomField(1, &poly);
    oFeat.SetField("f_def_notnull", "valid_def");
    oFeat.SetField("f_unique", "unique_fid777");
    oFeat.SetField("custom_fid", static_cast<GIntBig>(777));
    EXPECT_EQ(poTableLayer->CreateFeature(&oFeat), OGRERR_NONE);
    EXPECT_EQ(oFeat.GetFID(), 777);
  }

  // Test unset default field with null geometry (omits ? in INSERT, hits bind
  // error in lines 2653, 3399)
  {
    OGRFeature oFeatFail(poTableLayer->GetLayerDefn());
    oFeatFail.SetField("f_unique", "unique_fail_geom");
    CPLErrorReset();
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(poTableLayer->CreateFeature(&oFeatFail), OGRERR_FAILURE);
    CPLPopErrorHandler();
  }

  // TODO: b/335317901 - Upstream GDAL bug in
  // OGRSQLiteTableLayer::RecomputeOrdinals() (ogrsqlitetablelayer.cpp:796-805):
  // when a column is both m_pszFIDColumn and a regular field ("custom_fid"),
  // RecomputeOrdinals() increments nCountFieldOrdinals without initializing
  // m_panFieldOrdinals[nIdx] from CPLMalloc. Ignoring "custom_fid" avoids the
  // uninitialized m_panFieldOrdinals[0] read in GetNextRawFeature() across all
  // builds while m_iFIDAsRegularColumnIndex still populates the field from FID.
  constexpr std::array<const char*, 2> kIgnoredFid = {"custom_fid", nullptr};
  ASSERT_EQ(poTableLayer->SetIgnoredFields(kIgnoredFid.data()), OGRERR_NONE);

  // Test GetFeature and GetNextFeature
  {
    std::unique_ptr<OGRFeature> poF(poTableLayer->GetFeature(1));
    ASSERT_NE(poF, nullptr);
    EXPECT_EQ(poF->GetFID(), 1);
    EXPECT_EQ(poF->GetFieldAsInteger("f_int"), 42);
    EXPECT_STREQ(
        poF->GetFieldAsString("comp_str"),
        "this is a very compressible string repeated repeated repeated");

    poTableLayer->ResetReading();
    std::unique_ptr<OGRFeature> poFNext(poTableLayer->GetNextFeature());
    ASSERT_NE(poFNext, nullptr);
    EXPECT_EQ(poFNext->GetFID(), 1);
    EXPECT_EQ(poFNext->GetFieldAsInteger64("custom_fid"), 1);
  }

  // Test ISetFeature
  {
    std::unique_ptr<OGRFeature> poF(poTableLayer->GetFeature(1));
    ASSERT_NE(poF, nullptr);
    poF->SetField("f_str", "updated string");

    // Unset FID fails
    poF->SetFID(OGRNullFID);
    CPLErrorReset();
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(poTableLayer->SetFeature(poF.get()), OGRERR_FAILURE);
    CPLPopErrorHandler();

    // Mismatched FID vs regular column fails
    poF->SetFID(1);
    poF->SetField("custom_fid", static_cast<GIntBig>(999));
    CPLErrorReset();
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(poTableLayer->SetFeature(poF.get()), OGRERR_FAILURE);
    CPLPopErrorHandler();

    // Valid update
    poF->SetField("custom_fid", static_cast<GIntBig>(1));
    EXPECT_EQ(poTableLayer->SetFeature(poF.get()), OGRERR_NONE);

    // Constraint violation in sqlite3_step during SetFeature (lines 2970-2974)
    std::unique_ptr<OGRFeature> poF2(poTableLayer->GetFeature(2));
    ASSERT_NE(poF2, nullptr);
    poF2->SetField("f_unique", "unique_1");
    CPLErrorReset();
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(poTableLayer->SetFeature(poF2.get()), OGRERR_FAILURE);
    CPLPopErrorHandler();

    // Non-existing feature
    poF->SetFID(999999);
    poF->SetField("custom_fid", static_cast<GIntBig>(999999));
    EXPECT_EQ(poTableLayer->SetFeature(poF.get()), OGRERR_NON_EXISTING_FEATURE);
  }

  // Test DeleteFeature
  {
    EXPECT_EQ(poTableLayer->DeleteFeature(999999), OGRERR_NON_EXISTING_FEATURE);
    EXPECT_EQ(poTableLayer->DeleteFeature(3), OGRERR_NONE);
  }

  // Test 64-bit FID detection: insert with FID > INT_MAX
  {
    OGRFeature oFeat(poTableLayer->GetLayerDefn());
    OGRPoint pt(0, 0);
    oFeat.SetGeomField(0, &pt);
    OGRPolygon poly;
    OGRLinearRing ring;
    ring.addPoint(0, 0);
    ring.addPoint(0, 1);
    ring.addPoint(1, 1);
    ring.addPoint(0, 0);
    poly.addRing(&ring);
    oFeat.SetGeomField(1, &poly);
    oFeat.SetFID(3000000000LL);
    oFeat.SetField("custom_fid", static_cast<GIntBig>(3000000000LL));
    oFeat.SetField("f_unique", "unique_big");
    EXPECT_EQ(poTableLayer->CreateFeature(&oFeat), OGRERR_NONE);

    EXPECT_EQ(poTableLayer->GetMetadataItem("OTHER"), nullptr);
    const char* pszFID64EmptyDomain =
        poTableLayer->GetMetadataItem(OLMD_FID64, "");
    ASSERT_NE(pszFID64EmptyDomain, nullptr);
    EXPECT_STREQ(pszFID64EmptyDomain, "YES");
    const char* pszFID64 = poTableLayer->GetMetadataItem(OLMD_FID64);
    ASSERT_NE(pszFID64, nullptr);
    EXPECT_STREQ(pszFID64, "YES");
  }

  // Test capabilities and attribute query / feature count edge cases on
  // test_tbl (without spatial index)
  {
    EXPECT_FALSE(poTableLayer->TestCapability("BogusCap"));

    poTableLayer->SetSpatialFilter(nullptr);
    poTableLayer->SetAttributeFilter(nullptr);
    GIntBig nCount = poTableLayer->GetFeatureCount(TRUE);
    EXPECT_GT(nCount, 0);
    // Calling GetFeatureCount(TRUE) again hits the cached count return (line
    // 1224)
    EXPECT_EQ(poTableLayer->GetFeatureCount(TRUE), nCount);

    // Spatial filter on layer without spatial index calls base GetFeatureCount
    // (line 1220)
    poTableLayer->SetSpatialFilterRect(0, 0, 10, 10);
    EXPECT_GE(poTableLayer->GetFeatureCount(TRUE), 0);
    poTableLayer->SetSpatialFilter(nullptr);

    // Attribute filter only without spatial filter hits BuildWhere (lines
    // 1140-1142)
    poTableLayer->SetAttributeFilter("f_int = 42");
    EXPECT_EQ(poTableLayer->GetFeatureCount(TRUE), 1);

    // Invalid SQL syntax in attribute filter hits ResetStatement error and
    // GetFeatureCount error
    poTableLayer->SetAttributeFilter("INVALID SQL SYNTAX ===");
    CPLErrorReset();
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(poTableLayer->GetNextFeature(), nullptr);
    EXPECT_EQ(poTableLayer->GetFeatureCount(TRUE), -1);
    CPLPopErrorHandler();
    poTableLayer->SetAttributeFilter(nullptr);
  }

  // Test CreateGeomField on existing table with non-nullable WKT column (lines
  // 1720-1735)
  {
    OGRGeomFieldDefn oLateWkt("late_wkt_geom", wkbPoint);
    oLateWkt.SetNullable(0);
    EXPECT_EQ(poTableLayer->CreateGeomField(&oLateWkt), OGRERR_NONE);
  }

  // Test WKT layer with unique field reorder, 3D geometry reload, and null
  // geometry_type metadata
  {
    CPLStringList osWktLCO;
    osWktLCO.AddNameValue("FORMAT", "WKT");
    OGRLayer* poWktLayer = poDS->CreateLayer("wkt_separate_tbl", nullptr,
                                             wkbPoint, osWktLCO.List());
    ASSERT_NE(poWktLayer, nullptr);
    auto* poWktTable = dynamic_cast<OGRSQLiteTableLayer*>(poWktLayer);
    ASSERT_NE(poWktTable, nullptr);

    OGRFieldDefn fUniq("u_col", OFTInteger);
    fUniq.SetUnique(TRUE);
    EXPECT_EQ(poWktTable->CreateField(&fUniq), OGRERR_NONE);

    OGRFieldDefn fExtra("f_extra", OFTString);
    EXPECT_EQ(poWktTable->CreateField(&fExtra), OGRERR_NONE);

    OGRGeomFieldDefn oWktGeom2("geom_wkt_nn", wkbLineString);
    oWktGeom2.SetNullable(0);
    EXPECT_EQ(poWktTable->CreateGeomField(&oWktGeom2), OGRERR_NONE);

    EXPECT_EQ(poWktTable->RunDeferredCreationIfNecessary(), OGRERR_NONE);

    std::array<int, 2> anMap = {1, 0};
    EXPECT_EQ(poWktTable->ReorderFields(anMap.data()), OGRERR_NONE);

    OGRGeomFieldDefn oGeom3D("geom_3d", wkbPoint25D);
    EXPECT_EQ(poWktTable->CreateGeomField(&oGeom3D), OGRERR_NONE);

    OGRGeomFieldDefn oGeomNullMeta("geom_null_meta", wkbPoint);
    EXPECT_EQ(poWktTable->CreateGeomField(&oGeomNullMeta), OGRERR_NONE);

    auto* poSQLiteDS = dynamic_cast<OGRSQLiteDataSource*>(poDS.get());
    ASSERT_NE(poSQLiteDS, nullptr);
    sqlite3* hDB = poSQLiteDS->GetDB();
    sqlite3_exec(hDB,
                 "UPDATE geometry_columns SET geometry_type = NULL WHERE "
                 "f_geometry_column = 'geom_null_meta'",
                 nullptr, nullptr, nullptr);

    poSQLiteDS->ReloadLayers();
    OGRLayer* poReloadedWkt = poDS->GetLayerByName("wkt_separate_tbl");
    ASSERT_NE(poReloadedWkt, nullptr);
    EXPECT_NE(poReloadedWkt->GetLayerDefn(), nullptr);
  }
}

TEST_F(OgrSqliteTableLayerTest,
       DDLOperationsDeleteAlterReorderForeignKeysAndStrict) {
  const std::string dbPath = std::string(kTestDir) + "/ddl.db";
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("SQLite");
  ASSERT_NE(poDriver, nullptr);

  std::unique_ptr<GDALDataset> poDS(
      poDriver->Create(dbPath.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(poDS, nullptr);

  // Create parent table
  OGRLayer* poParent =
      poDS->CreateLayer("parent_tbl", nullptr, wkbNone, nullptr);
  ASSERT_NE(poParent, nullptr);
  OGRFieldDefn fParentName("parent_name", OFTString);
  EXPECT_EQ(poParent->CreateField(&fParentName), OGRERR_NONE);

  // Create child table with compressed columns
  CPLStringList osChildLCO;
  osChildLCO.AddNameValue("COMPRESS_COLUMNS", "c_str,c_str2");
  OGRLayer* poChild =
      poDS->CreateLayer("child_tbl", nullptr, wkbPoint, osChildLCO.List());
  ASSERT_NE(poChild, nullptr);

  auto* poTableLayer = dynamic_cast<OGRSQLiteTableLayer*>(poChild);
  ASSERT_NE(poTableLayer, nullptr);

  OGRFieldDefn fParentId("parent_id", OFTInteger);
  EXPECT_EQ(poTableLayer->CreateField(&fParentId), OGRERR_NONE);

  OGRFieldDefn fCStr("c_str", OFTString);
  EXPECT_EQ(poTableLayer->CreateField(&fCStr), OGRERR_NONE);

  // Compressed columns must have width = 0 to avoid VARCHAR(50)_deflate syntax
  // error
  OGRFieldDefn fCStr2("c_str2", OFTString);
  EXPECT_EQ(poTableLayer->CreateField(&fCStr2), OGRERR_NONE);

  // Run deferred creation so subsequent field additions run in immediate mode
  EXPECT_EQ(poTableLayer->RunDeferredCreationIfNecessary(), OGRERR_NONE);

  // Add field in immediate mode with NOT NULL and without default (tests
  // DEFAULT '')
  OGRFieldDefn fNotNullNoDef("not_null_col", OFTString);
  fNotNullNoDef.SetNullable(0);
  EXPECT_EQ(poTableLayer->CreateField(&fNotNullNoDef), OGRERR_NONE);

  // Add field and geom field in immediate mode inside transaction
  poDS->StartTransaction();
  OGRFieldDefn fInTxn("in_txn_col", OFTInteger);
  EXPECT_EQ(poTableLayer->CreateField(&fInTxn), OGRERR_NONE);
  OGRGeomFieldDefn oGeomInTxn("second_geom", wkbLineString);
  EXPECT_EQ(poTableLayer->CreateGeomField(&oGeomInTxn), OGRERR_NONE);
  poDS->CommitTransaction();

  // Create an index so RecreateTable exercises saving and restoring indexes
  auto* poSQLiteDS = dynamic_cast<OGRSQLiteDataSource*>(poDS.get());
  ASSERT_NE(poSQLiteDS, nullptr);
  ASSERT_EQ(
      sqlite3_exec(poSQLiteDS->GetDB(),
                   "CREATE INDEX idx_child_parent ON child_tbl(parent_id)",
                   nullptr, nullptr, nullptr),
      SQLITE_OK);

  // Test AlterFieldDefn:
  // 1. Invalid index
  OGRFieldDefn fNewDefn("dummy", OFTString);
  EXPECT_EQ(poTableLayer->AlterFieldDefn(-1, &fNewDefn, ALTER_ALL_FLAG),
            OGRERR_FAILURE);
  EXPECT_EQ(poTableLayer->AlterFieldDefn(99, &fNewDefn, ALTER_ALL_FLAG),
            OGRERR_FAILURE);

  // 2. Duplicate name check
  int iCStr = poTableLayer->GetLayerDefn()->GetFieldIndex("c_str");
  ASSERT_GE(iCStr, 0);
  OGRFieldDefn fDup("parent_id", OFTString);
  EXPECT_EQ(poTableLayer->AlterFieldDefn(iCStr, &fDup, ALTER_NAME_FLAG),
            OGRERR_FAILURE);

  // 3. ALTER_NAME_FLAG only on c_str -> c_str_renamed (fast rename path)
  OGRFieldDefn fRenamed("c_str_renamed", OFTString);
  EXPECT_EQ(poTableLayer->AlterFieldDefn(iCStr, &fRenamed, ALTER_NAME_FLAG),
            OGRERR_NONE);
  EXPECT_STREQ(poTableLayer->GetLayerDefn()->GetFieldDefn(iCStr)->GetNameRef(),
               "c_str_renamed");

  // 4. ALTER_ALL_FLAG on c_str2 (keeping OFTString without width change ->
  // appends _deflate)
  int iCStr2 = poTableLayer->GetLayerDefn()->GetFieldIndex("c_str2");
  ASSERT_GE(iCStr2, 0);
  OGRFieldDefn fAlteredCStr2("c_str2_new", OFTString);
  fAlteredCStr2.SetNullable(0);
  fAlteredCStr2.SetUnique(TRUE);
  fAlteredCStr2.SetDefault("'def'");
  EXPECT_EQ(
      poTableLayer->AlterFieldDefn(iCStr2, &fAlteredCStr2, ALTER_ALL_FLAG),
      OGRERR_NONE);

  // 4b. ALTER_WIDTH_PRECISION_FLAG and ALTER_DEFAULT_FLAG inside transaction
  // (lines 2253-2269)
  int iNotNullForAlter =
      poTableLayer->GetLayerDefn()->GetFieldIndex("not_null_col");
  ASSERT_GE(iNotNullForAlter, 0);
  poDS->StartTransaction();
  OGRFieldDefn fWidth("not_null_col", OFTString);
  fWidth.SetWidth(50);
  fWidth.SetPrecision(2);
  EXPECT_EQ(poTableLayer->AlterFieldDefn(iNotNullForAlter, &fWidth,
                                         ALTER_WIDTH_PRECISION_FLAG),
            OGRERR_NONE);
  OGRFieldDefn fSameDef("not_null_col", OFTString);
  fSameDef.SetDefault("'def'");
  EXPECT_EQ(poTableLayer->AlterFieldDefn(iNotNullForAlter, &fSameDef,
                                         ALTER_DEFAULT_FLAG),
            OGRERR_NONE);
  // Second call where both old and new defaults are non-null and equal (lines
  // 2267-2269)
  OGRFieldDefn fSameDef2("not_null_col", OFTString);
  fSameDef2.SetDefault("'def'");
  fSameDef2.SetWidth(60);
  EXPECT_EQ(poTableLayer->AlterFieldDefn(
                iNotNullForAlter, &fSameDef2,
                ALTER_DEFAULT_FLAG | ALTER_WIDTH_PRECISION_FLAG),
            OGRERR_NONE);
  poDS->CommitTransaction();

  // 5. Change c_str_renamed from OFTString to OFTInteger (removes from
  // compressed columns)
  OGRFieldDefn fCStrToTypeInt("c_str_renamed", OFTInteger);
  EXPECT_EQ(
      poTableLayer->AlterFieldDefn(iCStr, &fCStrToTypeInt, ALTER_TYPE_FLAG),
      OGRERR_NONE);
  EXPECT_EQ(poTableLayer->GetLayerDefn()->GetFieldDefn(iCStr)->GetType(),
            OFTInteger);

  // Test DeleteField:
  EXPECT_EQ(poTableLayer->DeleteField(-1), OGRERR_FAILURE);
  EXPECT_EQ(poTableLayer->DeleteField(99), OGRERR_FAILURE);

  int iInTxn = poTableLayer->GetLayerDefn()->GetFieldIndex("in_txn_col");
  ASSERT_GE(iInTxn, 0);
  EXPECT_EQ(poTableLayer->DeleteField(iInTxn), OGRERR_NONE);

  // DeleteField inside transaction with foreign_keys check
  ASSERT_EQ(sqlite3_exec(poSQLiteDS->GetDB(), "PRAGMA foreign_keys = ON",
                         nullptr, nullptr, nullptr),
            SQLITE_OK);
  poDS->StartTransaction();
  int iNotNull = poTableLayer->GetLayerDefn()->GetFieldIndex("not_null_col");
  ASSERT_GE(iNotNull, 0);
  EXPECT_EQ(poTableLayer->DeleteField(iNotNull), OGRERR_NONE);
  poDS->CommitTransaction();

  // Test ReorderFields:
  {
    // Empty layer check
    OGRLayer* poEmpty =
        poDS->CreateLayer("empty_tbl", nullptr, wkbNone, nullptr);
    int panMapEmpty[] = {0};
    EXPECT_EQ(poEmpty->ReorderFields(panMapEmpty), OGRERR_NONE);

    // Invalid map check
    int panBadMap[] = {0, 0, 0};
    EXPECT_EQ(poTableLayer->ReorderFields(panBadMap), OGRERR_FAILURE);

    // Valid permutation
    int nFieldCount = poTableLayer->GetLayerDefn()->GetFieldCount();
    std::vector<int> panMap(nFieldCount);
    for (int i = 0; i < nFieldCount; ++i) {
      panMap[i] = nFieldCount - 1 - i;
    }
    EXPECT_EQ(poTableLayer->ReorderFields(panMap.data()), OGRERR_NONE);
  }

  // Test AddForeignKeysToTable:
  EXPECT_EQ(poTableLayer->AddForeignKeysToTable("INVALID FOREIGN KEY SYNTAX"),
            OGRERR_FAILURE);
  EXPECT_EQ(poTableLayer->AddForeignKeysToTable(
                "FOREIGN KEY(parent_id) REFERENCES parent_tbl(ogc_fid)"),
            OGRERR_NONE);

  // Create STRICT=YES table and verify parsing of STRICT and CHECK constraints
  // on reload
  {
    CPLStringList osStrictLCO;
    osStrictLCO.AddNameValue("STRICT", "YES");
    OGRLayer* poStrict =
        poDS->CreateLayer("strict_tbl", nullptr, wkbNone, osStrictLCO.List());
    ASSERT_NE(poStrict, nullptr);

    OGRFieldDefn sInt("s_int", OFTInteger);
    EXPECT_EQ(poStrict->CreateField(&sInt), OGRERR_NONE);
    OGRFieldDefn sInt64("s_int64", OFTInteger64);
    EXPECT_EQ(poStrict->CreateField(&sInt64), OGRERR_NONE);
    OGRFieldDefn sReal("s_real", OFTReal);
    EXPECT_EQ(poStrict->CreateField(&sReal), OGRERR_NONE);
    OGRFieldDefn sBlob("s_blob", OFTBinary);
    EXPECT_EQ(poStrict->CreateField(&sBlob), OGRERR_NONE);
    OGRFieldDefn sDT("s_dt", OFTDateTime);
    EXPECT_EQ(poStrict->CreateField(&sDT), OGRERR_NONE);
    OGRFieldDefn sDate("s_date", OFTDate);
    EXPECT_EQ(poStrict->CreateField(&sDate), OGRERR_NONE);
    OGRFieldDefn sTime("s_time", OFTTime);
    EXPECT_EQ(poStrict->CreateField(&sTime), OGRERR_NONE);
    OGRFieldDefn sStr("s_str", OFTString);
    EXPECT_EQ(poStrict->CreateField(&sStr), OGRERR_NONE);

    OGRFeature oF(poStrict->GetLayerDefn());
    oF.SetField("s_int", 123);
    oF.SetField("s_int64", static_cast<GIntBig>(456789));
    oF.SetField("s_real", 1.23);
    const GByte b = 0xAA;
    oF.SetField(oF.GetFieldIndex("s_blob"), 1, &b);
    oF.SetField("s_str", "text");
    EXPECT_EQ(poStrict->CreateFeature(&oF), OGRERR_NONE);

    // DateTime in STRICT mode has a CHECK constraint using LIKE '[0-9]...'.
    // In standard SQLite, LIKE checks literal '[' rather than regex, so
    // non-null values fail.
    {
      OGRFeature oFBad(poStrict->GetLayerDefn());
      oFBad.SetField("s_dt", 2026, 11, 3, 10, 20, 30, 0);
      CPLErrorReset();
      CPLPushErrorHandler(CPLQuietErrorHandler);
      EXPECT_EQ(poStrict->CreateFeature(&oFBad), OGRERR_FAILURE);
      CPLPopErrorHandler();
    }
  }

  // Close and reopen to trigger EstablishFeatureDefn on STRICT table
  poDS.reset();

  poDS.reset(
      GDALDataset::Open(dbPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE));
  ASSERT_NE(poDS, nullptr);
  OGRLayer* poStrictReload = poDS->GetLayerByName("strict_tbl");
  ASSERT_NE(poStrictReload, nullptr);
  EXPECT_EQ(poStrictReload->GetLayerDefn()->GetFieldDefn(0)->GetType(),
            OFTInteger);
  EXPECT_EQ(poStrictReload->GetLayerDefn()->GetFieldDefn(1)->GetType(),
            OFTInteger64);
  EXPECT_EQ(poStrictReload->GetLayerDefn()->GetFieldDefn(4)->GetType(),
            OFTDateTime);
  EXPECT_EQ(poStrictReload->GetLayerDefn()->GetFieldDefn(5)->GetType(),
            OFTDate);
  EXPECT_EQ(poStrictReload->GetLayerDefn()->GetFieldDefn(6)->GetType(),
            OFTTime);
}

TEST_F(OgrSqliteTableLayerTest,
       EstablishFeatureDefnSpecialTablesAndErrorPaths) {
  const std::string dbPath = std::string(kTestDir) + "/special.db";
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("SQLite");
  ASSERT_NE(poDriver, nullptr);

  CPLStringList osDSCO;
  osDSCO.AddNameValue("METADATA", "NO");

  {
    std::unique_ptr<GDALDataset> poDSInit(
        poDriver->Create(dbPath.c_str(), 0, 0, 0, GDT_Unknown, osDSCO.List()));
    ASSERT_NE(poDSInit, nullptr);
    auto* poSQLiteDSInit = dynamic_cast<OGRSQLiteDataSource*>(poDSInit.get());
    ASSERT_NE(poSQLiteDSInit, nullptr);
    sqlite3* hDB = poSQLiteDSInit->GetDB();

    // 1. Table WITHOUT ROWID
    ASSERT_EQ(
        sqlite3_exec(
            hDB,
            "CREATE TABLE without_rowid_tbl (id INT PRIMARY KEY, name TEXT) "
            "WITHOUT ROWID;"
            "INSERT INTO without_rowid_tbl VALUES (1, 'alice');",
            nullptr, nullptr, nullptr),
        SQLITE_OK);

    // 2. Table with 64-bit FID but no sqlite_sequence table
    ASSERT_EQ(
        sqlite3_exec(hDB,
                     "CREATE TABLE no_seq_fid64 (ogc_fid INTEGER PRIMARY KEY, "
                     "name TEXT);"
                     "INSERT INTO no_seq_fid64 VALUES (3000000000, 'big');",
                     nullptr, nullptr, nullptr),
        SQLITE_OK);

    // 3. Table with unquoted default strings and dates
    ASSERT_EQ(
        sqlite3_exec(
            hDB,
            "CREATE TABLE defaults_tbl ("
            "  ogc_fid INTEGER PRIMARY KEY,"
            "  s VARCHAR DEFAULT unquoted_str,"
            "  d DATE DEFAULT custom_date_fn,"
            "  dt TIMESTAMP DEFAULT custom_dt_fn,"
            "  n INT NOT NULL UNIQUE,"
            "  geom BLOB NOT NULL);"
            "CREATE TABLE geometry_columns ("
            "  f_table_name VARCHAR, f_geometry_column VARCHAR, "
            "  geometry_type INTEGER, coord_dimension INTEGER, srid INTEGER, "
            "  geometry_format VARCHAR);"
            "INSERT INTO geometry_columns VALUES ('defaults_tbl', 'geom', 1, "
            "2, 4326, 'WKB');"
            "INSERT INTO geometry_columns VALUES ('defaults_tbl', 'geom_null', "
            "NULL, NULL, NULL, 'WKB');",
            nullptr, nullptr, nullptr),
        SQLITE_OK);

    // 4. Empty definition table with default values for test ICreateFeature
    // DEFAULT VALUES
    ASSERT_EQ(sqlite3_exec(hDB,
                           "CREATE TABLE empty_def_tbl (ogc_fid INTEGER "
                           "PRIMARY KEY, val TEXT DEFAULT 'x');",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
  }

  CPLStringList osOpenOptions;
  osOpenOptions.AddNameValue("LIST_ALL_TABLES", "YES");

  std::unique_ptr<GDALDataset> poDS(
      GDALDataset::Open(dbPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
                        nullptr, osOpenOptions.List(), nullptr));
  ASSERT_NE(poDS, nullptr);

  auto* poSQLiteDS = dynamic_cast<OGRSQLiteDataSource*>(poDS.get());
  ASSERT_NE(poSQLiteDS, nullptr);

  // Test without_rowid_tbl
  {
    OGRLayer* poL = poDS->GetLayerByName("without_rowid_tbl");
    ASSERT_NE(poL, nullptr);
    EXPECT_EQ(poL->GetFeatureCount(TRUE), 1);
    std::unique_ptr<OGRFeature> poF(poL->GetFeature(0));
    ASSERT_NE(poF, nullptr);
    EXPECT_STREQ(poF->GetFieldAsString("name"), "alice");

    // SetFeature and DeleteFeature should fail because there is no FID col
    EXPECT_EQ(poL->SetFeature(poF.get()), OGRERR_FAILURE);
    EXPECT_EQ(poL->DeleteFeature(1), OGRERR_FAILURE);
  }

  // Test no_seq_fid64: OLMD_FID64 fallback via SELECT MAX(fid)
  {
    OGRLayer* poL = poDS->GetLayerByName("no_seq_fid64");
    ASSERT_NE(poL, nullptr);
    const char* pszFID64 = poL->GetMetadataItem(OLMD_FID64);
    ASSERT_NE(pszFID64, nullptr);
    EXPECT_STREQ(pszFID64, "YES");
  }

  // Test defaults_tbl
  {
    OGRLayer* poL = poDS->GetLayerByName("defaults_tbl");
    ASSERT_NE(poL, nullptr);
    const OGRFeatureDefn* poDefn = poL->GetLayerDefn();
    int idxS = poDefn->GetFieldIndex("s");
    int idxD = poDefn->GetFieldIndex("d");
    int idxDT = poDefn->GetFieldIndex("dt");
    int idxN = poDefn->GetFieldIndex("n");
    ASSERT_GE(idxS, 0);
    ASSERT_GE(idxD, 0);
    ASSERT_GE(idxDT, 0);
    ASSERT_GE(idxN, 0);

    EXPECT_STREQ(poDefn->GetFieldDefn(idxS)->GetDefault(), "'unquoted_str'");
    EXPECT_STREQ(poDefn->GetFieldDefn(idxD)->GetDefault(), "(custom_date_fn)");
    EXPECT_STREQ(poDefn->GetFieldDefn(idxDT)->GetDefault(), "(custom_dt_fn)");
    EXPECT_FALSE(poDefn->GetFieldDefn(idxN)->IsNullable());
    EXPECT_TRUE(poDefn->GetFieldDefn(idxN)->IsUnique());
    EXPECT_FALSE(poDefn->GetGeomFieldDefn(0)->IsNullable());
  }

  // Test empty_def_tbl: CreateFeature with DEFAULT VALUES and SetFeature with
  // no changes
  {
    OGRLayer* poL = poDS->GetLayerByName("empty_def_tbl");
    ASSERT_NE(poL, nullptr);
    OGRFeature oF(poL->GetLayerDefn());
    // Do not set any field
    EXPECT_EQ(poL->CreateFeature(&oF), OGRERR_NONE);
    EXPECT_EQ(oF.GetFID(), 1);

    // SetFeature with no fields set
    OGRFeature oFUpdate(poL->GetLayerDefn());
    oFUpdate.SetFID(1);
    EXPECT_EQ(poL->SetFeature(&oFUpdate), OGRERR_NONE);
  }

  // Direct tests on OGRSQLiteTableLayer
  {
    OGRSQLiteTableLayer tblLayer(poSQLiteDS);

    // Initialize with table(geom_col) syntax: successful case
    EXPECT_EQ(
        tblLayer.Initialize("defaults_tbl(geom)", true, false, false, true),
        CE_None);
    EXPECT_EQ(tblLayer.GetLayerDefn()->GetGeomFieldCount(), 1);

    // Initialize with table(geom_col) syntax: nonexistent geometry column
    OGRSQLiteTableLayer tblLayerBadGeom(poSQLiteDS);
    EXPECT_EQ(tblLayerBadGeom.Initialize("defaults_tbl(nonexistent_geom)", true,
                                         false, false, false),
              CE_Failure);

    // Initialize virtual shape table
    OGRSQLiteTableLayer tblLayerVS(poSQLiteDS);
    EXPECT_EQ(tblLayerVS.Initialize("defaults_tbl", false, true, false, true),
              CE_None);
    EXPECT_NE(tblLayerVS.GetLayerDefn(), nullptr);

    // Initialize nonexistent table: exercises HasLayerDefnError() branches
    OGRSQLiteTableLayer tblLayerError(poSQLiteDS);
    EXPECT_EQ(tblLayerError.Initialize("nonexistent_xyz_tbl", true, false,
                                       false, false),
              CE_None);
    EXPECT_TRUE(tblLayerError.HasLayerDefnError());

    EXPECT_EQ(tblLayerError.GetNextFeature(), nullptr);
    EXPECT_EQ(tblLayerError.GetFeature(1), nullptr);
    EXPECT_EQ(tblLayerError.GetFeatureCount(TRUE), 0);
    OGREnvelope sEnv;
    EXPECT_EQ(tblLayerError.GetExtent(&sEnv, TRUE), OGRERR_FAILURE);
    OGRFieldDefn fldDummy("dummy", OFTInteger);
    EXPECT_EQ(tblLayerError.CreateField(&fldDummy), OGRERR_FAILURE);
    EXPECT_EQ(tblLayerError.DeleteField(0), OGRERR_FAILURE);
    EXPECT_EQ(tblLayerError.AlterFieldDefn(0, &fldDummy, ALTER_ALL_FLAG),
              OGRERR_FAILURE);
    EXPECT_EQ(tblLayerError.AddForeignKeysToTable("dummy"), OGRERR_FAILURE);
    int panDummyMap[] = {0};
    EXPECT_EQ(tblLayerError.ReorderFields(panDummyMap), OGRERR_FAILURE);
    OGRFeature oDummyFeat(tblLayerError.GetLayerDefn());
    EXPECT_EQ(tblLayerError.SetFeature(&oDummyFeat), OGRERR_FAILURE);
    EXPECT_EQ(tblLayerError.CreateFeature(&oDummyFeat), OGRERR_FAILURE);
    EXPECT_EQ(tblLayerError.DeleteFeature(1), OGRERR_FAILURE);

    // Test direct layer creation with FGF and SpatiaLite formats,
    // UNINITIALIZED_SRID, strftime default
    {
      OGRSQLiteTableLayer fgfLayer(poSQLiteDS);
      EXPECT_EQ(fgfLayer.Initialize("fgf_direct", true, false, true, false),
                CE_None);
      fgfLayer.SetCreationParameters("OGC_FID", wkbPoint, "FGF", "GEOMETRY",
                                     nullptr, UNINITIALIZED_SRID);
      OGRFieldDefn fStrf("f_strf", OFTString);
      fStrf.SetDefault("( strftime('%s','now'))");
      EXPECT_EQ(fgfLayer.CreateField(&fStrf), OGRERR_NONE);
      OGRFieldDefn fInt("f_int", OFTInteger);
      EXPECT_EQ(fgfLayer.CreateField(&fInt), OGRERR_NONE);
      OGRFeature oFeat(fgfLayer.GetLayerDefn());
      OGRPoint pt(1.0, 2.0);
      oFeat.SetGeometry(&pt);
      oFeat.SetField("f_int", 1);
      EXPECT_EQ(fgfLayer.CreateFeature(&oFeat), OGRERR_NONE);
      oFeat.SetField("f_int", 2);
      EXPECT_EQ(fgfLayer.SetFeature(&oFeat), OGRERR_NONE);

      OGRSQLiteTableLayer splFormatLayer(poSQLiteDS);
      EXPECT_EQ(
          splFormatLayer.Initialize("spl_fmt_direct", true, false, true, false),
          CE_None);
      splFormatLayer.SetCreationParameters("OGC_FID", wkbPoint, "SpatiaLite",
                                           "GEOMETRY", nullptr, -1);

      OGRSQLiteTableLayer unnamedGeomLayer(poSQLiteDS);
      EXPECT_EQ(unnamedGeomLayer.Initialize("unnamed_geom_direct", true, false,
                                            true, false),
                CE_None);
      unnamedGeomLayer.SetCreationParameters("OGC_FID", wkbNone, nullptr, "",
                                             nullptr, -1);
      OGRGeomFieldDefn oEmptyNameGeom("", wkbPoint);
      EXPECT_EQ(unnamedGeomLayer.CreateGeomField(&oEmptyNameGeom), OGRERR_NONE);
      OGRFeature* poRaw = unnamedGeomLayer.GetNextRawFeature();
      delete poRaw;

      OGRSQLiteTableLayer defSpatialIdxLayer(poSQLiteDS);
      EXPECT_EQ(defSpatialIdxLayer.Initialize("def_spatial_idx", true, false,
                                              true, false),
                CE_None);
      defSpatialIdxLayer.SetCreationParameters("OGC_FID", wkbPoint, "WKB",
                                               "GEOMETRY", nullptr, -1);
      EXPECT_EQ(defSpatialIdxLayer.CreateSpatialIndex(0), FALSE);
    }

    // Pre-created table dup_def to test deferred creation failure branches
    sqlite3* hDB = poSQLiteDS->GetDB();
    sqlite3_exec(hDB, "CREATE TABLE dup_def (id INT);", nullptr, nullptr,
                 nullptr);
    {
      CPLPushErrorHandler(CPLQuietErrorHandler);
      OGRSQLiteTableLayer l1(poSQLiteDS);
      l1.Initialize("dup_def", true, false, true, false);
      l1.SetCreationParameters("OGC_FID", wkbNone, "WKB", "GEOMETRY", nullptr,
                               -1);
      EXPECT_EQ(l1.GetNextFeature(), nullptr);

      OGRSQLiteTableLayer l2(poSQLiteDS);
      l2.Initialize("dup_def", true, false, true, false);
      l2.SetCreationParameters("OGC_FID", wkbNone, "WKB", "GEOMETRY", nullptr,
                               -1);
      EXPECT_EQ(l2.GetFeature(1), nullptr);

      OGRSQLiteTableLayer l3(poSQLiteDS);
      l3.Initialize("dup_def", true, false, true, false);
      l3.SetCreationParameters("OGC_FID", wkbNone, "WKB", "GEOMETRY", nullptr,
                               -1);
      OGRFeature f3(l3.GetLayerDefn());
      f3.SetFID(1);
      EXPECT_EQ(l3.SetFeature(&f3), OGRERR_FAILURE);

      OGRSQLiteTableLayer l4(poSQLiteDS);
      l4.Initialize("dup_def", true, false, true, false);
      l4.SetCreationParameters("OGC_FID", wkbNone, "WKB", "GEOMETRY", nullptr,
                               -1);
      OGRFeature f4(l4.GetLayerDefn());
      EXPECT_EQ(l4.CreateFeature(&f4), OGRERR_FAILURE);

      OGRSQLiteTableLayer l5(poSQLiteDS);
      l5.Initialize("dup_def", true, false, true, false);
      l5.SetCreationParameters("OGC_FID", wkbNone, "WKB", "GEOMETRY", nullptr,
                               -1);
      EXPECT_EQ(l5.DeleteFeature(1), OGRERR_FAILURE);
      CPLPopErrorHandler();
    }

    // Dropped table failures
    {
      OGRLayer* poDropL =
          poDS->CreateLayer("dropped_tbl", nullptr, wkbNone, nullptr);
      ASSERT_NE(poDropL, nullptr);
      auto* poDropTable = dynamic_cast<OGRSQLiteTableLayer*>(poDropL);
      ASSERT_NE(poDropTable, nullptr);
      OGRFieldDefn f1("f1", OFTInteger);
      EXPECT_EQ(poDropTable->CreateField(&f1), OGRERR_NONE);
      OGRFieldDefn f2("f2", OFTString);
      EXPECT_EQ(poDropTable->CreateField(&f2), OGRERR_NONE);
      {
        OGRFeature feat(poDropTable->GetLayerDefn());
        feat.SetField(0, 10);
        EXPECT_EQ(poDropTable->CreateFeature(&feat), OGRERR_NONE);
      }
      sqlite3_exec(hDB, "DROP TABLE dropped_tbl", nullptr, nullptr, nullptr);

      CPLPushErrorHandler(CPLQuietErrorHandler);
      EXPECT_EQ(poDropTable->GetFeature(1), nullptr);
      EXPECT_EQ(poDropTable->DeleteFeature(1), OGRERR_FAILURE);
      EXPECT_EQ(poDropTable->DeleteField(0), OGRERR_FAILURE);
      {
        OGRFeature f(poDropTable->GetLayerDefn());
        f.SetField(0, 1);
        EXPECT_EQ(poDropTable->CreateFeature(&f), OGRERR_FAILURE);
      }
      {
        OGRFeature f(poDropTable->GetLayerDefn());
        f.SetFID(1);
        f.SetField(0, 1);
        EXPECT_EQ(poDropTable->SetFeature(&f), OGRERR_FAILURE);
      }
      {
        OGRFieldDefn fRen("f1_ren", OFTInteger);
        EXPECT_EQ(poDropTable->AlterFieldDefn(0, &fRen, ALTER_NAME_FLAG),
                  OGRERR_FAILURE);
      }
      {
        OGRFieldDefn fType("f1", OFTString);
        EXPECT_EQ(poDropTable->AlterFieldDefn(0, &fType, ALTER_TYPE_FLAG),
                  OGRERR_FAILURE);
      }
      {
        std::array<int, 2> map = {1, 0};
        EXPECT_EQ(poDropTable->ReorderFields(map.data()), OGRERR_FAILURE);
      }
      CPLPopErrorHandler();
    }

    // Custom SQLite step failure simulation
    static bool bFailStep = false;
    sqlite3_create_function(
        hDB, "fail_step_fn", 1, SQLITE_UTF8, nullptr,
        [](sqlite3_context* ctx, int, sqlite3_value**) {
          if (bFailStep) {
            sqlite3_result_error(ctx, "step failure simulation", -1);
          } else {
            sqlite3_result_int(ctx, 1);
          }
        },
        nullptr, nullptr);

    ASSERT_EQ(
        sqlite3_exec(
            hDB, "CREATE VIEW bad_step_view AS SELECT fail_step_fn(1) AS x;",
            nullptr, nullptr, nullptr),
        SQLITE_OK);
    {
      bFailStep = true;
      OGRSQLiteTableLayer viewErrLayer(poSQLiteDS);
      EXPECT_EQ(
          viewErrLayer.Initialize("bad_step_view", false, false, false, true),
          CE_None);
      CPLPushErrorHandler(CPLQuietErrorHandler);
      EXPECT_TRUE(viewErrLayer.HasLayerDefnError());
      CPLPopErrorHandler();
      bFailStep = false;
    }
  }

  // Close and reopen in Read-Only mode to test UNSUPPORTED_OP_READ_ONLY
  // branches
  poDS.reset();
  poDS.reset(GDALDataset::Open(dbPath.c_str(),
                               GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
                               osOpenOptions.List(), nullptr));
  ASSERT_NE(poDS, nullptr);

  OGRLayer* poROLayer = poDS->GetLayerByName("empty_def_tbl");
  ASSERT_NE(poROLayer, nullptr);

  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);
  OGRFieldDefn fldRO("ro", OFTInteger);
  EXPECT_EQ(poROLayer->CreateField(&fldRO), OGRERR_FAILURE);
  EXPECT_EQ(poROLayer->DeleteField(0), OGRERR_FAILURE);
  EXPECT_EQ(poROLayer->AlterFieldDefn(0, &fldRO, ALTER_ALL_FLAG),
            OGRERR_FAILURE);
  auto* poTableRO = dynamic_cast<OGRSQLiteTableLayer*>(poROLayer);
  ASSERT_NE(poTableRO, nullptr);
  EXPECT_EQ(poTableRO->AddForeignKeysToTable("dummy"), OGRERR_FAILURE);
  int panMapRO[] = {0};
  EXPECT_EQ(poROLayer->ReorderFields(panMapRO), OGRERR_FAILURE);
  OGRFeature oFeatRO(poROLayer->GetLayerDefn());
  oFeatRO.SetFID(1);
  EXPECT_EQ(poROLayer->SetFeature(&oFeatRO), OGRERR_FAILURE);
  EXPECT_EQ(poROLayer->CreateFeature(&oFeatRO), OGRERR_FAILURE);
  EXPECT_EQ(poROLayer->DeleteFeature(1), OGRERR_FAILURE);
  CPLPopErrorHandler();
}

TEST_F(OgrSqliteTableLayerTest,
       Spatialite4TriggersSpatialIndexExtentAndStatistics) {
  const std::string dbPath = std::string(kTestDir) + "/sp4.db";
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("SQLite");
  ASSERT_NE(poDriver, nullptr);

  // 1. Create SQLite DB and populate SpatiaLite 4 layout
  CPLStringList osDSCO;
  osDSCO.AddNameValue("METADATA", "NO");

  std::unique_ptr<GDALDataset> poDS(
      poDriver->Create(dbPath.c_str(), 0, 0, 0, GDT_Unknown, osDSCO.List()));
  ASSERT_NE(poDS, nullptr);
  auto* poSQLiteDS = dynamic_cast<OGRSQLiteDataSource*>(poDS.get());
  ASSERT_NE(poSQLiteDS, nullptr);
  sqlite3* hDB = poSQLiteDS->GetDB();
  ASSERT_NE(hDB, nullptr);

  // Register stub functions so SpatiaLite triggers, AddGeometryColumn, and
  // CreateSpatialIndex succeed
  sqlite3_create_function(
      hDB, "GeometryConstraints", 3, SQLITE_UTF8, nullptr,
      [](sqlite3_context* ctx, int, sqlite3_value**) {
        sqlite3_result_int(ctx, 1);
      },
      nullptr, nullptr);

  sqlite3_create_function(
      hDB, "AddGeometryColumn", -1, SQLITE_UTF8, hDB,
      [](sqlite3_context* ctx, int argc, sqlite3_value** argv) {
        if (argc >= 2) {
          sqlite3* db = static_cast<sqlite3*>(sqlite3_user_data(ctx));
          const char* tbl =
              reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
          const char* col =
              reinterpret_cast<const char*>(sqlite3_value_text(argv[1]));
          char* sql = sqlite3_mprintf(
              "ALTER TABLE '%q' ADD COLUMN '%q' BLOB",  // NOLINT
              tbl, col);
          sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
          sqlite3_free(sql);
        }
        sqlite3_result_int(ctx, 1);
      },
      nullptr, nullptr);

  sqlite3_create_function(
      hDB, "CreateSpatialIndex", -1, SQLITE_UTF8, nullptr,
      [](sqlite3_context* ctx, int argc, sqlite3_value** argv) {
        if (argc >= 1 && argv[0] != nullptr) {
          const char* tbl =
              reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
          if (tbl && strcmp(tbl, "fail_spatial_idx") == 0) {
            sqlite3_result_error(ctx, "failed to create spatial index", -1);
            return;
          }
        }
        sqlite3_result_int(ctx, 1);
      },
      nullptr, nullptr);

  ASSERT_EQ(
      sqlite3_exec(
          hDB,
          "CREATE TABLE spatial_ref_sys ("
          "  srid INTEGER UNIQUE, auth_name TEXT, auth_srid TEXT, ref_sys_name "
          "TEXT, proj4text TEXT, srtext TEXT);"
          "CREATE TABLE geometry_columns ("
          "  f_table_name VARCHAR, f_geometry_column VARCHAR, "
          "  geometry_type INTEGER, coord_dimension INTEGER, srid INTEGER, "
          "  spatial_index_enabled INTEGER);"
          "CREATE TABLE geometry_columns_time ("
          "  f_table_name TEXT, f_geometry_column TEXT, "
          "  last_insert TEXT, last_update TEXT, last_delete TEXT);"
          "CREATE TABLE geometry_columns_statistics ("
          "  f_table_name TEXT, f_geometry_column TEXT, last_verified TEXT, "
          "  row_count INTEGER, extent_min_x DOUBLE, extent_min_y DOUBLE, "
          "  extent_max_x DOUBLE, extent_max_y DOUBLE);"
          // sp4_pts table
          "CREATE TABLE sp4_pts (ogc_fid INTEGER PRIMARY KEY AUTOINCREMENT, "
          "  GEOMETRY BLOB, geom_z BLOB, geom_m BLOB, geom_zm BLOB);"
          "INSERT INTO geometry_columns VALUES ('sp4_pts', 'GEOMETRY', 1, 2, "
          "4326, 1);"
          "INSERT INTO geometry_columns VALUES ('sp4_pts', 'geom_z', 1001, 3, "
          "4326, 1);"
          "INSERT INTO geometry_columns VALUES ('sp4_pts', 'geom_m', 2001, 3, "
          "4326, 1);"
          "INSERT INTO geometry_columns VALUES ('sp4_pts', 'geom_zm', 3001, 4, "
          "4326, 1);"
          "CREATE TABLE idx_sp4_pts_GEOMETRY (pkid INTEGER PRIMARY KEY, "
          "  xmin DOUBLE, xmax DOUBLE, ymin DOUBLE, ymax DOUBLE);"
          "INSERT INTO idx_sp4_pts_GEOMETRY VALUES (1, 10.0, 10.0, 20.0, 20.0);"
          "INSERT INTO idx_sp4_pts_GEOMETRY VALUES (2, 30.0, 30.0, 40.0, 40.0);"
          "INSERT INTO geometry_columns_time VALUES ('sp4_pts', 'geometry', "
          "  '2026-01-01T00:00:00.000Z', '2026-01-01T00:00:00.000Z', "
          "'2026-01-01T00:00:00.000Z');"
          "INSERT INTO geometry_columns_time VALUES ('sp4_pts', 'geom_z', "
          "  '2026-01-01T00:00:00.000Z', '2026-01-01T00:00:00.000Z', "
          "'2026-01-01T00:00:00.000Z');"
          "INSERT INTO geometry_columns_time VALUES ('sp4_pts', 'geom_m', "
          "  '2026-01-01T00:00:00.000Z', '2026-01-01T00:00:00.000Z', "
          "'2026-01-01T00:00:00.000Z');"
          "INSERT INTO geometry_columns_statistics VALUES ('sp4_pts', "
          "'geometry', "
          "  '2099-01-02T00:00:00.000Z', 2, 10.0, 20.0, 30.0, 40.0);"
          "INSERT INTO geometry_columns_statistics VALUES ('sp4_pts', "
          "'geom_z', "
          "  '2099-01-02T00:00:00.000Z', 0, 10.0, 20.0, 30.0, 40.0);"
          "INSERT INTO geometry_columns_statistics VALUES ('sp4_pts', "
          "'geom_m', "
          "  '2025-01-01T00:00:00.000Z', 2, 10.0, 20.0, 30.0, 40.0);"
          // Triggers on sp4_pts
          "CREATE TRIGGER \"ggi_sp4_pts_GEOMETRY\" BEFORE INSERT ON "
          "\"sp4_pts\" "
          "FOR EACH ROW BEGIN "
          "SELECT RAISE(ROLLBACK, 'sp4_pts.GEOMETRY violates Geometry "
          "constraint [geom-type or SRID not allowed]') "
          "WHERE (SELECT geometry_type FROM geometry_columns WHERE "
          "Lower(f_table_name) = Lower('sp4_pts') AND "
          "Lower(f_geometry_column) = Lower('GEOMETRY') AND "
          "GeometryConstraints(NEW.\"GEOMETRY\", geometry_type, srid) = 1) IS "
          "NULL; "
          "END;"
          "CREATE TRIGGER \"tmi_sp4_pts_GEOMETRY\" AFTER INSERT ON \"sp4_pts\" "
          "FOR EACH ROW BEGIN "
          "UPDATE geometry_columns_time SET last_insert = "
          "strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
          "WHERE Lower(f_table_name) = Lower('sp4_pts') AND "
          "Lower(f_geometry_column) = Lower('GEOMETRY'); "
          "END;"
          "CREATE TRIGGER ggi_sp4_pts_GEOMETRY_custom BEFORE INSERT ON sp4_pts "
          "BEGIN SELECT 1; END;"
          // no_idx_pts table and triggers to test trigger dropping and checking
          "CREATE TABLE no_idx_pts (ogc_fid INTEGER PRIMARY KEY AUTOINCREMENT, "
          "GEOMETRY BLOB);"
          "INSERT INTO geometry_columns VALUES ('no_idx_pts', 'GEOMETRY', 1, "
          "2, 4326, 0);"
          "CREATE TRIGGER \"ggi_no_idx_pts_GEOMETRY\" BEFORE INSERT ON "
          "\"no_idx_pts\"\n"
          "FOR EACH ROW BEGIN "
          "SELECT RAISE(ROLLBACK, 'no_idx_pts.GEOMETRY violates Geometry "
          "constraint [geom-type or SRID not allowed]') "
          "WHERE (SELECT geometry_type FROM geometry_columns WHERE "
          "Lower(f_table_name) = Lower('no_idx_pts') AND "
          "Lower(f_geometry_column) = Lower('GEOMETRY') AND "
          "GeometryConstraints(NEW.\"GEOMETRY\", geometry_type, srid) = 1) IS "
          "NULL; "
          "END;"
          "CREATE TRIGGER \"tmi_no_idx_pts_GEOMETRY\" AFTER INSERT ON "
          "\"no_idx_pts\" "
          "FOR EACH ROW BEGIN "
          "UPDATE geometry_columns_time SET last_insert = "
          "strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
          "WHERE Lower(f_table_name) = Lower('no_idx_pts') AND "
          "Lower(f_geometry_column) = Lower('GEOMETRY'); "
          "END;"
          "CREATE TRIGGER ggi_no_idx_pts_GEOMETRY_custom BEFORE INSERT ON "
          "no_idx_pts BEGIN SELECT 1; END;",
          nullptr, nullptr, nullptr),
      SQLITE_OK);

  // Reload layers while in read-only to allow SpatiaLite detection without
  // libspatialite
  poSQLiteDS->DisableUpdate();
  poSQLiteDS->ReloadLayers();
  ExposedGDALDataset::SetUpdate(poDS.get());

  EXPECT_TRUE(poSQLiteDS->IsSpatialiteDB());
  EXPECT_TRUE(poSQLiteDS->HasSpatialite4Layout());

  OGRLayer* poLayer = poDS->GetLayerByName("sp4_pts");
  ASSERT_NE(poLayer, nullptr);
  auto* poTableLayer = dynamic_cast<OGRSQLiteTableLayer*>(poLayer);
  ASSERT_NE(poTableLayer, nullptr);
  ASSERT_NE(poTableLayer->GetLayerDefn(), nullptr);

  // CreateGeomField with wkbCircularString (> wkbGeometryCollection) fails in
  // SpatiaLite
  {
    CPLPushErrorHandler(CPLQuietErrorHandler);
    OGRGeomFieldDefn oCurvGeom("curve", wkbCircularString);
    EXPECT_EQ(poTableLayer->CreateGeomField(&oCurvGeom), OGRERR_FAILURE);
    CPLPopErrorHandler();
  }

  // CreateGeomField with various types on SpatiaLite (lines 1610-1628,
  // 1758-1793)
  {
    CPLPushErrorHandler(CPLQuietErrorHandler);
    OGRGeomFieldDefn gNone("g_none", wkbNone);
    EXPECT_EQ(poTableLayer->CreateGeomField(&gNone), OGRERR_FAILURE);
    CPLPopErrorHandler();
    OGRGeomFieldDefn gUnk("g_unk", wkbUnknown);
    EXPECT_EQ(poTableLayer->CreateGeomField(&gUnk), OGRERR_NONE);
    OGRGeomFieldDefn gM("g_m", wkbPointM);
    EXPECT_EQ(poTableLayer->CreateGeomField(&gM), OGRERR_NONE);
    OGRGeomFieldDefn gZM("g_zm", wkbPointZM);
    EXPECT_EQ(poTableLayer->CreateGeomField(&gZM), OGRERR_NONE);
    OGRGeomFieldDefn gZ("g_z", wkbPoint25D);
    gZ.SetNullable(0);
    EXPECT_EQ(poTableLayer->CreateGeomField(&gZ), OGRERR_NONE);
  }

  // Insert valid features
  {
    OGRFeature oF1(poTableLayer->GetLayerDefn());
    OGRPoint pt1(10.0, 20.0);
    oF1.SetGeomField(0, &pt1);
    OGRPoint ptZ(1.0, 2.0, 3.0);
    oF1.SetGeomField(1, &ptZ);
    OGRPoint ptM(4.0, 5.0, 0.0, 6.0);
    oF1.SetGeomField(2, &ptM);
    OGRPoint ptZM(7.0, 8.0, 9.0, 10.0);
    oF1.SetGeomField(3, &ptZM);
    EXPECT_EQ(poTableLayer->CreateFeature(&oF1), OGRERR_NONE);

    OGRFeature oF2(poTableLayer->GetLayerDefn());
    OGRPoint pt2(30.0, 40.0);
    oF2.SetGeomField(0, &pt2);
    EXPECT_EQ(poTableLayer->CreateFeature(&oF2), OGRERR_NONE);
  }

  // Test Spatial capabilities & Spatial filters
  EXPECT_TRUE(poTableLayer->HasSpatialIndex(0));
  EXPECT_TRUE(poTableLayer->HasFastSpatialFilter(0));
  EXPECT_TRUE(poTableLayer->TestCapability(OLCFastFeatureCount));
  EXPECT_TRUE(poTableLayer->TestCapability(OLCFastSpatialFilter));
  EXPECT_TRUE(poTableLayer->TestCapability(OLCRandomRead));
  EXPECT_TRUE(poTableLayer->TestCapability(OLCSequentialWrite));
  EXPECT_TRUE(poTableLayer->TestCapability(OLCRandomWrite));
  EXPECT_TRUE(poTableLayer->TestCapability(OLCDeleteFeature));
  EXPECT_TRUE(poTableLayer->TestCapability(OLCCreateField));
  EXPECT_TRUE(poTableLayer->TestCapability(OLCCreateGeomField));
  EXPECT_TRUE(poTableLayer->TestCapability(OLCDeleteField));
  EXPECT_TRUE(poTableLayer->TestCapability(OLCAlterFieldDefn));
  EXPECT_TRUE(poTableLayer->TestCapability(OLCReorderFields));
  EXPECT_TRUE(poTableLayer->TestCapability(OLCZGeometries));

  // Test IGetExtent (from RTree idx table)
  OGREnvelope sExtent;
  EXPECT_EQ(poTableLayer->GetExtent(0, &sExtent, TRUE), OGRERR_NONE);
  EXPECT_DOUBLE_EQ(sExtent.MinX, 10.0);
  EXPECT_DOUBLE_EQ(sExtent.MaxX, 30.0);
  EXPECT_DOUBLE_EQ(sExtent.MinY, 20.0);
  EXPECT_DOUBLE_EQ(sExtent.MaxY, 40.0);
  EXPECT_TRUE(poTableLayer->TestCapability(OLCFastGetExtent));

  // Test OGR_SQLITE_EXACT_EXTENT=YES fallback
  {
    CPLSetConfigOption("OGR_SQLITE_EXACT_EXTENT", "YES");
    poTableLayer->InvalidateCachedFeatureCountAndExtent();
    OGREnvelope sExactExtent;
    EXPECT_EQ(poTableLayer->GetExtent(0, &sExactExtent, TRUE), OGRERR_NONE);
    EXPECT_DOUBLE_EQ(sExactExtent.MinX, 10.0);
    EXPECT_DOUBLE_EQ(sExactExtent.MaxX, 30.0);
    CPLSetConfigOption("OGR_SQLITE_EXACT_EXTENT", nullptr);
  }

  // Test GetFeatureCount with spatial filter using RTree count
  {
    OGRPolygon poly;
    OGRLinearRing ring;
    ring.addPoint(5, 15);
    ring.addPoint(5, 25);
    ring.addPoint(15, 25);
    ring.addPoint(15, 15);
    ring.addPoint(5, 15);
    poly.addRing(&ring);

    poTableLayer->SetSpatialFilter(0, &poly);
    EXPECT_EQ(poTableLayer->GetFeatureCount(TRUE), 1);

    // Attribute query combined with spatial filter
    poTableLayer->SetAttributeFilter("ogc_fid = 1");
    EXPECT_EQ(poTableLayer->GetFeatureCount(TRUE), 1);
    poTableLayer->SetAttributeFilter("ogc_fid = 999");
    EXPECT_EQ(poTableLayer->GetFeatureCount(TRUE), 0);

    poTableLayer->SetAttributeFilter(nullptr);
    poTableLayer->SetSpatialFilter(nullptr);
    EXPECT_EQ(poTableLayer->GetFeatureCount(TRUE), 2);
  }

  // Test no_idx_pts table without spatial index: trigger dropping, type checks,
  // and CreateSpatialIndex
  {
    OGRLayer* poNoIdxLayer = poDS->GetLayerByName("no_idx_pts");
    ASSERT_NE(poNoIdxLayer, nullptr);
    auto* poNoIdxTable = dynamic_cast<OGRSQLiteTableLayer*>(poNoIdxLayer);
    ASSERT_NE(poNoIdxTable, nullptr);

    // Test inserting wrong geometry type (catches trigger check in lines
    // 3198-3215)
    {
      OGRFeature oBadF(poNoIdxTable->GetLayerDefn());
      OGRLineString line;
      line.addPoint(0, 0);
      line.addPoint(1, 1);
      oBadF.SetGeomField(0, &line);
      CPLErrorReset();
      CPLPushErrorHandler(CPLQuietErrorHandler);
      EXPECT_EQ(poNoIdxTable->CreateFeature(&oBadF), OGRERR_FAILURE);
      CPLPopErrorHandler();
    }

    // Insert valid feature (triggers are dropped and restored on layer
    // destruction)
    OGRFeature oF(poNoIdxTable->GetLayerDefn());
    OGRPoint pt(5.0, 5.0);
    oF.SetGeomField(0, &pt);
    EXPECT_EQ(poNoIdxTable->CreateFeature(&oF), OGRERR_NONE);

    // Test CreateSpatialIndex (lines 3523-3555)
    EXPECT_EQ(poNoIdxTable->CreateSpatialIndex(-1), FALSE);
    poNoIdxTable->SetDeferredSpatialIndexCreation(true);
    poNoIdxTable->CreateSpatialIndexIfNecessary();
    EXPECT_TRUE(poNoIdxTable->HasSpatialIndex(0));
    EXPECT_FALSE(poNoIdxTable->HasFastSpatialFilter(0));
  }

  // Test Statistics methods: SaveStatistics, LoadStatisticsSpatialite4DB
  // Call LoadStatisticsSpatialite4DB directly via private accessor (covers
  // lines 3756-3880)
  (poTableLayer->*GetPrivate(LoadStatsSp4Tag{}))();
  // geom_z has row_count = 0, which sets m_nFeatureCount = -1 (lines
  // 3844-3847), so AreStatisticsValid is false
  EXPECT_FALSE(poTableLayer->AreStatisticsValid());

  // Cached extent return (lines 1292-1293)
  {
    OGREnvelope sCachedExt;
    EXPECT_EQ(poTableLayer->GetExtent(0, &sCachedExt, FALSE), OGRERR_NONE);
    EXPECT_DOUBLE_EQ(sCachedExt.MinX, 10.0);
    EXPECT_DOUBLE_EQ(sCachedExt.MaxX, 30.0);
  }

  // HasSpatialIndex(-1) and CheckSpatialIndexTable(-1) via ISetSpatialFilter
  EXPECT_FALSE(poTableLayer->HasSpatialIndex(-1));
  {
    OGRPoint oPt(10, 20);
    EXPECT_EQ(poTableLayer->ISetSpatialFilter(-1, &oPt), OGRERR_NONE);
    EXPECT_EQ(poTableLayer->GetFeatureCount(TRUE), 2);
    EXPECT_EQ(poTableLayer->SetSpatialFilter(nullptr), OGRERR_NONE);
  }

  // Test CreateSpatialIndex failure via SQLite error (lines 3547-3550)
  {
    CPLPushErrorHandler(CPLQuietErrorHandler);
    OGRSQLiteTableLayer failIdxLayer(poSQLiteDS);
    failIdxLayer.Initialize("fail_spatial_idx", true, false, true, false);
    failIdxLayer.SetCreationParameters("OGC_FID", wkbPoint, "SpatiaLite",
                                       "GEOMETRY", nullptr, -1);
    EXPECT_EQ(failIdxLayer.CreateSpatialIndex(0), FALSE);
    CPLPopErrorHandler();
  }

  // Test SaveStatistics (returns -1 when !IsSpatialiteLoaded() or
  // GeomFieldCount != 1)
  poTableLayer->ForceStatisticsToBeFlushed();
  EXPECT_FALSE(poTableLayer->DoStatisticsNeedToBeFlushed());
  EXPECT_EQ(poTableLayer->SaveStatistics(), -1);

  // Invalidate cached feature count and save statistics
  poTableLayer->InvalidateCachedFeatureCountAndExtent();
  EXPECT_FALSE(poTableLayer->AreStatisticsValid());
  poTableLayer->ForceStatisticsToBeFlushed();
  EXPECT_EQ(poTableLayer->SaveStatistics(), -1);

  // InitFeatureCount and ForceStatisticsToBeFlushed
  poTableLayer->InitFeatureCount();
  EXPECT_TRUE(poTableLayer->AreStatisticsValid());

  // Test field laundering and ROWID warning in SpatiaLite.
  // `poTableLayer` is out of deferred creation mode, so `CreateField` executes
  // an immediate `ALTER TABLE`. SQLite 3.54.0+ rejects `ADD COLUMN 'rowid'` on
  // rowid tables ("cannot add rowid alias: rowid") after GDAL emits the
  // SpatiaLite warning, overwriting `CPLGetLastErrorMsg()`. Keep this block at
  // the end of the test because `poTableLayer`'s schema state diverges across
  // SQLite < 3.54 and >= 3.54.
  {
    CPLErrorReset();
    std::vector<std::string> error_msgs;
    {
      CPLErrorHandlerPusher error_pusher(RecordErrorMessage, &error_msgs);
      OGRFieldDefn rowid_field("ROWID", OFTString);
      if (sqlite3_libversion_number() >= 3054000) {
        EXPECT_EQ(OGRERR_FAILURE, poTableLayer->CreateField(&rowid_field));
        EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
        EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("cannot add rowid alias"));
      } else {
        EXPECT_EQ(OGRERR_NONE, poTableLayer->CreateField(&rowid_field));
        EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
      }
    }
    EXPECT_THAT(error_msgs, Contains(HasSubstr("can corrupt spatial index")));
    CPLErrorReset();
  }
}

TEST_F(OgrSqliteTableLayerTest, Spatialite3LegacyLayoutAndStatistics) {
  const std::string dbPath = std::string(kTestDir) + "/sp3.db";
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("SQLite");
  ASSERT_NE(poDriver, nullptr);

  CPLStringList osDSCO;
  osDSCO.AddNameValue("METADATA", "NO");

  {
    std::unique_ptr<GDALDataset> poDSInit(
        poDriver->Create(dbPath.c_str(), 0, 0, 0, GDT_Unknown, osDSCO.List()));
    ASSERT_NE(poDSInit, nullptr);
    auto* poSQLiteDSInit = dynamic_cast<OGRSQLiteDataSource*>(poDSInit.get());
    ASSERT_NE(poSQLiteDSInit, nullptr);
    sqlite3* hDB = poSQLiteDSInit->GetDB();

    ASSERT_EQ(
        sqlite3_exec(
            hDB,
            "CREATE TABLE spatial_ref_sys ("
            "  srid INTEGER UNIQUE, auth_name TEXT, auth_srid TEXT, "
            "ref_sys_name TEXT, proj4text TEXT, srtext TEXT);"
            "CREATE TABLE geometry_columns ("
            "  f_table_name VARCHAR, f_geometry_column VARCHAR, "
            "  type VARCHAR, coord_dimension VARCHAR, srid INTEGER, "
            "  spatial_index_enabled INTEGER);"
            "CREATE TABLE sp3_xy (ogc_fid INTEGER PRIMARY KEY, geom BLOB);"
            "CREATE TABLE sp3_xyz (ogc_fid INTEGER PRIMARY KEY, geom BLOB);"
            "CREATE TABLE sp3_xym (ogc_fid INTEGER PRIMARY KEY, geom BLOB);"
            "CREATE TABLE sp3_xyzm (ogc_fid INTEGER PRIMARY KEY, geom BLOB);"
            "INSERT INTO geometry_columns VALUES ('sp3_xy', 'geom', 'POINT', "
            "'XY', 4326, 0);"
            "INSERT INTO geometry_columns VALUES ('sp3_xyz', 'geom', 'POINT', "
            "'XYZ', 4326, 0);"
            "INSERT INTO geometry_columns VALUES ('sp3_xym', 'geom', 'POINT', "
            "'XYM', 4326, 0);"
            "INSERT INTO geometry_columns VALUES ('sp3_xyzm', 'geom', 'POINT', "
            "'XYZM', 4326, 0);"
            "CREATE TABLE spatialite_history ("
            "  table_name TEXT, geometry_column TEXT, event TEXT, timestamp "
            "TEXT);"
            "CREATE TABLE layer_statistics ("
            "  table_name TEXT, geometry_column TEXT, raster_layer INTEGER, "
            "  row_count INTEGER, extent_min_x DOUBLE, extent_min_y DOUBLE, "
            "  extent_max_x DOUBLE, extent_max_y DOUBLE);",
            nullptr, nullptr, nullptr),
        SQLITE_OK);
  }

  // Open dataset in read-only mode, then enable update mode so SpatiaLite 3
  // layout is active
  std::unique_ptr<GDALDataset> poDS(
      GDALDataset::Open(dbPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(poDS, nullptr);
  ExposedGDALDataset::SetUpdate(poDS.get());

  auto* poSQLiteDS = dynamic_cast<OGRSQLiteDataSource*>(poDS.get());
  ASSERT_NE(poSQLiteDS, nullptr);
  EXPECT_TRUE(poSQLiteDS->IsSpatialiteDB());
  EXPECT_FALSE(poSQLiteDS->HasSpatialite4Layout());

  // Check that all 4 SpatiaLite 3 dimension types were established
  OGRLayer* poXY = poDS->GetLayerByName("sp3_xy");
  ASSERT_NE(poXY, nullptr);
  EXPECT_EQ(poXY->GetGeomType(), wkbPoint);

  OGRLayer* poXYZ = poDS->GetLayerByName("sp3_xyz");
  ASSERT_NE(poXYZ, nullptr);
  EXPECT_EQ(poXYZ->GetGeomType(), wkbPoint25D);

  OGRLayer* poXYM = poDS->GetLayerByName("sp3_xym");
  ASSERT_NE(poXYM, nullptr);
  EXPECT_EQ(poXYM->GetGeomType(), wkbPointM);

  OGRLayer* poXYZM = poDS->GetLayerByName("sp3_xyzm");
  ASSERT_NE(poXYZM, nullptr);
  EXPECT_EQ(poXYZM->GetGeomType(), wkbPointZM);
}

// TODO: b/335317901 - Cover OGRSQLiteTableLayer spatialite trigger creation and
// update paths if spatialite is loaded.

}  // namespace
}  // namespace autotest2
