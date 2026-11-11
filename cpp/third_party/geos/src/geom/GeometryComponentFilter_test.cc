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

// Unit tests for third_party/geos/src/geom/GeometryComponentFilter.cpp.
//
// google3 notes:
// - //third_party/geos compiles with -DNDEBUG in fastbuild and opt builds, so
//   the assert(0) fallback inside base filter_rw/filter_ro is compiled out
//   outside -c dbg builds.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/GeometryComponentFilter.cpp

#include "third_party/geos/include/geos/geom/GeometryComponentFilter.h"

#include <array>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
#include "third_party/geos/include/geos/geom/CompoundCurve.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Curve.h"
#include "third_party/geos/include/geos/geom/CurvePolygon.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiCurve.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/MultiSurface.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/SimpleCurve.h"
#include "third_party/geos/include/geos/geom/Surface.h"

namespace geos_google {
namespace {

using ::testing::ElementsAre;

std::unique_ptr<geos::geom::LineString> MakeLineString(
    const geos::geom::GeometryFactory& factory,
    std::initializer_list<geos::geom::CoordinateXY> coords) {
  auto seq = std::make_unique<geos::geom::CoordinateSequence>(0u, false, false);
  for (const auto& coord : coords) {
    seq->add(coord.x, coord.y);
  }
  return factory.createLineString(std::move(seq));
}

std::unique_ptr<geos::geom::CircularString> MakeCircularString(
    const geos::geom::GeometryFactory& factory,
    std::initializer_list<geos::geom::CoordinateXY> coords) {
  auto seq = std::make_unique<geos::geom::CoordinateSequence>(0u, false, false);
  for (const auto& coord : coords) {
    seq->add(coord.x, coord.y);
  }
  return factory.createCircularString(std::move(seq));
}

std::unique_ptr<geos::geom::LinearRing> MakeRectRing(
    const geos::geom::GeometryFactory& factory, double min_x, double min_y,
    double max_x, double max_y) {
  auto seq = std::make_unique<geos::geom::CoordinateSequence>(0u, false, false);
  seq->add(min_x, min_y);
  seq->add(max_x, min_y);
  seq->add(max_x, max_y);
  seq->add(min_x, max_y);
  seq->add(min_x, min_y);
  return factory.createLinearRing(std::move(seq));
}

class DestructorTrackerFilter : public geos::geom::GeometryComponentFilter {
 public:
  explicit DestructorTrackerFilter(bool* destroyed) : destroyed_(destroyed) {}
  ~DestructorTrackerFilter() override { *destroyed_ = true; }

  void filter_ro(const geos::geom::Geometry* /*geom*/) override {}

 private:
  bool* destroyed_;
};

class ReadOnlyRecorder : public geos::geom::GeometryComponentFilter {
 public:
  explicit ReadOnlyRecorder(
      std::size_t max_visits = std::numeric_limits<std::size_t>::max())
      : max_visits_(max_visits) {}

  void filter_ro(const geos::geom::Geometry* geom) override {
    visited_.push_back(geom);
    types_.push_back(geom->getGeometryTypeId());
    user_data_.push_back(geom->getUserData());
  }

  bool isDone() override { return visited_.size() >= max_visits_; }

  const std::vector<const geos::geom::Geometry*>& visited() const {
    return visited_;
  }
  const std::vector<geos::geom::GeometryTypeId>& types() const {
    return types_;
  }
  const std::vector<void*>& user_data() const { return user_data_; }

 private:
  std::size_t max_visits_;
  std::vector<const geos::geom::Geometry*> visited_;
  std::vector<geos::geom::GeometryTypeId> types_;
  std::vector<void*> user_data_;
};

class ReadWriteUserDataSetter : public geos::geom::GeometryComponentFilter {
 public:
  explicit ReadWriteUserDataSetter(
      void* user_data,
      std::size_t max_visits = std::numeric_limits<std::size_t>::max())
      : user_data_(user_data), max_visits_(max_visits) {}

  void filter_rw(geos::geom::Geometry* geom) override {
    geom->setUserData(user_data_);
    visited_.push_back(geom);
    types_.push_back(geom->getGeometryTypeId());
  }

  bool isDone() override { return visited_.size() >= max_visits_; }

  const std::vector<geos::geom::Geometry*>& visited() const { return visited_; }
  const std::vector<geos::geom::GeometryTypeId>& types() const {
    return types_;
  }

 private:
  void* user_data_;
  std::size_t max_visits_;
  std::vector<geos::geom::Geometry*> visited_;
  std::vector<geos::geom::GeometryTypeId> types_;
};

class DualModeFilter : public geos::geom::GeometryComponentFilter {
 public:
  explicit DualModeFilter(
      std::optional<geos::geom::GeometryTypeId> stop_on_type = std::nullopt)
      : stop_on_type_(stop_on_type) {}

  void filter_ro(const geos::geom::Geometry* geom) override {
    ro_visited_.push_back(geom);
    if (stop_on_type_.has_value() &&
        geom->getGeometryTypeId() == *stop_on_type_) {
      done_ = true;
    }
  }

  void filter_rw(geos::geom::Geometry* geom) override {
    rw_visited_.push_back(geom);
    if (stop_on_type_.has_value() &&
        geom->getGeometryTypeId() == *stop_on_type_) {
      done_ = true;
    }
  }

  bool isDone() override { return done_; }

  const std::vector<const geos::geom::Geometry*>& ro_visited() const {
    return ro_visited_;
  }
  const std::vector<geos::geom::Geometry*>& rw_visited() const {
    return rw_visited_;
  }

 private:
  std::optional<geos::geom::GeometryTypeId> stop_on_type_;
  bool done_ = false;
  std::vector<const geos::geom::Geometry*> ro_visited_;
  std::vector<geos::geom::Geometry*> rw_visited_;
};

class PointShifterFilter : public geos::geom::GeometryComponentFilter {
 public:
  PointShifterFilter(double dx, double dy) : dx_(dx), dy_(dy) {}

  void filter_rw(geos::geom::Geometry* geom) override {
    if (auto* pt = dynamic_cast<geos::geom::Point*>(geom)) {
      if (!pt->isEmpty()) {
        pt->setXY(pt->getX() + dx_, pt->getY() + dy_);
      }
    }
  }

 private:
  double dx_;
  double dy_;
};

TEST(GeometryComponentFilterTest, DefaultIsDoneReturnsFalse) {
  geos::geom::GeometryComponentFilter base_filter;
  EXPECT_FALSE(base_filter.isDone());
  EXPECT_FALSE(base_filter.isDone());

  PointShifterFilter shifter(1.0, 2.0);
  geos::geom::GeometryComponentFilter& base_ref = shifter;
  EXPECT_FALSE(base_ref.isDone());
}

TEST(GeometryComponentFilterTest, VirtualDestructorDeletesDerivedThroughBase) {
  bool destroyed = false;
  {
    std::unique_ptr<geos::geom::GeometryComponentFilter> filter =
        std::make_unique<DestructorTrackerFilter>(&destroyed);
    EXPECT_FALSE(destroyed);
    EXPECT_FALSE(filter->isDone());
  }
  EXPECT_TRUE(destroyed);
}

TEST(GeometryComponentFilterTest, BaseFilterRwIsNoOpWhenNdDebugEnabled) {
  auto factory = geos::geom::GeometryFactory::create();
  auto point = factory->createPoint(geos::geom::CoordinateXY{1.0, 2.0});
  std::vector<std::unique_ptr<geos::geom::LinearRing>> holes;
  holes.push_back(MakeRectRing(*factory, 1.0, 1.0, 2.0, 2.0));
  auto poly = factory->createPolygon(MakeRectRing(*factory, 0.0, 0.0, 5.0, 5.0),
                                     std::move(holes));

  // //third_party/geos compiles with -DNDEBUG outside -c dbg builds, so the
  // default base filter_rw fallback returns cleanly without triggering assert.
  geos::geom::GeometryComponentFilter base_filter;
  base_filter.filter_rw(nullptr);
  base_filter.filter_rw(point.get());
  EXPECT_DOUBLE_EQ(point->getX(), 1.0);
  EXPECT_DOUBLE_EQ(point->getY(), 2.0);
  EXPECT_EQ(point->getUserData(), nullptr);

  ReadOnlyRecorder ro_only;
  point->apply_rw(&ro_only);
  poly->apply_rw(&ro_only);
  EXPECT_TRUE(ro_only.visited().empty());
  EXPECT_EQ(poly->getUserData(), nullptr);
  EXPECT_EQ(poly->getExteriorRing()->getUserData(), nullptr);
  EXPECT_EQ(poly->getInteriorRingN(0)->getUserData(), nullptr);
}

TEST(GeometryComponentFilterTest, BaseFilterRoIsNoOpWhenNdDebugEnabled) {
  auto factory = geos::geom::GeometryFactory::create();
  auto point = factory->createPoint(geos::geom::CoordinateXY{1.0, 2.0});
  std::vector<std::unique_ptr<geos::geom::LinearRing>> holes;
  holes.push_back(MakeRectRing(*factory, 1.0, 1.0, 2.0, 2.0));
  auto poly = factory->createPolygon(MakeRectRing(*factory, 0.0, 0.0, 5.0, 5.0),
                                     std::move(holes));

  // //third_party/geos compiles with -DNDEBUG outside -c dbg builds, so the
  // default base filter_ro fallback returns cleanly without triggering assert.
  geos::geom::GeometryComponentFilter base_filter;
  base_filter.filter_ro(nullptr);
  base_filter.filter_ro(point.get());
  EXPECT_DOUBLE_EQ(point->getX(), 1.0);
  EXPECT_DOUBLE_EQ(point->getY(), 2.0);
  EXPECT_EQ(point->getUserData(), nullptr);

  int tag = 1;
  ReadWriteUserDataSetter rw_only(&tag);
  point->apply_ro(&rw_only);
  poly->apply_ro(&rw_only);
  EXPECT_TRUE(rw_only.visited().empty());
  EXPECT_EQ(point->getUserData(), nullptr);
  EXPECT_EQ(poly->getUserData(), nullptr);
  EXPECT_EQ(poly->getExteriorRing()->getUserData(), nullptr);
  EXPECT_EQ(poly->getInteriorRingN(0)->getUserData(), nullptr);
}

TEST(GeometryComponentFilterTest,
     ReadOnlyFilterVisitsAtomicAndEmptyGeometries) {
  auto factory = geos::geom::GeometryFactory::create();

  auto point = factory->createPoint(geos::geom::CoordinateXY{1.0, 2.0});
  auto empty_point = factory->createPoint();
  auto line = MakeLineString(*factory, {{0.0, 0.0}, {3.0, 4.0}});
  auto empty_line = factory->createLineString();
  auto ring = MakeRectRing(*factory, 0.0, 0.0, 5.0, 5.0);
  auto empty_ring = factory->createLinearRing();
  auto circular =
      MakeCircularString(*factory, {{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}});
  auto empty_gc = factory->createGeometryCollection();

  std::vector<std::unique_ptr<geos::geom::SimpleCurve>> sections;
  sections.push_back(MakeLineString(*factory, {{0.0, 0.0}, {2.0, 0.0}}));
  sections.push_back(MakeLineString(*factory, {{2.0, 0.0}, {2.0, 2.0}}));
  auto compound = factory->createCompoundCurve(std::move(sections));

  const std::array<const geos::geom::Geometry*, 9> geometries = {
      point.get(),      empty_point.get(), line.get(),
      empty_line.get(), ring.get(),        empty_ring.get(),
      circular.get(),   empty_gc.get(),    compound.get(),
  };
  const std::array<geos::geom::GeometryTypeId, 9> expected_types = {
      geos::geom::GEOS_POINT,          geos::geom::GEOS_POINT,
      geos::geom::GEOS_LINESTRING,     geos::geom::GEOS_LINESTRING,
      geos::geom::GEOS_LINEARRING,     geos::geom::GEOS_LINEARRING,
      geos::geom::GEOS_CIRCULARSTRING, geos::geom::GEOS_GEOMETRYCOLLECTION,
      geos::geom::GEOS_COMPOUNDCURVE,
  };

  for (std::size_t i = 0; i < geometries.size(); ++i) {
    ReadOnlyRecorder recorder;
    geometries[i]->apply_ro(&recorder);
    EXPECT_THAT(recorder.visited(), ElementsAre(geometries[i]));
    EXPECT_THAT(recorder.types(), ElementsAre(expected_types[i]));
  }
}

TEST(GeometryComponentFilterTest,
     ReadOnlyFilterVisitsPolygonExteriorAndInteriorRings) {
  auto factory = geos::geom::GeometryFactory::create();

  auto shell = MakeRectRing(*factory, 0.0, 0.0, 20.0, 20.0);
  std::vector<std::unique_ptr<geos::geom::LinearRing>> holes;
  holes.push_back(MakeRectRing(*factory, 2.0, 2.0, 4.0, 4.0));
  holes.push_back(MakeRectRing(*factory, 10.0, 10.0, 14.0, 14.0));
  auto poly = factory->createPolygon(std::move(shell), std::move(holes));

  ReadOnlyRecorder poly_recorder;
  poly->apply_ro(&poly_recorder);
  EXPECT_THAT(
      poly_recorder.visited(),
      ElementsAre(poly.get(), poly->getExteriorRing(),
                  poly->getInteriorRingN(0), poly->getInteriorRingN(1)));
  EXPECT_THAT(
      poly_recorder.types(),
      ElementsAre(geos::geom::GEOS_POLYGON, geos::geom::GEOS_LINEARRING,
                  geos::geom::GEOS_LINEARRING, geos::geom::GEOS_LINEARRING));

  auto empty_poly = factory->createPolygon();
  ReadOnlyRecorder empty_recorder;
  empty_poly->apply_ro(&empty_recorder);
  EXPECT_THAT(empty_recorder.visited(),
              ElementsAre(empty_poly.get(), empty_poly->getExteriorRing()));

  auto curve_shell = MakeRectRing(*factory, 0.0, 0.0, 10.0, 10.0);
  std::vector<std::unique_ptr<geos::geom::Curve>> curve_holes;
  curve_holes.push_back(MakeRectRing(*factory, 2.0, 2.0, 4.0, 4.0));
  auto curve_poly = factory->createCurvePolygon(std::move(curve_shell),
                                                std::move(curve_holes));

  ReadOnlyRecorder curve_poly_recorder;
  curve_poly->apply_ro(&curve_poly_recorder);
  EXPECT_THAT(curve_poly_recorder.visited(),
              ElementsAre(curve_poly.get(), curve_poly->getExteriorRing(),
                          curve_poly->getInteriorRingN(0)));
}

TEST(GeometryComponentFilterTest,
     ReadOnlyFilterVisitsMultiGeometriesAndNestedCollections) {
  auto factory = geos::geom::GeometryFactory::create();

  std::vector<std::unique_ptr<geos::geom::Point>> pts;
  pts.push_back(factory->createPoint(geos::geom::CoordinateXY{1.0, 1.0}));
  pts.push_back(factory->createPoint(geos::geom::CoordinateXY{2.0, 2.0}));
  auto multi_point = factory->createMultiPoint(std::move(pts));

  ReadOnlyRecorder mp_recorder;
  multi_point->apply_ro(&mp_recorder);
  EXPECT_THAT(mp_recorder.visited(),
              ElementsAre(multi_point.get(), multi_point->getGeometryN(0),
                          multi_point->getGeometryN(1)));

  std::vector<std::unique_ptr<geos::geom::LineString>> lines;
  lines.push_back(MakeLineString(*factory, {{0.0, 0.0}, {1.0, 1.0}}));
  lines.push_back(MakeLineString(*factory, {{2.0, 2.0}, {3.0, 3.0}}));
  auto multi_line = factory->createMultiLineString(std::move(lines));

  ReadOnlyRecorder ml_recorder;
  multi_line->apply_ro(&ml_recorder);
  EXPECT_THAT(ml_recorder.visited(),
              ElementsAre(multi_line.get(), multi_line->getGeometryN(0),
                          multi_line->getGeometryN(1)));

  std::vector<std::unique_ptr<geos::geom::Polygon>> polys;
  std::vector<std::unique_ptr<geos::geom::LinearRing>> first_holes;
  first_holes.push_back(MakeRectRing(*factory, 1.0, 1.0, 2.0, 2.0));
  polys.push_back(factory->createPolygon(
      MakeRectRing(*factory, 0.0, 0.0, 5.0, 5.0), std::move(first_holes)));
  polys.push_back(
      factory->createPolygon(MakeRectRing(*factory, 10.0, 10.0, 15.0, 15.0)));
  auto multi_poly = factory->createMultiPolygon(std::move(polys));

  const geos::geom::Polygon* poly0 = multi_poly->getGeometryN(0);
  const geos::geom::Polygon* poly1 = multi_poly->getGeometryN(1);
  ReadOnlyRecorder mpg_recorder;
  multi_poly->apply_ro(&mpg_recorder);
  EXPECT_THAT(
      mpg_recorder.visited(),
      ElementsAre(multi_poly.get(), poly0, poly0->getExteriorRing(),
                  poly0->getInteriorRingN(0), poly1, poly1->getExteriorRing()));

  std::vector<std::unique_ptr<geos::geom::Geometry>> inner_geoms;
  inner_geoms.push_back(MakeLineString(*factory, {{0.0, 0.0}, {4.0, 0.0}}));
  inner_geoms.push_back(
      factory->createPoint(geos::geom::CoordinateXY{9.0, 9.0}));
  auto inner_gc = factory->createGeometryCollection(std::move(inner_geoms));

  std::vector<std::unique_ptr<geos::geom::LinearRing>> outer_poly_holes;
  outer_poly_holes.push_back(MakeRectRing(*factory, 1.0, 1.0, 3.0, 3.0));
  auto outer_poly = factory->createPolygon(
      MakeRectRing(*factory, 0.0, 0.0, 6.0, 6.0), std::move(outer_poly_holes));

  std::vector<std::unique_ptr<geos::geom::Geometry>> outer_geoms;
  outer_geoms.push_back(
      factory->createPoint(geos::geom::CoordinateXY{7.0, 8.0}));
  outer_geoms.push_back(std::move(outer_poly));
  outer_geoms.push_back(std::move(inner_gc));
  auto outer_gc = factory->createGeometryCollection(std::move(outer_geoms));

  const geos::geom::Geometry* pt0 = outer_gc->getGeometryN(0);
  const auto* poly_child =
      dynamic_cast<const geos::geom::Polygon*>(outer_gc->getGeometryN(1));
  ASSERT_NE(poly_child, nullptr);
  const auto* inner_child = dynamic_cast<const geos::geom::GeometryCollection*>(
      outer_gc->getGeometryN(2));
  ASSERT_NE(inner_child, nullptr);

  ReadOnlyRecorder nested_recorder;
  outer_gc->apply_ro(&nested_recorder);
  EXPECT_THAT(
      nested_recorder.visited(),
      ElementsAre(outer_gc.get(), pt0, poly_child,
                  poly_child->getExteriorRing(),
                  poly_child->getInteriorRingN(0), inner_child,
                  inner_child->getGeometryN(0), inner_child->getGeometryN(1)));
}

TEST(GeometryComponentFilterTest, ReadWriteFilterMutatesNestedComponents) {
  auto factory = geos::geom::GeometryFactory::create();

  std::vector<std::unique_ptr<geos::geom::LinearRing>> holes;
  holes.push_back(MakeRectRing(*factory, 2.0, 2.0, 4.0, 4.0));
  auto poly = factory->createPolygon(
      MakeRectRing(*factory, 0.0, 0.0, 10.0, 10.0), std::move(holes));

  std::vector<std::unique_ptr<geos::geom::Geometry>> inner;
  inner.push_back(MakeLineString(*factory, {{0.0, 0.0}, {1.0, 1.0}}));
  auto inner_gc = factory->createGeometryCollection(std::move(inner));

  std::vector<std::unique_ptr<geos::geom::Geometry>> outer;
  outer.push_back(factory->createPoint(geos::geom::CoordinateXY{5.0, 5.0}));
  outer.push_back(std::move(poly));
  outer.push_back(std::move(inner_gc));
  auto root = factory->createGeometryCollection(std::move(outer));

  int tag = 42;
  ReadWriteUserDataSetter user_data_setter(&tag);
  root->apply_rw(&user_data_setter);

  EXPECT_EQ(user_data_setter.visited().size(), 7u);
  EXPECT_THAT(
      user_data_setter.types(),
      ElementsAre(geos::geom::GEOS_GEOMETRYCOLLECTION, geos::geom::GEOS_POINT,
                  geos::geom::GEOS_POLYGON, geos::geom::GEOS_LINEARRING,
                  geos::geom::GEOS_LINEARRING,
                  geos::geom::GEOS_GEOMETRYCOLLECTION,
                  geos::geom::GEOS_LINESTRING));

  ReadOnlyRecorder verifier;
  root->apply_ro(&verifier);
  EXPECT_THAT(verifier.user_data(),
              ElementsAre(&tag, &tag, &tag, &tag, &tag, &tag, &tag));
}

TEST(GeometryComponentFilterTest,
     ReadWriteFilterMutatesCoordinatesAndGeometryChangedUpdatesEnvelope) {
  auto factory = geos::geom::GeometryFactory::create();

  std::vector<std::unique_ptr<geos::geom::Point>> pts;
  pts.push_back(factory->createPoint(geos::geom::CoordinateXY{1.0, 2.0}));
  pts.push_back(factory->createPoint(geos::geom::CoordinateXY{3.0, 4.0}));
  auto multi_point = factory->createMultiPoint(std::move(pts));

  const geos::geom::Envelope* before = multi_point->getEnvelopeInternal();
  EXPECT_DOUBLE_EQ(before->getMinX(), 1.0);
  EXPECT_DOUBLE_EQ(before->getMaxX(), 3.0);
  EXPECT_DOUBLE_EQ(before->getMinY(), 2.0);
  EXPECT_DOUBLE_EQ(before->getMaxY(), 4.0);

  PointShifterFilter shifter(10.0, 20.0);
  multi_point->apply_rw(&shifter);
  multi_point->geometryChanged();

  EXPECT_DOUBLE_EQ(multi_point->getGeometryN(0)->getX(), 11.0);
  EXPECT_DOUBLE_EQ(multi_point->getGeometryN(0)->getY(), 22.0);
  EXPECT_DOUBLE_EQ(multi_point->getGeometryN(1)->getX(), 13.0);
  EXPECT_DOUBLE_EQ(multi_point->getGeometryN(1)->getY(), 24.0);

  const geos::geom::Envelope* after = multi_point->getEnvelopeInternal();
  EXPECT_DOUBLE_EQ(after->getMinX(), 11.0);
  EXPECT_DOUBLE_EQ(after->getMaxX(), 13.0);
  EXPECT_DOUBLE_EQ(after->getMinY(), 22.0);
  EXPECT_DOUBLE_EQ(after->getMaxY(), 24.0);
}

TEST(GeometryComponentFilterTest, IsDoneStopsGeometryCollectionTraversalEarly) {
  auto factory = geos::geom::GeometryFactory::create();

  std::vector<std::unique_ptr<geos::geom::Geometry>> inner;
  inner.push_back(MakeLineString(*factory, {{0.0, 0.0}, {1.0, 1.0}}));
  inner.push_back(factory->createPoint(geos::geom::CoordinateXY{2.0, 2.0}));
  auto inner_gc = factory->createGeometryCollection(std::move(inner));

  std::vector<std::unique_ptr<geos::geom::Geometry>> outer;
  outer.push_back(factory->createPoint(geos::geom::CoordinateXY{0.0, 0.0}));
  outer.push_back(std::move(inner_gc));
  outer.push_back(factory->createPoint(geos::geom::CoordinateXY{3.0, 3.0}));
  auto root = factory->createGeometryCollection(std::move(outer));

  const geos::geom::Geometry* child0 = root->getGeometryN(0);
  const auto* child1 = dynamic_cast<const geos::geom::GeometryCollection*>(
      root->getGeometryN(1));
  ASSERT_NE(child1, nullptr);
  const geos::geom::Geometry* child2 = root->getGeometryN(2);

  // Stop immediately after visiting the outer collection itself.
  ReadOnlyRecorder ro_stop_at_1(/*max_visits=*/1);
  root->apply_ro(&ro_stop_at_1);
  EXPECT_TRUE(ro_stop_at_1.isDone());
  EXPECT_THAT(ro_stop_at_1.visited(), ElementsAre(root.get()));

  int tag1 = 1;
  ReadWriteUserDataSetter rw_stop_at_1(&tag1, /*max_visits=*/1);
  root->apply_rw(&rw_stop_at_1);
  EXPECT_TRUE(rw_stop_at_1.isDone());
  EXPECT_THAT(rw_stop_at_1.visited(), ElementsAre(root.get()));
  EXPECT_EQ(root->getUserData(), &tag1);
  EXPECT_EQ(child0->getUserData(), nullptr);
  EXPECT_EQ(child1->getUserData(), nullptr);
  EXPECT_EQ(child2->getUserData(), nullptr);

  // Stop after visiting the outer collection and its first child.
  ReadOnlyRecorder ro_stop_at_2(/*max_visits=*/2);
  root->apply_ro(&ro_stop_at_2);
  EXPECT_THAT(ro_stop_at_2.visited(), ElementsAre(root.get(), child0));

  int tag2 = 2;
  ReadWriteUserDataSetter rw_stop_at_2(&tag2, /*max_visits=*/2);
  root->apply_rw(&rw_stop_at_2);
  EXPECT_THAT(rw_stop_at_2.visited(), ElementsAre(root.get(), child0));
  EXPECT_EQ(root->getUserData(), &tag2);
  EXPECT_EQ(child0->getUserData(), &tag2);
  EXPECT_EQ(child1->getUserData(), nullptr);
  EXPECT_EQ(child2->getUserData(), nullptr);

  // Stop inside the nested collection upon seeing the LineString, preventing
  // visits to the remaining inner Point and the third outer element.
  DualModeFilter ro_stop_on_line(geos::geom::GEOS_LINESTRING);
  root->apply_ro(&ro_stop_on_line);
  EXPECT_TRUE(ro_stop_on_line.isDone());
  EXPECT_THAT(ro_stop_on_line.ro_visited(),
              ElementsAre(root.get(), child0, child1, child1->getGeometryN(0)));

  DualModeFilter rw_stop_on_line(geos::geom::GEOS_LINESTRING);
  root->apply_rw(&rw_stop_on_line);
  EXPECT_TRUE(rw_stop_on_line.isDone());
  EXPECT_THAT(rw_stop_on_line.rw_visited(),
              ElementsAre(root.get(), child0, child1, child1->getGeometryN(0)));
}

TEST(GeometryComponentFilterTest,
     IsDoneStopsSurfaceTraversalAfterShellOrFirstHole) {
  auto factory = geos::geom::GeometryFactory::create();

  auto shell = MakeRectRing(*factory, 0.0, 0.0, 20.0, 20.0);
  std::vector<std::unique_ptr<geos::geom::LinearRing>> holes;
  holes.push_back(MakeRectRing(*factory, 2.0, 2.0, 4.0, 4.0));
  holes.push_back(MakeRectRing(*factory, 10.0, 10.0, 14.0, 14.0));
  auto poly = factory->createPolygon(std::move(shell), std::move(holes));

  // Surface always applies to the exterior ring after the polygon itself, then
  // checks isDone() before each interior ring.
  ReadOnlyRecorder ro_stop_at_1(/*max_visits=*/1);
  poly->apply_ro(&ro_stop_at_1);
  EXPECT_THAT(ro_stop_at_1.visited(),
              ElementsAre(poly.get(), poly->getExteriorRing()));

  int tag1 = 1;
  ReadWriteUserDataSetter rw_stop_at_1(&tag1, /*max_visits=*/1);
  poly->apply_rw(&rw_stop_at_1);
  EXPECT_THAT(rw_stop_at_1.visited(),
              ElementsAre(poly.get(), poly->getExteriorRing()));
  EXPECT_EQ(poly->getUserData(), &tag1);
  EXPECT_EQ(poly->getExteriorRing()->getUserData(), &tag1);
  EXPECT_EQ(poly->getInteriorRingN(0)->getUserData(), nullptr);
  EXPECT_EQ(poly->getInteriorRingN(1)->getUserData(), nullptr);

  ReadOnlyRecorder ro_stop_at_3(/*max_visits=*/3);
  poly->apply_ro(&ro_stop_at_3);
  EXPECT_THAT(ro_stop_at_3.visited(),
              ElementsAre(poly.get(), poly->getExteriorRing(),
                          poly->getInteriorRingN(0)));

  int tag3 = 3;
  ReadWriteUserDataSetter rw_stop_at_3(&tag3, /*max_visits=*/3);
  poly->apply_rw(&rw_stop_at_3);
  EXPECT_THAT(rw_stop_at_3.visited(),
              ElementsAre(poly.get(), poly->getExteriorRing(),
                          poly->getInteriorRingN(0)));
  EXPECT_EQ(poly->getInteriorRingN(0)->getUserData(), &tag3);
  EXPECT_EQ(poly->getInteriorRingN(1)->getUserData(), nullptr);
}

TEST(GeometryComponentFilterTest,
     DualModeFilterAccumulatesAcrossReadOnlyAndReadWriteCalls) {
  auto factory = geos::geom::GeometryFactory::create();
  auto pt1 = factory->createPoint(geos::geom::CoordinateXY{1.0, 2.0});
  auto pt2 = factory->createPoint(geos::geom::CoordinateXY{3.0, 4.0});
  auto line = MakeLineString(*factory, {{0.0, 0.0}, {1.0, 0.0}});

  DualModeFilter filter;
  pt1->apply_ro(&filter);
  pt2->apply_rw(&filter);
  line->apply_ro(&filter);

  EXPECT_FALSE(filter.isDone());
  EXPECT_THAT(filter.ro_visited(), ElementsAre(pt1.get(), line.get()));
  EXPECT_THAT(filter.rw_visited(), ElementsAre(pt2.get()));
}

TEST(GeometryComponentFilterTest,
     ReadOnlyAndReadWriteVisitMultiCurveAndMultiSurface) {
  auto factory = geos::geom::GeometryFactory::create();

  std::vector<std::unique_ptr<geos::geom::Curve>> curves;
  curves.push_back(MakeLineString(*factory, {{0.0, 0.0}, {1.0, 0.0}}));
  curves.push_back(
      MakeCircularString(*factory, {{1.0, 0.0}, {2.0, 1.0}, {3.0, 0.0}}));
  auto multi_curve = factory->createMultiCurve(std::move(curves));

  ReadOnlyRecorder mc_ro;
  multi_curve->apply_ro(&mc_ro);
  EXPECT_THAT(mc_ro.visited(),
              ElementsAre(multi_curve.get(), multi_curve->getGeometryN(0),
                          multi_curve->getGeometryN(1)));
  EXPECT_THAT(mc_ro.types(), ElementsAre(geos::geom::GEOS_MULTICURVE,
                                         geos::geom::GEOS_LINESTRING,
                                         geos::geom::GEOS_CIRCULARSTRING));

  int curve_tag = 7;
  ReadWriteUserDataSetter mc_rw(&curve_tag, /*max_visits=*/2);
  multi_curve->apply_rw(&mc_rw);
  EXPECT_THAT(mc_rw.visited(),
              ElementsAre(multi_curve.get(), multi_curve->getGeometryN(0)));
  EXPECT_EQ(multi_curve->getGeometryN(0)->getUserData(), &curve_tag);
  EXPECT_EQ(multi_curve->getGeometryN(1)->getUserData(), nullptr);

  std::vector<std::unique_ptr<geos::geom::Surface>> surfaces;
  std::vector<std::unique_ptr<geos::geom::LinearRing>> poly_holes;
  poly_holes.push_back(MakeRectRing(*factory, 1.0, 1.0, 2.0, 2.0));
  surfaces.push_back(factory->createPolygon(
      MakeRectRing(*factory, 0.0, 0.0, 5.0, 5.0), std::move(poly_holes)));
  surfaces.push_back(factory->createCurvePolygon(MakeCircularString(
      *factory,
      {{10.0, 0.0}, {11.0, 1.0}, {12.0, 0.0}, {11.0, -1.0}, {10.0, 0.0}})));
  auto multi_surface = factory->createMultiSurface(std::move(surfaces));

  const auto* poly_child =
      dynamic_cast<const geos::geom::Polygon*>(multi_surface->getGeometryN(0));
  ASSERT_NE(poly_child, nullptr);
  const auto* curve_poly_child = dynamic_cast<const geos::geom::CurvePolygon*>(
      multi_surface->getGeometryN(1));
  ASSERT_NE(curve_poly_child, nullptr);

  ReadOnlyRecorder ms_ro;
  multi_surface->apply_ro(&ms_ro);
  EXPECT_THAT(ms_ro.visited(),
              ElementsAre(multi_surface.get(), poly_child,
                          poly_child->getExteriorRing(),
                          poly_child->getInteriorRingN(0), curve_poly_child,
                          curve_poly_child->getExteriorRing()));

  int surface_tag = 9;
  ReadWriteUserDataSetter ms_rw(&surface_tag);
  multi_surface->apply_rw(&ms_rw);
  EXPECT_EQ(ms_rw.visited().size(), 6u);
  EXPECT_EQ(curve_poly_child->getExteriorRing()->getUserData(), &surface_tag);
}

}  // namespace
}  // namespace geos_google
