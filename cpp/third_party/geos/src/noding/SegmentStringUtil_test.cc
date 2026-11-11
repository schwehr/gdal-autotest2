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

// Unit tests for third_party/geos/include/geos/noding/SegmentStringUtil.h
// (and third_party/geos/src/noding/SegmentStringUtil.cpp).
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/noding/SegmentStringUtil.h

#include "third_party/geos/include/geos/noding/SegmentStringUtil.h"

#include <array>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/noding/BasicSegmentString.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::io::WKTReader;
using geos::noding::BasicSegmentString;
using geos::noding::SegmentString;
using geos::noding::SegmentStringUtil;

// Owns SegmentString* pointers appended by
// SegmentStringUtil::extractSegmentStrings. Note that BasicSegmentString
// borrows its CoordinateSequence* from the source Geometry, so the source
// Geometry must outlive this guard.
class ScopedConstSegmentStrings {
 public:
  ScopedConstSegmentStrings() = default;
  ScopedConstSegmentStrings(const ScopedConstSegmentStrings&) = delete;
  ScopedConstSegmentStrings& operator=(const ScopedConstSegmentStrings&) =
      delete;

  ~ScopedConstSegmentStrings() {
    for (const SegmentString* ss : strings_) {
      delete ss;
    }
  }

  SegmentString::ConstVect& vec() { return strings_; }
  const SegmentString::ConstVect& vec() const { return strings_; }

 private:
  SegmentString::ConstVect strings_;
};

TEST(SegmentStringUtilTest, ExtractsFromLineString) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  Geometry::Ptr geom = reader.read("LINESTRING (0 0, 1 2, 3 4)");
  ASSERT_NE(geom, nullptr);

  ScopedConstSegmentStrings segs;
  SegmentStringUtil::extractSegmentStrings(geom.get(), segs.vec());

  ASSERT_EQ(segs.vec().size(), 1u);
  const SegmentString* ss = segs.vec()[0];
  ASSERT_NE(ss, nullptr);
  EXPECT_NE(dynamic_cast<const BasicSegmentString*>(ss), nullptr);
  EXPECT_EQ(ss->getData(), static_cast<const void*>(geom.get()));
  EXPECT_FALSE(ss->isClosed());
  ASSERT_EQ(ss->size(), 3u);
  EXPECT_EQ(ss->getCoordinate<CoordinateXY>(0), CoordinateXY(0.0, 0.0));
  EXPECT_EQ(ss->getCoordinate<CoordinateXY>(1), CoordinateXY(1.0, 2.0));
  EXPECT_EQ(ss->getCoordinate<CoordinateXY>(2), CoordinateXY(3.0, 4.0));

  const auto* line = dynamic_cast<const LineString*>(geom.get());
  ASSERT_NE(line, nullptr);
  EXPECT_EQ(ss->getCoordinates(), line->getCoordinatesRO());

  std::ostringstream oss;
  oss << *ss;
  EXPECT_FALSE(oss.str().empty());
}

TEST(SegmentStringUtilTest, ExtractsFromLinearRing) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto seq =
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {0.0, 0.0}, {2.0, 0.0}, {2.0, 2.0}, {0.0, 2.0}, {0.0, 0.0}});
  std::unique_ptr<LinearRing> ring = factory->createLinearRing(std::move(seq));
  ASSERT_NE(ring, nullptr);

  ScopedConstSegmentStrings segs;
  SegmentStringUtil::extractSegmentStrings(ring.get(), segs.vec());

  ASSERT_EQ(segs.vec().size(), 1u);
  const SegmentString* ss = segs.vec()[0];
  ASSERT_NE(ss, nullptr);
  EXPECT_EQ(ss->getData(), static_cast<const void*>(ring.get()));
  EXPECT_TRUE(ss->isClosed());
  ASSERT_EQ(ss->size(), 5u);
  EXPECT_EQ(ss->getCoordinate<CoordinateXY>(0), CoordinateXY(0.0, 0.0));
  EXPECT_EQ(ss->getCoordinate<CoordinateXY>(2), CoordinateXY(2.0, 2.0));
}

TEST(SegmentStringUtilTest, ExtractsShellAndHolesFromPolygon) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  Geometry::Ptr poly = reader.read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0),"
      " (1 1, 1 3, 3 3, 3 1, 1 1),"
      " (5 5, 5 8, 8 8, 8 5, 5 5))");
  ASSERT_NE(poly, nullptr);

  ScopedConstSegmentStrings segs;
  SegmentStringUtil::extractSegmentStrings(poly.get(), segs.vec());

  ASSERT_EQ(segs.vec().size(), 3u);
  for (const SegmentString* ss : segs.vec()) {
    ASSERT_NE(ss, nullptr);
    EXPECT_NE(dynamic_cast<const BasicSegmentString*>(ss), nullptr);
    EXPECT_EQ(ss->getData(), static_cast<const void*>(poly.get()));
    EXPECT_TRUE(ss->isClosed());
    EXPECT_EQ(ss->size(), 5u);
  }
  EXPECT_EQ(segs.vec()[0]->getCoordinate<CoordinateXY>(2),
            CoordinateXY(10.0, 10.0));
  EXPECT_EQ(segs.vec()[1]->getCoordinate<CoordinateXY>(2),
            CoordinateXY(3.0, 3.0));
  EXPECT_EQ(segs.vec()[2]->getCoordinate<CoordinateXY>(2),
            CoordinateXY(8.0, 8.0));
}

TEST(SegmentStringUtilTest, ExtractsFromMultiLineStringAndMultiPolygon) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  Geometry::Ptr mls =
      reader.read("MULTILINESTRING ((0 0, 1 1), EMPTY, (2 2, 3 3, 4 4))");
  Geometry::Ptr mp = reader.read(
      "MULTIPOLYGON ("
      "((0 0, 4 0, 4 4, 0 4, 0 0), (1 1, 1 2, 2 2, 2 1, 1 1)),"
      "((10 10, 12 10, 12 12, 10 12, 10 10)))");
  ASSERT_NE(mls, nullptr);
  ASSERT_NE(mp, nullptr);

  {
    ScopedConstSegmentStrings mls_segs;
    SegmentStringUtil::extractSegmentStrings(mls.get(), mls_segs.vec());
    ASSERT_EQ(mls_segs.vec().size(), 2u);
    EXPECT_EQ(mls_segs.vec()[0]->getData(),
              static_cast<const void*>(mls.get()));
    EXPECT_EQ(mls_segs.vec()[1]->getData(),
              static_cast<const void*>(mls.get()));
    EXPECT_EQ(mls_segs.vec()[0]->size(), 2u);
    EXPECT_EQ(mls_segs.vec()[1]->size(), 3u);
  }

  {
    ScopedConstSegmentStrings mp_segs;
    SegmentStringUtil::extractSegmentStrings(mp.get(), mp_segs.vec());
    ASSERT_EQ(mp_segs.vec().size(), 3u);
    for (const SegmentString* ss : mp_segs.vec()) {
      EXPECT_EQ(ss->getData(), static_cast<const void*>(mp.get()));
      EXPECT_TRUE(ss->isClosed());
      EXPECT_EQ(ss->size(), 5u);
    }
  }
}

TEST(SegmentStringUtilTest, ExtractsFromHeterogeneousGeometryCollection) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  Geometry::Ptr gc = reader.read(
      "GEOMETRYCOLLECTION ("
      "POINT (9 9),"
      "LINESTRING EMPTY,"
      "LINESTRING (0 0, 2 3),"
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0)))");
  ASSERT_NE(gc, nullptr);

  ScopedConstSegmentStrings segs;
  SegmentStringUtil::extractSegmentStrings(gc.get(), segs.vec());

  ASSERT_EQ(segs.vec().size(), 2u);
  EXPECT_EQ(segs.vec()[0]->getData(), static_cast<const void*>(gc.get()));
  EXPECT_EQ(segs.vec()[1]->getData(), static_cast<const void*>(gc.get()));
  EXPECT_EQ(segs.vec()[0]->size(), 2u);
  EXPECT_FALSE(segs.vec()[0]->isClosed());
  EXPECT_EQ(segs.vec()[1]->size(), 5u);
  EXPECT_TRUE(segs.vec()[1]->isClosed());
}

TEST(SegmentStringUtilTest, IgnoresZeroDimAndEmptyGeometries) {
  struct EmptyOrZeroDimCase {
    std::string_view name;
    std::string_view wkt;
  };
  constexpr auto kCases = std::to_array<EmptyOrZeroDimCase>({
      {.name = "point", .wkt = "POINT (1 2)"},
      {.name = "point_empty", .wkt = "POINT EMPTY"},
      {.name = "multipoint", .wkt = "MULTIPOINT ((0 0), (1 1))"},
      {.name = "linestring_empty", .wkt = "LINESTRING EMPTY"},
      {.name = "linearring_empty", .wkt = "LINEARRING EMPTY"},
      {.name = "polygon_empty", .wkt = "POLYGON EMPTY"},
      {.name = "multilinestring_empty", .wkt = "MULTILINESTRING EMPTY"},
      {.name = "multipolygon_empty", .wkt = "MULTIPOLYGON EMPTY"},
      {.name = "collection_empty", .wkt = "GEOMETRYCOLLECTION EMPTY"},
      {.name = "collection_of_points",
       .wkt = "GEOMETRYCOLLECTION (POINT (0 0), MULTIPOINT ((1 1)))"},
  });

  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  for (const auto& tc : kCases) {
    SCOPED_TRACE(std::string(tc.name));
    Geometry::Ptr geom = reader.read(std::string(tc.wkt));
    ASSERT_NE(geom, nullptr);

    ScopedConstSegmentStrings segs;
    SegmentStringUtil::extractSegmentStrings(geom.get(), segs.vec());
    EXPECT_TRUE(segs.vec().empty());
  }
}

TEST(SegmentStringUtilTest, AppendsToNonEmptyVectorWithoutClearing) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  Geometry::Ptr line1 = reader.read("LINESTRING (0 0, 1 0)");
  Geometry::Ptr line2 = reader.read("LINESTRING (2 0, 3 0, 4 0)");
  ASSERT_NE(line1, nullptr);
  ASSERT_NE(line2, nullptr);

  ScopedConstSegmentStrings segs;
  SegmentStringUtil::extractSegmentStrings(line1.get(), segs.vec());
  ASSERT_EQ(segs.vec().size(), 1u);

  SegmentStringUtil::extractSegmentStrings(line2.get(), segs.vec());
  ASSERT_EQ(segs.vec().size(), 2u);
  EXPECT_EQ(segs.vec()[0]->getData(), static_cast<const void*>(line1.get()));
  EXPECT_EQ(segs.vec()[1]->getData(), static_cast<const void*>(line2.get()));
  EXPECT_EQ(segs.vec()[0]->size(), 2u);
  EXPECT_EQ(segs.vec()[1]->size(), 3u);
}

TEST(SegmentStringUtilTest, PreservesZAndMCoordinates) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto xyzm_seq = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXYZM>{{1.0, 2.0, 3.0, 4.0},
                                            {5.0, 6.0, 7.0, 8.0}});
  std::unique_ptr<LineString> xyzm_line =
      factory->createLineString(std::move(xyzm_seq));

  auto xym_seq =
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXYM>{
          {10.0, 20.0, 30.0}, {40.0, 50.0, 60.0}});
  std::unique_ptr<LineString> xym_line =
      factory->createLineString(std::move(xym_seq));

  ScopedConstSegmentStrings segs;
  SegmentStringUtil::extractSegmentStrings(xyzm_line.get(), segs.vec());
  SegmentStringUtil::extractSegmentStrings(xym_line.get(), segs.vec());

  ASSERT_EQ(segs.vec().size(), 2u);

  const CoordinateSequence* out_xyzm = segs.vec()[0]->getCoordinates();
  EXPECT_TRUE(out_xyzm->hasZ());
  EXPECT_TRUE(out_xyzm->hasM());
  CoordinateXYZM c0;
  out_xyzm->getAt(1, c0);
  EXPECT_TRUE(c0.equals4D(CoordinateXYZM(5.0, 6.0, 7.0, 8.0)));

  // Use 2-arg getAt(i, out) on XYM sequences because GEOS_COORDSEQ_PADZ pads Z
  // at slot 2 and places M at slot 3.
  const CoordinateSequence* out_xym = segs.vec()[1]->getCoordinates();
  EXPECT_FALSE(out_xym->hasZ());
  EXPECT_TRUE(out_xym->hasM());
  CoordinateXYM c1;
  out_xym->getAt(1, c1);
  EXPECT_DOUBLE_EQ(c1.x, 40.0);
  EXPECT_DOUBLE_EQ(c1.y, 50.0);
  EXPECT_DOUBLE_EQ(c1.m, 60.0);
  EXPECT_TRUE(std::isnan(out_xym->getOrdinate(1, CoordinateSequence::Z)));
}

}  // namespace
}  // namespace geos_google
