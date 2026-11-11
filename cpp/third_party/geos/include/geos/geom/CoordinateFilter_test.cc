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

// Unit tests for third_party/geos/include/geos/geom/CoordinateFilter.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/geom/CoordinateFilter.h

#include "third_party/geos/include/geos/geom/CoordinateFilter.h"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateFilter;
using ::geos::geom::CoordinateInspector;
using ::geos::geom::CoordinateMutator;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateType;
using ::geos::geom::CoordinateXY;
using ::geos::geom::CoordinateXYM;
using ::geos::geom::CoordinateXYZM;
using ::geos::geom::Geometry;
using ::geos::geom::GeometryFactory;
using ::geos::geom::LinearRing;

struct DummyInspector : public CoordinateInspector<DummyInspector> {
  void filter(const CoordinateXY*) {}
  void filter(const Coordinate*) {}
  void filter(const CoordinateXYM*) {}
  void filter(const CoordinateXYZM*) {}
};

struct DummyMutator : public CoordinateMutator<DummyMutator> {
  void filter(CoordinateXY*) const {}
  void filter(Coordinate*) const {}
  void filter(CoordinateXYM*) const {}
  void filter(CoordinateXYZM*) const {}
};

static_assert(std::has_virtual_destructor_v<CoordinateFilter>);
static_assert(std::is_base_of_v<CoordinateFilter, DummyInspector>);
static_assert(std::is_base_of_v<CoordinateFilter, DummyMutator>);

class DestructorTrackingFilter : public CoordinateFilter {
 public:
  explicit DestructorTrackingFilter(bool* destroyed) : destroyed_(destroyed) {}
  ~DestructorTrackingFilter() override { *destroyed_ = true; }

 private:
  bool* destroyed_;
};

class XyOnlyFilter : public CoordinateFilter {
 public:
  using CoordinateFilter::filter_ro;
  using CoordinateFilter::filter_rw;

  XyOnlyFilter(double dx, double dy) : dx_(dx), dy_(dy) {}

  void filter_rw(CoordinateXY* coord) const override {
    coord->x += dx_;
    coord->y += dy_;
    ++rw_count;
  }

  void filter_ro(const CoordinateXY* coord) override {
    sum_x += coord->x;
    sum_y += coord->y;
    ++ro_count;
  }

  mutable std::size_t rw_count = 0;
  std::size_t ro_count = 0;
  double sum_x = 0.0;
  double sum_y = 0.0;

 private:
  double dx_;
  double dy_;
};

class TypedInspector : public CoordinateInspector<TypedInspector> {
 public:
  void filter(const CoordinateXY* c) {
    visited_types.push_back(CoordinateType::XY);
    total += c->x + c->y;
  }
  void filter(const Coordinate* c) {
    visited_types.push_back(CoordinateType::XYZ);
    total += c->x + c->y + c->z;
  }
  void filter(const CoordinateXYM* c) {
    visited_types.push_back(CoordinateType::XYM);
    total += c->x + c->y + c->m;
  }
  void filter(const CoordinateXYZM* c) {
    visited_types.push_back(CoordinateType::XYZM);
    total += c->x + c->y + c->z + c->m;
  }

  std::vector<CoordinateType> visited_types;
  double total = 0.0;
};

class GenericInspector : public CoordinateInspector<GenericInspector> {
 public:
  explicit GenericInspector(std::size_t max_visits = 1000)
      : max_visits_(max_visits) {}

  bool isDone() const override { return visits >= max_visits_; }

  template <typename CoordT>
  void filter(const CoordT* c) {
    sum_x += c->x;
    sum_y += c->y;
    ++visits;
  }

  std::size_t visits = 0;
  double sum_x = 0.0;
  double sum_y = 0.0;

 private:
  std::size_t max_visits_;
};

class TypedMutator : public CoordinateMutator<TypedMutator> {
 public:
  void filter(CoordinateXY* c) const {
    c->x += 1.0;
    c->y += 2.0;
  }
  void filter(Coordinate* c) const {
    c->x += 1.0;
    c->y += 2.0;
    c->z += 10.0;
  }
  void filter(CoordinateXYM* c) const {
    c->x += 1.0;
    c->y += 2.0;
    c->m += 20.0;
  }
  void filter(CoordinateXYZM* c) const {
    c->x += 1.0;
    c->y += 2.0;
    c->z += 100.0;
    c->m += 200.0;
  }
};

class GenericMutator : public CoordinateMutator<GenericMutator> {
 public:
  GenericMutator(double dx, double dy, std::size_t max_visits = 1000)
      : dx_(dx), dy_(dy), max_visits_(max_visits) {}

  bool isDone() const override { return visits >= max_visits_; }

  template <typename CoordT>
  void filter(CoordT* c) const {
    c->x += dx_;
    c->y += dy_;
    ++visits;
  }

  mutable std::size_t visits = 0;

 private:
  double dx_;
  double dy_;
  std::size_t max_visits_;
};

TEST(CoordinateFilterTest, DefaultIsDoneAndVirtualDestructor) {
  CoordinateFilter base_filter;
  EXPECT_FALSE(base_filter.isDone());

  XyOnlyFilter derived_filter(1.0, 2.0);
  const CoordinateFilter& base_ref = derived_filter;
  EXPECT_FALSE(base_ref.isDone());

  bool destroyed = false;
  {
    std::unique_ptr<CoordinateFilter> tracked =
        std::make_unique<DestructorTrackingFilter>(&destroyed);
    EXPECT_FALSE(destroyed);
    EXPECT_FALSE(tracked->isDone());
  }
  EXPECT_TRUE(destroyed);

  std::unique_ptr<CoordinateFilter> inspector =
      std::make_unique<TypedInspector>();
  EXPECT_FALSE(inspector->isDone());

  std::unique_ptr<CoordinateFilter> mutator = std::make_unique<TypedMutator>();
  EXPECT_FALSE(mutator->isDone());
}

TEST(CoordinateFilterTest, BaseForwardsHigherDimensionalCoordsToXyOverrides) {
  XyOnlyFilter filter(10.0, -5.0);
  CoordinateFilter* base_rw = &filter;
  const CoordinateFilter* const_rw = &filter;

  CoordinateXY xy(1.0, 2.0);
  Coordinate xyz(3.0, 4.0, 99.0);
  CoordinateXYM xym(5.0, 6.0, 88.0);
  CoordinateXYZM xyzm(7.0, 8.0, 77.0, 66.0);

  // Read-only forwarding across all four coordinate dimensions.
  base_rw->filter_ro(&xy);
  base_rw->filter_ro(&xyz);
  base_rw->filter_ro(&xym);
  base_rw->filter_ro(&xyzm);
  EXPECT_EQ(filter.ro_count, 4u);
  EXPECT_DOUBLE_EQ(filter.sum_x, 16.0);
  EXPECT_DOUBLE_EQ(filter.sum_y, 20.0);

  // Read-write forwarding preserves Z and M ordinates untouched.
  const_rw->filter_rw(&xy);
  const_rw->filter_rw(&xyz);
  const_rw->filter_rw(&xym);
  const_rw->filter_rw(&xyzm);
  EXPECT_EQ(filter.rw_count, 4u);

  EXPECT_DOUBLE_EQ(xy.x, 11.0);
  EXPECT_DOUBLE_EQ(xy.y, -3.0);

  EXPECT_DOUBLE_EQ(xyz.x, 13.0);
  EXPECT_DOUBLE_EQ(xyz.y, -1.0);
  EXPECT_DOUBLE_EQ(xyz.z, 99.0);

  EXPECT_DOUBLE_EQ(xym.x, 15.0);
  EXPECT_DOUBLE_EQ(xym.y, 1.0);
  EXPECT_DOUBLE_EQ(xym.m, 88.0);

  EXPECT_DOUBLE_EQ(xyzm.x, 17.0);
  EXPECT_DOUBLE_EQ(xyzm.y, 3.0);
  EXPECT_DOUBLE_EQ(xyzm.z, 77.0);
  EXPECT_DOUBLE_EQ(xyzm.m, 66.0);

  // Also test through CoordinateSequence::apply_ro and apply_rw.
  CoordinateSequence seq(
      {Coordinate(1.0, 2.0, 5.0), Coordinate(3.0, 4.0, 6.0)});
  seq.apply_rw(&filter);
  EXPECT_EQ(filter.rw_count, 6u);
  EXPECT_DOUBLE_EQ(seq.getX(0), 11.0);
  EXPECT_DOUBLE_EQ(seq.getY(0), -3.0);
  EXPECT_DOUBLE_EQ(seq.getOrdinate(0, CoordinateSequence::Z), 5.0);

  filter.sum_x = 0.0;
  filter.sum_y = 0.0;
  seq.apply_ro(&filter);
  EXPECT_EQ(filter.ro_count, 6u);
  EXPECT_DOUBLE_EQ(filter.sum_x, 24.0);
  EXPECT_DOUBLE_EQ(filter.sum_y, -4.0);
}

TEST(CoordinateFilterTest, CoordinateInspectorDispatchesAllCoordinateTypes) {
  TypedInspector typed;
  CoordinateFilter* base = &typed;

  const CoordinateXY xy(1.0, 2.0);
  const Coordinate xyz(1.0, 2.0, 3.0);
  const CoordinateXYM xym(1.0, 2.0, 4.0);
  const CoordinateXYZM xyzm(1.0, 2.0, 3.0, 4.0);

  base->filter_ro(&xy);
  base->filter_ro(&xyz);
  base->filter_ro(&xym);
  base->filter_ro(&xyzm);

  ASSERT_EQ(typed.visited_types.size(), 4u);
  EXPECT_EQ(typed.visited_types[0], CoordinateType::XY);
  EXPECT_EQ(typed.visited_types[1], CoordinateType::XYZ);
  EXPECT_EQ(typed.visited_types[2], CoordinateType::XYM);
  EXPECT_EQ(typed.visited_types[3], CoordinateType::XYZM);
  EXPECT_DOUBLE_EQ(typed.total, 3.0 + 6.0 + 7.0 + 10.0);

  // Generic template inspector with isDone() early-stop on CoordinateSequence.
  CoordinateSequence seq({CoordinateXY(1.0, 10.0), CoordinateXY(2.0, 20.0),
                          CoordinateXY(3.0, 30.0)});
  GenericInspector limited(2);
  seq.apply_ro(&limited);
  EXPECT_EQ(limited.visits, 2u);
  EXPECT_DOUBLE_EQ(limited.sum_x, 3.0);
  EXPECT_DOUBLE_EQ(limited.sum_y, 30.0);

  CoordinateSequence xyzm_seq({CoordinateXYZM(1.0, 2.0, 3.0, 4.0)});
  TypedInspector seq_typed;
  xyzm_seq.apply_ro(&seq_typed);
  ASSERT_EQ(seq_typed.visited_types.size(), 1u);
  EXPECT_EQ(seq_typed.visited_types[0], CoordinateType::XYZM);
  EXPECT_DOUBLE_EQ(seq_typed.total, 10.0);
}

TEST(CoordinateFilterTest, CoordinateMutatorDispatchesAllCoordinateTypes) {
  TypedMutator typed;
  const CoordinateFilter* base = &typed;

  CoordinateXY xy(1.0, 2.0);
  Coordinate xyz(1.0, 2.0, 3.0);
  CoordinateXYM xym(1.0, 2.0, 4.0);
  CoordinateXYZM xyzm(1.0, 2.0, 3.0, 4.0);

  base->filter_rw(&xy);
  base->filter_rw(&xyz);
  base->filter_rw(&xym);
  base->filter_rw(&xyzm);

  EXPECT_DOUBLE_EQ(xy.x, 2.0);
  EXPECT_DOUBLE_EQ(xy.y, 4.0);

  EXPECT_DOUBLE_EQ(xyz.x, 2.0);
  EXPECT_DOUBLE_EQ(xyz.y, 4.0);
  EXPECT_DOUBLE_EQ(xyz.z, 13.0);

  EXPECT_DOUBLE_EQ(xym.x, 2.0);
  EXPECT_DOUBLE_EQ(xym.y, 4.0);
  EXPECT_DOUBLE_EQ(xym.m, 24.0);

  EXPECT_DOUBLE_EQ(xyzm.x, 2.0);
  EXPECT_DOUBLE_EQ(xyzm.y, 4.0);
  EXPECT_DOUBLE_EQ(xyzm.z, 103.0);
  EXPECT_DOUBLE_EQ(xyzm.m, 204.0);

  // Generic template mutator with isDone() early-stop on CoordinateSequence.
  CoordinateSequence seq({Coordinate(0.0, 0.0, 1.0), Coordinate(1.0, 1.0, 2.0),
                          Coordinate(2.0, 2.0, 3.0)});
  GenericMutator limited(5.0, -5.0, 2);
  seq.apply_rw(&limited);
  EXPECT_EQ(limited.visits, 2u);
  EXPECT_DOUBLE_EQ(seq.getX(0), 5.0);
  EXPECT_DOUBLE_EQ(seq.getY(0), -5.0);
  EXPECT_DOUBLE_EQ(seq.getX(1), 6.0);
  EXPECT_DOUBLE_EQ(seq.getY(1), -4.0);
  EXPECT_DOUBLE_EQ(seq.getX(2), 2.0);
  EXPECT_DOUBLE_EQ(seq.getY(2), 2.0);

  CoordinateSequence xyzm_seq({CoordinateXYZM(1.0, 2.0, 3.0, 4.0)});
  xyzm_seq.apply_rw(&typed);
  EXPECT_DOUBLE_EQ(xyzm_seq.getX(0), 2.0);
  EXPECT_DOUBLE_EQ(xyzm_seq.getY(0), 4.0);
  EXPECT_DOUBLE_EQ(xyzm_seq.getOrdinate(0, CoordinateSequence::Z), 103.0);
  EXPECT_DOUBLE_EQ(xyzm_seq.getOrdinate(0, CoordinateSequence::M), 204.0);
}

TEST(CoordinateFilterTest, AppliesThroughGeometryHierarchy) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto pt = factory->createPoint(Coordinate(1.0, 2.0, 3.0));
  auto line = factory->createLineString(
      std::make_unique<CoordinateSequence>(std::initializer_list<Coordinate>{
          Coordinate(0.0, 0.0, 1.0), Coordinate(2.0, 0.0, 2.0)}));

  auto shell_seq =
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});
  auto hole_seq =
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {2.0, 2.0}, {4.0, 2.0}, {4.0, 4.0}, {2.0, 4.0}, {2.0, 2.0}});
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(factory->createLinearRing(std::move(hole_seq)));
  auto poly = factory->createPolygon(
      factory->createLinearRing(std::move(shell_seq)), std::move(holes));

  std::vector<std::unique_ptr<Geometry>> geoms;
  geoms.push_back(std::move(pt));
  geoms.push_back(std::move(line));
  geoms.push_back(std::move(poly));
  auto collection = factory->createGeometryCollection(std::move(geoms));

  GenericMutator shift(100.0, -100.0);
  collection->apply_rw(&shift);
  collection->geometryChanged();
  EXPECT_EQ(shift.visits, 13u);

  GenericInspector inspector;
  collection->apply_ro(&inspector);
  EXPECT_EQ(inspector.visits, 13u);
  // Sum of original X = 1 + (0+2) + (0+10+10+0+0) + (2+4+4+2+2) = 37,
  // plus 13 * 100 = 1337.
  EXPECT_DOUBLE_EQ(inspector.sum_x, 1337.0);
  // Sum of original Y = 2 + (0+0) + (0+0+10+10+0) + (2+2+4+4+2) = 36,
  // minus 13 * 100 = -1264.
  EXPECT_DOUBLE_EQ(inspector.sum_y, -1264.0);
}

#ifdef NDEBUG
TEST(CoordinateFilterTest, BaseXyMethodsAreNoOpsUnderNdDebug) {
  CoordinateFilter base_filter;
  CoordinateXY rw_coord(1.0, 2.0);
  const CoordinateXY ro_coord(3.0, 4.0);
  base_filter.filter_rw(&rw_coord);
  base_filter.filter_ro(&ro_coord);
  EXPECT_DOUBLE_EQ(rw_coord.x, 1.0);
  EXPECT_DOUBLE_EQ(rw_coord.y, 2.0);
  EXPECT_DOUBLE_EQ(ro_coord.x, 3.0);
  EXPECT_DOUBLE_EQ(ro_coord.y, 4.0);
}
#elif defined(GTEST_HAS_DEATH_TEST)
TEST(CoordinateFilterTest, BaseXyMethodsAbortWithoutNdDebug) {
  CoordinateFilter base_filter;
  CoordinateXY rw_coord(1.0, 2.0);
  const CoordinateXY ro_coord(3.0, 4.0);
  EXPECT_DEATH(base_filter.filter_rw(&rw_coord), "");
  EXPECT_DEATH(base_filter.filter_ro(&ro_coord), "");
}
#endif

}  // namespace
}  // namespace geos_google
