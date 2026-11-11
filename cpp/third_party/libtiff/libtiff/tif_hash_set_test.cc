// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Clean-room unit tests for libtiff/tif_hash_set.c (TIFFHashSetNew,
// TIFFHashSetDestroy, TIFFHashSetSize, TIFFHashSetInsert, TIFFHashSetLookup,
// and TIFFHashSetRemove).
//
// See also:
//   https://gitlab.com/libtiff/libtiff/-/blob/master/libtiff/tif_hash_set.c

#include "third_party/tiff/libtiff/tif_hash_set.h"

#include <cstddef>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"

namespace libtiff {
namespace {

using HashValue = std::invoke_result_t<TIFFHashSetHashFunc, const void*>;

struct HashSetDeleter {
  void operator()(TIFFHashSet* set) const noexcept { TIFFHashSetDestroy(set); }
};

using UniqueHashSet = std::unique_ptr<TIFFHashSet, HashSetDeleter>;

struct TestEntry {
  HashValue hash;
  int key;
  int value;
  std::vector<int>* freed_values = nullptr;
};

HashValue EntryHash(const void* elt) {
  return static_cast<const TestEntry*>(elt)->hash;
}

bool EntryEqual(const void* elt1, const void* elt2) {
  return static_cast<const TestEntry*>(elt1)->key ==
         static_cast<const TestEntry*>(elt2)->key;
}

void EntryFree(void* elt) {
  auto* entry = static_cast<TestEntry*>(elt);
  if (entry->freed_values != nullptr) {
    entry->freed_values->push_back(entry->value);
  }
  delete entry;
}

TestEntry* MakeEntry(HashValue hash, int key, int value,
                     std::vector<int>* freed_values = nullptr) {
  auto entry = std::make_unique<TestEntry>(TestEntry{
      .hash = hash, .key = key, .value = value, .freed_values = freed_values});
  return entry.release();
}

TEST(TifHashSetTest, DestroyNullIsNoOp) { TIFFHashSetDestroy(nullptr); }

TEST(TifHashSetTest, EmptySetLifecycleAndMissingOperations) {
  UniqueHashSet set(TIFFHashSetNew(EntryHash, EntryEqual, EntryFree));
  ASSERT_NE(set.get(), nullptr);
  EXPECT_EQ(TIFFHashSetSize(set.get()), 0);

  const TestEntry probe{.hash = 42, .key = 1, .value = 0};
  EXPECT_EQ(TIFFHashSetLookup(set.get(), &probe), nullptr);
  EXPECT_FALSE(TIFFHashSetRemove(set.get(), &probe));
  EXPECT_EQ(TIFFHashSetSize(set.get()), 0);
}

TEST(TifHashSetTest, DefaultPointerHashAndEqualFallback) {
  // Elements spaced by 53 array slots have byte offsets congruent modulo 53,
  // forcing hash collisions under the default pointer hash.
  std::vector<int> backing(53 * 5, 0);

  UniqueHashSet set(TIFFHashSetNew(nullptr, nullptr, nullptr));
  ASSERT_NE(set.get(), nullptr);
  EXPECT_EQ(TIFFHashSetSize(set.get()), 0);

  EXPECT_TRUE(TIFFHashSetInsert(set.get(), &backing[0]));
  EXPECT_TRUE(TIFFHashSetInsert(set.get(), &backing[53]));
  EXPECT_TRUE(TIFFHashSetInsert(set.get(), &backing[53 * 2]));
  EXPECT_TRUE(TIFFHashSetInsert(set.get(), &backing[53 * 3]));
  EXPECT_TRUE(TIFFHashSetInsert(set.get(), nullptr));
  EXPECT_EQ(TIFFHashSetSize(set.get()), 5);

  // Re-inserting an existing pointer replaces in place without changing size.
  EXPECT_TRUE(TIFFHashSetInsert(set.get(), &backing[53]));
  EXPECT_EQ(TIFFHashSetSize(set.get()), 5);

  EXPECT_EQ(TIFFHashSetLookup(set.get(), &backing[0]), &backing[0]);
  EXPECT_EQ(TIFFHashSetLookup(set.get(), &backing[53]), &backing[53]);
  EXPECT_EQ(TIFFHashSetLookup(set.get(), &backing[53 * 2]), &backing[53 * 2]);
  EXPECT_EQ(TIFFHashSetLookup(set.get(), &backing[53 * 3]), &backing[53 * 3]);
  // Uninserted pointer in the same bucket walks the full chain and misses.
  EXPECT_EQ(TIFFHashSetLookup(set.get(), &backing[53 * 4]), nullptr);
  EXPECT_FALSE(TIFFHashSetRemove(set.get(), &backing[53 * 4]));

  // Remove head, middle, tail, and null pointer.
  EXPECT_TRUE(TIFFHashSetRemove(set.get(), &backing[53 * 3]));
  EXPECT_TRUE(TIFFHashSetRemove(set.get(), &backing[53]));
  EXPECT_TRUE(TIFFHashSetRemove(set.get(), &backing[0]));
  EXPECT_TRUE(TIFFHashSetRemove(set.get(), nullptr));
  EXPECT_EQ(TIFFHashSetSize(set.get()), 1);

  EXPECT_EQ(TIFFHashSetLookup(set.get(), &backing[53 * 2]), &backing[53 * 2]);
  EXPECT_EQ(TIFFHashSetLookup(set.get(), &backing[53]), nullptr);
}

TEST(TifHashSetTest, PartialNullCallbackCombinations) {
  // Null hash function + custom equality: elements spaced by 53 array slots
  // hash to the same bucket modulo 53 so custom equality runs.
  std::vector<TestEntry> pool(53 * 2);
  pool[0] = {.hash = 0, .key = 777, .value = 10};
  pool[53] = {.hash = 0, .key = 777, .value = 20};

  UniqueHashSet custom_eq_only(TIFFHashSetNew(nullptr, EntryEqual, nullptr));
  ASSERT_NE(custom_eq_only.get(), nullptr);
  EXPECT_TRUE(TIFFHashSetInsert(custom_eq_only.get(), &pool[0]));
  EXPECT_EQ(TIFFHashSetLookup(custom_eq_only.get(), &pool[53]), &pool[0]);
  // Replace with distinct equal pointer when fnFreeEltFunc is nullptr.
  EXPECT_TRUE(TIFFHashSetInsert(custom_eq_only.get(), &pool[53]));
  EXPECT_EQ(TIFFHashSetSize(custom_eq_only.get()), 1);
  EXPECT_EQ(TIFFHashSetLookup(custom_eq_only.get(), &pool[0]), &pool[53]);
  EXPECT_TRUE(TIFFHashSetRemove(custom_eq_only.get(), &pool[0]));
  EXPECT_EQ(TIFFHashSetSize(custom_eq_only.get()), 0);

  // Custom hash + null equality: equal pointers match, distinct pointers with
  // equal keys collide without matching. Also exercises max HashValue.
  TestEntry e1{
      .hash = std::numeric_limits<HashValue>::max(), .key = 1, .value = 100};
  TestEntry e2{
      .hash = std::numeric_limits<HashValue>::max(), .key = 1, .value = 200};
  UniqueHashSet custom_hash_only(TIFFHashSetNew(EntryHash, nullptr, nullptr));
  ASSERT_NE(custom_hash_only.get(), nullptr);
  EXPECT_TRUE(TIFFHashSetInsert(custom_hash_only.get(), &e1));
  EXPECT_TRUE(TIFFHashSetInsert(custom_hash_only.get(), &e2));
  EXPECT_EQ(TIFFHashSetSize(custom_hash_only.get()), 2);
  EXPECT_EQ(TIFFHashSetLookup(custom_hash_only.get(), &e1), &e1);
  EXPECT_EQ(TIFFHashSetLookup(custom_hash_only.get(), &e2), &e2);
}

TEST(TifHashSetTest, CollisionChainLookupReplaceAndRemovePositions) {
  std::vector<int> freed_values;
  UniqueHashSet set(TIFFHashSetNew(EntryHash, EntryEqual, EntryFree));
  ASSERT_NE(set.get(), nullptr);

  // All entries use hash=100 so they share one bucket (head=4, ..., tail=1).
  EXPECT_TRUE(
      TIFFHashSetInsert(set.get(), MakeEntry(100, 1, 101, &freed_values)));
  EXPECT_TRUE(
      TIFFHashSetInsert(set.get(), MakeEntry(100, 2, 102, &freed_values)));
  EXPECT_TRUE(
      TIFFHashSetInsert(set.get(), MakeEntry(100, 3, 103, &freed_values)));
  EXPECT_TRUE(
      TIFFHashSetInsert(set.get(), MakeEntry(100, 4, 104, &freed_values)));
  EXPECT_EQ(TIFFHashSetSize(set.get()), 4);

  // Replace head (key=4), middle (key=2), and tail (key=1).
  EXPECT_TRUE(
      TIFFHashSetInsert(set.get(), MakeEntry(100, 4, 204, &freed_values)));
  EXPECT_TRUE(
      TIFFHashSetInsert(set.get(), MakeEntry(100, 2, 202, &freed_values)));
  EXPECT_TRUE(
      TIFFHashSetInsert(set.get(), MakeEntry(100, 1, 201, &freed_values)));
  EXPECT_EQ(TIFFHashSetSize(set.get()), 4);
  EXPECT_EQ(freed_values, (std::vector<int>{104, 102, 101}));

  // Lookup every chain position and a colliding miss.
  for (const auto& [key, expected_val] : std::vector<std::pair<int, int>>{
           {4, 204}, {3, 103}, {2, 202}, {1, 201}}) {
    SCOPED_TRACE(key);
    const TestEntry probe{.hash = 100, .key = key, .value = 0};
    auto* found = static_cast<TestEntry*>(TIFFHashSetLookup(set.get(), &probe));
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->value, expected_val);
  }
  const TestEntry miss_probe{.hash = 100, .key = 999, .value = 0};
  EXPECT_EQ(TIFFHashSetLookup(set.get(), &miss_probe), nullptr);
  EXPECT_FALSE(TIFFHashSetRemove(set.get(), &miss_probe));

  // Remove middle (key=3), tail (key=1), head (key=4), then remaining (key=2).
  const TestEntry rm3{.hash = 100, .key = 3, .value = 0};
  EXPECT_TRUE(TIFFHashSetRemove(set.get(), &rm3));
  EXPECT_EQ(TIFFHashSetSize(set.get()), 3);

  const TestEntry rm1{.hash = 100, .key = 1, .value = 0};
  EXPECT_TRUE(TIFFHashSetRemove(set.get(), &rm1));
  EXPECT_EQ(TIFFHashSetSize(set.get()), 2);

  const TestEntry rm4{.hash = 100, .key = 4, .value = 0};
  EXPECT_TRUE(TIFFHashSetRemove(set.get(), &rm4));
  EXPECT_EQ(TIFFHashSetSize(set.get()), 1);

  const TestEntry rm2{.hash = 100, .key = 2, .value = 0};
  auto* found_rm2 = static_cast<TestEntry*>(TIFFHashSetLookup(set.get(), &rm2));
  ASSERT_NE(found_rm2, nullptr);
  EXPECT_EQ(found_rm2->value, 202);
  EXPECT_TRUE(TIFFHashSetRemove(set.get(), &rm2));
  EXPECT_EQ(TIFFHashSetSize(set.get()), 0);
  EXPECT_EQ(TIFFHashSetLookup(set.get(), &rm2), nullptr);
  EXPECT_FALSE(TIFFHashSetRemove(set.get(), &rm2));

  EXPECT_EQ(freed_values,
            (std::vector<int>{104, 102, 101, 103, 201, 204, 202}));
}

TEST(TifHashSetTest, GrowthShrinkRehashAndRecyclingList) {
  std::vector<int> freed_values;
  UniqueHashSet set(TIFFHashSetNew(EntryHash, EntryEqual, EntryFree));
  ASSERT_NE(set.get(), nullptr);

  // Insert 400 items with deliberate hash clustering (key % 32) so multiple
  // rehashes (53 -> 97 -> 193 -> 389 -> 769) redistribute multi-node chains.
  constexpr int kTotalItems = 400;
  for (int i = 0; i < kTotalItems; ++i) {
    ASSERT_TRUE(TIFFHashSetInsert(
        set.get(),
        MakeEntry(static_cast<HashValue>(i % 32), i, i + 1000, &freed_values)));
  }
  EXPECT_EQ(TIFFHashSetSize(set.get()), kTotalItems);

  for (int i = 0; i < kTotalItems; ++i) {
    SCOPED_TRACE(i);
    const TestEntry probe{
        .hash = static_cast<HashValue>(i % 32), .key = i, .value = 0};
    auto* found = static_cast<TestEntry*>(TIFFHashSetLookup(set.get(), &probe));
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->value, i + 1000);
  }

  // Remove all 400 items sequentially. This triggers multiple shrink rehashes
  // (769 -> 389 -> 193 -> 97 -> 53) and exceeds the 128-node recycling list
  // cap.
  for (int i = 0; i < kTotalItems; ++i) {
    SCOPED_TRACE(i);
    const TestEntry probe{
        .hash = static_cast<HashValue>(i % 32), .key = i, .value = 0};
    ASSERT_TRUE(TIFFHashSetRemove(set.get(), &probe));
    EXPECT_EQ(TIFFHashSetLookup(set.get(), &probe), nullptr);
    if (i == 17 || i == 210 || i == 305 || i == 353) {
      for (int j = i + 1; j < kTotalItems; ++j) {
        const TestEntry rem_probe{
            .hash = static_cast<HashValue>(j % 32), .key = j, .value = 0};
        auto* found =
            static_cast<TestEntry*>(TIFFHashSetLookup(set.get(), &rem_probe));
        ASSERT_NE(found, nullptr);
        EXPECT_EQ(found->value, j + 1000);
      }
    }
  }
  EXPECT_EQ(TIFFHashSetSize(set.get()), 0);
  EXPECT_EQ(freed_values.size(), static_cast<size_t>(kTotalItems));

  // Re-insert 150 items to drain all 128 recycled list nodes and grow again,
  // then remove 10 so psRecyclingList is non-empty when TIFFHashSetDestroy
  // runs.
  freed_values.clear();
  for (int i = 0; i < 150; ++i) {
    ASSERT_TRUE(TIFFHashSetInsert(
        set.get(),
        MakeEntry(static_cast<HashValue>(i), i, i + 2000, &freed_values)));
  }
  EXPECT_EQ(TIFFHashSetSize(set.get()), 150);

  for (int i = 0; i < 10; ++i) {
    const TestEntry probe{
        .hash = static_cast<HashValue>(i), .key = i, .value = 0};
    ASSERT_TRUE(TIFFHashSetRemove(set.get(), &probe));
  }
  EXPECT_EQ(TIFFHashSetSize(set.get()), 140);

  set.reset();
  EXPECT_EQ(freed_values.size(), 150U);
}

TEST(TifHashSetTest, DestroyNonEmptyWithoutFreeFunc) {
  std::vector<TestEntry> storage;
  storage.reserve(40);
  for (int i = 0; i < 40; ++i) {
    storage.push_back({.hash = static_cast<HashValue>(i % 7),
                       .key = i,
                       .value = i,
                       .freed_values = nullptr});
  }

  UniqueHashSet set(TIFFHashSetNew(EntryHash, EntryEqual, nullptr));
  ASSERT_NE(set.get(), nullptr);
  for (auto& entry : storage) {
    ASSERT_TRUE(TIFFHashSetInsert(set.get(), &entry));
  }
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(TIFFHashSetRemove(set.get(), &storage[i]));
  }
  EXPECT_EQ(TIFFHashSetSize(set.get()), 35);
  // UniqueHashSet destruction clears remaining buckets and recycled nodes
  // without invoking any free callback.
}

}  // namespace
}  // namespace libtiff
