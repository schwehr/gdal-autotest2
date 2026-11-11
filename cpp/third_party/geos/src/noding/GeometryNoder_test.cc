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

// Unit tests for third_party/geos/src/noding/GeometryNoder.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/noding/GeometryNoder.cpp

#include "third_party/geos/include/geos/noding/GeometryNoder.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/noding/Noder.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GeometryTypeId;
using geos::geom::LineString;
using geos::geom::MultiLineString;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::noding::GeometryNoder;
using geos::util::IllegalArgumentException;
using geos::util::UnsupportedOperationException;

struct EmptyGeometryCase {
  std::string_view name;
  std::string_view wkt;
  GeometryTypeId expected_type;
};

constexpr auto kEmptyCases = std::to_array<EmptyGeometryCase>({
    {
        .name = "empty_linestring",
        .wkt = "LINESTRING EMPTY",
        .expected_type = GeometryTypeId::GEOS_LINESTRING,
    },
    {
        .name = "empty_multilinestring",
        .wkt = "MULTILINESTRING EMPTY",
        .expected_type = GeometryTypeId::GEOS_MULTILINESTRING,
    },
    {
        .name = "empty_polygon",
        .wkt = "POLYGON EMPTY",
        .expected_type = GeometryTypeId::GEOS_POLYGON,
    },
    {
        .name = "empty_geometrycollection",
        .wkt = "GEOMETRYCOLLECTION EMPTY",
        .expected_type = GeometryTypeId::GEOS_GEOMETRYCOLLECTION,
    },
    {
        .name = "empty_point",
        .wkt = "POINT EMPTY",
        .expected_type = GeometryTypeId::GEOS_POINT,
    },
});

struct NodingWktCase {
  std::string_view name;
  std::string_view input_wkt;
  std::string_view expected_wkt;
  std::size_t expected_num_geoms;
};

constexpr auto kNodingCases = std::to_array<NodingWktCase>({
    {
        .name = "crossing_lines",
        .input_wkt = "MULTILINESTRING ((0 0, 10 10), (0 10, 10 0))",
        .expected_wkt =
            "MULTILINESTRING ((0 0, 5 5), (5 5, 10 10), (0 10, 5 5), (5 5, 10 "
            "0))",
        .expected_num_geoms = 4,
    },
    {
        .name = "self_intersecting_linestring",
        .input_wkt = "LINESTRING (0 0, 10 10, 10 0, 0 10)",
        .expected_wkt =
            "MULTILINESTRING ((0 0, 5 5), (5 5, 10 10, 10 0, 5 5), (5 5, 0 "
            "10))",
        .expected_num_geoms = 3,
    },
    {
        .name = "single_non_intersecting_linestring",
        .input_wkt = "LINESTRING (0 0, 10 0, 10 5)",
        .expected_wkt = "MULTILINESTRING ((0 0, 10 0, 10 5))",
        .expected_num_geoms = 1,
    },
    {
        .name = "t_junction_endpoint_on_interior",
        .input_wkt = "MULTILINESTRING ((0 0, 10 0), (5 0, 5 5))",
        .expected_wkt = "MULTILINESTRING ((0 0, 5 0), (5 0, 10 0), (5 0, 5 5))",
        .expected_num_geoms = 3,
    },
    {
        .name = "direct_linear_ring",
        .input_wkt = "LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)",
        .expected_wkt = "MULTILINESTRING ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .expected_num_geoms = 1,
    },
    {
        .name = "backtracking_folded_linestring",
        .input_wkt = "LINESTRING (0 0, 10 0, 0 0)",
        .expected_wkt = "MULTILINESTRING ((0 0, 10 0))",
        .expected_num_geoms = 1,
    },
    {
        .name = "duplicate_identical_lines",
        .input_wkt = "MULTILINESTRING ((0 0, 10 0), (0 0, 10 0))",
        .expected_wkt = "MULTILINESTRING ((0 0, 10 0))",
        .expected_num_geoms = 1,
    },
    {
        .name = "duplicate_reversed_two_point_lines",
        .input_wkt = "MULTILINESTRING ((0 0, 10 0), (10 0, 0 0))",
        .expected_wkt = "MULTILINESTRING ((0 0, 10 0))",
        .expected_num_geoms = 1,
    },
    {
        .name = "duplicate_reversed_multi_segment_lines",
        .input_wkt = "MULTILINESTRING ((0 0, 5 5, 10 0), (10 0, 5 5, 0 0))",
        .expected_wkt = "MULTILINESTRING ((0 0, 5 5), (5 5, 10 0))",
        .expected_num_geoms = 2,
    },
    {
        .name = "partially_overlapping_reversed_lines",
        .input_wkt = "MULTILINESTRING ((0 0, 10 0), (15 0, 5 0))",
        .expected_wkt =
            "MULTILINESTRING ((0 0, 5 0), (5 0, 10 0), (10 0, 15 0))",
        .expected_num_geoms = 3,
    },
    {
        .name = "overlapping_polygons_extract_and_node_rings",
        .input_wkt = "MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0)), "
                     "((5 5, 15 5, 15 15, 5 15, 5 5)))",
        .expected_wkt =
            "MULTILINESTRING ((0 0, 10 0, 10 5), (10 5, 10 10, 5 10), "
            "(5 10, 0 10, 0 0), (5 5, 10 5), "
            "(10 5, 15 5, 15 15, 5 15, 5 10), (5 10, 5 5))",
        .expected_num_geoms = 6,
    },
    {
        .name = "nested_geometry_collection",
        .input_wkt =
            "GEOMETRYCOLLECTION (GEOMETRYCOLLECTION (LINESTRING (0 0, 10 10)), "
            "LINESTRING (0 10, 10 0))",
        .expected_wkt =
            "MULTILINESTRING ((0 0, 5 5), (5 5, 10 10), (0 10, 5 5), "
            "(5 5, 10 0))",
        .expected_num_geoms = 4,
    },
    {
        .name = "mixed_geometry_collection_ignores_point_and_nodes_rings",
        .input_wkt = "GEOMETRYCOLLECTION (POINT (100 100), "
                     "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
                     "(2 2, 8 2, 8 8, 2 8, 2 2)), "
                     "LINESTRING (-1 5, 11 5))",
        .expected_wkt =
            "MULTILINESTRING ((0 0, 10 0, 10 5), (10 5, 10 10, 0 10, 0 5), "
            "(0 5, 0 0), (2 2, 8 2, 8 5), (8 5, 8 8, 2 8, 2 5), (2 5, 2 2), "
            "(-1 5, 0 5), (0 5, 2 5), (2 5, 8 5), (8 5, 10 5), (10 5, 11 5))",
        .expected_num_geoms = 11,
    },
});

TEST(GeometryNoderTest, EmptyGeometriesReturnClonedEmptyGeometry) {
  PrecisionModel pm;
  GeometryFactory::Ptr factory = GeometryFactory::create(&pm, 3857);
  WKTReader reader(factory.get());

  for (const EmptyGeometryCase& tc : kEmptyCases) {
    SCOPED_TRACE(tc.name);
    Geometry::Ptr input = reader.read(std::string(tc.wkt));
    Geometry::Ptr noded = GeometryNoder::node(*input);
    ASSERT_NE(noded, nullptr);
    EXPECT_NE(noded.get(), input.get());
    EXPECT_TRUE(noded->isEmpty());
    EXPECT_EQ(noded->getGeometryTypeId(), tc.expected_type);
    EXPECT_EQ(noded->getFactory(), factory.get());
    EXPECT_EQ(noded->getSRID(), 3857);
  }
}

TEST(GeometryNoderTest, PuntalGeometriesReturnEmptyMultiLineString) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  constexpr auto kPuntalWkts = std::to_array<std::string_view>(
      {"POINT (1 2)", "MULTIPOINT ((0 0), (3 4))"});
  for (const std::string_view wkt : kPuntalWkts) {
    SCOPED_TRACE(wkt);
    Geometry::Ptr input = reader.read(std::string(wkt));
    Geometry::Ptr noded = GeometryNoder::node(*input);
    ASSERT_NE(noded, nullptr);
    EXPECT_EQ(noded->getGeometryTypeId(), GeometryTypeId::GEOS_MULTILINESTRING);
    EXPECT_TRUE(noded->isEmpty());
    EXPECT_EQ(noded->getNumGeometries(), 0u);
  }
}

TEST(GeometryNoderTest, NodingWktCases) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (const NodingWktCase& tc : kNodingCases) {
    SCOPED_TRACE(tc.name);
    Geometry::Ptr input = reader.read(std::string(tc.input_wkt));
    Geometry::Ptr actual = GeometryNoder::node(*input);
    Geometry::Ptr expected = reader.read(std::string(tc.expected_wkt));

    ASSERT_NE(actual, nullptr);
    EXPECT_EQ(actual->getGeometryTypeId(),
              GeometryTypeId::GEOS_MULTILINESTRING);
    EXPECT_EQ(actual->getNumGeometries(), tc.expected_num_geoms);

    actual->normalize();
    expected->normalize();
    EXPECT_TRUE(actual->equalsExact(expected.get(), 1e-9));
  }
}

TEST(GeometryNoderTest, PreservesZDimensionAndInterpolatesIntersectionZ) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  Geometry::Ptr input =
      reader.read("MULTILINESTRING Z ((0 0 0, 10 10 10), (0 10 20, 10 0 20))");

  Geometry::Ptr noded = GeometryNoder::node(*input);
  ASSERT_NE(noded, nullptr);
  EXPECT_TRUE(noded->hasZ());
  EXPECT_FALSE(noded->hasM());

  const auto* mls = dynamic_cast<const MultiLineString*>(noded.get());
  ASSERT_NE(mls, nullptr);
  ASSERT_EQ(mls->getNumGeometries(), 4u);

  int midpoint_count = 0;
  for (std::size_t i = 0; i < mls->getNumGeometries(); ++i) {
    const LineString* ls = mls->getGeometryN(i);
    const CoordinateSequence* seq = ls->getCoordinatesRO();
    for (std::size_t j = 0; j < seq->size(); ++j) {
      Coordinate pt;
      seq->getAt(j, pt);
      if (pt.equals2D(CoordinateXY(5.0, 5.0))) {
        EXPECT_DOUBLE_EQ(pt.z, 12.5);
        EXPECT_TRUE(std::isnan(seq->getOrdinate(j, CoordinateSequence::M)));
        ++midpoint_count;
      }
    }
  }
  EXPECT_EQ(midpoint_count, 4);
}

TEST(GeometryNoderTest, PreservesMDimensionAndInterpolatesIntersectionM) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  Geometry::Ptr input =
      reader.read("MULTILINESTRING M ((0 0 1, 10 10 11), (0 10 2, 10 0 12))");

  Geometry::Ptr noded = GeometryNoder::node(*input);
  ASSERT_NE(noded, nullptr);
  EXPECT_FALSE(noded->hasZ());
  EXPECT_TRUE(noded->hasM());

  const auto* mls = dynamic_cast<const MultiLineString*>(noded.get());
  ASSERT_NE(mls, nullptr);
  ASSERT_EQ(mls->getNumGeometries(), 4u);

  int midpoint_count = 0;
  for (std::size_t i = 0; i < mls->getNumGeometries(); ++i) {
    const LineString* ls = mls->getGeometryN(i);
    const CoordinateSequence* seq = ls->getCoordinatesRO();
    for (std::size_t j = 0; j < seq->size(); ++j) {
      CoordinateXYM pt;
      seq->getAt(j, pt);
      if (pt.equals2D(CoordinateXY(5.0, 5.0))) {
        EXPECT_TRUE(std::isnan(seq->getOrdinate(j, CoordinateSequence::Z)));
        EXPECT_DOUBLE_EQ(pt.m, 6.5);
        ++midpoint_count;
      }
    }
  }
  EXPECT_EQ(midpoint_count, 4);
}

TEST(GeometryNoderTest, PreservesZMDimensionsAndInterpolatesBoth) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  Geometry::Ptr input = reader.read(
      "MULTILINESTRING ZM ((0 0 0 1, 10 10 10 11), (0 10 20 2, 10 0 20 12))");

  Geometry::Ptr noded = GeometryNoder::node(*input);
  ASSERT_NE(noded, nullptr);
  EXPECT_TRUE(noded->hasZ());
  EXPECT_TRUE(noded->hasM());

  const auto* mls = dynamic_cast<const MultiLineString*>(noded.get());
  ASSERT_NE(mls, nullptr);
  ASSERT_EQ(mls->getNumGeometries(), 4u);

  int midpoint_count = 0;
  for (std::size_t i = 0; i < mls->getNumGeometries(); ++i) {
    const LineString* ls = mls->getGeometryN(i);
    const CoordinateSequence* seq = ls->getCoordinatesRO();
    for (std::size_t j = 0; j < seq->size(); ++j) {
      CoordinateXYZM pt;
      seq->getAt(j, pt);
      if (pt.equals2D(CoordinateXY(5.0, 5.0))) {
        EXPECT_DOUBLE_EQ(pt.z, 12.5);
        EXPECT_DOUBLE_EQ(pt.m, 6.5);
        ++midpoint_count;
      }
    }
  }
  EXPECT_EQ(midpoint_count, 4);
}

TEST(GeometryNoderTest, UsesInputFactoryPrecisionModelAndSrid) {
  PrecisionModel fixed_pm(1.0);
  GeometryFactory::Ptr factory = GeometryFactory::create(&fixed_pm, 4326);
  WKTReader reader(factory.get());

  Geometry::Ptr input =
      reader.read("MULTILINESTRING ((0 0, 10 3), (0 2, 10 0))");
  Geometry::Ptr actual = GeometryNoder::node(*input);
  Geometry::Ptr expected = reader.read(
      "MULTILINESTRING ((0 0, 4 1), (4 1, 10 3), (0 2, 4 1), (4 1, 10 0))");

  ASSERT_NE(actual, nullptr);
  EXPECT_EQ(actual->getFactory(), factory.get());
  EXPECT_EQ(actual->getSRID(), 4326);
  EXPECT_EQ(actual->getPrecisionModel()->getType(), PrecisionModel::FIXED);

  actual->normalize();
  expected->normalize();
  EXPECT_TRUE(actual->equalsExact(expected.get(), 0.0));
}

TEST(GeometryNoderTest, ReusableGeometryNoderInstanceGetNodedTwice) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  Geometry::Ptr input =
      reader.read("MULTILINESTRING ((0 0, 10 10), (0 10, 10 0))");

  GeometryNoder noder(*input);
  Geometry::Ptr first = noder.getNoded();
  Geometry::Ptr second = noder.getNoded();

  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  first->normalize();
  second->normalize();
  EXPECT_TRUE(first->equalsExact(second.get(), 1e-9));
}

TEST(GeometryNoderTest, ThrowsUnsupportedOperationOnCurvedGeometries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr circular = reader.read("CIRCULARSTRING (0 0, 1 1, 2 0)");
  EXPECT_THROW(GeometryNoder::node(*circular), UnsupportedOperationException);

  Geometry::Ptr empty_circular = reader.read("CIRCULARSTRING EMPTY");
  EXPECT_THROW(GeometryNoder::node(*empty_circular),
               UnsupportedOperationException);

  Geometry::Ptr compound =
      reader.read("COMPOUNDCURVE ((0 0, 1 1), CIRCULARSTRING (1 1, 2 0, 3 1))");
  EXPECT_THROW(GeometryNoder::node(*compound), UnsupportedOperationException);

  Geometry::Ptr mixed = reader.read(
      "GEOMETRYCOLLECTION (LINESTRING (0 0, 2 2), "
      "CIRCULARSTRING (0 0, 1 1, 2 0))");
  EXPECT_THROW({ GeometryNoder noder(*mixed); }, UnsupportedOperationException);
}

TEST(GeometryNoderTest, CleansUpAndRethrowsWhenNodingThrows) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto seq1 = std::make_unique<CoordinateSequence>(0u, false, false);
  seq1->add(CoordinateXY(0.0, 0.0));
  seq1->add(CoordinateXY(2.0, 2.0));

  auto seq2 = std::make_unique<CoordinateSequence>(0u, false, false);
  seq2->add(CoordinateXY(1.0, 0.0));
  seq2->add(CoordinateXY(3.0, std::numeric_limits<double>::infinity()));

  std::vector<std::unique_ptr<Geometry>> lines;
  lines.push_back(factory->createLineString(std::move(seq1)));
  lines.push_back(factory->createLineString(std::move(seq2)));
  Geometry::Ptr invalid_mls = factory->createMultiLineString(std::move(lines));

  EXPECT_THROW(GeometryNoder::node(*invalid_mls), IllegalArgumentException);
}

}  // namespace
}  // namespace geos_google
