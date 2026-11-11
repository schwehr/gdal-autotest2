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
// third_party/geos/src/operation/relateng/EdgeSetIntersector.cpp.

#include "third_party/geos/include/geos/operation/relateng/EdgeSetIntersector.h"

#include <cstddef>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/IntersectionMatrix.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/relateng/EdgeSegmentIntersector.h"
#include "third_party/geos/include/geos/operation/relateng/RelateGeometry.h"
#include "third_party/geos/include/geos/operation/relateng/RelateMatrixPredicate.h"
#include "third_party/geos/include/geos/operation/relateng/RelatePredicate.h"
#include "third_party/geos/include/geos/operation/relateng/TopologyComputer.h"
#include "third_party/geos/include/geos/operation/relateng/TopologyPredicate.h"
#include "third_party/geos/include/geos/util/GEOSException.h"
#include "third_party/geos/include/geos/util/Interrupt.h"

namespace geos_google {
namespace {

using geos::geom::Dimension;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::IntersectionMatrix;
using geos::geom::Location;
using geos::io::WKTReader;
using geos::operation::relateng::EdgeSegmentIntersector;
using geos::operation::relateng::EdgeSetIntersector;
using geos::operation::relateng::RelateGeometry;
using geos::operation::relateng::RelateMatrixPredicate;
using geos::operation::relateng::RelatePredicate;
using geos::operation::relateng::TopologyComputer;
using geos::operation::relateng::TopologyPredicate;
using geos::util::CurrentThreadInterrupt;
using geos::util::GEOSException;
using geos::util::Interrupt;

class CountingTopologyPredicate : public TopologyPredicate {
 public:
  explicit CountingTopologyPredicate(bool stop_after_first)
      : stop_after_first_(stop_after_first) {}

  std::string name() const override { return "counting"; }

  void finish() override {}

  bool isKnown() const override { return known_; }

  bool value() const override { return known_; }

  void updateDimension(geos::geom::Location /*locA*/,
                       geos::geom::Location /*locB*/,
                       int /*dimension*/) override {
    ++update_count_;
    if (stop_after_first_) {
      known_ = true;
    }
  }

  void ResetCount() {
    update_count_ = 0;
    known_ = false;
  }

  std::size_t update_count() const { return update_count_; }

 private:
  bool stop_after_first_;
  bool known_ = false;
  std::size_t update_count_ = 0;
};

int AlwaysInterruptCallback(void* /*data*/) { return 1; }

class EdgeSetIntersectorTest : public ::testing::Test {
 protected:
  EdgeSetIntersectorTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  static void InitPredicate(TopologyPredicate& pred, const RelateGeometry& a,
                            const RelateGeometry& b) {
    pred.init(a.getDimensionReal(), b.getDimensionReal());
    pred.init(*a.getEnvelope(), *b.getEnvelope());
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(EdgeSetIntersectorTest, EmptyEdgeSetsAndSingleChainAreNoOps) {
  auto point_a = ReadWkt("POINT (0 0)");
  auto point_b = ReadWkt("POINT (1 1)");
  RelateGeometry rel_pt_a(point_a.get());
  RelateGeometry rel_pt_b(point_b.get());
  auto empty_edges_a =
      rel_pt_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
  auto empty_edges_b =
      rel_pt_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);
  EXPECT_TRUE(empty_edges_a.empty());
  EXPECT_TRUE(empty_edges_b.empty());

  RelateMatrixPredicate empty_pred;
  InitPredicate(empty_pred, rel_pt_a, rel_pt_b);
  TopologyComputer empty_topo(empty_pred, rel_pt_a, rel_pt_b);
  EdgeSegmentIntersector empty_intersector(empty_topo);
  EdgeSetIntersector empty_esi(empty_edges_a, empty_edges_b, nullptr);
  empty_esi.process(empty_intersector);
  empty_topo.evaluateNodes();
  empty_topo.finish();
  EXPECT_EQ(empty_pred.getIM()->toString(), "FFFFFFFF2");

  // Single monotone chain in total (numItems == 1 < 2 in TemplateSTRtree).
  auto line_a = ReadWkt("LINESTRING (0 0, 2 2)");
  RelateGeometry rel_line_a(line_a.get());
  auto single_edge_a =
      rel_line_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
  ASSERT_EQ(single_edge_a.size(), 1u);

  RelateMatrixPredicate single_pred;
  InitPredicate(single_pred, rel_line_a, rel_pt_b);
  TopologyComputer single_topo(single_pred, rel_line_a, rel_pt_b);
  EdgeSegmentIntersector single_intersector(single_topo);
  EdgeSetIntersector single_esi(single_edge_a, empty_edges_b, nullptr);
  single_esi.process(single_intersector);
  single_topo.evaluateNodes();
  single_topo.finish();
  EXPECT_EQ(single_pred.getIM()->get(Location::INTERIOR, Location::INTERIOR),
            Dimension::False);
}

TEST_F(EdgeSetIntersectorTest, NullEnvelopeIndexesAllChainsAndFindsCrossings) {
  auto geom_a = ReadWkt("LINESTRING (0 0, 2 2, 4 0, 6 2)");
  auto geom_b = ReadWkt("LINESTRING (0 1, 6 1)");
  RelateGeometry rel_a(geom_a.get());
  RelateGeometry rel_b(geom_b.get());
  auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
  auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

  RelateMatrixPredicate pred;
  InitPredicate(pred, rel_a, rel_b);
  TopologyComputer topo(pred, rel_a, rel_b);
  EdgeSegmentIntersector intersector(topo);
  EdgeSetIntersector esi(edges_a, edges_b, nullptr);

  esi.process(intersector);
  topo.evaluateNodes();
  topo.finish();

  EXPECT_EQ(pred.getIM()->toString(), "0F1FFF1F2");
}

TEST_F(EdgeSetIntersectorTest, EnvelopeFiltersOutChainsOutsideBounds) {
  // Chain 0 & 1 of A are near [0,2]x[0,2]; chain 2 is in [5,12]x[-1,2].
  auto geom_a = ReadWkt("LINESTRING (0 0, 1 1, 2 0, 5 -1, 10 0, 12 2)");
  // Component 0 of B is inside [0,2]x[0,2] (disjoint from A);
  // Component 1 of B crosses chain 2 of A at (11, 1).
  auto geom_b = ReadWkt("MULTILINESTRING ((0 1.5, 0.5 2), (10 2, 12 0))");

  auto run_with_env = [&](const Envelope* env) {
    RelateGeometry rel_a(geom_a.get());
    RelateGeometry rel_b(geom_b.get());
    auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

    RelateMatrixPredicate pred;
    InitPredicate(pred, rel_a, rel_b);
    TopologyComputer topo(pred, rel_a, rel_b);
    EdgeSegmentIntersector intersector(topo);
    EdgeSetIntersector esi(edges_a, edges_b, env);

    esi.process(intersector);
    topo.evaluateNodes();
    topo.finish();
    return pred.getIM();
  };

  // Strict envelope excludes out-of-bounds crossing chains.
  Envelope clip_env(0.0, 2.0, 0.0, 2.0);
  std::unique_ptr<IntersectionMatrix> im_clipped = run_with_env(&clip_env);
  EXPECT_EQ(im_clipped->get(Location::INTERIOR, Location::INTERIOR),
            Dimension::False);

  // Full envelope includes the crossing chain pair at (11, 1).
  Envelope full_env(0.0, 12.0, -1.0, 2.0);
  std::unique_ptr<IntersectionMatrix> im_full = run_with_env(&full_env);
  EXPECT_EQ(im_full->get(Location::INTERIOR, Location::INTERIOR), Dimension::P);
}

TEST_F(EdgeSetIntersectorTest, SelfNodingIncludesSameGeometryCrossings) {
  {
    auto geom_a = ReadWkt("LINESTRING (0 0, 2 2, 0 2, 2 0)");
    auto geom_b = ReadWkt("LINESTRING (0 0, 2 2)");
    RelateGeometry rel_a(geom_a.get());
    RelateGeometry rel_b(geom_b.get());
    auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

    RelateMatrixPredicate pred;
    InitPredicate(pred, rel_a, rel_b);
    TopologyComputer topo(pred, rel_a, rel_b);
    EdgeSegmentIntersector intersector(topo);
    EdgeSetIntersector esi(edges_a, edges_b, nullptr);

    esi.process(intersector);
    topo.evaluateNodes();
    topo.finish();

    std::unique_ptr<IntersectionMatrix> im = pred.getIM();
    EXPECT_EQ(im->get(Location::INTERIOR, Location::INTERIOR), Dimension::L);
    EXPECT_EQ(im->get(Location::EXTERIOR, Location::INTERIOR),
              Dimension::False);
  }

  {
    auto geom_a = ReadWkt("LINESTRING (0 0, 2 2)");
    auto geom_b = ReadWkt("LINESTRING (0 0, 2 2, 0 2, 2 0)");
    RelateGeometry rel_a(geom_a.get());
    RelateGeometry rel_b(geom_b.get());
    auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

    RelateMatrixPredicate pred;
    InitPredicate(pred, rel_a, rel_b);
    TopologyComputer topo(pred, rel_a, rel_b);
    EdgeSegmentIntersector intersector(topo);
    EdgeSetIntersector esi(edges_a, edges_b, nullptr);

    esi.process(intersector);
    topo.evaluateNodes();
    topo.finish();

    std::unique_ptr<IntersectionMatrix> im = pred.getIM();
    EXPECT_EQ(im->get(Location::INTERIOR, Location::INTERIOR), Dimension::L);
    EXPECT_EQ(im->get(Location::INTERIOR, Location::EXTERIOR),
              Dimension::False);
  }
}

TEST_F(EdgeSetIntersectorTest,
       DisjointAndOverlappingEnvelopeSegmentsWithoutIntersection) {
  // Disjoint chain envelopes.
  {
    auto geom_a = ReadWkt("LINESTRING (0 0, 1 1)");
    auto geom_b = ReadWkt("LINESTRING (10 10, 11 11)");
    RelateGeometry rel_a(geom_a.get());
    RelateGeometry rel_b(geom_b.get());
    auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

    RelateMatrixPredicate pred;
    InitPredicate(pred, rel_a, rel_b);
    TopologyComputer topo(pred, rel_a, rel_b);
    EdgeSegmentIntersector intersector(topo);
    EdgeSetIntersector esi(edges_a, edges_b, nullptr);

    esi.process(intersector);
    topo.evaluateNodes();
    topo.finish();
    EXPECT_EQ(pred.getIM()->get(Location::INTERIOR, Location::INTERIOR),
              Dimension::False);
  }

  // Overlapping chain envelopes without any segment intersection.
  {
    auto geom_a = ReadWkt("LINESTRING (0 0, 10 10)");
    auto geom_b = ReadWkt("LINESTRING (0 2, 8 10)");
    RelateGeometry rel_a(geom_a.get());
    RelateGeometry rel_b(geom_b.get());
    auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

    RelateMatrixPredicate pred;
    InitPredicate(pred, rel_a, rel_b);
    TopologyComputer topo(pred, rel_a, rel_b);
    EdgeSegmentIntersector intersector(topo);
    EdgeSetIntersector esi(edges_a, edges_b, nullptr);

    esi.process(intersector);
    topo.evaluateNodes();
    topo.finish();
    EXPECT_EQ(pred.getIM()->get(Location::INTERIOR, Location::INTERIOR),
              Dimension::False);
  }
}

TEST_F(EdgeSetIntersectorTest, StopsEarlyWhenIntersectorIsDone) {
  // Single horizontal chain A has strictly lowest getX and getY so it is
  // placed first in the STRtree; B's 8 vertical chains are pairwise disjoint.
  auto geom_a = ReadWkt("LINESTRING (-10 2, 10 2)");
  auto geom_b = ReadWkt(
      "MULTILINESTRING ((1 0, 1 10), (2 0, 2 10), (3 0, 3 10), (4 0, 4 10), "
      "(5 0, 5 10), (6 0, 6 10), (7 0, 7 10), (8 0, 8 10))");

  auto run_counting = [&](bool stop_after_first) {
    RelateGeometry rel_a(geom_a.get());
    RelateGeometry rel_b(geom_b.get());
    auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

    CountingTopologyPredicate pred(stop_after_first);
    InitPredicate(pred, rel_a, rel_b);
    TopologyComputer topo(pred, rel_a, rel_b);
    pred.ResetCount();

    EdgeSegmentIntersector intersector(topo);
    EdgeSetIntersector esi(edges_a, edges_b, nullptr);
    esi.process(intersector);
    return pred.update_count();
  };

  std::size_t full_count = run_counting(false);
  std::size_t early_count = run_counting(true);
  EXPECT_EQ(full_count, 8u);
  EXPECT_EQ(early_count, 1u);

  // Confirm with standard IntersectsPredicate and verify the same
  // EdgeSetIntersector can be processed again with a full matrix predicate.
  RelateGeometry rel_a(geom_a.get());
  RelateGeometry rel_b(geom_b.get());
  auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
  auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);
  EdgeSetIntersector esi(edges_a, edges_b, nullptr);

  RelatePredicate::IntersectsPredicate intersects_pred;
  InitPredicate(intersects_pred, rel_a, rel_b);
  TopologyComputer intersects_topo(intersects_pred, rel_a, rel_b);
  EdgeSegmentIntersector intersects_intersector(intersects_topo);
  esi.process(intersects_intersector);
  EXPECT_TRUE(intersects_intersector.isDone());
  EXPECT_TRUE(intersects_topo.getResult());

  RelateMatrixPredicate matrix_pred;
  InitPredicate(matrix_pred, rel_a, rel_b);
  TopologyComputer matrix_topo(matrix_pred, rel_a, rel_b);
  EdgeSegmentIntersector matrix_intersector(matrix_topo);
  esi.process(matrix_intersector);
  matrix_topo.evaluateNodes();
  matrix_topo.finish();
  std::unique_ptr<IntersectionMatrix> im = matrix_pred.getIM();
  EXPECT_EQ(im->get(Location::INTERIOR, Location::INTERIOR), Dimension::P);
  EXPECT_EQ(im->get(Location::INTERIOR, Location::EXTERIOR), Dimension::L);
  EXPECT_EQ(im->get(Location::EXTERIOR, Location::INTERIOR), Dimension::L);
}

TEST_F(EdgeSetIntersectorTest, InterruptRequestAbortsProcessing) {
  auto geom_a = ReadWkt("LINESTRING (0 0, 2 2)");
  auto geom_b = ReadWkt("LINESTRING (0 2, 2 0)");
  RelateGeometry rel_a(geom_a.get());
  RelateGeometry rel_b(geom_b.get());
  auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
  auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

  {
    RelateMatrixPredicate pred;
    InitPredicate(pred, rel_a, rel_b);
    TopologyComputer topo(pred, rel_a, rel_b);
    EdgeSegmentIntersector intersector(topo);
    EdgeSetIntersector esi(edges_a, edges_b, nullptr);

    Interrupt::request();
    absl::Cleanup cancel_interrupt = [] { Interrupt::cancel(); };
    EXPECT_THROW(esi.process(intersector), GEOSException);
  }

  {
    RelateMatrixPredicate pred;
    InitPredicate(pred, rel_a, rel_b);
    TopologyComputer topo(pred, rel_a, rel_b);
    EdgeSegmentIntersector intersector(topo);
    EdgeSetIntersector esi(edges_a, edges_b, nullptr);

    auto* prev_cb = CurrentThreadInterrupt::registerCallback(
        &AlwaysInterruptCallback, nullptr);
    absl::Cleanup restore_cb = [prev_cb] {
      CurrentThreadInterrupt::registerCallback(prev_cb, nullptr);
    };
    EXPECT_THROW(esi.process(intersector), GEOSException);
  }

  // After cleanup, subsequent processing succeeds cleanly.
  RelateMatrixPredicate pred;
  InitPredicate(pred, rel_a, rel_b);
  TopologyComputer topo(pred, rel_a, rel_b);
  EdgeSegmentIntersector intersector(topo);
  EdgeSetIntersector esi(edges_a, edges_b, nullptr);
  esi.process(intersector);
  topo.evaluateNodes();
  topo.finish();
  EXPECT_EQ(pred.getIM()->get(Location::INTERIOR, Location::INTERIOR),
            Dimension::P);
}

TEST_F(EdgeSetIntersectorTest, PolygonalWithHoleAndAreaAreaCrossings) {
  {
    auto poly_with_hole = ReadWkt(
        "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 8, 8 8, 8 2, 2 2))");
    auto line = ReadWkt("LINESTRING (1 5, 5 5)");

    RelateGeometry rel_a(poly_with_hole.get());
    RelateGeometry rel_b(line.get());
    auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);
    EXPECT_EQ(edges_a.size(), 2u);

    RelateMatrixPredicate pred;
    InitPredicate(pred, rel_a, rel_b);
    TopologyComputer topo(pred, rel_a, rel_b);
    EdgeSegmentIntersector intersector(topo);
    EdgeSetIntersector esi(edges_a, edges_b, nullptr);

    esi.process(intersector);
    topo.evaluateNodes();
    topo.finish();

    std::unique_ptr<IntersectionMatrix> im = pred.getIM();
    EXPECT_EQ(im->get(Location::INTERIOR, Location::INTERIOR), Dimension::L);
    EXPECT_EQ(im->get(Location::BOUNDARY, Location::INTERIOR), Dimension::P);
    EXPECT_EQ(im->get(Location::EXTERIOR, Location::INTERIOR), Dimension::L);
  }

  {
    auto poly_a = ReadWkt("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
    auto poly_b = ReadWkt("POLYGON ((2 2, 6 2, 6 6, 2 6, 2 2))");

    RelateGeometry rel_a(poly_a.get());
    RelateGeometry rel_b(poly_b.get());
    auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

    RelateMatrixPredicate pred;
    InitPredicate(pred, rel_a, rel_b);
    TopologyComputer topo(pred, rel_a, rel_b);
    EdgeSegmentIntersector intersector(topo);
    EdgeSetIntersector esi(edges_a, edges_b, nullptr);

    esi.process(intersector);
    topo.evaluateNodes();
    topo.finish();

    std::unique_ptr<IntersectionMatrix> im = pred.getIM();
    EXPECT_EQ(im->get(Location::INTERIOR, Location::INTERIOR), Dimension::A);
    EXPECT_EQ(im->get(Location::INTERIOR, Location::BOUNDARY), Dimension::L);
    EXPECT_EQ(im->get(Location::BOUNDARY, Location::INTERIOR), Dimension::L);
    EXPECT_EQ(im->get(Location::BOUNDARY, Location::BOUNDARY), Dimension::P);
  }
}

TEST_F(EdgeSetIntersectorTest, NullObjectEnvelopeFiltersAllChains) {
  auto geom_a = ReadWkt("LINESTRING (0 0, 4 4)");
  auto geom_b = ReadWkt("LINESTRING (0 4, 4 0)");
  RelateGeometry rel_a(geom_a.get());
  RelateGeometry rel_b(geom_b.get());
  auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
  auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

  const Envelope empty_env;
  ASSERT_TRUE(empty_env.isNull());

  CountingTopologyPredicate pred(/*stop_after_first=*/false);
  InitPredicate(pred, rel_a, rel_b);
  TopologyComputer topo(pred, rel_a, rel_b);
  EdgeSegmentIntersector intersector(topo);
  EdgeSetIntersector esi(edges_a, edges_b, &empty_env);
  esi.process(intersector);
  topo.evaluateNodes();
  topo.finish();

  EXPECT_EQ(pred.update_count(), 0u);
}

TEST_F(EdgeSetIntersectorTest, ManyChainsAndMultiSegmentMonotoneChains) {
  // 200-segment zigzag produces 200 monotone chains across multiple deque
  // blocks and STRtree levels, each crossing the horizontal segment B.
  {
    std::ostringstream wkt;
    wkt << "LINESTRING (";
    for (int i = 0; i <= 200; ++i) {
      if (i > 0) wkt << ", ";
      wkt << i << ' ' << ((i % 2 == 0) ? 0 : 2);
    }
    wkt << ")";

    auto zigzag = ReadWkt(wkt.str());
    auto horiz = ReadWkt("LINESTRING (0 1, 200 1)");
    RelateGeometry rel_a(zigzag.get());
    RelateGeometry rel_b(horiz.get());
    auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

    CountingTopologyPredicate count_pred(/*stop_after_first=*/false);
    InitPredicate(count_pred, rel_a, rel_b);
    TopologyComputer count_topo(count_pred, rel_a, rel_b);
    EdgeSegmentIntersector count_intersector(count_topo);
    EdgeSetIntersector esi(edges_a, edges_b, nullptr);
    esi.process(count_intersector);
    EXPECT_EQ(count_pred.update_count(), 200u);

    RelateMatrixPredicate matrix_pred;
    InitPredicate(matrix_pred, rel_a, rel_b);
    TopologyComputer matrix_topo(matrix_pred, rel_a, rel_b);
    EdgeSegmentIntersector matrix_intersector(matrix_topo);
    esi.process(matrix_intersector);
    matrix_topo.evaluateNodes();
    matrix_topo.finish();
    std::unique_ptr<IntersectionMatrix> im = matrix_pred.getIM();
    EXPECT_EQ(im->get(Location::INTERIOR, Location::INTERIOR), Dimension::P);
    EXPECT_EQ(im->get(Location::INTERIOR, Location::EXTERIOR), Dimension::L);
    EXPECT_EQ(im->get(Location::EXTERIOR, Location::INTERIOR), Dimension::L);
  }

  // Two multi-segment single monotone chains meeting at a shared interior
  // vertex (2, 3).
  {
    auto geom_a = ReadWkt("LINESTRING (0 0, 1 1, 2 3, 3 6, 4 10)");
    auto geom_b = ReadWkt("LINESTRING (0 10, 1 6, 2 3, 3 1, 4 0)");
    RelateGeometry rel_a(geom_a.get());
    RelateGeometry rel_b(geom_b.get());
    auto edges_a = rel_a.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    auto edges_b = rel_b.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);

    RelateMatrixPredicate pred;
    InitPredicate(pred, rel_a, rel_b);
    TopologyComputer topo(pred, rel_a, rel_b);
    EdgeSegmentIntersector intersector(topo);
    EdgeSetIntersector esi(edges_a, edges_b, nullptr);
    esi.process(intersector);
    topo.evaluateNodes();
    topo.finish();

    std::unique_ptr<IntersectionMatrix> im = pred.getIM();
    EXPECT_EQ(im->get(Location::INTERIOR, Location::INTERIOR), Dimension::P);
    EXPECT_EQ(im->get(Location::INTERIOR, Location::EXTERIOR), Dimension::L);
    EXPECT_EQ(im->get(Location::EXTERIOR, Location::INTERIOR), Dimension::L);
  }
}

}  // namespace
}  // namespace geos_google
