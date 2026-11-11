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

// Unit tests for third_party/geos/src/geom/Polygon.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/Polygon.cpp

#include "third_party/geos/include/geos/geom/Polygon.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/Orientation.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::algorithm::Orientation;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYZM;
using geos::geom::Dimension;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::MultiLineString;
using geos::geom::Polygon;
using geos::util::IllegalArgumentException;

template <typename CoordT>
std::unique_ptr<LinearRing> MakeRing(const GeometryFactory* factory,
                                     std::initializer_list<CoordT> coords) {
  CoordinateSequence seq(coords);
  return factory->createLinearRing(seq);
}

void ExpectSequenceXY(const CoordinateSequence& seq,
                      std::initializer_list<CoordinateXY> expected) {
  ASSERT_EQ(seq.size(), expected.size());
  std::size_t idx = 0;
  for (const CoordinateXY& c : expected) {
    EXPECT_DOUBLE_EQ(seq.getX(idx), c.x) << "at index " << idx;
    EXPECT_DOUBLE_EQ(seq.getY(idx), c.y) << "at index " << idx;
    ++idx;
  }
}

TEST(PolygonTest, EmptyPolygonPropertiesAndDimensions) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> empty = factory->createPolygon();
  ASSERT_NE(empty, nullptr);

  EXPECT_TRUE(empty->isEmpty());
  EXPECT_TRUE(empty->isPolygonal());
  EXPECT_FALSE(empty->isRectangle());
  EXPECT_EQ(empty->getNumPoints(), 0u);
  EXPECT_EQ(empty->getNumInteriorRing(), 0u);
  EXPECT_EQ(empty->getDimension(), Dimension::A);
  EXPECT_EQ(empty->getBoundaryDimension(), 1);
  EXPECT_EQ(empty->getGeometryType(), "Polygon");
  EXPECT_EQ(empty->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(empty->getArea(), 0.0);
  EXPECT_DOUBLE_EQ(empty->getLength(), 0.0);
  EXPECT_EQ(empty->getCoordinate(), nullptr);
  const Envelope* empty_env = empty->getEnvelopeInternal();
  ASSERT_NE(empty_env, nullptr);
  EXPECT_TRUE(empty_env->isNull());

  struct DimCase {
    bool has_z;
    bool has_m;
  };
  constexpr DimCase cases[] = {
      {.has_z = false, .has_m = false},
      {.has_z = true, .has_m = false},
      {.has_z = false, .has_m = true},
      {.has_z = true, .has_m = true},
  };
  for (const auto& tc : cases) {
    std::unique_ptr<Polygon> poly = factory->createPolygon(tc.has_z, tc.has_m);
    std::unique_ptr<CoordinateSequence> coords = poly->getCoordinates();
    ASSERT_NE(coords, nullptr);
    EXPECT_TRUE(coords->isEmpty());
    EXPECT_EQ(coords->hasZ(), tc.has_z);
    EXPECT_EQ(coords->hasM(), tc.has_m);
  }
}

TEST(PolygonTest, ConstructionVariantsCloningAndInvalidInputs) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Construct directly from CoordinateSequence rvalue.
  std::unique_ptr<Polygon> from_seq = factory->createPolygon(CoordinateSequence{
      CoordinateXY{0.0, 0.0},
      CoordinateXY{4.0, 0.0},
      CoordinateXY{4.0, 4.0},
      CoordinateXY{0.0, 4.0},
      CoordinateXY{0.0, 0.0},
  });
  ASSERT_NE(from_seq, nullptr);
  EXPECT_DOUBLE_EQ(from_seq->getArea(), 16.0);

  // Construct via raw LinearRing pointer factory overload.
  std::unique_ptr<LinearRing> shell = MakeRing<CoordinateXY>(
      factory.get(),
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});
  std::unique_ptr<LinearRing> hole = MakeRing<CoordinateXY>(
      factory.get(),
      {{2.0, 2.0}, {4.0, 2.0}, {4.0, 4.0}, {2.0, 4.0}, {2.0, 2.0}});
  std::vector<LinearRing*> hole_ptrs{hole.get()};
  std::unique_ptr<Polygon> poly(factory->createPolygon(*shell, hole_ptrs));
  int user_data = 7;
  poly->setSRID(3857);
  poly->setUserData(&user_data);

  std::unique_ptr<Polygon> cloned = poly->clone();
  EXPECT_NE(cloned.get(), poly.get());
  EXPECT_EQ(cloned->getSRID(), 3857);
  EXPECT_EQ(cloned->getUserData(), nullptr);
  EXPECT_TRUE(cloned->equalsExact(poly.get()));
  EXPECT_EQ(cloned->getNumInteriorRing(), 1u);

  const Geometry* as_geom = poly.get();
  std::unique_ptr<Geometry> geom_clone = as_geom->clone();
  EXPECT_EQ(geom_clone->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(geom_clone->equalsExact(poly.get()));

  // Release ownership of shell and interior rings cleanly.
  std::unique_ptr<LinearRing> released_shell = cloned->releaseExteriorRing();
  std::vector<std::unique_ptr<LinearRing>> released_holes =
      cloned->releaseInteriorRings();
  EXPECT_EQ(released_shell->getNumPoints(), 5u);
  EXPECT_EQ(released_holes.size(), 1u);

  // Empty shell with non-empty hole throws.
  std::vector<std::unique_ptr<LinearRing>> non_empty_holes;
  non_empty_holes.push_back(std::move(hole));
  EXPECT_THROW(factory->createPolygon(factory->createLinearRing(),
                                      std::move(non_empty_holes)),
               IllegalArgumentException);

  // Null hole element throws.
  std::vector<std::unique_ptr<LinearRing>> null_holes;
  null_holes.push_back(nullptr);
  EXPECT_THROW(factory->createPolygon(std::move(shell), std::move(null_holes)),
               IllegalArgumentException);
}

TEST(PolygonTest, GetCoordinatesConcatenatesShellAndHoles) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Shell only (XYZM).
  std::unique_ptr<LinearRing> xyzm_shell =
      MakeRing<CoordinateXYZM>(factory.get(), {{0.0, 0.0, 1.0, 10.0},
                                               {5.0, 0.0, 2.0, 20.0},
                                               {5.0, 5.0, 3.0, 30.0},
                                               {0.0, 0.0, 1.0, 10.0}});
  std::unique_ptr<Polygon> xyzm_poly =
      factory->createPolygon(std::move(xyzm_shell));
  std::unique_ptr<CoordinateSequence> xyzm_coords = xyzm_poly->getCoordinates();
  EXPECT_TRUE(xyzm_coords->hasZ());
  EXPECT_TRUE(xyzm_coords->hasM());
  ASSERT_EQ(xyzm_coords->size(), 4u);
  EXPECT_DOUBLE_EQ(xyzm_coords->getOrdinate(2, CoordinateSequence::Z), 3.0);
  EXPECT_DOUBLE_EQ(xyzm_coords->getOrdinate(2, CoordinateSequence::M), 30.0);

  // XY shell + two holes -> coordinates concatenated in order.
  std::unique_ptr<LinearRing> shell = MakeRing<CoordinateXY>(
      factory.get(),
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(MakeRing<CoordinateXY>(
      factory.get(),
      {{1.0, 1.0}, {1.0, 2.0}, {2.0, 2.0}, {2.0, 1.0}, {1.0, 1.0}}));
  holes.push_back(MakeRing<CoordinateXY>(
      factory.get(),
      {{6.0, 6.0}, {6.0, 8.0}, {8.0, 8.0}, {8.0, 6.0}, {6.0, 6.0}}));
  std::unique_ptr<Polygon> poly_with_holes =
      factory->createPolygon(std::move(shell), std::move(holes));
  std::unique_ptr<CoordinateSequence> all_coords =
      poly_with_holes->getCoordinates();
  EXPECT_FALSE(all_coords->hasZ());
  EXPECT_FALSE(all_coords->hasM());
  ExpectSequenceXY(*all_coords, {{0.0, 0.0},
                                 {10.0, 0.0},
                                 {10.0, 10.0},
                                 {0.0, 10.0},
                                 {0.0, 0.0},
                                 {1.0, 1.0},
                                 {1.0, 2.0},
                                 {2.0, 2.0},
                                 {2.0, 1.0},
                                 {1.0, 1.0},
                                 {6.0, 6.0},
                                 {6.0, 8.0},
                                 {8.0, 8.0},
                                 {8.0, 6.0},
                                 {6.0, 6.0}});

  // Mixed dimensions (XY shell + XYZ hole): polygon reports hasZ() == true.
  std::unique_ptr<LinearRing> xy_shell = MakeRing<CoordinateXY>(
      factory.get(),
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});
  std::vector<std::unique_ptr<LinearRing>> xyz_holes;
  xyz_holes.push_back(MakeRing<Coordinate>(factory.get(), {{2.0, 2.0, 5.0},
                                                           {2.0, 4.0, 5.0},
                                                           {4.0, 4.0, 5.0},
                                                           {4.0, 2.0, 5.0},
                                                           {2.0, 2.0, 5.0}}));
  std::unique_ptr<Polygon> mixed_poly =
      factory->createPolygon(std::move(xy_shell), std::move(xyz_holes));
  EXPECT_TRUE(mixed_poly->hasZ());
  std::unique_ptr<CoordinateSequence> mixed_coords =
      mixed_poly->getCoordinates();
  EXPECT_TRUE(mixed_coords->hasZ());
  ASSERT_EQ(mixed_coords->size(), 10u);
  EXPECT_TRUE(std::isnan(mixed_coords->getOrdinate(0, CoordinateSequence::Z)));
  EXPECT_DOUBLE_EQ(mixed_coords->getOrdinate(5, CoordinateSequence::Z), 5.0);
}

TEST(PolygonTest, GetBoundaryReturnsMultiLineStringOrLineString) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Empty polygon -> empty MultiLineString.
  std::unique_ptr<Geometry> empty_boundary =
      factory->createPolygon()->getBoundary();
  ASSERT_NE(empty_boundary, nullptr);
  EXPECT_NE(dynamic_cast<const MultiLineString*>(empty_boundary.get()),
            nullptr);
  EXPECT_EQ(empty_boundary->getGeometryTypeId(),
            geos::geom::GEOS_MULTILINESTRING);
  EXPECT_TRUE(empty_boundary->isEmpty());
  EXPECT_EQ(empty_boundary->getNumGeometries(), 0u);

  // Shell only -> LineString (not LinearRing).
  std::unique_ptr<LinearRing> shell = MakeRing<CoordinateXY>(
      factory.get(),
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});
  std::unique_ptr<Polygon> simple_poly = factory->createPolygon(shell->clone());
  std::unique_ptr<Geometry> shell_boundary = simple_poly->getBoundary();
  ASSERT_NE(shell_boundary, nullptr);
  EXPECT_EQ(shell_boundary->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_EQ(shell_boundary->getGeometryType(), "LineString");
  EXPECT_NE(dynamic_cast<const LineString*>(shell_boundary.get()), nullptr);
  EXPECT_EQ(dynamic_cast<const LinearRing*>(shell_boundary.get()), nullptr);
  ExpectSequenceXY(
      *shell_boundary->getCoordinates(),
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});

  // Shell + 2 holes -> MultiLineString of 3 LineStrings.
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(MakeRing<CoordinateXY>(
      factory.get(),
      {{1.0, 1.0}, {3.0, 1.0}, {3.0, 3.0}, {1.0, 3.0}, {1.0, 1.0}}));
  holes.push_back(MakeRing<CoordinateXY>(
      factory.get(),
      {{6.0, 6.0}, {8.0, 6.0}, {8.0, 8.0}, {6.0, 8.0}, {6.0, 6.0}}));
  std::unique_ptr<Polygon> holed_poly =
      factory->createPolygon(std::move(shell), std::move(holes));
  std::unique_ptr<Geometry> multi_boundary = holed_poly->getBoundary();
  ASSERT_NE(multi_boundary, nullptr);
  EXPECT_EQ(multi_boundary->getGeometryTypeId(),
            geos::geom::GEOS_MULTILINESTRING);
  ASSERT_EQ(multi_boundary->getNumGeometries(), 3u);
  for (std::size_t i = 0; i < 3; ++i) {
    const Geometry* part = multi_boundary->getGeometryN(i);
    EXPECT_EQ(part->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
    EXPECT_EQ(dynamic_cast<const LinearRing*>(part), nullptr);
  }
  ExpectSequenceXY(
      *multi_boundary->getGeometryN(0)->getCoordinates(),
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});
  ExpectSequenceXY(
      *multi_boundary->getGeometryN(1)->getCoordinates(),
      {{1.0, 1.0}, {3.0, 1.0}, {3.0, 3.0}, {1.0, 3.0}, {1.0, 1.0}});
  ExpectSequenceXY(
      *multi_boundary->getGeometryN(2)->getCoordinates(),
      {{6.0, 6.0}, {8.0, 6.0}, {8.0, 8.0}, {6.0, 8.0}, {6.0, 6.0}});
}

TEST(PolygonTest, GetAreaAccountsForShellHolesAndOrientation) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<Polygon> ccw_square =
      factory->createPolygon(MakeRing<CoordinateXY>(
          factory.get(),
          {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}}));
  EXPECT_DOUBLE_EQ(ccw_square->getArea(), 100.0);

  std::unique_ptr<Polygon> cw_square =
      factory->createPolygon(MakeRing<CoordinateXY>(
          factory.get(),
          {{0.0, 0.0}, {0.0, 10.0}, {10.0, 10.0}, {10.0, 0.0}, {0.0, 0.0}}));
  EXPECT_DOUBLE_EQ(cw_square->getArea(), 100.0);

  std::unique_ptr<Polygon> triangle =
      factory->createPolygon(MakeRing<CoordinateXY>(
          factory.get(), {{0.0, 0.0}, {6.0, 0.0}, {0.0, 4.0}, {0.0, 0.0}}));
  EXPECT_DOUBLE_EQ(triangle->getArea(), 12.0);

  std::vector<std::unique_ptr<LinearRing>> holes;
  // 2x3 hole (CW) -> area 6.
  holes.push_back(MakeRing<CoordinateXY>(
      factory.get(),
      {{1.0, 1.0}, {1.0, 4.0}, {3.0, 4.0}, {3.0, 1.0}, {1.0, 1.0}}));
  // 3x2 hole (CCW) -> area 6.
  holes.push_back(MakeRing<CoordinateXY>(
      factory.get(),
      {{5.0, 5.0}, {8.0, 5.0}, {8.0, 7.0}, {5.0, 7.0}, {5.0, 5.0}}));
  std::unique_ptr<Polygon> holed = factory->createPolygon(
      MakeRing<CoordinateXY>(
          factory.get(),
          {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}}),
      std::move(holes));
  EXPECT_DOUBLE_EQ(holed->getArea(), 88.0);

  std::unique_ptr<Polygon> collapsed =
      factory->createPolygon(MakeRing<CoordinateXY>(
          factory.get(), {{0.0, 0.0}, {5.0, 0.0}, {10.0, 0.0}, {0.0, 0.0}}));
  EXPECT_DOUBLE_EQ(collapsed->getArea(), 0.0);
}

TEST(PolygonTest, IsRectangleChecksAllGeometryConditions) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Valid CW rectangle.
  std::unique_ptr<Polygon> rect_cw =
      factory->createPolygon(MakeRing<CoordinateXY>(
          factory.get(),
          {{0.0, 0.0}, {0.0, 4.0}, {5.0, 4.0}, {5.0, 0.0}, {0.0, 0.0}}));
  EXPECT_TRUE(rect_cw->isRectangle());

  // Valid CCW rectangle starting at top-right.
  std::unique_ptr<Polygon> rect_ccw =
      factory->createPolygon(MakeRing<CoordinateXY>(
          factory.get(),
          {{5.0, 4.0}, {0.0, 4.0}, {0.0, 0.0}, {5.0, 0.0}, {5.0, 4.0}}));
  EXPECT_TRUE(rect_ccw->isRectangle());

  // Valid rectangle with negative and fractional coordinates.
  std::unique_ptr<Polygon> rect_neg =
      factory->createPolygon(MakeRing<CoordinateXY>(
          factory.get(),
          {{-2.5, -1.5}, {3.5, -1.5}, {3.5, 2.5}, {-2.5, 2.5}, {-2.5, -1.5}}));
  EXPECT_TRUE(rect_neg->isRectangle());

  // Rectangle shell with a hole -> false.
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(MakeRing<CoordinateXY>(
      factory.get(),
      {{2.0, 2.0}, {4.0, 2.0}, {4.0, 4.0}, {2.0, 4.0}, {2.0, 2.0}}));
  std::unique_ptr<Polygon> rect_with_hole = factory->createPolygon(
      MakeRing<CoordinateXY>(
          factory.get(),
          {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}}),
      std::move(holes));
  EXPECT_FALSE(rect_with_hole->isRectangle());

  // Empty, triangle (4 pts), extra collinear midpoint (6 pts) -> false.
  EXPECT_FALSE(factory->createPolygon()->isRectangle());
  EXPECT_FALSE(
      factory
          ->createPolygon(MakeRing<CoordinateXY>(
              factory.get(), {{0.0, 0.0}, {4.0, 0.0}, {2.0, 3.0}, {0.0, 0.0}}))
          ->isRectangle());
  EXPECT_FALSE(
      factory
          ->createPolygon(MakeRing<CoordinateXY>(factory.get(), {{0.0, 0.0},
                                                                 {5.0, 0.0},
                                                                 {10.0, 0.0},
                                                                 {10.0, 10.0},
                                                                 {0.0, 10.0},
                                                                 {0.0, 0.0}}))
          ->isRectangle());

  // 5-point trapezoid (interior X coordinate) -> false.
  EXPECT_FALSE(
      factory
          ->createPolygon(MakeRing<CoordinateXY>(
              factory.get(),
              {{0.0, 0.0}, {8.0, 0.0}, {10.0, 5.0}, {2.0, 5.0}, {0.0, 0.0}}))
          ->isRectangle());

  // All X values on minX/maxX, but one Y value strictly inside [minY, maxY] ->
  // false.
  EXPECT_FALSE(
      factory
          ->createPolygon(MakeRing<CoordinateXY>(
              factory.get(),
              {{0.0, 0.0}, {10.0, 0.0}, {10.0, 5.0}, {0.0, 10.0}, {0.0, 0.0}}))
          ->isRectangle());

  // Uses only the 4 envelope corners, but takes a diagonal step -> false.
  EXPECT_FALSE(
      factory
          ->createPolygon(MakeRing<CoordinateXY>(
              factory.get(),
              {{0.0, 0.0}, {10.0, 10.0}, {10.0, 0.0}, {0.0, 10.0}, {0.0, 0.0}}))
          ->isRectangle());

  // Uses only envelope corners, but repeats a vertex (xChanged == yChanged) ->
  // false.
  EXPECT_FALSE(
      factory
          ->createPolygon(MakeRing<CoordinateXY>(
              factory.get(),
              {{0.0, 0.0}, {10.0, 0.0}, {10.0, 0.0}, {0.0, 10.0}, {0.0, 0.0}}))
          ->isRectangle());
}

TEST(PolygonTest, OrientRingsControlsExteriorAndHoleWindings) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<Polygon> empty = factory->createPolygon();
  empty->orientRings(/*exteriorCW=*/true);
  EXPECT_TRUE(empty->isEmpty());

  // Shell starts CCW at (10, 0); hole 0 starts CW; hole 1 starts CCW.
  std::unique_ptr<LinearRing> shell = MakeRing<CoordinateXY>(
      factory.get(),
      {{10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}, {10.0, 0.0}});
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(MakeRing<CoordinateXY>(
      factory.get(),
      {{2.0, 2.0}, {2.0, 4.0}, {4.0, 4.0}, {4.0, 2.0}, {2.0, 2.0}}));
  holes.push_back(MakeRing<CoordinateXY>(
      factory.get(),
      {{6.0, 6.0}, {8.0, 6.0}, {8.0, 8.0}, {6.0, 8.0}, {6.0, 6.0}}));
  std::unique_ptr<Polygon> poly =
      factory->createPolygon(std::move(shell), std::move(holes));

  poly->orientRings(/*exteriorCW=*/true);
  EXPECT_FALSE(Orientation::isCCW(poly->getExteriorRing()->getCoordinatesRO()));
  EXPECT_TRUE(
      Orientation::isCCW(poly->getInteriorRingN(0)->getCoordinatesRO()));
  EXPECT_TRUE(
      Orientation::isCCW(poly->getInteriorRingN(1)->getCoordinatesRO()));
  // orientRings preserves the starting vertex rather than scrolling.
  EXPECT_DOUBLE_EQ(poly->getExteriorRing()->getCoordinatesRO()->getX(0), 10.0);
  EXPECT_DOUBLE_EQ(poly->getExteriorRing()->getCoordinatesRO()->getY(0), 0.0);

  poly->orientRings(/*exteriorCW=*/false);
  EXPECT_TRUE(Orientation::isCCW(poly->getExteriorRing()->getCoordinatesRO()));
  EXPECT_FALSE(
      Orientation::isCCW(poly->getInteriorRingN(0)->getCoordinatesRO()));
  EXPECT_FALSE(
      Orientation::isCCW(poly->getInteriorRingN(1)->getCoordinatesRO()));
}

TEST(PolygonTest, ReverseReversesShellAndHoles) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<Polygon> empty = factory->createPolygon();
  empty->setSRID(4326);
  std::unique_ptr<Polygon> empty_rev = empty->reverse();
  ASSERT_NE(empty_rev, nullptr);
  EXPECT_NE(empty_rev.get(), empty.get());
  EXPECT_TRUE(empty_rev->isEmpty());
  EXPECT_EQ(empty_rev->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_EQ(empty_rev->getSRID(), 4326);

  std::unique_ptr<LinearRing> shell = MakeRing<CoordinateXY>(
      factory.get(),
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(MakeRing<CoordinateXY>(
      factory.get(),
      {{1.0, 1.0}, {1.0, 3.0}, {3.0, 3.0}, {3.0, 1.0}, {1.0, 1.0}}));
  holes.push_back(MakeRing<CoordinateXY>(
      factory.get(),
      {{6.0, 6.0}, {8.0, 6.0}, {8.0, 8.0}, {6.0, 8.0}, {6.0, 6.0}}));
  std::unique_ptr<Polygon> poly =
      factory->createPolygon(std::move(shell), std::move(holes));

  std::unique_ptr<Polygon> rev = poly->reverse();
  ASSERT_NE(rev, nullptr);
  EXPECT_EQ(rev->getNumInteriorRing(), 2u);
  ExpectSequenceXY(
      *rev->getExteriorRing()->getCoordinatesRO(),
      {{0.0, 0.0}, {0.0, 10.0}, {10.0, 10.0}, {10.0, 0.0}, {0.0, 0.0}});
  ExpectSequenceXY(
      *rev->getInteriorRingN(0)->getCoordinatesRO(),
      {{1.0, 1.0}, {3.0, 1.0}, {3.0, 3.0}, {1.0, 3.0}, {1.0, 1.0}});
  ExpectSequenceXY(
      *rev->getInteriorRingN(1)->getCoordinatesRO(),
      {{6.0, 6.0}, {6.0, 8.0}, {8.0, 8.0}, {8.0, 6.0}, {6.0, 6.0}});

  // Original remains unchanged.
  EXPECT_TRUE(Orientation::isCCW(poly->getExteriorRing()->getCoordinatesRO()));

  // Polymorphic Geometry::reverse() invokes Polygon::reverseImpl().
  const Geometry* as_geom = poly.get();
  std::unique_ptr<Geometry> geom_rev = as_geom->reverse();
  EXPECT_EQ(geom_rev->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(geom_rev->equalsExact(rev.get()));
}

TEST(PolygonTest, NormalizeCanonicalizesShellHolesAndHoleOrder) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<Polygon> empty = factory->createPolygon();
  empty->normalize();
  EXPECT_TRUE(empty->isEmpty());

  // Polygon A: CCW shell starting at (10, 0), smaller hole A (CW) then larger
  // hole B (CCW).
  std::unique_ptr<LinearRing> shell_a = MakeRing<CoordinateXY>(
      factory.get(),
      {{10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}, {10.0, 0.0}});
  std::vector<std::unique_ptr<LinearRing>> holes_a;
  holes_a.push_back(MakeRing<CoordinateXY>(
      factory.get(),
      {{2.0, 2.0}, {2.0, 4.0}, {4.0, 4.0}, {4.0, 2.0}, {2.0, 2.0}}));
  holes_a.push_back(MakeRing<CoordinateXY>(
      factory.get(),
      {{8.0, 6.0}, {8.0, 8.0}, {6.0, 8.0}, {6.0, 6.0}, {8.0, 6.0}}));
  std::unique_ptr<Polygon> poly_a =
      factory->createPolygon(std::move(shell_a), std::move(holes_a));

  // Polygon B: same rings in CW shell starting at (10, 10), larger hole B (CW)
  // then smaller hole A (CCW).
  std::unique_ptr<LinearRing> shell_b = MakeRing<CoordinateXY>(
      factory.get(),
      {{10.0, 10.0}, {10.0, 0.0}, {0.0, 0.0}, {0.0, 10.0}, {10.0, 10.0}});
  std::vector<std::unique_ptr<LinearRing>> holes_b;
  holes_b.push_back(MakeRing<CoordinateXY>(
      factory.get(),
      {{6.0, 8.0}, {8.0, 8.0}, {8.0, 6.0}, {6.0, 6.0}, {6.0, 8.0}}));
  holes_b.push_back(MakeRing<CoordinateXY>(
      factory.get(),
      {{4.0, 4.0}, {2.0, 4.0}, {2.0, 2.0}, {4.0, 2.0}, {4.0, 4.0}}));
  std::unique_ptr<Polygon> poly_b =
      factory->createPolygon(std::move(shell_b), std::move(holes_b));

  poly_a->normalize();
  poly_b->normalize();

  // Shell normalized to CW starting at min coordinate (0, 0).
  ExpectSequenceXY(
      *poly_a->getExteriorRing()->getCoordinatesRO(),
      {{0.0, 0.0}, {0.0, 10.0}, {10.0, 10.0}, {10.0, 0.0}, {0.0, 0.0}});
  EXPECT_FALSE(
      Orientation::isCCW(poly_a->getExteriorRing()->getCoordinatesRO()));

  // Holes normalized to CCW starting at min coordinate and sorted descending
  // by compareTo (so hole starting at (6, 6) precedes hole starting at (2, 2)).
  ASSERT_EQ(poly_a->getNumInteriorRing(), 2u);
  ExpectSequenceXY(
      *poly_a->getInteriorRingN(0)->getCoordinatesRO(),
      {{6.0, 6.0}, {8.0, 6.0}, {8.0, 8.0}, {6.0, 8.0}, {6.0, 6.0}});
  ExpectSequenceXY(
      *poly_a->getInteriorRingN(1)->getCoordinatesRO(),
      {{2.0, 2.0}, {4.0, 2.0}, {4.0, 4.0}, {2.0, 4.0}, {2.0, 2.0}});
  EXPECT_TRUE(
      Orientation::isCCW(poly_a->getInteriorRingN(0)->getCoordinatesRO()));
  EXPECT_TRUE(
      Orientation::isCCW(poly_a->getInteriorRingN(1)->getCoordinatesRO()));

  EXPECT_TRUE(poly_a->equalsIdentical(poly_b.get()));

  // Normalize XYZM polygon containing an empty hole preserves Z/M and skips
  // empty ring safely.
  std::unique_ptr<LinearRing> xyzm_shell =
      MakeRing<CoordinateXYZM>(factory.get(), {{10.0, 0.0, 1.0, 11.0},
                                               {10.0, 10.0, 2.0, 12.0},
                                               {0.0, 10.0, 3.0, 13.0},
                                               {0.0, 0.0, 4.0, 14.0},
                                               {10.0, 0.0, 1.0, 11.0}});
  std::vector<std::unique_ptr<LinearRing>> holes_with_empty;
  holes_with_empty.push_back(factory->createLinearRing(true, true));
  holes_with_empty.push_back(
      MakeRing<CoordinateXYZM>(factory.get(), {{2.0, 2.0, 5.0, 15.0},
                                               {2.0, 4.0, 6.0, 16.0},
                                               {4.0, 4.0, 7.0, 17.0},
                                               {4.0, 2.0, 8.0, 18.0},
                                               {2.0, 2.0, 5.0, 15.0}}));
  std::unique_ptr<Polygon> xyzm_poly = factory->createPolygon(
      std::move(xyzm_shell), std::move(holes_with_empty));
  xyzm_poly->normalize();

  const CoordinateSequence* norm_shell =
      xyzm_poly->getExteriorRing()->getCoordinatesRO();
  EXPECT_TRUE(norm_shell->hasZ());
  EXPECT_TRUE(norm_shell->hasM());
  EXPECT_DOUBLE_EQ(norm_shell->getX(0), 0.0);
  EXPECT_DOUBLE_EQ(norm_shell->getY(0), 0.0);
  EXPECT_DOUBLE_EQ(norm_shell->getOrdinate(0, CoordinateSequence::Z), 4.0);
  EXPECT_DOUBLE_EQ(norm_shell->getOrdinate(0, CoordinateSequence::M), 14.0);
  // Non-empty hole sorts before empty hole (compareTo > 0).
  EXPECT_FALSE(xyzm_poly->getInteriorRingN(0)->isEmpty());
  EXPECT_TRUE(xyzm_poly->getInteriorRingN(1)->isEmpty());
}

}  // namespace
}  // namespace geos_google
