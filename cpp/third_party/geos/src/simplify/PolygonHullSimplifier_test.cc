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

#include "third_party/geos/include/geos/simplify/PolygonHullSimplifier.h"

#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::MultiPolygon;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::simplify::PolygonHullSimplifier;
using geos::util::IllegalArgumentException;

// Square [0, 10] x [0, 10] with a bottom notch (area 6) and right notch (area
// 12). Total area = 82, 11 points (10 distinct vertices).
constexpr char kNotchedSquareWkt[] =
    "POLYGON ((0 0, 2 0, 5 2, 8 0, 10 0, 10 2, 6 5, 10 8, 10 10, 0 10, 0 0))";

// Square [0, 12] x [0, 12] with four disjoint triangular notches of areas
// 3 (bottom), 6 (right), 9 (top), and 12 (left). Total area = 114, 17 points.
constexpr char kFourNotchSquareWkt[] =
    "POLYGON ((0 0, 3 0, 6 1, 9 0, 12 0, 12 3, 10 6, 12 9, 12 12, "
    "9 12, 6 9, 3 12, 0 12, 0 9, 4 6, 0 3, 0 0))";

// Square [0, 20] x [0, 20] with a small top spike (area 4) and a larger
// bottom spike (area 16). Total area = 420, 11 points.
constexpr char kTwoSpikeSquareWkt[] =
    "POLYGON ((0 0, 6 0, 10 -4, 14 0, 20 0, 20 20, 11 20, 10 24, 9 20, 0 20, 0 "
    "0))";

class PolygonHullSimplifierTest : public ::testing::Test {
 protected:
  PolygonHullSimplifierTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  std::unique_ptr<Polygon> ReadPolygon(const std::string& wkt) const {
    return reader_.read<Polygon>(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(PolygonHullSimplifierTest, RejectsNonPolygonalGeometries) {
  const std::vector<std::string> non_polygonal_wkts = {
      "POINT (1 2)",
      "POINT EMPTY",
      "LINESTRING (0 0, 10 10)",
      "LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)",
      "MULTIPOINT ((0 0), (1 1))",
      "MULTILINESTRING ((0 0, 5 5), (5 5, 10 0))",
      "GEOMETRYCOLLECTION (POINT (0 0), POLYGON ((0 0, 10 0, 10 10, 0 10, 0 "
      "0)))",
  };

  for (const std::string& wkt : non_polygonal_wkts) {
    SCOPED_TRACE(wkt);
    std::unique_ptr<Geometry> geom = ReadWkt(wkt);
    ASSERT_FALSE(geom->isPolygonal());

    for (bool is_outer : {true, false}) {
      EXPECT_THROW(
          { PolygonHullSimplifier simplifier(geom.get(), is_outer); },
          IllegalArgumentException);
      EXPECT_THROW(PolygonHullSimplifier::hull(geom.get(), is_outer, 0.5),
                   IllegalArgumentException);
      EXPECT_THROW(
          PolygonHullSimplifier::hullByAreaDelta(geom.get(), is_outer, 0.5),
          IllegalArgumentException);
    }
  }

  std::unique_ptr<Geometry> point = ReadWkt("POINT (0 0)");
  try {
    PolygonHullSimplifier::hull(point.get(), true, 0.5);
    FAIL() << "Expected IllegalArgumentException";
  } catch (const IllegalArgumentException& e) {
    EXPECT_NE(std::string(e.what()).find("polygonal"), std::string::npos);
  }
}

TEST_F(PolygonHullSimplifierTest,
       RejectsPolygonalCollectionsAndCurvedSurfacesOnSimplify) {
  const std::vector<std::string> unsupported_polygonal_wkts = {
      "GEOMETRYCOLLECTION (POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)))",
      "GEOMETRYCOLLECTION EMPTY",
      "CURVEPOLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
      "MULTISURFACE (((0 0, 10 0, 10 10, 0 10, 0 0)))",
  };

  for (const std::string& wkt : unsupported_polygonal_wkts) {
    SCOPED_TRACE(wkt);
    std::unique_ptr<Geometry> geom = ReadWkt(wkt);
    ASSERT_TRUE(geom->isPolygonal());

    // Trivial parameters return a clone before dispatching by concrete type.
    std::unique_ptr<Geometry> exact =
        PolygonHullSimplifier::hull(geom.get(), true, 1.0);
    EXPECT_TRUE(exact->equalsExact(geom.get()));

    std::unique_ptr<Geometry> zero_delta =
        PolygonHullSimplifier::hullByAreaDelta(geom.get(), false, 0.0);
    EXPECT_TRUE(zero_delta->equalsExact(geom.get()));

    for (bool is_outer : {true, false}) {
      PolygonHullSimplifier simplifier(geom.get(), is_outer);
      simplifier.setVertexNumFraction(0.5);
      EXPECT_THROW(simplifier.getResult(), IllegalArgumentException);

      EXPECT_THROW(PolygonHullSimplifier::hull(geom.get(), is_outer, 0.5),
                   IllegalArgumentException);
      EXPECT_THROW(
          PolygonHullSimplifier::hullByAreaDelta(geom.get(), is_outer, 0.5),
          IllegalArgumentException);
    }
  }
}

TEST_F(PolygonHullSimplifierTest, TrivialAndOutOfRangeParameters) {
  std::unique_ptr<Geometry> poly = ReadWkt(kFourNotchSquareWkt);
  ASSERT_TRUE(poly->isValid());

  // vertexNumFraction == 1 (or > 1, clamped to 1) returns an exact clone.
  for (double frac :
       {1.0, 1.5, -1.0, -2.5, std::numeric_limits<double>::infinity()}) {
    SCOPED_TRACE(frac);
    std::unique_ptr<Geometry> res =
        PolygonHullSimplifier::hull(poly.get(), true, frac);
    EXPECT_TRUE(res->equalsExact(poly.get()));
  }

  // areaDeltaRatio == 0 returns an exact clone.
  std::unique_ptr<Geometry> zero_delta =
      PolygonHullSimplifier::hullByAreaDelta(poly.get(), true, 0.0);
  EXPECT_TRUE(zero_delta->equalsExact(poly.get()));

  // Static helpers take std::abs() of negative parameters.
  std::unique_ptr<Geometry> pos_frac =
      PolygonHullSimplifier::hull(poly.get(), true, 0.5);
  std::unique_ptr<Geometry> neg_frac =
      PolygonHullSimplifier::hull(poly.get(), true, -0.5);
  EXPECT_TRUE(neg_frac->equalsExact(pos_frac.get()));

  std::unique_ptr<Geometry> pos_delta =
      PolygonHullSimplifier::hullByAreaDelta(poly.get(), true, 0.08);
  std::unique_ptr<Geometry> neg_delta =
      PolygonHullSimplifier::hullByAreaDelta(poly.get(), true, -0.08);
  EXPECT_TRUE(neg_delta->equalsExact(pos_delta.get()));

  // Instance API: no target set leaves geometry unsimplified.
  {
    PolygonHullSimplifier unset(poly.get(), true);
    std::unique_ptr<Geometry> res = unset.getResult();
    EXPECT_TRUE(res->isValid());
    EXPECT_TRUE(res->equals(poly.get()));
    EXPECT_EQ(res->getNumPoints(), poly->getNumPoints());
  }

  // Instance API: negative areaDeltaRatio leaves no target active.
  {
    PolygonHullSimplifier neg_instance_delta(poly.get(), true);
    neg_instance_delta.setAreaDeltaRatio(-0.5);
    std::unique_ptr<Geometry> res = neg_instance_delta.getResult();
    EXPECT_TRUE(res->equals(poly.get()));
    EXPECT_EQ(res->getNumPoints(), poly->getNumPoints());
  }

  // Instance API: setVertexNumFraction clamps negative values directly to 0.0
  // and values > 1.0 to 1.0.
  {
    PolygonHullSimplifier clamp_low(poly.get(), true);
    clamp_low.setVertexNumFraction(-0.5);
    std::unique_ptr<Geometry> res = clamp_low.getResult();
    std::unique_ptr<Geometry> expected_convex = poly->convexHull();
    EXPECT_TRUE(res->equals(expected_convex.get()));
    EXPECT_EQ(res->getNumPoints(), 5u);
  }
  {
    PolygonHullSimplifier clamp_high(poly.get(), false);
    clamp_high.setVertexNumFraction(2.0);
    std::unique_ptr<Geometry> res = clamp_high.getResult();
    EXPECT_TRUE(res->equalsExact(poly.get()));
  }
  {
    PolygonHullSimplifier zero_delta_inst(poly.get(), false);
    zero_delta_inst.setAreaDeltaRatio(0.0);
    std::unique_ptr<Geometry> res = zero_delta_inst.getResult();
    EXPECT_TRUE(res->equalsExact(poly.get()));
  }

  // Instance API: setting both vertexNumFraction and areaDeltaRatio uses
  // vertexNumFraction when both are >= 0.
  {
    PolygonHullSimplifier both(poly.get(), true);
    both.setAreaDeltaRatio(0.01);
    both.setVertexNumFraction(0.0);
    std::unique_ptr<Geometry> res = both.getResult();
    std::unique_ptr<Geometry> expected_convex = poly->convexHull();
    EXPECT_TRUE(res->equals(expected_convex.get()));
    EXPECT_EQ(res->getNumPoints(), 5u);
  }

  // Infinite areaDeltaRatio simplifies as far as topology allows.
  {
    std::unique_ptr<Geometry> inf_delta =
        PolygonHullSimplifier::hullByAreaDelta(
            poly.get(), true, std::numeric_limits<double>::infinity());
    std::unique_ptr<Geometry> expected_convex = poly->convexHull();
    EXPECT_TRUE(inf_delta->equals(expected_convex.get()));
    EXPECT_EQ(inf_delta->getNumPoints(), 5u);
  }
}

TEST_F(PolygonHullSimplifierTest, EmptyPolygonAndEmptyMultiPolygon) {
  std::unique_ptr<Geometry> empty_poly = ReadWkt("POLYGON EMPTY");
  std::unique_ptr<Geometry> empty_mp = ReadWkt("MULTIPOLYGON EMPTY");

  for (bool is_outer : {true, false}) {
    for (double frac : {0.0, 0.5, 1.0}) {
      std::unique_ptr<Geometry> poly_res =
          PolygonHullSimplifier::hull(empty_poly.get(), is_outer, frac);
      EXPECT_TRUE(poly_res->isEmpty());
      EXPECT_EQ(poly_res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
      EXPECT_EQ(poly_res->getFactory(), factory_.get());

      std::unique_ptr<Geometry> mp_res =
          PolygonHullSimplifier::hull(empty_mp.get(), is_outer, frac);
      EXPECT_TRUE(mp_res->isEmpty());
      EXPECT_EQ(mp_res->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
      EXPECT_EQ(mp_res->getFactory(), factory_.get());
    }

    for (double ratio : {0.0, 0.2}) {
      std::unique_ptr<Geometry> poly_res =
          PolygonHullSimplifier::hullByAreaDelta(empty_poly.get(), is_outer,
                                                 ratio);
      EXPECT_TRUE(poly_res->isEmpty());
      EXPECT_EQ(poly_res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);

      std::unique_ptr<Geometry> mp_res = PolygonHullSimplifier::hullByAreaDelta(
          empty_mp.get(), is_outer, ratio);
      EXPECT_TRUE(mp_res->isEmpty());
      EXPECT_EQ(mp_res->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
    }
  }
}

TEST_F(PolygonHullSimplifierTest, ConvexPolygonOuterAndInnerHulls) {
  std::unique_ptr<Geometry> triangle =
      ReadWkt("POLYGON ((0 0, 10 0, 5 8, 0 0))");
  for (bool is_outer : {true, false}) {
    std::unique_ptr<Geometry> res_frac =
        PolygonHullSimplifier::hull(triangle.get(), is_outer, 0.0);
    EXPECT_TRUE(res_frac->isValid());
    EXPECT_TRUE(res_frac->equals(triangle.get()));
    EXPECT_EQ(res_frac->getNumPoints(), 4u);

    std::unique_ptr<Geometry> res_area =
        PolygonHullSimplifier::hullByAreaDelta(triangle.get(), is_outer, 10.0);
    EXPECT_TRUE(res_area->isValid());
    EXPECT_TRUE(res_area->equals(triangle.get()));
    EXPECT_EQ(res_area->getNumPoints(), 4u);
  }

  std::unique_ptr<Geometry> square =
      ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  std::unique_ptr<Geometry> outer_square =
      PolygonHullSimplifier::hull(square.get(), true, 0.0);
  EXPECT_TRUE(outer_square->isValid());
  EXPECT_TRUE(outer_square->equals(square.get()));
  EXPECT_EQ(outer_square->getNumPoints(), 5u);
  EXPECT_DOUBLE_EQ(outer_square->getArea(), 100.0);

  // Inner hull of a square simplifies down to a triangle (half area).
  std::unique_ptr<Geometry> inner_square =
      PolygonHullSimplifier::hull(square.get(), false, 0.0);
  EXPECT_TRUE(inner_square->isValid());
  EXPECT_TRUE(square->covers(inner_square.get()));
  EXPECT_EQ(inner_square->getNumPoints(), 4u);
  EXPECT_DOUBLE_EQ(inner_square->getArea(), 50.0);
}

TEST_F(PolygonHullSimplifierTest, OuterHullByVertexFraction) {
  std::unique_ptr<Geometry> notched = ReadWkt(kNotchedSquareWkt);
  ASSERT_TRUE(notched->isValid());
  ASSERT_DOUBLE_EQ(notched->getArea(), 82.0);
  ASSERT_EQ(notched->getNumPoints(), 11u);

  // Fraction 0.0 yields the convex hull.
  std::unique_ptr<Geometry> full_hull =
      PolygonHullSimplifier::hull(notched.get(), true, 0.0);
  std::unique_ptr<Geometry> expected_convex = notched->convexHull();
  EXPECT_TRUE(full_hull->isValid());
  EXPECT_TRUE(full_hull->equals(expected_convex.get()));
  EXPECT_EQ(full_hull->getNumPoints(), 5u);
  EXPECT_DOUBLE_EQ(full_hull->getArea(), 100.0);

  // Fraction 0.8 (target 8 of 10 vertices) bridges only the smaller notch.
  std::unique_ptr<Geometry> partial_hull =
      PolygonHullSimplifier::hull(notched.get(), true, 0.8);
  EXPECT_TRUE(partial_hull->isValid());
  EXPECT_TRUE(partial_hull->covers(notched.get()));
  EXPECT_EQ(partial_hull->getNumPoints(), 8u);
  EXPECT_DOUBLE_EQ(partial_hull->getArea(), 88.0);

  // Sweep fractions from 1.0 down to 0.0 on a 4-notch polygon.
  std::unique_ptr<Geometry> four_notch = ReadWkt(kFourNotchSquareWkt);
  ASSERT_TRUE(four_notch->isValid());
  double prev_area = four_notch->getArea();
  std::size_t prev_points = four_notch->getNumPoints();
  for (double frac : {1.0, 0.8, 0.6, 0.4, 0.2, 0.0}) {
    SCOPED_TRACE(frac);
    std::unique_ptr<Geometry> res =
        PolygonHullSimplifier::hull(four_notch.get(), true, frac);
    EXPECT_TRUE(res->isValid());
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(res->covers(four_notch.get()));
    EXPECT_GE(res->getArea(), prev_area);
    EXPECT_LE(res->getNumPoints(), prev_points);
    prev_area = res->getArea();
    prev_points = res->getNumPoints();
  }
  EXPECT_DOUBLE_EQ(prev_area, 144.0);
  EXPECT_EQ(prev_points, 5u);
}

TEST_F(PolygonHullSimplifierTest, OuterHullByAreaDelta) {
  std::unique_ptr<Geometry> poly = ReadWkt(kFourNotchSquareWkt);
  ASSERT_TRUE(poly->isValid());
  ASSERT_DOUBLE_EQ(poly->getArea(), 114.0);

  struct Case {
    double ratio;
    double expected_area;
    std::size_t expected_points;
  };
  const std::vector<Case> cases = {
      {0.02, 114.0, 17u},  // max delta 2.28 < smallest notch (3)
      {0.03, 117.0, 14u},  // max delta 3.42 bridges notch of area 3
      {0.08, 123.0, 11u},  // max delta 9.12 bridges notches 3 + 6 = 9
      {0.16, 132.0, 8u},   // max delta 18.24 bridges notches 3 + 6 + 9 = 18
      {0.30, 144.0, 5u},   // max delta 34.2 bridges all 4 notches (sum 30)
  };

  for (const Case& c : cases) {
    SCOPED_TRACE(c.ratio);
    std::unique_ptr<Geometry> res =
        PolygonHullSimplifier::hullByAreaDelta(poly.get(), true, c.ratio);
    EXPECT_TRUE(res->isValid());
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(res->covers(poly.get()));
    EXPECT_NEAR(res->getArea(), c.expected_area, 1e-9);
    EXPECT_EQ(res->getNumPoints(), c.expected_points);
    EXPECT_LE(res->getArea() - poly->getArea(),
              c.ratio * poly->getArea() + 1e-9);
  }
}

TEST_F(PolygonHullSimplifierTest, InnerHullByVertexFractionAndAreaDelta) {
  std::unique_ptr<Geometry> poly = ReadWkt(kTwoSpikeSquareWkt);
  ASSERT_TRUE(poly->isValid());
  ASSERT_DOUBLE_EQ(poly->getArea(), 420.0);
  ASSERT_EQ(poly->getNumPoints(), 11u);

  // Too small area delta trims nothing.
  std::unique_ptr<Geometry> none_trimmed =
      PolygonHullSimplifier::hullByAreaDelta(poly.get(), false, 0.005);
  EXPECT_TRUE(none_trimmed->isValid());
  EXPECT_DOUBLE_EQ(none_trimmed->getArea(), 420.0);
  EXPECT_EQ(none_trimmed->getNumPoints(), 11u);

  // Ratio 0.02 (max delta 8.4) trims only the top spike of area 4.0.
  std::unique_ptr<Geometry> one_trimmed =
      PolygonHullSimplifier::hullByAreaDelta(poly.get(), false, 0.02);
  EXPECT_TRUE(one_trimmed->isValid());
  EXPECT_TRUE(poly->covers(one_trimmed.get()));
  EXPECT_NEAR(one_trimmed->getArea(), 416.0, 1e-9);
  EXPECT_EQ(one_trimmed->getNumPoints(), 8u);

  // Ratio 0.06 (max delta 25.2) trims both spikes (4 + 16 = 20) to [0, 20]^2.
  std::unique_ptr<Geometry> both_trimmed =
      PolygonHullSimplifier::hullByAreaDelta(poly.get(), false, 0.06);
  std::unique_ptr<Geometry> expected_square =
      ReadWkt("POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0))");
  EXPECT_TRUE(both_trimmed->isValid());
  EXPECT_TRUE(poly->covers(both_trimmed.get()));
  EXPECT_TRUE(both_trimmed->equals(expected_square.get()));
  EXPECT_NEAR(both_trimmed->getArea(), 400.0, 1e-9);
  EXPECT_EQ(both_trimmed->getNumPoints(), 5u);

  // Vertex fraction 0.0 simplifies an unholed polygon to a triangle.
  std::unique_ptr<Geometry> min_inner =
      PolygonHullSimplifier::hull(poly.get(), false, 0.0);
  EXPECT_TRUE(min_inner->isValid());
  EXPECT_TRUE(poly->covers(min_inner.get()));
  EXPECT_EQ(min_inner->getNumPoints(), 4u);
  EXPECT_LT(min_inner->getArea(), 400.0);
}

TEST_F(PolygonHullSimplifierTest, OrientationIndependenceAndCollinearVertices) {
  std::unique_ptr<Geometry> ccw_poly = ReadWkt(kNotchedSquareWkt);
  std::unique_ptr<Geometry> cw_poly = ccw_poly->reverse();

  for (bool is_outer : {true, false}) {
    std::unique_ptr<Geometry> ccw_res =
        PolygonHullSimplifier::hull(ccw_poly.get(), is_outer, 0.5);
    std::unique_ptr<Geometry> cw_res =
        PolygonHullSimplifier::hull(cw_poly.get(), is_outer, 0.5);
    EXPECT_TRUE(ccw_res->isValid());
    EXPECT_TRUE(cw_res->isValid());
    EXPECT_TRUE(ccw_res->equals(cw_res.get()));
    EXPECT_DOUBLE_EQ(ccw_res->getArea(), cw_res->getArea());
  }

  // Extra collinear points along straight edges have corner area 0 and are
  // removed even for a tiny areaDeltaRatio without changing polygon area.
  std::unique_ptr<Geometry> subdivided_rect = ReadWkt(
      "POLYGON ((0 0, 3 0, 7 0, 10 0, 10 5, 10 10, 5 10, 0 10, 0 5, 0 0))");
  ASSERT_TRUE(subdivided_rect->isValid());
  std::unique_ptr<Geometry> expected_rect =
      ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");

  for (bool is_outer : {true, false}) {
    std::unique_ptr<Geometry> simplified =
        PolygonHullSimplifier::hullByAreaDelta(subdivided_rect.get(), is_outer,
                                               0.001);
    EXPECT_TRUE(simplified->isValid());
    EXPECT_TRUE(simplified->equals(expected_rect.get()));
    EXPECT_EQ(simplified->getNumPoints(), 5u);
    EXPECT_DOUBLE_EQ(simplified->getArea(), 100.0);
  }
}

TEST_F(PolygonHullSimplifierTest,
       ComplexSpiralPolygonPreventsSelfIntersection) {
  std::unique_ptr<Geometry> spiral = ReadWkt(
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 4, 16 4, 16 16, 4 16, 4 8, "
      "12 8, 12 10, 6 10, 6 14, 14 14, 14 6, 2 6, 2 18, 18 18, 18 2, 0 2, 0 "
      "0))");
  ASSERT_TRUE(spiral->isValid());

  for (double frac : {0.0, 0.3, 0.6}) {
    std::unique_ptr<Geometry> outer =
        PolygonHullSimplifier::hull(spiral.get(), true, frac);
    EXPECT_TRUE(outer->isValid());
    EXPECT_TRUE(outer->covers(spiral.get()));
    EXPECT_GE(outer->getArea(), spiral->getArea());

    std::unique_ptr<Geometry> inner =
        PolygonHullSimplifier::hull(spiral.get(), false, frac);
    EXPECT_TRUE(inner->isValid());
    EXPECT_TRUE(spiral->covers(inner.get()));
    EXPECT_LE(inner->getArea(), spiral->getArea());
  }

  for (double ratio : {0.05, 0.25, 0.8}) {
    std::unique_ptr<Geometry> outer =
        PolygonHullSimplifier::hullByAreaDelta(spiral.get(), true, ratio);
    EXPECT_TRUE(outer->isValid());
    EXPECT_TRUE(outer->covers(spiral.get()));

    std::unique_ptr<Geometry> inner =
        PolygonHullSimplifier::hullByAreaDelta(spiral.get(), false, ratio);
    EXPECT_TRUE(inner->isValid());
    EXPECT_TRUE(spiral->covers(inner.get()));
  }
}

TEST_F(PolygonHullSimplifierTest, PolygonWithSingleHole) {
  // Shell area 398 minus concave hole area 112 = 286.
  std::unique_ptr<Geometry> poly = ReadWkt(
      "POLYGON ((0 0, 20 0, 20 8, 23 10, 20 12, 20 20, 12 20, 10 16, 8 20, "
      "0 20, 0 0), (4 4, 16 4, 16 14, 12 14, 10 10, 8 14, 4 14, 4 4))");
  ASSERT_TRUE(poly->isValid());
  ASSERT_NEAR(poly->getArea(), 286.0, 1e-9);

  // Outer hull: shell expands (convex hull of shell), hole shrinks (inner
  // hull).
  {
    std::unique_ptr<Geometry> res =
        PolygonHullSimplifier::hull(poly.get(), true, 0.0);
    ASSERT_TRUE(res->isValid());
    const auto* res_poly = dynamic_cast<const Polygon*>(res.get());
    ASSERT_NE(res_poly, nullptr);
    EXPECT_TRUE(res_poly->covers(poly.get()));
    EXPECT_EQ(res_poly->getNumInteriorRing(), 1u);
    EXPECT_EQ(res_poly->getInteriorRingN(0)->getNumPoints(), 4u);
    EXPECT_GT(res_poly->getArea(), poly->getArea());
  }
  {
    std::unique_ptr<Geometry> res =
        PolygonHullSimplifier::hullByAreaDelta(poly.get(), true, 0.15);
    ASSERT_TRUE(res->isValid());
    const auto* res_poly = dynamic_cast<const Polygon*>(res.get());
    ASSERT_NE(res_poly, nullptr);
    EXPECT_TRUE(res_poly->covers(poly.get()));
    EXPECT_EQ(res_poly->getNumInteriorRing(), 1u);
    EXPECT_GE(res_poly->getArea(), poly->getArea());
  }

  // Inner hull with hole enables RingHullIndex to prevent shell-hole overlap.
  {
    std::unique_ptr<Geometry> res =
        PolygonHullSimplifier::hull(poly.get(), false, 0.0);
    ASSERT_TRUE(res->isValid());
    const auto* res_poly = dynamic_cast<const Polygon*>(res.get());
    ASSERT_NE(res_poly, nullptr);
    EXPECT_TRUE(poly->covers(res_poly));
    EXPECT_EQ(res_poly->getNumInteriorRing(), 1u);
    EXPECT_LT(res_poly->getArea(), poly->getArea());
  }
  {
    std::unique_ptr<Geometry> res =
        PolygonHullSimplifier::hullByAreaDelta(poly.get(), false, 0.15);
    ASSERT_TRUE(res->isValid());
    const auto* res_poly = dynamic_cast<const Polygon*>(res.get());
    ASSERT_NE(res_poly, nullptr);
    EXPECT_TRUE(poly->covers(res_poly));
    EXPECT_EQ(res_poly->getNumInteriorRing(), 1u);
    EXPECT_LE(res_poly->getArea(), poly->getArea());
  }
}

TEST_F(PolygonHullSimplifierTest, PolygonWithMultipleHolesPreventsRingOverlap) {
  // Shell [0, 20]^2 with two nearby C-shaped holes.
  std::unique_ptr<Geometry> poly = ReadWkt(
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
      "(2 2, 9 2, 9 6, 5 6, 5 14, 9 14, 9 18, 2 18, 2 2), "
      "(11 2, 18 2, 18 18, 11 18, 11 14, 15 14, 15 6, 11 6, 11 2))");
  ASSERT_TRUE(poly->isValid());

  for (bool is_outer : {true, false}) {
    std::unique_ptr<Geometry> frac_res =
        PolygonHullSimplifier::hull(poly.get(), is_outer, 0.0);
    ASSERT_TRUE(frac_res->isValid());
    const auto* frac_poly = dynamic_cast<const Polygon*>(frac_res.get());
    ASSERT_NE(frac_poly, nullptr);
    EXPECT_EQ(frac_poly->getNumInteriorRing(), 2u);
    if (is_outer) {
      EXPECT_TRUE(frac_poly->covers(poly.get()));
      EXPECT_GT(frac_poly->getArea(), poly->getArea());
    } else {
      EXPECT_TRUE(poly->covers(frac_poly));
      EXPECT_LT(frac_poly->getArea(), poly->getArea());
    }

    std::unique_ptr<Geometry> area_res =
        PolygonHullSimplifier::hullByAreaDelta(poly.get(), is_outer, 0.4);
    ASSERT_TRUE(area_res->isValid());
    const auto* area_poly = dynamic_cast<const Polygon*>(area_res.get());
    ASSERT_NE(area_poly, nullptr);
    EXPECT_EQ(area_poly->getNumInteriorRing(), 2u);
    if (is_outer) {
      EXPECT_TRUE(area_poly->covers(poly.get()));
      EXPECT_GE(area_poly->getArea(), poly->getArea());
    } else {
      EXPECT_TRUE(poly->covers(area_poly));
      EXPECT_LE(area_poly->getArea(), poly->getArea());
    }
  }
}

TEST_F(PolygonHullSimplifierTest, MultiPolygonSingleElement) {
  std::unique_ptr<Geometry> mp =
      ReadWkt("MULTIPOLYGON (((0 0, 2 0, 5 2, 8 0, 10 0, 10 10, 0 10, 0 0)))");
  ASSERT_TRUE(mp->isValid());
  ASSERT_DOUBLE_EQ(mp->getArea(), 94.0);

  // Single-component MultiPolygon uses computeMultiPolygonEach even for outer.
  std::unique_ptr<Geometry> outer_frac =
      PolygonHullSimplifier::hull(mp.get(), true, 0.0);
  ASSERT_TRUE(outer_frac->isValid());
  EXPECT_EQ(outer_frac->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(outer_frac->getNumGeometries(), 1u);
  EXPECT_TRUE(outer_frac->covers(mp.get()));
  EXPECT_DOUBLE_EQ(outer_frac->getArea(), 100.0);

  std::unique_ptr<Geometry> outer_delta =
      PolygonHullSimplifier::hullByAreaDelta(mp.get(), true, 0.1);
  ASSERT_TRUE(outer_delta->isValid());
  EXPECT_EQ(outer_delta->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_DOUBLE_EQ(outer_delta->getArea(), 100.0);

  std::unique_ptr<Geometry> inner_frac =
      PolygonHullSimplifier::hull(mp.get(), false, 0.0);
  ASSERT_TRUE(inner_frac->isValid());
  EXPECT_EQ(inner_frac->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_TRUE(mp->covers(inner_frac.get()));
  EXPECT_LT(inner_frac->getArea(), 94.0);
}

TEST_F(PolygonHullSimplifierTest,
       MultiPolygonOuterHullPreventsOverlapBetweenComponents) {
  // Component 1 is a C-shape; component 2 sits inside its bay and must block
  // component 1 from bridging across the bay mouth.
  std::unique_ptr<Geometry> bay_mp = ReadWkt(
      "MULTIPOLYGON ("
      "((0 0, 20 0, 20 4, 6 4, 6 16, 20 16, 20 20, 0 20, 0 0)),"
      "((10 7, 16 7, 16 13, 13 10, 10 13, 10 7)))");
  ASSERT_TRUE(bay_mp->isValid());

  const std::array<std::unique_ptr<Geometry>, 2> bay_results = {
      PolygonHullSimplifier::hull(bay_mp.get(), true, 0.0),
      PolygonHullSimplifier::hullByAreaDelta(bay_mp.get(), true, 0.5),
  };
  for (const auto& res : bay_results) {
    ASSERT_TRUE(res->isValid());
    const auto* res_mp = dynamic_cast<const MultiPolygon*>(res.get());
    ASSERT_NE(res_mp, nullptr);
    EXPECT_EQ(res_mp->getNumGeometries(), 2u);
    EXPECT_TRUE(res_mp->covers(bay_mp.get()));
    EXPECT_GE(res_mp->getArea(), bay_mp->getArea());
    // Bay component cannot swallow the entire [0, 20]^2 square.
    EXPECT_LT(res_mp->getGeometryN(0)->getArea(), 400.0);
  }

  // Three-component MultiPolygon: holed polygon with an island inside its hole
  // plus a separate polygon outside.
  std::unique_ptr<Geometry> island_mp = ReadWkt(
      "MULTIPOLYGON ("
      "((0 0, 30 0, 30 30, 0 30, 0 0),"
      " (4 4, 26 4, 26 26, 20 26, 15 22, 10 26, 4 26, 4 4)),"
      "((8 8, 22 8, 22 18, 15 14, 8 18, 8 8)),"
      "((40 0, 50 0, 50 10, 45 6, 40 10, 40 0)))");
  ASSERT_TRUE(island_mp->isValid());

  const std::array<std::unique_ptr<Geometry>, 2> island_results = {
      PolygonHullSimplifier::hull(island_mp.get(), true, 0.0),
      PolygonHullSimplifier::hullByAreaDelta(island_mp.get(), true, 0.5),
  };
  for (const auto& res : island_results) {
    ASSERT_TRUE(res->isValid());
    const auto* res_mp = dynamic_cast<const MultiPolygon*>(res.get());
    ASSERT_NE(res_mp, nullptr);
    EXPECT_EQ(res_mp->getNumGeometries(), 3u);
    EXPECT_TRUE(res_mp->covers(island_mp.get()));
    EXPECT_GE(res_mp->getArea(), island_mp->getArea());
  }
}

TEST_F(PolygonHullSimplifierTest,
       MultiPolygonInnerHullUsesPerPolygonSimplification) {
  std::unique_ptr<Geometry> mp = ReadWkt(
      "MULTIPOLYGON ("
      "((0 0, 20 0, 20 20, 11 20, 10 24, 9 20, 0 20, 0 0),"
      " (4 4, 16 4, 16 14, 12 14, 10 10, 8 14, 4 14, 4 4)),"
      "((30 0, 40 0, 40 10, 36 10, 35 14, 34 10, 30 10, 30 0)))");
  ASSERT_TRUE(mp->isValid());

  std::unique_ptr<Geometry> frac_res =
      PolygonHullSimplifier::hull(mp.get(), false, 0.0);
  ASSERT_TRUE(frac_res->isValid());
  const auto* frac_mp = dynamic_cast<const MultiPolygon*>(frac_res.get());
  ASSERT_NE(frac_mp, nullptr);
  EXPECT_EQ(frac_mp->getNumGeometries(), 2u);
  EXPECT_TRUE(mp->covers(frac_mp));
  EXPECT_LT(frac_mp->getArea(), mp->getArea());

  std::unique_ptr<Geometry> area_res =
      PolygonHullSimplifier::hullByAreaDelta(mp.get(), false, 0.1);
  ASSERT_TRUE(area_res->isValid());
  const auto* area_mp = dynamic_cast<const MultiPolygon*>(area_res.get());
  ASSERT_NE(area_mp, nullptr);
  EXPECT_EQ(area_mp->getNumGeometries(), 2u);
  EXPECT_TRUE(mp->covers(area_mp));
  EXPECT_LE(area_mp->getArea(), mp->getArea());
}

TEST_F(PolygonHullSimplifierTest, MultiPolygonWithEmptyComponent) {
  auto make_mp_with_empty = [&]() -> std::unique_ptr<MultiPolygon> {
    std::vector<std::unique_ptr<Polygon>> polys;
    polys.push_back(factory_->createPolygon());
    polys.push_back(ReadPolygon(kNotchedSquareWkt));
    return factory_->createMultiPolygon(std::move(polys));
  };

  std::unique_ptr<MultiPolygon> mp = make_mp_with_empty();
  ASSERT_EQ(mp->getNumGeometries(), 2u);
  ASSERT_TRUE(mp->getGeometryN(0)->isEmpty());

  for (bool is_outer : {true, false}) {
    std::unique_ptr<Geometry> frac_res =
        PolygonHullSimplifier::hull(mp.get(), is_outer, 0.0);
    const auto* frac_mp = dynamic_cast<const MultiPolygon*>(frac_res.get());
    ASSERT_NE(frac_mp, nullptr);
    ASSERT_EQ(frac_mp->getNumGeometries(), 2u);
    EXPECT_TRUE(frac_mp->getGeometryN(0)->isEmpty());
    EXPECT_TRUE(frac_mp->getGeometryN(1)->isValid());
    if (is_outer) {
      EXPECT_DOUBLE_EQ(frac_mp->getGeometryN(1)->getArea(), 100.0);
    } else {
      EXPECT_EQ(frac_mp->getGeometryN(1)->getNumPoints(), 4u);
      EXPECT_TRUE(mp->getGeometryN(1)->covers(frac_mp->getGeometryN(1)));
    }

    std::unique_ptr<Geometry> delta_res =
        PolygonHullSimplifier::hullByAreaDelta(mp.get(), is_outer, 0.25);
    const auto* delta_mp = dynamic_cast<const MultiPolygon*>(delta_res.get());
    ASSERT_NE(delta_mp, nullptr);
    ASSERT_EQ(delta_mp->getNumGeometries(), 2u);
    EXPECT_TRUE(delta_mp->getGeometryN(0)->isEmpty());
    EXPECT_TRUE(delta_mp->getGeometryN(1)->isValid());
  }
}

TEST_F(PolygonHullSimplifierTest,
       PreservesFactorySridAndHandlesCoordinateScales) {
  GeometryFactory::Ptr srid_factory =
      GeometryFactory::create(factory_->getPrecisionModel(), 4326);
  WKTReader srid_reader(*srid_factory);
  std::unique_ptr<Geometry> poly = srid_reader.read(kNotchedSquareWkt);
  ASSERT_EQ(poly->getSRID(), 4326);

  std::unique_ptr<Geometry> hull =
      PolygonHullSimplifier::hull(poly.get(), true, 0.0);
  EXPECT_EQ(hull->getFactory(), srid_factory.get());
  EXPECT_EQ(hull->getSRID(), 4326);
  EXPECT_DOUBLE_EQ(hull->getArea(), 100.0);

  // Large translated coordinates.
  std::unique_ptr<Geometry> large_poly = ReadWkt(
      "POLYGON ((1000000 2000000, 1000002 2000000, 1000005 2000002, "
      "1000008 2000000, 1000010 2000000, 1000010 2000010, 1000000 2000010, "
      "1000000 2000000))");
  ASSERT_TRUE(large_poly->isValid());
  std::unique_ptr<Geometry> large_hull =
      PolygonHullSimplifier::hullByAreaDelta(large_poly.get(), true, 0.1);
  EXPECT_TRUE(large_hull->isValid());
  EXPECT_TRUE(large_hull->covers(large_poly.get()));
  EXPECT_NEAR(large_hull->getArea(), 100.0, 1e-6);

  // Small scaled coordinates.
  std::unique_ptr<Geometry> small_poly = ReadWkt(
      "POLYGON ((0 0, 0.0002 0, 0.0005 0.0002, 0.0008 0, 0.001 0, "
      "0.001 0.001, 0 0.001, 0 0))");
  ASSERT_TRUE(small_poly->isValid());
  std::unique_ptr<Geometry> small_hull =
      PolygonHullSimplifier::hullByAreaDelta(small_poly.get(), true, 0.1);
  EXPECT_TRUE(small_hull->isValid());
  EXPECT_TRUE(small_hull->covers(small_poly.get()));
  EXPECT_NEAR(small_hull->getArea(), 1e-6, 1e-12);
}

}  // namespace
}  // namespace geos_google
