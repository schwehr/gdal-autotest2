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

// Unit tests for third_party/geos/include/geos/index/strtree/ItemBoundable.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/index/strtree/ItemBoundable.h

#include "third_party/geos/include/geos/index/strtree/ItemBoundable.h"

#include <memory>
#include <string>
#include <type_traits>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/index/strtree/Boundable.h"
#include "third_party/geos/include/geos/index/strtree/Interval.h"

namespace geos_google {
namespace {

using geos::geom::Envelope;
using geos::index::strtree::Boundable;
using geos::index::strtree::Interval;
using geos::index::strtree::ItemBoundable;

static_assert(std::is_base_of_v<Boundable, ItemBoundable>);
static_assert(!std::is_abstract_v<ItemBoundable>);
static_assert(std::has_virtual_destructor_v<ItemBoundable>);

struct TrackedPayload {
  int id = 0;
  int* destroyed_count = nullptr;

  ~TrackedPayload() {
    if (destroyed_count != nullptr) {
      ++(*destroyed_count);
    }
  }
};

TEST(ItemBoundableTest, StoresEnvelopeBoundsAndItemPointer) {
  const Envelope env(1.0, 3.0, 2.0, 4.0);
  std::string payload = "parcel_42";

  const ItemBoundable item_boundable(&env, &payload);
  const Boundable& as_boundable = item_boundable;

  EXPECT_TRUE(item_boundable.isLeaf());
  EXPECT_TRUE(as_boundable.isLeaf());

  EXPECT_EQ(item_boundable.getBounds(), &env);
  EXPECT_EQ(as_boundable.getBounds(), &env);
  EXPECT_TRUE(
      static_cast<const Envelope*>(as_boundable.getBounds())->equals(&env));

  EXPECT_EQ(item_boundable.getItem(), &payload);
  EXPECT_EQ(*static_cast<std::string*>(item_boundable.getItem()), "parcel_42");
}

TEST(ItemBoundableTest, StoresIntervalBoundsAndAllowsItemMutation) {
  const Interval interval(-5.5, 12.25);
  int item_id = 99;

  const ItemBoundable item_boundable(&interval, &item_id);
  EXPECT_TRUE(item_boundable.isLeaf());
  EXPECT_EQ(item_boundable.getBounds(), &interval);
  EXPECT_TRUE(static_cast<const Interval*>(item_boundable.getBounds())
                  ->equals(&interval));

  auto* stored_item = static_cast<int*>(item_boundable.getItem());
  ASSERT_EQ(stored_item, &item_id);
  *stored_item = 100;
  EXPECT_EQ(item_id, 100);
}

TEST(ItemBoundableTest, HandlesNullBoundsAndNullItem) {
  const Envelope env(0.0, 1.0, 0.0, 1.0);
  int item = 7;

  const ItemBoundable both_null(nullptr, nullptr);
  EXPECT_TRUE(both_null.isLeaf());
  EXPECT_EQ(both_null.getBounds(), nullptr);
  EXPECT_EQ(both_null.getItem(), nullptr);

  const ItemBoundable null_bounds(nullptr, &item);
  EXPECT_TRUE(null_bounds.isLeaf());
  EXPECT_EQ(null_bounds.getBounds(), nullptr);
  EXPECT_EQ(null_bounds.getItem(), &item);

  const ItemBoundable null_item(&env, nullptr);
  EXPECT_TRUE(null_item.isLeaf());
  EXPECT_EQ(null_item.getBounds(), &env);
  EXPECT_EQ(null_item.getItem(), nullptr);
}

TEST(ItemBoundableTest, PreservesNullEnvelopeWithoutInspection) {
  const Envelope null_env;
  ASSERT_TRUE(null_env.isNull());
  int item = 11;

  const ItemBoundable item_boundable(&null_env, &item);
  EXPECT_TRUE(item_boundable.isLeaf());
  EXPECT_EQ(item_boundable.getBounds(), &null_env);
  EXPECT_TRUE(
      static_cast<const Envelope*>(item_boundable.getBounds())->isNull());
  EXPECT_EQ(item_boundable.getItem(), &item);
}

TEST(ItemBoundableTest, BoundsAndItemMayAliasSameObject) {
  Envelope env(0.0, 10.0, 0.0, 10.0);

  const ItemBoundable item_boundable(&env, &env);
  EXPECT_TRUE(item_boundable.isLeaf());
  EXPECT_EQ(item_boundable.getBounds(), &env);
  EXPECT_EQ(item_boundable.getItem(), &env);
}

TEST(ItemBoundableTest, DestructionDoesNotOwnBoundsOrItem) {
  int destroyed_count = 0;
  const Envelope env(1.0, 2.0, 3.0, 4.0);
  TrackedPayload payload{.id = 5, .destroyed_count = &destroyed_count};

  std::unique_ptr<Boundable> as_base =
      std::make_unique<ItemBoundable>(&env, &payload);
  EXPECT_TRUE(as_base->isLeaf());
  EXPECT_EQ(as_base->getBounds(), &env);
  as_base.reset();
  EXPECT_EQ(destroyed_count, 0);

  std::unique_ptr<ItemBoundable> as_derived =
      std::make_unique<ItemBoundable>(&env, &payload);
  EXPECT_EQ(as_derived->getItem(), &payload);
  as_derived.reset();
  EXPECT_EQ(destroyed_count, 0);
}

TEST(ItemBoundableTest, DistinctItemsCanShareSameBoundsPointer) {
  const Envelope shared_env(-1.0, 1.0, -1.0, 1.0);
  int first_item = 1;
  int second_item = 2;

  const ItemBoundable first(&shared_env, &first_item);
  const ItemBoundable second(&shared_env, &second_item);

  EXPECT_EQ(first.getBounds(), second.getBounds());
  EXPECT_NE(first.getItem(), second.getItem());
  EXPECT_EQ(*static_cast<const int*>(first.getItem()), 1);
  EXPECT_EQ(*static_cast<const int*>(second.getItem()), 2);
}

}  // namespace
}  // namespace geos_google
