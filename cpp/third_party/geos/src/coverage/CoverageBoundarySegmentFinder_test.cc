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

// Unit tests for
// third_party/geos/src/coverage/CoverageBoundarySegmentFinder.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/coverage/CoverageBoundarySegmentFinder.cpp

#include "third_party/geos/include/geos/coverage/CoverageBoundarySegmentFinder.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineSegment.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::coverage::CoverageBoundarySegmentFinder;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LineSegment;
using geos::geom::Polygon;
using geos::io::WKTReader;

TEST(CoverageBoundarySegmentFinderTest,
     EmptyAndSinglePointInputsReturnEmptySet) {
  std::vector<const Geometry*> empty_geoms;
  LineSegment::UnorderedSet empty_segs =
      CoverageBoundarySegmentFinder::findBoundarySegments(empty_geoms);
  EXPECT_TRUE(empty_segs.empty());

  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  auto empty_poly = reader.read("POLYGON EMPTY");
  auto point = reader.read("POINT (1 2)");
  std::vector<const Geometry*> degenerate = {empty_poly.get(), point.get()};

  LineSegment::UnorderedSet deg_segs =
      CoverageBoundarySegmentFinder::findBoundarySegments(degenerate);
  EXPECT_TRUE(deg_segs.empty());
}

TEST(CoverageBoundarySegmentFinderTest, SinglePolygonWithHoleMarksAllSegments) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  auto poly = reader.read<Polygon>(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(2 2, 2 8, 8 8, 8 2, 2 2))");

  std::vector<const Geometry*> geoms = {poly.get()};
  LineSegment::UnorderedSet segs =
      CoverageBoundarySegmentFinder::findBoundarySegments(geoms);
  EXPECT_EQ(segs.size(), 8u);

  const CoordinateSequence* shell_seq =
      poly->getExteriorRing()->getCoordinatesRO();
  for (std::size_t i = 0; i < 4; ++i) {
    EXPECT_TRUE(
        CoverageBoundarySegmentFinder::isBoundarySegment(segs, shell_seq, i))
        << "shell segment " << i;
  }

  const CoordinateSequence* hole_seq =
      poly->getInteriorRingN(0)->getCoordinatesRO();
  for (std::size_t i = 0; i < 4; ++i) {
    EXPECT_TRUE(
        CoverageBoundarySegmentFinder::isBoundarySegment(segs, hole_seq, i))
        << "hole segment " << i;
  }

  // Reversed orientation of an existing boundary segment must still match.
  CoordinateSequence reversed_probe({
      CoordinateXY(10.0, 0.0),
      CoordinateXY(0.0, 0.0),
      CoordinateXY(10.0, 10.0),
  });
  EXPECT_TRUE(CoverageBoundarySegmentFinder::isBoundarySegment(
      segs, &reversed_probe, 0));
  EXPECT_FALSE(CoverageBoundarySegmentFinder::isBoundarySegment(
      segs, &reversed_probe, 1));
}

TEST(CoverageBoundarySegmentFinderTest, AdjacentPolygonsCancelSharedEdge) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  auto left = reader.read<Polygon>("POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0))");
  auto right = reader.read<Polygon>("POLYGON ((5 0, 10 0, 10 5, 5 5, 5 0))");

  std::vector<const Geometry*> geoms = {left.get(), right.get()};
  LineSegment::UnorderedSet segs =
      CoverageBoundarySegmentFinder::findBoundarySegments(geoms);
  // 4 + 4 segments minus 2 copies of the shared edge = 6 outer segments.
  EXPECT_EQ(segs.size(), 6u);

  const CoordinateSequence* left_seq =
      left->getExteriorRing()->getCoordinatesRO();
  EXPECT_TRUE(
      CoverageBoundarySegmentFinder::isBoundarySegment(segs, left_seq, 0));
  EXPECT_FALSE(CoverageBoundarySegmentFinder::isBoundarySegment(
      segs, left_seq, 1));  // shared (5,0)-(5,5)
  EXPECT_TRUE(
      CoverageBoundarySegmentFinder::isBoundarySegment(segs, left_seq, 2));
  EXPECT_TRUE(
      CoverageBoundarySegmentFinder::isBoundarySegment(segs, left_seq, 3));

  const CoordinateSequence* right_seq =
      right->getExteriorRing()->getCoordinatesRO();
  EXPECT_TRUE(
      CoverageBoundarySegmentFinder::isBoundarySegment(segs, right_seq, 0));
  EXPECT_TRUE(
      CoverageBoundarySegmentFinder::isBoundarySegment(segs, right_seq, 1));
  EXPECT_TRUE(
      CoverageBoundarySegmentFinder::isBoundarySegment(segs, right_seq, 2));
  EXPECT_FALSE(CoverageBoundarySegmentFinder::isBoundarySegment(
      segs, right_seq, 3));  // shared (5,5)-(5,0)
}

TEST(CoverageBoundarySegmentFinderTest, MultiPolygonGridAndUnnodedEdges) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // 2x2 grid formed by a MultiPolygon on the diagonal plus two bridge cells.
  auto diag_multi = reader.read(
      "MULTIPOLYGON (((0 0, 1 0, 1 1, 0 1, 0 0)), "
      "((1 1, 2 1, 2 2, 1 2, 1 1)))");
  auto bottom_right = reader.read("POLYGON ((1 0, 2 0, 2 1, 1 1, 1 0))");
  auto top_left = reader.read("POLYGON ((0 1, 1 1, 1 2, 0 2, 0 1))");

  std::vector<const Geometry*> grid = {
      diag_multi.get(),
      bottom_right.get(),
      top_left.get(),
  };
  LineSegment::UnorderedSet grid_segs =
      CoverageBoundarySegmentFinder::findBoundarySegments(grid);
  EXPECT_EQ(grid_segs.size(), 8u);

  // Unnoded edge (0,0)-(10,0) does not cancel against split edges (0,0)-(5,0)
  // and (5,0)-(10,0).
  auto top = reader.read("POLYGON ((0 0, 10 0, 10 5, 0 5, 0 0))");
  auto bl = reader.read("POLYGON ((0 -5, 5 -5, 5 0, 0 0, 0 -5))");
  auto br = reader.read("POLYGON ((5 -5, 10 -5, 10 0, 5 0, 5 -5))");
  std::vector<const Geometry*> unnoded = {top.get(), bl.get(), br.get()};
  LineSegment::UnorderedSet unnoded_segs =
      CoverageBoundarySegmentFinder::findBoundarySegments(unnoded);
  // Shared vertical edge (5,-5)-(5,0) cancels (2 occurrences), leaving
  // 4 + 4 + 4 - 2 = 10 segments.
  EXPECT_EQ(unnoded_segs.size(), 10u);

  CoordinateSequence probe({
      CoordinateXY(0.0, 0.0),
      CoordinateXY(10.0, 0.0),
      CoordinateXY(5.0, 0.0),
      CoordinateXY(5.0, -5.0),
  });
  EXPECT_TRUE(CoverageBoundarySegmentFinder::isBoundarySegment(unnoded_segs,
                                                               &probe, 0));
  EXPECT_TRUE(CoverageBoundarySegmentFinder::isBoundarySegment(unnoded_segs,
                                                               &probe, 1));
  EXPECT_FALSE(CoverageBoundarySegmentFinder::isBoundarySegment(unnoded_segs,
                                                                &probe, 2));
}

TEST(CoverageBoundarySegmentFinderTest,
     FilterRoMetadataAndSegmentParityToggle) {
  LineSegment::UnorderedSet segs;
  CoverageBoundarySegmentFinder finder(segs);
  EXPECT_FALSE(finder.isGeometryChanged());
  EXPECT_FALSE(finder.isDone());

  // 1-point sequence: i=0 >= size-1 returns without inserting.
  CoordinateSequence single_pt({CoordinateXY(1.0, 2.0)});
  finder.filter_ro(single_pt, 0);
  EXPECT_TRUE(segs.empty());

  // 3-point sequence: i=0 and i=1 insert segments; i=2 returns early.
  CoordinateSequence line({
      CoordinateXY(0.0, 0.0),
      CoordinateXY(3.0, 0.0),
      CoordinateXY(3.0, 4.0),
  });
  finder.filter_ro(line, 0);
  finder.filter_ro(line, 1);
  finder.filter_ro(line, 2);
  EXPECT_EQ(segs.size(), 2u);
  EXPECT_TRUE(CoverageBoundarySegmentFinder::isBoundarySegment(segs, &line, 0));
  EXPECT_TRUE(CoverageBoundarySegmentFinder::isBoundarySegment(segs, &line, 1));

  // Applying the reversed segment toggles membership off, then on again.
  CoordinateSequence rev_first(
      {CoordinateXY(3.0, 0.0), CoordinateXY(0.0, 0.0)});
  finder.filter_ro(rev_first, 0);
  EXPECT_EQ(segs.size(), 1u);
  EXPECT_FALSE(
      CoverageBoundarySegmentFinder::isBoundarySegment(segs, &line, 0));

  finder.filter_ro(line, 0);
  EXPECT_EQ(segs.size(), 2u);
  EXPECT_TRUE(CoverageBoundarySegmentFinder::isBoundarySegment(segs, &line, 0));
}

}  // namespace
}  // namespace geos_google
