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

#include "third_party/geos/include/geos/triangulate/IncrementalDelaunayTriangulator.h"

#include <array>
#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/triangulate/quadedge/LocateFailureException.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdge.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdgeLocator.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdgeSubdivision.h"
#include "third_party/geos/include/geos/triangulate/quadedge/Vertex.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::Envelope;
using geos::geom::GeometryFactory;
using geos::triangulate::IncrementalDelaunayTriangulator;
using geos::triangulate::quadedge::LocateFailureException;
using geos::triangulate::quadedge::QuadEdge;
using geos::triangulate::quadedge::QuadEdgeLocator;
using geos::triangulate::quadedge::QuadEdgeSubdivision;
using geos::triangulate::quadedge::Vertex;

class NullQuadEdgeLocator final : public QuadEdgeLocator {
 public:
  QuadEdge* locate(const Vertex& /*v*/) override { return nullptr; }
};

class IncrementalDelaunayTriangulatorTest : public ::testing::Test {
 protected:
  GeometryFactory::Ptr factory_ = GeometryFactory::create();
};

TEST_F(IncrementalDelaunayTriangulatorTest,
       InsertSiteCreatesTriangleAndReturnsIncidentEdge) {
  Envelope env{0.0, 10.0, 0.0, 10.0};
  QuadEdgeSubdivision subdiv{env, 0.0};
  IncrementalDelaunayTriangulator triangulator{&subdiv};

  const Vertex v1{0.0, 0.0};
  const Vertex v2{10.0, 0.0};
  const Vertex v3{0.0, 10.0};

  QuadEdge& e1 = triangulator.insertSite(v1);
  EXPECT_TRUE(e1.isLive());
  EXPECT_TRUE(e1.dest().equals(v1));

  QuadEdge& e2 = triangulator.insertSite(v2);
  EXPECT_TRUE(e2.isLive());
  EXPECT_TRUE(e2.dest().equals(v2));

  QuadEdge& e3 = triangulator.insertSite(v3);
  EXPECT_TRUE(e3.isLive());
  EXPECT_TRUE(e3.dest().equals(v3));

  auto tris = subdiv.getTriangles(*factory_);
  auto edges = subdiv.getEdges(*factory_);
  EXPECT_EQ(tris->getNumGeometries(), 1u);
  EXPECT_DOUBLE_EQ(tris->getArea(), 50.0);
  EXPECT_EQ(edges->getNumGeometries(), 3u);
}

TEST_F(IncrementalDelaunayTriangulatorTest,
       InsertSitesHandlesEmptyAndBatchVertices) {
  Envelope env{0.0, 10.0, 0.0, 10.0};
  QuadEdgeSubdivision subdiv{env, 0.0};
  IncrementalDelaunayTriangulator triangulator{&subdiv};

  IncrementalDelaunayTriangulator::VertexList empty_list;
  triangulator.insertSites(empty_list);
  EXPECT_EQ(subdiv.getTriangles(*factory_)->getNumGeometries(), 0u);
  EXPECT_EQ(subdiv.getEdges(*factory_)->getNumGeometries(), 0u);

  IncrementalDelaunayTriangulator::VertexList square{
      Vertex{0.0, 0.0}, Vertex{10.0, 0.0}, Vertex{10.0, 10.0},
      Vertex{0.0, 10.0}};
  triangulator.insertSites(square);

  auto tris = subdiv.getTriangles(*factory_);
  auto edges = subdiv.getEdges(*factory_);
  EXPECT_EQ(tris->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(tris->getArea(), 100.0);
  EXPECT_EQ(edges->getNumGeometries(), 5u);
}

TEST_F(IncrementalDelaunayTriangulatorTest,
       InsertSiteThrowsLocateFailureWhenLocatorReturnsNull) {
  Envelope env{0.0, 10.0, 0.0, 10.0};
  QuadEdgeSubdivision subdiv{env, 0.0};
  subdiv.setLocator(std::make_unique<NullQuadEdgeLocator>());

  IncrementalDelaunayTriangulator triangulator{&subdiv};
  EXPECT_THROW(triangulator.insertSite(Vertex{5.0, 5.0}),
               LocateFailureException);
}

TEST_F(IncrementalDelaunayTriangulatorTest,
       DuplicateAndNearVerticesIgnoredWithinTolerance) {
  {
    Envelope env{0.0, 10.0, 0.0, 10.0};
    QuadEdgeSubdivision subdiv{env, 0.25};
    IncrementalDelaunayTriangulator triangulator{&subdiv};

    triangulator.insertSites(
        {Vertex{0.0, 0.0}, Vertex{10.0, 0.0}, Vertex{0.0, 10.0}});

    const Vertex exact_dup{0.0, 0.0};
    QuadEdge& dup_edge = triangulator.insertSite(exact_dup);
    EXPECT_TRUE(subdiv.isVertexOfEdge(dup_edge, exact_dup));

    // Locate returns (0,0)->(0,10) for (0.0, 0.1) and (10,0)->(0,10) for
    // (0.02, 9.95) and (9.95, 0.02), so isVertexOfEdge matches an existing
    // endpoint within tolerance and avoids inserting a duplicate site.
    const Vertex near_origin{0.0, 0.1};
    QuadEdge& near_origin_edge = triangulator.insertSite(near_origin);
    EXPECT_TRUE(subdiv.isVertexOfEdge(near_origin_edge, near_origin));
    EXPECT_FALSE(near_origin_edge.orig().equals(near_origin));
    EXPECT_FALSE(near_origin_edge.dest().equals(near_origin));

    const Vertex near_dest{0.02, 9.95};
    QuadEdge& near_dest_edge = triangulator.insertSite(near_dest);
    EXPECT_TRUE(subdiv.isVertexOfEdge(near_dest_edge, near_dest));
    EXPECT_FALSE(near_dest_edge.orig().equals(near_dest));
    EXPECT_FALSE(near_dest_edge.dest().equals(near_dest));

    const Vertex near_right{9.95, 0.02};
    QuadEdge& near_right_edge = triangulator.insertSite(near_right);
    EXPECT_TRUE(subdiv.isVertexOfEdge(near_right_edge, near_right));
    EXPECT_FALSE(near_right_edge.orig().equals(near_right));
    EXPECT_FALSE(near_right_edge.dest().equals(near_right));

    EXPECT_EQ(subdiv.getTriangles(*factory_)->getNumGeometries(), 1u);
    EXPECT_EQ(subdiv.getEdges(*factory_)->getNumGeometries(), 3u);
  }

  // With zero tolerance, a distinct nearby interior site splits the triangle.
  {
    Envelope env{0.0, 10.0, 0.0, 10.0};
    QuadEdgeSubdivision subdiv{env, 0.0};
    IncrementalDelaunayTriangulator triangulator{&subdiv};

    triangulator.insertSites(
        {Vertex{0.0, 0.0}, Vertex{10.0, 0.0}, Vertex{0.0, 10.0}});
    triangulator.insertSite(Vertex{0.05, 0.1});

    auto tris = subdiv.getTriangles(*factory_);
    EXPECT_EQ(tris->getNumGeometries(), 3u);
    EXPECT_DOUBLE_EQ(tris->getArea(), 50.0);
    EXPECT_EQ(subdiv.getEdges(*factory_)->getNumGeometries(), 6u);
  }
}

TEST_F(IncrementalDelaunayTriangulatorTest,
       InsertSiteOnExistingEdgeReplacesEdgeAndSplitsFaces) {
  // Case 1: Point on the interior diagonal of a square.
  {
    Envelope env{0.0, 10.0, 0.0, 10.0};
    QuadEdgeSubdivision subdiv{env, 0.01};
    IncrementalDelaunayTriangulator triangulator{&subdiv};

    triangulator.insertSites({Vertex{0.0, 0.0}, Vertex{10.0, 0.0},
                              Vertex{10.0, 10.0}, Vertex{0.0, 10.0}});
    triangulator.insertSite(Vertex{5.0, 5.0});

    auto tris = subdiv.getTriangles(*factory_);
    auto edges = subdiv.getEdges(*factory_);
    EXPECT_EQ(tris->getNumGeometries(), 4u);
    EXPECT_DOUBLE_EQ(tris->getArea(), 100.0);
    EXPECT_EQ(edges->getNumGeometries(), 8u);
    EXPECT_NE(subdiv.locate(Coordinate{5.0, 5.0}, Coordinate{0.0, 0.0}),
              nullptr);
    EXPECT_NE(subdiv.locate(Coordinate{5.0, 5.0}, Coordinate{10.0, 0.0}),
              nullptr);
    EXPECT_NE(subdiv.locate(Coordinate{5.0, 5.0}, Coordinate{10.0, 10.0}),
              nullptr);
    EXPECT_NE(subdiv.locate(Coordinate{5.0, 5.0}, Coordinate{0.0, 10.0}),
              nullptr);
  }

  // Case 2: Point on the boundary edge of a triangle.
  {
    Envelope env{0.0, 10.0, 0.0, 10.0};
    QuadEdgeSubdivision subdiv{env, 0.01};
    IncrementalDelaunayTriangulator triangulator{&subdiv};

    triangulator.insertSites(
        {Vertex{0.0, 0.0}, Vertex{10.0, 0.0}, Vertex{5.0, 10.0}});
    triangulator.insertSite(Vertex{5.0, 0.0});

    auto tris = subdiv.getTriangles(*factory_);
    auto edges = subdiv.getEdges(*factory_);
    EXPECT_EQ(tris->getNumGeometries(), 2u);
    EXPECT_DOUBLE_EQ(tris->getArea(), 50.0);
    EXPECT_EQ(edges->getNumGeometries(), 5u);
    EXPECT_EQ(subdiv.locate(Coordinate{0.0, 0.0}, Coordinate{10.0, 0.0}),
              nullptr);
    EXPECT_NE(subdiv.locate(Coordinate{0.0, 0.0}, Coordinate{5.0, 0.0}),
              nullptr);
    EXPECT_NE(subdiv.locate(Coordinate{5.0, 0.0}, Coordinate{10.0, 0.0}),
              nullptr);
    EXPECT_NE(subdiv.locate(Coordinate{5.0, 0.0}, Coordinate{5.0, 10.0}),
              nullptr);
  }
}

TEST_F(IncrementalDelaunayTriangulatorTest,
       CircumcircleViolationFlipsNonDelaunayEdge) {
  // Triangle (0,0)-(4,0)-(0,4) has circumcenter (2,2) and radius sqrt(8).
  // Vertex (3,3) lies strictly inside that circumcircle across edge
  // (4,0)-(0,4), forcing a Delaunay swap to (0,0)-(3,3).
  Envelope env{0.0, 10.0, 0.0, 10.0};
  QuadEdgeSubdivision subdiv{env, 0.0};
  IncrementalDelaunayTriangulator triangulator{&subdiv};

  triangulator.insertSites(
      {Vertex{0.0, 0.0}, Vertex{4.0, 0.0}, Vertex{0.0, 4.0}, Vertex{3.0, 3.0}});

  auto tris = subdiv.getTriangles(*factory_);
  EXPECT_EQ(tris->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(tris->getArea(), 12.0);
  EXPECT_NE(subdiv.locate(Coordinate{0.0, 0.0}, Coordinate{3.0, 3.0}), nullptr);
  EXPECT_EQ(subdiv.locate(Coordinate{4.0, 0.0}, Coordinate{0.0, 4.0}), nullptr);
}

TEST_F(IncrementalDelaunayTriangulatorTest,
       ForceConvexRepairsConcaveBoundaryThatFalsePreserves) {
  // After inserting (0,50) and (50,49), inserting (100,50) lies strictly
  // outside the circumcircle of ((50,49), (0,50), top-frame), creating a
  // concave top boundary unless forceConvex(true) flips to edge
  // (0,50)-(100,50).
  const IncrementalDelaunayTriangulator::VertexList sites{
      Vertex{0.0, 50.0}, Vertex{50.0, 49.0}, Vertex{100.0, 50.0}};

  {
    Envelope env{0.0, 100.0, 0.0, 100.0};
    QuadEdgeSubdivision subdiv{env, 0.0};
    IncrementalDelaunayTriangulator triangulator{&subdiv};
    triangulator.forceConvex(true);
    triangulator.insertSites(sites);

    auto tris = subdiv.getTriangles(*factory_);
    EXPECT_EQ(tris->getNumGeometries(), 1u);
    EXPECT_DOUBLE_EQ(tris->getArea(), 50.0);
    EXPECT_NE(subdiv.locate(Coordinate{0.0, 50.0}, Coordinate{100.0, 50.0}),
              nullptr);
  }

  {
    Envelope env{0.0, 100.0, 0.0, 100.0};
    QuadEdgeSubdivision subdiv{env, 0.0};
    IncrementalDelaunayTriangulator triangulator{&subdiv};
    triangulator.forceConvex(false);
    triangulator.insertSites(sites);

    auto tris = subdiv.getTriangles(*factory_);
    EXPECT_EQ(tris->getNumGeometries(), 0u);
    EXPECT_EQ(subdiv.getEdges(*factory_)->getNumGeometries(), 2u);
    EXPECT_EQ(subdiv.locate(Coordinate{0.0, 50.0}, Coordinate{100.0, 50.0}),
              nullptr);
  }
}

TEST_F(IncrementalDelaunayTriangulatorTest,
       ForceConvexPreventsFlippingHullEdgeBetweenFrameAndInserted) {
  // Inserting (5, 0.01) inside triangle (0,0)-(10,0)-(5,10) puts it inside the
  // circumcircle of ((0,0), frame, (10,0)). forceConvex(true) keeps the
  // boundary edge (0,0)-(10,0) via isBetweenFrameAndInserted, whereas
  // forceConvex(false) flips it away to the frame vertex.
  const IncrementalDelaunayTriangulator::VertexList sites{
      Vertex{0.0, 0.0}, Vertex{10.0, 0.0}, Vertex{5.0, 10.0},
      Vertex{5.0, 0.01}};

  {
    Envelope env{0.0, 10.0, 0.0, 10.0};
    QuadEdgeSubdivision subdiv{env, 0.0};
    IncrementalDelaunayTriangulator triangulator{&subdiv};
    triangulator.forceConvex(true);
    triangulator.insertSites(sites);

    auto tris = subdiv.getTriangles(*factory_);
    EXPECT_EQ(tris->getNumGeometries(), 3u);
    EXPECT_DOUBLE_EQ(tris->getArea(), 50.0);
    EXPECT_NE(subdiv.locate(Coordinate{0.0, 0.0}, Coordinate{10.0, 0.0}),
              nullptr);
  }

  {
    Envelope env{0.0, 10.0, 0.0, 10.0};
    QuadEdgeSubdivision subdiv{env, 0.0};
    IncrementalDelaunayTriangulator triangulator{&subdiv};
    triangulator.forceConvex(false);
    triangulator.insertSites(sites);

    auto tris = subdiv.getTriangles(*factory_);
    EXPECT_EQ(tris->getNumGeometries(), 2u);
    EXPECT_EQ(subdiv.locate(Coordinate{0.0, 0.0}, Coordinate{10.0, 0.0}),
              nullptr);
  }
}

TEST_F(IncrementalDelaunayTriangulatorTest,
       ForceConvexHandlesAllFourBoundaryOrientations) {
  // Rotate the three-site concave boundary configuration around (50, 50) by
  // 0, 90, 180, and 270 degrees so boundary edges hit both frame-origin and
  // frame-destination checks in isConcaveBoundary.
  struct RotationCase {
    std::array<Coordinate, 3> pts;
  };
  const std::array<RotationCase, 4> cases{{
      {{Coordinate{0.0, 50.0}, Coordinate{50.0, 49.0},
        Coordinate{100.0, 50.0}}},
      {{Coordinate{50.0, 0.0}, Coordinate{51.0, 50.0},
        Coordinate{50.0, 100.0}}},
      {{Coordinate{100.0, 50.0}, Coordinate{50.0, 51.0},
        Coordinate{0.0, 50.0}}},
      {{Coordinate{50.0, 100.0}, Coordinate{49.0, 50.0},
        Coordinate{50.0, 0.0}}},
  }};

  for (const auto& c : cases) {
    for (const bool reverse : {false, true}) {
      Envelope env{0.0, 100.0, 0.0, 100.0};
      QuadEdgeSubdivision subdiv{env, 0.0};
      IncrementalDelaunayTriangulator triangulator{&subdiv};
      if (reverse) {
        triangulator.insertSites(
            {Vertex{c.pts[2]}, Vertex{c.pts[1]}, Vertex{c.pts[0]}});
      } else {
        triangulator.insertSites(
            {Vertex{c.pts[0]}, Vertex{c.pts[1]}, Vertex{c.pts[2]}});
      }

      auto tris = subdiv.getTriangles(*factory_);
      EXPECT_EQ(tris->getNumGeometries(), 1u);
      EXPECT_NEAR(tris->getArea(), 50.0, 1e-9);
      EXPECT_NE(subdiv.locate(c.pts[0], c.pts[2]), nullptr);
    }
  }
}

TEST_F(IncrementalDelaunayTriangulatorTest,
       CollinearSitesAndInteriorPentagonMaintainConvexHull) {
  {
    Envelope env{0.0, 10.0, 0.0, 10.0};
    QuadEdgeSubdivision subdiv{env, 0.01};
    IncrementalDelaunayTriangulator triangulator{&subdiv};

    triangulator.insertSites(
        {Vertex{0.0, 0.0}, Vertex{5.0, 0.0}, Vertex{10.0, 0.0}});
    EXPECT_EQ(subdiv.getTriangles(*factory_)->getNumGeometries(), 0u);
    EXPECT_EQ(subdiv.getEdges(*factory_)->getNumGeometries(), 2u);

    triangulator.insertSite(Vertex{5.0, 6.0});
    auto tris = subdiv.getTriangles(*factory_);
    EXPECT_EQ(tris->getNumGeometries(), 2u);
    EXPECT_DOUBLE_EQ(tris->getArea(), 30.0);
    EXPECT_EQ(subdiv.getEdges(*factory_)->getNumGeometries(), 5u);
  }

  {
    Envelope env{-2.0, 6.0, -1.0, 6.0};
    QuadEdgeSubdivision subdiv{env, 0.0};
    IncrementalDelaunayTriangulator triangulator{&subdiv};

    triangulator.insertSites({Vertex{0.0, 0.0}, Vertex{4.0, 0.0},
                              Vertex{5.0, 3.0}, Vertex{1.0, 5.0},
                              Vertex{-1.0, 2.0}});
    auto hull_tris = subdiv.getTriangles(*factory_);
    EXPECT_EQ(hull_tris->getNumGeometries(), 3u);
    EXPECT_DOUBLE_EQ(hull_tris->getArea(), 20.5);
    EXPECT_EQ(subdiv.getEdges(*factory_)->getNumGeometries(), 7u);

    triangulator.insertSite(Vertex{1.5, 2.2});
    auto full_tris = subdiv.getTriangles(*factory_);
    EXPECT_EQ(full_tris->getNumGeometries(), 5u);
    EXPECT_DOUBLE_EQ(full_tris->getArea(), 20.5);
    EXPECT_EQ(subdiv.getEdges(*factory_)->getNumGeometries(), 10u);
  }
}

}  // namespace
}  // namespace geos_google
