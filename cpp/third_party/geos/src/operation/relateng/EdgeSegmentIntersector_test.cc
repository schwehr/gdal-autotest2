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
// third_party/geos/src/operation/relateng/EdgeSegmentIntersector.cpp.

#include "third_party/geos/include/geos/operation/relateng/EdgeSegmentIntersector.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/IntersectionMatrix.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/noding/SegmentIntersector.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"
#include "third_party/geos/include/geos/operation/relateng/BasicPredicate.h"
#include "third_party/geos/include/geos/operation/relateng/EdgeSetIntersector.h"
#include "third_party/geos/include/geos/operation/relateng/IMPredicate.h"
#include "third_party/geos/include/geos/operation/relateng/RelateGeometry.h"
#include "third_party/geos/include/geos/operation/relateng/RelateMatrixPredicate.h"
#include "third_party/geos/include/geos/operation/relateng/RelatePredicate.h"
#include "third_party/geos/include/geos/operation/relateng/TopologyComputer.h"
#include "third_party/geos/include/geos/operation/relateng/TopologyPredicate.h"

namespace geos_google {
namespace {

using geos::geom::Dimension;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::IntersectionMatrix;
using geos::geom::Location;
using geos::io::WKTReader;
using geos::noding::SegmentIntersector;
using geos::noding::SegmentString;
using geos::operation::relateng::EdgeSegmentIntersector;
using geos::operation::relateng::EdgeSetIntersector;
using geos::operation::relateng::RelateGeometry;
using geos::operation::relateng::RelateMatrixPredicate;
using geos::operation::relateng::RelatePredicate;
using geos::operation::relateng::TopologyComputer;
using geos::operation::relateng::TopologyPredicate;

struct DimUpdate {
  Location loc_a;
  Location loc_b;
  int dim;

  bool operator==(const DimUpdate& other) const {
    return loc_a == other.loc_a && loc_b == other.loc_b && dim == other.dim;
  }
};

class RecordingTopologyPredicate : public TopologyPredicate {
 public:
  std::string name() const override { return "recording"; }

  void finish() override { finished_ = true; }

  bool isKnown() const override { return known_; }

  bool value() const override { return value_; }

  void updateDimension(geos::geom::Location locA, geos::geom::Location locB,
                       int dimension) override {
    updates_.push_back({locA, locB, dimension});
  }

  void ClearUpdates() { updates_.clear(); }

  const std::vector<DimUpdate>& updates() const { return updates_; }
  bool finished() const { return finished_; }

 private:
  std::vector<DimUpdate> updates_;
  bool known_ = false;
  bool value_ = false;
  bool finished_ = false;
};

class EdgeSegmentIntersectorTest : public ::testing::Test {
 protected:
  EdgeSegmentIntersectorTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  static void InitPredicate(TopologyPredicate& pred, const RelateGeometry& a,
                            const RelateGeometry& b) {
    pred.init(a.getDimensionReal(), b.getDimensionReal());
    pred.init(*a.getEnvelope(), *b.getEnvelope());
  }

  static SegmentString* MutableSeg(const SegmentString* ss) {
    return const_cast<SegmentString*>(ss);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(EdgeSegmentIntersectorTest, IsDoneReflectsTopologyComputerResultKnown) {
  auto geom_a = ReadWkt("LINESTRING (0 0, 2 2)");
  auto geom_b = ReadWkt("LINESTRING (0 2, 2 0)");
  RelateGeometry rel_a(geom_a.get());
  RelateGeometry rel_b(geom_b.get());
  auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
  auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

  RelatePredicate::IntersectsPredicate intersects_pred;
  InitPredicate(intersects_pred, rel_a, rel_b);
  TopologyComputer topo_intersects(intersects_pred, rel_a, rel_b);
  EdgeSegmentIntersector intersector(topo_intersects);
  SegmentIntersector* base = &intersector;

  EXPECT_FALSE(intersector.isDone());
  EXPECT_FALSE(base->isDone());

  base->processIntersections(MutableSeg(edges_a[0]), 0, MutableSeg(edges_b[0]),
                             0);
  EXPECT_TRUE(intersector.isDone());
  EXPECT_TRUE(base->isDone());
  EXPECT_TRUE(topo_intersects.getResult());

  auto disjoint_pred = RelatePredicate::disjoint();
  InitPredicate(*disjoint_pred, rel_a, rel_b);
  TopologyComputer topo_disjoint(*disjoint_pred, rel_a, rel_b);
  EdgeSegmentIntersector disjoint_intersector(topo_disjoint);

  EXPECT_FALSE(disjoint_intersector.isDone());
  disjoint_intersector.processIntersections(MutableSeg(edges_a[0]), 0,
                                            MutableSeg(edges_b[0]), 0);
  EXPECT_TRUE(disjoint_intersector.isDone());
  EXPECT_FALSE(topo_disjoint.getResult());

  // Predicate already determined from geometry dimensions before any
  // intersection.
  auto poly_a = ReadWkt("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  auto poly_b = ReadWkt("POLYGON ((1 1, 3 1, 3 3, 1 3, 1 1))");
  RelateGeometry rel_poly_a(poly_a.get());
  RelateGeometry rel_poly_b(poly_b.get());
  auto crosses_pred = RelatePredicate::crosses();
  InitPredicate(*crosses_pred, rel_poly_a, rel_poly_b);
  TopologyComputer topo_crosses(*crosses_pred, rel_poly_a, rel_poly_b);
  EdgeSegmentIntersector crosses_intersector(topo_crosses);
  EXPECT_TRUE(crosses_intersector.isDone());
  EXPECT_FALSE(topo_crosses.getResult());
}

TEST_F(EdgeSegmentIntersectorTest,
       SelfSegmentShortCircuitSkipsSameSegmentStringAndIndex) {
  auto geom_a = ReadWkt("LINESTRING (0 0, 2 2)");
  auto geom_b = ReadWkt("LINESTRING (0 0, 2 2)");
  RelateGeometry rel_a(geom_a.get());
  RelateGeometry rel_b(geom_b.get());
  auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
  auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

  RecordingTopologyPredicate pred;
  InitPredicate(pred, rel_a, rel_b);
  TopologyComputer topo(pred, rel_a, rel_b);
  EdgeSegmentIntersector intersector(topo);
  pred.ClearUpdates();

  intersector.processIntersections(MutableSeg(edges_a[0]), 0,
                                   MutableSeg(edges_a[0]), 0);
  intersector.processIntersections(MutableSeg(edges_b[0]), 0,
                                   MutableSeg(edges_b[0]), 0);
  topo.evaluateNodes();
  topo.finish();

  EXPECT_TRUE(pred.finished());
  EXPECT_TRUE(pred.updates().empty());
}

TEST_F(EdgeSegmentIntersectorTest,
       SelfCrossingSameSegmentStringDifferentIndicesAddsSelfNodes) {
  // Canonical RelateNG case: self-crossing A vs line B identical to segment 0
  // of A.
  auto eval = [&](bool is_a_self_crossing, bool include_self_pair) {
    auto geom_a = ReadWkt(is_a_self_crossing ? "LINESTRING (0 0, 2 2, 0 2, 2 0)"
                                             : "LINESTRING (0 0, 2 2)");
    auto geom_b =
        ReadWkt(is_a_self_crossing ? "LINESTRING (0 0, 2 2)"
                                   : "LINESTRING (0 0, 2 2, 0 2, 2 0)");
    RelateGeometry rel_a(geom_a.get());
    RelateGeometry rel_b(geom_b.get());
    auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

    RelateMatrixPredicate pred;
    InitPredicate(pred, rel_a, rel_b);
    TopologyComputer topo(pred, rel_a, rel_b);
    EdgeSegmentIntersector intersector(topo);

    if (is_a_self_crossing) {
      if (include_self_pair) {
        intersector.processIntersections(MutableSeg(edges_a[0]), 0,
                                         MutableSeg(edges_a[0]), 2);
      }
      for (std::size_t i = 0; i < 3; ++i) {
        intersector.processIntersections(MutableSeg(edges_a[0]), i,
                                         MutableSeg(edges_b[0]), 0);
      }
    } else {
      if (include_self_pair) {
        intersector.processIntersections(MutableSeg(edges_b[0]), 0,
                                         MutableSeg(edges_b[0]), 2);
      }
      for (std::size_t i = 0; i < 3; ++i) {
        intersector.processIntersections(MutableSeg(edges_a[0]), 0,
                                         MutableSeg(edges_b[0]), i);
      }
    }

    topo.evaluateNodes();
    topo.finish();
    return pred.getIM();
  };

  // When self-crossing is processed for A, B's segment at (1,1) is properly
  // recognized as coinciding with A's segment 0 rather than A's exterior.
  std::unique_ptr<IntersectionMatrix> im_a_without_self = eval(true, false);
  std::unique_ptr<IntersectionMatrix> im_a_with_self = eval(true, true);
  EXPECT_EQ(im_a_without_self->get(Location::EXTERIOR, Location::INTERIOR),
            Dimension::L);
  EXPECT_EQ(im_a_with_self->get(Location::INTERIOR, Location::INTERIOR),
            Dimension::L);
  EXPECT_EQ(im_a_with_self->get(Location::EXTERIOR, Location::INTERIOR),
            Dimension::False);

  // Symmetric check when self-crossing segment string belongs to B (isA() ==
  // false).
  std::unique_ptr<IntersectionMatrix> im_b_without_self = eval(false, false);
  std::unique_ptr<IntersectionMatrix> im_b_with_self = eval(false, true);
  EXPECT_EQ(im_b_without_self->get(Location::INTERIOR, Location::EXTERIOR),
            Dimension::L);
  EXPECT_EQ(im_b_with_self->get(Location::INTERIOR, Location::INTERIOR),
            Dimension::L);
  EXPECT_EQ(im_b_with_self->get(Location::INTERIOR, Location::EXTERIOR),
            Dimension::False);
}

TEST_F(EdgeSegmentIntersectorTest, SwapsArgumentsWhenFirstSegmentStringIsB) {
  auto run_order = [&](bool b_first) {
    auto geom_a = ReadWkt("LINESTRING (0 1, 2 1)");
    auto geom_b = ReadWkt("LINESTRING (1 0, 1 2)");
    RelateGeometry rel_a(geom_a.get());
    RelateGeometry rel_b(geom_b.get());
    auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

    RelateMatrixPredicate pred;
    InitPredicate(pred, rel_a, rel_b);
    TopologyComputer topo(pred, rel_a, rel_b);
    EdgeSegmentIntersector intersector(topo);

    if (b_first) {
      intersector.processIntersections(MutableSeg(edges_b[0]), 0,
                                       MutableSeg(edges_a[0]), 0);
    } else {
      intersector.processIntersections(MutableSeg(edges_a[0]), 0,
                                       MutableSeg(edges_b[0]), 0);
    }
    topo.evaluateNodes();
    topo.finish();
    return pred.getIM()->toString();
  };

  std::string im_ab = run_order(false);
  std::string im_ba = run_order(true);
  EXPECT_EQ(im_ab, im_ba);
  EXPECT_EQ(im_ab, "0F1FFF1F2");
}

TEST_F(EdgeSegmentIntersectorTest, NonIntersectingSegmentsReturnEarly) {
  auto geom_a = ReadWkt("LINESTRING (0 0, 10 10)");
  auto geom_b = ReadWkt("LINESTRING (0 2, 8 10)");
  RelateGeometry rel_a(geom_a.get());
  RelateGeometry rel_b(geom_b.get());
  auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
  auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

  RecordingTopologyPredicate pred;
  InitPredicate(pred, rel_a, rel_b);
  TopologyComputer topo(pred, rel_a, rel_b);
  EdgeSegmentIntersector intersector(topo);
  pred.ClearUpdates();

  // Overlapping envelopes, disjoint parallel segments.
  intersector.processIntersections(MutableSeg(edges_a[0]), 0,
                                   MutableSeg(edges_b[0]), 0);
  topo.evaluateNodes();
  topo.finish();

  EXPECT_TRUE(pred.finished());
  EXPECT_TRUE(pred.updates().empty());
}

TEST_F(EdgeSegmentIntersectorTest,
       EndpointContainingSegmentFilteringForOpenLinesAndClosedRings) {
  // Subcase 1: intPt at non-final end vertex of A seg 0 -> A
  // isContainingSegment false.
  {
    auto geom_a = ReadWkt("LINESTRING (0 0, 1 1, 2 0)");
    auto geom_b = ReadWkt("LINESTRING (1 1, 1 3)");
    RelateGeometry rel_a(geom_a.get());
    RelateGeometry rel_b(geom_b.get());
    auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

    RecordingTopologyPredicate pred;
    InitPredicate(pred, rel_a, rel_b);
    TopologyComputer topo(pred, rel_a, rel_b);
    EdgeSegmentIntersector intersector(topo);
    pred.ClearUpdates();

    intersector.processIntersections(MutableSeg(edges_a[0]), 0,
                                     MutableSeg(edges_b[0]), 0);
    EXPECT_TRUE(pred.updates().empty());

    intersector.processIntersections(MutableSeg(edges_a[0]), 1,
                                     MutableSeg(edges_b[0]), 0);
    ASSERT_EQ(pred.updates().size(), 1u);
    EXPECT_EQ(pred.updates()[0], (DimUpdate{Location::INTERIOR,
                                            Location::BOUNDARY, Dimension::P}));
  }

  // Subcase 2: intPt at non-final end vertex of B seg 0 -> B
  // isContainingSegment false.
  {
    auto geom_a = ReadWkt("LINESTRING (1 1, 1 3)");
    auto geom_b = ReadWkt("LINESTRING (0 0, 1 1, 2 0)");
    RelateGeometry rel_a(geom_a.get());
    RelateGeometry rel_b(geom_b.get());
    auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

    RecordingTopologyPredicate pred;
    InitPredicate(pred, rel_a, rel_b);
    TopologyComputer topo(pred, rel_a, rel_b);
    EdgeSegmentIntersector intersector(topo);
    pred.ClearUpdates();

    intersector.processIntersections(MutableSeg(edges_a[0]), 0,
                                     MutableSeg(edges_b[0]), 0);
    EXPECT_TRUE(pred.updates().empty());

    intersector.processIntersections(MutableSeg(edges_a[0]), 0,
                                     MutableSeg(edges_b[0]), 1);
    ASSERT_EQ(pred.updates().size(), 1u);
    EXPECT_EQ(pred.updates()[0], (DimUpdate{Location::BOUNDARY,
                                            Location::INTERIOR, Dimension::P}));
  }

  // Subcase 3: Final endpoint of open line IS contained by its final segment.
  {
    auto geom_a = ReadWkt("LINESTRING (0 0, 1 1, 2 0)");
    auto geom_b = ReadWkt("LINESTRING (2 0, 3 0)");
    RelateGeometry rel_a(geom_a.get());
    RelateGeometry rel_b(geom_b.get());
    auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

    RecordingTopologyPredicate pred;
    InitPredicate(pred, rel_a, rel_b);
    TopologyComputer topo(pred, rel_a, rel_b);
    EdgeSegmentIntersector intersector(topo);
    pred.ClearUpdates();

    intersector.processIntersections(MutableSeg(edges_a[0]), 1,
                                     MutableSeg(edges_b[0]), 0);
    ASSERT_EQ(pred.updates().size(), 1u);
    EXPECT_EQ(pred.updates()[0], (DimUpdate{Location::BOUNDARY,
                                            Location::BOUNDARY, Dimension::P}));
  }

  // Subcase 4: Closed ring closing vertex is owned by segment 0, NOT final
  // segment.
  {
    auto geom_a = ReadWkt("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
    auto geom_b = ReadWkt("LINESTRING (-1 0, 0 0)");
    RelateGeometry rel_a(geom_a.get());
    RelateGeometry rel_b(geom_b.get());
    auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

    RecordingTopologyPredicate pred;
    InitPredicate(pred, rel_a, rel_b);
    TopologyComputer topo(pred, rel_a, rel_b);
    EdgeSegmentIntersector intersector(topo);
    pred.ClearUpdates();

    // Final segment (index 3) ends at closing vertex (0, 0) -> skipped.
    intersector.processIntersections(MutableSeg(edges_a[0]), 3,
                                     MutableSeg(edges_b[0]), 0);
    EXPECT_TRUE(pred.updates().empty());

    // First segment (index 0) starts at (0, 0) -> accepted.
    intersector.processIntersections(MutableSeg(edges_a[0]), 0,
                                     MutableSeg(edges_b[0]), 0);
    ASSERT_EQ(pred.updates().size(), 1u);
    EXPECT_EQ(pred.updates()[0], (DimUpdate{Location::BOUNDARY,
                                            Location::BOUNDARY, Dimension::P}));
  }
}

TEST_F(EdgeSegmentIntersectorTest,
       CollinearIntersectionLoopProcessesMultiplePoints) {
  auto geom_a = ReadWkt("LINESTRING (0 0, 3 0)");
  auto geom_b = ReadWkt("LINESTRING (1 0, 2 0, 4 0)");
  RelateGeometry rel_a(geom_a.get());
  RelateGeometry rel_b(geom_b.get());
  auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
  auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

  RelateMatrixPredicate pred;
  InitPredicate(pred, rel_a, rel_b);
  TopologyComputer topo(pred, rel_a, rel_b);
  EdgeSegmentIntersector intersector(topo);

  // Segment 0 of B overlaps (1,0)-(2,0): (1,0) is accepted, (2,0) is skipped on
  // seg 0.
  intersector.processIntersections(MutableSeg(edges_a[0]), 0,
                                   MutableSeg(edges_b[0]), 0);
  // Segment 1 of B overlaps (2,0)-(3,0): both (2,0) and (3,0) are accepted.
  intersector.processIntersections(MutableSeg(edges_a[0]), 0,
                                   MutableSeg(edges_b[0]), 1);

  topo.evaluateNodes();
  topo.finish();

  std::unique_ptr<IntersectionMatrix> im = pred.getIM();
  EXPECT_EQ(im->get(Location::INTERIOR, Location::INTERIOR), Dimension::L);
  EXPECT_EQ(im->get(Location::INTERIOR, Location::BOUNDARY), Dimension::P);
  EXPECT_EQ(im->get(Location::BOUNDARY, Location::INTERIOR), Dimension::P);
}

TEST_F(EdgeSegmentIntersectorTest, PolygonPolygonCrossingAndTouchingEdges) {
  // Crossing polygons: proper edge crossings set II=2 immediately.
  {
    auto geom_a = ReadWkt("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
    auto geom_b = ReadWkt("POLYGON ((2 2, 6 2, 6 6, 2 6, 2 2))");
    RelateGeometry rel_a(geom_a.get());
    RelateGeometry rel_b(geom_b.get());
    auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

    RelateMatrixPredicate pred;
    InitPredicate(pred, rel_a, rel_b);
    TopologyComputer topo(pred, rel_a, rel_b);
    EdgeSegmentIntersector intersector(topo);

    for (std::size_t i = 0; i < edges_a[0]->size() - 1; ++i) {
      for (std::size_t j = 0; j < edges_b[0]->size() - 1; ++j) {
        intersector.processIntersections(MutableSeg(edges_a[0]), i,
                                         MutableSeg(edges_b[0]), j);
      }
    }
    EXPECT_EQ(pred.getDimension(Location::INTERIOR, Location::INTERIOR),
              Dimension::A);
    EXPECT_EQ(pred.getDimension(Location::BOUNDARY, Location::BOUNDARY),
              Dimension::P);

    topo.evaluateNodes();
    topo.finish();
    std::unique_ptr<IntersectionMatrix> im = pred.getIM();
    EXPECT_EQ(im->get(Location::INTERIOR, Location::BOUNDARY), Dimension::L);
    EXPECT_EQ(im->get(Location::BOUNDARY, Location::INTERIOR), Dimension::L);
  }

  // Adjacent polygons sharing an edge: BB=1, II remains False.
  {
    auto geom_a = ReadWkt("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
    auto geom_b = ReadWkt("POLYGON ((2 0, 4 0, 4 2, 2 2, 2 0))");
    RelateGeometry rel_a(geom_a.get());
    RelateGeometry rel_b(geom_b.get());
    auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

    RelateMatrixPredicate pred;
    InitPredicate(pred, rel_a, rel_b);
    TopologyComputer topo(pred, rel_a, rel_b);
    EdgeSegmentIntersector intersector(topo);

    for (std::size_t i = 0; i < edges_a[0]->size() - 1; ++i) {
      for (std::size_t j = 0; j < edges_b[0]->size() - 1; ++j) {
        intersector.processIntersections(MutableSeg(edges_a[0]), i,
                                         MutableSeg(edges_b[0]), j);
      }
    }
    topo.evaluateNodes();
    topo.finish();

    std::unique_ptr<IntersectionMatrix> im = pred.getIM();
    EXPECT_EQ(im->get(Location::INTERIOR, Location::INTERIOR),
              Dimension::False);
    EXPECT_EQ(im->get(Location::BOUNDARY, Location::BOUNDARY), Dimension::L);
  }
}

TEST_F(EdgeSegmentIntersectorTest,
       ClosedRingOnBFiltersClosingSegmentAtInteriorIntersection) {
  auto geom_a = ReadWkt("LINESTRING (-1 -1, 1 1)");
  auto geom_b = ReadWkt("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  RelateGeometry rel_a(geom_a.get());
  RelateGeometry rel_b(geom_b.get());
  auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
  auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

  RecordingTopologyPredicate pred;
  InitPredicate(pred, rel_a, rel_b);
  TopologyComputer topo(pred, rel_a, rel_b);
  EdgeSegmentIntersector intersector(topo);
  pred.ClearUpdates();

  // Final segment (index 3) of closed ring B ends at (0, 0) -> skipped.
  intersector.processIntersections(MutableSeg(edges_a[0]), 0,
                                   MutableSeg(edges_b[0]), 3);
  EXPECT_TRUE(pred.updates().empty());

  // First segment (index 0) of closed ring B starts at (0, 0) -> accepted.
  intersector.processIntersections(MutableSeg(edges_a[0]), 0,
                                   MutableSeg(edges_b[0]), 0);
  ASSERT_EQ(pred.updates().size(), 1u);
  EXPECT_EQ(pred.updates()[0],
            (DimUpdate{Location::INTERIOR, Location::BOUNDARY, Dimension::P}));
}

TEST_F(EdgeSegmentIntersectorTest,
       DifferentSegmentStringsFromSameGeometrySelfIntersect) {
  auto run_self_components =
      [&](bool self_in_a) -> std::unique_ptr<IntersectionMatrix> {
    auto geom_a = ReadWkt(self_in_a ? "MULTILINESTRING ((0 0, 2 2), (0 2, 2 0))"
                                    : "LINESTRING (0 0, 2 2)");
    auto geom_b =
        ReadWkt(self_in_a ? "LINESTRING (0 0, 2 2)"
                          : "MULTILINESTRING ((0 0, 2 2), (0 2, 2 0))");
    RelateGeometry rel_a(geom_a.get());
    RelateGeometry rel_b(geom_b.get());
    auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

    RelateMatrixPredicate pred;
    InitPredicate(pred, rel_a, rel_b);
    TopologyComputer topo(pred, rel_a, rel_b);
    EdgeSegmentIntersector intersector(topo);

    if (self_in_a) {
      EXPECT_EQ(edges_a.size(), 2u);
      EXPECT_EQ(edges_b.size(), 1u);
      if (edges_a.size() != 2u || edges_b.empty()) {
        return nullptr;
      }
      intersector.processIntersections(MutableSeg(edges_a[0]), 0,
                                       MutableSeg(edges_a[1]), 0);
      intersector.processIntersections(MutableSeg(edges_a[0]), 0,
                                       MutableSeg(edges_b[0]), 0);
      intersector.processIntersections(MutableSeg(edges_a[1]), 0,
                                       MutableSeg(edges_b[0]), 0);
    } else {
      EXPECT_EQ(edges_a.size(), 1u);
      EXPECT_EQ(edges_b.size(), 2u);
      if (edges_a.empty() || edges_b.size() != 2u) {
        return nullptr;
      }
      intersector.processIntersections(MutableSeg(edges_b[0]), 0,
                                       MutableSeg(edges_b[1]), 0);
      intersector.processIntersections(MutableSeg(edges_a[0]), 0,
                                       MutableSeg(edges_b[0]), 0);
      intersector.processIntersections(MutableSeg(edges_a[0]), 0,
                                       MutableSeg(edges_b[1]), 0);
    }

    topo.evaluateNodes();
    topo.finish();
    return pred.getIM();
  };

  std::unique_ptr<IntersectionMatrix> im_a = run_self_components(true);
  ASSERT_NE(im_a, nullptr);
  EXPECT_EQ(im_a->get(Location::INTERIOR, Location::INTERIOR), Dimension::L);
  EXPECT_EQ(im_a->get(Location::EXTERIOR, Location::INTERIOR),
            Dimension::False);

  std::unique_ptr<IntersectionMatrix> im_b = run_self_components(false);
  ASSERT_NE(im_b, nullptr);
  EXPECT_EQ(im_b->get(Location::INTERIOR, Location::INTERIOR), Dimension::L);
  EXPECT_EQ(im_b->get(Location::INTERIOR, Location::EXTERIOR),
            Dimension::False);
}

TEST_F(EdgeSegmentIntersectorTest, HandlesXYZAndXYZMCoordinates) {
  {
    auto geom_a = ReadWkt("LINESTRING Z (0 0 10, 2 2 20)");
    auto geom_b = ReadWkt("LINESTRING Z (0 2 30, 2 0 40)");
    RelateGeometry rel_a(geom_a.get());
    RelateGeometry rel_b(geom_b.get());
    auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

    RelateMatrixPredicate pred;
    InitPredicate(pred, rel_a, rel_b);
    TopologyComputer topo(pred, rel_a, rel_b);
    EdgeSegmentIntersector intersector(topo);

    intersector.processIntersections(MutableSeg(edges_a[0]), 0,
                                     MutableSeg(edges_b[0]), 0);
    topo.evaluateNodes();
    topo.finish();

    EXPECT_EQ(pred.getIM()->toString(), "0F1FFF1F2");
  }

  {
    auto geom_a = ReadWkt("LINESTRING ZM (0 0 10 1, 2 2 20 2)");
    auto geom_b = ReadWkt("LINESTRING ZM (0 2 30 3, 2 0 40 4)");
    RelateGeometry rel_a(geom_a.get());
    RelateGeometry rel_b(geom_b.get());
    auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

    RelateMatrixPredicate pred;
    InitPredicate(pred, rel_a, rel_b);
    TopologyComputer topo(pred, rel_a, rel_b);
    EdgeSegmentIntersector intersector(topo);

    intersector.processIntersections(MutableSeg(edges_a[0]), 0,
                                     MutableSeg(edges_b[0]), 0);
    topo.evaluateNodes();
    topo.finish();

    EXPECT_EQ(pred.getIM()->toString(), "0F1FFF1F2");
  }
}

TEST_F(EdgeSegmentIntersectorTest, EdgeSetIntersectorIntegrationAndEarlyStop) {
  // Full matrix evaluation via EdgeSetIntersector for a self-crossing line.
  {
    auto geom_a = ReadWkt("LINESTRING (0 0, 4 4, 0 4, 4 0)");
    auto geom_b = ReadWkt("LINESTRING (0 0, 4 4)");
    RelateGeometry rel_a(geom_a.get());
    RelateGeometry rel_b(geom_b.get());
    Envelope env_int;
    rel_a.getEnvelope()->intersection(*rel_b.getEnvelope(), env_int);
    auto edges_a =
        rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, &env_int);
    auto edges_b =
        rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, &env_int);

    RelateMatrixPredicate pred;
    InitPredicate(pred, rel_a, rel_b);
    TopologyComputer topo(pred, rel_a, rel_b);
    EdgeSegmentIntersector intersector(topo);
    EdgeSetIntersector edge_set(edges_a, edges_b, &env_int);

    EXPECT_FALSE(intersector.isDone());
    edge_set.process(intersector);
    EXPECT_FALSE(intersector.isDone());
    topo.evaluateNodes();
    topo.finish();

    std::unique_ptr<IntersectionMatrix> im = pred.getIM();
    EXPECT_EQ(im->get(Location::INTERIOR, Location::INTERIOR), Dimension::L);
    EXPECT_EQ(im->get(Location::EXTERIOR, Location::INTERIOR),
              Dimension::False);
  }

  // Early termination via isDone() when intersects becomes known.
  {
    auto geom_a = ReadWkt("MULTILINESTRING ((0 0, 2 2), (10 0, 12 2))");
    auto geom_b = ReadWkt("MULTILINESTRING ((0 2, 2 0), (10 2, 12 0))");
    RelateGeometry rel_a(geom_a.get());
    RelateGeometry rel_b(geom_b.get());
    Envelope env_int;
    rel_a.getEnvelope()->intersection(*rel_b.getEnvelope(), env_int);
    auto edges_a =
        rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, &env_int);
    auto edges_b =
        rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, &env_int);

    RelatePredicate::IntersectsPredicate pred;
    InitPredicate(pred, rel_a, rel_b);
    TopologyComputer topo(pred, rel_a, rel_b);
    EdgeSegmentIntersector intersector(topo);
    EdgeSetIntersector edge_set(edges_a, edges_b, &env_int);

    EXPECT_FALSE(intersector.isDone());
    edge_set.process(intersector);
    EXPECT_TRUE(intersector.isDone());
    EXPECT_TRUE(topo.getResult());
  }

  // Polymorphic ownership via SegmentIntersector base pointer.
  {
    auto geom_a = ReadWkt("LINESTRING (0 1, 2 1)");
    auto geom_b = ReadWkt("LINESTRING (1 0, 1 2)");
    RelateGeometry rel_a(geom_a.get());
    RelateGeometry rel_b(geom_b.get());
    auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

    RelateMatrixPredicate pred;
    InitPredicate(pred, rel_a, rel_b);
    TopologyComputer topo(pred, rel_a, rel_b);
    std::unique_ptr<SegmentIntersector> intersector =
        std::make_unique<EdgeSegmentIntersector>(topo);

    intersector->processIntersections(MutableSeg(edges_a[0]), 0,
                                      MutableSeg(edges_b[0]), 0);
    EXPECT_FALSE(intersector->isDone());
    topo.evaluateNodes();
    topo.finish();
    EXPECT_EQ(pred.getIM()->toString(), "0F1FFF1F2");
  }
}

TEST_F(EdgeSegmentIntersectorTest,
       CollinearOppositeDirectionAndSinglePointTouch) {
  auto evaluate_single_pair = [&](const std::string& wkt_a,
                                  const std::string& wkt_b) {
    auto geom_a = ReadWkt(wkt_a);
    auto geom_b = ReadWkt(wkt_b);
    RelateGeometry rel_a(geom_a.get());
    RelateGeometry rel_b(geom_b.get());
    auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

    RelateMatrixPredicate pred;
    InitPredicate(pred, rel_a, rel_b);
    TopologyComputer topo(pred, rel_a, rel_b);
    EdgeSegmentIntersector intersector(topo);

    EXPECT_EQ(edges_a.size(), 1u);
    EXPECT_EQ(edges_b.size(), 1u);
    if (!edges_a.empty() && !edges_b.empty()) {
      intersector.processIntersections(MutableSeg(edges_a[0]), 0,
                                       MutableSeg(edges_b[0]), 0);
    }
    topo.evaluateNodes();
    topo.finish();
    return pred.getIM();
  };

  // Opposite-direction collinear overlap processes both overlap endpoints.
  {
    std::unique_ptr<IntersectionMatrix> im =
        evaluate_single_pair("LINESTRING (0 0, 4 0)", "LINESTRING (3 0, 1 0)");
    EXPECT_EQ(im->get(Location::INTERIOR, Location::INTERIOR), Dimension::L);
    EXPECT_EQ(im->get(Location::INTERIOR, Location::BOUNDARY), Dimension::P);
    EXPECT_EQ(im->get(Location::EXTERIOR, Location::INTERIOR),
              Dimension::False);
  }

  // Identical single-segment lines share both endpoints and full interior.
  {
    std::unique_ptr<IntersectionMatrix> im =
        evaluate_single_pair("LINESTRING (0 0, 2 0)", "LINESTRING (0 0, 2 0)");
    EXPECT_EQ(im->get(Location::INTERIOR, Location::INTERIOR), Dimension::L);
    EXPECT_EQ(im->get(Location::BOUNDARY, Location::BOUNDARY), Dimension::P);
    EXPECT_EQ(im->get(Location::INTERIOR, Location::EXTERIOR),
              Dimension::False);
    EXPECT_EQ(im->get(Location::EXTERIOR, Location::INTERIOR),
              Dimension::False);
  }

  // Collinear segments meeting at a single endpoint record only a 0D node.
  {
    std::unique_ptr<IntersectionMatrix> im =
        evaluate_single_pair("LINESTRING (0 0, 2 0)", "LINESTRING (2 0, 4 0)");
    EXPECT_EQ(im->get(Location::BOUNDARY, Location::BOUNDARY), Dimension::P);
    EXPECT_EQ(im->get(Location::INTERIOR, Location::INTERIOR),
              Dimension::False);
  }
}

}  // namespace
}  // namespace geos_google
