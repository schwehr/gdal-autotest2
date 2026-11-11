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

// Unit tests for third_party/geos/src/geom/LineString.cpp and
// third_party/geos/include/geos/geom/LineString.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/LineString.cpp

#include "third_party/geos/include/geos/geom/LineString.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateFilter.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/CoordinateSequenceFilter.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateMutator;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateSequenceFilter;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYZM;
using geos::geom::Dimension;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::LineStringLT;
using geos::geom::Point;
using geos::util::IllegalArgumentException;

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

struct ShiftFilter : public CoordinateMutator<ShiftFilter> {
  double dx;
  double dy;
  ShiftFilter(double dx_in, double dy_in) : dx(dx_in), dy(dy_in) {}
  void filter(CoordinateXY* c) const {
    c->x += dx;
    c->y += dy;
  }
};

class ScaleSequenceFilter : public CoordinateSequenceFilter {
 public:
  explicit ScaleSequenceFilter(double factor) : factor_(factor) {}

  void filter_rw(CoordinateSequence& seq, std::size_t i) override {
    seq.setOrdinate(i, CoordinateSequence::X, seq.getX(i) * factor_);
    seq.setOrdinate(i, CoordinateSequence::Y, seq.getY(i) * factor_);
  }

  bool isDone() const override { return false; }
  bool isGeometryChanged() const override { return true; }

 private:
  double factor_;
};

TEST(LineStringTest, EmptyConstructionAndMetadata) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<LineString> empty_2d = factory->createLineString();
  ASSERT_NE(empty_2d, nullptr);
  EXPECT_TRUE(empty_2d->isEmpty());
  EXPECT_EQ(empty_2d->getNumPoints(), 0u);
  EXPECT_DOUBLE_EQ(empty_2d->getLength(), 0.0);
  EXPECT_EQ(empty_2d->getGeometryType(), "LineString");
  EXPECT_EQ(empty_2d->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_FALSE(empty_2d->isCurved());
  EXPECT_FALSE(empty_2d->hasCurvedComponents());
  EXPECT_EQ(empty_2d->getDimension(), Dimension::L);
  EXPECT_EQ(empty_2d->getBoundaryDimension(), 0);
  EXPECT_FALSE(empty_2d->isClosed());
  EXPECT_FALSE(empty_2d->hasZ());
  EXPECT_FALSE(empty_2d->hasM());
  EXPECT_EQ(empty_2d->getCoordinateDimension(), 2u);
  EXPECT_EQ(empty_2d->getCoordinate(), nullptr);
  EXPECT_EQ(empty_2d->getStartPoint(), nullptr);
  EXPECT_EQ(empty_2d->getEndPoint(), nullptr);
  EXPECT_TRUE(empty_2d->getBoundary()->isEmpty());

  std::unique_ptr<LineString> empty_3d = factory->createLineString(3);
  EXPECT_TRUE(empty_3d->isEmpty());
  EXPECT_TRUE(empty_3d->hasZ());
  EXPECT_FALSE(empty_3d->hasM());
  EXPECT_EQ(empty_3d->getCoordinateDimension(), 3u);

  std::unique_ptr<LineString> empty_xym =
      factory->createLineString(false, true);
  EXPECT_TRUE(empty_xym->isEmpty());
  EXPECT_FALSE(empty_xym->hasZ());
  EXPECT_TRUE(empty_xym->hasM());
  EXPECT_EQ(empty_xym->getCoordinateDimension(), 3u);

  std::unique_ptr<LineString> empty_xyzm =
      factory->createLineString(true, true);
  EXPECT_TRUE(empty_xyzm->isEmpty());
  EXPECT_TRUE(empty_xyzm->hasZ());
  EXPECT_TRUE(empty_xyzm->hasM());
  EXPECT_EQ(empty_xyzm->getCoordinateDimension(), 4u);

  CoordinateSequence::Ptr null_seq;
  std::unique_ptr<LineString> from_null =
      factory->createLineString(std::move(null_seq));
  ASSERT_NE(from_null, nullptr);
  EXPECT_TRUE(from_null->isEmpty());
  EXPECT_EQ(from_null->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);

  std::unique_ptr<LineString> from_empty_seq =
      factory->createLineString(MakeSeqXY({}));
  ASSERT_NE(from_empty_seq, nullptr);
  EXPECT_TRUE(from_empty_seq->isEmpty());
}

TEST(LineStringTest, RejectsSinglePointSequence) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  EXPECT_THROW(factory->createLineString(MakeSeqXY({{1.0, 2.0}})),
               IllegalArgumentException);

  const CoordinateSequence one_pt_seq{CoordinateXY(3.0, 4.0)};
  EXPECT_THROW(factory->createLineString(one_pt_seq), IllegalArgumentException);
}

TEST(LineStringTest, CopyAndCloneCreateIndependentDeepCopies) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LineString> orig =
      factory->createLineString(MakeSeqXYZ({{0.0, 0.0, 1.0}, {3.0, 4.0, 2.0}}));
  orig->setSRID(4326);

  std::unique_ptr<LineString> cloned = orig->clone();
  ASSERT_NE(cloned, nullptr);
  EXPECT_NE(cloned.get(), orig.get());
  EXPECT_EQ(cloned->getFactory(), factory.get());
  EXPECT_TRUE(cloned->equalsIdentical(orig.get()));

  std::unique_ptr<LineString> copied = factory->createLineString(*orig);
  ASSERT_NE(copied, nullptr);
  EXPECT_NE(copied.get(), orig.get());
  EXPECT_EQ(copied->getSRID(), 4326);
  EXPECT_TRUE(copied->equalsIdentical(orig.get()));

  const Geometry* base_ptr = orig.get();
  Geometry::Ptr base_clone = base_ptr->clone();
  EXPECT_EQ(base_clone->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(base_clone->equalsIdentical(orig.get()));

  const ShiftFilter shift(10.0, 0.0);
  orig->apply_rw(&shift);
  orig->geometryChanged();
  EXPECT_FALSE(cloned->equalsExact(orig.get()));
  EXPECT_FALSE(copied->equalsExact(orig.get()));
}

TEST(LineStringTest, GetLengthAcrossCases) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  {
    std::unique_ptr<LineString> collapsed =
        factory->createLineString(MakeSeqXY({{1.0, 2.0}, {1.0, 2.0}}));
    EXPECT_DOUBLE_EQ(collapsed->getLength(), 0.0);
    EXPECT_FALSE(collapsed->isValid());
    EXPECT_TRUE(std::isnan(collapsed->getPointN(0)->getZ()));
  }

  {
    std::unique_ptr<LineString> path = factory->createLineString(
        MakeSeqXYZ({{0.0, 0.0, 10.0}, {3.0, 4.0, 99.0}, {3.0, 10.0, -5.0}}));
    EXPECT_DOUBLE_EQ(path->getLength(), 11.0);
    EXPECT_TRUE(path->isValid());
    EXPECT_DOUBLE_EQ(path->getPointN(1)->getZ(), 99.0);
    CoordinateSequence::Ptr coords_copy = path->getCoordinates();
    ASSERT_NE(coords_copy, nullptr);
    EXPECT_EQ(coords_copy->size(), 3u);
    std::unique_ptr<LineString> from_copy =
        factory->createLineString(*coords_copy);
    EXPECT_TRUE(path->equalsExact(from_copy.get(), 1e-12));
  }
}

TEST(LineStringTest, ReversePreservesFactoryAndReversesAllDimensions) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<LineString> empty = factory->createLineString(true, true);
  std::unique_ptr<LineString> empty_rev = empty->reverse();
  ASSERT_NE(empty_rev, nullptr);
  EXPECT_TRUE(empty_rev->isEmpty());
  EXPECT_EQ(empty_rev->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_EQ(empty_rev->getFactory(), factory.get());

  std::unique_ptr<LineString> line4d =
      factory->createLineString(MakeSeqXYZM({{1.0, 2.0, 10.0, 100.0},
                                             {4.0, 6.0, 20.0, 200.0},
                                             {7.0, 9.0, 30.0, 300.0}}));
  std::unique_ptr<LineString> rev4d = line4d->reverse();
  ASSERT_NE(rev4d, nullptr);
  EXPECT_EQ(rev4d->getFactory(), factory.get());
  EXPECT_EQ(rev4d->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_EQ(rev4d->getNumPoints(), 3u);
  EXPECT_TRUE(rev4d->getCoordinatesRO()->getAt<CoordinateXYZM>(0).equals4D(
      CoordinateXYZM(7.0, 9.0, 30.0, 300.0)));
  EXPECT_TRUE(rev4d->getCoordinatesRO()->getAt<CoordinateXYZM>(1).equals4D(
      CoordinateXYZM(4.0, 6.0, 20.0, 200.0)));
  EXPECT_TRUE(rev4d->getCoordinatesRO()->getAt<CoordinateXYZM>(2).equals4D(
      CoordinateXYZM(1.0, 2.0, 10.0, 100.0)));
  EXPECT_DOUBLE_EQ(rev4d->getLength(), line4d->getLength());

  const Geometry* base_ptr = line4d.get();
  Geometry::Ptr base_rev = base_ptr->reverse();
  EXPECT_EQ(base_rev->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(base_rev->equalsIdentical(rev4d.get()));
}

TEST(LineStringTest, GeometryChangedActionRecomputesEnvelope) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LineString> line =
      factory->createLineString(MakeSeqXY({{0.0, 0.0}, {2.0, 3.0}}));

  const Envelope* env = line->getEnvelopeInternal();
  EXPECT_DOUBLE_EQ(env->getMinX(), 0.0);
  EXPECT_DOUBLE_EQ(env->getMaxX(), 2.0);
  EXPECT_DOUBLE_EQ(env->getMinY(), 0.0);
  EXPECT_DOUBLE_EQ(env->getMaxY(), 3.0);

  const ShiftFilter shift(10.0, -5.0);
  line->apply_rw(&shift);
  line->geometryChanged();

  EXPECT_DOUBLE_EQ(env->getMinX(), 10.0);
  EXPECT_DOUBLE_EQ(env->getMaxX(), 12.0);
  EXPECT_DOUBLE_EQ(env->getMinY(), -5.0);
  EXPECT_DOUBLE_EQ(env->getMaxY(), -2.0);

  ScaleSequenceFilter scale(2.0);
  line->apply_rw(scale);
  EXPECT_DOUBLE_EQ(env->getMinX(), 20.0);
  EXPECT_DOUBLE_EQ(env->getMaxX(), 24.0);
  EXPECT_DOUBLE_EQ(env->getMinY(), -10.0);
  EXPECT_DOUBLE_EQ(env->getMaxY(), -4.0);
}

TEST(LineStringTest, SortIndexAndLineStringLT) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(0.0, 0.0));
  std::unique_ptr<LineString> a =
      factory->createLineString(MakeSeqXY({{0.0, 0.0}, {1.0, 0.0}}));
  std::unique_ptr<LineString> b =
      factory->createLineString(MakeSeqXY({{0.0, 0.0}, {2.0, 0.0}}));
  std::unique_ptr<LineString> c = factory->createLineString(
      MakeSeqXY({{0.0, 0.0}, {1.0, 0.0}, {2.0, 0.0}}));
  std::unique_ptr<LinearRing> ring = factory->createLinearRing(
      MakeSeqXY({{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}, {0.0, 0.0}}));

  EXPECT_LT(pt->compareTo(a.get()), 0);
  EXPECT_GT(a->compareTo(pt.get()), 0);
  EXPECT_LT(a->compareTo(ring.get()), 0);
  EXPECT_GT(ring->compareTo(a.get()), 0);

  LineStringLT less;
  EXPECT_TRUE(less(a.get(), b.get()));
  EXPECT_FALSE(less(b.get(), a.get()));
  EXPECT_FALSE(less(a.get(), a.get()));
  EXPECT_TRUE(less(b.get(), c.get()));

  std::vector<const LineString*> lines = {c.get(), b.get(), a.get()};
  std::sort(lines.begin(), lines.end(), LineStringLT{});
  EXPECT_EQ(lines[0], a.get());
  EXPECT_EQ(lines[1], b.get());
  EXPECT_EQ(lines[2], c.get());
}

TEST(LineStringTest, CurveAndBoundaryContracts) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<LineString> open = factory->createLineString(
      MakeSeqXY({{5.0, 5.0}, {1.0, 1.0}, {0.0, 0.0}}));
  EXPECT_FALSE(open->isClosed());
  EXPECT_FALSE(open->isRing());
  EXPECT_TRUE(open->isSimple());
  EXPECT_EQ(open->getBoundaryDimension(), 0);
  EXPECT_EQ(open->getNumCurves(), 1u);
  EXPECT_EQ(open->getCurveN(0), open.get());
  EXPECT_EQ(*open->getCoordinate(), CoordinateXY(5.0, 5.0));
  EXPECT_EQ(*open->getStartPoint()->getCoordinate(), CoordinateXY(5.0, 5.0));
  EXPECT_EQ(*open->getEndPoint()->getCoordinate(), CoordinateXY(0.0, 0.0));

  CoordinateXY middle(1.0, 1.0);
  CoordinateXY absent(9.0, 9.0);
  EXPECT_TRUE(open->isCoordinate(middle));
  EXPECT_FALSE(open->isCoordinate(absent));

  Geometry::Ptr boundary = open->getBoundary();
  EXPECT_EQ(boundary->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(boundary->getNumGeometries(), 2u);

  open->normalize();
  EXPECT_EQ(open->getCoordinateN(0), CoordinateXY(0.0, 0.0));
  EXPECT_EQ(open->getCoordinateN(2), CoordinateXY(5.0, 5.0));

  std::unique_ptr<LineString> closed = factory->createLineString(
      MakeSeqXY({{0.0, 0.0}, {2.0, 0.0}, {0.0, 2.0}, {0.0, 0.0}}));
  EXPECT_TRUE(closed->isClosed());
  EXPECT_TRUE(closed->isRing());
  EXPECT_EQ(closed->getBoundaryDimension(), Dimension::False);
  EXPECT_TRUE(closed->getBoundary()->isEmpty());

  CoordinateSequence::Ptr released = closed->releaseCoordinates();
  ASSERT_NE(released, nullptr);
  EXPECT_EQ(released->size(), 4u);
  EXPECT_TRUE(closed->isEmpty());
}

}  // namespace
}  // namespace geos_google
