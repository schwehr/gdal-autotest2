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

#include "third_party/geos/include/geos/algorithm/construct/ExactMaxInscribedCircle.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineSegment.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/IllegalStateException.h"

namespace geos_google {
namespace {

using geos::algorithm::construct::ExactMaxInscribedCircle;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineSegment;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::util::IllegalArgumentException;
using geos::util::IllegalStateException;

class ExactMaxInscribedCircleTest : public ::testing::Test {
 protected:
  ExactMaxInscribedCircleTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> ReadGeom(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  std::unique_ptr<Polygon> ReadPoly(const std::string& wkt) const {
    return reader_.read<Polygon>(wkt);
  }

  static void VerifyInscribedCircle(
      const Polygon& poly, const std::pair<CoordinateXY, CoordinateXY>& result,
      double tol = 1e-9) {
    const CoordinateXY& center = result.first;
    const CoordinateXY& radius_pt = result.second;
    ASSERT_FALSE(center.isNull());
    ASSERT_FALSE(radius_pt.isNull());
    EXPECT_TRUE(center.isValid());
    EXPECT_TRUE(radius_pt.isValid());

    const double radius = center.distance(radius_pt);
    const auto* ring = poly.getExteriorRing()->getCoordinatesRO();
    ASSERT_GE(ring->size(), 4u);

    double min_edge_dist = std::numeric_limits<double>::infinity();
    double radius_pt_min_edge_dist = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i + 1 < ring->size(); ++i) {
      LineSegment seg(ring->getAt(i), ring->getAt(i + 1));
      min_edge_dist = std::min(min_edge_dist, seg.distance(center));
      radius_pt_min_edge_dist =
          std::min(radius_pt_min_edge_dist, seg.distance(radius_pt));
    }
    EXPECT_NEAR(radius, min_edge_dist, tol);
    EXPECT_NEAR(radius_pt_min_edge_dist, 0.0, tol);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(ExactMaxInscribedCircleTest, IsSupportedRejectsNonPolygonsAndEmpty) {
  EXPECT_FALSE(
      ExactMaxInscribedCircle::isSupported(ReadGeom("POINT (1 2)").get()));
  EXPECT_FALSE(
      ExactMaxInscribedCircle::isSupported(ReadGeom("POINT EMPTY").get()));
  EXPECT_FALSE(ExactMaxInscribedCircle::isSupported(
      ReadGeom("LINESTRING (0 0, 10 0)").get()));
  EXPECT_FALSE(ExactMaxInscribedCircle::isSupported(
      ReadGeom("MULTIPOLYGON (((0 0, 1 0, 1 1, 0 1, 0 0)))").get()));
  EXPECT_FALSE(ExactMaxInscribedCircle::isSupported(
      ReadGeom("GEOMETRYCOLLECTION (POLYGON ((0 0, 1 0, 0 1, 0 0)))").get()));
  EXPECT_FALSE(
      ExactMaxInscribedCircle::isSupported(ReadGeom("POLYGON EMPTY").get()));

  auto tri = ReadPoly("POLYGON ((0 0, 4 0, 0 4, 0 0))");
  const LinearRing* ring = tri->getExteriorRing();
  EXPECT_FALSE(ExactMaxInscribedCircle::isSupported(ring));
}

TEST_F(ExactMaxInscribedCircleTest, IsSupportedRejectsPolygonsWithHoles) {
  EXPECT_FALSE(ExactMaxInscribedCircle::isSupported(
      ReadGeom("POLYGON ((0 0, 10 0, 0 10, 0 0), (1 1, 3 1, 1 3, 1 1))")
          .get()));
  EXPECT_FALSE(ExactMaxInscribedCircle::isSupported(
      ReadGeom("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
               "(2 2, 8 2, 8 8, 2 8, 2 2))")
          .get()));
}

TEST_F(ExactMaxInscribedCircleTest,
       IsSupportedAcceptsTrianglesAndConvexQuadrilaterals) {
  EXPECT_TRUE(ExactMaxInscribedCircle::isSupported(
      ReadGeom("POLYGON ((0 0, 3 0, 0 4, 0 0))").get()));
  EXPECT_TRUE(ExactMaxInscribedCircle::isSupported(
      ReadGeom("POLYGON ((0 0, 0 4, 3 0, 0 0))").get()));
  EXPECT_TRUE(ExactMaxInscribedCircle::isSupported(
      ReadGeom("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))").get()));
  EXPECT_TRUE(ExactMaxInscribedCircle::isSupported(
      ReadGeom("POLYGON ((0 0, 0 4, 4 4, 4 0, 0 0))").get()));
  EXPECT_TRUE(ExactMaxInscribedCircle::isSupported(
      ReadGeom("POLYGON ((0 0, 10 0, 8 6, 0 2, 0 0))").get()));
  // Degenerate 5-point ring containing three collinear consecutive vertices.
  EXPECT_TRUE(ExactMaxInscribedCircle::isSupported(
      ReadGeom("POLYGON ((0 0, 5 0, 10 0, 0 10, 0 0))").get()));
}

TEST_F(ExactMaxInscribedCircleTest,
       IsSupportedRejectsConcaveQuadsAndHigherOrderPolygons) {
  // Concave quadrilaterals (CCW and CW, starting before or at reflex vertex).
  EXPECT_FALSE(ExactMaxInscribedCircle::isSupported(
      ReadGeom("POLYGON ((0 0, 4 0, 1 1, 0 4, 0 0))").get()));
  EXPECT_FALSE(ExactMaxInscribedCircle::isSupported(
      ReadGeom("POLYGON ((0 0, 0 4, 1 1, 4 0, 0 0))").get()));
  EXPECT_FALSE(ExactMaxInscribedCircle::isSupported(
      ReadGeom("POLYGON ((4 0, 1 1, 0 4, 0 0, 4 0))").get()));

  // Convex pentagon and hexagon are not supported by ExactMaxInscribedCircle.
  EXPECT_FALSE(ExactMaxInscribedCircle::isSupported(
      ReadGeom("POLYGON ((0 0, 2 0, 3 2, 1 4, -1 2, 0 0))").get()));
  EXPECT_FALSE(ExactMaxInscribedCircle::isSupported(
      ReadGeom("POLYGON ((0 0, 2 0, 3 1, 2 3, 0 3, -1 1, 0 0))").get()));
}

TEST_F(ExactMaxInscribedCircleTest, ComputeRadiusTriangles) {
  {
    auto tri_ccw = ReadPoly("POLYGON ((0 0, 3 0, 0 4, 0 0))");
    auto res = ExactMaxInscribedCircle::computeRadius(tri_ccw.get());
    VerifyInscribedCircle(*tri_ccw, res);
    EXPECT_NEAR(res.first.x, 1.0, 1e-12);
    EXPECT_NEAR(res.first.y, 1.0, 1e-12);
    EXPECT_NEAR(res.second.x, 1.0, 1e-12);
    EXPECT_NEAR(res.second.y, 0.0, 1e-12);
    EXPECT_NEAR(res.first.distance(res.second), 1.0, 1e-12);
  }
  {
    auto tri_cw = ReadPoly("POLYGON ((0 0, 0 4, 3 0, 0 0))");
    auto res = ExactMaxInscribedCircle::computeRadius(tri_cw.get());
    VerifyInscribedCircle(*tri_cw, res);
    EXPECT_NEAR(res.first.x, 1.0, 1e-12);
    EXPECT_NEAR(res.first.y, 1.0, 1e-12);
    EXPECT_NEAR(res.second.x, 0.0, 1e-12);
    EXPECT_NEAR(res.second.y, 1.0, 1e-12);
    EXPECT_NEAR(res.first.distance(res.second), 1.0, 1e-12);
  }
  {
    auto tri_obtuse = ReadPoly("POLYGON ((0 0, 10 0, 2 1, 0 0))");
    auto res = ExactMaxInscribedCircle::computeRadius(tri_obtuse.get());
    VerifyInscribedCircle(*tri_obtuse, res);
    const double perimeter = 10.0 + std::sqrt(65.0) + std::sqrt(5.0);
    const double expected_radius = 10.0 / perimeter;
    EXPECT_NEAR(res.first.distance(res.second), expected_radius, 1e-12);
  }
  {
    auto tri_collinear = ReadPoly("POLYGON ((0 0, 1 0, 4 0, 0 0))");
    auto res = ExactMaxInscribedCircle::computeRadius(tri_collinear.get());
    VerifyInscribedCircle(*tri_collinear, res);
    EXPECT_NEAR(res.first.x, 1.0, 1e-12);
    EXPECT_NEAR(res.first.y, 0.0, 1e-12);
    EXPECT_NEAR(res.first.distance(res.second), 0.0, 1e-12);
  }
}

TEST_F(ExactMaxInscribedCircleTest,
       ComputeRadiusSquareAndRectangleBothOrientations) {
  for (const std::string& wkt : {
           std::string("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))"),
           std::string("POLYGON ((0 0, 0 4, 4 4, 4 0, 0 0))"),
       }) {
    auto square = ReadPoly(wkt);
    auto res = ExactMaxInscribedCircle::computeRadius(square.get());
    VerifyInscribedCircle(*square, res);
    EXPECT_NEAR(res.first.x, 2.0, 1e-9);
    EXPECT_NEAR(res.first.y, 2.0, 1e-9);
    EXPECT_NEAR(res.first.distance(res.second), 2.0, 1e-9);
  }

  // Wide CW rectangle has exterior bisector intersections skipped by
  // isPointInConvexRing.
  {
    auto wide_rect_cw = ReadPoly("POLYGON ((0 0, 0 4, 10 4, 10 0, 0 0))");
    auto res = ExactMaxInscribedCircle::computeRadius(wide_rect_cw.get());
    VerifyInscribedCircle(*wide_rect_cw, res);
    EXPECT_GE(res.first.x, 2.0 - 1e-9);
    EXPECT_LE(res.first.x, 8.0 + 1e-9);
    EXPECT_NEAR(res.first.y, 2.0, 1e-9);
    EXPECT_NEAR(res.first.distance(res.second), 2.0, 1e-9);
  }

  // Tall CCW rectangle exercises ring reversal and exterior node filtering.
  {
    auto tall_rect_ccw = ReadPoly("POLYGON ((0 0, 4 0, 4 10, 0 10, 0 0))");
    auto res = ExactMaxInscribedCircle::computeRadius(tall_rect_ccw.get());
    VerifyInscribedCircle(*tall_rect_ccw, res);
    EXPECT_NEAR(res.first.x, 2.0, 1e-9);
    EXPECT_GE(res.first.y, 2.0 - 1e-9);
    EXPECT_LE(res.first.y, 8.0 + 1e-9);
    EXPECT_NEAR(res.first.distance(res.second), 2.0, 1e-9);
  }
}

TEST_F(ExactMaxInscribedCircleTest,
       ComputeRadiusAsymmetricQuadAndCollinearVertices) {
  const double expected_radius =
      42.0 / (7.0 + 3.0 * std::sqrt(5.0) + std::sqrt(10.0));
  const double expected_cx = expected_radius * (2.0 + std::sqrt(5.0)) - 4.0;

  // Test multiple vertex start positions and orientations so the node loop
  // encounters both larger-than-maxDist and smaller-than-maxDist candidates.
  for (const std::string& wkt : {
           std::string("POLYGON ((0 0, 10 0, 8 6, 0 2, 0 0))"),
           std::string("POLYGON ((8 6, 10 0, 0 0, 0 2, 8 6))"),
           std::string("POLYGON ((10 0, 8 6, 0 2, 0 0, 10 0))"),
       }) {
    auto quad = ReadPoly(wkt);
    auto res = ExactMaxInscribedCircle::computeRadius(quad.get());
    VerifyInscribedCircle(*quad, res);
    EXPECT_NEAR(res.first.x, expected_cx, 1e-9);
    EXPECT_NEAR(res.first.y, expected_radius, 1e-9);
    EXPECT_NEAR(res.first.distance(res.second), expected_radius, 1e-9);
  }

  // 5-point ring with 3 collinear vertices matches the underlying triangle MIC.
  auto collinear_quad = ReadPoly("POLYGON ((0 0, 5 0, 10 0, 0 10, 0 0))");
  auto res = ExactMaxInscribedCircle::computeRadius(collinear_quad.get());
  VerifyInscribedCircle(*collinear_quad, res);
  const double expected_tri_r = 10.0 / (2.0 + std::sqrt(2.0));
  EXPECT_NEAR(res.first.x, expected_tri_r, 1e-9);
  EXPECT_NEAR(res.first.y, expected_tri_r, 1e-9);
  EXPECT_NEAR(res.first.distance(res.second), expected_tri_r, 1e-9);
}

TEST_F(ExactMaxInscribedCircleTest,
       ComputeRadiusThinTrapezoidWithNonIntersectingBisectors) {
  // Adjacent bisectors from (0, 0) and (10, 0) converge near y = 1000, beyond
  // the 2 * diameter segment length (~420), leaving nodePt null.
  for (const std::string& wkt : {
           std::string("POLYGON ((-100 1, 110 1, 10 0, 0 0, -100 1))"),
           std::string("POLYGON ((-100 1, 0 0, 10 0, 110 1, -100 1))"),
       }) {
    auto trapezoid = ReadPoly(wkt);
    auto res = ExactMaxInscribedCircle::computeRadius(trapezoid.get());
    VerifyInscribedCircle(*trapezoid, res, 1e-7);
    EXPECT_NEAR(res.first.y, 0.5, 1e-4);
    EXPECT_NEAR(res.first.distance(res.second), 0.5, 1e-4);
  }
}

TEST_F(ExactMaxInscribedCircleTest, ComputeRadiusThrowsOnInvalidInputs) {
  auto empty_poly = ReadPoly("POLYGON EMPTY");
  EXPECT_THROW(ExactMaxInscribedCircle::computeRadius(empty_poly.get()),
               IllegalArgumentException);

  auto pentagon = ReadPoly("POLYGON ((0 0, 2 0, 3 2, 1 4, -1 2, 0 0))");
  EXPECT_THROW(ExactMaxInscribedCircle::computeRadius(pentagon.get()),
               IllegalArgumentException);

  // Calling computeRadius directly on a non-convex quadrilateral skips
  // isSupported() and triggers IllegalStateException inside bisector checks.
  auto concave_ccw = ReadPoly("POLYGON ((0 0, 4 0, 1 1, 0 4, 0 0))");
  EXPECT_THROW(ExactMaxInscribedCircle::computeRadius(concave_ccw.get()),
               IllegalStateException);

  // Reflex vertex at index 0 in CW order exercises iPrev wrap-around.
  auto concave_cw_idx0 = ReadPoly("POLYGON ((1 1, 4 0, 0 0, 0 4, 1 1))");
  EXPECT_THROW(ExactMaxInscribedCircle::computeRadius(concave_cw_idx0.get()),
               IllegalStateException);
}

}  // namespace
}  // namespace geos_google
