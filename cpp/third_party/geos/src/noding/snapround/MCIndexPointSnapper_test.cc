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

// Unit tests for third_party/geos/src/noding/snapround/MCIndexPointSnapper.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/noding/snapround/MCIndexPointSnapper.cpp

#include "third_party/geos/include/geos/noding/snapround/MCIndexPointSnapper.h"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/index/chain/MonotoneChain.h"
#include "third_party/geos/include/geos/index/chain/MonotoneChainBuilder.h"
#include "third_party/geos/include/geos/index/strtree/STRtree.h"
#include "third_party/geos/include/geos/index/strtree/TemplateSTRtree.h"
#include "third_party/geos/include/geos/noding/NodedSegmentString.h"
#include "third_party/geos/include/geos/noding/SegmentNode.h"
#include "third_party/geos/include/geos/noding/SegmentNodeList.h"
#include "third_party/geos/include/geos/noding/snapround/HotPixel.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYZM;
using geos::geom::Envelope;
using geos::index::chain::MonotoneChain;
using geos::index::chain::MonotoneChainBuilder;
using geos::index::strtree::STRtree;
using geos::index::strtree::TemplateSTRtree;
using geos::noding::NodedSegmentString;
using geos::noding::SegmentNode;
using geos::noding::snapround::HotPixel;
using geos::noding::snapround::MCIndexPointSnapper;

std::unique_ptr<NodedSegmentString> MakeSegmentString(
    std::initializer_list<CoordinateXY> coords) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  return std::make_unique<NodedSegmentString>(seq.release(), false, false,
                                              nullptr);
}

std::unique_ptr<NodedSegmentString> MakeSegmentStringXYZM(
    std::initializer_list<CoordinateXYZM> coords) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  return std::make_unique<NodedSegmentString>(seq.release(), true, true,
                                              nullptr);
}

struct ChainIndexHolder {
  std::vector<MonotoneChain> chains;
  TemplateSTRtree<MonotoneChain*> index;

  void build(const std::vector<NodedSegmentString*>& seg_strings) {
    chains.clear();
    for (NodedSegmentString* ss : seg_strings) {
      MonotoneChainBuilder::getChains(ss->getCoordinates(), ss, chains);
    }
    for (MonotoneChain& mc : chains) {
      index.insert(mc.getEnvelope(), &mc);
    }
  }
};

TEST(MCIndexPointSnapperTest, GetSafeEnvelopeExpandsAroundOriginalCoordinate) {
  TemplateSTRtree<MonotoneChain*> empty_index;
  MCIndexPointSnapper snapper(empty_index);

  const HotPixel hp_unit(CoordinateXY(2.0, 3.0), 1.0);
  const Envelope env_unit = snapper.getSafeEnvelope(hp_unit);
  EXPECT_DOUBLE_EQ(env_unit.getMinX(), 1.25);
  EXPECT_DOUBLE_EQ(env_unit.getMaxX(), 2.75);
  EXPECT_DOUBLE_EQ(env_unit.getMinY(), 2.25);
  EXPECT_DOUBLE_EQ(env_unit.getMaxY(), 3.75);

  // Safe envelope uses the unrounded original coordinate and 0.75 /
  // scaleFactor.
  const HotPixel hp_scaled(CoordinateXY(1.27, 2.73), 4.0);
  const Envelope env_scaled = snapper.getSafeEnvelope(hp_scaled);
  EXPECT_DOUBLE_EQ(env_scaled.getMinX(), 1.27 - 0.1875);
  EXPECT_DOUBLE_EQ(env_scaled.getMaxX(), 1.27 + 0.1875);
  EXPECT_DOUBLE_EQ(env_scaled.getMinY(), 2.73 - 0.1875);
  EXPECT_DOUBLE_EQ(env_scaled.getMaxY(), 2.73 + 0.1875);
}

TEST(MCIndexPointSnapperTest, EmptyIndexNeverSnaps) {
  TemplateSTRtree<MonotoneChain*> empty_index;
  MCIndexPointSnapper snapper(empty_index);

  HotPixel hp(CoordinateXY(0.0, 0.0), 1.0);
  EXPECT_FALSE(snapper.snap(hp));
  EXPECT_FALSE(snapper.snap(hp, nullptr, 0));
}

TEST(MCIndexPointSnapperTest, SnapDistinguishesTrueHitFromSafeEnvelopeOnlyHit) {
  auto hit_edge = MakeSegmentString({{0.0, 0.0}, {10.0, 0.0}});
  // Lies within getSafeEnvelope [-0.75, 0.75] of (0, 0), and even outside
  // HotPixel [-0.5, 0.5) at x = 0.6.
  auto near_miss_edge = MakeSegmentString({{0.6, -2.0}, {0.6, 2.0}});
  // Lies on the open right boundary x = 0.5 of HotPixel(0, 0).
  auto open_edge = MakeSegmentString({{0.5, -2.0}, {0.5, 2.0}});
  auto far_edge = MakeSegmentString({{0.0, 10.0}, {10.0, 10.0}});

  ChainIndexHolder holder;
  holder.build(
      {hit_edge.get(), near_miss_edge.get(), open_edge.get(), far_edge.get()});
  MCIndexPointSnapper snapper(holder.index);

  // Near-miss and open-boundary segments do not trigger a node when tested
  // without hit_edge in the index.
  HotPixel hp_miss(CoordinateXY(0.0, 0.0), 1.0);
  {
    ChainIndexHolder miss_holder;
    miss_holder.build({near_miss_edge.get(), open_edge.get(), far_edge.get()});
    MCIndexPointSnapper miss_snapper(miss_holder.index);
    EXPECT_FALSE(miss_snapper.snap(hp_miss));
    EXPECT_FALSE(near_miss_edge->hasNodes());
    EXPECT_FALSE(open_edge->hasNodes());
    EXPECT_FALSE(far_edge->hasNodes());
  }

  // Hot pixel at (5.0, 0.0) intersects hit_edge only.
  HotPixel hp_hit(CoordinateXY(5.0, 0.0), 1.0);
  EXPECT_TRUE(snapper.snap(hp_hit));
  EXPECT_TRUE(hit_edge->hasNodes());
  EXPECT_FALSE(near_miss_edge->hasNodes());
  EXPECT_FALSE(open_edge->hasNodes());
  EXPECT_FALSE(far_edge->hasNodes());

  ASSERT_EQ(hit_edge->getNodeList().size(), 1);
  const SegmentNode& node = *hit_edge->getNodeList().begin();
  EXPECT_EQ(node.segmentIndex, 0);
  EXPECT_DOUBLE_EQ(node.coord.x, 5.0);
  EXPECT_DOUBLE_EQ(node.coord.y, 0.0);
}

TEST(MCIndexPointSnapperTest, SkipsIncidentSegmentsOfParentEdgeOnly) {
  // edge_a vertices:
  //   0: (0.0, 0.0)
  //   1: (5.0, 0.0)
  //   2: (5.0, 5.0)
  //   3: (0.2, 0.2) -> non-incident segment 2 ends inside pixel around vertex
  //   0.
  auto edge_a =
      MakeSegmentString({{0.0, 0.0}, {5.0, 0.0}, {5.0, 5.0}, {0.2, 0.2}});
  // edge_b passes through vertex 1 (5.0, 0.0) of edge_a.
  auto edge_b = MakeSegmentString({{5.0, -3.0}, {5.0, 3.0}});

  ChainIndexHolder holder;
  holder.build({edge_a.get(), edge_b.get()});
  MCIndexPointSnapper snapper(holder.index);

  // Vertex 1 (5.0, 0.0): both segment 0 (startIndex + 1 == 1) and segment 1
  // (startIndex == 1) of parent edge_a must be skipped, while edge_b is noded.
  HotPixel hp_v1(CoordinateXY(5.0, 0.0), 1.0);
  EXPECT_TRUE(snapper.snap(hp_v1, edge_a.get(), 1));
  EXPECT_FALSE(edge_a->hasNodes());
  ASSERT_EQ(edge_b->getNodeList().size(), 1);
  EXPECT_DOUBLE_EQ(edge_b->getNodeList().begin()->coord.x, 5.0);
  EXPECT_DOUBLE_EQ(edge_b->getNodeList().begin()->coord.y, 0.0);

  // Vertex 0 (0.0, 0.0): incident segment 0 of edge_a is skipped, but
  // non-incident segment 2 of edge_a intersects the pixel and must be noded.
  HotPixel hp_v0(CoordinateXY(0.0, 0.0), 1.0);
  EXPECT_TRUE(snapper.snap(hp_v0, edge_a.get(), 0));
  ASSERT_EQ(edge_a->getNodeList().size(), 1);
  const SegmentNode& self_node = *edge_a->getNodeList().begin();
  EXPECT_EQ(self_node.segmentIndex, 2);
  EXPECT_DOUBLE_EQ(self_node.coord.x, 0.0);
  EXPECT_DOUBLE_EQ(self_node.coord.y, 0.0);
}

TEST(MCIndexPointSnapperTest,
     ParentEdgeSkipsBothIncidentSegmentsWithoutOtherHit) {
  auto edge = MakeSegmentString({{0.0, 0.0}, {4.0, 0.0}, {4.0, 4.0}});
  ChainIndexHolder holder;
  holder.build({edge.get()});
  MCIndexPointSnapper snapper(holder.index);

  HotPixel hp_mid(CoordinateXY(4.0, 0.0), 1.0);
  EXPECT_FALSE(snapper.snap(hp_mid, edge.get(), 1));
  EXPECT_FALSE(edge->hasNodes());
}

TEST(MCIndexPointSnapperTest, MultiSegmentMonotoneChainAndZMPreservation) {
  // Single monotone chain with 4 segments forces MonotoneChain binary search.
  auto chain_edge = MakeSegmentStringXYZM({
      {0.0, 0.0, 1.0, 10.0},
      {2.0, 1.0, 2.0, 20.0},
      {4.0, 2.0, 3.0, 30.0},
      {6.0, 3.0, 4.0, 40.0},
      {8.0, 4.0, 5.0, 50.0},
  });
  // Near-miss segment overlaps safe envelope around (3.0, 1.5) (y <= 2.25)
  // without intersecting HotPixel [2.5, 3.5) x [1.0, 2.0).
  auto near_miss = MakeSegmentString({{3.0, 2.15}, {3.0, 3.0}});

  ChainIndexHolder holder;
  holder.build({chain_edge.get(), near_miss.get()});
  MCIndexPointSnapper snapper(holder.index);

  HotPixel hp1(CoordinateXYZM(3.0, 1.5, 42.0, 77.0), 1.0);
  EXPECT_TRUE(snapper.snap(hp1));
  EXPECT_FALSE(near_miss->hasNodes());

  HotPixel hp2(CoordinateXYZM(7.0, 3.5, 99.0, 88.0), 1.0);
  EXPECT_TRUE(snapper.snap(hp2));

  ASSERT_EQ(chain_edge->getNodeList().size(), 2);
  auto it = chain_edge->getNodeList().begin();
  EXPECT_EQ(it->segmentIndex, 1);
  EXPECT_DOUBLE_EQ(it->coord.x, 3.0);
  EXPECT_DOUBLE_EQ(it->coord.y, 1.5);
  EXPECT_DOUBLE_EQ(it->coord.z, 42.0);
  EXPECT_DOUBLE_EQ(it->coord.m, 77.0);
  ++it;
  EXPECT_EQ(it->segmentIndex, 3);
  EXPECT_DOUBLE_EQ(it->coord.x, 7.0);
  EXPECT_DOUBLE_EQ(it->coord.y, 3.5);
  EXPECT_DOUBLE_EQ(it->coord.z, 99.0);
  EXPECT_DOUBLE_EQ(it->coord.m, 88.0);

  auto noded = chain_edge->getNodedCoordinates();
  EXPECT_EQ(noded->size(), 7);
}

TEST(MCIndexPointSnapperTest, WorksWithSTRtreeAndNonUnitScaleFactor) {
  auto edge = MakeSegmentString({{0.0, 0.0}, {1.0, 0.0}});
  std::vector<MonotoneChain> chains;
  MonotoneChainBuilder::getChains(edge->getCoordinates(), edge.get(), chains);

  STRtree strtree;
  for (MonotoneChain& mc : chains) {
    strtree.insert(&mc.getEnvelope(), &mc);
  }
  MCIndexPointSnapper snapper(strtree);

  // Pixel width 0.01: (0.50, 0.006) rounds to scaled (50, 1), covering
  // y in [0.005, 0.015), which misses the segment on y = 0.0.
  HotPixel hp_miss(CoordinateXY(0.50, 0.006), 100.0);
  EXPECT_FALSE(snapper.snap(hp_miss));
  EXPECT_FALSE(edge->hasNodes());

  // (0.50, 0.004) rounds to scaled (50, 0), covering y in [-0.005, 0.005),
  // which intersects the segment on y = 0.0.
  HotPixel hp_hit(CoordinateXY(0.50, 0.004), 100.0);
  EXPECT_TRUE(snapper.snap(hp_hit));
  ASSERT_EQ(edge->getNodeList().size(), 1);
  EXPECT_DOUBLE_EQ(edge->getNodeList().begin()->coord.x, 0.50);
  EXPECT_DOUBLE_EQ(edge->getNodeList().begin()->coord.y, 0.004);
}

}  // namespace
}  // namespace geos_google
