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

// Unit tests for third_party/geos/src/index/chain/MonotoneChainBuilder.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/index/chain/MonotoneChainBuilder.cpp

#include "third_party/geos/include/geos/index/chain/MonotoneChainBuilder.h"

#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/LineSegment.h"
#include "third_party/geos/include/geos/index/chain/MonotoneChain.h"
#include "third_party/geos/include/geos/index/chain/MonotoneChainSelectAction.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateXY;
using ::geos::geom::CoordinateXYM;
using ::geos::geom::CoordinateXYZM;
using ::geos::geom::Envelope;
using ::geos::geom::LineSegment;
using ::geos::index::chain::MonotoneChain;
using ::geos::index::chain::MonotoneChainBuilder;
using ::geos::index::chain::MonotoneChainSelectAction;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Pair;

static_assert(!std::is_copy_constructible_v<MonotoneChainBuilder>);
static_assert(!std::is_copy_assignable_v<MonotoneChainBuilder>);

std::vector<std::pair<std::size_t, std::size_t>> ExtractRanges(
    const std::vector<MonotoneChain>& chains) {
  std::vector<std::pair<std::size_t, std::size_t>> ranges;
  ranges.reserve(chains.size());
  for (const MonotoneChain& chain : chains) {
    ranges.emplace_back(chain.getStartIndex(), chain.getEndIndex());
  }
  return ranges;
}

class CountingSelectAction : public MonotoneChainSelectAction {
 public:
  void select(const LineSegment& seg) override { segments.push_back(seg); }

  std::vector<LineSegment> segments;
};

TEST(MonotoneChainBuilderTest, EmptySequenceProducesNoChains) {
  CoordinateSequence seq = CoordinateSequence::XY(0);
  std::vector<MonotoneChain> chains;
  MonotoneChainBuilder::getChains(&seq, nullptr, chains);
  EXPECT_THAT(chains, IsEmpty());
}

TEST(MonotoneChainBuilderTest, SingleCoordinateProducesDegenerateChain) {
  CoordinateSequence seq{CoordinateXY(3.0, 4.0)};
  int ctx = 11;
  std::vector<MonotoneChain> chains;
  MonotoneChainBuilder::getChains(&seq, &ctx, chains);

  EXPECT_THAT(ExtractRanges(chains), ElementsAre(Pair(0u, 0u)));
  EXPECT_EQ(chains[0].getContext(), &ctx);
}

TEST(MonotoneChainBuilderTest, AllRepeatedCoordinatesProduceSingleChain) {
  CoordinateSequence seq{
      CoordinateXY(2.0, 5.0),
      CoordinateXY(2.0, 5.0),
      CoordinateXY(2.0, 5.0),
  };
  std::vector<MonotoneChain> chains;
  MonotoneChainBuilder::getChains(&seq, nullptr, chains);

  EXPECT_THAT(ExtractRanges(chains), ElementsAre(Pair(0u, 2u)));
  EXPECT_EQ(chains[0].getContext(), nullptr);
}

TEST(MonotoneChainBuilderTest, RepeatedPointsWithinAndAtTransitionHandled) {
  CoordinateSequence seq{
      CoordinateXY(0.0, 0.0), CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0),
      CoordinateXY(1.0, 1.0), CoordinateXY(2.0, 2.0), CoordinateXY(2.0, 2.0),
      CoordinateXY(1.0, 3.0), CoordinateXY(1.0, 3.0),
  };
  std::vector<MonotoneChain> chains;
  MonotoneChainBuilder::getChains(&seq, nullptr, chains);

  // Leading duplicates and mid-chain duplicates stay in the NE chain [0, 5],
  // then the transition to NW splits at index 5, forming [5, 7].
  EXPECT_THAT(ExtractRanges(chains), ElementsAre(Pair(0u, 5u), Pair(5u, 7u)));
}

TEST(MonotoneChainBuilderTest, SingleQuadrantPathsProduceSingleChainEach) {
  CoordinateSequence ne_seq{
      CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 0.0), CoordinateXY(2.0, 3.0),
      CoordinateXY(5.0, 3.0), CoordinateXY(7.0, 8.0),
  };
  CoordinateSequence nw_seq{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(-1.0, 0.0),
      CoordinateXY(-2.0, 3.0),
  };
  CoordinateSequence sw_seq{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(-1.0, -1.0),
      CoordinateXY(-3.0, -5.0),
  };
  CoordinateSequence se_seq{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(0.0, -2.0),
      CoordinateXY(3.0, -4.0),
  };

  std::vector<MonotoneChain> ne_chains;
  MonotoneChainBuilder::getChains(&ne_seq, nullptr, ne_chains);
  EXPECT_THAT(ExtractRanges(ne_chains), ElementsAre(Pair(0u, 4u)));

  std::vector<MonotoneChain> nw_chains;
  MonotoneChainBuilder::getChains(&nw_seq, nullptr, nw_chains);
  EXPECT_THAT(ExtractRanges(nw_chains), ElementsAre(Pair(0u, 2u)));

  std::vector<MonotoneChain> sw_chains;
  MonotoneChainBuilder::getChains(&sw_seq, nullptr, sw_chains);
  EXPECT_THAT(ExtractRanges(sw_chains), ElementsAre(Pair(0u, 2u)));

  std::vector<MonotoneChain> se_chains;
  MonotoneChainBuilder::getChains(&se_seq, nullptr, se_chains);
  EXPECT_THAT(ExtractRanges(se_chains), ElementsAre(Pair(0u, 2u)));
}

TEST(MonotoneChainBuilderTest, MultiQuadrantTransitionsSplitAtEachTurn) {
  CoordinateSequence seq{
      CoordinateXY(0.0, 0.0),    // 0
      CoordinateXY(2.0, 2.0),    // 1 (NE)
      CoordinateXY(4.0, 3.0),    // 2 (NE)
      CoordinateXY(2.0, 5.0),    // 3 (NW) -> split at 2
      CoordinateXY(0.0, 7.0),    // 4 (NW)
      CoordinateXY(-2.0, 5.0),   // 5 (SW) -> split at 4
      CoordinateXY(-4.0, 2.0),   // 6 (SW)
      CoordinateXY(-1.0, -1.0),  // 7 (SE) -> split at 6
      CoordinateXY(1.0, -3.0),   // 8 (SE)
      CoordinateXY(4.0, 0.0),    // 9 (NE) -> split at 8
  };
  int ctx = 99;
  std::vector<MonotoneChain> chains;
  MonotoneChainBuilder::getChains(&seq, &ctx, chains);

  EXPECT_THAT(ExtractRanges(chains),
              ElementsAre(Pair(0u, 2u), Pair(2u, 4u), Pair(4u, 6u),
                          Pair(6u, 8u), Pair(8u, 9u)));
  for (const MonotoneChain& chain : chains) {
    EXPECT_EQ(chain.getContext(), &ctx);
  }

  // Built chains remain selectable via envelope queries.
  CountingSelectAction action;
  chains[1].select(Envelope(1.5, 2.5, 4.5, 5.5), action);
  EXPECT_THAT(action.segments, ElementsAre(LineSegment(4.0, 3.0, 2.0, 5.0),
                                           LineSegment(2.0, 5.0, 0.0, 7.0)));
}

TEST(MonotoneChainBuilderTest, ClosedSquareRingSplitsByQuadrantConvention) {
  // (0,0)->(10,0)->(10,10) are NE (dx>=0, dy>=0),
  // (10,10)->(0,10) is NW (dx<0, dy>=0),
  // (0,10)->(0,0) is SE (dx=0 >= 0, dy<0).
  CoordinateSequence ring{
      CoordinateXY(0.0, 0.0),   CoordinateXY(10.0, 0.0),
      CoordinateXY(10.0, 10.0), CoordinateXY(0.0, 10.0),
      CoordinateXY(0.0, 0.0),
  };
  std::vector<MonotoneChain> chains;
  MonotoneChainBuilder::getChains(&ring, nullptr, chains);

  EXPECT_THAT(ExtractRanges(chains),
              ElementsAre(Pair(0u, 2u), Pair(2u, 3u), Pair(3u, 4u)));
}

TEST(MonotoneChainBuilderTest,
     AppendsToExistingChainVectorWithDistinctContexts) {
  CoordinateSequence seq1{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(1.0, 1.0),
      CoordinateXY(0.0, 2.0),
  };
  CoordinateSequence seq2{
      CoordinateXY(10.0, 10.0),
      CoordinateXY(12.0, 12.0),
  };
  int ctx1 = 1;
  int ctx2 = 2;

  std::vector<MonotoneChain> chains;
  MonotoneChainBuilder::getChains(&seq1, &ctx1, chains);
  MonotoneChainBuilder::getChains(&seq2, &ctx2, chains);

  EXPECT_THAT(ExtractRanges(chains),
              ElementsAre(Pair(0u, 1u), Pair(1u, 2u), Pair(0u, 1u)));
  EXPECT_EQ(chains[0].getContext(), &ctx1);
  EXPECT_EQ(chains[1].getContext(), &ctx1);
  EXPECT_EQ(chains[2].getContext(), &ctx2);
  EXPECT_TRUE(
      CoordinateSequence::equals(chains[0].getCoordinates().get(), &seq1));
  EXPECT_TRUE(
      CoordinateSequence::equals(chains[2].getCoordinates().get(), &seq2));
}

TEST(MonotoneChainBuilderTest, SupportsHigherDimensionalCoordinateSequences) {
  CoordinateSequence xyz_seq{
      Coordinate(0.0, 0.0, 5.0),
      Coordinate(2.0, 2.0, 10.0),
      Coordinate(0.0, 4.0, 15.0),
  };
  CoordinateSequence xym_seq{
      CoordinateXYM(0.0, 0.0, 1.0),
      CoordinateXYM(2.0, 2.0, 2.0),
      CoordinateXYM(0.0, 4.0, 3.0),
  };
  CoordinateSequence xyzm_seq{
      CoordinateXYZM(0.0, 0.0, 5.0, 1.0),
      CoordinateXYZM(2.0, 2.0, 10.0, 2.0),
      CoordinateXYZM(0.0, 4.0, 15.0, 3.0),
  };

  std::vector<MonotoneChain> xyz_chains;
  MonotoneChainBuilder::getChains(&xyz_seq, nullptr, xyz_chains);
  EXPECT_THAT(ExtractRanges(xyz_chains),
              ElementsAre(Pair(0u, 1u), Pair(1u, 2u)));

  std::vector<MonotoneChain> xym_chains;
  MonotoneChainBuilder::getChains(&xym_seq, nullptr, xym_chains);
  EXPECT_THAT(ExtractRanges(xym_chains),
              ElementsAre(Pair(0u, 1u), Pair(1u, 2u)));

  std::vector<MonotoneChain> xyzm_chains;
  MonotoneChainBuilder::getChains(&xyzm_seq, nullptr, xyzm_chains);
  EXPECT_THAT(ExtractRanges(xyzm_chains),
              ElementsAre(Pair(0u, 1u), Pair(1u, 2u)));
}

}  // namespace
}  // namespace geos_google
