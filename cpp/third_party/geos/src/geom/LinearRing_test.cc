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

// Unit tests for third_party/geos/src/geom/LinearRing.cpp and
// third_party/geos/include/geos/geom/LinearRing.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/LinearRing.cpp

#include "third_party/geos/include/geos/geom/LinearRing.h"

#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/Orientation.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::algorithm::Orientation;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYZM;
using geos::geom::Dimension;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::Polygon;
using geos::util::IllegalArgumentException;
using ::testing::HasSubstr;

static_assert(LinearRing::MINIMUM_VALID_SIZE == 3);

CoordinateSequence::Ptr MakeSeqXY(std::initializer_list<CoordinateXY> coords) {
  return std::make_unique<CoordinateSequence>(coords);
}

CoordinateSequence::Ptr MakeSeqXYZ(std::initializer_list<Coordinate> coords) {
  return std::make_unique<CoordinateSequence>(coords);
}

CoordinateSequence::Ptr MakeSeqXYZM(
    std::initializer_list<CoordinateXYZM> coords) {
  return std::make_unique<CoordinateSequence>(coords);
}

TEST(LinearRingTest, EmptyConstructionAndMetadata) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<LinearRing> empty = factory->createLinearRing();
  ASSERT_NE(empty, nullptr);
  EXPECT_TRUE(empty->isEmpty());
  EXPECT_TRUE(empty->isClosed());
  EXPECT_EQ(empty->getNumPoints(), 0u);
  EXPECT_EQ(empty->getGeometryType(), "LinearRing");
  EXPECT_EQ(empty->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  EXPECT_EQ(empty->getDimension(), Dimension::L);
  EXPECT_EQ(empty->getBoundaryDimension(), Dimension::False);
  EXPECT_TRUE(empty->getBoundary()->isEmpty());
  EXPECT_DOUBLE_EQ(empty->getLength(), 0.0);
  EXPECT_FALSE(empty->hasZ());
  EXPECT_FALSE(empty->hasM());

  std::unique_ptr<LinearRing> empty_3d = factory->createLinearRing(3);
  EXPECT_TRUE(empty_3d->isEmpty());
  EXPECT_TRUE(empty_3d->isClosed());
  EXPECT_TRUE(empty_3d->hasZ());
  EXPECT_FALSE(empty_3d->hasM());

  std::unique_ptr<LinearRing> empty_xyzm =
      factory->createLinearRing(true, true);
  EXPECT_TRUE(empty_xyzm->isEmpty());
  EXPECT_TRUE(empty_xyzm->hasZ());
  EXPECT_TRUE(empty_xyzm->hasM());

  CoordinateSequence::Ptr null_seq;
  LinearRing direct_empty(std::move(null_seq), *factory);
  EXPECT_TRUE(direct_empty.isEmpty());
  EXPECT_TRUE(direct_empty.isClosed());
  EXPECT_EQ(direct_empty.getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
}

TEST(LinearRingTest, ConstructionValidationChecksClosureAndMinimumSize) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Single-point sequence fails base LineString validation.
  EXPECT_THROW((void)LinearRing(MakeSeqXY({{0.0, 0.0}}), *factory),
               IllegalArgumentException);

  // Unclosed sequences fail closure validation.
  try {
    LinearRing unclosed_two(MakeSeqXY({{0.0, 0.0}, {1.0, 0.0}}), *factory);
    FAIL() << "Expected IllegalArgumentException for unclosed 2-point ring";
  } catch (const IllegalArgumentException& ex) {
    EXPECT_THAT(ex.what(), HasSubstr("closed"));
  }

  EXPECT_THROW(factory->createLinearRing(
                   MakeSeqXY({{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}})),
               IllegalArgumentException);

  // Closed 2-point sequence passes isClosed() but fails MINIMUM_VALID_SIZE (3).
  try {
    LinearRing closed_two(MakeSeqXY({{1.0, 2.0}, {1.0, 2.0}}), *factory);
    FAIL() << "Expected IllegalArgumentException for 2-point closed ring";
  } catch (const IllegalArgumentException& ex) {
    EXPECT_THAT(ex.what(), HasSubstr("Invalid number of points"));
  }

  // Collapsed 3-point closed ring is allowed by constructor per contract.
  LinearRing collapsed_three(MakeSeqXY({{0.0, 0.0}, {1.0, 0.0}, {0.0, 0.0}}),
                             *factory);
  EXPECT_EQ(collapsed_three.getNumPoints(), 3u);
  EXPECT_TRUE(collapsed_three.isClosed());
  EXPECT_FALSE(collapsed_three.isValid());

  // 2D closure with differing Z ordinates is valid.
  std::unique_ptr<LinearRing> ring_z = factory->createLinearRing(MakeSeqXYZ(
      {{0.0, 0.0, 1.0}, {2.0, 0.0, 2.0}, {0.0, 2.0, 3.0}, {0.0, 0.0, 99.0}}));
  EXPECT_TRUE(ring_z->isClosed());
  EXPECT_TRUE(ring_z->isValid());
}

TEST(LinearRingTest, CopyConstructorAndCloneAreDeepCopies) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LinearRing> orig = factory->createLinearRing(
      MakeSeqXY({{0.0, 0.0}, {2.0, 0.0}, {2.0, 2.0}, {0.0, 2.0}, {0.0, 0.0}}));
  orig->setSRID(3857);

  LinearRing copied(*orig);
  EXPECT_TRUE(copied.equalsIdentical(orig.get()));
  EXPECT_EQ(copied.getSRID(), 3857);
  EXPECT_EQ(copied.getGeometryTypeId(), geos::geom::GEOS_LINEARRING);

  std::unique_ptr<LinearRing> cloned = orig->clone();
  ASSERT_NE(cloned, nullptr);
  EXPECT_NE(cloned.get(), orig.get());
  EXPECT_TRUE(cloned->equalsIdentical(orig.get()));

  const Geometry* base_ptr = orig.get();
  Geometry::Ptr base_clone = base_ptr->clone();
  EXPECT_EQ(base_clone->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  EXPECT_TRUE(base_clone->equalsIdentical(orig.get()));

  const CoordinateSequence replacement{
      CoordinateXY(10.0, 10.0), CoordinateXY(11.0, 10.0),
      CoordinateXY(10.0, 11.0), CoordinateXY(10.0, 10.0)};
  orig->setPoints(&replacement);
  EXPECT_FALSE(copied.equalsExact(orig.get()));
  EXPECT_FALSE(cloned->equalsExact(orig.get()));
}

TEST(LinearRingTest, SetPointsCopiesSequenceAndAffectsIsClosed) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LinearRing> ring = factory->createLinearRing();
  EXPECT_TRUE(ring->isClosed());

  CoordinateSequence closed_seq{CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0),
                                CoordinateXY(1.0, 1.0), CoordinateXY(0.0, 1.0),
                                CoordinateXY(0.0, 0.0)};
  ring->setPoints(&closed_seq);
  EXPECT_EQ(ring->getNumPoints(), 5u);
  EXPECT_TRUE(ring->isClosed());
  EXPECT_DOUBLE_EQ(ring->getLength(), 4.0);

  // Mutating the source sequence must not alter the ring.
  closed_seq.setOrdinate(1, CoordinateSequence::X, 99.0);
  EXPECT_DOUBLE_EQ(ring->getCoordinateN(1).x, 1.0);

  // Non-empty isClosed() delegates to LineString::isClosed().
  const CoordinateSequence unclosed_seq{
      CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0), CoordinateXY(1.0, 1.0)};
  ring->setPoints(&unclosed_seq);
  EXPECT_FALSE(ring->isClosed());
}

TEST(LinearRingTest, OrientHandlesEmptyCcwAndCwRings) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<LinearRing> empty = factory->createLinearRing();
  empty->orient(true);
  empty->orient(false);
  EXPECT_TRUE(empty->isEmpty());
  EXPECT_TRUE(empty->isClosed());

  // CCW triangle: (0,0) -> (4,0) -> (0,3) -> (0,0).
  const CoordinateSequence ccw_seq{
      CoordinateXY(0.0, 0.0), CoordinateXY(4.0, 0.0), CoordinateXY(0.0, 3.0),
      CoordinateXY(0.0, 0.0)};
  std::unique_ptr<LinearRing> ring = factory->createLinearRing(ccw_seq);
  ASSERT_TRUE(Orientation::isCCW(ring->getCoordinatesRO()));

  // Requesting CCW on an already-CCW ring leaves vertices unchanged.
  ring->orient(false);
  EXPECT_TRUE(Orientation::isCCW(ring->getCoordinatesRO()));
  EXPECT_EQ(ring->getCoordinateN(1), CoordinateXY(4.0, 0.0));

  // Requesting CW on a CCW ring reverses it.
  ring->orient(true);
  EXPECT_FALSE(Orientation::isCCW(ring->getCoordinatesRO()));
  EXPECT_EQ(ring->getCoordinateN(1), CoordinateXY(0.0, 3.0));
  EXPECT_EQ(ring->getCoordinateN(2), CoordinateXY(4.0, 0.0));

  // Requesting CW on an already-CW ring leaves vertices unchanged.
  ring->orient(true);
  EXPECT_FALSE(Orientation::isCCW(ring->getCoordinatesRO()));
  EXPECT_EQ(ring->getCoordinateN(1), CoordinateXY(0.0, 3.0));

  // Requesting CCW on a CW ring reverses it back.
  ring->orient(false);
  EXPECT_TRUE(Orientation::isCCW(ring->getCoordinatesRO()));
  EXPECT_EQ(ring->getCoordinateN(1), CoordinateXY(4.0, 0.0));
  EXPECT_EQ(ring->getCoordinateN(2), CoordinateXY(0.0, 3.0));
}

TEST(LinearRingTest, ReversePreservesFactoryTypeAndDimensions) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<LinearRing> empty = factory->createLinearRing(true, true);
  std::unique_ptr<LinearRing> empty_rev = empty->reverse();
  ASSERT_NE(empty_rev, nullptr);
  EXPECT_TRUE(empty_rev->isEmpty());
  EXPECT_EQ(empty_rev->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  EXPECT_EQ(empty_rev->getFactory(), factory.get());

  std::unique_ptr<LinearRing> ring4d =
      factory->createLinearRing(MakeSeqXYZM({{0.0, 0.0, 1.0, 10.0},
                                             {4.0, 0.0, 2.0, 20.0},
                                             {0.0, 3.0, 3.0, 30.0},
                                             {0.0, 0.0, 1.0, 10.0}}));
  ASSERT_TRUE(Orientation::isCCW(ring4d->getCoordinatesRO()));

  std::unique_ptr<LinearRing> rev4d = ring4d->reverse();
  ASSERT_NE(rev4d, nullptr);
  EXPECT_EQ(rev4d->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  EXPECT_EQ(rev4d->getFactory(), factory.get());
  EXPECT_FALSE(Orientation::isCCW(rev4d->getCoordinatesRO()));
  EXPECT_EQ(rev4d->getNumPoints(), 4u);
  EXPECT_TRUE(rev4d->getCoordinatesRO()->getAt<CoordinateXYZM>(0).equals4D(
      CoordinateXYZM(0.0, 0.0, 1.0, 10.0)));
  EXPECT_TRUE(rev4d->getCoordinatesRO()->getAt<CoordinateXYZM>(1).equals4D(
      CoordinateXYZM(0.0, 3.0, 3.0, 30.0)));
  EXPECT_TRUE(rev4d->getCoordinatesRO()->getAt<CoordinateXYZM>(2).equals4D(
      CoordinateXYZM(4.0, 0.0, 2.0, 20.0)));
  EXPECT_TRUE(rev4d->getCoordinatesRO()->getAt<CoordinateXYZM>(3).equals4D(
      CoordinateXYZM(0.0, 0.0, 1.0, 10.0)));
  EXPECT_DOUBLE_EQ(rev4d->getLength(), ring4d->getLength());

  const Geometry* base_ptr = ring4d.get();
  Geometry::Ptr base_rev = base_ptr->reverse();
  EXPECT_EQ(base_rev->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  EXPECT_TRUE(base_rev->equalsIdentical(rev4d.get()));
}

TEST(LinearRingTest, BoundarySortIndexRingValidityAndNormalize) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<LinearRing> square = factory->createLinearRing(
      MakeSeqXY({{1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}, {0.0, 0.0}, {1.0, 0.0}}));
  EXPECT_EQ(square->getBoundaryDimension(), Dimension::False);
  EXPECT_TRUE(square->getBoundary()->isEmpty());
  EXPECT_TRUE(square->isRing());
  EXPECT_TRUE(square->isValid());
  EXPECT_DOUBLE_EQ(square->getLength(), 4.0);

  // Compare against LineString (SORTINDEX_LINESTRING < SORTINDEX_LINEARRING)
  // and Polygon (SORTINDEX_LINEARRING < SORTINDEX_POLYGON).
  std::unique_ptr<LineString> same_as_line =
      factory->createLineString(*square->getCoordinatesRO());
  std::unique_ptr<Polygon> as_poly = factory->createPolygon(square->clone());
  EXPECT_LT(same_as_line->compareTo(square.get()), 0);
  EXPECT_GT(square->compareTo(same_as_line.get()), 0);
  EXPECT_LT(square->compareTo(as_poly.get()), 0);
  EXPECT_GT(as_poly->compareTo(square.get()), 0);

  square->normalize();
  EXPECT_EQ(square->getCoordinateN(0), CoordinateXY(0.0, 0.0));
  EXPECT_FALSE(Orientation::isCCW(square->getCoordinatesRO()));

  // Self-intersecting bowtie ring is closed on construction, not a simple ring.
  std::unique_ptr<LinearRing> bowtie = factory->createLinearRing(
      MakeSeqXY({{0.0, 0.0}, {2.0, 2.0}, {2.0, 0.0}, {0.0, 2.0}, {0.0, 0.0}}));
  EXPECT_TRUE(bowtie->isClosed());
  EXPECT_FALSE(bowtie->isRing());
  EXPECT_FALSE(bowtie->isValid());
}

}  // namespace
}  // namespace geos_google
