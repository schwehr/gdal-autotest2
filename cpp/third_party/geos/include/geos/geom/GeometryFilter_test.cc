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

#include "third_party/geos/include/geos/geom/GeometryFilter.h"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <type_traits>
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
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryComponentFilter.h"
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
#include "third_party/geos/include/geos/geom/util/GeometryExtracter.h"
#include "third_party/geos/include/geos/geom/util/PolygonExtracter.h"

namespace geos_google {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

using geos::geom::CircularString;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Curve;
using geos::geom::Geometry;
using geos::geom::GeometryComponentFilter;
using geos::geom::GeometryFactory;
using geos::geom::GeometryFilter;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::SimpleCurve;
using geos::geom::Surface;

static_assert(!std::is_abstract_v<GeometryFilter>);
static_assert(std::has_virtual_destructor_v<GeometryFilter>);

std::unique_ptr<CoordinateSequence> MakeXYSeq(
    std::initializer_list<CoordinateXY> coords) {
  return std::make_unique<CoordinateSequence>(coords);
}

std::unique_ptr<LinearRing> MakeRectRing(const GeometryFactory& factory,
                                         double min_x, double min_y,
                                         double max_x, double max_y) {
  return factory.createLinearRing(MakeXYSeq({
      {min_x, min_y},
      {max_x, min_y},
      {max_x, max_y},
      {min_x, max_y},
      {min_x, min_y},
  }));
}

std::unique_ptr<Polygon> MakePolygonWithHole(const GeometryFactory& factory) {
  auto shell = MakeRectRing(factory, 0.0, 0.0, 10.0, 10.0);
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(MakeRectRing(factory, 2.0, 2.0, 4.0, 4.0));
  return factory.createPolygon(std::move(shell), std::move(holes));
}

std::unique_ptr<CircularString> MakeCircleCurve(const GeometryFactory& factory,
                                                double cx, double cy,
                                                double radius) {
  return factory.createCircularString(MakeXYSeq({
      {cx - radius, cy},
      {cx, cy + radius},
      {cx + radius, cy},
      {cx, cy - radius},
      {cx - radius, cy},
  }));
}

class ReadOnlyCollector : public GeometryFilter {
 public:
  void filter_ro(const Geometry* geom) override { visited_.push_back(geom); }

  const std::vector<const Geometry*>& visited() const { return visited_; }

 private:
  std::vector<const Geometry*> visited_;
};

class SridMutatorFilter : public GeometryFilter {
 public:
  explicit SridMutatorFilter(int start_srid) : next_srid_(start_srid) {}

  void filter_rw(Geometry* geom) override {
    visited_.push_back(geom);
    geom->setSRID(next_srid_++);
  }

  const std::vector<Geometry*>& visited() const { return visited_; }

 private:
  int next_srid_;
  std::vector<Geometry*> visited_;
};

class DualModeFilter : public GeometryFilter {
 public:
  void filter_ro(const Geometry* geom) override { ro_visited_.push_back(geom); }

  void filter_rw(Geometry* geom) override { rw_visited_.push_back(geom); }

  const std::vector<const Geometry*>& ro_visited() const { return ro_visited_; }
  const std::vector<Geometry*>& rw_visited() const { return rw_visited_; }

 private:
  std::vector<const Geometry*> ro_visited_;
  std::vector<Geometry*> rw_visited_;
};

class DestructorTrackingFilter : public GeometryFilter {
 public:
  explicit DestructorTrackingFilter(bool* destroyed) : destroyed_(destroyed) {}
  ~DestructorTrackingFilter() override { *destroyed_ = true; }

  void filter_ro(const Geometry* geom) override {
    if (geom != nullptr) {
      ++count_;
    }
  }

  std::size_t count() const { return count_; }

 private:
  bool* destroyed_;
  std::size_t count_ = 0;
};

class ComponentCollector : public GeometryComponentFilter {
 public:
  void filter_ro(const Geometry* geom) override { visited_.push_back(geom); }

  const std::vector<const Geometry*>& visited() const { return visited_; }

 private:
  std::vector<const Geometry*> visited_;
};

class GeometryFilterTest : public ::testing::Test {
 protected:
  GeometryFactory::Ptr factory_ = GeometryFactory::create();
};

TEST_F(GeometryFilterTest, BaseConstructionAndVirtualDestructor) {
  {
    auto direct_base = std::make_unique<GeometryFilter>();
    EXPECT_NE(direct_base.get(), nullptr);
  }

  bool derived_destroyed = false;
  {
    auto pt = factory_->createPoint(CoordinateXY(3.0, 4.0));
    std::unique_ptr<GeometryFilter> filter =
        std::make_unique<DestructorTrackingFilter>(&derived_destroyed);
    pt->apply_ro(filter.get());
    EXPECT_EQ(static_cast<DestructorTrackingFilter*>(filter.get())->count(),
              1u);
    EXPECT_FALSE(derived_destroyed);
  }
  EXPECT_TRUE(derived_destroyed);
}

TEST_F(GeometryFilterTest, ReadOnlyFilterVisitsAtomicAndEmptyGeometries) {
  auto pt = factory_->createPoint(CoordinateXY(1.0, 2.0));
  auto empty_pt = factory_->createPoint();
  auto ls = factory_->createLineString(MakeXYSeq({{0.0, 0.0}, {1.0, 1.0}}));
  auto empty_ls = factory_->createLineString();
  auto ring = MakeRectRing(*factory_, 0.0, 0.0, 2.0, 2.0);
  auto empty_ring = factory_->createLinearRing();
  auto poly = MakePolygonWithHole(*factory_);
  auto empty_poly = factory_->createPolygon();
  auto circle = MakeCircleCurve(*factory_, 0.0, 0.0, 5.0);

  std::vector<std::unique_ptr<SimpleCurve>> sections;
  sections.push_back(factory_->createCircularString(
      MakeXYSeq({{0.0, 0.0}, {2.0, 2.0}, {4.0, 0.0}})));
  sections.push_back(
      factory_->createLineString(MakeXYSeq({{4.0, 0.0}, {0.0, 0.0}})));
  auto compound = factory_->createCompoundCurve(std::move(sections));

  std::vector<std::unique_ptr<Curve>> curve_holes;
  curve_holes.push_back(MakeCircleCurve(*factory_, 0.0, 0.0, 1.0));
  auto curve_poly = factory_->createCurvePolygon(
      MakeCircleCurve(*factory_, 0.0, 0.0, 5.0), std::move(curve_holes));

  const std::vector<const Geometry*> geoms = {
      pt.get(),     empty_pt.get(),   ls.get(),         empty_ls.get(),
      ring.get(),   empty_ring.get(), poly.get(),       empty_poly.get(),
      circle.get(), compound.get(),   curve_poly.get(),
  };

  for (const Geometry* geom : geoms) {
    ReadOnlyCollector collector;
    geom->apply_ro(&collector);
    EXPECT_THAT(collector.visited(), ElementsAre(geom))
        << "Geometry type: " << geom->getGeometryType();
  }
}

TEST_F(GeometryFilterTest, DoesNotDescendIntoPolygonOrCompoundCurveComponents) {
  auto poly = MakePolygonWithHole(*factory_);

  ReadOnlyCollector geom_filter;
  poly->apply_ro(&geom_filter);
  EXPECT_THAT(geom_filter.visited(), ElementsAre(poly.get()));

  ComponentCollector comp_filter;
  poly->apply_ro(&comp_filter);
  EXPECT_THAT(comp_filter.visited(),
              ElementsAre(poly.get(), poly->getExteriorRing(),
                          poly->getInteriorRingN(0)));
}

TEST_F(GeometryFilterTest, ReadOnlyFilterVisitsEmptyCollectionsOnce) {
  auto empty_gc = factory_->createGeometryCollection();
  auto empty_mp = factory_->createMultiPoint();
  auto empty_mls = factory_->createMultiLineString();
  auto empty_mpoly = factory_->createMultiPolygon();
  auto empty_mc = factory_->createMultiCurve();
  auto empty_ms = factory_->createMultiSurface();

  const std::vector<const Geometry*> empties = {
      empty_gc.get(),    empty_mp.get(), empty_mls.get(),
      empty_mpoly.get(), empty_mc.get(), empty_ms.get(),
  };

  for (const Geometry* geom : empties) {
    ReadOnlyCollector collector;
    geom->apply_ro(&collector);
    EXPECT_THAT(collector.visited(), ElementsAre(geom));
  }
}

TEST_F(GeometryFilterTest, ReadOnlyFilterTraversesMultiGeometriesInPreOrder) {
  std::vector<std::unique_ptr<Point>> pts;
  pts.push_back(factory_->createPoint(CoordinateXY(0.0, 0.0)));
  pts.push_back(factory_->createPoint(CoordinateXY(1.0, 1.0)));
  const Point* p0 = pts[0].get();
  const Point* p1 = pts[1].get();
  auto mp = factory_->createMultiPoint(std::move(pts));

  ReadOnlyCollector mp_collector;
  mp->apply_ro(&mp_collector);
  EXPECT_THAT(mp_collector.visited(), ElementsAre(mp.get(), p0, p1));

  std::vector<std::unique_ptr<LineString>> lines;
  lines.push_back(
      factory_->createLineString(MakeXYSeq({{0.0, 0.0}, {1.0, 0.0}})));
  lines.push_back(
      factory_->createLineString(MakeXYSeq({{1.0, 0.0}, {2.0, 0.0}})));
  const LineString* l0 = lines[0].get();
  const LineString* l1 = lines[1].get();
  auto mls = factory_->createMultiLineString(std::move(lines));

  ReadOnlyCollector mls_collector;
  mls->apply_ro(&mls_collector);
  EXPECT_THAT(mls_collector.visited(), ElementsAre(mls.get(), l0, l1));

  std::vector<std::unique_ptr<Polygon>> polys;
  polys.push_back(MakePolygonWithHole(*factory_));
  polys.push_back(factory_->createPolygon());
  const Polygon* poly0 = polys[0].get();
  const Polygon* poly1 = polys[1].get();
  auto mpoly = factory_->createMultiPolygon(std::move(polys));

  ReadOnlyCollector mpoly_collector;
  mpoly->apply_ro(&mpoly_collector);
  EXPECT_THAT(mpoly_collector.visited(),
              ElementsAre(mpoly.get(), poly0, poly1));

  std::vector<std::unique_ptr<Curve>> curves;
  curves.push_back(
      factory_->createLineString(MakeXYSeq({{0.0, 0.0}, {3.0, 0.0}})));
  curves.push_back(MakeCircleCurve(*factory_, 5.0, 5.0, 2.0));
  const Curve* c0 = curves[0].get();
  const Curve* c1 = curves[1].get();
  auto mc = factory_->createMultiCurve(std::move(curves));

  ReadOnlyCollector mc_collector;
  mc->apply_ro(&mc_collector);
  EXPECT_THAT(mc_collector.visited(), ElementsAre(mc.get(), c0, c1));

  std::vector<std::unique_ptr<Surface>> surfaces;
  surfaces.push_back(MakePolygonWithHole(*factory_));
  surfaces.push_back(
      factory_->createCurvePolygon(MakeCircleCurve(*factory_, 0.0, 0.0, 3.0)));
  const Surface* s0 = surfaces[0].get();
  const Surface* s1 = surfaces[1].get();
  auto ms = factory_->createMultiSurface(std::move(surfaces));

  ReadOnlyCollector ms_collector;
  ms->apply_ro(&ms_collector);
  EXPECT_THAT(ms_collector.visited(), ElementsAre(ms.get(), s0, s1));
}

TEST_F(GeometryFilterTest, ReadOnlyFilterTraversesNestedGeometryCollections) {
  auto p_a = factory_->createPoint(CoordinateXY(1.0, 1.0));
  auto p_b = factory_->createPoint(CoordinateXY(2.0, 2.0));
  std::vector<std::unique_ptr<Point>> mp_elems;
  mp_elems.push_back(std::move(p_a));
  mp_elems.push_back(std::move(p_b));
  auto mp = factory_->createMultiPoint(std::move(mp_elems));

  auto inner_ls =
      factory_->createLineString(MakeXYSeq({{0.0, 0.0}, {5.0, 5.0}}));
  auto inner_empty_poly = factory_->createPolygon();
  std::vector<std::unique_ptr<Geometry>> inner_elems;
  inner_elems.push_back(std::move(inner_ls));
  inner_elems.push_back(std::move(inner_empty_poly));
  auto inner_gc = factory_->createGeometryCollection(std::move(inner_elems));

  auto top_pt = factory_->createPoint(CoordinateXY(9.0, 9.0));
  auto top_poly = MakePolygonWithHole(*factory_);

  const Geometry* expected_order[] = {
      nullptr,  // outer_gc filled after construction
      top_pt.get(),
      top_poly.get(),
      mp.get(),
      mp->getGeometryN(0),
      mp->getGeometryN(1),
      inner_gc.get(),
      inner_gc->getGeometryN(0),
      inner_gc->getGeometryN(1),
  };

  std::vector<std::unique_ptr<Geometry>> outer_elems;
  outer_elems.push_back(std::move(top_pt));
  outer_elems.push_back(std::move(top_poly));
  outer_elems.push_back(std::move(mp));
  outer_elems.push_back(std::move(inner_gc));
  auto outer_gc = factory_->createGeometryCollection(std::move(outer_elems));
  expected_order[0] = outer_gc.get();

  ReadOnlyCollector collector;
  outer_gc->apply_ro(&collector);

  ASSERT_EQ(collector.visited().size(), 9u);
  for (std::size_t i = 0; i < collector.visited().size(); ++i) {
    EXPECT_EQ(collector.visited()[i], expected_order[i]) << "at index " << i;
  }
}

TEST_F(GeometryFilterTest,
       ReadWriteFilterMutatesAtomicAndCollectionGeometries) {
  auto pt = factory_->createPoint(CoordinateXY(4.0, 5.0));
  int user_tag = 77;

  class UserDataMutator : public GeometryFilter {
   public:
    explicit UserDataMutator(void* tag) : tag_(tag) {}
    void filter_rw(Geometry* geom) override { geom->setUserData(tag_); }

   private:
    void* tag_;
  };

  UserDataMutator tagger(&user_tag);
  pt->apply_rw(&tagger);
  EXPECT_EQ(pt->getUserData(), &user_tag);

  auto nested_ls =
      factory_->createLineString(MakeXYSeq({{0.0, 0.0}, {1.0, 2.0}}));
  Geometry* c2_0 = nested_ls.get();
  std::vector<std::unique_ptr<Geometry>> inner_geoms;
  inner_geoms.push_back(std::move(nested_ls));
  auto inner_gc = factory_->createGeometryCollection(std::move(inner_geoms));

  auto child_pt = factory_->createPoint(CoordinateXY(1.0, 1.0));
  auto child_poly = MakePolygonWithHole(*factory_);
  Geometry* c0 = child_pt.get();
  Geometry* c1 = child_poly.get();
  Geometry* c2 = inner_gc.get();

  std::vector<std::unique_ptr<Geometry>> outer_geoms;
  outer_geoms.push_back(std::move(child_pt));
  outer_geoms.push_back(std::move(child_poly));
  outer_geoms.push_back(std::move(inner_gc));
  auto outer_gc = factory_->createGeometryCollection(std::move(outer_geoms));

  SridMutatorFilter mutator(100);
  outer_gc->apply_rw(&mutator);

  // GeometryCollection::setSRID propagates to children before recursive
  // apply_rw visits each child, so each child ends with its own unique SRID.
  EXPECT_THAT(mutator.visited(), ElementsAre(outer_gc.get(), c0, c1, c2, c2_0));
  EXPECT_EQ(outer_gc->getSRID(), 100);
  EXPECT_EQ(c0->getSRID(), 101);
  EXPECT_EQ(c1->getSRID(), 102);
  EXPECT_EQ(c2->getSRID(), 103);
  EXPECT_EQ(c2_0->getSRID(), 104);
}

TEST_F(GeometryFilterTest, DualModeFilterDispatchesRoAndRwSeparately) {
  auto pt = factory_->createPoint(CoordinateXY(2.0, 3.0));
  auto child_poly = MakePolygonWithHole(*factory_);
  Geometry* child_ptr = child_poly.get();
  std::vector<std::unique_ptr<Geometry>> elems;
  elems.push_back(std::move(child_poly));
  auto gc = factory_->createGeometryCollection(std::move(elems));

  DualModeFilter dual;
  pt->apply_ro(&dual);
  gc->apply_rw(&dual);

  EXPECT_THAT(dual.ro_visited(), ElementsAre(pt.get()));
  EXPECT_THAT(dual.rw_visited(), ElementsAre(gc.get(), child_ptr));
}

TEST_F(GeometryFilterTest, BuiltInGeosFiltersExerciseGeometryFilter) {
  std::vector<std::unique_ptr<Polygon>> mp_polys;
  mp_polys.push_back(MakePolygonWithHole(*factory_));
  mp_polys.push_back(factory_->createPolygon());
  auto mp = factory_->createMultiPolygon(std::move(mp_polys));

  std::vector<std::unique_ptr<Geometry>> items;
  items.push_back(factory_->createPoint(CoordinateXY(1.0, 2.0)));
  items.push_back(MakePolygonWithHole(*factory_));
  items.push_back(std::move(mp));
  auto gc = factory_->createGeometryCollection(std::move(items));

  const auto* poly_top = dynamic_cast<const Polygon*>(gc->getGeometryN(1));
  const auto* poly_m0 =
      dynamic_cast<const Polygon*>(gc->getGeometryN(2)->getGeometryN(0));
  const auto* poly_m1 =
      dynamic_cast<const Polygon*>(gc->getGeometryN(2)->getGeometryN(1));
  ASSERT_NE(poly_top, nullptr);
  ASSERT_NE(poly_m0, nullptr);
  ASSERT_NE(poly_m1, nullptr);

  std::vector<const Polygon*> extracted_ro;
  geos::geom::util::PolygonExtracter::getPolygons(*gc, extracted_ro);
  EXPECT_THAT(extracted_ro, ElementsAre(poly_top, poly_m0, poly_m1));

  std::vector<const Polygon*> extracted_rw;
  geos::geom::util::PolygonExtracter extracter(extracted_rw);
  gc->apply_rw(&extracter);
  EXPECT_EQ(extracted_rw, extracted_ro);

  const auto* pt_top = dynamic_cast<const Point*>(gc->getGeometryN(0));
  ASSERT_NE(pt_top, nullptr);
  std::vector<const Point*> extracted_pts;
  geos::geom::util::GeometryExtracter::extract<Point>(*gc, extracted_pts);
  EXPECT_THAT(extracted_pts, ElementsAre(pt_top));

  ReadOnlyCollector empty_check;
  EXPECT_THAT(empty_check.visited(), IsEmpty());
}

TEST_F(GeometryFilterTest, BaseUnimplementedMethodsTriggerAssertInDebug) {
  GeometryFilter base_filter;
  auto pt = factory_->createPoint(CoordinateXY(1.0, 2.0));
  ReadOnlyCollector ro_filter;
  SridMutatorFilter rw_filter(10);

  // Base GeometryFilter::filter_ro and filter_rw unconditionally execute
  // assert(0) so read-only and write-only subclasses need not stub the other.
  EXPECT_DEBUG_DEATH(base_filter.filter_ro(pt.get()), "");
  EXPECT_DEBUG_DEATH(base_filter.filter_rw(pt.get()), "");
  EXPECT_DEBUG_DEATH(pt->apply_ro(&base_filter), "");
  EXPECT_DEBUG_DEATH(pt->apply_rw(&base_filter), "");
  EXPECT_DEBUG_DEATH(pt->apply_rw(&ro_filter), "");
  EXPECT_DEBUG_DEATH(pt->apply_ro(&rw_filter), "");
}

}  // namespace
}  // namespace geos_google
