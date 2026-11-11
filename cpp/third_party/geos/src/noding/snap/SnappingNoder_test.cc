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

#include "third_party/geos/include/geos/noding/snap/SnappingNoder.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/noding/NodedSegmentString.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::noding::NodedSegmentString;
using geos::noding::SegmentString;
using geos::noding::snap::SnappingNoder;

struct NodedSubstringsDeleter {
  void operator()(std::vector<SegmentString*>* vec) const {
    if (vec != nullptr) {
      for (SegmentString* ss : *vec) {
        delete ss;
      }
      delete vec;
    }
  }
};

using OwnedSubstrings =
    std::unique_ptr<std::vector<SegmentString*>, NodedSubstringsDeleter>;

std::unique_ptr<NodedSegmentString> MakeSegmentString(
    std::initializer_list<Coordinate> coords, const void* context = nullptr) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  return std::make_unique<NodedSegmentString>(seq.release(), false, false,
                                              context);
}

void ExpectCoords2D(const SegmentString* ss,
                    std::initializer_list<CoordinateXY> expected) {
  ASSERT_NE(ss, nullptr);
  ASSERT_EQ(ss->size(), expected.size());
  std::size_t idx = 0;
  for (const CoordinateXY& exp : expected) {
    const Coordinate& actual = ss->getCoordinate(idx);
    EXPECT_DOUBLE_EQ(actual.x, exp.x) << "at index " << idx;
    EXPECT_DOUBLE_EQ(actual.y, exp.y) << "at index " << idx;
    ++idx;
  }
}

TEST(SnappingNoderTest, EmptyInputProducesEmptyOutput) {
  SnappingNoder noder(0.1);
  std::vector<SegmentString*> input;

  noder.computeNodes(&input);
  OwnedSubstrings result(noder.getNodedSubstrings());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->empty());
}

TEST(SnappingNoderTest, SingleNonIntersectingSegmentStringPreservesContext) {
  int context = 42;
  auto ss = MakeSegmentString({{0.0, 0.0}, {5.0, 0.0}, {10.0, 5.0}}, &context);
  std::vector<SegmentString*> input = {ss.get()};

  SnappingNoder noder(0.01);
  noder.computeNodes(&input);
  OwnedSubstrings result(noder.getNodedSubstrings());

  ASSERT_NE(result, nullptr);
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ((*result)[0]->getData(), &context);
  ExpectCoords2D((*result)[0], {{0.0, 0.0}, {5.0, 0.0}, {10.0, 5.0}});
}

TEST(SnappingNoderTest, CrossingSegmentsSplitAtIntersectionAndKeepContexts) {
  int ctx_a = 1;
  int ctx_b = 2;
  auto ss_a = MakeSegmentString({{0.0, 0.0}, {10.0, 10.0}}, &ctx_a);
  auto ss_b = MakeSegmentString({{0.0, 10.0}, {10.0, 0.0}}, &ctx_b);
  std::vector<SegmentString*> input = {ss_a.get(), ss_b.get()};

  SnappingNoder noder(0.001);
  noder.computeNodes(&input);
  OwnedSubstrings result(noder.getNodedSubstrings());

  ASSERT_NE(result, nullptr);
  ASSERT_EQ(result->size(), 4u);
  EXPECT_EQ((*result)[0]->getData(), &ctx_a);
  EXPECT_EQ((*result)[1]->getData(), &ctx_a);
  EXPECT_EQ((*result)[2]->getData(), &ctx_b);
  EXPECT_EQ((*result)[3]->getData(), &ctx_b);

  ExpectCoords2D((*result)[0], {{0.0, 0.0}, {5.0, 5.0}});
  ExpectCoords2D((*result)[1], {{5.0, 5.0}, {10.0, 10.0}});
  ExpectCoords2D((*result)[2], {{0.0, 10.0}, {5.0, 5.0}});
  ExpectCoords2D((*result)[3], {{5.0, 5.0}, {10.0, 0.0}});
}

TEST(SnappingNoderTest, SnapsVerticesAndPrioritizesVerticesOverIntersections) {
  // Vertex-to-vertex snapping: (10.04, 0.03) is 0.05 away from (10, 0).
  {
    auto ss0 = MakeSegmentString({{0.0, 0.0}, {10.0, 0.0}});
    auto ss1 = MakeSegmentString({{10.04, 0.03}, {20.0, 0.0}});
    std::vector<SegmentString*> input = {ss0.get(), ss1.get()};

    SnappingNoder noder(0.1);
    noder.computeNodes(&input);
    OwnedSubstrings result(noder.getNodedSubstrings());

    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->size(), 2u);
    ExpectCoords2D((*result)[0], {{0.0, 0.0}, {10.0, 0.0}});
    ExpectCoords2D((*result)[1], {{10.0, 0.0}, {20.0, 0.0}});
  }

  // Vertex priority over intersection: crossing at (5, 0) snaps to nearby
  // existing vertex (5.03, 0.04) within snapTolerance = 0.1.
  {
    auto ss0 = MakeSegmentString({{0.0, 0.0}, {10.0, 0.0}});
    auto cross = MakeSegmentString({{5.0, -5.0}, {5.0, 5.0}});
    auto anchor = MakeSegmentString({{5.03, 0.04}, {7.0, 2.0}});
    std::vector<SegmentString*> input = {ss0.get(), cross.get(), anchor.get()};

    SnappingNoder noder(0.1);
    noder.computeNodes(&input);
    OwnedSubstrings result(noder.getNodedSubstrings());

    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->size(), 5u);
    ExpectCoords2D((*result)[0], {{0.0, 0.0}, {5.03, 0.04}});
    ExpectCoords2D((*result)[1], {{5.03, 0.04}, {10.0, 0.0}});
    ExpectCoords2D((*result)[2], {{5.0, -5.0}, {5.03, 0.04}});
    ExpectCoords2D((*result)[3], {{5.03, 0.04}, {5.0, 5.0}});
    ExpectCoords2D((*result)[4], {{5.03, 0.04}, {7.0, 2.0}});
  }
}

TEST(SnappingNoderTest, NearVertexToSegmentInteriorNodesOnlyWithinTolerance) {
  auto make_inputs = []() {
    std::vector<std::unique_ptr<NodedSegmentString>> owned;
    owned.push_back(MakeSegmentString({{0.0, 0.0}, {10.0, 0.0}}));
    owned.push_back(MakeSegmentString({{5.0, 0.05}, {5.0, 10.0}}));
    return owned;
  };

  // Within snapTolerance (0.05 < 0.1): target segment splits at (5, 0.05).
  {
    auto owned = make_inputs();
    std::vector<SegmentString*> input = {owned[0].get(), owned[1].get()};
    SnappingNoder noder(0.1);
    noder.computeNodes(&input);
    OwnedSubstrings result(noder.getNodedSubstrings());

    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->size(), 3u);
    ExpectCoords2D((*result)[0], {{0.0, 0.0}, {5.0, 0.05}});
    ExpectCoords2D((*result)[1], {{5.0, 0.05}, {10.0, 0.0}});
    ExpectCoords2D((*result)[2], {{5.0, 0.05}, {5.0, 10.0}});
  }

  // Outside snapTolerance (0.05 > 0.04): no split occurs.
  {
    auto owned = make_inputs();
    std::vector<SegmentString*> input = {owned[0].get(), owned[1].get()};
    SnappingNoder noder(0.04);
    noder.computeNodes(&input);
    OwnedSubstrings result(noder.getNodedSubstrings());

    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->size(), 2u);
    ExpectCoords2D((*result)[0], {{0.0, 0.0}, {10.0, 0.0}});
    ExpectCoords2D((*result)[1], {{5.0, 0.05}, {5.0, 10.0}});
  }
}

TEST(SnappingNoderTest, RemovesRepeatedAndCollapsedVerticesWithinTolerance) {
  auto ss = MakeSegmentString(
      {{0.0, 0.0}, {0.02, 0.01}, {5.0, 0.0}, {5.03, 0.02}, {10.0, 0.0}});
  // Entire segment collapses to a single coordinate after snapping.
  auto collapsed = MakeSegmentString({{20.0, 0.0}, {20.02, 0.02}});
  auto intact = MakeSegmentString({{30.0, 0.0}, {30.0, 5.0}});
  std::vector<SegmentString*> input = {ss.get(), collapsed.get(), intact.get()};

  SnappingNoder noder(0.1);
  noder.computeNodes(&input);
  OwnedSubstrings result(noder.getNodedSubstrings());

  ASSERT_NE(result, nullptr);
  ASSERT_EQ(result->size(), 2u);
  ExpectCoords2D((*result)[0], {{0.0, 0.0}, {5.0, 0.0}, {10.0, 0.0}});
  ExpectCoords2D((*result)[1], {{30.0, 0.0}, {30.0, 5.0}});
}

TEST(SnappingNoderTest, LargeSegmentStringExercisesSeedSnapIndex) {
  // >= 200 vertices ensures seedSnapIndex executes multiple iterations,
  // covering both rand <= 1 and rand > 1 (wrap-around) branches.
  auto zigzag_seq = std::make_unique<CoordinateSequence>();
  constexpr std::size_t kVertexCount = 250;
  zigzag_seq->reserve(kVertexCount);
  for (std::size_t i = 0; i < kVertexCount; ++i) {
    zigzag_seq->add(
        Coordinate(static_cast<double>(i), (i % 2 == 0) ? 0.0 : 1.0));
  }
  auto zigzag = std::make_unique<NodedSegmentString>(zigzag_seq.release(),
                                                     false, false, nullptr);
  auto horizontal = MakeSegmentString({{0.0, 0.5}, {249.0, 0.5}});
  std::vector<SegmentString*> input = {zigzag.get(), horizontal.get()};

  SnappingNoder noder(0.01);
  noder.computeNodes(&input);
  OwnedSubstrings result(noder.getNodedSubstrings());

  // 249 zigzag segments each cross y = 0.5 once in their interior:
  // zigzag splits at the 249 nodes into 250 substrings, and horizontal
  // splits into 250 substrings, yielding 500 substrings total.
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->size(), 500u);
  ExpectCoords2D((*result)[0], {{0.0, 0.0}, {0.5, 0.5}});
  ExpectCoords2D((*result)[1], {{0.5, 0.5}, {1.0, 1.0}, {1.5, 0.5}});
  ExpectCoords2D((*result)[250], {{0.0, 0.5}, {0.5, 0.5}});
  ExpectCoords2D((*result)[499], {{248.5, 0.5}, {249.0, 0.5}});
}

TEST(SnappingNoderTest, CollinearOverlapAndSelfIntersection) {
  auto ss0 = MakeSegmentString({{0.0, 0.0}, {10.0, 0.0}});
  auto ss1 = MakeSegmentString({{3.0, 0.0}, {7.0, 0.0}});
  auto self_xing =
      MakeSegmentString({{0.0, 10.0}, {10.0, 10.0}, {5.0, 15.0}, {5.0, 5.0}});
  std::vector<SegmentString*> input = {ss0.get(), ss1.get(), self_xing.get()};

  SnappingNoder noder(0.01);
  noder.computeNodes(&input);
  OwnedSubstrings result(noder.getNodedSubstrings());

  ASSERT_NE(result, nullptr);
  ASSERT_EQ(result->size(), 7u);
  ExpectCoords2D((*result)[0], {{0.0, 0.0}, {3.0, 0.0}});
  ExpectCoords2D((*result)[1], {{3.0, 0.0}, {7.0, 0.0}});
  ExpectCoords2D((*result)[2], {{7.0, 0.0}, {10.0, 0.0}});
  ExpectCoords2D((*result)[3], {{3.0, 0.0}, {7.0, 0.0}});
  ExpectCoords2D((*result)[4], {{0.0, 10.0}, {5.0, 10.0}});
  ExpectCoords2D((*result)[5],
                 {{5.0, 10.0}, {10.0, 10.0}, {5.0, 15.0}, {5.0, 10.0}});
  ExpectCoords2D((*result)[6], {{5.0, 10.0}, {5.0, 5.0}});
}

TEST(SnappingNoderTest, ZeroToleranceNodesExactCrossingsOnly) {
  auto ss0 = MakeSegmentString({{0.0, 0.0}, {10.0, 0.0}});
  auto ss1 = MakeSegmentString({{5.0, -5.0}, {5.0, 5.0}});
  auto near_only = MakeSegmentString({{2.0, 1e-6}, {2.0, 5.0}});
  std::vector<SegmentString*> input = {ss0.get(), ss1.get(), near_only.get()};

  SnappingNoder noder(0.0);
  noder.computeNodes(&input);
  OwnedSubstrings result(noder.getNodedSubstrings());

  ASSERT_NE(result, nullptr);
  ASSERT_EQ(result->size(), 5u);
  ExpectCoords2D((*result)[0], {{0.0, 0.0}, {5.0, 0.0}});
  ExpectCoords2D((*result)[1], {{5.0, 0.0}, {10.0, 0.0}});
  ExpectCoords2D((*result)[2], {{5.0, -5.0}, {5.0, 0.0}});
  ExpectCoords2D((*result)[3], {{5.0, 0.0}, {5.0, 5.0}});
  ExpectCoords2D((*result)[4], {{2.0, 1e-6}, {2.0, 5.0}});
}

TEST(SnappingNoderTest, SupportsXYAndXYZCoordinateSequences) {
  // XY sequence (GEOS_COORDSEQ_PADZ stores stride 3 with NaN Z).
  auto xy_seq0 = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{{0.0, 0.0}, {10.0, 10.0}});
  auto xy_seq1 = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{{0.0, 10.0}, {10.0, 0.0}});
  auto xy_ss0 = std::make_unique<NodedSegmentString>(xy_seq0.release(), false,
                                                     false, nullptr);
  auto xy_ss1 = std::make_unique<NodedSegmentString>(xy_seq1.release(), false,
                                                     false, nullptr);
  std::vector<SegmentString*> xy_input = {xy_ss0.get(), xy_ss1.get()};

  SnappingNoder xy_noder(0.01);
  xy_noder.computeNodes(&xy_input);
  OwnedSubstrings xy_result(xy_noder.getNodedSubstrings());
  ASSERT_NE(xy_result, nullptr);
  ASSERT_EQ(xy_result->size(), 4u);
  ExpectCoords2D((*xy_result)[0], {{0.0, 0.0}, {5.0, 5.0}});
  EXPECT_TRUE(std::isnan((*xy_result)[0]->getCoordinate(1).z));

  // XYZ sequence preserves Z values on Coordinate storage.
  auto z_ss0 = MakeSegmentString({{0.0, 0.0, 10.0}, {10.0, 10.0, 20.0}});
  auto z_ss1 = MakeSegmentString({{0.0, 10.0, 30.0}, {10.0, 0.0, 40.0}});
  std::vector<SegmentString*> z_input = {z_ss0.get(), z_ss1.get()};

  SnappingNoder z_noder(0.01);
  z_noder.computeNodes(&z_input);
  OwnedSubstrings z_result(z_noder.getNodedSubstrings());
  ASSERT_NE(z_result, nullptr);
  ASSERT_EQ(z_result->size(), 4u);
  ExpectCoords2D((*z_result)[0], {{0.0, 0.0}, {5.0, 5.0}});
  EXPECT_DOUBLE_EQ((*z_result)[0]->getCoordinate(0).z, 10.0);
  EXPECT_DOUBLE_EQ((*z_result)[0]->getCoordinate(1).z, 25.0);
  EXPECT_DOUBLE_EQ((*z_result)[1]->getCoordinate(1).z, 20.0);
}

}  // namespace
}  // namespace geos_google
