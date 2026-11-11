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

#include "third_party/geos/include/geos/io/WKBStreamReader.h"

#include <cstddef>
#include <ios>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/ByteOrderValues.h"
#include "third_party/geos/include/geos/io/ParseException.h"
#include "third_party/geos/include/geos/io/WKBConstants.h"
#include "third_party/geos/include/geos/io/WKBWriter.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::Point;
using geos::io::ByteOrderValues;
using geos::io::ParseException;
namespace WKBConstants = geos::io::WKBConstants;
using geos::io::WKBStreamReader;
using geos::io::WKBWriter;

TEST(WKBStreamReaderTest, EmptyStreamReturnsNullptr) {
  std::istringstream iss("");
  WKBStreamReader reader(iss);

  EXPECT_EQ(reader.next(), nullptr);
  EXPECT_EQ(reader.next(), nullptr);
}

TEST(WKBStreamReaderTest, FailedStreamReturnsNullptr) {
  // Valid little-endian POINT (1 2) in stream, but failbit is already set.
  std::istringstream iss("0101000000000000000000F03F0000000000000040");
  iss.setstate(std::ios::failbit);
  WKBStreamReader reader(iss);

  EXPECT_EQ(reader.next(), nullptr);
}

TEST(WKBStreamReaderTest, SingleLineWithoutTrailingNewline) {
  // Little-endian (NDR) WKB-HEX for POINT (1.0 2.0).
  // 01 (NDR) + 01000000 (wkbPoint=1) + 1.0 (000000000000F03F) + 2.0
  // (0000000000000040)
  std::istringstream iss("0101000000000000000000F03F0000000000000040");
  WKBStreamReader reader(iss);

  std::unique_ptr<Geometry> geom = reader.next();
  ASSERT_NE(geom, nullptr);
  EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_FALSE(geom->hasZ());
  EXPECT_FALSE(geom->hasM());
  EXPECT_EQ(geom->getSRID(), 0);

  const auto* pt = dynamic_cast<const Point*>(geom.get());
  ASSERT_NE(pt, nullptr);
  EXPECT_DOUBLE_EQ(pt->getX(), 1.0);
  EXPECT_DOUBLE_EQ(pt->getY(), 2.0);

  EXPECT_EQ(reader.next(), nullptr);
}

TEST(WKBStreamReaderTest, SingleLineWithTrailingNewlineBigEndianLowercaseHex) {
  // Big-endian (XDR) lowercase WKB-HEX for POINT (-3.5 4.25).
  // 00 (XDR) + 00000001 (wkbPoint=1) + -3.5 (c00c000000000000) + 4.25
  // (4011000000000000)
  std::istringstream iss("0000000001c00c0000000000004011000000000000\n");
  WKBStreamReader reader(iss);

  std::unique_ptr<Geometry> geom = reader.next();
  ASSERT_NE(geom, nullptr);
  EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_POINT);

  const auto* pt = dynamic_cast<const Point*>(geom.get());
  ASSERT_NE(pt, nullptr);
  EXPECT_DOUBLE_EQ(pt->getX(), -3.5);
  EXPECT_DOUBLE_EQ(pt->getY(), 4.25);

  EXPECT_EQ(reader.next(), nullptr);
}

TEST(WKBStreamReaderTest, ReadsMultipleDiverseGeometriesInSequence) {
  auto factory = GeometryFactory::create();

  std::vector<std::unique_ptr<Geometry>> expected;
  expected.push_back(factory->createPoint(CoordinateXY(10.5, -20.25)));
  expected.push_back(factory->createPoint(Coordinate(1.0, 2.0, 3.5)));

  auto pt_xym = factory->createPoint(CoordinateXYM(4.0, 5.0, 99.0));
  pt_xym->setSRID(4326);
  expected.push_back(std::move(pt_xym));

  expected.push_back(factory->createLineString(CoordinateSequence{
      CoordinateXY(0.0, 0.0), CoordinateXY(3.0, 4.0), CoordinateXY(6.0, 0.0)}));

  auto shell = factory->createLinearRing(CoordinateSequence{
      CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0), CoordinateXY(10.0, 10.0),
      CoordinateXY(0.0, 10.0), CoordinateXY(0.0, 0.0)});
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(factory->createLinearRing(CoordinateSequence{
      CoordinateXY(2.0, 2.0), CoordinateXY(2.0, 4.0), CoordinateXY(4.0, 4.0),
      CoordinateXY(4.0, 2.0), CoordinateXY(2.0, 2.0)}));
  expected.push_back(
      factory->createPolygon(std::move(shell), std::move(holes)));

  expected.push_back(factory->createMultiPoint());

  std::vector<std::unique_ptr<Geometry>> parts;
  parts.push_back(factory->createPoint(CoordinateXY(7.0, 8.0)));
  parts.push_back(factory->createLineString(
      CoordinateSequence{CoordinateXY(1.0, 1.0), CoordinateXY(2.0, 2.0)}));
  expected.push_back(factory->createGeometryCollection(std::move(parts)));

  std::stringstream hex_stream;
  WKBWriter writer(4, ByteOrderValues::ENDIAN_LITTLE, true,
                   WKBConstants::wkbExtended);
  for (const auto& g : expected) {
    writer.writeHEX(*g, hex_stream);
    hex_stream << '\n';
  }

  WKBStreamReader reader(hex_stream);
  for (std::size_t i = 0; i < expected.size(); ++i) {
    std::unique_ptr<Geometry> actual = reader.next();
    ASSERT_NE(actual, nullptr) << "Failed at index " << i;
    EXPECT_TRUE(actual->equalsIdentical(expected[i].get()))
        << "Mismatch at index " << i;
    EXPECT_EQ(actual->getSRID(), expected[i]->getSRID())
        << "SRID mismatch at index " << i;
    EXPECT_EQ(actual->hasZ(), expected[i]->hasZ())
        << "hasZ mismatch at index " << i;
    EXPECT_EQ(actual->hasM(), expected[i]->hasM())
        << "hasM mismatch at index " << i;
  }

  EXPECT_EQ(reader.next(), nullptr);
}

TEST(WKBStreamReaderTest, MixedByteOrdersIsoAndExtendedFlavors) {
  auto factory = GeometryFactory::create();
  auto pt_xyz = factory->createPoint(Coordinate(11.0, 22.0, 33.0));
  auto pt_xyzm = factory->createPoint(CoordinateXYZM(-1.0, -2.0, -3.0, 44.5));
  pt_xyzm->setSRID(3857);

  std::stringstream hex_stream;
  WKBWriter iso_be_writer(4, ByteOrderValues::ENDIAN_BIG, false,
                          WKBConstants::wkbIso);
  iso_be_writer.writeHEX(*pt_xyz, hex_stream);
  hex_stream << '\n';

  WKBWriter ext_le_writer(4, ByteOrderValues::ENDIAN_LITTLE, true,
                          WKBConstants::wkbExtended);
  ext_le_writer.writeHEX(*pt_xyzm, hex_stream);
  hex_stream << '\n';

  WKBStreamReader reader(hex_stream);

  auto g1 = reader.next();
  ASSERT_NE(g1, nullptr);
  EXPECT_TRUE(g1->equalsIdentical(pt_xyz.get()));
  EXPECT_TRUE(g1->hasZ());
  EXPECT_FALSE(g1->hasM());
  EXPECT_EQ(g1->getSRID(), 0);

  auto g2 = reader.next();
  ASSERT_NE(g2, nullptr);
  EXPECT_TRUE(g2->equalsIdentical(pt_xyzm.get()));
  EXPECT_TRUE(g2->hasZ());
  EXPECT_TRUE(g2->hasM());
  EXPECT_EQ(g2->getSRID(), 3857);

  EXPECT_EQ(reader.next(), nullptr);
}

TEST(WKBStreamReaderTest, InvalidHexLinesThrowAndReaderAdvancesPastThem) {
  // Line 1: valid POINT (1 2)
  // Line 2: odd number of hex digits
  // Line 3: invalid hex characters
  // Line 4: blank line (0 bytes)
  // Line 5: truncated WKB buffer
  // Line 6: valid POINT (1 2)
  const std::string valid_pt = "0101000000000000000000F03F0000000000000040";
  std::istringstream iss(valid_pt + "\n" + "0101000\n" + "ZZZZ\n" + "\n" +
                         "0101000000\n" + valid_pt);
  WKBStreamReader reader(iss);

  auto first = reader.next();
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->getGeometryTypeId(), geos::geom::GEOS_POINT);

  EXPECT_THROW(reader.next(), ParseException);
  EXPECT_THROW(reader.next(), ParseException);
  EXPECT_THROW(reader.next(), ParseException);
  EXPECT_THROW(reader.next(), ParseException);

  auto last = reader.next();
  ASSERT_NE(last, nullptr);
  EXPECT_TRUE(last->equalsIdentical(first.get()));

  EXPECT_EQ(reader.next(), nullptr);
}

TEST(WKBStreamReaderTest, EmptyGeometriesRoundTrip) {
  auto factory = GeometryFactory::create();

  std::vector<std::unique_ptr<Geometry>> expected;
  expected.push_back(factory->createPoint());
  expected.push_back(factory->createLineString());
  expected.push_back(factory->createPolygon());
  expected.push_back(factory->createGeometryCollection());

  std::stringstream hex_stream;
  WKBWriter writer;
  for (const auto& g : expected) {
    writer.writeHEX(*g, hex_stream);
    hex_stream << '\n';
  }

  WKBStreamReader reader(hex_stream);
  for (std::size_t i = 0; i < expected.size(); ++i) {
    std::unique_ptr<Geometry> actual = reader.next();
    ASSERT_NE(actual, nullptr) << "Failed at index " << i;
    EXPECT_TRUE(actual->isEmpty()) << "Expected empty at index " << i;
    EXPECT_EQ(actual->getGeometryTypeId(), expected[i]->getGeometryTypeId())
        << "Type mismatch at index " << i;
    EXPECT_TRUE(actual->equalsIdentical(expected[i].get()))
        << "Mismatch at index " << i;
  }

  EXPECT_EQ(reader.next(), nullptr);
}

TEST(WKBStreamReaderTest, HeapAllocatedReaderAndBadStreamState) {
  std::istringstream bad_stream("0101000000000000000000F03F0000000000000040\n");
  bad_stream.setstate(std::ios::badbit);
  auto reader = std::make_unique<WKBStreamReader>(bad_stream);
  EXPECT_EQ(reader->next(), nullptr);
  EXPECT_EQ(reader->next(), nullptr);
}

TEST(WKBStreamReaderTest,
     CarriageReturnTrailingSpacesAndUnknownWkbTypeThrowAndContinue) {
  const std::string valid_pt = "0101000000000000000000F03F0000000000000040";
  // Line 1: CRLF leaves trailing '\r' after std::getline -> odd hex length.
  // Line 2: Two trailing spaces -> invalid hex character ' '.
  // Line 3: Valid hex header with unknown WKB type 99 (0x63).
  // Line 4: Valid POINT (1 2).
  std::istringstream iss(valid_pt + "\r\n" + valid_pt + "  \n" +
                         "0163000000\n" + valid_pt + "\n");
  WKBStreamReader reader(iss);

  EXPECT_THROW(reader.next(), ParseException);
  EXPECT_THROW(reader.next(), ParseException);
  EXPECT_THROW(reader.next(), ParseException);

  std::unique_ptr<Geometry> recovered = reader.next();
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(recovered->getGeometryTypeId(), geos::geom::GEOS_POINT);
  const auto* pt = dynamic_cast<const Point*>(recovered.get());
  ASSERT_NE(pt, nullptr);
  EXPECT_DOUBLE_EQ(pt->getX(), 1.0);
  EXPECT_DOUBLE_EQ(pt->getY(), 2.0);

  EXPECT_EQ(reader.next(), nullptr);
}

}  // namespace
}  // namespace geos_google
