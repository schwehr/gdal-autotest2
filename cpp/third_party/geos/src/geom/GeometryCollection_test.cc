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

// Unit tests for third_party/geos/src/geom/GeometryCollection.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - GeometryCollection's constructor evaluates computeEnvelopeInternal() in its
//   member initializer list before checking hasNullElements(&geometries), so
//   passing null component pointers is undefined behavior and is not tested.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/GeometryCollection.cpp

#include "third_party/geos/include/geos/geom/GeometryCollection.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateFilter.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/CoordinateSequenceFilter.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryComponentFilter.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/GeometryFilter.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
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
using geos::geom::GEOS_GEOMETRYCOLLECTION;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::util::IllegalArgumentException;
using geos::util::UnsupportedOperationException;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::SizeIs;

std::unique_ptr<LineString> MakeLine(const GeometryFactory& factory,
                                     std::initializer_list<CoordinateXY> pts) {
  CoordinateSequence seq(pts);
  return factory.createLineString(seq);
}

std::unique_ptr<LinearRing> MakeRing(const GeometryFactory& factory,
                                     std::initializer_list<CoordinateXY> pts) {
  CoordinateSequence seq(pts);
  return factory.createLinearRing(seq);
}

std::unique_ptr<Polygon> MakeBoxPolygon(const GeometryFactory& factory,
                                        double min_x, double min_y,
                                        double max_x, double max_y) {
  auto shell = MakeRing(factory, {
                                     CoordinateXY(min_x, min_y),
                                     CoordinateXY(max_x, min_y),
                                     CoordinateXY(max_x, max_y),
                                     CoordinateXY(min_x, max_y),
                                     CoordinateXY(min_x, min_y),
                                 });
  return factory.createPolygon(std::move(shell));
}

template <typename... Geoms>
std::unique_ptr<GeometryCollection> MakeCollection(
    const GeometryFactory& factory, Geoms&&... geoms) {
  std::vector<std::unique_ptr<Geometry>> vec;
  vec.reserve(sizeof...(Geoms));
  (vec.push_back(std::forward<Geoms>(geoms)), ...);
  return factory.createGeometryCollection(std::move(vec));
}

// Exposes protected GeometryCollection constructors and copy-assignment for
// direct coverage of protected member functions.
class TestableGeometryCollection : public GeometryCollection {
 public:
  TestableGeometryCollection(std::vector<std::unique_ptr<Geometry>>&& geoms,
                             const GeometryFactory& factory)
      : GeometryCollection(std::move(geoms), factory) {}

  template <typename T>
  TestableGeometryCollection(std::vector<std::unique_ptr<T>>&& geoms,
                             const GeometryFactory& factory)
      : GeometryCollection(std::move(geoms), factory) {}

  TestableGeometryCollection& operator=(const GeometryCollection& other) {
    GeometryCollection::operator=(other);
    return *this;
  }
};

struct DimensionExpectation {
  Dimension::DimensionType dim;
  int boundary_dim;
  bool has_p;
  bool has_l;
  bool has_a;
  bool strict_p;
  bool strict_l;
  bool strict_a;
  bool strict_false;
  bool strict_dontcare;
  bool mixed_dim;
};

void ExpectDimensionProperties(const GeometryCollection& gc,
                               const DimensionExpectation& expected) {
  EXPECT_EQ(gc.getDimension(), expected.dim);
  EXPECT_EQ(gc.getBoundaryDimension(), expected.boundary_dim);
  EXPECT_EQ(gc.hasDimension(Dimension::P), expected.has_p);
  EXPECT_EQ(gc.hasDimension(Dimension::L), expected.has_l);
  EXPECT_EQ(gc.hasDimension(Dimension::A), expected.has_a);
  EXPECT_FALSE(gc.hasDimension(Dimension::False));
  EXPECT_FALSE(gc.hasDimension(Dimension::DONTCARE));
  EXPECT_EQ(gc.isDimensionStrict(Dimension::P), expected.strict_p);
  EXPECT_EQ(gc.isDimensionStrict(Dimension::L), expected.strict_l);
  EXPECT_EQ(gc.isDimensionStrict(Dimension::A), expected.strict_a);
  EXPECT_EQ(gc.isDimensionStrict(Dimension::False), expected.strict_false);
  EXPECT_EQ(gc.isDimensionStrict(Dimension::DONTCARE),
            expected.strict_dontcare);
  EXPECT_EQ(gc.isMixedDimension(), expected.mixed_dim);
}

class CollectCoordFilter : public CoordinateInspector<CollectCoordFilter> {
 public:
  void filter(const CoordinateXY* coord) { coords_.push_back(*coord); }
  bool isDone() const override { return false; }

  const std::vector<CoordinateXY>& coords() const { return coords_; }

 private:
  std::vector<CoordinateXY> coords_;
};

class ShiftCoordFilter : public CoordinateMutator<ShiftCoordFilter> {
 public:
  ShiftCoordFilter(double dx, double dy) : dx_(dx), dy_(dy) {}
  void filter(CoordinateXY* coord) const {
    coord->x += dx_;
    coord->y += dy_;
  }

 private:
  double dx_;
  double dy_;
};

class RecordingGeometryFilter : public GeometryFilter {
 public:
  void filter_ro(const Geometry* geom) override {
    ro_types_.push_back(geom->getGeometryType());
  }

  void filter_rw(Geometry* geom) override {
    rw_types_.push_back(geom->getGeometryType());
    geom->setUserData(user_data_tag_);
  }

  void set_user_data_tag(void* tag) { user_data_tag_ = tag; }
  const std::vector<std::string>& ro_types() const { return ro_types_; }
  const std::vector<std::string>& rw_types() const { return rw_types_; }

 private:
  void* user_data_tag_ = nullptr;
  std::vector<std::string> ro_types_;
  std::vector<std::string> rw_types_;
};

class RecordingComponentFilter : public GeometryComponentFilter {
 public:
  explicit RecordingComponentFilter(std::size_t max_visits = 0)
      : max_visits_(max_visits) {}

  void filter_ro(const Geometry* geom) override {
    ro_types_.push_back(geom->getGeometryType());
    if (max_visits_ > 0 && ro_types_.size() >= max_visits_) {
      done_ = true;
    }
  }

  void filter_rw(Geometry* geom) override {
    rw_types_.push_back(geom->getGeometryType());
    if (max_visits_ > 0 && rw_types_.size() >= max_visits_) {
      done_ = true;
    }
  }

  bool isDone() override { return done_; }

  const std::vector<std::string>& ro_types() const { return ro_types_; }
  const std::vector<std::string>& rw_types() const { return rw_types_; }

 private:
  std::size_t max_visits_;
  bool done_ = false;
  std::vector<std::string> ro_types_;
  std::vector<std::string> rw_types_;
};

class RecordingSeqFilter : public CoordinateSequenceFilter {
 public:
  RecordingSeqFilter(bool mutate, bool report_changed, std::size_t max_visits)
      : mutate_(mutate),
        report_changed_(report_changed),
        max_visits_(max_visits) {}

  void filter_ro(const CoordinateSequence& seq, std::size_t i) override {
    visited_.push_back(seq.getAt<CoordinateXY>(i));
    if (max_visits_ > 0 && visited_.size() >= max_visits_) {
      done_ = true;
    }
  }

  void filter_rw(CoordinateSequence& seq, std::size_t i) override {
    CoordinateXY c = seq.getAt<CoordinateXY>(i);
    visited_.push_back(c);
    if (mutate_) {
      seq.setOrdinate(i, CoordinateSequence::X, c.x + 100.0);
      seq.setOrdinate(i, CoordinateSequence::Y, c.y + 200.0);
    }
    if (max_visits_ > 0 && visited_.size() >= max_visits_) {
      done_ = true;
    }
  }

  bool isDone() const override { return done_; }
  bool isGeometryChanged() const override { return report_changed_; }

  const std::vector<CoordinateXY>& visited() const { return visited_; }

 private:
  bool mutate_;
  bool report_changed_;
  std::size_t max_visits_;
  bool done_ = false;
  std::vector<CoordinateXY> visited_;
};

TEST(GeometryCollectionTest, EmptyCollectionDefaultsAndProperties) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<GeometryCollection> gc = factory->createGeometryCollection();
  ASSERT_NE(gc, nullptr);

  EXPECT_TRUE(gc->isEmpty());
  EXPECT_TRUE(gc->isCollection());
  EXPECT_EQ(gc->getNumGeometries(), 0u);
  EXPECT_EQ(gc->getNumPoints(), 0u);
  EXPECT_EQ(gc->getGeometryType(), "GeometryCollection");
  EXPECT_EQ(gc->getGeometryTypeId(), GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(gc->getCoordinate(), nullptr);
  EXPECT_TRUE(gc->getCoordinates()->isEmpty());
  EXPECT_TRUE(gc->getEnvelopeInternal()->isNull());
  EXPECT_DOUBLE_EQ(gc->getArea(), 0.0);
  EXPECT_DOUBLE_EQ(gc->getLength(), 0.0);
  EXPECT_EQ(static_cast<int>(gc->getCoordinateDimension()), 2);
  EXPECT_FALSE(gc->hasZ());
  EXPECT_FALSE(gc->hasM());
  const Geometry* base = gc.get();
  EXPECT_FALSE(base->hasCurvedComponents());
  EXPECT_EQ(gc->begin(), gc->end());
  EXPECT_EQ(gc->getFactory(), factory.get());
  EXPECT_EQ(gc->toText(), "GEOMETRYCOLLECTION EMPTY");

  ExpectDimensionProperties(*gc, {
                                     .dim = Dimension::False,
                                     .boundary_dim = Dimension::False,
                                     .has_p = false,
                                     .has_l = false,
                                     .has_a = false,
                                     .strict_p = true,
                                     .strict_l = true,
                                     .strict_a = true,
                                     .strict_false = true,
                                     .strict_dontcare = true,
                                     .mixed_dim = false,
                                 });
}

TEST(GeometryCollectionTest, ConstructFromTypedVectorsAndRawPointers) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::vector<std::unique_ptr<Point>> owned_points;
  owned_points.push_back(factory->createPoint(CoordinateXY(1.0, 2.0)));
  owned_points.push_back(factory->createPoint(CoordinateXY(3.0, 4.0)));
  std::unique_ptr<GeometryCollection> from_points =
      factory->createGeometryCollection(std::move(owned_points));

  ASSERT_EQ(from_points->getNumGeometries(), 2u);
  EXPECT_EQ(from_points->getGeometryTypeId(), GEOS_GEOMETRYCOLLECTION);
  std::vector<std::string> iterated_types;
  for (const auto& component : *from_points) {
    iterated_types.push_back(component->getGeometryType());
  }
  EXPECT_THAT(iterated_types, ElementsAre("Point", "Point"));

  // Exercise protected template<typename T> constructor directly.
  std::vector<std::unique_ptr<Point>> protected_points;
  protected_points.push_back(factory->createPoint(CoordinateXY(5.0, 6.0)));
  TestableGeometryCollection from_protected(std::move(protected_points),
                                            *factory);
  ASSERT_EQ(from_protected.getNumGeometries(), 1u);
  EXPECT_DOUBLE_EQ(from_protected.getGeometryN(0)->getCoordinate()->x, 5.0);

  // Exercise deep-copying overload accepting raw const Geometry* pointers.
  auto line =
      MakeLine(*factory, {CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 0.0)});
  auto poly = MakeBoxPolygon(*factory, 1.0, 1.0, 3.0, 3.0);
  std::vector<const Geometry*> raw_ptrs = {line.get(), poly.get()};
  std::unique_ptr<GeometryCollection> copied =
      factory->createGeometryCollection(raw_ptrs);

  // Mutating original inputs must not affect the collection created from them.
  ShiftCoordFilter shift(50.0, 50.0);
  line->apply_rw(&shift);
  line->geometryChanged();

  ASSERT_EQ(copied->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(copied->getGeometryN(0)->getCoordinate()->x, 0.0);
  EXPECT_DOUBLE_EQ(copied->getGeometryN(1)->getArea(), 4.0);
}

TEST(GeometryCollectionTest, CloneDeepCopiesElementsAndFlags) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto gc = MakeCollection(
      *factory, factory->createPoint(Coordinate(1.0, 2.0, 3.0)),
      MakeLine(*factory, {CoordinateXY(4.0, 5.0), CoordinateXY(6.0, 7.0)}));

  int sentinel_tag = 42;
  gc->setUserData(&sentinel_tag);

  // Clone before flags are computed.
  std::unique_ptr<GeometryCollection> clone_unflagged = gc->clone();
  EXPECT_EQ(clone_unflagged->getUserData(), nullptr);
  EXPECT_TRUE(clone_unflagged->equalsExact(gc.get()));

  // Trigger flag calculation, then clone again.
  EXPECT_EQ(gc->getDimension(), Dimension::L);
  EXPECT_TRUE(gc->hasZ());
  EXPECT_FALSE(gc->hasM());
  std::unique_ptr<GeometryCollection> clone_flagged = gc->clone();
  EXPECT_EQ(clone_flagged->getDimension(), Dimension::L);
  EXPECT_TRUE(clone_flagged->hasZ());
  EXPECT_FALSE(clone_flagged->hasM());

  // Mutate original and verify clones stay isolated.
  ShiftCoordFilter shift(10.0, -5.0);
  gc->apply_rw(&shift);
  gc->geometryChanged();

  EXPECT_DOUBLE_EQ(gc->getCoordinate()->x, 11.0);
  EXPECT_DOUBLE_EQ(clone_unflagged->getCoordinate()->x, 1.0);
  EXPECT_DOUBLE_EQ(clone_flagged->getCoordinate()->x, 1.0);

  // Empty clone.
  auto empty = factory->createGeometryCollection();
  auto empty_clone = empty->clone();
  EXPECT_TRUE(empty_clone->isEmpty());
  EXPECT_TRUE(empty_clone->equalsExact(empty.get()));
}

TEST(GeometryCollectionTest, CopyAssignmentDeepCopiesAndResizes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto src_large = MakeCollection(
      *factory, factory->createPoint(CoordinateXY(1.0, 2.0)),
      MakeLine(*factory, {CoordinateXY(3.0, 4.0), CoordinateXY(5.0, 6.0)}),
      MakeBoxPolygon(*factory, 0.0, 0.0, 2.0, 2.0));
  EXPECT_EQ(src_large->getDimension(), Dimension::A);

  auto src_small =
      MakeCollection(*factory, factory->createPoint(CoordinateXY(9.0, 8.0)));
  auto src_empty = factory->createGeometryCollection();

  std::vector<std::unique_ptr<Geometry>> init_vec;
  TestableGeometryCollection target(std::move(init_vec), *factory);
  EXPECT_TRUE(target.isEmpty());

  // Grow from 0 to 3 elements.
  target = *src_large;
  ASSERT_EQ(target.getNumGeometries(), 3u);
  EXPECT_EQ(target.getDimension(), Dimension::A);
  EXPECT_DOUBLE_EQ(target.getEnvelopeInternal()->getMaxX(), 5.0);

  // Verify deep copy on assignment.
  ShiftCoordFilter shift(100.0, 0.0);
  src_large->apply_rw(&shift);
  src_large->geometryChanged();
  EXPECT_DOUBLE_EQ(target.getGeometryN(0)->getCoordinate()->x, 1.0);

  // Shrink from 3 to 1 element.
  target = *src_small;
  ASSERT_EQ(target.getNumGeometries(), 1u);
  EXPECT_EQ(target.getDimension(), Dimension::P);
  EXPECT_DOUBLE_EQ(target.getCoordinate()->x, 9.0);
  EXPECT_DOUBLE_EQ(target.getCoordinate()->y, 8.0);

  // Assign empty over non-empty.
  target = *src_empty;
  EXPECT_EQ(target.getNumGeometries(), 0u);
  EXPECT_TRUE(target.isEmpty());
  EXPECT_TRUE(target.getEnvelopeInternal()->isNull());
}

TEST(GeometryCollectionTest, ConstructorAndSetSridPropagateRecursively) {
  PrecisionModel pm(PrecisionModel::FLOATING);
  GeometryFactory::Ptr factory_4326 = GeometryFactory::create(&pm, 4326);
  GeometryFactory::Ptr factory_3857 = GeometryFactory::create(&pm, 3857);

  auto nested = MakeCollection(
      *factory_3857, factory_3857->createPoint(CoordinateXY(10.0, 20.0)));
  auto pt = factory_3857->createPoint(CoordinateXY(1.0, 2.0));
  EXPECT_EQ(nested->getSRID(), 3857);
  EXPECT_EQ(pt->getSRID(), 3857);

  auto outer = MakeCollection(*factory_4326, std::move(pt), std::move(nested));
  EXPECT_EQ(outer->getSRID(), 4326);
  EXPECT_EQ(outer->getGeometryN(0)->getSRID(), 4326);
  const auto* inner_gc =
      dynamic_cast<const GeometryCollection*>(outer->getGeometryN(1));
  ASSERT_NE(inner_gc, nullptr);
  EXPECT_EQ(inner_gc->getSRID(), 4326);
  EXPECT_EQ(inner_gc->getGeometryN(0)->getSRID(), 4326);

  outer->setSRID(32632);
  EXPECT_EQ(outer->getSRID(), 32632);
  EXPECT_EQ(outer->getGeometryN(0)->getSRID(), 32632);
  EXPECT_EQ(inner_gc->getSRID(), 32632);
  EXPECT_EQ(inner_gc->getGeometryN(0)->getSRID(), 32632);
}

TEST(GeometryCollectionTest, IsEmptyHandlesNestedAndMixedEmptyElements) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto only_empties = MakeCollection(
      *factory, factory->createPoint(), factory->createLineString(),
      factory->createPolygon(), factory->createGeometryCollection());
  EXPECT_TRUE(only_empties->isEmpty());
  EXPECT_EQ(only_empties->getNumGeometries(), 4u);

  auto leading_empty_then_point =
      MakeCollection(*factory, factory->createPoint(),
                     factory->createPoint(CoordinateXY(2.0, 3.0)));
  EXPECT_FALSE(leading_empty_then_point->isEmpty());

  auto nested_non_empty = MakeCollection(
      *factory, factory->createPoint(),
      MakeCollection(*factory, factory->createLineString(),
                     factory->createPoint(CoordinateXY(4.0, 5.0))));
  EXPECT_FALSE(nested_non_empty->isEmpty());
}

TEST(GeometryCollectionTest, GetCoordinateSkipsEmptySubgeometries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto only_empties = MakeCollection(*factory, factory->createPoint(),
                                     factory->createLineString());
  EXPECT_EQ(only_empties->getCoordinate(), nullptr);

  auto leading_empty = MakeCollection(
      *factory, factory->createPoint(), factory->createLineString(),
      factory->createPoint(CoordinateXY(7.5, -2.25)),
      factory->createPoint(CoordinateXY(99.0, 99.0)));
  const CoordinateXY* coord = leading_empty->getCoordinate();
  ASSERT_NE(coord, nullptr);
  EXPECT_DOUBLE_EQ(coord->x, 7.5);
  EXPECT_DOUBLE_EQ(coord->y, -2.25);
}

TEST(GeometryCollectionTest, GetCoordinatesAndNumPointsFlattenNested) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto only_empties = MakeCollection(*factory, factory->createPoint(),
                                     factory->createLineString());
  EXPECT_EQ(only_empties->getNumPoints(), 0u);
  EXPECT_TRUE(only_empties->getCoordinates()->isEmpty());

  auto shell = MakeRing(*factory, {
                                      CoordinateXY(0.0, 0.0),
                                      CoordinateXY(10.0, 0.0),
                                      CoordinateXY(10.0, 10.0),
                                      CoordinateXY(0.0, 10.0),
                                      CoordinateXY(0.0, 0.0),
                                  });
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(MakeRing(*factory, {
                                         CoordinateXY(2.0, 2.0),
                                         CoordinateXY(2.0, 4.0),
                                         CoordinateXY(4.0, 4.0),
                                         CoordinateXY(4.0, 2.0),
                                         CoordinateXY(2.0, 2.0),
                                     }));
  auto poly_with_hole =
      factory->createPolygon(std::move(shell), std::move(holes));

  auto gc = MakeCollection(
      *factory, factory->createPoint(CoordinateXY(1.0, 2.0)),
      MakeLine(*factory, {CoordinateXY(3.0, 4.0), CoordinateXY(5.0, 6.0)}),
      std::move(poly_with_hole),
      MakeCollection(*factory, factory->createPoint(CoordinateXY(7.0, 8.0))));

  // 1 (Point) + 2 (LineString) + 10 (Polygon shell + hole) + 1 (nested Point).
  EXPECT_EQ(gc->getNumPoints(), 14u);
  std::unique_ptr<CoordinateSequence> coords = gc->getCoordinates();
  ASSERT_EQ(coords->getSize(), 14u);
  EXPECT_EQ(coords->getAt<CoordinateXY>(0), CoordinateXY(1.0, 2.0));
  EXPECT_TRUE(std::isnan(coords->getOrdinate(0, CoordinateSequence::Z)));
  EXPECT_EQ(coords->getAt<CoordinateXY>(1), CoordinateXY(3.0, 4.0));
  EXPECT_EQ(coords->getAt<CoordinateXY>(2), CoordinateXY(5.0, 6.0));
  EXPECT_EQ(coords->getAt<CoordinateXY>(3), CoordinateXY(0.0, 0.0));
  EXPECT_EQ(coords->getAt<CoordinateXY>(13), CoordinateXY(7.0, 8.0));

  // Verify 3D coordinates are preserved in flattened sequence.
  CoordinateSequence line_3d(
      {Coordinate(3.0, 4.0, 30.0), Coordinate(5.0, 6.0, 40.0)});
  auto gc_3d =
      MakeCollection(*factory, factory->createPoint(Coordinate(1.0, 2.0, 10.0)),
                     factory->createLineString(line_3d));
  std::unique_ptr<CoordinateSequence> coords_3d = gc_3d->getCoordinates();
  ASSERT_EQ(coords_3d->getSize(), 3u);
  EXPECT_TRUE(
      coords_3d->getAt<Coordinate>(0).equals3D(Coordinate(1.0, 2.0, 10.0)));
  EXPECT_TRUE(
      coords_3d->getAt<Coordinate>(1).equals3D(Coordinate(3.0, 4.0, 30.0)));
  EXPECT_TRUE(
      coords_3d->getAt<Coordinate>(2).equals3D(Coordinate(5.0, 6.0, 40.0)));
}

TEST(GeometryCollectionTest, ReleaseGeometriesTransfersOwnership) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto gc = MakeCollection(
      *factory, factory->createPoint(CoordinateXY(1.0, 2.0)),
      MakeLine(*factory, {CoordinateXY(3.0, 4.0), CoordinateXY(5.0, 6.0)}));

  std::vector<std::unique_ptr<Geometry>> released = gc->releaseGeometries();
  ASSERT_EQ(released.size(), 2u);
  EXPECT_EQ(released[0]->getGeometryType(), "Point");
  EXPECT_DOUBLE_EQ(released[0]->getCoordinate()->x, 1.0);
  EXPECT_EQ(released[1]->getGeometryType(), "LineString");
  EXPECT_EQ(released[1]->getNumPoints(), 2u);

  auto empty_gc = factory->createGeometryCollection();
  EXPECT_THAT(empty_gc->releaseGeometries(), IsEmpty());
}

TEST(GeometryCollectionTest, DimensionAndStrictnessQueries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Collection containing only empty Point has isEmpty() == true, yet
  // Point::hasDimension(Dimension::P) is true.
  {
    auto gc_empty_pt = MakeCollection(*factory, factory->createPoint());
    EXPECT_TRUE(gc_empty_pt->isEmpty());
    ExpectDimensionProperties(*gc_empty_pt,
                              {
                                  .dim = Dimension::P,
                                  .boundary_dim = Dimension::False,
                                  .has_p = true,
                                  .has_l = false,
                                  .has_a = false,
                                  .strict_p = true,
                                  .strict_l = true,
                                  .strict_a = true,
                                  .strict_false = true,
                                  .strict_dontcare = true,
                                  .mixed_dim = false,
                              });
  }

  // Homogeneous points.
  {
    auto gc =
        MakeCollection(*factory, factory->createPoint(CoordinateXY(0.0, 0.0)),
                       factory->createPoint(CoordinateXY(1.0, 1.0)));
    ExpectDimensionProperties(*gc, {
                                       .dim = Dimension::P,
                                       .boundary_dim = Dimension::False,
                                       .has_p = true,
                                       .has_l = false,
                                       .has_a = false,
                                       .strict_p = true,
                                       .strict_l = false,
                                       .strict_a = false,
                                       .strict_false = false,
                                       .strict_dontcare = false,
                                       .mixed_dim = false,
                                   });
  }

  // Homogeneous open lines.
  {
    auto gc = MakeCollection(
        *factory,
        MakeLine(*factory, {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0)}),
        MakeLine(*factory, {CoordinateXY(2.0, 0.0), CoordinateXY(3.0, 1.0)}));
    ExpectDimensionProperties(*gc, {
                                       .dim = Dimension::L,
                                       .boundary_dim = Dimension::P,
                                       .has_p = false,
                                       .has_l = true,
                                       .has_a = false,
                                       .strict_p = false,
                                       .strict_l = true,
                                       .strict_a = false,
                                       .strict_false = false,
                                       .strict_dontcare = false,
                                       .mixed_dim = false,
                                   });
  }

  // Homogeneous closed lines have no boundary (Dimension::False).
  {
    auto gc = MakeCollection(
        *factory,
        MakeLine(*factory, {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0),
                            CoordinateXY(0.0, 1.0), CoordinateXY(0.0, 0.0)}));
    EXPECT_EQ(gc->getDimension(), Dimension::L);
    EXPECT_EQ(gc->getBoundaryDimension(), Dimension::False);
  }

  // Homogeneous polygons.
  {
    auto gc = MakeCollection(*factory, MakeBoxPolygon(*factory, 0, 0, 1, 1));
    ExpectDimensionProperties(*gc, {
                                       .dim = Dimension::A,
                                       .boundary_dim = Dimension::L,
                                       .has_p = false,
                                       .has_l = false,
                                       .has_a = true,
                                       .strict_p = false,
                                       .strict_l = false,
                                       .strict_a = true,
                                       .strict_false = false,
                                       .strict_dontcare = false,
                                       .mixed_dim = false,
                                   });
  }

  // Mixed P + L.
  {
    auto gc = MakeCollection(
        *factory, factory->createPoint(CoordinateXY(0.0, 0.0)),
        MakeLine(*factory, {CoordinateXY(1.0, 0.0), CoordinateXY(2.0, 0.0)}));
    ExpectDimensionProperties(*gc, {
                                       .dim = Dimension::L,
                                       .boundary_dim = Dimension::P,
                                       .has_p = true,
                                       .has_l = true,
                                       .has_a = false,
                                       .strict_p = false,
                                       .strict_l = false,
                                       .strict_a = false,
                                       .strict_false = false,
                                       .strict_dontcare = false,
                                       .mixed_dim = true,
                                   });
  }

  // Mixed P + A.
  {
    auto gc =
        MakeCollection(*factory, factory->createPoint(CoordinateXY(0.0, 0.0)),
                       MakeBoxPolygon(*factory, 1, 1, 3, 3));
    ExpectDimensionProperties(*gc, {
                                       .dim = Dimension::A,
                                       .boundary_dim = Dimension::L,
                                       .has_p = true,
                                       .has_l = false,
                                       .has_a = true,
                                       .strict_p = false,
                                       .strict_l = false,
                                       .strict_a = false,
                                       .strict_false = false,
                                       .strict_dontcare = false,
                                       .mixed_dim = true,
                                   });
  }

  // Mixed L + A.
  {
    auto gc = MakeCollection(
        *factory,
        MakeLine(*factory, {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0)}),
        MakeBoxPolygon(*factory, 2, 2, 4, 4));
    ExpectDimensionProperties(*gc, {
                                       .dim = Dimension::A,
                                       .boundary_dim = Dimension::L,
                                       .has_p = false,
                                       .has_l = true,
                                       .has_a = true,
                                       .strict_p = false,
                                       .strict_l = false,
                                       .strict_a = false,
                                       .strict_false = false,
                                       .strict_dontcare = false,
                                       .mixed_dim = true,
                                   });
  }

  // Mixed P + L + A (and verify flag caching via repeated calls).
  {
    auto gc = MakeCollection(
        *factory, factory->createPoint(CoordinateXY(0.0, 0.0)),
        MakeLine(*factory, {CoordinateXY(1.0, 0.0), CoordinateXY(2.0, 0.0)}),
        MakeBoxPolygon(*factory, 3, 3, 5, 5));
    const DimensionExpectation expected = {
        .dim = Dimension::A,
        .boundary_dim = Dimension::L,
        .has_p = true,
        .has_l = true,
        .has_a = true,
        .strict_p = false,
        .strict_l = false,
        .strict_a = false,
        .strict_false = false,
        .strict_dontcare = false,
        .mixed_dim = true,
    };
    ExpectDimensionProperties(*gc, expected);
    // Second pass exercises cached flags in setFlags().
    ExpectDimensionProperties(*gc, expected);
  }

  // Nested mixed collection propagates child dimension flags recursively.
  {
    auto nested_mixed = MakeCollection(
        *factory, factory->createPoint(CoordinateXY(0.0, 0.0)),
        MakeCollection(*factory, MakeBoxPolygon(*factory, 1, 1, 2, 2)));
    ExpectDimensionProperties(*nested_mixed, {
                                                 .dim = Dimension::A,
                                                 .boundary_dim = Dimension::L,
                                                 .has_p = true,
                                                 .has_l = false,
                                                 .has_a = true,
                                                 .strict_p = false,
                                                 .strict_l = false,
                                                 .strict_a = false,
                                                 .strict_false = false,
                                                 .strict_dontcare = false,
                                                 .mixed_dim = true,
                                             });
  }
}

TEST(GeometryCollectionTest, CoordinateDimensionHasZAndHasM) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto xy = MakeCollection(*factory, factory->createPoint(CoordinateXY(1, 2)));
  EXPECT_EQ(static_cast<int>(xy->getCoordinateDimension()), 2);
  EXPECT_FALSE(xy->hasZ());
  EXPECT_FALSE(xy->hasM());

  auto xyz =
      MakeCollection(*factory, factory->createPoint(Coordinate(1, 2, 3)));
  EXPECT_EQ(static_cast<int>(xyz->getCoordinateDimension()), 3);
  EXPECT_TRUE(xyz->hasZ());
  EXPECT_FALSE(xyz->hasM());

  auto xym =
      MakeCollection(*factory, factory->createPoint(CoordinateXYM(1, 2, 4)));
  EXPECT_EQ(static_cast<int>(xym->getCoordinateDimension()), 3);
  EXPECT_FALSE(xym->hasZ());
  EXPECT_TRUE(xym->hasM());

  auto mixed_xyz_xym =
      MakeCollection(*factory, factory->createPoint(Coordinate(1, 2, 3)),
                     factory->createPoint(CoordinateXYM(4, 5, 6)));
  EXPECT_EQ(static_cast<int>(mixed_xyz_xym->getCoordinateDimension()), 3);
  EXPECT_TRUE(mixed_xyz_xym->hasZ());
  EXPECT_TRUE(mixed_xyz_xym->hasM());

  auto xyzm = MakeCollection(*factory, factory->createPoint(CoordinateXY(0, 0)),
                             factory->createPoint(CoordinateXYZM(1, 2, 3, 4)));
  EXPECT_EQ(static_cast<int>(xyzm->getCoordinateDimension()), 4);
  EXPECT_TRUE(xyzm->hasZ());
  EXPECT_TRUE(xyzm->hasM());
  // Repeat hasZ/hasM to exercise cached flags path.
  EXPECT_TRUE(xyzm->hasZ());
  EXPECT_TRUE(xyzm->hasM());

  // Nested Z and M propagation.
  auto nested_zm = MakeCollection(
      *factory,
      MakeCollection(*factory, factory->createPoint(Coordinate(1, 2, 3))),
      MakeCollection(*factory, factory->createPoint(CoordinateXYM(4, 5, 6))));
  EXPECT_EQ(static_cast<int>(nested_zm->getCoordinateDimension()), 3);
  EXPECT_TRUE(nested_zm->hasZ());
  EXPECT_TRUE(nested_zm->hasM());
}

TEST(GeometryCollectionTest, HasCurvedComponentsDetectsCurvedChildren) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto linear =
      MakeCollection(*factory, factory->createPoint(CoordinateXY(0.0, 0.0)),
                     MakeBoxPolygon(*factory, 0.0, 0.0, 2.0, 2.0));
  const Geometry* linear_base = linear.get();
  EXPECT_FALSE(linear_base->hasCurvedComponents());
  EXPECT_NO_THROW(linear_base->getInteriorPoint());

  CoordinateSequence arc_coords(
      {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0), CoordinateXY(2.0, 0.0)});
  auto curved =
      MakeCollection(*factory, factory->createPoint(CoordinateXY(5.0, 5.0)),
                     factory->createCircularString(arc_coords));
  const Geometry* curved_base = curved.get();
  EXPECT_TRUE(curved_base->hasCurvedComponents());
  EXPECT_TRUE(curved_base->hasCurvedComponents());
  EXPECT_THROW(curved_base->getInteriorPoint(), UnsupportedOperationException);

  auto nested_curved = MakeCollection(
      *factory,
      MakeCollection(*factory, factory->createCircularString(arc_coords)));
  const Geometry* nested_base = nested_curved.get();
  EXPECT_TRUE(nested_base->hasCurvedComponents());
}

TEST(GeometryCollectionTest, GetBoundaryThrowsIllegalArgumentException) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto empty = factory->createGeometryCollection();
  EXPECT_THROW(empty->getBoundary(), IllegalArgumentException);

  auto non_empty =
      MakeCollection(*factory, factory->createPoint(CoordinateXY(1.0, 2.0)));
  EXPECT_THROW(non_empty->getBoundary(), IllegalArgumentException);
}

TEST(GeometryCollectionTest, AreaAndLengthSumComponentsRecursively) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto points_only =
      MakeCollection(*factory, factory->createPoint(CoordinateXY(0.0, 0.0)),
                     factory->createPoint(CoordinateXY(3.0, 4.0)));
  EXPECT_DOUBLE_EQ(points_only->getArea(), 0.0);
  EXPECT_DOUBLE_EQ(points_only->getLength(), 0.0);

  // 4x4 outer square (area 16, perimeter 16) with 1x1 hole (area 1, perimeter
  // 4).
  auto shell = MakeRing(*factory, {
                                      CoordinateXY(0.0, 0.0),
                                      CoordinateXY(4.0, 0.0),
                                      CoordinateXY(4.0, 4.0),
                                      CoordinateXY(0.0, 4.0),
                                      CoordinateXY(0.0, 0.0),
                                  });
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(MakeRing(*factory, {
                                         CoordinateXY(1.0, 1.0),
                                         CoordinateXY(1.0, 2.0),
                                         CoordinateXY(2.0, 2.0),
                                         CoordinateXY(2.0, 1.0),
                                         CoordinateXY(1.0, 1.0),
                                     }));
  auto holed_poly = factory->createPolygon(std::move(shell), std::move(holes));

  // Line of length 3 + 4 = 7; nested 2x2 polygon (area 4, perimeter 8).
  auto gc = MakeCollection(
      *factory, factory->createPoint(CoordinateXY(10.0, 10.0)),
      MakeLine(*factory, {CoordinateXY(0.0, 0.0), CoordinateXY(3.0, 0.0),
                          CoordinateXY(3.0, 4.0)}),
      std::move(holed_poly),
      MakeCollection(*factory, MakeBoxPolygon(*factory, 10, 10, 12, 12)));

  EXPECT_DOUBLE_EQ(gc->getArea(), 19.0);
  EXPECT_DOUBLE_EQ(gc->getLength(), 35.0);
}

TEST(GeometryCollectionTest, EnvelopeComputationAndInvalidation) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto gc = MakeCollection(
      *factory, factory->createPoint(),
      factory->createPoint(CoordinateXY(-2.0, 10.0)),
      MakeLine(*factory, {CoordinateXY(5.0, -3.0), CoordinateXY(1.0, 4.0)}));

  const Envelope* env = gc->getEnvelopeInternal();
  ASSERT_FALSE(env->isNull());
  EXPECT_DOUBLE_EQ(env->getMinX(), -2.0);
  EXPECT_DOUBLE_EQ(env->getMaxX(), 5.0);
  EXPECT_DOUBLE_EQ(env->getMinY(), -3.0);
  EXPECT_DOUBLE_EQ(env->getMaxY(), 10.0);

  // Calling again returns cached envelope.
  EXPECT_EQ(gc->getEnvelopeInternal(), env);
  EXPECT_DOUBLE_EQ(gc->getEnvelope()->getArea(), 7.0 * 13.0);

  auto empty_gc = factory->createGeometryCollection();
  EXPECT_TRUE(empty_gc->getEnvelope()->isEmpty());

  // Modify coordinates and notify via geometryChanged(), invalidating envelope.
  ShiftCoordFilter shift(10.0, 20.0);
  gc->apply_rw(&shift);
  gc->geometryChanged();

  const Envelope* updated = gc->getEnvelopeInternal();
  EXPECT_DOUBLE_EQ(updated->getMinX(), 8.0);
  EXPECT_DOUBLE_EQ(updated->getMaxX(), 15.0);
  EXPECT_DOUBLE_EQ(updated->getMinY(), 17.0);
  EXPECT_DOUBLE_EQ(updated->getMaxY(), 30.0);
  EXPECT_DOUBLE_EQ(gc->getEnvelope()->getArea(), 7.0 * 13.0);
}

TEST(GeometryCollectionTest, EqualsExactChecksClassCountElementsAndTolerance) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto gc = MakeCollection(
      *factory, factory->createPoint(CoordinateXY(1.0, 2.0)),
      MakeLine(*factory, {CoordinateXY(3.0, 4.0), CoordinateXY(5.0, 6.0)}));

  EXPECT_TRUE(gc->equalsExact(gc.get()));

  // Class mismatch: Point and MultiPoint (including empty MultiPoint vs empty
  // GC).
  auto pt = factory->createPoint(CoordinateXY(1.0, 2.0));
  EXPECT_FALSE(gc->equalsExact(pt.get()));

  std::vector<std::unique_ptr<Point>> mp_pts;
  mp_pts.push_back(factory->createPoint(CoordinateXY(1.0, 2.0)));
  auto mp = factory->createMultiPoint(std::move(mp_pts));
  EXPECT_FALSE(gc->equalsExact(mp.get()));

  auto empty_gc = factory->createGeometryCollection();
  auto empty_gc2 = factory->createGeometryCollection();
  auto empty_mp = factory->createMultiPoint();
  EXPECT_TRUE(empty_gc->equalsExact(empty_gc2.get()));
  EXPECT_FALSE(empty_gc->equalsExact(empty_mp.get()));

  // Size mismatch.
  auto shorter =
      MakeCollection(*factory, factory->createPoint(CoordinateXY(1.0, 2.0)));
  EXPECT_FALSE(gc->equalsExact(shorter.get()));

  // Same size, element mismatch.
  auto diff_elem = MakeCollection(
      *factory, factory->createPoint(CoordinateXY(1.0, 2.0)),
      MakeLine(*factory, {CoordinateXY(3.0, 4.0), CoordinateXY(9.0, 6.0)}));
  EXPECT_FALSE(gc->equalsExact(diff_elem.get()));

  // Tolerance checks.
  auto near_gc = MakeCollection(
      *factory, factory->createPoint(CoordinateXY(1.01, 2.01)),
      MakeLine(*factory, {CoordinateXY(3.0, 4.0), CoordinateXY(5.01, 6.01)}));
  EXPECT_FALSE(gc->equalsExact(near_gc.get(), 0.0));
  EXPECT_FALSE(gc->equalsExact(near_gc.get(), 0.01));
  EXPECT_TRUE(gc->equalsExact(near_gc.get(), 0.02));
}

TEST(GeometryCollectionTest, EqualsIdenticalChecksClassCountEnvelopeAndDims) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto empty_gc1 = factory->createGeometryCollection();
  auto empty_gc2 = factory->createGeometryCollection();
  auto empty_mp = factory->createMultiPoint();
  EXPECT_TRUE(empty_gc1->equalsIdentical(empty_gc2.get()));
  EXPECT_FALSE(empty_gc1->equalsIdentical(empty_mp.get()));

  auto gc =
      MakeCollection(*factory, factory->createPoint(CoordinateXY(0.0, 0.0)),
                     factory->createPoint(CoordinateXY(1.0, 1.0)));
  auto identical =
      MakeCollection(*factory, factory->createPoint(CoordinateXY(0.0, 0.0)),
                     factory->createPoint(CoordinateXY(1.0, 1.0)));
  EXPECT_TRUE(gc->equalsIdentical(identical.get()));

  // Different class.
  auto pt = factory->createPoint(CoordinateXY(0.0, 0.0));
  EXPECT_FALSE(gc->equalsIdentical(pt.get()));

  // Different element count.
  auto one_pt =
      MakeCollection(*factory, factory->createPoint(CoordinateXY(0.0, 0.0)));
  EXPECT_FALSE(gc->equalsIdentical(one_pt.get()));

  // Same count, different envelope.
  auto other_env =
      MakeCollection(*factory, factory->createPoint(CoordinateXY(0.0, 0.0)),
                     factory->createPoint(CoordinateXY(2.0, 1.0)));
  EXPECT_FALSE(gc->equalsIdentical(other_env.get()));

  // Same count and identical envelope, reordered elements.
  auto swapped =
      MakeCollection(*factory, factory->createPoint(CoordinateXY(1.0, 1.0)),
                     factory->createPoint(CoordinateXY(0.0, 0.0)));
  EXPECT_EQ(*gc->getEnvelopeInternal(), *swapped->getEnvelopeInternal());
  EXPECT_FALSE(gc->equalsIdentical(swapped.get()));

  // Same count and envelope, differing Z/M dimensions or values.
  auto z1 =
      MakeCollection(*factory, factory->createPoint(Coordinate(0.0, 0.0, 1.0)));
  auto z2 =
      MakeCollection(*factory, factory->createPoint(Coordinate(0.0, 0.0, 2.0)));
  auto z1_copy =
      MakeCollection(*factory, factory->createPoint(Coordinate(0.0, 0.0, 1.0)));
  auto xy1 =
      MakeCollection(*factory, factory->createPoint(CoordinateXY(0.0, 0.0)));
  auto xym1 = MakeCollection(
      *factory, factory->createPoint(CoordinateXYM(0.0, 0.0, 5.0)));
  auto xyzm1 = MakeCollection(
      *factory, factory->createPoint(CoordinateXYZM(0.0, 0.0, 1.0, 5.0)));
  auto xyzm1_copy = MakeCollection(
      *factory, factory->createPoint(CoordinateXYZM(0.0, 0.0, 1.0, 5.0)));
  auto nan_z = MakeCollection(
      *factory, factory->createPoint(Coordinate(0.0, 0.0, DoubleNotANumber)));
  auto nan_z_copy = MakeCollection(
      *factory, factory->createPoint(Coordinate(0.0, 0.0, DoubleNotANumber)));

  EXPECT_TRUE(z1->equalsIdentical(z1_copy.get()));
  EXPECT_TRUE(xyzm1->equalsIdentical(xyzm1_copy.get()));
  EXPECT_TRUE(nan_z->equalsIdentical(nan_z_copy.get()));
  EXPECT_FALSE(z1->equalsIdentical(z2.get()));
  EXPECT_FALSE(xy1->equalsIdentical(z1.get()));
  EXPECT_FALSE(xy1->equalsIdentical(xym1.get()));
}

TEST(GeometryCollectionTest, CompareToOrdering) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto empty1 = factory->createGeometryCollection();
  auto empty2 = factory->createGeometryCollection();
  auto p1 =
      MakeCollection(*factory, factory->createPoint(CoordinateXY(1.0, 1.0)));
  auto p1_copy =
      MakeCollection(*factory, factory->createPoint(CoordinateXY(1.0, 1.0)));
  auto p2 =
      MakeCollection(*factory, factory->createPoint(CoordinateXY(2.0, 2.0)));
  auto p1_p2 =
      MakeCollection(*factory, factory->createPoint(CoordinateXY(1.0, 1.0)),
                     factory->createPoint(CoordinateXY(2.0, 2.0)));
  auto p1_p3 =
      MakeCollection(*factory, factory->createPoint(CoordinateXY(1.0, 1.0)),
                     factory->createPoint(CoordinateXY(3.0, 3.0)));
  auto line_gc = MakeCollection(
      *factory,
      MakeLine(*factory, {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0)}));

  EXPECT_EQ(p1->compareTo(p1.get()), 0);
  EXPECT_EQ(p1->compareTo(p1_copy.get()), 0);
  EXPECT_EQ(empty1->compareTo(empty2.get()), 0);
  EXPECT_LT(empty1->compareTo(p1.get()), 0);
  EXPECT_GT(p1->compareTo(empty1.get()), 0);

  EXPECT_LT(p1->compareTo(p2.get()), 0);
  EXPECT_GT(p2->compareTo(p1.get()), 0);

  // Equal prefix resolves on subsequent elements.
  EXPECT_LT(p1_p2->compareTo(p1_p3.get()), 0);
  EXPECT_GT(p1_p3->compareTo(p1_p2.get()), 0);

  // Differing child geometry types at index 0.
  EXPECT_LT(p1->compareTo(line_gc.get()), 0);
  EXPECT_GT(line_gc->compareTo(p1.get()), 0);

  // Prefix collection sorts before longer collection.
  EXPECT_LT(p1->compareTo(p1_p2.get()), 0);
  EXPECT_GT(p1_p2->compareTo(p1.get()), 0);

  // Cross-class sort index comparison.
  auto bare_point = factory->createPoint(CoordinateXY(100.0, 100.0));
  EXPECT_GT(empty1->compareTo(bare_point.get()), 0);
  EXPECT_LT(bare_point->compareTo(empty1.get()), 0);
}

TEST(GeometryCollectionTest, NormalizeSortsDescendingAndNormalizesChildren) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto empty = factory->createGeometryCollection();
  empty->normalize();
  EXPECT_TRUE(empty->isEmpty());

  // CCW polygon starting at non-min vertex (2,0).
  auto ccw_poly = factory->createPolygon(MakeRing(
      *factory,
      {CoordinateXY(2.0, 0.0), CoordinateXY(2.0, 2.0), CoordinateXY(0.0, 2.0),
       CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 0.0)}));
  auto reversed_line =
      MakeLine(*factory, {CoordinateXY(3.0, 0.0), CoordinateXY(1.0, 0.0)});
  auto nested =
      MakeCollection(*factory, factory->createPoint(CoordinateXY(0.0, 1.0)),
                     factory->createPoint(CoordinateXY(4.0, 1.0)));

  auto gc = MakeCollection(
      *factory, factory->createPoint(CoordinateXY(0.0, 0.0)),
      std::move(reversed_line), std::move(ccw_poly),
      factory->createPoint(CoordinateXY(5.0, 5.0)), std::move(nested));

  gc->normalize();

  ASSERT_EQ(gc->getNumGeometries(), 5u);
  // Descending sort index: GeometryCollection (7) > Polygon (5) > LineString
  // (2) > Point (0).
  EXPECT_EQ(gc->getGeometryN(0)->getGeometryType(), "GeometryCollection");
  EXPECT_EQ(gc->getGeometryN(1)->getGeometryType(), "Polygon");
  EXPECT_EQ(gc->getGeometryN(2)->getGeometryType(), "LineString");
  EXPECT_EQ(gc->getGeometryN(3)->getGeometryType(), "Point");
  EXPECT_EQ(gc->getGeometryN(4)->getGeometryType(), "Point");

  // Points sorted descending: (5,5) before (0,0).
  EXPECT_EQ(*gc->getGeometryN(3)->getCoordinate(), CoordinateXY(5.0, 5.0));
  EXPECT_EQ(*gc->getGeometryN(4)->getCoordinate(), CoordinateXY(0.0, 0.0));

  // Nested collection also normalized: (4,1) before (0,1).
  const auto* norm_nested =
      dynamic_cast<const GeometryCollection*>(gc->getGeometryN(0));
  ASSERT_NE(norm_nested, nullptr);
  EXPECT_EQ(*norm_nested->getGeometryN(0)->getCoordinate(),
            CoordinateXY(4.0, 1.0));
  EXPECT_EQ(*norm_nested->getGeometryN(1)->getCoordinate(),
            CoordinateXY(0.0, 1.0));

  // LineString normalized to (1,0) -> (3,0).
  std::unique_ptr<CoordinateSequence> line_coords =
      gc->getGeometryN(2)->getCoordinates();
  EXPECT_EQ(line_coords->getAt<CoordinateXY>(0), CoordinateXY(1.0, 0.0));
  EXPECT_EQ(line_coords->getAt<CoordinateXY>(1), CoordinateXY(3.0, 0.0));

  // Polygon exterior ring normalized to CW starting at min vertex (0,0).
  const auto* norm_poly = dynamic_cast<const Polygon*>(gc->getGeometryN(1));
  ASSERT_NE(norm_poly, nullptr);
  const CoordinateSequence* shell_coords =
      norm_poly->getExteriorRing()->getCoordinatesRO();
  EXPECT_EQ(shell_coords->getAt<CoordinateXY>(0), CoordinateXY(0.0, 0.0));
  EXPECT_EQ(shell_coords->getAt<CoordinateXY>(1), CoordinateXY(0.0, 2.0));

  // Normalize is idempotent.
  auto snapshot = gc->clone();
  gc->normalize();
  EXPECT_TRUE(gc->equalsExact(snapshot.get()));
}

TEST(GeometryCollectionTest, ReverseHandlesEmptyAndPreservesComponentOrder) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto empty = factory->createGeometryCollection();
  std::unique_ptr<GeometryCollection> empty_rev = empty->reverse();
  EXPECT_TRUE(empty_rev->isEmpty());
  EXPECT_EQ(empty_rev->getGeometryTypeId(), GEOS_GEOMETRYCOLLECTION);

  auto only_empties = MakeCollection(*factory, factory->createPoint(),
                                     factory->createLineString());
  std::unique_ptr<GeometryCollection> only_empties_rev =
      only_empties->reverse();
  EXPECT_TRUE(only_empties_rev->isEmpty());
  EXPECT_EQ(only_empties_rev->getNumGeometries(), 2u);

  auto gc = MakeCollection(
      *factory, factory->createPoint(CoordinateXY(1.0, 2.0)),
      MakeLine(*factory, {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0),
                          CoordinateXY(2.0, 1.0)}),
      MakeBoxPolygon(*factory, 0.0, 0.0, 2.0, 2.0),
      MakeCollection(*factory, MakeLine(*factory, {CoordinateXY(0.0, 0.0),
                                                   CoordinateXY(0.0, 5.0)})));

  std::unique_ptr<GeometryCollection> rev = gc->reverse();
  ASSERT_EQ(rev->getNumGeometries(), 4u);

  // Component order is preserved.
  EXPECT_EQ(rev->getGeometryN(0)->getGeometryType(), "Point");
  EXPECT_EQ(rev->getGeometryN(1)->getGeometryType(), "LineString");
  EXPECT_EQ(rev->getGeometryN(2)->getGeometryType(), "Polygon");
  EXPECT_EQ(rev->getGeometryN(3)->getGeometryType(), "GeometryCollection");

  // LineString vertices are reversed.
  std::unique_ptr<CoordinateSequence> rev_line =
      rev->getGeometryN(1)->getCoordinates();
  ASSERT_EQ(rev_line->getSize(), 3u);
  EXPECT_EQ(rev_line->getAt<CoordinateXY>(0), CoordinateXY(2.0, 1.0));
  EXPECT_EQ(rev_line->getAt<CoordinateXY>(1), CoordinateXY(1.0, 0.0));
  EXPECT_EQ(rev_line->getAt<CoordinateXY>(2), CoordinateXY(0.0, 0.0));

  // Polygon ring vertex order is reversed (CW instead of CCW).
  const auto* rev_poly = dynamic_cast<const Polygon*>(rev->getGeometryN(2));
  ASSERT_NE(rev_poly, nullptr);
  EXPECT_EQ(
      rev_poly->getExteriorRing()->getCoordinatesRO()->getAt<CoordinateXY>(1),
      CoordinateXY(0.0, 2.0));

  // Nested collection components are also reversed.
  const auto* rev_nested =
      dynamic_cast<const GeometryCollection*>(rev->getGeometryN(3));
  ASSERT_NE(rev_nested, nullptr);
  std::unique_ptr<CoordinateSequence> rev_nested_line =
      rev_nested->getGeometryN(0)->getCoordinates();
  EXPECT_EQ(rev_nested_line->getAt<CoordinateXY>(0), CoordinateXY(0.0, 5.0));
  EXPECT_EQ(rev_nested_line->getAt<CoordinateXY>(1), CoordinateXY(0.0, 0.0));

  // Original collection remains untouched.
  EXPECT_EQ(gc->getGeometryN(1)->getCoordinates()->getAt<CoordinateXY>(0),
            CoordinateXY(0.0, 0.0));
}

TEST(GeometryCollectionTest, CoordinateFilterReadOnlyAndReadWrite) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto empty = factory->createGeometryCollection();
  CollectCoordFilter empty_ro;
  empty->apply_ro(&empty_ro);
  EXPECT_THAT(empty_ro.coords(), IsEmpty());
  ShiftCoordFilter empty_rw(1.0, 1.0);
  empty->apply_rw(&empty_rw);
  EXPECT_TRUE(empty->isEmpty());

  auto gc = MakeCollection(
      *factory, factory->createPoint(CoordinateXY(1.0, 2.0)),
      MakeLine(*factory, {CoordinateXY(3.0, 4.0), CoordinateXY(5.0, 6.0)}),
      MakeCollection(*factory, factory->createPoint(CoordinateXY(7.0, 8.0))));

  CollectCoordFilter ro_filter;
  gc->apply_ro(&ro_filter);
  EXPECT_THAT(ro_filter.coords(),
              ElementsAre(CoordinateXY(1.0, 2.0), CoordinateXY(3.0, 4.0),
                          CoordinateXY(5.0, 6.0), CoordinateXY(7.0, 8.0)));

  ShiftCoordFilter rw_filter(10.0, -2.0);
  gc->apply_rw(&rw_filter);
  gc->geometryChanged();

  CollectCoordFilter after_filter;
  gc->apply_ro(&after_filter);
  EXPECT_THAT(after_filter.coords(),
              ElementsAre(CoordinateXY(11.0, 0.0), CoordinateXY(13.0, 2.0),
                          CoordinateXY(15.0, 4.0), CoordinateXY(17.0, 6.0)));
  EXPECT_DOUBLE_EQ(gc->getEnvelopeInternal()->getMinX(), 11.0);
  EXPECT_DOUBLE_EQ(gc->getEnvelopeInternal()->getMaxX(), 17.0);
}

TEST(GeometryCollectionTest, GeometryFilterReadOnlyAndReadWrite) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto empty = factory->createGeometryCollection();
  RecordingGeometryFilter empty_filter;
  empty->apply_ro(&empty_filter);
  empty->apply_rw(&empty_filter);
  EXPECT_THAT(empty_filter.ro_types(), ElementsAre("GeometryCollection"));
  EXPECT_THAT(empty_filter.rw_types(), ElementsAre("GeometryCollection"));

  auto gc = MakeCollection(
      *factory, factory->createPoint(CoordinateXY(1.0, 2.0)),
      MakeLine(*factory, {CoordinateXY(3.0, 4.0), CoordinateXY(5.0, 6.0)}),
      MakeCollection(*factory, factory->createPoint(CoordinateXY(7.0, 8.0))));

  RecordingGeometryFilter filter;
  int tag = 123;
  filter.set_user_data_tag(&tag);

  gc->apply_ro(&filter);
  EXPECT_THAT(filter.ro_types(),
              ElementsAre("GeometryCollection", "Point", "LineString",
                          "GeometryCollection", "Point"));

  gc->apply_rw(&filter);
  EXPECT_THAT(filter.rw_types(),
              ElementsAre("GeometryCollection", "Point", "LineString",
                          "GeometryCollection", "Point"));
  EXPECT_EQ(gc->getUserData(), &tag);
  EXPECT_EQ(gc->getGeometryN(0)->getUserData(), &tag);
  EXPECT_EQ(gc->getGeometryN(1)->getUserData(), &tag);
  EXPECT_EQ(gc->getGeometryN(2)->getUserData(), &tag);
  const auto* inner_gc =
      dynamic_cast<const GeometryCollection*>(gc->getGeometryN(2));
  ASSERT_NE(inner_gc, nullptr);
  EXPECT_EQ(inner_gc->getGeometryN(0)->getUserData(), &tag);
}

TEST(GeometryCollectionTest, GeometryComponentFilterFullAndShortCircuit) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto empty = factory->createGeometryCollection();
  RecordingComponentFilter empty_ro;
  empty->apply_ro(&empty_ro);
  EXPECT_THAT(empty_ro.ro_types(), ElementsAre("GeometryCollection"));
  RecordingComponentFilter empty_rw;
  empty->apply_rw(&empty_rw);
  EXPECT_THAT(empty_rw.rw_types(), ElementsAre("GeometryCollection"));

  auto gc = MakeCollection(
      *factory, factory->createPoint(CoordinateXY(1.0, 2.0)),
      MakeBoxPolygon(*factory, 0.0, 0.0, 2.0, 2.0),
      MakeCollection(*factory, factory->createPoint(CoordinateXY(5.0, 6.0))));

  // Full RO and RW traversal.
  RecordingComponentFilter full_ro;
  gc->apply_ro(&full_ro);
  EXPECT_THAT(full_ro.ro_types(),
              ElementsAre("GeometryCollection", "Point", "Polygon",
                          "LinearRing", "GeometryCollection", "Point"));

  RecordingComponentFilter full_rw;
  gc->apply_rw(&full_rw);
  EXPECT_THAT(full_rw.rw_types(),
              ElementsAre("GeometryCollection", "Point", "Polygon",
                          "LinearRing", "GeometryCollection", "Point"));

  // Immediate short-circuit on root GeometryCollection.
  RecordingComponentFilter stop_at_root_ro(1);
  gc->apply_ro(&stop_at_root_ro);
  EXPECT_THAT(stop_at_root_ro.ro_types(), ElementsAre("GeometryCollection"));

  RecordingComponentFilter stop_at_root_rw(1);
  gc->apply_rw(&stop_at_root_rw);
  EXPECT_THAT(stop_at_root_rw.rw_types(), ElementsAre("GeometryCollection"));

  // Short-circuit after first child component.
  RecordingComponentFilter stop_after_first_child_ro(2);
  gc->apply_ro(&stop_after_first_child_ro);
  EXPECT_THAT(stop_after_first_child_ro.ro_types(),
              ElementsAre("GeometryCollection", "Point"));

  RecordingComponentFilter stop_after_first_child_rw(2);
  gc->apply_rw(&stop_after_first_child_rw);
  EXPECT_THAT(stop_after_first_child_rw.rw_types(),
              ElementsAre("GeometryCollection", "Point"));
}

TEST(GeometryCollectionTest, CoordinateSequenceFilterFullAndShortCircuit) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto empty = factory->createGeometryCollection();
  RecordingSeqFilter empty_ro(/*mutate=*/false, /*report_changed=*/false,
                              /*max_visits=*/0);
  empty->apply_ro(empty_ro);
  EXPECT_THAT(empty_ro.visited(), IsEmpty());
  RecordingSeqFilter empty_rw(/*mutate=*/false, /*report_changed=*/false,
                              /*max_visits=*/0);
  empty->apply_rw(empty_rw);
  EXPECT_THAT(empty_rw.visited(), IsEmpty());

  auto gc = MakeCollection(
      *factory, factory->createPoint(CoordinateXY(1.0, 2.0)),
      MakeLine(*factory, {CoordinateXY(3.0, 4.0), CoordinateXY(5.0, 6.0)}),
      factory->createPoint(CoordinateXY(7.0, 8.0)));

  // RO full traversal.
  RecordingSeqFilter ro_full(/*mutate=*/false, /*report_changed=*/false,
                             /*max_visits=*/0);
  gc->apply_ro(ro_full);
  EXPECT_THAT(ro_full.visited(),
              ElementsAre(CoordinateXY(1.0, 2.0), CoordinateXY(3.0, 4.0),
                          CoordinateXY(5.0, 6.0), CoordinateXY(7.0, 8.0)));

  // RO short-circuit after first component breaks GeometryCollection loop.
  RecordingSeqFilter ro_short(/*mutate=*/false, /*report_changed=*/false,
                              /*max_visits=*/1);
  gc->apply_ro(ro_short);
  EXPECT_THAT(ro_short.visited(), ElementsAre(CoordinateXY(1.0, 2.0)));

  // RW with isGeometryChanged() == false skips geometryChanged().
  RecordingSeqFilter rw_unchanged(/*mutate=*/false, /*report_changed=*/false,
                                  /*max_visits=*/0);
  gc->apply_rw(rw_unchanged);
  EXPECT_THAT(rw_unchanged.visited(), SizeIs(4));
  EXPECT_DOUBLE_EQ(gc->getEnvelopeInternal()->getMinX(), 1.0);

  // RW short-circuit after first component without mutation.
  RecordingSeqFilter rw_short(/*mutate=*/false, /*report_changed=*/false,
                              /*max_visits=*/1);
  gc->apply_rw(rw_short);
  EXPECT_THAT(rw_short.visited(), ElementsAre(CoordinateXY(1.0, 2.0)));

  // RW short-circuit after mutating first component still triggers
  // geometryChanged() without visiting remaining components.
  auto partial = gc->clone();
  RecordingSeqFilter rw_short_mutating(/*mutate=*/true, /*report_changed=*/true,
                                       /*max_visits=*/1);
  partial->apply_rw(rw_short_mutating);
  EXPECT_THAT(rw_short_mutating.visited(), ElementsAre(CoordinateXY(1.0, 2.0)));
  EXPECT_DOUBLE_EQ(partial->getEnvelopeInternal()->getMinX(), 3.0);
  EXPECT_DOUBLE_EQ(partial->getEnvelopeInternal()->getMaxX(), 101.0);
  EXPECT_DOUBLE_EQ(partial->getEnvelopeInternal()->getMaxY(), 202.0);

  // RW mutating traversal with isGeometryChanged() == true updates envelope.
  RecordingSeqFilter rw_mutating(/*mutate=*/true, /*report_changed=*/true,
                                 /*max_visits=*/0);
  gc->apply_rw(rw_mutating);
  EXPECT_THAT(rw_mutating.visited(), SizeIs(4));
  EXPECT_DOUBLE_EQ(gc->getEnvelopeInternal()->getMinX(), 101.0);
  EXPECT_DOUBLE_EQ(gc->getEnvelopeInternal()->getMaxX(), 107.0);
  EXPECT_DOUBLE_EQ(gc->getEnvelopeInternal()->getMinY(), 202.0);
  EXPECT_DOUBLE_EQ(gc->getEnvelopeInternal()->getMaxY(), 208.0);
}

}  // namespace
}  // namespace geos_google
