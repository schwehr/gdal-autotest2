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

#include "third_party/geos/include/geos/noding/snapround/MCIndexSnapRounder.h"

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
#include "third_party/geos/include/geos/noding/Noder.h"
#include "third_party/geos/include/geos/noding/NodingValidator.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateXY;
using ::geos::geom::CoordinateXYZM;
using ::geos::geom::PrecisionModel;
using ::geos::noding::NodedSegmentString;
using ::geos::noding::Noder;
using ::geos::noding::NodingValidator;
using ::geos::noding::SegmentString;
using ::geos::noding::snapround::MCIndexSnapRounder;
using ::geos::util::IllegalArgumentException;
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

std::vector<std::unique_ptr<NodedSegmentString>> BuildOwnedInputs(
    std::initializer_list<std::pair<std::vector<Coordinate>, const void*>>
        specs) {
  std::vector<std::unique_ptr<NodedSegmentString>> inputs;
  inputs.reserve(specs.size());
  for (const auto& [coords, context] : specs) {
    auto seq = std::make_unique<CoordinateSequence>(0u, true, false);
    for (const Coordinate& c : coords) {
      seq->add(c);
    }
    inputs.push_back(std::make_unique<NodedSegmentString>(seq.release(), true,
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

TEST(MCIndexSnapRounderTest, EmptyInputProducesNoSubstrings) {
  PrecisionModel pm(PrecisionModel::FIXED);
  MCIndexSnapRounder rounder(pm);

  std::vector<SegmentString*> empty_inputs;
  rounder.computeNodes(&empty_inputs);

  auto result = TakeSubstringsOwnership(rounder.getNodedSubstrings());
  EXPECT_TRUE(result.empty());
}

TEST(MCIndexSnapRounderTest, DisjointSegmentsRemainUnchangedWithContext) {
  PrecisionModel pm(1.0);
  MCIndexSnapRounder rounder(pm);

  int tag_a = 1;
  int tag_b = 2;
  auto owned = BuildOwnedInputs({
      {{{0.0, 0.0}, {10.0, 0.0}}, &tag_a},
      {{{0.0, 5.0}, {10.0, 5.0}}, &tag_b},
  });
  auto inputs = BorrowRaw(owned);

  rounder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(rounder.getNodedSubstrings());

  ValidateNoding(result);
  ExpectSubstringsMatch(result, {
                                    {{{0.0, 0.0}, {10.0, 0.0}}, &tag_a},
                                    {{{0.0, 5.0}, {10.0, 5.0}}, &tag_b},
                                });
}

TEST(MCIndexSnapRounderTest, CrossingSegmentsSplitAtIntersection) {
  PrecisionModel pm(1.0);
  MCIndexSnapRounder rounder(pm);

  int tag_a = 1;
  int tag_b = 2;
  auto owned = BuildOwnedInputs({
      {{{0.0, 0.0}, {10.0, 10.0}}, &tag_a},
      {{{0.0, 10.0}, {10.0, 0.0}}, &tag_b},
  });
  auto inputs = BorrowRaw(owned);

  rounder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(rounder.getNodedSubstrings());

  ValidateNoding(result);
  ExpectSubstringsMatch(result, {
                                    {{{0.0, 0.0}, {5.0, 5.0}}, &tag_a},
                                    {{{5.0, 5.0}, {10.0, 10.0}}, &tag_a},
                                    {{{0.0, 10.0}, {5.0, 5.0}}, &tag_b},
                                    {{{5.0, 5.0}, {10.0, 0.0}}, &tag_b},
                                });
}

TEST(MCIndexSnapRounderTest, IntersectionHotPixelSnapsThirdNearbySegment) {
  PrecisionModel pm(1.0);
  MCIndexSnapRounder rounder(pm);

  int tag_a = 1;
  int tag_b = 2;
  int tag_c = 3;
  // Segments A and B cross at (4.5, 4.5), which rounds to hot pixel (5, 5).
  // Segment C = (0, 6)-(9, 5) does not intersect A or B in exact geometry,
  // passing through (5, 5.444...) inside HotPixel(5, 5) = [4.5, 5.5) x
  // [4.5, 5.5), so computeIntersectionSnaps must split C at (5, 5).
  auto owned = BuildOwnedInputs({
      {{{4.0, 4.0}, {5.0, 5.0}}, &tag_a},
      {{{4.0, 5.0}, {5.0, 4.0}}, &tag_b},
      {{{0.0, 6.0}, {9.0, 5.0}}, &tag_c},
  });
  auto inputs = BorrowRaw(owned);

  rounder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(rounder.getNodedSubstrings());

  ValidateNoding(result);
  ExpectSubstringsMatch(result, {
                                    {{{4.0, 4.0}, {5.0, 5.0}}, &tag_a},
                                    {{{4.0, 5.0}, {5.0, 5.0}}, &tag_b},
                                    {{{5.0, 5.0}, {5.0, 4.0}}, &tag_b},
                                    {{{0.0, 6.0}, {5.0, 5.0}}, &tag_c},
                                    {{{5.0, 5.0}, {9.0, 5.0}}, &tag_c},
                                });
}

TEST(MCIndexSnapRounderTest, VertexSnapsNonIntersectingSegmentAndNodesParent) {
  PrecisionModel pm(1.0);
  MCIndexSnapRounder rounder(pm);

  int tag_a = 1;
  int tag_b = 2;
  // Edge A has interior vertex (6, 1) which does not intersect B =
  // (0,0)-(10,1). However, B passes through (6, 0.6) inside HotPixel(6, 1), so
  // computeVertexSnaps must snap B and also add a node at vertex 1 of A.
  auto owned = BuildOwnedInputs({
      {{{0.0, 2.0}, {6.0, 1.0}, {10.0, 2.0}}, &tag_a},
      {{{0.0, 0.0}, {10.0, 1.0}}, &tag_b},
  });
  auto inputs = BorrowRaw(owned);

  rounder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(rounder.getNodedSubstrings());

  ValidateNoding(result);
  ExpectSubstringsMatch(result, {
                                    {{{0.0, 2.0}, {6.0, 1.0}}, &tag_a},
                                    {{{6.0, 1.0}, {10.0, 2.0}}, &tag_a},
                                    {{{0.0, 0.0}, {6.0, 1.0}}, &tag_b},
                                    {{{6.0, 1.0}, {10.0, 1.0}}, &tag_b},
                                });
}

TEST(MCIndexSnapRounderTest, SelfIntersectingPolylineIsNoded) {
  PrecisionModel pm(1.0);
  MCIndexSnapRounder rounder(pm);

  int tag = 7;
  auto owned = BuildOwnedInputs({
      {{{0.0, 0.0}, {10.0, 10.0}, {10.0, 0.0}, {0.0, 10.0}}, &tag},
  });
  auto inputs = BorrowRaw(owned);

  rounder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(rounder.getNodedSubstrings());

  ValidateNoding(result);
  ExpectSubstringsMatch(
      result, {
                  {{{0.0, 0.0}, {5.0, 5.0}}, &tag},
                  {{{5.0, 5.0}, {10.0, 10.0}, {10.0, 0.0}, {5.0, 5.0}}, &tag},
                  {{{5.0, 5.0}, {0.0, 10.0}}, &tag},
              });
}

TEST(MCIndexSnapRounderTest, FractionalScaleRoundsIntersectionAndSnapsVertex) {
  PrecisionModel pm(10.0);
  MCIndexSnapRounder rounder(pm);

  int tag_a = 1;
  int tag_b = 2;
  int tag_c = 3;
  // At scale 10.0 (grid size 0.1), A and B intersect at (0.5, 0.5), while the
  // non-intersecting edge's interior vertex (0.6, 2.1) snaps (0.0, 2.0)-(1.0,
  // 2.1) at (0.6, 2.1).
  auto owned = BuildOwnedInputs({
      {{{0.0, 0.0}, {1.0, 1.0}}, &tag_a},
      {{{0.0, 1.0}, {1.0, 0.0}}, &tag_b},
      {{{0.0, 2.2}, {0.6, 2.1}, {1.0, 2.2}}, &tag_c},
      {{{0.0, 2.0}, {1.0, 2.1}}, &tag_c},
  });
  auto inputs = BorrowRaw(owned);

  rounder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(rounder.getNodedSubstrings());

  ValidateNoding(result);
  ExpectSubstringsMatch(result, {
                                    {{{0.0, 0.0}, {0.5, 0.5}}, &tag_a},
                                    {{{0.5, 0.5}, {1.0, 1.0}}, &tag_a},
                                    {{{0.0, 1.0}, {0.5, 0.5}}, &tag_b},
                                    {{{0.5, 0.5}, {1.0, 0.0}}, &tag_b},
                                    {{{0.0, 2.2}, {0.6, 2.1}}, &tag_c},
                                    {{{0.6, 2.1}, {1.0, 2.2}}, &tag_c},
                                    {{{0.0, 2.0}, {0.6, 2.1}}, &tag_c},
                                    {{{0.6, 2.1}, {1.0, 2.1}}, &tag_c},
                                });
}

TEST(MCIndexSnapRounderTest, NegativeScaleGridSizeSnapsVerticesAndCrossings) {
  // Negative scale specifies grid size = 10.0 (scale = 0.1).
  PrecisionModel pm(-10.0);
  MCIndexSnapRounder rounder(pm);

  int tag_a = 1;
  int tag_b = 2;
  auto owned = BuildOwnedInputs({
      {{{0.0, 20.0}, {60.0, 10.0}, {100.0, 20.0}}, &tag_a},
      {{{0.0, 0.0}, {100.0, 10.0}}, &tag_b},
  });
  auto inputs = BorrowRaw(owned);

  rounder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(rounder.getNodedSubstrings());

  ValidateNoding(result);
  ExpectSubstringsMatch(result, {
                                    {{{0.0, 20.0}, {60.0, 10.0}}, &tag_a},
                                    {{{60.0, 10.0}, {100.0, 20.0}}, &tag_a},
                                    {{{0.0, 0.0}, {60.0, 10.0}}, &tag_b},
                                    {{{60.0, 10.0}, {100.0, 10.0}}, &tag_b},
                                });
}

TEST(MCIndexSnapRounderTest, PreservesZAndMCoordinatesOnNodedSubstrings) {
  PrecisionModel pm(1.0);
  MCIndexSnapRounder rounder(pm);

  auto seq0 = std::make_unique<CoordinateSequence>(0u, true, true);
  seq0->add(CoordinateXYZM(0.0, 0.0, 10.0, 100.0));
  seq0->add(CoordinateXYZM(10.0, 10.0, 20.0, 200.0));

  auto seq1 = std::make_unique<CoordinateSequence>(0u, true, true);
  seq1->add(CoordinateXYZM(0.0, 10.0, 30.0, 300.0));
  seq1->add(CoordinateXYZM(10.0, 0.0, 40.0, 400.0));

  NodedSegmentString ss0(seq0.release(), true, true, nullptr);
  NodedSegmentString ss1(seq1.release(), true, true, nullptr);
  std::vector<SegmentString*> inputs{&ss0, &ss1};

  rounder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(rounder.getNodedSubstrings());

  ASSERT_EQ(result.size(), 4u);
  ValidateNoding(result);

  std::vector<double> mid_zs;
  std::vector<double> endpoint_ms;
  for (const auto& ss : result) {
    const CoordinateSequence* pts = ss->getCoordinates();
    ASSERT_TRUE(pts->hasZ());
    ASSERT_TRUE(pts->hasM());
    for (std::size_t i = 0; i < pts->size(); ++i) {
      if (pts->getAt<CoordinateXY>(i).equals2D(CoordinateXY(5.0, 5.0))) {
        mid_zs.push_back(pts->getOrdinate(i, CoordinateSequence::Z));
        // IntersectionFinderAdder queries Coordinate (XYZ), so M is NaN at
        // newly computed intersection nodes.
        EXPECT_TRUE(std::isnan(pts->getOrdinate(i, CoordinateSequence::M)));
      } else {
        endpoint_ms.push_back(pts->getOrdinate(i, CoordinateSequence::M));
      }
    }
  }
  EXPECT_THAT(mid_zs, UnorderedElementsAreArray({25.0, 25.0, 25.0, 25.0}));
  EXPECT_THAT(endpoint_ms,
              UnorderedElementsAreArray({100.0, 200.0, 300.0, 400.0}));
}

TEST(MCIndexSnapRounderTest,
     DefaultFloatingPrecisionModelThrowsIllegalArgument) {
  PrecisionModel pm;
  ASSERT_TRUE(pm.isFloating());
  ASSERT_EQ(pm.getScale(), 0.0);

  // Non-intersecting segment string throws in computeVertexSnaps.
  {
    MCIndexSnapRounder rounder(pm);
    auto owned = BuildOwnedInputs({
        {{{0.0, 0.0}, {10.0, 10.0}}, nullptr},
    });
    auto inputs = BorrowRaw(owned);
    EXPECT_THROW(rounder.computeNodes(&inputs), IllegalArgumentException);
  }

  // Crossing segment strings throw in computeIntersectionSnaps.
  {
    MCIndexSnapRounder rounder(pm);
    auto owned = BuildOwnedInputs({
        {{{0.0, 0.0}, {10.0, 10.0}}, nullptr},
        {{{0.0, 10.0}, {10.0, 0.0}}, nullptr},
    });
    auto inputs = BorrowRaw(owned);
    EXPECT_THROW(rounder.computeNodes(&inputs), IllegalArgumentException);
  }

  // Empty input constructs no HotPixel and returns no substrings.
  {
    MCIndexSnapRounder rounder(pm);
    std::vector<SegmentString*> empty_inputs;
    EXPECT_NO_THROW(rounder.computeNodes(&empty_inputs));
    auto result = TakeSubstringsOwnership(rounder.getNodedSubstrings());
    EXPECT_TRUE(result.empty());
  }
}

TEST(MCIndexSnapRounderTest,
     ExplicitFloatingTypeUsesUnitScaleAndPreservesExactIntersection) {
  // PrecisionModel(Type) initializes scale=1.0 even for FLOATING, so
  // LineIntersector does not round intersection coordinates while HotPixel uses
  // scale factor 1.0.
  PrecisionModel pm(PrecisionModel::FLOATING);
  ASSERT_TRUE(pm.isFloating());
  ASSERT_EQ(pm.getScale(), 1.0);

  MCIndexSnapRounder rounder(pm);
  int tag_a = 1;
  int tag_b = 2;
  auto owned = BuildOwnedInputs({
      {{{4.0, 4.0}, {5.0, 5.0}}, &tag_a},
      {{{4.0, 5.0}, {5.0, 4.0}}, &tag_b},
  });
  auto inputs = BorrowRaw(owned);

  rounder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(rounder.getNodedSubstrings());

  ValidateNoding(result);
  ExpectSubstringsMatch(result, {
                                    {{{4.0, 4.0}, {4.5, 4.5}}, &tag_a},
                                    {{{4.5, 4.5}, {5.0, 5.0}}, &tag_a},
                                    {{{4.0, 5.0}, {4.5, 4.5}}, &tag_b},
                                    {{{4.5, 4.5}, {5.0, 4.0}}, &tag_b},
                                });
}

TEST(MCIndexSnapRounderTest, NegativeScaleGridSizeRoundsCrossings) {
  PrecisionModel pm(-10.0);
  MCIndexSnapRounder rounder(pm);

  int tag_a = 1;
  int tag_b = 2;
  int tag_c = 3;
  // Cross at (45, 45), which rounds to (50, 50) on grid size 10.0, snapping
  // the third nearby segment at (50, 50).
  auto owned = BuildOwnedInputs({
      {{{40.0, 40.0}, {50.0, 50.0}}, &tag_a},
      {{{40.0, 50.0}, {50.0, 40.0}}, &tag_b},
      {{{0.0, 60.0}, {90.0, 50.0}}, &tag_c},
  });
  auto inputs = BorrowRaw(owned);

  rounder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(rounder.getNodedSubstrings());

  ValidateNoding(result);
  ExpectSubstringsMatch(result, {
                                    {{{40.0, 40.0}, {50.0, 50.0}}, &tag_a},
                                    {{{40.0, 50.0}, {50.0, 50.0}}, &tag_b},
                                    {{{50.0, 50.0}, {50.0, 40.0}}, &tag_b},
                                    {{{0.0, 60.0}, {50.0, 50.0}}, &tag_c},
                                    {{{50.0, 50.0}, {90.0, 50.0}}, &tag_c},
                                });
}

TEST(MCIndexSnapRounderTest, CollinearOverlappingSegmentsAreSplitAtEndpoints) {
  PrecisionModel pm(1.0);
  MCIndexSnapRounder rounder(pm);

  int tag_a = 1;
  int tag_b = 2;
  int tag_c = 3;
  int tag_d = 4;
  auto owned = BuildOwnedInputs({
      {{{0.0, 0.0}, {10.0, 0.0}}, &tag_a},
      {{{4.0, 0.0}, {14.0, 0.0}}, &tag_b},
      {{{0.0, 5.0}, {10.0, 5.0}}, &tag_c},
      {{{2.0, 5.0}, {8.0, 5.0}}, &tag_d},
  });
  auto inputs = BorrowRaw(owned);

  rounder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(rounder.getNodedSubstrings());

  ValidateNoding(result);
  ExpectSubstringsMatch(result, {
                                    {{{0.0, 0.0}, {4.0, 0.0}}, &tag_a},
                                    {{{4.0, 0.0}, {10.0, 0.0}}, &tag_a},
                                    {{{4.0, 0.0}, {10.0, 0.0}}, &tag_b},
                                    {{{10.0, 0.0}, {14.0, 0.0}}, &tag_b},
                                    {{{0.0, 5.0}, {2.0, 5.0}}, &tag_c},
                                    {{{2.0, 5.0}, {8.0, 5.0}}, &tag_c},
                                    {{{8.0, 5.0}, {10.0, 5.0}}, &tag_c},
                                    {{{2.0, 5.0}, {8.0, 5.0}}, &tag_d},
                                });
}

TEST(MCIndexSnapRounderTest, EndpointTouchingInteriorSplitsTargetSegment) {
  PrecisionModel pm(1.0);
  MCIndexSnapRounder rounder(pm);

  int tag_a = 1;
  int tag_b = 2;
  auto owned = BuildOwnedInputs({
      {{{0.0, 0.0}, {10.0, 0.0}}, &tag_a},
      {{{5.0, 0.0}, {5.0, 10.0}}, &tag_b},
  });
  auto inputs = BorrowRaw(owned);

  rounder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(rounder.getNodedSubstrings());

  ValidateNoding(result);
  ExpectSubstringsMatch(result, {
                                    {{{0.0, 0.0}, {5.0, 0.0}}, &tag_a},
                                    {{{5.0, 0.0}, {10.0, 0.0}}, &tag_a},
                                    {{{5.0, 0.0}, {5.0, 10.0}}, &tag_b},
                                });
}

TEST(MCIndexSnapRounderTest, StartVertexSnapsDisjointSegment) {
  PrecisionModel pm(1.0);
  MCIndexSnapRounder rounder(pm);

  int tag_a = 1;
  int tag_b = 2;
  // Vertex 0 of A is (0, 1); B crosses x=0 at y=2/3 inside HotPixel(0, 1),
  // strictly below A's y-range [1, 10].
  auto owned = BuildOwnedInputs({
      {{{0.0, 1.0}, {0.0, 10.0}}, &tag_a},
      {{{-1.0, 0.0}, {2.0, 2.0}}, &tag_b},
  });
  auto inputs = BorrowRaw(owned);

  rounder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(rounder.getNodedSubstrings());

  ValidateNoding(result);
  ExpectSubstringsMatch(result, {
                                    {{{0.0, 1.0}, {0.0, 10.0}}, &tag_a},
                                    {{{-1.0, 0.0}, {0.0, 1.0}}, &tag_b},
                                    {{{0.0, 1.0}, {2.0, 2.0}}, &tag_b},
                                });
}

TEST(MCIndexSnapRounderTest, HotPixelBoundaryLeftBottomClosedRightTopOpen) {
  PrecisionModel pm(1.0);

  // Left edge (x = 4.5) is closed and snaps; right edge (x = 5.5) is open.
  {
    MCIndexSnapRounder rounder(pm);
    int tag_e = 1;
    int tag_l = 2;
    int tag_r = 3;
    auto owned = BuildOwnedInputs({
        {{{4.0, 7.0}, {5.0, 5.0}, {6.0, 7.0}}, &tag_e},
        {{{4.5, 0.0}, {4.5, 5.2}}, &tag_l},
        {{{5.5, 0.0}, {5.5, 5.2}}, &tag_r},
    });
    auto inputs = BorrowRaw(owned);

    rounder.computeNodes(&inputs);
    auto result = TakeSubstringsOwnership(rounder.getNodedSubstrings());

    ValidateNoding(result);
    ExpectSubstringsMatch(result, {
                                      {{{4.0, 7.0}, {5.0, 5.0}}, &tag_e},
                                      {{{5.0, 5.0}, {6.0, 7.0}}, &tag_e},
                                      {{{4.5, 0.0}, {5.0, 5.0}}, &tag_l},
                                      {{{5.0, 5.0}, {4.5, 5.2}}, &tag_l},
                                      {{{5.5, 0.0}, {5.5, 5.2}}, &tag_r},
                                  });
  }

  // Bottom edge (y = 4.5) is closed and snaps; top edge (y = 5.5) is open.
  {
    MCIndexSnapRounder rounder(pm);
    int tag_e = 1;
    int tag_b = 2;
    int tag_t = 3;
    auto owned = BuildOwnedInputs({
        {{{7.0, 4.0}, {5.0, 5.0}, {7.0, 6.0}}, &tag_e},
        {{{0.0, 4.5}, {5.2, 4.5}}, &tag_b},
        {{{0.0, 5.5}, {5.2, 5.5}}, &tag_t},
    });
    auto inputs = BorrowRaw(owned);

    rounder.computeNodes(&inputs);
    auto result = TakeSubstringsOwnership(rounder.getNodedSubstrings());

    ValidateNoding(result);
    ExpectSubstringsMatch(result, {
                                      {{{7.0, 4.0}, {5.0, 5.0}}, &tag_e},
                                      {{{5.0, 5.0}, {7.0, 6.0}}, &tag_e},
                                      {{{0.0, 4.5}, {5.0, 5.0}}, &tag_b},
                                      {{{5.0, 5.0}, {5.2, 4.5}}, &tag_b},
                                      {{{0.0, 5.5}, {5.2, 5.5}}, &tag_t},
                                  });
  }
}

TEST(MCIndexSnapRounderTest, MultipleIntersectionsAndSnapsAlongSingleSegment) {
  PrecisionModel pm(1.0);
  MCIndexSnapRounder rounder(pm);

  int tag_l = 1;
  int tag_v1 = 2;
  int tag_v2 = 3;
  int tag_c = 4;
  auto owned = BuildOwnedInputs({
      {{{0.0, 0.0}, {20.0, 1.0}}, &tag_l},
      {{{3.0, -2.0}, {5.0, 0.0}, {7.0, -2.0}}, &tag_v1},
      {{{8.0, 2.0}, {10.0, 1.0}, {12.0, 2.0}}, &tag_v2},
      {{{15.0, 0.0}, {15.0, 2.0}}, &tag_c},
  });
  auto inputs = BorrowRaw(owned);

  rounder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(rounder.getNodedSubstrings());

  ValidateNoding(result);
  ExpectSubstringsMatch(result, {
                                    {{{0.0, 0.0}, {5.0, 0.0}}, &tag_l},
                                    {{{5.0, 0.0}, {10.0, 1.0}}, &tag_l},
                                    {{{10.0, 1.0}, {15.0, 1.0}}, &tag_l},
                                    {{{15.0, 1.0}, {20.0, 1.0}}, &tag_l},
                                    {{{3.0, -2.0}, {5.0, 0.0}}, &tag_v1},
                                    {{{5.0, 0.0}, {7.0, -2.0}}, &tag_v1},
                                    {{{8.0, 2.0}, {10.0, 1.0}}, &tag_v2},
                                    {{{10.0, 1.0}, {12.0, 2.0}}, &tag_v2},
                                    {{{15.0, 0.0}, {15.0, 1.0}}, &tag_c},
                                    {{{15.0, 1.0}, {15.0, 2.0}}, &tag_c},
                                });
}

TEST(MCIndexSnapRounderTest, ClosedRingsIntersectionIsNoded) {
  PrecisionModel pm(1.0);
  MCIndexSnapRounder rounder(pm);

  int tag_a = 1;
  int tag_b = 2;
  auto owned = BuildOwnedInputs({
      {{{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}},
       &tag_a},
      {{{5.0, 5.0}, {15.0, 5.0}, {15.0, 15.0}, {5.0, 15.0}, {5.0, 5.0}},
       &tag_b},
  });
  auto inputs = BorrowRaw(owned);

  rounder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(rounder.getNodedSubstrings());

  ValidateNoding(result);
  ExpectSubstringsMatch(
      result,
      {
          {{{0.0, 0.0}, {10.0, 0.0}, {10.0, 5.0}}, &tag_a},
          {{{10.0, 5.0}, {10.0, 10.0}, {5.0, 10.0}}, &tag_a},
          {{{5.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}}, &tag_a},
          {{{5.0, 5.0}, {10.0, 5.0}}, &tag_b},
          {{{10.0, 5.0}, {15.0, 5.0}, {15.0, 15.0}, {5.0, 15.0}, {5.0, 10.0}},
           &tag_b},
          {{{5.0, 10.0}, {5.0, 5.0}}, &tag_b},
      });
}

TEST(MCIndexSnapRounderTest, PolymorphicNoderAndNegativeCoordinates) {
  PrecisionModel pm(1.0);
  std::unique_ptr<Noder> noder = std::make_unique<MCIndexSnapRounder>(pm);

  int tag_a = 1;
  int tag_b = 2;
  int tag_c = 3;
  int tag_d = 4;
  // C and D cross at (-14.5, -14.5); util::round(-14.5) == -14.0.
  auto owned = BuildOwnedInputs({
      {{{-10.0, -10.0}, {0.0, 0.0}}, &tag_a},
      {{{-10.0, 0.0}, {0.0, -10.0}}, &tag_b},
      {{{-14.0, -14.0}, {-15.0, -15.0}}, &tag_c},
      {{{-14.0, -15.0}, {-15.0, -14.0}}, &tag_d},
  });
  auto inputs = BorrowRaw(owned);

  noder->computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(noder->getNodedSubstrings());

  ValidateNoding(result);
  ExpectSubstringsMatch(result, {
                                    {{{-10.0, -10.0}, {-5.0, -5.0}}, &tag_a},
                                    {{{-5.0, -5.0}, {0.0, 0.0}}, &tag_a},
                                    {{{-10.0, 0.0}, {-5.0, -5.0}}, &tag_b},
                                    {{{-5.0, -5.0}, {0.0, -10.0}}, &tag_b},
                                    {{{-14.0, -14.0}, {-15.0, -15.0}}, &tag_c},
                                    {{{-14.0, -15.0}, {-14.0, -14.0}}, &tag_d},
                                    {{{-14.0, -14.0}, {-15.0, -14.0}}, &tag_d},
                                });
}

TEST(MCIndexSnapRounderTest, VertexSnapPreservesZOnXYZInput) {
  PrecisionModel pm(1.0);
  MCIndexSnapRounder rounder(pm);

  auto seq0 = std::make_unique<CoordinateSequence>(0u, true, false);
  seq0->add(Coordinate(0.0, 2.0, 10.0));
  seq0->add(Coordinate(6.0, 1.0, 25.0));
  seq0->add(Coordinate(10.0, 2.0, 30.0));

  auto seq1 = std::make_unique<CoordinateSequence>(0u, true, false);
  seq1->add(Coordinate(0.0, 0.0, 100.0));
  seq1->add(Coordinate(10.0, 1.0, 200.0));

  NodedSegmentString ss0(seq0.release(), true, false, nullptr);
  NodedSegmentString ss1(seq1.release(), true, false, nullptr);
  std::vector<SegmentString*> inputs{&ss0, &ss1};

  rounder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(rounder.getNodedSubstrings());

  ASSERT_EQ(result.size(), 4u);
  ValidateNoding(result);

  std::vector<double> snap_zs;
  for (const auto& ss : result) {
    const CoordinateSequence* pts = ss->getCoordinates();
    ASSERT_TRUE(pts->hasZ());
    ASSERT_FALSE(pts->hasM());
    for (std::size_t i = 0; i < pts->size(); ++i) {
      EXPECT_TRUE(std::isnan(pts->getOrdinate(i, CoordinateSequence::M)));
      if (pts->getAt<CoordinateXY>(i).equals2D(CoordinateXY(6.0, 1.0))) {
        snap_zs.push_back(pts->getOrdinate(i, CoordinateSequence::Z));
      }
    }
  }
  EXPECT_THAT(snap_zs, UnorderedElementsAreArray({25.0, 25.0, 25.0, 25.0}));
}

TEST(MCIndexSnapRounderTest, MissingZAndMRemainNaNOn2DInput) {
  PrecisionModel pm(1.0);
  MCIndexSnapRounder rounder(pm);

  auto seq0 = std::make_unique<CoordinateSequence>(0u, false, false);
  seq0->add(CoordinateXY(0.0, 0.0));
  seq0->add(CoordinateXY(10.0, 10.0));

  auto seq1 = std::make_unique<CoordinateSequence>(0u, false, false);
  seq1->add(CoordinateXY(0.0, 10.0));
  seq1->add(CoordinateXY(10.0, 0.0));

  NodedSegmentString ss0(seq0.release(), false, false, nullptr);
  NodedSegmentString ss1(seq1.release(), false, false, nullptr);
  std::vector<SegmentString*> inputs{&ss0, &ss1};

  rounder.computeNodes(&inputs);
  auto result = TakeSubstringsOwnership(rounder.getNodedSubstrings());

  ASSERT_EQ(result.size(), 4u);
  ValidateNoding(result);
  for (const auto& ss : result) {
    const CoordinateSequence* pts = ss->getCoordinates();
    for (std::size_t i = 0; i < pts->size(); ++i) {
      EXPECT_TRUE(std::isnan(pts->getOrdinate(i, CoordinateSequence::Z)));
      EXPECT_TRUE(std::isnan(pts->getOrdinate(i, CoordinateSequence::M)));
    }
  }
}

template <typename Tag, typename Tag::type M>
struct AccessSnapRounderPrivate {
  friend typename Tag::type GetSnapRounderPrivate(Tag) { return M; }
};

struct CheckCorrectnessTag {
  using type = void (MCIndexSnapRounder::*)(SegmentString::NonConstVect&);
  friend type GetSnapRounderPrivate(CheckCorrectnessTag);
};

template struct AccessSnapRounderPrivate<CheckCorrectnessTag,
                                         &MCIndexSnapRounder::checkCorrectness>;

TEST(MCIndexSnapRounderTest, CheckCorrectnessHandlesEmptyInputWithoutLeaking) {
  PrecisionModel pm(1.0);
  MCIndexSnapRounder rounder(pm);
  auto check_fn = GetSnapRounderPrivate(CheckCorrectnessTag{});

  // Upstream MCIndexSnapRounder::checkCorrectness() owns only the outer
  // resultSegStrings vector and not its allocated split SegmentStrings, so
  // non-empty inputs leak; empty input exercises the method leak-free.
  std::vector<SegmentString*> empty_inputs;
  EXPECT_NO_THROW((rounder.*check_fn)(empty_inputs));
}

}  // namespace
}  // namespace geos_google
