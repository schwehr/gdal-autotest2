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

#include "third_party/geos/include/geos/operation/distance/DistanceOp.h"

#include <cmath>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::io::WKTReader;
using geos::operation::distance::DistanceOp;
using geos::util::UnsupportedOperationException;

class DistanceOpTest : public ::testing::Test {
 protected:
  DistanceOpTest() : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  void ExpectNearestPointsNear(const CoordinateSequence* pts, double x0,
                               double y0, double x1, double y1,
                               double tol = 1e-9) const {
    ASSERT_NE(pts, nullptr);
    ASSERT_EQ(pts->size(), 2u);
    EXPECT_NEAR(pts->getX(0), x0, tol);
    EXPECT_NEAR(pts->getY(0), y0, tol);
    EXPECT_NEAR(pts->getX(1), x1, tol);
    EXPECT_NEAR(pts->getY(1), y1, tol);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(DistanceOpTest, StaticAndMemberEntryPointsAndCaching) {
  auto p0 = ReadWkt("POINT (0 0)");
  auto p1 = ReadWkt("POINT (3 4)");
  auto l0 = ReadWkt("LINESTRING (0 0, 10 0)");
  auto l1 = ReadWkt("LINESTRING (0 5, 10 5)");

  // Static reference and pointer overloads.
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*p0, *p1), 5.0);
  EXPECT_DOUBLE_EQ(DistanceOp::distance(p0.get(), p1.get()), 5.0);
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*l0, *l1), 5.0);
  EXPECT_DOUBLE_EQ(DistanceOp::distance(l0.get(), l1.get()), 5.0);

  auto static_pts = DistanceOp::nearestPoints(p0.get(), p1.get());
  ExpectNearestPointsNear(static_pts.get(), 0.0, 0.0, 3.0, 4.0);

  // Deprecated pointer constructor.
  DistanceOp ptr_op(l0.get(), l1.get());
  EXPECT_DOUBLE_EQ(ptr_op.distance(), 5.0);
  // Repeated calls reuse the cached result (if (computed) return;).
  EXPECT_DOUBLE_EQ(ptr_op.distance(), 5.0);
  auto ptr_pts = ptr_op.nearestPoints();
  ExpectNearestPointsNear(ptr_pts.get(), 0.0, 0.0, 0.0, 5.0);

  // Point-Point fast path in distance() followed by nearestPoints() and again
  // distance(), plus nearestPoints() before distance().
  DistanceOp pt_op(*p0, *p1);
  EXPECT_DOUBLE_EQ(pt_op.distance(), 5.0);
  auto pt_pts = pt_op.nearestPoints();
  ExpectNearestPointsNear(pt_pts.get(), 0.0, 0.0, 3.0, 4.0);
  EXPECT_DOUBLE_EQ(pt_op.distance(), 5.0);

  DistanceOp pt_op_rev(*p0, *p1);
  ExpectNearestPointsNear(pt_op_rev.nearestPoints().get(), 0.0, 0.0, 3.0, 4.0);
  EXPECT_DOUBLE_EQ(pt_op_rev.distance(), 5.0);

  // NearestPoints first, then distance.
  DistanceOp line_op(*l0, *l1);
  auto line_pts = line_op.nearestPoints();
  ExpectNearestPointsNear(line_pts.get(), 0.0, 0.0, 0.0, 5.0);
  EXPECT_DOUBLE_EQ(line_op.distance(), 5.0);

  // Negative terminateDistance is clamped to 0.0 and does not stop early on
  // positive distances.
  DistanceOp neg_term_op(*l0, *l1, -5.0);
  EXPECT_DOUBLE_EQ(neg_term_op.distance(), 5.0);
  auto neg_pts = neg_term_op.nearestPoints();
  ExpectNearestPointsNear(neg_pts.get(), 0.0, 0.0, 0.0, 5.0);
}

TEST_F(DistanceOpTest, EmptyGeometriesReturnZeroAndNullNearestPoints) {
  auto empty_pt = ReadWkt("POINT EMPTY");
  auto empty_line = ReadWkt("LINESTRING EMPTY");
  auto empty_poly = ReadWkt("POLYGON EMPTY");
  auto empty_gc = ReadWkt("GEOMETRYCOLLECTION EMPTY");
  auto gc_all_empty =
      ReadWkt("GEOMETRYCOLLECTION (POINT EMPTY, LINESTRING EMPTY)");
  auto pt = ReadWkt("POINT (3 4)");
  auto line = ReadWkt("LINESTRING (0 0, 10 0)");
  auto poly = ReadWkt("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");

  const Geometry* empties[] = {empty_pt.get(), empty_line.get(),
                               empty_poly.get(), empty_gc.get(),
                               gc_all_empty.get()};

  for (const Geometry* empty : empties) {
    EXPECT_DOUBLE_EQ(DistanceOp::distance(*empty, *pt), 0.0);
    EXPECT_DOUBLE_EQ(DistanceOp::distance(*pt, *empty), 0.0);
    EXPECT_DOUBLE_EQ(DistanceOp::distance(*empty, *line), 0.0);
    EXPECT_DOUBLE_EQ(DistanceOp::distance(*poly, *empty), 0.0);
    EXPECT_EQ(DistanceOp::nearestPoints(empty, pt.get()), nullptr);
    EXPECT_EQ(DistanceOp::nearestPoints(pt.get(), empty), nullptr);
    EXPECT_EQ(DistanceOp::nearestPoints(empty, poly.get()), nullptr);
    EXPECT_FALSE(DistanceOp::isWithinDistance(*empty, *pt, 0.0));
    EXPECT_FALSE(DistanceOp::isWithinDistance(*pt, *empty, 100.0));
  }

  EXPECT_DOUBLE_EQ(DistanceOp::distance(*empty_pt, *empty_poly), 0.0);
  EXPECT_EQ(DistanceOp::nearestPoints(empty_pt.get(), empty_poly.get()),
            nullptr);
  EXPECT_FALSE(DistanceOp::isWithinDistance(*empty_pt, *empty_poly, 10.0));
}

TEST_F(DistanceOpTest, CurvedComponentsThrowUnsupportedOperationException) {
  auto arc = ReadWkt("CIRCULARSTRING (0 0, 1 1, 2 0)");
  auto empty_arc = ReadWkt("CIRCULARSTRING EMPTY");
  auto curve_poly = ReadWkt(
      "CURVEPOLYGON (COMPOUNDCURVE (CIRCULARSTRING (0 0, 1 1, 2 0), (2 0, 0 "
      "0)))");
  auto gc_with_curve = ReadWkt(
      "GEOMETRYCOLLECTION (POINT (0 0), CIRCULARSTRING (0 0, 1 1, 2 0))");
  auto pt_near = ReadWkt("POINT (1 0)");
  auto pt_far = ReadWkt("POINT (50 50)");

  EXPECT_THROW(DistanceOp::distance(*arc, *pt_near),
               UnsupportedOperationException);
  EXPECT_THROW(DistanceOp::distance(*pt_near, *arc),
               UnsupportedOperationException);
  EXPECT_THROW(DistanceOp::distance(*empty_arc, *pt_near),
               UnsupportedOperationException);
  EXPECT_THROW(DistanceOp::distance(*curve_poly, *pt_near),
               UnsupportedOperationException);
  EXPECT_THROW(DistanceOp::distance(*gc_with_curve, *pt_near),
               UnsupportedOperationException);

  // isWithinDistance throws once envelope pruning does not short-circuit.
  EXPECT_THROW(DistanceOp::isWithinDistance(*arc, *pt_near, 10.0),
               UnsupportedOperationException);
  // Envelope short-circuit returns false before DistanceOp::distance() runs.
  EXPECT_FALSE(DistanceOp::isWithinDistance(*arc, *pt_far, 1.0));
}

TEST_F(DistanceOpTest, PointToPointAndMultiPointDistances) {
  auto p0 = ReadWkt("POINT (2 3)");
  auto p0_dup = ReadWkt("POINT (2 3)");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*p0, *p0_dup), 0.0);
  auto same_pts = DistanceOp::nearestPoints(p0.get(), p0_dup.get());
  ExpectNearestPointsNear(same_pts.get(), 2.0, 3.0, 2.0, 3.0);

  // Z and M ordinates do not affect 2D distance; nearestPoints omits Z.
  auto p_z0 = ReadWkt("POINT Z (0 0 100)");
  auto p_z1 = ReadWkt("POINT Z (3 4 200)");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*p_z0, *p_z1), 5.0);
  auto z_pts = DistanceOp::nearestPoints(p_z0.get(), p_z1.get());
  ExpectNearestPointsNear(z_pts.get(), 0.0, 0.0, 3.0, 4.0);
  EXPECT_TRUE(std::isnan(z_pts->getAt(0).z));
  EXPECT_TRUE(std::isnan(z_pts->getAt(1).z));

  auto p_m = ReadWkt("POINT M (0 0 10)");
  auto p_zm = ReadWkt("POINT ZM (3 4 50 20)");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*p_m, *p_zm), 5.0);

  // MultiPoint with empty components and varying pairwise distances.
  auto mp0 = ReadWkt("MULTIPOINT (EMPTY, (0 0), (10 10))");
  auto mp1 = ReadWkt("MULTIPOINT (EMPTY, (20 20), (3 4))");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*mp0, *mp1), 5.0);
  auto mp_pts = DistanceOp::nearestPoints(mp0.get(), mp1.get());
  ExpectNearestPointsNear(mp_pts.get(), 0.0, 0.0, 3.0, 4.0);

  // Early termination inside computeMinDistancePoints when terminateDistance is
  // satisfied before later closer pairs.
  auto mp_early0 = ReadWkt("MULTIPOINT ((0 0), (100 0))");
  auto mp_early1 = ReadWkt("MULTIPOINT ((20 0), (3 4), (100 0))");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*mp_early0, *mp_early1), 0.0);
  DistanceOp mp_term_op(*mp_early0, *mp_early1, 5.0);
  EXPECT_DOUBLE_EQ(mp_term_op.distance(), 5.0);
  auto mp_term_pts = mp_term_op.nearestPoints();
  ExpectNearestPointsNear(mp_term_pts.get(), 0.0, 0.0, 3.0, 4.0);
}

TEST_F(DistanceOpTest, LineToPointAndPointToLineAndLinearRing) {
  auto line = ReadWkt("LINESTRING (0 0, 10 0)");
  auto pt_on = ReadWkt("POINT (5 0)");
  auto pt_end = ReadWkt("POINT (10 0)");
  auto pt_above = ReadWkt("POINT (5 3)");
  auto pt_past_end = ReadWkt("POINT (13 4)");

  EXPECT_DOUBLE_EQ(DistanceOp::distance(*line, *pt_on), 0.0);
  ExpectNearestPointsNear(
      DistanceOp::nearestPoints(line.get(), pt_on.get()).get(), 5.0, 0.0, 5.0,
      0.0);
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*line, *pt_end), 0.0);

  // Line vs Point (non-flipped).
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*line, *pt_above), 3.0);
  ExpectNearestPointsNear(
      DistanceOp::nearestPoints(line.get(), pt_above.get()).get(), 5.0, 0.0,
      5.0, 3.0);

  // Point vs Line (flipped order in updateMinDistance).
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*pt_above, *line), 3.0);
  ExpectNearestPointsNear(
      DistanceOp::nearestPoints(pt_above.get(), line.get()).get(), 5.0, 3.0,
      5.0, 0.0);

  // Closest point is an endpoint.
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*pt_past_end, *line), 5.0);
  ExpectNearestPointsNear(
      DistanceOp::nearestPoints(pt_past_end.get(), line.get()).get(), 13.0, 4.0,
      10.0, 0.0);

  // Multi-segment LineString where intermediate segment is farther and final
  // segment is closest.
  auto multi_seg = ReadWkt("LINESTRING (0 0, 2 0, 2 10, 5 10, 5 2)");
  auto pt_target = ReadWkt("POINT (4 2)");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*multi_seg, *pt_target), 1.0);
  ExpectNearestPointsNear(
      DistanceOp::nearestPoints(multi_seg.get(), pt_target.get()).get(), 5.0,
      2.0, 4.0, 2.0);

  // MultiLineString envelope pruning (env0->distance(*env1) > minDistance).
  auto mline = ReadWkt("MULTILINESTRING ((0 0, 10 0), (100 100, 110 100))");
  auto pt_near_first = ReadWkt("POINT (5 1)");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*mline, *pt_near_first), 1.0);
  ExpectNearestPointsNear(
      DistanceOp::nearestPoints(mline.get(), pt_near_first.get()).get(), 5.0,
      0.0, 5.0, 1.0);

  // Early termination inside computeMinDistance(LineString*, Point*).
  auto line_revisit = ReadWkt("LINESTRING (0 0, 10 0, 10 20, 5 1)");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*line_revisit, *pt_near_first), 0.0);
  DistanceOp line_pt_term(*line_revisit, *pt_near_first, 1.5);
  EXPECT_DOUBLE_EQ(line_pt_term.distance(), 1.0);
  ExpectNearestPointsNear(line_pt_term.nearestPoints().get(), 5.0, 0.0, 5.0,
                          1.0);

  // computeMinDistanceLinesPoints: empty Point skipped and early termination
  // across multiple lines/points.
  auto mline_two = ReadWkt("MULTILINESTRING ((0 0, 10 0), (20 20, 30 20))");
  auto gc_pts =
      ReadWkt("GEOMETRYCOLLECTION (POINT EMPTY, POINT (5 1), POINT (25 20))");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*mline_two, *gc_pts), 0.0);
  DistanceOp mline_pts_term(*mline_two, *gc_pts, 1.5);
  EXPECT_DOUBLE_EQ(mline_pts_term.distance(), 1.0);
  ExpectNearestPointsNear(mline_pts_term.nearestPoints().get(), 5.0, 0.0, 5.0,
                          1.0);

  // LinearRing is a 1D closed curve, not a 2D Polygon: interior point has
  // positive distance to the ring boundary.
  auto ring = ReadWkt("LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  auto pt_inside_ring = ReadWkt("POINT (5 5)");
  auto pt_outside_ring = ReadWkt("POINT (13 4)");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*ring, *pt_inside_ring), 5.0);
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*ring, *pt_outside_ring), 3.0);
  ExpectNearestPointsNear(
      DistanceOp::nearestPoints(ring.get(), pt_outside_ring.get()).get(), 10.0,
      4.0, 13.0, 4.0);
}

TEST_F(DistanceOpTest, LineToLinePruningAndEarlyTermination) {
  // Crossing segments.
  auto cross_a = ReadWkt("LINESTRING (0 0, 4 4)");
  auto cross_b = ReadWkt("LINESTRING (0 4, 4 0)");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*cross_a, *cross_b), 0.0);
  ExpectNearestPointsNear(
      DistanceOp::nearestPoints(cross_a.get(), cross_b.get()).get(), 2.0, 2.0,
      2.0, 2.0);

  // Parallel overlapping segments.
  auto par_a = ReadWkt("LINESTRING (0 0, 10 0)");
  auto par_b = ReadWkt("LINESTRING (2 3, 8 3)");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*par_a, *par_b), 3.0);
  ExpectNearestPointsNear(
      DistanceOp::nearestPoints(par_a.get(), par_b.get()).get(), 2.0, 0.0, 2.0,
      3.0);

  // Multi-segment pruning for segEnv0 vs lineEnv1 and segEnv0 vs segEnv1.
  auto prune_a = ReadWkt("LINESTRING (0 0, 1 0, 1 -30, 30 -30)");
  auto prune_b = ReadWkt("LINESTRING (0 1, 1 1, 5 1, 15 10)");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*prune_a, *prune_b), 1.0);
  ExpectNearestPointsNear(
      DistanceOp::nearestPoints(prune_a.get(), prune_b.get()).get(), 0.0, 0.0,
      0.0, 1.0);

  // Segment envelopes overlap (distSq == 0), yet actual segment-to-segment
  // distance exceeds current minDistance.
  auto overlap_env_a =
      ReadWkt("LINESTRING (0 0, 2 0, 2 -2, 22 -2, 22 10, 10 20)");
  auto overlap_env_b = ReadWkt("LINESTRING (0 0.5, 2 0.5, 2 10, 10 10, 12 12)");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*overlap_env_a, *overlap_env_b), 0.5);
  ExpectNearestPointsNear(
      DistanceOp::nearestPoints(overlap_env_a.get(), overlap_env_b.get()).get(),
      0.0, 0.0, 0.0, 0.5);

  // MultiLineString envelope pruning (lineEnv0->distance(*lineEnv1) >
  // minDistance).
  auto ml_prune_a = ReadWkt("MULTILINESTRING ((0 0, 5 0), (100 100, 110 100))");
  auto ml_prune_b = ReadWkt("LINESTRING (0 2, 5 2)");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*ml_prune_a, *ml_prune_b), 2.0);
  ExpectNearestPointsNear(
      DistanceOp::nearestPoints(ml_prune_a.get(), ml_prune_b.get()).get(), 0.0,
      0.0, 0.0, 2.0);

  // Early termination inside computeMinDistance(LineString*, LineString*) and
  // computeMinDistanceLines().
  auto ml_term_a =
      ReadWkt("MULTILINESTRING ((0 0, 10 0, 10 5), (50 50, 60 50))");
  auto ml_term_b =
      ReadWkt("MULTILINESTRING ((0 1, 10 1, 10 20), (50 50, 60 50))");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*ml_term_a, *ml_term_b), 0.0);
  DistanceOp line_term_op(*ml_term_a, *ml_term_b, 1.5);
  EXPECT_DOUBLE_EQ(line_term_op.distance(), 1.0);
  ExpectNearestPointsNear(line_term_op.nearestPoints().get(), 0.0, 0.0, 0.0,
                          1.0);
}

TEST_F(DistanceOpTest, PolygonContainmentHolesAndBoundaries) {
  auto big_poly = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto small_poly = ReadWkt("POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))");
  auto pt_inside = ReadWkt("POINT (5 5)");
  auto pt_boundary = ReadWkt("POINT (0 5)");

  // Point inside Polygon (both orders: polys1 and polys0 flip).
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*pt_inside, *big_poly), 0.0);
  ExpectNearestPointsNear(
      DistanceOp::nearestPoints(pt_inside.get(), big_poly.get()).get(), 5.0,
      5.0, 5.0, 5.0);
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*big_poly, *pt_inside), 0.0);
  ExpectNearestPointsNear(
      DistanceOp::nearestPoints(big_poly.get(), pt_inside.get()).get(), 5.0,
      5.0, 5.0, 5.0);

  // Point on Polygon boundary.
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*pt_boundary, *big_poly), 0.0);
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*big_poly, *pt_boundary), 0.0);

  // LineString starting inside Polygon.
  auto line_from_inside = ReadWkt("LINESTRING (5 5, 20 20)");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*line_from_inside, *big_poly), 0.0);
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*big_poly, *line_from_inside), 0.0);

  // LineString passing through Polygon with both endpoints outside.
  auto line_through = ReadWkt("LINESTRING (-5 5, 15 5)");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*line_through, *big_poly), 0.0);
  auto line_through_pts =
      DistanceOp::nearestPoints(line_through.get(), big_poly.get());
  ASSERT_NE(line_through_pts, nullptr);
  EXPECT_DOUBLE_EQ(
      line_through_pts->getAt(0).distance(line_through_pts->getAt(1)), 0.0);

  // Disjoint polygons.
  auto left_poly = ReadWkt("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  auto right_poly = ReadWkt("POLYGON ((6 0, 10 0, 10 4, 6 4, 6 0))");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*left_poly, *right_poly), 2.0);
  ExpectNearestPointsNear(
      DistanceOp::nearestPoints(left_poly.get(), right_poly.get()).get(), 4.0,
      0.0, 6.0, 0.0);

  // Polygon inside Polygon (both orders).
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*small_poly, *big_poly), 0.0);
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*big_poly, *small_poly), 0.0);

  // Cross ("+") polygons: edges cross, but no vertex of either lies inside the
  // other, so containment passes fall through to computeFacetDistance().
  auto cross_vert = ReadWkt("POLYGON ((4 -2, 6 -2, 6 12, 4 12, 4 -2))");
  auto cross_horiz = ReadWkt("POLYGON ((-2 4, 12 4, 12 6, -2 6, -2 4))");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*cross_vert, *cross_horiz), 0.0);
  auto cross_pts =
      DistanceOp::nearestPoints(cross_vert.get(), cross_horiz.get());
  ASSERT_NE(cross_pts, nullptr);
  EXPECT_DOUBLE_EQ(cross_pts->getAt(0).distance(cross_pts->getAt(1)), 0.0);

  // Polygon with hole: point inside hole is exterior to polygon.
  auto poly_with_hole = ReadWkt(
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), (4 4, 16 4, 16 16, 4 16, 4 4))");
  auto pt_in_hole = ReadWkt("POINT (6 10)");
  auto pt_on_hole_edge = ReadWkt("POINT (4 10)");
  auto poly_in_hole = ReadWkt("POLYGON ((6 6, 14 6, 14 14, 6 14, 6 6))");

  EXPECT_DOUBLE_EQ(DistanceOp::distance(*pt_in_hole, *poly_with_hole), 2.0);
  ExpectNearestPointsNear(
      DistanceOp::nearestPoints(pt_in_hole.get(), poly_with_hole.get()).get(),
      6.0, 10.0, 4.0, 10.0);
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*poly_with_hole, *pt_in_hole), 2.0);
  ExpectNearestPointsNear(
      DistanceOp::nearestPoints(poly_with_hole.get(), pt_in_hole.get()).get(),
      4.0, 10.0, 6.0, 10.0);

  EXPECT_DOUBLE_EQ(DistanceOp::distance(*pt_on_hole_edge, *poly_with_hole),
                   0.0);
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*poly_in_hole, *poly_with_hole), 2.0);

  // MultiPoint vs MultiPolygon where only a later point is inside a later
  // polygon (exercises inner/outer loops in computeInside).
  auto mp = ReadWkt("MULTIPOINT ((50 50), (15 5))");
  auto mpoly = ReadWkt(
      "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
      "((10 0, 20 0, 20 10, 10 10, 10 0)))");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*mp, *mpoly), 0.0);
  ExpectNearestPointsNear(
      DistanceOp::nearestPoints(mp.get(), mpoly.get()).get(), 15.0, 5.0, 15.0,
      5.0);
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*mpoly, *mp), 0.0);
  ExpectNearestPointsNear(
      DistanceOp::nearestPoints(mpoly.get(), mp.get()).get(), 15.0, 5.0, 15.0,
      5.0);
}

TEST_F(DistanceOpTest, GeometryCollectionsAndFacetStageEarlyTermination) {
  // Stage 1 (lines0-lines1) early exit in computeFacetDistance().
  auto gc_s1_a =
      ReadWkt("GEOMETRYCOLLECTION (LINESTRING (0 0, 10 0), POINT (100 100))");
  auto gc_s1_b =
      ReadWkt("GEOMETRYCOLLECTION (LINESTRING (0 1, 10 1), POINT (100 100))");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*gc_s1_a, *gc_s1_b), 0.0);
  DistanceOp s1_term(*gc_s1_a, *gc_s1_b, 1.5);
  EXPECT_DOUBLE_EQ(s1_term.distance(), 1.0);
  ExpectNearestPointsNear(s1_term.nearestPoints().get(), 0.0, 0.0, 0.0, 1.0);

  // Stage 2 (lines0-pts1) early exit in computeFacetDistance().
  auto gc_s2_a =
      ReadWkt("GEOMETRYCOLLECTION (LINESTRING (0 0, 10 0), POINT (100 100))");
  auto gc_s2_b = ReadWkt(
      "GEOMETRYCOLLECTION (LINESTRING (50 50, 51 50), POINT (5 1), "
      "POINT (100 100))");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*gc_s2_a, *gc_s2_b), 0.0);
  DistanceOp s2_term(*gc_s2_a, *gc_s2_b, 1.5);
  EXPECT_DOUBLE_EQ(s2_term.distance(), 1.0);
  ExpectNearestPointsNear(s2_term.nearestPoints().get(), 5.0, 0.0, 5.0, 1.0);

  // Stage 3 (lines1-pts0) early exit with flip=true in computeFacetDistance().
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*gc_s2_b, *gc_s2_a), 0.0);
  DistanceOp s3_term(*gc_s2_b, *gc_s2_a, 1.5);
  EXPECT_DOUBLE_EQ(s3_term.distance(), 1.0);
  ExpectNearestPointsNear(s3_term.nearestPoints().get(), 5.0, 1.0, 5.0, 0.0);

  // Stage 3 wins with terminateDistance = 0.0 while stage 4 (pts0-pts1) finds
  // nothing closer (updateMinDistance ignores unset locGeom).
  auto gc_s3_win_a =
      ReadWkt("GEOMETRYCOLLECTION (LINESTRING (50 50, 51 50), POINT (5 2))");
  auto gc_s3_win_b =
      ReadWkt("GEOMETRYCOLLECTION (LINESTRING (0 0, 10 0), POINT (80 80))");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*gc_s3_win_a, *gc_s3_win_b), 2.0);
  ExpectNearestPointsNear(
      DistanceOp::nearestPoints(gc_s3_win_a.get(), gc_s3_win_b.get()).get(),
      5.0, 2.0, 5.0, 0.0);

  // Stage 4 (pts0-pts1) wins when lines are farther than points.
  auto gc_s4_a =
      ReadWkt("GEOMETRYCOLLECTION (LINESTRING (0 0, 1 0), POINT (10 0))");
  auto gc_s4_b =
      ReadWkt("GEOMETRYCOLLECTION (LINESTRING (0 20, 1 20), POINT (10 2))");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*gc_s4_a, *gc_s4_b), 2.0);
  ExpectNearestPointsNear(
      DistanceOp::nearestPoints(gc_s4_a.get(), gc_s4_b.get()).get(), 10.0, 0.0,
      10.0, 2.0);

  // Stage 1 wins with terminateDistance = 0.0 while later stages find nothing
  // closer.
  auto gc_lines_win_a =
      ReadWkt("GEOMETRYCOLLECTION (LINESTRING (0 0, 10 0), POINT (50 50))");
  auto gc_lines_win_b =
      ReadWkt("GEOMETRYCOLLECTION (LINESTRING (0 2, 10 2), POINT (60 60))");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*gc_lines_win_a, *gc_lines_win_b), 2.0);

  // Nested GeometryCollection with mixed dimensions.
  auto nested_a = ReadWkt(
      "GEOMETRYCOLLECTION (GEOMETRYCOLLECTION (POINT (0 0)), "
      "LINESTRING (10 0, 10 5))");
  auto nested_b =
      ReadWkt("GEOMETRYCOLLECTION (POLYGON ((3 -1, 5 -1, 5 1, 3 1, 3 -1)))");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*nested_a, *nested_b), 3.0);
  ExpectNearestPointsNear(
      DistanceOp::nearestPoints(nested_a.get(), nested_b.get()).get(), 0.0, 0.0,
      3.0, 0.0);
}

TEST_F(DistanceOpTest, IsWithinDistanceCoversAllBranches) {
  auto p0 = ReadWkt("POINT (0 0)");
  auto p1 = ReadWkt("POINT (3 4)");
  auto l0 = ReadWkt("LINESTRING (0 0, 10 0)");
  auto l1 = ReadWkt("LINESTRING (0 3, 10 3)");

  // Negative distance fails envelope distance check (0.0 > -1.0).
  EXPECT_FALSE(DistanceOp::isWithinDistance(*p0, *p0, -1.0));

  // Disjoint envelopes fail envelope distance check.
  EXPECT_FALSE(DistanceOp::isWithinDistance(*p0, *p1, 2.0));
  EXPECT_FALSE(DistanceOp::isWithinDistance(*l0, *l1, 2.999999));

  // Boundary equality and larger threshold pass.
  EXPECT_TRUE(DistanceOp::isWithinDistance(*p0, *p1, 5.0));
  EXPECT_TRUE(DistanceOp::isWithinDistance(*p0, *p1, 5.1));
  EXPECT_TRUE(DistanceOp::isWithinDistance(*l0, *l1, 3.0));

  // Overlapping envelopes (envDist == 0.0 <= distance), but actual distance
  // exceeds threshold: falls past envelope check into DistanceOp.
  auto diag_a = ReadWkt("LINESTRING (0 0, 10 10)");
  auto diag_b = ReadWkt("LINESTRING (0 10, 2 8)");
  EXPECT_FALSE(DistanceOp::isWithinDistance(*diag_a, *diag_b, 1.0));
  EXPECT_TRUE(DistanceOp::isWithinDistance(*diag_a, *diag_b, 6.0));

  // Containment satisfies zero threshold.
  auto poly = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto inside = ReadWkt("POINT (5 5)");
  EXPECT_TRUE(DistanceOp::isWithinDistance(*poly, *inside, 0.0));
}

TEST_F(DistanceOpTest, EdgeCasesEmptyComponentsZeroLengthSegmentsAndIslands) {
  // Empty components mixed with non-empty components in a GeometryCollection.
  auto gc_mixed_empties = ReadWkt(
      "GEOMETRYCOLLECTION (POLYGON EMPTY, LINESTRING EMPTY, POINT EMPTY, "
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0)))");
  auto pt_in = ReadWkt("POINT (2 2)");
  auto pt_out = ReadWkt("POINT (6 2)");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*gc_mixed_empties, *pt_in), 0.0);
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*pt_in, *gc_mixed_empties), 0.0);
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*gc_mixed_empties, *pt_out), 2.0);
  ExpectNearestPointsNear(
      DistanceOp::nearestPoints(gc_mixed_empties.get(), pt_out.get()).get(),
      4.0, 2.0, 6.0, 2.0);
  ExpectNearestPointsNear(
      DistanceOp::nearestPoints(pt_out.get(), gc_mixed_empties.get()).get(),
      6.0, 2.0, 4.0, 2.0);

  auto gc_line_empty =
      ReadWkt("GEOMETRYCOLLECTION (LINESTRING EMPTY, LINESTRING (0 0, 10 0))");
  auto line_other = ReadWkt("LINESTRING (0 3, 10 3)");
  auto pt_other = ReadWkt("POINT (5 3)");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*gc_line_empty, *line_other), 3.0);
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*gc_line_empty, *pt_other), 3.0);

  // MultiPolygon with an island inside a hole.
  auto mpoly_island = ReadWkt(
      "MULTIPOLYGON (((0 0, 20 0, 20 20, 0 20, 0 0), "
      "(4 4, 16 4, 16 16, 4 16, 4 4)), "
      "((8 8, 12 8, 12 12, 8 12, 8 8)))");
  auto pt_island = ReadWkt("POINT (10 10)");
  auto pt_moat = ReadWkt("POINT (6 10)");
  auto pt_shell = ReadWkt("POINT (2 10)");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*mpoly_island, *pt_island), 0.0);
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*pt_island, *mpoly_island), 0.0);
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*mpoly_island, *pt_shell), 0.0);
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*mpoly_island, *pt_moat), 2.0);
  auto moat_pts = DistanceOp::nearestPoints(mpoly_island.get(), pt_moat.get());
  ASSERT_NE(moat_pts, nullptr);
  EXPECT_DOUBLE_EQ(moat_pts->getAt(0).distance(moat_pts->getAt(1)), 2.0);

  // Degenerate zero-length LineStrings.
  auto deg_line0 = ReadWkt("LINESTRING (1 1, 1 1)");
  auto deg_line1 = ReadWkt("LINESTRING (4 5, 4 5)");
  auto pt_far = ReadWkt("POINT (4 5)");
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*deg_line0, *pt_far), 5.0);
  ExpectNearestPointsNear(
      DistanceOp::nearestPoints(deg_line0.get(), pt_far.get()).get(), 1.0, 1.0,
      4.0, 5.0);
  EXPECT_DOUBLE_EQ(DistanceOp::distance(*deg_line0, *deg_line1), 5.0);
  ExpectNearestPointsNear(
      DistanceOp::nearestPoints(deg_line0.get(), deg_line1.get()).get(), 1.0,
      1.0, 4.0, 5.0);
}

}  // namespace
}  // namespace geos_google
