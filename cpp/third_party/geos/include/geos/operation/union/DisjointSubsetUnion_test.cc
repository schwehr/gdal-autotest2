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
// third_party/geos/include/geos/operation/union/DisjointSubsetUnion.h.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - Allocates isolated GeometryFactory instances via GeometryFactory::create()
//   rather than GeometryFactory::getDefaultInstance().
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/operation/union/DisjointSubsetUnion.h

#include "third_party/geos/include/geos/operation/union/DisjointSubsetUnion.h"

#include <array>
#include <cstddef>
#include <memory>
#include <numbers>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/BoundaryNodeRule.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/valid/IsSimpleOp.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::algorithm::BoundaryNodeRule;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GeometryTypeId;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::operation::geounion::DisjointSubsetUnion;
using geos::operation::valid::IsSimpleOp;
using geos::util::UnsupportedOperationException;

struct EmptyUnionCase {
  std::string_view name;
  std::string_view input_wkt;
};

constexpr auto kEmptyCases = std::to_array<EmptyUnionCase>({
    {.name = "empty_point", .input_wkt = "POINT EMPTY"},
    {.name = "empty_linestring", .input_wkt = "LINESTRING EMPTY"},
    {.name = "empty_polygon", .input_wkt = "POLYGON EMPTY"},
    {.name = "empty_multipoint", .input_wkt = "MULTIPOINT EMPTY"},
    {.name = "empty_multilinestring", .input_wkt = "MULTILINESTRING EMPTY"},
    {.name = "empty_multipolygon", .input_wkt = "MULTIPOLYGON EMPTY"},
    {.name = "empty_geometrycollection",
     .input_wkt = "GEOMETRYCOLLECTION EMPTY"},
});

void ExpectNormalizedEqual(const Geometry& actual, const Geometry& expected,
                           double tolerance = 1e-12) {
  Geometry::Ptr actual_norm = actual.clone();
  Geometry::Ptr expected_norm = expected.clone();
  actual_norm->normalize();
  expected_norm->normalize();
  EXPECT_TRUE(actual_norm->equalsExact(expected_norm.get(), tolerance))
      << "actual=" << actual_norm->toText()
      << "\nexpected=" << expected_norm->toText();
}

TEST(DisjointSubsetUnionTest, HandlesEmptyGeometriesAndCollections) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  for (const auto& tc : kEmptyCases) {
    SCOPED_TRACE(std::string(tc.name));
    Geometry::Ptr input = reader.read(std::string(tc.input_wkt));
    ASSERT_NE(input, nullptr);

    Geometry::Ptr result = DisjointSubsetUnion::Union(input.get());
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->isEmpty());
    EXPECT_DOUBLE_EQ(result->getArea(), 0.0);
    EXPECT_DOUBLE_EQ(result->getLength(), 0.0);
    EXPECT_EQ(result->getFactory(), factory.get());
  }
}

struct SingleGeometryCase {
  std::string_view name;
  std::string_view input_wkt;
  std::string_view expected_wkt;
  GeometryTypeId expected_type;
  double expected_area;
  double expected_length;
};

constexpr auto kSingleGeometryCases = std::to_array<SingleGeometryCase>({
    {
        .name = "single_point",
        .input_wkt = "POINT (3 4)",
        .expected_wkt = "POINT (3 4)",
        .expected_type = GeometryTypeId::GEOS_POINT,
        .expected_area = 0.0,
        .expected_length = 0.0,
    },
    {
        .name = "single_polygon_with_hole",
        .input_wkt = "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), "
                     "(1 1, 1 3, 3 3, 3 1, 1 1))",
        .expected_wkt = "POLYGON ((0 0, 0 4, 4 4, 4 0, 0 0), "
                        "(1 1, 3 1, 3 3, 1 3, 1 1))",
        .expected_type = GeometryTypeId::GEOS_POLYGON,
        .expected_area = 12.0,
        .expected_length = 24.0,
    },
    {
        .name = "single_element_multipolygon",
        .input_wkt = "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)))",
        .expected_wkt = "POLYGON ((0 0, 0 2, 2 2, 2 0, 0 0))",
        .expected_type = GeometryTypeId::GEOS_POLYGON,
        .expected_area = 4.0,
        .expected_length = 8.0,
    },
    {
        .name = "single_element_geometrycollection",
        .input_wkt = "GEOMETRYCOLLECTION (POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0)))",
        .expected_wkt = "POLYGON ((0 0, 0 2, 2 2, 2 0, 0 0))",
        .expected_type = GeometryTypeId::GEOS_POLYGON,
        .expected_area = 4.0,
        .expected_length = 8.0,
    },
});

TEST(DisjointSubsetUnionTest, HandlesSingleComponentInputs) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  for (const auto& tc : kSingleGeometryCases) {
    SCOPED_TRACE(std::string(tc.name));
    Geometry::Ptr input = reader.read(std::string(tc.input_wkt));
    Geometry::Ptr expected = reader.read(std::string(tc.expected_wkt));

    Geometry::Ptr result = DisjointSubsetUnion::Union(input.get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getGeometryTypeId(), tc.expected_type);
    EXPECT_TRUE(result->isValid());
    EXPECT_DOUBLE_EQ(result->getArea(), tc.expected_area);
    EXPECT_DOUBLE_EQ(result->getLength(), tc.expected_length);
    ExpectNormalizedEqual(*result, *expected);
  }
}

TEST(DisjointSubsetUnionTest, NodesSelfIntersectingSingleLineString) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Noding a self-intersecting LineString at (1, 1) emits a closed cycle
  // (1 1, 2 2, 2 0, 1 1) plus two open branches, which is simple under the
  // Endpoint boundary rule (not under OGC SFS Mod-2 where rings lack boundary).
  Geometry::Ptr input = reader.read("LINESTRING (0 0, 2 2, 2 0, 0 2)");
  Geometry::Ptr result = DisjointSubsetUnion::Union(input.get());
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_MULTILINESTRING);
  EXPECT_EQ(result->getNumGeometries(), 3u);
  EXPECT_FALSE(result->isSimple());
  EXPECT_TRUE(
      IsSimpleOp(*result, BoundaryNodeRule::getBoundaryEndPoint()).isSimple());
  EXPECT_NEAR(result->getLength(), 2.0 + 4.0 * std::numbers::sqrt2, 1e-12);

  Geometry::Ptr expected = reader.read(
      "MULTILINESTRING ((0 0, 1 1), (1 1, 2 2, 2 0, 1 1), (0 2, 1 1))");
  ExpectNormalizedEqual(*result, *expected);

  Geometry::Ptr direct_union = input->Union();
  ExpectNormalizedEqual(*result, *direct_union);

  // A retracing LineString dissolves coincident segments and is Mod-2 simple.
  Geometry::Ptr retrace = reader.read("LINESTRING (0 0, 2 0, 1 0)");
  Geometry::Ptr retrace_result = DisjointSubsetUnion::Union(retrace.get());
  ASSERT_NE(retrace_result, nullptr);
  EXPECT_EQ(retrace_result->getGeometryTypeId(),
            GeometryTypeId::GEOS_MULTILINESTRING);
  EXPECT_EQ(retrace_result->getNumGeometries(), 2u);
  EXPECT_TRUE(retrace_result->isSimple());
  EXPECT_DOUBLE_EQ(retrace_result->getLength(), 2.0);
  ExpectNormalizedEqual(*retrace_result, *retrace->Union());
}

TEST(DisjointSubsetUnionTest, DeduplicatesMultiPointAndCollapsesIdentical) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  {
    Geometry::Ptr input =
        reader.read("MULTIPOINT ((0 0), (0 0), (3 4), (0 0), (-1 2))");
    Geometry::Ptr expected = reader.read("MULTIPOINT ((-1 2), (0 0), (3 4))");

    Geometry::Ptr result = DisjointSubsetUnion::Union(input.get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_MULTIPOINT);
    EXPECT_EQ(result->getNumGeometries(), 3u);
    EXPECT_TRUE(result->isSimple());
    ExpectNormalizedEqual(*result, *expected);
  }

  {
    Geometry::Ptr input = reader.read("MULTIPOINT ((2 5), (2 5), (2 5))");
    Geometry::Ptr expected = reader.read("POINT (2 5)");

    Geometry::Ptr result = DisjointSubsetUnion::Union(input.get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_POINT);
    EXPECT_EQ(result->getNumGeometries(), 1u);
    ExpectNormalizedEqual(*result, *expected);
  }
}

TEST(DisjointSubsetUnionTest, NodesAndDissolvesMultiLineStringSubsets) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  Geometry::Ptr input = reader.read(
      "MULTILINESTRING ((0 0, 4 0), (2 -2, 2 2), (10 0, 12 0), (10 0, 12 0))");
  Geometry::Ptr result = DisjointSubsetUnion::Union(input.get());
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_MULTILINESTRING);
  EXPECT_EQ(result->getNumGeometries(), 5u);
  EXPECT_TRUE(result->isSimple());
  EXPECT_DOUBLE_EQ(result->getLength(), 10.0);

  Geometry::Ptr direct_union = input->Union();
  ExpectNormalizedEqual(*result, *direct_union);
}

struct MultiPolygonCase {
  std::string_view name;
  std::string_view input_wkt;
  std::string_view expected_wkt;
  GeometryTypeId expected_type;
  std::size_t expected_num_geometries;
  double expected_area;
};

constexpr auto kMultiPolygonCases = std::to_array<MultiPolygonCase>({
    {
        .name = "overlapping_pair_plus_disjoint",
        .input_wkt = "MULTIPOLYGON ("
                     "((0 0, 2 0, 2 2, 0 2, 0 0)), "
                     "((1 0, 3 0, 3 2, 1 2, 1 0)), "
                     "((10 10, 12 10, 12 12, 10 12, 10 10)))",
        .expected_wkt = "MULTIPOLYGON ("
                        "((0 0, 0 2, 3 2, 3 0, 0 0)), "
                        "((10 10, 10 12, 12 12, 12 10, 10 10)))",
        .expected_type = GeometryTypeId::GEOS_MULTIPOLYGON,
        .expected_num_geometries = 2,
        .expected_area = 10.0,
    },
    {
        .name = "transitive_touching_chain",
        .input_wkt = "MULTIPOLYGON ("
                     "((0 0, 2 0, 2 2, 0 2, 0 0)), "
                     "((4 0, 6 0, 6 2, 4 2, 4 0)), "
                     "((2 0, 4 0, 4 2, 2 2, 2 0)))",
        .expected_wkt = "POLYGON ((0 0, 0 2, 6 2, 6 0, 0 0))",
        .expected_type = GeometryTypeId::GEOS_POLYGON,
        .expected_num_geometries = 1,
        .expected_area = 12.0,
    },
    {
        .name = "enclosing_hole_plus_disjoint",
        .input_wkt = "MULTIPOLYGON ("
                     "((0 0, 3 0, 3 1, 0 1, 0 0)), "
                     "((0 2, 3 2, 3 3, 0 3, 0 2)), "
                     "((0 1, 1 1, 1 2, 0 2, 0 1)), "
                     "((2 1, 3 1, 3 2, 2 2, 2 1)), "
                     "((10 10, 11 10, 11 11, 10 11, 10 10)))",
        .expected_wkt = "MULTIPOLYGON ("
                        "((0 0, 0 3, 3 3, 3 0, 0 0), "
                        "(1 1, 2 1, 2 2, 1 2, 1 1)), "
                        "((10 10, 10 11, 11 11, 11 10, 10 10)))",
        .expected_type = GeometryTypeId::GEOS_MULTIPOLYGON,
        .expected_num_geometries = 2,
        .expected_area = 9.0,
    },
    {
        .name = "vertex_touching_pair_plus_disjoint",
        .input_wkt = "MULTIPOLYGON ("
                     "((0 0, 1 0, 1 1, 0 1, 0 0)), "
                     "((1 1, 2 1, 2 2, 1 2, 1 1)), "
                     "((10 0, 11 0, 11 1, 10 1, 10 0)))",
        .expected_wkt = "MULTIPOLYGON ("
                        "((0 0, 0 1, 1 1, 1 0, 0 0)), "
                        "((1 1, 1 2, 2 2, 2 1, 1 1)), "
                        "((10 0, 10 1, 11 1, 11 0, 10 0)))",
        .expected_type = GeometryTypeId::GEOS_MULTIPOLYGON,
        .expected_num_geometries = 3,
        .expected_area = 3.0,
    },
});

TEST(DisjointSubsetUnionTest, UnionsPolygonSubsetsCorrectly) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  for (const auto& tc : kMultiPolygonCases) {
    SCOPED_TRACE(std::string(tc.name));
    Geometry::Ptr input = reader.read(std::string(tc.input_wkt));
    Geometry::Ptr expected = reader.read(std::string(tc.expected_wkt));

    Geometry::Ptr result = DisjointSubsetUnion::Union(input.get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getGeometryTypeId(), tc.expected_type);
    EXPECT_EQ(result->getNumGeometries(), tc.expected_num_geometries);
    EXPECT_TRUE(result->isValid());
    EXPECT_DOUBLE_EQ(result->getArea(), tc.expected_area);
    EXPECT_DOUBLE_EQ(result->getLength(), expected->getLength());
    // OverlayNG preserves collinear noded vertices along merged edges, so use
    // topological equality rather than vertex-exact equalsExact.
    EXPECT_TRUE(result->equals(expected.get()));

    Geometry::Ptr direct_union = input->Union();
    EXPECT_TRUE(result->equals(direct_union.get()));
  }
}

TEST(DisjointSubsetUnionTest, SplitsNestedCollectionsBeforeClustering) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Overlapping components live across separate nested MultiPolygons and a
  // nested GeometryCollection, verifying setSplitInputs(true) flattens inputs.
  Geometry::Ptr input = reader.read(
      "GEOMETRYCOLLECTION ("
      "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "((20 0, 21 0, 21 1, 20 1, 20 0))), "
      "GEOMETRYCOLLECTION (POLYGON ((20 0, 22 0, 22 1, 20 1, 20 0))), "
      "MULTIPOLYGON (((1 0, 3 0, 3 2, 1 2, 1 0)), "
      "((40 0, 41 0, 41 1, 40 1, 40 0))))");
  Geometry::Ptr expected = reader.read(
      "MULTIPOLYGON ("
      "((0 0, 0 2, 3 2, 3 0, 0 0)), "
      "((20 0, 20 1, 22 1, 22 0, 20 0)), "
      "((40 0, 40 1, 41 1, 41 0, 40 0)))");

  Geometry::Ptr result = DisjointSubsetUnion::Union(input.get());
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_MULTIPOLYGON);
  EXPECT_EQ(result->getNumGeometries(), 3u);
  EXPECT_TRUE(result->isValid());
  EXPECT_DOUBLE_EQ(result->getArea(), 9.0);
  EXPECT_DOUBLE_EQ(result->getLength(), expected->getLength());
  EXPECT_TRUE(result->equals(expected.get()));

  Geometry::Ptr direct_union = input->Union();
  EXPECT_TRUE(result->equals(direct_union.get()));
}

TEST(DisjointSubsetUnionTest, UnionsMixedDimensionsAndAbsorbsCoveredParts) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  Geometry::Ptr input = reader.read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "POINT (1 1), "
      "LINESTRING (1 1, 4 1), "
      "POINT (10 10), "
      "LINESTRING (20 0, 20 3))");

  Geometry::Ptr result = DisjointSubsetUnion::Union(input.get());
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getGeometryTypeId(),
            GeometryTypeId::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(result->getNumGeometries(), 4u);
  EXPECT_TRUE(result->isValid());
  EXPECT_DOUBLE_EQ(result->getArea(), 4.0);
  EXPECT_DOUBLE_EQ(result->getLength(), 13.0);

  Geometry::Ptr direct_union = input->Union();
  EXPECT_TRUE(result->equals(direct_union.get()));
  ExpectNormalizedEqual(*result, *direct_union);
}

TEST(DisjointSubsetUnionTest, HandlesCollectionWithEmptyElements) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  {
    Geometry::Ptr input = reader.read(
        "GEOMETRYCOLLECTION ("
        "POINT EMPTY, "
        "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0)), "
        "LINESTRING EMPTY, "
        "POLYGON ((1 0, 3 0, 3 2, 1 2, 1 0)))");

    Geometry::Ptr result = DisjointSubsetUnion::Union(input.get());
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->isValid());
    EXPECT_DOUBLE_EQ(result->getArea(), 6.0);

    Geometry::Ptr expected = reader.read("POLYGON ((0 0, 0 2, 3 2, 3 0, 0 0))");
    EXPECT_TRUE(result->equals(expected.get()));
  }

  {
    Geometry::Ptr only_empty = reader.read(
        "GEOMETRYCOLLECTION (POINT EMPTY, LINESTRING EMPTY, POLYGON EMPTY)");
    Geometry::Ptr result = DisjointSubsetUnion::Union(only_empty.get());
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->isEmpty());
    EXPECT_DOUBLE_EQ(result->getArea(), 0.0);
    EXPECT_DOUBLE_EQ(result->getLength(), 0.0);
    EXPECT_EQ(result->getFactory(), factory.get());
  }
}

TEST(DisjointSubsetUnionTest, UnionsManyDisjointClustersAcrossTreeLevels) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Build 12 disjoint clusters (24 overlapping rectangles) arranged in a 4x3
  // grid so the underlying TemplateSTRtree exceeds its default node capacity.
  std::vector<Geometry::Ptr> squares;
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 4; ++col) {
      const double x = static_cast<double>(col) * 4.0;
      const double y = static_cast<double>(row) * 4.0;
      std::ostringstream left;
      left << "POLYGON ((" << x << ' ' << y << ", " << (x + 1.5) << ' ' << y
           << ", " << (x + 1.5) << ' ' << (y + 1.0) << ", " << x << ' '
           << (y + 1.0) << ", " << x << ' ' << y << "))";
      std::ostringstream right;
      right << "POLYGON ((" << (x + 0.5) << ' ' << y << ", " << (x + 2.0) << ' '
            << y << ", " << (x + 2.0) << ' ' << (y + 1.0) << ", " << (x + 0.5)
            << ' ' << (y + 1.0) << ", " << (x + 0.5) << ' ' << y << "))";
      Geometry::Ptr left_poly = reader.read(left.str());
      Geometry::Ptr right_poly = reader.read(right.str());
      ASSERT_NE(left_poly, nullptr);
      ASSERT_NE(right_poly, nullptr);
      squares.push_back(std::move(left_poly));
      squares.push_back(std::move(right_poly));
    }
  }

  Geometry::Ptr multi = factory->createMultiPolygon(std::move(squares));
  Geometry::Ptr result = DisjointSubsetUnion::Union(multi.get());
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_MULTIPOLYGON);
  EXPECT_EQ(result->getNumGeometries(), 12u);
  EXPECT_TRUE(result->isValid());
  EXPECT_DOUBLE_EQ(result->getArea(), 24.0);

  Geometry::Ptr direct_union = multi->Union();
  ExpectNormalizedEqual(*result, *direct_union);
}

TEST(DisjointSubsetUnionTest, UsesInputFactoryAndHandlesZCoordinates) {
  PrecisionModel fixed_pm(100.0);
  GeometryFactory::Ptr factory = GeometryFactory::create(&fixed_pm, 4326);
  WKTReader reader(*factory);

  Geometry::Ptr input = reader.read(
      "MULTIPOLYGON Z ("
      "((0 0 1, 2 0 1, 2 2 1, 0 2 1, 0 0 1)), "
      "((1 0 2, 3 0 2, 3 2 2, 1 2 2, 1 0 2)), "
      "((10 0 3, 11 0 3, 11 1 3, 10 1 3, 10 0 3)))");

  Geometry::Ptr result = DisjointSubsetUnion::Union(input.get());
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getFactory(), factory.get());
  EXPECT_EQ(result->getSRID(), 4326);
  EXPECT_TRUE(result->hasZ());
  EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_MULTIPOLYGON);
  EXPECT_EQ(result->getNumGeometries(), 2u);
  EXPECT_TRUE(result->isValid());
  EXPECT_DOUBLE_EQ(result->getArea(), 7.0);
  EXPECT_TRUE(result->equals(input->Union().get()));
}

TEST(DisjointSubsetUnionTest, ThrowsOnCurvedGeometryInputs) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  Geometry::Ptr curved = reader.read("CIRCULARSTRING (0 0, 1 1, 2 0)");
  EXPECT_THROW(DisjointSubsetUnion::Union(curved.get()),
               UnsupportedOperationException);

  Geometry::Ptr mixed_curved = reader.read(
      "GEOMETRYCOLLECTION (POINT (0 0), CIRCULARSTRING (10 0, 11 1, 12 0))");
  EXPECT_THROW(DisjointSubsetUnion::Union(mixed_curved.get()),
               UnsupportedOperationException);
}

}  // namespace
}  // namespace geos_google
