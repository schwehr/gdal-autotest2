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

#include "third_party/geos/include/geos/simplify/RingHull.h"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/Area.h"
#include "third_party/geos/include/geos/algorithm/Orientation.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/simplify/LinkedRing.h"
#include "third_party/geos/include/geos/simplify/RingHullIndex.h"

namespace geos_google {
namespace {

using geos::algorithm::Area;
using geos::algorithm::Orientation;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Envelope;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::simplify::LinkedRing;
using geos::simplify::RingHull;
using geos::simplify::RingHullIndex;

CoordinateSequence MakeSeq(std::initializer_list<CoordinateXY> coords) {
  CoordinateSequence seq;
  seq.reserve(coords.size());
  for (const auto& c : coords) {
    seq.add(c);
  }
  return seq;
}

std::unique_ptr<LinearRing> MakeRing(
    const GeometryFactory& factory,
    std::initializer_list<CoordinateXY> coords) {
  auto seq = std::make_unique<CoordinateSequence>();
  seq->reserve(coords.size());
  for (const auto& c : coords) {
    seq->add(c);
  }
  return factory.createLinearRing(std::move(seq));
}

std::unique_ptr<LinearRing> MakeRing3D(
    const GeometryFactory& factory, std::initializer_list<Coordinate> coords) {
  auto seq =
      std::make_unique<CoordinateSequence>(0, /*hasz=*/true, /*hasm=*/false);
  seq->reserve(coords.size());
  for (const auto& c : coords) {
    seq->add(c);
  }
  return factory.createLinearRing(std::move(seq));
}

std::unique_ptr<Polygon> RingToPolygon(const GeometryFactory& factory,
                                       const LinearRing& ring) {
  return factory.createPolygon(ring.clone());
}

TEST(RingHullTest, StaticIsConvexAndAreaOnLinkedRing) {
  CoordinateSequence seq = MakeSeq({
      {0.0, 0.0},
      {0.0, 10.0},
      {4.0, 10.0},
      {5.0, 6.0},
      {6.0, 10.0},
      {10.0, 10.0},
      {10.0, 0.0},
      {5.0, 0.0},
      {0.0, 0.0},
  });
  LinkedRing linked(seq);

  // Convex CW corners.
  EXPECT_TRUE(RingHull::isConvex(linked, 0));
  EXPECT_TRUE(RingHull::isConvex(linked, 1));
  EXPECT_TRUE(RingHull::isConvex(linked, 2));
  EXPECT_DOUBLE_EQ(RingHull::area(linked, 1), 20.0);
  EXPECT_DOUBLE_EQ(RingHull::area(linked, 2), 8.0);

  // Concave notch vertex at (5, 6): triangle (4,10)-(5,6)-(6,10) has area 4.
  EXPECT_FALSE(RingHull::isConvex(linked, 3));
  EXPECT_DOUBLE_EQ(RingHull::area(linked, 3), 4.0);

  // Collinear vertex at (5, 0) along bottom edge is non-convex with zero area.
  EXPECT_FALSE(RingHull::isConvex(linked, 7));
  EXPECT_DOUBLE_EQ(RingHull::area(linked, 7), 0.0);
}

TEST(RingHullTest, GetEnvelopeMatchesInputRing) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LinearRing> ring = MakeRing(
      *factory,
      {{-3.5, -1.0}, {-3.5, 8.75}, {12.25, 8.75}, {12.25, -1.0}, {-3.5, -1.0}});
  RingHull hull(ring.get(), true);

  const Envelope* env = hull.getEnvelope();
  ASSERT_NE(env, nullptr);
  EXPECT_DOUBLE_EQ(env->getMinX(), -3.5);
  EXPECT_DOUBLE_EQ(env->getMaxX(), 12.25);
  EXPECT_DOUBLE_EQ(env->getMinY(), -1.0);
  EXPECT_DOUBLE_EQ(env->getMaxY(), 8.75);
}

TEST(RingHullTest, NoTargetSetLeavesRingUnchanged) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LinearRing> ring = MakeRing(*factory, {{0.0, 0.0},
                                                         {0.0, 10.0},
                                                         {4.0, 10.0},
                                                         {5.0, 6.0},
                                                         {6.0, 10.0},
                                                         {10.0, 10.0},
                                                         {10.0, 0.0},
                                                         {0.0, 0.0}});
  RingHull hull(ring.get(), true);

  std::unique_ptr<Polygon> before = hull.toGeometry();
  ASSERT_NE(before, nullptr);
  EXPECT_TRUE(before->isValid());
  EXPECT_DOUBLE_EQ(before->getArea(), 96.0);

  RingHullIndex empty_index;
  std::unique_ptr<LinearRing> result = hull.getHull(empty_index);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getNumPoints(), 8u);
  EXPECT_DOUBLE_EQ(Area::ofRing(result->getCoordinatesRO()), 96.0);

  std::unique_ptr<Polygon> after = hull.toGeometry();
  ASSERT_NE(after, nullptr);
  EXPECT_DOUBLE_EQ(after->getArea(), 96.0);
}

TEST(RingHullTest, InitReversesOrientationAccordingToIsOuter) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // L-shaped ring (area 64) provided in both CCW and CW orientations.
  std::unique_ptr<LinearRing> ccw_ring = MakeRing(*factory, {{0.0, 0.0},
                                                             {10.0, 0.0},
                                                             {10.0, 4.0},
                                                             {4.0, 4.0},
                                                             {4.0, 10.0},
                                                             {0.0, 10.0},
                                                             {0.0, 0.0}});
  std::unique_ptr<LinearRing> cw_ring = MakeRing(*factory, {{0.0, 0.0},
                                                            {0.0, 10.0},
                                                            {4.0, 10.0},
                                                            {4.0, 4.0},
                                                            {10.0, 4.0},
                                                            {10.0, 0.0},
                                                            {0.0, 0.0}});
  std::unique_ptr<Polygon> orig_poly = RingToPolygon(*factory, *ccw_ring);
  RingHullIndex index;

  // Outer hulls orient CW and expand over the concave corner (4, 4).
  for (const LinearRing* input : {ccw_ring.get(), cw_ring.get()}) {
    RingHull outer_hull(input, /*p_isOuter=*/true);
    outer_hull.setMinVertexNum(0);
    std::unique_ptr<LinearRing> result = outer_hull.getHull(index);
    ASSERT_NE(result, nullptr);
    EXPECT_FALSE(Orientation::isCCW(result->getCoordinatesRO()));
    EXPECT_EQ(result->getNumPoints(), 6u);
    EXPECT_DOUBLE_EQ(Area::ofRing(result->getCoordinatesRO()), 82.0);

    std::unique_ptr<Polygon> hull_poly = RingToPolygon(*factory, *result);
    EXPECT_TRUE(hull_poly->isValid());
    EXPECT_TRUE(hull_poly->covers(orig_poly.get()));
  }

  // Inner hulls orient CCW and shrink down to a 3-vertex triangle.
  for (const LinearRing* input : {ccw_ring.get(), cw_ring.get()}) {
    RingHull inner_hull(input, /*p_isOuter=*/false);
    inner_hull.setMinVertexNum(0);
    std::unique_ptr<LinearRing> result = inner_hull.getHull(index);
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(Orientation::isCCW(result->getCoordinatesRO()));
    EXPECT_EQ(result->getNumPoints(), 4u);

    std::unique_ptr<Polygon> hull_poly = RingToPolygon(*factory, *result);
    EXPECT_TRUE(hull_poly->isValid());
    EXPECT_TRUE(orig_poly->covers(hull_poly.get()));
  }
}

TEST(RingHullTest, TriangleRingIsNeverSimplifiedFurther) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LinearRing> tri =
      MakeRing(*factory, {{0.0, 0.0}, {0.0, 10.0}, {10.0, 0.0}, {0.0, 0.0}});
  RingHullIndex index;

  for (bool is_outer : {true, false}) {
    RingHull hull(tri.get(), is_outer);
    hull.setMinVertexNum(0);
    std::unique_ptr<LinearRing> result = hull.getHull(index);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getNumPoints(), 4u);
    EXPECT_DOUBLE_EQ(Area::ofRing(result->getCoordinatesRO()), 50.0);
  }
}

TEST(RingHullTest, SetMinVertexNumStopsAtTarget) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // 7 distinct vertices (8 points), notches at (2, 9) [area 2] and (7, 7)
  // [area 9]. Removing both notches makes (4, 10) collinear [area 0].
  std::unique_ptr<LinearRing> ring = MakeRing(*factory, {{0.0, 0.0},
                                                         {0.0, 10.0},
                                                         {2.0, 9.0},
                                                         {4.0, 10.0},
                                                         {7.0, 7.0},
                                                         {10.0, 10.0},
                                                         {10.0, 0.0},
                                                         {0.0, 0.0}});
  RingHullIndex index;

  auto run_with_min = [&](std::size_t min_vertices) {
    RingHull hull(ring.get(), true);
    hull.setMinVertexNum(min_vertices);
    return hull.getHull(index);
  };

  std::unique_ptr<LinearRing> r8 = run_with_min(8);
  EXPECT_EQ(r8->getNumPoints(), 8u);
  EXPECT_DOUBLE_EQ(Area::ofRing(r8->getCoordinatesRO()), 89.0);

  std::unique_ptr<LinearRing> r7 = run_with_min(7);
  EXPECT_EQ(r7->getNumPoints(), 7u);
  EXPECT_DOUBLE_EQ(Area::ofRing(r7->getCoordinatesRO()), 91.0);

  std::unique_ptr<LinearRing> r6 = run_with_min(6);
  EXPECT_EQ(r6->getNumPoints(), 6u);
  EXPECT_DOUBLE_EQ(Area::ofRing(r6->getCoordinatesRO()), 100.0);

  std::unique_ptr<LinearRing> r5 = run_with_min(5);
  EXPECT_EQ(r5->getNumPoints(), 5u);
  EXPECT_DOUBLE_EQ(Area::ofRing(r5->getCoordinatesRO()), 100.0);

  std::unique_ptr<LinearRing> r0 = run_with_min(0);
  EXPECT_EQ(r0->getNumPoints(), 5u);
  EXPECT_DOUBLE_EQ(Area::ofRing(r0->getCoordinatesRO()), 100.0);
}

TEST(RingHullTest, SetMaxAreaDeltaPreventsOvershootAndRemovesCollinearAtZero) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Collinear vertex (5, 10) [area 0], bottom notch (5, 1) [area 1],
  // right notch (8, 5) [area 4]. Original area = 95.
  std::unique_ptr<LinearRing> ring = MakeRing(*factory, {{0.0, 0.0},
                                                         {0.0, 10.0},
                                                         {5.0, 10.0},
                                                         {10.0, 10.0},
                                                         {10.0, 7.0},
                                                         {8.0, 5.0},
                                                         {10.0, 3.0},
                                                         {10.0, 0.0},
                                                         {6.0, 0.0},
                                                         {5.0, 1.0},
                                                         {4.0, 0.0},
                                                         {0.0, 0.0}});
  RingHullIndex index;

  // Max area delta 0 removes only the collinear vertex (5, 10).
  {
    RingHull hull(ring.get(), true);
    hull.setMaxAreaDelta(0.0);
    std::unique_ptr<LinearRing> result = hull.getHull(index);
    EXPECT_EQ(result->getNumPoints(), 11u);
    EXPECT_DOUBLE_EQ(Area::ofRing(result->getCoordinatesRO()), 95.0);
  }

  // Max area delta 1.5 removes (5, 10), (5, 1), and its newly-collinear
  // neighbors (6, 0) and (4, 0), stopping before (8, 5) [area 4].
  {
    RingHull hull(ring.get(), true);
    hull.setMaxAreaDelta(1.5);
    std::unique_ptr<LinearRing> result = hull.getHull(index);
    EXPECT_EQ(result->getNumPoints(), 8u);
    EXPECT_DOUBLE_EQ(Area::ofRing(result->getCoordinatesRO()), 96.0);
  }

  // Exact boundary 5.0 removes both notches and leaves the [0,10]x[0,10] box.
  {
    RingHull hull(ring.get(), true);
    hull.setMaxAreaDelta(5.0);
    std::unique_ptr<LinearRing> result = hull.getHull(index);
    EXPECT_EQ(result->getNumPoints(), 5u);
    EXPECT_DOUBLE_EQ(Area::ofRing(result->getCoordinatesRO()), 100.0);
  }
}

TEST(RingHullTest, MultiVertexConcavitySkipsStaleQueuedCorner) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Adjacent concave vertices (3, 8) [area 2.5] and (7, 7) [initial area 7.5].
  // Removing (3, 8) invalidates the initial corner for (7, 7) and re-queues
  // (7, 7) with updated area 15.0.
  std::unique_ptr<LinearRing> ring = MakeRing(*factory, {{0.0, 0.0},
                                                         {0.0, 10.0},
                                                         {3.0, 8.0},
                                                         {7.0, 7.0},
                                                         {10.0, 10.0},
                                                         {10.0, 0.0},
                                                         {0.0, 0.0}});
  RingHullIndex index;

  // With maxAreaDelta = 10.0, stale corner (7, 7) [area 7.5] is skipped before
  // isAtTarget, and updated corner (7, 7) [area 15.0] stops before overshoot.
  {
    RingHull hull(ring.get(), true);
    hull.setMaxAreaDelta(10.0);
    std::unique_ptr<LinearRing> result = hull.getHull(index);
    EXPECT_EQ(result->getNumPoints(), 6u);
    EXPECT_DOUBLE_EQ(Area::ofRing(result->getCoordinatesRO()), 85.0);
  }

  // Full simplification removes both concave vertices.
  {
    RingHull hull(ring.get(), true);
    hull.setMinVertexNum(0);
    std::unique_ptr<LinearRing> result = hull.getHull(index);
    EXPECT_EQ(result->getNumPoints(), 5u);
    EXPECT_DOUBLE_EQ(Area::ofRing(result->getCoordinatesRO()), 100.0);
  }
}

TEST(RingHullTest, CollinearChainAndEqualAreaTieBreak) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Three collinear vertices along y=10 plus two symmetric area-4 notches.
  std::unique_ptr<LinearRing> ring = MakeRing(*factory, {{0.0, 0.0},
                                                         {0.0, 3.0},
                                                         {2.0, 5.0},
                                                         {0.0, 7.0},
                                                         {0.0, 10.0},
                                                         {2.0, 10.0},
                                                         {4.0, 10.0},
                                                         {6.0, 10.0},
                                                         {10.0, 10.0},
                                                         {10.0, 7.0},
                                                         {8.0, 5.0},
                                                         {10.0, 3.0},
                                                         {10.0, 0.0},
                                                         {0.0, 0.0}});
  RingHullIndex index;

  // Area delta 4.0 removes all collinear points and exactly one area-4 notch.
  {
    RingHull hull(ring.get(), true);
    hull.setMaxAreaDelta(4.0);
    std::unique_ptr<LinearRing> result = hull.getHull(index);
    EXPECT_DOUBLE_EQ(Area::ofRing(result->getCoordinatesRO()), 96.0);
    std::unique_ptr<Polygon> poly = RingToPolygon(*factory, *result);
    EXPECT_TRUE(poly->isValid());
  }

  // Area delta 8.0 removes both notches and reduces to the outer square.
  {
    RingHull hull(ring.get(), true);
    hull.setMaxAreaDelta(8.0);
    std::unique_ptr<LinearRing> result = hull.getHull(index);
    EXPECT_EQ(result->getNumPoints(), 5u);
    EXPECT_DOUBLE_EQ(Area::ofRing(result->getCoordinatesRO()), 100.0);
  }
}

TEST(RingHullTest, SelfIntersectingCornerIsNotRemoved) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Top notch (3, 10)-(5, 7)-(7, 10) [area 6] surrounds arm tip (4.8, 8)-
  // (5.2, 8) from the same ring, while all other concave corners have area
  // >= 32.
  std::unique_ptr<LinearRing> ring = MakeRing(*factory, {{0.0, 0.0},
                                                         {0.0, 10.0},
                                                         {3.0, 10.0},
                                                         {5.0, 7.0},
                                                         {7.0, 10.0},
                                                         {15.0, 10.0},
                                                         {15.0, 18.0},
                                                         {5.2, 18.0},
                                                         {5.2, 8.0},
                                                         {4.8, 8.0},
                                                         {4.8, 25.0},
                                                         {20.0, 25.0},
                                                         {20.0, 0.0},
                                                         {0.0, 0.0}});
  std::unique_ptr<Polygon> orig_poly = RingToPolygon(*factory, *ring);
  ASSERT_TRUE(orig_poly->isValid());

  RingHull hull(ring.get(), true);
  hull.setMaxAreaDelta(10.0);
  RingHullIndex empty_index;
  std::unique_ptr<LinearRing> result = hull.getHull(empty_index);

  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getNumPoints(), 14u);
  std::unique_ptr<Polygon> hull_poly = RingToPolygon(*factory, *result);
  EXPECT_TRUE(hull_poly->isValid());
  EXPECT_TRUE(hull_poly->covers(orig_poly.get()));
}

TEST(RingHullTest, OtherHullInIndexBlocksOrAllowsCornerRemoval) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LinearRing> notched_ring = MakeRing(*factory, {{0.0, 0.0},
                                                                 {0.0, 10.0},
                                                                 {4.0, 10.0},
                                                                 {5.0, 6.0},
                                                                 {6.0, 10.0},
                                                                 {10.0, 10.0},
                                                                 {10.0, 0.0},
                                                                 {0.0, 0.0}});
  // Envelope [4, 4.2] x [6, 7] overlaps cornerEnv [4, 6] x [6, 10], but all
  // vertices lie strictly outside triangle (4, 10)-(5, 6)-(6, 10).
  std::unique_ptr<LinearRing> env_only_ring =
      MakeRing(*factory, {{4.0, 6.0}, {4.0, 7.0}, {4.2, 6.0}, {4.0, 6.0}});
  // Far-away ring whose envelope is disjoint from cornerEnv.
  std::unique_ptr<LinearRing> far_ring = MakeRing(
      *factory,
      {{50.0, 50.0}, {50.0, 60.0}, {60.0, 60.0}, {60.0, 50.0}, {50.0, 50.0}});
  // Ring lying inside triangle (4, 10)-(5, 6)-(6, 10).
  std::unique_ptr<LinearRing> blocker_ring =
      MakeRing(*factory, {{4.8, 8.0}, {5.0, 9.0}, {5.2, 8.0}, {4.8, 8.0}});

  // Non-intersecting other hulls (plus `this` in the index) allow removal.
  {
    RingHull hull(notched_ring.get(), true);
    hull.setMinVertexNum(0);
    RingHull env_only_hull(env_only_ring.get(), true);
    RingHull far_hull(far_ring.get(), true);

    RingHullIndex index;
    index.add(&hull);
    index.add(&env_only_hull);
    index.add(&far_hull);

    std::unique_ptr<LinearRing> result = hull.getHull(index);
    EXPECT_EQ(result->getNumPoints(), 5u);
    EXPECT_DOUBLE_EQ(Area::ofRing(result->getCoordinatesRO()), 100.0);
  }

  // An intersecting vertex from another hull in the index blocks removal.
  {
    RingHull hull(notched_ring.get(), true);
    hull.setMinVertexNum(0);
    RingHull blocker_hull(blocker_ring.get(), true);

    RingHullIndex index;
    index.add(&hull);
    index.add(&blocker_hull);

    std::unique_ptr<LinearRing> result = hull.getHull(index);
    EXPECT_EQ(result->getNumPoints(), 8u);
    EXPECT_DOUBLE_EQ(Area::ofRing(result->getCoordinatesRO()), 96.0);
  }
}

TEST(RingHullTest, InnerHullRespectsHoleOuterHull) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LinearRing> shell_ring = MakeRing(
      *factory,
      {{0.0, 0.0}, {0.0, 20.0}, {20.0, 20.0}, {20.0, 0.0}, {0.0, 0.0}});
  std::unique_ptr<LinearRing> hole_ring = MakeRing(
      *factory,
      {{5.0, 5.0}, {5.0, 15.0}, {15.0, 15.0}, {15.0, 5.0}, {5.0, 5.0}});
  std::unique_ptr<Polygon> shell_poly = RingToPolygon(*factory, *shell_ring);
  std::unique_ptr<Polygon> hole_poly = RingToPolygon(*factory, *hole_ring);

  // Without the hole in the index, the square inner hull collapses to a
  // triangle.
  {
    RingHull shell_hull(shell_ring.get(), /*p_isOuter=*/false);
    shell_hull.setMinVertexNum(0);
    RingHullIndex empty_index;
    std::unique_ptr<LinearRing> result = shell_hull.getHull(empty_index);
    EXPECT_EQ(result->getNumPoints(), 4u);
    EXPECT_DOUBLE_EQ(Area::ofRing(result->getCoordinatesRO()), 200.0);
  }

  // With the hole in the index, no shell corner can be removed without
  // intersecting the hole.
  {
    RingHull shell_hull(shell_ring.get(), /*p_isOuter=*/false);
    shell_hull.setMinVertexNum(0);
    RingHull hole_hull(hole_ring.get(), /*p_isOuter=*/true);

    RingHullIndex index;
    index.add(&shell_hull);
    index.add(&hole_hull);

    std::unique_ptr<LinearRing> result = shell_hull.getHull(index);
    EXPECT_EQ(result->getNumPoints(), 5u);
    EXPECT_DOUBLE_EQ(Area::ofRing(result->getCoordinatesRO()), 400.0);

    std::unique_ptr<Polygon> result_poly = RingToPolygon(*factory, *result);
    EXPECT_TRUE(result_poly->isValid());
    EXPECT_TRUE(shell_poly->covers(result_poly.get()));
    EXPECT_TRUE(result_poly->covers(hole_poly.get()));
  }
}

TEST(RingHullTest, ComputeAndToGeometryAreIdempotent) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LinearRing> ring = MakeRing(*factory, {{0.0, 0.0},
                                                         {0.0, 10.0},
                                                         {4.0, 10.0},
                                                         {5.0, 6.0},
                                                         {6.0, 10.0},
                                                         {10.0, 10.0},
                                                         {10.0, 0.0},
                                                         {0.0, 0.0}});
  RingHull hull(ring.get(), true);
  hull.setMinVertexNum(0);
  RingHullIndex index;

  hull.compute(index);
  std::unique_ptr<Polygon> poly1 = hull.toGeometry();
  ASSERT_NE(poly1, nullptr);
  EXPECT_TRUE(poly1->isValid());
  EXPECT_DOUBLE_EQ(poly1->getArea(), 100.0);
  EXPECT_EQ(poly1->getNumPoints(), 5u);

  // Calling compute and getHull again leaves the computed hull unchanged.
  hull.compute(index);
  std::unique_ptr<LinearRing> ring2 = hull.getHull(index);
  ASSERT_NE(ring2, nullptr);
  EXPECT_EQ(ring2->getNumPoints(), 5u);
  EXPECT_DOUBLE_EQ(Area::ofRing(ring2->getCoordinatesRO()), 100.0);
}

TEST(RingHullTest, ConvexPentagonStopsWithEmptyQueueEvenAboveThreeVertices) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LinearRing> pentagon = MakeRing(*factory, {{0.0, 0.0},
                                                             {0.0, 10.0},
                                                             {5.0, 14.0},
                                                             {10.0, 10.0},
                                                             {10.0, 0.0},
                                                             {0.0, 0.0}});
  std::unique_ptr<Polygon> orig_poly = RingToPolygon(*factory, *pentagon);
  RingHullIndex index;

  // Outer hull has no concave corners, so compute exits immediately via empty
  // cornerQueue even while vertexRing->size() > 3.
  {
    RingHull hull(pentagon.get(), /*p_isOuter=*/true);
    hull.setMinVertexNum(0);
    std::unique_ptr<LinearRing> result = hull.getHull(index);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getNumPoints(), 6u);
    EXPECT_DOUBLE_EQ(Area::ofRing(result->getCoordinatesRO()), 120.0);
  }
  {
    RingHull hull(pentagon.get(), /*p_isOuter=*/true);
    hull.setMaxAreaDelta(1000.0);
    std::unique_ptr<LinearRing> result = hull.getHull(index);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getNumPoints(), 6u);
    EXPECT_DOUBLE_EQ(Area::ofRing(result->getCoordinatesRO()), 120.0);
  }

  // Inner hull reverses to CCW so all vertices are removable to a triangle.
  {
    RingHull hull(pentagon.get(), /*p_isOuter=*/false);
    hull.setMinVertexNum(0);
    std::unique_ptr<LinearRing> result = hull.getHull(index);
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(Orientation::isCCW(result->getCoordinatesRO()));
    EXPECT_EQ(result->getNumPoints(), 4u);
    EXPECT_DOUBLE_EQ(Area::ofRing(result->getCoordinatesRO()), 50.0);

    std::unique_ptr<Polygon> hull_poly = RingToPolygon(*factory, *result);
    EXPECT_TRUE(hull_poly->isValid());
    EXPECT_TRUE(orig_poly->covers(hull_poly.get()));
  }
}

TEST(RingHullTest, ConcaveStartVertexAtIndexZeroIsRemovedCleanly) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Ring starts and closes directly on the concave notch vertex (5, 4).
  // Removing index 0 relies on init() removing the duplicate closing vertex
  // from vertexIndex and updates wraparound links cleanly.
  std::unique_ptr<LinearRing> cw_ring = MakeRing(*factory, {{5.0, 4.0},
                                                            {8.0, 8.0},
                                                            {10.0, 0.0},
                                                            {0.0, 0.0},
                                                            {2.0, 8.0},
                                                            {5.0, 4.0}});
  std::unique_ptr<LinearRing> ccw_ring = MakeRing(*factory, {{5.0, 4.0},
                                                             {2.0, 8.0},
                                                             {0.0, 0.0},
                                                             {10.0, 0.0},
                                                             {8.0, 8.0},
                                                             {5.0, 4.0}});
  std::unique_ptr<Polygon> orig_poly = RingToPolygon(*factory, *cw_ring);
  ASSERT_TRUE(orig_poly->isValid());
  EXPECT_DOUBLE_EQ(orig_poly->getArea(), 52.0);
  RingHullIndex index;

  for (const LinearRing* input : {cw_ring.get(), ccw_ring.get()}) {
    RingHull hull(input, /*p_isOuter=*/true);
    hull.setMinVertexNum(0);
    std::unique_ptr<LinearRing> result = hull.getHull(index);
    ASSERT_NE(result, nullptr);
    EXPECT_FALSE(Orientation::isCCW(result->getCoordinatesRO()));
    EXPECT_EQ(result->getNumPoints(), 5u);
    EXPECT_DOUBLE_EQ(Area::ofRing(result->getCoordinatesRO()), 64.0);

    std::unique_ptr<Polygon> hull_poly = RingToPolygon(*factory, *result);
    EXPECT_TRUE(hull_poly->isValid());
    EXPECT_TRUE(hull_poly->covers(orig_poly.get()));
  }

  // Notch triangle (2, 8)-(5, 4)-(8, 8) has area 12.0.
  {
    RingHull hull(cw_ring.get(), /*p_isOuter=*/true);
    hull.setMaxAreaDelta(11.5);
    std::unique_ptr<LinearRing> result = hull.getHull(index);
    EXPECT_EQ(result->getNumPoints(), 6u);
    EXPECT_DOUBLE_EQ(Area::ofRing(result->getCoordinatesRO()), 52.0);
  }
  {
    RingHull hull(cw_ring.get(), /*p_isOuter=*/true);
    hull.setMaxAreaDelta(12.0);
    std::unique_ptr<LinearRing> result = hull.getHull(index);
    EXPECT_EQ(result->getNumPoints(), 5u);
    EXPECT_DOUBLE_EQ(Area::ofRing(result->getCoordinatesRO()), 64.0);
  }
}

TEST(RingHullTest, NextNeighborRemovalInvalidatesStaleQueuedCorner) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Adjacent concave vertices (3, 6) [area 11] and (7, 8) [area 1].
  // Removing (7, 8) changes only the `next` link of (3, 6) while keeping its
  // `prev` link unchanged, invalidating the stale area-11 corner and
  // re-queueing (3, 6) with area 20.
  std::unique_ptr<LinearRing> ring = MakeRing(*factory, {{0.0, 0.0},
                                                         {0.0, 10.0},
                                                         {3.0, 6.0},
                                                         {7.0, 8.0},
                                                         {10.0, 10.0},
                                                         {10.0, 0.0},
                                                         {0.0, 0.0}});
  RingHullIndex index;

  {
    RingHull hull(ring.get(), /*p_isOuter=*/true);
    hull.setMaxAreaDelta(15.0);
    std::unique_ptr<LinearRing> result = hull.getHull(index);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getNumPoints(), 6u);
    EXPECT_DOUBLE_EQ(Area::ofRing(result->getCoordinatesRO()), 80.0);
  }
  {
    RingHull hull(ring.get(), /*p_isOuter=*/true);
    hull.setMaxAreaDelta(21.0);
    std::unique_ptr<LinearRing> result = hull.getHull(index);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getNumPoints(), 5u);
    EXPECT_DOUBLE_EQ(Area::ofRing(result->getCoordinatesRO()), 100.0);
  }
}

TEST(RingHullTest, EqualAreaCornersTieBreakByLowerVertexIndex) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Left notch (2, 5) at index 2 and right notch (8, 5) at index 7 both have
  // area 4.0. With maxAreaDelta = 4.0, tie-breaking removes index 2 first (plus
  // its newly collinear neighbors at indices 1 and 3), leaving (8, 5).
  std::unique_ptr<LinearRing> ring = MakeRing(*factory, {{0.0, 0.0},
                                                         {0.0, 3.0},
                                                         {2.0, 5.0},
                                                         {0.0, 7.0},
                                                         {0.0, 10.0},
                                                         {10.0, 10.0},
                                                         {10.0, 7.0},
                                                         {8.0, 5.0},
                                                         {10.0, 3.0},
                                                         {10.0, 0.0},
                                                         {0.0, 0.0}});
  RingHull hull(ring.get(), /*p_isOuter=*/true);
  hull.setMaxAreaDelta(4.0);
  RingHullIndex index;
  std::unique_ptr<LinearRing> result = hull.getHull(index);

  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getNumPoints(), 8u);
  EXPECT_DOUBLE_EQ(Area::ofRing(result->getCoordinatesRO()), 96.0);

  std::unique_ptr<Polygon> hull_poly = RingToPolygon(*factory, *result);
  EXPECT_TRUE(hull_poly->isValid());
  std::unique_ptr<Point> left_notch_pt =
      factory->createPoint(CoordinateXY(1.0, 5.0));
  std::unique_ptr<Point> right_notch_pt =
      factory->createPoint(CoordinateXY(9.0, 5.0));
  EXPECT_TRUE(hull_poly->covers(left_notch_pt.get()));
  EXPECT_FALSE(hull_poly->covers(right_notch_pt.get()));
}

TEST(RingHullTest, MinVertexNumTakesPrecedenceOverMaxAreaDeltaAndPreservesZ) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LinearRing> ring = MakeRing3D(*factory, {{0.0, 0.0, 10.0},
                                                           {0.0, 10.0, 20.0},
                                                           {4.0, 10.0, 30.0},
                                                           {5.0, 6.0, 40.0},
                                                           {6.0, 10.0, 50.0},
                                                           {10.0, 10.0, 60.0},
                                                           {10.0, 0.0, 70.0},
                                                           {0.0, 0.0, 10.0}});
  RingHullIndex index;

  // Setting minVertexNum=0 overrides maxAreaDelta=0.0 while retaining Z
  // ordinates on surviving vertices.
  {
    RingHull hull(ring.get(), /*p_isOuter=*/true);
    hull.setMaxAreaDelta(0.0);
    hull.setMinVertexNum(0);
    std::unique_ptr<LinearRing> result = hull.getHull(index);

    ASSERT_NE(result, nullptr);
    const CoordinateSequence* seq = result->getCoordinatesRO();
    ASSERT_EQ(seq->size(), 5u);
    EXPECT_TRUE(seq->hasZ());
    EXPECT_TRUE(seq->getAt(0).equals3D(Coordinate{0.0, 0.0, 10.0}));
    EXPECT_TRUE(seq->getAt(1).equals3D(Coordinate{0.0, 10.0, 20.0}));
    EXPECT_TRUE(seq->getAt(2).equals3D(Coordinate{10.0, 10.0, 60.0}));
    EXPECT_TRUE(seq->getAt(3).equals3D(Coordinate{10.0, 0.0, 70.0}));
    EXPECT_TRUE(seq->getAt(4).equals3D(Coordinate{0.0, 0.0, 10.0}));

    std::unique_ptr<Polygon> poly = hull.toGeometry();
    ASSERT_NE(poly, nullptr);
    EXPECT_EQ(poly->getExteriorRing()->getNumPoints(), 5u);
    EXPECT_TRUE(poly->getExteriorRing()->getCoordinatesRO()->getAt(2).equals3D(
        Coordinate{10.0, 10.0, 60.0}));
  }

  // Conversely, a high minVertexNum stops early even with a large maxAreaDelta.
  {
    RingHull hull(ring.get(), /*p_isOuter=*/true);
    hull.setMinVertexNum(7);
    hull.setMaxAreaDelta(100.0);
    std::unique_ptr<LinearRing> result = hull.getHull(index);

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getNumPoints(), 7u);
    EXPECT_DOUBLE_EQ(Area::ofRing(result->getCoordinatesRO()), 100.0);
  }
}

template <typename Tag, typename Tag::type M>
struct AccessRingHullPrivate {
  friend typename Tag::type GetRingHullPrivate(Tag) { return M; }
};

struct QueryHullTag {
  using type = void (RingHull::*)(const Envelope&, std::vector<Coordinate>&);
  friend type GetRingHullPrivate(QueryHullTag);
};

template struct AccessRingHullPrivate<QueryHullTag, &RingHull::queryHull>;

TEST(RingHullTest, QueryHullSkipsRemovedVerticesAndReturnsRemaining) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LinearRing> ring = MakeRing(*factory, {{0.0, 0.0},
                                                         {0.0, 10.0},
                                                         {10.0, 10.0},
                                                         {8.0, 5.0},
                                                         {10.0, 0.0},
                                                         {0.0, 0.0}});
  RingHull hull(ring.get(), /*p_isOuter=*/true);
  hull.setMinVertexNum(4);
  RingHullIndex index;
  hull.compute(index);

  auto query_fn = GetRingHullPrivate(QueryHullTag{});
  std::vector<Coordinate> pts;
  (hull.*query_fn)(Envelope(-1.0, 11.0, -1.0, 11.0), pts);
  EXPECT_EQ(pts.size(), 4u);
}

}  // namespace
}  // namespace geos_google
