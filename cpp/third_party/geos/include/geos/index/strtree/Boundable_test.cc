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

// Unit tests for third_party/geos/include/geos/index/strtree/Boundable.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/index/strtree/Boundable.h

#include "third_party/geos/include/geos/index/strtree/Boundable.h"

#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/index/strtree/AbstractNode.h"
#include "third_party/geos/include/geos/index/strtree/Interval.h"
#include "third_party/geos/include/geos/index/strtree/ItemBoundable.h"

namespace geos_google {
namespace {

using geos::geom::Envelope;
using geos::index::strtree::AbstractNode;
using geos::index::strtree::Boundable;
using geos::index::strtree::Interval;
using geos::index::strtree::ItemBoundable;

static_assert(std::is_abstract_v<Boundable>);
static_assert(std::is_polymorphic_v<Boundable>);
static_assert(std::has_virtual_destructor_v<Boundable>);

class StubBoundable final : public Boundable {
 public:
  StubBoundable(const void* bounds, bool is_leaf,
                int* destroyed_count = nullptr)
      : bounds_(bounds), is_leaf_(is_leaf), destroyed_count_(destroyed_count) {}

  StubBoundable(const StubBoundable&) = delete;
  StubBoundable& operator=(const StubBoundable&) = delete;

  ~StubBoundable() override {
    if (destroyed_count_ != nullptr) {
      ++(*destroyed_count_);
    }
  }

  const void* getBounds() const override { return bounds_; }

  bool isLeaf() const override { return is_leaf_; }

 private:
  const void* bounds_;
  bool is_leaf_;
  int* destroyed_count_;
};

class EnvelopeNode final : public AbstractNode {
 public:
  explicit EnvelopeNode(int level) : AbstractNode(level) {}

  EnvelopeNode(const EnvelopeNode&) = delete;
  EnvelopeNode& operator=(const EnvelopeNode&) = delete;

  ~EnvelopeNode() override { delete static_cast<Envelope*>(bounds); }

 protected:
  void* computeBounds() const override {
    const std::vector<Boundable*>* children = getChildBoundables();
    std::unique_ptr<Envelope> result;
    for (const Boundable* child : *children) {
      if (child == nullptr) {
        continue;
      }
      const auto* child_env = static_cast<const Envelope*>(child->getBounds());
      if (child_env == nullptr) {
        continue;
      }
      if (result == nullptr) {
        result = std::make_unique<Envelope>(*child_env);
      } else {
        result->expandToInclude(child_env);
      }
    }
    return result.release();
  }
};

TEST(BoundableTest, PolymorphicLeafAndNonLeafBehavior) {
  const Envelope env(1.0, 4.0, 2.0, 6.0);
  const Interval interval(-3.5, 7.5);

  const StubBoundable leaf(&env, true);
  const StubBoundable branch(&interval, false);

  const Boundable& leaf_ref = leaf;
  const Boundable& branch_ref = branch;

  EXPECT_TRUE(leaf_ref.isLeaf());
  EXPECT_EQ(leaf_ref.getBounds(), &env);
  EXPECT_TRUE(static_cast<const Envelope*>(leaf_ref.getBounds())->equals(&env));

  EXPECT_FALSE(branch_ref.isLeaf());
  EXPECT_EQ(branch_ref.getBounds(), &interval);
  EXPECT_TRUE(
      static_cast<const Interval*>(branch_ref.getBounds())->equals(&interval));
}

TEST(BoundableTest, VirtualDestructorRunsThroughBasePointer) {
  const Envelope env(0.0, 1.0, 0.0, 1.0);
  int destroyed_count = 0;

  std::unique_ptr<Boundable> boundable =
      std::make_unique<StubBoundable>(&env, true, &destroyed_count);
  EXPECT_EQ(destroyed_count, 0);
  EXPECT_TRUE(boundable->isLeaf());
  EXPECT_EQ(boundable->getBounds(), &env);

  boundable.reset();
  EXPECT_EQ(destroyed_count, 1);
}

TEST(BoundableTest, ReturnsNullBoundsVerbatim) {
  const StubBoundable null_leaf(nullptr, true);
  const StubBoundable null_branch(nullptr, false);

  const Boundable* items[] = {&null_leaf, &null_branch};
  EXPECT_TRUE(items[0]->isLeaf());
  EXPECT_EQ(items[0]->getBounds(), nullptr);
  EXPECT_FALSE(items[1]->isLeaf());
  EXPECT_EQ(items[1]->getBounds(), nullptr);
}

TEST(BoundableTest, HeterogeneousBoundablesWithGeosSubclasses) {
  const Envelope env1(0.0, 2.0, 0.0, 2.0);
  const Envelope env2(3.0, 5.0, 1.0, 4.0);
  int item1 = 10;
  int item2 = 20;

  auto leaf = std::make_unique<ItemBoundable>(&env1, &item1);
  ItemBoundable child_leaf(&env2, &item2);
  auto node = std::make_unique<EnvelopeNode>(0);
  node->addChildBoundable(&child_leaf);

  std::vector<std::unique_ptr<Boundable>> boundables;
  boundables.push_back(std::move(leaf));
  boundables.push_back(std::move(node));

  EXPECT_TRUE(boundables[0]->isLeaf());
  EXPECT_EQ(boundables[0]->getBounds(), &env1);

  EXPECT_FALSE(boundables[1]->isLeaf());
  const auto* node_env =
      static_cast<const Envelope*>(boundables[1]->getBounds());
  ASSERT_NE(node_env, nullptr);
  EXPECT_TRUE(node_env->equals(&env2));
}

}  // namespace
}  // namespace geos_google
