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

// Unit tests for third_party/geos/src/geom/util/SineStarFactory.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for IllegalArgumentException
//   assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/util/SineStarFactory.cpp

#include "third_party/geos/include/geos/geom/util/SineStarFactory.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateXY;
using ::geos::geom::Envelope;
using ::geos::geom::GeometryFactory;
using ::geos::geom::LinearRing;
using ::geos::geom::Polygon;
using ::geos::geom::PrecisionModel;
using ::geos::geom::util::SineStarFactory;
using ::geos::util::IllegalArgumentException;

double DistanceFrom(const CoordinateXY& pt, double cx, double cy) {
  return std::hypot(pt.x - cx, pt.y - cy);
}

TEST(SineStarFactoryTest, CreatesDefaultSineStarWithEightArms) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  SineStarFactory star_factory(factory.get());
  star_factory.setCentre(CoordinateXY(0.0, 0.0));
  star_factory.setSize(10.0);

  std::unique_ptr<Polygon> poly = star_factory.createSineStar();
  ASSERT_NE(poly, nullptr);
  EXPECT_FALSE(poly->isEmpty());
  EXPECT_TRUE(poly->isValid());
  EXPECT_EQ(poly->getGeometryTypeId(), ::geos::geom::GEOS_POLYGON);
  EXPECT_EQ(poly->getNumInteriorRing(), 0u);
  // Default GeometricShapeFactory::nPts is 100, plus 1 closing vertex.
  EXPECT_EQ(poly->getNumPoints(), 101u);

  const LinearRing* ring = poly->getExteriorRing();
  ASSERT_NE(ring, nullptr);
  EXPECT_TRUE(ring->isClosed());
  const CoordinateSequence* coords = ring->getCoordinatesRO();
  ASSERT_EQ(coords->size(), 101u);
  EXPECT_EQ(coords->getAt<CoordinateXY>(0), coords->getAt<CoordinateXY>(100));

  const Envelope* env = poly->getEnvelopeInternal();
  EXPECT_NEAR(env->getMinX(), -5.0, 1e-12);
  EXPECT_NEAR(env->getMaxX(), 5.0, 1e-12);
  EXPECT_NEAR(env->getMinY(), -5.0, 1e-12);
  EXPECT_NEAR(env->getMaxY(), 5.0, 1e-12);

  // With radius 5.0, default numArms 8, default armLengthRatio 0.5, and default
  // nPts 100, radial extent spans [2.5, 5.0] with 12.5 vertices per arm.
  // The area lies strictly between pi * 2.5^2 and pi * 5.0^2 and matches the
  // exact discrete 100-gon formula:
  //   0.5 * 100 * sin(2*pi/100) * (3.75^2 + 0.5 * 1.25^2 * cos(8 * 2*pi/100)).
  constexpr double kPi = std::numbers::pi;
  EXPECT_GT(poly->getArea(), kPi * 2.5 * 2.5);
  EXPECT_LT(poly->getArea(), kPi * 5.0 * 5.0);
  const double expected_discrete_area =
      0.5 * 100.0 * std::sin(2.0 * kPi / 100.0) *
      (3.75 * 3.75 + 0.5 * 1.25 * 1.25 * std::cos(8.0 * 2.0 * kPi / 100.0));
  EXPECT_NEAR(poly->getArea(), expected_discrete_area, 1e-11);
}

TEST(SineStarFactoryTest, ComputesExactVerticesAndRadialProfile) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  SineStarFactory star_factory(factory.get());

  constexpr std::uint32_t kNumPoints = 16;
  constexpr int kNumArms = 4;
  constexpr double kArmRatio = 0.5;
  constexpr double kSize = 20.0;
  constexpr double kCentreX = 3.0;
  constexpr double kCentreY = -4.0;
  constexpr double kRadius = kSize / 2.0;
  constexpr double kInsideRadius = (1.0 - kArmRatio) * kRadius;
  constexpr double kArmMaxLen = kArmRatio * kRadius;

  star_factory.setCentre(CoordinateXY(kCentreX, kCentreY));
  star_factory.setSize(kSize);
  star_factory.setNumPoints(kNumPoints);
  star_factory.setNumArms(kNumArms);
  star_factory.setArmLengthRatio(kArmRatio);

  std::unique_ptr<Polygon> poly = star_factory.createSineStar();
  ASSERT_NE(poly, nullptr);
  EXPECT_TRUE(poly->isValid());

  const CoordinateSequence* coords =
      poly->getExteriorRing()->getCoordinatesRO();
  ASSERT_EQ(coords->size(), kNumPoints + 1);

  for (std::uint32_t i = 0; i < kNumPoints; ++i) {
    const double arc_frac =
        (static_cast<double>(i) / static_cast<double>(kNumPoints)) * kNumArms;
    const double arm_ang_frac = arc_frac - std::floor(arc_frac);
    const double arm_ang = 2.0 * std::numbers::pi * arm_ang_frac;
    const double arm_len_frac = (std::cos(arm_ang) + 1.0) / 2.0;
    const double expected_radius = kInsideRadius + kArmMaxLen * arm_len_frac;
    const double ang =
        static_cast<double>(i) * (2.0 * std::numbers::pi / kNumPoints);
    const double expected_x = expected_radius * std::cos(ang) + kCentreX;
    const double expected_y = expected_radius * std::sin(ang) + kCentreY;

    const CoordinateXY& actual = coords->getAt<CoordinateXY>(i);
    EXPECT_NEAR(actual.x, expected_x, 1e-12) << "at vertex " << i;
    EXPECT_NEAR(actual.y, expected_y, 1e-12) << "at vertex " << i;
  }

  // Verify arm tips, midpoints, valleys, and ring closure directly.
  EXPECT_NEAR(DistanceFrom(coords->getAt<CoordinateXY>(0), kCentreX, kCentreY),
              10.0, 1e-12);
  EXPECT_NEAR(DistanceFrom(coords->getAt<CoordinateXY>(4), kCentreX, kCentreY),
              10.0, 1e-12);
  EXPECT_NEAR(DistanceFrom(coords->getAt<CoordinateXY>(1), kCentreX, kCentreY),
              7.5, 1e-12);
  EXPECT_NEAR(DistanceFrom(coords->getAt<CoordinateXY>(2), kCentreX, kCentreY),
              5.0, 1e-12);
  EXPECT_NEAR(DistanceFrom(coords->getAt<CoordinateXY>(6), kCentreX, kCentreY),
              5.0, 1e-12);
  EXPECT_EQ(coords->getAt<CoordinateXY>(kNumPoints),
            coords->getAt<CoordinateXY>(0));
}

TEST(SineStarFactoryTest, ClampsNegativeArmLengthRatioToZeroCreatingCircle) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  constexpr std::uint32_t kNumPoints = 32;
  constexpr double kSize = 12.0;
  constexpr double kRadius = kSize / 2.0;
  constexpr double kCentreX = 1.0;
  constexpr double kCentreY = 2.0;

  SineStarFactory zero_factory(factory.get());
  zero_factory.setCentre(CoordinateXY(kCentreX, kCentreY));
  zero_factory.setSize(kSize);
  zero_factory.setNumPoints(kNumPoints);
  zero_factory.setNumArms(6);
  zero_factory.setArmLengthRatio(0.0);
  std::unique_ptr<Polygon> zero_poly = zero_factory.createSineStar();

  SineStarFactory neg_factory(factory.get());
  neg_factory.setCentre(CoordinateXY(kCentreX, kCentreY));
  neg_factory.setSize(kSize);
  neg_factory.setNumPoints(kNumPoints);
  neg_factory.setNumArms(6);
  neg_factory.setArmLengthRatio(-0.75);
  std::unique_ptr<Polygon> neg_poly = neg_factory.createSineStar();

  ASSERT_NE(zero_poly, nullptr);
  ASSERT_NE(neg_poly, nullptr);
  EXPECT_TRUE(zero_poly->isValid());
  EXPECT_TRUE(neg_poly->isValid());
  EXPECT_TRUE(neg_poly->equalsExact(zero_poly.get(), 1e-12));

  const CoordinateSequence* coords =
      neg_poly->getExteriorRing()->getCoordinatesRO();
  for (std::size_t i = 0; i < coords->size(); ++i) {
    EXPECT_NEAR(
        DistanceFrom(coords->getAt<CoordinateXY>(i), kCentreX, kCentreY),
        kRadius, 1e-12);
  }

  const double expected_area = 0.5 * kNumPoints * kRadius * kRadius *
                               std::sin(2.0 * std::numbers::pi / kNumPoints);
  EXPECT_NEAR(neg_poly->getArea(), expected_area, 1e-9);
}

TEST(SineStarFactoryTest, ClampsArmLengthRatioAboveOneToOne) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  constexpr std::uint32_t kNumPoints = 16;

  SineStarFactory one_factory(factory.get());
  one_factory.setCentre(CoordinateXY(0.0, 0.0));
  one_factory.setSize(10.0);
  one_factory.setNumPoints(kNumPoints);
  one_factory.setNumArms(4);
  one_factory.setArmLengthRatio(1.0);
  std::unique_ptr<Polygon> one_poly = one_factory.createSineStar();

  SineStarFactory over_factory(factory.get());
  over_factory.setCentre(CoordinateXY(0.0, 0.0));
  over_factory.setSize(10.0);
  over_factory.setNumPoints(kNumPoints);
  over_factory.setNumArms(4);
  over_factory.setArmLengthRatio(1.75);
  std::unique_ptr<Polygon> over_poly = over_factory.createSineStar();

  ASSERT_NE(one_poly, nullptr);
  ASSERT_NE(over_poly, nullptr);
  EXPECT_TRUE(over_poly->equalsExact(one_poly.get(), 1e-12));
  // With 4 arms and armRatio == 1.0, all 4 valleys pinch at (0, 0).
  EXPECT_FALSE(over_poly->isValid());

  const CoordinateSequence* coords =
      over_poly->getExteriorRing()->getCoordinatesRO();
  for (std::size_t valley_idx :
       {std::size_t{2}, std::size_t{6}, std::size_t{10}, std::size_t{14}}) {
    const CoordinateXY& pt = coords->getAt<CoordinateXY>(valley_idx);
    EXPECT_NEAR(pt.x, 0.0, 1e-12);
    EXPECT_NEAR(pt.y, 0.0, 1e-12);
  }

  // With 1 arm and armRatio clamped to 1.0, only one point touches the center,
  // forming a valid pinched cardioid-like polygon.
  SineStarFactory single_arm_factory(factory.get());
  single_arm_factory.setCentre(CoordinateXY(0.0, 0.0));
  single_arm_factory.setSize(10.0);
  single_arm_factory.setNumPoints(kNumPoints);
  single_arm_factory.setNumArms(1);
  single_arm_factory.setArmLengthRatio(2.5);
  std::unique_ptr<Polygon> single_arm_poly =
      single_arm_factory.createSineStar();
  EXPECT_TRUE(single_arm_poly->isValid());
  const CoordinateXY& valley = single_arm_poly->getExteriorRing()
                                   ->getCoordinatesRO()
                                   ->getAt<CoordinateXY>(kNumPoints / 2);
  EXPECT_NEAR(valley.x, 0.0, 1e-12);
  EXPECT_NEAR(valley.y, 0.0, 1e-12);
}

TEST(SineStarFactoryTest, SupportsZeroOneAndOddNumArms) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  SineStarFactory circle_by_arms(factory.get());
  circle_by_arms.setCentre(CoordinateXY(0.0, 0.0));
  circle_by_arms.setSize(10.0);
  circle_by_arms.setNumPoints(24);
  circle_by_arms.setNumArms(0);
  circle_by_arms.setArmLengthRatio(0.8);
  std::unique_ptr<Polygon> zero_arms_poly = circle_by_arms.createSineStar();

  SineStarFactory circle_by_ratio(factory.get());
  circle_by_ratio.setCentre(CoordinateXY(0.0, 0.0));
  circle_by_ratio.setSize(10.0);
  circle_by_ratio.setNumPoints(24);
  circle_by_ratio.setNumArms(5);
  circle_by_ratio.setArmLengthRatio(0.0);
  std::unique_ptr<Polygon> zero_ratio_poly = circle_by_ratio.createSineStar();

  EXPECT_TRUE(zero_arms_poly->isValid());
  EXPECT_TRUE(zero_arms_poly->equalsExact(zero_ratio_poly.get(), 1e-12));

  for (int arms : {1, 3, 5}) {
    SineStarFactory odd_factory(factory.get());
    odd_factory.setCentre(CoordinateXY(0.0, 0.0));
    odd_factory.setSize(10.0);
    odd_factory.setNumPoints(30);
    odd_factory.setNumArms(arms);
    odd_factory.setArmLengthRatio(0.4);
    std::unique_ptr<Polygon> poly = odd_factory.createSineStar();
    ASSERT_NE(poly, nullptr);
    EXPECT_TRUE(poly->isValid()) << "arms=" << arms;
    const CoordinateSequence* coords =
        poly->getExteriorRing()->getCoordinatesRO();
    EXPECT_NEAR(coords->getAt<CoordinateXY>(0).x, 5.0, 1e-12);
    EXPECT_NEAR(coords->getAt<CoordinateXY>(0).y, 0.0, 1e-12);
    if (arms == 1) {
      // Opposite angle pi hits the single valley at insideRadius = 3.0.
      EXPECT_NEAR(coords->getAt<CoordinateXY>(15).x, -3.0, 1e-12);
      EXPECT_NEAR(coords->getAt<CoordinateXY>(15).y, 0.0, 1e-12);
    }
  }
}

TEST(SineStarFactoryTest, RespectsBaseCentreAndWidthBasedCentering) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Positioned via setBase: envelope [10, 18] x [20, 28], centre (14, 24).
  {
    SineStarFactory ssf(factory.get());
    ssf.setBase(CoordinateXY(10.0, 20.0));
    ssf.setSize(8.0);
    ssf.setNumPoints(16);
    ssf.setNumArms(4);
    std::unique_ptr<Polygon> poly = ssf.createSineStar();
    const Envelope* env = poly->getEnvelopeInternal();
    EXPECT_NEAR(env->getMinX(), 10.0, 1e-12);
    EXPECT_NEAR(env->getMaxX(), 18.0, 1e-12);
    EXPECT_NEAR(env->getMinY(), 20.0, 1e-12);
    EXPECT_NEAR(env->getMaxY(), 28.0, 1e-12);
  }

  // Default origin (no base/centre set): SineStarFactory derives radius and
  // both centre offsets from env->getWidth() / 2.0.
  {
    SineStarFactory ssf(factory.get());
    ssf.setWidth(6.0);
    ssf.setHeight(2.0);
    ssf.setNumPoints(16);
    ssf.setNumArms(4);
    std::unique_ptr<Polygon> poly = ssf.createSineStar();
    const Envelope* env = poly->getEnvelopeInternal();
    EXPECT_NEAR(env->getMinX(), 0.0, 1e-12);
    EXPECT_NEAR(env->getMaxX(), 6.0, 1e-12);
    EXPECT_NEAR(env->getMinY(), 0.0, 1e-12);
    EXPECT_NEAR(env->getMaxY(), 6.0, 1e-12);
  }

  // Rectangular envelope with setCentre: centreY = env->getMinY() + width / 2.
  {
    SineStarFactory ssf(factory.get());
    ssf.setCentre(CoordinateXY(2.0, 5.0));
    ssf.setWidth(10.0);
    ssf.setHeight(4.0);
    ssf.setNumPoints(16);
    ssf.setNumArms(4);
    std::unique_ptr<Polygon> poly = ssf.createSineStar();
    const Envelope* env = poly->getEnvelopeInternal();
    EXPECT_NEAR(env->getMinX(), -3.0, 1e-12);
    EXPECT_NEAR(env->getMaxX(), 7.0, 1e-12);
    EXPECT_NEAR(env->getMinY(), 3.0, 1e-12);
    EXPECT_NEAR(env->getMaxY(), 13.0, 1e-12);
  }
}

TEST(SineStarFactoryTest, AppliesFactoryPrecisionModelAndSrid) {
  PrecisionModel fixed_pm(1.0);
  GeometryFactory::Ptr factory = GeometryFactory::create(&fixed_pm, 3857);
  SineStarFactory star_factory(factory.get());
  star_factory.setCentre(CoordinateXY(0.0, 0.0));
  star_factory.setSize(20.0);
  star_factory.setNumPoints(16);
  star_factory.setNumArms(4);

  std::unique_ptr<Polygon> poly = star_factory.createSineStar();
  ASSERT_NE(poly, nullptr);
  EXPECT_EQ(poly->getSRID(), 3857);
  EXPECT_EQ(poly->getExteriorRing()->getSRID(), 3857);

  const CoordinateSequence* coords =
      poly->getExteriorRing()->getCoordinatesRO();
  ASSERT_EQ(coords->size(), 17u);
  for (std::size_t i = 0; i < coords->size(); ++i) {
    const CoordinateXY& pt = coords->getAt<CoordinateXY>(i);
    EXPECT_DOUBLE_EQ(pt.x, std::trunc(pt.x));
    EXPECT_DOUBLE_EQ(pt.y, std::trunc(pt.y));
  }
  // At i=1 (ang = pi/8, r = 7.5), 7.5 * cos(pi/8) ~ 6.929 rounds to 7.0.
  EXPECT_DOUBLE_EQ(coords->getAt<CoordinateXY>(1).x, 7.0);
}

TEST(SineStarFactoryTest, HandlesMinimalAndInvalidPointCounts) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Minimum valid non-degenerate ring has 3 distinct vertices + closing point.
  {
    SineStarFactory ssf(factory.get());
    ssf.setCentre(CoordinateXY(0.0, 0.0));
    ssf.setSize(10.0);
    ssf.setNumPoints(3);
    ssf.setNumArms(3);
    std::unique_ptr<Polygon> poly = ssf.createSineStar();
    ASSERT_NE(poly, nullptr);
    EXPECT_EQ(poly->getNumPoints(), 4u);
    EXPECT_TRUE(poly->isValid());
    EXPECT_GT(poly->getArea(), 0.0);
  }

  // nPts == 2 yields a 3-point closed collapsed ring (constructible, zero
  // area).
  {
    SineStarFactory ssf(factory.get());
    ssf.setCentre(CoordinateXY(0.0, 0.0));
    ssf.setSize(10.0);
    ssf.setNumPoints(2);
    std::unique_ptr<Polygon> poly = ssf.createSineStar();
    ASSERT_NE(poly, nullptr);
    EXPECT_EQ(poly->getNumPoints(), 3u);
    EXPECT_DOUBLE_EQ(poly->getArea(), 0.0);
    EXPECT_FALSE(poly->isValid());
  }

  // nPts == 1 yields 2 coordinates, violating LinearRing::MINIMUM_VALID_SIZE.
  {
    SineStarFactory ssf(factory.get());
    ssf.setCentre(CoordinateXY(0.0, 0.0));
    ssf.setSize(10.0);
    ssf.setNumPoints(1);
    EXPECT_THROW(ssf.createSineStar(), IllegalArgumentException);
  }

  // nPts == 0 yields 1 coordinate (0 loop iterations), rejected by LineString.
  {
    SineStarFactory ssf(factory.get());
    ssf.setCentre(CoordinateXY(0.0, 0.0));
    ssf.setSize(10.0);
    ssf.setNumPoints(0);
    EXPECT_THROW(ssf.createSineStar(), IllegalArgumentException);
  }
}

TEST(SineStarFactoryTest, SupportsNegativeNumArmsZeroSizeAndReuse) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Negative numArms wraps symmetrically via floor() in armAngFrac, yielding
  // the same vertices as its positive counterpart.
  SineStarFactory pos_factory(factory.get());
  pos_factory.setCentre(CoordinateXY(0.0, 0.0));
  pos_factory.setSize(10.0);
  pos_factory.setNumPoints(24);
  pos_factory.setNumArms(4);
  pos_factory.setArmLengthRatio(0.6);

  SineStarFactory neg_factory(factory.get());
  neg_factory.setCentre(CoordinateXY(0.0, 0.0));
  neg_factory.setSize(10.0);
  neg_factory.setNumPoints(24);
  neg_factory.setNumArms(-4);
  neg_factory.setArmLengthRatio(0.6);

  const SineStarFactory& const_neg_factory = neg_factory;
  std::unique_ptr<Polygon> pos_poly = pos_factory.createSineStar();
  std::unique_ptr<Polygon> neg_poly = const_neg_factory.createSineStar();
  ASSERT_NE(pos_poly, nullptr);
  ASSERT_NE(neg_poly, nullptr);
  EXPECT_TRUE(pos_poly->isValid());
  EXPECT_TRUE(neg_poly->equalsExact(pos_poly.get(), 1e-12));

  // Mutating factory settings on an existing instance updates subsequent stars.
  neg_factory.setNumArms(6);
  neg_factory.setArmLengthRatio(0.25);
  std::unique_ptr<Polygon> updated_poly = const_neg_factory.createSineStar();
  ASSERT_NE(updated_poly, nullptr);
  EXPECT_TRUE(updated_poly->isValid());
  EXPECT_FALSE(updated_poly->equalsExact(pos_poly.get(), 1e-6));
  const CoordinateSequence* updated_coords =
      updated_poly->getExteriorRing()->getCoordinatesRO();
  // At i=2 (1/12 revolution, 1/2 of an arm for 6 arms), curveRadius equals
  // insideRadius = (1 - 0.25) * 5.0 = 3.75.
  EXPECT_NEAR(DistanceFrom(updated_coords->getAt<CoordinateXY>(2), 0.0, 0.0),
              3.75, 1e-12);

  // Zero size collapses all vertices to the configured centre.
  SineStarFactory zero_size_factory(factory.get());
  zero_size_factory.setCentre(CoordinateXY(2.0, -3.0));
  zero_size_factory.setSize(0.0);
  zero_size_factory.setNumPoints(8);
  std::unique_ptr<Polygon> collapsed = zero_size_factory.createSineStar();
  ASSERT_NE(collapsed, nullptr);
  EXPECT_EQ(collapsed->getNumPoints(), 9u);
  EXPECT_DOUBLE_EQ(collapsed->getArea(), 0.0);
  EXPECT_FALSE(collapsed->isValid());
  const CoordinateSequence* collapsed_coords =
      collapsed->getExteriorRing()->getCoordinatesRO();
  for (std::size_t i = 0; i < collapsed_coords->size(); ++i) {
    const CoordinateXY& pt = collapsed_coords->getAt<CoordinateXY>(i);
    EXPECT_DOUBLE_EQ(pt.x, 2.0);
    EXPECT_DOUBLE_EQ(pt.y, -3.0);
  }
}

}  // namespace
}  // namespace geos_google
