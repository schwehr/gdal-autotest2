// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Unit tests for
// third_party/geos/src/operation/distance/IndexedFacetDistance.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/distance/IndexedFacetDistance.cpp

#include "third_party/geos/include/geos/operation/distance/IndexedFacetDistance.h"

#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/distance/GeometryLocation.h"
#include "third_party/geos/include/geos/util/GEOSException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::operation::distance::GeometryLocation;
using geos::operation::distance::IndexedFacetDistance;
using geos::util::GEOSException;

class IndexedFacetDistanceTest : public ::testing::Test {
 protected:
  IndexedFacetDistanceTest()
      : factory_(GeometryFactory::create()), reader_(factory_.get()) {}

  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(IndexedFacetDistanceTest, StaticAndInstanceDistancePointsAndLines) {
  auto pt0 = ReadWkt("POINT (0 0)");
  auto pt1 = ReadWkt("POINT (3 4)");
  EXPECT_DOUBLE_EQ(IndexedFacetDistance::distance(pt0.get(), pt1.get()), 5.0);
  EXPECT_DOUBLE_EQ(IndexedFacetDistance(pt0.get()).distance(pt1.get()), 5.0);

  auto line = ReadWkt("LINESTRING (0 0, 4 0)");
  auto off_line = ReadWkt("POINT (2 3)");
  EXPECT_DOUBLE_EQ(IndexedFacetDistance::distance(line.get(), off_line.get()),
                   3.0);

  auto diag1 = ReadWkt("LINESTRING (0 0, 4 4)");
  auto diag2 = ReadWkt("LINESTRING (0 4, 4 0)");
  EXPECT_DOUBLE_EQ(IndexedFacetDistance::distance(diag1.get(), diag2.get()),
                   0.0);

  // Polyline with 15 vertices (> FACET_SEQUENCE_SIZE = 6) so multiple tree
  // nodes are constructed and pruned during branch-and-bound traversal.
  auto long_line = ReadWkt(
      "LINESTRING (0 0, 1 1, 2 0, 3 1, 4 0, 5 1, 6 0, 7 1, "
      "8 0, 9 1, 10 0, 11 1, 12 0, 13 1, 14 0)");
  // Segment (9,1)-(10,0) lies on x + y = 10; (9.8, 1.2) projects orthogonally
  // to (9.3, 0.7) at distance |9.8 + 1.2 - 10| / sqrt(2) = 1 / sqrt(2).
  auto query_pt = ReadWkt("POINT (9.8 1.2)");
  EXPECT_NEAR(IndexedFacetDistance::distance(long_line.get(), query_pt.get()),
              1.0 / std::sqrt(2.0), 1e-12);
  EXPECT_NEAR(IndexedFacetDistance(long_line.get()).distance(query_pt.get()),
              1.0 / std::sqrt(2.0), 1e-12);
}

TEST_F(IndexedFacetDistanceTest, PolygonDistanceUsesBoundaryFacets) {
  auto square = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto inside_pt = ReadWkt("POINT (5 4)");
  // Facet distance measures distance to boundary segments, not filled interior.
  EXPECT_DOUBLE_EQ(
      IndexedFacetDistance::distance(square.get(), inside_pt.get()), 4.0);

  auto with_hole = ReadWkt(
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
      "(8 8, 12 8, 12 12, 8 12, 8 8))");
  auto in_hole = ReadWkt("POINT (10 9.5)");
  EXPECT_DOUBLE_EQ(
      IndexedFacetDistance(with_hole.get()).distance(in_hole.get()), 1.5);
}

TEST_F(IndexedFacetDistanceTest, ReusesCachedIndexAcrossRepeatedQueries) {
  auto network =
      ReadWkt("MULTILINESTRING ((0 0, 10 0), (0 10, 10 10), (20 0, 20 10))");
  IndexedFacetDistance ifd(network.get());

  auto p1 = ReadWkt("POINT (5 3)");
  auto p2 = ReadWkt("POINT (15 5)");
  auto p3 = ReadWkt("POINT (5 8)");

  EXPECT_DOUBLE_EQ(ifd.distance(p1.get()), 3.0);
  EXPECT_DOUBLE_EQ(ifd.distance(p2.get()), 5.0);
  EXPECT_DOUBLE_EQ(ifd.distance(p3.get()), 2.0);
  EXPECT_TRUE(ifd.isWithinDistance(p3.get(), 2.0));
  EXPECT_FALSE(ifd.isWithinDistance(p3.get(), 1.99));

  std::unique_ptr<CoordinateSequence> pts = ifd.nearestPoints(p2.get());
  ASSERT_EQ(pts->size(), 2u);
  EXPECT_DOUBLE_EQ(pts->getAt<CoordinateXY>(0).x, 20.0);
  EXPECT_DOUBLE_EQ(pts->getAt<CoordinateXY>(0).y, 5.0);
  EXPECT_DOUBLE_EQ(pts->getAt<CoordinateXY>(1).x, 15.0);
  EXPECT_DOUBLE_EQ(pts->getAt<CoordinateXY>(1).y, 5.0);
}

TEST_F(IndexedFacetDistanceTest, NearestPointsAndLocations) {
  auto line =
      ReadWkt("LINESTRING (0 0, 2 0, 2 6, 4 6, 6 6, 8 6, 10 6, 12 6, 12 0)");
  auto target = ReadWkt("POINT (12 3)");

  std::unique_ptr<CoordinateSequence> static_pts =
      IndexedFacetDistance::nearestPoints(line.get(), target.get());
  ASSERT_EQ(static_pts->size(), 2u);
  EXPECT_DOUBLE_EQ(static_pts->getAt<CoordinateXY>(0).x, 12.0);
  EXPECT_DOUBLE_EQ(static_pts->getAt<CoordinateXY>(0).y, 3.0);
  EXPECT_DOUBLE_EQ(static_pts->getAt<CoordinateXY>(1).x, 12.0);
  EXPECT_DOUBLE_EQ(static_pts->getAt<CoordinateXY>(1).y, 3.0);

  auto seg = ReadWkt("LINESTRING (15 3, 18 3)");
  IndexedFacetDistance ifd(line.get());
  std::vector<GeometryLocation> locs = ifd.nearestLocations(seg.get());
  ASSERT_EQ(locs.size(), 2u);
  EXPECT_EQ(locs[0].getGeometryComponent(), line.get());
  EXPECT_EQ(locs[0].getSegmentIndex(), 7u);
  EXPECT_FALSE(locs[0].isInsideArea());
  EXPECT_DOUBLE_EQ(locs[0].getCoordinate().x, 12.0);
  EXPECT_DOUBLE_EQ(locs[0].getCoordinate().y, 3.0);

  EXPECT_EQ(locs[1].getGeometryComponent(), seg.get());
  EXPECT_EQ(locs[1].getSegmentIndex(), 0u);
  EXPECT_FALSE(locs[1].isInsideArea());
  EXPECT_DOUBLE_EQ(locs[1].getCoordinate().x, 15.0);
  EXPECT_DOUBLE_EQ(locs[1].getCoordinate().y, 3.0);

  // Verify component pointer for MultiLineString vs Point.
  auto multi = ReadWkt("MULTILINESTRING ((0 0, 1 0), (10 0, 10 10))");
  auto pt = ReadWkt("POINT (13 4)");
  IndexedFacetDistance multi_ifd(multi.get());
  std::vector<GeometryLocation> multi_locs =
      multi_ifd.nearestLocations(pt.get());
  ASSERT_EQ(multi_locs.size(), 2u);
  EXPECT_EQ(multi_locs[0].getGeometryComponent(), multi->getGeometryN(1));
  EXPECT_EQ(multi_locs[0].getSegmentIndex(), 0u);
  EXPECT_DOUBLE_EQ(multi_locs[0].getCoordinate().x, 10.0);
  EXPECT_DOUBLE_EQ(multi_locs[0].getCoordinate().y, 4.0);
  // In GEOS FacetSequence::nearestLocations, when the base facet is linear
  // and the query facet is a point, computeDistancePointLine(seqPt, *this, ...)
  // populates both GeometryLocation entries with `this->geom` before swapping.
  EXPECT_EQ(multi_locs[1].getGeometryComponent(), multi->getGeometryN(1));
  EXPECT_EQ(multi_locs[1].getSegmentIndex(), 0u);
  EXPECT_DOUBLE_EQ(multi_locs[1].getCoordinate().x, 13.0);
  EXPECT_DOUBLE_EQ(multi_locs[1].getCoordinate().y, 4.0);

  // When the point is the base geometry and the multiline is the query, both
  // component pointers match their respective geometries.
  IndexedFacetDistance pt_ifd(pt.get());
  std::vector<GeometryLocation> rev_locs = pt_ifd.nearestLocations(multi.get());
  ASSERT_EQ(rev_locs.size(), 2u);
  EXPECT_EQ(rev_locs[0].getGeometryComponent(), pt.get());
  EXPECT_EQ(rev_locs[0].getSegmentIndex(), 0u);
  EXPECT_DOUBLE_EQ(rev_locs[0].getCoordinate().x, 13.0);
  EXPECT_DOUBLE_EQ(rev_locs[0].getCoordinate().y, 4.0);
  EXPECT_EQ(rev_locs[1].getGeometryComponent(), multi->getGeometryN(1));
  EXPECT_EQ(rev_locs[1].getSegmentIndex(), 0u);
  EXPECT_DOUBLE_EQ(rev_locs[1].getCoordinate().x, 10.0);
  EXPECT_DOUBLE_EQ(rev_locs[1].getCoordinate().y, 4.0);

  // Point-to-point nearestLocations and nearestPoints across MultiPoints.
  auto mp1 = ReadWkt("MULTIPOINT ((0 0), (10 0))");
  auto mp2 = ReadWkt("MULTIPOINT ((10 3), (25 25))");
  IndexedFacetDistance mp_ifd(mp1.get());
  EXPECT_DOUBLE_EQ(mp_ifd.distance(mp2.get()), 3.0);
  std::vector<GeometryLocation> mp_locs = mp_ifd.nearestLocations(mp2.get());
  ASSERT_EQ(mp_locs.size(), 2u);
  EXPECT_EQ(mp_locs[0].getGeometryComponent(), mp1->getGeometryN(1));
  EXPECT_EQ(mp_locs[0].getSegmentIndex(), 0u);
  EXPECT_DOUBLE_EQ(mp_locs[0].getCoordinate().x, 10.0);
  EXPECT_DOUBLE_EQ(mp_locs[0].getCoordinate().y, 0.0);
  EXPECT_EQ(mp_locs[1].getGeometryComponent(), mp2->getGeometryN(0));
  EXPECT_EQ(mp_locs[1].getSegmentIndex(), 0u);
  EXPECT_DOUBLE_EQ(mp_locs[1].getCoordinate().x, 10.0);
  EXPECT_DOUBLE_EQ(mp_locs[1].getCoordinate().y, 3.0);

  std::unique_ptr<CoordinateSequence> mp_pts =
      IndexedFacetDistance::nearestPoints(mp1.get(), mp2.get());
  ASSERT_EQ(mp_pts->size(), 2u);
  EXPECT_DOUBLE_EQ(mp_pts->getAt<CoordinateXY>(0).x, 10.0);
  EXPECT_DOUBLE_EQ(mp_pts->getAt<CoordinateXY>(0).y, 0.0);
  EXPECT_DOUBLE_EQ(mp_pts->getAt<CoordinateXY>(1).x, 10.0);
  EXPECT_DOUBLE_EQ(mp_pts->getAt<CoordinateXY>(1).y, 3.0);

  // Polygon hole boundary to inner polygon exterior ring.
  auto outer_geom = ReadWkt(
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
      "(5 5, 15 5, 15 15, 5 15, 5 5))");
  auto inner_geom = ReadWkt("POLYGON ((9 7, 11 8, 10 11, 9 7))");
  const auto* outer = dynamic_cast<const Polygon*>(outer_geom.get());
  const auto* inner = dynamic_cast<const Polygon*>(inner_geom.get());
  ASSERT_NE(outer, nullptr);
  ASSERT_NE(inner, nullptr);
  EXPECT_DOUBLE_EQ(IndexedFacetDistance::distance(outer, inner), 2.0);
  std::vector<GeometryLocation> poly_locs =
      IndexedFacetDistance(outer).nearestLocations(inner);
  ASSERT_EQ(poly_locs.size(), 2u);
  EXPECT_EQ(poly_locs[0].getGeometryComponent(), outer->getInteriorRingN(0));
  EXPECT_EQ(poly_locs[0].getSegmentIndex(), 0u);
  EXPECT_FALSE(poly_locs[0].isInsideArea());
  EXPECT_DOUBLE_EQ(poly_locs[0].getCoordinate().x, 9.0);
  EXPECT_DOUBLE_EQ(poly_locs[0].getCoordinate().y, 5.0);
  EXPECT_EQ(poly_locs[1].getGeometryComponent(), inner->getExteriorRing());
  EXPECT_FALSE(poly_locs[1].isInsideArea());
  EXPECT_DOUBLE_EQ(poly_locs[1].getCoordinate().x, 9.0);
  EXPECT_DOUBLE_EQ(poly_locs[1].getCoordinate().y, 7.0);
}

TEST_F(IndexedFacetDistanceTest, IsWithinDistanceAllBranches) {
  // 1. Negative maxDistance clamps to 0.0.
  {
    auto g1 = ReadWkt("LINESTRING (0 0, 2 0)");
    auto g2 = ReadWkt("LINESTRING (1 0, 1 2)");
    auto g3 = ReadWkt("POINT (1 0.5)");
    IndexedFacetDistance ifd(g1.get());
    EXPECT_TRUE(ifd.isWithinDistance(g2.get(), -3.0));
    EXPECT_FALSE(ifd.isWithinDistance(g3.get(), -1.0));
  }

  // 2. Envelope distance > maxDistance short-circuit.
  {
    auto g1 = ReadWkt("LINESTRING (0 0, 1 1)");
    auto g2 = ReadWkt("LINESTRING (4 5, 5 6)");
    // Envelope gap is (3, 4) -> envDist = 5.0.
    EXPECT_FALSE(
        IndexedFacetDistance(g1.get()).isWithinDistance(g2.get(), 4.9));
  }

  // 3. Atomic baseGeometry heuristic: envelope intersects g's envelope
  // (envDist == 0), g's envelope does NOT contain base's envelope.
  // base = (0,0)-(10,10); g = (0,9)-(0,10)-(1,10).
  // env2 = [0,1]x[9,10]; distance(base, env2) = |1 - 9|/sqrt(2) = 8/sqrt(2)
  // ~ 5.657. Actual facet distance(base, g) = 9/sqrt(2) ~ 6.364.
  {
    auto diag = ReadWkt("LINESTRING (0 0, 10 10)");
    auto corner = ReadWkt("LINESTRING (0 9, 0 10, 1 10)");
    IndexedFacetDistance ifd(diag.get());
    EXPECT_FALSE(ifd.isWithinDistance(corner.get(), 3.0));
    EXPECT_FALSE(ifd.isWithinDistance(corner.get(), 6.0));
    EXPECT_TRUE(ifd.isWithinDistance(corner.get(), 6.5));
  }

  // 4. Atomic baseGeometry whose envelope IS contained in g's envelope
  // skips the env2 heuristic and uses the STRtree check directly.
  {
    auto inner_pt = ReadWkt("POINT (5 5)");
    auto box = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
    IndexedFacetDistance ifd(inner_pt.get());
    EXPECT_FALSE(ifd.isWithinDistance(box.get(), 4.9));
    EXPECT_TRUE(ifd.isWithinDistance(box.get(), 5.0));
  }

  // 5. Multi-geometry baseGeometry (getNumGeometries() > 1) skips heuristic.
  {
    auto multi = ReadWkt("MULTILINESTRING ((0 0, 1 0), (10 0, 11 0))");
    auto mid_pt = ReadWkt("POINT (5 0)");
    IndexedFacetDistance ifd(multi.get());
    EXPECT_FALSE(ifd.isWithinDistance(mid_pt.get(), 3.9));
    EXPECT_TRUE(ifd.isWithinDistance(mid_pt.get(), 4.0));
  }

  // 6. Empty geometry collection yields false for isWithinDistance.
  {
    auto empty_multi = ReadWkt("MULTIPOINT EMPTY");
    auto pt = ReadWkt("POINT (0 0)");
    EXPECT_FALSE(IndexedFacetDistance(empty_multi.get())
                     .isWithinDistance(pt.get(), 1.0));
    EXPECT_FALSE(IndexedFacetDistance(empty_multi.get())
                     .isWithinDistance(
                         pt.get(), std::numeric_limits<double>::infinity()));

    auto multi = ReadWkt("MULTIPOINT ((0 0), (2 2))");
    auto empty_pt = ReadWkt("POINT EMPTY");
    EXPECT_FALSE(IndexedFacetDistance(multi.get())
                     .isWithinDistance(empty_pt.get(), 1.0));
  }

  // 7. Exact boundary cases: maxDistance == 0.0, envDist == maxDistance, +inf.
  {
    auto l1 = ReadWkt("LINESTRING (0 0, 2 0)");
    auto touching = ReadWkt("LINESTRING (1 0, 1 2)");
    auto near_disjoint = ReadWkt("LINESTRING (1 0.001, 1 2)");
    IndexedFacetDistance ifd(l1.get());
    EXPECT_TRUE(ifd.isWithinDistance(touching.get(), 0.0));
    EXPECT_FALSE(ifd.isWithinDistance(near_disjoint.get(), 0.0));

    // Envelope distance is 5.0; actual facet distance is sqrt(32.5) > 5.0.
    auto l_diag = ReadWkt("LINESTRING (0 0, 0 1, 1 0)");
    auto l_far = ReadWkt("LINESTRING (4 5, 5 6)");
    IndexedFacetDistance diag_ifd(l_diag.get());
    EXPECT_FALSE(diag_ifd.isWithinDistance(l_far.get(), 5.0));
    EXPECT_TRUE(diag_ifd.isWithinDistance(
        l_far.get(), std::numeric_limits<double>::infinity()));
  }
}

TEST_F(IndexedFacetDistanceTest, GeometryCollectionAnd3DCoordinates) {
  auto gc = ReadWkt(
      "GEOMETRYCOLLECTION (POINT (0 0), LINESTRING (10 0, 10 10), "
      "POLYGON ((20 0, 30 0, 30 10, 20 10, 20 0)))");
  auto query = ReadWkt("POINT (18 5)");
  IndexedFacetDistance gc_ifd(gc.get());
  EXPECT_DOUBLE_EQ(gc_ifd.distance(query.get()), 2.0);
  EXPECT_TRUE(gc_ifd.isWithinDistance(query.get(), 2.0));
  EXPECT_FALSE(gc_ifd.isWithinDistance(query.get(), 1.99));

  std::unique_ptr<CoordinateSequence> pts = gc_ifd.nearestPoints(query.get());
  ASSERT_EQ(pts->size(), 2u);
  EXPECT_DOUBLE_EQ(pts->getAt<CoordinateXY>(0).x, 20.0);
  EXPECT_DOUBLE_EQ(pts->getAt<CoordinateXY>(0).y, 5.0);
  EXPECT_DOUBLE_EQ(pts->getAt<CoordinateXY>(1).x, 18.0);
  EXPECT_DOUBLE_EQ(pts->getAt<CoordinateXY>(1).y, 5.0);

  // Facet distance operates in 2D XY space even for 3D Z geometries.
  auto p3d = ReadWkt("POINT Z (0 0 100)");
  auto l3d = ReadWkt("LINESTRING Z (3 0 -50, 3 8 50)");
  EXPECT_DOUBLE_EQ(IndexedFacetDistance::distance(p3d.get(), l3d.get()), 3.0);
  std::unique_ptr<CoordinateSequence> pts3d =
      IndexedFacetDistance::nearestPoints(p3d.get(), l3d.get());
  ASSERT_EQ(pts3d->size(), 2u);
  EXPECT_DOUBLE_EQ(pts3d->getAt<CoordinateXY>(0).x, 0.0);
  EXPECT_DOUBLE_EQ(pts3d->getAt<CoordinateXY>(0).y, 0.0);
  EXPECT_DOUBLE_EQ(pts3d->getAt<CoordinateXY>(1).x, 3.0);
  EXPECT_DOUBLE_EQ(pts3d->getAt<CoordinateXY>(1).y, 0.0);
}

TEST_F(IndexedFacetDistanceTest, ThrowsOnEmptyGeometriesAndMixedCollection) {
  auto pt = ReadWkt("POINT (1 2)");
  for (const char* empty_wkt : {"POINT EMPTY", "LINESTRING EMPTY",
                                "POLYGON EMPTY", "GEOMETRYCOLLECTION EMPTY"}) {
    SCOPED_TRACE(empty_wkt);
    auto empty_geom = ReadWkt(empty_wkt);

    IndexedFacetDistance empty_base(empty_geom.get());
    EXPECT_THROW(empty_base.distance(pt.get()), GEOSException);
    EXPECT_THROW(empty_base.nearestLocations(pt.get()), GEOSException);
    EXPECT_THROW(empty_base.nearestPoints(pt.get()), GEOSException);

    IndexedFacetDistance non_empty_base(pt.get());
    EXPECT_THROW(non_empty_base.distance(empty_geom.get()), GEOSException);
    EXPECT_THROW(non_empty_base.nearestLocations(empty_geom.get()),
                 GEOSException);
    EXPECT_THROW(non_empty_base.nearestPoints(empty_geom.get()), GEOSException);

    EXPECT_THROW(IndexedFacetDistance::distance(empty_geom.get(), pt.get()),
                 GEOSException);
    EXPECT_THROW(
        IndexedFacetDistance::nearestPoints(pt.get(), empty_geom.get()),
        GEOSException);
  }

  // Collection containing both an empty and a non-empty component succeeds.
  auto mixed = ReadWkt("GEOMETRYCOLLECTION (POINT EMPTY, POINT (4 6))");
  EXPECT_DOUBLE_EQ(IndexedFacetDistance::distance(mixed.get(), pt.get()), 5.0);
}

}  // namespace
}  // namespace geos_google
