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

// Unit tests for third_party/geos/src/dissolve/LineDissolver.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/dissolve/LineDissolver.cpp

#include "third_party/geos/include/geos/dissolve/LineDissolver.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::dissolve::LineDissolver;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::io::WKTReader;

static_assert(!std::is_copy_constructible_v<LineDissolver>);
static_assert(!std::is_copy_assignable_v<LineDissolver>);

struct DissolveCase {
  std::string_view name;
  std::string_view input_wkt;
  std::string_view expected_wkt;
};

constexpr auto kSingleGeometryCases = std::to_array<DissolveCase>({
    {
        .name = "single_segment",
        .input_wkt = "LINESTRING (0 0, 1 0)",
        .expected_wkt = "LINESTRING (0 0, 1 0)",
    },
    {
        .name = "self_overlapping_backtrack",
        .input_wkt = "LINESTRING (0 0, 1 0, 0 0)",
        .expected_wkt = "LINESTRING (0 0, 1 0)",
    },
    {
        .name = "zero_length_segments_skipped",
        .input_wkt = "LINESTRING (0 0, 0 0, 1 0, 1 0, 2 0)",
        .expected_wkt = "LINESTRING (0 0, 1 0, 2 0)",
    },
    {
        .name = "all_zero_length_segments",
        .input_wkt = "LINESTRING (0 0, 0 0)",
        .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
    },
    {
        .name = "empty_linestring",
        .input_wkt = "LINESTRING EMPTY",
        .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
    },
    {
        .name = "empty_multilinestring",
        .input_wkt = "MULTILINESTRING EMPTY",
        .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
    },
    {
        .name = "empty_polygon",
        .input_wkt = "POLYGON EMPTY",
        .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
    },
    {
        .name = "point_ignored",
        .input_wkt = "POINT (1 2)",
        .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
    },
    {
        .name = "multipoint_ignored",
        .input_wkt = "MULTIPOINT ((0 0), (1 1))",
        .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
    },
    {
        .name = "duplicate_lines_opposite_direction",
        .input_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 0 0))",
        .expected_wkt = "LINESTRING (0 0, 1 0)",
    },
    {
        .name = "merge_chain_with_reversed_middle_segment",
        .input_wkt = "MULTILINESTRING ((0 0, 1 0), (2 1, 1 0), (2 1, 3 1))",
        .expected_wkt = "LINESTRING (0 0, 1 0, 2 1, 3 1)",
    },
    {
        .name = "y_junction_splits_at_degree_3_node",
        .input_wkt = "MULTILINESTRING ((0 0, 1 1, 2 0), (1 1, 1 3))",
        .expected_wkt = "MULTILINESTRING ((0 0, 1 1), (1 1, 2 0), (1 1, 1 3))",
    },
    {
        .name = "four_way_intersection_node",
        .input_wkt = "MULTILINESTRING ((0 1, 1 1, 2 1), (1 0, 1 1, 1 2))",
        .expected_wkt =
            "MULTILINESTRING ((0 1, 1 1), (1 1, 2 1), (1 0, 1 1), (1 1, 1 2))",
    },
    {
        .name = "lollipop_stem_and_loop",
        .input_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 2 0, 2 1, 1 0))",
        .expected_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 2 0, 2 1, 1 0))",
    },
    {
        .name = "figure_eight_pinched_ring",
        .input_wkt =
            "MULTILINESTRING ((0 0, 1 0, 1 1, 0 0), (0 0, -1 0, -1 -1, 0 0))",
        .expected_wkt =
            "MULTILINESTRING ((0 0, 1 0, 1 1, 0 0), (0 0, -1 0, -1 -1, 0 0))",
    },
    {
        .name = "two_paths_between_two_vertices_form_ring",
        .input_wkt = "MULTILINESTRING ((0 0, 1 1, 2 0), (0 0, 1 -1, 2 0))",
        .expected_wkt = "LINESTRING (0 0, 1 1, 2 0, 1 -1, 0 0)",
    },
    {
        .name = "parallel_paths_between_two_nodes",
        .input_wkt =
            "MULTILINESTRING ((0 0, 1 1, 2 0), (0 0, 1 -1, 2 0), (0 0, 2 0))",
        .expected_wkt =
            "MULTILINESTRING ((0 0, 1 1, 2 0), (0 0, 1 -1, 2 0), (0 0, 2 0))",
    },
    {
        .name = "self_loop_attached_along_line",
        .input_wkt = "LINESTRING (0 0, 1 0, 2 1, 2 0, 1 0, 3 0)",
        .expected_wkt =
            "MULTILINESTRING ((0 0, 1 0), (1 0, 2 1, 2 0, 1 0), (1 0, 3 0))",
    },
    {
        .name = "adjacent_polygons_shared_edge_dissolved",
        .input_wkt = "MULTIPOLYGON (((0 0, 1 0, 1 1, 0 1, 0 0)), "
                     "((1 0, 2 0, 2 1, 1 1, 1 0)))",
        .expected_wkt = "MULTILINESTRING ((1 0, 0 0, 0 1, 1 1), (1 0, 1 1), "
                        "(1 0, 2 0, 2 1, 1 1))",
    },
    {
        .name = "three_adjacent_polygons_shared_edges",
        .input_wkt = "MULTIPOLYGON (((0 0, 1 0, 1 1, 1 2, 0 2, 0 0)), "
                     "((1 0, 2 0, 2 1, 1 1, 1 0)), "
                     "((1 1, 2 1, 2 2, 1 2, 1 1)))",
        .expected_wkt =
            "MULTILINESTRING ((1 0, 0 0, 0 2, 1 2), (1 0, 1 1), (1 1, 1 2), "
            "(1 0, 2 0, 2 1), (1 1, 2 1), (2 1, 2 2, 1 2))",
    },
    {
        .name = "polygon_with_hole_extracts_both_rings",
        .input_wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
                     "(2 2, 2 8, 8 8, 8 2, 2 2))",
        .expected_wkt = "MULTILINESTRING ((0 0, 10 0, 10 10, 0 10, 0 0), "
                        "(2 2, 2 8, 8 8, 8 2, 2 2))",
    },
    {
        .name = "isolated_ring_linestring",
        .input_wkt = "LINESTRING (0 0, 1 0, 1 1, 0 1, 0 0)",
        .expected_wkt = "LINESTRING (0 0, 1 0, 1 1, 0 1, 0 0)",
    },
    {
        .name = "disconnected_components",
        .input_wkt = "MULTILINESTRING ((0 0, 1 0), (10 10, 11 10, 11 11))",
        .expected_wkt = "MULTILINESTRING ((0 0, 1 0), (10 10, 11 10, 11 11))",
    },
    {
        .name = "geometry_collection_mixed_and_nested",
        .input_wkt =
            "GEOMETRYCOLLECTION (POINT (99 99), LINESTRING (0 0, 1 0), "
            "POLYGON ((1 0, 2 0, 2 1, 1 1, 1 0)), "
            "GEOMETRYCOLLECTION (LINESTRING (1 0, 1 1)))",
        .expected_wkt =
            "MULTILINESTRING ((0 0, 1 0), (1 0, 2 0, 2 1, 1 1, 1 0))",
    },
    {
        .name = "crossing_lines_remain_unnoded",
        .input_wkt = "MULTILINESTRING ((0 0, 2 2), (0 2, 2 0))",
        .expected_wkt = "MULTILINESTRING ((0 0, 2 2), (0 2, 2 0))",
    },
});

class LineDissolverTest : public ::testing::Test {
 protected:
  LineDissolverTest()
      : factory_(GeometryFactory::create()), reader_(factory_.get()) {}

  std::unique_ptr<Geometry> ReadWkt(std::string_view wkt) const {
    return reader_.read(std::string(wkt));
  }

  void ExpectNormalizedEquals(Geometry* actual,
                              std::string_view expected_wkt) const {
    ASSERT_NE(actual, nullptr);
    std::unique_ptr<Geometry> expected = ReadWkt(expected_wkt);
    ASSERT_NE(expected, nullptr);

    actual->normalize();
    expected->normalize();
    EXPECT_TRUE(actual->equalsExact(expected.get(), 1e-9))
        << "actual: " << actual->toText()
        << "\nexpected: " << expected->toText();
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(LineDissolverTest, SingleGeometryStaticDissolveCases) {
  for (const DissolveCase& tc : kSingleGeometryCases) {
    SCOPED_TRACE(tc.name);
    std::unique_ptr<Geometry> input = ReadWkt(tc.input_wkt);
    ASSERT_NE(input, nullptr);

    std::unique_ptr<Geometry> actual = LineDissolver::dissolve(input.get());
    ExpectNormalizedEquals(actual.get(), tc.expected_wkt);
  }
}

TEST_F(LineDissolverTest, AddVectorOfGeometriesOverload) {
  std::unique_ptr<Geometry> pt = ReadWkt("POINT (5 5)");
  std::unique_ptr<Geometry> poly =
      ReadWkt("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  std::unique_ptr<Geometry> shared_edge = ReadWkt("LINESTRING (2 0, 2 2)");
  std::unique_ptr<Geometry> tail = ReadWkt("LINESTRING (2 2, 4 2)");

  LineDissolver dissolver;
  dissolver.add(std::vector<const Geometry*>{pt.get(), poly.get(),
                                             shared_edge.get(), tail.get()});
  // Empty vector after adding geometries should be a no-op.
  dissolver.add(std::vector<const Geometry*>{});

  std::unique_ptr<Geometry> actual = dissolver.getResult();
  ExpectNormalizedEquals(
      actual.get(), "MULTILINESTRING ((2 2, 0 2, 0 0, 2 0, 2 2), (2 2, 4 2))");
}

TEST_F(LineDissolverTest, AddLineStringOverloadDirectly) {
  auto ls1 = reader_.read<LineString>("LINESTRING (0 0, 1 0)");
  auto ls2 = reader_.read<LineString>("LINESTRING (2 0, 1 0)");
  auto ls_empty = reader_.read<LineString>("LINESTRING EMPTY");

  LineDissolver dissolver;
  // Calling add(const LineString*) first initializes factory when nullptr.
  dissolver.add(ls1.get());
  // Subsequent calls reuse existing factory and skip empty coordinate seqs.
  dissolver.add(ls2.get());
  dissolver.add(ls_empty.get());

  std::unique_ptr<Geometry> actual = dissolver.getResult();
  ExpectNormalizedEquals(actual.get(), "LINESTRING (0 0, 1 0, 2 0)");
}

TEST_F(LineDissolverTest, AddLinearRingDirectly) {
  auto coords =
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {0.0, 0.0}, {2.0, 0.0}, {2.0, 2.0}, {0.0, 2.0}, {0.0, 0.0}});
  std::unique_ptr<LinearRing> ring =
      factory_->createLinearRing(std::move(coords));

  const Geometry* geom_ring = ring.get();
  std::unique_ptr<Geometry> actual = LineDissolver::dissolve(geom_ring);
  ASSERT_NE(actual, nullptr);
  EXPECT_EQ(actual->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  ExpectNormalizedEquals(actual.get(), "LINESTRING (0 0, 2 0, 2 2, 0 2, 0 0)");
}

TEST_F(LineDissolverTest,
       IsolatedRingPreservesOriginalStartVertexAndDirection) {
  // Start vertex (1 1) is not the lowest coordinate and ring is clockwise.
  // Traversal visits the start edge (1 1)->(0 1) and preserves the original
  // start vertex and orientation.
  constexpr std::string_view kRingWkt = "LINESTRING (1 1, 0 1, 0 0, 1 0, 1 1)";
  std::unique_ptr<Geometry> input = ReadWkt(kRingWkt);

  std::unique_ptr<Geometry> actual = LineDissolver::dissolve(input.get());
  ASSERT_NE(actual, nullptr);
  EXPECT_EQ(actual->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(actual->equalsExact(input.get(), 1e-9))
      << "actual: " << actual->toText() << "\nexpected: " << input->toText();
}

TEST_F(LineDissolverTest, IsolatedRingFromTwoLinesKeepsLowerFirstStartVertex) {
  // Lowest vertex is (0 0); stack pops (0 0)->(0 1) first, so traversal
  // visits start edge at (0 1) before higher start edge at (2 0), keeping (0
  // 1).
  std::unique_ptr<Geometry> part1 = ReadWkt("LINESTRING (2 0, 0 0, 0 1)");
  std::unique_ptr<Geometry> part2 = ReadWkt("LINESTRING (0 1, 2 1, 2 0)");

  LineDissolver dissolver;
  dissolver.add(part1.get());
  dissolver.add(part2.get());
  std::unique_ptr<Geometry> actual = dissolver.getResult();
  std::unique_ptr<Geometry> expected =
      ReadWkt("LINESTRING (0 1, 2 1, 2 0, 0 0, 0 1)");

  ASSERT_NE(actual, nullptr);
  EXPECT_EQ(actual->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(actual->equalsExact(expected.get(), 1e-9))
      << "actual: " << actual->toText() << "\nexpected: " << expected->toText();
}

TEST_F(LineDissolverTest, IsolatedRingFromTwoLinesUpdatesToLowerStartVertex) {
  // Lowest vertex is (0 0); stack pops (0 0)->(1 0) first, so traversal
  // visits start edge at (1 0) before lower start edge at (0 1), updating
  // ringStartEdge to (0 1).
  std::unique_ptr<Geometry> part1 = ReadWkt("LINESTRING (0 1, 0 0, 1 0)");
  std::unique_ptr<Geometry> part2 = ReadWkt("LINESTRING (1 0, 1 1, 0 1)");

  LineDissolver dissolver;
  dissolver.add(part1.get());
  dissolver.add(part2.get());
  std::unique_ptr<Geometry> actual = dissolver.getResult();
  std::unique_ptr<Geometry> expected =
      ReadWkt("LINESTRING (0 1, 0 0, 1 0, 1 1, 0 1)");

  ASSERT_NE(actual, nullptr);
  EXPECT_EQ(actual->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(actual->equalsExact(expected.get(), 1e-9))
      << "actual: " << actual->toText() << "\nexpected: " << expected->toText();
}

TEST_F(LineDissolverTest,
       IsolatedRingFromOppositeOrientationsUpdatesViaSymStartEdge) {
  // Ring formed from three inputs with opposing directions: traversal pops
  // (0 0)->(0 1), encounters sym()->isStart() on (2 1)->(0 1), updates to
  // lower sym()->isStart() on (2 0)->(2 1), and keeps (2 0) when meeting
  // forward start edge (2 0)->(0 0).
  std::unique_ptr<Geometry> part1 = ReadWkt("LINESTRING (2 0, 0 0, 0 1)");
  std::unique_ptr<Geometry> part2 = ReadWkt("LINESTRING (2 1, 0 1)");
  std::unique_ptr<Geometry> part3 = ReadWkt("LINESTRING (2 0, 2 1)");

  LineDissolver dissolver;
  dissolver.add(part1.get());
  dissolver.add(part2.get());
  dissolver.add(part3.get());
  std::unique_ptr<Geometry> actual = dissolver.getResult();
  std::unique_ptr<Geometry> expected =
      ReadWkt("LINESTRING (2 0, 2 1, 0 1, 0 0, 2 0)");

  ASSERT_NE(actual, nullptr);
  EXPECT_EQ(actual->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(actual->equalsExact(expected.get(), 1e-9))
      << "actual: " << actual->toText() << "\nexpected: " << expected->toText();
}

TEST_F(LineDissolverTest, DuplicateOverlappingRingsCollapseToSingleRing) {
  std::unique_ptr<Geometry> ring1 =
      ReadWkt("LINESTRING (0 0, 1 0, 1 1, 0 1, 0 0)");
  std::unique_ptr<Geometry> ring2 =
      ReadWkt("LINESTRING (1 1, 1 0, 0 0, 0 1, 1 1)");

  LineDissolver dissolver;
  dissolver.add(ring1.get());
  dissolver.add(ring2.get());
  std::unique_ptr<Geometry> actual = dissolver.getResult();

  ASSERT_NE(actual, nullptr);
  EXPECT_EQ(actual->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_DOUBLE_EQ(actual->getLength(), 4.0);
  ExpectNormalizedEquals(actual.get(), "LINESTRING (0 0, 1 0, 1 1, 0 1, 0 0)");
}

TEST_F(LineDissolverTest, PreservesXYZMAndSetsMissingZMToNaN) {
  // 2D input produces NaN for missing Z and M ordinates.
  std::unique_ptr<Geometry> input2d = ReadWkt("LINESTRING (0 0, 2 3)");
  std::unique_ptr<Geometry> out2d = LineDissolver::dissolve(input2d.get());
  ASSERT_NE(out2d, nullptr);
  const auto* ls2d = dynamic_cast<const LineString*>(out2d.get());
  ASSERT_NE(ls2d, nullptr);
  const CoordinateSequence* seq2d = ls2d->getCoordinatesRO();
  ASSERT_EQ(seq2d->size(), 2u);
  for (std::size_t i = 0; i < seq2d->size(); ++i) {
    EXPECT_TRUE(std::isnan(seq2d->getOrdinate(i, CoordinateSequence::Z)));
    EXPECT_TRUE(std::isnan(seq2d->getOrdinate(i, CoordinateSequence::M)));
  }

  // 4D open line merges segments and preserves Z and M ordinates.
  std::unique_ptr<Geometry> seg1 =
      ReadWkt("LINESTRING ZM (0 0 10 100, 1 0 20 200)");
  std::unique_ptr<Geometry> seg2 =
      ReadWkt("LINESTRING ZM (1 0 20 200, 2 1 30 300)");
  LineDissolver dissolver;
  dissolver.add(seg1.get());
  dissolver.add(seg2.get());
  std::unique_ptr<Geometry> out4d = dissolver.getResult();
  std::unique_ptr<Geometry> expected4d =
      ReadWkt("LINESTRING ZM (0 0 10 100, 1 0 20 200, 2 1 30 300)");
  ASSERT_NE(out4d, nullptr);
  EXPECT_TRUE(out4d->hasZ());
  EXPECT_TRUE(out4d->hasM());
  EXPECT_TRUE(out4d->equalsIdentical(expected4d.get()));

  // 4D isolated ring preserves Z and M ordinates through buildRing.
  std::unique_ptr<Geometry> ring4d = ReadWkt(
      "LINESTRING ZM (1 1 10 1, 0 1 20 2, 0 0 30 3, 1 0 40 4, 1 1 10 1)");
  std::unique_ptr<Geometry> out_ring4d = LineDissolver::dissolve(ring4d.get());
  ASSERT_NE(out_ring4d, nullptr);
  EXPECT_TRUE(out_ring4d->equalsIdentical(ring4d.get()));
}

TEST_F(LineDissolverTest, MultipleMixedAddCallsDissolveTogether) {
  std::unique_ptr<Geometry> poly =
      ReadWkt("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  std::unique_ptr<Geometry> diag = ReadWkt("LINESTRING (0 0, 2 2)");
  auto extra = reader_.read<LineString>("LINESTRING (2 2, 0 0)");

  LineDissolver dissolver;
  dissolver.add(poly.get());
  dissolver.add(std::vector<const Geometry*>{diag.get()});
  dissolver.add(extra.get());

  std::unique_ptr<Geometry> actual = dissolver.getResult();
  ExpectNormalizedEquals(
      actual.get(),
      "MULTILINESTRING ((0 0, 2 0, 2 2), (0 0, 0 2, 2 2), (0 0, 2 2))");
}

TEST_F(LineDissolverTest, EmptyVectorBeforeGeometryLeavesFactoryDeferred) {
  LineDissolver dissolver;
  dissolver.add(std::vector<const Geometry*>{});
  std::unique_ptr<Geometry> line = ReadWkt("LINESTRING (0 0, 2 0)");
  dissolver.add(line.get());

  std::unique_ptr<Geometry> actual = dissolver.getResult();
  ExpectNormalizedEquals(actual.get(), "LINESTRING (0 0, 2 0)");
}

TEST_F(LineDissolverTest, PreservesInputFactorySridAndPrecisionModel) {
  geos::geom::PrecisionModel pm(100.0);
  GeometryFactory::Ptr custom_factory = GeometryFactory::create(&pm, 4326);
  WKTReader custom_reader(custom_factory.get());

  std::unique_ptr<Geometry> g1 = custom_reader.read("LINESTRING (0 0, 1.5 0)");
  std::unique_ptr<Geometry> g2 = custom_reader.read("LINESTRING (1.5 0, 3 0)");

  LineDissolver dissolver;
  dissolver.add(g1.get());
  dissolver.add(g2.get());
  std::unique_ptr<Geometry> actual = dissolver.getResult();

  ASSERT_NE(actual, nullptr);
  EXPECT_EQ(actual->getFactory(), custom_factory.get());
  EXPECT_EQ(actual->getSRID(), 4326);
  ExpectNormalizedEquals(actual.get(), "LINESTRING (0 0, 1.5 0, 3 0)");
}

TEST_F(LineDissolverTest,
       PreservesXYZAndXYMOrdinatesWithMissingOrdinatesAsNaN) {
  std::unique_ptr<Geometry> xyz1 = ReadWkt("LINESTRING Z (0 0 10, 1 0 20)");
  std::unique_ptr<Geometry> xyz2 = ReadWkt("LINESTRING Z (2 0 30, 1 0 20)");
  LineDissolver xyz_dissolver;
  xyz_dissolver.add(xyz1.get());
  xyz_dissolver.add(xyz2.get());
  std::unique_ptr<Geometry> xyz_out = xyz_dissolver.getResult();
  ASSERT_NE(xyz_out, nullptr);
  const auto* xyz_ls = dynamic_cast<const LineString*>(xyz_out.get());
  ASSERT_NE(xyz_ls, nullptr);
  const CoordinateSequence* xyz_seq = xyz_ls->getCoordinatesRO();
  ASSERT_EQ(xyz_seq->size(), 3u);
  EXPECT_DOUBLE_EQ(xyz_seq->getX(0), 0.0);
  EXPECT_DOUBLE_EQ(xyz_seq->getOrdinate(0, CoordinateSequence::Z), 10.0);
  EXPECT_TRUE(std::isnan(xyz_seq->getOrdinate(0, CoordinateSequence::M)));
  EXPECT_DOUBLE_EQ(xyz_seq->getX(1), 1.0);
  EXPECT_DOUBLE_EQ(xyz_seq->getOrdinate(1, CoordinateSequence::Z), 20.0);
  EXPECT_TRUE(std::isnan(xyz_seq->getOrdinate(1, CoordinateSequence::M)));
  EXPECT_DOUBLE_EQ(xyz_seq->getX(2), 2.0);
  EXPECT_DOUBLE_EQ(xyz_seq->getOrdinate(2, CoordinateSequence::Z), 30.0);
  EXPECT_TRUE(std::isnan(xyz_seq->getOrdinate(2, CoordinateSequence::M)));

  std::unique_ptr<Geometry> xym = ReadWkt("LINESTRING M (0 0 100, 1 1 200)");
  std::unique_ptr<Geometry> xym_out = LineDissolver::dissolve(xym.get());
  ASSERT_NE(xym_out, nullptr);
  const auto* xym_ls = dynamic_cast<const LineString*>(xym_out.get());
  ASSERT_NE(xym_ls, nullptr);
  const CoordinateSequence* xym_seq = xym_ls->getCoordinatesRO();
  ASSERT_EQ(xym_seq->size(), 2u);
  EXPECT_TRUE(std::isnan(xym_seq->getOrdinate(0, CoordinateSequence::Z)));
  EXPECT_DOUBLE_EQ(xym_seq->getOrdinate(0, CoordinateSequence::M), 100.0);
  EXPECT_TRUE(std::isnan(xym_seq->getOrdinate(1, CoordinateSequence::Z)));
  EXPECT_DOUBLE_EQ(xym_seq->getOrdinate(1, CoordinateSequence::M), 200.0);
}

}  // namespace
}  // namespace geos_google
