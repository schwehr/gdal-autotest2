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
// third_party/geos/src/operation/relateng/EdgeSegmentOverlapAction.cpp.

#include "third_party/geos/include/geos/operation/relateng/EdgeSegmentOverlapAction.h"

#include <cstddef>
#include <memory>
#include <ostream>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/index/chain/MonotoneChain.h"
#include "third_party/geos/include/geos/index/chain/MonotoneChainBuilder.h"
#include "third_party/geos/include/geos/index/chain/MonotoneChainOverlapAction.h"
#include "third_party/geos/include/geos/noding/BasicSegmentString.h"
#include "third_party/geos/include/geos/noding/SegmentIntersectionDetector.h"
#include "third_party/geos/include/geos/noding/SegmentIntersector.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"

namespace geos_google {
namespace {

using geos::algorithm::LineIntersector;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::index::chain::MonotoneChain;
using geos::index::chain::MonotoneChainBuilder;
using geos::index::chain::MonotoneChainOverlapAction;
using geos::noding::BasicSegmentString;
using geos::noding::SegmentIntersectionDetector;
using geos::noding::SegmentIntersector;
using geos::noding::SegmentString;
using geos::operation::relateng::EdgeSegmentOverlapAction;

struct RecordedCall {
  const SegmentString* ss0;
  std::size_t seg_index0;
  const SegmentString* ss1;
  std::size_t seg_index1;

  bool operator==(const RecordedCall& other) const {
    return ss0 == other.ss0 && seg_index0 == other.seg_index0 &&
           ss1 == other.ss1 && seg_index1 == other.seg_index1;
  }
};

std::ostream& operator<<(std::ostream& os, const RecordedCall& call) {
  return os << "RecordedCall{ss0=" << call.ss0
            << ", seg_index0=" << call.seg_index0 << ", ss1=" << call.ss1
            << ", seg_index1=" << call.seg_index1 << "}";
}

class RecordingSegmentIntersector : public SegmentIntersector {
 public:
  void processIntersections(SegmentString* e0, std::size_t segIndex0,
                            SegmentString* e1, std::size_t segIndex1) override {
    calls_.push_back({e0, segIndex0, e1, segIndex1});
  }

  const std::vector<RecordedCall>& calls() const { return calls_; }

 private:
  std::vector<RecordedCall> calls_;
};

TEST(EdgeSegmentOverlapActionTest,
     DirectOverlapForwardsSegmentStringsAndIndices) {
  CoordinateSequence seq1{CoordinateXY{0.0, 0.0}, CoordinateXY{1.0, 1.0},
                          CoordinateXY{2.0, 0.0}, CoordinateXY{3.0, 1.0}};
  CoordinateSequence seq2{CoordinateXY{0.0, 1.0}, CoordinateXY{1.0, 0.0},
                          CoordinateXY{2.0, 1.0}};
  BasicSegmentString ss1(&seq1, nullptr);
  BasicSegmentString ss2(&seq2, nullptr);

  MonotoneChain mc1(seq1, 0, seq1.size() - 1, &ss1);
  MonotoneChain mc2(seq2, 0, seq2.size() - 1, &ss2);

  RecordingSegmentIntersector intersector;
  EdgeSegmentOverlapAction action(intersector);
  MonotoneChainOverlapAction* base_action = &action;
  std::unique_ptr<MonotoneChainOverlapAction> owned_action =
      std::make_unique<EdgeSegmentOverlapAction>(intersector);

  action.overlap(mc1, 1, mc2, 0);
  base_action->overlap(mc1, 2, mc2, 1);
  owned_action->overlap(mc2, 0, mc1, 2);

  const std::vector<RecordedCall> expected{
      {&ss1, 1, &ss2, 0},
      {&ss1, 2, &ss2, 1},
      {&ss2, 0, &ss1, 2},
  };
  EXPECT_EQ(intersector.calls(), expected);
}

TEST(EdgeSegmentOverlapActionTest, PreservesSelfAndNullContextPointers) {
  CoordinateSequence seq{CoordinateXY{0.0, 0.0}, CoordinateXY{2.0, 2.0}};
  BasicSegmentString ss(&seq, nullptr);

  MonotoneChain mc_with_ss(seq, 0, 1, &ss);
  MonotoneChain mc_null(seq, 0, 1, nullptr);

  RecordingSegmentIntersector intersector;
  EdgeSegmentOverlapAction action(intersector);

  action.overlap(mc_with_ss, 0, mc_with_ss, 0);
  action.overlap(mc_null, 0, mc_with_ss, 0);
  action.overlap(mc_with_ss, 0, mc_null, 0);
  action.overlap(mc_null, 0, mc_null, 0);

  const std::vector<RecordedCall> expected{
      {&ss, 0, &ss, 0},
      {nullptr, 0, &ss, 0},
      {&ss, 0, nullptr, 0},
      {nullptr, 0, nullptr, 0},
  };
  EXPECT_EQ(intersector.calls(), expected);
}

TEST(EdgeSegmentOverlapActionTest,
     MonotoneChainComputeOverlapsDispatchesOverlappingPairs) {
  CoordinateSequence seq1{CoordinateXY{0.0, 0.0}, CoordinateXY{1.0, 1.0},
                          CoordinateXY{2.0, 2.0}, CoordinateXY{3.0, 1.0},
                          CoordinateXY{4.0, 0.0}};
  // Flip quadrant at (10.0, 2.0) so the disjoint x=[10, 12] segments form a
  // separate monotone chain whose envelope does not overlap seq1.
  CoordinateSequence seq2{CoordinateXY{0.0, 2.0}, CoordinateXY{1.0, 0.0},
                          CoordinateXY{10.0, 2.0}, CoordinateXY{11.0, 0.0},
                          CoordinateXY{12.0, -2.0}};
  BasicSegmentString ss1(&seq1, nullptr);
  BasicSegmentString ss2(&seq2, nullptr);

  std::vector<MonotoneChain> chains1;
  std::vector<MonotoneChain> chains2;
  MonotoneChainBuilder::getChains(&seq1, &ss1, chains1);
  MonotoneChainBuilder::getChains(&seq2, &ss2, chains2);
  ASSERT_EQ(chains1.size(), 2u);
  ASSERT_EQ(chains2.size(), 3u);

  RecordingSegmentIntersector intersector;
  EdgeSegmentOverlapAction action(intersector);

  for (const MonotoneChain& mc1 : chains1) {
    for (const MonotoneChain& mc2 : chains2) {
      mc1.computeOverlaps(&mc2, &action);
    }
  }

  const std::vector<RecordedCall> expected{
      {&ss1, 0, &ss2, 0}, {&ss1, 1, &ss2, 0}, {&ss1, 0, &ss2, 1},
      {&ss1, 1, &ss2, 1}, {&ss1, 2, &ss2, 1}, {&ss1, 3, &ss2, 1},
  };
  EXPECT_EQ(intersector.calls(), expected);
  for (const RecordedCall& call : intersector.calls()) {
    EXPECT_LT(call.seg_index1, 2u);
  }
}

TEST(EdgeSegmentOverlapActionTest,
     MonotoneChainComputeOverlapsWithToleranceTriggersNearSegments) {
  CoordinateSequence seq1{CoordinateXY{0.0, 0.0}, CoordinateXY{2.0, 0.0}};
  CoordinateSequence seq2{CoordinateXY{0.0, 0.4}, CoordinateXY{2.0, 0.4}};
  BasicSegmentString ss1(&seq1, nullptr);
  BasicSegmentString ss2(&seq2, nullptr);

  MonotoneChain mc1(seq1, 0, 1, &ss1);
  MonotoneChain mc2(seq2, 0, 1, &ss2);

  RecordingSegmentIntersector zero_tol_intersector;
  EdgeSegmentOverlapAction zero_tol_action(zero_tol_intersector);
  mc1.computeOverlaps(&mc2, 0.0, &zero_tol_action);
  // Single-segment chains invoke overlap directly at recursion base case,
  // whereas multi-segment chains prune by envelope before reaching the leaf.
  const std::vector<RecordedCall> expected_single{{&ss1, 0, &ss2, 0}};
  EXPECT_EQ(zero_tol_intersector.calls(), expected_single);

  CoordinateSequence multi1{CoordinateXY{0.0, 0.0}, CoordinateXY{1.0, 0.0},
                            CoordinateXY{2.0, 0.0}};
  CoordinateSequence multi2{CoordinateXY{0.0, 0.4}, CoordinateXY{1.0, 0.4},
                            CoordinateXY{2.0, 0.4}};
  BasicSegmentString multi_ss1(&multi1, nullptr);
  BasicSegmentString multi_ss2(&multi2, nullptr);
  MonotoneChain multi_mc1(multi1, 0, 2, &multi_ss1);
  MonotoneChain multi_mc2(multi2, 0, 2, &multi_ss2);

  RecordingSegmentIntersector strict_intersector;
  EdgeSegmentOverlapAction strict_action(strict_intersector);
  multi_mc1.computeOverlaps(&multi_mc2, 0.0, &strict_action);
  EXPECT_TRUE(strict_intersector.calls().empty());

  RecordingSegmentIntersector tol_intersector;
  EdgeSegmentOverlapAction tol_action(tol_intersector);
  multi_mc1.computeOverlaps(&multi_mc2, 0.5, &tol_action);
  const std::vector<RecordedCall> expected_multi{
      {&multi_ss1, 0, &multi_ss2, 0},
      {&multi_ss1, 0, &multi_ss2, 1},
      {&multi_ss1, 1, &multi_ss2, 0},
      {&multi_ss1, 1, &multi_ss2, 1},
  };
  EXPECT_EQ(tol_intersector.calls(), expected_multi);
}

TEST(EdgeSegmentOverlapActionTest, IntegratesWithSegmentIntersectionDetector) {
  CoordinateSequence seq_a{CoordinateXY{0.0, 0.0}, CoordinateXY{2.0, 2.0},
                           CoordinateXY{4.0, 4.0}};
  CoordinateSequence seq_b{CoordinateXY{0.0, 2.0}, CoordinateXY{2.0, 0.0},
                           CoordinateXY{4.0, -2.0}};
  CoordinateSequence seq_disjoint{CoordinateXY{10.0, 0.0},
                                  CoordinateXY{11.0, 1.0},
                                  CoordinateXY{12.0, 2.0}};
  BasicSegmentString ss_a(&seq_a, nullptr);
  BasicSegmentString ss_b(&seq_b, nullptr);
  BasicSegmentString ss_disjoint(&seq_disjoint, nullptr);

  std::vector<MonotoneChain> chains_a;
  std::vector<MonotoneChain> chains_b;
  std::vector<MonotoneChain> chains_disjoint;
  MonotoneChainBuilder::getChains(&seq_a, &ss_a, chains_a);
  MonotoneChainBuilder::getChains(&seq_b, &ss_b, chains_b);
  MonotoneChainBuilder::getChains(&seq_disjoint, &ss_disjoint, chains_disjoint);
  ASSERT_EQ(chains_a.size(), 1u);
  ASSERT_EQ(chains_b.size(), 1u);
  ASSERT_EQ(chains_disjoint.size(), 1u);

  LineIntersector disjoint_li;
  SegmentIntersectionDetector disjoint_detector(&disjoint_li);
  EdgeSegmentOverlapAction disjoint_action(disjoint_detector);
  chains_a[0].computeOverlaps(&chains_disjoint[0], &disjoint_action);
  EXPECT_FALSE(disjoint_detector.hasIntersection());

  LineIntersector li;
  SegmentIntersectionDetector detector(&li);
  EdgeSegmentOverlapAction action(detector);
  chains_a[0].computeOverlaps(&chains_b[0], &action);

  EXPECT_TRUE(detector.hasIntersection());
  EXPECT_TRUE(detector.hasProperIntersection());
  ASSERT_NE(detector.getIntersection(), nullptr);
  EXPECT_DOUBLE_EQ(detector.getIntersection()->x, 1.0);
  EXPECT_DOUBLE_EQ(detector.getIntersection()->y, 1.0);
  ASSERT_NE(detector.getIntersectionSegments(), nullptr);
  ASSERT_EQ(detector.getIntersectionSegments()->size(), 4u);
  EXPECT_DOUBLE_EQ(detector.getIntersectionSegments()->getX(0), 0.0);
  EXPECT_DOUBLE_EQ(detector.getIntersectionSegments()->getY(0), 0.0);
  EXPECT_DOUBLE_EQ(detector.getIntersectionSegments()->getX(1), 2.0);
  EXPECT_DOUBLE_EQ(detector.getIntersectionSegments()->getY(1), 2.0);
  EXPECT_DOUBLE_EQ(detector.getIntersectionSegments()->getX(2), 0.0);
  EXPECT_DOUBLE_EQ(detector.getIntersectionSegments()->getY(2), 2.0);
  EXPECT_DOUBLE_EQ(detector.getIntersectionSegments()->getX(3), 2.0);
  EXPECT_DOUBLE_EQ(detector.getIntersectionSegments()->getY(3), 0.0);
}

}  // namespace
}  // namespace geos_google
