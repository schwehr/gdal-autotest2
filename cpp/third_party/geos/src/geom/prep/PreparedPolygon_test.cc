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

// Unit tests for third_party/geos/src/geom/prep/PreparedPolygon.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - Creates an isolated GeometryFactory per test to avoid shared _refCount
//   mutation on GeometryFactory::getDefaultInstance().
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/prep/PreparedPolygon.cpp

#include "third_party/geos/include/geos/geom/prep/PreparedPolygon.h"

#include <array>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/algorithm/locate/IndexedPointInAreaLocator.h"
#include "third_party/geos/include/geos/algorithm/locate/PointOnGeometryLocator.h"
#include "third_party/geos/include/geos/algorithm/locate/SimplePointInAreaLocator.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/prep/PreparedGeometry.h"
#include "third_party/geos/include/geos/geom/prep/PreparedGeometryFactory.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/noding/FastSegmentSetIntersectionFinder.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"
#include "third_party/geos/include/geos/noding/SegmentStringUtil.h"
#include "third_party/geos/include/geos/operation/distance/IndexedFacetDistance.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::algorithm::locate::IndexedPointInAreaLocator;
using geos::algorithm::locate::PointOnGeometryLocator;
using geos::algorithm::locate::SimplePointInAreaLocator;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::geom::prep::PreparedGeometry;
using geos::geom::prep::PreparedGeometryFactory;
using geos::geom::prep::PreparedPolygon;
using geos::io::WKTReader;
using geos::noding::FastSegmentSetIntersectionFinder;
using geos::noding::SegmentString;
using geos::noding::SegmentStringUtil;
using geos::operation::distance::IndexedFacetDistance;
using geos::util::UnsupportedOperationException;

struct PredicateCase {
  std::string_view name;
  std::string_view test_wkt;
  bool expected;
};

class PreparedPolygonTest : public ::testing::Test {
 protected:
  Geometry::Ptr Read(std::string_view wkt) const {
    return reader_.read(std::string(wkt));
  }

  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader reader_{*factory_};
};

TEST_F(PreparedPolygonTest, FactoryCreatesPreparedPolygonForPolygonalTypes) {
  constexpr auto kTargetWkts = std::to_array<std::string_view>({
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
      "POLYGON ((0 0, 10 0, 5 10, 0 0))",
      "POLYGON EMPTY",
      "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
      "((10 0, 14 0, 14 4, 10 4, 10 0)))",
  });

  for (std::string_view wkt : kTargetWkts) {
    SCOPED_TRACE(wkt);
    Geometry::Ptr target = Read(wkt);
    std::unique_ptr<PreparedGeometry> pg =
        PreparedGeometryFactory::prepare(target.get());
    ASSERT_NE(pg, nullptr);
    EXPECT_NE(dynamic_cast<const PreparedPolygon*>(pg.get()), nullptr);
    EXPECT_EQ(&pg->getGeometry(), target.get());
  }

  Geometry::Ptr rect = Read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  std::unique_ptr<PreparedGeometry> pg =
      PreparedGeometryFactory::prepare(rect.get());
  Geometry::Ptr inside = Read("POINT (5 5)");
  Geometry::Ptr outside = Read("POINT (15 5)");

  EXPECT_TRUE(pg->contains(inside.get()));
  EXPECT_TRUE(pg->containsProperly(inside.get()));
  EXPECT_TRUE(pg->covers(inside.get()));
  EXPECT_TRUE(pg->intersects(inside.get()));
  EXPECT_FALSE(pg->disjoint(inside.get()));
  EXPECT_TRUE(pg->disjoint(outside.get()));
  EXPECT_DOUBLE_EQ(pg->distance(outside.get()), 5.0);
  EXPECT_TRUE(pg->isWithinDistance(outside.get(), 5.0));
  EXPECT_FALSE(pg->isWithinDistance(outside.get(), 4.9));
}

TEST_F(PreparedPolygonTest, DestructorCleansUpWithAndWithoutSegmentStrings) {
  Geometry::Ptr poly = Read("POLYGON ((0 0, 10 0, 10 8, 5 5, 0 10, 0 0))");
  {
    PreparedPolygon unused_prep(poly.get());
    EXPECT_EQ(&unused_prep.getGeometry(), poly.get());
  }
  {
    PreparedPolygon used_prep(poly.get());
    EXPECT_NE(used_prep.getIntersectionFinder(), nullptr);
    EXPECT_NE(used_prep.getPointLocator(), nullptr);
    EXPECT_NE(used_prep.getIndexedFacetDistance(), nullptr);
  }
  Geometry::Ptr empty_poly = Read("POLYGON EMPTY");
  {
    PreparedPolygon empty_prep(empty_poly.get());
    EXPECT_TRUE(empty_prep.getGeometry().isEmpty());
  }
}

TEST_F(PreparedPolygonTest,
       GetIntersectionFinderCachesAndDetectsIntersections) {
  Geometry::Ptr poly = Read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(2 2, 8 2, 8 8, 2 8, 2 2))");
  PreparedPolygon prep(poly.get());

  FastSegmentSetIntersectionFinder* finder1 = prep.getIntersectionFinder();
  FastSegmentSetIntersectionFinder* finder2 = prep.getIntersectionFinder();
  ASSERT_NE(finder1, nullptr);
  EXPECT_EQ(finder1, finder2);

  auto check_intersects = [&](std::string_view line_wkt, bool expected) {
    SCOPED_TRACE(line_wkt);
    Geometry::Ptr line = Read(line_wkt);
    SegmentString::ConstVect segs;
    absl::Cleanup cleanup = [&segs] {
      for (const SegmentString* s : segs) {
        delete s;
      }
    };
    SegmentStringUtil::extractSegmentStrings(line.get(), segs);
    EXPECT_EQ(finder1->intersects(&segs), expected);
  };

  check_intersects("LINESTRING (1 0, 1 10)", true);
  check_intersects("LINESTRING (5 1, 5 5)", true);
  check_intersects("LINESTRING (1 1, 1 9)", false);
  check_intersects("LINESTRING (4 4, 6 6)", false);

  Geometry::Ptr empty_poly = Read("POLYGON EMPTY");
  PreparedPolygon empty_prep(empty_poly.get());
  FastSegmentSetIntersectionFinder* empty_finder1 =
      empty_prep.getIntersectionFinder();
  FastSegmentSetIntersectionFinder* empty_finder2 =
      empty_prep.getIntersectionFinder();
  ASSERT_NE(empty_finder1, nullptr);
  EXPECT_EQ(empty_finder1, empty_finder2);
  Geometry::Ptr line = Read("LINESTRING (1 0, 1 10)");
  SegmentString::ConstVect segs;
  absl::Cleanup cleanup = [&segs] {
    for (const SegmentString* s : segs) {
      delete s;
    }
  };
  SegmentStringUtil::extractSegmentStrings(line.get(), segs);
  EXPECT_FALSE(empty_finder1->intersects(&segs));
}

TEST_F(PreparedPolygonTest,
       GetPointLocatorTransitionsFromSimpleToCachedIndexed) {
  Geometry::Ptr poly = Read("POLYGON ((0 0, 10 0, 10 10, 5 6, 0 10, 0 0))");
  PreparedPolygon prep(poly.get());

  PointOnGeometryLocator* loc1 = prep.getPointLocator();
  ASSERT_NE(loc1, nullptr);
  EXPECT_NE(dynamic_cast<SimplePointInAreaLocator*>(loc1), nullptr);

  PointOnGeometryLocator* loc2 = prep.getPointLocator();
  ASSERT_NE(loc2, nullptr);
  EXPECT_NE(loc2, loc1);
  EXPECT_NE(dynamic_cast<IndexedPointInAreaLocator*>(loc2), nullptr);

  PointOnGeometryLocator* loc3 = prep.getPointLocator();
  EXPECT_EQ(loc3, loc2);

  const CoordinateXY interior(2.0, 2.0);
  const CoordinateXY boundary(0.0, 5.0);
  const CoordinateXY concavity(5.0, 8.0);
  const CoordinateXY outside(15.0, 5.0);

  for (PointOnGeometryLocator* loc : {loc1, loc2, loc3}) {
    EXPECT_EQ(loc->locate(&interior), Location::INTERIOR);
    EXPECT_EQ(loc->locate(&boundary), Location::BOUNDARY);
    EXPECT_EQ(loc->locate(&concavity), Location::EXTERIOR);
    EXPECT_EQ(loc->locate(&outside), Location::EXTERIOR);
  }
}

TEST_F(PreparedPolygonTest, GetIndexedFacetDistanceCachesAndComputesDistance) {
  Geometry::Ptr poly = Read("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  PreparedPolygon prep(poly.get());

  IndexedFacetDistance* idf1 = prep.getIndexedFacetDistance();
  IndexedFacetDistance* idf2 = prep.getIndexedFacetDistance();
  ASSERT_NE(idf1, nullptr);
  EXPECT_EQ(idf1, idf2);

  Geometry::Ptr pt = Read("POINT (7 8)");
  EXPECT_DOUBLE_EQ(idf1->distance(pt.get()), 5.0);
  EXPECT_TRUE(idf1->isWithinDistance(pt.get(), 5.0));
  EXPECT_FALSE(idf1->isWithinDistance(pt.get(), 4.99));
}

TEST_F(PreparedPolygonTest, ContainsRectangleFastPathAndEnvelopeShortCircuit) {
  Geometry::Ptr rect = Read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  ASSERT_TRUE(rect->isRectangle());
  PreparedPolygon prep(rect.get());

  constexpr auto kCases = std::to_array<PredicateCase>({
      {.name = "outside_point", .test_wkt = "POINT (12 5)", .expected = false},
      {.name = "empty_point", .test_wkt = "POINT EMPTY", .expected = false},
      {.name = "empty_line", .test_wkt = "LINESTRING EMPTY", .expected = false},
      {.name = "empty_polygon", .test_wkt = "POLYGON EMPTY", .expected = false},
      {.name = "partially_outside_line",
       .test_wkt = "LINESTRING (5 5, 15 5)",
       .expected = false},
      {.name = "partially_outside_polygon",
       .test_wkt = "POLYGON ((5 5, 12 5, 12 8, 5 8, 5 5))",
       .expected = false},
      {.name = "interior_point", .test_wkt = "POINT (5 5)", .expected = true},
      {.name = "boundary_edge_point",
       .test_wkt = "POINT (0 5)",
       .expected = false},
      {.name = "boundary_corner_point",
       .test_wkt = "POINT (0 0)",
       .expected = false},
      {.name = "boundary_edge_line",
       .test_wkt = "LINESTRING (0 0, 10 0)",
       .expected = false},
      {.name = "line_touching_boundary_and_interior",
       .test_wkt = "LINESTRING (0 0, 5 5)",
       .expected = true},
      {.name = "identical_rectangle",
       .test_wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
       .expected = true},
      {.name = "interior_polygon",
       .test_wkt = "POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))",
       .expected = true},
  });

  for (const auto& c : kCases) {
    SCOPED_TRACE(c.name);
    Geometry::Ptr test_geom = Read(c.test_wkt);
    EXPECT_EQ(prep.contains(test_geom.get()), c.expected);
    EXPECT_EQ(rect->contains(test_geom.get()), c.expected);
  }

  Geometry::Ptr empty_poly = Read("POLYGON EMPTY");
  PreparedPolygon empty_prep(empty_poly.get());
  Geometry::Ptr pt = Read("POINT (5 5)");
  EXPECT_FALSE(empty_prep.contains(pt.get()));
}

TEST_F(PreparedPolygonTest, ContainsNonRectanglePolygonAndMultiPolygon) {
  Geometry::Ptr concave =
      Read("POLYGON ((0 0, 10 0, 10 4, 4 4, 4 10, 0 10, 0 0))");
  ASSERT_FALSE(concave->isRectangle());
  PreparedPolygon prep_concave(concave.get());

  constexpr auto kConcaveCases = std::to_array<PredicateCase>({
      {.name = "outside_envelope",
       .test_wkt = "POINT (-1 5)",
       .expected = false},
      {.name = "point_in_concavity",
       .test_wkt = "POINT (8 8)",
       .expected = false},
      {.name = "point_on_boundary",
       .test_wkt = "POINT (0 5)",
       .expected = false},
      {.name = "point_in_interior",
       .test_wkt = "POINT (2 2)",
       .expected = true},
      {.name = "line_in_interior",
       .test_wkt = "LINESTRING (2 2, 2 8)",
       .expected = true},
      {.name = "line_touching_boundary_going_inside",
       .test_wkt = "LINESTRING (0 2, 2 2)",
       .expected = true},
      {.name = "line_crossing_concavity",
       .test_wkt = "LINESTRING (2 8, 8 2)",
       .expected = false},
      {.name = "line_on_boundary_only",
       .test_wkt = "LINESTRING (0 1, 0 9)",
       .expected = false},
      {.name = "polygon_in_interior",
       .test_wkt = "POLYGON ((1 1, 3 1, 3 3, 1 3, 1 1))",
       .expected = true},
      {.name = "polygon_identical",
       .test_wkt = "POLYGON ((0 0, 10 0, 10 4, 4 4, 4 10, 0 10, 0 0))",
       .expected = true},
      {.name = "polygon_overlapping_concavity",
       .test_wkt = "POLYGON ((1 1, 9 1, 9 9, 1 9, 1 1))",
       .expected = false},
  });

  for (const auto& c : kConcaveCases) {
    SCOPED_TRACE(c.name);
    Geometry::Ptr test_geom = Read(c.test_wkt);
    EXPECT_EQ(prep_concave.contains(test_geom.get()), c.expected);
    EXPECT_EQ(concave->contains(test_geom.get()), c.expected);
  }

  Geometry::Ptr mp = Read(
      "MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "((20 0, 30 0, 30 10, 20 10, 20 0)))");
  PreparedPolygon prep_mp(mp.get());
  Geometry::Ptr inside_first = Read("POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))");
  Geometry::Ptr spanning_gap = Read("POLYGON ((5 2, 25 2, 25 8, 5 8, 5 2))");
  EXPECT_TRUE(prep_mp.contains(inside_first.get()));
  EXPECT_FALSE(prep_mp.contains(spanning_gap.get()));
}

TEST_F(PreparedPolygonTest, ContainsProperlyShortCircuitsAndDelegates) {
  Geometry::Ptr donut = Read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(3 3, 7 3, 7 7, 3 7, 3 3))");
  PreparedPolygon prep(donut.get());

  constexpr auto kCases = std::to_array<PredicateCase>({
      {.name = "outside_envelope",
       .test_wkt = "POINT (12 5)",
       .expected = false},
      {.name = "empty_geom", .test_wkt = "POLYGON EMPTY", .expected = false},
      {.name = "strictly_interior_point",
       .test_wkt = "POINT (1 1)",
       .expected = true},
      {.name = "boundary_point", .test_wkt = "POINT (0 5)", .expected = false},
      {.name = "hole_boundary_point",
       .test_wkt = "POINT (3 5)",
       .expected = false},
      {.name = "point_inside_hole",
       .test_wkt = "POINT (5 5)",
       .expected = false},
      {.name = "strictly_interior_line",
       .test_wkt = "LINESTRING (1 1, 2 9)",
       .expected = true},
      {.name = "line_touching_boundary",
       .test_wkt = "LINESTRING (0 1, 2 1)",
       .expected = false},
      {.name = "strictly_interior_polygon",
       .test_wkt = "POLYGON ((1 1, 2 1, 2 9, 1 9, 1 1))",
       .expected = true},
      {.name = "polygon_enclosing_hole",
       .test_wkt = "POLYGON ((1 1, 9 1, 9 9, 1 9, 1 1))",
       .expected = false},
      {.name = "identical_polygon",
       .test_wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
                   "(3 3, 7 3, 7 7, 3 7, 3 3))",
       .expected = false},
  });

  for (const auto& c : kCases) {
    SCOPED_TRACE(c.name);
    Geometry::Ptr test_geom = Read(c.test_wkt);
    EXPECT_EQ(prep.containsProperly(test_geom.get()), c.expected);
    EXPECT_EQ(donut->relate(test_geom.get(), "T**FF*FF*"), c.expected);
  }

  Geometry::Ptr empty_poly = Read("POLYGON EMPTY");
  PreparedPolygon empty_prep(empty_poly.get());
  Geometry::Ptr pt = Read("POINT (1 1)");
  EXPECT_FALSE(empty_prep.containsProperly(pt.get()));
}

TEST_F(PreparedPolygonTest, CoversRectangleFastPathAndGeneralPolygon) {
  Geometry::Ptr rect = Read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  ASSERT_TRUE(rect->isRectangle());
  PreparedPolygon prep_rect(rect.get());

  constexpr auto kRectCases = std::to_array<PredicateCase>({
      {.name = "outside_point", .test_wkt = "POINT (11 5)", .expected = false},
      {.name = "empty_point", .test_wkt = "POINT EMPTY", .expected = false},
      {.name = "empty_polygon", .test_wkt = "POLYGON EMPTY", .expected = false},
      {.name = "interior_point", .test_wkt = "POINT (5 5)", .expected = true},
      {.name = "boundary_corner", .test_wkt = "POINT (0 0)", .expected = true},
      {.name = "boundary_edge_line",
       .test_wkt = "LINESTRING (0 0, 10 0)",
       .expected = true},
      {.name = "identical_rectangle",
       .test_wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
       .expected = true},
      {.name = "partially_outside_line",
       .test_wkt = "LINESTRING (5 5, 11 5)",
       .expected = false},
  });

  for (const auto& c : kRectCases) {
    SCOPED_TRACE(c.name);
    Geometry::Ptr test_geom = Read(c.test_wkt);
    EXPECT_EQ(prep_rect.covers(test_geom.get()), c.expected);
    EXPECT_EQ(rect->covers(test_geom.get()), c.expected);
  }

  Geometry::Ptr concave =
      Read("POLYGON ((0 0, 10 0, 10 4, 4 4, 4 10, 0 10, 0 0))");
  ASSERT_FALSE(concave->isRectangle());
  PreparedPolygon prep_concave(concave.get());

  constexpr auto kConcaveCases = std::to_array<PredicateCase>({
      {.name = "outside_envelope",
       .test_wkt = "POINT (12 5)",
       .expected = false},
      {.name = "empty_line", .test_wkt = "LINESTRING EMPTY", .expected = false},
      {.name = "point_in_concavity",
       .test_wkt = "POINT (8 8)",
       .expected = false},
      {.name = "point_on_boundary",
       .test_wkt = "POINT (0 5)",
       .expected = true},
      {.name = "line_on_boundary",
       .test_wkt = "LINESTRING (0 0, 0 10)",
       .expected = true},
      {.name = "identical_polygon",
       .test_wkt = "POLYGON ((0 0, 10 0, 10 4, 4 4, 4 10, 0 10, 0 0))",
       .expected = true},
      {.name = "polygon_spanning_concavity",
       .test_wkt = "POLYGON ((1 1, 9 1, 9 9, 1 9, 1 1))",
       .expected = false},
  });

  for (const auto& c : kConcaveCases) {
    SCOPED_TRACE(c.name);
    Geometry::Ptr test_geom = Read(c.test_wkt);
    EXPECT_EQ(prep_concave.covers(test_geom.get()), c.expected);
    EXPECT_EQ(concave->covers(test_geom.get()), c.expected);
  }

  Geometry::Ptr empty_poly = Read("POLYGON EMPTY");
  PreparedPolygon empty_prep(empty_poly.get());
  Geometry::Ptr pt = Read("POINT (5 5)");
  EXPECT_FALSE(empty_prep.covers(pt.get()));
}

TEST_F(PreparedPolygonTest, IntersectsRectangleGeneralAndCurvedThrows) {
  Geometry::Ptr rect = Read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  ASSERT_TRUE(rect->isRectangle());
  PreparedPolygon prep_rect(rect.get());

  Geometry::Ptr curved_line = Read("CIRCULARSTRING (0 0, 5 5, 10 0)");
  Geometry::Ptr curved_poly =
      Read("CURVEPOLYGON (CIRCULARSTRING (0 0, 4 0, 4 4, 0 4, 0 0))");
  EXPECT_THROW(prep_rect.intersects(curved_line.get()),
               UnsupportedOperationException);
  EXPECT_THROW(prep_rect.intersects(curved_poly.get()),
               UnsupportedOperationException);

  constexpr auto kRectCases = std::to_array<PredicateCase>({
      {.name = "disjoint_point", .test_wkt = "POINT (15 5)", .expected = false},
      {.name = "empty_point", .test_wkt = "POINT EMPTY", .expected = false},
      {.name = "empty_line", .test_wkt = "LINESTRING EMPTY", .expected = false},
      {.name = "overlapping_envelope_disjoint_diagonal",
       .test_wkt = "LINESTRING (8 15, 15 8)",
       .expected = false},
      {.name = "boundary_touch_point",
       .test_wkt = "POINT (0 5)",
       .expected = true},
      {.name = "interior_point", .test_wkt = "POINT (5 5)", .expected = true},
      {.name = "crossing_line",
       .test_wkt = "LINESTRING (-5 5, 15 5)",
       .expected = true},
      {.name = "enclosing_polygon",
       .test_wkt = "POLYGON ((-2 -2, 12 -2, 12 12, -2 12, -2 -2))",
       .expected = true},
  });

  for (const auto& c : kRectCases) {
    SCOPED_TRACE(c.name);
    Geometry::Ptr test_geom = Read(c.test_wkt);
    EXPECT_EQ(prep_rect.intersects(test_geom.get()), c.expected);
    EXPECT_EQ(rect->intersects(test_geom.get()), c.expected);
  }

  Geometry::Ptr donut = Read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(2 2, 8 2, 8 8, 2 8, 2 2))");
  ASSERT_FALSE(donut->isRectangle());
  PreparedPolygon prep_donut(donut.get());

  EXPECT_THROW(prep_donut.intersects(curved_line.get()),
               UnsupportedOperationException);

  constexpr auto kDonutCases = std::to_array<PredicateCase>({
      {.name = "disjoint_envelope",
       .test_wkt = "POINT (20 5)",
       .expected = false},
      {.name = "point_inside_hole",
       .test_wkt = "POINT (5 5)",
       .expected = false},
      {.name = "polygon_inside_hole",
       .test_wkt = "POLYGON ((3 3, 7 3, 7 7, 3 7, 3 3))",
       .expected = false},
      {.name = "point_in_ring", .test_wkt = "POINT (1 5)", .expected = true},
      {.name = "point_on_hole_boundary",
       .test_wkt = "POINT (2 5)",
       .expected = true},
      {.name = "crossing_line",
       .test_wkt = "LINESTRING (5 -2, 5 12)",
       .expected = true},
      {.name = "enclosing_polygon",
       .test_wkt = "POLYGON ((-2 -2, 12 -2, 12 12, -2 12, -2 -2))",
       .expected = true},
  });

  for (const auto& c : kDonutCases) {
    SCOPED_TRACE(c.name);
    Geometry::Ptr test_geom = Read(c.test_wkt);
    EXPECT_EQ(prep_donut.intersects(test_geom.get()), c.expected);
    EXPECT_EQ(donut->intersects(test_geom.get()), c.expected);
  }

  Geometry::Ptr empty_poly = Read("POLYGON EMPTY");
  PreparedPolygon empty_prep(empty_poly.get());
  Geometry::Ptr pt = Read("POINT (5 5)");
  EXPECT_FALSE(empty_prep.intersects(pt.get()));
}

TEST_F(PreparedPolygonTest, DistanceAndIsWithinDistance) {
  Geometry::Ptr square = Read("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  PreparedPolygon prep(square.get());

  Geometry::Ptr empty_pt = Read("POINT EMPTY");
  Geometry::Ptr empty_poly = Read("POLYGON EMPTY");
  PreparedPolygon empty_prep(empty_poly.get());

  EXPECT_EQ(prep.distance(empty_pt.get()),
            std::numeric_limits<double>::infinity());
  EXPECT_EQ(prep.distance(empty_poly.get()),
            std::numeric_limits<double>::infinity());
  EXPECT_FALSE(prep.isWithinDistance(empty_pt.get(), 10.0));
  EXPECT_FALSE(prep.isWithinDistance(empty_poly.get(), 10.0));

  Geometry::Ptr interior_pt = Read("POINT (2 2)");
  EXPECT_EQ(empty_prep.distance(interior_pt.get()),
            std::numeric_limits<double>::infinity());
  EXPECT_FALSE(empty_prep.isWithinDistance(interior_pt.get(), 10.0));

  EXPECT_DOUBLE_EQ(prep.distance(interior_pt.get()), 0.0);
  EXPECT_TRUE(prep.isWithinDistance(interior_pt.get(), 0.0));

  Geometry::Ptr boundary_pt = Read("POINT (4 2)");
  EXPECT_DOUBLE_EQ(prep.distance(boundary_pt.get()), 0.0);
  EXPECT_TRUE(prep.isWithinDistance(boundary_pt.get(), 0.0));

  Geometry::Ptr far_pt = Read("POINT (7 8)");
  EXPECT_DOUBLE_EQ(prep.distance(far_pt.get()), 5.0);
  EXPECT_TRUE(prep.isWithinDistance(far_pt.get(), 5.0));
  EXPECT_FALSE(prep.isWithinDistance(far_pt.get(), 4.999));
  EXPECT_FALSE(prep.isWithinDistance(far_pt.get(), 0.0));
  EXPECT_FALSE(prep.isWithinDistance(far_pt.get(), -1.0));

  Geometry::Ptr disjoint_poly = Read("POLYGON ((7 0, 9 0, 9 4, 7 4, 7 0))");
  EXPECT_DOUBLE_EQ(prep.distance(disjoint_poly.get()), 3.0);
  EXPECT_TRUE(prep.isWithinDistance(disjoint_poly.get(), 3.0));
  EXPECT_FALSE(prep.isWithinDistance(disjoint_poly.get(), 2.999));

  Geometry::Ptr enclosing_poly =
      Read("POLYGON ((-5 -5, 10 -5, 10 10, -5 10, -5 -5))");
  EXPECT_DOUBLE_EQ(prep.distance(enclosing_poly.get()), 0.0);
  EXPECT_TRUE(prep.isWithinDistance(enclosing_poly.get(), 0.0));
}

}  // namespace
}  // namespace geos_google
