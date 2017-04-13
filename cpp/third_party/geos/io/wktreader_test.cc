// Copyright 2017 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests WKTReader.cpp.
//
// See also:
//   https://trac.osgeo.org/geos/browser/trunk/src/io/WKTReader.cpp
//   https://trac.osgeo.org/geos/browser/trunk/tests/unit/io/WKTReaderTest.cpp

#include <memory>

#include "gunit.h"
#include "geom/CoordinateSequence.h"
#include "geom/Geometry.h"
#include "io/ParseException.h"
#include "io/WKTReader.h"
#include "util/IllegalArgumentException.h"

namespace geos {
namespace io {
namespace {

typedef std::unique_ptr<geos::geom::Geometry> GeomPtr;
typedef std::unique_ptr<geos::geom::CoordinateSequence> CoordSeqPtr;

TEST(WktreaderTest, Points) {
  WKTReader reader;
  GeomPtr geom(reader.read("POINT(0 0)"));
  ASSERT_NE(nullptr, geom);
  CoordSeqPtr coords(geom->getCoordinates());
  ASSERT_NE(nullptr, coords);
  EXPECT_EQ(2, coords->getDimension());
  EXPECT_EQ(0, coords->getX(0));
  EXPECT_EQ(0, coords->getY(0));
}

TEST(WktreaderTest, BadMultiLinestring) {
  const std::string s = "MULTILINESTRING(";
  WKTReader reader;
  EXPECT_THROW(reader.read(s), geos::io::ParseException);
}

TEST(WktreaderTest, BadMultiPolygon) {
  const std::string s = "MULTIPOLYGON(";
  WKTReader reader;
  EXPECT_THROW(reader.read(s), geos::io::ParseException);
}

TEST(WktreaderTest, BadMultiPolygon2) {
  const std::string s = "MULTIPOLYGON(\x0a EMPTY(\x9a";
  WKTReader reader;
  EXPECT_THROW(reader.read(s), geos::io::ParseException);
}

TEST(WktreaderTest, BadGeometryCollection) {
  const char kData[] =
      "GEOMETRYCOLLECTION\x0a(\x00Q;Q;\x0a\x00,\x00\x04;\x0a\x00,"
      "\x00\x04\x00\x0a\x00,\x00\x04;\x0a\x00,\x00\x04;\x0a\x00,"
      "\x00\x04\x00\x0a\x00,\x00\x04;\x0a\x00*,\x00\x04;\x0a\x00*,\x00\x04;"
      "\x0a\x00,\x00\x04;\x0a\x00,\x00\x04\x00\x0a\x00,\x00\x04;\x0a\x00,"
      "\x00\x04;\x0a\x00,\x00\x04\x00\x0a\x00,\x00\x04;\x0a\x00*,\x00\x04;"
      "\x0a\x00*,\x00\x04;\x0a\x00,\x00\x04\x00\x0a\x00,\x00\x04;\x0a\x00,"
      "\x00\x0a\x00)\x91\xe0\x00\x00\x00";
  const std::string s(kData, ARRAYSIZE(kData));
  WKTReader reader;
  EXPECT_THROW(reader.read(s), geos::io::ParseException);
}

TEST(WktreaderTest, BadGeometryCollection2) {
  const char kData[] =
      "GEOMETRYCOLLECTION\x0a(GEOMETRYCOLLECTION\x0a(LINEARRING\x0a("
      "2\x0a\x00\x00\x00,"
      "\x00\x04\x00\x00\x00\x0a\x00\xe0\x9a\xe0\xe0\xe0\xe0N2\x0a\x00\x00\x00,"
      "\x00\x04\x00 "
      "\x00\xe0\xe0\x91\x91\x91\x91\x91\x91\xe0Q\x00;,\x00\x0a\x00)"
      "\x91\xe0\x00\x00\x00\x00\x00\x00";
  const std::string s(kData, ARRAYSIZE(kData));
  WKTReader reader;
  EXPECT_THROW(reader.read(s), geos::util::IllegalArgumentException);
}

}  // namespace
}  // namespace io
}  // namespace geos
