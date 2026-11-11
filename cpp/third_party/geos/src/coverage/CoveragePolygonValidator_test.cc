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

// Unit tests for third_party/geos/src/coverage/CoveragePolygonValidator.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/coverage/CoveragePolygonValidator.cpp

#include "third_party/geos/include/geos/coverage/CoveragePolygonValidator.h"

#include <array>
#include <cstddef>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::coverage::CoveragePolygonValidator;
using geos::geom::CoordinateSequence;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GeometryTypeId;
using geos::geom::LinearRing;
using geos::geom::Polygon;
using geos::io::WKTReader;

struct ValidCase {
  std::string_view name;
  std::string_view target_wkt;
  std::array<std::string_view, 4> adj_wkts;
  std::size_t adj_count;
};

struct InvalidCase {
  std::string_view name;
  std::string_view target_wkt;
  std::array<std::string_view, 2> adj_wkts;
  std::size_t adj_count;
  std::string_view expected_wkt;
  GeometryTypeId expected_type;
  std::size_t expected_num_geoms;
  double expected_length;
};

constexpr auto kValidCases = std::to_array<ValidCase>({
    {
        .name = "single_polygon_no_adjacent",
        .target_wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .adj_wkts = {},
        .adj_count = 0,
    },
    {
        .name = "surrounded_on_all_four_sides",
        .target_wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .adj_wkts =
            {
                "POLYGON ((-10 0, 0 0, 0 10, -10 10, -10 0))",
                "POLYGON ((0 10, 10 10, 10 20, 0 20, 0 10))",
                "POLYGON ((10 0, 20 0, 20 10, 10 10, 10 0))",
                "POLYGON ((0 -10, 10 -10, 10 0, 0 0, 0 -10))",
            },
        .adj_count = 4,
    },
    {
        .name = "partial_boundary_with_distant_disjoint",
        .target_wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .adj_wkts =
            {
                "POLYGON ((10 0, 20 0, 20 10, 10 10, 10 0))",
                "POLYGON ((100 100, 110 100, 110 110, 100 110, 100 100))",
            },
        .adj_count = 2,
    },
    {
        .name = "cw_target_and_ccw_adjacent",
        .target_wkt = "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))",
        .adj_wkts = {"POLYGON ((10 0, 20 0, 20 10, 10 10, 10 0))"},
        .adj_count = 1,
    },
    {
        .name = "ccw_target_and_cw_adjacent",
        .target_wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .adj_wkts = {"POLYGON ((10 0, 10 10, 20 10, 20 0, 10 0))"},
        .adj_count = 1,
    },
    {
        .name = "both_cw_rings",
        .target_wkt = "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))",
        .adj_wkts = {"POLYGON ((0 10, 0 20, 10 20, 10 10, 0 10))"},
        .adj_count = 1,
    },
    {
        .name = "target_with_cw_hole_filled_by_adjacent",
        .target_wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 8, 8 8, "
                      "8 2, 2 2))",
        .adj_wkts = {"POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))"},
        .adj_count = 1,
    },
    {
        .name = "target_with_ccw_hole_filled_by_adjacent",
        .target_wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 8 2, 8 8, "
                      "2 8, 2 2))",
        .adj_wkts = {"POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))"},
        .adj_count = 1,
    },
    {
        .name = "target_fills_adjacent_hole",
        .target_wkt = "POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))",
        .adj_wkts =
            {
                "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
                "(2 2, 2 8, 8 8, 8 2, 2 2))",
            },
        .adj_count = 1,
    },
    {
        .name = "three_polygons_meeting_at_shared_edge_node",
        .target_wkt = "POLYGON ((0 0, 10 0, 10 5, 10 10, 0 10, 0 0))",
        .adj_wkts =
            {
                "POLYGON ((10 0, 20 0, 20 5, 10 5, 10 0))",
                "POLYGON ((10 5, 20 5, 20 10, 10 10, 10 5))",
            },
        .adj_count = 2,
    },
    {
        .name = "repeated_vertices_in_target_and_adjacent",
        .target_wkt = "POLYGON ((0 0, 10 0, 10 0, 10 10, 0 10, 0 0))",
        .adj_wkts = {"POLYGON ((10 0, 20 0, 20 10, 10 10, 10 10, 10 0))"},
        .adj_count = 1,
    },
    {
        .name = "empty_target_polygon",
        .target_wkt = "POLYGON EMPTY",
        .adj_wkts = {"POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))"},
        .adj_count = 1,
    },
    {
        .name = "non_polygon_and_empty_in_adjacent",
        .target_wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .adj_wkts =
            {
                "POINT (5 5)",
                "LINESTRING (0 0, 10 10)",
                "POLYGON EMPTY",
                "POLYGON ((10 0, 20 0, 20 10, 10 10, 10 0))",
            },
        .adj_count = 4,
    },
    {
        .name = "multipolygon_target_and_adjacent",
        .target_wkt = "MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0)), "
                      "((20 0, 30 0, 30 10, 20 10, 20 0)))",
        .adj_wkts =
            {
                "MULTIPOLYGON (((10 0, 20 0, 20 10, 10 10, 10 0)), "
                "((30 0, 40 0, 40 10, 30 10, 30 0)))",
            },
        .adj_count = 1,
    },
    {
        .name = "geometry_collection_target_and_adjacent",
        .target_wkt = "GEOMETRYCOLLECTION (POINT (100 100), "
                      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)))",
        .adj_wkts =
            {
                "GEOMETRYCOLLECTION (LINESTRING (0 0, 5 5), "
                "POLYGON ((10 0, 20 0, 20 10, 10 10, 10 0)))",
            },
        .adj_count = 1,
    },
    {
        .name = "empty_geometry_collection_target",
        .target_wkt = "GEOMETRYCOLLECTION EMPTY",
        .adj_wkts = {"POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))"},
        .adj_count = 1,
    },
    {
        .name = "polygon_with_z_coordinates",
        .target_wkt = "POLYGON Z ((0 0 1, 10 0 2, 10 10 3, 0 10 4, 0 0 1))",
        .adj_wkts = {"POLYGON Z ((10 0 5, 20 0 6, 20 10 7, 10 10 8, 10 0 5))"},
        .adj_count = 1,
    },
});

constexpr auto kInvalidCases = std::to_array<InvalidCase>({
    {
        .name = "duplicate_polygon_ccw",
        .target_wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .adj_wkts = {"POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))"},
        .adj_count = 1,
        .expected_wkt = "LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)",
        .expected_type = GeometryTypeId::GEOS_LINESTRING,
        .expected_num_geoms = 1,
        .expected_length = 40.0,
    },
    {
        .name = "duplicate_polygon_reversed_cw_orientation",
        .target_wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .adj_wkts = {"POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))"},
        .adj_count = 1,
        .expected_wkt = "LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)",
        .expected_type = GeometryTypeId::GEOS_LINESTRING,
        .expected_num_geoms = 1,
        .expected_length = 40.0,
    },
    {
        .name = "three_polygon_stack_all_segments_matched_and_invalid",
        .target_wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .adj_wkts =
            {
                "POLYGON ((-10 -10, 20 -10, 20 20, -10 20, -10 -10), "
                "(0 0, 0 10, 10 10, 10 0, 0 0))",
                "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
            },
        .adj_count = 2,
        .expected_wkt = "LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)",
        .expected_type = GeometryTypeId::GEOS_LINESTRING,
        .expected_num_geoms = 1,
        .expected_length = 40.0,
    },
    {
        .name = "shared_edge_same_orientation_overlap",
        .target_wkt = "POLYGON ((0 0, 10 0, 5 10, 0 0))",
        .adj_wkts = {"POLYGON ((0 0, 10 0, 5 5, 0 0))"},
        .adj_count = 1,
        .expected_wkt = "LINESTRING (0 0, 10 0)",
        .expected_type = GeometryTypeId::GEOS_LINESTRING,
        .expected_num_geoms = 1,
        .expected_length = 10.0,
    },
    {
        .name = "collinear_partial_overlap_t_node",
        .target_wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .adj_wkts = {"POLYGON ((10 0, 20 0, 20 5, 10 5, 10 0))"},
        .adj_count = 1,
        .expected_wkt = "LINESTRING (10 0, 10 10)",
        .expected_type = GeometryTypeId::GEOS_LINESTRING,
        .expected_num_geoms = 1,
        .expected_length = 10.0,
    },
    {
        .name = "vertex_touches_segment_interior",
        .target_wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .adj_wkts = {"POLYGON ((10 5, 15 0, 15 10, 10 5))"},
        .adj_count = 1,
        .expected_wkt = "LINESTRING (10 0, 10 10)",
        .expected_type = GeometryTypeId::GEOS_LINESTRING,
        .expected_num_geoms = 1,
        .expected_length = 10.0,
    },
    {
        .name = "crossing_segments",
        .target_wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .adj_wkts = {"POLYGON ((5 2, 15 2, 15 8, 5 8, 5 2))"},
        .adj_count = 1,
        .expected_wkt = "LINESTRING (10 0, 10 10)",
        .expected_type = GeometryTypeId::GEOS_LINESTRING,
        .expected_num_geoms = 1,
        .expected_length = 10.0,
    },
    {
        .name = "shared_vertex_segment_enters_adjacent_interior",
        .target_wkt = "POLYGON ((0 10, 10 0, 15 15, 0 10))",
        .adj_wkts = {"POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))"},
        .adj_count = 1,
        .expected_wkt = "LINESTRING (0 10, 10 0)",
        .expected_type = GeometryTypeId::GEOS_LINESTRING,
        .expected_num_geoms = 1,
        .expected_length = 10.0 * std::numbers::sqrt2,
    },
    {
        .name = "target_completely_contained_in_adjacent",
        .target_wkt = "POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))",
        .adj_wkts = {"POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))"},
        .adj_count = 1,
        .expected_wkt = "LINESTRING (2 2, 8 2, 8 8, 2 8, 2 2)",
        .expected_type = GeometryTypeId::GEOS_LINESTRING,
        .expected_num_geoms = 1,
        .expected_length = 24.0,
    },
    {
        .name = "partial_interior_wrap_around_at_index_zero",
        .target_wkt = "POLYGON ((5 5, 7 5, 7 15, 5 15, 5 5))",
        .adj_wkts = {"POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))"},
        .adj_count = 1,
        .expected_wkt = "LINESTRING (5 15, 5 5, 7 5, 7 15)",
        .expected_type = GeometryTypeId::GEOS_LINESTRING,
        .expected_num_geoms = 1,
        .expected_length = 22.0,
    },
    {
        .name = "partial_interior_vertex_after_index_zero",
        .target_wkt = "POLYGON ((5 15, 5 5, 7 5, 7 15, 5 15))",
        .adj_wkts = {"POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))"},
        .adj_count = 1,
        .expected_wkt = "LINESTRING (5 15, 5 5, 7 5, 7 15)",
        .expected_type = GeometryTypeId::GEOS_LINESTRING,
        .expected_num_geoms = 1,
        .expected_length = 22.0,
    },
    {
        .name = "two_disconnected_invalid_edges_returns_multilinestring",
        .target_wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .adj_wkts =
            {
                "POLYGON ((-5 2, 0 2, 0 8, -5 8, -5 2))",
                "POLYGON ((10 2, 15 2, 15 8, 10 8, 10 2))",
            },
        .adj_count = 2,
        .expected_wkt = "MULTILINESTRING ((0 10, 0 0), (10 0, 10 10))",
        .expected_type = GeometryTypeId::GEOS_MULTILINESTRING,
        .expected_num_geoms = 2,
        .expected_length = 20.0,
    },
    {
        .name = "invalid_hole_boundary",
        .target_wkt = "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
                      "(5 5, 5 15, 15 15, 15 5, 5 5))",
        .adj_wkts = {"POLYGON ((5 5, 15 5, 15 10, 5 10, 5 5))"},
        .adj_count = 1,
        .expected_wkt = "MULTILINESTRING ((5 5, 5 15), (15 15, 15 5))",
        .expected_type = GeometryTypeId::GEOS_MULTILINESTRING,
        .expected_num_geoms = 2,
        .expected_length = 20.0,
    },
    {
        .name = "invalid_both_shell_and_hole",
        .target_wkt = "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
                      "(5 5, 5 15, 15 15, 15 5, 5 5))",
        .adj_wkts =
            {
                "POLYGON ((20 0, 30 0, 30 10, 20 10, 20 0))",
                "POLYGON ((8 5, 12 5, 12 10, 8 10, 8 5))",
            },
        .adj_count = 2,
        .expected_wkt = "MULTILINESTRING ((20 0, 20 20), (15 5, 5 5))",
        .expected_type = GeometryTypeId::GEOS_MULTILINESTRING,
        .expected_num_geoms = 2,
        .expected_length = 30.0,
    },
    {
        .name = "multipolygon_target_invalid_both_parts",
        .target_wkt = "MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0)), "
                      "((20 0, 30 0, 30 10, 20 10, 20 0)))",
        .adj_wkts = {"POLYGON ((10 2, 20 2, 20 8, 10 8, 10 2))"},
        .adj_count = 1,
        .expected_wkt = "MULTILINESTRING ((10 0, 10 10), (20 10, 20 0))",
        .expected_type = GeometryTypeId::GEOS_MULTILINESTRING,
        .expected_num_geoms = 2,
        .expected_length = 20.0,
    },
    {
        .name = "repeated_vertices_invalid_overlap",
        .target_wkt = "POLYGON ((0 0, 10 0, 10 0, 10 10, 0 10, 0 0))",
        .adj_wkts = {"POLYGON ((10 0, 20 0, 20 5, 10 5, 10 0))"},
        .adj_count = 1,
        .expected_wkt = "LINESTRING (10 0, 10 10)",
        .expected_type = GeometryTypeId::GEOS_LINESTRING,
        .expected_num_geoms = 1,
        .expected_length = 10.0,
    },
});

struct GeometryList {
  std::vector<Geometry::Ptr> owned;
  std::vector<const Geometry*> ptrs;
};

GeometryList ReadGeometries(const WKTReader& reader,
                            std::span<const std::string_view> wkts) {
  GeometryList list;
  list.owned.reserve(wkts.size());
  list.ptrs.reserve(wkts.size());
  for (std::string_view wkt : wkts) {
    list.owned.push_back(reader.read(std::string(wkt)));
    list.ptrs.push_back(list.owned.back().get());
  }
  return list;
}

void ExpectEmptyLineString(const Geometry* result) {
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isEmpty());
  EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_LINESTRING);
}

void ExpectMatchesLinework(const Geometry* result, const Geometry* expected,
                           GeometryTypeId expected_type,
                           std::size_t expected_num_geoms,
                           double expected_length) {
  ASSERT_NE(result, nullptr);
  ASSERT_NE(expected, nullptr);
  EXPECT_FALSE(result->isEmpty());
  EXPECT_EQ(result->getGeometryTypeId(), expected_type);
  EXPECT_EQ(result->getNumGeometries(), expected_num_geoms);
  EXPECT_NEAR(result->getLength(), expected_length, 1e-9);
  EXPECT_TRUE(result->equals(expected));
}

TEST(CoveragePolygonValidatorTest, ValidCoverageCases) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (const auto& tc : kValidCases) {
    SCOPED_TRACE(tc.name);
    Geometry::Ptr target = reader.read(std::string(tc.target_wkt));
    GeometryList adj = ReadGeometries(
        reader,
        std::span<const std::string_view>(tc.adj_wkts.data(), tc.adj_count));

    Geometry::Ptr result =
        CoveragePolygonValidator::validate(target.get(), adj.ptrs);
    ExpectEmptyLineString(result.get());
  }
}

TEST(CoveragePolygonValidatorTest, InvalidCoverageCases) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (const auto& tc : kInvalidCases) {
    SCOPED_TRACE(tc.name);
    Geometry::Ptr target = reader.read(std::string(tc.target_wkt));
    GeometryList adj = ReadGeometries(
        reader,
        std::span<const std::string_view>(tc.adj_wkts.data(), tc.adj_count));

    Geometry::Ptr expected = reader.read(std::string(tc.expected_wkt));
    Geometry::Ptr result =
        CoveragePolygonValidator::validate(target.get(), adj.ptrs);
    ExpectMatchesLinework(result.get(), expected.get(), tc.expected_type,
                          tc.expected_num_geoms, tc.expected_length);
  }
}

TEST(CoveragePolygonValidatorTest, GapWidthDetectionStaticAndInstance) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr target = reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  Geometry::Ptr adj_gap =
      reader.read("POLYGON ((0 10.5, 10 10.5, 10 20, 0 20, 0 10.5))");
  std::vector<const Geometry*> adj_ptrs = {adj_gap.get()};

  // Without gapWidth or with gapWidth smaller than gap (0.5), result is valid.
  ExpectEmptyLineString(
      CoveragePolygonValidator::validate(target.get(), adj_ptrs).get());
  ExpectEmptyLineString(
      CoveragePolygonValidator::validate(target.get(), adj_ptrs, 0.49).get());

  // With gapWidth >= 0.5, the narrow parallel gap is flagged.
  Geometry::Ptr expected_top = reader.read("LINESTRING (0 10, 10 10)");
  Geometry::Ptr static_result_exact =
      CoveragePolygonValidator::validate(target.get(), adj_ptrs, 0.5);
  ExpectMatchesLinework(static_result_exact.get(), expected_top.get(),
                        GeometryTypeId::GEOS_LINESTRING, 1, 10.0);

  Geometry::Ptr static_result_wider =
      CoveragePolygonValidator::validate(target.get(), adj_ptrs, 1.0);
  ExpectMatchesLinework(static_result_wider.get(), expected_top.get(),
                        GeometryTypeId::GEOS_LINESTRING, 1, 10.0);

  // Exercise instance constructor + setGapWidth + validate().
  CoveragePolygonValidator default_validator(target.get(), adj_ptrs);
  ExpectEmptyLineString(default_validator.validate().get());

  CoveragePolygonValidator gap_validator(target.get(), adj_ptrs);
  gap_validator.setGapWidth(1.0);
  Geometry::Ptr instance_result = gap_validator.validate();
  ExpectMatchesLinework(instance_result.get(), expected_top.get(),
                        GeometryTypeId::GEOS_LINESTRING, 1, 10.0);
}

TEST(CoveragePolygonValidatorTest, GapWidthGoreAndShortSegmentThreshold) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // Narrow gore starting at shared vertex (0, 10) and diverging to 0.4 width.
  Geometry::Ptr target = reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  Geometry::Ptr adj_gore =
      reader.read("POLYGON ((0 10, 10 10.4, 10 20, 0 20, 0 10))");
  std::vector<const Geometry*> gore_adj = {adj_gore.get()};

  ExpectEmptyLineString(
      CoveragePolygonValidator::validate(target.get(), gore_adj, 0.0).get());
  Geometry::Ptr expected_gore = reader.read("LINESTRING (0 10, 10 10)");
  Geometry::Ptr gore_result =
      CoveragePolygonValidator::validate(target.get(), gore_adj, 0.5);
  ExpectMatchesLinework(gore_result.get(), expected_gore.get(),
                        GeometryTypeId::GEOS_LINESTRING, 1, 10.0);

  // Segments whose overlapping projection length <= gapWidth are not flagged.
  Geometry::Ptr short_target =
      reader.read("POLYGON ((0 0, 0.5 0, 0.5 10, 0 10, 0 0))");
  Geometry::Ptr short_adj =
      reader.read("POLYGON ((0 10.4, 0.5 10.4, 0.5 20, 0 20, 0 10.4))");
  std::vector<const Geometry*> short_adj_ptrs = {short_adj.get()};
  ExpectEmptyLineString(CoveragePolygonValidator::validate(short_target.get(),
                                                           short_adj_ptrs, 0.6)
                            .get());
}

TEST(CoveragePolygonValidatorTest, LargeRingExceedingInteriorSectionStride) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // Build a rectangular polygon with 1202 segments (> stride 1000):
  // - Section 0 (indices 0..999): x in [200, 700] at y=0, (700, 10), and
  //   x in [202, 700] at y=10 (envelope [200, 700] x [0, 10]).
  // - Section 1 (indices 1000..1201): x in [100, 201] at y=10, (100, 0), and
  //   x in [100, 200] at y=0 (envelope [100, 201] x [0, 10]).
  auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
  seq->reserve(1203);
  for (int x = 200; x <= 700; ++x) {
    seq->add(static_cast<double>(x), 0.0);
  }
  for (int x = 700; x >= 100; --x) {
    seq->add(static_cast<double>(x), 10.0);
  }
  for (int x = 100; x <= 200; ++x) {
    seq->add(static_cast<double>(x), 0.0);
  }
  ASSERT_EQ(seq->size(), 1203u);

  std::unique_ptr<LinearRing> ring = factory->createLinearRing(std::move(seq));
  std::unique_ptr<Polygon> target = factory->createPolygon(std::move(ring));

  // adj1 intersects only section 0; adj2 intersects only section 1.
  Geometry::Ptr adj1 =
      reader.read("POLYGON ((300.5 -5, 400.5 -5, 400.5 5, 300.5 5, 300.5 -5))");
  Geometry::Ptr adj2 =
      reader.read("POLYGON ((120.5 5, 140.5 5, 140.5 15, 120.5 15, 120.5 5))");
  std::vector<const Geometry*> adj_ptrs = {adj1.get(), adj2.get()};

  Geometry::Ptr expected =
      reader.read("MULTILINESTRING ((300 0, 401 0), (141 10, 120 10))");
  Geometry::Ptr result =
      CoveragePolygonValidator::validate(target.get(), adj_ptrs);

  ExpectMatchesLinework(result.get(), expected.get(),
                        GeometryTypeId::GEOS_MULTILINESTRING, 2, 122.0);
  EXPECT_EQ(result->getNumPoints(), 124u);
}

TEST(CoveragePolygonValidatorTest, LargeRingValidWithDistantAdjacent) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // Valid > 1000-segment polygon exercised against a non-overlapping adjacent
  // polygon so all stride sections run with intersectsEnv(sectionEnv) == false.
  auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
  seq->reserve(1103);
  for (int x = 0; x <= 550; ++x) {
    seq->add(static_cast<double>(x), 0.0);
  }
  for (int x = 550; x >= 0; --x) {
    seq->add(static_cast<double>(x), 10.0);
  }
  seq->add(0.0, 0.0);
  ASSERT_EQ(seq->size(), 1103u);

  std::unique_ptr<LinearRing> ring = factory->createLinearRing(std::move(seq));
  std::unique_ptr<Polygon> target = factory->createPolygon(std::move(ring));

  Geometry::Ptr adj_disjoint =
      reader.read("POLYGON ((1000 0, 1010 0, 1010 10, 1000 10, 1000 0))");
  std::vector<const Geometry*> adj_ptrs = {adj_disjoint.get()};

  ExpectEmptyLineString(
      CoveragePolygonValidator::validate(target.get(), adj_ptrs).get());
}

}  // namespace
}  // namespace geos_google
