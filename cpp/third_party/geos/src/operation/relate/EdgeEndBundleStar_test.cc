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

// Unit tests for third_party/geos/src/operation/relate/EdgeEndBundleStar.cpp
// and third_party/geos/include/geos/operation/relate/EdgeEndBundleStar.h.

#include "third_party/geos/include/geos/operation/relate/EdgeEndBundleStar.h"

#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/BoundaryNodeRule.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/IntersectionMatrix.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/EdgeEnd.h"
#include "third_party/geos/include/geos/geomgraph/EdgeEndStar.h"
#include "third_party/geos/include/geos/geomgraph/GeometryGraph.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/relate/EdgeEndBundle.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using geos::algorithm::BoundaryNodeRule;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::IntersectionMatrix;
using geos::geom::Location;
using geos::geomgraph::Edge;
using geos::geomgraph::EdgeEnd;
using geos::geomgraph::EdgeEndStar;
using geos::geomgraph::GeometryGraph;
using geos::geomgraph::Label;
using geos::io::WKTReader;
using geos::operation::relate::EdgeEndBundle;
using geos::operation::relate::EdgeEndBundleStar;
using geos::util::TopologyException;

class CountingEdgeEnd final : public EdgeEnd {
 public:
  CountingEdgeEnd(Edge* edge, const Coordinate& p0, const Coordinate& p1,
                  const Label& label, int* destroyed_count)
      : EdgeEnd(edge, p0, p1, label), destroyed_count_(destroyed_count) {}

  ~CountingEdgeEnd() override { ++(*destroyed_count_); }

 private:
  int* destroyed_count_;
};

std::unique_ptr<Edge> MakeSegmentEdge(const Coordinate& p0,
                                      const Coordinate& p1,
                                      const Label& label) {
  return std::make_unique<Edge>(new CoordinateSequence{p0, p1}, label);
}

TEST(EdgeEndBundleStarTest, EmptyStarPropertiesAndUpdateIM) {
  EdgeEndBundleStar star;
  EXPECT_EQ(star.getDegree(), 0u);
  EXPECT_EQ(star.begin(), star.end());
  EXPECT_TRUE(star.getCoordinate().isNull());

  IntersectionMatrix im;
  star.updateIM(im);
  EXPECT_EQ(im.toString(), "FFFFFFFFF");
}

TEST(EdgeEndBundleStarTest,
     InsertBundlesParallelRaysAndOrdersBundlesCounterClockwise) {
  int destroyed = 0;
  auto edge = MakeSegmentEdge(Coordinate(1, 2), Coordinate(4, 2), Label());
  const Coordinate origin(1, 2);

  {
    EdgeEndBundleStar star;
    // Insert out of angular order across all four quadrants, including two
    // duplicate-direction rays with different segment lengths.
    star.insert(new CountingEdgeEnd(edge.get(), origin, Coordinate(2, 1),
                                    Label(0, Location::INTERIOR), &destroyed));
    star.insert(new CountingEdgeEnd(edge.get(), origin, Coordinate(0, 3),
                                    Label(0, Location::INTERIOR), &destroyed));
    star.insert(new CountingEdgeEnd(edge.get(), origin, Coordinate(4, 2),
                                    Label(0, Location::INTERIOR), &destroyed));
    star.insert(new CountingEdgeEnd(edge.get(), origin, Coordinate(-1, 0),
                                    Label(0, Location::INTERIOR), &destroyed));
    star.insert(new CountingEdgeEnd(edge.get(), origin, Coordinate(3, 4),
                                    Label(0, Location::INTERIOR), &destroyed));
    // Parallel to (3, 4) from (1, 2): dx=2, dy=2 vs dx=4, dy=4.
    star.insert(new CountingEdgeEnd(edge.get(), origin, Coordinate(5, 6),
                                    Label(1, Location::BOUNDARY), &destroyed));
    // Parallel to (4, 2) from (1, 2): dx=3, dy=0 vs dx=1, dy=0.
    star.insert(new CountingEdgeEnd(edge.get(), origin, Coordinate(2, 2),
                                    Label(1, Location::INTERIOR), &destroyed));

    EXPECT_EQ(star.getDegree(), 5u);
    EXPECT_TRUE(star.getCoordinate().equals2D(origin));

    std::vector<EdgeEndBundle*> bundles;
    for (EdgeEnd* ee : star) {
      bundles.push_back(static_cast<EdgeEndBundle*>(ee));
    }
    ASSERT_EQ(bundles.size(), 5u);

    // CCW order starting from +x axis around (1, 2).
    EXPECT_TRUE(bundles[0]->getDirectedCoordinate().equals2D(Coordinate(4, 2)));
    EXPECT_EQ(bundles[0]->getEdgeEnds().size(), 2u);

    EXPECT_TRUE(bundles[1]->getDirectedCoordinate().equals2D(Coordinate(3, 4)));
    EXPECT_EQ(bundles[1]->getEdgeEnds().size(), 2u);

    EXPECT_TRUE(bundles[2]->getDirectedCoordinate().equals2D(Coordinate(0, 3)));
    EXPECT_EQ(bundles[2]->getEdgeEnds().size(), 1u);

    EXPECT_TRUE(
        bundles[3]->getDirectedCoordinate().equals2D(Coordinate(-1, 0)));
    EXPECT_EQ(bundles[3]->getEdgeEnds().size(), 1u);

    EXPECT_TRUE(bundles[4]->getDirectedCoordinate().equals2D(Coordinate(2, 1)));
    EXPECT_EQ(bundles[4]->getEdgeEnds().size(), 1u);

    EXPECT_EQ(star.getNextCW(bundles[0]), bundles[4]);
    EXPECT_EQ(star.getNextCW(bundles[1]), bundles[0]);
    EXPECT_EQ(destroyed, 0);
  }
  EXPECT_EQ(destroyed, 7);
}

TEST(EdgeEndBundleStarTest, VirtualInsertAndDestructorThroughBasePointer) {
  int destroyed = 0;
  auto edge = MakeSegmentEdge(Coordinate(0, 0), Coordinate(5, 0), Label());

  std::unique_ptr<EdgeEndStar> star = std::make_unique<EdgeEndBundleStar>();
  star->insert(new CountingEdgeEnd(edge.get(), Coordinate(0, 0),
                                   Coordinate(5, 0),
                                   Label(0, Location::INTERIOR), &destroyed));
  star->insert(new CountingEdgeEnd(edge.get(), Coordinate(0, 0),
                                   Coordinate(10, 0),
                                   Label(1, Location::INTERIOR), &destroyed));
  star->insert(new CountingEdgeEnd(edge.get(), Coordinate(0, 0),
                                   Coordinate(0, 5),
                                   Label(0, Location::BOUNDARY), &destroyed));

  EXPECT_EQ(star->getDegree(), 2u);
  EXPECT_EQ(destroyed, 0);
  star.reset();
  EXPECT_EQ(destroyed, 3);
}

TEST(EdgeEndBundleStarTest, UpdateIMAccumulatesContributionsFromAllBundles) {
  auto edge = MakeSegmentEdge(Coordinate(0, 0), Coordinate(5, 0), Label());
  EdgeEndBundleStar star;

  // Bundle 0 (East): area edges from both geometries.
  star.insert(new EdgeEnd(
      edge.get(), Coordinate(0, 0), Coordinate(5, 0),
      Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR)));
  star.insert(new EdgeEnd(
      edge.get(), Coordinate(0, 0), Coordinate(8, 0),
      Label(1, Location::BOUNDARY, Location::EXTERIOR, Location::INTERIOR)));

  // Bundle 1 (North): line edges from both geometries.
  star.insert(new EdgeEnd(edge.get(), Coordinate(0, 0), Coordinate(0, 5),
                          Label(0, Location::INTERIOR)));
  star.insert(new EdgeEnd(edge.get(), Coordinate(0, 0), Coordinate(0, 8),
                          Label(1, Location::BOUNDARY)));

  for (EdgeEnd* ee : star) {
    static_cast<EdgeEndBundle*>(ee)->computeLabel(
        BoundaryNodeRule::getBoundaryRuleMod2());
  }

  IntersectionMatrix im;
  star.updateIM(im);
  EXPECT_EQ(im.toString(), "F12F1F2FF");
}

TEST(EdgeEndBundleStarTest,
     ComputeLabellingAndUpdateIMWithPolygonAndLineGraphs) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  std::unique_ptr<Geometry> geom_a =
      reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  std::unique_ptr<Geometry> geom_b = reader.read("LINESTRING (-5 0, 0 0, 5 5)");

  std::vector<std::unique_ptr<GeometryGraph>> graphs;
  graphs.push_back(std::make_unique<GeometryGraph>(0, geom_a.get()));
  graphs.push_back(std::make_unique<GeometryGraph>(1, geom_b.get()));

  auto edge = MakeSegmentEdge(Coordinate(0, 0), Coordinate(10, 0), Label());
  EdgeEndBundleStar star;
  // Polygon boundary rays meeting at (0, 0).
  star.insert(new EdgeEnd(
      edge.get(), Coordinate(0, 0), Coordinate(10, 0),
      Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR)));
  star.insert(new EdgeEnd(
      edge.get(), Coordinate(0, 0), Coordinate(0, 10),
      Label(0, Location::BOUNDARY, Location::EXTERIOR, Location::INTERIOR)));
  // LineString rays leaving (0, 0) into polygon interior and exterior.
  star.insert(new EdgeEnd(edge.get(), Coordinate(0, 0), Coordinate(5, 5),
                          Label(1, Location::INTERIOR)));
  star.insert(new EdgeEnd(edge.get(), Coordinate(0, 0), Coordinate(-5, 0),
                          Label(1, Location::INTERIOR)));

  star.computeLabelling(graphs);

  IntersectionMatrix im;
  star.updateIM(im);
  EXPECT_EQ(im.toString(), "1F2FF11F2");
}

TEST(EdgeEndBundleStarTest, ComputeLabellingAndUpdateIMWithOverlappingSquares) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  std::unique_ptr<Geometry> geom_a =
      reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  std::unique_ptr<Geometry> geom_b =
      reader.read("POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0))");

  std::vector<std::unique_ptr<GeometryGraph>> graphs;
  graphs.push_back(std::make_unique<GeometryGraph>(0, geom_a.get()));
  graphs.push_back(std::make_unique<GeometryGraph>(1, geom_b.get()));

  auto edge = MakeSegmentEdge(Coordinate(0, 0), Coordinate(10, 0), Label());
  EdgeEndBundleStar star;
  star.insert(new EdgeEnd(
      edge.get(), Coordinate(0, 0), Coordinate(10, 0),
      Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR)));
  star.insert(new EdgeEnd(
      edge.get(), Coordinate(0, 0), Coordinate(5, 0),
      Label(1, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR)));
  star.insert(new EdgeEnd(
      edge.get(), Coordinate(0, 0), Coordinate(0, 10),
      Label(0, Location::BOUNDARY, Location::EXTERIOR, Location::INTERIOR)));
  star.insert(new EdgeEnd(
      edge.get(), Coordinate(0, 0), Coordinate(0, 5),
      Label(1, Location::BOUNDARY, Location::EXTERIOR, Location::INTERIOR)));

  EXPECT_EQ(star.getDegree(), 2u);
  star.computeLabelling(graphs);

  IntersectionMatrix im;
  star.updateIM(im);
  EXPECT_EQ(im.toString(), "2FFF1FFF2");
}

TEST(EdgeEndBundleStarTest,
     ComputeLabellingThrowsTopologyExceptionOnSideConflictAndCleansUp) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  std::unique_ptr<Geometry> geom_a =
      reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  std::unique_ptr<Geometry> geom_b =
      reader.read("POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0))");

  std::vector<std::unique_ptr<GeometryGraph>> graphs;
  graphs.push_back(std::make_unique<GeometryGraph>(0, geom_a.get()));
  graphs.push_back(std::make_unique<GeometryGraph>(1, geom_b.get()));

  int destroyed = 0;
  auto edge = MakeSegmentEdge(Coordinate(0, 0), Coordinate(10, 0), Label());
  {
    EdgeEndBundleStar star;
    // Conflicting side labels: East has LEFT=INTERIOR, North has
    // RIGHT=EXTERIOR (should match East's LEFT).
    star.insert(new CountingEdgeEnd(
        edge.get(), Coordinate(0, 0), Coordinate(10, 0),
        Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR),
        &destroyed));
    star.insert(new CountingEdgeEnd(
        edge.get(), Coordinate(0, 0), Coordinate(0, 10),
        Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR),
        &destroyed));

    EXPECT_THROW(star.computeLabelling(graphs), TopologyException);
  }
  EXPECT_EQ(destroyed, 2);
}

}  // namespace
}  // namespace geos_google
