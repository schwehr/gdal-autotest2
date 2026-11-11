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

// Unit tests for third_party/geos/src/coverage/CoverageRing.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/coverage/CoverageRing.cpp

#include "third_party/geos/include/geos/coverage/CoverageRing.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"

namespace geos_google {
namespace {

using geos::coverage::CoverageRing;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Envelope;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using ::testing::IsEmpty;
using ::testing::SizeIs;

struct Pt2D {
  double x;
  double y;
};

struct Pt3D {
  double x;
  double y;
  double z;
};

constexpr std::array<Pt2D, 5> kCcwSquarePts{{
    {0.0, 0.0},
    {10.0, 0.0},
    {10.0, 10.0},
    {0.0, 10.0},
    {0.0, 0.0},
}};

constexpr std::array<Pt2D, 7> kCcwHexagonPts{{
    {2.0, 0.0},
    {6.0, 0.0},
    {8.0, 3.0},
    {6.0, 6.0},
    {2.0, 6.0},
    {0.0, 3.0},
    {2.0, 0.0},
}};

std::unique_ptr<CoordinateSequence> MakeSequence(std::span<const Pt2D> pts) {
  auto seq = std::make_unique<CoordinateSequence>();
  seq->reserve(pts.size());
  for (const Pt2D& p : pts) {
    seq->add(p.x, p.y);
  }
  return seq;
}

std::unique_ptr<CoordinateSequence> MakeSequence3D(std::span<const Pt3D> pts) {
  auto seq = std::make_unique<CoordinateSequence>(0, 3);
  seq->reserve(pts.size());
  for (const Pt3D& p : pts) {
    seq->add(Coordinate(p.x, p.y, p.z));
  }
  return seq;
}

void ExpectInvalidLinesMatch(
    const CoverageRing& ring, const GeometryFactory* expected_factory,
    const std::vector<std::unique_ptr<LineString>>& lines,
    const std::vector<std::vector<std::size_t>>& expected_vertex_indices) {
  ASSERT_EQ(lines.size(), expected_vertex_indices.size());
  for (std::size_t i = 0; i < lines.size(); ++i) {
    SCOPED_TRACE(::testing::Message() << "line index " << i);
    const std::unique_ptr<LineString>& line = lines[i];
    ASSERT_NE(line, nullptr);
    EXPECT_EQ(line->getFactory(), expected_factory);
    const CoordinateSequence* coords = line->getCoordinatesRO();
    ASSERT_NE(coords, nullptr);
    const std::vector<std::size_t>& expected_indices =
        expected_vertex_indices[i];
    ASSERT_EQ(coords->size(), expected_indices.size());
    for (std::size_t j = 0; j < expected_indices.size(); ++j) {
      const Coordinate& expected = ring.getCoordinate(expected_indices[j]);
      EXPECT_DOUBLE_EQ(coords->getX(j), expected.x);
      EXPECT_DOUBLE_EQ(coords->getY(j), expected.y);
      if (std::isnan(expected.z)) {
        EXPECT_TRUE(std::isnan(coords->getAt(j).z));
      } else {
        EXPECT_DOUBLE_EQ(coords->getAt(j).z, expected.z);
      }
    }
  }
}

TEST(CoverageRingTest, ConstructFromCoordinateSequence) {
  std::unique_ptr<CoordinateSequence> seq = MakeSequence(kCcwSquarePts);

  CoverageRing right_ring(seq.get(), true);
  EXPECT_TRUE(right_ring.isInteriorOnRight());
  EXPECT_EQ(right_ring.size(), 5U);
  EXPECT_TRUE(right_ring.isClosed());
  EXPECT_EQ(right_ring.getCoordinates(), seq.get());
  EXPECT_EQ(right_ring.getData(), nullptr);
  EXPECT_FALSE(right_ring.hasInvalid());
  EXPECT_FALSE(right_ring.isInvalid());
  EXPECT_FALSE(right_ring.isKnown());
  for (std::size_t i = 0; i < right_ring.size() - 1; ++i) {
    EXPECT_FALSE(right_ring.isInvalid(i));
    EXPECT_FALSE(right_ring.isKnown(i));
  }

  CoverageRing left_ring(seq.get(), false);
  EXPECT_FALSE(left_ring.isInteriorOnRight());
}

TEST(CoverageRingTest, ConstructFromDegenerateShortSequences) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Single-coordinate sequence yields zero segments.
  constexpr std::array<Pt2D, 1> kSinglePt{{{1.0, 2.0}}};
  std::unique_ptr<CoordinateSequence> single_seq = MakeSequence(kSinglePt);
  CoverageRing zero_seg_ring(single_seq.get(), true);
  EXPECT_EQ(zero_seg_ring.size(), 1U);
  EXPECT_TRUE(zero_seg_ring.isInteriorOnRight());
  EXPECT_FALSE(zero_seg_ring.hasInvalid());
  EXPECT_TRUE(zero_seg_ring.isInvalid());
  EXPECT_TRUE(zero_seg_ring.isKnown());
  std::vector<std::unique_ptr<LineString>> lines;
  zero_seg_ring.createInvalidLines(factory.get(), lines);
  EXPECT_THAT(lines, IsEmpty());

  // Two-coordinate sequence yields one segment.
  constexpr std::array<Pt2D, 2> kTwoPts{{{1.0, 2.0}, {4.0, 6.0}}};
  std::unique_ptr<CoordinateSequence> two_seq = MakeSequence(kTwoPts);
  CoverageRing one_seg_ring(two_seq.get(), false);
  EXPECT_EQ(one_seg_ring.size(), 2U);
  EXPECT_EQ(one_seg_ring.prev(0), 0U);
  EXPECT_EQ(one_seg_ring.next(0), 0U);
  one_seg_ring.markInvalid(0);
  EXPECT_TRUE(one_seg_ring.hasInvalid());
  EXPECT_TRUE(one_seg_ring.isInvalid());
  one_seg_ring.createInvalidLines(factory.get(), lines);
  ExpectInvalidLinesMatch(one_seg_ring, factory.get(), lines, {{0, 1}});
}

TEST(CoverageRingTest, ConstructFromLinearRingOrientation) {
  struct SquareOrientationCase {
    std::string_view name;
    std::array<Pt2D, 5> pts;
    bool is_shell;
    bool expected_interior_on_right;
  };

  constexpr auto kSquareCases = std::to_array<SquareOrientationCase>({
      {
          .name = "cw_shell_has_interior_on_right",
          .pts = {{{0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}}},
          .is_shell = true,
          .expected_interior_on_right = true,
      },
      {
          .name = "ccw_shell_has_interior_on_left",
          .pts = kCcwSquarePts,
          .is_shell = true,
          .expected_interior_on_right = false,
      },
      {
          .name = "ccw_hole_has_interior_on_right",
          .pts = kCcwSquarePts,
          .is_shell = false,
          .expected_interior_on_right = true,
      },
      {
          .name = "cw_hole_has_interior_on_left",
          .pts = {{{0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}}},
          .is_shell = false,
          .expected_interior_on_right = false,
      },
  });

  GeometryFactory::Ptr factory = GeometryFactory::create();
  for (const SquareOrientationCase& tc : kSquareCases) {
    SCOPED_TRACE(tc.name);
    std::unique_ptr<LinearRing> linear_ring =
        factory->createLinearRing(MakeSequence(tc.pts));
    CoverageRing ring(linear_ring.get(), tc.is_shell);
    EXPECT_EQ(ring.isInteriorOnRight(), tc.expected_interior_on_right);
    EXPECT_EQ(ring.size(), 5U);
    EXPECT_EQ(ring.getCoordinates(), linear_ring->getCoordinatesRO());
  }

  // Verify pointed-cap triangles (Orientation::isCCW cap-point branch).
  struct TriangleOrientationCase {
    std::string_view name;
    std::array<Pt2D, 4> pts;
    bool is_shell;
    bool expected_interior_on_right;
  };

  constexpr auto kTriangleCases = std::to_array<TriangleOrientationCase>({
      {
          .name = "cw_triangle_shell",
          .pts = {{{0, 0}, {5, 10}, {10, 0}, {0, 0}}},
          .is_shell = true,
          .expected_interior_on_right = true,
      },
      {
          .name = "ccw_triangle_shell",
          .pts = {{{0, 0}, {10, 0}, {5, 10}, {0, 0}}},
          .is_shell = true,
          .expected_interior_on_right = false,
      },
      {
          .name = "ccw_triangle_hole",
          .pts = {{{0, 0}, {10, 0}, {5, 10}, {0, 0}}},
          .is_shell = false,
          .expected_interior_on_right = true,
      },
      {
          .name = "cw_triangle_hole",
          .pts = {{{0, 0}, {5, 10}, {10, 0}, {0, 0}}},
          .is_shell = false,
          .expected_interior_on_right = false,
      },
  });

  for (const TriangleOrientationCase& tc : kTriangleCases) {
    SCOPED_TRACE(tc.name);
    std::unique_ptr<LinearRing> linear_ring =
        factory->createLinearRing(MakeSequence(tc.pts));
    CoverageRing ring(linear_ring.get(), tc.is_shell);
    EXPECT_EQ(ring.isInteriorOnRight(), tc.expected_interior_on_right);
    EXPECT_EQ(ring.size(), 4U);
  }
}

TEST(CoverageRingTest, GetEnvelopeCoversHalfOpenRange) {
  constexpr std::array<Pt2D, 5> kQuadPts{{
      {0.0, 0.0},
      {10.0, 2.0},
      {5.0, 8.0},
      {-3.0, 4.0},
      {0.0, 0.0},
  }};
  std::unique_ptr<CoordinateSequence> seq = MakeSequence(kQuadPts);
  CoverageRing ring(seq.get(), true);

  Envelope empty_env = ring.getEnvelope(2, 2);
  EXPECT_TRUE(empty_env.isNull());

  Envelope inverted_env = ring.getEnvelope(3, 1);
  EXPECT_TRUE(inverted_env.isNull());

  Envelope single_env = ring.getEnvelope(2, 3);
  ASSERT_FALSE(single_env.isNull());
  EXPECT_DOUBLE_EQ(single_env.getMinX(), 5.0);
  EXPECT_DOUBLE_EQ(single_env.getMaxX(), 5.0);
  EXPECT_DOUBLE_EQ(single_env.getMinY(), 8.0);
  EXPECT_DOUBLE_EQ(single_env.getMaxY(), 8.0);

  Envelope sub_env = ring.getEnvelope(1, 3);
  ASSERT_FALSE(sub_env.isNull());
  EXPECT_DOUBLE_EQ(sub_env.getMinX(), 5.0);
  EXPECT_DOUBLE_EQ(sub_env.getMaxX(), 10.0);
  EXPECT_DOUBLE_EQ(sub_env.getMinY(), 2.0);
  EXPECT_DOUBLE_EQ(sub_env.getMaxY(), 8.0);

  Envelope full_env = ring.getEnvelope(0, ring.size());
  ASSERT_FALSE(full_env.isNull());
  EXPECT_DOUBLE_EQ(full_env.getMinX(), -3.0);
  EXPECT_DOUBLE_EQ(full_env.getMaxX(), 10.0);
  EXPECT_DOUBLE_EQ(full_env.getMinY(), 0.0);
  EXPECT_DOUBLE_EQ(full_env.getMaxY(), 8.0);
}

TEST(CoverageRingTest, SegmentStatusAndKnownState) {
  std::unique_ptr<CoordinateSequence> seq = MakeSequence(kCcwSquarePts);

  // Exercise all four combinations of (matched, invalid) on a single ring.
  CoverageRing ring(seq.get(), false);
  ring.markMatched(1);
  ring.markInvalid(2);
  ring.markMatched(3);
  ring.markInvalid(3);

  EXPECT_FALSE(ring.isKnown(0));
  EXPECT_FALSE(ring.isInvalid(0));

  EXPECT_TRUE(ring.isKnown(1));
  EXPECT_FALSE(ring.isInvalid(1));

  EXPECT_TRUE(ring.isKnown(2));
  EXPECT_TRUE(ring.isInvalid(2));

  EXPECT_TRUE(ring.isKnown(3));
  EXPECT_TRUE(ring.isInvalid(3));

  EXPECT_TRUE(ring.hasInvalid());
  EXPECT_FALSE(ring.isInvalid());
  // CoverageRing::isKnown() requires both m_isMatched[i] and m_isInvalid[i].
  EXPECT_FALSE(ring.isKnown());

  // Mark only matched on all segments: isKnown(i) is true, but ring isKnown()
  // and hasInvalid()/isInvalid() remain false.
  CoverageRing matched_only_ring(seq.get(), true);
  for (std::size_t i = 0; i < 4; ++i) {
    matched_only_ring.markMatched(i);
  }
  EXPECT_FALSE(matched_only_ring.hasInvalid());
  EXPECT_FALSE(matched_only_ring.isInvalid());
  EXPECT_FALSE(matched_only_ring.isKnown());

  // Mark only invalid on all segments: isInvalid() is true, isKnown() is false.
  CoverageRing invalid_only_ring(seq.get(), true);
  for (std::size_t i = 0; i < 4; ++i) {
    invalid_only_ring.markInvalid(i);
  }
  EXPECT_TRUE(invalid_only_ring.hasInvalid());
  EXPECT_TRUE(invalid_only_ring.isInvalid());
  EXPECT_FALSE(invalid_only_ring.isKnown());

  // Both matched and invalid on all segments except the last, then complete.
  CoverageRing partial_then_full_ring(seq.get(), true);
  for (std::size_t i = 0; i < 3; ++i) {
    partial_then_full_ring.markMatched(i);
    partial_then_full_ring.markInvalid(i);
  }
  partial_then_full_ring.markMatched(3);
  EXPECT_FALSE(partial_then_full_ring.isKnown());
  partial_then_full_ring.markInvalid(3);
  EXPECT_TRUE(partial_then_full_ring.isKnown());
  EXPECT_TRUE(partial_then_full_ring.isInvalid());
  EXPECT_TRUE(partial_then_full_ring.hasInvalid());
}

TEST(CoverageRingTest, StaticIsKnownAcrossVector) {
  std::vector<CoverageRing*> empty_rings;
  EXPECT_TRUE(CoverageRing::isKnown(empty_rings));

  std::unique_ptr<CoordinateSequence> seq = MakeSequence(kCcwSquarePts);
  CoverageRing known_ring_1(seq.get(), true);
  CoverageRing known_ring_2(seq.get(), false);
  CoverageRing unknown_ring(seq.get(), true);
  for (std::size_t i = 0; i < 4; ++i) {
    known_ring_1.markMatched(i);
    known_ring_1.markInvalid(i);
    known_ring_2.markMatched(i);
    known_ring_2.markInvalid(i);
  }
  unknown_ring.markMatched(0);

  std::vector<CoverageRing*> only_unknown{&unknown_ring};
  EXPECT_FALSE(CoverageRing::isKnown(only_unknown));

  std::vector<CoverageRing*> mixed_rings{&known_ring_1, &unknown_ring};
  EXPECT_FALSE(CoverageRing::isKnown(mixed_rings));

  std::vector<CoverageRing*> all_known{&known_ring_1, &known_ring_2};
  EXPECT_TRUE(CoverageRing::isKnown(all_known));
}

TEST(CoverageRingTest, PrevAndNextWrapAround) {
  constexpr std::array<Pt2D, 4> kTrianglePts{{
      {0.0, 0.0},
      {5.0, 0.0},
      {2.0, 4.0},
      {0.0, 0.0},
  }};
  std::unique_ptr<CoordinateSequence> tri_seq = MakeSequence(kTrianglePts);
  CoverageRing tri_ring(tri_seq.get(), true);
  EXPECT_EQ(tri_ring.prev(0), 2U);
  EXPECT_EQ(tri_ring.prev(1), 0U);
  EXPECT_EQ(tri_ring.prev(2), 1U);
  EXPECT_EQ(tri_ring.next(0), 1U);
  EXPECT_EQ(tri_ring.next(1), 2U);
  EXPECT_EQ(tri_ring.next(2), 0U);
  EXPECT_EQ(tri_ring.next(3), 0U);

  std::unique_ptr<CoordinateSequence> sq_seq = MakeSequence(kCcwSquarePts);
  CoverageRing sq_ring(sq_seq.get(), false);
  EXPECT_EQ(sq_ring.prev(0), 3U);
  EXPECT_EQ(sq_ring.prev(1), 0U);
  EXPECT_EQ(sq_ring.prev(2), 1U);
  EXPECT_EQ(sq_ring.prev(3), 2U);
  EXPECT_EQ(sq_ring.next(0), 1U);
  EXPECT_EQ(sq_ring.next(1), 2U);
  EXPECT_EQ(sq_ring.next(2), 3U);
  EXPECT_EQ(sq_ring.next(3), 0U);
  EXPECT_EQ(sq_ring.next(4), 0U);
}

TEST(CoverageRingTest, FindVertexPrevAndNext) {
  std::unique_ptr<CoordinateSequence> sq_seq = MakeSequence(kCcwSquarePts);
  CoverageRing sq_ring(sq_seq.get(), true);

  // Non-matching query point returns immediately without advancing.
  const Coordinate off_ring(99.0, 99.0);
  const Coordinate& no_step_prev = sq_ring.findVertexPrev(1, off_ring);
  EXPECT_EQ(&no_step_prev, &sq_ring.getCoordinate(1));
  EXPECT_TRUE(no_step_prev.equals2D(Coordinate(10.0, 0.0)));

  const Coordinate& no_step_next = sq_ring.findVertexNext(1, off_ring);
  EXPECT_EQ(&no_step_next, &sq_ring.getCoordinate(2));
  EXPECT_TRUE(no_step_next.equals2D(Coordinate(10.0, 10.0)));

  // Matching interior vertex steps once.
  EXPECT_TRUE(sq_ring.findVertexPrev(2, Coordinate(10.0, 10.0))
                  .equals2D(Coordinate(10.0, 0.0)));
  EXPECT_TRUE(sq_ring.findVertexNext(1, Coordinate(10.0, 10.0))
                  .equals2D(Coordinate(0.0, 10.0)));

  // Matching vertex at the closing seam wraps around vertex 0.
  EXPECT_TRUE(sq_ring.findVertexPrev(0, Coordinate(0.0, 0.0))
                  .equals2D(Coordinate(0.0, 10.0)));
  EXPECT_TRUE(sq_ring.findVertexNext(3, Coordinate(0.0, 0.0))
                  .equals2D(Coordinate(10.0, 0.0)));

  // Repeated consecutive vertices across interior and ring seam are skipped.
  constexpr std::array<Pt2D, 7> kRepeatedPts{{
      {0.0, 0.0},
      {0.0, 0.0},
      {10.0, 0.0},
      {10.0, 10.0},
      {10.0, 10.0},
      {0.0, 10.0},
      {0.0, 0.0},
  }};
  std::unique_ptr<CoordinateSequence> rep_seq = MakeSequence(kRepeatedPts);
  CoverageRing rep_ring(rep_seq.get(), true);

  EXPECT_TRUE(rep_ring.findVertexPrev(1, Coordinate(0.0, 0.0))
                  .equals2D(Coordinate(0.0, 10.0)));
  EXPECT_TRUE(rep_ring.findVertexNext(5, Coordinate(0.0, 0.0))
                  .equals2D(Coordinate(10.0, 0.0)));
  EXPECT_TRUE(rep_ring.findVertexPrev(4, Coordinate(10.0, 10.0))
                  .equals2D(Coordinate(10.0, 0.0)));
  EXPECT_TRUE(rep_ring.findVertexNext(2, Coordinate(10.0, 10.0))
                  .equals2D(Coordinate(0.0, 10.0)));

  // 2D equality ignores Z ordinates when skipping vertices.
  constexpr std::array<Pt3D, 5> kSquare3DPts{{
      {0.0, 0.0, 1.0},
      {10.0, 0.0, 2.0},
      {10.0, 10.0, 3.0},
      {0.0, 10.0, 4.0},
      {0.0, 0.0, 1.0},
  }};
  std::unique_ptr<CoordinateSequence> seq3d = MakeSequence3D(kSquare3DPts);
  CoverageRing ring3d(seq3d.get(), true);
  EXPECT_TRUE(ring3d.findVertexPrev(1, Coordinate(10.0, 0.0, 999.0))
                  .equals3D(Coordinate(0.0, 0.0, 1.0)));
  EXPECT_TRUE(ring3d.findVertexNext(0, Coordinate(10.0, 0.0, 999.0))
                  .equals3D(Coordinate(10.0, 10.0, 3.0)));
}

TEST(CoverageRingTest, CreateInvalidLinesSquareCases) {
  struct InvalidLinesCase {
    std::string_view name;
    std::vector<std::size_t> invalid_segments;
    std::vector<std::vector<std::size_t>> expected_vertex_chains;
  };

  const std::vector<InvalidLinesCase> cases = {
      {
          .name = "none_invalid",
          .invalid_segments = {},
          .expected_vertex_chains = {},
      },
      {
          .name = "all_invalid",
          .invalid_segments = {0, 1, 2, 3},
          .expected_vertex_chains = {{0, 1, 2, 3, 4}},
      },
      {
          .name = "first_segment_only",
          .invalid_segments = {0},
          .expected_vertex_chains = {{0, 1}},
      },
      {
          .name = "middle_segment_only",
          .invalid_segments = {1},
          .expected_vertex_chains = {{1, 2}},
      },
      {
          .name = "last_segment_only_wraps_to_zero",
          .invalid_segments = {3},
          .expected_vertex_chains = {{3, 0}},
      },
      {
          .name = "contiguous_middle",
          .invalid_segments = {1, 2},
          .expected_vertex_chains = {{1, 2, 3}},
      },
      {
          .name = "first_three_segments",
          .invalid_segments = {0, 1, 2},
          .expected_vertex_chains = {{0, 1, 2, 3}},
      },
      {
          .name = "wrap_across_seam",
          .invalid_segments = {3, 0},
          .expected_vertex_chains = {{3, 0, 1}},
      },
      {
          .name = "only_first_segment_valid_wraps_rest",
          .invalid_segments = {1, 2, 3},
          .expected_vertex_chains = {{1, 2, 3, 0}},
      },
      {
          .name = "disconnected_0_and_2",
          .invalid_segments = {0, 2},
          .expected_vertex_chains = {{2, 3}, {0, 1}},
      },
      {
          .name = "disconnected_1_and_3",
          .invalid_segments = {1, 3},
          .expected_vertex_chains = {{3, 0}, {1, 2}},
      },
  };

  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<CoordinateSequence> seq = MakeSequence(kCcwSquarePts);

  for (const InvalidLinesCase& tc : cases) {
    SCOPED_TRACE(tc.name);
    CoverageRing ring(seq.get(), false);
    for (std::size_t idx : tc.invalid_segments) {
      ring.markInvalid(idx);
    }
    std::vector<std::unique_ptr<LineString>> lines;
    ring.createInvalidLines(factory.get(), lines);
    ExpectInvalidLinesMatch(ring, factory.get(), lines,
                            tc.expected_vertex_chains);
  }
}

TEST(CoverageRingTest, CreateInvalidLinesHexagonAndAppending) {
  struct InvalidLinesCase {
    std::string_view name;
    std::vector<std::size_t> invalid_segments;
    std::vector<std::vector<std::size_t>> expected_vertex_chains;
  };

  const std::vector<InvalidLinesCase> cases = {
      {
          .name = "long_wrap_across_seam",
          .invalid_segments = {4, 5, 0, 1},
          .expected_vertex_chains = {{4, 5, 0, 1, 2}},
      },
      {
          .name = "two_non_wrapping_sections",
          .invalid_segments = {1, 3, 4},
          .expected_vertex_chains = {{3, 4, 5}, {1, 2}},
      },
      {
          .name = "two_sections_one_wrapping",
          .invalid_segments = {0, 2, 3, 5},
          .expected_vertex_chains = {{2, 3, 4}, {5, 0, 1}},
      },
      {
          .name = "three_non_wrapping_sections",
          .invalid_segments = {0, 2, 4},
          .expected_vertex_chains = {{2, 3}, {4, 5}, {0, 1}},
      },
      {
          .name = "three_sections_with_wrap_at_end",
          .invalid_segments = {1, 3, 5},
          .expected_vertex_chains = {{3, 4}, {5, 0}, {1, 2}},
      },
  };

  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<CoordinateSequence> hex_seq = MakeSequence(kCcwHexagonPts);

  for (const InvalidLinesCase& tc : cases) {
    SCOPED_TRACE(tc.name);
    CoverageRing ring(hex_seq.get(), true);
    for (std::size_t idx : tc.invalid_segments) {
      ring.markInvalid(idx);
    }
    std::vector<std::unique_ptr<LineString>> lines;
    ring.createInvalidLines(factory.get(), lines);
    ExpectInvalidLinesMatch(ring, factory.get(), lines,
                            tc.expected_vertex_chains);
  }

  // Verify createInvalidLines appends to existing output vector.
  CoverageRing first_ring(hex_seq.get(), true);
  first_ring.markInvalid(1);
  CoverageRing second_ring(hex_seq.get(), true);
  CoverageRing third_ring(hex_seq.get(), true);
  third_ring.markInvalid(4);

  std::vector<std::unique_ptr<LineString>> accumulated;
  first_ring.createInvalidLines(factory.get(), accumulated);
  second_ring.createInvalidLines(factory.get(), accumulated);
  third_ring.createInvalidLines(factory.get(), accumulated);
  ASSERT_THAT(accumulated, SizeIs(2));
  EXPECT_TRUE(
      accumulated[0]->getCoordinatesRO()->getAt(0).equals2D(hex_seq->getAt(1)));
  EXPECT_TRUE(
      accumulated[1]->getCoordinatesRO()->getAt(0).equals2D(hex_seq->getAt(4)));
}

TEST(CoverageRingTest, CreateInvalidLinesPreservesZCoordinates) {
  constexpr std::array<Pt3D, 5> kSquare3DPts{{
      {0.0, 0.0, 10.0},
      {10.0, 0.0, 20.0},
      {10.0, 10.0, 30.0},
      {0.0, 10.0, 40.0},
      {0.0, 0.0, 10.0},
  }};
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<CoordinateSequence> seq3d = MakeSequence3D(kSquare3DPts);

  // Full ring path (extractSection).
  CoverageRing full_ring(seq3d.get(), true);
  for (std::size_t i = 0; i < 4; ++i) {
    full_ring.markInvalid(i);
  }
  std::vector<std::unique_ptr<LineString>> full_lines;
  full_ring.createInvalidLines(factory.get(), full_lines);
  ExpectInvalidLinesMatch(full_ring, factory.get(), full_lines,
                          {{0, 1, 2, 3, 4}});
  EXPECT_TRUE(full_lines[0]->hasZ());

  // Wrap-around path (extractSectionWrap).
  CoverageRing wrap_ring(seq3d.get(), true);
  wrap_ring.markInvalid(3);
  wrap_ring.markInvalid(0);
  std::vector<std::unique_ptr<LineString>> wrap_lines;
  wrap_ring.createInvalidLines(factory.get(), wrap_lines);
  ExpectInvalidLinesMatch(wrap_ring, factory.get(), wrap_lines, {{3, 0, 1}});
  EXPECT_TRUE(wrap_lines[0]->hasZ());

  // 2D input produces LineString without Z (NaN Z ordinates).
  std::unique_ptr<CoordinateSequence> seq2d = MakeSequence(kCcwSquarePts);
  CoverageRing ring2d(seq2d.get(), true);
  ring2d.markInvalid(3);
  ring2d.markInvalid(0);
  std::vector<std::unique_ptr<LineString>> lines2d;
  ring2d.createInvalidLines(factory.get(), lines2d);
  ExpectInvalidLinesMatch(ring2d, factory.get(), lines2d, {{3, 0, 1}});
  EXPECT_FALSE(lines2d[0]->hasZ());
}

}  // namespace
}  // namespace geos_google
