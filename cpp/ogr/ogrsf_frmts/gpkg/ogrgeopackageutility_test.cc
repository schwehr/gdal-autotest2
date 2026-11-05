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

// Unit tests for GeoPackage utility functions.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/gpkg/ogrgeopackageutility.cpp
//   https://gdal.org/drivers/vector/gpkg.html

#include "third_party/gdal/ogr/ogrsf_frmts/gpkg/ogrgeopackageutility.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gpkg/ogr_geopackage.h"
#include "third_party/gdal/ogr/ogrsf_frmts/sqlite/ogrsqlitebase.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/sqlite/sqlite3.h"

namespace autotest2 {
namespace {

struct GeometryTestCase {
  const char* gpkg_type;
  bool has_z;
  bool has_m;
  OGRwkbGeometryType expected;
};

class OGRGeoPackageUtilityTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { RegisterOGRGeoPackage(); }

  static void TearDownTestSuite() { GDALDestroyDriverManager(); }
};

TEST_F(OGRGeoPackageUtilityTest, GeometryTypeToWKB) {
  const std::vector<GeometryTestCase> cases = {
      {"Point", false, false, wkbPoint},
      {"Point", true, false, wkbSetZ(wkbPoint)},
      {"Point", false, true, wkbSetM(wkbPoint)},
      {"Point", true, true, wkbSetM(wkbSetZ(wkbPoint))},
      {"LineString", false, false, wkbLineString},
      {"LineString", true, false, wkbSetZ(wkbLineString)},
      {"LineString", false, true, wkbSetM(wkbLineString)},
      {"LineString", true, true, wkbSetM(wkbSetZ(wkbLineString))},
      {"Polygon", false, false, wkbPolygon},
      {"Polygon", true, false, wkbSetZ(wkbPolygon)},
      {"Polygon", false, true, wkbSetM(wkbPolygon)},
      {"Polygon", true, true, wkbSetM(wkbSetZ(wkbPolygon))},
      {"MultiPoint", false, false, wkbMultiPoint},
      {"MultiPoint", true, false, wkbSetZ(wkbMultiPoint)},
      {"MultiPoint", false, true, wkbSetM(wkbMultiPoint)},
      {"MultiPoint", true, true, wkbSetM(wkbSetZ(wkbMultiPoint))},
      {"MultiLineString", false, false, wkbMultiLineString},
      {"MultiLineString", true, false, wkbSetZ(wkbMultiLineString)},
      {"MultiLineString", false, true, wkbSetM(wkbMultiLineString)},
      {"MultiLineString", true, true, wkbSetM(wkbSetZ(wkbMultiLineString))},
      {"MultiPolygon", false, false, wkbMultiPolygon},
      {"MultiPolygon", true, false, wkbSetZ(wkbMultiPolygon)},
      {"MultiPolygon", false, true, wkbSetM(wkbMultiPolygon)},
      {"MultiPolygon", true, true, wkbSetM(wkbSetZ(wkbMultiPolygon))},
      {"GeometryCollection", false, false, wkbGeometryCollection},
      {"GeometryCollection", true, false, wkbSetZ(wkbGeometryCollection)},
      {"GeometryCollection", false, true, wkbSetM(wkbGeometryCollection)},
      {"GeometryCollection", true, true,
       wkbSetM(wkbSetZ(wkbGeometryCollection))},
      {"GeomCollection", false, false, wkbGeometryCollection},
      {"GeomCollection", true, false, wkbSetZ(wkbGeometryCollection)},
      {"CircularString", false, false, wkbCircularString},
      {"CircularString", true, false, wkbSetZ(wkbCircularString)},
      {"CircularString", false, true, wkbSetM(wkbCircularString)},
      {"CircularString", true, true, wkbSetM(wkbSetZ(wkbCircularString))},
      {"CompoundCurve", false, false, wkbCompoundCurve},
      {"CompoundCurve", true, false, wkbSetZ(wkbCompoundCurve)},
      {"CompoundCurve", false, true, wkbSetM(wkbCompoundCurve)},
      {"CompoundCurve", true, true, wkbSetM(wkbSetZ(wkbCompoundCurve))},
      {"CurvePolygon", false, false, wkbCurvePolygon},
      {"CurvePolygon", true, false, wkbSetZ(wkbCurvePolygon)},
      {"CurvePolygon", false, true, wkbSetM(wkbCurvePolygon)},
      {"CurvePolygon", true, true, wkbSetM(wkbSetZ(wkbCurvePolygon))},
      {"MultiCurve", false, false, wkbMultiCurve},
      {"MultiCurve", true, false, wkbSetZ(wkbMultiCurve)},
      {"MultiCurve", false, true, wkbSetM(wkbMultiCurve)},
      {"MultiCurve", true, true, wkbSetM(wkbSetZ(wkbMultiCurve))},
      {"MultiSurface", false, false, wkbMultiSurface},
      {"MultiSurface", true, false, wkbSetZ(wkbMultiSurface)},
      {"MultiSurface", false, true, wkbSetM(wkbMultiSurface)},
      {"MultiSurface", true, true, wkbSetM(wkbSetZ(wkbMultiSurface))},
      {"Curve", false, false, wkbCurve},
      {"Curve", true, false, wkbSetZ(wkbCurve)},
      {"Curve", false, true, wkbSetM(wkbCurve)},
      {"Curve", true, true, wkbSetM(wkbSetZ(wkbCurve))},
      {"Surface", false, false, wkbSurface},
      {"Surface", true, false, wkbSetZ(wkbSurface)},
      {"Surface", false, true, wkbSetM(wkbSurface)},
      {"Surface", true, true, wkbSetM(wkbSetZ(wkbSurface))},
      {"PolyhedralSurface", false, false, wkbPolyhedralSurface},
      {"PolyhedralSurface", true, false, wkbSetZ(wkbPolyhedralSurface)},
      {"PolyhedralSurface", false, true, wkbSetM(wkbPolyhedralSurface)},
      {"PolyhedralSurface", true, true, wkbSetM(wkbSetZ(wkbPolyhedralSurface))},
      {"TIN", false, false, wkbTIN},
      {"TIN", true, false, wkbSetZ(wkbTIN)},
      {"TIN", false, true, wkbSetM(wkbTIN)},
      {"TIN", true, true, wkbSetM(wkbSetZ(wkbTIN))},
      {"Triangle", false, false, wkbTriangle},
      {"Triangle", true, false, wkbSetZ(wkbTriangle)},
      {"Triangle", false, true, wkbSetM(wkbTriangle)},
      {"Triangle", true, true, wkbSetM(wkbSetZ(wkbTriangle))},
      {"Geometry", false, false, wkbUnknown},
      {"Geometry", true, false, wkbSetZ(wkbUnknown)},
      {"Geometry", false, true, wkbSetM(wkbUnknown)},
      {"Geometry", true, true, wkbSetM(wkbSetZ(wkbUnknown))},
      {"NonExistentType", false, false, wkbNone},
  };

  for (const auto& c : cases) {
    EXPECT_EQ(c.expected, GPkgGeometryTypeToWKB(c.gpkg_type, c.has_z, c.has_m))
        << "Failed for type: " << c.gpkg_type << " z=" << c.has_z
        << " m=" << c.has_m;
  }
}

TEST_F(OGRGeoPackageUtilityTest, FieldToOGR) {
  OGRFieldSubType sub_type = OFSTNone;
  int max_width = 0;

  EXPECT_EQ(OFTInteger64, GPkgFieldToOGR("INT", sub_type, max_width));
  EXPECT_EQ(OFSTNone, sub_type);
  EXPECT_EQ(0, max_width);

  EXPECT_EQ(OFTInteger64, GPkgFieldToOGR("INTEGER", sub_type, max_width));
  EXPECT_EQ(OFTInteger, GPkgFieldToOGR("TINYINT", sub_type, max_width));
  EXPECT_EQ(OFTInteger, GPkgFieldToOGR("SMALLINT", sub_type, max_width));
  EXPECT_EQ(OFSTInt16, sub_type);

  EXPECT_EQ(OFTInteger, GPkgFieldToOGR("MEDIUMINT", sub_type, max_width));
  EXPECT_EQ(OFSTNone, sub_type);

  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(OFTInteger64, GPkgFieldToOGR("INT4", sub_type, max_width));
    EXPECT_EQ(OFTInteger64, GPkgFieldToOGR("INT8", sub_type, max_width));
    EXPECT_EQ(OFTMaxType + 1, GPkgFieldToOGR("BIGINT", sub_type, max_width));
  }

  EXPECT_EQ(OFTInteger, GPkgFieldToOGR("BOOLEAN", sub_type, max_width));
  EXPECT_EQ(OFSTBoolean, sub_type);

  EXPECT_EQ(OFTReal, GPkgFieldToOGR("FLOAT", sub_type, max_width));
  EXPECT_EQ(OFSTFloat32, sub_type);

  EXPECT_EQ(OFTReal, GPkgFieldToOGR("DOUBLE", sub_type, max_width));
  EXPECT_EQ(OFSTNone, sub_type);
  EXPECT_EQ(OFTReal, GPkgFieldToOGR("REAL", sub_type, max_width));
  EXPECT_EQ(OFTReal, GPkgFieldToOGR("NUMERIC", sub_type, max_width));

  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(OFTMaxType + 1, GPkgFieldToOGR("FLOAT4", sub_type, max_width));
  }

  EXPECT_EQ(OFTString, GPkgFieldToOGR("TEXT", sub_type, max_width));
  EXPECT_EQ(0, max_width);

  EXPECT_EQ(OFTString, GPkgFieldToOGR("TEXT(42)", sub_type, max_width));
  EXPECT_EQ(42, max_width);

  EXPECT_EQ(OFTString, GPkgFieldToOGR("TEXT(0)", sub_type, max_width));
  EXPECT_EQ(0, max_width);

  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(OFTString, GPkgFieldToOGR("TEXT(abc)", sub_type, max_width));
    EXPECT_EQ(OFTString, GPkgFieldToOGR("TEXT_BAD", sub_type, max_width));
  }

  EXPECT_EQ(OFTBinary, GPkgFieldToOGR("BLOB", sub_type, max_width));
  EXPECT_EQ(0, max_width);

  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(OFTBinary, GPkgFieldToOGR("BLOB(128)", sub_type, max_width));
    EXPECT_EQ(OFTBinary, GPkgFieldToOGR("BLOB(xyz)", sub_type, max_width));
    EXPECT_EQ(OFTBinary, GPkgFieldToOGR("BLOB_EXTRA", sub_type, max_width));
  }

  EXPECT_EQ(OFTDate, GPkgFieldToOGR("DATE", sub_type, max_width));
  EXPECT_EQ(OFTDateTime, GPkgFieldToOGR("DATETIME", sub_type, max_width));

  // Geometry type passed as field type.
  EXPECT_EQ(OFTMaxType + 1, GPkgFieldToOGR("POINT", sub_type, max_width));

  // Completely unknown type.
  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(OFTMaxType + 1,
              GPkgFieldToOGR("COMPLETELY_UNKNOWN_TYPE", sub_type, max_width));
  }
}

TEST_F(OGRGeoPackageUtilityTest, FieldFromOGR) {
  EXPECT_STREQ("BOOLEAN", GPkgFieldFromOGR(OFTInteger, OFSTBoolean, 0));
  EXPECT_STREQ("SMALLINT", GPkgFieldFromOGR(OFTInteger, OFSTInt16, 0));
  EXPECT_STREQ("MEDIUMINT", GPkgFieldFromOGR(OFTInteger, OFSTNone, 0));

  EXPECT_STREQ("INTEGER", GPkgFieldFromOGR(OFTInteger64, OFSTNone, 0));

  EXPECT_STREQ("FLOAT", GPkgFieldFromOGR(OFTReal, OFSTFloat32, 0));
  EXPECT_STREQ("REAL", GPkgFieldFromOGR(OFTReal, OFSTNone, 0));

  EXPECT_STREQ("TEXT", GPkgFieldFromOGR(OFTString, OFSTNone, 0));
  EXPECT_STREQ("TEXT(50)", GPkgFieldFromOGR(OFTString, OFSTNone, 50));

  EXPECT_STREQ("BLOB", GPkgFieldFromOGR(OFTBinary, OFSTNone, 0));
  EXPECT_STREQ("DATE", GPkgFieldFromOGR(OFTDate, OFSTNone, 0));
  EXPECT_STREQ("DATETIME", GPkgFieldFromOGR(OFTDateTime, OFSTNone, 0));

  // Fallback type.
  EXPECT_STREQ("TEXT", GPkgFieldFromOGR(OFTIntegerList, OFSTNone, 0));
}

TEST_F(OGRGeoPackageUtilityTest, GeometryFromOGRAndToOGR) {
  // Empty point (in GPKG, Point has no envelope).
  OGRPoint empty_pt;
  empty_pt.empty();
  size_t gpkg_len = 0;
  GByte* paby = GPkgGeometryFromOGR(&empty_pt, 4326, nullptr, &gpkg_len);
  ASSERT_NE(nullptr, paby);
  EXPECT_GE(gpkg_len, 8u);
  GPkgHeader hdr;
  EXPECT_EQ(OGRERR_NONE, GPkgHeaderFromWKB(paby, gpkg_len, &hdr));
  EXPECT_TRUE(hdr.bEmpty);
  EXPECT_EQ(4326, hdr.iSrsId);

  std::unique_ptr<OGRGeometry> geom_back(
      GPkgGeometryToOGR(paby, gpkg_len, nullptr));
  ASSERT_NE(nullptr, geom_back);
  EXPECT_TRUE(geom_back->IsEmpty());
  EXPECT_EQ(wkbPoint, wkbFlatten(geom_back->getGeometryType()));
  CPLFree(paby);

  // 2D Point.
  OGRPoint pt(1.5, 2.5);
  paby = GPkgGeometryFromOGR(&pt, 4326, nullptr, &gpkg_len);
  ASSERT_NE(nullptr, paby);
  EXPECT_EQ(OGRERR_NONE, GPkgHeaderFromWKB(paby, gpkg_len, &hdr));
  EXPECT_FALSE(hdr.bEmpty);
  // Points have no envelope in GPKG.
  EXPECT_FALSE(hdr.bExtentHasXY);

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  geom_back.reset(GPkgGeometryToOGR(paby, gpkg_len, &srs));
  ASSERT_NE(nullptr, geom_back);
  EXPECT_EQ(wkbPoint, geom_back->getGeometryType());
  OGRPoint* pt_back = geom_back->toPoint();
  EXPECT_DOUBLE_EQ(1.5, pt_back->getX());
  EXPECT_DOUBLE_EQ(2.5, pt_back->getY());
  EXPECT_NE(nullptr, geom_back->getSpatialReference());
  CPLFree(paby);

  // 2D LineString (gets XY envelope).
  OGRLineString line;
  line.addPoint(1.0, 2.0);
  line.addPoint(3.0, 4.0);
  paby = GPkgGeometryFromOGR(&line, 4326, nullptr, &gpkg_len);
  ASSERT_NE(nullptr, paby);
  EXPECT_EQ(OGRERR_NONE, GPkgHeaderFromWKB(paby, gpkg_len, &hdr));
  EXPECT_TRUE(hdr.bExtentHasXY);
  EXPECT_FALSE(hdr.bExtentHasZ);
  EXPECT_DOUBLE_EQ(1.0, hdr.MinX);
  EXPECT_DOUBLE_EQ(3.0, hdr.MaxX);
  EXPECT_DOUBLE_EQ(2.0, hdr.MinY);
  EXPECT_DOUBLE_EQ(4.0, hdr.MaxY);
  CPLFree(paby);

  // 3D LineString (gets XYZ envelope).
  OGRLineString line_z;
  line_z.addPoint(1.0, 2.0, 10.0);
  line_z.addPoint(3.0, 4.0, 20.0);
  paby = GPkgGeometryFromOGR(&line_z, 4326, nullptr, &gpkg_len);
  ASSERT_NE(nullptr, paby);
  EXPECT_EQ(OGRERR_NONE, GPkgHeaderFromWKB(paby, gpkg_len, &hdr));
  EXPECT_TRUE(hdr.bExtentHasXY);
  EXPECT_TRUE(hdr.bExtentHasZ);
  EXPECT_DOUBLE_EQ(10.0, hdr.MinZ);
  EXPECT_DOUBLE_EQ(20.0, hdr.MaxZ);
  geom_back.reset(GPkgGeometryToOGR(paby, gpkg_len, nullptr));
  ASSERT_NE(nullptr, geom_back);
  EXPECT_EQ(wkbSetZ(wkbLineString), geom_back->getGeometryType());
  CPLFree(paby);

  // Empty Polygon.
  OGRPolygon empty_poly;
  empty_poly.empty();
  paby = GPkgGeometryFromOGR(&empty_poly, 4326, nullptr, &gpkg_len);
  ASSERT_NE(nullptr, paby);
  EXPECT_EQ(OGRERR_NONE, GPkgHeaderFromWKB(paby, gpkg_len, &hdr));
  EXPECT_TRUE(hdr.bEmpty);
  geom_back.reset(GPkgGeometryToOGR(paby, gpkg_len, nullptr));
  ASSERT_NE(nullptr, geom_back);
  EXPECT_TRUE(geom_back->IsEmpty());
  CPLFree(paby);

  // With coordinate precision.
  OGRGeomCoordinateBinaryPrecision prec;
  prec.nXYBitPrecision = 10;
  prec.nZBitPrecision = 10;
  paby = GPkgGeometryFromOGR(&line_z, 4326, &prec, &gpkg_len);
  ASSERT_NE(nullptr, paby);
  geom_back.reset(GPkgGeometryToOGR(paby, gpkg_len, nullptr));
  ASSERT_NE(nullptr, geom_back);
  CPLFree(paby);
}

TEST_F(OGRGeoPackageUtilityTest, HeaderFromWKBAndHeaderUpdate) {
  // Truncated buffer (< 8 bytes).
  const GByte short_buf[6] = {'G', 'P', 0, 1, 0, 0};
  GPkgHeader hdr;
  EXPECT_NE(OGRERR_NONE, GPkgHeaderFromWKB(short_buf, sizeof(short_buf), &hdr));

  // Invalid magic.
  const GByte bad_magic[8] = {'B', 'A', 0, 1, 0, 0, 0, 0};
  EXPECT_NE(OGRERR_NONE, GPkgHeaderFromWKB(bad_magic, sizeof(bad_magic), &hdr));

  // Invalid version.
  const GByte bad_version[8] = {'G', 'P', 1, 1, 0, 0, 0, 0};
  EXPECT_NE(OGRERR_NONE,
            GPkgHeaderFromWKB(bad_version, sizeof(bad_version), &hdr));

  // Invalid envelope indicator (5).
  const GByte bad_env[8] = {'G', 'P', 0, (5 << 1) | 1, 0, 0, 0, 0};
  EXPECT_NE(OGRERR_NONE, GPkgHeaderFromWKB(bad_env, sizeof(bad_env), &hdr));

  // Truncated envelope buffer for envelope 1 (requires 8 + 32 = 40 bytes).
  std::vector<GByte> truncated_env1(20, 0);
  truncated_env1[0] = 'G';
  truncated_env1[1] = 'P';
  truncated_env1[3] = (1 << 1) | 1;  // envelope 1, little endian
  EXPECT_NE(OGRERR_NONE, GPkgHeaderFromWKB(truncated_env1.data(),
                                           truncated_env1.size(), &hdr));

  // Valid big-endian (wkbXDR) envelope 1.
  std::vector<GByte> xdr_env1(40, 0);
  xdr_env1[0] = 'G';
  xdr_env1[1] = 'P';
  xdr_env1[3] = (1 << 1) | 0;  // envelope 1, big endian
  // SRS ID = 4326 (0x000010E6) big-endian
  xdr_env1[4] = 0x00;
  xdr_env1[5] = 0x00;
  xdr_env1[6] = 0x10;
  xdr_env1[7] = 0xE6;
  EXPECT_EQ(OGRERR_NONE,
            GPkgHeaderFromWKB(xdr_env1.data(), xdr_env1.size(), &hdr));
  EXPECT_EQ(4326, hdr.iSrsId);
  EXPECT_TRUE(hdr.bExtentHasXY);

  // Test GPkgUpdateHeader for envelope sizes (0, 1, 2, 3, 4).
  // Envelope 0:
  std::vector<GByte> buf_env0(8, 0);
  buf_env0[0] = 'G';
  buf_env0[1] = 'P';
  buf_env0[3] = 1;  // envelope 0, little endian
  EXPECT_TRUE(GPkgUpdateHeader(buf_env0.data(), buf_env0.size(), 3857, 0, 0, 0,
                               0, 0, 0));
  EXPECT_EQ(OGRERR_NONE,
            GPkgHeaderFromWKB(buf_env0.data(), buf_env0.size(), &hdr));
  EXPECT_EQ(3857, hdr.iSrsId);

  // Envelope 1 (XY):
  std::vector<GByte> buf_env1(40, 0);
  buf_env1[0] = 'G';
  buf_env1[1] = 'P';
  buf_env1[3] = (1 << 1) | 1;
  EXPECT_TRUE(GPkgUpdateHeader(buf_env1.data(), buf_env1.size(), 4326, 1.0, 2.0,
                               3.0, 4.0, 0, 0));
  EXPECT_EQ(OGRERR_NONE,
            GPkgHeaderFromWKB(buf_env1.data(), buf_env1.size(), &hdr));
  EXPECT_DOUBLE_EQ(1.0, hdr.MinX);
  EXPECT_DOUBLE_EQ(2.0, hdr.MaxX);
  EXPECT_DOUBLE_EQ(3.0, hdr.MinY);
  EXPECT_DOUBLE_EQ(4.0, hdr.MaxY);

  // Envelope 1 big-endian:
  buf_env1[3] = (1 << 1) | 0;
  EXPECT_TRUE(GPkgUpdateHeader(buf_env1.data(), buf_env1.size(), 4326, 1.0, 2.0,
                               3.0, 4.0, 0, 0));
  EXPECT_EQ(OGRERR_NONE,
            GPkgHeaderFromWKB(buf_env1.data(), buf_env1.size(), &hdr));
  EXPECT_DOUBLE_EQ(1.0, hdr.MinX);

  // Envelope 2 (XYZ):
  std::vector<GByte> buf_env2(56, 0);
  buf_env2[0] = 'G';
  buf_env2[1] = 'P';
  buf_env2[3] = (2 << 1) | 1;
  EXPECT_TRUE(GPkgUpdateHeader(buf_env2.data(), buf_env2.size(), 4326, 1.0, 2.0,
                               3.0, 4.0, 5.0, 6.0));
  EXPECT_EQ(OGRERR_NONE,
            GPkgHeaderFromWKB(buf_env2.data(), buf_env2.size(), &hdr));
  EXPECT_DOUBLE_EQ(5.0, hdr.MinZ);
  EXPECT_DOUBLE_EQ(6.0, hdr.MaxZ);

  // Envelope 2 big-endian:
  buf_env2[3] = (2 << 1) | 0;
  EXPECT_TRUE(GPkgUpdateHeader(buf_env2.data(), buf_env2.size(), 4326, 1.0, 2.0,
                               3.0, 4.0, 5.0, 6.0));
  EXPECT_EQ(OGRERR_NONE,
            GPkgHeaderFromWKB(buf_env2.data(), buf_env2.size(), &hdr));
  EXPECT_DOUBLE_EQ(5.0, hdr.MinZ);

  // Envelope 3 (XYM):
  std::vector<GByte> buf_env3(56, 0);
  buf_env3[0] = 'G';
  buf_env3[1] = 'P';
  buf_env3[3] = (3 << 1) | 1;
  EXPECT_TRUE(GPkgUpdateHeader(buf_env3.data(), buf_env3.size(), 4326, 1.0, 2.0,
                               3.0, 4.0, 0, 0));
  EXPECT_EQ(OGRERR_NONE,
            GPkgHeaderFromWKB(buf_env3.data(), buf_env3.size(), &hdr));
  EXPECT_DOUBLE_EQ(1.0, hdr.MinX);

  // Envelope 3 big-endian:
  buf_env3[3] = (3 << 1) | 0;
  EXPECT_TRUE(GPkgUpdateHeader(buf_env3.data(), buf_env3.size(), 4326, 1.0, 2.0,
                               3.0, 4.0, 0, 0));
  EXPECT_EQ(OGRERR_NONE,
            GPkgHeaderFromWKB(buf_env3.data(), buf_env3.size(), &hdr));

  // Envelope 4 (XYZM):
  std::vector<GByte> buf_env4(72, 0);
  buf_env4[0] = 'G';
  buf_env4[1] = 'P';
  buf_env4[3] = (4 << 1) | 1;
  EXPECT_TRUE(GPkgUpdateHeader(buf_env4.data(), buf_env4.size(), 4326, 1.0, 2.0,
                               3.0, 4.0, 5.0, 6.0));
  EXPECT_EQ(OGRERR_NONE,
            GPkgHeaderFromWKB(buf_env4.data(), buf_env4.size(), &hdr));
  EXPECT_DOUBLE_EQ(5.0, hdr.MinZ);

  // Envelope 4 big-endian:
  buf_env4[3] = (4 << 1) | 0;
  EXPECT_TRUE(GPkgUpdateHeader(buf_env4.data(), buf_env4.size(), 4326, 1.0, 2.0,
                               3.0, 4.0, 5.0, 6.0));
  EXPECT_EQ(OGRERR_NONE,
            GPkgHeaderFromWKB(buf_env4.data(), buf_env4.size(), &hdr));
  EXPECT_DOUBLE_EQ(5.0, hdr.MinZ);

  // Invalid envelope code 5 on update.
  std::vector<GByte> buf_env5(72, 0);
  buf_env5[0] = 'G';
  buf_env5[1] = 'P';
  buf_env5[3] = (5 << 1) | 1;
  EXPECT_FALSE(GPkgUpdateHeader(buf_env5.data(), buf_env5.size(), 4326, 0, 0, 0,
                                0, 0, 0));

  // Truncated buffer for update.
  std::vector<GByte> short_upd(20, 0);
  short_upd[0] = 'G';
  short_upd[1] = 'P';
  short_upd[3] = (1 << 1) | 1;
  EXPECT_FALSE(GPkgUpdateHeader(short_upd.data(), short_upd.size(), 4326, 0, 0,
                                0, 0, 0, 0));
}

TEST_F(OGRGeoPackageUtilityTest, GeometryToOGRErrors) {
  // Short buffer.
  const GByte short_buf[4] = {'G', 'P', 0, 0};
  EXPECT_EQ(nullptr, GPkgGeometryToOGR(short_buf, sizeof(short_buf), nullptr));

  // Valid header but corrupt WKB.
  std::vector<GByte> corrupt_wkb(12, 0);
  corrupt_wkb[0] = 'G';
  corrupt_wkb[1] = 'P';
  corrupt_wkb[3] = 1;     // envelope 0, little-endian
  corrupt_wkb[8] = 1;     // wkb byte order
  corrupt_wkb[9] = 0xFF;  // invalid wkb type
  WithQuietHandler quiet_handler;
  EXPECT_EQ(nullptr,
            GPkgGeometryToOGR(corrupt_wkb.data(), corrupt_wkb.size(), nullptr));
}

// Wrapper function to test OGRGeoPackageGetHeader from SQLite context.
static void TestGetHeaderFunc(sqlite3_context* context, int argc,
                              sqlite3_value** argv) {
  int need_extent = sqlite3_user_data(context)
                        ? static_cast<int>(reinterpret_cast<intptr_t>(
                              sqlite3_user_data(context)))
                        : 0;
  bool bNeedExtent = (need_extent & 1) != 0;
  bool bNeedExtent3D = (need_extent & 2) != 0;

  GPkgHeader hdr;
  bool res = OGRGeoPackageGetHeader(context, argc, argv, &hdr, bNeedExtent,
                                    bNeedExtent3D);
  if (!res) {
    sqlite3_result_int(context, 0);
  } else {
    sqlite3_result_int(context, 1);
  }
}

TEST_F(OGRGeoPackageUtilityTest, GetHeaderSQLiteContext) {
  sqlite3* db = nullptr;
  ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));
  absl::Cleanup cleanup_db = [&] { sqlite3_close(db); };

  sqlite3_create_function(db, "get_hdr_0", 1, SQLITE_ANY,
                          reinterpret_cast<void*>(0), TestGetHeaderFunc,
                          nullptr, nullptr);
  sqlite3_create_function(db, "get_hdr_1", 1, SQLITE_ANY,
                          reinterpret_cast<void*>(1), TestGetHeaderFunc,
                          nullptr, nullptr);
  sqlite3_create_function(db, "get_hdr_2", 1, SQLITE_ANY,
                          reinterpret_cast<void*>(2), TestGetHeaderFunc,
                          nullptr, nullptr);
  sqlite3_create_function(db, "get_hdr_3", 1, SQLITE_ANY,
                          reinterpret_cast<void*>(3), TestGetHeaderFunc,
                          nullptr, nullptr);

  auto exec_int = [&](const char* sql) -> int {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
      return -1;
    }
    int val = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      val = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return val;
  };

  // 1. Non-blob argument.
  EXPECT_EQ(0, exec_int("SELECT get_hdr_0(123)"));
  EXPECT_EQ(0, exec_int("SELECT get_hdr_0('text')"));

  // 2. Short blob (< 8 bytes).
  EXPECT_EQ(0, exec_int("SELECT get_hdr_0(X'000102')"));

  // 3. SpatiaLite blob exported via GDAL helper:
  OGRPoint sl_pt(1.0, 2.0);
  GByte* sl_data = nullptr;
  int sl_len = 0;
  ASSERT_EQ(OGRERR_NONE,
            OGRSQLiteExportSpatiaLiteGeometry(&sl_pt, 4326, wkbNDR, true, false,
                                              &sl_data, &sl_len));
  ASSERT_NE(nullptr, sl_data);
  absl::Cleanup cleanup_sl = [&] { CPLFree(sl_data); };

  ASSERT_EQ(SQLITE_OK, sqlite3_exec(db, "CREATE TABLE blobs (b BLOB);", nullptr,
                                    nullptr, nullptr));
  sqlite3_stmt* insert_stmt = nullptr;
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, "INSERT INTO blobs VALUES (?);",
                                          -1, &insert_stmt, nullptr));
  ASSERT_EQ(SQLITE_OK,
            sqlite3_bind_blob(insert_stmt, 1, sl_data, sl_len, SQLITE_STATIC));
  ASSERT_EQ(SQLITE_DONE, sqlite3_step(insert_stmt));
  sqlite3_finalize(insert_stmt);

  // Test SpatiaLite blob with get_hdr_0 and get_hdr_1.
  EXPECT_EQ(1, exec_int("SELECT get_hdr_0(b) FROM blobs WHERE rowid = 1;"));
  EXPECT_EQ(1, exec_int("SELECT get_hdr_1(b) FROM blobs WHERE rowid = 1;"));

  // Empty SpatiaLite blob (empty LineString has nCount == 0, triggering bEmpty
  // = true):
  OGRLineString sl_empty_ls;
  GByte* sl_empty_data = nullptr;
  int sl_empty_len = 0;
  ASSERT_EQ(OGRERR_NONE, OGRSQLiteExportSpatiaLiteGeometry(
                             &sl_empty_ls, 4326, wkbNDR, true, false,
                             &sl_empty_data, &sl_empty_len));
  ASSERT_NE(nullptr, sl_empty_data);
  absl::Cleanup cleanup_sl_empty = [&] { CPLFree(sl_empty_data); };

  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, "INSERT INTO blobs VALUES (?);",
                                          -1, &insert_stmt, nullptr));
  ASSERT_EQ(SQLITE_OK, sqlite3_bind_blob(insert_stmt, 1, sl_empty_data,
                                         sl_empty_len, SQLITE_STATIC));
  ASSERT_EQ(SQLITE_DONE, sqlite3_step(insert_stmt));
  sqlite3_finalize(insert_stmt);

  // For empty SpatiaLite: get_hdr_0 succeeds, get_hdr_1 fails because empty
  // geometry has no extent.
  EXPECT_EQ(1, exec_int("SELECT get_hdr_0(b) FROM blobs WHERE rowid = 2;"));
  EXPECT_EQ(0, exec_int("SELECT get_hdr_1(b) FROM blobs WHERE rowid = 2;"));

  // 4. GPKG empty geometry blob:
  OGRPoint empty_pt;
  empty_pt.empty();
  size_t gpkg_len = 0;
  GByte* paby = GPkgGeometryFromOGR(&empty_pt, 4326, nullptr, &gpkg_len);
  ASSERT_NE(nullptr, paby);
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, "INSERT INTO blobs VALUES (?);",
                                          -1, &insert_stmt, nullptr));
  ASSERT_EQ(SQLITE_OK,
            sqlite3_bind_blob(insert_stmt, 1, paby, static_cast<int>(gpkg_len),
                              SQLITE_STATIC));
  ASSERT_EQ(SQLITE_DONE, sqlite3_step(insert_stmt));
  sqlite3_finalize(insert_stmt);
  CPLFree(paby);

  // Empty GPKG: get_hdr_0 succeeds, get_hdr_1 fails because empty geometry has
  // no extent.
  EXPECT_EQ(1, exec_int("SELECT get_hdr_0(b) FROM blobs WHERE rowid = 3;"));
  EXPECT_EQ(0, exec_int("SELECT get_hdr_1(b) FROM blobs WHERE rowid = 3;"));

  // 5. GPKG 2D Point without envelope:
  OGRPoint pt2d(10.0, 20.0);
  paby = GPkgGeometryFromOGR(&pt2d, 4326, nullptr, &gpkg_len);
  ASSERT_NE(nullptr, paby);
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, "INSERT INTO blobs VALUES (?);",
                                          -1, &insert_stmt, nullptr));
  ASSERT_EQ(SQLITE_OK,
            sqlite3_bind_blob(insert_stmt, 1, paby, static_cast<int>(gpkg_len),
                              SQLITE_STATIC));
  ASSERT_EQ(SQLITE_DONE, sqlite3_step(insert_stmt));
  sqlite3_finalize(insert_stmt);
  CPLFree(paby);

  // get_hdr_1 needs XY extent, parses WKB:
  EXPECT_EQ(1, exec_int("SELECT get_hdr_1(b) FROM blobs WHERE rowid = 4;"));

  // Corrupt WKB in GPKG without envelope:
  std::vector<GByte> gpkg_corrupt(8 + 21, 0);
  gpkg_corrupt[0] = 'G';
  gpkg_corrupt[1] = 'P';
  gpkg_corrupt[3] = 1;     // envelope 0
  gpkg_corrupt[8] = 1;     // NDR
  gpkg_corrupt[9] = 0xFF;  // invalid WKB type
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, "INSERT INTO blobs VALUES (?);",
                                          -1, &insert_stmt, nullptr));
  ASSERT_EQ(SQLITE_OK, sqlite3_bind_blob(insert_stmt, 1, gpkg_corrupt.data(),
                                         static_cast<int>(gpkg_corrupt.size()),
                                         SQLITE_STATIC));
  ASSERT_EQ(SQLITE_DONE, sqlite3_step(insert_stmt));
  sqlite3_finalize(insert_stmt);

  WithQuietHandler quiet_handler;
  EXPECT_EQ(0, exec_int("SELECT get_hdr_1(b) FROM blobs WHERE rowid = 5;"));
  EXPECT_EQ(0, exec_int("SELECT get_hdr_3(b) FROM blobs WHERE rowid = 5;"));

  // 6. GPKG 3D Point:
  OGRPoint pt3d(10.0, 20.0, 30.0);
  paby = GPkgGeometryFromOGR(&pt3d, 4326, nullptr, &gpkg_len);
  ASSERT_NE(nullptr, paby);
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, "INSERT INTO blobs VALUES (?);",
                                          -1, &insert_stmt, nullptr));
  ASSERT_EQ(SQLITE_OK,
            sqlite3_bind_blob(insert_stmt, 1, paby, static_cast<int>(gpkg_len),
                              SQLITE_STATIC));
  ASSERT_EQ(SQLITE_DONE, sqlite3_step(insert_stmt));
  sqlite3_finalize(insert_stmt);
  CPLFree(paby);

  // get_hdr_3 needs 3D extent:
  EXPECT_EQ(1, exec_int("SELECT get_hdr_3(b) FROM blobs WHERE rowid = 6;"));

  // 7. GPKG LineString 3D with envelope 2 (already has Z envelope):
  OGRLineString ls3d;
  ls3d.addPoint(1, 2, 3);
  ls3d.addPoint(4, 5, 6);
  paby = GPkgGeometryFromOGR(&ls3d, 4326, nullptr, &gpkg_len);
  ASSERT_NE(nullptr, paby);
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, "INSERT INTO blobs VALUES (?);",
                                          -1, &insert_stmt, nullptr));
  ASSERT_EQ(SQLITE_OK,
            sqlite3_bind_blob(insert_stmt, 1, paby, static_cast<int>(gpkg_len),
                              SQLITE_STATIC));
  ASSERT_EQ(SQLITE_DONE, sqlite3_step(insert_stmt));
  sqlite3_finalize(insert_stmt);
  CPLFree(paby);

  EXPECT_EQ(1, exec_int("SELECT get_hdr_3(b) FROM blobs WHERE rowid = 7;"));
}

// TODO: b/335317901 - Cover remaining WKB byte-swapping edge cases in
// ogrgeopackageutility.cpp.

}  // namespace
}  // namespace autotest2
