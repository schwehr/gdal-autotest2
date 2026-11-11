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

#include "third_party/geos/include/geos/linearref/LocationIndexOfLine.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/linearref/LinearLocation.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::io::WKTReader;
using geos::linearref::LinearLocation;
using geos::linearref::LocationIndexOfLine;
using geos::util::IllegalArgumentException;

class LocationIndexOfLineTest : public ::testing::Test {
 protected:
  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader reader_{*factory_};

  std::unique_ptr<Geometry> ReadWKT(const std::string& wkt) const {
    return reader_.read(wkt);
  }
};

TEST_F(LocationIndexOfLineTest, StaticIndicesOfSimpleSubLine) {
  auto line = ReadWKT("LINESTRING (0 0, 10 0, 10 10)");
  auto sub = ReadWKT("LINESTRING (5 0, 10 0, 10 5)");

  std::unique_ptr<LinearLocation[]> locs(
      LocationIndexOfLine::indicesOf(line.get(), sub.get()));
  ASSERT_NE(locs, nullptr);

  EXPECT_EQ(locs[0].compareLocationValues(0, 0, 0.5), 0);
  EXPECT_EQ(locs[1].compareLocationValues(0, 1, 0.5), 0);
  EXPECT_TRUE(locs[0].getCoordinate(line.get()).equals2D(Coordinate(5.0, 0.0)));
  EXPECT_TRUE(
      locs[1].getCoordinate(line.get()).equals2D(Coordinate(10.0, 5.0)));
}

TEST_F(LocationIndexOfLineTest, InstanceIndicesOfFullAndOffsetLines) {
  auto line = ReadWKT("LINESTRING (0 0, 10 0, 10 10)");
  LocationIndexOfLine locater(line.get());

  // Subline matching the entire line.
  {
    auto sub = ReadWKT("LINESTRING (0 0, 10 0, 10 10)");
    std::unique_ptr<LinearLocation[]> locs(locater.indicesOf(sub.get()));
    ASSERT_NE(locs, nullptr);

    EXPECT_EQ(locs[0].compareLocationValues(0, 0, 0.0), 0);
    EXPECT_EQ(locs[1].compareLocationValues(0, 2, 0.0), 0);
    EXPECT_TRUE(
        locs[0].getCoordinate(line.get()).equals2D(Coordinate(0.0, 0.0)));
    EXPECT_TRUE(
        locs[1].getCoordinate(line.get()).equals2D(Coordinate(10.0, 10.0)));
  }

  // Subline with endpoints slightly offset from the reference line.
  {
    auto sub = ReadWKT("LINESTRING (2 1, 8 -1)");
    std::unique_ptr<LinearLocation[]> locs(locater.indicesOf(sub.get()));
    ASSERT_NE(locs, nullptr);

    EXPECT_EQ(locs[0].compareLocationValues(0, 0, 0.2), 0);
    EXPECT_EQ(locs[1].compareLocationValues(0, 0, 0.8), 0);
  }
}

TEST_F(LocationIndexOfLineTest, ZeroLengthSubLine) {
  auto line = ReadWKT("LINESTRING (0 0, 10 0, 20 0)");

  // Zero-length subline on segment interior.
  {
    auto zero_sub = ReadWKT("LINESTRING (5 0, 5 0)");
    std::unique_ptr<LinearLocation[]> locs(
        LocationIndexOfLine::indicesOf(line.get(), zero_sub.get()));
    ASSERT_NE(locs, nullptr);

    EXPECT_EQ(locs[0].compareLocationValues(0, 0, 0.5), 0);
    EXPECT_EQ(locs[1].compareTo(locs[0]), 0);
  }

  // Zero-length subline at an interior vertex.
  {
    auto zero_vertex = ReadWKT("LINESTRING (10 0, 10 0)");
    std::unique_ptr<LinearLocation[]> locs(
        LocationIndexOfLine::indicesOf(line.get(), zero_vertex.get()));
    ASSERT_NE(locs, nullptr);

    EXPECT_EQ(locs[0].compareLocationValues(0, 1, 0.0), 0);
    EXPECT_EQ(locs[1].compareTo(locs[0]), 0);
  }
}

TEST_F(LocationIndexOfLineTest, ClosedRingSubLines) {
  auto ring_line = ReadWKT("LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  auto full_ring_sub = ReadWKT("LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)");

  std::unique_ptr<LinearLocation[]> full_locs(
      LocationIndexOfLine::indicesOf(ring_line.get(), full_ring_sub.get()));
  ASSERT_NE(full_locs, nullptr);
  EXPECT_EQ(full_locs[0].compareLocationValues(0, 0, 0.0), 0);
  EXPECT_EQ(full_locs[1].compareLocationValues(0, 4, 0.0), 0);

  auto linear_ring = ReadWKT("LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  auto tail_sub = ReadWKT("LINESTRING (0 5, 0 0)");
  std::unique_ptr<LinearLocation[]> tail_locs(
      LocationIndexOfLine::indicesOf(linear_ring.get(), tail_sub.get()));
  ASSERT_NE(tail_locs, nullptr);
  EXPECT_EQ(tail_locs[0].compareLocationValues(0, 3, 0.5), 0);
  EXPECT_EQ(tail_locs[1].compareLocationValues(0, 4, 0.0), 0);
}

TEST_F(LocationIndexOfLineTest, MultiLineStringAndCollectionSubLines) {
  auto mls = ReadWKT("MULTILINESTRING ((0 0, 10 0), (20 0, 30 0, 40 0))");

  // Single LineString subline on second component.
  {
    auto sub = ReadWKT("LINESTRING (25 0, 35 0)");
    std::unique_ptr<LinearLocation[]> locs(
        LocationIndexOfLine::indicesOf(mls.get(), sub.get()));
    ASSERT_NE(locs, nullptr);

    EXPECT_EQ(locs[0].compareLocationValues(1, 0, 0.5), 0);
    EXPECT_EQ(locs[1].compareLocationValues(1, 1, 0.5), 0);
  }

  // MultiLineString subline spanning both components.
  {
    auto sub_mls = ReadWKT("MULTILINESTRING ((5 0, 10 0), (20 0, 35 0))");
    std::unique_ptr<LinearLocation[]> locs(
        LocationIndexOfLine::indicesOf(mls.get(), sub_mls.get()));
    ASSERT_NE(locs, nullptr);

    EXPECT_EQ(locs[0].compareLocationValues(0, 0, 0.5), 0);
    EXPECT_EQ(locs[1].compareLocationValues(1, 1, 0.5), 0);
  }

  // GeometryCollection containing multiple LineStrings.
  {
    std::vector<std::unique_ptr<Geometry>> parts;
    parts.push_back(ReadWKT("LINESTRING (2 0, 10 0)"));
    parts.push_back(ReadWKT("LINESTRING (20 0, 28 0)"));
    auto sub_gc = factory_->createGeometryCollection(std::move(parts));

    std::unique_ptr<LinearLocation[]> locs(
        LocationIndexOfLine::indicesOf(mls.get(), sub_gc.get()));
    ASSERT_NE(locs, nullptr);

    EXPECT_EQ(locs[0].compareLocationValues(0, 0, 0.2), 0);
    EXPECT_EQ(locs[1].compareLocationValues(1, 0, 0.8), 0);
  }
}

TEST_F(LocationIndexOfLineTest, NonLineStringSubLineThrows) {
  auto line = ReadWKT("LINESTRING (0 0, 10 0)");

  auto pt = ReadWKT("POINT (5 0)");
  EXPECT_THROW(LocationIndexOfLine::indicesOf(line.get(), pt.get()),
               IllegalArgumentException);

  auto poly = ReadWKT("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  EXPECT_THROW(LocationIndexOfLine::indicesOf(line.get(), poly.get()),
               IllegalArgumentException);

  // First component non-line, last component line.
  {
    std::vector<std::unique_ptr<Geometry>> parts;
    parts.push_back(ReadWKT("POINT (0 0)"));
    parts.push_back(ReadWKT("LINESTRING (0 0, 10 0)"));
    auto gc = factory_->createGeometryCollection(std::move(parts));
    EXPECT_THROW(LocationIndexOfLine::indicesOf(line.get(), gc.get()),
                 IllegalArgumentException);
  }

  // First component line, last component non-line.
  {
    std::vector<std::unique_ptr<Geometry>> parts;
    parts.push_back(ReadWKT("LINESTRING (0 0, 5 0)"));
    parts.push_back(ReadWKT("POINT (5 0)"));
    auto gc = factory_->createGeometryCollection(std::move(parts));
    EXPECT_THROW(LocationIndexOfLine::indicesOf(line.get(), gc.get()),
                 IllegalArgumentException);
  }
}

}  // namespace
}  // namespace geos_google
