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
// Tests Open FileGDB layer writing implementation
// (ogropenfilegdblayer_write.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/ogropenfilegdblayer_write.cpp
//   https://gdal.org/drivers/vector/openfilegdb.html

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
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
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/filegdbtable.h"
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

constexpr const char* kTestDir = "/vsimem/ogropenfilegdblayer_write_test";

inline CPLErr CloseDS(GDALDataset& ds) { return ds.Close(); }

inline CPLErr CloseDS(GDALDataset* ds) {
  return ds != nullptr ? ds->Close() : CE_None;
}

class OgrOpenFileGdbLayerWriteTest : public ::testing::Test {
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

std::unique_ptr<GDALDataset> CreateGDB(const std::string& path) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
  if (!poDriver) return nullptr;
  GDALDataset* poDS =
      poDriver->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
  return std::unique_ptr<GDALDataset>(poDS);
}

// -----------------------------------------------------------------------------
// Test 1: Layer Creation and Laundering
// -----------------------------------------------------------------------------
TEST_F(OgrOpenFileGdbLayerWriteTest, LayerCreationAndLaundering) {
  const std::string gdb_path = std::string(kTestDir) + "/layer_create.gdb";
  auto ds = CreateGDB(gdb_path);
  ASSERT_NE(ds, nullptr);

  // 1. Layer name starting with a digit: laundered to _123layer
  {
    OGRLayer* lyr = ds->CreateLayer("123layer", nullptr, wkbPoint, nullptr);
    ASSERT_NE(lyr, nullptr);
    EXPECT_STREQ(lyr->GetName(), "_123layer");
  }

  // 2. Layer name with special characters
  {
    OGRLayer* lyr = ds->CreateLayer("a-b!c", nullptr, wkbPoint, nullptr);
    ASSERT_NE(lyr, nullptr);
    EXPECT_STREQ(lyr->GetName(), "a_b_c");
  }

  // 3. Reserved words
  {
    OGRLayer* lyr = ds->CreateLayer("SELECT", nullptr, wkbPoint, nullptr);
    ASSERT_NE(lyr, nullptr);
    EXPECT_STREQ(lyr->GetName(), "SELECT_");
  }
  {
    OGRLayer* lyr = ds->CreateLayer("TABLE", nullptr, wkbPoint, nullptr);
    ASSERT_NE(lyr, nullptr);
    EXPECT_STREQ(lyr->GetName(), "TABLE_");
  }

  // 4. Unsupported prefixes: sde_, gdb_, delta_
  {
    OGRLayer* lyr = ds->CreateLayer("sde_test", nullptr, wkbPoint, nullptr);
    ASSERT_NE(lyr, nullptr);
    EXPECT_STREQ(lyr->GetName(), "_sde_test");
  }
  {
    OGRLayer* lyr = ds->CreateLayer("gdb_test", nullptr, wkbPoint, nullptr);
    ASSERT_NE(lyr, nullptr);
    EXPECT_STREQ(lyr->GetName(), "_gdb_test");
  }
  {
    OGRLayer* lyr = ds->CreateLayer("delta_test", nullptr, wkbPoint, nullptr);
    ASSERT_NE(lyr, nullptr);
    EXPECT_STREQ(lyr->GetName(), "_delta_test");
  }

  // 5. Truncate name > 160 characters
  {
    std::string long_name(200, 'x');
    OGRLayer* lyr =
        ds->CreateLayer(long_name.c_str(), nullptr, wkbPoint, nullptr);
    ASSERT_NE(lyr, nullptr);
    EXPECT_EQ(strlen(lyr->GetName()), 160);
  }

  // 6. Duplicate layer names (up to > 10 duplicates to test _1.._9 and
  // _10.._99)
  for (int i = 0; i < 12; ++i) {
    OGRLayer* lyr = ds->CreateLayer("dup_layer", nullptr, wkbPoint, nullptr);
    ASSERT_NE(lyr, nullptr);
    EXPECT_THAT(lyr->GetName(), testing::StartsWith("dup_layer"));
  }

  // 7. Layer creation options validation:
  // Invalid TARGET_ARCGIS_VERSION
  {
    const char* options[] = {"TARGET_ARCGIS_VERSION=INVALID_VERSION", nullptr};
    CPLErrorReset();
    CPLPushErrorHandler(CPLQuietErrorHandler);
    OGRLayer* lyr = ds->CreateLayer("bad_ver", nullptr, wkbPoint, options);
    CPLPopErrorHandler();
    EXPECT_NE(lyr, nullptr);
  }

  // Valid TARGET_ARCGIS_VERSION=ARCGIS_PRO_3_2_OR_LATER
  {
    const char* options[] = {"TARGET_ARCGIS_VERSION=ARCGIS_PRO_3_2_OR_LATER",
                             nullptr};
    OGRLayer* lyr = ds->CreateLayer("v32_layer", nullptr, wkbPoint, options);
    ASSERT_NE(lyr, nullptr);
  }

  // CONFIGURATION_KEYWORD options:
  {
    const char* options[] = {"CONFIGURATION_KEYWORD=MAX_FILE_SIZE_4GB",
                             nullptr};
    EXPECT_NE(ds->CreateLayer("kw_4gb", nullptr, wkbPoint, options), nullptr);
  }
  {
    const char* options[] = {"CONFIGURATION_KEYWORD=MAX_FILE_SIZE_256TB",
                             nullptr};
    EXPECT_NE(ds->CreateLayer("kw_256tb", nullptr, wkbPoint, options), nullptr);
  }
  {
    const char* options[] = {"CONFIGURATION_KEYWORD=TEXT_UTF16", nullptr};
    EXPECT_NE(ds->CreateLayer("kw_utf16", nullptr, wkbPoint, options), nullptr);
  }
  {
    const char* options[] = {"CONFIGURATION_KEYWORD=DEFAULTS", nullptr};
    EXPECT_NE(ds->CreateLayer("kw_def", nullptr, wkbPoint, options), nullptr);
  }
  {
    const char* options[] = {"CONFIGURATION_KEYWORD=NON_EXISTENT_KW", nullptr};
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(ds->CreateLayer("kw_invalid", nullptr, wkbPoint, options),
              nullptr);
    CPLPopErrorHandler();
  }

  // DOCUMENTATION, LAYER_ALIAS, TIME_IN_UTC, FID, GEOMETRY_NAME,
  // GEOMETRY_NULLABLE
  {
    const char* options[] = {"DOCUMENTATION=<doc>My Documentation</doc>",
                             "LAYER_ALIAS=My Layer Alias",
                             "TIME_IN_UTC=YES",
                             "FID=MY_FID",
                             "GEOMETRY_NAME=MY_SHAPE",
                             "GEOMETRY_NULLABLE=NO",
                             "CREATE_SHAPE_AREA_AND_LENGTH_FIELDS=YES",
                             "AREA_FIELD_NAME=MY_AREA",
                             "LENGTH_FIELD_NAME=MY_LENGTH",
                             "XORIGIN=-1000",
                             "YORIGIN=-2000",
                             "ZORIGIN=-500",
                             "MORIGIN=-100",
                             "XYSCALE=10000",
                             "ZSCALE=100",
                             "MSCALE=10",
                             "XYTOLERANCE=0.001",
                             "ZTOLERANCE=0.01",
                             "MTOLERANCE=0.1",
                             nullptr};
    OGRLayer* lyr =
        ds->CreateLayer("custom_opts", nullptr, wkbPolygon25D, options);
    ASSERT_NE(lyr, nullptr);
    EXPECT_STREQ(lyr->GetFIDColumn(), "MY_FID");
    EXPECT_STREQ(lyr->GetGeometryColumn(), "MY_SHAPE");
  }

  // OPENFILEGDB_CREATE_FIELD_BEFORE_GEOMETRY
  {
    CPLSetConfigOption("OPENFILEGDB_CREATE_FIELD_BEFORE_GEOMETRY", "YES");
    absl::Cleanup cleanup = [] {
      CPLSetConfigOption("OPENFILEGDB_CREATE_FIELD_BEFORE_GEOMETRY", nullptr);
    };
    OGRLayer* lyr =
        ds->CreateLayer("field_before_geom", nullptr, wkbPoint, nullptr);
    ASSERT_NE(lyr, nullptr);
  }

  // Unsupported geometry type: wkbLinearRing
  {
    CPLPushErrorHandler(CPLQuietErrorHandler);
    OGRLayer* lyr =
        ds->CreateLayer("bad_geom", nullptr, wkbLinearRing, nullptr);
    CPLPopErrorHandler();
    EXPECT_EQ(lyr, nullptr);
  }

  // All supported geometry types:
  EXPECT_NE(ds->CreateLayer("geom_none", nullptr, wkbNone, nullptr), nullptr);
  EXPECT_NE(ds->CreateLayer("geom_pt", nullptr, wkbPoint, nullptr), nullptr);
  EXPECT_NE(ds->CreateLayer("geom_pt25d", nullptr, wkbPoint25D, nullptr),
            nullptr);
  EXPECT_NE(ds->CreateLayer("geom_ptm", nullptr, wkbPointM, nullptr), nullptr);
  EXPECT_NE(ds->CreateLayer("geom_ptzm", nullptr, wkbPointZM, nullptr),
            nullptr);
  EXPECT_NE(ds->CreateLayer("geom_mpt", nullptr, wkbMultiPoint, nullptr),
            nullptr);
  EXPECT_NE(ds->CreateLayer("geom_line", nullptr, wkbLineString, nullptr),
            nullptr);
  EXPECT_NE(ds->CreateLayer("geom_mline", nullptr, wkbMultiLineString, nullptr),
            nullptr);
  EXPECT_NE(ds->CreateLayer("geom_circ", nullptr, wkbCircularString, nullptr),
            nullptr);
  EXPECT_NE(ds->CreateLayer("geom_comp", nullptr, wkbCompoundCurve, nullptr),
            nullptr);
  EXPECT_NE(ds->CreateLayer("geom_mcurve", nullptr, wkbMultiCurve, nullptr),
            nullptr);
  EXPECT_NE(ds->CreateLayer("geom_poly", nullptr, wkbPolygon, nullptr),
            nullptr);
  EXPECT_NE(ds->CreateLayer("geom_mpoly", nullptr, wkbMultiPolygon, nullptr),
            nullptr);
  EXPECT_NE(ds->CreateLayer("geom_cpoly", nullptr, wkbCurvePolygon, nullptr),
            nullptr);
  EXPECT_NE(ds->CreateLayer("geom_msurf", nullptr, wkbMultiSurface, nullptr),
            nullptr);
  EXPECT_NE(ds->CreateLayer("geom_tin", nullptr, wkbTIN, nullptr), nullptr);
  EXPECT_NE(
      ds->CreateLayer("geom_polyhed", nullptr, wkbPolyhedralSurface, nullptr),
      nullptr);
  EXPECT_NE(
      ds->CreateLayer("geom_gc25d", nullptr, wkbGeometryCollection25D, nullptr),
      nullptr);
  EXPECT_NE(
      ds->CreateLayer("geom_unk25d", nullptr, wkbSetZ(wkbUnknown), nullptr),
      nullptr);

  // CREATE_MULTIPATCH=YES
  {
    const char* options[] = {"CREATE_MULTIPATCH=YES", nullptr};
    EXPECT_NE(ds->CreateLayer("geom_mpatch", nullptr, wkbMultiPolygon, options),
              nullptr);
  }

  // FEATURE_DATASET creation and reuse
  OGRSpatialReference srs4326;
  srs4326.importFromEPSG(4326);
  {
    const char* options[] = {"FEATURE_DATASET=MyDataset", nullptr};
    OGRLayer* lyr1 = ds->CreateLayer("fd_lyr1", &srs4326, wkbPoint, options);
    ASSERT_NE(lyr1, nullptr);
    OGRLayer* lyr2 =
        ds->CreateLayer("fd_lyr2", &srs4326, wkbLineString, options);
    ASSERT_NE(lyr2, nullptr);

    // Mismatched SRS in same feature dataset: should fail
    OGRSpatialReference srs32631;
    srs32631.importFromEPSG(32631);
    CPLPushErrorHandler(CPLQuietErrorHandler);
    OGRLayer* lyr3 = ds->CreateLayer("fd_lyr3", &srs32631, wkbPoint, options);
    CPLPopErrorHandler();
    EXPECT_EQ(lyr3, nullptr);
  }

  // Compound SRS with vertical datum and config options
  {
    CPLSetConfigOption("OPENFILEGDB_WRITE_WKID", "YES");
    CPLSetConfigOption("OPENFILEGDB_WRITE_LATESTWKID", "YES");
    CPLSetConfigOption("OPENFILEGDB_WRITE_VCSWKID", "YES");
    CPLSetConfigOption("OPENFILEGDB_WRITE_LATESTVCSWKID", "YES");
    absl::Cleanup cleanup = [] {
      CPLSetConfigOption("OPENFILEGDB_WRITE_WKID", nullptr);
      CPLSetConfigOption("OPENFILEGDB_WRITE_LATESTWKID", nullptr);
      CPLSetConfigOption("OPENFILEGDB_WRITE_VCSWKID", nullptr);
      CPLSetConfigOption("OPENFILEGDB_WRITE_LATESTVCSWKID", nullptr);
    };

    OGRSpatialReference srsCompound;
    srsCompound.importFromEPSG(4326);
    OGRSpatialReference srsVert;
    srsVert.importFromEPSG(5773);
    OGRSpatialReference comp;
    comp.SetCompoundCS("WGS84 + EGM96", &srsCompound, &srsVert);

    OGRLayer* lyr =
        ds->CreateLayer("compound_srs_lyr", &comp, wkbPoint25D, nullptr);
    ASSERT_NE(lyr, nullptr);
  }

  CloseDS(*ds);
}

// -----------------------------------------------------------------------------
// Test 2: Field Creation, Types, Defaults, and XML Definition
// -----------------------------------------------------------------------------
TEST_F(OgrOpenFileGdbLayerWriteTest, FieldCreationTypesAndDefaults) {
  const std::string gdb_path = std::string(kTestDir) + "/field_create.gdb";
  auto ds = CreateGDB(gdb_path);
  ASSERT_NE(ds, nullptr);

  // Setup a coded field domain in datasource to test domain linking
  {
    std::string failureReason;
    auto domain = std::make_unique<OGRCodedFieldDomain>(
        "my_domain", "My Domain Description", OFTInteger, OFSTNone,
        std::vector<OGRCodedValue>{{CPLStrdup("1"), CPLStrdup("One")},
                                   {nullptr, nullptr}});
    EXPECT_TRUE(ds->AddFieldDomain(std::move(domain), failureReason));
  }

  OGRLayer* lyr = ds->CreateLayer("fields_test", nullptr, wkbPoint, nullptr);
  ASSERT_NE(lyr, nullptr);

  // 1. Basic types
  {
    OGRFieldDefn fldInt("f_int", OFTInteger);
    EXPECT_EQ(lyr->CreateField(&fldInt), OGRERR_NONE);

    OGRFieldDefn fldInt16("f_int16", OFTInteger);
    fldInt16.SetSubType(OFSTInt16);
    EXPECT_EQ(lyr->CreateField(&fldInt16), OGRERR_NONE);

    OGRFieldDefn fldReal("f_real", OFTReal);
    EXPECT_EQ(lyr->CreateField(&fldReal), OGRERR_NONE);

    OGRFieldDefn fldFloat("f_float", OFTReal);
    fldFloat.SetSubType(OFSTFloat32);
    EXPECT_EQ(lyr->CreateField(&fldFloat), OGRERR_NONE);

    // In legacy mode (default), Integer64, Date, Time become Float64 / DateTime
    CPLPushErrorHandler(CPLQuietErrorHandler);
    OGRFieldDefn fldInt64("f_int64", OFTInteger64);
    EXPECT_EQ(lyr->CreateField(&fldInt64), OGRERR_NONE);

    OGRFieldDefn fldDate("f_date", OFTDate);
    EXPECT_EQ(lyr->CreateField(&fldDate), OGRERR_NONE);

    OGRFieldDefn fldTime("f_time", OFTTime);
    EXPECT_EQ(lyr->CreateField(&fldTime), OGRERR_NONE);
    CPLPopErrorHandler();

    OGRFieldDefn fldDateTime("f_datetime", OFTDateTime);
    EXPECT_EQ(lyr->CreateField(&fldDateTime), OGRERR_NONE);

    OGRFieldDefn fldStr("f_str", OFTString);
    fldStr.SetWidth(50);
    EXPECT_EQ(lyr->CreateField(&fldStr), OGRERR_NONE);

    OGRFieldDefn fldBin("f_bin", OFTBinary);
    EXPECT_EQ(lyr->CreateField(&fldBin), OGRERR_NONE);

    // List types become string
    OGRFieldDefn fldStrList("f_strlist", OFTStringList);
    EXPECT_EQ(lyr->CreateField(&fldStrList), OGRERR_NONE);
    OGRFieldDefn fldIntList("f_intlist", OFTIntegerList);
    EXPECT_EQ(lyr->CreateField(&fldIntList), OGRERR_NONE);
    OGRFieldDefn fldInt64List("f_int64list", OFTInteger64List);
    EXPECT_EQ(lyr->CreateField(&fldInt64List), OGRERR_NONE);
    OGRFieldDefn fldRealList("f_reallist", OFTRealList);
    EXPECT_EQ(lyr->CreateField(&fldRealList), OGRERR_NONE);
  }

  // 2. String default width config option
  {
    CPLSetConfigOption("OPENFILEGDB_DEFAULT_STRING_WIDTH", "80");
    absl::Cleanup cleanup = [] {
      CPLSetConfigOption("OPENFILEGDB_DEFAULT_STRING_WIDTH", nullptr);
    };
    OGRFieldDefn fldStrDefault("f_str_default", OFTString);
    EXPECT_EQ(lyr->CreateField(&fldStrDefault), OGRERR_NONE);
    int idxStrDef = lyr->GetLayerDefn()->GetFieldIndex("f_str_default");
    ASSERT_GE(idxStrDef, 0);
    EXPECT_EQ(lyr->GetLayerDefn()->GetFieldDefn(idxStrDef)->GetWidth(), 80);
  }

  // 3. Field name laundering
  {
    // ApproxOK = false: laundering fails
    OGRFieldDefn fldBad("bad field name!", OFTString);
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_NE(lyr->CreateField(&fldBad, /*bApproxOK=*/FALSE), OGRERR_NONE);
    CPLPopErrorHandler();

    // ApproxOK = true: laundering succeeds with warning
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(lyr->CreateField(&fldBad, /*bApproxOK=*/TRUE), OGRERR_NONE);
    CPLPopErrorHandler();
    int idxLaundered = lyr->GetLayerDefn()->GetFieldIndex("bad_field_name_");
    EXPECT_GE(idxLaundered, 0);
  }

  // 4. Duplicate field names (test uniqueness numbering _1.._9 and _10.._99)
  for (int i = 0; i < 12; ++i) {
    OGRFieldDefn fldDup("dup_field", OFTInteger);
    EXPECT_EQ(lyr->CreateField(&fldDup, /*bApproxOK=*/TRUE), OGRERR_NONE);
  }

  // 5. Field matching FID column name (OBJECTID)
  {
    // OFTInteger: succeeds
    OGRFieldDefn fldFID("OBJECTID", OFTInteger);
    EXPECT_EQ(lyr->CreateField(&fldFID), OGRERR_NONE);

    // Second time with wrong type: fails
    OGRFieldDefn fldBadFID("OBJECTID", OFTString);
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_NE(lyr->CreateField(&fldBadFID), OGRERR_NONE);
    CPLPopErrorHandler();
  }

  // 6. Default values parsing: integer, real, int64, string, date, time,
  // datetime
  {
    OGRFieldDefn fld("def_int", OFTInteger);
    fld.SetDefault("42");
    EXPECT_EQ(lyr->CreateField(&fld), OGRERR_NONE);
  }
  {
    OGRFieldDefn fld("def_real", OFTReal);
    fld.SetDefault("3.14159265");
    EXPECT_EQ(lyr->CreateField(&fld), OGRERR_NONE);
  }
  {
    OGRFieldDefn fld("def_str", OFTString);
    fld.SetDefault("'hello ''world'''");
    EXPECT_EQ(lyr->CreateField(&fld), OGRERR_NONE);
  }
  {
    OGRFieldDefn fld("def_dt", OFTDateTime);
    fld.SetDefault("'2023-02-01T04:05:06'");
    EXPECT_EQ(lyr->CreateField(&fld), OGRERR_NONE);
  }
  {
    OGRFieldDefn fld("def_bad_dt", OFTDateTime);
    fld.SetDefault("CURRENT_TIMESTAMP");
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_NE(lyr->CreateField(&fld, /*bApproxOK=*/FALSE), OGRERR_NONE);
    EXPECT_EQ(lyr->CreateField(&fld, /*bApproxOK=*/TRUE), OGRERR_NONE);

    fld.SetName("def_unparseable");
    fld.SetDefault("'not-a-date'");
    EXPECT_NE(lyr->CreateField(&fld, /*bApproxOK=*/FALSE), OGRERR_NONE);
    EXPECT_EQ(lyr->CreateField(&fld, /*bApproxOK=*/TRUE), OGRERR_NONE);
    CPLPopErrorHandler();
  }

  // 7. Domain linking and alias
  {
    OGRFieldDefn fldDomain("f_domain", OFTInteger);
    fldDomain.SetDomainName("my_domain");
    fldDomain.SetAlternativeName("My Domain Alias");
    EXPECT_EQ(lyr->CreateField(&fldDomain), OGRERR_NONE);

    // Non-existent domain: cleared with warning/fallback
    OGRFieldDefn fldMissingDomain("f_no_domain", OFTInteger);
    fldMissingDomain.SetDomainName("non_existent_domain");
    EXPECT_EQ(lyr->CreateField(&fldMissingDomain), OGRERR_NONE);
    int idxMissing = lyr->GetLayerDefn()->GetFieldIndex("f_no_domain");
    ASSERT_GE(idxMissing, 0);
    EXPECT_TRUE(
        lyr->GetLayerDefn()->GetFieldDefn(idxMissing)->GetDomainName().empty());
  }

  // 8. Shape area and length special fields
  {
    OGRFieldDefn fldArea("shape_area_calc", OFTReal);
    fldArea.SetDefault("FILEGEODATABASE_SHAPE_AREA");
    EXPECT_EQ(lyr->CreateField(&fldArea), OGRERR_NONE);

    OGRFieldDefn fldLen("shape_length_calc", OFTReal);
    fldLen.SetDefault("FILEGEODATABASE_SHAPE_LENGTH");
    EXPECT_EQ(lyr->CreateField(&fldLen), OGRERR_NONE);
  }

  // 9. SyncToDisk() (registers table in GDB_Items) then CreateField on
  // registered table
  EXPECT_EQ(lyr->SyncToDisk(), OGRERR_NONE);
  {
    OGRFieldDefn fldAfterSync("fld_after_sync", OFTString);
    EXPECT_EQ(lyr->CreateField(&fldAfterSync), OGRERR_NONE);
  }

  // 10. ArcGIS Pro 3.2+ layer: test native BigInteger, DateOnly, TimeOnly,
  // TimestampOffset
  {
    const char* options[] = {"TARGET_ARCGIS_VERSION=ARCGIS_PRO_3_2_OR_LATER",
                             nullptr};
    OGRLayer* lyr32 = ds->CreateLayer("v32_fields", nullptr, wkbPoint, options);
    ASSERT_NE(lyr32, nullptr);

    OGRFieldDefn fldInt64("f_int64", OFTInteger64);
    fldInt64.SetDefault("9876543210123");
    EXPECT_EQ(lyr32->CreateField(&fldInt64), OGRERR_NONE);

    OGRFieldDefn fldDate("f_date", OFTDate);
    fldDate.SetDefault("'2023-02-01'");
    EXPECT_EQ(lyr32->CreateField(&fldDate), OGRERR_NONE);

    OGRFieldDefn fldTime("f_time", OFTTime);
    fldTime.SetDefault("'04:05:06'");
    EXPECT_EQ(lyr32->CreateField(&fldTime), OGRERR_NONE);

    OGRFieldDefn fldDTOffset("f_dtoffset", OFTDateTime);
    fldDTOffset.SetDefault("'2023-02-01T04:05:06+06:00'");
    EXPECT_EQ(lyr32->CreateField(&fldDTOffset), OGRERR_NONE);

    EXPECT_EQ(lyr32->SyncToDisk(), OGRERR_NONE);
  }

  // 11. COLUMN_TYPES creation option
  {
    const char* options[] = {"TARGET_ARCGIS_VERSION=ARCGIS_PRO_3_2_OR_LATER",
                             "COLUMN_TYPES="
                             "c_sint=esriFieldTypeSmallInteger,"
                             "c_int=esriFieldTypeInteger,"
                             "c_bint=esriFieldTypeBigInteger,"
                             "c_single=esriFieldTypeSingle,"
                             "c_double=esriFieldTypeDouble,"
                             "c_str=esriFieldTypeString,"
                             "c_date=esriFieldTypeDate,"
                             "c_blob=esriFieldTypeBlob,"
                             "c_guid=esriFieldTypeGUID,"
                             "c_globalid=esriFieldTypeGlobalID,"
                             "c_xml=esriFieldTypeXML,"
                             "c_dateonly=esriFieldTypeDateOnly,"
                             "c_timeonly=esriFieldTypeTimeOnly,"
                             "c_tsoffset=esriFieldTypeTimestampOffset,"
                             "c_inconsist=esriFieldTypeInteger,"
                             "c_unrecog=esriFieldTypeUnknown",
                             nullptr};
    OGRLayer* lyrCols =
        ds->CreateLayer("col_types_test", nullptr, wkbPoint, options);
    ASSERT_NE(lyrCols, nullptr);

    OGRFieldDefn c_sint("c_sint", OFTInteger);
    EXPECT_EQ(lyrCols->CreateField(&c_sint), OGRERR_NONE);
    OGRFieldDefn c_int("c_int", OFTInteger);
    EXPECT_EQ(lyrCols->CreateField(&c_int), OGRERR_NONE);
    OGRFieldDefn c_bint("c_bint", OFTInteger64);
    EXPECT_EQ(lyrCols->CreateField(&c_bint), OGRERR_NONE);
    OGRFieldDefn c_single("c_single", OFTReal);
    EXPECT_EQ(lyrCols->CreateField(&c_single), OGRERR_NONE);
    OGRFieldDefn c_double("c_double", OFTReal);
    EXPECT_EQ(lyrCols->CreateField(&c_double), OGRERR_NONE);
    OGRFieldDefn c_str("c_str", OFTString);
    EXPECT_EQ(lyrCols->CreateField(&c_str), OGRERR_NONE);
    OGRFieldDefn c_date("c_date", OFTDateTime);
    EXPECT_EQ(lyrCols->CreateField(&c_date), OGRERR_NONE);
    OGRFieldDefn c_blob("c_blob", OFTBinary);
    EXPECT_EQ(lyrCols->CreateField(&c_blob), OGRERR_NONE);
    OGRFieldDefn c_guid("c_guid", OFTString);
    EXPECT_EQ(lyrCols->CreateField(&c_guid), OGRERR_NONE);
    OGRFieldDefn c_globalid("c_globalid", OFTString);
    EXPECT_EQ(lyrCols->CreateField(&c_globalid), OGRERR_NONE);
    OGRFieldDefn c_xml("c_xml", OFTString);
    EXPECT_EQ(lyrCols->CreateField(&c_xml), OGRERR_NONE);
    OGRFieldDefn c_dateonly("c_dateonly", OFTDate);
    EXPECT_EQ(lyrCols->CreateField(&c_dateonly), OGRERR_NONE);
    OGRFieldDefn c_timeonly("c_timeonly", OFTTime);
    EXPECT_EQ(lyrCols->CreateField(&c_timeonly), OGRERR_NONE);
    OGRFieldDefn c_tsoffset("c_tsoffset", OFTDateTime);
    EXPECT_EQ(lyrCols->CreateField(&c_tsoffset), OGRERR_NONE);

    // Inconsistent and unrecognized
    CPLPushErrorHandler(CPLQuietErrorHandler);
    OGRFieldDefn c_inconsist("c_inconsist",
                             OFTString);  // string vs esriFieldTypeInteger
    EXPECT_EQ(lyrCols->CreateField(&c_inconsist), OGRERR_NONE);
    OGRFieldDefn c_unrecog("c_unrecog", OFTString);
    EXPECT_EQ(lyrCols->CreateField(&c_unrecog), OGRERR_NONE);
    CPLPopErrorHandler();
  }

  CloseDS(*ds);
}

// -----------------------------------------------------------------------------
// Test 3: Field and Geometry Field Alteration and Deletion
// -----------------------------------------------------------------------------
TEST_F(OgrOpenFileGdbLayerWriteTest, AlterationAndDeletion) {
  const std::string gdb_path = std::string(kTestDir) + "/alter_delete.gdb";
  auto ds = CreateGDB(gdb_path);
  ASSERT_NE(ds, nullptr);

  // Add domain
  {
    std::string failureReason;
    auto domain = std::make_unique<OGRCodedFieldDomain>(
        "alter_domain", "Alter Domain", OFTInteger, OFSTNone,
        std::vector<OGRCodedValue>{{CPLStrdup("10"), CPLStrdup("Ten")},
                                   {nullptr, nullptr}});
    EXPECT_TRUE(ds->AddFieldDomain(std::move(domain), failureReason));
  }

  const char* options[] = {"CREATE_SHAPE_AREA_AND_LENGTH_FIELDS=YES", nullptr};
  OGRLayer* lyr = ds->CreateLayer("alter_test", nullptr, wkbPolygon, options);
  ASSERT_NE(lyr, nullptr);

  // Create fields
  OGRFieldDefn fldFidCol("OBJECTID", OFTInteger);
  EXPECT_EQ(lyr->CreateField(&fldFidCol), OGRERR_NONE);

  OGRFieldDefn fldA("field_a", OFTString);
  fldA.SetWidth(30);
  fldA.SetDefault("'def'");
  EXPECT_EQ(lyr->CreateField(&fldA), OGRERR_NONE);

  OGRFieldDefn fldB("field_b", OFTInteger);
  fldB.SetDomainName("alter_domain");
  EXPECT_EQ(lyr->CreateField(&fldB), OGRERR_NONE);

  // Sync to disk to test alterations on registered table
  EXPECT_EQ(lyr->SyncToDisk(), OGRERR_NONE);

  int idxA = lyr->GetLayerDefn()->GetFieldIndex("field_a");
  ASSERT_GE(idxA, 0);

  // 1. AlterFieldDefn error checks:
  // Invalid index
  OGRFieldDefn fldDummy("dummy", OFTString);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_NE(lyr->AlterFieldDefn(-1, &fldDummy, ALTER_ALL_FLAG), OGRERR_NONE);
  EXPECT_NE(lyr->AlterFieldDefn(100, &fldDummy, ALTER_ALL_FLAG), OGRERR_NONE);

  // FID regular column alteration: rejected
  int idxFid = lyr->GetLayerDefn()->GetFieldIndex("OBJECTID");
  EXPECT_NE(lyr->AlterFieldDefn(idxFid, &fldDummy, ALTER_ALL_FLAG),
            OGRERR_NONE);

  // Alter type: rejected
  OGRFieldDefn fldTypeChange("field_a", OFTInteger);
  EXPECT_NE(lyr->AlterFieldDefn(idxA, &fldTypeChange, ALTER_TYPE_FLAG),
            OGRERR_NONE);

  // Alter name with invalid laundered name: rejected
  OGRFieldDefn fldBadName("123bad", OFTString);
  EXPECT_NE(lyr->AlterFieldDefn(idxA, &fldBadName, ALTER_NAME_FLAG),
            OGRERR_NONE);

  // Alter nullable state: rejected
  OGRFieldDefn fldNullChange("field_a", OFTString);
  fldNullChange.SetNullable(FALSE);
  EXPECT_NE(lyr->AlterFieldDefn(idxA, &fldNullChange, ALTER_NULLABLE_FLAG),
            OGRERR_NONE);
  CPLPopErrorHandler();

  // 2. Successful AlterFieldDefn:
  // Alter name, width, default, alternative name
  OGRFieldDefn fldNewA("field_a_renamed", OFTString);
  fldNewA.SetWidth(60);
  fldNewA.SetDefault("'new_def'");
  fldNewA.SetAlternativeName("Field A Alias");
  EXPECT_EQ(
      lyr->AlterFieldDefn(idxA, &fldNewA,
                          ALTER_NAME_FLAG | ALTER_WIDTH_PRECISION_FLAG |
                              ALTER_DEFAULT_FLAG | ALTER_ALTERNATIVE_NAME_FLAG),
      OGRERR_NONE);
  EXPECT_STREQ(lyr->GetLayerDefn()->GetFieldDefn(idxA)->GetNameRef(),
               "field_a_renamed");
  EXPECT_EQ(lyr->GetLayerDefn()->GetFieldDefn(idxA)->GetWidth(), 60);

  // Alter area field name
  int idxArea = lyr->GetLayerDefn()->GetFieldIndex("Shape_Area");
  if (idxArea >= 0) {
    OGRFieldDefn fldNewArea("My_Shape_Area", OFTReal);
    EXPECT_EQ(lyr->AlterFieldDefn(idxArea, &fldNewArea, ALTER_NAME_FLAG),
              OGRERR_NONE);
    EXPECT_STREQ(lyr->GetLayerDefn()->GetFieldDefn(idxArea)->GetNameRef(),
                 "My_Shape_Area");
  }

  // Alter length field name
  int idxLen = lyr->GetLayerDefn()->GetFieldIndex("Shape_Length");
  if (idxLen >= 0) {
    OGRFieldDefn fldNewLen("My_Shape_Length", OFTReal);
    EXPECT_EQ(lyr->AlterFieldDefn(idxLen, &fldNewLen, ALTER_NAME_FLAG),
              OGRERR_NONE);
    EXPECT_STREQ(lyr->GetLayerDefn()->GetFieldDefn(idxLen)->GetNameRef(),
                 "My_Shape_Length");
  }

  // Alter domain name (unlink)
  int idxB = lyr->GetLayerDefn()->GetFieldIndex("field_b");
  ASSERT_GE(idxB, 0);
  OGRFieldDefn fldNewB("field_b", OFTInteger);
  fldNewB.SetDomainName("");
  EXPECT_EQ(lyr->AlterFieldDefn(idxB, &fldNewB, ALTER_DOMAIN_FLAG),
            OGRERR_NONE);
  EXPECT_TRUE(lyr->GetLayerDefn()->GetFieldDefn(idxB)->GetDomainName().empty());

  // Alter to link domain on registered table, then unlink it
  EXPECT_EQ(lyr->AlterFieldDefn(idxB, &fldB, ALTER_DOMAIN_FLAG), OGRERR_NONE);
  EXPECT_STREQ(lyr->GetLayerDefn()->GetFieldDefn(idxB)->GetDomainName().c_str(),
               "alter_domain");
  EXPECT_EQ(lyr->AlterFieldDefn(idxB, &fldNewB, ALTER_DOMAIN_FLAG),
            OGRERR_NONE);
  EXPECT_TRUE(lyr->GetLayerDefn()->GetFieldDefn(idxB)->GetDomainName().empty());

  // 3. AlterGeomFieldDefn
  // Invalid index
  OGRGeomFieldDefn geomDummy("dummy", wkbPolygon);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_NE(
      lyr->AlterGeomFieldDefn(-1, &geomDummy, ALTER_GEOM_FIELD_DEFN_ALL_FLAG),
      OGRERR_NONE);
  EXPECT_NE(
      lyr->AlterGeomFieldDefn(5, &geomDummy, ALTER_GEOM_FIELD_DEFN_ALL_FLAG),
      OGRERR_NONE);

  // Alter geom type: rejected
  OGRGeomFieldDefn geomBadType("SHAPE", wkbPoint);
  EXPECT_NE(
      lyr->AlterGeomFieldDefn(0, &geomBadType, ALTER_GEOM_FIELD_DEFN_TYPE_FLAG),
      OGRERR_NONE);

  // Alter geom nullable: rejected
  OGRGeomFieldDefn geomBadNull("SHAPE", wkbPolygon);
  geomBadNull.SetNullable(FALSE);
  EXPECT_NE(lyr->AlterGeomFieldDefn(0, &geomBadNull,
                                    ALTER_GEOM_FIELD_DEFN_NULLABLE_FLAG),
            OGRERR_NONE);

  // Alter geom name with invalid name: rejected
  OGRGeomFieldDefn geomBadName("123bad", wkbPolygon);
  EXPECT_NE(
      lyr->AlterGeomFieldDefn(0, &geomBadName, ALTER_GEOM_FIELD_DEFN_NAME_FLAG),
      OGRERR_NONE);
  CPLPopErrorHandler();

  // Alter geom name to valid name: succeeds
  OGRGeomFieldDefn geomNewName("MY_SHAPE_RENAMED", wkbPolygon);
  EXPECT_EQ(
      lyr->AlterGeomFieldDefn(0, &geomNewName, ALTER_GEOM_FIELD_DEFN_NAME_FLAG),
      OGRERR_NONE);
  EXPECT_STREQ(lyr->GetLayerDefn()->GetGeomFieldDefn(0)->GetNameRef(),
               "MY_SHAPE_RENAMED");

  // Alter geom SRS: succeeds
  OGRSpatialReference srs4326;
  srs4326.importFromEPSG(4326);
  OGRGeomFieldDefn geomNewSRS("MY_SHAPE_RENAMED", wkbPolygon);
  geomNewSRS.SetSpatialRef(&srs4326);
  EXPECT_EQ(
      lyr->AlterGeomFieldDefn(0, &geomNewSRS, ALTER_GEOM_FIELD_DEFN_SRS_FLAG),
      OGRERR_NONE);

  // Alter geom SRS to nullptr: succeeds
  OGRGeomFieldDefn geomNullSRS("MY_SHAPE_RENAMED", wkbPolygon);
  geomNullSRS.SetSpatialRef(nullptr);
  EXPECT_EQ(
      lyr->AlterGeomFieldDefn(0, &geomNullSRS, ALTER_GEOM_FIELD_DEFN_SRS_FLAG),
      OGRERR_NONE);

  // In a feature dataset, altering geom SRS is rejected
  {
    const char* fd_opts[] = {"FEATURE_DATASET=FD_SRS_TEST", nullptr};
    OGRLayer* fdLyr =
        ds->CreateLayer("fd_geom_lyr", &srs4326, wkbPoint, fd_opts);
    ASSERT_NE(fdLyr, nullptr);
    OGRSpatialReference srs32631;
    srs32631.importFromEPSG(32631);
    OGRGeomFieldDefn fdGeom("SHAPE", wkbPoint);
    fdGeom.SetSpatialRef(&srs32631);
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_NE(
        fdLyr->AlterGeomFieldDefn(0, &fdGeom, ALTER_GEOM_FIELD_DEFN_SRS_FLAG),
        OGRERR_NONE);
    CPLPopErrorHandler();
  }

  // 4. DeleteField
  // Invalid index
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_NE(lyr->DeleteField(-1), OGRERR_NONE);
  EXPECT_NE(lyr->DeleteField(100), OGRERR_NONE);

  // Deleting FID column: rejected
  idxFid = lyr->GetLayerDefn()->GetFieldIndex("OBJECTID");
  EXPECT_NE(lyr->DeleteField(idxFid), OGRERR_NONE);
  CPLPopErrorHandler();

  // Deleting normal field
  int idxToDelete = lyr->GetLayerDefn()->GetFieldIndex("field_a_renamed");
  ASSERT_GE(idxToDelete, 0);
  EXPECT_EQ(lyr->DeleteField(idxToDelete), OGRERR_NONE);
  EXPECT_EQ(lyr->GetLayerDefn()->GetFieldIndex("field_a_renamed"), -1);

  // Deleting area and length fields
  idxArea = lyr->GetLayerDefn()->GetFieldIndex("My_Shape_Area");
  if (idxArea >= 0) {
    EXPECT_EQ(lyr->DeleteField(idxArea), OGRERR_NONE);
  }
  idxLen = lyr->GetLayerDefn()->GetFieldIndex("My_Shape_Length");
  if (idxLen >= 0) {
    EXPECT_EQ(lyr->DeleteField(idxLen), OGRERR_NONE);
  }

  CloseDS(*ds);
}

// -----------------------------------------------------------------------------
// Test 4: Feature Operations (CreateFeature, SetFeature, DeleteFeature)
// -----------------------------------------------------------------------------
TEST_F(OgrOpenFileGdbLayerWriteTest, FeatureCreateSetDelete) {
  const std::string gdb_path = std::string(kTestDir) + "/features.gdb";
  auto ds = CreateGDB(gdb_path);
  ASSERT_NE(ds, nullptr);

  // 1. Geometry type compatibility checks in ICreateFeature
  // Point layer
  {
    OGRLayer* lyrPt = ds->CreateLayer("lyr_pt", nullptr, wkbPoint, nullptr);
    ASSERT_NE(lyrPt, nullptr);

    OGRFeature featPt(lyrPt->GetLayerDefn());
    OGRPoint pt(1.0, 2.0);
    featPt.SetGeometry(&pt);
    EXPECT_EQ(lyrPt->CreateFeature(&featPt), OGRERR_NONE);
    EXPECT_EQ(featPt.GetFID(), 1);

    // Mismatched geometry: LineString in Point layer
    OGRLineString line;
    line.addPoint(0, 0);
    line.addPoint(1, 1);
    featPt.SetGeometry(&line);
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_NE(lyrPt->CreateFeature(&featPt), OGRERR_NONE);
    CPLPopErrorHandler();
  }

  // MultiPoint layer
  {
    OGRLayer* lyrMPt =
        ds->CreateLayer("lyr_mpt", nullptr, wkbMultiPoint, nullptr);
    ASSERT_NE(lyrMPt, nullptr);

    OGRFeature featMPt(lyrMPt->GetLayerDefn());
    OGRMultiPoint mpt;
    OGRPoint pt(1.0, 2.0);
    mpt.addGeometry(&pt);
    featMPt.SetGeometry(&mpt);
    EXPECT_EQ(lyrMPt->CreateFeature(&featMPt), OGRERR_NONE);

    // Mismatched: Point in MultiPoint layer
    featMPt.SetGeometry(&pt);
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_NE(lyrMPt->CreateFeature(&featMPt), OGRERR_NONE);
    CPLPopErrorHandler();
  }

  // Line layer: accepts LineString, MultiLineString, CircularString,
  // CompoundCurve, MultiCurve
  {
    OGRLayer* lyrLine =
        ds->CreateLayer("lyr_line", nullptr, wkbLineString, nullptr);
    ASSERT_NE(lyrLine, nullptr);

    OGRFeature feat(lyrLine->GetLayerDefn());
    OGRLineString ls;
    ls.addPoint(0, 0);
    ls.addPoint(1, 1);
    feat.SetGeometry(&ls);
    EXPECT_EQ(lyrLine->CreateFeature(&feat), OGRERR_NONE);

    OGRMultiLineString mls;
    mls.addGeometry(&ls);
    feat.SetFID(OGRNullFID);
    feat.SetGeometry(&mls);
    EXPECT_EQ(lyrLine->CreateFeature(&feat), OGRERR_NONE);

    OGRCircularString cs;
    cs.addPoint(0, 0);
    cs.addPoint(1, 1);
    cs.addPoint(2, 0);
    feat.SetFID(OGRNullFID);
    feat.SetGeometry(&cs);
    EXPECT_EQ(lyrLine->CreateFeature(&feat), OGRERR_NONE);

    OGRCompoundCurve cc;
    cc.addCurveDirectly(cs.clone());
    feat.SetFID(OGRNullFID);
    feat.SetGeometry(&cc);
    EXPECT_EQ(lyrLine->CreateFeature(&feat), OGRERR_NONE);

    OGRMultiCurve mc;
    mc.addGeometry(&ls);
    feat.SetFID(OGRNullFID);
    feat.SetGeometry(&mc);
    EXPECT_EQ(lyrLine->CreateFeature(&feat), OGRERR_NONE);

    // Mismatched: Polygon in Line layer
    OGRPolygon poly;
    OGRLinearRing ring;
    ring.addPoint(0, 0);
    ring.addPoint(0, 1);
    ring.addPoint(1, 0);
    ring.addPoint(0, 0);
    poly.addRing(&ring);
    feat.SetGeometry(&poly);
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_NE(lyrLine->CreateFeature(&feat), OGRERR_NONE);
    CPLPopErrorHandler();
  }

  // Polygon layer: accepts Polygon, MultiPolygon, CurvePolygon, MultiSurface
  {
    const char* options[] = {"CREATE_SHAPE_AREA_AND_LENGTH_FIELDS=YES",
                             nullptr};
    OGRLayer* lyrPoly =
        ds->CreateLayer("lyr_poly", nullptr, wkbPolygon, options);
    ASSERT_NE(lyrPoly, nullptr);

    OGRFeature feat(lyrPoly->GetLayerDefn());
    OGRPolygon poly;
    OGRLinearRing ring;
    ring.addPoint(0, 0);
    ring.addPoint(0, 10);
    ring.addPoint(10, 10);
    ring.addPoint(10, 0);
    ring.addPoint(0, 0);
    poly.addRing(&ring);
    feat.SetGeometry(&poly);
    EXPECT_EQ(lyrPoly->CreateFeature(&feat), OGRERR_NONE);

    // Check automatic area and length computation
    int idxArea = lyrPoly->GetLayerDefn()->GetFieldIndex("Shape_Area");
    int idxLen = lyrPoly->GetLayerDefn()->GetFieldIndex("Shape_Length");
    ASSERT_GE(idxArea, 0);
    ASSERT_GE(idxLen, 0);
    EXPECT_NEAR(feat.GetFieldAsDouble(idxArea), 100.0, 1e-4);
    EXPECT_NEAR(feat.GetFieldAsDouble(idxLen), 40.0, 1e-4);

    // MultiPolygon
    feat.SetFID(OGRNullFID);
    OGRMultiPolygon mpoly;
    mpoly.addGeometry(&poly);
    feat.SetGeometry(&mpoly);
    EXPECT_EQ(lyrPoly->CreateFeature(&feat), OGRERR_NONE);

    // CurvePolygon
    feat.SetFID(OGRNullFID);
    OGRCurvePolygon cpoly;
    auto closed_cs = std::make_unique<OGRCircularString>();
    closed_cs->addPoint(0, 0);
    closed_cs->addPoint(0, 2);
    closed_cs->addPoint(2, 2);
    closed_cs->addPoint(2, 0);
    closed_cs->addPoint(0, 0);
    EXPECT_EQ(cpoly.addRing(std::move(closed_cs)), OGRERR_NONE);
    feat.SetGeometry(&cpoly);
    EXPECT_EQ(lyrPoly->CreateFeature(&feat), OGRERR_NONE);

    // MultiSurface
    feat.SetFID(OGRNullFID);
    OGRMultiSurface msurf;
    msurf.addGeometry(&poly);
    feat.SetGeometry(&msurf);
    EXPECT_EQ(lyrPoly->CreateFeature(&feat), OGRERR_NONE);

    // Null geometry
    feat.SetFID(OGRNullFID);
    feat.SetGeometry(nullptr);
    EXPECT_EQ(lyrPoly->CreateFeature(&feat), OGRERR_NONE);
    EXPECT_TRUE(feat.IsFieldNull(idxArea));
    EXPECT_TRUE(feat.IsFieldNull(idxLen));
  }

  // MultiPatch layer: accepts TIN, PolyhedralSurface, GeometryCollection
  {
    OGRLayer* lyrTIN = ds->CreateLayer("lyr_tin", nullptr, wkbTIN, nullptr);
    ASSERT_NE(lyrTIN, nullptr);

    OGRTriangulatedSurface tin;
    OGRTriangle tri(OGRPoint(0, 0, 0), OGRPoint(0, 1, 0), OGRPoint(1, 0, 0));
    tin.addGeometry(&tri);
    OGRFeature feat(lyrTIN->GetLayerDefn());
    feat.SetGeometry(&tin);
    EXPECT_EQ(lyrTIN->CreateFeature(&feat), OGRERR_NONE);

    OGRPolyhedralSurface ps;
    OGRPolygon poly;
    OGRLinearRing ring;
    ring.addPoint(0, 0, 0);
    ring.addPoint(0, 1, 0);
    ring.addPoint(1, 0, 0);
    ring.addPoint(0, 0, 0);
    poly.addRing(&ring);
    ps.addGeometry(&poly);
    feat.SetFID(OGRNullFID);
    feat.SetGeometry(&ps);
    EXPECT_EQ(lyrTIN->CreateFeature(&feat), OGRERR_NONE);

    feat.SetFID(OGRNullFID);
    OGRGeometryCollection gc;
    gc.addGeometry(&poly);
    feat.SetGeometry(&gc);
    EXPECT_EQ(lyrTIN->CreateFeature(&feat), OGRERR_NONE);

    // Point in TIN: rejected
    OGRPoint pt(1, 2, 3);
    feat.SetGeometry(&pt);
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_NE(lyrTIN->CreateFeature(&feat), OGRERR_NONE);
    CPLPopErrorHandler();
  }

  // MultiPolygon in CREATE_MULTIPATCH=YES
  {
    const char* options[] = {"CREATE_MULTIPATCH=YES", nullptr};
    OGRLayer* lyrMP =
        ds->CreateLayer("lyr_mp_multipatch", nullptr, wkbMultiPolygon, options);
    ASSERT_NE(lyrMP, nullptr);
    OGRPolygon poly;
    OGRLinearRing ring;
    ring.addPoint(0, 0, 0);
    ring.addPoint(0, 1, 0);
    ring.addPoint(1, 0, 0);
    ring.addPoint(0, 0, 0);
    poly.addRing(&ring);
    OGRMultiPolygon mpoly;
    mpoly.addGeometry(&poly);
    OGRFeature feat(lyrMP->GetLayerDefn());
    feat.SetGeometry(&mpoly);
    EXPECT_EQ(lyrMP->CreateFeature(&feat), OGRERR_NONE);
  }

  // Empty geometry handling: OGR_OPENFILEGDB_WRITE_EMPTY_GEOMETRY
  {
    OGRLayer* lyrEmpty =
        ds->CreateLayer("lyr_empty", nullptr, wkbPoint, nullptr);
    ASSERT_NE(lyrEmpty, nullptr);

    OGRPoint emptyPt;
    OGRFeature feat(lyrEmpty->GetLayerDefn());
    feat.SetGeometry(&emptyPt);
    EXPECT_EQ(lyrEmpty->CreateFeature(&feat), OGRERR_NONE);

    CPLSetConfigOption("OGR_OPENFILEGDB_WRITE_EMPTY_GEOMETRY", "YES");
    absl::Cleanup cleanup = [] {
      CPLSetConfigOption("OGR_OPENFILEGDB_WRITE_EMPTY_GEOMETRY", nullptr);
    };
    feat.SetFID(OGRNullFID);
    EXPECT_EQ(lyrEmpty->CreateFeature(&feat), OGRERR_NONE);
  }

  // 2. All field data types and GlobalID / UUID generation
  {
    const char* col_opts[] = {"TARGET_ARCGIS_VERSION=ARCGIS_PRO_3_2_OR_LATER",
                              "COLUMN_TYPES="
                              "gid_auto=esriFieldTypeGlobalID,"
                              "gid_custom=esriFieldTypeGlobalID,"
                              "guid_col=esriFieldTypeGUID,"
                              "xml_col=esriFieldTypeXML",
                              nullptr};
    OGRLayer* lyrTypes =
        ds->CreateLayer("lyr_all_types", nullptr, wkbNone, col_opts);
    ASSERT_NE(lyrTypes, nullptr);

    OGRFieldDefn fInt("f_int", OFTInteger);
    OGRFieldDefn fInt16("f_int16", OFTInteger);
    fInt16.SetSubType(OFSTInt16);
    OGRFieldDefn fReal("f_real", OFTReal);
    OGRFieldDefn fFloat("f_float", OFTReal);
    fFloat.SetSubType(OFSTFloat32);
    OGRFieldDefn fInt64("f_int64", OFTInteger64);
    OGRFieldDefn fStr("f_str", OFTString);
    OGRFieldDefn fBin("f_bin", OFTBinary);
    OGRFieldDefn fDate("f_date", OFTDate);
    OGRFieldDefn fTime("f_time", OFTTime);
    OGRFieldDefn fDateTime("f_datetime", OFTDateTime);
    OGRFieldDefn fGidAuto("gid_auto", OFTString);
    OGRFieldDefn fGidCustom("gid_custom", OFTString);
    OGRFieldDefn fGuid("guid_col", OFTString);
    OGRFieldDefn fXml("xml_col", OFTString);

    EXPECT_EQ(lyrTypes->CreateField(&fInt), OGRERR_NONE);
    EXPECT_EQ(lyrTypes->CreateField(&fInt16), OGRERR_NONE);
    EXPECT_EQ(lyrTypes->CreateField(&fReal), OGRERR_NONE);
    EXPECT_EQ(lyrTypes->CreateField(&fFloat), OGRERR_NONE);
    EXPECT_EQ(lyrTypes->CreateField(&fInt64), OGRERR_NONE);
    EXPECT_EQ(lyrTypes->CreateField(&fStr), OGRERR_NONE);
    EXPECT_EQ(lyrTypes->CreateField(&fBin), OGRERR_NONE);
    EXPECT_EQ(lyrTypes->CreateField(&fDate), OGRERR_NONE);
    EXPECT_EQ(lyrTypes->CreateField(&fTime), OGRERR_NONE);
    EXPECT_EQ(lyrTypes->CreateField(&fDateTime), OGRERR_NONE);
    EXPECT_EQ(lyrTypes->CreateField(&fGidAuto), OGRERR_NONE);
    EXPECT_EQ(lyrTypes->CreateField(&fGidCustom), OGRERR_NONE);
    EXPECT_EQ(lyrTypes->CreateField(&fGuid), OGRERR_NONE);
    EXPECT_EQ(lyrTypes->CreateField(&fXml), OGRERR_NONE);

    OGRFeature feat(lyrTypes->GetLayerDefn());
    feat.SetField("f_int", 100);
    feat.SetField("f_int16", 20);
    feat.SetField("f_real", 123.456);
    feat.SetField("f_float", 78.5);
    feat.SetField("f_int64", static_cast<GIntBig>(999999999999LL));
    feat.SetField("f_str", "hello world");
    const GByte binData[] = {0x01, 0x02, 0x03, 0x04};
    feat.SetField(feat.GetFieldIndex("f_bin"), 4, binData);
    feat.SetField("f_date", 2023, 5, 12, 0, 0, 0, 0);
    feat.SetField("f_time", 0, 0, 0, 14, 30, 45, 0);
    feat.SetField("f_datetime", 2023, 5, 12, 14, 30, 45, 100);  // UTC
    // gid_auto left unset -> will generate UUID
    feat.SetField("gid_custom", "{12345678-1234-1234-1234-123456789abc}");
    feat.SetField("guid_col", "{87654321-4321-4321-4321-cba987654321}");
    feat.SetField("xml_col", "<data>test</data>");

    // OPENFILEGDB_REGENERATE_GLOBALID=YES: warns and regenerates
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(lyrTypes->CreateFeature(&feat), OGRERR_NONE);
    CPLPopErrorHandler();

    // Test SetFeature to update feature with GLOBALID
    feat.SetField("f_str", "updated string");
    EXPECT_EQ(lyrTypes->SetFeature(&feat), OGRERR_NONE);

    // OPENFILEGDB_REGENERATE_GLOBALID=NO: keeps value
    CPLSetConfigOption("OPENFILEGDB_REGENERATE_GLOBALID", "NO");
    absl::Cleanup cleanup = [] {
      CPLSetConfigOption("OPENFILEGDB_REGENERATE_GLOBALID", nullptr);
    };
    OGRFeature feat2(lyrTypes->GetLayerDefn());
    feat2.SetField("gid_custom", "{12345678-1234-1234-1234-123456789abc}");
    EXPECT_EQ(lyrTypes->CreateFeature(&feat2), OGRERR_NONE);
  }

  // 3. FID column as regular column consistency checks
  {
    OGRLayer* lyrFid =
        ds->CreateLayer("lyr_fid_col", nullptr, wkbNone, nullptr);
    ASSERT_NE(lyrFid, nullptr);

    OGRFieldDefn fldFid("OBJECTID", OFTInteger64);
    EXPECT_EQ(lyrFid->CreateField(&fldFid), OGRERR_NONE);

    // Case a: FID unset in feature, regular column set -> propagates to FID
    OGRFeature feat1(lyrFid->GetLayerDefn());
    feat1.SetField("OBJECTID", static_cast<GIntBig>(5));
    EXPECT_EQ(lyrFid->CreateFeature(&feat1), OGRERR_NONE);
    EXPECT_EQ(feat1.GetFID(), 5);

    // Case b: FID set and matches regular column -> ok
    OGRFeature feat2(lyrFid->GetLayerDefn());
    feat2.SetFID(6);
    feat2.SetField("OBJECTID", static_cast<GIntBig>(6));
    EXPECT_EQ(lyrFid->CreateFeature(&feat2), OGRERR_NONE);

    // Case c: FID set and mismatches regular column -> fails
    OGRFeature feat3(lyrFid->GetLayerDefn());
    feat3.SetFID(7);
    feat3.SetField("OBJECTID", static_cast<GIntBig>(8));
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_NE(lyrFid->CreateFeature(&feat3), OGRERR_NONE);
    CPLPopErrorHandler();

    // Case d: Invalid FID values (< -1, 0, > INT_MAX)
    OGRFeature featBadFid(lyrFid->GetLayerDefn());
    featBadFid.SetFID(-2);
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_NE(lyrFid->CreateFeature(&featBadFid), OGRERR_NONE);
    featBadFid.SetFID(0);
    EXPECT_NE(lyrFid->CreateFeature(&featBadFid), OGRERR_NONE);
    featBadFid.SetFID(static_cast<GIntBig>(std::numeric_limits<int>::max()) +
                      10);
    EXPECT_NE(lyrFid->CreateFeature(&featBadFid), OGRERR_NONE);
    CPLPopErrorHandler();
  }

  // FID as regular column with OFTReal
  {
    OGRLayer* lyrRealFid =
        ds->CreateLayer("lyr_real_fid", nullptr, wkbNone, nullptr);
    ASSERT_NE(lyrRealFid, nullptr);

    OGRFieldDefn fldFid("OBJECTID", OFTReal);
    fldFid.SetWidth(15);
    fldFid.SetPrecision(0);
    EXPECT_EQ(lyrRealFid->CreateField(&fldFid), OGRERR_NONE);

    OGRFeature feat(lyrRealFid->GetLayerDefn());
    feat.SetField("OBJECTID", 10.0);
    EXPECT_EQ(lyrRealFid->CreateFeature(&feat), OGRERR_NONE);
    EXPECT_EQ(feat.GetFID(), 10);

    // Non-integer real value -> fails
    OGRFeature featBad(lyrRealFid->GetLayerDefn());
    featBad.SetField("OBJECTID", 10.5);
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_NE(lyrRealFid->CreateFeature(&featBad), OGRERR_NONE);
    CPLPopErrorHandler();

    // Test SetFeature with real FID
    feat.SetField("OBJECTID", 10.0);
    EXPECT_EQ(lyrRealFid->SetFeature(&feat), OGRERR_NONE);
  }

  // Test DateTime warning with TIME_IN_UTC=YES
  {
    const char* utc_opts[] = {"TIME_IN_UTC=YES", nullptr};
    OGRLayer* lyrUTC =
        ds->CreateLayer("lyr_utc_warn", nullptr, wkbNone, utc_opts);
    ASSERT_NE(lyrUTC, nullptr);
    OGRFieldDefn fDt("dt_col", OFTDateTime);
    EXPECT_EQ(lyrUTC->CreateField(&fDt), OGRERR_NONE);
    EXPECT_EQ(lyrUTC->SyncToDisk(), OGRERR_NONE);

    OGRFeature featUTC(lyrUTC->GetLayerDefn());
    // Local / unknown timezone (TZFlag = 0)
    featUTC.SetField("dt_col", 2023, 1, 1, 12, 0, 0, 0);
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(lyrUTC->CreateFeature(&featUTC), OGRERR_NONE);
    CPLPopErrorHandler();
  }

  // Float64/String from other types
  {
    OGRLayer* lyrConv = ds->CreateLayer("lyr_conv", nullptr, wkbNone, nullptr);
    ASSERT_NE(lyrConv, nullptr);
    OGRFieldDefn fInt("f_int", OFTInteger);
    OGRFieldDefn fStrList("f_strlist", OFTStringList);
    EXPECT_EQ(lyrConv->CreateField(&fInt), OGRERR_NONE);
    EXPECT_EQ(lyrConv->CreateField(&fStrList), OGRERR_NONE);

    OGRFeature featConv(lyrConv->GetLayerDefn());
    featConv.SetField("f_int", 42);
    const char* const listVals[] = {"val1", "val2", nullptr};
    featConv.SetField("f_strlist", listVals);
    EXPECT_EQ(lyrConv->CreateFeature(&featConv), OGRERR_NONE);
  }

  // 4. ISetFeature and DeleteFeature
  {
    OGRLayer* lyrMod = ds->CreateLayer("lyr_mod", nullptr, wkbPoint, nullptr);
    ASSERT_NE(lyrMod, nullptr);
    OGRFieldDefn fldName("name", OFTString);
    EXPECT_EQ(lyrMod->CreateField(&fldName), OGRERR_NONE);

    OGRFeature feat(lyrMod->GetLayerDefn());
    feat.SetField("name", "initial");
    OGRPoint pt(1.0, 1.0);
    feat.SetGeometry(&pt);
    EXPECT_EQ(lyrMod->CreateFeature(&feat), OGRERR_NONE);
    GIntBig nFID = feat.GetFID();

    // SetFeature valid
    feat.SetField("name", "updated");
    OGRPoint pt2(2.0, 2.0);
    feat.SetGeometry(&pt2);
    EXPECT_EQ(lyrMod->SetFeature(&feat), OGRERR_NONE);

    // SetFeature non-existing FID
    feat.SetFID(9999);
    EXPECT_EQ(lyrMod->SetFeature(&feat), OGRERR_NON_EXISTING_FEATURE);
    feat.SetFID(-1);
    EXPECT_EQ(lyrMod->SetFeature(&feat), OGRERR_NON_EXISTING_FEATURE);

    // DeleteFeature valid
    EXPECT_EQ(lyrMod->DeleteFeature(nFID), OGRERR_NONE);

    // DeleteFeature already deleted FID
    EXPECT_EQ(lyrMod->DeleteFeature(nFID), OGRERR_NON_EXISTING_FEATURE);

    // DeleteFeature non-existing FID
    EXPECT_EQ(lyrMod->DeleteFeature(9999), OGRERR_NON_EXISTING_FEATURE);
    EXPECT_EQ(lyrMod->DeleteFeature(-1), OGRERR_NON_EXISTING_FEATURE);
  }

  CloseDS(*ds);
}

// -----------------------------------------------------------------------------
// Test 5: Indexes, Maintenance, Transactions, and Rename
// -----------------------------------------------------------------------------
TEST_F(OgrOpenFileGdbLayerWriteTest, IndexesMaintenanceTransactionsAndRename) {
  const std::string gdb_path = std::string(kTestDir) + "/maint.gdb";
  auto ds = CreateGDB(gdb_path);
  ASSERT_NE(ds, nullptr);

  OGRLayer* lyr = ds->CreateLayer("maint_test", nullptr, wkbPoint, nullptr);
  ASSERT_NE(lyr, nullptr);

  OGRFieldDefn fldCode("code", OFTString);
  EXPECT_EQ(lyr->CreateField(&fldCode), OGRERR_NONE);

  for (int i = 0; i < 5; ++i) {
    OGRFeature feat(lyr->GetLayerDefn());
    feat.SetField("code", CPLSPrintf("item_%d", i));
    OGRPoint pt(i, i);
    feat.SetGeometry(&pt);
    EXPECT_EQ(lyr->CreateFeature(&feat), OGRERR_NONE);
  }

  auto* gdbLayer = dynamic_cast<OGROpenFileGDBLayer*>(lyr);
  ASSERT_NE(gdbLayer, nullptr);

  // 1. CreateSpatialIndex
  gdbLayer->CreateSpatialIndex();

  // 2. CreateIndex
  // Reserved keyword index name -> fails with error
  CPLPushErrorHandler(CPLQuietErrorHandler);
  gdbLayer->CreateIndex("SELECT", "code");
  CPLPopErrorHandler();

  // Valid index
  gdbLayer->CreateIndex("code_idx", "code");

  // 3. Maintenance methods
  EXPECT_TRUE(gdbLayer->Repack(nullptr, nullptr));
  gdbLayer->RecomputeExtent();
  EXPECT_TRUE(gdbLayer->CheckFreeListConsistency());

  // 4. Emulated Transactions (driven through dataset StartTransaction)
  EXPECT_EQ(ds->StartTransaction(TRUE), OGRERR_NONE);

  {
    OGRFeature featTx(lyr->GetLayerDefn());
    featTx.SetField("code", "in_tx");
    OGRPoint ptTx(10, 10);
    featTx.SetGeometry(&ptTx);
    EXPECT_EQ(lyr->CreateFeature(&featTx), OGRERR_NONE);
  }

  OGRFieldDefn fldTx("tx_field", OFTInteger);
  EXPECT_EQ(lyr->CreateField(&fldTx), OGRERR_NONE);

  // Rollback restores feature definition and file state
  EXPECT_EQ(ds->RollbackTransaction(), OGRERR_NONE);
  EXPECT_EQ(lyr->GetLayerDefn()->GetFieldIndex("tx_field"), -1);

  // Another transaction with Commit
  EXPECT_EQ(ds->StartTransaction(TRUE), OGRERR_NONE);
  OGRFieldDefn fldTxCommit("tx_commit_field", OFTInteger);
  EXPECT_EQ(lyr->CreateField(&fldTxCommit), OGRERR_NONE);
  EXPECT_EQ(ds->CommitTransaction(), OGRERR_NONE);
  EXPECT_GE(lyr->GetLayerDefn()->GetFieldIndex("tx_commit_field"), 0);

  // 5. Rename
  // Invalid new name -> fails
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_NE(lyr->Rename("123invalid"), OGRERR_NONE);
  CPLPopErrorHandler();

  // Already existing name -> fails
  OGRLayer* otherLyr = ds->CreateLayer("other_lyr", nullptr, wkbNone, nullptr);
  ASSERT_NE(otherLyr, nullptr);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_NE(lyr->Rename("other_lyr"), OGRERR_NONE);
  CPLPopErrorHandler();

  // Valid rename
  EXPECT_EQ(lyr->Rename("maint_test_renamed"), OGRERR_NONE);
  EXPECT_STREQ(lyr->GetName(), "maint_test_renamed");

  // Rename layer inside a feature dataset
  const char* fd_opts[] = {"FEATURE_DATASET=FD_RENAME", nullptr};
  OGRLayer* fdLyr =
      ds->CreateLayer("fd_lyr_rename", nullptr, wkbPoint, fd_opts);
  ASSERT_NE(fdLyr, nullptr);
  EXPECT_EQ(fdLyr->Rename("fd_lyr_newname"), OGRERR_NONE);
  EXPECT_STREQ(fdLyr->GetName(), "fd_lyr_newname");

  CloseDS(*ds);
}

}  // namespace
}  // namespace autotest2
