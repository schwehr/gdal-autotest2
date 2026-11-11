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

// Unit tests for
// third_party/geos/src/geom/util/ComponentCoordinateExtracter.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/util/ComponentCoordinateExtracter.cpp

#include "third_party/geos/include/geos/geom/util/ComponentCoordinateExtracter.h"

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
#include "third_party/geos/include/geos/geom/GeometryComponentFilter.h"
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
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Curve;
using geos::geom::Geometry;
using geos::geom::GeometryComponentFilter;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::SimpleCurve;
using geos::geom::Surface;
using geos::geom::util::ComponentCoordinateExtracter;
using ::testing::ElementsAre;

static_assert(
    std::is_base_of_v<GeometryComponentFilter, ComponentCoordinateExtracter>);
static_assert(!std::is_copy_constructible_v<ComponentCoordinateExtracter>);
static_assert(!std::is_copy_assignable_v<ComponentCoordinateExtracter>);

std::unique_ptr<LineString> MakeLineString(
    const GeometryFactory& factory,
    std::initializer_list<CoordinateXY> coords) {
  auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
  for (const auto& coord : coords) {
    seq->add(coord);
  }
  return factory.createLineString(std::move(seq));
}

std::unique_ptr<LinearRing> MakeRing(
    const GeometryFactory& factory,
    std::initializer_list<CoordinateXY> coords) {
  auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
  for (const auto& coord : coords) {
    seq->add(coord);
  }
  return factory.createLinearRing(std::move(seq));
}

std::unique_ptr<CircularString> MakeCircularString(
    const GeometryFactory& factory,
    std::initializer_list<CoordinateXY> coords) {
  auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
  for (const auto& coord : coords) {
    seq->add(coord);
  }
  return factory.createCircularString(std::move(seq));
}

TEST(ComponentCoordinateExtracterTest, EmptyGeometriesYieldNoCoordinates) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::vector<std::unique_ptr<Geometry>> empties;
  empties.push_back(factory->createPoint());
  empties.push_back(factory->createLineString());
  empties.push_back(factory->createLinearRing());
  empties.push_back(factory->createPolygon());
  empties.push_back(factory->createMultiPoint());
  empties.push_back(factory->createMultiLineString());
  empties.push_back(factory->createMultiPolygon());
  empties.push_back(factory->createGeometryCollection());
  empties.push_back(factory->createCircularString(false, false));
  empties.push_back(factory->createCompoundCurve());
  empties.push_back(factory->createCurvePolygon(false, false));
  empties.push_back(factory->createMultiCurve());
  empties.push_back(factory->createMultiSurface());

  for (const auto& geom : empties) {
    std::vector<const CoordinateXY*> ro_coords;
    ComponentCoordinateExtracter::getCoordinates(*geom, ro_coords);
    EXPECT_TRUE(ro_coords.empty()) << geom->getGeometryType();

    std::vector<const CoordinateXY*> rw_coords;
    ComponentCoordinateExtracter rw_extractor(rw_coords);
    geom->apply_rw(&rw_extractor);
    EXPECT_TRUE(rw_coords.empty()) << geom->getGeometryType();
  }
}

TEST(ComponentCoordinateExtracterTest,
     ExtractsFromPointLineStringLinearRingAndPolygon) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto pt = factory->createPoint(CoordinateXY(1.5, 2.5));
  auto line = MakeLineString(*factory, {{3.0, 4.0}, {5.0, 6.0}, {7.0, 8.0}});
  auto ring =
      MakeRing(*factory, {{10, 10}, {20, 10}, {20, 20}, {10, 20}, {10, 10}});

  auto shell = MakeRing(*factory, {{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}});
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(MakeRing(*factory, {{1, 1}, {2, 1}, {2, 2}, {1, 2}, {1, 1}}));
  holes.push_back(MakeRing(*factory, {{5, 5}, {6, 5}, {6, 6}, {5, 6}, {5, 5}}));
  auto poly = factory->createPolygon(std::move(shell), std::move(holes));

  {
    std::vector<const CoordinateXY*> ro_pt;
    ComponentCoordinateExtracter::getCoordinates(*pt, ro_pt);
    EXPECT_THAT(ro_pt, ElementsAre(pt->getCoordinate()));

    std::vector<const CoordinateXY*> rw_pt;
    ComponentCoordinateExtracter rw_filter(rw_pt);
    pt->apply_rw(&rw_filter);
    EXPECT_THAT(rw_pt, ElementsAre(pt->getCoordinate()));
  }

  {
    std::vector<const CoordinateXY*> ro_line;
    ComponentCoordinateExtracter::getCoordinates(*line, ro_line);
    EXPECT_THAT(ro_line, ElementsAre(line->getCoordinate()));

    std::vector<const CoordinateXY*> rw_line;
    ComponentCoordinateExtracter rw_filter(rw_line);
    line->apply_rw(&rw_filter);
    EXPECT_THAT(rw_line, ElementsAre(line->getCoordinate()));
  }

  {
    std::vector<const CoordinateXY*> ro_ring;
    ComponentCoordinateExtracter::getCoordinates(*ring, ro_ring);
    EXPECT_THAT(ro_ring, ElementsAre(ring->getCoordinate()));

    std::vector<const CoordinateXY*> rw_ring;
    ComponentCoordinateExtracter rw_filter(rw_ring);
    ring->apply_rw(&rw_filter);
    EXPECT_THAT(rw_ring, ElementsAre(ring->getCoordinate()));
  }

  {
    const CoordinateXY* expected_shell =
        poly->getExteriorRing()->getCoordinate();
    const CoordinateXY* expected_h0 =
        poly->getInteriorRingN(0)->getCoordinate();
    const CoordinateXY* expected_h1 =
        poly->getInteriorRingN(1)->getCoordinate();

    std::vector<const CoordinateXY*> ro_poly;
    ComponentCoordinateExtracter::getCoordinates(*poly, ro_poly);
    EXPECT_THAT(ro_poly, ElementsAre(expected_shell, expected_h0, expected_h1));

    std::vector<const CoordinateXY*> rw_poly;
    ComponentCoordinateExtracter rw_filter(rw_poly);
    poly->apply_rw(&rw_filter);
    EXPECT_THAT(rw_poly, ElementsAre(expected_shell, expected_h0, expected_h1));
  }
}

TEST(ComponentCoordinateExtracterTest,
     ExtractsFromMultiGeometriesAndNestedCollections) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::vector<std::unique_ptr<Point>> mp_pts;
  mp_pts.push_back(factory->createPoint(CoordinateXY(1.0, 2.0)));
  mp_pts.push_back(factory->createPoint());  // Empty point must be skipped.
  mp_pts.push_back(factory->createPoint(CoordinateXY(3.0, 4.0)));
  auto multi_pt = factory->createMultiPoint(std::move(mp_pts));

  std::vector<std::unique_ptr<LineString>> mls_lines;
  mls_lines.push_back(MakeLineString(*factory, {{10.0, 11.0}, {12.0, 13.0}}));
  mls_lines.push_back(
      factory->createLineString());  // Empty line must be skipped.
  mls_lines.push_back(MakeLineString(*factory, {{14.0, 15.0}, {16.0, 17.0}}));
  auto multi_line = factory->createMultiLineString(std::move(mls_lines));

  std::vector<std::unique_ptr<Polygon>> mp_polys;
  mp_polys.push_back(factory->createPolygon(
      MakeRing(*factory, {{0, 0}, {5, 0}, {5, 5}, {0, 5}, {0, 0}})));
  mp_polys.push_back(
      factory->createPolygon());  // Empty polygon must be skipped.
  std::vector<std::unique_ptr<LinearRing>> poly2_holes;
  poly2_holes.push_back(
      MakeRing(*factory, {{21, 21}, {22, 21}, {22, 22}, {21, 22}, {21, 21}}));
  mp_polys.push_back(factory->createPolygon(
      MakeRing(*factory, {{20, 20}, {30, 20}, {30, 30}, {20, 30}, {20, 20}}),
      std::move(poly2_holes)));
  auto multi_poly = factory->createMultiPolygon(std::move(mp_polys));

  std::vector<std::unique_ptr<Geometry>> collection_items;
  collection_items.push_back(std::move(multi_pt));
  collection_items.push_back(std::move(multi_line));
  collection_items.push_back(std::move(multi_poly));
  auto collection =
      factory->createGeometryCollection(std::move(collection_items));

  std::vector<const CoordinateXY*> ro_coords;
  ComponentCoordinateExtracter::getCoordinates(*collection, ro_coords);
  ASSERT_EQ(ro_coords.size(), 7u);
  EXPECT_TRUE(ro_coords[0]->equals2D(CoordinateXY(1.0, 2.0)));
  EXPECT_TRUE(ro_coords[1]->equals2D(CoordinateXY(3.0, 4.0)));
  EXPECT_TRUE(ro_coords[2]->equals2D(CoordinateXY(10.0, 11.0)));
  EXPECT_TRUE(ro_coords[3]->equals2D(CoordinateXY(14.0, 15.0)));
  EXPECT_TRUE(ro_coords[4]->equals2D(CoordinateXY(0.0, 0.0)));
  EXPECT_TRUE(ro_coords[5]->equals2D(CoordinateXY(20.0, 20.0)));
  EXPECT_TRUE(ro_coords[6]->equals2D(CoordinateXY(21.0, 21.0)));

  std::vector<const CoordinateXY*> rw_coords;
  ComponentCoordinateExtracter rw_filter(rw_coords);
  collection->apply_rw(&rw_filter);
  EXPECT_EQ(rw_coords, ro_coords);
}

TEST(ComponentCoordinateExtracterTest, IgnoresNonLinearNonPointGeometryTypes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto circ =
      MakeCircularString(*factory, {{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}});
  std::vector<std::unique_ptr<SimpleCurve>> compound_parts;
  compound_parts.push_back(
      MakeCircularString(*factory, {{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}}));
  compound_parts.push_back(MakeLineString(*factory, {{2.0, 0.0}, {4.0, 0.0}}));
  auto compound = factory->createCompoundCurve(std::move(compound_parts));

  // CurvePolygon whose shell is a closed CircularString visits only
  // CurvePolygon and CircularString (neither is Point/LineString/LinearRing).
  auto closed_circ = MakeCircularString(
      *factory, {{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}, {1.0, -1.0}, {0.0, 0.0}});
  auto curve_poly = factory->createCurvePolygon(std::move(closed_circ));

  for (Geometry* geom : std::initializer_list<Geometry*>{
           circ.get(), compound.get(), curve_poly.get()}) {
    std::vector<const CoordinateXY*> ro_coords;
    ComponentCoordinateExtracter::getCoordinates(*geom, ro_coords);
    EXPECT_TRUE(ro_coords.empty()) << geom->getGeometryType();

    std::vector<const CoordinateXY*> rw_coords;
    ComponentCoordinateExtracter rw_filter(rw_coords);
    geom->apply_rw(&rw_filter);
    EXPECT_TRUE(rw_coords.empty()) << geom->getGeometryType();
  }
}

TEST(ComponentCoordinateExtracterTest,
     AppendsToExistingVectorAndSupportsFilterReuse) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  CoordinateXY sentinel(99.0, -99.0);
  auto pt1 = factory->createPoint(CoordinateXY(1.0, 2.0));
  auto line = MakeLineString(*factory, {{3.0, 4.0}, {5.0, 6.0}});
  auto pt2 = factory->createPoint(CoordinateXY(7.0, 8.0));

  std::vector<const CoordinateXY*> coords = {&sentinel};
  ComponentCoordinateExtracter::getCoordinates(*pt1, coords);
  EXPECT_THAT(coords, ElementsAre(&sentinel, pt1->getCoordinate()));

  ComponentCoordinateExtracter reusable(coords);
  line->apply_ro(&reusable);
  pt2->apply_rw(&reusable);
  EXPECT_THAT(coords, ElementsAre(&sentinel, pt1->getCoordinate(),
                                  line->getCoordinate(), pt2->getCoordinate()));
}

TEST(ComponentCoordinateExtracterTest,
     ExtractsXYFromHigherDimensionGeometries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto pt_z = factory->createPoint(Coordinate(1.25, 2.5, 3.75));
  auto line_zm = factory->createLineString(CoordinateSequence{
      CoordinateXYZM(4.0, 5.0, 6.0, 7.0),
      CoordinateXYZM(8.0, 9.0, 10.0, 11.0),
  });
  auto pt_m = factory->createPoint(CoordinateXYM(12.0, 13.0, 14.0));

  std::vector<const CoordinateXY*> coords;
  ComponentCoordinateExtracter::getCoordinates(*pt_z, coords);
  ComponentCoordinateExtracter::getCoordinates(*line_zm, coords);
  ComponentCoordinateExtracter::getCoordinates(*pt_m, coords);

  ASSERT_EQ(coords.size(), 3u);
  EXPECT_TRUE(coords[0]->equals2D(CoordinateXY(1.25, 2.5)));
  EXPECT_TRUE(coords[1]->equals2D(CoordinateXY(4.0, 5.0)));
  EXPECT_TRUE(coords[2]->equals2D(CoordinateXY(12.0, 13.0)));
}

TEST(ComponentCoordinateExtracterTest, ExtractsFromMultiCurveAndMultiSurface) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::vector<std::unique_ptr<Curve>> curves;
  curves.push_back(MakeLineString(*factory, {{1.0, 2.0}, {3.0, 4.0}}));
  curves.push_back(factory->createLineString());
  curves.push_back(
      MakeCircularString(*factory, {{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}}));
  std::vector<std::unique_ptr<SimpleCurve>> compound_parts;
  compound_parts.push_back(MakeLineString(*factory, {{5.0, 6.0}, {7.0, 8.0}}));
  curves.push_back(factory->createCompoundCurve(std::move(compound_parts)));
  auto multi_curve = factory->createMultiCurve(std::move(curves));

  {
    std::vector<const CoordinateXY*> ro_coords;
    ComponentCoordinateExtracter::getCoordinates(*multi_curve, ro_coords);
    ASSERT_EQ(ro_coords.size(), 1u);
    EXPECT_TRUE(ro_coords[0]->equals2D(CoordinateXY(1.0, 2.0)));

    std::vector<const CoordinateXY*> rw_coords;
    ComponentCoordinateExtracter rw_filter(rw_coords);
    multi_curve->apply_rw(&rw_filter);
    EXPECT_EQ(rw_coords, ro_coords);
  }

  std::vector<std::unique_ptr<Surface>> surfaces;
  surfaces.push_back(factory->createPolygon(
      MakeRing(*factory, {{0, 0}, {4, 0}, {4, 4}, {0, 4}, {0, 0}})));
  surfaces.push_back(factory->createPolygon());
  surfaces.push_back(factory->createCurvePolygon(MakeCircularString(
      *factory,
      {{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}, {1.0, -1.0}, {0.0, 0.0}})));
  surfaces.push_back(factory->createCurvePolygon(
      MakeRing(*factory, {{10, 10}, {14, 10}, {14, 14}, {10, 14}, {10, 10}})));
  auto multi_surface = factory->createMultiSurface(std::move(surfaces));

  {
    std::vector<const CoordinateXY*> ro_coords;
    ComponentCoordinateExtracter::getCoordinates(*multi_surface, ro_coords);
    ASSERT_EQ(ro_coords.size(), 2u);
    EXPECT_TRUE(ro_coords[0]->equals2D(CoordinateXY(0.0, 0.0)));
    EXPECT_TRUE(ro_coords[1]->equals2D(CoordinateXY(10.0, 10.0)));

    std::vector<const CoordinateXY*> rw_coords;
    ComponentCoordinateExtracter rw_filter(rw_coords);
    multi_surface->apply_rw(&rw_filter);
    EXPECT_EQ(rw_coords, ro_coords);
  }
}

TEST(ComponentCoordinateExtracterTest,
     DirectFilterRoAndRwIgnoreContainerNodes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto poly = factory->createPolygon(
      MakeRing(*factory, {{0, 0}, {4, 0}, {4, 4}, {0, 4}, {0, 0}}));
  std::vector<std::unique_ptr<Point>> mp_pts;
  mp_pts.push_back(factory->createPoint(CoordinateXY(9.0, 9.0)));
  auto multi_pt = factory->createMultiPoint(std::move(mp_pts));

  auto pt = factory->createPoint(CoordinateXY(2.0, 3.0));
  auto line = MakeLineString(*factory, {{4.0, 5.0}, {6.0, 7.0}});
  auto ring = MakeRing(*factory, {{8, 8}, {9, 8}, {9, 9}, {8, 8}});

  auto empty_pt = factory->createPoint();
  auto empty_line = factory->createLineString();
  auto empty_ring = factory->createLinearRing();

  std::vector<const CoordinateXY*> coords;
  ComponentCoordinateExtracter filter(coords);
  EXPECT_FALSE(filter.isDone());

  // Direct calls bypass container/Surface apply_ro/rw recursion; Polygon and
  // MultiPoint are ignored, empty geometries are skipped, and Point,
  // LineString, and LinearRing are recorded.
  filter.filter_ro(poly.get());
  filter.filter_rw(poly.get());
  filter.filter_ro(multi_pt.get());
  filter.filter_rw(multi_pt.get());
  filter.filter_ro(empty_pt.get());
  filter.filter_rw(empty_pt.get());
  filter.filter_ro(empty_line.get());
  filter.filter_rw(empty_line.get());
  filter.filter_ro(empty_ring.get());
  filter.filter_rw(empty_ring.get());
  EXPECT_TRUE(coords.empty());

  filter.filter_ro(pt.get());
  filter.filter_rw(pt.get());
  filter.filter_ro(line.get());
  filter.filter_rw(line.get());
  filter.filter_ro(ring.get());
  filter.filter_rw(ring.get());
  EXPECT_THAT(coords,
              ElementsAre(pt->getCoordinate(), pt->getCoordinate(),
                          line->getCoordinate(), line->getCoordinate(),
                          ring->getCoordinate(), ring->getCoordinate()));
}

}  // namespace
}  // namespace geos_google
