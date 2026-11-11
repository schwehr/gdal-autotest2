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
// third_party/geos/src/triangulate/polygon/PolygonHoleJoiner.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/triangulate/polygon/PolygonHoleJoiner.cpp

#include "third_party/geos/include/geos/triangulate/polygon/PolygonHoleJoiner.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/Area.h"
#include "third_party/geos/include/geos/algorithm/Orientation.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalStateException.h"

namespace geos_google {
namespace {

using geos::algorithm::Area;
using geos::algorithm::Orientation;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::triangulate::polygon::PolygonHoleJoiner;
using geos::util::IllegalStateException;

std::unique_ptr<Polygon> ReadPolygon(const GeometryFactory& factory,
                                     std::string_view wkt) {
  WKTReader reader(factory);
  return reader.read<Polygon>(std::string(wkt));
}

std::size_t CountVertexOccurrences2D(const CoordinateSequence& seq,
                                     const CoordinateXY& target) {
  std::size_t count = 0;
  for (std::size_t i = 0; i < seq.size(); ++i) {
    if (seq.getAt<CoordinateXY>(i).equals2D(target)) {
      ++count;
    }
  }
  return count;
}

bool ContainsAllRingVertices2D(const CoordinateSequence& joined,
                               const LinearRing& ring) {
  const CoordinateSequence* ring_coords = ring.getCoordinatesRO();
  for (std::size_t i = 0; i < ring_coords->size(); ++i) {
    if (CountVertexOccurrences2D(joined, ring_coords->getAt<CoordinateXY>(i)) ==
        0) {
      return false;
    }
  }
  return true;
}

void ExpectSequenceEquals2D(const CoordinateSequence& actual,
                            const std::vector<CoordinateXY>& expected) {
  ASSERT_EQ(actual.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_DOUBLE_EQ(actual.getX(i), expected[i].x);
    EXPECT_DOUBLE_EQ(actual.getY(i), expected[i].y);
  }
}

void ExpectValidJoinedPolygon(const Polygon& input, double expected_area,
                              std::size_t expected_point_count) {
  std::unique_ptr<CoordinateSequence> joined = PolygonHoleJoiner::join(&input);
  ASSERT_NE(joined, nullptr);
  ASSERT_GE(joined->size(), 4u);
  EXPECT_EQ(joined->size(), expected_point_count);
  EXPECT_TRUE(joined->isRing());
  EXPECT_TRUE(joined->front().equals2D(joined->back()));
  EXPECT_FALSE(Orientation::isCCW(joined.get()));
  EXPECT_NEAR(Area::ofRingSigned(joined.get()), expected_area, 1e-9);

  EXPECT_TRUE(ContainsAllRingVertices2D(*joined, *input.getExteriorRing()));
  for (std::size_t i = 0; i < input.getNumInteriorRing(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_TRUE(ContainsAllRingVertices2D(*joined, *input.getInteriorRingN(i)));
  }

  std::unique_ptr<Polygon> joined_poly =
      PolygonHoleJoiner::joinAsPolygon(&input);
  ASSERT_NE(joined_poly, nullptr);
  EXPECT_EQ(joined_poly->getFactory(), input.getFactory());
  EXPECT_EQ(joined_poly->getNumInteriorRing(), 0u);
  EXPECT_TRUE(CoordinateSequence::equals(
      joined_poly->getExteriorRing()->getCoordinatesRO(), joined.get()));
  EXPECT_NEAR(joined_poly->getArea(), expected_area, 1e-9);
}

TEST(PolygonHoleJoinerTest, NoHolesCcwReversedToCw) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> poly =
      ReadPolygon(*factory, "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  ASSERT_TRUE(Orientation::isCCW(poly->getExteriorRing()->getCoordinatesRO()));

  std::unique_ptr<CoordinateSequence> joined =
      PolygonHoleJoiner::join(poly.get());
  ASSERT_NE(joined, nullptr);
  ExpectSequenceEquals2D(*joined, {{0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}});
  ExpectValidJoinedPolygon(*poly, 100.0, 5);
}

TEST(PolygonHoleJoinerTest, NoHolesCwPreservedViaDirectCompute) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> poly =
      ReadPolygon(*factory, "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))");
  ASSERT_FALSE(Orientation::isCCW(poly->getExteriorRing()->getCoordinatesRO()));

  PolygonHoleJoiner joiner(poly.get());
  std::unique_ptr<CoordinateSequence> joined = joiner.compute();
  ASSERT_NE(joined, nullptr);
  ExpectSequenceEquals2D(*joined, {{0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}});
  ExpectValidJoinedPolygon(*poly, 100.0, 5);
}

TEST(PolygonHoleJoinerTest, SingleNonTouchingHoleNormalizesBothOrientations) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // First: OGC orientation (CCW shell, CW hole).
  // Second: opposite orientation (CW shell, CCW hole).
  constexpr std::string_view kWkts[] = {
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 8, 8 8, 8 2, 2 2))",
      "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), (2 2, 8 2, 8 8, 2 8, 2 2))",
  };
  const std::vector<CoordinateXY> expected = {
      {0, 0}, {0, 10}, {2, 2},   {8, 2},  {8, 8}, {2, 8},
      {2, 2}, {0, 10}, {10, 10}, {10, 0}, {0, 0},
  };

  for (std::string_view wkt : kWkts) {
    SCOPED_TRACE(std::string(wkt));
    std::unique_ptr<Polygon> poly = ReadPolygon(*factory, wkt);
    std::unique_ptr<CoordinateSequence> joined =
        PolygonHoleJoiner::join(poly.get());
    ASSERT_NE(joined, nullptr);
    ExpectSequenceEquals2D(*joined, expected);
    ExpectValidJoinedPolygon(*poly, 64.0, expected.size());
  }
}

TEST(PolygonHoleJoinerTest,
     NonTouchingHoleFindsLowestLeftVertexNotAtStartWithYTieBreak) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // CCW hole starting at (8, 6): visits (8,6) -> (2,6) -> (2,3) -> (8,3).
  // Lowest-left vertex is (2,3) at index 2 (tie on x=2 broken by smaller y).
  std::unique_ptr<Polygon> poly = ReadPolygon(
      *factory,
      "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), (8 6, 2 6, 2 3, 8 3, 8 6))");

  std::unique_ptr<CoordinateSequence> joined =
      PolygonHoleJoiner::join(poly.get());
  ASSERT_NE(joined, nullptr);
  ExpectSequenceEquals2D(*joined, {{0, 0},
                                   {0, 10},
                                   {2, 3},
                                   {8, 3},
                                   {8, 6},
                                   {2, 6},
                                   {2, 3},
                                   {0, 10},
                                   {10, 10},
                                   {10, 0},
                                   {0, 0}});
  ExpectValidJoinedPolygon(*poly, 82.0, 11);
}

TEST(PolygonHoleJoinerTest, NonTouchingHoleJoinsAtStartOfJoinedPts) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Triangle shell where (0, 0) is the sole vertex in x <= 5 (joinedPts.begin)
  // and lies at index 0 in joinedRing, exercising prev(0, size) in
  // isLineInterior.
  std::unique_ptr<Polygon> poly = ReadPolygon(
      *factory, "POLYGON ((0 0, 10 10, 10 0, 0 0), (5 1, 9 1, 9 3, 5 1))");

  std::unique_ptr<CoordinateSequence> joined =
      PolygonHoleJoiner::join(poly.get());
  ASSERT_NE(joined, nullptr);
  ExpectSequenceEquals2D(*joined, {{0, 0},
                                   {5, 1},
                                   {9, 1},
                                   {9, 3},
                                   {5, 1},
                                   {0, 0},
                                   {10, 10},
                                   {10, 0},
                                   {0, 0}});
  ExpectValidJoinedPolygon(*poly, 46.0, 9);
}

TEST(PolygonHoleJoinerTest, HoleTouchingShellAtStartVertex) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Hole starts at the touching vertex (0, 0), which is index 0 of both rings.
  // Exercises holeTouchIndex == 0, prev(0, size), and zero-length join
  // (no duplicate join vertices).
  std::unique_ptr<Polygon> poly = ReadPolygon(
      *factory,
      "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), (0 0, 4 2, 2 4, 0 0))");

  std::unique_ptr<CoordinateSequence> joined =
      PolygonHoleJoiner::join(poly.get());
  ASSERT_NE(joined, nullptr);
  ExpectSequenceEquals2D(
      *joined,
      {{0, 0}, {4, 2}, {2, 4}, {0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}});
  ExpectValidJoinedPolygon(*poly, 94.0, 8);
}

TEST(PolygonHoleJoinerTest, HoleTouchingShellAtLastUniqueVertex) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Touch vertex (10, 0) is at index size - 2 in shellRing, exercising
  // next(size - 2, size) wrapping to 0 in isLineInterior.
  std::unique_ptr<Polygon> poly = ReadPolygon(
      *factory,
      "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), (6 2, 10 0, 8 4, 6 2))");

  std::unique_ptr<CoordinateSequence> joined =
      PolygonHoleJoiner::join(poly.get());
  ASSERT_NE(joined, nullptr);
  ExpectSequenceEquals2D(
      *joined,
      {{0, 0}, {0, 10}, {10, 10}, {10, 0}, {8, 4}, {6, 2}, {10, 0}, {0, 0}});
  ExpectValidJoinedPolygon(*poly, 94.0, 8);
}

TEST(PolygonHoleJoinerTest, HoleVertexTouchingShellEdgeInteriorNodesShell) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Hole vertex (5, 0) touches the interior of shell edge (10, 0)-(0, 0).
  // PolygonNoder adds (5, 0) to shellRing before joinTouchingHole runs.
  std::unique_ptr<Polygon> poly = ReadPolygon(
      *factory,
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (5 0, 7 3, 3 3, 5 0))");

  std::unique_ptr<CoordinateSequence> joined =
      PolygonHoleJoiner::join(poly.get());
  ASSERT_NE(joined, nullptr);
  ExpectSequenceEquals2D(*joined, {{0, 0},
                                   {0, 10},
                                   {10, 10},
                                   {10, 0},
                                   {5, 0},
                                   {7, 3},
                                   {3, 3},
                                   {5, 0},
                                   {0, 0}});
  ExpectValidJoinedPolygon(*poly, 94.0, 9);
}

TEST(PolygonHoleJoinerTest, ShellVertexTouchingHoleEdgeInteriorNodesHole) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Shell reflex vertex (5, 5) touches the interior of hole edge (3, 5)-(7, 5).
  // PolygonNoder adds (5, 5) to holeRing before joinTouchingHole runs.
  std::unique_ptr<Polygon> poly = ReadPolygon(
      *factory,
      "POLYGON ((0 0, 10 0, 10 10, 5 5, 0 10, 0 0), (3 5, 7 5, 5 2, 3 5))");

  std::unique_ptr<CoordinateSequence> joined =
      PolygonHoleJoiner::join(poly.get());
  ASSERT_NE(joined, nullptr);
  ExpectSequenceEquals2D(*joined, {{0, 0},
                                   {0, 10},
                                   {5, 5},
                                   {3, 5},
                                   {5, 2},
                                   {7, 5},
                                   {5, 5},
                                   {10, 10},
                                   {10, 0},
                                   {0, 0}});
  ExpectValidJoinedPolygon(*poly, 69.0, 10);
}

TEST(PolygonHoleJoinerTest, ChainedTouchingHolesSortedAndJoinedToPriorHole) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Listed right-to-left to exercise sortHoles:
  // - Hole A ([2..4]x[2..4]) does not touch the shell, only Hole B;
  //   so isHoleTouchingHint[0] is true, findHoleTouchIndex returns
  //   NO_COORD_INDEX, and joinHole falls back to joinNonTouchingHole.
  // - Hole B ([4..6]x[4..6]) touches A at vertex (4, 4).
  // - Hole C ((6,5)-(8,4)-(8,6)) touches B on interior of edge (6,4)-(6,6).
  std::unique_ptr<Polygon> poly =
      ReadPolygon(*factory,
                  "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), "
                  "(6 5, 8 4, 8 6, 6 5), "
                  "(4 4, 6 4, 6 6, 4 6, 4 4), "
                  "(2 2, 4 2, 4 4, 2 4, 2 2))");

  std::unique_ptr<CoordinateSequence> joined =
      PolygonHoleJoiner::join(poly.get());
  ASSERT_NE(joined, nullptr);
  ExpectValidJoinedPolygon(*poly, 90.0, 19);
  EXPECT_EQ(CountVertexOccurrences2D(*joined, CoordinateXY(4, 4)), 2u);
  EXPECT_EQ(CountVertexOccurrences2D(*joined, CoordinateXY(6, 5)), 2u);
}

TEST(PolygonHoleJoinerTest,
     FindJoinIndexSelectsInteriorCornerAtRepeatedVertex) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Hole 1 touches top shell edge at (5, 10) and stays in x <= 5.
  // Hole 2 starts at x = 5.1 (sorted after Hole 1) and joins to (5, 10).
  // At (5, 10), the first occurrence in joinedRing bounds the sector to the
  // left of Hole 1, so findJoinIndex must skip it (isLineInterior == false)
  // and select the second occurrence.
  std::unique_ptr<Polygon> poly =
      ReadPolygon(*factory,
                  "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), "
                  "(5.1 2, 8 2, 8 4, 5.1 4, 5.1 2), "
                  "(5 10, 4 8, 4.9 8, 5 10))");

  std::unique_ptr<CoordinateSequence> joined =
      PolygonHoleJoiner::join(poly.get());
  ASSERT_NE(joined, nullptr);
  ExpectValidJoinedPolygon(*poly, 93.3, 15);
  EXPECT_EQ(CountVertexOccurrences2D(*joined, CoordinateXY(5, 10)), 3u);
}

TEST(PolygonHoleJoinerTest,
     FindJoinableVertexSkipsCollinearAndNonIntersectingEnvelopeEdges) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Hole lowest-left is (5, 2), matching shell vertex (5, 10) in x.
  // findJoinableVertex advances past (5, 10) in the upper_bound loop, tests
  // (5, 2)-(5, 10) which collinearly overlaps hole edge (5, 4)-(5, 2)
  // (getIntersectionNum() >= 2), then tests (5, 2)-(0, 10) whose envelope
  // [0, 5]x[2, 10] overlaps shell edge (6, 1)-(-2, 5) (envelope [-2, 6]x[1, 5])
  // without intersecting it (getIntersectionNum() == 0).
  std::unique_ptr<Polygon> poly =
      ReadPolygon(*factory,
                  "POLYGON ((-2 5, 0 10, 5 10, 10 10, 10 0, 6 1, -2 5), "
                  "(5 2, 8 2, 8 4, 5 4, 5 2))");

  std::unique_ptr<CoordinateSequence> joined =
      PolygonHoleJoiner::join(poly.get());
  ASSERT_NE(joined, nullptr);
  ExpectSequenceEquals2D(*joined, {{-2, 5},
                                   {0, 10},
                                   {5, 2},
                                   {8, 2},
                                   {8, 4},
                                   {5, 4},
                                   {5, 2},
                                   {0, 10},
                                   {5, 10},
                                   {10, 10},
                                   {10, 0},
                                   {6, 1},
                                   {-2, 5}});
  ExpectValidJoinedPolygon(*poly, 83.0, 13);
}

TEST(PolygonHoleJoinerTest, FindJoinableVertexWalksPastBlockedCandidates) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Hole (7,8)-(9,8)-(9,10)-(7,10) sits above a horizontal shelf at y in [6,7].
  // All vertices in x in [2, 5] lie below the shelf, so findJoinableVertex must
  // reject every blocked vertex until reaching (0, 12).
  std::unique_ptr<Polygon> poly =
      ReadPolygon(*factory,
                  "POLYGON ((0 0, 0 6, 8 6, 8 7, 0 7, 0 12, 12 12, 12 0, "
                  "5 0, 5 5, 4 5, 4 0, 3 0, 3 5, 2 5, 2 0, 0 0), "
                  "(7 8, 9 8, 9 10, 7 10, 7 8))");

  std::unique_ptr<CoordinateSequence> joined =
      PolygonHoleJoiner::join(poly.get());
  ASSERT_NE(joined, nullptr);
  ExpectValidJoinedPolygon(*poly, 122.0, 23);
  EXPECT_EQ(CountVertexOccurrences2D(*joined, CoordinateXY(0, 12)), 2u);
}

TEST(PolygonHoleJoinerTest,
     MultipleUnsortedHolesUseEnvelopeTieBreakAndJoinPreviouslyJoinedHoles) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Four holes in scrambled order; B and C share minx = 4 (tie broken by miny).
  // Hole A forms a vertical barrier, forcing B to join to A, C to B, D to C.
  std::unique_ptr<Polygon> poly =
      ReadPolygon(*factory,
                  "POLYGON ((0 0, 0 12, 12 12, 12 0, 0 0), "
                  "(8 5, 10 5, 10 7, 8 7, 8 5), "
                  "(4 7, 6 7, 6 10, 4 10, 4 7), "
                  "(1 1, 3 1, 3 11, 1 11, 1 1), "
                  "(4 1, 6 1, 6 4, 4 4, 4 1))");

  ExpectValidJoinedPolygon(*poly, 108.0, 29);
}

TEST(PolygonHoleJoinerTest, ConcaveShellWithConcaveHole) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> poly =
      ReadPolygon(*factory,
                  "POLYGON ((0 0, 0 10, 6 10, 6 4, 10 4, 10 0, 0 0), "
                  "(1 1, 5 1, 5 3, 3 3, 3 8, 1 8, 1 1))");

  ExpectValidJoinedPolygon(*poly, 58.0, 15);
}

TEST(PolygonHoleJoinerTest, PolygonWithZCoordinatesHandledCleanly) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Without holes: shell Z ordinates are preserved after CW orientation.
  std::unique_ptr<Polygon> no_hole_z = ReadPolygon(
      *factory, "POLYGON Z ((0 0 10, 10 0 20, 10 10 30, 0 10 40, 0 0 10))");
  std::unique_ptr<CoordinateSequence> joined_no_hole =
      PolygonHoleJoiner::join(no_hole_z.get());
  ASSERT_NE(joined_no_hole, nullptr);
  ASSERT_EQ(joined_no_hole->size(), 5u);
  EXPECT_TRUE(joined_no_hole->hasZ());
  EXPECT_DOUBLE_EQ(joined_no_hole->getAt<Coordinate>(0).z, 10.0);
  EXPECT_DOUBLE_EQ(joined_no_hole->getAt<Coordinate>(1).z, 40.0);
  EXPECT_DOUBLE_EQ(joined_no_hole->getAt<Coordinate>(2).z, 30.0);
  EXPECT_DOUBLE_EQ(joined_no_hole->getAt<Coordinate>(3).z, 20.0);
  EXPECT_DOUBLE_EQ(joined_no_hole->getAt<Coordinate>(4).z, 10.0);
  ExpectValidJoinedPolygon(*no_hole_z, 100.0, 5);

  // With hole: existing vertex Z ordinates propagate through PolygonNoder
  // while computing a valid 2D joined ring.
  std::unique_ptr<Polygon> with_hole_z =
      ReadPolygon(*factory,
                  "POLYGON Z ((0 0 1, 10 0 2, 10 10 3, 0 10 4, 0 0 1), "
                  "(2 2 5, 2 8 6, 8 8 7, 8 2 8, 2 2 5))");
  std::unique_ptr<CoordinateSequence> joined_with_hole =
      PolygonHoleJoiner::join(with_hole_z.get());
  ASSERT_NE(joined_with_hole, nullptr);
  ASSERT_EQ(joined_with_hole->size(), 11u);
  EXPECT_TRUE(joined_with_hole->hasZ());
  const double expected_z[] = {1.0, 4.0, 5.0, 8.0, 7.0, 6.0,
                               5.0, 4.0, 3.0, 2.0, 1.0};
  for (std::size_t i = 0; i < 11u; ++i) {
    SCOPED_TRACE(i);
    EXPECT_DOUBLE_EQ(joined_with_hole->getAt<Coordinate>(i).z, expected_z[i]);
  }
  ExpectValidJoinedPolygon(*with_hole_z, 64.0, 11);
}

TEST(PolygonHoleJoinerTest, PolygonWithMAndZMCoordinatesHandledCleanly) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<Polygon> with_hole_m = ReadPolygon(
      *factory,
      "POLYGON M ((0 0 100, 10 0 200, 10 10 300, 0 10 400, 0 0 100), "
      "(2 2 1, 2 8 2, 8 8 3, 8 2 4, 2 2 1))");
  std::unique_ptr<CoordinateSequence> joined_m =
      PolygonHoleJoiner::join(with_hole_m.get());
  ASSERT_NE(joined_m, nullptr);
  EXPECT_FALSE(joined_m->hasM());
  EXPECT_FALSE(joined_m->hasZ());
  EXPECT_TRUE(std::isnan(joined_m->getAt<Coordinate>(0).z));
  ExpectValidJoinedPolygon(*with_hole_m, 64.0, 11);

  std::unique_ptr<Polygon> with_hole_zm = ReadPolygon(
      *factory,
      "POLYGON ZM ((0 0 1 10, 10 0 2 20, 10 10 3 30, 0 10 4 40, 0 0 1 10), "
      "(2 2 5 50, 2 8 6 60, 8 8 7 70, 8 2 8 80, 2 2 5 50))");
  std::unique_ptr<CoordinateSequence> joined_zm =
      PolygonHoleJoiner::join(with_hole_zm.get());
  ASSERT_NE(joined_zm, nullptr);
  EXPECT_FALSE(joined_zm->hasM());
  ExpectValidJoinedPolygon(*with_hole_zm, 64.0, 11);
}

TEST(PolygonHoleJoinerTest, HoleJoinVertexAtLastUniqueIndexWrapsCleanly) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Non-touching CCW hole with lowest-left vertex (2, 2) at index holeSize-1.
  std::unique_ptr<Polygon> non_touching = ReadPolygon(
      *factory,
      "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), (8 2, 8 8, 2 8, 2 2, 8 2))");
  std::unique_ptr<CoordinateSequence> joined_non_touching =
      PolygonHoleJoiner::join(non_touching.get());
  ASSERT_NE(joined_non_touching, nullptr);
  ExpectSequenceEquals2D(*joined_non_touching, {{0, 0},
                                                {0, 10},
                                                {2, 2},
                                                {8, 2},
                                                {8, 8},
                                                {2, 8},
                                                {2, 2},
                                                {0, 10},
                                                {10, 10},
                                                {10, 0},
                                                {0, 0}});
  ExpectValidJoinedPolygon(*non_touching, 64.0, 11);

  // Touching CCW hole with touching vertex (0, 0) at index holeSize - 1.
  std::unique_ptr<Polygon> touching = ReadPolygon(
      *factory,
      "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), (4 2, 2 4, 0 0, 4 2))");
  std::unique_ptr<CoordinateSequence> joined_touching =
      PolygonHoleJoiner::join(touching.get());
  ASSERT_NE(joined_touching, nullptr);
  ExpectSequenceEquals2D(
      *joined_touching,
      {{0, 0}, {4, 2}, {2, 4}, {0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}});
  ExpectValidJoinedPolygon(*touching, 94.0, 8);
}

TEST(PolygonHoleJoinerTest,
     FindJoinableVertexSkipsMultipleSameXVerticesAndComputesDirectly) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Shell has three vertices at x=5 above y=2: (5, 8), (5, 9), (5, 10).
  // findJoinableVertex advances across all three in its x-equality loop,
  // then rejects each because its join segment hits (5, 8)-(10, 8) or overlaps
  // hole edge (5, 4)-(5, 2) before selecting (0, 10).
  std::unique_ptr<Polygon> poly =
      ReadPolygon(*factory,
                  "POLYGON ((0 0, 0 10, 5 10, 5 9, 6 9, 5 8, 10 8, 10 0, 0 0), "
                  "(5 2, 8 2, 8 4, 5 4, 5 2))");

  PolygonHoleJoiner joiner(poly.get());
  std::unique_ptr<CoordinateSequence> joined = joiner.compute();
  ASSERT_NE(joined, nullptr);
  ExpectSequenceEquals2D(*joined, {{0, 0},
                                   {0, 10},
                                   {5, 2},
                                   {8, 2},
                                   {8, 4},
                                   {5, 4},
                                   {5, 2},
                                   {0, 10},
                                   {5, 10},
                                   {5, 9},
                                   {6, 9},
                                   {5, 8},
                                   {10, 8},
                                   {10, 0},
                                   {0, 0}});
  ExpectValidJoinedPolygon(*poly, 84.5, 15);
}

TEST(PolygonHoleJoinerTest, ThrowsIllegalStateWhenHoleTouchesFromOutsideShell) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Invalid polygon: hole lies outside the shell and touches shell edge
  // (10, 0)-(10, 10) at (10, 5), so no interior join line exists at (10, 5).
  std::unique_ptr<Polygon> invalid_poly = ReadPolygon(
      *factory,
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (10 5, 12 3, 12 7, 10 5))");

  EXPECT_THROW(PolygonHoleJoiner::join(invalid_poly.get()),
               IllegalStateException);
  EXPECT_THROW(PolygonHoleJoiner::joinAsPolygon(invalid_poly.get()),
               IllegalStateException);
}

}  // namespace
}  // namespace geos_google
