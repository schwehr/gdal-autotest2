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

#include "third_party/geos/include/geos/triangulate/polygon/PolygonEarClipper.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <memory>
#include <numbers>
#include <utility>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/triangulate/polygon/PolygonHoleJoiner.h"
#include "third_party/geos/include/geos/triangulate/tri/Tri.h"
#include "third_party/geos/include/geos/triangulate/tri/TriList.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/IllegalStateException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::triangulate::polygon::PolygonEarClipper;
using geos::triangulate::polygon::PolygonHoleJoiner;
using geos::triangulate::tri::Tri;
using geos::triangulate::tri::TriList;
using geos::util::IllegalArgumentException;
using geos::util::IllegalStateException;

constexpr double kAreaTol = 1e-9;

CoordinateSequence MakeSeq(std::initializer_list<Coordinate> coords) {
  return CoordinateSequence(coords);
}

bool HasVertex2D(const Tri& tri, const Coordinate& pt) {
  for (int i = 0; i < 3; ++i) {
    if (tri.getCoordinate(i).equals2D(pt)) {
      return true;
    }
  }
  return false;
}

bool AnyTriHasVertex2D(const TriList<Tri>& tris, const Coordinate& pt) {
  for (const Tri* tri : tris) {
    if (HasVertex2D(*tri, pt)) {
      return true;
    }
  }
  return false;
}

void VerifyTriangulationBasic(TriList<Tri>& tris, std::size_t expected_count,
                              double expected_area) {
  ASSERT_EQ(tris.size(), expected_count);
  EXPECT_NO_THROW(tris.validate());
  for (const Tri* tri : tris) {
    EXPECT_GT(tri->getArea(), 0.0);
  }
  EXPECT_NEAR(tris.area(), expected_area, kAreaTol);
}

void VerifyTriangulationDisjoint(TriList<Tri>& tris, const GeometryFactory* gf,
                                 const Geometry* expected_geom) {
  for (std::size_t i = 0; i < tris.size(); ++i) {
    auto poly_i = tris[i]->toPolygon(gf);
    for (std::size_t j = i + 1; j < tris.size(); ++j) {
      auto poly_j = tris[j]->toPolygon(gf);
      auto inter = poly_i->intersection(poly_j.get());
      EXPECT_NEAR(inter->getArea(), 0.0, kAreaTol);
    }
  }
  if (expected_geom != nullptr) {
    auto collection = tris.toGeometry(gf);
    auto union_geom = collection->Union();
    EXPECT_TRUE(union_geom->equals(expected_geom));
  }
}

TEST(PolygonEarClipperTest, SingleTrianglePreservesVerticesAndZ) {
  CoordinateSequence shell = MakeSeq({
      Coordinate(0.0, 0.0, 1.0),
      Coordinate(0.0, 10.0, 2.0),
      Coordinate(10.0, 0.0, 3.0),
      Coordinate(0.0, 0.0, 1.0),
  });

  TriList<Tri> tris;
  PolygonEarClipper::triangulate(shell, tris);

  VerifyTriangulationBasic(tris, 1, 50.0);
  const Tri* tri = tris[0];
  EXPECT_TRUE(tri->getCoordinate(0).equals2D(Coordinate(0.0, 0.0)));
  EXPECT_DOUBLE_EQ(tri->getCoordinate(0).z, 1.0);
  EXPECT_TRUE(tri->getCoordinate(1).equals2D(Coordinate(0.0, 10.0)));
  EXPECT_DOUBLE_EQ(tri->getCoordinate(1).z, 2.0);
  EXPECT_TRUE(tri->getCoordinate(2).equals2D(Coordinate(10.0, 0.0)));
  EXPECT_DOUBLE_EQ(tri->getCoordinate(2).z, 3.0);
}

TEST(PolygonEarClipperTest, ConvexSquareAndPentagon) {
  auto gf = GeometryFactory::create();
  WKTReader reader(*gf);

  {
    auto square =
        reader.read<Polygon>("POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))");
    auto coords = square->getExteriorRing()->getCoordinates();
    TriList<Tri> tris;
    PolygonEarClipper::triangulate(*coords, tris);
    VerifyTriangulationBasic(tris, 2, 100.0);
    VerifyTriangulationDisjoint(tris, gf.get(), square.get());
  }

  {
    // CW pentagon wraps around and removes vertexFirst (index 0) during
    // clipping.
    auto pentagon =
        reader.read<Polygon>("POLYGON ((0 0, 0 6, 5 10, 10 6, 8 0, 0 0))");
    auto coords = pentagon->getExteriorRing()->getCoordinates();
    TriList<Tri> tris;
    PolygonEarClipper::triangulate(*coords, tris);
    VerifyTriangulationBasic(tris, 3, pentagon->getArea());
    VerifyTriangulationDisjoint(tris, gf.get(), pentagon.get());
  }
}

TEST(PolygonEarClipperTest, ConcavePolygonRejectsIntrudingVertexEar) {
  auto gf = GeometryFactory::create();
  WKTReader reader(*gf);

  // CW chevron where (5,11) lies inside candidate ear ((0,10), (5,12),
  // (10,10)), while (8,4) lies inside candidate ear envelope [5,10]x[0,12] of
  // ((5,12), (10,10), (10,0)) but outside the triangle itself.
  auto chevron = reader.read<Polygon>(
      "POLYGON ((0 10, 5 12, 10 10, 10 0, 8 4, 5 11, 0 0, 0 10))");
  auto coords = chevron->getExteriorRing()->getCoordinates();

  TriList<Tri> tris;
  PolygonEarClipper::triangulate(*coords, tris);

  VerifyTriangulationBasic(tris, 5, 56.0);
  VerifyTriangulationDisjoint(tris, gf.get(), chevron.get());
}

TEST(PolygonEarClipperTest, MultiNodeStarExercisesPackedRtreeLevels) {
  auto gf = GeometryFactory::create();

  // 36-vertex CW star polygon (> 16 vertices per VertexSequencePackedRtree leaf
  // node).
  constexpr std::size_t kPoints = 18;
  constexpr std::size_t kVertices = kPoints * 2;
  constexpr double kOuterR = 10.0;
  constexpr double kInnerR = 5.0;

  CoordinateSequence shell(0, false, false);
  for (std::size_t i = 0; i < kVertices; ++i) {
    double angle = -2.0 * std::numbers::pi * static_cast<double>(i) /
                   static_cast<double>(kVertices);
    double r = (i % 2 == 0) ? kOuterR : kInnerR;
    shell.add(r * std::cos(angle), r * std::sin(angle));
  }
  shell.closeRing();

  auto ring = gf->createLinearRing(shell.clone());
  auto poly = gf->createPolygon(std::move(ring));
  const double expected_area =
      static_cast<double>(kVertices) * 0.5 * kOuterR * kInnerR *
      std::sin(2.0 * std::numbers::pi / static_cast<double>(kVertices));

  TriList<Tri> tris;
  PolygonEarClipper::triangulate(shell, tris);

  VerifyTriangulationBasic(tris, kVertices - 2, expected_area);
  EXPECT_NEAR(poly->getArea(), expected_area, kAreaTol);
  VerifyTriangulationDisjoint(tris, gf.get(), poly.get());
}

TEST(PolygonEarClipperTest,
     FlatCornersIncludedByDefaultAndSkippedWhenConfigured) {
  auto gf = GeometryFactory::create();
  WKTReader reader(*gf);

  // CW square with 4 collinear midpoint vertices on its edges.
  auto poly = reader.read<Polygon>(
      "POLYGON ((0 0, 0 5, 0 10, 5 10, 10 10, 10 5, 10 0, 5 0, 0 0))");
  auto coords = poly->getExteriorRing()->getCoordinates();

  {
    TriList<Tri> tris_default;
    PolygonEarClipper clipper(*coords);
    clipper.setSkipFlatCorners(false);
    clipper.compute(tris_default);

    VerifyTriangulationBasic(tris_default, 6, 100.0);
    VerifyTriangulationDisjoint(tris_default, gf.get(), poly.get());
    EXPECT_TRUE(AnyTriHasVertex2D(tris_default, Coordinate(0.0, 5.0)));
    EXPECT_TRUE(AnyTriHasVertex2D(tris_default, Coordinate(5.0, 10.0)));
    EXPECT_TRUE(AnyTriHasVertex2D(tris_default, Coordinate(10.0, 5.0)));
    EXPECT_TRUE(AnyTriHasVertex2D(tris_default, Coordinate(5.0, 0.0)));
  }

  {
    TriList<Tri> tris_skipped;
    PolygonEarClipper clipper(*coords);
    clipper.setSkipFlatCorners(true);
    clipper.compute(tris_skipped);

    VerifyTriangulationBasic(tris_skipped, 2, 100.0);
    VerifyTriangulationDisjoint(tris_skipped, gf.get(), poly.get());
    EXPECT_FALSE(AnyTriHasVertex2D(tris_skipped, Coordinate(0.0, 5.0)));
    EXPECT_FALSE(AnyTriHasVertex2D(tris_skipped, Coordinate(5.0, 10.0)));
    EXPECT_FALSE(AnyTriHasVertex2D(tris_skipped, Coordinate(10.0, 5.0)));
    EXPECT_FALSE(AnyTriHasVertex2D(tris_skipped, Coordinate(5.0, 0.0)));
  }

  // With skipFlatCorners=true, a non-flat reflex corner must be retained while
  // a flat midpoint visited on the ring is removed.
  auto l_shape = reader.read<Polygon>(
      "POLYGON ((0 0, 0 10, 5 10, 5 5, 10 5, 10 0, 5 0, 0 0))");
  auto l_coords = l_shape->getExteriorRing()->getCoordinates();
  {
    TriList<Tri> tris_l_default;
    PolygonEarClipper clipper_l(*l_coords);
    clipper_l.compute(tris_l_default);
    VerifyTriangulationBasic(tris_l_default, 5, 75.0);
    VerifyTriangulationDisjoint(tris_l_default, gf.get(), l_shape.get());
    EXPECT_TRUE(AnyTriHasVertex2D(tris_l_default, Coordinate(5.0, 5.0)));
    EXPECT_TRUE(AnyTriHasVertex2D(tris_l_default, Coordinate(5.0, 0.0)));
  }
  {
    TriList<Tri> tris_l_skip;
    PolygonEarClipper clipper_l(*l_coords);
    clipper_l.setSkipFlatCorners(true);
    clipper_l.compute(tris_l_skip);

    VerifyTriangulationBasic(tris_l_skip, 4, 75.0);
    VerifyTriangulationDisjoint(tris_l_skip, gf.get(), l_shape.get());
    EXPECT_TRUE(AnyTriHasVertex2D(tris_l_skip, Coordinate(5.0, 5.0)));
    EXPECT_FALSE(AnyTriHasVertex2D(tris_l_skip, Coordinate(5.0, 0.0)));
  }
}

TEST(PolygonEarClipperTest, RemovesRepeatedAndCollapsedCornerVertices) {
  auto gf = GeometryFactory::create();

  // Exercises AAB (pts[1]==pts[0]) and ABB (pts[1]==pts[2]) repeated corners.
  CoordinateSequence repeated = MakeSeq({
      Coordinate(0.0, 0.0),
      Coordinate(0.0, 0.0),
      Coordinate(0.0, 10.0),
      Coordinate(10.0, 10.0),
      Coordinate(10.0, 10.0),
      Coordinate(10.0, 0.0),
      Coordinate(0.0, 0.0),
  });
  TriList<Tri> tris_rep;
  PolygonEarClipper::triangulate(repeated, tris_rep);
  VerifyTriangulationBasic(tris_rep, 2, 100.0);
  VerifyTriangulationDisjoint(tris_rep, gf.get(), nullptr);

  // Exercises ABA collapsed spike (pts[0]==pts[2], pts[1] distinct).
  CoordinateSequence spike = MakeSeq({
      Coordinate(0.0, 0.0),
      Coordinate(0.0, 10.0),
      Coordinate(5.0, 15.0),
      Coordinate(0.0, 10.0),
      Coordinate(10.0, 10.0),
      Coordinate(10.0, 0.0),
      Coordinate(0.0, 0.0),
  });
  TriList<Tri> tris_spike;
  PolygonEarClipper::triangulate(spike, tris_spike);
  VerifyTriangulationBasic(tris_spike, 2, 100.0);
  EXPECT_FALSE(AnyTriHasVertex2D(tris_spike, Coordinate(5.0, 15.0)));
  VerifyTriangulationDisjoint(tris_spike, gf.get(), nullptr);
}

TEST(PolygonEarClipperTest, TriangulatesPolygonsWithJoinedHoles) {
  auto gf = GeometryFactory::create();
  WKTReader reader(*gf);

  // Non-touching interior hole.
  {
    auto poly = reader.read<Polygon>(
        "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), "
        "(3 3, 7 3, 7 7, 3 7, 3 3))");
    auto joined = PolygonHoleJoiner::join(poly.get());
    TriList<Tri> tris;
    PolygonEarClipper::triangulate(*joined, tris);

    VerifyTriangulationBasic(tris, 8, 84.0);
    VerifyTriangulationDisjoint(tris, gf.get(), poly.get());
  }

  // Hole touching exterior ring at a vertex, plus a second disjoint hole.
  {
    auto poly = reader.read<Polygon>(
        "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), "
        "(5 10, 3 7, 7 7, 5 10), "
        "(2 2, 4 2, 4 4, 2 4, 2 2))");
    auto joined = PolygonHoleJoiner::join(poly.get());
    TriList<Tri> tris;
    PolygonEarClipper::triangulate(*joined, tris);

    EXPECT_NO_THROW(tris.validate());
    EXPECT_NEAR(tris.area(), poly->getArea(), kAreaTol);
    VerifyTriangulationDisjoint(tris, gf.get(), poly.get());
  }
}

TEST(PolygonEarClipperTest, DuplicateApexScanHandlesAllEdgeCases) {
  auto gf = GeometryFactory::create();
  WKTReader reader(*gf);

  // 1. Bent two-segment out-and-back bridge
  // (0,0)->(5,6)->(10,8)->hole->(10,8)->(5,6)->(0,0). At corner
  // ((0,0),(5,6),(10,8)), duplicate apex (5,6) triggers (aOut == 0 && aIn ==
  // cornerAngle) -> false, while ((5,6),(10,8),(14,8)) passes isValidEarScan ->
  // true.
  {
    auto expected = reader.read<Polygon>(
        "POLYGON ((0 0, 0 20, 20 20, 20 0, 0 0), "
        "(10 8, 14 8, 12 14, 10 8))");
    CoordinateSequence bent_bridge = MakeSeq({
        Coordinate(0.0, 0.0),
        Coordinate(5.0, 6.0),
        Coordinate(10.0, 8.0),
        Coordinate(14.0, 8.0),
        Coordinate(12.0, 14.0),
        Coordinate(10.0, 8.0),
        Coordinate(5.0, 6.0),
        Coordinate(0.0, 0.0),
        Coordinate(0.0, 20.0),
        Coordinate(20.0, 20.0),
        Coordinate(20.0, 0.0),
        Coordinate(0.0, 0.0),
    });

    TriList<Tri> tris;
    PolygonEarClipper::triangulate(bent_bridge, tris);
    VerifyTriangulationBasic(tris, 9, 388.0);
    VerifyTriangulationDisjoint(tris, gf.get(), expected.get());
  }

  // 2. Duplicate apex with outgoing edge inside corner wedge (aOut > 0 && aOut
  // < cornerAngle) while vNext=(0,5) lies outside the candidate ear's bounding
  // box [0,5]x[8,10].
  {
    CoordinateSequence out_wedge = MakeSeq({
        Coordinate(0.0, 10.0),
        Coordinate(5.0, 10.0),
        Coordinate(5.0, 8.0),
        Coordinate(2.0, 6.0),
        Coordinate(-5.0, 15.0),
        Coordinate(5.0, 10.0),
        Coordinate(0.0, 5.0),
        Coordinate(0.0, 10.0),
    });

    TriList<Tri> tris;
    PolygonEarClipper::triangulate(out_wedge, tris);
    VerifyTriangulationBasic(tris, 5, 43.0);
  }

  // 3. Duplicate apex with incoming edge inside corner wedge (aIn > 0 && aIn <
  // cornerAngle) while vPrev=(0,5) and vNext=(5,15) lie outside candidate ear
  // envelope [-5,0]x[8,10].
  {
    CoordinateSequence in_wedge = MakeSeq({
        Coordinate(-5.0, 8.0),
        Coordinate(-5.0, 10.0),
        Coordinate(0.0, 10.0),
        Coordinate(0.0, 5.0),
        Coordinate(-5.0, 10.0),
        Coordinate(5.0, 15.0),
        Coordinate(0.0, 0.0),
        Coordinate(-5.0, 8.0),
    });

    TriList<Tri> tris;
    PolygonEarClipper::triangulate(in_wedge, tris);
    VerifyTriangulationBasic(tris, 5, 80.0);
  }
}

TEST(PolygonEarClipperTest, ToGeometryBeforeAndAfterCompute) {
  CoordinateSequence shell = MakeSeq({
      Coordinate(0.0, 0.0),
      Coordinate(0.0, 10.0),
      Coordinate(6.0, 10.0),
      Coordinate(6.0, 4.0),
      Coordinate(10.0, 4.0),
      Coordinate(10.0, 0.0),
      Coordinate(0.0, 0.0),
  });

  PolygonEarClipper clipper(shell);

  // Before compute(), toGeometry() returns a polygon matching the unclipped
  // shell.
  auto before = clipper.toGeometry();
  ASSERT_NE(before, nullptr);
  EXPECT_FALSE(before->isEmpty());
  EXPECT_TRUE(before->isValid());
  EXPECT_EQ(before->getNumInteriorRing(), 0u);
  EXPECT_NEAR(before->getArea(), 76.0, kAreaTol);

  TriList<Tri> tris;
  clipper.compute(tris);
  VerifyTriangulationBasic(tris, 4, 76.0);

  // After compute(), vertexSize is 2 with distinct endpoints, so closing the
  // ring yields a 3-point collapsed (invalid, zero-area) polygon.
  auto after = clipper.toGeometry();
  ASSERT_NE(after, nullptr);
  EXPECT_FALSE(after->isEmpty());
  EXPECT_FALSE(after->isValid());
  EXPECT_EQ(after->getExteriorRing()->getNumPoints(), 3u);
  EXPECT_NEAR(after->getArea(), 0.0, kAreaTol);

  // When compute() reduces a ring of identical coordinates to 2 equal vertices,
  // closeRing() does not add a 3rd point and LinearRing throws.
  CoordinateSequence all_same = MakeSeq({
      Coordinate(1.0, 2.0),
      Coordinate(1.0, 2.0),
      Coordinate(1.0, 2.0),
      Coordinate(1.0, 2.0),
  });
  PolygonEarClipper degenerate_clipper(all_same);
  TriList<Tri> degenerate_tris;
  degenerate_clipper.compute(degenerate_tris);
  EXPECT_TRUE(degenerate_tris.empty());
  EXPECT_THROW(degenerate_clipper.toGeometry(), IllegalArgumentException);
}

TEST(PolygonEarClipperTest, DegenerateAndInvalidOrientations) {
  // Counter-clockwise ring has no convex CW corners -> throws
  // IllegalStateException.
  CoordinateSequence ccw_square = MakeSeq({
      Coordinate(0.0, 0.0),
      Coordinate(10.0, 0.0),
      Coordinate(10.0, 10.0),
      Coordinate(0.0, 10.0),
      Coordinate(0.0, 0.0),
  });
  TriList<Tri> tris_ccw;
  EXPECT_THROW(PolygonEarClipper::triangulate(ccw_square, tris_ccw),
               IllegalStateException);

  // Collapsed out-and-back collinear spike reduces to vertexSize < 3 with 0
  // triangles.
  CoordinateSequence collapsed = MakeSeq({
      Coordinate(0.0, 0.0),
      Coordinate(5.0, 0.0),
      Coordinate(10.0, 0.0),
      Coordinate(5.0, 0.0),
      Coordinate(0.0, 0.0),
  });
  TriList<Tri> tris_collapsed;
  PolygonEarClipper::triangulate(collapsed, tris_collapsed);
  EXPECT_TRUE(tris_collapsed.empty());

  // 3-vertex collinear ring throws when flat corners are kept, but empties when
  // skipped.
  CoordinateSequence flat_ring = MakeSeq({
      Coordinate(0.0, 0.0),
      Coordinate(5.0, 0.0),
      Coordinate(10.0, 0.0),
      Coordinate(0.0, 0.0),
  });
  {
    PolygonEarClipper clipper(flat_ring);
    TriList<Tri> tris_keep;
    EXPECT_THROW(clipper.compute(tris_keep), IllegalStateException);
  }
  {
    PolygonEarClipper clipper(flat_ring);
    clipper.setSkipFlatCorners(true);
    TriList<Tri> tris_skip;
    clipper.compute(tris_skip);
    EXPECT_TRUE(tris_skip.empty());
  }

  // NaN ordinate triggers IllegalArgumentException from Orientation::index.
  CoordinateSequence nan_ring = MakeSeq({
      Coordinate(0.0, 0.0),
      Coordinate(0.0, std::numeric_limits<double>::quiet_NaN()),
      Coordinate(10.0, 10.0),
      Coordinate(10.0, 0.0),
      Coordinate(0.0, 0.0),
  });
  TriList<Tri> tris_nan;
  EXPECT_THROW(PolygonEarClipper::triangulate(nan_ring, tris_nan),
               IllegalArgumentException);
}

}  // namespace
}  // namespace geos_google
