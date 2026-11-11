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

// Unit tests for third_party/geos/src/io/WKBReader.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/io/WKBReader.cpp

#include "third_party/geos/include/geos/io/WKBReader.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
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
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/geom/SimpleCurve.h"
#include "third_party/geos/include/geos/io/ByteOrderValues.h"
#include "third_party/geos/include/geos/io/ParseException.h"
#include "third_party/geos/include/geos/io/WKBConstants.h"
#include "third_party/geos/include/geos/io/WKBWriter.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/Machine.h"

namespace geos_google {
namespace {

using geos::geom::CircularString;
using geos::geom::CompoundCurve;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Curve;
using geos::geom::CurvePolygon;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::GEOS_CIRCULARSTRING;
using geos::geom::GEOS_COMPOUNDCURVE;
using geos::geom::GEOS_CURVEPOLYGON;
using geos::geom::GEOS_GEOMETRYCOLLECTION;
using geos::geom::GEOS_LINESTRING;
using geos::geom::GEOS_MULTICURVE;
using geos::geom::GEOS_MULTILINESTRING;
using geos::geom::GEOS_MULTIPOINT;
using geos::geom::GEOS_MULTIPOLYGON;
using geos::geom::GEOS_MULTISURFACE;
using geos::geom::GEOS_POINT;
using geos::geom::GEOS_POLYGON;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::MultiCurve;
using geos::geom::MultiLineString;
using geos::geom::MultiPoint;
using geos::geom::MultiPolygon;
using geos::geom::MultiSurface;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::geom::SimpleCurve;
using geos::io::ByteOrderValues;
using geos::io::ParseException;
using geos::io::WKBReader;
using geos::io::WKBWriter;
using geos::util::IllegalArgumentException;
using ::testing::HasSubstr;

namespace WKBConstants = geos::io::WKBConstants;

constexpr uint32_t kSfsqlZFlag = 0x80000000u;
constexpr uint32_t kSfsqlMFlag = 0x40000000u;
constexpr uint32_t kSfsqlSridFlag = 0x20000000u;

class WkbBuilder {
 public:
  void addByte(uint8_t value) { data_.push_back(value); }

  void addU32(uint32_t value, bool little_endian) {
    for (int i = 0; i < 4; ++i) {
      int shift = little_endian ? (i * 8) : ((3 - i) * 8);
      data_.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
    }
  }

  void addI32(int32_t value, bool little_endian) {
    addU32(static_cast<uint32_t>(value), little_endian);
  }

  void addDouble(double value, bool little_endian) {
    const uint64_t bits = std::bit_cast<uint64_t>(value);
    for (int i = 0; i < 8; ++i) {
      int shift = little_endian ? (i * 8) : ((7 - i) * 8);
      data_.push_back(static_cast<uint8_t>((bits >> shift) & 0xFFu));
    }
  }

  void addRawHeader(uint8_t byte_order, uint32_t raw_type, bool little_endian,
                    std::optional<int32_t> srid = std::nullopt) {
    addByte(byte_order);
    if (srid.has_value()) {
      raw_type |= kSfsqlSridFlag;
    }
    addU32(raw_type, little_endian);
    if (srid.has_value()) {
      addI32(*srid, little_endian);
    }
  }

  void addHeader(uint32_t base_type, bool has_z, bool has_m, bool iso_format,
                 bool little_endian,
                 std::optional<int32_t> srid = std::nullopt) {
    const uint8_t bo =
        little_endian ? WKBConstants::wkbNDR : WKBConstants::wkbXDR;
    uint32_t type = base_type;
    if (iso_format) {
      if (has_z) {
        type += 1000u;
      }
      if (has_m) {
        type += 2000u;
      }
    } else {
      if (has_z) {
        type |= kSfsqlZFlag;
      }
      if (has_m) {
        type |= kSfsqlMFlag;
      }
    }
    addRawHeader(bo, type, little_endian, srid);
  }

  void addCoord(double x, double y, bool little_endian) {
    addDouble(x, little_endian);
    addDouble(y, little_endian);
  }

  void addCoord(double x, double y, bool has_z, double z, bool has_m, double m,
                bool little_endian) {
    addDouble(x, little_endian);
    addDouble(y, little_endian);
    if (has_z) {
      addDouble(z, little_endian);
    }
    if (has_m) {
      addDouble(m, little_endian);
    }
  }

  void addPoint(double x, double y, bool has_z, double z, bool has_m, double m,
                bool iso_format, bool little_endian,
                std::optional<int32_t> srid = std::nullopt) {
    addHeader(WKBConstants::wkbPoint, has_z, has_m, iso_format, little_endian,
              srid);
    addCoord(x, y, has_z, z, has_m, m, little_endian);
  }

  void addCoords(const std::vector<CoordinateXYZM>& coords, bool has_z,
                 bool has_m, bool little_endian) {
    addU32(static_cast<uint32_t>(coords.size()), little_endian);
    for (const auto& c : coords) {
      addCoord(c.x, c.y, has_z, c.z, has_m, c.m, little_endian);
    }
  }

  void addLineString(const std::vector<CoordinateXYZM>& coords, bool has_z,
                     bool has_m, bool iso_format, bool little_endian,
                     std::optional<int32_t> srid = std::nullopt) {
    addHeader(WKBConstants::wkbLineString, has_z, has_m, iso_format,
              little_endian, srid);
    addCoords(coords, has_z, has_m, little_endian);
  }

  void addCircularString(const std::vector<CoordinateXYZM>& coords, bool has_z,
                         bool has_m, bool iso_format, bool little_endian,
                         std::optional<int32_t> srid = std::nullopt) {
    addHeader(WKBConstants::wkbCircularString, has_z, has_m, iso_format,
              little_endian, srid);
    addCoords(coords, has_z, has_m, little_endian);
  }

  void addPolygon(const std::vector<std::vector<CoordinateXYZM>>& rings,
                  bool has_z, bool has_m, bool iso_format, bool little_endian,
                  std::optional<int32_t> srid = std::nullopt) {
    addHeader(WKBConstants::wkbPolygon, has_z, has_m, iso_format, little_endian,
              srid);
    addU32(static_cast<uint32_t>(rings.size()), little_endian);
    for (const auto& ring : rings) {
      addCoords(ring, has_z, has_m, little_endian);
    }
  }

  const unsigned char* data() const { return data_.data(); }
  size_t size() const { return data_.size(); }

  std::string toBinaryString() const {
    return std::string(reinterpret_cast<const char*>(data_.data()),
                       data_.size());
  }

  std::string toHexString(bool lowercase = false) const {
    static constexpr char kUpper[] = "0123456789ABCDEF";
    static constexpr char kLower[] = "0123456789abcdef";
    const char* digits = lowercase ? kLower : kUpper;
    std::string hex;
    hex.reserve(data_.size() * 2);
    for (uint8_t b : data_) {
      hex.push_back(digits[(b >> 4) & 0x0F]);
      hex.push_back(digits[b & 0x0F]);
    }
    return hex;
  }

 private:
  std::vector<uint8_t> data_;
};

template <typename Fn>
void ExpectParseExceptionWith(Fn&& fn, std::string_view expected_substr) {
  try {
    fn();
    FAIL() << "Expected ParseException containing \"" << expected_substr
           << "\"";
  } catch (const ParseException& e) {
    EXPECT_THAT(e.what(), HasSubstr(std::string(expected_substr)));
  }
}

void VerifyPointCoordinates(const Point& pt, double expected_x,
                            double expected_y, bool has_z, double expected_z,
                            bool has_m, double expected_m) {
  ASSERT_FALSE(pt.isEmpty());
  EXPECT_EQ(pt.hasZ(), has_z);
  EXPECT_EQ(pt.hasM(), has_m);
  EXPECT_EQ(pt.getCoordinateDimension(), 2 + (has_z ? 1 : 0) + (has_m ? 1 : 0));
  EXPECT_DOUBLE_EQ(pt.getX(), expected_x);
  EXPECT_DOUBLE_EQ(pt.getY(), expected_y);
  if (has_z) {
    EXPECT_DOUBLE_EQ(pt.getZ(), expected_z);
  } else {
    EXPECT_TRUE(std::isnan(pt.getZ()));
  }
  if (has_m) {
    EXPECT_DOUBLE_EQ(pt.getM(), expected_m);
  } else {
    EXPECT_TRUE(std::isnan(pt.getM()));
  }
}

TEST(WKBReaderTest, DefaultConstructorUsesDefaultFactory) {
  WKBReader reader;
  WkbBuilder wkb;
  wkb.addPoint(1.5, -2.5, false, 0.0, false, 0.0, false, true);

  std::unique_ptr<Geometry> geom = reader.read(wkb.data(), wkb.size());
  ASSERT_NE(geom, nullptr);
  EXPECT_EQ(geom->getFactory(), GeometryFactory::getDefaultInstance());
  EXPECT_EQ(geom->getGeometryTypeId(), GEOS_POINT);

  const auto* pt = dynamic_cast<const Point*>(geom.get());
  ASSERT_NE(pt, nullptr);
  VerifyPointCoordinates(*pt, 1.5, -2.5, false, 0.0, false, 0.0);
}

TEST(WKBReaderTest, CustomFactoryAppliesPrecisionModelToXYOnly) {
  PrecisionModel fixed_pm(100.0);
  GeometryFactory::Ptr fixed_factory =
      GeometryFactory::create(&fixed_pm, /*newSRID=*/999);
  WKBReader fixed_reader(*fixed_factory);

  WkbBuilder wkb;
  wkb.addPoint(1.2349, -5.6789, true, 9.87654321, true, 4.32109876, false,
               true);

  std::unique_ptr<Geometry> geom = fixed_reader.read(wkb.data(), wkb.size());
  ASSERT_NE(geom, nullptr);
  EXPECT_EQ(geom->getFactory(), fixed_factory.get());
  // Absent SRID flag in WKB overrides factory SRID with 0.
  EXPECT_EQ(geom->getSRID(), 0);

  const auto* pt = dynamic_cast<const Point*>(geom.get());
  ASSERT_NE(pt, nullptr);
  VerifyPointCoordinates(*pt, 1.23, -5.68, true, 9.87654321, true, 4.32109876);

  PrecisionModel single_pm(PrecisionModel::FLOATING_SINGLE);
  GeometryFactory::Ptr single_factory = GeometryFactory::create(&single_pm);
  WKBReader single_reader(*single_factory);

  constexpr double kRawX = 1.23456789012345;
  constexpr double kRawY = -2.34567890123456;
  constexpr double kRawZ = 3.45678901234567;
  WkbBuilder single_wkb;
  single_wkb.addPoint(kRawX, kRawY, true, kRawZ, false, 0.0, true, false);

  std::unique_ptr<Geometry> single_geom =
      single_reader.read(single_wkb.data(), single_wkb.size());
  const auto* single_pt = dynamic_cast<const Point*>(single_geom.get());
  ASSERT_NE(single_pt, nullptr);
  EXPECT_DOUBLE_EQ(single_pt->getX(),
                   static_cast<double>(static_cast<float>(kRawX)));
  EXPECT_DOUBLE_EQ(single_pt->getY(),
                   static_cast<double>(static_cast<float>(kRawY)));
  EXPECT_DOUBLE_EQ(single_pt->getZ(), kRawZ);
}

TEST(WKBReaderTest, ReadsFromBufferAndStreamRewindingInitialPosition) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKBReader reader(*factory);

  WkbBuilder wkb;
  wkb.addLineString(
      {{0.0, 1.0, 0.0, 0.0}, {2.0, 3.0, 0.0, 0.0}, {4.0, -1.0, 0.0, 0.0}},
      false, false, false, true);

  std::unique_ptr<Geometry> from_buf = reader.read(wkb.data(), wkb.size());
  ASSERT_NE(from_buf, nullptr);

  std::istringstream iss(wkb.toBinaryString(), std::ios::binary | std::ios::in);
  iss.seekg(5);
  std::unique_ptr<Geometry> from_stream = reader.read(iss);
  ASSERT_NE(from_stream, nullptr);

  EXPECT_TRUE(from_buf->equalsIdentical(from_stream.get()));
}

TEST(WKBReaderTest, ByteOrderNdrXdrMachineFallbackAndNestedMixedOrder) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKBReader reader(*factory);

  // Unknown byte order byte falls back to machine endianness.
  const bool machine_le =
      (getMachineByteOrder() == ByteOrderValues::ENDIAN_LITTLE);
  WkbBuilder fallback_wkb;
  fallback_wkb.addRawHeader(/*byte_order=*/0x42, WKBConstants::wkbPoint,
                            machine_le);
  fallback_wkb.addCoord(7.25, -3.5, machine_le);

  std::unique_ptr<Geometry> fallback_geom =
      reader.read(fallback_wkb.data(), fallback_wkb.size());
  const auto* fallback_pt = dynamic_cast<const Point*>(fallback_geom.get());
  ASSERT_NE(fallback_pt, nullptr);
  EXPECT_DOUBLE_EQ(fallback_pt->getX(), 7.25);
  EXPECT_DOUBLE_EQ(fallback_pt->getY(), -3.5);

  // Outer GeometryCollection is NDR (little-endian), while children alternate
  // between XDR (big-endian) and NDR (little-endian).
  WkbBuilder mixed_wkb;
  mixed_wkb.addHeader(WKBConstants::wkbGeometryCollection, false, false, false,
                      /*little_endian=*/true);
  mixed_wkb.addU32(2, /*little_endian=*/true);
  mixed_wkb.addPoint(10.0, 20.0, false, 0.0, false, 0.0, false,
                     /*little_endian=*/false);
  mixed_wkb.addPoint(30.0, 40.0, false, 0.0, false, 0.0, true,
                     /*little_endian=*/true);

  std::unique_ptr<Geometry> mixed =
      reader.read(mixed_wkb.data(), mixed_wkb.size());
  ASSERT_NE(mixed, nullptr);
  EXPECT_EQ(mixed->getGeometryTypeId(), GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(mixed->getNumGeometries(), 2u);
  const auto* p0 = dynamic_cast<const Point*>(mixed->getGeometryN(0));
  const auto* p1 = dynamic_cast<const Point*>(mixed->getGeometryN(1));
  ASSERT_NE(p0, nullptr);
  ASSERT_NE(p1, nullptr);
  EXPECT_DOUBLE_EQ(p0->getX(), 10.0);
  EXPECT_DOUBLE_EQ(p0->getY(), 20.0);
  EXPECT_DOUBLE_EQ(p1->getX(), 30.0);
  EXPECT_DOUBLE_EQ(p1->getY(), 40.0);
}

TEST(WKBReaderTest, ReadsPointAllDimensionsFormatsAndByteOrders) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKBReader reader(*factory);

  struct DimCase {
    std::string_view name;
    bool has_z;
    bool has_m;
  };
  constexpr std::array<DimCase, 4> kDimCases = {{
      {.name = "XY", .has_z = false, .has_m = false},
      {.name = "XYZ", .has_z = true, .has_m = false},
      {.name = "XYM", .has_z = false, .has_m = true},
      {.name = "XYZM", .has_z = true, .has_m = true},
  }};

  for (const bool little_endian : {true, false}) {
    for (const bool iso_format : {false, true}) {
      for (const DimCase& dc : kDimCases) {
        SCOPED_TRACE(dc.name);
        SCOPED_TRACE(iso_format ? "ISO" : "Extended");
        SCOPED_TRACE(little_endian ? "NDR" : "XDR");

        WkbBuilder wkb;
        wkb.addPoint(11.25, -22.5, dc.has_z, 33.75, dc.has_m, 44.125,
                     iso_format, little_endian);

        std::unique_ptr<Geometry> geom = reader.read(wkb.data(), wkb.size());
        ASSERT_NE(geom, nullptr);
        EXPECT_EQ(geom->getGeometryTypeId(), GEOS_POINT);
        const auto* pt = dynamic_cast<const Point*>(geom.get());
        ASSERT_NE(pt, nullptr);
        VerifyPointCoordinates(*pt, 11.25, -22.5, dc.has_z, 33.75, dc.has_m,
                               44.125);
      }
    }
  }
}

TEST(WKBReaderTest, ReadsPointEmptyAndPartialNaN) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKBReader reader(*factory);

  const double kNaN = std::numeric_limits<double>::quiet_NaN();

  for (const bool iso_format : {false, true}) {
    for (const bool has_z : {false, true}) {
      for (const bool has_m : {false, true}) {
        WkbBuilder wkb;
        wkb.addPoint(kNaN, kNaN, has_z, kNaN, has_m, kNaN, iso_format, true);
        std::unique_ptr<Geometry> geom = reader.read(wkb.data(), wkb.size());
        ASSERT_NE(geom, nullptr);
        EXPECT_EQ(geom->getGeometryTypeId(), GEOS_POINT);
        EXPECT_TRUE(geom->isEmpty());
        EXPECT_EQ(geom->getNumPoints(), 0u);
        EXPECT_EQ(geom->hasZ(), has_z);
        EXPECT_EQ(geom->hasM(), has_m);
      }
    }
  }

  // Only one NaN ordinate must not be collapsed into POINT EMPTY.
  {
    WkbBuilder x_nan;
    x_nan.addPoint(kNaN, 5.0, false, 0.0, false, 0.0, false, true);
    std::unique_ptr<Geometry> geom = reader.read(x_nan.data(), x_nan.size());
    const auto* pt = dynamic_cast<const Point*>(geom.get());
    ASSERT_NE(pt, nullptr);
    EXPECT_FALSE(pt->isEmpty());
    EXPECT_TRUE(std::isnan(pt->getX()));
    EXPECT_DOUBLE_EQ(pt->getY(), 5.0);
  }
  {
    WkbBuilder y_nan;
    y_nan.addPoint(5.0, kNaN, false, 0.0, false, 0.0, false, false);
    std::unique_ptr<Geometry> geom = reader.read(y_nan.data(), y_nan.size());
    const auto* pt = dynamic_cast<const Point*>(geom.get());
    ASSERT_NE(pt, nullptr);
    EXPECT_FALSE(pt->isEmpty());
    EXPECT_DOUBLE_EQ(pt->getX(), 5.0);
    EXPECT_TRUE(std::isnan(pt->getY()));
  }
}

TEST(WKBReaderTest, ReadsSridExtendedFlagAcrossDimensionsAndCollections) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKBReader reader(*factory);

  for (const bool little_endian : {true, false}) {
    for (const int32_t srid : {4326, 0, -1}) {
      WkbBuilder wkb;
      wkb.addPoint(1.0, 2.0, /*has_z=*/true, 3.0, /*has_m=*/true, 4.0,
                   /*iso_format=*/false, little_endian, srid);
      std::unique_ptr<Geometry> geom = reader.read(wkb.data(), wkb.size());
      ASSERT_NE(geom, nullptr);
      EXPECT_EQ(geom->getSRID(), srid);
      const auto* pt = dynamic_cast<const Point*>(geom.get());
      ASSERT_NE(pt, nullptr);
      VerifyPointCoordinates(*pt, 1.0, 2.0, true, 3.0, true, 4.0);
    }
  }

  // Parent GeometryCollection SRID propagates to child geometries.
  WkbBuilder gc_wkb;
  gc_wkb.addHeader(WKBConstants::wkbGeometryCollection, false, false, false,
                   true, /*srid=*/3857);
  gc_wkb.addU32(2, true);
  gc_wkb.addPoint(10.0, 20.0, false, 0.0, false, 0.0, false, true);
  gc_wkb.addLineString({{0.0, 0.0, 0.0, 0.0}, {1.0, 1.0, 0.0, 0.0}}, false,
                       false, false, true);
  std::unique_ptr<Geometry> gc = reader.read(gc_wkb.data(), gc_wkb.size());
  ASSERT_NE(gc, nullptr);
  EXPECT_EQ(gc->getSRID(), 3857);
  ASSERT_EQ(gc->getNumGeometries(), 2u);
  EXPECT_EQ(gc->getGeometryN(0)->getSRID(), 3857);
  EXPECT_EQ(gc->getGeometryN(1)->getSRID(), 3857);
}

TEST(WKBReaderTest, ReadsLineStringEmptyNonEmptyAllDimensionsAndInvalid) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKBReader reader(*factory);

  const std::vector<CoordinateXYZM> kCoords = {
      {0.0, 0.0, 10.0, 100.0},
      {1.5, 2.5, 20.0, 200.0},
      {3.0, -4.0, 30.0, 300.0},
  };

  for (const bool little_endian : {true, false}) {
    for (const bool iso_format : {false, true}) {
      for (const bool has_z : {false, true}) {
        for (const bool has_m : {false, true}) {
          // Empty LineString.
          WkbBuilder empty_wkb;
          empty_wkb.addLineString({}, has_z, has_m, iso_format, little_endian);
          std::unique_ptr<Geometry> empty_geom =
              reader.read(empty_wkb.data(), empty_wkb.size());
          ASSERT_NE(empty_geom, nullptr);
          EXPECT_EQ(empty_geom->getGeometryTypeId(), GEOS_LINESTRING);
          EXPECT_TRUE(empty_geom->isEmpty());
          EXPECT_EQ(empty_geom->hasZ(), has_z);
          EXPECT_EQ(empty_geom->hasM(), has_m);

          // Non-empty LineString.
          WkbBuilder wkb;
          wkb.addLineString(kCoords, has_z, has_m, iso_format, little_endian);
          std::unique_ptr<Geometry> geom = reader.read(wkb.data(), wkb.size());
          ASSERT_NE(geom, nullptr);
          EXPECT_EQ(geom->getGeometryTypeId(), GEOS_LINESTRING);
          const auto* ls = dynamic_cast<const LineString*>(geom.get());
          ASSERT_NE(ls, nullptr);
          EXPECT_EQ(ls->hasZ(), has_z);
          EXPECT_EQ(ls->hasM(), has_m);
          const CoordinateSequence* seq = ls->getCoordinatesRO();
          ASSERT_EQ(seq->size(), kCoords.size());
          for (size_t i = 0; i < kCoords.size(); ++i) {
            EXPECT_DOUBLE_EQ(seq->getX(i), kCoords[i].x);
            EXPECT_DOUBLE_EQ(seq->getY(i), kCoords[i].y);
            if (has_z) {
              EXPECT_DOUBLE_EQ(seq->getOrdinate(i, CoordinateSequence::Z),
                               kCoords[i].z);
            } else {
              EXPECT_TRUE(
                  std::isnan(seq->getOrdinate(i, CoordinateSequence::Z)));
            }
            if (has_m) {
              CoordinateXYM xym;
              seq->getAt(i, xym);
              EXPECT_DOUBLE_EQ(xym.m, kCoords[i].m);
              EXPECT_DOUBLE_EQ(seq->getOrdinate(i, CoordinateSequence::M),
                               kCoords[i].m);
            } else {
              EXPECT_TRUE(
                  std::isnan(seq->getOrdinate(i, CoordinateSequence::M)));
            }
          }
        }
      }
    }
  }

  // Single-point LineString is invalid in GEOS.
  WkbBuilder one_pt;
  one_pt.addLineString({{1.0, 2.0, 0.0, 0.0}}, false, false, false, true);
  EXPECT_THROW(reader.read(one_pt.data(), one_pt.size()),
               IllegalArgumentException);
}

TEST(WKBReaderTest, ReadsPolygonEmptySingleRingAndWithHoles) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKBReader reader(*factory);

  for (const bool iso_format : {false, true}) {
    for (const bool has_z : {false, true}) {
      for (const bool has_m : {false, true}) {
        WkbBuilder empty_wkb;
        empty_wkb.addPolygon({}, has_z, has_m, iso_format, true);
        std::unique_ptr<Geometry> empty_poly =
            reader.read(empty_wkb.data(), empty_wkb.size());
        ASSERT_NE(empty_poly, nullptr);
        EXPECT_EQ(empty_poly->getGeometryTypeId(), GEOS_POLYGON);
        EXPECT_TRUE(empty_poly->isEmpty());
        EXPECT_EQ(empty_poly->hasZ(), has_z);
        EXPECT_EQ(empty_poly->hasM(), has_m);
      }
    }
  }

  // Shell only (10x10 square).
  const std::vector<CoordinateXYZM> shell = {
      {0.0, 0.0, 1.0, 10.0},  {10.0, 0.0, 2.0, 20.0}, {10.0, 10.0, 3.0, 30.0},
      {0.0, 10.0, 4.0, 40.0}, {0.0, 0.0, 1.0, 10.0},
  };
  WkbBuilder single_ring_wkb;
  single_ring_wkb.addPolygon({shell}, true, true, true, false);
  std::unique_ptr<Geometry> single_geom =
      reader.read(single_ring_wkb.data(), single_ring_wkb.size());
  const auto* single_poly = dynamic_cast<const Polygon*>(single_geom.get());
  ASSERT_NE(single_poly, nullptr);
  EXPECT_EQ(single_poly->getNumInteriorRing(), 0u);
  EXPECT_DOUBLE_EQ(single_poly->getArea(), 100.0);
  EXPECT_TRUE(single_poly->hasZ());
  EXPECT_TRUE(single_poly->hasM());

  // Shell + 2 holes (areas 4 and 9 -> net area 87).
  const std::vector<CoordinateXYZM> hole1 = {
      {1.0, 1.0, 5.0, 50.0}, {3.0, 1.0, 5.0, 50.0}, {3.0, 3.0, 5.0, 50.0},
      {1.0, 3.0, 5.0, 50.0}, {1.0, 1.0, 5.0, 50.0},
  };
  const std::vector<CoordinateXYZM> hole2 = {
      {5.0, 5.0, 6.0, 60.0}, {8.0, 5.0, 6.0, 60.0}, {8.0, 8.0, 6.0, 60.0},
      {5.0, 8.0, 6.0, 60.0}, {5.0, 5.0, 6.0, 60.0},
  };
  WkbBuilder holed_wkb;
  holed_wkb.addPolygon({shell, hole1, hole2}, false, true, false, true);
  std::unique_ptr<Geometry> holed_geom =
      reader.read(holed_wkb.data(), holed_wkb.size());
  const auto* holed_poly = dynamic_cast<const Polygon*>(holed_geom.get());
  ASSERT_NE(holed_poly, nullptr);
  ASSERT_EQ(holed_poly->getNumInteriorRing(), 2u);
  EXPECT_DOUBLE_EQ(holed_poly->getArea(), 87.0);
  EXPECT_FALSE(holed_poly->hasZ());
  EXPECT_TRUE(holed_poly->hasM());
  EXPECT_DOUBLE_EQ(
      holed_poly->getInteriorRingN(1)->getCoordinatesRO()->getOrdinate(
          0, CoordinateSequence::M),
      60.0);

  // Polygon with 1 explicit empty ring produces an empty Polygon.
  WkbBuilder one_empty_ring;
  one_empty_ring.addPolygon({{}}, false, false, false, true);
  std::unique_ptr<Geometry> empty_ring_poly =
      reader.read(one_empty_ring.data(), one_empty_ring.size());
  ASSERT_NE(empty_ring_poly, nullptr);
  EXPECT_EQ(empty_ring_poly->getGeometryTypeId(), GEOS_POLYGON);
  EXPECT_TRUE(empty_ring_poly->isEmpty());

  // Polygon with an empty shell and a non-empty hole is rejected.
  WkbBuilder empty_shell_holed;
  empty_shell_holed.addPolygon({{}, hole1}, false, true, false, true);
  EXPECT_THROW(reader.read(empty_shell_holed.data(), empty_shell_holed.size()),
               IllegalArgumentException);
}

TEST(WKBReaderTest, SetFixStructureClosesUnclosedPolygonRings) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKBReader reader(*factory);

  // 4-point unclosed exterior ring.
  const std::vector<CoordinateXYZM> unclosed_square = {
      {0.0, 0.0, 0.0, 0.0},
      {4.0, 0.0, 0.0, 0.0},
      {4.0, 4.0, 0.0, 0.0},
      {0.0, 4.0, 0.0, 0.0},
  };
  WkbBuilder unclosed_shell_wkb;
  unclosed_shell_wkb.addPolygon({unclosed_square}, false, false, false, true);

  // Default fixStructure == false rejects unclosed ring.
  EXPECT_THROW(
      reader.read(unclosed_shell_wkb.data(), unclosed_shell_wkb.size()),
      IllegalArgumentException);

  reader.setFixStructure(true);
  std::unique_ptr<Geometry> fixed_square =
      reader.read(unclosed_shell_wkb.data(), unclosed_shell_wkb.size());
  const auto* poly = dynamic_cast<const Polygon*>(fixed_square.get());
  ASSERT_NE(poly, nullptr);
  EXPECT_EQ(poly->getExteriorRing()->getNumPoints(), 5u);
  EXPECT_DOUBLE_EQ(poly->getArea(), 16.0);

  // 3-point unclosed triangle ring becomes 4-point closed ring.
  WkbBuilder unclosed_tri_wkb;
  unclosed_tri_wkb.addPolygon(
      {{{0.0, 0.0, 0.0, 0.0}, {4.0, 0.0, 0.0, 0.0}, {0.0, 4.0, 0.0, 0.0}}},
      false, false, false, true);
  std::unique_ptr<Geometry> fixed_tri =
      reader.read(unclosed_tri_wkb.data(), unclosed_tri_wkb.size());
  const auto* tri_poly = dynamic_cast<const Polygon*>(fixed_tri.get());
  ASSERT_NE(tri_poly, nullptr);
  EXPECT_EQ(tri_poly->getExteriorRing()->getNumPoints(), 4u);
  EXPECT_DOUBLE_EQ(tri_poly->getArea(), 8.0);

  // Unclosed hole ring inside a closed shell is also fixed.
  const std::vector<CoordinateXYZM> closed_shell = {
      {0.0, 0.0, 0.0, 0.0},  {10.0, 0.0, 0.0, 0.0}, {10.0, 10.0, 0.0, 0.0},
      {0.0, 10.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0},
  };
  const std::vector<CoordinateXYZM> unclosed_hole = {
      {1.0, 1.0, 0.0, 0.0},
      {3.0, 1.0, 0.0, 0.0},
      {3.0, 3.0, 0.0, 0.0},
      {1.0, 3.0, 0.0, 0.0},
  };
  WkbBuilder unclosed_hole_wkb;
  unclosed_hole_wkb.addPolygon({closed_shell, unclosed_hole}, false, false,
                               false, true);
  std::unique_ptr<Geometry> fixed_holed =
      reader.read(unclosed_hole_wkb.data(), unclosed_hole_wkb.size());
  const auto* holed_poly = dynamic_cast<const Polygon*>(fixed_holed.get());
  ASSERT_NE(holed_poly, nullptr);
  EXPECT_EQ(holed_poly->getExteriorRing()->getNumPoints(), 5u);
  ASSERT_EQ(holed_poly->getNumInteriorRing(), 1u);
  EXPECT_EQ(holed_poly->getInteriorRingN(0)->getNumPoints(), 5u);
  EXPECT_DOUBLE_EQ(holed_poly->getArea(), 96.0);

  // 3-point unclosed XYZM triangle copies X, Y, Z, M from vertex 0 on close.
  WkbBuilder unclosed_xyzm_tri;
  unclosed_xyzm_tri.addPolygon(
      {{{1.0, 2.0, 7.0, 70.0}, {5.0, 2.0, 8.0, 80.0}, {1.0, 6.0, 9.0, 90.0}}},
      true, true, true, true);
  std::unique_ptr<Geometry> fixed_xyzm_tri =
      reader.read(unclosed_xyzm_tri.data(), unclosed_xyzm_tri.size());
  const auto* xyzm_poly = dynamic_cast<const Polygon*>(fixed_xyzm_tri.get());
  ASSERT_NE(xyzm_poly, nullptr);
  const CoordinateSequence* xyzm_seq =
      xyzm_poly->getExteriorRing()->getCoordinatesRO();
  ASSERT_EQ(xyzm_seq->size(), 4u);
  EXPECT_DOUBLE_EQ(xyzm_seq->getX(3), 1.0);
  EXPECT_DOUBLE_EQ(xyzm_seq->getY(3), 2.0);
  EXPECT_DOUBLE_EQ(xyzm_seq->getOrdinate(3, CoordinateSequence::Z), 7.0);
  EXPECT_DOUBLE_EQ(xyzm_seq->getOrdinate(3, CoordinateSequence::M), 70.0);

  // Already-closed 4+ point ring is left untouched when fixStructure is true.
  WkbBuilder already_closed_wkb;
  already_closed_wkb.addPolygon({closed_shell}, false, false, false, true);
  std::unique_ptr<Geometry> already_closed_geom =
      reader.read(already_closed_wkb.data(), already_closed_wkb.size());
  const auto* already_closed_poly =
      dynamic_cast<const Polygon*>(already_closed_geom.get());
  ASSERT_NE(already_closed_poly, nullptr);
  EXPECT_EQ(already_closed_poly->getExteriorRing()->getNumPoints(), 5u);
  EXPECT_DOUBLE_EQ(already_closed_poly->getArea(), 100.0);

  // Empty 0-point ring remains empty when fixStructure is true.
  WkbBuilder empty_ring_wkb;
  empty_ring_wkb.addPolygon({{}}, false, false, false, true);
  std::unique_ptr<Geometry> empty_ring_geom =
      reader.read(empty_ring_wkb.data(), empty_ring_wkb.size());
  ASSERT_NE(empty_ring_geom, nullptr);
  EXPECT_TRUE(empty_ring_geom->isEmpty());

  // 2-point unclosed ring closes to a 3-point collapsed ring
  // (LinearRing::MINIMUM_VALID_SIZE == 3) with zero area.
  WkbBuilder two_pt_ring;
  two_pt_ring.addPolygon({{{0.0, 0.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0}}}, false,
                         false, false, true);
  std::unique_ptr<Geometry> collapsed_geom =
      reader.read(two_pt_ring.data(), two_pt_ring.size());
  const auto* collapsed_poly =
      dynamic_cast<const Polygon*>(collapsed_geom.get());
  ASSERT_NE(collapsed_poly, nullptr);
  const LineString* collapsed_ring = collapsed_poly->getExteriorRing();
  ASSERT_EQ(collapsed_ring->getNumPoints(), 3u);
  EXPECT_DOUBLE_EQ(collapsed_ring->getCoordinatesRO()->getX(2), 0.0);
  EXPECT_DOUBLE_EQ(collapsed_ring->getCoordinatesRO()->getY(2), 0.0);
  EXPECT_DOUBLE_EQ(collapsed_poly->getArea(), 0.0);

  // 1-point ring and 2-point already-closed ring still fail validation because
  // front == back prevents closeRing() from extending them to 3 points.
  WkbBuilder one_pt_ring;
  one_pt_ring.addPolygon({{{0.0, 0.0, 0.0, 0.0}}}, false, false, false, true);
  EXPECT_THROW(reader.read(one_pt_ring.data(), one_pt_ring.size()),
               IllegalArgumentException);

  WkbBuilder two_pt_closed_ring;
  two_pt_closed_ring.addPolygon({{{0.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0}}},
                                false, false, false, true);
  EXPECT_THROW(
      reader.read(two_pt_closed_ring.data(), two_pt_closed_ring.size()),
      IllegalArgumentException);

  // Toggling back to false rejects unclosed rings again.
  reader.setFixStructure(false);
  EXPECT_THROW(
      reader.read(unclosed_shell_wkb.data(), unclosed_shell_wkb.size()),
      IllegalArgumentException);
}

TEST(WKBReaderTest, ReadsCircularStringEmptyNonEmptyAndInvalid) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKBReader reader(*factory);

  // Empty CircularString across dimensions.
  for (const bool has_z : {false, true}) {
    for (const bool has_m : {false, true}) {
      WkbBuilder empty_wkb;
      empty_wkb.addCircularString({}, has_z, has_m, true, true);
      std::unique_ptr<Geometry> geom =
          reader.read(empty_wkb.data(), empty_wkb.size());
      ASSERT_NE(geom, nullptr);
      EXPECT_EQ(geom->getGeometryTypeId(), GEOS_CIRCULARSTRING);
      EXPECT_TRUE(geom->isEmpty());
      EXPECT_EQ(geom->hasZ(), has_z);
      EXPECT_EQ(geom->hasM(), has_m);
    }
  }

  // Semicircle of radius 1: (-1,0) -> (0,1) -> (1,0), arc length == pi.
  const std::vector<CoordinateXYZM> arc = {
      {-1.0, 0.0, 1.0, 2.0},
      {0.0, 1.0, 3.0, 4.0},
      {1.0, 0.0, 5.0, 6.0},
  };
  for (const bool little_endian : {true, false}) {
    for (const bool iso_format : {false, true}) {
      WkbBuilder wkb;
      wkb.addCircularString(arc, true, true, iso_format, little_endian);
      std::unique_ptr<Geometry> geom = reader.read(wkb.data(), wkb.size());
      const auto* cs = dynamic_cast<const CircularString*>(geom.get());
      ASSERT_NE(cs, nullptr);
      EXPECT_TRUE(cs->hasCurvedComponents());
      EXPECT_EQ(cs->getNumPoints(), 3u);
      EXPECT_NEAR(cs->getLength(), std::numbers::pi, 1e-9);
      EXPECT_DOUBLE_EQ(
          cs->getCoordinatesRO()->getOrdinate(1, CoordinateSequence::Z), 3.0);
      EXPECT_DOUBLE_EQ(
          cs->getCoordinatesRO()->getOrdinate(1, CoordinateSequence::M), 4.0);
    }
  }

  // 2-point CircularString is invalid.
  WkbBuilder bad_cs;
  bad_cs.addCircularString({{0.0, 0.0, 0.0, 0.0}, {1.0, 1.0, 0.0, 0.0}}, false,
                           false, true, true);
  EXPECT_THROW(reader.read(bad_cs.data(), bad_cs.size()),
               IllegalArgumentException);
}

TEST(WKBReaderTest, ReadsCompoundCurveEmptyNonEmptyAndErrors) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKBReader reader(*factory);

  // Empty CompoundCurve.
  WkbBuilder empty_wkb;
  empty_wkb.addHeader(WKBConstants::wkbCompoundCurve, false, false, true, true);
  empty_wkb.addU32(0, true);
  std::unique_ptr<Geometry> empty_geom =
      reader.read(empty_wkb.data(), empty_wkb.size());
  const auto* empty_cc = dynamic_cast<const CompoundCurve*>(empty_geom.get());
  ASSERT_NE(empty_cc, nullptr);
  EXPECT_TRUE(empty_cc->isEmpty());
  EXPECT_EQ(empty_cc->getNumCurves(), 0u);

  // Closed CompoundCurve: LineString (1,0)->(-1,0) + CircularString
  // (-1,0)->(0,1)->(1,0).
  WkbBuilder cc_wkb;
  cc_wkb.addHeader(WKBConstants::wkbCompoundCurve, true, true, false, true);
  cc_wkb.addU32(2, true);
  cc_wkb.addLineString({{1.0, 0.0, 1.0, 10.0}, {-1.0, 0.0, 2.0, 20.0}}, true,
                       true, false, true);
  cc_wkb.addCircularString(
      {{-1.0, 0.0, 2.0, 20.0}, {0.0, 1.0, 3.0, 30.0}, {1.0, 0.0, 1.0, 10.0}},
      true, true, true, false);
  std::unique_ptr<Geometry> cc_geom = reader.read(cc_wkb.data(), cc_wkb.size());
  const auto* cc = dynamic_cast<const CompoundCurve*>(cc_geom.get());
  ASSERT_NE(cc, nullptr);
  EXPECT_EQ(cc->getGeometryTypeId(), GEOS_COMPOUNDCURVE);
  ASSERT_EQ(cc->getNumCurves(), 2u);
  const SimpleCurve* seg0 = cc->getCurveN(0);
  const SimpleCurve* seg1 = cc->getCurveN(1);
  ASSERT_NE(seg0, nullptr);
  ASSERT_NE(seg1, nullptr);
  EXPECT_EQ(seg0->getGeometryTypeId(), GEOS_LINESTRING);
  EXPECT_FALSE(seg0->isCurved());
  EXPECT_EQ(seg1->getGeometryTypeId(), GEOS_CIRCULARSTRING);
  EXPECT_TRUE(seg1->isCurved());
  EXPECT_TRUE(cc->isClosed());
  EXPECT_TRUE(cc->hasZ());
  EXPECT_TRUE(cc->hasM());
  EXPECT_NEAR(cc->getLength(), 2.0 + std::numbers::pi, 1e-9);

  // Wrong child type: Point inside CompoundCurve throws ParseException.
  WkbBuilder wrong_child;
  wrong_child.addHeader(WKBConstants::wkbCompoundCurve, false, false, true,
                        true);
  wrong_child.addU32(1, true);
  wrong_child.addPoint(0.0, 0.0, false, 0.0, false, 0.0, true, true);
  ExpectParseExceptionWith(
      [&] { reader.read(wrong_child.data(), wrong_child.size()); },
      "Expected SimpleCurve but got Point");

  // Nested CompoundCurve inside CompoundCurve is not a SimpleCurve.
  // Inner CompoundCurve includes a 2-point LineString so total payload exceeds
  // minMemSize(GEOS_COMPOUNDCURVE, 1).
  WkbBuilder nested_cc;
  nested_cc.addHeader(WKBConstants::wkbCompoundCurve, false, false, true, true);
  nested_cc.addU32(1, true);
  nested_cc.addHeader(WKBConstants::wkbCompoundCurve, false, false, true, true);
  nested_cc.addU32(1, true);
  nested_cc.addLineString({{0.0, 0.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0}}, false,
                          false, true, true);
  ExpectParseExceptionWith(
      [&] { reader.read(nested_cc.data(), nested_cc.size()); },
      "Expected SimpleCurve but got CompoundCurve");

  // Error on second child cleanly discards already-parsed first child.
  WkbBuilder second_wrong_cc;
  second_wrong_cc.addHeader(WKBConstants::wkbCompoundCurve, false, false, true,
                            true);
  second_wrong_cc.addU32(2, true);
  second_wrong_cc.addLineString({{0.0, 0.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0}},
                                false, false, true, true);
  second_wrong_cc.addPoint(1.0, 0.0, false, 0.0, false, 0.0, true, true);
  ExpectParseExceptionWith(
      [&] { reader.read(second_wrong_cc.data(), second_wrong_cc.size()); },
      "Expected SimpleCurve but got Point");

  // Non-contiguous sections throw IllegalArgumentException.
  WkbBuilder gap_cc;
  gap_cc.addHeader(WKBConstants::wkbCompoundCurve, false, false, true, true);
  gap_cc.addU32(2, true);
  gap_cc.addLineString({{0.0, 0.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0}}, false,
                       false, true, true);
  gap_cc.addLineString({{2.0, 0.0, 0.0, 0.0}, {3.0, 0.0, 0.0, 0.0}}, false,
                       false, true, true);
  EXPECT_THROW(reader.read(gap_cc.data(), gap_cc.size()),
               IllegalArgumentException);
}

TEST(WKBReaderTest, ReadsCurvePolygonEmptySingleShellWithHolesAndErrors) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKBReader reader(*factory);

  // Empty CurvePolygon across dimensions.
  for (const bool iso_format : {false, true}) {
    for (const bool has_z : {false, true}) {
      for (const bool has_m : {false, true}) {
        WkbBuilder empty_wkb;
        empty_wkb.addHeader(WKBConstants::wkbCurvePolygon, has_z, has_m,
                            iso_format, true);
        empty_wkb.addU32(0, true);
        std::unique_ptr<Geometry> geom =
            reader.read(empty_wkb.data(), empty_wkb.size());
        const auto* cp = dynamic_cast<const CurvePolygon*>(geom.get());
        ASSERT_NE(cp, nullptr);
        EXPECT_TRUE(cp->isEmpty());
        EXPECT_EQ(cp->hasZ(), has_z);
        EXPECT_EQ(cp->hasM(), has_m);
      }
    }
  }

  // Single shell: circle of radius 2 centered at (0, 0) -> area 4*pi.
  const std::vector<CoordinateXYZM> outer_circle = {
      {-2.0, 0.0, 0.0, 0.0}, {0.0, 2.0, 0.0, 0.0},  {2.0, 0.0, 0.0, 0.0},
      {0.0, -2.0, 0.0, 0.0}, {-2.0, 0.0, 0.0, 0.0},
  };
  WkbBuilder single_cp;
  single_cp.addHeader(WKBConstants::wkbCurvePolygon, false, false, true, true);
  single_cp.addU32(1, true);
  single_cp.addCircularString(outer_circle, false, false, true, true);
  std::unique_ptr<Geometry> single_geom =
      reader.read(single_cp.data(), single_cp.size());
  const auto* cp_single = dynamic_cast<const CurvePolygon*>(single_geom.get());
  ASSERT_NE(cp_single, nullptr);
  EXPECT_EQ(cp_single->getNumInteriorRing(), 0u);
  EXPECT_TRUE(cp_single->hasCurvedComponents());
  EXPECT_NEAR(cp_single->getArea(), 4.0 * std::numbers::pi, 1e-9);

  // Shell + 2 holes (LineString square area 1 + CircularString radius 0.2).
  WkbBuilder holed_cp;
  holed_cp.addHeader(WKBConstants::wkbCurvePolygon, false, false, true, false);
  holed_cp.addU32(3, false);
  holed_cp.addCircularString(outer_circle, false, false, true, false);
  holed_cp.addLineString({{-0.5, -0.5, 0.0, 0.0},
                          {0.5, -0.5, 0.0, 0.0},
                          {0.5, 0.5, 0.0, 0.0},
                          {-0.5, 0.5, 0.0, 0.0},
                          {-0.5, -0.5, 0.0, 0.0}},
                         false, false, true, true);
  holed_cp.addCircularString({{1.0, 0.0, 0.0, 0.0},
                              {1.2, 0.2, 0.0, 0.0},
                              {1.4, 0.0, 0.0, 0.0},
                              {1.2, -0.2, 0.0, 0.0},
                              {1.0, 0.0, 0.0, 0.0}},
                             false, false, true, false);
  std::unique_ptr<Geometry> holed_geom =
      reader.read(holed_cp.data(), holed_cp.size());
  const auto* cp_holed = dynamic_cast<const CurvePolygon*>(holed_geom.get());
  ASSERT_NE(cp_holed, nullptr);
  const Curve* cp_shell = cp_holed->getExteriorRing();
  ASSERT_NE(cp_shell, nullptr);
  EXPECT_TRUE(cp_shell->isClosed());
  ASSERT_EQ(cp_holed->getNumInteriorRing(), 2u);
  EXPECT_NEAR(cp_holed->getArea(), 3.96 * std::numbers::pi - 1.0, 1e-9);

  // Wrong shell child type.
  WkbBuilder bad_shell;
  bad_shell.addHeader(WKBConstants::wkbCurvePolygon, false, false, true, true);
  bad_shell.addU32(1, true);
  bad_shell.addPoint(0.0, 0.0, false, 0.0, false, 0.0, true, true);
  ExpectParseExceptionWith(
      [&] { reader.read(bad_shell.data(), bad_shell.size()); },
      "Expected Curve but got Point");

  // Wrong hole child type.
  WkbBuilder bad_hole;
  bad_hole.addHeader(WKBConstants::wkbCurvePolygon, false, false, true, true);
  bad_hole.addU32(2, true);
  bad_hole.addCircularString(outer_circle, false, false, true, true);
  bad_hole.addPolygon({}, false, false, true, true);
  ExpectParseExceptionWith(
      [&] { reader.read(bad_hole.data(), bad_hole.size()); },
      "Expected Curve but got Polygon");

  // CurvePolygon with CompoundCurve shell and CompoundCurve hole (XYZ).
  WkbBuilder compound_cp;
  compound_cp.addHeader(WKBConstants::wkbCurvePolygon, true, false, true, true);
  compound_cp.addU32(2, true);
  // Shell: upper half-disk of radius 2 -> area 2*pi.
  compound_cp.addHeader(WKBConstants::wkbCompoundCurve, true, false, true,
                        true);
  compound_cp.addU32(2, true);
  compound_cp.addLineString({{2.0, 0.0, 1.0, 0.0}, {-2.0, 0.0, 1.0, 0.0}}, true,
                            false, true, true);
  compound_cp.addCircularString(
      {{-2.0, 0.0, 1.0, 0.0}, {0.0, 2.0, 1.0, 0.0}, {2.0, 0.0, 1.0, 0.0}}, true,
      false, true, true);
  // Hole: upper half-disk of radius 1 centered at (0, 0.5) -> area 0.5*pi.
  compound_cp.addHeader(WKBConstants::wkbCompoundCurve, true, false, true,
                        true);
  compound_cp.addU32(2, true);
  compound_cp.addLineString({{1.0, 0.5, 2.0, 0.0}, {-1.0, 0.5, 2.0, 0.0}}, true,
                            false, true, true);
  compound_cp.addCircularString(
      {{-1.0, 0.5, 2.0, 0.0}, {0.0, 1.5, 2.0, 0.0}, {1.0, 0.5, 2.0, 0.0}}, true,
      false, true, true);
  std::unique_ptr<Geometry> compound_cp_geom =
      reader.read(compound_cp.data(), compound_cp.size());
  const auto* cp_compound =
      dynamic_cast<const CurvePolygon*>(compound_cp_geom.get());
  ASSERT_NE(cp_compound, nullptr);
  EXPECT_TRUE(cp_compound->hasZ());
  EXPECT_FALSE(cp_compound->hasM());
  EXPECT_EQ(cp_compound->getExteriorRing()->getGeometryTypeId(),
            GEOS_COMPOUNDCURVE);
  ASSERT_EQ(cp_compound->getNumInteriorRing(), 1u);
  EXPECT_EQ(cp_compound->getInteriorRingN(0)->getGeometryTypeId(),
            GEOS_COMPOUNDCURVE);
  EXPECT_NEAR(cp_compound->getArea(), 1.5 * std::numbers::pi, 1e-9);
}

TEST(WKBReaderTest, ReadsMultiPointMultiLineStringMultiPolygonAndChildErrors) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKBReader reader(*factory);

  // Empty MultiPoint, MultiLineString, MultiPolygon.
  for (const uint32_t multi_type :
       {WKBConstants::wkbMultiPoint, WKBConstants::wkbMultiLineString,
        WKBConstants::wkbMultiPolygon}) {
    WkbBuilder wkb;
    wkb.addHeader(multi_type, false, false, false, true);
    wkb.addU32(0, true);
    std::unique_ptr<Geometry> geom = reader.read(wkb.data(), wkb.size());
    ASSERT_NE(geom, nullptr);
    EXPECT_TRUE(geom->isEmpty());
    EXPECT_EQ(geom->getNumGeometries(), 0u);
  }

  // Populated MultiPoint (including empty point and XYZ).
  WkbBuilder mp_wkb;
  mp_wkb.addHeader(WKBConstants::wkbMultiPoint, true, false, false, true);
  mp_wkb.addU32(3, true);
  mp_wkb.addPoint(1.0, 2.0, true, 3.0, false, 0.0, false, true);
  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  mp_wkb.addPoint(kNaN, kNaN, true, kNaN, false, 0.0, false, false);
  mp_wkb.addPoint(4.0, 5.0, true, 6.0, false, 0.0, true, true);
  std::unique_ptr<Geometry> mp_geom = reader.read(mp_wkb.data(), mp_wkb.size());
  const auto* mp = dynamic_cast<const MultiPoint*>(mp_geom.get());
  ASSERT_NE(mp, nullptr);
  EXPECT_EQ(mp->getGeometryTypeId(), GEOS_MULTIPOINT);
  ASSERT_EQ(mp->getNumGeometries(), 3u);
  EXPECT_TRUE(mp->getGeometryN(1)->isEmpty());
  EXPECT_TRUE(mp->hasZ());

  // MultiPoint with LineString child throws ParseException.
  // LineString has 2 points so total payload exceeds
  // minMemSize(GEOS_MULTIPOINT, 1).
  WkbBuilder bad_mp;
  bad_mp.addHeader(WKBConstants::wkbMultiPoint, false, false, false, true);
  bad_mp.addU32(1, true);
  bad_mp.addLineString({{0.0, 0.0, 0.0, 0.0}, {1.0, 1.0, 0.0, 0.0}}, false,
                       false, false, true);
  ExpectParseExceptionWith([&] { reader.read(bad_mp.data(), bad_mp.size()); },
                           "Expected Point but got LineString");

  // Error on second child of MultiPoint cleans up parsed first Point.
  WkbBuilder bad_mp_second;
  bad_mp_second.addHeader(WKBConstants::wkbMultiPoint, false, false, false,
                          true);
  bad_mp_second.addU32(2, true);
  bad_mp_second.addPoint(1.0, 2.0, false, 0.0, false, 0.0, false, true);
  bad_mp_second.addLineString({{0.0, 0.0, 0.0, 0.0}, {1.0, 1.0, 0.0, 0.0}},
                              false, false, false, true);
  ExpectParseExceptionWith(
      [&] { reader.read(bad_mp_second.data(), bad_mp_second.size()); },
      "Expected Point but got LineString");

  // Populated MultiLineString.
  WkbBuilder mls_wkb;
  mls_wkb.addHeader(WKBConstants::wkbMultiLineString, false, false, true,
                    false);
  mls_wkb.addU32(2, false);
  mls_wkb.addLineString({{0.0, 0.0, 0.0, 0.0}, {3.0, 4.0, 0.0, 0.0}}, false,
                        false, true, false);
  mls_wkb.addLineString({}, false, false, true, true);
  std::unique_ptr<Geometry> mls_geom =
      reader.read(mls_wkb.data(), mls_wkb.size());
  const auto* mls = dynamic_cast<const MultiLineString*>(mls_geom.get());
  ASSERT_NE(mls, nullptr);
  EXPECT_EQ(mls->getGeometryTypeId(), GEOS_MULTILINESTRING);
  ASSERT_EQ(mls->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(mls->getLength(), 5.0);

  // MultiLineString with CircularString child throws ParseException.
  WkbBuilder bad_mls;
  bad_mls.addHeader(WKBConstants::wkbMultiLineString, false, false, false,
                    true);
  bad_mls.addU32(1, true);
  bad_mls.addCircularString({}, false, false, true, true);
  ExpectParseExceptionWith([&] { reader.read(bad_mls.data(), bad_mls.size()); },
                           "Expected LineString but got CircularString");

  // Populated MultiPolygon.
  WkbBuilder mpoly_wkb;
  mpoly_wkb.addHeader(WKBConstants::wkbMultiPolygon, false, false, false, true);
  mpoly_wkb.addU32(2, true);
  mpoly_wkb.addPolygon({{{0.0, 0.0, 0.0, 0.0},
                         {2.0, 0.0, 0.0, 0.0},
                         {2.0, 2.0, 0.0, 0.0},
                         {0.0, 2.0, 0.0, 0.0},
                         {0.0, 0.0, 0.0, 0.0}}},
                       false, false, false, true);
  mpoly_wkb.addPolygon({}, false, false, false, false);
  std::unique_ptr<Geometry> mpoly_geom =
      reader.read(mpoly_wkb.data(), mpoly_wkb.size());
  const auto* mpoly = dynamic_cast<const MultiPolygon*>(mpoly_geom.get());
  ASSERT_NE(mpoly, nullptr);
  EXPECT_EQ(mpoly->getGeometryTypeId(), GEOS_MULTIPOLYGON);
  ASSERT_EQ(mpoly->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(mpoly->getArea(), 4.0);

  // MultiPolygon with CurvePolygon child throws ParseException.
  WkbBuilder bad_mpoly;
  bad_mpoly.addHeader(WKBConstants::wkbMultiPolygon, false, false, true, true);
  bad_mpoly.addU32(1, true);
  bad_mpoly.addHeader(WKBConstants::wkbCurvePolygon, false, false, true, true);
  bad_mpoly.addU32(0, true);
  ExpectParseExceptionWith(
      [&] { reader.read(bad_mpoly.data(), bad_mpoly.size()); },
      "Expected Polygon but got CurvePolygon");
}

TEST(WKBReaderTest, ReadsMultiCurveMultiSurfaceAndGeometryCollection) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKBReader reader(*factory);

  // Empty MultiCurve, MultiSurface, GeometryCollection.
  for (const uint32_t type :
       {WKBConstants::wkbMultiCurve, WKBConstants::wkbMultiSurface,
        WKBConstants::wkbGeometryCollection}) {
    WkbBuilder wkb;
    wkb.addHeader(type, false, false, true, true);
    wkb.addU32(0, true);
    std::unique_ptr<Geometry> geom = reader.read(wkb.data(), wkb.size());
    ASSERT_NE(geom, nullptr);
    EXPECT_TRUE(geom->isEmpty());
    EXPECT_EQ(geom->getNumGeometries(), 0u);
  }

  // Populated MultiCurve with LineString + CircularString + CompoundCurve.
  WkbBuilder mc_wkb;
  mc_wkb.addHeader(WKBConstants::wkbMultiCurve, false, false, true, true);
  mc_wkb.addU32(3, true);
  mc_wkb.addLineString({{0.0, 0.0, 0.0, 0.0}, {2.0, 0.0, 0.0, 0.0}}, false,
                       false, true, true);
  mc_wkb.addCircularString(
      {{-1.0, 0.0, 0.0, 0.0}, {0.0, 1.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0}},
      false, false, true, false);
  mc_wkb.addHeader(WKBConstants::wkbCompoundCurve, false, false, true, true);
  mc_wkb.addU32(1, true);
  mc_wkb.addLineString({{0.0, 0.0, 0.0, 0.0}, {0.0, 3.0, 0.0, 0.0}}, false,
                       false, true, true);
  std::unique_ptr<Geometry> mc_geom = reader.read(mc_wkb.data(), mc_wkb.size());
  const auto* mc = dynamic_cast<const MultiCurve*>(mc_geom.get());
  ASSERT_NE(mc, nullptr);
  EXPECT_EQ(mc->getGeometryTypeId(), GEOS_MULTICURVE);
  ASSERT_EQ(mc->getNumGeometries(), 3u);
  EXPECT_EQ(mc->getGeometryN(0)->getGeometryTypeId(), GEOS_LINESTRING);
  EXPECT_EQ(mc->getGeometryN(1)->getGeometryTypeId(), GEOS_CIRCULARSTRING);
  EXPECT_EQ(mc->getGeometryN(2)->getGeometryTypeId(), GEOS_COMPOUNDCURVE);
  EXPECT_NEAR(mc->getLength(), 5.0 + std::numbers::pi, 1e-9);

  // Wrong child in MultiCurve.
  WkbBuilder bad_mc;
  bad_mc.addHeader(WKBConstants::wkbMultiCurve, false, false, true, true);
  bad_mc.addU32(1, true);
  bad_mc.addPolygon({}, false, false, true, true);
  ExpectParseExceptionWith([&] { reader.read(bad_mc.data(), bad_mc.size()); },
                           "Expected Curve but got Polygon");

  // Populated MultiSurface with Polygon + CurvePolygon.
  WkbBuilder ms_wkb;
  ms_wkb.addHeader(WKBConstants::wkbMultiSurface, false, false, true, true);
  ms_wkb.addU32(2, true);
  ms_wkb.addPolygon({{{0.0, 0.0, 0.0, 0.0},
                      {2.0, 0.0, 0.0, 0.0},
                      {2.0, 2.0, 0.0, 0.0},
                      {0.0, 2.0, 0.0, 0.0},
                      {0.0, 0.0, 0.0, 0.0}}},
                    false, false, true, true);
  ms_wkb.addHeader(WKBConstants::wkbCurvePolygon, false, false, true, false);
  ms_wkb.addU32(1, false);
  ms_wkb.addCircularString({{-1.0, 0.0, 0.0, 0.0},
                            {0.0, 1.0, 0.0, 0.0},
                            {1.0, 0.0, 0.0, 0.0},
                            {0.0, -1.0, 0.0, 0.0},
                            {-1.0, 0.0, 0.0, 0.0}},
                           false, false, true, true);
  std::unique_ptr<Geometry> ms_geom = reader.read(ms_wkb.data(), ms_wkb.size());
  const auto* ms = dynamic_cast<const MultiSurface*>(ms_geom.get());
  ASSERT_NE(ms, nullptr);
  EXPECT_EQ(ms->getGeometryTypeId(), GEOS_MULTISURFACE);
  ASSERT_EQ(ms->getNumGeometries(), 2u);
  EXPECT_EQ(ms->getGeometryN(0)->getGeometryTypeId(), GEOS_POLYGON);
  EXPECT_EQ(ms->getGeometryN(1)->getGeometryTypeId(), GEOS_CURVEPOLYGON);
  EXPECT_NEAR(ms->getArea(), 4.0 + std::numbers::pi, 1e-9);

  // Wrong child in MultiSurface.
  WkbBuilder bad_ms;
  bad_ms.addHeader(WKBConstants::wkbMultiSurface, false, false, true, true);
  bad_ms.addU32(1, true);
  bad_ms.addLineString({}, false, false, true, true);
  ExpectParseExceptionWith([&] { reader.read(bad_ms.data(), bad_ms.size()); },
                           "Expected Surface but got LineString");

  // Nested heterogeneous GeometryCollection.
  WkbBuilder gc_wkb;
  gc_wkb.addHeader(WKBConstants::wkbGeometryCollection, false, false, false,
                   true);
  gc_wkb.addU32(3, true);
  gc_wkb.addPoint(1.0, 2.0, false, 0.0, false, 0.0, false, true);
  gc_wkb.addCircularString(
      {{-1.0, 0.0, 0.0, 0.0}, {0.0, 1.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0}},
      false, false, true, true);
  gc_wkb.addHeader(WKBConstants::wkbGeometryCollection, false, false, true,
                   false);
  gc_wkb.addU32(1, false);
  gc_wkb.addPolygon({}, false, false, true, false);
  std::unique_ptr<Geometry> gc = reader.read(gc_wkb.data(), gc_wkb.size());
  const auto* gc_ptr = dynamic_cast<const GeometryCollection*>(gc.get());
  ASSERT_NE(gc_ptr, nullptr);
  EXPECT_EQ(gc_ptr->getGeometryTypeId(), GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(gc_ptr->getNumGeometries(), 3u);
  EXPECT_EQ(gc_ptr->getGeometryN(0)->getGeometryTypeId(), GEOS_POINT);
  EXPECT_EQ(gc_ptr->getGeometryN(1)->getGeometryTypeId(), GEOS_CIRCULARSTRING);
  EXPECT_EQ(gc_ptr->getGeometryN(2)->getGeometryTypeId(),
            GEOS_GEOMETRYCOLLECTION);
  EXPECT_TRUE(gc_ptr->getGeometryN(2)->isEmpty());
}

TEST(WKBReaderTest, ReadHexSupportsAllHexCasesAndRejectsInvalidInput) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKBReader reader(*factory);

  // Choose finite double bit patterns covering all nibbles 0-9 and A-F.
  constexpr uint64_t kBitsX = 0x40123456789ABCDEULL;
  constexpr uint64_t kBitsY = 0x40F0123456789ABCULL;
  const double expected_x = std::bit_cast<double>(kBitsX);
  const double expected_y = std::bit_cast<double>(kBitsY);

  WkbBuilder wkb;
  wkb.addPoint(expected_x, expected_y, false, 0.0, false, 0.0, false, true);

  for (const bool lowercase : {false, true}) {
    std::istringstream hex_in(wkb.toHexString(lowercase));
    std::unique_ptr<Geometry> geom = reader.readHEX(hex_in);
    const auto* pt = dynamic_cast<const Point*>(geom.get());
    ASSERT_NE(pt, nullptr);
    EXPECT_EQ(std::bit_cast<uint64_t>(pt->getX()), kBitsX);
    EXPECT_EQ(std::bit_cast<uint64_t>(pt->getY()), kBitsY);
  }

  // Invalid hex character in high nibble, low nibble, and whitespace.
  for (const std::string_view bad_hex :
       {"G0", "0G", "z1", "1/", "01 2", "01@2"}) {
    SCOPED_TRACE(bad_hex);
    std::istringstream bad_in{std::string(bad_hex)};
    ExpectParseExceptionWith([&] { reader.readHEX(bad_in); },
                             "Invalid HEX char");
  }

  // Odd length hex string.
  for (const std::string_view odd_hex : {"0", "010", "010100000"}) {
    SCOPED_TRACE(odd_hex);
    std::istringstream odd_in{std::string(odd_hex)};
    ExpectParseExceptionWith([&] { reader.readHEX(odd_in); },
                             "Premature end of HEX string");
  }

  // Empty hex stream.
  std::istringstream empty_in("");
  ExpectParseExceptionWith([&] { reader.readHEX(empty_in); }, "Unexpected EOF");
}

TEST(WKBReaderTest, PrintHexFormatsAllNibblesAndPreservesStreamPosition) {
  const std::string raw_bytes("\x00\x12\x34\x56\x78\x9A\xBC\xDE\xF0", 9);
  std::istringstream is(raw_bytes, std::ios::binary | std::ios::in);
  is.seekg(4);

  std::ostringstream os;
  std::ostream& ret = WKBReader::printHEX(is, os);
  EXPECT_EQ(&ret, &os);
  EXPECT_EQ(os.str(), "00123456789ABCDEF0");
  EXPECT_FALSE(is.eof());
  EXPECT_TRUE(is.good());
  EXPECT_EQ(is.tellg(), std::streampos(4));
  EXPECT_EQ(is.get(), 0x78);

  // Empty stream.
  std::istringstream empty_is("", std::ios::binary | std::ios::in);
  std::ostringstream empty_os;
  WKBReader::printHEX(empty_is, empty_os);
  EXPECT_EQ(empty_os.str(), "");
  EXPECT_FALSE(empty_is.eof());
}

TEST(WKBReaderTest, RejectsUnknownTypesTruncatedBuffersAndOversizedCounts) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKBReader reader(*factory);

  // Unknown WKB geometry types.
  for (const uint32_t bad_type : {0u, 13u, 999u, 1000u, 1013u}) {
    WkbBuilder wkb;
    wkb.addRawHeader(WKBConstants::wkbNDR, bad_type, true);
    ExpectParseExceptionWith([&] { reader.read(wkb.data(), wkb.size()); },
                             "Unknown WKB type");
  }

  // Empty buffer and empty stream.
  const unsigned char dummy = 0;
  ExpectParseExceptionWith([&] { reader.read(&dummy, 0); }, "Unexpected EOF");
  std::istringstream empty_is("", std::ios::binary | std::ios::in);
  ExpectParseExceptionWith([&] { reader.read(empty_is); }, "Unexpected EOF");

  // Truncated header (byte order only).
  const unsigned char byte_order_only = WKBConstants::wkbNDR;
  ExpectParseExceptionWith([&] { reader.read(&byte_order_only, 1); },
                           "Unexpected EOF");

  // Truncated SRID.
  WkbBuilder trunc_srid;
  trunc_srid.addRawHeader(WKBConstants::wkbNDR,
                          WKBConstants::wkbPoint | kSfsqlSridFlag, true);
  ExpectParseExceptionWith(
      [&] { reader.read(trunc_srid.data(), trunc_srid.size()); },
      "Unexpected EOF");

  // Truncated XYZ coordinate (has XY, missing Z -> passes minMemSize, hits
  // EOF).
  WkbBuilder trunc_z;
  trunc_z.addHeader(WKBConstants::wkbPoint, true, false, false, true);
  trunc_z.addCoord(1.0, 2.0, true);
  ExpectParseExceptionWith([&] { reader.read(trunc_z.data(), trunc_z.size()); },
                           "Unexpected EOF");

  // Truncated XYZM coordinate (missing M).
  WkbBuilder trunc_m;
  trunc_m.addHeader(WKBConstants::wkbPoint, true, true, false, true);
  trunc_m.addCoord(1.0, 2.0, true, 3.0, false, 0.0, true);
  ExpectParseExceptionWith([&] { reader.read(trunc_m.data(), trunc_m.size()); },
                           "Unexpected EOF");

  // Truncated Point XY coordinate triggers minMemSize in
  // readCoordinateSequence.
  WkbBuilder trunc_xy;
  trunc_xy.addHeader(WKBConstants::wkbPoint, false, false, false, true);
  trunc_xy.addDouble(1.0, true);
  ExpectParseExceptionWith(
      [&] { reader.read(trunc_xy.data(), trunc_xy.size()); },
      "Input buffer is smaller than requested object size");

  // Oversized count triggers minMemSize across all counted types.
  for (const uint32_t counted_type :
       {WKBConstants::wkbLineString, WKBConstants::wkbCircularString,
        WKBConstants::wkbCompoundCurve, WKBConstants::wkbPolygon,
        WKBConstants::wkbCurvePolygon, WKBConstants::wkbMultiPoint,
        WKBConstants::wkbMultiLineString, WKBConstants::wkbMultiCurve,
        WKBConstants::wkbMultiPolygon, WKBConstants::wkbMultiSurface,
        WKBConstants::wkbGeometryCollection}) {
    WkbBuilder huge_count;
    huge_count.addHeader(counted_type, false, false, true, true);
    huge_count.addU32(1000u, true);
    ExpectParseExceptionWith(
        [&] { reader.read(huge_count.data(), huge_count.size()); },
        "Input buffer is smaller than requested object size");
  }

  // Polygon ring with oversized point count triggers readLinearRing minMemSize.
  WkbBuilder huge_ring;
  huge_ring.addHeader(WKBConstants::wkbPolygon, false, false, false, true);
  huge_ring.addU32(1u, true);
  huge_ring.addU32(1000u, true);
  ExpectParseExceptionWith(
      [&] { reader.read(huge_ring.data(), huge_ring.size()); },
      "Input buffer is smaller than requested object size");

  // Verify reader remains reusable after ParseException.
  WkbBuilder valid_pt;
  valid_pt.addPoint(9.0, 8.0, false, 0.0, false, 0.0, false, true);
  std::unique_ptr<Geometry> recovered =
      reader.read(valid_pt.data(), valid_pt.size());
  const auto* pt = dynamic_cast<const Point*>(recovered.get());
  ASSERT_NE(pt, nullptr);
  EXPECT_DOUBLE_EQ(pt->getX(), 9.0);
  EXPECT_DOUBLE_EQ(pt->getY(), 8.0);
}

TEST(WKBReaderTest, RoundTripsWithWkbWriterAcrossFormatsAndTypes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKBReader reader(*factory);

  std::vector<std::unique_ptr<Geometry>> items;
  items.push_back(factory->createPoint(CoordinateXYZM(1.0, 2.0, 3.0, 4.0)));
  items.push_back(factory->createLineString(CoordinateSequence{
      CoordinateXYZM(0.0, 0.0, 1.0, 2.0),
      CoordinateXYZM(5.0, 6.0, 7.0, 8.0),
  }));
  items.push_back(factory->createCircularString(CoordinateSequence{
      CoordinateXYZM(-1.0, 0.0, 0.0, 1.0),
      CoordinateXYZM(0.0, 1.0, 1.0, 2.0),
      CoordinateXYZM(1.0, 0.0, 2.0, 3.0),
  }));
  {
    auto shell = factory->createLinearRing(CoordinateSequence{
        CoordinateXYZM(0.0, 0.0, 1.0, 10.0),
        CoordinateXYZM(10.0, 0.0, 2.0, 20.0),
        CoordinateXYZM(10.0, 10.0, 3.0, 30.0),
        CoordinateXYZM(0.0, 10.0, 4.0, 40.0),
        CoordinateXYZM(0.0, 0.0, 1.0, 10.0),
    });
    std::vector<std::unique_ptr<LinearRing>> holes;
    holes.push_back(factory->createLinearRing(CoordinateSequence{
        CoordinateXYZM(2.0, 2.0, 5.0, 50.0),
        CoordinateXYZM(4.0, 2.0, 5.0, 50.0),
        CoordinateXYZM(4.0, 4.0, 5.0, 50.0),
        CoordinateXYZM(2.0, 4.0, 5.0, 50.0),
        CoordinateXYZM(2.0, 2.0, 5.0, 50.0),
    }));
    items.push_back(factory->createPolygon(std::move(shell), std::move(holes)));
  }
  {
    std::vector<std::unique_ptr<SimpleCurve>> curves;
    curves.push_back(factory->createLineString(CoordinateSequence{
        CoordinateXYZM(1.0, 0.0, 1.0, 10.0),
        CoordinateXYZM(-1.0, 0.0, 2.0, 20.0),
    }));
    curves.push_back(factory->createCircularString(CoordinateSequence{
        CoordinateXYZM(-1.0, 0.0, 2.0, 20.0),
        CoordinateXYZM(0.0, 1.0, 3.0, 30.0),
        CoordinateXYZM(1.0, 0.0, 1.0, 10.0),
    }));
    items.push_back(factory->createCompoundCurve(std::move(curves)));
  }
  items.push_back(factory->createMultiPoint(CoordinateSequence{
      CoordinateXYZM(7.0, 8.0, 9.0, 10.0),
      CoordinateXYZM(11.0, 12.0, 13.0, 14.0),
  }));
  {
    std::vector<std::unique_ptr<LineString>> lines;
    lines.push_back(factory->createLineString(CoordinateSequence{
        CoordinateXYZM(0.0, 0.0, 1.0, 2.0),
        CoordinateXYZM(3.0, 4.0, 5.0, 6.0),
    }));
    items.push_back(factory->createMultiLineString(std::move(lines)));
  }
  {
    std::vector<std::unique_ptr<Polygon>> polys;
    polys.push_back(factory->createPolygon(CoordinateSequence{
        CoordinateXYZM(0.0, 0.0, 1.0, 2.0),
        CoordinateXYZM(2.0, 0.0, 3.0, 4.0),
        CoordinateXYZM(2.0, 2.0, 5.0, 6.0),
        CoordinateXYZM(0.0, 2.0, 7.0, 8.0),
        CoordinateXYZM(0.0, 0.0, 1.0, 2.0),
    }));
    items.push_back(factory->createMultiPolygon(std::move(polys)));
  }

  std::unique_ptr<Geometry> collection =
      factory->createGeometryCollection(std::move(items));
  collection->setSRID(4326);

  for (const int byte_order :
       {ByteOrderValues::ENDIAN_LITTLE, ByteOrderValues::ENDIAN_BIG}) {
    for (const int flavor : {WKBConstants::wkbExtended, WKBConstants::wkbIso}) {
      const bool include_srid = (flavor == WKBConstants::wkbExtended);
      WKBWriter writer(/*dims=*/4, byte_order, include_srid, flavor);

      std::stringstream bin_stream(std::ios::binary | std::ios::in |
                                   std::ios::out);
      writer.write(*collection, bin_stream);
      std::unique_ptr<Geometry> decoded_bin = reader.read(bin_stream);
      ASSERT_NE(decoded_bin, nullptr);
      EXPECT_TRUE(decoded_bin->equalsIdentical(collection.get()));
      EXPECT_EQ(decoded_bin->getSRID(), include_srid ? 4326 : 0);

      std::stringstream hex_stream;
      writer.writeHEX(*collection, hex_stream);
      std::unique_ptr<Geometry> decoded_hex = reader.readHEX(hex_stream);
      ASSERT_NE(decoded_hex, nullptr);
      EXPECT_TRUE(decoded_hex->equalsIdentical(collection.get()));
      EXPECT_EQ(decoded_hex->getSRID(), include_srid ? 4326 : 0);
    }
  }
}

}  // namespace
}  // namespace geos_google
