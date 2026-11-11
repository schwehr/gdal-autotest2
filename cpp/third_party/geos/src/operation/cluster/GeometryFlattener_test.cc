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

// Unit tests for third_party/geos/src/operation/cluster/GeometryFlattener.cpp.
//
// google3 notes:
// - Uses isolated GeometryFactory::create() instances per test.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/cluster/GeometryFlattener.cpp

#include "third_party/geos/include/geos/operation/cluster/GeometryFlattener.h"

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GeometryTypeId;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::operation::cluster::GeometryFlattener;

struct PassthroughCase {
  std::string_view name;
  std::string_view wkt;
  GeometryTypeId expected_type;
  bool expected_empty;
};

struct FlattenCase {
  std::string_view name;
  std::string_view input_wkt;
  std::string_view expected_wkt;
  GeometryTypeId expected_type;
};

constexpr auto kNonCollectionCases = std::to_array<PassthroughCase>({
    {
        .name = "point",
        .wkt = "POINT (1 2)",
        .expected_type = geos::geom::GEOS_POINT,
        .expected_empty = false,
    },
    {
        .name = "empty_point",
        .wkt = "POINT EMPTY",
        .expected_type = geos::geom::GEOS_POINT,
        .expected_empty = true,
    },
    {
        .name = "linestring",
        .wkt = "LINESTRING (0 0, 3 4)",
        .expected_type = geos::geom::GEOS_LINESTRING,
        .expected_empty = false,
    },
    {
        .name = "empty_linestring",
        .wkt = "LINESTRING EMPTY",
        .expected_type = geos::geom::GEOS_LINESTRING,
        .expected_empty = true,
    },
    {
        .name = "linear_ring",
        .wkt = "LINEARRING (0 0, 1 0, 1 1, 0 0)",
        .expected_type = geos::geom::GEOS_LINEARRING,
        .expected_empty = false,
    },
    {
        .name = "polygon",
        .wkt = "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
        .expected_type = geos::geom::GEOS_POLYGON,
        .expected_empty = false,
    },
    {
        .name = "empty_polygon",
        .wkt = "POLYGON EMPTY",
        .expected_type = geos::geom::GEOS_POLYGON,
        .expected_empty = true,
    },
    {
        .name = "circular_string",
        .wkt = "CIRCULARSTRING (0 0, 1 1, 2 0)",
        .expected_type = geos::geom::GEOS_CIRCULARSTRING,
        .expected_empty = false,
    },
});

constexpr auto kEmptyCollectionCases = std::to_array<PassthroughCase>({
    {
        .name = "empty_geometrycollection",
        .wkt = "GEOMETRYCOLLECTION EMPTY",
        .expected_type = geos::geom::GEOS_GEOMETRYCOLLECTION,
        .expected_empty = true,
    },
    {
        .name = "empty_multipoint",
        .wkt = "MULTIPOINT EMPTY",
        .expected_type = geos::geom::GEOS_MULTIPOINT,
        .expected_empty = true,
    },
    {
        .name = "empty_multilinestring",
        .wkt = "MULTILINESTRING EMPTY",
        .expected_type = geos::geom::GEOS_MULTILINESTRING,
        .expected_empty = true,
    },
    {
        .name = "empty_multipolygon",
        .wkt = "MULTIPOLYGON EMPTY",
        .expected_type = geos::geom::GEOS_MULTIPOLYGON,
        .expected_empty = true,
    },
    {
        .name = "collection_of_only_empty_point",
        .wkt = "GEOMETRYCOLLECTION (POINT EMPTY)",
        .expected_type = geos::geom::GEOS_GEOMETRYCOLLECTION,
        .expected_empty = true,
    },
    {
        .name = "nested_empty_collections",
        .wkt =
            "GEOMETRYCOLLECTION (GEOMETRYCOLLECTION EMPTY, MULTIPOINT EMPTY)",
        .expected_type = geos::geom::GEOS_GEOMETRYCOLLECTION,
        .expected_empty = true,
    },
});

constexpr auto kFlattenCases = std::to_array<FlattenCase>({
    {
        .name = "single_point_in_collection",
        .input_wkt = "GEOMETRYCOLLECTION (POINT (1 2))",
        .expected_wkt = "POINT (1 2)",
        .expected_type = geos::geom::GEOS_POINT,
    },
    {
        .name = "single_point_in_multipoint",
        .input_wkt = "MULTIPOINT ((1 2))",
        .expected_wkt = "POINT (1 2)",
        .expected_type = geos::geom::GEOS_POINT,
    },
    {
        .name = "single_linestring_in_multilinestring",
        .input_wkt = "MULTILINESTRING ((0 0, 1 1))",
        .expected_wkt = "LINESTRING (0 0, 1 1)",
        .expected_type = geos::geom::GEOS_LINESTRING,
    },
    {
        .name = "single_polygon_in_multipolygon",
        .input_wkt = "MULTIPOLYGON (((0 0, 1 0, 1 1, 0 1, 0 0)))",
        .expected_wkt = "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))",
        .expected_type = geos::geom::GEOS_POLYGON,
    },
    {
        .name = "deeply_nested_single_polygon",
        .input_wkt =
            "GEOMETRYCOLLECTION (GEOMETRYCOLLECTION (MULTIPOLYGON (((0 0, 4 0, "
            "4 4, 0 4, 0 0)))))",
        .expected_wkt = "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))",
        .expected_type = geos::geom::GEOS_POLYGON,
    },
    {
        .name = "single_nonempty_with_empty_subcollections",
        .input_wkt =
            "GEOMETRYCOLLECTION (GEOMETRYCOLLECTION EMPTY, MULTIPOLYGON EMPTY, "
            "POINT (3 4))",
        .expected_wkt = "POINT (3 4)",
        .expected_type = geos::geom::GEOS_POINT,
    },
    {
        .name = "homogeneous_points_to_multipoint",
        .input_wkt =
            "GEOMETRYCOLLECTION (POINT (0 0), MULTIPOINT ((1 1), (2 2)))",
        .expected_wkt = "MULTIPOINT ((0 0), (1 1), (2 2))",
        .expected_type = geos::geom::GEOS_MULTIPOINT,
    },
    {
        .name = "point_and_empty_point_to_multipoint",
        .input_wkt = "GEOMETRYCOLLECTION (POINT (1 2), POINT EMPTY)",
        .expected_wkt = "MULTIPOINT ((1 2), EMPTY)",
        .expected_type = geos::geom::GEOS_MULTIPOINT,
    },
    {
        .name = "homogeneous_linestrings_to_multilinestring",
        .input_wkt =
            "GEOMETRYCOLLECTION (LINESTRING (0 0, 1 0), MULTILINESTRING ((1 0, "
            "1 1), (1 1, 0 1)))",
        .expected_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 1 1), (1 1, 0 1))",
        .expected_type = geos::geom::GEOS_MULTILINESTRING,
    },
    {
        .name = "homogeneous_polygons_to_multipolygon",
        .input_wkt =
            "GEOMETRYCOLLECTION (POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)), "
            "MULTIPOLYGON (((2 0, 3 0, 3 1, 2 1, 2 0)), ((4 0, 5 0, 5 1, 4 1, "
            "4 0))))",
        .expected_wkt =
            "MULTIPOLYGON (((0 0, 1 0, 1 1, 0 1, 0 0)), ((2 0, 3 0, 3 1, 2 1, "
            "2 0)), ((4 0, 5 0, 5 1, 4 1, 4 0)))",
        .expected_type = geos::geom::GEOS_MULTIPOLYGON,
    },
    {
        .name = "flat_multipolygon_remains_multipolygon",
        .input_wkt =
            "MULTIPOLYGON (((0 0, 1 0, 1 1, 0 1, 0 0)), ((2 0, 3 0, 3 1, 2 1, "
            "2 0)))",
        .expected_wkt =
            "MULTIPOLYGON (((0 0, 1 0, 1 1, 0 1, 0 0)), ((2 0, 3 0, 3 1, 2 1, "
            "2 0)))",
        .expected_type = geos::geom::GEOS_MULTIPOLYGON,
    },
    {
        .name = "heterogeneous_nested_collections",
        .input_wkt =
            "GEOMETRYCOLLECTION (MULTIPOINT ((0 0), (1 1)), MULTILINESTRING "
            "((0 0, 2 2)), MULTIPOLYGON (((3 3, 4 3, 4 4, 3 4, 3 3))))",
        .expected_wkt =
            "GEOMETRYCOLLECTION (POINT (0 0), POINT (1 1), LINESTRING (0 0, 2 "
            "2), POLYGON ((3 3, 4 3, 4 4, 3 4, 3 3)))",
        .expected_type = geos::geom::GEOS_GEOMETRYCOLLECTION,
    },
    {
        .name = "deeply_nested_mixed_with_empty_subcollection",
        .input_wkt =
            "GEOMETRYCOLLECTION (GEOMETRYCOLLECTION (POINT (1 1), "
            "GEOMETRYCOLLECTION EMPTY), MULTILINESTRING ((0 0, 1 1), (2 2, 3 "
            "3)), GEOMETRYCOLLECTION (POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))))",
        .expected_wkt =
            "GEOMETRYCOLLECTION (POINT (1 1), LINESTRING (0 0, 1 1), "
            "LINESTRING (2 2, 3 3), POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)))",
        .expected_type = geos::geom::GEOS_GEOMETRYCOLLECTION,
    },
});

TEST(GeometryFlattenerTest, PreservesNonCollectionGeometries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (const auto& test_case : kNonCollectionCases) {
    SCOPED_TRACE(test_case.name);
    Geometry::Ptr input = reader.read(std::string(test_case.wkt));
    Geometry::Ptr expected = reader.read(std::string(test_case.wkt));
    const Geometry* raw_ptr = input.get();

    Geometry::Ptr flattened = GeometryFlattener::flatten(std::move(input));
    ASSERT_NE(flattened, nullptr);
    EXPECT_EQ(flattened.get(), raw_ptr);
    EXPECT_EQ(flattened->getGeometryTypeId(), test_case.expected_type);
    EXPECT_EQ(flattened->isEmpty(), test_case.expected_empty);
    EXPECT_TRUE(flattened->equalsIdentical(expected.get()));
  }
}

TEST(GeometryFlattenerTest, PreservesEmptyCollections) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (const auto& test_case : kEmptyCollectionCases) {
    SCOPED_TRACE(test_case.name);
    Geometry::Ptr input = reader.read(std::string(test_case.wkt));
    Geometry::Ptr expected = reader.read(std::string(test_case.wkt));
    const Geometry* raw_ptr = input.get();

    Geometry::Ptr flattened = GeometryFlattener::flatten(std::move(input));
    ASSERT_NE(flattened, nullptr);
    EXPECT_EQ(flattened.get(), raw_ptr);
    EXPECT_EQ(flattened->getGeometryTypeId(), test_case.expected_type);
    EXPECT_TRUE(flattened->isEmpty());
    EXPECT_TRUE(flattened->equalsIdentical(expected.get()));
  }
}

TEST(GeometryFlattenerTest, FlattensAndNarrowsToExpectedRepresentation) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (const auto& test_case : kFlattenCases) {
    SCOPED_TRACE(test_case.name);
    Geometry::Ptr input = reader.read(std::string(test_case.input_wkt));
    Geometry::Ptr expected = reader.read(std::string(test_case.expected_wkt));

    Geometry::Ptr flattened = GeometryFlattener::flatten(std::move(input));
    ASSERT_NE(flattened, nullptr);
    EXPECT_EQ(flattened->getGeometryTypeId(), test_case.expected_type);
    EXPECT_TRUE(flattened->equalsExact(expected.get()));

    if (flattened->isCollection()) {
      for (std::size_t i = 0; i < flattened->getNumGeometries(); ++i) {
        EXPECT_FALSE(flattened->getGeometryN(i)->isCollection());
      }
    }
  }
}

TEST(GeometryFlattenerTest, FlattensLinearRings) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr single_ring_gc =
      reader.read("GEOMETRYCOLLECTION (LINEARRING (0 0, 1 0, 1 1, 0 0))");
  Geometry::Ptr flattened_single =
      GeometryFlattener::flatten(std::move(single_ring_gc));
  ASSERT_NE(flattened_single, nullptr);
  EXPECT_EQ(flattened_single->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);

  Geometry::Ptr two_rings_gc = reader.read(
      "GEOMETRYCOLLECTION (LINEARRING (0 0, 1 0, 1 1, 0 0), "
      "LINEARRING (2 2, 3 2, 3 3, 2 2))");
  Geometry::Ptr flattened_multi =
      GeometryFlattener::flatten(std::move(two_rings_gc));
  ASSERT_NE(flattened_multi, nullptr);
  EXPECT_EQ(flattened_multi->getGeometryTypeId(),
            geos::geom::GEOS_MULTILINESTRING);
  ASSERT_EQ(flattened_multi->getNumGeometries(), 2);
  EXPECT_EQ(flattened_multi->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_LINEARRING);
  EXPECT_EQ(flattened_multi->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_LINEARRING);
}

TEST(GeometryFlattenerTest, PreservesZAndMCoordinates) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  constexpr auto kZmCases = std::to_array<FlattenCase>({
      {
          .name = "xyz_collection_to_multipoint",
          .input_wkt =
              "GEOMETRYCOLLECTION Z (MULTIPOINT Z ((1 2 3)), POINT Z (4 5 6))",
          .expected_wkt = "MULTIPOINT Z ((1 2 3), (4 5 6))",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "xym_collection_to_multipoint",
          .input_wkt =
              "GEOMETRYCOLLECTION M (POINT M (1 2 10), MULTIPOINT M ((3 4 "
              "20)))",
          .expected_wkt = "MULTIPOINT M ((1 2 10), (3 4 20))",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "xyzm_nested_collection_to_linestring",
          .input_wkt =
              "GEOMETRYCOLLECTION ZM (GEOMETRYCOLLECTION ZM (LINESTRING ZM (0 "
              "0 1 2, 1 1 3 4)))",
          .expected_wkt = "LINESTRING ZM (0 0 1 2, 1 1 3 4)",
          .expected_type = geos::geom::GEOS_LINESTRING,
      },
  });

  for (const auto& test_case : kZmCases) {
    SCOPED_TRACE(test_case.name);
    Geometry::Ptr input = reader.read(std::string(test_case.input_wkt));
    Geometry::Ptr expected = reader.read(std::string(test_case.expected_wkt));

    Geometry::Ptr flattened = GeometryFlattener::flatten(std::move(input));
    ASSERT_NE(flattened, nullptr);
    EXPECT_EQ(flattened->getGeometryTypeId(), test_case.expected_type);
    EXPECT_EQ(flattened->hasZ(), expected->hasZ());
    EXPECT_EQ(flattened->hasM(), expected->hasM());
    EXPECT_TRUE(flattened->equalsIdentical(expected.get()));
  }
}

TEST(GeometryFlattenerTest, PreservesFactoryPrecisionModelAndSrid) {
  PrecisionModel pm(100.0);
  GeometryFactory::Ptr factory = GeometryFactory::create(&pm, 4326);
  WKTReader reader(factory.get());

  Geometry::Ptr input = reader.read(
      "GEOMETRYCOLLECTION (POINT (1.23 4.56), MULTIPOINT ((7.89 0.12)))");
  Geometry::Ptr flattened = GeometryFlattener::flatten(std::move(input));

  ASSERT_NE(flattened, nullptr);
  EXPECT_EQ(flattened->getFactory(), factory.get());
  EXPECT_EQ(flattened->getSRID(), 4326);
  EXPECT_DOUBLE_EQ(flattened->getPrecisionModel()->getScale(), 100.0);
  EXPECT_EQ(flattened->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
}

}  // namespace
}  // namespace geos_google
