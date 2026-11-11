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

// Unit tests for third_party/geos/src/linearref/LocationIndexOfPoint.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/linearref/LocationIndexOfPoint.cpp

#include "third_party/geos/include/geos/linearref/LocationIndexOfPoint.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/linearref/LinearLocation.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::DoubleInfinity;
using geos::DoubleNegInfinity;
using geos::DoubleNotANumber;
using geos::geom::Coordinate;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::io::WKTReader;
using geos::linearref::LinearLocation;
using geos::linearref::LocationIndexOfPoint;
using geos::util::IllegalArgumentException;

struct PointLocationCase {
  std::string_view name;
  double pt_x;
  double pt_y;
  std::size_t expected_component;
  std::size_t expected_segment;
  double expected_fraction;
  double expected_coord_x;
  double expected_coord_y;
};

void ExpectLocation(const LinearLocation& loc, std::size_t expected_component,
                    std::size_t expected_segment, double expected_fraction) {
  EXPECT_EQ(loc.getComponentIndex(), expected_component);
  EXPECT_EQ(loc.getSegmentIndex(), expected_segment);
  EXPECT_NEAR(loc.getSegmentFraction(), expected_fraction, 1e-12);
}

class LocationIndexOfPointTest : public ::testing::Test {
 protected:
  Geometry::Ptr ReadWkt(const std::string& wkt) const {
    Geometry::Ptr geom = reader_.read(wkt);
    EXPECT_NE(geom, nullptr);
    return geom;
  }

  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader reader_{*factory_};
};

TEST_F(LocationIndexOfPointTest, StaticAndInstanceIndexOfSingleSegment) {
  Geometry::Ptr line = ReadWkt("LINESTRING (0 0, 10 0)");
  ASSERT_NE(line, nullptr);
  const LocationIndexOfPoint locater(line.get());

  constexpr auto kCases = std::to_array<PointLocationCase>({
      {
          .name = "start_vertex",
          .pt_x = 0.0,
          .pt_y = 0.0,
          .expected_component = 0,
          .expected_segment = 0,
          .expected_fraction = 0.0,
          .expected_coord_x = 0.0,
          .expected_coord_y = 0.0,
      },
      {
          .name = "before_start",
          .pt_x = -5.0,
          .pt_y = 3.0,
          .expected_component = 0,
          .expected_segment = 0,
          .expected_fraction = 0.0,
          .expected_coord_x = 0.0,
          .expected_coord_y = 0.0,
      },
      {
          .name = "interior_on_line",
          .pt_x = 2.5,
          .pt_y = 0.0,
          .expected_component = 0,
          .expected_segment = 0,
          .expected_fraction = 0.25,
          .expected_coord_x = 2.5,
          .expected_coord_y = 0.0,
      },
      {
          .name = "interior_offset",
          .pt_x = 7.5,
          .pt_y = 4.0,
          .expected_component = 0,
          .expected_segment = 0,
          .expected_fraction = 0.75,
          .expected_coord_x = 7.5,
          .expected_coord_y = 0.0,
      },
      {
          .name = "end_vertex_normalized",
          .pt_x = 10.0,
          .pt_y = 0.0,
          .expected_component = 0,
          .expected_segment = 1,
          .expected_fraction = 0.0,
          .expected_coord_x = 10.0,
          .expected_coord_y = 0.0,
      },
      {
          .name = "beyond_end_normalized",
          .pt_x = 15.0,
          .pt_y = -3.0,
          .expected_component = 0,
          .expected_segment = 1,
          .expected_fraction = 0.0,
          .expected_coord_x = 10.0,
          .expected_coord_y = 0.0,
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const CoordinateXY pt(tc.pt_x, tc.pt_y);

    const LinearLocation static_loc =
        LocationIndexOfPoint::indexOf(line.get(), pt);
    ExpectLocation(static_loc, tc.expected_component, tc.expected_segment,
                   tc.expected_fraction);
    EXPECT_TRUE(static_loc.isValid(line.get()));

    const Coordinate coord = static_loc.getCoordinate(line.get());
    EXPECT_NEAR(coord.x, tc.expected_coord_x, 1e-12);
    EXPECT_NEAR(coord.y, tc.expected_coord_y, 1e-12);

    const LinearLocation member_loc = locater.indexOf(pt);
    EXPECT_EQ(member_loc.compareTo(static_loc), 0);
  }

  // Verify 3D Coordinate slicing to CoordinateXY ignores Z for planar distance.
  const LinearLocation z_input_loc =
      LocationIndexOfPoint::indexOf(line.get(), Coordinate(5.0, 2.0, 99.0));
  ExpectLocation(z_input_loc, 0, 0, 0.5);
}

TEST_F(LocationIndexOfPointTest, IndexOfMultiSegmentLineString) {
  Geometry::Ptr line = ReadWkt("LINESTRING (0 0, 10 0, 10 10, 20 10)");
  ASSERT_NE(line, nullptr);

  constexpr auto kCases = std::to_array<PointLocationCase>({
      {
          .name = "closest_to_first_segment",
          .pt_x = 2.0,
          .pt_y = -1.0,
          .expected_component = 0,
          .expected_segment = 0,
          .expected_fraction = 0.2,
          .expected_coord_x = 2.0,
          .expected_coord_y = 0.0,
      },
      {
          .name = "closest_to_middle_segment",
          .pt_x = 12.0,
          .pt_y = 5.0,
          .expected_component = 0,
          .expected_segment = 1,
          .expected_fraction = 0.5,
          .expected_coord_x = 10.0,
          .expected_coord_y = 5.0,
      },
      {
          .name = "closest_to_last_segment",
          .pt_x = 18.0,
          .pt_y = 13.0,
          .expected_component = 0,
          .expected_segment = 2,
          .expected_fraction = 0.8,
          .expected_coord_x = 18.0,
          .expected_coord_y = 10.0,
      },
      {
          .name = "interior_vertex_tie_keeps_first_zero_distance",
          .pt_x = 10.0,
          .pt_y = 0.0,
          .expected_component = 0,
          .expected_segment = 1,
          .expected_fraction = 0.0,
          .expected_coord_x = 10.0,
          .expected_coord_y = 0.0,
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const LinearLocation loc = LocationIndexOfPoint::indexOf(
        line.get(), CoordinateXY(tc.pt_x, tc.pt_y));
    ExpectLocation(loc, tc.expected_component, tc.expected_segment,
                   tc.expected_fraction);
    const Coordinate coord = loc.getCoordinate(line.get());
    EXPECT_NEAR(coord.x, tc.expected_coord_x, 1e-12);
    EXPECT_NEAR(coord.y, tc.expected_coord_y, 1e-12);
  }
}

TEST_F(LocationIndexOfPointTest, IndexOfTieBreaksTowardStartOfGeometry) {
  // Equidistant between first and third segments of a U-shape.
  Geometry::Ptr u_line = ReadWkt("LINESTRING (0 0, 10 0, 10 4, 0 4)");
  ASSERT_NE(u_line, nullptr);
  ExpectLocation(
      LocationIndexOfPoint::indexOf(u_line.get(), CoordinateXY(5, 2)), 0, 0,
      0.5);

  // Self-retracing line: forward pass beats reverse pass.
  Geometry::Ptr retrace = ReadWkt("LINESTRING (0 0, 10 0, 0 0)");
  ASSERT_NE(retrace, nullptr);
  ExpectLocation(
      LocationIndexOfPoint::indexOf(retrace.get(), CoordinateXY(3, 1)), 0, 0,
      0.3);

  // Closed ring: start vertex beats closing vertex.
  Geometry::Ptr ring = ReadWkt("LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  ASSERT_NE(ring, nullptr);
  ExpectLocation(LocationIndexOfPoint::indexOf(ring.get(), CoordinateXY(0, 0)),
                 0, 0, 0.0);
}

TEST_F(LocationIndexOfPointTest, IndexOfDiagonalAndZeroLengthSegments) {
  Geometry::Ptr diagonal = ReadWkt("LINESTRING (0 0, 3 4)");
  ASSERT_NE(diagonal, nullptr);
  // Projection of (3, 0) onto (0, 0)-(3, 4) has factor (3*3 + 0*4) / 25 = 0.36.
  ExpectLocation(
      LocationIndexOfPoint::indexOf(diagonal.get(), CoordinateXY(3, 0)), 0, 0,
      0.36);

  Geometry::Ptr repeated_start = ReadWkt("LINESTRING (0 0, 0 0, 10 0)");
  ASSERT_NE(repeated_start, nullptr);
  ExpectLocation(
      LocationIndexOfPoint::indexOf(repeated_start.get(), CoordinateXY(0, 0)),
      0, 0, 0.0);
  ExpectLocation(
      LocationIndexOfPoint::indexOf(repeated_start.get(), CoordinateXY(4, 0)),
      0, 1, 0.4);

  Geometry::Ptr degenerate = ReadWkt("LINESTRING (3 4, 3 4)");
  ASSERT_NE(degenerate, nullptr);
  ExpectLocation(
      LocationIndexOfPoint::indexOf(degenerate.get(), CoordinateXY(0, 0)), 0, 0,
      0.0);
}

TEST_F(LocationIndexOfPointTest, IndexOfLineStringZInterpolatesZ) {
  Geometry::Ptr line_z = ReadWkt("LINESTRING Z (0 0 10, 10 0 20)");
  ASSERT_NE(line_z, nullptr);

  const LinearLocation loc =
      LocationIndexOfPoint::indexOf(line_z.get(), CoordinateXY(5.0, 3.0));
  ExpectLocation(loc, 0, 0, 0.5);

  const Coordinate coord = loc.getCoordinate(line_z.get());
  EXPECT_NEAR(coord.x, 5.0, 1e-12);
  EXPECT_NEAR(coord.y, 0.0, 1e-12);
  EXPECT_NEAR(coord.z, 15.0, 1e-12);
}

TEST_F(LocationIndexOfPointTest, IndexOfMultiLineString) {
  Geometry::Ptr multi =
      ReadWkt("MULTILINESTRING ((0 0, 10 0), (20 0, 30 0), (40 0, 40 10))");
  ASSERT_NE(multi, nullptr);

  ExpectLocation(
      LocationIndexOfPoint::indexOf(multi.get(), CoordinateXY(24.0, 3.0)), 1, 0,
      0.4);
  ExpectLocation(
      LocationIndexOfPoint::indexOf(multi.get(), CoordinateXY(42.0, 7.0)), 2, 0,
      0.7);
  // Equidistant between end of component 0 (10, 0) and start of component 1
  // (20, 0); component 0 wins.
  ExpectLocation(
      LocationIndexOfPoint::indexOf(multi.get(), CoordinateXY(15.0, 0.0)), 0, 1,
      0.0);
}

TEST_F(LocationIndexOfPointTest, EmptyMultiLineString) {
  Geometry::Ptr empty_multi = ReadWkt("MULTILINESTRING EMPTY");
  ASSERT_NE(empty_multi, nullptr);

  // Initial minFrac=-1.0 is clamped to 0.0 during LinearLocation construction.
  ExpectLocation(
      LocationIndexOfPoint::indexOf(empty_multi.get(), CoordinateXY(1.0, 2.0)),
      0, 0, 0.0);

  const LinearLocation zero_min(0, 0, 0.0);
  ExpectLocation(LocationIndexOfPoint::indexOfAfter(
                     empty_multi.get(), CoordinateXY(1.0, 2.0), &zero_min),
                 0, 0, 0.0);

  const LinearLocation later_min(0, 0, 0.5);
  ExpectLocation(LocationIndexOfPoint::indexOfAfter(
                     empty_multi.get(), CoordinateXY(1.0, 2.0), &later_min),
                 0, 0, 0.0);
}

TEST_F(LocationIndexOfPointTest, IndexOfAfterNullMinIndexUsesStart) {
  Geometry::Ptr line = ReadWkt("LINESTRING (0 0, 10 0, 20 0)");
  ASSERT_NE(line, nullptr);

  ExpectLocation(LocationIndexOfPoint::indexOfAfter(
                     line.get(), CoordinateXY(3.0, 1.0), nullptr),
                 0, 0, 0.3);

  const LocationIndexOfPoint locater(line.get());
  ExpectLocation(locater.indexOfAfter(CoordinateXY(14.0, -2.0), nullptr), 0, 1,
                 0.4);
}

TEST_F(LocationIndexOfPointTest, IndexOfAfterAtOrPastEndReturnsEndLocation) {
  Geometry::Ptr line = ReadWkt("LINESTRING (0 0, 10 0, 20 0)");
  ASSERT_NE(line, nullptr);

  const LinearLocation end_loc = LinearLocation::getEndLocation(line.get());
  ExpectLocation(end_loc, 0, 2, 1.0);

  ExpectLocation(LocationIndexOfPoint::indexOfAfter(
                     line.get(), CoordinateXY(1.0, 0.0), &end_loc),
                 0, 2, 1.0);

  // LinearLocation(0, 2, 1.0) normalizes to (0, 3, 0.0), which is > end_loc.
  const LinearLocation normalized_past_end(0, 2, 1.0);
  ExpectLocation(LocationIndexOfPoint::indexOfAfter(
                     line.get(), CoordinateXY(1.0, 0.0), &normalized_past_end),
                 0, 2, 1.0);

  const LinearLocation component_past_end(1, 0, 0.0);
  ExpectLocation(LocationIndexOfPoint::indexOfAfter(
                     line.get(), CoordinateXY(1.0, 0.0), &component_past_end),
                 0, 2, 1.0);

  Geometry::Ptr multi = ReadWkt("MULTILINESTRING ((0 0, 10 0), (20 0, 30 0))");
  ASSERT_NE(multi, nullptr);
  const LinearLocation multi_end = LinearLocation::getEndLocation(multi.get());
  ExpectLocation(multi_end, 1, 1, 1.0);
  ExpectLocation(LocationIndexOfPoint::indexOfAfter(
                     multi.get(), CoordinateXY(5.0, 0.0), &multi_end),
                 1, 1, 1.0);

  const LinearLocation multi_past_end(2, 0, 0.0);
  ExpectLocation(LocationIndexOfPoint::indexOfAfter(
                     multi.get(), CoordinateXY(5.0, 0.0), &multi_past_end),
                 1, 1, 1.0);
}

TEST_F(LocationIndexOfPointTest, IndexOfAfterAdvancesPastMinLocation) {
  // Later point on the same segment.
  Geometry::Ptr single = ReadWkt("LINESTRING (0 0, 10 0)");
  ASSERT_NE(single, nullptr);
  const LinearLocation min_same_seg(0, 0, 0.3);
  ExpectLocation(LocationIndexOfPoint::indexOfAfter(
                     single.get(), CoordinateXY(7.0, 2.0), &min_same_seg),
                 0, 0, 0.7);

  // Minimum at start when nearest point is also start (not strictly after,
  // falls back to equal location without throwing).
  const LinearLocation min_start(0, 0, 0.0);
  ExpectLocation(LocationIndexOfPoint::indexOfAfter(
                     single.get(), CoordinateXY(0.0, 0.0), &min_start),
                 0, 0, 0.0);

  // Self-retracing line: second pass found after first pass.
  Geometry::Ptr retrace = ReadWkt("LINESTRING (0 0, 10 0, 0 0)");
  ASSERT_NE(retrace, nullptr);
  const LocationIndexOfPoint retrace_locater(retrace.get());
  const LinearLocation first = retrace_locater.indexOf(CoordinateXY(3.0, 1.0));
  ExpectLocation(first, 0, 0, 0.3);
  const LinearLocation second =
      retrace_locater.indexOfAfter(CoordinateXY(3.0, 1.0), &first);
  ExpectLocation(second, 0, 1, 0.7);
  const Coordinate second_coord = second.getCoordinate(retrace.get());
  EXPECT_NEAR(second_coord.x, 3.0, 1e-12);
  EXPECT_NEAR(second_coord.y, 0.0, 1e-12);

  // Earlier segment has distance 0 but is before minIndex; later segment wins
  // because minDistance is not polluted by rejected candidates.
  Geometry::Ptr multi_seg = ReadWkt("LINESTRING (0 0, 10 0, 10 10, 20 10)");
  ASSERT_NE(multi_seg, nullptr);
  const LinearLocation min_mid(0, 1, 0.5);
  ExpectLocation(LocationIndexOfPoint::indexOfAfter(
                     multi_seg.get(), CoordinateXY(5.0, 0.0), &min_mid),
                 0, 2, 0.0);

  // MultiLineString with overlapping components.
  Geometry::Ptr multi = ReadWkt("MULTILINESTRING ((0 0, 10 0), (0 0, 10 0))");
  ASSERT_NE(multi, nullptr);
  const LinearLocation min_comp0(0, 0, 0.5);
  ExpectLocation(LocationIndexOfPoint::indexOfAfter(
                     multi.get(), CoordinateXY(2.0, 1.0), &min_comp0),
                 1, 0, 0.2);

  // Closed ring revisiting start point at the end of the final segment.
  Geometry::Ptr ring = ReadWkt("LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  ASSERT_NE(ring, nullptr);
  const LinearLocation ring_end = LocationIndexOfPoint::indexOfAfter(
      ring.get(), CoordinateXY(0.0, 0.0), &min_start);
  ExpectLocation(ring_end, 0, 4, 0.0);
  const Coordinate ring_coord = ring_end.getCoordinate(ring.get());
  EXPECT_NEAR(ring_coord.x, 0.0, 1e-12);
  EXPECT_NEAR(ring_coord.y, 0.0, 1e-12);
}

TEST_F(LocationIndexOfPointTest, IndexOfAfterThrowsWhenNoCandidateAfterMin) {
  Geometry::Ptr line = ReadWkt("LINESTRING (0 0, 10 0)");
  ASSERT_NE(line, nullptr);

  const LinearLocation min_loc(0, 0, 0.8);
  // Nearest point is strictly before min_loc.
  EXPECT_THROW(LocationIndexOfPoint::indexOfAfter(
                   line.get(), CoordinateXY(2.0, 1.0), &min_loc),
               IllegalArgumentException);

  // Nearest point equals min_loc on the only segment; candidate is rejected
  // because compareLocationValues is 0 (not < 0), leaving (0, 0, 0.0) < min.
  EXPECT_THROW(LocationIndexOfPoint::indexOfAfter(
                   line.get(), CoordinateXY(8.0, 0.0), &min_loc),
               IllegalArgumentException);

  // min_at_vertex is normalized to (0, 1, 0.0), while end_loc is (0, 1, 1.0).
  const LinearLocation min_at_vertex(0, 0, 1.0);
  EXPECT_THROW(LocationIndexOfPoint::indexOfAfter(
                   line.get(), CoordinateXY(10.0, 0.0), &min_at_vertex),
               IllegalArgumentException);
}

TEST_F(LocationIndexOfPointTest, NonLinealGeometryThrowsIllegalArgument) {
  constexpr auto kNonLinealWkts = std::to_array<std::string_view>({
      "POINT (1 2)",
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
      "GEOMETRYCOLLECTION (LINESTRING (0 0, 10 0), POINT (5 5))",
      "GEOMETRYCOLLECTION (POINT (5 5), LINESTRING (0 0, 10 0))",
  });
  const CoordinateXY query(1.0, 2.0);
  const LinearLocation min_loc(0, 0, 0.0);

  for (const std::string_view wkt : kNonLinealWkts) {
    SCOPED_TRACE(wkt);
    Geometry::Ptr geom = ReadWkt(std::string(wkt));
    ASSERT_NE(geom, nullptr);
    EXPECT_THROW(LocationIndexOfPoint::indexOf(geom.get(), query),
                 IllegalArgumentException);
    EXPECT_THROW(
        LocationIndexOfPoint::indexOfAfter(geom.get(), query, &min_loc),
        IllegalArgumentException);
  }
}

TEST_F(LocationIndexOfPointTest, NonFiniteInputCoordinates) {
  Geometry::Ptr line = ReadWkt("LINESTRING (0 0, 10 0)");
  ASSERT_NE(line, nullptr);

  struct CoordPair {
    std::string_view name;
    double x;
    double y;
  };
  constexpr auto kNonFiniteCases = std::to_array<CoordPair>({
      {.name = "nan_x", .x = DoubleNotANumber, .y = 0.0},
      {.name = "nan_y", .x = 0.0, .y = DoubleNotANumber},
      {.name = "pos_inf_x", .x = DoubleInfinity, .y = 0.0},
      {.name = "neg_inf_x", .x = DoubleNegInfinity, .y = 0.0},
      {.name = "pos_inf_y", .x = 5.0, .y = DoubleInfinity},
  });

  // Non-finite distances never satisfy segDistance < DoubleInfinity, so the
  // default fallback (0, 0, 0.0) is returned by indexOf.
  for (const auto& tc : kNonFiniteCases) {
    SCOPED_TRACE(tc.name);
    ExpectLocation(
        LocationIndexOfPoint::indexOf(line.get(), CoordinateXY(tc.x, tc.y)), 0,
        0, 0.0);
  }

  const LinearLocation min_loc(0, 0, 0.5);
  EXPECT_THROW(LocationIndexOfPoint::indexOfAfter(
                   line.get(), CoordinateXY(DoubleNotANumber, 0.0), &min_loc),
               IllegalArgumentException);
}

}  // namespace
}  // namespace geos_google
