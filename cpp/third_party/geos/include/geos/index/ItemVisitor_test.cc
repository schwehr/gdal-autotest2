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

// Unit tests for third_party/geos/include/geos/index/ItemVisitor.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/index/ItemVisitor.h

#include "third_party/geos/include/geos/index/ItemVisitor.h"

#include <array>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/index/intervalrtree/SortedPackedIntervalRTree.h"
#include "third_party/geos/include/geos/index/quadtree/Quadtree.h"
#include "third_party/geos/include/geos/index/strtree/STRtree.h"
#include "third_party/geos/include/geos/index/strtree/SimpleSTRtree.h"

namespace geos_google {
namespace {

using geos::geom::Envelope;
using geos::index::ItemVisitor;
using geos::index::intervalrtree::SortedPackedIntervalRTree;
using geos::index::quadtree::Quadtree;
using geos::index::strtree::SimpleSTRtree;
using geos::index::strtree::STRtree;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::IsNull;
using ::testing::Not;
using ::testing::UnorderedElementsAre;

static_assert(std::is_abstract_v<ItemVisitor>);
static_assert(std::is_polymorphic_v<ItemVisitor>);
static_assert(std::has_virtual_destructor_v<ItemVisitor>);
static_assert(!std::is_default_constructible_v<ItemVisitor>);
static_assert(!std::is_copy_constructible_v<ItemVisitor>);
static_assert(std::is_same_v<decltype(std::declval<ItemVisitor&>().visitItem(
                                 std::declval<void*>())),
                             void>);

class RecordingVisitor : public ItemVisitor {
 public:
  explicit RecordingVisitor(bool* destroyed = nullptr)
      : destroyed_(destroyed) {}

  ~RecordingVisitor() override {
    if (destroyed_ != nullptr) {
      *destroyed_ = true;
    }
  }

  void visitItem(void* item) override { items_.push_back(item); }

  const std::vector<void*>& items() const { return items_; }

 private:
  std::vector<void*> items_;
  bool* destroyed_;
};

class DestructionFlag {
 public:
  explicit DestructionFlag(bool* destroyed) : destroyed_(destroyed) {}
  ~DestructionFlag() {
    if (destroyed_ != nullptr) {
      *destroyed_ = true;
    }
  }

 private:
  bool* destroyed_;
};

// Uses an implicitly declared virtual destructor to exercise base-class
// destructor chaining without an explicit ~Derived() override.
class CountingVisitor : public ItemVisitor {
 public:
  CountingVisitor(std::size_t* count, bool* destroyed)
      : count_(count), flag_(destroyed) {}

  void visitItem(void* item) override {
    if (item != nullptr) {
      ++(*count_);
    }
  }

 private:
  std::size_t* count_;
  DestructionFlag flag_;
};

TEST(ItemVisitorTest, DirectPolymorphicVisitAndVirtualDestruction) {
  std::array<int, 3> payloads = {10, 20, 30};

  bool stack_destroyed = false;
  {
    RecordingVisitor stack_visitor(&stack_destroyed);
    ItemVisitor& visitor_ref = stack_visitor;
    visitor_ref.visitItem(&payloads[0]);
    visitor_ref.visitItem(nullptr);
    visitor_ref.visitItem(&payloads[2]);
    EXPECT_THAT(stack_visitor.items(),
                ElementsAre(&payloads[0], IsNull(), &payloads[2]));
  }
  EXPECT_TRUE(stack_destroyed);

  bool recording_destroyed = false;
  bool counting_destroyed = false;
  std::size_t non_null_count = 0;

  std::vector<std::unique_ptr<ItemVisitor>> visitors;
  visitors.push_back(std::make_unique<RecordingVisitor>(&recording_destroyed));
  visitors.push_back(
      std::make_unique<CountingVisitor>(&non_null_count, &counting_destroyed));

  for (const auto& visitor : visitors) {
    visitor->visitItem(&payloads[0]);
    visitor->visitItem(nullptr);
    visitor->visitItem(&payloads[1]);
  }

  const auto* recorder =
      dynamic_cast<const RecordingVisitor*>(visitors[0].get());
  ASSERT_NE(recorder, nullptr);
  EXPECT_THAT(recorder->items(),
              ElementsAre(&payloads[0], IsNull(), &payloads[1]));
  EXPECT_EQ(non_null_count, 2);

  EXPECT_FALSE(recording_destroyed);
  EXPECT_FALSE(counting_destroyed);
  visitors.clear();
  EXPECT_TRUE(recording_destroyed);
  EXPECT_TRUE(counting_destroyed);
}

TEST(ItemVisitorTest, WorksWithQuadtreeQuery) {
  std::array<int, 4> tags = {1, 2, 3, 4};
  const std::array<Envelope, 4> envs = {
      Envelope(1.0, 3.0, 1.0, 3.0),
      Envelope(2.0, 4.0, 2.0, 4.0),
      Envelope(50.0, 52.0, 50.0, 52.0),
      Envelope(-20.0, -10.0, -20.0, -10.0),
  };

  Quadtree tree;
  for (std::size_t i = 0; i < envs.size(); ++i) {
    tree.insert(&envs[i], &tags[i]);
  }

  RecordingVisitor hit_visitor;
  const Envelope search_pos(1.5, 3.5, 1.5, 3.5);
  tree.query(&search_pos, hit_visitor);
  EXPECT_THAT(hit_visitor.items(), Contains(&tags[0]));
  EXPECT_THAT(hit_visitor.items(), Contains(&tags[1]));
  EXPECT_THAT(hit_visitor.items(), Not(Contains(&tags[3])));

  // Quadrant (+, -) has no inserted items, so primary filtering visits none.
  RecordingVisitor empty_visitor;
  const Envelope empty_quadrant(10.0, 20.0, -20.0, -10.0);
  tree.query(&empty_quadrant, empty_visitor);
  EXPECT_THAT(empty_visitor.items(), IsEmpty());
}

TEST(ItemVisitorTest, WorksWithSTRtreeAndSimpleSTRtree) {
  std::array<int, 4> tags = {100, 200, 300, 400};
  const std::array<Envelope, 4> envs = {
      Envelope(0.0, 2.0, 0.0, 2.0),
      Envelope(1.0, 4.0, 1.0, 4.0),
      Envelope(10.0, 12.0, 10.0, 12.0),
      Envelope(-8.0, -4.0, -8.0, -4.0),
  };

  STRtree str_tree(2);
  SimpleSTRtree simple_tree(2);
  for (std::size_t i = 0; i < envs.size(); ++i) {
    str_tree.insert(&envs[i], &tags[i]);
    simple_tree.insert(&envs[i], &tags[i]);
  }

  const Envelope search(1.5, 3.0, 1.5, 3.0);
  const Envelope disjoint(100.0, 110.0, 100.0, 110.0);

  RecordingVisitor str_hits;
  str_tree.query(&search, str_hits);
  EXPECT_THAT(str_hits.items(), UnorderedElementsAre(&tags[0], &tags[1]));

  RecordingVisitor str_miss;
  str_tree.query(&disjoint, str_miss);
  EXPECT_THAT(str_miss.items(), IsEmpty());

  RecordingVisitor simple_hits;
  simple_tree.query(&search, simple_hits);
  EXPECT_THAT(simple_hits.items(), UnorderedElementsAre(&tags[0], &tags[1]));

  RecordingVisitor simple_miss;
  simple_tree.query(&disjoint, simple_miss);
  EXPECT_THAT(simple_miss.items(), IsEmpty());

  RecordingVisitor all_visitor;
  simple_tree.iterate(all_visitor);
  EXPECT_THAT(all_visitor.items(),
              UnorderedElementsAre(&tags[0], &tags[1], &tags[2], &tags[3]));
}

TEST(ItemVisitorTest, WorksWithSortedPackedIntervalRTree) {
  SortedPackedIntervalRTree empty_tree;
  RecordingVisitor empty_visitor;
  empty_tree.query(0.0, 10.0, &empty_visitor);
  EXPECT_THAT(empty_visitor.items(), IsEmpty());

  std::array<int, 4> tags = {1, 2, 3, 4};
  SortedPackedIntervalRTree tree;
  tree.insert(0.0, 2.0, &tags[0]);
  tree.insert(3.0, 5.0, &tags[1]);
  tree.insert(4.0, 10.0, &tags[2]);
  tree.insert(20.0, 30.0, &tags[3]);

  RecordingVisitor overlap_visitor;
  tree.query(3.5, 4.5, &overlap_visitor);
  EXPECT_THAT(overlap_visitor.items(),
              UnorderedElementsAre(&tags[1], &tags[2]));

  RecordingVisitor point_visitor;
  tree.query(2.0, 2.0, &point_visitor);
  EXPECT_THAT(point_visitor.items(), ElementsAre(&tags[0]));

  RecordingVisitor gap_visitor;
  tree.query(12.0, 15.0, &gap_visitor);
  EXPECT_THAT(gap_visitor.items(), IsEmpty());
}

}  // namespace
}  // namespace geos_google
