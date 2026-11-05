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

#include "third_party/gdal/ogr/ogr_wkb.h"

#include <climits>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::DoubleNear;
using ::testing::Eq;
using ::testing::Ne;
using ::testing::NotNull;

std::vector<GByte> GeomToWkb(const OGRGeometry& geom,
                             OGRwkbByteOrder eByteOrder = wkbNDR,
                             OGRwkbVariant eVariant = wkbVariantIso) {
  size_t size = geom.WkbSize();
  std::vector<GByte> buf(size);
  geom.exportToWkb(eByteOrder, buf.data(), eVariant);
  return buf;
}

std::vector<GByte> WktToWkb(const char* pszWkt,
                            OGRwkbByteOrder eByteOrder = wkbNDR,
                            OGRwkbVariant eVariant = wkbVariantIso) {
  auto [geom, err] = OGRGeometryFactory::createFromWkt(pszWkt);
  if (!geom) return {};
  return GeomToWkb(*geom, eByteOrder, eVariant);
}

std::unique_ptr<OGRGeometry> WkbToGeom(const void* pabyWkb,
                                       size_t nSize = static_cast<size_t>(-1),
                                       OGRwkbVariant eVariant = wkbVariantIso) {
  OGRGeometry* poGeom = nullptr;
  if (OGRGeometryFactory::createFromWkb(pabyWkb, nullptr, &poGeom, nSize,
                                        eVariant) == OGRERR_NONE) {
    return std::unique_ptr<OGRGeometry>(poGeom);
  }
  delete poGeom;
  return nullptr;
}

class OgrWkbTest : public ::testing::Test {
 protected:
  void TearDown() override { OSRCleanup(); }
};

TEST_F(OgrWkbTest, GetGeomType) {
  // Less than 5 bytes
  const GByte short_buf[4] = {0x01, 0x01, 0x00, 0x00};
  bool bNeedSwap = false;
  uint32_t nType = 0;
  EXPECT_FALSE(
      OGRWKBGetGeomType(short_buf, sizeof(short_buf), bNeedSwap, nType));

  // Valid NDR Point
  const GByte ndr_point[5] = {0x01, 0x01, 0x00, 0x00, 0x00};
  EXPECT_TRUE(
      OGRWKBGetGeomType(ndr_point, sizeof(ndr_point), bNeedSwap, nType));
#if CPL_IS_LSB
  EXPECT_FALSE(bNeedSwap);
#else
  EXPECT_TRUE(bNeedSwap);
#endif
  EXPECT_EQ(1u, nType);

  // Valid XDR Point
  const GByte xdr_point[5] = {0x00, 0x00, 0x00, 0x00, 0x01};
  EXPECT_TRUE(
      OGRWKBGetGeomType(xdr_point, sizeof(xdr_point), bNeedSwap, nType));
#if CPL_IS_LSB
  EXPECT_TRUE(bNeedSwap);
#else
  EXPECT_FALSE(bNeedSwap);
#endif
  EXPECT_EQ(1u, nType);
}

TEST_F(OgrWkbTest, PolygonGetArea) {
  const char* wkt_poly =
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 4 2, 4 4, 2 4, 2 2))";
  auto poly = OGRGeometryFactory::createFromWkt(wkt_poly).first;
  ASSERT_THAT(poly, NotNull());
  double expected_area = poly->toPolygon()->get_Area();

  for (auto byte_order : {wkbNDR, wkbXDR}) {
    auto wkb = GeomToWkb(*poly, byte_order);
    const GByte* ptr = wkb.data();
    size_t size = wkb.size();
    double area = 0.0;
    EXPECT_TRUE(OGRWKBPolygonGetArea(ptr, size, area));
    EXPECT_DOUBLE_EQ(expected_area, area);
  }

  // 3D, M, ZM variants
  for (const char* wkt : {
           "POLYGON Z ((0 0 1, 10 0 1, 10 10 1, 0 10 1, 0 0 1))",
           "POLYGON M ((0 0 2, 10 0 2, 10 10 2, 0 10 2, 0 0 2))",
           "POLYGON ZM ((0 0 1 2, 10 0 1 2, 10 10 1 2, 0 10 1 2, 0 0 1 2))",
       }) {
    auto geom = OGRGeometryFactory::createFromWkt(wkt).first;
    ASSERT_THAT(geom, NotNull());
    for (auto byte_order : {wkbNDR, wkbXDR}) {
      auto wkb = GeomToWkb(*geom, byte_order);
      const GByte* ptr = wkb.data();
      size_t size = wkb.size();
      double area = 0.0;
      EXPECT_TRUE(OGRWKBPolygonGetArea(ptr, size, area));
      EXPECT_DOUBLE_EQ(100.0, area);
    }
  }

  // Error cases: truncated, non-polygon type, degenerate rings
  {
    // < 9 bytes
    const GByte trunc[8] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00};
    const GByte* ptr = trunc;
    size_t size = sizeof(trunc);
    double area = 0.0;
    EXPECT_FALSE(OGRWKBPolygonGetArea(ptr, size, area));
  }
  {
    // Non-polygon type (e.g. Point = 1)
    const GByte point_wkb[9] = {0x01, 0x01, 0x00, 0x00, 0x00, 0, 0, 0, 0};
    const GByte* ptr = point_wkb;
    size_t size = sizeof(point_wkb);
    double area = 0.0;
    EXPECT_FALSE(OGRWKBPolygonGetArea(ptr, size, area));
  }
  {
    // Truncated at ring point count
    const GByte bad_ring[11] = {
        0x01, 0x03, 0x00, 0x00, 0x00,  // Polygon
        0x01, 0x00, 0x00, 0x00,        // 1 ring
        0x01, 0x00                     // 2 bytes only (< 4 bytes for nPoints)
    };
    const GByte* ptr = bad_ring;
    size_t size = sizeof(bad_ring);
    double area = 0.0;
    EXPECT_FALSE(OGRWKBPolygonGetArea(ptr, size, area));
  }
  {
    // Ring with < 4 points
    const GByte bad_points[13] = {
        0x01, 0x03, 0x00, 0x00, 0x00,  // Polygon
        0x01, 0x00, 0x00, 0x00,        // 1 ring
        0x02, 0x00, 0x00, 0x00         // 2 points (< 4)
    };
    const GByte* ptr = bad_points;
    size_t size = sizeof(bad_points);
    double area = 0.0;
    EXPECT_FALSE(OGRWKBPolygonGetArea(ptr, size, area));
  }
}

TEST_F(OgrWkbTest, MultiPolygonGetArea) {
  const char* wkt =
      "MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0)), ((20 0, 25 0, 25 5, 20 "
      "5, 20 0)))";
  auto mp = OGRGeometryFactory::createFromWkt(wkt).first;
  ASSERT_THAT(mp, NotNull());
  double expected_area = mp->toMultiPolygon()->get_Area();

  for (auto byte_order : {wkbNDR, wkbXDR}) {
    auto wkb = GeomToWkb(*mp, byte_order);
    const GByte* ptr = wkb.data();
    size_t size = wkb.size();
    double area = 0.0;
    EXPECT_TRUE(OGRWKBMultiPolygonGetArea(ptr, size, area));
    EXPECT_DOUBLE_EQ(expected_area, area);
  }

  // MultiPolygon Z, M, ZM
  for (const char* wkt_z : {
           "MULTIPOLYGON Z (((0 0 1, 10 0 1, 10 10 1, 0 10 1, 0 0 1)))",
           "MULTIPOLYGON M (((0 0 2, 10 0 2, 10 10 2, 0 10 2, 0 0 2)))",
           "MULTIPOLYGON ZM (((0 0 1 2, 10 0 1 2, 10 10 1 2, 0 10 1 2, 0 0 1 "
           "2)))",
       }) {
    auto geom = OGRGeometryFactory::createFromWkt(wkt_z).first;
    ASSERT_THAT(geom, NotNull());
    for (auto byte_order : {wkbNDR, wkbXDR}) {
      auto wkb = GeomToWkb(*geom, byte_order);
      const GByte* ptr = wkb.data();
      size_t size = wkb.size();
      double area = 0.0;
      EXPECT_TRUE(OGRWKBMultiPolygonGetArea(ptr, size, area));
      EXPECT_DOUBLE_EQ(100.0, area);
    }
  }

  // Error cases: truncated, non-multipolygon type
  {
    const GByte trunc[8] = {0x01, 0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00};
    const GByte* ptr = trunc;
    size_t size = sizeof(trunc);
    double area = 0.0;
    EXPECT_FALSE(OGRWKBMultiPolygonGetArea(ptr, size, area));
  }
  {
    const GByte not_mp[] = {
        0x01, 0x06, 0x00, 0x00, 0x00,  // MultiPolygon
        0x01, 0x00, 0x00, 0x00,        // 1 part
        0x01, 0x01, 0x00, 0x00, 0x00,  // Point (not polygon)
        0,    0,    0,    0,    0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    const GByte* ptr = not_mp;
    size_t size = sizeof(not_mp);
    double area = 0.0;
    EXPECT_FALSE(OGRWKBMultiPolygonGetArea(ptr, size, area));
  }
}

TEST_F(OgrWkbTest, WkbFromEWKB) {
  // Truncated < 5 bytes
  GByte short_buf[4] = {0x01, 0x01, 0x00, 0x00};
  size_t out_size = 0;
  int srid = 0;
  EXPECT_EQ(nullptr,
            WKBFromEWKB(short_buf, sizeof(short_buf), out_size, &srid));

  // Regular WKB (no SRID flag)
  GByte reg_wkb[] = {
      0x01, 0x01, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0,
      0,    0,    0,    0,    0,    0, 0, 0, 0, 0,
  };
  const GByte* ret = WKBFromEWKB(reg_wkb, sizeof(reg_wkb), out_size, &srid);
  EXPECT_EQ(reg_wkb, ret);
  EXPECT_EQ(sizeof(reg_wkb), out_size);
  EXPECT_EQ(INT_MIN, srid);

  // EWKB with SRID (NDR)
  // Byte order: 0x01
  // Type: wkbPoint (1) | 0x20000000 = 0x20000001
  // SRID: 4326 = 0x000010E6
  // Coordinates: (1.0, 2.0)
  GByte ewkb_ndr[] = {
      0x01, 0x01, 0x00, 0x00, 0x20,                    // type with SRID flag
      0xe6, 0x10, 0x00, 0x00,                          // SRID 4326
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x3f,  // 1.0
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40,  // 2.0
  };
  ret = WKBFromEWKB(ewkb_ndr, sizeof(ewkb_ndr), out_size, &srid);
  EXPECT_EQ(ewkb_ndr + 4, ret);
  EXPECT_EQ(sizeof(ewkb_ndr) - 4, out_size);
  EXPECT_EQ(4326, srid);
  // Verify stripped WKB header: byte order 0x01, type 0x00000001
  EXPECT_EQ(0x01, ret[0]);
  uint32_t stripped_type;
  memcpy(&stripped_type, ret + 1, sizeof(uint32_t));
  EXPECT_EQ(1u, stripped_type);

  // EWKB with SRID (XDR)
  GByte ewkb_xdr[] = {
      0x00, 0x20, 0x00, 0x00, 0x01,  // type with SRID flag in big endian
      0x00, 0x00, 0x10, 0xe6,        // SRID 4326
      0x3f, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 1.0
      0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 2.0
  };
  ret = WKBFromEWKB(ewkb_xdr, sizeof(ewkb_xdr), out_size, nullptr);
  EXPECT_EQ(ewkb_xdr + 4, ret);
  EXPECT_EQ(sizeof(ewkb_xdr) - 4, out_size);

  // EWKB with SRID flag but <= 9 bytes -> treated as regular WKB (not stripped)
  GByte ewkb_short[] = {0x01, 0x01, 0x00, 0x00, 0x20, 0xe6, 0x10, 0x00};
  EXPECT_EQ(ewkb_short,
            WKBFromEWKB(ewkb_short, sizeof(ewkb_short), out_size, &srid));
}

TEST_F(OgrWkbTest, GetBoundingBoxAllGeomTypes) {
  const std::vector<const char*> wkts = {
      "POINT (1 2)",
      "POINT Z (1 2 3)",
      "POINT M (1 2 4)",
      "POINT ZM (1 2 3 4)",
      "POINT EMPTY",
      "LINESTRING (0 0, 2 3)",
      "LINESTRING Z (0 0 1, 2 3 5)",
      "LINESTRING M (0 0 1, 2 3 5)",
      "LINESTRING ZM (0 0 1 2, 2 3 5 6)",
      "CIRCULARSTRING (0 0, 1 1, 2 0)",
      "CIRCULARSTRING Z (0 0 1, 1 1 2, 2 0 3)",
      "CIRCULARSTRING M (0 0 1, 1 1 2, 2 0 3)",
      "CIRCULARSTRING ZM (0 0 1 4, 1 1 2 5, 2 0 3 6)",
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), (1 1, 2 1, 2 2, 1 2, 1 1))",
      "POLYGON Z ((0 0 1, 4 0 2, 4 4 3, 0 4 4, 0 0 1))",
      "POLYGON M ((0 0 1, 4 0 2, 4 4 3, 0 4 4, 0 0 1))",
      "POLYGON ZM ((0 0 1 1, 4 0 2 2, 4 4 3 3, 0 4 4 4, 0 0 1 1))",
      "TRIANGLE ((0 0, 1 0, 0 1, 0 0))",
      "TRIANGLE Z ((0 0 1, 1 0 2, 0 1 3, 0 0 1))",
      "MULTIPOINT ((0 0), (3 4))",
      "MULTIPOINT Z ((0 0 1), (3 4 5))",
      "MULTIPOINT EMPTY",
      "MULTILINESTRING ((0 0, 1 1), (2 2, 3 5))",
      "MULTILINESTRING Z ((0 0 1, 1 1 2), (2 2 3, 3 5 6))",
      "MULTIPOLYGON (((0 0, 1 0, 1 1, 0 0)), ((2 2, 5 2, 5 6, 2 2)))",
      "MULTIPOLYGON Z (((0 0 1, 1 0 1, 1 1 1, 0 0 1)))",
      "GEOMETRYCOLLECTION (POINT (1 2), LINESTRING (0 0, 3 4))",
      "COMPOUNDCURVE ((0 0, 1 1), CIRCULARSTRING (1 1, 2 2, 3 1))",
      "CURVEPOLYGON (CIRCULARSTRING (0 0, 1 1, 2 0, 1 -1, 0 0))",
      "MULTICURVE ((0 0, 1 1), CIRCULARSTRING (1 1, 2 2, 3 1))",
      "MULTISURFACE (((0 0, 2 0, 2 2, 0 0)))",
      "POLYHEDRALSURFACE (((0 0 0, 0 1 0, 1 1 0, 0 0 0)))",
      "TIN (((0 0 0, 0 1 0, 1 0 0, 0 0 0)))",
  };

  for (const char* wkt : wkts) {
    auto geom = OGRGeometryFactory::createFromWkt(wkt).first;
    ASSERT_THAT(geom, NotNull()) << "Failed to parse: " << wkt;

    OGREnvelope expected_env;
    geom->getEnvelope(&expected_env);
    OGREnvelope3D expected_env3d;
    geom->getEnvelope(&expected_env3d);

    for (auto byte_order : {wkbNDR, wkbXDR}) {
      auto wkb = GeomToWkb(*geom, byte_order);

      OGREnvelope env;
      EXPECT_TRUE(OGRWKBGetBoundingBox(wkb.data(), wkb.size(), env))
          << "Failed on: " << wkt;
      if (!geom->IsEmpty()) {
        EXPECT_NEAR(expected_env.MinX, env.MinX, 1e-6);
        EXPECT_NEAR(expected_env.MaxX, env.MaxX, 1e-6);
        EXPECT_NEAR(expected_env.MinY, env.MinY, 1e-6);
        EXPECT_NEAR(expected_env.MaxY, env.MaxY, 1e-6);
      }

      OGREnvelope3D env3d;
      EXPECT_TRUE(OGRWKBGetBoundingBox(wkb.data(), wkb.size(), env3d))
          << "Failed 3D on: " << wkt;
      if (!geom->IsEmpty()) {
        EXPECT_NEAR(expected_env3d.MinX, env3d.MinX, 1e-6);
        EXPECT_NEAR(expected_env3d.MaxX, env3d.MaxX, 1e-6);
        EXPECT_NEAR(expected_env3d.MinY, env3d.MinY, 1e-6);
        EXPECT_NEAR(expected_env3d.MaxY, env3d.MaxY, 1e-6);
        if (geom->Is3D()) {
          EXPECT_NEAR(expected_env3d.MinZ, env3d.MinZ, 1e-6);
          EXPECT_NEAR(expected_env3d.MaxZ, env3d.MaxZ, 1e-6);
        }
      }
    }
  }

  // Error cases: truncated, invalid byte order, unknown geom type
  {
    OGREnvelope env;
    const GByte short_wkb[4] = {0x01, 0x01, 0x00, 0x00};
    EXPECT_FALSE(OGRWKBGetBoundingBox(short_wkb, sizeof(short_wkb), env));
  }
  {
    OGREnvelope env;
    const GByte bad_endian[9] = {0x03, 0x01, 0x00, 0x00, 0x00, 0, 0, 0, 0};
    EXPECT_FALSE(OGRWKBGetBoundingBox(bad_endian, sizeof(bad_endian), env));
  }
  {
    OGREnvelope env;
    const GByte unknown_type[9] = {0x01, 0x99, 0x99, 0x00, 0x00, 0, 0, 0, 0};
    EXPECT_FALSE(OGRWKBGetBoundingBox(unknown_type, sizeof(unknown_type), env));
  }
}

TEST_F(OgrWkbTest, IntersectsPessimistic) {
  const char* wkt_poly = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))";
  auto poly = OGRGeometryFactory::createFromWkt(wkt_poly).first;
  ASSERT_THAT(poly, NotNull());

  OGREnvelope intersecting_env;
  intersecting_env.MinX = 5.0;
  intersecting_env.MaxX = 15.0;
  intersecting_env.MinY = 5.0;
  intersecting_env.MaxY = 15.0;

  OGREnvelope disjoint_env;
  disjoint_env.MinX = 100.0;
  disjoint_env.MaxX = 110.0;
  disjoint_env.MinY = 100.0;
  disjoint_env.MaxY = 110.0;

  for (auto byte_order : {wkbNDR, wkbXDR}) {
    auto wkb = GeomToWkb(*poly, byte_order);
    EXPECT_TRUE(
        OGRWKBIntersectsPessimistic(wkb.data(), wkb.size(), intersecting_env));
    EXPECT_FALSE(
        OGRWKBIntersectsPessimistic(wkb.data(), wkb.size(), disjoint_env));
  }

  // Point, Point EMPTY, LineString, GeometryCollection
  for (const char* wkt : {
           "POINT (5 5)",
           "POINT EMPTY",
           "LINESTRING (0 0, 5 5)",
           "CIRCULARSTRING (4 4, 6 6, 8 4)",
           "TRIANGLE ((4 4, 10 4, 7 10, 4 4))",
           "MULTIPOINT ((5 5), (6 6))",
           "MULTILINESTRING ((0 0, 5 5))",
           "MULTIPOLYGON (((0 0, 5 0, 5 5, 0 5, 0 0)))",
           "GEOMETRYCOLLECTION (POINT (5 5))",
       }) {
    auto geom = OGRGeometryFactory::createFromWkt(wkt).first;
    ASSERT_THAT(geom, NotNull());
    for (auto byte_order : {wkbNDR, wkbXDR}) {
      auto wkb = GeomToWkb(*geom, byte_order);
      if (geom->IsEmpty()) {
        EXPECT_FALSE(OGRWKBIntersectsPessimistic(wkb.data(), wkb.size(),
                                                 intersecting_env));
      } else {
        EXPECT_TRUE(OGRWKBIntersectsPessimistic(wkb.data(), wkb.size(),
                                                intersecting_env));
      }
      EXPECT_FALSE(
          OGRWKBIntersectsPessimistic(wkb.data(), wkb.size(), disjoint_env));
    }
  }

  // Error cases
  const GByte bad_wkb[4] = {0x01, 0x01, 0x00, 0x00};
  EXPECT_FALSE(
      OGRWKBIntersectsPessimistic(bad_wkb, sizeof(bad_wkb), intersecting_env));
}

TEST_F(OgrWkbTest, FixupCounterClockWiseExternalRing) {
  // Clockwise exterior ring: (0 0, 0 10, 10 10, 10 0, 0 0)
  // Counter-clockwise interior ring: (2 2, 4 2, 4 4, 2 4, 2 2)
  const char* wkt =
      "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), (2 2, 4 2, 4 4, 2 4, 2 2))";
  auto poly = OGRGeometryFactory::createFromWkt(wkt).first;
  ASSERT_THAT(poly, NotNull());

  for (auto byte_order : {wkbNDR, wkbXDR}) {
    auto wkb = GeomToWkb(*poly, byte_order);
    OGRWKBFixupCounterClockWiseExternalRing(wkb.data(), wkb.size());

    // Import back and check that exterior ring is now counter-clockwise
    auto fixed = WkbToGeom(wkb.data(), wkb.size());
    ASSERT_THAT(fixed, NotNull());
    auto* fixed_poly = fixed->toPolygon();
    ASSERT_THAT(fixed_poly, NotNull());
    EXPECT_FALSE(fixed_poly->getExteriorRing()->isClockwise());
    EXPECT_TRUE(fixed_poly->getInteriorRing(0)->isClockwise());
  }

  // 3D, M, ZM variants
  for (const char* wkt_z : {
           "POLYGON Z ((0 0 1, 0 10 1, 10 10 1, 10 0 1, 0 0 1))",
           "POLYGON M ((0 0 2, 0 10 2, 10 10 2, 10 0 2, 0 0 2))",
           "POLYGON ZM ((0 0 1 2, 0 10 1 2, 10 10 1 2, 10 0 1 2, 0 0 1 2))",
       }) {
    auto geom = OGRGeometryFactory::createFromWkt(wkt_z).first;
    ASSERT_THAT(geom, NotNull());
    auto wkb = GeomToWkb(*geom, wkbNDR);
    OGRWKBFixupCounterClockWiseExternalRing(wkb.data(), wkb.size());
    auto fixed = WkbToGeom(wkb.data(), wkb.size());
    ASSERT_THAT(fixed, NotNull());
    EXPECT_FALSE(fixed->toPolygon()->getExteriorRing()->isClockwise());
  }

  // MultiPolygon and GeometryCollection containing clockwise polygons
  const char* wkt_mp = "MULTIPOLYGON (((0 0, 0 10, 10 10, 10 0, 0 0)))";
  auto mp = OGRGeometryFactory::createFromWkt(wkt_mp).first;
  ASSERT_THAT(mp, NotNull());
  auto mp_wkb = GeomToWkb(*mp, wkbNDR);
  OGRWKBFixupCounterClockWiseExternalRing(mp_wkb.data(), mp_wkb.size());
  auto fixed_mp = WkbToGeom(mp_wkb.data(), mp_wkb.size());
  ASSERT_THAT(fixed_mp, NotNull());
  EXPECT_FALSE(fixed_mp->toMultiPolygon()
                   ->getGeometryRef(0)
                   ->getExteriorRing()
                   ->isClockwise());

  // Degenerate polygon triggering Green's formula fallback: duplicate
  // lowest-rightmost vertex
  const char* wkt_fallback = "POLYGON ((0 0, 10 0, 10 0, 10 10, 0 10, 0 0))";
  auto geom_fb = OGRGeometryFactory::createFromWkt(wkt_fallback).first;
  ASSERT_THAT(geom_fb, NotNull());
  auto fb_wkb = GeomToWkb(*geom_fb, wkbNDR);
  OGRWKBFixupCounterClockWiseExternalRing(fb_wkb.data(), fb_wkb.size());
}

class TestPointUpdater : public OGRWKBPointUpdater {
 public:
  double dx = 10.0;
  double dy = 20.0;
  double dz = 30.0;
  double dm = 40.0;
  bool fail = false;

  bool update(bool bNeedSwap, void* x, void* y, void* z, void* m) override {
    if (fail) return false;
    double dfX, dfY;
    memcpy(&dfX, x, sizeof(double));
    memcpy(&dfY, y, sizeof(double));
    if (bNeedSwap) {
      CPL_SWAP64PTR(&dfX);
      CPL_SWAP64PTR(&dfY);
    }
    dfX += dx;
    dfY += dy;
    if (bNeedSwap) {
      CPL_SWAP64PTR(&dfX);
      CPL_SWAP64PTR(&dfY);
    }
    memcpy(x, &dfX, sizeof(double));
    memcpy(y, &dfY, sizeof(double));

    if (z) {
      double dfZ;
      memcpy(&dfZ, z, sizeof(double));
      if (bNeedSwap) CPL_SWAP64PTR(&dfZ);
      dfZ += dz;
      if (bNeedSwap) CPL_SWAP64PTR(&dfZ);
      memcpy(z, &dfZ, sizeof(double));
    }
    if (m) {
      double dfM;
      memcpy(&dfM, m, sizeof(double));
      if (bNeedSwap) CPL_SWAP64PTR(&dfM);
      dfM += dm;
      if (bNeedSwap) CPL_SWAP64PTR(&dfM);
      memcpy(m, &dfM, sizeof(double));
    }
    return true;
  }
};

TEST_F(OgrWkbTest, UpdatePoints) {
  const std::vector<const char*> wkts = {
      "POINT (1 2)",
      "POINT Z (1 2 3)",
      "POINT M (1 2 4)",
      "POINT ZM (1 2 3 4)",
      "POINT EMPTY",
      "LINESTRING (0 0, 1 1)",
      "CIRCULARSTRING (0 0, 1 1, 2 0)",
      "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))",
      "TRIANGLE ((0 0, 1 0, 0 1, 0 0))",
      "MULTIPOINT ((0 0), (1 1))",
      "MULTILINESTRING ((0 0, 1 1))",
      "MULTIPOLYGON (((0 0, 1 0, 1 1, 0 0)))",
      "GEOMETRYCOLLECTION (POINT (1 2))",
      "COMPOUNDCURVE ((0 0, 1 1), CIRCULARSTRING (1 1, 2 2, 3 1))",
      "CURVEPOLYGON (CIRCULARSTRING (0 0, 1 1, 2 0, 1 -1, 0 0))",
      "MULTICURVE ((0 0, 1 1))",
      "MULTISURFACE (((0 0, 1 0, 1 1, 0 0)))",
      "POLYHEDRALSURFACE (((0 0 0, 0 1 0, 1 1 0, 0 0 0)))",
      "TIN (((0 0 0, 0 1 0, 1 0 0, 0 0 0)))",
  };

  for (const char* wkt : wkts) {
    auto geom = OGRGeometryFactory::createFromWkt(wkt).first;
    ASSERT_THAT(geom, NotNull());

    for (auto byte_order : {wkbNDR, wkbXDR}) {
      auto wkb = GeomToWkb(*geom, byte_order);
      TestPointUpdater updater;
      EXPECT_TRUE(OGRWKBUpdatePoints(wkb.data(), wkb.size(), updater))
          << "Failed on: " << wkt;

      if (!geom->IsEmpty()) {
        auto updated = WkbToGeom(wkb.data(), wkb.size());
        ASSERT_THAT(updated, NotNull());
        OGREnvelope orig_env, updated_env;
        geom->getEnvelope(&orig_env);
        updated->getEnvelope(&updated_env);
        EXPECT_NEAR(orig_env.MinX + 10.0, updated_env.MinX, 1e-6);
        EXPECT_NEAR(orig_env.MinY + 20.0, updated_env.MinY, 1e-6);
      }
    }
  }

  // Failure from updater
  {
    auto wkb = WktToWkb("POINT (1 2)");
    TestPointUpdater fail_updater;
    fail_updater.fail = true;
    EXPECT_FALSE(OGRWKBUpdatePoints(wkb.data(), wkb.size(), fail_updater));
  }
}

class ShiftCT : public OGRCoordinateTransformation {
 public:
  OGRSpatialReference* GetSourceCS() const override { return nullptr; }
  OGRSpatialReference* GetTargetCS() const override { return nullptr; }
  int Transform(size_t nCount, double* x, double* y, double* z, double* m,
                int* pabSuccess) override {
    for (size_t i = 0; i < nCount; ++i) {
      x[i] += 5.0;
      y[i] += 5.0;
      if (z) z[i] += 5.0;
      if (pabSuccess) pabSuccess[i] = TRUE;
    }
    return TRUE;
  }
  OGRCoordinateTransformation* Clone() const override { return new ShiftCT(); }
  OGRCoordinateTransformation* GetInverse() const override { return nullptr; }
};

TEST_F(OgrWkbTest, Transform) {
  ShiftCT ct;
  OGRWKBTransformCache cache;

  const std::vector<const char*> wkts = {
      "POINT (1 2)",
      "POINT Z (1 2 3)",
      "POINT EMPTY",
      "LINESTRING (0 0, 1 1)",
      "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
      "GEOMETRYCOLLECTION (POINT (1 2), POINT EMPTY)",
  };

  for (const char* wkt : wkts) {
    auto geom = OGRGeometryFactory::createFromWkt(wkt).first;
    ASSERT_THAT(geom, NotNull());

    for (auto byte_order : {wkbNDR, wkbXDR}) {
      auto wkb = GeomToWkb(*geom, byte_order);
      OGREnvelope3D env;
      EXPECT_TRUE(OGRWKBTransform(wkb.data(), wkb.size(), &ct, cache, env))
          << "Failed on: " << wkt;
    }
  }

  // Truncated WKB error
  const GByte bad_wkb[4] = {0x01, 0x01, 0x00, 0x00};
  OGREnvelope3D env;
  EXPECT_FALSE(OGRWKBTransform(const_cast<GByte*>(bad_wkb), sizeof(bad_wkb),
                               &ct, cache, env));
}

class TestAppendBuffer : public OGRAppendBuffer {
 public:
  bool fail_grow = false;
  std::vector<GByte> storage;

  TestAppendBuffer() {
    m_nCapacity = 0;
    m_nSize = 0;
    m_pRawBuffer = nullptr;
  }

 protected:
  bool Grow(size_t nItemSize) override {
    if (fail_grow) return false;
    size_t new_cap = m_nSize + nItemSize + 1024;
    storage.resize(new_cap);
    m_nCapacity = new_cap;
    m_pRawBuffer = storage.data();
    return true;
  }
};

TEST_F(OgrWkbTest, AppendBufferAndWktToWkbTranslator) {
  TestAppendBuffer buf;
  OGRWKTToWKBTranslator translator(buf);

  // Fast path: MULTIPOLYGON single-part single-ring with bCanAlterByteAfter =
  // true
  std::string wkt_fast = "MULTIPOLYGON (((0 0, 1 0, 1 1, 0 0)))";
  std::vector<char> wkt_buf(wkt_fast.begin(), wkt_fast.end());
  wkt_buf.push_back('\0');  // extra byte after
  size_t bytes = translator.TranslateWKT(wkt_buf.data(), wkt_fast.size(), true);
  EXPECT_NE(static_cast<size_t>(-1), bytes);
  EXPECT_EQ(bytes, buf.GetSize());

  // Fast path variants: Z, M, ZM
  std::string wkt_z = "MULTIPOLYGON Z (((0 0 1, 1 0 2, 1 1 3, 0 0 1)))";
  std::vector<char> buf_z(wkt_z.begin(), wkt_z.end());
  buf_z.push_back('\0');
  EXPECT_NE(static_cast<size_t>(-1),
            translator.TranslateWKT(buf_z.data(), wkt_z.size(), true));

  std::string wkt_m = "MULTIPOLYGON M (((0 0 1, 1 0 2, 1 1 3, 0 0 1)))";
  std::vector<char> buf_m(wkt_m.begin(), wkt_m.end());
  buf_m.push_back('\0');
  EXPECT_NE(static_cast<size_t>(-1),
            translator.TranslateWKT(buf_m.data(), wkt_m.size(), true));

  std::string wkt_zm =
      "MULTIPOLYGON ZM (((0 0 1 2, 1 0 2 3, 1 1 3 4, 0 0 1 2)))";
  std::vector<char> buf_zm(wkt_zm.begin(), wkt_zm.end());
  buf_zm.push_back('\0');
  EXPECT_NE(static_cast<size_t>(-1),
            translator.TranslateWKT(buf_zm.data(), wkt_zm.size(), true));

  // Multi-ring fallback in fast path
  std::string wkt_multi_ring =
      "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 0), (1 1, 2 1, 2 2, 1 1)))";
  std::vector<char> buf_mr(wkt_multi_ring.begin(), wkt_multi_ring.end());
  buf_mr.push_back('\0');
  EXPECT_NE(
      static_cast<size_t>(-1),
      translator.TranslateWKT(buf_mr.data(), wkt_multi_ring.size(), true));

  // Invalid coordinates in fast path
  std::string bad_coords = "MULTIPOLYGON (((0 0, 1)))";
  std::vector<char> bad_buf(bad_coords.begin(), bad_coords.end());
  bad_buf.push_back('\0');
  {
    WithQuietHandler quiet;
    EXPECT_EQ(static_cast<size_t>(-1),
              translator.TranslateWKT(bad_buf.data(), bad_coords.size(), true));
  }

  // Grow failure in fast path
  TestAppendBuffer fail_fast_buf;
  fail_fast_buf.fail_grow = true;
  OGRWKTToWKBTranslator fail_fast_trans(fail_fast_buf);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(
        static_cast<size_t>(-1),
        fail_fast_trans.TranslateWKT(wkt_buf.data(), wkt_fast.size(), true));
  }

  // General case: POINT with bCanAlterByteAfter = true and false
  std::string pt_wkt = "POINT (1 2)";
  std::vector<char> pt_buf(pt_wkt.begin(), pt_wkt.end());
  pt_buf.push_back('\0');
  EXPECT_NE(static_cast<size_t>(-1),
            translator.TranslateWKT(pt_buf.data(), pt_wkt.size(), true));
  EXPECT_NE(static_cast<size_t>(-1),
            translator.TranslateWKT(pt_buf.data(), pt_wkt.size(), false));

  // Invalid WKT
  std::string invalid_wkt = "NOT_A_GEOMETRY (1 2)";
  std::vector<char> inv_buf(invalid_wkt.begin(), invalid_wkt.end());
  inv_buf.push_back('\0');
  {
    WithQuietHandler quiet;
    EXPECT_EQ(
        static_cast<size_t>(-1),
        translator.TranslateWKT(inv_buf.data(), invalid_wkt.size(), true));
  }

  // Grow failure in general case
  TestAppendBuffer fail_gen_buf;
  fail_gen_buf.fail_grow = true;
  OGRWKTToWKBTranslator fail_gen_trans(fail_gen_buf);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(static_cast<size_t>(-1),
              fail_gen_trans.TranslateWKT(pt_buf.data(), pt_wkt.size(), false));
  }
}

}  // namespace
}  // namespace autotest2
