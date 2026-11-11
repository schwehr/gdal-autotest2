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

#include "third_party/geos/include/geos/linearref/LinearIterator.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/linearref/LinearLocation.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::io::WKTReader;
using geos::linearref::LinearIterator;
using geos::linearref::LinearLocation;
using geos::util::IllegalArgumentException;

void ExpectNullCoordinate(const Coordinate& coord) {
  EXPECT_TRUE(coord.isNull());
  EXPECT_TRUE(std::isnan(coord.x));
  EXPECT_TRUE(std::isnan(coord.y));
  EXPECT_TRUE(std::isnan(coord.z));
}

class LinearIteratorTest : public ::testing::Test {
 protected:
  LinearIteratorTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) {
    return reader_.read(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(LinearIteratorTest, IteratesSingleLineStringSegmentsAndEndpoint) {
  auto line = ReadWkt("LINESTRING (0 0, 10 0, 10 20)");
  LinearIterator it(line.get());

  // Vertex 0 -> segment (0,0)-(10,0).
  ASSERT_TRUE(it.hasNext());
  EXPECT_FALSE(it.isEndOfLine());
  EXPECT_EQ(it.getComponentIndex(), 0u);
  EXPECT_EQ(it.getVertexIndex(), 0u);
  EXPECT_EQ(it.getLine(), line.get());
  EXPECT_TRUE(it.getSegmentStart().equals2D(Coordinate(0.0, 0.0)));
  EXPECT_TRUE(it.getSegmentEnd().equals2D(Coordinate(10.0, 0.0)));

  // Vertex 1 -> segment (10,0)-(10,20).
  it.next();
  ASSERT_TRUE(it.hasNext());
  EXPECT_FALSE(it.isEndOfLine());
  EXPECT_EQ(it.getComponentIndex(), 0u);
  EXPECT_EQ(it.getVertexIndex(), 1u);
  EXPECT_EQ(it.getLine(), line.get());
  EXPECT_TRUE(it.getSegmentStart().equals2D(Coordinate(10.0, 0.0)));
  EXPECT_TRUE(it.getSegmentEnd().equals2D(Coordinate(10.0, 20.0)));

  // Vertex 2 -> end of line, getSegmentEnd returns null Coordinate.
  it.next();
  ASSERT_TRUE(it.hasNext());
  EXPECT_TRUE(it.isEndOfLine());
  EXPECT_EQ(it.getComponentIndex(), 0u);
  EXPECT_EQ(it.getVertexIndex(), 2u);
  EXPECT_EQ(it.getLine(), line.get());
  EXPECT_TRUE(it.getSegmentStart().equals2D(Coordinate(10.0, 20.0)));
  ExpectNullCoordinate(it.getSegmentEnd());

  // Exhausted state.
  it.next();
  EXPECT_FALSE(it.hasNext());
  EXPECT_FALSE(it.isEndOfLine());
  EXPECT_EQ(it.getComponentIndex(), 1u);
  EXPECT_EQ(it.getVertexIndex(), 0u);
  EXPECT_EQ(it.getLine(), nullptr);

  // Calling next() past end is a safe no-op.
  it.next();
  EXPECT_FALSE(it.hasNext());
  EXPECT_EQ(it.getComponentIndex(), 1u);
  EXPECT_EQ(it.getVertexIndex(), 0u);
}

TEST_F(LinearIteratorTest, IteratesMultiLineStringAcrossComponents) {
  auto mls = ReadWkt("MULTILINESTRING ((0 0, 1 1), (2 2, 3 3, 4 4))");
  LinearIterator it(mls.get());

  struct Step {
    std::size_t comp;
    std::size_t vert;
    bool end_of_line;
    Coordinate start;
  };
  const Step expected[] = {
      {0, 0, false, Coordinate(0.0, 0.0)}, {0, 1, true, Coordinate(1.0, 1.0)},
      {1, 0, false, Coordinate(2.0, 2.0)}, {1, 1, false, Coordinate(3.0, 3.0)},
      {1, 2, true, Coordinate(4.0, 4.0)},
  };

  std::size_t idx = 0;
  for (; it.hasNext(); it.next(), ++idx) {
    ASSERT_LT(idx, 5u);
    EXPECT_EQ(it.getComponentIndex(), expected[idx].comp);
    EXPECT_EQ(it.getVertexIndex(), expected[idx].vert);
    EXPECT_EQ(it.isEndOfLine(), expected[idx].end_of_line);
    EXPECT_EQ(it.getLine(), mls->getGeometryN(expected[idx].comp));
    EXPECT_TRUE(it.getSegmentStart().equals2D(expected[idx].start));
    if (expected[idx].end_of_line) {
      ExpectNullCoordinate(it.getSegmentEnd());
    } else {
      EXPECT_FALSE(it.getSegmentEnd().isNull());
    }
  }
  EXPECT_EQ(idx, 5u);
  EXPECT_FALSE(it.hasNext());
  EXPECT_FALSE(it.isEndOfLine());
  EXPECT_EQ(it.getLine(), nullptr);
}

TEST_F(LinearIteratorTest, ConstructWithLinearLocation) {
  auto mls = ReadWkt("MULTILINESTRING ((0 0, 5 0, 10 0), (20 0, 30 0))");

  // Fraction == 0.0 starts at segmentIndex.
  {
    LinearIterator it(mls.get(), LinearLocation(0, 1, 0.0));
    ASSERT_TRUE(it.hasNext());
    EXPECT_FALSE(it.isEndOfLine());
    EXPECT_EQ(it.getComponentIndex(), 0u);
    EXPECT_EQ(it.getVertexIndex(), 1u);
    EXPECT_TRUE(it.getSegmentStart().equals2D(Coordinate(5.0, 0.0)));
    EXPECT_TRUE(it.getSegmentEnd().equals2D(Coordinate(10.0, 0.0)));
  }

  // Fraction > 0.0 starts at segmentIndex + 1.
  {
    LinearIterator it(mls.get(), LinearLocation(0, 0, 0.25));
    ASSERT_TRUE(it.hasNext());
    EXPECT_EQ(it.getComponentIndex(), 0u);
    EXPECT_EQ(it.getVertexIndex(), 1u);
    EXPECT_TRUE(it.getSegmentStart().equals2D(Coordinate(5.0, 0.0)));
  }

  // Fraction > 0.0 in second component advances to its end vertex.
  {
    LinearIterator it(mls.get(), LinearLocation(1, 0, 0.75));
    ASSERT_TRUE(it.hasNext());
    EXPECT_TRUE(it.isEndOfLine());
    EXPECT_EQ(it.getComponentIndex(), 1u);
    EXPECT_EQ(it.getVertexIndex(), 1u);
    EXPECT_TRUE(it.getSegmentStart().equals2D(Coordinate(30.0, 0.0)));
    ExpectNullCoordinate(it.getSegmentEnd());
  }

  // Location past last vertex of last component has no remaining vertices.
  {
    LinearIterator it(mls.get(), LinearLocation(1, 1, 0.5));
    EXPECT_FALSE(it.hasNext());
    EXPECT_EQ(it.getComponentIndex(), 1u);
    EXPECT_EQ(it.getVertexIndex(), 2u);
  }
}

TEST_F(LinearIteratorTest, ConstructWithComponentAndVertexIndices) {
  auto mls = ReadWkt("MULTILINESTRING ((0 0, 5 0, 10 0), (20 0, 30 0))");

  {
    LinearIterator it(mls.get(), 1, 0);
    ASSERT_TRUE(it.hasNext());
    EXPECT_FALSE(it.isEndOfLine());
    EXPECT_EQ(it.getComponentIndex(), 1u);
    EXPECT_EQ(it.getVertexIndex(), 0u);
    EXPECT_EQ(it.getLine(), mls->getGeometryN(1));
    EXPECT_TRUE(it.getSegmentStart().equals2D(Coordinate(20.0, 0.0)));
    EXPECT_TRUE(it.getSegmentEnd().equals2D(Coordinate(30.0, 0.0)));
  }

  // Vertex index >= numPoints on last component.
  {
    LinearIterator it(mls.get(), 1, 2);
    EXPECT_FALSE(it.hasNext());
    EXPECT_EQ(it.getLine(), mls->getGeometryN(1));
    it.next();
    EXPECT_FALSE(it.hasNext());
  }

  // Component index >= numLines sets currentLine to nullptr.
  {
    LinearIterator it(mls.get(), 2, 0);
    EXPECT_FALSE(it.hasNext());
    EXPECT_FALSE(it.isEndOfLine());
    EXPECT_EQ(it.getComponentIndex(), 2u);
    EXPECT_EQ(it.getVertexIndex(), 0u);
    EXPECT_EQ(it.getLine(), nullptr);
  }
}

TEST_F(LinearIteratorTest, EmptyGeometriesAndLinearRing) {
  {
    auto empty_line = ReadWkt("LINESTRING EMPTY");
    LinearIterator it(empty_line.get());
    EXPECT_FALSE(it.hasNext());
    EXPECT_EQ(it.getComponentIndex(), 0u);
    EXPECT_EQ(it.getVertexIndex(), 0u);
    EXPECT_EQ(it.getLine(), empty_line.get());
    it.next();
    EXPECT_FALSE(it.hasNext());
  }

  {
    auto empty_mls = ReadWkt("MULTILINESTRING EMPTY");
    LinearIterator it(empty_mls.get());
    EXPECT_FALSE(it.hasNext());
    EXPECT_FALSE(it.isEndOfLine());
    EXPECT_EQ(it.getComponentIndex(), 0u);
    EXPECT_EQ(it.getVertexIndex(), 0u);
    EXPECT_EQ(it.getLine(), nullptr);
    it.next();
    EXPECT_FALSE(it.hasNext());
  }

  {
    auto ring = ReadWkt("LINEARRING (0 0, 10 0, 10 10, 0 0)");
    LinearIterator it(ring.get());
    std::size_t count = 0;
    for (; it.hasNext(); it.next()) {
      ++count;
    }
    EXPECT_EQ(count, 4u);
  }
}

TEST_F(LinearIteratorTest, NonLinealGeometryThrowsIllegalArgumentException) {
  auto point = ReadWkt("POINT (1 2)");
  EXPECT_THROW((LinearIterator{point.get()}), IllegalArgumentException);

  auto poly = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  EXPECT_THROW((LinearIterator{poly.get()}), IllegalArgumentException);

  auto mp = ReadWkt("MULTIPOINT ((0 0), (1 1))");
  EXPECT_THROW((LinearIterator{mp.get()}), IllegalArgumentException);

  auto empty_gc = ReadWkt("GEOMETRYCOLLECTION EMPTY");
  LinearIterator empty_it(empty_gc.get());
  EXPECT_FALSE(empty_it.hasNext());
  EXPECT_EQ(empty_it.getLine(), nullptr);

  // Mixed collection fails when advancing onto the non-lineal component.
  auto mixed_gc =
      ReadWkt("GEOMETRYCOLLECTION (LINESTRING (0 0, 10 0), POINT (5 5))");
  LinearIterator mixed_it(mixed_gc.get());
  ASSERT_TRUE(mixed_it.hasNext());
  EXPECT_FALSE(mixed_it.isEndOfLine());
  mixed_it.next();
  ASSERT_TRUE(mixed_it.hasNext());
  EXPECT_TRUE(mixed_it.isEndOfLine());
  EXPECT_THROW(mixed_it.next(), IllegalArgumentException);

  EXPECT_THROW((LinearIterator{mixed_gc.get(), 1, 0}),
               IllegalArgumentException);
}

}  // namespace
}  // namespace geos_google
