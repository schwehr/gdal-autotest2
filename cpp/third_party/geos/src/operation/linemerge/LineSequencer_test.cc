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

// Unit tests for third_party/geos/src/operation/linemerge/LineSequencer.cpp.
//
// google3 notes:
// - Uses isolated GeometryFactory::create() instances per test.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/linemerge/LineSequencer.cpp

#include "third_party/geos/include/geos/operation/linemerge/LineSequencer.h"

#include <array>
#include <cmath>
#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GeometryTypeId;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::MultiLineString;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::operation::linemerge::LineSequencer;

struct IsSequencedCase {
  std::string_view name;
  std::string_view wkt;
  bool expected;
};

struct SequenceCase {
  std::string_view name;
  std::string_view input_wkt;
  std::string_view expected_wkt;
  GeometryTypeId expected_type;
};

struct NonSequenceableCase {
  std::string_view name;
  std::string_view input_wkt;
};

constexpr auto kIsSequencedCases = std::to_array<IsSequencedCase>({
    {
        .name = "empty_multilinestring",
        .wkt = "MULTILINESTRING EMPTY",
        .expected = true,
    },
    {
        .name = "single_line_multilinestring",
        .wkt = "MULTILINESTRING ((0 0, 1 0))",
        .expected = true,
    },
    {
        .name = "chained_two_lines",
        .wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 2 0))",
        .expected = true,
    },
    {
        .name = "closed_chain",
        .wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 1 1), (1 1, 0 0))",
        .expected = true,
    },
    {
        .name = "two_disjoint_sequenced_subgraphs",
        .wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 2 0), "
               "(10 0, 11 0), (11 0, 12 0))",
        .expected = true,
    },
    {
        .name = "z_differs_at_2d_junction",
        .wkt = "MULTILINESTRING Z ((0 0 1, 1 0 2), (1 0 9, 2 0 3))",
        .expected = true,
    },
    {
        .name = "figure_eight_sequenced",
        .wkt = "MULTILINESTRING ((0 0, 1 1), (1 1, 0 1), (0 1, 0 0), "
               "(0 0, -1 -1), (-1 -1, -1 0), (-1 0, 0 0))",
        .expected = true,
    },
    {
        .name = "interleaved_subgraph_start_hits_previous",
        .wkt = "MULTILINESTRING ((0 0, 1 0), (10 0, 11 0), (1 0, 2 0))",
        .expected = false,
    },
    {
        .name = "interleaved_subgraph_end_hits_previous",
        .wkt = "MULTILINESTRING ((0 0, 1 0), (10 0, 11 0), (2 0, 1 0))",
        .expected = false,
    },
    {
        .name = "single_component_gap_start_hits_previous",
        .wkt = "MULTILINESTRING ((0 0, 1 0), (2 0, 3 0), (1 0, 2 0))",
        .expected = false,
    },
    {
        .name = "single_component_gap_end_hits_previous",
        .wkt = "MULTILINESTRING ((0 0, 1 0), (2 0, 3 0), (3 0, 1 0))",
        .expected = false,
    },
    {
        .name = "shared_interior_vertex_disjoint_endpoints",
        .wkt = "MULTILINESTRING ((0 0, 1 1, 2 0), (10 0, 1 1, 11 0))",
        .expected = true,
    },
    {
        .name = "self_loop_in_chain_sequenced",
        .wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 2 1, 2 -1, 1 0), "
               "(1 0, 2 0))",
        .expected = true,
    },
});

constexpr auto kSequenceCases = std::to_array<SequenceCase>({
    {
        .name = "single_linestring",
        .input_wkt = "LINESTRING (0 0, 1 0, 2 1)",
        .expected_wkt = "LINESTRING (0 0, 1 0, 2 1)",
        .expected_type = geos::geom::GEOS_LINESTRING,
    },
    {
        .name = "single_reversed_linestring_preserves_direction",
        .input_wkt = "LINESTRING (1 0, 0 0)",
        .expected_wkt = "LINESTRING (1 0, 0 0)",
        .expected_type = geos::geom::GEOS_LINESTRING,
    },
    {
        .name = "single_line_multilinestring",
        .input_wkt = "MULTILINESTRING ((0 0, 1 0))",
        .expected_wkt = "LINESTRING (0 0, 1 0)",
        .expected_type = geos::geom::GEOS_LINESTRING,
    },
    {
        .name = "closed_line_backward_de_not_reversed",
        .input_wkt = "LINESTRING (0 0, 1 1, 1 0, 0 0)",
        .expected_wkt = "LINESTRING (0 0, 1 1, 1 0, 0 0)",
        .expected_type = geos::geom::GEOS_LINESTRING,
    },
    {
        .name = "both_forward_out_of_order",
        .input_wkt = "MULTILINESTRING ((1 0, 2 0), (0 0, 1 0))",
        .expected_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 2 0))",
        .expected_type = geos::geom::GEOS_MULTILINESTRING,
    },
    {
        .name = "both_backward_flips_whole_sequence",
        .input_wkt = "MULTILINESTRING ((1 0, 0 0), (2 0, 1 0))",
        .expected_wkt = "MULTILINESTRING ((2 0, 1 0), (1 0, 0 0))",
        .expected_type = geos::geom::GEOS_MULTILINESTRING,
    },
    {
        .name = "inward_start_wins_over_end",
        .input_wkt = "MULTILINESTRING ((0 0, 1 0), (2 0, 1 0))",
        .expected_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 2 0))",
        .expected_type = geos::geom::GEOS_MULTILINESTRING,
    },
    {
        .name = "outward_no_obvious_start_flips",
        .input_wkt = "MULTILINESTRING ((1 0, 0 0), (1 0, 2 0))",
        .expected_wkt = "MULTILINESTRING ((2 0, 1 0), (1 0, 0 0))",
        .expected_type = geos::geom::GEOS_MULTILINESTRING,
    },
    {
        .name = "four_segment_scrambled_path",
        .input_wkt = "MULTILINESTRING ((3 0, 4 0), (1 0, 0 0), "
                     "(2 0, 3 0), (1 0, 2 0))",
        .expected_wkt = "MULTILINESTRING ((4 0, 3 0), (3 0, 2 0), "
                        "(2 0, 1 0), (1 0, 0 0))",
        .expected_type = geos::geom::GEOS_MULTILINESTRING,
    },
    {
        .name = "closed_triangle_no_degree_1_nodes",
        .input_wkt = "MULTILINESTRING ((1 0, 1 1), (0 0, 1 0), (0 0, 1 1))",
        .expected_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 1 1), (1 1, 0 0))",
        .expected_type = geos::geom::GEOS_MULTILINESTRING,
    },
    {
        .name = "lollipop_stem_pointing_away",
        .input_wkt = "MULTILINESTRING ((1 0, 0 0), (1 0, 2 1), "
                     "(2 1, 2 -1), (2 -1, 1 0))",
        .expected_wkt = "MULTILINESTRING ((1 0, 2 -1), (2 -1, 2 1), "
                        "(2 1, 1 0), (1 0, 0 0))",
        .expected_type = geos::geom::GEOS_MULTILINESTRING,
    },
    {
        .name = "lollipop_stem_pointing_into_cycle",
        .input_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 2 1), "
                     "(2 1, 2 -1), (2 -1, 1 0))",
        .expected_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 2 1), "
                        "(2 1, 2 -1), (2 -1, 1 0))",
        .expected_type = geos::geom::GEOS_MULTILINESTRING,
    },
    {
        .name = "lollipop_prefers_well_oriented_over_smaller_angle",
        .input_wkt = "MULTILINESTRING ((0 0, 1 0), (2 1, 1 0), "
                     "(2 -1, 2 1), (1 0, 2 -1))",
        .expected_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 2 -1), "
                        "(2 -1, 2 1), (2 1, 1 0))",
        .expected_type = geos::geom::GEOS_MULTILINESTRING,
    },
    {
        .name = "right_tail_lowest_degree_at_largest_x",
        .input_wkt = "MULTILINESTRING ((0 0, 1 1), (1 1, 2 0), "
                     "(2 0, 0 0), (3 0, 2 0))",
        .expected_wkt = "MULTILINESTRING ((3 0, 2 0), (2 0, 0 0), "
                        "(0 0, 1 1), (1 1, 2 0))",
        .expected_type = geos::geom::GEOS_MULTILINESTRING,
    },
    {
        .name = "side_loop_spliced_closed_subpath",
        .input_wkt = "MULTILINESTRING ((0 0, 10 0), (10 0, 10 -10), "
                     "(10 0, 20 0), (20 0, 20 10), (20 10, 10 0))",
        .expected_wkt = "MULTILINESTRING ((0 0, 10 0), (10 0, 20 0), "
                        "(20 0, 20 10), (20 10, 10 0), (10 0, 10 -10))",
        .expected_type = geos::geom::GEOS_MULTILINESTRING,
    },
    {
        .name = "two_disconnected_components",
        .input_wkt = "MULTILINESTRING ((1 0, 2 0), (0 0, 1 0), "
                     "(12 0, 11 0), (10 0, 11 0))",
        .expected_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 2 0), "
                        "(10 0, 11 0), (11 0, 12 0))",
        .expected_type = geos::geom::GEOS_MULTILINESTRING,
    },
    {
        .name = "two_disconnected_components_with_reversed_second",
        .input_wkt = "MULTILINESTRING ((1 0, 2 0), (0 0, 1 0), "
                     "(11 0, 10 0), (12 0, 11 0))",
        .expected_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 2 0), "
                        "(12 0, 11 0), (11 0, 10 0))",
        .expected_type = geos::geom::GEOS_MULTILINESTRING,
    },
});

constexpr auto kNonSequenceableCases = std::to_array<NonSequenceableCase>({
    {
        .name = "y_junction_four_odd_nodes",
        .input_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 2 1), (1 0, 2 -1))",
    },
    {
        .name = "h_shape_four_odd_nodes",
        .input_wkt = "MULTILINESTRING ((0 -1, 0 0), (0 0, 0 1), (0 0, 1 0), "
                     "(1 -1, 1 0), (1 0, 1 1))",
    },
    {
        .name = "four_ray_star",
        .input_wkt = "MULTILINESTRING ((0 0, 1 0), (0 0, -1 0), "
                     "(0 0, 0 1), (0 0, 0 -1))",
    },
    {
        .name = "complete_graph_k4",
        .input_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 1 1), (1 1, 0 1), "
                     "(0 1, 0 0), (0 0, 1 1), (1 0, 0 1))",
    },
    {
        .name = "six_odd_nodes_double_y",
        .input_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 2 1), (1 0, 2 -1), "
                     "(0 0, -1 1), (0 0, -1 -1))",
    },
    {
        .name = "sequenceable_component_then_non_sequenceable",
        .input_wkt = "MULTILINESTRING ((0 0, 1 0), "
                     "(10 0, 11 0), (11 0, 12 1), (11 0, 12 -1))",
    },
    // Keep non-sequenceable components last: upstream
    // LineSequencer::findSequences() deletes only the failing Subgraph* before
    // returning nullptr, leaking any later Subgraph* entries in subgraphs.
    {
        .name = "two_sequenceable_components_then_non_sequenceable",
        .input_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 2 0), "
                     "(10 0, 11 0), "
                     "(20 0, 21 0), (21 0, 22 1), (21 0, 22 -1))",
    },
});

void VerifySequencedOutput(const Geometry& actual, const Geometry& expected,
                           GeometryTypeId expected_type) {
  EXPECT_EQ(actual.getGeometryTypeId(), expected_type);
  EXPECT_EQ(actual.getNumGeometries(), expected.getNumGeometries());
  EXPECT_TRUE(LineSequencer::isSequenced(&actual));
  EXPECT_TRUE(actual.equalsIdentical(&expected))
      << "actual:   " << actual.toText() << "\nexpected: " << expected.toText();
}

TEST(LineSequencerTest, IsSequencedNonMultiLineStringTypes) {
  EXPECT_TRUE(LineSequencer::isSequenced(nullptr));

  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  constexpr auto kNonMlsWkts = std::to_array<std::string_view>({
      "POINT (1 2)",
      "POINT EMPTY",
      "LINESTRING (0 0, 1 1)",
      "LINESTRING EMPTY",
      "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
      "MULTIPOINT ((0 0), (1 1))",
      "MULTIPOLYGON (((0 0, 1 0, 1 1, 0 0)))",
      "GEOMETRYCOLLECTION (MULTILINESTRING ((0 0, 1 0), (10 0, 11 0), "
      "(1 0, 2 0)))",
  });

  for (std::string_view wkt : kNonMlsWkts) {
    SCOPED_TRACE(std::string(wkt));
    Geometry::Ptr geom = reader.read(std::string(wkt));
    ASSERT_NE(geom, nullptr);
    EXPECT_TRUE(LineSequencer::isSequenced(geom.get()));
  }

  CoordinateSequence ring_coords = {
      CoordinateXY(0.0, 0.0),
      CoordinateXY(1.0, 0.0),
      CoordinateXY(1.0, 1.0),
      CoordinateXY(0.0, 0.0),
  };
  std::unique_ptr<LinearRing> ring = factory->createLinearRing(ring_coords);
  ASSERT_NE(ring, nullptr);
  EXPECT_TRUE(LineSequencer::isSequenced(ring.get()));
}

TEST(LineSequencerTest, IsSequencedMultiLineStringCases) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (const auto& tc : kIsSequencedCases) {
    SCOPED_TRACE(std::string(tc.name));
    Geometry::Ptr geom = reader.read(std::string(tc.wkt));
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(LineSequencer::isSequenced(geom.get()), tc.expected);
  }
}

TEST(LineSequencerTest, SequencesExpectedCases) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (const auto& tc : kSequenceCases) {
    SCOPED_TRACE(std::string(tc.name));
    Geometry::Ptr input = reader.read(std::string(tc.input_wkt));
    Geometry::Ptr expected = reader.read(std::string(tc.expected_wkt));
    ASSERT_NE(input, nullptr);
    ASSERT_NE(expected, nullptr);

    std::unique_ptr<Geometry> static_res(LineSequencer::sequence(*input));
    ASSERT_NE(static_res, nullptr);
    VerifySequencedOutput(*static_res, *expected, tc.expected_type);

    LineSequencer sequencer;
    // Pass const Geometry& so overload resolution selects add(const Geometry&)
    // rather than template add(TargetContainer&).
    sequencer.add(static_cast<const Geometry&>(*input));
    EXPECT_TRUE(sequencer.isSequenceable());
    EXPECT_TRUE(sequencer.isSequenceable());
    std::unique_ptr<Geometry> instance_res(sequencer.getSequencedLineStrings());
    ASSERT_NE(instance_res, nullptr);
    VerifySequencedOutput(*instance_res, *expected, tc.expected_type);
  }
}

TEST(LineSequencerTest, SequencesLinearRingAndMultipleRings) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  CoordinateSequence coords1 = {
      CoordinateXY(0.0, 0.0),
      CoordinateXY(1.0, 0.0),
      CoordinateXY(1.0, 1.0),
      CoordinateXY(0.0, 0.0),
  };
  std::unique_ptr<LinearRing> ring1 = factory->createLinearRing(coords1);
  ASSERT_NE(ring1, nullptr);

  std::unique_ptr<Geometry> single_res(LineSequencer::sequence(*ring1));
  ASSERT_NE(single_res, nullptr);
  EXPECT_EQ(single_res->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  EXPECT_TRUE(single_res->equalsIdentical(ring1.get()));

  CoordinateSequence coords2 = {
      CoordinateXY(0.0, 0.0),
      CoordinateXY(-1.0, 0.0),
      CoordinateXY(-1.0, -1.0),
      CoordinateXY(0.0, 0.0),
  };
  std::unique_ptr<LinearRing> ring2 = factory->createLinearRing(coords2);
  ASSERT_NE(ring2, nullptr);

  LineSequencer sequencer;
  sequencer.add(static_cast<const Geometry&>(*ring1));
  sequencer.add(static_cast<const Geometry&>(*ring2));
  EXPECT_TRUE(sequencer.isSequenceable());
  std::unique_ptr<Geometry> multi_res(sequencer.getSequencedLineStrings());
  ASSERT_NE(multi_res, nullptr);
  EXPECT_EQ(multi_res->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(multi_res->getNumGeometries(), 2u);
  EXPECT_TRUE(LineSequencer::isSequenced(multi_res.get()));
}

TEST(LineSequencerTest, SequencesComplexEulerianTopologies) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // Nested side loops requiring multiple Eulerian subpath splices.
  Geometry::Ptr nested_loops = reader.read(
      "MULTILINESTRING ((0 0, 10 0), (10 0, 10 -10), (10 0, 20 0), "
      "(20 0, 20 -5), (20 -5, 25 -5), (25 -5, 20 0), "
      "(20 0, 20 10), (20 10, 10 0))");
  ASSERT_NE(nested_loops, nullptr);
  std::unique_ptr<Geometry> nested_res(LineSequencer::sequence(*nested_loops));
  ASSERT_NE(nested_res, nullptr);
  EXPECT_EQ(nested_res->getNumGeometries(), 8u);
  EXPECT_TRUE(LineSequencer::isSequenced(nested_res.get()));

  // Figure-eight of two closed LineStrings sharing (0, 0).
  Geometry::Ptr figure_eight = reader.read(
      "MULTILINESTRING ((0 0, 1 0, 1 1, 0 0), (0 0, -1 0, -1 -1, 0 0))");
  ASSERT_NE(figure_eight, nullptr);
  std::unique_ptr<Geometry> fig8_res(LineSequencer::sequence(*figure_eight));
  ASSERT_NE(fig8_res, nullptr);
  EXPECT_EQ(fig8_res->getNumGeometries(), 2u);
  EXPECT_TRUE(LineSequencer::isSequenced(fig8_res.get()));

  // Two parallel lines forming a 2-edge cycle.
  Geometry::Ptr two_parallel =
      reader.read("MULTILINESTRING ((0 0, 1 1, 2 0), (0 0, 1 -1, 2 0))");
  ASSERT_NE(two_parallel, nullptr);
  std::unique_ptr<Geometry> par2_res(LineSequencer::sequence(*two_parallel));
  ASSERT_NE(par2_res, nullptr);
  EXPECT_EQ(par2_res->getNumGeometries(), 2u);
  EXPECT_TRUE(LineSequencer::isSequenced(par2_res.get()));

  // Three parallel lines between two degree-3 nodes (no degree-1 nodes).
  Geometry::Ptr three_parallel = reader.read(
      "MULTILINESTRING ((0 0, 1 1, 2 0), (0 0, 2 0), (0 0, 1 -1, 2 0))");
  ASSERT_NE(three_parallel, nullptr);
  std::unique_ptr<Geometry> par3_res(LineSequencer::sequence(*three_parallel));
  ASSERT_NE(par3_res, nullptr);
  EXPECT_EQ(par3_res->getNumGeometries(), 3u);
  EXPECT_TRUE(LineSequencer::isSequenced(par3_res.get()));

  // Four parallel lines between two degree-4 nodes (Eulerian circuit).
  Geometry::Ptr four_parallel = reader.read(
      "MULTILINESTRING ((0 0, 1 2, 2 0), (0 0, 1 1, 2 0), "
      "(0 0, 1 -1, 2 0), (0 0, 1 -2, 2 0))");
  ASSERT_NE(four_parallel, nullptr);
  std::unique_ptr<Geometry> par4_res(LineSequencer::sequence(*four_parallel));
  ASSERT_NE(par4_res, nullptr);
  EXPECT_EQ(par4_res->getNumGeometries(), 4u);
  EXPECT_TRUE(LineSequencer::isSequenced(par4_res.get()));

  // Disconnected mix: isolated edge + closed triangle + reversed 2-edge chain.
  Geometry::Ptr mixed_disconnected = reader.read(
      "MULTILINESTRING ((20 0, 21 0), (30 0, 31 0), (31 0, 31 1), "
      "(31 1, 30 0), (2 0, 1 0), (1 0, 0 0))");
  ASSERT_NE(mixed_disconnected, nullptr);
  std::unique_ptr<Geometry> mixed_res(
      LineSequencer::sequence(*mixed_disconnected));
  ASSERT_NE(mixed_res, nullptr);
  EXPECT_EQ(mixed_res->getNumGeometries(), 6u);
  EXPECT_TRUE(LineSequencer::isSequenced(mixed_res.get()));

  // Line with consecutive repeated intermediate vertices still sequences.
  Geometry::Ptr repeated_pts =
      reader.read("MULTILINESTRING ((0 0, 0 0, 1 0, 1 0), (2 0, 1 0))");
  Geometry::Ptr repeated_expected =
      reader.read("MULTILINESTRING ((0 0, 0 0, 1 0, 1 0), (1 0, 2 0))");
  ASSERT_NE(repeated_pts, nullptr);
  ASSERT_NE(repeated_expected, nullptr);
  std::unique_ptr<Geometry> rep_res(LineSequencer::sequence(*repeated_pts));
  ASSERT_NE(rep_res, nullptr);
  VerifySequencedOutput(*rep_res, *repeated_expected,
                        geos::geom::GEOS_MULTILINESTRING);
}

TEST(LineSequencerTest, RejectsNonSequenceableGraphs) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (const auto& tc : kNonSequenceableCases) {
    SCOPED_TRACE(std::string(tc.name));
    Geometry::Ptr input = reader.read(std::string(tc.input_wkt));
    ASSERT_NE(input, nullptr);

    LineSequencer sequencer;
    sequencer.add(static_cast<const Geometry&>(*input));
    EXPECT_FALSE(sequencer.isSequenceable());
    EXPECT_FALSE(sequencer.isSequenceable());
    EXPECT_EQ(sequencer.getSequencedLineStrings(false), nullptr);
    EXPECT_EQ(sequencer.getSequencedLineStrings(true), nullptr);

    std::unique_ptr<Geometry> static_res(LineSequencer::sequence(*input));
    EXPECT_EQ(static_res, nullptr);
  }
}

TEST(LineSequencerTest, AddMethodsAndComponentFiltering) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // Add heterogeneous GeometryCollection: Point and Polygon are filtered out.
  Geometry::Ptr collection = reader.read(
      "GEOMETRYCOLLECTION (POINT (5 5), LINESTRING (2 0, 1 0), "
      "POLYGON ((10 10, 12 10, 12 12, 10 10)), LINESTRING (0 0, 1 0))");
  Geometry::Ptr expected =
      reader.read("MULTILINESTRING ((0 0, 1 0), (1 0, 2 0))");
  ASSERT_NE(collection, nullptr);
  ASSERT_NE(expected, nullptr);

  LineSequencer collection_seq;
  collection_seq.add(static_cast<const Geometry&>(*collection));
  EXPECT_TRUE(collection_seq.isSequenceable());
  std::unique_ptr<Geometry> coll_res(collection_seq.getSequencedLineStrings());
  ASSERT_NE(coll_res, nullptr);
  VerifySequencedOutput(*coll_res, *expected, geos::geom::GEOS_MULTILINESTRING);

  // Add via template container overload (const vector and list) and direct
  // filter().
  Geometry::Ptr pt = reader.read("POINT (9 9)");
  Geometry::Ptr g0 = reader.read("LINESTRING (2 0, 1 0)");
  Geometry::Ptr g1 = reader.read("LINESTRING (0 0, 1 0)");
  ASSERT_NE(pt, nullptr);
  ASSERT_NE(g0, nullptr);
  ASSERT_NE(g1, nullptr);

  const std::vector<const Geometry*> geom_vec = {pt.get(), g0.get(), g1.get()};
  LineSequencer vec_seq;
  vec_seq.add(geom_vec);
  EXPECT_TRUE(vec_seq.isSequenceable());
  std::unique_ptr<Geometry> vec_res(vec_seq.getSequencedLineStrings());
  ASSERT_NE(vec_res, nullptr);
  VerifySequencedOutput(*vec_res, *expected, geos::geom::GEOS_MULTILINESTRING);

  std::list<const Geometry*> geom_list = {g0.get(), pt.get(), g1.get()};
  LineSequencer list_seq;
  list_seq.add(geom_list);
  EXPECT_TRUE(list_seq.isSequenceable());
  std::unique_ptr<Geometry> list_res(list_seq.getSequencedLineStrings());
  ASSERT_NE(list_res, nullptr);
  VerifySequencedOutput(*list_res, *expected, geos::geom::GEOS_MULTILINESTRING);

  // Incremental add(const Geometry&) calls across multiple inputs.
  LineSequencer incremental_seq;
  incremental_seq.add(static_cast<const Geometry&>(*pt));
  incremental_seq.add(static_cast<const Geometry&>(*g0));
  incremental_seq.add(static_cast<const Geometry&>(*g1));
  EXPECT_TRUE(incremental_seq.isSequenceable());
  std::unique_ptr<Geometry> inc_res(incremental_seq.getSequencedLineStrings());
  ASSERT_NE(inc_res, nullptr);
  VerifySequencedOutput(*inc_res, *expected, geos::geom::GEOS_MULTILINESTRING);

  // Separate LineString container plus direct filter() calls.
  const auto* ls0 = dynamic_cast<const LineString*>(g0.get());
  const auto* ls1 = dynamic_cast<const LineString*>(g1.get());
  ASSERT_NE(ls0, nullptr);
  ASSERT_NE(ls1, nullptr);
  std::vector<const LineString*> line_vec = {ls0};

  Geometry::Ptr poly = reader.read("POLYGON ((10 10, 12 10, 12 12, 10 10))");
  ASSERT_NE(poly, nullptr);

  LineSequencer filter_seq;
  filter_seq.add(line_vec);
  filter_seq.filter(nullptr);
  filter_seq.filter(pt.get());
  filter_seq.filter(poly.get());
  filter_seq.filter(ls1);
  EXPECT_TRUE(filter_seq.isSequenceable());
  // Adding after computeSequence() has already run leaves cached output intact.
  Geometry::Ptr extra = reader.read("LINESTRING (2 0, 3 0)");
  ASSERT_NE(extra, nullptr);
  filter_seq.add(static_cast<const Geometry&>(*extra));
  std::unique_ptr<Geometry> filter_res(filter_seq.getSequencedLineStrings());
  ASSERT_NE(filter_res, nullptr);
  VerifySequencedOutput(*filter_res, *expected,
                        geos::geom::GEOS_MULTILINESTRING);
}

TEST(LineSequencerTest, GetSequencedLineStringsReleaseOwnership) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr input = reader.read("MULTILINESTRING ((0 0, 1 0), (2 0, 1 0))");
  ASSERT_NE(input, nullptr);

  LineSequencer sequencer;
  sequencer.add(static_cast<const Geometry&>(*input));

  const Geometry* borrowed1 = sequencer.getSequencedLineStrings(false);
  ASSERT_NE(borrowed1, nullptr);
  const Geometry* borrowed2 = sequencer.getSequencedLineStrings(false);
  EXPECT_EQ(borrowed1, borrowed2);
  EXPECT_TRUE(sequencer.isSequenceable());

  std::unique_ptr<Geometry> owned(sequencer.getSequencedLineStrings(true));
  EXPECT_EQ(owned.get(), borrowed1);
  EXPECT_EQ(sequencer.getSequencedLineStrings(false), nullptr);
  EXPECT_EQ(sequencer.getSequencedLineStrings(), nullptr);
  EXPECT_TRUE(sequencer.isSequenceable());
}

TEST(LineSequencerTest, PreservesCustomFactoryAndZAndMCoordinates) {
  PrecisionModel pm(1000.0);
  GeometryFactory::Ptr custom_factory = GeometryFactory::create(&pm, 3857);
  WKTReader reader(custom_factory.get());

  Geometry::Ptr xy_input =
      reader.read("MULTILINESTRING ((0 0, 1 0), (2 0, 1 0))");
  ASSERT_NE(xy_input, nullptr);
  std::unique_ptr<Geometry> xy_res(LineSequencer::sequence(*xy_input));
  ASSERT_NE(xy_res, nullptr);
  EXPECT_EQ(xy_res->getFactory(), custom_factory.get());
  EXPECT_EQ(xy_res->getSRID(), 3857);

  // XYZ coordinates preserved on forward and reversed segments.
  Geometry::Ptr xyz_input =
      reader.read("MULTILINESTRING Z ((0 0 10, 1 0 20), (2 0 30, 1 0 99))");
  Geometry::Ptr xyz_expected =
      reader.read("MULTILINESTRING Z ((0 0 10, 1 0 20), (1 0 99, 2 0 30))");
  ASSERT_NE(xyz_input, nullptr);
  ASSERT_NE(xyz_expected, nullptr);
  std::unique_ptr<Geometry> xyz_res(LineSequencer::sequence(*xyz_input));
  ASSERT_NE(xyz_res, nullptr);
  EXPECT_TRUE(xyz_res->hasZ());
  EXPECT_FALSE(xyz_res->hasM());
  VerifySequencedOutput(*xyz_res, *xyz_expected,
                        geos::geom::GEOS_MULTILINESTRING);

  // XYM coordinates preserved without introducing Z values.
  Geometry::Ptr xym_input =
      reader.read("MULTILINESTRING M ((0 0 100, 1 0 200), (2 0 300, 1 0 400))");
  ASSERT_NE(xym_input, nullptr);
  std::unique_ptr<Geometry> xym_res(LineSequencer::sequence(*xym_input));
  ASSERT_NE(xym_res, nullptr);
  EXPECT_FALSE(xym_res->hasZ());
  EXPECT_TRUE(xym_res->hasM());

  const auto* xym_mls = dynamic_cast<const MultiLineString*>(xym_res.get());
  ASSERT_NE(xym_mls, nullptr);
  ASSERT_EQ(xym_mls->getNumGeometries(), 2u);

  const CoordinateSequence* seq0 = xym_mls->getGeometryN(0)->getCoordinatesRO();
  const CoordinateSequence* seq1 = xym_mls->getGeometryN(1)->getCoordinatesRO();
  ASSERT_EQ(seq0->size(), 2u);
  ASSERT_EQ(seq1->size(), 2u);

  CoordinateXYM pt;
  seq0->getAt(0, pt);
  EXPECT_DOUBLE_EQ(pt.x, 0.0);
  EXPECT_DOUBLE_EQ(pt.y, 0.0);
  EXPECT_DOUBLE_EQ(pt.m, 100.0);
  EXPECT_TRUE(std::isnan(seq0->getOrdinate(0, CoordinateSequence::Z)));

  seq1->getAt(0, pt);
  EXPECT_DOUBLE_EQ(pt.x, 1.0);
  EXPECT_DOUBLE_EQ(pt.y, 0.0);
  EXPECT_DOUBLE_EQ(pt.m, 400.0);
  seq1->getAt(1, pt);
  EXPECT_DOUBLE_EQ(pt.x, 2.0);
  EXPECT_DOUBLE_EQ(pt.y, 0.0);
  EXPECT_DOUBLE_EQ(pt.m, 300.0);

  // XYZM coordinates preserved across reversal.
  Geometry::Ptr xyzm_input = reader.read(
      "MULTILINESTRING ZM ((0 0 1 10, 1 0 2 20), (2 0 3 30, 1 0 4 40))");
  Geometry::Ptr xyzm_expected = reader.read(
      "MULTILINESTRING ZM ((0 0 1 10, 1 0 2 20), (1 0 4 40, 2 0 3 30))");
  ASSERT_NE(xyzm_input, nullptr);
  ASSERT_NE(xyzm_expected, nullptr);
  std::unique_ptr<Geometry> xyzm_res(LineSequencer::sequence(*xyzm_input));
  ASSERT_NE(xyzm_res, nullptr);
  EXPECT_TRUE(xyzm_res->hasZ());
  EXPECT_TRUE(xyzm_res->hasM());
  VerifySequencedOutput(*xyzm_res, *xyzm_expected,
                        geos::geom::GEOS_MULTILINESTRING);

  const auto* xyzm_mls = dynamic_cast<const MultiLineString*>(xyzm_res.get());
  ASSERT_NE(xyzm_mls, nullptr);
  CoordinateXYZM pt4;
  xyzm_mls->getGeometryN(1)->getCoordinatesRO()->getAt(1, pt4);
  EXPECT_DOUBLE_EQ(pt4.x, 2.0);
  EXPECT_DOUBLE_EQ(pt4.y, 0.0);
  EXPECT_DOUBLE_EQ(pt4.z, 3.0);
  EXPECT_DOUBLE_EQ(pt4.m, 30.0);
}

}  // namespace
}  // namespace geos_google
