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
// Tests Open FileGDB table writing, feature encoding, sparse tablx, and
// repacking (filegdbtable_write.cpp).

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
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/filegdbtable.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/filegdbtable_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using namespace OpenFileGDB;

constexpr char kTestDir[] = "/vsimem/test_filegdbtable_write";

class FileGDBTableWriteTest : public ::testing::Test {
 protected:
  void SetUp() override { VSIMkdir(kTestDir, 0755); }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

std::vector<OGRField> MakeUnsetFields(int count) {
  std::vector<OGRField> fields(count);
  for (auto& f : fields) {
    f.Set.nMarker1 = OGRUnsetMarker;
    f.Set.nMarker2 = OGRUnsetMarker;
    f.Set.nMarker3 = OGRUnsetMarker;
  }
  return fields;
}

TEST_F(FileGDBTableWriteTest, CreateAndBasicErrorsAndUTF16) {
  const std::string table_path = std::string(kTestDir) + "/utf16.gdbtable";

  // Create error: non .gdbtable extension
  {
    FileGDBTable table;
    WithQuietHandler quiet;
    EXPECT_FALSE(table.Create("/vsimem/bad.txt", 4, FGTGT_NONE, false, false));
  }

  // Create error: invalid directory path
  {
    FileGDBTable table;
    WithQuietHandler quiet;
    VSILFILE* fp = VSIFOpenL("/vsimem/dummy_file", "wb");
    if (fp) VSIFCloseL(fp);
    EXPECT_FALSE(table.Create("/vsimem/dummy_file/table.gdbtable", 4,
                              FGTGT_NONE, false, false));
    VSIUnlink("/vsimem/dummy_file");
  }

  // Create error: .gdbtablx cannot be created
  {
    std::string tablx_dir = std::string(kTestDir) + "/fail_tablx.gdbtablx";
    VSIMkdir(tablx_dir.c_str(), 0755);
    FileGDBTable tbl_fail;
    WithQuietHandler quiet;
    EXPECT_FALSE(tbl_fail.Create(
        (std::string(kTestDir) + "/fail_tablx.gdbtable").c_str(), 4, FGTGT_NONE,
        false, false));
    VSIRmdir(tablx_dir.c_str());
  }

  FileGDBTable table;
  ASSERT_TRUE(table.Create(table_path.c_str(), 4, FGTGT_NONE, false, false));

  // SetTextUTF16 immediately after Create succeeds
  EXPECT_TRUE(table.SetTextUTF16());

  // Add fields of various types
  // 0: OBJECTID
  ASSERT_TRUE(table.CreateField(
      std::make_unique<FileGDBField>("OBJECTID", "", FGFT_OBJECTID, false, true,
                                     false, 0, FileGDBField::UNSET_FIELD)));

  // 1: str_utf16 (nullable)
  ASSERT_TRUE(table.CreateField(
      std::make_unique<FileGDBField>("str_utf16", "", FGFT_STRING, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD)));

  // 2: str_req (non-nullable)
  ASSERT_TRUE(table.CreateField(
      std::make_unique<FileGDBField>("str_req", "", FGFT_STRING, false, false,
                                     true, 0, FileGDBField::UNSET_FIELD)));

  // 3: xml_fld
  ASSERT_TRUE(table.CreateField(
      std::make_unique<FileGDBField>("xml_fld", "", FGFT_XML, true, false, true,
                                     0, FileGDBField::UNSET_FIELD)));

  // 4: int16_fld
  ASSERT_TRUE(table.CreateField(
      std::make_unique<FileGDBField>("int16_fld", "", FGFT_INT16, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD)));

  // 5: int32_fld
  ASSERT_TRUE(table.CreateField(
      std::make_unique<FileGDBField>("int32_fld", "", FGFT_INT32, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD)));

  // 6: int64_fld
  ASSERT_TRUE(table.CreateField(
      std::make_unique<FileGDBField>("int64_fld", "", FGFT_INT64, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD)));

  // 7: float32_fld
  ASSERT_TRUE(table.CreateField(std::make_unique<FileGDBField>(
      "float32_fld", "", FGFT_FLOAT32, true, false, true, 0,
      FileGDBField::UNSET_FIELD)));

  // 8: float64_fld
  ASSERT_TRUE(table.CreateField(std::make_unique<FileGDBField>(
      "float64_fld", "", FGFT_FLOAT64, true, false, true, 0,
      FileGDBField::UNSET_FIELD)));

  // 9: date_fld
  ASSERT_TRUE(table.CreateField(
      std::make_unique<FileGDBField>("date_fld", "", FGFT_DATE, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD)));

  // 10: dt_fld (high precision)
  auto dt_fld =
      std::make_unique<FileGDBField>("dt_fld", "", FGFT_DATETIME, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD);
  dt_fld->SetHighPrecision();
  ASSERT_TRUE(table.CreateField(std::move(dt_fld)));

  // 11: time_fld
  ASSERT_TRUE(table.CreateField(
      std::make_unique<FileGDBField>("time_fld", "", FGFT_TIME, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD)));

  // 12: dto_fld
  ASSERT_TRUE(table.CreateField(std::make_unique<FileGDBField>(
      "dto_fld", "", FGFT_DATETIME_WITH_OFFSET, true, false, true, 0,
      FileGDBField::UNSET_FIELD)));

  // 13: bin_fld
  ASSERT_TRUE(table.CreateField(
      std::make_unique<FileGDBField>("bin_fld", "", FGFT_BINARY, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD)));

  // 14: guid_fld
  ASSERT_TRUE(table.CreateField(
      std::make_unique<FileGDBField>("guid_fld", "", FGFT_GUID, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD)));

  // 15: globalid_fld
  ASSERT_TRUE(table.CreateField(std::make_unique<FileGDBField>(
      "globalid_fld", "", FGFT_GLOBALID, false, false, true, 0,
      FileGDBField::UNSET_FIELD)));

  // Calling Sync() writes field descriptors
  ASSERT_TRUE(table.Sync());

  // SetTextUTF16 after field descriptors written fails
  {
    WithQuietHandler quiet;
    EXPECT_FALSE(table.SetTextUTF16());
  }

  // Feature creation error: wrong raw fields size
  {
    std::vector<OGRField> bad_fields = MakeUnsetFields(5);
    WithQuietHandler quiet;
    EXPECT_FALSE(table.CreateFeature(bad_fields, nullptr));
  }

  // Feature creation error: non-nullable field unset/null
  {
    std::vector<OGRField> raw_fields = MakeUnsetFields(table.GetFieldCount());
    WithQuietHandler quiet;
    EXPECT_FALSE(table.CreateFeature(raw_fields, nullptr));
  }

  // Feature creation error: invalid UUID string
  {
    std::vector<OGRField> raw_fields = MakeUnsetFields(table.GetFieldCount());
    char req_str[] = "Required text";
    raw_fields[2].String = req_str;
    char short_uuid[] = "{invalid}";
    raw_fields[14].String = short_uuid;
    char valid_guid[] = "{12345678-1234-1234-1234-123456789abc}";
    raw_fields[15].String = valid_guid;
    WithQuietHandler quiet;
    EXPECT_FALSE(table.CreateFeature(raw_fields, nullptr));
  }

  // Valid feature creation with all types populated
  {
    std::vector<OGRField> raw_fields = MakeUnsetFields(table.GetFieldCount());
    char u16_str[] = "UTF16 text";
    char req_str[] = "Required UTF16 text";
    char xml_str[] = "<root>test</root>";
    char guid_str1[] = "{12345678-1234-1234-1234-123456789abc}";
    char guid_str2[] = "{87654321-4321-4321-4321-cba987654321}";
    GByte bin_data[] = {0x01, 0x02, 0x03, 0x04};

    raw_fields[1].String = u16_str;
    raw_fields[2].String = req_str;
    raw_fields[3].String = xml_str;
    raw_fields[4].Integer = 16;
    raw_fields[5].Integer = 32000;
    raw_fields[6].Integer64 = 64000000000LL;
    raw_fields[7].Real = 3.14f;
    raw_fields[8].Real = 2.718281828459;

    std::memset(&raw_fields[9], 0, sizeof(OGRField));
    raw_fields[9].Date.Year = 2026;
    raw_fields[9].Date.Month = 11;
    raw_fields[9].Date.Day = 3;

    std::memset(&raw_fields[10], 0, sizeof(OGRField));
    raw_fields[10].Date.Year = 2026;
    raw_fields[10].Date.Month = 11;
    raw_fields[10].Date.Day = 3;
    raw_fields[10].Date.Hour = 12;
    raw_fields[10].Date.Minute = 34;
    raw_fields[10].Date.Second = 56.789f;

    std::memset(&raw_fields[11], 0, sizeof(OGRField));
    raw_fields[11].Date.Hour = 10;
    raw_fields[11].Date.Minute = 20;
    raw_fields[11].Date.Second = 30.0f;

    std::memset(&raw_fields[12], 0, sizeof(OGRField));
    raw_fields[12].Date.Year = 2026;
    raw_fields[12].Date.Month = 11;
    raw_fields[12].Date.Day = 3;
    raw_fields[12].Date.Hour = 14;
    raw_fields[12].Date.Minute = 0;
    raw_fields[12].Date.Second = 0.0f;
    raw_fields[12].Date.TZFlag = 104;  // UTC+1

    raw_fields[13].Binary.nCount = 4;
    raw_fields[13].Binary.paData = bin_data;

    raw_fields[14].String = guid_str1;
    raw_fields[15].String = guid_str2;

    int nFID = 0;
    EXPECT_TRUE(table.CreateFeature(raw_fields, nullptr, &nFID));
    EXPECT_EQ(nFID, 1);

    // Second feature with TZFlag = 0 and empty binary
    nFID = 0;
    raw_fields[12].Date.TZFlag = 0;
    raw_fields[13].Binary.nCount = 0;
    raw_fields[13].Binary.paData = nullptr;
    EXPECT_TRUE(table.CreateFeature(raw_fields, nullptr, &nFID));
    EXPECT_EQ(nFID, 2);

    // Third feature with nullable fields unset
    nFID = 0;
    std::vector<OGRField> sparse_fields =
        MakeUnsetFields(table.GetFieldCount());
    sparse_fields[2].String = req_str;
    sparse_fields[15].String = guid_str2;
    EXPECT_TRUE(table.CreateFeature(sparse_fields, nullptr, &nFID));
    EXPECT_EQ(nFID, 3);

    // Fourth feature with large string exceeding m_nHeaderBufferMaxSize
    nFID = 0;
    std::vector<OGRField> big_fields = MakeUnsetFields(table.GetFieldCount());
    std::string big_str(5000, 'A');
    big_fields[2].String = const_cast<char*>(big_str.c_str());
    big_fields[15].String = guid_str2;
    EXPECT_TRUE(table.CreateFeature(big_fields, nullptr, &nFID));
    EXPECT_EQ(nFID, 4);

    EXPECT_TRUE(table.CreateIndex("idx_str", "str_req"));
  }

  ASSERT_TRUE(table.Sync());
  table.Close();

  // Read-only tests
  FileGDBTable ro_table;
  ASSERT_TRUE(ro_table.Open(table_path.c_str(), false));
  EXPECT_TRUE(ro_table.Sync());

  std::vector<OGRField> dummy = MakeUnsetFields(ro_table.GetFieldCount());
  EXPECT_FALSE(ro_table.CreateFeature(dummy, nullptr));
  EXPECT_FALSE(ro_table.UpdateFeature(1, dummy, nullptr));
  EXPECT_FALSE(ro_table.DeleteFeature(1));
  EXPECT_FALSE(ro_table.Repack(nullptr, nullptr));
}

TEST_F(FileGDBTableWriteTest, AllGeometryTypesEncoding) {
  const std::string table_path = std::string(kTestDir) + "/geoms.gdbtable";

  auto test_geom_table = [&](FileGDBTableGeometryType eGeomType, bool bZ,
                             bool bM, const std::vector<std::string>& wkts) {
    std::string path = table_path + "_" +
                       std::to_string(static_cast<int>(eGeomType)) +
                       (bZ ? "_z" : "") + (bM ? "_m" : "") + ".gdbtable";
    FileGDBTable tbl;
    ASSERT_TRUE(tbl.Create(path.c_str(), 4, eGeomType, bZ, bM));

    ASSERT_TRUE(tbl.CreateField(std::make_unique<FileGDBField>(
        "OBJECTID", "", FGFT_OBJECTID, false, true, false, 0,
        FileGDBField::UNSET_FIELD)));

    std::vector<double> grid_res = {10.0, 100.0};
    auto geom_field = std::make_unique<FileGDBGeomField>(
        "Shape", "", true, "POINT", -1000.0, -1000.0, 10000.0, 0.001, grid_res);
    if (bZ) {
      geom_field->SetZOriginScaleTolerance(-1000.0, 10000.0, 0.001);
    }
    if (bM) {
      geom_field->SetMOriginScaleTolerance(-1000.0, 10000.0, 0.001);
    }
    ASSERT_TRUE(tbl.CreateField(std::move(geom_field)));
    ASSERT_TRUE(tbl.Sync());

    std::vector<OGRField> raw_fields = MakeUnsetFields(tbl.GetFieldCount());

    for (const auto& wkt : wkts) {
      OGRGeometry* geom = nullptr;
      ASSERT_EQ(OGRERR_NONE,
                OGRGeometryFactory::createFromWkt(wkt.c_str(), nullptr, &geom));
      ASSERT_NE(geom, nullptr);
      EXPECT_TRUE(tbl.CreateFeature(raw_fields, geom))
          << "Failed to encode WKT: " << wkt;
      OGRGeometryFactory::destroyGeometry(geom);
    }

    // Also test null geometry on nullable geometry field
    EXPECT_TRUE(tbl.CreateFeature(raw_fields, nullptr));

    // Test RecomputeExtent when features exist
    tbl.RecomputeExtent();

    ASSERT_TRUE(tbl.Sync());
  };

  // 1. Point
  test_geom_table(FGTGT_POINT, false, false, {"POINT (1 2)", "POINT EMPTY"});
  test_geom_table(FGTGT_POINT, true, false,
                  {"POINT Z (1 2 3)", "POINT Z EMPTY"});
  test_geom_table(FGTGT_POINT, false, true,
                  {"POINT M (1 2 4)", "POINT M EMPTY"});
  test_geom_table(FGTGT_POINT, true, true,
                  {"POINT ZM (1 2 3 4)", "POINT ZM EMPTY"});

  // Point out-of-range encoding error
  {
    std::string path = std::string(kTestDir) + "/oor_point.gdbtable";
    FileGDBTable tbl;
    ASSERT_TRUE(tbl.Create(path.c_str(), 4, FGTGT_POINT, false, false));
    ASSERT_TRUE(tbl.CreateField(std::make_unique<FileGDBField>(
        "OBJECTID", "", FGFT_OBJECTID, false, true, false, 0,
        FileGDBField::UNSET_FIELD)));
    std::vector<double> grid_res = {10.0};
    auto geom_field = std::make_unique<FileGDBGeomField>(
        "Shape", "", false, "POINT", 0.0, 0.0, 10000.0, 0.001, grid_res);
    ASSERT_TRUE(tbl.CreateField(std::move(geom_field)));
    ASSERT_TRUE(tbl.Sync());

    std::vector<OGRField> raw_fields = MakeUnsetFields(tbl.GetFieldCount());

    // Null geometry on non-nullable field fails
    {
      WithQuietHandler quiet;
      EXPECT_FALSE(tbl.CreateFeature(raw_fields, nullptr));
    }

    // Negative coordinate < XOrigin fails encoding
    OGRPoint pt(-10.0, 5.0);
    {
      WithQuietHandler quiet;
      EXPECT_FALSE(tbl.CreateFeature(raw_fields, &pt));
    }
  }

  // 2. MultiPoint
  test_geom_table(FGTGT_MULTIPOINT, false, false,
                  {"MULTIPOINT ((1 2), (3 4))", "MULTIPOINT EMPTY"});
  test_geom_table(FGTGT_MULTIPOINT, true, false,
                  {"MULTIPOINT Z ((1 2 3), (4 5 6))"});
  test_geom_table(FGTGT_MULTIPOINT, false, true,
                  {"MULTIPOINT M ((1 2 3), (4 5 6))"});
  test_geom_table(FGTGT_MULTIPOINT, true, true,
                  {"MULTIPOINT ZM ((1 2 3 4), (5 6 7 8))"});

  // 3. LineString & Curves
  test_geom_table(FGTGT_LINE, false, false,
                  {"LINESTRING (0 0, 1 1, 2 0)", "LINESTRING EMPTY"});
  test_geom_table(FGTGT_LINE, true, false, {"LINESTRING Z (0 0 1, 1 1 2)"});
  test_geom_table(FGTGT_LINE, false, true, {"LINESTRING M (0 0 1, 1 1 2)"});
  test_geom_table(
      FGTGT_LINE, true, true,
      {"LINESTRING ZM (0 0 1 2, 1 1 3 4)",
       "MULTILINESTRING ZM ((0 0 1 2, 1 1 3 4), (2 2 5 6, 3 3 7 8))",
       "CIRCULARSTRING ZM (0 0 0 0, 1 1 1 1, 2 0 2 2)",
       "COMPOUNDCURVE ZM ((0 0 0 0, 1 0 1 1), CIRCULARSTRING ZM (1 0 1 1, 2 1 "
       "2 2, 3 0 3 3), (3 0 3 3, 4 0 4 4))",
       "MULTICURVE ZM ((0 0 0 0, 1 1 1 1), CIRCULARSTRING ZM (2 0 0 0, 3 1 1 "
       "1, 4 0 2 2))"});

  // 4. Polygon, CurvePolygon & MultiSurface
  test_geom_table(
      FGTGT_POLYGON, false, false,
      {"POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), (2 2, 8 2, 8 8, 2 8, 2 2))",
       "POLYGON EMPTY"});
  test_geom_table(FGTGT_POLYGON, true, false,
                  {"POLYGON Z ((0 0 1, 10 0 1, 10 10 1, 0 10 1, 0 0 1))"});
  test_geom_table(FGTGT_POLYGON, false, true,
                  {"POLYGON M ((0 0 1, 10 0 1, 10 10 1, 0 10 1, 0 0 1))"});
  test_geom_table(
      FGTGT_POLYGON, true, true,
      {"MULTIPOLYGON ZM (((0 0 1 2, 0 10 1 2, 10 10 1 2, 10 0 1 2, 0 0 1 2)), "
       "((20 20 1 2, 30 20 1 2, 30 30 1 2, 20 20 1 2)))",
       // CW exterior compound curve + hole
       "CURVEPOLYGON ZM (COMPOUNDCURVE ZM ((0 0 1 2, 0 10 1 2), CIRCULARSTRING "
       "ZM (0 10 1 2, 5 15 1 2, 10 10 1 2), (10 10 1 2, 10 0 1 2, 0 0 1 2)), "
       "(2 2 1 2, 8 2 1 2, 8 8 1 2, 2 8 1 2, 2 2 1 2))",
       // CCW exterior compound curve (triggers bReverseOrder)
       "CURVEPOLYGON ZM (COMPOUNDCURVE ZM ((0 0 1 2, 10 0 1 2, 10 10 1 2), "
       "CIRCULARSTRING ZM (10 10 1 2, 5 15 1 2, 0 10 1 2), (0 10 1 2, 0 0 1 "
       "2)))",
       // CW CircularString ring
       "CURVEPOLYGON ZM (CIRCULARSTRING ZM (0 0 1 2, 0 4 1 2, 4 4 1 2, 4 0 1 "
       "2, 0 0 1 2))",
       // CCW CircularString ring (triggers bReverseOrder)
       "CURVEPOLYGON ZM (CIRCULARSTRING ZM (0 0 1 2, 4 0 1 2, 4 4 1 2, 0 4 1 "
       "2, 0 0 1 2))",
       "MULTISURFACE ZM (((0 0 1 2, 0 5 1 2, 5 5 1 2, 5 0 1 2, 0 0 1 2)))"});

  // 5. MultiPatch / TIN / PolyhedralSurface
  test_geom_table(
      FGTGT_MULTIPATCH, true, false,
      {"TIN Z EMPTY",
       "TIN Z (((0 0 0, 0 1 1, 1 1 2, 0 0 0)), ((0 0 0, 1 1 2, 1 0 3, 0 0 0)))",
       "POLYHEDRALSURFACE Z (((0 0 0, 0 1 0, 1 1 0, 1 0 0, 0 0 0)), ((1 0 0, 1 "
       "1 0, 2 1 0, 2 0 0, 1 0 0)))"});

  // 6. Unsupported geometry type
  {
    std::string path = std::string(kTestDir) + "/unsupported.gdbtable";
    FileGDBTable tbl;
    ASSERT_TRUE(tbl.Create(path.c_str(), 4, FGTGT_POLYGON, false, false));
    ASSERT_TRUE(tbl.CreateField(std::make_unique<FileGDBField>(
        "OBJECTID", "", FGFT_OBJECTID, false, true, false, 0,
        FileGDBField::UNSET_FIELD)));
    std::vector<double> grid_res = {10.0};
    auto geom_field = std::make_unique<FileGDBGeomField>(
        "Shape", "", true, "POLYGON", 0.0, 0.0, 10000.0, 0.001, grid_res);
    ASSERT_TRUE(tbl.CreateField(std::move(geom_field)));
    ASSERT_TRUE(tbl.Sync());

    std::vector<OGRField> raw_fields = MakeUnsetFields(tbl.GetFieldCount());
    OGRTriangle tri;
    WithQuietHandler quiet;
    EXPECT_FALSE(tbl.CreateFeature(raw_fields, &tri));
  }

  // 7. RecomputeExtent when table has no features (sets ESRI_NAN)
  {
    std::string path = std::string(kTestDir) + "/empty_extent.gdbtable";
    FileGDBTable tbl;
    ASSERT_TRUE(tbl.Create(path.c_str(), 4, FGTGT_POINT, false, false));
    ASSERT_TRUE(tbl.CreateField(std::make_unique<FileGDBField>(
        "OBJECTID", "", FGFT_OBJECTID, false, true, false, 0,
        FileGDBField::UNSET_FIELD)));
    std::vector<double> grid_res = {10.0};
    auto geom_field = std::make_unique<FileGDBGeomField>(
        "Shape", "", true, "POINT", 0.0, 0.0, 10000.0, 0.001, grid_res);
    ASSERT_TRUE(tbl.CreateField(std::move(geom_field)));
    ASSERT_TRUE(tbl.Sync());

    tbl.RecomputeExtent();
    auto* gf = cpl::down_cast<const FileGDBGeomField*>(tbl.GetField(1));
    EXPECT_TRUE(std::isnan(gf->GetXMin()));
  }

  // 8. CreateSpatialIndex and Sync to flush
  // m_bDirtyGeomFieldSpatialIndexGridRes
  {
    std::string path = std::string(kTestDir) + "/spatial_idx.gdbtable";
    FileGDBTable tbl;
    ASSERT_TRUE(tbl.Create(path.c_str(), 4, FGTGT_POINT, false, false));
    ASSERT_TRUE(tbl.CreateField(std::make_unique<FileGDBField>(
        "OBJECTID", "", FGFT_OBJECTID, false, true, false, 0,
        FileGDBField::UNSET_FIELD)));
    std::vector<double> grid_res = {10.0};
    auto geom_field = std::make_unique<FileGDBGeomField>(
        "Shape", "", true, "POINT", 0.0, 0.0, 10000.0, 0.001, grid_res);
    ASSERT_TRUE(tbl.CreateField(std::move(geom_field)));
    ASSERT_TRUE(tbl.Sync());

    std::vector<OGRField> raw_fields = MakeUnsetFields(tbl.GetFieldCount());
    OGRPoint pt(100.0, 200.0);
    EXPECT_TRUE(tbl.CreateFeature(raw_fields, &pt));
    EXPECT_TRUE(tbl.Sync());

    CPLSetThreadLocalConfigOption("OPENFILEGDB_GRID_SIZE", "50.0");
    auto cleanup = absl::MakeCleanup([] {
      CPLSetThreadLocalConfigOption("OPENFILEGDB_GRID_SIZE", nullptr);
    });
    EXPECT_TRUE(tbl.CreateSpatialIndex());
    EXPECT_TRUE(tbl.Sync());
  }
}

TEST_F(FileGDBTableWriteTest, SparseTablXAndUpdateAndDeleteFeature) {
  const std::string table_path = std::string(kTestDir) + "/sparse.gdbtable";

  FileGDBTable tbl;
  ASSERT_TRUE(tbl.Create(table_path.c_str(), 4, FGTGT_NONE, false, false));

  ASSERT_TRUE(tbl.CreateField(
      std::make_unique<FileGDBField>("OBJECTID", "", FGFT_OBJECTID, false, true,
                                     false, 0, FileGDBField::UNSET_FIELD)));

  ASSERT_TRUE(tbl.CreateField(
      std::make_unique<FileGDBField>("str_fld", "", FGFT_STRING, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD)));

  ASSERT_TRUE(tbl.Sync());

  std::vector<OGRField> raw_fields = MakeUnsetFields(tbl.GetFieldCount());
  char str1[] = "Feature 1";
  raw_fields[1].String = str1;

  int fid = 1;
  EXPECT_TRUE(tbl.CreateFeature(raw_fields, nullptr, &fid));
  EXPECT_EQ(fid, 1);

  // Creating duplicate FID 1 fails
  fid = 1;
  {
    WithQuietHandler quiet;
    EXPECT_FALSE(tbl.CreateFeature(raw_fields, nullptr, &fid));
  }

  // Allocate sparse block map by writing large FID = 3000
  fid = 3000;
  char str3000[] = "Feature 3000";
  raw_fields[1].String = str3000;
  EXPECT_TRUE(tbl.CreateFeature(raw_fields, nullptr, &fid));
  EXPECT_EQ(fid, 3000);

  // Extend sparse block map with FID = 5000
  fid = 5000;
  char str5000[] = "Feature 5000";
  raw_fields[1].String = str5000;
  EXPECT_TRUE(tbl.CreateFeature(raw_fields, nullptr, &fid));
  EXPECT_EQ(fid, 5000);

  // Insert a new page between allocated blocks with FID = 1500
  fid = 1500;
  char str1500[] = "Feature 1500";
  raw_fields[1].String = str1500;
  EXPECT_TRUE(tbl.CreateFeature(raw_fields, nullptr, &fid));
  EXPECT_EQ(fid, 1500);

  // Sequential access after block (iBlock >= m_nCountBlocksBeforeIBlockIdx)
  fid = 1501;
  char str1501[] = "Feature 1501";
  raw_fields[1].String = str1501;
  EXPECT_TRUE(tbl.CreateFeature(raw_fields, nullptr, &fid));
  EXPECT_EQ(fid, 1501);

  // Access before block (iBlock < m_nCountBlocksBeforeIBlockIdx)
  fid = 2;
  char str2[] = "Feature 2";
  raw_fields[1].String = str2;
  EXPECT_TRUE(tbl.CreateFeature(raw_fields, nullptr, &fid));
  EXPECT_EQ(fid, 2);

  // Set m_nCountBlocksBeforeIBlockIdx to block 2 with FID 3001, then access
  // already-allocated block 1 with FID 1502
  fid = 3001;
  raw_fields[1].String = str2;
  EXPECT_TRUE(tbl.CreateFeature(raw_fields, nullptr, &fid));
  EXPECT_EQ(fid, 3001);

  fid = 1502;
  EXPECT_TRUE(tbl.CreateFeature(raw_fields, nullptr, &fid));
  EXPECT_EQ(fid, 1502);

  // UpdateFeature:
  // 1. Non-existent FID
  EXPECT_FALSE(tbl.UpdateFeature(99999, raw_fields, nullptr));

  // 2. In-place rewrite (shorter string)
  char str_short[] = "F1";
  raw_fields[1].String = str_short;
  EXPECT_TRUE(tbl.UpdateFeature(1, raw_fields, nullptr));

  // 3. Feature grows larger (appends to end, adds old slot to freelist)
  std::string long_str(500, 'A');
  raw_fields[1].String = const_cast<char*>(long_str.c_str());
  EXPECT_TRUE(tbl.UpdateFeature(1, raw_fields, nullptr));

  // 4. Update with freelist slot reuse:
  // Create a 6000-byte feature and delete it so 6000 bytes is available in
  // freelist.
  fid = 0;
  std::string str_6000(6000, 'B');
  raw_fields[1].String = const_cast<char*>(str_6000.c_str());
  EXPECT_TRUE(tbl.CreateFeature(raw_fields, nullptr, &fid));
  int big_fid = fid;
  EXPECT_TRUE(tbl.DeleteFeature(big_fid));

  // Update FID 2 to 5500 bytes (fits in the 6000-byte freelist slot)
  std::string str_5500(5500, 'C');
  raw_fields[1].String = const_cast<char*>(str_5500.c_str());
  EXPECT_TRUE(tbl.UpdateFeature(2, raw_fields, nullptr));

  // Update FID 2 to 7000 bytes (exceeds m_nHeaderBufferMaxSize)
  std::string str_7000(7000, 'D');
  raw_fields[1].String = const_cast<char*>(str_7000.c_str());
  EXPECT_TRUE(tbl.UpdateFeature(2, raw_fields, nullptr));

  // DeleteFeature:
  // 1. Non-existent FID
  EXPECT_FALSE(tbl.DeleteFeature(99999));

  // 2. Valid delete of FID 1500
  EXPECT_TRUE(tbl.DeleteFeature(1500));

  // 3. Create feature reusing freelist slot
  fid = 0;
  char str_new[] = "New feature reusing freelist";
  raw_fields[1].String = str_new;
  EXPECT_TRUE(tbl.CreateFeature(raw_fields, nullptr, &fid));

  ASSERT_TRUE(tbl.Sync());

  // Test maximum file size reached for nTablxOffsetSize = 1 (255 bytes limit)
  {
    const std::string tiny_path =
        std::string(kTestDir) + "/tiny_offset.gdbtable";
    FileGDBTable tiny_tbl;
    ASSERT_TRUE(
        tiny_tbl.Create(tiny_path.c_str(), 1, FGTGT_NONE, false, false));
    ASSERT_TRUE(tiny_tbl.CreateField(std::make_unique<FileGDBField>(
        "OBJECTID", "", FGFT_OBJECTID, false, true, false, 0,
        FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(tiny_tbl.CreateField(
        std::make_unique<FileGDBField>("s", "", FGFT_STRING, true, false, true,
                                       0, FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(tiny_tbl.Sync());

    std::vector<OGRField> flds = MakeUnsetFields(tiny_tbl.GetFieldCount());
    char short_s[] = "a";
    flds[1].String = short_s;
    int tiny_fid = 0;
    EXPECT_TRUE(tiny_tbl.CreateFeature(flds, nullptr, &tiny_fid));

    std::string over_limit(300, 'X');
    flds[1].String = const_cast<char*>(over_limit.c_str());
    {
      WithQuietHandler quiet;
      EXPECT_FALSE(tiny_tbl.CreateFeature(flds, nullptr));
      EXPECT_FALSE(tiny_tbl.UpdateFeature(tiny_fid, flds, nullptr));
    }
  }
}

TEST_F(FileGDBTableWriteTest, WholeFileRewriterAndRepackModes) {
  auto create_populated_table =
      [](const std::string& path) -> std::unique_ptr<FileGDBTable> {
    auto tbl = std::make_unique<FileGDBTable>();
    if (!tbl->Create(path.c_str(), 4, FGTGT_NONE, false, false)) return nullptr;
    if (!tbl->CreateField(std::make_unique<FileGDBField>(
            "OBJECTID", "", FGFT_OBJECTID, false, true, false, 0,
            FileGDBField::UNSET_FIELD)))
      return nullptr;
    if (!tbl->CreateField(
            std::make_unique<FileGDBField>("val", "", FGFT_STRING, true, false,
                                           true, 0, FileGDBField::UNSET_FIELD)))
      return nullptr;
    if (!tbl->Sync()) return nullptr;

    std::vector<OGRField> raw_fields = MakeUnsetFields(tbl->GetFieldCount());
    for (int i = 1; i <= 4; ++i) {
      std::string s = "Feature " + std::to_string(i);
      raw_fields[1].String = const_cast<char*>(s.c_str());
      if (!tbl->CreateFeature(raw_fields, nullptr)) return nullptr;
    }
    if (!tbl->Sync()) return nullptr;
    return tbl;
  };

  // Case 1: Already compacted table
  {
    const std::string path = std::string(kTestDir) + "/repack1.gdbtable";
    auto tbl = create_populated_table(path);
    ASSERT_NE(tbl, nullptr);
    EXPECT_TRUE(tbl->Repack(GDALDummyProgress, nullptr));
  }

  // Case 2: Deleted feature at end of file (truncation path without rewriter)
  {
    const std::string path = std::string(kTestDir) + "/repack2.gdbtable";
    auto tbl = create_populated_table(path);
    ASSERT_NE(tbl, nullptr);
    EXPECT_TRUE(tbl->DeleteFeature(4));
    EXPECT_TRUE(tbl->Repack(GDALDummyProgress, nullptr));
  }

  // Case 3: Middle feature deleted -> triggers WholeFileRewriter
  // Test progress cancellation during rewrite
  {
    const std::string path = std::string(kTestDir) + "/repack3_cancel.gdbtable";
    auto tbl = create_populated_table(path);
    ASSERT_NE(tbl, nullptr);
    EXPECT_TRUE(tbl->DeleteFeature(2));

    // Cancel on rewrite (progress > 0.2)
    auto cancel_rewrite = [](double dfComplete, const char*, void*) -> int {
      return dfComplete > 0.2 ? 0 : 1;
    };
    EXPECT_FALSE(tbl->Repack(cancel_rewrite, nullptr));
  }
  {
    const std::string path =
        std::string(kTestDir) + "/repack3_success.gdbtable";
    auto tbl = create_populated_table(path);
    ASSERT_NE(tbl, nullptr);
    EXPECT_TRUE(tbl->DeleteFeature(2));

    // Normal successful repack
    EXPECT_TRUE(tbl->Repack(GDALDummyProgress, nullptr));
  }

  // Case 3b: WholeFileRewriter tmp file creation errors
  {
    const std::string path =
        std::string(kTestDir) + "/repack3_tmp_fail.gdbtable";
    auto tbl = create_populated_table(path);
    ASSERT_NE(tbl, nullptr);
    EXPECT_TRUE(tbl->DeleteFeature(2));

    // Fail creating _compress.gdbtable
    std::string tmp_table_dir =
        CPLResetExtensionSafe(path.c_str(), "_compress.gdbtable");
    VSIMkdir(tmp_table_dir.c_str(), 0755);
    {
      WithQuietHandler quiet;
      EXPECT_FALSE(tbl->Repack(GDALDummyProgress, nullptr));
    }
    VSIRmdir(tmp_table_dir.c_str());

    // Fail creating _compress.gdbtablx
    std::string tmp_tablx_dir =
        CPLResetExtensionSafe(path.c_str(), "_compress.gdbtablx");
    VSIMkdir(tmp_tablx_dir.c_str(), 0755);
    {
      WithQuietHandler quiet;
      EXPECT_FALSE(tbl->Repack(GDALDummyProgress, nullptr));
    }
    VSIRmdir(tmp_tablx_dir.c_str());
  }

  // Case 4: OPENFILEGDB_MODIFY_IN_PLACE=YES
  {
    CPLSetThreadLocalConfigOption("OPENFILEGDB_MODIFY_IN_PLACE", "YES");
    auto cleanup = absl::MakeCleanup([] {
      CPLSetThreadLocalConfigOption("OPENFILEGDB_MODIFY_IN_PLACE", nullptr);
    });

    const std::string path = std::string(kTestDir) + "/repack_inplace.gdbtable";
    auto tbl = create_populated_table(path);
    ASSERT_NE(tbl, nullptr);
    EXPECT_TRUE(tbl->DeleteFeature(2));

    // Test existing backup error in Begin()
    std::string backup_path =
        CPLResetExtensionSafe(path.c_str(), "_backup.gdbtable");
    VSILFILE* fp = VSIFOpenL(backup_path.c_str(), "wb");
    if (fp) VSIFCloseL(fp);
    {
      WithQuietHandler quiet;
      EXPECT_FALSE(tbl->Repack(GDALDummyProgress, nullptr));
    }
    VSIUnlink(backup_path.c_str());

    // Test _backup.gdbtablx copy failure
    std::string backup_tablx_dir =
        CPLResetExtensionSafe(path.c_str(), "_backup.gdbtablx");
    VSIMkdir(backup_tablx_dir.c_str(), 0755);
    {
      WithQuietHandler quiet;
      EXPECT_FALSE(tbl->Repack(GDALDummyProgress, nullptr));
    }
    VSIRmdir(backup_tablx_dir.c_str());

    // Cancel on rewrite to exercise Rollback in modify-in-place mode
    auto cancel_rewrite = [](double dfComplete, const char*, void*) -> int {
      return dfComplete > 0.2 ? 0 : 1;
    };
    EXPECT_FALSE(tbl->Repack(cancel_rewrite, nullptr));

    // Fresh table for successful modify-in-place repack
    const std::string path2 =
        std::string(kTestDir) + "/repack_inplace2.gdbtable";
    auto tbl2 = create_populated_table(path2);
    ASSERT_NE(tbl2, nullptr);
    EXPECT_TRUE(tbl2->DeleteFeature(2));
    EXPECT_TRUE(tbl2->Repack(GDALDummyProgress, nullptr));
  }

  // Case 5: OPENFILEGDB_SIMUL_WIN32=YES with OPENFILEGDB_MODIFY_IN_PLACE=NO
  {
    CPLSetThreadLocalConfigOption("OPENFILEGDB_MODIFY_IN_PLACE", "NO");
    CPLSetThreadLocalConfigOption("OPENFILEGDB_SIMUL_WIN32", "YES");
    auto cleanup = absl::MakeCleanup([] {
      CPLSetThreadLocalConfigOption("OPENFILEGDB_MODIFY_IN_PLACE", nullptr);
      CPLSetThreadLocalConfigOption("OPENFILEGDB_SIMUL_WIN32", nullptr);
    });

    const std::string path = std::string(kTestDir) + "/repack_win32.gdbtable";
    auto tbl = create_populated_table(path);
    ASSERT_NE(tbl, nullptr);
    EXPECT_TRUE(tbl->DeleteFeature(2));
    EXPECT_TRUE(tbl->Repack(GDALDummyProgress, nullptr));
  }

  // Case 6: Field descriptors relocated (ghost area at offset 40)
  {
    const std::string path = std::string(kTestDir) + "/repack_ghost.gdbtable";
    FileGDBTable tbl;
    ASSERT_TRUE(tbl.Create(path.c_str(), 4, FGTGT_NONE, false, false));
    ASSERT_TRUE(tbl.CreateField(std::make_unique<FileGDBField>(
        "OBJECTID", "", FGFT_OBJECTID, false, true, false, 0,
        FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(tbl.CreateField(
        std::make_unique<FileGDBField>("fld1", "", FGFT_STRING, true, false,
                                       true, 0, FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(tbl.Sync());

    std::vector<OGRField> raw_fields = MakeUnsetFields(tbl.GetFieldCount());
    char s1[] = "Hello";
    raw_fields[1].String = s1;
    EXPECT_TRUE(tbl.CreateFeature(raw_fields, nullptr));
    EXPECT_TRUE(tbl.Sync());

    // Add a field to relocate field descriptors after the feature
    ASSERT_TRUE(tbl.CreateField(
        std::make_unique<FileGDBField>("fld2", "", FGFT_INT32, true, false,
                                       true, 0, FileGDBField::UNSET_FIELD)));
    ASSERT_TRUE(tbl.Sync());

    // Repack recognizes ghost area and succeeds
    EXPECT_TRUE(tbl.Repack(GDALDummyProgress, nullptr));

    // Now corrupt offset 44 (so GDAL marker does not match)
    VSILFILE* fp = VSIFOpenL(path.c_str(), "rb+");
    ASSERT_NE(fp, nullptr);
    VSIFSeekL(fp, 44, SEEK_SET);
    char bad_magic[4] = {'B', 'A', 'D', '!'};
    VSIFWriteL(bad_magic, 1, 4, fp);
    VSIFCloseL(fp);

    FileGDBTable tbl2;
    ASSERT_TRUE(tbl2.Open(path.c_str(), true));
    EXPECT_TRUE(tbl2.Repack(GDALDummyProgress, nullptr));
  }
}

}  // namespace
}  // namespace autotest2
