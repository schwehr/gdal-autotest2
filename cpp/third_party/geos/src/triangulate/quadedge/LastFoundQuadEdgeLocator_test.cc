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
// third_party/geos/src/triangulate/quadedge/LastFoundQuadEdgeLocator.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/triangulate/quadedge/LastFoundQuadEdgeLocator.cpp

#include "third_party/geos/include/geos/triangulate/quadedge/LastFoundQuadEdgeLocator.h"

#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/triangulate/IncrementalDelaunayTriangulator.h"
#include "third_party/geos/include/geos/triangulate/quadedge/LocateFailureException.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdge.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdgeLocator.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdgeQuartet.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdgeSubdivision.h"
#include "third_party/geos/include/geos/triangulate/quadedge/Vertex.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::Envelope;
using geos::triangulate::IncrementalDelaunayTriangulator;
using geos::triangulate::quadedge::LastFoundQuadEdgeLocator;
using geos::triangulate::quadedge::LocateFailureException;
using geos::triangulate::quadedge::QuadEdge;
using geos::triangulate::quadedge::QuadEdgeLocator;
using geos::triangulate::quadedge::QuadEdgeSubdivision;
using geos::triangulate::quadedge::Vertex;

static_assert(std::is_base_of_v<QuadEdgeLocator, LastFoundQuadEdgeLocator>);

// Verifies locate()'s postcondition: v is either an endpoint of e, or on/inside
// the triangle to the left of e.
bool EdgeContainsOrTouchesVertex(const QuadEdge& e, const Vertex& v) {
  if (v.equals(e.orig()) || v.equals(e.dest())) {
    return true;
  }
  const QuadEdge& e1 = e.lNext();
  const QuadEdge& e2 = e1.lNext();
  return &e2.lNext() == &e && !v.rightOf(e) && !v.rightOf(e1) && !v.rightOf(e2);
}

TEST(LastFoundQuadEdgeLocatorTest, InitialLocateInitializesLastEdge) {
  QuadEdgeSubdivision subdiv(Envelope(0.0, 10.0, 0.0, 10.0), 0.0);
  LastFoundQuadEdgeLocator locator(&subdiv);

  const Vertex query(5.0, 5.0);
  QuadEdge* edge = locator.locate(query);
  ASSERT_NE(edge, nullptr);
  EXPECT_TRUE(edge->isLive());
  EXPECT_TRUE(EdgeContainsOrTouchesVertex(*edge, query));
}

TEST(LastFoundQuadEdgeLocatorTest, SubsequentLocatesReuseLiveLastEdge) {
  QuadEdgeSubdivision subdiv(Envelope(0.0, 10.0, 0.0, 10.0), 1e-6);
  IncrementalDelaunayTriangulator triangulator(&subdiv);
  const IncrementalDelaunayTriangulator::VertexList sites = {
      Vertex(0.0, 0.0),  Vertex(10.0, 0.0), Vertex(10.0, 10.0),
      Vertex(0.0, 10.0), Vertex(5.0, 5.0),  Vertex(2.0, 8.0),
      Vertex(8.0, 2.0),
  };
  triangulator.insertSites(sites);

  LastFoundQuadEdgeLocator locator(&subdiv);

  // Querying existing sites exercises the initial nullptr -> init() path and
  // subsequent live lastEdge reuse across multiple locations.
  for (const Vertex& site : sites) {
    QuadEdge* edge = locator.locate(site);
    ASSERT_NE(edge, nullptr);
    EXPECT_TRUE(edge->isLive());
    EXPECT_TRUE(site.equals(edge->orig()) || site.equals(edge->dest()));
  }

  // Query interior points not coincident with any site.
  const std::vector<Vertex> interior_queries = {
      Vertex(3.0, 3.0),
      Vertex(7.0, 7.0),
      Vertex(5.0, 1.0),
      Vertex(1.0, 5.0),
  };
  for (const Vertex& query : interior_queries) {
    QuadEdge* edge = locator.locate(query);
    ASSERT_NE(edge, nullptr);
    EXPECT_TRUE(edge->isLive());
    EXPECT_TRUE(EdgeContainsOrTouchesVertex(*edge, query));
  }
}

TEST(LastFoundQuadEdgeLocatorTest, ReinitializesWhenLastEdgeBecomesNotLive) {
  QuadEdgeSubdivision subdiv(Envelope(0.0, 10.0, 0.0, 10.0), 0.0);
  const Vertex site(5.0, 5.0);
  subdiv.insertSite(site);

  LastFoundQuadEdgeLocator locator(&subdiv);

  // Locate the inserted vertex; returned edge connects site to a frame vertex
  // and is distinct from subdiv.getEdges()[0].base().
  QuadEdge* first_edge = locator.locate(site);
  ASSERT_NE(first_edge, nullptr);
  ASSERT_TRUE(first_edge->isLive());
  ASSERT_NE(first_edge, &(subdiv.getEdges()[0].base()));

  // Remove first_edge so lastEdge is non-null but no longer live.
  subdiv.remove(*first_edge);
  ASSERT_FALSE(first_edge->isLive());

  // Next locate() must notice !lastEdge->isLive(), call init(), and succeed via
  // the remaining live edges connected to site.
  QuadEdge* second_edge = locator.locate(site);
  ASSERT_NE(second_edge, nullptr);
  EXPECT_NE(second_edge, first_edge);
  EXPECT_TRUE(second_edge->isLive());
  EXPECT_TRUE(site.equals(second_edge->orig()) ||
              site.equals(second_edge->dest()));
}

TEST(LastFoundQuadEdgeLocatorTest, RecoversDuringIncrementalSiteOnEdgeSplit) {
  QuadEdgeSubdivision subdiv(Envelope(0.0, 10.0, 0.0, 10.0), 1e-6);
  auto locator_owner = std::make_unique<LastFoundQuadEdgeLocator>(&subdiv);
  QuadEdgeLocator* locator = locator_owner.get();
  subdiv.setLocator(std::move(locator_owner));

  IncrementalDelaunayTriangulator triangulator(&subdiv);
  triangulator.insertSites({
      Vertex(0.0, 0.0),
      Vertex(10.0, 0.0),
      Vertex(10.0, 10.0),
      Vertex(0.0, 10.0),
  });

  // Midpoint (5, 5) lies on the diagonal of the square, so locating it and
  // inserting it splits and deletes that diagonal edge.
  const Vertex midpoint(5.0, 5.0);
  QuadEdge* diagonal = locator->locate(midpoint);
  ASSERT_NE(diagonal, nullptr);
  ASSERT_TRUE(subdiv.isOnEdge(*diagonal, Coordinate(5.0, 5.0)));

  triangulator.insertSite(midpoint);
  EXPECT_FALSE(diagonal->isLive());

  // Subsequent triangulation insertion locates through the replaced locator
  // after its cached lastEdge was deleted.
  const Vertex follow_up(2.5, 5.0);
  triangulator.insertSite(follow_up);
  QuadEdge* located = subdiv.locate(follow_up);
  ASSERT_NE(located, nullptr);
  EXPECT_TRUE(located->isLive());
  EXPECT_TRUE(follow_up.equals(located->orig()) ||
              follow_up.equals(located->dest()));
}

TEST(LastFoundQuadEdgeLocatorTest, PropagatesLocateFailureOutsideFrame) {
  QuadEdgeSubdivision subdiv(Envelope(0.0, 10.0, 0.0, 10.0), 0.0);
  LastFoundQuadEdgeLocator locator(&subdiv);

  EXPECT_THROW(locator.locate(Vertex(-1000.0, 0.0)), LocateFailureException);
}

}  // namespace
}  // namespace geos_google
