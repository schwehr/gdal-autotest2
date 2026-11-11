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

#include "third_party/geos/include/geos/algorithm/Rectangle.h"

#include <cstddef>
#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/Orientation.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"

namespace geos_google {
namespace {

using geos::algorithm::Orientation;
using geos::algorithm::Rectangle;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;

void ExpectRectangleOrthogonalAndCW(const Polygon& poly, double expected_area,
                                    double tol = 1e-9) {
  ASSERT_FALSE(poly.isEmpty());
  const LinearRing* ring = poly.getExteriorRing();
  ASSERT_NE(ring, nullptr);
  const CoordinateSequence* coords = ring->getCoordinatesRO();
  ASSERT_NE(coords, nullptr);
  ASSERT_EQ(coords->size(), 5u);
  EXPECT_FALSE(coords->hasZ());
  EXPECT_FALSE(coords->hasM());
  EXPECT_TRUE(
      coords->getAt<CoordinateXY>(0).equals2D(coords->getAt<CoordinateXY>(4)));

  if (expected_area > 0.0) {
    EXPECT_TRUE(poly.isValid());
    EXPECT_FALSE(Orientation::isCCW(coords));
  }
  EXPECT_NEAR(poly.getArea(), expected_area, tol);

  for (std::size_t i = 0; i < 4; ++i) {
    const CoordinateXY& a = coords->getAt<CoordinateXY>(i);
    const CoordinateXY& b = coords->getAt<CoordinateXY>(i + 1);
    const CoordinateXY& c = coords->getAt<CoordinateXY>((i + 2) % 4);
    const double ux = b.x - a.x;
    const double uy = b.y - a.y;
    const double vx = c.x - b.x;
    const double vy = c.y - b.y;
    EXPECT_NEAR(ux * vx + uy * vy, 0.0, tol);
  }
}

TEST(RectangleTest, HorizontalBaseNoCornerCoincidence) {
  PrecisionModel pm;
  GeometryFactory::Ptr factory = GeometryFactory::create(&pm, 4326);

  // CW corners: (4,0) -> (0,0) -> (0,3) -> (4,3) -> (4,0)
  // None of the side points coincide with rectangle corners.
  std::unique_ptr<Polygon> poly = Rectangle::createFromSidePts(
      CoordinateXY{3.0, 0.0}, CoordinateXY{1.0, 0.0}, CoordinateXY{2.0, 3.0},
      CoordinateXY{0.0, 1.0}, CoordinateXY{4.0, 2.0}, factory.get());
  ASSERT_NE(poly, nullptr);
  EXPECT_EQ(poly->getSRID(), 4326);
  EXPECT_TRUE(poly->isRectangle());
  ExpectRectangleOrthogonalAndCW(*poly, 12.0);
  EXPECT_DOUBLE_EQ(poly->getLength(), 14.0);

  const CoordinateSequence* coords =
      poly->getExteriorRing()->getCoordinatesRO();
  EXPECT_TRUE(coords->getAt<CoordinateXY>(0).equals2D(CoordinateXY{4.0, 0.0}));
  EXPECT_TRUE(coords->getAt<CoordinateXY>(1).equals2D(CoordinateXY{0.0, 0.0}));
  EXPECT_TRUE(coords->getAt<CoordinateXY>(2).equals2D(CoordinateXY{0.0, 3.0}));
  EXPECT_TRUE(coords->getAt<CoordinateXY>(3).equals2D(CoordinateXY{4.0, 3.0}));
  EXPECT_TRUE(coords->getAt<CoordinateXY>(4).equals2D(CoordinateXY{4.0, 0.0}));
}

TEST(RectangleTest, VerticalBaseNoCornerCoincidence) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // CW corners: (10,2) -> (10,6) -> (15,6) -> (15,2) -> (10,2)
  std::unique_ptr<Polygon> poly = Rectangle::createFromSidePts(
      CoordinateXY{10.0, 3.0}, CoordinateXY{10.0, 5.0}, CoordinateXY{15.0, 4.0},
      CoordinateXY{12.0, 6.0}, CoordinateXY{14.0, 2.0}, factory.get());
  ASSERT_NE(poly, nullptr);
  EXPECT_TRUE(poly->isRectangle());
  ExpectRectangleOrthogonalAndCW(*poly, 20.0);

  const CoordinateSequence* coords =
      poly->getExteriorRing()->getCoordinatesRO();
  EXPECT_TRUE(coords->getAt<CoordinateXY>(0).equals2D(CoordinateXY{10.0, 2.0}));
  EXPECT_TRUE(coords->getAt<CoordinateXY>(1).equals2D(CoordinateXY{10.0, 6.0}));
  EXPECT_TRUE(coords->getAt<CoordinateXY>(2).equals2D(CoordinateXY{15.0, 6.0}));
  EXPECT_TRUE(coords->getAt<CoordinateXY>(3).equals2D(CoordinateXY{15.0, 2.0}));
}

TEST(RectangleTest, Diagonal45DegreeBaseSlope) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Equal |dx| == |dy| tests the |b| > |a| boundary condition.
  // CW corners: (4,2) -> (0,-2) -> (-2,0) -> (2,4) -> (4,2)
  // Edge lengths: 4*sqrt(2) by 2*sqrt(2), area = 16.
  std::unique_ptr<Polygon> poly = Rectangle::createFromSidePts(
      CoordinateXY{3.0, 1.0}, CoordinateXY{1.0, -1.0}, CoordinateXY{0.0, 2.0},
      CoordinateXY{-1.0, -1.0}, CoordinateXY{3.0, 3.0}, factory.get());
  ASSERT_NE(poly, nullptr);
  ExpectRectangleOrthogonalAndCW(*poly, 16.0);

  const CoordinateSequence* coords =
      poly->getExteriorRing()->getCoordinatesRO();
  EXPECT_NEAR(coords->getAt<CoordinateXY>(0).x, 4.0, 1e-12);
  EXPECT_NEAR(coords->getAt<CoordinateXY>(0).y, 2.0, 1e-12);
  EXPECT_NEAR(coords->getAt<CoordinateXY>(1).x, 0.0, 1e-12);
  EXPECT_NEAR(coords->getAt<CoordinateXY>(1).y, -2.0, 1e-12);
  EXPECT_NEAR(coords->getAt<CoordinateXY>(2).x, -2.0, 1e-12);
  EXPECT_NEAR(coords->getAt<CoordinateXY>(2).y, 0.0, 1e-12);
  EXPECT_NEAR(coords->getAt<CoordinateXY>(3).x, 2.0, 1e-12);
  EXPECT_NEAR(coords->getAt<CoordinateXY>(3).y, 4.0, 1e-12);
}

TEST(RectangleTest, TiltedShallowAndSteepBases) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Shallow base: vector (-4, -2), perpendicular (-1, 2).
  // Corners: p0=(2, 1), p1=(-2, -1), p2=(-3, 1), p3=(1, 3), area = 10.
  std::unique_ptr<Polygon> shallow = Rectangle::createFromSidePts(
      CoordinateXY{1.0, 0.5}, CoordinateXY{-1.0, -0.5}, CoordinateXY{-1.0, 2.0},
      CoordinateXY{-2.5, 0.0}, CoordinateXY{1.5, 2.0}, factory.get());
  ASSERT_NE(shallow, nullptr);
  ExpectRectangleOrthogonalAndCW(*shallow, 10.0);

  const CoordinateSequence* sc = shallow->getExteriorRing()->getCoordinatesRO();
  EXPECT_NEAR(sc->getAt<CoordinateXY>(0).x, 2.0, 1e-12);
  EXPECT_NEAR(sc->getAt<CoordinateXY>(0).y, 1.0, 1e-12);
  EXPECT_NEAR(sc->getAt<CoordinateXY>(1).x, -2.0, 1e-12);
  EXPECT_NEAR(sc->getAt<CoordinateXY>(1).y, -1.0, 1e-12);
  EXPECT_NEAR(sc->getAt<CoordinateXY>(2).x, -3.0, 1e-12);
  EXPECT_NEAR(sc->getAt<CoordinateXY>(2).y, 1.0, 1e-12);
  EXPECT_NEAR(sc->getAt<CoordinateXY>(3).x, 1.0, 1e-12);
  EXPECT_NEAR(sc->getAt<CoordinateXY>(3).y, 3.0, 1e-12);

  // Steep base: vector (2, 4), perpendicular (4, -2).
  // Corners: p0=(-3, -4), p1=(-1, 0), p2=(3, -2), p3=(1, -6), area = 20.
  std::unique_ptr<Polygon> steep = Rectangle::createFromSidePts(
      CoordinateXY{-2.5, -3.0}, CoordinateXY{-1.5, -1.0},
      CoordinateXY{2.0, -4.0}, CoordinateXY{1.0, -1.0},
      CoordinateXY{-1.0, -5.0}, factory.get());
  ASSERT_NE(steep, nullptr);
  ExpectRectangleOrthogonalAndCW(*steep, 20.0);

  const CoordinateSequence* tc = steep->getExteriorRing()->getCoordinatesRO();
  EXPECT_NEAR(tc->getAt<CoordinateXY>(0).x, -3.0, 1e-12);
  EXPECT_NEAR(tc->getAt<CoordinateXY>(0).y, -4.0, 1e-12);
  EXPECT_NEAR(tc->getAt<CoordinateXY>(1).x, -1.0, 1e-12);
  EXPECT_NEAR(tc->getAt<CoordinateXY>(1).y, 0.0, 1e-12);
  EXPECT_NEAR(tc->getAt<CoordinateXY>(2).x, 3.0, 1e-12);
  EXPECT_NEAR(tc->getAt<CoordinateXY>(2).y, -2.0, 1e-12);
  EXPECT_NEAR(tc->getAt<CoordinateXY>(3).x, 1.0, 1e-12);
  EXPECT_NEAR(tc->getAt<CoordinateXY>(3).y, -6.0, 1e-12);
}

TEST(RectangleTest, CornerCoincidencesUseExactInputPoints) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Case 1: rightSidePt == baseRightPt and leftSidePt == baseLeftPt (p0, p1).
  // Line segments are built near the origin (x in [0, 1] or y in [0, 1]), so at
  // ~(1e6, 2e6) the non-coincident intersections p2 and p3 drift by ~3e-4,
  // while p0 and p1 must retain the exact input coordinates.
  const CoordinateXY base_right{1000000.123456789, 2000000.987654321};
  const CoordinateXY base_left{999997.123456789, 1999999.987654321};
  const CoordinateXY opp{999997.623456789, 2000004.487654321};
  std::unique_ptr<Polygon> base_coincident = Rectangle::createFromSidePts(
      base_right, base_left, opp, base_left, base_right, factory.get());
  ASSERT_NE(base_coincident, nullptr);
  ExpectRectangleOrthogonalAndCW(*base_coincident, 13.0, 1e-2);
  const CoordinateSequence* bc =
      base_coincident->getExteriorRing()->getCoordinatesRO();
  EXPECT_TRUE(bc->getAt<CoordinateXY>(0).equals2D(base_right));
  EXPECT_TRUE(bc->getAt<CoordinateXY>(1).equals2D(base_left));
  EXPECT_TRUE(bc->getAt<CoordinateXY>(4).equals2D(base_right));

  // Individual p0-only and p1-only coincidences at moderate scale.
  const CoordinateXY mod_right{3.25, 1.5};
  const CoordinateXY mod_left{1.25, 0.5};
  const CoordinateXY mod_opp{0.25, 5.0};
  std::unique_ptr<Polygon> p0_only = Rectangle::createFromSidePts(
      mod_right, mod_left, mod_opp, CoordinateXY{0.25, 2.5}, mod_right,
      factory.get());
  ASSERT_NE(p0_only, nullptr);
  ExpectRectangleOrthogonalAndCW(*p0_only, 10.0);
  EXPECT_TRUE(p0_only->getExteriorRing()
                  ->getCoordinatesRO()
                  ->getAt<CoordinateXY>(0)
                  .equals2D(mod_right));

  std::unique_ptr<Polygon> p1_only =
      Rectangle::createFromSidePts(mod_right, mod_left, mod_opp, mod_left,
                                   CoordinateXY{2.25, 3.5}, factory.get());
  ASSERT_NE(p1_only, nullptr);
  ExpectRectangleOrthogonalAndCW(*p1_only, 10.0);
  EXPECT_TRUE(p1_only->getExteriorRing()
                  ->getCoordinatesRO()
                  ->getAt<CoordinateXY>(1)
                  .equals2D(mod_left));

  // Case 2: leftSidePt == oppositePt (p2 coincides with oppositePt).
  const CoordinateXY opp_left{0.125, 2.375};
  std::unique_ptr<Polygon> p2_coincident = Rectangle::createFromSidePts(
      CoordinateXY{3.125, 0.375}, CoordinateXY{1.125, 0.375}, opp_left,
      opp_left, CoordinateXY{4.125, 1.375}, factory.get());
  ASSERT_NE(p2_coincident, nullptr);
  ExpectRectangleOrthogonalAndCW(*p2_coincident, 8.0);
  EXPECT_TRUE(p2_coincident->getExteriorRing()
                  ->getCoordinatesRO()
                  ->getAt<CoordinateXY>(2)
                  .equals2D(opp_left));

  // Case 3: rightSidePt == oppositePt (p3 coincides with oppositePt).
  const CoordinateXY opp_right{4.125, 2.375};
  std::unique_ptr<Polygon> p3_coincident = Rectangle::createFromSidePts(
      CoordinateXY{3.125, 0.375}, CoordinateXY{1.125, 0.375}, opp_right,
      CoordinateXY{0.125, 1.375}, opp_right, factory.get());
  ASSERT_NE(p3_coincident, nullptr);
  ExpectRectangleOrthogonalAndCW(*p3_coincident, 8.0);
  EXPECT_TRUE(p3_coincident->getExteriorRing()
                  ->getCoordinatesRO()
                  ->getAt<CoordinateXY>(3)
                  .equals2D(opp_right));
}

TEST(RectangleTest, NegativeDiagonalAndSidePtsNarrowerThanBase) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Opposite 45-degree diagonal (dx = 4, dy = -4) with side points inside the
  // base segment span, verifying the base line extends/contracts as needed.
  std::unique_ptr<Polygon> poly = Rectangle::createFromSidePts(
      CoordinateXY{-4.0, 2.0}, CoordinateXY{0.0, -2.0},
      CoordinateXY{-4.5, -2.5}, CoordinateXY{-2.5, -1.5},
      CoordinateXY{-3.5, -0.5}, factory.get());
  ASSERT_NE(poly, nullptr);
  ExpectRectangleOrthogonalAndCW(*poly, 5.0);

  const CoordinateSequence* coords =
      poly->getExteriorRing()->getCoordinatesRO();
  EXPECT_NEAR(coords->getAt<CoordinateXY>(0).x, -2.5, 1e-12);
  EXPECT_NEAR(coords->getAt<CoordinateXY>(0).y, 0.5, 1e-12);
  EXPECT_NEAR(coords->getAt<CoordinateXY>(1).x, -1.5, 1e-12);
  EXPECT_NEAR(coords->getAt<CoordinateXY>(1).y, -0.5, 1e-12);
  EXPECT_NEAR(coords->getAt<CoordinateXY>(2).x, -4.0, 1e-12);
  EXPECT_NEAR(coords->getAt<CoordinateXY>(2).y, -3.0, 1e-12);
  EXPECT_NEAR(coords->getAt<CoordinateXY>(3).x, -5.0, 1e-12);
  EXPECT_NEAR(coords->getAt<CoordinateXY>(3).y, -2.0, 1e-12);
}

TEST(RectangleTest, DegenerateZeroWidthAndZeroHeight) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Zero height: oppositePt is collinear with base segment.
  std::unique_ptr<Polygon> zero_height = Rectangle::createFromSidePts(
      CoordinateXY{4.0, 0.0}, CoordinateXY{1.0, 0.0}, CoordinateXY{2.0, 0.0},
      CoordinateXY{0.0, 0.0}, CoordinateXY{5.0, 0.0}, factory.get());
  ASSERT_NE(zero_height, nullptr);
  ExpectRectangleOrthogonalAndCW(*zero_height, 0.0);
  const CoordinateSequence* hc =
      zero_height->getExteriorRing()->getCoordinatesRO();
  EXPECT_TRUE(hc->getAt<CoordinateXY>(0).equals2D(hc->getAt<CoordinateXY>(3)));
  EXPECT_TRUE(hc->getAt<CoordinateXY>(1).equals2D(hc->getAt<CoordinateXY>(2)));

  // Zero width: leftSidePt and rightSidePt coincide on the same perpendicular.
  const CoordinateXY side_pt{2.0, 1.0};
  std::unique_ptr<Polygon> zero_width = Rectangle::createFromSidePts(
      CoordinateXY{4.0, 0.0}, CoordinateXY{1.0, 0.0}, CoordinateXY{2.0, 3.0},
      side_pt, side_pt, factory.get());
  ASSERT_NE(zero_width, nullptr);
  ExpectRectangleOrthogonalAndCW(*zero_width, 0.0);
  const CoordinateSequence* wc =
      zero_width->getExteriorRing()->getCoordinatesRO();
  EXPECT_TRUE(wc->getAt<CoordinateXY>(0).equals2D(wc->getAt<CoordinateXY>(1)));
  EXPECT_TRUE(wc->getAt<CoordinateXY>(2).equals2D(wc->getAt<CoordinateXY>(3)));
}

}  // namespace
}  // namespace geos_google
