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

// Unit tests for third_party/geos/src/precision/CommonBitsRemover.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/precision/CommonBitsRemover.cpp

#include "third_party/geos/include/geos/precision/CommonBitsRemover.h"

#include <cmath>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/CurvePolygon.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"

namespace geos_google {
namespace {

using geos::geom::CircularString;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::CurvePolygon;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::MultiLineString;
using geos::geom::MultiPoint;
using geos::geom::MultiPolygon;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::precision::CommonBitsRemover;

static_assert(!std::is_copy_constructible_v<CommonBitsRemover>);
static_assert(!std::is_copy_assignable_v<CommonBitsRemover>);

std::unique_ptr<Polygon> MakeBox(const GeometryFactory* factory, double x0,
                                 double y0, double x1, double y1) {
  return factory->createPolygon(CoordinateSequence{
      Coordinate(x0, y0),
      Coordinate(x1, y0),
      Coordinate(x1, y1),
      Coordinate(x0, y1),
      Coordinate(x0, y0),
  });
}

TEST(CommonBitsRemoverTest, InitialCommonCoordinateIsZero) {
  CommonBitsRemover remover;
  const Coordinate& common = remover.getCommonCoordinate();
  EXPECT_DOUBLE_EQ(common.x, 0.0);
  EXPECT_DOUBLE_EQ(common.y, 0.0);
  EXPECT_TRUE(std::isnan(common.z));
}

TEST(CommonBitsRemoverTest, EmptyGeometriesKeepCommonCoordinateZero) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> empty_pt = factory->createPoint();
  std::unique_ptr<LineString> empty_line = factory->createLineString();
  std::unique_ptr<Polygon> empty_poly = factory->createPolygon();
  std::unique_ptr<GeometryCollection> empty_gc =
      factory->createGeometryCollection();

  CommonBitsRemover remover;
  remover.add(empty_pt.get());
  remover.add(empty_line.get());
  remover.add(empty_poly.get());
  remover.add(empty_gc.get());

  const Coordinate& common = remover.getCommonCoordinate();
  EXPECT_DOUBLE_EQ(common.x, 0.0);
  EXPECT_DOUBLE_EQ(common.y, 0.0);

  remover.removeCommonBits(empty_poly.get());
  EXPECT_TRUE(empty_poly->isEmpty());
  Geometry* returned = remover.addCommonBits(empty_poly.get());
  EXPECT_EQ(returned, empty_poly.get());
  EXPECT_TRUE(empty_poly->isEmpty());
}

TEST(CommonBitsRemoverTest, ZeroCommonBitsRemoveIsNoOp) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> p0 = factory->createPoint(Coordinate(-10.5, -20.5));
  std::unique_ptr<Point> p1 = factory->createPoint(Coordinate(10.5, 20.5));

  CommonBitsRemover remover;
  remover.add(p0.get());
  remover.add(p1.get());

  const Coordinate& common = remover.getCommonCoordinate();
  EXPECT_DOUBLE_EQ(common.x, 0.0);
  EXPECT_DOUBLE_EQ(common.y, 0.0);

  remover.removeCommonBits(p0.get());
  EXPECT_DOUBLE_EQ(p0->getX(), -10.5);
  EXPECT_DOUBLE_EQ(p0->getY(), -20.5);

  const Envelope* env = p0->getEnvelopeInternal();
  EXPECT_DOUBLE_EQ(env->getMinX(), -10.5);
  EXPECT_DOUBLE_EQ(env->getMinY(), -20.5);

  EXPECT_EQ(remover.addCommonBits(p0.get()), p0.get());
  EXPECT_DOUBLE_EQ(p0->getX(), -10.5);
  EXPECT_DOUBLE_EQ(p0->getY(), -20.5);
}

TEST(CommonBitsRemoverTest, SingleAxisCommonBitsShiftsOnlyThatAxis) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Common X = 1000000, common Y = 0.
  {
    std::unique_ptr<Point> p0 =
        factory->createPoint(Coordinate(1000000.25, -10.5));
    std::unique_ptr<Point> p1 =
        factory->createPoint(Coordinate(1000001.75, 10.5));

    CommonBitsRemover remover;
    remover.add(p0.get());
    remover.add(p1.get());
    EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().x, 1000000.0);
    EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().y, 0.0);

    remover.removeCommonBits(p0.get());
    remover.removeCommonBits(p1.get());
    EXPECT_DOUBLE_EQ(p0->getX(), 0.25);
    EXPECT_DOUBLE_EQ(p0->getY(), -10.5);
    EXPECT_DOUBLE_EQ(p1->getX(), 1.75);
    EXPECT_DOUBLE_EQ(p1->getY(), 10.5);

    remover.addCommonBits(p0.get());
    remover.addCommonBits(p1.get());
    EXPECT_DOUBLE_EQ(p0->getX(), 1000000.25);
    EXPECT_DOUBLE_EQ(p0->getY(), -10.5);
    EXPECT_DOUBLE_EQ(p1->getX(), 1000001.75);
    EXPECT_DOUBLE_EQ(p1->getY(), 10.5);
  }

  // Common X = 0, common Y = 2000000.
  {
    std::unique_ptr<Point> p0 =
        factory->createPoint(Coordinate(-10.5, 2000000.25));
    std::unique_ptr<Point> p1 =
        factory->createPoint(Coordinate(10.5, 2000001.75));

    CommonBitsRemover remover;
    remover.add(p0.get());
    remover.add(p1.get());
    EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().x, 0.0);
    EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().y, 2000000.0);

    remover.removeCommonBits(p0.get());
    remover.removeCommonBits(p1.get());
    EXPECT_DOUBLE_EQ(p0->getX(), -10.5);
    EXPECT_DOUBLE_EQ(p0->getY(), 0.25);
    EXPECT_DOUBLE_EQ(p1->getX(), 10.5);
    EXPECT_DOUBLE_EQ(p1->getY(), 1.75);

    remover.addCommonBits(p0.get());
    remover.addCommonBits(p1.get());
    EXPECT_DOUBLE_EQ(p0->getX(), -10.5);
    EXPECT_DOUBLE_EQ(p0->getY(), 2000000.25);
    EXPECT_DOUBLE_EQ(p1->getX(), 10.5);
    EXPECT_DOUBLE_EQ(p1->getY(), 2000001.75);
  }
}

TEST(CommonBitsRemoverTest, RemovesAndRestoresPointAndUpdatesEnvelope) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> pt =
      factory->createPoint(Coordinate(1000000.25, 2000000.75));

  // Populate envelope before shifting to verify geometryChanged() updates it.
  EXPECT_DOUBLE_EQ(pt->getEnvelopeInternal()->getMinX(), 1000000.25);
  EXPECT_DOUBLE_EQ(pt->getEnvelopeInternal()->getMinY(), 2000000.75);

  CommonBitsRemover remover;
  remover.add(pt.get());
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().x, 1000000.0);
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().y, 2000000.0);

  remover.removeCommonBits(pt.get());
  EXPECT_DOUBLE_EQ(pt->getX(), 0.25);
  EXPECT_DOUBLE_EQ(pt->getY(), 0.75);
  EXPECT_DOUBLE_EQ(pt->getEnvelopeInternal()->getMinX(), 0.25);
  EXPECT_DOUBLE_EQ(pt->getEnvelopeInternal()->getMaxX(), 0.25);
  EXPECT_DOUBLE_EQ(pt->getEnvelopeInternal()->getMinY(), 0.75);
  EXPECT_DOUBLE_EQ(pt->getEnvelopeInternal()->getMaxY(), 0.75);

  Geometry* returned = remover.addCommonBits(pt.get());
  EXPECT_EQ(returned, pt.get());
  EXPECT_DOUBLE_EQ(pt->getX(), 1000000.25);
  EXPECT_DOUBLE_EQ(pt->getY(), 2000000.75);
  EXPECT_DOUBLE_EQ(pt->getEnvelopeInternal()->getMinX(), 1000000.25);
  EXPECT_DOUBLE_EQ(pt->getEnvelopeInternal()->getMinY(), 2000000.75);
}

TEST(CommonBitsRemoverTest,
     RemovesAndRestoresPolygonWithHoleLineAndCollection) {
  constexpr double kBaseX = 1000000000.0;
  constexpr double kBaseY = 2000000000.0;

  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<LinearRing> shell =
      factory->createLinearRing(CoordinateSequence{
          Coordinate(kBaseX, kBaseY),
          Coordinate(kBaseX + 10.0, kBaseY),
          Coordinate(kBaseX + 10.0, kBaseY + 10.0),
          Coordinate(kBaseX, kBaseY + 10.0),
          Coordinate(kBaseX, kBaseY),
      });
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(factory->createLinearRing(CoordinateSequence{
      Coordinate(kBaseX + 2.0, kBaseY + 2.0),
      Coordinate(kBaseX + 4.0, kBaseY + 2.0),
      Coordinate(kBaseX + 4.0, kBaseY + 4.0),
      Coordinate(kBaseX + 2.0, kBaseY + 4.0),
      Coordinate(kBaseX + 2.0, kBaseY + 2.0),
  }));
  std::unique_ptr<Polygon> poly =
      factory->createPolygon(std::move(shell), std::move(holes));

  std::unique_ptr<LineString> line =
      factory->createLineString(CoordinateSequence{
          Coordinate(kBaseX + 1.0, kBaseY + 1.0),
          Coordinate(kBaseX + 3.0, kBaseY + 5.0),
      });

  std::vector<std::unique_ptr<Geometry>> elements;
  elements.push_back(
      factory->createPoint(Coordinate(kBaseX + 7.0, kBaseY + 8.0)));
  elements.push_back(factory->createLineString(CoordinateSequence{
      Coordinate(kBaseX, kBaseY + 2.0),
      Coordinate(kBaseX + 6.0, kBaseY + 9.0),
  }));
  std::unique_ptr<GeometryCollection> gc =
      factory->createGeometryCollection(std::move(elements));

  Geometry::Ptr poly_orig = poly->clone();
  Geometry::Ptr line_orig = line->clone();
  Geometry::Ptr gc_orig = gc->clone();

  // Touch envelopes before shifting.
  ASSERT_FALSE(poly->getEnvelopeInternal()->isNull());
  ASSERT_FALSE(line->getEnvelopeInternal()->isNull());
  ASSERT_FALSE(gc->getEnvelopeInternal()->isNull());

  CommonBitsRemover remover;
  remover.add(poly.get());
  remover.add(line.get());
  remover.add(gc.get());

  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().x, kBaseX);
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().y, kBaseY);

  remover.removeCommonBits(poly.get());
  remover.removeCommonBits(line.get());
  remover.removeCommonBits(gc.get());

  std::unique_ptr<Polygon> expected_shifted_poly =
      MakeBox(factory.get(), 0.0, 0.0, 10.0, 10.0);
  EXPECT_DOUBLE_EQ(poly->getArea(), 96.0);
  EXPECT_DOUBLE_EQ(poly->getEnvelopeInternal()->getMinX(), 0.0);
  EXPECT_DOUBLE_EQ(poly->getEnvelopeInternal()->getMaxX(), 10.0);
  EXPECT_DOUBLE_EQ(poly->getEnvelopeInternal()->getMinY(), 0.0);
  EXPECT_DOUBLE_EQ(poly->getEnvelopeInternal()->getMaxY(), 10.0);
  ASSERT_EQ(poly->getNumInteriorRing(), 1U);
  EXPECT_DOUBLE_EQ(poly->getInteriorRingN(0)->getEnvelopeInternal()->getMinX(),
                   2.0);
  EXPECT_DOUBLE_EQ(poly->getInteriorRingN(0)->getEnvelopeInternal()->getMaxX(),
                   4.0);
  EXPECT_TRUE(poly->getExteriorRing()->equalsExact(
      expected_shifted_poly->getExteriorRing(), 0.0));

  EXPECT_DOUBLE_EQ(line->getEnvelopeInternal()->getMinX(), 1.0);
  EXPECT_DOUBLE_EQ(line->getEnvelopeInternal()->getMaxX(), 3.0);
  EXPECT_DOUBLE_EQ(line->getEnvelopeInternal()->getMinY(), 1.0);
  EXPECT_DOUBLE_EQ(line->getEnvelopeInternal()->getMaxY(), 5.0);

  EXPECT_DOUBLE_EQ(gc->getEnvelopeInternal()->getMinX(), 0.0);
  EXPECT_DOUBLE_EQ(gc->getEnvelopeInternal()->getMaxX(), 7.0);
  EXPECT_DOUBLE_EQ(gc->getEnvelopeInternal()->getMinY(), 2.0);
  EXPECT_DOUBLE_EQ(gc->getEnvelopeInternal()->getMaxY(), 9.0);

  EXPECT_EQ(remover.addCommonBits(poly.get()), poly.get());
  EXPECT_EQ(remover.addCommonBits(line.get()), line.get());
  EXPECT_EQ(remover.addCommonBits(gc.get()), gc.get());

  EXPECT_TRUE(poly->equalsExact(poly_orig.get(), 0.0));
  EXPECT_TRUE(line->equalsExact(line_orig.get(), 0.0));
  EXPECT_TRUE(gc->equalsExact(gc_orig.get(), 0.0));
  EXPECT_DOUBLE_EQ(poly->getEnvelopeInternal()->getMinX(), kBaseX);
  EXPECT_DOUBLE_EQ(poly->getEnvelopeInternal()->getMaxX(), kBaseX + 10.0);
}

TEST(CommonBitsRemoverTest, PreservesZAndMOrdinates) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> pt =
      factory->createPoint(Coordinate(1000000.5, 2000000.5, 42.25));
  std::unique_ptr<LineString> line =
      factory->createLineString(CoordinateSequence{
          CoordinateXYZM(1000000.0, 2000000.0, 10.0, 1.5),
          CoordinateXYZM(1000001.0, 2000002.0, 20.0, 2.5),
      });
  Geometry::Ptr line_orig = line->clone();

  CommonBitsRemover remover;
  remover.add(pt.get());
  remover.add(line.get());

  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().x, 1000000.0);
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().y, 2000000.0);

  remover.removeCommonBits(pt.get());
  EXPECT_TRUE(pt->hasZ());
  EXPECT_FALSE(pt->hasM());
  EXPECT_DOUBLE_EQ(pt->getX(), 0.5);
  EXPECT_DOUBLE_EQ(pt->getY(), 0.5);
  EXPECT_DOUBLE_EQ(pt->getZ(), 42.25);

  remover.removeCommonBits(line.get());
  EXPECT_TRUE(line->hasZ());
  EXPECT_TRUE(line->hasM());
  const CoordinateSequence* seq = line->getCoordinatesRO();
  ASSERT_EQ(seq->size(), 2U);
  EXPECT_DOUBLE_EQ(seq->getX(0), 0.0);
  EXPECT_DOUBLE_EQ(seq->getY(0), 0.0);
  EXPECT_DOUBLE_EQ(seq->getOrdinate(0, CoordinateSequence::Z), 10.0);
  EXPECT_DOUBLE_EQ(seq->getOrdinate(0, CoordinateSequence::M), 1.5);
  EXPECT_DOUBLE_EQ(seq->getX(1), 1.0);
  EXPECT_DOUBLE_EQ(seq->getY(1), 2.0);
  EXPECT_DOUBLE_EQ(seq->getOrdinate(1, CoordinateSequence::Z), 20.0);
  EXPECT_DOUBLE_EQ(seq->getOrdinate(1, CoordinateSequence::M), 2.5);

  remover.addCommonBits(pt.get());
  remover.addCommonBits(line.get());
  EXPECT_DOUBLE_EQ(pt->getX(), 1000000.5);
  EXPECT_DOUBLE_EQ(pt->getY(), 2000000.5);
  EXPECT_DOUBLE_EQ(pt->getZ(), 42.25);
  EXPECT_TRUE(line->equalsIdentical(line_orig.get()));
}

TEST(CommonBitsRemoverTest, HandlesNegativeCommonCoordinates) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> p0 =
      factory->createPoint(Coordinate(-1000000.5, -2000000.5));
  std::unique_ptr<Point> p1 =
      factory->createPoint(Coordinate(-1000001.5, -2000001.5));

  CommonBitsRemover remover;
  remover.add(p0.get());
  remover.add(p1.get());

  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().x, -1000000.0);
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().y, -2000000.0);

  remover.removeCommonBits(p0.get());
  remover.removeCommonBits(p1.get());
  EXPECT_DOUBLE_EQ(p0->getX(), -0.5);
  EXPECT_DOUBLE_EQ(p0->getY(), -0.5);
  EXPECT_DOUBLE_EQ(p1->getX(), -1.5);
  EXPECT_DOUBLE_EQ(p1->getY(), -1.5);

  remover.addCommonBits(p0.get());
  remover.addCommonBits(p1.get());
  EXPECT_DOUBLE_EQ(p0->getX(), -1000000.5);
  EXPECT_DOUBLE_EQ(p0->getY(), -2000000.5);
  EXPECT_DOUBLE_EQ(p1->getX(), -1000001.5);
  EXPECT_DOUBLE_EQ(p1->getY(), -2000001.5);
}

TEST(CommonBitsRemoverTest, NoOpBeforeAdd) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> pt = factory->createPoint(Coordinate(123.5, 456.25));
  ASSERT_FALSE(pt->getEnvelopeInternal()->isNull());

  CommonBitsRemover remover;
  Coordinate& common = remover.getCommonCoordinate();
  EXPECT_DOUBLE_EQ(common.x, 0.0);
  EXPECT_DOUBLE_EQ(common.y, 0.0);

  remover.removeCommonBits(pt.get());
  EXPECT_DOUBLE_EQ(pt->getX(), 123.5);
  EXPECT_DOUBLE_EQ(pt->getY(), 456.25);

  Geometry* returned = remover.addCommonBits(pt.get());
  EXPECT_EQ(returned, pt.get());
  EXPECT_DOUBLE_EQ(pt->getX(), 123.5);
  EXPECT_DOUBLE_EQ(pt->getY(), 456.25);
  EXPECT_DOUBLE_EQ(pt->getEnvelopeInternal()->getMinX(), 123.5);
  EXPECT_DOUBLE_EQ(pt->getEnvelopeInternal()->getMinY(), 456.25);
}

TEST(CommonBitsRemoverTest, IncrementalAddUpdatesCommonCoordinate) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> p0 =
      factory->createPoint(Coordinate(1049104.25, 2097152.25));
  std::unique_ptr<Point> p1 =
      factory->createPoint(Coordinate(1049120.75, 2097153.75));
  std::unique_ptr<Point> p2 =
      factory->createPoint(Coordinate(-10.0, 2097152.5));
  std::unique_ptr<Point> p3 = factory->createPoint(Coordinate(5.0, -10.0));

  CommonBitsRemover remover;
  remover.add(p0.get());
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().x, 1049104.0);
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().y, 2097152.0);

  // 1049104 (0x100210) and 1049120 (0x100220) share common prefix 0x100200.
  remover.add(p1.get());
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().x, 1049088.0);
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().y, 2097152.0);

  // Opposite-sign X clears common X bits while keeping common Y bits.
  remover.add(p2.get());
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().x, 0.0);
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().y, 2097152.0);

  // Opposite-sign Y clears common Y bits as well.
  remover.add(p3.get());
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().x, 0.0);
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().y, 0.0);
}

TEST(CommonBitsRemoverTest, PreservesXYAndXYMCoordinateTypes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<Point> pt_xy =
      factory->createPoint(CoordinateXY(1000000.25, 2000000.75));
  Geometry::Ptr pt_xy_orig = pt_xy->clone();

  std::unique_ptr<Point> pt_xym =
      factory->createPoint(CoordinateXYM(1000000.5, 2000000.5, 7.25));
  Geometry::Ptr pt_xym_orig = pt_xym->clone();

  std::unique_ptr<LineString> line_xym =
      factory->createLineString(CoordinateSequence{
          CoordinateXYM(1000000.0, 2000000.0, 1.5),
          CoordinateXYM(1000001.0, 2000002.0, 3.5),
      });
  Geometry::Ptr line_xym_orig = line_xym->clone();

  CommonBitsRemover remover;
  remover.add(pt_xy.get());
  remover.add(pt_xym.get());
  remover.add(line_xym.get());

  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().x, 1000000.0);
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().y, 2000000.0);

  remover.removeCommonBits(pt_xy.get());
  EXPECT_FALSE(pt_xy->hasZ());
  EXPECT_FALSE(pt_xy->hasM());
  EXPECT_DOUBLE_EQ(pt_xy->getX(), 0.25);
  EXPECT_DOUBLE_EQ(pt_xy->getY(), 0.75);
  EXPECT_TRUE(std::isnan(pt_xy->getZ()));
  EXPECT_TRUE(std::isnan(pt_xy->getM()));

  remover.removeCommonBits(pt_xym.get());
  EXPECT_FALSE(pt_xym->hasZ());
  EXPECT_TRUE(pt_xym->hasM());
  EXPECT_DOUBLE_EQ(pt_xym->getX(), 0.5);
  EXPECT_DOUBLE_EQ(pt_xym->getY(), 0.5);
  EXPECT_TRUE(std::isnan(pt_xym->getZ()));
  EXPECT_DOUBLE_EQ(pt_xym->getM(), 7.25);

  remover.removeCommonBits(line_xym.get());
  EXPECT_FALSE(line_xym->hasZ());
  EXPECT_TRUE(line_xym->hasM());
  const CoordinateSequence* seq = line_xym->getCoordinatesRO();
  ASSERT_EQ(seq->size(), 2U);
  EXPECT_DOUBLE_EQ(seq->getX(0), 0.0);
  EXPECT_DOUBLE_EQ(seq->getY(0), 0.0);
  EXPECT_TRUE(std::isnan(seq->getOrdinate(0, CoordinateSequence::Z)));
  EXPECT_DOUBLE_EQ(seq->getOrdinate(0, CoordinateSequence::M), 1.5);
  EXPECT_DOUBLE_EQ(seq->getX(1), 1.0);
  EXPECT_DOUBLE_EQ(seq->getY(1), 2.0);
  EXPECT_TRUE(std::isnan(seq->getOrdinate(1, CoordinateSequence::Z)));
  EXPECT_DOUBLE_EQ(seq->getOrdinate(1, CoordinateSequence::M), 3.5);

  remover.addCommonBits(pt_xy.get());
  remover.addCommonBits(pt_xym.get());
  remover.addCommonBits(line_xym.get());
  EXPECT_TRUE(pt_xy->equalsIdentical(pt_xy_orig.get()));
  EXPECT_TRUE(pt_xym->equalsIdentical(pt_xym_orig.get()));
  EXPECT_TRUE(line_xym->equalsIdentical(line_xym_orig.get()));
}

TEST(CommonBitsRemoverTest, HandlesMultiGeometries) {
  constexpr double kBaseX = 1000000000.0;
  constexpr double kBaseY = 2000000000.0;

  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<MultiPoint> mp = factory->createMultiPoint(CoordinateSequence{
      Coordinate(kBaseX + 1.0, kBaseY + 2.0),
      Coordinate(kBaseX + 3.0, kBaseY + 4.0),
  });

  std::vector<std::unique_ptr<LineString>> lines;
  lines.push_back(factory->createLineString(CoordinateSequence{
      Coordinate(kBaseX, kBaseY + 1.0),
      Coordinate(kBaseX + 2.0, kBaseY + 5.0),
  }));
  lines.push_back(factory->createLineString(CoordinateSequence{
      Coordinate(kBaseX + 4.0, kBaseY),
      Coordinate(kBaseX + 6.0, kBaseY + 3.0),
  }));
  std::unique_ptr<MultiLineString> mls =
      factory->createMultiLineString(std::move(lines));

  std::vector<std::unique_ptr<Polygon>> polys;
  polys.push_back(
      MakeBox(factory.get(), kBaseX, kBaseY, kBaseX + 5.0, kBaseY + 5.0));
  polys.push_back(MakeBox(factory.get(), kBaseX + 6.0, kBaseY + 1.0,
                          kBaseX + 10.0, kBaseY + 4.0));
  std::unique_ptr<MultiPolygon> mpoly =
      factory->createMultiPolygon(std::move(polys));

  Geometry::Ptr mp_orig = mp->clone();
  Geometry::Ptr mls_orig = mls->clone();
  Geometry::Ptr mpoly_orig = mpoly->clone();

  ASSERT_FALSE(mp->getEnvelopeInternal()->isNull());
  ASSERT_FALSE(mls->getEnvelopeInternal()->isNull());
  ASSERT_FALSE(mpoly->getEnvelopeInternal()->isNull());

  CommonBitsRemover remover;
  remover.add(mp.get());
  remover.add(mls.get());
  remover.add(mpoly.get());

  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().x, kBaseX);
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().y, kBaseY);

  remover.removeCommonBits(mp.get());
  remover.removeCommonBits(mls.get());
  remover.removeCommonBits(mpoly.get());

  EXPECT_DOUBLE_EQ(mp->getEnvelopeInternal()->getMinX(), 1.0);
  EXPECT_DOUBLE_EQ(mp->getEnvelopeInternal()->getMaxX(), 3.0);
  EXPECT_DOUBLE_EQ(mp->getEnvelopeInternal()->getMinY(), 2.0);
  EXPECT_DOUBLE_EQ(mp->getEnvelopeInternal()->getMaxY(), 4.0);

  EXPECT_DOUBLE_EQ(mls->getEnvelopeInternal()->getMinX(), 0.0);
  EXPECT_DOUBLE_EQ(mls->getEnvelopeInternal()->getMaxX(), 6.0);
  EXPECT_DOUBLE_EQ(mls->getEnvelopeInternal()->getMinY(), 0.0);
  EXPECT_DOUBLE_EQ(mls->getEnvelopeInternal()->getMaxY(), 5.0);

  EXPECT_DOUBLE_EQ(mpoly->getArea(), 37.0);
  EXPECT_DOUBLE_EQ(mpoly->getEnvelopeInternal()->getMinX(), 0.0);
  EXPECT_DOUBLE_EQ(mpoly->getEnvelopeInternal()->getMaxX(), 10.0);
  EXPECT_DOUBLE_EQ(mpoly->getEnvelopeInternal()->getMinY(), 0.0);
  EXPECT_DOUBLE_EQ(mpoly->getEnvelopeInternal()->getMaxY(), 5.0);

  EXPECT_EQ(remover.addCommonBits(mp.get()), mp.get());
  EXPECT_EQ(remover.addCommonBits(mls.get()), mls.get());
  EXPECT_EQ(remover.addCommonBits(mpoly.get()), mpoly.get());

  EXPECT_TRUE(mp->equalsExact(mp_orig.get(), 0.0));
  EXPECT_TRUE(mls->equalsExact(mls_orig.get(), 0.0));
  EXPECT_TRUE(mpoly->equalsExact(mpoly_orig.get(), 0.0));
  EXPECT_DOUBLE_EQ(mpoly->getEnvelopeInternal()->getMinX(), kBaseX);
  EXPECT_DOUBLE_EQ(mpoly->getEnvelopeInternal()->getMaxX(), kBaseX + 10.0);
}

TEST(CommonBitsRemoverTest, ShiftsUnaddedGeometryByComputedCommonCoordinate) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> seed =
      factory->createPoint(Coordinate(1000000.25, 2000000.25));
  std::unique_ptr<Polygon> other =
      MakeBox(factory.get(), 1000001.0, 2000001.0, 1000004.0, 2000005.0);
  Geometry::Ptr other_orig = other->clone();
  ASSERT_FALSE(other->getEnvelopeInternal()->isNull());

  CommonBitsRemover remover;
  remover.add(seed.get());

  remover.removeCommonBits(other.get());
  EXPECT_DOUBLE_EQ(other->getArea(), 12.0);
  EXPECT_DOUBLE_EQ(other->getEnvelopeInternal()->getMinX(), 1.0);
  EXPECT_DOUBLE_EQ(other->getEnvelopeInternal()->getMaxX(), 4.0);
  EXPECT_DOUBLE_EQ(other->getEnvelopeInternal()->getMinY(), 1.0);
  EXPECT_DOUBLE_EQ(other->getEnvelopeInternal()->getMaxY(), 5.0);

  EXPECT_EQ(remover.addCommonBits(other.get()), other.get());
  EXPECT_TRUE(other->equalsExact(other_orig.get(), 0.0));
  EXPECT_DOUBLE_EQ(other->getEnvelopeInternal()->getMinX(), 1000001.0);
  EXPECT_DOUBLE_EQ(other->getEnvelopeInternal()->getMinY(), 2000001.0);
}

TEST(CommonBitsRemoverTest, FractionalCoordinatesHaveZeroCommonBits) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> p0 = factory->createPoint(Coordinate(0.25, 0.75));
  std::unique_ptr<Point> p1 = factory->createPoint(Coordinate(0.5, 0.25));

  CommonBitsRemover remover;
  remover.add(p0.get());
  remover.add(p1.get());

  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().x, 0.0);
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().y, 0.0);

  remover.removeCommonBits(p0.get());
  EXPECT_DOUBLE_EQ(p0->getX(), 0.25);
  EXPECT_DOUBLE_EQ(p0->getY(), 0.75);

  EXPECT_EQ(remover.addCommonBits(p0.get()), p0.get());
  EXPECT_DOUBLE_EQ(p0->getX(), 0.25);
  EXPECT_DOUBLE_EQ(p0->getY(), 0.75);
}

TEST(CommonBitsRemoverTest,
     EmptyGeometryBeforeAndAfterNonEmptyPreservesCommonCoordinate) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> empty_pt = factory->createPoint();
  std::unique_ptr<Polygon> empty_poly = factory->createPolygon();
  std::unique_ptr<Point> pt =
      factory->createPoint(Coordinate(1000000.25, 2000000.75));

  CommonBitsRemover remover;
  remover.add(empty_pt.get());
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().x, 0.0);
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().y, 0.0);

  remover.add(pt.get());
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().x, 1000000.0);
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().y, 2000000.0);

  remover.add(empty_poly.get());
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().x, 1000000.0);
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().y, 2000000.0);

  remover.removeCommonBits(empty_pt.get());
  EXPECT_TRUE(empty_pt->isEmpty());
  remover.removeCommonBits(pt.get());
  EXPECT_DOUBLE_EQ(pt->getX(), 0.25);
  EXPECT_DOUBLE_EQ(pt->getY(), 0.75);
}

TEST(CommonBitsRemoverTest,
     CircularStringAndCurvePolygonRemoveAndRestoreCommonBits) {
  constexpr double kBaseX = 1000000.0;
  constexpr double kBaseY = 2000000.0;

  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<CircularString> arc =
      factory->createCircularString(CoordinateSequence{
          Coordinate(kBaseX, kBaseY),
          Coordinate(kBaseX + 1.0, kBaseY + 1.0),
          Coordinate(kBaseX + 2.0, kBaseY),
      });
  Geometry::Ptr arc_orig = arc->clone();
  ASSERT_FALSE(arc->getEnvelopeInternal()->isNull());

  std::unique_ptr<CircularString> shell =
      factory->createCircularString(CoordinateSequence{
          Coordinate(kBaseX, kBaseY),
          Coordinate(kBaseX + 1.0, kBaseY + 1.0),
          Coordinate(kBaseX + 2.0, kBaseY),
          Coordinate(kBaseX + 1.0, kBaseY - 1.0),
          Coordinate(kBaseX, kBaseY),
      });
  std::unique_ptr<CurvePolygon> cpoly =
      factory->createCurvePolygon(std::move(shell));
  Geometry::Ptr cpoly_orig = cpoly->clone();
  ASSERT_FALSE(cpoly->getEnvelopeInternal()->isNull());

  CommonBitsRemover remover;
  remover.add(arc.get());
  remover.add(cpoly.get());
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().x, kBaseX);
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().y, kBaseY);

  remover.removeCommonBits(arc.get());
  EXPECT_DOUBLE_EQ(arc->getEnvelopeInternal()->getMinX(), 0.0);
  EXPECT_DOUBLE_EQ(arc->getEnvelopeInternal()->getMaxX(), 2.0);
  EXPECT_DOUBLE_EQ(arc->getEnvelopeInternal()->getMinY(), 0.0);
  EXPECT_DOUBLE_EQ(arc->getEnvelopeInternal()->getMaxY(), 1.0);

  remover.removeCommonBits(cpoly.get());
  EXPECT_DOUBLE_EQ(cpoly->getEnvelopeInternal()->getMinX(), 0.0);
  EXPECT_DOUBLE_EQ(cpoly->getEnvelopeInternal()->getMaxX(), 2.0);
  EXPECT_DOUBLE_EQ(cpoly->getEnvelopeInternal()->getMinY(), -1.0);
  EXPECT_DOUBLE_EQ(cpoly->getEnvelopeInternal()->getMaxY(), 1.0);

  EXPECT_EQ(remover.addCommonBits(arc.get()), arc.get());
  EXPECT_EQ(remover.addCommonBits(cpoly.get()), cpoly.get());
  EXPECT_TRUE(arc->equalsExact(arc_orig.get(), 0.0));
  EXPECT_TRUE(cpoly->equalsExact(cpoly_orig.get(), 0.0));
  EXPECT_DOUBLE_EQ(arc->getEnvelopeInternal()->getMinX(), kBaseX);
  EXPECT_DOUBLE_EQ(cpoly->getEnvelopeInternal()->getMinY(), kBaseY - 1.0);
}

TEST(CommonBitsRemoverTest, ShiftedOverlayResultRestoresOriginalCoordinates) {
  constexpr double kBaseX = 1000000000.0;
  constexpr double kBaseY = 2000000000.0;

  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> box0 =
      MakeBox(factory.get(), kBaseX, kBaseY, kBaseX + 4.0, kBaseY + 4.0);
  std::unique_ptr<Polygon> box1 = MakeBox(
      factory.get(), kBaseX + 2.0, kBaseY + 1.0, kBaseX + 6.0, kBaseY + 5.0);

  CommonBitsRemover remover;
  remover.add(box0.get());
  remover.add(box1.get());
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().x, kBaseX);
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().y, kBaseY);

  remover.removeCommonBits(box0.get());
  remover.removeCommonBits(box1.get());

  std::unique_ptr<Geometry> inter = box0->intersection(box1.get());
  ASSERT_NE(inter, nullptr);
  EXPECT_DOUBLE_EQ(inter->getArea(), 6.0);
  EXPECT_DOUBLE_EQ(inter->getEnvelopeInternal()->getMinX(), 2.0);
  EXPECT_DOUBLE_EQ(inter->getEnvelopeInternal()->getMaxX(), 4.0);
  EXPECT_DOUBLE_EQ(inter->getEnvelopeInternal()->getMinY(), 1.0);
  EXPECT_DOUBLE_EQ(inter->getEnvelopeInternal()->getMaxY(), 4.0);

  Geometry* shifted_back = remover.addCommonBits(inter.get());
  EXPECT_EQ(shifted_back, inter.get());
  EXPECT_DOUBLE_EQ(inter->getArea(), 6.0);
  EXPECT_DOUBLE_EQ(inter->getEnvelopeInternal()->getMinX(), kBaseX + 2.0);
  EXPECT_DOUBLE_EQ(inter->getEnvelopeInternal()->getMaxX(), kBaseX + 4.0);
  EXPECT_DOUBLE_EQ(inter->getEnvelopeInternal()->getMinY(), kBaseY + 1.0);
  EXPECT_DOUBLE_EQ(inter->getEnvelopeInternal()->getMaxY(), kBaseY + 4.0);
}

TEST(CommonBitsRemoverTest, MutableCommonCoordinateReferenceAndSubsequentAdd) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> p0 =
      factory->createPoint(Coordinate(1000000.5, 2000000.5));
  std::unique_ptr<Point> p1 =
      factory->createPoint(Coordinate(1000001.5, 2000001.5));

  CommonBitsRemover remover;
  Coordinate& common = remover.getCommonCoordinate();
  EXPECT_DOUBLE_EQ(common.x, 0.0);
  EXPECT_DOUBLE_EQ(common.y, 0.0);

  common.x = 100.0;
  common.y = 200.0;
  remover.removeCommonBits(p0.get());
  EXPECT_DOUBLE_EQ(p0->getX(), 999900.5);
  EXPECT_DOUBLE_EQ(p0->getY(), 1999800.5);
  remover.addCommonBits(p0.get());
  EXPECT_DOUBLE_EQ(p0->getX(), 1000000.5);
  EXPECT_DOUBLE_EQ(p0->getY(), 2000000.5);

  remover.add(p0.get());
  remover.add(p1.get());
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().x, 1000000.0);
  EXPECT_DOUBLE_EQ(remover.getCommonCoordinate().y, 2000000.0);
}

}  // namespace
}  // namespace geos_google
