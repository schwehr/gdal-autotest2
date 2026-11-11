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

#include "third_party/geos/include/geos/algorithm/hull/ConcaveHull.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::algorithm::hull::ConcaveHull;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::MultiPoint;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::io::WKTReader;

class ConcaveHullTest : public ::testing::Test {
 protected:
  ConcaveHullTest() : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> ReadWKT(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  std::unique_ptr<MultiPoint> MakeMultiPoint(
      const std::vector<CoordinateXY>& coords) const {
    return factory_->createMultiPoint(coords);
  }

  std::unique_ptr<Point> MakePoint(double x, double y) const {
    return factory_->createPoint(CoordinateXY(x, y));
  }

  // Dense integer grid U-shape inside [0, 10] x [0, 10]:
  // bottom bar y <= 2, left arm x <= 2, right arm x >= 8.
  std::unique_ptr<MultiPoint> MakeUShapePoints() const {
    std::vector<CoordinateXY> coords;
    for (int x = 0; x <= 10; ++x) {
      for (int y = 0; y <= 10; ++y) {
        if (y <= 2 || x <= 2 || x >= 8) {
          coords.emplace_back(static_cast<double>(x), static_cast<double>(y));
        }
      }
    }
    return MakeMultiPoint(coords);
  }

  // Thick [0, 10] x [0, 10] frame leaving an empty [2, 8] x [2, 8] interior.
  std::unique_ptr<MultiPoint> MakeSingleHoleFramePoints() const {
    std::vector<CoordinateXY> coords;
    for (int x = 0; x <= 10; ++x) {
      for (int y = 0; y <= 10; ++y) {
        if (x >= 3 && x <= 7 && y >= 3 && y <= 7) {
          continue;
        }
        coords.emplace_back(static_cast<double>(x), static_cast<double>(y));
      }
    }
    return MakeMultiPoint(coords);
  }

  // Thick [0, 18] x [0, 10] frame with two separate empty interior chambers.
  std::unique_ptr<MultiPoint> MakeDoubleHoleFramePoints() const {
    std::vector<CoordinateXY> coords;
    for (int x = 0; x <= 18; ++x) {
      for (int y = 0; y <= 10; ++y) {
        const bool in_hole1 = (x >= 3 && x <= 6 && y >= 3 && y <= 7);
        const bool in_hole2 = (x >= 12 && x <= 15 && y >= 3 && y <= 7);
        if (!in_hole1 && !in_hole2) {
          coords.emplace_back(static_cast<double>(x), static_cast<double>(y));
        }
      }
    }
    return MakeMultiPoint(coords);
  }

  // Thick [0, 14] x [0, 14] frame with a U-shaped empty interior cavity
  // wrapping around a central solid peninsula at x in [6, 8], y in [3, 8].
  std::unique_ptr<MultiPoint> MakeUShapedHoleFramePoints() const {
    std::vector<CoordinateXY> coords;
    for (int x = 0; x <= 14; ++x) {
      for (int y = 0; y <= 14; ++y) {
        const bool in_left_shaft = (x >= 3 && x <= 5 && y >= 3 && y <= 11);
        const bool in_right_shaft = (x >= 9 && x <= 11 && y >= 3 && y <= 11);
        const bool in_top_bar = (x >= 3 && x <= 11 && y >= 9 && y <= 11);
        if (!in_left_shaft && !in_right_shaft && !in_top_bar) {
          coords.emplace_back(static_cast<double>(x), static_cast<double>(y));
        }
      }
    }
    return MakeMultiPoint(coords);
  }

  // Thick [0, 11] x [0, 10] frame with two interior cavities separated by
  // a single-column wall of points at x = 5.
  std::unique_ptr<MultiPoint> MakeAdjacentHolesThinWallPoints() const {
    std::vector<CoordinateXY> coords;
    for (int x = 0; x <= 11; ++x) {
      for (int y = 0; y <= 10; ++y) {
        const bool in_left_hole = (x >= 3 && x <= 4 && y >= 3 && y <= 7);
        const bool in_right_hole = (x >= 6 && x <= 8 && y >= 3 && y <= 7);
        if (!in_left_hole && !in_right_hole) {
          coords.emplace_back(static_cast<double>(x), static_cast<double>(y));
        }
      }
    }
    return MakeMultiPoint(coords);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(ConcaveHullTest, EmptyGeometriesReturnEmptyPolygon) {
  const char* const empty_wkts[] = {
      "POINT EMPTY",      "LINESTRING EMPTY",         "POLYGON EMPTY",
      "MULTIPOINT EMPTY", "GEOMETRYCOLLECTION EMPTY",
  };

  for (const char* wkt : empty_wkts) {
    SCOPED_TRACE(wkt);
    std::unique_ptr<Geometry> geom = ReadWKT(wkt);

    std::unique_ptr<Geometry> hull_len =
        ConcaveHull::concaveHullByLength(geom.get(), 1.0);
    EXPECT_TRUE(hull_len->isEmpty());
    EXPECT_EQ(hull_len->getGeometryTypeId(), geos::geom::GEOS_POLYGON);

    std::unique_ptr<Geometry> hull_ratio =
        ConcaveHull::concaveHullByLengthRatio(geom.get(), 0.5);
    EXPECT_TRUE(hull_ratio->isEmpty());
    EXPECT_EQ(hull_ratio->getGeometryTypeId(), geos::geom::GEOS_POLYGON);

    std::unique_ptr<Geometry> hull_alpha =
        ConcaveHull::alphaShape(geom.get(), 1.0, false);
    EXPECT_TRUE(hull_alpha->isEmpty());
    EXPECT_EQ(hull_alpha->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  }
}

TEST_F(ConcaveHullTest, CurvedGeometryThrowsUnsupportedOperationException) {
  std::unique_ptr<Geometry> arc = ReadWKT("CIRCULARSTRING (0 0, 1 1, 2 0)");
  EXPECT_THROW(ConcaveHull(arc.get()),
               geos::util::UnsupportedOperationException);
  EXPECT_THROW(ConcaveHull::concaveHullByLength(arc.get(), 1.0),
               geos::util::UnsupportedOperationException);

  std::unique_ptr<Geometry> compound =
      ReadWKT("COMPOUNDCURVE (CIRCULARSTRING (0 0, 1 1, 2 0), (2 0, 3 0))");
  EXPECT_THROW(ConcaveHull(compound.get()),
               geos::util::UnsupportedOperationException);
}

TEST_F(ConcaveHullTest, InvalidParametersThrowIllegalArgumentException) {
  std::unique_ptr<Geometry> square =
      ReadWKT("MULTIPOINT ((0 0), (10 0), (10 10), (0 10))");

  ConcaveHull hull(square.get());
  EXPECT_THROW(hull.setMaximumEdgeLength(-0.001),
               geos::util::IllegalArgumentException);
  EXPECT_THROW(ConcaveHull::concaveHullByLength(square.get(), -1.0),
               geos::util::IllegalArgumentException);

  EXPECT_THROW(hull.setMaximumEdgeLengthRatio(-0.01),
               geos::util::IllegalArgumentException);
  EXPECT_THROW(hull.setMaximumEdgeLengthRatio(1.01),
               geos::util::IllegalArgumentException);
  EXPECT_THROW(ConcaveHull::concaveHullByLengthRatio(square.get(), -0.5),
               geos::util::IllegalArgumentException);
  EXPECT_THROW(ConcaveHull::concaveHullByLengthRatio(square.get(), 1.5),
               geos::util::IllegalArgumentException);

  // Boundary values 0.0 and 1.0 must succeed.
  EXPECT_NO_THROW(hull.setMaximumEdgeLength(0.0));
  EXPECT_NO_THROW(hull.setMaximumEdgeLengthRatio(0.0));
  EXPECT_NO_THROW(hull.setMaximumEdgeLengthRatio(1.0));
}

TEST_F(ConcaveHullTest, DegenerateTriangulationsReturnConvexHull) {
  // Single point.
  {
    std::unique_ptr<Geometry> pt = ReadWKT("POINT (3 4)");
    std::unique_ptr<Geometry> hull =
        ConcaveHull::concaveHullByLength(pt.get(), 1.0);
    EXPECT_EQ(hull->getGeometryTypeId(), geos::geom::GEOS_POINT);
    EXPECT_TRUE(hull->equalsExact(pt.get(), 1e-12));

    std::unique_ptr<Geometry> hull_ratio =
        ConcaveHull::concaveHullByLengthRatio(pt.get(), 0.5);
    EXPECT_TRUE(hull_ratio->equalsExact(pt.get(), 1e-12));

    std::unique_ptr<Geometry> hull_alpha =
        ConcaveHull::alphaShape(pt.get(), 1.0, false);
    EXPECT_TRUE(hull_alpha->equalsExact(pt.get(), 1e-12));
  }

  // Two points.
  {
    std::unique_ptr<Geometry> two_pts = ReadWKT("MULTIPOINT ((0 0), (5 0))");
    std::unique_ptr<Geometry> expected = ReadWKT("LINESTRING (0 0, 5 0)");

    std::unique_ptr<Geometry> hull =
        ConcaveHull::concaveHullByLength(two_pts.get(), 1.0);
    EXPECT_EQ(hull->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
    EXPECT_TRUE(hull->equals(expected.get()));
  }

  // Collinear 4 points produce empty Delaunay triangle list -> convexHull().
  {
    std::unique_ptr<Geometry> collinear =
        ReadWKT("MULTIPOINT ((0 0), (1 0), (3 0), (6 0))");
    std::unique_ptr<Geometry> expected = ReadWKT("LINESTRING (0 0, 6 0)");

    std::unique_ptr<Geometry> hull_len =
        ConcaveHull::concaveHullByLength(collinear.get(), 0.5);
    EXPECT_EQ(hull_len->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
    EXPECT_TRUE(hull_len->equals(expected.get()));

    std::unique_ptr<Geometry> hull_ratio =
        ConcaveHull::concaveHullByLengthRatio(collinear.get(), 0.5);
    EXPECT_TRUE(hull_ratio->equals(expected.get()));

    std::unique_ptr<Geometry> hull_alpha =
        ConcaveHull::alphaShape(collinear.get(), 0.5, false);
    EXPECT_TRUE(hull_alpha->equals(expected.get()));
  }
}

TEST_F(ConcaveHullTest, SmallPointSetsAndQueuedTriangleBecomingNonRemovable) {
  // Single triangle (3 points): numAdjacent() == 0, triList.size() == 1.
  {
    std::unique_ptr<Geometry> tri =
        ReadWKT("MULTIPOINT ((0 0), (10 0), (5 8))");
    std::unique_ptr<Geometry> hull =
        ConcaveHull::concaveHullByLength(tri.get(), 0.0);
    EXPECT_EQ(hull->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_DOUBLE_EQ(hull->getArea(), 40.0);
  }

  // Four points forming a square: 2 tris each with numAdjacent() == 1.
  {
    std::unique_ptr<Geometry> square =
        ReadWKT("MULTIPOINT ((0 0), (10 0), (10 10), (0 10))");
    std::unique_ptr<Geometry> hull =
        ConcaveHull::concaveHullByLength(square.get(), 0.0);
    EXPECT_EQ(hull->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_DOUBLE_EQ(hull->getArea(), 100.0);
  }

  // Triangle plus one interior point: 3 tris all initially with
  // numAdjacent() == 2; removing the largest leaves the other two in the queue
  // with numAdjacent() == 1 (exercising isRemovableBorder's numAdjacent != 2).
  {
    std::unique_ptr<Geometry> fan =
        ReadWKT("MULTIPOINT ((0 0), (10 0), (5 10), (5 3))");
    std::unique_ptr<Geometry> hull =
        ConcaveHull::concaveHullByLength(fan.get(), 0.0);
    EXPECT_EQ(hull->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(hull->isValid());
    EXPECT_GT(hull->getArea(), 0.0);
    EXPECT_LT(hull->getArea(), 50.0);
  }
}

TEST_F(ConcaveHullTest, UniformEdgeLengthComputation) {
  std::unique_ptr<Geometry> square =
      ReadWKT("MULTIPOINT ((0 0), (10 0), (10 10), (0 10))");
  // areaCH = 100, numPts = 4 -> sqrt(100 / 4) = 5.0
  EXPECT_DOUBLE_EQ(ConcaveHull::uniformEdgeLength(square.get()), 5.0);

  std::vector<CoordinateXY> grid_pts;
  for (int x = 0; x < 5; ++x) {
    for (int y = 0; y < 5; ++y) {
      grid_pts.emplace_back(static_cast<double>(x), static_cast<double>(y));
    }
  }
  std::unique_ptr<MultiPoint> grid = MakeMultiPoint(grid_pts);
  // areaCH = 16, numPts = 25 -> sqrt(16 / 25) = 0.8
  EXPECT_DOUBLE_EQ(ConcaveHull::uniformEdgeLength(grid.get()), 0.8);

  std::unique_ptr<Geometry> line = ReadWKT("LINESTRING (0 0, 5 0, 10 0)");
  EXPECT_DOUBLE_EQ(ConcaveHull::uniformEdgeLength(line.get()), 0.0);
}

TEST_F(ConcaveHullTest, ConcaveHullByLengthOnUShape) {
  std::unique_ptr<MultiPoint> u_shape = MakeUShapePoints();

  // Large maxLength preserves the entire convex hull.
  {
    std::unique_ptr<Geometry> convex =
        ConcaveHull::concaveHullByLength(u_shape.get(), 100.0);
    EXPECT_EQ(convex->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(convex->isValid());
    EXPECT_DOUBLE_EQ(convex->getArea(), 100.0);
  }

  // maxLength = 2.0 erodes the wide open cavity between the arms.
  {
    std::unique_ptr<Geometry> concave =
        ConcaveHull::concaveHullByLength(u_shape.get(), 2.0);
    EXPECT_EQ(concave->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(concave->isValid());
    EXPECT_GE(concave->getArea(), 51.9);
    EXPECT_LE(concave->getArea(), 54.0);

    std::unique_ptr<Point> gap_pt = MakePoint(5.0, 6.0);
    std::unique_ptr<Point> bottom_pt = MakePoint(5.0, 1.0);
    std::unique_ptr<Point> left_arm_pt = MakePoint(1.0, 7.0);
    std::unique_ptr<Point> right_arm_pt = MakePoint(9.0, 7.0);
    EXPECT_FALSE(concave->intersects(gap_pt.get()));
    EXPECT_TRUE(concave->covers(bottom_pt.get()));
    EXPECT_TRUE(concave->covers(left_arm_pt.get()));
    EXPECT_TRUE(concave->covers(right_arm_pt.get()));
  }

  // Explicit 3-arg overload with holesAllowed = true (no holes in U-shape).
  {
    std::unique_ptr<Geometry> concave_holes_flag =
        ConcaveHull::concaveHullByLength(u_shape.get(), 2.0, true);
    EXPECT_EQ(concave_holes_flag->getGeometryTypeId(),
              geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(concave_holes_flag->isValid());
    EXPECT_GE(concave_holes_flag->getArea(), 51.9);
    EXPECT_LE(concave_holes_flag->getArea(), 54.0);
  }

  // maxLength = 0.0 erodes maximally while remaining a single connected
  // polygon.
  {
    std::unique_ptr<Geometry> min_hull =
        ConcaveHull::concaveHullByLength(u_shape.get(), 0.0);
    EXPECT_EQ(min_hull->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(min_hull->isValid());
    EXPECT_GT(min_hull->getArea(), 0.0);
    EXPECT_LE(min_hull->getArea(), 54.0);
  }
}

TEST_F(ConcaveHullTest, ConcaveHullByLengthRatioOnUShape) {
  std::unique_ptr<MultiPoint> u_shape = MakeUShapePoints();

  // Ratio 1.0 produces the convex hull.
  {
    std::unique_ptr<Geometry> convex =
        ConcaveHull::concaveHullByLengthRatio(u_shape.get(), 1.0);
    EXPECT_EQ(convex->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(convex->isValid());
    EXPECT_DOUBLE_EQ(convex->getArea(), 100.0);
  }

  // Intermediate ratio 0.15 carves out the concave cavity.
  {
    std::unique_ptr<Geometry> concave =
        ConcaveHull::concaveHullByLengthRatio(u_shape.get(), 0.15);
    EXPECT_EQ(concave->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(concave->isValid());
    EXPECT_GE(concave->getArea(), 51.9);
    EXPECT_LE(concave->getArea(), 54.0);

    std::unique_ptr<Point> gap_pt = MakePoint(5.0, 6.0);
    EXPECT_FALSE(concave->intersects(gap_pt.get()));
  }

  // Ratio 0.0 sets target length to 0 and drains the entire border queue.
  {
    std::unique_ptr<Geometry> min_hull =
        ConcaveHull::concaveHullByLengthRatio(u_shape.get(), 0.0);
    EXPECT_EQ(min_hull->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(min_hull->isValid());
    EXPECT_GT(min_hull->getArea(), 0.0);
    EXPECT_LE(min_hull->getArea(), 54.0);
  }
}

TEST_F(ConcaveHullTest, AlphaShapeOnUShape) {
  std::unique_ptr<MultiPoint> u_shape = MakeUShapePoints();

  // Large alpha returns convex hull.
  {
    std::unique_ptr<Geometry> convex =
        ConcaveHull::alphaShape(u_shape.get(), 100.0, false);
    EXPECT_EQ(convex->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(convex->isValid());
    EXPECT_DOUBLE_EQ(convex->getArea(), 100.0);
  }

  // alpha = 1.5 exceeds grid triangle circumradius (~0.707) but erodes gap.
  {
    std::unique_ptr<Geometry> alpha_hull =
        ConcaveHull::alphaShape(u_shape.get(), 1.5, false);
    EXPECT_EQ(alpha_hull->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(alpha_hull->isValid());
    EXPECT_GE(alpha_hull->getArea(), 51.9);
    EXPECT_LE(alpha_hull->getArea(), 54.0);

    std::unique_ptr<Point> gap_pt = MakePoint(5.0, 6.0);
    EXPECT_FALSE(alpha_hull->intersects(gap_pt.get()));
  }

  // Zero and negative alpha erode maximally without throwing.
  {
    std::unique_ptr<Geometry> zero_alpha =
        ConcaveHull::alphaShape(u_shape.get(), 0.0, false);
    EXPECT_EQ(zero_alpha->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(zero_alpha->isValid());
    EXPECT_GT(zero_alpha->getArea(), 0.0);

    std::unique_ptr<Geometry> neg_alpha =
        ConcaveHull::alphaShape(u_shape.get(), -1.0, false);
    EXPECT_EQ(neg_alpha->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(neg_alpha->isValid());
    EXPECT_GT(neg_alpha->getArea(), 0.0);
  }
}

TEST_F(ConcaveHullTest, HolesAllowedSingleAndMultipleHoles) {
  std::unique_ptr<MultiPoint> frame = MakeSingleHoleFramePoints();

  // Without holes allowed, the frame collapses to the outer convex hull.
  {
    std::unique_ptr<Geometry> no_holes =
        ConcaveHull::concaveHullByLength(frame.get(), 2.0, false);
    const auto* poly = dynamic_cast<const Polygon*>(no_holes.get());
    ASSERT_NE(poly, nullptr);
    EXPECT_EQ(poly->getNumInteriorRing(), 0u);
    EXPECT_DOUBLE_EQ(poly->getArea(), 100.0);
  }

  // With holes allowed (by length, length ratio, and alphaShape), 1 hole forms.
  {
    std::unique_ptr<Geometry> with_hole_len =
        ConcaveHull::concaveHullByLength(frame.get(), 2.0, true);
    ASSERT_TRUE(with_hole_len->isValid());
    const auto* poly = dynamic_cast<const Polygon*>(with_hole_len.get());
    ASSERT_NE(poly, nullptr);
    EXPECT_EQ(poly->getNumInteriorRing(), 1u);
    EXPECT_GE(poly->getArea(), 63.0);
    EXPECT_LE(poly->getArea(), 68.0);

    std::unique_ptr<Point> center_pt = MakePoint(5.0, 5.0);
    std::unique_ptr<Point> frame_pt = MakePoint(1.0, 5.0);
    EXPECT_FALSE(poly->intersects(center_pt.get()));
    EXPECT_TRUE(poly->covers(frame_pt.get()));
  }

  {
    std::unique_ptr<Geometry> with_hole_ratio =
        ConcaveHull::concaveHullByLengthRatio(frame.get(), 0.15, true);
    ASSERT_TRUE(with_hole_ratio->isValid());
    const auto* poly = dynamic_cast<const Polygon*>(with_hole_ratio.get());
    ASSERT_NE(poly, nullptr);
    EXPECT_EQ(poly->getNumInteriorRing(), 1u);
    EXPECT_GE(poly->getArea(), 63.0);
    EXPECT_LE(poly->getArea(), 68.0);
  }

  {
    std::unique_ptr<Geometry> with_hole_alpha =
        ConcaveHull::alphaShape(frame.get(), 1.5, true);
    ASSERT_TRUE(with_hole_alpha->isValid());
    const auto* poly = dynamic_cast<const Polygon*>(with_hole_alpha.get());
    ASSERT_NE(poly, nullptr);
    EXPECT_EQ(poly->getNumInteriorRing(), 1u);
    EXPECT_GE(poly->getArea(), 63.0);
    EXPECT_LE(poly->getArea(), 68.0);
  }

  // Two disjoint interior holes.
  {
    std::unique_ptr<MultiPoint> double_frame = MakeDoubleHoleFramePoints();
    std::unique_ptr<Geometry> two_holes =
        ConcaveHull::concaveHullByLength(double_frame.get(), 2.0, true);
    ASSERT_TRUE(two_holes->isValid());
    const auto* poly = dynamic_cast<const Polygon*>(two_holes.get());
    ASSERT_NE(poly, nullptr);
    EXPECT_EQ(poly->getNumInteriorRing(), 2u);
    EXPECT_LT(poly->getArea(), 140.0);

    std::unique_ptr<Point> hole1_center = MakePoint(4.5, 5.0);
    std::unique_ptr<Point> hole2_center = MakePoint(13.5, 5.0);
    std::unique_ptr<Point> bridge_pt = MakePoint(9.0, 5.0);
    EXPECT_FALSE(poly->intersects(hole1_center.get()));
    EXPECT_FALSE(poly->intersects(hole2_center.get()));
    EXPECT_TRUE(poly->covers(bridge_pt.get()));
  }
}

TEST_F(ConcaveHullTest, ComplexHolesWithBoundaryTouchAndPinch) {
  // U-shaped cavity exercises wrapping hole erosion, boundary touches, and
  // already-removed/border candidate filtering in computeHullHoles.
  {
    std::unique_ptr<MultiPoint> u_hole = MakeUShapedHoleFramePoints();
    std::unique_ptr<Geometry> hull =
        ConcaveHull::concaveHullByLength(u_hole.get(), 2.0, true);
    ASSERT_TRUE(hull->isValid());
    const auto* poly = dynamic_cast<const Polygon*>(hull.get());
    ASSERT_NE(poly, nullptr);
    EXPECT_GE(poly->getNumInteriorRing(), 1u);
    EXPECT_LT(poly->getArea(), 196.0);

    std::unique_ptr<Point> peninsula_pt = MakePoint(7.0, 5.0);
    std::unique_ptr<Point> cavity_top_pt = MakePoint(7.0, 10.0);
    EXPECT_TRUE(poly->covers(peninsula_pt.get()));
    EXPECT_FALSE(poly->intersects(cavity_top_pt.get()));

    std::unique_ptr<Geometry> alpha_hull =
        ConcaveHull::alphaShape(u_hole.get(), 1.5, true);
    ASSERT_TRUE(alpha_hull->isValid());
    EXPECT_EQ(alpha_hull->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_LT(alpha_hull->getArea(), 196.0);
  }

  // Two cavities separated by a 1-column wall exercise hasBoundaryTouch()
  // guards in both findCandidateHoles, computeHullHoles, and isRemovableHole.
  {
    std::unique_ptr<MultiPoint> thin_wall = MakeAdjacentHolesThinWallPoints();
    std::unique_ptr<Geometry> hull =
        ConcaveHull::concaveHullByLength(thin_wall.get(), 2.0, true);
    ASSERT_TRUE(hull->isValid());
    EXPECT_EQ(hull->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_LT(hull->getArea(), 110.0);
  }
}

TEST_F(ConcaveHullTest, DumbbellConnectingTrianglesStayConnected) {
  std::vector<CoordinateXY> coords;
  for (int x = 0; x <= 2; ++x) {
    for (int y = 0; y <= 2; ++y) {
      coords.emplace_back(static_cast<double>(x), static_cast<double>(y));
    }
  }
  for (int x = 8; x <= 10; ++x) {
    for (int y = 0; y <= 2; ++y) {
      coords.emplace_back(static_cast<double>(x), static_cast<double>(y));
    }
  }
  std::unique_ptr<MultiPoint> dumbbell = MakeMultiPoint(coords);

  // Even with maxLength = 0.0, connecting bridge triangles cannot be eroded
  // because the hull must remain a single connected Polygon.
  for (double max_len : {0.0, 2.0}) {
    std::unique_ptr<Geometry> hull =
        ConcaveHull::concaveHullByLength(dumbbell.get(), max_len);
    EXPECT_EQ(hull->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(hull->isValid());
    EXPECT_GT(hull->getArea(), 0.0);

    std::unique_ptr<Point> left_pt = MakePoint(1.0, 1.0);
    std::unique_ptr<Point> right_pt = MakePoint(9.0, 1.0);
    EXPECT_TRUE(hull->covers(left_pt.get()));
    EXPECT_TRUE(hull->covers(right_pt.get()));
  }
}

TEST_F(ConcaveHullTest, InstanceReuseAndCriteriaSwitching) {
  std::unique_ptr<MultiPoint> u_shape = MakeUShapePoints();
  ConcaveHull hull_builder(u_shape.get());

  // Default state: edge length 0.0, holes disallowed.
  {
    std::unique_ptr<Geometry> g0 = hull_builder.getHull();
    EXPECT_EQ(g0->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(g0->isValid());
    EXPECT_LE(g0->getArea(), 54.0);
  }

  // Switch to alpha criteria.
  {
    hull_builder.setAlpha(1.5);
    std::unique_ptr<Geometry> g1 = hull_builder.getHull();
    EXPECT_TRUE(g1->isValid());
    EXPECT_GE(g1->getArea(), 51.9);
    EXPECT_LE(g1->getArea(), 54.0);
  }

  // Switch back to explicit edge length criteria (clears ratio & alpha mode).
  {
    hull_builder.setMaximumEdgeLength(100.0);
    std::unique_ptr<Geometry> g2 = hull_builder.getHull();
    EXPECT_TRUE(g2->isValid());
    EXPECT_DOUBLE_EQ(g2->getArea(), 100.0);
  }

  // Switch to edge length ratio criteria.
  {
    hull_builder.setMaximumEdgeLengthRatio(0.15);
    std::unique_ptr<Geometry> g3 = hull_builder.getHull();
    EXPECT_TRUE(g3->isValid());
    EXPECT_GE(g3->getArea(), 51.9);
    EXPECT_LE(g3->getArea(), 54.0);
  }

  // Toggle holesAllowed on a single ConcaveHull instance.
  std::unique_ptr<MultiPoint> frame = MakeSingleHoleFramePoints();
  ConcaveHull frame_builder(frame.get());
  frame_builder.setMaximumEdgeLength(2.0);
  frame_builder.setHolesAllowed(true);
  {
    std::unique_ptr<Geometry> with_hole = frame_builder.getHull();
    const auto* poly = dynamic_cast<const Polygon*>(with_hole.get());
    ASSERT_NE(poly, nullptr);
    EXPECT_EQ(poly->getNumInteriorRing(), 1u);
  }
  frame_builder.setHolesAllowed(false);
  {
    std::unique_ptr<Geometry> without_hole = frame_builder.getHull();
    const auto* poly = dynamic_cast<const Polygon*>(without_hole.get());
    ASSERT_NE(poly, nullptr);
    EXPECT_EQ(poly->getNumInteriorRing(), 0u);
    EXPECT_DOUBLE_EQ(poly->getArea(), 100.0);
  }
}

TEST_F(ConcaveHullTest, NonPointAndDuplicatePointInputs) {
  // Polygon input extracts ring vertices (including repeated start/end vertex).
  {
    std::unique_ptr<Geometry> l_poly =
        ReadWKT("POLYGON ((0 0, 10 0, 10 2, 2 2, 2 10, 0 10, 0 0))");
    // Convex hull area = 10*10 - 0.5*8*8 = 68.0.
    std::unique_ptr<Geometry> convex =
        ConcaveHull::concaveHullByLength(l_poly.get(), 100.0);
    EXPECT_EQ(convex->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_DOUBLE_EQ(convex->getArea(), 68.0);

    // Longest chord across the L-concavity (10,2)-(2,10) has length 8*sqrt(2)
    // (~11.31), while outer edges are at most 10.0, so maxLength = 10.5
    // recovers the L-shape of area 36.0.
    std::unique_ptr<Geometry> concave =
        ConcaveHull::concaveHullByLength(l_poly.get(), 10.5);
    EXPECT_EQ(concave->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_DOUBLE_EQ(concave->getArea(), 36.0);
  }

  // LineString input.
  {
    std::unique_ptr<Geometry> line = ReadWKT("LINESTRING (0 0, 4 0, 4 4, 0 4)");
    std::unique_ptr<Geometry> hull =
        ConcaveHull::concaveHullByLengthRatio(line.get(), 1.0);
    EXPECT_EQ(hull->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_DOUBLE_EQ(hull->getArea(), 16.0);
  }

  // GeometryCollection with duplicate coordinates.
  {
    std::unique_ptr<Geometry> gc = ReadWKT(
        "GEOMETRYCOLLECTION (POINT (0 0), POINT (0 0), "
        "LINESTRING (10 0, 10 10), POINT (0 10))");
    std::unique_ptr<Geometry> hull =
        ConcaveHull::concaveHullByLength(gc.get(), 20.0);
    EXPECT_EQ(hull->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_DOUBLE_EQ(hull->getArea(), 100.0);
  }
}

}  // namespace
}  // namespace geos_google
