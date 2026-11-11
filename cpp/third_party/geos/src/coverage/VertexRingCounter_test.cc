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

// Unit tests for third_party/geos/src/coverage/VertexRingCounter.cpp and
// third_party/geos/include/geos/coverage/VertexRingCounter.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/coverage/VertexRingCounter.cpp

#include "third_party/geos/include/geos/coverage/VertexRingCounter.h"

#include <cstddef>
#include <map>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::coverage::VertexRingCounter;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::io::WKTReader;

TEST(VertexRingCounterTest, EmptyGeometryVectorLeavesCountsUnchanged) {
  std::vector<const Geometry*> empty_geoms;
  std::map<Coordinate, std::size_t> counts;
  VertexRingCounter::count(empty_geoms, counts);
  EXPECT_TRUE(counts.empty());

  counts[Coordinate(1.0, 2.0)] = 3;
  VertexRingCounter::count(empty_geoms, counts);
  ASSERT_EQ(counts.size(), 1u);
  EXPECT_EQ(counts.at(Coordinate(1.0, 2.0)), 3u);
}

TEST(VertexRingCounterTest, SinglePolygonDoesNotDoubleCountClosingVertex) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  auto poly = reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  std::map<Coordinate, std::size_t> counts;
  VertexRingCounter::count({poly.get()}, counts);

  ASSERT_EQ(counts.size(), 4u);
  EXPECT_EQ(counts.at(Coordinate(0.0, 0.0)), 1u);
  EXPECT_EQ(counts.at(Coordinate(10.0, 0.0)), 1u);
  EXPECT_EQ(counts.at(Coordinate(10.0, 10.0)), 1u);
  EXPECT_EQ(counts.at(Coordinate(0.0, 10.0)), 1u);
}

TEST(VertexRingCounterTest, PolygonWithDisjointAndTouchingHole) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  auto disjoint_hole_poly = reader.read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 8 2, 8 8, 2 8, 2 2))");
  std::map<Coordinate, std::size_t> disjoint_counts;
  VertexRingCounter::count({disjoint_hole_poly.get()}, disjoint_counts);
  ASSERT_EQ(disjoint_counts.size(), 8u);
  for (const auto& [coord, count] : disjoint_counts) {
    EXPECT_EQ(count, 1u) << coord;
  }

  // Hole touches shell at (5, 0), giving vertex (5, 0) a ring count of 2.
  auto touching_hole_poly = reader.read(
      "POLYGON ((0 0, 5 0, 10 0, 10 10, 0 10, 0 0), "
      "(5 0, 8 4, 2 4, 5 0))");
  std::map<Coordinate, std::size_t> touching_counts;
  VertexRingCounter::count({touching_hole_poly.get()}, touching_counts);
  ASSERT_EQ(touching_counts.size(), 7u);
  EXPECT_EQ(touching_counts.at(Coordinate(5.0, 0.0)), 2u);
  EXPECT_EQ(touching_counts.at(Coordinate(0.0, 0.0)), 1u);
  EXPECT_EQ(touching_counts.at(Coordinate(8.0, 4.0)), 1u);
}

TEST(VertexRingCounterTest, SharedVerticesAcrossMultiplePolygons) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  auto p0 = reader.read("POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0))");
  auto p1 = reader.read("POLYGON ((5 0, 10 0, 10 5, 5 5, 5 0))");
  auto p2 = reader.read("POLYGON ((0 5, 5 5, 10 5, 10 10, 0 10, 0 5))");

  std::map<Coordinate, std::size_t> counts;
  VertexRingCounter::count({p0.get(), p1.get(), p2.get()}, counts);

  // (5, 5) is shared by all three polygons; edge junctions are shared by two.
  EXPECT_EQ(counts.at(Coordinate(5.0, 5.0)), 3u);
  EXPECT_EQ(counts.at(Coordinate(5.0, 0.0)), 2u);
  EXPECT_EQ(counts.at(Coordinate(0.0, 5.0)), 2u);
  EXPECT_EQ(counts.at(Coordinate(10.0, 5.0)), 2u);
  EXPECT_EQ(counts.at(Coordinate(0.0, 0.0)), 1u);
  EXPECT_EQ(counts.at(Coordinate(10.0, 0.0)), 1u);
  EXPECT_EQ(counts.at(Coordinate(10.0, 10.0)), 1u);
  EXPECT_EQ(counts.at(Coordinate(0.0, 10.0)), 1u);
}

TEST(VertexRingCounterTest, NonRingLineStringsPointsAndMultiPolygon) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // Open LineString is not a ring: index 0 IS counted.
  auto line = reader.read("LINESTRING (0 0, 5 5, 10 0)");
  auto pt = reader.read("POINT (42 7)");
  auto empty_poly = reader.read("POLYGON EMPTY");
  auto empty_line = reader.read("LINESTRING EMPTY");
  auto mp = reader.read(
      "MULTIPOLYGON (((0 0, 5 0, 5 5, 0 5, 0 0)), "
      "((5 0, 10 0, 10 5, 5 5, 5 0)))");

  std::map<Coordinate, std::size_t> counts;
  VertexRingCounter::count(
      {line.get(), pt.get(), empty_poly.get(), empty_line.get(), mp.get()},
      counts);

  EXPECT_EQ(counts.at(Coordinate(0.0, 0.0)), 2u);   // line + mp left poly
  EXPECT_EQ(counts.at(Coordinate(5.0, 5.0)), 3u);   // line + both mp polys
  EXPECT_EQ(counts.at(Coordinate(10.0, 0.0)), 2u);  // line + mp right poly
  EXPECT_EQ(counts.at(Coordinate(5.0, 0.0)), 2u);   // both mp polys
  EXPECT_EQ(counts.at(Coordinate(42.0, 7.0)), 1u);  // point
}

TEST(VertexRingCounterTest, AccumulatesPreexistingCountsAndGroupsBy2D) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  auto poly_z = reader.read("POLYGON Z ((0 0 9, 4 0 9, 4 4 9, 0 4 9, 0 0 9))");
  std::map<Coordinate, std::size_t> counts;
  counts[Coordinate(0.0, 0.0)] = 4;

  VertexRingCounter::count({poly_z.get()}, counts);
  EXPECT_EQ(counts.at(Coordinate(0.0, 0.0)), 5u);
  EXPECT_EQ(counts.at(Coordinate(4.0, 0.0)), 1u);
}

TEST(VertexRingCounterTest, DirectFilterInterface) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  std::map<Coordinate, std::size_t> counts;
  VertexRingCounter counter(counts);
  EXPECT_FALSE(counter.isGeometryChanged());
  EXPECT_FALSE(counter.isDone());

  auto poly = reader.read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  poly->apply_ro(counter);
  EXPECT_EQ(counts.size(), 4u);
  EXPECT_EQ(counts.at(Coordinate(0.0, 0.0)), 1u);

  CoordinateSequence ring_seq = {
      Coordinate(0.0, 0.0),
      Coordinate(2.0, 0.0),
      Coordinate(2.0, 2.0),
      Coordinate(0.0, 0.0),
  };
  ASSERT_TRUE(ring_seq.isRing());
  counter.filter_ro(ring_seq, 0);
  EXPECT_EQ(counts.at(Coordinate(0.0, 0.0)), 1u);
  counter.filter_ro(ring_seq, 1);
  EXPECT_EQ(counts.at(Coordinate(2.0, 0.0)), 2u);
}

}  // namespace
}  // namespace geos_google
