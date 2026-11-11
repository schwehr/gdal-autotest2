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

#include "third_party/geos/include/geos/algorithm/RayCrossingCounter.h"

#include <array>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/CircularArc.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
#include "third_party/geos/include/geos/geom/CompoundCurve.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/SimpleCurve.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::DoubleInfinity;
using geos::DoubleNotANumber;
using geos::algorithm::RayCrossingCounter;
using geos::geom::CircularArc;
using geos::geom::CircularString;
using geos::geom::CompoundCurve;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::Location;
using geos::geom::SimpleCurve;

TEST(RayCrossingCounterTest, InitialStateAndAccessors) {
  const CoordinateXY pt{1.0, 2.0};
  RayCrossingCounter rcc(pt);

  EXPECT_FALSE(rcc.isOnSegment());
  EXPECT_EQ(rcc.getCount(), 0u);
  EXPECT_EQ(rcc.getLocation(), Location::EXTERIOR);
  EXPECT_FALSE(rcc.isPointInPolygon());
}

TEST(RayCrossingCounterTest, CountSegmentLeftCullAndEndpointMatch) {
  const CoordinateXY pt{5.0, 2.0};

  {
    RayCrossingCounter rcc(pt);
    // Strictly to the left of pt.x -> skipped early.
    rcc.countSegment(CoordinateXY{-1.0, 0.0}, CoordinateXY{4.0, 5.0});
    EXPECT_FALSE(rcc.isOnSegment());
    EXPECT_EQ(rcc.getCount(), 0u);
    EXPECT_EQ(rcc.getLocation(), Location::EXTERIOR);
  }

  {
    RayCrossingCounter rcc(pt);
    // Point equals segment endpoint p2.
    rcc.countSegment(CoordinateXY{1.0, 0.0}, pt);
    EXPECT_TRUE(rcc.isOnSegment());
    EXPECT_EQ(rcc.getLocation(), Location::BOUNDARY);
    EXPECT_TRUE(rcc.isPointInPolygon());
  }
}

TEST(RayCrossingCounterTest, CountSegmentHorizontalCases) {
  {
    // Point on forward horizontal segment (p1.x < p2.x).
    const CoordinateXY pt{2.0, 5.0};
    RayCrossingCounter rcc(pt);
    rcc.countSegment(CoordinateXY{1.0, 5.0}, CoordinateXY{4.0, 5.0});
    EXPECT_TRUE(rcc.isOnSegment());
    EXPECT_EQ(rcc.getLocation(), Location::BOUNDARY);
    EXPECT_EQ(rcc.getCount(), 0u);
  }

  {
    // Point on backward horizontal segment (p1.x > p2.x).
    const CoordinateXY pt{2.0, 5.0};
    RayCrossingCounter rcc(pt);
    rcc.countSegment(CoordinateXY{4.0, 5.0}, CoordinateXY{1.0, 5.0});
    EXPECT_TRUE(rcc.isOnSegment());
    EXPECT_EQ(rcc.getLocation(), Location::BOUNDARY);
  }

  {
    // Point on horizontal segment starting left of pt and ending right of pt.
    const CoordinateXY pt{3.0, 5.0};
    RayCrossingCounter rcc(pt);
    rcc.countSegment(CoordinateXY{1.0, 5.0}, CoordinateXY{6.0, 5.0});
    EXPECT_TRUE(rcc.isOnSegment());
    EXPECT_EQ(rcc.getLocation(), Location::BOUNDARY);
  }

  {
    // Point strictly left of forward and backward horizontal segments.
    const CoordinateXY pt{0.0, 5.0};
    RayCrossingCounter rcc_fwd(pt);
    rcc_fwd.countSegment(CoordinateXY{1.0, 5.0}, CoordinateXY{4.0, 5.0});
    EXPECT_FALSE(rcc_fwd.isOnSegment());
    EXPECT_EQ(rcc_fwd.getCount(), 0u);
    EXPECT_EQ(rcc_fwd.getLocation(), Location::EXTERIOR);

    RayCrossingCounter rcc_bwd(pt);
    rcc_bwd.countSegment(CoordinateXY{4.0, 5.0}, CoordinateXY{1.0, 5.0});
    EXPECT_FALSE(rcc_bwd.isOnSegment());
    EXPECT_EQ(rcc_bwd.getCount(), 0u);
    EXPECT_EQ(rcc_bwd.getLocation(), Location::EXTERIOR);
  }
}

TEST(RayCrossingCounterTest, CountSegmentNonHorizontalBranches) {
  const CoordinateXY pt{2.0, 2.0};

  {
    // Both endpoints above or below pt.y -> no crossing.
    RayCrossingCounter rcc(pt);
    rcc.countSegment(CoordinateXY{3.0, 3.0}, CoordinateXY{5.0, 6.0});
    rcc.countSegment(CoordinateXY{3.0, 0.0}, CoordinateXY{5.0, 1.0});
    // One endpoint on pt.y and the other below pt.y -> excluded.
    rcc.countSegment(CoordinateXY{4.0, 2.0}, CoordinateXY{5.0, 0.0});
    rcc.countSegment(CoordinateXY{5.0, 0.0}, CoordinateXY{4.0, 2.0});
    EXPECT_FALSE(rcc.isOnSegment());
    EXPECT_EQ(rcc.getCount(), 0u);
    EXPECT_EQ(rcc.getLocation(), Location::EXTERIOR);
  }

  {
    // Upward crossing to the right of pt.
    RayCrossingCounter rcc(pt);
    rcc.countSegment(CoordinateXY{4.0, 0.0}, CoordinateXY{6.0, 4.0});
    EXPECT_FALSE(rcc.isOnSegment());
    EXPECT_EQ(rcc.getCount(), 1u);
    EXPECT_EQ(rcc.getLocation(), Location::INTERIOR);
    EXPECT_TRUE(rcc.isPointInPolygon());
  }

  {
    // Upward segment crossing y = pt.y to the LEFT of pt while p2.x >= pt.x.
    RayCrossingCounter rcc(pt);
    rcc.countSegment(CoordinateXY{0.0, 0.0}, CoordinateXY{3.0, 5.0});
    EXPECT_FALSE(rcc.isOnSegment());
    EXPECT_EQ(rcc.getCount(), 0u);
    EXPECT_EQ(rcc.getLocation(), Location::EXTERIOR);
  }

  {
    // Downward crossing to the right of pt.
    RayCrossingCounter rcc(pt);
    rcc.countSegment(CoordinateXY{6.0, 4.0}, CoordinateXY{4.0, 0.0});
    EXPECT_FALSE(rcc.isOnSegment());
    EXPECT_EQ(rcc.getCount(), 1u);
    EXPECT_EQ(rcc.getLocation(), Location::INTERIOR);
  }

  {
    // Downward segment crossing y = pt.y to the LEFT of pt while p1.x >= pt.x.
    RayCrossingCounter rcc(pt);
    rcc.countSegment(CoordinateXY{3.0, 5.0}, CoordinateXY{0.0, 0.0});
    EXPECT_FALSE(rcc.isOnSegment());
    EXPECT_EQ(rcc.getCount(), 0u);
    EXPECT_EQ(rcc.getLocation(), Location::EXTERIOR);
  }

  {
    // Collinear point strictly inside diagonal upward and downward segments.
    RayCrossingCounter rcc_up(pt);
    rcc_up.countSegment(CoordinateXY{0.0, 0.0}, CoordinateXY{4.0, 4.0});
    EXPECT_TRUE(rcc_up.isOnSegment());
    EXPECT_EQ(rcc_up.getLocation(), Location::BOUNDARY);

    RayCrossingCounter rcc_down(pt);
    rcc_down.countSegment(CoordinateXY{4.0, 4.0}, CoordinateXY{0.0, 0.0});
    EXPECT_TRUE(rcc_down.isOnSegment());
    EXPECT_EQ(rcc_down.getLocation(), Location::BOUNDARY);
  }

  {
    // Edges touching y == pt.y at one endpoint and extending above pt.y are
    // counted on the right and ignored on the left.
    RayCrossingCounter rcc(pt);
    rcc.countSegment(CoordinateXY{4.0, 2.0}, CoordinateXY{5.0, 5.0});
    rcc.countSegment(CoordinateXY{5.0, 5.0}, CoordinateXY{4.0, 2.0});
    rcc.countSegment(CoordinateXY{1.0, 2.0}, CoordinateXY{0.0, 5.0});
    rcc.countSegment(CoordinateXY{0.0, 5.0}, CoordinateXY{1.0, 2.0});
    EXPECT_FALSE(rcc.isOnSegment());
    EXPECT_EQ(rcc.getCount(), 2u);
    EXPECT_EQ(rcc.getLocation(), Location::EXTERIOR);

    // Point equals starting endpoint p1 of an upward edge.
    RayCrossingCounter rcc_p1(pt);
    rcc_p1.countSegment(pt, CoordinateXY{4.0, 5.0});
    EXPECT_TRUE(rcc_p1.isOnSegment());
    EXPECT_EQ(rcc_p1.getLocation(), Location::BOUNDARY);
  }
}

TEST(RayCrossingCounterTest, CountSegmentExactOrientationAndNonFinitePoints) {
  const CoordinateXY p1{0.0, 0.0};
  const CoordinateXY p2{1e8, 1e8};

  {
    const CoordinateXY left_of_edge{5e7 - 1e-6, 5e7};
    RayCrossingCounter rcc(left_of_edge);
    rcc.countSegment(p1, p2);
    EXPECT_EQ(rcc.getLocation(), Location::INTERIOR);
    EXPECT_EQ(rcc.getCount(), 1u);
  }

  {
    const CoordinateXY right_of_edge{5e7 + 1e-6, 5e7};
    RayCrossingCounter rcc(right_of_edge);
    rcc.countSegment(p1, p2);
    EXPECT_EQ(rcc.getLocation(), Location::EXTERIOR);
    EXPECT_EQ(rcc.getCount(), 0u);
  }

  {
    // Sub-filter offset triggers double-double orientation fallback.
    const CoordinateXY near_left{5e7 - 1e-8, 5e7};
    RayCrossingCounter rcc_left(near_left);
    rcc_left.countSegment(p1, p2);
    EXPECT_EQ(rcc_left.getLocation(), Location::INTERIOR);
    EXPECT_EQ(rcc_left.getCount(), 1u);

    const CoordinateXY near_right{5e7 + 1e-8, 5e7};
    RayCrossingCounter rcc_right(near_right);
    rcc_right.countSegment(p1, p2);
    EXPECT_EQ(rcc_right.getLocation(), Location::EXTERIOR);
    EXPECT_EQ(rcc_right.getCount(), 0u);
  }

  {
    const CoordinateXY on_edge{5e7, 5e7};
    RayCrossingCounter rcc(on_edge);
    rcc.countSegment(p1, p2);
    EXPECT_EQ(rcc.getLocation(), Location::BOUNDARY);
    EXPECT_TRUE(rcc.isOnSegment());
  }

  {
    const CoordinateXY nan_pt{DoubleNotANumber, DoubleNotANumber};
    RayCrossingCounter rcc(nan_pt);
    rcc.countSegment(CoordinateXY{0.0, 0.0}, CoordinateXY{2.0, 2.0});
    EXPECT_EQ(rcc.getLocation(), Location::EXTERIOR);
    EXPECT_FALSE(rcc.isOnSegment());
  }

  {
    const CoordinateXY inf_pt{-DoubleInfinity, 0.0};
    RayCrossingCounter rcc(inf_pt);
    EXPECT_THROW(
        rcc.countSegment(CoordinateXY{1.0, -1.0}, CoordinateXY{1.0, 1.0}),
        geos::util::IllegalArgumentException);
  }
}

TEST(RayCrossingCounterTest, LocatePointInRingCoordinateSequence) {
  // Empty and single-point sequences are exterior.
  const CoordinateSequence empty_seq;
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{0.0, 0.0}, empty_seq),
      Location::EXTERIOR);

  const CoordinateSequence single_seq({CoordinateXY{1.0, 1.0}});
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{1.0, 1.0}, single_seq),
      Location::EXTERIOR);

  // Square ring in CCW and CW orderings.
  const CoordinateSequence ccw_square({
      CoordinateXY{0.0, 0.0},
      CoordinateXY{4.0, 0.0},
      CoordinateXY{4.0, 4.0},
      CoordinateXY{0.0, 4.0},
      CoordinateXY{0.0, 0.0},
  });
  CoordinateSequence cw_square(ccw_square);
  cw_square.reverse();

  for (const CoordinateSequence* sq :
       std::array<const CoordinateSequence*, 2>{&ccw_square, &cw_square}) {
    EXPECT_EQ(
        RayCrossingCounter::locatePointInRing(CoordinateXY{2.0, 2.0}, *sq),
        Location::INTERIOR);
    EXPECT_EQ(
        RayCrossingCounter::locatePointInRing(CoordinateXY{-1.0, 2.0}, *sq),
        Location::EXTERIOR);
    EXPECT_EQ(
        RayCrossingCounter::locatePointInRing(CoordinateXY{5.0, 2.0}, *sq),
        Location::EXTERIOR);
    EXPECT_EQ(
        RayCrossingCounter::locatePointInRing(CoordinateXY{2.0, 5.0}, *sq),
        Location::EXTERIOR);
    EXPECT_EQ(
        RayCrossingCounter::locatePointInRing(CoordinateXY{2.0, -1.0}, *sq),
        Location::EXTERIOR);

    // Vertices and edge midpoints (including early exit on first edge).
    EXPECT_EQ(
        RayCrossingCounter::locatePointInRing(CoordinateXY{0.0, 0.0}, *sq),
        Location::BOUNDARY);
    EXPECT_EQ(
        RayCrossingCounter::locatePointInRing(CoordinateXY{2.0, 0.0}, *sq),
        Location::BOUNDARY);
    EXPECT_EQ(
        RayCrossingCounter::locatePointInRing(CoordinateXY{4.0, 2.0}, *sq),
        Location::BOUNDARY);
    EXPECT_EQ(
        RayCrossingCounter::locatePointInRing(CoordinateXY{2.0, 4.0}, *sq),
        Location::BOUNDARY);
    EXPECT_EQ(
        RayCrossingCounter::locatePointInRing(CoordinateXY{0.0, 2.0}, *sq),
        Location::BOUNDARY);
  }

  // XYZ CoordinateSequence stores stride-3 Coordinates and projects XY.
  const CoordinateSequence xyz_square({
      Coordinate{0.0, 0.0, 10.0},
      Coordinate{4.0, 0.0, 20.0},
      Coordinate{4.0, 4.0, 30.0},
      Coordinate{0.0, 4.0, 40.0},
      Coordinate{0.0, 0.0, 10.0},
  });
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{2.0, 2.0}, xyz_square),
      Location::INTERIOR);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{5.0, 2.0}, xyz_square),
      Location::EXTERIOR);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{4.0, 2.0}, xyz_square),
      Location::BOUNDARY);
}

TEST(RayCrossingCounterTest, LocatePointInConcaveAndVertexAlignedRings) {
  // U-shaped polygon with horizontal notch edge at y = 2.
  const CoordinateSequence u_shape({
      CoordinateXY{0.0, 0.0},
      CoordinateXY{6.0, 0.0},
      CoordinateXY{6.0, 4.0},
      CoordinateXY{4.0, 4.0},
      CoordinateXY{4.0, 2.0},
      CoordinateXY{2.0, 2.0},
      CoordinateXY{2.0, 4.0},
      CoordinateXY{0.0, 4.0},
      CoordinateXY{0.0, 0.0},
  });

  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{-1.0, 3.0}, u_shape),
      Location::EXTERIOR);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{1.0, 3.0}, u_shape),
      Location::INTERIOR);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{3.0, 3.0}, u_shape),
      Location::EXTERIOR);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{5.0, 3.0}, u_shape),
      Location::INTERIOR);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{7.0, 3.0}, u_shape),
      Location::EXTERIOR);

  // Horizontal ray collinear with the notch edge at y = 2.
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{-1.0, 2.0}, u_shape),
      Location::EXTERIOR);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{1.0, 2.0}, u_shape),
      Location::INTERIOR);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{3.0, 2.0}, u_shape),
      Location::BOUNDARY);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{5.0, 2.0}, u_shape),
      Location::INTERIOR);

  // M-shaped polygon with valleys at y = 2 and peaks at y = 4.
  const CoordinateSequence m_shape({
      CoordinateXY{0.0, 0.0},
      CoordinateXY{6.0, 0.0},
      CoordinateXY{6.0, 4.0},
      CoordinateXY{4.0, 2.0},
      CoordinateXY{2.0, 4.0},
      CoordinateXY{1.0, 2.0},
      CoordinateXY{0.0, 4.0},
      CoordinateXY{0.0, 0.0},
  });

  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{-1.0, 2.0}, m_shape),
      Location::EXTERIOR);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{0.5, 2.0}, m_shape),
      Location::INTERIOR);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{1.0, 2.0}, m_shape),
      Location::BOUNDARY);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{2.5, 2.0}, m_shape),
      Location::INTERIOR);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{-1.0, 4.0}, m_shape),
      Location::EXTERIOR);

  // Incremental counting across shell + hole rings.
  const CoordinateSequence shell({
      CoordinateXY{0.0, 0.0},
      CoordinateXY{10.0, 0.0},
      CoordinateXY{10.0, 10.0},
      CoordinateXY{0.0, 10.0},
      CoordinateXY{0.0, 0.0},
  });
  const CoordinateSequence hole({
      CoordinateXY{2.0, 2.0},
      CoordinateXY{8.0, 2.0},
      CoordinateXY{8.0, 8.0},
      CoordinateXY{2.0, 8.0},
      CoordinateXY{2.0, 2.0},
  });

  const CoordinateXY in_shell_outside_hole{1.0, 5.0};
  RayCrossingCounter rcc_ring(in_shell_outside_hole);
  rcc_ring.processSequence(shell, /*isLinear=*/true);
  rcc_ring.processSequence(hole, /*isLinear=*/true);
  EXPECT_EQ(rcc_ring.getCount(), 3u);
  EXPECT_EQ(rcc_ring.getLocation(), Location::INTERIOR);

  const CoordinateXY inside_hole{5.0, 5.0};
  RayCrossingCounter rcc_hole(inside_hole);
  rcc_hole.processSequence(shell, /*isLinear=*/true);
  rcc_hole.processSequence(hole, /*isLinear=*/true);
  EXPECT_EQ(rcc_hole.getCount(), 2u);
  EXPECT_EQ(rcc_hole.getLocation(), Location::EXTERIOR);
}

TEST(RayCrossingCounterTest, LocatePointInRingCoordinatePointerVector) {
  const std::vector<const Coordinate*> empty_vec;
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{0.0, 0.0}, empty_vec),
      Location::EXTERIOR);

  const Coordinate p0{0.0, 0.0};
  const Coordinate p1{4.0, 0.0};
  const Coordinate p2{2.0, 4.0};
  const std::vector<const Coordinate*> single_vec{&p0};
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{0.0, 0.0}, single_vec),
      Location::EXTERIOR);

  const std::vector<const Coordinate*> triangle{&p0, &p1, &p2, &p0};
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{2.0, 1.0}, triangle),
      Location::INTERIOR);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{-1.0, 1.0}, triangle),
      Location::EXTERIOR);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{5.0, 1.0}, triangle),
      Location::EXTERIOR);
  // Early exit on first edge and vertex, plus hit on last edge.
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{2.0, 0.0}, triangle),
      Location::BOUNDARY);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{4.0, 0.0}, triangle),
      Location::BOUNDARY);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{1.0, 2.0}, triangle),
      Location::BOUNDARY);
}

TEST(RayCrossingCounterTest, ProcessSequenceEarlyReturnAndPreBoundaryGuard) {
  const CoordinateSequence triangle({
      CoordinateXY{0.0, 0.0},
      CoordinateXY{4.0, 0.0},
      CoordinateXY{2.0, 4.0},
      CoordinateXY{0.0, 0.0},
  });

  // Short-circuits on first linear segment before counting later segments.
  {
    const CoordinateXY pt{2.0, 0.0};
    RayCrossingCounter rcc(pt);
    rcc.processSequence(triangle, /*isLinear=*/true);
    EXPECT_TRUE(rcc.isOnSegment());
    EXPECT_EQ(rcc.getLocation(), Location::BOUNDARY);
    EXPECT_EQ(rcc.getCount(), 0u);

    // Subsequent call when isOnSegment() is already true returns immediately.
    rcc.processSequence(triangle, /*isLinear=*/false);
    EXPECT_TRUE(rcc.isOnSegment());
    EXPECT_EQ(rcc.getCount(), 0u);
  }

  // Curved sequence with fewer than 3 points performs zero arc steps.
  {
    const CoordinateSequence short_arc({
        CoordinateXY{0.0, 0.0},
        CoordinateXY{1.0, 1.0},
    });
    const CoordinateXY pt{0.5, 0.5};
    RayCrossingCounter rcc(pt);
    rcc.processSequence(short_arc, /*isLinear=*/false);
    EXPECT_FALSE(rcc.isOnSegment());
    EXPECT_EQ(rcc.getCount(), 0u);
    EXPECT_EQ(rcc.getLocation(), Location::EXTERIOR);
  }

  // Multi-arc sequence short-circuits on the first arc when point lies on it.
  {
    const CoordinateSequence two_arcs({
        CoordinateXY{5.0, 0.0},
        CoordinateXY{0.0, 5.0},
        CoordinateXY{-5.0, 0.0},
        CoordinateXY{0.0, -5.0},
        CoordinateXY{5.0, 0.0},
    });
    const CoordinateXY pt{3.0, 4.0};
    RayCrossingCounter rcc(pt);
    rcc.processSequence(two_arcs, /*isLinear=*/false);
    EXPECT_TRUE(rcc.isOnSegment());
    EXPECT_EQ(rcc.getLocation(), Location::BOUNDARY);
    EXPECT_EQ(rcc.getCount(), 0u);
  }
}

TEST(RayCrossingCounterTest, ShouldCountCrossingCircularArcOrientations) {
  // CCW upper semicircle: center (0,0), p0=(1,0) NE (upward), p2=(-1,0) NW
  // (downward).
  const CoordinateXY e_right{1.0, 0.0};
  const CoordinateXY e_top{0.0, 1.0};
  const CoordinateXY e_left{-1.0, 0.0};
  const CoordinateXY e_bottom{0.0, -1.0};

  const CircularArc ccw_upper(e_right, e_top, e_left);
  EXPECT_TRUE(RayCrossingCounter::shouldCountCrossing(ccw_upper, e_right));
  EXPECT_TRUE(RayCrossingCounter::shouldCountCrossing(ccw_upper, e_left));
  EXPECT_TRUE(RayCrossingCounter::shouldCountCrossing(ccw_upper, e_top));

  // CCW lower semicircle: p0=(-1,0) NW (downward), p2=(1,0) NE (upward).
  const CircularArc ccw_lower(e_left, e_bottom, e_right);
  EXPECT_FALSE(RayCrossingCounter::shouldCountCrossing(ccw_lower, e_left));
  EXPECT_FALSE(RayCrossingCounter::shouldCountCrossing(ccw_lower, e_right));
  EXPECT_TRUE(RayCrossingCounter::shouldCountCrossing(ccw_lower, e_bottom));

  // CW upper semicircle: p0=(-1,0) NW (upward), p2=(1,0) NE (downward).
  const CircularArc cw_upper(e_left, e_top, e_right);
  EXPECT_TRUE(RayCrossingCounter::shouldCountCrossing(cw_upper, e_left));
  EXPECT_TRUE(RayCrossingCounter::shouldCountCrossing(cw_upper, e_right));
  EXPECT_TRUE(RayCrossingCounter::shouldCountCrossing(
      cw_upper, CoordinateXY{std::sqrt(0.5), std::sqrt(0.5)}));

  // CW lower semicircle: p0=(1,0) NE (downward), p2=(-1,0) NW (upward).
  const CircularArc cw_lower(e_right, e_bottom, e_left);
  EXPECT_FALSE(RayCrossingCounter::shouldCountCrossing(cw_lower, e_right));
  EXPECT_FALSE(RayCrossingCounter::shouldCountCrossing(cw_lower, e_left));
}

TEST(RayCrossingCounterTest, PointsIntersectingHorizontalRay) {
  const CoordinateXY u0{5.0, 0.0};
  const CoordinateXY u1{0.0, 5.0};
  const CoordinateXY u2{-5.0, 0.0};
  const CircularArc upper_arc(u0, u1, u2);

  // Two intersections: ray starts to the left of upper arc.
  {
    auto pts = RayCrossingCounter::pointsIntersectingHorizontalRay(
        upper_arc, CoordinateXY{-6.0, 3.0});
    EXPECT_FALSE(pts[0].isNull());
    EXPECT_FALSE(pts[1].isNull());
    EXPECT_NEAR(pts[0].x, -4.0, 1e-12);
    EXPECT_DOUBLE_EQ(pts[0].y, 3.0);
    EXPECT_NEAR(pts[1].x, 4.0, 1e-12);
    EXPECT_DOUBLE_EQ(pts[1].y, 3.0);
  }

  // One intersection: ray originates inside circle.
  {
    auto pts = RayCrossingCounter::pointsIntersectingHorizontalRay(
        upper_arc, CoordinateXY{0.0, 3.0});
    EXPECT_FALSE(pts[0].isNull());
    EXPECT_TRUE(pts[1].isNull());
    EXPECT_NEAR(pts[0].x, 4.0, 1e-12);
    EXPECT_DOUBLE_EQ(pts[0].y, 3.0);
  }

  // One intersection when only one of the two circle intersections lies on arc.
  {
    const CoordinateXY r0{0.0, -5.0};
    const CoordinateXY r1{5.0, 0.0};
    const CoordinateXY r2{0.0, 5.0};
    const CircularArc right_arc(r0, r1, r2);
    auto right_pts = RayCrossingCounter::pointsIntersectingHorizontalRay(
        right_arc, CoordinateXY{-6.0, 3.0});
    EXPECT_FALSE(right_pts[0].isNull());
    EXPECT_TRUE(right_pts[1].isNull());
    EXPECT_NEAR(right_pts[0].x, 4.0, 1e-12);
    EXPECT_DOUBLE_EQ(right_pts[0].y, 3.0);

    const CoordinateXY l0{0.0, 5.0};
    const CoordinateXY l1{-5.0, 0.0};
    const CoordinateXY l2{0.0, -5.0};
    const CircularArc left_arc(l0, l1, l2);
    auto left_pts = RayCrossingCounter::pointsIntersectingHorizontalRay(
        left_arc, CoordinateXY{-6.0, 3.0});
    EXPECT_FALSE(left_pts[0].isNull());
    EXPECT_TRUE(left_pts[1].isNull());
    EXPECT_NEAR(left_pts[0].x, -4.0, 1e-12);
    EXPECT_DOUBLE_EQ(left_pts[0].y, 3.0);
  }

  // Zero intersections: ray to the right or in circle's excluded lower half.
  {
    auto pts_right = RayCrossingCounter::pointsIntersectingHorizontalRay(
        upper_arc, CoordinateXY{5.0, 3.0});
    EXPECT_TRUE(pts_right[0].isNull());
    EXPECT_TRUE(pts_right[1].isNull());

    auto pts_lower = RayCrossingCounter::pointsIntersectingHorizontalRay(
        upper_arc, CoordinateXY{-6.0, -3.0});
    EXPECT_TRUE(pts_lower[0].isNull());
    EXPECT_TRUE(pts_lower[1].isNull());
  }

  // Top tangent (dx == 0): touches p1 twice.
  {
    auto pts_tangent = RayCrossingCounter::pointsIntersectingHorizontalRay(
        upper_arc, CoordinateXY{-6.0, 5.0});
    EXPECT_FALSE(pts_tangent[0].isNull());
    EXPECT_FALSE(pts_tangent[1].isNull());
    EXPECT_TRUE(pts_tangent[0].equals2D(CoordinateXY{0.0, 5.0}));
    EXPECT_TRUE(pts_tangent[1].equals2D(CoordinateXY{0.0, 5.0}));
  }

  // Non-dyadic coordinates test endpoint snapping within eps = 1e-14.
  const CoordinateXY p0{0.3, 1.7};
  const CoordinateXY p1{1.3, 2.7};
  const CoordinateXY p2{2.3, 1.7};
  const CircularArc shifted_arc(p0, p1, p2);
  auto snapped = RayCrossingCounter::pointsIntersectingHorizontalRay(
      shifted_arc, CoordinateXY{-1.0, 1.7});
  EXPECT_TRUE(snapped[0].equals2D(p0));
  EXPECT_TRUE(snapped[1].equals2D(p2));
}

TEST(RayCrossingCounterTest, CountArcLinearEnvelopeAndBoundaryCases) {
  // Degenerate collinear arc delegates to countSegment(p1, p2) +
  // countSegment(p2, p3).
  {
    const CoordinateXY pt{0.0, 2.0};
    RayCrossingCounter rcc(pt);
    rcc.countArc(CoordinateXY{4.0, 0.0}, CoordinateXY{4.0, 2.0},
                 CoordinateXY{4.0, 4.0});
    EXPECT_FALSE(rcc.isOnSegment());
    EXPECT_EQ(rcc.getCount(), 1u);
    EXPECT_EQ(rcc.getLocation(), Location::INTERIOR);
  }

  {
    const CoordinateXY pt{1.0, 1.0};
    RayCrossingCounter rcc(pt);
    rcc.countArc(CoordinateXY{0.0, 0.0}, CoordinateXY{2.0, 2.0},
                 CoordinateXY{4.0, 4.0});
    EXPECT_TRUE(rcc.isOnSegment());
    EXPECT_EQ(rcc.getLocation(), Location::BOUNDARY);
  }

  // Envelope culling on X and Y bounds.
  const CoordinateXY a0{5.0, 0.0};
  const CoordinateXY a1{0.0, 5.0};
  const CoordinateXY a2{-5.0, 0.0};
  for (const CoordinateXY& outside :
       {CoordinateXY{6.0, 2.0}, CoordinateXY{0.0, 6.0},
        CoordinateXY{0.0, -1.0}}) {
    RayCrossingCounter rcc(outside);
    rcc.countArc(a0, a1, a2);
    EXPECT_FALSE(rcc.isOnSegment());
    EXPECT_EQ(rcc.getCount(), 0u);
  }

  // Interior arc point on exact circle (3^2 + 4^2 == 5^2) triggers BOUNDARY.
  for (const CoordinateXY& on_arc :
       {CoordinateXY{5.0, 0.0}, CoordinateXY{0.0, 5.0}, CoordinateXY{-5.0, 0.0},
        CoordinateXY{3.0, 4.0}, CoordinateXY{-3.0, 4.0}}) {
    RayCrossingCounter rcc(on_arc);
    rcc.countArc(a0, a1, a2);
    EXPECT_TRUE(rcc.isOnSegment());
    EXPECT_EQ(rcc.getLocation(), Location::BOUNDARY);
  }

  // Lower semicircle with ray hitting endpoints where shouldCountCrossing is
  // false.
  {
    const CoordinateXY pt{-6.0, 0.0};
    RayCrossingCounter rcc(pt);
    rcc.countArc(CoordinateXY{-5.0, 0.0}, CoordinateXY{0.0, -5.0},
                 CoordinateXY{5.0, 0.0});
    EXPECT_FALSE(rcc.isOnSegment());
    EXPECT_EQ(rcc.getCount(), 0u);
    EXPECT_EQ(rcc.getLocation(), Location::EXTERIOR);
  }

  // Single arc crossing (1 intersection) and double arc crossing (2
  // intersections).
  {
    const CoordinateXY pt_inside{0.0, 3.0};
    RayCrossingCounter rcc_one(pt_inside);
    rcc_one.countArc(a0, a1, a2);
    EXPECT_FALSE(rcc_one.isOnSegment());
    EXPECT_EQ(rcc_one.getCount(), 1u);
    EXPECT_EQ(rcc_one.getLocation(), Location::INTERIOR);

    const CoordinateXY pt_left{-6.0, 3.0};
    RayCrossingCounter rcc_two(pt_left);
    rcc_two.countArc(a0, a1, a2);
    EXPECT_FALSE(rcc_two.isOnSegment());
    EXPECT_EQ(rcc_two.getCount(), 2u);
    EXPECT_EQ(rcc_two.getLocation(), Location::EXTERIOR);
  }

  // CW 270-degree arc where intPts[0] counts and intPts[1] (p0 downward) does
  // not.
  {
    const CoordinateXY pt{-6.0, 0.0};
    RayCrossingCounter rcc(pt);
    rcc.countArc(CoordinateXY{5.0, 0.0}, CoordinateXY{-5.0, 0.0},
                 CoordinateXY{3.0, 4.0});
    EXPECT_FALSE(rcc.isOnSegment());
    EXPECT_EQ(rcc.getCount(), 1u);
    EXPECT_EQ(rcc.getLocation(), Location::INTERIOR);
  }
}

TEST(RayCrossingCounterTest, LocatePointInCurveRings) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Empty curves return EXTERIOR.
  std::unique_ptr<LineString> empty_ls =
      factory->createLineString(false, false);
  std::unique_ptr<CircularString> empty_cs =
      factory->createCircularString(false, false);
  std::unique_ptr<CompoundCurve> empty_cc = factory->createCompoundCurve();
  const CoordinateXY origin{0.0, 0.0};
  EXPECT_EQ(RayCrossingCounter::locatePointInRing(origin, *empty_ls),
            Location::EXTERIOR);
  EXPECT_EQ(RayCrossingCounter::locatePointInRing(origin, *empty_cs),
            Location::EXTERIOR);
  EXPECT_EQ(RayCrossingCounter::locatePointInRing(origin, *empty_cc),
            Location::EXTERIOR);

  // LinearRing passed through the Curve overload.
  std::unique_ptr<LinearRing> ring = factory->createLinearRing(
      CoordinateSequence({CoordinateXY{0.0, 0.0}, CoordinateXY{4.0, 0.0},
                          CoordinateXY{4.0, 4.0}, CoordinateXY{0.0, 4.0},
                          CoordinateXY{0.0, 0.0}}));
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{2.0, 2.0}, *ring),
      Location::INTERIOR);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{5.0, 2.0}, *ring),
      Location::EXTERIOR);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{2.0, 0.0}, *ring),
      Location::BOUNDARY);

  // Closed CCW and CW CircularString circles of radius 5 centered at (0, 0).
  const CoordinateSequence ccw_circle_coords({
      CoordinateXY{5.0, 0.0},
      CoordinateXY{0.0, 5.0},
      CoordinateXY{-5.0, 0.0},
      CoordinateXY{0.0, -5.0},
      CoordinateXY{5.0, 0.0},
  });
  CoordinateSequence cw_circle_coords(ccw_circle_coords);
  cw_circle_coords.reverse();

  std::unique_ptr<CircularString> ccw_circle =
      factory->createCircularString(ccw_circle_coords);
  std::unique_ptr<CircularString> cw_circle =
      factory->createCircularString(cw_circle_coords);

  for (const CircularString* circle : std::array<const CircularString*, 2>{
           ccw_circle.get(), cw_circle.get()}) {
    EXPECT_EQ(
        RayCrossingCounter::locatePointInRing(CoordinateXY{0.0, 0.0}, *circle),
        Location::INTERIOR);
    EXPECT_EQ(
        RayCrossingCounter::locatePointInRing(CoordinateXY{0.0, 3.0}, *circle),
        Location::INTERIOR);
    EXPECT_EQ(
        RayCrossingCounter::locatePointInRing(CoordinateXY{0.0, -3.0}, *circle),
        Location::INTERIOR);
    EXPECT_EQ(
        RayCrossingCounter::locatePointInRing(CoordinateXY{-6.0, 0.0}, *circle),
        Location::EXTERIOR);
    EXPECT_EQ(
        RayCrossingCounter::locatePointInRing(CoordinateXY{6.0, 0.0}, *circle),
        Location::EXTERIOR);
    EXPECT_EQ(
        RayCrossingCounter::locatePointInRing(CoordinateXY{-6.0, 3.0}, *circle),
        Location::EXTERIOR);
    EXPECT_EQ(
        RayCrossingCounter::locatePointInRing(CoordinateXY{3.0, 4.0}, *circle),
        Location::BOUNDARY);
    EXPECT_EQ(RayCrossingCounter::locatePointInRing(CoordinateXY{-3.0, -4.0},
                                                    *circle),
              Location::BOUNDARY);
    EXPECT_EQ(
        RayCrossingCounter::locatePointInRing(CoordinateXY{5.0, 0.0}, *circle),
        Location::BOUNDARY);
  }

  // Closed circle split at top (0,5) and bottom (0,-5) tests tangent vertices.
  std::unique_ptr<CircularString> vertical_split_circle =
      factory->createCircularString(CoordinateSequence({
          CoordinateXY{0.0, 5.0},
          CoordinateXY{-5.0, 0.0},
          CoordinateXY{0.0, -5.0},
          CoordinateXY{5.0, 0.0},
          CoordinateXY{0.0, 5.0},
      }));
  EXPECT_EQ(RayCrossingCounter::locatePointInRing(CoordinateXY{-6.0, 5.0},
                                                  *vertical_split_circle),
            Location::EXTERIOR);
  EXPECT_EQ(RayCrossingCounter::locatePointInRing(CoordinateXY{-6.0, -5.0},
                                                  *vertical_split_circle),
            Location::EXTERIOR);
  EXPECT_EQ(RayCrossingCounter::locatePointInRing(CoordinateXY{0.0, 0.0},
                                                  *vertical_split_circle),
            Location::INTERIOR);
}

TEST(RayCrossingCounterTest, LocatePointInCompoundCurveRings) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // D-shaped CompoundCurve: straight edge on left, semicircular arc on right.
  std::vector<std::unique_ptr<SimpleCurve>> d_parts;
  d_parts.push_back(factory->createLineString(
      CoordinateSequence({CoordinateXY{0.0, 4.0}, CoordinateXY{0.0, -4.0}})));
  d_parts.push_back(factory->createCircularString(
      CoordinateSequence({CoordinateXY{0.0, -4.0}, CoordinateXY{4.0, 0.0},
                          CoordinateXY{0.0, 4.0}})));
  std::unique_ptr<CompoundCurve> d_curve =
      factory->createCompoundCurve(std::move(d_parts));

  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{1.0, 0.0}, *d_curve),
      Location::INTERIOR);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{2.0, 2.0}, *d_curve),
      Location::INTERIOR);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{-1.0, 0.0}, *d_curve),
      Location::EXTERIOR);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{5.0, 0.0}, *d_curve),
      Location::EXTERIOR);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{3.0, 3.0}, *d_curve),
      Location::EXTERIOR);
  // Boundary on first section triggers isOnSegment() guard in second section.
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{0.0, 0.0}, *d_curve),
      Location::BOUNDARY);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{4.0, 0.0}, *d_curve),
      Location::BOUNDARY);

  // Crescent CompoundCurve built from two circular arcs.
  std::vector<std::unique_ptr<SimpleCurve>> crescent_parts;
  crescent_parts.push_back(factory->createCircularString(
      CoordinateSequence({CoordinateXY{0.0, 5.0}, CoordinateXY{5.0, 0.0},
                          CoordinateXY{0.0, -5.0}})));
  crescent_parts.push_back(factory->createCircularString(
      CoordinateSequence({CoordinateXY{0.0, -5.0}, CoordinateXY{2.0, 0.0},
                          CoordinateXY{0.0, 5.0}})));
  std::unique_ptr<CompoundCurve> crescent =
      factory->createCompoundCurve(std::move(crescent_parts));

  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{3.0, 0.0}, *crescent),
      Location::INTERIOR);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{1.0, 0.0}, *crescent),
      Location::EXTERIOR);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{-1.0, 0.0}, *crescent),
      Location::EXTERIOR);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{6.0, 0.0}, *crescent),
      Location::EXTERIOR);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{2.0, 0.0}, *crescent),
      Location::BOUNDARY);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{5.0, 0.0}, *crescent),
      Location::BOUNDARY);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{0.0, 5.0}, *crescent),
      Location::BOUNDARY);
  EXPECT_EQ(
      RayCrossingCounter::locatePointInRing(CoordinateXY{0.0, -5.0}, *crescent),
      Location::BOUNDARY);
}

}  // namespace
}  // namespace geos_google
