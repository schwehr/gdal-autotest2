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

#include "third_party/geos/include/geos/noding/MCIndexSegmentSetMutualIntersector.h"

#include <cstddef>
#include <initializer_list>
#include <limits>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/index/ItemVisitor.h"
#include "third_party/geos/include/geos/index/SpatialIndex.h"
#include "third_party/geos/include/geos/index/chain/MonotoneChain.h"
#include "third_party/geos/include/geos/noding/BasicSegmentString.h"
#include "third_party/geos/include/geos/noding/NodingIntersectionFinder.h"
#include "third_party/geos/include/geos/noding/SegmentIntersector.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"

namespace geos_google {
namespace {

using geos::algorithm::LineIntersector;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Envelope;
using geos::index::ItemVisitor;
using geos::index::SpatialIndex;
using geos::index::chain::MonotoneChain;
using geos::noding::BasicSegmentString;
using geos::noding::MCIndexSegmentSetMutualIntersector;
using geos::noding::NodingIntersectionFinder;
using geos::noding::SegmentIntersector;
using geos::noding::SegmentString;

// Owns CoordinateSequences and BasicSegmentStrings for clean RAII lifetimes.
class OwnedSegmentSet {
 public:
  const SegmentString* Add(std::initializer_list<CoordinateXY> coords,
                           const void* context = nullptr) {
    auto seq = std::make_unique<CoordinateSequence>(coords);
    auto seg = std::make_unique<BasicSegmentString>(seq.get(), context);
    const SegmentString* raw = seg.get();
    sequences_.push_back(std::move(seq));
    strings_.push_back(std::move(seg));
    view_.push_back(raw);
    return raw;
  }

  SegmentString::ConstVect* view() { return &view_; }

 private:
  std::vector<std::unique_ptr<CoordinateSequence>> sequences_;
  std::vector<std::unique_ptr<BasicSegmentString>> strings_;
  SegmentString::ConstVect view_;
};

struct OverlapCall {
  const SegmentString* e0 = nullptr;
  std::size_t seg_index0 = 0;
  const SegmentString* e1 = nullptr;
  std::size_t seg_index1 = 0;
};

class RecordingSegmentIntersector final : public SegmentIntersector {
 public:
  explicit RecordingSegmentIntersector(
      std::size_t stop_after = std::numeric_limits<std::size_t>::max())
      : stop_after_(stop_after) {}

  void processIntersections(SegmentString* e0, std::size_t segIndex0,
                            SegmentString* e1, std::size_t segIndex1) override {
    calls_.push_back({e0, segIndex0, e1, segIndex1});
  }

  bool isDone() const override { return calls_.size() >= stop_after_; }

  const std::vector<OverlapCall>& calls() const { return calls_; }

 private:
  std::size_t stop_after_;
  std::vector<OverlapCall> calls_;
};

class CollectingVisitor final : public ItemVisitor {
 public:
  void visitItem(void* item) override { items_.push_back(item); }
  const std::vector<void*>& items() const { return items_; }

 private:
  std::vector<void*> items_;
};

TEST(MCIndexSegmentSetMutualIntersectorTest, DefaultConstructAndBasicCrossing) {
  OwnedSegmentSet base;
  const SegmentString* base_seg = base.Add({{0.0, 0.0}, {10.0, 0.0}});

  OwnedSegmentSet query;
  const SegmentString* query_seg = query.Add({{5.0, -5.0}, {5.0, 5.0}});

  MCIndexSegmentSetMutualIntersector intersector;
  RecordingSegmentIntersector recorder;
  intersector.setSegmentIntersector(&recorder);
  intersector.setBaseSegments(base.view());
  intersector.process(query.view());

  ASSERT_EQ(recorder.calls().size(), 1u);
  EXPECT_EQ(recorder.calls()[0].e0, query_seg);
  EXPECT_EQ(recorder.calls()[0].seg_index0, 0u);
  EXPECT_EQ(recorder.calls()[0].e1, base_seg);
  EXPECT_EQ(recorder.calls()[0].seg_index1, 0u);

  // Also verify with a real geometric SegmentIntersector.
  MCIndexSegmentSetMutualIntersector geom_intersector;
  LineIntersector li;
  NodingIntersectionFinder finder(li);
  geom_intersector.setSegmentIntersector(&finder);
  geom_intersector.setBaseSegments(base.view());
  geom_intersector.process(query.view());

  EXPECT_TRUE(finder.hasIntersection());
  EXPECT_TRUE(finder.getInteriorIntersection().equals2D(Coordinate(5.0, 0.0)));
}

TEST(MCIndexSegmentSetMutualIntersectorTest, DisjointSegmentsSkipIntersector) {
  OwnedSegmentSet base;
  base.Add({{0.0, 0.0}, {2.0, 0.0}});

  OwnedSegmentSet query;
  query.Add({{5.0, 5.0}, {8.0, 8.0}});

  MCIndexSegmentSetMutualIntersector intersector;
  RecordingSegmentIntersector recorder;
  intersector.setSegmentIntersector(&recorder);
  intersector.setBaseSegments(base.view());
  intersector.process(query.view());

  EXPECT_TRUE(recorder.calls().empty());
}

TEST(MCIndexSegmentSetMutualIntersectorTest, EmptyInputsAndZeroSizeStrings) {
  // Both vectors empty.
  {
    SegmentString::ConstVect empty_base;
    SegmentString::ConstVect empty_query;
    MCIndexSegmentSetMutualIntersector intersector;
    RecordingSegmentIntersector recorder;
    intersector.setSegmentIntersector(&recorder);
    intersector.setBaseSegments(&empty_base);
    intersector.process(&empty_query);
    EXPECT_TRUE(recorder.calls().empty());
  }

  // Zero-size SegmentStrings mixed with valid crossing SegmentStrings.
  {
    OwnedSegmentSet base;
    base.Add({});
    const SegmentString* base_valid = base.Add({{0.0, 0.0}, {10.0, 0.0}});
    base.Add({{100.0, 100.0}});  // Single vertex (0 segments).

    OwnedSegmentSet query;
    query.Add({});
    const SegmentString* query_valid = query.Add({{5.0, -2.0}, {5.0, 2.0}});
    query.Add({{100.0, 100.0}});  // Single vertex (0 segments).

    MCIndexSegmentSetMutualIntersector intersector;
    RecordingSegmentIntersector recorder;
    intersector.setSegmentIntersector(&recorder);
    intersector.setBaseSegments(base.view());
    intersector.process(query.view());

    ASSERT_EQ(recorder.calls().size(), 1u);
    EXPECT_EQ(recorder.calls()[0].e0, query_valid);
    EXPECT_EQ(recorder.calls()[0].e1, base_valid);
  }
}

TEST(MCIndexSegmentSetMutualIntersectorTest, ToleranceExpandsOverlapDetection) {
  OwnedSegmentSet base;
  const SegmentString* base_seg = base.Add({{0.0, 0.0}, {10.0, 0.0}});

  // Query runs parallel 0.5 units above base: disjoint envelopes at tol=0.1,
  // overlapping envelopes at tol=1.0.
  OwnedSegmentSet query;
  const SegmentString* query_seg = query.Add({{2.0, 0.5}, {8.0, 0.5}});

  {
    MCIndexSegmentSetMutualIntersector tight_intersector(0.1);
    RecordingSegmentIntersector recorder;
    tight_intersector.setSegmentIntersector(&recorder);
    tight_intersector.setBaseSegments(base.view());
    tight_intersector.process(query.view());
    EXPECT_TRUE(recorder.calls().empty());
  }

  {
    MCIndexSegmentSetMutualIntersector wide_intersector(1.0);
    RecordingSegmentIntersector recorder;
    wide_intersector.setSegmentIntersector(&recorder);
    wide_intersector.setBaseSegments(base.view());
    wide_intersector.process(query.view());

    ASSERT_EQ(recorder.calls().size(), 1u);
    EXPECT_EQ(recorder.calls()[0].e0, query_seg);
    EXPECT_EQ(recorder.calls()[0].e1, base_seg);
  }
}

TEST(MCIndexSegmentSetMutualIntersectorTest,
     EnvelopeFiltersIndexedAndQueryChains) {
  const Envelope filter_env(0.0, 10.0, 0.0, 10.0);

  OwnedSegmentSet base;
  const SegmentString* base_inside = base.Add({{2.0, 2.0}, {8.0, 8.0}});
  // Outside filter_env even though query_outside crosses it.
  base.Add({{20.0, 20.0}, {30.0, 30.0}});

  OwnedSegmentSet query;
  const SegmentString* query_inside = query.Add({{2.0, 8.0}, {8.0, 2.0}});
  // Crosses base_outside, but both are outside filter_env.
  query.Add({{20.0, 30.0}, {30.0, 20.0}});
  // Intersects filter_env (so added to monoChains) but misses base_inside.
  query.Add({{9.0, 1.0}, {12.0, 1.0}});

  MCIndexSegmentSetMutualIntersector intersector(&filter_env);
  RecordingSegmentIntersector recorder;
  intersector.setSegmentIntersector(&recorder);
  intersector.setBaseSegments(base.view());
  intersector.process(query.view());

  ASSERT_EQ(recorder.calls().size(), 1u);
  EXPECT_EQ(recorder.calls()[0].e0, query_inside);
  EXPECT_EQ(recorder.calls()[0].e1, base_inside);

  // Verify the spatial index only contains chains intersecting filter_env.
  SpatialIndex* index = intersector.getIndex();
  ASSERT_NE(index, nullptr);
  const Envelope all_env(-100.0, 100.0, -100.0, 100.0);
  std::vector<void*> results;
  index->query(&all_env, results);
  ASSERT_EQ(results.size(), 1u);
  const auto* indexed_chain = static_cast<const MonotoneChain*>(results[0]);
  EXPECT_EQ(indexed_chain->getContext(), base_inside);
}

TEST(MCIndexSegmentSetMutualIntersectorTest,
     MultipleProcessCallsReuseIndexAndClearPreviousQueryChains) {
  OwnedSegmentSet base;
  const SegmentString* base0 = base.Add({{0.0, 0.0}, {10.0, 0.0}});
  const SegmentString* base1 = base.Add({{0.0, 10.0}, {10.0, 10.0}});

  MCIndexSegmentSetMutualIntersector intersector;
  intersector.setBaseSegments(base.view());

  // Before process(), the spatial index has not been populated.
  SpatialIndex* index = intersector.getIndex();
  ASSERT_NE(index, nullptr);
  const Envelope all_env(-100.0, 100.0, -100.0, 100.0);
  CollectingVisitor before_visitor;
  index->query(&all_env, before_visitor);
  EXPECT_TRUE(before_visitor.items().empty());

  // First process() builds the index and tests query1.
  OwnedSegmentSet query1;
  const SegmentString* q1 = query1.Add({{5.0, -2.0}, {5.0, 2.0}});
  RecordingSegmentIntersector rec1;
  intersector.setSegmentIntersector(&rec1);
  intersector.process(query1.view());

  ASSERT_EQ(rec1.calls().size(), 1u);
  EXPECT_EQ(rec1.calls()[0].e0, q1);
  EXPECT_EQ(rec1.calls()[0].e1, base0);

  // Index now contains both base monotone chains.
  CollectingVisitor after_visitor;
  index->query(&all_env, after_visitor);
  EXPECT_EQ(after_visitor.items().size(), 2u);

  // Second process() reuses the built index and clears old query chains.
  OwnedSegmentSet query2;
  const SegmentString* q2 = query2.Add({{5.0, 8.0}, {5.0, 12.0}});
  RecordingSegmentIntersector rec2;
  intersector.setSegmentIntersector(&rec2);
  intersector.process(query2.view());

  ASSERT_EQ(rec2.calls().size(), 1u);
  EXPECT_EQ(rec2.calls()[0].e0, q2);
  EXPECT_EQ(rec2.calls()[0].e1, base1);
}

TEST(MCIndexSegmentSetMutualIntersectorTest,
     AbortsIndexQueryEarlyWhenSegmentIntersectorIsDone) {
  OwnedSegmentSet base;
  for (int x = 1; x <= 6; ++x) {
    base.Add({{static_cast<double>(x), 0.0}, {static_cast<double>(x), 10.0}});
  }

  OwnedSegmentSet query;
  query.Add({{0.0, 5.0}, {7.0, 5.0}});

  // Without early termination, all 6 base chains overlap the query chain.
  {
    MCIndexSegmentSetMutualIntersector intersector;
    RecordingSegmentIntersector recorder;
    intersector.setSegmentIntersector(&recorder);
    intersector.setBaseSegments(base.view());
    intersector.process(query.view());
    EXPECT_EQ(recorder.calls().size(), 6u);
  }

  // With stop_after=1, isDone() returns true after the first overlap,
  // causing index.query to abort before visiting the remaining 5 chains.
  {
    MCIndexSegmentSetMutualIntersector intersector;
    RecordingSegmentIntersector recorder(/*stop_after=*/1);
    intersector.setSegmentIntersector(&recorder);
    intersector.setBaseSegments(base.view());
    intersector.process(query.view());
    EXPECT_EQ(recorder.calls().size(), 1u);
  }
}

TEST(MCIndexSegmentSetMutualIntersectorTest,
     MultiChainPolylineIdentifiesExactSegmentIndices) {
  // Base polyline:
  //   seg 0: (0, 0) -> (5, 5)     [chain 0, quadrant NE]
  //   seg 1: (5, 5) -> (10, 10)   [chain 0, quadrant NE]
  //   seg 2: (10, 10) -> (15, 5)  [chain 1, quadrant SE]
  //   seg 3: (15, 5) -> (20, 0)   [chain 1, quadrant SE]
  OwnedSegmentSet base;
  const SegmentString* base_poly = base.Add(
      {{0.0, 0.0}, {5.0, 5.0}, {10.0, 10.0}, {15.0, 5.0}, {20.0, 0.0}});

  // Query polyline:
  //   seg 0: (6, 0) -> (7, 4)    (below base)
  //   seg 1: (7, 4) -> (8, 9)    (crosses base seg 1: (5,5)-(10,10))
  //   seg 2: (8, 9) -> (18, 9)   (crosses base seg 2: (10,10)-(15,5))
  OwnedSegmentSet query;
  const SegmentString* query_poly =
      query.Add({{6.0, 0.0}, {7.0, 4.0}, {8.0, 9.0}, {18.0, 9.0}});

  MCIndexSegmentSetMutualIntersector intersector;
  LineIntersector li;
  NodingIntersectionFinder finder(li);
  finder.setFindAllIntersections(true);
  intersector.setSegmentIntersector(&finder);
  intersector.setBaseSegments(base.view());
  intersector.process(query.view());

  // Note NodingIntersectionFinder::isDone() returns true after first find,
  // so also verify exact segment indices via a non-stopping recorder.
  RecordingSegmentIntersector recorder;
  intersector.setSegmentIntersector(&recorder);
  intersector.process(query.view());

  bool saw_seg1_seg1 = false;
  bool saw_seg2_seg2 = false;
  for (const OverlapCall& call : recorder.calls()) {
    EXPECT_EQ(call.e0, query_poly);
    EXPECT_EQ(call.e1, base_poly);
    if (call.seg_index0 == 1 && call.seg_index1 == 1) {
      saw_seg1_seg1 = true;
    }
    if (call.seg_index0 == 2 && call.seg_index1 == 2) {
      saw_seg2_seg2 = true;
    }
  }
  EXPECT_TRUE(saw_seg1_seg1);
  EXPECT_TRUE(saw_seg2_seg2);
}

TEST(MCIndexSegmentSetMutualIntersectorTest, SegmentOverlapActionDirectCall) {
  CoordinateSequence seq1({CoordinateXY(0.0, 0.0), CoordinateXY(5.0, 0.0),
                           CoordinateXY(10.0, 0.0)});
  CoordinateSequence seq2({CoordinateXY(0.0, 5.0), CoordinateXY(5.0, 5.0),
                           CoordinateXY(10.0, 5.0), CoordinateXY(15.0, 5.0)});
  BasicSegmentString ss1(&seq1, nullptr);
  BasicSegmentString ss2(&seq2, nullptr);

  MonotoneChain mc1(seq1, 0, 2, &ss1);
  MonotoneChain mc2(seq2, 0, 3, &ss2);

  RecordingSegmentIntersector recorder;
  MCIndexSegmentSetMutualIntersector::SegmentOverlapAction action(recorder);
  action.overlap(mc1, 1, mc2, 2);

  ASSERT_EQ(recorder.calls().size(), 1u);
  EXPECT_EQ(recorder.calls()[0].e0, &ss1);
  EXPECT_EQ(recorder.calls()[0].seg_index0, 1u);
  EXPECT_EQ(recorder.calls()[0].e1, &ss2);
  EXPECT_EQ(recorder.calls()[0].seg_index1, 2u);
}

}  // namespace
}  // namespace geos_google
