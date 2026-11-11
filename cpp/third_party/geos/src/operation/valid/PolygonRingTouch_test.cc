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

// Unit tests for third_party/geos/src/operation/valid/PolygonRingTouch.cpp.

#include "third_party/geos/include/geos/operation/valid/PolygonRingTouch.h"

#include <initializer_list>
#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/operation/valid/PolygonRing.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::GeometryFactory;
using geos::operation::valid::PolygonRing;
using geos::operation::valid::PolygonRingTouch;

TEST(PolygonRingTouchTest, AccessorsReturnStoredRingAndCoordinate) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto shell_lr = factory->createLinearRing(
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {0.0, 0.0}, {0.0, 10.0}, {10.0, 10.0}, {10.0, 0.0}, {0.0, 0.0}}));
  auto hole_lr = factory->createLinearRing(
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {5.0, 0.0}, {7.0, 4.0}, {3.0, 4.0}, {5.0, 0.0}}));

  PolygonRing shell_ring(shell_lr.get());
  PolygonRing hole_ring(hole_lr.get(), 0, &shell_ring);

  CoordinateXY pt(12.5, -3.75);
  PolygonRingTouch touch(&hole_ring, pt);
  // Mutating the original CoordinateXY must not affect the stored touchPt.
  pt.x = -100.0;
  pt.y = 100.0;

  EXPECT_EQ(touch.getRing(), &hole_ring);

  const CoordinateXY* stored_pt = touch.getCoordinate();
  ASSERT_NE(stored_pt, nullptr);
  EXPECT_DOUBLE_EQ(stored_pt->x, 12.5);
  EXPECT_DOUBLE_EQ(stored_pt->y, -3.75);
  EXPECT_EQ(touch.getCoordinate(), stored_pt);
}

TEST(PolygonRingTouchTest, NullRingPointerIsPreserved) {
  const CoordinateXY pt(-4.0, 8.25);
  PolygonRingTouch touch(nullptr, pt);

  EXPECT_EQ(touch.getRing(), nullptr);
  ASSERT_NE(touch.getCoordinate(), nullptr);
  EXPECT_TRUE(touch.getCoordinate()->equals2D(pt));
  EXPECT_TRUE(touch.isAtLocation(pt));
}

TEST(PolygonRingTouchTest, IsAtLocationUsesExact2DComparison) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto shell_lr = factory->createLinearRing(
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {0.0, 0.0}, {0.0, 10.0}, {10.0, 10.0}, {10.0, 0.0}, {0.0, 0.0}}));
  PolygonRing shell_ring(shell_lr.get());

  const CoordinateXY touch_pt(4.0, 9.0);
  PolygonRingTouch touch(&shell_ring, touch_pt);

  EXPECT_TRUE(touch.isAtLocation(CoordinateXY(4.0, 9.0)));
  EXPECT_FALSE(touch.isAtLocation(CoordinateXY(4.0 + 1e-12, 9.0)));
  EXPECT_FALSE(touch.isAtLocation(CoordinateXY(4.0, 9.0 - 1e-12)));
  EXPECT_FALSE(touch.isAtLocation(CoordinateXY(9.0, 4.0)));

  // 3D Coordinate converts to CoordinateXY; Z ordinate is ignored by equals2D.
  const Coordinate coord_3d(4.0, 9.0, 42.0);
  EXPECT_TRUE(touch.isAtLocation(coord_3d));
}

TEST(PolygonRingTouchTest, IsAtLocationHandlesSignedZeroAndNaN) {
  PolygonRingTouch zero_touch(nullptr, CoordinateXY(-0.0, 0.0));
  EXPECT_TRUE(zero_touch.isAtLocation(CoordinateXY(0.0, -0.0)));

  const CoordinateXY nan_x(geos::DoubleNotANumber, 1.0);
  PolygonRingTouch nan_touch(nullptr, nan_x);
  EXPECT_FALSE(nan_touch.isAtLocation(nan_x));
  EXPECT_FALSE(nan_touch.isAtLocation(CoordinateXY(0.0, 1.0)));

  PolygonRingTouch finite_touch(nullptr, CoordinateXY(1.0, 2.0));
  EXPECT_FALSE(
      finite_touch.isAtLocation(CoordinateXY(1.0, geos::DoubleNotANumber)));
}

}  // namespace
}  // namespace geos_google
