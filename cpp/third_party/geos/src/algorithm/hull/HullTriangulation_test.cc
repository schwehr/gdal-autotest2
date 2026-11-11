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

// Unit tests for third_party/geos/src/algorithm/hull/HullTriangulation.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/algorithm/hull/HullTriangulation.cpp

#include "third_party/geos/include/geos/algorithm/hull/HullTriangulation.h"

#include <array>
#include <deque>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/hull/HullTri.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdge.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdgeQuartet.h"
#include "third_party/geos/include/geos/triangulate/quadedge/Vertex.h"
#include "third_party/geos/include/geos/triangulate/tri/TriList.h"
#include "third_party/geos/include/geos/triangulate/tri/TriangulationBuilder.h"
#include "third_party/geos/include/geos/util/AssertionFailedException.h"
#include "third_party/geos/include/geos/util/IllegalStateException.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using geos::algorithm::hull::HullTri;
using geos::algorithm::hull::HullTriangulation;
using geos::geom::Coordinate;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GeometryTypeId;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::triangulate::quadedge::QuadEdge;
using geos::triangulate::quadedge::QuadEdgeQuartet;
using geos::triangulate::quadedge::Vertex;
using geos::triangulate::tri::TriangulationBuilder;
using geos::triangulate::tri::TriList;
using geos::util::AssertionFailedException;
using geos::util::IllegalStateException;
using geos::util::TopologyException;

void LinkTris(TriList<HullTri>& triList) {
  TriangulationBuilder tb;
  for (HullTri* tri : triList) {
    tb.add(tri);
  }
}

TEST(HullTriangulationTest, DefaultConstructorSucceeds) {
  [[maybe_unused]] HullTriangulation triangulation;
}

TEST(HullTriangulationTest, CreateDelaunayTriangulationDegenerateSites) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  {
    std::unique_ptr<Geometry> pt = reader.read("POINT (1 2)");
    TriList<HullTri> triList;
    HullTriangulation::createDelaunayTriangulation(pt.get(), triList);
    EXPECT_TRUE(triList.empty());
  }

  {
    std::unique_ptr<Geometry> two_pts =
        reader.read("MULTIPOINT ((0 0), (5 5))");
    TriList<HullTri> triList;
    HullTriangulation::createDelaunayTriangulation(two_pts.get(), triList);
    EXPECT_TRUE(triList.empty());
  }

  {
    std::unique_ptr<Geometry> collinear =
        reader.read("LINESTRING (0 0, 5 0, 10 0)");
    TriList<HullTri> triList;
    HullTriangulation::createDelaunayTriangulation(collinear.get(), triList);
    EXPECT_TRUE(triList.empty());
  }
}

TEST(HullTriangulationTest, CreateDelaunayTriangulationBuildsValidMeshes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  {
    std::unique_ptr<Geometry> three_pts =
        reader.read("MULTIPOINT ((0 0), (10 0), (0 10))");
    TriList<HullTri> triList;
    HullTriangulation::createDelaunayTriangulation(three_pts.get(), triList);
    EXPECT_EQ(triList.size(), 1u);
    EXPECT_NO_THROW(triList.validate());
    EXPECT_DOUBLE_EQ(triList.area(), 50.0);
  }

  {
    std::unique_ptr<Geometry> square_with_dup =
        reader.read("MULTIPOINT ((0 0), (0 10), (10 10), (10 0), (0 0))");
    TriList<HullTri> triList;
    HullTriangulation::createDelaunayTriangulation(square_with_dup.get(),
                                                   triList);
    EXPECT_EQ(triList.size(), 2u);
    EXPECT_NO_THROW(triList.validate());
    EXPECT_DOUBLE_EQ(triList.area(), 100.0);
  }

  {
    // 4x4 grid has 9 unit squares -> 18 triangles, including interior tris.
    std::unique_ptr<Geometry> grid = reader.read(
        "MULTIPOINT ((0 0), (1 0), (2 0), (3 0), "
        "(0 1), (1 1), (2 1), (3 1), "
        "(0 2), (1 2), (2 2), (3 2), "
        "(0 3), (1 3), (2 3), (3 3))");
    TriList<HullTri> triList;
    HullTriangulation::createDelaunayTriangulation(grid.get(), triList);
    EXPECT_EQ(triList.size(), 18u);
    EXPECT_NO_THROW(triList.validate());
    EXPECT_DOUBLE_EQ(triList.area(), 9.0);
  }

  {
    std::unique_ptr<Geometry> concave =
        reader.read("POLYGON ((0 0, 4 0, 4 3, 2 1, 0 3, 0 0))");
    TriList<HullTri> triList;
    HullTriangulation::createDelaunayTriangulation(concave.get(), triList);
    EXPECT_NO_THROW(triList.validate());
    EXPECT_DOUBLE_EQ(triList.area(), 12.0);
  }

  {
    std::unique_ptr<Geometry> multi = reader.read(
        "MULTIPOLYGON (((0 0, 0 2, 2 2, 2 0, 0 0)), "
        "((4 0, 4 2, 6 2, 6 0, 4 0)))");
    TriList<HullTri> triList;
    HullTriangulation::createDelaunayTriangulation(multi.get(), triList);
    EXPECT_FALSE(triList.empty());
    EXPECT_NO_THROW(triList.validate());
    EXPECT_DOUBLE_EQ(triList.area(), 12.0);
  }
}

TEST(HullTriangulationTest, HullTriVisitorOrientsCCWAndPreservesCW) {
  std::deque<QuadEdgeQuartet> edges;
  TriList<HullTri> triList;
  HullTriangulation::HullTriVisitor visitor(triList);

  // CCW triangle: visitor must flip p1 and p2 to make it clockwise.
  {
    const Coordinate p0(0, 0);
    const Coordinate p1(10, 0);
    const Coordinate p2(0, 10);
    std::array<QuadEdge*, 3> triEdges = {
        QuadEdge::makeEdge(Vertex(p0), Vertex(p1), edges),
        QuadEdge::makeEdge(Vertex(p1), Vertex(p2), edges),
        QuadEdge::makeEdge(Vertex(p2), Vertex(p0), edges),
    };
    visitor.visit(triEdges);
    ASSERT_EQ(triList.size(), 1u);
    EXPECT_EQ(triList[0]->getCoordinate(0), p0);
    EXPECT_EQ(triList[0]->getCoordinate(1), p2);
    EXPECT_EQ(triList[0]->getCoordinate(2), p1);
    EXPECT_NO_THROW(triList[0]->validate());
  }

  // CW triangle: hits the else branch and preserves vertex order (p0, p1, p2).
  {
    const Coordinate p0(0, 0);
    const Coordinate p1(0, 10);
    const Coordinate p2(10, 0);
    std::array<QuadEdge*, 3> triEdges = {
        QuadEdge::makeEdge(Vertex(p0), Vertex(p1), edges),
        QuadEdge::makeEdge(Vertex(p1), Vertex(p2), edges),
        QuadEdge::makeEdge(Vertex(p2), Vertex(p0), edges),
    };
    visitor.visit(triEdges);
    ASSERT_EQ(triList.size(), 2u);
    EXPECT_EQ(triList[1]->getCoordinate(0), p0);
    EXPECT_EQ(triList[1]->getCoordinate(1), p1);
    EXPECT_EQ(triList[1]->getCoordinate(2), p2);
    EXPECT_NO_THROW(triList[1]->validate());
  }

  // Collinear triple: isCCW is false -> takes the else branch.
  {
    const Coordinate p0(0, 0);
    const Coordinate p1(5, 0);
    const Coordinate p2(10, 0);
    std::array<QuadEdge*, 3> triEdges = {
        QuadEdge::makeEdge(Vertex(p0), Vertex(p1), edges),
        QuadEdge::makeEdge(Vertex(p1), Vertex(p2), edges),
        QuadEdge::makeEdge(Vertex(p2), Vertex(p0), edges),
    };
    visitor.visit(triEdges);
    ASSERT_EQ(triList.size(), 3u);
    EXPECT_DOUBLE_EQ(triList[2]->getArea(), 0.0);
  }
}

TEST(HullTriangulationTest, TraceBoundaryPolygonSingleTriangle) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  TriList<HullTri> triList;
  triList.add(Coordinate(0, 0), Coordinate(0, 10), Coordinate(10, 0));

  std::unique_ptr<Geometry> poly =
      HullTriangulation::traceBoundaryPolygon(triList, factory.get());
  ASSERT_NE(poly, nullptr);
  EXPECT_TRUE(poly->isValid());
  EXPECT_EQ(poly->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(poly->getArea(), 50.0);

  std::unique_ptr<Geometry> expected =
      reader.read("POLYGON ((0 0, 0 10, 10 0, 0 0))");
  EXPECT_TRUE(poly->equals(expected.get()));
}

TEST(HullTriangulationTest, TraceBoundaryPolygonEmptyThrowsAssertionFailed) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  TriList<HullTri> empty;

  EXPECT_THROW(HullTriangulation::traceBoundaryPolygon(empty, factory.get()),
               AssertionFailedException);
}

TEST(HullTriangulationTest,
     TraceBoundaryPolygonWithoutBorderTrisThrowsAssertionFailed) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  TriList<HullTri> borderless;
  borderless.add(Coordinate(0, 0), Coordinate(0, 4), Coordinate(4, 0));
  borderless.add(Coordinate(0, 0), Coordinate(4, 0), Coordinate(0, 4));
  borderless[0]->setAdjacent(borderless[1], borderless[1], borderless[1]);
  borderless[1]->setAdjacent(borderless[0], borderless[0], borderless[0]);

  EXPECT_THROW(
      HullTriangulation::traceBoundaryPolygon(borderless, factory.get()),
      AssertionFailedException);
}

TEST(HullTriangulationTest, TraceBoundaryPolygonMultiTriangleCases) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // 1. Two-triangle square where every triangle has 2 boundary edges.
  {
    TriList<HullTri> triList;
    triList.add(Coordinate(0, 0), Coordinate(0, 10), Coordinate(10, 10));
    triList.add(Coordinate(0, 0), Coordinate(10, 10), Coordinate(10, 0));
    LinkTris(triList);

    std::unique_ptr<Geometry> poly =
        HullTriangulation::traceBoundaryPolygon(triList, factory.get());
    std::unique_ptr<Geometry> expected =
        reader.read("POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))");
    ASSERT_NE(poly, nullptr);
    EXPECT_TRUE(poly->isValid());
    EXPECT_DOUBLE_EQ(poly->getArea(), 100.0);
    EXPECT_TRUE(poly->equals(expected.get()));
  }

  // 2. Four-triangle fan around center (1,1): each tri has 1 boundary edge.
  {
    TriList<HullTri> triList;
    const Coordinate center(1, 1);
    triList.add(Coordinate(0, 0), Coordinate(0, 2), center);
    triList.add(Coordinate(0, 2), Coordinate(2, 2), center);
    triList.add(Coordinate(2, 2), Coordinate(2, 0), center);
    triList.add(Coordinate(2, 0), Coordinate(0, 0), center);
    LinkTris(triList);

    std::unique_ptr<Geometry> poly =
        HullTriangulation::traceBoundaryPolygon(triList, factory.get());
    std::unique_ptr<Geometry> expected =
        reader.read("POLYGON ((0 0, 0 2, 2 2, 2 0, 0 0))");
    ASSERT_NE(poly, nullptr);
    EXPECT_TRUE(poly->isValid());
    EXPECT_DOUBLE_EQ(poly->getArea(), 4.0);
    EXPECT_TRUE(poly->equals(expected.get()));
  }

  // 3. First triangle in triList is interior, forcing findBorderTri to skip it.
  {
    TriList<HullTri> triList;
    const Coordinate a(0, 0);
    const Coordinate b(2, 4);
    const Coordinate c(4, 0);
    const Coordinate m_ab(1, 2);
    const Coordinate m_bc(3, 2);
    const Coordinate m_ca(2, 0);
    triList.add(m_ab, m_bc, m_ca);  // Interior tri placed at index 0.
    triList.add(m_ab, b, m_bc);
    triList.add(m_bc, c, m_ca);
    triList.add(a, m_ab, m_ca);
    LinkTris(triList);
    ASSERT_FALSE(triList[0]->isBorder());

    std::unique_ptr<Geometry> poly =
        HullTriangulation::traceBoundaryPolygon(triList, factory.get());
    std::unique_ptr<Geometry> expected =
        reader.read("POLYGON ((0 0, 2 4, 4 0, 0 0))");
    ASSERT_NE(poly, nullptr);
    EXPECT_TRUE(poly->isValid());
    EXPECT_DOUBLE_EQ(poly->getArea(), 8.0);
    EXPECT_TRUE(poly->equals(expected.get()));
  }

  // 4. Delaunay triangulation of a 4x4 grid -> square [0,3]x[0,3].
  {
    std::unique_ptr<Geometry> grid = reader.read(
        "MULTIPOINT ((0 0), (1 0), (2 0), (3 0), "
        "(0 1), (1 1), (2 1), (3 1), "
        "(0 2), (1 2), (2 2), (3 2), "
        "(0 3), (1 3), (2 3), (3 3))");
    TriList<HullTri> triList;
    HullTriangulation::createDelaunayTriangulation(grid.get(), triList);

    std::unique_ptr<Geometry> poly =
        HullTriangulation::traceBoundaryPolygon(triList, factory.get());
    std::unique_ptr<Geometry> expected =
        reader.read("POLYGON ((0 0, 0 3, 3 3, 3 0, 0 0))");
    ASSERT_NE(poly, nullptr);
    EXPECT_TRUE(poly->isValid());
    EXPECT_DOUBLE_EQ(poly->getArea(), 9.0);
    EXPECT_TRUE(poly->equals(expected.get()));
  }
}

TEST(HullTriangulationTest, NextBorderTriScansFanAndThrowsOnSelfAdjacency) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Three-triangle fan sharing vertex V=(0,0); stepping from T2 back to T0
  // scans across T1 around V.
  TriList<HullTri> triList;
  const Coordinate v(0, 0);
  triList.add(v, Coordinate(0, 3), Coordinate(2, 2));   // T0
  triList.add(v, Coordinate(2, 2), Coordinate(3, 0));   // T1
  triList.add(v, Coordinate(3, 0), Coordinate(2, -2));  // T2
  LinkTris(triList);
  triList.validate();

  EXPECT_EQ(HullTriangulation::nextBorderTri(triList[0]), triList[1]);
  EXPECT_EQ(HullTriangulation::nextBorderTri(triList[1]), triList[2]);
  EXPECT_EQ(HullTriangulation::nextBorderTri(triList[2]), triList[0]);

  std::unique_ptr<Geometry> poly =
      HullTriangulation::traceBoundaryPolygon(triList, factory.get());
  std::unique_ptr<Geometry> expected =
      reader.read("POLYGON ((0 0, 0 3, 2 2, 3 0, 2 -2, 0 0))");
  ASSERT_NE(poly, nullptr);
  EXPECT_TRUE(poly->isValid());
  EXPECT_DOUBLE_EQ(poly->getArea(), 9.0);
  EXPECT_TRUE(poly->equals(expected.get()));

  // Malformed self-adjacent triangle triggers IllegalStateException.
  HullTri bad(Coordinate(0, 0), Coordinate(0, 1), Coordinate(1, 0));
  bad.setAdjacent(&bad, nullptr, nullptr);
  EXPECT_THROW(HullTriangulation::nextBorderTri(&bad), IllegalStateException);
}

TEST(HullTriangulationTest,
     GeomunionHandlesEmptySingleConnectedHoleDisconnectedAndOverlap) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // 1. Empty TriList -> empty Geometry.
  {
    TriList<HullTri> empty;
    std::unique_ptr<Geometry> result =
        HullTriangulation::geomunion(empty, factory.get());
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->isEmpty());
  }

  // 2. Single triangle -> Polygon.
  {
    TriList<HullTri> single;
    single.add(Coordinate(0, 0), Coordinate(0, 4), Coordinate(3, 0));
    std::unique_ptr<Geometry> result =
        HullTriangulation::geomunion(single, factory.get());
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->isValid());
    EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
    EXPECT_DOUBLE_EQ(result->getArea(), 6.0);
  }

  // 3. Connected 4-tri fan without holes -> equals traceBoundaryPolygon.
  {
    TriList<HullTri> fan;
    const Coordinate center(1, 1);
    fan.add(Coordinate(0, 0), Coordinate(0, 2), center);
    fan.add(Coordinate(0, 2), Coordinate(2, 2), center);
    fan.add(Coordinate(2, 2), Coordinate(2, 0), center);
    fan.add(Coordinate(2, 0), Coordinate(0, 0), center);
    LinkTris(fan);

    std::unique_ptr<Geometry> unioned =
        HullTriangulation::geomunion(fan, factory.get());
    std::unique_ptr<Geometry> traced =
        HullTriangulation::traceBoundaryPolygon(fan, factory.get());
    ASSERT_NE(unioned, nullptr);
    EXPECT_TRUE(unioned->isValid());
    EXPECT_TRUE(unioned->equals(traced.get()));
  }

  // 4. Eight-triangle square annulus [0,3]x[0,3] minus [1,2]x[1,2] -> Polygon
  // with 1 hole.
  {
    TriList<HullTri> annulus;
    const Coordinate o0(0, 0);
    const Coordinate o1(0, 3);
    const Coordinate o2(3, 3);
    const Coordinate o3(3, 0);
    const Coordinate i0(1, 1);
    const Coordinate i1(1, 2);
    const Coordinate i2(2, 2);
    const Coordinate i3(2, 1);

    annulus.add(o0, o1, i1);
    annulus.add(o0, i1, i0);
    annulus.add(o1, o2, i2);
    annulus.add(o1, i2, i1);
    annulus.add(o2, o3, i3);
    annulus.add(o2, i3, i2);
    annulus.add(o3, o0, i0);
    annulus.add(o3, i0, i3);
    LinkTris(annulus);
    ASSERT_NO_THROW(annulus.validate());

    std::unique_ptr<Geometry> result =
        HullTriangulation::geomunion(annulus, factory.get());
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->isValid());
    EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
    const auto* poly = dynamic_cast<const Polygon*>(result.get());
    ASSERT_NE(poly, nullptr);
    EXPECT_EQ(poly->getNumInteriorRing(), 1u);
    EXPECT_NEAR(result->getArea(), 8.0, 1e-12);

    std::unique_ptr<Geometry> expected = reader.read(
        "POLYGON ((0 0, 0 3, 3 3, 3 0, 0 0), (1 1, 2 1, 2 2, 1 2, 1 1))");
    EXPECT_TRUE(result->equals(expected.get()));
  }

  // 5. Two disconnected triangles -> MultiPolygon.
  {
    TriList<HullTri> disjoint;
    disjoint.add(Coordinate(0, 0), Coordinate(0, 2), Coordinate(2, 0));
    disjoint.add(Coordinate(10, 0), Coordinate(10, 2), Coordinate(12, 0));

    std::unique_ptr<Geometry> result =
        HullTriangulation::geomunion(disjoint, factory.get());
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->isValid());
    EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_MULTIPOLYGON);
    EXPECT_EQ(result->getNumGeometries(), 2u);
    EXPECT_DOUBLE_EQ(result->getArea(), 4.0);
  }

  // 6. Duplicate and edge-overlapping triangles violate coverage invariants ->
  // TopologyException.
  {
    TriList<HullTri> duplicate;
    duplicate.add(Coordinate(0, 0), Coordinate(0, 4), Coordinate(4, 0));
    duplicate.add(Coordinate(0, 0), Coordinate(0, 4), Coordinate(4, 0));
    EXPECT_THROW(HullTriangulation::geomunion(duplicate, factory.get()),
                 TopologyException);

    TriList<HullTri> edge_overlap;
    edge_overlap.add(Coordinate(0, 0), Coordinate(0, 4), Coordinate(4, 0));
    edge_overlap.add(Coordinate(0, 0), Coordinate(0, 4), Coordinate(2, 0));
    EXPECT_THROW(HullTriangulation::geomunion(edge_overlap, factory.get()),
                 TopologyException);
  }
}

}  // namespace
}  // namespace geos_google
