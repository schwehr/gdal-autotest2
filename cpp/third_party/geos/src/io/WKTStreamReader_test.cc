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

// Unit tests for third_party/geos/src/io/WKTStreamReader.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/io/WKTStreamReader.cpp

#include "third_party/geos/include/geos/io/WKTStreamReader.h"

#include <cmath>
#include <ios>
#include <memory>
#include <sstream>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/io/ParseException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Geometry;
using geos::io::ParseException;
using geos::io::WKTStreamReader;

TEST(WKTStreamReaderTest, EmptyWhitespaceAndFailedStreamsReturnNullptr) {
  {
    std::istringstream empty_stream("");
    WKTStreamReader reader(empty_stream);
    EXPECT_EQ(reader.next(), nullptr);
    EXPECT_EQ(reader.next(), nullptr);
  }

  {
    std::istringstream whitespace_stream("\n   \n\t \n");
    auto reader = std::make_unique<WKTStreamReader>(whitespace_stream);
    EXPECT_EQ(reader->next(), nullptr);
  }

  {
    std::istringstream failed_stream("POINT (1 2)\n");
    failed_stream.setstate(std::ios::failbit);
    WKTStreamReader reader(failed_stream);
    EXPECT_EQ(reader.next(), nullptr);
  }
}

TEST(WKTStreamReaderTest, ReadsMultipleSingleLineGeometriesUntilEof) {
  std::istringstream stream(
      "POINT (1.5 -2.25)\n"
      "LINESTRING (0 0, 3 4)\n"
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))\n"
      "MULTIPOINT ((1 1), (2 2))\n"
      "GEOMETRYCOLLECTION (POINT (5 6), LINESTRING (0 0, 1 1))");
  WKTStreamReader reader(stream);

  std::unique_ptr<Geometry> g1 = reader.next();
  ASSERT_NE(g1, nullptr);
  EXPECT_EQ(g1->getGeometryTypeId(), geos::geom::GEOS_POINT);
  ASSERT_NE(g1->getCoordinate(), nullptr);
  EXPECT_DOUBLE_EQ(g1->getCoordinate()->x, 1.5);
  EXPECT_DOUBLE_EQ(g1->getCoordinate()->y, -2.25);

  std::unique_ptr<Geometry> g2 = reader.next();
  ASSERT_NE(g2, nullptr);
  EXPECT_EQ(g2->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_EQ(g2->getNumPoints(), 2u);
  EXPECT_DOUBLE_EQ(g2->getLength(), 5.0);

  std::unique_ptr<Geometry> g3 = reader.next();
  ASSERT_NE(g3, nullptr);
  EXPECT_EQ(g3->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(g3->getArea(), 16.0);

  std::unique_ptr<Geometry> g4 = reader.next();
  ASSERT_NE(g4, nullptr);
  EXPECT_EQ(g4->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(g4->getNumGeometries(), 2u);

  // Final geometry has no trailing newline; getline still succeeds before EOF.
  std::unique_ptr<Geometry> g5 = reader.next();
  ASSERT_NE(g5, nullptr);
  EXPECT_EQ(g5->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(g5->getNumGeometries(), 2u);

  EXPECT_EQ(reader.next(), nullptr);
  EXPECT_EQ(reader.next(), nullptr);
}

TEST(WKTStreamReaderTest, SkipsBlankAndParenFreeLinesBeforeGeometry) {
  std::istringstream stream(
      "\n"
      "   \t  \n"
      "POINT (7 8)\n"
      "\n"
      "POLYGON\n"
      "((0 0, 5 0, 5 5, 0 5, 0 0))\n"
      "\n"
      "   \n");
  WKTStreamReader reader(stream);

  std::unique_ptr<Geometry> point = reader.next();
  ASSERT_NE(point, nullptr);
  EXPECT_EQ(point->getGeometryTypeId(), geos::geom::GEOS_POINT);
  ASSERT_NE(point->getCoordinate(), nullptr);
  EXPECT_DOUBLE_EQ(point->getCoordinate()->x, 7.0);
  EXPECT_DOUBLE_EQ(point->getCoordinate()->y, 8.0);

  std::unique_ptr<Geometry> poly = reader.next();
  ASSERT_NE(poly, nullptr);
  EXPECT_EQ(poly->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(poly->getArea(), 25.0);

  EXPECT_EQ(reader.next(), nullptr);
}

TEST(WKTStreamReaderTest, ReadsMultilineNestedGeometries) {
  std::istringstream stream(
      "POLYGON (\n"
      "  (0 0, 10 0, 10 10, 0 10, 0 0),\n"
      "  (2 2, 2 5, 5 5, 5 2, 2 2)\n"
      ")\n"
      "GEOMETRYCOLLECTION (\n"
      "  POINT (1 2),\n"
      "  POLYGON (\n"
      "    (0 0, 2 0, 2 2, 0 2, 0 0)\n"
      "  )\n"
      ")\n");
  WKTStreamReader reader(stream);

  std::unique_ptr<Geometry> poly = reader.next();
  ASSERT_NE(poly, nullptr);
  EXPECT_EQ(poly->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(poly->getArea(), 91.0);

  std::unique_ptr<Geometry> collection = reader.next();
  ASSERT_NE(collection, nullptr);
  EXPECT_EQ(collection->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(collection->getNumGeometries(), 2u);
  EXPECT_EQ(collection->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_POINT);
  ASSERT_NE(collection->getGeometryN(0)->getCoordinate(), nullptr);
  EXPECT_DOUBLE_EQ(collection->getGeometryN(0)->getCoordinate()->x, 1.0);
  EXPECT_DOUBLE_EQ(collection->getGeometryN(0)->getCoordinate()->y, 2.0);
  EXPECT_EQ(collection->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(collection->getGeometryN(1)->getArea(), 4.0);

  EXPECT_EQ(reader.next(), nullptr);
}

TEST(WKTStreamReaderTest, HandlesDimensionsAndEmptyGeometries) {
  std::istringstream stream(
      "POINT Z (1 2 3)\n"
      "POINT M (4 5 6)\n"
      "POINT ZM (7 8 9 10)\n"
      "GEOMETRYCOLLECTION (POINT EMPTY, POLYGON EMPTY)\n");
  WKTStreamReader reader(stream);

  std::unique_ptr<Geometry> pz = reader.next();
  ASSERT_NE(pz, nullptr);
  EXPECT_TRUE(pz->hasZ());
  EXPECT_FALSE(pz->hasM());
  EXPECT_EQ(pz->getCoordinateDimension(), 3);
  auto pz_coords = pz->getCoordinates();
  ASSERT_EQ(pz_coords->size(), 1u);
  EXPECT_DOUBLE_EQ(pz_coords->getX(0), 1.0);
  EXPECT_DOUBLE_EQ(pz_coords->getY(0), 2.0);
  EXPECT_DOUBLE_EQ(pz_coords->getOrdinate(0, CoordinateSequence::Z), 3.0);
  EXPECT_TRUE(std::isnan(pz_coords->getOrdinate(0, CoordinateSequence::M)));

  std::unique_ptr<Geometry> pm = reader.next();
  ASSERT_NE(pm, nullptr);
  EXPECT_FALSE(pm->hasZ());
  EXPECT_TRUE(pm->hasM());
  EXPECT_EQ(pm->getCoordinateDimension(), 3);
  auto pm_coords = pm->getCoordinates();
  ASSERT_EQ(pm_coords->size(), 1u);
  CoordinateXYM xym;
  pm_coords->getAt(0, xym);
  EXPECT_DOUBLE_EQ(xym.x, 4.0);
  EXPECT_DOUBLE_EQ(xym.y, 5.0);
  EXPECT_DOUBLE_EQ(xym.m, 6.0);
  EXPECT_TRUE(std::isnan(pm_coords->getOrdinate(0, CoordinateSequence::Z)));
  EXPECT_DOUBLE_EQ(pm_coords->getOrdinate(0, CoordinateSequence::M), 6.0);

  std::unique_ptr<Geometry> pzm = reader.next();
  ASSERT_NE(pzm, nullptr);
  EXPECT_TRUE(pzm->hasZ());
  EXPECT_TRUE(pzm->hasM());
  EXPECT_EQ(pzm->getCoordinateDimension(), 4);
  auto pzm_coords = pzm->getCoordinates();
  ASSERT_EQ(pzm_coords->size(), 1u);
  CoordinateXYZM xyzm;
  pzm_coords->getAt(0, xyzm);
  EXPECT_DOUBLE_EQ(xyzm.x, 7.0);
  EXPECT_DOUBLE_EQ(xyzm.y, 8.0);
  EXPECT_DOUBLE_EQ(xyzm.z, 9.0);
  EXPECT_DOUBLE_EQ(xyzm.m, 10.0);

  std::unique_ptr<Geometry> empty_collection = reader.next();
  ASSERT_NE(empty_collection, nullptr);
  EXPECT_EQ(empty_collection->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(empty_collection->getNumGeometries(), 2u);
  EXPECT_TRUE(empty_collection->getGeometryN(0)->isEmpty());
  EXPECT_TRUE(empty_collection->getGeometryN(1)->isEmpty());

  EXPECT_EQ(reader.next(), nullptr);

  // Standalone EMPTY WKT has no parentheses, so WKTStreamReader reads until EOF
  // and returns nullptr.
  std::istringstream bare_empty("POINT EMPTY\n");
  WKTStreamReader bare_empty_reader(bare_empty);
  EXPECT_EQ(bare_empty_reader.next(), nullptr);
}

TEST(WKTStreamReaderTest, HandlesCrlfLineEndings) {
  std::istringstream stream(
      "POINT (10 20)\r\n"
      "LINESTRING (\r\n"
      "  0 0,\r\n"
      "  3 0\r\n"
      ")\r\n");
  WKTStreamReader reader(stream);

  std::unique_ptr<Geometry> pt = reader.next();
  ASSERT_NE(pt, nullptr);
  EXPECT_EQ(pt->getGeometryTypeId(), geos::geom::GEOS_POINT);
  ASSERT_NE(pt->getCoordinate(), nullptr);
  EXPECT_DOUBLE_EQ(pt->getCoordinate()->x, 10.0);
  EXPECT_DOUBLE_EQ(pt->getCoordinate()->y, 20.0);

  std::unique_ptr<Geometry> line = reader.next();
  ASSERT_NE(line, nullptr);
  EXPECT_EQ(line->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_DOUBLE_EQ(line->getLength(), 3.0);

  EXPECT_EQ(reader.next(), nullptr);
}

TEST(WKTStreamReaderTest, UnbalancedParenthesesAtEofReturnNullptr) {
  {
    std::istringstream truncated("LINESTRING (0 0,\n1 1\n");
    WKTStreamReader reader(truncated);
    EXPECT_EQ(reader.next(), nullptr);
  }

  {
    std::istringstream extra_close("POINT ) (1 2)\nPOINT (3 4)\n");
    WKTStreamReader reader(extra_close);
    EXPECT_EQ(reader.next(), nullptr);
  }
}

TEST(WKTStreamReaderTest, InvalidWktWithBalancedParenthesesThrowsAndRecovers) {
  {
    std::istringstream unknown_type("NOT_A_GEOMETRY (1 2)\n");
    WKTStreamReader reader(unknown_type);
    EXPECT_THROW(static_cast<void>(reader.next()), ParseException);
  }

  {
    std::istringstream two_on_one_line("POINT (1 2) POINT (3 4)\n");
    WKTStreamReader reader(two_on_one_line);
    EXPECT_THROW(static_cast<void>(reader.next()), ParseException);
  }

  {
    // Lines are concatenated without adding a separator, merging "1" and "2".
    std::istringstream split_number("POINT (1\n2)\n");
    WKTStreamReader reader(split_number);
    EXPECT_THROW(static_cast<void>(reader.next()), ParseException);
  }

  {
    std::istringstream bad_then_good("POINT (abc def)\nPOINT (9 10)\n");
    WKTStreamReader reader(bad_then_good);
    EXPECT_THROW(static_cast<void>(reader.next()), ParseException);

    std::unique_ptr<Geometry> recovered = reader.next();
    ASSERT_NE(recovered, nullptr);
    EXPECT_EQ(recovered->getGeometryTypeId(), geos::geom::GEOS_POINT);
    ASSERT_NE(recovered->getCoordinate(), nullptr);
    EXPECT_DOUBLE_EQ(recovered->getCoordinate()->x, 9.0);
    EXPECT_DOUBLE_EQ(recovered->getCoordinate()->y, 10.0);
    EXPECT_EQ(reader.next(), nullptr);
  }
}

}  // namespace
}  // namespace geos_google
