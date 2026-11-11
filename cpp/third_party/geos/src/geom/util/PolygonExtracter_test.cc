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

// Unit tests for third_party/geos/src/geom/util/PolygonExtracter.cpp and
// third_party/geos/include/geos/geom/util/PolygonExtracter.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/util/PolygonExtracter.cpp

#include "third_party/geos/include/geos/geom/util/PolygonExtracter.h"

#include <array>
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
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/GeometryFilter.h"
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

using geos::geom::CircularString;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Curve;
using geos::geom::CurvePolygon;
using geos::geom::Dimension;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GeometryFilter;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::SimpleCurve;
using geos::geom::Surface;
using geos::geom::util::PolygonExtracter;
using ::testing::ElementsAre;
using ::testing::IsEmpty;

static_assert(std::is_base_of_v<GeometryFilter, PolygonExtracter>);
static_assert(!std::is_copy_constructible_v<PolygonExtracter>);
static_assert(!std::is_copy_assignable_v<PolygonExtracter>);

std::unique_ptr<LineString> MakeLineString(
    const GeometryFactory& factory,
    std::initializer_list<CoordinateXY> coords) {
  auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
  for (const auto& coord : coords) {
    seq->add(coord.x, coord.y);
  }
  return factory.createLineString(std::move(seq));
}

std::unique_ptr<CircularString> MakeCircularString(
    const GeometryFactory& factory,
    std::initializer_list<CoordinateXY> coords) {
  auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
  for (const auto& coord : coords) {
    seq->add(coord.x, coord.y);
  }
  return factory.createCircularString(std::move(seq));
}

std::unique_ptr<LinearRing> MakeRectRing(const GeometryFactory& factory,
                                         double min_x, double min_y,
                                         double max_x, double max_y) {
  auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
  seq->add(min_x, min_y);
  seq->add(max_x, min_y);
  seq->add(max_x, max_y);
  seq->add(min_x, max_y);
  seq->add(min_x, min_y);
  return factory.createLinearRing(std::move(seq));
}

std::unique_ptr<Polygon> MakeRectPoly(const GeometryFactory& factory,
                                      double min_x, double min_y, double max_x,
                                      double max_y) {
  return factory.createPolygon(
      MakeRectRing(factory, min_x, min_y, max_x, max_y));
}

std::unique_ptr<CurvePolygon> MakeCircularCurvePolygon(
    const GeometryFactory& factory, double center_x, double center_y,
    double radius) {
  auto shell = MakeCircularString(factory, {{center_x - radius, center_y},
                                            {center_x, center_y + radius},
                                            {center_x + radius, center_y},
                                            {center_x, center_y - radius},
                                            {center_x - radius, center_y}});
  return factory.createCurvePolygon(std::move(shell));
}

TEST(PolygonExtracterTest,
     GetPolygonsExtractsSinglePolygonWithHolesAndEmptyPolygon) {
  auto factory = GeometryFactory::create();

  auto simple_poly = MakeRectPoly(*factory, 0.0, 0.0, 4.0, 3.0);
  std::vector<const Polygon*> extracted_simple;
  PolygonExtracter::getPolygons(*simple_poly, extracted_simple);
  EXPECT_THAT(extracted_simple, ElementsAre(simple_poly.get()));
  EXPECT_DOUBLE_EQ(extracted_simple[0]->getArea(), 12.0);

  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(MakeRectRing(*factory, 1.0, 1.0, 2.0, 2.0));
  holes.push_back(MakeRectRing(*factory, 3.0, 1.0, 4.0, 2.0));
  auto poly_with_holes = factory->createPolygon(
      MakeRectRing(*factory, 0.0, 0.0, 5.0, 5.0), std::move(holes));
  std::vector<const Polygon*> extracted_holes;
  PolygonExtracter::getPolygons(*poly_with_holes, extracted_holes);
  EXPECT_THAT(extracted_holes, ElementsAre(poly_with_holes.get()));
  EXPECT_EQ(extracted_holes[0]->getNumInteriorRing(), 2u);

  // Empty Polygon still reports Dimension::A, so hasDimension(A) is true and it
  // is extracted as an empty Polygon component.
  auto empty_poly = factory->createPolygon();
  ASSERT_TRUE(empty_poly->hasDimension(Dimension::A));
  std::vector<const Polygon*> extracted_empty;
  PolygonExtracter::getPolygons(*empty_poly, extracted_empty);
  EXPECT_THAT(extracted_empty, ElementsAre(empty_poly.get()));
  EXPECT_TRUE(extracted_empty[0]->isEmpty());
}

TEST(PolygonExtracterTest, GetPolygonsReturnsEarlyForNonArealGeometries) {
  auto factory = GeometryFactory::create();

  auto pt = factory->createPoint(CoordinateXY{1.0, 2.0});
  auto empty_pt = factory->createPoint();

  std::vector<std::unique_ptr<Point>> pts;
  pts.push_back(factory->createPoint(CoordinateXY{3.0, 4.0}));
  auto multi_pt = factory->createMultiPoint(std::move(pts));
  auto empty_mp = factory->createMultiPoint();

  auto line = MakeLineString(*factory, {{0.0, 0.0}, {1.0, 1.0}});
  auto empty_line = factory->createLineString();
  auto ring = MakeRectRing(*factory, 0.0, 0.0, 2.0, 2.0);
  auto circular =
      MakeCircularString(*factory, {{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}});

  std::vector<std::unique_ptr<SimpleCurve>> sections;
  sections.push_back(MakeLineString(*factory, {{0.0, 0.0}, {1.0, 0.0}}));
  sections.push_back(MakeLineString(*factory, {{1.0, 0.0}, {1.0, 1.0}}));
  auto compound = factory->createCompoundCurve(std::move(sections));

  std::vector<std::unique_ptr<LineString>> lines;
  lines.push_back(MakeLineString(*factory, {{0.0, 0.0}, {2.0, 0.0}}));
  auto multi_line = factory->createMultiLineString(std::move(lines));

  std::vector<std::unique_ptr<Curve>> curves;
  curves.push_back(MakeLineString(*factory, {{0.0, 0.0}, {3.0, 0.0}}));
  auto multi_curve = factory->createMultiCurve(std::move(curves));

  auto empty_gc = factory->createGeometryCollection();

  std::vector<std::unique_ptr<Geometry>> non_areal_children;
  non_areal_children.push_back(factory->createPoint(CoordinateXY{5.0, 6.0}));
  non_areal_children.push_back(
      MakeLineString(*factory, {{0.0, 0.0}, {4.0, 4.0}}));
  non_areal_children.push_back(factory->createGeometryCollection());
  auto non_areal_gc =
      factory->createGeometryCollection(std::move(non_areal_children));

  const std::array<const Geometry*, 13> non_areal_geoms = {
      pt.get(),           empty_pt.get(),   multi_pt.get(),    empty_mp.get(),
      line.get(),         empty_line.get(), ring.get(),        circular.get(),
      compound.get(),     multi_line.get(), multi_curve.get(), empty_gc.get(),
      non_areal_gc.get(),
  };

  auto sentinel = MakeRectPoly(*factory, 0.0, 0.0, 1.0, 1.0);
  for (const Geometry* geom : non_areal_geoms) {
    ASSERT_FALSE(geom->hasDimension(Dimension::A));

    std::vector<const Polygon*> empty_ret;
    PolygonExtracter::getPolygons(*geom, empty_ret);
    EXPECT_THAT(empty_ret, IsEmpty());

    std::vector<const Polygon*> preserved_ret = {sentinel.get()};
    PolygonExtracter::getPolygons(*geom, preserved_ret);
    EXPECT_THAT(preserved_ret, ElementsAre(sentinel.get()));
  }
}

TEST(PolygonExtracterTest, GetPolygonsHandlesEmptyAndNonEmptyMultiPolygon) {
  auto factory = GeometryFactory::create();

  // MultiPolygon::hasDimension(Dimension::A) is true even when empty, so
  // apply_ro visits the MultiPolygon container itself (not a Polygon).
  auto empty_mp = factory->createMultiPolygon();
  ASSERT_TRUE(empty_mp->hasDimension(Dimension::A));
  std::vector<const Polygon*> extracted_empty_mp;
  PolygonExtracter::getPolygons(*empty_mp, extracted_empty_mp);
  EXPECT_THAT(extracted_empty_mp, IsEmpty());

  std::vector<std::unique_ptr<Polygon>> polys;
  polys.push_back(MakeRectPoly(*factory, 0.0, 0.0, 2.0, 2.0));
  polys.push_back(factory->createPolygon());
  polys.push_back(MakeRectPoly(*factory, 10.0, 10.0, 13.0, 13.0));
  const Polygon* poly0 = polys[0].get();
  const Polygon* poly1 = polys[1].get();
  const Polygon* poly2 = polys[2].get();
  auto multi_poly = factory->createMultiPolygon(std::move(polys));

  std::vector<const Polygon*> extracted_mp;
  PolygonExtracter::getPolygons(*multi_poly, extracted_mp);
  EXPECT_THAT(extracted_mp, ElementsAre(poly0, poly1, poly2));

  // A GeometryCollection holding an empty MultiPolygon reports hasDimension(A).
  std::vector<std::unique_ptr<Geometry>> geoms;
  geoms.push_back(factory->createMultiPolygon());
  geoms.push_back(factory->createPoint(CoordinateXY{1.0, 1.0}));
  auto gc_with_empty_mp = factory->createGeometryCollection(std::move(geoms));
  ASSERT_TRUE(gc_with_empty_mp->hasDimension(Dimension::A));

  std::vector<const Polygon*> extracted_gc_empty_mp;
  PolygonExtracter::getPolygons(*gc_with_empty_mp, extracted_gc_empty_mp);
  EXPECT_THAT(extracted_gc_empty_mp, IsEmpty());
}

TEST(
    PolygonExtracterTest,
    GetPolygonsSkipsCurvePolygonAndExtractsOnlyLinearPolygonsFromMultiSurface) {
  auto factory = GeometryFactory::create();

  auto curve_poly = MakeCircularCurvePolygon(*factory, 0.0, 0.0, 2.0);
  ASSERT_TRUE(curve_poly->hasDimension(Dimension::A));

  std::vector<const Polygon*> extracted_cp;
  PolygonExtracter::getPolygons(*curve_poly, extracted_cp);
  EXPECT_THAT(extracted_cp, IsEmpty());

  auto empty_curve_poly = factory->createCurvePolygon(false, false);
  ASSERT_TRUE(empty_curve_poly->hasDimension(Dimension::A));
  PolygonExtracter::getPolygons(*empty_curve_poly, extracted_cp);
  EXPECT_THAT(extracted_cp, IsEmpty());

  std::vector<std::unique_ptr<Surface>> surfaces;
  auto linear_poly = MakeRectPoly(*factory, 10.0, 10.0, 15.0, 15.0);
  const Polygon* linear_poly_ptr = linear_poly.get();
  surfaces.push_back(MakeCircularCurvePolygon(*factory, 0.0, 0.0, 1.0));
  surfaces.push_back(std::move(linear_poly));
  auto multi_surface = factory->createMultiSurface(std::move(surfaces));

  std::vector<const Polygon*> extracted_ms;
  PolygonExtracter::getPolygons(*multi_surface, extracted_ms);
  EXPECT_THAT(extracted_ms, ElementsAre(linear_poly_ptr));
}

TEST(PolygonExtracterTest,
     GetPolygonsExtractsNestedCollectionAndPreservesExistingEntries) {
  auto factory = GeometryFactory::create();

  auto preexisting = MakeRectPoly(*factory, -5.0, -5.0, -1.0, -1.0);

  auto deep_poly = MakeRectPoly(*factory, 50.0, 50.0, 55.0, 55.0);
  const Polygon* deep_poly_ptr = deep_poly.get();
  std::vector<std::unique_ptr<Geometry>> deep_geoms;
  deep_geoms.push_back(std::move(deep_poly));
  auto deep_gc = factory->createGeometryCollection(std::move(deep_geoms));

  std::vector<std::unique_ptr<Surface>> surfaces;
  auto ms_poly = MakeRectPoly(*factory, 40.0, 40.0, 42.0, 42.0);
  const Polygon* ms_poly_ptr = ms_poly.get();
  surfaces.push_back(std::move(ms_poly));
  surfaces.push_back(MakeCircularCurvePolygon(*factory, 30.0, 30.0, 1.0));
  auto multi_surface = factory->createMultiSurface(std::move(surfaces));

  auto inner_empty_poly = factory->createPolygon();
  const Polygon* inner_empty_poly_ptr = inner_empty_poly.get();
  std::vector<std::unique_ptr<Geometry>> inner_geoms;
  inner_geoms.push_back(factory->createPoint(CoordinateXY{1.0, 2.0}));
  inner_geoms.push_back(std::move(inner_empty_poly));
  inner_geoms.push_back(std::move(multi_surface));
  inner_geoms.push_back(std::move(deep_gc));
  auto inner_gc = factory->createGeometryCollection(std::move(inner_geoms));

  std::vector<std::unique_ptr<Polygon>> mp_polys;
  mp_polys.push_back(MakeRectPoly(*factory, 10.0, 0.0, 12.0, 2.0));
  mp_polys.push_back(MakeRectPoly(*factory, 20.0, 0.0, 22.0, 2.0));
  const Polygon* mp_poly0_ptr = mp_polys[0].get();
  const Polygon* mp_poly1_ptr = mp_polys[1].get();
  auto multi_poly = factory->createMultiPolygon(std::move(mp_polys));

  auto top_poly = MakeRectPoly(*factory, 0.0, 0.0, 3.0, 3.0);
  const Polygon* top_poly_ptr = top_poly.get();

  std::vector<std::unique_ptr<Geometry>> outer_geoms;
  outer_geoms.push_back(MakeLineString(*factory, {{0.0, 0.0}, {1.0, 1.0}}));
  outer_geoms.push_back(std::move(top_poly));
  outer_geoms.push_back(MakeCircularCurvePolygon(*factory, 0.0, 10.0, 1.0));
  outer_geoms.push_back(std::move(multi_poly));
  outer_geoms.push_back(std::move(inner_gc));
  outer_geoms.push_back(factory->createGeometryCollection());
  auto root_gc = factory->createGeometryCollection(std::move(outer_geoms));

  std::vector<const Polygon*> extracted = {preexisting.get()};
  PolygonExtracter::getPolygons(*root_gc, extracted);
  EXPECT_THAT(
      extracted,
      ElementsAre(preexisting.get(), top_poly_ptr, mp_poly0_ptr, mp_poly1_ptr,
                  inner_empty_poly_ptr, ms_poly_ptr, deep_poly_ptr));
}

TEST(PolygonExtracterTest, DirectFilterRoReuseAcrossMultipleGeometries) {
  auto factory = GeometryFactory::create();

  auto pt = factory->createPoint(CoordinateXY{1.0, 2.0});
  auto line = MakeLineString(*factory, {{0.0, 0.0}, {1.0, 0.0}});
  auto poly1 = MakeRectPoly(*factory, 0.0, 0.0, 2.0, 2.0);
  auto curve_poly = MakeCircularCurvePolygon(*factory, 5.0, 5.0, 1.0);

  std::vector<std::unique_ptr<Polygon>> mp_polys;
  mp_polys.push_back(MakeRectPoly(*factory, 10.0, 10.0, 11.0, 11.0));
  const Polygon* mp_poly_ptr = mp_polys[0].get();
  auto mp = factory->createMultiPolygon(std::move(mp_polys));

  auto poly2 = MakeRectPoly(*factory, 20.0, 20.0, 21.0, 21.0);
  auto empty_gc = factory->createGeometryCollection();

  std::vector<const Polygon*> extracted;
  PolygonExtracter filter(extracted);
  GeometryFilter& base_filter = filter;

  pt->apply_ro(&filter);
  line->apply_ro(&filter);
  poly1->apply_ro(&filter);
  curve_poly->apply_ro(&filter);
  mp->apply_ro(&filter);
  empty_gc->apply_ro(&filter);
  base_filter.filter_ro(pt.get());
  base_filter.filter_ro(poly2.get());

  EXPECT_THAT(extracted, ElementsAre(poly1.get(), mp_poly_ptr, poly2.get()));
}

TEST(PolygonExtracterTest, DirectFilterRwViaApplyRwAndVirtualDispatch) {
  auto factory = GeometryFactory::create();

  auto poly1 = MakeRectPoly(*factory, 0.0, 0.0, 2.0, 2.0);
  auto empty_poly = factory->createPolygon();
  auto pt = factory->createPoint(CoordinateXY{1.0, 2.0});
  auto line = MakeLineString(*factory, {{0.0, 0.0}, {1.0, 1.0}});
  auto curve_poly = MakeCircularCurvePolygon(*factory, 0.0, 0.0, 2.0);
  auto empty_mp = factory->createMultiPolygon();

  std::vector<std::unique_ptr<Polygon>> mp_polys;
  mp_polys.push_back(MakeRectPoly(*factory, 3.0, 3.0, 4.0, 4.0));
  mp_polys.push_back(MakeRectPoly(*factory, 5.0, 5.0, 6.0, 6.0));
  const Polygon* mp_poly0_ptr = mp_polys[0].get();
  const Polygon* mp_poly1_ptr = mp_polys[1].get();
  auto mp = factory->createMultiPolygon(std::move(mp_polys));

  std::vector<std::unique_ptr<Surface>> surfaces;
  auto ms_poly = MakeRectPoly(*factory, 7.0, 7.0, 8.0, 8.0);
  const Polygon* ms_poly_ptr = ms_poly.get();
  surfaces.push_back(std::move(ms_poly));
  surfaces.push_back(MakeCircularCurvePolygon(*factory, 10.0, 10.0, 1.0));
  auto ms = factory->createMultiSurface(std::move(surfaces));

  auto gc_poly = MakeRectPoly(*factory, 9.0, 9.0, 10.0, 10.0);
  const Polygon* gc_poly_ptr = gc_poly.get();
  std::vector<std::unique_ptr<Geometry>> gc_children;
  gc_children.push_back(factory->createPoint(CoordinateXY{0.0, 0.0}));
  gc_children.push_back(std::move(gc_poly));
  auto gc = factory->createGeometryCollection(std::move(gc_children));

  auto direct_poly = MakeRectPoly(*factory, 11.0, 11.0, 12.0, 12.0);

  std::vector<const Polygon*> extracted;
  PolygonExtracter filter(extracted);
  GeometryFilter& base_filter = filter;

  poly1->apply_rw(&filter);
  empty_poly->apply_rw(&filter);
  pt->apply_rw(&filter);
  line->apply_rw(&filter);
  curve_poly->apply_rw(&filter);
  empty_mp->apply_rw(&filter);
  mp->apply_rw(&filter);
  ms->apply_rw(&filter);
  gc->apply_rw(&filter);
  base_filter.filter_rw(line.get());
  base_filter.filter_rw(direct_poly.get());

  EXPECT_THAT(extracted, ElementsAre(poly1.get(), empty_poly.get(),
                                     mp_poly0_ptr, mp_poly1_ptr, ms_poly_ptr,
                                     gc_poly_ptr, direct_poly.get()));
}

}  // namespace
}  // namespace geos_google
