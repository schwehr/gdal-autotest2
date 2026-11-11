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

// Unit tests for third_party/geos/src/operation/linemerge/LineMergeEdge.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/linemerge/LineMergeEdge.cpp

#include "third_party/geos/include/geos/operation/linemerge/LineMergeEdge.h"

#include <initializer_list>
#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/operation/linemerge/LineMergeDirectedEdge.h"
#include "third_party/geos/include/geos/planargraph/Edge.h"
#include "third_party/geos/include/geos/planargraph/Node.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::operation::linemerge::LineMergeDirectedEdge;
using geos::operation::linemerge::LineMergeEdge;
using geos::planargraph::Edge;
using geos::planargraph::Node;

TEST(LineMergeEdgeTest, ConstructsWithLineStringAndReturnsSamePointer) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LineString> line = factory->createLineString(
      CoordinateSequence(std::initializer_list<CoordinateXY>{
          {0.0, 0.0}, {3.0, 4.0}, {6.0, 0.0}}));

  LineMergeEdge edge(line.get());
  const LineMergeEdge& const_edge = edge;

  ASSERT_EQ(const_edge.getLine(), line.get());
  EXPECT_EQ(const_edge.getLine()->getNumPoints(), 3u);
  EXPECT_DOUBLE_EQ(const_edge.getLine()->getLength(), 10.0);
}

TEST(LineMergeEdgeTest, StoresNullLineStringWithoutDereferencing) {
  LineMergeEdge edge(nullptr);
  EXPECT_EQ(edge.getLine(), nullptr);
}

TEST(LineMergeEdgeTest, StoresEmptyLineString) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LineString> empty_line = factory->createLineString();

  LineMergeEdge edge(empty_line.get());
  ASSERT_EQ(edge.getLine(), empty_line.get());
  EXPECT_TRUE(edge.getLine()->isEmpty());
}

TEST(LineMergeEdgeTest, InheritsPlanarGraphEdgeLifecycleAndFlags) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LineString> line =
      factory->createLineString(CoordinateSequence(
          std::initializer_list<CoordinateXY>{{0.0, 0.0}, {2.0, 0.0}}));

  Node n0(Coordinate(0.0, 0.0));
  Node n1(Coordinate(2.0, 0.0));
  LineMergeDirectedEdge de_fwd(&n0, &n1, Coordinate(2.0, 0.0), true);
  LineMergeDirectedEdge de_rev(&n1, &n0, Coordinate(0.0, 0.0), false);

  std::unique_ptr<Edge> base_edge = std::make_unique<LineMergeEdge>(line.get());
  EXPECT_FALSE(base_edge->isMarked());
  EXPECT_FALSE(base_edge->isVisited());

  base_edge->setMarked(true);
  base_edge->setVisited(true);
  EXPECT_TRUE(base_edge->isMarked());
  EXPECT_TRUE(base_edge->isVisited());

  base_edge->setMarked(false);
  base_edge->setVisited(false);
  EXPECT_FALSE(base_edge->isMarked());
  EXPECT_FALSE(base_edge->isVisited());

  base_edge->setDirectedEdges(&de_fwd, &de_rev);
  EXPECT_EQ(base_edge->getDirEdge(0), &de_fwd);
  EXPECT_EQ(base_edge->getDirEdge(1), &de_rev);
  EXPECT_EQ(base_edge->getDirEdge(&n0), &de_fwd);
  EXPECT_EQ(base_edge->getOppositeNode(&n0), &n1);
  EXPECT_EQ(de_fwd.getEdge(), base_edge.get());

  auto* merged_edge = dynamic_cast<LineMergeEdge*>(base_edge.get());
  ASSERT_NE(merged_edge, nullptr);
  EXPECT_EQ(merged_edge->getLine(), line.get());
}

}  // namespace
}  // namespace geos_google
