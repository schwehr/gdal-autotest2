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

// Unit tests for third_party/geos/src/linearref/LengthIndexedLine.cpp and
// third_party/geos/include/geos/linearref/LengthIndexedLine.h.

#include "third_party/geos/include/geos/linearref/LengthIndexedLine.h"

#include <cmath>
#include <limits>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/IllegalStateException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::io::WKTReader;
using geos::linearref::LengthIndexedLine;
using geos::util::IllegalArgumentException;
using geos::util::IllegalStateException;

class LengthIndexedLineTest : public ::testing::Test {
 protected:
  void ExpectWktEqual(const Geometry& actual, const std::string& expected_wkt,
                      double tolerance = 1e-9) const {
    SCOPED_TRACE(expected_wkt);
    std::unique_ptr<Geometry> expected = reader_.read(expected_wkt);
    ASSERT_NE(expected, nullptr);
    EXPECT_TRUE(actual.equalsExact(expected.get(), tolerance))
        << "actual=" << actual.toText() << " expected=" << expected->toText();
  }

  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader reader_{*factory_};
};

TEST_F(LengthIndexedLineTest, StartEndRangeValidationAndClamping) {
  std::unique_ptr<Geometry> line =
      reader_.read("LINESTRING (0 0, 10 0, 10 10)");
  LengthIndexedLine lil(line.get());

  EXPECT_DOUBLE_EQ(lil.getStartIndex(), 0.0);
  EXPECT_DOUBLE_EQ(lil.getEndIndex(), 20.0);

  EXPECT_FALSE(lil.isValidIndex(-0.1));
  EXPECT_TRUE(lil.isValidIndex(0.0));
  EXPECT_TRUE(lil.isValidIndex(12.5));
  EXPECT_TRUE(lil.isValidIndex(20.0));
  EXPECT_FALSE(lil.isValidIndex(20.1));
  EXPECT_FALSE(lil.isValidIndex(std::numeric_limits<double>::quiet_NaN()));
  EXPECT_FALSE(lil.isValidIndex(std::numeric_limits<double>::infinity()));
  EXPECT_FALSE(lil.isValidIndex(-std::numeric_limits<double>::infinity()));

  EXPECT_DOUBLE_EQ(lil.clampIndex(0.0), 0.0);
  EXPECT_DOUBLE_EQ(lil.clampIndex(7.5), 7.5);
  EXPECT_DOUBLE_EQ(lil.clampIndex(20.0), 20.0);
  EXPECT_DOUBLE_EQ(lil.clampIndex(25.0), 20.0);
  EXPECT_DOUBLE_EQ(lil.clampIndex(-4.0), 16.0);
  EXPECT_DOUBLE_EQ(lil.clampIndex(-20.0), 0.0);
  EXPECT_DOUBLE_EQ(lil.clampIndex(-25.0), 0.0);
  EXPECT_DOUBLE_EQ(lil.clampIndex(std::numeric_limits<double>::infinity()),
                   20.0);
  EXPECT_DOUBLE_EQ(lil.clampIndex(-std::numeric_limits<double>::infinity()),
                   0.0);

  std::unique_ptr<Geometry> empty = reader_.read("LINESTRING EMPTY");
  LengthIndexedLine empty_lil(empty.get());
  EXPECT_DOUBLE_EQ(empty_lil.getStartIndex(), 0.0);
  EXPECT_DOUBLE_EQ(empty_lil.getEndIndex(), 0.0);
  EXPECT_TRUE(empty_lil.isValidIndex(0.0));
  EXPECT_FALSE(empty_lil.isValidIndex(1.0));
  EXPECT_FALSE(empty_lil.isValidIndex(-1.0));
  EXPECT_DOUBLE_EQ(empty_lil.clampIndex(5.0), 0.0);
  EXPECT_DOUBLE_EQ(empty_lil.clampIndex(-5.0), 0.0);
}

TEST_F(LengthIndexedLineTest, ExtractPointAndZInterpolation) {
  std::unique_ptr<Geometry> line =
      reader_.read("LINESTRING (0 0, 10 0, 10 10)");
  LengthIndexedLine lil(line.get());

  EXPECT_EQ(lil.extractPoint(0.0), Coordinate(0, 0));
  EXPECT_EQ(lil.extractPoint(4.0), Coordinate(4, 0));
  EXPECT_EQ(lil.extractPoint(10.0), Coordinate(10, 0));
  EXPECT_EQ(lil.extractPoint(15.0), Coordinate(10, 5));
  EXPECT_EQ(lil.extractPoint(20.0), Coordinate(10, 10));
  EXPECT_EQ(lil.extractPoint(-3.0), Coordinate(10, 7));
  EXPECT_EQ(lil.extractPoint(100.0), Coordinate(10, 10));
  EXPECT_EQ(lil.extractPoint(-100.0), Coordinate(0, 0));

  std::unique_ptr<Geometry> line3d =
      reader_.read("LINESTRING Z (0 0 0, 10 0 10, 10 10 30)");
  LengthIndexedLine lil3d(line3d.get());
  Coordinate p1 = lil3d.extractPoint(5.0);
  EXPECT_DOUBLE_EQ(p1.x, 5.0);
  EXPECT_DOUBLE_EQ(p1.y, 0.0);
  EXPECT_DOUBLE_EQ(p1.z, 5.0);
  Coordinate p2 = lil3d.extractPoint(15.0);
  EXPECT_DOUBLE_EQ(p2.x, 10.0);
  EXPECT_DOUBLE_EQ(p2.y, 5.0);
  EXPECT_DOUBLE_EQ(p2.z, 20.0);

  std::unique_ptr<Geometry> empty = reader_.read("LINESTRING EMPTY");
  LengthIndexedLine empty_lil(empty.get());
  EXPECT_TRUE(empty_lil.extractPoint(0.0).isNull());
}

TEST_F(LengthIndexedLineTest, ExtractPointWithOffset) {
  std::unique_ptr<Geometry> line =
      reader_.read("LINESTRING (0 0, 10 0, 10 10)");
  LengthIndexedLine lil(line.get());

  // Zero offset matches extractPoint(index).
  EXPECT_EQ(lil.extractPoint(5.0, 0.0), Coordinate(5, 0));
  // Positive offset is left of segment direction (+Y for +X segment).
  EXPECT_EQ(lil.extractPoint(5.0, 2.0), Coordinate(5, 2));
  // Negative offset is right of segment direction (-Y for +X segment).
  EXPECT_EQ(lil.extractPoint(5.0, -3.0), Coordinate(5, -3));

  // Second segment runs +Y, so left is -X and right is +X.
  EXPECT_EQ(lil.extractPoint(15.0, 2.0), Coordinate(8, 5));
  EXPECT_EQ(lil.extractPoint(15.0, -2.0), Coordinate(12, 5));

  // Negative index and clamped past-end index with offsets.
  EXPECT_EQ(lil.extractPoint(-2.0, 1.0), Coordinate(9, 8));
  EXPECT_EQ(lil.extractPoint(50.0, 2.0), Coordinate(8, 10));

  // Diagonal 3-4-5 segment: left unit normal is (-0.8, 0.6).
  std::unique_ptr<Geometry> diag = reader_.read("LINESTRING (0 0, 3 4)");
  LengthIndexedLine diag_lil(diag.get());
  Coordinate diag_offset = diag_lil.extractPoint(2.5, 5.0);
  EXPECT_NEAR(diag_offset.x, -2.5, 1e-12);
  EXPECT_NEAR(diag_offset.y, 5.0, 1e-12);

  // Zero-length segment rejects non-zero offset.
  std::unique_ptr<Geometry> zero_len = reader_.read("LINESTRING (1 1, 1 1)");
  LengthIndexedLine zero_lil(zero_len.get());
  EXPECT_THROW(zero_lil.extractPoint(0.0, 1.0), IllegalStateException);
}

TEST_F(LengthIndexedLineTest, ExtractLineForwardReverseAndClamped) {
  std::unique_ptr<Geometry> line =
      reader_.read("LINESTRING (0 0, 10 0, 10 10, 20 10)");
  LengthIndexedLine lil(line.get());

  ExpectWktEqual(*lil.extractLine(5.0, 25.0),
                 "LINESTRING (5 0, 10 0, 10 10, 15 10)");
  ExpectWktEqual(*lil.extractLine(-15.0, -5.0),
                 "LINESTRING (10 5, 10 10, 15 10)");
  ExpectWktEqual(*lil.extractLine(25.0, 5.0),
                 "LINESTRING (15 10, 10 10, 10 0, 5 0)");
  ExpectWktEqual(*lil.extractLine(-5.0, 5.0),
                 "LINESTRING (15 10, 10 10, 10 0, 5 0)");
  ExpectWktEqual(*lil.extractLine(-100.0, 100.0),
                 "LINESTRING (0 0, 10 0, 10 10, 20 10)");
  ExpectWktEqual(*lil.extractLine(-100.0, -50.0), "LINESTRING (0 0, 0 0)");
  ExpectWktEqual(*lil.extractLine(15.0, 15.0), "LINESTRING (10 5, 10 5)");
}

TEST_F(LengthIndexedLineTest, ExtractLineMultiLineStringComponentBoundary) {
  std::unique_ptr<Geometry> mls =
      reader_.read("MULTILINESTRING ((0 0, 10 0), (20 0, 30 0))");
  LengthIndexedLine lil(mls.get());

  // Zero-length extraction at component boundary resolves start lower so
  // start and end stay on component 0 endpoint (10, 0).
  ExpectWktEqual(*lil.extractLine(10.0, 10.0), "LINESTRING (10 0, 10 0)");

  // Positive-length extraction starting at component boundary resolves start
  // higher to component 1 start (20, 0).
  ExpectWktEqual(*lil.extractLine(10.0, 15.0), "LINESTRING (20 0, 25 0)");

  // Spanning across components forward and reversed. Note that
  // MultiLineString::reverseImpl reverses each component in-place without
  // reversing component order.
  ExpectWktEqual(*lil.extractLine(5.0, 15.0),
                 "MULTILINESTRING ((5 0, 10 0), (20 0, 25 0))");
  ExpectWktEqual(*lil.extractLine(15.0, 5.0),
                 "MULTILINESTRING ((10 0, 5 0), (25 0, 20 0))");

  // Skipping an intermediate zero-length component when resolving start higher.
  std::unique_ptr<Geometry> mls_zero =
      reader_.read("MULTILINESTRING ((0 0, 10 0), (15 0, 15 0), (20 0, 30 0))");
  LengthIndexedLine lil_zero(mls_zero.get());
  ExpectWktEqual(*lil_zero.extractLine(10.0, 10.0), "LINESTRING (10 0, 10 0)");
  ExpectWktEqual(*lil_zero.extractLine(10.0, 15.0), "LINESTRING (20 0, 25 0)");
}

TEST_F(LengthIndexedLineTest, ExtractLineThrowsOnNaNAndNonLinear) {
  std::unique_ptr<Geometry> line = reader_.read("LINESTRING (0 0, 10 0)");
  LengthIndexedLine lil(line.get());

  const double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_THROW(lil.extractLine(nan, 5.0), IllegalArgumentException);
  EXPECT_THROW(lil.extractLine(0.0, nan), IllegalArgumentException);

  std::unique_ptr<Geometry> pt = reader_.read("POINT (0 0)");
  LengthIndexedLine pt_lil(pt.get());
  EXPECT_THROW(pt_lil.extractLine(0.0, 0.0), IllegalArgumentException);

  std::unique_ptr<Geometry> poly =
      reader_.read("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  LengthIndexedLine poly_lil(poly.get());
  EXPECT_THROW(poly_lil.extractLine(0.0, 1.0), IllegalArgumentException);
}

TEST_F(LengthIndexedLineTest, IndexOfIndexOfAfterAndProject) {
  std::unique_ptr<Geometry> loop = reader_.read("LINESTRING (0 0, 10 0, 0 0)");
  LengthIndexedLine loop_lil(loop.get());

  EXPECT_DOUBLE_EQ(loop_lil.indexOf(Coordinate(4, 0)), 4.0);
  EXPECT_DOUBLE_EQ(loop_lil.indexOfAfter(Coordinate(4, 0), 4.0), 16.0);
  EXPECT_DOUBLE_EQ(loop_lil.indexOfAfter(Coordinate(4, 0), -1.0), 4.0);
  EXPECT_DOUBLE_EQ(loop_lil.indexOfAfter(Coordinate(4, 0), 25.0), 20.0);
  EXPECT_THROW(loop_lil.indexOfAfter(Coordinate(4, 0), 20.0),
               IllegalArgumentException);

  std::unique_ptr<Geometry> line =
      reader_.read("LINESTRING (0 0, 10 0, 10 10)");
  LengthIndexedLine lil(line.get());
  EXPECT_DOUBLE_EQ(lil.project(Coordinate(100, 3)), 13.0);
  EXPECT_DOUBLE_EQ(lil.project(Coordinate(-50, -50)), 0.0);
  EXPECT_DOUBLE_EQ(lil.project(Coordinate(10, 100)), 20.0);
}

TEST_F(LengthIndexedLineTest, IndicesOfSublines) {
  std::unique_ptr<Geometry> line =
      reader_.read("LINESTRING (0 0, 10 0, 10 10, 20 10)");
  LengthIndexedLine lil(line.get());

  std::unique_ptr<Geometry> sub = lil.extractLine(5.0, 25.0);
  std::unique_ptr<double[]> roundtrip(lil.indicesOf(sub.get()));
  EXPECT_DOUBLE_EQ(roundtrip[0], 5.0);
  EXPECT_DOUBLE_EQ(roundtrip[1], 25.0);

  std::unique_ptr<Geometry> zero_sub = reader_.read("LINESTRING (10 5, 10 5)");
  std::unique_ptr<double[]> zero_indices(lil.indicesOf(zero_sub.get()));
  EXPECT_DOUBLE_EQ(zero_indices[0], 15.0);
  EXPECT_DOUBLE_EQ(zero_indices[1], 15.0);

  std::unique_ptr<Geometry> mls =
      reader_.read("MULTILINESTRING ((0 0, 10 0), (20 0, 30 0))");
  LengthIndexedLine mls_lil(mls.get());
  std::unique_ptr<Geometry> mls_sub =
      reader_.read("MULTILINESTRING ((5 0, 10 0), (20 0, 25 0))");
  std::unique_ptr<double[]> mls_indices(mls_lil.indicesOf(mls_sub.get()));
  EXPECT_DOUBLE_EQ(mls_indices[0], 5.0);
  EXPECT_DOUBLE_EQ(mls_indices[1], 15.0);

  std::unique_ptr<Geometry> pt = reader_.read("POINT (5 0)");
  std::unique_ptr<double[]> invalid_indices;
  EXPECT_THROW(invalid_indices.reset(lil.indicesOf(pt.get())),
               IllegalArgumentException);
}

}  // namespace
}  // namespace geos_google
