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

#include "third_party/geos/include/geos/noding/snapround/SnapRoundingIntersectionAdder.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/noding/MCIndexNoder.h"
#include "third_party/geos/include/geos/noding/NodedSegmentString.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"

namespace geos_google {
namespace {

using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateXY;
using ::geos::geom::CoordinateXYZM;
using ::geos::noding::MCIndexNoder;
using ::geos::noding::NodedSegmentString;
using ::geos::noding::SegmentString;
using ::geos::noding::snapround::SnapRoundingIntersectionAdder;

std::unique_ptr<NodedSegmentString> MakeSegmentStringXY(
    std::initializer_list<CoordinateXY> coords, const void* context = nullptr) {
  auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
  for (const CoordinateXY& c : coords) {
    seq->add(c);
  }
  return std::make_unique<NodedSegmentString>(seq.release(), false, false,
                                              context);
}

std::unique_ptr<NodedSegmentString> MakeSegmentStringXYZM(
    std::initializer_list<CoordinateXYZM> coords,
    const void* context = nullptr) {
  auto seq = std::make_unique<CoordinateSequence>(0u, true, true);
  for (const CoordinateXYZM& c : coords) {
    seq->add(c);
  }
  return std::make_unique<NodedSegmentString>(seq.release(), true, true,
                                              context);
}

TEST(SnapRoundingIntersectionAdderTest, InitialStateAndIsDone) {
  SnapRoundingIntersectionAdder adder(0.01);
  EXPECT_FALSE(adder.isDone());
  CoordinateSequence ints = adder.getIntersections();
  EXPECT_TRUE(ints.isEmpty());
  EXPECT_TRUE(ints.hasZ());
  EXPECT_TRUE(ints.hasM());
}

TEST(SnapRoundingIntersectionAdderTest, SkipsIdenticalSegment) {
  SnapRoundingIntersectionAdder adder(0.01);
  auto e0 = MakeSegmentStringXY({{0.0, 0.0}, {10.0, 10.0}});

  adder.processIntersections(e0.get(), 0, e0.get(), 0);

  EXPECT_FALSE(e0->hasNodes());
  EXPECT_EQ(adder.getIntersections().size(), 0u);
}

TEST(SnapRoundingIntersectionAdderTest, SameStringAdjacentAndCrossingSegments) {
  SnapRoundingIntersectionAdder adder(0.01);
  auto poly =
      MakeSegmentStringXY({{0.0, 0.0}, {10.0, 10.0}, {10.0, 0.0}, {0.0, 10.0}});

  // Adjacent segments (0 and 1) only touch at shared vertex (10, 10).
  adder.processIntersections(poly.get(), 0, poly.get(), 1);
  EXPECT_FALSE(poly->hasNodes());
  EXPECT_EQ(adder.getIntersections().size(), 0u);

  // Non-adjacent segments (0 and 2) cross in their interior at (5, 5).
  adder.processIntersections(poly.get(), 0, poly.get(), 2);
  EXPECT_TRUE(poly->hasNodes());

  CoordinateSequence ints = adder.getIntersections();
  ASSERT_EQ(ints.size(), 1u);
  EXPECT_TRUE(ints.getAt<CoordinateXY>(0).equals2D(CoordinateXY(5.0, 5.0)));

  auto noded = poly->getNodedCoordinates();
  ASSERT_EQ(noded->size(), 6u);
  EXPECT_TRUE(noded->getAt<CoordinateXY>(1).equals2D(CoordinateXY(5.0, 5.0)));
  EXPECT_TRUE(noded->getAt<CoordinateXY>(4).equals2D(CoordinateXY(5.0, 5.0)));
}

TEST(SnapRoundingIntersectionAdderTest, ProperCrossingInterpolatesZAndM) {
  SnapRoundingIntersectionAdder adder(0.01);
  auto e0 = MakeSegmentStringXYZM(
      {{0.0, 0.0, 10.0, 100.0}, {10.0, 10.0, 20.0, 200.0}});
  auto e1 = MakeSegmentStringXYZM(
      {{0.0, 10.0, 30.0, 300.0}, {10.0, 0.0, 40.0, 400.0}});

  adder.processIntersections(e0.get(), 0, e1.get(), 0);

  EXPECT_TRUE(e0->hasNodes());
  EXPECT_TRUE(e1->hasNodes());

  CoordinateSequence ints = adder.getIntersections();
  ASSERT_EQ(ints.size(), 1u);
  CoordinateXYZM pt;
  ints.getAt(0, pt);
  EXPECT_DOUBLE_EQ(pt.x, 5.0);
  EXPECT_DOUBLE_EQ(pt.y, 5.0);
  EXPECT_DOUBLE_EQ(pt.z, 25.0);
  EXPECT_DOUBLE_EQ(pt.m, 250.0);

  auto noded0 = e0->getNodedCoordinates();
  auto noded1 = e1->getNodedCoordinates();
  ASSERT_EQ(noded0->size(), 3u);
  ASSERT_EQ(noded1->size(), 3u);
  EXPECT_TRUE(noded0->getAt<CoordinateXY>(1).equals2D(CoordinateXY(5.0, 5.0)));
  EXPECT_TRUE(noded1->getAt<CoordinateXY>(1).equals2D(CoordinateXY(5.0, 5.0)));
}

TEST(SnapRoundingIntersectionAdderTest, TIntersectionNodesInteriorSegment) {
  SnapRoundingIntersectionAdder adder(0.01);
  auto e0 = MakeSegmentStringXY({{0.0, 5.0}, {10.0, 5.0}});
  auto e1 = MakeSegmentStringXY({{5.0, 5.0}, {5.0, 10.0}});

  adder.processIntersections(e0.get(), 0, e1.get(), 0);

  CoordinateSequence ints = adder.getIntersections();
  ASSERT_EQ(ints.size(), 1u);
  EXPECT_TRUE(ints.getAt<CoordinateXY>(0).equals2D(CoordinateXY(5.0, 5.0)));

  auto noded0 = e0->getNodedCoordinates();
  ASSERT_EQ(noded0->size(), 3u);
  EXPECT_TRUE(noded0->getAt<CoordinateXY>(1).equals2D(CoordinateXY(5.0, 5.0)));
}

TEST(SnapRoundingIntersectionAdderTest, EndpointOnlyTouchAddsNoIntersection) {
  SnapRoundingIntersectionAdder adder(0.01);
  auto e0 = MakeSegmentStringXY({{0.0, 0.0}, {10.0, 0.0}});
  auto e1 = MakeSegmentStringXY({{10.0, 0.0}, {10.0, 10.0}});

  adder.processIntersections(e0.get(), 0, e1.get(), 0);

  EXPECT_FALSE(e0->hasNodes());
  EXPECT_FALSE(e1->hasNodes());
  EXPECT_EQ(adder.getIntersections().size(), 0u);
}

TEST(SnapRoundingIntersectionAdderTest, CollinearOverlapAddsTwoIntersections) {
  SnapRoundingIntersectionAdder adder(0.01);
  auto e0 = MakeSegmentStringXY({{0.0, 0.0}, {10.0, 0.0}});
  auto e1 = MakeSegmentStringXY({{3.0, 0.0}, {7.0, 0.0}});

  adder.processIntersections(e0.get(), 0, e1.get(), 0);

  CoordinateSequence ints = adder.getIntersections();
  ASSERT_EQ(ints.size(), 2u);
  EXPECT_TRUE(ints.getAt<CoordinateXY>(0).equals2D(CoordinateXY(3.0, 0.0)));
  EXPECT_TRUE(ints.getAt<CoordinateXY>(1).equals2D(CoordinateXY(7.0, 0.0)));

  auto noded0 = e0->getNodedCoordinates();
  ASSERT_EQ(noded0->size(), 4u);
  EXPECT_TRUE(noded0->getAt<CoordinateXY>(1).equals2D(CoordinateXY(3.0, 0.0)));
  EXPECT_TRUE(noded0->getAt<CoordinateXY>(2).equals2D(CoordinateXY(7.0, 0.0)));
}

TEST(SnapRoundingIntersectionAdderTest,
     DisjointSegmentsBeyondToleranceAddNone) {
  SnapRoundingIntersectionAdder adder(0.01);
  auto e0 = MakeSegmentStringXY({{0.0, 0.0}, {10.0, 0.0}});
  auto e1 = MakeSegmentStringXY({{5.0, 0.02}, {5.0, 5.0}});

  adder.processIntersections(e0.get(), 0, e1.get(), 0);

  EXPECT_FALSE(e0->hasNodes());
  EXPECT_FALSE(e1->hasNodes());
  EXPECT_EQ(adder.getIntersections().size(), 0u);
}

TEST(SnapRoundingIntersectionAdderTest, ExactToleranceBoundaryIsNotNear) {
  SnapRoundingIntersectionAdder adder(0.01);
  auto e0 = MakeSegmentStringXY({{0.0, 0.0}, {10.0, 0.0}});
  auto e1 = MakeSegmentStringXY({{5.0, 0.01}, {5.0, 5.0}});

  adder.processIntersections(e0.get(), 0, e1.get(), 0);

  EXPECT_FALSE(e0->hasNodes());
  EXPECT_FALSE(e1->hasNodes());
  EXPECT_EQ(adder.getIntersections().size(), 0u);
}

TEST(SnapRoundingIntersectionAdderTest, NearSegmentEndpointsAreRejected) {
  constexpr double kTol = 0.01;
  auto base = MakeSegmentStringXY({{0.0, 0.0}, {10.0, 0.0}});

  // Within tolerance of base start (0, 0): rejected by p.distance(p0) < tol.
  {
    SnapRoundingIntersectionAdder adder(kTol);
    auto near_start = MakeSegmentStringXY({{0.002, 0.001}, {0.002, 5.0}});
    adder.processIntersections(base.get(), 0, near_start.get(), 0);
    EXPECT_FALSE(base->hasNodes());
    EXPECT_FALSE(near_start->hasNodes());
    EXPECT_EQ(adder.getIntersections().size(), 0u);
  }

  // Far from base start, within tolerance of base end (10, 0): rejected by
  // p.distance(p1) < tol.
  {
    SnapRoundingIntersectionAdder adder(kTol);
    auto near_end = MakeSegmentStringXY({{9.998, 0.001}, {9.998, 5.0}});
    adder.processIntersections(base.get(), 0, near_end.get(), 0);
    EXPECT_FALSE(base->hasNodes());
    EXPECT_FALSE(near_end->hasNodes());
    EXPECT_EQ(adder.getIntersections().size(), 0u);
  }
}

TEST(SnapRoundingIntersectionAdderTest, NearVertexInteriorAllFourEndpoints) {
  constexpr double kTol = 0.01;

  // 1. seq0[segIndex0] near interior of e1 (preserves Z/M of near vertex).
  {
    SnapRoundingIntersectionAdder adder(kTol);
    auto e0 = MakeSegmentStringXYZM(
        {{5.0, 0.002, 11.0, 22.0}, {5.0, 5.0, 15.0, 25.0}});
    auto e1 =
        MakeSegmentStringXYZM({{0.0, 0.0, 0.0, 0.0}, {10.0, 0.0, 0.0, 0.0}});
    adder.processIntersections(e0.get(), 0, e1.get(), 0);

    EXPECT_FALSE(e0->hasNodes());
    EXPECT_TRUE(e1->hasNodes());
    CoordinateSequence ints = adder.getIntersections();
    ASSERT_EQ(ints.size(), 1u);
    CoordinateXYZM pt;
    ints.getAt(0, pt);
    EXPECT_DOUBLE_EQ(pt.x, 5.0);
    EXPECT_DOUBLE_EQ(pt.y, 0.002);
    EXPECT_DOUBLE_EQ(pt.z, 11.0);
    EXPECT_DOUBLE_EQ(pt.m, 22.0);
    EXPECT_EQ(e1->getNodedCoordinates()->size(), 3u);
  }

  // 2. seq0[segIndex0 + 1] near interior of e1 (using 2D XY sequence).
  {
    SnapRoundingIntersectionAdder adder(kTol);
    auto e0 = MakeSegmentStringXY({{5.0, 5.0}, {5.0, 0.002}});
    auto e1 = MakeSegmentStringXY({{0.0, 0.0}, {10.0, 0.0}});
    adder.processIntersections(e0.get(), 0, e1.get(), 0);

    EXPECT_FALSE(e0->hasNodes());
    EXPECT_TRUE(e1->hasNodes());
    CoordinateSequence ints = adder.getIntersections();
    ASSERT_EQ(ints.size(), 1u);
    EXPECT_TRUE(ints.getAt<CoordinateXY>(0).equals2D(CoordinateXY(5.0, 0.002)));
    EXPECT_TRUE(std::isnan(ints.getOrdinate(0, CoordinateSequence::Z)));
    EXPECT_TRUE(std::isnan(ints.getOrdinate(0, CoordinateSequence::M)));
    EXPECT_EQ(e1->getNodedCoordinates()->size(), 3u);
  }

  // 3. seq1[segIndex1] near interior of e0 (using non-zero segIndex0/1).
  {
    SnapRoundingIntersectionAdder adder(kTol);
    auto e0 = MakeSegmentStringXY({{0.0, -5.0}, {0.0, 0.0}, {10.0, 0.0}});
    auto e1 = MakeSegmentStringXY({{5.0, 10.0}, {5.0, 0.002}, {5.0, 5.0}});
    adder.processIntersections(e0.get(), 1, e1.get(), 1);

    EXPECT_TRUE(e0->hasNodes());
    EXPECT_FALSE(e1->hasNodes());
    CoordinateSequence ints = adder.getIntersections();
    ASSERT_EQ(ints.size(), 1u);
    EXPECT_TRUE(ints.getAt<CoordinateXY>(0).equals2D(CoordinateXY(5.0, 0.002)));
    auto noded0 = e0->getNodedCoordinates();
    ASSERT_EQ(noded0->size(), 4u);
    EXPECT_TRUE(
        noded0->getAt<CoordinateXY>(2).equals2D(CoordinateXY(5.0, 0.002)));
  }

  // 4. seq1[segIndex1 + 1] near interior of e0.
  {
    SnapRoundingIntersectionAdder adder(kTol);
    auto e0 = MakeSegmentStringXY({{0.0, 0.0}, {10.0, 0.0}});
    auto e1 = MakeSegmentStringXY({{5.0, 5.0}, {5.0, 0.002}});
    adder.processIntersections(e0.get(), 0, e1.get(), 0);

    EXPECT_TRUE(e0->hasNodes());
    EXPECT_FALSE(e1->hasNodes());
    CoordinateSequence ints = adder.getIntersections();
    ASSERT_EQ(ints.size(), 1u);
    EXPECT_TRUE(ints.getAt<CoordinateXY>(0).equals2D(CoordinateXY(5.0, 0.002)));
    EXPECT_EQ(e0->getNodedCoordinates()->size(), 3u);
  }
}

TEST(SnapRoundingIntersectionAdderTest,
     ZeroToleranceDisablesNearVertexHeuristic) {
  SnapRoundingIntersectionAdder adder(0.0);
  auto e0 = MakeSegmentStringXY({{0.0, 0.0}, {10.0, 0.0}});
  auto near = MakeSegmentStringXY({{5.0, 1e-9}, {5.0, 5.0}});
  auto cross = MakeSegmentStringXY({{2.0, -2.0}, {2.0, 2.0}});

  adder.processIntersections(e0.get(), 0, near.get(), 0);
  EXPECT_FALSE(e0->hasNodes());
  EXPECT_EQ(adder.getIntersections().size(), 0u);

  adder.processIntersections(e0.get(), 0, cross.get(), 0);
  EXPECT_TRUE(e0->hasNodes());
  EXPECT_TRUE(cross->hasNodes());
  CoordinateSequence ints = adder.getIntersections();
  ASSERT_EQ(ints.size(), 1u);
  EXPECT_TRUE(ints.getAt<CoordinateXY>(0).equals2D(CoordinateXY(2.0, 0.0)));
}

TEST(SnapRoundingIntersectionAdderTest, IntegratesWithMCIndexNoder) {
  constexpr double kTol = 0.01;
  SnapRoundingIntersectionAdder adder(kTol);
  MCIndexNoder noder(&adder, kTol);

  auto e0 = MakeSegmentStringXY({{0.0, 0.0}, {10.0, 0.0}});
  auto e1 = MakeSegmentStringXY({{5.0, 0.005}, {5.0, 10.0}});
  auto e2 = MakeSegmentStringXY({{2.0, -5.0}, {2.0, 5.0}});
  std::vector<SegmentString*> inputs{e0.get(), e1.get(), e2.get()};

  noder.computeNodes(&inputs);

  CoordinateSequence ints = adder.getIntersections();
  EXPECT_EQ(ints.size(), 2u);
  auto noded0 = e0->getNodedCoordinates();
  ASSERT_EQ(noded0->size(), 4u);
  EXPECT_TRUE(noded0->getAt<CoordinateXY>(1).equals2D(CoordinateXY(2.0, 0.0)));
  EXPECT_TRUE(
      noded0->getAt<CoordinateXY>(2).equals2D(CoordinateXY(5.0, 0.005)));
}

}  // namespace
}  // namespace geos_google
