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

// Unit tests for third_party/geos/src/geomgraph/EdgeNodingValidator.cpp.

#include "third_party/geos/include/geos/geomgraph/EdgeNodingValidator.h"

#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Location;
using geos::geomgraph::Edge;
using geos::geomgraph::EdgeNodingValidator;
using geos::geomgraph::Label;
using geos::util::TopologyException;

std::unique_ptr<Edge> MakeEdge(std::initializer_list<Coordinate> coords) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  return std::make_unique<Edge>(seq.release(), Label(Location::BOUNDARY));
}

TEST(EdgeNodingValidatorTest, EmptyEdgeCollectionIsValid) {
  std::vector<Edge*> edges;
  EXPECT_NO_THROW(EdgeNodingValidator::checkValid(edges));

  EdgeNodingValidator validator(edges);
  EXPECT_NO_THROW(validator.checkValid());
}

TEST(EdgeNodingValidatorTest, SingleSimpleAndDisjointEdgesAreValid) {
  auto e0 = MakeEdge({{0.0, 0.0}, {1.0, 2.0}, {3.0, 2.0}});
  auto e1 = MakeEdge({{10.0, 0.0}, {12.0, 3.0}});
  std::vector<Edge*> single_edge = {e0.get()};
  std::vector<Edge*> disjoint_edges = {e0.get(), e1.get()};

  EXPECT_NO_THROW(EdgeNodingValidator::checkValid(single_edge));
  EXPECT_NO_THROW(EdgeNodingValidator::checkValid(disjoint_edges));

  EdgeNodingValidator validator(disjoint_edges);
  EXPECT_NO_THROW(validator.checkValid());
}

TEST(EdgeNodingValidatorTest, ProperlyNodedEdgesMeetingAtEndpointsAreValid) {
  auto e0 = MakeEdge({{0.0, 0.0}, {2.0, 0.0}});
  auto e1 = MakeEdge({{2.0, 0.0}, {4.0, 0.0}});
  auto e2 = MakeEdge({{2.0, 0.0}, {2.0, 3.0}});
  auto e3 = MakeEdge({{0.0, 0.0}, {1.0, 2.0}, {2.0, 3.0}});
  std::vector<Edge*> edges = {e0.get(), e1.get(), e2.get(), e3.get()};

  EXPECT_NO_THROW(EdgeNodingValidator::checkValid(edges));

  EdgeNodingValidator validator(edges);
  EXPECT_NO_THROW(validator.checkValid());
}

TEST(EdgeNodingValidatorTest, CrossingSegmentsThrowTopologyException) {
  auto e0 = MakeEdge({{0.0, 0.0}, {4.0, 4.0}});
  auto e1 = MakeEdge({{0.0, 4.0}, {4.0, 0.0}});
  std::vector<Edge*> edges = {e0.get(), e1.get()};

  try {
    EdgeNodingValidator::checkValid(edges);
    FAIL() << "Expected TopologyException for crossing edges";
  } catch (TopologyException& ex) {
    EXPECT_TRUE(ex.getCoordinate().equals2D(Coordinate(2.0, 2.0)));
    EXPECT_NE(std::string(ex.what()).find("non-noded intersection"),
              std::string::npos);
  }

  EdgeNodingValidator validator(edges);
  EXPECT_THROW(validator.checkValid(), TopologyException);
  EXPECT_THROW(validator.checkValid(), TopologyException);
}

TEST(EdgeNodingValidatorTest,
     TJunctionInSegmentInteriorThrowsTopologyException) {
  auto e0 = MakeEdge({{0.0, 0.0}, {6.0, 0.0}});
  auto e1 = MakeEdge({{3.0, 0.0}, {3.0, 4.0}});
  std::vector<Edge*> edges = {e0.get(), e1.get()};

  try {
    EdgeNodingValidator::checkValid(edges);
    FAIL() << "Expected TopologyException for un-noded T-junction";
  } catch (TopologyException& ex) {
    EXPECT_TRUE(ex.getCoordinate().equals2D(Coordinate(3.0, 0.0)));
  }
}

TEST(EdgeNodingValidatorTest, EndpointAtInteriorVertexThrowsTopologyException) {
  auto e0 = MakeEdge({{0.0, 0.0}, {2.0, 0.0}, {4.0, 0.0}});
  auto e1 = MakeEdge({{2.0, 0.0}, {2.0, 3.0}});
  std::vector<Edge*> edges = {e0.get(), e1.get()};

  try {
    EdgeNodingValidator::checkValid(edges);
    FAIL() << "Expected TopologyException for interior vertex intersection";
  } catch (TopologyException& ex) {
    EXPECT_TRUE(ex.getCoordinate().equals2D(Coordinate(2.0, 0.0)));
  }
}

TEST(EdgeNodingValidatorTest, SelfIntersectingEdgeThrowsTopologyException) {
  auto e0 = MakeEdge({{0.0, 0.0}, {4.0, 4.0}, {4.0, 0.0}, {0.0, 4.0}});
  std::vector<Edge*> edges = {e0.get()};

  try {
    EdgeNodingValidator::checkValid(edges);
    FAIL() << "Expected TopologyException for self-intersecting edge";
  } catch (TopologyException& ex) {
    EXPECT_TRUE(ex.getCoordinate().equals2D(Coordinate(2.0, 2.0)));
  }
}

}  // namespace
}  // namespace geos_google
