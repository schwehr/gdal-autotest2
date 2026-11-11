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

#include "third_party/geos/include/geos/operation/overlayng/PolygonBuilder.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/operation/overlayng/Edge.h"
#include "third_party/geos/include/geos/operation/overlayng/EdgeSourceInfo.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayEdge.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayEdgeRing.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayGraph.h"
#include "third_party/geos/include/geos/util/AssertionFailedException.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::GeometryFactory;
using geos::geom::Polygon;
using geos::operation::overlayng::Edge;
using geos::operation::overlayng::EdgeSourceInfo;
using geos::operation::overlayng::OverlayEdge;
using geos::operation::overlayng::OverlayEdgeRing;
using geos::operation::overlayng::OverlayGraph;
using geos::operation::overlayng::PolygonBuilder;

// Adds one directed boundary edge per segment of a closed ring (where
// ring_pts.front() == ring_pts.back()) and marks each forward edge as in the
// result area.
void AddRingEdges2D(OverlayGraph& graph,
                    const std::vector<CoordinateXY>& ring_pts,
                    const EdgeSourceInfo& info,
                    std::vector<OverlayEdge*>& result_edges) {
  for (std::size_t i = 0; i + 1 < ring_pts.size(); ++i) {
    auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
    seq->add(ring_pts[i]);
    seq->add(ring_pts[i + 1]);
    Edge edge(std::move(seq), &info);
    OverlayEdge* oe = graph.addEdge(&edge);
    oe->markInResultArea();
    result_edges.push_back(oe);
  }
}

void AddRingEdges3D(OverlayGraph& graph,
                    const std::vector<Coordinate>& ring_pts,
                    const EdgeSourceInfo& info,
                    std::vector<OverlayEdge*>& result_edges) {
  for (std::size_t i = 0; i + 1 < ring_pts.size(); ++i) {
    auto seq = std::make_unique<CoordinateSequence>(0u, true, false);
    seq->add(ring_pts[i]);
    seq->add(ring_pts[i + 1]);
    Edge edge(std::move(seq), &info);
    OverlayEdge* oe = graph.addEdge(&edge);
    oe->markInResultArea();
    result_edges.push_back(oe);
  }
}

class PolygonBuilderTest : public ::testing::Test {
 protected:
  PolygonBuilderTest() : gf_(GeometryFactory::create()) {}

  GeometryFactory::Ptr gf_;
};

TEST_F(PolygonBuilderTest, EmptyEdgesProducesNoPolygons) {
  std::vector<OverlayEdge*> empty_edges;
  PolygonBuilder builder(empty_edges, gf_.get());

  EXPECT_TRUE(builder.getShellRings().empty());
  EXPECT_TRUE(builder.getPolygons().empty());
}

TEST_F(PolygonBuilderTest, SingleShellPolygonAndShellRingsAccessors) {
  OverlayGraph graph;
  std::vector<OverlayEdge*> result_edges;
  EdgeSourceInfo shell_info(0, 1, false);
  AddRingEdges2D(graph, {{0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}},
                 shell_info, result_edges);

  PolygonBuilder builder(result_edges, gf_.get());
  std::vector<OverlayEdgeRing*> shells = builder.getShellRings();
  ASSERT_EQ(shells.size(), 1u);
  EXPECT_FALSE(shells[0]->isHole());
  EXPECT_FALSE(shells[0]->hasShell());
  EXPECT_EQ(shells[0]->getShell(), shells[0]);
  EXPECT_EQ(shells[0]->getRingPtr()->getNumPoints(), 5u);

  std::vector<std::unique_ptr<Polygon>> polys = builder.getPolygons();
  ASSERT_EQ(polys.size(), 1u);
  EXPECT_TRUE(polys[0]->isValid());
  EXPECT_EQ(polys[0]->getNumInteriorRing(), 0u);
  EXPECT_DOUBLE_EQ(polys[0]->getArea(), 100.0);
}

TEST_F(PolygonBuilderTest, MultipleDisjointShellsPreserveZCoordinates) {
  OverlayGraph graph;
  std::vector<OverlayEdge*> result_edges;
  EdgeSourceInfo shell_info(0, 1, false);
  AddRingEdges3D(graph, {{0, 0, 1}, {0, 4, 1}, {4, 4, 1}, {4, 0, 1}, {0, 0, 1}},
                 shell_info, result_edges);
  AddRingEdges3D(
      graph, {{10, 10, 5}, {10, 13, 5}, {14, 13, 5}, {14, 10, 5}, {10, 10, 5}},
      shell_info, result_edges);

  PolygonBuilder builder(result_edges, gf_.get());
  std::vector<std::unique_ptr<Polygon>> polys = builder.getPolygons();
  ASSERT_EQ(polys.size(), 2u);

  std::vector<double> areas;
  std::vector<double> z_values;
  for (const auto& poly : polys) {
    EXPECT_TRUE(poly->isValid());
    EXPECT_TRUE(poly->hasZ());
    ASSERT_GT(poly->getExteriorRing()->getNumPoints(), 0u);
    areas.push_back(poly->getArea());
    const double z = poly->getExteriorRing()->getCoordinateN(0).z;
    for (std::size_t i = 0; i < poly->getExteriorRing()->getNumPoints(); ++i) {
      EXPECT_DOUBLE_EQ(poly->getExteriorRing()->getCoordinateN(i).z, z);
    }
    z_values.push_back(z);
  }
  std::sort(areas.begin(), areas.end());
  std::sort(z_values.begin(), z_values.end());
  EXPECT_DOUBLE_EQ(areas[0], 12.0);
  EXPECT_DOUBLE_EQ(areas[1], 16.0);
  EXPECT_DOUBLE_EQ(z_values[0], 1.0);
  EXPECT_DOUBLE_EQ(z_values[1], 5.0);
}

void ExpectNestedIslandPolygons(const GeometryFactory* gf, bool island_first,
                                bool use_three_arg_ctor) {
  OverlayGraph graph;
  std::vector<OverlayEdge*> result_edges;
  EdgeSourceInfo shell_info(0, 1, false);
  EdgeSourceInfo hole_info(0, 1, true);

  auto add_outer = [&]() {
    // Outer shell [0,20]x[0,20] (CW, area 400).
    AddRingEdges2D(graph, {{0, 0}, {0, 20}, {20, 20}, {20, 0}, {0, 0}},
                   shell_info, result_edges);
    // Outer hole [2,18]x[2,18] (CCW, area 256).
    AddRingEdges2D(graph, {{2, 2}, {18, 2}, {18, 18}, {2, 18}, {2, 2}},
                   hole_info, result_edges);
  };
  auto add_island = [&]() {
    // Island shell [6,14]x[6,14] inside the hole (CW, area 64).
    AddRingEdges2D(graph, {{6, 6}, {6, 14}, {14, 14}, {14, 6}, {6, 6}},
                   shell_info, result_edges);
    // Inner hole [8,12]x[8,12] inside the island (CCW, area 16).
    AddRingEdges2D(graph, {{8, 8}, {12, 8}, {12, 12}, {8, 12}, {8, 8}},
                   hole_info, result_edges);
  };

  if (island_first) {
    add_island();
    add_outer();
  } else {
    add_outer();
    add_island();
  }

  std::unique_ptr<PolygonBuilder> builder =
      use_three_arg_ctor
          ? std::make_unique<PolygonBuilder>(result_edges, gf, true)
          : std::make_unique<PolygonBuilder>(result_edges, gf);
  EXPECT_EQ(builder->getShellRings().size(), 2u);

  std::vector<std::unique_ptr<Polygon>> polys = builder->getPolygons();
  ASSERT_EQ(polys.size(), 2u);

  std::vector<double> areas;
  for (const auto& poly : polys) {
    EXPECT_TRUE(poly->isValid());
    EXPECT_EQ(poly->getNumInteriorRing(), 1u);
    areas.push_back(poly->getArea());
  }
  std::sort(areas.begin(), areas.end());
  // Island: 64 - 16 = 48; outer polygon: 400 - 256 = 144.
  EXPECT_DOUBLE_EQ(areas[0], 48.0);
  EXPECT_DOUBLE_EQ(areas[1], 144.0);
}

TEST_F(PolygonBuilderTest, DisconnectedFreeHolesAndNestedIsland) {
  ExpectNestedIslandPolygons(gf_.get(), /*island_first=*/false,
                             /*use_three_arg_ctor=*/false);
  ExpectNestedIslandPolygons(gf_.get(), /*island_first=*/true,
                             /*use_three_arg_ctor=*/true);
}

TEST_F(PolygonBuilderTest, HoleTouchingShellAtVertex) {
  OverlayGraph graph;
  std::vector<OverlayEdge*> result_edges;
  EdgeSourceInfo shell_info(0, 1, false);
  EdgeSourceInfo hole_info(0, 1, true);

  // Outer CW shell with vertex (5,0) shared by the hole.
  AddRingEdges2D(graph, {{0, 0}, {0, 10}, {10, 10}, {10, 0}, {5, 0}, {0, 0}},
                 shell_info, result_edges);
  // Inner CCW triangular hole touching the shell at (5,0).
  AddRingEdges2D(graph, {{5, 0}, {7, 4}, {3, 4}, {5, 0}}, hole_info,
                 result_edges);

  PolygonBuilder builder(result_edges, gf_.get());
  ASSERT_EQ(builder.getShellRings().size(), 1u);

  std::vector<std::unique_ptr<Polygon>> polys = builder.getPolygons();
  ASSERT_EQ(polys.size(), 1u);
  EXPECT_TRUE(polys[0]->isValid());
  EXPECT_EQ(polys[0]->getNumInteriorRing(), 1u);
  EXPECT_DOUBLE_EQ(polys[0]->getArea(), 92.0);
}

TEST_F(PolygonBuilderTest, TwoTouchingHolesInsideDisconnectedShell) {
  OverlayGraph graph;
  std::vector<OverlayEdge*> result_edges;
  EdgeSourceInfo shell_info(0, 1, false);
  EdgeSourceInfo hole_info(0, 1, true);

  // Disconnected outer CW shell [0,20]x[0,20] (area 400).
  AddRingEdges2D(graph, {{0, 0}, {0, 20}, {20, 20}, {20, 0}, {0, 0}},
                 shell_info, result_edges);
  // Two CCW triangular holes touching at (10,10), forming one MaximalEdgeRing
  // of all holes (findSingleShell returns nullptr -> placed in freeHoleList).
  AddRingEdges2D(graph, {{10, 10}, {4, 14}, {4, 6}, {10, 10}}, hole_info,
                 result_edges);
  AddRingEdges2D(graph, {{10, 10}, {16, 6}, {16, 14}, {10, 10}}, hole_info,
                 result_edges);

  PolygonBuilder builder(result_edges, gf_.get());
  ASSERT_EQ(builder.getShellRings().size(), 1u);

  std::vector<std::unique_ptr<Polygon>> polys = builder.getPolygons();
  ASSERT_EQ(polys.size(), 1u);
  EXPECT_TRUE(polys[0]->isValid());
  EXPECT_EQ(polys[0]->getNumInteriorRing(), 2u);
  EXPECT_DOUBLE_EQ(polys[0]->getArea(), 352.0);
}

TEST_F(PolygonBuilderTest, NonBoundaryEdgesMarkedInResultAreaAreSkipped) {
  OverlayGraph graph;
  std::vector<OverlayEdge*> result_edges;

  EdgeSourceInfo shell_info(0, 1, false);
  AddRingEdges2D(graph, {{0, 0}, {0, 4}, {4, 4}, {4, 0}, {0, 0}}, shell_info,
                 result_edges);

  // Add linear edges marked as in-result-area: they pass node-linking
  // preconditions but are skipped by buildMaximalRings because
  // !label->isBoundaryEither().
  EdgeSourceInfo line_info(0);
  AddRingEdges2D(graph, {{10, 0}, {14, 0}, {12, 4}, {10, 0}}, line_info,
                 result_edges);

  PolygonBuilder builder(result_edges, gf_.get());
  std::vector<std::unique_ptr<Polygon>> polys = builder.getPolygons();
  ASSERT_EQ(polys.size(), 1u);
  EXPECT_DOUBLE_EQ(polys[0]->getArea(), 16.0);
}

TEST_F(PolygonBuilderTest, OrphanHoleThrowsWhenEnforcePolygonalTrue) {
  OverlayGraph graph;
  std::vector<OverlayEdge*> result_edges;
  EdgeSourceInfo hole_info(0, 1, true);
  AddRingEdges2D(graph, {{2, 2}, {8, 2}, {8, 8}, {2, 8}, {2, 2}}, hole_info,
                 result_edges);

  EXPECT_THROW(
      { PolygonBuilder builder(result_edges, gf_.get()); },
      geos::util::TopologyException);

  // Re-create graph cleanly because maximal/minimal ring pointers were mutated.
  OverlayGraph graph2;
  std::vector<OverlayEdge*> result_edges2;
  AddRingEdges2D(graph2, {{2, 2}, {8, 2}, {8, 8}, {2, 8}, {2, 2}}, hole_info,
                 result_edges2);
  bool caught = false;
  try {
    PolygonBuilder builder(result_edges2, gf_.get(), true);
  } catch (const geos::util::TopologyException& ex) {
    caught = true;
    EXPECT_NE(
        std::string(ex.what()).find("unable to assign free hole to a shell"),
        std::string::npos);
  }
  EXPECT_TRUE(caught);
}

TEST_F(PolygonBuilderTest, OrphanHoleAllowedWhenEnforcePolygonalFalse) {
  // Pure orphan hole with enforcePolygonal = false returns no polygons.
  {
    OverlayGraph graph;
    std::vector<OverlayEdge*> result_edges;
    EdgeSourceInfo hole_info(0, 1, true);
    AddRingEdges2D(graph, {{2, 2}, {8, 2}, {8, 8}, {2, 8}, {2, 2}}, hole_info,
                   result_edges);

    PolygonBuilder builder(result_edges, gf_.get(), false);
    EXPECT_TRUE(builder.getShellRings().empty());
    EXPECT_TRUE(builder.getPolygons().empty());
  }

  // Shell plus an uncontained external hole with enforcePolygonal = false.
  {
    OverlayGraph graph;
    std::vector<OverlayEdge*> result_edges;
    EdgeSourceInfo shell_info(0, 1, false);
    EdgeSourceInfo hole_info(0, 1, true);
    AddRingEdges2D(graph, {{0, 0}, {0, 4}, {4, 4}, {4, 0}, {0, 0}}, shell_info,
                   result_edges);
    AddRingEdges2D(graph, {{10, 10}, {14, 10}, {14, 14}, {10, 14}, {10, 10}},
                   hole_info, result_edges);

    PolygonBuilder builder(result_edges, gf_.get(), false);
    std::vector<std::unique_ptr<Polygon>> polys = builder.getPolygons();
    ASSERT_EQ(polys.size(), 1u);
    EXPECT_EQ(polys[0]->getNumInteriorRing(), 0u);
    EXPECT_DOUBLE_EQ(polys[0]->getArea(), 16.0);
  }
}

TEST_F(PolygonBuilderTest, TwoShellsInSingleMaximalRingThrowsAssertion) {
  // Construct two CW shells meeting at (5,5) whose incident half-edges
  // alternate (outA 0 deg, inA 90 deg, outB 180 deg, inB 270 deg) so
  // MaximalEdgeRing links both into one maximal ring and splits them into two
  // minimal rings that are both shells, triggering findSingleShell's assert.
  OverlayGraph graph;
  std::vector<OverlayEdge*> result_edges;
  EdgeSourceInfo shell_info(0, 1, false);

  AddRingEdges2D(graph, {{5, 5}, {6, 5}, {0, 0}, {5, 6}, {5, 5}}, shell_info,
                 result_edges);
  AddRingEdges2D(graph, {{5, 5}, {4, 5}, {10, 10}, {5, 4}, {5, 5}}, shell_info,
                 result_edges);

  EXPECT_THROW(
      { PolygonBuilder builder(result_edges, gf_.get()); },
      geos::util::AssertionFailedException);
}

TEST_F(PolygonBuilderTest, CollapsedAndGeometryBEdgesHandledCorrectly) {
  OverlayGraph graph;
  std::vector<OverlayEdge*> result_edges;

  // Geometry index 1 (aDim = NOT_PART, bDim = BOUNDARY) exercises the second
  // branch of OverlayLabel::isBoundaryEither() in buildMaximalRings.
  EdgeSourceInfo b_shell_info(1, 1, false);
  EdgeSourceInfo b_hole_info(1, 1, true);
  AddRingEdges2D(graph, {{0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}},
                 b_shell_info, result_edges);
  AddRingEdges2D(graph, {{2, 2}, {8, 2}, {8, 8}, {2, 8}, {2, 2}}, b_hole_info,
                 result_edges);

  // Collapsed area edges (depthDelta == 0 -> DIM_COLLAPSE) marked in result
  // area pass node linking but are skipped by buildMaximalRings.
  EdgeSourceInfo collapse_info(0, 0, false);
  AddRingEdges2D(graph, {{20, 0}, {24, 0}, {22, 4}, {20, 0}}, collapse_info,
                 result_edges);

  PolygonBuilder builder(result_edges, gf_.get(), true);
  std::vector<std::unique_ptr<Polygon>> polys = builder.getPolygons();
  ASSERT_EQ(polys.size(), 1u);
  EXPECT_TRUE(polys[0]->isValid());
  EXPECT_EQ(polys[0]->getNumInteriorRing(), 1u);
  EXPECT_DOUBLE_EQ(polys[0]->getArea(), 64.0);
}

TEST_F(PolygonBuilderTest, ShellWithMultipleAttachedHolesAndFreeHole) {
  OverlayGraph graph;
  std::vector<OverlayEdge*> result_edges;
  EdgeSourceInfo shell_info(0, 1, false);
  EdgeSourceInfo hole_info(0, 1, true);

  // Outer CW shell [0,10]x[0,10] (area 100) sharing (5,0) and (5,10) with two
  // attached triangular holes.
  AddRingEdges2D(graph,
                 {{0, 0}, {0, 10}, {5, 10}, {10, 10}, {10, 0}, {5, 0}, {0, 0}},
                 shell_info, result_edges);
  // CCW hole touching at (5,0), area = 6.
  AddRingEdges2D(graph, {{5, 0}, {7, 3}, {3, 3}, {5, 0}}, hole_info,
                 result_edges);
  // CCW hole touching at (5,10), area = 6.
  AddRingEdges2D(graph, {{5, 10}, {3, 7}, {7, 7}, {5, 10}}, hole_info,
                 result_edges);
  // Disconnected CCW hole inside [1,2]x[4,6], area = 2.
  AddRingEdges2D(graph, {{1, 4}, {2, 4}, {2, 6}, {1, 6}, {1, 4}}, hole_info,
                 result_edges);

  PolygonBuilder builder(result_edges, gf_.get());
  ASSERT_EQ(builder.getShellRings().size(), 1u);
  std::vector<std::unique_ptr<Polygon>> polys = builder.getPolygons();
  ASSERT_EQ(polys.size(), 1u);
  EXPECT_TRUE(polys[0]->isValid());
  EXPECT_EQ(polys[0]->getNumInteriorRing(), 3u);
  EXPECT_DOUBLE_EQ(polys[0]->getArea(), 86.0);
}

TEST_F(PolygonBuilderTest, MultiPointEdgesBuildValidPolygon) {
  OverlayGraph graph;
  std::vector<OverlayEdge*> result_edges;
  EdgeSourceInfo shell_info(0, 1, false);

  auto add_multi_pt_edge = [&](const std::vector<CoordinateXY>& pts) {
    auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
    for (const CoordinateXY& pt : pts) {
      seq->add(pt);
    }
    Edge edge(std::move(seq), &shell_info);
    OverlayEdge* oe = graph.addEdge(&edge);
    oe->markInResultArea();
    result_edges.push_back(oe);
  };

  add_multi_pt_edge({{0, 0}, {0, 6}, {6, 6}});
  add_multi_pt_edge({{6, 6}, {6, 0}, {0, 0}});

  PolygonBuilder builder(result_edges, gf_.get());
  std::vector<std::unique_ptr<Polygon>> polys = builder.getPolygons();
  ASSERT_EQ(polys.size(), 1u);
  EXPECT_TRUE(polys[0]->isValid());
  EXPECT_EQ(polys[0]->getNumPoints(), 5u);
  EXPECT_DOUBLE_EQ(polys[0]->getArea(), 36.0);
}

}  // namespace
}  // namespace geos_google
