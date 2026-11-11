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

// Unit tests for third_party/geos/src/index/chain/MonotoneChain.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/index/chain/MonotoneChain.cpp

#include "third_party/geos/include/geos/index/chain/MonotoneChain.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/LineSegment.h"
#include "third_party/geos/include/geos/index/chain/MonotoneChainOverlapAction.h"
#include "third_party/geos/include/geos/index/chain/MonotoneChainSelectAction.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateXY;
using ::geos::geom::Envelope;
using ::geos::geom::LineSegment;
using ::geos::index::chain::MonotoneChain;
using ::geos::index::chain::MonotoneChainOverlapAction;
using ::geos::index::chain::MonotoneChainSelectAction;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Pair;

class RecordingSelectAction : public MonotoneChainSelectAction {
 public:
  void select(const MonotoneChain& mc, std::size_t start) override {
    selected_indices.emplace_back(&mc, start);
    MonotoneChainSelectAction::select(mc, start);
  }

  void select(const LineSegment& seg) override { segments.push_back(seg); }

  std::vector<std::pair<const MonotoneChain*, std::size_t>> selected_indices;
  std::vector<LineSegment> segments;
};

class RecordingOverlapAction : public MonotoneChainOverlapAction {
 public:
  struct Entry {
    std::size_t start1;
    std::size_t start2;
    LineSegment seg1;
    LineSegment seg2;
  };

  void overlap(const MonotoneChain& mc1, std::size_t start1,
               const MonotoneChain& mc2, std::size_t start2) override {
    pending_start1_ = start1;
    pending_start2_ = start2;
    MonotoneChainOverlapAction::overlap(mc1, start1, mc2, start2);
  }

  void overlap(const LineSegment& seg1, const LineSegment& seg2) override {
    entries.push_back({pending_start1_, pending_start2_, seg1, seg2});
  }

  std::vector<Entry> entries;

 private:
  std::size_t pending_start1_ = 0;
  std::size_t pending_start2_ = 0;
};

std::vector<std::pair<std::size_t, std::size_t>> IndexPairs(
    const RecordingOverlapAction& action) {
  std::vector<std::pair<std::size_t, std::size_t>> pairs;
  pairs.reserve(action.entries.size());
  for (const auto& entry : action.entries) {
    pairs.emplace_back(entry.start1, entry.start2);
  }
  return pairs;
}

TEST(MonotoneChainTest, AccessorsAndLineSegmentExtraction) {
  CoordinateSequence xy_seq{
      CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0), CoordinateXY(2.0, 3.0),
      CoordinateXY(4.0, 5.0), CoordinateXY(6.0, 8.0),
  };
  int context_tag = 7;
  MonotoneChain chain(xy_seq, 1, 3, &context_tag);

  EXPECT_EQ(chain.getStartIndex(), 1u);
  EXPECT_EQ(chain.getEndIndex(), 3u);
  EXPECT_EQ(chain.getContext(), &context_tag);

  LineSegment seg;
  chain.getLineSegment(1, seg);
  EXPECT_EQ(seg.p0, CoordinateXY(1.0, 1.0));
  EXPECT_EQ(seg.p1, CoordinateXY(2.0, 3.0));
  EXPECT_TRUE(std::isnan(seg.p0.z));
  EXPECT_TRUE(std::isnan(seg.p1.z));

  chain.getLineSegment(2, seg);
  EXPECT_EQ(seg.p0, CoordinateXY(2.0, 3.0));
  EXPECT_EQ(seg.p1, CoordinateXY(4.0, 5.0));

  CoordinateSequence xyz_seq{
      Coordinate(1.0, 2.0, 10.0),
      Coordinate(3.0, 4.0, 20.0),
  };
  MonotoneChain xyz_chain(xyz_seq, 0, 1, nullptr);
  EXPECT_EQ(xyz_chain.getContext(), nullptr);
  xyz_chain.getLineSegment(0, seg);
  EXPECT_TRUE(seg.p0.equals3D(Coordinate(1.0, 2.0, 10.0)));
  EXPECT_TRUE(seg.p1.equals3D(Coordinate(3.0, 4.0, 20.0)));
}

TEST(MonotoneChainTest, GetCoordinatesReturnsIndependentClone) {
  CoordinateSequence seq{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(2.0, 1.0),
      CoordinateXY(4.0, 3.0),
  };
  MonotoneChain chain(seq, 1, 2, nullptr);

  std::unique_ptr<CoordinateSequence> copy = chain.getCoordinates();
  ASSERT_NE(copy, nullptr);
  EXPECT_TRUE(CoordinateSequence::equals(copy.get(), &seq));

  copy->setOrdinate(0, CoordinateSequence::X, 99.0);
  EXPECT_DOUBLE_EQ(seq.getX(0), 0.0);
  EXPECT_DOUBLE_EQ(chain.getEnvelope().getMinX(), 2.0);
}

TEST(MonotoneChainTest, GetEnvelopeCachesUnexpandedAndExpandedBounds) {
  CoordinateSequence sw_seq{
      CoordinateXY(10.0, 8.0),
      CoordinateXY(7.0, 5.0),
      CoordinateXY(3.0, 1.0),
  };

  MonotoneChain unexpanded(sw_seq, 0, 2, nullptr);
  const Envelope& env1 = unexpanded.getEnvelope();
  EXPECT_DOUBLE_EQ(env1.getMinX(), 3.0);
  EXPECT_DOUBLE_EQ(env1.getMaxX(), 10.0);
  EXPECT_DOUBLE_EQ(env1.getMinY(), 1.0);
  EXPECT_DOUBLE_EQ(env1.getMaxY(), 8.0);
  // Second call returns the cached envelope without recomputation.
  const Envelope& env2 = unexpanded.getEnvelope();
  EXPECT_EQ(&env2, &env1);
  const Envelope& env3 = unexpanded.getEnvelope(5.0);
  EXPECT_EQ(&env3, &env1);

  MonotoneChain expanded(sw_seq, 0, 2, nullptr);
  const Envelope& exp1 = expanded.getEnvelope(2.5);
  EXPECT_DOUBLE_EQ(exp1.getMinX(), 0.5);
  EXPECT_DOUBLE_EQ(exp1.getMaxX(), 12.5);
  EXPECT_DOUBLE_EQ(exp1.getMinY(), -1.5);
  EXPECT_DOUBLE_EQ(exp1.getMaxY(), 10.5);
  // Once cached, subsequent calls return the cached expanded envelope.
  const Envelope& exp2 = expanded.getEnvelope(100.0);
  EXPECT_EQ(&exp2, &exp1);

  MonotoneChain non_positive(sw_seq, 0, 2, nullptr);
  const Envelope& env_neg = non_positive.getEnvelope(-1.0);
  EXPECT_DOUBLE_EQ(env_neg.getMinX(), 3.0);
  EXPECT_DOUBLE_EQ(env_neg.getMaxX(), 10.0);
  EXPECT_DOUBLE_EQ(env_neg.getMinY(), 1.0);
  EXPECT_DOUBLE_EQ(env_neg.getMaxY(), 8.0);

  MonotoneChain single_pt(sw_seq, 1, 1, nullptr);
  const Envelope& pt_env = single_pt.getEnvelope();
  EXPECT_DOUBLE_EQ(pt_env.getMinX(), 7.0);
  EXPECT_DOUBLE_EQ(pt_env.getMaxX(), 7.0);
  EXPECT_DOUBLE_EQ(pt_env.getMinY(), 5.0);
  EXPECT_DOUBLE_EQ(pt_env.getMaxY(), 5.0);

  MonotoneChain single_pt_exp(sw_seq, 1, 1, nullptr);
  const Envelope& pt_exp = single_pt_exp.getEnvelope(2.0);
  EXPECT_DOUBLE_EQ(pt_exp.getMinX(), 5.0);
  EXPECT_DOUBLE_EQ(pt_exp.getMaxX(), 9.0);
  EXPECT_DOUBLE_EQ(pt_exp.getMinY(), 3.0);
  EXPECT_DOUBLE_EQ(pt_exp.getMaxY(), 7.0);
}

TEST(MonotoneChainTest, SelectSingleSegmentChainAlwaysInvokesAction) {
  CoordinateSequence seq{CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0)};
  MonotoneChain chain(seq, 0, 1, nullptr);

  RecordingSelectAction action;
  Envelope disjoint(100.0, 200.0, 100.0, 200.0);
  chain.select(disjoint, action);

  EXPECT_THAT(action.selected_indices, ElementsAre(Pair(&chain, 0u)));
  ASSERT_EQ(action.segments.size(), 1u);
  EXPECT_EQ(action.segments[0], LineSegment(0.0, 0.0, 1.0, 1.0));

  RecordingSelectAction null_env_action;
  chain.select(Envelope(), null_env_action);
  EXPECT_THAT(null_env_action.selected_indices, ElementsAre(Pair(&chain, 0u)));
}

TEST(MonotoneChainTest, SelectDegenerateZeroSegmentChainEmitsNothing) {
  CoordinateSequence seq{CoordinateXY(5.0, 5.0), CoordinateXY(6.0, 6.0)};
  MonotoneChain chain(seq, 0, 0, nullptr);

  RecordingSelectAction disjoint_action;
  chain.select(Envelope(10.0, 20.0, 10.0, 20.0), disjoint_action);
  EXPECT_THAT(disjoint_action.selected_indices, IsEmpty());

  RecordingSelectAction intersecting_action;
  chain.select(Envelope(4.0, 6.0, 4.0, 6.0), intersecting_action);
  EXPECT_THAT(intersecting_action.selected_indices, IsEmpty());
}

TEST(MonotoneChainTest, SelectMultiSegmentChainPrunesAndBinarySplits) {
  CoordinateSequence seq{
      CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0), CoordinateXY(2.0, 2.0),
      CoordinateXY(3.0, 3.0), CoordinateXY(4.0, 4.0), CoordinateXY(5.0, 5.0),
      CoordinateXY(6.0, 6.0), CoordinateXY(7.0, 7.0), CoordinateXY(8.0, 8.0),
  };
  MonotoneChain full_chain(seq, 0, 8, nullptr);

  RecordingSelectAction disjoint_action;
  full_chain.select(Envelope(100.0, 110.0, 100.0, 110.0), disjoint_action);
  EXPECT_THAT(disjoint_action.selected_indices, IsEmpty());

  RecordingSelectAction null_env_action;
  full_chain.select(Envelope(), null_env_action);
  EXPECT_THAT(null_env_action.selected_indices, IsEmpty());

  // Intersects only subchain [0, 2], pruning [2, 4] and [4, 8].
  RecordingSelectAction partial_action;
  full_chain.select(Envelope(1.2, 1.8, 1.2, 1.8), partial_action);
  EXPECT_THAT(partial_action.selected_indices,
              ElementsAre(Pair(&full_chain, 0u), Pair(&full_chain, 1u)));

  // Intersects the entire chain.
  RecordingSelectAction full_action;
  full_chain.select(Envelope(-1.0, 10.0, -1.0, 10.0), full_action);
  EXPECT_THAT(full_action.selected_indices,
              ElementsAre(Pair(&full_chain, 0u), Pair(&full_chain, 1u),
                          Pair(&full_chain, 2u), Pair(&full_chain, 3u),
                          Pair(&full_chain, 4u), Pair(&full_chain, 5u),
                          Pair(&full_chain, 6u), Pair(&full_chain, 7u)));

  // Sub-slices going SE (increasing x, decreasing y).
  CoordinateSequence se_seq{
      CoordinateXY(-5.0, 20.0), CoordinateXY(0.0, 10.0), CoordinateXY(2.0, 8.0),
      CoordinateXY(4.0, 6.0),   CoordinateXY(6.0, 4.0),  CoordinateXY(9.0, 0.0),
  };
  // 3-segment sub-slice [1, 4] splits into 1-segment leaf [1, 2] (always
  // emitted because leaf termination precedes the envelope check) and [2, 4].
  MonotoneChain se_slice_3(se_seq, 1, 4, nullptr);
  RecordingSelectAction se_action_3;
  se_slice_3.select(Envelope(3.5, 5.0, 5.0, 7.0), se_action_3);
  EXPECT_THAT(se_action_3.selected_indices,
              ElementsAre(Pair(&se_slice_3, 1u), Pair(&se_slice_3, 2u),
                          Pair(&se_slice_3, 3u)));

  // 4-segment sub-slice [1, 5] splits into [1, 3] (pruned) and [3, 5] -> 3, 4.
  MonotoneChain se_slice_4(se_seq, 1, 5, nullptr);
  RecordingSelectAction se_action_4;
  se_slice_4.select(Envelope(4.5, 5.5, 4.5, 5.5), se_action_4);
  EXPECT_THAT(se_action_4.selected_indices,
              ElementsAre(Pair(&se_slice_4, 3u), Pair(&se_slice_4, 4u)));
}

TEST(MonotoneChainTest, ComputeOverlapsZeroToleranceAcrossChainShapes) {
  // 1-segment vs 1-segment directly triggers overlap without bounding check,
  // even when envelopes are disjoint.
  CoordinateSequence seg_a_seq{CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 2.0)};
  CoordinateSequence seg_b_seq{CoordinateXY(0.0, 2.0), CoordinateXY(2.0, 0.0)};
  CoordinateSequence far_seg_seq{CoordinateXY(100.0, 100.0),
                                 CoordinateXY(101.0, 101.0)};
  MonotoneChain seg_a(seg_a_seq, 0, 1, nullptr);
  MonotoneChain seg_b(seg_b_seq, 0, 1, nullptr);
  MonotoneChain far_seg(far_seg_seq, 0, 1, nullptr);

  RecordingOverlapAction single_vs_single;
  seg_a.computeOverlaps(&seg_b, &single_vs_single);
  ASSERT_EQ(single_vs_single.entries.size(), 1u);
  EXPECT_EQ(single_vs_single.entries[0].start1, 0u);
  EXPECT_EQ(single_vs_single.entries[0].start2, 0u);
  EXPECT_EQ(single_vs_single.entries[0].seg1, LineSegment(0.0, 0.0, 2.0, 2.0));
  EXPECT_EQ(single_vs_single.entries[0].seg2, LineSegment(0.0, 2.0, 2.0, 0.0));

  RecordingOverlapAction disjoint_1v1;
  seg_a.computeOverlaps(&far_seg, &disjoint_1v1);
  EXPECT_THAT(IndexPairs(disjoint_1v1), ElementsAre(Pair(0u, 0u)));

  // Multi-segment chain vs 1-segment chain, and reversed argument order.
  CoordinateSequence multi_seq{
      CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0), CoordinateXY(2.0, 2.0),
      CoordinateXY(3.0, 3.0), CoordinateXY(4.0, 4.0),
  };
  CoordinateSequence probe_seq{CoordinateXY(2.2, 2.8), CoordinateXY(2.8, 2.2)};
  MonotoneChain multi(multi_seq, 0, 4, nullptr);
  MonotoneChain probe(probe_seq, 0, 1, nullptr);

  // [0, 4] splits to [0, 2] (pruned) and [2, 4] -> leaves (2, 0) and (3, 0).
  RecordingOverlapAction multi_vs_one;
  multi.computeOverlaps(&probe, &multi_vs_one);
  EXPECT_THAT(IndexPairs(multi_vs_one),
              ElementsAre(Pair(2u, 0u), Pair(3u, 0u)));

  RecordingOverlapAction one_vs_multi;
  probe.computeOverlaps(&multi, &one_vs_multi);
  EXPECT_THAT(IndexPairs(one_vs_multi),
              ElementsAre(Pair(0u, 2u), Pair(0u, 3u)));

  // 3-segment chain [0, 3] vs 1-segment probe near [0, 1] prunes [1, 3].
  CoordinateSequence low_probe_seq{CoordinateXY(0.2, 0.8),
                                   CoordinateXY(0.8, 0.2)};
  MonotoneChain tri(multi_seq, 0, 3, nullptr);
  MonotoneChain low_probe(low_probe_seq, 0, 1, nullptr);

  RecordingOverlapAction tri_vs_one;
  tri.computeOverlaps(&low_probe, &tri_vs_one);
  EXPECT_THAT(IndexPairs(tri_vs_one), ElementsAre(Pair(0u, 0u)));

  RecordingOverlapAction one_vs_tri;
  low_probe.computeOverlaps(&tri, &one_vs_tri);
  EXPECT_THAT(IndexPairs(one_vs_tri), ElementsAre(Pair(0u, 0u)));

  // Two multi-segment chains overlapping in the interior and touching at
  // subchain split boundaries.
  CoordinateSequence cross_seq{
      CoordinateXY(0.0, 5.0), CoordinateXY(1.0, 4.0), CoordinateXY(2.0, 3.0),
      CoordinateXY(3.0, 2.0), CoordinateXY(4.0, 1.0), CoordinateXY(5.0, 0.0),
  };
  MonotoneChain cross(cross_seq, 0, 5, nullptr);

  RecordingOverlapAction multi_vs_multi;
  multi.computeOverlaps(&cross, &multi_vs_multi);
  EXPECT_THAT(
      IndexPairs(multi_vs_multi),
      ElementsAre(Pair(0u, 2u), Pair(1u, 2u), Pair(2u, 0u), Pair(2u, 1u),
                  Pair(3u, 0u), Pair(3u, 1u), Pair(2u, 2u), Pair(2u, 3u),
                  Pair(2u, 4u), Pair(3u, 2u)));

  // Disjoint multi-segment chains prune immediately.
  CoordinateSequence far_seq{
      CoordinateXY(50.0, 50.0),
      CoordinateXY(51.0, 51.0),
      CoordinateXY(52.0, 52.0),
  };
  MonotoneChain far_chain(far_seq, 0, 2, nullptr);
  RecordingOverlapAction disjoint_overlaps;
  multi.computeOverlaps(&far_chain, &disjoint_overlaps);
  EXPECT_THAT(disjoint_overlaps.entries, IsEmpty());

  // Self-overlap on a 4-segment chain touches at midpoint (2, 2), visiting all
  // 16 quadrant pairs; an 8-segment chain prunes 24 distant subchain pairs.
  RecordingOverlapAction self_overlaps_4;
  multi.computeOverlaps(&multi, &self_overlaps_4);
  EXPECT_EQ(self_overlaps_4.entries.size(), 16u);

  CoordinateSequence long_seq{
      CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0), CoordinateXY(2.0, 2.0),
      CoordinateXY(3.0, 3.0), CoordinateXY(4.0, 4.0), CoordinateXY(5.0, 5.0),
      CoordinateXY(6.0, 6.0), CoordinateXY(7.0, 7.0), CoordinateXY(8.0, 8.0),
  };
  MonotoneChain long_chain(long_seq, 0, 8, nullptr);
  RecordingOverlapAction self_overlaps_8;
  long_chain.computeOverlaps(&long_chain, &self_overlaps_8);
  const std::vector<std::pair<std::size_t, std::size_t>> expected_long_pairs = {
      {0, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3},
      {2, 0}, {2, 1}, {3, 0}, {3, 1}, {2, 2}, {2, 3}, {3, 2}, {3, 3},
      {2, 4}, {2, 5}, {3, 4}, {3, 5}, {4, 2}, {4, 3}, {5, 2}, {5, 3},
      {4, 4}, {4, 5}, {5, 4}, {5, 5}, {4, 6}, {4, 7}, {5, 6}, {5, 7},
      {6, 4}, {6, 5}, {7, 4}, {7, 5}, {6, 6}, {6, 7}, {7, 6}, {7, 7},
  };
  EXPECT_EQ(IndexPairs(self_overlaps_8), expected_long_pairs);

  // Zero-segment degenerate chain overlapping or disjoint emits nothing.
  MonotoneChain point_chain(multi_seq, 2, 2, nullptr);
  RecordingOverlapAction deg_a;
  point_chain.computeOverlaps(&multi, &deg_a);
  EXPECT_THAT(deg_a.entries, IsEmpty());

  RecordingOverlapAction deg_b;
  multi.computeOverlaps(&point_chain, &deg_b);
  EXPECT_THAT(deg_b.entries, IsEmpty());

  RecordingOverlapAction deg_disjoint;
  point_chain.computeOverlaps(&far_chain, &deg_disjoint);
  EXPECT_THAT(deg_disjoint.entries, IsEmpty());

  RecordingOverlapAction deg_self;
  point_chain.computeOverlaps(&point_chain, &deg_self);
  EXPECT_THAT(deg_self.entries, IsEmpty());
}

TEST(MonotoneChainTest, ComputeOverlapsWithToleranceChecksAllBounds) {
  CoordinateSequence base_seq{
      CoordinateXY(10.0, 10.0),
      CoordinateXY(15.0, 15.0),
      CoordinateXY(20.0, 20.0),
  };
  MonotoneChain base(base_seq, 0, 2, nullptr);
  constexpr double kTol = 1.0;

  // 1) Disjoint on +X beyond tolerance: minp_x (22) > maxq_x + 1 (21).
  CoordinateSequence right_seq{
      CoordinateXY(22.0, 12.0),
      CoordinateXY(24.0, 15.0),
      CoordinateXY(26.0, 18.0),
  };
  MonotoneChain right_chain(right_seq, 0, 2, nullptr);
  RecordingOverlapAction act_right;
  right_chain.computeOverlaps(&base, kTol, &act_right);
  EXPECT_THAT(act_right.entries, IsEmpty());

  // 2) Disjoint on -X beyond tolerance: maxp_x (8) < minq_x - 1 (9).
  CoordinateSequence left_seq{
      CoordinateXY(2.0, 12.0),
      CoordinateXY(5.0, 15.0),
      CoordinateXY(8.0, 18.0),
  };
  MonotoneChain left_chain(left_seq, 0, 2, nullptr);
  RecordingOverlapAction act_left;
  left_chain.computeOverlaps(&base, kTol, &act_left);
  EXPECT_THAT(act_left.entries, IsEmpty());

  // 3) X overlaps, disjoint on +Y beyond tolerance: minp_y (22) > maxq_y + 1.
  CoordinateSequence above_seq{
      CoordinateXY(12.0, 22.0),
      CoordinateXY(15.0, 24.0),
      CoordinateXY(18.0, 26.0),
  };
  MonotoneChain above_chain(above_seq, 0, 2, nullptr);
  RecordingOverlapAction act_above;
  above_chain.computeOverlaps(&base, kTol, &act_above);
  EXPECT_THAT(act_above.entries, IsEmpty());

  // 4) X overlaps, disjoint on -Y beyond tolerance (reversed coords).
  CoordinateSequence below_seq{
      CoordinateXY(18.0, 8.0),
      CoordinateXY(15.0, 5.0),
      CoordinateXY(12.0, 2.0),
  };
  MonotoneChain below_chain(below_seq, 0, 2, nullptr);
  RecordingOverlapAction act_below;
  below_chain.computeOverlaps(&base, kTol, &act_below);
  EXPECT_THAT(act_below.entries, IsEmpty());

  // 5) Separated by 0.5 in X and Y: disjoint at tolerance <= 0, overlaps at 1.
  CoordinateSequence near_seq{
      CoordinateXY(20.5, 20.5),
      CoordinateXY(22.0, 22.0),
      CoordinateXY(25.0, 25.0),
  };
  MonotoneChain near_chain(near_seq, 0, 2, nullptr);

  RecordingOverlapAction act_zero_tol;
  near_chain.computeOverlaps(&base, 0.0, &act_zero_tol);
  EXPECT_THAT(act_zero_tol.entries, IsEmpty());

  RecordingOverlapAction act_neg_tol;
  near_chain.computeOverlaps(&base, -2.0, &act_neg_tol);
  EXPECT_THAT(act_neg_tol.entries, IsEmpty());

  RecordingOverlapAction act_near;
  near_chain.computeOverlaps(&base, kTol, &act_near);
  EXPECT_THAT(IndexPairs(act_near), ElementsAre(Pair(0u, 0u), Pair(0u, 1u),
                                                Pair(1u, 0u), Pair(1u, 1u)));

  // 6) 4-segment chains prune distant halves before emitting near quadrants.
  CoordinateSequence base4_seq{
      CoordinateXY(0.0, 0.0),   CoordinateXY(5.0, 5.0),
      CoordinateXY(10.0, 10.0), CoordinateXY(15.0, 15.0),
      CoordinateXY(20.0, 20.0),
  };
  CoordinateSequence near4_seq{
      CoordinateXY(20.5, 20.5), CoordinateXY(22.0, 22.0),
      CoordinateXY(25.0, 25.0), CoordinateXY(27.0, 27.0),
      CoordinateXY(30.0, 30.0),
  };
  MonotoneChain base4(base4_seq, 0, 4, nullptr);
  MonotoneChain near4(near4_seq, 0, 4, nullptr);
  RecordingOverlapAction act_near4;
  near4.computeOverlaps(&base4, kTol, &act_near4);
  EXPECT_THAT(IndexPairs(act_near4), ElementsAre(Pair(0u, 2u), Pair(0u, 3u),
                                                 Pair(1u, 2u), Pair(1u, 3u)));

  // 7) Exact boundary equality with reversed coordinates on q.
  CoordinateSequence q_rev_seq{
      CoordinateXY(20.0, 20.0),
      CoordinateXY(15.0, 15.0),
      CoordinateXY(10.0, 10.0),
  };
  MonotoneChain q_rev(q_rev_seq, 0, 2, nullptr);

  CoordinateSequence p_right_touch_seq{
      CoordinateXY(21.0, 21.0),
      CoordinateXY(22.0, 22.0),
      CoordinateXY(23.0, 23.0),
  };
  MonotoneChain p_right_touch(p_right_touch_seq, 0, 2, nullptr);
  RecordingOverlapAction act_right_touch;
  p_right_touch.computeOverlaps(&q_rev, kTol, &act_right_touch);
  EXPECT_EQ(act_right_touch.entries.size(), 4u);

  CoordinateSequence p_right_miss_seq{
      CoordinateXY(21.01, 21.01),
      CoordinateXY(22.0, 22.0),
      CoordinateXY(23.0, 23.0),
  };
  MonotoneChain p_right_miss(p_right_miss_seq, 0, 2, nullptr);
  RecordingOverlapAction act_right_miss;
  p_right_miss.computeOverlaps(&q_rev, kTol, &act_right_miss);
  EXPECT_THAT(act_right_miss.entries, IsEmpty());

  CoordinateSequence p_left_touch_seq{
      CoordinateXY(7.0, 12.0),
      CoordinateXY(8.0, 15.0),
      CoordinateXY(9.0, 18.0),
  };
  MonotoneChain p_left_touch(p_left_touch_seq, 0, 2, nullptr);
  RecordingOverlapAction act_left_touch;
  p_left_touch.computeOverlaps(&q_rev, kTol, &act_left_touch);
  EXPECT_EQ(act_left_touch.entries.size(), 4u);

  CoordinateSequence p_below_touch_seq{
      CoordinateXY(12.0, 7.0),
      CoordinateXY(15.0, 8.0),
      CoordinateXY(18.0, 9.0),
  };
  MonotoneChain p_below_touch(p_below_touch_seq, 0, 2, nullptr);
  RecordingOverlapAction act_below_touch;
  p_below_touch.computeOverlaps(&q_rev, kTol, &act_below_touch);
  EXPECT_EQ(act_below_touch.entries.size(), 4u);

  CoordinateSequence p_above_touch_seq{
      CoordinateXY(12.0, 21.0),
      CoordinateXY(15.0, 22.0),
      CoordinateXY(18.0, 23.0),
  };
  MonotoneChain p_above_touch(p_above_touch_seq, 0, 2, nullptr);
  RecordingOverlapAction act_above_touch;
  p_above_touch.computeOverlaps(&q_rev, kTol, &act_above_touch);
  EXPECT_EQ(act_above_touch.entries.size(), 4u);
}

}  // namespace
}  // namespace geos_google
