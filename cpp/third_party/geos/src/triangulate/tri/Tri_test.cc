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

// Unit tests for third_party/geos/src/triangulate/tri/Tri.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for IllegalArgumentException
//   and IllegalStateException assertions.
// - Does not violate validateAdjacent() internal assert() preconditions so
// tests
//   pass under both fastbuild (assertions enabled) and opt (-DNDEBUG).
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/triangulate/tri/Tri.cpp

#include "third_party/geos/include/geos/triangulate/tri/Tri.h"

#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/triangulate/tri/TriList.h"
#include "third_party/geos/include/geos/triangulate/tri/TriangulationBuilder.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/IllegalStateException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Polygon;
using geos::triangulate::tri::Tri;
using geos::triangulate::tri::TriangulationBuilder;
using geos::triangulate::tri::TriList;
using geos::util::IllegalArgumentException;
using geos::util::IllegalStateException;

// Cyclic vertex shift preserving clockwise triangle orientation.
Tri MakeRotatedCwTri(const std::array<Coordinate, 3>& verts, int offset) {
  return Tri(verts[(0 + offset) % 3], verts[(1 + offset) % 3],
             verts[(2 + offset) % 3]);
}

TEST(TriTest, StaticIndexHelpersCoverValidAndOutOfRangeIndices) {
  EXPECT_EQ(Tri::next(0), 1);
  EXPECT_EQ(Tri::next(1), 2);
  EXPECT_EQ(Tri::next(2), 0);
  EXPECT_EQ(Tri::next(-1), -1);
  EXPECT_EQ(Tri::next(3), -1);
  EXPECT_EQ(Tri::next(42), -1);

  EXPECT_EQ(Tri::prev(0), 2);
  EXPECT_EQ(Tri::prev(1), 0);
  EXPECT_EQ(Tri::prev(2), 1);
  EXPECT_EQ(Tri::prev(-1), -1);
  EXPECT_EQ(Tri::prev(3), -1);
  EXPECT_EQ(Tri::prev(42), -1);

  EXPECT_EQ(Tri::oppVertex(0), 2);
  EXPECT_EQ(Tri::oppVertex(1), 0);
  EXPECT_EQ(Tri::oppVertex(2), 1);
  EXPECT_EQ(Tri::oppVertex(-1), -1);
  EXPECT_EQ(Tri::oppVertex(3), -1);

  EXPECT_EQ(Tri::oppEdge(0), 1);
  EXPECT_EQ(Tri::oppEdge(1), 2);
  EXPECT_EQ(Tri::oppEdge(2), 0);
  EXPECT_EQ(Tri::oppEdge(-1), -1);
  EXPECT_EQ(Tri::oppEdge(3), -1);
}

TEST(TriTest, ConstructsWithCoordinatesAndNullAdjacencies) {
  const Coordinate c0(0.0, 0.0, 10.0);
  const Coordinate c1(0.0, 3.0, 20.0);
  const Coordinate c2(4.0, 0.0, 30.0);
  const Tri tri(c0, c1, c2);
  Tri unrelated(Coordinate(10.0, 10.0), Coordinate(10.0, 11.0),
                Coordinate(11.0, 10.0));

  EXPECT_TRUE(tri.getCoordinate(0).equals3D(c0));
  EXPECT_TRUE(tri.getCoordinate(1).equals3D(c1));
  EXPECT_TRUE(tri.getCoordinate(2).equals3D(c2));

  EXPECT_TRUE(tri.getEdgeStart(0).equals3D(c0));
  EXPECT_TRUE(tri.getEdgeEnd(0).equals3D(c1));
  EXPECT_TRUE(tri.getEdgeStart(1).equals3D(c1));
  EXPECT_TRUE(tri.getEdgeEnd(1).equals3D(c2));
  EXPECT_TRUE(tri.getEdgeStart(2).equals3D(c2));
  EXPECT_TRUE(tri.getEdgeEnd(2).equals3D(c0));

  EXPECT_EQ(tri.getAdjacent(0), nullptr);
  EXPECT_EQ(tri.getAdjacent(1), nullptr);
  EXPECT_EQ(tri.getAdjacent(2), nullptr);
  EXPECT_FALSE(tri.hasAdjacent());
  EXPECT_FALSE(tri.hasAdjacent(0));
  EXPECT_FALSE(tri.hasAdjacent(1));
  EXPECT_FALSE(tri.hasAdjacent(2));
  EXPECT_EQ(tri.numAdjacent(), 0);
  EXPECT_TRUE(tri.isBorder());
  EXPECT_TRUE(tri.isBoundary(0));
  EXPECT_TRUE(tri.isBoundary(1));
  EXPECT_TRUE(tri.isBoundary(2));
  EXPECT_FALSE(tri.isAdjacent(&unrelated));

  EXPECT_THROW(tri.getCoordinate(-1), IllegalArgumentException);
  EXPECT_THROW(tri.getCoordinate(3), IllegalArgumentException);
  EXPECT_THROW(tri.getEdgeStart(-1), IllegalArgumentException);
  EXPECT_THROW(tri.getEdgeStart(3), IllegalArgumentException);
  EXPECT_THROW(tri.getEdgeEnd(-1), IllegalArgumentException);
  EXPECT_THROW(tri.getEdgeEnd(3), IllegalArgumentException);
  EXPECT_THROW(tri.getAdjacent(-1), IllegalArgumentException);
  EXPECT_THROW(tri.getAdjacent(3), IllegalArgumentException);
  EXPECT_THROW(tri.hasAdjacent(-1), IllegalArgumentException);
  EXPECT_THROW(tri.hasAdjacent(3), IllegalArgumentException);
  EXPECT_THROW(tri.isBoundary(-1), IllegalArgumentException);
  EXPECT_THROW(tri.isBoundary(3), IllegalArgumentException);
}

TEST(TriTest, CoordinateLookupMethodsUse2DEquality) {
  const Tri tri(Coordinate(0.0, 0.0, 1.0), Coordinate(0.0, 4.0, 2.0),
                Coordinate(3.0, 0.0, 3.0));

  EXPECT_TRUE(tri.hasCoordinate(Coordinate(0.0, 0.0)));
  EXPECT_TRUE(tri.hasCoordinate(Coordinate(0.0, 4.0, 99.0)));
  EXPECT_TRUE(tri.hasCoordinate(Coordinate(3.0, 0.0, -5.0)));
  EXPECT_FALSE(tri.hasCoordinate(Coordinate(1.0, 1.0)));
  EXPECT_FALSE(tri.hasCoordinate(Coordinate(0.0, 3.0)));

  EXPECT_EQ(tri.getIndex(Coordinate(0.0, 0.0, 100.0)), 0);
  EXPECT_EQ(tri.getIndex(Coordinate(0.0, 4.0)), 1);
  EXPECT_EQ(tri.getIndex(Coordinate(3.0, 0.0, 200.0)), 2);
  EXPECT_EQ(tri.getIndex(Coordinate(9.0, 9.0)), -1);
}

TEST(TriTest, AdjacencySettersQueriesAndCounts) {
  const Coordinate p0(0.0, 0.0);
  const Coordinate p1(0.0, 2.0);
  const Coordinate p2(2.0, 0.0);
  Tri center(p0, p1, p2);
  Tri n0(p1, p0, Coordinate(-2.0, 1.0));
  Tri n1(p2, p1, Coordinate(2.0, 2.0));
  Tri n2(p0, p2, Coordinate(1.0, -2.0));
  Tri unrelated(Coordinate(10.0, 10.0), Coordinate(10.0, 12.0),
                Coordinate(12.0, 10.0));

  center.setTri(0, &n0);
  EXPECT_EQ(center.numAdjacent(), 1);
  EXPECT_TRUE(center.hasAdjacent());
  EXPECT_TRUE(center.hasAdjacent(0));
  EXPECT_FALSE(center.hasAdjacent(1));
  EXPECT_FALSE(center.hasAdjacent(2));
  EXPECT_TRUE(center.isBorder());
  EXPECT_FALSE(center.isBoundary(0));
  EXPECT_TRUE(center.isBoundary(1));
  EXPECT_TRUE(center.isBoundary(2));
  EXPECT_EQ(center.getIndex(&n0), 0);
  EXPECT_TRUE(center.isAdjacent(&n0));

  center.setAdjacent(nullptr, &n1, nullptr);
  EXPECT_EQ(center.numAdjacent(), 1);
  EXPECT_TRUE(center.hasAdjacent());
  EXPECT_EQ(center.getIndex(&n1), 1);
  EXPECT_TRUE(center.isBorder());

  center.setAdjacent(nullptr, nullptr, &n2);
  EXPECT_EQ(center.numAdjacent(), 1);
  EXPECT_TRUE(center.hasAdjacent());
  EXPECT_EQ(center.getIndex(&n2), 2);
  EXPECT_TRUE(center.isBorder());

  center.setAdjacent(&n0, &n1, &n2);
  EXPECT_EQ(center.numAdjacent(), 3);
  EXPECT_TRUE(center.hasAdjacent());
  EXPECT_FALSE(center.isBorder());
  EXPECT_FALSE(center.isBoundary(0));
  EXPECT_FALSE(center.isBoundary(1));
  EXPECT_FALSE(center.isBoundary(2));
  EXPECT_EQ(center.getIndex(&n0), 0);
  EXPECT_EQ(center.getIndex(&n1), 1);
  EXPECT_EQ(center.getIndex(&n2), 2);
  EXPECT_EQ(center.getIndex(&unrelated), -1);
  EXPECT_EQ(center.getIndex(static_cast<const Tri*>(nullptr)), -1);
  EXPECT_FALSE(center.isAdjacent(&unrelated));

  // Replace via coordinate lookup (ignoring Z).
  center.setAdjacent(Coordinate(0.0, 0.0, 77.0), &unrelated);
  EXPECT_EQ(center.getAdjacent(0), &unrelated);
  center.setAdjacent(p1, nullptr);
  EXPECT_EQ(center.getAdjacent(1), nullptr);
  EXPECT_EQ(center.numAdjacent(), 2);
  center.setAdjacent(p2, &n1);
  EXPECT_EQ(center.getAdjacent(2), &n1);

  EXPECT_THROW(center.setAdjacent(Coordinate(5.0, 5.0), &n0),
               IllegalArgumentException);
  EXPECT_THROW(center.setTri(-1, &n0), IllegalArgumentException);
  EXPECT_THROW(center.setTri(3, &n0), IllegalArgumentException);
}

TEST(TriTest, GetEdgeReturnsSharedEdgeCoordinatesOrThrows) {
  const Coordinate p0(0.0, 0.0, 1.0);
  const Coordinate p1(0.0, 2.0, 2.0);
  const Coordinate p2(2.0, 0.0, 3.0);
  Tri center(p0, p1, p2);
  Tri n0(p1, p0, Coordinate(-2.0, 1.0));
  Tri n1(p2, p1, Coordinate(2.0, 2.0));
  Tri n2(p0, p2, Coordinate(1.0, -2.0));
  Tri other(Coordinate(10.0, 0.0), Coordinate(10.0, 1.0),
            Coordinate(11.0, 0.0));

  center.setAdjacent(&n0, &n1, &n2);

  const auto edge0 = center.getEdge(&n0);
  EXPECT_EQ(&edge0.first, &center.getCoordinate(0));
  EXPECT_EQ(&edge0.second, &center.getCoordinate(1));
  EXPECT_TRUE(edge0.first.equals3D(p0));
  EXPECT_TRUE(edge0.second.equals3D(p1));

  const auto edge1 = center.getEdge(&n1);
  EXPECT_EQ(&edge1.first, &center.getCoordinate(1));
  EXPECT_EQ(&edge1.second, &center.getCoordinate(2));
  EXPECT_TRUE(edge1.first.equals3D(p1));
  EXPECT_TRUE(edge1.second.equals3D(p2));

  const auto edge2 = center.getEdge(&n2);
  EXPECT_EQ(&edge2.first, &center.getCoordinate(2));
  EXPECT_EQ(&edge2.second, &center.getCoordinate(0));
  EXPECT_TRUE(edge2.first.equals3D(p2));
  EXPECT_TRUE(edge2.second.equals3D(p0));

  EXPECT_THROW(center.getEdge(&other), IllegalArgumentException);
  EXPECT_THROW(center.getEdge(nullptr), IllegalArgumentException);
}

TEST(TriTest, MidpointAreaAndEdgeLengths) {
  const Tri right(Coordinate(0.0, 0.0, 5.0), Coordinate(0.0, 3.0, 7.0),
                  Coordinate(4.0, 0.0, 9.0));

  EXPECT_DOUBLE_EQ(right.getArea(), 6.0);
  EXPECT_DOUBLE_EQ(right.getLength(), 12.0);
  EXPECT_DOUBLE_EQ(right.getLength(0), 3.0);
  EXPECT_DOUBLE_EQ(right.getLength(1), 5.0);
  EXPECT_DOUBLE_EQ(right.getLength(2), 4.0);
  EXPECT_THROW(right.getLength(-1), IllegalArgumentException);
  EXPECT_THROW(right.getLength(3), IllegalArgumentException);

  const Coordinate m0 = right.midpoint(0);
  EXPECT_DOUBLE_EQ(m0.x, 0.0);
  EXPECT_DOUBLE_EQ(m0.y, 1.5);
  EXPECT_TRUE(std::isnan(m0.z));

  const Coordinate m1 = right.midpoint(1);
  EXPECT_DOUBLE_EQ(m1.x, 2.0);
  EXPECT_DOUBLE_EQ(m1.y, 1.5);
  EXPECT_TRUE(std::isnan(m1.z));

  const Coordinate m2 = right.midpoint(2);
  EXPECT_DOUBLE_EQ(m2.x, 2.0);
  EXPECT_DOUBLE_EQ(m2.y, 0.0);
  EXPECT_TRUE(std::isnan(m2.z));

  EXPECT_THROW(right.midpoint(-1), IllegalArgumentException);
  EXPECT_THROW(right.midpoint(3), IllegalArgumentException);

  const Tri collinear(Coordinate(0.0, 0.0), Coordinate(1.0, 1.0),
                      Coordinate(2.0, 2.0));
  EXPECT_DOUBLE_EQ(collinear.getArea(), 0.0);
  EXPECT_NEAR(collinear.getLength(), 4.0 * std::sqrt(2.0), 1e-12);
}

TEST(TriTest, ValidateChecksClockwiseOrientationAndAdjacencies) {
  Tri single_cw(Coordinate(0.0, 0.0), Coordinate(0.0, 2.0),
                Coordinate(2.0, 0.0));
  EXPECT_NO_THROW(single_cw.validate());

  TriList<Tri> tri_list;
  tri_list.add(Coordinate(0.0, 0.0), Coordinate(0.0, 2.0),
               Coordinate(2.0, 0.0));
  tri_list.add(Coordinate(0.0, 2.0), Coordinate(0.0, 0.0),
               Coordinate(-2.0, 1.0));
  tri_list.add(Coordinate(2.0, 0.0), Coordinate(0.0, 2.0),
               Coordinate(2.0, 2.0));
  tri_list.add(Coordinate(0.0, 0.0), Coordinate(2.0, 0.0),
               Coordinate(1.0, -2.0));
  TriangulationBuilder::build(tri_list);

  Tri* center = tri_list[0];
  EXPECT_EQ(center->numAdjacent(), 3);
  EXPECT_NO_THROW(center->validate());
  EXPECT_NO_THROW(center->validateAdjacent(0));
  EXPECT_NO_THROW(center->validateAdjacent(1));
  EXPECT_NO_THROW(center->validateAdjacent(2));

  EXPECT_THROW(center->validateAdjacent(-1), IllegalArgumentException);
  EXPECT_THROW(center->validateAdjacent(3), IllegalArgumentException);

  Tri ccw(Coordinate(0.0, 0.0), Coordinate(2.0, 0.0), Coordinate(0.0, 2.0));
  EXPECT_THROW(ccw.validate(), IllegalArgumentException);

  Tri collinear(Coordinate(0.0, 0.0), Coordinate(1.0, 1.0),
                Coordinate(2.0, 2.0));
  EXPECT_THROW(collinear.validate(), IllegalArgumentException);

  Tri nan_tri(Coordinate(0.0, 0.0), Coordinate(0.0, 1.0),
              Coordinate(std::numeric_limits<double>::quiet_NaN(), 0.0));
  EXPECT_THROW(nan_tri.validate(), IllegalArgumentException);
}

TEST(TriTest, IsInteriorVertexHandlesSurroundedBoundaryAndInconsistentFan) {
  const Coordinate center(1.0, 1.0);
  const Coordinate sw(0.0, 0.0);
  const Coordinate nw(0.0, 2.0);
  const Coordinate ne(2.0, 2.0);
  const Coordinate se(2.0, 0.0);

  TriList<Tri> fan;
  fan.add(center, sw,
          nw);  // [0]: edge 0=center->sw, edge 1=sw->nw, edge 2=nw->center
  fan.add(center, nw, ne);  // [1]
  fan.add(center, ne, se);  // [2]
  fan.add(center, se, sw);  // [3]
  TriangulationBuilder::build(fan);
  EXPECT_NO_THROW(fan.validate());

  EXPECT_TRUE(fan[0]->isInteriorVertex(0));
  EXPECT_TRUE(fan[1]->isInteriorVertex(0));
  EXPECT_TRUE(fan[2]->isInteriorVertex(0));
  EXPECT_TRUE(fan[3]->isInteriorVertex(0));

  // Boundary vertex whose first adjacent edge is already null.
  EXPECT_FALSE(fan[0]->isInteriorVertex(1));
  // Boundary vertex that traverses through fan[1] before hitting null edge.
  EXPECT_FALSE(fan[0]->isInteriorVertex(2));

  EXPECT_THROW(fan[0]->isInteriorVertex(-1), IllegalArgumentException);
  EXPECT_THROW(fan[0]->isInteriorVertex(3), IllegalArgumentException);

  // Break symmetry on fan[3] so fan[0] points to fan[3], but fan[3] forgets
  // fan[0].
  const TriIndex back_idx = fan[3]->getIndex(fan[0]);
  ASSERT_GE(back_idx, 0);
  fan[3]->setTri(back_idx, nullptr);
  EXPECT_THROW(fan[0]->isInteriorVertex(0), IllegalStateException);
}

TEST(TriTest, RemoveDetachesNeighborsAndRemovesFromTriList) {
  // Isolated tri remove() is a safe no-op.
  Tri isolated(Coordinate(0.0, 0.0), Coordinate(0.0, 1.0),
               Coordinate(1.0, 0.0));
  isolated.remove();
  EXPECT_EQ(isolated.numAdjacent(), 0);

  // Triangulation with central tri linked to 3 outer tris.
  TriList<Tri> tri_list;
  tri_list.add(Coordinate(0.0, 0.0), Coordinate(0.0, 2.0),
               Coordinate(2.0, 0.0));
  tri_list.add(Coordinate(0.0, 2.0), Coordinate(0.0, 0.0),
               Coordinate(-2.0, 1.0));
  tri_list.add(Coordinate(2.0, 0.0), Coordinate(0.0, 2.0),
               Coordinate(2.0, 2.0));
  tri_list.add(Coordinate(0.0, 0.0), Coordinate(2.0, 0.0),
               Coordinate(1.0, -2.0));
  TriangulationBuilder::build(tri_list);

  Tri* center = tri_list[0];
  Tri* n0 = tri_list[1];
  Tri* n1 = tri_list[2];
  Tri* n2 = tri_list[3];
  ASSERT_EQ(center->numAdjacent(), 3);

  center->remove(tri_list);
  EXPECT_EQ(tri_list.size(), 3u);
  EXPECT_EQ(center->numAdjacent(), 0);
  EXPECT_FALSE(center->hasAdjacent());
  EXPECT_EQ(n0->getIndex(center), -1);
  EXPECT_EQ(n1->getIndex(center), -1);
  EXPECT_EQ(n2->getIndex(center), -1);
  EXPECT_EQ(n0->numAdjacent(), 0);
  EXPECT_EQ(n1->numAdjacent(), 0);
  EXPECT_EQ(n2->numAdjacent(), 0);
  EXPECT_NO_THROW(tri_list.validate());

  // Direct remove() when only slot 1 is linked.
  const Coordinate a(0.0, 0.0);
  const Coordinate b(0.0, 2.0);
  const Coordinate c(2.0, 0.0);
  const Coordinate d(2.0, 2.0);
  Tri t_left(a, b, c);
  Tri t_right(c, b, d);
  t_left.setTri(1, &t_right);
  t_right.setTri(0, &t_left);
  EXPECT_NO_THROW(t_left.validate());

  t_left.remove();
  EXPECT_EQ(t_left.numAdjacent(), 0);
  EXPECT_EQ(t_right.numAdjacent(), 0);
  EXPECT_FALSE(t_right.isAdjacent(&t_left));
}

TEST(TriTest, FlipDiagonalOnIsolatedQuad) {
  const Coordinate a(0.0, 0.0);
  const Coordinate b(0.0, 2.0);
  const Coordinate c(2.0, 2.0);
  const Coordinate d(2.0, 0.0);

  Tri t0(c, a, b);  // Edge 0 is c -> a.
  Tri t1(a, c, d);  // Edge 0 is a -> c.
  t0.setTri(0, &t1);
  t1.setTri(0, &t0);
  EXPECT_NO_THROW(t0.validate());
  EXPECT_NO_THROW(t1.validate());

  t0.flip(0);
  EXPECT_NO_THROW(t0.validate());
  EXPECT_NO_THROW(t1.validate());
  EXPECT_TRUE(t0.getCoordinate(0).equals2D(d));
  EXPECT_TRUE(t0.getCoordinate(1).equals2D(b));
  EXPECT_TRUE(t0.getCoordinate(2).equals2D(c));
  EXPECT_TRUE(t1.getCoordinate(0).equals2D(b));
  EXPECT_TRUE(t1.getCoordinate(1).equals2D(d));
  EXPECT_TRUE(t1.getCoordinate(2).equals2D(a));
  EXPECT_EQ(t0.getAdjacent(0), &t1);
  EXPECT_EQ(t0.getAdjacent(1), nullptr);
  EXPECT_EQ(t0.getAdjacent(2), nullptr);
  EXPECT_EQ(t1.getAdjacent(0), &t0);
  EXPECT_EQ(t1.getAdjacent(1), nullptr);
  EXPECT_EQ(t1.getAdjacent(2), nullptr);
  EXPECT_DOUBLE_EQ(t0.getArea() + t1.getArea(), 4.0);

  // Flipping back swaps back to the a-c diagonal.
  t0.flip(0);
  EXPECT_NO_THROW(t0.validate());
  EXPECT_NO_THROW(t1.validate());
  EXPECT_TRUE(t0.hasCoordinate(a) && t0.hasCoordinate(c));
  EXPECT_TRUE(t1.hasCoordinate(a) && t1.hasCoordinate(c));
  EXPECT_DOUBLE_EQ(t0.getArea() + t1.getArea(), 4.0);
}

TEST(TriTest, FlipSurroundedQuadAcrossAllEdgesAndNeighborSlots) {
  const Coordinate a(0.0, 0.0);
  const Coordinate b(0.0, 2.0);
  const Coordinate c(2.0, 2.0);
  const Coordinate d(2.0, 0.0);
  const Coordinate p_top(1.0, 3.0);
  const Coordinate p_left(-1.0, 1.0);
  const Coordinate p_right(3.0, 1.0);
  const Coordinate p_bot(1.0, -1.0);

  const std::array<Coordinate, 3> t0_verts{c, a, b};
  const std::array<Coordinate, 3> t1_verts{a, c, d};
  const std::array<Coordinate, 3> top_verts{c, b, p_top};
  const std::array<Coordinate, 3> left_verts{b, a, p_left};
  const std::array<Coordinate, 3> right_verts{d, c, p_right};
  const std::array<Coordinate, 3> bot_verts{a, d, p_bot};

  for (int edge_idx = 0; edge_idx < 3; ++edge_idx) {
    for (int slot = 0; slot < 3; ++slot) {
      SCOPED_TRACE(edge_idx);
      SCOPED_TRACE(slot);

      // Rotate t0 and t1 so shared edge c-a is at edge_idx on t0, and rotate
      // outer neighbors so their shared edges sit at tri0, tri1, and tri2.
      Tri t0 = MakeRotatedCwTri(t0_verts, (3 - edge_idx) % 3);
      Tri t1 = MakeRotatedCwTri(t1_verts, (3 - slot) % 3);
      Tri n_top = MakeRotatedCwTri(top_verts, (3 - slot) % 3);
      Tri n_left = MakeRotatedCwTri(left_verts, (3 - slot) % 3);
      Tri n_right = MakeRotatedCwTri(right_verts, (3 - slot) % 3);
      Tri n_bot = MakeRotatedCwTri(bot_verts, (3 - slot) % 3);

      TriList<Tri> tris;
      tris.add(t0.getCoordinate(0), t0.getCoordinate(1), t0.getCoordinate(2));
      tris.add(t1.getCoordinate(0), t1.getCoordinate(1), t1.getCoordinate(2));
      tris.add(n_top.getCoordinate(0), n_top.getCoordinate(1),
               n_top.getCoordinate(2));
      tris.add(n_left.getCoordinate(0), n_left.getCoordinate(1),
               n_left.getCoordinate(2));
      tris.add(n_right.getCoordinate(0), n_right.getCoordinate(1),
               n_right.getCoordinate(2));
      tris.add(n_bot.getCoordinate(0), n_bot.getCoordinate(1),
               n_bot.getCoordinate(2));
      TriangulationBuilder::build(tris);
      ASSERT_NO_THROW(tris.validate());

      Tri* p_t0 = tris[0];
      Tri* p_t1 = tris[1];
      Tri* p_top_tri = tris[2];
      Tri* p_left_tri = tris[3];
      Tri* p_right_tri = tris[4];
      Tri* p_bot_tri = tris[5];

      ASSERT_EQ(p_t0->getAdjacent(edge_idx), p_t1);
      ASSERT_EQ(p_left_tri->getIndex(p_t0), slot);
      ASSERT_EQ(p_right_tri->getIndex(p_t1), slot);

      p_t0->flip(edge_idx);

      EXPECT_NO_THROW(tris.validate());
      EXPECT_TRUE(p_t0->isAdjacent(p_t1));
      EXPECT_TRUE(p_t1->isAdjacent(p_t0));
      EXPECT_TRUE(p_t0->hasCoordinate(b) && p_t0->hasCoordinate(d));
      EXPECT_TRUE(p_t1->hasCoordinate(b) && p_t1->hasCoordinate(d));
      EXPECT_TRUE(p_top_tri->isAdjacent(p_t0));
      EXPECT_TRUE(p_right_tri->isAdjacent(p_t0));
      EXPECT_FALSE(p_right_tri->isAdjacent(p_t1));
      EXPECT_TRUE(p_left_tri->isAdjacent(p_t1));
      EXPECT_FALSE(p_left_tri->isAdjacent(p_t0));
      EXPECT_TRUE(p_bot_tri->isAdjacent(p_t1));
      EXPECT_DOUBLE_EQ(p_t0->getArea(), 2.0);
      EXPECT_DOUBLE_EQ(p_t1->getArea(), 2.0);

      // Flipping back swaps diagonal back to a-c, rotating triangle halves.
      const TriIndex shared_after = p_t0->getIndex(p_t1);
      ASSERT_GE(shared_after, 0);
      p_t0->flip(shared_after);

      EXPECT_NO_THROW(tris.validate());
      EXPECT_TRUE(p_t0->hasCoordinate(a) && p_t0->hasCoordinate(c));
      EXPECT_TRUE(p_t1->hasCoordinate(a) && p_t1->hasCoordinate(c));
      EXPECT_TRUE(p_left_tri->isAdjacent(p_t1));
      EXPECT_FALSE(p_left_tri->isAdjacent(p_t0));
      EXPECT_TRUE(p_top_tri->isAdjacent(p_t1));
      EXPECT_TRUE(p_right_tri->isAdjacent(p_t0));
      EXPECT_FALSE(p_right_tri->isAdjacent(p_t1));
      EXPECT_TRUE(p_bot_tri->isAdjacent(p_t0));
    }
  }
}

TEST(TriTest, ToPolygonAndToGeometryConvertTrianglesToGeosGeometries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  const Tri tri2d(Coordinate(0.0, 0.0), Coordinate(0.0, 3.0),
                  Coordinate(4.0, 0.0));
  std::unique_ptr<Polygon> poly2d = tri2d.toPolygon(factory.get());
  ASSERT_NE(poly2d, nullptr);
  EXPECT_EQ(poly2d->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(poly2d->isValid());
  EXPECT_FALSE(poly2d->hasZ());
  EXPECT_EQ(poly2d->getNumPoints(), 4u);
  EXPECT_DOUBLE_EQ(poly2d->getArea(), 6.0);
  EXPECT_DOUBLE_EQ(poly2d->getLength(), 12.0);

  std::unique_ptr<CoordinateSequence> seq2d =
      poly2d->getExteriorRing()->getCoordinates();
  ASSERT_EQ(seq2d->size(), 4u);
  EXPECT_TRUE(seq2d->getAt(0).equals2D(Coordinate(0.0, 0.0)));
  EXPECT_TRUE(seq2d->getAt(1).equals2D(Coordinate(0.0, 3.0)));
  EXPECT_TRUE(seq2d->getAt(2).equals2D(Coordinate(4.0, 0.0)));
  EXPECT_TRUE(seq2d->getAt(3).equals2D(Coordinate(0.0, 0.0)));

  const Tri tri3d(Coordinate(0.0, 0.0, 5.0), Coordinate(0.0, 3.0, 7.0),
                  Coordinate(4.0, 0.0, 9.0));
  std::unique_ptr<Polygon> poly3d = tri3d.toPolygon(factory.get());
  ASSERT_NE(poly3d, nullptr);
  EXPECT_TRUE(poly3d->hasZ());
  std::unique_ptr<CoordinateSequence> seq3d =
      poly3d->getExteriorRing()->getCoordinates();
  ASSERT_EQ(seq3d->size(), 4u);
  EXPECT_TRUE(seq3d->getAt(0).equals3D(Coordinate(0.0, 0.0, 5.0)));
  EXPECT_TRUE(seq3d->getAt(1).equals3D(Coordinate(0.0, 3.0, 7.0)));
  EXPECT_TRUE(seq3d->getAt(2).equals3D(Coordinate(4.0, 0.0, 9.0)));
  EXPECT_TRUE(seq3d->getAt(3).equals3D(Coordinate(0.0, 0.0, 5.0)));

  std::set<Tri*> empty_tris;
  std::unique_ptr<Geometry> empty_coll =
      Tri::toGeometry(empty_tris, factory.get());
  ASSERT_NE(empty_coll, nullptr);
  EXPECT_EQ(empty_coll->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_TRUE(empty_coll->isEmpty());
  EXPECT_EQ(empty_coll->getNumGeometries(), 0u);

  Tri second(Coordinate(4.0, 0.0), Coordinate(0.0, 3.0), Coordinate(4.0, 3.0));
  Tri first = tri2d;
  std::set<Tri*> tris{&first, &second};
  std::unique_ptr<Geometry> coll = Tri::toGeometry(tris, factory.get());
  ASSERT_NE(coll, nullptr);
  EXPECT_EQ(coll->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_FALSE(coll->isEmpty());
  EXPECT_EQ(coll->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(coll->getArea(), 12.0);
  EXPECT_EQ(coll->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_POLYGON);
  EXPECT_EQ(coll->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(coll->getGeometryN(0)->getArea(), 6.0);
  EXPECT_DOUBLE_EQ(coll->getGeometryN(1)->getArea(), 6.0);
}

TEST(TriTest, StreamOutputFormatsWktPolygon) {
  const Tri tri2d(Coordinate(0.0, 0.0), Coordinate(0.0, 1.0),
                  Coordinate(1.0, 0.0));
  std::ostringstream oss2d;
  oss2d << tri2d;
  EXPECT_EQ(oss2d.str(), "POLYGON ((0 0, 0 1, 1 0, 0 0))");

  const Tri tri3d(Coordinate(0.0, 0.0, 5.0), Coordinate(0.0, 1.0, 6.0),
                  Coordinate(1.0, 0.0, 7.0));
  std::ostringstream oss3d;
  oss3d << tri3d << "; " << tri2d;
  EXPECT_EQ(
      oss3d.str(),
      "POLYGON ((0 0 5, 0 1 6, 1 0 7, 0 0 5)); POLYGON ((0 0, 0 1, 1 0, 0 0))");
}

}  // namespace
}  // namespace geos_google
