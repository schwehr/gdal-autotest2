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

// Unit tests for third_party/geos/src/operation/overlayng/LineLimiter.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/overlayng/LineLimiter.cpp

#include "third_party/geos/include/geos/operation/overlayng/LineLimiter.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Envelope;
using geos::operation::overlayng::LineLimiter;

void ExpectSections2D(
    const std::vector<std::unique_ptr<CoordinateSequence>>& actual,
    const std::vector<std::vector<CoordinateXY>>& expected) {
  ASSERT_EQ(actual.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    ASSERT_NE(actual[i], nullptr);
    EXPECT_FALSE(actual[i]->hasRepeatedPoints());
    ASSERT_EQ(actual[i]->size(), expected[i].size());
    for (std::size_t j = 0; j < expected[i].size(); ++j) {
      EXPECT_EQ(actual[i]->getAt<CoordinateXY>(j), expected[i][j]);
    }
  }
}

TEST(LineLimiterTest, EmptySequenceReturnsNoSections) {
  Envelope env(0.0, 10.0, 0.0, 10.0);
  LineLimiter limiter(&env);
  CoordinateSequence empty_seq = CoordinateSequence::XY(0);

  ExpectSections2D(limiter.limit(&empty_seq), {});
}

TEST(LineLimiterTest, NullEnvelopeReturnsNoSections) {
  Envelope null_env;
  LineLimiter limiter(&null_env);
  CoordinateSequence seq({CoordinateXY{0.0, 0.0}, CoordinateXY{5.0, 5.0},
                          CoordinateXY{10.0, 10.0}});

  ExpectSections2D(limiter.limit(&seq), {});
}

TEST(LineLimiterTest, SinglePointInsideAndOutside) {
  Envelope env(0.0, 10.0, 0.0, 10.0);
  LineLimiter limiter(&env);

  CoordinateSequence inside({CoordinateXY{5.0, 5.0}});
  ExpectSections2D(limiter.limit(&inside), {{{5.0, 5.0}}});

  CoordinateSequence outside({CoordinateXY{15.0, 5.0}});
  ExpectSections2D(limiter.limit(&outside), {});
}

TEST(LineLimiterTest, AllPointsInsideYieldsSingleUnchangedSection) {
  Envelope env(0.0, 10.0, 0.0, 10.0);
  LineLimiter limiter(&env);
  CoordinateSequence seq(
      {CoordinateXY{1.0, 2.0}, CoordinateXY{5.0, 5.0}, CoordinateXY{9.0, 8.0}});

  ExpectSections2D(limiter.limit(&seq), {{{1.0, 2.0}, {5.0, 5.0}, {9.0, 8.0}}});
}

TEST(LineLimiterTest, AllOutsideWithDisjointSegmentsYieldsNoSections) {
  Envelope env(0.0, 10.0, 0.0, 10.0);
  LineLimiter limiter(&env);
  CoordinateSequence seq({CoordinateXY{-5.0, -5.0}, CoordinateXY{-5.0, 15.0},
                          CoordinateXY{15.0, 15.0}, CoordinateXY{15.0, -5.0}});

  ExpectSections2D(limiter.limit(&seq), {});
}

TEST(LineLimiterTest, EntersAndExitsEnvelopeKeepingAdjacentOutsideEndpoints) {
  Envelope env(0.0, 10.0, 0.0, 10.0);
  LineLimiter limiter(&env);

  // Leading segment (-20, 5)->(-5, 5) and trailing segment (15, 5)->(30, 5)
  // are disjoint from env and discarded; entry (-5, 5) and exit (15, 5) are
  // preserved.
  CoordinateSequence seq({CoordinateXY{-20.0, 5.0}, CoordinateXY{-5.0, 5.0},
                          CoordinateXY{2.0, 5.0}, CoordinateXY{8.0, 5.0},
                          CoordinateXY{15.0, 5.0}, CoordinateXY{30.0, 5.0}});

  ExpectSections2D(limiter.limit(&seq),
                   {{{-5.0, 5.0}, {2.0, 5.0}, {8.0, 5.0}, {15.0, 5.0}}});
}

TEST(LineLimiterTest, SegmentCrossingEnvelopeWithBothEndpointsOutsideIsKept) {
  Envelope env(0.0, 10.0, 0.0, 10.0);
  LineLimiter limiter(&env);

  CoordinateSequence seq({CoordinateXY{-10.0, 5.0}, CoordinateXY{-5.0, 5.0},
                          CoordinateXY{15.0, 5.0}, CoordinateXY{25.0, 5.0}});

  ExpectSections2D(limiter.limit(&seq), {{{-5.0, 5.0}, {15.0, 5.0}}});
}

TEST(LineLimiterTest, DiagonalSegmentUsesEnvelopeIntersectionTest) {
  Envelope env(0.0, 10.0, 0.0, 10.0);
  LineLimiter limiter(&env);

  // Segment (-1, 20)->(15, 9) passes above (10, 10) geometrically (y = 12.4375
  // at x = 10), but its segment envelope [-1, 15] x [9, 20] intersects env.
  CoordinateSequence seq({CoordinateXY{-1.0, 20.0}, CoordinateXY{15.0, 9.0}});

  ExpectSections2D(limiter.limit(&seq), {{{-1.0, 20.0}, {15.0, 9.0}}});
}

TEST(LineLimiterTest, MultipleReentriesSplitIntoSeparateSections) {
  Envelope env(0.0, 10.0, 0.0, 10.0);
  LineLimiter limiter(&env);

  // Two separate crossings separated by outside disjoint segments, ending with
  // an inside vertex (so finishSection runs with lastOutside == nullptr).
  CoordinateSequence seq({CoordinateXY{-5.0, 2.0}, CoordinateXY{5.0, 2.0},
                          CoordinateXY{15.0, 2.0}, CoordinateXY{25.0, 2.0},
                          CoordinateXY{25.0, 8.0}, CoordinateXY{15.0, 8.0},
                          CoordinateXY{5.0, 8.0}});

  ExpectSections2D(limiter.limit(&seq), {{{-5.0, 2.0}, {5.0, 2.0}, {15.0, 2.0}},
                                         {{15.0, 8.0}, {5.0, 8.0}}});
}

TEST(LineLimiterTest, ImmediateReentryAtSingleOutsideVertexStaysOneSection) {
  Envelope env(0.0, 10.0, 0.0, 10.0);
  LineLimiter limiter(&env);

  CoordinateSequence seq({CoordinateXY{5.0, 2.0}, CoordinateXY{15.0, 5.0},
                          CoordinateXY{5.0, 8.0}});

  ExpectSections2D(limiter.limit(&seq),
                   {{{5.0, 2.0}, {15.0, 5.0}, {5.0, 8.0}}});
}

TEST(LineLimiterTest, BoundaryTouchAndPointSizedEnvelope) {
  Envelope env(0.0, 10.0, 0.0, 10.0);
  LineLimiter limiter(&env);

  // Vertices on envelope edges and corners intersect the envelope.
  CoordinateSequence boundary_touch(
      {CoordinateXY{-5.0, 0.0}, CoordinateXY{0.0, 0.0},
       CoordinateXY{10.0, 10.0}, CoordinateXY{15.0, 10.0}});
  ExpectSections2D(limiter.limit(&boundary_touch),
                   {{{-5.0, 0.0}, {0.0, 0.0}, {10.0, 10.0}, {15.0, 10.0}}});

  // Degenerate point-sized envelope [5, 5] x [5, 5].
  Envelope point_env(5.0, 5.0, 5.0, 5.0);
  LineLimiter point_limiter(&point_env);
  CoordinateSequence crossing({CoordinateXY{0.0, 5.0}, CoordinateXY{5.0, 5.0},
                               CoordinateXY{10.0, 5.0}});
  ExpectSections2D(point_limiter.limit(&crossing),
                   {{{0.0, 5.0}, {5.0, 5.0}, {10.0, 5.0}}});
}

TEST(LineLimiterTest, DeduplicatesRepeatedConsecutivePoints) {
  Envelope env(0.0, 10.0, 0.0, 10.0);
  LineLimiter limiter(&env);

  CoordinateSequence seq({CoordinateXY{2.0, 2.0}, CoordinateXY{2.0, 2.0},
                          CoordinateXY{5.0, 5.0}, CoordinateXY{5.0, 5.0},
                          CoordinateXY{15.0, 5.0}, CoordinateXY{15.0, 5.0}});

  ExpectSections2D(limiter.limit(&seq),
                   {{{2.0, 2.0}, {5.0, 5.0}, {15.0, 5.0}}});
}

TEST(LineLimiterTest, ReusesLimiterAcrossCallsAndPreservesZ) {
  Envelope env(0.0, 10.0, 0.0, 10.0);
  LineLimiter limiter(&env);

  CoordinateSequence xyz_seq({Coordinate{-5.0, 5.0, 10.5},
                              Coordinate{5.0, 5.0, 20.5},
                              Coordinate{15.0, 5.0, 30.5}});
  const auto& sections_first = limiter.limit(&xyz_seq);
  ASSERT_EQ(sections_first.size(), 1u);
  EXPECT_TRUE(sections_first[0]->hasZ());
  EXPECT_FALSE(sections_first[0]->hasM());
  ASSERT_EQ(sections_first[0]->size(), 3u);
  EXPECT_DOUBLE_EQ(sections_first[0]->getAt(0).z, 10.5);
  EXPECT_DOUBLE_EQ(sections_first[0]->getAt(1).z, 20.5);
  EXPECT_DOUBLE_EQ(sections_first[0]->getAt(2).z, 30.5);

  CoordinateSequence xy_seq({CoordinateXY{1.0, 1.0}, CoordinateXY{2.0, 2.0}});
  const auto& sections_second = limiter.limit(&xy_seq);
  ExpectSections2D(sections_second, {{{1.0, 1.0}, {2.0, 2.0}}});
  EXPECT_FALSE(sections_second[0]->hasZ());
  EXPECT_TRUE(std::isnan(sections_second[0]->getAt(0).z));

  CoordinateSequence empty_seq = CoordinateSequence::XY(0);
  ExpectSections2D(limiter.limit(&empty_seq), {});
}

}  // namespace
}  // namespace geos_google
