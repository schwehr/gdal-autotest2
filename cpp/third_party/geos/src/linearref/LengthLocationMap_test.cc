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

#include "third_party/geos/include/geos/linearref/LengthLocationMap.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/linearref/LinearLocation.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::io::WKTReader;
using geos::linearref::LengthLocationMap;
using geos::linearref::LinearLocation;

void ExpectLocationEq(const LinearLocation& actual, std::size_t comp,
                      std::size_t seg, double frac, double tol = 1e-12) {
  EXPECT_EQ(actual.getComponentIndex(), comp);
  EXPECT_EQ(actual.getSegmentIndex(), seg);
  EXPECT_NEAR(actual.getSegmentFraction(), frac, tol);
}

class LengthLocationMapTest : public ::testing::Test {
 protected:
  LengthLocationMapTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) {
    return reader_.read(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(LengthLocationMapTest, SingleLineStringLocationsAndLengths) {
  auto line = ReadWkt("LINESTRING (0 0, 10 0, 10 10)");
  LengthLocationMap map(line.get());

  // At start and before start.
  const LinearLocation loc_zero = map.getLocation(0.0);
  ExpectLocationEq(loc_zero, 0, 0, 0.0);
  EXPECT_DOUBLE_EQ(map.getLength(loc_zero), 0.0);
  EXPECT_DOUBLE_EQ(LengthLocationMap::getLength(line.get(), loc_zero), 0.0);

  // Midpoint of first segment.
  const LinearLocation loc_seg0_mid = map.getLocation(5.0);
  ExpectLocationEq(loc_seg0_mid, 0, 0, 0.5);
  EXPECT_DOUBLE_EQ(map.getLength(loc_seg0_mid), 5.0);
  EXPECT_TRUE(
      loc_seg0_mid.getCoordinate(line.get()).equals2D(Coordinate(5, 0)));

  // Exact interior vertex resolves to start of next segment.
  const LinearLocation loc_vertex = map.getLocation(10.0);
  ExpectLocationEq(loc_vertex, 0, 1, 0.0);
  EXPECT_DOUBLE_EQ(map.getLength(loc_vertex), 10.0);
  EXPECT_TRUE(loc_vertex.getCoordinate(line.get()).equals2D(Coordinate(10, 0)));

  // Fractional point on second segment.
  const LinearLocation loc_seg1 = map.getLocation(12.5);
  ExpectLocationEq(loc_seg1, 0, 1, 0.25);
  EXPECT_DOUBLE_EQ(map.getLength(loc_seg1), 12.5);
  EXPECT_TRUE(loc_seg1.getCoordinate(line.get()).equals2D(Coordinate(10, 2.5)));

  // Exact total length hits end-of-line branch in getLocationForward.
  const LinearLocation loc_exact_end = map.getLocation(20.0);
  ExpectLocationEq(loc_exact_end, 0, 2, 0.0);
  EXPECT_DOUBLE_EQ(map.getLength(loc_exact_end), 20.0);
  EXPECT_TRUE(
      loc_exact_end.getCoordinate(line.get()).equals2D(Coordinate(10, 10)));

  // Beyond total length clamps to end location (0, 2, 1.0).
  const LinearLocation loc_clamped = map.getLocation(35.0);
  ExpectLocationEq(loc_clamped, 0, 2, 1.0);
  EXPECT_DOUBLE_EQ(map.getLength(loc_clamped), 20.0);
  EXPECT_DOUBLE_EQ(LengthLocationMap::getLength(line.get(), loc_clamped), 20.0);

  // Static convenience wrappers.
  ExpectLocationEq(LengthLocationMap::getLocation(line.get(), 15.0), 0, 1, 0.5);
  ExpectLocationEq(LengthLocationMap::getLocation(line.get(), 20.0, false), 0,
                   2, 0.0);
}

TEST_F(LengthLocationMapTest, NegativeLengthsMeasuredFromEnd) {
  auto line = ReadWkt("LINESTRING (0 0, 10 0, 10 20)");
  LengthLocationMap map(line.get());

  const LinearLocation loc_minus_5 = map.getLocation(-5.0);
  ExpectLocationEq(loc_minus_5, 0, 1, 0.75);
  EXPECT_DOUBLE_EQ(map.getLength(loc_minus_5), 25.0);

  ExpectLocationEq(map.getLocation(-20.0), 0, 1, 0.0);
  ExpectLocationEq(map.getLocation(-30.0), 0, 0, 0.0);
  // Beyond start clamps to (0, 0, 0.0).
  ExpectLocationEq(map.getLocation(-100.0), 0, 0, 0.0);

  // Negative lengths with resolveLower parameter on MultiLineString.
  auto mls = ReadWkt("MULTILINESTRING ((0 0, 10 0), (20 0, 30 0))");
  LengthLocationMap mls_map(mls.get());
  ExpectLocationEq(mls_map.getLocation(-10.0, true), 0, 1, 0.0);
  ExpectLocationEq(mls_map.getLocation(-10.0, false), 1, 0, 0.0);
  ExpectLocationEq(mls_map.getLocation(-5.0, false), 1, 0, 0.5);
}

TEST_F(LengthLocationMapTest, MultiLineStringResolveLowerAndHigher) {
  auto mls = ReadWkt("MULTILINESTRING ((0 0, 10 0), (20 0, 30 0, 30 10))");
  LengthLocationMap map(mls.get());

  // Component boundary at length 10.0.
  const LinearLocation loc_lower = map.getLocation(10.0, true);
  ExpectLocationEq(loc_lower, 0, 1, 0.0);
  EXPECT_DOUBLE_EQ(map.getLength(loc_lower), 10.0);

  const LinearLocation loc_higher = map.getLocation(10.0, false);
  ExpectLocationEq(loc_higher, 1, 0, 0.0);
  EXPECT_DOUBLE_EQ(map.getLength(loc_higher), 10.0);

  // Interior of second component: crosses end of component 0 where
  // totalLength != length, and !isEndpoint returns location unchanged.
  const LinearLocation loc_mid_comp1 = map.getLocation(15.0, false);
  ExpectLocationEq(loc_mid_comp1, 1, 0, 0.5);
  EXPECT_DOUBLE_EQ(map.getLength(loc_mid_comp1), 15.0);
  EXPECT_DOUBLE_EQ(map.getLength(LinearLocation(1, 1, 0.5)), 25.0);

  // Exact end of last component cannot resolve higher.
  const LinearLocation loc_last_end = map.getLocation(30.0, false);
  ExpectLocationEq(loc_last_end, 1, 2, 0.0);
  EXPECT_DOUBLE_EQ(map.getLength(loc_last_end), 30.0);

  // Out-of-range component index iterates to end and returns total length.
  EXPECT_DOUBLE_EQ(map.getLength(LinearLocation(5, 0, 0.0)), 30.0);
}

TEST_F(LengthLocationMapTest, ResolveHigherSkipsZeroLengthComponents) {
  auto mls = ReadWkt(
      "MULTILINESTRING ((0 0, 10 0), (10 0, 10 0), (10 0, 10 0), (10 0, 25 "
      "0))");
  LengthLocationMap map(mls.get());

  ExpectLocationEq(map.getLocation(10.0, true), 0, 1, 0.0);
  ExpectLocationEq(map.getLocation(10.0, false), 3, 0, 0.0);
  EXPECT_DOUBLE_EQ(map.getLength(LinearLocation(3, 0, 0.0)), 10.0);
  EXPECT_DOUBLE_EQ(map.getLength(LinearLocation(3, 0, 0.5)), 17.5);

  // When the trailing component itself has zero length, resolveHigher stops at
  // the last component index.
  auto trailing_zero = ReadWkt("MULTILINESTRING ((0 0, 10 0), (10 0, 10 0))");
  ExpectLocationEq(
      LengthLocationMap::getLocation(trailing_zero.get(), 10.0, false), 1, 0,
      0.0);

  // Zero-length single component line and repeated intermediate vertex.
  auto zero_line = ReadWkt("LINESTRING (5 5, 5 5)");
  ExpectLocationEq(LengthLocationMap::getLocation(zero_line.get(), 0.0, false),
                   0, 0, 0.0);
  ExpectLocationEq(LengthLocationMap::getLocation(zero_line.get(), 5.0), 0, 1,
                   1.0);
  EXPECT_DOUBLE_EQ(
      LengthLocationMap::getLength(zero_line.get(), LinearLocation(0, 0, 0.5)),
      0.0);

  auto repeated_vertex = ReadWkt("LINESTRING (0 0, 5 0, 5 0, 10 0)");
  ExpectLocationEq(LengthLocationMap::getLocation(repeated_vertex.get(), 5.0),
                   0, 2, 0.0);
  ExpectLocationEq(LengthLocationMap::getLocation(repeated_vertex.get(), 7.5),
                   0, 2, 0.5);
  EXPECT_DOUBLE_EQ(LengthLocationMap::getLength(repeated_vertex.get(),
                                                LinearLocation(0, 1, 0.5)),
                   5.0);
}

TEST_F(LengthLocationMapTest, EmptyGeometries) {
  auto empty_mls = ReadWkt("MULTILINESTRING EMPTY");
  LengthLocationMap mls_map(empty_mls.get());
  ExpectLocationEq(mls_map.getLocation(0.0), 0, 0, 0.0);
  ExpectLocationEq(mls_map.getLocation(5.0), 0, 0, 0.0);
  ExpectLocationEq(mls_map.getLocation(-5.0), 0, 0, 0.0);
  EXPECT_DOUBLE_EQ(mls_map.getLength(LinearLocation(0, 0, 0.0)), 0.0);

  auto empty_line = ReadWkt("LINESTRING EMPTY");
  LengthLocationMap line_map(empty_line.get());
  ExpectLocationEq(line_map.getLocation(0.0), 0, 0, 0.0);
  ExpectLocationEq(line_map.getLocation(-1.0), 0, 0, 0.0);
  EXPECT_DOUBLE_EQ(line_map.getLength(LinearLocation(0, 0, 0.0)), 0.0);
}

TEST_F(LengthLocationMapTest, LinearRingDiagonalAnd3DPlanarLength) {
  auto ring = ReadWkt("LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  ExpectLocationEq(LengthLocationMap::getLocation(ring.get(), 15.0), 0, 1, 0.5);
  ExpectLocationEq(LengthLocationMap::getLocation(ring.get(), 40.0, false), 0,
                   4, 0.0);
  EXPECT_DOUBLE_EQ(
      LengthLocationMap::getLength(ring.get(), LinearLocation(0, 2, 0.5)),
      25.0);

  auto diag = ReadWkt("LINESTRING (0 0, 3 4, 3 9)");
  ExpectLocationEq(LengthLocationMap::getLocation(diag.get(), 2.5), 0, 0, 0.5);
  EXPECT_DOUBLE_EQ(
      LengthLocationMap::getLength(diag.get(), LinearLocation(0, 1, 0.5)), 7.5);

  // Linear referencing uses 2D planar length even when Z ordinates are set.
  auto line3d = ReadWkt("LINESTRING Z (0 0 0, 10 0 100)");
  ExpectLocationEq(LengthLocationMap::getLocation(line3d.get(), 5.0), 0, 0,
                   0.5);
  EXPECT_DOUBLE_EQ(
      LengthLocationMap::getLength(line3d.get(), LinearLocation(0, 0, 0.5)),
      5.0);
  const Coordinate mid3d = LengthLocationMap::getLocation(line3d.get(), 5.0)
                               .getCoordinate(line3d.get());
  EXPECT_DOUBLE_EQ(mid3d.x, 5.0);
  EXPECT_DOUBLE_EQ(mid3d.y, 0.0);
  EXPECT_DOUBLE_EQ(mid3d.z, 50.0);
}

}  // namespace
}  // namespace geos_google
