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

#include "third_party/geos/include/geos/geomgraph/index/MonotoneChainIndexer.h"

#include <cstddef>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geomgraph::index::MonotoneChainIndexer;
using geos::util::IllegalArgumentException;
using testing::ElementsAre;

TEST(MonotoneChainIndexerTest, SingleSegmentReturnsSingleChain) {
  CoordinateSequence seq({Coordinate(0.0, 0.0), Coordinate(3.0, 4.0)});
  MonotoneChainIndexer indexer;
  std::vector<std::size_t> indices;

  indexer.getChainStartIndices(&seq, indices);

  EXPECT_THAT(indices, ElementsAre(0, 1));
}

TEST(MonotoneChainIndexerTest, MultiSegmentSingleChainInEachQuadrant) {
  MonotoneChainIndexer indexer;

  // NE quadrant: dx >= 0, dy >= 0 (includes horizontal and vertical segments).
  {
    CoordinateSequence ne_seq({
        Coordinate(0.0, 0.0),
        Coordinate(2.0, 0.0),
        Coordinate(2.0, 3.0),
        Coordinate(5.0, 6.0),
    });
    std::vector<std::size_t> indices;
    indexer.getChainStartIndices(&ne_seq, indices);
    EXPECT_THAT(indices, ElementsAre(0, 3));
  }

  // NW quadrant: dx < 0, dy >= 0.
  {
    CoordinateSequence nw_seq({
        Coordinate(6.0, 0.0),
        Coordinate(4.0, 0.0),
        Coordinate(2.0, 2.0),
        Coordinate(0.0, 5.0),
    });
    std::vector<std::size_t> indices;
    indexer.getChainStartIndices(&nw_seq, indices);
    EXPECT_THAT(indices, ElementsAre(0, 3));
  }

  // SW quadrant: dx < 0, dy < 0.
  {
    CoordinateSequence sw_seq({
        Coordinate(5.0, 5.0),
        Coordinate(3.0, 4.0),
        Coordinate(1.0, 2.0),
        Coordinate(-2.0, -1.0),
    });
    std::vector<std::size_t> indices;
    indexer.getChainStartIndices(&sw_seq, indices);
    EXPECT_THAT(indices, ElementsAre(0, 3));
  }

  // SE quadrant: dx >= 0, dy < 0 (includes vertical downward dx == 0).
  {
    CoordinateSequence se_seq({
        Coordinate(0.0, 6.0),
        Coordinate(1.0, 5.0),
        Coordinate(1.0, 2.0),
        Coordinate(4.0, 0.0),
    });
    std::vector<std::size_t> indices;
    indexer.getChainStartIndices(&se_seq, indices);
    EXPECT_THAT(indices, ElementsAre(0, 3));
  }
}

TEST(MonotoneChainIndexerTest,
     AxisAlignedTransitionsSplitChainsAtQuadrantBoundaries) {
  // Segments:
  // 0->1: (+1,  0) -> NE (dx >= 0, dy >= 0)
  // 1->2: ( 0, +1) -> NE (dx >= 0, dy >= 0)
  // 2->3: (-1,  0) -> NW (dx <  0, dy >= 0)
  // 3->4: ( 0, -1) -> SE (dx >= 0, dy <  0)
  // 4->5: (-1, -1) -> SW (dx <  0, dy <  0)
  CoordinateSequence seq({
      Coordinate(0.0, 0.0),
      Coordinate(1.0, 0.0),
      Coordinate(1.0, 1.0),
      Coordinate(0.0, 1.0),
      Coordinate(0.0, 0.0),
      Coordinate(-1.0, -1.0),
  });
  MonotoneChainIndexer indexer;
  std::vector<std::size_t> indices;

  indexer.getChainStartIndices(&seq, indices);

  EXPECT_THAT(indices, ElementsAre(0, 2, 3, 4, 5));
}

TEST(MonotoneChainIndexerTest, MultiSegmentClosedRingSplitsAtQuadrantChanges) {
  CoordinateSequence diamond({
      Coordinate(0.0, 0.0),
      Coordinate(1.0, 0.5),
      Coordinate(2.0, 2.0),
      Coordinate(1.0, 3.5),
      Coordinate(0.0, 4.0),
      Coordinate(-1.0, 3.5),
      Coordinate(-2.0, 2.0),
      Coordinate(-1.0, 0.5),
      Coordinate(0.0, 0.0),
  });
  MonotoneChainIndexer indexer;
  std::vector<std::size_t> indices;

  indexer.getChainStartIndices(&diamond, indices);

  EXPECT_THAT(indices, ElementsAre(0, 2, 4, 6, 8));
}

TEST(MonotoneChainIndexerTest, ZigZagSplitsAtEveryDirectionReversal) {
  CoordinateSequence zigzag({
      Coordinate(0.0, 0.0),
      Coordinate(1.0, 1.0),
      Coordinate(2.0, 0.0),
      Coordinate(3.0, 1.0),
      Coordinate(4.0, 0.0),
      Coordinate(5.0, 1.0),
  });
  MonotoneChainIndexer indexer;
  std::vector<std::size_t> indices;

  indexer.getChainStartIndices(&zigzag, indices);

  EXPECT_THAT(indices, ElementsAre(0, 1, 2, 3, 4, 5));
}

TEST(MonotoneChainIndexerTest, AppendsIndicesToNonEmptyVector) {
  CoordinateSequence seq({
      Coordinate(0.0, 0.0),
      Coordinate(1.0, 1.0),
      Coordinate(2.0, 2.0),
      Coordinate(1.0, 3.0),
  });
  MonotoneChainIndexer indexer;
  std::vector<std::size_t> indices = {42};

  indexer.getChainStartIndices(&seq, indices);

  EXPECT_THAT(indices, ElementsAre(42, 0, 2, 3));
}

TEST(MonotoneChainIndexerTest, RepeatedConsecutivePointsThrow) {
  MonotoneChainIndexer indexer;

  CoordinateSequence repeated_start({
      Coordinate(1.0, 1.0),
      Coordinate(1.0, 1.0),
      Coordinate(2.0, 2.0),
  });
  std::vector<std::size_t> start_indices;
  EXPECT_THROW(indexer.getChainStartIndices(&repeated_start, start_indices),
               IllegalArgumentException);

  CoordinateSequence repeated_middle({
      Coordinate(0.0, 0.0),
      Coordinate(1.0, 1.0),
      Coordinate(1.0, 1.0),
      Coordinate(2.0, 2.0),
  });
  std::vector<std::size_t> middle_indices;
  EXPECT_THROW(indexer.getChainStartIndices(&repeated_middle, middle_indices),
               IllegalArgumentException);
}

}  // namespace
}  // namespace geos_google
