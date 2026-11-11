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

// Unit tests for third_party/geos/src/util/GeometricShapeFactory.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - Dimensions::width and Dimensions::height are uninitialized by default in
//   upstream GeometricShapeFactory::Dimensions::Dimensions(), so callers must
//   set dimensions before calling create* methods (especially under MSan).
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/util/GeometricShapeFactory.cpp

#include "third_party/geos/include/geos/util/GeometricShapeFactory.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Envelope;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::util::GeometricShapeFactory;
using geos::util::IllegalArgumentException;

constexpr double kPi = std::numbers::pi;
constexpr double kTwoPi = 2.0 * kPi;

class ExposedShapeFactory : public GeometricShapeFactory {
 public:
  using GeometricShapeFactory::GeometricShapeFactory;

  CoordinateXY MakePreciseCoord(double x, double y) const {
    return coord(x, y);
  }

  std::unique_ptr<Envelope> GetDimensionsEnvelope() const {
    return dim.getEnvelope();
  }
};

TEST(GeometricShapeFactoryTest, DefaultOriginAndNumPointsForRectangle) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  GeometricShapeFactory shape(factory.get());
  shape.setSize(10.0);

  std::unique_ptr<Polygon> rect = shape.createRectangle();
  ASSERT_NE(rect, nullptr);
  EXPECT_TRUE(rect->isValid());
  EXPECT_EQ(rect->getNumInteriorRing(), 0u);
  // Default nPts = 100 -> nSide = 25 -> 4 * 25 + 1 = 101 points.
  EXPECT_EQ(rect->getNumPoints(), 101u);
  EXPECT_FALSE(rect->isRectangle());
  EXPECT_DOUBLE_EQ(rect->getArea(), 100.0);
  // Subdivided edges use step 10.0 / 25 = 0.4, accumulating ~1e-13 FP error.
  EXPECT_NEAR(rect->getLength(), 40.0, 1e-12);

  const Envelope* env = rect->getEnvelopeInternal();
  ASSERT_NE(env, nullptr);
  EXPECT_DOUBLE_EQ(env->getMinX(), 0.0);
  EXPECT_DOUBLE_EQ(env->getMaxX(), 10.0);
  EXPECT_DOUBLE_EQ(env->getMinY(), 0.0);
  EXPECT_DOUBLE_EQ(env->getMaxY(), 10.0);
}

TEST(GeometricShapeFactoryTest, RectangleWithBaseWidthHeightAndFourPoints) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  GeometricShapeFactory shape(factory.get());
  shape.setBase(CoordinateXY(1.0, 2.0));
  shape.setWidth(6.0);
  shape.setHeight(3.0);
  shape.setNumPoints(4);

  std::unique_ptr<Polygon> rect = shape.createRectangle();
  ASSERT_NE(rect, nullptr);
  EXPECT_TRUE(rect->isValid());
  EXPECT_TRUE(rect->isRectangle());
  EXPECT_EQ(rect->getNumPoints(), 5u);
  EXPECT_DOUBLE_EQ(rect->getArea(), 18.0);
  EXPECT_DOUBLE_EQ(rect->getLength(), 18.0);

  const CoordinateSequence* coords =
      rect->getExteriorRing()->getCoordinatesRO();
  ASSERT_EQ(coords->size(), 5u);
  EXPECT_TRUE(coords->getAt<CoordinateXY>(0).equals2D(CoordinateXY(1.0, 2.0)));
  EXPECT_TRUE(coords->getAt<CoordinateXY>(1).equals2D(CoordinateXY(7.0, 2.0)));
  EXPECT_TRUE(coords->getAt<CoordinateXY>(2).equals2D(CoordinateXY(7.0, 5.0)));
  EXPECT_TRUE(coords->getAt<CoordinateXY>(3).equals2D(CoordinateXY(1.0, 5.0)));
  EXPECT_TRUE(coords->getAt<CoordinateXY>(4).equals2D(CoordinateXY(1.0, 2.0)));
}

TEST(GeometricShapeFactoryTest,
     RectangleClampsSmallNumPointsAndSubdividesSides) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  for (uint32_t small_npts : {0u, 1u, 3u}) {
    SCOPED_TRACE(small_npts);
    GeometricShapeFactory shape(factory.get());
    shape.setBase(CoordinateXY(-2.0, -4.0));
    shape.setSize(4.0);
    shape.setNumPoints(small_npts);

    std::unique_ptr<Polygon> rect = shape.createRectangle();
    ASSERT_NE(rect, nullptr);
    EXPECT_TRUE(rect->isRectangle());
    EXPECT_EQ(rect->getNumPoints(), 5u);
    EXPECT_DOUBLE_EQ(rect->getArea(), 16.0);
  }

  // nPts = 10 -> integer division nSide = 10 / 4 = 2 -> 4 * 2 + 1 = 9 points.
  GeometricShapeFactory subdivided(factory.get());
  subdivided.setBase(CoordinateXY(1.0, 2.0));
  subdivided.setWidth(6.0);
  subdivided.setHeight(4.0);
  subdivided.setNumPoints(10);

  std::unique_ptr<Polygon> rect = subdivided.createRectangle();
  ASSERT_NE(rect, nullptr);
  EXPECT_TRUE(rect->isValid());
  EXPECT_FALSE(rect->isRectangle());
  EXPECT_EQ(rect->getNumPoints(), 9u);
  EXPECT_DOUBLE_EQ(rect->getArea(), 24.0);

  const CoordinateSequence* coords =
      rect->getExteriorRing()->getCoordinatesRO();
  constexpr auto kExpected = std::to_array<std::array<double, 2>>({
      {1.0, 2.0},
      {4.0, 2.0},
      {7.0, 2.0},
      {7.0, 4.0},
      {7.0, 6.0},
      {4.0, 6.0},
      {1.0, 6.0},
      {1.0, 4.0},
      {1.0, 2.0},
  });
  ASSERT_EQ(coords->size(), kExpected.size());
  for (std::size_t i = 0; i < kExpected.size(); ++i) {
    EXPECT_DOUBLE_EQ(coords->getX(i), kExpected[i][0]);
    EXPECT_DOUBLE_EQ(coords->getY(i), kExpected[i][1]);
  }
}

TEST(GeometricShapeFactoryTest,
     EnvelopePlacementPrecedenceBetweenBaseCentreAndOrigin) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  ExposedShapeFactory shape(factory.get());
  shape.setWidth(6.0);
  shape.setHeight(4.0);

  // Neither base nor centre set -> [0, width] x [0, height].
  std::unique_ptr<Envelope> env = shape.GetDimensionsEnvelope();
  EXPECT_EQ(*env, Envelope(0.0, 6.0, 0.0, 4.0));

  // Centre set -> [centre.x - width/2, centre.x + width/2] x [...].
  shape.setCentre(CoordinateXY(5.0, -3.0));
  env = shape.GetDimensionsEnvelope();
  EXPECT_EQ(*env, Envelope(2.0, 8.0, -5.0, -1.0));

  // Base takes precedence over centre when both are non-null.
  shape.setBase(CoordinateXY(10.0, 20.0));
  env = shape.GetDimensionsEnvelope();
  EXPECT_EQ(*env, Envelope(10.0, 16.0, 20.0, 24.0));

  // Clearing base falls back to centre.
  shape.setBase(CoordinateXY::getNull());
  env = shape.GetDimensionsEnvelope();
  EXPECT_EQ(*env, Envelope(2.0, 8.0, -5.0, -1.0));

  // Clearing centre falls back to origin.
  shape.setCentre(CoordinateXY::getNull());
  env = shape.GetDimensionsEnvelope();
  EXPECT_EQ(*env, Envelope(0.0, 6.0, 0.0, 4.0));
}

TEST(GeometricShapeFactoryTest, CircleDefaultsAndFourPointEllipseVertices) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Default 100-gon circle centered at origin with radius 10.
  GeometricShapeFactory circle_factory(factory.get());
  circle_factory.setCentre(CoordinateXY(0.0, 0.0));
  circle_factory.setSize(20.0);

  std::unique_ptr<Polygon> circle = circle_factory.createCircle();
  ASSERT_NE(circle, nullptr);
  EXPECT_TRUE(circle->isValid());
  EXPECT_EQ(circle->getNumPoints(), 101u);
  const double expected_area = 100.0 * 0.5 * 100.0 * std::sin(kTwoPi / 100.0);
  const double expected_perim = 100.0 * 2.0 * 10.0 * std::sin(kPi / 100.0);
  EXPECT_NEAR(circle->getArea(), expected_area, 1e-9);
  EXPECT_NEAR(circle->getLength(), expected_perim, 1e-9);
  EXPECT_EQ(*circle->getEnvelopeInternal(), Envelope(-10.0, 10.0, -10.0, 10.0));

  // 4-point ellipse -> diamond with exact snapped cardinal vertices.
  GeometricShapeFactory ellipse_factory(factory.get());
  ellipse_factory.setCentre(CoordinateXY(2.0, 3.0));
  ellipse_factory.setWidth(8.0);
  ellipse_factory.setHeight(6.0);
  ellipse_factory.setNumPoints(4);

  std::unique_ptr<Polygon> diamond = ellipse_factory.createCircle();
  ASSERT_NE(diamond, nullptr);
  EXPECT_TRUE(diamond->isValid());
  EXPECT_EQ(diamond->getNumPoints(), 5u);
  EXPECT_DOUBLE_EQ(diamond->getArea(), 24.0);

  const CoordinateSequence* coords =
      diamond->getExteriorRing()->getCoordinatesRO();
  constexpr auto kExpected = std::to_array<std::array<double, 2>>({
      {6.0, 3.0},
      {2.0, 6.0},
      {-2.0, 3.0},
      {2.0, 0.0},
      {6.0, 3.0},
  });
  ASSERT_EQ(coords->size(), kExpected.size());
  for (std::size_t i = 0; i < kExpected.size(); ++i) {
    EXPECT_DOUBLE_EQ(coords->getX(i), kExpected[i][0]);
    EXPECT_DOUBLE_EQ(coords->getY(i), kExpected[i][1]);
  }
}

TEST(GeometricShapeFactoryTest, CircleEdgeCasesForSmallNumPoints) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  GeometricShapeFactory shape(factory.get());
  shape.setCentre(CoordinateXY(0.0, 0.0));
  shape.setSize(10.0);

  shape.setNumPoints(0);
  EXPECT_THROW(shape.createCircle(), IllegalArgumentException);

  shape.setNumPoints(1);
  EXPECT_THROW(shape.createCircle(), IllegalArgumentException);

  shape.setNumPoints(2);
  std::unique_ptr<Polygon> collapsed = shape.createCircle();
  ASSERT_NE(collapsed, nullptr);
  EXPECT_EQ(collapsed->getNumPoints(), 3u);
  EXPECT_FALSE(collapsed->isValid());
  EXPECT_DOUBLE_EQ(collapsed->getArea(), 0.0);
}

TEST(GeometricShapeFactoryTest, ArcGeometryAndExtentClamping) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  GeometricShapeFactory shape(factory.get());
  shape.setCentre(CoordinateXY(1.0, 2.0));
  shape.setWidth(8.0);
  shape.setHeight(4.0);
  shape.setNumPoints(3);

  // Counter-clockwise half ellipse from pi/2 to 3*pi/2: (1,4) -> (-3,2) ->
  // (1,0).
  std::unique_ptr<LineString> arc = shape.createArc(kPi / 2.0, kPi);
  ASSERT_NE(arc, nullptr);
  EXPECT_EQ(arc->getNumPoints(), 3u);
  EXPECT_FALSE(arc->isClosed());
  EXPECT_DOUBLE_EQ(arc->getCoordinateN(0).x, 1.0);
  EXPECT_DOUBLE_EQ(arc->getCoordinateN(0).y, 4.0);
  EXPECT_DOUBLE_EQ(arc->getCoordinateN(1).x, -3.0);
  EXPECT_DOUBLE_EQ(arc->getCoordinateN(1).y, 2.0);
  EXPECT_DOUBLE_EQ(arc->getCoordinateN(2).x, 1.0);
  EXPECT_DOUBLE_EQ(arc->getCoordinateN(2).y, 0.0);

  // Minimal 2-point arc produces a chord between the start and end angles.
  shape.setNumPoints(2);
  std::unique_ptr<LineString> chord = shape.createArc(0.0, kPi);
  ASSERT_NE(chord, nullptr);
  EXPECT_EQ(chord->getNumPoints(), 2u);
  EXPECT_DOUBLE_EQ(chord->getCoordinateN(0).x, 5.0);
  EXPECT_DOUBLE_EQ(chord->getCoordinateN(0).y, 2.0);
  EXPECT_DOUBLE_EQ(chord->getCoordinateN(1).x, -3.0);
  EXPECT_DOUBLE_EQ(chord->getCoordinateN(1).y, 2.0);
  EXPECT_DOUBLE_EQ(chord->getLength(), 8.0);

  // Extent <= 0 or > 2*pi clamps to full 2*pi; exact 2*pi also forms full arc.
  struct ExtentCase {
    double ang_extent;
  };
  constexpr auto kFullCircleExtents = std::to_array<ExtentCase>({
      {.ang_extent = 0.0},
      {.ang_extent = -1.5},
      {.ang_extent = kTwoPi},
      {.ang_extent = kTwoPi + 1.0},
  });

  shape.setNumPoints(5);
  for (const auto& tc : kFullCircleExtents) {
    SCOPED_TRACE(tc.ang_extent);
    std::unique_ptr<LineString> full_arc = shape.createArc(0.0, tc.ang_extent);
    ASSERT_NE(full_arc, nullptr);
    EXPECT_EQ(full_arc->getNumPoints(), 5u);
    EXPECT_TRUE(full_arc->isClosed());
    EXPECT_DOUBLE_EQ(full_arc->getCoordinateN(0).x, 5.0);
    EXPECT_DOUBLE_EQ(full_arc->getCoordinateN(0).y, 2.0);
    EXPECT_DOUBLE_EQ(full_arc->getCoordinateN(1).x, 1.0);
    EXPECT_DOUBLE_EQ(full_arc->getCoordinateN(1).y, 4.0);
    EXPECT_DOUBLE_EQ(full_arc->getCoordinateN(2).x, -3.0);
    EXPECT_DOUBLE_EQ(full_arc->getCoordinateN(2).y, 2.0);
    EXPECT_DOUBLE_EQ(full_arc->getCoordinateN(3).x, 1.0);
    EXPECT_DOUBLE_EQ(full_arc->getCoordinateN(3).y, 0.0);
    EXPECT_DOUBLE_EQ(full_arc->getCoordinateN(4).x, 5.0);
    EXPECT_DOUBLE_EQ(full_arc->getCoordinateN(4).y, 2.0);
  }
}

TEST(GeometricShapeFactoryTest, ArcPolygonSectorsAndExtentClamping) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Minimal 2-point arc polygon forms a closed triangle with centre vertex.
  GeometricShapeFactory sector(factory.get());
  sector.setCentre(CoordinateXY(1.0, 2.0));
  sector.setSize(6.0);
  sector.setNumPoints(2);

  std::unique_ptr<Polygon> triangle_sector =
      sector.createArcPolygon(0.0, kPi / 2.0);
  ASSERT_NE(triangle_sector, nullptr);
  EXPECT_TRUE(triangle_sector->isValid());
  EXPECT_EQ(triangle_sector->getNumPoints(), 4u);
  EXPECT_DOUBLE_EQ(triangle_sector->getArea(), 4.5);

  const CoordinateSequence* tri_coords =
      triangle_sector->getExteriorRing()->getCoordinatesRO();
  EXPECT_TRUE(
      tri_coords->getAt<CoordinateXY>(0).equals2D(CoordinateXY(1.0, 2.0)));
  EXPECT_TRUE(
      tri_coords->getAt<CoordinateXY>(1).equals2D(CoordinateXY(4.0, 2.0)));
  EXPECT_TRUE(
      tri_coords->getAt<CoordinateXY>(2).equals2D(CoordinateXY(1.0, 5.0)));
  EXPECT_TRUE(
      tri_coords->getAt<CoordinateXY>(3).equals2D(CoordinateXY(1.0, 2.0)));

  // Elliptical half sector: vertices (0,0) -> (0,2) -> (-5,0) -> (0,-2) ->
  // (0,0).
  GeometricShapeFactory ellipse_sector(factory.get());
  ellipse_sector.setCentre(CoordinateXY(0.0, 0.0));
  ellipse_sector.setWidth(10.0);
  ellipse_sector.setHeight(4.0);
  ellipse_sector.setNumPoints(3);

  std::unique_ptr<Polygon> half_ellipse =
      ellipse_sector.createArcPolygon(kPi / 2.0, kPi);
  ASSERT_NE(half_ellipse, nullptr);
  EXPECT_TRUE(half_ellipse->isValid());
  EXPECT_EQ(half_ellipse->getNumPoints(), 5u);
  EXPECT_DOUBLE_EQ(half_ellipse->getArea(), 10.0);

  // High-resolution quarter circle sector matches analytical quarter-circle
  // area.
  ellipse_sector.setSize(20.0);
  ellipse_sector.setNumPoints(100);
  std::unique_ptr<Polygon> quarter_circle =
      ellipse_sector.createArcPolygon(0.0, kPi / 2.0);
  ASSERT_NE(quarter_circle, nullptr);
  EXPECT_TRUE(quarter_circle->isValid());
  EXPECT_EQ(quarter_circle->getNumPoints(), 102u);
  EXPECT_NEAR(quarter_circle->getArea(), 0.25 * kPi * 100.0, 0.05);
  EXPECT_NEAR(quarter_circle->getLength(), 20.0 + 0.5 * kPi * 10.0, 0.05);

  // Clamped extents (<= 0 and > 2*pi) produce nPts + 2 coordinates
  // starting/ending at centre.
  ellipse_sector.setNumPoints(5);
  for (double extent : {0.0, -0.5, kTwoPi + 0.5}) {
    SCOPED_TRACE(extent);
    std::unique_ptr<Polygon> full_sector =
        ellipse_sector.createArcPolygon(0.0, extent);
    ASSERT_NE(full_sector, nullptr);
    EXPECT_EQ(full_sector->getNumPoints(), 7u);
    const CoordinateSequence* seq =
        full_sector->getExteriorRing()->getCoordinatesRO();
    EXPECT_TRUE(seq->front<CoordinateXY>().equals2D(CoordinateXY(0.0, 0.0)));
    EXPECT_TRUE(seq->back<CoordinateXY>().equals2D(CoordinateXY(0.0, 0.0)));
    EXPECT_TRUE(seq->getAt<CoordinateXY>(1).equals2D(CoordinateXY(10.0, 0.0)));
    EXPECT_TRUE(seq->getAt<CoordinateXY>(2).equals2D(CoordinateXY(0.0, 10.0)));
    EXPECT_TRUE(seq->getAt<CoordinateXY>(3).equals2D(CoordinateXY(-10.0, 0.0)));
    EXPECT_TRUE(seq->getAt<CoordinateXY>(4).equals2D(CoordinateXY(0.0, -10.0)));
    EXPECT_TRUE(seq->getAt<CoordinateXY>(5).equals2D(CoordinateXY(10.0, 0.0)));
  }
}

TEST(GeometricShapeFactoryTest, AppliesFactoryPrecisionModelToGeneratedCoords) {
  PrecisionModel integer_pm(1.0);
  GeometryFactory::Ptr factory = GeometryFactory::create(&integer_pm);

  ExposedShapeFactory shape(factory.get());
  shape.setCentre(CoordinateXY(0.0, 0.0));
  shape.setSize(5.0);  // radius = 2.5
  shape.setNumPoints(4);

  // Asymmetric rounding (java_math_round): +2.5 -> +3, -2.5 -> -2.
  EXPECT_TRUE(
      shape.MakePreciseCoord(2.5, -2.5).equals2D(CoordinateXY(3.0, -2.0)));

  std::unique_ptr<Polygon> circle = shape.createCircle();
  ASSERT_NE(circle, nullptr);
  const CoordinateSequence* coords =
      circle->getExteriorRing()->getCoordinatesRO();
  ASSERT_EQ(coords->size(), 5u);
  EXPECT_TRUE(coords->getAt<CoordinateXY>(0).equals2D(CoordinateXY(3.0, 0.0)));
  EXPECT_TRUE(coords->getAt<CoordinateXY>(1).equals2D(CoordinateXY(0.0, 3.0)));
  EXPECT_TRUE(coords->getAt<CoordinateXY>(2).equals2D(CoordinateXY(-2.0, 0.0)));
  EXPECT_TRUE(coords->getAt<CoordinateXY>(3).equals2D(CoordinateXY(0.0, -2.0)));
  EXPECT_TRUE(coords->getAt<CoordinateXY>(4).equals2D(CoordinateXY(3.0, 0.0)));
}

TEST(GeometricShapeFactoryTest, DimensionSettersOverrideAndBaseAnchoredShapes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  GeometricShapeFactory shape(factory.get());
  shape.setBase(CoordinateXY(1.0, 2.0));
  shape.setSize(10.0);
  shape.setWidth(6.0);
  shape.setHeight(4.0);
  shape.setNumPoints(4);

  // Base (1, 2) with width 6 and height 4 has centre (4, 4) and radii (3, 2).
  std::unique_ptr<Polygon> ellipse = shape.createCircle();
  ASSERT_NE(ellipse, nullptr);
  EXPECT_TRUE(ellipse->isValid());
  EXPECT_EQ(*ellipse->getEnvelopeInternal(), Envelope(1.0, 7.0, 2.0, 6.0));
  EXPECT_DOUBLE_EQ(ellipse->getArea(), 12.0);

  // Negative start angle on base-anchored ellipse: (4, 2) -> (7, 4) -> (4, 6).
  shape.setNumPoints(3);
  std::unique_ptr<LineString> arc = shape.createArc(-kPi / 2.0, kPi);
  ASSERT_NE(arc, nullptr);
  ASSERT_EQ(arc->getNumPoints(), 3u);
  EXPECT_NEAR(arc->getCoordinateN(0).x, 4.0, 1e-12);
  EXPECT_NEAR(arc->getCoordinateN(0).y, 2.0, 1e-12);
  EXPECT_NEAR(arc->getCoordinateN(1).x, 7.0, 1e-12);
  EXPECT_NEAR(arc->getCoordinateN(1).y, 4.0, 1e-12);
  EXPECT_NEAR(arc->getCoordinateN(2).x, 4.0, 1e-12);
  EXPECT_NEAR(arc->getCoordinateN(2).y, 6.0, 1e-12);

  std::unique_ptr<Polygon> sector = shape.createArcPolygon(-kPi / 2.0, kPi);
  ASSERT_NE(sector, nullptr);
  EXPECT_TRUE(sector->isValid());
  EXPECT_EQ(sector->getNumPoints(), 5u);
  EXPECT_NEAR(sector->getArea(), 6.0, 1e-12);

  // setSize resets both width and height; clearing base activates centre.
  shape.setSize(8.0);
  shape.setCentre(CoordinateXY(0.0, 0.0));
  shape.setBase(CoordinateXY::getNull());
  shape.setNumPoints(4);
  std::unique_ptr<Polygon> centered_square = shape.createRectangle();
  ASSERT_NE(centered_square, nullptr);
  EXPECT_TRUE(centered_square->isRectangle());
  EXPECT_EQ(*centered_square->getEnvelopeInternal(),
            Envelope(-4.0, 4.0, -4.0, 4.0));
  EXPECT_DOUBLE_EQ(centered_square->getArea(), 64.0);
}

TEST(GeometricShapeFactoryTest,
     PolymorphicDestructionAndFactorySridPropagation) {
  PrecisionModel integer_pm(1.0);
  GeometryFactory::Ptr factory = GeometryFactory::create(&integer_pm, 4326);

  std::unique_ptr<GeometricShapeFactory> shape =
      std::make_unique<ExposedShapeFactory>(factory.get());
  shape->setBase(CoordinateXY(0.3, 0.7));
  shape->setWidth(4.0);
  shape->setHeight(2.0);
  shape->setNumPoints(4);

  std::unique_ptr<Polygon> rect = shape->createRectangle();
  ASSERT_NE(rect, nullptr);
  EXPECT_EQ(rect->getFactory(), factory.get());
  EXPECT_EQ(rect->getSRID(), 4326);
  EXPECT_TRUE(rect->isRectangle());
  EXPECT_DOUBLE_EQ(rect->getArea(), 8.0);
  EXPECT_EQ(*rect->getEnvelopeInternal(), Envelope(0.0, 4.0, 1.0, 3.0));

  std::unique_ptr<LineString> arc = shape->createArc(0.0, kPi);
  ASSERT_NE(arc, nullptr);
  EXPECT_EQ(arc->getFactory(), factory.get());
  EXPECT_EQ(arc->getSRID(), 4326);

  std::unique_ptr<Polygon> sector = shape->createArcPolygon(0.0, kPi / 2.0);
  ASSERT_NE(sector, nullptr);
  EXPECT_EQ(sector->getFactory(), factory.get());
  EXPECT_EQ(sector->getSRID(), 4326);

  shape.reset();
}

}  // namespace
}  // namespace geos_google
