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

// Unit tests for third_party/geos/src/geom/util/PolygonalExtracter.cpp and
// third_party/geos/include/geos/geom/util/PolygonalExtracter.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/util/PolygonalExtracter.cpp

#include "third_party/geos/include/geos/geom/util/PolygonalExtracter.h"

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

using geos::geom::CircularString;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Curve;
using geos::geom::CurvePolygon;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::SimpleCurve;
using geos::geom::Surface;
using geos::geom::util::PolygonalExtracter;
using ::testing::ElementsAre;
using ::testing::IsEmpty;

static_assert(!std::is_default_constructible_v<PolygonalExtracter>);
static_assert(!std::is_copy_constructible_v<PolygonalExtracter>);
static_assert(!std::is_copy_assignable_v<PolygonalExtracter>);

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

TEST(PolygonalExtracterTest,
     GetPolygonalsExtractsSinglePolygonWithHolesAndEmptyPolygon) {
  auto factory = GeometryFactory::create();

  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(MakeRectRing(*factory, 1.0, 1.0, 2.0, 2.0));
  auto poly = factory->createPolygon(MakeRectRing(*factory, 0.0, 0.0, 5.0, 5.0),
                                     std::move(holes));
  std::vector<const Geometry*> extracted_poly;
  PolygonalExtracter::getPolygonals(*poly, extracted_poly);
  EXPECT_THAT(extracted_poly, ElementsAre(poly.get()));
  EXPECT_EQ(extracted_poly[0]->getGeometryTypeId(), geos::geom::GEOS_POLYGON);

  auto empty_poly = factory->createPolygon();
  std::vector<const Geometry*> extracted_empty;
  PolygonalExtracter::getPolygonals(*empty_poly, extracted_empty);
  EXPECT_THAT(extracted_empty, ElementsAre(empty_poly.get()));
  EXPECT_TRUE(extracted_empty[0]->isEmpty());
}

TEST(PolygonalExtracterTest,
     GetPolygonalsPreservesMultiPolygonWithoutFlattening) {
  auto factory = GeometryFactory::create();

  std::vector<std::unique_ptr<Polygon>> polys;
  polys.push_back(MakeRectPoly(*factory, 0.0, 0.0, 2.0, 2.0));
  polys.push_back(MakeRectPoly(*factory, 4.0, 4.0, 6.0, 6.0));
  auto multi_poly = factory->createMultiPolygon(std::move(polys));

  std::vector<const Geometry*> extracted;
  PolygonalExtracter::getPolygonals(*multi_poly, extracted);
  EXPECT_THAT(extracted, ElementsAre(multi_poly.get()));
  EXPECT_EQ(extracted[0]->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);

  auto empty_multi_poly = factory->createMultiPolygon();
  std::vector<const Geometry*> extracted_empty;
  PolygonalExtracter::getPolygonals(*empty_multi_poly, extracted_empty);
  EXPECT_THAT(extracted_empty, ElementsAre(empty_multi_poly.get()));
  EXPECT_TRUE(extracted_empty[0]->isEmpty());
}

TEST(PolygonalExtracterTest,
     GetPolygonalsIgnoresNonPolygonalAndCurveSurfaceTypes) {
  auto factory = GeometryFactory::create();

  auto pt = factory->createPoint(CoordinateXY{1.0, 2.0});
  auto empty_pt = factory->createPoint();
  auto line = MakeLineString(*factory, {{0.0, 0.0}, {1.0, 1.0}});
  auto empty_line = factory->createLineString();
  auto ring = MakeRectRing(*factory, 0.0, 0.0, 2.0, 2.0);
  auto circular =
      MakeCircularString(*factory, {{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}});

  std::vector<std::unique_ptr<SimpleCurve>> sections;
  sections.push_back(MakeLineString(*factory, {{0.0, 0.0}, {1.0, 0.0}}));
  sections.push_back(MakeLineString(*factory, {{1.0, 0.0}, {1.0, 1.0}}));
  auto compound = factory->createCompoundCurve(std::move(sections));

  auto curve_poly = MakeCircularCurvePolygon(*factory, 0.0, 0.0, 2.0);
  auto empty_curve_poly = factory->createCurvePolygon(false, false);

  std::vector<std::unique_ptr<Point>> pts;
  pts.push_back(factory->createPoint(CoordinateXY{3.0, 4.0}));
  auto multi_pt = factory->createMultiPoint(std::move(pts));
  auto empty_mp = factory->createMultiPoint();

  std::vector<std::unique_ptr<LineString>> lines;
  lines.push_back(MakeLineString(*factory, {{0.0, 0.0}, {2.0, 0.0}}));
  auto multi_line = factory->createMultiLineString(std::move(lines));
  auto empty_mls = factory->createMultiLineString();

  std::vector<std::unique_ptr<Curve>> curves;
  curves.push_back(MakeLineString(*factory, {{0.0, 0.0}, {3.0, 0.0}}));
  auto multi_curve = factory->createMultiCurve(std::move(curves));
  auto empty_mc = factory->createMultiCurve();

  auto empty_gc = factory->createGeometryCollection();

  const std::array<const Geometry*, 16> non_polygonal_geoms = {
      pt.get(),         empty_pt.get(),    line.get(),
      empty_line.get(), ring.get(),        circular.get(),
      compound.get(),   curve_poly.get(),  empty_curve_poly.get(),
      multi_pt.get(),   empty_mp.get(),    multi_line.get(),
      empty_mls.get(),  multi_curve.get(), empty_mc.get(),
      empty_gc.get(),
  };

  auto sentinel = MakeRectPoly(*factory, 0.0, 0.0, 1.0, 1.0);
  for (const Geometry* geom : non_polygonal_geoms) {
    std::vector<const Geometry*> empty_ret;
    PolygonalExtracter::getPolygonals(*geom, empty_ret);
    EXPECT_THAT(empty_ret, IsEmpty());

    std::vector<const Geometry*> preserved_ret = {sentinel.get()};
    PolygonalExtracter::getPolygonals(*geom, preserved_ret);
    EXPECT_THAT(preserved_ret, ElementsAre(sentinel.get()));
  }
}

TEST(PolygonalExtracterTest,
     GetPolygonalsExtractsOnlyPolygonChildrenFromMultiSurface) {
  auto factory = GeometryFactory::create();

  auto empty_ms = factory->createMultiSurface();
  std::vector<const Geometry*> extracted_empty_ms;
  PolygonalExtracter::getPolygonals(*empty_ms, extracted_empty_ms);
  EXPECT_THAT(extracted_empty_ms, IsEmpty());

  std::vector<std::unique_ptr<Surface>> surfaces;
  auto poly0 = MakeRectPoly(*factory, 0.0, 0.0, 2.0, 2.0);
  auto poly1 = MakeRectPoly(*factory, 10.0, 10.0, 12.0, 12.0);
  const Geometry* poly0_ptr = poly0.get();
  const Geometry* poly1_ptr = poly1.get();
  surfaces.push_back(std::move(poly0));
  surfaces.push_back(MakeCircularCurvePolygon(*factory, 5.0, 5.0, 1.0));
  surfaces.push_back(std::move(poly1));
  auto multi_surface = factory->createMultiSurface(std::move(surfaces));

  // MultiSurface inherits from GeometryCollection (and is not Polygon or
  // MultiPolygon), so PolygonalExtracter recurses into its Surface children.
  std::vector<const Geometry*> extracted_ms;
  PolygonalExtracter::getPolygonals(*multi_surface, extracted_ms);
  EXPECT_THAT(extracted_ms, ElementsAre(poly0_ptr, poly1_ptr));
}

TEST(PolygonalExtracterTest,
     GetPolygonalsExtractsNestedCollectionAndPreservesExistingEntries) {
  auto factory = GeometryFactory::create();

  auto preexisting = MakeRectPoly(*factory, -4.0, -4.0, -1.0, -1.0);

  auto deep_poly = MakeRectPoly(*factory, 50.0, 50.0, 52.0, 52.0);
  const Geometry* deep_poly_ptr = deep_poly.get();
  std::vector<std::unique_ptr<Polygon>> deep_mp_polys;
  deep_mp_polys.push_back(MakeRectPoly(*factory, 60.0, 60.0, 62.0, 62.0));
  auto deep_mp = factory->createMultiPolygon(std::move(deep_mp_polys));
  const Geometry* deep_mp_ptr = deep_mp.get();

  std::vector<std::unique_ptr<Geometry>> deep_geoms;
  deep_geoms.push_back(std::move(deep_poly));
  deep_geoms.push_back(std::move(deep_mp));
  auto deep_gc = factory->createGeometryCollection(std::move(deep_geoms));

  std::vector<std::unique_ptr<Surface>> ms_surfaces;
  auto ms_poly = MakeRectPoly(*factory, 40.0, 40.0, 41.0, 41.0);
  const Geometry* ms_poly_ptr = ms_poly.get();
  ms_surfaces.push_back(std::move(ms_poly));
  ms_surfaces.push_back(MakeCircularCurvePolygon(*factory, 35.0, 35.0, 1.0));
  auto multi_surface = factory->createMultiSurface(std::move(ms_surfaces));

  auto inner_empty_poly = factory->createPolygon();
  const Geometry* inner_empty_poly_ptr = inner_empty_poly.get();
  auto inner_empty_mp = factory->createMultiPolygon();
  const Geometry* inner_empty_mp_ptr = inner_empty_mp.get();

  std::vector<std::unique_ptr<Geometry>> inner_geoms;
  inner_geoms.push_back(std::move(inner_empty_poly));
  inner_geoms.push_back(std::move(inner_empty_mp));
  inner_geoms.push_back(std::move(multi_surface));
  inner_geoms.push_back(std::move(deep_gc));
  auto inner_gc = factory->createGeometryCollection(std::move(inner_geoms));

  std::vector<std::unique_ptr<Polygon>> top_mp_polys;
  top_mp_polys.push_back(MakeRectPoly(*factory, 10.0, 0.0, 12.0, 2.0));
  top_mp_polys.push_back(MakeRectPoly(*factory, 15.0, 0.0, 17.0, 2.0));
  auto top_mp = factory->createMultiPolygon(std::move(top_mp_polys));
  const Geometry* top_mp_ptr = top_mp.get();

  auto top_poly = MakeRectPoly(*factory, 0.0, 0.0, 3.0, 3.0);
  const Geometry* top_poly_ptr = top_poly.get();

  std::vector<std::unique_ptr<Point>> mp_pts;
  mp_pts.push_back(factory->createPoint(CoordinateXY{1.0, 1.0}));
  auto multi_pt = factory->createMultiPoint(std::move(mp_pts));

  std::vector<std::unique_ptr<LineString>> mls_lines;
  mls_lines.push_back(MakeLineString(*factory, {{0.0, 0.0}, {1.0, 0.0}}));
  auto multi_line = factory->createMultiLineString(std::move(mls_lines));

  std::vector<std::unique_ptr<Geometry>> outer_geoms;
  outer_geoms.push_back(factory->createPoint(CoordinateXY{0.0, 0.0}));
  outer_geoms.push_back(MakeLineString(*factory, {{0.0, 0.0}, {2.0, 2.0}}));
  outer_geoms.push_back(std::move(top_poly));
  outer_geoms.push_back(std::move(top_mp));
  outer_geoms.push_back(MakeCircularCurvePolygon(*factory, 0.0, 10.0, 1.0));
  outer_geoms.push_back(std::move(multi_pt));
  outer_geoms.push_back(std::move(multi_line));
  outer_geoms.push_back(std::move(inner_gc));
  outer_geoms.push_back(factory->createGeometryCollection());
  auto root_gc = factory->createGeometryCollection(std::move(outer_geoms));

  std::vector<const Geometry*> extracted = {preexisting.get()};
  PolygonalExtracter::getPolygonals(*root_gc, extracted);
  EXPECT_THAT(extracted,
              ElementsAre(preexisting.get(), top_poly_ptr, top_mp_ptr,
                          inner_empty_poly_ptr, inner_empty_mp_ptr, ms_poly_ptr,
                          deep_poly_ptr, deep_mp_ptr));
}

TEST(PolygonalExtracterTest, GetPolygonalsRepeatedCallsAccumulateInOrder) {
  auto factory = GeometryFactory::create();

  auto poly = MakeRectPoly(*factory, 0.0, 0.0, 1.0, 1.0);
  auto line = MakeLineString(*factory, {{0.0, 0.0}, {2.0, 2.0}});
  std::vector<std::unique_ptr<Polygon>> mp_polys;
  mp_polys.push_back(MakeRectPoly(*factory, 5.0, 5.0, 6.0, 6.0));
  auto mp = factory->createMultiPolygon(std::move(mp_polys));

  std::vector<const Geometry*> accumulated;
  PolygonalExtracter::getPolygonals(*poly, accumulated);
  PolygonalExtracter::getPolygonals(*line, accumulated);
  PolygonalExtracter::getPolygonals(*mp, accumulated);

  EXPECT_THAT(accumulated, ElementsAre(poly.get(), mp.get()));
}

}  // namespace
}  // namespace geos_google
