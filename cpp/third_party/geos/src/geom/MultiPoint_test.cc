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

#include "third_party/geos/include/geos/geom/MultiPoint.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateFilter.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/GeometryFilter.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"

namespace geos_google {
namespace {

class TestableMultiPoint : public geos::geom::MultiPoint {
 public:
  TestableMultiPoint(std::vector<std::unique_ptr<geos::geom::Point>>&& points,
                     const geos::geom::GeometryFactory& factory)
      : MultiPoint(std::move(points), factory) {}

  TestableMultiPoint(
      std::vector<std::unique_ptr<geos::geom::Geometry>>&& points,
      const geos::geom::GeometryFactory& factory)
      : MultiPoint(std::move(points), factory) {}

  TestableMultiPoint(const TestableMultiPoint& other) = default;

  const geos::geom::CoordinateXY* CoordinateAt(std::size_t n) const {
    return getCoordinateN(n);
  }

  int SortIndex() const { return getSortIndex(); }

  bool CurvedComponents() const { return hasCurvedComponents(); }
};

class CountingGeometryFilter : public geos::geom::GeometryFilter {
 public:
  void filter_ro(const geos::geom::Geometry* geom) override {
    types.push_back(geom->getGeometryType());
  }

  std::vector<std::string> types;
};

class ShiftXFilter : public geos::geom::CoordinateFilter {
 public:
  explicit ShiftXFilter(double dx) : dx_(dx) {}

  void filter_rw(geos::geom::CoordinateXY* coord) const override {
    coord->x += dx_;
  }

 private:
  double dx_;
};

TEST(MultiPointTest, CreateEmpty) {
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create();
  std::unique_ptr<geos::geom::MultiPoint> mp = factory->createMultiPoint();
  ASSERT_NE(mp, nullptr);

  EXPECT_TRUE(mp->isEmpty());
  EXPECT_EQ(mp->getNumGeometries(), 0u);
  EXPECT_EQ(mp->getNumPoints(), 0u);
  EXPECT_EQ(mp->getCoordinate(), nullptr);
  EXPECT_EQ(mp->getGeometryType(), "MultiPoint");
  EXPECT_EQ(mp->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(mp->getDimension(), geos::geom::Dimension::P);
  EXPECT_EQ(mp->getBoundaryDimension(), geos::geom::Dimension::False);
  EXPECT_EQ(mp->getCoordinateDimension(), 2);
  EXPECT_FALSE(mp->hasZ());
  EXPECT_FALSE(mp->hasM());
  EXPECT_DOUBLE_EQ(mp->getArea(), 0.0);
  EXPECT_DOUBLE_EQ(mp->getLength(), 0.0);
  EXPECT_TRUE(mp->isPuntal());
  EXPECT_FALSE(mp->isLineal());
  EXPECT_FALSE(mp->isPolygonal());
  EXPECT_TRUE(mp->isCollection());
  EXPECT_FALSE(mp->isMixedDimension());
  EXPECT_TRUE(mp->isValid());
  EXPECT_TRUE(mp->isSimple());
  EXPECT_TRUE(mp->getEnvelopeInternal()->isNull());
  EXPECT_EQ(mp->toText(), "MULTIPOINT EMPTY");

  const geos::geom::Geometry* base = mp.get();
  EXPECT_FALSE(base->hasCurvedComponents());

  std::unique_ptr<geos::geom::MultiPoint> empty_from_pt_vec =
      factory->createMultiPoint(
          std::vector<std::unique_ptr<geos::geom::Point>>{});
  EXPECT_TRUE(empty_from_pt_vec->isEmpty());
  EXPECT_EQ(empty_from_pt_vec->getNumGeometries(), 0u);
  EXPECT_EQ(empty_from_pt_vec->getGeometryTypeId(),
            geos::geom::GEOS_MULTIPOINT);

  std::unique_ptr<geos::geom::MultiPoint> empty_from_geom_vec =
      factory->createMultiPoint(
          std::vector<std::unique_ptr<geos::geom::Geometry>>{});
  EXPECT_TRUE(empty_from_geom_vec->isEmpty());
  EXPECT_EQ(empty_from_geom_vec->getNumGeometries(), 0u);

  std::unique_ptr<geos::geom::MultiPoint> empty_from_raw_vec =
      factory->createMultiPoint(std::vector<const geos::geom::Geometry*>{});
  EXPECT_TRUE(empty_from_raw_vec->isEmpty());
  EXPECT_EQ(empty_from_raw_vec->getNumGeometries(), 0u);
}

TEST(MultiPointTest, CreateFromPointVector) {
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create();
  std::vector<std::unique_ptr<geos::geom::Point>> points;
  points.push_back(factory->createPoint(geos::geom::CoordinateXY(1.5, 2.5)));
  points.push_back(factory->createPoint(geos::geom::CoordinateXY(-3.0, 4.0)));

  std::unique_ptr<geos::geom::MultiPoint> mp =
      factory->createMultiPoint(std::move(points));
  ASSERT_NE(mp, nullptr);

  EXPECT_FALSE(mp->isEmpty());
  EXPECT_EQ(mp->getNumGeometries(), 2u);
  EXPECT_EQ(mp->getNumPoints(), 2u);

  const geos::geom::Point* p0 = mp->getGeometryN(0);
  const geos::geom::Point* p1 = mp->getGeometryN(1);
  ASSERT_NE(p0, nullptr);
  ASSERT_NE(p1, nullptr);
  EXPECT_DOUBLE_EQ(p0->getX(), 1.5);
  EXPECT_DOUBLE_EQ(p0->getY(), 2.5);
  EXPECT_DOUBLE_EQ(p1->getX(), -3.0);
  EXPECT_DOUBLE_EQ(p1->getY(), 4.0);

  const geos::geom::CoordinateXY* first = mp->getCoordinate();
  ASSERT_NE(first, nullptr);
  EXPECT_DOUBLE_EQ(first->x, 1.5);
  EXPECT_DOUBLE_EQ(first->y, 2.5);
  EXPECT_EQ(mp->toText(), "MULTIPOINT ((1.5 2.5), (-3 4))");

  const geos::geom::Geometry* as_geom = mp.get();
  const geos::geom::GeometryCollection* as_gc = mp.get();
  EXPECT_EQ(as_geom->getGeometryN(0), p0);
  EXPECT_EQ(as_gc->getGeometryN(1), p1);
}

TEST(MultiPointTest, CreateFromGeometryVectorAndConstPointers) {
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create();

  std::vector<std::unique_ptr<geos::geom::Geometry>> owned;
  owned.push_back(factory->createPoint(geos::geom::CoordinateXY(10.0, 20.0)));
  owned.push_back(factory->createPoint(geos::geom::CoordinateXY(30.0, 40.0)));
  std::unique_ptr<geos::geom::MultiPoint> from_geom =
      factory->createMultiPoint(std::move(owned));
  ASSERT_EQ(from_geom->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(from_geom->getGeometryN(0)->getX(), 10.0);
  EXPECT_DOUBLE_EQ(from_geom->getGeometryN(1)->getY(), 40.0);

  auto src0 = factory->createPoint(geos::geom::CoordinateXY(10.0, 20.0));
  auto src1 = factory->createPoint(geos::geom::CoordinateXY(30.0, 40.0));
  std::vector<const geos::geom::Geometry*> raw_ptrs = {src0.get(), src1.get()};
  std::unique_ptr<geos::geom::MultiPoint> copied =
      factory->createMultiPoint(raw_ptrs);
  src0->setXY(99.0, 99.0);
  EXPECT_DOUBLE_EQ(copied->getGeometryN(0)->getX(), 10.0);
  EXPECT_TRUE(copied->equalsExact(from_geom.get()));
}

TEST(MultiPointTest, CreateFromCoordinateSequenceAndContainers) {
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create();

  geos::geom::CoordinateSequence empty_seq(0u, false, false);
  std::unique_ptr<geos::geom::MultiPoint> empty_from_seq =
      factory->createMultiPoint(empty_seq);
  EXPECT_TRUE(empty_from_seq->isEmpty());
  EXPECT_EQ(empty_from_seq->getNumGeometries(), 0u);

  geos::geom::CoordinateSequence seq(0u, false, false);
  seq.add(geos::geom::CoordinateXY(1.0, 2.0));
  seq.add(geos::geom::CoordinateXY(5.0, 6.0));
  std::unique_ptr<geos::geom::MultiPoint> from_seq =
      factory->createMultiPoint(seq);
  ASSERT_EQ(from_seq->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(from_seq->getGeometryN(0)->getX(), 1.0);
  EXPECT_DOUBLE_EQ(from_seq->getGeometryN(1)->getY(), 6.0);

  std::vector<geos::geom::CoordinateXY> vec = {
      geos::geom::CoordinateXY(1.0, 2.0),
      geos::geom::CoordinateXY(5.0, 6.0),
  };
  std::unique_ptr<geos::geom::MultiPoint> from_vec =
      factory->createMultiPoint(vec);
  EXPECT_TRUE(from_vec->equalsExact(from_seq.get()));
}

TEST(MultiPointTest, ContainsEmptyPoints) {
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create();

  std::vector<std::unique_ptr<geos::geom::Point>> only_empty;
  only_empty.push_back(factory->createPoint());
  only_empty.push_back(factory->createPoint());
  std::unique_ptr<geos::geom::MultiPoint> mp_all_empty =
      factory->createMultiPoint(std::move(only_empty));

  EXPECT_TRUE(mp_all_empty->isEmpty());
  EXPECT_EQ(mp_all_empty->getNumGeometries(), 2u);
  EXPECT_EQ(mp_all_empty->getNumPoints(), 0u);
  EXPECT_EQ(mp_all_empty->getCoordinate(), nullptr);
  EXPECT_EQ(mp_all_empty->getDimension(), geos::geom::Dimension::P);
  EXPECT_EQ(mp_all_empty->getBoundaryDimension(), geos::geom::Dimension::False);

  std::vector<std::unique_ptr<geos::geom::Point>> mixed;
  mixed.push_back(factory->createPoint());
  mixed.push_back(factory->createPoint(geos::geom::CoordinateXY(4.0, 5.0)));
  std::unique_ptr<geos::geom::MultiPoint> mp_mixed =
      factory->createMultiPoint(std::move(mixed));
  EXPECT_FALSE(mp_mixed->isEmpty());
  EXPECT_EQ(mp_mixed->getNumGeometries(), 2u);
  EXPECT_EQ(mp_mixed->getNumPoints(), 1u);
  ASSERT_NE(mp_mixed->getCoordinate(), nullptr);
  EXPECT_DOUBLE_EQ(mp_mixed->getCoordinate()->x, 4.0);
  EXPECT_DOUBLE_EQ(mp_mixed->getCoordinate()->y, 5.0);
  EXPECT_TRUE(mp_mixed->getGeometryN(0)->isEmpty());
  EXPECT_FALSE(mp_mixed->getGeometryN(1)->isEmpty());
  EXPECT_EQ(mp_mixed->toText(), "MULTIPOINT (EMPTY, (4 5))");
}

TEST(MultiPointTest, DimensionPredicates) {
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create();
  std::unique_ptr<geos::geom::MultiPoint> empty = factory->createMultiPoint();

  std::vector<geos::geom::CoordinateXY> coords = {{1.0, 2.0}};
  std::unique_ptr<geos::geom::MultiPoint> non_empty =
      factory->createMultiPoint(coords);

  for (const geos::geom::MultiPoint* mp : {empty.get(), non_empty.get()}) {
    const geos::geom::Geometry* base = mp;
    EXPECT_EQ(base->getDimension(), geos::geom::Dimension::P);
    EXPECT_EQ(base->getBoundaryDimension(), geos::geom::Dimension::False);

    EXPECT_TRUE(base->hasDimension(geos::geom::Dimension::P));
    EXPECT_FALSE(base->hasDimension(geos::geom::Dimension::L));
    EXPECT_FALSE(base->hasDimension(geos::geom::Dimension::A));
    EXPECT_FALSE(base->hasDimension(geos::geom::Dimension::False));
    EXPECT_FALSE(base->hasDimension(geos::geom::Dimension::True));
    EXPECT_FALSE(base->hasDimension(geos::geom::Dimension::DONTCARE));

    EXPECT_TRUE(base->isDimensionStrict(geos::geom::Dimension::P));
    EXPECT_FALSE(base->isDimensionStrict(geos::geom::Dimension::L));
    EXPECT_FALSE(base->isDimensionStrict(geos::geom::Dimension::A));
    EXPECT_FALSE(base->isDimensionStrict(geos::geom::Dimension::False));
    EXPECT_FALSE(base->isDimensionStrict(geos::geom::Dimension::True));
    EXPECT_FALSE(base->isDimensionStrict(geos::geom::Dimension::DONTCARE));
  }
}

TEST(MultiPointTest, BoundaryIsEmptyGeometryCollection) {
  geos::geom::PrecisionModel pm;
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create(&pm, 4326);

  std::unique_ptr<geos::geom::MultiPoint> empty = factory->createMultiPoint();
  std::unique_ptr<geos::geom::Geometry> empty_boundary = empty->getBoundary();
  ASSERT_NE(empty_boundary, nullptr);
  EXPECT_TRUE(empty_boundary->isEmpty());
  EXPECT_EQ(empty_boundary->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(empty_boundary->getNumGeometries(), 0u);
  EXPECT_EQ(empty_boundary->getSRID(), 4326);

  std::vector<geos::geom::CoordinateXY> coords = {{0.0, 0.0}, {10.0, 10.0}};
  std::unique_ptr<geos::geom::MultiPoint> non_empty =
      factory->createMultiPoint(coords);
  std::unique_ptr<geos::geom::Geometry> boundary = non_empty->getBoundary();
  ASSERT_NE(boundary, nullptr);
  EXPECT_TRUE(boundary->isEmpty());
  EXPECT_EQ(boundary->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(boundary->getNumGeometries(), 0u);
  EXPECT_EQ(boundary->getSRID(), 4326);
}

TEST(MultiPointTest, ProtectedGetCoordinateNAndConstructors) {
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create();

  std::vector<std::unique_ptr<geos::geom::Point>> pts;
  pts.push_back(factory->createPoint(geos::geom::CoordinateXY(7.0, 8.0)));
  pts.push_back(factory->createPoint(geos::geom::CoordinateXY(9.0, 10.0)));
  pts.push_back(factory->createPoint());

  TestableMultiPoint mp_from_pts(std::move(pts), *factory);
  EXPECT_EQ(mp_from_pts.SortIndex(), geos::geom::SORTINDEX_MULTIPOINT);
  EXPECT_FALSE(mp_from_pts.CurvedComponents());

  const geos::geom::CoordinateXY* c0 = mp_from_pts.CoordinateAt(0);
  const geos::geom::CoordinateXY* c1 = mp_from_pts.CoordinateAt(1);
  const geos::geom::CoordinateXY* c2 = mp_from_pts.CoordinateAt(2);
  ASSERT_NE(c0, nullptr);
  ASSERT_NE(c1, nullptr);
  EXPECT_DOUBLE_EQ(c0->x, 7.0);
  EXPECT_DOUBLE_EQ(c0->y, 8.0);
  EXPECT_DOUBLE_EQ(c1->x, 9.0);
  EXPECT_DOUBLE_EQ(c1->y, 10.0);
  EXPECT_EQ(c2, nullptr);

  std::vector<std::unique_ptr<geos::geom::Geometry>> geoms;
  geoms.push_back(factory->createPoint(geos::geom::CoordinateXY(-1.0, -2.0)));
  TestableMultiPoint mp_from_geoms(std::move(geoms), *factory);
  const geos::geom::CoordinateXY* cg = mp_from_geoms.CoordinateAt(0);
  ASSERT_NE(cg, nullptr);
  EXPECT_DOUBLE_EQ(cg->x, -1.0);
  EXPECT_DOUBLE_EQ(cg->y, -2.0);

  TestableMultiPoint copied(mp_from_pts);
  EXPECT_TRUE(copied.equalsIdentical(&mp_from_pts));
  ASSERT_NE(copied.CoordinateAt(1), nullptr);
  EXPECT_DOUBLE_EQ(copied.CoordinateAt(1)->x, 9.0);
  EXPECT_DOUBLE_EQ(copied.CoordinateAt(1)->y, 10.0);

  std::vector<std::unique_ptr<geos::geom::Point>> heap_pts;
  heap_pts.push_back(factory->createPoint(geos::geom::CoordinateXY(5.0, 6.0)));
  std::unique_ptr<geos::geom::Geometry> poly_mp =
      std::make_unique<TestableMultiPoint>(std::move(heap_pts), *factory);
  EXPECT_EQ(poly_mp->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(poly_mp->getGeometryType(), "MultiPoint");
  EXPECT_EQ(poly_mp->getNumGeometries(), 1u);
}

TEST(MultiPointTest, CloneAndReverse) {
  geos::geom::PrecisionModel pm;
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create(&pm, 3857);

  std::vector<geos::geom::CoordinateXY> coords = {{1.0, 2.0}, {3.0, 4.0}};
  std::unique_ptr<geos::geom::MultiPoint> mp =
      factory->createMultiPoint(coords);

  std::unique_ptr<geos::geom::MultiPoint> cloned = mp->clone();
  ASSERT_NE(cloned, nullptr);
  EXPECT_NE(cloned.get(), mp.get());
  EXPECT_EQ(cloned->getSRID(), 3857);
  EXPECT_TRUE(cloned->equalsIdentical(mp.get()));

  std::unique_ptr<geos::geom::MultiPoint> reversed = mp->reverse();
  ASSERT_NE(reversed, nullptr);
  EXPECT_NE(reversed.get(), mp.get());
  EXPECT_EQ(reversed->getSRID(), 3857);
  EXPECT_TRUE(reversed->equalsIdentical(mp.get()));

  const geos::geom::Geometry* base = mp.get();
  std::unique_ptr<geos::geom::Geometry> poly_clone = base->clone();
  std::unique_ptr<geos::geom::Geometry> poly_rev = base->reverse();
  EXPECT_EQ(poly_clone->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(poly_rev->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_TRUE(poly_clone->equalsIdentical(mp.get()));
  EXPECT_TRUE(poly_rev->equalsIdentical(mp.get()));

  std::unique_ptr<geos::geom::MultiPoint> empty = factory->createMultiPoint();
  std::unique_ptr<geos::geom::MultiPoint> empty_rev = empty->reverse();
  EXPECT_TRUE(empty_rev->isEmpty());
  EXPECT_NE(empty_rev.get(), empty.get());
  EXPECT_EQ(empty_rev->getSRID(), 3857);
}

TEST(MultiPointTest, CoordinateDimensionsZAndM) {
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create();

  std::vector<std::unique_ptr<geos::geom::Point>> xy_pts;
  xy_pts.push_back(factory->createPoint(geos::geom::CoordinateXY(1.0, 2.0)));
  auto mp_xy = factory->createMultiPoint(std::move(xy_pts));
  EXPECT_EQ(mp_xy->getCoordinateDimension(), 2);
  EXPECT_FALSE(mp_xy->hasZ());
  EXPECT_FALSE(mp_xy->hasM());
  EXPECT_TRUE(std::isnan(mp_xy->getGeometryN(0)->getZ()));
  EXPECT_TRUE(std::isnan(mp_xy->getGeometryN(0)->getM()));

  std::vector<std::unique_ptr<geos::geom::Point>> xyz_pts;
  xyz_pts.push_back(
      factory->createPoint(geos::geom::Coordinate(1.0, 2.0, 3.0)));
  auto mp_xyz = factory->createMultiPoint(std::move(xyz_pts));
  EXPECT_EQ(mp_xyz->getCoordinateDimension(), 3);
  EXPECT_TRUE(mp_xyz->hasZ());
  EXPECT_FALSE(mp_xyz->hasM());
  EXPECT_DOUBLE_EQ(mp_xyz->getGeometryN(0)->getZ(), 3.0);
  EXPECT_TRUE(std::isnan(mp_xyz->getGeometryN(0)->getM()));

  std::vector<std::unique_ptr<geos::geom::Point>> xym_pts;
  xym_pts.push_back(
      factory->createPoint(geos::geom::CoordinateXYM(1.0, 2.0, 4.0)));
  auto mp_xym = factory->createMultiPoint(std::move(xym_pts));
  EXPECT_EQ(mp_xym->getCoordinateDimension(), 3);
  EXPECT_FALSE(mp_xym->hasZ());
  EXPECT_TRUE(mp_xym->hasM());
  EXPECT_TRUE(std::isnan(mp_xym->getGeometryN(0)->getZ()));
  EXPECT_DOUBLE_EQ(mp_xym->getGeometryN(0)->getM(), 4.0);

  std::vector<std::unique_ptr<geos::geom::Point>> xyzm_pts;
  xyzm_pts.push_back(
      factory->createPoint(geos::geom::CoordinateXYZM(1.0, 2.0, 3.0, 4.0)));
  auto mp_xyzm = factory->createMultiPoint(std::move(xyzm_pts));
  EXPECT_EQ(mp_xyzm->getCoordinateDimension(), 4);
  EXPECT_TRUE(mp_xyzm->hasZ());
  EXPECT_TRUE(mp_xyzm->hasM());
  EXPECT_DOUBLE_EQ(mp_xyzm->getGeometryN(0)->getZ(), 3.0);
  EXPECT_DOUBLE_EQ(mp_xyzm->getGeometryN(0)->getM(), 4.0);

  EXPECT_TRUE(mp_xy->equalsExact(mp_xyzm.get()));
  EXPECT_FALSE(mp_xy->equalsIdentical(mp_xyzm.get()));
}

TEST(MultiPointTest, CompareToAndSortOrder) {
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create();

  auto pt = factory->createPoint(geos::geom::CoordinateXY(1.0, 1.0));
  auto empty_mp = factory->createMultiPoint();
  auto mp_low = factory->createMultiPoint(
      std::vector<geos::geom::CoordinateXY>{{1.0, 1.0}, {2.0, 2.0}});
  auto mp_high = factory->createMultiPoint(
      std::vector<geos::geom::CoordinateXY>{{1.0, 1.0}, {5.0, 5.0}});

  auto line_seq =
      std::make_unique<geos::geom::CoordinateSequence>(0u, false, false);
  line_seq->add(geos::geom::CoordinateXY(0.0, 0.0));
  line_seq->add(geos::geom::CoordinateXY(1.0, 1.0));
  auto line = factory->createLineString(std::move(line_seq));

  EXPECT_LT(pt->compareTo(mp_low.get()), 0);
  EXPECT_GT(mp_low->compareTo(pt.get()), 0);
  EXPECT_LT(mp_low->compareTo(line.get()), 0);
  EXPECT_GT(line->compareTo(mp_low.get()), 0);

  EXPECT_EQ(mp_low->compareTo(mp_low.get()), 0);
  EXPECT_LT(empty_mp->compareTo(mp_low.get()), 0);
  EXPECT_GT(mp_low->compareTo(empty_mp.get()), 0);
  EXPECT_LT(mp_low->compareTo(mp_high.get()), 0);
  EXPECT_GT(mp_high->compareTo(mp_low.get()), 0);
}

TEST(MultiPointTest, EnvelopeNormalizeSimpleAndFilters) {
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create();

  auto mp = factory->createMultiPoint(std::vector<geos::geom::CoordinateXY>{
      {1.0, 2.0}, {5.0, 8.0}, {-2.0, 4.0}});
  EXPECT_TRUE(mp->isSimple());

  const geos::geom::Envelope* env = mp->getEnvelopeInternal();
  EXPECT_DOUBLE_EQ(env->getMinX(), -2.0);
  EXPECT_DOUBLE_EQ(env->getMaxX(), 5.0);
  EXPECT_DOUBLE_EQ(env->getMinY(), 2.0);
  EXPECT_DOUBLE_EQ(env->getMaxY(), 8.0);

  auto coords = mp->getCoordinates();
  ASSERT_EQ(coords->size(), 3u);
  EXPECT_DOUBLE_EQ(coords->getX(0), 1.0);
  EXPECT_DOUBLE_EQ(coords->getY(2), 4.0);

  mp->normalize();
  EXPECT_DOUBLE_EQ(mp->getGeometryN(0)->getX(), 5.0);
  EXPECT_DOUBLE_EQ(mp->getGeometryN(1)->getX(), 1.0);
  EXPECT_DOUBLE_EQ(mp->getGeometryN(2)->getX(), -2.0);

  CountingGeometryFilter geom_filter;
  mp->apply_ro(&geom_filter);
  ASSERT_EQ(geom_filter.types.size(), 4u);
  EXPECT_EQ(geom_filter.types[0], "MultiPoint");
  EXPECT_EQ(geom_filter.types[1], "Point");

  ShiftXFilter shift(10.0);
  mp->apply_rw(&shift);
  mp->geometryChanged();
  EXPECT_DOUBLE_EQ(mp->getGeometryN(0)->getX(), 15.0);
  EXPECT_DOUBLE_EQ(mp->getEnvelopeInternal()->getMaxX(), 15.0);

  auto dup = factory->createMultiPoint(
      std::vector<geos::geom::CoordinateXY>{{1.0, 1.0}, {1.0, 1.0}});
  EXPECT_FALSE(dup->isSimple());

  std::vector<std::unique_ptr<geos::geom::Geometry>> gc_elems;
  gc_elems.push_back(factory->createPoint(geos::geom::CoordinateXY(1.0, 1.0)));
  gc_elems.push_back(factory->createPoint(geos::geom::CoordinateXY(1.0, 1.0)));
  auto gc = factory->createGeometryCollection(std::move(gc_elems));
  EXPECT_FALSE(dup->equalsExact(gc.get()));

  auto srid_test = dup->clone();
  srid_test->setSRID(32610);
  EXPECT_EQ(srid_test->getSRID(), 32610);
  EXPECT_EQ(srid_test->getGeometryN(0)->getSRID(), 32610);
}

TEST(MultiPointTest, FactoryEmptyAndBuildHelpers) {
  geos::geom::GeometryFactory::Ptr factory =
      geos::geom::GeometryFactory::create();

  auto empty_geom = factory->createEmptyGeometry(geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(empty_geom->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_TRUE(empty_geom->isEmpty());

  auto empty_typed = factory->createEmpty(geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(empty_typed->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_TRUE(empty_typed->isEmpty());

  std::vector<std::unique_ptr<geos::geom::Point>> pts;
  pts.push_back(factory->createPoint(geos::geom::CoordinateXY(0.0, 0.0)));
  pts.push_back(factory->createPoint(geos::geom::CoordinateXY(2.0, 0.0)));
  auto built = factory->buildGeometry(std::move(pts));
  EXPECT_EQ(built->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(built->getNumGeometries(), 2u);

  auto single_pt = factory->createPoint(geos::geom::CoordinateXY(3.0, 4.0));
  auto multi = factory->createMulti(std::move(single_pt));
  EXPECT_EQ(multi->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(multi->getNumGeometries(), 1u);
}

}  // namespace
}  // namespace geos_google
