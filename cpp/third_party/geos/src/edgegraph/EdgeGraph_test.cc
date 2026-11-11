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

// Unit tests for third_party/geos/src/edgegraph/EdgeGraph.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/edgegraph/EdgeGraph.cpp

#include "third_party/geos/include/geos/edgegraph/EdgeGraph.h"

#include <array>
#include <cmath>
#include <deque>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/edgegraph/HalfEdge.h"
#include "third_party/geos/include/geos/edgegraph/MarkHalfEdge.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"

namespace geos_google {
namespace {

using geos::DoubleNotANumber;
using geos::edgegraph::EdgeGraph;
using geos::edgegraph::HalfEdge;
using geos::edgegraph::MarkHalfEdge;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYZM;

CoordinateXYZM MakeCoord(double x, double y, double z = DoubleNotANumber,
                         double m = DoubleNotANumber) {
  return CoordinateXYZM(x, y, z, m);
}

class MarkingEdgeGraph : public EdgeGraph {
 protected:
  HalfEdge* createEdge(const CoordinateXYZM& orig) override {
    marked_edges_.emplace_back(orig);
    return &marked_edges_.back();
  }

 private:
  std::deque<MarkHalfEdge> marked_edges_;
};

struct ValidEdgeCase {
  double x0;
  double y0;
  double x1;
  double y1;
  bool expected;
};

constexpr auto kValidEdgeCases = std::to_array<ValidEdgeCase>({
    {.x0 = 0.0, .y0 = 0.0, .x1 = 0.0, .y1 = 0.0, .expected = false},
    {.x0 = 3.5, .y0 = -2.5, .x1 = 3.5, .y1 = -2.5, .expected = false},
    {.x0 = 0.0, .y0 = 0.0, .x1 = 1.0, .y1 = 0.0, .expected = true},
    {.x0 = 0.0, .y0 = 0.0, .x1 = 0.0, .y1 = 1.0, .expected = true},
    {.x0 = -1.0, .y0 = -2.0, .x1 = 3.0, .y1 = 4.0, .expected = true},
});

TEST(EdgeGraphTest, IsValidEdgeChecksNonZero2DLength) {
  for (const auto& tc : kValidEdgeCases) {
    EXPECT_EQ(EdgeGraph::isValidEdge(CoordinateXY(tc.x0, tc.y0),
                                     CoordinateXY(tc.x1, tc.y1)),
              tc.expected);
  }
}

TEST(EdgeGraphTest, AddEdgeRejectsZeroLengthSegments) {
  EdgeGraph graph;

  EXPECT_EQ(graph.addEdge(MakeCoord(1.0, 2.0), MakeCoord(1.0, 2.0)), nullptr);
  // 2D equality determines edge validity even when Z or M differ.
  EXPECT_EQ(graph.addEdge(MakeCoord(1.0, 2.0, 10.0, 20.0),
                          MakeCoord(1.0, 2.0, 30.0, 40.0)),
            nullptr);

  std::vector<const HalfEdge*> vertex_edges;
  graph.getVertexEdges(vertex_edges);
  EXPECT_TRUE(vertex_edges.empty());
  EXPECT_EQ(graph.findEdge(CoordinateXY(1.0, 2.0), CoordinateXY(1.0, 2.0)),
            nullptr);
}

TEST(EdgeGraphTest, AddEdgeAndFindEdgeCoverAllInsertionCases) {
  EdgeGraph graph;

  // Case 1: neither origin nor destination is in the graph yet.
  HalfEdge* e_00_10 = graph.addEdge(MakeCoord(0.0, 0.0), MakeCoord(1.0, 0.0));
  ASSERT_NE(e_00_10, nullptr);
  EXPECT_TRUE(e_00_10->equals(CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0)));
  EXPECT_EQ(graph.findEdge(CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0)),
            e_00_10);
  EXPECT_EQ(graph.findEdge(CoordinateXY(1.0, 0.0), CoordinateXY(0.0, 0.0)),
            e_00_10->sym());

  // Case 2: origin already exists in vertexMap, destination is new.
  HalfEdge* e_00_01 = graph.addEdge(MakeCoord(0.0, 0.0), MakeCoord(0.0, 1.0));
  ASSERT_NE(e_00_01, nullptr);
  EXPECT_EQ(e_00_10->degree(), 2);
  EXPECT_TRUE(e_00_10->isEdgesSorted());

  // Case 3: origin is new, destination already exists in vertexMap.
  HalfEdge* e_11_10 = graph.addEdge(MakeCoord(1.0, 1.0), MakeCoord(1.0, 0.0));
  ASSERT_NE(e_11_10, nullptr);
  EXPECT_EQ(e_11_10->sym()->degree(), 2);
  EXPECT_TRUE(e_11_10->sym()->isEdgesSorted());
  EXPECT_EQ(graph.findEdge(CoordinateXY(1.0, 0.0), CoordinateXY(1.0, 1.0)),
            e_11_10->sym());

  // Case 4: both origin and destination already exist in vertexMap.
  HalfEdge* e_01_10 = graph.addEdge(MakeCoord(0.0, 1.0), MakeCoord(1.0, 0.0));
  ASSERT_NE(e_01_10, nullptr);
  EXPECT_EQ(e_01_10->degree(), 2);
  EXPECT_TRUE(e_01_10->isEdgesSorted());
  EXPECT_EQ(e_11_10->sym()->degree(), 3);
  EXPECT_TRUE(e_11_10->sym()->isEdgesSorted());

  // Non-existent origin and non-existent destination from existing origin.
  EXPECT_EQ(graph.findEdge(CoordinateXY(9.0, 9.0), CoordinateXY(0.0, 0.0)),
            nullptr);
  EXPECT_EQ(graph.findEdge(CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0)),
            nullptr);
}

TEST(EdgeGraphTest, AddEdgeDeduplicatesDuplicateAndReverseEdges) {
  EdgeGraph graph;

  HalfEdge* e = graph.addEdge(MakeCoord(0.0, 0.0, 1.0, 2.0),
                              MakeCoord(5.0, 0.0, 3.0, 4.0));
  ASSERT_NE(e, nullptr);

  HalfEdge* duplicate = graph.addEdge(MakeCoord(0.0, 0.0, 9.0, 9.0),
                                      MakeCoord(5.0, 0.0, 8.0, 8.0));
  EXPECT_EQ(duplicate, e);
  EXPECT_DOUBLE_EQ(e->orig().z, 1.0);
  EXPECT_DOUBLE_EQ(e->orig().m, 2.0);
  EXPECT_EQ(e->degree(), 1);

  HalfEdge* reverse = graph.addEdge(MakeCoord(5.0, 0.0), MakeCoord(0.0, 0.0));
  EXPECT_EQ(reverse, e->sym());
  EXPECT_EQ(e->degree(), 1);
  EXPECT_EQ(reverse->degree(), 1);
}

TEST(EdgeGraphTest, GetVertexEdgesAppendsOneEdgePerVertexOrderedByCoordinate) {
  EdgeGraph graph;

  // Empty graph appends nothing.
  std::vector<const HalfEdge*> edges;
  graph.getVertexEdges(edges);
  EXPECT_TRUE(edges.empty());

  HalfEdge* e1 = graph.addEdge(MakeCoord(1.0, 0.0), MakeCoord(0.0, 1.0));
  HalfEdge* e2 = graph.addEdge(MakeCoord(0.0, 1.0), MakeCoord(0.0, 0.0));
  HalfEdge* e3 = graph.addEdge(MakeCoord(0.0, 0.0), MakeCoord(1.0, 0.0));
  ASSERT_NE(e1, nullptr);
  ASSERT_NE(e2, nullptr);
  ASSERT_NE(e3, nullptr);

  graph.getVertexEdges(edges);
  ASSERT_EQ(edges.size(), 3u);
  EXPECT_TRUE(edges[0]->orig().equals2D(CoordinateXY(0.0, 0.0)));
  EXPECT_TRUE(edges[1]->orig().equals2D(CoordinateXY(0.0, 1.0)));
  EXPECT_TRUE(edges[2]->orig().equals2D(CoordinateXY(1.0, 0.0)));

  for (const HalfEdge* v_edge : edges) {
    EXPECT_TRUE(v_edge->isEdgesSorted());
  }

  // Calling getVertexEdges again appends without clearing existing contents.
  graph.getVertexEdges(edges);
  EXPECT_EQ(edges.size(), 6u);
}

TEST(EdgeGraphTest, PreservesXYZMCoordinatesAndNaNDefaults) {
  EdgeGraph graph;

  HalfEdge* xyzm_edge = graph.addEdge(MakeCoord(1.5, -2.5, 10.25, 42.5),
                                      MakeCoord(3.5, 4.5, -5.0, 99.0));
  ASSERT_NE(xyzm_edge, nullptr);
  EXPECT_DOUBLE_EQ(xyzm_edge->orig().x, 1.5);
  EXPECT_DOUBLE_EQ(xyzm_edge->orig().y, -2.5);
  EXPECT_DOUBLE_EQ(xyzm_edge->orig().z, 10.25);
  EXPECT_DOUBLE_EQ(xyzm_edge->orig().m, 42.5);
  EXPECT_DOUBLE_EQ(xyzm_edge->dest().x, 3.5);
  EXPECT_DOUBLE_EQ(xyzm_edge->dest().y, 4.5);
  EXPECT_DOUBLE_EQ(xyzm_edge->dest().z, -5.0);
  EXPECT_DOUBLE_EQ(xyzm_edge->dest().m, 99.0);

  HalfEdge* xy_edge =
      graph.addEdge(MakeCoord(10.0, 10.0), MakeCoord(11.0, 12.0));
  ASSERT_NE(xy_edge, nullptr);
  EXPECT_TRUE(std::isnan(xy_edge->orig().z));
  EXPECT_TRUE(std::isnan(xy_edge->orig().m));
  EXPECT_TRUE(std::isnan(xy_edge->dest().z));
  EXPECT_TRUE(std::isnan(xy_edge->dest().m));
}

TEST(EdgeGraphTest, StarTopologyAroundSharedHubRemainsCCWSorted) {
  EdgeGraph graph;
  const CoordinateXYZM hub = MakeCoord(0.0, 0.0);

  // Add spokes from all 4 quadrants and axis directions in mixed orientations.
  HalfEdge* s0 = graph.addEdge(hub, MakeCoord(-1.0, -1.0));
  HalfEdge* s1 = graph.addEdge(MakeCoord(1.0, 0.0), hub);
  HalfEdge* s2 = graph.addEdge(hub, MakeCoord(0.0, 1.0));
  HalfEdge* s3 = graph.addEdge(MakeCoord(-1.0, 1.0), hub);
  HalfEdge* s4 = graph.addEdge(hub, MakeCoord(1.0, 1.0));
  HalfEdge* s5 = graph.addEdge(MakeCoord(0.0, -1.0), hub);
  HalfEdge* s6 = graph.addEdge(hub, MakeCoord(1.0, -1.0));
  HalfEdge* s7 = graph.addEdge(MakeCoord(-1.0, 0.0), hub);

  ASSERT_NE(s0, nullptr);
  ASSERT_NE(s1, nullptr);
  ASSERT_NE(s2, nullptr);
  ASSERT_NE(s3, nullptr);
  ASSERT_NE(s4, nullptr);
  ASSERT_NE(s5, nullptr);
  ASSERT_NE(s6, nullptr);
  ASSERT_NE(s7, nullptr);

  EXPECT_EQ(s0->degree(), 8);
  EXPECT_TRUE(s0->isEdgesSorted());
  EXPECT_EQ(s1->degree(), 1);
  EXPECT_EQ(s3->degree(), 1);
  EXPECT_EQ(s5->degree(), 1);
  EXPECT_EQ(s7->degree(), 1);
  EXPECT_EQ(graph.findEdge(CoordinateXY(0.0, 0.0), CoordinateXY(-1.0, 0.0)),
            s7->sym());
}

TEST(EdgeGraphTest, SubclassCanOverrideCreateEdgeWithMarkHalfEdge) {
  MarkingEdgeGraph graph;

  HalfEdge* ab = graph.addEdge(MakeCoord(0.0, 0.0), MakeCoord(1.0, 0.0));
  HalfEdge* bc = graph.addEdge(MakeCoord(1.0, 0.0), MakeCoord(0.0, 1.0));
  HalfEdge* ca = graph.addEdge(MakeCoord(0.0, 1.0), MakeCoord(0.0, 0.0));
  ASSERT_NE(ab, nullptr);
  ASSERT_NE(bc, nullptr);
  ASSERT_NE(ca, nullptr);

  EXPECT_FALSE(MarkHalfEdge::isMarked(ab));
  EXPECT_FALSE(MarkHalfEdge::isMarked(ab->sym()));
  MarkHalfEdge::mark(ab);
  EXPECT_TRUE(MarkHalfEdge::isMarked(ab));
  EXPECT_FALSE(MarkHalfEdge::isMarked(ab->sym()));

  MarkHalfEdge::markBoth(bc);
  EXPECT_TRUE(MarkHalfEdge::isMarked(bc));
  EXPECT_TRUE(MarkHalfEdge::isMarked(bc->sym()));

  EXPECT_EQ(graph.findEdge(CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0)), ab);
  EXPECT_TRUE(MarkHalfEdge::isMarked(
      graph.findEdge(CoordinateXY(1.0, 0.0), CoordinateXY(0.0, 1.0))));
  EXPECT_FALSE(MarkHalfEdge::isMarked(ca));
}

}  // namespace
}  // namespace geos_google
