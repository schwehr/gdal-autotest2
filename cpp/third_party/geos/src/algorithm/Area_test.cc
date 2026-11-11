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

// Unit tests for third_party/geos/src/algorithm/Area.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/algorithm/Area.cpp

#include "third_party/geos/include/geos/algorithm/Area.h"

#include <cstddef>
#include <memory>
#include <numbers>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
#include "third_party/geos/include/geos/geom/CompoundCurve.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/SimpleCurve.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::algorithm::Area;
using geos::geom::CircularString;
using geos::geom::CompoundCurve;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::SimpleCurve;
using geos::util::IllegalArgumentException;

constexpr double kTol = 1e-11;

std::vector<Coordinate> ToCoordVector(const std::vector<CoordinateXY>& pts) {
  std::vector<Coordinate> out;
  out.reserve(pts.size());
  double z = 0.0;
  for (const auto& p : pts) {
    out.emplace_back(p.x, p.y, z);
    z += 10.0;
  }
  return out;
}

TEST(AreaTest, OfRingDegenerateAndTooShortVectors) {
  const std::vector<Coordinate> empty_c;
  const std::vector<Coordinate> one_c = {{1.0, 2.0, 3.0}};
  const std::vector<Coordinate> two_c = {{0.0, 0.0, 0.0}, {3.0, 4.0, 5.0}};
  const std::vector<Coordinate> collinear_c = {
      {0.0, 0.0}, {1.0, 0.0}, {2.0, 0.0}, {0.0, 0.0}};

  EXPECT_DOUBLE_EQ(Area::ofRingSigned(empty_c), 0.0);
  EXPECT_DOUBLE_EQ(Area::ofRing(empty_c), 0.0);
  EXPECT_DOUBLE_EQ(Area::ofRingSigned(one_c), 0.0);
  EXPECT_DOUBLE_EQ(Area::ofRing(one_c), 0.0);
  EXPECT_DOUBLE_EQ(Area::ofRingSigned(two_c), 0.0);
  EXPECT_DOUBLE_EQ(Area::ofRing(two_c), 0.0);
  EXPECT_DOUBLE_EQ(Area::ofRingSigned(collinear_c), 0.0);
  EXPECT_DOUBLE_EQ(Area::ofRing(collinear_c), 0.0);

  const std::vector<CoordinateXY> empty_xy;
  const std::vector<CoordinateXY> one_xy = {{1.0, 2.0}};
  const std::vector<CoordinateXY> two_xy = {{0.0, 0.0}, {3.0, 4.0}};
  const std::vector<CoordinateXY> collinear_xy = {
      {0.0, 0.0}, {1.0, 1.0}, {2.0, 2.0}, {0.0, 0.0}};

  EXPECT_DOUBLE_EQ(Area::ofRing(empty_xy), 0.0);
  EXPECT_DOUBLE_EQ(Area::ofRing(one_xy), 0.0);
  EXPECT_DOUBLE_EQ(Area::ofRing(two_xy), 0.0);
  EXPECT_DOUBLE_EQ(Area::ofRing(collinear_xy), 0.0);
}

TEST(AreaTest, OfRingCoordinateAndCoordinateXYVectors) {
  // CW 2x3 rectangle: signed area is +6.0.
  const std::vector<CoordinateXY> cw_rect = {
      {0.0, 0.0}, {0.0, 3.0}, {2.0, 3.0}, {2.0, 0.0}, {0.0, 0.0}};
  const std::vector<CoordinateXY> ccw_rect = {
      {0.0, 0.0}, {2.0, 0.0}, {2.0, 3.0}, {0.0, 3.0}, {0.0, 0.0}};

  EXPECT_DOUBLE_EQ(Area::ofRingSigned(ToCoordVector(cw_rect)), 6.0);
  EXPECT_DOUBLE_EQ(Area::ofRing(ToCoordVector(cw_rect)), 6.0);
  EXPECT_DOUBLE_EQ(Area::ofRing(cw_rect), 6.0);

  EXPECT_DOUBLE_EQ(Area::ofRingSigned(ToCoordVector(ccw_rect)), -6.0);
  EXPECT_DOUBLE_EQ(Area::ofRing(ToCoordVector(ccw_rect)), 6.0);
  EXPECT_DOUBLE_EQ(Area::ofRing(ccw_rect), 6.0);

  // CW 3-4-5 right triangle: area = 6.0.
  const std::vector<Coordinate> cw_tri = {
      {0.0, 0.0}, {0.0, 3.0}, {4.0, 0.0}, {0.0, 0.0}};
  const std::vector<Coordinate> ccw_tri = {
      {0.0, 0.0}, {4.0, 0.0}, {0.0, 3.0}, {0.0, 0.0}};
  EXPECT_DOUBLE_EQ(Area::ofRingSigned(cw_tri), 6.0);
  EXPECT_DOUBLE_EQ(Area::ofRingSigned(ccw_tri), -6.0);
  EXPECT_DOUBLE_EQ(Area::ofRing(cw_tri), 6.0);
  EXPECT_DOUBLE_EQ(Area::ofRing(std::vector<CoordinateXY>{
                       {0.0, 0.0}, {0.0, 3.0}, {4.0, 0.0}, {0.0, 0.0}}),
                   6.0);

  // Non-convex CW L-shape: 2x2 square missing upper-right 1x1 corner => area 3.
  const std::vector<Coordinate> cw_l = {{0.0, 0.0}, {0.0, 2.0}, {1.0, 2.0},
                                        {1.0, 1.0}, {2.0, 1.0}, {2.0, 0.0},
                                        {0.0, 0.0}};
  EXPECT_DOUBLE_EQ(Area::ofRingSigned(cw_l), 3.0);
  EXPECT_DOUBLE_EQ(Area::ofRing(cw_l), 3.0);

  // Symmetric self-intersecting bowtie: signed lobes cancel to 0.
  const std::vector<Coordinate> bowtie = {
      {0.0, 0.0}, {2.0, 2.0}, {2.0, 0.0}, {0.0, 2.0}, {0.0, 0.0}};
  EXPECT_DOUBLE_EQ(Area::ofRingSigned(bowtie), 0.0);
  EXPECT_DOUBLE_EQ(Area::ofRing(bowtie), 0.0);

  // Far-translated 2x2 square checks precision of x0 origin subtraction.
  constexpr double kOffset = 1e7;
  const std::vector<CoordinateXY> shifted = {{kOffset, -kOffset},
                                             {kOffset, -kOffset + 2.0},
                                             {kOffset + 2.0, -kOffset + 2.0},
                                             {kOffset + 2.0, -kOffset},
                                             {kOffset, -kOffset}};
  EXPECT_DOUBLE_EQ(Area::ofRing(shifted), 4.0);
}

TEST(AreaTest, OfRingCoordinateSequenceAcrossDimensions) {
  const CoordinateSequence empty_seq = CoordinateSequence::XY(0);
  const CoordinateSequence one_seq({CoordinateXY(1.0, 2.0)});
  const CoordinateSequence two_seq(
      {CoordinateXY(0.0, 0.0), CoordinateXY(5.0, 5.0)});

  EXPECT_DOUBLE_EQ(Area::ofRingSigned(&empty_seq), 0.0);
  EXPECT_DOUBLE_EQ(Area::ofRing(&empty_seq), 0.0);
  EXPECT_DOUBLE_EQ(Area::ofRingSigned(&one_seq), 0.0);
  EXPECT_DOUBLE_EQ(Area::ofRing(&one_seq), 0.0);
  EXPECT_DOUBLE_EQ(Area::ofRingSigned(&two_seq), 0.0);
  EXPECT_DOUBLE_EQ(Area::ofRing(&two_seq), 0.0);

  // XY sequence (CW and CCW).
  const CoordinateSequence xy_cw(
      {CoordinateXY(0.0, 0.0), CoordinateXY(0.0, 2.0), CoordinateXY(3.0, 2.0),
       CoordinateXY(3.0, 0.0), CoordinateXY(0.0, 0.0)});
  CoordinateSequence xy_ccw = xy_cw;
  xy_ccw.reverse();
  EXPECT_DOUBLE_EQ(Area::ofRingSigned(&xy_cw), 6.0);
  EXPECT_DOUBLE_EQ(Area::ofRing(&xy_cw), 6.0);
  EXPECT_DOUBLE_EQ(Area::ofRingSigned(&xy_ccw), -6.0);
  EXPECT_DOUBLE_EQ(Area::ofRing(&xy_ccw), 6.0);

  // XYZ sequence.
  const CoordinateSequence xyz_cw(
      {Coordinate(0.0, 0.0, 1.0), Coordinate(0.0, 2.0, 2.0),
       Coordinate(2.0, 2.0, 3.0), Coordinate(2.0, 0.0, 4.0),
       Coordinate(0.0, 0.0, 1.0)});
  EXPECT_DOUBLE_EQ(Area::ofRingSigned(&xyz_cw), 4.0);
  EXPECT_DOUBLE_EQ(Area::ofRing(&xyz_cw), 4.0);

  // XYM sequence.
  const CoordinateSequence xym_ccw(
      {CoordinateXYM(0.0, 0.0, 10.0), CoordinateXYM(2.0, 0.0, 20.0),
       CoordinateXYM(2.0, 2.0, 30.0), CoordinateXYM(0.0, 2.0, 40.0),
       CoordinateXYM(0.0, 0.0, 10.0)});
  EXPECT_DOUBLE_EQ(Area::ofRingSigned(&xym_ccw), -4.0);
  EXPECT_DOUBLE_EQ(Area::ofRing(&xym_ccw), 4.0);

  // XYZM sequence far from origin.
  constexpr double kOffset = 1e7;
  const CoordinateSequence xyzm_cw({
      CoordinateXYZM(kOffset, kOffset, 1.0, 2.0),
      CoordinateXYZM(kOffset, kOffset + 3.0, 3.0, 4.0),
      CoordinateXYZM(kOffset + 4.0, kOffset + 3.0, 5.0, 6.0),
      CoordinateXYZM(kOffset + 4.0, kOffset, 7.0, 8.0),
      CoordinateXYZM(kOffset, kOffset, 1.0, 2.0),
  });
  EXPECT_DOUBLE_EQ(Area::ofRingSigned(&xyzm_cw), 12.0);
  EXPECT_DOUBLE_EQ(Area::ofRing(&xyzm_cw), 12.0);
}

TEST(AreaTest, OfClosedCurveRejectsUnclosedOrEmptyOpenCurves) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Empty LineString, CircularString, and CompoundCurve report isClosed() ==
  // false.
  std::unique_ptr<LineString> empty_ls = factory->createLineString();
  EXPECT_THROW(Area::ofClosedCurve(*empty_ls), IllegalArgumentException);

  std::unique_ptr<CircularString> empty_cs =
      factory->createCircularString(false, false);
  EXPECT_THROW(Area::ofClosedCurve(*empty_cs), IllegalArgumentException);

  std::unique_ptr<CompoundCurve> empty_cc = factory->createCompoundCurve();
  EXPECT_THROW(Area::ofClosedCurve(*empty_cc), IllegalArgumentException);

  // Non-empty open curves.
  std::unique_ptr<LineString> open_ls = factory->createLineString(
      CoordinateSequence({CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 0.0),
                          CoordinateXY(2.0, 2.0)}));
  EXPECT_THROW(Area::ofClosedCurve(*open_ls), IllegalArgumentException);

  std::unique_ptr<CircularString> open_cs = factory->createCircularString(
      CoordinateSequence({CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0),
                          CoordinateXY(2.0, 0.0)}));
  EXPECT_THROW(Area::ofClosedCurve(*open_cs), IllegalArgumentException);

  std::vector<std::unique_ptr<SimpleCurve>> open_parts;
  open_parts.push_back(factory->createLineString(
      CoordinateSequence({CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 0.0)})));
  open_parts.push_back(factory->createCircularString(
      CoordinateSequence({CoordinateXY(2.0, 0.0), CoordinateXY(1.0, 1.0),
                          CoordinateXY(0.0, 0.5)})));
  std::unique_ptr<CompoundCurve> open_cc =
      factory->createCompoundCurve(std::move(open_parts));
  EXPECT_THROW(Area::ofClosedCurve(*open_cc), IllegalArgumentException);
}

TEST(AreaTest, OfClosedCurveEmptyLinearRingAndLinearCurves) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Empty LinearRing is closed by definition, hitting the empty-section
  // continue.
  std::unique_ptr<LinearRing> empty_ring = factory->createLinearRing();
  ASSERT_TRUE(empty_ring->isClosed());
  EXPECT_DOUBLE_EQ(Area::ofClosedCurve(*empty_ring), 0.0);

  // Closed CCW and CW LinearRings.
  const CoordinateSequence ccw_coords(
      {CoordinateXY(0.0, 0.0), CoordinateXY(3.0, 0.0), CoordinateXY(3.0, 4.0),
       CoordinateXY(0.0, 4.0), CoordinateXY(0.0, 0.0)});
  std::unique_ptr<LinearRing> ccw_ring = factory->createLinearRing(ccw_coords);
  EXPECT_DOUBLE_EQ(Area::ofClosedCurve(*ccw_ring), 12.0);

  std::unique_ptr<LinearRing> cw_ring = ccw_ring->reverse();
  EXPECT_DOUBLE_EQ(Area::ofClosedCurve(*cw_ring), 12.0);

  // Closed LineStrings (shifted off origin).
  std::unique_ptr<LineString> ccw_ls = factory->createLineString(
      CoordinateSequence({CoordinateXY(10.0, 20.0), CoordinateXY(14.0, 20.0),
                          CoordinateXY(10.0, 23.0), CoordinateXY(10.0, 20.0)}));
  EXPECT_DOUBLE_EQ(Area::ofClosedCurve(*ccw_ls), 6.0);
  std::unique_ptr<LineString> cw_ls = ccw_ls->reverse();
  EXPECT_DOUBLE_EQ(Area::ofClosedCurve(*cw_ls), 6.0);

  // Degenerate closed collinear LineString.
  std::unique_ptr<LineString> flat_ls = factory->createLineString(
      CoordinateSequence({CoordinateXY(0.0, 0.0), CoordinateXY(5.0, 0.0),
                          CoordinateXY(0.0, 0.0)}));
  EXPECT_DOUBLE_EQ(Area::ofClosedCurve(*flat_ls), 0.0);
}

TEST(AreaTest, OfClosedCurveCircularStrings) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Full circle of radius 2 centered at (0, 0), oriented CCW.
  std::unique_ptr<CircularString> circle_ccw = factory->createCircularString(
      CoordinateSequence({CoordinateXY(2.0, 0.0), CoordinateXY(0.0, 2.0),
                          CoordinateXY(-2.0, 0.0), CoordinateXY(0.0, -2.0),
                          CoordinateXY(2.0, 0.0)}));
  EXPECT_NEAR(Area::ofClosedCurve(*circle_ccw), 4.0 * std::numbers::pi, kTol);

  // Reversed CW circle exercises the negative orientation branch.
  std::unique_ptr<CircularString> circle_cw = circle_ccw->reverse();
  EXPECT_NEAR(Area::ofClosedCurve(*circle_cw), 4.0 * std::numbers::pi, kTol);

  // Single-arc full circle centered at (1, 0) with radius 1.
  std::unique_ptr<CircularString> single_arc_circle =
      factory->createCircularString(
          CoordinateSequence({CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 0.0),
                              CoordinateXY(0.0, 0.0)}));
  EXPECT_NEAR(Area::ofClosedCurve(*single_arc_circle), std::numbers::pi, kTol);

  // Off-origin circle of radius 3 centered at (5, -3).
  std::unique_ptr<CircularString> shifted_circle =
      factory->createCircularString(
          CoordinateSequence({CoordinateXY(8.0, -3.0), CoordinateXY(5.0, 0.0),
                              CoordinateXY(2.0, -3.0), CoordinateXY(5.0, -6.0),
                              CoordinateXY(8.0, -3.0)}));
  EXPECT_NEAR(Area::ofClosedCurve(*shifted_circle), 9.0 * std::numbers::pi,
              kTol);

  // Closed CircularString composed solely of collinear arcs (isLinear() ==
  // true).
  std::unique_ptr<CircularString> linear_cs = factory->createCircularString(
      CoordinateSequence({CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0),
                          CoordinateXY(2.0, 0.0), CoordinateXY(1.0, 0.0),
                          CoordinateXY(0.0, 0.0)}));
  EXPECT_DOUBLE_EQ(Area::ofClosedCurve(*linear_cs), 0.0);
}

TEST(AreaTest, OfClosedCurveCompoundCurves) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // 1. D-shape: chord + semicircle of radius 2 => area = 2 * pi (CCW and CW).
  {
    std::vector<std::unique_ptr<SimpleCurve>> parts;
    parts.push_back(factory->createLineString(
        CoordinateSequence({CoordinateXY(-2.0, 0.0), CoordinateXY(2.0, 0.0)})));
    parts.push_back(factory->createCircularString(
        CoordinateSequence({CoordinateXY(2.0, 0.0), CoordinateXY(0.0, 2.0),
                            CoordinateXY(-2.0, 0.0)})));
    std::unique_ptr<CompoundCurve> d_ccw =
        factory->createCompoundCurve(std::move(parts));
    EXPECT_NEAR(Area::ofClosedCurve(*d_ccw), 2.0 * std::numbers::pi, kTol);

    std::unique_ptr<CompoundCurve> d_cw = d_ccw->reverse();
    EXPECT_NEAR(Area::ofClosedCurve(*d_cw), 2.0 * std::numbers::pi, kTol);
  }

  // 2. Square [0, 2] x [0, 2] with an outward semicircle on the right edge:
  //    area = 4 + pi / 2.
  {
    std::vector<std::unique_ptr<SimpleCurve>> parts;
    parts.push_back(factory->createLineString(
        CoordinateSequence({CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 0.0)})));
    parts.push_back(factory->createCircularString(
        CoordinateSequence({CoordinateXY(2.0, 0.0), CoordinateXY(3.0, 1.0),
                            CoordinateXY(2.0, 2.0)})));
    parts.push_back(factory->createLineString(
        CoordinateSequence({CoordinateXY(2.0, 2.0), CoordinateXY(0.0, 2.0),
                            CoordinateXY(0.0, 0.0)})));
    std::unique_ptr<CompoundCurve> bulge_ccw =
        factory->createCompoundCurve(std::move(parts));
    EXPECT_NEAR(Area::ofClosedCurve(*bulge_ccw), 4.0 + 0.5 * std::numbers::pi,
                kTol);

    std::unique_ptr<CompoundCurve> bulge_cw = bulge_ccw->reverse();
    EXPECT_NEAR(Area::ofClosedCurve(*bulge_cw), 4.0 + 0.5 * std::numbers::pi,
                kTol);
  }

  // 3. Square [0, 2] x [0, 2] with an inward semicircular dent on the right
  // edge:
  //    area = 4 - pi / 2 (tests opposing sign between polygon and circular
  //    arc).
  {
    std::vector<std::unique_ptr<SimpleCurve>> parts;
    parts.push_back(factory->createLineString(
        CoordinateSequence({CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 0.0)})));
    parts.push_back(factory->createCircularString(
        CoordinateSequence({CoordinateXY(2.0, 0.0), CoordinateXY(1.0, 1.0),
                            CoordinateXY(2.0, 2.0)})));
    parts.push_back(factory->createLineString(
        CoordinateSequence({CoordinateXY(2.0, 2.0), CoordinateXY(0.0, 2.0),
                            CoordinateXY(0.0, 0.0)})));
    std::unique_ptr<CompoundCurve> dent_ccw =
        factory->createCompoundCurve(std::move(parts));
    EXPECT_NEAR(Area::ofClosedCurve(*dent_ccw), 4.0 - 0.5 * std::numbers::pi,
                kTol);

    std::unique_ptr<CompoundCurve> dent_cw = dent_ccw->reverse();
    EXPECT_NEAR(Area::ofClosedCurve(*dent_cw), 4.0 - 0.5 * std::numbers::pi,
                kTol);
  }

  // 4. Stadium: 4x2 rectangle + two radius-1 semicircles => area = 8 + pi.
  {
    std::vector<std::unique_ptr<SimpleCurve>> parts;
    parts.push_back(factory->createLineString(
        CoordinateSequence({CoordinateXY(0.0, 0.0), CoordinateXY(4.0, 0.0)})));
    parts.push_back(factory->createCircularString(
        CoordinateSequence({CoordinateXY(4.0, 0.0), CoordinateXY(5.0, 1.0),
                            CoordinateXY(4.0, 2.0)})));
    parts.push_back(factory->createLineString(
        CoordinateSequence({CoordinateXY(4.0, 2.0), CoordinateXY(0.0, 2.0)})));
    parts.push_back(factory->createCircularString(
        CoordinateSequence({CoordinateXY(0.0, 2.0), CoordinateXY(-1.0, 1.0),
                            CoordinateXY(0.0, 0.0)})));
    std::unique_ptr<CompoundCurve> stadium =
        factory->createCompoundCurve(std::move(parts));
    EXPECT_NEAR(Area::ofClosedCurve(*stadium), 8.0 + std::numbers::pi, kTol);
  }

  // 5. Square [0, 2] x [0, 2] whose bottom edge is a collinear CircularString
  //    (exercises arc.isLinear() inside a non-zero-area CompoundCurve).
  {
    std::vector<std::unique_ptr<SimpleCurve>> parts;
    parts.push_back(factory->createCircularString(
        CoordinateSequence({CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0),
                            CoordinateXY(2.0, 0.0)})));
    parts.push_back(factory->createLineString(
        CoordinateSequence({CoordinateXY(2.0, 0.0), CoordinateXY(2.0, 2.0),
                            CoordinateXY(0.0, 2.0), CoordinateXY(0.0, 0.0)})));
    std::unique_ptr<CompoundCurve> mixed =
        factory->createCompoundCurve(std::move(parts));
    EXPECT_DOUBLE_EQ(Area::ofClosedCurve(*mixed), 4.0);
  }
}

}  // namespace
}  // namespace geos_google
