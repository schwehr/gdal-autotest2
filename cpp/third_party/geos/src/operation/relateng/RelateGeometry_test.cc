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

// Unit tests for third_party/geos/src/operation/relateng/RelateGeometry.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/relateng/RelateGeometry.cpp

#include "third_party/geos/include/geos/operation/relateng/RelateGeometry.h"

#include <array>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/BoundaryNodeRule.h"
#include "third_party/geos/include/geos/algorithm/Orientation.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"
#include "third_party/geos/include/geos/operation/relateng/DimensionLocation.h"
#include "third_party/geos/include/geos/operation/relateng/NodeSection.h"
#include "third_party/geos/include/geos/operation/relateng/RelateSegmentString.h"

namespace geos_google {
namespace {

using geos::algorithm::BoundaryNodeRule;
using geos::algorithm::Orientation;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Dimension;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::Location;
using geos::geom::MultiPolygon;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::noding::SegmentString;
using geos::operation::relateng::DimensionLocation;
using geos::operation::relateng::NodeSection;
using geos::operation::relateng::RelateGeometry;
using geos::operation::relateng::RelateSegmentString;

Geometry::Ptr ReadWkt(const GeometryFactory& factory, const std::string& wkt) {
  WKTReader reader(factory);
  return reader.read(wkt);
}

std::unique_ptr<CoordinateSequence> MakeCoords2D(
    std::initializer_list<CoordinateXY> coords) {
  auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
  seq->reserve(coords.size());
  for (const CoordinateXY& c : coords) {
    seq->add(c);
  }
  return seq;
}

std::unique_ptr<LinearRing> MakeRing(
    const GeometryFactory& factory,
    std::initializer_list<CoordinateXY> coords) {
  return factory.createLinearRing(MakeCoords2D(coords));
}

const RelateSegmentString* AsRelateSegmentString(const SegmentString* ss) {
  return dynamic_cast<const RelateSegmentString*>(ss);
}

std::unique_ptr<NodeSection> FirstNodeSection(const RelateSegmentString* rss) {
  return std::unique_ptr<NodeSection>(
      rss->createNodeSection(0, rss->getCoordinate(0)));
}

TEST(RelateGeometryTest, ConstructorsNameAndStringFormatting) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  Geometry::Ptr geom = ReadWkt(*factory, "LINESTRING (0 0, 3 4)");
  ASSERT_NE(geom, nullptr);

  RelateGeometry rg_default(geom.get());
  RelateGeometry rg_rule(geom.get(), BoundaryNodeRule::getBoundaryEndPoint());
  RelateGeometry rg_prepared(geom.get(), true,
                             BoundaryNodeRule::getBoundaryRuleMod2());

  EXPECT_EQ(rg_default.getGeometry(), geom.get());
  EXPECT_FALSE(rg_default.isPrepared());
  EXPECT_EQ(rg_default.getEnvelope(), geom->getEnvelopeInternal());

  EXPECT_EQ(rg_rule.getGeometry(), geom.get());
  EXPECT_FALSE(rg_rule.isPrepared());

  EXPECT_EQ(rg_prepared.getGeometry(), geom.get());
  EXPECT_TRUE(rg_prepared.isPrepared());

  constexpr bool kGeomA = RelateGeometry::GEOM_A;
  constexpr bool kGeomB = RelateGeometry::GEOM_B;
  EXPECT_TRUE(kGeomA);
  EXPECT_FALSE(kGeomB);
  EXPECT_EQ(RelateGeometry::name(kGeomA), "A");
  EXPECT_EQ(RelateGeometry::name(kGeomB), "B");

  EXPECT_EQ(rg_default.toString(), geom->toString());
  std::ostringstream os;
  os << rg_default;
  EXPECT_EQ(os.str(), geom->toString());
}

TEST(RelateGeometryTest, EmptyGeometries) {
  struct EmptyCase {
    std::string_view name;
    const char* wkt;
    int expected_dim;
    bool expected_polygonal;
    std::size_t expected_effective_points;
  };
  constexpr auto kCases = std::to_array<EmptyCase>({
      {
          .name = "point_empty",
          .wkt = "POINT EMPTY",
          .expected_dim = Dimension::P,
          .expected_polygonal = false,
          .expected_effective_points = 1,
      },
      {
          .name = "linestring_empty",
          .wkt = "LINESTRING EMPTY",
          .expected_dim = Dimension::L,
          .expected_polygonal = false,
          .expected_effective_points = 0,
      },
      {
          .name = "polygon_empty",
          .wkt = "POLYGON EMPTY",
          .expected_dim = Dimension::A,
          .expected_polygonal = true,
          .expected_effective_points = 0,
      },
      {
          .name = "multipoint_empty",
          .wkt = "MULTIPOINT EMPTY",
          .expected_dim = Dimension::P,
          .expected_polygonal = false,
          .expected_effective_points = 0,
      },
      {
          .name = "multilinestring_empty",
          .wkt = "MULTILINESTRING EMPTY",
          .expected_dim = Dimension::L,
          .expected_polygonal = false,
          .expected_effective_points = 0,
      },
      {
          .name = "multipolygon_empty",
          .wkt = "MULTIPOLYGON EMPTY",
          .expected_dim = Dimension::A,
          .expected_polygonal = true,
          .expected_effective_points = 0,
      },
      {
          .name = "gc_empty",
          .wkt = "GEOMETRYCOLLECTION EMPTY",
          .expected_dim = Dimension::False,
          .expected_polygonal = false,
          .expected_effective_points = 0,
      },
      {
          .name = "gc_of_empties",
          .wkt = "GEOMETRYCOLLECTION (POINT EMPTY, LINESTRING EMPTY, "
                 "POLYGON EMPTY)",
          .expected_dim = Dimension::A,
          .expected_polygonal = false,
          .expected_effective_points = 1,
      },
  });

  GeometryFactory::Ptr factory = GeometryFactory::create();
  const CoordinateXY probe(1.0, 2.0);
  for (const EmptyCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    Geometry::Ptr geom = ReadWkt(*factory, tc.wkt);
    ASSERT_NE(geom, nullptr);

    RelateGeometry rg(geom.get());
    EXPECT_TRUE(rg.isEmpty());
    EXPECT_EQ(rg.getDimension(), tc.expected_dim);
    EXPECT_EQ(rg.getDimensionReal(), Dimension::False);
    EXPECT_FALSE(rg.hasDimension(Dimension::P));
    EXPECT_FALSE(rg.hasDimension(Dimension::L));
    EXPECT_FALSE(rg.hasDimension(Dimension::A));
    EXPECT_FALSE(rg.hasDimension(Dimension::False));
    EXPECT_FALSE(rg.hasAreaAndLine());
    EXPECT_FALSE(rg.hasEdges());
    EXPECT_EQ(rg.isPolygonal(), tc.expected_polygonal);
    EXPECT_EQ(rg.locateWithDim(&probe), DimensionLocation::EXTERIOR);
    EXPECT_FALSE(rg.isNodeInArea(&probe, nullptr));

    // When getDimensionReal() <= Dimension::P, PointExtracter components are
    // returned directly (including empty Point instances).
    std::vector<const Point*> effective_pts = rg.getEffectivePoints();
    EXPECT_EQ(effective_pts.size(), tc.expected_effective_points);
    for (const Point* pt : effective_pts) {
      EXPECT_TRUE(pt->isEmpty());
    }

    EXPECT_TRUE(rg.getUniquePoints().empty());
    EXPECT_TRUE(rg.getUniquePoints().empty());
    EXPECT_TRUE(
        rg.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr).empty());
  }
}

TEST(RelateGeometryTest, DimensionsForTypedGeometries) {
  struct DimCase {
    std::string_view name;
    const char* wkt;
    int expected_dim;
    bool has_p;
    bool has_l;
    bool has_a;
    bool has_edges;
    bool self_noding;
    bool polygonal;
  };
  constexpr auto kCases = std::to_array<DimCase>({
      {
          .name = "point",
          .wkt = "POINT (1 2)",
          .expected_dim = Dimension::P,
          .has_p = true,
          .has_l = false,
          .has_a = false,
          .has_edges = false,
          .self_noding = false,
          .polygonal = false,
      },
      {
          .name = "multipoint",
          .wkt = "MULTIPOINT ((1 2), (3 4))",
          .expected_dim = Dimension::P,
          .has_p = true,
          .has_l = false,
          .has_a = false,
          .has_edges = false,
          .self_noding = false,
          .polygonal = false,
      },
      {
          .name = "linestring",
          .wkt = "LINESTRING (0 0, 5 5)",
          .expected_dim = Dimension::L,
          .has_p = false,
          .has_l = true,
          .has_a = false,
          .has_edges = true,
          .self_noding = true,
          .polygonal = false,
      },
      {
          .name = "multilinestring",
          .wkt = "MULTILINESTRING ((0 0, 2 0), (0 1, 2 1))",
          .expected_dim = Dimension::L,
          .has_p = false,
          .has_l = true,
          .has_a = false,
          .has_edges = true,
          .self_noding = true,
          .polygonal = false,
      },
      {
          .name = "polygon",
          .wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
          .expected_dim = Dimension::A,
          .has_p = false,
          .has_l = false,
          .has_a = true,
          .has_edges = true,
          .self_noding = false,
          .polygonal = true,
      },
      {
          .name = "multipolygon",
          .wkt = "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
                 "((5 5, 8 5, 8 8, 5 8, 5 5)))",
          .expected_dim = Dimension::A,
          .has_p = false,
          .has_l = false,
          .has_a = true,
          .has_edges = true,
          .self_noding = false,
          .polygonal = true,
      },
  });

  GeometryFactory::Ptr factory = GeometryFactory::create();
  for (const DimCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    Geometry::Ptr geom = ReadWkt(*factory, tc.wkt);
    ASSERT_NE(geom, nullptr);
    RelateGeometry rg(geom.get());
    EXPECT_FALSE(rg.isEmpty());
    EXPECT_EQ(rg.getDimension(), tc.expected_dim);
    EXPECT_EQ(rg.getDimensionReal(), tc.expected_dim);
    EXPECT_EQ(rg.hasDimension(Dimension::P), tc.has_p);
    EXPECT_EQ(rg.hasDimension(Dimension::L), tc.has_l);
    EXPECT_EQ(rg.hasDimension(Dimension::A), tc.has_a);
    EXPECT_FALSE(rg.hasDimension(Dimension::DONTCARE));
    EXPECT_FALSE(rg.hasAreaAndLine());
    EXPECT_EQ(rg.hasEdges(), tc.has_edges);
    EXPECT_EQ(rg.isSelfNodingRequired(), tc.self_noding);
    EXPECT_EQ(rg.isPolygonal(), tc.polygonal);
  }

  std::unique_ptr<LinearRing> ring =
      MakeRing(*factory, {{0, 0}, {4, 0}, {4, 4}, {0, 4}, {0, 0}});
  RelateGeometry rg_ring(ring.get());
  EXPECT_EQ(rg_ring.getDimension(), Dimension::L);
  EXPECT_EQ(rg_ring.getDimensionReal(), Dimension::L);
  EXPECT_TRUE(rg_ring.hasDimension(Dimension::L));
  EXPECT_TRUE(rg_ring.hasEdges());
  EXPECT_TRUE(rg_ring.isSelfNodingRequired());
  EXPECT_FALSE(rg_ring.isPolygonal());
}

TEST(RelateGeometryTest, GeometryCollectionDimensionsAndSelfNoding) {
  struct GcCase {
    std::string_view name;
    const char* wkt;
    int expected_dim;
    int expected_real_dim;
    bool has_p;
    bool has_l;
    bool has_a;
    bool has_area_and_line;
    bool self_noding;
  };
  constexpr auto kCases = std::to_array<GcCase>({
      {
          .name = "single_point_gc",
          .wkt = "GEOMETRYCOLLECTION (POINT (1 1))",
          .expected_dim = Dimension::P,
          .expected_real_dim = Dimension::P,
          .has_p = true,
          .has_l = false,
          .has_a = false,
          .has_area_and_line = false,
          .self_noding = true,
      },
      {
          .name = "single_line_gc",
          .wkt = "GEOMETRYCOLLECTION (LINESTRING (0 0, 2 2))",
          .expected_dim = Dimension::L,
          .expected_real_dim = Dimension::L,
          .has_p = false,
          .has_l = true,
          .has_a = false,
          .has_area_and_line = false,
          .self_noding = true,
      },
      {
          .name = "single_polygon_gc",
          .wkt = "GEOMETRYCOLLECTION (POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0)))",
          .expected_dim = Dimension::A,
          .expected_real_dim = Dimension::A,
          .has_p = false,
          .has_l = false,
          .has_a = true,
          .has_area_and_line = false,
          .self_noding = false,
      },
      {
          .name = "two_polygons_gc",
          .wkt = "GEOMETRYCOLLECTION ("
                 "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0)), "
                 "POLYGON ((2 2, 6 2, 6 6, 2 6, 2 2)))",
          .expected_dim = Dimension::A,
          .expected_real_dim = Dimension::A,
          .has_p = false,
          .has_l = false,
          .has_a = true,
          .has_area_and_line = false,
          .self_noding = true,
      },
      {
          .name = "polygon_and_point_gc",
          .wkt = "GEOMETRYCOLLECTION ("
                 "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0)), "
                 "POINT (10 10))",
          .expected_dim = Dimension::A,
          .expected_real_dim = Dimension::A,
          .has_p = true,
          .has_l = false,
          .has_a = true,
          .has_area_and_line = false,
          .self_noding = true,
      },
      {
          .name = "polygon_and_line_gc",
          .wkt = "GEOMETRYCOLLECTION ("
                 "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0)), "
                 "LINESTRING (2 2, 8 8))",
          .expected_dim = Dimension::A,
          .expected_real_dim = Dimension::A,
          .has_p = false,
          .has_l = true,
          .has_a = true,
          .has_area_and_line = true,
          .self_noding = true,
      },
      {
          .name = "point_line_polygon_with_empties_gc",
          .wkt = "GEOMETRYCOLLECTION ("
                 "POINT EMPTY, LINESTRING EMPTY, POLYGON EMPTY, "
                 "POINT (1 1), LINESTRING (0 0, 3 0), "
                 "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0)))",
          .expected_dim = Dimension::A,
          .expected_real_dim = Dimension::A,
          .has_p = true,
          .has_l = true,
          .has_a = true,
          .has_area_and_line = true,
          .self_noding = true,
      },
      {
          .name = "empty_polygon_with_point_gc",
          .wkt = "GEOMETRYCOLLECTION (POLYGON EMPTY, LINESTRING EMPTY, "
                 "POINT (5 5))",
          .expected_dim = Dimension::A,
          .expected_real_dim = Dimension::P,
          .has_p = true,
          .has_l = false,
          .has_a = false,
          .has_area_and_line = false,
          .self_noding = true,
      },
      {
          .name = "empty_line_with_point_gc",
          .wkt = "GEOMETRYCOLLECTION (LINESTRING EMPTY, POINT (5 5))",
          .expected_dim = Dimension::L,
          .expected_real_dim = Dimension::P,
          .has_p = true,
          .has_l = false,
          .has_a = false,
          .has_area_and_line = false,
          .self_noding = true,
      },
      {
          .name = "single_multipolygon_gc",
          .wkt = "GEOMETRYCOLLECTION (MULTIPOLYGON ("
                 "((0 0, 4 0, 4 4, 0 4, 0 0)), "
                 "((10 10, 14 10, 14 14, 10 14, 10 10))))",
          .expected_dim = Dimension::A,
          .expected_real_dim = Dimension::A,
          .has_p = false,
          .has_l = false,
          .has_a = true,
          .has_area_and_line = false,
          .self_noding = false,
      },
  });

  GeometryFactory::Ptr factory = GeometryFactory::create();
  for (const GcCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    Geometry::Ptr geom = ReadWkt(*factory, tc.wkt);
    ASSERT_NE(geom, nullptr);
    RelateGeometry rg(geom.get());
    EXPECT_FALSE(rg.isEmpty());
    EXPECT_EQ(rg.getDimension(), tc.expected_dim);
    EXPECT_EQ(rg.getDimensionReal(), tc.expected_real_dim);
    EXPECT_EQ(rg.hasDimension(Dimension::P), tc.has_p);
    EXPECT_EQ(rg.hasDimension(Dimension::L), tc.has_l);
    EXPECT_EQ(rg.hasDimension(Dimension::A), tc.has_a);
    EXPECT_EQ(rg.hasAreaAndLine(), tc.has_area_and_line);
    EXPECT_EQ(rg.isSelfNodingRequired(), tc.self_noding);
    EXPECT_FALSE(rg.isPolygonal());
  }

  // A GeometryCollection with a single empty polygon reports hasAreas == false
  // and therefore requires self-noding check fallback.
  {
    Geometry::Ptr geom =
        ReadWkt(*factory, "GEOMETRYCOLLECTION (POLYGON EMPTY)");
    RelateGeometry rg(geom.get());
    EXPECT_TRUE(rg.isEmpty());
    EXPECT_EQ(rg.getDimension(), Dimension::A);
    EXPECT_EQ(rg.getDimensionReal(), Dimension::False);
    EXPECT_FALSE(rg.hasDimension(Dimension::A));
    EXPECT_TRUE(rg.isSelfNodingRequired());
  }

  std::vector<std::unique_ptr<Geometry>> geoms;
  geoms.push_back(MakeRing(*factory, {{0, 0}, {3, 0}, {3, 3}, {0, 3}, {0, 0}}));
  std::unique_ptr<GeometryCollection> gc_ring =
      factory->createGeometryCollection(std::move(geoms));
  RelateGeometry rg_gc_ring(gc_ring.get());
  EXPECT_EQ(rg_gc_ring.getDimensionReal(), Dimension::L);
  EXPECT_TRUE(rg_gc_ring.hasDimension(Dimension::L));
  EXPECT_TRUE(rg_gc_ring.hasEdges());
}

TEST(RelateGeometryTest, ZeroLengthLineStringsReportPointRealDimension) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  struct ZeroLenCase {
    std::string_view name;
    const char* wkt;
    int expected_real_dim;
  };
  constexpr auto kCases = std::to_array<ZeroLenCase>({
      {
          .name = "zero_len_2pt",
          .wkt = "LINESTRING (1 2, 1 2)",
          .expected_real_dim = Dimension::P,
      },
      {
          .name = "zero_len_3d_vertical",
          .wkt = "LINESTRING Z (1 2 0, 1 2 10)",
          .expected_real_dim = Dimension::P,
      },
      {
          .name = "zero_len_prefix_nonzero_tail",
          .wkt = "LINESTRING (0 0, 0 0, 1 0)",
          .expected_real_dim = Dimension::L,
      },
      {
          .name = "multilinestring_all_zero_len",
          .wkt = "MULTILINESTRING ((0 0, 0 0), (2 3, 2 3, 2 3))",
          .expected_real_dim = Dimension::P,
      },
      {
          .name = "multilinestring_mixed_zero_and_nonzero",
          .wkt = "MULTILINESTRING ((0 0, 0 0), (1 1, 4 1))",
          .expected_real_dim = Dimension::L,
      },
      {
          .name = "gc_point_and_zero_len_line",
          .wkt = "GEOMETRYCOLLECTION (POINT (9 9), LINESTRING (1 1, 1 1))",
          .expected_real_dim = Dimension::P,
      },
      {
          .name = "gc_point_and_nonzero_line",
          .wkt = "GEOMETRYCOLLECTION (POINT (9 9), LINESTRING (1 1, 1 1), "
                 "LINESTRING (0 0, 3 4))",
          .expected_real_dim = Dimension::L,
      },
  });

  for (const ZeroLenCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    Geometry::Ptr geom = ReadWkt(*factory, tc.wkt);
    ASSERT_NE(geom, nullptr);
    RelateGeometry rg(geom.get());
    EXPECT_EQ(rg.getDimension(), Dimension::L);
    EXPECT_EQ(rg.getDimensionReal(), tc.expected_real_dim);
  }

  std::vector<std::unique_ptr<Geometry>> geoms;
  geoms.push_back(ReadWkt(*factory, "POINT (1 1)"));
  geoms.push_back(MakeRing(*factory, {{2, 2}, {2, 2}, {2, 2}}));
  std::unique_ptr<GeometryCollection> gc_zero_ring =
      factory->createGeometryCollection(std::move(geoms));
  RelateGeometry rg_zero_ring(gc_zero_ring.get());
  EXPECT_EQ(rg_zero_ring.getDimension(), Dimension::L);
  EXPECT_EQ(rg_zero_ring.getDimensionReal(), Dimension::P);
}

TEST(RelateGeometryTest, BoundaryNodeRulesAndLocateLineEndWithDim) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Open LineString always has boundary at endpoints.
  {
    Geometry::Ptr open_line = ReadWkt(*factory, "LINESTRING (0 0, 5 0, 10 0)");
    RelateGeometry rg(open_line.get());
    EXPECT_TRUE(rg.hasBoundary());
    const CoordinateXY p_start(0, 0);
    const CoordinateXY p_mid(5, 0);
    const CoordinateXY p_end(10, 0);
    EXPECT_EQ(rg.locateLineEndWithDim(&p_start),
              DimensionLocation::LINE_BOUNDARY);
    EXPECT_EQ(rg.locateLineEndWithDim(&p_mid),
              DimensionLocation::LINE_INTERIOR);
    EXPECT_EQ(rg.locateLineEndWithDim(&p_end),
              DimensionLocation::LINE_BOUNDARY);
  }

  // Closed LineString depends on BoundaryNodeRule.
  {
    Geometry::Ptr closed_line =
        ReadWkt(*factory, "LINESTRING (0 0, 4 0, 4 4, 0 0)");
    const CoordinateXY p_closure(0, 0);

    RelateGeometry rg_mod2(closed_line.get(),
                           BoundaryNodeRule::getBoundaryRuleMod2());
    EXPECT_FALSE(rg_mod2.hasBoundary());
    EXPECT_EQ(rg_mod2.locateLineEndWithDim(&p_closure),
              DimensionLocation::LINE_INTERIOR);

    RelateGeometry rg_sfs(closed_line.get(),
                          BoundaryNodeRule::getBoundaryOGCSFS());
    EXPECT_FALSE(rg_sfs.hasBoundary());

    RelateGeometry rg_ep(closed_line.get(),
                         BoundaryNodeRule::getBoundaryEndPoint());
    EXPECT_TRUE(rg_ep.hasBoundary());
    EXPECT_EQ(rg_ep.locateLineEndWithDim(&p_closure),
              DimensionLocation::LINE_BOUNDARY);
  }

  // T-junction MultiLineString: valence-3 at (5,0) and valence-1 at (0,0).
  {
    Geometry::Ptr mls = ReadWkt(
        *factory, "MULTILINESTRING ((0 0, 5 0), (5 0, 10 0), (5 0, 5 5))");
    const CoordinateXY p_deg1(0, 0);
    const CoordinateXY p_deg3(5, 0);

    RelateGeometry rg_mod2(mls.get(), BoundaryNodeRule::getBoundaryRuleMod2());
    EXPECT_TRUE(rg_mod2.hasBoundary());
    EXPECT_EQ(rg_mod2.locateLineEndWithDim(&p_deg1),
              DimensionLocation::LINE_BOUNDARY);
    EXPECT_EQ(rg_mod2.locateLineEndWithDim(&p_deg3),
              DimensionLocation::LINE_BOUNDARY);

    RelateGeometry rg_mono(mls.get(),
                           BoundaryNodeRule::getBoundaryMonovalentEndPoint());
    EXPECT_TRUE(rg_mono.hasBoundary());
    EXPECT_EQ(rg_mono.locateLineEndWithDim(&p_deg1),
              DimensionLocation::LINE_BOUNDARY);
    EXPECT_EQ(rg_mono.locateLineEndWithDim(&p_deg3),
              DimensionLocation::LINE_INTERIOR);

    RelateGeometry rg_multi(mls.get(),
                            BoundaryNodeRule::getBoundaryMultivalentEndPoint());
    EXPECT_TRUE(rg_multi.hasBoundary());
    EXPECT_EQ(rg_multi.locateLineEndWithDim(&p_deg1),
              DimensionLocation::LINE_INTERIOR);
    EXPECT_EQ(rg_multi.locateLineEndWithDim(&p_deg3),
              DimensionLocation::LINE_BOUNDARY);
  }

  // GeometryCollection containing a line with ends inside, on boundary, and
  // outside a polygon.
  {
    Geometry::Ptr gc = ReadWkt(*factory,
                               "GEOMETRYCOLLECTION ("
                               "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)), "
                               "LINESTRING (5 5, 10 5), "
                               "LINESTRING (10 5, 15 5))");
    RelateGeometry rg(gc.get());
    EXPECT_TRUE(rg.hasBoundary());
    const CoordinateXY inside_poly(5, 5);
    const CoordinateXY on_poly_bdy(10, 5);
    const CoordinateXY outside_poly(15, 5);
    EXPECT_EQ(rg.locateLineEndWithDim(&inside_poly),
              DimensionLocation::AREA_INTERIOR);
    EXPECT_EQ(rg.locateLineEndWithDim(&on_poly_bdy),
              DimensionLocation::AREA_BOUNDARY);
    EXPECT_EQ(rg.locateLineEndWithDim(&outside_poly),
              DimensionLocation::LINE_BOUNDARY);
  }
}

TEST(RelateGeometryTest, PointLocationNodesAndAreaVertices) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Point geometry.
  {
    Geometry::Ptr pt_geom = ReadWkt(*factory, "POINT (3 4)");
    RelateGeometry rg(pt_geom.get());
    const CoordinateXY on_pt(3, 4);
    const CoordinateXY off_pt(0, 0);
    EXPECT_EQ(rg.locateWithDim(&on_pt), DimensionLocation::POINT_INTERIOR);
    EXPECT_EQ(rg.locateWithDim(&off_pt), DimensionLocation::EXTERIOR);
    EXPECT_EQ(rg.locateNode(&on_pt, nullptr), Location::INTERIOR);
    EXPECT_FALSE(rg.isNodeInArea(&on_pt, nullptr));
  }

  // Polygon with a hole in both unprepared and prepared modes.
  for (bool is_prepared : {false, true}) {
    SCOPED_TRACE(is_prepared ? "prepared" : "unprepared");
    Geometry::Ptr poly = ReadWkt(*factory,
                                 "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
                                 "(3 3, 3 7, 7 7, 7 3, 3 3))");
    RelateGeometry rg(poly.get(), is_prepared,
                      BoundaryNodeRule::getBoundaryRuleMod2());

    const CoordinateXY in_area(1, 1);
    const CoordinateXY shell_edge(5, 0);
    const CoordinateXY shell_vertex(0, 0);
    const CoordinateXY hole_vertex(3, 3);
    const CoordinateXY in_hole(5, 5);
    const CoordinateXY outside(20, 20);

    // Call twice to verify lazy RelatePointLocator initialization and reuse.
    EXPECT_EQ(rg.locateWithDim(&in_area), DimensionLocation::AREA_INTERIOR);
    EXPECT_EQ(rg.locateWithDim(&in_area), DimensionLocation::AREA_INTERIOR);
    EXPECT_EQ(rg.locateWithDim(&shell_edge), DimensionLocation::AREA_BOUNDARY);
    EXPECT_EQ(rg.locateWithDim(&shell_vertex),
              DimensionLocation::AREA_BOUNDARY);
    EXPECT_EQ(rg.locateWithDim(&hole_vertex), DimensionLocation::AREA_BOUNDARY);
    EXPECT_EQ(rg.locateWithDim(&in_hole), DimensionLocation::EXTERIOR);
    EXPECT_EQ(rg.locateWithDim(&outside), DimensionLocation::EXTERIOR);

    EXPECT_EQ(rg.locateAreaVertex(&shell_vertex), Location::BOUNDARY);
    EXPECT_EQ(rg.locateAreaVertex(&hole_vertex), Location::BOUNDARY);
    EXPECT_EQ(rg.locateNode(&shell_vertex, poly.get()), Location::BOUNDARY);
    EXPECT_FALSE(rg.isNodeInArea(&shell_vertex, poly.get()));
  }

  // MultiPolygon vertex and interior point.
  {
    Geometry::Ptr mp = ReadWkt(*factory,
                               "MULTIPOLYGON ("
                               "((0 0, 4 0, 4 4, 0 4, 0 0)), "
                               "((10 10, 14 10, 14 14, 10 14, 10 10)))");
    RelateGeometry rg(mp.get(), true, BoundaryNodeRule::getBoundaryRuleMod2());
    const CoordinateXY in_first(2, 2);
    const CoordinateXY on_second_vertex(10, 10);
    EXPECT_EQ(rg.locateWithDim(&in_first), DimensionLocation::AREA_INTERIOR);
    EXPECT_EQ(rg.locateAreaVertex(&on_second_vertex), Location::BOUNDARY);
  }

  // Overlapping polygons in a GeometryCollection: inner polygon vertex is
  // in the effective interior of the collection.
  {
    Geometry::Ptr outer =
        ReadWkt(*factory, "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
    Geometry::Ptr inner =
        ReadWkt(*factory, "POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))");
    const Geometry* inner_ptr = inner.get();
    const Geometry* outer_ptr = outer.get();

    std::vector<std::unique_ptr<Geometry>> geoms;
    geoms.push_back(std::move(outer));
    geoms.push_back(std::move(inner));
    std::unique_ptr<GeometryCollection> gc =
        factory->createGeometryCollection(std::move(geoms));

    RelateGeometry rg(gc.get());
    const CoordinateXY inner_vertex(2, 2);
    const CoordinateXY outer_vertex(0, 0);
    EXPECT_EQ(rg.locateAreaVertex(&inner_vertex), Location::INTERIOR);
    EXPECT_EQ(rg.locateNode(&inner_vertex, inner_ptr), Location::INTERIOR);
    EXPECT_TRUE(rg.isNodeInArea(&inner_vertex, inner_ptr));

    EXPECT_EQ(rg.locateAreaVertex(&outer_vertex), Location::BOUNDARY);
    EXPECT_EQ(rg.locateNode(&outer_vertex, outer_ptr), Location::BOUNDARY);
    EXPECT_FALSE(rg.isNodeInArea(&outer_vertex, outer_ptr));
  }

  // Adjacent polygons sharing an edge in a GeometryCollection trigger
  // AdjacentEdgeLocator on the shared edge midpoint and corner.
  {
    Geometry::Ptr gc = ReadWkt(*factory,
                               "GEOMETRYCOLLECTION ("
                               "POLYGON ((0 0, 5 0, 5 10, 0 10, 0 0)), "
                               "POLYGON ((5 0, 10 0, 10 10, 5 10, 5 0)))");
    RelateGeometry rg(gc.get());
    const CoordinateXY shared_edge_mid(5, 5);
    const CoordinateXY shared_corner(5, 0);
    EXPECT_EQ(rg.locateAreaVertex(&shared_edge_mid), Location::INTERIOR);
    EXPECT_TRUE(rg.isNodeInArea(&shared_edge_mid, nullptr));
    EXPECT_EQ(rg.locateAreaVertex(&shared_corner), Location::BOUNDARY);
    EXPECT_FALSE(rg.isNodeInArea(&shared_corner, nullptr));
  }

  // Mixed collection checks A > L > P precedence.
  {
    Geometry::Ptr gc = ReadWkt(*factory,
                               "GEOMETRYCOLLECTION ("
                               "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)), "
                               "LINESTRING (5 5, 15 5), "
                               "POINT (2 2), "
                               "POINT (12 5), "
                               "POINT (20 20))");
    RelateGeometry rg(gc.get());
    const CoordinateXY in_poly_with_pt(2, 2);
    const CoordinateXY line_end_in_poly(5, 5);
    const CoordinateXY on_poly_bdy_and_line(10, 5);
    const CoordinateXY on_line_with_pt(12, 5);
    const CoordinateXY on_line_end(15, 5);
    const CoordinateXY isolated_pt(20, 20);
    const CoordinateXY disjoint(30, 30);

    EXPECT_EQ(rg.locateWithDim(&in_poly_with_pt),
              DimensionLocation::AREA_INTERIOR);
    EXPECT_EQ(rg.locateWithDim(&line_end_in_poly),
              DimensionLocation::AREA_INTERIOR);
    EXPECT_TRUE(rg.isNodeInArea(&line_end_in_poly, nullptr));
    EXPECT_EQ(rg.locateWithDim(&on_poly_bdy_and_line),
              DimensionLocation::AREA_BOUNDARY);
    EXPECT_EQ(rg.locateWithDim(&on_line_with_pt),
              DimensionLocation::LINE_INTERIOR);
    EXPECT_EQ(rg.locateWithDim(&on_line_end), DimensionLocation::LINE_BOUNDARY);
    EXPECT_EQ(rg.locateWithDim(&isolated_pt),
              DimensionLocation::POINT_INTERIOR);
    EXPECT_EQ(rg.locateWithDim(&disjoint), DimensionLocation::EXTERIOR);
  }
}

TEST(RelateGeometryTest, UniquePointsAndEffectivePoints) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // MultiPoint with duplicate coordinates deduplicates in getUniquePoints(),
  // while getEffectivePoints() returns all Point components when real
  // dimension <= P.
  {
    Geometry::Ptr mp =
        ReadWkt(*factory, "MULTIPOINT ((1 1), (2 2), (1 1), (3 3))");
    RelateGeometry rg(mp.get());
    const auto& unique_first = rg.getUniquePoints();
    EXPECT_EQ(unique_first.size(), 3u);
    const auto& unique_cached = rg.getUniquePoints();
    EXPECT_EQ(&unique_cached, &unique_first);
    EXPECT_EQ(rg.getEffectivePoints().size(), 4u);
  }

  // Zero-length LineString has real dimension P, so getEffectivePoints returns
  // no Point objects (PointExtracter yields none), while getUniquePoints
  // captures its representative coordinate.
  {
    Geometry::Ptr zero_line = ReadWkt(*factory, "LINESTRING (4 4, 4 4)");
    RelateGeometry rg(zero_line.get());
    EXPECT_EQ(rg.getDimensionReal(), Dimension::P);
    EXPECT_TRUE(rg.getEffectivePoints().empty());
    const CoordinateXY c4(4, 4);
    const auto& unique_pts = rg.getUniquePoints();
    EXPECT_EQ(unique_pts.size(), 1u);
    EXPECT_EQ(unique_pts.count(&c4), 1u);
  }

  // Collection of Point, empty Point, and zero-length LineStrings has real
  // dimension P: uniquePoints deduplicates representative coordinates across
  // non-empty components, and getEffectivePoints returns both Point components.
  {
    Geometry::Ptr gc = ReadWkt(*factory,
                               "GEOMETRYCOLLECTION (POINT (1 1), POINT EMPTY, "
                               "LINESTRING (1 1, 1 1), LINESTRING (2 2, 2 2))");
    RelateGeometry rg(gc.get());
    EXPECT_EQ(rg.getDimensionReal(), Dimension::P);
    const CoordinateXY c1(1, 1);
    const CoordinateXY c2(2, 2);
    const auto& unique_pts = rg.getUniquePoints();
    EXPECT_EQ(unique_pts.size(), 2u);
    EXPECT_EQ(unique_pts.count(&c1), 1u);
    EXPECT_EQ(unique_pts.count(&c2), 1u);
    EXPECT_EQ(rg.getEffectivePoints().size(), 2u);
  }

  // Mixed collection with Points in/on Polygon, on LineString interior and
  // boundary, empty Point, and uncovered Point: only the uncovered Point is
  // effective.
  {
    Geometry::Ptr gc = ReadWkt(*factory,
                               "GEOMETRYCOLLECTION ("
                               "POINT EMPTY, "
                               "POINT (2 2), "
                               "POINT (0 5), "
                               "POINT (12 5), "
                               "POINT (15 5), "
                               "POINT (20 20), "
                               "LINESTRING (5 5, 15 5), "
                               "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)))");
    RelateGeometry rg(gc.get());
    std::vector<const Point*> effective = rg.getEffectivePoints();
    ASSERT_EQ(effective.size(), 1u);
    EXPECT_DOUBLE_EQ(effective[0]->getX(), 20.0);
    EXPECT_DOUBLE_EQ(effective[0]->getY(), 20.0);
  }

  // Lineal collection filters out Points on line interior and endpoints.
  {
    Geometry::Ptr gc = ReadWkt(*factory,
                               "GEOMETRYCOLLECTION ("
                               "LINESTRING (0 0, 10 0), "
                               "POINT (5 0), "
                               "POINT (0 0), "
                               "POINT (5 5))");
    RelateGeometry rg(gc.get());
    EXPECT_EQ(rg.getDimensionReal(), Dimension::L);
    std::vector<const Point*> effective = rg.getEffectivePoints();
    ASSERT_EQ(effective.size(), 1u);
    EXPECT_DOUBLE_EQ(effective[0]->getX(), 5.0);
    EXPECT_DOUBLE_EQ(effective[0]->getY(), 5.0);
  }
}

TEST(RelateGeometryTest, ExtractSegmentStringsCachingModes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  Geometry::Ptr poly =
      ReadWkt(*factory, "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))");
  const Envelope filter_env(0.0, 5.0, 0.0, 5.0);

  // Prepared GEOM_A with null envelope caches permanently in
  // segStringPermStore.
  {
    RelateGeometry rg(poly.get(), true,
                      BoundaryNodeRule::getBoundaryRuleMod2());
    std::vector<const SegmentString*> first =
        rg.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    std::vector<const SegmentString*> second =
        rg.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    ASSERT_EQ(first.size(), 1u);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(first[0], second[0]);
    const RelateSegmentString* rss = AsRelateSegmentString(first[0]);
    ASSERT_NE(rss, nullptr);
    EXPECT_TRUE(rss->isA());
    EXPECT_EQ(rss->getGeometry(), &rg);
    EXPECT_EQ(rss->getPolygonal(), poly.get());

    // Passing a non-null envelope uses temp store without overwriting perm
    // store.
    std::vector<const SegmentString*> env_filtered =
        rg.extractSegmentStrings(RelateGeometry::GEOM_A, &filter_env);
    ASSERT_EQ(env_filtered.size(), 1u);
    EXPECT_NE(env_filtered[0], first[0]);

    std::vector<const SegmentString*> third =
        rg.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    ASSERT_EQ(third.size(), 1u);
    EXPECT_EQ(third[0], first[0]);

    // Prepared RelateGeometry called with GEOM_B uses temp store as well.
    std::vector<const SegmentString*> as_b =
        rg.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);
    ASSERT_EQ(as_b.size(), 1u);
    EXPECT_NE(as_b[0], first[0]);
    EXPECT_FALSE(AsRelateSegmentString(as_b[0])->isA());

    // Perm store remains intact for GEOM_A.
    std::vector<const SegmentString*> fourth =
        rg.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    ASSERT_EQ(fourth.size(), 1u);
    EXPECT_EQ(fourth[0], first[0]);
  }

  // Non-prepared GEOM_A and GEOM_B always extract into temp store.
  {
    RelateGeometry rg(poly.get(), false,
                      BoundaryNodeRule::getBoundaryRuleMod2());
    std::vector<const SegmentString*> a1 =
        rg.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    ASSERT_EQ(a1.size(), 1u);
    EXPECT_TRUE(AsRelateSegmentString(a1[0])->isA());

    std::vector<const SegmentString*> b1 =
        rg.extractSegmentStrings(RelateGeometry::GEOM_B, nullptr);
    ASSERT_EQ(b1.size(), 1u);
    EXPECT_FALSE(AsRelateSegmentString(b1[0])->isA());
  }
}

TEST(RelateGeometryTest, ExtractSegmentStringsTypesHierarchyAndEnvelopes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Point and MultiPoint produce zero segment strings; MultiLineString produces
  // one segment string per line with null parentPolygonal.
  {
    Geometry::Ptr pt = ReadWkt(*factory, "POINT (1 2)");
    RelateGeometry rg_pt(pt.get());
    EXPECT_TRUE(
        rg_pt.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr).empty());

    Geometry::Ptr mp = ReadWkt(*factory, "MULTIPOINT ((1 2), (3 4))");
    RelateGeometry rg_mp(mp.get());
    EXPECT_TRUE(
        rg_mp.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr).empty());

    Geometry::Ptr mls =
        ReadWkt(*factory, "MULTILINESTRING ((0 0, 3 0), (0 5, 3 5))");
    RelateGeometry rg_mls(mls.get());
    std::vector<const SegmentString*> strings =
        rg_mls.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    ASSERT_EQ(strings.size(), 2u);
    const RelateSegmentString* rss0 = AsRelateSegmentString(strings[0]);
    const RelateSegmentString* rss1 = AsRelateSegmentString(strings[1]);
    ASSERT_NE(rss0, nullptr);
    ASSERT_NE(rss1, nullptr);
    EXPECT_EQ(rss0->getPolygonal(), nullptr);
    EXPECT_EQ(rss1->getPolygonal(), nullptr);
    EXPECT_NE(FirstNodeSection(rss0)->id(), FirstNodeSection(rss1)->id());
  }

  // MultiPolygon sets parentPolygonal to the MultiPolygon, and assigns distinct
  // element IDs and ring IDs.
  {
    Geometry::Ptr mp_geom =
        ReadWkt(*factory,
                "MULTIPOLYGON ("
                "((0 0, 0 20, 20 20, 20 0, 0 0), (2 2, 6 2, 6 6, 2 6, 2 2)), "
                "((30 30, 30 40, 40 40, 40 30, 30 30)))");
    const auto* mp = dynamic_cast<const MultiPolygon*>(mp_geom.get());
    ASSERT_NE(mp, nullptr);
    RelateGeometry rg(mp);
    std::vector<const SegmentString*> strings =
        rg.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    ASSERT_EQ(strings.size(), 3u);

    for (const SegmentString* ss : strings) {
      const RelateSegmentString* rss = AsRelateSegmentString(ss);
      ASSERT_NE(rss, nullptr);
      EXPECT_EQ(rss->getPolygonal(), mp);
      EXPECT_EQ(rss->getGeometry(), &rg);
    }

    auto ns0 = FirstNodeSection(AsRelateSegmentString(strings[0]));
    auto ns1 = FirstNodeSection(AsRelateSegmentString(strings[1]));
    auto ns2 = FirstNodeSection(AsRelateSegmentString(strings[2]));
    EXPECT_EQ(ns0->dimension(), Dimension::A);
    EXPECT_EQ(ns0->ringId(), 0);
    EXPECT_EQ(ns1->ringId(), 1);
    EXPECT_EQ(ns0->id(), ns1->id());
    EXPECT_EQ(ns2->ringId(), 0);
    EXPECT_NE(ns2->id(), ns0->id());
  }

  // Polygon with an empty hole and two non-empty holes: empty hole is skipped,
  // and envelope filter skips disjoint holes.
  {
    std::unique_ptr<LinearRing> shell =
        MakeRing(*factory, {{0, 0}, {0, 100}, {100, 100}, {100, 0}, {0, 0}});
    std::vector<std::unique_ptr<LinearRing>> holes;
    holes.push_back(factory->createLinearRing());  // Empty hole.
    holes.push_back(
        MakeRing(*factory, {{10, 10}, {20, 10}, {20, 20}, {10, 20}, {10, 10}}));
    holes.push_back(
        MakeRing(*factory, {{70, 70}, {80, 70}, {80, 80}, {70, 80}, {70, 70}}));
    std::unique_ptr<Polygon> poly =
        factory->createPolygon(std::move(shell), std::move(holes));

    RelateGeometry rg(poly.get());
    std::vector<const SegmentString*> all =
        rg.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    ASSERT_EQ(all.size(), 3u);  // Shell + 2 non-empty holes.
    EXPECT_EQ(FirstNodeSection(AsRelateSegmentString(all[0]))->ringId(), 0);
    EXPECT_EQ(FirstNodeSection(AsRelateSegmentString(all[1]))->ringId(), 2);
    EXPECT_EQ(FirstNodeSection(AsRelateSegmentString(all[2]))->ringId(), 3);

    const Envelope env_first_hole(5.0, 25.0, 5.0, 25.0);
    std::vector<const SegmentString*> filtered =
        rg.extractSegmentStrings(RelateGeometry::GEOM_B, &env_first_hole);
    ASSERT_EQ(filtered.size(), 2u);  // Shell + first non-empty hole only.
    EXPECT_EQ(FirstNodeSection(AsRelateSegmentString(filtered[0]))->ringId(),
              0);
    EXPECT_EQ(FirstNodeSection(AsRelateSegmentString(filtered[1]))->ringId(),
              2);

    const Envelope env_disjoint(200.0, 210.0, 200.0, 210.0);
    EXPECT_TRUE(rg.extractSegmentStrings(RelateGeometry::GEOM_A, &env_disjoint)
                    .empty());
  }

  // Nested collections containing empty elements, LineString, LinearRing with
  // repeated points, Polygon (whose parentPolygonal is the Polygon itself),
  // and a MultiPolygon containing an empty Polygon and a non-empty Polygon
  // (whose parentPolygonal is the nested MultiPolygon).
  {
    std::vector<std::unique_ptr<Geometry>> mp_polys;
    mp_polys.push_back(factory->createPolygon());
    mp_polys.push_back(
        ReadWkt(*factory, "POLYGON ((30 30, 30 35, 35 35, 35 30, 30 30))"));
    std::unique_ptr<MultiPolygon> nested_mp =
        factory->createMultiPolygon(std::move(mp_polys));
    const MultiPolygon* nested_mp_ptr = nested_mp.get();

    std::vector<std::unique_ptr<Geometry>> inner_geoms;
    inner_geoms.push_back(ReadWkt(*factory, "LINESTRING EMPTY"));
    inner_geoms.push_back(
        MakeRing(*factory, {{1, 1}, {3, 1}, {3, 1}, {3, 3}, {1, 3}, {1, 1}}));
    Geometry::Ptr inner_poly =
        ReadWkt(*factory, "POLYGON ((10 10, 10 15, 15 15, 15 10, 10 10))");
    const Geometry* inner_poly_ptr = inner_poly.get();
    inner_geoms.push_back(std::move(inner_poly));
    inner_geoms.push_back(std::move(nested_mp));

    std::vector<std::unique_ptr<Geometry>> outer_geoms;
    outer_geoms.push_back(ReadWkt(*factory, "POINT (50 50)"));
    outer_geoms.push_back(ReadWkt(*factory, "LINESTRING (0 0, 2 2)"));
    outer_geoms.push_back(
        factory->createGeometryCollection(std::move(inner_geoms)));
    std::unique_ptr<GeometryCollection> gc =
        factory->createGeometryCollection(std::move(outer_geoms));

    RelateGeometry rg(gc.get());
    std::vector<const SegmentString*> strings =
        rg.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    ASSERT_EQ(strings.size(), 4u);

    const RelateSegmentString* rss_line = AsRelateSegmentString(strings[0]);
    const RelateSegmentString* rss_ring = AsRelateSegmentString(strings[1]);
    const RelateSegmentString* rss_poly = AsRelateSegmentString(strings[2]);
    const RelateSegmentString* rss_mp = AsRelateSegmentString(strings[3]);
    ASSERT_NE(rss_line, nullptr);
    ASSERT_NE(rss_ring, nullptr);
    ASSERT_NE(rss_poly, nullptr);
    ASSERT_NE(rss_mp, nullptr);

    EXPECT_EQ(rss_line->getPolygonal(), nullptr);
    EXPECT_EQ(rss_ring->getPolygonal(), nullptr);
    EXPECT_EQ(rss_ring->getCoordinates()->size(), 5u);
    EXPECT_FALSE(rss_ring->getCoordinates()->hasRepeatedPoints());
    EXPECT_EQ(rss_poly->getPolygonal(), inner_poly_ptr);
    EXPECT_EQ(rss_mp->getPolygonal(), nested_mp_ptr);
    EXPECT_EQ(FirstNodeSection(rss_line)->dimension(), Dimension::L);
    EXPECT_EQ(FirstNodeSection(rss_ring)->dimension(), Dimension::L);
    EXPECT_EQ(FirstNodeSection(rss_poly)->dimension(), Dimension::A);
    EXPECT_EQ(FirstNodeSection(rss_mp)->dimension(), Dimension::A);
  }
}

TEST(RelateGeometryTest, ExtractSegmentStringsOrientationAndRepeatedPoints) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // LineString without repeated points reuses the underlying
  // CoordinateSequence.
  {
    Geometry::Ptr line_geom = ReadWkt(*factory, "LINESTRING (0 0, 5 5, 10 0)");
    const auto* line = dynamic_cast<const LineString*>(line_geom.get());
    ASSERT_NE(line, nullptr);

    RelateGeometry rg(line_geom.get());
    std::vector<const SegmentString*> strings =
        rg.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    ASSERT_EQ(strings.size(), 1u);
    EXPECT_EQ(strings[0]->getCoordinates(), line->getCoordinatesRO());
  }

  // LineString with consecutive repeated points removes duplicates.
  {
    Geometry::Ptr line_geom =
        ReadWkt(*factory, "LINESTRING (0 0, 0 0, 5 5, 5 5, 10 0)");
    const auto* line = dynamic_cast<const LineString*>(line_geom.get());
    ASSERT_NE(line, nullptr);

    RelateGeometry rg(line_geom.get());
    std::vector<const SegmentString*> strings =
        rg.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    ASSERT_EQ(strings.size(), 1u);
    const CoordinateSequence* cs = strings[0]->getCoordinates();
    EXPECT_NE(cs, line->getCoordinatesRO());
    EXPECT_EQ(cs->size(), 3u);
    EXPECT_FALSE(cs->hasRepeatedPoints());
  }

  // Polygon shell & hole permutations covering all 4 (isFlipped, hasRepeated)
  // combinations in orientAndRemoveRepeated().
  struct RingOrientCase {
    std::string_view name;
    bool shell_cw;
    bool shell_repeated;
    bool hole_ccw;
    bool hole_repeated;
    std::size_t expected_shell_pts;
    std::size_t expected_hole_pts;
    bool expect_shell_reused;
    bool expect_hole_reused;
  };
  constexpr auto kCases = std::to_array<RingOrientCase>({
      {
          .name = "cw_shell_clean_ccw_hole_clean",
          .shell_cw = true,
          .shell_repeated = false,
          .hole_ccw = true,
          .hole_repeated = false,
          .expected_shell_pts = 5,
          .expected_hole_pts = 5,
          .expect_shell_reused = true,
          .expect_hole_reused = true,
      },
      {
          .name = "ccw_shell_clean_cw_hole_clean",
          .shell_cw = false,
          .shell_repeated = false,
          .hole_ccw = false,
          .hole_repeated = false,
          .expected_shell_pts = 5,
          .expected_hole_pts = 5,
          .expect_shell_reused = false,
          .expect_hole_reused = false,
      },
      {
          .name = "cw_shell_repeated_ccw_hole_repeated",
          .shell_cw = true,
          .shell_repeated = true,
          .hole_ccw = true,
          .hole_repeated = true,
          .expected_shell_pts = 5,
          .expected_hole_pts = 5,
          .expect_shell_reused = false,
          .expect_hole_reused = false,
      },
      {
          .name = "ccw_shell_repeated_cw_hole_repeated",
          .shell_cw = false,
          .shell_repeated = true,
          .hole_ccw = false,
          .hole_repeated = true,
          .expected_shell_pts = 5,
          .expected_hole_pts = 5,
          .expect_shell_reused = false,
          .expect_hole_reused = false,
      },
  });

  for (const RingOrientCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    std::unique_ptr<LinearRing> shell;
    if (tc.shell_cw && !tc.shell_repeated) {
      shell = MakeRing(*factory, {{0, 0}, {0, 20}, {20, 20}, {20, 0}, {0, 0}});
    } else if (!tc.shell_cw && !tc.shell_repeated) {
      shell = MakeRing(*factory, {{0, 0}, {20, 0}, {20, 20}, {0, 20}, {0, 0}});
    } else if (tc.shell_cw && tc.shell_repeated) {
      shell = MakeRing(*factory,
                       {{0, 0}, {0, 20}, {0, 20}, {20, 20}, {20, 0}, {0, 0}});
    } else {
      shell = MakeRing(*factory,
                       {{0, 0}, {20, 0}, {20, 0}, {20, 20}, {0, 20}, {0, 0}});
    }

    std::vector<std::unique_ptr<LinearRing>> holes;
    if (tc.hole_ccw && !tc.hole_repeated) {
      holes.push_back(
          MakeRing(*factory, {{2, 2}, {8, 2}, {8, 8}, {2, 8}, {2, 2}}));
    } else if (!tc.hole_ccw && !tc.hole_repeated) {
      holes.push_back(
          MakeRing(*factory, {{2, 2}, {2, 8}, {8, 8}, {8, 2}, {2, 2}}));
    } else if (tc.hole_ccw && tc.hole_repeated) {
      holes.push_back(
          MakeRing(*factory, {{2, 2}, {8, 2}, {8, 2}, {8, 8}, {2, 8}, {2, 2}}));
    } else {
      holes.push_back(
          MakeRing(*factory, {{2, 2}, {2, 8}, {2, 8}, {8, 8}, {8, 2}, {2, 2}}));
    }

    std::unique_ptr<Polygon> poly =
        factory->createPolygon(std::move(shell), std::move(holes));
    const CoordinateSequence* orig_shell_cs =
        poly->getExteriorRing()->getCoordinatesRO();
    const CoordinateSequence* orig_hole_cs =
        poly->getInteriorRingN(0)->getCoordinatesRO();

    RelateGeometry rg(poly.get());
    std::vector<const SegmentString*> strings =
        rg.extractSegmentStrings(RelateGeometry::GEOM_A, nullptr);
    ASSERT_EQ(strings.size(), 2u);

    const CoordinateSequence* out_shell_cs = strings[0]->getCoordinates();
    const CoordinateSequence* out_hole_cs = strings[1]->getCoordinates();

    EXPECT_EQ(out_shell_cs == orig_shell_cs, tc.expect_shell_reused);
    EXPECT_EQ(out_hole_cs == orig_hole_cs, tc.expect_hole_reused);
    EXPECT_EQ(out_shell_cs->size(), tc.expected_shell_pts);
    EXPECT_EQ(out_hole_cs->size(), tc.expected_hole_pts);
    EXPECT_FALSE(out_shell_cs->hasRepeatedPoints());
    EXPECT_FALSE(out_hole_cs->hasRepeatedPoints());
    // Exterior rings must be CW (!isCCW); interior holes must be CCW (isCCW).
    EXPECT_FALSE(Orientation::isCCW(out_shell_cs));
    EXPECT_TRUE(Orientation::isCCW(out_hole_cs));
  }
}

}  // namespace
}  // namespace geos_google
