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

// Unit tests for third_party/geos/include/geos/index/kdtree/KdNodeVisitor.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/index/kdtree/KdNodeVisitor.h

#include "third_party/geos/include/geos/index/kdtree/KdNodeVisitor.h"

#include <cstddef>
#include <type_traits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/index/kdtree/KdNode.h"
#include "third_party/geos/include/geos/index/kdtree/KdTree.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::Envelope;
using geos::index::kdtree::KdNode;
using geos::index::kdtree::KdNodeVisitor;
using geos::index::kdtree::KdTree;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;

class RecordingVisitor final : public KdNodeVisitor {
 public:
  void visit(KdNode* node) override {
    if (node == nullptr) {
      ++null_count_;
      return;
    }
    visited_.push_back(node);
  }

  const std::vector<KdNode*>& visited() const { return visited_; }
  std::size_t null_count() const { return null_count_; }

 private:
  std::vector<KdNode*> visited_;
  std::size_t null_count_ = 0;
};

class IncrementingVisitor final : public KdNodeVisitor {
 public:
  void visit(KdNode* node) override {
    if (node != nullptr) {
      node->increment();
      ++visit_count_;
    }
  }

  std::size_t visit_count() const { return visit_count_; }

 private:
  std::size_t visit_count_ = 0;
};

TEST(KdNodeVisitorTest, InterfaceTypeTraits) {
  static_assert(std::is_abstract_v<KdNodeVisitor>);
  static_assert(std::is_polymorphic_v<KdNodeVisitor>);
  static_assert(!std::is_default_constructible_v<KdNodeVisitor>);
  static_assert(std::is_default_constructible_v<RecordingVisitor>);
  static_assert(std::is_invocable_r_v<void, decltype(&KdNodeVisitor::visit),
                                      KdNodeVisitor&, KdNode*>);

  EXPECT_TRUE(std::is_abstract_v<KdNodeVisitor>);
  EXPECT_TRUE(std::is_polymorphic_v<KdNodeVisitor>);
  EXPECT_FALSE(std::is_default_constructible_v<KdNodeVisitor>);
}

TEST(KdNodeVisitorTest, DirectDispatchThroughBaseReferenceAndPointer) {
  int payload1 = 11;
  int payload2 = 22;
  KdNode node1(1.5, -2.5, &payload1);
  KdNode node2(Coordinate(3.0, 4.0), &payload2);

  RecordingVisitor recorder;
  KdNodeVisitor& visitor_ref = recorder;
  KdNodeVisitor* visitor_ptr = &recorder;

  visitor_ref.visit(&node1);
  visitor_ptr->visit(&node2);
  visitor_ref.visit(nullptr);

  EXPECT_THAT(recorder.visited(), ElementsAre(&node1, &node2));
  EXPECT_EQ(recorder.null_count(), 1);
  EXPECT_EQ(recorder.visited()[0]->getData(), &payload1);
  EXPECT_DOUBLE_EQ(recorder.visited()[0]->getX(), 1.5);
  EXPECT_DOUBLE_EQ(recorder.visited()[0]->getY(), -2.5);
  EXPECT_EQ(recorder.visited()[1]->getData(), &payload2);
  EXPECT_DOUBLE_EQ(recorder.visited()[1]->getX(), 3.0);
  EXPECT_DOUBLE_EQ(recorder.visited()[1]->getY(), 4.0);
}

TEST(KdNodeVisitorTest, AllowsMutatingVisitedNodeThroughInterface) {
  KdNode node(10.0, 20.0, nullptr);
  EXPECT_EQ(node.getCount(), 1);
  EXPECT_FALSE(node.isRepeated());

  IncrementingVisitor incrementer;
  KdNodeVisitor& visitor = incrementer;
  visitor.visit(&node);
  visitor.visit(&node);

  EXPECT_EQ(incrementer.visit_count(), 2);
  EXPECT_EQ(node.getCount(), 3);
  EXPECT_TRUE(node.isRepeated());
}

TEST(KdNodeVisitorTest, DispatchesDuringKdTreeRangeQueries) {
  KdTree empty_tree;
  RecordingVisitor empty_visitor;
  empty_tree.query(Envelope(-100.0, 100.0, -100.0, 100.0), empty_visitor);
  EXPECT_THAT(empty_visitor.visited(), IsEmpty());

  KdTree tree;
  int id_origin = 1;
  int id_center = 2;
  int id_outside = 3;
  int id_left = 4;

  tree.insert(Coordinate(0.0, 0.0), &id_origin);
  tree.insert(Coordinate(5.0, 5.0), &id_center);
  tree.insert(Coordinate(5.0, 5.0), &id_center);
  tree.insert(Coordinate(10.0, 10.0), &id_outside);
  tree.insert(Coordinate(-3.0, 4.0), &id_left);

  RecordingVisitor range_visitor;
  tree.query(Envelope(-4.0, 6.0, 0.0, 6.0), range_visitor);

  std::vector<int> visited_ids;
  for (KdNode* node : range_visitor.visited()) {
    ASSERT_NE(node, nullptr);
    ASSERT_NE(node->getData(), nullptr);
    const int id = *static_cast<const int*>(node->getData());
    visited_ids.push_back(id);
    if (id == id_center) {
      EXPECT_EQ(node->getCount(), 2);
      EXPECT_TRUE(node->isRepeated());
    } else {
      EXPECT_EQ(node->getCount(), 1);
      EXPECT_FALSE(node->isRepeated());
    }
  }
  EXPECT_THAT(visited_ids, UnorderedElementsAre(id_origin, id_center, id_left));

  IncrementingVisitor mutating_visitor;
  tree.query(Envelope(4.0, 6.0, 4.0, 6.0), mutating_visitor);
  EXPECT_EQ(mutating_visitor.visit_count(), 1);
  KdNode* center_node = tree.query(Coordinate(5.0, 5.0));
  ASSERT_NE(center_node, nullptr);
  EXPECT_EQ(center_node->getCount(), 3);
}

TEST(KdNodeVisitorTest, VisitsSnappedNodeInToleranceKdTree) {
  KdTree snapped_tree(0.5);
  int id_first = 42;
  int id_second = 99;
  snapped_tree.insert(Coordinate(2.0, 2.0), &id_first);
  snapped_tree.insert(Coordinate(2.2, 2.2), &id_second);

  RecordingVisitor visitor;
  snapped_tree.query(Envelope(1.0, 3.0, 1.0, 3.0), visitor);
  ASSERT_EQ(visitor.visited().size(), 1);
  EXPECT_EQ(*static_cast<const int*>(visitor.visited()[0]->getData()),
            id_first);
  EXPECT_EQ(visitor.visited()[0]->getCount(), 2);
  EXPECT_TRUE(visitor.visited()[0]->isRepeated());
}

}  // namespace
}  // namespace geos_google
