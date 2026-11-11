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

#include "third_party/geos/include/geos/index/chain/MonotoneChainSelectAction.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
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
using ::geos::geom::Envelope;
using ::geos::geom::LineSegment;
using ::geos::index::chain::MonotoneChain;
using ::geos::index::chain::MonotoneChainBuilder;
using ::geos::index::chain::MonotoneChainSelectAction;

struct RecordedSelection {
  LineSegment seg;
  bool aliased_to_protected;
};

class RecordingSelectAction : public MonotoneChainSelectAction {
 public:
  using MonotoneChainSelectAction::select;

  void select(const LineSegment& seg) override {
    records_.push_back(RecordedSelection{seg, &seg == &selectedSegment});
  }

  const std::vector<RecordedSelection>& records() const { return records_; }

  const LineSegment& protected_selected_segment() const {
    return selectedSegment;
  }

 private:
  std::vector<RecordedSelection> records_;
};

struct RecordedChainSelection {
  const MonotoneChain* mc;
  std::size_t start;
  void* context;
};

class ChainRecordingSelectAction : public MonotoneChainSelectAction {
 public:
  explicit ChainRecordingSelectAction(bool forward_to_base)
      : forward_to_base_(forward_to_base) {}

  void select(const MonotoneChain& mc, std::size_t start) override {
    chain_records_.push_back(
        RecordedChainSelection{&mc, start, mc.getContext()});
    if (forward_to_base_) {
      MonotoneChainSelectAction::select(mc, start);
    }
  }

  void select(const LineSegment& seg) override {
    segment_records_.push_back(
        RecordedSelection{seg, &seg == &selectedSegment});
  }

  const std::vector<RecordedChainSelection>& chain_records() const {
    return chain_records_;
  }
  const std::vector<RecordedSelection>& segment_records() const {
    return segment_records_;
  }

 private:
  bool forward_to_base_;
  std::vector<RecordedChainSelection> chain_records_;
  std::vector<RecordedSelection> segment_records_;
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

TEST(MonotoneChainSelectActionTest,
     DefaultProtectedSegmentAndPolymorphicDestroy) {
  auto action = std::make_unique<RecordingSelectAction>();
  RecordingSelectAction* raw_action = action.get();
  ExpectSegment2D(action->protected_selected_segment(), 0.0, 0.0, 0.0, 0.0);
  ExpectSegmentZNaN(action->protected_selected_segment());

  std::unique_ptr<MonotoneChainSelectAction> base_ptr = std::move(action);
  base_ptr->select(LineSegment(1.0, 2.0, 3.0, 4.0));
  ASSERT_EQ(raw_action->records().size(), 1u);
  EXPECT_FALSE(raw_action->records()[0].aliased_to_protected);
  ExpectSegment2D(raw_action->records()[0].seg, 1.0, 2.0, 3.0, 4.0);
  base_ptr.reset();
  EXPECT_EQ(base_ptr, nullptr);
}

TEST(MonotoneChainSelectActionTest,
     DirectSelectPopulatesSelectedSegmentAcrossIndices) {
  const CoordinateSequence seq{
      CoordinateXY{0.0, 0.0},
      CoordinateXY{1.0, 2.0},
      CoordinateXY{3.0, 5.0},
      CoordinateXY{6.0, 9.0},
  };
  const MonotoneChain mc(seq, 0, 3, nullptr);

  RecordingSelectAction action;
  MonotoneChainSelectAction& base_ref = action;

  base_ref.select(mc, 0);
  ExpectSegment2D(action.protected_selected_segment(), 0.0, 0.0, 1.0, 2.0);
  ExpectSegmentZNaN(action.protected_selected_segment());

  base_ref.select(mc, 2);
  ExpectSegment2D(action.protected_selected_segment(), 3.0, 5.0, 6.0, 9.0);

  base_ref.select(mc, 1);
  ExpectSegment2D(action.protected_selected_segment(), 1.0, 2.0, 3.0, 5.0);

  // Direct 1-arg virtual dispatch passes an external segment rather than the
  // protected selectedSegment member.
  base_ref.select(LineSegment(9.0, 8.0, 7.0, 6.0));

  ASSERT_EQ(action.records().size(), 4u);
  EXPECT_TRUE(action.records()[0].aliased_to_protected);
  ExpectSegment2D(action.records()[0].seg, 0.0, 0.0, 1.0, 2.0);
  EXPECT_TRUE(action.records()[1].aliased_to_protected);
  ExpectSegment2D(action.records()[1].seg, 3.0, 5.0, 6.0, 9.0);
  EXPECT_TRUE(action.records()[2].aliased_to_protected);
  ExpectSegment2D(action.records()[2].seg, 1.0, 2.0, 3.0, 5.0);
  EXPECT_FALSE(action.records()[3].aliased_to_protected);
  ExpectSegment2D(action.records()[3].seg, 9.0, 8.0, 7.0, 6.0);

  // Protected member retains the segment populated by the last 2-arg call.
  ExpectSegment2D(action.protected_selected_segment(), 1.0, 2.0, 3.0, 5.0);
}

TEST(MonotoneChainSelectActionTest,
     PreservesZCoordinatesAcrossCoordinateTypes) {
  const CoordinateSequence seq_xyz{
      Coordinate{0.0, 0.0, 12.25},
      Coordinate{2.0, 2.0, 24.75},
      Coordinate{4.0, 4.0, 36.5},
  };
  const CoordinateSequence seq_xyzm{
      CoordinateXYZM{0.0, 0.0, -5.5, 100.0},
      CoordinateXYZM{2.0, 2.0, -10.5, 200.0},
  };
  const CoordinateSequence seq_xym{
      CoordinateXYM{0.0, 0.0, 42.0},
      CoordinateXYM{2.0, 2.0, 84.0},
  };

  const MonotoneChain mc_xyz(seq_xyz, 0, 2, nullptr);
  const MonotoneChain mc_xyzm(seq_xyzm, 0, 1, nullptr);
  const MonotoneChain mc_xym(seq_xym, 0, 1, nullptr);

  RecordingSelectAction action;
  mc_xyz.select(Envelope(0.0, 4.0, 0.0, 4.0), action);
  action.select(mc_xyzm, 0);
  action.select(mc_xym, 0);

  ASSERT_EQ(action.records().size(), 4u);
  ExpectSegment2D(action.records()[0].seg, 0.0, 0.0, 2.0, 2.0);
  ExpectSegmentZ(action.records()[0].seg, 12.25, 24.75);
  ExpectSegment2D(action.records()[1].seg, 2.0, 2.0, 4.0, 4.0);
  ExpectSegmentZ(action.records()[1].seg, 24.75, 36.5);

  ExpectSegment2D(action.records()[2].seg, 0.0, 0.0, 2.0, 2.0);
  ExpectSegmentZ(action.records()[2].seg, -5.5, -10.5);

  ExpectSegment2D(action.records()[3].seg, 0.0, 0.0, 2.0, 2.0);
  ExpectSegmentZNaN(action.records()[3].seg);
}

TEST(MonotoneChainSelectActionTest,
     OverridingTwoArgSelectReceivesChainIndexAndContext) {
  int context = 777;
  const CoordinateSequence seq{
      CoordinateXY{0.0, 0.0}, CoordinateXY{1.0, 1.0}, CoordinateXY{2.0, 2.0},
      CoordinateXY{3.0, 3.0}, CoordinateXY{4.0, 4.0},
  };
  const MonotoneChain mc(seq, 0, 4, &context);
  const Envelope search_env(0.5, 1.5, 0.5, 1.5);

  ChainRecordingSelectAction no_forward(/*forward_to_base=*/false);
  mc.select(search_env, no_forward);

  ASSERT_EQ(no_forward.chain_records().size(), 2u);
  EXPECT_TRUE(no_forward.segment_records().empty());
  EXPECT_EQ(no_forward.chain_records()[0].mc, &mc);
  EXPECT_EQ(no_forward.chain_records()[0].start, 0u);
  EXPECT_EQ(no_forward.chain_records()[0].context, &context);
  EXPECT_EQ(no_forward.chain_records()[1].mc, &mc);
  EXPECT_EQ(no_forward.chain_records()[1].start, 1u);
  EXPECT_EQ(no_forward.chain_records()[1].context, &context);

  ChainRecordingSelectAction with_forward(/*forward_to_base=*/true);
  mc.select(search_env, with_forward);

  ASSERT_EQ(with_forward.chain_records().size(), 2u);
  ASSERT_EQ(with_forward.segment_records().size(), 2u);
  EXPECT_TRUE(with_forward.segment_records()[0].aliased_to_protected);
  ExpectSegment2D(with_forward.segment_records()[0].seg, 0.0, 0.0, 1.0, 1.0);
  EXPECT_TRUE(with_forward.segment_records()[1].aliased_to_protected);
  ExpectSegment2D(with_forward.segment_records()[1].seg, 1.0, 1.0, 2.0, 2.0);
}

TEST(MonotoneChainSelectActionTest,
     IntegrationWithMonotoneChainSelectEnvelopeQueries) {
  const CoordinateSequence seq{
      CoordinateXY{0.0, 0.0}, CoordinateXY{1.0, 1.0}, CoordinateXY{2.0, 2.0},
      CoordinateXY{3.0, 3.0}, CoordinateXY{4.0, 4.0},
  };
  const MonotoneChain mc(seq, 0, 4, nullptr);

  // Null and disjoint envelopes prune at the root on a multi-segment chain.
  RecordingSelectAction null_action;
  mc.select(Envelope(), null_action);
  EXPECT_TRUE(null_action.records().empty());

  RecordingSelectAction disjoint_action;
  mc.select(Envelope(10.0, 20.0, 10.0, 20.0), disjoint_action);
  EXPECT_TRUE(disjoint_action.records().empty());

  // Full envelope visits every segment in order.
  RecordingSelectAction full_action;
  mc.select(Envelope(0.0, 4.0, 0.0, 4.0), full_action);
  ASSERT_EQ(full_action.records().size(), 4u);
  ExpectSegment2D(full_action.records()[0].seg, 0.0, 0.0, 1.0, 1.0);
  ExpectSegment2D(full_action.records()[1].seg, 1.0, 1.0, 2.0, 2.0);
  ExpectSegment2D(full_action.records()[2].seg, 2.0, 2.0, 3.0, 3.0);
  ExpectSegment2D(full_action.records()[3].seg, 3.0, 3.0, 4.0, 4.0);

  // Single-point boundary envelope at (4,4) visits the upper half [2..4].
  RecordingSelectAction corner_action;
  mc.select(Envelope(CoordinateXY{4.0, 4.0}), corner_action);
  ASSERT_EQ(corner_action.records().size(), 2u);
  ExpectSegment2D(corner_action.records()[0].seg, 2.0, 2.0, 3.0, 3.0);
  ExpectSegment2D(corner_action.records()[1].seg, 3.0, 3.0, 4.0, 4.0);

  // A 1-segment chain immediately dispatches to select() before envelope check.
  const CoordinateSequence single_seg_seq{CoordinateXY{0.0, 0.0},
                                          CoordinateXY{2.0, 2.0}};
  const MonotoneChain single_seg_chain(single_seg_seq, 0, 1, nullptr);
  RecordingSelectAction single_seg_action;
  single_seg_chain.select(Envelope(10.0, 20.0, 10.0, 20.0), single_seg_action);
  ASSERT_EQ(single_seg_action.records().size(), 1u);
  ExpectSegment2D(single_seg_action.records()[0].seg, 0.0, 0.0, 2.0, 2.0);

  // A 1-vertex degenerate chain has zero segments and never invokes select().
  const CoordinateSequence single_pt{CoordinateXY{0.0, 0.0}};
  std::vector<MonotoneChain> degenerate_chains;
  MonotoneChainBuilder::getChains(&single_pt, nullptr, degenerate_chains);
  ASSERT_EQ(degenerate_chains.size(), 1u);
  RecordingSelectAction degenerate_action;
  degenerate_chains[0].select(Envelope(-1.0, 1.0, -1.0, 1.0),
                              degenerate_action);
  EXPECT_TRUE(degenerate_action.records().empty());
}

TEST(MonotoneChainSelectActionTest,
     IntegrationWithMonotoneChainBuilderMultiChain) {
  const CoordinateSequence polyline{
      CoordinateXY{0.0, 0.0},  CoordinateXY{1.0, 1.0}, CoordinateXY{2.0, 2.0},
      CoordinateXY{3.0, 1.0},  CoordinateXY{4.0, 0.0}, CoordinateXY{5.0, -1.0},
      CoordinateXY{6.0, -2.0},
  };
  std::vector<MonotoneChain> chains;
  MonotoneChainBuilder::getChains(&polyline, nullptr, chains);
  ASSERT_EQ(chains.size(), 2u);

  // Query intersecting only the second half of chain 1 [4..6].
  RecordingSelectAction action;
  const Envelope search_env(4.2, 4.8, -0.8, -0.2);
  for (const auto& chain : chains) {
    chain.select(search_env, action);
  }

  ASSERT_EQ(action.records().size(), 2u);
  ExpectSegment2D(action.records()[0].seg, 4.0, 0.0, 5.0, -1.0);
  ExpectSegment2D(action.records()[1].seg, 5.0, -1.0, 6.0, -2.0);
}

}  // namespace
}  // namespace geos_google
