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

// Unit tests for third_party/geos/src/operation/valid/IsSimpleOp.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - Tests C++ IsSimpleOp directly; uses an isolated GeometryFactory per test.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/valid/IsSimpleOp.cpp

#include "third_party/geos/include/geos/operation/valid/IsSimpleOp.h"

#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/BoundaryNodeRule.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::algorithm::BoundaryNodeRule;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::io::WKTReader;
using geos::operation::valid::IsSimpleOp;
using geos::util::UnsupportedOperationException;
using ::testing::AnyOf;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::SizeIs;
using ::testing::UnorderedElementsAre;

struct WktSimplicityCase {
  std::string_view name;
  std::string_view wkt;
  double expected_x = 0.0;
  double expected_y = 0.0;
};

constexpr auto kEmptyWktCases = std::to_array<std::string_view>({
    "POINT EMPTY",
    "MULTIPOINT EMPTY",
    "MULTIPOINT (EMPTY, EMPTY)",
    "LINESTRING EMPTY",
    "LINEARRING EMPTY",
    "POLYGON EMPTY",
    "MULTILINESTRING EMPTY",
    "MULTILINESTRING (EMPTY, EMPTY)",
    "MULTIPOLYGON EMPTY",
    "GEOMETRYCOLLECTION EMPTY",
    "GEOMETRYCOLLECTION (POINT EMPTY, LINESTRING EMPTY)",
    "CIRCULARSTRING EMPTY",
    "COMPOUNDCURVE EMPTY",
    "CURVEPOLYGON EMPTY",
    "MULTICURVE EMPTY",
    "MULTISURFACE EMPTY",
});

constexpr auto kSimpleLinearCases = std::to_array<WktSimplicityCase>({
    {.name = "single_segment", .wkt = "LINESTRING (0 0, 10 0)"},
    {.name = "two_adjacent_segments", .wkt = "LINESTRING (0 0, 5 5, 10 0)"},
    {.name = "adjacent_segindex_increasing",
     .wkt = "LINESTRING (0 0, 10 0, 5 5)"},
    {.name = "adjacent_segindex_decreasing",
     .wkt = "LINESTRING (10 10, 0 0, 5 2)"},
    {.name = "reverse_x_quadrants", .wkt = "LINESTRING (10 10, 0 0, 5 10)"},
    {.name = "forward_x_quadrants", .wkt = "LINESTRING (0 0, 10 10, 5 0)"},
    {.name = "overlapping_envelopes_disjoint_segs",
     .wkt = "LINESTRING (0 0, 10 10, 2 8, 1 9)"},
    {.name = "closed_ring", .wkt = "LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)"},
    {.name = "consecutive_repeated_points",
     .wkt = "LINESTRING (0 0, 0 0, 5 5, 5 5, 10 0, 10 0)"},
    {.name = "single_repeated_pair", .wkt = "LINESTRING (2 3, 2 3)"},
    {.name = "all_points_identical", .wkt = "LINESTRING (2 3, 2 3, 2 3)"},
    {.name = "four_dimensional_simple",
     .wkt = "LINESTRING ZM (0 0 1 2, 10 0 3 4, 10 10 5 6)"},
    {.name = "multiline_open_meeting_at_endpoint",
     .wkt = "MULTILINESTRING ((0 0, 5 5), (5 5, 10 0))"},
    {.name = "multiline_meeting_at_start_endpoints",
     .wkt = "MULTILINESTRING ((0 0, 5 5), (0 0, 10 0))"},
    {.name = "multiline_meeting_at_end_endpoints",
     .wkt = "MULTILINESTRING ((0 0, 5 5), (10 0, 5 5))"},
    {.name = "multiline_two_open_forming_loop",
     .wkt = "MULTILINESTRING ((0 0, 10 0, 10 10), (0 0, 0 10, 10 10))"},
    {.name = "multiline_three_open_at_endpoint",
     .wkt = "MULTILINESTRING ((0 0, 5 5), (5 5, 10 0), (5 5, 5 10))"},
    {.name = "multiline_overlapping_envelopes_disjoint",
     .wkt = "MULTILINESTRING ((0 0, 10 10), (1 8, 2 9))"},
    {.name = "multiline_with_empty_component",
     .wkt = "MULTILINESTRING (EMPTY, (0 0, 10 0))"},
    {.name = "multiline_multiseg_meeting_at_end_endpoints",
     .wkt = "MULTILINESTRING ((0 0, 5 0, 10 5), (20 0, 10 5))"},
    {.name = "multiline_single_repeated_pt_and_segment",
     .wkt = "MULTILINESTRING ((2 3, 2 3), (0 0, 10 0))"},
    {.name = "closed_ring_with_repeated_points",
     .wkt = "LINESTRING (0 0, 0 0, 10 0, 10 10, 0 10, 0 0)"},
});

constexpr auto kNonSimplePointLocationCases = std::to_array<WktSimplicityCase>({
    {.name = "linestring_interior_crossing",
     .wkt = "LINESTRING (0 0, 10 10, 10 0, 0 10)",
     .expected_x = 5.0,
     .expected_y = 5.0},
    {.name = "linestring_z_differs_still_crosses_2d",
     .wkt = "LINESTRING Z (0 0 0, 10 10 0, 10 0 100, 0 10 100)",
     .expected_x = 5.0,
     .expected_y = 5.0},
    {.name = "linestring_repeated_pts_with_crossing",
     .wkt = "LINESTRING (0 0, 0 0, 10 10, 10 10, 10 0, 0 10)",
     .expected_x = 5.0,
     .expected_y = 5.0},
    {.name = "linestring_end_touches_segment_interior",
     .wkt = "LINESTRING (0 0, 10 0, 10 5, 5 5, 5 0)",
     .expected_x = 5.0,
     .expected_y = 0.0},
    {.name = "linestring_start_touches_segment_interior",
     .wkt = "LINESTRING (5 0, 5 5, 0 5, 0 0, 10 0)",
     .expected_x = 5.0,
     .expected_y = 0.0},
    {.name = "linestring_end_touches_interior_vertex",
     .wkt = "LINESTRING (0 0, 5 0, 10 0, 10 5, 5 5, 5 0)",
     .expected_x = 5.0,
     .expected_y = 0.0},
    {.name = "linestring_start_touches_interior_vertex",
     .wkt = "LINESTRING (5 0, 5 5, 10 5, 10 0, 5 0, 0 0)",
     .expected_x = 5.0,
     .expected_y = 0.0},
    {.name = "linestring_interior_vertices_touch",
     .wkt = "LINESTRING (0 0, 5 5, 10 0, 10 10, 5 5, 0 10, 0 0)",
     .expected_x = 5.0,
     .expected_y = 5.0},
    {.name = "multiline_interior_crossing",
     .wkt = "MULTILINESTRING ((0 0, 10 10), (0 10, 10 0))",
     .expected_x = 5.0,
     .expected_y = 5.0},
    {.name = "multiline_t_intersection",
     .wkt = "MULTILINESTRING ((0 5, 10 5), (5 0, 5 5))",
     .expected_x = 5.0,
     .expected_y = 5.0},
    {.name = "multiline_start_at_interior_vertex",
     .wkt = "MULTILINESTRING ((0 0, 5 5, 10 0), (5 5, 5 10))",
     .expected_x = 5.0,
     .expected_y = 5.0},
    {.name = "multiline_end_at_interior_vertex",
     .wkt = "MULTILINESTRING ((0 0, 5 5, 10 0), (5 10, 5 5))",
     .expected_x = 5.0,
     .expected_y = 5.0},
    {.name = "multiline_interior_vertices_touch",
     .wkt = "MULTILINESTRING ((0 0, 5 5, 10 0), (0 10, 5 5, 10 10))",
     .expected_x = 5.0,
     .expected_y = 5.0},
});

constexpr auto kUnsupportedCurvedWkts = std::to_array<std::string_view>({
    "CIRCULARSTRING (0 0, 1 1, 2 0)",
    "COMPOUNDCURVE ((0 0, 1 1), CIRCULARSTRING (1 1, 2 2, 3 1))",
    "CURVEPOLYGON (CIRCULARSTRING (0 0, 2 2, 4 0, 2 -2, 0 0))",
    "MULTICURVE ((0 0, 1 1), CIRCULARSTRING (2 0, 3 1, 4 0))",
    "MULTISURFACE (CURVEPOLYGON (CIRCULARSTRING (0 0, 2 2, 4 0, 2 -2, 0 0)))",
    "GEOMETRYCOLLECTION (POINT (0 0), CIRCULARSTRING (0 0, 1 1, 2 0))",
});

class IsSimpleOpTest : public ::testing::Test {
 protected:
  IsSimpleOpTest()
      : factory_(GeometryFactory::create()), reader_(factory_.get()) {}

  std::unique_ptr<Geometry> ReadWkt(std::string_view wkt) const {
    return reader_.read(std::string(wkt));
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(IsSimpleOpTest, EmptyGeometriesAreSimple) {
  for (std::string_view wkt : kEmptyWktCases) {
    SCOPED_TRACE(wkt);
    std::unique_ptr<Geometry> geom = ReadWkt(wkt);
    IsSimpleOp op(geom.get());
    EXPECT_TRUE(op.isSimple());

    CoordinateXY loc = op.getNonSimpleLocation();
    EXPECT_TRUE(loc.isNull());
    EXPECT_TRUE(std::isnan(loc.x));
    EXPECT_TRUE(std::isnan(loc.y));
    EXPECT_THAT(op.getNonSimpleLocations(), IsEmpty());

    EXPECT_TRUE(IsSimpleOp::isSimple(*geom));
    EXPECT_TRUE(IsSimpleOp::isSimple(geom.get()));
    EXPECT_TRUE(geom->isSimple());
  }
}

TEST_F(IsSimpleOpTest, StaticPointerIsSimpleHandlesNullAndValid) {
  const Geometry* null_geom = nullptr;
  EXPECT_FALSE(IsSimpleOp::isSimple(null_geom));

  std::unique_ptr<Geometry> simple = ReadWkt("POINT (1 2)");
  std::unique_ptr<Geometry> non_simple =
      ReadWkt("LINESTRING (0 0, 10 10, 10 0, 0 10)");
  EXPECT_TRUE(IsSimpleOp::isSimple(simple.get()));
  EXPECT_FALSE(IsSimpleOp::isSimple(non_simple.get()));
}

TEST_F(IsSimpleOpTest, PointsAreAlwaysSimple) {
  constexpr auto kPointWkts = std::to_array<std::string_view>({
      "POINT (0 0)",
      "POINT (-3.5 7.25)",
      "POINT Z (1 2 3)",
      "POINT M (1 2 4)",
      "POINT ZM (1 2 3 4)",
  });

  for (std::string_view wkt : kPointWkts) {
    SCOPED_TRACE(wkt);
    std::unique_ptr<Geometry> pt = ReadWkt(wkt);
    IsSimpleOp op(*pt);
    EXPECT_TRUE(op.isSimple());
    EXPECT_TRUE(op.getNonSimpleLocation().isNull());
    EXPECT_THAT(op.getNonSimpleLocations(), IsEmpty());
  }
}

TEST_F(IsSimpleOpTest, MultiPointSimpleCases) {
  constexpr auto kSimpleMultiPointWkts = std::to_array<std::string_view>({
      "MULTIPOINT ((0 0))",
      "MULTIPOINT ((0 0), (1 0), (0 1))",
      "MULTIPOINT (EMPTY, (1 2), (3 4))",
  });

  for (std::string_view wkt : kSimpleMultiPointWkts) {
    SCOPED_TRACE(wkt);
    std::unique_ptr<Geometry> mp = ReadWkt(wkt);
    IsSimpleOp op(*mp);
    EXPECT_TRUE(op.isSimple());
    EXPECT_TRUE(op.getNonSimpleLocation().isNull());
    EXPECT_THAT(op.getNonSimpleLocations(), IsEmpty());
  }
}

TEST_F(IsSimpleOpTest, MultiPointNonSimpleDetects2DDuplicates) {
  constexpr auto kDupMultiPointCases = std::to_array<WktSimplicityCase>({
      {.name = "duplicate_2d",
       .wkt = "MULTIPOINT ((1 2), (3 4), (1 2))",
       .expected_x = 1.0,
       .expected_y = 2.0},
      {.name = "skips_empty_points_before_duplicate",
       .wkt = "MULTIPOINT (EMPTY, (5 6), EMPTY, (5 6))",
       .expected_x = 5.0,
       .expected_y = 6.0},
      {.name = "ignores_distinct_z",
       .wkt = "MULTIPOINT Z ((2 3 0), (2 3 10))",
       .expected_x = 2.0,
       .expected_y = 3.0},
      {.name = "ignores_distinct_m",
       .wkt = "MULTIPOINT M ((2 3 1), (2 3 2))",
       .expected_x = 2.0,
       .expected_y = 3.0},
  });

  for (const auto& test_case : kDupMultiPointCases) {
    SCOPED_TRACE(test_case.name);
    std::unique_ptr<Geometry> mp = ReadWkt(test_case.wkt);
    IsSimpleOp op(*mp);
    EXPECT_FALSE(op.isSimple());
    EXPECT_EQ(op.getNonSimpleLocation(),
              CoordinateXY(test_case.expected_x, test_case.expected_y));
  }
}

TEST_F(IsSimpleOpTest, MultiPointFindAllLocations) {
  std::unique_ptr<Geometry> mp =
      ReadWkt("MULTIPOINT ((1 2), (3 4), (1 2), (3 4))");

  IsSimpleOp op_first(*mp);
  op_first.setFindAllLocations(false);
  EXPECT_FALSE(op_first.isSimple());
  EXPECT_THAT(op_first.getNonSimpleLocations(),
              ElementsAre(CoordinateXY(1, 2)));

  IsSimpleOp op_all(*mp);
  op_all.setFindAllLocations(true);
  EXPECT_FALSE(op_all.isSimple());
  EXPECT_THAT(op_all.getNonSimpleLocations(),
              UnorderedElementsAre(CoordinateXY(1, 2), CoordinateXY(3, 4)));
}

TEST_F(IsSimpleOpTest, LinearGeometriesSimple) {
  for (const auto& test_case : kSimpleLinearCases) {
    SCOPED_TRACE(test_case.name);
    std::unique_ptr<Geometry> geom = ReadWkt(test_case.wkt);
    IsSimpleOp op(*geom);
    EXPECT_TRUE(op.isSimple());
    EXPECT_TRUE(op.getNonSimpleLocation().isNull());
    EXPECT_THAT(op.getNonSimpleLocations(), IsEmpty());
  }
}

TEST_F(IsSimpleOpTest, LinearGeometriesNonSimpleAtKnownPoint) {
  for (const auto& test_case : kNonSimplePointLocationCases) {
    SCOPED_TRACE(test_case.name);
    std::unique_ptr<Geometry> geom = ReadWkt(test_case.wkt);
    IsSimpleOp op(*geom);
    EXPECT_FALSE(op.isSimple());
    EXPECT_EQ(op.getNonSimpleLocation(),
              CoordinateXY(test_case.expected_x, test_case.expected_y));
    EXPECT_THAT(
        op.getNonSimpleLocations(),
        ElementsAre(CoordinateXY(test_case.expected_x, test_case.expected_y)));
  }
}

TEST_F(IsSimpleOpTest, EqualAndOverlappingCollinearSegmentsAreNonSimple) {
  {
    SCOPED_TRACE("linestring_exact_backtrack");
    std::unique_ptr<Geometry> geom = ReadWkt("LINESTRING (0 0, 10 0, 0 0)");
    IsSimpleOp op(*geom);
    EXPECT_FALSE(op.isSimple());
    EXPECT_THAT(op.getNonSimpleLocation(),
                AnyOf(CoordinateXY(0, 0), CoordinateXY(10, 0)));
  }
  {
    SCOPED_TRACE("linestring_partial_collinear_overlap");
    std::unique_ptr<Geometry> geom = ReadWkt("LINESTRING (0 0, 10 0, 4 0)");
    IsSimpleOp op(*geom);
    EXPECT_FALSE(op.isSimple());
    EXPECT_THAT(op.getNonSimpleLocation(),
                AnyOf(CoordinateXY(4, 0), CoordinateXY(10, 0)));
  }
  {
    SCOPED_TRACE("multiline_identical_same_direction");
    std::unique_ptr<Geometry> geom =
        ReadWkt("MULTILINESTRING ((0 0, 10 0), (0 0, 10 0))");
    IsSimpleOp op(*geom);
    EXPECT_FALSE(op.isSimple());
    EXPECT_THAT(op.getNonSimpleLocation(),
                AnyOf(CoordinateXY(0, 0), CoordinateXY(10, 0)));
  }
  {
    SCOPED_TRACE("multiline_identical_opposite_direction");
    std::unique_ptr<Geometry> geom =
        ReadWkt("MULTILINESTRING ((0 0, 10 0), (10 0, 0 0))");
    IsSimpleOp op(*geom);
    EXPECT_FALSE(op.isSimple());
    EXPECT_THAT(op.getNonSimpleLocation(),
                AnyOf(CoordinateXY(0, 0), CoordinateXY(10, 0)));
  }
  {
    SCOPED_TRACE("multiline_shared_segment_in_longer_lines");
    std::unique_ptr<Geometry> geom =
        ReadWkt("MULTILINESTRING ((0 0, 10 0, 10 10), (0 10, 0 0, 10 0))");
    IsSimpleOp op(*geom);
    EXPECT_FALSE(op.isSimple());
    EXPECT_THAT(op.getNonSimpleLocation(),
                AnyOf(CoordinateXY(0, 0), CoordinateXY(10, 0)));
  }
  {
    SCOPED_TRACE("multiline_partial_collinear_overlap");
    std::unique_ptr<Geometry> geom =
        ReadWkt("MULTILINESTRING ((0 0, 10 0), (5 0, 15 0))");
    IsSimpleOp op(*geom);
    EXPECT_FALSE(op.isSimple());
    EXPECT_THAT(op.getNonSimpleLocation(),
                AnyOf(CoordinateXY(5, 0), CoordinateXY(10, 0)));
  }
}

TEST_F(IsSimpleOpTest, LineStringFindAllLocationsReturnsAllCrossings) {
  // Put the horizontal base segment below y=0 so its monotone chain has the
  // smallest Y-center in the STRtree and is queried first, allowing default
  // early-exit mode to stop after the first crossing before checking others.
  std::unique_ptr<Geometry> base_first =
      ReadWkt("LINESTRING (-20 -2, 10 -2, 8 4, 6 -4, 4 4, 2 -4)");

  IsSimpleOp op_first(*base_first);
  EXPECT_FALSE(op_first.isSimple());
  EXPECT_THAT(op_first.getNonSimpleLocations(), SizeIs(1));
  EXPECT_THAT(op_first.getNonSimpleLocation(),
              AnyOf(CoordinateXY(2.5, -2.0), CoordinateXY(5.5, -2.0),
                    CoordinateXY(6.5, -2.0)));

  IsSimpleOp op_all(*base_first);
  op_all.setFindAllLocations(true);
  EXPECT_FALSE(op_all.isSimple());
  EXPECT_THAT(
      op_all.getNonSimpleLocations(),
      UnorderedElementsAre(CoordinateXY(2.5, -2.0), CoordinateXY(5.5, -2.0),
                           CoordinateXY(6.5, -2.0)));

  std::unique_ptr<Geometry> zigzag =
      ReadWkt("LINESTRING (0 0, 10 0, 8 4, 6 -4, 4 4, 2 -4)");
  IsSimpleOp op_zigzag_first(*zigzag);
  EXPECT_FALSE(op_zigzag_first.isSimple());
  EXPECT_THAT(
      op_zigzag_first.getNonSimpleLocation(),
      AnyOf(CoordinateXY(3, 0), CoordinateXY(5, 0), CoordinateXY(7, 0)));

  IsSimpleOp op_zigzag_all(*zigzag);
  op_zigzag_all.setFindAllLocations(true);
  EXPECT_FALSE(op_zigzag_all.isSimple());
  EXPECT_THAT(op_zigzag_all.getNonSimpleLocations(),
              UnorderedElementsAre(CoordinateXY(3, 0), CoordinateXY(5, 0),
                                   CoordinateXY(7, 0)));
}

TEST_F(IsSimpleOpTest, MultiLineStringFindAllDeduplicatesCoincidentLocations) {
  std::unique_ptr<Geometry> three_at_center =
      ReadWkt("MULTILINESTRING ((0 0, 10 10), (0 10, 10 0), (5 0, 5 10))");
  IsSimpleOp op_single(*three_at_center);
  op_single.setFindAllLocations(true);
  EXPECT_FALSE(op_single.isSimple());
  EXPECT_THAT(op_single.getNonSimpleLocations(),
              ElementsAre(CoordinateXY(5, 5)));

  std::unique_ptr<Geometry> line_across_vertex =
      ReadWkt("MULTILINESTRING ((0 5, 10 5), (5 0, 5 5, 5 10))");
  IsSimpleOp op_vertex(*line_across_vertex);
  op_vertex.setFindAllLocations(true);
  EXPECT_FALSE(op_vertex.isSimple());
  EXPECT_THAT(op_vertex.getNonSimpleLocations(),
              ElementsAre(CoordinateXY(5, 5)));

  std::unique_ptr<Geometry> touching_interior_vertices =
      ReadWkt("MULTILINESTRING ((0 0, 5 5, 10 0), (0 10, 5 5, 10 10))");
  IsSimpleOp op_interior_vertices(*touching_interior_vertices);
  op_interior_vertices.setFindAllLocations(true);
  EXPECT_FALSE(op_interior_vertices.isSimple());
  EXPECT_THAT(op_interior_vertices.getNonSimpleLocations(),
              ElementsAre(CoordinateXY(5, 5)));

  std::unique_ptr<Geometry> endpoint_at_interior_start =
      ReadWkt("MULTILINESTRING ((0 0, 5 5, 10 0), (5 5, 5 10))");
  IsSimpleOp op_endpt_start(*endpoint_at_interior_start);
  op_endpt_start.setFindAllLocations(true);
  EXPECT_FALSE(op_endpt_start.isSimple());
  EXPECT_THAT(op_endpt_start.getNonSimpleLocations(),
              ElementsAre(CoordinateXY(5, 5)));

  std::unique_ptr<Geometry> endpoint_at_interior_end =
      ReadWkt("MULTILINESTRING ((0 0, 5 5, 10 0), (5 10, 5 5))");
  IsSimpleOp op_endpt_end(*endpoint_at_interior_end);
  op_endpt_end.setFindAllLocations(true);
  EXPECT_FALSE(op_endpt_end.isSimple());
  EXPECT_THAT(op_endpt_end.getNonSimpleLocations(),
              ElementsAre(CoordinateXY(5, 5)));

  std::unique_ptr<Geometry> self_touch_vertex =
      ReadWkt("LINESTRING (0 0, 5 5, 10 0, 10 10, 5 5, 0 10, 0 0)");
  IsSimpleOp op_self_vertex(*self_touch_vertex);
  op_self_vertex.setFindAllLocations(true);
  EXPECT_FALSE(op_self_vertex.isSimple());
  EXPECT_THAT(op_self_vertex.getNonSimpleLocations(),
              ElementsAre(CoordinateXY(5, 5)));

  std::unique_ptr<Geometry> two_clusters = ReadWkt(
      "MULTILINESTRING ((0 0, 10 10), (0 10, 10 0), (5 0, 5 10), "
      "(20 0, 30 10), (20 10, 30 0))");
  IsSimpleOp op_clusters(*two_clusters);
  op_clusters.setFindAllLocations(true);
  EXPECT_FALSE(op_clusters.isSimple());
  EXPECT_THAT(op_clusters.getNonSimpleLocations(),
              UnorderedElementsAre(CoordinateXY(5, 5), CoordinateXY(25, 5)));
}

TEST_F(IsSimpleOpTest, BoundaryNodeRulesControlClosedLineEndpoints) {
  std::unique_ptr<Geometry> single_ring =
      ReadWkt("LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  EXPECT_TRUE(IsSimpleOp(*single_ring, BoundaryNodeRule::getBoundaryRuleMod2())
                  .isSimple());
  EXPECT_TRUE(IsSimpleOp(*single_ring, BoundaryNodeRule::getBoundaryEndPoint())
                  .isSimple());

  constexpr auto kClosedEndpointTouches = std::to_array<WktSimplicityCase>({
      {.name = "closed_then_open_lower_y",
       .wkt = "MULTILINESTRING ((0 0, 10 0, 10 10, 0 10, 0 0), (0 0, -5 0))",
       .expected_x = 0.0,
       .expected_y = 0.0},
      {.name = "open_lower_y_then_closed",
       .wkt = "MULTILINESTRING ((-5 0, 0 0), (0 0, 10 0, 10 10, 0 10, 0 0))",
       .expected_x = 0.0,
       .expected_y = 0.0},
      {.name = "open_strictly_below_closed",
       .wkt = "MULTILINESTRING ((0 0, 0 -5), (0 0, 10 0, 10 10, 0 10, 0 0))",
       .expected_x = 0.0,
       .expected_y = 0.0},
      {.name = "closed_then_open_upper_y",
       .wkt = "MULTILINESTRING ((0 10, 0 0, 10 0, 10 10, 0 10), (0 10, 0 25))",
       .expected_x = 0.0,
       .expected_y = 10.0},
      {.name = "two_closed_rings_meeting_at_endpoints",
       .wkt = "MULTILINESTRING ((0 0, 10 0, 10 10, 0 10, 0 0), "
              "(0 0, -10 0, -10 -10, 0 -10, 0 0))",
       .expected_x = 0.0,
       .expected_y = 0.0},
  });

  for (const auto& test_case : kClosedEndpointTouches) {
    SCOPED_TRACE(test_case.name);
    std::unique_ptr<Geometry> geom = ReadWkt(test_case.wkt);
    const CoordinateXY expected(test_case.expected_x, test_case.expected_y);

    IsSimpleOp mod2(*geom, BoundaryNodeRule::getBoundaryRuleMod2());
    EXPECT_FALSE(mod2.isSimple());
    EXPECT_EQ(mod2.getNonSimpleLocation(), expected);

    IsSimpleOp ogc(*geom, BoundaryNodeRule::getBoundaryOGCSFS());
    EXPECT_FALSE(ogc.isSimple());
    EXPECT_EQ(ogc.getNonSimpleLocation(), expected);

    IsSimpleOp monovalent(*geom,
                          BoundaryNodeRule::getBoundaryMonovalentEndPoint());
    EXPECT_FALSE(monovalent.isSimple());
    EXPECT_EQ(monovalent.getNonSimpleLocation(), expected);

    IsSimpleOp endpoint(*geom, BoundaryNodeRule::getBoundaryEndPoint());
    EXPECT_TRUE(endpoint.isSimple());
    EXPECT_TRUE(endpoint.getNonSimpleLocation().isNull());

    IsSimpleOp multivalent(*geom,
                           BoundaryNodeRule::getBoundaryMultivalentEndPoint());
    EXPECT_TRUE(multivalent.isSimple());
    EXPECT_TRUE(multivalent.getNonSimpleLocation().isNull());
  }
}

TEST_F(IsSimpleOpTest, BoundaryEndPointRuleStillRejectsInteriorRingVertex) {
  std::unique_ptr<Geometry> touch_interior =
      ReadWkt("MULTILINESTRING ((0 0, 10 0, 10 10, 0 10, 0 0), (10 0, 15 0))");

  IsSimpleOp mod2(*touch_interior, BoundaryNodeRule::getBoundaryRuleMod2());
  EXPECT_FALSE(mod2.isSimple());
  EXPECT_EQ(mod2.getNonSimpleLocation(), CoordinateXY(10, 0));

  IsSimpleOp endpoint(*touch_interior, BoundaryNodeRule::getBoundaryEndPoint());
  EXPECT_FALSE(endpoint.isSimple());
  EXPECT_EQ(endpoint.getNonSimpleLocation(), CoordinateXY(10, 0));
}

TEST_F(IsSimpleOpTest, BoundaryNodeRuleWithFindAllLocations) {
  std::unique_ptr<Geometry> two_closed_touches = ReadWkt(
      "MULTILINESTRING ((0 0, 10 0, 10 10, 0 10, 0 0), (0 0, -5 0), "
      "(20 0, 30 0, 30 10, 20 10, 20 0), (20 0, 15 0))");

  IsSimpleOp mod2(*two_closed_touches, BoundaryNodeRule::getBoundaryRuleMod2());
  mod2.setFindAllLocations(true);
  EXPECT_FALSE(mod2.isSimple());
  EXPECT_THAT(mod2.getNonSimpleLocations(),
              UnorderedElementsAre(CoordinateXY(0, 0), CoordinateXY(20, 0)));

  IsSimpleOp endpoint(*two_closed_touches,
                      BoundaryNodeRule::getBoundaryEndPoint());
  endpoint.setFindAllLocations(true);
  EXPECT_TRUE(endpoint.isSimple());
  EXPECT_THAT(endpoint.getNonSimpleLocations(), IsEmpty());
}

TEST_F(IsSimpleOpTest, PolygonalGeometriesSimpleWhenAllRingsAreSimple) {
  constexpr auto kSimplePolygonalWkts = std::to_array<std::string_view>({
      "LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)",
      "LINEARRING (0 0, 0 0, 10 0, 10 10, 0 10, 0 0)",
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(2 2, 2 8, 8 8, 8 2, 2 2))",
      // Each ring is individually simple even though shell and hole cross.
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(5 5, 15 5, 15 15, 5 15, 5 5))",
      "MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "((5 5, 15 5, 15 15, 5 15, 5 5)))",
      "MULTIPOLYGON (EMPTY, ((0 0, 5 0, 5 5, 0 5, 0 0)))",
  });

  for (std::string_view wkt : kSimplePolygonalWkts) {
    SCOPED_TRACE(wkt);
    std::unique_ptr<Geometry> geom = ReadWkt(wkt);
    IsSimpleOp op(*geom);
    EXPECT_TRUE(op.isSimple());
    EXPECT_TRUE(op.getNonSimpleLocation().isNull());
  }
}

TEST_F(IsSimpleOpTest, PolygonalGeometriesNonSimpleAndFindAll) {
  std::unique_ptr<Geometry> bowtie_ring =
      ReadWkt("LINEARRING (0 0, 10 10, 10 0, 0 10, 0 0)");
  IsSimpleOp ring_op(*bowtie_ring);
  EXPECT_FALSE(ring_op.isSimple());
  EXPECT_EQ(ring_op.getNonSimpleLocation(), CoordinateXY(5, 5));

  std::unique_ptr<Geometry> bowtie_hole = ReadWkt(
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
      "(2 2, 8 8, 8 2, 2 8, 2 2))");
  IsSimpleOp hole_op(*bowtie_hole);
  EXPECT_FALSE(hole_op.isSimple());
  EXPECT_EQ(hole_op.getNonSimpleLocation(), CoordinateXY(5, 5));

  std::unique_ptr<Geometry> two_bowtie_rings = ReadWkt(
      "POLYGON ((0 0, 20 20, 20 0, 0 20, 0 0), "
      "(2 12, 8 18, 8 12, 2 18, 2 12))");
  IsSimpleOp poly_first(*two_bowtie_rings);
  EXPECT_FALSE(poly_first.isSimple());
  EXPECT_THAT(poly_first.getNonSimpleLocations(),
              ElementsAre(CoordinateXY(10, 10)));

  IsSimpleOp poly_all(*two_bowtie_rings);
  poly_all.setFindAllLocations(true);
  EXPECT_FALSE(poly_all.isSimple());
  EXPECT_THAT(poly_all.getNonSimpleLocations(),
              UnorderedElementsAre(CoordinateXY(10, 10), CoordinateXY(5, 15)));

  std::unique_ptr<Geometry> multi_cross_ring =
      ReadWkt("LINEARRING (-20 -2, 10 -2, 8 4, 6 -4, 4 4, 2 -4, -20 -2)");
  IsSimpleOp ring_all(*multi_cross_ring);
  ring_all.setFindAllLocations(true);
  EXPECT_FALSE(ring_all.isSimple());
  EXPECT_THAT(
      ring_all.getNonSimpleLocations(),
      UnorderedElementsAre(CoordinateXY(2.5, -2.0), CoordinateXY(5.5, -2.0),
                           CoordinateXY(6.5, -2.0)));

  std::unique_ptr<Geometry> multi_poly = ReadWkt(
      "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
      "((10 0, 20 10, 20 0, 10 10, 10 0)), "
      "((30 0, 40 10, 40 0, 30 10, 30 0)))");
  IsSimpleOp mp_first(*multi_poly);
  EXPECT_FALSE(mp_first.isSimple());
  EXPECT_THAT(mp_first.getNonSimpleLocations(),
              ElementsAre(CoordinateXY(15, 5)));

  IsSimpleOp mp_all(*multi_poly);
  mp_all.setFindAllLocations(true);
  EXPECT_FALSE(mp_all.isSimple());
  EXPECT_THAT(mp_all.getNonSimpleLocations(),
              UnorderedElementsAre(CoordinateXY(15, 5), CoordinateXY(35, 5)));
}

TEST_F(IsSimpleOpTest, GeometryCollectionEvaluatesComponentsIndependently) {
  std::unique_ptr<Geometry> simple_gc = ReadWkt(
      "GEOMETRYCOLLECTION (POINT (1 1), POINT EMPTY, CIRCULARSTRING EMPTY, "
      "MULTISURFACE EMPTY, LINESTRING (0 0, 10 10), LINESTRING (0 10, 10 0), "
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0)))");
  IsSimpleOp simple_op(*simple_gc);
  EXPECT_TRUE(simple_op.isSimple());
  EXPECT_TRUE(simple_op.getNonSimpleLocation().isNull());

  std::unique_ptr<Geometry> non_simple_gc = ReadWkt(
      "GEOMETRYCOLLECTION (MULTIPOINT ((1 1), (1 1)), "
      "LINESTRING (10 0, 20 10, 20 0, 10 10), "
      "GEOMETRYCOLLECTION (POLYGON ((30 0, 40 10, 40 0, 30 10, 30 0))))");

  IsSimpleOp first_op(*non_simple_gc);
  EXPECT_FALSE(first_op.isSimple());
  EXPECT_THAT(first_op.getNonSimpleLocations(),
              ElementsAre(CoordinateXY(1, 1)));

  IsSimpleOp all_op(*non_simple_gc);
  all_op.setFindAllLocations(true);
  EXPECT_FALSE(all_op.isSimple());
  EXPECT_THAT(all_op.getNonSimpleLocations(),
              UnorderedElementsAre(CoordinateXY(1, 1), CoordinateXY(15, 5),
                                   CoordinateXY(35, 5)));
}

TEST_F(IsSimpleOpTest, NonEmptyCurvedTypesThrowUnsupportedOperation) {
  for (std::string_view wkt : kUnsupportedCurvedWkts) {
    SCOPED_TRACE(wkt);
    std::unique_ptr<Geometry> geom = ReadWkt(wkt);
    EXPECT_THROW(IsSimpleOp::isSimple(*geom), UnsupportedOperationException);
  }
}

TEST_F(IsSimpleOpTest, PartialComputationCacheAfterExceptionInCollection) {
  std::unique_ptr<Geometry> partial_gc = ReadWkt(
      "GEOMETRYCOLLECTION (LINESTRING (0 0, 10 10, 10 0, 0 10), "
      "CIRCULARSTRING (0 0, 1 1, 2 0))");
  IsSimpleOp op(*partial_gc);
  op.setFindAllLocations(true);
  EXPECT_THROW(op.isSimple(), UnsupportedOperationException);
  EXPECT_THAT(op.getNonSimpleLocations(), ElementsAre(CoordinateXY(5, 5)));
  EXPECT_FALSE(op.isSimple());
}

TEST_F(IsSimpleOpTest, MultiPointTripleDuplicateBehavior) {
  std::unique_ptr<Geometry> mp = ReadWkt("MULTIPOINT ((1 2), (1 2), (1 2))");
  IsSimpleOp op_first(*mp);
  EXPECT_FALSE(op_first.isSimple());
  EXPECT_THAT(op_first.getNonSimpleLocations(),
              ElementsAre(CoordinateXY(1, 2)));

  IsSimpleOp op_all(*mp);
  op_all.setFindAllLocations(true);
  EXPECT_FALSE(op_all.isSimple());
  EXPECT_THAT(op_all.getNonSimpleLocations(),
              ElementsAre(CoordinateXY(1, 2), CoordinateXY(1, 2)));
}

TEST_F(IsSimpleOpTest, CachesComputedResultAcrossQueries) {
  std::unique_ptr<Geometry> non_simple =
      ReadWkt("LINESTRING (0 0, 10 10, 10 0, 0 10)");
  IsSimpleOp op1(*non_simple);
  EXPECT_FALSE(op1.isSimple());
  EXPECT_EQ(op1.getNonSimpleLocation(), CoordinateXY(5, 5));
  EXPECT_THAT(op1.getNonSimpleLocations(), ElementsAre(CoordinateXY(5, 5)));
  EXPECT_FALSE(op1.isSimple());

  IsSimpleOp op2(*non_simple);
  const std::vector<CoordinateXY>& locs = op2.getNonSimpleLocations();
  EXPECT_THAT(locs, ElementsAre(CoordinateXY(5, 5)));
  EXPECT_EQ(op2.getNonSimpleLocation(), CoordinateXY(5, 5));
  EXPECT_FALSE(op2.isSimple());

  std::unique_ptr<Geometry> simple = ReadWkt("LINESTRING (0 0, 1 1)");
  IsSimpleOp op3(*simple);
  EXPECT_TRUE(op3.getNonSimpleLocation().isNull());
  EXPECT_THAT(op3.getNonSimpleLocations(), IsEmpty());
  EXPECT_TRUE(op3.isSimple());
}

TEST_F(IsSimpleOpTest, GetNonSimpleLocationOverloadTestsOtherGeometry) {
  std::unique_ptr<Geometry> base_simple = ReadWkt("POINT (0 0)");
  std::unique_ptr<Geometry> other_non_simple =
      ReadWkt("LINESTRING (0 0, 10 10, 10 0, 0 10)");
  std::unique_ptr<Geometry> other_simple = ReadWkt("LINESTRING (0 0, 1 1)");

  EXPECT_EQ(IsSimpleOp(*other_non_simple).getNonSimpleLocation(),
            CoordinateXY(5, 5));
  EXPECT_TRUE(IsSimpleOp(*other_simple).getNonSimpleLocation().isNull());

  IsSimpleOp op(*base_simple);
  EXPECT_EQ(op.getNonSimpleLocation(*other_non_simple), CoordinateXY(5, 5));
  EXPECT_TRUE(op.getNonSimpleLocation(*other_simple).isNull());
  EXPECT_TRUE(op.isSimple());
}

}  // namespace
}  // namespace geos_google
