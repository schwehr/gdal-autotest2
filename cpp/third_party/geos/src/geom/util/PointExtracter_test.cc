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

// Unit tests for third_party/geos/src/geom/util/PointExtracter.cpp and
// third_party/geos/include/geos/geom/util/PointExtracter.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/util/PointExtracter.cpp

#include "third_party/geos/include/geos/geom/util/PointExtracter.h"

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
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Curve;
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
using geos::geom::util::PointExtracter;
using ::testing::ElementsAre;
using ::testing::IsEmpty;

static_assert(std::is_base_of_v<GeometryFilter, PointExtracter>);
static_assert(!std::is_copy_constructible_v<PointExtracter>);
static_assert(!std::is_copy_assignable_v<PointExtracter>);

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

TEST(PointExtracterTest, GetPointsExtractsSinglePointAndEmptyPoint) {
  auto factory = GeometryFactory::create();

  auto pt_xy = factory->createPoint(CoordinateXY{1.5, -2.5});
  Point::ConstVect extracted_xy;
  PointExtracter::getPoints(*pt_xy, extracted_xy);
  EXPECT_THAT(extracted_xy, ElementsAre(pt_xy.get()));
  EXPECT_DOUBLE_EQ(extracted_xy[0]->getX(), 1.5);
  EXPECT_DOUBLE_EQ(extracted_xy[0]->getY(), -2.5);

  auto pt_xyz = factory->createPoint(Coordinate{3.0, 4.0, 5.0});
  auto pt_xym = factory->createPoint(CoordinateXYM{6.0, 7.0, 8.0});
  auto pt_xyzm = factory->createPoint(CoordinateXYZM{9.0, 10.0, 11.0, 12.0});
  Point::ConstVect extracted_dims;
  PointExtracter::getPoints(*pt_xyz, extracted_dims);
  PointExtracter::getPoints(*pt_xym, extracted_dims);
  PointExtracter::getPoints(*pt_xyzm, extracted_dims);
  EXPECT_THAT(extracted_dims,
              ElementsAre(pt_xyz.get(), pt_xym.get(), pt_xyzm.get()));
  EXPECT_TRUE(extracted_dims[0]->hasZ());
  EXPECT_DOUBLE_EQ(extracted_dims[0]->getZ(), 5.0);
  EXPECT_TRUE(extracted_dims[1]->hasM());
  EXPECT_DOUBLE_EQ(extracted_dims[1]->getM(), 8.0);
  EXPECT_TRUE(extracted_dims[2]->hasZ());
  EXPECT_TRUE(extracted_dims[2]->hasM());

  // Empty Point still reports Dimension::P, so hasDimension(P) is true and it
  // is extracted as an empty Point component.
  auto empty_pt = factory->createPoint();
  ASSERT_TRUE(empty_pt->hasDimension(Dimension::P));
  Point::ConstVect extracted_empty;
  PointExtracter::getPoints(*empty_pt, extracted_empty);
  EXPECT_THAT(extracted_empty, ElementsAre(empty_pt.get()));
  EXPECT_TRUE(extracted_empty[0]->isEmpty());
}

TEST(PointExtracterTest, GetPointsReturnsEarlyForNonPointGeometries) {
  auto factory = GeometryFactory::create();

  auto line = MakeLineString(*factory, {{0.0, 0.0}, {1.0, 1.0}});
  auto empty_line = factory->createLineString();
  auto ring = MakeRectRing(*factory, 0.0, 0.0, 2.0, 2.0);
  auto circular =
      MakeCircularString(*factory, {{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}});

  std::vector<std::unique_ptr<SimpleCurve>> sections;
  sections.push_back(MakeLineString(*factory, {{0.0, 0.0}, {1.0, 0.0}}));
  sections.push_back(MakeLineString(*factory, {{1.0, 0.0}, {1.0, 1.0}}));
  auto compound = factory->createCompoundCurve(std::move(sections));

  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(MakeRectRing(*factory, 1.0, 1.0, 2.0, 2.0));
  auto poly_with_hole = factory->createPolygon(
      MakeRectRing(*factory, 0.0, 0.0, 5.0, 5.0), std::move(holes));
  auto empty_poly = factory->createPolygon();
  auto curve_poly = factory->createCurvePolygon(MakeCircularString(
      *factory, {{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}, {1.0, -1.0}, {0.0, 0.0}}));

  std::vector<std::unique_ptr<LineString>> lines;
  lines.push_back(MakeLineString(*factory, {{0.0, 0.0}, {1.0, 0.0}}));
  auto multi_line = factory->createMultiLineString(std::move(lines));

  std::vector<std::unique_ptr<Curve>> curves;
  curves.push_back(MakeLineString(*factory, {{0.0, 0.0}, {2.0, 0.0}}));
  auto multi_curve = factory->createMultiCurve(std::move(curves));

  std::vector<std::unique_ptr<Polygon>> polys;
  polys.push_back(MakeRectPoly(*factory, 0.0, 0.0, 3.0, 3.0));
  auto multi_poly = factory->createMultiPolygon(std::move(polys));

  std::vector<std::unique_ptr<Surface>> surfaces;
  surfaces.push_back(MakeRectPoly(*factory, 0.0, 0.0, 4.0, 4.0));
  auto multi_surface = factory->createMultiSurface(std::move(surfaces));

  auto empty_gc = factory->createGeometryCollection();

  std::vector<std::unique_ptr<Geometry>> non_point_children;
  non_point_children.push_back(
      MakeLineString(*factory, {{2.0, 2.0}, {3.0, 3.0}}));
  non_point_children.push_back(MakeRectPoly(*factory, 10.0, 10.0, 12.0, 12.0));
  non_point_children.push_back(factory->createGeometryCollection());
  auto non_point_gc =
      factory->createGeometryCollection(std::move(non_point_children));

  const std::array<const Geometry*, 14> non_point_geoms = {
      line.get(),        empty_line.get(),   ring.get(),
      circular.get(),    compound.get(),     poly_with_hole.get(),
      empty_poly.get(),  curve_poly.get(),   multi_line.get(),
      multi_curve.get(), multi_poly.get(),   multi_surface.get(),
      empty_gc.get(),    non_point_gc.get(),
  };

  auto sentinel = factory->createPoint(CoordinateXY{99.0, 99.0});
  for (const Geometry* geom : non_point_geoms) {
    ASSERT_FALSE(geom->hasDimension(Dimension::P));

    Point::ConstVect empty_ret;
    PointExtracter::getPoints(*geom, empty_ret);
    EXPECT_THAT(empty_ret, IsEmpty());

    Point::ConstVect preserved_ret = {sentinel.get()};
    PointExtracter::getPoints(*geom, preserved_ret);
    EXPECT_THAT(preserved_ret, ElementsAre(sentinel.get()));
  }
}

TEST(PointExtracterTest, GetPointsHandlesEmptyAndNonEmptyMultiPoint) {
  auto factory = GeometryFactory::create();

  // MultiPoint::hasDimension(Dimension::P) is true even when empty, so apply_ro
  // visits the MultiPoint container itself (GEOS_MULTIPOINT != GEOS_POINT).
  auto empty_mp = factory->createMultiPoint();
  ASSERT_TRUE(empty_mp->hasDimension(Dimension::P));
  Point::ConstVect extracted_empty_mp;
  PointExtracter::getPoints(*empty_mp, extracted_empty_mp);
  EXPECT_THAT(extracted_empty_mp, IsEmpty());

  std::vector<std::unique_ptr<Point>> pts;
  pts.push_back(factory->createPoint(CoordinateXY{1.0, 2.0}));
  pts.push_back(factory->createPoint());
  pts.push_back(factory->createPoint(CoordinateXY{3.0, 4.0}));
  const Point* pt0 = pts[0].get();
  const Point* pt1 = pts[1].get();
  const Point* pt2 = pts[2].get();
  auto multi_pt = factory->createMultiPoint(std::move(pts));

  Point::ConstVect extracted_mp;
  PointExtracter::getPoints(*multi_pt, extracted_mp);
  EXPECT_THAT(extracted_mp, ElementsAre(pt0, pt1, pt2));

  // A GeometryCollection holding an empty MultiPoint reports hasDimension(P).
  std::vector<std::unique_ptr<Geometry>> geoms;
  geoms.push_back(factory->createMultiPoint());
  geoms.push_back(MakeLineString(*factory, {{0.0, 0.0}, {1.0, 1.0}}));
  auto gc_with_empty_mp = factory->createGeometryCollection(std::move(geoms));
  ASSERT_TRUE(gc_with_empty_mp->hasDimension(Dimension::P));

  Point::ConstVect extracted_gc_empty_mp;
  PointExtracter::getPoints(*gc_with_empty_mp, extracted_gc_empty_mp);
  EXPECT_THAT(extracted_gc_empty_mp, IsEmpty());
}

TEST(PointExtracterTest,
     GetPointsExtractsNestedCollectionAndPreservesExistingEntries) {
  auto factory = GeometryFactory::create();

  auto preexisting = factory->createPoint(CoordinateXY{-1.0, -1.0});

  auto deep_pt = factory->createPoint(CoordinateXY{50.0, 60.0});
  const Point* deep_pt_ptr = deep_pt.get();
  std::vector<std::unique_ptr<Geometry>> deep_geoms;
  deep_geoms.push_back(std::move(deep_pt));
  auto deep_gc = factory->createGeometryCollection(std::move(deep_geoms));

  std::vector<std::unique_ptr<LineString>> lines;
  lines.push_back(MakeLineString(*factory, {{0.0, 0.0}, {2.0, 2.0}}));
  auto multi_line = factory->createMultiLineString(std::move(lines));

  std::vector<std::unique_ptr<Polygon>> polys;
  polys.push_back(MakeRectPoly(*factory, 0.0, 0.0, 2.0, 2.0));
  auto multi_poly = factory->createMultiPolygon(std::move(polys));

  auto inner_empty_pt = factory->createPoint();
  const Point* inner_empty_pt_ptr = inner_empty_pt.get();
  std::vector<std::unique_ptr<Geometry>> inner_geoms;
  inner_geoms.push_back(std::move(inner_empty_pt));
  inner_geoms.push_back(std::move(multi_line));
  inner_geoms.push_back(std::move(multi_poly));
  inner_geoms.push_back(std::move(deep_gc));
  auto inner_gc = factory->createGeometryCollection(std::move(inner_geoms));

  std::vector<std::unique_ptr<Point>> mp_pts;
  mp_pts.push_back(factory->createPoint(CoordinateXY{20.0, 21.0}));
  mp_pts.push_back(factory->createPoint(CoordinateXY{30.0, 31.0}));
  const Point* mp_pt0_ptr = mp_pts[0].get();
  const Point* mp_pt1_ptr = mp_pts[1].get();
  auto multi_pt = factory->createMultiPoint(std::move(mp_pts));

  auto top_pt = factory->createPoint(CoordinateXY{10.0, 11.0});
  const Point* top_pt_ptr = top_pt.get();

  std::vector<std::unique_ptr<Geometry>> outer_geoms;
  outer_geoms.push_back(std::move(top_pt));
  outer_geoms.push_back(MakeLineString(*factory, {{1.0, 0.0}, {2.0, 0.0}}));
  outer_geoms.push_back(MakeRectPoly(*factory, 0.0, 0.0, 4.0, 4.0));
  outer_geoms.push_back(std::move(multi_pt));
  outer_geoms.push_back(std::move(inner_gc));
  outer_geoms.push_back(factory->createGeometryCollection());
  auto root_gc = factory->createGeometryCollection(std::move(outer_geoms));

  Point::ConstVect extracted = {preexisting.get()};
  PointExtracter::getPoints(*root_gc, extracted);
  EXPECT_THAT(extracted,
              ElementsAre(preexisting.get(), top_pt_ptr, mp_pt0_ptr, mp_pt1_ptr,
                          inner_empty_pt_ptr, deep_pt_ptr));
}

TEST(PointExtracterTest, DirectFilterRoReuseAcrossMultipleGeometries) {
  auto factory = GeometryFactory::create();

  auto pt1 = factory->createPoint(CoordinateXY{1.0, 2.0});
  auto line = MakeLineString(*factory, {{0.0, 0.0}, {1.0, 0.0}});
  auto poly = MakeRectPoly(*factory, 0.0, 0.0, 3.0, 3.0);

  std::vector<std::unique_ptr<Point>> mp_pts;
  mp_pts.push_back(factory->createPoint(CoordinateXY{4.0, 5.0}));
  const Point* mp_pt_ptr = mp_pts[0].get();
  auto mp = factory->createMultiPoint(std::move(mp_pts));

  auto pt2 = factory->createPoint(CoordinateXY{6.0, 7.0});
  auto empty_gc = factory->createGeometryCollection();

  Point::ConstVect extracted;
  PointExtracter filter(extracted);
  GeometryFilter& base_filter = filter;

  pt1->apply_ro(&filter);
  line->apply_ro(&filter);
  poly->apply_ro(&filter);
  mp->apply_ro(&filter);
  empty_gc->apply_ro(&filter);
  base_filter.filter_ro(line.get());
  base_filter.filter_ro(pt2.get());

  EXPECT_THAT(extracted, ElementsAre(pt1.get(), mp_pt_ptr, pt2.get()));
}

TEST(PointExtracterTest, DirectFilterRwViaApplyRwAndVirtualDispatch) {
  auto factory = GeometryFactory::create();

  auto pt1 = factory->createPoint(CoordinateXY{1.0, 2.0});
  auto empty_pt = factory->createPoint();
  auto line = MakeLineString(*factory, {{0.0, 0.0}, {1.0, 1.0}});
  auto poly = MakeRectPoly(*factory, 0.0, 0.0, 2.0, 2.0);
  auto empty_mp = factory->createMultiPoint();

  std::vector<std::unique_ptr<Point>> mp_pts;
  mp_pts.push_back(factory->createPoint(CoordinateXY{3.0, 4.0}));
  mp_pts.push_back(factory->createPoint(CoordinateXY{5.0, 6.0}));
  const Point* mp_pt0_ptr = mp_pts[0].get();
  const Point* mp_pt1_ptr = mp_pts[1].get();
  auto mp = factory->createMultiPoint(std::move(mp_pts));

  auto gc_pt = factory->createPoint(CoordinateXY{7.0, 8.0});
  const Point* gc_pt_ptr = gc_pt.get();
  std::vector<std::unique_ptr<Geometry>> gc_children;
  gc_children.push_back(MakeLineString(*factory, {{1.0, 1.0}, {2.0, 2.0}}));
  gc_children.push_back(std::move(gc_pt));
  gc_children.push_back(MakeRectPoly(*factory, 3.0, 3.0, 5.0, 5.0));
  auto gc = factory->createGeometryCollection(std::move(gc_children));

  auto direct_pt = factory->createPoint(CoordinateXY{9.0, 10.0});

  Point::ConstVect extracted;
  PointExtracter filter(extracted);
  GeometryFilter& base_filter = filter;

  pt1->apply_rw(&filter);
  empty_pt->apply_rw(&filter);
  line->apply_rw(&filter);
  poly->apply_rw(&filter);
  empty_mp->apply_rw(&filter);
  mp->apply_rw(&filter);
  gc->apply_rw(&filter);
  base_filter.filter_rw(line.get());
  base_filter.filter_rw(direct_pt.get());

  EXPECT_THAT(extracted, ElementsAre(pt1.get(), empty_pt.get(), mp_pt0_ptr,
                                     mp_pt1_ptr, gc_pt_ptr, direct_pt.get()));
}

}  // namespace
}  // namespace geos_google
