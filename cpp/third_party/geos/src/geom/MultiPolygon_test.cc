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

#include "third_party/geos/include/geos/geom/MultiPolygon.h"

#include <cmath>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"

namespace geos_google {
namespace {

std::unique_ptr<geos::geom::LinearRing> MakeRing(
    const geos::geom::GeometryFactory& factory,
    std::initializer_list<geos::geom::CoordinateXY> coords) {
  auto seq = std::make_unique<geos::geom::CoordinateSequence>(0u, false, false);
  for (const auto& c : coords) {
    seq->add(c);
  }
  return factory.createLinearRing(std::move(seq));
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

TEST(MultiPolygonTest, CreateEmptyAndMetadata) {
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create();
  std::unique_ptr<geos::geom::MultiPolygon> mp = factory->createMultiPolygon();
  ASSERT_NE(mp, nullptr);

  EXPECT_TRUE(mp->isEmpty());
  EXPECT_EQ(mp->getNumGeometries(), 0u);
  EXPECT_EQ(mp->getNumPoints(), 0u);
  EXPECT_EQ(mp->getCoordinate(), nullptr);
  EXPECT_EQ(mp->getGeometryType(), "MultiPolygon");
  EXPECT_EQ(mp->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(mp->getDimension(), geos::geom::Dimension::A);
  EXPECT_EQ(mp->getBoundaryDimension(), 1);
  EXPECT_EQ(mp->getCoordinateDimension(), 2);
  EXPECT_FALSE(mp->hasZ());
  EXPECT_FALSE(mp->hasM());
  EXPECT_DOUBLE_EQ(mp->getArea(), 0.0);
  EXPECT_DOUBLE_EQ(mp->getLength(), 0.0);
  EXPECT_TRUE(mp->isPolygonal());
  EXPECT_FALSE(mp->isPuntal());
  EXPECT_FALSE(mp->isLineal());
  EXPECT_TRUE(mp->isCollection());
  EXPECT_TRUE(mp->isValid());
  EXPECT_TRUE(mp->getEnvelopeInternal()->isNull());
  EXPECT_EQ(mp->toText(), "MULTIPOLYGON EMPTY");

  const geos::geom::Geometry* base = mp.get();
  EXPECT_FALSE(base->hasCurvedComponents());
}

TEST(MultiPolygonTest, CreateFromPolygonGeometryAndPointerVectors) {
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create();

  std::vector<std::unique_ptr<geos::geom::Polygon>> polys;
  polys.push_back(MakeBox(*factory, 0.0, 0.0, 4.0, 5.0));
  polys.push_back(MakeBox(*factory, 10.0, 0.0, 12.0, 3.0));
  std::unique_ptr<geos::geom::MultiPolygon> from_polys =
      factory->createMultiPolygon(std::move(polys));
  ASSERT_EQ(from_polys->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(from_polys->getArea(), 26.0);
  EXPECT_DOUBLE_EQ(from_polys->getLength(), 28.0);
  EXPECT_EQ(from_polys->getNumPoints(), 10u);

  const geos::geom::Polygon* p0 = from_polys->getGeometryN(0);
  const geos::geom::Polygon* p1 = from_polys->getGeometryN(1);
  ASSERT_NE(p0, nullptr);
  ASSERT_NE(p1, nullptr);
  EXPECT_DOUBLE_EQ(p0->getArea(), 20.0);
  EXPECT_DOUBLE_EQ(p1->getArea(), 6.0);

  std::vector<std::unique_ptr<geos::geom::Geometry>> geoms;
  geoms.push_back(MakeBox(*factory, 0.0, 0.0, 4.0, 5.0));
  geoms.push_back(MakeBox(*factory, 10.0, 0.0, 12.0, 3.0));
  std::unique_ptr<geos::geom::MultiPolygon> from_geoms =
      factory->createMultiPolygon(std::move(geoms));
  EXPECT_TRUE(from_geoms->equalsExact(from_polys.get()));

  auto box0 = MakeBox(*factory, 0.0, 0.0, 4.0, 5.0);
  auto box1 = MakeBox(*factory, 10.0, 0.0, 12.0, 3.0);
  std::vector<const geos::geom::Geometry*> ptrs = {box0.get(), box1.get()};
  std::unique_ptr<geos::geom::MultiPolygon> copied =
      factory->createMultiPolygon(ptrs);
  box0.reset();
  box1.reset();
  EXPECT_TRUE(copied->equalsExact(from_polys.get()));

  // Destroy polymorphically through Geometry base pointer.
  std::unique_ptr<geos::geom::Geometry> base_owner = std::move(copied);
  EXPECT_EQ(base_owner->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  base_owner.reset();
}

TEST(MultiPolygonTest, EmptyVectorsCreateEmptyMultiPolygons) {
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create();

  std::vector<std::unique_ptr<geos::geom::Polygon>> empty_polys;
  auto mp_polys = factory->createMultiPolygon(std::move(empty_polys));
  EXPECT_TRUE(mp_polys->isEmpty());
  EXPECT_EQ(mp_polys->getNumGeometries(), 0u);
  EXPECT_FALSE(mp_polys->isRectangle());

  std::vector<std::unique_ptr<geos::geom::Geometry>> empty_geoms;
  auto mp_geoms = factory->createMultiPolygon(std::move(empty_geoms));
  EXPECT_TRUE(mp_geoms->isEmpty());
  EXPECT_EQ(mp_geoms->getNumGeometries(), 0u);

  std::vector<const geos::geom::Geometry*> empty_ptrs;
  auto mp_ptrs = factory->createMultiPolygon(empty_ptrs);
  EXPECT_TRUE(mp_ptrs->isEmpty());
  EXPECT_EQ(mp_ptrs->getNumGeometries(), 0u);
  EXPECT_TRUE(mp_polys->equalsExact(mp_geoms.get()));
  EXPECT_TRUE(mp_polys->equalsExact(mp_ptrs.get()));
}

TEST(MultiPolygonTest, DimensionPredicates) {
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create();

  std::unique_ptr<geos::geom::MultiPolygon> empty =
      factory->createMultiPolygon();
  std::vector<std::unique_ptr<geos::geom::Polygon>> polys;
  polys.push_back(MakeBox(*factory, 0.0, 0.0, 2.0, 2.0));
  std::unique_ptr<geos::geom::MultiPolygon> non_empty =
      factory->createMultiPolygon(std::move(polys));

  for (const geos::geom::MultiPolygon* mp : {empty.get(), non_empty.get()}) {
    EXPECT_EQ(mp->getDimension(), geos::geom::Dimension::A);
    EXPECT_EQ(mp->getBoundaryDimension(), 1);

    EXPECT_TRUE(mp->hasDimension(geos::geom::Dimension::A));
    EXPECT_FALSE(mp->hasDimension(geos::geom::Dimension::L));
    EXPECT_FALSE(mp->hasDimension(geos::geom::Dimension::P));
    EXPECT_FALSE(mp->hasDimension(geos::geom::Dimension::False));
    EXPECT_FALSE(mp->hasDimension(geos::geom::Dimension::DONTCARE));

    EXPECT_TRUE(mp->isDimensionStrict(geos::geom::Dimension::A));
    EXPECT_FALSE(mp->isDimensionStrict(geos::geom::Dimension::L));
    EXPECT_FALSE(mp->isDimensionStrict(geos::geom::Dimension::P));
    EXPECT_FALSE(mp->isDimensionStrict(geos::geom::Dimension::False));
  }
}

TEST(MultiPolygonTest, BoundaryEmptyAndOnlyEmptyPolygons) {
  geos::geom::PrecisionModel pm;
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create(&pm, 4326);

  std::unique_ptr<geos::geom::MultiPolygon> empty =
      factory->createMultiPolygon();
  std::unique_ptr<geos::geom::Geometry> empty_boundary = empty->getBoundary();
  ASSERT_NE(empty_boundary, nullptr);
  EXPECT_EQ(empty_boundary->getGeometryTypeId(),
            geos::geom::GEOS_MULTILINESTRING);
  EXPECT_TRUE(empty_boundary->isEmpty());
  EXPECT_EQ(empty_boundary->getNumGeometries(), 0u);
  EXPECT_EQ(empty_boundary->getSRID(), 4326);

  std::vector<std::unique_ptr<geos::geom::Polygon>> only_empty_polys;
  only_empty_polys.push_back(factory->createPolygon());
  only_empty_polys.push_back(factory->createPolygon());
  std::unique_ptr<geos::geom::MultiPolygon> mp_empty_elems =
      factory->createMultiPolygon(std::move(only_empty_polys));
  EXPECT_TRUE(mp_empty_elems->isEmpty());
  EXPECT_EQ(mp_empty_elems->getNumGeometries(), 2u);
  std::unique_ptr<geos::geom::Geometry> elem_empty_boundary =
      mp_empty_elems->getBoundary();
  EXPECT_EQ(elem_empty_boundary->getGeometryTypeId(),
            geos::geom::GEOS_MULTILINESTRING);
  EXPECT_TRUE(elem_empty_boundary->isEmpty());
  EXPECT_EQ(elem_empty_boundary->getNumGeometries(), 0u);
}

TEST(MultiPolygonTest, BoundaryWithoutHolesWithHolesAndMixed) {
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create();

  // Polygons without holes: each Polygon::getBoundary() has 1 geometry.
  std::vector<std::unique_ptr<geos::geom::Polygon>> simple_polys;
  simple_polys.push_back(MakeBox(*factory, 0.0, 0.0, 1.0, 1.0));
  simple_polys.push_back(MakeBox(*factory, 5.0, 0.0, 7.0, 2.0));
  auto mp_simple = factory->createMultiPolygon(std::move(simple_polys));

  auto bnd_simple = mp_simple->getBoundary();
  ASSERT_EQ(bnd_simple->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  ASSERT_EQ(bnd_simple->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(bnd_simple->getLength(), 12.0);

  // Mix of: polygon with 2 holes (3 rings), polygon without holes (1 ring),
  // and an empty polygon (0 rings).
  auto shell = MakeRing(
      *factory,
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});
  std::vector<std::unique_ptr<geos::geom::LinearRing>> holes;
  holes.push_back(MakeRing(
      *factory, {{1.0, 1.0}, {1.0, 2.0}, {2.0, 2.0}, {2.0, 1.0}, {1.0, 1.0}}));
  holes.push_back(MakeRing(
      *factory, {{5.0, 5.0}, {5.0, 7.0}, {7.0, 7.0}, {7.0, 5.0}, {5.0, 5.0}}));

  std::vector<std::unique_ptr<geos::geom::Polygon>> mixed;
  mixed.push_back(factory->createPolygon(std::move(shell), std::move(holes)));
  mixed.push_back(MakeBox(*factory, 20.0, 0.0, 23.0, 4.0));
  mixed.push_back(factory->createPolygon());
  auto mp_mixed = factory->createMultiPolygon(std::move(mixed));

  EXPECT_DOUBLE_EQ(mp_mixed->getArea(), 100.0 - 1.0 - 4.0 + 12.0);
  auto bnd_mixed = mp_mixed->getBoundary();
  ASSERT_EQ(bnd_mixed->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  ASSERT_EQ(bnd_mixed->getNumGeometries(), 4u);
  EXPECT_DOUBLE_EQ(bnd_mixed->getLength(), 40.0 + 4.0 + 8.0 + 14.0);
}

TEST(MultiPolygonTest, CloneAndReverseEmptyAndNonEmpty) {
  geos::geom::PrecisionModel pm;
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create(&pm, 3857);

  auto empty = factory->createMultiPolygon();
  auto empty_rev = empty->reverse();
  ASSERT_NE(empty_rev, nullptr);
  EXPECT_NE(empty_rev.get(), empty.get());
  EXPECT_TRUE(empty_rev->isEmpty());
  EXPECT_EQ(empty_rev->getSRID(), 3857);
  EXPECT_EQ(empty_rev->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);

  std::vector<std::unique_ptr<geos::geom::Polygon>> polys;
  polys.push_back(
      MakeBoxWithHole(*factory, 0.0, 0.0, 10.0, 10.0, 2.0, 2.0, 4.0, 4.0));
  polys.push_back(MakeBox(*factory, 20.0, 0.0, 22.0, 3.0));
  auto mp = factory->createMultiPolygon(std::move(polys));

  auto cloned = mp->clone();
  ASSERT_NE(cloned, nullptr);
  EXPECT_NE(cloned.get(), mp.get());
  EXPECT_EQ(cloned->getSRID(), 3857);
  EXPECT_TRUE(cloned->equalsIdentical(mp.get()));

  auto reversed = mp->reverse();
  ASSERT_NE(reversed, nullptr);
  EXPECT_NE(reversed.get(), mp.get());
  EXPECT_EQ(reversed->getSRID(), 3857);
  EXPECT_EQ(reversed->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(reversed->getArea(), mp->getArea());
  EXPECT_DOUBLE_EQ(reversed->getLength(), mp->getLength());

  // Component order is preserved while each polygon's rings are reversed.
  const geos::geom::Polygon* rev0 = reversed->getGeometryN(0);
  const geos::geom::Polygon* rev1 = reversed->getGeometryN(1);
  ASSERT_EQ(rev0->getNumInteriorRing(), 1u);
  EXPECT_DOUBLE_EQ(rev0->getArea(), 96.0);
  EXPECT_DOUBLE_EQ(rev1->getArea(), 6.0);
  EXPECT_EQ(rev0->getExteriorRing()->getCoordinateN(1),
            geos::geom::Coordinate(0.0, 10.0));
  EXPECT_EQ(rev0->getInteriorRingN(0)->getCoordinateN(1),
            geos::geom::Coordinate(4.0, 2.0));

  auto double_reversed = reversed->reverse();
  EXPECT_TRUE(double_reversed->equalsIdentical(mp.get()));

  const geos::geom::Geometry* base = mp.get();
  auto poly_rev = base->reverse();
  EXPECT_EQ(poly_rev->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_TRUE(poly_rev->equalsIdentical(reversed.get()));
}

TEST(MultiPolygonTest, ValidityNormalizeCompareAndCoordinates) {
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create();

  // Disjoint polygons are valid.
  std::vector<std::unique_ptr<geos::geom::Polygon>> valid_polys;
  valid_polys.push_back(MakeBox(*factory, 0.0, 0.0, 2.0, 2.0));
  valid_polys.push_back(MakeBox(*factory, 5.0, 0.0, 7.0, 2.0));
  auto mp_valid = factory->createMultiPolygon(std::move(valid_polys));
  EXPECT_TRUE(mp_valid->isValid());

  // Overlapping polygons violate OGC MultiPolygon rules.
  std::vector<std::unique_ptr<geos::geom::Polygon>> overlap_polys;
  overlap_polys.push_back(MakeBox(*factory, 0.0, 0.0, 3.0, 3.0));
  overlap_polys.push_back(MakeBox(*factory, 2.0, 0.0, 5.0, 3.0));
  auto mp_overlap = factory->createMultiPolygon(std::move(overlap_polys));
  EXPECT_FALSE(mp_overlap->isValid());

  // Envelope and centroid.
  const geos::geom::Envelope* env = mp_valid->getEnvelopeInternal();
  EXPECT_DOUBLE_EQ(env->getMinX(), 0.0);
  EXPECT_DOUBLE_EQ(env->getMaxX(), 7.0);
  EXPECT_DOUBLE_EQ(env->getMinY(), 0.0);
  EXPECT_DOUBLE_EQ(env->getMaxY(), 2.0);

  auto centroid = mp_valid->getCentroid();
  EXPECT_DOUBLE_EQ(centroid->getX(), 3.5);
  EXPECT_DOUBLE_EQ(centroid->getY(), 1.0);

  // Normalize reorders components deterministically.
  std::vector<std::unique_ptr<geos::geom::Polygon>> reversed_order;
  reversed_order.push_back(MakeBox(*factory, 5.0, 0.0, 7.0, 2.0));
  reversed_order.push_back(MakeBox(*factory, 0.0, 0.0, 2.0, 2.0));
  auto mp_other_order = factory->createMultiPolygon(std::move(reversed_order));
  EXPECT_FALSE(mp_other_order->equalsExact(mp_valid.get()));

  mp_valid->normalize();
  mp_other_order->normalize();
  EXPECT_TRUE(mp_other_order->equalsExact(mp_valid.get()));

  // Sort index ordering: Polygon < MultiPolygon < GeometryCollection.
  auto single_poly = MakeBox(*factory, 0.0, 0.0, 2.0, 2.0);
  std::vector<std::unique_ptr<geos::geom::Geometry>> gc_elems;
  gc_elems.push_back(MakeBox(*factory, 0.0, 0.0, 2.0, 2.0));
  auto gc = factory->createGeometryCollection(std::move(gc_elems));
  EXPECT_LT(single_poly->compareTo(mp_valid.get()), 0);
  EXPECT_GT(mp_valid->compareTo(single_poly.get()), 0);
  EXPECT_LT(mp_valid->compareTo(gc.get()), 0);
  EXPECT_GT(gc->compareTo(mp_valid.get()), 0);
  EXPECT_FALSE(mp_valid->equalsExact(gc.get()));
}

TEST(MultiPolygonTest, CoordinateDimensionsZAndM) {
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create();

  auto z_seq =
      std::make_unique<geos::geom::CoordinateSequence>(0u, true, false);
  z_seq->add(geos::geom::Coordinate(0.0, 0.0, 10.0));
  z_seq->add(geos::geom::Coordinate(2.0, 0.0, 10.0));
  z_seq->add(geos::geom::Coordinate(2.0, 2.0, 10.0));
  z_seq->add(geos::geom::Coordinate(0.0, 2.0, 10.0));
  z_seq->add(geos::geom::Coordinate(0.0, 0.0, 10.0));

  std::vector<std::unique_ptr<geos::geom::Polygon>> polys_z;
  polys_z.push_back(
      factory->createPolygon(factory->createLinearRing(std::move(z_seq))));
  auto mp_z = factory->createMultiPolygon(std::move(polys_z));

  EXPECT_EQ(mp_z->getCoordinateDimension(), 3);
  EXPECT_TRUE(mp_z->hasZ());
  EXPECT_FALSE(mp_z->hasM());

  auto m_seq =
      std::make_unique<geos::geom::CoordinateSequence>(0u, false, true);
  m_seq->add(geos::geom::CoordinateXYM(0.0, 0.0, 5.0));
  m_seq->add(geos::geom::CoordinateXYM(2.0, 0.0, 5.0));
  m_seq->add(geos::geom::CoordinateXYM(2.0, 2.0, 5.0));
  m_seq->add(geos::geom::CoordinateXYM(0.0, 2.0, 5.0));
  m_seq->add(geos::geom::CoordinateXYM(0.0, 0.0, 5.0));

  std::vector<std::unique_ptr<geos::geom::Polygon>> polys_m;
  polys_m.push_back(
      factory->createPolygon(factory->createLinearRing(std::move(m_seq))));
  auto mp_m = factory->createMultiPolygon(std::move(polys_m));

  EXPECT_EQ(mp_m->getCoordinateDimension(), 3);
  EXPECT_FALSE(mp_m->hasZ());
  EXPECT_TRUE(mp_m->hasM());
  EXPECT_TRUE(mp_z->equalsExact(mp_m.get()));
  EXPECT_FALSE(mp_z->equalsIdentical(mp_m.get()));
  geos::geom::CoordinateXYZM coord;
  mp_m->getGeometryN(0)->getExteriorRing()->getCoordinatesRO()->getAt(0, coord);
  EXPECT_TRUE(std::isnan(coord.z));
  EXPECT_DOUBLE_EQ(coord.m, 5.0);
}

TEST(MultiPolygonTest, FactoryEmptyBuildAndPredicates) {
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create();

  auto empty_geom = factory->createEmptyGeometry(geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(empty_geom->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_TRUE(empty_geom->isEmpty());

  std::vector<std::unique_ptr<geos::geom::Polygon>> polys;
  polys.push_back(MakeBox(*factory, 0.0, 0.0, 2.0, 2.0));
  polys.push_back(MakeBox(*factory, 4.0, 0.0, 6.0, 2.0));
  auto built = factory->buildGeometry(std::move(polys));
  EXPECT_EQ(built->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(built->getNumGeometries(), 2u);

  auto inside_pt = factory->createPoint(geos::geom::CoordinateXY(5.0, 1.0));
  auto outside_pt = factory->createPoint(geos::geom::CoordinateXY(3.0, 1.0));
  EXPECT_TRUE(built->contains(inside_pt.get()));
  EXPECT_FALSE(built->contains(outside_pt.get()));
  EXPECT_TRUE(built->disjoint(outside_pt.get()));

  auto single_poly = MakeBox(*factory, 0.0, 0.0, 1.0, 1.0);
  auto multi = factory->createMulti(std::move(single_poly));
  EXPECT_EQ(multi->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(multi->getNumGeometries(), 1u);
}

TEST(MultiPolygonTest, VirtualDispatchCloneReverseBoundaryAndCoordinates) {
  geos::geom::PrecisionModel pm;
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create(&pm, 4326);

  // Empty MultiPolygon clone/coordinates/virtual dispatch.
  auto empty_mp = factory->createMultiPolygon();
  auto empty_clone = empty_mp->clone();
  EXPECT_TRUE(empty_clone->isEmpty());
  EXPECT_EQ(empty_clone->getSRID(), 4326);
  EXPECT_EQ(empty_mp->getCoordinates()->getSize(), 0u);

  const geos::geom::Geometry* empty_base = empty_mp.get();
  auto empty_base_clone = empty_base->clone();
  EXPECT_EQ(empty_base_clone->getGeometryTypeId(),
            geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_TRUE(empty_base_clone->isEmpty());

  // MultiPolygon containing only empty polygons: isEmpty() is true, so
  // reverse() takes the empty-clone path while keeping component count.
  std::vector<std::unique_ptr<geos::geom::Polygon>> only_empty;
  only_empty.push_back(factory->createPolygon());
  only_empty.push_back(factory->createPolygon());
  auto mp_only_empty = factory->createMultiPolygon(std::move(only_empty));
  auto rev_only_empty = mp_only_empty->reverse();
  EXPECT_TRUE(rev_only_empty->isEmpty());
  EXPECT_EQ(rev_only_empty->getNumGeometries(), 2u);
  EXPECT_EQ(rev_only_empty->getSRID(), 4326);

  // Mixed non-empty (with hole) and empty polygon: isEmpty() is false, so
  // reverse() transforms each element.
  std::vector<std::unique_ptr<geos::geom::Polygon>> mixed;
  mixed.push_back(
      MakeBoxWithHole(*factory, 0.0, 0.0, 6.0, 6.0, 1.0, 1.0, 2.0, 2.0));
  mixed.push_back(factory->createPolygon());
  auto mp_mixed = factory->createMultiPolygon(std::move(mixed));
  auto rev_mixed = mp_mixed->reverse();
  ASSERT_EQ(rev_mixed->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(rev_mixed->getArea(), 35.0);
  EXPECT_TRUE(rev_mixed->getGeometryN(1)->isEmpty());

  // Virtual dispatch through Geometry* for all overridden methods.
  const geos::geom::Geometry* base = mp_mixed.get();
  EXPECT_EQ(base->getDimension(), geos::geom::Dimension::A);
  EXPECT_EQ(base->getBoundaryDimension(), 1);
  EXPECT_EQ(base->getGeometryType(), "MultiPolygon");
  EXPECT_EQ(base->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_TRUE(base->hasDimension(geos::geom::Dimension::A));
  EXPECT_FALSE(base->hasDimension(geos::geom::Dimension::P));
  EXPECT_TRUE(base->isDimensionStrict(geos::geom::Dimension::A));
  EXPECT_FALSE(base->isDimensionStrict(geos::geom::Dimension::L));
  EXPECT_FALSE(base->hasCurvedComponents());
  EXPECT_FALSE(base->isRectangle());
  EXPECT_EQ(base->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_POLYGON);
  EXPECT_EQ(base->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_POLYGON);

  auto base_bnd = base->getBoundary();
  EXPECT_EQ(base_bnd->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(base_bnd->getNumGeometries(), 2u);

  auto base_clone = base->clone();
  EXPECT_EQ(base_clone->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_TRUE(base_clone->equalsIdentical(mp_mixed.get()));

  auto base_coords = base->getCoordinates();
  EXPECT_EQ(base_coords->getSize(), 10u);

  // Z-coordinate preservation through getBoundary() and reverse().
  auto z_seq =
      std::make_unique<geos::geom::CoordinateSequence>(0u, true, false);
  z_seq->add(geos::geom::Coordinate(0.0, 0.0, 7.0));
  z_seq->add(geos::geom::Coordinate(3.0, 0.0, 7.0));
  z_seq->add(geos::geom::Coordinate(3.0, 3.0, 7.0));
  z_seq->add(geos::geom::Coordinate(0.0, 3.0, 7.0));
  z_seq->add(geos::geom::Coordinate(0.0, 0.0, 7.0));
  std::vector<std::unique_ptr<geos::geom::Polygon>> polys_z;
  polys_z.push_back(
      factory->createPolygon(factory->createLinearRing(std::move(z_seq))));
  auto mp_z = factory->createMultiPolygon(std::move(polys_z));
  auto z_bnd = mp_z->getBoundary();
  EXPECT_TRUE(z_bnd->hasZ());
  EXPECT_EQ(z_bnd->getNumGeometries(), 1u);
  auto z_rev = mp_z->reverse();
  EXPECT_TRUE(z_rev->hasZ());
  EXPECT_DOUBLE_EQ(z_rev->getArea(), 9.0);

  // Release geometries transfers ownership cleanly.
  auto released = mp_mixed->releaseGeometries();
  EXPECT_EQ(released.size(), 2u);
  EXPECT_EQ(released[0]->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
}

}  // namespace
}  // namespace geos_google
