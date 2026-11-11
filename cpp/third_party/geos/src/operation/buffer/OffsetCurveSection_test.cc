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

// Unit tests for third_party/geos/src/operation/buffer/OffsetCurveSection.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/buffer/OffsetCurveSection.cpp

#include "third_party/geos/include/geos/operation/buffer/OffsetCurveSection.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GEOS_LINESTRING;
using geos::geom::GEOS_MULTILINESTRING;
using geos::operation::buffer::OffsetCurveSection;

struct Point2D {
  double x;
  double y;
};

std::unique_ptr<CoordinateSequence> MakeSequence(std::span<const Point2D> pts) {
  auto seq = std::make_unique<CoordinateSequence>();
  for (const Point2D& pt : pts) {
    seq->add(Coordinate(pt.x, pt.y));
  }
  return seq;
}

std::unique_ptr<CoordinateSequence> MakeSequence(
    std::initializer_list<Point2D> pts) {
  return MakeSequence(std::span<const Point2D>(pts.begin(), pts.size()));
}

std::unique_ptr<OffsetCurveSection> MakeSection(std::span<const Point2D> pts,
                                                double loc, double loc_last) {
  return std::make_unique<OffsetCurveSection>(MakeSequence(pts), loc, loc_last);
}

std::unique_ptr<OffsetCurveSection> MakeSection(
    std::initializer_list<Point2D> pts, double loc, double loc_last) {
  return MakeSection(std::span<const Point2D>(pts.begin(), pts.size()), loc,
                     loc_last);
}

void ExpectCoordsEqual2D(const CoordinateSequence* actual,
                         std::span<const Point2D> expected) {
  ASSERT_NE(actual, nullptr);
  ASSERT_EQ(actual->size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_DOUBLE_EQ(actual->getAt(i).x, expected[i].x);
    EXPECT_DOUBLE_EQ(actual->getAt(i).y, expected[i].y);
  }
}

void ExpectCoordsEqual2D(const CoordinateSequence* actual,
                         std::initializer_list<Point2D> expected) {
  ExpectCoordsEqual2D(
      actual, std::span<const Point2D>(expected.begin(), expected.size()));
}

TEST(OffsetCurveSectionTest, ConstructsAndExposesLocationAndCoordinates) {
  auto section = MakeSection({{0.0, 0.0}, {1.0, 2.0}, {3.0, 4.0}}, 2.5, 4.75);
  EXPECT_DOUBLE_EQ(section->getLocation(), 2.5);

  const CoordinateSequence* borrowed = section->getCoordinates();
  ExpectCoordsEqual2D(borrowed, {{0.0, 0.0}, {1.0, 2.0}, {3.0, 4.0}});
  // Calling getCoordinates() repeatedly is non-destructive.
  EXPECT_EQ(section->getCoordinates(), borrowed);

  std::unique_ptr<CoordinateSequence> released = section->releaseCoordinates();
  ExpectCoordsEqual2D(released.get(), {{0.0, 0.0}, {1.0, 2.0}, {3.0, 4.0}});
  EXPECT_EQ(section->getCoordinates(), nullptr);
  EXPECT_EQ(section->releaseCoordinates(), nullptr);

  auto null_section = std::make_unique<OffsetCurveSection>(nullptr, -1.5, 0.0);
  EXPECT_DOUBLE_EQ(null_section->getLocation(), -1.5);
  EXPECT_EQ(null_section->getCoordinates(), nullptr);
  EXPECT_EQ(null_section->releaseCoordinates(), nullptr);
}

TEST(OffsetCurveSectionTest, ComparatorOrdersByLocation) {
  struct ComparatorCase {
    std::string_view name;
    double loc_a;
    double loc_b;
    bool expected_less;
  };

  constexpr auto kCases = std::to_array<ComparatorCase>({
      {.name = "strictly_less",
       .loc_a = 1.25,
       .loc_b = 1.75,
       .expected_less = true},
      {.name = "strictly_greater",
       .loc_a = 1.75,
       .loc_b = 1.25,
       .expected_less = false},
      {.name = "equal_locations",
       .loc_a = 2.5,
       .loc_b = 2.5,
       .expected_less = false},
      {.name = "negative_locations",
       .loc_a = -1.5,
       .loc_b = -0.5,
       .expected_less = true},
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    auto a = MakeSection({{0.0, 0.0}, {1.0, 0.0}}, tc.loc_a, tc.loc_a);
    auto b = MakeSection({{2.0, 0.0}, {3.0, 0.0}}, tc.loc_b, tc.loc_b);
    EXPECT_EQ(OffsetCurveSection::OffsetCurveSectionComparator(a, b),
              tc.expected_less);
  }

  std::vector<std::unique_ptr<OffsetCurveSection>> sections;
  sections.push_back(MakeSection({{0.0, 0.0}, {1.0, 0.0}}, 3.1, 3.5));
  sections.push_back(MakeSection({{0.0, 0.0}, {1.0, 0.0}}, 0.5, 0.9));
  sections.push_back(MakeSection({{0.0, 0.0}, {1.0, 0.0}}, 2.9, 3.0));
  sections.push_back(MakeSection({{0.0, 0.0}, {1.0, 0.0}}, 1.0, 1.4));

  std::sort(sections.begin(), sections.end(),
            OffsetCurveSection::OffsetCurveSectionComparator);

  ASSERT_EQ(sections.size(), 4);
  EXPECT_DOUBLE_EQ(sections[0]->getLocation(), 0.5);
  EXPECT_DOUBLE_EQ(sections[1]->getLocation(), 1.0);
  EXPECT_DOUBLE_EQ(sections[2]->getLocation(), 2.9);
  EXPECT_DOUBLE_EQ(sections[3]->getLocation(), 3.1);
}

TEST(OffsetCurveSectionTest, CreateExtractsLinearAndWrappedSectionsFromRing) {
  constexpr auto kRing = std::to_array<Point2D>({
      {0.0, 0.0},
      {10.0, 0.0},
      {10.0, 10.0},
      {0.0, 10.0},
      {0.0, 0.0},
  });
  std::unique_ptr<CoordinateSequence> ring = MakeSequence(kRing);

  struct CreateCase {
    std::string_view name;
    std::size_t start;
    std::size_t end;
    double loc;
    double loc_last;
    std::array<Point2D, 5> expected;
    std::size_t expected_size;
  };

  constexpr auto kCases = std::to_array<CreateCase>({
      {
          .name = "linear_single_segment",
          .start = 0,
          .end = 1,
          .loc = 0.1,
          .loc_last = 0.9,
          .expected = {{{0.0, 0.0}, {10.0, 0.0}}},
          .expected_size = 2,
      },
      {
          .name = "linear_interior_slice",
          .start = 1,
          .end = 3,
          .loc = 1.25,
          .loc_last = 2.75,
          .expected = {{{10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}}},
          .expected_size = 3,
      },
      {
          .name = "linear_ending_at_closing_index",
          .start = 2,
          .end = 4,
          .loc = 2.0,
          .loc_last = 3.5,
          .expected = {{{10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}}},
          .expected_size = 3,
      },
      {
          .name = "linear_full_ring_zero_to_end",
          .start = 0,
          .end = 4,
          .loc = 0.0,
          .loc_last = 3.9,
          .expected = {{{0.0, 0.0},
                        {10.0, 0.0},
                        {10.0, 10.0},
                        {0.0, 10.0},
                        {0.0, 0.0}}},
          .expected_size = 5,
      },
      {
          .name = "wrapped_across_seam",
          .start = 3,
          .end = 1,
          .loc = 3.2,
          .loc_last = 4.8,
          .expected = {{{0.0, 10.0}, {0.0, 0.0}, {10.0, 0.0}}},
          .expected_size = 3,
      },
      {
          .name = "wrapped_ending_at_zero",
          .start = 2,
          .end = 0,
          .loc = 2.5,
          .loc_last = 3.9,
          .expected = {{{10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}}},
          .expected_size = 3,
      },
      {
          .name = "full_ring_equal_interior_start_and_end",
          .start = 2,
          .end = 2,
          .loc = 0.1,
          .loc_last = 3.9,
          .expected = {{{10.0, 10.0},
                        {0.0, 10.0},
                        {0.0, 0.0},
                        {10.0, 0.0},
                        {10.0, 10.0}}},
          .expected_size = 5,
      },
      {
          .name = "full_ring_equal_zero_start_and_end",
          .start = 0,
          .end = 0,
          .loc = 0.0,
          .loc_last = 3.0,
          .expected = {{{0.0, 0.0},
                        {10.0, 0.0},
                        {10.0, 10.0},
                        {0.0, 10.0},
                        {0.0, 0.0}}},
          .expected_size = 5,
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    std::unique_ptr<OffsetCurveSection> section = OffsetCurveSection::create(
        ring.get(), tc.start, tc.end, tc.loc, tc.loc_last);
    ASSERT_NE(section, nullptr);
    EXPECT_DOUBLE_EQ(section->getLocation(), tc.loc);
    ExpectCoordsEqual2D(
        section->getCoordinates(),
        std::span<const Point2D>(tc.expected.data(), tc.expected_size));
  }
}

TEST(OffsetCurveSectionTest, ToGeometryHandlesEmptyAndSingleSection) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::vector<std::unique_ptr<OffsetCurveSection>> empty_sections;
  std::unique_ptr<Geometry> empty_geom =
      OffsetCurveSection::toGeometry(empty_sections, factory.get());
  ASSERT_NE(empty_geom, nullptr);
  EXPECT_EQ(empty_geom->getGeometryTypeId(), GEOS_LINESTRING);
  EXPECT_TRUE(empty_geom->isEmpty());

  std::vector<std::unique_ptr<OffsetCurveSection>> single_section;
  single_section.push_back(
      MakeSection({{1.0, 2.0}, {3.0, 4.0}, {5.0, 6.0}}, 2.0, 3.0));
  std::unique_ptr<Geometry> single_geom =
      OffsetCurveSection::toGeometry(single_section, factory.get());
  ASSERT_NE(single_geom, nullptr);
  EXPECT_EQ(single_geom->getGeometryTypeId(), GEOS_LINESTRING);
  EXPECT_FALSE(single_geom->isEmpty());
  std::unique_ptr<CoordinateSequence> single_coords =
      single_geom->getCoordinates();
  ExpectCoordsEqual2D(single_coords.get(),
                      {{1.0, 2.0}, {3.0, 4.0}, {5.0, 6.0}});
  EXPECT_EQ(single_section[0]->getCoordinates(), nullptr);

  std::vector<std::unique_ptr<OffsetCurveSection>> single_empty_coords;
  single_empty_coords.push_back(MakeSection({}, 0.0, 0.0));
  std::unique_ptr<Geometry> empty_single_geom =
      OffsetCurveSection::toGeometry(single_empty_coords, factory.get());
  ASSERT_NE(empty_single_geom, nullptr);
  EXPECT_EQ(empty_single_geom->getGeometryTypeId(), GEOS_LINESTRING);
  EXPECT_TRUE(empty_single_geom->isEmpty());
}

TEST(OffsetCurveSectionTest, ToGeometryMultipleSectionsSortsByLocation) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::vector<std::unique_ptr<OffsetCurveSection>> sections;
  sections.push_back(MakeSection({{50.0, 0.0}, {60.0, 0.0}}, 5.2, 6.0));
  sections.push_back(MakeSection({{10.0, 0.0}, {20.0, 0.0}}, 1.1, 2.0));
  sections.push_back(MakeSection({{30.0, 0.0}, {40.0, 0.0}}, 3.8, 4.0));

  std::unique_ptr<Geometry> multi =
      OffsetCurveSection::toGeometry(sections, factory.get());
  ASSERT_NE(multi, nullptr);
  EXPECT_EQ(multi->getGeometryTypeId(), GEOS_MULTILINESTRING);
  ASSERT_EQ(multi->getNumGeometries(), 3);

  std::unique_ptr<CoordinateSequence> c0 =
      multi->getGeometryN(0)->getCoordinates();
  std::unique_ptr<CoordinateSequence> c1 =
      multi->getGeometryN(1)->getCoordinates();
  std::unique_ptr<CoordinateSequence> c2 =
      multi->getGeometryN(2)->getCoordinates();
  ExpectCoordsEqual2D(c0.get(), {{10.0, 0.0}, {20.0, 0.0}});
  ExpectCoordsEqual2D(c1.get(), {{30.0, 0.0}, {40.0, 0.0}});
  ExpectCoordsEqual2D(c2.get(), {{50.0, 0.0}, {60.0, 0.0}});

  for (const auto& section : sections) {
    EXPECT_EQ(section->getCoordinates(), nullptr);
  }
}

TEST(OffsetCurveSectionTest, ToLineHandlesEmptyAndSingleSection) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::vector<std::unique_ptr<OffsetCurveSection>> empty_sections;
  std::unique_ptr<Geometry> empty_line =
      OffsetCurveSection::toLine(empty_sections, factory.get());
  ASSERT_NE(empty_line, nullptr);
  EXPECT_EQ(empty_line->getGeometryTypeId(), GEOS_LINESTRING);
  EXPECT_TRUE(empty_line->isEmpty());

  std::vector<std::unique_ptr<OffsetCurveSection>> single_section;
  single_section.push_back(
      MakeSection({{0.0, 0.0}, {2.0, 0.0}, {2.0, 3.0}}, 0.5, 1.5));
  std::unique_ptr<Geometry> single_line =
      OffsetCurveSection::toLine(single_section, factory.get());
  ASSERT_NE(single_line, nullptr);
  EXPECT_EQ(single_line->getGeometryTypeId(), GEOS_LINESTRING);
  EXPECT_FALSE(single_line->isEmpty());
  std::unique_ptr<CoordinateSequence> coords = single_line->getCoordinates();
  ExpectCoordsEqual2D(coords.get(), {{0.0, 0.0}, {2.0, 0.0}, {2.0, 3.0}});
  EXPECT_EQ(single_section[0]->getCoordinates(), nullptr);
}

TEST(OffsetCurveSectionTest,
     ToLineJoinVertexRemovalFollowsSegmentFloorBoundary) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  struct JoinBoundaryCase {
    std::string_view name;
    double first_loc;
    double first_loc_last;
    double second_loc;
    double second_loc_last;
    bool insert_second_first;
    std::array<Point2D, 6> expected;
    std::size_t expected_size;
  };

  // First section coords: (0,0), (1,0), (2,0)
  // Second section coords: (3,0), (4,0), (5,0)
  // When join lies in the same segment, (2,0) and (3,0) are removed.
  constexpr auto kCases = std::to_array<JoinBoundaryCase>({
      {
          .name = "same_segment_fractional",
          .first_loc = 0.2,
          .first_loc_last = 1.25,
          .second_loc = 1.85,
          .second_loc_last = 3.0,
          .insert_second_first = true,
          .expected = {{{0.0, 0.0}, {1.0, 0.0}, {4.0, 0.0}, {5.0, 0.0}}},
          .expected_size = 4,
      },
      {
          .name = "same_segment_exact_integer_floor",
          .first_loc = 0.5,
          .first_loc_last = 2.0,
          .second_loc = 2.999,
          .second_loc_last = 4.0,
          .insert_second_first = false,
          .expected = {{{0.0, 0.0}, {1.0, 0.0}, {4.0, 0.0}, {5.0, 0.0}}},
          .expected_size = 4,
      },
      {
          .name = "same_segment_exact_equal_integer",
          .first_loc = 0.5,
          .first_loc_last = 2.0,
          .second_loc = 2.0,
          .second_loc_last = 4.0,
          .insert_second_first = false,
          .expected = {{{0.0, 0.0}, {1.0, 0.0}, {4.0, 0.0}, {5.0, 0.0}}},
          .expected_size = 4,
      },
      {
          .name = "different_segment_just_below_integer",
          .first_loc = 0.5,
          .first_loc_last = 1.999,
          .second_loc = 2.0,
          .second_loc_last = 4.0,
          .insert_second_first = true,
          .expected = {{{0.0, 0.0},
                        {1.0, 0.0},
                        {2.0, 0.0},
                        {3.0, 0.0},
                        {4.0, 0.0},
                        {5.0, 0.0}}},
          .expected_size = 6,
      },
      {
          .name = "different_segment_interior_fractions",
          .first_loc = 1.0,
          .first_loc_last = 1.75,
          .second_loc = 2.25,
          .second_loc_last = 3.5,
          .insert_second_first = true,
          .expected = {{{0.0, 0.0},
                        {1.0, 0.0},
                        {2.0, 0.0},
                        {3.0, 0.0},
                        {4.0, 0.0},
                        {5.0, 0.0}}},
          .expected_size = 6,
      },
      {
          .name = "same_negative_segment",
          .first_loc = -2.5,
          .first_loc_last = -1.2,
          .second_loc = -1.8,
          .second_loc_last = -0.3,
          .insert_second_first = true,
          .expected = {{{0.0, 0.0}, {1.0, 0.0}, {4.0, 0.0}, {5.0, 0.0}}},
          .expected_size = 4,
      },
      {
          .name = "different_segment_across_zero",
          .first_loc = -1.5,
          .first_loc_last = -0.2,
          .second_loc = 0.2,
          .second_loc_last = 1.5,
          .insert_second_first = true,
          .expected = {{{0.0, 0.0},
                        {1.0, 0.0},
                        {2.0, 0.0},
                        {3.0, 0.0},
                        {4.0, 0.0},
                        {5.0, 0.0}}},
          .expected_size = 6,
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    auto first = MakeSection({{0.0, 0.0}, {1.0, 0.0}, {2.0, 0.0}}, tc.first_loc,
                             tc.first_loc_last);
    auto second = MakeSection({{3.0, 0.0}, {4.0, 0.0}, {5.0, 0.0}},
                              tc.second_loc, tc.second_loc_last);

    std::vector<std::unique_ptr<OffsetCurveSection>> sections;
    if (tc.insert_second_first) {
      sections.push_back(std::move(second));
      sections.push_back(std::move(first));
    } else {
      sections.push_back(std::move(first));
      sections.push_back(std::move(second));
    }

    std::unique_ptr<Geometry> line =
        OffsetCurveSection::toLine(sections, factory.get());
    ASSERT_NE(line, nullptr);
    EXPECT_EQ(line->getGeometryTypeId(), GEOS_LINESTRING);
    std::unique_ptr<CoordinateSequence> coords = line->getCoordinates();
    ExpectCoordsEqual2D(
        coords.get(),
        std::span<const Point2D>(tc.expected.data(), tc.expected_size));

    // When joining 2+ sections, toLine reads coordinates without releasing.
    EXPECT_NE(sections[0]->getCoordinates(), nullptr);
    EXPECT_NE(sections[1]->getCoordinates(), nullptr);
  }
}

TEST(OffsetCurveSectionTest,
     ToLineDeduplicatesTouchingEndpointAcrossDifferentSegments) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::vector<std::unique_ptr<OffsetCurveSection>> sections;
  sections.push_back(MakeSection({{0.0, 0.0}, {2.0, 0.0}}, 0.5, 1.5));
  sections.push_back(MakeSection({{2.0, 0.0}, {4.0, 0.0}}, 3.5, 4.5));

  std::unique_ptr<Geometry> line =
      OffsetCurveSection::toLine(sections, factory.get());
  ASSERT_NE(line, nullptr);
  EXPECT_EQ(line->getGeometryTypeId(), GEOS_LINESTRING);
  std::unique_ptr<CoordinateSequence> coords = line->getCoordinates();
  ExpectCoordsEqual2D(coords.get(), {{0.0, 0.0}, {2.0, 0.0}, {4.0, 0.0}});
}

TEST(OffsetCurveSectionTest,
     ToLineChainsThreeSectionsWithMiddleTruncationAndMixedJoins) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  {
    SCOPED_TRACE("three_point_middle_loses_both_ends");
    std::vector<std::unique_ptr<OffsetCurveSection>> sections;
    sections.push_back(
        MakeSection({{6.0, 0.0}, {7.0, 0.0}, {8.0, 0.0}}, 3.7, 5.0));
    sections.push_back(
        MakeSection({{0.0, 0.0}, {1.0, 0.0}, {2.0, 0.0}}, 0.1, 1.2));
    sections.push_back(
        MakeSection({{3.0, 0.0}, {4.0, 0.0}, {5.0, 0.0}}, 1.8, 3.1));

    std::unique_ptr<Geometry> line =
        OffsetCurveSection::toLine(sections, factory.get());
    ASSERT_NE(line, nullptr);
    std::unique_ptr<CoordinateSequence> coords = line->getCoordinates();
    ExpectCoordsEqual2D(
        coords.get(),
        {{0.0, 0.0}, {1.0, 0.0}, {4.0, 0.0}, {7.0, 0.0}, {8.0, 0.0}});
  }

  {
    SCOPED_TRACE("two_point_middle_completely_dropped");
    std::vector<std::unique_ptr<OffsetCurveSection>> sections;
    sections.push_back(
        MakeSection({{0.0, 0.0}, {1.0, 0.0}, {2.0, 0.0}}, 0.0, 1.1));
    sections.push_back(MakeSection({{3.0, 0.0}, {4.0, 0.0}}, 1.9, 2.2));
    sections.push_back(
        MakeSection({{5.0, 0.0}, {6.0, 0.0}, {7.0, 0.0}}, 2.8, 4.0));

    std::unique_ptr<Geometry> line =
        OffsetCurveSection::toLine(sections, factory.get());
    ASSERT_NE(line, nullptr);
    std::unique_ptr<CoordinateSequence> coords = line->getCoordinates();
    ExpectCoordsEqual2D(coords.get(),
                        {{0.0, 0.0}, {1.0, 0.0}, {6.0, 0.0}, {7.0, 0.0}});
  }

  {
    SCOPED_TRACE("mixed_same_then_different_segment");
    std::vector<std::unique_ptr<OffsetCurveSection>> sections;
    sections.push_back(MakeSection({{6.0, 0.0}, {7.0, 0.0}}, 3.1, 4.0));
    sections.push_back(
        MakeSection({{3.0, 0.0}, {4.0, 0.0}, {5.0, 0.0}}, 1.9, 2.2));
    sections.push_back(
        MakeSection({{0.0, 0.0}, {1.0, 0.0}, {2.0, 0.0}}, 0.0, 1.1));

    std::unique_ptr<Geometry> line =
        OffsetCurveSection::toLine(sections, factory.get());
    ASSERT_NE(line, nullptr);
    std::unique_ptr<CoordinateSequence> coords = line->getCoordinates();
    ExpectCoordsEqual2D(coords.get(), {{0.0, 0.0},
                                       {1.0, 0.0},
                                       {4.0, 0.0},
                                       {5.0, 0.0},
                                       {6.0, 0.0},
                                       {7.0, 0.0}});
  }
}

TEST(OffsetCurveSectionTest, ToLineHandlesEmptySectionAmongMultipleSections) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  {
    SCOPED_TRACE("empty_middle_section");
    std::vector<std::unique_ptr<OffsetCurveSection>> sections;
    sections.push_back(MakeSection({{0.0, 0.0}, {1.0, 0.0}}, 0.0, 1.2));
    sections.push_back(MakeSection({}, 2.0, 3.0));
    sections.push_back(MakeSection({{2.0, 0.0}, {3.0, 0.0}}, 4.0, 5.0));

    std::unique_ptr<Geometry> line =
        OffsetCurveSection::toLine(sections, factory.get());
    ASSERT_NE(line, nullptr);
    EXPECT_EQ(line->getGeometryTypeId(), GEOS_LINESTRING);
    std::unique_ptr<CoordinateSequence> coords = line->getCoordinates();
    ExpectCoordsEqual2D(coords.get(),
                        {{0.0, 0.0}, {1.0, 0.0}, {2.0, 0.0}, {3.0, 0.0}});
  }

  {
    SCOPED_TRACE("all_empty_sections");
    std::vector<std::unique_ptr<OffsetCurveSection>> sections;
    sections.push_back(MakeSection({}, 1.0, 1.5));
    sections.push_back(MakeSection({}, 2.0, 2.5));

    std::unique_ptr<Geometry> line =
        OffsetCurveSection::toLine(sections, factory.get());
    ASSERT_NE(line, nullptr);
    EXPECT_EQ(line->getGeometryTypeId(), GEOS_LINESTRING);
    EXPECT_TRUE(line->isEmpty());
  }
}

TEST(OffsetCurveSectionTest, PreservesZAndLeavesMissingZAsNaN) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  CoordinateSequence ring_xyz;
  for (const Coordinate& c : {
           Coordinate(0.0, 0.0, 10.5),
           Coordinate(5.0, 0.0, 20.5),
           Coordinate(5.0, 5.0, 30.5),
           Coordinate(0.0, 5.0, 40.5),
           Coordinate(0.0, 0.0, 10.5),
       }) {
    ring_xyz.add(c);
  }

  std::vector<std::unique_ptr<OffsetCurveSection>> xyz_sections;
  xyz_sections.push_back(OffsetCurveSection::create(&ring_xyz, 0, 1, 0.0, 0.9));
  xyz_sections.push_back(OffsetCurveSection::create(&ring_xyz, 2, 3, 2.0, 2.9));

  std::unique_ptr<Geometry> line_xyz =
      OffsetCurveSection::toLine(xyz_sections, factory.get());
  ASSERT_NE(line_xyz, nullptr);
  std::unique_ptr<CoordinateSequence> coords_xyz = line_xyz->getCoordinates();
  ASSERT_NE(coords_xyz, nullptr);
  ASSERT_EQ(coords_xyz->size(), 4);
  EXPECT_DOUBLE_EQ(coords_xyz->getAt(0).z, 10.5);
  EXPECT_DOUBLE_EQ(coords_xyz->getAt(1).z, 20.5);
  EXPECT_DOUBLE_EQ(coords_xyz->getAt(2).z, 30.5);
  EXPECT_DOUBLE_EQ(coords_xyz->getAt(3).z, 40.5);

  std::unique_ptr<CoordinateSequence> ring_xy = MakeSequence({
      {0.0, 0.0},
      {5.0, 0.0},
      {5.0, 5.0},
      {0.0, 5.0},
      {0.0, 0.0},
  });
  std::unique_ptr<OffsetCurveSection> sec_xy =
      OffsetCurveSection::create(ring_xy.get(), 1, 3, 1.0, 2.5);
  const CoordinateSequence* coords_xy = sec_xy->getCoordinates();
  ASSERT_NE(coords_xy, nullptr);
  ASSERT_EQ(coords_xy->size(), 3);
  for (std::size_t i = 0; i < coords_xy->size(); ++i) {
    EXPECT_TRUE(std::isnan(coords_xy->getAt(i).z));
  }
}

}  // namespace
}  // namespace geos_google
