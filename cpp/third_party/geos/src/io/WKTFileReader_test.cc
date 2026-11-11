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

// Unit tests for third_party/geos/src/io/WKTFileReader.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for ParseException assertions.
// - WKTFileReader::read internally constructs a default WKTReader (using
//   GeometryFactory::getDefaultInstance() and CLocalizer), so tests run on a
//   single thread.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/io/WKTFileReader.cpp

#include "third_party/geos/include/geos/io/WKTFileReader.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <ios>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/google/util/temp_file.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/io/ParseException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::io::ParseException;
using geos::io::WKTFileReader;
using ::testing::IsEmpty;
using ::testing::SizeIs;

struct WktFileCase {
  std::string_view name;
  std::string_view contents;
};

::testing::AssertionResult WriteFileContents(const TempFile& file,
                                             std::string_view contents) {
  std::ofstream out(file.path(), std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    return ::testing::AssertionFailure()
           << "Failed to open temp file: " << file.string();
  }
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  out.close();
  if (!out.good()) {
    return ::testing::AssertionFailure()
           << "Failed writing temp file: " << file.string();
  }
  return ::testing::AssertionSuccess();
}

TEST(WKTFileReaderTest, ReadsSingleLineGeometries) {
  TempFile file("wkt_file_reader_single_line.wkt");
  ASSERT_TRUE(WriteFileContents(
      file,
      "POINT (1 2)\n"
      "LINESTRING (0 0, 3 4)\n"
      "POLYGON ((0 0, 4 0, 4 3, 0 3, 0 0))\n"
      "MULTIPOINT ((1 1), (2 2))\n"
      "MULTILINESTRING ((0 0, 1 1), (2 2, 3 3))\n"
      "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "((10 10, 11 10, 11 11, 10 11, 10 10)))\n"
      "GEOMETRYCOLLECTION (POINT (5 6), LINESTRING (0 0, 1 0))\n"));

  WKTFileReader reader;
  std::vector<std::unique_ptr<Geometry>> geoms = reader.read(file.string());
  ASSERT_THAT(geoms, SizeIs(7));

  EXPECT_EQ(geoms[0]->getGeometryTypeId(), geos::geom::GEOS_POINT);
  ASSERT_NE(geoms[0]->getCoordinate(), nullptr);
  EXPECT_EQ(*geoms[0]->getCoordinate(), CoordinateXY(1.0, 2.0));

  EXPECT_EQ(geoms[1]->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_EQ(geoms[1]->getNumPoints(), 2u);
  EXPECT_DOUBLE_EQ(geoms[1]->getLength(), 5.0);

  EXPECT_EQ(geoms[2]->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(geoms[2]->getArea(), 12.0);
  EXPECT_DOUBLE_EQ(geoms[2]->getLength(), 14.0);

  EXPECT_EQ(geoms[3]->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(geoms[3]->getNumGeometries(), 2u);

  EXPECT_EQ(geoms[4]->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(geoms[4]->getNumGeometries(), 2u);

  EXPECT_EQ(geoms[5]->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(geoms[5]->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(geoms[5]->getArea(), 5.0);

  EXPECT_EQ(geoms[6]->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(geoms[6]->getNumGeometries(), 2u);
  EXPECT_EQ(geoms[6]->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_POINT);
  EXPECT_EQ(geoms[6]->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_LINESTRING);
}

TEST(WKTFileReaderTest, ReadsMultilineAndNestedParenthesesWkt) {
  TempFile file("wkt_file_reader_multiline.wkt");
  ASSERT_TRUE(WriteFileContents(file,
                                "POLYGON (\n"
                                "  (0 0, 10 0, 10 10, 0 10, 0 0),\n"
                                "  (2 2, 2 6, 6 6, 6 2, 2 2)\n"
                                ")\n"
                                "GEOMETRYCOLLECTION (\n"
                                "  POINT (1 2),\n"
                                "  MULTIPOLYGON (\n"
                                "    ((0 0, 2 0, 2 2, 0 2, 0 0))\n"
                                "  )\n"
                                ")\n"));

  WKTFileReader reader;
  std::vector<std::unique_ptr<Geometry>> geoms = reader.read(file.string());
  ASSERT_THAT(geoms, SizeIs(2));

  EXPECT_EQ(geoms[0]->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(geoms[0]->isValid());
  EXPECT_DOUBLE_EQ(geoms[0]->getArea(), 84.0);
  EXPECT_DOUBLE_EQ(geoms[0]->getLength(), 56.0);

  EXPECT_EQ(geoms[1]->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(geoms[1]->getNumGeometries(), 2u);
  ASSERT_NE(geoms[1]->getGeometryN(0)->getCoordinate(), nullptr);
  EXPECT_EQ(*geoms[1]->getGeometryN(0)->getCoordinate(),
            CoordinateXY(1.0, 2.0));
  EXPECT_EQ(geoms[1]->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_DOUBLE_EQ(geoms[1]->getGeometryN(1)->getArea(), 4.0);
}

TEST(WKTFileReaderTest, ReadsWithoutTrailingNewline) {
  TempFile file("wkt_file_reader_no_trailing_newline.wkt");
  ASSERT_TRUE(WriteFileContents(file, "POINT (7.5 -2.25)"));

  WKTFileReader reader;
  std::vector<std::unique_ptr<Geometry>> geoms = reader.read(file.string());
  ASSERT_THAT(geoms, SizeIs(1));
  EXPECT_EQ(geoms[0]->getGeometryTypeId(), geos::geom::GEOS_POINT);
  ASSERT_NE(geoms[0]->getCoordinate(), nullptr);
  EXPECT_EQ(*geoms[0]->getCoordinate(), CoordinateXY(7.5, -2.25));
}

TEST(WKTFileReaderTest, SkipsBlankAndWhitespaceLines) {
  TempFile file("wkt_file_reader_blank_lines.wkt");
  ASSERT_TRUE(WriteFileContents(file,
                                "\n"
                                "   \n"
                                "\t\n"
                                "POINT (1 2)\n"
                                "\n"
                                "   \t  \n"
                                "LINESTRING (0 0, 0 5)\n"
                                "\n"
                                "  \n"));

  WKTFileReader reader;
  std::vector<std::unique_ptr<Geometry>> geoms = reader.read(file.string());
  ASSERT_THAT(geoms, SizeIs(2));
  EXPECT_EQ(geoms[0]->getGeometryTypeId(), geos::geom::GEOS_POINT);
  ASSERT_NE(geoms[0]->getCoordinate(), nullptr);
  EXPECT_EQ(*geoms[0]->getCoordinate(), CoordinateXY(1.0, 2.0));
  EXPECT_EQ(geoms[1]->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_DOUBLE_EQ(geoms[1]->getLength(), 5.0);
}

TEST(WKTFileReaderTest, ReadsCrlfLineEndings) {
  TempFile file("wkt_file_reader_crlf.wkt");
  ASSERT_TRUE(WriteFileContents(file,
                                "POINT (3 4)\r\n"
                                "LINESTRING (\r\n"
                                "  0 0,\r\n"
                                "  6 8\r\n"
                                ")\r\n"));

  WKTFileReader reader;
  std::vector<std::unique_ptr<Geometry>> geoms = reader.read(file.string());
  ASSERT_THAT(geoms, SizeIs(2));
  EXPECT_EQ(geoms[0]->getGeometryTypeId(), geos::geom::GEOS_POINT);
  ASSERT_NE(geoms[0]->getCoordinate(), nullptr);
  EXPECT_EQ(*geoms[0]->getCoordinate(), CoordinateXY(3.0, 4.0));
  EXPECT_EQ(geoms[1]->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_DOUBLE_EQ(geoms[1]->getLength(), 10.0);
}

TEST(WKTFileReaderTest, ReadsZAndMAndZMCoordinates) {
  TempFile file("wkt_file_reader_zm.wkt");
  ASSERT_TRUE(WriteFileContents(file,
                                "POINT Z (1 2 3)\n"
                                "POINT M (4 5 6)\n"
                                "POINT ZM (7 8 9 10)\n"));

  WKTFileReader reader;
  std::vector<std::unique_ptr<Geometry>> geoms = reader.read(file.string());
  ASSERT_THAT(geoms, SizeIs(3));

  EXPECT_TRUE(geoms[0]->hasZ());
  EXPECT_FALSE(geoms[0]->hasM());
  EXPECT_EQ(geoms[0]->getCoordinateDimension(), std::uint8_t{3});
  std::unique_ptr<CoordinateSequence> seq0 = geoms[0]->getCoordinates();
  ASSERT_NE(seq0, nullptr);
  ASSERT_EQ(seq0->size(), 1u);
  EXPECT_DOUBLE_EQ(seq0->getX(0), 1.0);
  EXPECT_DOUBLE_EQ(seq0->getY(0), 2.0);
  EXPECT_DOUBLE_EQ(seq0->getOrdinate(0, CoordinateSequence::Z), 3.0);
  EXPECT_TRUE(std::isnan(seq0->getOrdinate(0, CoordinateSequence::M)));

  EXPECT_FALSE(geoms[1]->hasZ());
  EXPECT_TRUE(geoms[1]->hasM());
  EXPECT_EQ(geoms[1]->getCoordinateDimension(), std::uint8_t{3});
  std::unique_ptr<CoordinateSequence> seq1 = geoms[1]->getCoordinates();
  ASSERT_NE(seq1, nullptr);
  ASSERT_EQ(seq1->size(), 1u);
  EXPECT_DOUBLE_EQ(seq1->getX(0), 4.0);
  EXPECT_DOUBLE_EQ(seq1->getY(0), 5.0);
  EXPECT_TRUE(std::isnan(seq1->getOrdinate(0, CoordinateSequence::Z)));
  EXPECT_DOUBLE_EQ(seq1->getOrdinate(0, CoordinateSequence::M), 6.0);

  EXPECT_TRUE(geoms[2]->hasZ());
  EXPECT_TRUE(geoms[2]->hasM());
  EXPECT_EQ(geoms[2]->getCoordinateDimension(), std::uint8_t{4});
  std::unique_ptr<CoordinateSequence> seq2 = geoms[2]->getCoordinates();
  ASSERT_NE(seq2, nullptr);
  ASSERT_EQ(seq2->size(), 1u);
  EXPECT_DOUBLE_EQ(seq2->getX(0), 7.0);
  EXPECT_DOUBLE_EQ(seq2->getY(0), 8.0);
  EXPECT_DOUBLE_EQ(seq2->getOrdinate(0, CoordinateSequence::Z), 9.0);
  EXPECT_DOUBLE_EQ(seq2->getOrdinate(0, CoordinateSequence::M), 10.0);
}

constexpr auto kEmptyInputCases = std::to_array<WktFileCase>({
    {.name = "zero_bytes", .contents = ""},
    {.name = "whitespace_only", .contents = "\n  \t \n\n"},
    {.name = "parenless_empty_wkt_at_eof", .contents = "POINT EMPTY\n"},
});

TEST(WKTFileReaderTest, ReturnsEmptyVectorForEmptyOrParenlessInputs) {
  WKTFileReader reader;
  for (const auto& test_case : kEmptyInputCases) {
    SCOPED_TRACE(test_case.name);
    TempFile file("wkt_file_reader_empty.wkt");
    ASSERT_TRUE(WriteFileContents(file, test_case.contents));
    EXPECT_THAT(reader.read(file.string()), IsEmpty());
  }
}

TEST(WKTFileReaderTest, ReturnsEmptyVectorForNonExistentFile) {
  TempFile missing("wkt_file_reader_missing.wkt");
  WKTFileReader reader;
  EXPECT_THAT(reader.read(missing.string()), IsEmpty());
}

TEST(WKTFileReaderTest, StopsAtEofWithUnclosedTrailingParentheses) {
  TempFile file("wkt_file_reader_unclosed.wkt");
  ASSERT_TRUE(WriteFileContents(file,
                                "POINT (1 2)\n"
                                "POLYGON ((0 0, 1 0, 1 1, 0 0)\n"));

  WKTFileReader reader;
  std::vector<std::unique_ptr<Geometry>> geoms = reader.read(file.string());
  ASSERT_THAT(geoms, SizeIs(1));
  EXPECT_EQ(geoms[0]->getGeometryTypeId(), geos::geom::GEOS_POINT);
  ASSERT_NE(geoms[0]->getCoordinate(), nullptr);
  EXPECT_EQ(*geoms[0]->getCoordinate(), CoordinateXY(1.0, 2.0));
}

constexpr auto kMalformedWktCases = std::to_array<WktFileCase>({
    {.name = "unknown_type", .contents = "NOT_A_GEOMETRY (1 2)\n"},
    {.name = "non_numeric_coord", .contents = "POINT (abc)\n"},
    {.name = "two_geometries_same_line",
     .contents = "POINT (0 0) POINT (1 1)\n"},
    {.name = "reversed_parentheses", .contents = ")(\n"},
    {.name = "malformed_after_valid",
     .contents = "POINT (1 2)\nPOINT (not_a_number)\n"},
});

TEST(WKTFileReaderTest, ThrowsParseExceptionOnMalformedBalancedWkt) {
  WKTFileReader reader;
  for (const auto& test_case : kMalformedWktCases) {
    SCOPED_TRACE(test_case.name);
    TempFile file("wkt_file_reader_malformed.wkt");
    ASSERT_TRUE(WriteFileContents(file, test_case.contents));
    EXPECT_THROW(reader.read(file.string()), ParseException);
  }
}

TEST(WKTFileReaderTest, ReusesReaderAcrossMultipleFilesAndHeapAllocation) {
  TempFile first("wkt_file_reader_first.wkt");
  TempFile second("wkt_file_reader_second.wkt");
  ASSERT_TRUE(WriteFileContents(first, "POINT (10 20)\n"));
  ASSERT_TRUE(WriteFileContents(second,
                                "LINESTRING (0 0, 1 0)\n"
                                "LINESTRING (0 0, 0 2)\n"));

  auto reader = std::make_unique<WKTFileReader>();
  std::vector<std::unique_ptr<Geometry>> first_geoms =
      reader->read(first.string());
  ASSERT_THAT(first_geoms, SizeIs(1));
  ASSERT_NE(first_geoms[0]->getCoordinate(), nullptr);
  EXPECT_EQ(*first_geoms[0]->getCoordinate(), CoordinateXY(10.0, 20.0));

  std::vector<std::unique_ptr<Geometry>> second_geoms =
      reader->read(second.string());
  ASSERT_THAT(second_geoms, SizeIs(2));
  EXPECT_DOUBLE_EQ(second_geoms[0]->getLength(), 1.0);
  EXPECT_DOUBLE_EQ(second_geoms[1]->getLength(), 2.0);
}

}  // namespace
}  // namespace geos_google
