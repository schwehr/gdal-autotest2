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
// third_party/geos/src/operation/valid/IndexedNestedPolygonTester.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/valid/IndexedNestedPolygonTester.cpp

#include "third_party/geos/include/geos/operation/valid/IndexedNestedPolygonTester.h"

#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::MultiPolygon;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::operation::valid::IndexedNestedPolygonTester;

std::unique_ptr<MultiPolygon> ReadMultiPolygon(const GeometryFactory& factory,
                                               std::string_view wkt) {
  WKTReader reader(factory);
  return reader.read<MultiPolygon>(std::string(wkt));
}

std::unique_ptr<Polygon> MakeSquarePolygon(const GeometryFactory& factory,
                                           double x0, double y0, double size) {
  auto seq =
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {x0, y0},
          {x0 + size, y0},
          {x0 + size, y0 + size},
          {x0, y0 + size},
          {x0, y0},
      });
  std::unique_ptr<LinearRing> ring = factory.createLinearRing(std::move(seq));
  return factory.createPolygon(std::move(ring));
}

TEST(IndexedNestedPolygonTesterTest, EmptyAndSinglePolygon) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  for (std::string_view wkt : {
           "MULTIPOLYGON EMPTY",
           "MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0)))",
       }) {
    SCOPED_TRACE(wkt);
    std::unique_ptr<MultiPolygon> mp = ReadMultiPolygon(*factory, wkt);
    IndexedNestedPolygonTester tester(mp.get());
    EXPECT_TRUE(tester.getNestedPoint().isNull());
    EXPECT_FALSE(tester.isNested());
    EXPECT_TRUE(tester.getNestedPoint().isNull());
  }
}

TEST(IndexedNestedPolygonTesterTest, MultiPolygonWithEmptyComponentPolygon) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Empty polygon component alongside one non-empty polygon is not nested.
  {
    std::vector<std::unique_ptr<Polygon>> polys;
    polys.push_back(factory->createPolygon());
    polys.push_back(MakeSquarePolygon(*factory, 0.0, 0.0, 10.0));
    std::unique_ptr<MultiPolygon> mp =
        factory->createMultiPolygon(std::move(polys));

    IndexedNestedPolygonTester tester(mp.get());
    EXPECT_FALSE(tester.isNested());
    EXPECT_TRUE(tester.getNestedPoint().isNull());
  }

  // Empty polygon component alongside two nested non-empty polygons.
  {
    std::unique_ptr<MultiPolygon> mp =
        ReadMultiPolygon(*factory,
                         "MULTIPOLYGON (EMPTY, "
                         "((0 0, 20 0, 20 20, 0 20, 0 0)), "
                         "((3 3, 8 3, 8 8, 3 8, 3 3)))");
    IndexedNestedPolygonTester tester(mp.get());
    EXPECT_TRUE(tester.isNested());
    EXPECT_EQ(tester.getNestedPoint(), CoordinateXY(3.0, 3.0));
  }
}

TEST(IndexedNestedPolygonTesterTest,
     DisjointPolygonsAndOverlappingEnvelopesWithoutCover) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Disjoint separated polygons.
  {
    std::unique_ptr<MultiPolygon> mp =
        ReadMultiPolygon(*factory,
                         "MULTIPOLYGON ("
                         "((0 0, 2 0, 2 2, 0 2, 0 0)), "
                         "((5 5, 7 5, 7 7, 5 7, 5 5)))");
    IndexedNestedPolygonTester tester(mp.get());
    EXPECT_FALSE(tester.isNested());
    EXPECT_TRUE(tester.getNestedPoint().isNull());
  }

  // L-shaped polygon [0, 8] x [0, 8] and square [4, 10] x [4, 10] have
  // intersecting envelopes without either envelope covering the other.
  {
    std::unique_ptr<MultiPolygon> mp =
        ReadMultiPolygon(*factory,
                         "MULTIPOLYGON ("
                         "((0 0, 8 0, 8 2, 2 2, 2 8, 0 8, 0 0)), "
                         "((4 4, 10 4, 10 10, 4 10, 4 4)))");
    IndexedNestedPolygonTester tester(mp.get());
    EXPECT_FALSE(tester.isNested());
    EXPECT_TRUE(tester.getNestedPoint().isNull());
  }
}

TEST(IndexedNestedPolygonTesterTest, Vertex0InteriorBothComponentOrders) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Outer polygon listed first, inner polygon strictly inside.
  {
    std::unique_ptr<MultiPolygon> mp =
        ReadMultiPolygon(*factory,
                         "MULTIPOLYGON ("
                         "((0 0, 20 0, 20 20, 0 20, 0 0)), "
                         "((3 3, 8 3, 8 8, 3 8, 3 3)))");
    IndexedNestedPolygonTester tester(mp.get());
    EXPECT_TRUE(tester.isNested());
    EXPECT_EQ(tester.getNestedPoint(), CoordinateXY(3.0, 3.0));
  }

  // Inner polygon listed first, outer polygon second.
  {
    std::unique_ptr<MultiPolygon> mp =
        ReadMultiPolygon(*factory,
                         "MULTIPOLYGON ("
                         "((3 3, 8 3, 8 8, 3 8, 3 3)), "
                         "((0 0, 20 0, 20 20, 0 20, 0 0)))");
    IndexedNestedPolygonTester tester(mp.get());
    EXPECT_TRUE(tester.isNested());
    EXPECT_EQ(tester.getNestedPoint(), CoordinateXY(3.0, 3.0));
  }
}

TEST(IndexedNestedPolygonTesterTest, Vertex0ExteriorInHoleOrConcaveBay) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Island strictly inside a hole of the outer polygon.
  {
    std::unique_ptr<MultiPolygon> mp = ReadMultiPolygon(
        *factory,
        "MULTIPOLYGON ("
        "((0 0, 20 0, 20 20, 0 20, 0 0), (2 2, 18 2, 18 18, 2 18, 2 2)), "
        "((5 5, 10 5, 10 10, 5 10, 5 5)))");
    IndexedNestedPolygonTester tester(mp.get());
    EXPECT_FALSE(tester.isNested());
    EXPECT_TRUE(tester.getNestedPoint().isNull());
  }

  // Second polygon strictly inside the concave bay of a C-shaped polygon.
  {
    std::unique_ptr<MultiPolygon> mp = ReadMultiPolygon(
        *factory,
        "MULTIPOLYGON ("
        "((0 0, 20 0, 20 4, 6 4, 6 16, 20 16, 20 20, 0 20, 0 0)), "
        "((8 6, 16 6, 16 14, 8 14, 8 6)))");
    IndexedNestedPolygonTester tester(mp.get());
    EXPECT_FALSE(tester.isNested());
    EXPECT_TRUE(tester.getNestedPoint().isNull());
  }
}

TEST(IndexedNestedPolygonTesterTest, Vertex0OnBoundaryVertex1Exterior) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Island inside a hole touching the hole boundary at vertex 0 only, with
  // vertex 1 strictly inside the hole (exterior to the outer polygon).
  {
    std::unique_ptr<MultiPolygon> mp = ReadMultiPolygon(
        *factory,
        "MULTIPOLYGON ("
        "((0 0, 20 0, 20 20, 0 20, 0 0), (2 2, 18 2, 18 18, 2 18, 2 2)), "
        "((2 10, 8 6, 8 14, 2 10)))");
    IndexedNestedPolygonTester tester(mp.get());
    EXPECT_FALSE(tester.isNested());
    EXPECT_TRUE(tester.getNestedPoint().isNull());
  }

  // Polygon in a C-shaped bay touching the bay wall at vertex 0 only.
  {
    std::unique_ptr<MultiPolygon> mp = ReadMultiPolygon(
        *factory,
        "MULTIPOLYGON ("
        "((0 0, 20 0, 20 4, 6 4, 6 16, 20 16, 20 20, 0 20, 0 0)), "
        "((6 10, 12 6, 12 14, 6 10)))");
    IndexedNestedPolygonTester tester(mp.get());
    EXPECT_FALSE(tester.isNested());
    EXPECT_TRUE(tester.getNestedPoint().isNull());
  }
}

TEST(IndexedNestedPolygonTesterTest, Vertex0OnBoundaryVertex1Interior) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Vertex 0 lies on the outer shell boundary while vertex 1 lies in the
  // interior; nestedPt must report vertex 1.
  {
    std::unique_ptr<MultiPolygon> mp =
        ReadMultiPolygon(*factory,
                         "MULTIPOLYGON ("
                         "((0 0, 20 0, 20 20, 0 20, 0 0)), "
                         "((0 10, 8 6, 8 14, 0 10)))");
    IndexedNestedPolygonTester tester(mp.get());
    EXPECT_TRUE(tester.isNested());
    EXPECT_EQ(tester.getNestedPoint(), CoordinateXY(8.0, 6.0));
  }

  // Vertex 0 lies on a hole boundary while vertex 1 lies in the solid
  // interior between shell and hole.
  {
    std::unique_ptr<MultiPolygon> mp = ReadMultiPolygon(
        *factory,
        "MULTIPOLYGON ("
        "((0 0, 20 0, 20 20, 0 20, 0 0), (6 6, 14 6, 14 14, 6 14, 6 6)), "
        "((6 10, 2 6, 2 14, 6 10)))");
    IndexedNestedPolygonTester tester(mp.get());
    EXPECT_TRUE(tester.isNested());
    EXPECT_EQ(tester.getNestedPoint(), CoordinateXY(2.0, 6.0));
  }
}

TEST(IndexedNestedPolygonTesterTest,
     BothVerticesOnBoundaryExteriorConcaveBayNotNested) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Both vertex 0 (6, 10) and vertex 1 (12, 4) lie on the C-shaped shell's
  // boundary, while the incident segment (6, 10) -> (12, 4) traverses the
  // exterior concave bay.
  std::unique_ptr<MultiPolygon> mp = ReadMultiPolygon(
      *factory,
      "MULTIPOLYGON ("
      "((0 0, 20 0, 20 4, 6 4, 6 16, 20 16, 20 20, 0 20, 0 0)), "
      "((6 10, 12 4, 14 12, 6 10)))");

  IndexedNestedPolygonTester tester(mp.get());
  EXPECT_FALSE(tester.isNested());
  EXPECT_TRUE(tester.getNestedPoint().isNull());
}

TEST(IndexedNestedPolygonTesterTest,
     BothVerticesOnBoundaryInsideHoleWithMultipleHoles) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Outer polygon [0, 30] x [0, 30] has three holes:
  // - Hole 0 [1, 3] x [1, 3]: envelope does not cover inner [12, 22] x [8, 18].
  // - Hole 1 (C-shaped) [5, 25] x [2, 28]: envelope covers inner, not nested.
  // - Hole 2 [12, 24] x [8, 22]: envelope covers inner AND contains inner.
  // Inner triangle touches Hole 2 at both vertex 0 (12, 15) and vertex 1
  // (18, 8).
  std::unique_ptr<MultiPolygon> mp = ReadMultiPolygon(
      *factory,
      "MULTIPOLYGON ("
      "((0 0, 30 0, 30 30, 0 30, 0 0), "
      " (1 1, 3 1, 3 3, 1 3, 1 1), "
      " (5 2, 25 2, 25 5, 10 5, 10 25, 25 25, 25 28, 5 28, 5 2), "
      " (12 8, 24 8, 24 22, 12 22, 12 8)), "
      "((12 15, 18 8, 22 18, 12 15)))");

  IndexedNestedPolygonTester tester(mp.get());
  EXPECT_FALSE(tester.isNested());
  EXPECT_TRUE(tester.getNestedPoint().isNull());
}

TEST(IndexedNestedPolygonTesterTest,
     BothVerticesOnBoundaryNestedInSolidWithAndWithoutHoles) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Outer polygon has no holes; inner touches shell at vertex 0 (0, 10) and
  // vertex 1 (10, 0), with incident segment going into the solid interior.
  {
    std::unique_ptr<MultiPolygon> mp =
        ReadMultiPolygon(*factory,
                         "MULTIPOLYGON ("
                         "((0 0, 20 0, 20 20, 0 20, 0 0)), "
                         "((0 10, 10 0, 15 15, 0 10)))");
    IndexedNestedPolygonTester tester(mp.get());
    EXPECT_TRUE(tester.isNested());
    EXPECT_EQ(tester.getNestedPoint(), CoordinateXY(0.0, 10.0));
  }

  // Outer polygon has Hole 0 (non-covering envelope) and C-shaped Hole 1
  // (covering envelope); inner lies in Hole 1's concave bay (solid interior of
  // outer polygon) and touches Hole 1 at vertex 0 (10, 15) and vertex 1 (18,
  // 5).
  {
    std::unique_ptr<MultiPolygon> mp = ReadMultiPolygon(
        *factory,
        "MULTIPOLYGON ("
        "((0 0, 30 0, 30 30, 0 30, 0 0), "
        " (1 1, 3 1, 3 3, 1 3, 1 1), "
        " (5 2, 25 2, 25 5, 10 5, 10 25, 25 25, 25 28, 5 28, 5 2)), "
        "((10 15, 18 5, 22 20, 10 15)))");
    IndexedNestedPolygonTester tester(mp.get());
    EXPECT_TRUE(tester.isNested());
    EXPECT_EQ(tester.getNestedPoint(), CoordinateXY(10.0, 15.0));
  }
}

TEST(IndexedNestedPolygonTesterTest,
     LocatorCacheHitAcrossMultipleContainedPolygons) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Two valid islands in two separate holes share the same enclosing outer
  // polygon, exercising the locators map cache hit on the second query.
  {
    std::unique_ptr<MultiPolygon> mp =
        ReadMultiPolygon(*factory,
                         "MULTIPOLYGON ("
                         "((0 0, 30 0, 30 30, 0 30, 0 0), "
                         " (2 2, 12 2, 12 28, 2 28, 2 2), "
                         " (18 2, 28 2, 28 28, 18 28, 18 2)), "
                         "((4 4, 10 4, 10 10, 4 10, 4 4)), "
                         "((20 4, 26 4, 26 10, 20 10, 20 4)))");
    IndexedNestedPolygonTester tester(mp.get());
    EXPECT_FALSE(tester.isNested());
    EXPECT_TRUE(tester.getNestedPoint().isNull());
    // Second call exercises repeated cached lookups.
    EXPECT_FALSE(tester.isNested());
  }

  // Two valid islands plus a third polygon nested in the solid middle strip.
  {
    std::unique_ptr<MultiPolygon> mp =
        ReadMultiPolygon(*factory,
                         "MULTIPOLYGON ("
                         "((0 0, 30 0, 30 30, 0 30, 0 0), "
                         " (2 2, 12 2, 12 28, 2 28, 2 2), "
                         " (18 2, 28 2, 28 28, 18 28, 18 2)), "
                         "((4 4, 10 4, 10 10, 4 10, 4 4)), "
                         "((20 4, 26 4, 26 10, 20 10, 20 4)), "
                         "((14 14, 16 14, 16 16, 14 16, 14 14)))");
    IndexedNestedPolygonTester tester(mp.get());
    EXPECT_TRUE(tester.isNested());
    EXPECT_EQ(tester.getNestedPoint(), CoordinateXY(14.0, 14.0));
  }
}

TEST(IndexedNestedPolygonTesterTest,
     RepeatedStartVertexOrientationsZCoordinatesAndManyPolygons) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Repeated start vertex on boundary followed by a vertex on another edge.
  {
    std::vector<std::unique_ptr<Polygon>> polys;
    polys.push_back(MakeSquarePolygon(*factory, 0.0, 0.0, 20.0));
    auto inner_seq = std::make_unique<CoordinateSequence>(
        std::initializer_list<CoordinateXY>{
            {0, 10}, {0, 10}, {10, 0}, {15, 15}, {0, 10}});
    polys.push_back(factory->createPolygon(
        factory->createLinearRing(std::move(inner_seq))));
    std::unique_ptr<MultiPolygon> mp =
        factory->createMultiPolygon(std::move(polys));

    IndexedNestedPolygonTester tester(mp.get());
    EXPECT_TRUE(tester.isNested());
    EXPECT_EQ(tester.getNestedPoint(), CoordinateXY(0.0, 10.0));
  }

  // Opposite ring orientations (CW outer, CCW inner) with Z coordinates.
  {
    std::unique_ptr<MultiPolygon> mp =
        ReadMultiPolygon(*factory,
                         "MULTIPOLYGON Z ("
                         "((0 0 1, 0 20 1, 20 20 1, 20 0 1, 0 0 1)), "
                         "((0 10 2, 10 0 2, 15 15 2, 0 10 2)))");
    IndexedNestedPolygonTester tester(mp.get());
    EXPECT_TRUE(tester.isNested());
    EXPECT_EQ(tester.getNestedPoint(), CoordinateXY(0.0, 10.0));
  }

  // Many disjoint polygons (> default STRtree node capacity) and one nested.
  {
    std::vector<std::unique_ptr<Polygon>> disjoint_polys;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        disjoint_polys.push_back(
            MakeSquarePolygon(*factory, 10.0 * c, 10.0 * r, 5.0));
      }
    }
    std::unique_ptr<MultiPolygon> mp_valid =
        factory->createMultiPolygon(std::move(disjoint_polys));
    IndexedNestedPolygonTester valid_tester(mp_valid.get());
    EXPECT_FALSE(valid_tester.isNested());
    EXPECT_TRUE(valid_tester.getNestedPoint().isNull());
  }

  {
    std::vector<std::unique_ptr<Polygon>> polys;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        polys.push_back(MakeSquarePolygon(*factory, 10.0 * c, 10.0 * r, 8.0));
      }
    }
    polys.push_back(MakeSquarePolygon(*factory, 22.0, 22.0, 4.0));
    std::unique_ptr<MultiPolygon> mp_nested =
        factory->createMultiPolygon(std::move(polys));
    IndexedNestedPolygonTester nested_tester(mp_nested.get());
    EXPECT_TRUE(nested_tester.isNested());
    EXPECT_EQ(nested_tester.getNestedPoint(), CoordinateXY(22.0, 22.0));
  }
}

}  // namespace
}  // namespace geos_google
