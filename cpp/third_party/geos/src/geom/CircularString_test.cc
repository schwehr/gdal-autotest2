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

// Unit tests for third_party/geos/src/geom/CircularString.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/CircularString.cpp

#include "third_party/geos/include/geos/geom/CircularString.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <numbers>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/CoordinateSequenceFilter.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::geom::CircularString;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateSequenceFilter;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Dimension;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Point;
using geos::io::WKTReader;
using geos::util::IllegalArgumentException;
using geos::util::UnsupportedOperationException;
using ::testing::HasSubstr;

std::unique_ptr<CoordinateSequence> MakeSeqXY(
    std::initializer_list<CoordinateXY> coords) {
  return std::make_unique<CoordinateSequence>(coords);
}

std::unique_ptr<CoordinateSequence> MakeSeqXYZ(
    std::initializer_list<Coordinate> coords) {
  return std::make_unique<CoordinateSequence>(coords);
}

std::unique_ptr<CoordinateSequence> MakeSeqXYM(
    std::initializer_list<CoordinateXYM> coords) {
  return std::make_unique<CoordinateSequence>(coords);
}

std::unique_ptr<CoordinateSequence> MakeSeqXYZM(
    std::initializer_list<CoordinateXYZM> coords) {
  return std::make_unique<CoordinateSequence>(coords);
}

class ScaleSequenceFilter final : public CoordinateSequenceFilter {
 public:
  explicit ScaleSequenceFilter(double scale) : scale_(scale) {}

  void filter_rw(CoordinateSequence& seq, std::size_t i) override {
    CoordinateXY c = seq.getAt<CoordinateXY>(i);
    c.x *= scale_;
    c.y *= scale_;
    seq.setAt(c, i);
  }

  bool isDone() const override { return false; }

  bool isGeometryChanged() const override { return true; }

 private:
  double scale_;
};

class CircularStringTest : public ::testing::Test {
 protected:
  CircularStringTest() : factory_(GeometryFactory::create()) {}

  GeometryFactory::Ptr factory_;
};

TEST_F(CircularStringTest, EmptyCreationAndDimensions) {
  struct DimCase {
    bool has_z;
    bool has_m;
    std::uint8_t expected_coord_dim;
  };
  constexpr DimCase kCases[] = {
      {.has_z = false, .has_m = false, .expected_coord_dim = 2},
      {.has_z = true, .has_m = false, .expected_coord_dim = 3},
      {.has_z = false, .has_m = true, .expected_coord_dim = 3},
      {.has_z = true, .has_m = true, .expected_coord_dim = 4},
  };

  for (const auto& c : kCases) {
    std::unique_ptr<CircularString> cs =
        factory_->createCircularString(c.has_z, c.has_m);
    ASSERT_NE(cs, nullptr);
    EXPECT_TRUE(cs->isEmpty());
    EXPECT_EQ(cs->getNumPoints(), 0u);
    EXPECT_EQ(cs->getLength(), 0.0);
    EXPECT_EQ(cs->getCoordinate(), nullptr);
    EXPECT_EQ(cs->getStartPoint(), nullptr);
    EXPECT_EQ(cs->getEndPoint(), nullptr);
    EXPECT_EQ(cs->getGeometryType(), "CircularString");
    EXPECT_EQ(cs->getGeometryTypeId(), geos::geom::GEOS_CIRCULARSTRING);
    EXPECT_TRUE(cs->hasCurvedComponents());
    EXPECT_TRUE(cs->isCurved());
    EXPECT_FALSE(cs->isClosed());
    EXPECT_EQ(cs->getDimension(), Dimension::L);
    EXPECT_EQ(cs->getBoundaryDimension(), 0);
    EXPECT_TRUE(cs->getEnvelopeInternal()->isNull());
    EXPECT_EQ(cs->hasZ(), c.has_z);
    EXPECT_EQ(cs->hasM(), c.has_m);
    EXPECT_EQ(cs->getCoordinateDimension(), c.expected_coord_dim);
  }
}

TEST_F(CircularStringTest, NullAndEmptySequenceCreation) {
  std::unique_ptr<CircularString> from_null =
      factory_->createCircularString(CoordinateSequence::Ptr{});
  ASSERT_NE(from_null, nullptr);
  EXPECT_TRUE(from_null->isEmpty());
  EXPECT_EQ(from_null->getGeometryTypeId(), geos::geom::GEOS_CIRCULARSTRING);
  EXPECT_EQ(from_null->getLength(), 0.0);

  std::unique_ptr<CircularString> from_empty =
      factory_->createCircularString(MakeSeqXY({}));
  ASSERT_NE(from_empty, nullptr);
  EXPECT_TRUE(from_empty->isEmpty());
  EXPECT_TRUE(from_empty->equalsExact(from_null.get()));
}

TEST_F(CircularStringTest, TwoPointSequenceThrowsIllegalArgument) {
  auto two_pts = MakeSeqXY({{0.0, 0.0}, {1.0, 1.0}});
  try {
    factory_->createCircularString(std::move(two_pts));
    FAIL() << "Expected IllegalArgumentException for 2-point CircularString";
  } catch (const IllegalArgumentException& e) {
    EXPECT_THAT(e.what(), HasSubstr("point array must contain 0 or >2"));
  }
}

TEST_F(CircularStringTest, CopyAndCloneConstruction) {
  auto seq = MakeSeqXY({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}});
  std::unique_ptr<CircularString> cs =
      factory_->createCircularString(std::move(seq));
  cs->setSRID(4326);

  std::unique_ptr<CircularString> cloned = cs->clone();
  ASSERT_NE(cloned, nullptr);
  EXPECT_NE(cloned.get(), cs.get());
  EXPECT_EQ(cloned->getGeometryTypeId(), geos::geom::GEOS_CIRCULARSTRING);
  EXPECT_EQ(cloned->getSRID(), 4326);
  EXPECT_TRUE(cloned->equalsExact(cs.get()));
  EXPECT_TRUE(cloned->equalsIdentical(cs.get()));

  const Geometry* base_geom = cs.get();
  std::unique_ptr<Geometry> base_cloned = base_geom->clone();
  ASSERT_NE(base_cloned, nullptr);
  EXPECT_EQ(base_cloned->getGeometryTypeId(), geos::geom::GEOS_CIRCULARSTRING);
  EXPECT_TRUE(base_cloned->equalsIdentical(cs.get()));

  std::unique_ptr<CircularString> copied = factory_->createCircularString(*cs);
  ASSERT_NE(copied, nullptr);
  EXPECT_NE(copied.get(), cs.get());
  EXPECT_TRUE(copied->equalsIdentical(cs.get()));

  CoordinateSequence source_seq(
      {CoordinateXY{0.0, 0.0}, CoordinateXY{1.0, 1.0}, CoordinateXY{2.0, 0.0}});
  std::unique_ptr<CircularString> from_const_seq =
      factory_->createCircularString(source_seq);
  source_seq.setAt(CoordinateXY{99.0, 99.0}, 1);
  EXPECT_EQ(from_const_seq->getCoordinateN(1), (CoordinateXY{1.0, 1.0}));
}

TEST_F(CircularStringTest, ArcLengthEnvelopeAndClosure) {
  // Upper semicircle of radius 1 centered at (1, 0), clockwise.
  std::unique_ptr<CircularString> semi_cw = factory_->createCircularString(
      MakeSeqXY({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}}));
  EXPECT_NEAR(semi_cw->getLength(), std::numbers::pi, 1e-12);
  EXPECT_FALSE(semi_cw->isClosed());
  EXPECT_EQ(semi_cw->getBoundaryDimension(), 0);
  EXPECT_EQ(semi_cw->getNumCurves(), 1u);
  EXPECT_EQ(semi_cw->getCurveN(0), semi_cw.get());
  EXPECT_EQ(semi_cw->getNumPoints(), 3u);
  const Envelope* semi_env = semi_cw->getEnvelopeInternal();
  EXPECT_NEAR(semi_env->getMinX(), 0.0, 1e-12);
  EXPECT_NEAR(semi_env->getMaxX(), 2.0, 1e-12);
  EXPECT_NEAR(semi_env->getMinY(), 0.0, 1e-12);
  EXPECT_NEAR(semi_env->getMaxY(), 1.0, 1e-12);

  // Counterclockwise major arc of radius 1 centered at (0, 0): 3*pi/2 radians.
  std::unique_ptr<CircularString> major_ccw = factory_->createCircularString(
      MakeSeqXY({{1.0, 0.0}, {0.0, 1.0}, {0.0, -1.0}}));
  EXPECT_NEAR(major_ccw->getLength(), 1.5 * std::numbers::pi, 1e-12);
  const Envelope* major_env = major_ccw->getEnvelopeInternal();
  EXPECT_NEAR(major_env->getMinX(), -1.0, 1e-12);
  EXPECT_NEAR(major_env->getMaxX(), 1.0, 1e-12);
  EXPECT_NEAR(major_env->getMinY(), -1.0, 1e-12);
  EXPECT_NEAR(major_env->getMaxY(), 1.0, 1e-12);

  // Closed circle: diameter from (0, 0) to (2, 0), radius 1 centered at (1, 0).
  std::unique_ptr<CircularString> circle = factory_->createCircularString(
      MakeSeqXY({{0.0, 0.0}, {2.0, 0.0}, {0.0, 0.0}}));
  EXPECT_TRUE(circle->isClosed());
  EXPECT_EQ(circle->getBoundaryDimension(), Dimension::False);
  EXPECT_NEAR(circle->getLength(), 2.0 * std::numbers::pi, 1e-12);
  const Envelope* circle_env = circle->getEnvelopeInternal();
  EXPECT_NEAR(circle_env->getMinX(), 0.0, 1e-12);
  EXPECT_NEAR(circle_env->getMaxX(), 2.0, 1e-12);
  EXPECT_NEAR(circle_env->getMinY(), -1.0, 1e-12);
  EXPECT_NEAR(circle_env->getMaxY(), 1.0, 1e-12);

  // Two consecutive semicircles of radius 1 (5 points).
  std::unique_ptr<CircularString> wave = factory_->createCircularString(
      MakeSeqXY({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}, {3.0, -1.0}, {4.0, 0.0}}));
  EXPECT_EQ(wave->getNumPoints(), 5u);
  EXPECT_FALSE(wave->isClosed());
  EXPECT_NEAR(wave->getLength(), 2.0 * std::numbers::pi, 1e-12);
  const Envelope* wave_env = wave->getEnvelopeInternal();
  EXPECT_NEAR(wave_env->getMinX(), 0.0, 1e-12);
  EXPECT_NEAR(wave_env->getMaxX(), 4.0, 1e-12);
  EXPECT_NEAR(wave_env->getMinY(), -1.0, 1e-12);
  EXPECT_NEAR(wave_env->getMaxY(), 1.0, 1e-12);
}

TEST_F(CircularStringTest, ReverseEmptyAndNonEmpty) {
  std::unique_ptr<CircularString> empty_cs =
      factory_->createCircularString(true, false);
  std::unique_ptr<CircularString> empty_rev = empty_cs->reverse();
  ASSERT_NE(empty_rev, nullptr);
  EXPECT_NE(empty_rev.get(), empty_cs.get());
  EXPECT_TRUE(empty_rev->isEmpty());
  EXPECT_EQ(empty_rev->getGeometryTypeId(), geos::geom::GEOS_CIRCULARSTRING);
  EXPECT_TRUE(empty_rev->hasZ());
  EXPECT_FALSE(empty_rev->hasM());

  std::unique_ptr<CircularString> arc = factory_->createCircularString(
      MakeSeqXY({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}}));
  std::unique_ptr<CircularString> rev = arc->reverse();
  ASSERT_NE(rev, nullptr);
  EXPECT_EQ(rev->getGeometryTypeId(), geos::geom::GEOS_CIRCULARSTRING);
  EXPECT_EQ(rev->getNumPoints(), 3u);
  EXPECT_EQ(rev->getCoordinateN(0), (CoordinateXY{2.0, 0.0}));
  EXPECT_EQ(rev->getCoordinateN(1), (CoordinateXY{1.0, 1.0}));
  EXPECT_EQ(rev->getCoordinateN(2), (CoordinateXY{0.0, 0.0}));
  EXPECT_NEAR(rev->getLength(), arc->getLength(), 1e-12);

  std::unique_ptr<CircularString> double_rev = rev->reverse();
  EXPECT_TRUE(double_rev->equalsExact(arc.get()));

  const Geometry* base_arc = arc.get();
  std::unique_ptr<Geometry> base_rev = base_arc->reverse();
  EXPECT_EQ(base_rev->getGeometryTypeId(), geos::geom::GEOS_CIRCULARSTRING);
  EXPECT_TRUE(base_rev->equalsExact(rev.get()));
}

TEST_F(CircularStringTest, GeometryChangedRecomputesEnvelopeAndLength) {
  std::unique_ptr<CircularString> arc = factory_->createCircularString(
      MakeSeqXY({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}}));
  ScaleSequenceFilter scale_by_two(2.0);
  arc->apply_rw(scale_by_two);

  EXPECT_NEAR(arc->getLength(), 2.0 * std::numbers::pi, 1e-12);
  const Envelope* env = arc->getEnvelopeInternal();
  EXPECT_NEAR(env->getMinX(), 0.0, 1e-12);
  EXPECT_NEAR(env->getMaxX(), 4.0, 1e-12);
  EXPECT_NEAR(env->getMinY(), 0.0, 1e-12);
  EXPECT_NEAR(env->getMaxY(), 2.0, 1e-12);
}

TEST_F(CircularStringTest, CompareToAndSortOrder) {
  std::unique_ptr<CircularString> a = factory_->createCircularString(
      MakeSeqXY({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}}));
  std::unique_ptr<CircularString> b = factory_->createCircularString(
      MakeSeqXY({{0.0, 0.0}, {1.0, 2.0}, {2.0, 0.0}}));
  std::unique_ptr<CircularString> empty =
      factory_->createCircularString(false, false);
  std::unique_ptr<Point> pt = factory_->createPoint(CoordinateXY{0.0, 0.0});

  EXPECT_EQ(a->compareTo(a.get()), 0);
  EXPECT_LT(a->compareTo(b.get()), 0);
  EXPECT_GT(b->compareTo(a.get()), 0);
  EXPECT_LT(empty->compareTo(a.get()), 0);
  EXPECT_GT(a->compareTo(empty.get()), 0);
  EXPECT_GT(a->compareTo(pt.get()), 0);
  EXPECT_LT(pt->compareTo(a.get()), 0);
}

TEST_F(CircularStringTest, ZAndMCoordinates) {
  std::unique_ptr<CircularString> xyz = factory_->createCircularString(
      MakeSeqXYZ({{0.0, 0.0, 10.0}, {1.0, 1.0, 20.0}, {2.0, 0.0, 30.0}}));
  EXPECT_TRUE(xyz->hasZ());
  EXPECT_FALSE(xyz->hasM());
  EXPECT_EQ(xyz->getCoordinateDimension(), 3);
  std::unique_ptr<Point> p1 = xyz->getPointN(1);
  EXPECT_DOUBLE_EQ(p1->getZ(), 20.0);
  EXPECT_TRUE(std::isnan(p1->getM()));

  std::unique_ptr<CircularString> xym = factory_->createCircularString(
      MakeSeqXYM({{0.0, 0.0, 1.5}, {1.0, 1.0, 2.5}, {2.0, 0.0, 3.5}}));
  EXPECT_FALSE(xym->hasZ());
  EXPECT_TRUE(xym->hasM());
  EXPECT_EQ(xym->getCoordinateDimension(), 3);
  std::unique_ptr<Point> p2 = xym->getPointN(2);
  EXPECT_TRUE(std::isnan(p2->getZ()));
  EXPECT_DOUBLE_EQ(p2->getM(), 3.5);

  std::unique_ptr<CircularString> xyzm =
      factory_->createCircularString(MakeSeqXYZM(
          {{0.0, 0.0, 1.0, 5.0}, {1.0, 1.0, 2.0, 6.0}, {2.0, 0.0, 3.0, 7.0}}));
  EXPECT_TRUE(xyzm->hasZ());
  EXPECT_TRUE(xyzm->hasM());
  EXPECT_EQ(xyzm->getCoordinateDimension(), 4);
  std::unique_ptr<Point> p0 = xyzm->getPointN(0);
  EXPECT_DOUBLE_EQ(p0->getZ(), 1.0);
  EXPECT_DOUBLE_EQ(p0->getM(), 5.0);
}

TEST_F(CircularStringTest, UnsupportedCurvedOperationsThrow) {
  std::unique_ptr<CircularString> arc = factory_->createCircularString(
      MakeSeqXY({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}}));
  EXPECT_THROW(arc->normalize(), UnsupportedOperationException);
  EXPECT_THROW(arc->getBoundary(), UnsupportedOperationException);

  std::unique_ptr<CircularString> empty_cs =
      factory_->createCircularString(false, false);
  EXPECT_THROW(empty_cs->normalize(), UnsupportedOperationException);
  EXPECT_THROW(empty_cs->getBoundary(), UnsupportedOperationException);
}

TEST_F(CircularStringTest, WktRoundTrip) {
  WKTReader reader(*factory_);
  std::unique_ptr<CircularString> parsed =
      reader.read<CircularString>("CIRCULARSTRING (0 0, 1 1, 2 0)");
  ASSERT_NE(parsed, nullptr);
  EXPECT_EQ(parsed->getGeometryTypeId(), geos::geom::GEOS_CIRCULARSTRING);
  EXPECT_NEAR(parsed->getLength(), std::numbers::pi, 1e-12);
  EXPECT_EQ(parsed->toText(), "CIRCULARSTRING (0 0, 1 1, 2 0)");

  std::unique_ptr<CircularString> parsed_empty =
      reader.read<CircularString>("CIRCULARSTRING EMPTY");
  ASSERT_NE(parsed_empty, nullptr);
  EXPECT_TRUE(parsed_empty->isEmpty());
  EXPECT_EQ(parsed_empty->toText(), "CIRCULARSTRING EMPTY");
}

}  // namespace
}  // namespace geos_google
