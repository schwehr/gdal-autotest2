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

#include "third_party/geos/include/geos/geom/prep/PreparedLineStringIntersects.h"

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/prep/PreparedGeometry.h"
#include "third_party/geos/include/geos/geom/prep/PreparedGeometryFactory.h"
#include "third_party/geos/include/geos/geom/prep/PreparedLineString.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

class PreparedLineStringIntersectsTest : public ::testing::Test {
 protected:
  std::unique_ptr<geos::geom::Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  void ExpectIntersects(const std::string& target_wkt,
                        const std::string& test_wkt, bool expected) const {
    auto target = ReadWkt(target_wkt);
    auto test = ReadWkt(test_wkt);

    geos::geom::prep::PreparedLineString prep_static(target.get());
    EXPECT_EQ(geos::geom::prep::PreparedLineStringIntersects::intersects(
                  prep_static, test.get()),
              expected)
        << "static intersects(" << target_wkt << ", " << test_wkt << ")";

    geos::geom::prep::PreparedLineString prep_instance(target.get());
    geos::geom::prep::PreparedLineStringIntersects op(prep_instance);
    EXPECT_EQ(op.intersects(test.get()), expected)
        << "instance intersects(" << target_wkt << ", " << test_wkt << ")";

    EXPECT_EQ(prep_instance.intersects(test.get()), expected)
        << "PreparedLineString::intersects(" << target_wkt << ", " << test_wkt
        << ")";

    auto prepared_via_factory =
        geos::geom::prep::PreparedGeometryFactory::prepare(target.get());
    EXPECT_EQ(prepared_via_factory->intersects(test.get()), expected)
        << "factory prepared intersects(" << target_wkt << ", " << test_wkt
        << ")";
  }

  geos::geom::GeometryFactory::Ptr factory_ =
      geos::geom::GeometryFactory::create();
  geos::io::WKTReader reader_{*factory_};
};

TEST_F(PreparedLineStringIntersectsTest, LineVsLineCrossingAndTouching) {
  // Proper interior crossing.
  ExpectIntersects("LINESTRING (0 0, 10 10)", "LINESTRING (0 10, 10 0)", true);
  // Touching at endpoints.
  ExpectIntersects("LINESTRING (0 0, 5 0)", "LINESTRING (5 0, 5 5)", true);
  // T-junction on segment interior.
  ExpectIntersects("LINESTRING (0 0, 10 0)", "LINESTRING (5 0, 5 5)", true);
  // Collinear overlap.
  ExpectIntersects("LINESTRING (0 0, 10 0)", "LINESTRING (3 0, 8 0)", true);
  // Multi-segment polyline intersection.
  ExpectIntersects("LINESTRING (0 0, 4 4, 8 0, 12 4)", "LINESTRING (2 3, 10 1)",
                   true);
}

TEST_F(PreparedLineStringIntersectsTest, LineVsLineDisjoint) {
  // Overlapping envelopes, disjoint segments.
  ExpectIntersects("LINESTRING (0 0, 10 10)", "LINESTRING (0 2, 8 10)", false);
  // Non-overlapping envelopes (direct call still runs segment check).
  ExpectIntersects("LINESTRING (0 0, 5 5)", "LINESTRING (20 20, 30 30)", false);
}

TEST_F(PreparedLineStringIntersectsTest, LineVsPolygonBoundaryIntersection) {
  // Crossing exterior ring.
  ExpectIntersects("LINESTRING (-2 5, 5 5)",
                   "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))", true);
  // Touching exterior vertex.
  ExpectIntersects("LINESTRING (-5 -5, 0 0)",
                   "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))", true);
  // Collinear along polygon boundary edge.
  ExpectIntersects("LINESTRING (2 0, 8 0)",
                   "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))", true);
  // Crossing polygon hole boundary.
  ExpectIntersects(
      "LINESTRING (1 5, 5 5)",
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 8 2, 8 8, 2 8, 2 2))",
      true);
}

TEST_F(PreparedLineStringIntersectsTest, LineStrictlyInsidePolygonOrHole) {
  // Properly contained inside polygon exterior (no segment intersection).
  ExpectIntersects("LINESTRING (2 2, 8 8)",
                   "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))", true);
  // Strictly inside shell and outside hole.
  ExpectIntersects(
      "LINESTRING (1 1, 1 9)",
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 8 2, 8 8, 2 8, 2 2))",
      true);
  // Strictly inside polygon hole (envelope overlaps, not contained in polygon).
  ExpectIntersects(
      "LINESTRING (3 3, 7 7)",
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 8 2, 8 8, 2 8, 2 2))",
      false);
  // Outside polygon with overlapping envelope.
  ExpectIntersects("LINESTRING (0 8, 2 10)",
                   "POLYGON ((4 0, 10 0, 10 6, 4 6, 4 0))", false);
  // Outside polygon with disjoint envelope.
  ExpectIntersects("LINESTRING (0 0, 2 2)",
                   "POLYGON ((10 10, 20 10, 20 20, 10 20, 10 10))", false);
}

TEST_F(PreparedLineStringIntersectsTest, LineVsMultiPolygon) {
  ExpectIntersects("LINESTRING (12 2, 18 8)",
                   "MULTIPOLYGON (((0 0, 5 0, 5 5, 0 5, 0 0)), "
                   "((10 0, 20 0, 20 10, 10 10, 10 0)))",
                   true);
  ExpectIntersects("LINESTRING (6 1, 9 4)",
                   "MULTIPOLYGON (((0 0, 5 0, 5 5, 0 5, 0 0)), "
                   "((10 0, 20 0, 20 10, 10 10, 10 0)))",
                   false);
}

TEST_F(PreparedLineStringIntersectsTest, LineVsPointAndMultiPoint) {
  // Endpoint of line.
  ExpectIntersects("LINESTRING (0 0, 10 0)", "POINT (0 0)", true);
  // Interior point of segment.
  ExpectIntersects("LINESTRING (0 0, 10 0)", "POINT (5 0)", true);
  // Off-line point inside envelope.
  ExpectIntersects("LINESTRING (0 0, 10 10)", "POINT (0 10)", false);
  // Off-line point outside envelope.
  ExpectIntersects("LINESTRING (0 0, 10 10)", "POINT (20 20)", false);
  // MultiPoint where first point misses and second hits.
  ExpectIntersects("LINESTRING (0 0, 10 0)", "MULTIPOINT ((5 5), (5 0))", true);
  // MultiPoint where all points miss.
  ExpectIntersects("LINESTRING (0 0, 10 0)",
                   "MULTIPOINT ((1 1), (5 -2), (9 3))", false);
}

TEST_F(PreparedLineStringIntersectsTest, LinearRingAndMultiLineStringTargets) {
  // LinearRing is 1D; interior of ring does not intersect the ring.
  ExpectIntersects("LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)", "POINT (5 5)",
                   false);
  ExpectIntersects("LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)", "POINT (0 5)",
                   true);
  ExpectIntersects("LINEARRING (2 2, 8 2, 8 8, 2 8, 2 2)",
                   "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))", true);
  ExpectIntersects("LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)",
                   "LINESTRING (-1 5, 1 5)", true);

  // MultiLineString with one component outside and one strictly inside polygon.
  ExpectIntersects("MULTILINESTRING ((20 20, 25 25), (2 2, 8 8))",
                   "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))", true);
  // MultiLineString vs MultiLineString hitting second component.
  ExpectIntersects("MULTILINESTRING ((0 0, 4 0), (10 0, 14 0))",
                   "MULTILINESTRING ((0 5, 4 5), (12 -2, 12 2))", true);
  // Point on vertex shared by two MultiLineString components.
  ExpectIntersects("MULTILINESTRING ((0 0, 5 0), (5 0, 10 0))", "POINT (5 0)",
                   true);
  // LineString vs MultiLineString hitting second component.
  ExpectIntersects("LINESTRING (0 0, 10 0)",
                   "MULTILINESTRING ((0 5, 10 5), (5 -2, 5 2))", true);
}

TEST_F(PreparedLineStringIntersectsTest, MixedDimensionGeometryCollection) {
  // Disjoint polygon + intersecting point in same GeometryCollection:
  // fails segment check and L/A check, succeeds on L/P fallback.
  ExpectIntersects(
      "LINESTRING (0 0, 10 0)",
      "GEOMETRYCOLLECTION (POLYGON ((20 20, 30 20, 30 30, 20 30, 20 20)), "
      "POINT (5 0))",
      true);
  // Disjoint polygon + disjoint point in same GeometryCollection.
  ExpectIntersects(
      "LINESTRING (0 0, 10 0)",
      "GEOMETRYCOLLECTION (POLYGON ((20 20, 30 20, 30 30, 20 30, 20 20)), "
      "POINT (5 5))",
      false);
  // Disjoint line + intersecting point in GeometryCollection.
  ExpectIntersects(
      "LINESTRING (0 0, 10 0)",
      "GEOMETRYCOLLECTION (LINESTRING (20 20, 30 30), POINT (5 0))", true);
  // Polygon in GeometryCollection strictly containing target line.
  ExpectIntersects(
      "LINESTRING (2 2, 8 8)",
      "GEOMETRYCOLLECTION (POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)))", true);
}

TEST_F(PreparedLineStringIntersectsTest, EmptyGeometries) {
  ExpectIntersects("LINESTRING (0 0, 10 0)", "POINT EMPTY", false);
  ExpectIntersects("LINESTRING (0 0, 10 0)", "MULTIPOINT EMPTY", false);
  ExpectIntersects("LINESTRING (0 0, 10 0)", "LINESTRING EMPTY", false);
  ExpectIntersects("LINESTRING (0 0, 10 0)", "POLYGON EMPTY", false);
  ExpectIntersects("LINESTRING (0 0, 10 0)", "GEOMETRYCOLLECTION EMPTY", false);

  ExpectIntersects("LINESTRING EMPTY", "POINT (0 0)", false);
  ExpectIntersects("LINESTRING EMPTY", "LINESTRING (0 0, 1 1)", false);
  ExpectIntersects("LINESTRING EMPTY", "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
                   false);
  ExpectIntersects("LINESTRING EMPTY", "POINT EMPTY", false);
  ExpectIntersects("LINESTRING EMPTY", "LINESTRING EMPTY", false);

  ExpectIntersects("MULTILINESTRING EMPTY", "POINT (0 0)", false);
  ExpectIntersects("MULTILINESTRING EMPTY", "LINESTRING (0 0, 1 1)", false);
  ExpectIntersects("MULTILINESTRING EMPTY",
                   "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))", false);
}

TEST_F(PreparedLineStringIntersectsTest, ReusesPreparedIndexAcrossQueries) {
  auto target = ReadWkt("LINESTRING (0 0, 10 0, 10 10)");
  geos::geom::prep::PreparedLineString prep(target.get());
  geos::geom::prep::PreparedLineStringIntersects op(prep);

  auto crossing = ReadWkt("LINESTRING (5 -5, 5 5)");
  auto disjoint_line = ReadWkt("LINESTRING (0 5, 5 5)");
  auto enclosing_poly =
      ReadWkt("POLYGON ((-1 -1, 11 -1, 11 11, -1 11, -1 -1))");
  auto point_on = ReadWkt("POINT (10 5)");
  auto point_off = ReadWkt("POINT (3 3)");

  EXPECT_TRUE(op.intersects(crossing.get()));
  EXPECT_FALSE(op.intersects(disjoint_line.get()));
  EXPECT_TRUE(op.intersects(enclosing_poly.get()));
  EXPECT_TRUE(op.intersects(point_on.get()));
  EXPECT_FALSE(op.intersects(point_off.get()));
}

TEST_F(PreparedLineStringIntersectsTest, CurvedGeometryThrowsOnPreparedFacade) {
  auto target = ReadWkt("LINESTRING (0 0, 2 0)");
  auto circular = ReadWkt("CIRCULARSTRING (0 0, 1 1, 2 0)");
  geos::geom::prep::PreparedLineString prep(target.get());

  EXPECT_THROW(prep.intersects(circular.get()),
               geos::util::UnsupportedOperationException);
}

}  // namespace
}  // namespace geos_google
