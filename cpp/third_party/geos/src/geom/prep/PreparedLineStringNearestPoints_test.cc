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

#include "third_party/geos/include/geos/geom/prep/PreparedLineStringNearestPoints.h"

#include <cstddef>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/prep/PreparedGeometry.h"
#include "third_party/geos/include/geos/geom/prep/PreparedGeometryFactory.h"
#include "third_party/geos/include/geos/geom/prep/PreparedLineString.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/GEOSException.h"

namespace geos_google {
namespace {

class PreparedLineStringNearestPointsTest : public ::testing::Test {
 protected:
  std::unique_ptr<geos::geom::Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  void ExpectNearestPoints(const std::string& target_wkt,
                           const std::string& query_wkt, double expected_x0,
                           double expected_y0, double expected_x1,
                           double expected_y1) const {
    auto target = ReadWkt(target_wkt);
    auto query = ReadWkt(query_wkt);

    geos::geom::prep::PreparedLineString prep(target.get());
    auto static_pts =
        geos::geom::prep::PreparedLineStringNearestPoints::nearestPoints(
            prep, query.get());
    ASSERT_NE(static_pts, nullptr);
    ASSERT_EQ(static_pts->size(), std::size_t{2});
    EXPECT_NEAR(static_pts->getX(0), expected_x0, 1e-12);
    EXPECT_NEAR(static_pts->getY(0), expected_y0, 1e-12);
    EXPECT_NEAR(static_pts->getX(1), expected_x1, 1e-12);
    EXPECT_NEAR(static_pts->getY(1), expected_y1, 1e-12);

    geos::geom::prep::PreparedLineStringNearestPoints op(prep);
    auto instance_pts = op.nearestPoints(query.get());
    ASSERT_NE(instance_pts, nullptr);
    ASSERT_EQ(instance_pts->size(), std::size_t{2});
    EXPECT_NEAR(instance_pts->getX(0), expected_x0, 1e-12);
    EXPECT_NEAR(instance_pts->getY(0), expected_y0, 1e-12);
    EXPECT_NEAR(instance_pts->getX(1), expected_x1, 1e-12);
    EXPECT_NEAR(instance_pts->getY(1), expected_y1, 1e-12);

    auto facade_pts = prep.nearestPoints(query.get());
    ASSERT_NE(facade_pts, nullptr);
    ASSERT_EQ(facade_pts->size(), std::size_t{2});
    EXPECT_NEAR(facade_pts->getX(0), expected_x0, 1e-12);
    EXPECT_NEAR(facade_pts->getY(0), expected_y0, 1e-12);
    EXPECT_NEAR(facade_pts->getX(1), expected_x1, 1e-12);
    EXPECT_NEAR(facade_pts->getY(1), expected_y1, 1e-12);
  }

  geos::geom::GeometryFactory::Ptr factory_ =
      geos::geom::GeometryFactory::create();
  geos::io::WKTReader reader_{*factory_};
};

TEST_F(PreparedLineStringNearestPointsTest, NearestPointsToPoint) {
  // Perpendicular projection onto segment interior.
  ExpectNearestPoints("LINESTRING (0 0, 10 0)", "POINT (4 3)", 4.0, 0.0, 4.0,
                      3.0);
  // Nearest point at start endpoint.
  ExpectNearestPoints("LINESTRING (0 0, 10 0)", "POINT (-3 4)", 0.0, 0.0, -3.0,
                      4.0);
  // Point lies directly on the line.
  ExpectNearestPoints("LINESTRING (0 0, 10 0)", "POINT (5 0)", 5.0, 0.0, 5.0,
                      0.0);
  // Diagonal segment projection.
  ExpectNearestPoints("LINESTRING (0 0, 4 4)", "POINT (0 4)", 2.0, 2.0, 0.0,
                      4.0);
}

TEST_F(PreparedLineStringNearestPointsTest,
       NearestPointsToLineMultiLineAndRing) {
  // Disjoint lines: closest pair between segments.
  ExpectNearestPoints("LINESTRING (0 0, 10 0)", "LINESTRING (2 5, 8 2)", 8.0,
                      0.0, 8.0, 2.0);
  // Intersecting lines: nearest points coincide at intersection.
  ExpectNearestPoints("LINESTRING (0 0, 10 10)", "LINESTRING (0 10, 10 0)", 5.0,
                      5.0, 5.0, 5.0);
  // Prepared MultiLineString selects closest component.
  ExpectNearestPoints("MULTILINESTRING ((0 0, 2 0), (10 0, 12 0))",
                      "POINT (11 3)", 11.0, 0.0, 11.0, 3.0);
  // Prepared LinearRing.
  ExpectNearestPoints("LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)",
                      "POINT (5 12)", 5.0, 10.0, 5.0, 12.0);
}

TEST_F(PreparedLineStringNearestPointsTest, NearestPointsToPolygonBoundary) {
  // Disjoint line and polygon.
  auto line = ReadWkt("LINESTRING (0 0, 0 10)");
  auto poly = ReadWkt("POLYGON ((4 2, 8 2, 8 8, 4 8, 4 2))");
  geos::geom::prep::PreparedLineString prep(line.get());

  auto pts = geos::geom::prep::PreparedLineStringNearestPoints::nearestPoints(
      prep, poly.get());
  ASSERT_NE(pts, nullptr);
  ASSERT_EQ(pts->size(), std::size_t{2});
  EXPECT_DOUBLE_EQ(pts->getX(0), 0.0);
  EXPECT_DOUBLE_EQ(pts->getX(1), 4.0);
  EXPECT_DOUBLE_EQ(pts->getAt(0).distance(pts->getAt(1)), 4.0);

  // Line crossing polygon boundary yields coincident nearest points.
  ExpectNearestPoints("LINESTRING (0 5, 5 5)",
                      "POLYGON ((2 0, 10 0, 10 10, 2 10, 2 0))", 2.0, 5.0, 2.0,
                      5.0);

  // IndexedFacetDistance computes distance to polygon boundary facets.
  auto inner_line = ReadWkt("LINESTRING (4 5, 6 5)");
  auto outer_poly = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  geos::geom::prep::PreparedLineString prep_inner(inner_line.get());
  auto inner_pts =
      geos::geom::prep::PreparedLineStringNearestPoints::nearestPoints(
          prep_inner, outer_poly.get());
  ASSERT_NE(inner_pts, nullptr);
  ASSERT_EQ(inner_pts->size(), std::size_t{2});
  EXPECT_DOUBLE_EQ(inner_pts->getAt(0).distance(inner_pts->getAt(1)), 4.0);
}

TEST_F(PreparedLineStringNearestPointsTest,
       ReusesCachedFacetDistanceViaFactory) {
  auto line = ReadWkt("LINESTRING (0 0, 10 0, 10 10)");
  auto prepared =
      geos::geom::prep::PreparedGeometryFactory::prepare(line.get());

  auto pt1 = ReadWkt("POINT (3 4)");
  auto pts1 = prepared->nearestPoints(pt1.get());
  ASSERT_NE(pts1, nullptr);
  ASSERT_EQ(pts1->size(), std::size_t{2});
  EXPECT_DOUBLE_EQ(pts1->getX(0), 3.0);
  EXPECT_DOUBLE_EQ(pts1->getY(0), 0.0);
  EXPECT_DOUBLE_EQ(pts1->getX(1), 3.0);
  EXPECT_DOUBLE_EQ(pts1->getY(1), 4.0);

  auto pt2 = ReadWkt("MULTIPOINT ((20 20), (13 6))");
  auto pts2 = prepared->nearestPoints(pt2.get());
  ASSERT_NE(pts2, nullptr);
  ASSERT_EQ(pts2->size(), std::size_t{2});
  EXPECT_DOUBLE_EQ(pts2->getX(0), 10.0);
  EXPECT_DOUBLE_EQ(pts2->getY(0), 6.0);
  EXPECT_DOUBLE_EQ(pts2->getX(1), 13.0);
  EXPECT_DOUBLE_EQ(pts2->getY(1), 6.0);
}

TEST_F(PreparedLineStringNearestPointsTest, EmptyGeometryThrowsGEOSException) {
  auto line = ReadWkt("LINESTRING (0 0, 10 0)");
  geos::geom::prep::PreparedLineString prep(line.get());
  geos::geom::prep::PreparedLineStringNearestPoints op(prep);

  auto empty_pt = ReadWkt("POINT EMPTY");
  auto empty_line = ReadWkt("LINESTRING EMPTY");
  auto empty_poly = ReadWkt("POLYGON EMPTY");
  auto empty_gc = ReadWkt("GEOMETRYCOLLECTION EMPTY");

  EXPECT_THROW(op.nearestPoints(empty_pt.get()), geos::util::GEOSException);
  EXPECT_THROW(op.nearestPoints(empty_line.get()), geos::util::GEOSException);
  EXPECT_THROW(op.nearestPoints(empty_poly.get()), geos::util::GEOSException);
  EXPECT_THROW(op.nearestPoints(empty_gc.get()), geos::util::GEOSException);

  geos::geom::prep::PreparedLineString empty_prep(empty_line.get());
  auto pt = ReadWkt("POINT (1 1)");
  EXPECT_THROW(geos::geom::prep::PreparedLineStringNearestPoints::nearestPoints(
                   empty_prep, pt.get()),
               geos::util::GEOSException);
}

}  // namespace
}  // namespace geos_google
