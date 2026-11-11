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

#include "third_party/geos/include/geos/geom/prep/PreparedPoint.h"

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/prep/PreparedGeometry.h"
#include "third_party/geos/include/geos/geom/prep/PreparedGeometryFactory.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

class PreparedPointTest : public ::testing::Test {
 protected:
  std::unique_ptr<geos::geom::Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  void ExpectIntersects(const std::string& target_wkt,
                        const std::string& test_wkt, bool expected) const {
    auto target = ReadWkt(target_wkt);
    auto test = ReadWkt(test_wkt);

    geos::geom::prep::PreparedPoint prep_direct(target.get());
    EXPECT_EQ(prep_direct.intersects(test.get()), expected)
        << "PreparedPoint::intersects(" << target_wkt << ", " << test_wkt
        << ")";

    auto prep_factory =
        geos::geom::prep::PreparedGeometryFactory::prepare(target.get());
    EXPECT_EQ(prep_factory->intersects(test.get()), expected)
        << "factory prepared intersects(" << target_wkt << ", " << test_wkt
        << ")";
  }

  geos::geom::GeometryFactory::Ptr factory_ =
      geos::geom::GeometryFactory::create();
  geos::io::WKTReader reader_{*factory_};
};

TEST_F(PreparedPointTest, PointIntersectsPointAndMultiPoint) {
  ExpectIntersects("POINT (3 4)", "POINT (3 4)", true);
  ExpectIntersects("POINT (3 4)", "POINT (3 5)", false);
  // MultiPoint containing matching coordinate after a non-matching one.
  ExpectIntersects("POINT (5 5)", "MULTIPOINT ((1 1), (5 5))", true);
  // Point falls inside MultiPoint envelope, but matches no component point.
  ExpectIntersects("POINT (5 5)", "MULTIPOINT ((0 0), (10 10))", false);
}

TEST_F(PreparedPointTest, PointIntersectsLineStringAndLinearRing) {
  // Endpoint and interior of LineString.
  ExpectIntersects("POINT (0 0)", "LINESTRING (0 0, 10 10)", true);
  ExpectIntersects("POINT (5 5)", "LINESTRING (0 0, 10 10)", true);
  // Inside LineString envelope, off the segment.
  ExpectIntersects("POINT (0 10)", "LINESTRING (0 0, 10 10)", false);
  // Outside LineString envelope.
  ExpectIntersects("POINT (20 20)", "LINESTRING (0 0, 10 10)", false);

  // LinearRing boundary vs interior.
  ExpectIntersects("POINT (0 5)", "LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)",
                   true);
  ExpectIntersects("POINT (5 5)", "LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)",
                   false);
}

TEST_F(PreparedPointTest, PointIntersectsPolygonAndMultiPolygon) {
  const std::string poly_with_hole =
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 8 2, 8 8, 2 8, 2 2))";

  // Strictly inside shell outside hole.
  ExpectIntersects("POINT (1 1)", poly_with_hole, true);
  // On exterior boundary vertex and edge.
  ExpectIntersects("POINT (0 0)", poly_with_hole, true);
  ExpectIntersects("POINT (5 0)", poly_with_hole, true);
  // On hole boundary.
  ExpectIntersects("POINT (2 2)", poly_with_hole, true);
  // Strictly inside hole (envelope intersects, point in exterior).
  ExpectIntersects("POINT (5 5)", poly_with_hole, false);
  // Outside envelope.
  ExpectIntersects("POINT (-1 5)", poly_with_hole, false);

  // MultiPolygon cases.
  const std::string multi_poly =
      "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
      "((10 0, 14 0, 14 4, 10 4, 10 0)))";
  ExpectIntersects("POINT (12 2)", multi_poly, true);
  // Between the two polygons (envelope intersects, no polygon intersects).
  ExpectIntersects("POINT (7 2)", multi_poly, false);
}

TEST_F(PreparedPointTest, MultiPointTarget) {
  ExpectIntersects("MULTIPOINT ((0 0), (5 5))", "POINT (5 5)", true);
  ExpectIntersects("MULTIPOINT ((0 0), (10 10))", "POINT (5 5)", false);
  ExpectIntersects("MULTIPOINT ((0 0), (10 10))", "LINESTRING (1 1, 9 9)",
                   false);
  ExpectIntersects("MULTIPOINT ((0 0), (5 5))",
                   "POLYGON ((4 4, 6 4, 6 6, 4 6, 4 4))", true);
  ExpectIntersects("MULTIPOINT ((0 0), (10 10))",
                   "POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))", false);
  ExpectIntersects("MULTIPOINT ((0 0), (5 5))", "POINT (20 20)", false);
}

TEST_F(PreparedPointTest, GeometryCollectionTestTarget) {
  ExpectIntersects(
      "POINT (5 5)",
      "GEOMETRYCOLLECTION (POLYGON ((20 20, 30 20, 30 30, 20 30, 20 20)), "
      "POINT (5 5))",
      true);
  ExpectIntersects("POINT (5 5)",
                   "GEOMETRYCOLLECTION (LINESTRING (0 0, 10 0), "
                   "POLYGON ((20 20, 30 20, 30 30, 20 30, 20 20)))",
                   false);
}

TEST_F(PreparedPointTest, EmptyGeometries) {
  ExpectIntersects("POINT (1 1)", "POINT EMPTY", false);
  ExpectIntersects("POINT (1 1)", "MULTIPOINT EMPTY", false);
  ExpectIntersects("POINT (1 1)", "LINESTRING EMPTY", false);
  ExpectIntersects("POINT (1 1)", "POLYGON EMPTY", false);
  ExpectIntersects("POINT (1 1)", "GEOMETRYCOLLECTION EMPTY", false);

  ExpectIntersects("POINT EMPTY", "POINT (1 1)", false);
  ExpectIntersects("POINT EMPTY", "LINESTRING (0 0, 2 2)", false);
  ExpectIntersects("POINT EMPTY", "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))", false);
  ExpectIntersects("POINT EMPTY", "POINT EMPTY", false);

  ExpectIntersects("MULTIPOINT EMPTY", "POINT (1 1)", false);
  ExpectIntersects("MULTIPOINT EMPTY", "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
                   false);
}

TEST_F(PreparedPointTest, CurvedGeometryThrowsUnsupportedOperation) {
  auto pt = ReadWkt("POINT (1 1)");
  geos::geom::prep::PreparedPoint prep(pt.get());

  auto circular = ReadWkt("CIRCULARSTRING (0 0, 1 1, 2 0)");
  auto compound =
      ReadWkt("COMPOUNDCURVE ((0 0, 1 0), CIRCULARSTRING (1 0, 2 1, 3 0))");
  auto gc_curved =
      ReadWkt("GEOMETRYCOLLECTION (CIRCULARSTRING (0 0, 1 1, 2 0))");

  EXPECT_THROW(prep.intersects(circular.get()),
               geos::util::UnsupportedOperationException);
  EXPECT_THROW(prep.intersects(compound.get()),
               geos::util::UnsupportedOperationException);
  EXPECT_THROW(prep.intersects(gc_curved.get()),
               geos::util::UnsupportedOperationException);
}

TEST_F(PreparedPointTest, FactoryProducesPreparedPointAndReusesAcrossQueries) {
  auto pt = ReadWkt("POINT (5 5)");
  auto prep_pt = geos::geom::prep::PreparedGeometryFactory::prepare(pt.get());
  EXPECT_NE(dynamic_cast<const geos::geom::prep::PreparedPoint*>(prep_pt.get()),
            nullptr);
  EXPECT_EQ(&prep_pt->getGeometry(), pt.get());

  auto inside = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto outside = ReadWkt("POLYGON ((20 20, 30 20, 30 30, 20 30, 20 20))");
  EXPECT_TRUE(prep_pt->intersects(inside.get()));
  EXPECT_FALSE(prep_pt->disjoint(inside.get()));
  EXPECT_FALSE(prep_pt->intersects(outside.get()));
  EXPECT_TRUE(prep_pt->disjoint(outside.get()));

  auto mpt = ReadWkt("MULTIPOINT ((0 0), (5 5))");
  auto prep_mpt = geos::geom::prep::PreparedGeometryFactory::prepare(mpt.get());
  EXPECT_NE(
      dynamic_cast<const geos::geom::prep::PreparedPoint*>(prep_mpt.get()),
      nullptr);
  EXPECT_TRUE(prep_mpt->intersects(inside.get()));
  EXPECT_FALSE(prep_mpt->intersects(outside.get()));
}

}  // namespace
}  // namespace geos_google
