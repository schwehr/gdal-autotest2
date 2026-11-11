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

#include "third_party/geos/include/geos/geom/GeometryFactory.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
#include "third_party/geos/include/geos/geom/CompoundCurve.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Curve.h"
#include "third_party/geos/include/geos/geom/CurvePolygon.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiCurve.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/MultiSurface.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/geom/SimpleCurve.h"
#include "third_party/geos/include/geos/geom/Surface.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::DoubleNotANumber;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Curve;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GeometryTypeId;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::geom::SimpleCurve;
using geos::geom::Surface;
using geos::util::IllegalArgumentException;
using geos::util::UnsupportedOperationException;

std::unique_ptr<CoordinateSequence> MakeBoxSeq(double minx, double miny,
                                               double maxx, double maxy) {
  return std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{
          {minx, miny},
          {maxx, miny},
          {maxx, maxy},
          {minx, maxy},
          {minx, miny},
      });
}

std::unique_ptr<CoordinateSequence> MakeLineSeq(double x1, double y1, double x2,
                                                double y2) {
  return std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{{x1, y1}, {x2, y2}});
}

std::unique_ptr<CoordinateSequence> MakeCircleArcSeq(double cx, double cy,
                                                     double r) {
  return std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{
          {cx - r, cy},
          {cx, cy + r},
          {cx + r, cy},
          {cx, cy - r},
          {cx - r, cy},
      });
}

TEST(GeometryFactoryTest, CreateDefaultAndGetDefaultInstance) {
  GeometryFactory::Ptr gf = GeometryFactory::create();
  ASSERT_NE(gf, nullptr);
  EXPECT_EQ(gf->getSRID(), 0);
  ASSERT_NE(gf->getPrecisionModel(), nullptr);
  EXPECT_EQ(gf->getPrecisionModel()->getType(), PrecisionModel::FLOATING);
  EXPECT_TRUE(gf->getPrecisionModel()->isFloating());

  const GeometryFactory* def1 = GeometryFactory::getDefaultInstance();
  const GeometryFactory* def2 = GeometryFactory::getDefaultInstance();
  ASSERT_NE(def1, nullptr);
  EXPECT_EQ(def1, def2);
  EXPECT_NE(def1, gf.get());
  EXPECT_EQ(def1->getSRID(), 0);
  EXPECT_EQ(def1->getPrecisionModel()->getType(), PrecisionModel::FLOATING);
}

TEST(GeometryFactoryTest, CreateWithPrecisionModelAndSRIDAndCopy) {
  GeometryFactory::Ptr gf_null_pm = GeometryFactory::create(nullptr);
  ASSERT_NE(gf_null_pm, nullptr);
  EXPECT_EQ(gf_null_pm->getSRID(), 0);
  EXPECT_EQ(gf_null_pm->getPrecisionModel()->getType(),
            PrecisionModel::FLOATING);

  GeometryFactory::Ptr gf_null_pm_srid = GeometryFactory::create(nullptr, 3857);
  ASSERT_NE(gf_null_pm_srid, nullptr);
  EXPECT_EQ(gf_null_pm_srid->getSRID(), 3857);
  EXPECT_EQ(gf_null_pm_srid->getPrecisionModel()->getType(),
            PrecisionModel::FLOATING);

  GeometryFactory::Ptr gf_fixed_no_srid;
  GeometryFactory::Ptr gf_fixed_srid;
  {
    PrecisionModel fixed_pm(100.0);
    gf_fixed_no_srid = GeometryFactory::create(&fixed_pm);
    gf_fixed_srid = GeometryFactory::create(&fixed_pm, 4326);
  }
  ASSERT_NE(gf_fixed_no_srid, nullptr);
  EXPECT_EQ(gf_fixed_no_srid->getSRID(), 0);
  EXPECT_EQ(gf_fixed_no_srid->getPrecisionModel()->getType(),
            PrecisionModel::FIXED);
  EXPECT_DOUBLE_EQ(gf_fixed_no_srid->getPrecisionModel()->getScale(), 100.0);

  ASSERT_NE(gf_fixed_srid, nullptr);
  EXPECT_EQ(gf_fixed_srid->getSRID(), 4326);
  EXPECT_EQ(gf_fixed_srid->getPrecisionModel()->getType(),
            PrecisionModel::FIXED);
  EXPECT_DOUBLE_EQ(gf_fixed_srid->getPrecisionModel()->getScale(), 100.0);

  PrecisionModel single_pm(PrecisionModel::FLOATING_SINGLE);
  auto gf_single = GeometryFactory::create(&single_pm, 32610);
  EXPECT_EQ(gf_single->getPrecisionModel()->getType(),
            PrecisionModel::FLOATING_SINGLE);
  EXPECT_EQ(gf_single->getSRID(), 32610);

  GeometryFactory::Ptr gf_clone = GeometryFactory::create(*gf_fixed_srid);
  ASSERT_NE(gf_clone, nullptr);
  EXPECT_NE(gf_clone.get(), gf_fixed_srid.get());
  EXPECT_EQ(gf_clone->getSRID(), 4326);
  EXPECT_EQ(gf_clone->getPrecisionModel()->getType(), PrecisionModel::FIXED);
  EXPECT_DOUBLE_EQ(gf_clone->getPrecisionModel()->getScale(), 100.0);

  // Cloning a factory that has live children creates an independent factory
  // with refCount=0 that can be destroyed independently.
  auto live_child = gf_fixed_srid->createPoint(CoordinateXY(1.0, 2.0));
  auto gf_clone_with_live_parent = GeometryFactory::create(*gf_fixed_srid);
  EXPECT_EQ(gf_clone_with_live_parent->getSRID(), 4326);
  gf_clone_with_live_parent.reset();
  EXPECT_EQ(live_child->getSRID(), 4326);
}

TEST(GeometryFactoryTest, RefCountAndAutoDestroyLifecycle) {
  // Case 1: Factory outlives all children (refCount drops 2 -> 1 -> 0 before
  // factory destroy()).
  {
    PrecisionModel pm(10.0);
    auto gf = GeometryFactory::create(&pm, 4326);
    auto p1 = gf->createPoint(CoordinateXY(1.0, 2.0));
    auto p2 = gf->createPoint(CoordinateXY(3.0, 4.0));
    EXPECT_EQ(p1->getFactory(), gf.get());
    EXPECT_EQ(p2->getFactory(), gf.get());
    p1.reset();
    p2.reset();
    gf.reset();
  }

  // Case 2: Children outlive the caller's GeometryFactory::Ptr; factory must
  // remain alive via _autoDestroy until the last child is destroyed.
  std::unique_ptr<Point> p1;
  std::unique_ptr<Point> p2;
  const GeometryFactory* raw_gf = nullptr;
  {
    PrecisionModel pm(10.0);
    auto gf = GeometryFactory::create(&pm, 26910);
    raw_gf = gf.get();
    p1 = gf->createPoint(CoordinateXY(5.0, 6.0));
    p2 = gf->createPoint(CoordinateXY(7.0, 8.0));
    gf.reset();  // destroy() sets _autoDestroy = true while _refCount == 2.
  }

  EXPECT_EQ(p1->getFactory(), raw_gf);
  EXPECT_EQ(p1->getSRID(), 26910);
  EXPECT_DOUBLE_EQ(p1->getX(), 5.0);
  EXPECT_DOUBLE_EQ(p1->getPrecisionModel()->getScale(), 10.0);
  p1.reset();  // _refCount 2 -> 1.
  EXPECT_EQ(p2->getSRID(), 26910);
  EXPECT_DOUBLE_EQ(p2->getY(), 8.0);
  p2.reset();  // _refCount 1 -> 0 triggers delete this.
}

TEST(GeometryFactoryTest, CreatePointFromInternalCoord) {
  auto gf_float = GeometryFactory::create();
  auto ex_float = gf_float->createPoint(CoordinateXY(0.0, 0.0));
  Coordinate coord_float(1.23456, 7.89012, 9.5);
  auto pt_float = GeometryFactory::createPointFromInternalCoord(&coord_float,
                                                                ex_float.get());
  EXPECT_EQ(pt_float->getFactory(), gf_float.get());
  EXPECT_EQ(pt_float->getSRID(), 0);
  EXPECT_DOUBLE_EQ(pt_float->getX(), 1.23456);
  EXPECT_DOUBLE_EQ(pt_float->getY(), 7.89012);
  EXPECT_DOUBLE_EQ(pt_float->getZ(), 9.5);

  PrecisionModel pm_fixed(10.0);
  auto gf_fixed = GeometryFactory::create(&pm_fixed, 4326);
  auto ex_fixed = gf_fixed->createPoint(CoordinateXY(0.0, 0.0));
  Coordinate coord_fixed(1.26, 2.34, 4.75);
  auto pt_fixed = GeometryFactory::createPointFromInternalCoord(&coord_fixed,
                                                                ex_fixed.get());
  EXPECT_EQ(pt_fixed->getFactory(), gf_fixed.get());
  EXPECT_EQ(pt_fixed->getSRID(), 4326);
  EXPECT_DOUBLE_EQ(pt_fixed->getX(), 1.3);
  EXPECT_DOUBLE_EQ(pt_fixed->getY(), 2.3);
  EXPECT_DOUBLE_EQ(pt_fixed->getZ(), 4.75);
  // Original coordinate passed in must not be mutated.
  EXPECT_DOUBLE_EQ(coord_fixed.x, 1.26);
  EXPECT_DOUBLE_EQ(coord_fixed.y, 2.34);
}

TEST(GeometryFactoryTest, ToGeometryFromEnvelope) {
  PrecisionModel pm;
  auto gf = GeometryFactory::create(&pm, 4326);

  // Null envelope -> empty Point.
  Envelope null_env;
  auto g_null = gf->toGeometry(&null_env);
  EXPECT_EQ(g_null->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_TRUE(g_null->isEmpty());
  EXPECT_EQ(g_null->getFactory(), gf.get());
  EXPECT_EQ(g_null->getSRID(), 4326);

  // Single-point envelope -> Point.
  Envelope pt_env(3.5, 3.5, -2.0, -2.0);
  auto g_pt = gf->toGeometry(&pt_env);
  EXPECT_EQ(g_pt->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_FALSE(g_pt->isEmpty());
  const auto* pt = dynamic_cast<const Point*>(g_pt.get());
  ASSERT_NE(pt, nullptr);
  EXPECT_DOUBLE_EQ(pt->getX(), 3.5);
  EXPECT_DOUBLE_EQ(pt->getY(), -2.0);
  EXPECT_EQ(pt->getSRID(), 4326);

  // 2D extent envelope -> rectangular Polygon.
  Envelope box_env(1.0, 4.0, 2.0, 6.0);
  auto g_box = gf->toGeometry(&box_env);
  EXPECT_EQ(g_box->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(g_box->isRectangle());
  EXPECT_DOUBLE_EQ(g_box->getArea(), 12.0);
  EXPECT_EQ(g_box->getNumPoints(), 5u);
  const auto* poly = dynamic_cast<const Polygon*>(g_box.get());
  ASSERT_NE(poly, nullptr);
  const CoordinateSequence* ring = poly->getExteriorRing()->getCoordinatesRO();
  EXPECT_DOUBLE_EQ(ring->getX(0), 1.0);
  EXPECT_DOUBLE_EQ(ring->getY(0), 2.0);
  EXPECT_DOUBLE_EQ(ring->getX(1), 4.0);
  EXPECT_DOUBLE_EQ(ring->getY(1), 2.0);
  EXPECT_DOUBLE_EQ(ring->getX(2), 4.0);
  EXPECT_DOUBLE_EQ(ring->getY(2), 6.0);
  EXPECT_DOUBLE_EQ(ring->getX(3), 1.0);
  EXPECT_DOUBLE_EQ(ring->getY(3), 6.0);
  EXPECT_DOUBLE_EQ(ring->getX(4), 1.0);
  EXPECT_DOUBLE_EQ(ring->getY(4), 2.0);

  // Degenerate horizontal and vertical envelopes -> Polygon.
  Envelope horiz_env(0.0, 5.0, 2.0, 2.0);
  auto g_horiz = gf->toGeometry(&horiz_env);
  EXPECT_EQ(g_horiz->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_EQ(g_horiz->getNumPoints(), 5u);
  EXPECT_DOUBLE_EQ(g_horiz->getArea(), 0.0);

  Envelope vert_env(2.0, 2.0, 1.0, 5.0);
  auto g_vert = gf->toGeometry(&vert_env);
  EXPECT_EQ(g_vert->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_EQ(g_vert->getNumPoints(), 5u);
  EXPECT_DOUBLE_EQ(g_vert->getArea(), 0.0);
}

TEST(GeometryFactoryTest, CreatePointOverloads) {
  auto gf = GeometryFactory::create();

  // Default dimension = 2 and explicit dimensions 2, 3, 4.
  auto p_def = gf->createPoint();
  EXPECT_TRUE(p_def->isEmpty());
  EXPECT_EQ(p_def->getCoordinateDimension(), 2u);
  EXPECT_FALSE(p_def->hasZ());
  EXPECT_FALSE(p_def->hasM());

  auto p_dim3 = gf->createPoint(std::size_t{3});
  EXPECT_TRUE(p_dim3->isEmpty());
  EXPECT_EQ(p_dim3->getCoordinateDimension(), 3u);
  EXPECT_TRUE(p_dim3->hasZ());
  EXPECT_FALSE(p_dim3->hasM());

  auto p_dim4 = gf->createPoint(std::size_t{4});
  EXPECT_TRUE(p_dim4->isEmpty());
  EXPECT_EQ(p_dim4->getCoordinateDimension(), 4u);
  EXPECT_TRUE(p_dim4->hasZ());
  EXPECT_TRUE(p_dim4->hasM());

  EXPECT_THROW(gf->createPoint(std::size_t{1}), IllegalArgumentException);
  EXPECT_THROW(gf->createPoint(std::size_t{5}), IllegalArgumentException);

  // hasZ / hasM empty points.
  auto p_xy = gf->createPoint(false, false);
  EXPECT_TRUE(p_xy->isEmpty());
  EXPECT_EQ(p_xy->getCoordinateDimension(), 2u);
  EXPECT_FALSE(p_xy->hasZ());
  EXPECT_FALSE(p_xy->hasM());

  auto p_xyz = gf->createPoint(true, false);
  EXPECT_TRUE(p_xyz->isEmpty());
  EXPECT_EQ(p_xyz->getCoordinateDimension(), 3u);
  EXPECT_TRUE(p_xyz->hasZ());
  EXPECT_FALSE(p_xyz->hasM());

  auto p_xym = gf->createPoint(false, true);
  EXPECT_TRUE(p_xym->isEmpty());
  EXPECT_EQ(p_xym->getCoordinateDimension(), 3u);
  EXPECT_FALSE(p_xym->hasZ());
  EXPECT_TRUE(p_xym->hasM());

  auto p_xyzm = gf->createPoint(true, true);
  EXPECT_TRUE(p_xyzm->isEmpty());
  EXPECT_EQ(p_xyzm->getCoordinateDimension(), 4u);
  EXPECT_TRUE(p_xyzm->hasZ());
  EXPECT_TRUE(p_xyzm->hasM());

  // Coordinate overloads.
  auto pt2 = gf->createPoint(CoordinateXY(1.5, -2.5));
  EXPECT_FALSE(pt2->isEmpty());
  EXPECT_EQ(pt2->getCoordinateDimension(), 2u);
  EXPECT_FALSE(pt2->hasZ());
  EXPECT_FALSE(pt2->hasM());
  EXPECT_DOUBLE_EQ(pt2->getX(), 1.5);
  EXPECT_DOUBLE_EQ(pt2->getY(), -2.5);
  EXPECT_TRUE(std::isnan(pt2->getZ()));
  EXPECT_TRUE(std::isnan(pt2->getM()));

  auto pt3 = gf->createPoint(Coordinate(1.0, 2.0, 3.0));
  EXPECT_EQ(pt3->getCoordinateDimension(), 3u);
  EXPECT_TRUE(pt3->hasZ());
  EXPECT_FALSE(pt3->hasM());
  EXPECT_DOUBLE_EQ(pt3->getZ(), 3.0);
  EXPECT_TRUE(std::isnan(pt3->getM()));

  auto pt3_nanz = gf->createPoint(Coordinate(1.0, 2.0));
  EXPECT_EQ(pt3_nanz->getCoordinateDimension(), 2u);
  EXPECT_FALSE(pt3_nanz->hasZ());
  EXPECT_TRUE(std::isnan(pt3_nanz->getZ()));

  auto ptm = gf->createPoint(CoordinateXYM(4.0, 5.0, 6.5));
  EXPECT_EQ(ptm->getCoordinateDimension(), 3u);
  EXPECT_FALSE(ptm->hasZ());
  EXPECT_TRUE(ptm->hasM());
  EXPECT_TRUE(std::isnan(ptm->getZ()));
  EXPECT_DOUBLE_EQ(ptm->getM(), 6.5);

  auto pt4 = gf->createPoint(CoordinateXYZM(1.0, 2.0, 3.0, 4.0));
  EXPECT_EQ(pt4->getCoordinateDimension(), 4u);
  EXPECT_TRUE(pt4->hasZ());
  EXPECT_TRUE(pt4->hasM());
  EXPECT_DOUBLE_EQ(pt4->getX(), 1.0);
  EXPECT_DOUBLE_EQ(pt4->getY(), 2.0);
  EXPECT_DOUBLE_EQ(pt4->getZ(), 3.0);
  EXPECT_DOUBLE_EQ(pt4->getM(), 4.0);

  auto pt4_nanz =
      gf->createPoint(CoordinateXYZM(1.0, 2.0, DoubleNotANumber, 8.0));
  EXPECT_FALSE(pt4_nanz->hasZ());
  EXPECT_TRUE(pt4_nanz->hasM());
  EXPECT_DOUBLE_EQ(pt4_nanz->getM(), 8.0);

  // CoordinateSequence unique_ptr&& overload (including nullptr and >1 error).
  std::unique_ptr<CoordinateSequence> null_seq;
  auto pt_null_seq = gf->createPoint(std::move(null_seq));
  EXPECT_TRUE(pt_null_seq->isEmpty());
  EXPECT_EQ(pt_null_seq->getCoordinateDimension(), 2u);

  auto empty_seq = std::make_unique<CoordinateSequence>(0u, true, true);
  auto pt_empty_seq = gf->createPoint(std::move(empty_seq));
  EXPECT_TRUE(pt_empty_seq->isEmpty());
  EXPECT_TRUE(pt_empty_seq->hasZ());
  EXPECT_TRUE(pt_empty_seq->hasM());

  auto one_seq = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXYZM>{{9.0, 8.0, 7.0, 6.0}});
  auto pt_moved = gf->createPoint(std::move(one_seq));
  EXPECT_DOUBLE_EQ(pt_moved->getX(), 9.0);
  EXPECT_DOUBLE_EQ(pt_moved->getZ(), 7.0);
  EXPECT_DOUBLE_EQ(pt_moved->getM(), 6.0);

  auto two_seq = MakeLineSeq(0.0, 0.0, 1.0, 1.0);
  EXPECT_THROW(gf->createPoint(std::move(two_seq)), IllegalArgumentException);

  // CoordinateSequence const& deep copy overload.
  CoordinateSequence src_seq(
      std::initializer_list<Coordinate>{{10.0, 20.0, 30.0}});
  auto pt_copy = gf->createPoint(src_seq);
  src_seq.setOrdinate(0, CoordinateSequence::X, 999.0);
  EXPECT_DOUBLE_EQ(pt_copy->getX(), 10.0);
  EXPECT_DOUBLE_EQ(pt_copy->getY(), 20.0);
  EXPECT_DOUBLE_EQ(pt_copy->getZ(), 30.0);

  CoordinateSequence bad_copy_seq(
      std::initializer_list<CoordinateXY>{{0.0, 0.0}, {1.0, 1.0}});
  EXPECT_THROW(gf->createPoint(bad_copy_seq), IllegalArgumentException);
}

TEST(GeometryFactoryTest, CreateLineStringOverloads) {
  auto gf = GeometryFactory::create();

  auto ls_def = gf->createLineString();
  EXPECT_TRUE(ls_def->isEmpty());
  EXPECT_EQ(ls_def->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_EQ(ls_def->getCoordinateDimension(), 2u);

  auto ls_dim3 = gf->createLineString(std::size_t{3});
  EXPECT_TRUE(ls_dim3->isEmpty());
  EXPECT_TRUE(ls_dim3->hasZ());
  EXPECT_FALSE(ls_dim3->hasM());

  auto ls_dim4 = gf->createLineString(std::size_t{4});
  EXPECT_TRUE(ls_dim4->isEmpty());
  EXPECT_TRUE(ls_dim4->hasZ());
  EXPECT_TRUE(ls_dim4->hasM());

  EXPECT_THROW(gf->createLineString(std::size_t{1}), IllegalArgumentException);

  for (bool hasZ : {false, true}) {
    for (bool hasM : {false, true}) {
      auto ls = gf->createLineString(hasZ, hasM);
      EXPECT_TRUE(ls->isEmpty());
      EXPECT_EQ(ls->hasZ(), hasZ);
      EXPECT_EQ(ls->hasM(), hasM);
    }
  }

  // CoordinateSequence::Ptr&& with nullptr.
  std::unique_ptr<CoordinateSequence> null_seq;
  auto ls_null = gf->createLineString(std::move(null_seq));
  EXPECT_TRUE(ls_null->isEmpty());
  EXPECT_EQ(ls_null->getCoordinateDimension(), 2u);

  // CoordinateSequence::Ptr&& with 1 point throws.
  auto single_seq = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{{1.0, 2.0}});
  EXPECT_THROW(gf->createLineString(std::move(single_seq)),
               IllegalArgumentException);

  // CoordinateSequence::Ptr&& with valid coords.
  auto ls_valid = gf->createLineString(MakeLineSeq(0.0, 0.0, 3.0, 4.0));
  EXPECT_EQ(ls_valid->getNumPoints(), 2u);
  EXPECT_DOUBLE_EQ(ls_valid->getLength(), 5.0);

  // CoordinateSequence const& deep copy.
  CoordinateSequence src(
      std::initializer_list<CoordinateXY>{{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}});
  auto ls_copy_seq = gf->createLineString(src);
  src.setOrdinate(1, CoordinateSequence::X, 50.0);
  EXPECT_EQ(ls_copy_seq->getNumPoints(), 3u);
  EXPECT_DOUBLE_EQ(ls_copy_seq->getCoordinateN(1).x, 1.0);

  // const LineString& copy constructor overload.
  auto ls_copy_obj = gf->createLineString(*ls_copy_seq);
  EXPECT_TRUE(ls_copy_obj->equalsExact(ls_copy_seq.get()));
  EXPECT_NE(ls_copy_obj.get(), ls_copy_seq.get());

  auto ls_empty_copy = gf->createLineString(*ls_def);
  EXPECT_TRUE(ls_empty_copy->isEmpty());
}

TEST(GeometryFactoryTest, CreateLinearRingOverloads) {
  auto gf = GeometryFactory::create();

  auto lr_def = gf->createLinearRing();
  EXPECT_TRUE(lr_def->isEmpty());
  EXPECT_TRUE(lr_def->isClosed());
  EXPECT_EQ(lr_def->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  EXPECT_EQ(lr_def->getCoordinateDimension(), 2u);

  auto lr_dim3 = gf->createLinearRing(std::size_t{3});
  EXPECT_TRUE(lr_dim3->isEmpty());
  EXPECT_TRUE(lr_dim3->hasZ());
  EXPECT_FALSE(lr_dim3->hasM());

  auto lr_dim4 = gf->createLinearRing(std::size_t{4});
  EXPECT_TRUE(lr_dim4->isEmpty());
  EXPECT_TRUE(lr_dim4->hasZ());
  EXPECT_TRUE(lr_dim4->hasM());

  for (bool hasZ : {false, true}) {
    for (bool hasM : {false, true}) {
      auto lr = gf->createLinearRing(hasZ, hasM);
      EXPECT_TRUE(lr->isEmpty());
      EXPECT_EQ(lr->hasZ(), hasZ);
      EXPECT_EQ(lr->hasM(), hasM);
    }
  }

  std::unique_ptr<CoordinateSequence> null_seq;
  auto lr_null = gf->createLinearRing(std::move(null_seq));
  EXPECT_TRUE(lr_null->isEmpty());

  auto lr_valid = gf->createLinearRing(MakeBoxSeq(0.0, 0.0, 2.0, 2.0));
  EXPECT_FALSE(lr_valid->isEmpty());
  EXPECT_TRUE(lr_valid->isClosed());
  EXPECT_EQ(lr_valid->getNumPoints(), 5u);

  // Unclosed sequence throws.
  auto unclosed = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}});
  EXPECT_THROW(gf->createLinearRing(std::move(unclosed)),
               IllegalArgumentException);

  // Invalid coordinate dimension throws.
  EXPECT_THROW(gf->createLinearRing(std::size_t{1}), IllegalArgumentException);

  // Too few points (2 identical points) throws.
  auto two_pts = MakeLineSeq(0.0, 0.0, 0.0, 0.0);
  EXPECT_THROW(gf->createLinearRing(std::move(two_pts)),
               IllegalArgumentException);

  // const CoordinateSequence& deep copy.
  CoordinateSequence ring_seq(std::initializer_list<CoordinateXY>{
      {0.0, 0.0}, {4.0, 0.0}, {4.0, 4.0}, {0.0, 0.0}});
  auto lr_copy = gf->createLinearRing(ring_seq);
  ring_seq.setOrdinate(1, CoordinateSequence::X, 100.0);
  EXPECT_EQ(lr_copy->getNumPoints(), 4u);
  EXPECT_DOUBLE_EQ(lr_copy->getCoordinateN(1).x, 4.0);
}

TEST(GeometryFactoryTest, CreatePolygonOverloadsAndDestroyGeometry) {
  auto gf = GeometryFactory::create();

  auto p_def = gf->createPolygon();
  EXPECT_TRUE(p_def->isEmpty());
  EXPECT_EQ(p_def->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_EQ(p_def->getCoordinateDimension(), 2u);

  auto p_dim3 = gf->createPolygon(std::size_t{3});
  EXPECT_TRUE(p_dim3->isEmpty());
  EXPECT_TRUE(p_dim3->hasZ());
  EXPECT_FALSE(p_dim3->hasM());

  auto p_dim4 = gf->createPolygon(std::size_t{4});
  EXPECT_TRUE(p_dim4->isEmpty());
  EXPECT_TRUE(p_dim4->hasZ());
  EXPECT_TRUE(p_dim4->hasM());

  EXPECT_THROW(gf->createPolygon(std::size_t{1}), IllegalArgumentException);

  for (bool hasZ : {false, true}) {
    for (bool hasM : {false, true}) {
      auto p = gf->createPolygon(hasZ, hasM);
      EXPECT_TRUE(p->isEmpty());
      EXPECT_EQ(p->hasZ(), hasZ);
      EXPECT_EQ(p->hasM(), hasM);
    }
  }

  // createPolygon(unique_ptr<LinearRing>&&).
  std::unique_ptr<LinearRing> null_shell;
  auto p_null_shell = gf->createPolygon(std::move(null_shell));
  EXPECT_TRUE(p_null_shell->isEmpty());

  auto shell = gf->createLinearRing(MakeBoxSeq(0.0, 0.0, 10.0, 10.0));
  auto p_shell = gf->createPolygon(std::move(shell));
  EXPECT_FALSE(p_shell->isEmpty());
  EXPECT_DOUBLE_EQ(p_shell->getArea(), 100.0);
  EXPECT_EQ(p_shell->getNumInteriorRing(), 0u);

  // createPolygon(CoordinateSequence&&).
  CoordinateSequence moved_seq(std::initializer_list<CoordinateXY>{
      {0.0, 0.0}, {3.0, 0.0}, {3.0, 4.0}, {0.0, 4.0}, {0.0, 0.0}});
  auto p_moved = gf->createPolygon(std::move(moved_seq));
  EXPECT_DOUBLE_EQ(p_moved->getArea(), 12.0);

  CoordinateSequence empty_moved(0u, 2u);
  auto p_empty_moved = gf->createPolygon(std::move(empty_moved));
  EXPECT_TRUE(p_empty_moved->isEmpty());

  // createPolygon(shell, vector<unique_ptr<LinearRing>>&&).
  auto outer = gf->createLinearRing(MakeBoxSeq(0.0, 0.0, 10.0, 10.0));
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(gf->createLinearRing(MakeBoxSeq(2.0, 2.0, 4.0, 4.0)));
  holes.push_back(gf->createLinearRing(MakeBoxSeq(6.0, 6.0, 8.0, 8.0)));
  auto p_with_holes = gf->createPolygon(std::move(outer), std::move(holes));
  EXPECT_EQ(p_with_holes->getNumInteriorRing(), 2u);
  EXPECT_DOUBLE_EQ(p_with_holes->getArea(), 100.0 - 4.0 - 4.0);

  // Empty shell with non-empty hole throws.
  std::vector<std::unique_ptr<LinearRing>> bad_holes;
  bad_holes.push_back(gf->createLinearRing(MakeBoxSeq(1.0, 1.0, 2.0, 2.0)));
  EXPECT_THROW(gf->createPolygon(gf->createLinearRing(), std::move(bad_holes)),
               IllegalArgumentException);

  // Raw pointer overload createPolygon(const LinearRing&, const
  // vector<LinearRing*>&) cleaned up via destroyGeometry.
  auto raw_shell = gf->createLinearRing(MakeBoxSeq(0.0, 0.0, 6.0, 6.0));
  auto raw_hole = gf->createLinearRing(MakeBoxSeq(1.0, 1.0, 3.0, 3.0));
  std::vector<LinearRing*> hole_ptrs{raw_hole.get()};
  Polygon* raw_poly = gf->createPolygon(*raw_shell, hole_ptrs);
  ASSERT_NE(raw_poly, nullptr);
  EXPECT_EQ(raw_poly->getNumInteriorRing(), 1u);
  EXPECT_DOUBLE_EQ(raw_poly->getArea(), 36.0 - 4.0);
  gf->destroyGeometry(raw_poly);

  std::vector<LinearRing*> no_holes;
  Polygon* raw_poly_no_holes = gf->createPolygon(*raw_shell, no_holes);
  ASSERT_NE(raw_poly_no_holes, nullptr);
  EXPECT_EQ(raw_poly_no_holes->getNumInteriorRing(), 0u);
  EXPECT_DOUBLE_EQ(raw_poly_no_holes->getArea(), 36.0);
  gf->destroyGeometry(raw_poly_no_holes);
  gf->destroyGeometry(nullptr);
}

TEST(GeometryFactoryTest, CreateCircularStringCompoundCurveAndCurvePolygon) {
  auto gf = GeometryFactory::create();

  // CircularString overloads.
  for (bool hasZ : {false, true}) {
    for (bool hasM : {false, true}) {
      auto cs = gf->createCircularString(hasZ, hasM);
      EXPECT_TRUE(cs->isEmpty());
      EXPECT_EQ(cs->getGeometryTypeId(), geos::geom::GEOS_CIRCULARSTRING);
      EXPECT_EQ(cs->hasZ(), hasZ);
      EXPECT_EQ(cs->hasM(), hasM);
    }
  }

  std::unique_ptr<CoordinateSequence> null_seq;
  auto cs_null = gf->createCircularString(std::move(null_seq));
  EXPECT_TRUE(cs_null->isEmpty());
  EXPECT_EQ(cs_null->getCoordinateDimension(), 2u);

  auto cs_bad = MakeLineSeq(0.0, 0.0, 1.0, 1.0);
  EXPECT_THROW(gf->createCircularString(std::move(cs_bad)),
               IllegalArgumentException);

  auto arc_seq = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}});
  auto cs_arc = gf->createCircularString(std::move(arc_seq));
  EXPECT_FALSE(cs_arc->isEmpty());
  EXPECT_EQ(cs_arc->getNumPoints(), 3u);
  EXPECT_TRUE(cs_arc->hasCurvedComponents());
  EXPECT_NEAR(cs_arc->getLength(), geos::MATH_PI, 1e-9);

  CoordinateSequence arc_src(
      std::initializer_list<CoordinateXY>{{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}});
  auto cs_copy_seq = gf->createCircularString(arc_src);
  arc_src.setOrdinate(1, CoordinateSequence::Y, 99.0);
  EXPECT_DOUBLE_EQ(cs_copy_seq->getCoordinateN(1).y, 1.0);

  auto cs_copy_obj = gf->createCircularString(*cs_arc);
  EXPECT_TRUE(cs_copy_obj->equalsExact(cs_arc.get()));
  EXPECT_NE(cs_copy_obj.get(), cs_arc.get());

  // CompoundCurve overloads.
  auto cc_empty = gf->createCompoundCurve();
  EXPECT_TRUE(cc_empty->isEmpty());
  EXPECT_EQ(cc_empty->getGeometryTypeId(), geos::geom::GEOS_COMPOUNDCURVE);
  EXPECT_EQ(cc_empty->getNumCurves(), 0u);

  std::vector<std::unique_ptr<SimpleCurve>> empty_curves;
  auto cc_from_empty = gf->createCompoundCurve(std::move(empty_curves));
  EXPECT_TRUE(cc_from_empty->isEmpty());

  std::vector<std::unique_ptr<SimpleCurve>> curves;
  curves.push_back(gf->createLineString(MakeLineSeq(0.0, 0.0, 2.0, 0.0)));
  curves.push_back(gf->createCircularString(
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {2.0, 0.0}, {1.0, 1.0}, {0.0, 0.0}})));
  auto cc_closed = gf->createCompoundCurve(std::move(curves));
  EXPECT_FALSE(cc_closed->isEmpty());
  EXPECT_EQ(cc_closed->getNumCurves(), 2u);
  EXPECT_TRUE(cc_closed->isClosed());
  EXPECT_TRUE(cc_closed->hasCurvedComponents());

  // Non-contiguous sections throw.
  std::vector<std::unique_ptr<SimpleCurve>> bad_curves;
  bad_curves.push_back(gf->createLineString(MakeLineSeq(0.0, 0.0, 2.0, 0.0)));
  bad_curves.push_back(gf->createLineString(MakeLineSeq(5.0, 5.0, 6.0, 6.0)));
  EXPECT_THROW(gf->createCompoundCurve(std::move(bad_curves)),
               IllegalArgumentException);

  // CurvePolygon overloads.
  for (bool hasZ : {false, true}) {
    for (bool hasM : {false, true}) {
      auto cp = gf->createCurvePolygon(hasZ, hasM);
      EXPECT_TRUE(cp->isEmpty());
      EXPECT_EQ(cp->getGeometryTypeId(), geos::geom::GEOS_CURVEPOLYGON);
      EXPECT_EQ(cp->hasZ(), hasZ);
      EXPECT_EQ(cp->hasM(), hasM);
    }
  }

  auto cp_shell = gf->createCurvePolygon(
      gf->createCircularString(MakeCircleArcSeq(2.0, 0.0, 2.0)));
  EXPECT_FALSE(cp_shell->isEmpty());
  EXPECT_TRUE(cp_shell->hasCurvedComponents());
  EXPECT_EQ(cp_shell->getNumInteriorRing(), 0u);
  EXPECT_NEAR(cp_shell->getArea(), 4.0 * geos::MATH_PI, 1e-9);

  auto cp_cc_shell = gf->createCurvePolygon(std::move(cc_closed));
  EXPECT_FALSE(cp_cc_shell->isEmpty());
  EXPECT_TRUE(cp_cc_shell->hasCurvedComponents());

  std::unique_ptr<Curve> null_cp_shell;
  auto cp_null_shell = gf->createCurvePolygon(std::move(null_cp_shell));
  EXPECT_TRUE(cp_null_shell->isEmpty());
  EXPECT_EQ(cp_null_shell->getGeometryTypeId(), geos::geom::GEOS_CURVEPOLYGON);

  auto cp_outer = gf->createCircularString(MakeCircleArcSeq(2.0, 0.0, 2.0));
  std::vector<std::unique_ptr<Curve>> cp_holes;
  cp_holes.push_back(gf->createCircularString(MakeCircleArcSeq(2.0, 0.0, 1.0)));
  auto cp_with_hole =
      gf->createCurvePolygon(std::move(cp_outer), std::move(cp_holes));
  EXPECT_EQ(cp_with_hole->getNumInteriorRing(), 1u);
  EXPECT_NEAR(cp_with_hole->getArea(), 3.0 * geos::MATH_PI, 1e-9);
}

TEST(GeometryFactoryTest, CreateMultiPointOverloads) {
  auto gf = GeometryFactory::create();

  auto mp_empty = gf->createMultiPoint();
  EXPECT_TRUE(mp_empty->isEmpty());
  EXPECT_EQ(mp_empty->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(mp_empty->getNumGeometries(), 0u);

  // Template createMultiPoint(const T& fromCoords).
  std::vector<CoordinateXY> empty_coords;
  auto mp_empty_tpl = gf->createMultiPoint(empty_coords);
  EXPECT_TRUE(mp_empty_tpl->isEmpty());

  std::vector<CoordinateXY> xy_coords{{1.0, 2.0}, {3.0, 4.0}};
  auto mp_xy = gf->createMultiPoint(xy_coords);
  EXPECT_EQ(mp_xy->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(mp_xy->getGeometryN(0)->getX(), 1.0);
  EXPECT_DOUBLE_EQ(mp_xy->getGeometryN(1)->getY(), 4.0);
  EXPECT_FALSE(mp_xy->hasZ());
  EXPECT_FALSE(mp_xy->hasM());

  std::vector<CoordinateXYZM> xyzm_coords{{1.0, 2.0, 3.0, 4.0},
                                          {5.0, 6.0, 7.0, 8.0}};
  auto mp_xyzm = gf->createMultiPoint(xyzm_coords);
  EXPECT_EQ(mp_xyzm->getNumGeometries(), 2u);
  EXPECT_TRUE(mp_xyzm->hasZ());
  EXPECT_TRUE(mp_xyzm->hasM());
  EXPECT_DOUBLE_EQ(mp_xyzm->getGeometryN(1)->getZ(), 7.0);
  EXPECT_DOUBLE_EQ(mp_xyzm->getGeometryN(1)->getM(), 8.0);

  std::vector<Coordinate> xyz_coords{{1.0, 2.0, 3.0}};
  auto mp_xyz = gf->createMultiPoint(xyz_coords);
  EXPECT_EQ(mp_xyz->getNumGeometries(), 1u);
  EXPECT_TRUE(mp_xyz->hasZ());
  EXPECT_FALSE(mp_xyz->hasM());
  EXPECT_DOUBLE_EQ(mp_xyz->getGeometryN(0)->getZ(), 3.0);

  std::vector<CoordinateXYM> xym_coords{{7.0, 8.0, 9.0}};
  auto mp_xym = gf->createMultiPoint(xym_coords);
  EXPECT_EQ(mp_xym->getNumGeometries(), 1u);
  EXPECT_FALSE(mp_xym->hasZ());
  EXPECT_TRUE(mp_xym->hasM());
  EXPECT_DOUBLE_EQ(mp_xym->getGeometryN(0)->getM(), 9.0);

  // vector<unique_ptr<Point>>&& (empty and populated).
  std::vector<std::unique_ptr<Point>> empty_pts;
  EXPECT_TRUE(gf->createMultiPoint(std::move(empty_pts))->isEmpty());

  std::vector<std::unique_ptr<Point>> pts;
  pts.push_back(gf->createPoint(CoordinateXY(10.0, 20.0)));
  pts.push_back(gf->createPoint(CoordinateXY(30.0, 40.0)));
  auto mp_pts = gf->createMultiPoint(std::move(pts));
  EXPECT_EQ(mp_pts->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(mp_pts->getGeometryN(0)->getX(), 10.0);

  // vector<unique_ptr<Geometry>>&& (empty and populated).
  std::vector<std::unique_ptr<Geometry>> empty_geom_pts;
  EXPECT_TRUE(gf->createMultiPoint(std::move(empty_geom_pts))->isEmpty());

  std::vector<std::unique_ptr<Geometry>> geom_pts;
  geom_pts.push_back(gf->createPoint(CoordinateXY(5.0, 6.0)));
  auto mp_geom_pts = gf->createMultiPoint(std::move(geom_pts));
  EXPECT_EQ(mp_geom_pts->getNumGeometries(), 1u);
  EXPECT_DOUBLE_EQ(mp_geom_pts->getGeometryN(0)->getY(), 6.0);

  // const vector<const Geometry*>& empty and deep copy (including empty/XYZ).
  std::vector<const Geometry*> empty_raw;
  EXPECT_TRUE(gf->createMultiPoint(empty_raw)->isEmpty());

  auto p1 = gf->createPoint(CoordinateXY(1.0, 1.0));
  auto p2 = gf->createPoint(Coordinate(2.0, 3.0, 4.0));
  auto p3_empty = gf->createPoint();
  std::vector<const Geometry*> raw_pts{p1.get(), p2.get(), p3_empty.get()};
  auto mp_copy = gf->createMultiPoint(raw_pts);
  p1.reset();
  p2.reset();
  p3_empty.reset();
  EXPECT_EQ(mp_copy->getNumGeometries(), 3u);
  EXPECT_DOUBLE_EQ(mp_copy->getGeometryN(0)->getX(), 1.0);
  EXPECT_DOUBLE_EQ(mp_copy->getGeometryN(1)->getY(), 3.0);
  EXPECT_DOUBLE_EQ(mp_copy->getGeometryN(1)->getZ(), 4.0);
  EXPECT_TRUE(mp_copy->getGeometryN(2)->isEmpty());

  // const CoordinateSequence& overloads across XY, XYZ, XYM, XYZM and empty.
  CoordinateSequence seq_empty(0u, 2u);
  EXPECT_TRUE(gf->createMultiPoint(seq_empty)->isEmpty());

  CoordinateSequence seq_xy(
      std::initializer_list<CoordinateXY>{{1.0, 2.0}, {3.0, 4.0}});
  auto mp_seq_xy = gf->createMultiPoint(seq_xy);
  EXPECT_EQ(mp_seq_xy->getNumGeometries(), 2u);
  EXPECT_FALSE(mp_seq_xy->hasZ());
  EXPECT_FALSE(mp_seq_xy->hasM());

  CoordinateSequence seq_xyz(
      std::initializer_list<Coordinate>{{1.0, 2.0, 3.0}, {4.0, 5.0, 6.0}});
  auto mp_seq_xyz = gf->createMultiPoint(seq_xyz);
  EXPECT_TRUE(mp_seq_xyz->hasZ());
  EXPECT_FALSE(mp_seq_xyz->hasM());
  EXPECT_DOUBLE_EQ(mp_seq_xyz->getGeometryN(1)->getZ(), 6.0);

  CoordinateSequence seq_xym(
      std::initializer_list<CoordinateXYM>{{1.0, 2.0, 9.0}});
  auto mp_seq_xym = gf->createMultiPoint(seq_xym);
  EXPECT_FALSE(mp_seq_xym->hasZ());
  EXPECT_TRUE(mp_seq_xym->hasM());
  EXPECT_DOUBLE_EQ(mp_seq_xym->getGeometryN(0)->getM(), 9.0);

  CoordinateSequence seq_xyzm(
      std::initializer_list<CoordinateXYZM>{{1.0, 2.0, 3.0, 4.0}});
  auto mp_seq_xyzm = gf->createMultiPoint(seq_xyzm);
  EXPECT_TRUE(mp_seq_xyzm->hasZ());
  EXPECT_TRUE(mp_seq_xyzm->hasM());
  EXPECT_DOUBLE_EQ(mp_seq_xyzm->getGeometryN(0)->getZ(), 3.0);
  EXPECT_DOUBLE_EQ(mp_seq_xyzm->getGeometryN(0)->getM(), 4.0);
}

TEST(GeometryFactoryTest, CreateMultiLineStringOverloads) {
  auto gf = GeometryFactory::create();

  auto mls_empty = gf->createMultiLineString();
  EXPECT_TRUE(mls_empty->isEmpty());
  EXPECT_EQ(mls_empty->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);

  std::vector<std::unique_ptr<LineString>> empty_typed_lines;
  EXPECT_TRUE(
      gf->createMultiLineString(std::move(empty_typed_lines))->isEmpty());

  std::vector<std::unique_ptr<LineString>> typed_lines;
  typed_lines.push_back(gf->createLineString(MakeLineSeq(0.0, 0.0, 3.0, 0.0)));
  typed_lines.push_back(gf->createLineString(MakeLineSeq(0.0, 1.0, 0.0, 5.0)));
  auto mls_typed = gf->createMultiLineString(std::move(typed_lines));
  EXPECT_EQ(mls_typed->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(mls_typed->getLength(), 7.0);

  std::vector<std::unique_ptr<Geometry>> empty_geom_lines;
  EXPECT_TRUE(
      gf->createMultiLineString(std::move(empty_geom_lines))->isEmpty());

  std::vector<std::unique_ptr<Geometry>> geom_lines;
  geom_lines.push_back(gf->createLineString(MakeLineSeq(0.0, 0.0, 1.0, 0.0)));
  auto mls_geom = gf->createMultiLineString(std::move(geom_lines));
  EXPECT_EQ(mls_geom->getNumGeometries(), 1u);

  // const vector<const Geometry*>& empty and valid deep copy (including
  // LinearRing, which is a LineString subclass).
  std::vector<const Geometry*> empty_raw;
  EXPECT_TRUE(gf->createMultiLineString(empty_raw)->isEmpty());

  auto l1 = gf->createLineString(MakeLineSeq(0.0, 0.0, 2.0, 0.0));
  auto l2 = gf->createLineString(MakeLineSeq(2.0, 0.0, 2.0, 3.0));
  auto lr_line = gf->createLinearRing(MakeBoxSeq(0.0, 0.0, 1.0, 1.0));
  std::vector<const Geometry*> raw_lines{l1.get(), l2.get(), lr_line.get()};
  auto mls_copy = gf->createMultiLineString(raw_lines);
  l1.reset();
  l2.reset();
  lr_line.reset();
  EXPECT_EQ(mls_copy->getNumGeometries(), 3u);
  EXPECT_DOUBLE_EQ(mls_copy->getLength(), 9.0);
  EXPECT_EQ(mls_copy->getGeometryN(2)->getGeometryTypeId(),
            geos::geom::GEOS_LINESTRING);

  // Non-LineString element or nullptr throws IllegalArgumentException.
  auto pt = gf->createPoint(CoordinateXY(0.0, 0.0));
  auto valid_line = gf->createLineString(MakeLineSeq(0.0, 0.0, 1.0, 1.0));
  std::vector<const Geometry*> mixed{valid_line.get(), pt.get()};
  EXPECT_THROW(gf->createMultiLineString(mixed), IllegalArgumentException);

  std::vector<const Geometry*> with_null{nullptr};
  EXPECT_THROW(gf->createMultiLineString(with_null), IllegalArgumentException);
}

TEST(GeometryFactoryTest, CreateMultiPolygonOverloads) {
  auto gf = GeometryFactory::create();

  auto mp_empty = gf->createMultiPolygon();
  EXPECT_TRUE(mp_empty->isEmpty());
  EXPECT_EQ(mp_empty->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);

  std::vector<std::unique_ptr<Polygon>> empty_typed_polys;
  EXPECT_TRUE(gf->createMultiPolygon(std::move(empty_typed_polys))->isEmpty());

  std::vector<std::unique_ptr<Polygon>> typed_polys;
  typed_polys.push_back(
      gf->createPolygon(gf->createLinearRing(MakeBoxSeq(0.0, 0.0, 2.0, 2.0))));
  typed_polys.push_back(
      gf->createPolygon(gf->createLinearRing(MakeBoxSeq(5.0, 5.0, 8.0, 8.0))));
  auto mp_typed = gf->createMultiPolygon(std::move(typed_polys));
  EXPECT_EQ(mp_typed->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(mp_typed->getArea(), 13.0);

  std::vector<std::unique_ptr<Geometry>> empty_geom_polys;
  EXPECT_TRUE(gf->createMultiPolygon(std::move(empty_geom_polys))->isEmpty());

  std::vector<std::unique_ptr<Geometry>> geom_polys;
  geom_polys.push_back(
      gf->createPolygon(gf->createLinearRing(MakeBoxSeq(0.0, 0.0, 1.0, 1.0))));
  auto mp_geom = gf->createMultiPolygon(std::move(geom_polys));
  EXPECT_EQ(mp_geom->getNumGeometries(), 1u);
  EXPECT_DOUBLE_EQ(mp_geom->getArea(), 1.0);

  std::vector<const Geometry*> empty_raw;
  EXPECT_TRUE(gf->createMultiPolygon(empty_raw)->isEmpty());

  std::vector<std::unique_ptr<LinearRing>> p1_holes;
  p1_holes.push_back(gf->createLinearRing(MakeBoxSeq(0.5, 0.5, 1.5, 1.5)));
  auto p1 =
      gf->createPolygon(gf->createLinearRing(MakeBoxSeq(0.0, 0.0, 2.0, 3.0)),
                        std::move(p1_holes));
  auto p2 =
      gf->createPolygon(gf->createLinearRing(MakeBoxSeq(4.0, 4.0, 6.0, 6.0)));
  std::vector<const Geometry*> raw_polys{p1.get(), p2.get()};
  auto mp_copy = gf->createMultiPolygon(raw_polys);
  p1.reset();
  p2.reset();
  EXPECT_EQ(mp_copy->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(mp_copy->getArea(), 9.0);
  EXPECT_EQ(mp_copy->getGeometryN(0)->getNumInteriorRing(), 1u);
}

TEST(GeometryFactoryTest, CreateMultiCurveAndMultiSurfaceOverloads) {
  auto gf = GeometryFactory::create();

  // MultiCurve.
  auto mc_empty = gf->createMultiCurve();
  EXPECT_TRUE(mc_empty->isEmpty());
  EXPECT_EQ(mc_empty->getGeometryTypeId(), geos::geom::GEOS_MULTICURVE);

  std::vector<std::unique_ptr<Curve>> curves;
  curves.push_back(gf->createLineString(MakeLineSeq(0.0, 0.0, 3.0, 0.0)));
  curves.push_back(gf->createCircularString(
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}})));
  auto mc_curves = gf->createMultiCurve(std::move(curves));
  EXPECT_EQ(mc_curves->getNumGeometries(), 2u);
  EXPECT_TRUE(
      static_cast<const Geometry*>(mc_curves.get())->hasCurvedComponents());

  std::vector<std::unique_ptr<Geometry>> mc_geoms;
  mc_geoms.push_back(gf->createLineString(MakeLineSeq(0.0, 0.0, 1.0, 0.0)));
  auto mc_from_geom = gf->createMultiCurve(std::move(mc_geoms));
  EXPECT_EQ(mc_from_geom->getNumGeometries(), 1u);
  EXPECT_FALSE(
      static_cast<const Geometry*>(mc_from_geom.get())->hasCurvedComponents());

  std::vector<std::unique_ptr<Geometry>> mc_invalid;
  mc_invalid.push_back(gf->createPoint(CoordinateXY(0.0, 0.0)));
  EXPECT_THROW(gf->createMultiCurve(std::move(mc_invalid)),
               IllegalArgumentException);

  // MultiSurface.
  auto ms_empty = gf->createMultiSurface();
  EXPECT_TRUE(ms_empty->isEmpty());
  EXPECT_EQ(ms_empty->getGeometryTypeId(), geos::geom::GEOS_MULTISURFACE);

  std::vector<std::unique_ptr<Surface>> surfaces;
  surfaces.push_back(
      gf->createPolygon(gf->createLinearRing(MakeBoxSeq(0.0, 0.0, 2.0, 2.0))));
  surfaces.push_back(gf->createCurvePolygon(
      gf->createCircularString(MakeCircleArcSeq(10.0, 0.0, 1.0))));
  auto ms_surfaces = gf->createMultiSurface(std::move(surfaces));
  EXPECT_EQ(ms_surfaces->getNumGeometries(), 2u);
  EXPECT_TRUE(
      static_cast<const Geometry*>(ms_surfaces.get())->hasCurvedComponents());

  std::vector<std::unique_ptr<Geometry>> ms_geoms;
  ms_geoms.push_back(
      gf->createPolygon(gf->createLinearRing(MakeBoxSeq(0.0, 0.0, 3.0, 3.0))));
  auto ms_from_geom = gf->createMultiSurface(std::move(ms_geoms));
  EXPECT_EQ(ms_from_geom->getNumGeometries(), 1u);
  EXPECT_DOUBLE_EQ(ms_from_geom->getArea(), 9.0);
  EXPECT_FALSE(
      static_cast<const Geometry*>(ms_from_geom.get())->hasCurvedComponents());

  std::vector<std::unique_ptr<Geometry>> ms_invalid;
  ms_invalid.push_back(gf->createPoint(CoordinateXY(0.0, 0.0)));
  EXPECT_THROW(gf->createMultiSurface(std::move(ms_invalid)),
               IllegalArgumentException);
}

TEST(GeometryFactoryTest, CreateGeometryCollectionOverloads) {
  auto gf = GeometryFactory::create();

  auto gc_empty = gf->createGeometryCollection();
  EXPECT_TRUE(gc_empty->isEmpty());
  EXPECT_EQ(gc_empty->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);

  // Templated unique_ptr<T> overload with Geometry and with Point subclass.
  std::vector<std::unique_ptr<Geometry>> mixed;
  mixed.push_back(gf->createPoint(CoordinateXY(1.0, 2.0)));
  mixed.push_back(gf->createLineString(MakeLineSeq(0.0, 0.0, 2.0, 0.0)));
  auto gc_mixed = gf->createGeometryCollection(std::move(mixed));
  EXPECT_EQ(gc_mixed->getNumGeometries(), 2u);
  EXPECT_EQ(gc_mixed->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_POINT);
  EXPECT_EQ(gc_mixed->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_LINESTRING);

  std::vector<std::unique_ptr<Point>> pt_vec;
  pt_vec.push_back(gf->createPoint(CoordinateXY(3.0, 4.0)));
  auto gc_pts = gf->createGeometryCollection(std::move(pt_vec));
  EXPECT_EQ(gc_pts->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(gc_pts->getNumGeometries(), 1u);

  // const vector<const Geometry*>& deep copy.
  std::vector<const Geometry*> empty_raw;
  EXPECT_TRUE(gf->createGeometryCollection(empty_raw)->isEmpty());

  auto p = gf->createPoint(CoordinateXY(5.0, 6.0));
  auto poly =
      gf->createPolygon(gf->createLinearRing(MakeBoxSeq(0.0, 0.0, 2.0, 2.0)));
  std::vector<const Geometry*> raw_geoms{p.get(), poly.get()};
  auto gc_copy = gf->createGeometryCollection(raw_geoms);
  p.reset();
  poly.reset();
  EXPECT_EQ(gc_copy->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(gc_copy->getArea(), 4.0);
}

TEST(GeometryFactoryTest, CreateEmptyAndCreateEmptyGeometry) {
  PrecisionModel pm;
  auto gf = GeometryFactory::create(&pm, 4326);

  // createEmpty(int dimension).
  auto e_coll = gf->createEmpty(-1);
  EXPECT_TRUE(e_coll->isEmpty());
  EXPECT_EQ(e_coll->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(e_coll->getSRID(), 4326);

  auto e_pt = gf->createEmpty(0);
  EXPECT_TRUE(e_pt->isEmpty());
  EXPECT_EQ(e_pt->getGeometryTypeId(), geos::geom::GEOS_POINT);

  auto e_ls = gf->createEmpty(1);
  EXPECT_TRUE(e_ls->isEmpty());
  EXPECT_EQ(e_ls->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);

  auto e_poly = gf->createEmpty(2);
  EXPECT_TRUE(e_poly->isEmpty());
  EXPECT_EQ(e_poly->getGeometryTypeId(), geos::geom::GEOS_POLYGON);

  EXPECT_THROW(gf->createEmpty(-2), IllegalArgumentException);
  EXPECT_THROW(gf->createEmpty(3), IllegalArgumentException);

  // createEmpty(GeometryTypeId).
  EXPECT_EQ(gf->createEmpty(geos::geom::GEOS_POINT)->getGeometryTypeId(),
            geos::geom::GEOS_POINT);
  EXPECT_EQ(gf->createEmpty(geos::geom::GEOS_LINESTRING)->getGeometryTypeId(),
            geos::geom::GEOS_LINESTRING);
  EXPECT_EQ(gf->createEmpty(geos::geom::GEOS_POLYGON)->getGeometryTypeId(),
            geos::geom::GEOS_POLYGON);
  EXPECT_EQ(gf->createEmpty(geos::geom::GEOS_MULTIPOINT)->getGeometryTypeId(),
            geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(
      gf->createEmpty(geos::geom::GEOS_MULTILINESTRING)->getGeometryTypeId(),
      geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(gf->createEmpty(geos::geom::GEOS_MULTIPOLYGON)->getGeometryTypeId(),
            geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(
      gf->createEmpty(geos::geom::GEOS_GEOMETRYCOLLECTION)->getGeometryTypeId(),
      geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_THROW(gf->createEmpty(geos::geom::GEOS_LINEARRING),
               IllegalArgumentException);
  EXPECT_THROW(gf->createEmpty(geos::geom::GEOS_CIRCULARSTRING),
               IllegalArgumentException);
  EXPECT_THROW(gf->createEmpty(geos::geom::GEOS_COMPOUNDCURVE),
               IllegalArgumentException);
  EXPECT_THROW(gf->createEmpty(geos::geom::GEOS_CURVEPOLYGON),
               IllegalArgumentException);
  EXPECT_THROW(gf->createEmpty(geos::geom::GEOS_MULTICURVE),
               IllegalArgumentException);
  EXPECT_THROW(gf->createEmpty(geos::geom::GEOS_MULTISURFACE),
               IllegalArgumentException);

  // createEmptyGeometry(type, hasZ, hasM).
  auto def_empty = gf->createEmptyGeometry();
  EXPECT_TRUE(def_empty->isEmpty());
  EXPECT_EQ(def_empty->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);

  const GeometryTypeId all_types[] = {
      geos::geom::GEOS_POINT,          geos::geom::GEOS_LINESTRING,
      geos::geom::GEOS_LINEARRING,     geos::geom::GEOS_POLYGON,
      geos::geom::GEOS_MULTIPOINT,     geos::geom::GEOS_MULTILINESTRING,
      geos::geom::GEOS_MULTIPOLYGON,   geos::geom::GEOS_GEOMETRYCOLLECTION,
      geos::geom::GEOS_CIRCULARSTRING, geos::geom::GEOS_COMPOUNDCURVE,
      geos::geom::GEOS_CURVEPOLYGON,   geos::geom::GEOS_MULTICURVE,
      geos::geom::GEOS_MULTISURFACE,
  };
  for (GeometryTypeId t : all_types) {
    auto g = gf->createEmptyGeometry(t, false, false);
    EXPECT_TRUE(g->isEmpty());
    EXPECT_EQ(g->getGeometryTypeId(), t);
    EXPECT_EQ(g->getSRID(), 4326);
  }

  // Z/M propagation for coordinate-bearing empty geometry types.
  const GeometryTypeId zm_types[] = {
      geos::geom::GEOS_POINT,          geos::geom::GEOS_LINESTRING,
      geos::geom::GEOS_LINEARRING,     geos::geom::GEOS_POLYGON,
      geos::geom::GEOS_CIRCULARSTRING, geos::geom::GEOS_CURVEPOLYGON,
  };
  for (GeometryTypeId t : zm_types) {
    for (bool hasZ : {false, true}) {
      for (bool hasM : {false, true}) {
        auto g = gf->createEmptyGeometry(t, hasZ, hasM);
        EXPECT_EQ(g->getGeometryTypeId(), t);
        EXPECT_TRUE(g->isEmpty());
        EXPECT_EQ(g->hasZ(), hasZ);
        EXPECT_EQ(g->hasM(), hasM);
      }
    }
  }
}

TEST(GeometryFactoryTest, CreateMulti) {
  auto gf = GeometryFactory::create();
  PrecisionModel pm;
  auto other_gf = GeometryFactory::create(&pm, 3857);

  // Already a collection -> returned unchanged.
  auto mp_orig = gf->createMultiPoint();
  const Geometry* mp_orig_ptr = mp_orig.get();
  auto mp_res = gf->createMulti(std::move(mp_orig));
  EXPECT_EQ(mp_res.get(), mp_orig_ptr);

  auto mls_orig = gf->createMultiLineString();
  const Geometry* mls_orig_ptr = mls_orig.get();
  EXPECT_EQ(gf->createMulti(std::move(mls_orig)).get(), mls_orig_ptr);

  auto mpoly_orig = gf->createMultiPolygon();
  const Geometry* mpoly_orig_ptr = mpoly_orig.get();
  EXPECT_EQ(gf->createMulti(std::move(mpoly_orig)).get(), mpoly_orig_ptr);

  auto gc_orig = gf->createGeometryCollection();
  const Geometry* gc_orig_ptr = gc_orig.get();
  EXPECT_EQ(gf->createMulti(std::move(gc_orig)).get(), gc_orig_ptr);

  // Empty atomic geometries -> corresponding empty multi geometries.
  auto empty_mp = gf->createMulti(other_gf->createPoint());
  EXPECT_TRUE(empty_mp->isEmpty());
  EXPECT_EQ(empty_mp->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(empty_mp->getFactory(), other_gf.get());
  EXPECT_EQ(empty_mp->getSRID(), 3857);

  auto empty_mls = gf->createMulti(gf->createLineString());
  EXPECT_TRUE(empty_mls->isEmpty());
  EXPECT_EQ(empty_mls->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);

  auto empty_mpoly = gf->createMulti(gf->createPolygon());
  EXPECT_TRUE(empty_mpoly->isEmpty());
  EXPECT_EQ(empty_mpoly->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);

  // Non-empty atomic geometries -> single-element multi geometries.
  auto single_mp =
      gf->createMulti(other_gf->createPoint(CoordinateXY(1.0, 2.0)));
  EXPECT_EQ(single_mp->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(single_mp->getNumGeometries(), 1u);
  EXPECT_EQ(single_mp->getFactory(), other_gf.get());
  EXPECT_EQ(single_mp->getSRID(), 3857);

  auto single_mls =
      gf->createMulti(gf->createLineString(MakeLineSeq(0.0, 0.0, 2.0, 0.0)));
  EXPECT_EQ(single_mls->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(single_mls->getNumGeometries(), 1u);
  EXPECT_DOUBLE_EQ(single_mls->getLength(), 2.0);

  auto single_mpoly = gf->createMulti(
      gf->createPolygon(gf->createLinearRing(MakeBoxSeq(0.0, 0.0, 3.0, 3.0))));
  EXPECT_EQ(single_mpoly->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(single_mpoly->getNumGeometries(), 1u);
  EXPECT_DOUBLE_EQ(single_mpoly->getArea(), 9.0);

  // Unsupported types throw IllegalArgumentException.
  EXPECT_THROW(
      gf->createMulti(gf->createLinearRing(MakeBoxSeq(0.0, 0.0, 1.0, 1.0))),
      IllegalArgumentException);
  EXPECT_THROW(gf->createMulti(gf->createLinearRing()),
               IllegalArgumentException);
  EXPECT_THROW(gf->createMulti(
                   gf->createCircularString(MakeCircleArcSeq(0.0, 0.0, 1.0))),
               IllegalArgumentException);
}

TEST(GeometryFactoryTest, BuildGeometryVectorUniquePtrOverloads) {
  auto gf = GeometryFactory::create();

  // vector<unique_ptr<Geometry>>&&: empty, single, homogeneous, heterogeneous.
  std::vector<std::unique_ptr<Geometry>> empty_vec;
  auto b_empty = gf->buildGeometry(std::move(empty_vec));
  EXPECT_TRUE(b_empty->isEmpty());
  EXPECT_EQ(b_empty->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);

  std::vector<std::unique_ptr<Geometry>> single_vec;
  single_vec.push_back(gf->createPoint(CoordinateXY(2.0, 3.0)));
  auto b_single = gf->buildGeometry(std::move(single_vec));
  EXPECT_EQ(b_single->getGeometryTypeId(), geos::geom::GEOS_POINT);

  std::vector<std::unique_ptr<Geometry>> pts;
  pts.push_back(gf->createPoint(CoordinateXY(0.0, 0.0)));
  pts.push_back(gf->createPoint(CoordinateXY(1.0, 1.0)));
  auto b_pts = gf->buildGeometry(std::move(pts));
  EXPECT_EQ(b_pts->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(b_pts->getNumGeometries(), 2u);

  std::vector<std::unique_ptr<Geometry>> lines;
  lines.push_back(gf->createLineString(MakeLineSeq(0.0, 0.0, 1.0, 0.0)));
  lines.push_back(gf->createLineString(MakeLineSeq(0.0, 1.0, 1.0, 1.0)));
  auto b_lines = gf->buildGeometry(std::move(lines));
  EXPECT_EQ(b_lines->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(b_lines->getNumGeometries(), 2u);

  std::vector<std::unique_ptr<Geometry>> rings;
  rings.push_back(gf->createLinearRing(MakeBoxSeq(0.0, 0.0, 1.0, 1.0)));
  rings.push_back(gf->createLinearRing(MakeBoxSeq(2.0, 2.0, 3.0, 3.0)));
  auto b_rings = gf->buildGeometry(std::move(rings));
  EXPECT_EQ(b_rings->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(b_rings->getNumGeometries(), 2u);

  std::vector<std::unique_ptr<Geometry>> polys;
  polys.push_back(
      gf->createPolygon(gf->createLinearRing(MakeBoxSeq(0.0, 0.0, 1.0, 1.0))));
  polys.push_back(
      gf->createPolygon(gf->createLinearRing(MakeBoxSeq(2.0, 2.0, 4.0, 4.0))));
  auto b_polys = gf->buildGeometry(std::move(polys));
  EXPECT_EQ(b_polys->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(b_polys->getNumGeometries(), 2u);

  std::vector<std::unique_ptr<Geometry>> hetero;
  hetero.push_back(gf->createPoint(CoordinateXY(0.0, 0.0)));
  hetero.push_back(gf->createLineString(MakeLineSeq(0.0, 0.0, 1.0, 1.0)));
  auto b_hetero = gf->buildGeometry(std::move(hetero));
  EXPECT_EQ(b_hetero->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(b_hetero->getNumGeometries(), 2u);

  // Homogeneous MultiPoints -> commonType default -> GeometryCollection.
  std::vector<std::unique_ptr<Geometry>> multi_pts;
  multi_pts.push_back(gf->createMultiPoint());
  multi_pts.push_back(gf->createMultiPoint());
  auto b_multi_pts = gf->buildGeometry(std::move(multi_pts));
  EXPECT_EQ(b_multi_pts->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(b_multi_pts->getNumGeometries(), 2u);

  // Typed vector<unique_ptr<Point>>&&.
  std::vector<std::unique_ptr<Point>> pt_empty;
  EXPECT_EQ(gf->buildGeometry(std::move(pt_empty))->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  std::vector<std::unique_ptr<Point>> pt_one;
  pt_one.push_back(gf->createPoint(CoordinateXY(1.0, 2.0)));
  EXPECT_EQ(gf->buildGeometry(std::move(pt_one))->getGeometryTypeId(),
            geos::geom::GEOS_POINT);
  std::vector<std::unique_ptr<Point>> pt_two;
  pt_two.push_back(gf->createPoint(CoordinateXY(1.0, 2.0)));
  pt_two.push_back(gf->createPoint(CoordinateXY(3.0, 4.0)));
  auto b_pt_two = gf->buildGeometry(std::move(pt_two));
  EXPECT_EQ(b_pt_two->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(b_pt_two->getNumGeometries(), 2u);

  // Typed vector<unique_ptr<LineString>>&&.
  std::vector<std::unique_ptr<LineString>> ls_empty;
  EXPECT_EQ(gf->buildGeometry(std::move(ls_empty))->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  std::vector<std::unique_ptr<LineString>> ls_one;
  ls_one.push_back(gf->createLineString(MakeLineSeq(0.0, 0.0, 1.0, 1.0)));
  EXPECT_EQ(gf->buildGeometry(std::move(ls_one))->getGeometryTypeId(),
            geos::geom::GEOS_LINESTRING);
  std::vector<std::unique_ptr<LineString>> ls_two;
  ls_two.push_back(gf->createLineString(MakeLineSeq(0.0, 0.0, 1.0, 0.0)));
  ls_two.push_back(gf->createLineString(MakeLineSeq(0.0, 1.0, 1.0, 1.0)));
  auto b_ls_two = gf->buildGeometry(std::move(ls_two));
  EXPECT_EQ(b_ls_two->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(b_ls_two->getNumGeometries(), 2u);

  // Typed vector<unique_ptr<Polygon>>&&.
  std::vector<std::unique_ptr<Polygon>> poly_empty;
  EXPECT_EQ(gf->buildGeometry(std::move(poly_empty))->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  std::vector<std::unique_ptr<Polygon>> poly_one;
  poly_one.push_back(
      gf->createPolygon(gf->createLinearRing(MakeBoxSeq(0.0, 0.0, 1.0, 1.0))));
  EXPECT_EQ(gf->buildGeometry(std::move(poly_one))->getGeometryTypeId(),
            geos::geom::GEOS_POLYGON);
  std::vector<std::unique_ptr<Polygon>> poly_two;
  poly_two.push_back(
      gf->createPolygon(gf->createLinearRing(MakeBoxSeq(0.0, 0.0, 1.0, 1.0))));
  poly_two.push_back(
      gf->createPolygon(gf->createLinearRing(MakeBoxSeq(2.0, 2.0, 3.0, 3.0))));
  auto b_poly_two = gf->buildGeometry(std::move(poly_two));
  EXPECT_EQ(b_poly_two->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(b_poly_two->getNumGeometries(), 2u);
}

TEST(GeometryFactoryTest, BuildGeometryRawVectorAndIteratorOverloads) {
  auto gf = GeometryFactory::create();
  auto pt1 = gf->createPoint(CoordinateXY(0.0, 0.0));
  auto pt2 = gf->createPoint(CoordinateXY(1.0, 1.0));
  auto ls1 = gf->createLineString(MakeLineSeq(0.0, 0.0, 2.0, 0.0));
  auto ls2 = gf->createLineString(MakeLineSeq(0.0, 1.0, 2.0, 1.0));
  auto lr1 = gf->createLinearRing(MakeBoxSeq(0.0, 0.0, 1.0, 1.0));
  auto lr2 = gf->createLinearRing(MakeBoxSeq(2.0, 2.0, 3.0, 3.0));
  auto poly1 =
      gf->createPolygon(gf->createLinearRing(MakeBoxSeq(0.0, 0.0, 2.0, 2.0)));
  auto poly2 =
      gf->createPolygon(gf->createLinearRing(MakeBoxSeq(3.0, 3.0, 5.0, 5.0)));
  auto mp1 = gf->createMultiPoint();
  auto mp2 = gf->createMultiPoint();
  auto gc1 = gf->createGeometryCollection();
  auto gc2 = gf->createGeometryCollection();

  // buildGeometry(const vector<const Geometry*>&).
  std::vector<const Geometry*> raw_empty;
  EXPECT_TRUE(gf->buildGeometry(raw_empty)->isEmpty());
  EXPECT_EQ(gf->buildGeometry(raw_empty)->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);

  std::vector<const Geometry*> raw_one{pt1.get()};
  auto b_raw_one = gf->buildGeometry(raw_one);
  EXPECT_EQ(b_raw_one->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_NE(b_raw_one.get(), pt1.get());
  EXPECT_TRUE(b_raw_one->equalsExact(pt1.get()));

  std::vector<const Geometry*> raw_pts{pt1.get(), pt2.get()};
  EXPECT_EQ(gf->buildGeometry(raw_pts)->getGeometryTypeId(),
            geos::geom::GEOS_MULTIPOINT);

  std::vector<const Geometry*> raw_lines{ls1.get(), ls2.get()};
  EXPECT_EQ(gf->buildGeometry(raw_lines)->getGeometryTypeId(),
            geos::geom::GEOS_MULTILINESTRING);

  std::vector<const Geometry*> raw_rings{lr1.get(), lr2.get()};
  EXPECT_EQ(gf->buildGeometry(raw_rings)->getGeometryTypeId(),
            geos::geom::GEOS_MULTILINESTRING);

  std::vector<const Geometry*> raw_polys{poly1.get(), poly2.get()};
  EXPECT_EQ(gf->buildGeometry(raw_polys)->getGeometryTypeId(),
            geos::geom::GEOS_MULTIPOLYGON);

  std::vector<const Geometry*> raw_hetero{pt1.get(), poly1.get()};
  EXPECT_EQ(gf->buildGeometry(raw_hetero)->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);

  std::vector<const Geometry*> raw_multis{mp1.get(), mp2.get()};
  EXPECT_EQ(gf->buildGeometry(raw_multis)->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);

  // Templated iterator buildGeometry(T from, T toofar).
  auto t_empty = gf->buildGeometry(raw_empty.cbegin(), raw_empty.cend());
  EXPECT_TRUE(t_empty->isEmpty());
  EXPECT_EQ(t_empty->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);

  auto t_one = gf->buildGeometry(raw_one.cbegin(), raw_one.cend());
  EXPECT_EQ(t_one->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_NE(t_one.get(), pt1.get());

  auto t_pts = gf->buildGeometry(raw_pts.cbegin(), raw_pts.cend());
  EXPECT_EQ(t_pts->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(t_pts->getNumGeometries(), 2u);

  auto t_lines = gf->buildGeometry(raw_lines.cbegin(), raw_lines.cend());
  EXPECT_EQ(t_lines->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(t_lines->getNumGeometries(), 2u);

  auto t_polys = gf->buildGeometry(raw_polys.cbegin(), raw_polys.cend());
  EXPECT_EQ(t_polys->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(t_polys->getNumGeometries(), 2u);

  auto t_hetero = gf->buildGeometry(raw_hetero.cbegin(), raw_hetero.cend());
  EXPECT_EQ(t_hetero->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(t_hetero->getNumGeometries(), 2u);

  // Two empty GeometryCollections have Dimension::False -> throws.
  std::vector<const Geometry*> raw_empty_gcs{gc1.get(), gc2.get()};
  EXPECT_THROW(gf->buildGeometry(raw_empty_gcs.cbegin(), raw_empty_gcs.cend()),
               IllegalArgumentException);
}

TEST(GeometryFactoryTest, CreateGeometryAcrossFactories) {
  auto src_gf = GeometryFactory::create();
  PrecisionModel dst_pm(10.0);
  auto dst_gf = GeometryFactory::create(&dst_pm, 4326);

  // Point (non-empty and empty). Note createGeometry does not round coords.
  auto pt = src_gf->createPoint(Coordinate(1.234, 5.678, 9.0));
  auto pt_dst = dst_gf->createGeometry(pt.get());
  EXPECT_EQ(pt_dst->getFactory(), dst_gf.get());
  EXPECT_EQ(pt_dst->getSRID(), 4326);
  EXPECT_TRUE(pt_dst->equalsExact(pt.get()));

  auto pt_empty_dst = dst_gf->createGeometry(src_gf->createPoint().get());
  EXPECT_TRUE(pt_empty_dst->isEmpty());
  EXPECT_EQ(pt_empty_dst->getFactory(), dst_gf.get());
  EXPECT_EQ(pt_empty_dst->getSRID(), 4326);

  // LineString and LinearRing.
  auto ls = src_gf->createLineString(MakeLineSeq(0.0, 0.0, 3.0, 4.0));
  auto ls_dst = dst_gf->createGeometry(ls.get());
  EXPECT_EQ(ls_dst->getFactory(), dst_gf.get());
  EXPECT_EQ(ls_dst->getSRID(), 4326);
  EXPECT_TRUE(ls_dst->equalsExact(ls.get()));

  auto lr = src_gf->createLinearRing(MakeBoxSeq(0.0, 0.0, 2.0, 2.0));
  auto lr_dst = dst_gf->createGeometry(lr.get());
  EXPECT_EQ(lr_dst->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  EXPECT_EQ(lr_dst->getFactory(), dst_gf.get());
  EXPECT_EQ(lr_dst->getSRID(), 4326);
  EXPECT_TRUE(lr_dst->equalsExact(lr.get()));

  // Polygon (empty and with hole).
  auto poly_empty_dst = dst_gf->createGeometry(src_gf->createPolygon().get());
  EXPECT_TRUE(poly_empty_dst->isEmpty());
  EXPECT_EQ(poly_empty_dst->getFactory(), dst_gf.get());
  EXPECT_EQ(poly_empty_dst->getSRID(), 4326);

  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(src_gf->createLinearRing(MakeBoxSeq(1.0, 1.0, 2.0, 2.0)));
  auto poly = src_gf->createPolygon(
      src_gf->createLinearRing(MakeBoxSeq(0.0, 0.0, 5.0, 5.0)),
      std::move(holes));
  auto poly_dst = dst_gf->createGeometry(poly.get());
  EXPECT_EQ(poly_dst->getFactory(), dst_gf.get());
  EXPECT_EQ(poly_dst->getSRID(), 4326);
  EXPECT_TRUE(poly_dst->equalsExact(poly.get()));

  // MultiPoint, MultiLineString, MultiPolygon, GeometryCollection.
  auto mp = src_gf->createMultiPoint(
      std::vector<CoordinateXY>{{1.0, 2.0}, {3.0, 4.0}});
  auto mp_dst = dst_gf->createGeometry(mp.get());
  EXPECT_EQ(mp_dst->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(mp_dst->getFactory(), dst_gf.get());
  EXPECT_EQ(mp_dst->getSRID(), 4326);
  EXPECT_TRUE(mp_dst->equalsExact(mp.get()));

  std::vector<std::unique_ptr<LineString>> mls_elems;
  mls_elems.push_back(
      src_gf->createLineString(MakeLineSeq(0.0, 0.0, 2.0, 0.0)));
  mls_elems.push_back(
      src_gf->createLineString(MakeLineSeq(0.0, 1.0, 2.0, 1.0)));
  auto mls = src_gf->createMultiLineString(std::move(mls_elems));
  auto mls_dst = dst_gf->createGeometry(mls.get());
  EXPECT_EQ(mls_dst->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(mls_dst->getFactory(), dst_gf.get());
  EXPECT_EQ(mls_dst->getSRID(), 4326);
  EXPECT_TRUE(mls_dst->equalsExact(mls.get()));

  std::vector<std::unique_ptr<Polygon>> mpoly_elems;
  mpoly_elems.push_back(src_gf->createPolygon(
      src_gf->createLinearRing(MakeBoxSeq(0.0, 0.0, 2.0, 2.0))));
  auto mpoly = src_gf->createMultiPolygon(std::move(mpoly_elems));
  auto mpoly_dst = dst_gf->createGeometry(mpoly.get());
  EXPECT_EQ(mpoly_dst->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(mpoly_dst->getFactory(), dst_gf.get());
  EXPECT_EQ(mpoly_dst->getSRID(), 4326);
  EXPECT_TRUE(mpoly_dst->equalsExact(mpoly.get()));

  std::vector<std::unique_ptr<Geometry>> gc_elems;
  gc_elems.push_back(src_gf->createPoint(CoordinateXY(7.0, 8.0)));
  gc_elems.push_back(src_gf->createLineString(MakeLineSeq(0.0, 0.0, 1.0, 1.0)));
  auto gc = src_gf->createGeometryCollection(std::move(gc_elems));
  auto gc_dst = dst_gf->createGeometry(gc.get());
  EXPECT_EQ(gc_dst->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(gc_dst->getFactory(), dst_gf.get());
  EXPECT_EQ(gc_dst->getSRID(), 4326);
  EXPECT_TRUE(gc_dst->equalsExact(gc.get()));

  // Same-factory and default-factory createGeometry conversions.
  auto pt_same = dst_gf->createGeometry(pt_dst.get());
  EXPECT_NE(pt_same.get(), pt_dst.get());
  EXPECT_EQ(pt_same->getFactory(), dst_gf.get());
  EXPECT_EQ(pt_same->getSRID(), 4326);
  EXPECT_TRUE(pt_same->equalsExact(pt_dst.get()));

  auto def_pt = GeometryFactory::getDefaultInstance()->createPoint(
      CoordinateXY(2.0, 3.0));
  auto def_pt_dst = dst_gf->createGeometry(def_pt.get());
  EXPECT_EQ(def_pt_dst->getFactory(), dst_gf.get());
  EXPECT_EQ(def_pt_dst->getSRID(), 4326);
  EXPECT_TRUE(def_pt_dst->equalsExact(def_pt.get()));

  // Curved geometry throws UnsupportedOperationException via GeometryEditor.
  auto circ = src_gf->createCircularString(MakeCircleArcSeq(0.0, 0.0, 1.0));
  EXPECT_THROW(dst_gf->createGeometry(circ.get()),
               UnsupportedOperationException);
}

}  // namespace
}  // namespace geos_google
