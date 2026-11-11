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

#include "third_party/geos/include/geos/geom/MultiSurface.h"

#include <cmath>
#include <initializer_list>
#include <memory>
#include <numbers>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
#include "third_party/geos/include/geos/geom/CompoundCurve.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Curve.h"
#include "third_party/geos/include/geos/geom/CurvePolygon.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiCurve.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/geom/SimpleCurve.h"
#include "third_party/geos/include/geos/geom/Surface.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

std::unique_ptr<geos::geom::CoordinateSequence> MakeXYSeq(
    std::initializer_list<geos::geom::CoordinateXY> coords) {
  auto seq = std::make_unique<geos::geom::CoordinateSequence>(0u, false, false);
  for (const auto& c : coords) {
    seq->add(c);
  }
  return seq;
}

std::unique_ptr<geos::geom::LinearRing> MakeRing(
    const geos::geom::GeometryFactory& factory,
    std::initializer_list<geos::geom::CoordinateXY> coords) {
  return factory.createLinearRing(MakeXYSeq(coords));
}

std::unique_ptr<geos::geom::Polygon> MakeBox(
    const geos::geom::GeometryFactory& factory, double minx, double miny,
    double maxx, double maxy) {
  return factory.createPolygon(MakeRing(
      factory,
      {{minx, miny}, {maxx, miny}, {maxx, maxy}, {minx, maxy}, {minx, miny}}));
}

std::unique_ptr<geos::geom::Polygon> MakeBoxWithHole(
    const geos::geom::GeometryFactory& factory, double minx, double miny,
    double maxx, double maxy, double hminx, double hminy, double hmaxx,
    double hmaxy) {
  auto shell = MakeRing(
      factory,
      {{minx, miny}, {maxx, miny}, {maxx, maxy}, {minx, maxy}, {minx, miny}});
  std::vector<std::unique_ptr<geos::geom::LinearRing>> holes;
  holes.push_back(MakeRing(factory, {{hminx, hminy},
                                     {hminx, hmaxy},
                                     {hmaxx, hmaxy},
                                     {hmaxx, hminy},
                                     {hminx, hminy}}));
  return factory.createPolygon(std::move(shell), std::move(holes));
}

// Circle of radius 1 centered at (cx, cy).
std::unique_ptr<geos::geom::CurvePolygon> MakeCircleCurvePolygon(
    const geos::geom::GeometryFactory& factory, double cx, double cy) {
  std::unique_ptr<geos::geom::Curve> shell =
      factory.createCircularString(MakeXYSeq({{cx - 1.0, cy},
                                              {cx, cy + 1.0},
                                              {cx + 1.0, cy},
                                              {cx, cy - 1.0},
                                              {cx - 1.0, cy}}));
  return factory.createCurvePolygon(std::move(shell));
}

// Upper unit semicircle bounded by arc (0,0)-(1,1)-(2,0) and line (2,0)-(0,0).
std::unique_ptr<geos::geom::CurvePolygon> MakeSemicircleCurvePolygon(
    const geos::geom::GeometryFactory& factory) {
  std::vector<std::unique_ptr<geos::geom::SimpleCurve>> sections;
  sections.push_back(factory.createCircularString(
      MakeXYSeq({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}})));
  sections.push_back(
      factory.createLineString(MakeXYSeq({{2.0, 0.0}, {0.0, 0.0}})));
  std::unique_ptr<geos::geom::Curve> shell =
      factory.createCompoundCurve(std::move(sections));
  return factory.createCurvePolygon(std::move(shell));
}

TEST(MultiSurfaceTest, CreateEmptyAndMetadata) {
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create();
  std::unique_ptr<geos::geom::MultiSurface> ms = factory->createMultiSurface();
  ASSERT_NE(ms, nullptr);

  EXPECT_TRUE(ms->isEmpty());
  EXPECT_EQ(ms->getNumGeometries(), 0u);
  EXPECT_EQ(ms->getNumPoints(), 0u);
  EXPECT_EQ(ms->getCoordinate(), nullptr);
  EXPECT_EQ(ms->getGeometryType(), "MultiSurface");
  EXPECT_EQ(ms->getGeometryTypeId(), geos::geom::GEOS_MULTISURFACE);
  EXPECT_EQ(ms->getDimension(), geos::geom::Dimension::A);
  EXPECT_EQ(ms->getBoundaryDimension(), 1);
  EXPECT_EQ(ms->getCoordinateDimension(), 2);
  EXPECT_FALSE(ms->hasZ());
  EXPECT_FALSE(ms->hasM());
  EXPECT_DOUBLE_EQ(ms->getArea(), 0.0);
  EXPECT_DOUBLE_EQ(ms->getLength(), 0.0);
  EXPECT_TRUE(ms->isPolygonal());
  EXPECT_FALSE(ms->isPuntal());
  EXPECT_FALSE(ms->isLineal());
  EXPECT_TRUE(ms->getEnvelopeInternal()->isNull());
  EXPECT_EQ(ms->toText(), "MULTISURFACE EMPTY");

  const geos::geom::Geometry* base = ms.get();
  EXPECT_FALSE(base->hasCurvedComponents());
}

TEST(MultiSurfaceTest, CreateFromSurfaceAndGeometryVectors) {
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create();

  std::vector<std::unique_ptr<geos::geom::Surface>> surfaces;
  surfaces.push_back(MakeBox(*factory, 0.0, 0.0, 10.0, 10.0));
  surfaces.push_back(MakeCircleCurvePolygon(*factory, 20.0, 0.0));
  std::unique_ptr<geos::geom::MultiSurface> ms_from_surfaces =
      factory->createMultiSurface(std::move(surfaces));

  ASSERT_EQ(ms_from_surfaces->getNumGeometries(), 2u);
  EXPECT_FALSE(ms_from_surfaces->isEmpty());
  EXPECT_NEAR(ms_from_surfaces->getArea(), 100.0 + std::numbers::pi, 1e-12);
  EXPECT_NEAR(ms_from_surfaces->getLength(), 40.0 + 2.0 * std::numbers::pi,
              1e-12);

  const geos::geom::Geometry* base = ms_from_surfaces.get();
  EXPECT_TRUE(base->hasCurvedComponents());

  std::vector<std::unique_ptr<geos::geom::Geometry>> geoms;
  geoms.push_back(MakeBox(*factory, 0.0, 0.0, 10.0, 10.0));
  geoms.push_back(MakeCircleCurvePolygon(*factory, 20.0, 0.0));
  std::unique_ptr<geos::geom::MultiSurface> ms_from_geoms =
      factory->createMultiSurface(std::move(geoms));
  EXPECT_TRUE(ms_from_geoms->equalsExact(ms_from_surfaces.get()));

  // Empty geometry vector creates empty MultiSurface.
  std::vector<std::unique_ptr<geos::geom::Geometry>> empty_geoms;
  auto empty_from_geoms = factory->createMultiSurface(std::move(empty_geoms));
  EXPECT_TRUE(empty_from_geoms->isEmpty());
  EXPECT_EQ(empty_from_geoms->getGeometryTypeId(),
            geos::geom::GEOS_MULTISURFACE);

  // Polymorphic destruction through Geometry pointer exercises ~MultiSurface().
  std::unique_ptr<geos::geom::Geometry> poly_owner = std::move(ms_from_geoms);
  EXPECT_EQ(poly_owner->getGeometryTypeId(), geos::geom::GEOS_MULTISURFACE);
  poly_owner.reset();
}

TEST(MultiSurfaceTest, NonSurfaceElementsThrowIllegalArgumentException) {
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create();

  // Non-surface elements in Geometry vector throw IllegalArgumentException.
  {
    std::vector<std::unique_ptr<geos::geom::Geometry>> bad;
    bad.push_back(factory->createPoint(geos::geom::CoordinateXY(1.0, 2.0)));
    EXPECT_THROW(factory->createMultiSurface(std::move(bad)),
                 geos::util::IllegalArgumentException);
  }
  {
    std::vector<std::unique_ptr<geos::geom::Geometry>> bad;
    bad.push_back(MakeBox(*factory, 0.0, 0.0, 2.0, 2.0));
    bad.push_back(
        factory->createLineString(MakeXYSeq({{0.0, 0.0}, {1.0, 1.0}})));
    EXPECT_THROW(factory->createMultiSurface(std::move(bad)),
                 geos::util::IllegalArgumentException);
  }
  {
    std::vector<std::unique_ptr<geos::geom::Geometry>> bad;
    bad.push_back(factory->createLineString());
    EXPECT_THROW(factory->createMultiSurface(std::move(bad)),
                 geos::util::IllegalArgumentException);
  }
  {
    std::vector<std::unique_ptr<geos::geom::Geometry>> bad;
    bad.push_back(
        MakeRing(*factory, {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 0.0}}));
    EXPECT_THROW(factory->createMultiSurface(std::move(bad)),
                 geos::util::IllegalArgumentException);
  }
  {
    std::vector<std::unique_ptr<geos::geom::Geometry>> bad;
    bad.push_back(factory->createCircularString(
        MakeXYSeq({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}})));
    EXPECT_THROW(factory->createMultiSurface(std::move(bad)),
                 geos::util::IllegalArgumentException);
  }
  {
    std::vector<std::unique_ptr<geos::geom::SimpleCurve>> sections;
    sections.push_back(
        factory->createLineString(MakeXYSeq({{0.0, 0.0}, {1.0, 0.0}})));
    std::vector<std::unique_ptr<geos::geom::Geometry>> bad;
    bad.push_back(factory->createCompoundCurve(std::move(sections)));
    EXPECT_THROW(factory->createMultiSurface(std::move(bad)),
                 geos::util::IllegalArgumentException);
  }
  {
    std::vector<std::unique_ptr<geos::geom::Geometry>> bad;
    bad.push_back(factory->createMultiPolygon());
    EXPECT_THROW(factory->createMultiSurface(std::move(bad)),
                 geos::util::IllegalArgumentException);
  }
  {
    std::vector<std::unique_ptr<geos::geom::Geometry>> bad;
    bad.push_back(factory->createMultiCurve());
    EXPECT_THROW(factory->createMultiSurface(std::move(bad)),
                 geos::util::IllegalArgumentException);
  }
  {
    std::vector<std::unique_ptr<geos::geom::Geometry>> bad;
    bad.push_back(factory->createMultiSurface());
    EXPECT_THROW(factory->createMultiSurface(std::move(bad)),
                 geos::util::IllegalArgumentException);
  }
  {
    std::vector<std::unique_ptr<geos::geom::Geometry>> bad;
    bad.push_back(factory->createGeometryCollection());
    EXPECT_THROW(factory->createMultiSurface(std::move(bad)),
                 geos::util::IllegalArgumentException);
  }
}

TEST(MultiSurfaceTest, DimensionPredicates) {
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create();

  auto empty = factory->createMultiSurface();
  std::vector<std::unique_ptr<geos::geom::Surface>> surfaces;
  surfaces.push_back(MakeBox(*factory, 0.0, 0.0, 1.0, 1.0));
  auto non_empty = factory->createMultiSurface(std::move(surfaces));

  for (const geos::geom::MultiSurface* ms : {empty.get(), non_empty.get()}) {
    EXPECT_EQ(ms->getDimension(), geos::geom::Dimension::A);
    EXPECT_EQ(ms->getBoundaryDimension(), 1);

    EXPECT_TRUE(ms->hasDimension(geos::geom::Dimension::A));
    EXPECT_FALSE(ms->hasDimension(geos::geom::Dimension::L));
    EXPECT_FALSE(ms->hasDimension(geos::geom::Dimension::P));
    EXPECT_FALSE(ms->hasDimension(geos::geom::Dimension::False));
    EXPECT_FALSE(ms->hasDimension(geos::geom::Dimension::True));
    EXPECT_FALSE(ms->hasDimension(geos::geom::Dimension::DONTCARE));

    EXPECT_TRUE(ms->isDimensionStrict(geos::geom::Dimension::A));
    EXPECT_FALSE(ms->isDimensionStrict(geos::geom::Dimension::L));
    EXPECT_FALSE(ms->isDimensionStrict(geos::geom::Dimension::P));
    EXPECT_FALSE(ms->isDimensionStrict(geos::geom::Dimension::False));
    EXPECT_FALSE(ms->isDimensionStrict(geos::geom::Dimension::True));
    EXPECT_FALSE(ms->isDimensionStrict(geos::geom::Dimension::DONTCARE));
  }
}

TEST(MultiSurfaceTest, BoundaryEmptyAndOnlyEmptySurfaces) {
  geos::geom::PrecisionModel pm;
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create(&pm, 4326);

  auto empty = factory->createMultiSurface();
  auto empty_boundary = empty->getBoundary();
  ASSERT_NE(empty_boundary, nullptr);
  EXPECT_EQ(empty_boundary->getGeometryTypeId(), geos::geom::GEOS_MULTICURVE);
  EXPECT_TRUE(empty_boundary->isEmpty());
  EXPECT_EQ(empty_boundary->getNumGeometries(), 0u);
  EXPECT_EQ(empty_boundary->getSRID(), 4326);

  std::vector<std::unique_ptr<geos::geom::Surface>> only_empty;
  only_empty.push_back(factory->createPolygon());
  only_empty.push_back(factory->createCurvePolygon(false, false));
  auto ms_only_empty = factory->createMultiSurface(std::move(only_empty));
  EXPECT_TRUE(ms_only_empty->isEmpty());
  auto bnd_only_empty = ms_only_empty->getBoundary();
  EXPECT_EQ(bnd_only_empty->getGeometryTypeId(), geos::geom::GEOS_MULTICURVE);
  EXPECT_TRUE(bnd_only_empty->isEmpty());
  EXPECT_EQ(bnd_only_empty->getNumGeometries(), 0u);
}

TEST(MultiSurfaceTest, BoundaryWithPolygonsAndCurvePolygonThrows) {
  geos::geom::PrecisionModel pm;
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create(&pm, 4326);

  // Polygons without holes hit the g->getNumGeometries() == 1 branch.
  std::vector<std::unique_ptr<geos::geom::Surface>> simple_polys;
  simple_polys.push_back(MakeBox(*factory, 0.0, 0.0, 2.0, 3.0));
  simple_polys.push_back(MakeBox(*factory, 5.0, 0.0, 6.0, 4.0));
  auto ms_simple = factory->createMultiSurface(std::move(simple_polys));

  auto bnd_simple = ms_simple->getBoundary();
  ASSERT_EQ(bnd_simple->getGeometryTypeId(), geos::geom::GEOS_MULTICURVE);
  EXPECT_EQ(bnd_simple->getSRID(), 4326);
  const auto* mc_simple =
      dynamic_cast<const geos::geom::MultiCurve*>(bnd_simple.get());
  ASSERT_NE(mc_simple, nullptr);
  EXPECT_EQ(mc_simple->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(mc_simple->getLength(), 20.0);

  // Mix of polygon with hole (2 rings), polygon without holes (1 ring), and
  // empty polygon (0 rings) exercises both branches and releaseGeometries().
  std::vector<std::unique_ptr<geos::geom::Surface>> mixed_polys;
  mixed_polys.push_back(
      MakeBoxWithHole(*factory, 0.0, 0.0, 10.0, 10.0, 2.0, 2.0, 4.0, 4.0));
  mixed_polys.push_back(MakeBox(*factory, 20.0, 0.0, 22.0, 3.0));
  mixed_polys.push_back(factory->createPolygon());
  auto ms_mixed = factory->createMultiSurface(std::move(mixed_polys));

  auto bnd_mixed = ms_mixed->getBoundary();
  ASSERT_EQ(bnd_mixed->getGeometryTypeId(), geos::geom::GEOS_MULTICURVE);
  ASSERT_EQ(bnd_mixed->getNumGeometries(), 3u);
  EXPECT_EQ(bnd_mixed->getSRID(), 4326);
  EXPECT_DOUBLE_EQ(bnd_mixed->getLength(), 40.0 + 8.0 + 10.0);

  // CurvePolygon::getBoundary() is unsupported and propagates out.
  std::vector<std::unique_ptr<geos::geom::Surface>> with_curve;
  with_curve.push_back(MakeBox(*factory, 0.0, 0.0, 1.0, 1.0));
  with_curve.push_back(MakeCircleCurvePolygon(*factory, 5.0, 0.0));
  auto ms_curved = factory->createMultiSurface(std::move(with_curve));
  EXPECT_THROW(ms_curved->getBoundary(),
               geos::util::UnsupportedOperationException);
}

TEST(MultiSurfaceTest, CloneAndReverseEmptyAndMixedSurfaces) {
  geos::geom::PrecisionModel pm;
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create(&pm, 3857);

  auto empty = factory->createMultiSurface();
  auto empty_rev = empty->reverse();
  ASSERT_NE(empty_rev, nullptr);
  EXPECT_NE(empty_rev.get(), empty.get());
  EXPECT_TRUE(empty_rev->isEmpty());
  EXPECT_EQ(empty_rev->getSRID(), 3857);
  EXPECT_EQ(empty_rev->getGeometryTypeId(), geos::geom::GEOS_MULTISURFACE);

  std::vector<std::unique_ptr<geos::geom::Surface>> only_empty;
  only_empty.push_back(factory->createPolygon());
  only_empty.push_back(factory->createCurvePolygon(false, false));
  auto ms_only_empty = factory->createMultiSurface(std::move(only_empty));
  auto only_empty_rev = ms_only_empty->reverse();
  ASSERT_NE(only_empty_rev, nullptr);
  EXPECT_NE(only_empty_rev.get(), ms_only_empty.get());
  EXPECT_TRUE(only_empty_rev->isEmpty());
  EXPECT_EQ(only_empty_rev->getNumGeometries(), 2u);
  EXPECT_EQ(only_empty_rev->getSRID(), 3857);

  std::vector<std::unique_ptr<geos::geom::Surface>> surfaces;
  surfaces.push_back(
      MakeBoxWithHole(*factory, 10.0, 0.0, 20.0, 10.0, 12.0, 2.0, 14.0, 4.0));
  surfaces.push_back(MakeSemicircleCurvePolygon(*factory));
  auto ms = factory->createMultiSurface(std::move(surfaces));

  auto cloned = ms->clone();
  ASSERT_NE(cloned, nullptr);
  EXPECT_NE(cloned.get(), ms.get());
  EXPECT_EQ(cloned->getSRID(), 3857);
  EXPECT_TRUE(cloned->equalsIdentical(ms.get()));

  auto reversed = ms->reverse();
  ASSERT_NE(reversed, nullptr);
  EXPECT_NE(reversed.get(), ms.get());
  EXPECT_EQ(reversed->getSRID(), 3857);
  EXPECT_EQ(reversed->getNumGeometries(), 2u);
  EXPECT_NEAR(reversed->getArea(), 96.0 + std::numbers::pi / 2.0, 1e-12);
  EXPECT_NEAR(reversed->getLength(), 48.0 + std::numbers::pi + 2.0, 1e-12);

  const auto* rev_poly =
      dynamic_cast<const geos::geom::Polygon*>(reversed->getGeometryN(0));
  ASSERT_NE(rev_poly, nullptr);
  EXPECT_EQ(rev_poly->getExteriorRing()->getCoordinateN(1),
            geos::geom::Coordinate(10.0, 10.0));
  EXPECT_EQ(rev_poly->getInteriorRingN(0)->getCoordinateN(1),
            geos::geom::Coordinate(14.0, 2.0));

  auto double_reversed = reversed->reverse();
  EXPECT_TRUE(double_reversed->equalsIdentical(ms.get()));

  const geos::geom::Geometry* base = ms.get();
  auto poly_clone = base->clone();
  EXPECT_EQ(poly_clone->getGeometryTypeId(), geos::geom::GEOS_MULTISURFACE);
  EXPECT_TRUE(poly_clone->equalsIdentical(ms.get()));
  auto poly_rev = base->reverse();
  EXPECT_EQ(poly_rev->getGeometryTypeId(), geos::geom::GEOS_MULTISURFACE);
  EXPECT_TRUE(poly_rev->equalsIdentical(reversed.get()));
}

TEST(MultiSurfaceTest, HasCurvedComponentsWktNormalizeAndCompare) {
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create();

  // MultiSurface of Polygons only has no curved components and can normalize.
  std::vector<std::unique_ptr<geos::geom::Surface>> poly_only;
  poly_only.push_back(MakeBox(*factory, 0.0, 0.0, 1.0, 1.0));
  poly_only.push_back(MakeBox(*factory, 5.0, 0.0, 6.0, 1.0));
  auto ms_polys = factory->createMultiSurface(std::move(poly_only));
  EXPECT_FALSE(static_cast<const geos::geom::Geometry*>(ms_polys.get())
                   ->hasCurvedComponents());

  std::vector<std::unique_ptr<geos::geom::Polygon>> mp_elems;
  mp_elems.push_back(MakeBox(*factory, 0.0, 0.0, 1.0, 1.0));
  mp_elems.push_back(MakeBox(*factory, 5.0, 0.0, 6.0, 1.0));
  auto multi_poly = factory->createMultiPolygon(std::move(mp_elems));
  EXPECT_FALSE(ms_polys->equalsExact(multi_poly.get()));
  EXPECT_LT(multi_poly->compareTo(ms_polys.get()), 0);
  EXPECT_GT(ms_polys->compareTo(multi_poly.get()), 0);

  ms_polys->normalize();
  EXPECT_DOUBLE_EQ(ms_polys->getGeometryN(0)->getEnvelopeInternal()->getMinX(),
                   5.0);

  // MultiSurface containing CurvePolygon reports curved components and formats
  // WKT with CURVEPOLYGON tags, while normalize throws.
  std::vector<std::unique_ptr<geos::geom::Surface>> mixed;
  mixed.push_back(MakeBox(*factory, 0.0, 0.0, 1.0, 1.0));
  mixed.push_back(MakeCircleCurvePolygon(*factory, 10.0, 0.0));
  auto ms_mixed = factory->createMultiSurface(std::move(mixed));
  EXPECT_TRUE(static_cast<const geos::geom::Geometry*>(ms_mixed.get())
                  ->hasCurvedComponents());
  EXPECT_EQ(ms_mixed->toText(),
            "MULTISURFACE (((0 0, 1 0, 1 1, 0 1, 0 0)), "
            "CURVEPOLYGON (CIRCULARSTRING (9 0, 10 1, 11 0, 10 -1, 9 0)))");
  EXPECT_THROW(ms_mixed->normalize(),
               geos::util::UnsupportedOperationException);

  const geos::geom::Envelope* env = ms_mixed->getEnvelopeInternal();
  EXPECT_DOUBLE_EQ(env->getMinX(), 0.0);
  EXPECT_DOUBLE_EQ(env->getMaxX(), 11.0);
  EXPECT_DOUBLE_EQ(env->getMinY(), -1.0);
  EXPECT_DOUBLE_EQ(env->getMaxY(), 1.0);

  // Sort index check against CurvePolygon and empty MultiSurface.
  auto empty_ms = factory->createMultiSurface();
  auto single_cp = MakeCircleCurvePolygon(*factory, 0.0, 0.0);
  EXPECT_LT(single_cp->compareTo(ms_mixed.get()), 0);
  EXPECT_GT(ms_mixed->compareTo(single_cp.get()), 0);
  EXPECT_LT(empty_ms->compareTo(ms_mixed.get()), 0);
  EXPECT_GT(ms_mixed->compareTo(empty_ms.get()), 0);
  EXPECT_EQ(empty_ms->compareTo(empty_ms.get()), 0);
}

TEST(MultiSurfaceTest, CoordinateDimensionsZAndM) {
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create();

  auto z_seq =
      std::make_unique<geos::geom::CoordinateSequence>(0u, true, false);
  z_seq->add(geos::geom::Coordinate(0.0, 0.0, 3.0));
  z_seq->add(geos::geom::Coordinate(2.0, 0.0, 3.0));
  z_seq->add(geos::geom::Coordinate(2.0, 2.0, 3.0));
  z_seq->add(geos::geom::Coordinate(0.0, 2.0, 3.0));
  z_seq->add(geos::geom::Coordinate(0.0, 0.0, 3.0));

  std::vector<std::unique_ptr<geos::geom::Surface>> z_surfaces;
  z_surfaces.push_back(
      factory->createPolygon(factory->createLinearRing(std::move(z_seq))));
  auto ms_z = factory->createMultiSurface(std::move(z_surfaces));
  EXPECT_EQ(ms_z->getCoordinateDimension(), 3);
  EXPECT_TRUE(ms_z->hasZ());
  EXPECT_FALSE(ms_z->hasM());

  auto m_seq =
      std::make_unique<geos::geom::CoordinateSequence>(0u, false, true);
  m_seq->add(geos::geom::CoordinateXYM(0.0, 0.0, 7.0));
  m_seq->add(geos::geom::CoordinateXYM(2.0, 0.0, 7.0));
  m_seq->add(geos::geom::CoordinateXYM(2.0, 2.0, 7.0));
  m_seq->add(geos::geom::CoordinateXYM(0.0, 2.0, 7.0));
  m_seq->add(geos::geom::CoordinateXYM(0.0, 0.0, 7.0));

  std::vector<std::unique_ptr<geos::geom::Surface>> m_surfaces;
  m_surfaces.push_back(
      factory->createPolygon(factory->createLinearRing(std::move(m_seq))));
  auto ms_m = factory->createMultiSurface(std::move(m_surfaces));
  EXPECT_EQ(ms_m->getCoordinateDimension(), 3);
  EXPECT_FALSE(ms_m->hasZ());
  EXPECT_TRUE(ms_m->hasM());
  EXPECT_TRUE(ms_z->equalsExact(ms_m.get()));
  EXPECT_FALSE(ms_z->equalsIdentical(ms_m.get()));

  const auto* poly_m =
      dynamic_cast<const geos::geom::Polygon*>(ms_m->getGeometryN(0));
  ASSERT_NE(poly_m, nullptr);
  geos::geom::CoordinateXYZM coord;
  poly_m->getExteriorRing()->getCoordinatesRO()->getAt(0, coord);
  EXPECT_TRUE(std::isnan(coord.z));
  EXPECT_DOUBLE_EQ(coord.m, 7.0);

  auto zm_seq =
      std::make_unique<geos::geom::CoordinateSequence>(0u, true, true);
  zm_seq->add(geos::geom::CoordinateXYZM(0.0, 0.0, 3.0, 7.0));
  zm_seq->add(geos::geom::CoordinateXYZM(2.0, 0.0, 3.0, 7.0));
  zm_seq->add(geos::geom::CoordinateXYZM(2.0, 2.0, 3.0, 7.0));
  zm_seq->add(geos::geom::CoordinateXYZM(0.0, 2.0, 3.0, 7.0));
  zm_seq->add(geos::geom::CoordinateXYZM(0.0, 0.0, 3.0, 7.0));

  std::vector<std::unique_ptr<geos::geom::Surface>> zm_surfaces;
  zm_surfaces.push_back(
      factory->createPolygon(factory->createLinearRing(std::move(zm_seq))));
  auto ms_zm = factory->createMultiSurface(std::move(zm_surfaces));
  EXPECT_EQ(ms_zm->getCoordinateDimension(), 4);
  EXPECT_TRUE(ms_zm->hasZ());
  EXPECT_TRUE(ms_zm->hasM());

  auto empty_geom = factory->createEmptyGeometry(geos::geom::GEOS_MULTISURFACE);
  EXPECT_EQ(empty_geom->getGeometryTypeId(), geos::geom::GEOS_MULTISURFACE);
  EXPECT_TRUE(empty_geom->isEmpty());
}

}  // namespace
}  // namespace geos_google
