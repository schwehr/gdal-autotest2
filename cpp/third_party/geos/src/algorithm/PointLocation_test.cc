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

// Unit tests for third_party/geos/src/algorithm/PointLocation.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/algorithm/PointLocation.cpp

#include "third_party/geos/include/geos/algorithm/PointLocation.h"

#include <array>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
#include "third_party/geos/include/geos/geom/CompoundCurve.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::algorithm::PointLocation;
using geos::geom::CircularString;
using geos::geom::CompoundCurve;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::Location;
using geos::io::WKTReader;

struct SegmentCase {
  std::string_view name;
  CoordinateXY p;
  CoordinateXY p0;
  CoordinateXY p1;
  bool expected;
};

const auto kSegmentCases = std::to_array<SegmentCase>({
    {
        .name = "outside_envelope_collinear_before",
        .p = {-1.0, -1.0},
        .p0 = {0.0, 0.0},
        .p1 = {10.0, 10.0},
        .expected = false,
    },
    {
        .name = "outside_envelope_collinear_after",
        .p = {11.0, 11.0},
        .p0 = {0.0, 0.0},
        .p1 = {10.0, 10.0},
        .expected = false,
    },
    {
        .name = "outside_envelope_x",
        .p = {12.0, 5.0},
        .p0 = {0.0, 0.0},
        .p1 = {10.0, 10.0},
        .expected = false,
    },
    {
        .name = "outside_envelope_y",
        .p = {5.0, -0.5},
        .p0 = {0.0, 0.0},
        .p1 = {10.0, 10.0},
        .expected = false,
    },
    {
        .name = "zero_length_segment_match",
        .p = {3.5, -2.5},
        .p0 = {3.5, -2.5},
        .p1 = {3.5, -2.5},
        .expected = true,
    },
    {
        .name = "zero_length_segment_miss",
        .p = {3.5, -2.0},
        .p0 = {3.5, -2.5},
        .p1 = {3.5, -2.5},
        .expected = false,
    },
    {
        .name = "exact_start_endpoint",
        .p = {0.0, 0.0},
        .p0 = {0.0, 0.0},
        .p1 = {10.0, 10.0},
        .expected = true,
    },
    {
        .name = "exact_end_endpoint",
        .p = {10.0, 10.0},
        .p0 = {0.0, 0.0},
        .p1 = {10.0, 10.0},
        .expected = true,
    },
    {
        .name = "interior_diagonal",
        .p = {4.0, 4.0},
        .p0 = {0.0, 0.0},
        .p1 = {10.0, 10.0},
        .expected = true,
    },
    {
        .name = "interior_reversed_diagonal",
        .p = {4.0, 4.0},
        .p0 = {10.0, 10.0},
        .p1 = {0.0, 0.0},
        .expected = true,
    },
    {
        .name = "interior_horizontal",
        .p = {-3.0, 2.0},
        .p0 = {-5.0, 2.0},
        .p1 = {5.0, 2.0},
        .expected = true,
    },
    {
        .name = "interior_vertical",
        .p = {7.0, 1.5},
        .p0 = {7.0, -4.0},
        .p1 = {7.0, 6.0},
        .expected = true,
    },
    {
        .name = "inside_envelope_left_of_segment",
        .p = {4.0, 5.0},
        .p0 = {0.0, 0.0},
        .p1 = {10.0, 10.0},
        .expected = false,
    },
    {
        .name = "inside_envelope_right_of_segment",
        .p = {6.0, 5.0},
        .p0 = {0.0, 0.0},
        .p1 = {10.0, 10.0},
        .expected = false,
    },
    {
        .name = "large_coordinates_exact_midpoint",
        .p = {1.0e9 + 5.0, 1.0e9 + 5.0},
        .p0 = {1.0e9, 1.0e9},
        .p1 = {1.0e9 + 10.0, 1.0e9 + 10.0},
        .expected = true,
    },
    {
        .name = "large_coordinates_slightly_off",
        .p = {1.0e9 + 5.0, 1.0e9 + 5.0 + 1.0e-6},
        .p0 = {1.0e9, 1.0e9},
        .p1 = {1.0e9 + 10.0, 1.0e9 + 10.0},
        .expected = false,
    },
});

TEST(PointLocationTest, IsOnSegmentCases) {
  for (const auto& tc : kSegmentCases) {
    SCOPED_TRACE(tc.name);
    EXPECT_EQ(PointLocation::isOnSegment(tc.p, tc.p0, tc.p1), tc.expected);
  }
}

TEST(PointLocationTest, IsOnSegmentWithNanReturnsFalse) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(PointLocation::isOnSegment(CoordinateXY{nan, 5.0},
                                          CoordinateXY{0.0, 0.0},
                                          CoordinateXY{10.0, 10.0}));
  EXPECT_FALSE(PointLocation::isOnSegment(CoordinateXY{5.0, nan},
                                          CoordinateXY{0.0, 0.0},
                                          CoordinateXY{10.0, 10.0}));
}

TEST(PointLocationTest, IsOnSegmentWithInfReturnsFalse) {
  const double inf = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(PointLocation::isOnSegment(CoordinateXY{inf, 5.0},
                                          CoordinateXY{0.0, 0.0},
                                          CoordinateXY{10.0, 10.0}));
  EXPECT_FALSE(PointLocation::isOnSegment(CoordinateXY{-inf, 5.0},
                                          CoordinateXY{0.0, 0.0},
                                          CoordinateXY{10.0, 10.0}));
  EXPECT_FALSE(PointLocation::isOnSegment(CoordinateXY{5.0, inf},
                                          CoordinateXY{0.0, 0.0},
                                          CoordinateXY{10.0, 10.0}));
  EXPECT_FALSE(PointLocation::isOnSegment(CoordinateXY{5.0, -inf},
                                          CoordinateXY{0.0, 0.0},
                                          CoordinateXY{10.0, 10.0}));
}

TEST(PointLocationTest, IsOnLineEmptyAndSingleVertex) {
  const CoordinateSequence empty_seq = CoordinateSequence::XY(0);
  EXPECT_FALSE(PointLocation::isOnLine(CoordinateXY{0.0, 0.0}, &empty_seq));

  const CoordinateSequence single_seq({CoordinateXY{2.0, 3.0}});
  EXPECT_FALSE(PointLocation::isOnLine(CoordinateXY{2.0, 3.0}, &single_seq));
  EXPECT_FALSE(PointLocation::isOnLine(CoordinateXY{0.0, 0.0}, &single_seq));
}

TEST(PointLocationTest, IsOnLinePolylineAndDimensionality) {
  const CoordinateSequence line({
      CoordinateXY{0.0, 0.0},
      CoordinateXY{10.0, 0.0},
      CoordinateXY{10.0, 0.0},  // Repeated point creates zero-length segment.
      CoordinateXY{10.0, 10.0},
      CoordinateXY{20.0, 20.0},
  });

  EXPECT_TRUE(PointLocation::isOnLine(CoordinateXY{0.0, 0.0}, &line));
  EXPECT_TRUE(PointLocation::isOnLine(CoordinateXY{5.0, 0.0}, &line));
  EXPECT_TRUE(PointLocation::isOnLine(CoordinateXY{10.0, 0.0}, &line));
  EXPECT_TRUE(PointLocation::isOnLine(CoordinateXY{10.0, 5.0}, &line));
  EXPECT_TRUE(PointLocation::isOnLine(CoordinateXY{15.0, 15.0}, &line));
  EXPECT_TRUE(PointLocation::isOnLine(CoordinateXY{20.0, 20.0}, &line));

  EXPECT_FALSE(PointLocation::isOnLine(CoordinateXY{-1.0, 0.0}, &line));
  EXPECT_FALSE(PointLocation::isOnLine(CoordinateXY{21.0, 21.0}, &line));
  EXPECT_FALSE(PointLocation::isOnLine(CoordinateXY{5.0, 5.0}, &line));

  CoordinateSequence xyz_line(0, true, false);
  xyz_line.add(Coordinate{0.0, 0.0, 100.0});
  xyz_line.add(Coordinate{4.0, 4.0, 200.0});
  EXPECT_TRUE(PointLocation::isOnLine(CoordinateXY{2.0, 2.0}, &xyz_line));
  EXPECT_FALSE(PointLocation::isOnLine(CoordinateXY{2.0, 2.1}, &xyz_line));

  CoordinateSequence xym_line = CoordinateSequence::XYM(0);
  xym_line.add(CoordinateXYM{0.0, 0.0, 1.0});
  xym_line.add(CoordinateXYM{8.0, 0.0, 2.0});
  EXPECT_TRUE(PointLocation::isOnLine(CoordinateXY{4.0, 0.0}, &xym_line));
  EXPECT_FALSE(PointLocation::isOnLine(CoordinateXY{4.0, 0.1}, &xym_line));

  CoordinateSequence xyzm_line(0, true, true);
  xyzm_line.add(CoordinateXYZM{0.0, 0.0, 1.0, 10.0});
  xyzm_line.add(CoordinateXYZM{0.0, 8.0, 2.0, 20.0});
  EXPECT_TRUE(PointLocation::isOnLine(CoordinateXY{0.0, 4.0}, &xyzm_line));
  EXPECT_FALSE(PointLocation::isOnLine(CoordinateXY{0.1, 4.0}, &xyzm_line));
}

std::vector<const Coordinate*> ToCoordPointers(
    const std::vector<Coordinate>& coords) {
  std::vector<const Coordinate*> ptrs;
  ptrs.reserve(coords.size());
  for (const Coordinate& c : coords) {
    ptrs.push_back(&c);
  }
  return ptrs;
}

TEST(PointLocationTest, LocateAndIsInRingWithVectorAndSequence) {
  // Empty ring representations are always exterior.
  const std::vector<const Coordinate*> empty_vec;
  EXPECT_EQ(PointLocation::locateInRing(CoordinateXY{0.0, 0.0}, empty_vec),
            Location::EXTERIOR);
  EXPECT_FALSE(PointLocation::isInRing(CoordinateXY{0.0, 0.0}, empty_vec));

  const CoordinateSequence empty_seq = CoordinateSequence::XY(0);
  EXPECT_EQ(PointLocation::locateInRing(CoordinateXY{0.0, 0.0}, empty_seq),
            Location::EXTERIOR);
  EXPECT_FALSE(PointLocation::isInRing(CoordinateXY{0.0, 0.0}, &empty_seq));

  // Concave U-shaped ring tested in both CCW and CW orientations.
  const std::vector<Coordinate> ccw_u_coords = {
      {0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {7.0, 10.0}, {7.0, 3.0},
      {3.0, 3.0}, {3.0, 10.0}, {0.0, 10.0},  {0.0, 0.0},
  };
  const std::vector<Coordinate> cw_u_coords(ccw_u_coords.rbegin(),
                                            ccw_u_coords.rend());

  CoordinateSequence ccw_seq = CoordinateSequence::XY(0);
  for (const Coordinate& c : ccw_u_coords) {
    ccw_seq.add(c);
  }
  CoordinateSequence cw_seq(0, true, false);
  for (const Coordinate& c : cw_u_coords) {
    cw_seq.add(Coordinate{c.x, c.y, 42.0});
  }

  struct RingCase {
    std::string_view name;
    CoordinateXY p;
    Location expected;
  };
  const auto kRingCases = std::to_array<RingCase>({
      {.name = "interior_left_arm",
       .p = {1.5, 6.0},
       .expected = Location::INTERIOR},
      {.name = "interior_right_arm",
       .p = {8.5, 6.0},
       .expected = Location::INTERIOR},
      {.name = "interior_ray_aligned_with_notch_bottom",
       .p = {1.5, 3.0},
       .expected = Location::INTERIOR},
      {.name = "boundary_notch_bottom_edge",
       .p = {5.0, 3.0},
       .expected = Location::BOUNDARY},
      {.name = "exterior_ray_aligned_with_notch_bottom",
       .p = {-1.0, 3.0},
       .expected = Location::EXTERIOR},
      {.name = "interior_base",
       .p = {5.0, 1.5},
       .expected = Location::INTERIOR},
      {.name = "exterior_concave_notch",
       .p = {5.0, 6.0},
       .expected = Location::EXTERIOR},
      {.name = "exterior_aligned_with_top_vertices",
       .p = {-1.0, 10.0},
       .expected = Location::EXTERIOR},
      {.name = "exterior_outside_right",
       .p = {12.0, 5.0},
       .expected = Location::EXTERIOR},
      {.name = "boundary_vertex",
       .p = {7.0, 3.0},
       .expected = Location::BOUNDARY},
      {.name = "boundary_bottom_edge",
       .p = {5.0, 0.0},
       .expected = Location::BOUNDARY},
      {.name = "boundary_vertical_edge",
       .p = {10.0, 5.0},
       .expected = Location::BOUNDARY},
  });

  const std::vector<const Coordinate*> ccw_ptrs = ToCoordPointers(ccw_u_coords);
  const std::vector<const Coordinate*> cw_ptrs = ToCoordPointers(cw_u_coords);

  for (const auto& tc : kRingCases) {
    SCOPED_TRACE(tc.name);
    const bool expected_in_ring = (tc.expected != Location::EXTERIOR);

    EXPECT_EQ(PointLocation::locateInRing(tc.p, ccw_ptrs), tc.expected);
    EXPECT_EQ(PointLocation::isInRing(tc.p, ccw_ptrs), expected_in_ring);
    EXPECT_EQ(PointLocation::locateInRing(tc.p, cw_ptrs), tc.expected);
    EXPECT_EQ(PointLocation::isInRing(tc.p, cw_ptrs), expected_in_ring);

    EXPECT_EQ(PointLocation::locateInRing(tc.p, ccw_seq), tc.expected);
    EXPECT_EQ(PointLocation::isInRing(tc.p, &ccw_seq), expected_in_ring);
    EXPECT_EQ(PointLocation::locateInRing(tc.p, cw_seq), tc.expected);
    EXPECT_EQ(PointLocation::isInRing(tc.p, &cw_seq), expected_in_ring);
  }
}

TEST(PointLocationTest, LocateInRingWithLinearRingCurve) {
  const GeometryFactory::Ptr factory = GeometryFactory::create();
  const WKTReader reader(factory.get());

  const auto empty_ring = reader.read<LinearRing>("LINEARRING EMPTY");
  EXPECT_EQ(PointLocation::locateInRing(CoordinateXY{1.0, 1.0}, *empty_ring),
            Location::EXTERIOR);

  // Diamond ring with diagonal edges.
  const auto diamond =
      reader.read<LinearRing>("LINEARRING (0 2, 2 0, 4 2, 2 4, 0 2)");
  EXPECT_EQ(PointLocation::locateInRing(CoordinateXY{2.0, 2.0}, *diamond),
            Location::INTERIOR);
  EXPECT_EQ(PointLocation::locateInRing(CoordinateXY{1.0, 1.0}, *diamond),
            Location::BOUNDARY);
  EXPECT_EQ(PointLocation::locateInRing(CoordinateXY{2.0, 0.0}, *diamond),
            Location::BOUNDARY);
  EXPECT_EQ(PointLocation::locateInRing(CoordinateXY{0.5, 0.5}, *diamond),
            Location::EXTERIOR);
  EXPECT_EQ(PointLocation::locateInRing(CoordinateXY{-1.0, 2.0}, *diamond),
            Location::EXTERIOR);
}

TEST(PointLocationTest, LocateInRingWithCircularStringAndCompoundCurve) {
  const GeometryFactory::Ptr factory = GeometryFactory::create();
  const WKTReader reader(factory.get());

  const auto empty_circle = reader.read<CircularString>("CIRCULARSTRING EMPTY");
  EXPECT_EQ(PointLocation::locateInRing(CoordinateXY{2.0, 0.0}, *empty_circle),
            Location::EXTERIOR);

  // Circle centered at (2, 0) with radius 2.
  const auto circle =
      reader.read<CircularString>("CIRCULARSTRING (0 0, 2 2, 4 0, 2 -2, 0 0)");
  EXPECT_EQ(PointLocation::locateInRing(CoordinateXY{2.0, 0.0}, *circle),
            Location::INTERIOR);
  EXPECT_EQ(PointLocation::locateInRing(CoordinateXY{1.0, 1.0}, *circle),
            Location::INTERIOR);
  EXPECT_EQ(PointLocation::locateInRing(CoordinateXY{2.0, 2.0}, *circle),
            Location::BOUNDARY);
  EXPECT_EQ(PointLocation::locateInRing(CoordinateXY{0.0, 0.0}, *circle),
            Location::BOUNDARY);
  EXPECT_EQ(PointLocation::locateInRing(CoordinateXY{-1.0, 0.0}, *circle),
            Location::EXTERIOR);
  EXPECT_EQ(PointLocation::locateInRing(CoordinateXY{5.0, 0.0}, *circle),
            Location::EXTERIOR);

  // Closed D-shaped compound curve: upper semicircle plus closing baseline.
  const auto empty_compound = reader.read<CompoundCurve>("COMPOUNDCURVE EMPTY");
  EXPECT_EQ(
      PointLocation::locateInRing(CoordinateXY{2.0, 1.0}, *empty_compound),
      Location::EXTERIOR);

  const auto d_ring = reader.read<CompoundCurve>(
      "COMPOUNDCURVE (CIRCULARSTRING (0 0, 2 2, 4 0), (4 0, 0 0))");
  EXPECT_EQ(PointLocation::locateInRing(CoordinateXY{2.0, 1.0}, *d_ring),
            Location::INTERIOR);
  EXPECT_EQ(PointLocation::locateInRing(CoordinateXY{2.0, 2.0}, *d_ring),
            Location::BOUNDARY);
  EXPECT_EQ(PointLocation::locateInRing(CoordinateXY{2.0, 0.0}, *d_ring),
            Location::BOUNDARY);
  EXPECT_EQ(PointLocation::locateInRing(CoordinateXY{2.0, -1.0}, *d_ring),
            Location::EXTERIOR);
  EXPECT_EQ(PointLocation::locateInRing(CoordinateXY{-1.0, 1.0}, *d_ring),
            Location::EXTERIOR);
}

}  // namespace
}  // namespace geos_google
