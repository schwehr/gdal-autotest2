// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "third_party/geos/include/geos/algorithm/ConvexHull.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/GEOSException.h"
#include "third_party/geos/include/geos/util/Interrupt.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::algorithm::ConvexHull;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GEOS_GEOMETRYCOLLECTION;
using geos::geom::GEOS_LINESTRING;
using geos::geom::GEOS_POINT;
using geos::geom::GEOS_POLYGON;
using geos::io::WKTReader;

class ConvexHullTest : public ::testing::Test {
 protected:
  ConvexHullTest() : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  std::unique_ptr<Geometry> ComputeHull(const Geometry& geom) const {
    ConvexHull ch(&geom);
    return ch.getConvexHull();
  }

  void ExpectHullEqualsWkt(const std::string& input_wkt,
                           const std::string& expected_wkt) const {
    auto input = ReadWkt(input_wkt);
    auto expected = ReadWkt(expected_wkt);
    auto hull = ComputeHull(*input);
    ASSERT_NE(hull, nullptr);
    EXPECT_TRUE(hull->isValid());
    EXPECT_TRUE(hull->equals(expected.get()))
        << "Input: " << input_wkt << "\nActual hull: " << hull->toString()
        << "\nExpected: " << expected->toString();

    auto via_geom = input->convexHull();
    ASSERT_NE(via_geom, nullptr);
    EXPECT_TRUE(via_geom->equalsExact(hull.get(), 1e-12));
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(ConvexHullTest, EmptyGeometriesReturnEmptyGeometryCollection) {
  const std::vector<std::string> empty_wkts = {
      "POINT EMPTY",
      "LINESTRING EMPTY",
      "POLYGON EMPTY",
      "MULTIPOINT EMPTY",
      "MULTILINESTRING EMPTY",
      "MULTIPOLYGON EMPTY",
      "GEOMETRYCOLLECTION EMPTY",
      "GEOMETRYCOLLECTION (POINT EMPTY, LINESTRING EMPTY, POLYGON EMPTY)",
  };

  for (const auto& wkt : empty_wkts) {
    auto input = ReadWkt(wkt);
    auto hull = ComputeHull(*input);
    ASSERT_NE(hull, nullptr) << wkt;
    EXPECT_TRUE(hull->isEmpty()) << wkt;
    EXPECT_EQ(hull->getGeometryTypeId(), GEOS_GEOMETRYCOLLECTION) << wkt;

    auto via_geom = input->convexHull();
    ASSERT_NE(via_geom, nullptr) << wkt;
    EXPECT_TRUE(via_geom->isEmpty()) << wkt;
    EXPECT_EQ(via_geom->getGeometryTypeId(), GEOS_GEOMETRYCOLLECTION) << wkt;
  }
}

TEST_F(ConvexHullTest, SinglePointAndRepeatedIdenticalPointsReturnPoint) {
  ExpectHullEqualsWkt("POINT (3 4)", "POINT (3 4)");
  ExpectHullEqualsWkt("POINT (-12.5 99.25)", "POINT (-12.5 99.25)");
  ExpectHullEqualsWkt("MULTIPOINT ((3 4), (3 4), (3 4))", "POINT (3 4)");
  ExpectHullEqualsWkt(
      "GEOMETRYCOLLECTION (POINT EMPTY, POINT (7 -2), MULTIPOINT ((7 -2)))",
      "POINT (7 -2)");

  auto hull = ComputeHull(*ReadWkt("POINT (3 4)"));
  EXPECT_EQ(hull->getGeometryTypeId(), GEOS_POINT);
  EXPECT_EQ(hull->getNumPoints(), 1u);
}

TEST_F(ConvexHullTest, TwoUniquePointsReturnTwoPointLineString) {
  ExpectHullEqualsWkt("LINESTRING (0 0, 3 4)", "LINESTRING (0 0, 3 4)");
  ExpectHullEqualsWkt("MULTIPOINT ((1 2), (5 6), (1 2), (5 6))",
                      "LINESTRING (1 2, 5 6)");
  ExpectHullEqualsWkt("LINESTRING (0 0, 10 0, 0 0, 10 0)",
                      "LINESTRING (0 0, 10 0)");

  auto hull = ComputeHull(*ReadWkt("MULTIPOINT ((1 2), (5 6), (1 2))"));
  EXPECT_EQ(hull->getGeometryTypeId(), GEOS_LINESTRING);
  EXPECT_EQ(hull->getNumPoints(), 2u);
}

TEST_F(ConvexHullTest, ThreeOrMoreCollinearPointsCollapseToTwoPointLine) {
  // Horizontal collinear points.
  ExpectHullEqualsWkt("MULTIPOINT ((5 0), (0 0), (2 0), (10 0), (5 0))",
                      "LINESTRING (0 0, 10 0)");
  // Vertical collinear points (exercises y-tiebreaking in RadiallyLessThen).
  ExpectHullEqualsWkt("MULTIPOINT ((3 10), (3 0), (3 5), (3 2), (3 8))",
                      "LINESTRING (3 0, 3 10)");
  // Positive-slope diagonal collinear points.
  ExpectHullEqualsWkt("LINESTRING (0 0, 1 1, 4 4, 2 2, 5 5)",
                      "LINESTRING (0 0, 5 5)");
  // Negative-slope diagonal collinear points.
  ExpectHullEqualsWkt("MULTIPOINT ((10 0), (0 10), (5 5), (2 8), (8 2))",
                      "LINESTRING (10 0, 0 10)");

  auto hull = ComputeHull(*ReadWkt("MULTIPOINT ((0 0), (1 1), (2 2), (3 3))"));
  EXPECT_EQ(hull->getGeometryTypeId(), GEOS_LINESTRING);
  EXPECT_EQ(hull->getNumPoints(), 2u);
}

TEST_F(ConvexHullTest, CollinearPointsAlongAllFourPolygonEdgesAreCleaned) {
  // Pre-sorted focal point tie-breaking: (5, 0) appears before (0, 0).
  // Midpoints on all four square edges exercise all 4 direction branches
  // of ConvexHull::isBetween in cleanRing.
  auto input = ReadWkt(
      "MULTIPOINT ((5 0), (10 0), (10 5), (10 10), "
      "(5 10), (0 10), (0 5), (0 0))");
  auto hull = ComputeHull(*input);
  ASSERT_NE(hull, nullptr);
  EXPECT_EQ(hull->getGeometryTypeId(), GEOS_POLYGON);
  EXPECT_EQ(hull->getNumPoints(), 5u);
  auto expected = ReadWkt("POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))");
  EXPECT_TRUE(hull->equals(expected.get()));
}

TEST_F(ConvexHullTest, HorizontalCollinearPointsOnBottomEdgeWithApex) {
  // Exercises horizontal collinear comparisons (p->x > q->x and p->x < q->x)
  // in RadiallyLessThen and multiple pops in grahamScan.
  ExpectHullEqualsWkt("MULTIPOINT ((8 0), (2 0), (0 0), (5 0), (10 0), (5 10))",
                      "POLYGON ((0 0, 5 10, 10 0, 0 0))");
}

TEST_F(ConvexHullTest, MinimalTriangleAndStarWithDeepGrahamScanPops) {
  // Exact 3-point triangle: grahamScan loop at i >= 3 executes 0 times.
  ExpectHullEqualsWkt("MULTIPOINT ((0 0), (4 0), (2 3))",
                      "POLYGON ((0 0, 2 3, 4 0, 0 0))");

  // Concave star-shaped polygon forcing multiple pops in grahamScan.
  ExpectHullEqualsWkt(
      "POLYGON ((0 0, 5 2, 10 0, 7 5, 10 10, 5 7, 0 10, 3 5, 0 0))",
      "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))");

  // Negative coordinates with multiple minimum-y points and midpoints on
  // negative edges, exercising focal-point tie-breaking in preSort and
  // collinear edge removal in cleanRing.
  ExpectHullEqualsWkt(
      "MULTIPOINT ((5 -20), (-10 10), (-10 -20), (10 -20), (10 10), "
      "(0 -20), (-10 0), (10 0), (0 10))",
      "POLYGON ((-10 -20, -10 10, 10 10, 10 -20, -10 -20))");
}

TEST_F(ConvexHullTest, ConcavePolygonWithHoleAndInteriorPoints) {
  // U-shaped polygon with a hole inside it.
  ExpectHullEqualsWkt(
      "POLYGON ((0 0, 10 0, 10 10, 8 10, 8 2, 2 2, 2 10, 0 10, 0 0), "
      "(3 3, 7 3, 7 5, 3 5, 3 3))",
      "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))");
}

TEST_F(ConvexHullTest, MultiGeometriesAndHeterogeneousCollection) {
  ExpectHullEqualsWkt("MULTILINESTRING ((0 0, 2 4), (4 4, 6 0), (3 1, 3 2))",
                      "POLYGON ((0 0, 2 4, 4 4, 6 0, 0 0))");

  ExpectHullEqualsWkt(
      "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "((4 0, 6 0, 6 2, 4 2, 4 0)))",
      "POLYGON ((0 0, 0 2, 6 2, 6 0, 0 0))");

  // Polygon vertices (2, 4) and (8, 4) protrude outside the (0,0)-(5,8)-(10,0)
  // triangle (whose side height at x = 2 and x = 8 is 3.2), forming a pentagon.
  ExpectHullEqualsWkt(
      "GEOMETRYCOLLECTION (POINT (5 8), LINESTRING (0 0, 10 0), "
      "POLYGON ((2 1, 8 1, 8 4, 2 4, 2 1)))",
      "POLYGON ((0 0, 2 4, 5 8, 8 4, 10 0, 0 0))");

  // Polygon strictly contained inside the triangle.
  ExpectHullEqualsWkt(
      "GEOMETRYCOLLECTION (POINT (5 8), LINESTRING (0 0, 10 0), "
      "POLYGON ((2 1, 8 1, 7 3, 3 3, 2 1)))",
      "POLYGON ((0 0, 5 8, 10 0, 0 0))");
}

TEST_F(ConvexHullTest, ExactBoundaryAroundTuningReduceSizeFifty) {
  // 50 input points: takes the non-reduced extractUnique branch.
  // Use exact integer interior points inside [1, 8] x [1, 6].
  {
    std::vector<Coordinate> coords;
    coords.emplace_back(0.0, 0.0);
    coords.emplace_back(10.0, 0.0);
    coords.emplace_back(10.0, 10.0);
    coords.emplace_back(0.0, 10.0);
    for (int i = 0; i < 46; ++i) {
      coords.emplace_back(1.0 + static_cast<double>(i % 8),
                          1.0 + static_cast<double>(i / 8));
    }
    ASSERT_EQ(coords.size(), 50u);
    auto mp = factory_->createMultiPoint(coords);
    auto hull = ComputeHull(*mp);
    auto expected = ReadWkt("POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))");
    EXPECT_TRUE(hull->equals(expected.get()));
  }

  // 51 input points: takes the reduce(inputPts) branch (> TUNING_REDUCE_SIZE).
  {
    std::vector<Coordinate> coords;
    coords.emplace_back(2.0, 2.0);  // Interior starting point.
    coords.emplace_back(0.0, 0.0);
    coords.emplace_back(10.0, 0.0);
    coords.emplace_back(10.0, 10.0);
    coords.emplace_back(0.0, 10.0);
    for (int i = 0; i < 46; ++i) {
      coords.emplace_back(1.0 + static_cast<double>(i % 8),
                          1.0 + static_cast<double>(i / 8));
    }
    ASSERT_EQ(coords.size(), 51u);
    auto mp = factory_->createMultiPoint(coords);
    auto hull = ComputeHull(*mp);
    auto expected = ReadWkt("POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))");
    EXPECT_TRUE(hull->equals(expected.get()));
  }

  // Strictly convex 50-gon and 51-gon preserve all hull vertices across
  // the TUNING_REDUCE_SIZE boundary.
  for (const int sides : std::array<int, 2>{50, 51}) {
    std::vector<Coordinate> coords;
    constexpr double kRadius = 10.0;
    for (int i = 0; i < sides; ++i) {
      const double angle =
          2.0 * std::numbers::pi * static_cast<double>(i) / sides;
      coords.emplace_back(kRadius * std::cos(angle), kRadius * std::sin(angle));
    }
    auto mp = factory_->createMultiPoint(coords);
    auto hull = ComputeHull(*mp);
    ASSERT_NE(hull, nullptr);
    EXPECT_EQ(hull->getGeometryTypeId(), GEOS_POLYGON);
    EXPECT_EQ(hull->getNumPoints(), static_cast<std::size_t>(sides + 1));
    EXPECT_TRUE(hull->isValid());
    EXPECT_TRUE(hull->covers(mp.get()));
    const double expected_area = 0.5 * sides * kRadius * kRadius *
                                 std::sin(2.0 * std::numbers::pi / sides);
    EXPECT_NEAR(hull->getArea(), expected_area, 1e-9);
  }
}

TEST_F(ConvexHullTest, LargePointCloudExercisesOctolateralReduction) {
  // Construct > 100 points: an interior grid plus a 24-gon circle perimeter
  // so that the 8 octolateral extremes form an inner octagon, interior grid
  // points are discarded as inside the ring, and intermediate circle vertices
  // remain outside the octolateral and survive into the final hull.
  std::vector<Coordinate> coords;
  coords.emplace_back(0.0, 0.0);

  for (int ix = -4; ix <= 3; ++ix) {
    for (int iy = -4; iy <= 3; ++iy) {
      coords.emplace_back(static_cast<double>(ix), static_cast<double>(iy));
    }
  }

  constexpr int kSides = 24;
  constexpr double kRadius = 10.0;
  for (int i = 0; i < kSides; ++i) {
    const double angle =
        2.0 * std::numbers::pi * static_cast<double>(i) / kSides;
    coords.emplace_back(kRadius * std::cos(angle), kRadius * std::sin(angle));
  }

  ASSERT_GT(coords.size(), 50u);
  auto mp = factory_->createMultiPoint(coords);
  auto hull = ComputeHull(*mp);
  ASSERT_NE(hull, nullptr);
  EXPECT_EQ(hull->getGeometryTypeId(), GEOS_POLYGON);
  EXPECT_EQ(hull->getNumPoints(), static_cast<std::size_t>(kSides + 1));
  EXPECT_TRUE(hull->isValid());
  EXPECT_TRUE(hull->covers(mp.get()));

  const double expected_area = 0.5 * kSides * kRadius * kRadius *
                               std::sin(2.0 * std::numbers::pi / kSides);
  EXPECT_NEAR(hull->getArea(), expected_area, 1e-9);
}

TEST_F(ConvexHullTest, LargeAxisAlignedCollinearCloudsTriggerPadArray3) {
  // > 50 collinear points on y = 0 starting at (0, 0).
  // The octolateral ring collapses to [(0,0), (10,0), (0,0), (0,0)],
  // reducing inputPts to 2 points and triggering padArray3 + cleanRing
  // duplicate coordinate skipping.
  {
    std::vector<Coordinate> coords;
    coords.emplace_back(0.0, 0.0);
    coords.emplace_back(10.0, 0.0);
    for (int i = 1; i <= 79; ++i) {
      coords.emplace_back(0.125 * static_cast<double>(i), 0.0);
    }
    auto mp = factory_->createMultiPoint(coords);
    auto hull = ComputeHull(*mp);
    ASSERT_NE(hull, nullptr);
    EXPECT_EQ(hull->getGeometryTypeId(), GEOS_LINESTRING);
    EXPECT_EQ(hull->getNumPoints(), 2u);
    auto expected = ReadWkt("LINESTRING (0 0, 10 0)");
    EXPECT_TRUE(hull->equals(expected.get()));
  }

  // > 50 collinear points on x = 4 starting at (4, 0).
  {
    std::vector<Coordinate> coords;
    coords.emplace_back(4.0, 0.0);
    coords.emplace_back(4.0, 10.0);
    for (int i = 1; i <= 79; ++i) {
      coords.emplace_back(4.0, 0.125 * static_cast<double>(i));
    }
    auto mp = factory_->createMultiPoint(coords);
    auto hull = ComputeHull(*mp);
    ASSERT_NE(hull, nullptr);
    EXPECT_EQ(hull->getGeometryTypeId(), GEOS_LINESTRING);
    EXPECT_EQ(hull->getNumPoints(), 2u);
    auto expected = ReadWkt("LINESTRING (4 0, 4 10)");
    EXPECT_TRUE(hull->equals(expected.get()));
  }
}

TEST_F(ConvexHullTest, LargeAntiDiagonalCollinearCloudReturnsEarlyFromReduce) {
  // > 50 collinear points along x + y = 10 using exact dyadic steps.
  // All 8 octolateral extremes collapse to just [(0, 10), (10, 0)], so
  // computeInnerOctolateralRing returns false (dest.size() < 3) and reduce()
  // returns early without removing duplicates.
  {
    std::vector<Coordinate> coords;
    coords.emplace_back(10.0, 0.0);
    coords.emplace_back(0.0, 10.0);
    for (int d = 0; d < 5; ++d) {
      // Intentional duplicates for polarCompare == 0.
      coords.emplace_back(5.0, 5.0);
    }
    for (int i = 1; i <= 79; ++i) {
      const double t = 0.125 * static_cast<double>(i);
      coords.emplace_back(t, 10.0 - t);
    }
    auto mp = factory_->createMultiPoint(coords);
    auto hull = ComputeHull(*mp);
    ASSERT_NE(hull, nullptr);
    EXPECT_EQ(hull->getGeometryTypeId(), GEOS_LINESTRING);
    EXPECT_EQ(hull->getNumPoints(), 2u);
    auto expected = ReadWkt("LINESTRING (10 0, 0 10)");
    EXPECT_TRUE(hull->equals(expected.get()));
  }

  // Dedicated all-duplicate middle points ensure std::sort compares equal
  // coordinates directly in polarCompare.
  {
    std::vector<Coordinate> coords;
    coords.emplace_back(10.0, 0.0);
    coords.emplace_back(0.0, 10.0);
    for (int i = 0; i < 55; ++i) {
      coords.emplace_back(5.0, 5.0);
    }
    auto mp = factory_->createMultiPoint(coords);
    auto hull = ComputeHull(*mp);
    ASSERT_NE(hull, nullptr);
    EXPECT_EQ(hull->getGeometryTypeId(), GEOS_LINESTRING);
    EXPECT_EQ(hull->getNumPoints(), 2u);
    auto expected = ReadWkt("LINESTRING (10 0, 0 10)");
    EXPECT_TRUE(hull->equals(expected.get()));
  }
}

TEST_F(ConvexHullTest, PreservesFiniteCoordinatesWithZAndMValues) {
  {
    auto input =
        ReadWkt("POLYGON Z ((0 0 10, 4 0 20, 2 1 30, 4 4 40, 0 4 50, 0 0 10))");
    auto hull = ComputeHull(*input);
    ASSERT_NE(hull, nullptr);
    EXPECT_EQ(hull->getGeometryTypeId(), GEOS_POLYGON);
    auto expected = ReadWkt("POLYGON ((0 0, 0 4, 4 4, 4 0, 0 0))");
    EXPECT_TRUE(hull->equals(expected.get()));
  }

  {
    auto input = ReadWkt(
        "POLYGON ZM ((0 0 1 2, 4 0 2 3, 2 1 3 4, 4 4 4 5, 0 4 5 6, 0 0 1 2))");
    auto hull = ComputeHull(*input);
    ASSERT_NE(hull, nullptr);
    EXPECT_EQ(hull->getGeometryTypeId(), GEOS_POLYGON);
    auto expected = ReadWkt("POLYGON ((0 0, 0 4, 4 4, 4 0, 0 0))");
    EXPECT_TRUE(hull->equals(expected.get()));
  }
}

TEST_F(ConvexHullTest, CurvedComponentsThrowUnsupportedOperationException) {
  auto seq = std::make_unique<CoordinateSequence>();
  seq->add(Coordinate(0.0, 0.0));
  seq->add(Coordinate(1.0, 1.0));
  seq->add(Coordinate(2.0, 0.0));
  auto circular = factory_->createCircularString(std::move(seq));

  EXPECT_THROW(
      { ConvexHull hull(circular.get()); },
      geos::util::UnsupportedOperationException);

  std::vector<std::unique_ptr<Geometry>> parts;
  parts.push_back(factory_->createPoint(Coordinate(0.0, 0.0)));
  parts.push_back(std::move(circular));
  auto collection = factory_->createGeometryCollection(std::move(parts));
  EXPECT_THROW(
      { ConvexHull hull(collection.get()); },
      geos::util::UnsupportedOperationException);
}

TEST_F(ConvexHullTest, CurrentThreadInterruptAbortsComputation) {
  struct InterruptState {
    int calls = 0;
    int interrupt_on = 1;
  };
  struct CallbackGuard {
    geos::util::CurrentThreadInterrupt::ThreadCallback* prev;
    CallbackGuard(geos::util::CurrentThreadInterrupt::ThreadCallback* cb,
                  void* data)
        : prev(geos::util::CurrentThreadInterrupt::registerCallback(cb, data)) {
    }
    ~CallbackGuard() {
      geos::util::CurrentThreadInterrupt::registerCallback(prev, nullptr);
    }
  };

  auto interrupt_on_nth = +[](void* data) -> int {
    auto* state = static_cast<InterruptState*>(data);
    ++state->calls;
    return state->calls >= state->interrupt_on ? 1 : 0;
  };

  auto poly = ReadWkt("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  for (const int check_idx : std::array<int, 3>{1, 2, 3}) {
    InterruptState state{0, check_idx};
    CallbackGuard guard(interrupt_on_nth, &state);
    EXPECT_THROW(
        { ComputeHull(*poly); }, geos::util::GEOSException)
        << "interrupt_on=" << check_idx;
    EXPECT_EQ(state.calls, check_idx);
  }
}

}  // namespace
}  // namespace geos_google
