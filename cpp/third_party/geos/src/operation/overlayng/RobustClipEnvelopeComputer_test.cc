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
// third_party/geos/src/operation/overlayng/RobustClipEnvelopeComputer.cpp.

#include "third_party/geos/include/geos/operation/overlayng/RobustClipEnvelopeComputer.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::operation::overlayng::RobustClipEnvelopeComputer;

TEST(RobustClipEnvelopeComputerTest, DirectConstructionReturnsTargetEnvelope) {
  const Envelope target_env(1.0, 5.0, 2.0, 8.0);
  RobustClipEnvelopeComputer cec(&target_env);
  EXPECT_EQ(cec.getEnvelope(), target_env);

  const Envelope null_env;
  RobustClipEnvelopeComputer null_cec(&null_env);
  EXPECT_TRUE(null_cec.getEnvelope().isNull());
}

TEST(RobustClipEnvelopeComputerTest,
     NullAndEmptyInputsReturnTargetEnvelopeUnchanged) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  const Envelope target_env(0.0, 10.0, 0.0, 10.0);

  EXPECT_EQ(
      RobustClipEnvelopeComputer::getEnvelope(nullptr, nullptr, &target_env),
      target_env);

  for (const std::string_view empty_wkt :
       {"POLYGON EMPTY", "MULTIPOLYGON EMPTY", "GEOMETRYCOLLECTION EMPTY",
        "LINESTRING EMPTY", "POINT EMPTY"}) {
    SCOPED_TRACE(empty_wkt);
    Geometry::Ptr empty_geom = reader.read(std::string(empty_wkt));
    ASSERT_NE(empty_geom, nullptr);
    EXPECT_EQ(RobustClipEnvelopeComputer::getEnvelope(empty_geom.get(), nullptr,
                                                      &target_env),
              target_env);
    EXPECT_EQ(RobustClipEnvelopeComputer::getEnvelope(nullptr, empty_geom.get(),
                                                      &target_env),
              target_env);
    EXPECT_EQ(RobustClipEnvelopeComputer::getEnvelope(
                  empty_geom.get(), empty_geom.get(), &target_env),
              target_env);
  }

  // Only one non-null polygonal operand contributes when the other is nullptr.
  Geometry::Ptr crossing_poly =
      reader.read("POLYGON ((-5 2, 5 2, 5 4, -5 4, -5 2))");
  const Envelope expected(-5.0, 10.0, 0.0, 10.0);
  EXPECT_EQ(RobustClipEnvelopeComputer::getEnvelope(crossing_poly.get(),
                                                    nullptr, &target_env),
            expected);
  EXPECT_EQ(RobustClipEnvelopeComputer::getEnvelope(
                nullptr, crossing_poly.get(), &target_env),
            expected);
}

TEST(RobustClipEnvelopeComputerTest,
     NonPolygonalAtomicAndCollectionGeometriesAreIgnored) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  const Envelope target_env(0.0, 10.0, 0.0, 10.0);

  Geometry::Ptr pt = reader.read("POINT (50 50)");
  Geometry::Ptr line = reader.read("LINESTRING (-50 5, 50 5)");
  Geometry::Ptr multi_pt = reader.read("MULTIPOINT ((5 5), (50 50))");
  Geometry::Ptr multi_line =
      reader.read("MULTILINESTRING ((-50 5, 50 5), (5 -50, 5 50))");

  CoordinateSequence ring_seq(
      {CoordinateXY{-50.0, 0.0}, CoordinateXY{50.0, 0.0},
       CoordinateXY{50.0, 50.0}, CoordinateXY{-50.0, 50.0},
       CoordinateXY{-50.0, 0.0}});
  std::unique_ptr<LinearRing> ring = factory->createLinearRing(ring_seq);

  EXPECT_EQ(RobustClipEnvelopeComputer::getEnvelope(pt.get(), line.get(),
                                                    &target_env),
            target_env);
  EXPECT_EQ(RobustClipEnvelopeComputer::getEnvelope(
                multi_pt.get(), multi_line.get(), &target_env),
            target_env);
  EXPECT_EQ(
      RobustClipEnvelopeComputer::getEnvelope(ring.get(), nullptr, &target_env),
      target_env);
}

TEST(RobustClipEnvelopeComputerTest,
     SinglePolygonExpandsOnlyForSegmentsIntersectingTarget) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  const Envelope target_env(0.0, 10.0, 0.0, 10.0);

  // Segments (-20, 5)->(2, 5) and (2, 5)->(2, 15) intersect target_env, while
  // the upper-right protrusion out to (25, 25) and (-20, 25) stays disjoint.
  Geometry::Ptr l_poly =
      reader.read("POLYGON ((-20 5, 2 5, 2 15, 25 15, 25 25, -20 25, -20 5))");
  ASSERT_TRUE(l_poly->isValid());

  const Envelope actual = RobustClipEnvelopeComputer::getEnvelope(
      l_poly.get(), nullptr, &target_env);
  EXPECT_EQ(actual, Envelope(-20.0, 10.0, 0.0, 15.0));
}

TEST(RobustClipEnvelopeComputerTest,
     SegmentCrossingOutsideTargetGeometryUsesSegmentEnvelopeIntersection) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  const Envelope target_env(0.0, 10.0, 0.0, 10.0);

  // Segment (-1, 20)->(20, 9) passes strictly above (10, 10), yet its segment
  // bounding box [-1, 20] x [9, 20] intersects [0, 10] x [0, 10].
  Geometry::Ptr triangle = reader.read("POLYGON ((-1 20, 20 9, 20 20, -1 20))");
  const Envelope actual = RobustClipEnvelopeComputer::getEnvelope(
      triangle.get(), nullptr, &target_env);
  EXPECT_EQ(actual, Envelope(-1.0, 20.0, 0.0, 20.0));
}

TEST(RobustClipEnvelopeComputerTest,
     PolygonEnclosingTargetWithDisjointEdgesLeavesTargetUnchanged) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  const Envelope target_env(2.0, 8.0, 2.0, 8.0);

  Geometry::Ptr enclosing =
      reader.read("POLYGON ((-50 -50, 50 -50, 50 50, -50 50, -50 -50))");
  const Envelope actual = RobustClipEnvelopeComputer::getEnvelope(
      enclosing.get(), nullptr, &target_env);
  EXPECT_EQ(actual, target_env);
}

TEST(RobustClipEnvelopeComputerTest,
     PolygonHolesExpandClipEnvelopeAndEmptyHolesAreSkipped) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  {
    const Envelope target_env(0.0, 10.0, 0.0, 10.0);
    // Outer shell boundary is completely disjoint from target_env. Hole 1 has
    // horizontal edges crossing target_env from x = -30 to x = 30; Hole 2 is
    // completely disjoint in [40, 50] x [40, 50].
    Geometry::Ptr poly_with_holes = reader.read(
        "POLYGON ((-100 -100, 100 -100, 100 100, -100 100, -100 -100), "
        "(-30 4, 30 4, 30 6, -30 6, -30 4), "
        "(40 40, 50 40, 50 50, 40 50, 40 40))");
    ASSERT_TRUE(poly_with_holes->isValid());

    const Envelope actual = RobustClipEnvelopeComputer::getEnvelope(
        poly_with_holes.get(), nullptr, &target_env);
    EXPECT_EQ(actual, Envelope(-30.0, 30.0, 0.0, 10.0));
  }

  // Construct a polygon containing an empty interior ring alongside a regular
  // interior ring to exercise the empty-ring guard in addPolygonRing.
  CoordinateSequence shell_seq({CoordinateXY{0.0, 0.0}, CoordinateXY{20.0, 0.0},
                                CoordinateXY{20.0, 20.0},
                                CoordinateXY{0.0, 20.0},
                                CoordinateXY{0.0, 0.0}});
  CoordinateSequence hole_seq({CoordinateXY{2.0, 2.0}, CoordinateXY{12.0, 2.0},
                               CoordinateXY{12.0, 4.0}, CoordinateXY{2.0, 4.0},
                               CoordinateXY{2.0, 2.0}});
  std::unique_ptr<LinearRing> shell = factory->createLinearRing(shell_seq);
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(factory->createLinearRing());
  holes.push_back(factory->createLinearRing(hole_seq));
  std::unique_ptr<Polygon> poly_empty_hole =
      factory->createPolygon(std::move(shell), std::move(holes));
  ASSERT_FALSE(poly_empty_hole->isEmpty());
  ASSERT_EQ(poly_empty_hole->getNumInteriorRing(), 2u);
  ASSERT_TRUE(poly_empty_hole->getInteriorRingN(0)->isEmpty());

  const Envelope inner_target(3.0, 5.0, 3.0, 5.0);
  const Envelope actual = RobustClipEnvelopeComputer::getEnvelope(
      poly_empty_hole.get(), nullptr, &inner_target);
  EXPECT_EQ(actual, Envelope(2.0, 12.0, 3.0, 5.0));
}

TEST(RobustClipEnvelopeComputerTest,
     MultiPolygonsNestedCollectionsAndBothOperandsCombine) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  const Envelope target_env(0.0, 10.0, 0.0, 10.0);

  // Nested GeometryCollection mixing an empty polygon, non-polygonal shapes,
  // and a polygon extending to x = -15.
  Geometry::Ptr geom_a = reader.read(
      "GEOMETRYCOLLECTION ("
      "POLYGON EMPTY, "
      "POINT (100 100), "
      "LINESTRING (-100 5, 100 5), "
      "GEOMETRYCOLLECTION (POLYGON ((-15 2, 5 2, 5 4, -15 4, -15 2))))");

  // MultiPolygon with one disjoint polygon and one intersecting polygon
  // extending to x = 25, y = 12.
  Geometry::Ptr geom_b = reader.read(
      "MULTIPOLYGON ("
      "((200 200, 210 200, 210 210, 200 210, 200 200)), "
      "((8 8, 25 8, 25 12, 8 12, 8 8)))");

  const Envelope actual = RobustClipEnvelopeComputer::getEnvelope(
      geom_a.get(), geom_b.get(), &target_env);
  EXPECT_EQ(actual, Envelope(-15.0, 25.0, 0.0, 12.0));
}

TEST(RobustClipEnvelopeComputerTest,
     BoundaryTouchPointTargetNullTargetAndZCoordinates) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Corner touch at (0, 0) intersects [0, 10] x [0, 10] via both incident
  // edges.
  const Envelope target_env(0.0, 10.0, 0.0, 10.0);
  Geometry::Ptr corner_touch =
      reader.read("POLYGON ((-5 0, 0 0, 0 -5, -5 -5, -5 0))");
  EXPECT_EQ(RobustClipEnvelopeComputer::getEnvelope(corner_touch.get(), nullptr,
                                                    &target_env),
            Envelope(-5.0, 10.0, -5.0, 10.0));

  // Point-sized target envelope [5, 5] x [5, 5].
  const Envelope point_env(5.0, 5.0, 5.0, 5.0);
  Geometry::Ptr edge_on_point =
      reader.read("POLYGON ((-2 5, 12 5, 12 15, -2 15, -2 5))");
  EXPECT_EQ(RobustClipEnvelopeComputer::getEnvelope(edge_on_point.get(),
                                                    nullptr, &point_env),
            Envelope(-2.0, 12.0, 5.0, 5.0));

  // Null target envelope never intersects any segment and remains null.
  const Envelope null_env;
  EXPECT_TRUE(RobustClipEnvelopeComputer::getEnvelope(edge_on_point.get(),
                                                      nullptr, &null_env)
                  .isNull());

  // 3D polygon computes the same 2D clip envelope.
  Geometry::Ptr poly_z =
      reader.read("POLYGON Z ((-5 5 10, 5 5 20, 5 15 30, -5 15 40, -5 5 10))");
  EXPECT_EQ(RobustClipEnvelopeComputer::getEnvelope(poly_z.get(), nullptr,
                                                    &target_env),
            Envelope(-5.0, 10.0, 0.0, 15.0));
}

}  // namespace
}  // namespace geos_google
