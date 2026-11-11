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

#include "third_party/geos/include/geos/io/WKBWriter.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
#include "third_party/geos/include/geos/geom/CompoundCurve.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Curve.h"
#include "third_party/geos/include/geos/geom/CurvePolygon.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiCurve.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/MultiSurface.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/SimpleCurve.h"
#include "third_party/geos/include/geos/geom/Surface.h"
#include "third_party/geos/include/geos/io/ByteOrderValues.h"
#include "third_party/geos/include/geos/io/WKBConstants.h"
#include "third_party/geos/include/geos/io/WKBReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/Machine.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Curve;
using geos::geom::CurvePolygon;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::SimpleCurve;
using geos::geom::Surface;
using geos::io::ByteOrderValues;
namespace WKBConstants = geos::io::WKBConstants;
using geos::io::WKBReader;
using geos::io::WKBWriter;
using geos::util::IllegalArgumentException;

std::vector<unsigned char> WriteToBytes(WKBWriter& writer, const Geometry& g) {
  std::ostringstream os(std::ios::binary);
  writer.write(g, os);
  const std::string str = os.str();
  return std::vector<unsigned char>(str.begin(), str.end());
}

std::string WriteToHex(WKBWriter& writer, const Geometry& g) {
  std::ostringstream os;
  writer.writeHEX(g, os);
  return os.str();
}

std::unique_ptr<Geometry> ReadFromBytes(
    const GeometryFactory& factory, const std::vector<unsigned char>& bytes) {
  WKBReader reader(factory);
  return reader.read(bytes.data(), bytes.size());
}

std::unique_ptr<Geometry> ReadFromHex(const GeometryFactory& factory,
                                      const std::string& hex) {
  WKBReader reader(factory);
  std::istringstream is(hex);
  return reader.readHEX(is);
}

std::unique_ptr<LinearRing> MakeRectRing(const GeometryFactory& factory,
                                         double min_x, double min_y,
                                         double max_x, double max_y) {
  return factory.createLinearRing(
      CoordinateSequence{CoordinateXY(min_x, min_y), CoordinateXY(max_x, min_y),
                         CoordinateXY(max_x, max_y), CoordinateXY(min_x, max_y),
                         CoordinateXY(min_x, min_y)});
}

TEST(WKBWriterTest, DefaultConstructorAndAccessors) {
  WKBWriter writer;
  EXPECT_EQ(writer.getOutputDimension(), std::uint8_t{4});
  EXPECT_EQ(writer.getByteOrder(), getMachineByteOrder());
  EXPECT_FALSE(writer.getIncludeSRID());
  EXPECT_EQ(writer.getFlavor(), +WKBConstants::wkbExtended);
}

TEST(WKBWriterTest, ParameterizedConstructorValidSettings) {
  WKBWriter w2_big_srid_iso(2, ByteOrderValues::ENDIAN_BIG, true,
                            WKBConstants::wkbIso);
  EXPECT_EQ(w2_big_srid_iso.getOutputDimension(), std::uint8_t{2});
  EXPECT_EQ(w2_big_srid_iso.getByteOrder(), +ByteOrderValues::ENDIAN_BIG);
  EXPECT_TRUE(w2_big_srid_iso.getIncludeSRID());
  EXPECT_EQ(w2_big_srid_iso.getFlavor(), +WKBConstants::wkbIso);

  WKBWriter w3_lit_ext(3, ByteOrderValues::ENDIAN_LITTLE, false,
                       WKBConstants::wkbExtended);
  EXPECT_EQ(w3_lit_ext.getOutputDimension(), std::uint8_t{3});
  EXPECT_EQ(w3_lit_ext.getByteOrder(), +ByteOrderValues::ENDIAN_LITTLE);
  EXPECT_FALSE(w3_lit_ext.getIncludeSRID());
  EXPECT_EQ(w3_lit_ext.getFlavor(), +WKBConstants::wkbExtended);
}

TEST(WKBWriterTest, ConstructorInvalidDimensionThrows) {
  // Note: upstream WKBWriter(dims) invokes getOutputOrdinates() in the member
  // initializer list before checking dims < 2 || dims > 4, so dims < 2 spins.
  // Values > 4 still execute the constructor validation throw path.
  EXPECT_THROW(WKBWriter(5), IllegalArgumentException);
  EXPECT_THROW(WKBWriter(255), IllegalArgumentException);
}

TEST(WKBWriterTest, SetAndGetOutputDimension) {
  WKBWriter writer;
  writer.setOutputDimension(2);
  EXPECT_EQ(writer.getOutputDimension(), std::uint8_t{2});
  writer.setOutputDimension(3);
  EXPECT_EQ(writer.getOutputDimension(), std::uint8_t{3});
  writer.setOutputDimension(4);
  EXPECT_EQ(writer.getOutputDimension(), std::uint8_t{4});

  EXPECT_THROW(writer.setOutputDimension(0), IllegalArgumentException);
  EXPECT_THROW(writer.setOutputDimension(1), IllegalArgumentException);
  EXPECT_THROW(writer.setOutputDimension(5), IllegalArgumentException);
  EXPECT_EQ(writer.getOutputDimension(), std::uint8_t{4});
}

TEST(WKBWriterTest, SetAndGetByteOrder) {
  WKBWriter writer;
  writer.setByteOrder(ByteOrderValues::ENDIAN_BIG);
  EXPECT_EQ(writer.getByteOrder(), +ByteOrderValues::ENDIAN_BIG);
  writer.setByteOrder(ByteOrderValues::ENDIAN_LITTLE);
  EXPECT_EQ(writer.getByteOrder(), +ByteOrderValues::ENDIAN_LITTLE);

  EXPECT_THROW(writer.setByteOrder(-1), IllegalArgumentException);
  EXPECT_THROW(writer.setByteOrder(2), IllegalArgumentException);
  EXPECT_EQ(writer.getByteOrder(), +ByteOrderValues::ENDIAN_LITTLE);
}

TEST(WKBWriterTest, SetAndGetIncludeSRID) {
  WKBWriter writer;
  EXPECT_FALSE(writer.getIncludeSRID());
  writer.setIncludeSRID(true);
  EXPECT_TRUE(writer.getIncludeSRID());
  writer.setIncludeSRID(false);
  EXPECT_FALSE(writer.getIncludeSRID());
}

TEST(WKBWriterTest, SetAndGetFlavor) {
  WKBWriter writer;
  writer.setFlavor(WKBConstants::wkbIso);
  EXPECT_EQ(writer.getFlavor(), +WKBConstants::wkbIso);
  writer.setFlavor(WKBConstants::wkbExtended);
  EXPECT_EQ(writer.getFlavor(), +WKBConstants::wkbExtended);

  EXPECT_THROW(writer.setFlavor(0), IllegalArgumentException);
  EXPECT_THROW(writer.setFlavor(3), IllegalArgumentException);
  EXPECT_THROW(writer.setFlavor(-1), IllegalArgumentException);
  EXPECT_EQ(writer.getFlavor(), +WKBConstants::wkbExtended);
}

TEST(WKBWriterTest, UnknownFlavorPassedToConstructorThrowsOnWrite) {
  auto factory = GeometryFactory::create();
  auto pt = factory->createPoint(CoordinateXY(1.0, 2.0));
  WKBWriter writer(2, ByteOrderValues::ENDIAN_LITTLE, false, 99);
  EXPECT_EQ(writer.getFlavor(), 99);

  std::ostringstream os(std::ios::binary);
  EXPECT_THROW(writer.write(*pt, os), IllegalArgumentException);

  std::ostringstream hex_os;
  EXPECT_THROW(writer.writeHEX(*pt, hex_os), IllegalArgumentException);
}

TEST(WKBWriterTest, GetWkbTypeCoversAllGeometryTypes) {
  auto factory = GeometryFactory::create();

  EXPECT_EQ(WKBWriter::getWkbType(*factory->createPoint()),
            +WKBConstants::wkbPoint);
  EXPECT_EQ(WKBWriter::getWkbType(*factory->createLineString()),
            +WKBConstants::wkbLineString);
  EXPECT_EQ(WKBWriter::getWkbType(*factory->createLinearRing()),
            +WKBConstants::wkbLineString);
  EXPECT_EQ(WKBWriter::getWkbType(*factory->createCircularString(false, false)),
            +WKBConstants::wkbCircularString);
  EXPECT_EQ(WKBWriter::getWkbType(*factory->createCompoundCurve()),
            +WKBConstants::wkbCompoundCurve);
  EXPECT_EQ(WKBWriter::getWkbType(*factory->createPolygon()),
            +WKBConstants::wkbPolygon);
  EXPECT_EQ(WKBWriter::getWkbType(*factory->createCurvePolygon(false, false)),
            +WKBConstants::wkbCurvePolygon);
  EXPECT_EQ(WKBWriter::getWkbType(*factory->createMultiPoint()),
            +WKBConstants::wkbMultiPoint);
  EXPECT_EQ(WKBWriter::getWkbType(*factory->createMultiLineString()),
            +WKBConstants::wkbMultiLineString);
  EXPECT_EQ(WKBWriter::getWkbType(*factory->createMultiCurve()),
            +WKBConstants::wkbMultiCurve);
  EXPECT_EQ(WKBWriter::getWkbType(*factory->createMultiPolygon()),
            +WKBConstants::wkbMultiPolygon);
  EXPECT_EQ(WKBWriter::getWkbType(*factory->createMultiSurface()),
            +WKBConstants::wkbMultiSurface);
  EXPECT_EQ(WKBWriter::getWkbType(*factory->createGeometryCollection()),
            +WKBConstants::wkbGeometryCollection);
}

TEST(WKBWriterTest, ByteOrderBinaryAndHexEncodingPoint) {
  auto factory = GeometryFactory::create();
  auto pt = factory->createPoint(CoordinateXY(1.5, -2.25));

  {
    WKBWriter writer(2, ByteOrderValues::ENDIAN_LITTLE, false,
                     WKBConstants::wkbExtended);
    const std::vector<unsigned char> bytes = WriteToBytes(writer, *pt);
    ASSERT_EQ(bytes.size(), std::size_t{21});
    EXPECT_EQ(bytes[0], static_cast<unsigned char>(WKBConstants::wkbNDR));
    EXPECT_EQ(ByteOrderValues::getUnsigned(bytes.data() + 1,
                                           ByteOrderValues::ENDIAN_LITTLE),
              static_cast<std::uint32_t>(WKBConstants::wkbPoint));
    EXPECT_DOUBLE_EQ(ByteOrderValues::getDouble(bytes.data() + 5,
                                                ByteOrderValues::ENDIAN_LITTLE),
                     1.5);
    EXPECT_DOUBLE_EQ(ByteOrderValues::getDouble(bytes.data() + 13,
                                                ByteOrderValues::ENDIAN_LITTLE),
                     -2.25);

    const std::string hex = WriteToHex(writer, *pt);
    // 1.5 = 0x3FF8000000000000, -2.25 = 0xC002000000000000
    EXPECT_EQ(hex, "0101000000000000000000F83F00000000000002C0");
    auto parsed = ReadFromHex(*factory, hex);
    EXPECT_TRUE(parsed->equalsIdentical(pt.get()));
  }

  {
    WKBWriter writer(2, ByteOrderValues::ENDIAN_BIG, false,
                     WKBConstants::wkbExtended);
    const std::vector<unsigned char> bytes = WriteToBytes(writer, *pt);
    ASSERT_EQ(bytes.size(), std::size_t{21});
    EXPECT_EQ(bytes[0], static_cast<unsigned char>(WKBConstants::wkbXDR));
    EXPECT_EQ(ByteOrderValues::getUnsigned(bytes.data() + 1,
                                           ByteOrderValues::ENDIAN_BIG),
              static_cast<std::uint32_t>(WKBConstants::wkbPoint));
    EXPECT_DOUBLE_EQ(ByteOrderValues::getDouble(bytes.data() + 5,
                                                ByteOrderValues::ENDIAN_BIG),
                     1.5);
    EXPECT_DOUBLE_EQ(ByteOrderValues::getDouble(bytes.data() + 13,
                                                ByteOrderValues::ENDIAN_BIG),
                     -2.25);

    const std::string hex = WriteToHex(writer, *pt);
    EXPECT_EQ(hex, "00000000013FF8000000000000C002000000000000");
    auto parsed = ReadFromHex(*factory, hex);
    EXPECT_TRUE(parsed->equalsIdentical(pt.get()));
  }
}

TEST(WKBWriterTest, OutputDimensionClampingAndOrdinateDropping) {
  auto factory = GeometryFactory::create();
  auto pt_xy = factory->createPoint(CoordinateXY(1.0, 2.0));
  auto pt_xyz = factory->createPoint(Coordinate(1.0, 2.0, 3.0));
  auto pt_xym = factory->createPoint(CoordinateXYM(1.0, 2.0, 4.0));
  auto pt_xyzm = factory->createPoint(CoordinateXYZM(1.0, 2.0, 3.0, 4.0));

  // dims = 4 preserves exact dimensionality of each geometry.
  {
    WKBWriter writer(4, ByteOrderValues::ENDIAN_LITTLE);

    auto b_xyzm = WriteToBytes(writer, *pt_xyzm);
    EXPECT_EQ(b_xyzm.size(), std::size_t{37});
    auto r_xyzm = ReadFromBytes(*factory, b_xyzm);
    EXPECT_TRUE(r_xyzm->equalsIdentical(pt_xyzm.get()));

    // Reuse same writer with XY, XYM, XYZ to ensure outputOrdinates resets.
    auto b_xy = WriteToBytes(writer, *pt_xy);
    EXPECT_EQ(b_xy.size(), std::size_t{21});
    auto r_xy = ReadFromBytes(*factory, b_xy);
    EXPECT_FALSE(r_xy->hasZ());
    EXPECT_FALSE(r_xy->hasM());
    EXPECT_TRUE(r_xy->equalsIdentical(pt_xy.get()));

    auto b_xym = WriteToBytes(writer, *pt_xym);
    EXPECT_EQ(b_xym.size(), std::size_t{29});
    auto r_xym = ReadFromBytes(*factory, b_xym);
    EXPECT_FALSE(r_xym->hasZ());
    EXPECT_TRUE(r_xym->hasM());
    auto seq_xym = r_xym->getCoordinates();
    CoordinateXYM out_xym;
    seq_xym->getAt(0, out_xym);
    EXPECT_DOUBLE_EQ(out_xym.m, 4.0);
    EXPECT_DOUBLE_EQ(seq_xym->getOrdinate(
                         0, static_cast<std::size_t>(CoordinateSequence::M)),
                     4.0);

    auto b_xyz = WriteToBytes(writer, *pt_xyz);
    EXPECT_EQ(b_xyz.size(), std::size_t{29});
    auto r_xyz = ReadFromBytes(*factory, b_xyz);
    EXPECT_TRUE(r_xyz->hasZ());
    EXPECT_FALSE(r_xyz->hasM());
    EXPECT_TRUE(r_xyz->equalsIdentical(pt_xyz.get()));
  }

  // dims = 3 keeps XYZ and XYM as-is, and drops M from XYZM keeping Z.
  {
    WKBWriter writer(3, ByteOrderValues::ENDIAN_LITTLE);

    auto b_xyzm = WriteToBytes(writer, *pt_xyzm);
    EXPECT_EQ(b_xyzm.size(), std::size_t{29});
    auto r_xyzm = ReadFromBytes(*factory, b_xyzm);
    EXPECT_TRUE(r_xyzm->hasZ());
    EXPECT_FALSE(r_xyzm->hasM());
    EXPECT_DOUBLE_EQ(r_xyzm->getCoordinates()->getOrdinate(
                         0, static_cast<std::size_t>(CoordinateSequence::Z)),
                     3.0);

    auto b_xym = WriteToBytes(writer, *pt_xym);
    EXPECT_EQ(b_xym.size(), std::size_t{29});
    auto r_xym = ReadFromBytes(*factory, b_xym);
    EXPECT_FALSE(r_xym->hasZ());
    EXPECT_TRUE(r_xym->hasM());
    EXPECT_DOUBLE_EQ(r_xym->getCoordinates()->getOrdinate(
                         0, static_cast<std::size_t>(CoordinateSequence::M)),
                     4.0);
  }

  // dims = 2 drops Z from XYZ, M from XYM, and both M & Z from XYZM.
  {
    WKBWriter writer(2, ByteOrderValues::ENDIAN_LITTLE);

    for (const Geometry* g : {static_cast<const Geometry*>(pt_xy.get()),
                              static_cast<const Geometry*>(pt_xyz.get()),
                              static_cast<const Geometry*>(pt_xym.get()),
                              static_cast<const Geometry*>(pt_xyzm.get())}) {
      auto bytes = WriteToBytes(writer, *g);
      EXPECT_EQ(bytes.size(), std::size_t{21});
      auto parsed = ReadFromBytes(*factory, bytes);
      EXPECT_FALSE(parsed->hasZ());
      EXPECT_FALSE(parsed->hasM());
      EXPECT_TRUE(parsed->equalsExact(pt_xy.get()));
    }
  }
}

TEST(WKBWriterTest, ExtendedAndIsoFlavorTypeWordsAndSRIDHandling) {
  auto factory = GeometryFactory::create();
  auto pt_xy = factory->createPoint(CoordinateXY(5.0, 6.0));
  auto pt_xyz = factory->createPoint(Coordinate(5.0, 6.0, 7.0));
  auto pt_xym = factory->createPoint(CoordinateXYM(5.0, 6.0, 8.0));
  auto pt_xyzm = factory->createPoint(CoordinateXYZM(5.0, 6.0, 7.0, 8.0));

  pt_xy->setSRID(4326);
  pt_xyz->setSRID(4326);
  pt_xym->setSRID(4326);
  pt_xyzm->setSRID(-1);

  // Extended with includeSRID = false: SRID is not embedded.
  {
    WKBWriter writer(4, ByteOrderValues::ENDIAN_LITTLE, false,
                     WKBConstants::wkbExtended);
    auto bytes = WriteToBytes(writer, *pt_xy);
    ASSERT_EQ(bytes.size(), std::size_t{21});
    EXPECT_EQ(ByteOrderValues::getUnsigned(bytes.data() + 1,
                                           ByteOrderValues::ENDIAN_LITTLE),
              0x00000001u);
    auto parsed = ReadFromBytes(*factory, bytes);
    EXPECT_EQ(parsed->getSRID(), 0);
  }

  // Extended with includeSRID = true and SRID == 0: SRID is NOT embedded.
  {
    auto pt_zero_srid = factory->createPoint(CoordinateXY(5.0, 6.0));
    WKBWriter writer(4, ByteOrderValues::ENDIAN_LITTLE, true,
                     WKBConstants::wkbExtended);
    auto bytes = WriteToBytes(writer, *pt_zero_srid);
    ASSERT_EQ(bytes.size(), std::size_t{21});
    EXPECT_EQ(ByteOrderValues::getUnsigned(bytes.data() + 1,
                                           ByteOrderValues::ENDIAN_LITTLE),
              0x00000001u);
  }

  // Extended with includeSRID = true and SRID != 0: checks Z, M, SRID bits.
  {
    WKBWriter writer(4, ByteOrderValues::ENDIAN_BIG, true,
                     WKBConstants::wkbExtended);

    auto b_xy = WriteToBytes(writer, *pt_xy);
    ASSERT_EQ(b_xy.size(), std::size_t{25});
    EXPECT_EQ(ByteOrderValues::getUnsigned(b_xy.data() + 1,
                                           ByteOrderValues::ENDIAN_BIG),
              0x20000001u);
    EXPECT_EQ(
        ByteOrderValues::getInt(b_xy.data() + 5, ByteOrderValues::ENDIAN_BIG),
        4326);
    EXPECT_EQ(ReadFromBytes(*factory, b_xy)->getSRID(), 4326);

    auto b_xyz = WriteToBytes(writer, *pt_xyz);
    ASSERT_EQ(b_xyz.size(), std::size_t{33});
    EXPECT_EQ(ByteOrderValues::getUnsigned(b_xyz.data() + 1,
                                           ByteOrderValues::ENDIAN_BIG),
              0xA0000001u);
    EXPECT_EQ(ReadFromBytes(*factory, b_xyz)->getSRID(), 4326);

    auto b_xym = WriteToBytes(writer, *pt_xym);
    ASSERT_EQ(b_xym.size(), std::size_t{33});
    EXPECT_EQ(ByteOrderValues::getUnsigned(b_xym.data() + 1,
                                           ByteOrderValues::ENDIAN_BIG),
              0x60000001u);
    EXPECT_EQ(ReadFromBytes(*factory, b_xym)->getSRID(), 4326);

    auto b_xyzm = WriteToBytes(writer, *pt_xyzm);
    ASSERT_EQ(b_xyzm.size(), std::size_t{41});
    EXPECT_EQ(ByteOrderValues::getUnsigned(b_xyzm.data() + 1,
                                           ByteOrderValues::ENDIAN_BIG),
              0xE0000001u);
    EXPECT_EQ(
        ByteOrderValues::getInt(b_xyzm.data() + 5, ByteOrderValues::ENDIAN_BIG),
        -1);
    auto r_xyzm = ReadFromBytes(*factory, b_xyzm);
    EXPECT_EQ(r_xyzm->getSRID(), -1);
    EXPECT_TRUE(r_xyzm->equalsIdentical(pt_xyzm.get()));
  }

  // ISO flavor ignores includeSRID and adds +1000/+2000/+3000 to wkbType.
  {
    WKBWriter writer(4, ByteOrderValues::ENDIAN_LITTLE, true,
                     WKBConstants::wkbIso);

    auto b_xy = WriteToBytes(writer, *pt_xy);
    ASSERT_EQ(b_xy.size(), std::size_t{21});
    EXPECT_EQ(ByteOrderValues::getUnsigned(b_xy.data() + 1,
                                           ByteOrderValues::ENDIAN_LITTLE),
              1u);
    EXPECT_EQ(ReadFromBytes(*factory, b_xy)->getSRID(), 0);

    auto b_xyz = WriteToBytes(writer, *pt_xyz);
    ASSERT_EQ(b_xyz.size(), std::size_t{29});
    EXPECT_EQ(ByteOrderValues::getUnsigned(b_xyz.data() + 1,
                                           ByteOrderValues::ENDIAN_LITTLE),
              1001u);
    EXPECT_TRUE(ReadFromBytes(*factory, b_xyz)->equalsIdentical(pt_xyz.get()));

    auto b_xym = WriteToBytes(writer, *pt_xym);
    ASSERT_EQ(b_xym.size(), std::size_t{29});
    EXPECT_EQ(ByteOrderValues::getUnsigned(b_xym.data() + 1,
                                           ByteOrderValues::ENDIAN_LITTLE),
              2001u);
    auto r_xym = ReadFromBytes(*factory, b_xym);
    EXPECT_FALSE(r_xym->hasZ());
    EXPECT_TRUE(r_xym->hasM());

    auto b_xyzm = WriteToBytes(writer, *pt_xyzm);
    ASSERT_EQ(b_xyzm.size(), std::size_t{37});
    EXPECT_EQ(ByteOrderValues::getUnsigned(b_xyzm.data() + 1,
                                           ByteOrderValues::ENDIAN_LITTLE),
              3001u);
    EXPECT_TRUE(
        ReadFromBytes(*factory, b_xyzm)->equalsIdentical(pt_xyzm.get()));
  }
}

TEST(WKBWriterTest, EmptyPointsAcrossDimensionsFlavorsAndSRID) {
  auto factory = GeometryFactory::create();
  auto empty_xy = factory->createPoint(false, false);
  auto empty_xyz = factory->createPoint(true, false);
  auto empty_xym = factory->createPoint(false, true);
  auto empty_xyzm = factory->createPoint(true, true);
  empty_xy->setSRID(4326);

  {
    WKBWriter writer(4, ByteOrderValues::ENDIAN_LITTLE, true,
                     WKBConstants::wkbExtended);

    auto b_xy = WriteToBytes(writer, *empty_xy);
    ASSERT_EQ(b_xy.size(), std::size_t{25});
    EXPECT_TRUE(std::isnan(ByteOrderValues::getDouble(
        b_xy.data() + 9, ByteOrderValues::ENDIAN_LITTLE)));
    EXPECT_TRUE(std::isnan(ByteOrderValues::getDouble(
        b_xy.data() + 17, ByteOrderValues::ENDIAN_LITTLE)));
    auto r_xy = ReadFromBytes(*factory, b_xy);
    EXPECT_TRUE(r_xy->isEmpty());
    EXPECT_FALSE(r_xy->hasZ());
    EXPECT_FALSE(r_xy->hasM());
    EXPECT_EQ(r_xy->getSRID(), 4326);

    auto b_xyz = WriteToBytes(writer, *empty_xyz);
    ASSERT_EQ(b_xyz.size(), std::size_t{29});
    EXPECT_TRUE(std::isnan(ByteOrderValues::getDouble(
        b_xyz.data() + 21, ByteOrderValues::ENDIAN_LITTLE)));
    auto r_xyz = ReadFromBytes(*factory, b_xyz);
    EXPECT_TRUE(r_xyz->isEmpty());
    EXPECT_TRUE(r_xyz->hasZ());
    EXPECT_FALSE(r_xyz->hasM());

    auto b_xym = WriteToBytes(writer, *empty_xym);
    ASSERT_EQ(b_xym.size(), std::size_t{29});
    EXPECT_TRUE(std::isnan(ByteOrderValues::getDouble(
        b_xym.data() + 21, ByteOrderValues::ENDIAN_LITTLE)));
    auto r_xym = ReadFromBytes(*factory, b_xym);
    EXPECT_TRUE(r_xym->isEmpty());
    EXPECT_FALSE(r_xym->hasZ());
    EXPECT_TRUE(r_xym->hasM());

    auto b_xyzm = WriteToBytes(writer, *empty_xyzm);
    ASSERT_EQ(b_xyzm.size(), std::size_t{37});
    auto r_xyzm = ReadFromBytes(*factory, b_xyzm);
    EXPECT_TRUE(r_xyzm->isEmpty());
    EXPECT_TRUE(r_xyzm->hasZ());
    EXPECT_TRUE(r_xyzm->hasM());
  }

  // Empty XYZM clamped to 2D.
  {
    WKBWriter writer(2, ByteOrderValues::ENDIAN_BIG, false,
                     WKBConstants::wkbIso);
    auto b_clamped = WriteToBytes(writer, *empty_xyzm);
    ASSERT_EQ(b_clamped.size(), std::size_t{21});
    auto r_clamped = ReadFromBytes(*factory, b_clamped);
    EXPECT_TRUE(r_clamped->isEmpty());
    EXPECT_FALSE(r_clamped->hasZ());
    EXPECT_FALSE(r_clamped->hasM());
  }
}

TEST(WKBWriterTest, SimpleCurvesLineStringLinearRingCircularString) {
  auto factory = GeometryFactory::create();

  // Empty and non-empty LineString (XY, XYZ, XYM, XYZM).
  auto ls_empty = factory->createLineString(false, false);
  auto ls_xy = factory->createLineString(CoordinateSequence{
      CoordinateXY(0.0, 0.0), CoordinateXY(3.0, 4.0), CoordinateXY(6.0, 0.0)});
  auto ls_xyzm = factory->createLineString(CoordinateSequence{
      CoordinateXYZM(1.0, 2.0, 3.0, 4.0), CoordinateXYZM(5.0, 6.0, 7.0, 8.0)});

  // LinearRing is written as wkbLineString.
  auto lr_empty = factory->createLinearRing(false, false);
  auto lr_xy = MakeRectRing(*factory, 0.0, 0.0, 2.0, 2.0);

  // CircularString empty and non-empty.
  auto cs_empty = factory->createCircularString(false, false);
  auto cs_xyz = factory->createCircularString(
      CoordinateSequence{Coordinate(0.0, 0.0, 1.0), Coordinate(1.0, 1.0, 2.0),
                         Coordinate(2.0, 0.0, 3.0), Coordinate(3.0, -1.0, 4.0),
                         Coordinate(4.0, 0.0, 5.0)});

  WKBWriter writer(4, ByteOrderValues::ENDIAN_LITTLE, false,
                   WKBConstants::wkbExtended);

  for (const Geometry* g : {static_cast<const Geometry*>(ls_empty.get()),
                            static_cast<const Geometry*>(ls_xy.get()),
                            static_cast<const Geometry*>(ls_xyzm.get()),
                            static_cast<const Geometry*>(cs_empty.get()),
                            static_cast<const Geometry*>(cs_xyz.get())}) {
    auto bytes = WriteToBytes(writer, *g);
    auto parsed = ReadFromBytes(*factory, bytes);
    EXPECT_TRUE(parsed->equalsIdentical(g)) << g->getGeometryType();

    auto hex = WriteToHex(writer, *g);
    auto parsed_hex = ReadFromHex(*factory, hex);
    EXPECT_TRUE(parsed_hex->equalsIdentical(g)) << g->getGeometryType();
  }

  // LinearRing round-trips as LineString with identical coordinates.
  {
    auto bytes = WriteToBytes(writer, *lr_empty);
    auto parsed = ReadFromBytes(*factory, bytes);
    EXPECT_EQ(parsed->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
    EXPECT_TRUE(parsed->isEmpty());
  }
  {
    auto bytes = WriteToBytes(writer, *lr_xy);
    auto parsed = ReadFromBytes(*factory, bytes);
    EXPECT_EQ(parsed->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
    EXPECT_TRUE(
        parsed->getCoordinates()->equalsIdentical(*lr_xy->getCoordinatesRO()));
  }
}

TEST(WKBWriterTest, CompoundCurveAndSRIDStatePreservation) {
  auto factory = GeometryFactory::create();

  auto cc_empty = factory->createCompoundCurve();

  std::vector<std::unique_ptr<SimpleCurve>> sections;
  sections.push_back(factory->createLineString(
      CoordinateSequence{CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 0.0)}));
  sections.push_back(factory->createCircularString(CoordinateSequence{
      CoordinateXY(2.0, 0.0), CoordinateXY(3.0, 1.0), CoordinateXY(4.0, 0.0)}));
  auto cc = factory->createCompoundCurve(std::move(sections));
  cc->setSRID(3857);

  std::vector<std::unique_ptr<SimpleCurve>> sections_xyzm;
  sections_xyzm.push_back(factory->createLineString(
      CoordinateSequence{CoordinateXYZM(0.0, 0.0, 1.0, 10.0),
                         CoordinateXYZM(2.0, 0.0, 2.0, 20.0)}));
  sections_xyzm.push_back(factory->createCircularString(CoordinateSequence{
      CoordinateXYZM(2.0, 0.0, 2.0, 20.0), CoordinateXYZM(3.0, 1.0, 3.0, 30.0),
      CoordinateXYZM(4.0, 0.0, 4.0, 40.0)}));
  auto cc_xyzm = factory->createCompoundCurve(std::move(sections_xyzm));

  WKBWriter writer(4, ByteOrderValues::ENDIAN_LITTLE, true,
                   WKBConstants::wkbExtended);

  {
    auto bytes = WriteToBytes(writer, *cc_empty);
    auto parsed = ReadFromBytes(*factory, bytes);
    EXPECT_TRUE(parsed->equalsIdentical(cc_empty.get()));
  }

  {
    auto bytes = WriteToBytes(writer, *cc);
    // Header: 1 byte order + 4 type (with SRID flag) + 4 SRID + 4 numCurves
    // = 13. Child 0 (LineString 2 pts, no SRID): 1 + 4 + 4 + 2*16 = 41. Child 1
    // (CircularString 3 pts, no SRID): 1 + 4 + 4 + 3*16 = 57. Total = 111 bytes
    // (verifies child sections do NOT embed SRID).
    ASSERT_EQ(bytes.size(), std::size_t{111});
    EXPECT_EQ(ByteOrderValues::getUnsigned(bytes.data() + 1,
                                           ByteOrderValues::ENDIAN_LITTLE),
              0x20000000u |
                  static_cast<std::uint32_t>(WKBConstants::wkbCompoundCurve));
    EXPECT_EQ(ByteOrderValues::getInt(bytes.data() + 5,
                                      ByteOrderValues::ENDIAN_LITTLE),
              3857);
    EXPECT_EQ(ByteOrderValues::getUnsigned(bytes.data() + 14,
                                           ByteOrderValues::ENDIAN_LITTLE),
              static_cast<std::uint32_t>(WKBConstants::wkbLineString));

    auto parsed = ReadFromBytes(*factory, bytes);
    EXPECT_TRUE(parsed->equalsIdentical(cc.get()));
    EXPECT_EQ(parsed->getSRID(), 3857);

    // Ensure includeSRID flag was restored after CompoundCurve write.
    EXPECT_TRUE(writer.getIncludeSRID());
    auto pt = factory->createPoint(CoordinateXY(1.0, 2.0));
    pt->setSRID(4326);
    auto pt_bytes = WriteToBytes(writer, *pt);
    EXPECT_EQ(pt_bytes.size(), std::size_t{25});
    EXPECT_EQ(ReadFromBytes(*factory, pt_bytes)->getSRID(), 4326);
  }

  {
    auto bytes = WriteToBytes(writer, *cc_xyzm);
    auto parsed = ReadFromBytes(*factory, bytes);
    EXPECT_TRUE(parsed->equalsIdentical(cc_xyzm.get()));
  }
}

TEST(WKBWriterTest, PolygonEmptyNoHolesWithMultipleHolesAndDimensions) {
  auto factory = GeometryFactory::create();

  auto poly_empty = factory->createPolygon(false, false);
  auto poly_empty_z = factory->createPolygon(true, false);

  auto poly_shell_only =
      factory->createPolygon(MakeRectRing(*factory, 0.0, 0.0, 10.0, 10.0));

  auto outer = MakeRectRing(*factory, 0.0, 0.0, 20.0, 20.0);
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(MakeRectRing(*factory, 2.0, 2.0, 6.0, 6.0));
  holes.push_back(MakeRectRing(*factory, 10.0, 10.0, 14.0, 14.0));
  auto poly_with_holes =
      factory->createPolygon(std::move(outer), std::move(holes));
  poly_with_holes->setSRID(4326);

  auto poly_xyzm = factory->createPolygon(factory->createLinearRing(
      CoordinateSequence{CoordinateXYZM(0.0, 0.0, 1.0, 10.0),
                         CoordinateXYZM(4.0, 0.0, 1.0, 20.0),
                         CoordinateXYZM(4.0, 4.0, 2.0, 30.0),
                         CoordinateXYZM(0.0, 4.0, 2.0, 40.0),
                         CoordinateXYZM(0.0, 0.0, 1.0, 10.0)}));

  WKBWriter writer(4, ByteOrderValues::ENDIAN_LITTLE, true,
                   WKBConstants::wkbExtended);

  for (const Geometry* g : {static_cast<const Geometry*>(poly_empty.get()),
                            static_cast<const Geometry*>(poly_empty_z.get()),
                            static_cast<const Geometry*>(poly_shell_only.get()),
                            static_cast<const Geometry*>(poly_with_holes.get()),
                            static_cast<const Geometry*>(poly_xyzm.get())}) {
    auto bytes = WriteToBytes(writer, *g);
    auto parsed = ReadFromBytes(*factory, bytes);
    EXPECT_TRUE(parsed->equalsIdentical(g));
    EXPECT_EQ(parsed->getSRID(), g->getSRID());
  }

  // Also check ISO big-endian for polygon with holes.
  writer.setFlavor(WKBConstants::wkbIso);
  writer.setByteOrder(ByteOrderValues::ENDIAN_BIG);
  auto iso_bytes = WriteToBytes(writer, *poly_with_holes);
  EXPECT_EQ(ByteOrderValues::getUnsigned(iso_bytes.data() + 1,
                                         ByteOrderValues::ENDIAN_BIG),
            static_cast<std::uint32_t>(WKBConstants::wkbPolygon));
  EXPECT_EQ(ByteOrderValues::getUnsigned(iso_bytes.data() + 5,
                                         ByteOrderValues::ENDIAN_BIG),
            3u);
  auto parsed_iso = ReadFromBytes(*factory, iso_bytes);
  EXPECT_TRUE(parsed_iso->equalsIdentical(poly_with_holes.get()));
  EXPECT_EQ(parsed_iso->getSRID(), 0);
}

TEST(WKBWriterTest, CurvePolygonEmptyShellOnlyAndWithHoles) {
  auto factory = GeometryFactory::create();

  auto cp_empty = factory->createCurvePolygon(false, false);
  auto cp_empty_z = factory->createCurvePolygon(true, false);

  auto cp_shell_only =
      factory->createCurvePolygon(factory->createCircularString(
          CoordinateSequence{CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 2.0),
                             CoordinateXY(4.0, 0.0), CoordinateXY(2.0, -2.0),
                             CoordinateXY(0.0, 0.0)}));

  auto cp_xyz = factory->createCurvePolygon(factory->createCircularString(
      CoordinateSequence{Coordinate(0.0, 0.0, 1.0), Coordinate(2.0, 2.0, 2.0),
                         Coordinate(4.0, 0.0, 3.0), Coordinate(2.0, -2.0, 2.0),
                         Coordinate(0.0, 0.0, 1.0)}));

  // CurvePolygon with CompoundCurve shell and two holes (CircularString +
  // LineString).
  std::vector<std::unique_ptr<SimpleCurve>> shell_parts;
  shell_parts.push_back(factory->createLineString(
      CoordinateSequence{CoordinateXY(-10.0, 0.0), CoordinateXY(10.0, 0.0)}));
  shell_parts.push_back(factory->createCircularString(
      CoordinateSequence{CoordinateXY(10.0, 0.0), CoordinateXY(0.0, 10.0),
                         CoordinateXY(-10.0, 0.0)}));
  auto cc_shell = factory->createCompoundCurve(std::move(shell_parts));

  std::vector<std::unique_ptr<Curve>> cp_holes;
  cp_holes.push_back(factory->createCircularString(CoordinateSequence{
      CoordinateXY(-2.0, 3.0), CoordinateXY(0.0, 5.0), CoordinateXY(2.0, 3.0),
      CoordinateXY(0.0, 1.0), CoordinateXY(-2.0, 3.0)}));
  cp_holes.push_back(factory->createLineString(CoordinateSequence{
      CoordinateXY(-6.0, 1.0), CoordinateXY(-4.0, 1.0), CoordinateXY(-4.0, 3.0),
      CoordinateXY(-6.0, 3.0), CoordinateXY(-6.0, 1.0)}));
  auto cp_with_holes =
      factory->createCurvePolygon(std::move(cc_shell), std::move(cp_holes));
  cp_with_holes->setSRID(3857);

  // CurvePolygon with LinearRing hole writes the hole as wkbLineString.
  std::vector<std::unique_ptr<Curve>> lr_holes;
  lr_holes.push_back(MakeRectRing(*factory, 1.0, -1.0, 3.0, 1.0));
  auto cp_lr_hole = factory->createCurvePolygon(
      factory->createCircularString(
          CoordinateSequence{CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 2.0),
                             CoordinateXY(4.0, 0.0), CoordinateXY(2.0, -2.0),
                             CoordinateXY(0.0, 0.0)}),
      std::move(lr_holes));

  WKBWriter writer(4, ByteOrderValues::ENDIAN_LITTLE, true,
                   WKBConstants::wkbExtended);

  for (const Geometry* g :
       {static_cast<const Geometry*>(cp_empty.get()),
        static_cast<const Geometry*>(cp_empty_z.get()),
        static_cast<const Geometry*>(cp_shell_only.get()),
        static_cast<const Geometry*>(cp_xyz.get()),
        static_cast<const Geometry*>(cp_with_holes.get())}) {
    auto bytes = WriteToBytes(writer, *g);
    auto parsed = ReadFromBytes(*factory, bytes);
    EXPECT_TRUE(parsed->equalsIdentical(g));
    EXPECT_EQ(parsed->getSRID(), g->getSRID());
  }

  {
    auto bytes = WriteToBytes(writer, *cp_lr_hole);
    auto parsed = ReadFromBytes(*factory, bytes);
    const auto* parsed_cp = dynamic_cast<const CurvePolygon*>(parsed.get());
    ASSERT_NE(parsed_cp, nullptr);
    ASSERT_EQ(parsed_cp->getNumInteriorRing(), std::size_t{1});
    EXPECT_EQ(parsed_cp->getInteriorRingN(0)->getGeometryTypeId(),
              geos::geom::GEOS_LINESTRING);
    EXPECT_TRUE(
        parsed_cp->getInteriorRingN(0)->getCoordinates()->equalsIdentical(
            *cp_lr_hole->getInteriorRingN(0)->getCoordinates()));
  }
}

TEST(WKBWriterTest, AllCollectionTypesAndNestedCollectionSRID) {
  auto factory = GeometryFactory::create();

  // Empty collections.
  auto mp_empty = factory->createMultiPoint();
  auto mls_empty = factory->createMultiLineString();
  auto mpoly_empty = factory->createMultiPolygon();
  auto mc_empty = factory->createMultiCurve();
  auto ms_empty = factory->createMultiSurface();
  auto gc_empty = factory->createGeometryCollection();

  // Non-empty MultiPoint (including an empty point element).
  std::vector<std::unique_ptr<Point>> pts;
  pts.push_back(factory->createPoint(CoordinateXY(1.0, 2.0)));
  pts.push_back(factory->createPoint(false, false));
  pts.push_back(factory->createPoint(CoordinateXY(3.0, 4.0)));
  auto mp = factory->createMultiPoint(std::move(pts));
  mp->setSRID(4326);

  // Non-empty MultiLineString.
  std::vector<std::unique_ptr<LineString>> lines;
  lines.push_back(factory->createLineString(
      CoordinateSequence{CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0)}));
  lines.push_back(factory->createLineString(
      CoordinateSequence{CoordinateXY(2.0, 2.0), CoordinateXY(3.0, 3.0)}));
  auto mls = factory->createMultiLineString(std::move(lines));

  // Non-empty MultiPolygon.
  std::vector<std::unique_ptr<Polygon>> polys;
  polys.push_back(
      factory->createPolygon(MakeRectRing(*factory, 0.0, 0.0, 2.0, 2.0)));
  polys.push_back(
      factory->createPolygon(MakeRectRing(*factory, 5.0, 5.0, 8.0, 8.0)));
  auto mpoly = factory->createMultiPolygon(std::move(polys));

  // Non-empty MultiCurve (LineString + CircularString).
  std::vector<std::unique_ptr<Curve>> curves;
  curves.push_back(factory->createLineString(
      CoordinateSequence{CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0)}));
  curves.push_back(factory->createCircularString(CoordinateSequence{
      CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0), CoordinateXY(2.0, 0.0)}));
  auto mc = factory->createMultiCurve(std::move(curves));

  // Non-empty MultiSurface (Polygon + CurvePolygon).
  std::vector<std::unique_ptr<Surface>> surfaces;
  surfaces.push_back(
      factory->createPolygon(MakeRectRing(*factory, 0.0, 0.0, 1.0, 1.0)));
  surfaces.push_back(factory->createCurvePolygon(factory->createCircularString(
      CoordinateSequence{CoordinateXY(2.0, 0.0), CoordinateXY(3.0, 1.0),
                         CoordinateXY(4.0, 0.0), CoordinateXY(3.0, -1.0),
                         CoordinateXY(2.0, 0.0)})));
  auto ms = factory->createMultiSurface(std::move(surfaces));

  // Non-empty heterogeneous + nested GeometryCollection with mixed dimensions &
  // SRID.
  std::vector<std::unique_ptr<Geometry>> nested_elems;
  nested_elems.push_back(factory->createPoint(Coordinate(9.0, 8.0, 7.0)));
  auto nested_gc = factory->createGeometryCollection(std::move(nested_elems));

  std::vector<std::unique_ptr<Geometry>> gc_elems;
  auto elem_pt = factory->createPoint(CoordinateXY(10.0, 20.0));
  elem_pt->setSRID(
      9999);  // Inner SRID must be suppressed when outer writes SRID.
  gc_elems.push_back(std::move(elem_pt));
  gc_elems.push_back(factory->createLineString(
      CoordinateSequence{CoordinateXY(0.0, 0.0), CoordinateXY(5.0, 5.0)}));
  gc_elems.push_back(std::move(nested_gc));
  auto gc = factory->createGeometryCollection(std::move(gc_elems));
  gc->setSRID(3857);

  WKBWriter writer(4, ByteOrderValues::ENDIAN_LITTLE, true,
                   WKBConstants::wkbExtended);

  for (const Geometry* g : {static_cast<const Geometry*>(mp_empty.get()),
                            static_cast<const Geometry*>(mls_empty.get()),
                            static_cast<const Geometry*>(mpoly_empty.get()),
                            static_cast<const Geometry*>(mc_empty.get()),
                            static_cast<const Geometry*>(ms_empty.get()),
                            static_cast<const Geometry*>(gc_empty.get()),
                            static_cast<const Geometry*>(mp.get()),
                            static_cast<const Geometry*>(mls.get()),
                            static_cast<const Geometry*>(mpoly.get()),
                            static_cast<const Geometry*>(mc.get()),
                            static_cast<const Geometry*>(ms.get()),
                            static_cast<const Geometry*>(gc.get())}) {
    auto bytes = WriteToBytes(writer, *g);
    auto parsed = ReadFromBytes(*factory, bytes);
    EXPECT_TRUE(parsed->equalsIdentical(g)) << g->getGeometryType();
    EXPECT_EQ(parsed->getSRID(), g->getSRID()) << g->getGeometryType();

    // Also verify writeHEX round-trips cleanly.
    auto hex = WriteToHex(writer, *g);
    auto parsed_hex = ReadFromHex(*factory, hex);
    EXPECT_TRUE(parsed_hex->equalsIdentical(g)) << g->getGeometryType();
  }

  // Verify MultiPoint and GeometryCollection did not leak includeSRID = false.
  EXPECT_TRUE(writer.getIncludeSRID());
  auto after_pt = factory->createPoint(CoordinateXY(1.0, 1.0));
  after_pt->setSRID(4326);
  auto after_bytes = WriteToBytes(writer, *after_pt);
  EXPECT_EQ(after_bytes.size(), std::size_t{25});
  EXPECT_EQ(ReadFromBytes(*factory, after_bytes)->getSRID(), 4326);

  // Verify child geometry inside gc did NOT emit an SRID flag or SRID value.
  auto gc_bytes = WriteToBytes(writer, *gc);
  // Outer gc header: 1 byteOrder + 4 type + 4 SRID + 4 numGeoms = 13 bytes.
  // Child 0 (Point XY): byte 13 is endianness, bytes 14..17 are wkbType.
  ASSERT_GT(gc_bytes.size(), std::size_t{18});
  EXPECT_EQ(ByteOrderValues::getUnsigned(gc_bytes.data() + 14,
                                         ByteOrderValues::ENDIAN_LITTLE),
            static_cast<std::uint32_t>(WKBConstants::wkbPoint));
}

TEST(WKBWriterTest, MultiVertexXYMAndIsoCodesAcrossGeometries) {
  auto factory = GeometryFactory::create();

  auto ls_xym = factory->createLineString(CoordinateSequence{
      CoordinateXYM(0.0, 1.0, 10.0), CoordinateXYM(2.0, 3.0, 20.0),
      CoordinateXYM(4.0, 5.0, 30.0)});
  auto ls_xyzm = factory->createLineString(
      CoordinateSequence{CoordinateXYZM(0.0, 1.0, 5.0, 10.0),
                         CoordinateXYZM(2.0, 3.0, 6.0, 20.0)});
  auto poly_xym =
      factory->createPolygon(factory->createLinearRing(CoordinateSequence{
          CoordinateXYM(0.0, 0.0, 1.0), CoordinateXYM(2.0, 0.0, 2.0),
          CoordinateXYM(2.0, 2.0, 3.0), CoordinateXYM(0.0, 2.0, 4.0),
          CoordinateXYM(0.0, 0.0, 1.0)}));
  auto cs_xyzm = factory->createCircularString(CoordinateSequence{
      CoordinateXYZM(0.0, 0.0, 1.0, 10.0), CoordinateXYZM(1.0, 1.0, 2.0, 20.0),
      CoordinateXYZM(2.0, 0.0, 3.0, 30.0)});

  WKBWriter iso_writer(4, ByteOrderValues::ENDIAN_LITTLE, false,
                       WKBConstants::wkbIso);

  // LineString XYM -> ISO 2002
  {
    auto bytes = WriteToBytes(iso_writer, *ls_xym);
    EXPECT_EQ(ByteOrderValues::getUnsigned(bytes.data() + 1,
                                           ByteOrderValues::ENDIAN_LITTLE),
              2002u);
    auto parsed = ReadFromBytes(*factory, bytes);
    EXPECT_TRUE(parsed->equalsIdentical(ls_xym.get()));
    EXPECT_FALSE(parsed->hasZ());
    EXPECT_TRUE(parsed->hasM());
  }

  // LineString XYZM -> ISO 3002 (dims=4) and ISO 1002 (dims=3)
  {
    auto bytes4 = WriteToBytes(iso_writer, *ls_xyzm);
    EXPECT_EQ(ByteOrderValues::getUnsigned(bytes4.data() + 1,
                                           ByteOrderValues::ENDIAN_LITTLE),
              3002u);
    EXPECT_TRUE(
        ReadFromBytes(*factory, bytes4)->equalsIdentical(ls_xyzm.get()));

    iso_writer.setOutputDimension(3);
    auto bytes3 = WriteToBytes(iso_writer, *ls_xyzm);
    EXPECT_EQ(ByteOrderValues::getUnsigned(bytes3.data() + 1,
                                           ByteOrderValues::ENDIAN_LITTLE),
              1002u);
    auto parsed3 = ReadFromBytes(*factory, bytes3);
    EXPECT_TRUE(parsed3->hasZ());
    EXPECT_FALSE(parsed3->hasM());

    iso_writer.setOutputDimension(2);
    auto bytes2 = WriteToBytes(iso_writer, *ls_xym);
    EXPECT_EQ(ByteOrderValues::getUnsigned(bytes2.data() + 1,
                                           ByteOrderValues::ENDIAN_LITTLE),
              2u);
    auto parsed2 = ReadFromBytes(*factory, bytes2);
    EXPECT_FALSE(parsed2->hasZ());
    EXPECT_FALSE(parsed2->hasM());
    EXPECT_TRUE(parsed2->equalsExact(ls_xym.get()));

    iso_writer.setOutputDimension(4);
  }

  // Polygon XYM -> ISO 2003
  {
    auto bytes = WriteToBytes(iso_writer, *poly_xym);
    EXPECT_EQ(ByteOrderValues::getUnsigned(bytes.data() + 1,
                                           ByteOrderValues::ENDIAN_LITTLE),
              2003u);
    EXPECT_TRUE(
        ReadFromBytes(*factory, bytes)->equalsIdentical(poly_xym.get()));
  }

  // CircularString XYZM -> ISO 3008
  {
    auto hex = WriteToHex(iso_writer, *cs_xyzm);
    auto parsed = ReadFromHex(*factory, hex);
    EXPECT_TRUE(parsed->equalsIdentical(cs_xyzm.get()));
    auto bytes = WriteToBytes(iso_writer, *cs_xyzm);
    EXPECT_EQ(ByteOrderValues::getUnsigned(bytes.data() + 1,
                                           ByteOrderValues::ENDIAN_LITTLE),
              3008u);
  }
}

}  // namespace
}  // namespace geos_google
