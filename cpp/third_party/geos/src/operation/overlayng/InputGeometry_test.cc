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

#include "third_party/geos/include/geos/operation/overlayng/InputGeometry.h"

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/locate/PointOnGeometryLocator.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::algorithm::locate::PointOnGeometryLocator;
using geos::geom::Coordinate;
using geos::geom::Dimension;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::io::WKTReader;
using geos::operation::overlayng::InputGeometry;

class InputGeometryTest : public ::testing::Test {
 protected:
  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader reader_{*factory_};
};

TEST_F(InputGeometryTest, SingleGeometryModeWithNullSecondInput) {
  auto poly = reader_.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  InputGeometry ig(poly.get(), nullptr);

  EXPECT_TRUE(ig.isSingle());
  EXPECT_EQ(ig.getGeometry(0), poly.get());
  EXPECT_EQ(ig.getGeometry(1), nullptr);
  EXPECT_EQ(ig.getEnvelope(0), poly->getEnvelopeInternal());
  EXPECT_FALSE(ig.isEmpty(0));

  EXPECT_EQ(ig.getDimension(0), static_cast<int>(Dimension::A));
  EXPECT_EQ(ig.getDimension(1), -1);
  EXPECT_EQ(ig.getCoordinateDimension(0), 2);
  EXPECT_EQ(ig.getCoordinateDimension(1), 0);

  EXPECT_TRUE(ig.isArea(0));
  EXPECT_FALSE(ig.isArea(1));
  EXPECT_EQ(ig.getAreaIndex(), 0);
  EXPECT_FALSE(ig.isLine(0));
  EXPECT_FALSE(ig.isLine(1));
  EXPECT_FALSE(ig.isAllPoints());
  EXPECT_FALSE(ig.hasPoints());
  EXPECT_TRUE(ig.hasEdges(0));
  EXPECT_FALSE(ig.hasEdges(1));

  // Single point input is not "all points" because second input is null.
  auto pt = reader_.read("POINT (1 2)");
  InputGeometry single_pt(pt.get(), nullptr);
  EXPECT_TRUE(single_pt.isSingle());
  EXPECT_FALSE(single_pt.isAllPoints());
  EXPECT_TRUE(single_pt.hasPoints());
  EXPECT_FALSE(single_pt.hasEdges(0));
  EXPECT_EQ(single_pt.getAreaIndex(), -1);

  // Null first geometry handled safely by null-checking accessors.
  InputGeometry null_first(nullptr, poly.get());
  EXPECT_FALSE(null_first.isSingle());
  EXPECT_EQ(null_first.getDimension(0), -1);
  EXPECT_EQ(null_first.getCoordinateDimension(0), 0);
  EXPECT_FALSE(null_first.isArea(0));
  EXPECT_TRUE(null_first.isArea(1));
  EXPECT_EQ(null_first.getAreaIndex(), 1);
  EXPECT_FALSE(null_first.hasEdges(0));
  EXPECT_TRUE(null_first.hasEdges(1));
}

TEST_F(InputGeometryTest, TwoGeometriesDimensionsEnvelopesAndCoordDim) {
  auto line_xy = reader_.read("LINESTRING (0 0, 5 5)");
  auto poly_xyz =
      reader_.read("POLYGON Z ((1 1 2, 4 1 2, 4 4 2, 1 4 2, 1 1 2))");
  InputGeometry ig(line_xy.get(), poly_xyz.get());

  EXPECT_FALSE(ig.isSingle());
  EXPECT_EQ(ig.getGeometry(0), line_xy.get());
  EXPECT_EQ(ig.getGeometry(1), poly_xyz.get());
  EXPECT_EQ(ig.getEnvelope(0), line_xy->getEnvelopeInternal());
  EXPECT_EQ(ig.getEnvelope(1), poly_xyz->getEnvelopeInternal());
  EXPECT_FALSE(ig.isEmpty(0));
  EXPECT_FALSE(ig.isEmpty(1));

  EXPECT_EQ(ig.getDimension(0), static_cast<int>(Dimension::L));
  EXPECT_EQ(ig.getDimension(1), static_cast<int>(Dimension::A));
  EXPECT_EQ(ig.getCoordinateDimension(0), 2);
  EXPECT_EQ(ig.getCoordinateDimension(1), 3);

  EXPECT_FALSE(ig.isArea(0));
  EXPECT_TRUE(ig.isArea(1));
  EXPECT_EQ(ig.getAreaIndex(), 1);
  EXPECT_TRUE(ig.isLine(0));
  EXPECT_FALSE(ig.isLine(1));
  EXPECT_FALSE(ig.isAllPoints());
  EXPECT_FALSE(ig.hasPoints());
  EXPECT_TRUE(ig.hasEdges(0));
  EXPECT_TRUE(ig.hasEdges(1));

  auto pt_xym = reader_.read("POINT M (1 2 3)");
  auto pt_xyzm = reader_.read("POINT ZM (1 2 3 4)");
  InputGeometry dims(pt_xym.get(), pt_xyzm.get());
  EXPECT_EQ(dims.getCoordinateDimension(0), 3);
  EXPECT_EQ(dims.getCoordinateDimension(1), 4);
}

TEST_F(InputGeometryTest, AreaIndexPriorityAndMultiGeometries) {
  auto poly_a = reader_.read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  auto multi_poly = reader_.read(
      "MULTIPOLYGON (((0 0, 1 0, 1 1, 0 1, 0 0)),"
      " ((5 5, 6 5, 6 6, 5 6, 5 5)))");
  auto multi_line = reader_.read("MULTILINESTRING ((0 0, 2 2), (3 3, 4 4))");
  auto pt = reader_.read("POINT (0 0)");

  // Both inputs areal -> returns index 0.
  InputGeometry both_areas(poly_a.get(), multi_poly.get());
  EXPECT_TRUE(both_areas.isArea(0));
  EXPECT_TRUE(both_areas.isArea(1));
  EXPECT_EQ(both_areas.getAreaIndex(), 0);

  // MultiPolygon and MultiLineString classification.
  InputGeometry area_and_line(multi_poly.get(), multi_line.get());
  EXPECT_EQ(area_and_line.getAreaIndex(), 0);
  EXPECT_TRUE(area_and_line.isLine(1));

  // No areal input -> -1.
  InputGeometry no_area(pt.get(), multi_line.get());
  EXPECT_EQ(no_area.getAreaIndex(), -1);
}

TEST_F(InputGeometryTest, PointClassificationPredicates) {
  auto pt_a = reader_.read("POINT (0 0)");
  auto multi_pt = reader_.read("MULTIPOINT ((1 1), (2 2))");
  auto line = reader_.read("LINESTRING (0 0, 3 3)");

  InputGeometry all_pts(pt_a.get(), multi_pt.get());
  EXPECT_TRUE(all_pts.isAllPoints());
  EXPECT_TRUE(all_pts.hasPoints());
  EXPECT_FALSE(all_pts.hasEdges(0));
  EXPECT_FALSE(all_pts.hasEdges(1));

  InputGeometry first_pt(pt_a.get(), line.get());
  EXPECT_FALSE(first_pt.isAllPoints());
  EXPECT_TRUE(first_pt.hasPoints());
  EXPECT_FALSE(first_pt.hasEdges(0));
  EXPECT_TRUE(first_pt.hasEdges(1));

  InputGeometry second_pt(line.get(), pt_a.get());
  EXPECT_FALSE(second_pt.isAllPoints());
  EXPECT_TRUE(second_pt.hasPoints());
  EXPECT_TRUE(second_pt.hasEdges(0));
  EXPECT_FALSE(second_pt.hasEdges(1));
}

TEST_F(InputGeometryTest, EmptyGeometriesAndEmptyLocatePointInArea) {
  auto empty_poly = reader_.read("POLYGON EMPTY");
  auto empty_gc = reader_.read("GEOMETRYCOLLECTION EMPTY");
  InputGeometry ig(empty_poly.get(), empty_gc.get());

  EXPECT_TRUE(ig.isEmpty(0));
  EXPECT_TRUE(ig.isEmpty(1));
  EXPECT_EQ(ig.getDimension(0), static_cast<int>(Dimension::A));
  EXPECT_EQ(ig.getDimension(1), static_cast<int>(Dimension::False));
  EXPECT_EQ(ig.locatePointInArea(0, Coordinate(0.0, 0.0)), Location::EXTERIOR);
}

TEST_F(InputGeometryTest, LocatePointInAreaAndLocatorCaching) {
  auto poly_with_hole = reader_.read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0),"
      " (2 2, 4 2, 4 4, 2 4, 2 2))");
  auto poly_b = reader_.read("POLYGON ((5 5, 15 5, 15 15, 5 15, 5 5))");
  InputGeometry ig(poly_with_hole.get(), poly_b.get());

  EXPECT_EQ(ig.locatePointInArea(0, Coordinate(1.0, 1.0)), Location::INTERIOR);
  EXPECT_EQ(ig.locatePointInArea(0, Coordinate(3.0, 3.0)), Location::EXTERIOR);
  EXPECT_EQ(ig.locatePointInArea(0, Coordinate(2.0, 3.0)), Location::BOUNDARY);
  EXPECT_EQ(ig.locatePointInArea(0, Coordinate(0.0, 5.0)), Location::BOUNDARY);
  EXPECT_EQ(ig.locatePointInArea(0, Coordinate(20.0, 20.0)),
            Location::EXTERIOR);

  EXPECT_EQ(ig.locatePointInArea(1, Coordinate(10.0, 10.0)),
            Location::INTERIOR);
  EXPECT_EQ(ig.locatePointInArea(1, Coordinate(5.0, 10.0)), Location::BOUNDARY);
  EXPECT_EQ(ig.locatePointInArea(1, Coordinate(1.0, 1.0)), Location::EXTERIOR);

  // Repeated getLocator calls return cached locators for each index.
  PointOnGeometryLocator* loc_a1 = ig.getLocator(0);
  PointOnGeometryLocator* loc_a2 = ig.getLocator(0);
  PointOnGeometryLocator* loc_b1 = ig.getLocator(1);
  PointOnGeometryLocator* loc_b2 = ig.getLocator(1);
  ASSERT_NE(loc_a1, nullptr);
  ASSERT_NE(loc_b1, nullptr);
  EXPECT_EQ(loc_a1, loc_a2);
  EXPECT_EQ(loc_b1, loc_b2);
  EXPECT_NE(loc_a1, loc_b1);

  Coordinate interior_a(1.0, 1.0);
  EXPECT_EQ(loc_a1->locate(&interior_a), Location::INTERIOR);
}

TEST_F(InputGeometryTest, SetCollapsedOverridesLocatePointInArea) {
  auto poly_a = reader_.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto poly_b = reader_.read("POLYGON ((20 20, 30 20, 30 30, 20 30, 20 20))");
  InputGeometry ig(poly_a.get(), poly_b.get());

  const Coordinate pt_a(5.0, 5.0);
  const Coordinate pt_b(25.0, 25.0);

  EXPECT_EQ(ig.locatePointInArea(0, pt_a), Location::INTERIOR);
  EXPECT_EQ(ig.locatePointInArea(1, pt_b), Location::INTERIOR);

  ig.setCollapsed(0, true);
  EXPECT_EQ(ig.locatePointInArea(0, pt_a), Location::EXTERIOR);
  EXPECT_EQ(ig.locatePointInArea(1, pt_b), Location::INTERIOR);

  ig.setCollapsed(1, true);
  EXPECT_EQ(ig.locatePointInArea(1, pt_b), Location::EXTERIOR);

  ig.setCollapsed(0, false);
  EXPECT_EQ(ig.locatePointInArea(0, pt_a), Location::INTERIOR);
  EXPECT_EQ(ig.locatePointInArea(1, pt_b), Location::EXTERIOR);
}

}  // namespace
}  // namespace geos_google
