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

#include "third_party/geos/include/geos/triangulate/polygon/ConstrainedDelaunayTriangulator.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/triangulate/tri/Tri.h"
#include "third_party/geos/include/geos/triangulate/tri/TriList.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateXY;
using ::geos::geom::Geometry;
using ::geos::geom::GeometryCollection;
using ::geos::geom::GeometryFactory;
using ::geos::geom::Polygon;
using ::geos::io::WKTReader;
using ::geos::triangulate::polygon::ConstrainedDelaunayTriangulator;
using ::geos::triangulate::tri::Tri;
using ::geos::triangulate::tri::TriList;

constexpr double kEps = 1e-6;

class ConstrainedDelaunayTriangulatorTest : public ::testing::Test {
 protected:
  ConstrainedDelaunayTriangulatorTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  static double SumTriangleAreas(const GeometryCollection& tris) {
    double total = 0.0;
    for (std::size_t i = 0; i < tris.getNumGeometries(); ++i) {
      total += tris.getGeometryN(i)->getArea();
    }
    return total;
  }

  static void ExpectValidTriangulation(const Geometry& input,
                                       const Geometry& result,
                                       std::size_t expected_tri_count,
                                       double expected_area) {
    ASSERT_EQ(result.getGeometryTypeId(),
              ::geos::geom::GEOS_GEOMETRYCOLLECTION);
    const auto* collection = dynamic_cast<const GeometryCollection*>(&result);
    ASSERT_NE(collection, nullptr);
    ASSERT_EQ(collection->getNumGeometries(), expected_tri_count);

    for (std::size_t i = 0; i < collection->getNumGeometries(); ++i) {
      const auto* tri =
          dynamic_cast<const Polygon*>(collection->getGeometryN(i));
      ASSERT_NE(tri, nullptr) << "element " << i << " is not a Polygon";
      EXPECT_TRUE(tri->isValid()) << "triangle " << i << " is invalid";
      EXPECT_EQ(tri->getNumInteriorRing(), 0u);
      EXPECT_EQ(tri->getExteriorRing()->getNumPoints(), 4u);
      EXPECT_GT(tri->getArea(), 0.0) << "triangle " << i << " is degenerate";
    }

    EXPECT_NEAR(SumTriangleAreas(*collection), expected_area, kEps);

    auto unioned = collection->Union();
    ASSERT_NE(unioned, nullptr);
    EXPECT_NEAR(unioned->getArea(), expected_area, kEps);
    EXPECT_TRUE(unioned->equals(&input));
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(ConstrainedDelaunayTriangulatorTest,
       EmptyGeometriesReturnEmptyCollection) {
  const std::vector<std::string> empty_wkts = {
      "POLYGON EMPTY", "MULTIPOLYGON EMPTY", "GEOMETRYCOLLECTION EMPTY",
      "POINT EMPTY",   "LINESTRING EMPTY",
  };

  for (const std::string& wkt : empty_wkts) {
    auto geom = reader_.read(wkt);
    ASSERT_NE(geom, nullptr) << wkt;

    auto result = ConstrainedDelaunayTriangulator::triangulate(geom.get());
    ASSERT_NE(result, nullptr) << wkt;
    EXPECT_EQ(result->getGeometryTypeId(),
              ::geos::geom::GEOS_GEOMETRYCOLLECTION);
    EXPECT_TRUE(result->isEmpty()) << wkt;
    EXPECT_EQ(result->getNumGeometries(), 0u) << wkt;
  }
}

TEST_F(ConstrainedDelaunayTriangulatorTest,
       NonPolygonGeometriesReturnEmptyCollection) {
  const std::vector<std::string> non_poly_wkts = {
      "POINT (1 2)",
      "MULTIPOINT ((0 0), (2 3))",
      "LINESTRING (0 0, 5 5, 10 0)",
      "MULTILINESTRING ((0 0, 1 1), (2 2, 3 3))",
  };

  for (const std::string& wkt : non_poly_wkts) {
    auto geom = reader_.read(wkt);
    ASSERT_NE(geom, nullptr) << wkt;

    auto result = ConstrainedDelaunayTriangulator::triangulate(geom.get());
    ASSERT_NE(result, nullptr) << wkt;
    EXPECT_EQ(result->getGeometryTypeId(),
              ::geos::geom::GEOS_GEOMETRYCOLLECTION);
    EXPECT_TRUE(result->isEmpty()) << wkt;
    EXPECT_EQ(result->getNumGeometries(), 0u) << wkt;
  }
}

TEST_F(ConstrainedDelaunayTriangulatorTest,
       CurvedGeometriesThrowUnsupportedOperation) {
  const std::vector<std::string> curved_wkts = {
      "CIRCULARSTRING (0 0, 1 1, 2 0)",
      "CURVEPOLYGON (CIRCULARSTRING (0 0, 2 0, 2 2, 0 2, 0 0))",
      "MULTISURFACE (CURVEPOLYGON (CIRCULARSTRING (0 0, 2 0, 2 2, 0 2, 0 0)))",
  };

  for (const std::string& wkt : curved_wkts) {
    auto geom = reader_.read(wkt);
    ASSERT_NE(geom, nullptr) << wkt;
    EXPECT_THROW(ConstrainedDelaunayTriangulator::triangulate(geom.get()),
                 ::geos::util::UnsupportedOperationException)
        << wkt;
  }
}

TEST_F(ConstrainedDelaunayTriangulatorTest, SingleTriangleAndConvexSquare) {
  auto tri_geom = reader_.read("POLYGON ((0 0, 10 0, 3 7, 0 0))");
  auto tri_result =
      ConstrainedDelaunayTriangulator::triangulate(tri_geom.get());
  ASSERT_NE(tri_result, nullptr);
  ExpectValidTriangulation(*tri_geom, *tri_result, 1u, 35.0);

  auto square = reader_.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto square_result =
      ConstrainedDelaunayTriangulator::triangulate(square.get());
  ASSERT_NE(square_result, nullptr);
  ExpectValidTriangulation(*square, *square_result, 2u, 100.0);
  EXPECT_FALSE(square_result->hasZ());
}

TEST_F(ConstrainedDelaunayTriangulatorTest,
       DelaunayImproverChoosesShortDiagonalOfDiamond) {
  // Ear clipper initially splits along (0,0)-(20,0) or (10,-1)-(10,1);
  // Delaunay improvement guarantees the shorter diagonal (10,-1)-(10,1)
  // is used so neither triangle contains the fourth vertex in its circumcircle.
  auto diamond = reader_.read("POLYGON ((0 0, 10 -1, 20 0, 10 1, 0 0))");
  auto result = ConstrainedDelaunayTriangulator::triangulate(diamond.get());
  ASSERT_NE(result, nullptr);
  ExpectValidTriangulation(*diamond, *result, 2u, 20.0);

  const auto* collection =
      dynamic_cast<const GeometryCollection*>(result.get());
  ASSERT_NE(collection, nullptr);
  const CoordinateXY top(10.0, 1.0);
  const CoordinateXY bottom(10.0, -1.0);
  for (std::size_t i = 0; i < collection->getNumGeometries(); ++i) {
    const auto* poly =
        dynamic_cast<const Polygon*>(collection->getGeometryN(i));
    ASSERT_NE(poly, nullptr);
    const CoordinateSequence* ring =
        poly->getExteriorRing()->getCoordinatesRO();
    bool has_top = false;
    bool has_bottom = false;
    for (std::size_t k = 0; k + 1 < ring->size(); ++k) {
      CoordinateXY c;
      ring->getAt(k, c);
      if (c.equals2D(top)) has_top = true;
      if (c.equals2D(bottom)) has_bottom = true;
    }
    EXPECT_TRUE(has_top && has_bottom)
        << "triangle " << i << " did not use Delaunay diagonal (10,-1)-(10,1)";
  }
}

TEST_F(ConstrainedDelaunayTriangulatorTest, ConcaveAndClockwisePolygons) {
  // L-shaped polygon (6 vertices -> 4 triangles, area = 36).
  auto l_shape =
      reader_.read("POLYGON ((0 0, 10 0, 10 2, 2 2, 2 10, 0 10, 0 0))");
  auto l_result = ConstrainedDelaunayTriangulator::triangulate(l_shape.get());
  ASSERT_NE(l_result, nullptr);
  ExpectValidTriangulation(*l_shape, *l_result, 4u, 36.0);

  // Clockwise U-shaped polygon (8 vertices -> 6 triangles, area = 52).
  auto u_shape_cw = reader_.read(
      "POLYGON ((0 0, 0 10, 2 10, 2 2, 8 2, 8 10, 10 10, 10 0, 0 0))");
  auto u_result =
      ConstrainedDelaunayTriangulator::triangulate(u_shape_cw.get());
  ASSERT_NE(u_result, nullptr);
  ExpectValidTriangulation(*u_shape_cw, *u_result, 6u, 52.0);
}

TEST_F(ConstrainedDelaunayTriangulatorTest, PolygonWithSingleAndMultipleHoles) {
  // 10x10 square with 4x4 hole: 8 vertices, 1 hole -> 8 + 2*1 - 2 = 8
  // triangles.
  auto one_hole = reader_.read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (3 3, 7 3, 7 7, 3 7, 3 3))");
  auto one_hole_result =
      ConstrainedDelaunayTriangulator::triangulate(one_hole.get());
  ASSERT_NE(one_hole_result, nullptr);
  ExpectValidTriangulation(*one_hole, *one_hole_result, 8u, 84.0);

  // 20x10 rectangle with two 4x6 holes: 12 vertices, 2 holes -> 14 triangles.
  auto two_holes = reader_.read(
      "POLYGON ((0 0, 20 0, 20 10, 0 10, 0 0), "
      "(2 2, 2 8, 6 8, 6 2, 2 2), "
      "(12 2, 16 2, 16 8, 12 8, 12 2))");
  auto two_holes_result =
      ConstrainedDelaunayTriangulator::triangulate(two_holes.get());
  ASSERT_NE(two_holes_result, nullptr);
  ExpectValidTriangulation(*two_holes, *two_holes_result, 14u, 152.0);
}

TEST_F(ConstrainedDelaunayTriangulatorTest, PolygonWithHoleTouchingShell) {
  auto touching_hole = reader_.read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (5 0, 3 5, 7 5, 5 0))");
  auto result =
      ConstrainedDelaunayTriangulator::triangulate(touching_hole.get());
  ASSERT_NE(result, nullptr);
  ExpectValidTriangulation(*touching_hole, *result, 6u, 90.0);
}

TEST_F(ConstrainedDelaunayTriangulatorTest,
       MultiPolygonAndMixedCollectionSkipEmptyPolygonComponents) {
  auto multi_poly = reader_.read(
      "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
      "((10 0, 14 0, 14 4, 10 4, 10 0)))");
  auto multi_result =
      ConstrainedDelaunayTriangulator::triangulate(multi_poly.get());
  ASSERT_NE(multi_result, nullptr);
  ExpectValidTriangulation(*multi_poly, *multi_result, 4u, 32.0);

  // Collection mixes non-polygons, empty polygon components, and valid
  // polygons.
  auto mixed = reader_.read(
      "GEOMETRYCOLLECTION ("
      "POINT (1 1), "
      "LINESTRING (0 0, 5 5), "
      "POLYGON EMPTY, "
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0)), "
      "MULTIPOLYGON (EMPTY, ((10 0, 13 0, 10 4, 10 0))))");
  auto mixed_result = ConstrainedDelaunayTriangulator::triangulate(mixed.get());
  ASSERT_NE(mixed_result, nullptr);

  auto expected_union = reader_.read(
      "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
      "((10 0, 13 0, 10 4, 10 0)))");
  ExpectValidTriangulation(*expected_union, *mixed_result, 3u, 22.0);
}

TEST_F(ConstrainedDelaunayTriangulatorTest, TriangulatePolygonStaticApi) {
  auto poly = reader_.read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 8 2, 8 8, 2 8, 2 2))");
  const auto* poly_ptr = dynamic_cast<const Polygon*>(poly.get());
  ASSERT_NE(poly_ptr, nullptr);

  TriList<Tri> tri_list;
  ConstrainedDelaunayTriangulator::triangulatePolygon(poly_ptr, tri_list);
  EXPECT_EQ(tri_list.size(), 8u);
  EXPECT_NEAR(tri_list.area(), 64.0, kEps);
  EXPECT_NO_THROW(tri_list.validate());

  auto geom = tri_list.toGeometry(factory_.get());
  ASSERT_NE(geom, nullptr);
  ExpectValidTriangulation(*poly, *geom, 8u, 64.0);
}

TEST_F(ConstrainedDelaunayTriangulatorTest, PolygonWithCollinearVertices) {
  auto poly =
      reader_.read("POLYGON ((0 0, 5 0, 10 0, 10 10, 5 10, 0 10, 0 0))");
  auto result = ConstrainedDelaunayTriangulator::triangulate(poly.get());
  ASSERT_NE(result, nullptr);
  ExpectValidTriangulation(*poly, *result, 4u, 100.0);
}

TEST_F(ConstrainedDelaunayTriangulatorTest, PreservesZCoordinates) {
  auto poly_z =
      reader_.read("POLYGON Z ((0 0 1, 10 0 2, 10 10 3, 0 10 4, 0 0 1))");
  auto result = ConstrainedDelaunayTriangulator::triangulate(poly_z.get());
  ASSERT_NE(result, nullptr);
  ExpectValidTriangulation(*poly_z, *result, 2u, 100.0);

  const auto* collection =
      dynamic_cast<const GeometryCollection*>(result.get());
  ASSERT_NE(collection, nullptr);
  for (std::size_t i = 0; i < collection->getNumGeometries(); ++i) {
    const auto* tri = dynamic_cast<const Polygon*>(collection->getGeometryN(i));
    ASSERT_NE(tri, nullptr);
    EXPECT_TRUE(tri->hasZ());
    const CoordinateSequence* ring = tri->getExteriorRing()->getCoordinatesRO();
    for (std::size_t k = 0; k < ring->size(); ++k) {
      double z = ring->getOrdinate(k, CoordinateSequence::Z);
      EXPECT_GE(z, 1.0 - kEps);
      EXPECT_LE(z, 4.0 + kEps);
    }
  }
}

}  // namespace
}  // namespace geos_google
