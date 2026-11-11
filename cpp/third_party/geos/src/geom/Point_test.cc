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

// Unit tests for third_party/geos/src/geom/Point.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/Point.cpp

#include "third_party/geos/include/geos/geom/Point.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateFilter.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/CoordinateSequenceFilter.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryComponentFilter.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/GeometryFilter.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::DoubleNotANumber;
using geos::geom::Coordinate;
using geos::geom::CoordinateInspector;
using geos::geom::CoordinateMutator;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateSequenceFilter;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Dimension;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryComponentFilter;
using geos::geom::GeometryFactory;
using geos::geom::GeometryFilter;
using geos::geom::LineString;
using geos::geom::MultiPoint;
using geos::geom::Point;
using geos::util::IllegalArgumentException;
using geos::util::UnsupportedOperationException;

class InspectCoordinateFilter final
    : public CoordinateInspector<InspectCoordinateFilter> {
 public:
  template <typename CoordT>
  void filter(const CoordT* coord) {
    visited_.emplace_back(coord->x, coord->y);
  }

  const std::vector<CoordinateXY>& visited() const { return visited_; }

 private:
  std::vector<CoordinateXY> visited_;
};

class ShiftCoordinateFilter final
    : public CoordinateMutator<ShiftCoordinateFilter> {
 public:
  ShiftCoordinateFilter(double dx, double dy) : dx_(dx), dy_(dy) {}

  template <typename CoordT>
  void filter(CoordT* coord) const {
    coord->x += dx_;
    coord->y += dy_;
    ++calls_;
  }

  int calls() const { return calls_; }

 private:
  double dx_;
  double dy_;
  mutable int calls_ = 0;
};

class RecordingGeometryFilter final : public GeometryFilter {
 public:
  void filter_ro(const Geometry* geom) override { ro_geoms_.push_back(geom); }

  void filter_rw(Geometry* geom) override { rw_geoms_.push_back(geom); }

  const std::vector<const Geometry*>& ro_geoms() const { return ro_geoms_; }
  const std::vector<Geometry*>& rw_geoms() const { return rw_geoms_; }

 private:
  std::vector<const Geometry*> ro_geoms_;
  std::vector<Geometry*> rw_geoms_;
};

class RecordingComponentFilter final : public GeometryComponentFilter {
 public:
  void filter_ro(const Geometry* geom) override {
    ro_components_.push_back(geom);
  }

  void filter_rw(Geometry* geom) override { rw_components_.push_back(geom); }

  const std::vector<const Geometry*>& ro_components() const {
    return ro_components_;
  }
  const std::vector<Geometry*>& rw_components() const { return rw_components_; }

 private:
  std::vector<const Geometry*> ro_components_;
  std::vector<Geometry*> rw_components_;
};

class RecordingSequenceFilter final : public CoordinateSequenceFilter {
 public:
  explicit RecordingSequenceFilter(bool mutate = false) : mutate_(mutate) {}

  void filter_ro(const CoordinateSequence& seq, std::size_t i) override {
    indices_.push_back(i);
    coords_.emplace_back(seq.getX(i), seq.getY(i));
  }

  void filter_rw(CoordinateSequence& seq, std::size_t i) override {
    indices_.push_back(i);
    coords_.emplace_back(seq.getX(i), seq.getY(i));
    if (mutate_) {
      seq.setOrdinate(i, CoordinateSequence::X, seq.getX(i) + 10.0);
      seq.setOrdinate(i, CoordinateSequence::Y, seq.getY(i) - 5.0);
      geometry_changed_ = true;
    }
  }

  bool isDone() const override { return false; }
  bool isGeometryChanged() const override { return geometry_changed_; }

  const std::vector<std::size_t>& indices() const { return indices_; }
  const std::vector<CoordinateXY>& coords() const { return coords_; }

 private:
  bool mutate_;
  bool geometry_changed_ = false;
  std::vector<std::size_t> indices_;
  std::vector<CoordinateXY> coords_;
};

TEST(PointTest, EmptyPointPropertiesAndThrows) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> pt = factory->createPoint();
  ASSERT_NE(pt, nullptr);

  EXPECT_TRUE(pt->isEmpty());
  EXPECT_TRUE(pt->isSimple());
  EXPECT_EQ(pt->getNumPoints(), 0u);
  EXPECT_EQ(pt->getDimension(), Dimension::P);
  EXPECT_EQ(pt->getCoordinateDimension(), 2u);
  EXPECT_FALSE(pt->hasZ());
  EXPECT_FALSE(pt->hasM());
  EXPECT_EQ(pt->getBoundaryDimension(), Dimension::False);
  EXPECT_EQ(pt->getGeometryType(), "Point");
  EXPECT_EQ(pt->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_EQ(pt->getCoordinate(), nullptr);
  EXPECT_TRUE(pt->getEnvelopeInternal()->isNull());

  const CoordinateSequence* ro_seq = pt->getCoordinatesRO();
  ASSERT_NE(ro_seq, nullptr);
  EXPECT_TRUE(ro_seq->isEmpty());
  EXPECT_EQ(ro_seq->size(), 0u);

  std::unique_ptr<CoordinateSequence> seq = pt->getCoordinates();
  ASSERT_NE(seq, nullptr);
  EXPECT_TRUE(seq->isEmpty());
  EXPECT_EQ(seq->size(), 0u);

  EXPECT_THROW(pt->getX(), UnsupportedOperationException);
  EXPECT_THROW(pt->getY(), UnsupportedOperationException);
  EXPECT_THROW(pt->getZ(), UnsupportedOperationException);
  EXPECT_THROW(pt->getM(), UnsupportedOperationException);
}

TEST(PointTest, EmptyPointDimensionVariants) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  struct DimCase {
    bool has_z;
    bool has_m;
    std::uint8_t expected_dim;
  };
  constexpr DimCase cases[] = {
      {.has_z = false, .has_m = false, .expected_dim = 2},
      {.has_z = true, .has_m = false, .expected_dim = 3},
      {.has_z = false, .has_m = true, .expected_dim = 3},
      {.has_z = true, .has_m = true, .expected_dim = 4},
  };

  for (const auto& tc : cases) {
    std::unique_ptr<Point> pt = factory->createPoint(tc.has_z, tc.has_m);
    ASSERT_NE(pt, nullptr);
    EXPECT_TRUE(pt->isEmpty());
    EXPECT_EQ(pt->hasZ(), tc.has_z);
    EXPECT_EQ(pt->hasM(), tc.has_m);
    EXPECT_EQ(pt->getCoordinateDimension(), tc.expected_dim);
  }

  for (std::size_t dim : {std::size_t{2}, std::size_t{3}, std::size_t{4}}) {
    std::unique_ptr<Point> pt = factory->createPoint(dim);
    ASSERT_NE(pt, nullptr);
    EXPECT_TRUE(pt->isEmpty());
    EXPECT_EQ(pt->getCoordinateDimension(), dim);
  }

  std::unique_ptr<Point> from_null_seq =
      factory->createPoint(std::unique_ptr<CoordinateSequence>(nullptr));
  ASSERT_NE(from_null_seq, nullptr);
  EXPECT_TRUE(from_null_seq->isEmpty());
  EXPECT_EQ(from_null_seq->getCoordinateDimension(), 2u);
}

TEST(PointTest, ConstructFromCoordinateTypes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  {
    std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(1.5, -2.5));
    EXPECT_FALSE(pt->isEmpty());
    EXPECT_EQ(pt->getNumPoints(), 1u);
    EXPECT_EQ(pt->getCoordinateDimension(), 2u);
    EXPECT_FALSE(pt->hasZ());
    EXPECT_FALSE(pt->hasM());
    EXPECT_DOUBLE_EQ(pt->getX(), 1.5);
    EXPECT_DOUBLE_EQ(pt->getY(), -2.5);
    EXPECT_TRUE(std::isnan(pt->getZ()));
    EXPECT_TRUE(std::isnan(pt->getM()));
  }

  {
    std::unique_ptr<Point> pt = factory->createPoint(Coordinate(3.0, 4.0));
    EXPECT_EQ(pt->getCoordinateDimension(), 2u);
    EXPECT_FALSE(pt->hasZ());
    EXPECT_FALSE(pt->hasM());
    EXPECT_DOUBLE_EQ(pt->getX(), 3.0);
    EXPECT_DOUBLE_EQ(pt->getY(), 4.0);
    EXPECT_TRUE(std::isnan(pt->getZ()));
    EXPECT_TRUE(std::isnan(pt->getM()));
  }

  {
    std::unique_ptr<Point> pt =
        factory->createPoint(Coordinate(3.0, 4.0, 5.25));
    EXPECT_EQ(pt->getCoordinateDimension(), 3u);
    EXPECT_TRUE(pt->hasZ());
    EXPECT_FALSE(pt->hasM());
    EXPECT_DOUBLE_EQ(pt->getX(), 3.0);
    EXPECT_DOUBLE_EQ(pt->getY(), 4.0);
    EXPECT_DOUBLE_EQ(pt->getZ(), 5.25);
    EXPECT_TRUE(std::isnan(pt->getM()));
  }

  {
    std::unique_ptr<Point> pt =
        factory->createPoint(CoordinateXYM(6.0, 7.0, 8.5));
    EXPECT_EQ(pt->getCoordinateDimension(), 3u);
    EXPECT_FALSE(pt->hasZ());
    EXPECT_TRUE(pt->hasM());
    EXPECT_DOUBLE_EQ(pt->getX(), 6.0);
    EXPECT_DOUBLE_EQ(pt->getY(), 7.0);
    EXPECT_TRUE(std::isnan(pt->getZ()));
    EXPECT_DOUBLE_EQ(pt->getM(), 8.5);
  }
}

TEST(PointTest, ConstructFromCoordinateXYZMInfersZAndM) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  {
    std::unique_ptr<Point> pt =
        factory->createPoint(CoordinateXYZM(1.0, 2.0, 3.0, 4.0));
    EXPECT_EQ(pt->getCoordinateDimension(), 4u);
    EXPECT_TRUE(pt->hasZ());
    EXPECT_TRUE(pt->hasM());
    EXPECT_DOUBLE_EQ(pt->getZ(), 3.0);
    EXPECT_DOUBLE_EQ(pt->getM(), 4.0);
  }

  {
    std::unique_ptr<Point> pt =
        factory->createPoint(CoordinateXYZM(1.0, 2.0, DoubleNotANumber, 4.0));
    EXPECT_EQ(pt->getCoordinateDimension(), 3u);
    EXPECT_FALSE(pt->hasZ());
    EXPECT_TRUE(pt->hasM());
    EXPECT_TRUE(std::isnan(pt->getZ()));
    EXPECT_DOUBLE_EQ(pt->getM(), 4.0);
  }

  {
    std::unique_ptr<Point> pt =
        factory->createPoint(CoordinateXYZM(1.0, 2.0, 3.0, DoubleNotANumber));
    EXPECT_EQ(pt->getCoordinateDimension(), 3u);
    EXPECT_TRUE(pt->hasZ());
    EXPECT_FALSE(pt->hasM());
    EXPECT_DOUBLE_EQ(pt->getZ(), 3.0);
    EXPECT_TRUE(std::isnan(pt->getM()));
  }

  {
    std::unique_ptr<Point> pt = factory->createPoint(
        CoordinateXYZM(1.0, 2.0, DoubleNotANumber, DoubleNotANumber));
    EXPECT_EQ(pt->getCoordinateDimension(), 2u);
    EXPECT_FALSE(pt->hasZ());
    EXPECT_FALSE(pt->hasM());
    EXPECT_TRUE(std::isnan(pt->getZ()));
    EXPECT_TRUE(std::isnan(pt->getM()));
  }
}

TEST(PointTest, ConstructFromCoordinateSequenceAndRejectMultiPointSequence) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  CoordinateSequence single{CoordinateXYZM{9.0, -3.0, 1.5, 2.5}};
  std::unique_ptr<Point> pt = factory->createPoint(single);
  ASSERT_NE(pt, nullptr);
  EXPECT_EQ(pt->getNumPoints(), 1u);
  EXPECT_EQ(pt->getCoordinateDimension(), 4u);
  EXPECT_DOUBLE_EQ(pt->getX(), 9.0);
  EXPECT_DOUBLE_EQ(pt->getY(), -3.0);
  EXPECT_DOUBLE_EQ(pt->getZ(), 1.5);
  EXPECT_DOUBLE_EQ(pt->getM(), 2.5);

  auto owned_seq = std::make_unique<CoordinateSequence>(
      CoordinateSequence{CoordinateXY{4.0, 5.0}});
  std::unique_ptr<Point> pt_moved = factory->createPoint(std::move(owned_seq));
  EXPECT_DOUBLE_EQ(pt_moved->getX(), 4.0);
  EXPECT_DOUBLE_EQ(pt_moved->getY(), 5.0);

  CoordinateSequence two_coords{CoordinateXY{1.0, 2.0}, CoordinateXY{3.0, 4.0}};
  EXPECT_THROW(factory->createPoint(two_coords), IllegalArgumentException);

  auto owned_two = std::make_unique<CoordinateSequence>(two_coords);
  EXPECT_THROW(factory->createPoint(std::move(owned_two)),
               IllegalArgumentException);
}

TEST(PointTest, CoordinatesEnvelopeBoundaryAndSetXY) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> pt =
      factory->createPoint(CoordinateXYZM(2.0, -4.0, 10.0, 20.0));

  const CoordinateXY* coord = pt->getCoordinate();
  ASSERT_NE(coord, nullptr);
  EXPECT_DOUBLE_EQ(coord->x, 2.0);
  EXPECT_DOUBLE_EQ(coord->y, -4.0);

  const Envelope* env = pt->getEnvelopeInternal();
  ASSERT_FALSE(env->isNull());
  EXPECT_DOUBLE_EQ(env->getMinX(), 2.0);
  EXPECT_DOUBLE_EQ(env->getMaxX(), 2.0);
  EXPECT_DOUBLE_EQ(env->getMinY(), -4.0);
  EXPECT_DOUBLE_EQ(env->getMaxY(), -4.0);

  std::unique_ptr<CoordinateSequence> copied_coords = pt->getCoordinates();
  ASSERT_EQ(copied_coords->size(), 1u);
  copied_coords->setOrdinate(0, CoordinateSequence::X, 999.0);
  EXPECT_DOUBLE_EQ(pt->getX(), 2.0);
  EXPECT_DOUBLE_EQ(pt->getCoordinatesRO()->getX(0), 2.0);

  std::unique_ptr<Geometry> boundary = pt->getBoundary();
  ASSERT_NE(boundary, nullptr);
  ASSERT_NE(dynamic_cast<const GeometryCollection*>(boundary.get()), nullptr);
  EXPECT_EQ(boundary->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_TRUE(boundary->isEmpty());
  EXPECT_EQ(boundary->getNumGeometries(), 0u);

  std::unique_ptr<Geometry> empty_boundary =
      factory->createPoint()->getBoundary();
  EXPECT_EQ(empty_boundary->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_TRUE(empty_boundary->isEmpty());

  pt->setXY(-8.5, 12.25);
  EXPECT_DOUBLE_EQ(pt->getX(), -8.5);
  EXPECT_DOUBLE_EQ(pt->getY(), 12.25);
  EXPECT_DOUBLE_EQ(pt->getZ(), 10.0);
  EXPECT_DOUBLE_EQ(pt->getM(), 20.0);
  EXPECT_DOUBLE_EQ(pt->getEnvelopeInternal()->getMinX(), -8.5);
  EXPECT_DOUBLE_EQ(pt->getEnvelopeInternal()->getMaxY(), 12.25);

  std::unique_ptr<Point> empty_pt = factory->createPoint();
  EXPECT_TRUE(empty_pt->isEmpty());
  empty_pt->setXY(3.25, 4.75);
  EXPECT_FALSE(empty_pt->isEmpty());
  EXPECT_EQ(empty_pt->getNumPoints(), 1u);
  EXPECT_DOUBLE_EQ(empty_pt->getX(), 3.25);
  EXPECT_DOUBLE_EQ(empty_pt->getY(), 4.75);
  EXPECT_DOUBLE_EQ(empty_pt->getEnvelopeInternal()->getMinX(), 3.25);
  EXPECT_DOUBLE_EQ(empty_pt->getEnvelopeInternal()->getMaxY(), 4.75);
}

TEST(PointTest, CloneReverseAndNormalize) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> pt =
      factory->createPoint(CoordinateXYZM(1.0, 2.0, 3.0, 4.0));
  int user_tag = 42;
  pt->setSRID(4326);
  pt->setUserData(&user_tag);

  std::unique_ptr<Point> cloned = pt->clone();
  ASSERT_NE(cloned, nullptr);
  EXPECT_NE(cloned.get(), pt.get());
  EXPECT_TRUE(cloned->equalsIdentical(pt.get()));
  EXPECT_EQ(cloned->getSRID(), 4326);
  EXPECT_EQ(cloned->getUserData(), nullptr);

  cloned->setXY(99.0, 99.0);
  EXPECT_DOUBLE_EQ(pt->getX(), 1.0);
  EXPECT_DOUBLE_EQ(pt->getY(), 2.0);

  const Geometry* as_geom = pt.get();
  std::unique_ptr<Geometry> poly_clone = as_geom->clone();
  EXPECT_EQ(poly_clone->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_TRUE(poly_clone->equalsIdentical(pt.get()));

  std::unique_ptr<Point> reversed = pt->reverse();
  ASSERT_NE(reversed, nullptr);
  EXPECT_NE(reversed.get(), pt.get());
  EXPECT_TRUE(reversed->equalsIdentical(pt.get()));

  std::unique_ptr<Geometry> poly_reversed = as_geom->reverse();
  EXPECT_EQ(poly_reversed->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_TRUE(poly_reversed->equalsIdentical(pt.get()));

  pt->normalize();
  EXPECT_DOUBLE_EQ(pt->getX(), 1.0);
  EXPECT_DOUBLE_EQ(pt->getY(), 2.0);
  EXPECT_DOUBLE_EQ(pt->getZ(), 3.0);
  EXPECT_DOUBLE_EQ(pt->getM(), 4.0);

  std::unique_ptr<Point> empty_pt = factory->createPoint();
  std::unique_ptr<Point> empty_rev = empty_pt->reverse();
  EXPECT_TRUE(empty_rev->isEmpty());
  EXPECT_NE(empty_rev.get(), empty_pt.get());
  empty_pt->normalize();
  EXPECT_TRUE(empty_pt->isEmpty());
}

TEST(PointTest, CoordinateAndGeometryFilters) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(2.0, 3.0));
  std::unique_ptr<Point> empty_pt = factory->createPoint();

  InspectCoordinateFilter ro_empty_coord_filter;
  empty_pt->apply_ro(&ro_empty_coord_filter);
  EXPECT_TRUE(ro_empty_coord_filter.visited().empty());

  InspectCoordinateFilter ro_coord_filter;
  pt->apply_ro(&ro_coord_filter);
  ASSERT_EQ(ro_coord_filter.visited().size(), 1u);
  EXPECT_DOUBLE_EQ(ro_coord_filter.visited()[0].x, 2.0);
  EXPECT_DOUBLE_EQ(ro_coord_filter.visited()[0].y, 3.0);

  ShiftCoordinateFilter rw_coord_filter(5.0, -7.0);
  empty_pt->apply_rw(&rw_coord_filter);
  EXPECT_EQ(rw_coord_filter.calls(), 0);
  pt->apply_rw(&rw_coord_filter);
  EXPECT_EQ(rw_coord_filter.calls(), 1);
  EXPECT_DOUBLE_EQ(pt->getX(), 7.0);
  EXPECT_DOUBLE_EQ(pt->getY(), -4.0);
  pt->geometryChanged();
  EXPECT_DOUBLE_EQ(pt->getEnvelopeInternal()->getMinX(), 7.0);
  EXPECT_DOUBLE_EQ(pt->getEnvelopeInternal()->getMaxY(), -4.0);

  RecordingGeometryFilter geom_filter;
  const Point* const_pt = pt.get();
  const_pt->apply_ro(&geom_filter);
  pt->apply_rw(&geom_filter);
  ASSERT_EQ(geom_filter.ro_geoms().size(), 1u);
  EXPECT_EQ(geom_filter.ro_geoms()[0], pt.get());
  ASSERT_EQ(geom_filter.rw_geoms().size(), 1u);
  EXPECT_EQ(geom_filter.rw_geoms()[0], pt.get());

  RecordingComponentFilter comp_filter;
  const_pt->apply_ro(&comp_filter);
  pt->apply_rw(&comp_filter);
  ASSERT_EQ(comp_filter.ro_components().size(), 1u);
  EXPECT_EQ(comp_filter.ro_components()[0], pt.get());
  ASSERT_EQ(comp_filter.rw_components().size(), 1u);
  EXPECT_EQ(comp_filter.rw_components()[0], pt.get());
}

TEST(PointTest, CoordinateSequenceFilters) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(1.0, 2.0));
  std::unique_ptr<Point> empty_pt = factory->createPoint();

  RecordingSequenceFilter ro_empty_filter;
  empty_pt->apply_ro(ro_empty_filter);
  EXPECT_TRUE(ro_empty_filter.indices().empty());

  RecordingSequenceFilter rw_empty_filter(/*mutate=*/true);
  empty_pt->apply_rw(rw_empty_filter);
  EXPECT_TRUE(rw_empty_filter.indices().empty());

  RecordingSequenceFilter ro_filter;
  const Point* const_pt = pt.get();
  const_pt->apply_ro(ro_filter);
  ASSERT_EQ(ro_filter.indices().size(), 1u);
  EXPECT_EQ(ro_filter.indices()[0], 0u);
  EXPECT_DOUBLE_EQ(ro_filter.coords()[0].x, 1.0);
  EXPECT_DOUBLE_EQ(ro_filter.coords()[0].y, 2.0);

  RecordingSequenceFilter rw_noop_filter(/*mutate=*/false);
  pt->apply_rw(rw_noop_filter);
  ASSERT_EQ(rw_noop_filter.indices().size(), 1u);
  EXPECT_FALSE(rw_noop_filter.isGeometryChanged());
  EXPECT_DOUBLE_EQ(pt->getX(), 1.0);
  EXPECT_DOUBLE_EQ(pt->getY(), 2.0);

  RecordingSequenceFilter rw_mut_filter(/*mutate=*/true);
  pt->apply_rw(rw_mut_filter);
  ASSERT_EQ(rw_mut_filter.indices().size(), 1u);
  EXPECT_TRUE(rw_mut_filter.isGeometryChanged());
  EXPECT_DOUBLE_EQ(pt->getX(), 11.0);
  EXPECT_DOUBLE_EQ(pt->getY(), -3.0);
  EXPECT_DOUBLE_EQ(pt->getEnvelopeInternal()->getMinX(), 11.0);
  EXPECT_DOUBLE_EQ(pt->getEnvelopeInternal()->getMaxY(), -3.0);
}

TEST(PointTest, EqualsExactHandlesTypesEmptinessAndTolerance) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> empty1 = factory->createPoint();
  std::unique_ptr<Point> empty2 = factory->createPoint(3);
  std::unique_ptr<Point> p0 = factory->createPoint(CoordinateXY(0.0, 0.0));
  std::unique_ptr<Point> p0_z =
      factory->createPoint(Coordinate(0.0, 0.0, 42.0));
  std::unique_ptr<Point> p34 = factory->createPoint(CoordinateXY(3.0, 4.0));
  std::unique_ptr<LineString> line = factory->createLineString();
  std::unique_ptr<MultiPoint> mp = factory->createMultiPoint();

  EXPECT_FALSE(p0->equalsExact(line.get()));
  EXPECT_FALSE(p0->equalsExact(mp.get()));
  EXPECT_FALSE(empty1->equalsExact(mp.get()));

  EXPECT_TRUE(empty1->equalsExact(empty2.get()));
  EXPECT_FALSE(empty1->equalsExact(p0.get()));
  EXPECT_FALSE(p0->equalsExact(empty1.get()));

  EXPECT_TRUE(p0->equalsExact(p0_z.get()));
  EXPECT_FALSE(p0->equalsExact(p34.get()));
  EXPECT_TRUE(p0->equalsExact(p34.get(), 5.0));
  EXPECT_FALSE(p0->equalsExact(p34.get(), 4.999999));
}

TEST(PointTest, EqualsIdenticalChecksAllDimensions) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> empty_xy = factory->createPoint(false, false);
  std::unique_ptr<Point> empty_xy2 = factory->createPoint(false, false);
  std::unique_ptr<Point> empty_xyz = factory->createPoint(true, false);
  std::unique_ptr<LineString> empty_line = factory->createLineString();

  EXPECT_FALSE(empty_xy->equalsIdentical(empty_line.get()));
  EXPECT_TRUE(empty_xy->equalsIdentical(empty_xy2.get()));
  EXPECT_FALSE(empty_xy->equalsIdentical(empty_xyz.get()));

  std::unique_ptr<Point> p_xy = factory->createPoint(CoordinateXY(1.0, 2.0));
  std::unique_ptr<Point> p_xy_same =
      factory->createPoint(CoordinateXY(1.0, 2.0));
  std::unique_ptr<Point> p_xyz_a =
      factory->createPoint(Coordinate(1.0, 2.0, 3.0));
  std::unique_ptr<Point> p_xyz_b =
      factory->createPoint(Coordinate(1.0, 2.0, 3.0));
  std::unique_ptr<Point> p_xyz_diff =
      factory->createPoint(Coordinate(1.0, 2.0, 4.0));
  std::unique_ptr<Point> p_xym_a =
      factory->createPoint(CoordinateXYM(1.0, 2.0, 5.0));
  std::unique_ptr<Point> p_xym_diff =
      factory->createPoint(CoordinateXYM(1.0, 2.0, 6.0));
  std::unique_ptr<Point> p_xyzm_a =
      factory->createPoint(CoordinateXYZM(1.0, 2.0, 3.0, 5.0));
  std::unique_ptr<Point> p_xyzm_b =
      factory->createPoint(CoordinateXYZM(1.0, 2.0, 3.0, 5.0));
  std::unique_ptr<Point> p_xyzm_diff_m =
      factory->createPoint(CoordinateXYZM(1.0, 2.0, 3.0, 9.0));

  CoordinateSequence seq_nan_z1(1u, true, false);
  seq_nan_z1.setAt(Coordinate(1.0, 2.0, DoubleNotANumber), 0);
  CoordinateSequence seq_nan_z2(1u, true, false);
  seq_nan_z2.setAt(Coordinate(1.0, 2.0, DoubleNotANumber), 0);
  std::unique_ptr<Point> p_nan_z1 = factory->createPoint(seq_nan_z1);
  std::unique_ptr<Point> p_nan_z2 = factory->createPoint(seq_nan_z2);

  EXPECT_TRUE(p_xy->equalsIdentical(p_xy.get()));
  EXPECT_TRUE(p_xy->equalsIdentical(p_xy_same.get()));
  EXPECT_FALSE(p_xy->equalsIdentical(p_xyz_a.get()));
  EXPECT_TRUE(p_xyz_a->equalsIdentical(p_xyz_b.get()));
  EXPECT_FALSE(p_xyz_a->equalsIdentical(p_xyz_diff.get()));
  EXPECT_TRUE(p_nan_z1->equalsIdentical(p_nan_z2.get()));
  EXPECT_FALSE(p_nan_z1->equalsIdentical(p_xyz_a.get()));
  EXPECT_FALSE(p_xym_a->equalsIdentical(p_xym_diff.get()));
  EXPECT_TRUE(p_xyzm_a->equalsIdentical(p_xyzm_b.get()));
  EXPECT_FALSE(p_xyzm_a->equalsIdentical(p_xyzm_diff_m.get()));
}

TEST(PointTest, CompareToOrdering) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> empty1 = factory->createPoint();
  std::unique_ptr<Point> empty2 = factory->createPoint();
  std::unique_ptr<Point> p12 = factory->createPoint(CoordinateXY(1.0, 2.0));
  std::unique_ptr<Point> p12_dup = factory->createPoint(CoordinateXY(1.0, 2.0));
  std::unique_ptr<Point> p13 = factory->createPoint(CoordinateXY(1.0, 3.0));
  std::unique_ptr<Point> p21 = factory->createPoint(CoordinateXY(2.0, 1.0));
  std::unique_ptr<MultiPoint> mp = factory->createMultiPoint();

  EXPECT_EQ(empty1->compareTo(empty2.get()), 0);
  EXPECT_EQ(empty1->compareTo(p12.get()), -1);
  EXPECT_EQ(p12->compareTo(empty1.get()), 1);
  EXPECT_EQ(p12->compareTo(p12.get()), 0);
  EXPECT_EQ(p12->compareTo(p12_dup.get()), 0);
  EXPECT_EQ(p12->compareTo(p13.get()), -1);
  EXPECT_EQ(p13->compareTo(p12.get()), 1);
  EXPECT_EQ(p13->compareTo(p21.get()), -1);
  EXPECT_EQ(p21->compareTo(p13.get()), 1);
  EXPECT_EQ(p12->compareTo(mp.get()), -1);
}

}  // namespace
}  // namespace geos_google
