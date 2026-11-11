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

#include "third_party/geos/include/geos/noding/snapround/SnapRoundingNoder.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/noding/NodedSegmentString.h"
#include "third_party/geos/include/geos/noding/NodingValidator.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateXY;
using ::geos::geom::CoordinateXYM;
using ::geos::geom::CoordinateXYZM;
using ::geos::geom::PrecisionModel;
using ::geos::noding::NodedSegmentString;
using ::geos::noding::NodingValidator;
using ::geos::noding::SegmentString;
using ::geos::noding::snapround::SnapRoundingNoder;
using ::testing::UnorderedElementsAreArray;

struct SegmentExpectation {
  std::vector<CoordinateXY> coords;
  const void* context;
};

bool HasSame2DCoords(const SegmentString& ss,
                     const std::vector<CoordinateXY>& expected) {
  if (ss.size() != expected.size()) {
    return false;
  }
  for (std::size_t i = 0; i < expected.size(); ++i) {
    if (!ss.getCoordinate<CoordinateXY>(i).equals2D(expected[i])) {
      return false;
    }
  }
  return true;
}

std::vector<std::unique_ptr<NodedSegmentString>> BuildOwnedInputsXY(
    std::initializer_list<std::pair<std::vector<CoordinateXY>, const void*>>
        specs) {
  std::vector<std::unique_ptr<NodedSegmentString>> inputs;
  inputs.reserve(specs.size());
  for (const auto& [coords, context] : specs) {
    auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
    for (const CoordinateXY& c : coords) {
      seq->add(c);
    }
    inputs.push_back(std::make_unique<NodedSegmentString>(seq.release(), false,
                                                          false, context));
  }
  return inputs;
}

std::vector<SegmentString*> BorrowRaw(
    const std::vector<std::unique_ptr<NodedSegmentString>>& owned) {
  std::vector<SegmentString*> raw;
  raw.reserve(owned.size());
  for (const auto& item : owned) {
    raw.push_back(item.get());
  }
  return raw;
}

std::vector<std::unique_ptr<SegmentString>> TakeSubstringsOwnership(
    std::vector<SegmentString*>* raw) {
  std::unique_ptr<std::vector<SegmentString*>> vec_owner(raw);
  std::vector<std::unique_ptr<SegmentString>> out;
  if (!vec_owner) {
    return out;
  }
  out.reserve(vec_owner->size());
  for (SegmentString* ss : *vec_owner) {
    out.emplace_back(ss);
  }
  return out;
}

void ValidateNoding(
    const std::vector<std::unique_ptr<SegmentString>>& substrings) {
  std::vector<SegmentString*> raw;
  raw.reserve(substrings.size());
  for (const auto& ss : substrings) {
    ASSERT_GE(ss->size(), 2u);
    raw.push_back(ss.get());
  }
  NodingValidator validator(raw);
  EXPECT_NO_THROW(validator.checkValid());
}

void ExpectSubstringsMatch(
    const std::vector<std::unique_ptr<SegmentString>>& actual,
    const std::vector<SegmentExpectation>& expected) {
  ASSERT_EQ(actual.size(), expected.size());
  std::vector<bool> matched(expected.size(), false);
  for (const auto& ss : actual) {
    bool found = false;
    for (std::size_t j = 0; j < expected.size(); ++j) {
      if (!matched[j] && ss->getData() == expected[j].context &&
          HasSame2DCoords(*ss, expected[j].coords)) {
        matched[j] = true;
        found = true;
        break;
      }
    }
    EXPECT_TRUE(found) << "Unexpected substring: " << *ss;
  }
}

TEST(SnapRoundingNoderTest, EmptyInputProducesEmptyResult) {
  PrecisionModel pm(PrecisionModel::FIXED);
  SnapRoundingNoder noder(&pm);

  std::vector<SegmentString*> empty_inputs;
  noder.computeNodes(&empty_inputs);

  auto result = TakeSubstringsOwnership(noder.getNodedSubstrings());
  EXPECT_TRUE(result.empty());
}

TEST(SnapRoundingNoderTest, SingleSegmentRoundsCoordinatesAndKeepsContext) {
  PrecisionModel pm(1.0);
  SnapRoundingNoder noder(&pm);

  int tag = 42;
  auto owned = BuildOwnedInputsXY({
      {{{0.1, 0.2}, {4.9, 5.1}}, &tag},
  });
  auto inputs = BorrowRaw(owned);

  noder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(noder.getNodedSubstrings());

  ValidateNoding(result);
  ExpectSubstringsMatch(result, {{{{0.0, 0.0}, {5.0, 5.0}}, &tag}});
}

TEST(SnapRoundingNoderTest, CrossingSegmentsSplitAtGridIntersection) {
  PrecisionModel pm(1.0);
  SnapRoundingNoder noder(&pm);

  int tag_a = 1;
  int tag_b = 2;
  auto owned = BuildOwnedInputsXY({
      {{{0.0, 0.0}, {10.0, 10.0}}, &tag_a},
      {{{0.0, 10.0}, {10.0, 0.0}}, &tag_b},
  });
  auto inputs = BorrowRaw(owned);

  noder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(noder.getNodedSubstrings());

  ValidateNoding(result);
  ExpectSubstringsMatch(result, {
                                    {{{0.0, 0.0}, {5.0, 5.0}}, &tag_a},
                                    {{{5.0, 5.0}, {10.0, 10.0}}, &tag_a},
                                    {{{0.0, 10.0}, {5.0, 5.0}}, &tag_b},
                                    {{{5.0, 5.0}, {10.0, 0.0}}, &tag_b},
                                });
}

TEST(SnapRoundingNoderTest, MidGridCrossingSnapsToRoundedIntersectionNode) {
  PrecisionModel pm(1.0);
  SnapRoundingNoder noder(&pm);

  int tag_a = 1;
  int tag_b = 2;
  // Exact intersection (0.5, 0.5) rounds asymmetrically to (1, 1).
  auto owned = BuildOwnedInputsXY({
      {{{0.0, 0.0}, {1.0, 1.0}}, &tag_a},
      {{{0.0, 1.0}, {1.0, 0.0}}, &tag_b},
  });
  auto inputs = BorrowRaw(owned);

  noder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(noder.getNodedSubstrings());

  ValidateNoding(result);
  ExpectSubstringsMatch(result, {
                                    {{{0.0, 0.0}, {1.0, 1.0}}, &tag_a},
                                    {{{0.0, 1.0}, {1.0, 1.0}}, &tag_b},
                                    {{{1.0, 1.0}, {1.0, 0.0}}, &tag_b},
                                });
}

TEST(SnapRoundingNoderTest, CompletelyCollapsedEdgesAreEliminated) {
  PrecisionModel pm(1.0);

  // One collapsed edge alongside one surviving edge.
  {
    SnapRoundingNoder noder(&pm);
    int tag_dead = 1;
    int tag_live = 2;
    auto owned = BuildOwnedInputsXY({
        {{{0.1, 0.1}, {0.2, 0.2}, {0.3, 0.3}}, &tag_dead},
        {{{5.0, 0.0}, {5.0, 10.0}}, &tag_live},
    });
    auto inputs = BorrowRaw(owned);

    noder.computeNodes(&inputs);
    auto result = TakeSubstringsOwnership(noder.getNodedSubstrings());

    ValidateNoding(result);
    ExpectSubstringsMatch(result, {{{{5.0, 0.0}, {5.0, 10.0}}, &tag_live}});
  }

  // All inputs collapse to single points.
  {
    SnapRoundingNoder noder(&pm);
    auto owned = BuildOwnedInputsXY({
        {{{10.1, 10.1}, {10.3, 10.2}}, nullptr},
    });
    auto inputs = BorrowRaw(owned);

    noder.computeNodes(&inputs);
    auto result = TakeSubstringsOwnership(noder.getNodedSubstrings());
    EXPECT_TRUE(result.empty());
  }
}

TEST(SnapRoundingNoderTest, PartiallyCollapsedSegmentsAreSkippedWithinEdge) {
  PrecisionModel pm(1.0);
  SnapRoundingNoder noder(&pm);

  int tag_a = 1;
  int tag_b = 2;
  // Leading collapsed segment on A; trailing collapsed segment on B.
  auto owned = BuildOwnedInputsXY({
      {{{0.1, 0.1}, {0.3, 0.3}, {10.0, 0.0}}, &tag_a},
      {{{0.0, 5.0}, {9.8, 5.1}, {10.2, 4.9}}, &tag_b},
  });
  auto inputs = BorrowRaw(owned);

  noder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(noder.getNodedSubstrings());

  ValidateNoding(result);
  ExpectSubstringsMatch(result, {
                                    {{{0.0, 0.0}, {10.0, 0.0}}, &tag_a},
                                    {{{0.0, 5.0}, {10.0, 5.0}}, &tag_b},
                                });
}

TEST(SnapRoundingNoderTest, NearVertexWithinToleranceCreatesSnapNode) {
  PrecisionModel pm(1.0);
  SnapRoundingNoder noder(&pm);

  int tag_a = 1;
  int tag_b = 2;
  // Tolerance is 1.0 / 1.0 / 100 = 0.01. Gap 0.005 < 0.01 triggers
  // SnapRoundingIntersectionAdder near-vertex snapping.
  auto owned = BuildOwnedInputsXY({
      {{{0.0, 0.0}, {10.0, 0.0}}, &tag_a},
      {{{5.0, 0.005}, {5.0, 10.0}}, &tag_b},
  });
  auto inputs = BorrowRaw(owned);

  noder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(noder.getNodedSubstrings());

  ValidateNoding(result);
  ExpectSubstringsMatch(result, {
                                    {{{0.0, 0.0}, {5.0, 0.0}}, &tag_a},
                                    {{{5.0, 0.0}, {10.0, 0.0}}, &tag_a},
                                    {{{5.0, 0.0}, {5.0, 10.0}}, &tag_b},
                                });
}

TEST(SnapRoundingNoderTest, InteriorVertexHotPixelSnapsOtherEdgeAndParent) {
  PrecisionModel pm(1.0);
  SnapRoundingNoder noder(&pm);

  int tag_a = 1;
  int tag_b = 2;
  // A's interior vertex (6, 1) does not touch B = (0, 0)-(10, 1), but B passes
  // through HotPixel(6, 1), promoting it to a node and splitting A via
  // addVertexNodeSnaps.
  auto owned = BuildOwnedInputsXY({
      {{{0.0, 2.0}, {6.0, 1.0}, {10.0, 2.0}}, &tag_a},
      {{{0.0, 0.0}, {10.0, 1.0}}, &tag_b},
  });
  auto inputs = BorrowRaw(owned);

  noder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(noder.getNodedSubstrings());

  ValidateNoding(result);
  ExpectSubstringsMatch(result, {
                                    {{{0.0, 2.0}, {6.0, 1.0}}, &tag_a},
                                    {{{6.0, 1.0}, {10.0, 2.0}}, &tag_a},
                                    {{{0.0, 0.0}, {6.0, 1.0}}, &tag_b},
                                    {{{6.0, 1.0}, {10.0, 1.0}}, &tag_b},
                                });
}

TEST(SnapRoundingNoderTest,
     InteriorVertexNotNodedWhenUninvolvedEvenNearAnotherNode) {
  PrecisionModel pm(1.0);
  SnapRoundingNoder noder(&pm);

  int tag_a = 1;
  int tag_b = 2;
  int tag_c = 3;
  // A has interior vertex (5, 0) which is not a node.
  // B crosses A at (6, 0), creating a node within 1 grid unit of (5, 0).
  // C is 1 grid unit above A (inside query envelope expansion, non-snapping).
  auto owned = BuildOwnedInputsXY({
      {{{0.0, 0.0}, {5.0, 0.0}, {10.0, 0.0}}, &tag_a},
      {{{6.0, -5.0}, {6.0, 5.0}}, &tag_b},
      {{{0.0, 1.0}, {10.0, 1.0}}, &tag_c},
  });
  auto inputs = BorrowRaw(owned);

  noder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(noder.getNodedSubstrings());

  ValidateNoding(result);
  ExpectSubstringsMatch(result,
                        {
                            {{{0.0, 0.0}, {5.0, 0.0}, {6.0, 0.0}}, &tag_a},
                            {{{6.0, 0.0}, {10.0, 0.0}}, &tag_a},
                            {{{6.0, -5.0}, {6.0, 0.0}}, &tag_b},
                            {{{6.0, 0.0}, {6.0, 1.0}}, &tag_b},
                            {{{6.0, 1.0}, {6.0, 5.0}}, &tag_b},
                            {{{0.0, 1.0}, {6.0, 1.0}}, &tag_c},
                            {{{6.0, 1.0}, {10.0, 1.0}}, &tag_c},
                        });
}

TEST(SnapRoundingNoderTest, CoincidentRoundedVerticesPromoteHotPixelToNode) {
  PrecisionModel pm(1.0);
  SnapRoundingNoder noder(&pm);

  int tag_a = 1;
  int tag_b = 2;
  // A's interior vertex (5.1, 5.1) and B's start (4.9, 5.3) do not intersect
  // and exceed the 0.01 nearness tolerance, yet both round to (5, 5).
  auto owned = BuildOwnedInputsXY({
      {{{0.0, 2.0}, {5.1, 5.1}, {10.0, 2.0}}, &tag_a},
      {{{4.9, 5.3}, {5.0, 10.0}}, &tag_b},
  });
  auto inputs = BorrowRaw(owned);

  noder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(noder.getNodedSubstrings());

  ValidateNoding(result);
  ExpectSubstringsMatch(result, {
                                    {{{0.0, 2.0}, {5.0, 5.0}}, &tag_a},
                                    {{{5.0, 5.0}, {10.0, 2.0}}, &tag_a},
                                    {{{5.0, 5.0}, {5.0, 10.0}}, &tag_b},
                                });
}

TEST(SnapRoundingNoderTest, HotPixelsComputedBeforeRoundingPreserveTopology) {
  PrecisionModel pm(1.0);
  SnapRoundingNoder noder(&pm);

  int tag_l = 1;
  int tag_s = 2;
  // S starts at (5, 0.49), strictly above L = (0,0)-(10,0.9) at x=5 (y=0.45),
  // before rounding moves S's start down to (5, 0). Querying original segment L
  // against HotPixel(5, 0) nodes L at (5, 0).
  auto owned = BuildOwnedInputsXY({
      {{{0.0, 0.0}, {10.0, 0.9}}, &tag_l},
      {{{5.0, 0.49}, {5.0, 10.0}}, &tag_s},
  });
  auto inputs = BorrowRaw(owned);

  noder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(noder.getNodedSubstrings());

  ValidateNoding(result);
  ExpectSubstringsMatch(result, {
                                    {{{0.0, 0.0}, {5.0, 0.0}}, &tag_l},
                                    {{{5.0, 0.0}, {10.0, 1.0}}, &tag_l},
                                    {{{5.0, 0.0}, {5.0, 10.0}}, &tag_s},
                                });
}

TEST(SnapRoundingNoderTest, PreservesXYZAndInterpolatesIntersectionZ) {
  PrecisionModel pm(1.0);
  SnapRoundingNoder noder(&pm);

  auto seq0 = std::make_unique<CoordinateSequence>(0u, true, false);
  seq0->add(Coordinate(0.1, 0.1, 10.0));
  seq0->add(Coordinate(10.1, 10.1, 20.0));

  auto seq1 = std::make_unique<CoordinateSequence>(0u, true, false);
  seq1->add(Coordinate(0.1, 10.1, 30.0));
  seq1->add(Coordinate(10.1, 0.1, 40.0));

  NodedSegmentString ss0(seq0.release(), true, false, nullptr);
  NodedSegmentString ss1(seq1.release(), true, false, nullptr);
  std::vector<SegmentString*> inputs{&ss0, &ss1};

  noder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(noder.getNodedSubstrings());

  ASSERT_EQ(result.size(), 4u);
  ValidateNoding(result);

  std::vector<double> mid_zs;
  for (const auto& ss : result) {
    const CoordinateSequence* pts = ss->getCoordinates();
    EXPECT_TRUE(pts->hasZ());
    EXPECT_FALSE(pts->hasM());
    for (std::size_t i = 0; i < pts->size(); ++i) {
      EXPECT_TRUE(std::isnan(pts->getOrdinate(i, CoordinateSequence::M)));
      if (pts->getAt<CoordinateXY>(i).equals2D(CoordinateXY(5.0, 5.0))) {
        mid_zs.push_back(pts->getOrdinate(i, CoordinateSequence::Z));
      }
    }
  }
  EXPECT_THAT(mid_zs, UnorderedElementsAreArray({25.0, 25.0, 25.0, 25.0}));
}

TEST(SnapRoundingNoderTest, PreservesXYMAndInterpolatesIntersectionM) {
  PrecisionModel pm(1.0);
  SnapRoundingNoder noder(&pm);

  auto seq0 = std::make_unique<CoordinateSequence>(0u, false, true);
  seq0->add(CoordinateXYM(0.0, 0.0, 100.0));
  seq0->add(CoordinateXYM(10.0, 10.0, 200.0));

  auto seq1 = std::make_unique<CoordinateSequence>(0u, false, true);
  seq1->add(CoordinateXYM(0.0, 10.0, 300.0));
  seq1->add(CoordinateXYM(10.0, 0.0, 400.0));

  NodedSegmentString ss0(seq0.release(), false, true, nullptr);
  NodedSegmentString ss1(seq1.release(), false, true, nullptr);
  std::vector<SegmentString*> inputs{&ss0, &ss1};

  noder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(noder.getNodedSubstrings());

  ASSERT_EQ(result.size(), 4u);
  ValidateNoding(result);

  std::vector<double> mid_ms;
  for (const auto& ss : result) {
    const CoordinateSequence* pts = ss->getCoordinates();
    EXPECT_FALSE(pts->hasZ());
    EXPECT_TRUE(pts->hasM());
    for (std::size_t i = 0; i < pts->size(); ++i) {
      EXPECT_TRUE(std::isnan(pts->getOrdinate(i, CoordinateSequence::Z)));
      CoordinateXYM c;
      pts->getAt(i, c);
      if (c.equals2D(CoordinateXY(5.0, 5.0))) {
        mid_ms.push_back(pts->getOrdinate(i, CoordinateSequence::M));
        EXPECT_DOUBLE_EQ(c.m, 250.0);
      }
    }
  }
  EXPECT_THAT(mid_ms, UnorderedElementsAreArray({250.0, 250.0, 250.0, 250.0}));
}

TEST(SnapRoundingNoderTest, PreservesXYZMAndInterpolatesZAndM) {
  PrecisionModel pm(1.0);
  SnapRoundingNoder noder(&pm);

  auto seq0 = std::make_unique<CoordinateSequence>(0u, true, true);
  seq0->add(CoordinateXYZM(0.0, 0.0, 10.0, 100.0));
  seq0->add(CoordinateXYZM(10.0, 10.0, 20.0, 200.0));

  auto seq1 = std::make_unique<CoordinateSequence>(0u, true, true);
  seq1->add(CoordinateXYZM(0.0, 10.0, 30.0, 300.0));
  seq1->add(CoordinateXYZM(10.0, 0.0, 40.0, 400.0));

  NodedSegmentString ss0(seq0.release(), true, true, nullptr);
  NodedSegmentString ss1(seq1.release(), true, true, nullptr);
  std::vector<SegmentString*> inputs{&ss0, &ss1};

  noder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(noder.getNodedSubstrings());

  ASSERT_EQ(result.size(), 4u);
  ValidateNoding(result);

  std::vector<double> mid_zs;
  std::vector<double> mid_ms;
  for (const auto& ss : result) {
    const CoordinateSequence* pts = ss->getCoordinates();
    EXPECT_TRUE(pts->hasZ());
    EXPECT_TRUE(pts->hasM());
    for (std::size_t i = 0; i < pts->size(); ++i) {
      CoordinateXYZM c;
      pts->getAt(i, c);
      if (c.equals2D(CoordinateXY(5.0, 5.0))) {
        mid_zs.push_back(c.z);
        mid_ms.push_back(c.m);
      }
    }
  }
  EXPECT_THAT(mid_zs, UnorderedElementsAreArray({25.0, 25.0, 25.0, 25.0}));
  EXPECT_THAT(mid_ms, UnorderedElementsAreArray({250.0, 250.0, 250.0, 250.0}));
}

TEST(SnapRoundingNoderTest, FractionalAndNegativeScalePrecisionModels) {
  // Fractional grid 0.1 (scale = 10.0).
  {
    PrecisionModel pm(10.0);
    SnapRoundingNoder noder(&pm);

    int tag_a = 1;
    int tag_b = 2;
    auto owned = BuildOwnedInputsXY({
        {{{0.01, 0.02}, {0.99, 1.01}}, &tag_a},
        {{{0.02, 0.99}, {1.01, -0.01}}, &tag_b},
    });
    auto inputs = BorrowRaw(owned);

    noder.computeNodes(&inputs);
    auto result = TakeSubstringsOwnership(noder.getNodedSubstrings());

    ValidateNoding(result);
    ExpectSubstringsMatch(result, {
                                      {{{0.0, 0.0}, {0.5, 0.5}}, &tag_a},
                                      {{{0.5, 0.5}, {1.0, 1.0}}, &tag_a},
                                      {{{0.0, 1.0}, {0.5, 0.5}}, &tag_b},
                                      {{{0.5, 0.5}, {1.0, 0.0}}, &tag_b},
                                  });
  }

  // Negative scale sets grid size 5.0 (scale = 0.2).
  {
    PrecisionModel pm(-5.0);
    SnapRoundingNoder noder(&pm);

    int tag_a = 1;
    int tag_b = 2;
    auto owned = BuildOwnedInputsXY({
        {{{1.0, 1.0}, {19.0, 21.0}}, &tag_a},
        {{{1.0, 19.0}, {21.0, 1.0}}, &tag_b},
    });
    auto inputs = BorrowRaw(owned);

    noder.computeNodes(&inputs);
    auto result = TakeSubstringsOwnership(noder.getNodedSubstrings());

    ValidateNoding(result);
    ExpectSubstringsMatch(result, {
                                      {{{0.0, 0.0}, {10.0, 10.0}}, &tag_a},
                                      {{{10.0, 10.0}, {20.0, 20.0}}, &tag_a},
                                      {{{0.0, 20.0}, {10.0, 10.0}}, &tag_b},
                                      {{{10.0, 10.0}, {20.0, 0.0}}, &tag_b},
                                  });
  }
}

}  // namespace
}  // namespace geos_google
