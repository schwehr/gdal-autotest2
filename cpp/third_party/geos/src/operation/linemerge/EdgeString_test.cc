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

// Unit tests for third_party/geos/src/operation/linemerge/EdgeString.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/linemerge/EdgeString.cpp

#include "third_party/geos/include/geos/operation/linemerge/EdgeString.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/operation/linemerge/LineMergeDirectedEdge.h"
#include "third_party/geos/include/geos/operation/linemerge/LineMergeEdge.h"
#include "third_party/geos/include/geos/planargraph/Node.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::geom::PrecisionModel;
using geos::operation::linemerge::EdgeString;
using geos::operation::linemerge::LineMergeDirectedEdge;
using geos::operation::linemerge::LineMergeEdge;
using geos::planargraph::Node;

struct EdgePair {
  LineMergeDirectedEdge* forward;
  LineMergeDirectedEdge* reverse;
};

class GraphBuilder {
 public:
  explicit GraphBuilder(const GeometryFactory& factory) : factory_(factory) {}

  Node* AddNode(double x, double y) {
    nodes_.push_back(std::make_unique<Node>(Coordinate(x, y)));
    return nodes_.back().get();
  }

  EdgePair AddEdge2D(Node* from, Node* to,
                     std::initializer_list<CoordinateXY> coords) {
    return AddEdge(from, to, std::make_unique<CoordinateSequence>(coords));
  }

  EdgePair AddEdge3D(Node* from, Node* to,
                     std::initializer_list<Coordinate> coords) {
    return AddEdge(from, to, std::make_unique<CoordinateSequence>(coords));
  }

 private:
  EdgePair AddEdge(Node* from, Node* to,
                   std::unique_ptr<CoordinateSequence> seq_in) {
    lines_.push_back(factory_.createLineString(std::move(seq_in)));
    const LineString* line = lines_.back().get();
    const CoordinateSequence* seq = line->getCoordinatesRO();

    dir_edges_.push_back(std::make_unique<LineMergeDirectedEdge>(
        from, to, Coordinate(seq->getX(1), seq->getY(1)), true));
    LineMergeDirectedEdge* fwd = dir_edges_.back().get();

    const std::size_t prev_idx = seq->size() - 2;
    dir_edges_.push_back(std::make_unique<LineMergeDirectedEdge>(
        to, from, Coordinate(seq->getX(prev_idx), seq->getY(prev_idx)), false));
    LineMergeDirectedEdge* rev = dir_edges_.back().get();

    edges_.push_back(std::make_unique<LineMergeEdge>(line));
    edges_.back()->setDirectedEdges(fwd, rev);
    return {.forward = fwd, .reverse = rev};
  }

  const GeometryFactory& factory_;
  std::vector<std::unique_ptr<LineString>> lines_;
  std::vector<std::unique_ptr<Node>> nodes_;
  std::vector<std::unique_ptr<LineMergeDirectedEdge>> dir_edges_;
  std::vector<std::unique_ptr<LineMergeEdge>> edges_;
};

void ExpectCoords2DEq(const LineString& actual,
                      std::initializer_list<CoordinateXY> expected) {
  const CoordinateSequence* seq = actual.getCoordinatesRO();
  ASSERT_EQ(seq->size(), expected.size());
  std::size_t idx = 0;
  for (const CoordinateXY& exp : expected) {
    SCOPED_TRACE(idx);
    EXPECT_DOUBLE_EQ(seq->getX(idx), exp.x);
    EXPECT_DOUBLE_EQ(seq->getY(idx), exp.y);
    ++idx;
  }
}

void ExpectCoords3DEq(const LineString& actual,
                      std::initializer_list<Coordinate> expected) {
  const CoordinateSequence* seq = actual.getCoordinatesRO();
  ASSERT_EQ(seq->size(), expected.size());
  std::size_t idx = 0;
  for (const Coordinate& exp : expected) {
    SCOPED_TRACE(idx);
    EXPECT_DOUBLE_EQ(seq->getX(idx), exp.x);
    EXPECT_DOUBLE_EQ(seq->getY(idx), exp.y);
    EXPECT_DOUBLE_EQ(seq->getOrdinate(idx, CoordinateSequence::Z), exp.z);
    ++idx;
  }
}

TEST(EdgeStringTest, EmptyEdgeStringProducesEmptyLineString) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  EdgeString edge_string(factory.get());
  const EdgeString& const_edge_string = edge_string;

  std::unique_ptr<LineString> line = const_edge_string.toLineString();
  ASSERT_NE(line, nullptr);
  EXPECT_TRUE(line->isEmpty());
  EXPECT_EQ(line->getNumPoints(), 0u);
  EXPECT_EQ(line->getFactory(), factory.get());
}

TEST(EdgeStringTest, SingleForwardEdgePreservesCoordinates) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  GraphBuilder graph(*factory);

  Node* n0 = graph.AddNode(0.0, 0.0);
  Node* n1 = graph.AddNode(3.0, 4.0);
  EdgePair e = graph.AddEdge2D(n0, n1, {{0.0, 0.0}, {1.0, 2.0}, {3.0, 4.0}});

  EdgeString edge_string(factory.get());
  edge_string.add(e.forward);

  std::unique_ptr<LineString> line = edge_string.toLineString();
  ASSERT_NE(line, nullptr);
  EXPECT_FALSE(line->hasZ());
  EXPECT_TRUE(std::isnan(
      line->getCoordinatesRO()->getOrdinate(0, CoordinateSequence::Z)));
  ExpectCoords2DEq(*line, {{0.0, 0.0}, {1.0, 2.0}, {3.0, 4.0}});
}

TEST(EdgeStringTest, SingleReverseEdgeFlipsBackToMajorityDirection) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  GraphBuilder graph(*factory);

  Node* n0 = graph.AddNode(0.0, 0.0);
  Node* n1 = graph.AddNode(3.0, 4.0);
  EdgePair e = graph.AddEdge2D(n0, n1, {{0.0, 0.0}, {1.0, 2.0}, {3.0, 4.0}});

  EdgeString edge_string(factory.get());
  edge_string.add(e.reverse);

  std::unique_ptr<LineString> line = edge_string.toLineString();
  ASSERT_NE(line, nullptr);
  ExpectCoords2DEq(*line, {{0.0, 0.0}, {1.0, 2.0}, {3.0, 4.0}});
}

TEST(EdgeStringTest, MajorityForwardMergesChainAndDeduplicatesVertices) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  GraphBuilder graph(*factory);

  Node* n0 = graph.AddNode(0.0, 0.0);
  Node* n1 = graph.AddNode(2.0, 0.0);
  Node* n2 = graph.AddNode(3.0, 1.0);
  Node* n3 = graph.AddNode(5.0, 0.0);

  // Include consecutive duplicate intermediate point in e0 as well.
  EdgePair e0 =
      graph.AddEdge2D(n0, n1, {{0.0, 0.0}, {1.0, 1.0}, {1.0, 1.0}, {2.0, 0.0}});
  EdgePair e1 = graph.AddEdge2D(n2, n1, {{3.0, 1.0}, {2.0, 0.0}});
  EdgePair e2 = graph.AddEdge2D(n2, n3, {{3.0, 1.0}, {4.0, 0.0}, {5.0, 0.0}});

  // 2 forward, 1 reverse -> keeps traversal order (n0 -> n3).
  EdgeString edge_string(factory.get());
  edge_string.add(e0.forward);
  edge_string.add(e1.reverse);
  edge_string.add(e2.forward);

  std::unique_ptr<LineString> line = edge_string.toLineString();
  ASSERT_NE(line, nullptr);
  ExpectCoords2DEq(
      *line,
      {{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}, {3.0, 1.0}, {4.0, 0.0}, {5.0, 0.0}});
}

TEST(EdgeStringTest, MajorityReverseMergesChainAndReversesOutput) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  GraphBuilder graph(*factory);

  Node* n0 = graph.AddNode(0.0, 0.0);
  Node* n1 = graph.AddNode(2.0, 0.0);
  Node* n2 = graph.AddNode(3.0, 1.0);
  Node* n3 = graph.AddNode(5.0, 0.0);

  EdgePair e0 = graph.AddEdge2D(n1, n0, {{2.0, 0.0}, {1.0, 1.0}, {0.0, 0.0}});
  EdgePair e1 = graph.AddEdge2D(n1, n2, {{2.0, 0.0}, {3.0, 1.0}});
  EdgePair e2 = graph.AddEdge2D(n3, n2, {{5.0, 0.0}, {4.0, 0.0}, {3.0, 1.0}});

  // 1 forward, 2 reverse -> reverses final coordinates (n3 -> n0).
  EdgeString edge_string(factory.get());
  edge_string.add(e0.reverse);
  edge_string.add(e1.forward);
  edge_string.add(e2.reverse);

  std::unique_ptr<LineString> line = edge_string.toLineString();
  ASSERT_NE(line, nullptr);
  ExpectCoords2DEq(
      *line,
      {{5.0, 0.0}, {4.0, 0.0}, {3.0, 1.0}, {2.0, 0.0}, {1.0, 1.0}, {0.0, 0.0}});
}

TEST(EdgeStringTest, TieBetweenForwardAndReverseKeepsTraversalOrder) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  GraphBuilder graph(*factory);

  Node* n0 = graph.AddNode(0.0, 0.0);
  Node* n1 = graph.AddNode(1.0, 0.0);
  Node* n2 = graph.AddNode(2.0, 0.0);

  EdgePair e0 = graph.AddEdge2D(n0, n1, {{0.0, 0.0}, {1.0, 0.0}});
  EdgePair e1 = graph.AddEdge2D(n2, n1, {{2.0, 0.0}, {1.0, 0.0}});

  // Tie 1 forward, 1 reverse (forward first).
  {
    EdgeString edge_string(factory.get());
    edge_string.add(e0.forward);
    edge_string.add(e1.reverse);

    std::unique_ptr<LineString> line = edge_string.toLineString();
    ASSERT_NE(line, nullptr);
    ExpectCoords2DEq(*line, {{0.0, 0.0}, {1.0, 0.0}, {2.0, 0.0}});
  }

  // Tie 1 reverse, 1 forward (reverse edge added first).
  {
    GraphBuilder rev_first_graph(*factory);
    Node* a = rev_first_graph.AddNode(0.0, 0.0);
    Node* b = rev_first_graph.AddNode(1.0, 0.0);
    Node* c = rev_first_graph.AddNode(2.0, 0.0);
    EdgePair ab = rev_first_graph.AddEdge2D(b, a, {{1.0, 0.0}, {0.0, 0.0}});
    EdgePair bc = rev_first_graph.AddEdge2D(b, c, {{1.0, 0.0}, {2.0, 0.0}});

    EdgeString edge_string(factory.get());
    edge_string.add(ab.reverse);
    edge_string.add(bc.forward);

    std::unique_ptr<LineString> line = edge_string.toLineString();
    ASSERT_NE(line, nullptr);
    ExpectCoords2DEq(*line, {{0.0, 0.0}, {1.0, 0.0}, {2.0, 0.0}});
  }
}

TEST(EdgeStringTest, ClosedLoopEdgesFormClosedLineStringAndRepeatableOutput) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  GraphBuilder graph(*factory);

  Node* n0 = graph.AddNode(0.0, 0.0);
  Node* n1 = graph.AddNode(2.0, 2.0);
  EdgePair e0 = graph.AddEdge2D(n0, n1, {{0.0, 0.0}, {2.0, 0.0}, {2.0, 2.0}});
  EdgePair e1 = graph.AddEdge2D(n1, n0, {{2.0, 2.0}, {0.0, 2.0}, {0.0, 0.0}});

  EdgeString edge_string(factory.get());
  edge_string.add(e0.forward);
  edge_string.add(e1.forward);

  std::unique_ptr<LineString> first = edge_string.toLineString();
  std::unique_ptr<LineString> second = edge_string.toLineString();

  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_NE(first.get(), second.get());
  EXPECT_TRUE(first->isClosed());
  EXPECT_TRUE(first->equalsExact(second.get()));
  ExpectCoords2DEq(
      *first, {{0.0, 0.0}, {2.0, 0.0}, {2.0, 2.0}, {0.0, 2.0}, {0.0, 0.0}});
}

TEST(EdgeStringTest, PreservesFactorySridAndZCoordinatesAcrossReversal) {
  PrecisionModel pm(100.0);
  GeometryFactory::Ptr factory = GeometryFactory::create(&pm, 4326);
  GraphBuilder graph(*factory);

  Node* n0 = graph.AddNode(0.0, 0.0);
  Node* n1 = graph.AddNode(1.0, 0.0);
  Node* n2 = graph.AddNode(2.0, 0.0);

  EdgePair e0 = graph.AddEdge3D(n1, n0, {{1.0, 0.0, 20.0}, {0.0, 0.0, 10.0}});
  EdgePair e1 = graph.AddEdge3D(n2, n1, {{2.0, 0.0, 30.0}, {1.0, 0.0, 20.0}});

  // Both reverse (2 > 0): traversed n0 -> n2, then reversed to n2 -> n0.
  EdgeString edge_string(factory.get());
  edge_string.add(e0.reverse);
  edge_string.add(e1.reverse);

  std::unique_ptr<LineString> line = edge_string.toLineString();
  ASSERT_NE(line, nullptr);
  EXPECT_EQ(line->getFactory(), factory.get());
  EXPECT_EQ(line->getSRID(), 4326);
  EXPECT_TRUE(line->hasZ());
  ExpectCoords3DEq(*line,
                   {{2.0, 0.0, 30.0}, {1.0, 0.0, 20.0}, {0.0, 0.0, 10.0}});
}

}  // namespace
}  // namespace geos_google
