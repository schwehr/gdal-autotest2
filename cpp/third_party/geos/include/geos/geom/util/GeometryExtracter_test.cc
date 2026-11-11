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

#include "third_party/geos/include/geos/geom/util/GeometryExtracter.h"

#include <deque>
#include <initializer_list>
#include <list>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
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
#include "third_party/geos/include/geos/geom/Surface.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

class GeometryExtracterTest : public ::testing::Test {
 protected:
  geos::geom::GeometryFactory::Ptr factory_ =
      geos::geom::GeometryFactory::create();
  geos::io::WKTReader reader_{*factory_};
};

TEST_F(GeometryExtracterTest, ExtractsMatchingSingletonDirectly) {
  auto pt = reader_.read("POINT (1 2)");
  std::vector<const geos::geom::Point*> points;
  geos::geom::util::GeometryExtracter::extract<geos::geom::Point>(*pt, points);
  ASSERT_EQ(points.size(), 1u);
  EXPECT_EQ(points[0], pt.get());

  auto empty_pt = reader_.read("POINT EMPTY");
  points.clear();
  geos::geom::util::GeometryExtracter::extract<geos::geom::Point>(*empty_pt,
                                                                  points);
  ASSERT_EQ(points.size(), 1u);
  EXPECT_EQ(points[0], empty_pt.get());

  auto line = reader_.read("LINESTRING (0 0, 1 1)");
  std::vector<const geos::geom::LineString*> lines;
  geos::geom::util::GeometryExtracter::extract<geos::geom::LineString>(*line,
                                                                       lines);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], line.get());

  auto ring_coords = std::make_unique<geos::geom::CoordinateSequence>(
      std::initializer_list<geos::geom::CoordinateXY>{
          {0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 0.0}});
  auto ring = factory_->createLinearRing(std::move(ring_coords));

  std::vector<const geos::geom::LinearRing*> rings;
  geos::geom::util::GeometryExtracter::extract<geos::geom::LinearRing>(*ring,
                                                                       rings);
  ASSERT_EQ(rings.size(), 1u);
  EXPECT_EQ(rings[0], ring.get());

  lines.clear();
  geos::geom::util::GeometryExtracter::extract<geos::geom::LineString>(*ring,
                                                                       lines);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], ring.get());

  auto poly = reader_.read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  std::vector<const geos::geom::Surface*> surfaces;
  geos::geom::util::GeometryExtracter::extract<geos::geom::Surface>(*poly,
                                                                    surfaces);
  ASSERT_EQ(surfaces.size(), 1u);
  EXPECT_EQ(surfaces[0], poly.get());
}

TEST_F(GeometryExtracterTest, IgnoresNonMatchingNonCollectionGeometry) {
  auto pt = reader_.read("POINT (5 6)");
  std::vector<const geos::geom::Polygon*> polys;
  geos::geom::util::GeometryExtracter::extract<geos::geom::Polygon>(*pt, polys);
  EXPECT_TRUE(polys.empty());

  auto line = reader_.read("LINESTRING (0 0, 2 2)");
  std::vector<const geos::geom::Point*> points;
  geos::geom::util::GeometryExtracter::extract<geos::geom::Point>(*line,
                                                                  points);
  EXPECT_TRUE(points.empty());

  // Polygon is not a GeometryCollection, so rings are not extracted as lines.
  auto poly = reader_.read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 4 2, 4 4, 2 4, 2 2))");
  std::vector<const geos::geom::LineString*> lines;
  geos::geom::util::GeometryExtracter::extract<geos::geom::LineString>(*poly,
                                                                       lines);
  EXPECT_TRUE(lines.empty());
}

TEST_F(GeometryExtracterTest, ExtractsFromHomogeneousAndCurvedCollections) {
  auto mpt = reader_.read("MULTIPOINT ((1 2), (3 4), (5 6))");
  std::vector<const geos::geom::Point*> points;
  geos::geom::util::GeometryExtracter::extract<geos::geom::Point>(*mpt, points);
  ASSERT_EQ(points.size(), 3u);
  EXPECT_DOUBLE_EQ(points[0]->getX(), 1.0);
  EXPECT_DOUBLE_EQ(points[1]->getX(), 3.0);
  EXPECT_DOUBLE_EQ(points[2]->getX(), 5.0);

  auto mls = reader_.read("MULTILINESTRING ((0 0, 1 1), (2 2, 3 3))");
  std::vector<const geos::geom::LineString*> lines;
  geos::geom::util::GeometryExtracter::extract<geos::geom::LineString>(*mls,
                                                                       lines);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0], mls->getGeometryN(0));
  EXPECT_EQ(lines[1], mls->getGeometryN(1));

  auto mpoly = reader_.read(
      "MULTIPOLYGON ("
      "((0 0, 1 0, 1 1, 0 1, 0 0)), "
      "((2 2, 4 2, 4 4, 2 4, 2 2)))");
  std::vector<const geos::geom::Polygon*> polys;
  geos::geom::util::GeometryExtracter::extract<geos::geom::Polygon>(*mpoly,
                                                                    polys);
  ASSERT_EQ(polys.size(), 2u);
  EXPECT_DOUBLE_EQ(polys[0]->getArea(), 1.0);
  EXPECT_DOUBLE_EQ(polys[1]->getArea(), 4.0);

  auto multicurve =
      reader_.read("MULTICURVE ((0 0, 1 1), CIRCULARSTRING (1 1, 2 2, 3 1))");
  std::vector<const geos::geom::CircularString*> circular_strings;
  geos::geom::util::GeometryExtracter::extract<geos::geom::CircularString>(
      *multicurve, circular_strings);
  ASSERT_EQ(circular_strings.size(), 1u);
  EXPECT_EQ(circular_strings[0], multicurve->getGeometryN(1));

  std::vector<const geos::geom::Curve*> curves;
  geos::geom::util::GeometryExtracter::extract<geos::geom::Curve>(*multicurve,
                                                                  curves);
  ASSERT_EQ(curves.size(), 2u);
  EXPECT_EQ(curves[0], multicurve->getGeometryN(0));
  EXPECT_EQ(curves[1], multicurve->getGeometryN(1));

  auto multisurface = reader_.read(
      "MULTISURFACE ("
      "((0 0, 1 0, 1 1, 0 1, 0 0)), "
      "CURVEPOLYGON (CIRCULARSTRING (0 0, 1 1, 2 0, 1 -1, 0 0)))");
  polys.clear();
  geos::geom::util::GeometryExtracter::extract<geos::geom::Polygon>(
      *multisurface, polys);
  ASSERT_EQ(polys.size(), 1u);
  EXPECT_EQ(polys[0], multisurface->getGeometryN(0));

  std::vector<const geos::geom::Surface*> surfaces;
  geos::geom::util::GeometryExtracter::extract<geos::geom::Surface>(
      *multisurface, surfaces);
  ASSERT_EQ(surfaces.size(), 2u);
  EXPECT_EQ(surfaces[0], multisurface->getGeometryN(0));
  EXPECT_EQ(surfaces[1], multisurface->getGeometryN(1));

  std::vector<const geos::geom::CurvePolygon*> curve_polygons;
  geos::geom::util::GeometryExtracter::extract<geos::geom::CurvePolygon>(
      *multisurface, curve_polygons);
  ASSERT_EQ(curve_polygons.size(), 1u);
  EXPECT_EQ(curve_polygons[0], multisurface->getGeometryN(1));

  std::vector<const geos::geom::MultiCurve*> multi_curves;
  geos::geom::util::GeometryExtracter::extract<geos::geom::MultiCurve>(
      *multicurve, multi_curves);
  ASSERT_EQ(multi_curves.size(), 1u);
  EXPECT_EQ(multi_curves[0], multicurve.get());

  std::vector<const geos::geom::MultiSurface*> multi_surfaces;
  geos::geom::util::GeometryExtracter::extract<geos::geom::MultiSurface>(
      *multisurface, multi_surfaces);
  ASSERT_EQ(multi_surfaces.size(), 1u);
  EXPECT_EQ(multi_surfaces[0], multisurface.get());
}

TEST_F(GeometryExtracterTest, ExtractsFromNestedHeterogeneousCollections) {
  auto nested = reader_.read(
      "GEOMETRYCOLLECTION ("
      "POINT (1 1), "
      "MULTIPOINT ((2 2), EMPTY), "
      "LINESTRING (0 0, 3 3), "
      "GEOMETRYCOLLECTION ("
      "  POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "  MULTILINESTRING ((1 0, 1 2), (2 0, 2 2)), "
      "  MULTIPOLYGON (((10 10, 11 10, 11 11, 10 11, 10 10))), "
      "  GEOMETRYCOLLECTION EMPTY), "
      "POINT (9 9))");

  std::vector<const geos::geom::Point*> points;
  geos::geom::util::GeometryExtracter::extract<geos::geom::Point>(*nested,
                                                                  points);
  ASSERT_EQ(points.size(), 4u);
  EXPECT_DOUBLE_EQ(points[0]->getX(), 1.0);
  EXPECT_DOUBLE_EQ(points[1]->getX(), 2.0);
  EXPECT_TRUE(points[2]->isEmpty());
  EXPECT_DOUBLE_EQ(points[3]->getX(), 9.0);

  std::vector<const geos::geom::LineString*> lines;
  geos::geom::util::GeometryExtracter::extract<geos::geom::LineString>(*nested,
                                                                       lines);
  ASSERT_EQ(lines.size(), 3u);

  std::vector<const geos::geom::Polygon*> polys;
  geos::geom::util::GeometryExtracter::extract<geos::geom::Polygon>(*nested,
                                                                    polys);
  ASSERT_EQ(polys.size(), 2u);
  EXPECT_DOUBLE_EQ(polys[0]->getArea(), 4.0);
  EXPECT_DOUBLE_EQ(polys[1]->getArea(), 1.0);

  std::vector<const geos::geom::MultiPoint*> mpts;
  geos::geom::util::GeometryExtracter::extract<geos::geom::MultiPoint>(*nested,
                                                                       mpts);
  ASSERT_EQ(mpts.size(), 1u);
  EXPECT_EQ(mpts[0], nested->getGeometryN(1));

  std::vector<const geos::geom::MultiLineString*> mlss;
  geos::geom::util::GeometryExtracter::extract<geos::geom::MultiLineString>(
      *nested, mlss);
  ASSERT_EQ(mlss.size(), 1u);
  EXPECT_EQ(mlss[0], nested->getGeometryN(3)->getGeometryN(1));

  std::vector<const geos::geom::MultiPolygon*> mpolys;
  geos::geom::util::GeometryExtracter::extract<geos::geom::MultiPolygon>(
      *nested, mpolys);
  ASSERT_EQ(mpolys.size(), 1u);
  EXPECT_EQ(mpolys[0], nested->getGeometryN(3)->getGeometryN(2));
}

TEST_F(GeometryExtracterTest, DoesNotRecurseWhenTopLevelMatchesComponentType) {
  auto mpt = reader_.read("MULTIPOINT ((1 2), (3 4))");
  std::vector<const geos::geom::MultiPoint*> mpts;
  geos::geom::util::GeometryExtracter::extract<geos::geom::MultiPoint>(*mpt,
                                                                       mpts);
  ASSERT_EQ(mpts.size(), 1u);
  EXPECT_EQ(mpts[0], mpt.get());

  auto nested = reader_.read(
      "GEOMETRYCOLLECTION ("
      "GEOMETRYCOLLECTION (POINT (1 2)), "
      "MULTIPOINT ((3 4)))");
  std::vector<const geos::geom::GeometryCollection*> gcs;
  geos::geom::util::GeometryExtracter::extract<geos::geom::GeometryCollection>(
      *nested, gcs);
  ASSERT_EQ(gcs.size(), 1u);
  EXPECT_EQ(gcs[0], nested.get());

  std::vector<const geos::geom::Geometry*> geoms;
  geos::geom::util::GeometryExtracter::extract<geos::geom::Geometry>(*nested,
                                                                     geoms);
  ASSERT_EQ(geoms.size(), 1u);
  EXPECT_EQ(geoms[0], nested.get());
}

TEST_F(GeometryExtracterTest, EmptyCollectionsYieldNoSubComponents) {
  std::vector<const geos::geom::Point*> points;
  for (const char* wkt : {"MULTIPOINT EMPTY", "MULTILINESTRING EMPTY",
                          "MULTIPOLYGON EMPTY", "GEOMETRYCOLLECTION EMPTY"}) {
    auto geom = reader_.read(wkt);
    geos::geom::util::GeometryExtracter::extract<geos::geom::Point>(*geom,
                                                                    points);
  }
  EXPECT_TRUE(points.empty());
}

TEST_F(GeometryExtracterTest, SupportsGenericTargetContainersAndAppending) {
  auto initial_pt = reader_.read("POINT (0 0)");
  auto gc = reader_.read(
      "GEOMETRYCOLLECTION ("
      "POINT (1 1), "
      "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "POINT (2 2))");

  std::vector<const geos::geom::Geometry*> geom_vec;
  geom_vec.push_back(initial_pt.get());
  geos::geom::util::GeometryExtracter::extract<geos::geom::Point>(*gc,
                                                                  geom_vec);
  ASSERT_EQ(geom_vec.size(), 3u);
  EXPECT_EQ(geom_vec[0], initial_pt.get());
  EXPECT_EQ(geom_vec[1], gc->getGeometryN(0));
  EXPECT_EQ(geom_vec[2], gc->getGeometryN(2));

  std::deque<const geos::geom::Point*> pt_deque;
  geos::geom::util::GeometryExtracter::extract<geos::geom::Point>(*gc,
                                                                  pt_deque);
  ASSERT_EQ(pt_deque.size(), 2u);
  EXPECT_EQ(pt_deque[0], gc->getGeometryN(0));
  EXPECT_EQ(pt_deque[1], gc->getGeometryN(2));

  std::list<const geos::geom::Polygon*> poly_list;
  geos::geom::util::GeometryExtracter::extract<geos::geom::Polygon>(*gc,
                                                                    poly_list);
  ASSERT_EQ(poly_list.size(), 1u);
  EXPECT_EQ(poly_list.front(), gc->getGeometryN(1));
}

TEST_F(GeometryExtracterTest, NonCopyable) {
  static_assert(
      !std::is_copy_constructible_v<geos::geom::util::GeometryExtracter>);
  static_assert(
      !std::is_copy_assignable_v<geos::geom::util::GeometryExtracter>);
}

}  // namespace
}  // namespace geos_google
