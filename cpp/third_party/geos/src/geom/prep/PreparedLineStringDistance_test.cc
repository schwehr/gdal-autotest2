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

#include "third_party/geos/include/geos/geom/prep/PreparedLineStringDistance.h"

#include <cmath>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/prep/PreparedLineString.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::prep::PreparedLineString;
using geos::geom::prep::PreparedLineStringDistance;
using geos::io::WKTReader;

class PreparedLineStringDistanceTest : public ::testing::Test {
 protected:
  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader reader_{*factory_};
};

TEST_F(PreparedLineStringDistanceTest, EmptyGeometriesReturnInfinityAndFalse) {
  auto empty_line = ReadWkt("LINESTRING EMPTY");
  auto empty_mls = ReadWkt("MULTILINESTRING EMPTY");
  auto line = ReadWkt("LINESTRING (0 0, 10 0)");
  auto pt = ReadWkt("POINT (1 1)");
  auto empty_pt = ReadWkt("POINT EMPTY");
  auto empty_poly = ReadWkt("POLYGON EMPTY");
  auto empty_gc = ReadWkt("GEOMETRYCOLLECTION EMPTY");

  const double expected_inf = geos::DoubleInfinity;

  {
    PreparedLineString prep_empty(empty_line.get());
    PreparedLineStringDistance dist_op(prep_empty);

    double d1 = PreparedLineStringDistance::distance(prep_empty, pt.get());
    EXPECT_TRUE(std::isinf(d1));
    EXPECT_EQ(d1, expected_inf);
    EXPECT_EQ(dist_op.distance(empty_pt.get()), expected_inf);

    EXPECT_FALSE(dist_op.isWithinDistance(pt.get(), 0.0));
    EXPECT_FALSE(dist_op.isWithinDistance(pt.get(), 100.0));
    EXPECT_FALSE(dist_op.isWithinDistance(pt.get(), expected_inf));
    EXPECT_FALSE(dist_op.isWithinDistance(empty_pt.get(), expected_inf));
  }

  {
    PreparedLineString prep_empty_mls(empty_mls.get());
    EXPECT_EQ(PreparedLineStringDistance::distance(prep_empty_mls, pt.get()),
              expected_inf);
    EXPECT_FALSE(PreparedLineStringDistance(prep_empty_mls)
                     .isWithinDistance(pt.get(), 10.0));
  }

  {
    PreparedLineString prep_line(line.get());
    PreparedLineStringDistance dist_op(prep_line);

    for (const Geometry* g :
         {empty_pt.get(), empty_line.get(), empty_poly.get(), empty_gc.get()}) {
      EXPECT_EQ(PreparedLineStringDistance::distance(prep_line, g),
                expected_inf);
      EXPECT_EQ(dist_op.distance(g), expected_inf);
      EXPECT_FALSE(dist_op.isWithinDistance(g, 0.0));
      EXPECT_FALSE(dist_op.isWithinDistance(g, 1000.0));
      EXPECT_FALSE(dist_op.isWithinDistance(g, expected_inf));
    }
  }
}

TEST_F(PreparedLineStringDistanceTest,
       ZeroFacetDistanceForTouchingAndIntersectingInputs) {
  auto line = ReadWkt("LINESTRING (0 0, 10 0)");
  PreparedLineString prep(line.get());
  PreparedLineStringDistance dist_op(prep);

  auto pt_on_interior = ReadWkt("POINT (5 0)");
  auto pt_on_endpoint = ReadWkt("POINT (0 0)");
  auto mpt_touching = ReadWkt("MULTIPOINT ((5 5), (10 0))");
  auto crossing_line = ReadWkt("LINESTRING (5 -3, 5 3)");
  auto touching_line = ReadWkt("LINESTRING (10 0, 14 4)");
  auto ring_crossing = ReadWkt("LINEARRING (4 -1, 6 -1, 6 1, 4 1, 4 -1)");
  auto touching_poly = ReadWkt("POLYGON ((2 0, 8 0, 8 4, 2 4, 2 0))");
  auto crossing_poly = ReadWkt("POLYGON ((4 -2, 6 -2, 6 2, 4 2, 4 -2))");

  for (const Geometry* g :
       {pt_on_interior.get(), pt_on_endpoint.get(), mpt_touching.get(),
        crossing_line.get(), touching_line.get(), ring_crossing.get(),
        touching_poly.get(), crossing_poly.get()}) {
    EXPECT_DOUBLE_EQ(PreparedLineStringDistance::distance(prep, g), 0.0);
    EXPECT_DOUBLE_EQ(dist_op.distance(g), 0.0);
    EXPECT_TRUE(dist_op.isWithinDistance(g, 0.0));
    EXPECT_TRUE(dist_op.isWithinDistance(g, -1.0));
    EXPECT_TRUE(dist_op.isWithinDistance(g, 0.5));
  }
}

TEST_F(PreparedLineStringDistanceTest,
       PositiveDistanceForNonArealDisjointInputs) {
  auto line = ReadWkt("LINESTRING (0 0, 10 0)");
  PreparedLineString prep(line.get());
  PreparedLineStringDistance dist_op(prep);

  // Perpendicular projection onto segment interior: distance = 3.0.
  auto pt_above = ReadWkt("POINT (5 3)");
  EXPECT_DOUBLE_EQ(PreparedLineStringDistance::distance(prep, pt_above.get()),
                   3.0);
  EXPECT_TRUE(dist_op.isWithinDistance(pt_above.get(), 3.0));
  EXPECT_TRUE(dist_op.isWithinDistance(pt_above.get(), 3.5));
  EXPECT_FALSE(dist_op.isWithinDistance(pt_above.get(), 2.99));
  EXPECT_FALSE(dist_op.isWithinDistance(pt_above.get(), 0.0));
  EXPECT_FALSE(dist_op.isWithinDistance(pt_above.get(), -1.0));
  EXPECT_TRUE(dist_op.isWithinDistance(pt_above.get(), geos::DoubleInfinity));

  // 3-4-5 right triangle from endpoint (10, 0) to (13, 4): distance = 5.0.
  auto pt_diag = ReadWkt("POINT (13 4)");
  EXPECT_DOUBLE_EQ(dist_op.distance(pt_diag.get()), 5.0);
  EXPECT_TRUE(dist_op.isWithinDistance(pt_diag.get(), 5.0));
  EXPECT_FALSE(dist_op.isWithinDistance(pt_diag.get(), 4.99));

  // Parallel disjoint LineString: distance = 4.0.
  auto parallel_line = ReadWkt("LINESTRING (0 4, 10 4)");
  EXPECT_DOUBLE_EQ(dist_op.distance(parallel_line.get()), 4.0);
  EXPECT_TRUE(dist_op.isWithinDistance(parallel_line.get(), 4.0));
  EXPECT_FALSE(dist_op.isWithinDistance(parallel_line.get(), 3.99));

  // MultiPoint / MultiLineString disjoint from prepared MultiLineString.
  auto mls = ReadWkt("MULTILINESTRING ((0 0, 4 0), (20 0, 24 0))");
  auto mpt = ReadWkt("MULTIPOINT ((2 10), (22 6))");
  PreparedLineString prep_mls(mls.get());
  PreparedLineStringDistance mls_dist(prep_mls);
  EXPECT_DOUBLE_EQ(mls_dist.distance(mpt.get()), 6.0);
  EXPECT_TRUE(mls_dist.isWithinDistance(mpt.get(), 6.0));
  EXPECT_FALSE(mls_dist.isWithinDistance(mpt.get(), 5.9));
}

TEST_F(PreparedLineStringDistanceTest,
       StrictlyContainedInPolygonFallsBackToPointInPolygon) {
  auto poly = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto line_inside = ReadWkt("LINESTRING (2 5, 8 5)");
  auto ring_inside = ReadWkt("LINEARRING (3 3, 7 3, 7 7, 3 7, 3 3)");
  auto mls_partial_inside =
      ReadWkt("MULTILINESTRING ((20 20, 25 25), (2 5, 8 5))");
  auto mpoly = ReadWkt(
      "MULTIPOLYGON (((20 20, 25 20, 25 25, 20 25, 20 20)), "
      "((0 0, 10 0, 10 10, 0 10, 0 0)))");
  auto gc_with_poly = ReadWkt(
      "GEOMETRYCOLLECTION (POINT (100 100), "
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)))");

  // Line boundary facet distance is 2.0, but interior containment gives 0.0.
  PreparedLineString prep_line(line_inside.get());
  PreparedLineStringDistance line_dist(prep_line);
  EXPECT_DOUBLE_EQ(PreparedLineStringDistance::distance(prep_line, poly.get()),
                   0.0);
  EXPECT_DOUBLE_EQ(line_dist.distance(mpoly.get()), 0.0);
  EXPECT_DOUBLE_EQ(line_dist.distance(gc_with_poly.get()), 0.0);
  EXPECT_TRUE(line_dist.isWithinDistance(poly.get(), 0.0));
  EXPECT_TRUE(line_dist.isWithinDistance(poly.get(), -1.0));
  EXPECT_TRUE(line_dist.isWithinDistance(poly.get(), 0.5));
  EXPECT_TRUE(line_dist.isWithinDistance(mpoly.get(), 0.0));
  EXPECT_TRUE(line_dist.isWithinDistance(gc_with_poly.get(), 0.0));

  PreparedLineString prep_ring(ring_inside.get());
  EXPECT_DOUBLE_EQ(PreparedLineStringDistance::distance(prep_ring, poly.get()),
                   0.0);
  EXPECT_TRUE(
      PreparedLineStringDistance(prep_ring).isWithinDistance(poly.get(), 0.0));

  PreparedLineString prep_mls(mls_partial_inside.get());
  EXPECT_DOUBLE_EQ(PreparedLineStringDistance::distance(prep_mls, poly.get()),
                   0.0);
  EXPECT_TRUE(
      PreparedLineStringDistance(prep_mls).isWithinDistance(poly.get(), 0.0));
}

TEST_F(PreparedLineStringDistanceTest,
       OutsidePolygonAndInsideHoleReturnBoundaryDistance) {
  auto poly_with_hole = ReadWkt(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(2 2, 8 2, 8 8, 2 8, 2 2))");
  auto line_in_hole = ReadWkt("LINESTRING (4 5, 6 5)");
  auto line_outside = ReadWkt("LINESTRING (0 -3, 10 -3)");

  // Line inside hole is 2.0 away from hole boundary (y=5 vs y=2 or x=4 vs x=2).
  PreparedLineString prep_hole(line_in_hole.get());
  PreparedLineStringDistance hole_dist(prep_hole);
  EXPECT_DOUBLE_EQ(
      PreparedLineStringDistance::distance(prep_hole, poly_with_hole.get()),
      2.0);
  EXPECT_FALSE(hole_dist.isWithinDistance(poly_with_hole.get(), 1.9));
  EXPECT_FALSE(hole_dist.isWithinDistance(poly_with_hole.get(), 0.0));
  EXPECT_TRUE(hole_dist.isWithinDistance(poly_with_hole.get(), 2.0));

  // Line strictly outside polygon is 3.0 away from bottom edge y=0.
  PreparedLineString prep_out(line_outside.get());
  PreparedLineStringDistance out_dist(prep_out);
  EXPECT_DOUBLE_EQ(out_dist.distance(poly_with_hole.get()), 3.0);
  EXPECT_FALSE(out_dist.isWithinDistance(poly_with_hole.get(), 2.9));
  EXPECT_TRUE(out_dist.isWithinDistance(poly_with_hole.get(), 3.0));
}

}  // namespace
}  // namespace geos_google
