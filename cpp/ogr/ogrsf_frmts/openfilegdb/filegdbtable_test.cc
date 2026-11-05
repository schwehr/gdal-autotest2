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
// Tests Open FileGDB table reading, headers, fields, indexes, spatial filter,
// and geometry converters (filegdbtable.cpp).

#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/filegdbtable.h"

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
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrpgeogeometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/filegdbtable_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using namespace OpenFileGDB;

constexpr GUInt32 EXT_SHAPE_Z_FLAG = 0x80000000U;
constexpr GUInt32 EXT_SHAPE_M_FLAG = 0x40000000U;
constexpr GUInt32 EXT_SHAPE_CURVE_FLAG = 0x20000000U;
constexpr GUInt32 EXT_SHAPE_SEGMENT_ARC = 1;
constexpr GUInt32 EXT_SHAPE_SEGMENT_BEZIER = 4;
constexpr GUInt32 EXT_SHAPE_SEGMENT_ELLIPSE = 5;

constexpr char kTestDir[] = "/vsimem/test_filegdbtable";

class FileGDBTableTest : public ::testing::Test {
 protected:
  void SetUp() override { VSIMkdir(kTestDir, 0755); }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

std::vector<OGRField> MakeUnsetFields(int count) {
  std::vector<OGRField> fields(count);
  for (int i = 0; i < count; ++i) {
    OGR_RawField_SetUnset(&fields[i]);
  }
  return fields;
}

// ---------------------------------------------------------------------------
// 1. Helper classes: FileGDBField, FileGDBGeomField, FileGDBRasterField,
//    FileGDBIndex, and Date/Time helpers.
// ---------------------------------------------------------------------------

TEST_F(FileGDBTableTest, FieldAndGeomFieldClasses) {
  FileGDBTable table;
  FileGDBField defaultField(&table);
  EXPECT_TRUE(OGR_RawField_IsUnset(defaultField.GetDefault()));
  EXPECT_FALSE(defaultField.IsHighPrecision());
  defaultField.SetHighPrecision();
  EXPECT_TRUE(defaultField.IsHighPrecision());

  OGRField strDef;
  strDef.String = CPLStrdup("def_val");
  FileGDBField customField("str_col", "alias_col", FGFT_STRING, true, false,
                           true, 60, strDef);
  CPLFree(strDef.String);

  EXPECT_EQ(customField.GetName(), "str_col");
  EXPECT_EQ(customField.GetAlias(), "alias_col");
  EXPECT_EQ(customField.GetType(), FGFT_STRING);
  EXPECT_TRUE(customField.IsNullable());
  EXPECT_FALSE(customField.IsRequired());
  EXPECT_TRUE(customField.IsEditable());
  EXPECT_EQ(customField.GetMaxWidth(), 60);
  ASSERT_NE(customField.GetDefault(), nullptr);
  EXPECT_STREQ(customField.GetDefault()->String, "def_val");

  customField.SetParent(&table);
  EXPECT_EQ(customField.HasIndex(), 0);
  EXPECT_EQ(customField.GetIndex(), nullptr);

  // FileGDBGeomField
  std::vector<double> gridRes = {1000.0, 100.0};
  FileGDBGeomField geomField("SHAPE", "Shape Alias", true, "WKT_STR", -400.0,
                             -400.0, 100000.0, 0.0001, gridRes);
  EXPECT_EQ(geomField.GetWKT(), "WKT_STR");
  EXPECT_DOUBLE_EQ(geomField.GetXOrigin(), -400.0);
  EXPECT_DOUBLE_EQ(geomField.GetYOrigin(), -400.0);
  EXPECT_DOUBLE_EQ(geomField.GetXYScale(), 100000.0);
  EXPECT_DOUBLE_EQ(geomField.GetXYTolerance(), 0.0001);
  EXPECT_EQ(geomField.GetSpatialIndexGridResolution(), gridRes);

  geomField.SetXYMinMax(10.0, 20.0, 30.0, 40.0);
  EXPECT_DOUBLE_EQ(geomField.GetXMin(), 10.0);
  EXPECT_DOUBLE_EQ(geomField.GetYMin(), 20.0);
  EXPECT_DOUBLE_EQ(geomField.GetXMax(), 30.0);
  EXPECT_DOUBLE_EQ(geomField.GetYMax(), 40.0);

  geomField.SetZOriginScaleTolerance(-100.0, 1000.0, 0.001);
  EXPECT_TRUE(geomField.HasZOriginScaleTolerance());
  EXPECT_DOUBLE_EQ(geomField.GetZOrigin(), -100.0);
  EXPECT_DOUBLE_EQ(geomField.GetZScale(), 1000.0);
  EXPECT_DOUBLE_EQ(geomField.GetZTolerance(), 0.001);

  geomField.SetZMinMax(-10.0, 50.0);
  EXPECT_DOUBLE_EQ(geomField.GetZMin(), -10.0);
  EXPECT_DOUBLE_EQ(geomField.GetZMax(), 50.0);

  geomField.SetMOriginScaleTolerance(0.0, 500.0, 0.002);
  EXPECT_TRUE(geomField.HasMOriginScaleTolerance());
  EXPECT_DOUBLE_EQ(geomField.GetMOrigin(), 0.0);
  EXPECT_DOUBLE_EQ(geomField.GetMScale(), 500.0);
  EXPECT_DOUBLE_EQ(geomField.GetMTolerance(), 0.002);

  geomField.SetMMinMax(1.0, 100.0);
  EXPECT_DOUBLE_EQ(geomField.GetMMin(), 1.0);
  EXPECT_DOUBLE_EQ(geomField.GetMMax(), 100.0);

  // FileGDBRasterField
  FileGDBRasterField rasterField(&table);
  EXPECT_EQ(rasterField.GetRasterColumnName(), "");
  EXPECT_EQ(rasterField.GetRasterType(), FileGDBRasterField::Type::EXTERNAL);
}

TEST_F(FileGDBTableTest, IndexClassAndMaxWidth) {
  FileGDBIndex index;
  EXPECT_EQ(index.GetIndexName(), "");
  EXPECT_EQ(index.GetExpression(), "");
  EXPECT_EQ(index.GetFieldName(), "");

  EXPECT_EQ(FileGDBIndex::GetFieldNameFromExpression("attrib"), "attrib");
  EXPECT_EQ(FileGDBIndex::GetFieldNameFromExpression("LOWER(my_field)"),
            "my_field");
  EXPECT_EQ(FileGDBIndex::GetFieldNameFromExpression("LOWER(my_field"),
            "LOWER(my_field");

  std::string tblPath = std::string(kTestDir) + "/index_widths.gdbtable";
  FileGDBTable table;
  ASSERT_TRUE(table.Create(tblPath.c_str(), 4, FGTGT_NONE, false, false));

  auto fldStr =
      std::make_unique<FileGDBField>("str_fld", "", FGFT_STRING, true, false,
                                     true, 42, FileGDBField::UNSET_FIELD);
  auto fldI16 =
      std::make_unique<FileGDBField>("i16_fld", "", FGFT_INT16, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD);
  auto fldI32 =
      std::make_unique<FileGDBField>("i32_fld", "", FGFT_INT32, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD);
  auto fldF32 =
      std::make_unique<FileGDBField>("f32_fld", "", FGFT_FLOAT32, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD);
  auto fldF64 =
      std::make_unique<FileGDBField>("f64_fld", "", FGFT_FLOAT64, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD);
  auto fldDt =
      std::make_unique<FileGDBField>("dt_fld", "", FGFT_DATETIME, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD);
  auto fldGuid =
      std::make_unique<FileGDBField>("guid_fld", "", FGFT_GUID, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD);
  auto fldGid =
      std::make_unique<FileGDBField>("gid_fld", "", FGFT_GLOBALID, false, true,
                                     false, 0, FileGDBField::UNSET_FIELD);
  auto fldI64 =
      std::make_unique<FileGDBField>("i64_fld", "", FGFT_INT64, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD);
  auto fldDate =
      std::make_unique<FileGDBField>("date_fld", "", FGFT_DATE, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD);
  auto fldTime =
      std::make_unique<FileGDBField>("time_fld", "", FGFT_TIME, true, false,
                                     true, 0, FileGDBField::UNSET_FIELD);
  auto fldDtOff = std::make_unique<FileGDBField>(
      "dtoff_fld", "", FGFT_DATETIME_WITH_OFFSET, true, false, true, 0,
      FileGDBField::UNSET_FIELD);

  ASSERT_TRUE(table.CreateField(std::move(fldStr)));
  ASSERT_TRUE(table.CreateField(std::move(fldI16)));
  ASSERT_TRUE(table.CreateField(std::move(fldI32)));
  ASSERT_TRUE(table.CreateField(std::move(fldF32)));
  ASSERT_TRUE(table.CreateField(std::move(fldF64)));
  ASSERT_TRUE(table.CreateField(std::move(fldDt)));
  ASSERT_TRUE(table.CreateField(std::move(fldGuid)));
  ASSERT_TRUE(table.CreateField(std::move(fldGid)));
  ASSERT_TRUE(table.CreateField(std::move(fldI64)));
  ASSERT_TRUE(table.CreateField(std::move(fldDate)));
  ASSERT_TRUE(table.CreateField(std::move(fldTime)));
  ASSERT_TRUE(table.CreateField(std::move(fldDtOff)));

  ASSERT_TRUE(table.CreateIndex("idx_str", "str_fld"));
  EXPECT_EQ(table.GetIndexCount(), 1);
  const FileGDBIndex* builtIdx = table.GetIndex(0);
  ASSERT_NE(builtIdx, nullptr);
  EXPECT_EQ(builtIdx->GetIndexName(), "idx_str");
  EXPECT_EQ(builtIdx->GetFieldName(), "str_fld");
  EXPECT_EQ(builtIdx->GetMaxWidthInBytes(&table), 2);

  FileGDBField* fldStrPtr = table.GetField(table.GetFieldIdx("str_fld"));
  ASSERT_NE(fldStrPtr, nullptr);
  EXPECT_EQ(fldStrPtr->HasIndex(), 1);
  EXPECT_EQ(fldStrPtr->GetIndex(), builtIdx);

  table.Close();
}

TEST_F(FileGDBTableTest, DateTimeHelpersAndESRITypes) {
  OGRField fld;

  // FileGDBDoubleDateToOGRDate
  // 25569.0 days = 1970/01/01 00:00:00
  EXPECT_TRUE(FileGDBDoubleDateToOGRDate(25569.0, false, &fld));
  EXPECT_EQ(fld.Date.Year, 1970);
  EXPECT_EQ(fld.Date.Month, 1);
  EXPECT_EQ(fld.Date.Day, 1);
  EXPECT_EQ(fld.Date.Hour, 0);
  EXPECT_EQ(fld.Date.Minute, 0);
  EXPECT_EQ(fld.Date.Second, 0);

  // High precision with fractional seconds
  EXPECT_TRUE(FileGDBDoubleDateToOGRDate(25569.50005, true, &fld));
  EXPECT_EQ(fld.Date.Hour, 12);

  // NaN / out of range
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_TRUE(FileGDBDoubleDateToOGRDate(
      std::numeric_limits<double>::quiet_NaN(), false, &fld));
  CPLPopErrorHandler();

  // FileGDBDoubleTimeToOGRTime
  // 0.5 days = 12:00:00
  EXPECT_TRUE(FileGDBDoubleTimeToOGRTime(0.5, &fld));
  EXPECT_EQ(fld.Date.Hour, 12);
  EXPECT_EQ(fld.Date.Minute, 0);
  EXPECT_NEAR(fld.Date.Second, 0.0f, 1e-3);

  // Invalid time < 0 or > 1.0 (86400s)
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_TRUE(FileGDBDoubleTimeToOGRTime(-0.1, &fld));
  EXPECT_TRUE(FileGDBDoubleTimeToOGRTime(2.0, &fld));
  CPLPopErrorHandler();

  // FileGDBDateTimeWithOffsetToOGRDate
  EXPECT_TRUE(FileGDBDateTimeWithOffsetToOGRDate(25569.0, 120, &fld));
  EXPECT_EQ(fld.Date.TZFlag, 100 + 120 / 15);

  // Invalid offset
  EXPECT_FALSE(FileGDBDateTimeWithOffsetToOGRDate(25569.0, 2000, &fld));

  // GetGeometryTypeFromESRI
  EXPECT_EQ(
      FileGDBOGRGeometryConverter::GetGeometryTypeFromESRI("esriGeometryPoint"),
      wkbPoint);
  EXPECT_EQ(FileGDBOGRGeometryConverter::GetGeometryTypeFromESRI(
                "esriGeometryMultipoint"),
            wkbMultiPoint);
  EXPECT_EQ(
      FileGDBOGRGeometryConverter::GetGeometryTypeFromESRI("esriGeometryLine"),
      wkbMultiLineString);
  EXPECT_EQ(FileGDBOGRGeometryConverter::GetGeometryTypeFromESRI(
                "esriGeometryPolyline"),
            wkbMultiLineString);
  EXPECT_EQ(FileGDBOGRGeometryConverter::GetGeometryTypeFromESRI(
                "esriGeometryPolygon"),
            wkbMultiPolygon);
  EXPECT_EQ(FileGDBOGRGeometryConverter::GetGeometryTypeFromESRI(
                "esriGeometryMultiPatch"),
            wkbUnknown);
  EXPECT_EQ(FileGDBOGRGeometryConverter::GetGeometryTypeFromESRI("esriUnknown"),
            wkbUnknown);
}

// ---------------------------------------------------------------------------
// 2. Geometry Converter: Point, MultiPoint, Line, Polygon, MultiPatch, Curves
// ---------------------------------------------------------------------------

TEST_F(FileGDBTableTest, GeometryConverterPointsAndMultiPoints) {
  std::vector<double> gridRes = {1000.0};
  FileGDBGeomField geomField("SHAPE", "", true, "", 0.0, 0.0, 100.0, 0.001,
                             gridRes);
  geomField.SetZOriginScaleTolerance(0.0, 100.0, 0.001);
  geomField.SetMOriginScaleTolerance(0.0, 100.0, 0.001);

  std::unique_ptr<FileGDBOGRGeometryConverter> converter(
      FileGDBOGRGeometryConverter::BuildConverter(&geomField));
  ASSERT_NE(converter, nullptr);

  // SHPT_NULL
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_NULL);
    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    EXPECT_EQ(converter->GetAsGeometry(&fld), nullptr);
  }

  // SHPT_POINT: x = 10.0, y = 20.0
  // quantized x = (10.0 - 0.0) * 100.0 + 1 = 1001
  // quantized y = (20.0 - 0.0) * 100.0 + 1 = 2001
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_POINT);
    WriteVarUInt(buf, 1001);
    WriteVarUInt(buf, 2001);
    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    std::unique_ptr<OGRGeometry> geom(converter->GetAsGeometry(&fld));
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(wkbFlatten(geom->getGeometryType()), wkbPoint);
    OGRPoint* pt = geom->toPoint();
    EXPECT_DOUBLE_EQ(pt->getX(), 10.0);
    EXPECT_DOUBLE_EQ(pt->getY(), 20.0);
  }

  // SHPT_POINT with NaN coords (x=0, y=0)
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_POINT);
    WriteVarUInt(buf, 0);
    WriteVarUInt(buf, 0);
    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    std::unique_ptr<OGRGeometry> geom(converter->GetAsGeometry(&fld));
    ASSERT_NE(geom, nullptr);
    OGRPoint* pt = geom->toPoint();
    EXPECT_TRUE(std::isnan(pt->getX()));
    EXPECT_TRUE(std::isnan(pt->getY()));
  }

  // SHPT_POINTZ and SHPT_POINTZM
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_POINTZM);
    WriteVarUInt(buf, 1001);  // X=10.0
    WriteVarUInt(buf, 2001);  // Y=20.0
    WriteVarUInt(buf, 301);   // Z=3.0 -> (301-1)/100 = 3.0
    WriteVarUInt(buf, 401);   // M=4.0 -> (401-1)/100 = 4.0
    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    std::unique_ptr<OGRGeometry> geom(converter->GetAsGeometry(&fld));
    ASSERT_NE(geom, nullptr);
    OGRPoint* pt = geom->toPoint();
    EXPECT_DOUBLE_EQ(pt->getX(), 10.0);
    EXPECT_DOUBLE_EQ(pt->getY(), 20.0);
    EXPECT_DOUBLE_EQ(pt->getZ(), 3.0);
    EXPECT_DOUBLE_EQ(pt->getM(), 4.0);
  }

  // SHPT_POINTZM with m=0 (NaN M)
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_POINTZM);
    WriteVarUInt(buf, 1001);
    WriteVarUInt(buf, 2001);
    WriteVarUInt(buf, 301);
    WriteVarUInt(buf, 0);  // m=0
    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    std::unique_ptr<OGRGeometry> geom(converter->GetAsGeometry(&fld));
    ASSERT_NE(geom, nullptr);
    OGRPoint* pt = geom->toPoint();
    EXPECT_TRUE(std::isnan(pt->getM()));
  }

  // SHPT_POINTM
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_POINTM);
    WriteVarUInt(buf, 1001);
    WriteVarUInt(buf, 2001);
    WriteVarUInt(buf, 501);  // M=5.0
    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    std::unique_ptr<OGRGeometry> geom(converter->GetAsGeometry(&fld));
    ASSERT_NE(geom, nullptr);
    OGRPoint* pt = geom->toPoint();
    EXPECT_DOUBLE_EQ(pt->getM(), 5.0);
  }

  // SHPT_MULTIPOINT empty
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_MULTIPOINT);
    WriteVarUInt(buf, 0);  // 0 points
    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    std::unique_ptr<OGRGeometry> geom(converter->GetAsGeometry(&fld));
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(geom->toMultiPoint()->getNumGeometries(), 0);
  }

  // SHPT_MULTIPOINTZM non-empty
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_MULTIPOINTZM);
    WriteVarUInt(buf, 2);  // 2 points
    // Skip 4 varuints (bbox)
    for (int i = 0; i < 4; ++i) WriteVarUInt(buf, 0);
    // XY delta points: dx0=1000, dy0=2000, dx1=500, dy1=500
    WriteVarInt(buf, 1000);
    WriteVarInt(buf, 2000);
    WriteVarInt(buf, 500);
    WriteVarInt(buf, 500);
    // Z deltas: dz0=100, dz1=50
    WriteVarInt(buf, 100);
    WriteVarInt(buf, 50);
    // M deltas: dm0=200, dm1=30
    WriteVarInt(buf, 200);
    WriteVarInt(buf, 30);

    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    std::unique_ptr<OGRGeometry> geom(converter->GetAsGeometry(&fld));
    ASSERT_NE(geom, nullptr);
    OGRMultiPoint* mp = geom->toMultiPoint();
    EXPECT_EQ(mp->getNumGeometries(), 2);
    EXPECT_TRUE(mp->Is3D());
    EXPECT_TRUE(mp->IsMeasured());
  }

  // Unhandled geom type
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, 99);
    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    EXPECT_EQ(converter->GetAsGeometry(&fld), nullptr);
  }
}

TEST_F(FileGDBTableTest, GeometryConverterPolylinesAndPolygons) {
  std::vector<double> gridRes = {1000.0};
  FileGDBGeomField geomField("SHAPE", "", true, "", 0.0, 0.0, 100.0, 0.001,
                             gridRes);
  geomField.SetZOriginScaleTolerance(0.0, 100.0, 0.001);
  geomField.SetMOriginScaleTolerance(0.0, 100.0, 0.001);

  std::unique_ptr<FileGDBOGRGeometryConverter> converter(
      FileGDBOGRGeometryConverter::BuildConverter(&geomField));
  ASSERT_NE(converter, nullptr);

  // SHPT_ARC empty
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_ARC);
    WriteVarUInt(buf, 0);  // 0 points
    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    std::unique_ptr<OGRGeometry> geom(converter->GetAsGeometry(&fld));
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(wkbFlatten(geom->getGeometryType()), wkbLineString);
    EXPECT_EQ(geom->toLineString()->getNumPoints(), 0);
  }

  // SHPT_ARCZM single part (2 points)
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_ARCZM);
    WriteVarUInt(buf, 2);  // 2 points
    WriteVarUInt(buf, 1);  // 1 part
    // Skip 4 varuints (bbox)
    for (int i = 0; i < 4; ++i) WriteVarUInt(buf, 0);
    // XY: dx0=1000, dy0=1000, dx1=1000, dy1=0
    WriteVarInt(buf, 1000);
    WriteVarInt(buf, 1000);
    WriteVarInt(buf, 1000);
    WriteVarInt(buf, 0);
    // Z: dz0=100, dz1=50
    WriteVarInt(buf, 100);
    WriteVarInt(buf, 50);
    // M: dz0=50, dz1=25
    WriteVarInt(buf, 50);
    WriteVarInt(buf, 25);

    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    std::unique_ptr<OGRGeometry> geom(converter->GetAsGeometry(&fld));
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(wkbFlatten(geom->getGeometryType()), wkbLineString);
    OGRLineString* ls = geom->toLineString();
    EXPECT_EQ(ls->getNumPoints(), 2);
    EXPECT_TRUE(ls->Is3D());
    EXPECT_TRUE(ls->IsMeasured());
    EXPECT_DOUBLE_EQ(ls->getX(0), 10.0);
    EXPECT_DOUBLE_EQ(ls->getX(1), 20.0);
  }

  // SHPT_ARC multipart (2 parts: 2 points and 2 points)
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_ARC);
    WriteVarUInt(buf, 4);  // 4 points total
    WriteVarUInt(buf, 2);  // 2 parts
    // Skip 4 varuints
    for (int i = 0; i < 4; ++i) WriteVarUInt(buf, 0);
    // Part 0 length = 2 points
    WriteVarUInt(buf, 2);
    // Part 1 length = 2 points (implicit)
    // 4 points XY
    for (int i = 0; i < 4; ++i) {
      WriteVarInt(buf, 100);
      WriteVarInt(buf, 100);
    }
    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    std::unique_ptr<OGRGeometry> geom(converter->GetAsGeometry(&fld));
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(wkbFlatten(geom->getGeometryType()), wkbMultiLineString);
    EXPECT_EQ(geom->toMultiLineString()->getNumGeometries(), 2);
  }

  // SHPT_POLYGON single ring (4 points: 0,0 -> 10,0 -> 10,10 -> 0,0)
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_POLYGON);
    WriteVarUInt(buf, 4);  // 4 points
    WriteVarUInt(buf, 1);  // 1 part
    for (int i = 0; i < 4; ++i) WriteVarUInt(buf, 0);
    // dx, dy deltas:
    // pt0: 0, 0
    WriteVarInt(buf, 0);
    WriteVarInt(buf, 0);
    // pt1: 1000, 0 (x=10, y=0)
    WriteVarInt(buf, 1000);
    WriteVarInt(buf, 0);
    // pt2: 0, 1000 (x=10, y=10)
    WriteVarInt(buf, 0);
    WriteVarInt(buf, 1000);
    // pt3: -1000, -1000 (x=0, y=0)
    WriteVarInt(buf, -1000);
    WriteVarInt(buf, -1000);

    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    std::unique_ptr<OGRGeometry> geom(converter->GetAsGeometry(&fld));
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(wkbFlatten(geom->getGeometryType()), wkbPolygon);
    OGRPolygon* poly = geom->toPolygon();
    EXPECT_EQ(poly->getExteriorRing()->getNumPoints(), 4);
  }

  // SHPT_POLYGON empty
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_POLYGON);
    WriteVarUInt(buf, 0);  // 0 points
    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    std::unique_ptr<OGRGeometry> geom(converter->GetAsGeometry(&fld));
    ASSERT_NE(geom, nullptr);
    EXPECT_TRUE(wkbFlatten(geom->getGeometryType()) == wkbPolygon ||
                wkbFlatten(geom->getGeometryType()) == wkbMultiPolygon);
  }

  // SHPT_MULTIPATCH empty and non-empty
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_MULTIPATCH);
    WriteVarUInt(buf, 0);  // 0 points
    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    std::unique_ptr<OGRGeometry> geom(converter->GetAsGeometry(&fld));
    ASSERT_NE(geom, nullptr);
    EXPECT_TRUE(wkbFlatten(geom->getGeometryType()) == wkbPolygon ||
                wkbFlatten(geom->getGeometryType()) == wkbMultiPolygon);
  }

  // MultiPatch non-empty (1 part triangle strip: 3 points)
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_MULTIPATCH);
    WriteVarUInt(buf, 3);  // 3 points
    WriteVarUInt(buf, 0);  // skip varuint for multipatch
    WriteVarUInt(buf, 1);  // 1 part
    for (int i = 0; i < 4; ++i) WriteVarUInt(buf, 0);
    // Part type = 0 (triangle strip)
    WriteVarUInt(buf, 0);
    // 3 points XY deltas
    WriteVarInt(buf, 0);
    WriteVarInt(buf, 0);
    WriteVarInt(buf, 100);
    WriteVarInt(buf, 0);
    WriteVarInt(buf, 0);
    WriteVarInt(buf, 100);
    // Z deltas
    WriteVarInt(buf, 10);
    WriteVarInt(buf, 10);
    WriteVarInt(buf, 10);

    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    std::unique_ptr<OGRGeometry> geom(converter->GetAsGeometry(&fld));
    ASSERT_NE(geom, nullptr);
  }

  // SHPT_ARCZM with nParts = 2
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_ARCZM);
    WriteVarUInt(buf, 4);                              // 4 points
    WriteVarUInt(buf, 2);                              // 2 parts
    WriteVarUInt(buf, 2);                              // panPointCount[0] = 2
    for (int i = 0; i < 4; ++i) WriteVarUInt(buf, 0);  // bbox
    // 4 points XY
    for (int i = 0; i < 4; ++i) {
      WriteVarInt(buf, 100);
      WriteVarInt(buf, 100);
    }
    // 4 Z deltas
    for (int i = 0; i < 4; ++i) WriteVarInt(buf, 10);
    // 4 M deltas
    for (int i = 0; i < 4; ++i) WriteVarInt(buf, 20);

    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    std::unique_ptr<OGRGeometry> geom(converter->GetAsGeometry(&fld));
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(wkbFlatten(geom->getGeometryType()), wkbMultiLineString);
  }

  // SHPT_ARCM with nParts = 2 and truncated M on part 2
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_ARCM);
    WriteVarUInt(buf, 4);                              // 4 points
    WriteVarUInt(buf, 2);                              // 2 parts
    WriteVarUInt(buf, 2);                              // panPointCount[0] = 2
    for (int i = 0; i < 4; ++i) WriteVarUInt(buf, 0);  // bbox
    for (int i = 0; i < 4; ++i) {
      WriteVarInt(buf, 100);
      WriteVarInt(buf, 100);
    }
    // Only 2 M deltas so part 2 M read encounters end of buffer
    WriteVarInt(buf, 20);
    WriteVarInt(buf, 20);

    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    std::unique_ptr<OGRGeometry> geom(converter->GetAsGeometry(&fld));
    ASSERT_NE(geom, nullptr);
  }

  // SHPT_POLYGONZM with nParts = 2 (outer ring + inner ring)
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_POLYGONZM);
    WriteVarUInt(buf, 8);                              // 8 points
    WriteVarUInt(buf, 2);                              // 2 parts
    WriteVarUInt(buf, 4);                              // panPointCount[0] = 4
    for (int i = 0; i < 4; ++i) WriteVarUInt(buf, 0);  // bbox
    // Outer ring (CW)
    WriteVarInt(buf, 0);
    WriteVarInt(buf, 0);
    WriteVarInt(buf, 0);
    WriteVarInt(buf, 1000);
    WriteVarInt(buf, 1000);
    WriteVarInt(buf, 0);
    WriteVarInt(buf, -1000);
    WriteVarInt(buf, -1000);
    // Inner ring (CCW)
    WriteVarInt(buf, 200);
    WriteVarInt(buf, 200);
    WriteVarInt(buf, 600);
    WriteVarInt(buf, 0);
    WriteVarInt(buf, 0);
    WriteVarInt(buf, 600);
    WriteVarInt(buf, -600);
    WriteVarInt(buf, -600);
    // 8 Z deltas
    for (int i = 0; i < 8; ++i) WriteVarInt(buf, 100);
    // 8 M deltas
    for (int i = 0; i < 8; ++i) WriteVarInt(buf, 200);

    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    std::unique_ptr<OGRGeometry> geom(converter->GetAsGeometry(&fld));
    ASSERT_NE(geom, nullptr);
    EXPECT_TRUE(wkbFlatten(geom->getGeometryType()) == wkbPolygon ||
                wkbFlatten(geom->getGeometryType()) == wkbMultiPolygon);
  }

  // SHPT_POLYGONM with nParts = 2 and truncated M
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_POLYGONM);
    WriteVarUInt(buf, 8);                              // 8 points
    WriteVarUInt(buf, 2);                              // 2 parts
    WriteVarUInt(buf, 4);                              // panPointCount[0] = 4
    for (int i = 0; i < 4; ++i) WriteVarUInt(buf, 0);  // bbox
    for (int i = 0; i < 8; ++i) {
      WriteVarInt(buf, 100);
      WriteVarInt(buf, 100);
    }
    // Only 4 M deltas
    for (int i = 0; i < 4; ++i) WriteVarInt(buf, 10);

    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    std::unique_ptr<OGRGeometry> geom(converter->GetAsGeometry(&fld));
    ASSERT_NE(geom, nullptr);
  }

  // SHPT_GENERALMULTIPATCH without Z (nParts = 2)
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_GENERALMULTIPATCH);
    WriteVarUInt(buf, 6);                              // 6 points
    WriteVarUInt(buf, 0);                              // skip varuint
    WriteVarUInt(buf, 2);                              // 2 parts
    WriteVarUInt(buf, 3);                              // panPointCount[0] = 3
    for (int i = 0; i < 4; ++i) WriteVarUInt(buf, 0);  // bbox
    WriteVarUInt(buf, 0);                              // part 0 type
    WriteVarUInt(buf, 0);                              // part 1 type
    for (int i = 0; i < 6; ++i) {
      WriteVarInt(buf, 50);
      WriteVarInt(buf, 50);
    }

    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    std::unique_ptr<OGRGeometry> geom(converter->GetAsGeometry(&fld));
    ASSERT_NE(geom, nullptr);
  }
}

TEST_F(FileGDBTableTest, GeometryConverterCurves) {
  std::vector<double> gridRes = {1000.0};
  FileGDBGeomField geomField("SHAPE", "", true, "", 0.0, 0.0, 100.0, 0.001,
                             gridRes);
  geomField.SetZOriginScaleTolerance(0.0, 100.0, 0.001);
  geomField.SetMOriginScaleTolerance(0.0, 100.0, 0.001);

  std::unique_ptr<FileGDBOGRGeometryConverter> converter(
      FileGDBOGRGeometryConverter::BuildConverter(&geomField));
  ASSERT_NE(converter, nullptr);

  // Line with Arc curve:
  // SHPT_GENERALPOLYLINE | EXT_SHAPE_CURVE_FLAG
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_GENERALPOLYLINE | EXT_SHAPE_CURVE_FLAG);
    WriteVarUInt(buf, 2);  // 2 points
    WriteVarUInt(buf, 1);  // 1 part
    WriteVarUInt(buf, 1);  // 1 curve
    for (int i = 0; i < 4; ++i) WriteVarUInt(buf, 0);

    // XY: 0,0 -> 10,0
    WriteVarInt(buf, 0);
    WriteVarInt(buf, 0);
    WriteVarInt(buf, 1000);
    WriteVarInt(buf, 0);

    // Curve descriptor: start index = 0, curve type = EXT_SHAPE_SEGMENT_ARC
    WriteVarUInt(buf, 0);
    WriteVarUInt(buf, EXT_SHAPE_SEGMENT_ARC);
    // Arc struct: 2 doubles (center x, y) + 1 uint32 (bits)
    WriteFloat64(buf, 5.0);
    WriteFloat64(buf, 0.0);
    WriteUInt32(buf, 0);

    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    std::unique_ptr<OGRGeometry> geom(converter->GetAsGeometry(&fld));
    ASSERT_NE(geom, nullptr);
  }

  // Curve with Bezier:
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_GENERALPOLYLINE | EXT_SHAPE_CURVE_FLAG);
    WriteVarUInt(buf, 2);  // 2 points
    WriteVarUInt(buf, 1);  // 1 part
    WriteVarUInt(buf, 1);  // 1 curve
    for (int i = 0; i < 4; ++i) WriteVarUInt(buf, 0);

    WriteVarInt(buf, 0);
    WriteVarInt(buf, 0);
    WriteVarInt(buf, 1000);
    WriteVarInt(buf, 0);

    WriteVarUInt(buf, 0);
    WriteVarUInt(buf, EXT_SHAPE_SEGMENT_BEZIER);
    // 4 doubles
    WriteFloat64(buf, 2.5);
    WriteFloat64(buf, 5.0);
    WriteFloat64(buf, 7.5);
    WriteFloat64(buf, 5.0);

    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    std::unique_ptr<OGRGeometry> geom(converter->GetAsGeometry(&fld));
    ASSERT_NE(geom, nullptr);
  }

  // Curve with Ellipse:
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_GENERALPOLYLINE | EXT_SHAPE_CURVE_FLAG);
    WriteVarUInt(buf, 2);  // 2 points
    WriteVarUInt(buf, 1);  // 1 part
    WriteVarUInt(buf, 1);  // 1 curve
    for (int i = 0; i < 4; ++i) WriteVarUInt(buf, 0);

    WriteVarInt(buf, 0);
    WriteVarInt(buf, 0);
    WriteVarInt(buf, 1000);
    WriteVarInt(buf, 0);

    WriteVarUInt(buf, 0);
    WriteVarUInt(buf, EXT_SHAPE_SEGMENT_ELLIPSE);
    // 5 doubles + 1 uint32
    WriteFloat64(buf, 5.0);
    WriteFloat64(buf, 0.0);
    WriteFloat64(buf, 0.0);
    WriteFloat64(buf, 1.0);
    WriteFloat64(buf, 0.5);
    WriteUInt32(buf, 0);

    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    std::unique_ptr<OGRGeometry> geom(converter->GetAsGeometry(&fld));
    ASSERT_NE(geom, nullptr);
  }

  // Curve fallback on error:
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_GENERALPOLYLINE | EXT_SHAPE_CURVE_FLAG);
    WriteVarUInt(buf, 2);
    WriteVarUInt(buf, 1);
    WriteVarUInt(buf, 1);
    for (int i = 0; i < 4; ++i) WriteVarUInt(buf, 0);

    WriteVarInt(buf, 0);
    WriteVarInt(buf, 0);
    WriteVarInt(buf, 1000);
    WriteVarInt(buf, 0);

    // Invalid curve type 999
    WriteVarUInt(buf, 0);
    WriteVarUInt(buf, 999);

    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    std::unique_ptr<OGRGeometry> geom(converter->GetAsGeometry(&fld));
    // Falls back without curves!
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(wkbFlatten(geom->getGeometryType()), wkbLineString);
  }

  // Curve with Z and M
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_GENERALPOLYLINE | EXT_SHAPE_CURVE_FLAG |
                          EXT_SHAPE_Z_FLAG | EXT_SHAPE_M_FLAG);
    WriteVarUInt(buf, 2);  // 2 points
    WriteVarUInt(buf, 1);  // 1 part
    WriteVarUInt(buf, 1);  // 1 curve
    for (int i = 0; i < 4; ++i) WriteVarUInt(buf, 0);

    WriteVarInt(buf, 0);
    WriteVarInt(buf, 0);
    WriteVarInt(buf, 1000);
    WriteVarInt(buf, 0);

    // Z deltas
    WriteVarInt(buf, 100);
    WriteVarInt(buf, 200);

    // M deltas
    WriteVarInt(buf, 300);
    WriteVarInt(buf, 400);

    WriteVarUInt(buf, 0);
    WriteVarUInt(buf, EXT_SHAPE_SEGMENT_ARC);
    WriteFloat64(buf, 5.0);
    WriteFloat64(buf, 5.0);
    WriteUInt32(buf, 0);

    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    std::unique_ptr<OGRGeometry> geom(converter->GetAsGeometry(&fld));
    ASSERT_NE(geom, nullptr);
    EXPECT_TRUE(geom->Is3D());
    EXPECT_TRUE(geom->IsMeasured());
  }

  // Curve with Z and M absent marker (byte 66)
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_GENERALPOLYLINE | EXT_SHAPE_CURVE_FLAG |
                          EXT_SHAPE_Z_FLAG | EXT_SHAPE_M_FLAG);
    WriteVarUInt(buf, 2);  // 2 points
    WriteVarUInt(buf, 1);  // 1 part
    WriteVarUInt(buf, 1);  // 1 curve
    for (int i = 0; i < 4; ++i) WriteVarUInt(buf, 0);

    WriteVarInt(buf, 0);
    WriteVarInt(buf, 0);
    WriteVarInt(buf, 1000);
    WriteVarInt(buf, 0);

    // Z deltas
    WriteVarInt(buf, 100);
    WriteVarInt(buf, 200);

    // M absent marker
    buf.push_back(66);

    WriteVarUInt(buf, 0);
    WriteVarUInt(buf, EXT_SHAPE_SEGMENT_ARC);
    WriteFloat64(buf, 5.0);
    WriteFloat64(buf, 5.0);
    WriteUInt32(buf, 0);

    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    std::unique_ptr<OGRGeometry> geom(converter->GetAsGeometry(&fld));
    ASSERT_NE(geom, nullptr);
    EXPECT_TRUE(geom->Is3D());
    EXPECT_FALSE(geom->IsMeasured());
  }

  // Curve with Polygon (SHPT_GENERALPOLYGON)
  {
    std::vector<GByte> buf;
    WriteVarUInt(buf, SHPT_GENERALPOLYGON | EXT_SHAPE_CURVE_FLAG);
    WriteVarUInt(buf, 4);  // 4 points (closed ring)
    WriteVarUInt(buf, 1);  // 1 part
    WriteVarUInt(buf, 1);  // 1 curve
    for (int i = 0; i < 4; ++i) WriteVarUInt(buf, 0);

    // 4 points: (0,0), (1000, 0), (1000, 1000), (0, 0)
    WriteVarInt(buf, 0);
    WriteVarInt(buf, 0);
    WriteVarInt(buf, 1000);
    WriteVarInt(buf, 0);
    WriteVarInt(buf, 0);
    WriteVarInt(buf, 1000);
    WriteVarInt(buf, -1000);
    WriteVarInt(buf, -1000);

    WriteVarUInt(buf, 0);
    WriteVarUInt(buf, EXT_SHAPE_SEGMENT_ARC);
    WriteFloat64(buf, 5.0);
    WriteFloat64(buf, 5.0);
    WriteUInt32(buf, 0);

    OGRField fld;
    fld.Binary.paData = buf.data();
    fld.Binary.nCount = static_cast<int>(buf.size());
    std::unique_ptr<OGRGeometry> geom(converter->GetAsGeometry(&fld));
    ASSERT_NE(geom, nullptr);
  }

  // Empty geometry types with Z and M
  {
    // Empty SHPT_MULTIPOINT with Z and M (0 points)
    std::vector<GByte> bufMP;
    WriteVarUInt(bufMP, SHPT_MULTIPOINT | EXT_SHAPE_Z_FLAG | EXT_SHAPE_M_FLAG);
    WriteVarUInt(bufMP, 0);                              // 0 points
    for (int i = 0; i < 4; ++i) WriteVarUInt(bufMP, 0);  // bbox
    OGRField fldMP;
    fldMP.Binary.paData = bufMP.data();
    fldMP.Binary.nCount = static_cast<int>(bufMP.size());
    std::unique_ptr<OGRGeometry> geomMP(converter->GetAsGeometry(&fldMP));
    ASSERT_NE(geomMP, nullptr);
    EXPECT_TRUE(geomMP->Is3D());
    EXPECT_TRUE(geomMP->IsMeasured());

    // Empty SHPT_ARC with Z and M (0 points)
    std::vector<GByte> bufArc;
    WriteVarUInt(bufArc, SHPT_ARC | EXT_SHAPE_Z_FLAG | EXT_SHAPE_M_FLAG);
    WriteVarUInt(bufArc, 0);                              // 0 points
    for (int i = 0; i < 4; ++i) WriteVarUInt(bufArc, 0);  // bbox
    OGRField fldArc;
    fldArc.Binary.paData = bufArc.data();
    fldArc.Binary.nCount = static_cast<int>(bufArc.size());
    std::unique_ptr<OGRGeometry> geomArc(converter->GetAsGeometry(&fldArc));
    ASSERT_NE(geomArc, nullptr);
    EXPECT_TRUE(geomArc->Is3D());
    EXPECT_TRUE(geomArc->IsMeasured());

    // Empty SHPT_POLYGON with Z and M (0 points)
    std::vector<GByte> bufPoly;
    WriteVarUInt(bufPoly, SHPT_POLYGON | EXT_SHAPE_Z_FLAG | EXT_SHAPE_M_FLAG);
    WriteVarUInt(bufPoly, 0);                              // 0 points
    for (int i = 0; i < 4; ++i) WriteVarUInt(bufPoly, 0);  // bbox
    OGRField fldPoly;
    fldPoly.Binary.paData = bufPoly.data();
    fldPoly.Binary.nCount = static_cast<int>(bufPoly.size());
    std::unique_ptr<OGRGeometry> geomPoly(converter->GetAsGeometry(&fldPoly));
    ASSERT_NE(geomPoly, nullptr);
    EXPECT_TRUE(geomPoly->Is3D());
    EXPECT_TRUE(geomPoly->IsMeasured());
  }
}

// ---------------------------------------------------------------------------
// 3. Table Open: Headers, Versions, Field Descriptors, UTF-16, Defaults
// ---------------------------------------------------------------------------

TEST_F(FileGDBTableTest, OpenErrorsAndVersions) {
  // Non-existent file
  {
    FileGDBTable tbl;
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_FALSE(tbl.Open("/vsimem/does_not_exist.gdbtable", false));
    CPLPopErrorHandler();
    tbl.Close();
  }

  // Truncated header (< 40 bytes)
  {
    std::string truncPath = std::string(kTestDir) + "/trunc.gdbtable";
    VSILFILE* fp = VSIFOpenL(truncPath.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    GByte smallBuf[20] = {0};
    VSIFWriteL(smallBuf, 20, 1, fp);
    VSIFCloseL(fp);

    FileGDBTable tbl;
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_FALSE(tbl.Open(truncPath.c_str(), false));
    CPLPopErrorHandler();
    tbl.Close();
    VSIUnlink(truncPath.c_str());
  }

  // Unsupported table version (e.g. 5)
  {
    std::string badVerPath = std::string(kTestDir) + "/badver.gdbtable";
    VSILFILE* fp = VSIFOpenL(badVerPath.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    GByte hdr[40] = {0};
    uint32_t ver5 = 5;
    memcpy(hdr, &ver5, 4);
    VSIFWriteL(hdr, 40, 1, fp);
    VSIFCloseL(fp);

    FileGDBTable tbl;
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_FALSE(tbl.Open(badVerPath.c_str(), false));
    CPLPopErrorHandler();
    tbl.Close();
    VSIUnlink(badVerPath.c_str());
  }

  // Table version 4 opened in update mode -> fails
  {
    std::string v4Path = std::string(kTestDir) + "/v4.gdbtable";
    VSILFILE* fp = VSIFOpenL(v4Path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    GByte hdr[40] = {0};
    uint32_t ver4 = 4;
    memcpy(hdr, &ver4, 4);
    VSIFWriteL(hdr, 40, 1, fp);
    VSIFCloseL(fp);

    FileGDBTable tbl;
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_FALSE(tbl.Open(v4Path.c_str(), true));
    CPLPopErrorHandler();
    tbl.Close();
    VSIUnlink(v4Path.c_str());
  }
}

TEST_F(FileGDBTableTest, TableWithAllFieldTypesAndDefaults) {
  std::string tblPath = std::string(kTestDir) + "/all_fields.gdbtable";
  FileGDBTable table;
  ASSERT_TRUE(table.Create(tblPath.c_str(), 4, FGTGT_POINT, true, true));
  EXPECT_FALSE(table.IsFileGDBV9());
  EXPECT_EQ(table.GetGeometryType(), FGTGT_POINT);
  EXPECT_TRUE(table.GetGeomTypeHasZ());
  EXPECT_TRUE(table.GetGeomTypeHasM());

  // Add fields with defaults
  OGRField defI16;
  defI16.Integer = 16;
  auto fldI16 = std::make_unique<FileGDBField>(
      "fld_i16", "alias_i16", FGFT_INT16, true, false, true, 0, defI16);

  OGRField defI32;
  defI32.Integer = 32;
  auto fldI32 = std::make_unique<FileGDBField>(
      "fld_i32", "alias_i32", FGFT_INT32, true, false, true, 0, defI32);

  OGRField defF32;
  defF32.Real = 3.14f;
  auto fldF32 = std::make_unique<FileGDBField>(
      "fld_f32", "alias_f32", FGFT_FLOAT32, true, false, true, 0, defF32);

  OGRField defF64;
  defF64.Real = 2.71828;
  auto fldF64 = std::make_unique<FileGDBField>(
      "fld_f64", "alias_f64", FGFT_FLOAT64, true, false, true, 0, defF64);

  OGRField defStr;
  defStr.String = CPLStrdup("default_str");
  auto fldStr = std::make_unique<FileGDBField>(
      "fld_str", "alias_str", FGFT_STRING, true, false, true, 50, defStr);
  CPLFree(defStr.String);

  OGRField defDt;
  FileGDBDoubleDateToOGRDate(25569.0, true, &defDt);
  auto fldDt = std::make_unique<FileGDBField>(
      "fld_dt", "alias_dt", FGFT_DATETIME, true, false, true, 0, defDt);

  auto fldBin =
      std::make_unique<FileGDBField>("fld_bin", "alias_bin", FGFT_BINARY, true,
                                     false, true, 0, FileGDBField::UNSET_FIELD);
  auto fldGuid =
      std::make_unique<FileGDBField>("fld_guid", "alias_guid", FGFT_GUID, true,
                                     false, true, 0, FileGDBField::UNSET_FIELD);
  auto fldGid = std::make_unique<FileGDBField>(
      "fld_gid", "alias_gid", FGFT_GLOBALID, false, true, false, 0,
      FileGDBField::UNSET_FIELD);
  OGRField defXml;
  defXml.String = CPLStrdup("<default_xml/>");
  auto fldXml = std::make_unique<FileGDBField>("fld_xml", "alias_xml", FGFT_XML,
                                               true, false, true, 0, defXml);
  CPLFree(defXml.String);

  OGRField defI64;
  defI64.Integer64 = 1234567890123LL;
  auto fldI64 = std::make_unique<FileGDBField>(
      "fld_i64", "alias_i64", FGFT_INT64, true, false, true, 0, defI64);

  OGRField defDate;
  FileGDBDoubleDateToOGRDate(25569.0, false, &defDate);
  auto fldDate = std::make_unique<FileGDBField>(
      "fld_date", "alias_date", FGFT_DATE, true, false, true, 0, defDate);

  OGRField defTime;
  FileGDBDoubleTimeToOGRTime(0.5, &defTime);
  auto fldTime = std::make_unique<FileGDBField>(
      "fld_time", "alias_time", FGFT_TIME, true, false, true, 0, defTime);

  OGRField defDtOff;
  FileGDBDateTimeWithOffsetToOGRDate(25569.5, 120, &defDtOff);
  auto fldDtOff = std::make_unique<FileGDBField>(
      "fld_dtoff", "alias_dtoff", FGFT_DATETIME_WITH_OFFSET, true, false, true,
      0, defDtOff);

  auto fldOid =
      std::make_unique<FileGDBField>("OBJECTID", "", FGFT_OBJECTID, false, true,
                                     false, 0, FileGDBField::UNSET_FIELD);
  ASSERT_TRUE(table.CreateField(std::move(fldOid)));

  std::vector<double> gridRes = {1000.0};
  auto geomFld = std::make_unique<FileGDBGeomField>(
      "SHAPE", "", true, "", -400.0, -400.0, 10000.0, 0.001, gridRes);
  geomFld->SetZOriginScaleTolerance(0.0, 100.0, 0.001);
  geomFld->SetMOriginScaleTolerance(0.0, 100.0, 0.001);
  ASSERT_TRUE(table.CreateField(std::move(geomFld)));

  ASSERT_TRUE(table.CreateField(std::move(fldI16)));
  ASSERT_TRUE(table.CreateField(std::move(fldI32)));
  ASSERT_TRUE(table.CreateField(std::move(fldF32)));
  ASSERT_TRUE(table.CreateField(std::move(fldF64)));
  ASSERT_TRUE(table.CreateField(std::move(fldStr)));
  ASSERT_TRUE(table.CreateField(std::move(fldDt)));
  ASSERT_TRUE(table.CreateField(std::move(fldBin)));
  ASSERT_TRUE(table.CreateField(std::move(fldGuid)));
  ASSERT_TRUE(table.CreateField(std::move(fldGid)));
  ASSERT_TRUE(table.CreateField(std::move(fldXml)));
  ASSERT_TRUE(table.CreateField(std::move(fldI64)));
  ASSERT_TRUE(table.CreateField(std::move(fldDate)));
  ASSERT_TRUE(table.CreateField(std::move(fldTime)));
  ASSERT_TRUE(table.CreateField(std::move(fldDtOff)));

  // Write a feature with non-null values for all fields
  auto rawFields = MakeUnsetFields(table.GetFieldCount());
  int idxI16 = table.GetFieldIdx("fld_i16");
  rawFields[idxI16].Integer = 100;
  int idxI32 = table.GetFieldIdx("fld_i32");
  rawFields[idxI32].Integer = 100000;
  int idxF32 = table.GetFieldIdx("fld_f32");
  rawFields[idxF32].Real = 1.5f;
  int idxF64 = table.GetFieldIdx("fld_f64");
  rawFields[idxF64].Real = 2.5;
  int idxStr = table.GetFieldIdx("fld_str");
  rawFields[idxStr].String = CPLStrdup("test_val");
  int idxDt = table.GetFieldIdx("fld_dt");
  rawFields[idxDt] = defDt;
  int idxBin = table.GetFieldIdx("fld_bin");
  GByte binData[3] = {1, 2, 3};
  rawFields[idxBin].Binary.paData = binData;
  rawFields[idxBin].Binary.nCount = 3;
  int idxGuid = table.GetFieldIdx("fld_guid");
  rawFields[idxGuid].String =
      CPLStrdup("{12345678-1234-1234-1234-123456789abc}");
  int idxGid = table.GetFieldIdx("fld_gid");
  rawFields[idxGid].String =
      CPLStrdup("{87654321-4321-4321-4321-cba987654321}");
  int idxXml = table.GetFieldIdx("fld_xml");
  rawFields[idxXml].String = CPLStrdup("<xml/>");
  int idxI64 = table.GetFieldIdx("fld_i64");
  rawFields[idxI64].Integer64 = 9876543210LL;
  int idxDate = table.GetFieldIdx("fld_date");
  rawFields[idxDate] = defDate;
  int idxTime = table.GetFieldIdx("fld_time");
  rawFields[idxTime] = defTime;
  int idxDtOff = table.GetFieldIdx("fld_dtoff");
  rawFields[idxDtOff] = defDtOff;

  OGRPoint pt(1.0, 2.0, 3.0, 4.0);
  int fid = 0;
  ASSERT_TRUE(table.CreateFeature(rawFields, &pt, &fid));
  EXPECT_EQ(fid, 1);
  CPLFree(rawFields[idxStr].String);
  CPLFree(rawFields[idxGuid].String);
  CPLFree(rawFields[idxGid].String);
  CPLFree(rawFields[idxXml].String);

  // Write a second feature with nulls
  fid = 0;
  auto rawFields2 = MakeUnsetFields(table.GetFieldCount());
  int idxGid2 = table.GetFieldIdx("fld_gid");
  rawFields2[idxGid2].String =
      CPLStrdup("{00000000-0000-0000-0000-000000000000}");
  bool bSuccess2 = table.CreateFeature(rawFields2, nullptr, &fid);
  CPLFree(rawFields2[idxGid2].String);
  ASSERT_TRUE(bSuccess2);
  EXPECT_EQ(fid, 2);

  table.Sync();
  table.Close();

  // Re-open in read-only mode and verify
  FileGDBTable roTable;
  ASSERT_TRUE(roTable.Open(tblPath.c_str(), false, "all_fields_lyr"));
  EXPECT_EQ(roTable.GetValidRecordCount(), 2);
  EXPECT_EQ(roTable.GetTotalRecordCount(), 2);
  EXPECT_GE(roTable.GetObjectIdFieldIdx(), 0);
  EXPECT_GE(roTable.GetGeomFieldIdx(), 0);
  ASSERT_NE(roTable.GetGeomField(), nullptr);

  // Read feature 1
  ASSERT_TRUE(roTable.SelectRow(0));
  EXPECT_EQ(roTable.GetCurRow(), 0);
  EXPECT_FALSE(roTable.IsCurRowDeleted());

  // Test out-of-order column reading: read col 5 then col 1
  const OGRField* fldValStr = roTable.GetFieldValue(idxStr);
  ASSERT_NE(fldValStr, nullptr);
  EXPECT_STREQ(fldValStr->String, "test_val");

  const OGRField* fldValI16 = roTable.GetFieldValue(idxI16);
  ASSERT_NE(fldValI16, nullptr);
  EXPECT_EQ(fldValI16->Integer, 100);

  // Read GUID multiple times to exercise m_nChSaved
  const OGRField* fldValGuid = roTable.GetFieldValue(idxGuid);
  ASSERT_NE(fldValGuid, nullptr);
  EXPECT_STREQ(fldValGuid->String, "{12345678-1234-1234-1234-123456789ABC}");
  fldValGuid = roTable.GetFieldValue(idxGuid);
  ASSERT_NE(fldValGuid, nullptr);
  EXPECT_STREQ(fldValGuid->String, "{12345678-1234-1234-1234-123456789ABC}");

  // Test GetAllFieldValues and FreeAllFieldValues
  std::vector<OGRField> allVals = roTable.GetAllFieldValues();
  EXPECT_EQ(allVals.size(), roTable.GetFieldCount());
  roTable.FreeAllFieldValues(allVals);

  // Jump from column 0 directly to last column (covers field skipping)
  ASSERT_TRUE(roTable.SelectRow(0));
  const OGRField* fldLast =
      roTable.GetFieldValue(static_cast<int>(roTable.GetFieldCount() - 1));
  EXPECT_NE(fldLast, nullptr);
  const OGRField* fldFirst = roTable.GetFieldValue(1);
  EXPECT_NE(fldFirst, nullptr);
  fldLast =
      roTable.GetFieldValue(static_cast<int>(roTable.GetFieldCount() - 1));
  EXPECT_NE(fldLast, nullptr);

  // Read feature 2 (nulls)
  ASSERT_TRUE(roTable.SelectRow(1));
  const OGRField* nullI16 = roTable.GetFieldValue(idxI16);
  EXPECT_TRUE(nullI16 == nullptr || OGR_RawField_IsNull(nullI16));

  // GetAndSelectNextNonEmptyRow
  EXPECT_EQ(roTable.GetAndSelectNextNonEmptyRow(0), 0);
  EXPECT_EQ(roTable.GetAndSelectNextNonEmptyRow(1), 1);
  EXPECT_EQ(roTable.GetAndSelectNextNonEmptyRow(2), -1);

  // Invalid row selection
  EXPECT_FALSE(roTable.SelectRow(-1));
  EXPECT_FALSE(roTable.SelectRow(100));

  roTable.Close();

  // Now delete .gdbtablx and open table so GuessFeatureLocations /
  // IsLikelyFeatureAtOffset runs across all field types
  std::string tablxPath = std::string(kTestDir) + "/all_fields.gdbtablx";
  VSIUnlink(tablxPath.c_str());
  {
    FileGDBTable guessedTable;
    ASSERT_TRUE(guessedTable.Open(tblPath.c_str(), false));
    EXPECT_EQ(guessedTable.GetTotalRecordCount(), 2);
    guessedTable.Close();
  }
}

TEST_F(FileGDBTableTest, HeaderRepairOnInconsistentBufferMaxSize) {
  std::string tblPath = std::string(kTestDir) + "/repair_header.gdbtable";
  FileGDBTable table;
  ASSERT_TRUE(table.Create(tblPath.c_str(), 4, FGTGT_POINT, false, false));

  auto fldStr =
      std::make_unique<FileGDBField>("str_fld", "", FGFT_STRING, true, false,
                                     true, 200, FileGDBField::UNSET_FIELD);
  ASSERT_TRUE(table.CreateField(std::move(fldStr)));

  auto rawFields = MakeUnsetFields(table.GetFieldCount());
  int idxStr = table.GetFieldIdx("str_fld");
  rawFields[idxStr].String = CPLStrdup("short");
  int fid = 0;
  OGRPoint pt(1.0, 2.0);
  ASSERT_TRUE(table.CreateFeature(rawFields, &pt, &fid));
  CPLFree(rawFields[idxStr].String);
  table.Sync();
  table.Close();

  // Manually corrupt m_nHeaderBufferMaxSize at byte 8 of .gdbtable to 1 byte
  VSILFILE* fp = VSIFOpenL(tblPath.c_str(), "r+b");
  ASSERT_NE(fp, nullptr);
  VSIFSeekL(fp, 8, SEEK_SET);
  uint32_t tinySize = 1;
  VSIFWriteL(&tinySize, 4, 1, fp);
  VSIFCloseL(fp);

  // Open in read-only mode: should trigger warning
  {
    FileGDBTable roTable;
    ASSERT_TRUE(roTable.Open(tblPath.c_str(), false));
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_TRUE(roTable.SelectRow(0));
    CPLPopErrorHandler();
    roTable.Close();
  }

  // With OGR_OPENFILEGDB_ERROR_ON_INCONSISTENT_BUFFER_MAX_SIZE=YES -> error
  {
    CPLSetConfigOption("OGR_OPENFILEGDB_ERROR_ON_INCONSISTENT_BUFFER_MAX_SIZE",
                       "YES");
    absl::Cleanup cleanup = [] {
      CPLSetConfigOption(
          "OGR_OPENFILEGDB_ERROR_ON_INCONSISTENT_BUFFER_MAX_SIZE", nullptr);
    };
    FileGDBTable roTable;
    ASSERT_TRUE(roTable.Open(tblPath.c_str(), false));
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_FALSE(roTable.SelectRow(0));
    CPLPopErrorHandler();
    roTable.Close();
  }

  // Open in update mode: should repair header on disk
  {
    FileGDBTable rwTable;
    ASSERT_TRUE(rwTable.Open(tblPath.c_str(), true));
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_TRUE(rwTable.SelectRow(0));
    CPLPopErrorHandler();
    rwTable.Sync();
    rwTable.Close();

    // Verify byte 8 is now repaired (> 1)
    fp = VSIFOpenL(tblPath.c_str(), "rb");
    ASSERT_NE(fp, nullptr);
    VSIFSeekL(fp, 8, SEEK_SET);
    uint32_t repairedSize = 0;
    VSIFReadL(&repairedSize, 4, 1, fp);
    VSIFCloseL(fp);
    EXPECT_GT(repairedSize, 1u);
  }
}

// ---------------------------------------------------------------------------
// 4. Missing gdbtablx & GuessFeatureLocations
// ---------------------------------------------------------------------------

TEST_F(FileGDBTableTest, GuessFeatureLocationsWithoutTablx) {
  std::string tblPath = std::string(kTestDir) + "/guess_features.gdbtable";
  FileGDBTable table;
  ASSERT_TRUE(table.Create(tblPath.c_str(), 4, FGTGT_POINT, false, false));

  auto fldStr =
      std::make_unique<FileGDBField>("str_fld", "", FGFT_STRING, true, false,
                                     true, 50, FileGDBField::UNSET_FIELD);
  ASSERT_TRUE(table.CreateField(std::move(fldStr)));

  auto rawFields = MakeUnsetFields(table.GetFieldCount());
  int idxStr = table.GetFieldIdx("str_fld");
  int fid = 0;
  OGRPoint pt(5.0, 10.0);

  rawFields[idxStr].String = CPLStrdup("feat1");
  bool bSuccess1 = table.CreateFeature(rawFields, &pt, &fid);
  CPLFree(rawFields[idxStr].String);
  ASSERT_TRUE(bSuccess1);

  fid = 0;
  rawFields[idxStr].String = CPLStrdup("feat2");
  bool bSuccess2 = table.CreateFeature(rawFields, &pt, &fid);
  CPLFree(rawFields[idxStr].String);
  ASSERT_TRUE(bSuccess2);

  vsi_l_offset nFeat1Offset = table.GetOffsetInTableForRow(0);
  table.Sync();
  table.Close();

  // Mark feat1 as deleted by negating its 4-byte size on disk (without blanking
  // body)
  VSILFILE* fp = VSIFOpenL(tblPath.c_str(), "r+b");
  ASSERT_NE(fp, nullptr);
  VSIFSeekL(fp, nFeat1Offset, SEEK_SET);
  uint32_t nSize = 0;
  ASSERT_EQ(VSIFReadL(&nSize, 4, 1, fp), 1u);
  int32_t nNegSize = -static_cast<int32_t>(nSize);
  VSIFSeekL(fp, nFeat1Offset, SEEK_SET);
  VSIFWriteL(&nNegSize, 4, 1, fp);
  VSIFCloseL(fp);

  // Delete the .gdbtablx file
  std::string tablxPath = std::string(kTestDir) + "/guess_features.gdbtablx";
  VSIUnlink(tablxPath.c_str());

  // Test OPENFILEGDB_IGNORE_GDBTABLX_ABSENCE=NO -> fails to open
  {
    CPLSetConfigOption("OPENFILEGDB_IGNORE_GDBTABLX_ABSENCE", "NO");
    absl::Cleanup cleanup = [] {
      CPLSetConfigOption("OPENFILEGDB_IGNORE_GDBTABLX_ABSENCE", nullptr);
    };
    FileGDBTable tblFail;
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_FALSE(tblFail.Open(tblPath.c_str(), false));
    CPLPopErrorHandler();
  }

  // Open with OPENFILEGDB_REPORT_DELETED_FEATURES=YES
  {
    CPLSetConfigOption("OPENFILEGDB_REPORT_DELETED_FEATURES", "YES");
    absl::Cleanup cleanup = [] {
      CPLSetConfigOption("OPENFILEGDB_REPORT_DELETED_FEATURES", nullptr);
    };
    FileGDBTable tblGuessed;
    CPLPushErrorHandler(CPLQuietErrorHandler);
    ASSERT_TRUE(tblGuessed.Open(tblPath.c_str(), false));
    CPLPopErrorHandler();

    EXPECT_GT(tblGuessed.GetTotalRecordCount(), 0);
    EXPECT_TRUE(tblGuessed.HasDeletedFeaturesListed());

    // Iterate rows
    int64_t validFound = 0;
    int64_t deletedFound = 0;
    for (int64_t i = 0; i < tblGuessed.GetTotalRecordCount(); ++i) {
      if (tblGuessed.SelectRow(i)) {
        if (tblGuessed.IsCurRowDeleted()) {
          deletedFound++;
        } else {
          validFound++;
        }
      }
    }
    EXPECT_GE(validFound, 1);
    EXPECT_GE(deletedFound, 1);
    tblGuessed.Close();
  }

  // Open with OPENFILEGDB_REPORT_DELETED_FEATURES=NO
  {
    CPLSetConfigOption("OPENFILEGDB_REPORT_DELETED_FEATURES", "NO");
    absl::Cleanup cleanup = [] {
      CPLSetConfigOption("OPENFILEGDB_REPORT_DELETED_FEATURES", nullptr);
    };
    FileGDBTable tblGuessed;
    CPLPushErrorHandler(CPLQuietErrorHandler);
    ASSERT_TRUE(tblGuessed.Open(tblPath.c_str(), false));
    CPLPopErrorHandler();

    EXPECT_FALSE(tblGuessed.HasDeletedFeaturesListed());
    tblGuessed.Close();
  }
}

// ---------------------------------------------------------------------------
// 5. Spatial Filter, Feature Extents, and Projection Y clamping
// ---------------------------------------------------------------------------

TEST_F(FileGDBTableTest, SpatialFilterAndEnvelopeChecks) {
  std::string tblPath = std::string(kTestDir) + "/spatial_filter.gdbtable";
  FileGDBTable table;
  ASSERT_TRUE(table.Create(tblPath.c_str(), 4, FGTGT_POINT, false, false));

  std::vector<double> gridRes = {1000.0};
  auto geomFld = std::make_unique<FileGDBGeomField>("SHAPE", "", true, "", 0.0,
                                                    0.0, 100.0, 0.001, gridRes);
  ASSERT_TRUE(table.CreateField(std::move(geomFld)));

  // Install filter envelope (10 <= x <= 20, 10 <= y <= 20)
  OGREnvelope filterEnv;
  filterEnv.MinX = 10.0;
  filterEnv.MaxX = 20.0;
  filterEnv.MinY = 10.0;
  filterEnv.MaxY = 20.0;
  table.InstallFilterEnvelope(&filterEnv);

  // Create point binary inside filter
  auto ptInside = std::make_unique<OGRPoint>(15.0, 15.0);
  std::vector<GByte> bufInside;
  WriteVarUInt(bufInside, SHPT_POINT);
  WriteVarUInt(bufInside, 1501);
  WriteVarUInt(bufInside, 1501);
  OGRField fldInside;
  fldInside.Binary.paData = bufInside.data();
  fldInside.Binary.nCount = static_cast<int>(bufInside.size());

  EXPECT_TRUE(table.DoesGeometryIntersectsFilterEnvelope(&fldInside));

  // Create point binary outside filter
  std::vector<GByte> bufOutside;
  WriteVarUInt(bufOutside, SHPT_POINT);
  WriteVarUInt(bufOutside, 501);
  WriteVarUInt(bufOutside, 501);
  OGRField fldOutside;
  fldOutside.Binary.paData = bufOutside.data();
  fldOutside.Binary.nCount = static_cast<int>(bufOutside.size());

  EXPECT_FALSE(table.DoesGeometryIntersectsFilterEnvelope(&fldOutside));

  // Point Empty
  std::vector<GByte> bufEmpty;
  WriteVarUInt(bufEmpty, SHPT_POINT);
  WriteVarUInt(bufEmpty, 0);
  WriteVarUInt(bufEmpty, 0);
  OGRField fldEmpty;
  fldEmpty.Binary.paData = bufEmpty.data();
  fldEmpty.Binary.nCount = static_cast<int>(bufEmpty.size());
  EXPECT_FALSE(table.DoesGeometryIntersectsFilterEnvelope(&fldEmpty));

  // GetFeatureExtent on point
  OGREnvelope outEnv;
  EXPECT_TRUE(table.GetFeatureExtent(&fldInside, &outEnv));
  EXPECT_DOUBLE_EQ(outEnv.MinX, 15.0);
  EXPECT_DOUBLE_EQ(outEnv.MinY, 15.0);
  EXPECT_DOUBLE_EQ(outEnv.MaxX, 15.0);
  EXPECT_DOUBLE_EQ(outEnv.MaxY, 15.0);

  // Test MultiPoint inside, empty, and outside
  {
    // Empty multipoint
    std::vector<GByte> bufMPEmpty;
    WriteVarUInt(bufMPEmpty, SHPT_MULTIPOINT);
    WriteVarUInt(bufMPEmpty, 0);  // 0 points
    OGRField fldMPEmpty;
    fldMPEmpty.Binary.paData = bufMPEmpty.data();
    fldMPEmpty.Binary.nCount = static_cast<int>(bufMPEmpty.size());
    EXPECT_TRUE(table.GetFeatureExtent(&fldMPEmpty, &outEnv));
    EXPECT_TRUE(table.DoesGeometryIntersectsFilterEnvelope(&fldMPEmpty));

    // Non-empty multipoint inside filter
    std::vector<GByte> bufMPInside;
    WriteVarUInt(bufMPInside, SHPT_MULTIPOINT);
    WriteVarUInt(bufMPInside, 1);     // 1 point
    WriteVarUInt(bufMPInside, 1500);  // vxmin
    WriteVarUInt(bufMPInside, 1500);  // vymin
    WriteVarUInt(bufMPInside, 100);   // vdx
    WriteVarUInt(bufMPInside, 100);   // vdy
    OGRField fldMPInside;
    fldMPInside.Binary.paData = bufMPInside.data();
    fldMPInside.Binary.nCount = static_cast<int>(bufMPInside.size());
    EXPECT_TRUE(table.GetFeatureExtent(&fldMPInside, &outEnv));
    EXPECT_TRUE(table.DoesGeometryIntersectsFilterEnvelope(&fldMPInside));

    // MultiPoint outside: vxmin > m_nFilterXMax
    std::vector<GByte> bufMPOutsideX;
    WriteVarUInt(bufMPOutsideX, SHPT_MULTIPOINT);
    WriteVarUInt(bufMPOutsideX, 1);
    WriteVarUInt(bufMPOutsideX, 2500);
    WriteVarUInt(bufMPOutsideX, 1500);
    WriteVarUInt(bufMPOutsideX, 100);
    WriteVarUInt(bufMPOutsideX, 100);
    OGRField fldMPOutsideX;
    fldMPOutsideX.Binary.paData = bufMPOutsideX.data();
    fldMPOutsideX.Binary.nCount = static_cast<int>(bufMPOutsideX.size());
    EXPECT_FALSE(table.DoesGeometryIntersectsFilterEnvelope(&fldMPOutsideX));

    // MultiPoint outside: vymin > m_nFilterYMax
    std::vector<GByte> bufMPOutsideY;
    WriteVarUInt(bufMPOutsideY, SHPT_MULTIPOINT);
    WriteVarUInt(bufMPOutsideY, 1);
    WriteVarUInt(bufMPOutsideY, 1500);
    WriteVarUInt(bufMPOutsideY, 2500);
    WriteVarUInt(bufMPOutsideY, 100);
    WriteVarUInt(bufMPOutsideY, 100);
    OGRField fldMPOutsideY;
    fldMPOutsideY.Binary.paData = bufMPOutsideY.data();
    fldMPOutsideY.Binary.nCount = static_cast<int>(bufMPOutsideY.size());
    EXPECT_FALSE(table.DoesGeometryIntersectsFilterEnvelope(&fldMPOutsideY));

    // MultiPoint outside: vxmin + vdx < m_nFilterXMin
    std::vector<GByte> bufMPLowX;
    WriteVarUInt(bufMPLowX, SHPT_MULTIPOINT);
    WriteVarUInt(bufMPLowX, 1);
    WriteVarUInt(bufMPLowX, 100);
    WriteVarUInt(bufMPLowX, 1500);
    WriteVarUInt(bufMPLowX, 10);
    WriteVarUInt(bufMPLowX, 100);
    OGRField fldMPLowX;
    fldMPLowX.Binary.paData = bufMPLowX.data();
    fldMPLowX.Binary.nCount = static_cast<int>(bufMPLowX.size());
    EXPECT_FALSE(table.DoesGeometryIntersectsFilterEnvelope(&fldMPLowX));

    // MultiPoint outside: vymin + vdy < m_nFilterYMin
    std::vector<GByte> bufMPLowY;
    WriteVarUInt(bufMPLowY, SHPT_MULTIPOINT);
    WriteVarUInt(bufMPLowY, 1);
    WriteVarUInt(bufMPLowY, 1500);
    WriteVarUInt(bufMPLowY, 100);
    WriteVarUInt(bufMPLowY, 100);
    WriteVarUInt(bufMPLowY, 10);
    OGRField fldMPLowY;
    fldMPLowY.Binary.paData = bufMPLowY.data();
    fldMPLowY.Binary.nCount = static_cast<int>(bufMPLowY.size());
    EXPECT_FALSE(table.DoesGeometryIntersectsFilterEnvelope(&fldMPLowY));
  }

  // Test Polyline / Polygon envelope skipping (nToSkip = 1)
  {
    std::vector<GByte> bufPoly;
    WriteVarUInt(bufPoly, SHPT_POLYGON);
    WriteVarUInt(bufPoly, 1);  // nPoints = 1
    WriteVarUInt(bufPoly, 1);  // nParts = 1 (to skip)
    WriteVarUInt(bufPoly, 1500);
    WriteVarUInt(bufPoly, 1500);
    WriteVarUInt(bufPoly, 100);
    WriteVarUInt(bufPoly, 100);
    OGRField fldPoly;
    fldPoly.Binary.paData = bufPoly.data();
    fldPoly.Binary.nCount = static_cast<int>(bufPoly.size());
    EXPECT_TRUE(table.GetFeatureExtent(&fldPoly, &outEnv));
    EXPECT_TRUE(table.DoesGeometryIntersectsFilterEnvelope(&fldPoly));
  }

  // Test Curve polyline (nToSkip = 2)
  {
    std::vector<GByte> bufCurve;
    WriteVarUInt(bufCurve, SHPT_GENERALPOLYLINE | EXT_SHAPE_CURVE_FLAG);
    WriteVarUInt(bufCurve, 1);  // nPoints = 1
    WriteVarUInt(bufCurve, 1);  // nParts = 1
    WriteVarUInt(bufCurve, 0);  // nCurves = 0
    WriteVarUInt(bufCurve, 1500);
    WriteVarUInt(bufCurve, 1500);
    WriteVarUInt(bufCurve, 100);
    WriteVarUInt(bufCurve, 100);
    OGRField fldCurve;
    fldCurve.Binary.paData = bufCurve.data();
    fldCurve.Binary.nCount = static_cast<int>(bufCurve.size());
    EXPECT_TRUE(table.GetFeatureExtent(&fldCurve, &outEnv));
    EXPECT_TRUE(table.DoesGeometryIntersectsFilterEnvelope(&fldCurve));
  }

  // Test MultiPatch (nToSkip = 2)
  {
    std::vector<GByte> bufMP;
    WriteVarUInt(bufMP, SHPT_MULTIPATCH);
    WriteVarUInt(bufMP, 1);  // nPoints = 1
    WriteVarUInt(bufMP, 0);  // skip varuint
    WriteVarUInt(bufMP, 1);  // nParts = 1
    WriteVarUInt(bufMP, 1500);
    WriteVarUInt(bufMP, 1500);
    WriteVarUInt(bufMP, 100);
    WriteVarUInt(bufMP, 100);
    OGRField fldMP;
    fldMP.Binary.paData = bufMP.data();
    fldMP.Binary.nCount = static_cast<int>(bufMP.size());
    EXPECT_TRUE(table.GetFeatureExtent(&fldMP, &outEnv));
    EXPECT_TRUE(table.DoesGeometryIntersectsFilterEnvelope(&fldMP));
  }

  // Unknown geometry type 99
  {
    std::vector<GByte> bufUnknown;
    WriteVarUInt(bufUnknown, 99);
    OGRField fldUnknown;
    fldUnknown.Binary.paData = bufUnknown.data();
    fldUnknown.Binary.nCount = static_cast<int>(bufUnknown.size());
    EXPECT_FALSE(table.GetFeatureExtent(&fldUnknown, &outEnv));
    EXPECT_TRUE(table.DoesGeometryIntersectsFilterEnvelope(&fldUnknown));
  }

  // Test negative envelope MinX / MinY and large MaxX / MaxY
  {
    OGREnvelope negEnv;
    negEnv.MinX = -100.0;
    negEnv.MinY = -100.0;
    negEnv.MaxX = 1e30;
    negEnv.MaxY = 1e30;
    table.InstallFilterEnvelope(&negEnv);
    EXPECT_TRUE(table.DoesGeometryIntersectsFilterEnvelope(&fldInside));
  }

  // Clear filter envelope
  table.InstallFilterEnvelope(nullptr);
  EXPECT_FALSE(table.DoesGeometryIntersectsFilterEnvelope(&fldOutside));

  // Spatial Index presence
  EXPECT_FALSE(table.HasSpatialIndex());

  table.Close();
}

TEST_F(FileGDBTableTest, SpatialIndexProjYClamping) {
  // Test Geographic SRS
  {
    std::string tblPath = std::string(kTestDir) + "/srs_geo.gdbtable";
    FileGDBTable table;
    ASSERT_TRUE(table.Create(tblPath.c_str(), 4, FGTGT_POINT, false, false));
    std::vector<double> gridRes = {1000.0};
    auto geomFld = std::make_unique<FileGDBGeomField>(
        "SHAPE", "", true,
        "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS "
        "84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\","
        "0.0174532925199433]]",
        -400.0, -400.0, 100000.0, 0.001, gridRes);
    ASSERT_TRUE(table.CreateField(std::move(geomFld)));

    double dfYMin = 0.0;
    double dfYMax = 0.0;
    table.GetMinMaxProjYForSpatialIndex(dfYMin, dfYMax);
    // Unprojected -> remains -inf / +inf
    EXPECT_LT(dfYMin, -1e30);
    EXPECT_GT(dfYMax, 1e30);
    table.Close();
  }

  // Test Projected SRS: Transverse Mercator with central meridian 0
  {
    const char* pszWktTM =
        "PROJCS[\"TM_0\",GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
        "SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],"
        "UNIT[\"degree\",0.0174532925199433]],PROJECTION[\"Transverse_"
        "Mercator\"],"
        "PARAMETER[\"latitude_of_origin\",0],PARAMETER[\"central_meridian\",0],"
        "PARAMETER[\"scale_factor\",1],PARAMETER[\"false_easting\",0],"
        "PARAMETER[\"false_northing\",0],UNIT[\"metre\",1]]";

    std::string tblPath = std::string(kTestDir) + "/srs_tm.gdbtable";
    FileGDBTable table;
    ASSERT_TRUE(table.Create(tblPath.c_str(), 4, FGTGT_POINT, false, false));
    std::vector<double> gridRes = {1000.0};
    auto geomFld = std::make_unique<FileGDBGeomField>(
        "SHAPE", "", true, pszWktTM, -400.0, -400.0, 100000.0, 0.001, gridRes);
    ASSERT_TRUE(table.CreateField(std::move(geomFld)));

    double dfYMin = 0.0;
    double dfYMax = 0.0;
    table.GetMinMaxProjYForSpatialIndex(dfYMin, dfYMax);
    EXPECT_GT(dfYMin, -1e30);
    EXPECT_LT(dfYMax, 1e30);
    table.Close();
  }

  // Test Projected SRS: Mercator_1SP (EPSG:3857)
  {
    OGRSpatialReference oSRS;
    oSRS.importFromEPSG(3857);
    oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    char* pszWkt = nullptr;
    oSRS.exportToWkt(&pszWkt);
    std::string wktStr = pszWkt;
    CPLFree(pszWkt);

    std::string tblPath = std::string(kTestDir) + "/srs_merc.gdbtable";
    FileGDBTable table;
    ASSERT_TRUE(table.Create(tblPath.c_str(), 4, FGTGT_POINT, false, false));
    std::vector<double> gridRes = {1000.0};
    auto geomFld = std::make_unique<FileGDBGeomField>(
        "SHAPE", "", true, wktStr, -400.0, -400.0, 100000.0, 0.001, gridRes);
    ASSERT_TRUE(table.CreateField(std::move(geomFld)));

    double dfYMin = 0.0;
    double dfYMax = 0.0;
    table.GetMinMaxProjYForSpatialIndex(dfYMin, dfYMax);
    EXPECT_GT(dfYMin, -1e30);
    EXPECT_LT(dfYMax, 1e30);
    table.Close();
  }

  // Test empty WKT and {invalid}
  {
    std::string tblPath = std::string(kTestDir) + "/srs_empty.gdbtable";
    FileGDBTable table;
    ASSERT_TRUE(table.Create(tblPath.c_str(), 4, FGTGT_POINT, false, false));
    std::vector<double> gridRes = {1000.0};
    auto geomFld = std::make_unique<FileGDBGeomField>(
        "SHAPE", "", true, "", -400.0, -400.0, 100000.0, 0.001, gridRes);
    ASSERT_TRUE(table.CreateField(std::move(geomFld)));

    double dfYMin = 0.0;
    double dfYMax = 0.0;
    table.GetMinMaxProjYForSpatialIndex(dfYMin, dfYMax);
    EXPECT_LT(dfYMin, -1e30);
    EXPECT_GT(dfYMax, 1e30);
    table.Close();
  }
  {
    std::string tblPath = std::string(kTestDir) + "/srs_invalid.gdbtable";
    FileGDBTable table;
    ASSERT_TRUE(table.Create(tblPath.c_str(), 4, FGTGT_POINT, false, false));
    std::vector<double> gridRes = {1000.0};
    auto geomFld = std::make_unique<FileGDBGeomField>(
        "SHAPE", "", true, "{some_guid}", -400.0, -400.0, 100000.0, 0.001,
        gridRes);
    ASSERT_TRUE(table.CreateField(std::move(geomFld)));

    double dfYMin = 0.0;
    double dfYMax = 0.0;
    table.GetMinMaxProjYForSpatialIndex(dfYMin, dfYMax);
    EXPECT_LT(dfYMin, -1e30);
    EXPECT_GT(dfYMax, 1e30);
    table.Close();
  }
}

// ---------------------------------------------------------------------------
// 6. GDB Indexes file parsing: V10, V9 with ATX, Strange Descriptors
// ---------------------------------------------------------------------------

TEST_F(FileGDBTableTest, IndexFileParsingVariants) {
  std::string tblPath = std::string(kTestDir) + "/idx_parse.gdbtable";
  FileGDBTable table;
  ASSERT_TRUE(table.Create(tblPath.c_str(), 4, FGTGT_NONE, false, false));
  auto fld =
      std::make_unique<FileGDBField>("block_key", "", FGFT_STRING, true, false,
                                     true, 50, FileGDBField::UNSET_FIELD);
  ASSERT_TRUE(table.CreateField(std::move(fld)));
  table.Sync();
  table.Close();

  std::string idxPath = std::string(kTestDir) + "/idx_parse.gdbindexes";

  // Test 1: V9 index file with 0x03859813 magic and blk_key_index.atx
  {
    VSILFILE* fp = VSIFOpenL(idxPath.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    uint32_t v9Magic = 0x03859813;
    VSIFWriteL(&v9Magic, 4, 1, fp);
    VSIFCloseL(fp);

    std::string atxPath =
        std::string(kTestDir) + "/idx_parse.blk_key_index.atx";
    fp = VSIFOpenL(atxPath.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(&v9Magic, 4, 1, fp);
    VSIFCloseL(fp);

    FileGDBTable roTable;
    ASSERT_TRUE(roTable.Open(tblPath.c_str(), false));
    EXPECT_EQ(roTable.GetIndexCount(), 1);
    const FileGDBIndex* idx = roTable.GetIndex(0);
    ASSERT_NE(idx, nullptr);
    EXPECT_EQ(idx->GetIndexName(), "blk_key_index");
    roTable.Close();

    // V9 index file with no matching .atx files
    VSIUnlink(atxPath.c_str());
    FileGDBTable roTableNoAtx;
    ASSERT_TRUE(roTableNoAtx.Open(tblPath.c_str(), false));
    EXPECT_EQ(roTableNoAtx.GetIndexCount(), 0);
    roTableNoAtx.Close();
  }

  // Test 2: V10 index with strange/deleted descriptor flags and
  // normal/duplicate/missing indices
  {
    std::vector<GByte> idxBuf;
    WriteUInt32(idxBuf, 4);  // 4 indices

    // Helper to write UTF-16 string
    auto writeUtf16 = [&](const std::string& str) {
      WriteUInt32(idxBuf, static_cast<uint32_t>(str.size()));
      for (char c : str) {
        idxBuf.push_back(static_cast<GByte>(c));
        idxBuf.push_back(0);
      }
    };

    // Descriptor 1: Strange/deleted descriptor (nMagic2 = 3)
    writeUtf16("idx_strange");
    WriteUInt16(idxBuf, 0);  // nMagic1
    WriteUInt32(idxBuf, 3);  // nMagic2 = 3 (nColNameCharCount)
    // 3 UTF-16 characters "del" + uint16 magic
    for (char c : std::string("del")) {
      idxBuf.push_back(static_cast<GByte>(c));
      idxBuf.push_back(0);
    }
    WriteUInt16(idxBuf, 0);

    // Descriptor 2: Normal index for "block_key"
    writeUtf16("idx_normal");
    WriteUInt16(idxBuf, 0);
    WriteUInt32(idxBuf, 2);  // valid magic2
    WriteUInt16(idxBuf, 0);  // valid magic3
    WriteUInt32(idxBuf, 0);
    writeUtf16("block_key");
    WriteUInt16(idxBuf, 0);

    // Descriptor 3: Duplicate index for "block_key"
    writeUtf16("idx_duplicate");
    WriteUInt16(idxBuf, 0);
    WriteUInt32(idxBuf, 2);
    WriteUInt16(idxBuf, 0);
    WriteUInt32(idxBuf, 0);
    writeUtf16("block_key");
    WriteUInt16(idxBuf, 0);

    // Descriptor 4: Index on non-existent field "missing"
    writeUtf16("idx_missing");
    WriteUInt16(idxBuf, 0);
    WriteUInt32(idxBuf, 2);
    WriteUInt16(idxBuf, 0);
    WriteUInt32(idxBuf, 0);
    writeUtf16("missing");
    WriteUInt16(idxBuf, 0);

    VSILFILE* fp = VSIFOpenL(idxPath.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(idxBuf.data(), idxBuf.size(), 1, fp);
    VSIFCloseL(fp);

    FileGDBTable roTable;
    ASSERT_TRUE(roTable.Open(tblPath.c_str(), false));
    EXPECT_EQ(roTable.GetIndexCount(), 3);
    roTable.Close();
  }
}

// ---------------------------------------------------------------------------
// 7. Sparse V3 and V4 Tables with Block Maps
// ---------------------------------------------------------------------------

TEST_F(FileGDBTableTest, SparseTablxV3AndV4Headers) {
  std::string tblPath = std::string(kTestDir) + "/sparse_v3.gdbtable";
  FileGDBTable table;
  ASSERT_TRUE(table.Create(tblPath.c_str(), 4, FGTGT_NONE, false, false));
  auto fld =
      std::make_unique<FileGDBField>("str_fld", "", FGFT_STRING, true, false,
                                     true, 50, FileGDBField::UNSET_FIELD);
  ASSERT_TRUE(table.CreateField(std::move(fld)));

  auto rawFields = MakeUnsetFields(table.GetFieldCount());
  int idxStr = table.GetFieldIdx("str_fld");
  rawFields[idxStr].String = CPLStrdup("sparse_feat1");
  int fid = 1;
  ASSERT_TRUE(table.CreateFeature(rawFields, nullptr, &fid));
  EXPECT_EQ(fid, 1);

  fid = 2050;  // Row 2049, Block 2, leaving Block 1 empty!
  ASSERT_TRUE(table.CreateFeature(rawFields, nullptr, &fid));
  EXPECT_EQ(fid, 2050);
  CPLFree(rawFields[idxStr].String);

  table.Sync();
  vsi_l_offset off0 = table.GetOffsetInTableForRow(0);
  table.Close();

  // Re-open in read-only mode to verify sparse block map handling
  {
    FileGDBTable roTable;
    ASSERT_TRUE(roTable.Open(tblPath.c_str(), false));
    EXPECT_EQ(roTable.GetTotalRecordCount(), 2050);
    EXPECT_EQ(roTable.GetValidRecordCount(), 2);

    // GetAndSelectNextNonEmptyRow
    EXPECT_EQ(roTable.GetAndSelectNextNonEmptyRow(0), 0);
    EXPECT_EQ(roTable.GetAndSelectNextNonEmptyRow(1), 2049);
    EXPECT_EQ(roTable.GetAndSelectNextNonEmptyRow(2050), -1);

    // GetOffsetInTableForRow
    EXPECT_EQ(roTable.GetOffsetInTableForRow(1024), 0u);  // Empty block 1
    vsi_l_offset off2049 = roTable.GetOffsetInTableForRow(2049);
    EXPECT_GT(off2049, 0u);

    // Backwards block search in block map
    vsi_l_offset off0_again = roTable.GetOffsetInTableForRow(0);
    EXPECT_EQ(off0_again, off0);

    vsi_l_offset nOffTableX = 0;
    EXPECT_GT(roTable.GetOffsetInTableForRow(0, &nOffTableX), 0u);
    EXPECT_GT(nOffTableX, 0u);

    roTable.Close();
  }

  // V4 Table and TablX Header Tests
  std::string v4TblPath = std::string(kTestDir) + "/v4_test.gdbtable";
  std::string v4TablxPath = std::string(kTestDir) + "/v4_test.gdbtablx";

  // Copy V3 .gdbtable to v4_test.gdbtable and set version 4
  {
    GByte* pabyData = nullptr;
    vsi_l_offset nLen = 0;
    ASSERT_EQ(VSIIngestFile(nullptr, tblPath.c_str(), &pabyData, &nLen, -1), 1);
    uint32_t ver4 = 4;
    memcpy(pabyData, &ver4, 4);
    uint64_t v4RecCount = 1;
    memcpy(pabyData + 16, &v4RecCount, 8);
    VSILFILE* fp = VSIFOpenL(v4TblPath.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(pabyData, nLen, 1, fp);
    VSIFCloseL(fp);
    VSIFree(pabyData);
  }

  // Case A: Version mismatch in .gdbtablx
  {
    VSILFILE* fp = VSIFOpenL(v4TablxPath.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    uint32_t ver3 = 3;
    VSIFWriteL(&ver3, 4, 1, fp);
    GByte zeros[12] = {0};
    VSIFWriteL(zeros, 12, 1, fp);
    VSIFCloseL(fp);

    FileGDBTable tbl;
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_FALSE(tbl.Open(v4TblPath.c_str(), false));
    CPLPopErrorHandler();
    tbl.Close();
  }

  // Case B: Valid V4 without bitmap section
  {
    VSILFILE* fp = VSIFOpenL(v4TablxPath.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    std::vector<GByte> hdr(16, 0);
    uint32_t ver4 = 4;
    uint64_t oneBlock = 1;
    uint32_t offSize = 4;
    memcpy(hdr.data(), &ver4, 4);
    memcpy(hdr.data() + 4, &oneBlock, 8);
    memcpy(hdr.data() + 12, &offSize, 4);
    VSIFWriteL(hdr.data(), 16, 1, fp);

    std::vector<GByte> body(4096, 0);
    uint32_t u32Off0 = static_cast<uint32_t>(off0);
    memcpy(body.data(), &u32Off0, 4);
    VSIFWriteL(body.data(), 4096, 1, fp);

    std::vector<GByte> trailer(12, 0);
    uint64_t recCount = 1;
    uint32_t bitmapSecSize = 0;
    memcpy(trailer.data(), &recCount, 8);
    memcpy(trailer.data() + 8, &bitmapSecSize, 4);
    VSIFWriteL(trailer.data(), 12, 1, fp);
    VSIFCloseL(fp);

    FileGDBTable tbl;
    ASSERT_TRUE(tbl.Open(v4TblPath.c_str(), false));
    EXPECT_EQ(tbl.GetTotalRecordCount(), 1);
    EXPECT_TRUE(tbl.SelectRow(0));
    tbl.Close();
  }

  // Case C: Valid V4 with 32842-byte bitmap section
  {
    VSILFILE* fp = VSIFOpenL(v4TablxPath.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    std::vector<GByte> hdr(16, 0);
    uint32_t ver4 = 4;
    uint64_t oneBlock = 1;
    uint32_t offSize = 4;
    memcpy(hdr.data(), &ver4, 4);
    memcpy(hdr.data() + 4, &oneBlock, 8);
    memcpy(hdr.data() + 12, &offSize, 4);
    VSIFWriteL(hdr.data(), 16, 1, fp);

    std::vector<GByte> body(4096, 0);
    uint32_t u32Off0 = static_cast<uint32_t>(off0);
    memcpy(body.data(), &u32Off0, 4);
    VSIFWriteL(body.data(), 4096, 1, fp);

    std::vector<GByte> trailer(12, 0);
    uint64_t recCount = 1;
    uint32_t bitmapSecSize = 22 + 32768 + 52;
    memcpy(trailer.data(), &recCount, 8);
    memcpy(trailer.data() + 8, &bitmapSecSize, 4);
    VSIFWriteL(trailer.data(), 12, 1, fp);

    std::vector<GByte> bitmapSec(bitmapSecSize, 0);
    memcpy(bitmapSec.data(), "\x01\x00\x01\x00\x00\x00", 6);
    bitmapSec[22] = 0x01;  // block 0 present
    memcpy(bitmapSec.data() + 22 + 32768,
           "\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", 12);
    VSIFWriteL(bitmapSec.data(), bitmapSecSize, 1, fp);
    VSIFCloseL(fp);

    FileGDBTable tbl;
    ASSERT_TRUE(tbl.Open(v4TblPath.c_str(), false));
    EXPECT_EQ(tbl.GetTotalRecordCount(), 1);
    EXPECT_TRUE(tbl.SelectRow(0));
    tbl.Close();
  }

  // Case D: V4 with unrecognized bitmap section size
  {
    VSILFILE* fp = VSIFOpenL(v4TablxPath.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    std::vector<GByte> hdr(16, 0);
    uint32_t ver4 = 4;
    uint64_t oneBlock = 1;
    uint32_t offSize = 4;
    memcpy(hdr.data(), &ver4, 4);
    memcpy(hdr.data() + 4, &oneBlock, 8);
    memcpy(hdr.data() + 12, &offSize, 4);
    VSIFWriteL(hdr.data(), 16, 1, fp);

    std::vector<GByte> body(4096, 0);
    uint32_t u32Off0 = static_cast<uint32_t>(off0);
    memcpy(body.data(), &u32Off0, 4);
    VSIFWriteL(body.data(), 4096, 1, fp);

    std::vector<GByte> trailer(12, 0);
    uint64_t recCount = 1;
    uint32_t bitmapSecSize = 10;
    memcpy(trailer.data(), &recCount, 8);
    memcpy(trailer.data() + 8, &bitmapSecSize, 4);
    VSIFWriteL(trailer.data(), 12, 1, fp);

    GByte dummy[10] = {0};
    VSIFWriteL(dummy, 10, 1, fp);
    VSIFCloseL(fp);

    FileGDBTable tbl;
    CPLPushErrorHandler(CPLQuietErrorHandler);
    ASSERT_TRUE(tbl.Open(v4TblPath.c_str(), false));
    CPLPopErrorHandler();
    tbl.Close();
  }
}

// ---------------------------------------------------------------------------
// 8. Synthetic Raster Fields Table
// ---------------------------------------------------------------------------

TEST_F(FileGDBTableTest, SyntheticRasterFieldsTable) {
  std::string tblPath = std::string(kTestDir) + "/raster_fields.gdbtable";

  // Build field descriptor buffer
  std::vector<GByte> fieldDesc;
  // Placeholder for field desc size
  for (int i = 0; i < 4; ++i) fieldDesc.push_back(0);
  WriteUInt32(fieldDesc, 4);  // secondary header version = 4
  WriteUInt32(fieldDesc, 0);  // geom type = FGTGT_NONE
  WriteUInt16(fieldDesc, 4);  // 4 fields

  auto appendRasterFieldDesc = [&](const std::string& name, GByte rasterType) {
    fieldDesc.push_back(static_cast<GByte>(name.size()));
    for (char c : name) {
      fieldDesc.push_back(static_cast<GByte>(c));
      fieldDesc.push_back(0);
    }
    fieldDesc.push_back(0);  // 0-char alias
    fieldDesc.push_back(static_cast<GByte>(FGFT_RASTER));
    fieldDesc.push_back(0);  // flags[0]
    fieldDesc.push_back(1);  // flags[1]: nullable = 1
    // raster col name: "c"
    fieldDesc.push_back(1);
    fieldDesc.push_back('c');
    fieldDesc.push_back(0);
    WriteUInt16(fieldDesc, 0);  // WKT length = 0
    fieldDesc.push_back(0);     // abyGeomFlags = 0
    fieldDesc.push_back(rasterType);
  };

  appendRasterFieldDesc("r_ext", 0);  // EXTERNAL
  appendRasterFieldDesc("r_man", 1);  // MANAGED
  appendRasterFieldDesc("r_inl", 2);  // INLINE

  // Append BINARY field: "bin" (sets bExactSizeKnown = FALSE in
  // IsLikelyFeatureAtOffset)
  fieldDesc.push_back(3);  // name length
  for (char c : std::string("bin")) {
    fieldDesc.push_back(static_cast<GByte>(c));
    fieldDesc.push_back(0);
  }
  fieldDesc.push_back(0);  // alias length
  fieldDesc.push_back(static_cast<GByte>(FGFT_BINARY));
  fieldDesc.push_back(0);  // length
  fieldDesc.push_back(1);  // flags: nullable = 1

  // 4 trailing bytes like FileGDB SDK (0xDE, 0xAD, 0xBE, 0xEF)
  fieldDesc.push_back(0xDE);
  fieldDesc.push_back(0xAD);
  fieldDesc.push_back(0xBE);
  fieldDesc.push_back(0xEF);

  // Set fieldDesc size
  uint32_t nDescSize = static_cast<uint32_t>(fieldDesc.size() - 4);
  memcpy(fieldDesc.data(), &nDescSize, 4);

  // Build row buffer
  std::vector<GByte> rowBuf;
  rowBuf.push_back(0x00);  // null bitmap: none null

  // Field 0 (EXTERNAL): VarUInt 4, UTF-16 "xy"
  WriteVarUInt(rowBuf, 4);
  rowBuf.push_back('x');
  rowBuf.push_back(0);
  rowBuf.push_back('y');
  rowBuf.push_back(0);

  // Field 1 (MANAGED): int32_t(77)
  WriteUInt32(rowBuf, 77);

  // Field 2 (INLINE): VarUInt 3, bytes 11, 22, 33
  WriteVarUInt(rowBuf, 3);
  rowBuf.push_back(0x11);
  rowBuf.push_back(0x22);
  rowBuf.push_back(0x33);

  // Field 3 (BINARY): VarUInt 2, bytes 0xAA, 0xBB
  WriteVarUInt(rowBuf, 2);
  rowBuf.push_back(0xAA);
  rowBuf.push_back(0xBB);

  // Build table header (40 bytes)
  std::vector<GByte> tblHdr(40, 0);
  uint32_t ver3 = 3;
  uint32_t nRecCount = 1;
  uint32_t nBufMaxSize = 500;
  uint64_t nFileSize =
      40 + fieldDesc.size() + sizeof(uint32_t) + rowBuf.size() + 4;
  uint64_t nOffsetFieldDesc = 40;
  memcpy(tblHdr.data(), &ver3, 4);
  memcpy(tblHdr.data() + 4, &nRecCount, 4);
  memcpy(tblHdr.data() + 8, &nBufMaxSize, 4);
  memcpy(tblHdr.data() + 24, &nFileSize, 8);
  memcpy(tblHdr.data() + 32, &nOffsetFieldDesc, 8);

  // Write .gdbtable
  VSILFILE* fp = VSIFOpenL(tblPath.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(tblHdr.data(), 40, 1, fp);
  VSIFWriteL(fieldDesc.data(), fieldDesc.size(), 1, fp);
  uint32_t nRowSize = static_cast<uint32_t>(rowBuf.size());
  VSIFWriteL(&nRowSize, 4, 1, fp);
  VSIFWriteL(rowBuf.data(), rowBuf.size(), 1, fp);
  GByte trailingZeroes[4] = {0};
  VSIFWriteL(trailingZeroes, 4, 1, fp);
  VSIFCloseL(fp);

  // Open without .gdbtablx (runs GuessFeatureLocations and tests FGFT_RASTER)
  {
    FileGDBTable roTable;
    ASSERT_TRUE(roTable.Open(tblPath.c_str(), false));
    EXPECT_EQ(roTable.GetTotalRecordCount(), 1);
    ASSERT_TRUE(roTable.SelectRow(0));

    // Jump from col 0 directly to col 3 (BINARY), skipping over all 3 raster
    // fields
    const OGRField* fldBin = roTable.GetFieldValue(3);
    ASSERT_NE(fldBin, nullptr);
    EXPECT_EQ(fldBin->Binary.nCount, 2);

    // Read each raster field directly
    const OGRField* fld0 = roTable.GetFieldValue(0);
    ASSERT_NE(fld0, nullptr);
    EXPECT_STREQ(fld0->String, "xy");

    const OGRField* fld1 = roTable.GetFieldValue(1);
    ASSERT_NE(fld1, nullptr);
    EXPECT_EQ(fld1->Integer, 77);

    const OGRField* fld2 = roTable.GetFieldValue(2);
    ASSERT_NE(fld2, nullptr);
    EXPECT_EQ(fld2->Binary.nCount, 3);

    // Read all fields via GetAllFieldValues
    std::vector<OGRField> allVals = roTable.GetAllFieldValues();
    EXPECT_EQ(allVals.size(), 4);
    EXPECT_EQ(allVals[3].Binary.nCount, 2);
    roTable.FreeAllFieldValues(allVals);

    roTable.Close();
  }
}

// ---------------------------------------------------------------------------
// 9. Additional Coverage Edges
// ---------------------------------------------------------------------------

TEST_F(FileGDBTableTest, AdditionalCoverageEdges) {
  // 1. Missing .gdbtablx with bUpdate=true or
  // OPENFILEGDB_IGNORE_GDBTABLX_ABSENCE=NO
  {
    std::string tblPath =
        std::string(kTestDir) + "/missing_tablx_edges.gdbtable";
    FileGDBTable table;
    ASSERT_TRUE(table.Create(tblPath.c_str(), 4, FGTGT_POINT, false, false));
    table.Close();
    std::string tablxPath = CPLResetExtension(tblPath.c_str(), "gdbtablx");

    // Set nValidRecordCount = 1 in header so openfilegdb tries to open tablx
    VSILFILE* fpTbl = VSIFOpenL(tblPath.c_str(), "r+b");
    ASSERT_NE(fpTbl, nullptr);
    uint32_t nOne = 1;
    VSIFSeekL(fpTbl, 4, SEEK_SET);
    VSIFWriteL(&nOne, 4, 1, fpTbl);
    VSIFCloseL(fpTbl);

    VSIUnlink(tablxPath.c_str());

    // Open with bUpdate = true when .gdbtablx missing
    {
      FileGDBTable roTable;
      CPLPushErrorHandler(CPLQuietErrorHandler);
      EXPECT_FALSE(roTable.Open(tblPath.c_str(), true));
      CPLPopErrorHandler();
    }

    // Open with OPENFILEGDB_IGNORE_GDBTABLX_ABSENCE = "NO"
    {
      CPLSetConfigOption("OPENFILEGDB_IGNORE_GDBTABLX_ABSENCE", "NO");
      FileGDBTable roTable;
      CPLPushErrorHandler(CPLQuietErrorHandler);
      EXPECT_FALSE(roTable.Open(tblPath.c_str(), false));
      CPLPopErrorHandler();
      CPLSetConfigOption("OPENFILEGDB_IGNORE_GDBTABLX_ABSENCE", nullptr);
    }
  }

  // 2. OPENFILEGDB_CHECK_RECORD_COUNT and valid > total record count
  {
    std::string tblPath = std::string(kTestDir) + "/chk_rec_count.gdbtable";
    FileGDBTable table;
    ASSERT_TRUE(table.Create(tblPath.c_str(), 4, FGTGT_POINT, false, false));
    auto fldStr = std::make_unique<FileGDBField>(
        "s", "", FGFT_STRING, true, false, true, 20, FileGDBField::UNSET_FIELD);
    ASSERT_TRUE(table.CreateField(std::move(fldStr)));
    auto rawFields = MakeUnsetFields(table.GetFieldCount());
    int fid = 0;
    OGRPoint pt(1.0, 2.0);
    rawFields[table.GetFieldIdx("s")].String = CPLStrdup("a");
    ASSERT_TRUE(table.CreateFeature(rawFields, &pt, &fid));
    CPLFree(rawFields[table.GetFieldIdx("s")].String);
    table.Sync();
    table.Close();

    // In .gdbtable header, set m_nValidRecordCount (byte 4) to 5 (while total
    // is 1)
    VSILFILE* fp = VSIFOpenL(tblPath.c_str(), "r+b");
    ASSERT_NE(fp, nullptr);
    uint32_t fakeValid = 5;
    VSIFSeekL(fp, 4, SEEK_SET);
    VSIFWriteL(&fakeValid, 4, 1, fp);
    VSIFCloseL(fp);

    // Open with default config (OPENFILEGDB_USE_GDBTABLE_RECORD_COUNT unset)
    {
      FileGDBTable roTable;
      CPLPushErrorHandler(CPLQuietErrorHandler);
      EXPECT_TRUE(roTable.Open(tblPath.c_str(), false));
      CPLPopErrorHandler();
      EXPECT_EQ(roTable.GetTotalRecordCount(), 1);
      roTable.Close();
    }

    // Open with OPENFILEGDB_USE_GDBTABLE_RECORD_COUNT = "YES"
    {
      CPLSetConfigOption("OPENFILEGDB_USE_GDBTABLE_RECORD_COUNT", "YES");
      FileGDBTable roTable;
      CPLPushErrorHandler(CPLQuietErrorHandler);
      EXPECT_TRUE(roTable.Open(tblPath.c_str(), false));
      CPLPopErrorHandler();
      CPLSetConfigOption("OPENFILEGDB_USE_GDBTABLE_RECORD_COUNT", nullptr);
      EXPECT_EQ(roTable.GetTotalRecordCount(), 5);
      roTable.Close();
    }
  }

  // 3. GuessFeatureLocations: deleted field description at offset 40
  {
    std::string tblPath = std::string(kTestDir) + "/deleted_fld_desc.gdbtable";
    // Build field descriptor buffer (starts at offset 140)
    std::vector<GByte> fieldDesc;
    for (int i = 0; i < 4; ++i) fieldDesc.push_back(0);
    WriteUInt32(fieldDesc, 4);  // v4
    WriteUInt32(fieldDesc, 0);  // geom = none
    WriteUInt16(fieldDesc, 1);  // 1 field
    fieldDesc.push_back(1);     // name len 1
    fieldDesc.push_back('x');
    fieldDesc.push_back(0);
    fieldDesc.push_back(0);  // alias len 0
    fieldDesc.push_back(static_cast<GByte>(FGFT_INT32));
    fieldDesc.push_back(4);  // len
    fieldDesc.push_back(1);  // nullable
    fieldDesc.push_back(0);  // def val len
    fieldDesc.push_back(0xDE);
    fieldDesc.push_back(0xAD);
    fieldDesc.push_back(0xBE);
    fieldDesc.push_back(0xEF);
    uint32_t nDescSize = static_cast<uint32_t>(fieldDesc.size() - 4);
    memcpy(fieldDesc.data(), &nDescSize, 4);

    // Row: null bitmap 0, int32 42
    std::vector<GByte> rowBuf;
    rowBuf.push_back(0);
    WriteUInt32(rowBuf, 42);

    // Table header (40 bytes)
    std::vector<GByte> tblHdr(40, 0);
    uint32_t ver3 = 3;
    uint32_t nRecCount = 1;
    uint32_t nBufMaxSize = 200;
    uint64_t nOffsetFieldDesc = 140;
    uint64_t nFileSize = 140 + fieldDesc.size() + 4 + rowBuf.size() + 4;
    memcpy(tblHdr.data(), &ver3, 4);
    memcpy(tblHdr.data() + 4, &nRecCount, 4);
    memcpy(tblHdr.data() + 8, &nBufMaxSize, 4);
    memcpy(tblHdr.data() + 24, &nFileSize, 8);
    memcpy(tblHdr.data() + 32, &nOffsetFieldDesc, 8);

    // Deleted descriptor at offset 40 (100 bytes long, so 40 + 100 = 140)
    std::vector<GByte> delDesc(100, 0);
    int32_t nDelSize = -static_cast<int32_t>(100 + fieldDesc.size());
    int32_t nDelVer = 4;
    memcpy(delDesc.data(), &nDelSize, 4);
    memcpy(delDesc.data() + 4, &nDelVer, 4);
    delDesc[8] = 1;  // IS_VALID_LAYER_GEOM_TYPE(1) = true (FGTGT_POINT)
    delDesc[9] = 3;
    delDesc[10] = 0;
    delDesc[11] = 0;

    VSILFILE* fp = VSIFOpenL(tblPath.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(tblHdr.data(), 40, 1, fp);
    VSIFWriteL(delDesc.data(), delDesc.size(), 1, fp);
    VSIFWriteL(fieldDesc.data(), fieldDesc.size(), 1, fp);
    uint32_t nRowSize = static_cast<uint32_t>(rowBuf.size());
    VSIFWriteL(&nRowSize, 4, 1, fp);
    VSIFWriteL(rowBuf.data(), rowBuf.size(), 1, fp);
    GByte zeros[4] = {0};
    VSIFWriteL(zeros, 4, 1, fp);
    VSIFCloseL(fp);

    FileGDBTable roTable;
    ASSERT_TRUE(roTable.Open(tblPath.c_str(), false));
    EXPECT_EQ(roTable.GetTotalRecordCount(), 1);
    roTable.Close();
  }

  // 4. GuessFeatureLocations: more features found than declared
  {
    std::string tblPath = std::string(kTestDir) + "/more_features.gdbtable";
    FileGDBTable table;
    ASSERT_TRUE(table.Create(tblPath.c_str(), 4, FGTGT_POINT, false, false));
    auto fldStr = std::make_unique<FileGDBField>(
        "s", "", FGFT_STRING, true, false, true, 20, FileGDBField::UNSET_FIELD);
    ASSERT_TRUE(table.CreateField(std::move(fldStr)));
    auto rawFields = MakeUnsetFields(table.GetFieldCount());
    int fid = 0;
    OGRPoint pt(1.0, 2.0);
    rawFields[table.GetFieldIdx("s")].String = CPLStrdup("f1");
    ASSERT_TRUE(table.CreateFeature(rawFields, &pt, &fid));
    CPLFree(rawFields[table.GetFieldIdx("s")].String);
    fid = 0;
    rawFields[table.GetFieldIdx("s")].String = CPLStrdup("f2");
    ASSERT_TRUE(table.CreateFeature(rawFields, &pt, &fid));
    CPLFree(rawFields[table.GetFieldIdx("s")].String);
    table.Sync();
    table.Close();

    // Delete tablx
    std::string tablxPath = CPLResetExtension(tblPath.c_str(), "gdbtablx");
    VSIUnlink(tablxPath.c_str());

    // Set nValidRecordCount in header to 1
    VSILFILE* fp = VSIFOpenL(tblPath.c_str(), "r+b");
    ASSERT_NE(fp, nullptr);
    uint32_t nOne = 1;
    VSIFSeekL(fp, 4, SEEK_SET);
    VSIFWriteL(&nOne, 4, 1, fp);
    VSIFCloseL(fp);

    FileGDBTable roTable;
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_TRUE(roTable.Open(tblPath.c_str(), false));
    CPLPopErrorHandler();
    EXPECT_EQ(roTable.GetTotalRecordCount(), 2);
    roTable.Close();
  }

  // 5. ReadTableXHeaderV3: version mismatch and secondary header version 3 with
  // bUpdate=true
  {
    std::string tblPath = std::string(kTestDir) + "/v3_v4_mismatch.gdbtable";
    std::string tablxPath = std::string(kTestDir) + "/v3_v4_mismatch.gdbtablx";
    FileGDBTable table;
    ASSERT_TRUE(table.Create(tblPath.c_str(), 4, FGTGT_POINT, false, false));
    table.Close();

    // Set nValidRecordCount = 1 in .gdbtable so .gdbtablx is opened on read
    VSILFILE* fpTbl = VSIFOpenL(tblPath.c_str(), "r+b");
    ASSERT_NE(fpTbl, nullptr);
    uint32_t nOne = 1;
    VSIFSeekL(fpTbl, 4, SEEK_SET);
    VSIFWriteL(&nOne, 4, 1, fpTbl);
    VSIFCloseL(fpTbl);

    // Overwrite .gdbtablx version at offset 0 with 4 (while table is v3)
    VSILFILE* fp = VSIFOpenL(tablxPath.c_str(), "r+b");
    ASSERT_NE(fp, nullptr);
    uint32_t ver4 = 4;
    VSIFWriteL(&ver4, 4, 1, fp);
    VSIFCloseL(fp);

    FileGDBTable roTable;
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_FALSE(roTable.Open(tblPath.c_str(), false));
    CPLPopErrorHandler();
    roTable.Close();

    // Secondary header version 3 with bUpdate=true (filegdbtable.cpp:992-998)
    fpTbl = VSIFOpenL(tblPath.c_str(), "r+b");
    ASSERT_NE(fpTbl, nullptr);
    uint64_t nOffsetFieldDesc = 0;
    VSIFSeekL(fpTbl, 32, SEEK_SET);
    ASSERT_EQ(VSIFReadL(&nOffsetFieldDesc, 8, 1, fpTbl), 1u);
    uint32_t secVer3 = 3;
    VSIFSeekL(fpTbl, nOffsetFieldDesc + 4, SEEK_SET);
    VSIFWriteL(&secVer3, 4, 1, fpTbl);
    VSIFCloseL(fpTbl);

    // Revert tablx version back to 3
    fp = VSIFOpenL(tablxPath.c_str(), "r+b");
    ASSERT_NE(fp, nullptr);
    uint32_t ver3 = 3;
    VSIFWriteL(&ver3, 4, 1, fp);
    VSIFCloseL(fp);

    FileGDBTable roTableUpd;
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_FALSE(roTableUpd.Open(tblPath.c_str(), true));
    CPLPopErrorHandler();
    roTableUpd.Close();
  }

  // 7. Field descriptor variations: unknown geom type, UTF-16 default value,
  // unknown raster type, unhandled field type
  {
    std::string tblPath =
        std::string(kTestDir) + "/fld_desc_variations.gdbtable";
    std::vector<GByte> fieldDesc;
    for (int i = 0; i < 4; ++i) fieldDesc.push_back(0);
    WriteUInt32(fieldDesc, 4);  // v4
    WriteUInt32(fieldDesc,
                99);  // unknown table geom type 99 (triggers line 1012)
    WriteUInt16(fieldDesc, 2);  // 2 fields

    // Field 0: FGFT_STRING with UTF-16 default value ("hi") in table with
    // m_bStringsAreUTF8 = false
    fieldDesc.push_back(1);
    fieldDesc.push_back('s');
    fieldDesc.push_back(0);
    fieldDesc.push_back(0);  // alias
    fieldDesc.push_back(static_cast<GByte>(FGFT_STRING));
    WriteUInt32(fieldDesc, 20);  // max width
    fieldDesc.push_back(FileGDBField::MASK_EDITABLE |
                        FileGDBField::MASK_NULLABLE);
    // default value: UTF-16 "hi" (4 bytes)
    WriteVarUInt(fieldDesc, 4);
    fieldDesc.push_back('h');
    fieldDesc.push_back(0);
    fieldDesc.push_back('i');
    fieldDesc.push_back(0);

    // Field 1: FGFT_RASTER with unknown raster type 99 (triggers line 1340)
    fieldDesc.push_back(1);
    fieldDesc.push_back('r');
    fieldDesc.push_back(0);
    fieldDesc.push_back(0);  // alias
    fieldDesc.push_back(static_cast<GByte>(FGFT_RASTER));
    fieldDesc.push_back(0);
    fieldDesc.push_back(1);  // nullable
    fieldDesc.push_back(1);  // col name len
    fieldDesc.push_back('c');
    fieldDesc.push_back(0);
    WriteUInt16(fieldDesc, 0);  // wkt len
    fieldDesc.push_back(0);     // flags
    fieldDesc.push_back(99);    // unknown raster type

    fieldDesc.push_back(0xDE);
    fieldDesc.push_back(0xAD);
    fieldDesc.push_back(0xBE);
    fieldDesc.push_back(0xEF);

    uint32_t nDescSize = static_cast<uint32_t>(fieldDesc.size() - 4);
    memcpy(fieldDesc.data(), &nDescSize, 4);

    std::vector<GByte> tblHdr(40, 0);
    uint32_t ver3 = 3;
    uint32_t nRecCount = 0;
    uint64_t nOffsetFieldDesc = 40;
    memcpy(tblHdr.data(), &ver3, 4);
    memcpy(tblHdr.data() + 4, &nRecCount, 4);
    memcpy(tblHdr.data() + 32, &nOffsetFieldDesc, 8);

    VSILFILE* fp = VSIFOpenL(tblPath.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(tblHdr.data(), 40, 1, fp);
    VSIFWriteL(fieldDesc.data(), fieldDesc.size(), 1, fp);
    VSIFCloseL(fp);

    std::string tablxPath = CPLResetExtension(tblPath.c_str(), "gdbtablx");
    std::vector<GByte> tablxHdr(16, 0);
    memcpy(tablxHdr.data(), &ver3, 4);
    fp = VSIFOpenL(tablxPath.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(tablxHdr.data(), 16, 1, fp);
    VSIFCloseL(fp);

    FileGDBTable roTable;
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_TRUE(roTable.Open(tblPath.c_str(), false));
    CPLPopErrorHandler();
    roTable.Close();

    // Unhandled field type 99 (triggers lines 1067-1069)
    std::string unhPath = std::string(kTestDir) + "/unhandled_fld.gdbtable";
    std::vector<GByte> unhDesc;
    for (int i = 0; i < 4; ++i) unhDesc.push_back(0);
    WriteUInt32(unhDesc, 4);
    WriteUInt32(unhDesc, 0);
    WriteUInt16(unhDesc, 1);
    unhDesc.push_back(1);
    unhDesc.push_back('u');
    unhDesc.push_back(0);
    unhDesc.push_back(0);
    unhDesc.push_back(99);  // invalid field type > FGFT_DATETIME_WITH_OFFSET
    unhDesc.push_back(0xDE);
    unhDesc.push_back(0xAD);
    unhDesc.push_back(0xBE);
    unhDesc.push_back(0xEF);
    uint32_t nUnhSize = static_cast<uint32_t>(unhDesc.size() - 4);
    memcpy(unhDesc.data(), &nUnhSize, 4);

    fp = VSIFOpenL(unhPath.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(tblHdr.data(), 40, 1, fp);
    VSIFWriteL(unhDesc.data(), unhDesc.size(), 1, fp);
    VSIFCloseL(fp);

    std::string unhTablxPath = CPLResetExtension(unhPath.c_str(), "gdbtablx");
    fp = VSIFOpenL(unhTablxPath.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(tablxHdr.data(), 16, 1, fp);
    VSIFCloseL(fp);

    FileGDBTable roTableUnh;
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_FALSE(roTableUnh.Open(unhPath.c_str(), false));
    CPLPopErrorHandler();
    roTableUnh.Close();
  }

  // 8. SelectRow: blob length > 100MB, GetAndSelectNextNonEmptyRow
  {
    std::string tblPath = std::string(kTestDir) + "/large_row.gdbtable";
    FileGDBTable table;
    ASSERT_TRUE(table.Create(tblPath.c_str(), 4, FGTGT_POINT, false, false));
    auto fldStr = std::make_unique<FileGDBField>(
        "s", "", FGFT_STRING, true, false, true, 20, FileGDBField::UNSET_FIELD);
    ASSERT_TRUE(table.CreateField(std::move(fldStr)));
    auto rawFields = MakeUnsetFields(table.GetFieldCount());
    int fid = 0;
    OGRPoint pt(1.0, 2.0);
    rawFields[table.GetFieldIdx("s")].String = CPLStrdup("hi");
    ASSERT_TRUE(table.CreateFeature(rawFields, &pt, &fid));
    CPLFree(rawFields[table.GetFieldIdx("s")].String);
    table.Sync();
    table.Close();

    // Call GetAndSelectNextNonEmptyRow on opened table
    {
      FileGDBTable roTable;
      ASSERT_TRUE(roTable.Open(tblPath.c_str(), false));
      EXPECT_EQ(roTable.GetAndSelectNextNonEmptyRow(100), -1);
      roTable.Close();
    }

    // Overwrite row length in .gdbtable at the feature offset to 105MB (105 *
    // 1024 * 1024)
    vsi_l_offset nFeatOffset = 0;
    std::string tablxPath = CPLResetExtension(tblPath.c_str(), "gdbtablx");
    VSILFILE* fpTablx = VSIFOpenL(tablxPath.c_str(), "rb");
    ASSERT_NE(fpTablx, nullptr);
    VSIFSeekL(fpTablx, 16, SEEK_SET);
    uint32_t off32 = 0;
    ASSERT_EQ(VSIFReadL(&off32, 4, 1, fpTablx), 1u);
    VSIFCloseL(fpTablx);
    nFeatOffset = off32;

    VSILFILE* fp = VSIFOpenL(tblPath.c_str(), "r+b");
    ASSERT_NE(fp, nullptr);
    VSIFSeekL(fp, nFeatOffset, SEEK_SET);
    uint32_t nHugeSize = 105 * 1024 * 1024;
    VSIFWriteL(&nHugeSize, 4, 1, fp);
    VSIFCloseL(fp);

    FileGDBTable roTable;
    ASSERT_TRUE(roTable.Open(tblPath.c_str(), false));
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_FALSE(roTable.SelectRow(0));
    CPLPopErrorHandler();
    roTable.Close();
  }

  // 9. FGFT_DATETIME_WITH_OFFSET field reading and skip loop
  {
    std::string tblPath = std::string(kTestDir) + "/dt_offset.gdbtable";
    std::vector<GByte> fieldDesc;
    for (int i = 0; i < 4; ++i) fieldDesc.push_back(0);
    WriteUInt32(fieldDesc, 4);
    WriteUInt32(fieldDesc, 0);
    WriteUInt16(fieldDesc, 2);  // 2 fields: d1 and d2
    // d1
    fieldDesc.push_back(2);
    fieldDesc.push_back('d');
    fieldDesc.push_back(0);
    fieldDesc.push_back('1');
    fieldDesc.push_back(0);
    fieldDesc.push_back(0);  // alias
    fieldDesc.push_back(static_cast<GByte>(FGFT_DATETIME_WITH_OFFSET));
    fieldDesc.push_back(10);  // length: sizeof(double) + sizeof(int16)
    fieldDesc.push_back(1);   // nullable
    fieldDesc.push_back(0);   // def val len
    // d2
    fieldDesc.push_back(2);
    fieldDesc.push_back('d');
    fieldDesc.push_back(0);
    fieldDesc.push_back('2');
    fieldDesc.push_back(0);
    fieldDesc.push_back(0);  // alias
    fieldDesc.push_back(static_cast<GByte>(FGFT_DATETIME_WITH_OFFSET));
    fieldDesc.push_back(10);  // length: sizeof(double) + sizeof(int16)
    fieldDesc.push_back(1);   // nullable
    fieldDesc.push_back(0);   // def val len
    fieldDesc.push_back(0xDE);
    fieldDesc.push_back(0xAD);
    fieldDesc.push_back(0xBE);
    fieldDesc.push_back(0xEF);
    uint32_t nDescSize = static_cast<uint32_t>(fieldDesc.size() - 4);
    memcpy(fieldDesc.data(), &nDescSize, 4);

    std::vector<GByte> rowBuf;
    rowBuf.push_back(0);  // not null
    // d1 value
    WriteFloat64(rowBuf, 40000.0);  // date double
    int16_t nOffsetMin1 = 120;      // UTC+2 hours (120 minutes)
    rowBuf.push_back(static_cast<GByte>(nOffsetMin1 & 0xFF));
    rowBuf.push_back(static_cast<GByte>((nOffsetMin1 >> 8) & 0xFF));
    // d2 value
    WriteFloat64(rowBuf, 41000.0);
    int16_t nOffsetMin2 = -300;  // UTC-5 hours (-300 minutes)
    rowBuf.push_back(static_cast<GByte>(nOffsetMin2 & 0xFF));
    rowBuf.push_back(static_cast<GByte>((nOffsetMin2 >> 8) & 0xFF));

    std::vector<GByte> tblHdr(40, 0);
    uint32_t ver3 = 3;
    uint32_t nRecCount = 1;
    uint32_t nBufMaxSize = 200;
    uint64_t nOffsetFieldDesc = 40;
    uint64_t nFileSize = 40 + fieldDesc.size() + 4 + rowBuf.size() + 4;
    memcpy(tblHdr.data(), &ver3, 4);
    memcpy(tblHdr.data() + 4, &nRecCount, 4);
    memcpy(tblHdr.data() + 8, &nBufMaxSize, 4);
    memcpy(tblHdr.data() + 24, &nFileSize, 8);
    memcpy(tblHdr.data() + 32, &nOffsetFieldDesc, 8);

    VSILFILE* fp = VSIFOpenL(tblPath.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(tblHdr.data(), 40, 1, fp);
    VSIFWriteL(fieldDesc.data(), fieldDesc.size(), 1, fp);
    uint32_t nRowSize = static_cast<uint32_t>(rowBuf.size());
    VSIFWriteL(&nRowSize, 4, 1, fp);
    VSIFWriteL(rowBuf.data(), rowBuf.size(), 1, fp);
    GByte zeros[4] = {0};
    VSIFWriteL(zeros, 4, 1, fp);
    VSIFCloseL(fp);

    FileGDBTable roTable;
    ASSERT_TRUE(roTable.Open(tblPath.c_str(), false));
    EXPECT_EQ(roTable.GetTotalRecordCount(), 1);
    ASSERT_TRUE(roTable.SelectRow(0));
    // Read col 1 first to trigger skip loop for col 0 (lines 2094-2096)
    const OGRField* fld1 = roTable.GetFieldValue(1);
    ASSERT_NE(fld1, nullptr);
    EXPECT_NE(fld1->Date.Year, 0);
    // Then read col 0
    const OGRField* fld0 = roTable.GetFieldValue(0);
    ASSERT_NE(fld0, nullptr);
    EXPECT_NE(fld0->Date.Year, 0);
    roTable.Close();
  }

  // 10. Truncated 1-byte rows for 8 field types in GetFieldValue error branches
  {
    std::string tblPath = std::string(kTestDir) + "/trunc_fields.gdbtable";
    std::string tablxPath = std::string(kTestDir) + "/trunc_fields.gdbtablx";
    FileGDBTable table;
    ASSERT_TRUE(table.Create(tblPath.c_str(), 4, FGTGT_NONE, false, false));

    std::vector<std::pair<std::string, FileGDBFieldType>> fldDefs = {
        {"f_str", FGFT_STRING},  {"f_i16", FGFT_INT16},
        {"f_i32", FGFT_INT32},   {"f_f32", FGFT_FLOAT32},
        {"f_f64", FGFT_FLOAT64}, {"f_dt", FGFT_DATETIME},
        {"f_bin", FGFT_BINARY},  {"f_i64", FGFT_INT64}};

    for (const auto& [name, type] : fldDefs) {
      int maxWidth = (type == FGFT_STRING) ? 50 : 0;
      auto fld =
          std::make_unique<FileGDBField>(name, "", type, true, false, true,
                                         maxWidth, FileGDBField::UNSET_FIELD);
      ASSERT_TRUE(table.CreateField(std::move(fld)));
    }

    auto rawFields = MakeUnsetFields(table.GetFieldCount());
    for (int k = 0; k < 8; ++k) {
      int fid = 0;
      ASSERT_TRUE(table.CreateFeature(rawFields, nullptr, &fid));
    }
    table.Sync();
    table.Close();

    // Read the 8 feature offsets from .gdbtablx and overwrite their rows to
    // 1-byte blobs
    VSILFILE* fpTablx = VSIFOpenL(tablxPath.c_str(), "rb");
    ASSERT_NE(fpTablx, nullptr);
    std::vector<uint32_t> offsets(8, 0);
    VSIFSeekL(fpTablx, 16, SEEK_SET);
    ASSERT_EQ(VSIFReadL(offsets.data(), 4, 8, fpTablx), 8u);
    VSIFCloseL(fpTablx);

    VSILFILE* fpTbl = VSIFOpenL(tblPath.c_str(), "r+b");
    ASSERT_NE(fpTbl, nullptr);
    for (int k = 0; k < 8; ++k) {
      VSIFSeekL(fpTbl, offsets[k], SEEK_SET);
      uint32_t nRowSize = 1;
      VSIFWriteL(&nRowSize, 4, 1, fpTbl);
      uint8_t nullBitmap = static_cast<uint8_t>((1 << k) - 1);
      VSIFWriteL(&nullBitmap, 1, 1, fpTbl);
    }
    VSIFCloseL(fpTbl);

    FileGDBTable roTable;
    ASSERT_TRUE(roTable.Open(tblPath.c_str(), false));
    EXPECT_EQ(roTable.GetTotalRecordCount(), 8);

    CPLPushErrorHandler(CPLQuietErrorHandler);
    for (int k = 0; k < 8; ++k) {
      ASSERT_TRUE(roTable.SelectRow(k));
      EXPECT_EQ(roTable.GetFieldValue(k), nullptr);
      EXPECT_TRUE(roTable.HasGotError());
    }

    // On row 0 (field 0 is not null), call GetFieldValue(1) to hit the
    // skip-loop ReadVarUInt32 error on field 0
    ASSERT_TRUE(roTable.SelectRow(1));
    ASSERT_TRUE(roTable.SelectRow(0));
    EXPECT_EQ(roTable.GetFieldValue(1), nullptr);
    EXPECT_TRUE(roTable.HasGotError());
    CPLPopErrorHandler();

    roTable.Close();
  }
}

}  // namespace
}  // namespace autotest2
