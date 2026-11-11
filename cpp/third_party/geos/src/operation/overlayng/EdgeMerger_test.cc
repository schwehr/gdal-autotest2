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

#include "third_party/geos/include/geos/operation/overlayng/EdgeMerger.h"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Position.h"
#include "third_party/geos/include/geos/operation/overlayng/Edge.h"
#include "third_party/geos/include/geos/operation/overlayng/EdgeSourceInfo.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayLabel.h"
#include "third_party/geos/include/geos/util/AssertionFailedException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Location;
using geos::geom::Position;
using geos::operation::overlayng::Edge;
using geos::operation::overlayng::EdgeMerger;
using geos::operation::overlayng::EdgeSourceInfo;
using geos::operation::overlayng::OverlayLabel;
using geos::util::AssertionFailedException;

std::unique_ptr<Edge> MakeEdge(std::initializer_list<CoordinateXY> coords,
                               const EdgeSourceInfo& info) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  return std::make_unique<Edge>(std::move(seq), &info);
}

TEST(EdgeMergerTest, EmptyInputReturnsEmptyVector) {
  std::vector<Edge*> edges;
  const std::vector<Edge*> merged = EdgeMerger::merge(edges);
  EXPECT_TRUE(merged.empty());
}

TEST(EdgeMergerTest, DistinctEdgesPreserveInputOrder) {
  const EdgeSourceInfo info0(uint8_t{0});
  const EdgeSourceInfo info1(uint8_t{1});

  auto e0 = MakeEdge({{10.0, 0.0}, {20.0, 0.0}}, info0);
  auto e1 = MakeEdge({{0.0, 0.0}, {5.0, 0.0}}, info1);
  auto e2 = MakeEdge({{5.0, 0.0}, {10.0, 0.0}}, info0);

  std::vector<Edge*> edges = {e0.get(), e1.get(), e2.get()};
  const std::vector<Edge*> merged = EdgeMerger::merge(edges);

  ASSERT_EQ(merged.size(), 3u);
  EXPECT_EQ(merged[0], e0.get());
  EXPECT_EQ(merged[1], e1.get());
  EXPECT_EQ(merged[2], e2.get());
}

TEST(EdgeMergerTest, CoincidentAreaEdgesSameDirectionCombineLabels) {
  const EdgeSourceInfo info0(uint8_t{0}, 1, false);
  const EdgeSourceInfo info1(uint8_t{1}, 1, false);

  auto e0 = MakeEdge({{0.0, 0.0}, {5.0, 5.0}, {10.0, 0.0}}, info0);
  auto e1 = MakeEdge({{0.0, 0.0}, {5.0, 5.0}, {10.0, 0.0}}, info1);

  std::vector<Edge*> edges = {e0.get(), e1.get()};
  const std::vector<Edge*> merged = EdgeMerger::merge(edges);

  ASSERT_EQ(merged.size(), 1u);
  EXPECT_EQ(merged[0], e0.get());

  OverlayLabel label;
  merged[0]->populateLabel(label);
  EXPECT_TRUE(label.isBoundaryBoth());
  EXPECT_FALSE(label.isBoundaryTouch());
  EXPECT_FALSE(label.isHole(0));
  EXPECT_FALSE(label.isHole(1));
  EXPECT_EQ(label.getLocation(0, +Position::LEFT, true), Location::EXTERIOR);
  EXPECT_EQ(label.getLocation(0, +Position::RIGHT, true), Location::INTERIOR);
  EXPECT_EQ(label.getLocation(1, +Position::LEFT, true), Location::EXTERIOR);
  EXPECT_EQ(label.getLocation(1, +Position::RIGHT, true), Location::INTERIOR);
}

TEST(EdgeMergerTest, CoincidentAreaEdgesOppositeDirectionDetectTouch) {
  const EdgeSourceInfo info0(uint8_t{0}, 1, false);
  const EdgeSourceInfo info1(uint8_t{1}, 1, false);

  auto e0 = MakeEdge({{0.0, 0.0}, {5.0, 5.0}, {10.0, 0.0}}, info0);
  auto e1 = MakeEdge({{10.0, 0.0}, {5.0, 5.0}, {0.0, 0.0}}, info1);

  std::vector<Edge*> edges = {e0.get(), e1.get()};
  const std::vector<Edge*> merged = EdgeMerger::merge(edges);

  ASSERT_EQ(merged.size(), 1u);
  EXPECT_EQ(merged[0], e0.get());
  EXPECT_TRUE(merged[0]->getCoordinate(0).equals2D({0.0, 0.0}));
  EXPECT_TRUE(merged[0]->getCoordinate(2).equals2D({10.0, 0.0}));

  OverlayLabel label;
  merged[0]->populateLabel(label);
  EXPECT_TRUE(label.isBoundaryBoth());
  EXPECT_TRUE(label.isBoundaryTouch());
  EXPECT_EQ(label.getLocation(0, +Position::LEFT, true), Location::EXTERIOR);
  EXPECT_EQ(label.getLocation(0, +Position::RIGHT, true), Location::INTERIOR);
  EXPECT_EQ(label.getLocation(1, +Position::LEFT, true), Location::INTERIOR);
  EXPECT_EQ(label.getLocation(1, +Position::RIGHT, true), Location::EXTERIOR);
}

TEST(EdgeMergerTest, CoincidentSameGeomAreaOppositeDirectionCollapses) {
  const EdgeSourceInfo shell_info(uint8_t{0}, 1, false);
  const EdgeSourceInfo hole_info(uint8_t{0}, 1, true);

  // Two shells in opposite directions collapse to a non-hole collapse edge.
  auto shell1 = MakeEdge({{0.0, 0.0}, {10.0, 0.0}}, shell_info);
  auto shell2 = MakeEdge({{10.0, 0.0}, {0.0, 0.0}}, shell_info);
  std::vector<Edge*> shell_edges = {shell1.get(), shell2.get()};
  const std::vector<Edge*> merged_shells = EdgeMerger::merge(shell_edges);
  ASSERT_EQ(merged_shells.size(), 1u);
  OverlayLabel shell_label;
  merged_shells[0]->populateLabel(shell_label);
  EXPECT_TRUE(shell_label.isCollapse(0));
  EXPECT_FALSE(shell_label.isHole(0));
  EXPECT_TRUE(shell_label.isNotPart(1));

  // A shell and a hole in opposite directions collapse with shell precedence.
  auto hole = MakeEdge({{0.0, 0.0}, {10.0, 0.0}}, hole_info);
  auto shell = MakeEdge({{10.0, 0.0}, {0.0, 0.0}}, shell_info);
  std::vector<Edge*> mixed_edges = {hole.get(), shell.get()};
  const std::vector<Edge*> merged_mixed = EdgeMerger::merge(mixed_edges);
  ASSERT_EQ(merged_mixed.size(), 1u);
  OverlayLabel mixed_label;
  merged_mixed[0]->populateLabel(mixed_label);
  EXPECT_TRUE(mixed_label.isCollapse(0));
  EXPECT_FALSE(mixed_label.isHole(0));

  // Two holes in opposite directions preserve isHole == true.
  auto hole1 = MakeEdge({{0.0, 0.0}, {10.0, 0.0}}, hole_info);
  auto hole2 = MakeEdge({{10.0, 0.0}, {0.0, 0.0}}, hole_info);
  std::vector<Edge*> hole_edges = {hole1.get(), hole2.get()};
  const std::vector<Edge*> merged_holes = EdgeMerger::merge(hole_edges);
  ASSERT_EQ(merged_holes.size(), 1u);
  OverlayLabel hole_label;
  merged_holes[0]->populateLabel(hole_label);
  EXPECT_TRUE(hole_label.isCollapse(0));
  EXPECT_TRUE(hole_label.isHole(0));
}

TEST(EdgeMergerTest, CoincidentSameGeomAreaSameDirectionStaysBoundary) {
  const EdgeSourceInfo shell_info(uint8_t{0}, 1, false);
  auto e0 = MakeEdge({{0.0, 0.0}, {10.0, 0.0}}, shell_info);
  auto e1 = MakeEdge({{0.0, 0.0}, {10.0, 0.0}}, shell_info);

  std::vector<Edge*> edges = {e0.get(), e1.get()};
  const std::vector<Edge*> merged = EdgeMerger::merge(edges);
  ASSERT_EQ(merged.size(), 1u);

  OverlayLabel label;
  merged[0]->populateLabel(label);
  EXPECT_TRUE(label.isBoundary(0));
  EXPECT_FALSE(label.isCollapse(0));
  EXPECT_EQ(label.getLocation(0, +Position::LEFT, true), Location::EXTERIOR);
  EXPECT_EQ(label.getLocation(0, +Position::RIGHT, true), Location::INTERIOR);
}

TEST(EdgeMergerTest, CoincidentLinearEdgesPreserveFirstDirection) {
  const EdgeSourceInfo line0(uint8_t{0});
  const EdgeSourceInfo line1(uint8_t{1});

  auto e0 = MakeEdge({{10.0, 0.0}, {0.0, 0.0}}, line0);
  auto e1 = MakeEdge({{0.0, 0.0}, {10.0, 0.0}}, line1);
  auto e2 = MakeEdge({{0.0, 0.0}, {10.0, 0.0}}, line0);

  std::vector<Edge*> edges = {e0.get(), e1.get(), e2.get()};
  const std::vector<Edge*> merged = EdgeMerger::merge(edges);

  ASSERT_EQ(merged.size(), 1u);
  EXPECT_EQ(merged[0], e0.get());
  EXPECT_TRUE(merged[0]->getCoordinate(0).equals2D({10.0, 0.0}));
  EXPECT_TRUE(merged[0]->getCoordinate(1).equals2D({0.0, 0.0}));

  OverlayLabel label;
  merged[0]->populateLabel(label);
  EXPECT_TRUE(label.isLine(0));
  EXPECT_TRUE(label.isLine(1));
}

TEST(EdgeMergerTest, CoincidentAreaAndLineEdgesCombineDimensions) {
  const EdgeSourceInfo area0(uint8_t{0}, 1, false);
  const EdgeSourceInfo line1(uint8_t{1});

  auto area_edge = MakeEdge({{0.0, 0.0}, {10.0, 0.0}}, area0);
  auto line_edge = MakeEdge({{10.0, 0.0}, {0.0, 0.0}}, line1);

  std::vector<Edge*> edges_area_first = {area_edge.get(), line_edge.get()};
  const std::vector<Edge*> merged1 = EdgeMerger::merge(edges_area_first);
  ASSERT_EQ(merged1.size(), 1u);

  OverlayLabel label1;
  merged1[0]->populateLabel(label1);
  EXPECT_TRUE(label1.isBoundary(0));
  EXPECT_TRUE(label1.isLine(1));
  EXPECT_EQ(label1.getLocation(0, +Position::RIGHT, true), Location::INTERIOR);

  // Also check merging an Area edge into a Line base edge.
  const EdgeSourceInfo line0(uint8_t{0});
  const EdgeSourceInfo area1(uint8_t{1}, 1, false);
  auto base_line = MakeEdge({{10.0, 0.0}, {0.0, 0.0}}, line0);
  auto added_area = MakeEdge({{0.0, 0.0}, {10.0, 0.0}}, area1);

  std::vector<Edge*> edges_line_first = {base_line.get(), added_area.get()};
  const std::vector<Edge*> merged2 = EdgeMerger::merge(edges_line_first);
  ASSERT_EQ(merged2.size(), 1u);

  OverlayLabel label2;
  merged2[0]->populateLabel(label2);
  EXPECT_TRUE(label2.isLine(0));
  EXPECT_TRUE(label2.isBoundary(1));
  EXPECT_EQ(label2.getLocation(1, +Position::LEFT, true), Location::INTERIOR);
  EXPECT_EQ(label2.getLocation(1, +Position::RIGHT, true), Location::EXTERIOR);
}

TEST(EdgeMergerTest, InterleavedCoincidentEdgesPreserveFirstOccurrenceOrder) {
  const EdgeSourceInfo info0(uint8_t{0}, 1, false);
  const EdgeSourceInfo info1(uint8_t{1}, 1, false);

  auto a1 = MakeEdge({{0.0, 0.0}, {10.0, 0.0}}, info0);
  auto b1 = MakeEdge({{20.0, 0.0}, {30.0, 0.0}}, info1);
  auto a2 = MakeEdge({{10.0, 0.0}, {0.0, 0.0}}, info1);
  auto a3 = MakeEdge({{0.0, 0.0}, {10.0, 0.0}}, info0);
  auto b2 = MakeEdge({{30.0, 0.0}, {20.0, 0.0}}, info1);

  std::vector<Edge*> edges = {a1.get(), b1.get(), a2.get(), a3.get(), b2.get()};
  const std::vector<Edge*> merged = EdgeMerger::merge(edges);

  ASSERT_EQ(merged.size(), 2u);
  EXPECT_EQ(merged[0], a1.get());
  EXPECT_EQ(merged[1], b1.get());

  OverlayLabel label_a;
  merged[0]->populateLabel(label_a);
  EXPECT_TRUE(label_a.isBoundaryBoth());
  EXPECT_TRUE(label_a.isBoundaryTouch());

  OverlayLabel label_b;
  merged[1]->populateLabel(label_b);
  EXPECT_TRUE(label_b.isNotPart(0));
  EXPECT_TRUE(label_b.isCollapse(1));
}

TEST(EdgeMergerTest, MismatchedSizeForSameEdgeKeyThrowsAssertionFailed) {
  const EdgeSourceInfo info0(uint8_t{0});
  const EdgeSourceInfo info1(uint8_t{1});

  // Both edges produce the same EdgeKey ((0,0) -> (10,0)), but differ in size.
  auto short_edge = MakeEdge({{0.0, 0.0}, {10.0, 0.0}}, info0);
  auto long_edge = MakeEdge({{0.0, 0.0}, {10.0, 0.0}, {20.0, 0.0}}, info1);

  std::vector<Edge*> edges = {short_edge.get(), long_edge.get()};
  EXPECT_THROW(EdgeMerger::merge(edges), AssertionFailedException);

  // Also verify reversed-direction mismatch with matching first segment.
  auto three_pt = MakeEdge({{0.0, 0.0}, {5.0, 5.0}, {10.0, 0.0}}, info0);
  auto two_pt_rev = MakeEdge({{5.0, 5.0}, {0.0, 0.0}}, info1);
  std::vector<Edge*> rev_edges = {three_pt.get(), two_pt_rev.get()};
  EXPECT_THROW(EdgeMerger::merge(rev_edges), AssertionFailedException);
}

}  // namespace
}  // namespace geos_google
