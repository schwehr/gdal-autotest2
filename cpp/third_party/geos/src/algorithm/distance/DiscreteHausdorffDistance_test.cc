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

// Unit tests for
// third_party/geos/src/algorithm/distance/DiscreteHausdorffDistance.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/algorithm/distance/DiscreteHausdorffDistance.cpp

#include "third_party/geos/include/geos/algorithm/distance/DiscreteHausdorffDistance.h"

#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/distance/PointPairDistance.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::algorithm::distance::DiscreteHausdorffDistance;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::io::WKTReader;
using geos::util::IllegalArgumentException;
using geos::util::UnsupportedOperationException;

struct WktDistanceCase {
  std::string_view name;
  std::string_view wkt0;
  std::string_view wkt1;
  double expected_distance;
};

TEST(DiscreteHausdorffDistanceTest, StandardGeometryPairs) {
  const auto kWktDistanceCases = std::to_array<WktDistanceCase>({
      {
          .name = "identical_points",
          .wkt0 = "POINT (3 4)",
          .wkt1 = "POINT (3 4)",
          .expected_distance = 0.0,
      },
      {
          .name = "distinct_points",
          .wkt0 = "POINT (0 0)",
          .wkt1 = "POINT (3 4)",
          .expected_distance = 5.0,
      },
      {
          .name = "point_vs_linestring",
          .wkt0 = "POINT (0 5)",
          .wkt1 = "LINESTRING (0 0, 10 0)",
          .expected_distance = std::sqrt(125.0),
      },
      {
          .name = "point_vs_linestring_pythagorean",
          .wkt0 = "POINT (0 4)",
          .wkt1 = "LINESTRING (0 0, 3 0)",
          .expected_distance = 5.0,
      },
      {
          .name = "linestring_vs_point",
          .wkt0 = "LINESTRING (0 0, 10 0)",
          .wkt1 = "POINT (4 0)",
          .expected_distance = 6.0,
      },
      {
          .name = "identical_linestrings",
          .wkt0 = "LINESTRING (0 0, 5 5, 10 0)",
          .wkt1 = "LINESTRING (0 0, 5 5, 10 0)",
          .expected_distance = 0.0,
      },
      {
          .name = "opposite_direction_identical_lines",
          .wkt0 = "LINESTRING (0 0, 10 0)",
          .wkt1 = "LINESTRING (10 0, 0 0)",
          .expected_distance = 0.0,
      },
      {
          .name = "parallel_linestrings",
          .wkt0 = "LINESTRING (0 0, 10 0)",
          .wkt1 = "LINESTRING (0 3, 10 3)",
          .expected_distance = 3.0,
      },
      {
          .name = "diverging_linestrings",
          .wkt0 = "LINESTRING (0 0, 10 0)",
          .wkt1 = "LINESTRING (0 0, 10 4)",
          .expected_distance = 4.0,
      },
      {
          .name = "polygon_with_hole_vs_outer_square",
          .wkt0 = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
                  "(2 2, 8 2, 8 8, 2 8, 2 2))",
          .wkt1 = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
          .expected_distance = 2.0,
      },
      {
          .name = "multipolygon_vs_polygon",
          .wkt0 = "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
                  "((10 0, 14 0, 14 4, 10 4, 10 0)))",
          .wkt1 = "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))",
          .expected_distance = 10.0,
      },
      {
          .name = "multipoints",
          .wkt0 = "MULTIPOINT ((0 0), (10 0))",
          .wkt1 = "MULTIPOINT ((0 0), (7 4))",
          .expected_distance = 5.0,
      },
      {
          .name = "multilinestring_vs_linestring",
          .wkt0 = "MULTILINESTRING ((0 0, 5 0), (5 0, 10 0))",
          .wkt1 = "LINESTRING (0 2, 10 2)",
          .expected_distance = 2.0,
      },
      {
          .name = "geometry_collection_vs_point",
          .wkt0 = "GEOMETRYCOLLECTION (POINT (0 0), LINESTRING (0 0, 6 0))",
          .wkt1 = "POINT (0 0)",
          .expected_distance = 6.0,
      },
      {
          .name = "point_z_ignores_z_ordinate",
          .wkt0 = "POINT Z (0 0 100)",
          .wkt1 = "POINT Z (3 4 0)",
          .expected_distance = 5.0,
      },
  });

  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (const auto& tc : kWktDistanceCases) {
    SCOPED_TRACE(tc.name);
    Geometry::Ptr g0 = reader.read(std::string(tc.wkt0));
    Geometry::Ptr g1 = reader.read(std::string(tc.wkt1));
    ASSERT_NE(g0, nullptr);
    ASSERT_NE(g1, nullptr);
    EXPECT_NEAR(DiscreteHausdorffDistance::distance(*g0, *g1),
                tc.expected_distance, 1e-12);
  }
}

TEST(DiscreteHausdorffDistanceTest, LinearRingGeometry) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  CoordinateSequence ring_coords{
      CoordinateXY(0.0, 0.0), CoordinateXY(4.0, 0.0), CoordinateXY(4.0, 4.0),
      CoordinateXY(0.0, 4.0), CoordinateXY(0.0, 0.0),
  };
  std::unique_ptr<LinearRing> ring = factory->createLinearRing(ring_coords);
  std::unique_ptr<LineString> base = factory->createLineString(
      CoordinateSequence{CoordinateXY(0.0, 0.0), CoordinateXY(4.0, 0.0)});
  ASSERT_NE(ring, nullptr);
  ASSERT_NE(base, nullptr);

  EXPECT_DOUBLE_EQ(DiscreteHausdorffDistance::distance(*ring, *base), 4.0);
  EXPECT_DOUBLE_EQ(DiscreteHausdorffDistance::distance(*base, *ring), 4.0);
}

TEST(DiscreteHausdorffDistanceTest, OrientedDistanceAndGetCoordinates) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr short_line = reader.read("LINESTRING (0 0, 2 0)");
  Geometry::Ptr long_line = reader.read("LINESTRING (0 0, 10 0)");
  Geometry::Ptr pt = reader.read("POINT (0 5)");
  ASSERT_NE(short_line, nullptr);
  ASSERT_NE(long_line, nullptr);
  ASSERT_NE(pt, nullptr);

  // Every vertex of short_line lies on long_line.
  DiscreteHausdorffDistance forward(*short_line, *long_line);
  EXPECT_DOUBLE_EQ(forward.orientedDistance(), 0.0);

  // Vertex (10, 0) of long_line is distance 8.0 from (2, 0) on short_line.
  DiscreteHausdorffDistance reverse(*long_line, *short_line);
  EXPECT_DOUBLE_EQ(reverse.orientedDistance(), 8.0);
  std::array<CoordinateXY, 2> rev_coords = reverse.getCoordinates();
  EXPECT_EQ(rev_coords[0], CoordinateXY(2.0, 0.0));
  EXPECT_EQ(rev_coords[1], CoordinateXY(10.0, 0.0));
  EXPECT_DOUBLE_EQ(rev_coords[0].distance(rev_coords[1]), 8.0);

  // Symmetric distance takes the maximum across both orientations.
  DiscreteHausdorffDistance sym(*short_line, *long_line);
  EXPECT_DOUBLE_EQ(sym.distance(), 8.0);
  std::array<CoordinateXY, 2> sym_coords = sym.getCoordinates();
  EXPECT_DOUBLE_EQ(sym_coords[0].distance(sym_coords[1]), 8.0);

  // Asymmetric point vs line: oriented distances are 5.0 and sqrt(125.0).
  DiscreteHausdorffDistance pt_to_line(*pt, *long_line);
  EXPECT_DOUBLE_EQ(pt_to_line.orientedDistance(), 5.0);
  std::array<CoordinateXY, 2> pt_to_line_coords = pt_to_line.getCoordinates();
  EXPECT_EQ(pt_to_line_coords[0], CoordinateXY(0.0, 0.0));
  EXPECT_EQ(pt_to_line_coords[1], CoordinateXY(0.0, 5.0));

  DiscreteHausdorffDistance line_to_pt(*long_line, *pt);
  EXPECT_NEAR(line_to_pt.orientedDistance(), std::sqrt(125.0), 1e-12);
  std::array<CoordinateXY, 2> line_to_pt_coords = line_to_pt.getCoordinates();
  EXPECT_EQ(line_to_pt_coords[0], CoordinateXY(0.0, 5.0));
  EXPECT_EQ(line_to_pt_coords[1], CoordinateXY(10.0, 0.0));
}

TEST(DiscreteHausdorffDistanceTest, DensificationImprovesApproximation) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr seg = reader.read("LINESTRING (0 0, 10 0)");
  Geometry::Ptr endpoints = reader.read("MULTIPOINT ((0 0), (10 0))");
  ASSERT_NE(seg, nullptr);
  ASSERT_NE(endpoints, nullptr);

  EXPECT_DOUBLE_EQ(DiscreteHausdorffDistance::distance(*seg, *endpoints), 0.0);
  // Boundary fraction 1.0 produces 1 subsegment per edge (vertices only).
  EXPECT_DOUBLE_EQ(DiscreteHausdorffDistance::distance(*seg, *endpoints, 1.0),
                   0.0);
  // Fraction 0.7 rounds 1/0.7 ~= 1.43 to 1 subsegment (vertices only).
  EXPECT_DOUBLE_EQ(DiscreteHausdorffDistance::distance(*seg, *endpoints, 0.7),
                   0.0);
  // Fraction 0.6 rounds 1/0.6 ~= 1.67 to 2 subsegments (midpoint at x=5).
  EXPECT_DOUBLE_EQ(DiscreteHausdorffDistance::distance(*seg, *endpoints, 0.6),
                   5.0);
  // Midpoint (5, 0) is 5.0 units away from both endpoints.
  EXPECT_DOUBLE_EQ(DiscreteHausdorffDistance::distance(*seg, *endpoints, 0.5),
                   5.0);
  EXPECT_DOUBLE_EQ(DiscreteHausdorffDistance::distance(*seg, *endpoints, 0.25),
                   5.0);

  // Fraction 0.3 rounds 1/0.3 ~= 3.33 to 3 subsegments: points at x=0, 3, 6.
  Geometry::Ptr seg9 = reader.read("LINESTRING (0 0, 9 0)");
  Geometry::Ptr endpoints9 = reader.read("MULTIPOINT ((0 0), (9 0))");
  ASSERT_NE(seg9, nullptr);
  ASSERT_NE(endpoints9, nullptr);
  EXPECT_NEAR(DiscreteHausdorffDistance::distance(*seg9, *endpoints9, 0.3), 3.0,
              1e-12);

  // Calling distance() before and after setDensifyFraction on the same object.
  DiscreteHausdorffDistance reusable(*seg, *endpoints);
  EXPECT_DOUBLE_EQ(reusable.distance(), 0.0);
  reusable.setDensifyFraction(0.5);
  EXPECT_DOUBLE_EQ(reusable.distance(), 5.0);

  // Long diagonal segment whose interior is much farther than its vertices.
  Geometry::Ptr wedge = reader.read("LINESTRING (0 0, 100 0, 10 100)");
  Geometry::Ptr corner = reader.read("LINESTRING (0 100, 0 10, 80 10)");
  ASSERT_NE(wedge, nullptr);
  ASSERT_NE(corner, nullptr);

  const double vertex_only =
      DiscreteHausdorffDistance::distance(*wedge, *corner);
  const double densified =
      DiscreteHausdorffDistance::distance(*wedge, *corner, 0.01);
  EXPECT_NEAR(vertex_only, std::sqrt(500.0), 1e-12);
  EXPECT_GT(densified, 47.0);
  EXPECT_LT(densified, 49.0);

  // Oriented distance also honors setDensifyFraction.
  Geometry::Ptr bottom = reader.read("LINESTRING (0 0, 10 0)");
  Geometry::Ptr u_shape = reader.read("LINESTRING (0 0, 0 10, 10 10, 10 0)");
  ASSERT_NE(bottom, nullptr);
  ASSERT_NE(u_shape, nullptr);

  DiscreteHausdorffDistance oriented(*bottom, *u_shape);
  EXPECT_DOUBLE_EQ(oriented.orientedDistance(), 0.0);
  oriented.setDensifyFraction(0.5);
  EXPECT_DOUBLE_EQ(oriented.orientedDistance(), 5.0);
  const std::array<CoordinateXY, 2> oriented_coords = oriented.getCoordinates();
  EXPECT_DOUBLE_EQ(oriented_coords[0].distance(oriented_coords[1]), 5.0);

  // Polygon and multi-segment LineString densification.
  Geometry::Ptr square = reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  Geometry::Ptr corners =
      reader.read("MULTIPOINT ((0 0), (10 0), (10 10), (0 10))");
  Geometry::Ptr l_shape = reader.read("LINESTRING (0 0, 10 0, 10 10)");
  Geometry::Ptr l_pts = reader.read("MULTIPOINT ((0 0), (10 0), (10 10))");
  ASSERT_NE(square, nullptr);
  ASSERT_NE(corners, nullptr);
  ASSERT_NE(l_shape, nullptr);
  ASSERT_NE(l_pts, nullptr);
  EXPECT_DOUBLE_EQ(DiscreteHausdorffDistance::distance(*square, *corners), 0.0);
  EXPECT_DOUBLE_EQ(DiscreteHausdorffDistance::distance(*square, *corners, 0.5),
                   5.0);
  EXPECT_DOUBLE_EQ(DiscreteHausdorffDistance::distance(*l_shape, *l_pts, 0.5),
                   5.0);

  // Zero-length segment and mixed GeometryCollection with densification.
  Geometry::Ptr zero_seg = reader.read("LINESTRING (0 0, 0 0, 6 0)");
  Geometry::Ptr origin = reader.read("POINT (0 0)");
  Geometry::Ptr mixed_coll =
      reader.read("GEOMETRYCOLLECTION (POINT (0 0), LINESTRING (0 0, 10 0))");
  Geometry::Ptr coll_with_empty =
      reader.read("GEOMETRYCOLLECTION (POINT EMPTY, LINESTRING (0 0, 10 0))");
  ASSERT_NE(zero_seg, nullptr);
  ASSERT_NE(origin, nullptr);
  ASSERT_NE(mixed_coll, nullptr);
  ASSERT_NE(coll_with_empty, nullptr);
  EXPECT_DOUBLE_EQ(DiscreteHausdorffDistance::distance(*zero_seg, *origin, 0.5),
                   6.0);
  EXPECT_DOUBLE_EQ(
      DiscreteHausdorffDistance::distance(*mixed_coll, *endpoints, 0.5), 5.0);
  EXPECT_DOUBLE_EQ(
      DiscreteHausdorffDistance::distance(*coll_with_empty, *endpoints, 0.5),
      5.0);

  // Point inputs exercise the index == 0 skip in
  // MaxDensifiedByFractionDistanceFilter::filter_ro.
  Geometry::Ptr pt0 = reader.read("POINT (0 0)");
  Geometry::Ptr pt1 = reader.read("POINT (3 4)");
  ASSERT_NE(pt0, nullptr);
  ASSERT_NE(pt1, nullptr);
  EXPECT_DOUBLE_EQ(DiscreteHausdorffDistance::distance(*pt0, *pt1, 0.5), 5.0);
}

TEST(DiscreteHausdorffDistanceTest, EmptyGeometriesReturnNaN) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr empty_pt = reader.read("POINT EMPTY");
  Geometry::Ptr empty_line = reader.read("LINESTRING EMPTY");
  Geometry::Ptr empty_coll = reader.read("GEOMETRYCOLLECTION EMPTY");
  Geometry::Ptr pt = reader.read("POINT (1 2)");
  ASSERT_NE(empty_pt, nullptr);
  ASSERT_NE(empty_line, nullptr);
  ASSERT_NE(empty_coll, nullptr);
  ASSERT_NE(pt, nullptr);

  EXPECT_TRUE(std::isnan(DiscreteHausdorffDistance::distance(*empty_pt, *pt)));
  EXPECT_TRUE(
      std::isnan(DiscreteHausdorffDistance::distance(*pt, *empty_line)));
  EXPECT_TRUE(
      std::isnan(DiscreteHausdorffDistance::distance(*empty_coll, *empty_pt)));
  EXPECT_TRUE(
      std::isnan(DiscreteHausdorffDistance::distance(*empty_line, *pt, 0.5)));
  EXPECT_TRUE(
      std::isnan(DiscreteHausdorffDistance::distance(*pt, *empty_line, 0.5)));

  DiscreteHausdorffDistance dhd(*empty_pt, *pt);
  EXPECT_TRUE(std::isnan(dhd.orientedDistance()));

  DiscreteHausdorffDistance dhd_rev(*pt, *empty_pt);
  EXPECT_TRUE(std::isnan(dhd_rev.orientedDistance()));
  dhd_rev.setDensifyFraction(0.5);
  EXPECT_TRUE(std::isnan(dhd_rev.orientedDistance()));
}

TEST(DiscreteHausdorffDistanceTest,
     InvalidDensifyFractionsThrowIllegalArgument) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr g0 = reader.read("LINESTRING (0 0, 1 0)");
  Geometry::Ptr g1 = reader.read("LINESTRING (0 1, 1 1)");
  ASSERT_NE(g0, nullptr);
  ASSERT_NE(g1, nullptr);

  constexpr auto kBadFractions = std::to_array<double>({
      geos::DoubleNotANumber,
      std::numeric_limits<double>::quiet_NaN(),
      1.0001,
      2.0,
      std::numeric_limits<double>::infinity(),
      0.0,
      -0.0,
      -0.5,
      -std::numeric_limits<double>::infinity(),
      // Positive <= 1.0 whose rounded reciprocal exceeds size_t::max().
      1e-25,
      std::numeric_limits<double>::min(),
      std::numeric_limits<double>::denorm_min(),
  });

  for (double frac : kBadFractions) {
    SCOPED_TRACE(frac);
    EXPECT_THROW(DiscreteHausdorffDistance::distance(*g0, *g1, frac),
                 IllegalArgumentException);
    DiscreteHausdorffDistance dhd(*g0, *g1);
    EXPECT_THROW(dhd.setDensifyFraction(frac), IllegalArgumentException);
  }
}

TEST(DiscreteHausdorffDistanceTest, CurvedGeometriesThrowUnsupported) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr arc = reader.read("CIRCULARSTRING (0 0, 1 1, 2 0)");
  Geometry::Ptr compound =
      reader.read("COMPOUNDCURVE ((0 0, 1 1), CIRCULARSTRING (1 1, 2 0, 3 1))");
  Geometry::Ptr curve_poly =
      reader.read("CURVEPOLYGON (CIRCULARSTRING (0 0, 2 2, 4 0, 2 -2, 0 0))");
  Geometry::Ptr coll_curved = reader.read(
      "GEOMETRYCOLLECTION (POINT (0 0), CIRCULARSTRING (0 0, 1 1, 2 0))");
  Geometry::Ptr line = reader.read("LINESTRING (0 0, 2 0)");
  ASSERT_NE(arc, nullptr);
  ASSERT_NE(compound, nullptr);
  ASSERT_NE(curve_poly, nullptr);
  ASSERT_NE(coll_curved, nullptr);
  ASSERT_NE(line, nullptr);

  EXPECT_THROW(DiscreteHausdorffDistance::distance(*arc, *line),
               UnsupportedOperationException);
  EXPECT_THROW(DiscreteHausdorffDistance::distance(*line, *arc),
               UnsupportedOperationException);
  EXPECT_THROW(DiscreteHausdorffDistance::distance(*compound, *line),
               UnsupportedOperationException);
  EXPECT_THROW(DiscreteHausdorffDistance::distance(*line, *curve_poly),
               UnsupportedOperationException);
  EXPECT_THROW(DiscreteHausdorffDistance::distance(*coll_curved, *line),
               UnsupportedOperationException);

  DiscreteHausdorffDistance oriented(*arc, *line);
  EXPECT_THROW(oriented.orientedDistance(), UnsupportedOperationException);
  DiscreteHausdorffDistance oriented_rev(*line, *arc);
  EXPECT_THROW(oriented_rev.orientedDistance(), UnsupportedOperationException);
}

TEST(DiscreteHausdorffDistanceTest, FilterClassesDirectBehavior) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr line = reader.read("LINESTRING (0 0, 10 0)");
  ASSERT_NE(line, nullptr);

  DiscreteHausdorffDistance::MaxPointDistanceFilter pt_filter(*line);
  const CoordinateXY c1(5.0, 2.0);
  const CoordinateXY c2(8.0, 7.0);
  pt_filter.filter_ro(&c1);
  pt_filter.filter_ro(&c2);
  EXPECT_DOUBLE_EQ(pt_filter.getMaxPointDistance().getDistance(), 7.0);

  Geometry::Ptr endpoints = reader.read("MULTIPOINT ((0 0), (10 0))");
  ASSERT_NE(endpoints, nullptr);
  DiscreteHausdorffDistance::MaxDensifiedByFractionDistanceFilter frac_filter(
      *endpoints, 0.5);
  EXPECT_FALSE(frac_filter.isGeometryChanged());
  EXPECT_FALSE(frac_filter.isDone());

  CoordinateSequence seq{CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0),
                         CoordinateXY(10.0, 12.0)};
  frac_filter.filter_ro(seq, 0);
  EXPECT_TRUE(std::isnan(frac_filter.getMaxPointDistance().getDistance()));

  frac_filter.filter_ro(seq, 1);
  EXPECT_DOUBLE_EQ(frac_filter.getMaxPointDistance().getDistance(), 5.0);

  frac_filter.filter_ro(seq, 2);
  EXPECT_DOUBLE_EQ(frac_filter.getMaxPointDistance().getDistance(), 6.0);
}

}  // namespace
}  // namespace geos_google
