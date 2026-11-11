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

// Unit tests for third_party/geos/src/noding/IntersectionAdder.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/noding/IntersectionAdder.cpp

#include "third_party/geos/include/geos/noding/IntersectionAdder.h"

#include <cstddef>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/noding/NodedSegmentString.h"
#include "third_party/geos/include/geos/noding/SegmentNodeList.h"

namespace geos_google {
namespace {

using geos::algorithm::LineIntersector;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYZM;
using geos::geom::PrecisionModel;
using geos::noding::IntersectionAdder;
using geos::noding::NodedSegmentString;

std::unique_ptr<NodedSegmentString> MakeNodedSegmentString(
    const std::vector<CoordinateXY>& coords) {
  auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
  seq->reserve(coords.size());
  for (const CoordinateXY& c : coords) {
    seq->add(c);
  }
  return std::make_unique<NodedSegmentString>(seq.release(), false, false,
                                              nullptr);
}

std::unique_ptr<NodedSegmentString> MakeNodedSegmentStringXYZM(
    const std::vector<CoordinateXYZM>& coords, bool construct_z,
    bool construct_m) {
  auto seq = std::make_unique<CoordinateSequence>(0u, construct_z, construct_m);
  seq->reserve(coords.size());
  for (const CoordinateXYZM& c : coords) {
    seq->add(c);
  }
  return std::make_unique<NodedSegmentString>(seq.release(), construct_z,
                                              construct_m, nullptr);
}

TEST(IntersectionAdderTest, InitialStateAndAccessors) {
  LineIntersector li;
  IntersectionAdder adder(li);

  EXPECT_EQ(&adder.getLineIntersector(), &li);
  EXPECT_FALSE(adder.hasIntersection());
  EXPECT_FALSE(adder.hasProperIntersection());
  EXPECT_FALSE(adder.hasProperInteriorIntersection());
  EXPECT_FALSE(adder.hasInteriorIntersection());
  EXPECT_FALSE(adder.isDone());

  EXPECT_EQ(adder.numIntersections, 0);
  EXPECT_EQ(adder.numInteriorIntersections, 0);
  EXPECT_EQ(adder.numProperIntersections, 0);
  EXPECT_EQ(adder.numTests, 0);

  EXPECT_TRUE(IntersectionAdder::isAdjacentSegments(0, 1));
  EXPECT_TRUE(IntersectionAdder::isAdjacentSegments(1, 0));
  EXPECT_TRUE(IntersectionAdder::isAdjacentSegments(5, 4));
  EXPECT_FALSE(IntersectionAdder::isAdjacentSegments(0, 0));
  EXPECT_FALSE(IntersectionAdder::isAdjacentSegments(0, 2));
  EXPECT_FALSE(IntersectionAdder::isAdjacentSegments(2, 0));
}

TEST(IntersectionAdderTest, SameSegmentSelfTestIsSkipped) {
  LineIntersector li;
  IntersectionAdder adder(li);
  auto ss = MakeNodedSegmentString({{0.0, 0.0}, {10.0, 10.0}});

  adder.processIntersections(ss.get(), 0, ss.get(), 0);

  EXPECT_EQ(adder.numTests, 0);
  EXPECT_EQ(adder.numIntersections, 0);
  EXPECT_FALSE(adder.hasIntersection());
  EXPECT_FALSE(ss->hasNodes());
}

TEST(IntersectionAdderTest, DisjointSegmentsIncrementTestsOnly) {
  LineIntersector li;
  IntersectionAdder adder(li);
  auto ss0 = MakeNodedSegmentString({{0.0, 0.0}, {5.0, 0.0}});
  auto ss1 = MakeNodedSegmentString({{0.0, 5.0}, {5.0, 5.0}});

  adder.processIntersections(ss0.get(), 0, ss1.get(), 0);

  EXPECT_EQ(adder.numTests, 1);
  EXPECT_EQ(adder.numIntersections, 0);
  EXPECT_EQ(adder.numInteriorIntersections, 0);
  EXPECT_EQ(adder.numProperIntersections, 0);
  EXPECT_FALSE(adder.hasIntersection());
  EXPECT_FALSE(adder.hasInteriorIntersection());
  EXPECT_FALSE(adder.hasProperIntersection());
  EXPECT_FALSE(adder.hasProperInteriorIntersection());
  EXPECT_FALSE(ss0->hasNodes());
  EXPECT_FALSE(ss1->hasNodes());
}

TEST(IntersectionAdderTest, ProperCrossingBetweenDistinctStrings) {
  LineIntersector li;
  IntersectionAdder adder(li);
  auto ss0 = MakeNodedSegmentString({{0.0, 0.0}, {10.0, 10.0}});
  auto ss1 = MakeNodedSegmentString({{0.0, 10.0}, {10.0, 0.0}});

  adder.processIntersections(ss0.get(), 0, ss1.get(), 0);

  EXPECT_EQ(adder.numTests, 1);
  EXPECT_EQ(adder.numIntersections, 1);
  EXPECT_EQ(adder.numInteriorIntersections, 1);
  EXPECT_EQ(adder.numProperIntersections, 1);
  EXPECT_TRUE(adder.hasIntersection());
  EXPECT_TRUE(adder.hasInteriorIntersection());
  EXPECT_TRUE(adder.hasProperIntersection());
  EXPECT_TRUE(adder.hasProperInteriorIntersection());
  EXPECT_TRUE(
      adder.getProperIntersectionPoint().equals2D(CoordinateXY(5.0, 5.0)));

  EXPECT_TRUE(ss0->hasNodes());
  EXPECT_TRUE(ss1->hasNodes());
  EXPECT_EQ(ss0->getNodeList().size(), 1u);
  EXPECT_EQ(ss1->getNodeList().size(), 1u);
  EXPECT_TRUE(
      ss0->getNodeList().begin()->coord.equals2D(CoordinateXY(5.0, 5.0)));

  auto coords0 = ss0->getNodedCoordinates();
  ASSERT_EQ(coords0->size(), 3u);
  EXPECT_TRUE(coords0->getAt<CoordinateXY>(1).equals2D(CoordinateXY(5.0, 5.0)));

  auto coords1 = ss1->getNodedCoordinates();
  ASSERT_EQ(coords1->size(), 3u);
  EXPECT_TRUE(coords1->getAt<CoordinateXY>(1).equals2D(CoordinateXY(5.0, 5.0)));
}

TEST(IntersectionAdderTest, ProperCrossingInterpolatesZMAndAppliesPrecision) {
  {
    LineIntersector li;
    IntersectionAdder adder(li);
    auto ss0 = MakeNodedSegmentStringXYZM(
        {{0.0, 0.0, 0.0, 10.0}, {10.0, 10.0, 10.0, 20.0}}, true, true);
    auto ss1 = MakeNodedSegmentStringXYZM(
        {{0.0, 10.0, 20.0, 30.0}, {10.0, 0.0, 20.0, 40.0}}, true, true);

    adder.processIntersections(ss0.get(), 0, ss1.get(), 0);

    ASSERT_TRUE(adder.hasProperIntersection());
    const CoordinateXYZM& pt = adder.getProperIntersectionPoint();
    EXPECT_DOUBLE_EQ(pt.x, 5.0);
    EXPECT_DOUBLE_EQ(pt.y, 5.0);
    EXPECT_DOUBLE_EQ(pt.z, 12.5);
    EXPECT_DOUBLE_EQ(pt.m, 25.0);
  }

  {
    PrecisionModel fixed_pm(1.0);
    LineIntersector li(&fixed_pm);
    IntersectionAdder adder(li);
    auto ss0 = MakeNodedSegmentString({{0.0, 0.0}, {10.0, 3.0}});
    auto ss1 = MakeNodedSegmentString({{0.0, 2.0}, {10.0, 0.0}});

    adder.processIntersections(ss0.get(), 0, ss1.get(), 0);

    ASSERT_TRUE(adder.hasProperIntersection());
    EXPECT_TRUE(
        adder.getProperIntersectionPoint().equals2D(CoordinateXY(4.0, 1.0)));
  }
}

TEST(IntersectionAdderTest,
     EndpointTouchBetweenDistinctStringsAddsNodesWithoutInteriorFlag) {
  LineIntersector li;
  IntersectionAdder adder(li);
  auto ss0 = MakeNodedSegmentString({{0.0, 0.0}, {5.0, 0.0}});
  auto ss1 = MakeNodedSegmentString({{5.0, 0.0}, {10.0, 0.0}});

  adder.processIntersections(ss0.get(), 0, ss1.get(), 0);

  EXPECT_EQ(adder.numTests, 1);
  EXPECT_EQ(adder.numIntersections, 1);
  EXPECT_EQ(adder.numInteriorIntersections, 0);
  EXPECT_EQ(adder.numProperIntersections, 0);
  EXPECT_TRUE(adder.hasIntersection());
  EXPECT_FALSE(adder.hasInteriorIntersection());
  EXPECT_FALSE(adder.hasProperIntersection());
  EXPECT_FALSE(adder.hasProperInteriorIntersection());
  EXPECT_TRUE(ss0->hasNodes());
  EXPECT_TRUE(ss1->hasNodes());
}

TEST(IntersectionAdderTest, TJunctionInteriorNonProperIntersection) {
  LineIntersector li;
  IntersectionAdder adder(li);
  auto ss0 = MakeNodedSegmentString({{0.0, 0.0}, {10.0, 0.0}});
  auto ss1 = MakeNodedSegmentString({{5.0, 0.0}, {5.0, 5.0}});

  adder.processIntersections(ss0.get(), 0, ss1.get(), 0);

  EXPECT_EQ(adder.numTests, 1);
  EXPECT_EQ(adder.numIntersections, 1);
  EXPECT_EQ(adder.numInteriorIntersections, 1);
  EXPECT_EQ(adder.numProperIntersections, 0);
  EXPECT_TRUE(adder.hasIntersection());
  EXPECT_TRUE(adder.hasInteriorIntersection());
  EXPECT_FALSE(adder.hasProperIntersection());
  EXPECT_FALSE(adder.hasProperInteriorIntersection());

  auto coords0 = ss0->getNodedCoordinates();
  ASSERT_EQ(coords0->size(), 3u);
  EXPECT_TRUE(coords0->getAt<CoordinateXY>(1).equals2D(CoordinateXY(5.0, 0.0)));
}

TEST(IntersectionAdderTest, CollinearOverlapBetweenDistinctStrings) {
  LineIntersector li;
  IntersectionAdder adder(li);
  auto ss0 = MakeNodedSegmentString({{0.0, 0.0}, {10.0, 0.0}});
  auto ss1 = MakeNodedSegmentString({{5.0, 0.0}, {15.0, 0.0}});

  adder.processIntersections(ss0.get(), 0, ss1.get(), 0);

  EXPECT_EQ(adder.numTests, 1);
  EXPECT_EQ(adder.numIntersections, 1);
  EXPECT_EQ(adder.numInteriorIntersections, 1);
  EXPECT_EQ(adder.numProperIntersections, 0);
  EXPECT_TRUE(adder.hasIntersection());
  EXPECT_TRUE(adder.hasInteriorIntersection());
  EXPECT_FALSE(adder.hasProperIntersection());
  EXPECT_TRUE(ss0->hasNodes());
  EXPECT_TRUE(ss1->hasNodes());
}

TEST(IntersectionAdderTest, AdjacentSameStringEndpointTouchIsTrivial) {
  LineIntersector li;
  IntersectionAdder adder(li);
  auto ss = MakeNodedSegmentString({{0.0, 0.0}, {5.0, 0.0}, {5.0, 5.0}});

  adder.processIntersections(ss.get(), 0, ss.get(), 1);
  adder.processIntersections(ss.get(), 1, ss.get(), 0);

  EXPECT_EQ(adder.numTests, 2);
  EXPECT_EQ(adder.numIntersections, 2);
  EXPECT_EQ(adder.numInteriorIntersections, 0);
  EXPECT_EQ(adder.numProperIntersections, 0);
  EXPECT_FALSE(adder.hasIntersection());
  EXPECT_FALSE(adder.hasInteriorIntersection());
  EXPECT_FALSE(adder.hasProperIntersection());
  EXPECT_FALSE(ss->hasNodes());
}

TEST(IntersectionAdderTest, AdjacentCollinearFoldbackOnSameStringIsNotTrivial) {
  LineIntersector li;
  IntersectionAdder adder(li);
  auto ss = MakeNodedSegmentString({{0.0, 0.0}, {10.0, 0.0}, {5.0, 0.0}});

  adder.processIntersections(ss.get(), 0, ss.get(), 1);

  EXPECT_EQ(adder.numTests, 1);
  EXPECT_EQ(adder.numIntersections, 1);
  EXPECT_EQ(adder.numInteriorIntersections, 1);
  EXPECT_EQ(adder.numProperIntersections, 0);
  EXPECT_TRUE(adder.hasIntersection());
  EXPECT_TRUE(adder.hasInteriorIntersection());
  EXPECT_FALSE(adder.hasProperIntersection());
  EXPECT_TRUE(ss->hasNodes());
  EXPECT_EQ(ss->getNodeList().size(), 3u);
}

TEST(IntersectionAdderTest,
     NonAdjacentSelfIntersectionOnOpenStringIsNotTrivial) {
  LineIntersector li;
  IntersectionAdder adder(li);
  auto ss = MakeNodedSegmentString(
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 5.0}, {5.0, 0.0}});
  ASSERT_FALSE(ss->isClosed());

  adder.processIntersections(ss.get(), 0, ss.get(), 2);

  EXPECT_EQ(adder.numTests, 1);
  EXPECT_EQ(adder.numIntersections, 1);
  EXPECT_EQ(adder.numInteriorIntersections, 1);
  EXPECT_EQ(adder.numProperIntersections, 0);
  EXPECT_TRUE(adder.hasIntersection());
  EXPECT_TRUE(adder.hasInteriorIntersection());
  EXPECT_FALSE(adder.hasProperIntersection());
  EXPECT_TRUE(ss->hasNodes());
}

TEST(IntersectionAdderTest, ClosedRingNonAdjacentSegmentsReachClosedCheck) {
  // Note: IntersectionAdder::isTrivialIntersection computes
  // maxSegIndex = e0->size() - 1 (vertex count - 1) rather than segment count
  // - 1, so valid closing segment index (size() - 2) returns false and is
  // treated as non-trivial when checked directly.
  {
    LineIntersector li;
    IntersectionAdder adder(li);
    auto ring = MakeNodedSegmentString(
        {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});
    ASSERT_TRUE(ring->isClosed());

    adder.processIntersections(ring.get(), 0, ring.get(), 3);
    adder.processIntersections(ring.get(), 3, ring.get(), 0);

    EXPECT_EQ(adder.numTests, 2);
    EXPECT_EQ(adder.numIntersections, 2);
    EXPECT_EQ(adder.numInteriorIntersections, 0);
    EXPECT_EQ(adder.numProperIntersections, 0);
    EXPECT_TRUE(adder.hasIntersection());
    EXPECT_FALSE(adder.hasInteriorIntersection());
    EXPECT_FALSE(adder.hasProperIntersection());
    EXPECT_TRUE(ring->hasNodes());
    EXPECT_EQ(ring->getNodeList().size(), 2u);
  }

  // Non-adjacent interior crossing where neither segment index is 0.
  {
    LineIntersector li;
    IntersectionAdder adder(li);
    auto ring = MakeNodedSegmentString({{0.0, 0.0},
                                        {10.0, 0.0},
                                        {5.0, 5.0},
                                        {10.0, 5.0},
                                        {5.0, 0.0},
                                        {0.0, 0.0}});
    ASSERT_TRUE(ring->isClosed());

    adder.processIntersections(ring.get(), 1, ring.get(), 3);

    EXPECT_EQ(adder.numTests, 1);
    EXPECT_EQ(adder.numIntersections, 1);
    EXPECT_EQ(adder.numInteriorIntersections, 1);
    EXPECT_EQ(adder.numProperIntersections, 1);
    EXPECT_TRUE(adder.hasIntersection());
    EXPECT_TRUE(adder.hasProperIntersection());
    EXPECT_TRUE(
        adder.getProperIntersectionPoint().equals2D(CoordinateXY(7.5, 2.5)));
  }
}

}  // namespace
}  // namespace geos_google
