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

// Unit tests for third_party/geos/src/operation/overlayng/LineBuilder.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/overlayng/LineBuilder.cpp

#include "third_party/geos/include/geos/operation/overlayng/LineBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/overlayng/Edge.h"
#include "third_party/geos/include/geos/operation/overlayng/EdgeSourceInfo.h"
#include "third_party/geos/include/geos/operation/overlayng/InputGeometry.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayEdge.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayGraph.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayLabel.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayNG.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::geom::Location;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::operation::overlayng::Edge;
using geos::operation::overlayng::EdgeSourceInfo;
using geos::operation::overlayng::InputGeometry;
using geos::operation::overlayng::LineBuilder;
using geos::operation::overlayng::OverlayEdge;
using geos::operation::overlayng::OverlayGraph;
using geos::operation::overlayng::OverlayLabel;
using geos::operation::overlayng::OverlayNG;

OverlayEdge* AddLineEdge(OverlayGraph& graph, const CoordinateXY& p0,
                         const CoordinateXY& p1, uint8_t geom_index) {
  auto cs = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{p0, p1});
  EdgeSourceInfo info(geom_index);
  Edge edge(std::move(cs), &info);
  return graph.addEdge(&edge);
}

OverlayEdge* AddBoundaryEdge(OverlayGraph& graph, const CoordinateXY& p0,
                             const CoordinateXY& p1, uint8_t geom_index,
                             int depth_delta, bool is_hole) {
  auto cs = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{p0, p1});
  EdgeSourceInfo info(geom_index, depth_delta, is_hole);
  Edge edge(std::move(cs), &info);
  return graph.addEdge(&edge);
}

std::vector<double> SortedStartXs(
    const std::vector<std::unique_ptr<LineString>>& lines) {
  std::vector<double> xs;
  xs.reserve(lines.size());
  for (const auto& line : lines) {
    xs.push_back(line->getCoordinateN(0).x);
  }
  std::sort(xs.begin(), xs.end());
  return xs;
}

TEST(LineBuilderTest, EmptyGraphProducesNoLines) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  Geometry::Ptr line_a = factory->createLineString();
  Geometry::Ptr line_b = factory->createLineString();
  InputGeometry input(line_a.get(), line_b.get());
  OverlayGraph graph;

  LineBuilder builder(&input, &graph, /*p_hasResultArea=*/false,
                      OverlayNG::INTERSECTION, factory.get());
  EXPECT_TRUE(builder.getLines().empty());
}

TEST(LineBuilderTest, SkipsEdgesAlreadyMarkedInResult) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  Geometry::Ptr line_a = factory->createLineString();
  Geometry::Ptr line_b = factory->createLineString();
  InputGeometry input(line_a.get(), line_b.get());
  OverlayGraph graph;

  // e1 has forward half-edge marked in result area -> skipped in
  // markResultLines and addResultLines.
  OverlayEdge* e1 = AddLineEdge(graph, {0.0, 0.0}, {1.0, 0.0}, 0);
  e1->getLabel()->initLine(1);
  e1->markInResultArea();

  // e2 has symmetric half-edge marked in result area -> isInResultEither() is
  // true -> skipped.
  OverlayEdge* e2 = AddLineEdge(graph, {2.0, 0.0}, {3.0, 0.0}, 0);
  e2->getLabel()->initLine(1);
  e2->symOE()->markInResultArea();

  // e3 is already marked in result line -> skipped in markResultLines, emitted
  // once in addResultLines while symOE is skipped via isVisited().
  OverlayEdge* e3 = AddLineEdge(graph, {4.0, 0.0}, {5.0, 0.0}, 0);
  e3->getLabel()->initLine(1);
  e3->markInResultLine();

  LineBuilder builder(&input, &graph, /*p_hasResultArea=*/false,
                      OverlayNG::INTERSECTION, factory.get());
  std::vector<std::unique_ptr<LineString>> lines = builder.getLines();

  ASSERT_EQ(lines.size(), 1u);
  EXPECT_DOUBLE_EQ(lines[0]->getCoordinateN(0).x, 4.0);
  EXPECT_DOUBLE_EQ(lines[0]->getCoordinateN(1).x, 5.0);
}

TEST(LineBuilderTest, BoundarySingletonEdgesAreExcludedAcrossAllOps) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  Geometry::Ptr empty_a = factory->createPolygon();
  Geometry::Ptr empty_b = factory->createPolygon();
  InputGeometry input(empty_a.get(), empty_b.get());

  const int ops[] = {OverlayNG::INTERSECTION, OverlayNG::UNION,
                     OverlayNG::DIFFERENCE, OverlayNG::SYMDIFFERENCE};
  for (int op : ops) {
    for (bool strict : {false, true}) {
      OverlayGraph graph;
      OverlayEdge* e_a =
          AddBoundaryEdge(graph, {0.0, 0.0}, {1.0, 0.0}, 0, 1, false);
      e_a->getLabel()->setLocationLine(1, Location::EXTERIOR);
      OverlayEdge* e_b =
          AddBoundaryEdge(graph, {2.0, 0.0}, {3.0, 0.0}, 1, 1, false);
      e_b->getLabel()->setLocationLine(0, Location::EXTERIOR);

      LineBuilder builder(&input, &graph, /*p_hasResultArea=*/false, op,
                          factory.get());
      builder.setStrictMode(strict);
      EXPECT_TRUE(builder.getLines().empty());
    }
  }
}

TEST(LineBuilderTest, BoundaryCollapseEdgesRespectStrictMode) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  Geometry::Ptr empty_a = factory->createPolygon();
  Geometry::Ptr empty_b = factory->createLineString();
  InputGeometry input(empty_a.get(), empty_b.get());

  auto populate_graph = [](OverlayGraph& graph) {
    // Collapse on A, NotPart/Exterior on B.
    OverlayEdge* e0 =
        AddBoundaryEdge(graph, {0.0, 0.0}, {1.0, 0.0}, 0, 0, false);
    e0->getLabel()->setLocationLine(0, Location::EXTERIOR);
    e0->getLabel()->setLocationLine(1, Location::EXTERIOR);

    // NotPart/Exterior on A, Collapse on B.
    OverlayEdge* e1 =
        AddBoundaryEdge(graph, {2.0, 0.0}, {3.0, 0.0}, 1, 0, false);
    e1->getLabel()->setLocationLine(0, Location::EXTERIOR);
    e1->getLabel()->setLocationLine(1, Location::EXTERIOR);

    // Collapse on A, Boundary on B.
    OverlayEdge* e2 =
        AddBoundaryEdge(graph, {4.0, 0.0}, {5.0, 0.0}, 0, 0, false);
    e2->getLabel()->setLocationLine(0, Location::EXTERIOR);
    e2->getLabel()->initBoundary(1, Location::EXTERIOR, Location::INTERIOR,
                                 false);

    // Collapse on A, Line on B (not a boundary collapse since isLine() is
    // true).
    OverlayEdge* e3 =
        AddBoundaryEdge(graph, {6.0, 0.0}, {7.0, 0.0}, 0, 0, false);
    e3->getLabel()->setLocationLine(0, Location::EXTERIOR);
    e3->getLabel()->initLine(1);
  };

  // Strict mode under UNION: filters boundary collapses e0, e1, e2, keeps e3.
  {
    OverlayGraph graph;
    populate_graph(graph);
    LineBuilder builder(&input, &graph, /*p_hasResultArea=*/false,
                        OverlayNG::UNION, factory.get());
    builder.setStrictMode(true);
    std::vector<double> xs = SortedStartXs(builder.getLines());
    EXPECT_EQ(xs, std::vector<double>({6.0}));
  }

  // Non-strict mode under UNION: allows collapse edges -> all 4 included.
  {
    OverlayGraph graph;
    populate_graph(graph);
    LineBuilder builder(&input, &graph, /*p_hasResultArea=*/false,
                        OverlayNG::UNION, factory.get());
    builder.setStrictMode(false);
    std::vector<double> xs = SortedStartXs(builder.getLines());
    EXPECT_EQ(xs, std::vector<double>({0.0, 2.0, 4.0, 6.0}));
  }

  // Non-strict mode under INTERSECTION: only e2 and e3 have INTERIOR effective
  // location for both inputs.
  {
    OverlayGraph graph;
    populate_graph(graph);
    LineBuilder builder(&input, &graph, /*p_hasResultArea=*/false,
                        OverlayNG::INTERSECTION, factory.get());
    builder.setStrictMode(false);
    std::vector<double> xs = SortedStartXs(builder.getLines());
    EXPECT_EQ(xs, std::vector<double>({4.0, 6.0}));
  }
}

TEST(LineBuilderTest, InteriorCollapseEdgesAlwaysExcluded) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  Geometry::Ptr empty_a = factory->createPolygon();
  Geometry::Ptr empty_b = factory->createLineString();
  InputGeometry input(empty_a.get(), empty_b.get());

  for (int op : {OverlayNG::INTERSECTION, OverlayNG::UNION}) {
    OverlayGraph graph;

    // Collapse on A with INTERIOR location, Line on B.
    OverlayEdge* e0 =
        AddBoundaryEdge(graph, {0.0, 0.0}, {1.0, 0.0}, 0, 0, true);
    e0->getLabel()->setLocationLine(0, Location::INTERIOR);
    e0->getLabel()->initLine(1);

    // Line on A, Collapse on B with INTERIOR location.
    OverlayEdge* e1 =
        AddBoundaryEdge(graph, {2.0, 0.0}, {3.0, 0.0}, 1, 0, true);
    e1->getLabel()->setLocationLine(1, Location::INTERIOR);
    e1->getLabel()->initLine(0);

    LineBuilder builder(&input, &graph, /*p_hasResultArea=*/false, op,
                        factory.get());
    builder.setStrictMode(false);
    EXPECT_TRUE(builder.getLines().empty());
  }
}

TEST(LineBuilderTest, CollapseAndNotPartInteriorExcludedForNonIntersectionOps) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  Geometry::Ptr empty_a = factory->createPolygon();
  Geometry::Ptr empty_b = factory->createPolygon();
  InputGeometry input(empty_a.get(), empty_b.get());

  auto populate_graph = [](OverlayGraph& graph) {
    // Collapse on A (exterior to A) inside B (NotPart + INTERIOR).
    OverlayEdge* e0 =
        AddBoundaryEdge(graph, {0.0, 0.0}, {1.0, 0.0}, 0, 0, false);
    e0->getLabel()->setLocationLine(0, Location::EXTERIOR);
    e0->getLabel()->setLocationLine(1, Location::INTERIOR);

    // NotPart + INTERIOR on A, Collapse on B (exterior to B).
    OverlayEdge* e1 =
        AddBoundaryEdge(graph, {2.0, 0.0}, {3.0, 0.0}, 1, 0, false);
    e1->getLabel()->setLocationLine(0, Location::INTERIOR);
    e1->getLabel()->setLocationLine(1, Location::EXTERIOR);
  };

  for (int op :
       {OverlayNG::UNION, OverlayNG::DIFFERENCE, OverlayNG::SYMDIFFERENCE}) {
    OverlayGraph graph;
    populate_graph(graph);
    LineBuilder builder(&input, &graph, /*p_hasResultArea=*/false, op,
                        factory.get());
    builder.setStrictMode(false);
    EXPECT_TRUE(builder.getLines().empty());
  }

  // Under INTERSECTION, both collapsed edges inside the other area are kept.
  {
    OverlayGraph graph;
    populate_graph(graph);
    LineBuilder builder(&input, &graph, /*p_hasResultArea=*/false,
                        OverlayNG::INTERSECTION, factory.get());
    builder.setStrictMode(false);
    std::vector<double> xs = SortedStartXs(builder.getLines());
    EXPECT_EQ(xs, std::vector<double>({0.0, 2.0}));
  }
}

TEST(LineBuilderTest, LineInsideResultAreaExcludedForNonIntersectionOps) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  Geometry::Ptr area = factory->createPolygon();
  Geometry::Ptr line = factory->createLineString();

  // Case 1: Area is geometry 0 (inputAreaIndex == 0), Line is geometry 1.
  {
    InputGeometry input(area.get(), line.get());
    auto populate_graph = [](OverlayGraph& graph) {
      OverlayEdge* inside = AddLineEdge(graph, {0.0, 0.0}, {1.0, 0.0}, 1);
      inside->getLabel()->setLocationLine(0, Location::INTERIOR);
      OverlayEdge* outside = AddLineEdge(graph, {2.0, 0.0}, {3.0, 0.0}, 1);
      outside->getLabel()->setLocationLine(0, Location::EXTERIOR);
    };

    // With hasResultArea=true and UNION, edge inside area 0 is omitted.
    {
      OverlayGraph graph;
      populate_graph(graph);
      LineBuilder builder(&input, &graph, /*p_hasResultArea=*/true,
                          OverlayNG::UNION, factory.get());
      EXPECT_EQ(SortedStartXs(builder.getLines()), std::vector<double>({2.0}));
    }

    // With hasResultArea=false and UNION, both are retained.
    {
      OverlayGraph graph;
      populate_graph(graph);
      LineBuilder builder(&input, &graph, /*p_hasResultArea=*/false,
                          OverlayNG::UNION, factory.get());
      EXPECT_EQ(SortedStartXs(builder.getLines()),
                std::vector<double>({0.0, 2.0}));
    }

    // With hasResultArea=true and INTERSECTION, inside edge is kept.
    {
      OverlayGraph graph;
      populate_graph(graph);
      LineBuilder builder(&input, &graph, /*p_hasResultArea=*/true,
                          OverlayNG::INTERSECTION, factory.get());
      EXPECT_EQ(SortedStartXs(builder.getLines()), std::vector<double>({0.0}));
    }
  }

  // Case 2: Line is geometry 0, Area is geometry 1 (inputAreaIndex == 1).
  {
    InputGeometry input(line.get(), area.get());
    OverlayGraph graph;
    OverlayEdge* inside = AddLineEdge(graph, {0.0, 0.0}, {1.0, 0.0}, 0);
    inside->getLabel()->setLocationLine(1, Location::INTERIOR);
    OverlayEdge* outside = AddLineEdge(graph, {2.0, 0.0}, {3.0, 0.0}, 0);
    outside->getLabel()->setLocationLine(1, Location::EXTERIOR);

    LineBuilder builder(&input, &graph, /*p_hasResultArea=*/true,
                        OverlayNG::DIFFERENCE, factory.get());
    EXPECT_EQ(SortedStartXs(builder.getLines()), std::vector<double>({2.0}));
  }
}

TEST(LineBuilderTest,
     BoundaryTouchInIntersectionRespectsMixedResultAndBooleanLogic) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  Geometry::Ptr poly_a = factory->createPolygon();
  Geometry::Ptr poly_b = factory->createPolygon();
  InputGeometry input(poly_a.get(), poly_b.get());

  auto populate_graph = [](OverlayGraph& graph) {
    // Opposite RIGHT-side locations -> isBoundaryTouch() == true.
    OverlayEdge* touch =
        AddBoundaryEdge(graph, {0.0, 0.0}, {1.0, 0.0}, 0, 1, false);
    touch->getLabel()->initBoundary(1, Location::INTERIOR, Location::EXTERIOR,
                                    false);

    // Same RIGHT-side locations -> isBoundaryTouch() == false.
    OverlayEdge* same_side =
        AddBoundaryEdge(graph, {2.0, 0.0}, {3.0, 0.0}, 0, 1, false);
    same_side->getLabel()->initBoundary(1, Location::EXTERIOR,
                                        Location::INTERIOR, false);
  };

  for (bool strict : {false, true}) {
    OverlayGraph graph;
    populate_graph(graph);
    LineBuilder builder(&input, &graph, /*p_hasResultArea=*/false,
                        OverlayNG::INTERSECTION, factory.get());
    builder.setStrictMode(strict);
    EXPECT_EQ(SortedStartXs(builder.getLines()),
              std::vector<double>({0.0, 2.0}));
  }

  {
    OverlayGraph graph;
    populate_graph(graph);
    LineBuilder builder(&input, &graph, /*p_hasResultArea=*/false,
                        OverlayNG::UNION, factory.get());
    builder.setStrictMode(false);
    EXPECT_EQ(SortedStartXs(builder.getLines()),
              std::vector<double>({0.0, 2.0}));
  }

  {
    OverlayGraph graph;
    populate_graph(graph);
    LineBuilder builder(&input, &graph, /*p_hasResultArea=*/false,
                        OverlayNG::DIFFERENCE, factory.get());
    builder.setStrictMode(false);
    EXPECT_TRUE(builder.getLines().empty());
  }
}

TEST(LineBuilderTest, OverlayOperationsBooleanLogicMatrix) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  Geometry::Ptr line_a = factory->createLineString();
  Geometry::Ptr line_b = factory->createLineString();
  InputGeometry input(line_a.get(), line_b.get());

  auto populate_graph = [](OverlayGraph& graph) {
    OverlayEdge* a_only = AddLineEdge(graph, {0.0, 0.0}, {1.0, 0.0}, 0);
    a_only->getLabel()->setLocationLine(1, Location::EXTERIOR);

    OverlayEdge* b_only = AddLineEdge(graph, {2.0, 0.0}, {3.0, 0.0}, 1);
    b_only->getLabel()->setLocationLine(0, Location::EXTERIOR);

    OverlayEdge* both = AddLineEdge(graph, {4.0, 0.0}, {5.0, 0.0}, 0);
    both->getLabel()->initLine(1);
  };

  struct OpExpectation {
    int op;
    std::vector<double> expected_xs;
  };
  const std::vector<OpExpectation> cases = {
      {OverlayNG::INTERSECTION, {4.0}},
      {OverlayNG::UNION, {0.0, 2.0, 4.0}},
      {OverlayNG::DIFFERENCE, {0.0}},
      {OverlayNG::SYMDIFFERENCE, {0.0, 2.0}},
  };

  for (const auto& tc : cases) {
    OverlayGraph graph;
    populate_graph(graph);
    LineBuilder builder(&input, &graph, /*p_hasResultArea=*/false, tc.op,
                        factory.get());
    EXPECT_EQ(SortedStartXs(builder.getLines()), tc.expected_xs);
  }
}

TEST(LineBuilderTest, ToLinePreservesMultiVertexCoordinatesZAndM) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  Geometry::Ptr line_a = factory->createLineString();
  Geometry::Ptr line_b = factory->createLineString();
  InputGeometry input(line_a.get(), line_b.get());

  // Multi-vertex 2D XY edge.
  {
    OverlayGraph graph;
    auto cs = std::make_unique<CoordinateSequence>(
        std::initializer_list<CoordinateXY>{
            {0.0, 0.0}, {1.0, 2.0}, {3.0, 2.0}, {4.0, 0.0}});
    EdgeSourceInfo info(0);
    Edge edge(std::move(cs), &info);
    OverlayEdge* oe = graph.addEdge(&edge);
    oe->getLabel()->initLine(1);

    LineBuilder builder(&input, &graph, false, OverlayNG::INTERSECTION,
                        factory.get());
    std::vector<std::unique_ptr<LineString>> lines = builder.getLines();
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_FALSE(lines[0]->hasZ());
    EXPECT_FALSE(lines[0]->hasM());
    ASSERT_EQ(lines[0]->getNumPoints(), 4u);
    EXPECT_EQ(lines[0]->getCoordinateN(0), CoordinateXY(0.0, 0.0));
    EXPECT_EQ(lines[0]->getCoordinateN(1), CoordinateXY(1.0, 2.0));
    EXPECT_EQ(lines[0]->getCoordinateN(2), CoordinateXY(3.0, 2.0));
    EXPECT_EQ(lines[0]->getCoordinateN(3), CoordinateXY(4.0, 0.0));
  }

  // Multi-vertex 3D XYZ edge.
  {
    OverlayGraph graph;
    auto cs =
        std::make_unique<CoordinateSequence>(std::initializer_list<Coordinate>{
            {0.0, 0.0, 10.0}, {1.0, 1.0, 20.0}, {2.0, 0.0, 30.0}});
    EdgeSourceInfo info(0);
    Edge edge(std::move(cs), &info);
    OverlayEdge* oe = graph.addEdge(&edge);
    oe->getLabel()->initLine(1);

    LineBuilder builder(&input, &graph, false, OverlayNG::INTERSECTION,
                        factory.get());
    std::vector<std::unique_ptr<LineString>> lines = builder.getLines();
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_TRUE(lines[0]->hasZ());
    EXPECT_FALSE(lines[0]->hasM());
    const CoordinateSequence* seq = lines[0]->getCoordinatesRO();
    ASSERT_EQ(seq->size(), 3u);
    EXPECT_DOUBLE_EQ(seq->getOrdinate(0, CoordinateSequence::Z), 10.0);
    EXPECT_DOUBLE_EQ(seq->getOrdinate(1, CoordinateSequence::Z), 20.0);
    EXPECT_DOUBLE_EQ(seq->getOrdinate(2, CoordinateSequence::Z), 30.0);
  }

  // Multi-vertex 3D XYM edge.
  {
    OverlayGraph graph;
    auto cs = std::make_unique<CoordinateSequence>(
        std::initializer_list<CoordinateXYM>{
            {0.0, 0.0, 1.5}, {1.0, 1.0, 2.5}, {2.0, 0.0, 3.5}});
    EdgeSourceInfo info(0);
    Edge edge(std::move(cs), &info);
    OverlayEdge* oe = graph.addEdge(&edge);
    oe->getLabel()->initLine(1);

    LineBuilder builder(&input, &graph, false, OverlayNG::INTERSECTION,
                        factory.get());
    std::vector<std::unique_ptr<LineString>> lines = builder.getLines();
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_FALSE(lines[0]->hasZ());
    EXPECT_TRUE(lines[0]->hasM());
    const CoordinateSequence* seq = lines[0]->getCoordinatesRO();
    ASSERT_EQ(seq->size(), 3u);
    EXPECT_TRUE(std::isnan(seq->getOrdinate(0, CoordinateSequence::Z)));
    EXPECT_DOUBLE_EQ(seq->getOrdinate(0, CoordinateSequence::M), 1.5);
    EXPECT_DOUBLE_EQ(seq->getOrdinate(1, CoordinateSequence::M), 2.5);
    EXPECT_DOUBLE_EQ(seq->getOrdinate(2, CoordinateSequence::M), 3.5);
  }

  // Multi-vertex 4D XYZM edge.
  {
    OverlayGraph graph;
    auto cs = std::make_unique<CoordinateSequence>(
        std::initializer_list<CoordinateXYZM>{{0.0, 0.0, 10.0, 1.0},
                                              {1.0, 1.0, 20.0, 2.0},
                                              {2.0, 0.0, 30.0, 3.0}});
    EdgeSourceInfo info(0);
    Edge edge(std::move(cs), &info);
    OverlayEdge* oe = graph.addEdge(&edge);
    oe->getLabel()->initLine(1);

    LineBuilder builder(&input, &graph, false, OverlayNG::INTERSECTION,
                        factory.get());
    std::vector<std::unique_ptr<LineString>> lines = builder.getLines();
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_TRUE(lines[0]->hasZ());
    EXPECT_TRUE(lines[0]->hasM());
    const CoordinateSequence* seq = lines[0]->getCoordinatesRO();
    ASSERT_EQ(seq->size(), 3u);
    EXPECT_DOUBLE_EQ(seq->getOrdinate(0, CoordinateSequence::Z), 10.0);
    EXPECT_DOUBLE_EQ(seq->getOrdinate(0, CoordinateSequence::M), 1.0);
    EXPECT_DOUBLE_EQ(seq->getOrdinate(2, CoordinateSequence::Z), 30.0);
    EXPECT_DOUBLE_EQ(seq->getOrdinate(2, CoordinateSequence::M), 3.0);
  }
}

TEST(LineBuilderTest, OverlayNGIntegrationLineAndPolygonOperations) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // Adjacent squares sharing an edge produce a 1D LineString intersection.
  Geometry::Ptr poly_left = reader.read("POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  Geometry::Ptr poly_right = reader.read("POLYGON ((1 0, 2 0, 2 1, 1 1, 1 0))");
  {
    OverlayNG ov(poly_left.get(), poly_right.get(), OverlayNG::INTERSECTION);
    Geometry::Ptr res = ov.getResult();
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
    EXPECT_DOUBLE_EQ(res->getLength(), 1.0);
  }

  // Polygon overlapping with one component and sharing an edge with another:
  // non-strict yields Polygon + LineString; strict omits the lower-dim line.
  Geometry::Ptr base_poly = reader.read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  Geometry::Ptr multi_poly = reader.read(
      "MULTIPOLYGON (((1 1, 3 1, 3 3, 1 3, 1 1)), "
      "((0 0, 1 0, 1 -1, 0 -1, 0 0)))");
  {
    OverlayNG ov(base_poly.get(), multi_poly.get(), OverlayNG::INTERSECTION);
    ov.setStrictMode(false);
    Geometry::Ptr res = ov.getResult();
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
    EXPECT_EQ(res->getNumGeometries(), 2u);
  }
  {
    OverlayNG ov(base_poly.get(), multi_poly.get(), OverlayNG::INTERSECTION);
    ov.setStrictMode(true);
    Geometry::Ptr res = ov.getResult();
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_DOUBLE_EQ(res->getArea(), 1.0);
  }

  // Line crossing polygon: UNION omits the interior segment and keeps exterior
  // segments; INTERSECTION keeps only the interior segment.
  Geometry::Ptr crossing_line = reader.read("LINESTRING (-1 1, 3 1)");
  {
    OverlayNG ov(base_poly.get(), crossing_line.get(), OverlayNG::UNION);
    Geometry::Ptr res = ov.getResult();
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
    EXPECT_DOUBLE_EQ(res->getArea(), 4.0);
    EXPECT_DOUBLE_EQ(res->getLength(), 10.0);  // 8.0 perimeter + 2.0 outside.
  }
  {
    OverlayNG ov(base_poly.get(), crossing_line.get(), OverlayNG::INTERSECTION);
    Geometry::Ptr res = ov.getResult();
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
    EXPECT_DOUBLE_EQ(res->getLength(), 2.0);
  }
}

TEST(LineBuilderTest, CollapseOnBothInputsRespectsStrictMode) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  Geometry::Ptr empty_a = factory->createPolygon();
  Geometry::Ptr empty_b = factory->createPolygon();
  InputGeometry input(empty_a.get(), empty_b.get());

  auto populate_graph = [](OverlayGraph& graph) {
    // Collapse with EXTERIOR line location on both A and B.
    OverlayEdge* both_ext =
        AddBoundaryEdge(graph, {0.0, 0.0}, {1.0, 0.0}, 0, 0, false);
    both_ext->getLabel()->setLocationLine(0, Location::EXTERIOR);
    both_ext->getLabel()->initCollapse(1, false);
    both_ext->getLabel()->setLocationLine(1, Location::EXTERIOR);

    // Collapse on A (EXTERIOR) and Collapse on B with INTERIOR location ->
    // isInteriorCollapse() is true, so always excluded.
    OverlayEdge* one_int =
        AddBoundaryEdge(graph, {2.0, 0.0}, {3.0, 0.0}, 0, 0, false);
    one_int->getLabel()->setLocationLine(0, Location::EXTERIOR);
    one_int->getLabel()->initCollapse(1, true);
    one_int->getLabel()->setLocationLine(1, Location::INTERIOR);
  };

  // Strict mode excludes boundary collapses even when both inputs collapsed.
  {
    OverlayGraph graph;
    populate_graph(graph);
    LineBuilder builder(&input, &graph, /*p_hasResultArea=*/false,
                        OverlayNG::UNION, factory.get());
    builder.setStrictMode(true);
    EXPECT_TRUE(builder.getLines().empty());
  }

  // Non-strict mode includes exterior collapse-collapse edges for INTERSECTION
  // and UNION, and excludes them for DIFFERENCE and SYMDIFFERENCE.
  for (int op : {OverlayNG::INTERSECTION, OverlayNG::UNION}) {
    OverlayGraph graph;
    populate_graph(graph);
    LineBuilder builder(&input, &graph, /*p_hasResultArea=*/false, op,
                        factory.get());
    builder.setStrictMode(false);
    EXPECT_EQ(SortedStartXs(builder.getLines()), std::vector<double>({0.0}));
  }
  for (int op : {OverlayNG::DIFFERENCE, OverlayNG::SYMDIFFERENCE}) {
    OverlayGraph graph;
    populate_graph(graph);
    LineBuilder builder(&input, &graph, /*p_hasResultArea=*/false, op,
                        factory.get());
    builder.setStrictMode(false);
    EXPECT_TRUE(builder.getLines().empty());
  }
}

TEST(LineBuilderTest, BoundaryCoincidentWithLineAcrossOperations) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  Geometry::Ptr empty_poly = factory->createPolygon();
  Geometry::Ptr empty_line = factory->createLineString();
  InputGeometry input(empty_poly.get(), empty_line.get());

  auto populate_graph = [](OverlayGraph& graph) {
    OverlayEdge* coincident =
        AddBoundaryEdge(graph, {0.0, 0.0}, {1.0, 0.0}, 0, 1, false);
    coincident->getLabel()->initLine(1);
  };

  // Boundary + Line is not a boundary collapse; strict mode retains it for
  // INTERSECTION and UNION when no result area is present.
  for (int op : {OverlayNG::INTERSECTION, OverlayNG::UNION}) {
    OverlayGraph graph;
    populate_graph(graph);
    LineBuilder builder(&input, &graph, /*p_hasResultArea=*/false, op,
                        factory.get());
    builder.setStrictMode(true);
    EXPECT_EQ(SortedStartXs(builder.getLines()), std::vector<double>({0.0}));
  }
  for (int op : {OverlayNG::DIFFERENCE, OverlayNG::SYMDIFFERENCE}) {
    OverlayGraph graph;
    populate_graph(graph);
    LineBuilder builder(&input, &graph, /*p_hasResultArea=*/false, op,
                        factory.get());
    builder.setStrictMode(true);
    EXPECT_TRUE(builder.getLines().empty());
  }

  // With hasResultArea=true, Boundary(0) has aLocLine=INTERIOR, so UNION omits
  // the line edge while INTERSECTION keeps it.
  {
    OverlayGraph graph;
    populate_graph(graph);
    LineBuilder builder(&input, &graph, /*p_hasResultArea=*/true,
                        OverlayNG::UNION, factory.get());
    EXPECT_TRUE(builder.getLines().empty());
  }
  {
    OverlayGraph graph;
    populate_graph(graph);
    LineBuilder builder(&input, &graph, /*p_hasResultArea=*/true,
                        OverlayNG::INTERSECTION, factory.get());
    builder.setStrictMode(true);
    EXPECT_EQ(SortedStartXs(builder.getLines()), std::vector<double>({0.0}));
  }

  // OverlayNG integration: LineString coincident with Polygon boundary.
  WKTReader reader(factory.get());
  Geometry::Ptr poly = reader.read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  Geometry::Ptr boundary_line = reader.read("LINESTRING (0 0, 2 0)");
  {
    OverlayNG ov(poly.get(), boundary_line.get(), OverlayNG::INTERSECTION);
    ov.setStrictMode(true);
    Geometry::Ptr res = ov.getResult();
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
    EXPECT_DOUBLE_EQ(res->getLength(), 2.0);
  }
  {
    OverlayNG ov(boundary_line.get(), poly.get(), OverlayNG::DIFFERENCE);
    Geometry::Ptr res = ov.getResult();
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
    EXPECT_TRUE(res->isEmpty());
  }
}

TEST(LineBuilderTest, SingleInputGeometryRepeatedGetLinesAndStrictToggle) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  Geometry::Ptr line_a = factory->createLineString();
  InputGeometry input(line_a.get(), nullptr);

  {
    OverlayGraph graph;
    AddLineEdge(graph, {1.0, 2.0}, {3.0, 2.0}, 0);
    LineBuilder builder(&input, &graph, /*p_hasResultArea=*/false,
                        OverlayNG::UNION, factory.get());

    std::vector<std::unique_ptr<LineString>> first = builder.getLines();
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first[0]->getCoordinateN(0), CoordinateXY(1.0, 2.0));
    EXPECT_EQ(first[0]->getCoordinateN(1), CoordinateXY(3.0, 2.0));

    // A second call sees all edges already marked and visited.
    std::vector<std::unique_ptr<LineString>> second = builder.getLines();
    EXPECT_TRUE(second.empty());
  }

  // Toggling strict mode true -> false restores boundary collapse emission.
  {
    Geometry::Ptr poly_a = factory->createPolygon();
    InputGeometry single_area(poly_a.get(), nullptr);
    OverlayGraph graph;
    OverlayEdge* collapse =
        AddBoundaryEdge(graph, {0.0, 0.0}, {1.0, 0.0}, 0, 0, false);
    collapse->getLabel()->setLocationLine(0, Location::EXTERIOR);

    LineBuilder builder(&single_area, &graph, /*p_hasResultArea=*/false,
                        OverlayNG::UNION, factory.get());
    builder.setStrictMode(true);
    builder.setStrictMode(false);
    EXPECT_EQ(SortedStartXs(builder.getLines()), std::vector<double>({0.0}));
  }
}

TEST(LineBuilderTest, BoundarySingletonInteriorOtherLocationStillExcluded) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  Geometry::Ptr poly_a = factory->createPolygon();
  Geometry::Ptr poly_b = factory->createPolygon();
  InputGeometry input(poly_a.get(), poly_b.get());

  const int ops[] = {OverlayNG::INTERSECTION, OverlayNG::UNION,
                     OverlayNG::DIFFERENCE, OverlayNG::SYMDIFFERENCE};
  for (int op : ops) {
    for (bool strict : {false, true}) {
      OverlayGraph graph;
      OverlayEdge* e_a =
          AddBoundaryEdge(graph, {0.0, 0.0}, {1.0, 0.0}, 0, 1, false);
      e_a->getLabel()->setLocationLine(1, Location::INTERIOR);
      OverlayEdge* e_b =
          AddBoundaryEdge(graph, {2.0, 0.0}, {3.0, 0.0}, 1, 1, false);
      e_b->getLabel()->setLocationLine(0, Location::INTERIOR);

      LineBuilder builder(&input, &graph, /*p_hasResultArea=*/false, op,
                          factory.get());
      builder.setStrictMode(strict);
      EXPECT_TRUE(builder.getLines().empty());
    }
  }
}

TEST(LineBuilderTest, LineInAreaAndBoundaryTouchFastPathAndUnknownOpCode) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  Geometry::Ptr area_a = factory->createPolygon();
  Geometry::Ptr line_b = factory->createLineString();

  // Only Location::INTERIOR is filtered by isLineInArea when
  // hasResultArea=true.
  {
    InputGeometry input(area_a.get(), line_b.get());
    auto populate_graph = [](OverlayGraph& graph) {
      OverlayEdge* inside = AddLineEdge(graph, {0.0, 0.0}, {1.0, 0.0}, 1);
      inside->getLabel()->setLocationLine(0, Location::INTERIOR);

      OverlayEdge* on_boundary = AddLineEdge(graph, {2.0, 0.0}, {3.0, 0.0}, 1);
      on_boundary->getLabel()->setLocationLine(0, Location::BOUNDARY);

      OverlayEdge* unknown_loc = AddLineEdge(graph, {4.0, 0.0}, {5.0, 0.0}, 1);
      unknown_loc->getLabel()->setLocationLine(0, Location::NONE);
    };

    {
      OverlayGraph graph;
      populate_graph(graph);
      LineBuilder builder(&input, &graph, /*p_hasResultArea=*/true,
                          OverlayNG::UNION, factory.get());
      EXPECT_EQ(SortedStartXs(builder.getLines()),
                std::vector<double>({2.0, 4.0}));
    }
    {
      OverlayGraph graph;
      populate_graph(graph);
      LineBuilder builder(&input, &graph, /*p_hasResultArea=*/true,
                          OverlayNG::DIFFERENCE, factory.get());
      EXPECT_TRUE(builder.getLines().empty());
    }
    {
      OverlayGraph graph;
      populate_graph(graph);
      LineBuilder builder(&input, &graph, /*p_hasResultArea=*/true,
                          OverlayNG::SYMDIFFERENCE, factory.get());
      EXPECT_EQ(SortedStartXs(builder.getLines()), std::vector<double>({4.0}));
    }
    {
      OverlayGraph graph;
      populate_graph(graph);
      LineBuilder builder(&input, &graph, /*p_hasResultArea=*/true,
                          OverlayNG::INTERSECTION, factory.get());
      EXPECT_EQ(SortedStartXs(builder.getLines()),
                std::vector<double>({0.0, 2.0}));
    }
  }

  // Isolates the isAllowMixedResult && INTERSECTION && isBoundaryTouch branch
  // from boolean fallback by setting exterior line locations on the touch.
  {
    Geometry::Ptr poly_b = factory->createPolygon();
    InputGeometry input(area_a.get(), poly_b.get());
    auto populate_touch = [](OverlayGraph& graph) {
      OverlayEdge* touch =
          AddBoundaryEdge(graph, {0.0, 0.0}, {1.0, 0.0}, 0, 1, false);
      touch->getLabel()->initBoundary(1, Location::INTERIOR, Location::EXTERIOR,
                                      false);
      touch->getLabel()->setLocationLine(0, Location::EXTERIOR);
      touch->getLabel()->setLocationLine(1, Location::EXTERIOR);
    };

    {
      OverlayGraph graph;
      populate_touch(graph);
      LineBuilder builder(&input, &graph, /*p_hasResultArea=*/false,
                          OverlayNG::INTERSECTION, factory.get());
      builder.setStrictMode(false);
      EXPECT_EQ(SortedStartXs(builder.getLines()), std::vector<double>({0.0}));
    }
    {
      OverlayGraph graph;
      populate_touch(graph);
      LineBuilder builder(&input, &graph, /*p_hasResultArea=*/false,
                          OverlayNG::INTERSECTION, factory.get());
      builder.setStrictMode(true);
      EXPECT_TRUE(builder.getLines().empty());
    }
    {
      OverlayGraph graph;
      populate_touch(graph);
      LineBuilder builder(&input, &graph, /*p_hasResultArea=*/false,
                          OverlayNG::SYMDIFFERENCE, factory.get());
      builder.setStrictMode(false);
      EXPECT_TRUE(builder.getLines().empty());
    }
  }

  // Unknown opCode falls through OverlayNG::isResultOfOp and yields no lines.
  {
    InputGeometry input(area_a.get(), line_b.get());
    OverlayGraph graph;
    OverlayEdge* edge = AddLineEdge(graph, {0.0, 0.0}, {1.0, 0.0}, 1);
    edge->getLabel()->setLocationLine(0, Location::EXTERIOR);
    LineBuilder builder(&input, &graph, /*p_hasResultArea=*/false,
                        /*p_opCode=*/99, factory.get());
    EXPECT_TRUE(builder.getLines().empty());
  }
}

TEST(LineBuilderTest, OverlayNGIntegrationPrecisionCollapseStrictVsNonStrict) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  PrecisionModel fixed_pm(1.0);

  // Thin exterior peninsula collapses to a boundary spike when rounded to grid
  // 1.0: non-strict mode retains the collapsed spike alongside the polygon,
  // whereas strict mode omits it.
  Geometry::Ptr peninsula_poly =
      reader.read("POLYGON ((0 0, 10 0, 10 4, 5.2 4, 5 5, 4.8 4, 0 4, 0 0))");
  {
    OverlayNG ov(peninsula_poly.get(), &fixed_pm);
    ov.setStrictMode(false);
    Geometry::Ptr res = ov.getResult();
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
    EXPECT_EQ(res->getNumGeometries(), 2u);
    EXPECT_DOUBLE_EQ(res->getArea(), 40.0);
    EXPECT_DOUBLE_EQ(res->getLength(), 29.0);
  }
  {
    OverlayNG ov(peninsula_poly.get(), &fixed_pm);
    ov.setStrictMode(true);
    Geometry::Ptr res = ov.getResult();
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_DOUBLE_EQ(res->getArea(), 40.0);
    EXPECT_DOUBLE_EQ(res->getLength(), 28.0);
  }

  // Thin interior gore collapses inside its parent area and is always omitted
  // by isInteriorCollapse(), even in non-strict mode.
  Geometry::Ptr gore_poly =
      reader.read("POLYGON ((0 0, 10 0, 10 4, 5.2 4, 5 3, 4.8 4, 0 4, 0 0))");
  {
    OverlayNG ov(gore_poly.get(), &fixed_pm);
    ov.setStrictMode(false);
    Geometry::Ptr res = ov.getResult();
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_DOUBLE_EQ(res->getArea(), 40.0);
    EXPECT_DOUBLE_EQ(res->getLength(), 28.0);
  }
}

template <typename Tag, typename Tag::type M>
struct LineBuilderPrivateAccess {
  friend typename Tag::type get(Tag) { return M; }
};

struct AddResultLinesMergedTag {
  using type = void (LineBuilder::*)();
  friend type get(AddResultLinesMergedTag);
};

template struct LineBuilderPrivateAccess<AddResultLinesMergedTag,
                                         &LineBuilder::addResultLinesMerged>;

TEST(LineBuilderTest, AddResultLinesMergedCoversChainsReverseAndRings) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  Geometry::Ptr line_a = reader.read("LINESTRING (0 0, 1 0)");
  InputGeometry input_geom(line_a.get(), nullptr);

  OverlayGraph graph;
  // Non-result edge incident to an interior degree-2 vertex (1, 0) exercises
  // skip paths in addResultLinesForNodes, addResultLinesRings, degreeOfLines,
  // and nextLineEdgeUnvisited.
  OverlayEdge* non_result = AddLineEdge(graph, {1, 0}, {1, 1}, 0);
  EXPECT_FALSE(non_result->isInResultLine());

  // Forward chain: (0,0) -> (1,0) -> (2,0), starting at forward node (0,0).
  OverlayEdge* fwd_a = AddLineEdge(graph, {0, 0}, {1, 0}, 0);
  OverlayEdge* fwd_b = AddLineEdge(graph, {1, 0}, {2, 0}, 0);
  fwd_a->markInResultLine();
  fwd_b->markInResultLine();

  // Reverse-start chain: (11,0) is inserted first with degree 2 so the loop in
  // addResultLinesForNodes skips it and starts at (10,0)->(11,0) where
  // isForward() == false, exercising coordinate reversal in buildLine().
  OverlayEdge* rev_a = AddLineEdge(graph, {11, 0}, {10, 0}, 0);
  OverlayEdge* rev_b = AddLineEdge(graph, {11, 0}, {12, 0}, 0);
  rev_a->markInResultLine();
  rev_b->markInResultLine();

  // Degree-3 junction at (20,0).
  OverlayEdge* branch_a = AddLineEdge(graph, {20, 0}, {21, 0}, 0);
  OverlayEdge* branch_b = AddLineEdge(graph, {20, 0}, {20, 1}, 0);
  OverlayEdge* branch_c = AddLineEdge(graph, {20, 0}, {19, 0}, 0);
  branch_a->markInResultLine();
  branch_b->markInResultLine();
  branch_c->markInResultLine();

  // Closed triangle of degree-2 result edges extracted by addResultLinesRings.
  OverlayEdge* ring_1 = AddLineEdge(graph, {30, 0}, {31, 0}, 0);
  OverlayEdge* ring_2 = AddLineEdge(graph, {31, 0}, {30.5, 1}, 0);
  OverlayEdge* ring_3 = AddLineEdge(graph, {30.5, 1}, {30, 0}, 0);
  ring_1->markInResultLine();
  ring_2->markInResultLine();
  ring_3->markInResultLine();

  LineBuilder builder(&input_geom, &graph, /*p_hasResultArea=*/false,
                      OverlayNG::UNION, factory.get());
  (builder.*get(AddResultLinesMergedTag{}))();

  EXPECT_FALSE(non_result->isVisited());
  for (OverlayEdge* e : {fwd_a, fwd_b, rev_a, rev_b, branch_a, branch_b,
                         branch_c, ring_1, ring_2, ring_3}) {
    EXPECT_TRUE(e->isVisited());
  }
}

}  // namespace
}  // namespace geos_google
