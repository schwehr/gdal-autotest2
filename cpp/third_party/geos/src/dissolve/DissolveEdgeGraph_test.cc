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

// Unit tests for third_party/geos/src/dissolve/DissolveEdgeGraph.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/dissolve/DissolveEdgeGraph.cpp

#include "third_party/geos/include/geos/dissolve/DissolveEdgeGraph.h"

#include <cmath>
#include <cstddef>
#include <type_traits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/dissolve/DissolveHalfEdge.h"
#include "third_party/geos/include/geos/edgegraph/EdgeGraph.h"
#include "third_party/geos/include/geos/edgegraph/HalfEdge.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"

namespace geos_google {
namespace {

using geos::dissolve::DissolveEdgeGraph;
using geos::dissolve::DissolveHalfEdge;
using geos::edgegraph::EdgeGraph;
using geos::edgegraph::HalfEdge;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYZM;

static_assert(std::is_base_of_v<EdgeGraph, DissolveEdgeGraph>);
static_assert(!std::is_copy_constructible_v<DissolveEdgeGraph>);
static_assert(!std::is_copy_assignable_v<DissolveEdgeGraph>);

TEST(DissolveEdgeGraphTest, CreateEdgeAllocatesDissolveHalfEdge) {
  DissolveEdgeGraph graph;
  const CoordinateXYZM p0(1.5, 2.5, 3.5, 4.5);
  const CoordinateXYZM p1(5.5, 6.5, 7.5, 8.5);

  HalfEdge* raw0 = graph.createEdge(p0);
  HalfEdge* raw1 = graph.createEdge(p1);
  ASSERT_NE(raw0, nullptr);
  ASSERT_NE(raw1, nullptr);
  raw0->link(raw1);

  auto* d0 = dynamic_cast<DissolveHalfEdge*>(raw0);
  auto* d1 = dynamic_cast<DissolveHalfEdge*>(raw1);
  ASSERT_NE(d0, nullptr);
  ASSERT_NE(d1, nullptr);

  EXPECT_TRUE(d0->orig().equals4D(p0));
  EXPECT_TRUE(d0->dest().equals4D(p1));
  EXPECT_FALSE(d0->isStart());
  EXPECT_FALSE(d1->isStart());
  EXPECT_FALSE(d0->isMarked());
  EXPECT_FALSE(d1->isMarked());

  d0->setStart();
  d0->mark();
  EXPECT_TRUE(d0->isStart());
  EXPECT_TRUE(d0->isMarked());
  EXPECT_FALSE(d1->isStart());
  EXPECT_FALSE(d1->isMarked());
}

TEST(DissolveEdgeGraphTest, CreateEdgePreservesNaNForMissingZM) {
  DissolveEdgeGraph graph;
  const CoordinateXYZM p0(CoordinateXY(10.0, 20.0));
  const CoordinateXYZM p1(CoordinateXY(30.0, 40.0));

  HalfEdge* e0 = graph.createEdge(p0);
  HalfEdge* e1 = graph.createEdge(p1);
  ASSERT_NE(e0, nullptr);
  ASSERT_NE(e1, nullptr);
  e0->link(e1);

  EXPECT_DOUBLE_EQ(e0->orig().x, 10.0);
  EXPECT_DOUBLE_EQ(e0->orig().y, 20.0);
  EXPECT_TRUE(std::isnan(e0->orig().z));
  EXPECT_TRUE(std::isnan(e0->orig().m));
  EXPECT_DOUBLE_EQ(e0->dest().x, 30.0);
  EXPECT_DOUBLE_EQ(e0->dest().y, 40.0);
  EXPECT_TRUE(std::isnan(e0->dest().z));
  EXPECT_TRUE(std::isnan(e0->dest().m));
}

TEST(DissolveEdgeGraphTest, AddEdgeCreatesAndDeduplicatesDissolveHalfEdges) {
  DissolveEdgeGraph graph;
  const CoordinateXYZM a(0.0, 0.0, 1.0, 10.0);
  const CoordinateXYZM b(3.0, 4.0, 2.0, 20.0);
  const CoordinateXYZM c(0.0, 5.0, 3.0, 30.0);

  EXPECT_EQ(graph.addEdge(a, a), nullptr);

  HalfEdge* ab = graph.addEdge(a, b);
  ASSERT_NE(ab, nullptr);
  auto* dab = dynamic_cast<DissolveHalfEdge*>(ab);
  ASSERT_NE(dab, nullptr);
  EXPECT_FALSE(dab->isStart());
  EXPECT_TRUE(ab->orig().equals4D(a));
  EXPECT_TRUE(ab->dest().equals4D(b));

  // Re-adding the same directed edge returns the existing edge.
  EXPECT_EQ(graph.addEdge(a, b), ab);
  // Adding the opposite directed edge returns its symmetric half-edge.
  HalfEdge* ba = graph.addEdge(b, a);
  EXPECT_EQ(ba, ab->sym());
  ASSERT_NE(dynamic_cast<DissolveHalfEdge*>(ba), nullptr);

  HalfEdge* ac = graph.addEdge(a, c);
  ASSERT_NE(ac, nullptr);
  ASSERT_NE(dynamic_cast<DissolveHalfEdge*>(ac), nullptr);
  EXPECT_EQ(ab->degree(), 2);
  EXPECT_EQ(graph.findEdge(a, b), ab);
  EXPECT_EQ(graph.findEdge(b, a), ba);
  EXPECT_EQ(graph.findEdge(a, c), ac);
}

TEST(DissolveEdgeGraphTest, PointerStabilityAcrossManyDequeInsertions) {
  DissolveEdgeGraph graph;
  const CoordinateXYZM origin(0.0, 0.0, 0.0, 0.0);

  constexpr std::size_t kEdgeCount = 200;
  std::vector<HalfEdge*> added;
  added.reserve(kEdgeCount);

  for (std::size_t i = 1; i <= kEdgeCount; ++i) {
    const double angle = static_cast<double>(i) * 0.02;
    const CoordinateXYZM tip(std::cos(angle), std::sin(angle),
                             static_cast<double>(i),
                             static_cast<double>(i) * 2.0);
    HalfEdge* e = graph.addEdge(origin, tip);
    ASSERT_NE(e, nullptr);
    added.push_back(e);
  }

  for (std::size_t i = 0; i < added.size(); ++i) {
    auto* dhe = dynamic_cast<DissolveHalfEdge*>(added[i]);
    ASSERT_NE(dhe, nullptr);
    EXPECT_TRUE(dhe->orig().equals4D(origin));
    EXPECT_DOUBLE_EQ(dhe->dest().z, static_cast<double>(i + 1));
    EXPECT_DOUBLE_EQ(dhe->dest().m, static_cast<double>(i + 1) * 2.0);
  }

  std::vector<const HalfEdge*> vertex_edges;
  graph.getVertexEdges(vertex_edges);
  EXPECT_EQ(vertex_edges.size(), kEdgeCount + 1);
  EXPECT_EQ(added.front()->degree(), static_cast<int>(kEdgeCount));
}

}  // namespace
}  // namespace geos_google
