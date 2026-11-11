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

// Unit tests for third_party/geos/include/geos/planargraph/GraphComponent.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/planargraph/GraphComponent.h

#include "third_party/geos/include/geos/planargraph/GraphComponent.h"

#include <array>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/planargraph/DirectedEdge.h"
#include "third_party/geos/include/geos/planargraph/Edge.h"
#include "third_party/geos/include/geos/planargraph/Node.h"
#include "third_party/geos/include/geos/planargraph/NodeMap.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::planargraph::DirectedEdge;
using geos::planargraph::Edge;
using geos::planargraph::GraphComponent;
using geos::planargraph::Node;
using geos::planargraph::NodeMap;

static_assert(std::has_virtual_destructor_v<GraphComponent>);
static_assert(std::is_default_constructible_v<GraphComponent>);
static_assert(std::is_base_of_v<GraphComponent, Node>);
static_assert(std::is_base_of_v<GraphComponent, Edge>);
static_assert(std::is_base_of_v<GraphComponent, DirectedEdge>);

class TrackingGraphComponent : public GraphComponent {
 public:
  explicit TrackingGraphComponent(bool* destroyed) : destroyed_(destroyed) {}

  ~TrackingGraphComponent() override {
    if (destroyed_ != nullptr) {
      *destroyed_ = true;
    }
  }

  bool markedVar() const { return isMarkedVar; }
  bool visitedVar() const { return isVisitedVar; }

  void setMarkedVarDirect(bool value) { isMarkedVar = value; }
  void setVisitedVarDirect(bool value) { isVisitedVar = value; }

 private:
  bool* destroyed_;
};

class SpyGraphComponent : public GraphComponent {
 public:
  bool isVisited() const override {
    ++is_visited_calls;
    return GraphComponent::isVisited();
  }

  void setVisited(bool is_visited) override {
    ++set_visited_calls;
    GraphComponent::setVisited(is_visited);
  }

  bool isMarked() const override {
    ++is_marked_calls;
    return GraphComponent::isMarked();
  }

  void setMarked(bool is_marked) override {
    ++set_marked_calls;
    GraphComponent::setMarked(is_marked);
  }

  mutable int is_visited_calls = 0;
  int set_visited_calls = 0;
  mutable int is_marked_calls = 0;
  int set_marked_calls = 0;
};

TEST(GraphComponentTest, DefaultConstructsUnmarkedAndUnvisited) {
  GraphComponent comp;
  const GraphComponent& const_ref = comp;

  EXPECT_FALSE(const_ref.isVisited());
  EXPECT_FALSE(const_ref.isMarked());
}

TEST(GraphComponentTest, VisitedAndMarkedFlagsToggleIndependently) {
  GraphComponent comp;
  const GraphComponent& const_ref = comp;

  comp.setVisited(true);
  EXPECT_TRUE(const_ref.isVisited());
  EXPECT_FALSE(const_ref.isMarked());

  comp.setVisited(true);
  EXPECT_TRUE(const_ref.isVisited());

  comp.setMarked(true);
  EXPECT_TRUE(const_ref.isVisited());
  EXPECT_TRUE(const_ref.isMarked());

  comp.setVisited(false);
  EXPECT_FALSE(const_ref.isVisited());
  EXPECT_TRUE(const_ref.isMarked());

  comp.setMarked(false);
  EXPECT_FALSE(const_ref.isVisited());
  EXPECT_FALSE(const_ref.isMarked());
}

TEST(GraphComponentTest, SupportsHeapAllocationAndPolymorphicDestruction) {
  auto base = std::make_unique<GraphComponent>();
  base->setVisited(true);
  base->setMarked(true);
  EXPECT_TRUE(base->isVisited());
  EXPECT_TRUE(base->isMarked());
  base.reset();

  bool destroyed = false;
  auto derived = std::make_unique<TrackingGraphComponent>(&destroyed);
  EXPECT_FALSE(derived->visitedVar());
  EXPECT_FALSE(derived->markedVar());

  derived->setVisitedVarDirect(true);
  derived->setMarkedVarDirect(true);
  EXPECT_TRUE(derived->isVisited());
  EXPECT_TRUE(derived->isMarked());

  std::unique_ptr<GraphComponent> poly = std::move(derived);
  EXPECT_FALSE(destroyed);
  poly.reset();
  EXPECT_TRUE(destroyed);
}

TEST(GraphComponentTest, DispatchesVirtualMethodsThroughBasePointers) {
  SpyGraphComponent spy0;
  SpyGraphComponent spy1;
  GraphComponent* base0 = &spy0;
  GraphComponent* base1 = &spy1;

  base0->setVisited(true);
  base0->setMarked(true);
  EXPECT_TRUE(base0->isVisited());
  EXPECT_TRUE(base0->isMarked());
  EXPECT_EQ(spy0.set_visited_calls, 1);
  EXPECT_EQ(spy0.is_visited_calls, 1);
  EXPECT_EQ(spy0.set_marked_calls, 1);
  EXPECT_EQ(spy0.is_marked_calls, 1);

  std::array<GraphComponent*, 2> items = {base0, base1};
  GraphComponent::setVisited(items.begin(), items.end(), false);
  GraphComponent::setMarked(items.begin(), items.end(), true);
  EXPECT_EQ(spy0.set_visited_calls, 2);
  EXPECT_EQ(spy1.set_visited_calls, 1);
  EXPECT_EQ(spy0.set_marked_calls, 2);
  EXPECT_EQ(spy1.set_marked_calls, 1);
  EXPECT_FALSE(spy0.isVisited());
  EXPECT_FALSE(spy1.isVisited());
  EXPECT_TRUE(spy0.isMarked());
  EXPECT_TRUE(spy1.isMarked());
}

TEST(GraphComponentTest, SetVisitedUpdatesRangeOnly) {
  std::vector<GraphComponent*> empty;
  GraphComponent::setVisited(empty.begin(), empty.end(), true);
  GraphComponent::setVisited(empty.begin(), empty.end(), false);

  std::array<GraphComponent, 4> items;
  std::vector<GraphComponent*> ptrs = {&items[0], &items[1], &items[2],
                                       &items[3]};

  // Empty subrange inside a populated vector leaves elements untouched.
  GraphComponent::setVisited(ptrs.begin() + 1, ptrs.begin() + 1, true);
  for (const GraphComponent* item : ptrs) {
    EXPECT_FALSE(item->isVisited());
    EXPECT_FALSE(item->isMarked());
  }

  GraphComponent::setVisited(ptrs.begin(), ptrs.end(), true);
  for (const GraphComponent* item : ptrs) {
    EXPECT_TRUE(item->isVisited());
    EXPECT_FALSE(item->isMarked());
  }

  // Clear only the interior half-open slice [1, 3).
  GraphComponent::setVisited(ptrs.begin() + 1, ptrs.begin() + 3, false);
  EXPECT_TRUE(items[0].isVisited());
  EXPECT_FALSE(items[1].isVisited());
  EXPECT_FALSE(items[2].isVisited());
  EXPECT_TRUE(items[3].isVisited());

  std::array<std::unique_ptr<GraphComponent>, 2> owned = {
      std::make_unique<GraphComponent>(),
      std::make_unique<GraphComponent>(),
  };
  GraphComponent::setVisited(owned.begin(), owned.end(), true);
  EXPECT_TRUE(owned[0]->isVisited());
  EXPECT_TRUE(owned[1]->isVisited());
  GraphComponent::setVisited(owned.begin(), owned.end(), false);
  EXPECT_FALSE(owned[0]->isVisited());
  EXPECT_FALSE(owned[1]->isVisited());
}

TEST(GraphComponentTest, SetMarkedUpdatesRangeOnly) {
  std::vector<GraphComponent*> empty;
  GraphComponent::setMarked(empty.begin(), empty.end(), true);
  GraphComponent::setMarked(empty.begin(), empty.end(), false);

  std::array<GraphComponent, 4> items;
  items[1].setVisited(true);
  std::vector<GraphComponent*> ptrs = {&items[0], &items[1], &items[2],
                                       &items[3]};

  GraphComponent::setMarked(ptrs.begin() + 1, ptrs.begin() + 1, true);
  for (const GraphComponent* item : ptrs) {
    EXPECT_FALSE(item->isMarked());
  }

  GraphComponent::setMarked(ptrs.begin(), ptrs.end(), true);
  for (const GraphComponent* item : ptrs) {
    EXPECT_TRUE(item->isMarked());
  }
  EXPECT_TRUE(items[1].isVisited());

  GraphComponent::setMarked(ptrs.begin() + 1, ptrs.begin() + 3, false);
  EXPECT_TRUE(items[0].isMarked());
  EXPECT_FALSE(items[1].isMarked());
  EXPECT_FALSE(items[2].isMarked());
  EXPECT_TRUE(items[3].isMarked());
  EXPECT_TRUE(items[1].isVisited());

  std::array<std::unique_ptr<GraphComponent>, 2> owned = {
      std::make_unique<GraphComponent>(),
      std::make_unique<GraphComponent>(),
  };
  GraphComponent::setMarked(owned.begin(), owned.end(), true);
  EXPECT_TRUE(owned[0]->isMarked());
  EXPECT_TRUE(owned[1]->isMarked());
  GraphComponent::setMarked(owned.begin(), owned.end(), false);
  EXPECT_FALSE(owned[0]->isMarked());
  EXPECT_FALSE(owned[1]->isMarked());
}

TEST(GraphComponentTest, SetVisitedMapAndSetMarkedMapUpdateMapValues) {
  std::map<int, GraphComponent*> empty_map;
  GraphComponent::setVisitedMap(empty_map.begin(), empty_map.end(), true);
  GraphComponent::setMarkedMap(empty_map.begin(), empty_map.end(), true);

  std::array<GraphComponent, 4> items;
  std::map<int, GraphComponent*> ordered = {
      {1, &items[0]},
      {2, &items[1]},
      {3, &items[2]},
      {4, &items[3]},
  };

  GraphComponent::setVisitedMap(ordered.begin(), ordered.end(), true);
  for (const auto& [key, comp] : ordered) {
    EXPECT_TRUE(comp->isVisited()) << "key=" << key;
    EXPECT_FALSE(comp->isMarked()) << "key=" << key;
  }

  GraphComponent::setVisitedMap(ordered.find(2), ordered.find(4), false);
  EXPECT_TRUE(items[0].isVisited());
  EXPECT_FALSE(items[1].isVisited());
  EXPECT_FALSE(items[2].isVisited());
  EXPECT_TRUE(items[3].isVisited());

  GraphComponent::setMarkedMap(ordered.begin(), ordered.end(), true);
  for (const auto& [key, comp] : ordered) {
    EXPECT_TRUE(comp->isMarked()) << "key=" << key;
  }
  EXPECT_TRUE(items[0].isVisited());
  EXPECT_FALSE(items[1].isVisited());

  GraphComponent::setMarkedMap(ordered.find(2), ordered.find(4), false);
  EXPECT_TRUE(items[0].isMarked());
  EXPECT_FALSE(items[1].isMarked());
  EXPECT_FALSE(items[2].isMarked());
  EXPECT_TRUE(items[3].isMarked());

  std::unordered_map<std::string, std::unique_ptr<GraphComponent>> owned_map;
  owned_map.emplace("a", std::make_unique<GraphComponent>());
  owned_map.emplace("b", std::make_unique<GraphComponent>());

  GraphComponent::setVisitedMap(owned_map.begin(), owned_map.end(), true);
  GraphComponent::setMarkedMap(owned_map.begin(), owned_map.end(), true);
  EXPECT_TRUE(owned_map["a"]->isVisited());
  EXPECT_TRUE(owned_map["a"]->isMarked());
  EXPECT_TRUE(owned_map["b"]->isVisited());
  EXPECT_TRUE(owned_map["b"]->isMarked());

  GraphComponent::setVisitedMap(owned_map.begin(), owned_map.end(), false);
  GraphComponent::setMarkedMap(owned_map.begin(), owned_map.end(), false);
  EXPECT_FALSE(owned_map["a"]->isVisited());
  EXPECT_FALSE(owned_map["a"]->isMarked());
  EXPECT_FALSE(owned_map["b"]->isVisited());
  EXPECT_FALSE(owned_map["b"]->isMarked());
}

TEST(GraphComponentTest, IntegratesWithPlanarGraphNodesEdgesAndNodeMap) {
  Node n0(Coordinate(0.0, 0.0));
  Node n1(Coordinate(1.0, 0.0));
  Node n2(Coordinate(2.0, 1.0));
  DirectedEdge de0(&n0, &n1, Coordinate(1.0, 0.0), true);
  DirectedEdge de1(&n1, &n0, Coordinate(0.0, 0.0), false);
  Edge edge(&de0, &de1);

  NodeMap node_map;
  node_map.add(&n0);
  node_map.add(&n1);
  node_map.add(&n2);

  GraphComponent::setVisitedMap(node_map.begin(), node_map.end(), true);
  EXPECT_TRUE(n0.isVisited());
  EXPECT_TRUE(n1.isVisited());
  EXPECT_TRUE(n2.isVisited());
  EXPECT_FALSE(n0.isMarked());

  GraphComponent::setMarkedMap(node_map.begin(), node_map.end(), true);
  EXPECT_TRUE(n0.isMarked());
  EXPECT_TRUE(n1.isMarked());
  EXPECT_TRUE(n2.isMarked());

  GraphComponent::setVisitedMap(node_map.begin(), node_map.end(), false);
  GraphComponent::setMarkedMap(node_map.begin(), node_map.end(), false);
  EXPECT_FALSE(n0.isVisited());
  EXPECT_FALSE(n0.isMarked());

  std::vector<Edge*> edges = {&edge};
  std::vector<DirectedEdge*> dir_edges = {&de0, &de1};
  GraphComponent::setVisited(edges.begin(), edges.end(), true);
  GraphComponent::setMarked(dir_edges.begin(), dir_edges.end(), true);
  EXPECT_TRUE(edge.isVisited());
  EXPECT_FALSE(edge.isMarked());
  EXPECT_TRUE(de0.isMarked());
  EXPECT_TRUE(de1.isMarked());
  EXPECT_FALSE(de0.isVisited());
  EXPECT_FALSE(de1.isVisited());
}

}  // namespace
}  // namespace geos_google
