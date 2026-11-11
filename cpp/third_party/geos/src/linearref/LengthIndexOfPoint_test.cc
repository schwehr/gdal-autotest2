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

// Unit tests for third_party/geos/src/linearref/LengthIndexOfPoint.cpp and
// third_party/geos/include/geos/linearref/LengthIndexOfPoint.h.

#include "third_party/geos/include/geos/linearref/LengthIndexOfPoint.h"

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::io::WKTReader;
using geos::linearref::LengthIndexOfPoint;
using geos::util::IllegalArgumentException;

class LengthIndexOfPointTest : public ::testing::Test {
 protected:
  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader reader_{*factory_};
};

TEST_F(LengthIndexOfPointTest, StaticAndInstanceIndexOfOnLineString) {
  std::unique_ptr<Geometry> line =
      reader_.read("LINESTRING (0 0, 10 0, 10 10, 20 10)");
  LengthIndexOfPoint locator(line.get());

  // Exact start point (projFactor == 0.0).
  EXPECT_DOUBLE_EQ(LengthIndexOfPoint::indexOf(line.get(), Coordinate(0, 0)),
                   0.0);
  EXPECT_DOUBLE_EQ(locator.indexOf(Coordinate(0, 0)), 0.0);

  // Point before start of first segment (projFactor < 0.0).
  EXPECT_DOUBLE_EQ(locator.indexOf(Coordinate(-5, 3)), 0.0);

  // Point perpendicular to start vertex (projFactor == 0.0).
  EXPECT_DOUBLE_EQ(locator.indexOf(Coordinate(0, -4)), 0.0);

  // Interior projection on first segment (0.0 < projFactor < 1.0).
  EXPECT_DOUBLE_EQ(locator.indexOf(Coordinate(4, 3)), 4.0);

  // Exact vertex at index 10.
  EXPECT_DOUBLE_EQ(locator.indexOf(Coordinate(10, 0)), 10.0);

  // Interior projection on second segment.
  EXPECT_DOUBLE_EQ(locator.indexOf(Coordinate(12, 6)), 16.0);

  // Beyond end of last segment (projFactor > 1.0).
  EXPECT_DOUBLE_EQ(locator.indexOf(Coordinate(25, 10)), 30.0);
  EXPECT_DOUBLE_EQ(LengthIndexOfPoint::indexOf(line.get(), Coordinate(25, 13)),
                   30.0);
}

TEST_F(LengthIndexOfPointTest, DiagonalSegmentProjection) {
  std::unique_ptr<Geometry> line = reader_.read("LINESTRING (0 0, 3 4)");
  // Projection of (3, 0) onto (0,0)->(3,4): dot=9, len2=25, factor=9/25,
  // measure = (9/25) * 5 = 1.8.
  EXPECT_NEAR(LengthIndexOfPoint::indexOf(line.get(), Coordinate(3, 0)), 1.8,
              1e-12);
}

TEST_F(LengthIndexOfPointTest, TieBreakingSelectsEarliestAndCloserLaterWins) {
  // C-shaped line of length 30: (0,0)->(10,0)->(10,10)->(0,10).
  std::unique_ptr<Geometry> line =
      reader_.read("LINESTRING (0 0, 10 0, 10 10, 0 10)");

  // Point (5, 5) is equidistant (distance 5) from all three segments.
  // Strict `<` ensures the earliest minimum index is returned.
  EXPECT_DOUBLE_EQ(LengthIndexOfPoint::indexOf(line.get(), Coordinate(5, 5)),
                   5.0);

  // Point (5, 8) is closest to the third segment (distance 2) at index 25.
  EXPECT_DOUBLE_EQ(LengthIndexOfPoint::indexOf(line.get(), Coordinate(5, 8)),
                   25.0);
}

TEST_F(LengthIndexOfPointTest, MultiLineStringMeasureAcrossComponents) {
  std::unique_ptr<Geometry> mls =
      reader_.read("MULTILINESTRING ((0 0, 10 0), (20 0, 20 10))");

  EXPECT_DOUBLE_EQ(LengthIndexOfPoint::indexOf(mls.get(), Coordinate(5, 1)),
                   5.0);
  // (14, 0) is distance 4 from (10, 0) and distance 6 from (20, 0).
  EXPECT_DOUBLE_EQ(LengthIndexOfPoint::indexOf(mls.get(), Coordinate(14, 0)),
                   10.0);
  // (17, 0) is distance 3 from (20, 0), which is start of component 1 at 10.0.
  EXPECT_DOUBLE_EQ(LengthIndexOfPoint::indexOf(mls.get(), Coordinate(17, 0)),
                   10.0);
  EXPECT_DOUBLE_EQ(LengthIndexOfPoint::indexOf(mls.get(), Coordinate(20, 7)),
                   17.0);
  EXPECT_DOUBLE_EQ(LengthIndexOfPoint::indexOf(mls.get(), Coordinate(20, 15)),
                   20.0);
}

TEST_F(LengthIndexOfPointTest, IndexOfAfterHandlesLoopsClampingAndErrors) {
  // Backtracking line of total length 20.
  std::unique_ptr<Geometry> loop = reader_.read("LINESTRING (0 0, 10 0, 0 0)");
  LengthIndexOfPoint locator(loop.get());

  EXPECT_DOUBLE_EQ(locator.indexOf(Coordinate(3, 0)), 3.0);

  // Strict minIndex skips the first segment match (3.0) and finds 17.0.
  EXPECT_DOUBLE_EQ(
      LengthIndexOfPoint::indexOfAfter(loop.get(), Coordinate(3, 0), 3.0),
      17.0);
  EXPECT_DOUBLE_EQ(locator.indexOfAfter(Coordinate(3, 0), 3.0), 17.0);

  // Negative minIndex falls back to indexOf.
  EXPECT_DOUBLE_EQ(locator.indexOfAfter(Coordinate(3, 0), -5.0), 3.0);

  // minIndex strictly greater than line length returns endIndex.
  EXPECT_DOUBLE_EQ(locator.indexOfAfter(Coordinate(3, 0), 25.0), 20.0);

  // minIndex equal to endIndex leaves no index strictly greater -> throws.
  EXPECT_THROW(locator.indexOfAfter(Coordinate(3, 0), 20.0),
               IllegalArgumentException);

  // On a single segment, if nearest measure <= minIndex, throws.
  std::unique_ptr<Geometry> single = reader_.read("LINESTRING (0 0, 10 0)");
  EXPECT_THROW(
      LengthIndexOfPoint::indexOfAfter(single.get(), Coordinate(2, 3), 5.0),
      IllegalArgumentException);

  // On a multi-segment line, seg 0 measure (2.0 <= 5.0) is skipped and seg 1
  // endpoint (10.0 > 5.0) is accepted.
  std::unique_ptr<Geometry> two_seg =
      reader_.read("LINESTRING (0 0, 10 0, 20 0)");
  EXPECT_DOUBLE_EQ(
      LengthIndexOfPoint::indexOfAfter(two_seg.get(), Coordinate(2, 3), 5.0),
      10.0);
}

TEST_F(LengthIndexOfPointTest, EmptyZeroLengthAndNonLinearGeometries) {
  std::unique_ptr<Geometry> empty_line = reader_.read("LINESTRING EMPTY");
  EXPECT_DOUBLE_EQ(
      LengthIndexOfPoint::indexOf(empty_line.get(), Coordinate(0, 0)), -1.0);
  EXPECT_DOUBLE_EQ(LengthIndexOfPoint::indexOfAfter(empty_line.get(),
                                                    Coordinate(0, 0), -1.0),
                   -1.0);
  EXPECT_DOUBLE_EQ(
      LengthIndexOfPoint::indexOfAfter(empty_line.get(), Coordinate(0, 0), 1.0),
      0.0);
  EXPECT_THROW(
      LengthIndexOfPoint::indexOfAfter(empty_line.get(), Coordinate(0, 0), 0.0),
      IllegalArgumentException);

  std::unique_ptr<Geometry> empty_mls = reader_.read("MULTILINESTRING EMPTY");
  EXPECT_DOUBLE_EQ(
      LengthIndexOfPoint::indexOf(empty_mls.get(), Coordinate(1, 2)), -1.0);

  std::unique_ptr<Geometry> zero_len = reader_.read("LINESTRING (5 5, 5 5)");
  EXPECT_DOUBLE_EQ(
      LengthIndexOfPoint::indexOf(zero_len.get(), Coordinate(8, 9)), 0.0);
  EXPECT_THROW(
      LengthIndexOfPoint::indexOfAfter(zero_len.get(), Coordinate(8, 9), 0.0),
      IllegalArgumentException);

  std::unique_ptr<Geometry> pt = reader_.read("POINT (1 2)");
  EXPECT_THROW(LengthIndexOfPoint::indexOf(pt.get(), Coordinate(1, 2)),
               IllegalArgumentException);

  std::unique_ptr<Geometry> poly =
      reader_.read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  EXPECT_THROW(LengthIndexOfPoint::indexOf(poly.get(), Coordinate(1, 1)),
               IllegalArgumentException);
}

}  // namespace
}  // namespace geos_google
