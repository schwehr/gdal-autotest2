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
// third_party/geos/src/triangulate/polygon/PolygonTriangulator.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/triangulate/polygon/PolygonTriangulator.cpp

#include "third_party/geos/include/geos/triangulate/polygon/PolygonTriangulator.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/geom/util/PolygonExtracter.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalStateException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::geom::util::PolygonExtracter;
using geos::io::WKTReader;
using geos::triangulate::polygon::PolygonTriangulator;

std::unique_ptr<Geometry> BuildExpectedPolygonalUnion(
    const Geometry& input, const GeometryFactory* factory) {
  std::vector<const Polygon*> raw_polys;
  PolygonExtracter::getPolygons(input, raw_polys);
  std::vector<std::unique_ptr<Geometry>> non_empty;
  for (const Polygon* poly : raw_polys) {
    if (!poly->isEmpty()) {
      non_empty.push_back(poly->clone());
    }
  }
  auto collection = factory->createGeometryCollection(std::move(non_empty));
  return collection->Union();
}

void VerifyTriangulation(const Geometry& input, const GeometryFactory* factory,
                         double expected_area, bool allow_zero_area = false) {
  std::unique_ptr<Geometry> result = PolygonTriangulator::triangulate(&input);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(result->getSRID(), factory->getSRID());
  ASSERT_FALSE(result->isEmpty());
  EXPECT_GE(result->getNumGeometries(), 1u);

  for (std::size_t i = 0; i < result->getNumGeometries(); ++i) {
    const Geometry* component = result->getGeometryN(i);
    ASSERT_NE(component, nullptr);
    EXPECT_EQ(component->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    const auto* tri_poly = dynamic_cast<const Polygon*>(component);
    ASSERT_NE(tri_poly, nullptr);
    EXPECT_FALSE(tri_poly->isEmpty());
    EXPECT_EQ(tri_poly->getNumInteriorRing(), 0u);
    const LinearRing* shell = tri_poly->getExteriorRing();
    ASSERT_NE(shell, nullptr);
    EXPECT_EQ(shell->getNumPoints(), 4u);
    EXPECT_TRUE(shell->getCoordinateN(0).equals2D(shell->getCoordinateN(3)));
    if (allow_zero_area) {
      EXPECT_GE(tri_poly->getArea(), 0.0);
    } else {
      EXPECT_GT(tri_poly->getArea(), 0.0);
      EXPECT_TRUE(tri_poly->isValid());
    }
  }

  std::unique_ptr<Geometry> tri_union = result->Union();
  std::unique_ptr<Geometry> expected_union =
      BuildExpectedPolygonalUnion(input, factory);
  ASSERT_NE(tri_union, nullptr);
  ASSERT_NE(expected_union, nullptr);
  EXPECT_TRUE(tri_union->equals(expected_union.get()));
  EXPECT_NEAR(result->getArea(), expected_area, 1e-9);
  EXPECT_NEAR(tri_union->getArea(), expected_area, 1e-9);
}

void VerifyTriangleVerticesMatchZ(
    const Geometry& result, const std::vector<Coordinate>& expected_vertices) {
  for (std::size_t i = 0; i < result.getNumGeometries(); ++i) {
    const auto* tri = dynamic_cast<const Polygon*>(result.getGeometryN(i));
    ASSERT_NE(tri, nullptr);
    const LinearRing* ring = tri->getExteriorRing();
    ASSERT_NE(ring, nullptr);
    for (std::size_t k = 0; k < ring->getNumPoints(); ++k) {
      const Coordinate& c = ring->getCoordinateN(k);
      ASSERT_FALSE(std::isnan(c.z));
      bool matched = false;
      for (const Coordinate& expected : expected_vertices) {
        if (c.equals2D(expected)) {
          EXPECT_DOUBLE_EQ(c.z, expected.z);
          matched = true;
          break;
        }
      }
      EXPECT_TRUE(matched) << "Unexpected vertex (" << c.x << ", " << c.y
                           << ")";
    }
  }
}

TEST(PolygonTriangulatorTest, EmptyGeometriesReturnEmptyCollection) {
  PrecisionModel pm(PrecisionModel::FLOATING);
  auto gf = GeometryFactory::create(&pm, 4326);
  WKTReader reader(gf.get());

  const std::vector<std::string> empty_wkts = {
      "POLYGON EMPTY",
      "MULTIPOLYGON EMPTY",
      "GEOMETRYCOLLECTION EMPTY",
      "POINT EMPTY",
      "LINESTRING EMPTY",
      "GEOMETRYCOLLECTION (POLYGON EMPTY)",
  };

  for (const std::string& wkt : empty_wkts) {
    std::unique_ptr<Geometry> geom = reader.read(wkt);
    ASSERT_NE(geom, nullptr) << wkt;
    std::unique_ptr<Geometry> result =
        PolygonTriangulator::triangulate(geom.get());
    ASSERT_NE(result, nullptr) << wkt;
    EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION)
        << wkt;
    EXPECT_TRUE(result->isEmpty()) << wkt;
    EXPECT_EQ(result->getNumGeometries(), 0u) << wkt;
    EXPECT_EQ(result->getSRID(), 4326) << wkt;
  }
}

TEST(PolygonTriangulatorTest, NonPolygonalGeometriesReturnEmptyCollection) {
  auto gf = GeometryFactory::create();
  WKTReader reader(gf.get());

  const std::vector<std::string> non_polygonal_wkts = {
      "POINT (1 2)",
      "LINESTRING (0 0, 5 5, 10 0)",
      "MULTIPOINT ((0 0), (1 1))",
      "MULTILINESTRING ((0 0, 1 1), (2 2, 3 3))",
      "GEOMETRYCOLLECTION (POINT (1 2), LINESTRING (0 0, 4 4))",
  };

  for (const std::string& wkt : non_polygonal_wkts) {
    std::unique_ptr<Geometry> geom = reader.read(wkt);
    ASSERT_NE(geom, nullptr) << wkt;
    std::unique_ptr<Geometry> result =
        PolygonTriangulator::triangulate(geom.get());
    ASSERT_NE(result, nullptr) << wkt;
    EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION)
        << wkt;
    EXPECT_TRUE(result->isEmpty()) << wkt;
    EXPECT_EQ(result->getNumGeometries(), 0u) << wkt;
  }
}

TEST(PolygonTriangulatorTest, SkipsEmptyComponentPolygonsInCollection) {
  auto gf = GeometryFactory::create();
  WKTReader reader(gf.get());

  // Non-empty collection containing only POINT + POLYGON EMPTY:
  // inputGeom->isEmpty() is false, loop runs and hits `if (poly->isEmpty())`.
  std::unique_ptr<Geometry> only_empty_poly =
      reader.read("GEOMETRYCOLLECTION (POINT (5 5), POLYGON EMPTY)");
  std::unique_ptr<Geometry> empty_result =
      PolygonTriangulator::triangulate(only_empty_poly.get());
  ASSERT_NE(empty_result, nullptr);
  EXPECT_EQ(empty_result->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_TRUE(empty_result->isEmpty());

  // Non-empty collection with both POLYGON EMPTY and a valid triangle.
  std::unique_ptr<Geometry> mixed_collection = reader.read(
      "GEOMETRYCOLLECTION (POLYGON EMPTY, POLYGON ((0 0, 4 0, 0 3, 0 0)))");
  VerifyTriangulation(*mixed_collection, gf.get(), 6.0);

  // Programmatic MultiPolygon combining an empty Polygon and a non-empty one.
  std::vector<std::unique_ptr<Geometry>> polys;
  polys.push_back(gf->createPolygon());
  polys.push_back(reader.read("POLYGON ((0 0, 4 0, 4 5, 0 5, 0 0))"));
  auto multi = gf->createMultiPolygon(std::move(polys));
  VerifyTriangulation(*multi, gf.get(), 20.0);
}

TEST(PolygonTriangulatorTest, TriangulatesConvexAndConcavePolygons) {
  auto gf = GeometryFactory::create();
  WKTReader reader(gf.get());

  // Single triangle (remains 1 triangle).
  std::unique_ptr<Geometry> tri = reader.read("POLYGON ((0 0, 6 0, 0 4, 0 0))");
  std::unique_ptr<Geometry> tri_res =
      PolygonTriangulator::triangulate(tri.get());
  EXPECT_EQ(tri_res->getNumGeometries(), 1u);
  VerifyTriangulation(*tri, gf.get(), 12.0);

  // Both CCW and CW squares produce 2 triangles.
  std::unique_ptr<Geometry> ccw_sq =
      reader.read("POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0))");
  std::unique_ptr<Geometry> cw_sq =
      reader.read("POLYGON ((0 0, 0 5, 5 5, 5 0, 0 0))");
  EXPECT_EQ(PolygonTriangulator::triangulate(ccw_sq.get())->getNumGeometries(),
            2u);
  EXPECT_EQ(PolygonTriangulator::triangulate(cw_sq.get())->getNumGeometries(),
            2u);
  VerifyTriangulation(*ccw_sq, gf.get(), 25.0);
  VerifyTriangulation(*cw_sq, gf.get(), 25.0);

  // Concave L-shape (area = 4*1 + 1*3 = 7) and 5-pointed chevron/star-like
  // shape.
  std::unique_ptr<Geometry> l_shape =
      reader.read("POLYGON ((0 0, 4 0, 4 1, 1 1, 1 4, 0 4, 0 0))");
  VerifyTriangulation(*l_shape, gf.get(), 7.0);

  std::unique_ptr<Geometry> chevron =
      reader.read("POLYGON ((0 0, 5 2, 10 0, 6 10, 5 4, 4 10, 0 0))");
  VerifyTriangulation(*chevron, gf.get(), 44.0);

  // Polygon with collinear vertex on an edge.
  std::unique_ptr<Geometry> with_collinear =
      reader.read("POLYGON ((0 0, 5 0, 10 0, 10 4, 0 4, 0 0))");
  VerifyTriangulation(*with_collinear, gf.get(), 40.0,
                      /*allow_zero_area=*/true);
}

TEST(PolygonTriangulatorTest, EarClipsDiamondWithoutDelaunayFlip) {
  auto gf = GeometryFactory::create();
  WKTReader reader(gf.get());

  // Diamond (0,1)-(5,0)-(10,1)-(5,2): PolygonTriangulator uses ear-clipping
  // without Delaunay edge-flipping (unlike ConstrainedDelaunayTriangulator).
  // Oriented CW as (0,1)-(5,2)-(10,1)-(5,0)-(0,1), clipping the first ear
  // splits along the horizontal diagonal (0,1)-(10,1).
  std::unique_ptr<Geometry> diamond =
      reader.read("POLYGON ((0 1, 5 0, 10 1, 5 2, 0 1))");
  std::unique_ptr<Geometry> result =
      PolygonTriangulator::triangulate(diamond.get());
  ASSERT_NE(result, nullptr);
  ASSERT_EQ(result->getNumGeometries(), 2u);
  VerifyTriangulation(*diamond, gf.get(), 10.0);

  int tris_with_horizontal_diagonal = 0;
  int tris_with_top = 0;
  int tris_with_bottom = 0;
  for (std::size_t i = 0; i < 2; ++i) {
    const auto* poly = dynamic_cast<const Polygon*>(result->getGeometryN(i));
    ASSERT_NE(poly, nullptr);
    EXPECT_DOUBLE_EQ(poly->getArea(), 5.0);
    const LinearRing* ring = poly->getExteriorRing();
    bool has_left = false;
    bool has_right = false;
    bool has_top = false;
    bool has_bottom = false;
    for (std::size_t k = 0; k < 3; ++k) {
      const Coordinate& c = ring->getCoordinateN(k);
      if (c.equals2D(Coordinate(0, 1))) has_left = true;
      if (c.equals2D(Coordinate(10, 1))) has_right = true;
      if (c.equals2D(Coordinate(5, 2))) has_top = true;
      if (c.equals2D(Coordinate(5, 0))) has_bottom = true;
    }
    if (has_left && has_right) {
      ++tris_with_horizontal_diagonal;
    }
    if (has_top) ++tris_with_top;
    if (has_bottom) ++tris_with_bottom;
    EXPECT_NE(has_top, has_bottom);
  }
  EXPECT_EQ(tris_with_horizontal_diagonal, 2);
  EXPECT_EQ(tris_with_top, 1);
  EXPECT_EQ(tris_with_bottom, 1);
}

TEST(PolygonTriangulatorTest,
     TriangulatesPolygonsWithDisjointAndTouchingHoles) {
  auto gf = GeometryFactory::create();
  WKTReader reader(gf.get());

  // Single centered square hole: 100 - 36 = 64.
  std::unique_ptr<Geometry> one_hole = reader.read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(2 2, 2 8, 8 8, 8 2, 2 2))");
  VerifyTriangulation(*one_hole, gf.get(), 64.0);

  // Two disjoint holes with mixed orientations: 200 - 36 - 36 = 128.
  std::unique_ptr<Geometry> two_holes = reader.read(
      "POLYGON ((0 0, 20 0, 20 10, 0 10, 0 0), "
      "(2 2, 8 2, 8 8, 2 8, 2 2), "
      "(12 2, 12 8, 18 8, 18 2, 12 2))");
  VerifyTriangulation(*two_holes, gf.get(), 128.0);

  // Hole touching outer shell at vertex (5, 0): 100 - 8 = 92.
  std::unique_ptr<Geometry> shell_touching_hole = reader.read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(5 0, 3 4, 7 4, 5 0))");
  VerifyTriangulation(*shell_touching_hole, gf.get(), 92.0);

  // Two diamond holes touching each other at vertex (8, 5): 160 - 18 - 18 =
  // 124.
  std::unique_ptr<Geometry> mutually_touching_holes = reader.read(
      "POLYGON ((0 0, 16 0, 16 10, 0 10, 0 0), "
      "(2 5, 5 2, 8 5, 5 8, 2 5), "
      "(8 5, 11 2, 14 5, 11 8, 8 5))");
  VerifyTriangulation(*mutually_touching_holes, gf.get(), 124.0);

  // Concave U-shaped polygon with a rectangular hole in the base: 112 - 16
  // = 96.
  std::unique_ptr<Geometry> concave_with_hole = reader.read(
      "POLYGON ((0 0, 12 0, 12 12, 8 12, 8 4, 4 4, 4 12, 0 12, 0 0), "
      "(2 1, 10 1, 10 3, 2 3, 2 1))");
  VerifyTriangulation(*concave_with_hole, gf.get(), 96.0);
}

TEST(PolygonTriangulatorTest, TriangulatesMultiPolygonAndMixedCollection) {
  auto gf = GeometryFactory::create();
  WKTReader reader(gf.get());

  // MultiPolygon where one polygon has a hole and another is simple:
  // (100 - 16) + 12 = 96.
  std::unique_ptr<Geometry> multi_poly = reader.read(
      "MULTIPOLYGON ("
      "((0 0, 10 0, 10 10, 0 10, 0 0), (3 3, 7 3, 7 7, 3 7, 3 3)), "
      "((15 0, 21 0, 15 4, 15 0))"
      ")");
  VerifyTriangulation(*multi_poly, gf.get(), 96.0);

  // Nested GeometryCollection mixing points, lines, and nested collections.
  std::unique_ptr<Geometry> mixed = reader.read(
      "GEOMETRYCOLLECTION ("
      "POINT (-5 -5), "
      "LINESTRING (-10 0, -1 0), "
      "POLYGON ((0 0, 4 0, 4 5, 0 5, 0 0)), "
      "GEOMETRYCOLLECTION (POLYGON ((10 0, 16 0, 16 4, 10 4, 10 0)))"
      ")");
  VerifyTriangulation(*mixed, gf.get(), 44.0);
}

TEST(PolygonTriangulatorTest, Preserves3DZCoordinatesForSimplePolygon) {
  auto gf = GeometryFactory::create();
  WKTReader reader(gf.get());

  std::unique_ptr<Geometry> poly_z =
      reader.read("POLYGON Z ((0 0 10, 10 0 20, 10 10 30, 0 10 40, 0 0 10))");
  std::unique_ptr<Geometry> result =
      PolygonTriangulator::triangulate(poly_z.get());
  ASSERT_EQ(result->getNumGeometries(), 2u);
  VerifyTriangulation(*poly_z, gf.get(), 100.0);
  VerifyTriangleVerticesMatchZ(*result, {
                                            Coordinate(0, 0, 10),
                                            Coordinate(10, 0, 20),
                                            Coordinate(10, 10, 30),
                                            Coordinate(0, 10, 40),
                                        });
}

TEST(PolygonTriangulatorTest, Preserves3DZCoordinatesWithHole) {
  auto gf = GeometryFactory::create();
  WKTReader reader(gf.get());

  std::unique_ptr<Geometry> poly_z_hole = reader.read(
      "POLYGON Z ((0 0 10, 10 0 20, 10 10 30, 0 10 40, 0 0 10), "
      "(2 2 100, 2 8 200, 8 8 300, 8 2 400, 2 2 100))");
  std::unique_ptr<Geometry> result =
      PolygonTriangulator::triangulate(poly_z_hole.get());
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getNumGeometries(), 8u);
  VerifyTriangulation(*poly_z_hole, gf.get(), 64.0);
  VerifyTriangleVerticesMatchZ(*result, {
                                            Coordinate(0, 0, 10),
                                            Coordinate(10, 0, 20),
                                            Coordinate(10, 10, 30),
                                            Coordinate(0, 10, 40),
                                            Coordinate(2, 2, 100),
                                            Coordinate(2, 8, 200),
                                            Coordinate(8, 8, 300),
                                            Coordinate(8, 2, 400),
                                        });
}

TEST(PolygonTriangulatorTest, PreservesFactorySridAndDirectConstruction) {
  PrecisionModel pm(PrecisionModel::FLOATING);
  auto gf = GeometryFactory::create(&pm, 3857);
  WKTReader reader(gf.get());

  std::unique_ptr<Geometry> poly =
      reader.read("POLYGON ((0 0, 8 0, 8 6, 0 6, 0 0))");
  // Public constructor accepts a non-null Geometry* and captures its factory.
  PolygonTriangulator direct_triangulator(poly.get());
  (void)direct_triangulator;

  std::unique_ptr<Geometry> result =
      PolygonTriangulator::triangulate(poly.get());
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getSRID(), 3857);
  EXPECT_EQ(result->getFactory(), gf.get());
  ASSERT_EQ(result->getNumGeometries(), 2u);
  for (std::size_t i = 0; i < result->getNumGeometries(); ++i) {
    EXPECT_EQ(result->getGeometryN(i)->getSRID(), 3857);
  }
  VerifyTriangulation(*poly, gf.get(), 48.0);
}

TEST(PolygonTriangulatorTest, DegenerateCollinearPolygonThrows) {
  auto gf = GeometryFactory::create();
  WKTReader reader(gf.get());

  std::unique_ptr<Geometry> degenerate =
      reader.read("POLYGON ((0 0, 1 0, 2 0, 0 0))");
  EXPECT_THROW(PolygonTriangulator::triangulate(degenerate.get()),
               geos::util::IllegalStateException);
}

}  // namespace
}  // namespace geos_google
