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
// third_party/geos/src/operation/valid/IndexedNestedHoleTester.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/valid/IndexedNestedHoleTester.cpp

#include "third_party/geos/include/geos/operation/valid/IndexedNestedHoleTester.h"

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
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::operation::valid::IndexedNestedHoleTester;

std::unique_ptr<Polygon> ReadPolygon(const GeometryFactory& factory,
                                     std::string_view wkt) {
  WKTReader reader(factory);
  return reader.read<Polygon>(std::string(wkt));
}

std::unique_ptr<LinearRing> MakeRing(
    const GeometryFactory& factory,
    std::initializer_list<CoordinateXY> coords) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  return factory.createLinearRing(std::move(seq));
}

TEST(IndexedNestedHoleTesterTest, EmptyPolygonAndNoHoles) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  for (std::string_view wkt : {
           "POLYGON EMPTY",
           "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
       }) {
    SCOPED_TRACE(wkt);
    std::unique_ptr<Polygon> poly = ReadPolygon(*factory, wkt);
    IndexedNestedHoleTester tester(poly.get());
    EXPECT_FALSE(tester.isNested());
    EXPECT_EQ(tester.getNestedPoint(), CoordinateXY(0.0, 0.0));
  }
}

TEST(IndexedNestedHoleTesterTest, SingleHoleIsNotNested) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> poly =
      ReadPolygon(*factory,
                  "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
                  "(2 2, 8 2, 8 8, 2 8, 2 2))");

  IndexedNestedHoleTester tester(poly.get());
  EXPECT_FALSE(tester.isNested());
  EXPECT_FALSE(tester.isNested());
}

TEST(IndexedNestedHoleTesterTest,
     EmptyInteriorRingAlongsideValidAndNestedHoles) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  {
    auto shell =
        MakeRing(*factory, {{0, 0}, {20, 0}, {20, 20}, {0, 20}, {0, 0}});
    std::vector<std::unique_ptr<LinearRing>> holes;
    holes.push_back(factory->createLinearRing());
    holes.push_back(
        MakeRing(*factory, {{2, 2}, {8, 2}, {8, 8}, {2, 8}, {2, 2}}));
    std::unique_ptr<Polygon> poly =
        factory->createPolygon(std::move(shell), std::move(holes));

    IndexedNestedHoleTester tester(poly.get());
    EXPECT_FALSE(tester.isNested());
  }

  {
    auto shell =
        MakeRing(*factory, {{0, 0}, {20, 0}, {20, 20}, {0, 20}, {0, 0}});
    std::vector<std::unique_ptr<LinearRing>> holes;
    holes.push_back(factory->createLinearRing());
    holes.push_back(
        MakeRing(*factory, {{2, 2}, {18, 2}, {18, 18}, {2, 18}, {2, 2}}));
    holes.push_back(
        MakeRing(*factory, {{5, 5}, {10, 5}, {10, 10}, {5, 10}, {5, 5}}));
    std::unique_ptr<Polygon> poly =
        factory->createPolygon(std::move(shell), std::move(holes));

    IndexedNestedHoleTester tester(poly.get());
    EXPECT_TRUE(tester.isNested());
    EXPECT_EQ(tester.getNestedPoint(), CoordinateXY(5.0, 5.0));
  }
}

TEST(IndexedNestedHoleTesterTest,
     DisjointHolesWithSeparatedAndOverlappingBoxes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Separated hole bounding boxes.
  std::unique_ptr<Polygon> separated =
      ReadPolygon(*factory,
                  "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
                  "(1 1, 4 1, 4 4, 1 4, 1 1), "
                  "(6 6, 9 6, 9 9, 6 9, 6 6))");
  {
    IndexedNestedHoleTester tester(separated.get());
    EXPECT_FALSE(tester.isNested());
  }

  // Disjoint L-shaped hole and square hole whose envelopes intersect on
  // [4, 8] x [4, 8] without either covering the other.
  std::unique_ptr<Polygon> overlapping_boxes =
      ReadPolygon(*factory,
                  "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
                  "(1 1, 8 1, 8 3, 3 3, 3 8, 1 8, 1 1), "
                  "(4 4, 9 4, 9 9, 4 9, 4 4))");
  {
    IndexedNestedHoleTester tester(overlapping_boxes.get());
    EXPECT_FALSE(tester.isNested());
  }

  // Two valid disjoint holes touching at a single point.
  std::unique_ptr<Polygon> touching_holes =
      ReadPolygon(*factory,
                  "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
                  "(1 1, 5 1, 5 5, 1 5, 1 1), "
                  "(5 5, 8 5, 8 8, 5 8, 5 5))");
  {
    IndexedNestedHoleTester tester(touching_holes.get());
    EXPECT_FALSE(tester.isNested());
  }
}

TEST(IndexedNestedHoleTesterTest, HoleInConcaveBayCoveredEnvelopeNotNested) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // C-shaped hole envelope [1, 15] x [1, 17] covers the second hole's envelope
  // [6, 12] x [6, 12], yet the second hole lies strictly in the bay outside it.
  std::unique_ptr<Polygon> interior_bay =
      ReadPolygon(*factory,
                  "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
                  "(1 1, 15 1, 15 3, 4 3, 4 15, 15 15, 15 17, 1 17, 1 1), "
                  "(6 6, 12 6, 12 12, 6 12, 6 6))");
  {
    IndexedNestedHoleTester tester(interior_bay.get());
    EXPECT_FALSE(tester.isNested());
  }

  // Second hole vertex 0 touches the C-shaped hole boundary at (4, 6) while its
  // incident edges extend into the exterior concave bay.
  std::unique_ptr<Polygon> boundary_touching_bay =
      ReadPolygon(*factory,
                  "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
                  "(1 1, 15 1, 15 3, 4 3, 4 15, 15 15, 15 17, 1 17, 1 1), "
                  "(4 6, 10 4, 10 12, 4 6))");
  {
    IndexedNestedHoleTester tester(boundary_touching_bay.get());
    EXPECT_FALSE(tester.isNested());
  }
}

TEST(IndexedNestedHoleTesterTest, NestedHoleStrictlyInsideBothInputOrders) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<Polygon> outer_first =
      ReadPolygon(*factory,
                  "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
                  "(2 2, 18 2, 18 18, 2 18, 2 2), "
                  "(5 5, 10 5, 10 10, 5 10, 5 5))");
  {
    IndexedNestedHoleTester tester(outer_first.get());
    EXPECT_TRUE(tester.isNested());
    EXPECT_EQ(tester.getNestedPoint(), CoordinateXY(5.0, 5.0));
    EXPECT_TRUE(tester.isNested());
    EXPECT_EQ(tester.getNestedPoint(), CoordinateXY(5.0, 5.0));
  }

  std::unique_ptr<Polygon> inner_first =
      ReadPolygon(*factory,
                  "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
                  "(5 5, 10 5, 10 10, 5 10, 5 5), "
                  "(2 2, 18 2, 18 18, 2 18, 2 2))");
  {
    IndexedNestedHoleTester tester(inner_first.get());
    EXPECT_TRUE(tester.isNested());
    EXPECT_EQ(tester.getNestedPoint(), CoordinateXY(5.0, 5.0));
  }
}

TEST(IndexedNestedHoleTesterTest,
     NestedHoleTouchingOuterHoleBoundaryAtVertex0) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Inner hole vertex 0 touches the outer hole edge at (2, 10) while the
  // remaining vertices lie inside the outer hole.
  std::unique_ptr<Polygon> touch_at_edge_pt =
      ReadPolygon(*factory,
                  "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
                  "(2 2, 18 2, 18 18, 2 18, 2 2), "
                  "(2 10, 10 4, 10 14, 2 10))");
  {
    IndexedNestedHoleTester tester(touch_at_edge_pt.get());
    EXPECT_TRUE(tester.isNested());
    EXPECT_EQ(tester.getNestedPoint(), CoordinateXY(2.0, 10.0));
  }

  // Repeated starting vertex on the outer hole boundary exercises skipping
  // duplicate vertices in PolygonTopologyAnalyzer::isRingNested.
  auto shell = MakeRing(*factory, {{0, 0}, {20, 0}, {20, 20}, {0, 20}, {0, 0}});
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(
      MakeRing(*factory, {{2, 2}, {18, 2}, {18, 18}, {2, 18}, {2, 2}}));
  holes.push_back(
      MakeRing(*factory, {{2, 10}, {2, 10}, {10, 4}, {10, 14}, {2, 10}}));
  std::unique_ptr<Polygon> repeated_start =
      factory->createPolygon(std::move(shell), std::move(holes));
  {
    IndexedNestedHoleTester tester(repeated_start.get());
    EXPECT_TRUE(tester.isNested());
    EXPECT_EQ(tester.getNestedPoint(), CoordinateXY(2.0, 10.0));
  }
}

TEST(IndexedNestedHoleTesterTest, ManyHolesMultiLevelSTRtreeAndNestedChain) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto make_grid_polygon = [&](bool add_nested) {
    auto shell =
        MakeRing(*factory, {{0, 0}, {100, 0}, {100, 100}, {0, 100}, {0, 0}});
    std::vector<std::unique_ptr<LinearRing>> holes;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        const double x0 = 10.0 * c + 2.0;
        const double y0 = 10.0 * r + 2.0;
        holes.push_back(MakeRing(*factory, {{x0, y0},
                                            {x0 + 6.0, y0},
                                            {x0 + 6.0, y0 + 6.0},
                                            {x0, y0 + 6.0},
                                            {x0, y0}}));
      }
    }
    if (add_nested) {
      // Nest inside grid cell (r=2, c=2): [22, 28] x [22, 28].
      holes.push_back(MakeRing(
          *factory, {{23, 23}, {27, 23}, {27, 27}, {23, 27}, {23, 23}}));
    }
    return factory->createPolygon(std::move(shell), std::move(holes));
  };

  {
    std::unique_ptr<Polygon> valid_many =
        make_grid_polygon(/*add_nested=*/false);
    IndexedNestedHoleTester tester(valid_many.get());
    EXPECT_FALSE(tester.isNested());
  }

  {
    std::unique_ptr<Polygon> invalid_many =
        make_grid_polygon(/*add_nested=*/true);
    IndexedNestedHoleTester tester(invalid_many.get());
    EXPECT_TRUE(tester.isNested());
    EXPECT_EQ(tester.getNestedPoint(), CoordinateXY(23.0, 23.0));
  }

  // Three mutually nested concentric holes.
  std::unique_ptr<Polygon> chain =
      ReadPolygon(*factory,
                  "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
                  "(2 2, 18 2, 18 18, 2 18, 2 2), "
                  "(4 4, 16 4, 16 16, 4 16, 4 4), "
                  "(6 6, 14 6, 14 14, 6 14, 6 6))");
  {
    IndexedNestedHoleTester tester(chain.get());
    EXPECT_TRUE(tester.isNested());
  }
}

TEST(IndexedNestedHoleTesterTest, OrientationsAndZCoordinates) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // CW outer hole with CCW inner hole and Z ordinates.
  std::unique_ptr<Polygon> poly_z =
      ReadPolygon(*factory,
                  "POLYGON Z ((0 0 1, 20 0 1, 20 20 1, 0 20 1, 0 0 1), "
                  "(2 2 5, 2 18 5, 18 18 5, 18 2 5, 2 2 5), "
                  "(5 5 9, 10 5 9, 10 10 9, 5 10 9, 5 5 9))");
  {
    IndexedNestedHoleTester tester(poly_z.get());
    EXPECT_TRUE(tester.isNested());
    EXPECT_EQ(tester.getNestedPoint(), CoordinateXY(5.0, 5.0));
  }

  // CCW outer hole with CW inner hole.
  std::unique_ptr<Polygon> ccw_outer_cw_inner =
      ReadPolygon(*factory,
                  "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
                  "(2 2, 18 2, 18 18, 2 18, 2 2), "
                  "(6 6, 6 12, 12 12, 12 6, 6 6))");
  {
    IndexedNestedHoleTester tester(ccw_outer_cw_inner.get());
    EXPECT_TRUE(tester.isNested());
    EXPECT_EQ(tester.getNestedPoint(), CoordinateXY(6.0, 6.0));
  }
}

}  // namespace
}  // namespace geos_google
