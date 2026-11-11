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
// third_party/geos/src/triangulate/quadedge/QuadEdgeSubdivision.cpp.

#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdgeSubdivision.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/Triangle.h"
#include "third_party/geos/include/geos/triangulate/IncrementalDelaunayTriangulator.h"
#include "third_party/geos/include/geos/triangulate/quadedge/LocateFailureException.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdge.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdgeLocator.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdgeQuartet.h"
#include "third_party/geos/include/geos/triangulate/quadedge/TriangleVisitor.h"
#include "third_party/geos/include/geos/triangulate/quadedge/Vertex.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateXY;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::geom::MultiLineString;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::Triangle;
using geos::triangulate::IncrementalDelaunayTriangulator;
using geos::triangulate::quadedge::LocateFailureException;
using geos::triangulate::quadedge::QuadEdge;
using geos::triangulate::quadedge::QuadEdgeLocator;
using geos::triangulate::quadedge::QuadEdgeQuartet;
using geos::triangulate::quadedge::QuadEdgeSubdivision;
using geos::triangulate::quadedge::TriangleVisitor;
using geos::triangulate::quadedge::Vertex;
using geos::util::IllegalArgumentException;
using ::testing::SizeIs;
using ::testing::UnorderedElementsAreArray;

class RecordingTriangleVisitor : public TriangleVisitor {
 public:
  void visit(std::array<QuadEdge*, 3>& tri_edges) override {
    visited_edges.push_back(tri_edges);
    visited_coords.push_back({
        tri_edges[0]->orig().getCoordinate(),
        tri_edges[1]->orig().getCoordinate(),
        tri_edges[2]->orig().getCoordinate(),
    });
  }

  std::vector<std::array<QuadEdge*, 3>> visited_edges;
  std::vector<std::array<CoordinateXY, 3>> visited_coords;
};

class StubQuadEdgeLocator : public QuadEdgeLocator {
 public:
  explicit StubQuadEdgeLocator(QuadEdge* result) : result_(result) {}

  QuadEdge* locate(const Vertex& /*v*/) override {
    ++call_count_;
    return result_;
  }

  int call_count() const { return call_count_; }

 private:
  QuadEdge* result_;
  int call_count_ = 0;
};

void PopulateDelaunay(QuadEdgeSubdivision& subdiv,
                      const std::vector<CoordinateXY>& coords,
                      bool force_convex = true) {
  std::vector<Vertex> vertices;
  vertices.reserve(coords.size());
  for (const CoordinateXY& c : coords) {
    vertices.emplace_back(c.x, c.y);
  }
  std::sort(vertices.begin(), vertices.end());

  IncrementalDelaunayTriangulator triangulator(&subdiv);
  triangulator.forceConvex(force_convex);
  triangulator.insertSites(vertices);
}

TEST(QuadEdgeSubdivisionTest, NullEnvelopeThrows) {
  const Envelope empty_env;
  ASSERT_TRUE(empty_env.isNull());
  EXPECT_THROW(QuadEdgeSubdivision(empty_env, 0.0), IllegalArgumentException);
}

TEST(QuadEdgeSubdivisionTest,
     ConstructorCreatesFrameForWideTallAndPointEnvelopes) {
  {
    const Envelope wide_env(0.0, 20.0, 0.0, 4.0);
    auto subdiv = std::make_unique<QuadEdgeSubdivision>(wide_env, 1e-3);

    EXPECT_DOUBLE_EQ(subdiv->getTolerance(), 1e-3);
    EXPECT_TRUE(subdiv->getEnvelope().contains(wide_env));
    EXPECT_DOUBLE_EQ(subdiv->getEnvelope().getMinX(), -200.0);
    EXPECT_DOUBLE_EQ(subdiv->getEnvelope().getMaxX(), 220.0);
    EXPECT_DOUBLE_EQ(subdiv->getEnvelope().getMinY(), -200.0);
    EXPECT_DOUBLE_EQ(subdiv->getEnvelope().getMaxY(), 204.0);
    EXPECT_THAT(subdiv->getEdges(), SizeIs(3));
    EXPECT_THAT(*subdiv->getPrimaryEdges(true), SizeIs(3));
    EXPECT_TRUE(subdiv->getPrimaryEdges(false)->empty());
    EXPECT_THAT(*subdiv->getVertexUniqueEdges(true), SizeIs(3));
    EXPECT_TRUE(subdiv->getVertexUniqueEdges(false)->empty());
  }

  {
    const Envelope tall_env(2.0, 6.0, 10.0, 30.0);
    QuadEdgeSubdivision subdiv(tall_env, 0.0);

    EXPECT_DOUBLE_EQ(subdiv.getTolerance(), 0.0);
    EXPECT_TRUE(subdiv.getEnvelope().contains(tall_env));
    EXPECT_DOUBLE_EQ(subdiv.getEnvelope().getMinX(), -198.0);
    EXPECT_DOUBLE_EQ(subdiv.getEnvelope().getMaxX(), 206.0);
    EXPECT_DOUBLE_EQ(subdiv.getEnvelope().getMinY(), -190.0);
    EXPECT_DOUBLE_EQ(subdiv.getEnvelope().getMaxY(), 230.0);
  }

  {
    const Envelope point_env(CoordinateXY(3.0, 4.0));
    QuadEdgeSubdivision subdiv(point_env, 0.0);

    EXPECT_DOUBLE_EQ(subdiv.getEnvelope().getMinX(), 3.0);
    EXPECT_DOUBLE_EQ(subdiv.getEnvelope().getMaxX(), 3.0);
    EXPECT_DOUBLE_EQ(subdiv.getEnvelope().getMinY(), 4.0);
    EXPECT_DOUBLE_EQ(subdiv.getEnvelope().getMaxY(), 4.0);
  }
}

TEST(QuadEdgeSubdivisionTest,
     GetTriangleEdgesReturnsTriangleAndThrowsOtherwise) {
  QuadEdgeSubdivision subdiv(Envelope(0.0, 10.0, 0.0, 10.0), 0.0);
  auto frame_edges = subdiv.getPrimaryEdges(true);
  ASSERT_THAT(*frame_edges, SizeIs(3));

  const QuadEdge* tri_edges[3] = {nullptr, nullptr, nullptr};
  QuadEdgeSubdivision::getTriangleEdges(*frame_edges->at(0), tri_edges);
  EXPECT_EQ(tri_edges[0], frame_edges->at(0));
  EXPECT_EQ(tri_edges[1], &tri_edges[0]->lNext());
  EXPECT_EQ(tri_edges[2], &tri_edges[1]->lNext());
  EXPECT_EQ(&tri_edges[2]->lNext(), tri_edges[0]);
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_TRUE(tri_edges[i]->dest().equals(tri_edges[(i + 1) % 3]->orig()));
  }

  // An isolated edge has a 2-edge left face and must fail the triangle check.
  std::deque<QuadEdgeQuartet> standalone_edges;
  QuadEdge* isolated =
      QuadEdge::makeEdge(Vertex(0.0, 0.0), Vertex(1.0, 0.0), standalone_edges);
  EXPECT_THROW(QuadEdgeSubdivision::getTriangleEdges(*isolated, tri_edges),
               IllegalArgumentException);

  // Removing a spoke merges two adjacent triangles into a 4-edge face.
  QuadEdge& spoke = subdiv.insertSite(Vertex(5.0, 5.0));
  QuadEdge& merged_face_edge = spoke.lNext();
  subdiv.remove(spoke);
  EXPECT_THROW(
      QuadEdgeSubdivision::getTriangleEdges(merged_face_edge, tri_edges),
      IllegalArgumentException);
}

TEST(QuadEdgeSubdivisionTest, MakeEdgeConnectAndRemove) {
  QuadEdgeSubdivision subdiv(Envelope(0.0, 10.0, 0.0, 10.0), 0.0);
  ASSERT_THAT(subdiv.getEdges(), SizeIs(3));

  QuadEdge& e1 = subdiv.makeEdge(Vertex(1.0, 1.0), Vertex(4.0, 1.0));
  QuadEdge& e2 = subdiv.makeEdge(Vertex(4.0, 4.0), Vertex(1.0, 4.0));
  EXPECT_THAT(subdiv.getEdges(), SizeIs(5));
  EXPECT_TRUE(e1.isLive());
  EXPECT_TRUE(e1.orig().equals(Vertex(1.0, 1.0)));
  EXPECT_TRUE(e1.dest().equals(Vertex(4.0, 1.0)));

  QuadEdge& e3 = subdiv.connect(e1, e2);
  EXPECT_THAT(subdiv.getEdges(), SizeIs(6));
  EXPECT_TRUE(e3.isLive());
  EXPECT_TRUE(e3.orig().equals(e1.dest()));
  EXPECT_TRUE(e3.dest().equals(e2.orig()));
  EXPECT_EQ(&e1.lNext(), &e3);
  EXPECT_EQ(&e3.lNext(), &e2);

  subdiv.remove(e3);
  EXPECT_FALSE(e3.isLive());
  EXPECT_FALSE(e3.sym().isLive());
  EXPECT_TRUE(e1.isLive());
  EXPECT_TRUE(e2.isLive());
  EXPECT_NE(&e1.lNext(), &e3);
  // Deque entries are preserved so existing QuadEdge pointers remain stable.
  EXPECT_THAT(subdiv.getEdges(), SizeIs(6));
}

TEST(QuadEdgeSubdivisionTest,
     LocateFromEdgeCoversWalkBranchesAndThrowsOnFailure) {
  QuadEdgeSubdivision subdiv(Envelope(0.0, 10.0, 0.0, 10.0), 0.0);
  auto initial_edges = subdiv.getPrimaryEdges(true);
  ASSERT_THAT(*initial_edges, SizeIs(3));
  const QuadEdge& dummy_start = *initial_edges->at(0);

  // Frame vertices for Envelope(0,10,0,10) are (5, 110), (-100, -100), (110,
  // -100).
  const Vertex frame_top(5.0, 110.0);
  const Vertex frame_bottom_left(-100.0, -100.0);

  QuadEdge* at_orig = subdiv.locateFromEdge(frame_top, dummy_start);
  ASSERT_NE(at_orig, nullptr);
  EXPECT_TRUE(at_orig->orig().equals(frame_top));

  QuadEdge* at_dest = subdiv.locateFromEdge(frame_bottom_left, dummy_start);
  ASSERT_NE(at_dest, nullptr);
  EXPECT_TRUE(at_dest->dest().equals(frame_bottom_left));

  // Query outside the starting edge flips to sym() via rightOf(), then fails
  // to converge outside the finite frame triangle.
  EXPECT_THROW(subdiv.locateFromEdge(Vertex(-500.0, 0.0), dummy_start),
               LocateFailureException);
  // Query far below the bottom frame edge walks via dPrev() and oNext() before
  // exceeding maxIter.
  EXPECT_THROW(subdiv.locateFromEdge(Vertex(5.0, -10000.0), dummy_start),
               LocateFailureException);

  // Populate multiple interior triangles and verify locate() /
  // locateFromEdge().
  PopulateDelaunay(subdiv, {
                               CoordinateXY(2.0, 2.0),
                               CoordinateXY(8.0, 2.0),
                               CoordinateXY(8.0, 8.0),
                               CoordinateXY(2.0, 8.0),
                               CoordinateXY(5.0, 5.0),
                           });

  const std::vector<Coordinate> queries = {
      Coordinate(5.0, 5.0), Coordinate(3.0, 5.0), Coordinate(7.0, 5.0),
      Coordinate(5.0, 1.0), Coordinate(5.0, 9.0),
  };
  for (const Coordinate& q : queries) {
    QuadEdge* e_v = subdiv.locate(Vertex(q));
    QuadEdge* e_c = subdiv.locate(q);
    QuadEdge* e_from = subdiv.locateFromEdge(Vertex(q), dummy_start);
    ASSERT_NE(e_v, nullptr);
    EXPECT_EQ(e_v, e_c);
    EXPECT_EQ(e_v, e_from);

    const QuadEdge* tri[3] = {nullptr, nullptr, nullptr};
    QuadEdgeSubdivision::getTriangleEdges(*e_v, tri);
    EXPECT_TRUE(Triangle::intersects(tri[0]->orig().getCoordinate(),
                                     tri[1]->orig().getCoordinate(),
                                     tri[2]->orig().getCoordinate(), q));
  }
}

TEST(QuadEdgeSubdivisionTest, LocateEdgeByCoordinatePairAndCustomLocator) {
  QuadEdgeSubdivision subdiv(Envelope(0.0, 10.0, 0.0, 10.0), 0.0);
  PopulateDelaunay(subdiv, {
                               CoordinateXY(0.0, 0.0),
                               CoordinateXY(10.0, 0.0),
                               CoordinateXY(10.0, 10.0),
                               CoordinateXY(0.0, 10.0),
                               CoordinateXY(5.0, 5.0),
                           });

  QuadEdge* forward = subdiv.locate(Coordinate(5.0, 5.0), Coordinate(0.0, 0.0));
  ASSERT_NE(forward, nullptr);
  EXPECT_TRUE(forward->orig().equals(Vertex(5.0, 5.0)));
  EXPECT_TRUE(forward->dest().equals(Vertex(0.0, 0.0)));

  QuadEdge* reverse = subdiv.locate(Coordinate(0.0, 0.0), Coordinate(5.0, 5.0));
  ASSERT_NE(reverse, nullptr);
  EXPECT_TRUE(reverse->orig().equals(Vertex(0.0, 0.0)));
  EXPECT_TRUE(reverse->dest().equals(Vertex(5.0, 5.0)));
  EXPECT_EQ(reverse, &forward->sym());

  // Opposite corners of the square are separated by center (5, 5).
  EXPECT_EQ(subdiv.locate(Coordinate(0.0, 0.0), Coordinate(10.0, 10.0)),
            nullptr);
  EXPECT_EQ(subdiv.locate(Coordinate(5.0, 5.0), Coordinate(42.0, 42.0)),
            nullptr);

  // When a custom locator returns an edge ending at p0, locate(p0, p1)
  // normalizes via sym() before scanning around p0.
  auto dest_locator = std::make_unique<StubQuadEdgeLocator>(reverse);
  const auto* dest_locator_ptr = dest_locator.get();
  subdiv.setLocator(std::move(dest_locator));
  QuadEdge* via_dest =
      subdiv.locate(Coordinate(5.0, 5.0), Coordinate(10.0, 0.0));
  ASSERT_NE(via_dest, nullptr);
  EXPECT_TRUE(via_dest->orig().equals(Vertex(5.0, 5.0)));
  EXPECT_TRUE(via_dest->dest().equals(Vertex(10.0, 0.0)));
  EXPECT_EQ(dest_locator_ptr->call_count(), 1);

  // When a custom locator returns an edge starting at p0, it scans directly.
  auto orig_locator = std::make_unique<StubQuadEdgeLocator>(forward);
  const auto* orig_locator_ptr = orig_locator.get();
  subdiv.setLocator(std::move(orig_locator));
  QuadEdge* via_orig =
      subdiv.locate(Coordinate(5.0, 5.0), Coordinate(0.0, 10.0));
  ASSERT_NE(via_orig, nullptr);
  EXPECT_TRUE(via_orig->orig().equals(Vertex(5.0, 5.0)));
  EXPECT_TRUE(via_orig->dest().equals(Vertex(0.0, 10.0)));
  EXPECT_EQ(orig_locator_ptr->call_count(), 1);

  // When the locator returns nullptr, locate(p0, p1) returns nullptr.
  auto null_locator = std::make_unique<StubQuadEdgeLocator>(nullptr);
  const auto* null_locator_ptr = null_locator.get();
  subdiv.setLocator(std::move(null_locator));
  EXPECT_EQ(subdiv.locate(Vertex(5.0, 5.0)), nullptr);
  EXPECT_EQ(subdiv.locate(Coordinate(5.0, 5.0)), nullptr);
  EXPECT_EQ(subdiv.locate(Coordinate(5.0, 5.0), Coordinate(0.0, 0.0)), nullptr);
  EXPECT_EQ(null_locator_ptr->call_count(), 3);
}

TEST(QuadEdgeSubdivisionTest, InsertSiteRespectsToleranceAndDuplicates) {
  QuadEdgeSubdivision subdiv(Envelope(0.0, 10.0, 0.0, 10.0), 0.1);
  ASSERT_THAT(subdiv.getEdges(), SizeIs(3));

  QuadEdge& first = subdiv.insertSite(Vertex(5.0, 5.0));
  EXPECT_TRUE(first.dest().equals(Vertex(5.0, 5.0)));
  EXPECT_THAT(subdiv.getEdges(), SizeIs(6));

  // Exact duplicate is ignored.
  QuadEdge& exact_dup = subdiv.insertSite(Vertex(5.0, 5.0));
  EXPECT_TRUE(exact_dup.dest().equals(Vertex(5.0, 5.0)));
  EXPECT_THAT(subdiv.getEdges(), SizeIs(6));

  // Near dest() within tolerance (distance 0.04 < 0.1) is ignored.
  QuadEdge& near_dest = subdiv.insertSite(Vertex(5.04, 5.0));
  EXPECT_TRUE(near_dest.dest().equals(Vertex(5.0, 5.0)));
  EXPECT_THAT(subdiv.getEdges(), SizeIs(6));

  // Near orig() of edge (5, 110)->(5, 5) within tolerance (0.05 < 0.1) is
  // ignored.
  QuadEdge& near_orig = subdiv.insertSite(Vertex(5.0, 109.95));
  EXPECT_TRUE(near_orig.orig().equals(Vertex(5.0, 110.0)));
  EXPECT_THAT(subdiv.getEdges(), SizeIs(6));

  // Beyond tolerance (distance 1.0 >= 0.1) inserts 3 new edges.
  QuadEdge& distinct = subdiv.insertSite(Vertex(6.0, 5.0));
  EXPECT_TRUE(distinct.dest().equals(Vertex(6.0, 5.0)));
  EXPECT_THAT(subdiv.getEdges(), SizeIs(9));

  // Distance equal to tolerance is strictly outside (`distance < tolerance`).
  QuadEdgeSubdivision exact_tol_subdiv(Envelope(0.0, 10.0, 0.0, 10.0), 0.25);
  exact_tol_subdiv.insertSite(Vertex(5.0, 5.0));
  ASSERT_THAT(exact_tol_subdiv.getEdges(), SizeIs(6));
  QuadEdge& at_boundary = exact_tol_subdiv.insertSite(Vertex(5.25, 5.0));
  EXPECT_TRUE(at_boundary.dest().equals(Vertex(5.25, 5.0)));
  EXPECT_THAT(exact_tol_subdiv.getEdges(), SizeIs(9));
}

TEST(QuadEdgeSubdivisionTest, FrameAndEdgePredicates) {
  QuadEdgeSubdivision subdiv(Envelope(0.0, 10.0, 0.0, 10.0), 1.0);
  auto frame_unique = subdiv.getVertexUniqueEdges(true);
  ASSERT_THAT(*frame_unique, SizeIs(3));
  for (const QuadEdge* qe : *frame_unique) {
    EXPECT_TRUE(subdiv.isFrameVertex(qe->orig()));
  }
  EXPECT_FALSE(subdiv.isFrameVertex(Vertex(5.0, 3.0)));

  // Use an asymmetric triangle + interior point to avoid cocircularity with
  // frame.
  PopulateDelaunay(subdiv, {
                               CoordinateXY(0.0, 1.0),
                               CoordinateXY(10.0, 0.0),
                               CoordinateXY(5.0, 10.0),
                               CoordinateXY(5.0, 3.0),
                           });

  QuadEdge* hull_edge =
      subdiv.locate(Coordinate(0.0, 1.0), Coordinate(10.0, 0.0));
  ASSERT_NE(hull_edge, nullptr);
  EXPECT_FALSE(subdiv.isFrameEdge(*hull_edge));
  // Both orientations of a hull edge hit different branches of
  // isFrameBorderEdge().
  EXPECT_TRUE(subdiv.isFrameBorderEdge(*hull_edge));
  EXPECT_TRUE(subdiv.isFrameBorderEdge(hull_edge->sym()));

  QuadEdge* interior_edge =
      subdiv.locate(Coordinate(5.0, 3.0), Coordinate(0.0, 1.0));
  ASSERT_NE(interior_edge, nullptr);
  EXPECT_FALSE(subdiv.isFrameEdge(*interior_edge));
  EXPECT_FALSE(subdiv.isFrameBorderEdge(*interior_edge));

  // Verify frame-incident edges report isFrameEdge() == true in both
  // directions.
  int frame_edge_count = 0;
  const auto primary_edges = subdiv.getPrimaryEdges(true);
  for (const QuadEdge* qe : *primary_edges) {
    if (subdiv.isFrameEdge(*qe)) {
      EXPECT_TRUE(subdiv.isFrameEdge(qe->sym()));
      ++frame_edge_count;
    }
  }
  EXPECT_GT(frame_edge_count, 0);

  // isOnEdge uses edgeCoincidenceTolerance = tolerance / 1000 = 0.001.
  QuadEdge* vertical_spoke =
      subdiv.locate(Coordinate(5.0, 3.0), Coordinate(5.0, 10.0));
  ASSERT_NE(vertical_spoke, nullptr);
  EXPECT_TRUE(subdiv.isOnEdge(*vertical_spoke, Coordinate(5.0, 6.5)));
  EXPECT_TRUE(subdiv.isOnEdge(*vertical_spoke, Coordinate(5.0005, 6.5)));
  EXPECT_FALSE(subdiv.isOnEdge(*vertical_spoke, Coordinate(5.01, 6.5)));

  // isVertexOfEdge uses strict distance < tolerance (1.0).
  EXPECT_TRUE(subdiv.isVertexOfEdge(*vertical_spoke, Vertex(5.5, 3.0)));
  EXPECT_TRUE(subdiv.isVertexOfEdge(*vertical_spoke, Vertex(5.0, 9.5)));
  EXPECT_FALSE(subdiv.isVertexOfEdge(*vertical_spoke, Vertex(6.0, 3.0)));
  EXPECT_FALSE(subdiv.isVertexOfEdge(*vertical_spoke, Vertex(5.0, 6.5)));

  // Verify const-qualified query and predicate accessors.
  const QuadEdgeSubdivision& const_subdiv = subdiv;
  EXPECT_DOUBLE_EQ(const_subdiv.getTolerance(), 1.0);
  EXPECT_FALSE(const_subdiv.getEnvelope().isNull());
  QuadEdge* located_const = const_subdiv.locate(Vertex(5.0, 3.0));
  ASSERT_NE(located_const, nullptr);
  EXPECT_EQ(const_subdiv.locateFromEdge(Vertex(5.0, 3.0), *located_const),
            located_const);
  EXPECT_FALSE(const_subdiv.isFrameEdge(*interior_edge));
  EXPECT_FALSE(const_subdiv.isFrameBorderEdge(*interior_edge));
  EXPECT_FALSE(const_subdiv.isFrameVertex(Vertex(5.0, 3.0)));
  EXPECT_TRUE(const_subdiv.isOnEdge(*vertical_spoke, Coordinate(5.0, 6.5)));
  EXPECT_TRUE(const_subdiv.isVertexOfEdge(*vertical_spoke, Vertex(5.5, 3.0)));

  // Exact binary boundary for edgeCoincidenceTolerance = 125.0 / 1000 = 0.125.
  QuadEdgeSubdivision exact_edge_tol(Envelope(0.0, 10.0, 0.0, 10.0), 125.0);
  QuadEdge& horiz_edge =
      exact_edge_tol.makeEdge(Vertex(0.0, 0.0), Vertex(8.0, 0.0));
  EXPECT_TRUE(exact_edge_tol.isOnEdge(horiz_edge, Coordinate(4.0, 0.0625)));
  EXPECT_FALSE(exact_edge_tol.isOnEdge(horiz_edge, Coordinate(4.0, 0.125)));

  // With zero tolerance, strict `< 0.0` always returns false.
  QuadEdgeSubdivision zero_tol(Envelope(0.0, 10.0, 0.0, 10.0), 0.0);
  QuadEdge& zero_edge = zero_tol.makeEdge(Vertex(0.0, 0.0), Vertex(10.0, 0.0));
  EXPECT_FALSE(zero_tol.isOnEdge(zero_edge, Coordinate(5.0, 0.0)));
  EXPECT_FALSE(zero_tol.isVertexOfEdge(zero_edge, Vertex(0.0, 0.0)));
}

TEST(QuadEdgeSubdivisionTest, PrimaryEdgesVisitTrianglesAndPrepareVisit) {
  QuadEdgeSubdivision subdiv(Envelope(0.0, 10.0, 0.0, 10.0), 0.0);
  PopulateDelaunay(subdiv, {
                               CoordinateXY(0.0, 0.0),
                               CoordinateXY(10.0, 0.0),
                               CoordinateXY(10.0, 10.0),
                               CoordinateXY(0.0, 10.0),
                           });

  auto interior_edges = subdiv.getPrimaryEdges(false);
  EXPECT_THAT(*interior_edges, SizeIs(5));
  for (QuadEdge* qe : *interior_edges) {
    EXPECT_FALSE(subdiv.isFrameEdge(*qe));
    EXPECT_EQ(&qe->getPrimary(), qe);
  }

  // Maximal planar triangulation on 4 + 3 = 7 vertices has 3*7 - 6 = 15 edges.
  // Calling getPrimaryEdges() a second time exercises prepareVisit() reset.
  auto all_edges = subdiv.getPrimaryEdges(true);
  EXPECT_THAT(*all_edges, SizeIs(15));

  // Maximal planar triangulation on 7 vertices has 2*7 - 4 = 10 faces
  // (2 interior triangles, 7 frame-border triangles, 1 exterior frame face).
  RecordingTriangleVisitor no_frame_visitor;
  subdiv.visitTriangles(&no_frame_visitor, false);
  EXPECT_THAT(no_frame_visitor.visited_coords, SizeIs(2));

  RecordingTriangleVisitor with_frame_visitor;
  subdiv.visitTriangles(&with_frame_visitor, true);
  EXPECT_THAT(with_frame_visitor.visited_coords, SizeIs(10));

  RecordingTriangleVisitor repeated_visitor;
  subdiv.visitTriangles(&repeated_visitor, false);
  EXPECT_THAT(repeated_visitor.visited_coords, SizeIs(2));
}

TEST(QuadEdgeSubdivisionTest, GetEdgesAndGetTrianglesGeometryOutput) {
  const auto factory = GeometryFactory::create();

  {
    QuadEdgeSubdivision empty_subdiv(Envelope(0.0, 10.0, 0.0, 10.0), 0.0);
    auto edges = empty_subdiv.getEdges(*factory);
    auto tris = empty_subdiv.getTriangles(*factory);
    EXPECT_TRUE(edges->isEmpty());
    EXPECT_EQ(edges->getNumGeometries(), 0u);
    EXPECT_TRUE(tris->isEmpty());
    EXPECT_EQ(tris->getNumGeometries(), 0u);
  }

  {
    QuadEdgeSubdivision one_site(Envelope(0.0, 10.0, 0.0, 10.0), 0.0);
    PopulateDelaunay(one_site, {CoordinateXY(5.0, 5.0)});
    EXPECT_TRUE(one_site.getEdges(*factory)->isEmpty());
    EXPECT_TRUE(one_site.getTriangles(*factory)->isEmpty());
  }

  {
    QuadEdgeSubdivision two_sites(Envelope(0.0, 10.0, 0.0, 10.0), 0.0);
    PopulateDelaunay(two_sites,
                     {CoordinateXY(3.0, 5.0), CoordinateXY(7.0, 5.0)});
    auto edges = two_sites.getEdges(*factory);
    EXPECT_EQ(edges->getNumGeometries(), 1u);
    EXPECT_DOUBLE_EQ(edges->getLength(), 4.0);
    EXPECT_TRUE(two_sites.getTriangles(*factory)->isEmpty());
  }

  {
    QuadEdgeSubdivision tri_subdiv(Envelope(0.0, 6.0, 0.0, 4.0), 0.0);
    PopulateDelaunay(tri_subdiv, {
                                     CoordinateXY(0.0, 0.0),
                                     CoordinateXY(6.0, 0.0),
                                     CoordinateXY(0.0, 4.0),
                                 });

    auto edges = tri_subdiv.getEdges(*factory);
    EXPECT_EQ(edges->getNumGeometries(), 3u);
    for (std::size_t i = 0; i < edges->getNumGeometries(); ++i) {
      const LineString* ls = edges->getGeometryN(i);
      EXPECT_EQ(ls->getNumPoints(), 2u);
      EXPECT_GT(ls->getLength(), 0.0);
    }

    auto tris = tri_subdiv.getTriangles(*factory);
    ASSERT_EQ(tris->getNumGeometries(), 1u);
    const auto* poly = dynamic_cast<const Polygon*>(tris->getGeometryN(0));
    ASSERT_NE(poly, nullptr);
    EXPECT_EQ(poly->getNumPoints(), 4u);
    EXPECT_DOUBLE_EQ(poly->getArea(), 12.0);
  }

  {
    QuadEdgeSubdivision subdiv(Envelope(0.0, 10.0, 0.0, 10.0), 0.0);
    PopulateDelaunay(subdiv, {
                                 CoordinateXY(0.0, 0.0),
                                 CoordinateXY(10.0, 0.0),
                                 CoordinateXY(10.0, 10.0),
                                 CoordinateXY(0.0, 10.0),
                                 CoordinateXY(5.0, 5.0),
                             });
    auto edges = subdiv.getEdges(*factory);
    EXPECT_EQ(edges->getNumGeometries(), 8u);

    auto tris = subdiv.getTriangles(*factory);
    EXPECT_EQ(tris->getNumGeometries(), 4u);
    EXPECT_DOUBLE_EQ(tris->getArea(), 100.0);
  }
}

TEST(QuadEdgeSubdivisionTest, GetVertexUniqueEdgesReturnsOneEdgePerVertex) {
  QuadEdgeSubdivision subdiv(Envelope(0.0, 10.0, 0.0, 10.0), 0.0);
  EXPECT_TRUE(subdiv.getVertexUniqueEdges(false)->empty());
  EXPECT_THAT(*subdiv.getVertexUniqueEdges(true), SizeIs(3));

  const std::vector<CoordinateXY> sites = {
      CoordinateXY(1.0, 1.0),
      CoordinateXY(9.0, 1.0),
      CoordinateXY(8.0, 8.0),
      CoordinateXY(2.0, 9.0),
  };
  PopulateDelaunay(subdiv, sites);

  auto site_edges = subdiv.getVertexUniqueEdges(false);
  ASSERT_THAT(*site_edges, SizeIs(sites.size()));
  std::vector<CoordinateXY> seen_sites;
  for (const QuadEdge* qe : *site_edges) {
    EXPECT_FALSE(subdiv.isFrameVertex(qe->orig()));
    seen_sites.push_back(qe->orig().getCoordinate());
  }
  EXPECT_THAT(seen_sites, UnorderedElementsAreArray(sites));

  auto all_edges = subdiv.getVertexUniqueEdges(true);
  ASSERT_THAT(*all_edges, SizeIs(sites.size() + 3));
  std::set<CoordinateXY> unique_origins;
  for (const QuadEdge* qe : *all_edges) {
    EXPECT_TRUE(unique_origins.insert(qe->orig().getCoordinate()).second);
  }
}

TEST(QuadEdgeSubdivisionTest, VoronoiDiagramAndCellMethods) {
  const auto factory = GeometryFactory::create();

  {
    QuadEdgeSubdivision empty_subdiv(Envelope(0.0, 10.0, 0.0, 10.0), 0.0);
    EXPECT_TRUE(empty_subdiv.getVoronoiDiagram(*factory)->isEmpty());
    EXPECT_TRUE(empty_subdiv.getVoronoiDiagramEdges(*factory)->isEmpty());
    EXPECT_TRUE(empty_subdiv.getVoronoiCellPolygons(*factory).empty());
    EXPECT_TRUE(empty_subdiv.getVoronoiCellEdges(*factory).empty());
  }

  {
    QuadEdgeSubdivision single_site(Envelope(0.0, 10.0, 0.0, 10.0), 0.0);
    PopulateDelaunay(single_site, {CoordinateXY(5.0, 5.0)},
                     /*force_convex=*/false);
    auto single_diagram = single_site.getVoronoiDiagram(*factory);
    ASSERT_EQ(single_diagram->getNumGeometries(), 1u);
    const auto* poly =
        dynamic_cast<const Polygon*>(single_diagram->getGeometryN(0));
    ASSERT_NE(poly, nullptr);
    EXPECT_TRUE(poly->isValid());
    auto pt = factory->createPoint(Coordinate(5.0, 5.0));
    EXPECT_TRUE(poly->contains(pt.get()));
  }

  const std::vector<CoordinateXY> sites = {
      CoordinateXY(5.0, 5.0),  CoordinateXY(1.0, 1.0),  CoordinateXY(9.0, 1.0),
      CoordinateXY(11.0, 5.0), CoordinateXY(5.0, 11.0), CoordinateXY(-1.0, 5.0),
  };
  Envelope env(-1.0, 11.0, 1.0, 11.0);
  env.expandBy(std::max(env.getWidth(), env.getHeight()));

  QuadEdgeSubdivision subdiv(env, 0.0);
  PopulateDelaunay(subdiv, sites, /*force_convex=*/false);

  auto diagram = subdiv.getVoronoiDiagram(*factory);
  ASSERT_EQ(diagram->getNumGeometries(), sites.size());

  std::vector<CoordinateXY> userdata_sites;
  for (std::size_t i = 0; i < diagram->getNumGeometries(); ++i) {
    const Geometry* geom = diagram->getGeometryN(i);
    const auto* poly = dynamic_cast<const Polygon*>(geom);
    ASSERT_NE(poly, nullptr);
    EXPECT_TRUE(poly->isValid());
    EXPECT_GT(poly->getArea(), 0.0);

    const auto* site_coord =
        static_cast<const Coordinate*>(poly->getUserData());
    ASSERT_NE(site_coord, nullptr);
    userdata_sites.emplace_back(site_coord->x, site_coord->y);

    auto site_pt = factory->createPoint(*site_coord);
    EXPECT_TRUE(poly->contains(site_pt.get()));
  }
  EXPECT_THAT(userdata_sites, UnorderedElementsAreArray(sites));

  auto diagram_edges = subdiv.getVoronoiDiagramEdges(*factory);
  ASSERT_EQ(diagram_edges->getNumGeometries(), sites.size());
  for (std::size_t i = 0; i < diagram_edges->getNumGeometries(); ++i) {
    const LineString* ls = diagram_edges->getGeometryN(i);
    EXPECT_TRUE(ls->isClosed());
    EXPECT_GT(ls->getLength(), 0.0);
  }
}

TEST(QuadEdgeSubdivisionTest, VoronoiCellsHandleCocircularAndSmallFans) {
  const auto factory = GeometryFactory::create();

  // Four cocircular corners share circumcentre (5, 5) across two triangles,
  // exercising consecutive duplicate circumcentre filtering in cell
  // construction.
  {
    Envelope env(-12.0, 20.0, -10.0, 25.0);
    QuadEdgeSubdivision subdiv(env, 0.0);
    PopulateDelaunay(subdiv,
                     {
                         CoordinateXY(0.0, 0.0),
                         CoordinateXY(10.0, 0.0),
                         CoordinateXY(10.0, 10.0),
                         CoordinateXY(0.0, 10.0),
                     },
                     /*force_convex=*/false);

    auto polys = subdiv.getVoronoiCellPolygons(*factory);
    ASSERT_THAT(polys, SizeIs(4));
    for (const auto& geom : polys) {
      const auto* poly = dynamic_cast<const Polygon*>(geom.get());
      ASSERT_NE(poly, nullptr);
      EXPECT_TRUE(poly->isValid());
      const auto* site = static_cast<const Coordinate*>(poly->getUserData());
      ASSERT_NE(site, nullptr);
      auto pt = factory->createPoint(*site);
      EXPECT_TRUE(poly->contains(pt.get()));
    }

    auto edges = subdiv.getVoronoiCellEdges(*factory);
    ASSERT_THAT(edges, SizeIs(4));
    for (const auto& geom : edges) {
      const auto* ls = dynamic_cast<const LineString*>(geom.get());
      ASSERT_NE(ls, nullptr);
      EXPECT_TRUE(ls->isClosed());
    }
  }

  // Degree-3 vertex fan with only two distinct circumcentres covers both
  // front != back (closing ring + padding to 4 points) and front == back
  // (already closed + padding to 4 points).
  {
    QuadEdgeSubdivision subdiv(Envelope(0.0, 10.0, 0.0, 10.0), 0.0);
    subdiv.insertSite(Vertex(5.0, 5.0));
    auto site_edges = subdiv.getVertexUniqueEdges(false);
    ASSERT_THAT(*site_edges, SizeIs(1));

    QuadEdge* qe0 = site_edges->at(0);
    QuadEdge* qe1 = &qe0->oPrev();
    QuadEdge* qe2 = &qe1->oPrev();
    ASSERT_EQ(&qe2->oPrev(), qe0);

    // Consecutive duplicates: [(1,1), (1,1), (2,2)] -> [(1,1), (2,2)] -> closed
    // & padded.
    qe0->rot().setOrig(Vertex(1.0, 1.0));
    qe1->rot().setOrig(Vertex(1.0, 1.0));
    qe2->rot().setOrig(Vertex(2.0, 2.0));

    auto poly_closed = subdiv.getVoronoiCellPolygon(qe0, *factory);
    EXPECT_EQ(poly_closed->getNumPoints(), 4u);
    const auto* site1 =
        static_cast<const Coordinate*>(poly_closed->getUserData());
    ASSERT_NE(site1, nullptr);
    EXPECT_TRUE(site1->equals2D(CoordinateXY(5.0, 5.0)));

    auto edge_closed = subdiv.getVoronoiCellEdge(qe0, *factory);
    const auto* ls_closed = dynamic_cast<const LineString*>(edge_closed.get());
    ASSERT_NE(ls_closed, nullptr);
    EXPECT_EQ(ls_closed->getNumPoints(), 3u);
    EXPECT_TRUE(ls_closed->isClosed());

    // Wrap-around duplicate: [(1,1), (2,2), (1,1)] -> front == back -> padded.
    qe0->rot().setOrig(Vertex(1.0, 1.0));
    qe1->rot().setOrig(Vertex(2.0, 2.0));
    qe2->rot().setOrig(Vertex(1.0, 1.0));

    auto poly_wrapped = subdiv.getVoronoiCellPolygon(qe0, *factory);
    EXPECT_EQ(poly_wrapped->getNumPoints(), 4u);

    auto edge_wrapped = subdiv.getVoronoiCellEdge(qe0, *factory);
    const auto* ls_wrapped =
        dynamic_cast<const LineString*>(edge_wrapped.get());
    ASSERT_NE(ls_wrapped, nullptr);
    EXPECT_EQ(ls_wrapped->getNumPoints(), 3u);
    EXPECT_TRUE(ls_wrapped->isClosed());
  }
}

}  // namespace
}  // namespace geos_google
