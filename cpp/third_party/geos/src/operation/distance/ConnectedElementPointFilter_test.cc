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

#include "third_party/geos/include/geos/operation/distance/ConnectedElementPointFilter.h"

#include <initializer_list>
#include <memory>
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

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Curve;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::SimpleCurve;
using geos::geom::Surface;
using geos::operation::distance::ConnectedElementPointFilter;

std::unique_ptr<CoordinateSequence> MakeSequence(
    std::initializer_list<CoordinateXY> coords) {
  auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
  seq->reserve(coords.size());
  for (const CoordinateXY& c : coords) {
    seq->add(c);
  }
  return seq;
}

std::unique_ptr<std::vector<const CoordinateXY*>> GetCoordinatesOwned(
    const Geometry* geom) {
  return std::unique_ptr<std::vector<const CoordinateXY*>>(
      ConnectedElementPointFilter::getCoordinates(geom));
}

class ConnectedElementPointFilterTest : public ::testing::Test {
 protected:
  GeometryFactory::Ptr factory_ = GeometryFactory::create();
};

TEST_F(ConnectedElementPointFilterTest,
       SingleNonEmptyPointReturnsCoordinatePointer) {
  auto pt2d = factory_->createPoint(CoordinateXY(1.5, -2.5));
  auto coords2d = GetCoordinatesOwned(pt2d.get());
  ASSERT_EQ(coords2d->size(), 1u);
  EXPECT_EQ((*coords2d)[0], pt2d->getCoordinate());
  ASSERT_NE((*coords2d)[0], nullptr);
  EXPECT_EQ(*(*coords2d)[0], CoordinateXY(1.5, -2.5));

  auto pt3d = factory_->createPoint(Coordinate(3.0, 4.0, 5.0));
  auto coords3d = GetCoordinatesOwned(pt3d.get());
  ASSERT_EQ(coords3d->size(), 1u);
  EXPECT_EQ((*coords3d)[0], pt3d->getCoordinate());
  ASSERT_NE((*coords3d)[0], nullptr);
  EXPECT_EQ(*(*coords3d)[0], CoordinateXY(3.0, 4.0));

  auto ptxym = factory_->createPoint(CoordinateXYM(6.0, 7.0, 8.0));
  auto coordsxym = GetCoordinatesOwned(ptxym.get());
  ASSERT_EQ(coordsxym->size(), 1u);
  EXPECT_EQ((*coordsxym)[0], ptxym->getCoordinate());
  ASSERT_NE((*coordsxym)[0], nullptr);
  EXPECT_EQ(*(*coordsxym)[0], CoordinateXY(6.0, 7.0));

  auto ptxyzm = factory_->createPoint(CoordinateXYZM(9.0, 10.0, 11.0, 12.0));
  auto coordsxyzm = GetCoordinatesOwned(ptxyzm.get());
  ASSERT_EQ(coordsxyzm->size(), 1u);
  EXPECT_EQ((*coordsxyzm)[0], ptxyzm->getCoordinate());
  ASSERT_NE((*coordsxyzm)[0], nullptr);
  EXPECT_EQ(*(*coordsxyzm)[0], CoordinateXY(9.0, 10.0));
}

TEST_F(ConnectedElementPointFilterTest,
       SingleNonEmptyLineStringReturnsFirstVertexPointer) {
  auto line = factory_->createLineString(
      MakeSequence({{10.0, 20.0}, {30.0, 40.0}, {50.0, 60.0}}));
  auto coords = GetCoordinatesOwned(line.get());
  ASSERT_EQ(coords->size(), 1u);
  EXPECT_EQ((*coords)[0], line->getCoordinate());
  ASSERT_NE((*coords)[0], nullptr);
  EXPECT_EQ(*(*coords)[0], CoordinateXY(10.0, 20.0));
}

TEST_F(ConnectedElementPointFilterTest,
       SingleNonEmptyPolygonWithHoleReturnsExteriorFirstVertexPointer) {
  auto shell = factory_->createLinearRing(MakeSequence(
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}}));
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(factory_->createLinearRing(MakeSequence(
      {{2.0, 2.0}, {4.0, 2.0}, {4.0, 4.0}, {2.0, 4.0}, {2.0, 2.0}})));
  auto poly = factory_->createPolygon(std::move(shell), std::move(holes));

  auto coords = GetCoordinatesOwned(poly.get());
  ASSERT_EQ(coords->size(), 1u);
  EXPECT_EQ((*coords)[0], poly->getCoordinate());
  ASSERT_NE((*coords)[0], nullptr);
  EXPECT_EQ(*(*coords)[0], CoordinateXY(0.0, 0.0));
}

TEST_F(ConnectedElementPointFilterTest,
       EmptyPointLineStringAndPolygonPushNullCoordinatePointers) {
  auto empty_pt = factory_->createPoint(2);
  auto pt_coords = GetCoordinatesOwned(empty_pt.get());
  ASSERT_EQ(pt_coords->size(), 1u);
  EXPECT_EQ((*pt_coords)[0], nullptr);

  auto empty_line = factory_->createLineString(2);
  auto line_coords = GetCoordinatesOwned(empty_line.get());
  ASSERT_EQ(line_coords->size(), 1u);
  EXPECT_EQ((*line_coords)[0], nullptr);

  auto empty_poly = factory_->createPolygon(2);
  auto poly_coords = GetCoordinatesOwned(empty_poly.get());
  ASSERT_EQ(poly_coords->size(), 1u);
  EXPECT_EQ((*poly_coords)[0], nullptr);

  EXPECT_TRUE(GetCoordinatesOwned(factory_->createMultiPoint().get())->empty());
  EXPECT_TRUE(
      GetCoordinatesOwned(factory_->createMultiLineString().get())->empty());
  EXPECT_TRUE(
      GetCoordinatesOwned(factory_->createMultiPolygon().get())->empty());
  EXPECT_TRUE(
      GetCoordinatesOwned(factory_->createGeometryCollection().get())->empty());
  EXPECT_TRUE(GetCoordinatesOwned(factory_->createMultiCurve().get())->empty());
  EXPECT_TRUE(
      GetCoordinatesOwned(factory_->createMultiSurface().get())->empty());
}

TEST_F(ConnectedElementPointFilterTest,
       LinearRingAndCurvedGeometriesAreIgnored) {
  auto ring = factory_->createLinearRing(
      MakeSequence({{1.0, 2.0}, {5.0, 2.0}, {5.0, 6.0}, {1.0, 2.0}}));
  EXPECT_TRUE(GetCoordinatesOwned(ring.get())->empty());
  EXPECT_TRUE(
      GetCoordinatesOwned(factory_->createLinearRing(2).get())->empty());

  auto arc = factory_->createCircularString(
      MakeSequence({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}}));
  EXPECT_TRUE(GetCoordinatesOwned(arc.get())->empty());
  EXPECT_TRUE(
      GetCoordinatesOwned(factory_->createCircularString(false, false).get())
          ->empty());

  std::vector<std::unique_ptr<SimpleCurve>> sections;
  sections.push_back(
      factory_->createLineString(MakeSequence({{0.0, 0.0}, {2.0, 0.0}})));
  sections.push_back(factory_->createCircularString(
      MakeSequence({{2.0, 0.0}, {3.0, 1.0}, {4.0, 0.0}})));
  auto compound = factory_->createCompoundCurve(std::move(sections));
  EXPECT_TRUE(GetCoordinatesOwned(compound.get())->empty());
  EXPECT_TRUE(
      GetCoordinatesOwned(factory_->createCompoundCurve().get())->empty());

  auto curve_poly =
      factory_->createCurvePolygon(factory_->createCircularString(MakeSequence(
          {{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}, {1.0, -1.0}, {0.0, 0.0}})));
  EXPECT_TRUE(GetCoordinatesOwned(curve_poly.get())->empty());
  EXPECT_TRUE(
      GetCoordinatesOwned(factory_->createCurvePolygon(false, false).get())
          ->empty());
}

TEST_F(ConnectedElementPointFilterTest,
       MultiGeometriesExtractChildCoordinatePointers) {
  std::vector<std::unique_ptr<Point>> pts;
  pts.push_back(factory_->createPoint(CoordinateXY(1.0, 2.0)));
  pts.push_back(factory_->createPoint());
  pts.push_back(factory_->createPoint(CoordinateXY(3.0, 4.0)));
  auto mp = factory_->createMultiPoint(std::move(pts));

  auto mp_coords = GetCoordinatesOwned(mp.get());
  ASSERT_EQ(mp_coords->size(), 3u);
  EXPECT_EQ((*mp_coords)[0], mp->getGeometryN(0)->getCoordinate());
  EXPECT_EQ(*(*mp_coords)[0], CoordinateXY(1.0, 2.0));
  EXPECT_EQ((*mp_coords)[1], nullptr);
  EXPECT_EQ((*mp_coords)[2], mp->getGeometryN(2)->getCoordinate());
  EXPECT_EQ(*(*mp_coords)[2], CoordinateXY(3.0, 4.0));

  std::vector<std::unique_ptr<LineString>> lines;
  lines.push_back(
      factory_->createLineString(MakeSequence({{5.0, 6.0}, {7.0, 8.0}})));
  lines.push_back(factory_->createLineString());
  lines.push_back(
      factory_->createLineString(MakeSequence({{9.0, 10.0}, {11.0, 12.0}})));
  auto mls = factory_->createMultiLineString(std::move(lines));

  auto mls_coords = GetCoordinatesOwned(mls.get());
  ASSERT_EQ(mls_coords->size(), 3u);
  EXPECT_EQ((*mls_coords)[0], mls->getGeometryN(0)->getCoordinate());
  EXPECT_EQ(*(*mls_coords)[0], CoordinateXY(5.0, 6.0));
  EXPECT_EQ((*mls_coords)[1], nullptr);
  EXPECT_EQ((*mls_coords)[2], mls->getGeometryN(2)->getCoordinate());
  EXPECT_EQ(*(*mls_coords)[2], CoordinateXY(9.0, 10.0));

  std::vector<std::unique_ptr<Polygon>> polys;
  polys.push_back(factory_->createPolygon(factory_->createLinearRing(
      MakeSequence({{0.0, 0.0}, {2.0, 0.0}, {2.0, 2.0}, {0.0, 0.0}}))));
  polys.push_back(factory_->createPolygon());
  polys.push_back(factory_->createPolygon(factory_->createLinearRing(
      MakeSequence({{10.0, 10.0}, {12.0, 10.0}, {12.0, 12.0}, {10.0, 10.0}}))));
  auto mpoly = factory_->createMultiPolygon(std::move(polys));

  auto mpoly_coords = GetCoordinatesOwned(mpoly.get());
  ASSERT_EQ(mpoly_coords->size(), 3u);
  EXPECT_EQ((*mpoly_coords)[0], mpoly->getGeometryN(0)->getCoordinate());
  EXPECT_EQ(*(*mpoly_coords)[0], CoordinateXY(0.0, 0.0));
  EXPECT_EQ((*mpoly_coords)[1], nullptr);
  EXPECT_EQ((*mpoly_coords)[2], mpoly->getGeometryN(2)->getCoordinate());
  EXPECT_EQ(*(*mpoly_coords)[2], CoordinateXY(10.0, 10.0));

  std::vector<std::unique_ptr<Curve>> curves;
  curves.push_back(
      factory_->createLineString(MakeSequence({{1.0, 2.0}, {3.0, 4.0}})));
  curves.push_back(factory_->createCircularString(
      MakeSequence({{3.0, 4.0}, {4.0, 5.0}, {5.0, 4.0}})));
  auto multi_curve = factory_->createMultiCurve(std::move(curves));

  auto mc_coords = GetCoordinatesOwned(multi_curve.get());
  ASSERT_EQ(mc_coords->size(), 1u);
  EXPECT_EQ((*mc_coords)[0], multi_curve->getGeometryN(0)->getCoordinate());
  EXPECT_EQ(*(*mc_coords)[0], CoordinateXY(1.0, 2.0));

  std::vector<std::unique_ptr<Surface>> surfaces;
  surfaces.push_back(factory_->createPolygon(factory_->createLinearRing(
      MakeSequence({{5.0, 5.0}, {8.0, 5.0}, {8.0, 8.0}, {5.0, 5.0}}))));
  surfaces.push_back(
      factory_->createCurvePolygon(factory_->createCircularString(MakeSequence(
          {{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}, {1.0, -1.0}, {0.0, 0.0}}))));
  auto multi_surface = factory_->createMultiSurface(std::move(surfaces));

  auto ms_coords = GetCoordinatesOwned(multi_surface.get());
  ASSERT_EQ(ms_coords->size(), 1u);
  EXPECT_EQ((*ms_coords)[0], multi_surface->getGeometryN(0)->getCoordinate());
  EXPECT_EQ(*(*ms_coords)[0], CoordinateXY(5.0, 5.0));
}

TEST_F(ConnectedElementPointFilterTest,
       NestedHeterogeneousGeometryCollectionExtractsInOrder) {
  std::vector<std::unique_ptr<Geometry>> inner;
  inner.push_back(factory_->createPolygon(factory_->createLinearRing(
      MakeSequence({{10.0, 10.0}, {20.0, 10.0}, {20.0, 20.0}, {10.0, 10.0}}))));
  inner.push_back(factory_->createPoint());
  auto inner_gc = factory_->createGeometryCollection(std::move(inner));
  const CoordinateXY* expected_poly_coord =
      inner_gc->getGeometryN(0)->getCoordinate();

  std::vector<std::unique_ptr<Point>> mp_pts;
  mp_pts.push_back(factory_->createPoint(CoordinateXY(30.0, 40.0)));
  auto mp = factory_->createMultiPoint(std::move(mp_pts));
  const CoordinateXY* expected_mp_coord = mp->getGeometryN(0)->getCoordinate();

  std::vector<std::unique_ptr<Geometry>> outer;
  outer.push_back(factory_->createPoint(CoordinateXY(1.0, 1.0)));
  outer.push_back(
      factory_->createLineString(MakeSequence({{2.0, 2.0}, {3.0, 3.0}})));
  outer.push_back(factory_->createLinearRing(
      MakeSequence({{4.0, 4.0}, {6.0, 4.0}, {6.0, 6.0}, {4.0, 4.0}})));
  outer.push_back(std::move(inner_gc));
  outer.push_back(std::move(mp));
  outer.push_back(factory_->createCircularString(
      MakeSequence({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}})));
  auto gc = factory_->createGeometryCollection(std::move(outer));

  auto coords = GetCoordinatesOwned(gc.get());
  ASSERT_EQ(coords->size(), 5u);
  EXPECT_EQ((*coords)[0], gc->getGeometryN(0)->getCoordinate());
  EXPECT_EQ(*(*coords)[0], CoordinateXY(1.0, 1.0));
  EXPECT_EQ((*coords)[1], gc->getGeometryN(1)->getCoordinate());
  EXPECT_EQ(*(*coords)[1], CoordinateXY(2.0, 2.0));
  EXPECT_EQ((*coords)[2], expected_poly_coord);
  EXPECT_EQ(*(*coords)[2], CoordinateXY(10.0, 10.0));
  EXPECT_EQ((*coords)[3], nullptr);
  EXPECT_EQ((*coords)[4], expected_mp_coord);
  EXPECT_EQ(*(*coords)[4], CoordinateXY(30.0, 40.0));
}

TEST_F(ConnectedElementPointFilterTest,
       DirectFilterConstructionAppendsAcrossMultipleApplies) {
  auto pt = factory_->createPoint(CoordinateXY(1.0, 2.0));
  auto ring = factory_->createLinearRing(
      MakeSequence({{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 0.0}}));
  auto line =
      factory_->createLineString(MakeSequence({{3.0, 4.0}, {5.0, 6.0}}));
  auto poly = factory_->createPolygon(factory_->createLinearRing(
      MakeSequence({{7.0, 8.0}, {9.0, 8.0}, {9.0, 10.0}, {7.0, 8.0}})));
  std::vector<std::unique_ptr<Point>> mp_pts;
  mp_pts.push_back(factory_->createPoint(CoordinateXY(11.0, 12.0)));
  auto mp = factory_->createMultiPoint(std::move(mp_pts));

  std::vector<const CoordinateXY*> collected;
  ConnectedElementPointFilter filter(&collected);

  pt->apply_ro(&filter);
  ring->apply_ro(&filter);
  line->apply_ro(&filter);
  poly->apply_ro(&filter);
  filter.filter_ro(mp.get());

  ASSERT_EQ(collected.size(), 3u);
  EXPECT_EQ(collected[0], pt->getCoordinate());
  EXPECT_EQ(collected[1], line->getCoordinate());
  EXPECT_EQ(collected[2], poly->getCoordinate());
}

}  // namespace
}  // namespace geos_google
