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

// Unit tests for third_party/geos/include/geos/index/SpatialIndex.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/index/SpatialIndex.h

#include "third_party/geos/include/geos/index/SpatialIndex.h"

#include <array>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/index/ItemVisitor.h"
#include "third_party/geos/include/geos/index/quadtree/Quadtree.h"
#include "third_party/geos/include/geos/index/strtree/STRtree.h"
#include "third_party/geos/include/geos/index/strtree/SimpleSTRtree.h"

namespace geos_google {
namespace {

using geos::geom::Envelope;
using geos::index::ItemVisitor;
using geos::index::SpatialIndex;
using geos::index::quadtree::Quadtree;
using geos::index::strtree::SimpleSTRtree;
using geos::index::strtree::STRtree;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::UnorderedElementsAreArray;

static_assert(std::is_abstract_v<SpatialIndex>);
static_assert(std::is_polymorphic_v<SpatialIndex>);
static_assert(std::has_virtual_destructor_v<SpatialIndex>);
static_assert(!std::is_default_constructible_v<SpatialIndex>);
static_assert(!std::is_copy_constructible_v<SpatialIndex>);
static_assert(std::is_base_of_v<SpatialIndex, Quadtree>);
static_assert(std::is_base_of_v<SpatialIndex, STRtree>);
static_assert(std::is_base_of_v<SpatialIndex, SimpleSTRtree>);
static_assert(
    std::is_same_v<decltype(std::declval<SpatialIndex&>().insert(
                       std::declval<const Envelope*>(), std::declval<void*>())),
                   void>);
static_assert(std::is_same_v<decltype(std::declval<SpatialIndex&>().query(
                                 std::declval<const Envelope*>(),
                                 std::declval<std::vector<void*>&>())),
                             void>);
static_assert(std::is_same_v<decltype(std::declval<SpatialIndex&>().query(
                                 std::declval<const Envelope*>(),
                                 std::declval<ItemVisitor&>())),
                             void>);
static_assert(
    std::is_same_v<decltype(std::declval<SpatialIndex&>().remove(
                       std::declval<const Envelope*>(), std::declval<void*>())),
                   bool>);

class RecordingVisitor : public ItemVisitor {
 public:
  void visitItem(void* item) override { items_.push_back(item); }

  const std::vector<void*>& items() const { return items_; }

  void clear() { items_.clear(); }

 private:
  std::vector<void*> items_;
};

class InMemorySpatialIndex : public SpatialIndex {
 public:
  explicit InMemorySpatialIndex(bool* destroyed = nullptr)
      : destroyed_(destroyed) {}

  ~InMemorySpatialIndex() override {
    if (destroyed_ != nullptr) {
      *destroyed_ = true;
    }
  }

  void insert(const Envelope* item_env, void* item) override {
    if (item_env == nullptr || item_env->isNull()) {
      return;
    }
    entries_.emplace_back(*item_env, item);
  }

  void query(const Envelope* search_env, std::vector<void*>& matches) override {
    if (search_env == nullptr || search_env->isNull()) {
      return;
    }
    for (const auto& [env, item] : entries_) {
      if (env.intersects(search_env)) {
        matches.push_back(item);
      }
    }
  }

  void query(const Envelope* search_env, ItemVisitor& visitor) override {
    if (search_env == nullptr || search_env->isNull()) {
      return;
    }
    for (const auto& [env, item] : entries_) {
      if (env.intersects(search_env)) {
        visitor.visitItem(item);
      }
    }
  }

  bool remove(const Envelope* item_env, void* item) override {
    if (item_env == nullptr || item_env->isNull()) {
      return false;
    }
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (it->second == item && it->first.intersects(item_env)) {
        entries_.erase(it);
        return true;
      }
    }
    return false;
  }

 private:
  std::vector<std::pair<Envelope, void*>> entries_;
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

// Relies on an implicitly declared virtual destructor to exercise base-class
// destructor chaining without an explicit ~Derived() override.
class ImplicitDtorSpatialIndex : public SpatialIndex {
 public:
  explicit ImplicitDtorSpatialIndex(bool* destroyed) : flag_(destroyed) {}

  void insert(const Envelope* item_env, void* item) override {
    if (item_env != nullptr && !item_env->isNull()) {
      items_.push_back(item);
    }
  }

  void query(const Envelope*, std::vector<void*>& matches) override {
    matches.insert(matches.end(), items_.begin(), items_.end());
  }

  void query(const Envelope*, ItemVisitor& visitor) override {
    for (void* item : items_) {
      visitor.visitItem(item);
    }
  }

  bool remove(const Envelope*, void* item) override {
    for (auto it = items_.begin(); it != items_.end(); ++it) {
      if (*it == item) {
        items_.erase(it);
        return true;
      }
    }
    return false;
  }

 private:
  std::vector<void*> items_;
  DestructionFlag flag_;
};

TEST(SpatialIndexTest, CustomImplementationPolymorphicLifecycleAndOps) {
  bool stack_destroyed = false;
  {
    ImplicitDtorSpatialIndex stack_index(&stack_destroyed);
    int item = 99;
    const Envelope unit(0.0, 1.0, 0.0, 1.0);
    stack_index.insert(&unit, &item);
    std::vector<void*> matches;
    stack_index.query(&unit, matches);
    EXPECT_THAT(matches, ElementsAre(&item));
  }
  EXPECT_TRUE(stack_destroyed);

  bool custom_destroyed = false;
  bool implicit_destroyed = false;
  std::vector<std::unique_ptr<SpatialIndex>> indexes;
  indexes.push_back(std::make_unique<InMemorySpatialIndex>(&custom_destroyed));
  indexes.push_back(
      std::make_unique<ImplicitDtorSpatialIndex>(&implicit_destroyed));

  std::array<int, 3> tags = {10, 20, 30};
  const Envelope null_env;
  const Envelope box_a(0.0, 2.0, 0.0, 2.0);
  const Envelope point_b(1.5, 1.5, 1.5, 1.5);
  const Envelope box_c(10.0, 12.0, 10.0, 12.0);
  const Envelope search(1.0, 3.0, 1.0, 3.0);

  SpatialIndex* custom = indexes[0].get();
  std::vector<void*> empty_vec;
  RecordingVisitor visitor;
  custom->query(&search, empty_vec);
  custom->query(&search, visitor);
  EXPECT_THAT(empty_vec, IsEmpty());
  EXPECT_THAT(visitor.items(), IsEmpty());
  EXPECT_FALSE(custom->remove(&box_a, &tags[0]));

  custom->insert(&null_env, &tags[0]);
  custom->insert(&box_a, &tags[0]);
  custom->insert(&point_b, &tags[1]);
  custom->insert(&box_c, &tags[2]);

  std::vector<void*> matches;
  custom->query(&search, matches);
  custom->query(&search, visitor);
  EXPECT_THAT(matches, ElementsAre(&tags[0], &tags[1]));
  EXPECT_THAT(visitor.items(), ElementsAre(&tags[0], &tags[1]));

  EXPECT_TRUE(custom->remove(&box_a, &tags[0]));
  EXPECT_FALSE(custom->remove(&box_a, &tags[0]));

  matches.clear();
  visitor.clear();
  custom->query(&search, matches);
  custom->query(&search, visitor);
  EXPECT_THAT(matches, ElementsAre(&tags[1]));
  EXPECT_THAT(visitor.items(), ElementsAre(&tags[1]));

  SpatialIndex* implicit = indexes[1].get();
  implicit->insert(&box_a, &tags[2]);
  std::vector<void*> implicit_vec;
  RecordingVisitor implicit_visitor;
  implicit->query(&box_a, implicit_vec);
  implicit->query(&box_a, implicit_visitor);
  EXPECT_THAT(implicit_vec, ElementsAre(&tags[2]));
  EXPECT_THAT(implicit_visitor.items(), ElementsAre(&tags[2]));
  EXPECT_TRUE(implicit->remove(&box_a, &tags[2]));
  EXPECT_FALSE(implicit->remove(&box_a, &tags[2]));

  EXPECT_FALSE(custom_destroyed);
  EXPECT_FALSE(implicit_destroyed);
  indexes.clear();
  EXPECT_TRUE(custom_destroyed);
  EXPECT_TRUE(implicit_destroyed);
}

TEST(SpatialIndexTest, EmptyGeosImplementationsThroughSpatialIndexInterface) {
  std::vector<std::unique_ptr<SpatialIndex>> empty_indexes;
  empty_indexes.push_back(std::make_unique<Quadtree>());
  empty_indexes.push_back(std::make_unique<STRtree>(2));
  empty_indexes.push_back(std::make_unique<SimpleSTRtree>(2));

  const Envelope search_hit(1.5, 3.5, 1.5, 3.5);
  for (std::size_t idx = 0; idx < empty_indexes.size(); ++idx) {
    SCOPED_TRACE(idx);
    SpatialIndex* index = empty_indexes[idx].get();

    std::vector<void*> initial_vec;
    RecordingVisitor initial_visitor;
    index->query(&search_hit, initial_vec);
    index->query(&search_hit, initial_visitor);
    EXPECT_THAT(initial_vec, IsEmpty());
    EXPECT_THAT(initial_visitor.items(), IsEmpty());
  }

  empty_indexes.clear();
}

TEST(SpatialIndexTest, GeosImplementationsThroughSpatialIndexInterface) {
  std::array<int, 4> tags = {1, 2, 3, 4};
  // Quadtree, STRtree, and SimpleSTRtree retain pointers to inserted
  // Envelopes, so envs must outlive indexes.
  const std::array<Envelope, 4> envs = {
      Envelope(1.0, 3.0, 1.0, 3.0),
      Envelope(2.0, 4.0, 2.0, 4.0),
      Envelope(100.0, 102.0, 100.0, 102.0),
      Envelope(-20.0, -10.0, -20.0, -10.0),
  };

  std::vector<std::unique_ptr<SpatialIndex>> indexes;
  indexes.push_back(std::make_unique<Quadtree>());
  indexes.push_back(std::make_unique<STRtree>(2));
  indexes.push_back(std::make_unique<SimpleSTRtree>(2));

  const Envelope search_hit(1.5, 3.5, 1.5, 3.5);
  const Envelope search_empty_quadrant(10.0, 20.0, -20.0, -10.0);

  for (std::size_t idx = 0; idx < indexes.size(); ++idx) {
    SCOPED_TRACE(idx);
    SpatialIndex* index = indexes[idx].get();

    // STRtree and SimpleSTRtree finalize their structure on first query or
    // remove, so all items must be inserted before querying.
    for (std::size_t i = 0; i < envs.size(); ++i) {
      index->insert(&envs[i], &tags[i]);
    }

    std::vector<void*> vec_hits;
    RecordingVisitor visitor_hits;
    index->query(&search_hit, vec_hits);
    index->query(&search_hit, visitor_hits);
    EXPECT_THAT(vec_hits, Contains(&tags[0]));
    EXPECT_THAT(vec_hits, Contains(&tags[1]));
    EXPECT_THAT(vec_hits, Not(Contains(&tags[3])));
    EXPECT_THAT(visitor_hits.items(), UnorderedElementsAreArray(vec_hits));

    std::vector<void*> empty_quad_vec;
    RecordingVisitor empty_quad_visitor;
    index->query(&search_empty_quadrant, empty_quad_vec);
    index->query(&search_empty_quadrant, empty_quad_visitor);
    EXPECT_THAT(empty_quad_vec, IsEmpty());
    EXPECT_THAT(empty_quad_visitor.items(), IsEmpty());

    EXPECT_TRUE(index->remove(&envs[0], &tags[0]));
    EXPECT_FALSE(index->remove(&envs[0], &tags[0]));

    std::vector<void*> post_remove_vec;
    RecordingVisitor post_remove_visitor;
    index->query(&search_hit, post_remove_vec);
    index->query(&search_hit, post_remove_visitor);
    EXPECT_THAT(post_remove_vec, Not(Contains(&tags[0])));
    EXPECT_THAT(post_remove_vec, Contains(&tags[1]));
    EXPECT_THAT(post_remove_visitor.items(),
                UnorderedElementsAreArray(post_remove_vec));
  }

  indexes.clear();
}

}  // namespace
}  // namespace geos_google
