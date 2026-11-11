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

#include "third_party/geos/include/geos/algorithm/MinimumBoundingCircle.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <numbers>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::algorithm::MinimumBoundingCircle;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::geom::Point;
using geos::io::WKTReader;

void ExpectEmptyMbc(const Geometry& geom) {
  MinimumBoundingCircle mbc(&geom);

  EXPECT_TRUE(mbc.getExtremalPoints().empty());

  CoordinateXY centre = mbc.getCentre();
  EXPECT_TRUE(centre.isNull());
  EXPECT_TRUE(std::isnan(centre.x));
  EXPECT_TRUE(std::isnan(centre.y));

  EXPECT_EQ(mbc.getRadius(), 0.0);

  std::unique_ptr<Geometry> circle = mbc.getCircle();
  ASSERT_NE(circle, nullptr);
  EXPECT_EQ(circle->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(circle->isEmpty());

  std::unique_ptr<Geometry> diameter = mbc.getDiameter();
  ASSERT_NE(diameter, nullptr);
  EXPECT_EQ(diameter->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(diameter->isEmpty());

  std::unique_ptr<Geometry> max_diameter = mbc.getMaximumDiameter();
  ASSERT_NE(max_diameter, nullptr);
  EXPECT_EQ(max_diameter->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(max_diameter->isEmpty());
}

void ExpectPointGeometryAt(const Geometry& geom, double x, double y,
                           double tol = 1e-12) {
  EXPECT_EQ(geom.getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_FALSE(geom.isEmpty());
  const auto* pt = dynamic_cast<const Point*>(&geom);
  ASSERT_NE(pt, nullptr);
  EXPECT_NEAR(pt->getX(), x, tol);
  EXPECT_NEAR(pt->getY(), y, tol);
}

void ExpectLineEndpointsMatchUnordered(const Geometry& geom,
                                       const CoordinateXY& a,
                                       const CoordinateXY& b,
                                       double tol = 1e-10) {
  EXPECT_EQ(geom.getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  const auto* line = dynamic_cast<const LineString*>(&geom);
  ASSERT_NE(line, nullptr);
  ASSERT_EQ(line->getNumPoints(), 2u);
  const CoordinateXY& p0 = line->getCoordinateN(0);
  const CoordinateXY& p1 = line->getCoordinateN(1);
  bool forward = p0.equals2D(a, tol) && p1.equals2D(b, tol);
  bool reverse = p0.equals2D(b, tol) && p1.equals2D(a, tol);
  EXPECT_TRUE(forward || reverse)
      << "Expected endpoints (" << a.x << ", " << a.y << ") and (" << b.x
      << ", " << b.y << "), got (" << p0.x << ", " << p0.y << ") and (" << p1.x
      << ", " << p1.y << ")";
}

void ExpectCirclePolygonMatches(const Geometry& circle_geom,
                                const CoordinateXY& centre, double radius) {
  EXPECT_EQ(circle_geom.getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_FALSE(circle_geom.isEmpty());
  EXPECT_TRUE(circle_geom.isValid());

  const double expected_area = std::numbers::pi * radius * radius;
  EXPECT_NEAR(circle_geom.getArea(), expected_area, 0.02 * expected_area);

  const Envelope* env = circle_geom.getEnvelopeInternal();
  ASSERT_NE(env, nullptr);
  const double tol = 1e-6 * std::max(1.0, radius);
  EXPECT_NEAR(env->getMinX(), centre.x - radius, tol);
  EXPECT_NEAR(env->getMaxX(), centre.x + radius, tol);
  EXPECT_NEAR(env->getMinY(), centre.y - radius, tol);
  EXPECT_NEAR(env->getMaxY(), centre.y + radius, tol);
}

void ExpectEnclosingCircleInvariant(const Geometry& geom,
                                    const CoordinateXY& centre, double radius,
                                    const std::vector<CoordinateXY>& extremal,
                                    double tol = 1e-10) {
  ASSERT_TRUE(extremal.size() == 2u || extremal.size() == 3u);
  for (const CoordinateXY& ep : extremal) {
    EXPECT_NEAR(centre.distance(ep), radius, tol);
  }
  std::unique_ptr<CoordinateSequence> coords = geom.getCoordinates();
  ASSERT_NE(coords, nullptr);
  for (std::size_t i = 0; i < coords->size(); ++i) {
    const CoordinateXY& pt = coords->getAt<CoordinateXY>(i);
    EXPECT_LE(centre.distance(pt), radius + tol);
  }
}

class MinimumBoundingCircleTest : public ::testing::Test {
 protected:
  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader reader_{*factory_};
};

TEST_F(MinimumBoundingCircleTest,
       EmptyGeometriesProduceNullCentreAndEmptyOutputs) {
  std::vector<std::unique_ptr<Geometry>> empty_geoms;
  empty_geoms.push_back(reader_.read("POINT EMPTY"));
  empty_geoms.push_back(reader_.read("LINESTRING EMPTY"));
  empty_geoms.push_back(reader_.read("POLYGON EMPTY"));
  empty_geoms.push_back(reader_.read("MULTIPOINT EMPTY"));
  empty_geoms.push_back(reader_.read("GEOMETRYCOLLECTION EMPTY"));

  for (const auto& geom : empty_geoms) {
    ASSERT_NE(geom, nullptr);
    ExpectEmptyMbc(*geom);
  }
}

TEST_F(MinimumBoundingCircleTest, SinglePointGeometriesProducePointOutputs) {
  {
    std::unique_ptr<Geometry> pt = reader_.read("POINT (1.5 -2.5)");
    MinimumBoundingCircle mbc(pt.get());

    std::vector<CoordinateXY> extremal = mbc.getExtremalPoints();
    ASSERT_EQ(extremal.size(), 1u);
    EXPECT_TRUE(extremal[0].equals2D(CoordinateXY(1.5, -2.5)));

    CoordinateXY centre = mbc.getCentre();
    EXPECT_FALSE(centre.isNull());
    EXPECT_DOUBLE_EQ(centre.x, 1.5);
    EXPECT_DOUBLE_EQ(centre.y, -2.5);
    EXPECT_DOUBLE_EQ(mbc.getRadius(), 0.0);

    ExpectPointGeometryAt(*mbc.getCircle(), 1.5, -2.5);
    ExpectPointGeometryAt(*mbc.getDiameter(), 1.5, -2.5);
    ExpectPointGeometryAt(*mbc.getMaximumDiameter(), 1.5, -2.5);
  }

  {
    std::unique_ptr<Geometry> pt_z = reader_.read("POINT Z (3.0 4.0 9.0)");
    MinimumBoundingCircle mbc(pt_z.get());
    EXPECT_EQ(mbc.getExtremalPoints().size(), 1u);
    EXPECT_TRUE(mbc.getCentre().equals2D(CoordinateXY(3.0, 4.0)));
    EXPECT_DOUBLE_EQ(mbc.getRadius(), 0.0);
    ExpectPointGeometryAt(*mbc.getCircle(), 3.0, 4.0);
  }

  {
    std::unique_ptr<Geometry> gc =
        reader_.read("GEOMETRYCOLLECTION (POINT EMPTY, POINT (-7.0 8.0))");
    MinimumBoundingCircle mbc(gc.get());
    std::vector<CoordinateXY> extremal = mbc.getExtremalPoints();
    ASSERT_EQ(extremal.size(), 1u);
    EXPECT_TRUE(extremal[0].equals2D(CoordinateXY(-7.0, 8.0)));
    EXPECT_TRUE(mbc.getCentre().equals2D(CoordinateXY(-7.0, 8.0)));
    EXPECT_DOUBLE_EQ(mbc.getRadius(), 0.0);
  }
}

TEST_F(MinimumBoundingCircleTest, RepeatedCallsReuseComputedResult) {
  std::unique_ptr<Geometry> poly =
      reader_.read("POLYGON ((0 0, 6 0, 3 4, 0 0))");
  MinimumBoundingCircle mbc(poly.get());

  const double r1 = mbc.getRadius();
  const CoordinateXY c1 = mbc.getCentre();
  const std::vector<CoordinateXY> e1 = mbc.getExtremalPoints();
  std::unique_ptr<Geometry> d1 = mbc.getDiameter();
  std::unique_ptr<Geometry> md1 = mbc.getMaximumDiameter();
  std::unique_ptr<Geometry> circ1 = mbc.getCircle();

  EXPECT_DOUBLE_EQ(mbc.getRadius(), r1);
  EXPECT_TRUE(mbc.getCentre().equals2D(c1));
  EXPECT_EQ(mbc.getExtremalPoints(), e1);
  EXPECT_TRUE(mbc.getDiameter()->equalsExact(d1.get(), 1e-12));
  EXPECT_TRUE(mbc.getMaximumDiameter()->equalsExact(md1.get(), 1e-12));
  EXPECT_TRUE(mbc.getCircle()->equalsExact(circ1.get(), 1e-12));
}

TEST_F(MinimumBoundingCircleTest,
       TwoPointAndCollinearInputsUseTwoExtremalPoints) {
  {
    std::unique_ptr<Geometry> line = reader_.read("LINESTRING (0 0, 6 8)");
    MinimumBoundingCircle mbc(line.get());

    std::vector<CoordinateXY> extremal = mbc.getExtremalPoints();
    ASSERT_EQ(extremal.size(), 2u);
    EXPECT_TRUE(mbc.getCentre().equals2D(CoordinateXY(3.0, 4.0), 1e-12));
    EXPECT_NEAR(mbc.getRadius(), 5.0, 1e-12);

    ExpectLineEndpointsMatchUnordered(*mbc.getDiameter(), CoordinateXY(0, 0),
                                      CoordinateXY(6, 8));
    ExpectLineEndpointsMatchUnordered(*mbc.getMaximumDiameter(),
                                      CoordinateXY(0, 0), CoordinateXY(6, 8));
    ExpectCirclePolygonMatches(*mbc.getCircle(), CoordinateXY(3.0, 4.0), 5.0);
  }

  {
    std::unique_ptr<Geometry> mp = reader_.read("MULTIPOINT ((-4 1), (2 9))");
    MinimumBoundingCircle mbc(mp.get());

    ASSERT_EQ(mbc.getExtremalPoints().size(), 2u);
    EXPECT_TRUE(mbc.getCentre().equals2D(CoordinateXY(-1.0, 5.0), 1e-12));
    EXPECT_NEAR(mbc.getRadius(), 5.0, 1e-12);
    ExpectLineEndpointsMatchUnordered(*mbc.getMaximumDiameter(),
                                      CoordinateXY(-4, 1), CoordinateXY(2, 9));
  }

  {
    std::unique_ptr<Geometry> collinear =
        reader_.read("LINESTRING (0 0, 2 2, 5 5, 10 10)");
    MinimumBoundingCircle mbc(collinear.get());

    std::vector<CoordinateXY> extremal = mbc.getExtremalPoints();
    ASSERT_EQ(extremal.size(), 2u);
    EXPECT_TRUE(mbc.getCentre().equals2D(CoordinateXY(5.0, 5.0), 1e-12));
    EXPECT_NEAR(mbc.getRadius(), std::hypot(5.0, 5.0), 1e-12);
    ExpectEnclosingCircleInvariant(*collinear, mbc.getCentre(), mbc.getRadius(),
                                   extremal);
    ExpectLineEndpointsMatchUnordered(*mbc.getMaximumDiameter(),
                                      CoordinateXY(0, 0), CoordinateXY(10, 10));
  }

  {
    std::unique_ptr<Geometry> folded =
        reader_.read("LINESTRING (1 1, 5 1, 1 1)");
    MinimumBoundingCircle mbc(folded.get());

    ASSERT_EQ(mbc.getExtremalPoints().size(), 2u);
    EXPECT_TRUE(mbc.getCentre().equals2D(CoordinateXY(3.0, 1.0), 1e-12));
    EXPECT_NEAR(mbc.getRadius(), 2.0, 1e-12);
    ExpectLineEndpointsMatchUnordered(*mbc.getMaximumDiameter(),
                                      CoordinateXY(1, 1), CoordinateXY(5, 1));
  }
}

TEST_F(MinimumBoundingCircleTest, ObtuseTrianglesCollapseToTwoExtremalPoints) {
  // Obtuse at R (PRQ > 90 deg) -> returns {P, Q} directly on first iteration.
  {
    std::unique_ptr<Geometry> poly =
        reader_.read("POLYGON ((-5 0, 5 0.1, 0 2, -5 0))");
    MinimumBoundingCircle mbc(poly.get());

    std::vector<CoordinateXY> extremal = mbc.getExtremalPoints();
    ASSERT_EQ(extremal.size(), 2u);
    EXPECT_TRUE(mbc.getCentre().equals2D(CoordinateXY(0.0, 0.05), 1e-12));
    EXPECT_NEAR(mbc.getRadius(), std::hypot(5.0, 0.05), 1e-12);
    ExpectEnclosingCircleInvariant(*poly, mbc.getCentre(), mbc.getRadius(),
                                   extremal);
    ExpectLineEndpointsMatchUnordered(
        *mbc.getMaximumDiameter(), CoordinateXY(-5, 0), CoordinateXY(5, 0.1));
    ExpectCirclePolygonMatches(*mbc.getCircle(), mbc.getCentre(),
                               mbc.getRadius());
  }

  // Obtuse at P (RPQ > 90 deg) -> updates P = R and iterates.
  {
    std::unique_ptr<Geometry> poly =
        reader_.read("POLYGON ((0 0, 4 0.1, -3 5, 0 0))");
    MinimumBoundingCircle mbc(poly.get());

    std::vector<CoordinateXY> extremal = mbc.getExtremalPoints();
    ASSERT_EQ(extremal.size(), 2u);
    EXPECT_TRUE(mbc.getCentre().equals2D(CoordinateXY(0.5, 2.55), 1e-12));
    EXPECT_NEAR(mbc.getRadius(), std::hypot(3.5, 2.45), 1e-12);
    ExpectEnclosingCircleInvariant(*poly, mbc.getCentre(), mbc.getRadius(),
                                   extremal);
    ExpectLineEndpointsMatchUnordered(
        *mbc.getMaximumDiameter(), CoordinateXY(-3, 5), CoordinateXY(4, 0.1));
  }

  // Obtuse at Q (RQP > 90 deg) -> updates Q = R and iterates.
  {
    std::unique_ptr<Geometry> poly =
        reader_.read("POLYGON ((4 0, 0 0.1, -3 5, 4 0))");
    MinimumBoundingCircle mbc(poly.get());

    std::vector<CoordinateXY> extremal = mbc.getExtremalPoints();
    ASSERT_EQ(extremal.size(), 2u);
    EXPECT_TRUE(mbc.getCentre().equals2D(CoordinateXY(0.5, 2.5), 1e-12));
    EXPECT_NEAR(mbc.getRadius(), std::hypot(3.5, 2.5), 1e-12);
    ExpectEnclosingCircleInvariant(*poly, mbc.getCentre(), mbc.getRadius(),
                                   extremal);
    ExpectLineEndpointsMatchUnordered(*mbc.getMaximumDiameter(),
                                      CoordinateXY(4, 0), CoordinateXY(-3, 5));
  }

  // Q selected along the -X direction because |dy|/len is smaller for (-10, 1)
  // than for (1, 5).
  {
    std::unique_ptr<Geometry> poly =
        reader_.read("POLYGON ((0 0, 1 5, -10 1, 0 0))");
    MinimumBoundingCircle mbc(poly.get());

    std::vector<CoordinateXY> extremal = mbc.getExtremalPoints();
    ASSERT_EQ(extremal.size(), 2u);
    EXPECT_TRUE(mbc.getCentre().equals2D(CoordinateXY(-4.5, 3.0), 1e-12));
    EXPECT_NEAR(mbc.getRadius(), std::hypot(5.5, 2.0), 1e-12);
    ExpectEnclosingCircleInvariant(*poly, mbc.getCentre(), mbc.getRadius(),
                                   extremal);
    ExpectLineEndpointsMatchUnordered(*mbc.getMaximumDiameter(),
                                      CoordinateXY(1, 5), CoordinateXY(-10, 1));
  }
}

TEST_F(MinimumBoundingCircleTest, AcuteTrianglesAndFarthestPointBranches) {
  // Branch 1 of farthestPoints(): dist(P, Q) is strictly longest.
  {
    std::unique_ptr<Geometry> poly =
        reader_.read("POLYGON ((0 0, 10 0, 5 7, 0 0))");
    MinimumBoundingCircle mbc(poly.get());

    std::vector<CoordinateXY> extremal = mbc.getExtremalPoints();
    ASSERT_EQ(extremal.size(), 3u);
    EXPECT_TRUE(mbc.getCentre().equals2D(CoordinateXY(5.0, 12.0 / 7.0), 1e-11));
    EXPECT_NEAR(mbc.getRadius(), 37.0 / 7.0, 1e-11);
    ExpectEnclosingCircleInvariant(*poly, mbc.getCentre(), mbc.getRadius(),
                                   extremal);
    ExpectLineEndpointsMatchUnordered(*mbc.getMaximumDiameter(),
                                      CoordinateXY(0, 0), CoordinateXY(10, 0));
    ExpectLineEndpointsMatchUnordered(*mbc.getDiameter(), extremal[0],
                                      extremal[1]);
    ExpectCirclePolygonMatches(*mbc.getCircle(), mbc.getCentre(),
                               mbc.getRadius());
  }

  // Branch 2 of farthestPoints(): dist(Q, R) is strictly longest.
  {
    std::unique_ptr<Geometry> poly =
        reader_.read("POLYGON ((0 0, 8 0.5, 1 8, 0 0))");
    MinimumBoundingCircle mbc(poly.get());

    std::vector<CoordinateXY> extremal = mbc.getExtremalPoints();
    ASSERT_EQ(extremal.size(), 3u);
    ExpectEnclosingCircleInvariant(*poly, mbc.getCentre(), mbc.getRadius(),
                                   extremal);
    ExpectLineEndpointsMatchUnordered(*mbc.getMaximumDiameter(),
                                      CoordinateXY(8, 0.5), CoordinateXY(1, 8));
    ExpectLineEndpointsMatchUnordered(*mbc.getDiameter(), extremal[0],
                                      extremal[1]);
  }

  // Branch 3 of farthestPoints(): dist(R, P) is strictly longest.
  {
    std::unique_ptr<Geometry> poly =
        reader_.read("POLYGON ((0 0, 8 0.5, 7 8, 0 0))");
    MinimumBoundingCircle mbc(poly.get());

    std::vector<CoordinateXY> extremal = mbc.getExtremalPoints();
    ASSERT_EQ(extremal.size(), 3u);
    ExpectEnclosingCircleInvariant(*poly, mbc.getCentre(), mbc.getRadius(),
                                   extremal);
    ExpectLineEndpointsMatchUnordered(*mbc.getMaximumDiameter(),
                                      CoordinateXY(7, 8), CoordinateXY(0, 0));
    ExpectLineEndpointsMatchUnordered(*mbc.getDiameter(), extremal[0],
                                      extremal[1]);
  }
}

TEST_F(MinimumBoundingCircleTest, RightAndEquilateralTriangles) {
  // 3-4-5 right triangle: dot product is 0, so isObtuse is false and all 3
  // vertices are extremal points.
  {
    std::unique_ptr<Geometry> right_tri =
        reader_.read("POLYGON ((0 0, 3 0, 0 4, 0 0))");
    MinimumBoundingCircle mbc(right_tri.get());

    std::vector<CoordinateXY> extremal = mbc.getExtremalPoints();
    ASSERT_EQ(extremal.size(), 3u);
    EXPECT_TRUE(mbc.getCentre().equals2D(CoordinateXY(1.5, 2.0), 1e-12));
    EXPECT_NEAR(mbc.getRadius(), 2.5, 1e-12);
    ExpectEnclosingCircleInvariant(*right_tri, mbc.getCentre(), mbc.getRadius(),
                                   extremal);
    ExpectLineEndpointsMatchUnordered(*mbc.getMaximumDiameter(),
                                      CoordinateXY(3, 0), CoordinateXY(0, 4));
  }

  // Equilateral triangle built directly via CoordinateSequence.
  {
    const double h = std::sqrt(3.0);
    auto seq = std::make_unique<CoordinateSequence>(
        std::initializer_list<CoordinateXY>{
            {0.0, 0.0}, {2.0, 0.0}, {1.0, h}, {0.0, 0.0}});
    std::unique_ptr<geos::geom::LinearRing> ring =
        factory_->createLinearRing(std::move(seq));
    // Test LinearRing directly as input geometry.
    {
      MinimumBoundingCircle ring_mbc(ring.get());
      EXPECT_EQ(ring_mbc.getExtremalPoints().size(), 3u);
      EXPECT_TRUE(
          ring_mbc.getCentre().equals2D(CoordinateXY(1.0, h / 3.0), 1e-12));
      EXPECT_NEAR(ring_mbc.getRadius(), 2.0 / h, 1e-12);
    }

    std::unique_ptr<geos::geom::Polygon> eq_tri =
        factory_->createPolygon(std::move(ring));
    MinimumBoundingCircle mbc(eq_tri.get());

    std::vector<CoordinateXY> extremal = mbc.getExtremalPoints();
    ASSERT_EQ(extremal.size(), 3u);
    EXPECT_TRUE(mbc.getCentre().equals2D(CoordinateXY(1.0, h / 3.0), 1e-12));
    EXPECT_NEAR(mbc.getRadius(), 2.0 / h, 1e-12);
    ExpectEnclosingCircleInvariant(*eq_tri, mbc.getCentre(), mbc.getRadius(),
                                   extremal);
  }
}

TEST_F(MinimumBoundingCircleTest, MultiVertexPolygonsHolesAndRegularNgon) {
  // Rectangle.
  {
    std::unique_ptr<Geometry> rect =
        reader_.read("POLYGON ((0 0, 10 0, 10 4, 0 4, 0 0))");
    MinimumBoundingCircle mbc(rect.get());

    EXPECT_TRUE(mbc.getCentre().equals2D(CoordinateXY(5.0, 2.0), 1e-12));
    EXPECT_NEAR(mbc.getRadius(), std::hypot(5.0, 2.0), 1e-12);
    ExpectEnclosingCircleInvariant(*rect, mbc.getCentre(), mbc.getRadius(),
                                   mbc.getExtremalPoints());
    EXPECT_NEAR(mbc.getMaximumDiameter()->getLength(), std::hypot(10.0, 4.0),
                1e-12);
  }

  // Concave polygon with a hole.
  {
    std::unique_ptr<Geometry> concave = reader_.read(
        "POLYGON ((-10 0, 0 -6, 10 0, 4 2, 0 8, -4 2, -10 0), "
        "(-1 0, 1 0, 0 1, -1 0))");
    MinimumBoundingCircle mbc(concave.get());
    std::vector<CoordinateXY> extremal = mbc.getExtremalPoints();
    ExpectEnclosingCircleInvariant(*concave, mbc.getCentre(), mbc.getRadius(),
                                   extremal);
    ExpectCirclePolygonMatches(*mbc.getCircle(), mbc.getCentre(),
                               mbc.getRadius());
  }

  // 16-gon with interior clutter points, exercising multiple candidate angles
  // in pointWithMinAngleWithSegment().
  {
    const CoordinateXY true_centre(3.5, -2.0);
    const double true_radius = 7.0;
    std::vector<CoordinateXY> pts;
    constexpr int kSides = 16;
    for (int i = 0; i < kSides; ++i) {
      const double angle =
          (2.0 * std::numbers::pi * static_cast<double>(i)) / kSides;
      pts.emplace_back(true_centre.x + true_radius * std::cos(angle),
                       true_centre.y + true_radius * std::sin(angle));
    }
    pts.emplace_back(true_centre.x + 1.0, true_centre.y - 1.5);
    pts.emplace_back(true_centre.x - 2.0, true_centre.y + 0.5);
    pts.emplace_back(true_centre.x, true_centre.y);

    std::unique_ptr<geos::geom::MultiPoint> mp =
        factory_->createMultiPoint(pts);
    MinimumBoundingCircle mbc(mp.get());

    EXPECT_TRUE(mbc.getCentre().equals2D(true_centre, 1e-10));
    EXPECT_NEAR(mbc.getRadius(), true_radius, 1e-10);
    ExpectEnclosingCircleInvariant(*mp, mbc.getCentre(), mbc.getRadius(),
                                   mbc.getExtremalPoints(), 1e-9);
  }

  // Asymmetric 7-point polygon requiring baseline updates across iterations.
  {
    std::unique_ptr<Geometry> poly = reader_.read(
        "POLYGON ((0 0, 2 0.1, 4 1, 3 5, 0.5 7, -3 4, -2 1, 0 0))");
    MinimumBoundingCircle mbc(poly.get());
    ExpectEnclosingCircleInvariant(*poly, mbc.getCentre(), mbc.getRadius(),
                                   mbc.getExtremalPoints());
    ExpectCirclePolygonMatches(*mbc.getCircle(), mbc.getCentre(),
                               mbc.getRadius());
  }
}

TEST_F(MinimumBoundingCircleTest, MultiGeometriesAndLargeOffsetCoordinates) {
  {
    std::unique_ptr<Geometry> mls =
        reader_.read("MULTILINESTRING ((0 0, 1 1), (6 0, 6 8))");
    MinimumBoundingCircle mbc(mls.get());
    EXPECT_TRUE(mbc.getCentre().equals2D(CoordinateXY(3.0, 4.0), 1e-12));
    EXPECT_NEAR(mbc.getRadius(), 5.0, 1e-12);
    ExpectEnclosingCircleInvariant(*mls, mbc.getCentre(), mbc.getRadius(),
                                   mbc.getExtremalPoints());
  }

  {
    std::unique_ptr<Geometry> mpoly = reader_.read(
        "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
        "((8 6, 10 6, 10 8, 8 8, 8 6)))");
    MinimumBoundingCircle mbc(mpoly.get());
    EXPECT_TRUE(mbc.getCentre().equals2D(CoordinateXY(5.0, 4.0), 1e-12));
    EXPECT_NEAR(mbc.getRadius(), std::hypot(5.0, 4.0), 1e-12);
    ExpectEnclosingCircleInvariant(*mpoly, mbc.getCentre(), mbc.getRadius(),
                                   mbc.getExtremalPoints());
    ExpectLineEndpointsMatchUnordered(*mbc.getMaximumDiameter(),
                                      CoordinateXY(0, 0), CoordinateXY(10, 8));
  }

  {
    std::unique_ptr<Geometry> gc = reader_.read(
        "GEOMETRYCOLLECTION (POINT (-3 0), LINESTRING (3 0, 0 4), "
        "POLYGON ((-1 -1, 1 -1, 0 1, -1 -1)))");
    MinimumBoundingCircle mbc(gc.get());
    EXPECT_TRUE(mbc.getCentre().equals2D(CoordinateXY(0.0, 0.875), 1e-12));
    EXPECT_NEAR(mbc.getRadius(), 3.125, 1e-12);
    ExpectEnclosingCircleInvariant(*gc, mbc.getCentre(), mbc.getRadius(),
                                   mbc.getExtremalPoints());
  }

  {
    std::unique_ptr<Geometry> shifted = reader_.read(
        "POLYGON ((1000000 2000000, 1000006 2000000, "
        "1000003 2000004, 1000000 2000000))");
    MinimumBoundingCircle mbc(shifted.get());
    EXPECT_TRUE(
        mbc.getCentre().equals2D(CoordinateXY(1000003.0, 2000000.875), 1e-9));
    EXPECT_NEAR(mbc.getRadius(), 3.125, 1e-9);
    ExpectEnclosingCircleInvariant(*shifted, mbc.getCentre(), mbc.getRadius(),
                                   mbc.getExtremalPoints(), 1e-9);
  }
}

TEST_F(MinimumBoundingCircleTest, PreservesZAndMFlagsInDiameterOutputs) {
  // XYZ input: getDiameter() and getMaximumDiameter() preserve hasZ == true.
  {
    std::unique_ptr<Geometry> line_z =
        reader_.read("LINESTRING Z (0 0 10, 6 8 20)");
    MinimumBoundingCircle mbc(line_z.get());

    std::unique_ptr<Geometry> diam = mbc.getDiameter();
    std::unique_ptr<Geometry> max_diam = mbc.getMaximumDiameter();
    EXPECT_TRUE(diam->hasZ());
    EXPECT_FALSE(diam->hasM());
    EXPECT_TRUE(max_diam->hasZ());
    EXPECT_FALSE(max_diam->hasM());

    std::unique_ptr<CoordinateSequence> cs = max_diam->getCoordinates();
    ASSERT_EQ(cs->size(), 2u);
    EXPECT_TRUE(std::isnan(cs->getOrdinate(0, CoordinateSequence::Z)));
    EXPECT_TRUE(std::isnan(cs->getOrdinate(1, CoordinateSequence::Z)));
  }

  // XYZM input with 3 extremal points: preserves hasZ == true and hasM == true.
  {
    std::unique_ptr<Geometry> poly_zm =
        reader_.read("POLYGON ZM ((0 0 1 2, 10 0 3 4, 5 7 5 6, 0 0 1 2))");
    MinimumBoundingCircle mbc(poly_zm.get());

    std::unique_ptr<Geometry> diam = mbc.getDiameter();
    std::unique_ptr<Geometry> max_diam = mbc.getMaximumDiameter();
    EXPECT_TRUE(diam->hasZ());
    EXPECT_TRUE(diam->hasM());
    EXPECT_TRUE(max_diam->hasZ());
    EXPECT_TRUE(max_diam->hasM());
    ExpectLineEndpointsMatchUnordered(*max_diam, CoordinateXY(0, 0),
                                      CoordinateXY(10, 0));

    std::unique_ptr<CoordinateSequence> cs = max_diam->getCoordinates();
    ASSERT_EQ(cs->size(), 2u);
    EXPECT_TRUE(std::isnan(cs->getOrdinate(0, CoordinateSequence::Z)));
    EXPECT_TRUE(std::isnan(cs->getOrdinate(0, CoordinateSequence::M)));
  }

  // XYM input: preserves hasZ == false and hasM == true.
  {
    std::unique_ptr<Geometry> line_m =
        reader_.read("LINESTRING M (0 0 10, 6 8 20)");
    MinimumBoundingCircle mbc(line_m.get());

    std::unique_ptr<Geometry> diam = mbc.getDiameter();
    std::unique_ptr<Geometry> max_diam = mbc.getMaximumDiameter();
    EXPECT_FALSE(diam->hasZ());
    EXPECT_TRUE(diam->hasM());
    EXPECT_FALSE(max_diam->hasZ());
    EXPECT_TRUE(max_diam->hasM());
    ExpectLineEndpointsMatchUnordered(*max_diam, CoordinateXY(0, 0),
                                      CoordinateXY(6, 8));
  }
}

}  // namespace
}  // namespace geos_google
