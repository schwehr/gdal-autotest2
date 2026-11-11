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

// Unit tests for third_party/geos/src/geom/Surface.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/Surface.cpp

#include "third_party/geos/include/geos/geom/Surface.h"

#include <cstddef>
#include <initializer_list>
#include <limits>
#include <memory>
#include <numbers>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateFilter.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/CoordinateSequenceFilter.h"
#include "third_party/geos/include/geos/geom/Curve.h"
#include "third_party/geos/include/geos/geom/CurvePolygon.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryComponentFilter.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/GeometryFilter.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::geom::CircularString;
using geos::geom::Coordinate;
using geos::geom::CoordinateInspector;
using geos::geom::CoordinateMutator;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateSequenceFilter;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Curve;
using geos::geom::CurvePolygon;
using geos::geom::Dimension;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryComponentFilter;
using geos::geom::GeometryFactory;
using geos::geom::GeometryFilter;
using geos::geom::LinearRing;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::Surface;
using geos::util::UnsupportedOperationException;
using ::testing::ElementsAre;
using ::testing::SizeIs;

std::unique_ptr<LinearRing> MakeSquareRingXY(const GeometryFactory& factory,
                                             double min_x, double min_y,
                                             double max_x, double max_y) {
  auto seq =
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {min_x, min_y},
          {max_x, min_y},
          {max_x, max_y},
          {min_x, max_y},
          {min_x, min_y},
      });
  return factory.createLinearRing(std::move(seq));
}

template <typename CoordT>
std::unique_ptr<LinearRing> MakeRing(const GeometryFactory& factory,
                                     std::initializer_list<CoordT> coords) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  return factory.createLinearRing(std::move(seq));
}

std::unique_ptr<Polygon> MakeSquareWithTwoHoles(
    const GeometryFactory& factory) {
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(MakeSquareRingXY(factory, 1.0, 1.0, 2.0, 2.0));
  holes.push_back(MakeSquareRingXY(factory, 4.0, 4.0, 6.0, 6.0));
  return factory.createPolygon(MakeSquareRingXY(factory, 0.0, 0.0, 10.0, 10.0),
                               std::move(holes));
}

std::unique_ptr<CircularString> MakeCircleCurve(const GeometryFactory& factory,
                                                double radius) {
  auto seq =
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {radius, radius},
          {-radius, radius},
          {-radius, -radius},
          {radius, -radius},
          {radius, radius},
      });
  return factory.createCircularString(std::move(seq));
}

class RecordingCoordFilter : public CoordinateInspector<RecordingCoordFilter> {
 public:
  void filter(const CoordinateXY* coord) { coords_.push_back(*coord); }
  const std::vector<CoordinateXY>& coords() const { return coords_; }

 private:
  std::vector<CoordinateXY> coords_;
};

class TranslatingCoordFilter
    : public CoordinateMutator<TranslatingCoordFilter> {
 public:
  TranslatingCoordFilter(double dx, double dy) : dx_(dx), dy_(dy) {}
  void filter(CoordinateXY* coord) const {
    coord->x += dx_;
    coord->y += dy_;
  }

 private:
  double dx_;
  double dy_;
};

class RecordingCoordSeqFilter : public CoordinateSequenceFilter {
 public:
  RecordingCoordSeqFilter(std::size_t stop_after, bool geometry_changed,
                          double dx, double dy)
      : stop_after_(stop_after),
        geometry_changed_(geometry_changed),
        dx_(dx),
        dy_(dy) {}

  void filter_ro(const CoordinateSequence& seq, std::size_t i) override {
    visited_.push_back(seq.getAt<CoordinateXY>(i));
  }

  void filter_rw(CoordinateSequence& seq, std::size_t i) override {
    CoordinateXY c = seq.getAt<CoordinateXY>(i);
    visited_.push_back(c);
    if (geometry_changed_) {
      seq.setOrdinate(i, CoordinateSequence::X, c.x + dx_);
      seq.setOrdinate(i, CoordinateSequence::Y, c.y + dy_);
    }
  }

  bool isDone() const override { return visited_.size() >= stop_after_; }
  bool isGeometryChanged() const override { return geometry_changed_; }

  const std::vector<CoordinateXY>& visited() const { return visited_; }

 private:
  std::size_t stop_after_;
  bool geometry_changed_;
  double dx_;
  double dy_;
  std::vector<CoordinateXY> visited_;
};

class RecordingComponentFilter : public GeometryComponentFilter {
 public:
  explicit RecordingComponentFilter(std::size_t stop_after)
      : stop_after_(stop_after) {}

  void filter_ro(const Geometry* geom) override { ro_geoms_.push_back(geom); }
  void filter_rw(Geometry* geom) override { rw_geoms_.push_back(geom); }

  bool isDone() override {
    return (ro_geoms_.size() + rw_geoms_.size()) >= stop_after_;
  }

  const std::vector<const Geometry*>& ro_geoms() const { return ro_geoms_; }
  const std::vector<Geometry*>& rw_geoms() const { return rw_geoms_; }

 private:
  std::size_t stop_after_;
  std::vector<const Geometry*> ro_geoms_;
  std::vector<Geometry*> rw_geoms_;
};

class RecordingGeometryFilter : public GeometryFilter {
 public:
  void filter_ro(const Geometry* geom) override { ro_geoms_.push_back(geom); }
  void filter_rw(Geometry* geom) override { rw_geoms_.push_back(geom); }

  const std::vector<const Geometry*>& ro_geoms() const { return ro_geoms_; }
  const std::vector<Geometry*>& rw_geoms() const { return rw_geoms_; }

 private:
  std::vector<const Geometry*> ro_geoms_;
  std::vector<Geometry*> rw_geoms_;
};

TEST(SurfaceTest, DimensionsAndEmptyRingConstruction) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<Polygon> empty_poly =
      factory->createPolygon(std::unique_ptr<LinearRing>(nullptr));
  const Surface* poly_surface = empty_poly.get();
  EXPECT_EQ(poly_surface->getDimension(), Dimension::A);
  EXPECT_EQ(poly_surface->getBoundaryDimension(), 1);
  EXPECT_TRUE(poly_surface->isEmpty());
  EXPECT_EQ(poly_surface->getNumInteriorRing(), 0U);
  ASSERT_NE(poly_surface->getExteriorRing(), nullptr);
  EXPECT_TRUE(poly_surface->getExteriorRing()->isEmpty());
  EXPECT_EQ(poly_surface->getCoordinate(), nullptr);
  EXPECT_EQ(poly_surface->getCoordinateDimension(), 2);
  EXPECT_TRUE(poly_surface->getEnvelopeInternal()->isNull());
  EXPECT_DOUBLE_EQ(poly_surface->getLength(), 0.0);
  EXPECT_EQ(poly_surface->getNumPoints(), 0U);
  EXPECT_FALSE(poly_surface->hasZ());
  EXPECT_FALSE(poly_surface->hasM());

  std::vector<std::unique_ptr<LinearRing>> no_holes;
  std::unique_ptr<Polygon> empty_with_holes_ctor = factory->createPolygon(
      std::unique_ptr<LinearRing>(nullptr), std::move(no_holes));
  EXPECT_TRUE(empty_with_holes_ctor->isEmpty());

  std::unique_ptr<CurvePolygon> empty_curve_poly =
      factory->createCurvePolygon(std::unique_ptr<Curve>(nullptr));
  const Surface* curve_surface = empty_curve_poly.get();
  EXPECT_EQ(curve_surface->getDimension(), Dimension::A);
  EXPECT_EQ(curve_surface->getBoundaryDimension(), 1);
  EXPECT_TRUE(curve_surface->isEmpty());
  ASSERT_NE(curve_surface->getExteriorRing(), nullptr);
  EXPECT_TRUE(curve_surface->getExteriorRing()->isEmpty());
}

TEST(SurfaceTest, ApplyReadOnlyCoordinateFilter) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<Polygon> empty = factory->createPolygon();
  RecordingCoordFilter empty_filter;
  empty->apply_ro(&empty_filter);
  EXPECT_TRUE(empty_filter.coords().empty());

  std::unique_ptr<Polygon> poly_no_holes =
      factory->createPolygon(MakeSquareRingXY(*factory, 0.0, 0.0, 2.0, 2.0));
  RecordingCoordFilter no_hole_filter;
  poly_no_holes->apply_ro(&no_hole_filter);
  EXPECT_THAT(no_hole_filter.coords(), SizeIs(5));

  std::unique_ptr<Polygon> poly_with_holes = MakeSquareWithTwoHoles(*factory);
  RecordingCoordFilter holes_filter;
  poly_with_holes->apply_ro(&holes_filter);
  ASSERT_THAT(holes_filter.coords(), SizeIs(15));
  EXPECT_EQ(holes_filter.coords().front(), CoordinateXY(0.0, 0.0));
  EXPECT_EQ(holes_filter.coords()[5], CoordinateXY(1.0, 1.0));
  EXPECT_EQ(holes_filter.coords()[10], CoordinateXY(4.0, 4.0));
}

TEST(SurfaceTest, ApplyReadWriteCoordinateFilter) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> poly = MakeSquareWithTwoHoles(*factory);

  TranslatingCoordFilter translate(10.0, 20.0);
  poly->apply_rw(&translate);
  poly->geometryChanged();

  EXPECT_EQ(*poly->getCoordinate(), CoordinateXY(10.0, 20.0));
  const Envelope* env = poly->getEnvelopeInternal();
  EXPECT_DOUBLE_EQ(env->getMinX(), 10.0);
  EXPECT_DOUBLE_EQ(env->getMinY(), 20.0);
  EXPECT_DOUBLE_EQ(env->getMaxX(), 20.0);
  EXPECT_DOUBLE_EQ(env->getMaxY(), 30.0);

  const CoordinateXY* hole0_coord = poly->getInteriorRingN(0)->getCoordinate();
  ASSERT_NE(hole0_coord, nullptr);
  EXPECT_EQ(*hole0_coord, CoordinateXY(11.0, 21.0));

  const CoordinateXY* hole1_coord = poly->getInteriorRingN(1)->getCoordinate();
  ASSERT_NE(hole1_coord, nullptr);
  EXPECT_EQ(*hole1_coord, CoordinateXY(14.0, 24.0));
}

TEST(SurfaceTest, ApplyReadOnlyCoordinateSequenceFilter) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> poly = MakeSquareWithTwoHoles(*factory);

  RecordingCoordSeqFilter all_filter(std::numeric_limits<std::size_t>::max(),
                                     false, 0.0, 0.0);
  poly->apply_ro(all_filter);
  EXPECT_THAT(all_filter.visited(), SizeIs(15));

  // Stop inside exterior ring -> skips both holes.
  RecordingCoordSeqFilter stop_in_shell(3, false, 0.0, 0.0);
  poly->apply_ro(stop_in_shell);
  EXPECT_THAT(stop_in_shell.visited(), SizeIs(3));

  // Stop inside first hole -> visits shell (5) + 2 coords of hole 0, skips
  // hole 1.
  RecordingCoordSeqFilter stop_in_hole0(7, false, 0.0, 0.0);
  poly->apply_ro(stop_in_hole0);
  ASSERT_THAT(stop_in_hole0.visited(), SizeIs(7));
  EXPECT_EQ(stop_in_hole0.visited()[5], CoordinateXY(1.0, 1.0));
  EXPECT_EQ(stop_in_hole0.visited()[6], CoordinateXY(2.0, 1.0));
}

TEST(SurfaceTest, ApplyReadWriteCoordinateSequenceFilter) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> poly = MakeSquareWithTwoHoles(*factory);

  // When isGeometryChanged() is false, coordinates are visited and
  // geometryChanged() is not invoked.
  RecordingCoordSeqFilter read_only_rw(std::numeric_limits<std::size_t>::max(),
                                       false, 100.0, 200.0);
  poly->apply_rw(read_only_rw);
  EXPECT_THAT(read_only_rw.visited(), SizeIs(15));
  EXPECT_EQ(*poly->getCoordinate(), CoordinateXY(0.0, 0.0));
  EXPECT_DOUBLE_EQ(poly->getEnvelopeInternal()->getMaxX(), 10.0);

  // Stop inside exterior ring -> skips both holes, still calls
  // geometryChanged().
  RecordingCoordSeqFilter stop_in_shell(2, true, 5.0, 5.0);
  poly->apply_rw(stop_in_shell);
  EXPECT_THAT(stop_in_shell.visited(), SizeIs(2));
  EXPECT_EQ(*poly->getInteriorRingN(0)->getCoordinate(),
            CoordinateXY(1.0, 1.0));
  EXPECT_EQ(*poly->getInteriorRingN(1)->getCoordinate(),
            CoordinateXY(4.0, 4.0));

  // Stop inside hole 0 -> skips hole 1.
  std::unique_ptr<Polygon> poly2 = MakeSquareWithTwoHoles(*factory);
  RecordingCoordSeqFilter stop_in_hole0(6, true, 100.0, 200.0);
  poly2->apply_rw(stop_in_hole0);
  EXPECT_THAT(stop_in_hole0.visited(), SizeIs(6));
  EXPECT_EQ(*poly2->getInteriorRingN(0)->getCoordinate(),
            CoordinateXY(101.0, 201.0));
  EXPECT_EQ(*poly2->getInteriorRingN(1)->getCoordinate(),
            CoordinateXY(4.0, 4.0));

  // Complete traversal with isGeometryChanged() == true updates envelopes.
  std::unique_ptr<Polygon> poly3 = MakeSquareWithTwoHoles(*factory);
  RecordingCoordSeqFilter full_rw(std::numeric_limits<std::size_t>::max(), true,
                                  100.0, 200.0);
  poly3->apply_rw(full_rw);
  EXPECT_THAT(full_rw.visited(), SizeIs(15));
  const Envelope* env = poly3->getEnvelopeInternal();
  EXPECT_DOUBLE_EQ(env->getMinX(), 100.0);
  EXPECT_DOUBLE_EQ(env->getMinY(), 200.0);
  EXPECT_DOUBLE_EQ(env->getMaxX(), 110.0);
  EXPECT_DOUBLE_EQ(env->getMaxY(), 210.0);
  EXPECT_DOUBLE_EQ(poly3->getInteriorRingN(1)->getEnvelopeInternal()->getMinX(),
                   104.0);
}

TEST(SurfaceTest, ApplyGeometryComponentFilters) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> poly = MakeSquareWithTwoHoles(*factory);

  RecordingComponentFilter ro_all(std::numeric_limits<std::size_t>::max());
  poly->apply_ro(&ro_all);
  EXPECT_THAT(
      ro_all.ro_geoms(),
      ElementsAre(poly.get(), poly->getExteriorRing(),
                  poly->getInteriorRingN(0), poly->getInteriorRingN(1)));

  RecordingComponentFilter ro_stop_after_shell(2);
  poly->apply_ro(&ro_stop_after_shell);
  EXPECT_THAT(ro_stop_after_shell.ro_geoms(),
              ElementsAre(poly.get(), poly->getExteriorRing()));

  RecordingComponentFilter ro_stop_after_hole0(3);
  poly->apply_ro(&ro_stop_after_hole0);
  EXPECT_THAT(ro_stop_after_hole0.ro_geoms(),
              ElementsAre(poly.get(), poly->getExteriorRing(),
                          poly->getInteriorRingN(0)));

  RecordingComponentFilter rw_all(std::numeric_limits<std::size_t>::max());
  poly->apply_rw(&rw_all);
  EXPECT_THAT(
      rw_all.rw_geoms(),
      ElementsAre(poly.get(), poly->getExteriorRing(),
                  poly->getInteriorRingN(0), poly->getInteriorRingN(1)));

  RecordingComponentFilter rw_stop_after_shell(2);
  poly->apply_rw(&rw_stop_after_shell);
  EXPECT_THAT(rw_stop_after_shell.rw_geoms(),
              ElementsAre(poly.get(), poly->getExteriorRing()));

  RecordingComponentFilter rw_stop_after_hole0(3);
  poly->apply_rw(&rw_stop_after_hole0);
  EXPECT_THAT(rw_stop_after_hole0.rw_geoms(),
              ElementsAre(poly.get(), poly->getExteriorRing(),
                          poly->getInteriorRingN(0)));
}

TEST(SurfaceTest, ApplyGeometryFilters) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> poly = MakeSquareWithTwoHoles(*factory);

  RecordingGeometryFilter filter;
  poly->apply_ro(&filter);
  EXPECT_THAT(filter.ro_geoms(), ElementsAre(poly.get()));
  EXPECT_TRUE(filter.rw_geoms().empty());

  poly->apply_rw(&filter);
  EXPECT_THAT(filter.rw_geoms(), ElementsAre(poly.get()));
}

TEST(SurfaceTest, CompareToSameClass) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Different exterior shells.
  std::unique_ptr<Polygon> small_shell =
      factory->createPolygon(MakeSquareRingXY(*factory, 0.0, 0.0, 10.0, 10.0));
  std::unique_ptr<Polygon> large_shell =
      factory->createPolygon(MakeSquareRingXY(*factory, 1.0, 0.0, 10.0, 10.0));
  EXPECT_LT(small_shell->compareTo(large_shell.get()), 0);
  EXPECT_GT(large_shell->compareTo(small_shell.get()), 0);

  // Equal shells, no holes (distinct instances).
  std::unique_ptr<Polygon> small_shell_copy =
      factory->createPolygon(MakeSquareRingXY(*factory, 0.0, 0.0, 10.0, 10.0));
  EXPECT_EQ(small_shell->compareTo(small_shell_copy.get()), 0);

  // Equal shells, different number of holes.
  std::unique_ptr<Polygon> one_hole = [&]() {
    std::vector<std::unique_ptr<LinearRing>> holes;
    holes.push_back(MakeSquareRingXY(*factory, 1.0, 1.0, 2.0, 2.0));
    return factory->createPolygon(
        MakeSquareRingXY(*factory, 0.0, 0.0, 10.0, 10.0), std::move(holes));
  }();
  std::unique_ptr<Polygon> two_holes = MakeSquareWithTwoHoles(*factory);
  EXPECT_LT(one_hole->compareTo(two_holes.get()), 0);
  EXPECT_GT(two_holes->compareTo(one_hole.get()), 0);

  // Equal shells, equal hole count, hole 0 identical, hole 1 differs.
  std::unique_ptr<Polygon> two_holes_shifted = [&]() {
    std::vector<std::unique_ptr<LinearRing>> holes;
    holes.push_back(MakeSquareRingXY(*factory, 1.0, 1.0, 2.0, 2.0));
    holes.push_back(MakeSquareRingXY(*factory, 5.0, 5.0, 7.0, 7.0));
    return factory->createPolygon(
        MakeSquareRingXY(*factory, 0.0, 0.0, 10.0, 10.0), std::move(holes));
  }();
  EXPECT_LT(two_holes->compareTo(two_holes_shifted.get()), 0);
  EXPECT_GT(two_holes_shifted->compareTo(two_holes.get()), 0);

  // Equal shells, equal holes (distinct instances).
  std::unique_ptr<Polygon> two_holes_copy = MakeSquareWithTwoHoles(*factory);
  EXPECT_EQ(two_holes->compareTo(two_holes_copy.get()), 0);

  // CurvePolygons also compare via Surface::compareToSameClass.
  std::unique_ptr<CurvePolygon> cp1 =
      factory->createCurvePolygon(MakeCircleCurve(*factory, 2.0));
  std::unique_ptr<CurvePolygon> cp2 =
      factory->createCurvePolygon(MakeCircleCurve(*factory, 2.0));
  std::unique_ptr<CurvePolygon> cp3 =
      factory->createCurvePolygon(MakeCircleCurve(*factory, 3.0));
  EXPECT_EQ(cp1->compareTo(cp2.get()), 0);
  EXPECT_LT(cp1->compareTo(cp3.get()), 0);
}

TEST(SurfaceTest, ConvexHull) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Empty polygon -> empty hull.
  std::unique_ptr<Polygon> empty = factory->createPolygon();
  std::unique_ptr<Geometry> empty_hull = empty->convexHull();
  EXPECT_TRUE(empty_hull->isEmpty());

  // Non-convex L-shaped polygon with an interior hole -> hull matches outer L.
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(MakeSquareRingXY(*factory, 0.5, 0.5, 1.5, 1.5));
  std::unique_ptr<Polygon> l_shape =
      factory->createPolygon(MakeRing<CoordinateXY>(*factory, {{0.0, 0.0},
                                                               {4.0, 0.0},
                                                               {4.0, 2.0},
                                                               {2.0, 2.0},
                                                               {2.0, 4.0},
                                                               {0.0, 4.0},
                                                               {0.0, 0.0}}),
                             std::move(holes));
  std::unique_ptr<Geometry> hull = l_shape->convexHull();
  EXPECT_DOUBLE_EQ(hull->getArea(), 14.0);

  // Curved CurvePolygon delegates convexHull() to exterior Curve, which throws.
  std::unique_ptr<CurvePolygon> curved =
      factory->createCurvePolygon(MakeCircleCurve(*factory, 1.0));
  EXPECT_THROW(curved->convexHull(), UnsupportedOperationException);
}

TEST(SurfaceTest, EqualsExact) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<Polygon> base = MakeSquareWithTwoHoles(*factory);
  std::unique_ptr<Polygon> exact_copy = MakeSquareWithTwoHoles(*factory);
  EXPECT_TRUE(base->equalsExact(exact_copy.get(), 0.0));

  // Mismatched geometry class.
  std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(0.0, 0.0));
  EXPECT_FALSE(base->equalsExact(pt.get(), 0.0));
  std::unique_ptr<CurvePolygon> curve_poly = factory->createCurvePolygon(
      MakeSquareRingXY(*factory, 0.0, 0.0, 10.0, 10.0));
  EXPECT_FALSE(base->equalsExact(curve_poly.get(), 0.0));

  // Exterior ring differs outside vs within tolerance.
  std::unique_ptr<Polygon> perturbed_shell = [&]() {
    std::vector<std::unique_ptr<LinearRing>> holes;
    holes.push_back(MakeSquareRingXY(*factory, 1.0, 1.0, 2.0, 2.0));
    holes.push_back(MakeSquareRingXY(*factory, 4.0, 4.0, 6.0, 6.0));
    return factory->createPolygon(
        MakeSquareRingXY(*factory, 0.0, 0.0, 10.05, 10.0), std::move(holes));
  }();
  EXPECT_FALSE(base->equalsExact(perturbed_shell.get(), 0.01));
  EXPECT_TRUE(base->equalsExact(perturbed_shell.get(), 0.1));

  // Same shell, different number of interior rings.
  std::unique_ptr<Polygon> one_hole = [&]() {
    std::vector<std::unique_ptr<LinearRing>> holes;
    holes.push_back(MakeSquareRingXY(*factory, 1.0, 1.0, 2.0, 2.0));
    return factory->createPolygon(
        MakeSquareRingXY(*factory, 0.0, 0.0, 10.0, 10.0), std::move(holes));
  }();
  EXPECT_FALSE(base->equalsExact(one_hole.get(), 0.0));

  // Same shell, same hole count, second hole differs.
  std::unique_ptr<Polygon> perturbed_hole = [&]() {
    std::vector<std::unique_ptr<LinearRing>> holes;
    holes.push_back(MakeSquareRingXY(*factory, 1.0, 1.0, 2.0, 2.0));
    holes.push_back(MakeSquareRingXY(*factory, 4.0, 4.0, 6.05, 6.0));
    return factory->createPolygon(
        MakeSquareRingXY(*factory, 0.0, 0.0, 10.0, 10.0), std::move(holes));
  }();
  EXPECT_FALSE(base->equalsExact(perturbed_hole.get(), 0.01));
  EXPECT_TRUE(base->equalsExact(perturbed_hole.get(), 0.1));

  // Empty vs empty and empty vs non-empty.
  std::unique_ptr<Polygon> empty1 = factory->createPolygon();
  std::unique_ptr<Polygon> empty2 = factory->createPolygon();
  EXPECT_TRUE(empty1->equalsExact(empty2.get(), 0.0));
  EXPECT_FALSE(empty1->equalsExact(base.get(), 0.0));
  EXPECT_FALSE(empty1->equalsExact(
      factory->createCurvePolygon(false, false).get(), 0.0));
}

TEST(SurfaceTest, EqualsIdentical) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto make_xyz_poly = [&](double shell_z, double hole1_z) {
    std::vector<std::unique_ptr<LinearRing>> holes;
    holes.push_back(MakeRing<Coordinate>(*factory, {{1.0, 1.0, 0.0},
                                                    {2.0, 1.0, 0.0},
                                                    {2.0, 2.0, 0.0},
                                                    {1.0, 2.0, 0.0},
                                                    {1.0, 1.0, 0.0}}));
    holes.push_back(MakeRing<Coordinate>(*factory, {{4.0, 4.0, hole1_z},
                                                    {6.0, 4.0, hole1_z},
                                                    {6.0, 6.0, hole1_z},
                                                    {4.0, 6.0, hole1_z},
                                                    {4.0, 4.0, hole1_z}}));
    return factory->createPolygon(
        MakeRing<Coordinate>(*factory, {{0.0, 0.0, shell_z},
                                        {10.0, 0.0, shell_z},
                                        {10.0, 10.0, shell_z},
                                        {0.0, 10.0, shell_z},
                                        {0.0, 0.0, shell_z}}),
        std::move(holes));
  };

  std::unique_ptr<Polygon> p1 = make_xyz_poly(5.0, 7.0);
  std::unique_ptr<Polygon> p1_copy = make_xyz_poly(5.0, 7.0);
  std::unique_ptr<Polygon> p_diff_shell_z = make_xyz_poly(6.0, 7.0);
  std::unique_ptr<Polygon> p_diff_hole_z = make_xyz_poly(5.0, 8.0);
  std::unique_ptr<Polygon> p_one_hole = [&]() {
    std::vector<std::unique_ptr<LinearRing>> holes;
    holes.push_back(MakeRing<Coordinate>(*factory, {{1.0, 1.0, 0.0},
                                                    {2.0, 1.0, 0.0},
                                                    {2.0, 2.0, 0.0},
                                                    {1.0, 2.0, 0.0},
                                                    {1.0, 1.0, 0.0}}));
    return factory->createPolygon(
        MakeRing<Coordinate>(*factory, {{0.0, 0.0, 5.0},
                                        {10.0, 0.0, 5.0},
                                        {10.0, 10.0, 5.0},
                                        {0.0, 10.0, 5.0},
                                        {0.0, 0.0, 5.0}}),
        std::move(holes));
  }();

  std::unique_ptr<Point> pt = factory->createPoint(Coordinate(0.0, 0.0, 5.0));
  EXPECT_FALSE(p1->equalsIdentical(pt.get()));
  EXPECT_FALSE(p1->equalsIdentical(p_one_hole.get()));
  EXPECT_FALSE(p1->equalsIdentical(p_diff_shell_z.get()));
  EXPECT_FALSE(p1->equalsIdentical(p_diff_hole_z.get()));
  EXPECT_TRUE(p1->equalsIdentical(p1_copy.get()));

  // XYZM and NaN equality inside coordinates.
  auto make_xyzm = [&](double z, double m) {
    return factory->createPolygon(
        MakeRing<CoordinateXYZM>(*factory, {{0.0, 0.0, z, m},
                                            {1.0, 0.0, z, m},
                                            {1.0, 1.0, z, m},
                                            {0.0, 0.0, z, m}}));
  };
  double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_TRUE(make_xyzm(nan, 3.0)->equalsIdentical(make_xyzm(nan, 3.0).get()));
  EXPECT_FALSE(make_xyzm(1.0, 3.0)->equalsIdentical(make_xyzm(1.0, 4.0).get()));

  // Empty polygons with matching vs differing coordinate dimensions.
  EXPECT_TRUE(
      factory->createPolygon(false, false)
          ->equalsIdentical(factory->createPolygon(false, false).get()));
  EXPECT_FALSE(
      factory->createPolygon(false, false)
          ->equalsIdentical(factory->createPolygon(true, false).get()));
}

TEST(SurfaceTest, CoordinateDimensionsAndHasZHasM) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Shell only: XY, XYZ, XYM, XYZM.
  std::unique_ptr<Polygon> xy_poly =
      factory->createPolygon(MakeSquareRingXY(*factory, 0.0, 0.0, 10.0, 10.0));
  EXPECT_EQ(xy_poly->getCoordinateDimension(), 2);
  EXPECT_FALSE(xy_poly->hasZ());
  EXPECT_FALSE(xy_poly->hasM());

  std::unique_ptr<Polygon> xyz_poly =
      factory->createPolygon(MakeRing<Coordinate>(*factory, {{0.0, 0.0, 1.0},
                                                             {1.0, 0.0, 1.0},
                                                             {1.0, 1.0, 1.0},
                                                             {0.0, 0.0, 1.0}}));
  EXPECT_EQ(xyz_poly->getCoordinateDimension(), 3);
  EXPECT_TRUE(xyz_poly->hasZ());
  EXPECT_FALSE(xyz_poly->hasM());

  std::unique_ptr<Polygon> xym_poly = factory->createPolygon(
      MakeRing<CoordinateXYM>(*factory, {{0.0, 0.0, 2.0},
                                         {1.0, 0.0, 2.0},
                                         {1.0, 1.0, 2.0},
                                         {0.0, 0.0, 2.0}}));
  EXPECT_EQ(xym_poly->getCoordinateDimension(), 3);
  EXPECT_FALSE(xym_poly->hasZ());
  EXPECT_TRUE(xym_poly->hasM());

  std::unique_ptr<Polygon> xyzm_poly = factory->createPolygon(
      MakeRing<CoordinateXYZM>(*factory, {{0.0, 0.0, 1.0, 2.0},
                                          {1.0, 0.0, 1.0, 2.0},
                                          {1.0, 1.0, 1.0, 2.0},
                                          {0.0, 0.0, 1.0, 2.0}}));
  EXPECT_EQ(xyzm_poly->getCoordinateDimension(), 4);
  EXPECT_TRUE(xyzm_poly->hasZ());
  EXPECT_TRUE(xyzm_poly->hasM());

  // XY shell with XY holes -> hasZ() and hasM() return false after checking
  // holes.
  std::unique_ptr<Polygon> xy_with_holes = MakeSquareWithTwoHoles(*factory);
  EXPECT_EQ(xy_with_holes->getCoordinateDimension(), 2);
  EXPECT_FALSE(xy_with_holes->hasZ());
  EXPECT_FALSE(xy_with_holes->hasM());

  // XY shell with a second hole carrying XYZ -> dimension 3, hasZ() true.
  {
    std::vector<std::unique_ptr<LinearRing>> holes;
    holes.push_back(MakeSquareRingXY(*factory, 1.0, 1.0, 2.0, 2.0));
    holes.push_back(MakeRing<Coordinate>(*factory, {{4.0, 4.0, 9.0},
                                                    {6.0, 4.0, 9.0},
                                                    {6.0, 6.0, 9.0},
                                                    {4.0, 6.0, 9.0},
                                                    {4.0, 4.0, 9.0}}));
    std::unique_ptr<Polygon> poly_hole_z = factory->createPolygon(
        MakeSquareRingXY(*factory, 0.0, 0.0, 10.0, 10.0), std::move(holes));
    EXPECT_EQ(poly_hole_z->getCoordinateDimension(), 3);
    EXPECT_TRUE(poly_hole_z->hasZ());
    EXPECT_FALSE(poly_hole_z->hasM());
  }

  // XY shell with a second hole carrying XYM -> dimension 3, hasM() true.
  {
    std::vector<std::unique_ptr<LinearRing>> holes;
    holes.push_back(MakeSquareRingXY(*factory, 1.0, 1.0, 2.0, 2.0));
    holes.push_back(MakeRing<CoordinateXYM>(*factory, {{4.0, 4.0, 7.0},
                                                       {6.0, 4.0, 7.0},
                                                       {6.0, 6.0, 7.0},
                                                       {4.0, 6.0, 7.0},
                                                       {4.0, 4.0, 7.0}}));
    std::unique_ptr<Polygon> poly_hole_m = factory->createPolygon(
        MakeSquareRingXY(*factory, 0.0, 0.0, 10.0, 10.0), std::move(holes));
    EXPECT_EQ(poly_hole_m->getCoordinateDimension(), 3);
    EXPECT_FALSE(poly_hole_m->hasZ());
    EXPECT_TRUE(poly_hole_m->hasM());
  }

  // XY shell with an XYZM hole -> dimension 4, both hasZ() and hasM() true.
  {
    std::vector<std::unique_ptr<LinearRing>> holes;
    holes.push_back(MakeRing<CoordinateXYZM>(*factory, {{1.0, 1.0, 3.0, 4.0},
                                                        {2.0, 1.0, 3.0, 4.0},
                                                        {2.0, 2.0, 3.0, 4.0},
                                                        {1.0, 1.0, 3.0, 4.0}}));
    std::unique_ptr<Polygon> poly_hole_zm = factory->createPolygon(
        MakeSquareRingXY(*factory, 0.0, 0.0, 10.0, 10.0), std::move(holes));
    EXPECT_EQ(poly_hole_zm->getCoordinateDimension(), 4);
    EXPECT_TRUE(poly_hole_zm->hasZ());
    EXPECT_TRUE(poly_hole_zm->hasM());
  }
}

TEST(SurfaceTest, LengthNumPointsEnvelopeAndCoordinate) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<Polygon> poly = MakeSquareWithTwoHoles(*factory);
  EXPECT_FALSE(poly->isEmpty());
  // Outer perimeter = 40, hole0 = 4, hole1 = 8 => 52.
  EXPECT_DOUBLE_EQ(poly->getLength(), 52.0);
  // 5 vertices per closed square * 3 rings = 15.
  EXPECT_EQ(poly->getNumPoints(), 15U);

  const CoordinateXY* coord = poly->getCoordinate();
  ASSERT_NE(coord, nullptr);
  EXPECT_EQ(*coord, CoordinateXY(0.0, 0.0));

  const Envelope* env = poly->getEnvelopeInternal();
  EXPECT_DOUBLE_EQ(env->getMinX(), 0.0);
  EXPECT_DOUBLE_EQ(env->getMinY(), 0.0);
  EXPECT_DOUBLE_EQ(env->getMaxX(), 10.0);
  EXPECT_DOUBLE_EQ(env->getMaxY(), 10.0);

  // CurvePolygon with CircularString shell (circle of radius sqrt(2)) and a
  // square LinearRing hole [ -0.5, 0.5 ] x [ -0.5, 0.5 ].
  std::vector<std::unique_ptr<Curve>> holes;
  holes.push_back(MakeSquareRingXY(*factory, -0.5, -0.5, 0.5, 0.5));
  std::unique_ptr<CurvePolygon> curve_poly = factory->createCurvePolygon(
      MakeCircleCurve(*factory, 1.0), std::move(holes));
  const double radius = std::numbers::sqrt2;
  EXPECT_NEAR(curve_poly->getLength(), 2.0 * std::numbers::pi * radius + 4.0,
              1e-9);
  EXPECT_EQ(curve_poly->getNumPoints(), 10U);
  const Envelope* curve_env = curve_poly->getEnvelopeInternal();
  EXPECT_NEAR(curve_env->getMinX(), -radius, 1e-9);
  EXPECT_NEAR(curve_env->getMaxX(), radius, 1e-9);
  EXPECT_NEAR(curve_env->getMinY(), -radius, 1e-9);
  EXPECT_NEAR(curve_env->getMaxY(), radius, 1e-9);
}

}  // namespace
}  // namespace geos_google
