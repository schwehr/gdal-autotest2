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
// third_party/geos/src/triangulate/polygon/TriDelaunayImprover.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/triangulate/polygon/TriDelaunayImprover.cpp

#include "third_party/geos/include/geos/triangulate/polygon/TriDelaunayImprover.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/Orientation.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/triangulate/polygon/PolygonEarClipper.h"
#include "third_party/geos/include/geos/triangulate/polygon/PolygonHoleJoiner.h"
#include "third_party/geos/include/geos/triangulate/quadedge/TrianglePredicate.h"
#include "third_party/geos/include/geos/triangulate/tri/Tri.h"
#include "third_party/geos/include/geos/triangulate/tri/TriList.h"
#include "third_party/geos/include/geos/triangulate/tri/TriangulationBuilder.h"

namespace geos_google {
namespace {

using geos::algorithm::Orientation;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::triangulate::polygon::PolygonEarClipper;
using geos::triangulate::polygon::PolygonHoleJoiner;
using geos::triangulate::polygon::TriDelaunayImprover;
using geos::triangulate::quadedge::TrianglePredicate;
using geos::triangulate::tri::Tri;
using geos::triangulate::tri::TriangulationBuilder;
using geos::triangulate::tri::TriList;

bool TriHasVertex2D(const Tri& tri, const Coordinate& pt) {
  for (TriIndex i = 0; i < 3; ++i) {
    if (tri.getCoordinate(i).equals2D(pt)) {
      return true;
    }
  }
  return false;
}

bool TriHasVertex3D(const Tri& tri, const Coordinate& pt) {
  for (TriIndex i = 0; i < 3; ++i) {
    if (tri.getCoordinate(i).equals3D(pt)) {
      return true;
    }
  }
  return false;
}

double MinTriangleAngle(const Tri& tri) {
  const Coordinate& p0 = tri.getCoordinate(0);
  const Coordinate& p1 = tri.getCoordinate(1);
  const Coordinate& p2 = tri.getCoordinate(2);
  const double a2 = p1.distanceSquared(p2);
  const double b2 = p0.distanceSquared(p2);
  const double c2 = p0.distanceSquared(p1);
  const double a = std::sqrt(a2);
  const double b = std::sqrt(b2);
  const double c = std::sqrt(c2);
  auto angle_from = [](double opp2, double s1, double s2, double s12,
                       double s22) {
    double cos_val = (s12 + s22 - opp2) / (2.0 * s1 * s2);
    cos_val = std::clamp(cos_val, -1.0, 1.0);
    return std::acos(cos_val);
  };
  return std::min({angle_from(a2, b, c, b2, c2), angle_from(b2, a, c, a2, c2),
                   angle_from(c2, a, b, a2, b2)});
}

int CountNonDelaunayConvexInteriorEdges(const TriList<Tri>& tri_list) {
  int violations = 0;
  for (const Tri* tri : tri_list) {
    if (tri == nullptr) continue;
    for (TriIndex index = 0; index < 3; ++index) {
      const Tri* tri1 = tri->getAdjacent(index);
      if (tri1 == nullptr) continue;
      const Coordinate& adj0 = tri->getCoordinate(index);
      const Coordinate& adj1 = tri->getCoordinate(Tri::next(index));
      const Coordinate& opp0 = tri->getCoordinate(Tri::oppVertex(index));
      const TriIndex index1 = tri1->getIndex(tri);
      const Coordinate& opp1 = tri1->getCoordinate(Tri::oppVertex(index1));

      const int dir0 = Orientation::index(opp0, adj0, opp1);
      const int dir1 = Orientation::index(opp1, adj1, opp0);
      if (dir0 != dir1) continue;

      if (TrianglePredicate::isInCircleRobust(adj0, opp0, adj1, opp1) ==
              Location::INTERIOR ||
          TrianglePredicate::isInCircleRobust(adj1, opp1, adj0, opp0) ==
              Location::INTERIOR) {
        ++violations;
      }
    }
  }
  return violations;
}

Coordinate InvertUnitCircle(double x, double y) {
  const double r2 = x * x + y * y;
  return Coordinate(x / r2, y / r2);
}

TEST(TriDelaunayImproverTest, EmptyTriListDoesNothing) {
  TriList<Tri> tri_list;
  EXPECT_TRUE(tri_list.empty());
  TriDelaunayImprover::improve(tri_list);
  EXPECT_EQ(tri_list.size(), 0u);
  EXPECT_NO_THROW(tri_list.validate());
}

TEST(TriDelaunayImproverTest, SingleTriangleDoesNothing) {
  TriList<Tri> tri_list;
  // Clockwise triangle: (0,0) -> (0,4) -> (3,0).
  tri_list.add(Coordinate(0, 0), Coordinate(0, 4), Coordinate(3, 0));
  TriangulationBuilder::build(tri_list);
  ASSERT_NO_THROW(tri_list.validate());

  TriDelaunayImprover::improve(tri_list);
  ASSERT_EQ(tri_list.size(), 1u);
  EXPECT_NO_THROW(tri_list.validate());
  EXPECT_DOUBLE_EQ(tri_list.area(), 6.0);
  EXPECT_TRUE(TriHasVertex2D(*tri_list[0], Coordinate(0, 0)));
  EXPECT_TRUE(TriHasVertex2D(*tri_list[0], Coordinate(0, 4)));
  EXPECT_TRUE(TriHasVertex2D(*tri_list[0], Coordinate(3, 0)));
}

TEST(TriDelaunayImproverTest, SkipsNullptrEntriesInTriList) {
  TriList<Tri> tri_list;
  // Non-Delaunay diamond split along long horizontal diagonal (0,1)-(10,1).
  tri_list.add(Coordinate(0, 1), Coordinate(5, 2), Coordinate(10, 1));
  tri_list.add(Coordinate(10, 1), Coordinate(5, 0), Coordinate(0, 1));
  TriangulationBuilder::build(tri_list);
  ASSERT_NO_THROW(tri_list.validate());

  // Insert nullptr at index 0 so improveScan encounters tri == nullptr
  // directly.
  std::vector<Tri*>& raw = tri_list.getTris();
  raw.insert(raw.begin(), nullptr);
  ASSERT_EQ(tri_list.size(), 3u);

  TriDelaunayImprover::improve(tri_list);
  EXPECT_EQ(tri_list[0], nullptr);
  ASSERT_NE(tri_list[1], nullptr);
  ASSERT_NE(tri_list[2], nullptr);

  // Valid remaining triangles should still have been flipped to (5,0)-(5,2).
  EXPECT_TRUE(TriHasVertex2D(*tri_list[1], Coordinate(5, 0)));
  EXPECT_TRUE(TriHasVertex2D(*tri_list[1], Coordinate(5, 2)));
  EXPECT_TRUE(TriHasVertex2D(*tri_list[2], Coordinate(5, 0)));
  EXPECT_TRUE(TriHasVertex2D(*tri_list[2], Coordinate(5, 2)));

  // Remove nullptr before validating invariants on tri_list.
  raw.erase(raw.begin());
  EXPECT_NO_THROW(tri_list.validate());
  EXPECT_EQ(CountNonDelaunayConvexInteriorEdges(tri_list), 0);
}

TEST(TriDelaunayImproverTest, FlipsNonDelaunayConvexQuad) {
  TriList<Tri> tri_list;
  // Two CW triangles sharing long diagonal (0,1)-(10,1).
  tri_list.add(Coordinate(0, 1), Coordinate(5, 2), Coordinate(10, 1));
  tri_list.add(Coordinate(10, 1), Coordinate(5, 0), Coordinate(0, 1));
  TriangulationBuilder::build(tri_list);
  ASSERT_NO_THROW(tri_list.validate());

  const double initial_area = tri_list.area();
  const double initial_min_angle =
      std::min(MinTriangleAngle(*tri_list[0]), MinTriangleAngle(*tri_list[1]));
  EXPECT_GT(CountNonDelaunayConvexInteriorEdges(tri_list), 0);

  TriDelaunayImprover::improve(tri_list);

  EXPECT_NO_THROW(tri_list.validate());
  EXPECT_DOUBLE_EQ(tri_list.area(), initial_area);
  EXPECT_EQ(CountNonDelaunayConvexInteriorEdges(tri_list), 0);

  // Short vertical diagonal (5,0)-(5,2) must be shared by both triangles.
  EXPECT_TRUE(TriHasVertex2D(*tri_list[0], Coordinate(5, 0)));
  EXPECT_TRUE(TriHasVertex2D(*tri_list[0], Coordinate(5, 2)));
  EXPECT_TRUE(TriHasVertex2D(*tri_list[1], Coordinate(5, 0)));
  EXPECT_TRUE(TriHasVertex2D(*tri_list[1], Coordinate(5, 2)));

  const double improved_min_angle =
      std::min(MinTriangleAngle(*tri_list[0]), MinTriangleAngle(*tri_list[1]));
  EXPECT_GT(improved_min_angle, initial_min_angle);
}

TEST(TriDelaunayImproverTest, DoesNotFlipAlreadyDelaunayOrCocircularQuad) {
  // Case 1: Strictly Delaunay convex quad stays unchanged.
  {
    TriList<Tri> tri_list;
    tri_list.add(Coordinate(0, 1), Coordinate(5, 2), Coordinate(5, 0));
    tri_list.add(Coordinate(5, 0), Coordinate(5, 2), Coordinate(10, 1));
    TriangulationBuilder::build(tri_list);
    ASSERT_NO_THROW(tri_list.validate());
    EXPECT_EQ(CountNonDelaunayConvexInteriorEdges(tri_list), 0);

    TriDelaunayImprover::improve(tri_list);
    EXPECT_NO_THROW(tri_list.validate());
    EXPECT_EQ(CountNonDelaunayConvexInteriorEdges(tri_list), 0);
    EXPECT_TRUE(TriHasVertex2D(*tri_list[0], Coordinate(0, 1)));
    EXPECT_FALSE(TriHasVertex2D(*tri_list[0], Coordinate(10, 1)));
    EXPECT_TRUE(TriHasVertex2D(*tri_list[1], Coordinate(10, 1)));
    EXPECT_FALSE(TriHasVertex2D(*tri_list[1], Coordinate(0, 1)));
  }

  // Case 2: Cocircular square gives Location::BOUNDARY and is not flipped.
  {
    TriList<Tri> tri_list;
    tri_list.add(Coordinate(0, 0), Coordinate(0, 10), Coordinate(10, 10));
    tri_list.add(Coordinate(10, 10), Coordinate(10, 0), Coordinate(0, 0));
    TriangulationBuilder::build(tri_list);
    ASSERT_NO_THROW(tri_list.validate());

    TriDelaunayImprover::improve(tri_list);
    EXPECT_NO_THROW(tri_list.validate());
    EXPECT_TRUE(TriHasVertex2D(*tri_list[0], Coordinate(0, 10)));
    EXPECT_FALSE(TriHasVertex2D(*tri_list[0], Coordinate(10, 0)));
    EXPECT_TRUE(TriHasVertex2D(*tri_list[1], Coordinate(10, 0)));
    EXPECT_FALSE(TriHasVertex2D(*tri_list[1], Coordinate(0, 10)));
  }
}

TEST(TriDelaunayImproverTest, DoesNotFlipNonConvexOrCollinearQuad) {
  // Case 1: Strictly non-convex (reflex) quad across edge (2,2)-(10,2).
  {
    TriList<Tri> tri_list;
    tri_list.add(Coordinate(2, 2), Coordinate(10, 2), Coordinate(6, 0));
    tri_list.add(Coordinate(10, 2), Coordinate(2, 2), Coordinate(1, 2.2));
    TriangulationBuilder::build(tri_list);
    ASSERT_NO_THROW(tri_list.validate());

    const double area_before = tri_list.area();
    TriDelaunayImprover::improve(tri_list);

    EXPECT_NO_THROW(tri_list.validate());
    EXPECT_DOUBLE_EQ(tri_list.area(), area_before);
    EXPECT_TRUE(TriHasVertex2D(*tri_list[0], Coordinate(6, 0)));
    EXPECT_FALSE(TriHasVertex2D(*tri_list[0], Coordinate(1, 2.2)));
    EXPECT_TRUE(TriHasVertex2D(*tri_list[1], Coordinate(1, 2.2)));
    EXPECT_FALSE(TriHasVertex2D(*tri_list[1], Coordinate(6, 0)));
  }

  // Case 2: Collinear corner where (0,0)-(0,5)-(0,10) yields dir0 == COLLINEAR
  // while dir1 == CLOCKWISE, so isConvex returns false.
  {
    TriList<Tri> tri_list;
    tri_list.add(Coordinate(0, 5), Coordinate(5, 5), Coordinate(0, 0));
    tri_list.add(Coordinate(5, 5), Coordinate(0, 5), Coordinate(0, 10));
    TriangulationBuilder::build(tri_list);
    ASSERT_NO_THROW(tri_list.validate());

    TriDelaunayImprover::improve(tri_list);

    EXPECT_NO_THROW(tri_list.validate());
    EXPECT_DOUBLE_EQ(tri_list.area(), 25.0);
    EXPECT_TRUE(TriHasVertex2D(*tri_list[0], Coordinate(0, 0)));
    EXPECT_FALSE(TriHasVertex2D(*tri_list[0], Coordinate(0, 10)));
    EXPECT_TRUE(TriHasVertex2D(*tri_list[1], Coordinate(0, 10)));
    EXPECT_FALSE(TriHasVertex2D(*tri_list[1], Coordinate(0, 0)));
  }
}

TEST(TriDelaunayImproverTest,
     FlipsViaSecondInCircleCheckWhenFirstHitsErrorBound) {
  const Coordinate adj0(0.0, 1.0);
  const Coordinate adj1(1.0, 0.0);
  const Coordinate opp0(0.5, 0.4995000002515);
  const Coordinate opp1(1000.0, 1001.0);

  // Confirm asymmetric behavior of TrianglePredicate::isInCircleRobust:
  // with the far point as the 2nd argument (origin of differences), the larger
  // error bound returns BOUNDARY; with the near point as the 2nd argument, the
  // tighter error bound detects INTERIOR.
  ASSERT_EQ(TrianglePredicate::isInCircleRobust(adj1, opp1, adj0, opp0),
            Location::BOUNDARY);
  ASSERT_EQ(TrianglePredicate::isInCircleRobust(adj0, opp0, adj1, opp1),
            Location::INTERIOR);

  // Adding the large CW triangle first ensures improveScan visits it at index 0
  // so isDelaunay evaluates the large triangle first (BOUNDARY -> false) and
  // triggers the flip solely via the second check.
  TriList<Tri> tri_list;
  tri_list.add(adj1, adj0, opp1);
  tri_list.add(adj0, adj1, opp0);
  TriangulationBuilder::build(tri_list);
  ASSERT_NO_THROW(tri_list.validate());

  const double initial_area = tri_list.area();
  TriDelaunayImprover::improve(tri_list);

  EXPECT_NO_THROW(tri_list.validate());
  // Post-flip area sums products of magnitude ~1e6, so IEEE-754 double
  // precision rounding introduces ~2e-11 error relative to initial_area.
  EXPECT_NEAR(tri_list.area(), initial_area, 1e-9);
  EXPECT_TRUE(TriHasVertex2D(*tri_list[0], opp0));
  EXPECT_TRUE(TriHasVertex2D(*tri_list[0], opp1));
  EXPECT_TRUE(TriHasVertex2D(*tri_list[1], opp0));
  EXPECT_TRUE(TriHasVertex2D(*tri_list[1], opp1));
  EXPECT_EQ(CountNonDelaunayConvexInteriorEdges(tri_list), 0);
}

TEST(TriDelaunayImproverTest, ImprovesPolygonWithHoleAfterEarClipping) {
  auto gf = GeometryFactory::create();
  WKTReader reader(gf.get());

  // Outer rectangle with a narrow diamond hole produces long skinny triangles
  // around the hole after ear clipping that Delaunay improvement refines.
  std::unique_ptr<geos::geom::Geometry> geom = reader.read(
      "POLYGON ((0 0, 20 0, 20 12, 0 12, 0 0), "
      "(2 6, 10 5, 18 6, 10 7, 2 6))");
  const auto* poly = dynamic_cast<const Polygon*>(geom.get());
  ASSERT_NE(poly, nullptr);

  std::unique_ptr<CoordinateSequence> joined = PolygonHoleJoiner::join(poly);
  ASSERT_NE(joined, nullptr);

  TriList<Tri> tri_list;
  PolygonEarClipper::triangulate(*joined, tri_list);
  TriangulationBuilder::build(tri_list);
  ASSERT_GT(tri_list.size(), 4u);
  ASSERT_NO_THROW(tri_list.validate());
  EXPECT_GT(CountNonDelaunayConvexInteriorEdges(tri_list), 0);
  const double ear_clipped_area = tri_list.area();
  EXPECT_NEAR(ear_clipped_area, poly->getArea(), 1e-9);

  TriDelaunayImprover::improve(tri_list);

  EXPECT_NO_THROW(tri_list.validate());
  EXPECT_NEAR(tri_list.area(), ear_clipped_area, 1e-9);
  EXPECT_EQ(CountNonDelaunayConvexInteriorEdges(tri_list), 0);
}

TEST(TriDelaunayImproverTest, StopsAtMaxIterationAndCompletesOnSecondCall) {
  constexpr int kFanCount = 214;
  constexpr double kCurvature = 1.5e-6;
  const Coordinate origin(0.0, 0.0);

  std::vector<Coordinate> arc;
  arc.reserve(kFanCount + 1);
  for (int k = 0; k <= kFanCount; ++k) {
    const double y = static_cast<double>(k) / static_cast<double>(kFanCount);
    const double x = 1.0 - kCurvature * y * y;
    arc.push_back(InvertUnitCircle(x, y));
  }

  const Coordinate q = InvertUnitCircle(
      1.0 + 0.75e-6, 1.0 - 0.5 / static_cast<double>(kFanCount));

  // Build CW fan T_0 .. T_{213} around origin plus terminal CW triangle T_Q.
  // Because each backward flip only becomes exposed on the next improveScan,
  // 214 passes are required; the first improve() stops at MAX_ITERATION (200).
  TriList<Tri> tri_list;
  for (int k = 0; k < kFanCount; ++k) {
    tri_list.add(arc[k], origin, arc[k + 1]);
  }
  tri_list.add(arc[kFanCount - 1], arc[kFanCount], q);
  TriangulationBuilder::build(tri_list);
  ASSERT_EQ(tri_list.size(), static_cast<std::size_t>(kFanCount + 1));
  ASSERT_NO_THROW(tri_list.validate());

  const double initial_area = tri_list.area();
  EXPECT_GT(CountNonDelaunayConvexInteriorEdges(tri_list), 0);

  // First call performs 200 flips and exits via loop bound, still incomplete.
  TriDelaunayImprover::improve(tri_list);
  EXPECT_NO_THROW(tri_list.validate());
  EXPECT_NEAR(tri_list.area(), initial_area, 1e-12);
  EXPECT_GT(CountNonDelaunayConvexInteriorEdges(tri_list), 0);

  // Second call finishes the remaining 14 flips and exits early with 0 flaws.
  TriDelaunayImprover::improve(tri_list);
  EXPECT_NO_THROW(tri_list.validate());
  EXPECT_NEAR(tri_list.area(), initial_area, 1e-12);
  EXPECT_EQ(CountNonDelaunayConvexInteriorEdges(tri_list), 0);

  // Calling a third time is a no-op.
  TriDelaunayImprover::improve(tri_list);
  EXPECT_EQ(CountNonDelaunayConvexInteriorEdges(tri_list), 0);
}

TEST(TriDelaunayImproverTest,
     ImprovesDisconnectedComponentsAndPreservesZCoordinates) {
  const Coordinate a0(0.0, 1.0, 11.0);
  const Coordinate a1(5.0, 2.0, 12.0);
  const Coordinate a2(10.0, 1.0, 13.0);
  const Coordinate a3(5.0, 0.0, 14.0);

  const Coordinate b0(20.0, 1.0, 21.0);
  const Coordinate b1(25.0, 2.0, 22.0);
  const Coordinate b2(30.0, 1.0, 23.0);
  const Coordinate b3(25.0, 0.0, 24.0);

  TriList<Tri> tri_list;
  tri_list.add(a0, a1, a2);
  tri_list.add(a2, a3, a0);
  tri_list.add(b0, b1, b2);
  tri_list.add(b2, b3, b0);
  TriangulationBuilder::build(tri_list);
  ASSERT_NO_THROW(tri_list.validate());
  EXPECT_EQ(CountNonDelaunayConvexInteriorEdges(tri_list), 4);

  TriDelaunayImprover::improve(tri_list);

  EXPECT_NO_THROW(tri_list.validate());
  EXPECT_DOUBLE_EQ(tri_list.area(), 20.0);
  EXPECT_EQ(CountNonDelaunayConvexInteriorEdges(tri_list), 0);

  // Both components flip to their short vertical diagonals while preserving Z.
  EXPECT_TRUE(TriHasVertex3D(*tri_list[0], a1));
  EXPECT_TRUE(TriHasVertex3D(*tri_list[0], a3));
  EXPECT_TRUE(TriHasVertex3D(*tri_list[0], a2));
  EXPECT_TRUE(TriHasVertex3D(*tri_list[1], a1));
  EXPECT_TRUE(TriHasVertex3D(*tri_list[1], a3));
  EXPECT_TRUE(TriHasVertex3D(*tri_list[1], a0));

  EXPECT_TRUE(TriHasVertex3D(*tri_list[2], b1));
  EXPECT_TRUE(TriHasVertex3D(*tri_list[2], b3));
  EXPECT_TRUE(TriHasVertex3D(*tri_list[2], b2));
  EXPECT_TRUE(TriHasVertex3D(*tri_list[3], b1));
  EXPECT_TRUE(TriHasVertex3D(*tri_list[3], b3));
  EXPECT_TRUE(TriHasVertex3D(*tri_list[3], b0));
}

TEST(TriDelaunayImproverTest, CascadingFlipsOnConvexHexagonFan) {
  const Coordinate p0(0.0, 0.0);
  const Coordinate p1(0.0, 2.0);
  const Coordinate p2(5.0, 3.0);
  const Coordinate p3(10.0, 2.0);
  const Coordinate p4(10.0, 0.0);
  const Coordinate p5(5.0, -1.0);

  TriList<Tri> tri_list;
  tri_list.add(p0, p1, p2);
  tri_list.add(p0, p2, p3);
  tri_list.add(p0, p3, p4);
  tri_list.add(p0, p4, p5);
  TriangulationBuilder::build(tri_list);
  ASSERT_NO_THROW(tri_list.validate());
  EXPECT_DOUBLE_EQ(tri_list.area(), 30.0);
  EXPECT_GT(CountNonDelaunayConvexInteriorEdges(tri_list), 0);

  TriDelaunayImprover::improve(tri_list);

  EXPECT_NO_THROW(tri_list.validate());
  EXPECT_DOUBLE_EQ(tri_list.area(), 30.0);
  EXPECT_EQ(CountNonDelaunayConvexInteriorEdges(tri_list), 0);
}

TEST(TriDelaunayImproverTest, NullptrOnlyTriListDoesNothing) {
  {
    TriList<Tri> tri_list;
    tri_list.getTris().push_back(nullptr);
    TriDelaunayImprover::improve(tri_list);
    ASSERT_EQ(tri_list.size(), 1u);
    EXPECT_EQ(tri_list[0], nullptr);
  }
  {
    TriList<Tri> tri_list;
    tri_list.getTris().push_back(nullptr);
    tri_list.getTris().push_back(nullptr);
    TriDelaunayImprover::improve(tri_list);
    ASSERT_EQ(tri_list.size(), 2u);
    EXPECT_EQ(tri_list[0], nullptr);
    EXPECT_EQ(tri_list[1], nullptr);
  }
}

}  // namespace
}  // namespace geos_google
