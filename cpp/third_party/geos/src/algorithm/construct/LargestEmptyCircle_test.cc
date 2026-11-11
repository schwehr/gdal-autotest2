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

#include "third_party/geos/include/geos/algorithm/construct/LargestEmptyCircle.h"

#include <cmath>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/GEOSException.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/Interrupt.h"

namespace geos_google {
namespace {

using geos::algorithm::construct::LargestEmptyCircle;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::util::CurrentThreadInterrupt;
using geos::util::GEOSException;
using geos::util::IllegalArgumentException;
using ::testing::HasSubstr;

class LargestEmptyCircleTest : public ::testing::Test {
 protected:
  LargestEmptyCircleTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  // Verifies structural consistency of center, radiusPoint, radiusLine, and
  // boundary containment for a LargestEmptyCircle instance.
  static void VerifyCircleConsistency(LargestEmptyCircle& lec,
                                      const Geometry& obstacles,
                                      const Geometry& expected_boundary,
                                      double tolerance) {
    std::unique_ptr<Point> center = lec.getCenter();
    std::unique_ptr<Point> radius_pt = lec.getRadiusPoint();
    std::unique_ptr<LineString> radius_line = lec.getRadiusLine();

    ASSERT_NE(center, nullptr);
    ASSERT_NE(radius_pt, nullptr);
    ASSERT_NE(radius_line, nullptr);
    EXPECT_FALSE(center->isEmpty());
    EXPECT_FALSE(radius_pt->isEmpty());
    EXPECT_FALSE(radius_line->isEmpty());

    EXPECT_EQ(center->getFactory(), obstacles.getFactory());
    EXPECT_EQ(radius_pt->getFactory(), obstacles.getFactory());
    EXPECT_EQ(radius_line->getFactory(), obstacles.getFactory());

    // Outputs are 2D XY points (missing Z/M are NaN).
    EXPECT_FALSE(center->hasZ());
    EXPECT_FALSE(center->hasM());
    EXPECT_TRUE(std::isnan(center->getZ()));
    EXPECT_TRUE(std::isnan(center->getM()));

    ASSERT_EQ(radius_line->getNumPoints(), 2u);
    std::unique_ptr<Point> line_start = radius_line->getStartPoint();
    std::unique_ptr<Point> line_end = radius_line->getEndPoint();
    EXPECT_DOUBLE_EQ(line_start->getX(), center->getX());
    EXPECT_DOUBLE_EQ(line_start->getY(), center->getY());
    EXPECT_DOUBLE_EQ(line_end->getX(), radius_pt->getX());
    EXPECT_DOUBLE_EQ(line_end->getY(), radius_pt->getY());

    EXPECT_NEAR(radius_line->getLength(), center->distance(radius_pt.get()),
                1e-9);
    EXPECT_NEAR(obstacles.distance(radius_pt.get()), 0.0, 1e-9);

    if (expected_boundary.getDimension() >= 2) {
      EXPECT_TRUE(expected_boundary.covers(center.get()))
          << "Center (" << center->getX() << ", " << center->getY()
          << ") is not covered by boundary within tolerance " << tolerance;
    }
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(LargestEmptyCircleTest, EmptyObstaclesThrowsIllegalArgument) {
  const std::vector<std::string> empty_wkts = {
      "POINT EMPTY",      "LINESTRING EMPTY",         "POLYGON EMPTY",
      "MULTIPOINT EMPTY", "GEOMETRYCOLLECTION EMPTY",
  };

  auto boundary = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  for (const std::string& wkt : empty_wkts) {
    auto obstacles = ReadWkt(wkt);
    EXPECT_THROW(LargestEmptyCircle(obstacles.get(), 0.1),
                 IllegalArgumentException)
        << "for WKT: " << wkt;
    EXPECT_THROW(LargestEmptyCircle(obstacles.get(), boundary.get(), 0.1),
                 IllegalArgumentException)
        << "with explicit boundary for WKT: " << wkt;
  }
}

TEST_F(LargestEmptyCircleTest, AcuteTriangleOfPointsFindsCircumcenter) {
  // Vertices (0, 0), (10, 0), (5, 10) have circumcenter (5, 3.75) with radius
  // 6.25, which lies strictly inside the convex hull.
  auto obstacles = ReadWkt("MULTIPOINT ((0 0), (10 0), (5 10))");
  auto hull = obstacles->convexHull();
  constexpr double kTol = 1e-4;

  LargestEmptyCircle lec(obstacles.get(), kTol);
  VerifyCircleConsistency(lec, *obstacles, *hull, kTol);

  auto center = lec.getCenter();
  auto radius_line = lec.getRadiusLine();
  EXPECT_NEAR(center->getX(), 5.0, kTol);
  EXPECT_NEAR(center->getY(), 3.75, kTol);
  EXPECT_NEAR(radius_line->getLength(), 6.25, kTol);
  EXPECT_GE(radius_line->getLength(), 6.25 - kTol);
  EXPECT_LE(radius_line->getLength(), 6.25 + 1e-9);
  EXPECT_NEAR(obstacles->distance(center.get()), radius_line->getLength(),
              1e-9);
}

TEST_F(LargestEmptyCircleTest, StaticHelpersAndRepeatedCallsAreConsistent) {
  auto obstacles = ReadWkt("MULTIPOINT ((0 0), (10 0), (5 10))");
  constexpr double kTol = 1e-4;

  auto static_center = LargestEmptyCircle::getCenter(obstacles.get(), kTol);
  auto static_line = LargestEmptyCircle::getRadiusLine(obstacles.get(), kTol);

  LargestEmptyCircle lec(obstacles.get(), kTol);
  // Trigger compute() via getRadiusPoint() first, then call other getters to
  // exercise cached (`done == true`) fast paths.
  auto rp1 = lec.getRadiusPoint();
  auto c1 = lec.getCenter();
  auto rl1 = lec.getRadiusLine();
  auto c2 = lec.getCenter();
  auto rp2 = lec.getRadiusPoint();
  auto rl2 = lec.getRadiusLine();

  EXPECT_DOUBLE_EQ(c1->getX(), c2->getX());
  EXPECT_DOUBLE_EQ(c1->getY(), c2->getY());
  EXPECT_DOUBLE_EQ(rp1->getX(), rp2->getX());
  EXPECT_DOUBLE_EQ(rp1->getY(), rp2->getY());
  EXPECT_TRUE(rl1->equalsExact(rl2.get(), 0.0));

  EXPECT_DOUBLE_EQ(c1->getX(), static_center->getX());
  EXPECT_DOUBLE_EQ(c1->getY(), static_center->getY());
  EXPECT_TRUE(rl1->equalsExact(static_line.get(), 0.0));
}

TEST_F(LargestEmptyCircleTest, AsymmetricPointsUpdateFarthestCellFromCentroid) {
  // Square corners plus (2, 5): centroid is (4.4, 5.0) (distance 2.4 to (2,
  // 5)), whereas the true LEC center is uniquely (7.5625, 5.0) with
  // radius 5.5625.
  auto obstacles =
      ReadWkt("MULTIPOINT ((0 0), (10 0), (10 10), (0 10), (2 5))");
  auto hull = obstacles->convexHull();
  constexpr double kTol = 1e-4;

  LargestEmptyCircle lec(obstacles.get(), kTol);
  VerifyCircleConsistency(lec, *obstacles, *hull, kTol);

  auto center = lec.getCenter();
  auto radius_line = lec.getRadiusLine();
  EXPECT_NEAR(center->getX(), 7.5625, kTol);
  EXPECT_NEAR(center->getY(), 5.0, kTol);
  EXPECT_NEAR(radius_line->getLength(), 5.5625, kTol);
}

TEST_F(LargestEmptyCircleTest, NullAndEmptyBoundaryFallbackToConvexHull) {
  auto obstacles = ReadWkt("MULTIPOINT ((0 0), (10 0), (5 10))");
  auto empty_poly = ReadWkt("POLYGON EMPTY");
  auto empty_pt = ReadWkt("POINT EMPTY");
  constexpr double kTol = 1e-3;

  LargestEmptyCircle lec_null(obstacles.get(), nullptr, kTol);
  LargestEmptyCircle lec_empty_poly(obstacles.get(), empty_poly.get(), kTol);
  LargestEmptyCircle lec_empty_pt(obstacles.get(), empty_pt.get(), kTol);

  auto c_null = lec_null.getCenter();
  auto c_poly = lec_empty_poly.getCenter();
  auto c_pt = lec_empty_pt.getCenter();
  EXPECT_DOUBLE_EQ(c_null->getX(), c_poly->getX());
  EXPECT_DOUBLE_EQ(c_null->getY(), c_poly->getY());
  EXPECT_DOUBLE_EQ(c_null->getX(), c_pt->getX());
  EXPECT_DOUBLE_EQ(c_null->getY(), c_pt->getY());
}

TEST_F(LargestEmptyCircleTest,
       DegenerateNonPolygonalBoundariesReturnZeroRadius) {
  // Single point obstacle -> convex hull has dimension 0.
  {
    auto obs = ReadWkt("POINT (3 4)");
    LargestEmptyCircle lec(obs.get(), 0.01);
    auto center = lec.getCenter();
    auto rp = lec.getRadiusPoint();
    auto rl = lec.getRadiusLine();
    EXPECT_DOUBLE_EQ(center->getX(), 3.0);
    EXPECT_DOUBLE_EQ(center->getY(), 4.0);
    EXPECT_DOUBLE_EQ(rp->getX(), 3.0);
    EXPECT_DOUBLE_EQ(rp->getY(), 4.0);
    EXPECT_DOUBLE_EQ(rl->getLength(), 0.0);
  }

  // Two-point obstacle -> convex hull is a LineString (dimension 1).
  {
    auto obs = ReadWkt("MULTIPOINT ((1 2), (7 2))");
    LargestEmptyCircle lec(obs.get(), 0.01);
    auto center = lec.getCenter();
    auto rp = lec.getRadiusPoint();
    EXPECT_DOUBLE_EQ(center->getX(), 1.0);
    EXPECT_DOUBLE_EQ(center->getY(), 2.0);
    EXPECT_DOUBLE_EQ(rp->getX(), 1.0);
    EXPECT_DOUBLE_EQ(rp->getY(), 2.0);
  }

  // Explicit 1D LineString boundary with 2D-spanning obstacles.
  {
    auto obs = ReadWkt("MULTIPOINT ((2 3), (10 0), (5 10))");
    auto bnd = ReadWkt("LINESTRING (0 0, 10 10)");
    LargestEmptyCircle lec(obs.get(), bnd.get(), 0.01);
    auto center = lec.getCenter();
    auto rl = lec.getRadiusLine();
    EXPECT_DOUBLE_EQ(center->getX(), 2.0);
    EXPECT_DOUBLE_EQ(center->getY(), 3.0);
    EXPECT_DOUBLE_EQ(rl->getLength(), 0.0);
  }
}

TEST_F(LargestEmptyCircleTest, ConcaveLShapedBoundaryExercisesOutsideCells) {
  // Asymmetric L-shape: gridEnv is [0, 12] x [0, 10], so initial center (6, 5)
  // and top-right subcells lie outside the L-shape, exercising isOutside()
  // (both significant and insignificant overlap) and isFullyOutside().
  auto obstacles = ReadWkt("POINT (0 0)");
  auto boundary = ReadWkt("POLYGON ((0 0, 12 0, 12 2, 2 2, 2 10, 0 10, 0 0))");
  constexpr double kTol = 1e-3;

  LargestEmptyCircle lec(obstacles.get(), boundary.get(), kTol);
  VerifyCircleConsistency(lec, *obstacles, *boundary, kTol);

  auto center = lec.getCenter();
  auto radius_pt = lec.getRadiusPoint();
  auto radius_line = lec.getRadiusLine();
  const double expected_radius = std::sqrt(148.0);
  EXPECT_NEAR(center->getX(), 12.0, kTol * 4.0);
  EXPECT_NEAR(center->getY(), 2.0, kTol * 4.0);
  EXPECT_DOUBLE_EQ(radius_pt->getX(), 0.0);
  EXPECT_DOUBLE_EQ(radius_pt->getY(), 0.0);
  EXPECT_NEAR(radius_line->getLength(), expected_radius, kTol * 4.0);
}

TEST_F(LargestEmptyCircleTest, BoundaryWithHoleAvoidsHoleInterior) {
  // Outer [0, 20]^2 with a central hole [6, 14]^2 and a corner obstacle (2, 2).
  // The obstacle centroid (2, 2) is inside, while (10, 10) lies in the hole.
  // The farthest point in the holed boundary is (20, 20) at distance
  // 18*sqrt(2).
  auto obstacles = ReadWkt("POINT (2 2)");
  auto boundary = ReadWkt(
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
      "(6 6, 14 6, 14 14, 6 14, 6 6))");
  constexpr double kTol = 1e-3;

  LargestEmptyCircle lec(obstacles.get(), boundary.get(), kTol);
  VerifyCircleConsistency(lec, *obstacles, *boundary, kTol);

  auto center = lec.getCenter();
  auto radius_line = lec.getRadiusLine();
  const double expected_radius = 18.0 * std::sqrt(2.0);
  EXPECT_NEAR(center->getX(), 20.0, kTol * 4.0);
  EXPECT_NEAR(center->getY(), 20.0, kTol * 4.0);
  EXPECT_NEAR(radius_line->getLength(), expected_radius, kTol * 4.0);

  // Also test when obstacle centroid itself falls inside the hole (exterior to
  // boundary), so initial farthestCell has negative distance.
  auto corner_obstacles =
      ReadWkt("MULTIPOINT ((0 0), (20 0), (20 20), (0 20))");
  LargestEmptyCircle lec_hole_centroid(corner_obstacles.get(), boundary.get(),
                                       kTol);
  VerifyCircleConsistency(lec_hole_centroid, *corner_obstacles, *boundary,
                          kTol);
  EXPECT_GE(lec_hole_centroid.getRadiusLine()->getLength(),
            std::sqrt(136.0) - kTol * 4.0);
}

TEST_F(LargestEmptyCircleTest,
       BoundaryIncludedAsObstacleConstrainsCircleWhollyInside) {
  // Include the [0, 20]^2 boundary outline as an obstacle alongside (5, 8).
  // Unique maximum inscribed empty circle is tangent to x=20, y=20, and (5, 8):
  // r = 27 - 6*sqrt(10), center = (20 - r, 20 - r).
  auto obstacles = ReadWkt(
      "GEOMETRYCOLLECTION ("
      "  LINESTRING (0 0, 20 0, 20 20, 0 20, 0 0),"
      "  POINT (5 8)"
      ")");
  auto boundary = ReadWkt("POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0))");
  constexpr double kTol = 1e-4;

  LargestEmptyCircle lec(obstacles.get(), boundary.get(), kTol);
  VerifyCircleConsistency(lec, *obstacles, *boundary, kTol);

  const double expected_r = 27.0 - 6.0 * std::sqrt(10.0);
  const double expected_xy = 20.0 - expected_r;
  auto center = lec.getCenter();
  auto radius_line = lec.getRadiusLine();

  EXPECT_TRUE(boundary->contains(center.get()));
  EXPECT_NEAR(center->getX(), expected_xy, kTol * 5.0);
  EXPECT_NEAR(center->getY(), expected_xy, kTol * 5.0);
  EXPECT_NEAR(radius_line->getLength(), expected_r, kTol);
  EXPECT_NEAR(obstacles->distance(center.get()), radius_line->getLength(),
              1e-9);
}

TEST_F(LargestEmptyCircleTest, MultiLineStringAndPolygonAndMixedObstacles) {
  // Parallel horizontal segments -> convex hull is [0, 10] x [0, 4], max
  // distance from both segments is 2.0 along y = 2.0.
  {
    auto obstacles = ReadWkt("MULTILINESTRING ((0 0, 10 0), (0 4, 10 4))");
    auto hull = obstacles->convexHull();
    constexpr double kTol = 1e-3;
    LargestEmptyCircle lec(obstacles.get(), kTol);
    VerifyCircleConsistency(lec, *obstacles, *hull, kTol);
    auto center = lec.getCenter();
    EXPECT_NEAR(center->getY(), 2.0, kTol);
    EXPECT_NEAR(lec.getRadiusLine()->getLength(), 2.0, kTol);
  }

  // Polygon obstacle inside a larger square boundary: farthest point from
  // [0, 2]^2 inside [0, 10]^2 is (10, 10) with radius 8*sqrt(2).
  {
    auto obstacles = ReadWkt("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
    auto boundary = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
    constexpr double kTol = 1e-3;
    LargestEmptyCircle lec(obstacles.get(), boundary.get(), kTol);
    VerifyCircleConsistency(lec, *obstacles, *boundary, kTol);
    auto center = lec.getCenter();
    EXPECT_NEAR(center->getX(), 10.0, kTol * 4.0);
    EXPECT_NEAR(center->getY(), 10.0, kTol * 4.0);
    EXPECT_NEAR(lec.getRadiusLine()->getLength(), 8.0 * std::sqrt(2.0),
                kTol * 4.0);
  }

  // Boundary completely covered by an obstacle polygon -> zero empty space.
  {
    auto obstacles = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
    auto boundary = ReadWkt("POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))");
    constexpr double kTol = 1e-2;
    LargestEmptyCircle lec(obstacles.get(), boundary.get(), kTol);
    VerifyCircleConsistency(lec, *obstacles, *boundary, kTol);
    EXPECT_DOUBLE_EQ(obstacles->distance(lec.getCenter().get()), 0.0);
  }

  // Mixed GeometryCollection (Point + LineString + Polygon).
  {
    auto obstacles = ReadWkt(
        "GEOMETRYCOLLECTION ("
        "  POINT (0 0),"
        "  LINESTRING (10 0, 10 10),"
        "  POLYGON ((0 8, 2 8, 2 10, 0 10, 0 8))"
        ")");
    auto hull = obstacles->convexHull();
    constexpr double kTol = 1e-3;
    LargestEmptyCircle lec(obstacles.get(), kTol);
    VerifyCircleConsistency(lec, *obstacles, *hull, kTol);
    EXPECT_GT(lec.getRadiusLine()->getLength(), 4.0);
  }
}

TEST_F(LargestEmptyCircleTest, MultiPolygonBoundaryAndNegativeCoordinates) {
  // MultiPolygon boundary with two disjoint squares: LEC resides in the square
  // farther from the obstacle point (2, 5).
  {
    auto obstacles = ReadWkt("POINT (2 5)");
    auto boundary = ReadWkt(
        "MULTIPOLYGON ("
        "  ((0 0, 10 0, 10 10, 0 10, 0 0)),"
        "  ((20 0, 30 0, 30 10, 20 10, 20 0))"
        ")");
    constexpr double kTol = 1e-3;
    LargestEmptyCircle lec(obstacles.get(), boundary.get(), kTol);
    VerifyCircleConsistency(lec, *obstacles, *boundary, kTol);
    auto center = lec.getCenter();
    EXPECT_NEAR(center->getX(), 30.0, kTol * 4.0);
    EXPECT_NEAR(lec.getRadiusLine()->getLength(), std::sqrt(809.0), kTol * 4.0);
  }

  // Negative XYZ coordinates: circumcenter of (-10,-10), (10,-10), (0,10) is
  // (0, -2.5) with radius 12.5, and output coordinates strip Z.
  {
    auto obstacles =
        ReadWkt("MULTIPOINT Z ((-10 -10 5), (10 -10 7), (0 10 9))");
    auto hull = obstacles->convexHull();
    constexpr double kTol = 1e-4;
    LargestEmptyCircle lec(obstacles.get(), kTol);
    VerifyCircleConsistency(lec, *obstacles, *hull, kTol);
    auto center = lec.getCenter();
    EXPECT_NEAR(center->getX(), 0.0, kTol);
    EXPECT_NEAR(center->getY(), -2.5, kTol);
    EXPECT_NEAR(lec.getRadiusLine()->getLength(), 12.5, kTol);
  }
}

TEST_F(LargestEmptyCircleTest,
       LargeFlatPlateauHitsMaxIterationsAndInterruptCheck) {
  // All points on y = 100 in [0, 100] x [0, 100] have equal distance 100 to
  // LINESTRING (0 0, 100 0), causing exponential tie refinement along y = 100
  // until iterationCount reaches maxIter (> 1000 checks
  // GEOS_CHECK_FOR_INTERRUPTS).
  auto obstacles = ReadWkt("LINESTRING (0 0, 100 0)");
  auto boundary = ReadWkt("POLYGON ((0 0, 100 0, 100 100, 0 100, 0 0))");
  constexpr double kTol = 1e-3;

  LargestEmptyCircle lec(obstacles.get(), boundary.get(), kTol);
  VerifyCircleConsistency(lec, *obstacles, *boundary, kTol);

  auto center = lec.getCenter();
  auto radius_line = lec.getRadiusLine();
  EXPECT_GE(center->getX(), 0.0);
  EXPECT_LE(center->getX(), 100.0);
  EXPECT_NEAR(center->getY(), 100.0, 0.05);
  EXPECT_NEAR(radius_line->getLength(), 100.0, 0.05);
}

TEST_F(LargestEmptyCircleTest, InterruptCallbackAbortsComputation) {
  struct InterruptState {
    int calls = 0;
    int trigger_on_call = 1;
  } state;

  auto* prev_cb = CurrentThreadInterrupt::registerCallback(
      [](void* data) -> int {
        auto* s = static_cast<InterruptState*>(data);
        return (++s->calls >= s->trigger_on_call) ? 1 : 0;
      },
      &state);
  absl::Cleanup restore_cb([prev_cb]() {
    CurrentThreadInterrupt::registerCallback(prev_cb, nullptr);
  });

  auto obstacles = ReadWkt("LINESTRING (0 0, 100 0)");
  auto boundary = ReadWkt("POLYGON ((0 0, 100 0, 100 100, 0 100, 0 0))");
  LargestEmptyCircle lec(obstacles.get(), boundary.get(), 1e-3);

  EXPECT_THROW(lec.getCenter(), GEOSException);
  EXPECT_EQ(state.calls, 1);

  // Also test interrupting after the initial iterationCount==0 check (at
  // iteration 1000).
  state.calls = 0;
  state.trigger_on_call = 2;
  LargestEmptyCircle lec2(obstacles.get(), boundary.get(), 1e-3);
  EXPECT_THROW(lec2.getCenter(), GEOSException);
  EXPECT_EQ(state.calls, 2);
}

TEST_F(LargestEmptyCircleTest, NonFiniteEnvelopeThrowsGEOSException) {
  // Coordinates are finite (1e200), but envelope area 1e200 * 1e200 overflows
  // to +inf, making Envelope::isFinite() return false in createInitialGrid().
  CoordinateSequence coords(std::initializer_list<CoordinateXY>{
      {0.0, 0.0},
      {1e200, 0.0},
      {1e200, 1e200},
      {0.0, 1e200},
      {0.0, 0.0},
  });
  std::unique_ptr<LinearRing> ring = factory_->createLinearRing(coords);
  std::unique_ptr<Polygon> huge_boundary =
      factory_->createPolygon(std::move(ring));
  auto obstacles = ReadWkt("POINT (1 1)");

  LargestEmptyCircle lec(obstacles.get(), huge_boundary.get(), 1.0);
  EXPECT_THROW(
      {
        try {
          lec.getCenter();
        } catch (const GEOSException& e) {
          EXPECT_THAT(e.what(), HasSubstr("Non-finite envelope"));
          throw;
        }
      },
      GEOSException);
}

TEST_F(LargestEmptyCircleTest, PreservesCustomGeometryFactoryAndSrid) {
  PrecisionModel pm(PrecisionModel::FLOATING);
  GeometryFactory::Ptr custom_factory = GeometryFactory::create(&pm, 4326);
  WKTReader custom_reader(*custom_factory);

  auto obstacles = custom_reader.read("MULTIPOINT ((0 0), (10 0), (5 10))");
  LargestEmptyCircle lec(obstacles.get(), 1e-3);

  auto center = lec.getCenter();
  auto radius_pt = lec.getRadiusPoint();
  auto radius_line = lec.getRadiusLine();

  EXPECT_EQ(center->getFactory(), custom_factory.get());
  EXPECT_EQ(radius_pt->getFactory(), custom_factory.get());
  EXPECT_EQ(radius_line->getFactory(), custom_factory.get());
  EXPECT_EQ(center->getSRID(), 4326);
  EXPECT_EQ(radius_pt->getSRID(), 4326);
  EXPECT_EQ(radius_line->getSRID(), 4326);
}

}  // namespace
}  // namespace geos_google
