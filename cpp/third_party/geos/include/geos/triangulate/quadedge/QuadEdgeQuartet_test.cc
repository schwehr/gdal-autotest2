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
// third_party/geos/include/geos/triangulate/quadedge/QuadEdgeQuartet.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/triangulate/quadedge/QuadEdgeQuartet.h

#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdgeQuartet.h"

#include <cmath>
#include <cstddef>
#include <deque>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdge.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdgeSubdivision.h"
#include "third_party/geos/include/geos/triangulate/quadedge/Vertex.h"

namespace geos_google {
namespace {

using geos::geom::Envelope;
using geos::triangulate::quadedge::QuadEdge;
using geos::triangulate::quadedge::QuadEdgeQuartet;
using geos::triangulate::quadedge::QuadEdgeSubdivision;
using geos::triangulate::quadedge::Vertex;

TEST(QuadEdgeQuartetTest, DefaultConstructorInitializesRingLinksAndFlags) {
  QuadEdgeQuartet quartet;
  const QuadEdgeQuartet& const_quartet = quartet;

  QuadEdge& e0 = quartet.base();
  const QuadEdge& ce0 = const_quartet.base();
  EXPECT_EQ(&e0, &ce0);

  QuadEdge& e1 = e0.rot();
  QuadEdge& e2 = e0.sym();
  QuadEdge& e3 = e0.invRot();

  // Verify quartet elements are distinct and follow contiguous rotation layout.
  EXPECT_EQ(&e1, &e0 + 1);
  EXPECT_EQ(&e2, &e0 + 2);
  EXPECT_EQ(&e3, &e0 + 3);
  EXPECT_EQ(&e1.rot(), &e2);
  EXPECT_EQ(&e2.rot(), &e3);
  EXPECT_EQ(&e3.rot(), &e0);

  // Verify initial next pointers set by QuadEdgeQuartet():
  // e[0].next = e[0], e[1].next = e[3], e[2].next = e[2], e[3].next = e[1].
  EXPECT_EQ(&e0.oNext(), &e0);
  EXPECT_EQ(&e1.oNext(), &e3);
  EXPECT_EQ(&e2.oNext(), &e2);
  EXPECT_EQ(&e3.oNext(), &e1);

  // Derived topological face/origin cycles for an isolated edge quartet.
  EXPECT_EQ(&e0.lNext(), &e2);
  EXPECT_EQ(&e2.lNext(), &e0);
  EXPECT_EQ(&e1.lNext(), &e1);
  EXPECT_EQ(&e3.lNext(), &e3);
  EXPECT_EQ(&e0.oPrev(), &e0);
  EXPECT_EQ(&e2.oPrev(), &e2);

  // Initial state: all four edges are live, unvisited, and default-vertexed.
  for (const QuadEdge* edge : {&e0, &e1, &e2, &e3}) {
    EXPECT_TRUE(edge->isLive());
    EXPECT_FALSE(edge->isVisited());
    EXPECT_DOUBLE_EQ(edge->orig().getX(), 0.0);
    EXPECT_DOUBLE_EQ(edge->orig().getY(), 0.0);
    EXPECT_TRUE(std::isnan(edge->orig().getZ()));
  }
}

TEST(QuadEdgeQuartetTest, BaseAccessorsSupportNonConstAndConstViews) {
  QuadEdgeQuartet quartet;
  const QuadEdgeQuartet& const_quartet = quartet;

  EXPECT_EQ(&quartet.base(), &const_quartet.base());

  const Vertex orig(1.5, 2.5, 3.5);
  const Vertex dest(4.5, 6.5, 7.5);
  quartet.base().setOrig(orig);
  quartet.base().setDest(dest);

  EXPECT_TRUE(const_quartet.base().orig().equals(orig));
  EXPECT_DOUBLE_EQ(const_quartet.base().orig().getZ(), 3.5);
  EXPECT_TRUE(const_quartet.base().dest().equals(dest));
  EXPECT_DOUBLE_EQ(const_quartet.base().dest().getZ(), 7.5);
  EXPECT_TRUE(const_quartet.base().sym().orig().equals(dest));
  EXPECT_TRUE(const_quartet.base().sym().dest().equals(orig));
}

TEST(QuadEdgeQuartetTest, MakeEdgeAppendsToDequeAndSetsEndpoints) {
  std::deque<QuadEdgeQuartet> edges;

  const Vertex o1(1.0, 2.0, 10.0);
  const Vertex d1(4.0, 6.0, 20.0);
  QuadEdge& e1 = QuadEdgeQuartet::makeEdge(o1, d1, edges);
  ASSERT_EQ(edges.size(), 1u);
  EXPECT_EQ(&e1, &edges.back().base());
  EXPECT_TRUE(e1.orig().equals(o1));
  EXPECT_DOUBLE_EQ(e1.orig().getZ(), 10.0);
  EXPECT_TRUE(e1.dest().equals(d1));
  EXPECT_DOUBLE_EQ(e1.dest().getZ(), 20.0);
  EXPECT_TRUE(e1.sym().orig().equals(d1));
  EXPECT_TRUE(e1.sym().dest().equals(o1));
  EXPECT_DOUBLE_EQ(e1.getLength(), 5.0);

  const Vertex o2(-3.0, 7.0);
  const Vertex d2(9.0, -5.0);
  QuadEdge& e2 = QuadEdgeQuartet::makeEdge(o2, d2, edges);
  ASSERT_EQ(edges.size(), 2u);
  EXPECT_EQ(&e2, &edges.back().base());
  EXPECT_NE(&e1, &e2);
  EXPECT_TRUE(e2.orig().equals(o2));
  EXPECT_TRUE(std::isnan(e2.orig().getZ()));
  EXPECT_TRUE(e2.dest().equals(d2));
  EXPECT_TRUE(std::isnan(e2.dest().getZ()));

  // Verify deque stability and quadedge algebra on quartet-created edges.
  EXPECT_EQ(&e1, &edges.front().base());
  QuadEdge& e3 = QuadEdgeQuartet::makeEdge(d1, o2, edges);
  QuadEdge::splice(e1.sym(), e3);
  EXPECT_EQ(&e1.lNext(), &e3);
  EXPECT_EQ(&e3.lPrev(), &e1);
}

TEST(QuadEdgeQuartetTest, MakeEdgePreservesPointerStabilityAcrossManyEdges) {
  std::deque<QuadEdgeQuartet> edges;
  std::vector<QuadEdge*> pointers;
  constexpr std::size_t kCount = 200;
  pointers.reserve(kCount);

  for (std::size_t i = 0; i < kCount; ++i) {
    const double coord = static_cast<double>(i);
    QuadEdge& edge = QuadEdgeQuartet::makeEdge(Vertex(coord, 0.0),
                                               Vertex(coord, 1.0), edges);
    pointers.push_back(&edge);
  }

  ASSERT_EQ(edges.size(), kCount);
  for (std::size_t i = 0; i < kCount; ++i) {
    EXPECT_EQ(pointers[i], &edges[i].base());
    EXPECT_TRUE(pointers[i]->isLive());
    EXPECT_DOUBLE_EQ(pointers[i]->orig().getX(), static_cast<double>(i));
    EXPECT_DOUBLE_EQ(pointers[i]->dest().getY(), 1.0);
  }
}

TEST(QuadEdgeQuartetTest, SetVisitedUpdatesAllFourEdgesInQuartet) {
  QuadEdgeQuartet quartet;
  QuadEdge& e0 = quartet.base();
  QuadEdge& e1 = e0.rot();
  QuadEdge& e2 = e0.sym();
  QuadEdge& e3 = e0.invRot();

  // Single-edge setVisited leaves the other three edges in the quartet
  // unchanged.
  e0.setVisited(true);
  EXPECT_TRUE(e0.isVisited());
  EXPECT_FALSE(e1.isVisited());
  EXPECT_FALSE(e2.isVisited());
  EXPECT_FALSE(e3.isVisited());

  // QuadEdgeQuartet::setVisited(true) marks all four edges in the quartet.
  quartet.setVisited(true);
  EXPECT_TRUE(e0.isVisited());
  EXPECT_TRUE(e1.isVisited());
  EXPECT_TRUE(e2.isVisited());
  EXPECT_TRUE(e3.isVisited());

  // Clearing a single edge leaves the rest visited until
  // quartet.setVisited(false).
  e2.setVisited(false);
  EXPECT_TRUE(e0.isVisited());
  EXPECT_TRUE(e1.isVisited());
  EXPECT_FALSE(e2.isVisited());
  EXPECT_TRUE(e3.isVisited());

  quartet.setVisited(false);
  EXPECT_FALSE(e0.isVisited());
  EXPECT_FALSE(e1.isVisited());
  EXPECT_FALSE(e2.isVisited());
  EXPECT_FALSE(e3.isVisited());

  // Partial dual visited flags are also cleared by setVisited(false).
  e1.setVisited(true);
  e3.setVisited(true);
  quartet.setVisited(false);
  EXPECT_FALSE(e0.isVisited());
  EXPECT_FALSE(e1.isVisited());
  EXPECT_FALSE(e2.isVisited());
  EXPECT_FALSE(e3.isVisited());
}

TEST(QuadEdgeQuartetTest, SetVisitedResetsSubdivisionEdgeContainer) {
  QuadEdgeSubdivision subdiv(Envelope(0.0, 10.0, 0.0, 10.0), 0.0);
  subdiv.insertSite(Vertex(5.0, 5.0));

  std::deque<QuadEdgeQuartet>& edges = subdiv.getEdges();
  ASSERT_FALSE(edges.empty());

  for (QuadEdgeQuartet& quartet : edges) {
    quartet.setVisited(true);
    const QuadEdgeQuartet& c_quartet = quartet;
    const QuadEdge& base = c_quartet.base();
    EXPECT_TRUE(base.isVisited());
    EXPECT_TRUE(base.rot().isVisited());
    EXPECT_TRUE(base.sym().isVisited());
    EXPECT_TRUE(base.invRot().isVisited());
  }

  for (QuadEdgeQuartet& quartet : edges) {
    quartet.setVisited(false);
    const QuadEdge& base = quartet.base();
    EXPECT_FALSE(base.isVisited());
    EXPECT_FALSE(base.rot().isVisited());
    EXPECT_FALSE(base.sym().isVisited());
    EXPECT_FALSE(base.invRot().isVisited());
  }
}

}  // namespace
}  // namespace geos_google
