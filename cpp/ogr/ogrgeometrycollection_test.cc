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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
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

class OgrGeometryCollectionTest : public ::testing::Test {
 protected:
  void TearDown() override { OSRCleanup(); }
};

TEST_F(OgrGeometryCollectionTest, BasicPropertiesAndConstruction) {
  OGRGeometryCollection gc;
  EXPECT_TRUE(gc.IsEmpty());
  EXPECT_EQ(0, gc.getNumGeometries());
  EXPECT_EQ(0, gc.getDimension());
  EXPECT_STREQ("GEOMETRYCOLLECTION", gc.getGeometryName());
  EXPECT_EQ(wkbGeometryCollection, gc.getGeometryType());

  // Add 2D Point
  auto pt = std::make_unique<OGRPoint>(1.0, 2.0);
  EXPECT_EQ(OGRERR_NONE, gc.addGeometry(pt.get()));
  EXPECT_FALSE(gc.IsEmpty());
  EXPECT_EQ(1, gc.getNumGeometries());
  EXPECT_EQ(0, gc.getDimension());
  EXPECT_THAT(gc.getGeometryRef(0), NotNull());
  EXPECT_EQ(nullptr, gc.getGeometryRef(-1));
  EXPECT_EQ(nullptr, gc.getGeometryRef(1));

  // Add 2D LineString -> dimension becomes 1
  auto ls = std::make_unique<OGRLineString>();
  ls->addPoint(0, 0);
  ls->addPoint(1, 1);
  EXPECT_EQ(OGRERR_NONE, gc.addGeometry(std::move(ls)));
  EXPECT_EQ(2, gc.getNumGeometries());
  EXPECT_EQ(1, gc.getDimension());

  // Add Polygon -> dimension becomes 2
  auto poly = std::make_unique<OGRPolygon>();
  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(0, 0);
  ring->addPoint(1, 0);
  ring->addPoint(1, 1);
  ring->addPoint(0, 0);
  poly->addRingDirectly(ring.release());
  EXPECT_EQ(OGRERR_NONE, gc.addGeometryDirectly(poly.release()));
  EXPECT_EQ(3, gc.getNumGeometries());
  EXPECT_EQ(2, gc.getDimension());

  // Const getGeometryRef
  const OGRGeometryCollection& const_gc = gc;
  EXPECT_THAT(const_gc.getGeometryRef(0), NotNull());
  EXPECT_EQ(nullptr, const_gc.getGeometryRef(-1));
  EXPECT_EQ(nullptr, const_gc.getGeometryRef(5));

  // Copy constructor and clone
  OGRGeometryCollection gc_copy(gc);
  EXPECT_EQ(3, gc_copy.getNumGeometries());
  EXPECT_TRUE(gc.Equals(&gc_copy));

  std::unique_ptr<OGRGeometryCollection> cloned(gc.clone());
  ASSERT_THAT(cloned, NotNull());
  EXPECT_TRUE(gc.Equals(cloned.get()));

  // Move constructor
  OGRGeometryCollection gc_moved(std::move(gc_copy));
  EXPECT_EQ(3, gc_moved.getNumGeometries());
  EXPECT_EQ(0, gc_copy.getNumGeometries());

  // Copy assignment
  OGRGeometryCollection gc_assigned;
  gc_assigned = gc_moved;
  EXPECT_EQ(3, gc_assigned.getNumGeometries());
  // Self assignment
  gc_assigned = *&gc_assigned;
  EXPECT_EQ(3, gc_assigned.getNumGeometries());

  // Move assignment
  OGRGeometryCollection gc_move_assigned;
  gc_move_assigned = std::move(gc_assigned);
  EXPECT_EQ(3, gc_move_assigned.getNumGeometries());
}

TEST_F(OgrGeometryCollectionTest, SubclassIncompatibleAssignment) {
  OGRMultiPoint mp;
  OGRGeometryCollection gc;
  auto ls = std::make_unique<OGRLineString>();
  ls->addPoint(0, 0);
  ls->addPoint(1, 1);
  gc.addGeometryDirectly(ls.release());

  // Assigning gc (containing a LineString) to an OGRMultiPoint through base ref
  OGRGeometryCollection& gcRef = mp;
  {
    WithQuietHandler q;
    gcRef = gc;
  }
  EXPECT_EQ(0, mp.getNumGeometries());
}

TEST_F(OgrGeometryCollectionTest, GeometryTypeAndDimensions) {
  OGRGeometryCollection gc;
  EXPECT_EQ(wkbGeometryCollection, gc.getGeometryType());

  gc.set3D(TRUE);
  EXPECT_EQ(wkbGeometryCollection25D, gc.getGeometryType());

  gc.setMeasured(TRUE);
  EXPECT_EQ(wkbGeometryCollectionZM, gc.getGeometryType());

  gc.set3D(FALSE);
  EXPECT_EQ(wkbGeometryCollectionM, gc.getGeometryType());

  gc.set3D(TRUE);
  gc.flattenTo2D();
  EXPECT_EQ(wkbGeometryCollection, gc.getGeometryType());
  EXPECT_FALSE(gc.Is3D());
  EXPECT_FALSE(gc.IsMeasured());

  gc.setCoordinateDimension(3);
  EXPECT_TRUE(gc.Is3D());
  gc.setCoordinateDimension(2);
  EXPECT_FALSE(gc.Is3D());
}

TEST_F(OgrGeometryCollectionTest, RemoveAndStealGeometry) {
  OGRGeometryCollection gc;
  gc.addGeometryDirectly(new OGRPoint(1, 1));
  gc.addGeometryDirectly(new OGRPoint(2, 2));
  gc.addGeometryDirectly(new OGRPoint(3, 3));
  EXPECT_EQ(3, gc.getNumGeometries());

  // Out of bounds remove
  EXPECT_EQ(OGRERR_FAILURE, gc.removeGeometry(-2));
  EXPECT_EQ(OGRERR_FAILURE, gc.removeGeometry(10));

  // Steal geometry
  EXPECT_EQ(nullptr, gc.stealGeometry(-1));
  EXPECT_EQ(nullptr, gc.stealGeometry(5));
  auto stolen = gc.stealGeometry(1);
  ASSERT_THAT(stolen, NotNull());
  EXPECT_EQ(wkbPoint, stolen->getGeometryType());
  EXPECT_EQ(2, gc.getNumGeometries());

  // Remove single geometry with delete=true
  EXPECT_EQ(OGRERR_NONE, gc.removeGeometry(0, TRUE));
  EXPECT_EQ(1, gc.getNumGeometries());

  // Remove all geometries with -1
  gc.addGeometryDirectly(new OGRPoint(4, 4));
  EXPECT_EQ(2, gc.getNumGeometries());
  EXPECT_EQ(OGRERR_NONE, gc.removeGeometry(-1));
  EXPECT_EQ(0, gc.getNumGeometries());
}

TEST_F(OgrGeometryCollectionTest, EmptyPartsHandling) {
  OGRGeometryCollection gc;
  EXPECT_FALSE(gc.hasEmptyParts());

  auto pt_valid = std::make_unique<OGRPoint>(1, 1);
  gc.addGeometry(pt_valid.get());
  EXPECT_FALSE(gc.hasEmptyParts());

  auto pt_empty = std::make_unique<OGRPoint>();
  pt_empty->empty();
  gc.addGeometry(pt_empty.get());
  EXPECT_TRUE(gc.hasEmptyParts());

  gc.removeEmptyParts();
  EXPECT_FALSE(gc.hasEmptyParts());
  EXPECT_EQ(1, gc.getNumGeometries());
}

TEST_F(OgrGeometryCollectionTest, WkbRoundtripAndVariants) {
  OGRGeometryCollection gc;
  gc.addGeometryDirectly(new OGRPoint(1.0, 2.0));
  gc.addGeometryDirectly(new OGRPoint(3.0, 4.0));

  size_t wkbSize = gc.WkbSize();
  EXPECT_GT(wkbSize, 0);

  std::vector<unsigned char> buffer(wkbSize);
  OGRwkbExportOptions opts;
  opts.eByteOrder = wkbNDR;
  opts.eWkbVariant = wkbVariantIso;
  EXPECT_EQ(OGRERR_NONE, gc.exportToWkb(buffer.data(), &opts));

  OGRGeometryCollection gc_imported;
  size_t bytesConsumed = 0;
  EXPECT_EQ(OGRERR_NONE,
            gc_imported.importFromWkb(buffer.data(), buffer.size(),
                                      wkbVariantIso, bytesConsumed));
  EXPECT_EQ(bytesConsumed, wkbSize);
  EXPECT_TRUE(gc.Equals(&gc_imported));

  // Truncated WKB error
  size_t dummy = 0;
  EXPECT_NE(OGRERR_NONE,
            gc_imported.importFromWkb(buffer.data(), 8, wkbVariantIso, dummy));

  // XDR export
  opts.eByteOrder = wkbXDR;
  EXPECT_EQ(OGRERR_NONE, gc.exportToWkb(buffer.data(), &opts));
  EXPECT_EQ(OGRERR_NONE,
            gc_imported.importFromWkb(buffer.data(), buffer.size(),
                                      wkbVariantIso, bytesConsumed));
  EXPECT_TRUE(gc.Equals(&gc_imported));

  // MultiCurve with OldOgc variant (gets patched to Iso)
  OGRMultiCurve mc;
  auto ls = std::make_unique<OGRLineString>();
  ls->addPoint(0, 0);
  ls->addPoint(1, 1);
  mc.addGeometryDirectly(ls.release());
  std::vector<unsigned char> mc_buf(mc.WkbSize());
  opts = OGRwkbExportOptions{};
  opts.eWkbVariant = wkbVariantOldOgc;
  EXPECT_EQ(OGRERR_NONE, mc.exportToWkb(mc_buf.data(), &opts));

  // PostGIS1 variant
  opts.eWkbVariant = wkbVariantPostGIS1;
  EXPECT_EQ(OGRERR_NONE, mc.exportToWkb(mc_buf.data(), &opts));
  mc.set3D(TRUE);
  mc_buf.resize(mc.WkbSize());
  EXPECT_EQ(OGRERR_NONE, mc.exportToWkb(mc_buf.data(), &opts));

  // MultiSurface with PostGIS1 variant
  OGRMultiSurface ms;
  auto poly = std::make_unique<OGRPolygon>();
  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(0, 0);
  ring->addPoint(1, 0);
  ring->addPoint(1, 1);
  ring->addPoint(0, 0);
  poly->addRingDirectly(ring.release());
  ms.addGeometryDirectly(poly.release());
  std::vector<unsigned char> ms_buf(ms.WkbSize());
  EXPECT_EQ(OGRERR_NONE, ms.exportToWkb(ms_buf.data(), &opts));
  ms.set3D(TRUE);
  ms_buf.resize(ms.WkbSize());
  EXPECT_EQ(OGRERR_NONE, ms.exportToWkb(ms_buf.data(), &opts));
}

TEST_F(OgrGeometryCollectionTest, WktRoundtripAndVariants) {
  OGRGeometryCollection gc;
  OGRErr err = OGRERR_NONE;
  std::string wkt = gc.exportToWkt(OGRWktOptions(), &err);
  EXPECT_EQ(OGRERR_NONE, err);
  EXPECT_EQ("GEOMETRYCOLLECTION EMPTY", wkt);

  gc.addGeometryDirectly(new OGRPoint(10.0, 20.0));
  gc.addGeometryDirectly(new OGRPoint(30.0, 40.0));
  wkt = gc.exportToWkt(OGRWktOptions(), &err);
  EXPECT_EQ(OGRERR_NONE, err);
  EXPECT_EQ("GEOMETRYCOLLECTION (POINT (10 20),POINT (30 40))", wkt);

  // Import from WKT
  const char* pszWkt = wkt.c_str();
  OGRGeometryCollection gc_imported;
  EXPECT_EQ(OGRERR_NONE, gc_imported.importFromWkt(&pszWkt));
  EXPECT_TRUE(gc.Equals(&gc_imported));

  // Nested GeometryCollection in WKT
  const char* pszNested =
      "GEOMETRYCOLLECTION (POINT (1 1), GEOMETRYCOLLECTION (POINT (2 2)))";
  EXPECT_EQ(OGRERR_NONE, gc_imported.importFromWkt(&pszNested));
  EXPECT_EQ(2, gc_imported.getNumGeometries());

  // Corrupt WKT (missing closing paren)
  const char* pszBad = "GEOMETRYCOLLECTION (POINT (1 1)";
  EXPECT_EQ(OGRERR_CORRUPT_DATA, gc_imported.importFromWkt(&pszBad));
}

TEST_F(OgrGeometryCollectionTest, EnvelopeComputation) {
  OGRGeometryCollection gc;
  OGREnvelope env;
  gc.getEnvelope(&env);
  EXPECT_DOUBLE_EQ(0.0, env.MinX);
  EXPECT_DOUBLE_EQ(0.0, env.MaxX);

  OGREnvelope3D env3d;
  gc.getEnvelope(&env3d);
  EXPECT_DOUBLE_EQ(0.0, env3d.MinZ);
  EXPECT_DOUBLE_EQ(0.0, env3d.MaxZ);

  gc.addGeometryDirectly(new OGRPoint(10.0, 20.0, 5.0));
  gc.addGeometryDirectly(new OGRPoint(30.0, 40.0, 15.0));
  gc.getEnvelope(&env);
  EXPECT_DOUBLE_EQ(10.0, env.MinX);
  EXPECT_DOUBLE_EQ(30.0, env.MaxX);
  EXPECT_DOUBLE_EQ(20.0, env.MinY);
  EXPECT_DOUBLE_EQ(40.0, env.MaxY);

  gc.getEnvelope(&env3d);
  EXPECT_DOUBLE_EQ(5.0, env3d.MinZ);
  EXPECT_DOUBLE_EQ(15.0, env3d.MaxZ);
}

TEST_F(OgrGeometryCollectionTest, EqualityComparisons) {
  OGRGeometryCollection gc1;
  OGRGeometryCollection gc2;
  EXPECT_TRUE(gc1.Equals(&gc1));  // Self
  EXPECT_TRUE(gc1.Equals(&gc2));  // Both empty

  gc1.addGeometryDirectly(new OGRPoint(1, 1));
  EXPECT_FALSE(gc1.Equals(&gc2));  // Different count

  gc2.addGeometryDirectly(new OGRPoint(1, 2));
  EXPECT_FALSE(gc1.Equals(&gc2));  // Different points

  OGRPoint pt(1, 1);
  EXPECT_FALSE(gc1.Equals(&pt));  // Different type
}

TEST_F(OgrGeometryCollectionTest, SpatialReferenceAndTransform) {
  OGRGeometryCollection gc;
  auto pt = std::make_unique<OGRPoint>(0.0, 0.0);
  gc.addGeometryDirectly(pt.release());

  auto* srs = new OGRSpatialReference();
  srs->importFromEPSG(4326);
  gc.assignSpatialReference(srs);
  srs->Release();

  EXPECT_THAT(gc.getSpatialReference(), NotNull());
  EXPECT_THAT(gc.getGeometryRef(0)->getSpatialReference(), NotNull());

  auto* targetSRS = new OGRSpatialReference();
  targetSRS->importFromEPSG(3857);
  auto ct = std::unique_ptr<OGRCoordinateTransformation>(
      OGRCreateCoordinateTransformation(gc.getSpatialReference(), targetSRS));
  targetSRS->Release();

  if (ct) {
    EXPECT_EQ(OGRERR_NONE, gc.transform(ct.get()));
  }
}

TEST_F(OgrGeometryCollectionTest, CloseRingsAndSegmentize) {
  OGRGeometryCollection gc;
  auto poly = std::make_unique<OGRPolygon>();
  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(0, 0);
  ring->addPoint(1, 0);
  ring->addPoint(1, 1);
  poly->addRingDirectly(ring.release());
  gc.addGeometryDirectly(poly.release());

  gc.closeRings();
  auto* poly_res = gc.getGeometryRef(0)->toPolygon();
  ASSERT_THAT(poly_res, NotNull());
  EXPECT_TRUE(poly_res->getExteriorRing()->get_IsClosed());

  // Segmentize
  EXPECT_FALSE(gc.segmentize(0.0));
  EXPECT_TRUE(gc.segmentize(0.5));
}

TEST_F(OgrGeometryCollectionTest, LengthAreaAndGeodesic) {
  OGRGeometryCollection gc;
  auto ls = std::make_unique<OGRLineString>();
  ls->addPoint(0, 0);
  ls->addPoint(3, 4);
  gc.addGeometryDirectly(ls.release());

  auto poly = std::make_unique<OGRPolygon>();
  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(0, 0);
  ring->addPoint(10, 0);
  ring->addPoint(10, 10);
  ring->addPoint(0, 10);
  ring->addPoint(0, 0);
  poly->addRingDirectly(ring.release());
  gc.addGeometryDirectly(poly.release());

  EXPECT_DOUBLE_EQ(5.0 + 40.0, gc.get_Length());
  EXPECT_DOUBLE_EQ(100.0, gc.get_Area());

  auto* srs = new OGRSpatialReference();
  srs->importFromEPSG(4326);
  gc.assignSpatialReference(srs);

  double geo_len = gc.get_GeodesicLength(srs);
  EXPECT_GT(geo_len, 0.0);

  double geo_area = gc.get_GeodesicArea(srs);
  EXPECT_GT(geo_area, 0.0);
  srs->Release();
}

TEST_F(OgrGeometryCollectionTest, CurveAndLinearGeometryConversion) {
  OGRGeometryCollection gc;
  auto ls = std::make_unique<OGRLineString>();
  ls->addPoint(0, 0);
  ls->addPoint(1, 1);
  gc.addGeometryDirectly(ls.release());

  EXPECT_FALSE(gc.hasCurveGeometry(TRUE));

  std::unique_ptr<OGRGeometry> linear(gc.getLinearGeometry());
  ASSERT_THAT(linear, NotNull());
  EXPECT_EQ(wkbGeometryCollection, linear->getGeometryType());

  std::unique_ptr<OGRGeometry> curve(gc.getCurveGeometry());
  ASSERT_THAT(curve, NotNull());
  EXPECT_EQ(wkbGeometryCollection, curve->getGeometryType());

  // CastToGeometryCollection
  auto* raw_gc = new OGRGeometryCollection();
  EXPECT_EQ(raw_gc, OGRGeometryCollection::CastToGeometryCollection(raw_gc));
  delete raw_gc;

  auto* mp = new OGRMultiPoint();
  mp->addGeometryDirectly(new OGRPoint(1, 1));
  OGRGeometryCollection* cast_gc =
      OGRGeometryCollection::CastToGeometryCollection(mp);
  ASSERT_THAT(cast_gc, NotNull());
  EXPECT_EQ(wkbGeometryCollection, cast_gc->getGeometryType());
  EXPECT_EQ(1, cast_gc->getNumGeometries());
  delete cast_gc;
}

TEST_F(OgrGeometryCollectionTest, SwapXY) {
  OGRGeometryCollection gc;
  gc.addGeometryDirectly(new OGRPoint(10.0, 20.0));
  gc.swapXY();
  auto* pt = gc.getGeometryRef(0)->toPoint();
  ASSERT_THAT(pt, NotNull());
  EXPECT_DOUBLE_EQ(20.0, pt->getX());
  EXPECT_DOUBLE_EQ(10.0, pt->getY());
}

TEST_F(OgrGeometryCollectionTest, FlattenTo2DWith3DGeom) {
  OGRGeometryCollection gc;
  gc.addGeometryDirectly(new OGRPoint(1.0, 2.0, 3.0));
  EXPECT_TRUE(gc.Is3D());
  gc.flattenTo2D();
  EXPECT_FALSE(gc.Is3D());
  auto* pt = gc.getGeometryRef(0)->toPoint();
  ASSERT_THAT(pt, NotNull());
  EXPECT_DOUBLE_EQ(1.0, pt->getX());
  EXPECT_DOUBLE_EQ(2.0, pt->getY());
}

TEST_F(OgrGeometryCollectionTest, NestedCollectionWkbAndPromotion) {
  OGRGeometryCollection outer;
  auto inner = std::make_unique<OGRGeometryCollection>();
  auto pt = std::make_unique<OGRPoint>(1.0, 2.0, 3.0);
  pt->setM(4.0);
  inner->addGeometryDirectly(pt.release());
  outer.addGeometryDirectly(inner.release());

  size_t wkbSize = outer.WkbSize();
  std::vector<unsigned char> buf(wkbSize);
  EXPECT_EQ(OGRERR_NONE, outer.exportToWkb(buf.data(), nullptr));

  OGRGeometryCollection imported;
  size_t bytesConsumed = 0;
  EXPECT_EQ(OGRERR_NONE, imported.importFromWkb(buf.data(), buf.size(),
                                                wkbVariantIso, bytesConsumed));
  EXPECT_EQ(bytesConsumed, wkbSize);
  EXPECT_TRUE(outer.Equals(&imported));

  // Craft WKB for 3D+M collection containing 2D point -> promotes to 3D+M
  // Header: byte_order (1), type (4 = wkbGeometryCollectionZM = 3007 in Iso),
  // count (4 = 1) Point: byte_order (1), type (4 = wkbPoint = 1), x (8 = 1.0),
  // y (8 = 2.0)
  std::vector<unsigned char> promote_buf = {
      0x01,                    // NDR
      0xbf, 0x0b, 0x00, 0x00,  // type 3007 (wkbGeometryCollectionZM)
      0x01, 0x00, 0x00, 0x00,  // count 1
      0x01,                    // NDR
      0x01, 0x00, 0x00, 0x00,  // type 1 (wkbPoint)
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x3f,  // 1.0
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40,  // 2.0
  };
  OGRGeometryCollection promote_gc;
  size_t consumed = 0;
  EXPECT_EQ(OGRERR_NONE,
            promote_gc.importFromWkb(promote_buf.data(), promote_buf.size(),
                                     wkbVariantIso, consumed));
  ASSERT_EQ(1, promote_gc.getNumGeometries());
  auto* promoted_pt = promote_gc.getGeometryRef(0)->toPoint();
  ASSERT_THAT(promoted_pt, NotNull());
  EXPECT_TRUE(promoted_pt->Is3D());
  EXPECT_TRUE(promoted_pt->IsMeasured());

  // Incompatible sub-geometry in MultiPoint: MultiPoint with LineString
  std::vector<unsigned char> bad_sub_buf = {
      0x01, 0x04, 0x00, 0x00, 0x00,  // type 4 (wkbMultiPoint)
      0x01, 0x00, 0x00, 0x00,        // count 1
      0x01, 0x02, 0x00, 0x00, 0x00,  // type 2 (wkbLineString) -> incompatible
      0x00, 0x00, 0x00, 0x00,        // 0 points
  };
  OGRMultiPoint bad_mp;
  {
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_CORRUPT_DATA,
              bad_mp.importFromWkb(bad_sub_buf.data(), bad_sub_buf.size(),
                                   wkbVariantIso, consumed));
  }

  // Subgeometry header truncated (< 9 bytes remaining)
  std::vector<unsigned char> trunc_sub = {
      0x01, 0x07, 0x00, 0x00, 0x00,  // wkbGeometryCollection
      0x01, 0x00, 0x00, 0x00,        // count 1
      0x01, 0x01, 0x00,              // 3 bytes only (< 9)
  };
  OGRGeometryCollection trunc_gc;
  {
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_NOT_ENOUGH_DATA,
              trunc_gc.importFromWkb(trunc_sub.data(), trunc_sub.size(),
                                     wkbVariantIso, consumed));
  }

  // Recursion depth > 32 in WKB
  std::vector<unsigned char> deep_wkb;
  for (int i = 0; i < 33; ++i) {
    deep_wkb.push_back(0x01);  // NDR
    uint32_t type = 7;         // wkbGeometryCollection
    deep_wkb.insert(deep_wkb.end(), reinterpret_cast<unsigned char*>(&type),
                    reinterpret_cast<unsigned char*>(&type) + 4);
    uint32_t count = 1;
    deep_wkb.insert(deep_wkb.end(), reinterpret_cast<unsigned char*>(&count),
                    reinterpret_cast<unsigned char*>(&count) + 4);
  }
  // leaf empty collection
  deep_wkb.push_back(0x01);
  uint32_t leaf_type = 7;
  deep_wkb.insert(deep_wkb.end(), reinterpret_cast<unsigned char*>(&leaf_type),
                  reinterpret_cast<unsigned char*>(&leaf_type) + 4);
  uint32_t leaf_count = 0;
  deep_wkb.insert(deep_wkb.end(), reinterpret_cast<unsigned char*>(&leaf_count),
                  reinterpret_cast<unsigned char*>(&leaf_count) + 4);
  OGRGeometryCollection deep_gc;
  {
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_CORRUPT_DATA,
              deep_gc.importFromWkb(deep_wkb.data(), deep_wkb.size(),
                                    wkbVariantIso, consumed));
  }

  // Export 3D collection with 2D subgeometry -> warnings about coordinate
  // dimension
  OGRGeometryCollection dim_warn_gc;
  dim_warn_gc.set3D(TRUE);
  auto pt2d = std::make_unique<OGRPoint>(1.0, 2.0);
  dim_warn_gc.addGeometryDirectly(pt2d.release());
  dim_warn_gc.getGeometryRef(0)->set3D(FALSE);
  std::vector<unsigned char> dim_buf(dim_warn_gc.WkbSize());
  {
    WithQuietHandler quiet;
    OGRwkbExportOptions exp_opts;
    exp_opts.eWkbVariant = wkbVariantIso;
    EXPECT_EQ(OGRERR_NONE, dim_warn_gc.exportToWkb(dim_buf.data(), &exp_opts));
  }
}

TEST_F(OgrGeometryCollectionTest, WktRecursionAndZMStripping) {
  // 33 nested levels
  std::string deep_wkt;
  for (int i = 0; i < 33; ++i) {
    deep_wkt += "GEOMETRYCOLLECTION (";
  }
  deep_wkt += "POINT (1 1)";
  for (int i = 0; i < 33; ++i) {
    deep_wkt += ")";
  }
  OGRGeometryCollection gc;
  const char* pszInput = deep_wkt.c_str();
  {
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_CORRUPT_DATA, gc.importFromWkt(&pszInput));
  }

  // Z, M, ZM import and export with OldOgc variant (strips Z/M)
  OGRGeometryCollection gc_z;
  const char* wkt_z = "GEOMETRYCOLLECTION Z (POINT Z (1 2 3))";
  EXPECT_EQ(OGRERR_NONE, gc_z.importFromWkt(&wkt_z));
  EXPECT_TRUE(gc_z.Is3D());
  OGRWktOptions opts_old;
  opts_old.variant = wkbVariantOldOgc;
  std::string out_wkt_z = gc_z.exportToWkt(opts_old);
  EXPECT_EQ("GEOMETRYCOLLECTION (POINT (1 2 3))", out_wkt_z);

  OGRGeometryCollection gc_m;
  const char* wkt_m = "GEOMETRYCOLLECTION M (POINT M (1 2 4))";
  EXPECT_EQ(OGRERR_NONE, gc_m.importFromWkt(&wkt_m));
  EXPECT_TRUE(gc_m.IsMeasured());
  std::string out_wkt_m = gc_m.exportToWkt(opts_old);
  EXPECT_EQ("GEOMETRYCOLLECTION (POINT (1 2))", out_wkt_m);

  OGRGeometryCollection gc_zm;
  const char* wkt_zm = "GEOMETRYCOLLECTION ZM (POINT ZM (1 2 3 4))";
  EXPECT_EQ(OGRERR_NONE, gc_zm.importFromWkt(&wkt_zm));
  EXPECT_TRUE(gc_zm.Is3D());
  EXPECT_TRUE(gc_zm.IsMeasured());
  std::string out_wkt_zm = gc_zm.exportToWkt(opts_old);
  EXPECT_EQ("GEOMETRYCOLLECTION (POINT (1 2 3))", out_wkt_zm);
}

class InverseCT : public OGRCoordinateTransformation {
 public:
  OGRSpatialReference* GetSourceCS() const override { return nullptr; }
  OGRSpatialReference* GetTargetCS() const override { return nullptr; }
  int Transform(size_t nCount, double* x, double* y, double* z, double* m,
                int* pabSuccess) override {
    for (size_t i = 0; i < nCount; ++i) {
      x[i] -= 10.0;
      y[i] -= 10.0;
      if (pabSuccess) pabSuccess[i] = TRUE;
    }
    return TRUE;
  }
  OGRCoordinateTransformation* Clone() const override {
    return new InverseCT();
  }
  OGRCoordinateTransformation* GetInverse() const override { return nullptr; }
};

class PartialFailCT : public OGRCoordinateTransformation {
 public:
  int call_count = 0;
  OGRSpatialReference* GetSourceCS() const override { return nullptr; }
  OGRSpatialReference* GetTargetCS() const override { return nullptr; }
  int Transform(size_t nCount, double* x, double* y, double* z, double* m,
                int* pabSuccess) override {
    call_count++;
    if (call_count == 1) {
      for (size_t i = 0; i < nCount; ++i) {
        x[i] += 10.0;
        y[i] += 10.0;
        if (pabSuccess) pabSuccess[i] = TRUE;
      }
      return TRUE;
    }
    return FALSE;
  }
  OGRCoordinateTransformation* Clone() const override {
    return new PartialFailCT();
  }
  OGRCoordinateTransformation* GetInverse() const override {
    return new InverseCT();
  }
};

TEST_F(OgrGeometryCollectionTest, TransformSecondSubGeomFails) {
  OGRGeometryCollection gc;
  gc.addGeometryDirectly(new OGRPoint(1.0, 2.0));
  gc.addGeometryDirectly(new OGRPoint(3.0, 4.0));
  PartialFailCT ct;
  {
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, gc.transform(&ct));
  }
  // The first point remains transformed because transform() cannot revert it
  auto* pt0 = gc.getGeometryRef(0)->toPoint();
  ASSERT_THAT(pt0, NotNull());
  EXPECT_DOUBLE_EQ(11.0, pt0->getX());
  EXPECT_DOUBLE_EQ(12.0, pt0->getY());
}

TEST_F(OgrGeometryCollectionTest, NestedCollectionLengthAreaAndGeodesic) {
  OGRGeometryCollection outer;
  auto inner_lines = std::make_unique<OGRGeometryCollection>();
  auto ls = std::make_unique<OGRLineString>();
  ls->addPoint(0, 0);
  ls->addPoint(3, 4);  // length = 5
  inner_lines->addGeometryDirectly(ls.release());
  outer.addGeometryDirectly(inner_lines.release());

  auto inner_polys = std::make_unique<OGRMultiSurface>();
  auto poly = std::make_unique<OGRPolygon>();
  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(0, 0);
  ring->addPoint(2, 0);
  ring->addPoint(2, 3);
  ring->addPoint(0, 3);
  ring->addPoint(0, 0);  // area = 6
  poly->addRingDirectly(ring.release());
  inner_polys->addGeometryDirectly(poly.release());
  outer.addGeometryDirectly(inner_polys.release());

  EXPECT_DOUBLE_EQ(15.0, outer.get_Length());
  EXPECT_DOUBLE_EQ(6.0, outer.get_Area());

  auto* srs = new OGRSpatialReference();
  srs->importFromEPSG(4326);
  EXPECT_GT(outer.get_GeodesicLength(srs), 0.0);
  EXPECT_GT(outer.get_GeodesicArea(srs), 0.0);

  // Invalid SRS that causes get_GeodesicArea / get_GeodesicLength to return < 0
  auto* invalid_srs = new OGRSpatialReference();
  EXPECT_LT(outer.get_GeodesicArea(invalid_srs), 0.0);
  EXPECT_LT(outer.get_GeodesicLength(invalid_srs), 0.0);
  invalid_srs->Release();
  srs->Release();
}

TEST_F(OgrGeometryCollectionTest, CurveGeometriesInCollection) {
  OGRGeometryCollection gc;
  auto cs = std::make_unique<OGRCircularString>();
  cs->addPoint(0, 0);
  cs->addPoint(1, 1);
  cs->addPoint(2, 0);
  gc.addGeometryDirectly(cs.release());

  EXPECT_TRUE(gc.hasCurveGeometry(TRUE));

  std::unique_ptr<OGRGeometry> linear(gc.getLinearGeometry());
  ASSERT_THAT(linear, NotNull());
  EXPECT_EQ(wkbGeometryCollection, linear->getGeometryType());

  std::unique_ptr<OGRGeometry> curve(gc.getCurveGeometry());
  ASSERT_THAT(curve, NotNull());
  EXPECT_EQ(wkbGeometryCollection, curve->getGeometryType());
}

}  // namespace
}  // namespace autotest2
