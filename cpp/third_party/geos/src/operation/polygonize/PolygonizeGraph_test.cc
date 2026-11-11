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

#include "third_party/geos/include/geos/operation/polygonize/PolygonizeGraph.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/Area.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/polygonize/EdgeRing.h"
#include "third_party/geos/include/geos/planargraph/DirectedEdge.h"
#include "third_party/geos/include/geos/planargraph/Node.h"

namespace geos_google {
namespace {

using ::geos::algorithm::Area;
using ::geos::geom::Coordinate;
using ::geos::geom::GeometryFactory;
using ::geos::geom::LinearRing;
using ::geos::geom::LineString;
using ::geos::io::WKTReader;
using ::geos::operation::polygonize::EdgeRing;
using ::geos::operation::polygonize::PolygonizeGraph;
using ::geos::planargraph::DirectedEdge;
using ::geos::planargraph::Node;

struct RingSummary {
  std::vector<double> shell_areas;
  std::vector<double> hole_areas;
  std::vector<std::size_t> edge_counts;
  int valid_count = 0;
  int invalid_count = 0;
};

RingSummary SummarizeRings(const std::vector<EdgeRing*>& edge_rings) {
  RingSummary summary;
  for (EdgeRing* er : edge_rings) {
    er->computeValid();
    if (er->isValid()) {
      ++summary.valid_count;
      er->computeHole();
      const LinearRing* ring = er->getRingInternal();
      EXPECT_NE(ring, nullptr);
      if (ring != nullptr) {
        const double area = Area::ofRing(ring->getCoordinatesRO());
        if (er->isHole()) {
          summary.hole_areas.push_back(area);
        } else {
          summary.shell_areas.push_back(area);
        }
      }
    } else {
      ++summary.invalid_count;
    }
    summary.edge_counts.push_back(er->getEdges().size());
  }
  std::sort(summary.shell_areas.begin(), summary.shell_areas.end());
  std::sort(summary.hole_areas.begin(), summary.hole_areas.end());
  std::sort(summary.edge_counts.begin(), summary.edge_counts.end());
  return summary;
}

class PolygonizeGraphTest : public ::testing::Test {
 protected:
  PolygonizeGraphTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::vector<const LineString*> AddLines(
      PolygonizeGraph& graph, const std::vector<std::string>& wkts,
      std::vector<std::unique_ptr<LineString>>& storage) const {
    std::vector<const LineString*> raw_ptrs;
    raw_ptrs.reserve(wkts.size());
    for (const std::string& wkt : wkts) {
      storage.push_back(reader_.read<LineString>(wkt));
      const LineString* ptr = storage.back().get();
      graph.addEdge(ptr);
      raw_ptrs.push_back(ptr);
    }
    return raw_ptrs;
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(PolygonizeGraphTest, EmptyGraphOperationsProduceEmptyOutputs) {
  PolygonizeGraph graph(factory_.get());

  EXPECT_TRUE(graph.getEdges()->empty());
  std::vector<Node*> nodes;
  graph.getNodes(nodes);
  EXPECT_TRUE(nodes.empty());

  std::vector<const LineString*> dangles;
  graph.deleteDangles(dangles);
  EXPECT_TRUE(dangles.empty());

  std::vector<const LineString*> cut_lines;
  graph.deleteCutEdges(cut_lines);
  EXPECT_TRUE(cut_lines.empty());

  std::vector<EdgeRing*> edge_rings;
  graph.getEdgeRings(edge_rings);
  EXPECT_TRUE(edge_rings.empty());
}

TEST_F(PolygonizeGraphTest, IgnoresEmptyAndRepeatedPointOnlyLineStrings) {
  std::vector<std::unique_ptr<LineString>> lines;
  PolygonizeGraph graph(factory_.get());

  AddLines(graph,
           {
               "LINESTRING EMPTY",
               "LINESTRING (5 5, 5 5)",
               "LINESTRING (7 8, 7 8, 7 8)",
           },
           lines);

  EXPECT_TRUE(graph.getEdges()->empty());
  std::vector<Node*> nodes;
  graph.getNodes(nodes);
  EXPECT_TRUE(nodes.empty());

  // Consecutive duplicate vertices within a multi-point line are stripped,
  // while the remaining distinct vertices still form a valid ring.
  AddLines(graph,
           {
               "LINESTRING (0 0, 0 0, 10 0, 10 0)",
               "LINESTRING (10 0, 10 10, 10 10)",
               "LINESTRING (10 10, 0 10)",
               "LINESTRING (0 10, 0 0, 0 0)",
           },
           lines);

  EXPECT_EQ(graph.getEdges()->size(), 4u);
  nodes.clear();
  graph.getNodes(nodes);
  EXPECT_EQ(nodes.size(), 4u);

  std::vector<const LineString*> dangles;
  graph.deleteDangles(dangles);
  EXPECT_TRUE(dangles.empty());

  std::vector<const LineString*> cut_lines;
  graph.deleteCutEdges(cut_lines);
  EXPECT_TRUE(cut_lines.empty());

  std::vector<EdgeRing*> edge_rings;
  graph.getEdgeRings(edge_rings);
  const RingSummary summary = SummarizeRings(edge_rings);
  EXPECT_EQ(summary.valid_count, 2);
  EXPECT_EQ(summary.invalid_count, 0);
  EXPECT_EQ(summary.shell_areas, (std::vector<double>{100.0}));
  EXPECT_EQ(summary.hole_areas, (std::vector<double>{100.0}));
  EXPECT_EQ(summary.edge_counts, (std::vector<std::size_t>{4, 4}));
}

TEST_F(PolygonizeGraphTest, SingleClosedLineStringFormsShellAndOuterHole) {
  std::vector<std::unique_ptr<LineString>> lines;
  PolygonizeGraph graph(factory_.get());

  AddLines(graph, {"LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)"}, lines);

  EXPECT_EQ(graph.getEdges()->size(), 1u);
  std::vector<Node*> nodes;
  graph.getNodes(nodes);
  ASSERT_EQ(nodes.size(), 1u);
  EXPECT_EQ(nodes[0]->getDegree(), 2u);

  std::vector<const LineString*> dangles;
  graph.deleteDangles(dangles);
  EXPECT_TRUE(dangles.empty());

  std::vector<const LineString*> cut_lines;
  graph.deleteCutEdges(cut_lines);
  EXPECT_TRUE(cut_lines.empty());

  std::vector<EdgeRing*> edge_rings;
  graph.getEdgeRings(edge_rings);
  const RingSummary summary = SummarizeRings(edge_rings);
  EXPECT_EQ(summary.valid_count, 2);
  EXPECT_EQ(summary.invalid_count, 0);
  EXPECT_EQ(summary.shell_areas, (std::vector<double>{100.0}));
  EXPECT_EQ(summary.hole_areas, (std::vector<double>{100.0}));
  EXPECT_EQ(summary.edge_counts, (std::vector<std::size_t>{1, 1}));
}

TEST_F(PolygonizeGraphTest, TwoEdgeLensPolygonUsesInteriorDirectionPoints) {
  std::vector<std::unique_ptr<LineString>> lines;
  PolygonizeGraph graph(factory_.get());

  AddLines(graph,
           {
               "LINESTRING (0 0, 5 5, 10 0)",
               "LINESTRING (0 0, 5 -5, 10 0)",
           },
           lines);

  EXPECT_EQ(graph.getEdges()->size(), 2u);
  std::vector<Node*> nodes;
  graph.getNodes(nodes);
  EXPECT_EQ(nodes.size(), 2u);

  std::vector<const LineString*> dangles;
  graph.deleteDangles(dangles);
  EXPECT_TRUE(dangles.empty());

  std::vector<const LineString*> cut_lines;
  graph.deleteCutEdges(cut_lines);
  EXPECT_TRUE(cut_lines.empty());

  std::vector<EdgeRing*> edge_rings;
  graph.getEdgeRings(edge_rings);
  const RingSummary summary = SummarizeRings(edge_rings);
  EXPECT_EQ(summary.valid_count, 2);
  EXPECT_EQ(summary.invalid_count, 0);
  EXPECT_EQ(summary.shell_areas, (std::vector<double>{50.0}));
  EXPECT_EQ(summary.hole_areas, (std::vector<double>{50.0}));
  EXPECT_EQ(summary.edge_counts, (std::vector<std::size_t>{2, 2}));
}

TEST_F(PolygonizeGraphTest, AdjacentSquaresShareInteriorEdge) {
  std::vector<std::unique_ptr<LineString>> lines;
  PolygonizeGraph graph(factory_.get());

  AddLines(graph,
           {
               "LINESTRING (0 0, 10 0)",
               "LINESTRING (10 0, 10 10)",
               "LINESTRING (10 10, 0 10)",
               "LINESTRING (0 10, 0 0)",
               "LINESTRING (10 0, 20 0)",
               "LINESTRING (20 0, 20 10)",
               "LINESTRING (20 10, 10 10)",
           },
           lines);

  EXPECT_EQ(graph.getEdges()->size(), 7u);
  std::vector<Node*> nodes;
  graph.getNodes(nodes);
  EXPECT_EQ(nodes.size(), 6u);

  std::vector<const LineString*> dangles;
  graph.deleteDangles(dangles);
  EXPECT_TRUE(dangles.empty());

  std::vector<const LineString*> cut_lines;
  graph.deleteCutEdges(cut_lines);
  EXPECT_TRUE(cut_lines.empty());

  std::vector<EdgeRing*> edge_rings;
  graph.getEdgeRings(edge_rings);
  const RingSummary summary = SummarizeRings(edge_rings);
  EXPECT_EQ(summary.valid_count, 3);
  EXPECT_EQ(summary.invalid_count, 0);
  EXPECT_EQ(summary.shell_areas, (std::vector<double>{100.0, 100.0}));
  EXPECT_EQ(summary.hole_areas, (std::vector<double>{200.0}));
  EXPECT_EQ(summary.edge_counts, (std::vector<std::size_t>{4, 4, 6}));
}

TEST_F(PolygonizeGraphTest, DeleteDanglesDeduplicatesIsolatedSegment) {
  std::vector<std::unique_ptr<LineString>> lines;
  PolygonizeGraph graph(factory_.get());

  const auto ptrs = AddLines(graph, {"LINESTRING (0 0, 5 0)"}, lines);

  std::vector<const LineString*> dangles;
  graph.deleteDangles(dangles);
  ASSERT_EQ(dangles.size(), 1u);
  EXPECT_EQ(dangles[0], ptrs[0]);

  for (auto it = graph.dirEdgeBegin(); it != graph.dirEdgeEnd(); ++it) {
    EXPECT_TRUE((*it)->isMarked());
  }

  std::vector<const LineString*> cut_lines;
  graph.deleteCutEdges(cut_lines);
  EXPECT_TRUE(cut_lines.empty());

  std::vector<EdgeRing*> edge_rings;
  graph.getEdgeRings(edge_rings);
  EXPECT_TRUE(edge_rings.empty());
}

TEST_F(PolygonizeGraphTest, DeleteDanglesRemovesIsolatedMultiEdgeChain) {
  std::vector<std::unique_ptr<LineString>> lines;
  PolygonizeGraph graph(factory_.get());

  const auto ptrs = AddLines(graph,
                             {
                                 "LINESTRING (0 0, 2 0)",
                                 "LINESTRING (2 0, 4 0)",
                                 "LINESTRING (4 0, 6 0)",
                             },
                             lines);

  std::vector<const LineString*> dangles;
  graph.deleteDangles(dangles);
  EXPECT_EQ(dangles.size(), 3u);
  const std::set<const LineString*> expected(ptrs.begin(), ptrs.end());
  const std::set<const LineString*> actual(dangles.begin(), dangles.end());
  EXPECT_EQ(actual, expected);

  std::vector<EdgeRing*> edge_rings;
  graph.getEdgeRings(edge_rings);
  EXPECT_TRUE(edge_rings.empty());
}

TEST_F(PolygonizeGraphTest,
       DeleteDanglesCascadesBranchingTreeAttachedToSquare) {
  std::vector<std::unique_ptr<LineString>> lines;
  PolygonizeGraph graph(factory_.get());

  AddLines(graph,
           {
               "LINESTRING (0 0, 10 0)",
               "LINESTRING (10 0, 10 10)",
               "LINESTRING (10 10, 0 10)",
               "LINESTRING (0 10, 0 0)",
           },
           lines);
  const auto dangle_ptrs = AddLines(graph,
                                    {
                                        "LINESTRING (10 10, 15 15)",
                                        "LINESTRING (15 15, 20 15)",
                                        "LINESTRING (15 15, 15 20)",
                                    },
                                    lines);

  std::vector<const LineString*> dangles;
  graph.deleteDangles(dangles);
  EXPECT_EQ(dangles.size(), 3u);
  const std::set<const LineString*> expected(dangle_ptrs.begin(),
                                             dangle_ptrs.end());
  const std::set<const LineString*> actual(dangles.begin(), dangles.end());
  EXPECT_EQ(actual, expected);

  std::vector<const LineString*> cut_lines;
  graph.deleteCutEdges(cut_lines);
  EXPECT_TRUE(cut_lines.empty());

  std::vector<EdgeRing*> edge_rings;
  graph.getEdgeRings(edge_rings);
  const RingSummary summary = SummarizeRings(edge_rings);
  EXPECT_EQ(summary.valid_count, 2);
  EXPECT_EQ(summary.invalid_count, 0);
  EXPECT_EQ(summary.shell_areas, (std::vector<double>{100.0}));
  EXPECT_EQ(summary.hole_areas, (std::vector<double>{100.0}));
  EXPECT_EQ(summary.edge_counts, (std::vector<std::size_t>{4, 4}));
}

TEST_F(PolygonizeGraphTest,
       DeleteCutEdgesRemovesMultiSegmentBridgeBetweenPolygons) {
  std::vector<std::unique_ptr<LineString>> lines;
  PolygonizeGraph graph(factory_.get());

  AddLines(graph,
           {
               // Left square split at (10, 5)
               "LINESTRING (0 0, 10 0)",
               "LINESTRING (10 0, 10 5)",
               "LINESTRING (10 5, 10 10)",
               "LINESTRING (10 10, 0 10)",
               "LINESTRING (0 10, 0 0)",
               // Right square split at (20, 5)
               "LINESTRING (20 0, 30 0)",
               "LINESTRING (30 0, 30 10)",
               "LINESTRING (30 10, 20 10)",
               "LINESTRING (20 10, 20 5)",
               "LINESTRING (20 5, 20 0)",
           },
           lines);
  const auto bridge_ptrs = AddLines(graph,
                                    {
                                        "LINESTRING (10 5, 15 5)",
                                        "LINESTRING (15 5, 20 5)",
                                    },
                                    lines);

  std::vector<const LineString*> dangles;
  graph.deleteDangles(dangles);
  EXPECT_TRUE(dangles.empty());

  std::vector<const LineString*> cut_lines;
  graph.deleteCutEdges(cut_lines);
  EXPECT_EQ(cut_lines.size(), 2u);
  const std::set<const LineString*> expected(bridge_ptrs.begin(),
                                             bridge_ptrs.end());
  const std::set<const LineString*> actual(cut_lines.begin(), cut_lines.end());
  EXPECT_EQ(actual, expected);

  std::vector<EdgeRing*> edge_rings;
  graph.getEdgeRings(edge_rings);
  const RingSummary summary = SummarizeRings(edge_rings);
  EXPECT_EQ(summary.valid_count, 4);
  EXPECT_EQ(summary.invalid_count, 0);
  EXPECT_EQ(summary.shell_areas, (std::vector<double>{100.0, 100.0}));
  EXPECT_EQ(summary.hole_areas, (std::vector<double>{100.0, 100.0}));
  EXPECT_EQ(summary.edge_counts, (std::vector<std::size_t>{5, 5, 5, 5}));
}

TEST_F(PolygonizeGraphTest,
       DeleteCutEdgesRemovesInternalBridgeToNestedPolygon) {
  std::vector<std::unique_ptr<LineString>> lines;
  PolygonizeGraph graph(factory_.get());

  AddLines(graph,
           {
               // Outer square split at (0, 5)
               "LINESTRING (0 0, 10 0)",
               "LINESTRING (10 0, 10 10)",
               "LINESTRING (10 10, 0 10)",
               "LINESTRING (0 10, 0 5)",
               "LINESTRING (0 5, 0 0)",
               // Inner square split at (2, 5)
               "LINESTRING (2 3, 6 3)",
               "LINESTRING (6 3, 6 7)",
               "LINESTRING (6 7, 2 7)",
               "LINESTRING (2 7, 2 5)",
               "LINESTRING (2 5, 2 3)",
           },
           lines);
  const auto bridge_ptrs = AddLines(graph, {"LINESTRING (0 5, 2 5)"}, lines);

  std::vector<const LineString*> dangles;
  graph.deleteDangles(dangles);
  EXPECT_TRUE(dangles.empty());

  std::vector<const LineString*> cut_lines;
  graph.deleteCutEdges(cut_lines);
  ASSERT_EQ(cut_lines.size(), 1u);
  EXPECT_EQ(cut_lines[0], bridge_ptrs[0]);

  std::vector<EdgeRing*> edge_rings;
  graph.getEdgeRings(edge_rings);
  const RingSummary summary = SummarizeRings(edge_rings);
  EXPECT_EQ(summary.valid_count, 4);
  EXPECT_EQ(summary.invalid_count, 0);
  EXPECT_EQ(summary.shell_areas, (std::vector<double>{16.0, 100.0}));
  EXPECT_EQ(summary.hole_areas, (std::vector<double>{16.0, 100.0}));
}

TEST_F(PolygonizeGraphTest, DeleteCutEdgesDirectCallHandlesDangleAndBridge) {
  std::vector<std::unique_ptr<LineString>> lines;
  PolygonizeGraph graph(factory_.get());

  AddLines(graph,
           {
               "LINESTRING (0 0, 10 0)",
               "LINESTRING (10 0, 10 5)",
               "LINESTRING (10 5, 10 10)",
               "LINESTRING (10 10, 0 10)",
               "LINESTRING (0 10, 0 0)",
               "LINESTRING (20 0, 30 0)",
               "LINESTRING (30 0, 30 10)",
               "LINESTRING (30 10, 20 10)",
               "LINESTRING (20 10, 20 5)",
               "LINESTRING (20 5, 20 0)",
           },
           lines);
  const auto cut_or_dangle = AddLines(graph,
                                      {
                                          "LINESTRING (10 5, 20 5)",
                                          "LINESTRING (10 10, 12 12)",
                                      },
                                      lines);

  std::vector<const LineString*> cut_lines;
  graph.deleteCutEdges(cut_lines);
  EXPECT_EQ(cut_lines.size(), 2u);
  const std::set<const LineString*> expected(cut_or_dangle.begin(),
                                             cut_or_dangle.end());
  const std::set<const LineString*> actual(cut_lines.begin(), cut_lines.end());
  EXPECT_EQ(actual, expected);

  std::vector<EdgeRing*> edge_rings;
  graph.getEdgeRings(edge_rings);
  const RingSummary summary = SummarizeRings(edge_rings);
  EXPECT_EQ(summary.valid_count, 4);
  EXPECT_EQ(summary.invalid_count, 0);
  EXPECT_EQ(summary.shell_areas, (std::vector<double>{100.0, 100.0}));
  EXPECT_EQ(summary.hole_areas, (std::vector<double>{100.0, 100.0}));
}

TEST_F(PolygonizeGraphTest, ConvertsMaximalToMinimalRingsForBowtieQ1Q3) {
  std::vector<std::unique_ptr<LineString>> lines;
  PolygonizeGraph graph(factory_.get());

  AddLines(graph,
           {
               // Square in quadrant 1 touching (0, 0)
               "LINESTRING (0 0, 10 0)",
               "LINESTRING (10 0, 10 10)",
               "LINESTRING (10 10, 0 10)",
               "LINESTRING (0 10, 0 0)",
               // Square in quadrant 3 touching (0, 0)
               "LINESTRING (0 0, -10 0)",
               "LINESTRING (-10 0, -10 -10)",
               "LINESTRING (-10 -10, 0 -10)",
               "LINESTRING (0 -10, 0 0)",
           },
           lines);

  std::vector<const LineString*> dangles;
  graph.deleteDangles(dangles);
  EXPECT_TRUE(dangles.empty());

  std::vector<const LineString*> cut_lines;
  graph.deleteCutEdges(cut_lines);
  EXPECT_TRUE(cut_lines.empty());

  std::vector<EdgeRing*> edge_rings;
  graph.getEdgeRings(edge_rings);
  const RingSummary summary = SummarizeRings(edge_rings);
  EXPECT_EQ(summary.valid_count, 4);
  EXPECT_EQ(summary.invalid_count, 0);
  EXPECT_EQ(summary.shell_areas, (std::vector<double>{100.0, 100.0}));
  EXPECT_EQ(summary.hole_areas, (std::vector<double>{100.0, 100.0}));
  EXPECT_EQ(summary.edge_counts, (std::vector<std::size_t>{4, 4, 4, 4}));
}

TEST_F(PolygonizeGraphTest,
       ConvertsMaximalToMinimalRingsAcrossXAxisWithInnerAndDangleEdges) {
  std::vector<std::unique_ptr<LineString>> lines;
  PolygonizeGraph graph(factory_.get());

  AddLines(graph,
           {
               // East diamond straddling +x axis (area 200)
               "LINESTRING (0 0, 10 -10)",
               "LINESTRING (10 -10, 20 0)",
               "LINESTRING (20 0, 10 10)",
               "LINESTRING (10 10, 0 0)",
               // West diamond straddling -x axis (area 200)
               "LINESTRING (0 0, -10 10)",
               "LINESTRING (-10 10, -20 0)",
               "LINESTRING (-20 0, -10 -10)",
               "LINESTRING (-10 -10, 0 0)",
               // Inner diamond inside East diamond touching (0, 0) (area 20)
               "LINESTRING (0 0, 5 -2)",
               "LINESTRING (5 -2, 10 0)",
               "LINESTRING (10 0, 5 2)",
               "LINESTRING (5 2, 0 0)",
           },
           lines);
  const auto dangle_ptrs = AddLines(graph, {"LINESTRING (0 0, 0 5)"}, lines);

  std::vector<const LineString*> dangles;
  graph.deleteDangles(dangles);
  ASSERT_EQ(dangles.size(), 1u);
  EXPECT_EQ(dangles[0], dangle_ptrs[0]);

  std::vector<const LineString*> cut_lines;
  graph.deleteCutEdges(cut_lines);
  EXPECT_TRUE(cut_lines.empty());

  std::vector<EdgeRing*> edge_rings;
  graph.getEdgeRings(edge_rings);
  const RingSummary summary = SummarizeRings(edge_rings);
  EXPECT_EQ(summary.valid_count, 6);
  EXPECT_EQ(summary.invalid_count, 0);
  EXPECT_EQ(summary.shell_areas, (std::vector<double>{20.0, 200.0, 200.0}));
  EXPECT_EQ(summary.hole_areas, (std::vector<double>{20.0, 200.0, 200.0}));
}

TEST_F(PolygonizeGraphTest, PinchedHoleTouchingShellAtVertex) {
  std::vector<std::unique_ptr<LineString>> lines;
  PolygonizeGraph graph(factory_.get());

  AddLines(graph,
           {
               // Outer square (area 100)
               "LINESTRING (0 0, 10 0)",
               "LINESTRING (10 0, 10 10)",
               "LINESTRING (10 10, 0 10)",
               "LINESTRING (0 10, 0 0)",
               // Inner triangle touching outer square at (0, 0) (area 7.5)
               "LINESTRING (0 0, 4 1)",
               "LINESTRING (4 1, 1 4)",
               "LINESTRING (1 4, 0 0)",
           },
           lines);

  std::vector<const LineString*> dangles;
  graph.deleteDangles(dangles);
  EXPECT_TRUE(dangles.empty());

  std::vector<const LineString*> cut_lines;
  graph.deleteCutEdges(cut_lines);
  EXPECT_TRUE(cut_lines.empty());

  std::vector<EdgeRing*> edge_rings;
  graph.getEdgeRings(edge_rings);
  const RingSummary summary = SummarizeRings(edge_rings);
  EXPECT_EQ(summary.valid_count, 4);
  EXPECT_EQ(summary.invalid_count, 0);
  EXPECT_EQ(summary.shell_areas, (std::vector<double>{7.5, 100.0}));
  EXPECT_EQ(summary.hole_areas, (std::vector<double>{7.5, 100.0}));
  EXPECT_EQ(summary.edge_counts, (std::vector<std::size_t>{3, 3, 4, 4}));
}

TEST_F(PolygonizeGraphTest,
       SelfIntersectingAndCollapsedLoopsProduceInvalidRings) {
  {
    std::vector<std::unique_ptr<LineString>> lines;
    PolygonizeGraph graph(factory_.get());
    AddLines(graph, {"LINESTRING (0 0, 10 0, 0 10, 10 10, 0 0)"}, lines);

    std::vector<const LineString*> dangles;
    graph.deleteDangles(dangles);
    EXPECT_TRUE(dangles.empty());

    std::vector<const LineString*> cut_lines;
    graph.deleteCutEdges(cut_lines);
    EXPECT_TRUE(cut_lines.empty());

    std::vector<EdgeRing*> edge_rings;
    graph.getEdgeRings(edge_rings);
    const RingSummary summary = SummarizeRings(edge_rings);
    EXPECT_EQ(summary.valid_count, 0);
    EXPECT_EQ(summary.invalid_count, 2);
  }

  {
    std::vector<std::unique_ptr<LineString>> lines;
    PolygonizeGraph graph(factory_.get());
    AddLines(graph, {"LINESTRING (0 0, 10 0, 0 0)"}, lines);

    std::vector<EdgeRing*> edge_rings;
    graph.getEdgeRings(edge_rings);
    const RingSummary summary = SummarizeRings(edge_rings);
    EXPECT_EQ(summary.valid_count, 0);
    EXPECT_EQ(summary.invalid_count, 2);
  }
}

TEST_F(PolygonizeGraphTest,
       DeleteAllEdgesMarksIncidentAndSymmetricDirectedEdges) {
  {
    std::vector<std::unique_ptr<LineString>> lines;
    PolygonizeGraph graph(factory_.get());

    AddLines(graph,
             {
                 "LINESTRING (0 0, 10 0)",
                 "LINESTRING (0 0, 0 10)",
             },
             lines);

    Node* center = graph.findNode(Coordinate(0.0, 0.0));
    ASSERT_NE(center, nullptr);
    PolygonizeGraph::deleteAllEdges(center);

    for (auto it = graph.dirEdgeBegin(); it != graph.dirEdgeEnd(); ++it) {
      EXPECT_TRUE((*it)->isMarked());
    }

    std::vector<EdgeRing*> edge_rings;
    graph.getEdgeRings(edge_rings);
    EXPECT_TRUE(edge_rings.empty());
  }

  // Also test a standalone node whose DirectedEdge has sym == nullptr, and an
  // isolated node with no outgoing edges.
  {
    Node n0(Coordinate(0.0, 0.0));
    Node n1(Coordinate(1.0, 0.0));
    DirectedEdge de(&n0, &n1, Coordinate(1.0, 0.0), true);
    de.setEdge(nullptr);
    de.setSym(nullptr);
    n0.addOutEdge(&de);

    EXPECT_FALSE(de.isMarked());
    PolygonizeGraph::deleteAllEdges(&n0);
    EXPECT_TRUE(de.isMarked());

    Node empty_node(Coordinate(3.0, 4.0));
    PolygonizeGraph::deleteAllEdges(&empty_node);
    EXPECT_EQ(empty_node.getDegree(), 0u);
  }
}

}  // namespace
}  // namespace geos_google
