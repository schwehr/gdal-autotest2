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

#include "third_party/geos/include/geos/operation/distance/ConnectedElementLocationFilter.h"

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
#include "third_party/geos/include/geos/operation/distance/GeometryLocation.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Curve;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::GeometryFilter;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::SimpleCurve;
using geos::geom::Surface;
using geos::operation::distance::ConnectedElementLocationFilter;
using geos::operation::distance::GeometryLocation;

std::unique_ptr<CoordinateSequence> MakeSequence(
    std::initializer_list<CoordinateXY> coords) {
  auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
  seq->reserve(coords.size());
  for (const CoordinateXY& c : coords) {
    seq->add(c);
  }
  return seq;
}

void ExpectLocation(GeometryLocation& loc, const Geometry* expected_component,
                    const CoordinateXY& expected_coord) {
  EXPECT_EQ(loc.getGeometryComponent(), expected_component);
  EXPECT_EQ(loc.getSegmentIndex(), 0u);
  EXPECT_FALSE(loc.isInsideArea());
  EXPECT_EQ(loc.getCoordinate(), expected_coord);
}

// Custom GeometryCollection subclass whose apply_ro forwards mutable child
// pointers through GeometryFilter::filter_rw so
// ConnectedElementLocationFilter's filter_rw override can be exercised via the
// public getLocations entry point.
class RwForwardingCollection : public GeometryCollection {
 public:
  using GeometryCollection::apply_ro;

  RwForwardingCollection(std::vector<std::unique_ptr<Geometry>>&& geoms,
                         const GeometryFactory& factory)
      : GeometryCollection(std::move(geoms), factory) {}

  void apply_ro(GeometryFilter* filter) const override {
    filter->filter_rw(const_cast<RwForwardingCollection*>(this));
    for (const auto& child : geometries) {
      filter->filter_rw(child.get());
    }
  }
};

class ConnectedElementLocationFilterTest : public ::testing::Test {
 protected:
  GeometryFactory::Ptr factory_ = GeometryFactory::create();
};

TEST_F(ConnectedElementLocationFilterTest,
       EmptySimpleAndCollectionGeometriesReturnNoLocations) {
  EXPECT_TRUE(ConnectedElementLocationFilter::getLocations(
                  factory_->createPoint(2).get())
                  .empty());
  EXPECT_TRUE(ConnectedElementLocationFilter::getLocations(
                  factory_->createPoint(true, true).get())
                  .empty());
  EXPECT_TRUE(ConnectedElementLocationFilter::getLocations(
                  factory_->createLineString(2).get())
                  .empty());
  EXPECT_TRUE(ConnectedElementLocationFilter::getLocations(
                  factory_->createLinearRing(2).get())
                  .empty());
  EXPECT_TRUE(ConnectedElementLocationFilter::getLocations(
                  factory_->createPolygon(2).get())
                  .empty());
  EXPECT_TRUE(ConnectedElementLocationFilter::getLocations(
                  factory_->createMultiPoint().get())
                  .empty());
  EXPECT_TRUE(ConnectedElementLocationFilter::getLocations(
                  factory_->createMultiLineString().get())
                  .empty());
  EXPECT_TRUE(ConnectedElementLocationFilter::getLocations(
                  factory_->createMultiPolygon().get())
                  .empty());
  EXPECT_TRUE(ConnectedElementLocationFilter::getLocations(
                  factory_->createGeometryCollection().get())
                  .empty());
  EXPECT_TRUE(ConnectedElementLocationFilter::getLocations(
                  factory_->createCircularString(false, false).get())
                  .empty());
  EXPECT_TRUE(ConnectedElementLocationFilter::getLocations(
                  factory_->createCompoundCurve().get())
                  .empty());
  EXPECT_TRUE(ConnectedElementLocationFilter::getLocations(
                  factory_->createCurvePolygon(false, false).get())
                  .empty());
  EXPECT_TRUE(ConnectedElementLocationFilter::getLocations(
                  factory_->createMultiCurve().get())
                  .empty());
  EXPECT_TRUE(ConnectedElementLocationFilter::getLocations(
                  factory_->createMultiSurface().get())
                  .empty());
}

TEST_F(ConnectedElementLocationFilterTest,
       SingleNonEmptyPointReturnsOneLocation) {
  auto pt2d = factory_->createPoint(CoordinateXY(1.5, -2.5));
  auto locs2d = ConnectedElementLocationFilter::getLocations(pt2d.get());
  ASSERT_EQ(locs2d.size(), 1u);
  ExpectLocation(locs2d[0], pt2d.get(), CoordinateXY(1.5, -2.5));

  auto pt3d = factory_->createPoint(Coordinate(3.0, 4.0, 5.0));
  auto locs3d = ConnectedElementLocationFilter::getLocations(pt3d.get());
  ASSERT_EQ(locs3d.size(), 1u);
  ExpectLocation(locs3d[0], pt3d.get(), CoordinateXY(3.0, 4.0));

  auto ptxym = factory_->createPoint(CoordinateXYM(6.0, 7.0, 8.0));
  auto locsxym = ConnectedElementLocationFilter::getLocations(ptxym.get());
  ASSERT_EQ(locsxym.size(), 1u);
  ExpectLocation(locsxym[0], ptxym.get(), CoordinateXY(6.0, 7.0));

  auto ptxyzm = factory_->createPoint(CoordinateXYZM(9.0, 10.0, 11.0, 12.0));
  auto locsxyzm = ConnectedElementLocationFilter::getLocations(ptxyzm.get());
  ASSERT_EQ(locsxyzm.size(), 1u);
  ExpectLocation(locsxyzm[0], ptxyzm.get(), CoordinateXY(9.0, 10.0));
}

TEST_F(ConnectedElementLocationFilterTest,
       SingleNonEmptyLineStringReturnsFirstVertex) {
  auto line = factory_->createLineString(
      MakeSequence({{10.0, 20.0}, {30.0, 40.0}, {50.0, 60.0}}));
  auto locs = ConnectedElementLocationFilter::getLocations(line.get());
  ASSERT_EQ(locs.size(), 1u);
  ExpectLocation(locs[0], line.get(), CoordinateXY(10.0, 20.0));
}

TEST_F(ConnectedElementLocationFilterTest,
       SingleNonEmptyLinearRingReturnsFirstVertex) {
  auto ring = factory_->createLinearRing(
      MakeSequence({{1.0, 2.0}, {5.0, 2.0}, {5.0, 6.0}, {1.0, 2.0}}));
  auto locs = ConnectedElementLocationFilter::getLocations(ring.get());
  ASSERT_EQ(locs.size(), 1u);
  ExpectLocation(locs[0], ring.get(), CoordinateXY(1.0, 2.0));
}

TEST_F(ConnectedElementLocationFilterTest,
       SingleNonEmptyPolygonWithHoleReturnsExteriorFirstVertexOnly) {
  auto shell = factory_->createLinearRing(MakeSequence(
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}}));
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(factory_->createLinearRing(MakeSequence(
      {{2.0, 2.0}, {4.0, 2.0}, {4.0, 4.0}, {2.0, 4.0}, {2.0, 2.0}})));
  auto poly = factory_->createPolygon(std::move(shell), std::move(holes));

  auto locs = ConnectedElementLocationFilter::getLocations(poly.get());
  ASSERT_EQ(locs.size(), 1u);
  ExpectLocation(locs[0], poly.get(), CoordinateXY(0.0, 0.0));
}

TEST_F(ConnectedElementLocationFilterTest,
       MultiPointExtractsNonEmptyPointsAndSkipsEmpty) {
  std::vector<std::unique_ptr<Point>> pts;
  pts.push_back(factory_->createPoint());
  pts.push_back(factory_->createPoint(CoordinateXY(1.0, 2.0)));
  pts.push_back(factory_->createPoint(CoordinateXY(3.0, 4.0)));
  pts.push_back(factory_->createPoint());
  auto mp = factory_->createMultiPoint(std::move(pts));

  auto locs = ConnectedElementLocationFilter::getLocations(mp.get());
  ASSERT_EQ(locs.size(), 2u);
  ExpectLocation(locs[0], mp->getGeometryN(1), CoordinateXY(1.0, 2.0));
  ExpectLocation(locs[1], mp->getGeometryN(2), CoordinateXY(3.0, 4.0));
}

TEST_F(ConnectedElementLocationFilterTest,
       MultiLineStringExtractsNonEmptyLinesAndSkipsEmpty) {
  std::vector<std::unique_ptr<LineString>> lines;
  lines.push_back(
      factory_->createLineString(MakeSequence({{5.0, 6.0}, {7.0, 8.0}})));
  lines.push_back(factory_->createLineString());
  lines.push_back(
      factory_->createLineString(MakeSequence({{9.0, 10.0}, {11.0, 12.0}})));
  auto mls = factory_->createMultiLineString(std::move(lines));

  auto locs = ConnectedElementLocationFilter::getLocations(mls.get());
  ASSERT_EQ(locs.size(), 2u);
  ExpectLocation(locs[0], mls->getGeometryN(0), CoordinateXY(5.0, 6.0));
  ExpectLocation(locs[1], mls->getGeometryN(2), CoordinateXY(9.0, 10.0));
}

TEST_F(ConnectedElementLocationFilterTest,
       MultiPolygonExtractsNonEmptyPolygonsAndSkipsEmpty) {
  std::vector<std::unique_ptr<Polygon>> polys;
  polys.push_back(factory_->createPolygon(factory_->createLinearRing(
      MakeSequence({{0.0, 0.0}, {2.0, 0.0}, {2.0, 2.0}, {0.0, 0.0}}))));
  polys.push_back(factory_->createPolygon());
  polys.push_back(factory_->createPolygon(factory_->createLinearRing(
      MakeSequence({{10.0, 10.0}, {12.0, 10.0}, {12.0, 12.0}, {10.0, 10.0}}))));
  auto mpoly = factory_->createMultiPolygon(std::move(polys));

  auto locs = ConnectedElementLocationFilter::getLocations(mpoly.get());
  ASSERT_EQ(locs.size(), 2u);
  ExpectLocation(locs[0], mpoly->getGeometryN(0), CoordinateXY(0.0, 0.0));
  ExpectLocation(locs[1], mpoly->getGeometryN(2), CoordinateXY(10.0, 10.0));
}

TEST_F(ConnectedElementLocationFilterTest,
       NestedHeterogeneousCollectionExtractsAllSupportedElementsInOrder) {
  std::vector<std::unique_ptr<Geometry>> inner;
  inner.push_back(factory_->createLinearRing(
      MakeSequence({{4.0, 4.0}, {6.0, 4.0}, {6.0, 6.0}, {4.0, 4.0}})));
  inner.push_back(factory_->createPolygon(factory_->createLinearRing(
      MakeSequence({{10.0, 10.0}, {20.0, 10.0}, {20.0, 20.0}, {10.0, 10.0}}))));
  inner.push_back(factory_->createPolygon());
  auto inner_gc = factory_->createGeometryCollection(std::move(inner));
  const Geometry* expected_ring = inner_gc->getGeometryN(0);
  const Geometry* expected_poly = inner_gc->getGeometryN(1);

  std::vector<std::unique_ptr<Point>> mp_pts;
  mp_pts.push_back(factory_->createPoint(CoordinateXY(30.0, 40.0)));
  auto mp = factory_->createMultiPoint(std::move(mp_pts));
  const Geometry* expected_mp_pt = mp->getGeometryN(0);

  std::vector<std::unique_ptr<Geometry>> outer;
  outer.push_back(factory_->createPoint(CoordinateXY(1.0, 1.0)));
  outer.push_back(factory_->createPoint());
  outer.push_back(
      factory_->createLineString(MakeSequence({{2.0, 2.0}, {3.0, 3.0}})));
  outer.push_back(std::move(inner_gc));
  outer.push_back(std::move(mp));
  auto gc = factory_->createGeometryCollection(std::move(outer));

  auto locs = ConnectedElementLocationFilter::getLocations(gc.get());
  ASSERT_EQ(locs.size(), 5u);
  ExpectLocation(locs[0], gc->getGeometryN(0), CoordinateXY(1.0, 1.0));
  ExpectLocation(locs[1], gc->getGeometryN(2), CoordinateXY(2.0, 2.0));
  ExpectLocation(locs[2], expected_ring, CoordinateXY(4.0, 4.0));
  ExpectLocation(locs[3], expected_poly, CoordinateXY(10.0, 10.0));
  ExpectLocation(locs[4], expected_mp_pt, CoordinateXY(30.0, 40.0));
}

TEST_F(ConnectedElementLocationFilterTest,
       CurvedGeometriesIgnoredDirectlyAndWithinMultiCollections) {
  auto arc = factory_->createCircularString(
      MakeSequence({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}}));
  EXPECT_TRUE(ConnectedElementLocationFilter::getLocations(arc.get()).empty());

  std::vector<std::unique_ptr<SimpleCurve>> sections;
  sections.push_back(
      factory_->createLineString(MakeSequence({{0.0, 0.0}, {2.0, 0.0}})));
  sections.push_back(factory_->createCircularString(
      MakeSequence({{2.0, 0.0}, {3.0, 1.0}, {4.0, 0.0}})));
  auto compound = factory_->createCompoundCurve(std::move(sections));
  EXPECT_TRUE(
      ConnectedElementLocationFilter::getLocations(compound.get()).empty());

  auto curve_poly =
      factory_->createCurvePolygon(factory_->createCircularString(MakeSequence(
          {{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}, {1.0, -1.0}, {0.0, 0.0}})));
  EXPECT_TRUE(
      ConnectedElementLocationFilter::getLocations(curve_poly.get()).empty());

  std::vector<std::unique_ptr<Curve>> curves;
  curves.push_back(
      factory_->createLineString(MakeSequence({{1.0, 2.0}, {3.0, 4.0}})));
  curves.push_back(factory_->createCircularString(
      MakeSequence({{3.0, 4.0}, {4.0, 5.0}, {5.0, 4.0}})));
  auto multi_curve = factory_->createMultiCurve(std::move(curves));
  auto mc_locs =
      ConnectedElementLocationFilter::getLocations(multi_curve.get());
  ASSERT_EQ(mc_locs.size(), 1u);
  ExpectLocation(mc_locs[0], multi_curve->getGeometryN(0),
                 CoordinateXY(1.0, 2.0));

  std::vector<std::unique_ptr<Surface>> surfaces;
  surfaces.push_back(factory_->createPolygon(factory_->createLinearRing(
      MakeSequence({{5.0, 5.0}, {8.0, 5.0}, {8.0, 8.0}, {5.0, 5.0}}))));
  surfaces.push_back(std::move(curve_poly));
  auto multi_surface = factory_->createMultiSurface(std::move(surfaces));
  auto ms_locs =
      ConnectedElementLocationFilter::getLocations(multi_surface.get());
  ASSERT_EQ(ms_locs.size(), 1u);
  ExpectLocation(ms_locs[0], multi_surface->getGeometryN(0),
                 CoordinateXY(5.0, 5.0));
}

TEST_F(ConnectedElementLocationFilterTest,
       FilterRwViaCustomCollectionCoversAllBranches) {
  std::vector<std::unique_ptr<Geometry>> empty_children;
  RwForwardingCollection empty_col(std::move(empty_children), *factory_);
  EXPECT_TRUE(ConnectedElementLocationFilter::getLocations(&empty_col).empty());

  std::vector<std::unique_ptr<Geometry>> children;
  children.push_back(factory_->createPoint());
  children.push_back(factory_->createLineString());
  children.push_back(factory_->createLinearRing());
  children.push_back(factory_->createPolygon());
  children.push_back(factory_->createPoint(CoordinateXY(11.0, 12.0)));
  children.push_back(
      factory_->createLineString(MakeSequence({{21.0, 22.0}, {23.0, 24.0}})));
  children.push_back(factory_->createLinearRing(
      MakeSequence({{31.0, 32.0}, {35.0, 32.0}, {35.0, 36.0}, {31.0, 32.0}})));
  children.push_back(factory_->createPolygon(factory_->createLinearRing(
      MakeSequence({{41.0, 42.0}, {45.0, 42.0}, {45.0, 46.0}, {41.0, 42.0}}))));

  std::vector<std::unique_ptr<Point>> mp_pts;
  mp_pts.push_back(factory_->createPoint(CoordinateXY(51.0, 52.0)));
  children.push_back(factory_->createMultiPoint(std::move(mp_pts)));

  RwForwardingCollection col(std::move(children), *factory_);
  auto locs = ConnectedElementLocationFilter::getLocations(&col);
  ASSERT_EQ(locs.size(), 4u);
  ExpectLocation(locs[0], col.getGeometryN(4), CoordinateXY(11.0, 12.0));
  ExpectLocation(locs[1], col.getGeometryN(5), CoordinateXY(21.0, 22.0));
  ExpectLocation(locs[2], col.getGeometryN(6), CoordinateXY(31.0, 32.0));
  ExpectLocation(locs[3], col.getGeometryN(7), CoordinateXY(41.0, 42.0));
}

}  // namespace
}  // namespace geos_google
