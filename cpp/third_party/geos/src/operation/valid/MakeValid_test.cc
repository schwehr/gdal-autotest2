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

#include "third_party/geos/include/geos/operation/valid/MakeValid.h"

#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/valid/IsValidOp.h"
#include "third_party/geos/include/geos/util/GEOSException.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/Interrupt.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::io::WKTReader;
using geos::operation::valid::IsValidOp;
using geos::operation::valid::MakeValid;

class MakeValidTest : public ::testing::Test {
 protected:
  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader reader_{*factory_};

  std::unique_ptr<Geometry> ParseWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }
};

TEST_F(MakeValidTest, ValidGeometriesReturnedAsValidClones) {
  const std::vector<std::string> valid_wkts = {
      "POINT EMPTY",
      "POINT (1 2)",
      "MULTIPOINT EMPTY",
      "MULTIPOINT ((0 0), (3 4))",
      "LINESTRING EMPTY",
      "LINESTRING (0 0, 5 5, 10 0)",
      "LINESTRING (0 0, 10 10, 10 0, 0 10)",
      "LINESTRING Z (0 0 1, 3 4 2)",
      "MULTILINESTRING EMPTY",
      "MULTILINESTRING ((0 0, 1 1), (2 2, 3 3))",
      "POLYGON EMPTY",
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 8, 8 8, 8 2, 2 2))",
      "MULTIPOLYGON EMPTY",
      "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
      "((10 10, 14 10, 14 14, 10 14, 10 10)))",
      "GEOMETRYCOLLECTION EMPTY",
      "GEOMETRYCOLLECTION (POINT (1 2), LINESTRING (0 0, 3 4))",
      "LINEARRING (0 0, 4 0, 4 4, 0 4, 0 0)",
  };

  MakeValid maker;
  for (const std::string& wkt : valid_wkts) {
    SCOPED_TRACE(wkt);
    std::unique_ptr<Geometry> input = ParseWkt(wkt);
    ASSERT_TRUE(IsValidOp::isValid(input.get()));

    std::unique_ptr<Geometry> output = maker.build(input.get());
    ASSERT_NE(output, nullptr);
    EXPECT_NE(output.get(), input.get());
    EXPECT_TRUE(IsValidOp::isValid(output.get()));
    EXPECT_EQ(output->getGeometryTypeId(), input->getGeometryTypeId());
    EXPECT_EQ(output->hasZ(), input->hasZ());
    EXPECT_EQ(output->hasM(), input->hasM());
    EXPECT_TRUE(output->equalsExact(input.get()));
  }
}

TEST_F(MakeValidTest, InvalidCollapsedLineStringBecomesPoint) {
  {
    std::unique_ptr<Geometry> line = ParseWkt("LINESTRING (3 4, 3 4)");
    ASSERT_FALSE(line->isValid());

    std::unique_ptr<Geometry> result = MakeValid().build(line.get());
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->isValid());
    EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_POINT);
    std::unique_ptr<Geometry> expected = ParseWkt("POINT (3 4)");
    EXPECT_TRUE(result->equalsExact(expected.get()));
  }
  {
    std::unique_ptr<Geometry> line =
        ParseWkt("LINESTRING (1 2, 1 2, 1 2, 1 2)");
    ASSERT_FALSE(line->isValid());

    std::unique_ptr<Geometry> result = MakeValid().build(line.get());
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->isValid());
    EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_POINT);
    std::unique_ptr<Geometry> expected = ParseWkt("POINT (1 2)");
    EXPECT_TRUE(result->equalsExact(expected.get()));
  }
}

TEST_F(MakeValidTest, InvalidMultiLineStringAllCollapsedToMultiplePoints) {
  std::unique_ptr<Geometry> mls =
      ParseWkt("MULTILINESTRING ((0 0, 0 0), (5 5, 5 5))");
  ASSERT_FALSE(mls->isValid());

  std::unique_ptr<Geometry> result = MakeValid().build(mls.get());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(result->getNumGeometries(), 2u);
  std::unique_ptr<Geometry> expected = ParseWkt("MULTIPOINT ((0 0), (5 5))");
  EXPECT_TRUE(result->equals(expected.get()));
}

TEST_F(MakeValidTest, InvalidMultiLineStringWithEmptyAndSingleCollapsedPoint) {
  std::unique_ptr<Geometry> mls =
      ParseWkt("MULTILINESTRING (EMPTY, (2 3, 2 3))");
  ASSERT_FALSE(mls->isValid());

  std::unique_ptr<Geometry> result = MakeValid().build(mls.get());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_POINT);
  std::unique_ptr<Geometry> expected = ParseWkt("POINT (2 3)");
  EXPECT_TRUE(result->equalsExact(expected.get()));
}

TEST_F(MakeValidTest, InvalidMultiLineStringMixedSinglePointAndSingleLine) {
  std::unique_ptr<Geometry> mls =
      ParseWkt("MULTILINESTRING ((0 0, 0 0), (1 1, 4 5))");
  ASSERT_FALSE(mls->isValid());

  std::unique_ptr<Geometry> result = MakeValid().build(mls.get());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(result->getNumGeometries(), 2u);
  EXPECT_EQ(result->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_POINT);
  EXPECT_EQ(result->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(
      result->getGeometryN(0)->equalsExact(ParseWkt("POINT (0 0)").get()));
  EXPECT_TRUE(result->getGeometryN(1)->equalsExact(
      ParseWkt("LINESTRING (1 1, 4 5)").get()));
}

TEST_F(MakeValidTest, InvalidMultiLineStringWithSelfCrossingComponent) {
  std::unique_ptr<Geometry> mls = ParseWkt(
      "MULTILINESTRING ((0 0, 0 0), (10 10, 10 10), (0 0, 10 10, 10 0, 0 10))");
  ASSERT_FALSE(mls->isValid());

  std::unique_ptr<Geometry> result = MakeValid().build(mls.get());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(result->getNumGeometries(), 2u);
  EXPECT_EQ(result->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(result->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_MULTILINESTRING);
  EXPECT_TRUE(result->getGeometryN(0)->equals(
      ParseWkt("MULTIPOINT ((0 0), (10 10))").get()));
  EXPECT_NEAR(result->getGeometryN(1)->getLength(),
              10.0 + 2.0 * std::hypot(10.0, 10.0), 1e-9);
}

TEST_F(MakeValidTest, InvalidPolygonBowtieSplitsIntoMultiPolygon) {
  std::unique_ptr<Geometry> poly =
      ParseWkt("POLYGON ((0 0, 10 10, 10 0, 0 10, 0 0))");
  ASSERT_FALSE(poly->isValid());

  std::unique_ptr<Geometry> result = MakeValid().build(poly.get());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(result->getNumGeometries(), 2u);
  EXPECT_NEAR(result->getArea(), 50.0, 1e-9);
}

TEST_F(MakeValidTest, InvalidPolygonSelfTouchingShellFormsValidHole) {
  std::unique_ptr<Geometry> poly =
      ParseWkt("POLYGON ((0 0, 10 0, 10 10, 5 10, 8 4, 2 4, 5 10, 0 10, 0 0))");
  ASSERT_FALSE(poly->isValid());

  std::unique_ptr<Geometry> result = MakeValid().build(poly.get());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  const auto* out_poly = dynamic_cast<const geos::geom::Polygon*>(result.get());
  ASSERT_NE(out_poly, nullptr);
  EXPECT_EQ(out_poly->getNumInteriorRing(), 1u);
  EXPECT_NEAR(out_poly->getArea(), 82.0, 1e-9);
}

TEST_F(MakeValidTest, InvalidPolygonWithHoleOutsideShell) {
  std::unique_ptr<Geometry> poly = ParseWkt(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(20 20, 30 20, 30 30, 20 30, 20 20))");
  ASSERT_FALSE(poly->isValid());

  std::unique_ptr<Geometry> result = MakeValid().build(poly.get());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(result->getNumGeometries(), 2u);
  EXPECT_NEAR(result->getArea(), 200.0, 1e-9);
}

TEST_F(MakeValidTest, InvalidPolygonWithNestedHolesCreatesIsland) {
  std::unique_ptr<Geometry> poly = ParseWkt(
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
      "(2 2, 18 2, 18 18, 2 18, 2 2), "
      "(6 6, 14 6, 14 14, 6 14, 6 6))");
  ASSERT_FALSE(poly->isValid());

  std::unique_ptr<Geometry> result = MakeValid().build(poly.get());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(result->getNumGeometries(), 2u);
  EXPECT_NEAR(result->getArea(), 208.0, 1e-9);
}

TEST_F(MakeValidTest, InvalidPolygonWithDisconnectedInteriorSplitsArea) {
  std::unique_ptr<Geometry> poly = ParseWkt(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(5 0, 10 5, 5 10, 0 5, 5 0))");
  ASSERT_FALSE(poly->isValid());

  std::unique_ptr<Geometry> result = MakeValid().build(poly.get());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(result->getNumGeometries(), 4u);
  EXPECT_NEAR(result->getArea(), 50.0, 1e-9);
}

TEST_F(MakeValidTest, InvalidMultiPolygonOverlappingUsesSymDifference) {
  std::unique_ptr<Geometry> mp = ParseWkt(
      "MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "((5 5, 15 5, 15 15, 5 15, 5 5)))");
  ASSERT_FALSE(mp->isValid());

  std::unique_ptr<Geometry> result = MakeValid().build(mp.get());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(result->getNumGeometries(), 2u);
  EXPECT_NEAR(result->getArea(), 150.0, 1e-9);
}

TEST_F(MakeValidTest, InvalidMultiPolygonWithEmptyComponentAndBowtie) {
  std::unique_ptr<Geometry> mp =
      ParseWkt("MULTIPOLYGON (EMPTY, ((0 0, 10 10, 10 0, 0 10, 0 0)))");
  ASSERT_FALSE(mp->isValid());

  std::unique_ptr<Geometry> result = MakeValid().build(mp.get());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_NEAR(result->getArea(), 50.0, 1e-9);
}

TEST_F(MakeValidTest, InvalidPolygonCollapsedCompletelyToPoint) {
  std::unique_ptr<Geometry> poly = ParseWkt("POLYGON ((2 3, 2 3, 2 3, 2 3))");
  ASSERT_FALSE(poly->isValid());

  std::unique_ptr<Geometry> result = MakeValid().build(poly.get());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_TRUE(result->equalsExact(ParseWkt("POINT (2 3)").get()));
}

TEST_F(MakeValidTest, InvalidPolygonCollapsedCompletelyToLine) {
  {
    std::unique_ptr<Geometry> poly =
        ParseWkt("POLYGON ((0 0, 10 0, 10 0, 0 0))");
    ASSERT_FALSE(poly->isValid());

    std::unique_ptr<Geometry> result = MakeValid().build(poly.get());
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->isValid());
    EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
    EXPECT_NEAR(result->getLength(), 10.0, 1e-9);
    EXPECT_TRUE(result->equals(ParseWkt("LINESTRING (0 0, 10 0)").get()));
  }
  {
    // Intermediate vertex (5 0) splits the noded boundary into two segments.
    std::unique_ptr<Geometry> poly =
        ParseWkt("POLYGON ((0 0, 10 0, 5 0, 0 0))");
    ASSERT_FALSE(poly->isValid());

    std::unique_ptr<Geometry> result = MakeValid().build(poly.get());
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->isValid());
    EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
    EXPECT_EQ(result->getNumGeometries(), 2u);
    EXPECT_NEAR(result->getLength(), 10.0, 1e-9);
    EXPECT_TRUE(result->equals(
        ParseWkt("MULTILINESTRING ((0 0, 5 0), (5 0, 10 0))").get()));
  }
}

TEST_F(MakeValidTest, InvalidPolygonWithDanglingSpikeRetainsAreaAndLine) {
  std::unique_ptr<Geometry> poly =
      ParseWkt("POLYGON ((0 0, 10 0, 15 0, 10 0, 10 10, 0 10, 0 0))");
  ASSERT_FALSE(poly->isValid());

  std::unique_ptr<Geometry> result = MakeValid().build(poly.get());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(result->getNumGeometries(), 2u);
  EXPECT_EQ(result->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_POLYGON);
  EXPECT_NEAR(result->getGeometryN(0)->getArea(), 100.0, 1e-9);
  EXPECT_EQ(result->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_LINESTRING);
  EXPECT_NEAR(result->getGeometryN(1)->getLength(), 5.0, 1e-9);
}

TEST_F(MakeValidTest, InvalidPolygonWithCollapsedHoleRetainsAreaAndPoint) {
  std::unique_ptr<Geometry> poly =
      ParseWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (5 5, 5 5, 5 5, 5 5))");
  ASSERT_FALSE(poly->isValid());

  std::unique_ptr<Geometry> result = MakeValid().build(poly.get());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(result->getNumGeometries(), 2u);
  EXPECT_EQ(result->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_POLYGON);
  EXPECT_NEAR(result->getGeometryN(0)->getArea(), 100.0, 1e-9);
  EXPECT_EQ(result->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_POINT);
  EXPECT_TRUE(
      result->getGeometryN(1)->equalsExact(ParseWkt("POINT (5 5)").get()));
}

TEST_F(MakeValidTest,
       InvalidPolygonWithSpikeAndCollapsedHoleProducesThreeParts) {
  std::unique_ptr<Geometry> poly = ParseWkt(
      "POLYGON ((0 0, 10 0, 15 0, 10 0, 10 10, 0 10, 0 0), "
      "(5 5, 5 5, 5 5, 5 5))");
  ASSERT_FALSE(poly->isValid());

  std::unique_ptr<Geometry> result = MakeValid().build(poly.get());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(result->getNumGeometries(), 3u);
  EXPECT_EQ(result->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_POLYGON);
  EXPECT_NEAR(result->getGeometryN(0)->getArea(), 100.0, 1e-9);
  EXPECT_EQ(result->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_LINESTRING);
  EXPECT_NEAR(result->getGeometryN(1)->getLength(), 5.0, 1e-9);
  EXPECT_EQ(result->getGeometryN(2)->getGeometryTypeId(),
            geos::geom::GEOS_POINT);
  EXPECT_TRUE(
      result->getGeometryN(2)->equalsExact(ParseWkt("POINT (5 5)").get()));
}

TEST_F(MakeValidTest,
       InvalidPolygonWithCollapsedLineShellAndCollapsedPointHole) {
  std::unique_ptr<Geometry> poly =
      ParseWkt("POLYGON ((0 0, 10 0, 10 0, 0 0), (5 5, 5 5, 5 5, 5 5))");
  ASSERT_FALSE(poly->isValid());

  std::unique_ptr<Geometry> result = MakeValid().build(poly.get());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(result->getNumGeometries(), 2u);
  EXPECT_EQ(result->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_LINESTRING);
  EXPECT_NEAR(result->getGeometryN(0)->getLength(), 10.0, 1e-9);
  EXPECT_TRUE(result->getGeometryN(0)->equals(
      ParseWkt("LINESTRING (0 0, 10 0)").get()));
  EXPECT_EQ(result->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_POINT);
  EXPECT_TRUE(
      result->getGeometryN(1)->equalsExact(ParseWkt("POINT (5 5)").get()));
}

TEST_F(MakeValidTest,
       InvalidPolygonWithSharedEdgeHolesRebuildsInMultipleIterations) {
  // Two interior holes sharing an edge require two BuildArea/symdif passes:
  // first pass absorbs the full outer box, second carves out the merged hole
  // and leaves the shared edge as a cut line.
  std::unique_ptr<Geometry> poly = ParseWkt(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(2 2, 5 2, 5 8, 2 8, 2 2), "
      "(5 2, 8 2, 8 8, 5 8, 5 2))");
  ASSERT_FALSE(poly->isValid());

  std::unique_ptr<Geometry> result = MakeValid().build(poly.get());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(result->getNumGeometries(), 2u);
  EXPECT_EQ(result->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_POLYGON);
  const auto* out_poly =
      dynamic_cast<const geos::geom::Polygon*>(result->getGeometryN(0));
  ASSERT_NE(out_poly, nullptr);
  EXPECT_EQ(out_poly->getNumInteriorRing(), 1u);
  EXPECT_NEAR(out_poly->getArea(), 64.0, 1e-9);
  EXPECT_EQ(result->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_LINESTRING);
  EXPECT_NEAR(result->getGeometryN(1)->getLength(), 6.0, 1e-9);
  EXPECT_TRUE(
      result->getGeometryN(1)->equals(ParseWkt("LINESTRING (5 2, 5 8)").get()));
}

TEST_F(MakeValidTest, InvalidMultiPolygonWithCollapsedComponentRetainsPoint) {
  std::unique_ptr<Geometry> mp = ParseWkt(
      "MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "((20 20, 20 20, 20 20, 20 20)))");
  ASSERT_FALSE(mp->isValid());

  std::unique_ptr<Geometry> result = MakeValid().build(mp.get());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(result->getNumGeometries(), 2u);
  EXPECT_NEAR(result->getGeometryN(0)->getArea(), 100.0, 1e-9);
  EXPECT_TRUE(
      result->getGeometryN(1)->equalsExact(ParseWkt("POINT (20 20)").get()));
}

TEST_F(MakeValidTest, InvalidGeometryCollectionRepairsEachMember) {
  std::unique_ptr<Geometry> gc = ParseWkt(
      "GEOMETRYCOLLECTION ("
      "POINT (1 1), "
      "LINESTRING (2 2, 2 2), "
      "POLYGON ((0 0, 10 10, 10 0, 0 10, 0 0)), "
      "MULTILINESTRING ((3 3, 3 3), (4 4, 6 4)))");
  ASSERT_FALSE(gc->isValid());

  std::unique_ptr<Geometry> result = MakeValid().build(gc.get());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(result->getNumGeometries(), 4u);
  EXPECT_EQ(result->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_POINT);
  EXPECT_EQ(result->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_POINT);
  EXPECT_EQ(result->getGeometryN(2)->getGeometryTypeId(),
            geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_NEAR(result->getGeometryN(2)->getArea(), 50.0, 1e-9);
  EXPECT_EQ(result->getGeometryN(3)->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
}

TEST_F(MakeValidTest, InvalidNestedGeometryCollectionRepairsRecursively) {
  std::unique_ptr<Geometry> gc = ParseWkt(
      "GEOMETRYCOLLECTION ("
      "GEOMETRYCOLLECTION ("
      "LINESTRING (9 9, 9 9), "
      "POLYGON ((0 0, 4 4, 4 0, 0 4, 0 0))), "
      "POINT (1 2))");
  ASSERT_FALSE(gc->isValid());

  std::unique_ptr<Geometry> result = MakeValid().build(gc.get());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(result->getNumGeometries(), 2u);

  const Geometry* nested = result->getGeometryN(0);
  EXPECT_EQ(nested->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(nested->getNumGeometries(), 2u);
  EXPECT_EQ(nested->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_POINT);
  EXPECT_TRUE(
      nested->getGeometryN(0)->equalsExact(ParseWkt("POINT (9 9)").get()));
  EXPECT_EQ(nested->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_NEAR(nested->getGeometryN(1)->getArea(), 8.0, 1e-9);

  EXPECT_EQ(result->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_POINT);
  EXPECT_TRUE(
      result->getGeometryN(1)->equalsExact(ParseWkt("POINT (1 2)").get()));
}

TEST_F(MakeValidTest, FixedPrecisionFactoryPreservesPrecisionModel) {
  geos::geom::PrecisionModel pm(1.0);
  GeometryFactory::Ptr fixed_factory = GeometryFactory::create(&pm);
  WKTReader fixed_reader(*fixed_factory);

  std::unique_ptr<Geometry> poly =
      fixed_reader.read("POLYGON ((0 0, 10 10, 10 0, 0 10, 0 0))");
  ASSERT_FALSE(poly->isValid());

  std::unique_ptr<Geometry> result = MakeValid().build(poly.get());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_FALSE(result->getPrecisionModel()->isFloating());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_NEAR(result->getArea(), 50.0, 1e-9);
}

TEST_F(MakeValidTest, UnsupportedInvalidGeometryTypesThrow) {
  const double nan = std::numeric_limits<double>::quiet_NaN();

  // Invalid Point (NaN coordinate) -> UnsupportedOperationException
  std::unique_ptr<geos::geom::Point> invalid_point =
      factory_->createPoint(CoordinateXY(nan, 0.0));
  ASSERT_FALSE(invalid_point->isValid());
  EXPECT_THROW(MakeValid().build(invalid_point.get()),
               geos::util::UnsupportedOperationException);

  // Invalid MultiPoint (contains NaN coordinate) ->
  // UnsupportedOperationException
  std::vector<std::unique_ptr<Geometry>> pts;
  pts.push_back(factory_->createPoint(CoordinateXY(0.0, 0.0)));
  pts.push_back(factory_->createPoint(CoordinateXY(nan, 1.0)));
  std::unique_ptr<geos::geom::MultiPoint> invalid_mp =
      factory_->createMultiPoint(std::move(pts));
  ASSERT_FALSE(invalid_mp->isValid());
  EXPECT_THROW(MakeValid().build(invalid_mp.get()),
               geos::util::UnsupportedOperationException);

  // Invalid LinearRing (self-intersecting) -> UnsupportedOperationException
  std::unique_ptr<Geometry> invalid_ring =
      ParseWkt("LINEARRING (0 0, 10 10, 10 0, 0 10, 0 0)");
  ASSERT_FALSE(invalid_ring->isValid());
  EXPECT_THROW(MakeValid().build(invalid_ring.get()),
               geos::util::UnsupportedOperationException);

  // Curved geometry -> UnsupportedOperationException from IsValidOp
  std::unique_ptr<Geometry> curve = ParseWkt("CIRCULARSTRING (0 0, 1 1, 2 0)");
  EXPECT_THROW(MakeValid().build(curve.get()),
               geos::util::UnsupportedOperationException);

  // Null geometry -> IllegalArgumentException from IsValidOp
  EXPECT_THROW(MakeValid().build(nullptr),
               geos::util::IllegalArgumentException);
}

int InterruptAlwaysCallback(void* /*data*/) { return 1; }

TEST_F(MakeValidTest, CurrentThreadInterruptAbortsBuild) {
  std::unique_ptr<Geometry> poly =
      ParseWkt("POLYGON ((0 0, 10 10, 10 0, 0 10, 0 0))");
  auto* prev_cb = geos::util::CurrentThreadInterrupt::registerCallback(
      &InterruptAlwaysCallback, nullptr);
  absl::Cleanup restore_cb = [prev_cb]() {
    geos::util::CurrentThreadInterrupt::registerCallback(prev_cb, nullptr);
  };

  EXPECT_THROW(MakeValid().build(poly.get()), geos::util::GEOSException);
}

}  // namespace
}  // namespace geos_google
