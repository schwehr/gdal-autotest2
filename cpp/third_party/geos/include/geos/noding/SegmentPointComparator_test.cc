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

// Unit tests for third_party/geos/include/geos/noding/SegmentPointComparator.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/noding/SegmentPointComparator.h

#include "third_party/geos/include/geos/noding/SegmentPointComparator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <type_traits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/noding/BasicSegmentString.h"
#include "third_party/geos/include/geos/noding/Octant.h"
#include "third_party/geos/include/geos/noding/SegmentNode.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::noding::BasicSegmentString;
using geos::noding::Octant;
using geos::noding::SegmentNode;
using geos::noding::SegmentPointComparator;

static_assert(
    std::is_same_v<decltype(&SegmentPointComparator::compare),
                   int (*)(int, const Coordinate&, const Coordinate&)>);
static_assert(std::is_same_v<decltype(&SegmentPointComparator::relativeSign),
                             int (*)(double, double)>);
static_assert(std::is_same_v<decltype(&SegmentPointComparator::compareValue),
                             int (*)(int, int)>);

struct RelativeSignCase {
  double x0;
  double x1;
  int expected;
};

struct CompareValueCase {
  int sign0;
  int sign1;
  int expected;
};

struct OctantSegmentSpec {
  int octant;
  double x0;
  double y0;
  double x1;
  double y1;
};

struct OctantTieCase {
  int octant;
  // Two points sharing the dominant-axis coordinate so ordering falls back to
  // the minor axis: `first` precedes `second` along the octant direction.
  double first_x;
  double first_y;
  double second_x;
  double second_y;
};

constexpr auto kOctantSegments = std::to_array<OctantSegmentSpec>({
    {.octant = 0, .x0 = 1.0, .y0 = 2.0, .x1 = 11.0, .y1 = 6.0},
    {.octant = 1, .x0 = 1.0, .y0 = 2.0, .x1 = 5.0, .y1 = 12.0},
    {.octant = 2, .x0 = 1.0, .y0 = 2.0, .x1 = -3.0, .y1 = 12.0},
    {.octant = 3, .x0 = 1.0, .y0 = 2.0, .x1 = -9.0, .y1 = 6.0},
    {.octant = 4, .x0 = 1.0, .y0 = 2.0, .x1 = -9.0, .y1 = -2.0},
    {.octant = 5, .x0 = 1.0, .y0 = 2.0, .x1 = -3.0, .y1 = -8.0},
    {.octant = 6, .x0 = 1.0, .y0 = 2.0, .x1 = 5.0, .y1 = -8.0},
    {.octant = 7, .x0 = 1.0, .y0 = 2.0, .x1 = 11.0, .y1 = -2.0},
});

constexpr auto kOctantTieCases = std::to_array<OctantTieCase>({
    // Octant 0: +x dominant, +y minor.
    {.octant = 0,
     .first_x = 5.0,
     .first_y = 1.0,
     .second_x = 5.0,
     .second_y = 3.0},
    // Octant 1: +y dominant, +x minor.
    {.octant = 1,
     .first_x = 1.0,
     .first_y = 5.0,
     .second_x = 3.0,
     .second_y = 5.0},
    // Octant 2: +y dominant, -x minor.
    {.octant = 2,
     .first_x = -1.0,
     .first_y = 5.0,
     .second_x = -3.0,
     .second_y = 5.0},
    // Octant 3: -x dominant, +y minor.
    {.octant = 3,
     .first_x = -5.0,
     .first_y = 1.0,
     .second_x = -5.0,
     .second_y = 3.0},
    // Octant 4: -x dominant, -y minor.
    {.octant = 4,
     .first_x = -5.0,
     .first_y = -1.0,
     .second_x = -5.0,
     .second_y = -3.0},
    // Octant 5: -y dominant, -x minor.
    {.octant = 5,
     .first_x = -1.0,
     .first_y = -5.0,
     .second_x = -3.0,
     .second_y = -5.0},
    // Octant 6: -y dominant, +x minor.
    {.octant = 6,
     .first_x = 1.0,
     .first_y = -5.0,
     .second_x = 3.0,
     .second_y = -5.0},
    // Octant 7: +x dominant, -y minor.
    {.octant = 7,
     .first_x = 5.0,
     .first_y = -1.0,
     .second_x = 5.0,
     .second_y = -3.0},
});

Coordinate Lerp(const OctantSegmentSpec& seg, double t) {
  return Coordinate(seg.x0 + t * (seg.x1 - seg.x0),
                    seg.y0 + t * (seg.y1 - seg.y0));
}

TEST(SegmentPointComparatorTest, RelativeSignOrderingAndSpecialDoubles) {
  constexpr double inf = std::numeric_limits<double>::infinity();
  constexpr double nan = std::numeric_limits<double>::quiet_NaN();
  constexpr double min_subnormal = std::numeric_limits<double>::denorm_min();

  constexpr auto kCases = std::to_array<RelativeSignCase>({
      {.x0 = -10.0, .x1 = -2.0, .expected = -1},
      {.x0 = -2.0, .x1 = -10.0, .expected = 1},
      {.x0 = 3.5, .x1 = 3.5, .expected = 0},
      {.x0 = -0.0, .x1 = 0.0, .expected = 0},
      {.x0 = 0.0, .x1 = -0.0, .expected = 0},
      {.x0 = -0.0, .x1 = min_subnormal, .expected = -1},
      {.x0 = -0.0, .x1 = -min_subnormal, .expected = 1},
      {.x0 = -inf, .x1 = 0.0, .expected = -1},
      {.x0 = inf, .x1 = 0.0, .expected = 1},
      {.x0 = -inf, .x1 = inf, .expected = -1},
      {.x0 = inf, .x1 = inf, .expected = 0},
      {.x0 = -inf, .x1 = -inf, .expected = 0},
      {.x0 = nan, .x1 = 0.0, .expected = 0},
      {.x0 = 0.0, .x1 = nan, .expected = 0},
      {.x0 = nan, .x1 = nan, .expected = 0},
  });

  for (const RelativeSignCase& tc : kCases) {
    EXPECT_EQ(SegmentPointComparator::relativeSign(tc.x0, tc.x1), tc.expected);
  }

  const double one_plus_ulp = std::nextafter(1.0, 2.0);
  EXPECT_EQ(SegmentPointComparator::relativeSign(1.0, one_plus_ulp), -1);
  EXPECT_EQ(SegmentPointComparator::relativeSign(one_plus_ulp, 1.0), 1);
}

TEST(SegmentPointComparatorTest, CompareValueLexicographicSignPairs) {
  constexpr int int_min = std::numeric_limits<int>::min();
  constexpr int int_max = std::numeric_limits<int>::max();

  constexpr auto kCases = std::to_array<CompareValueCase>({
      // Exhaustive 3x3 matrix for {-1, 0, 1}.
      {.sign0 = -1, .sign1 = -1, .expected = -1},
      {.sign0 = -1, .sign1 = 0, .expected = -1},
      {.sign0 = -1, .sign1 = 1, .expected = -1},
      {.sign0 = 0, .sign1 = -1, .expected = -1},
      {.sign0 = 0, .sign1 = 0, .expected = 0},
      {.sign0 = 0, .sign1 = 1, .expected = 1},
      {.sign0 = 1, .sign1 = -1, .expected = 1},
      {.sign0 = 1, .sign1 = 0, .expected = 1},
      {.sign0 = 1, .sign1 = 1, .expected = 1},
      // Arbitrary signed magnitudes.
      {.sign0 = -42, .sign1 = 99, .expected = -1},
      {.sign0 = 42, .sign1 = -99, .expected = 1},
      {.sign0 = 0, .sign1 = int_min, .expected = -1},
      {.sign0 = 0, .sign1 = int_max, .expected = 1},
  });

  for (const CompareValueCase& tc : kCases) {
    SCOPED_TRACE(tc.sign0);
    SCOPED_TRACE(tc.sign1);
    EXPECT_EQ(SegmentPointComparator::compareValue(tc.sign0, tc.sign1),
              tc.expected);
  }
}

TEST(SegmentPointComparatorTest, CompareAllEightOctantsAlongSegments) {
  for (const OctantSegmentSpec& seg : kOctantSegments) {
    SCOPED_TRACE(seg.octant);
    const Coordinate start(seg.x0, seg.y0);
    const Coordinate end(seg.x1, seg.y1);
    ASSERT_EQ(Octant::octant(start, end), seg.octant);

    const Coordinate early = Lerp(seg, 0.25);
    const Coordinate mid = Lerp(seg, 0.50);
    const Coordinate late = Lerp(seg, 0.75);

    EXPECT_EQ(SegmentPointComparator::compare(seg.octant, early, late), -1);
    EXPECT_EQ(SegmentPointComparator::compare(seg.octant, late, early), 1);
    EXPECT_EQ(SegmentPointComparator::compare(seg.octant, mid, mid), 0);
    EXPECT_EQ(SegmentPointComparator::compare(seg.octant, start, end), -1);
    EXPECT_EQ(SegmentPointComparator::compare(seg.octant, end, start), 1);
  }
}

TEST(SegmentPointComparatorTest, CompareTieBreakingOnDominantAxisAllOctants) {
  for (const OctantTieCase& tc : kOctantTieCases) {
    SCOPED_TRACE(tc.octant);
    const Coordinate first(tc.first_x, tc.first_y);
    const Coordinate second(tc.second_x, tc.second_y);

    EXPECT_EQ(SegmentPointComparator::compare(tc.octant, first, second), -1);
    EXPECT_EQ(SegmentPointComparator::compare(tc.octant, second, first), 1);
  }
}

TEST(SegmentPointComparatorTest, CompareAxisAlignedAndDiagonalBoundaries) {
  constexpr auto kBoundarySegments = std::to_array<OctantSegmentSpec>({
      // Coordinate axes.
      {.octant = 0, .x0 = 0.0, .y0 = 0.0, .x1 = 10.0, .y1 = 0.0},
      {.octant = 1, .x0 = 0.0, .y0 = 0.0, .x1 = 0.0, .y1 = 10.0},
      {.octant = 3, .x0 = 0.0, .y0 = 0.0, .x1 = -10.0, .y1 = 0.0},
      {.octant = 6, .x0 = 0.0, .y0 = 0.0, .x1 = 0.0, .y1 = -10.0},
      // 45-degree diagonals.
      {.octant = 0, .x0 = 0.0, .y0 = 0.0, .x1 = 10.0, .y1 = 10.0},
      {.octant = 3, .x0 = 0.0, .y0 = 0.0, .x1 = -10.0, .y1 = 10.0},
      {.octant = 4, .x0 = 0.0, .y0 = 0.0, .x1 = -10.0, .y1 = -10.0},
      {.octant = 7, .x0 = 0.0, .y0 = 0.0, .x1 = 10.0, .y1 = -10.0},
  });

  for (const OctantSegmentSpec& seg : kBoundarySegments) {
    SCOPED_TRACE(seg.octant);
    ASSERT_EQ(
        Octant::octant(Coordinate(seg.x0, seg.y0), Coordinate(seg.x1, seg.y1)),
        seg.octant);
    const Coordinate p_early = Lerp(seg, 0.2);
    const Coordinate p_late = Lerp(seg, 0.8);
    EXPECT_EQ(SegmentPointComparator::compare(seg.octant, p_early, p_late), -1);
    EXPECT_EQ(SegmentPointComparator::compare(seg.octant, p_late, p_early), 1);
  }
}

TEST(SegmentPointComparatorTest,
     CompareEqual2DIgnoresZSignedZeroAndShortCircuits) {
  const Coordinate p_z1(3.0, -4.0, 100.0);
  const Coordinate p_z2(3.0, -4.0, -50.0);
  const Coordinate p_znan(3.0, -4.0);
  const Coordinate p_neg_zero(-0.0, 5.0);
  const Coordinate p_pos_zero(0.0, 5.0);

  for (int octant = 0; octant < 8; ++octant) {
    SCOPED_TRACE(octant);
    EXPECT_EQ(SegmentPointComparator::compare(octant, p_z1, p_z2), 0);
    EXPECT_EQ(SegmentPointComparator::compare(octant, p_z1, p_znan), 0);
    EXPECT_EQ(SegmentPointComparator::compare(octant, p_neg_zero, p_pos_zero),
              0);
  }

  // Equal 2D coordinates return early before inspecting octant.
  EXPECT_EQ(SegmentPointComparator::compare(-1, p_z1, p_z2), 0);
  EXPECT_EQ(SegmentPointComparator::compare(8, p_z1, p_z2), 0);
}

TEST(SegmentPointComparatorTest, CompareHandlesNonFiniteCoordinates) {
  constexpr double inf = std::numeric_limits<double>::infinity();
  constexpr double nan = std::numeric_limits<double>::quiet_NaN();

  // NaN causes equals2D() to return false, yet both relativeSign() calls yield
  // 0 so compareValue(0, 0) returns 0 for all valid octants.
  const Coordinate p_nan_x(nan, 1.0);
  const Coordinate p_finite(0.0, 1.0);
  for (int octant = 0; octant < 8; ++octant) {
    SCOPED_TRACE(octant);
    EXPECT_EQ(SegmentPointComparator::compare(octant, p_nan_x, p_finite), 0);
    EXPECT_EQ(SegmentPointComparator::compare(octant, p_finite, p_nan_x), 0);
  }

  EXPECT_EQ(SegmentPointComparator::compare(0, Coordinate(0.0, 0.0),
                                            Coordinate(inf, 0.0)),
            -1);
  EXPECT_EQ(SegmentPointComparator::compare(4, Coordinate(0.0, 0.0),
                                            Coordinate(-inf, 0.0)),
            -1);
  EXPECT_EQ(SegmentPointComparator::compare(4, Coordinate(0.0, 0.0),
                                            Coordinate(inf, 0.0)),
            1);
}

TEST(SegmentPointComparatorTest, SortsPointsAlongAllOctantsAndSegmentNode) {
  for (const OctantSegmentSpec& seg : kOctantSegments) {
    SCOPED_TRACE(seg.octant);
    std::vector<Coordinate> expected = {
        Lerp(seg, 0.0),   Lerp(seg, 0.125), Lerp(seg, 0.375),
        Lerp(seg, 0.625), Lerp(seg, 0.875), Lerp(seg, 1.0),
    };
    std::vector<Coordinate> scrambled = {
        expected[4], expected[1], expected[5],
        expected[0], expected[3], expected[2],
    };

    const int octant = seg.octant;
    std::sort(scrambled.begin(), scrambled.end(),
              [octant](const Coordinate& a, const Coordinate& b) {
                return SegmentPointComparator::compare(octant, a, b) < 0;
              });

    ASSERT_EQ(scrambled.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
      EXPECT_TRUE(scrambled[i].equals2D(expected[i])) << "index " << i;
    }
  }

  // Verify SegmentNode::compareTo ordering on interior nodes delegates
  // consistently to SegmentPointComparator::compare.
  for (int octant : {0, 2, 5, 7}) {
    SCOPED_TRACE(octant);
    const OctantSegmentSpec& seg = kOctantSegments[octant];
    CoordinateSequence seq(
        {Coordinate(seg.x0, seg.y0), Coordinate(seg.x1, seg.y1)});
    BasicSegmentString ss(&seq, nullptr);
    const int computed_octant = ss.getSegmentOctant(0);
    ASSERT_EQ(computed_octant, octant);

    const Coordinate early = Lerp(seg, 0.25);
    const Coordinate late = Lerp(seg, 0.75);
    SegmentNode node_early(ss, early, 0, computed_octant);
    SegmentNode node_late(ss, late, 0, computed_octant);
    ASSERT_TRUE(node_early.isInterior());
    ASSERT_TRUE(node_late.isInterior());

    EXPECT_EQ(node_early.compareTo(node_late), -1);
    EXPECT_EQ(node_late.compareTo(node_early), 1);
    EXPECT_EQ(node_early.compareTo(node_early), 0);
  }
}

}  // namespace
}  // namespace geos_google
