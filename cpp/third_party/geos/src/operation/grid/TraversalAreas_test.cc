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

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
// TraversalAreas.h omits <memory> and geos::geom declarations, so it must be
// included after those headers.
#include "third_party/geos/include/geos/operation/grid/TraversalAreas.h"

namespace geos_google {
namespace {

using ::geos::geom::CoordinateXY;
using ::geos::geom::Envelope;
using ::geos::geom::Geometry;
using ::geos::geom::GeometryFactory;
using ::geos::geom::Polygon;
using ::geos::io::WKTReader;
using ::geos::operation::grid::TraversalAreas;

class TraversalAreasTest : public ::testing::Test {
 protected:
  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) const {
    WKTReader reader(*factory_);
    return reader.read(wkt);
  }

  GeometryFactory::Ptr factory_ = GeometryFactory::create();
};

TEST_F(TraversalAreasTest, ThrowsWhenCoordListsEmptyOrOnlyDegenerate) {
  const Envelope box(0.0, 10.0, 0.0, 10.0);

  // Empty coord_lists
  {
    const std::vector<const std::vector<CoordinateXY>*> empty_lists;
    EXPECT_THROW(TraversalAreas::getLeftHandArea(box, empty_lists),
                 std::runtime_error);
    EXPECT_THROW(TraversalAreas::getLeftHandRings(*factory_, box, empty_lists),
                 std::runtime_error);
  }

  // Only degenerate vectors (empty, 1-point, repeated identical points)
  {
    const std::vector<CoordinateXY> empty_vec;
    const std::vector<CoordinateXY> single_pt = {CoordinateXY(5.0, 5.0)};
    const std::vector<CoordinateXY> repeated_pts = {
        CoordinateXY(2.0, 2.0), CoordinateXY(2.0, 2.0), CoordinateXY(2.0, 2.0)};
    const std::vector<const std::vector<CoordinateXY>*> lists = {
        &empty_vec, &single_pt, &repeated_pts};

    EXPECT_THROW(TraversalAreas::getLeftHandArea(box, lists),
                 std::runtime_error);
    EXPECT_THROW(TraversalAreas::getLeftHandRings(*factory_, box, lists),
                 std::runtime_error);
  }
}

TEST_F(TraversalAreasTest, ThrowsWhenOpenChainEndpointsNotOnBoundary) {
  const Envelope box(0.0, 10.0, 0.0, 10.0);

  {
    const std::vector<CoordinateXY> bad_start = {CoordinateXY(1.0, 5.0),
                                                 CoordinateXY(10.0, 5.0)};
    const std::vector<const std::vector<CoordinateXY>*> lists = {&bad_start};
    EXPECT_THROW(TraversalAreas::getLeftHandArea(box, lists),
                 std::runtime_error);
    EXPECT_THROW(TraversalAreas::getLeftHandRings(*factory_, box, lists),
                 std::runtime_error);
  }

  {
    const std::vector<CoordinateXY> bad_end = {CoordinateXY(0.0, 5.0),
                                               CoordinateXY(11.0, 5.0)};
    const std::vector<const std::vector<CoordinateXY>*> lists = {&bad_end};
    EXPECT_THROW(TraversalAreas::getLeftHandArea(box, lists),
                 std::runtime_error);
    EXPECT_THROW(TraversalAreas::getLeftHandRings(*factory_, box, lists),
                 std::runtime_error);
  }
}

TEST_F(TraversalAreasTest, StraightTraversalsOnNonSquareBox) {
  // width = 6, height = 4, area = 24
  const Envelope box(2.0, 8.0, 1.0, 5.0);

  // Bottom -> Top at x = 4: left side is [2, 4] x [1, 5], area = 8
  const std::vector<CoordinateXY> b_to_t = {CoordinateXY(4.0, 1.0),
                                            CoordinateXY(4.0, 5.0)};
  const std::vector<CoordinateXY> t_to_b = {CoordinateXY(4.0, 5.0),
                                            CoordinateXY(4.0, 1.0)};
  // Left -> Right at y = 2.5: left side is [2, 8] x [2.5, 5], area = 15
  const std::vector<CoordinateXY> l_to_r = {CoordinateXY(2.0, 2.5),
                                            CoordinateXY(8.0, 2.5)};
  const std::vector<CoordinateXY> r_to_l = {CoordinateXY(8.0, 2.5),
                                            CoordinateXY(2.0, 2.5)};

  const double area_bt = TraversalAreas::getLeftHandArea(box, {&b_to_t});
  const double area_tb = TraversalAreas::getLeftHandArea(box, {&t_to_b});
  const double area_lr = TraversalAreas::getLeftHandArea(box, {&l_to_r});
  const double area_rl = TraversalAreas::getLeftHandArea(box, {&r_to_l});

  EXPECT_DOUBLE_EQ(area_bt, 8.0);
  EXPECT_DOUBLE_EQ(area_tb, 16.0);
  EXPECT_DOUBLE_EQ(area_bt + area_tb, box.getArea());
  EXPECT_DOUBLE_EQ(area_lr, 15.0);
  EXPECT_DOUBLE_EQ(area_rl, 9.0);
  EXPECT_DOUBLE_EQ(area_lr + area_rl, box.getArea());

  auto poly_bt = TraversalAreas::getLeftHandRings(*factory_, box, {&b_to_t});
  EXPECT_EQ(poly_bt->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(poly_bt->isValid());
  EXPECT_DOUBLE_EQ(poly_bt->getArea(), 8.0);
  EXPECT_TRUE(
      poly_bt->equals(ReadWkt("POLYGON ((2 1, 4 1, 4 5, 2 5, 2 1))").get()));

  auto poly_tb = TraversalAreas::getLeftHandRings(*factory_, box, {&t_to_b});
  EXPECT_TRUE(
      poly_tb->equals(ReadWkt("POLYGON ((4 1, 8 1, 8 5, 4 5, 4 1))").get()));

  auto poly_lr = TraversalAreas::getLeftHandRings(*factory_, box, {&l_to_r});
  EXPECT_TRUE(poly_lr->equals(
      ReadWkt("POLYGON ((2 2.5, 8 2.5, 8 5, 2 5, 2 2.5))").get()));

  auto poly_rl = TraversalAreas::getLeftHandRings(*factory_, box, {&r_to_l});
  EXPECT_TRUE(poly_rl->equals(
      ReadWkt("POLYGON ((2 1, 8 1, 8 2.5, 2 2.5, 2 1))").get()));
}

TEST_F(TraversalAreasTest, AdjacentSideCornerCutsAndComplements) {
  const Envelope box(0.0, 10.0, 0.0, 10.0);

  // Bottom-left triangle cut (area = 4.5) and its complement (area = 95.5)
  const std::vector<CoordinateXY> bl_cut = {CoordinateXY(3.0, 0.0),
                                            CoordinateXY(0.0, 3.0)};
  const std::vector<CoordinateXY> bl_comp = {CoordinateXY(0.0, 3.0),
                                             CoordinateXY(3.0, 0.0)};
  EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, {&bl_cut}), 4.5);
  EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, {&bl_comp}), 95.5);

  auto poly_cut = TraversalAreas::getLeftHandRings(*factory_, box, {&bl_cut});
  EXPECT_TRUE(poly_cut->isValid());
  EXPECT_TRUE(
      poly_cut->equals(ReadWkt("POLYGON ((0 0, 3 0, 0 3, 0 0))").get()));

  auto poly_comp = TraversalAreas::getLeftHandRings(*factory_, box, {&bl_comp});
  EXPECT_TRUE(poly_comp->isValid());
  EXPECT_TRUE(poly_comp->equals(
      ReadWkt("POLYGON ((0 3, 3 0, 10 0, 10 10, 0 10, 0 3))").get()));

  // Remaining three corner cuts
  const std::vector<CoordinateXY> tl_cut = {CoordinateXY(0.0, 7.0),
                                            CoordinateXY(3.0, 10.0)};
  const std::vector<CoordinateXY> tr_cut = {CoordinateXY(7.0, 10.0),
                                            CoordinateXY(10.0, 7.0)};
  const std::vector<CoordinateXY> br_cut = {CoordinateXY(10.0, 3.0),
                                            CoordinateXY(7.0, 0.0)};
  EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, {&tl_cut}), 4.5);
  EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, {&tr_cut}), 4.5);
  EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, {&br_cut}), 4.5);
}

TEST_F(TraversalAreasTest, SameSideUTurnTraversalsBothDirections) {
  const Envelope box(0.0, 10.0, 0.0, 10.0);

  // Left -> Left bulge (no corners visited): triangle area = 0.5 * 6 * 4 = 12
  const std::vector<CoordinateXY> left_bulge = {
      CoordinateXY(0.0, 2.0), CoordinateXY(4.0, 5.0), CoordinateXY(0.0, 8.0)};
  // Reversed notch (visits all 4 corners): area = 100 - 12 = 88
  const std::vector<CoordinateXY> left_notch = {
      CoordinateXY(0.0, 8.0), CoordinateXY(4.0, 5.0), CoordinateXY(0.0, 2.0)};

  EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, {&left_bulge}), 12.0);
  EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, {&left_notch}), 88.0);

  auto bulge_poly =
      TraversalAreas::getLeftHandRings(*factory_, box, {&left_bulge});
  EXPECT_EQ(bulge_poly->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(
      bulge_poly->equals(ReadWkt("POLYGON ((0 2, 4 5, 0 8, 0 2))").get()));

  auto notch_poly =
      TraversalAreas::getLeftHandRings(*factory_, box, {&left_notch});
  EXPECT_EQ(notch_poly->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(notch_poly->equals(
      ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 8, 4 5, 0 2, 0 0))").get()));

  // Top -> Top, Right -> Right, Bottom -> Bottom bulges (area = 6 each) and
  // their reversed notches (area = 94 each).
  const std::vector<CoordinateXY> top_bulge = {
      CoordinateXY(3.0, 10.0), CoordinateXY(5.0, 7.0), CoordinateXY(7.0, 10.0)};
  const std::vector<CoordinateXY> top_notch = {
      CoordinateXY(7.0, 10.0), CoordinateXY(5.0, 7.0), CoordinateXY(3.0, 10.0)};
  const std::vector<CoordinateXY> right_bulge = {
      CoordinateXY(10.0, 7.0), CoordinateXY(7.0, 5.0), CoordinateXY(10.0, 3.0)};
  const std::vector<CoordinateXY> right_notch = {
      CoordinateXY(10.0, 3.0), CoordinateXY(7.0, 5.0), CoordinateXY(10.0, 7.0)};
  const std::vector<CoordinateXY> bottom_bulge = {
      CoordinateXY(7.0, 0.0), CoordinateXY(5.0, 3.0), CoordinateXY(3.0, 0.0)};
  const std::vector<CoordinateXY> bottom_notch = {
      CoordinateXY(3.0, 0.0), CoordinateXY(5.0, 3.0), CoordinateXY(7.0, 0.0)};

  EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, {&top_bulge}), 6.0);
  EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, {&top_notch}), 94.0);
  EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, {&right_bulge}), 6.0);
  EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, {&right_notch}), 94.0);
  EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, {&bottom_bulge}), 6.0);
  EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, {&bottom_notch}), 94.0);

  EXPECT_TRUE(TraversalAreas::getLeftHandRings(*factory_, box, {&top_bulge})
                  ->equals(ReadWkt("POLYGON ((3 10, 5 7, 7 10, 3 10))").get()));
  EXPECT_TRUE(TraversalAreas::getLeftHandRings(*factory_, box, {&right_bulge})
                  ->equals(ReadWkt("POLYGON ((10 7, 7 5, 10 3, 10 7))").get()));
  EXPECT_TRUE(TraversalAreas::getLeftHandRings(*factory_, box, {&bottom_bulge})
                  ->equals(ReadWkt("POLYGON ((7 0, 5 3, 3 0, 7 0))").get()));
  EXPECT_TRUE(
      TraversalAreas::getLeftHandRings(*factory_, box, {&bottom_notch})
          ->equals(
              ReadWkt("POLYGON ((0 0, 3 0, 5 3, 7 0, 10 0, 10 10, 0 10, 0 0))")
                  .get()));
}

TEST_F(TraversalAreasTest, DiagonalCornerToCornerTraversals) {
  const Envelope box(0.0, 10.0, 0.0, 10.0);

  const std::vector<CoordinateXY> bl_to_tr = {CoordinateXY(0.0, 0.0),
                                              CoordinateXY(10.0, 10.0)};
  const std::vector<CoordinateXY> tr_to_bl = {CoordinateXY(10.0, 10.0),
                                              CoordinateXY(0.0, 0.0)};
  const std::vector<CoordinateXY> tl_to_br = {CoordinateXY(0.0, 10.0),
                                              CoordinateXY(10.0, 0.0)};
  const std::vector<CoordinateXY> br_to_tl = {CoordinateXY(10.0, 0.0),
                                              CoordinateXY(0.0, 10.0)};

  EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, {&bl_to_tr}), 50.0);
  EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, {&tr_to_bl}), 50.0);
  EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, {&tl_to_br}), 50.0);
  EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, {&br_to_tl}), 50.0);

  auto g1 = TraversalAreas::getLeftHandRings(*factory_, box, {&bl_to_tr});
  EXPECT_TRUE(g1->equals(ReadWkt("POLYGON ((0 0, 10 10, 0 10, 0 0))").get()));

  auto g2 = TraversalAreas::getLeftHandRings(*factory_, box, {&tr_to_bl});
  EXPECT_TRUE(g2->equals(ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 0))").get()));
}

TEST_F(TraversalAreasTest, MultipleTraversalsChainedIntoSingleRing) {
  const Envelope box(0.0, 10.0, 0.0, 10.0);

  // Horizontal band [0, 10] x [3, 7] with no corner chains included
  {
    const std::vector<CoordinateXY> lower = {CoordinateXY(0.0, 3.0),
                                             CoordinateXY(10.0, 3.0)};
    const std::vector<CoordinateXY> upper = {CoordinateXY(10.0, 7.0),
                                             CoordinateXY(0.0, 7.0)};
    const std::vector<const std::vector<CoordinateXY>*> lists = {&lower,
                                                                 &upper};

    EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, lists), 40.0);
    auto geom = TraversalAreas::getLeftHandRings(*factory_, box, lists);
    EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(geom->isValid());
    EXPECT_TRUE(
        geom->equals(ReadWkt("POLYGON ((0 3, 10 3, 10 7, 0 7, 0 3))").get()));
  }

  // Two opposing notches chained through all four corners (area = 100 - 12 =
  // 88)
  {
    const std::vector<CoordinateXY> left_notch = {
        CoordinateXY(0.0, 7.0), CoordinateXY(3.0, 5.0), CoordinateXY(0.0, 3.0)};
    const std::vector<CoordinateXY> right_notch = {CoordinateXY(10.0, 3.0),
                                                   CoordinateXY(7.0, 5.0),
                                                   CoordinateXY(10.0, 7.0)};
    const std::vector<const std::vector<CoordinateXY>*> lists = {&left_notch,
                                                                 &right_notch};

    EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, lists), 88.0);
    auto geom = TraversalAreas::getLeftHandRings(*factory_, box, lists);
    EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(geom->isValid());
    EXPECT_TRUE(geom->equals(ReadWkt("POLYGON ((0 0, 10 0, 10 3, 7 5, 10 7, 10 "
                                     "10, 0 10, 0 7, 3 5, 0 3, 0 0))")
                                 .get()));
  }
}

TEST_F(TraversalAreasTest, MultipleTraversalsFormingMultiPolygon) {
  const Envelope box(0.0, 10.0, 0.0, 10.0);

  const std::vector<CoordinateXY> bl_cut = {CoordinateXY(3.0, 0.0),
                                            CoordinateXY(0.0, 3.0)};
  const std::vector<CoordinateXY> tr_cut = {CoordinateXY(7.0, 10.0),
                                            CoordinateXY(10.0, 7.0)};
  const std::vector<CoordinateXY> ignored_pt = {CoordinateXY(5.0, 5.0)};
  const std::vector<const std::vector<CoordinateXY>*> lists = {
      &bl_cut, &ignored_pt, &tr_cut};

  EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, lists), 9.0);

  auto geom = TraversalAreas::getLeftHandRings(*factory_, box, lists);
  EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(geom->getNumGeometries(), 2U);
  EXPECT_TRUE(geom->isValid());
  EXPECT_DOUBLE_EQ(geom->getArea(), 9.0);
  EXPECT_TRUE(geom->equals(
      ReadWkt(
          "MULTIPOLYGON (((0 0, 3 0, 0 3, 0 0)), ((7 10, 10 7, 10 10, 7 10)))")
          .get()));
}

TEST_F(TraversalAreasTest, ClosedCounterClockwiseIslands) {
  const Envelope box(0.0, 10.0, 0.0, 10.0);

  // Single CCW closed square (area = 12)
  const std::vector<CoordinateXY> ccw_ring1 = {
      CoordinateXY(2.0, 2.0), CoordinateXY(6.0, 2.0), CoordinateXY(6.0, 5.0),
      CoordinateXY(2.0, 5.0), CoordinateXY(2.0, 2.0)};

  EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, {&ccw_ring1}), 12.0);
  auto poly = TraversalAreas::getLeftHandRings(*factory_, box, {&ccw_ring1});
  EXPECT_EQ(poly->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(poly->isValid());
  EXPECT_TRUE(
      poly->equals(ReadWkt("POLYGON ((2 2, 6 2, 6 5, 2 5, 2 2))").get()));

  // Two CCW closed islands -> MultiPolygon (area = 12 + 4 = 16)
  const std::vector<CoordinateXY> ccw_ring2 = {
      CoordinateXY(7.0, 7.0), CoordinateXY(9.0, 7.0), CoordinateXY(9.0, 9.0),
      CoordinateXY(7.0, 9.0), CoordinateXY(7.0, 7.0)};
  EXPECT_DOUBLE_EQ(
      TraversalAreas::getLeftHandArea(box, {&ccw_ring1, &ccw_ring2}), 16.0);
  auto mpoly = TraversalAreas::getLeftHandRings(*factory_, box,
                                                {&ccw_ring1, &ccw_ring2});
  EXPECT_EQ(mpoly->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(mpoly->getNumGeometries(), 2U);
  EXPECT_TRUE(mpoly->isValid());
  EXPECT_DOUBLE_EQ(mpoly->getArea(), 16.0);
}

TEST_F(TraversalAreasTest, ClosedClockwiseHolesWithoutShells) {
  const Envelope box(-2.0, 8.0, 0.0, 10.0);  // area = 100

  // Single CW hole of area 16
  const std::vector<CoordinateXY> hole1 = {
      CoordinateXY(1.0, 2.0), CoordinateXY(1.0, 6.0), CoordinateXY(5.0, 6.0),
      CoordinateXY(5.0, 2.0), CoordinateXY(1.0, 2.0)};
  EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, {&hole1}), 84.0);

  auto poly1 = TraversalAreas::getLeftHandRings(*factory_, box, {&hole1});
  EXPECT_EQ(poly1->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(poly1->isValid());
  EXPECT_DOUBLE_EQ(poly1->getArea(), 84.0);
  const auto* p1 = dynamic_cast<const Polygon*>(poly1.get());
  ASSERT_NE(p1, nullptr);
  EXPECT_EQ(p1->getNumInteriorRing(), 1U);
  EXPECT_TRUE(poly1->equals(
      ReadWkt(
          "POLYGON ((-2 0, 8 0, 8 10, -2 10, -2 0), (1 2, 1 6, 5 6, 5 2, 1 2))")
          .get()));

  // Two disjoint CW holes strictly inside the box (area 16 + 4 = 20 -> 80)
  const std::vector<CoordinateXY> hole2 = {
      CoordinateXY(5.5, 7.0), CoordinateXY(5.5, 9.0), CoordinateXY(7.5, 9.0),
      CoordinateXY(7.5, 7.0), CoordinateXY(5.5, 7.0)};
  EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, {&hole1, &hole2}),
                   80.0);

  auto poly2 =
      TraversalAreas::getLeftHandRings(*factory_, box, {&hole1, &hole2});
  EXPECT_EQ(poly2->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(poly2->isValid());
  EXPECT_DOUBLE_EQ(poly2->getArea(), 80.0);
  const auto* p2 = dynamic_cast<const Polygon*>(poly2.get());
  ASSERT_NE(p2, nullptr);
  EXPECT_EQ(p2->getNumInteriorRing(), 2U);
}

TEST_F(TraversalAreasTest, SingleShellWithClockwiseHole) {
  const Envelope box(0.0, 10.0, 0.0, 10.0);

  // Left shell [0, 6] x [0, 10] (area = 60) with CW hole (area = 24)
  const std::vector<CoordinateXY> traversal = {CoordinateXY(6.0, 0.0),
                                               CoordinateXY(6.0, 10.0)};
  const std::vector<CoordinateXY> hole = {
      CoordinateXY(1.0, 2.0), CoordinateXY(1.0, 8.0), CoordinateXY(5.0, 8.0),
      CoordinateXY(5.0, 2.0), CoordinateXY(1.0, 2.0)};
  const std::vector<const std::vector<CoordinateXY>*> lists = {&traversal,
                                                               &hole};

  EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, lists), 36.0);

  auto geom = TraversalAreas::getLeftHandRings(*factory_, box, lists);
  EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(geom->isValid());
  EXPECT_DOUBLE_EQ(geom->getArea(), 36.0);
  const auto* poly = dynamic_cast<const Polygon*>(geom.get());
  ASSERT_NE(poly, nullptr);
  EXPECT_EQ(poly->getNumInteriorRing(), 1U);
  EXPECT_TRUE(geom->equals(
      ReadWkt(
          "POLYGON ((0 0, 6 0, 6 10, 0 10, 0 0), (1 2, 1 8, 5 8, 5 2, 1 2))")
          .get()));
}

TEST_F(TraversalAreasTest, MultipleShellsWithHolesUsesPolygonizer) {
  const Envelope box(0.0, 10.0, 0.0, 10.0);

  // Two shells (one traversal with hole, one corner cut) -> Polygonizer path
  {
    const std::vector<CoordinateXY> shell1_trav = {CoordinateXY(6.0, 0.0),
                                                   CoordinateXY(6.0, 10.0)};
    const std::vector<CoordinateXY> hole = {
        CoordinateXY(1.0, 2.0), CoordinateXY(1.0, 8.0), CoordinateXY(5.0, 8.0),
        CoordinateXY(5.0, 2.0), CoordinateXY(1.0, 2.0)};
    const std::vector<CoordinateXY> shell2_trav = {CoordinateXY(7.0, 10.0),
                                                   CoordinateXY(10.0, 7.0)};
    const std::vector<const std::vector<CoordinateXY>*> lists = {
        &shell1_trav, &hole, &shell2_trav};

    EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, lists), 40.5);

    auto geom = TraversalAreas::getLeftHandRings(*factory_, box, lists);
    EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
    EXPECT_EQ(geom->getNumGeometries(), 2U);
    EXPECT_TRUE(geom->isValid());
    EXPECT_DOUBLE_EQ(geom->getArea(), 40.5);
    EXPECT_TRUE(
        geom->equals(ReadWkt("MULTIPOLYGON (((0 0, 6 0, 6 10, 0 10, 0 0), "
                             "(1 2, 1 8, 5 8, 5 2, 1 2)), "
                             "((7 10, 10 7, 10 10, 7 10)))")
                         .get()));
  }

  // Nested CCW outer ring + CW hole + CCW inner island -> Polygonizer path
  {
    const std::vector<CoordinateXY> outer = {
        CoordinateXY(1.0, 1.0), CoordinateXY(9.0, 1.0), CoordinateXY(9.0, 9.0),
        CoordinateXY(1.0, 9.0), CoordinateXY(1.0, 1.0)};
    const std::vector<CoordinateXY> hole = {
        CoordinateXY(2.0, 2.0), CoordinateXY(2.0, 8.0), CoordinateXY(8.0, 8.0),
        CoordinateXY(8.0, 2.0), CoordinateXY(2.0, 2.0)};
    const std::vector<CoordinateXY> island = {
        CoordinateXY(4.0, 4.0), CoordinateXY(6.0, 4.0), CoordinateXY(6.0, 6.0),
        CoordinateXY(4.0, 6.0), CoordinateXY(4.0, 4.0)};
    const std::vector<const std::vector<CoordinateXY>*> lists = {&outer, &hole,
                                                                 &island};

    EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, lists), 32.0);
    auto geom = TraversalAreas::getLeftHandRings(*factory_, box, lists);
    EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
    EXPECT_EQ(geom->getNumGeometries(), 2U);
    EXPECT_TRUE(geom->isValid());
    EXPECT_DOUBLE_EQ(geom->getArea(), 32.0);
  }
}

TEST_F(TraversalAreasTest, SmallIslandInsideLargerHoleInFullBoxArea) {
  const Envelope box(0.0, 10.0, 0.0, 10.0);  // area = 100
  const std::vector<CoordinateXY> large_hole = {
      CoordinateXY(1.0, 1.0), CoordinateXY(1.0, 9.0), CoordinateXY(9.0, 9.0),
      CoordinateXY(9.0, 1.0), CoordinateXY(1.0, 1.0)};  // CW, area = 64
  const std::vector<CoordinateXY> small_island = {
      CoordinateXY(4.0, 4.0), CoordinateXY(6.0, 4.0), CoordinateXY(6.0, 6.0),
      CoordinateXY(4.0, 6.0), CoordinateXY(4.0, 4.0)};  // CCW, area = 4

  // ccw_sum (4) < cw_sum (64), so box area (100) is added: 100 + 4 - 64 = 40.
  EXPECT_DOUBLE_EQ(
      TraversalAreas::getLeftHandArea(box, {&large_hole, &small_island}), 40.0);
}

TEST_F(TraversalAreasTest, ZeroAreaRingsAndBoundaryTraversals) {
  const Envelope box(0.0, 10.0, 0.0, 10.0);

  // Collinear upward traversal along the left edge encloses zero area.
  {
    const std::vector<CoordinateXY> left_up = {CoordinateXY(0.0, 2.0),
                                               CoordinateXY(0.0, 8.0)};
    EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, {&left_up}), 0.0);

    auto geom = TraversalAreas::getLeftHandRings(*factory_, box, {&left_up});
    EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(geom->isEmpty());
    EXPECT_DOUBLE_EQ(geom->getArea(), 0.0);
  }

  // Collinear downward traversal along the left edge encloses the full box.
  {
    const std::vector<CoordinateXY> left_down = {CoordinateXY(0.0, 8.0),
                                                 CoordinateXY(0.0, 2.0)};
    EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, {&left_down}), 100.0);

    auto geom = TraversalAreas::getLeftHandRings(*factory_, box, {&left_down});
    EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(
        geom->equals(ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))").get()));
  }

  // Collapsed 3-point closed ring returns 0 area and empty polygon.
  {
    const std::vector<CoordinateXY> spike = {
        CoordinateXY(2.0, 2.0), CoordinateXY(5.0, 5.0), CoordinateXY(2.0, 2.0)};
    EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, {&spike}), 0.0);

    auto geom = TraversalAreas::getLeftHandRings(*factory_, box, {&spike});
    EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(geom->isEmpty());
  }

  // Collapsed ring alongside a valid CCW shell is skipped cleanly.
  {
    const std::vector<CoordinateXY> ccw_square = {
        CoordinateXY(1.0, 1.0), CoordinateXY(3.0, 1.0), CoordinateXY(3.0, 3.0),
        CoordinateXY(1.0, 3.0), CoordinateXY(1.0, 1.0)};
    const std::vector<CoordinateXY> spike = {
        CoordinateXY(7.0, 7.0), CoordinateXY(8.0, 8.0), CoordinateXY(7.0, 7.0)};
    EXPECT_DOUBLE_EQ(
        TraversalAreas::getLeftHandArea(box, {&ccw_square, &spike}), 4.0);

    auto geom =
        TraversalAreas::getLeftHandRings(*factory_, box, {&ccw_square, &spike});
    EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(
        geom->equals(ReadWkt("POLYGON ((1 1, 3 1, 3 3, 1 3, 1 1))").get()));
  }
}

TEST_F(TraversalAreasTest, OverflowPerimeterEnvelopeThrowsOnNextChainFailure) {
  // Box perimeter overflows double to +inf, causing CCW chain selection to
  // fail.
  const Envelope huge(0.0, 1e308, 0.0, 1e308);
  const std::vector<CoordinateXY> trav = {CoordinateXY(0.0, 0.5e308),
                                          CoordinateXY(1e308, 0.5e308)};
  const std::vector<const std::vector<CoordinateXY>*> lists = {&trav};

  EXPECT_THROW(TraversalAreas::getLeftHandArea(huge, lists),
               std::runtime_error);
  EXPECT_THROW(TraversalAreas::getLeftHandRings(*factory_, huge, lists),
               std::runtime_error);
}

TEST_F(TraversalAreasTest, RepeatedInitialVerticesAndZeroAreaWithHole) {
  const Envelope box(0.0, 10.0, 0.0, 10.0);

  // Repeated starting points test loop continuation in
  // hasMultipleUniqueCoordinates for both open traversals and closed rings.
  {
    const std::vector<CoordinateXY> repeated_start_open = {
        CoordinateXY(4.0, 0.0), CoordinateXY(4.0, 0.0),
        CoordinateXY(4.0, 10.0)};
    const std::vector<CoordinateXY> repeated_start_ccw = {
        CoordinateXY(6.0, 2.0), CoordinateXY(6.0, 2.0), CoordinateXY(8.0, 2.0),
        CoordinateXY(8.0, 5.0), CoordinateXY(6.0, 5.0), CoordinateXY(6.0, 2.0)};
    const std::vector<const std::vector<CoordinateXY>*> lists = {
        &repeated_start_open, &repeated_start_ccw};

    EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, lists), 46.0);
    auto geom = TraversalAreas::getLeftHandRings(*factory_, box, lists);
    EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
    EXPECT_EQ(geom->getNumGeometries(), 2U);
    EXPECT_DOUBLE_EQ(geom->getArea(), 46.0);
    EXPECT_TRUE(
        geom->equals(ReadWkt("MULTIPOLYGON (((0 0, 4 0, 4 10, 0 10, 0 0)), "
                             "((6 2, 8 2, 8 5, 6 5, 6 2)))")
                         .get()));
  }

  // Zero-area open boundary traversal alongside a CW hole leaves shells empty
  // before falling back to the full box shell.
  {
    const std::vector<CoordinateXY> top_lr = {CoordinateXY(3.0, 10.0),
                                              CoordinateXY(7.0, 10.0)};
    const std::vector<CoordinateXY> collinear_closed = {
        CoordinateXY(2.0, 2.0), CoordinateXY(5.0, 5.0), CoordinateXY(8.0, 8.0),
        CoordinateXY(2.0, 2.0)};
    const std::vector<CoordinateXY> hole = {
        CoordinateXY(2.0, 2.0), CoordinateXY(2.0, 6.0), CoordinateXY(6.0, 6.0),
        CoordinateXY(6.0, 2.0), CoordinateXY(2.0, 2.0)};
    const std::vector<const std::vector<CoordinateXY>*> lists = {
        &top_lr, &collinear_closed, &hole};

    EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, lists), 84.0);
    auto geom = TraversalAreas::getLeftHandRings(*factory_, box, lists);
    EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(geom->isValid());
    EXPECT_DOUBLE_EQ(geom->getArea(), 84.0);
    const auto* poly = dynamic_cast<const Polygon*>(geom.get());
    ASSERT_NE(poly, nullptr);
    EXPECT_EQ(poly->getNumInteriorRing(), 1U);
  }
}

TEST_F(TraversalAreasTest,
       MultiChainRingWithCornersAndMultipleShellsWithMultipleHoles) {
  const Envelope box(0.0, 10.0, 0.0, 10.0);

  // Three open traversals chained into one 7-gon passing through (0, 10).
  {
    const std::vector<CoordinateXY> t1 = {CoordinateXY(0.0, 3.0),
                                          CoordinateXY(3.0, 0.0)};
    const std::vector<CoordinateXY> t2 = {CoordinateXY(7.0, 0.0),
                                          CoordinateXY(10.0, 3.0)};
    const std::vector<CoordinateXY> t3 = {CoordinateXY(10.0, 7.0),
                                          CoordinateXY(7.0, 10.0)};
    const std::vector<const std::vector<CoordinateXY>*> lists = {&t1, &t2, &t3};

    EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, lists), 86.5);
    auto geom = TraversalAreas::getLeftHandRings(*factory_, box, lists);
    EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(geom->isValid());
    EXPECT_DOUBLE_EQ(geom->getArea(), 86.5);
    EXPECT_TRUE(geom->equals(
        ReadWkt("POLYGON ((0 3, 3 0, 7 0, 10 3, 10 7, 7 10, 0 10, 0 3))")
            .get()));
  }

  // Two disjoint traversal shells, each containing a CW hole -> Polygonizer.
  {
    const std::vector<CoordinateXY> left_shell = {CoordinateXY(4.0, 0.0),
                                                  CoordinateXY(4.0, 10.0)};
    const std::vector<CoordinateXY> left_hole = {
        CoordinateXY(1.0, 2.0), CoordinateXY(1.0, 8.0), CoordinateXY(3.0, 8.0),
        CoordinateXY(3.0, 2.0), CoordinateXY(1.0, 2.0)};
    const std::vector<CoordinateXY> right_shell = {CoordinateXY(6.0, 10.0),
                                                   CoordinateXY(6.0, 0.0)};
    const std::vector<CoordinateXY> right_hole = {
        CoordinateXY(7.0, 2.0), CoordinateXY(7.0, 8.0), CoordinateXY(9.0, 8.0),
        CoordinateXY(9.0, 2.0), CoordinateXY(7.0, 2.0)};
    const std::vector<const std::vector<CoordinateXY>*> lists = {
        &left_shell, &left_hole, &right_shell, &right_hole};

    EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, lists), 56.0);
    auto geom = TraversalAreas::getLeftHandRings(*factory_, box, lists);
    EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
    EXPECT_EQ(geom->getNumGeometries(), 2U);
    EXPECT_TRUE(geom->isValid());
    EXPECT_DOUBLE_EQ(geom->getArea(), 56.0);
    for (std::size_t i = 0; i < geom->getNumGeometries(); ++i) {
      const auto* poly = dynamic_cast<const Polygon*>(geom->getGeometryN(i));
      ASSERT_NE(poly, nullptr);
      EXPECT_EQ(poly->getNumInteriorRing(), 1U);
    }
  }
}

TEST_F(TraversalAreasTest,
       TraversalMeetingCornerAndNegativeFractionalEnvelope) {
  // 6x6 envelope shifted to [-4.5, 1.5] x [-2.5, 3.5] (area = 36).
  const Envelope box(-4.5, 1.5, -2.5, 3.5);
  const std::vector<CoordinateXY> t1 = {CoordinateXY(-4.5, -2.5),
                                        CoordinateXY(1.5, 0.5)};
  const std::vector<CoordinateXY> t2 = {CoordinateXY(1.5, 0.5),
                                        CoordinateXY(-1.5, 3.5)};
  const std::vector<const std::vector<CoordinateXY>*> lists = {&t1, &t2};

  EXPECT_DOUBLE_EQ(TraversalAreas::getLeftHandArea(box, lists), 22.5);
  auto geom = TraversalAreas::getLeftHandRings(*factory_, box, lists);
  EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(geom->getArea(), 22.5);
  EXPECT_TRUE(geom->equals(
      ReadWkt("POLYGON ((-4.5 -2.5, 1.5 0.5, -1.5 3.5, -4.5 3.5, -4.5 -2.5))")
          .get()));
}

}  // namespace
}  // namespace geos_google
