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

#include "third_party/geos/include/geos/index/chain/MonotoneChainOverlapAction.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/LineSegment.h"
#include "third_party/geos/include/geos/index/chain/MonotoneChain.h"
#include "third_party/geos/include/geos/index/chain/MonotoneChainBuilder.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateXY;
using ::geos::geom::CoordinateXYM;
using ::geos::geom::CoordinateXYZM;
using ::geos::geom::LineSegment;
using ::geos::index::chain::MonotoneChain;
using ::geos::index::chain::MonotoneChainBuilder;
using ::geos::index::chain::MonotoneChainOverlapAction;

struct RecordedSegmentPair {
  LineSegment seg1;
  LineSegment seg2;
  bool aliased_to_protected;
};

class RecordingOverlapAction : public MonotoneChainOverlapAction {
 public:
  using MonotoneChainOverlapAction::overlap;

  void overlap(const LineSegment& seg1, const LineSegment& seg2) override {
    records_.push_back(RecordedSegmentPair{
        seg1,
        seg2,
        &seg1 == &overlapSeg1 && &seg2 == &overlapSeg2,
    });
  }

  const std::vector<RecordedSegmentPair>& records() const { return records_; }

  const LineSegment& protected_seg1() const { return overlapSeg1; }
  const LineSegment& protected_seg2() const { return overlapSeg2; }

 private:
  std::vector<RecordedSegmentPair> records_;
};

struct RecordedChainOverlap {
  const MonotoneChain* mc1;
  std::size_t start1;
  void* context1;
  const MonotoneChain* mc2;
  std::size_t start2;
  void* context2;
};

class ChainRecordingOverlapAction : public MonotoneChainOverlapAction {
 public:
  explicit ChainRecordingOverlapAction(bool forward_to_base)
      : forward_to_base_(forward_to_base) {}

  void overlap(const MonotoneChain& mc1, std::size_t start1,
               const MonotoneChain& mc2, std::size_t start2) override {
    chain_records_.push_back(RecordedChainOverlap{
        &mc1,
        start1,
        mc1.getContext(),
        &mc2,
        start2,
        mc2.getContext(),
    });
    if (forward_to_base_) {
      MonotoneChainOverlapAction::overlap(mc1, start1, mc2, start2);
    }
  }

  void overlap(const LineSegment& seg1, const LineSegment& seg2) override {
    segment_records_.push_back(RecordedSegmentPair{
        seg1,
        seg2,
        &seg1 == &overlapSeg1 && &seg2 == &overlapSeg2,
    });
  }

  const std::vector<RecordedChainOverlap>& chain_records() const {
    return chain_records_;
  }
  const std::vector<RecordedSegmentPair>& segment_records() const {
    return segment_records_;
  }

 private:
  bool forward_to_base_;
  std::vector<RecordedChainOverlap> chain_records_;
  std::vector<RecordedSegmentPair> segment_records_;
};

class DestructorTrackingOverlapAction : public MonotoneChainOverlapAction {
 public:
  explicit DestructorTrackingOverlapAction(bool* destroyed)
      : destroyed_(destroyed) {}

  ~DestructorTrackingOverlapAction() override { *destroyed_ = true; }

 private:
  bool* destroyed_;
};

void ExpectSegment2D(const LineSegment& seg, double x0, double y0, double x1,
                     double y1) {
  EXPECT_DOUBLE_EQ(seg.p0.x, x0);
  EXPECT_DOUBLE_EQ(seg.p0.y, y0);
  EXPECT_DOUBLE_EQ(seg.p1.x, x1);
  EXPECT_DOUBLE_EQ(seg.p1.y, y1);
}

void ExpectSegmentZNaN(const LineSegment& seg) {
  EXPECT_TRUE(std::isnan(seg.p0.z));
  EXPECT_TRUE(std::isnan(seg.p1.z));
}

void ExpectSegmentZ(const LineSegment& seg, double z0, double z1) {
  EXPECT_DOUBLE_EQ(seg.p0.z, z0);
  EXPECT_DOUBLE_EQ(seg.p1.z, z1);
}

TEST(MonotoneChainOverlapActionTest,
     BaseClassDefaultOverlapAndPolymorphicDestroy) {
  const CoordinateSequence seq1{CoordinateXY{0.0, 0.0}, CoordinateXY{2.0, 2.0}};
  const CoordinateSequence seq2{CoordinateXY{0.0, 2.0}, CoordinateXY{2.0, 0.0}};
  const MonotoneChain mc1(seq1, 0, 1, nullptr);
  const MonotoneChain mc2(seq2, 0, 1, nullptr);

  auto action = std::make_unique<MonotoneChainOverlapAction>();
  ASSERT_NE(action, nullptr);
  action->overlap(mc1, 0, mc2, 0);
  action->overlap(LineSegment(0.0, 0.0, 1.0, 1.0),
                  LineSegment(0.0, 1.0, 1.0, 0.0));
  action.reset();
  EXPECT_EQ(action, nullptr);

  bool destroyed = false;
  std::unique_ptr<MonotoneChainOverlapAction> derived =
      std::make_unique<DestructorTrackingOverlapAction>(&destroyed);
  ASSERT_NE(derived, nullptr);
  derived->overlap(mc1, 0, mc2, 0);
  EXPECT_FALSE(destroyed);
  derived.reset();
  EXPECT_TRUE(destroyed);
}

TEST(MonotoneChainOverlapActionTest,
     ProtectedOverlapSegmentsDefaultAndUpdatedInPlace) {
  RecordingOverlapAction action;

  ExpectSegment2D(action.protected_seg1(), 0.0, 0.0, 0.0, 0.0);
  ExpectSegmentZNaN(action.protected_seg1());
  ExpectSegment2D(action.protected_seg2(), 0.0, 0.0, 0.0, 0.0);
  ExpectSegmentZNaN(action.protected_seg2());

  const CoordinateSequence seq1{
      CoordinateXY{0.0, 0.0},
      CoordinateXY{1.0, 2.0},
      CoordinateXY{3.0, 4.0},
      CoordinateXY{6.0, 8.0},
  };
  const CoordinateSequence seq2{
      CoordinateXY{10.0, 20.0},
      CoordinateXY{11.0, 22.0},
      CoordinateXY{13.0, 24.0},
  };
  const MonotoneChain mc1(seq1, 0, 3, nullptr);
  const MonotoneChain mc2(seq2, 0, 2, nullptr);

  MonotoneChainOverlapAction& base_ref = action;
  base_ref.overlap(mc1, 0, mc2, 1);
  ExpectSegment2D(action.protected_seg1(), 0.0, 0.0, 1.0, 2.0);
  ExpectSegmentZNaN(action.protected_seg1());
  ExpectSegment2D(action.protected_seg2(), 11.0, 22.0, 13.0, 24.0);
  ExpectSegmentZNaN(action.protected_seg2());

  base_ref.overlap(mc1, 2, mc2, 0);
  ExpectSegment2D(action.protected_seg1(), 3.0, 4.0, 6.0, 8.0);
  ExpectSegment2D(action.protected_seg2(), 10.0, 20.0, 11.0, 22.0);

  // Calling the base 2-arg no-op explicitly leaves state and records unchanged.
  action.MonotoneChainOverlapAction::overlap(LineSegment(1.0, 2.0, 3.0, 4.0),
                                             LineSegment(5.0, 6.0, 7.0, 8.0));

  // Direct 2-arg virtual dispatch does not alias the protected members.
  base_ref.overlap(LineSegment(9.0, 8.0, 7.0, 6.0),
                   LineSegment(5.0, 4.0, 3.0, 2.0));

  ASSERT_EQ(action.records().size(), 3u);
  EXPECT_TRUE(action.records()[0].aliased_to_protected);
  ExpectSegment2D(action.records()[0].seg1, 0.0, 0.0, 1.0, 2.0);
  ExpectSegment2D(action.records()[0].seg2, 11.0, 22.0, 13.0, 24.0);

  EXPECT_TRUE(action.records()[1].aliased_to_protected);
  ExpectSegment2D(action.records()[1].seg1, 3.0, 4.0, 6.0, 8.0);
  ExpectSegment2D(action.records()[1].seg2, 10.0, 20.0, 11.0, 22.0);

  EXPECT_FALSE(action.records()[2].aliased_to_protected);
  ExpectSegment2D(action.records()[2].seg1, 9.0, 8.0, 7.0, 6.0);
  ExpectSegment2D(action.records()[2].seg2, 5.0, 4.0, 3.0, 2.0);

  // Protected members still hold the segments from the last 4-arg call.
  ExpectSegment2D(action.protected_seg1(), 3.0, 4.0, 6.0, 8.0);
  ExpectSegment2D(action.protected_seg2(), 10.0, 20.0, 11.0, 22.0);
}

TEST(MonotoneChainOverlapActionTest,
     PreservesZCoordinatesAcrossCoordinateTypes) {
  const CoordinateSequence seq_xyz{
      Coordinate{0.0, 0.0, 10.5},
      Coordinate{2.0, 2.0, 20.5},
  };
  const CoordinateSequence seq_xyzm{
      CoordinateXYZM{0.0, 2.0, 30.25, 100.0},
      CoordinateXYZM{2.0, 0.0, 40.75, 200.0},
  };
  const CoordinateSequence seq_xym{
      CoordinateXYM{0.0, 0.0, 50.0},
      CoordinateXYM{2.0, 2.0, 60.0},
  };

  const MonotoneChain mc_xyz(seq_xyz, 0, 1, nullptr);
  const MonotoneChain mc_xyzm(seq_xyzm, 0, 1, nullptr);
  const MonotoneChain mc_xym(seq_xym, 0, 1, nullptr);

  RecordingOverlapAction action;
  mc_xyz.computeOverlaps(&mc_xyzm, &action);
  action.overlap(mc_xym, 0, mc_xyz, 0);

  ASSERT_EQ(action.records().size(), 2u);
  ExpectSegment2D(action.records()[0].seg1, 0.0, 0.0, 2.0, 2.0);
  ExpectSegmentZ(action.records()[0].seg1, 10.5, 20.5);
  ExpectSegment2D(action.records()[0].seg2, 0.0, 2.0, 2.0, 0.0);
  ExpectSegmentZ(action.records()[0].seg2, 30.25, 40.75);

  ExpectSegment2D(action.records()[1].seg1, 0.0, 0.0, 2.0, 2.0);
  ExpectSegmentZNaN(action.records()[1].seg1);
  ExpectSegment2D(action.records()[1].seg2, 0.0, 0.0, 2.0, 2.0);
  ExpectSegmentZ(action.records()[1].seg2, 10.5, 20.5);
}

TEST(MonotoneChainOverlapActionTest,
     OverridingFourArgOverlapReceivesChainsIndicesAndContext) {
  int context1 = 101;
  int context2 = 202;
  const CoordinateSequence seq1{
      CoordinateXY{0.0, 0.0},
      CoordinateXY{5.0, 5.0},
      CoordinateXY{10.0, 10.0},
  };
  const CoordinateSequence seq2{
      CoordinateXY{0.0, 10.0},
      CoordinateXY{5.0, 5.0},
      CoordinateXY{10.0, 0.0},
  };
  const MonotoneChain mc1(seq1, 0, 2, &context1);
  const MonotoneChain mc2(seq2, 0, 2, &context2);

  ChainRecordingOverlapAction no_forward(/*forward_to_base=*/false);
  mc1.computeOverlaps(&mc2, &no_forward);

  ASSERT_EQ(no_forward.chain_records().size(), 4u);
  EXPECT_TRUE(no_forward.segment_records().empty());
  for (const auto& rec : no_forward.chain_records()) {
    EXPECT_EQ(rec.mc1, &mc1);
    EXPECT_EQ(rec.mc2, &mc2);
    EXPECT_EQ(rec.context1, &context1);
    EXPECT_EQ(rec.context2, &context2);
  }
  EXPECT_EQ(no_forward.chain_records()[0].start1, 0u);
  EXPECT_EQ(no_forward.chain_records()[0].start2, 0u);
  EXPECT_EQ(no_forward.chain_records()[1].start1, 0u);
  EXPECT_EQ(no_forward.chain_records()[1].start2, 1u);
  EXPECT_EQ(no_forward.chain_records()[2].start1, 1u);
  EXPECT_EQ(no_forward.chain_records()[2].start2, 0u);
  EXPECT_EQ(no_forward.chain_records()[3].start1, 1u);
  EXPECT_EQ(no_forward.chain_records()[3].start2, 1u);

  ChainRecordingOverlapAction with_forward(/*forward_to_base=*/true);
  mc1.computeOverlaps(&mc2, &with_forward);

  ASSERT_EQ(with_forward.chain_records().size(), 4u);
  ASSERT_EQ(with_forward.segment_records().size(), 4u);
  EXPECT_TRUE(with_forward.segment_records()[0].aliased_to_protected);
  ExpectSegment2D(with_forward.segment_records()[0].seg1, 0.0, 0.0, 5.0, 5.0);
  ExpectSegment2D(with_forward.segment_records()[0].seg2, 0.0, 10.0, 5.0, 5.0);
  ExpectSegment2D(with_forward.segment_records()[3].seg1, 5.0, 5.0, 10.0, 10.0);
  ExpectSegment2D(with_forward.segment_records()[3].seg2, 5.0, 5.0, 10.0, 0.0);
}

TEST(MonotoneChainOverlapActionTest,
     ComputeOverlapsCrossingMultiSegmentChains) {
  const CoordinateSequence seq1{
      CoordinateXY{0.0, 0.0}, CoordinateXY{1.0, 1.0}, CoordinateXY{2.0, 2.0},
      CoordinateXY{3.0, 3.0}, CoordinateXY{4.0, 4.0},
  };
  const CoordinateSequence seq2{
      CoordinateXY{0.0, 4.0}, CoordinateXY{1.0, 3.0}, CoordinateXY{2.0, 2.0},
      CoordinateXY{3.0, 1.0}, CoordinateXY{4.0, 0.0},
  };
  const MonotoneChain mc1(seq1, 0, 4, nullptr);
  const MonotoneChain mc2(seq2, 0, 4, nullptr);

  RecordingOverlapAction action;
  mc1.computeOverlaps(&mc2, &action);

  // All 4 depth-1 subchain pairs touch (2,2), and computeOverlaps invokes
  // overlap() directly on 1-segment leaf pairs without re-checking envelopes,
  // emitting all 4 x 4 = 16 pairs in binary-split traversal order.
  ASSERT_EQ(action.records().size(), 16u);
  ExpectSegment2D(action.records()[0].seg1, 0.0, 0.0, 1.0, 1.0);
  ExpectSegment2D(action.records()[0].seg2, 0.0, 4.0, 1.0, 3.0);
  EXPECT_TRUE(
      action.records()[0].seg1.intersection(action.records()[0].seg2).isNull());

  ExpectSegment2D(action.records()[3].seg1, 1.0, 1.0, 2.0, 2.0);
  ExpectSegment2D(action.records()[3].seg2, 1.0, 3.0, 2.0, 2.0);
  ExpectSegment2D(action.records()[6].seg1, 1.0, 1.0, 2.0, 2.0);
  ExpectSegment2D(action.records()[6].seg2, 2.0, 2.0, 3.0, 1.0);
  ExpectSegment2D(action.records()[9].seg1, 2.0, 2.0, 3.0, 3.0);
  ExpectSegment2D(action.records()[9].seg2, 1.0, 3.0, 2.0, 2.0);
  ExpectSegment2D(action.records()[12].seg1, 2.0, 2.0, 3.0, 3.0);
  ExpectSegment2D(action.records()[12].seg2, 2.0, 2.0, 3.0, 1.0);
  ExpectSegment2D(action.records()[15].seg1, 3.0, 3.0, 4.0, 4.0);
  ExpectSegment2D(action.records()[15].seg2, 3.0, 1.0, 4.0, 0.0);

  // Interior segment intersection yields the exact crossing point.
  const Coordinate crossing =
      action.records()[3].seg1.intersection(action.records()[3].seg2);
  EXPECT_DOUBLE_EQ(crossing.x, 2.0);
  EXPECT_DOUBLE_EQ(crossing.y, 2.0);

  // Crossing strictly inside the first half prunes the [2, 4] subchain of mc1.
  const CoordinateSequence seq_off{
      CoordinateXY{0.25, 0.75},
      CoordinateXY{0.5, 0.5},
      CoordinateXY{0.75, 0.25},
  };
  const MonotoneChain mc_off(seq_off, 0, 2, nullptr);
  RecordingOverlapAction pruned_action;
  mc1.computeOverlaps(&mc_off, &pruned_action);

  ASSERT_EQ(pruned_action.records().size(), 4u);
  ExpectSegment2D(pruned_action.records()[0].seg1, 0.0, 0.0, 1.0, 1.0);
  ExpectSegment2D(pruned_action.records()[0].seg2, 0.25, 0.75, 0.5, 0.5);
  ExpectSegment2D(pruned_action.records()[1].seg1, 1.0, 1.0, 2.0, 2.0);
  ExpectSegment2D(pruned_action.records()[1].seg2, 0.25, 0.75, 0.5, 0.5);
  ExpectSegment2D(pruned_action.records()[2].seg1, 0.0, 0.0, 1.0, 1.0);
  ExpectSegment2D(pruned_action.records()[2].seg2, 0.5, 0.5, 0.75, 0.25);
  ExpectSegment2D(pruned_action.records()[3].seg1, 1.0, 1.0, 2.0, 2.0);
  ExpectSegment2D(pruned_action.records()[3].seg2, 0.5, 0.5, 0.75, 0.25);
  const Coordinate off_crossing = pruned_action.records()[0].seg1.intersection(
      pruned_action.records()[0].seg2);
  EXPECT_DOUBLE_EQ(off_crossing.x, 0.5);
  EXPECT_DOUBLE_EQ(off_crossing.y, 0.5);
}

TEST(MonotoneChainOverlapActionTest, ComputeOverlapsDisjointAndWithTolerance) {
  const CoordinateSequence seq1{
      CoordinateXY{0.0, 0.0},  CoordinateXY{5.0, 0.0},  CoordinateXY{10.0, 0.0},
      CoordinateXY{15.0, 0.0}, CoordinateXY{20.0, 0.0},
  };
  const CoordinateSequence seq2{
      CoordinateXY{13.0, 2.0},
      CoordinateXY{15.0, 2.0},
      CoordinateXY{17.0, 2.0},
  };
  const MonotoneChain mc1(seq1, 0, 4, nullptr);
  const MonotoneChain mc2(seq2, 0, 2, nullptr);

  RecordingOverlapAction zero_tol_action;
  mc1.computeOverlaps(&mc2, &zero_tol_action);
  EXPECT_TRUE(zero_tol_action.records().empty());

  RecordingOverlapAction small_tol_action;
  mc1.computeOverlaps(&mc2, 1.9, &small_tol_action);
  EXPECT_TRUE(small_tol_action.records().empty());

  RecordingOverlapAction exact_tol_action;
  mc1.computeOverlaps(&mc2, 2.0, &exact_tol_action);
  // First half [0,10] of mc1 is X-disjoint even with tolerance 2.0; second half
  // [10,20] is tested against segment 0 of mc2 then segment 1 of mc2.
  ASSERT_EQ(exact_tol_action.records().size(), 4u);
  ExpectSegment2D(exact_tol_action.records()[0].seg1, 10.0, 0.0, 15.0, 0.0);
  ExpectSegment2D(exact_tol_action.records()[0].seg2, 13.0, 2.0, 15.0, 2.0);
  ExpectSegment2D(exact_tol_action.records()[1].seg1, 15.0, 0.0, 20.0, 0.0);
  ExpectSegment2D(exact_tol_action.records()[1].seg2, 13.0, 2.0, 15.0, 2.0);
  ExpectSegment2D(exact_tol_action.records()[2].seg1, 10.0, 0.0, 15.0, 0.0);
  ExpectSegment2D(exact_tol_action.records()[2].seg2, 15.0, 2.0, 17.0, 2.0);
  ExpectSegment2D(exact_tol_action.records()[3].seg1, 15.0, 0.0, 20.0, 0.0);
  ExpectSegment2D(exact_tol_action.records()[3].seg2, 15.0, 2.0, 17.0, 2.0);
}

TEST(MonotoneChainOverlapActionTest,
     ComputeOverlapsSelfAndMonotoneChainBuilder) {
  const CoordinateSequence monotone_seq{
      CoordinateXY{0.0, 0.0},
      CoordinateXY{1.0, 1.0},
      CoordinateXY{2.0, 2.0},
      CoordinateXY{3.0, 3.0},
  };
  const MonotoneChain self_chain(monotone_seq, 0, 3, nullptr);
  RecordingOverlapAction self_action;
  self_chain.computeOverlaps(&self_chain, &self_action);

  // Depth-1 subchains [0,1] and [1,3] touch at (1,1), and 1-segment leaves do
  // not re-check envelopes, emitting all 3 x 3 = 9 pairs:
  // (0,0), (0,1), (0,2), (1,0), (2,0), (1,1), (1,2), (2,1), (2,2).
  ASSERT_EQ(self_action.records().size(), 9u);
  ExpectSegment2D(self_action.records()[0].seg1, 0.0, 0.0, 1.0, 1.0);
  ExpectSegment2D(self_action.records()[0].seg2, 0.0, 0.0, 1.0, 1.0);
  ExpectSegment2D(self_action.records()[2].seg1, 0.0, 0.0, 1.0, 1.0);
  ExpectSegment2D(self_action.records()[2].seg2, 2.0, 2.0, 3.0, 3.0);
  ExpectSegment2D(self_action.records()[4].seg1, 2.0, 2.0, 3.0, 3.0);
  ExpectSegment2D(self_action.records()[4].seg2, 0.0, 0.0, 1.0, 1.0);
  ExpectSegment2D(self_action.records()[8].seg1, 2.0, 2.0, 3.0, 3.0);
  ExpectSegment2D(self_action.records()[8].seg2, 2.0, 2.0, 3.0, 3.0);

  // Single-vertex degenerate chain yields zero segment overlaps.
  const CoordinateSequence single_pt{CoordinateXY{1.0, 1.0}};
  std::vector<MonotoneChain> degenerate_chains;
  MonotoneChainBuilder::getChains(&single_pt, nullptr, degenerate_chains);
  ASSERT_EQ(degenerate_chains.size(), 1u);
  RecordingOverlapAction degenerate_action;
  degenerate_chains[0].computeOverlaps(&degenerate_chains[0],
                                       &degenerate_action);
  EXPECT_TRUE(degenerate_action.records().empty());

  // Non-monotone polylines split by MonotoneChainBuilder.
  const CoordinateSequence poly1{
      CoordinateXY{0.0, 0.0}, CoordinateXY{1.0, 1.0}, CoordinateXY{2.0, 2.0},
      CoordinateXY{3.0, 1.0}, CoordinateXY{4.0, 0.0},
  };
  const CoordinateSequence poly2{
      CoordinateXY{2.5, 0.5}, CoordinateXY{3.5, 1.5}, CoordinateXY{4.5, 2.5},
      CoordinateXY{5.5, 1.5}, CoordinateXY{6.5, 0.5},
  };
  std::vector<MonotoneChain> chains1;
  std::vector<MonotoneChain> chains2;
  MonotoneChainBuilder::getChains(&poly1, nullptr, chains1);
  MonotoneChainBuilder::getChains(&poly2, nullptr, chains2);
  ASSERT_EQ(chains1.size(), 2u);
  ASSERT_EQ(chains2.size(), 2u);

  RecordingOverlapAction builder_action;
  for (const auto& c1 : chains1) {
    for (const auto& c2 : chains2) {
      c1.computeOverlaps(&c2, &builder_action);
    }
  }

  ASSERT_EQ(builder_action.records().size(), 4u);
  ExpectSegment2D(builder_action.records()[0].seg1, 2.0, 2.0, 3.0, 1.0);
  ExpectSegment2D(builder_action.records()[0].seg2, 2.5, 0.5, 3.5, 1.5);
  const Coordinate intersection_pt =
      builder_action.records()[0].seg1.intersection(
          builder_action.records()[0].seg2);
  EXPECT_DOUBLE_EQ(intersection_pt.x, 3.0);
  EXPECT_DOUBLE_EQ(intersection_pt.y, 1.0);

  ExpectSegment2D(builder_action.records()[1].seg1, 2.0, 2.0, 3.0, 1.0);
  ExpectSegment2D(builder_action.records()[1].seg2, 3.5, 1.5, 4.5, 2.5);

  ExpectSegment2D(builder_action.records()[2].seg1, 3.0, 1.0, 4.0, 0.0);
  ExpectSegment2D(builder_action.records()[2].seg2, 2.5, 0.5, 3.5, 1.5);
  const Coordinate second_intersection_pt =
      builder_action.records()[2].seg1.intersection(
          builder_action.records()[2].seg2);
  EXPECT_DOUBLE_EQ(second_intersection_pt.x, 3.0);
  EXPECT_DOUBLE_EQ(second_intersection_pt.y, 1.0);

  ExpectSegment2D(builder_action.records()[3].seg1, 3.0, 1.0, 4.0, 0.0);
  ExpectSegment2D(builder_action.records()[3].seg2, 3.5, 1.5, 4.5, 2.5);
}

}  // namespace
}  // namespace geos_google
