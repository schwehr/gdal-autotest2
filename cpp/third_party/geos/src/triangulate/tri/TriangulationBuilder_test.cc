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

// Unit tests for third_party/geos/src/triangulate/tri/TriangulationBuilder.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions
//   and Tri::validate() calls.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/triangulate/tri/TriangulationBuilder.cpp

#include "third_party/geos/include/geos/triangulate/tri/TriangulationBuilder.h"

#include <array>
#include <cstddef>
#include <limits>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/triangulate/tri/Tri.h"
#include "third_party/geos/include/geos/triangulate/tri/TriList.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::triangulate::tri::Tri;
using geos::triangulate::tri::TriangulationBuilder;
using geos::triangulate::tri::TriList;

void VerifyAdjacencyConsistency(const TriList<Tri>& triList) {
  std::size_t triIndex = 0;
  for (Tri* tri : triList) {
    SCOPED_TRACE(triIndex);
    ++triIndex;
    for (int edge = 0; edge < 3; ++edge) {
      SCOPED_TRACE(edge);
      Tri* adj = tri->getAdjacent(edge);
      if (adj == nullptr) {
        EXPECT_FALSE(tri->hasAdjacent(edge));
        EXPECT_TRUE(tri->isBoundary(edge));
        continue;
      }
      EXPECT_NE(adj, tri);
      EXPECT_TRUE(tri->hasAdjacent(edge));
      EXPECT_FALSE(tri->isBoundary(edge));
      EXPECT_TRUE(adj->isAdjacent(tri));

      const int revEdge = adj->getIndex(tri);
      ASSERT_GE(revEdge, 0);
      ASSERT_LT(revEdge, 3);
      EXPECT_EQ(adj->getAdjacent(revEdge), tri);

      // Shared edge endpoints must run in opposite directions for consistently
      // oriented triangles.
      EXPECT_TRUE(tri->getEdgeStart(edge).equals2D(adj->getEdgeEnd(revEdge)));
      EXPECT_TRUE(tri->getEdgeEnd(edge).equals2D(adj->getEdgeStart(revEdge)));
    }
  }
}

TEST(TriangulationBuilderTest, BuildsEmptyTriList) {
  TriList<Tri> tris;
  TriangulationBuilder::build(tris);
  EXPECT_TRUE(tris.empty());
  EXPECT_EQ(tris.size(), 0u);
  tris.validate();
}

TEST(TriangulationBuilderTest, SingleTriangleHasNoAdjacentNeighbors) {
  TriList<Tri> tris;
  tris.add(Coordinate(0.0, 0.0), Coordinate(0.0, 2.0), Coordinate(2.0, 0.0));

  TriangulationBuilder::build(tris);

  Tri* tri = tris[0];
  EXPECT_FALSE(tri->hasAdjacent());
  EXPECT_EQ(tri->numAdjacent(), 0);
  EXPECT_TRUE(tri->isBorder());
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(tri->getAdjacent(i), nullptr);
    EXPECT_TRUE(tri->isBoundary(i));
    EXPECT_FALSE(tri->isInteriorVertex(i));
  }
  VerifyAdjacencyConsistency(tris);
  tris.validate();
}

TEST(TriangulationBuilderTest, TrianglesSharingOnlyVertexAreNotAdjacent) {
  TriList<Tri> tris;
  // Two clockwise triangles meeting only at (0, 0).
  tris.add(Coordinate(-2.0, -1.0), Coordinate(-2.0, 1.0), Coordinate(0.0, 0.0));
  tris.add(Coordinate(0.0, 0.0), Coordinate(2.0, 1.0), Coordinate(2.0, -1.0));

  TriangulationBuilder::build(tris);

  EXPECT_EQ(tris[0]->numAdjacent(), 0);
  EXPECT_EQ(tris[1]->numAdjacent(), 0);
  EXPECT_FALSE(tris[0]->isAdjacent(tris[1]));
  EXPECT_FALSE(tris[1]->isAdjacent(tris[0]));
  VerifyAdjacencyConsistency(tris);
  tris.validate();
}

TEST(TriangulationBuilderTest, LinksAdjacentPairAcrossAllVertexRotations) {
  const std::array<Coordinate, 3> baseLeft = {
      Coordinate(0.0, 0.0), Coordinate(0.0, 1.0), Coordinate(1.0, 1.0)};
  const std::array<Coordinate, 3> baseRight = {
      Coordinate(0.0, 0.0), Coordinate(1.0, 1.0), Coordinate(1.0, 0.0)};

  // Shared edge is (0,0)-(1,1): index 2 in baseLeft, index 0 in baseRight.
  for (int rot0 = 0; rot0 < 3; ++rot0) {
    for (int rot1 = 0; rot1 < 3; ++rot1) {
      SCOPED_TRACE(rot0);
      SCOPED_TRACE(rot1);

      TriList<Tri> tris;
      tris.add(baseLeft[rot0], baseLeft[(rot0 + 1) % 3],
               baseLeft[(rot0 + 2) % 3]);
      tris.add(baseRight[rot1], baseRight[(rot1 + 1) % 3],
               baseRight[(rot1 + 2) % 3]);

      TriangulationBuilder::build(tris);

      const int expectedLeftEdge = (2 - rot0 + 3) % 3;
      const int expectedRightEdge = (0 - rot1 + 3) % 3;

      EXPECT_EQ(tris[0]->numAdjacent(), 1);
      EXPECT_EQ(tris[1]->numAdjacent(), 1);
      EXPECT_EQ(tris[0]->getAdjacent(expectedLeftEdge), tris[1]);
      EXPECT_EQ(tris[1]->getAdjacent(expectedRightEdge), tris[0]);
      EXPECT_EQ(tris[0]->getAdjacent((expectedLeftEdge + 1) % 3), nullptr);
      EXPECT_EQ(tris[0]->getAdjacent((expectedLeftEdge + 2) % 3), nullptr);
      EXPECT_EQ(tris[1]->getAdjacent((expectedRightEdge + 1) % 3), nullptr);
      EXPECT_EQ(tris[1]->getAdjacent((expectedRightEdge + 2) % 3), nullptr);

      VerifyAdjacencyConsistency(tris);
      tris.validate();
    }
  }
}

TEST(TriangulationBuilderTest,
     InteriorTriangleWithThreeNeighborsLinksWhenAddedFirstOrLast) {
  // Subdivide CW outer triangle (0,0)-(2,4)-(4,0) at edge midpoints.
  const Coordinate a(0.0, 0.0);
  const Coordinate b(2.0, 4.0);
  const Coordinate c(4.0, 0.0);
  const Coordinate mab(1.0, 2.0);
  const Coordinate mbc(3.0, 2.0);
  const Coordinate mca(2.0, 0.0);

  for (bool centerFirst : {false, true}) {
    SCOPED_TRACE(centerFirst);
    TriList<Tri> tris;
    if (centerFirst) {
      tris.add(mab, mbc, mca);  // center
    }
    tris.add(a, mab, mca);  // left
    tris.add(mab, b, mbc);  // top
    tris.add(mca, mbc, c);  // right
    if (!centerFirst) {
      tris.add(mab, mbc, mca);  // center
    }

    TriangulationBuilder::build(tris);

    Tri* center = centerFirst ? tris[0] : tris[3];
    Tri* left = centerFirst ? tris[1] : tris[0];
    Tri* top = centerFirst ? tris[2] : tris[1];
    Tri* right = centerFirst ? tris[3] : tris[2];

    EXPECT_EQ(center->numAdjacent(), 3);
    EXPECT_FALSE(center->isBorder());
    EXPECT_EQ(center->getAdjacent(0), top);
    EXPECT_EQ(center->getAdjacent(1), right);
    EXPECT_EQ(center->getAdjacent(2), left);

    EXPECT_EQ(left->numAdjacent(), 1);
    EXPECT_EQ(left->getAdjacent(1), center);
    EXPECT_EQ(top->numAdjacent(), 1);
    EXPECT_EQ(top->getAdjacent(2), center);
    EXPECT_EQ(right->numAdjacent(), 1);
    EXPECT_EQ(right->getAdjacent(0), center);

    VerifyAdjacencyConsistency(tris);
    tris.validate();
  }
}

TEST(TriangulationBuilderTest, WheelFanFormsInteriorVertex) {
  const Coordinate center(1.0, 1.0);
  TriList<Tri> tris;
  tris.add(Coordinate(0.0, 0.0), Coordinate(0.0, 2.0), center);  // West
  tris.add(Coordinate(0.0, 2.0), Coordinate(2.0, 2.0), center);  // North
  tris.add(Coordinate(2.0, 2.0), Coordinate(2.0, 0.0), center);  // East
  tris.add(Coordinate(2.0, 0.0), Coordinate(0.0, 0.0), center);  // South

  TriangulationBuilder::build(tris);

  for (std::size_t i = 0; i < tris.size(); ++i) {
    Tri* tri = tris[i];
    EXPECT_EQ(tri->numAdjacent(), 2);
    EXPECT_TRUE(tri->isBorder());
    EXPECT_TRUE(tri->isBoundary(0));
    EXPECT_FALSE(tri->isBoundary(1));
    EXPECT_FALSE(tri->isBoundary(2));
    EXPECT_TRUE(tri->isInteriorVertex(2));
    EXPECT_FALSE(tri->isInteriorVertex(0));
    EXPECT_FALSE(tri->isInteriorVertex(1));
  }
  VerifyAdjacencyConsistency(tris);
  tris.validate();
}

TEST(TriangulationBuilderTest,
     TwoByTwoGridSupportsSubsequentFlipAndValidation) {
  TriList<Tri> tris;
  for (int x = 0; x < 2; ++x) {
    for (int y = 0; y < 2; ++y) {
      const double dx = static_cast<double>(x);
      const double dy = static_cast<double>(y);
      const Coordinate bl(dx, dy);
      const Coordinate tl(dx, dy + 1.0);
      const Coordinate tr(dx + 1.0, dy + 1.0);
      const Coordinate br(dx + 1.0, dy);
      tris.add(bl, tl, tr);  // upper-left CW
      tris.add(bl, tr, br);  // lower-right CW
    }
  }

  TriangulationBuilder::build(tris);
  VerifyAdjacencyConsistency(tris);
  tris.validate();

  std::array<int, 4> countsByNumAdj = {0, 0, 0, 0};
  int centerInteriorCount = 0;
  const Coordinate gridCenter(1.0, 1.0);
  for (Tri* tri : tris) {
    const int n = tri->numAdjacent();
    ASSERT_GE(n, 0);
    ASSERT_LE(n, 3);
    ++countsByNumAdj[static_cast<std::size_t>(n)];
    const int idx = tri->getIndex(gridCenter);
    if (idx >= 0 && tri->isInteriorVertex(idx)) {
      ++centerInteriorCount;
    }
  }

  EXPECT_EQ(countsByNumAdj[0], 0);
  EXPECT_EQ(countsByNumAdj[1], 2);
  EXPECT_EQ(countsByNumAdj[2], 4);
  EXPECT_EQ(countsByNumAdj[3], 2);
  EXPECT_EQ(centerInteriorCount, 6);

  // Flip the diagonal edge (index 2) of the bottom-left upper triangle and
  // verify adjacency links remain consistent and valid.
  tris[0]->flip(2);
  VerifyAdjacencyConsistency(tris);
  tris.validate();
}

TEST(TriangulationBuilderTest, BuildsMultipleDisconnectedComponents) {
  TriList<Tri> tris;
  // Component A: unit square around origin.
  tris.add(Coordinate(0.0, 0.0), Coordinate(0.0, 1.0), Coordinate(1.0, 1.0));
  tris.add(Coordinate(0.0, 0.0), Coordinate(1.0, 1.0), Coordinate(1.0, 0.0));
  // Component B: translated square at (10, 0).
  tris.add(Coordinate(10.0, 0.0), Coordinate(10.0, 1.0), Coordinate(11.0, 1.0));
  tris.add(Coordinate(10.0, 0.0), Coordinate(11.0, 1.0), Coordinate(11.0, 0.0));
  // Component C: isolated triangle at (20, 0).
  tris.add(Coordinate(20.0, 0.0), Coordinate(20.0, 1.0), Coordinate(21.0, 0.0));

  TriangulationBuilder::build(tris);

  EXPECT_EQ(tris[0]->getAdjacent(2), tris[1]);
  EXPECT_EQ(tris[1]->getAdjacent(0), tris[0]);
  EXPECT_FALSE(tris[0]->isAdjacent(tris[2]));
  EXPECT_FALSE(tris[0]->isAdjacent(tris[3]));

  EXPECT_EQ(tris[2]->getAdjacent(2), tris[3]);
  EXPECT_EQ(tris[3]->getAdjacent(0), tris[2]);
  EXPECT_EQ(tris[4]->numAdjacent(), 0);

  VerifyAdjacencyConsistency(tris);
  tris.validate();
}

TEST(TriangulationBuilderTest, DefaultConstructorAndIncrementalAdd) {
  Tri t0(Coordinate(0.0, 0.0), Coordinate(0.0, 1.0), Coordinate(1.0, 1.0));
  Tri t1(Coordinate(0.0, 0.0), Coordinate(1.0, 1.0), Coordinate(1.0, 0.0));
  Tri t2(Coordinate(0.0, 1.0), Coordinate(0.0, 2.0), Coordinate(1.0, 1.0));
  Tri isolated(Coordinate(10.0, 10.0), Coordinate(10.0, 11.0),
               Coordinate(11.0, 10.0));

  TriangulationBuilder builder;
  builder.add(&t0);
  EXPECT_EQ(t0.numAdjacent(), 0);

  builder.add(&t1);
  EXPECT_EQ(t0.numAdjacent(), 1);
  EXPECT_EQ(t1.numAdjacent(), 1);
  EXPECT_EQ(t0.getAdjacent(2), &t1);
  EXPECT_EQ(t1.getAdjacent(0), &t0);

  builder.add(&t2);
  EXPECT_EQ(t0.numAdjacent(), 2);
  EXPECT_EQ(t0.getAdjacent(1), &t2);
  EXPECT_EQ(t2.numAdjacent(), 1);
  EXPECT_EQ(t2.getAdjacent(2), &t0);

  builder.add(&isolated);
  EXPECT_EQ(isolated.numAdjacent(), 0);

  t0.validate();
  t1.validate();
  t2.validate();
  isolated.validate();
}

TEST(TriangulationBuilderTest, ConstructorWithTriListAllowsFurtherAdditions) {
  TriList<Tri> tris;
  tris.add(Coordinate(0.0, 0.0), Coordinate(0.0, 1.0), Coordinate(1.0, 1.0));
  tris.add(Coordinate(0.0, 0.0), Coordinate(1.0, 1.0), Coordinate(1.0, 0.0));

  TriangulationBuilder builder(tris);
  EXPECT_EQ(tris[0]->getAdjacent(2), tris[1]);
  EXPECT_EQ(tris[1]->getAdjacent(0), tris[0]);

  Tri extra(Coordinate(1.0, 0.0), Coordinate(1.0, 1.0), Coordinate(2.0, 1.0));
  builder.add(&extra);

  EXPECT_EQ(tris[1]->numAdjacent(), 2);
  EXPECT_EQ(tris[1]->getAdjacent(1), &extra);
  EXPECT_EQ(extra.numAdjacent(), 1);
  EXPECT_EQ(extra.getAdjacent(0), tris[1]);

  tris.validate();
  extra.validate();
}

TEST(TriangulationBuilderTest, OverwritesPreexistingStaleAdjacencyLinks) {
  TriList<Tri> tris;
  tris.add(Coordinate(0.0, 0.0), Coordinate(0.0, 1.0), Coordinate(1.0, 1.0));
  tris.add(Coordinate(0.0, 0.0), Coordinate(1.0, 1.0), Coordinate(1.0, 0.0));
  tris.add(Coordinate(5.0, 5.0), Coordinate(5.0, 6.0), Coordinate(6.0, 5.0));

  Tri* t0 = tris[0];
  Tri* t1 = tris[1];
  Tri* isolated = tris[2];
  t0->setAdjacent(isolated, isolated, isolated);
  t1->setAdjacent(isolated, isolated, isolated);
  isolated->setAdjacent(t0, t1, t0);

  TriangulationBuilder::build(tris);

  EXPECT_EQ(t0->getAdjacent(0), nullptr);
  EXPECT_EQ(t0->getAdjacent(1), nullptr);
  EXPECT_EQ(t0->getAdjacent(2), t1);

  EXPECT_EQ(t1->getAdjacent(0), t0);
  EXPECT_EQ(t1->getAdjacent(1), nullptr);
  EXPECT_EQ(t1->getAdjacent(2), nullptr);

  EXPECT_EQ(isolated->numAdjacent(), 0);
  VerifyAdjacencyConsistency(tris);
  tris.validate();
}

TEST(TriangulationBuilderTest, MatchesSharedEdgeIgnoringZOrdinate) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  TriList<Tri> tris;
  tris.add(Coordinate(0.0, 0.0, 10.0), Coordinate(0.0, 1.0, 15.0),
           Coordinate(1.0, 1.0, 20.0));
  tris.add(Coordinate(0.0, 0.0, -5.0), Coordinate(1.0, 1.0, nan),
           Coordinate(1.0, 0.0, 3.0));

  TriangulationBuilder::build(tris);

  EXPECT_EQ(tris[0]->numAdjacent(), 1);
  EXPECT_EQ(tris[1]->numAdjacent(), 1);
  EXPECT_EQ(tris[0]->getAdjacent(2), tris[1]);
  EXPECT_EQ(tris[1]->getAdjacent(0), tris[0]);
  VerifyAdjacencyConsistency(tris);
  tris.validate();
}

TEST(TriangulationBuilderTest, LinksNegativeAndFractionalCoordinates) {
  TriList<Tri> tris;
  tris.add(Coordinate(-3.5, -2.25), Coordinate(-3.5, -0.75),
           Coordinate(-1.25, -0.75));
  tris.add(Coordinate(-3.5, -2.25), Coordinate(-1.25, -0.75),
           Coordinate(-1.25, -2.25));

  TriangulationBuilder::build(tris);

  EXPECT_EQ(tris[0]->getAdjacent(2), tris[1]);
  EXPECT_EQ(tris[1]->getAdjacent(0), tris[0]);
  VerifyAdjacencyConsistency(tris);
  tris.validate();
}

TEST(TriangulationBuilderTest, LinksConsistentlyCounterClockwiseTriangles) {
  TriList<Tri> tris;
  tris.add(Coordinate(0.0, 0.0), Coordinate(1.0, 1.0), Coordinate(0.0, 1.0));
  tris.add(Coordinate(0.0, 0.0), Coordinate(1.0, 0.0), Coordinate(1.0, 1.0));

  TriangulationBuilder::build(tris);

  EXPECT_EQ(tris[0]->numAdjacent(), 1);
  EXPECT_EQ(tris[1]->numAdjacent(), 1);
  EXPECT_EQ(tris[0]->getAdjacent(0), tris[1]);
  EXPECT_EQ(tris[1]->getAdjacent(2), tris[0]);
  VerifyAdjacencyConsistency(tris);
}

}  // namespace
}  // namespace geos_google
