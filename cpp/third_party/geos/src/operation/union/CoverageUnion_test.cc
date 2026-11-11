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

#include "third_party/geos/include/geos/operation/union/CoverageUnion.h"

#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::operation::geounion::CoverageUnion;
using geos::util::IllegalArgumentException;
using geos::util::TopologyException;

class CoverageUnionTest : public ::testing::Test {
 protected:
  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader reader_{*factory_};

  std::unique_ptr<Geometry> Read(const std::string& wkt) const {
    return reader_.read(wkt);
  }
};

TEST_F(CoverageUnionTest, SinglePolygonWithAndWithoutHoles) {
  auto simple = Read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto simple_res = CoverageUnion::Union(simple.get());
  ASSERT_NE(simple_res, nullptr);
  EXPECT_EQ(simple_res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(simple_res->isValid());
  EXPECT_TRUE(simple_res->equals(simple.get()));
  EXPECT_DOUBLE_EQ(simple_res->getArea(), 100.0);

  auto with_hole = Read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(2 2, 8 2, 8 8, 2 8, 2 2))");
  auto hole_res = CoverageUnion::Union(with_hole.get());
  ASSERT_NE(hole_res, nullptr);
  EXPECT_EQ(hole_res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  const auto* hole_poly = dynamic_cast<const Polygon*>(hole_res.get());
  ASSERT_NE(hole_poly, nullptr);
  EXPECT_EQ(hole_poly->getNumInteriorRing(), 1);
  EXPECT_TRUE(hole_poly->equals(with_hole.get()));
  EXPECT_DOUBLE_EQ(hole_poly->getArea(), 64.0);
}

TEST_F(CoverageUnionTest, AdjacentSquaresMergeRegardlessOfRingWinding) {
  auto ccw_pair = Read(
      "MULTIPOLYGON ("
      "((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "((10 0, 20 0, 20 10, 10 10, 10 0)))");
  auto expected = Read("POLYGON ((0 0, 20 0, 20 10, 0 10, 0 0))");

  auto res1 = CoverageUnion::Union(ccw_pair.get());
  ASSERT_NE(res1, nullptr);
  EXPECT_EQ(res1->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(res1->isValid());
  EXPECT_TRUE(res1->equals(expected.get()));
  EXPECT_DOUBLE_EQ(res1->getArea(), 200.0);

  // Second polygon wound clockwise along the shared edge (10 0 -> 10 10).
  auto mixed_winding = Read(
      "MULTIPOLYGON ("
      "((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "((10 0, 10 10, 20 10, 20 0, 10 0)))");
  auto res2 = CoverageUnion::Union(mixed_winding.get());
  ASSERT_NE(res2, nullptr);
  EXPECT_EQ(res2->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(res2->equals(expected.get()));
}

TEST_F(CoverageUnionTest, DisjointAndPointTouchingPolygonsReturnMultiPolygon) {
  auto disjoint = Read(
      "MULTIPOLYGON ("
      "((0 0, 5 0, 5 5, 0 5, 0 0)), "
      "((10 0, 15 0, 15 5, 10 5, 10 0)))");
  auto res_disjoint = CoverageUnion::Union(disjoint.get());
  ASSERT_NE(res_disjoint, nullptr);
  EXPECT_EQ(res_disjoint->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(res_disjoint->getNumGeometries(), 2);
  EXPECT_TRUE(res_disjoint->equals(disjoint.get()));
  EXPECT_DOUBLE_EQ(res_disjoint->getArea(), 50.0);

  // Checkerboard squares touching at a single corner vertex (5, 5).
  auto corner_touch = Read(
      "MULTIPOLYGON ("
      "((0 0, 5 0, 5 5, 0 5, 0 0)), "
      "((5 5, 10 5, 10 10, 5 10, 5 5)))");
  auto res_corner = CoverageUnion::Union(corner_touch.get());
  ASSERT_NE(res_corner, nullptr);
  EXPECT_EQ(res_corner->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(res_corner->getNumGeometries(), 2);
  EXPECT_TRUE(res_corner->equals(corner_touch.get()));
  EXPECT_DOUBLE_EQ(res_corner->getArea(), 50.0);

  // Two adjacent squares merge while a third disjoint square stays separate.
  auto partial_merge = Read(
      "MULTIPOLYGON ("
      "((0 0, 5 0, 5 5, 0 5, 0 0)), "
      "((5 0, 10 0, 10 5, 5 5, 5 0)), "
      "((20 0, 25 0, 25 5, 20 5, 20 0)))");
  auto expected_partial = Read(
      "MULTIPOLYGON ("
      "((0 0, 10 0, 10 5, 0 5, 0 0)), "
      "((20 0, 25 0, 25 5, 20 5, 20 0)))");
  auto res_partial = CoverageUnion::Union(partial_merge.get());
  ASSERT_NE(res_partial, nullptr);
  EXPECT_EQ(res_partial->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(res_partial->getNumGeometries(), 2);
  EXPECT_TRUE(res_partial->equals(expected_partial.get()));
  EXPECT_DOUBLE_EQ(res_partial->getArea(), 75.0);
}

TEST_F(CoverageUnionTest, EnclosingHoleFillingHoleAndIslandInHole) {
  // Two C-shaped halves enclosing central hole [2, 8] x [2, 8].
  auto c_halves = Read(
      "MULTIPOLYGON ("
      "((0 0, 5 0, 5 2, 2 2, 2 8, 5 8, 5 10, 0 10, 0 0)), "
      "((5 0, 10 0, 10 10, 5 10, 5 8, 8 8, 8 2, 5 2, 5 0)))");
  auto expected_holed = Read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(2 2, 8 2, 8 8, 2 8, 2 2))");
  auto holed_res = CoverageUnion::Union(c_halves.get());
  ASSERT_NE(holed_res, nullptr);
  EXPECT_EQ(holed_res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  const auto* poly_with_hole = dynamic_cast<const Polygon*>(holed_res.get());
  ASSERT_NE(poly_with_hole, nullptr);
  EXPECT_EQ(poly_with_hole->getNumInteriorRing(), 1);
  EXPECT_TRUE(poly_with_hole->equals(expected_holed.get()));
  EXPECT_DOUBLE_EQ(poly_with_hole->getArea(), 64.0);

  // A polygon with a hole paired with a matching filler removes the hole.
  auto filled = Read(
      "MULTIPOLYGON ("
      "((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 8 2, 8 8, 2 8, 2 2)), "
      "((2 2, 8 2, 8 8, 2 8, 2 2)))");
  auto expected_solid = Read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto filled_res = CoverageUnion::Union(filled.get());
  ASSERT_NE(filled_res, nullptr);
  EXPECT_EQ(filled_res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  const auto* solid_poly = dynamic_cast<const Polygon*>(filled_res.get());
  ASSERT_NE(solid_poly, nullptr);
  EXPECT_EQ(solid_poly->getNumInteriorRing(), 0);
  EXPECT_TRUE(solid_poly->equals(expected_solid.get()));
  EXPECT_DOUBLE_EQ(solid_poly->getArea(), 100.0);

  // Island sitting strictly inside a hole stays separate in a MultiPolygon.
  auto with_island = Read(
      "MULTIPOLYGON ("
      "((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 8 2, 8 8, 2 8, 2 2)), "
      "((3 3, 7 3, 7 7, 3 7, 3 3)))");
  auto island_res = CoverageUnion::Union(with_island.get());
  ASSERT_NE(island_res, nullptr);
  EXPECT_EQ(island_res->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(island_res->getNumGeometries(), 2);
  EXPECT_TRUE(island_res->equals(with_island.get()));
  EXPECT_DOUBLE_EQ(island_res->getArea(), 80.0);
}

TEST_F(CoverageUnionTest, GeometryCollectionAndNestedCollections) {
  auto flat_gc = Read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0)), "
      "POLYGON ((5 0, 10 0, 10 5, 5 5, 5 0)))");
  auto expected_flat = Read("POLYGON ((0 0, 10 0, 10 5, 0 5, 0 0))");
  auto res_flat = CoverageUnion::Union(flat_gc.get());
  ASSERT_NE(res_flat, nullptr);
  EXPECT_EQ(res_flat->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(res_flat->equals(expected_flat.get()));

  auto nested_gc = Read(
      "GEOMETRYCOLLECTION ("
      "MULTIPOLYGON (((0 0, 5 0, 5 5, 0 5, 0 0)), "
      "((5 0, 10 0, 10 5, 5 5, 5 0))), "
      "POLYGON ((0 5, 5 5, 10 5, 10 10, 0 10, 0 5)))");
  auto expected_square = Read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto res_nested = CoverageUnion::Union(nested_gc.get());
  ASSERT_NE(res_nested, nullptr);
  EXPECT_EQ(res_nested->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(res_nested->equals(expected_square.get()));
  EXPECT_DOUBLE_EQ(res_nested->getArea(), 100.0);
}

TEST_F(CoverageUnionTest, GridCoverageDissolvesAllInteriorEdges) {
  std::vector<std::unique_ptr<Polygon>> squares;
  for (int ix = 0; ix < 4; ++ix) {
    for (int iy = 0; iy < 4; ++iy) {
      std::ostringstream wkt;
      wkt << "POLYGON ((" << ix << " " << iy << ", " << (ix + 1) << " " << iy
          << ", " << (ix + 1) << " " << (iy + 1) << ", " << ix << " "
          << (iy + 1) << ", " << ix << " " << iy << "))";
      squares.push_back(reader_.read<Polygon>(wkt.str()));
    }
  }
  auto grid = factory_->createMultiPolygon(std::move(squares));
  auto expected = Read("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");

  auto result = CoverageUnion::Union(grid.get());
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(result->isValid());
  EXPECT_TRUE(result->equals(expected.get()));
  EXPECT_DOUBLE_EQ(result->getArea(), 16.0);
}

TEST_F(CoverageUnionTest, ThrowsIllegalArgumentExceptionOnNonPolygonalInputs) {
  auto pt = Read("POINT (1 1)");
  auto line = Read("LINESTRING (0 0, 5 5)");
  auto ring = Read("LINEARRING (0 0, 5 0, 5 5, 0 5, 0 0)");
  auto multi_pt = Read("MULTIPOINT ((0 0), (1 1))");
  auto multi_line = Read("MULTILINESTRING ((0 0, 5 0), (5 0, 5 5))");
  auto gc_with_line = Read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0)), "
      "LINESTRING (5 0, 10 0))");
  auto gc_with_pt = Read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0)), "
      "POINT (10 10))");

  EXPECT_THROW(CoverageUnion::Union(pt.get()), IllegalArgumentException);
  EXPECT_THROW(CoverageUnion::Union(line.get()), IllegalArgumentException);
  EXPECT_THROW(CoverageUnion::Union(ring.get()), IllegalArgumentException);
  EXPECT_THROW(CoverageUnion::Union(multi_pt.get()), IllegalArgumentException);
  EXPECT_THROW(CoverageUnion::Union(multi_line.get()),
               IllegalArgumentException);
  EXPECT_THROW(CoverageUnion::Union(gc_with_line.get()),
               IllegalArgumentException);
  EXPECT_THROW(CoverageUnion::Union(gc_with_pt.get()),
               IllegalArgumentException);
}

TEST_F(CoverageUnionTest, ThrowsTopologyExceptionOnIncorrectlyNodedInputs) {
  // Two individually valid polygons whose cancelled shared segment leaves a
  // self-intersecting ring that Polygonizer rejects.
  auto unnoded_pair = Read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 5 0, 10 0, 10 10, 0 10, 0 0)), "
      "POLYGON ((5 0, 10 0, 2 15, 5 0)))");
  EXPECT_THROW(CoverageUnion::Union(unnoded_pair.get()), TopologyException);

  // Self-intersecting bowtie ring is also rejected by Polygonizer.
  auto bowtie = Read("POLYGON ((0 0, 10 10, 10 0, 0 10, 0 0))");
  EXPECT_THROW(CoverageUnion::Union(bowtie.get()), TopologyException);
}

TEST_F(CoverageUnionTest, ThrowsTopologyExceptionOnOverlappingInputs) {
  // Duplicate polygons: all segments cancel -> area_out (0) != area_in (200).
  auto duplicate = Read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)))");
  EXPECT_THROW(CoverageUnion::Union(duplicate.get()), TopologyException);

  // Overlapping interior rectangle sharing a noded bottom edge segment:
  // polygonizes cleanly into a cutout of area 70, mismatching area_in = 130.
  auto overlapping = Read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 2 0, 8 0, 10 0, 10 10, 0 10, 0 0)), "
      "POLYGON ((2 0, 8 0, 8 5, 2 5, 2 0)))");
  EXPECT_THROW(CoverageUnion::Union(overlapping.get()), TopologyException);

  // Concentric nested polygons: area_out (64) != area_in (136).
  auto nested = Read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2)))");
  EXPECT_THROW(CoverageUnion::Union(nested.get()), TopologyException);
}

}  // namespace
}  // namespace geos_google
