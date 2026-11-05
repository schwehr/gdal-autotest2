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
// Unit tests for ogrpgeogeometry.cpp.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrpgeogeometry.cpp

#include "third_party/gdal/ogr/ogrpgeogeometry.h"

#include <cstring>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr int kSHPP_TRISTRIP = 0;
constexpr int kSHPP_TRIFAN = 1;
constexpr int kSHPP_OUTERRING = 2;
constexpr int kSHPP_INNERRING = 3;
constexpr int kSHPP_FIRSTRING = 4;
constexpr int kSHPP_RING = 5;
constexpr int kSHPP_TRIANGLES = 6;

constexpr int kEXT_SHAPE_SEGMENT_ARC = 1;
constexpr int kEXT_SHAPE_SEGMENT_BEZIER = 4;
constexpr int kEXT_SHAPE_SEGMENT_ELLIPSE = 5;

constexpr int kEXT_SHAPE_ARC_EMPTY = 0x1;
constexpr int kEXT_SHAPE_ARC_CCW = 0x8;
constexpr int kEXT_SHAPE_ARC_MINOR = 0x10;
constexpr int kEXT_SHAPE_ARC_LINE = 0x20;
constexpr int kEXT_SHAPE_ARC_POINT = 0x40;
constexpr int kEXT_SHAPE_ARC_IP = 0x80;

constexpr int kEXT_SHAPE_ELLIPSE_MINOR = 0x1000;
constexpr int kEXT_SHAPE_ELLIPSE_COMPLETE = 0x2000;

void TestRoundtrip(const OGRGeometry* geom) {
  GByte* pabyShape = nullptr;
  int nBytes = 0;
  ASSERT_EQ(OGRWriteToShapeBin(geom, &pabyShape, &nBytes), OGRERR_NONE);
  ASSERT_NE(pabyShape, nullptr);
  ASSERT_GT(nBytes, 0);

  OGRGeometry* poNewGeom = nullptr;
  ASSERT_EQ(OGRCreateFromShapeBin(pabyShape, &poNewGeom, nBytes), OGRERR_NONE);

  if (geom == nullptr || geom->IsEmpty()) {
    EXPECT_EQ(poNewGeom, nullptr);
  } else {
    ASSERT_NE(poNewGeom, nullptr);
    EXPECT_TRUE(geom->Equals(poNewGeom))
        << "Original: " << geom->getGeometryName()
        << ", Reconstructed: " << poNewGeom->getGeometryName();
  }

  VSIFree(pabyShape);
  delete poNewGeom;
}

TEST(OGRPGeoGeometryTest, NullAndEmpty) {
  // nullptr input
  TestRoundtrip(nullptr);

  // Empty point
  OGRPoint emptyPoint;
  emptyPoint.empty();
  TestRoundtrip(&emptyPoint);

  // Empty linestring
  OGRLineString emptyLine;
  TestRoundtrip(&emptyLine);

  // Empty polygon
  OGRPolygon emptyPoly;
  TestRoundtrip(&emptyPoly);

  // Empty multipoint
  OGRMultiPoint emptyMPoint;
  TestRoundtrip(&emptyMPoint);

  // Empty multipolygon
  OGRMultiPolygon emptyMPoly;
  TestRoundtrip(&emptyMPoly);
}

TEST(OGRPGeoGeometryTest, PointsAllVariants) {
  // 2D Point
  OGRPoint pt2D(10.5, 20.5);
  TestRoundtrip(&pt2D);

  // 3D Point (Z)
  OGRPoint pt3D(10.5, 20.5, 30.5);
  TestRoundtrip(&pt3D);

  // 2D Point with M
  OGRPoint ptM;
  ptM.setX(10.5);
  ptM.setY(20.5);
  ptM.setM(40.5);
  TestRoundtrip(&ptM);

  // 4D Point (ZM)
  OGRPoint ptZM(10.5, 20.5, 30.5, 40.5);
  TestRoundtrip(&ptZM);
}

TEST(OGRPGeoGeometryTest, LineStringsAllVariants) {
  // 2D LineString
  OGRLineString ls2D;
  ls2D.addPoint(0, 0);
  ls2D.addPoint(10, 10);
  ls2D.addPoint(20, 0);
  TestRoundtrip(&ls2D);

  // 3D LineString
  OGRLineString ls3D;
  ls3D.addPoint(0, 0, 1);
  ls3D.addPoint(10, 10, 2);
  TestRoundtrip(&ls3D);

  // 2D LineString with M
  OGRLineString lsM;
  lsM.setCoordinateDimension(2);
  lsM.addPoint(0, 0, 0, 5);
  lsM.addPoint(10, 10, 0, 15);
  TestRoundtrip(&lsM);

  // 4D LineString (ZM)
  OGRLineString lsZM;
  lsZM.addPoint(0, 0, 1, 5);
  lsZM.addPoint(10, 10, 2, 15);
  TestRoundtrip(&lsZM);
}

TEST(OGRPGeoGeometryTest, MultiPointsAllVariants) {
  // 2D MultiPoint
  OGRMultiPoint mp2D;
  mp2D.addGeometryDirectly(new OGRPoint(1, 2));
  mp2D.addGeometryDirectly(new OGRPoint(3, 4));
  TestRoundtrip(&mp2D);

  // 3D MultiPoint
  OGRMultiPoint mp3D;
  mp3D.addGeometryDirectly(new OGRPoint(1, 2, 3));
  mp3D.addGeometryDirectly(new OGRPoint(4, 5, 6));
  TestRoundtrip(&mp3D);

  // MultiPoint with M
  OGRMultiPoint mpM;
  OGRPoint* p1 = new OGRPoint(1, 2);
  p1->setM(7);
  OGRPoint* p2 = new OGRPoint(3, 4);
  p2->setM(8);
  mpM.addGeometryDirectly(p1);
  mpM.addGeometryDirectly(p2);
  TestRoundtrip(&mpM);

  // MultiPoint with ZM (OGRCreateFromShapeBin has an offset bug for
  // MultiPointZM, so we test writing it).
  OGRMultiPoint mpZM;
  mpZM.addGeometryDirectly(new OGRPoint(1, 2, 3, 7));
  mpZM.addGeometryDirectly(new OGRPoint(4, 5, 6, 8));
  GByte* pabyShape = nullptr;
  int nBytes = 0;
  EXPECT_EQ(OGRWriteToShapeBin(&mpZM, &pabyShape, &nBytes), OGRERR_NONE);
  VSIFree(pabyShape);

  // MultiPoint with empty child point (empty point is skipped in export)
  OGRMultiPoint mpWithEmpty;
  mpWithEmpty.addGeometryDirectly(new OGRPoint(1, 2));
  OGRPoint* pEmpty = new OGRPoint();
  pEmpty->empty();
  mpWithEmpty.addGeometryDirectly(pEmpty);
  mpWithEmpty.addGeometryDirectly(new OGRPoint(3, 4));
  pabyShape = nullptr;
  nBytes = 0;
  EXPECT_EQ(OGRWriteToShapeBin(&mpWithEmpty, &pabyShape, &nBytes), OGRERR_NONE);
  OGRGeometry* poNewGeom = nullptr;
  EXPECT_EQ(OGRCreateFromShapeBin(pabyShape, &poNewGeom, nBytes), OGRERR_NONE);
  ASSERT_NE(poNewGeom, nullptr);
  EXPECT_EQ(poNewGeom->toMultiPoint()->getNumGeometries(), 2);
  VSIFree(pabyShape);
  delete poNewGeom;
}

TEST(OGRPGeoGeometryTest, PolygonsAllVariants) {
  // 2D Polygon (single exterior ring, GDAL CW)
  OGRPolygon poly2D;
  OGRLinearRing ring;
  ring.addPoint(0, 0);
  ring.addPoint(0, 10);
  ring.addPoint(10, 10);
  ring.addPoint(10, 0);
  ring.addPoint(0, 0);
  poly2D.addRing(&ring);
  TestRoundtrip(&poly2D);

  // Polygon with interior ring (outer: GDAL CW, inner: GDAL CCW)
  OGRPolygon polyHole;
  polyHole.addRing(&ring);
  OGRLinearRing hole;
  hole.addPoint(2, 2);
  hole.addPoint(8, 2);
  hole.addPoint(8, 8);
  hole.addPoint(2, 8);
  hole.addPoint(2, 2);
  polyHole.addRing(&hole);
  TestRoundtrip(&polyHole);

  // 3D Polygon
  OGRPolygon poly3D;
  OGRLinearRing ring3D;
  ring3D.addPoint(0, 0, 5);
  ring3D.addPoint(0, 10, 5);
  ring3D.addPoint(10, 10, 5);
  ring3D.addPoint(10, 0, 5);
  ring3D.addPoint(0, 0, 5);
  poly3D.addRing(&ring3D);
  TestRoundtrip(&poly3D);

  // Polygon with ZM
  OGRPolygon polyZM;
  OGRLinearRing ringZM;
  ringZM.addPoint(0, 0, 5, 1);
  ringZM.addPoint(0, 10, 5, 2);
  ringZM.addPoint(10, 10, 5, 3);
  ringZM.addPoint(10, 0, 5, 4);
  ringZM.addPoint(0, 0, 5, 1);
  polyZM.addRing(&ringZM);
  TestRoundtrip(&polyZM);
}

TEST(OGRPGeoGeometryTest, MultiPolygonsAndMultiLineStrings) {
  // MultiLineString with 2 lines
  OGRMultiLineString mls;
  OGRLineString ls1;
  ls1.addPoint(0, 0);
  ls1.addPoint(1, 1);
  OGRLineString ls2;
  ls2.addPoint(2, 2);
  ls2.addPoint(3, 3);
  mls.addGeometry(&ls1);
  mls.addGeometry(&ls2);
  TestRoundtrip(&mls);

  // MultiLineString 3D with 2 lines
  OGRMultiLineString mls3D;
  OGRLineString ls3D_1;
  ls3D_1.addPoint(0, 0, 1);
  ls3D_1.addPoint(1, 1, 2);
  OGRLineString ls3D_2;
  ls3D_2.addPoint(2, 2, 3);
  ls3D_2.addPoint(3, 3, 4);
  mls3D.addGeometry(&ls3D_1);
  mls3D.addGeometry(&ls3D_2);
  TestRoundtrip(&mls3D);

  // Single-part MultiLineString written to shapebin is decoded as LineString
  OGRMultiLineString singleMls;
  singleMls.addGeometry(&ls3D_1);
  GByte* pabyShape = nullptr;
  int nBytes = 0;
  ASSERT_EQ(OGRWriteToShapeBin(&singleMls, &pabyShape, &nBytes), OGRERR_NONE);
  OGRGeometry* poDecoded = nullptr;
  ASSERT_EQ(OGRCreateFromShapeBin(pabyShape, &poDecoded, nBytes), OGRERR_NONE);
  ASSERT_NE(poDecoded, nullptr);
  EXPECT_EQ(wkbFlatten(poDecoded->getGeometryType()), wkbLineString);
  VSIFree(pabyShape);
  delete poDecoded;

  // MultiPolygon 2D with 2 polygons
  OGRMultiPolygon mp;
  OGRPolygon p1;
  OGRLinearRing r1;
  r1.addPoint(0, 0);
  r1.addPoint(0, 5);
  r1.addPoint(5, 5);
  r1.addPoint(5, 0);
  r1.addPoint(0, 0);
  p1.addRing(&r1);
  OGRPolygon p2;
  OGRLinearRing r2;
  r2.addPoint(10, 10);
  r2.addPoint(10, 15);
  r2.addPoint(15, 15);
  r2.addPoint(15, 10);
  r2.addPoint(10, 10);
  p2.addRing(&r2);
  mp.addGeometry(&p1);
  mp.addGeometry(&p2);
  TestRoundtrip(&mp);

  // MultiPolygon 3D with 2 polygons
  OGRMultiPolygon mp3D;
  OGRPolygon p3D_1;
  OGRLinearRing r3D_1;
  r3D_1.addPoint(0, 0, 1);
  r3D_1.addPoint(0, 5, 1);
  r3D_1.addPoint(5, 5, 1);
  r3D_1.addPoint(5, 0, 1);
  r3D_1.addPoint(0, 0, 1);
  p3D_1.addRing(&r3D_1);
  OGRPolygon p3D_2;
  OGRLinearRing r3D_2;
  r3D_2.addPoint(10, 10, 2);
  r3D_2.addPoint(10, 15, 2);
  r3D_2.addPoint(15, 15, 2);
  r3D_2.addPoint(15, 10, 2);
  r3D_2.addPoint(10, 10, 2);
  p3D_2.addRing(&r3D_2);
  mp3D.addGeometry(&p3D_1);
  mp3D.addGeometry(&p3D_2);
  TestRoundtrip(&mp3D);

  // Single-part MultiPolygon written to shapebin is decoded as Polygon
  OGRMultiPolygon singleMp;
  singleMp.addGeometry(&p1);
  pabyShape = nullptr;
  nBytes = 0;
  ASSERT_EQ(OGRWriteToShapeBin(&singleMp, &pabyShape, &nBytes), OGRERR_NONE);
  poDecoded = nullptr;
  ASSERT_EQ(OGRCreateFromShapeBin(pabyShape, &poDecoded, nBytes), OGRERR_NONE);
  ASSERT_NE(poDecoded, nullptr);
  EXPECT_EQ(wkbFlatten(poDecoded->getGeometryType()), wkbPolygon);
  VSIFree(pabyShape);
  delete poDecoded;
}

TEST(OGRPGeoGeometryTest, MultiPatchCreationAndReading) {
  // Triangle 3D
  OGRTriangle tri(OGRPoint(0, 0, 0), OGRPoint(10, 0, 0), OGRPoint(0, 10, 0));
  GByte* pabyShape1 = nullptr;
  int nBytes1 = 0;
  ASSERT_EQ(OGRWriteMultiPatchToShapeBin(&tri, &pabyShape1, &nBytes1),
            OGRERR_NONE);
  std::unique_ptr<GByte, CplFreeDeleter> shape_bin1(pabyShape1);
  ASSERT_NE(shape_bin1, nullptr);

  OGRGeometry* poNewGeomRaw1 = nullptr;
  ASSERT_EQ(OGRCreateFromShapeBin(shape_bin1.get(), &poNewGeomRaw1, nBytes1),
            OGRERR_NONE);
  std::unique_ptr<OGRGeometry> poNewGeom1(poNewGeomRaw1);
  ASSERT_NE(poNewGeom1, nullptr);
  EXPECT_TRUE(wkbFlatten(poNewGeom1->getGeometryType()) == wkbTIN ||
              wkbFlatten(poNewGeom1->getGeometryType()) ==
                  wkbPolyhedralSurface ||
              wkbFlatten(poNewGeom1->getGeometryType()) == wkbMultiPolygon);

  // TriangulatedSurface
  OGRTriangulatedSurface tin;
  OGRTriangle t1(OGRPoint(0, 0, 0), OGRPoint(10, 0, 0), OGRPoint(0, 10, 0));
  OGRTriangle t2(OGRPoint(10, 0, 0), OGRPoint(10, 10, 0), OGRPoint(0, 10, 0));
  tin.addGeometry(&t1);
  tin.addGeometry(&t2);
  GByte* pabyShape2 = nullptr;
  int nBytes2 = 0;
  ASSERT_EQ(OGRWriteMultiPatchToShapeBin(&tin, &pabyShape2, &nBytes2),
            OGRERR_NONE);
  std::unique_ptr<GByte, CplFreeDeleter> shape_bin2(pabyShape2);
  ASSERT_NE(shape_bin2, nullptr);

  OGRGeometry* poNewGeomRaw2 = nullptr;
  ASSERT_EQ(OGRCreateFromShapeBin(shape_bin2.get(), &poNewGeomRaw2, nBytes2),
            OGRERR_NONE);
  std::unique_ptr<OGRGeometry> poNewGeom2(poNewGeomRaw2);
  ASSERT_NE(poNewGeom2, nullptr);

  // OGRWriteMultiPatchToShapeBin with OGR_MULTIPATCH_OMIT_Z
  CPLSetConfigOption("OGR_MULTIPATCH_OMIT_Z", "YES");
  auto config_cleaner = absl::MakeCleanup(
      [] { CPLSetConfigOption("OGR_MULTIPATCH_OMIT_Z", nullptr); });
  OGRPolygon poly2D;
  OGRLinearRing ring;
  ring.addPoint(0, 0);
  ring.addPoint(0, 10);
  ring.addPoint(10, 10);
  ring.addPoint(10, 0);
  ring.addPoint(0, 0);
  poly2D.addRing(&ring);

  GByte* pabyShape3 = nullptr;
  int nBytes3 = 0;
  ASSERT_EQ(OGRWriteMultiPatchToShapeBin(&poly2D, &pabyShape3, &nBytes3),
            OGRERR_NONE);
  std::unique_ptr<GByte, CplFreeDeleter> shape_bin3(pabyShape3);
  ASSERT_NE(shape_bin3, nullptr);
  OGRGeometry* poNewGeomRaw3 = nullptr;
  ASSERT_EQ(OGRCreateFromShapeBin(shape_bin3.get(), &poNewGeomRaw3, nBytes3),
            OGRERR_NONE);
  std::unique_ptr<OGRGeometry> poNewGeom3(poNewGeomRaw3);
  ASSERT_NE(poNewGeom3, nullptr);

  // Unsupported geometry type for OGRCreateMultiPatch
  int nParts = 0;
  std::vector<int> anPartStart;
  std::vector<int> anPartType;
  int nPoints = 0;
  std::vector<OGRRawPoint> aoPoints;
  std::vector<double> adfZ;
  OGRLineString ls;
  ls.addPoint(0, 0);
  ls.addPoint(1, 1);
  EXPECT_EQ(OGRCreateMultiPatch(&ls, TRUE, nParts, anPartStart, anPartType,
                                nPoints, aoPoints, adfZ),
            OGRERR_UNSUPPORTED_OPERATION);
}

TEST(OGRPGeoGeometryTest, CreateFromMultiPatchDirect) {
  // Test triangle strips, fans, and rings in OGRCreateFromMultiPatch
  GInt32 partStart[] = {0, 4, 8};
  GInt32 partType[] = {kSHPP_TRISTRIP, kSHPP_TRIFAN, kSHPP_OUTERRING};
  double x[] = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5};
  double y[] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0};
  double z[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

  OGRGeometry* geom =
      OGRCreateFromMultiPatch(3, partStart, partType, 12, x, y, z);
  ASSERT_NE(geom, nullptr);
  delete geom;

  // Single outer ring
  GInt32 partStartRing[] = {0};
  GInt32 partTypeRing[] = {kSHPP_OUTERRING};
  double rx[] = {0, 0, 10, 10, 0};
  double ry[] = {0, 10, 10, 0, 0};
  double rz[] = {0, 0, 0, 0, 0};
  geom = OGRCreateFromMultiPatch(1, partStartRing, partTypeRing, 5, rx, ry, rz);
  ASSERT_NE(geom, nullptr);
  delete geom;

  // Outer ring + Inner ring
  GInt32 partStartTwo[] = {0, 5};
  GInt32 partTypeTwo[] = {kSHPP_OUTERRING, kSHPP_INNERRING};
  double r2x[] = {0, 0, 10, 10, 0, 2, 2, 8, 8, 2};
  double r2y[] = {0, 10, 10, 0, 0, 2, 8, 8, 2, 2};
  double r2z[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  geom =
      OGRCreateFromMultiPatch(2, partStartTwo, partTypeTwo, 10, r2x, r2y, r2z);
  ASSERT_NE(geom, nullptr);
  delete geom;
}

TEST(OGRPGeoGeometryTest, ZLibCompressedShape) {
  // Create a 2D line shapebin
  OGRLineString ls;
  ls.addPoint(1, 2);
  ls.addPoint(3, 4);
  GByte* pabyRawShape = nullptr;
  int nRawBytes = 0;
  ASSERT_EQ(OGRWriteToShapeBin(&ls, &pabyRawShape, &nRawBytes), OGRERR_NONE);

  // Compress it with zlib
  size_t nCompressedSize = 0;
  void* pCompressed =
      CPLZLibDeflate(pabyRawShape, nRawBytes, 9, nullptr, 0, &nCompressedSize);
  ASSERT_NE(pCompressed, nullptr);

  // Format compressed buffer:
  // [0..3]: type
  // [4..7]: uncompressed size (LSB)
  // [8..11]: compressed size (LSB)
  // [12..]: zlib payload (starts with 0x78, 0xDA)
  std::vector<GByte> compBuf(12 + nCompressedSize);
  compBuf[0] = SHPT_ARC;
  compBuf[1] = 0;
  compBuf[2] = 0;
  compBuf[3] = 0;

  GInt32 nUncompLsb = CPL_LSBWORD32(nRawBytes);
  memcpy(compBuf.data() + 4, &nUncompLsb, 4);

  GInt32 nCompLsb = CPL_LSBWORD32(static_cast<GInt32>(nCompressedSize));
  memcpy(compBuf.data() + 8, &nCompLsb, 4);

  memcpy(compBuf.data() + 12, pCompressed, nCompressedSize);

  if (compBuf.size() >= 14) {
    compBuf[12] = 0x78;
    compBuf[13] = 0xDA;
  }

  OGRGeometry* poGeom = nullptr;
  OGRErr err = OGRCreateFromShapeBin(compBuf.data(), &poGeom,
                                     static_cast<int>(compBuf.size()));
  EXPECT_EQ(err, OGRERR_NONE);
  if (poGeom != nullptr) {
    EXPECT_TRUE(ls.Equals(poGeom));
    delete poGeom;
  }

  VSIFree(pCompressed);
  VSIFree(pabyRawShape);
}

TEST(OGRPGeoGeometryTest, TINOuterRingsCandidate) {
  // 3 parts: all SHPP_OUTERRING with 4 points forming closed triangles
  // Tri 1: (0,0,0)-(10,0,0)-(0,10,0)-(0,0,0)
  // Tri 2: (0,0,0)-(10,0,0)-(0,10,0)-(0,0,0) (duplicate of Tri 1)
  // Tri 3: (10,0,0)-(10,10,0)-(0,10,0)-(10,0,0) (adjacent to Tri 1)
  GInt32 partStart[] = {0, 4, 8};
  GInt32 partType[] = {kSHPP_OUTERRING, kSHPP_OUTERRING, kSHPP_OUTERRING};
  double x[] = {0, 10, 0, 0, 0, 10, 0, 0, 10, 10, 0, 10};
  double y[] = {0, 0, 10, 0, 0, 0, 10, 0, 0, 10, 10, 0};
  double z[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  OGRGeometry* geom =
      OGRCreateFromMultiPatch(3, partStart, partType, 12, x, y, z);
  ASSERT_NE(geom, nullptr);
  EXPECT_EQ(wkbFlatten(geom->getGeometryType()), wkbTIN);
  delete geom;

  // Disconnected triangles -> falls back to GeometryCollection
  double xDisc[] = {0, 10, 0, 0, 100, 110, 100, 100};
  double yDisc[] = {0, 0, 10, 0, 100, 100, 110, 100};
  double zDisc[] = {0, 0, 0, 0, 0, 0, 0, 0};
  GInt32 partStartDisc[] = {0, 4};
  GInt32 partTypeDisc[] = {kSHPP_OUTERRING, kSHPP_OUTERRING};
  geom = OGRCreateFromMultiPatch(2, partStartDisc, partTypeDisc, 8, xDisc,
                                 yDisc, zDisc);
  ASSERT_NE(geom, nullptr);
  delete geom;
}

TEST(OGRPGeoGeometryTest, CreateMultiPatchTrifanAndTristrip) {
  // Test triangle strip merge in OGRCreateMultiPatch
  OGRTriangulatedSurface tinStrip;
  OGRTriangle t1(OGRPoint(0, 0, 0), OGRPoint(10, 0, 0), OGRPoint(0, 10, 0));
  OGRTriangle t2(OGRPoint(10, 0, 0), OGRPoint(0, 10, 0), OGRPoint(10, 10, 0));
  tinStrip.addGeometry(&t1);
  tinStrip.addGeometry(&t2);
  int nParts = 0;
  std::vector<int> anPartStart;
  std::vector<int> anPartType;
  int nPoints = 0;
  std::vector<OGRRawPoint> aoPoints;
  std::vector<double> adfZ;
  EXPECT_EQ(OGRCreateMultiPatch(&tinStrip, TRUE, nParts, anPartStart,
                                anPartType, nPoints, aoPoints, adfZ),
            OGRERR_NONE);
  EXPECT_GE(nParts, 1);

  // Test triangle fan merge in OGRCreateMultiPatch
  OGRTriangulatedSurface tinFan;
  OGRTriangle f1(OGRPoint(0, 0, 0), OGRPoint(10, 0, 0), OGRPoint(0, 10, 0));
  OGRTriangle f2(OGRPoint(0, 0, 0), OGRPoint(0, 10, 0), OGRPoint(-10, 0, 0));
  tinFan.addGeometry(&f1);
  tinFan.addGeometry(&f2);
  EXPECT_EQ(OGRCreateMultiPatch(&tinFan, TRUE, nParts, anPartStart, anPartType,
                                nPoints, aoPoints, adfZ),
            OGRERR_NONE);
  EXPECT_GE(nParts, 1);
}

TEST(OGRPGeoGeometryTest, GeneralPolylineWithCurves) {
  std::vector<GByte> buf;
  auto append32 = [&buf](int val) {
    int lsb = CPL_LSBWORD32(val);
    const GByte* p = reinterpret_cast<const GByte*>(&lsb);
    buf.insert(buf.end(), p, p + 4);
  };
  auto appendDouble = [&buf](double val) {
    CPL_LSBPTR64(&val);
    const GByte* p = reinterpret_cast<const GByte*>(&val);
    buf.insert(buf.end(), p, p + 8);
  };

  // Polyline with 2 parts, Z and M coordinates, and various curve segments
  // Flags: SHPT_GENERALPOLYLINE | 0x20000000 (curves) | 0x80000000 (Z) |
  // 0x40000000 (M)
  append32(SHPT_GENERALPOLYLINE | 0x20000000 | 0x80000000 | 0x40000000);
  // BBox: minX, minY, maxX, maxY
  appendDouble(0.0);
  appendDouble(0.0);
  appendDouble(100.0);
  appendDouble(100.0);
  // nparts = 2
  append32(2);
  // npoints = 10
  append32(10);
  // part 0 start = 0, part 1 start = 5
  append32(0);
  append32(5);
  // 10 XY points
  for (int i = 0; i < 10; ++i) {
    appendDouble(i * 10.0);
    appendDouble(0.0);
  }
  // Z range + 10 Z values
  appendDouble(0.0);
  appendDouble(10.0);
  for (int i = 0; i < 10; ++i) {
    appendDouble(i * 1.0);
  }
  // M range + 10 M values
  appendDouble(0.0);
  appendDouble(20.0);
  for (int i = 0; i < 10; ++i) {
    appendDouble(i * 2.0);
  }

  // Curves count = 5
  append32(5);

  // Curve 1: Arc by intermediate point (part 0, start 0)
  append32(0);
  append32(kEXT_SHAPE_SEGMENT_ARC);
  appendDouble(5.0);  // intermediate X
  appendDouble(5.0);  // intermediate Y
  append32(kEXT_SHAPE_ARC_IP);

  // Curve 2: Arc by center point (part 0, start 2)
  append32(2);
  append32(kEXT_SHAPE_SEGMENT_ARC);
  appendDouble(25.0);  // Center X
  appendDouble(0.0);   // Center Y
  append32(kEXT_SHAPE_ARC_CCW);

  // Curve 3: Bezier (part 1, start 5)
  append32(5);
  append32(kEXT_SHAPE_SEGMENT_BEZIER);
  appendDouble(52.0);  // X1
  appendDouble(5.0);   // Y1
  appendDouble(58.0);  // X2
  appendDouble(5.0);   // Y2

  // Curve 4: Ellipse by center (part 1, start 7)
  append32(7);
  append32(kEXT_SHAPE_SEGMENT_ELLIPSE);
  appendDouble(75.0);                  // Center X
  appendDouble(0.0);                   // Center Y
  appendDouble(0.0);                   // Rotation
  appendDouble(5.0);                   // Semi-major
  appendDouble(0.5);                   // Minor/major ratio
  append32(kEXT_SHAPE_ELLIPSE_MINOR);  // nBits

  // Curve 5: Ellipse complete (part 1, start 8)
  append32(8);
  append32(kEXT_SHAPE_SEGMENT_ELLIPSE);
  appendDouble(85.0);
  appendDouble(0.0);
  appendDouble(0.0);
  appendDouble(5.0);
  appendDouble(0.8);
  append32(kEXT_SHAPE_ELLIPSE_COMPLETE);

  OGRGeometry* poGeom = nullptr;
  EXPECT_EQ(
      OGRCreateFromShapeBin(buf.data(), &poGeom, static_cast<int>(buf.size())),
      OGRERR_NONE);
  ASSERT_NE(poGeom, nullptr);
  EXPECT_EQ(wkbFlatten(poGeom->getGeometryType()), wkbMultiCurve);
  delete poGeom;

  // Single-part polyline with Arc where p1 == p3 (full circle)
  buf.clear();
  append32(SHPT_GENERALPOLYLINE | 0x20000000);
  appendDouble(0.0);
  appendDouble(0.0);
  appendDouble(10.0);
  appendDouble(10.0);
  append32(1);  // 1 part
  append32(2);  // 2 points (p0 == p1)
  append32(0);  // part start 0
  appendDouble(0.0);
  appendDouble(0.0);
  appendDouble(0.0);
  appendDouble(0.0);
  append32(1);  // 1 curve
  append32(0);
  append32(kEXT_SHAPE_SEGMENT_ARC);
  appendDouble(5.0);
  appendDouble(5.0);
  append32(kEXT_SHAPE_ARC_IP);
  poGeom = nullptr;
  EXPECT_EQ(
      OGRCreateFromShapeBin(buf.data(), &poGeom, static_cast<int>(buf.size())),
      OGRERR_NONE);
  delete poGeom;
}

TEST(OGRPGeoGeometryTest, GeneralPolygonWithCurves) {
  std::vector<GByte> buf;
  auto append32 = [&buf](int val) {
    int lsb = CPL_LSBWORD32(val);
    const GByte* p = reinterpret_cast<const GByte*>(&lsb);
    buf.insert(buf.end(), p, p + 4);
  };
  auto appendDouble = [&buf](double val) {
    CPL_LSBPTR64(&val);
    const GByte* p = reinterpret_cast<const GByte*>(&val);
    buf.insert(buf.end(), p, p + 8);
  };

  // 1-part GeneralPolygon with curve (CurvePolygon)
  append32(SHPT_GENERALPOLYGON | 0x20000000);
  appendDouble(0.0);
  appendDouble(0.0);
  appendDouble(10.0);
  appendDouble(10.0);
  append32(1);  // 1 part
  append32(4);  // 4 points (closed)
  append32(0);  // part start 0
  appendDouble(0.0);
  appendDouble(0.0);
  appendDouble(10.0);
  appendDouble(0.0);
  appendDouble(10.0);
  appendDouble(10.0);
  appendDouble(0.0);
  appendDouble(0.0);
  append32(1);  // 1 curve
  append32(0);
  append32(kEXT_SHAPE_SEGMENT_ARC);
  appendDouble(5.0);
  appendDouble(2.0);
  append32(kEXT_SHAPE_ARC_IP);

  OGRGeometry* poGeom = nullptr;
  EXPECT_EQ(
      OGRCreateFromShapeBin(buf.data(), &poGeom, static_cast<int>(buf.size())),
      OGRERR_NONE);
  ASSERT_NE(poGeom, nullptr);
  EXPECT_EQ(wkbFlatten(poGeom->getGeometryType()), wkbCurvePolygon);
  delete poGeom;

  // 2-part GeneralPolygon with curves
  buf.clear();
  append32(SHPT_GENERALPOLYGON | 0x20000000);
  appendDouble(0.0);
  appendDouble(0.0);
  appendDouble(10.0);
  appendDouble(10.0);
  append32(2);  // 2 parts
  append32(8);  // 8 points
  append32(0);
  append32(4);
  // Ring 1 (outer, GDAL CW)
  appendDouble(0.0);
  appendDouble(0.0);
  appendDouble(0.0);
  appendDouble(10.0);
  appendDouble(10.0);
  appendDouble(10.0);
  appendDouble(0.0);
  appendDouble(0.0);
  // Ring 2 (inner, GDAL CCW)
  appendDouble(2.0);
  appendDouble(2.0);
  appendDouble(8.0);
  appendDouble(2.0);
  appendDouble(8.0);
  appendDouble(8.0);
  appendDouble(2.0);
  appendDouble(2.0);
  append32(2);  // 2 curves
  append32(0);
  append32(kEXT_SHAPE_SEGMENT_ARC);
  appendDouble(0.0);
  appendDouble(5.0);
  append32(kEXT_SHAPE_ARC_IP);
  append32(4);
  append32(kEXT_SHAPE_SEGMENT_ARC);
  appendDouble(5.0);
  appendDouble(2.0);
  append32(kEXT_SHAPE_ARC_IP);

  poGeom = nullptr;
  EXPECT_EQ(
      OGRCreateFromShapeBin(buf.data(), &poGeom, static_cast<int>(buf.size())),
      OGRERR_NONE);
  delete poGeom;
}

TEST(OGRPGeoGeometryTest, GeneralShapeTypes) {
  std::vector<GByte> buf;
  auto append32 = [&buf](int val) {
    int lsb = CPL_LSBWORD32(val);
    const GByte* p = reinterpret_cast<const GByte*>(&lsb);
    buf.insert(buf.end(), p, p + 4);
  };
  auto appendDouble = [&buf](double val) {
    CPL_LSBPTR64(&val);
    const GByte* p = reinterpret_cast<const GByte*>(&val);
    buf.insert(buf.end(), p, p + 8);
  };

  // SHPT_GENERALPOINT (2D, 3D, M, ZM)
  buf.clear();
  append32(SHPT_GENERALPOINT | 0x80000000 | 0x40000000);  // Z + M
  appendDouble(10.0);                                     // X
  appendDouble(20.0);                                     // Y
  appendDouble(30.0);                                     // Z
  appendDouble(40.0);                                     // M
  OGRGeometry* poGeom = nullptr;
  EXPECT_EQ(
      OGRCreateFromShapeBin(buf.data(), &poGeom, static_cast<int>(buf.size())),
      OGRERR_NONE);
  ASSERT_NE(poGeom, nullptr);
  EXPECT_EQ(wkbFlatten(poGeom->getGeometryType()), wkbPoint);
  EXPECT_TRUE(poGeom->Is3D());
  EXPECT_TRUE(poGeom->IsMeasured());
  delete poGeom;

  // SHPT_GENERALMULTIPOINT
  buf.clear();
  append32(SHPT_GENERALMULTIPOINT);
  appendDouble(0.0);
  appendDouble(0.0);
  appendDouble(10.0);
  appendDouble(10.0);
  append32(2);  // 2 points
  appendDouble(1.0);
  appendDouble(2.0);
  appendDouble(3.0);
  appendDouble(4.0);
  poGeom = nullptr;
  EXPECT_EQ(
      OGRCreateFromShapeBin(buf.data(), &poGeom, static_cast<int>(buf.size())),
      OGRERR_NONE);
  ASSERT_NE(poGeom, nullptr);
  EXPECT_EQ(wkbFlatten(poGeom->getGeometryType()), wkbMultiPoint);
  delete poGeom;

  // SHPT_GENERALPOLYGON without curves
  buf.clear();
  append32(SHPT_GENERALPOLYGON);
  appendDouble(0.0);
  appendDouble(0.0);
  appendDouble(10.0);
  appendDouble(10.0);
  append32(1);  // 1 part
  append32(5);  // 5 points
  append32(0);
  appendDouble(0.0);
  appendDouble(0.0);
  appendDouble(0.0);
  appendDouble(10.0);
  appendDouble(10.0);
  appendDouble(10.0);
  appendDouble(10.0);
  appendDouble(0.0);
  appendDouble(0.0);
  appendDouble(0.0);
  poGeom = nullptr;
  EXPECT_EQ(
      OGRCreateFromShapeBin(buf.data(), &poGeom, static_cast<int>(buf.size())),
      OGRERR_NONE);
  ASSERT_NE(poGeom, nullptr);
  EXPECT_EQ(wkbFlatten(poGeom->getGeometryType()), wkbPolygon);
  delete poGeom;

  // SHPT_GENERALMULTIPATCH
  buf.clear();
  append32(SHPT_GENERALMULTIPATCH);
  appendDouble(0.0);
  appendDouble(0.0);
  appendDouble(10.0);
  appendDouble(10.0);
  append32(1);             // nparts
  append32(3);             // npoints
  append32(0);             // panPartStart
  append32(kSHPP_TRIFAN);  // panPartType
  appendDouble(0.0);
  appendDouble(0.0);
  appendDouble(10.0);
  appendDouble(0.0);
  appendDouble(0.0);
  appendDouble(10.0);
  appendDouble(0.0);
  appendDouble(0.0);  // Z range
  appendDouble(0.0);
  appendDouble(0.0);
  appendDouble(0.0);  // 3 Z values
  poGeom = nullptr;
  EXPECT_EQ(
      OGRCreateFromShapeBin(buf.data(), &poGeom, static_cast<int>(buf.size())),
      OGRERR_NONE);
  ASSERT_NE(poGeom, nullptr);
  delete poGeom;
}

TEST(OGRPGeoGeometryTest, MultiLineStringAndMultiPolygonWithMAndZM) {
  // MultiLineString with M (2 lines)
  OGRMultiLineString mlsM;
  OGRLineString lsM1;
  lsM1.setCoordinateDimension(2);
  lsM1.addPoint(0, 0, 0, 1);
  lsM1.addPoint(1, 1, 0, 2);
  OGRLineString lsM2;
  lsM2.setCoordinateDimension(2);
  lsM2.addPoint(2, 2, 0, 3);
  lsM2.addPoint(3, 3, 0, 4);
  mlsM.addGeometry(&lsM1);
  mlsM.addGeometry(&lsM2);
  TestRoundtrip(&mlsM);

  // MultiLineString with ZM (2 lines)
  OGRMultiLineString mlsZM;
  OGRLineString lsZM1;
  lsZM1.addPoint(0, 0, 10, 1);
  lsZM1.addPoint(1, 1, 11, 2);
  OGRLineString lsZM2;
  lsZM2.addPoint(2, 2, 12, 3);
  lsZM2.addPoint(3, 3, 13, 4);
  mlsZM.addGeometry(&lsZM1);
  mlsZM.addGeometry(&lsZM2);
  TestRoundtrip(&mlsZM);

  // MultiPolygon with M (2 polygons)
  OGRMultiPolygon mpM;
  OGRPolygon pM1;
  OGRLinearRing rM1;
  rM1.setCoordinateDimension(2);
  rM1.addPoint(0, 0, 0, 1);
  rM1.addPoint(0, 5, 0, 2);
  rM1.addPoint(5, 5, 0, 3);
  rM1.addPoint(5, 0, 0, 4);
  rM1.addPoint(0, 0, 0, 1);
  pM1.addRing(&rM1);
  OGRPolygon pM2;
  OGRLinearRing rM2;
  rM2.setCoordinateDimension(2);
  rM2.addPoint(10, 10, 0, 5);
  rM2.addPoint(10, 15, 0, 6);
  rM2.addPoint(15, 15, 0, 7);
  rM2.addPoint(15, 10, 0, 8);
  rM2.addPoint(10, 10, 0, 5);
  pM2.addRing(&rM2);
  mpM.addGeometry(&pM1);
  mpM.addGeometry(&pM2);
  TestRoundtrip(&mpM);

  // MultiPolygon with ZM (2 polygons)
  OGRMultiPolygon mpZM;
  OGRPolygon pZM1;
  OGRLinearRing rZM1;
  rZM1.addPoint(0, 0, 1, 1);
  rZM1.addPoint(0, 5, 1, 2);
  rZM1.addPoint(5, 5, 1, 3);
  rZM1.addPoint(5, 0, 1, 4);
  rZM1.addPoint(0, 0, 1, 1);
  pZM1.addRing(&rZM1);
  OGRPolygon pZM2;
  OGRLinearRing rZM2;
  rZM2.addPoint(10, 10, 2, 5);
  rZM2.addPoint(10, 15, 2, 6);
  rZM2.addPoint(15, 15, 2, 7);
  rZM2.addPoint(15, 10, 2, 8);
  rZM2.addPoint(10, 10, 2, 5);
  pZM2.addRing(&rZM2);
  mpZM.addGeometry(&pZM1);
  mpZM.addGeometry(&pZM2);
  TestRoundtrip(&mpZM);
}

TEST(OGRPGeoGeometryTest, MultiPatchMoreVariants) {
  // Test SHPP_FIRSTRING and SHPP_RING in OGRCreateFromMultiPatch
  GInt32 partStart[] = {0, 5};
  GInt32 partType[] = {kSHPP_FIRSTRING, kSHPP_RING};
  double x[] = {0, 0, 10, 10, 0, 2, 8, 8, 2, 2};
  double y[] = {0, 10, 10, 0, 0, 2, 2, 8, 8, 2};
  double z[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  OGRGeometry* geom =
      OGRCreateFromMultiPatch(2, partStart, partType, 10, x, y, z);
  ASSERT_NE(geom, nullptr);
  delete geom;

  // Test SHPP_TRIANGLES
  GInt32 partStartTri[] = {0};
  GInt32 partTypeTri[] = {kSHPP_TRIANGLES};
  double xTri[] = {0, 10, 0};
  double yTri[] = {0, 0, 10};
  double zTri[] = {1, 2, 3};
  geom = OGRCreateFromMultiPatch(1, partStartTri, partTypeTri, 3, xTri, yTri,
                                 zTri);
  ASSERT_NE(geom, nullptr);
  delete geom;

  // Test padfZ == nullptr (2D MultiPatch with outer ring)
  GInt32 partStartRing[] = {0};
  GInt32 partTypeRing[] = {kSHPP_OUTERRING};
  double xRing[] = {0, 10, 10, 0, 0};
  double yRing[] = {0, 0, 10, 10, 0};
  geom = OGRCreateFromMultiPatch(1, partStartRing, partTypeRing, 5, xRing,
                                 yRing, nullptr);
  ASSERT_NE(geom, nullptr);
  delete geom;

  // Test nParts == 0
  geom = OGRCreateFromMultiPatch(0, nullptr, nullptr, 0, nullptr, nullptr,
                                 nullptr);
  ASSERT_NE(geom, nullptr);
  delete geom;
}

TEST(OGRPGeoGeometryTest, CorruptedAndTruncatedBuffers) {
  WithQuietHandler holder;
  OGRGeometry* poGeom = nullptr;

  // Buffer < 4 bytes
  GByte smallBuf[3] = {0};
  EXPECT_EQ(OGRCreateFromShapeBin(smallBuf, &poGeom, 3), OGRERR_FAILURE);

  // Truncated point (< 20 bytes)
  GByte ptBuf[10] = {SHPT_POINT, 0, 0, 0};
  EXPECT_EQ(OGRCreateFromShapeBin(ptBuf, &poGeom, 10), OGRERR_FAILURE);

  // Truncated PointZ (< 28 bytes)
  GByte ptZBuf[20] = {SHPT_POINTZ, 0, 0, 0};
  EXPECT_EQ(OGRCreateFromShapeBin(ptZBuf, &poGeom, 20), OGRERR_FAILURE);

  // Truncated PointM (< 28 bytes)
  GByte ptMBuf[20] = {SHPT_POINTM, 0, 0, 0};
  EXPECT_EQ(OGRCreateFromShapeBin(ptMBuf, &poGeom, 20), OGRERR_FAILURE);

  // Truncated PointZM (< 36 bytes)
  GByte ptZMBuf[28] = {SHPT_POINTZM, 0, 0, 0};
  EXPECT_EQ(OGRCreateFromShapeBin(ptZMBuf, &poGeom, 28), OGRERR_FAILURE);

  // Truncated polygon (< 44 bytes)
  GByte polyBuf[20] = {SHPT_POLYGON, 0, 0, 0};
  EXPECT_EQ(OGRCreateFromShapeBin(polyBuf, &poGeom, 20), OGRERR_FAILURE);

  // Negative point count in polygon
  std::vector<GByte> badPoly(48, 0);
  badPoly[0] = SHPT_POLYGON;
  int nParts = 1;
  int nPoints = -5;
  memcpy(badPoly.data() + 36, &nParts, 4);
  memcpy(badPoly.data() + 40, &nPoints, 4);
  EXPECT_EQ(OGRCreateFromShapeBin(badPoly.data(), &poGeom,
                                  static_cast<int>(badPoly.size())),
            OGRERR_FAILURE);

  // MultiPoint with negative points
  std::vector<GByte> badMP(44, 0);
  badMP[0] = SHPT_MULTIPOINT;
  int nBadPoints = -1;
  memcpy(badMP.data() + 36, &nBadPoints, 4);
  EXPECT_EQ(OGRCreateFromShapeBin(badMP.data(), &poGeom,
                                  static_cast<int>(badMP.size())),
            OGRERR_FAILURE);

  // MultiPoint with huge points (> 50M)
  nBadPoints = 60000000;
  memcpy(badMP.data() + 36, &nBadPoints, 4);
  EXPECT_EQ(OGRCreateFromShapeBin(badMP.data(), &poGeom,
                                  static_cast<int>(badMP.size())),
            OGRERR_FAILURE);

  // Polygon with huge parts (> 10M)
  std::vector<GByte> badPolyParts(48, 0);
  badPolyParts[0] = SHPT_POLYGON;
  int nBadParts = 20000000;
  int nPts = 10;
  memcpy(badPolyParts.data() + 36, &nBadParts, 4);
  memcpy(badPolyParts.data() + 40, &nPts, 4);
  EXPECT_EQ(OGRCreateFromShapeBin(badPolyParts.data(), &poGeom,
                                  static_cast<int>(badPolyParts.size())),
            OGRERR_FAILURE);

  // Polygon with required size > nBytes
  std::vector<GByte> badPolySize(48, 0);
  badPolySize[0] = SHPT_POLYGON;
  nBadParts = 2;
  nPts = 100;
  memcpy(badPolySize.data() + 36, &nBadParts, 4);
  memcpy(badPolySize.data() + 40, &nPts, 4);
  EXPECT_EQ(OGRCreateFromShapeBin(badPolySize.data(), &poGeom,
                                  static_cast<int>(badPolySize.size())),
            OGRERR_FAILURE);

  // Polygon with decreasing part start
  std::vector<GByte> badPolyPartOrder(44 + 8 + 16 * 4, 0);
  badPolyPartOrder[0] = SHPT_POLYGON;
  nBadParts = 2;
  nPts = 4;
  memcpy(badPolyPartOrder.data() + 36, &nBadParts, 4);
  memcpy(badPolyPartOrder.data() + 40, &nPts, 4);
  int part0 = 2;
  int part1 = 1;  // decreasing!
  memcpy(badPolyPartOrder.data() + 44, &part0, 4);
  memcpy(badPolyPartOrder.data() + 48, &part1, 4);
  EXPECT_EQ(OGRCreateFromShapeBin(badPolyPartOrder.data(), &poGeom,
                                  static_cast<int>(badPolyPartOrder.size())),
            OGRERR_FAILURE);

  // Polygon with out of bound part start
  part0 = 100;  // >= nPoints
  memcpy(badPolyPartOrder.data() + 44, &part0, 4);
  EXPECT_EQ(OGRCreateFromShapeBin(badPolyPartOrder.data(), &poGeom,
                                  static_cast<int>(badPolyPartOrder.size())),
            OGRERR_FAILURE);

  // Zlib inflate failure: valid zlib header but corrupted body
  std::vector<GByte> badZlib(20, 0);
  badZlib[0] = SHPT_ARC;
  int uncomp = 100;
  int comp = 8;
  memcpy(badZlib.data() + 4, &uncomp, 4);
  memcpy(badZlib.data() + 8, &comp, 4);
  badZlib[12] = 0x78;
  badZlib[13] = 0xDA;
  // bytes 14..19 corrupted
  EXPECT_EQ(OGRCreateFromShapeBin(badZlib.data(), &poGeom,
                                  static_cast<int>(badZlib.size())),
            OGRERR_FAILURE);

  // Unsupported non-empty geometry for OGRWriteToShapeBin
  OGRGeometryCollection gc;
  gc.addGeometryDirectly(new OGRPoint(1, 2));
  GByte* pabyShape = nullptr;
  int nBytes = 0;
  EXPECT_EQ(OGRWriteToShapeBin(&gc, &pabyShape, &nBytes),
            OGRERR_UNSUPPORTED_OPERATION);
  VSIFree(pabyShape);
}

}  // namespace
}  // namespace autotest2
