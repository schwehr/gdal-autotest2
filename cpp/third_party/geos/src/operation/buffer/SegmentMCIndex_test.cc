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

#include "third_party/geos/include/geos/operation/buffer/SegmentMCIndex.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/LineSegment.h"
#include "third_party/geos/include/geos/index/chain/MonotoneChain.h"
#include "third_party/geos/include/geos/index/chain/MonotoneChainSelectAction.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Envelope;
using geos::geom::LineSegment;
using geos::index::chain::MonotoneChain;
using geos::index::chain::MonotoneChainSelectAction;
using geos::operation::buffer::SegmentMCIndex;

class RecordingSelectAction final : public MonotoneChainSelectAction {
 public:
  void select(const LineSegment& seg) override { segments.push_back(seg); }

  std::vector<LineSegment> segments;
};

struct ChainSelectionRecord {
  void* context;
  std::size_t start_index;
  LineSegment segment;
};

class ChainInspectingSelectAction final : public MonotoneChainSelectAction {
 public:
  void select(const MonotoneChain& mc, std::size_t start) override {
    LineSegment seg;
    mc.getLineSegment(start, seg);
    records.push_back({mc.getContext(), start, seg});
  }

  void select(const LineSegment& /*seg*/) override {}

  std::vector<ChainSelectionRecord> records;
};

void ExpectSegmentEq(const LineSegment& seg, double x0, double y0, double x1,
                     double y1) {
  EXPECT_EQ(seg.p0, Coordinate(x0, y0));
  EXPECT_EQ(seg.p1, Coordinate(x1, y1));
}

void ExpectSegmentsSortedEq(std::vector<LineSegment> actual,
                            std::vector<LineSegment> expected) {
  std::sort(actual.begin(), actual.end());
  std::sort(expected.begin(), expected.end());
  ASSERT_EQ(actual.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(actual[i].p0, expected[i].p0) << "at index " << i;
    EXPECT_EQ(actual[i].p1, expected[i].p1) << "at index " << i;
  }
}

TEST(SegmentMCIndexTest, EmptySequenceYieldsNoSegments) {
  CoordinateSequence seq;
  SegmentMCIndex index(&seq);

  RecordingSelectAction action;
  const Envelope env(-100.0, 100.0, -100.0, 100.0);
  index.query(&env, action);

  EXPECT_TRUE(action.segments.empty());
}

TEST(SegmentMCIndexTest, SinglePointSequenceYieldsNoSegments) {
  CoordinateSequence seq{Coordinate(3.0, 4.0)};
  SegmentMCIndex index(&seq);

  RecordingSelectAction action;
  const Envelope env(0.0, 10.0, 0.0, 10.0);
  index.query(&env, action);

  EXPECT_TRUE(action.segments.empty());
}

TEST(SegmentMCIndexTest, SingleSegmentMatchingDisjointAndNullEnvelopes) {
  CoordinateSequence seq{Coordinate(0.0, 0.0), Coordinate(10.0, 10.0)};
  SegmentMCIndex index(&seq);

  {
    RecordingSelectAction action;
    const Envelope overlapping(2.0, 4.0, 2.0, 4.0);
    index.query(&overlapping, action);
    ASSERT_EQ(action.segments.size(), 1u);
    ExpectSegmentEq(action.segments[0], 0.0, 0.0, 10.0, 10.0);
  }
  {
    RecordingSelectAction action;
    const Envelope endpoint_only(CoordinateXY(0.0, 0.0));
    index.query(&endpoint_only, action);
    ASSERT_EQ(action.segments.size(), 1u);
    ExpectSegmentEq(action.segments[0], 0.0, 0.0, 10.0, 10.0);
  }
  {
    RecordingSelectAction action;
    const Envelope other_endpoint(CoordinateXY(10.0, 10.0));
    index.query(&other_endpoint, action);
    ASSERT_EQ(action.segments.size(), 1u);
    ExpectSegmentEq(action.segments[0], 0.0, 0.0, 10.0, 10.0);
  }
  {
    RecordingSelectAction action;
    const Envelope near_miss(10.0001, 12.0, 10.0001, 12.0);
    index.query(&near_miss, action);
    EXPECT_TRUE(action.segments.empty());
  }
  {
    RecordingSelectAction action;
    const Envelope disjoint(20.0, 30.0, 20.0, 30.0);
    index.query(&disjoint, action);
    EXPECT_TRUE(action.segments.empty());
  }
  {
    RecordingSelectAction action;
    const Envelope null_env;
    index.query(&null_env, action);
    EXPECT_TRUE(action.segments.empty());
  }
}

TEST(SegmentMCIndexTest, MultiChainPolylineFiltersByEnvelope) {
  // Monotone chains produced by MonotoneChainBuilder:
  //   [0,1]: (0,0)->(4,4) [NE]
  //   [1,3]: (4,4)->(8,0)->(12,-4) [SE]
  //   [3,5]: (12,-4)->(16,0)->(16,8) [NE]
  //   [5,6]: (16,8)->(8,8) [NW]
  CoordinateSequence seq{
      Coordinate(0.0, 0.0),   Coordinate(4.0, 4.0),  Coordinate(8.0, 0.0),
      Coordinate(12.0, -4.0), Coordinate(16.0, 0.0), Coordinate(16.0, 8.0),
      Coordinate(8.0, 8.0),
  };
  SegmentMCIndex index(&seq);

  {
    // Overlaps chains [1,3] and [3,5]; excludes [0,1] and [5,6].
    // MonotoneChain::computeSelect emits all 1-segment leaves of an overlapping
    // 2-segment subchain without re-testing leaf bounds, so (16,0)->(16,8) is
    // visited alongside (12,-4)->(16,0).
    RecordingSelectAction action;
    const Envelope sub_env(7.0, 13.0, -5.0, 1.0);
    index.query(&sub_env, action);
    ExpectSegmentsSortedEq(action.segments,
                           {
                               LineSegment(4.0, 4.0, 8.0, 0.0),
                               LineSegment(8.0, 0.0, 12.0, -4.0),
                               LineSegment(12.0, -4.0, 16.0, 0.0),
                               LineSegment(16.0, 0.0, 16.0, 8.0),
                           });
  }
  {
    // Isolates only chain [5,6].
    RecordingSelectAction action;
    const Envelope top_only(8.5, 11.5, 7.5, 8.5);
    index.query(&top_only, action);
    ASSERT_EQ(action.segments.size(), 1u);
    ExpectSegmentEq(action.segments[0], 16.0, 8.0, 8.0, 8.0);
  }
  {
    RecordingSelectAction action;
    const Envelope full_env(-1.0, 20.0, -10.0, 10.0);
    index.query(&full_env, action);
    ExpectSegmentsSortedEq(action.segments,
                           {
                               LineSegment(0.0, 0.0, 4.0, 4.0),
                               LineSegment(4.0, 4.0, 8.0, 0.0),
                               LineSegment(8.0, 0.0, 12.0, -4.0),
                               LineSegment(12.0, -4.0, 16.0, 0.0),
                               LineSegment(16.0, 0.0, 16.0, 8.0),
                               LineSegment(16.0, 8.0, 8.0, 8.0),
                           });
  }
}

TEST(SegmentMCIndexTest, LongMonotoneChainPrunesNonIntersectingSubchains) {
  CoordinateSequence seq;
  for (int i = 0; i <= 8; ++i) {
    seq.add(Coordinate(static_cast<double>(i), static_cast<double>(i)));
  }
  SegmentMCIndex index(&seq);

  RecordingSelectAction action;
  const Envelope narrow_env(5.2, 5.8, 5.2, 5.8);
  index.query(&narrow_env, action);

  ExpectSegmentsSortedEq(action.segments, {
                                              LineSegment(4.0, 4.0, 5.0, 5.0),
                                              LineSegment(5.0, 5.0, 6.0, 6.0),
                                          });
}

TEST(SegmentMCIndexTest, LargeZigZagExercisesMultiLevelSTRtreeAndRequery) {
  CoordinateSequence seq;
  constexpr std::size_t kPoints = 101;
  seq.reserve(kPoints);
  for (std::size_t i = 0; i < kPoints; ++i) {
    seq.add(Coordinate(static_cast<double>(i), (i % 2 == 0) ? 0.0 : 5.0));
  }
  SegmentMCIndex index(&seq);

  {
    RecordingSelectAction action;
    const Envelope narrow_env(24.5, 25.5, 1.0, 4.0);
    index.query(&narrow_env, action);
    ExpectSegmentsSortedEq(action.segments,
                           {
                               LineSegment(24.0, 0.0, 25.0, 5.0),
                               LineSegment(25.0, 5.0, 26.0, 0.0),
                           });
  }
  {
    RecordingSelectAction action;
    const Envelope tail_env(98.5, 99.5, 1.0, 4.0);
    index.query(&tail_env, action);
    ExpectSegmentsSortedEq(action.segments,
                           {
                               LineSegment(98.0, 0.0, 99.0, 5.0),
                               LineSegment(99.0, 5.0, 100.0, 0.0),
                           });
  }
  {
    RecordingSelectAction action;
    const Envelope outside_env(200.0, 210.0, 0.0, 5.0);
    index.query(&outside_env, action);
    EXPECT_TRUE(action.segments.empty());
  }
}

TEST(SegmentMCIndexTest, HandlesRepeatedConsecutivePoints) {
  {
    CoordinateSequence seq{
        Coordinate(0.0, 0.0), Coordinate(0.0, 0.0),  Coordinate(5.0, 5.0),
        Coordinate(5.0, 5.0), Coordinate(10.0, 0.0),
    };
    SegmentMCIndex index(&seq);

    RecordingSelectAction action;
    const Envelope env(4.0, 6.0, 4.0, 6.0);
    index.query(&env, action);
    ExpectSegmentsSortedEq(action.segments,
                           {
                               LineSegment(0.0, 0.0, 0.0, 0.0),
                               LineSegment(0.0, 0.0, 5.0, 5.0),
                               LineSegment(5.0, 5.0, 5.0, 5.0),
                               LineSegment(5.0, 5.0, 10.0, 0.0),
                           });

    RecordingSelectAction right_action;
    const Envelope right_env(9.0, 11.0, -1.0, 1.0);
    index.query(&right_env, right_action);
    ASSERT_EQ(right_action.segments.size(), 1u);
    ExpectSegmentEq(right_action.segments[0], 5.0, 5.0, 10.0, 0.0);
  }
  {
    CoordinateSequence identical{
        Coordinate(2.0, 3.0),
        Coordinate(2.0, 3.0),
        Coordinate(2.0, 3.0),
    };
    SegmentMCIndex index(&identical);

    RecordingSelectAction hit_action;
    const Envelope hit_env(1.0, 3.0, 2.0, 4.0);
    index.query(&hit_env, hit_action);
    ExpectSegmentsSortedEq(hit_action.segments,
                           {
                               LineSegment(2.0, 3.0, 2.0, 3.0),
                               LineSegment(2.0, 3.0, 2.0, 3.0),
                           });

    RecordingSelectAction miss_action;
    const Envelope miss_env(10.0, 11.0, 10.0, 11.0);
    index.query(&miss_env, miss_action);
    EXPECT_TRUE(miss_action.segments.empty());
  }
}

TEST(SegmentMCIndexTest, ClosedRingWithAxisAlignedSegments) {
  CoordinateSequence seq{
      Coordinate(0.0, 0.0),  Coordinate(10.0, 0.0), Coordinate(10.0, 10.0),
      Coordinate(0.0, 10.0), Coordinate(0.0, 0.0),
  };
  SegmentMCIndex index(&seq);

  {
    RecordingSelectAction action;
    const Envelope top_env(2.0, 8.0, 9.5, 10.5);
    index.query(&top_env, action);
    ExpectSegmentsSortedEq(action.segments,
                           {
                               LineSegment(0.0, 0.0, 10.0, 0.0),
                               LineSegment(10.0, 0.0, 10.0, 10.0),
                               LineSegment(10.0, 10.0, 0.0, 10.0),
                           });
  }
  {
    RecordingSelectAction action;
    const Envelope all_env(-5.0, 15.0, -5.0, 15.0);
    index.query(&all_env, action);
    ExpectSegmentsSortedEq(action.segments,
                           {
                               LineSegment(0.0, 0.0, 10.0, 0.0),
                               LineSegment(10.0, 0.0, 10.0, 10.0),
                               LineSegment(10.0, 10.0, 0.0, 10.0),
                               LineSegment(0.0, 10.0, 0.0, 0.0),
                           });
  }
}

TEST(SegmentMCIndexTest, CustomChainActionReceivesNullContextAndCoordinates) {
  {
    CoordinateSequence seq_xyz{
        Coordinate(0.0, 0.0, 10.0),
        Coordinate(3.0, 4.0, 20.0),
        Coordinate(6.0, 0.0, 30.0),
    };
    SegmentMCIndex index(&seq_xyz);

    ChainInspectingSelectAction action;
    const Envelope env(-1.0, 7.0, -1.0, 5.0);
    index.query(&env, action);

    ASSERT_EQ(action.records.size(), 2u);
    std::sort(action.records.begin(), action.records.end(),
              [](const ChainSelectionRecord& a, const ChainSelectionRecord& b) {
                return a.start_index < b.start_index;
              });
    EXPECT_EQ(action.records[0].context, nullptr);
    EXPECT_EQ(action.records[0].start_index, 0u);
    EXPECT_TRUE(
        action.records[0].segment.p0.equals3D(Coordinate(0.0, 0.0, 10.0)));
    EXPECT_TRUE(
        action.records[0].segment.p1.equals3D(Coordinate(3.0, 4.0, 20.0)));

    EXPECT_EQ(action.records[1].context, nullptr);
    EXPECT_EQ(action.records[1].start_index, 1u);
    EXPECT_TRUE(
        action.records[1].segment.p0.equals3D(Coordinate(3.0, 4.0, 20.0)));
    EXPECT_TRUE(
        action.records[1].segment.p1.equals3D(Coordinate(6.0, 0.0, 30.0)));
  }
  {
    CoordinateSequence seq_xy{
        CoordinateXY(-2.0, -3.0),
        CoordinateXY(4.0, 5.0),
    };
    SegmentMCIndex index(&seq_xy);

    RecordingSelectAction action;
    const Envelope env(-1.0, 1.0, -1.0, 1.0);
    index.query(&env, action);

    ASSERT_EQ(action.segments.size(), 1u);
    ExpectSegmentEq(action.segments[0], -2.0, -3.0, 4.0, 5.0);
    EXPECT_TRUE(std::isnan(action.segments[0].p0.z));
    EXPECT_TRUE(std::isnan(action.segments[0].p1.z));
  }
}

}  // namespace
}  // namespace geos_google
