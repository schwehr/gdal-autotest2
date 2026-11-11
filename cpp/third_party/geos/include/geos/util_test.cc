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

// Unit tests for third_party/geos/include/geos/util.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/util.h

#include "third_party/geos/include/geos/util.h"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
#include "third_party/geos/include/geos/geom/CompoundCurve.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Curve.h"
#include "third_party/geos/include/geos/geom/CurvePolygon.h"
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
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::ignore_unused_variable_warning;
using geos::detail::down_cast;
using geos::detail::make_unique;
using geos::geom::CircularString;
using geos::geom::CompoundCurve;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Curve;
using geos::geom::CurvePolygon;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::MultiCurve;
using geos::geom::MultiLineString;
using geos::geom::MultiPoint;
using geos::geom::MultiPolygon;
using geos::geom::MultiSurface;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::SimpleCurve;
using geos::geom::Surface;
using geos::util::ensureNoCurvedComponents;
using geos::util::UnsupportedOperationException;

struct NonCopyableValue {
  explicit NonCopyableValue(int v) : value(v) {}
  NonCopyableValue(const NonCopyableValue&) = delete;
  NonCopyableValue& operator=(const NonCopyableValue&) = delete;
  NonCopyableValue(NonCopyableValue&&) = default;
  NonCopyableValue& operator=(NonCopyableValue&&) = default;

  int value;
};

struct TrackedObject {
  TrackedObject(int id_in, double weight_in, int* destroyed_count_in)
      : id(id_in), weight(weight_in), destroyed_count(destroyed_count_in) {}

  TrackedObject(const TrackedObject&) = delete;
  TrackedObject& operator=(const TrackedObject&) = delete;

  ~TrackedObject() {
    if (destroyed_count != nullptr) {
      ++(*destroyed_count);
    }
  }

  int id;
  double weight;
  int* destroyed_count;
};

struct Base {
  explicit Base(int b) : base_id(b) {}
  virtual ~Base() = default;
  int base_id;
};

struct Derived : Base {
  Derived(int b, int d) : Base(b), derived_id(d) {}
  int derived_id;
};

struct GrandChild final : Derived {
  GrandChild(int b, int d, int g) : Derived(b, d), grand_id(g) {}
  int grand_id;
};

struct SecondBase {
  explicit SecondBase(int s) : second_id(s) {}
  virtual ~SecondBase() = default;
  int second_id;
};

struct MultiDerived final : Base, SecondBase {
  MultiDerived(int b, int s, int m) : Base(b), SecondBase(s), multi_id(m) {}
  int multi_id;
};

struct StubCurvedCheck {
  bool curved = false;
  bool hasCurvedComponents() const { return curved; }
};

TEST(UtilTest, IgnoreUnusedVariableWarningAcceptsVariedTypes) {
  int lvalue = 42;
  const double const_lvalue = 3.25;
  const NonCopyableValue non_copyable(7);
  const void* null_ptr = nullptr;

  ignore_unused_variable_warning(lvalue);
  ignore_unused_variable_warning(const_lvalue);
  ignore_unused_variable_warning(non_copyable);
  ignore_unused_variable_warning(null_ptr);
  ignore_unused_variable_warning(123);
  ignore_unused_variable_warning(std::string("temporary"));

  EXPECT_EQ(lvalue, 42);
  EXPECT_DOUBLE_EQ(const_lvalue, 3.25);
  EXPECT_EQ(non_copyable.value, 7);
}

TEST(UtilTest, DetailMakeUniqueForwardsArgumentsAndManagesLifetime) {
  static_assert(
      std::is_same_v<decltype(make_unique<int>(0)), std::unique_ptr<int>>);
  static_assert(
      std::is_same_v<decltype(make_unique<int[]>(2)), std::unique_ptr<int[]>>);

  auto scalar = make_unique<int>(99);
  ASSERT_NE(scalar, nullptr);
  EXPECT_EQ(*scalar, 99);

  int destroyed_count = 0;
  {
    auto tracked = make_unique<TrackedObject>(7, 2.5, &destroyed_count);
    ASSERT_NE(tracked, nullptr);
    EXPECT_EQ(tracked->id, 7);
    EXPECT_DOUBLE_EQ(tracked->weight, 2.5);
    EXPECT_EQ(destroyed_count, 0);
  }
  EXPECT_EQ(destroyed_count, 1);

  auto arr = make_unique<int[]>(4);
  ASSERT_NE(arr, nullptr);
  for (std::size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(arr[i], 0);
    arr[i] = static_cast<int>(i) + 10;
  }
  EXPECT_EQ(arr[3], 13);

  auto coord = make_unique<CoordinateXY>(3.0, 4.0);
  EXPECT_DOUBLE_EQ(coord->x, 3.0);
  EXPECT_DOUBLE_EQ(coord->y, 4.0);
}

TEST(UtilTest, DownCastHandlesNullptrConstnessAndInheritance) {
  Base* null_base = nullptr;
  const Base* null_const_base = nullptr;
  EXPECT_EQ(down_cast<Derived*>(null_base), nullptr);
  EXPECT_EQ(down_cast<const Derived*>(null_const_base), nullptr);
  EXPECT_EQ(down_cast<GrandChild*>(null_base), nullptr);

  Derived derived(10, 20);
  Base* base_ptr = &derived;
  Derived* downcast_mut = down_cast<Derived*>(base_ptr);
  ASSERT_EQ(downcast_mut, &derived);
  EXPECT_EQ(downcast_mut->base_id, 10);
  EXPECT_EQ(downcast_mut->derived_id, 20);
  downcast_mut->derived_id = 25;
  EXPECT_EQ(derived.derived_id, 25);

  const Base* const_base_ptr = &derived;
  const Derived* downcast_const = down_cast<const Derived*>(const_base_ptr);
  ASSERT_EQ(downcast_const, &derived);
  EXPECT_EQ(downcast_const->derived_id, 25);

  const Derived* added_const = down_cast<const Derived*>(base_ptr);
  EXPECT_EQ(added_const, &derived);

  // Same-type identity downcast satisfies std::is_base_of<T, T>.
  EXPECT_EQ(down_cast<Base*>(base_ptr), base_ptr);
  EXPECT_EQ(down_cast<const Base*>(const_base_ptr), const_base_ptr);

  GrandChild grand(1, 2, 3);
  Base* grand_as_base = &grand;
  Derived* grand_as_derived = down_cast<Derived*>(grand_as_base);
  GrandChild* grand_as_leaf = down_cast<GrandChild*>(grand_as_base);
  ASSERT_EQ(grand_as_derived, &grand);
  ASSERT_EQ(grand_as_leaf, &grand);
  EXPECT_EQ(grand_as_leaf->grand_id, 3);

  // Multiple inheritance requires pointer-offset adjustment via static_cast.
  MultiDerived multi(11, 22, 33);
  SecondBase* second_ptr = &multi;
  MultiDerived* from_second = down_cast<MultiDerived*>(second_ptr);
  ASSERT_EQ(from_second, &multi);
  EXPECT_EQ(from_second->base_id, 11);
  EXPECT_EQ(from_second->second_id, 22);
  EXPECT_EQ(from_second->multi_id, 33);
}

TEST(UtilTest, DownCastGeosGeometryHierarchy) {
  const GeometryFactory::Ptr factory = GeometryFactory::create();

  EXPECT_EQ(down_cast<Point*>(static_cast<Geometry*>(nullptr)), nullptr);

  std::unique_ptr<Geometry> pt_geom = factory->createPoint(CoordinateXY(5, 6));
  Point* pt = down_cast<Point*>(pt_geom.get());
  ASSERT_EQ(pt, pt_geom.get());
  EXPECT_DOUBLE_EQ(pt->getX(), 5.0);
  EXPECT_DOUBLE_EQ(pt->getY(), 6.0);

  const Geometry* const_pt_geom = pt_geom.get();
  const Point* const_pt = down_cast<const Point*>(const_pt_geom);
  ASSERT_EQ(const_pt, pt_geom.get());
  EXPECT_DOUBLE_EQ(const_pt->getX(), 5.0);

  std::unique_ptr<Geometry> line_geom =
      factory->createLineString(std::make_unique<CoordinateSequence>(
          std::initializer_list<CoordinateXY>{{0.0, 0.0}, {3.0, 4.0}}));
  const LineString* line = down_cast<const LineString*>(line_geom.get());
  ASSERT_EQ(line, line_geom.get());
  EXPECT_EQ(line->getNumPoints(), 2u);
  Curve* line_as_curve = down_cast<Curve*>(line_geom.get());
  EXPECT_EQ(down_cast<LineString*>(line_as_curve), line_geom.get());

  auto ring = factory->createLinearRing(
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {0.0, 0.0}, {2.0, 0.0}, {2.0, 2.0}, {0.0, 2.0}, {0.0, 0.0}}));
  std::unique_ptr<Geometry> poly_geom = factory->createPolygon(std::move(ring));
  const Polygon* poly = down_cast<const Polygon*>(poly_geom.get());
  ASSERT_EQ(poly, poly_geom.get());
  EXPECT_EQ(poly->getNumInteriorRing(), 0u);

  std::unique_ptr<Geometry> arc_geom = factory->createCircularString(
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}}));
  const CircularString* arc = down_cast<const CircularString*>(arc_geom.get());
  ASSERT_EQ(arc, arc_geom.get());
  EXPECT_TRUE(arc->isCurved());

  std::vector<std::unique_ptr<Geometry>> items;
  items.push_back(pt_geom->clone());
  std::unique_ptr<Geometry> gc_geom =
      factory->createGeometryCollection(std::move(items));
  const GeometryCollection* gc =
      down_cast<const GeometryCollection*>(gc_geom.get());
  ASSERT_EQ(gc, gc_geom.get());
  EXPECT_EQ(gc->getNumGeometries(), 1u);
}

TEST(UtilTest, EnsureNoCurvedComponentsReferenceAndPointerStubs) {
  const StubCurvedCheck non_curved{.curved = false};
  EXPECT_NO_THROW(ensureNoCurvedComponents(non_curved));
  EXPECT_NO_THROW(ensureNoCurvedComponents(&non_curved));

  const StubCurvedCheck curved{.curved = true};
  {
    bool caught = false;
    try {
      ensureNoCurvedComponents(curved);
    } catch (const UnsupportedOperationException& ex) {
      caught = true;
      EXPECT_STREQ(
          ex.what(),
          "UnsupportedOperationException: Curved geometry types are not "
          "supported.");
    }
    EXPECT_TRUE(caught);
  }

  {
    bool caught = false;
    try {
      ensureNoCurvedComponents(&curved);
    } catch (const UnsupportedOperationException& ex) {
      caught = true;
      EXPECT_STREQ(
          ex.what(),
          "UnsupportedOperationException: Curved geometry types are not "
          "supported.");
    }
    EXPECT_TRUE(caught);
  }

  StubCurvedCheck mutable_stub{.curved = false};
  EXPECT_NO_THROW(ensureNoCurvedComponents(mutable_stub));
  mutable_stub.curved = true;
  EXPECT_THROW(ensureNoCurvedComponents(mutable_stub),
               UnsupportedOperationException);
}

TEST(UtilTest, EnsureNoCurvedComponentsAllowsLinearGeosGeometries) {
  const GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<Point> point = factory->createPoint(CoordinateXY(1.0, 2.0));
  std::unique_ptr<Point> empty_point = factory->createPoint();
  std::unique_ptr<LineString> line =
      factory->createLineString(std::make_unique<CoordinateSequence>(
          std::initializer_list<CoordinateXY>{{0.0, 0.0}, {2.0, 2.0}}));
  std::unique_ptr<LinearRing> ring = factory->createLinearRing(
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {0.0, 0.0}, {4.0, 0.0}, {4.0, 4.0}, {0.0, 4.0}, {0.0, 0.0}}));
  std::unique_ptr<Polygon> poly = factory->createPolygon(ring->clone());
  std::unique_ptr<CurvePolygon> linear_curve_poly =
      factory->createCurvePolygon(ring->clone());

  std::vector<std::unique_ptr<SimpleCurve>> linear_sections;
  linear_sections.push_back(
      factory->createLineString(std::make_unique<CoordinateSequence>(
          std::initializer_list<CoordinateXY>{{0.0, 0.0}, {1.0, 0.0}})));
  linear_sections.push_back(
      factory->createLineString(std::make_unique<CoordinateSequence>(
          std::initializer_list<CoordinateXY>{{1.0, 0.0}, {1.0, 1.0}})));
  std::unique_ptr<CompoundCurve> linear_compound =
      factory->createCompoundCurve(std::move(linear_sections));

  std::vector<std::unique_ptr<Point>> multi_point_parts;
  multi_point_parts.push_back(point->clone());
  std::unique_ptr<MultiPoint> linear_multi_point =
      factory->createMultiPoint(std::move(multi_point_parts));

  std::vector<std::unique_ptr<LineString>> multi_line_parts;
  multi_line_parts.push_back(line->clone());
  std::unique_ptr<MultiLineString> linear_multi_line =
      factory->createMultiLineString(std::move(multi_line_parts));

  std::vector<std::unique_ptr<Polygon>> multi_poly_parts;
  multi_poly_parts.push_back(poly->clone());
  std::unique_ptr<MultiPolygon> linear_multi_poly =
      factory->createMultiPolygon(std::move(multi_poly_parts));

  std::vector<std::unique_ptr<Curve>> multi_curve_parts;
  multi_curve_parts.push_back(line->clone());
  std::unique_ptr<MultiCurve> linear_multi_curve =
      factory->createMultiCurve(std::move(multi_curve_parts));

  std::vector<std::unique_ptr<Surface>> multi_surface_parts;
  multi_surface_parts.push_back(poly->clone());
  std::unique_ptr<MultiSurface> linear_multi_surface =
      factory->createMultiSurface(std::move(multi_surface_parts));

  std::vector<std::unique_ptr<Geometry>> gc_parts;
  gc_parts.push_back(point->clone());
  gc_parts.push_back(line->clone());
  std::unique_ptr<GeometryCollection> linear_collection =
      factory->createGeometryCollection(std::move(gc_parts));
  std::unique_ptr<GeometryCollection> empty_collection =
      factory->createGeometryCollection();

  const std::vector<const Geometry*> linear_geoms = {
      point.get(),
      empty_point.get(),
      line.get(),
      ring.get(),
      poly.get(),
      linear_curve_poly.get(),
      linear_compound.get(),
      linear_multi_point.get(),
      linear_multi_line.get(),
      linear_multi_poly.get(),
      linear_multi_curve.get(),
      linear_multi_surface.get(),
      linear_collection.get(),
      empty_collection.get(),
  };

  for (const Geometry* geom : linear_geoms) {
    ASSERT_NE(geom, nullptr);
    SCOPED_TRACE(geom->getGeometryType());
    EXPECT_FALSE(geom->hasCurvedComponents());
    EXPECT_NO_THROW(ensureNoCurvedComponents(*geom));
    EXPECT_NO_THROW(ensureNoCurvedComponents(geom));
  }

  // Also instantiate with concrete const-qualified typed pointers and
  // non-const references. (Non-const pointers would match `const T&` with
  // `T = U*` instead of `const T*`.)
  const Point* const_point = point.get();
  const LineString* const_line = line.get();
  const LinearRing* const_ring = ring.get();
  const Polygon* const_poly = poly.get();
  const Curve* const_linear_curve = line.get();
  const SimpleCurve* const_linear_simple_curve = line.get();
  const Surface* const_linear_surface = poly.get();
  const CompoundCurve* const_linear_compound = linear_compound.get();
  const CurvePolygon* const_linear_curve_poly = linear_curve_poly.get();
  EXPECT_NO_THROW(ensureNoCurvedComponents(const_point));
  EXPECT_NO_THROW(ensureNoCurvedComponents(*point));
  EXPECT_NO_THROW(ensureNoCurvedComponents(const_line));
  EXPECT_NO_THROW(ensureNoCurvedComponents(const_ring));
  EXPECT_NO_THROW(ensureNoCurvedComponents(const_poly));
  EXPECT_NO_THROW(ensureNoCurvedComponents(const_linear_curve));
  EXPECT_NO_THROW(ensureNoCurvedComponents(const_linear_simple_curve));
  EXPECT_NO_THROW(ensureNoCurvedComponents(const_linear_surface));
  EXPECT_NO_THROW(ensureNoCurvedComponents(const_linear_compound));
  EXPECT_NO_THROW(ensureNoCurvedComponents(*linear_compound));
  EXPECT_NO_THROW(ensureNoCurvedComponents(const_linear_curve_poly));
  EXPECT_NO_THROW(ensureNoCurvedComponents(*linear_curve_poly));

  // GeometryCollection::hasCurvedComponents() is protected (and inherited as
  // protected by MultiPoint, MultiLineString, MultiPolygon, MultiCurve, and
  // MultiSurface), so collection types must be passed via const Geometry* /
  // const Geometry&.
  const Geometry* const_linear_multi_curve_geom = linear_multi_curve.get();
  const Geometry* const_linear_multi_surface_geom = linear_multi_surface.get();
  const Geometry* const_linear_collection_geom = linear_collection.get();
  EXPECT_NO_THROW(ensureNoCurvedComponents(const_linear_multi_curve_geom));
  EXPECT_NO_THROW(ensureNoCurvedComponents(*const_linear_multi_curve_geom));
  EXPECT_NO_THROW(ensureNoCurvedComponents(const_linear_multi_surface_geom));
  EXPECT_NO_THROW(ensureNoCurvedComponents(*const_linear_multi_surface_geom));
  EXPECT_NO_THROW(ensureNoCurvedComponents(const_linear_collection_geom));
  EXPECT_NO_THROW(ensureNoCurvedComponents(*const_linear_collection_geom));

  // Verify indirect GEOS caller Geometry::getInteriorPoint succeeds for linear.
  std::unique_ptr<Point> interior;
  EXPECT_NO_THROW(interior = line->getInteriorPoint());
  ASSERT_NE(interior, nullptr);
  EXPECT_FALSE(interior->isEmpty());
}

TEST(UtilTest, EnsureNoCurvedComponentsRejectsCurvedGeosGeometries) {
  const GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<CircularString> arc = factory->createCircularString(
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}}));
  std::unique_ptr<CircularString> empty_arc =
      factory->createCircularString(false, false);

  std::vector<std::unique_ptr<SimpleCurve>> mixed_sections;
  mixed_sections.push_back(
      factory->createLineString(std::make_unique<CoordinateSequence>(
          std::initializer_list<CoordinateXY>{{0.0, 0.0}, {1.0, 0.0}})));
  mixed_sections.push_back(factory->createCircularString(
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {1.0, 0.0}, {2.0, 1.0}, {3.0, 0.0}})));
  std::unique_ptr<CompoundCurve> curved_compound =
      factory->createCompoundCurve(std::move(mixed_sections));

  std::unique_ptr<Curve> closed_arc = factory->createCircularString(
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}, {1.0, -1.0}, {0.0, 0.0}}));
  std::unique_ptr<CurvePolygon> curve_poly =
      factory->createCurvePolygon(std::move(closed_arc));

  std::unique_ptr<LinearRing> outer_ring = factory->createLinearRing(
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}}));
  std::vector<std::unique_ptr<Curve>> curved_holes;
  curved_holes.push_back(factory->createCircularString(
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {2.0, 5.0}, {5.0, 8.0}, {8.0, 5.0}, {5.0, 2.0}, {2.0, 5.0}})));
  std::unique_ptr<CurvePolygon> linear_shell_curved_hole =
      factory->createCurvePolygon(std::move(outer_ring),
                                  std::move(curved_holes));

  std::vector<std::unique_ptr<Curve>> multi_curve_parts;
  multi_curve_parts.push_back(arc->clone());
  std::unique_ptr<MultiCurve> curved_multi_curve =
      factory->createMultiCurve(std::move(multi_curve_parts));

  std::vector<std::unique_ptr<Geometry>> multi_surface_parts;
  multi_surface_parts.push_back(curve_poly->clone());
  std::unique_ptr<MultiSurface> curved_multi_surface =
      factory->createMultiSurface(std::move(multi_surface_parts));

  std::vector<std::unique_ptr<Geometry>> gc_parts;
  gc_parts.push_back(factory->createPoint(CoordinateXY(0.0, 0.0)));
  gc_parts.push_back(arc->clone());
  std::unique_ptr<GeometryCollection> curved_collection =
      factory->createGeometryCollection(std::move(gc_parts));

  const std::vector<const Geometry*> curved_geoms = {
      arc.get(),
      empty_arc.get(),
      curved_compound.get(),
      curve_poly.get(),
      linear_shell_curved_hole.get(),
      curved_multi_curve.get(),
      curved_multi_surface.get(),
      curved_collection.get(),
  };

  constexpr std::string_view kExpectedWhat =
      "UnsupportedOperationException: Curved geometry types are not supported.";

  for (const Geometry* geom : curved_geoms) {
    ASSERT_NE(geom, nullptr);
    SCOPED_TRACE(geom->getGeometryType());
    EXPECT_TRUE(geom->hasCurvedComponents());

    bool caught_ref = false;
    try {
      ensureNoCurvedComponents(*geom);
    } catch (const UnsupportedOperationException& ex) {
      caught_ref = true;
      EXPECT_EQ(std::string_view(ex.what()), kExpectedWhat);
    }
    EXPECT_TRUE(caught_ref);

    bool caught_ptr = false;
    try {
      ensureNoCurvedComponents(geom);
    } catch (const UnsupportedOperationException& ex) {
      caught_ptr = true;
      EXPECT_EQ(std::string_view(ex.what()), kExpectedWhat);
    }
    EXPECT_TRUE(caught_ptr);
  }

  // Also instantiate with concrete const-qualified typed pointers and
  // non-const references where `hasCurvedComponents()` is public.
  const CircularString* const_arc = arc.get();
  const CompoundCurve* const_curved_compound = curved_compound.get();
  const CurvePolygon* const_curve_poly = curve_poly.get();
  const CurvePolygon* const_linear_shell_curved_hole =
      linear_shell_curved_hole.get();
  const Curve* const_curved_curve = arc.get();
  const SimpleCurve* const_curved_simple_curve = arc.get();
  const Surface* const_curved_surface = curve_poly.get();
  EXPECT_THROW(ensureNoCurvedComponents(const_arc),
               UnsupportedOperationException);
  EXPECT_THROW(ensureNoCurvedComponents(*arc), UnsupportedOperationException);
  EXPECT_THROW(ensureNoCurvedComponents(const_curved_compound),
               UnsupportedOperationException);
  EXPECT_THROW(ensureNoCurvedComponents(*curved_compound),
               UnsupportedOperationException);
  EXPECT_THROW(ensureNoCurvedComponents(const_curve_poly),
               UnsupportedOperationException);
  EXPECT_THROW(ensureNoCurvedComponents(*curve_poly),
               UnsupportedOperationException);
  EXPECT_THROW(ensureNoCurvedComponents(const_linear_shell_curved_hole),
               UnsupportedOperationException);
  EXPECT_THROW(ensureNoCurvedComponents(const_curved_curve),
               UnsupportedOperationException);
  EXPECT_THROW(ensureNoCurvedComponents(const_curved_simple_curve),
               UnsupportedOperationException);
  EXPECT_THROW(ensureNoCurvedComponents(const_curved_surface),
               UnsupportedOperationException);

  // Collections inherit protected GeometryCollection::hasCurvedComponents(), so
  // pass them via const Geometry* / const Geometry&.
  const Geometry* const_curved_multi_curve_geom = curved_multi_curve.get();
  const Geometry* const_curved_multi_surface_geom = curved_multi_surface.get();
  const Geometry* const_curved_collection_geom = curved_collection.get();
  EXPECT_THROW(ensureNoCurvedComponents(const_curved_multi_curve_geom),
               UnsupportedOperationException);
  EXPECT_THROW(ensureNoCurvedComponents(*const_curved_multi_curve_geom),
               UnsupportedOperationException);
  EXPECT_THROW(ensureNoCurvedComponents(const_curved_multi_surface_geom),
               UnsupportedOperationException);
  EXPECT_THROW(ensureNoCurvedComponents(*const_curved_multi_surface_geom),
               UnsupportedOperationException);
  EXPECT_THROW(ensureNoCurvedComponents(const_curved_collection_geom),
               UnsupportedOperationException);
  EXPECT_THROW(ensureNoCurvedComponents(*const_curved_collection_geom),
               UnsupportedOperationException);

  // Verify indirect GEOS caller Geometry::getInteriorPoint rejects curved.
  std::unique_ptr<Point> interior;
  EXPECT_THROW(interior = arc->getInteriorPoint(),
               UnsupportedOperationException);
  EXPECT_EQ(interior, nullptr);
}

}  // namespace
}  // namespace geos_google
