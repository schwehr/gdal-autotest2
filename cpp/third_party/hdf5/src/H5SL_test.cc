// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests internal deterministic 1-2-3 skip list container operations across all
// supported key types, range queries, level promotions/demotions, iteration,
// and package lifecycle (third_party/hdf5/src/H5SL.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5SL.c

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

extern "C" {
#include "third_party/hdf5/src/H5SLprivate.h"
}  // extern "C"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

struct SkipListDeleter {
  void operator()(H5SL_t* slist) const {
    if (slist != nullptr) {
      H5SL_close(slist);
    }
  }
};

using ScopedSkipList = std::unique_ptr<H5SL_t, SkipListDeleter>;

ScopedSkipList MakeSkipList(H5SL_type_t type, H5SL_cmp_t cmp = nullptr) {
  return ScopedSkipList(H5SL_create(type, cmp));
}

template <typename KeyT>
const void* ToKeyPtr(const KeyT& key) {
  if constexpr (std::is_same_v<KeyT, std::string>) {
    return key.c_str();
  } else {
    return &key;
  }
}

struct CustomKey {
  int major;
  int minor;
};

int CompareCustomKey(const void* lhs, const void* rhs) {
  const auto* a = static_cast<const CustomKey*>(lhs);
  const auto* b = static_cast<const CustomKey*>(rhs);
  if (a->major != b->major) {
    return (a->major < b->major) ? -1 : 1;
  }
  if (a->minor != b->minor) {
    return (a->minor < b->minor) ? -1 : 1;
  }
  return 0;
}

class H5SLTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_GE(H5open(), 0); }
};

// Exercises insert, add, duplicate rejection, search/less/greater,
// find/below/above, forward/backward node traversal, and removal for any of
// the nine H5SL_type_t key types. `sorted_keys` must be strictly increasing in
// the skip list's ordering; `between_keys` must fall strictly between adjacent
// elements of `sorted_keys`.
template <typename KeyT>
void ExerciseKeyType(H5SL_type_t type, H5SL_cmp_t cmp,
                     std::span<const KeyT> sorted_keys, const KeyT& below_min,
                     std::span<const KeyT> between_keys,
                     const KeyT& above_max) {
  ASSERT_GE(sorted_keys.size(), 6u);
  ASSERT_EQ(between_keys.size(), sorted_keys.size() - 1);

  ScopedSkipList slist = MakeSkipList(type, cmp);
  ASSERT_NE(slist, nullptr);

  // Items are distinct indices in mutable storage so non-const void* item
  // pointers are never synthesized from const data.
  std::vector<size_t> items(sorted_keys.size());
  std::iota(items.begin(), items.end(), 0);

  // 1. Empty list queries all return nullptr and count is 0.
  EXPECT_EQ(H5SL_count(slist.get()), 0u);
  EXPECT_EQ(H5SL_search(slist.get(), ToKeyPtr(sorted_keys[0])), nullptr);
  EXPECT_EQ(H5SL_less(slist.get(), ToKeyPtr(sorted_keys[0])), nullptr);
  EXPECT_EQ(H5SL_greater(slist.get(), ToKeyPtr(sorted_keys[0])), nullptr);
  EXPECT_EQ(H5SL_find(slist.get(), ToKeyPtr(sorted_keys[0])), nullptr);
  EXPECT_EQ(H5SL_below(slist.get(), ToKeyPtr(sorted_keys[0])), nullptr);
  EXPECT_EQ(H5SL_above(slist.get(), ToKeyPtr(sorted_keys[0])), nullptr);
  EXPECT_EQ(H5SL_remove(slist.get(), ToKeyPtr(sorted_keys[0])), nullptr);
  EXPECT_EQ(H5SL_first(slist.get()), nullptr);
  EXPECT_EQ(H5SL_last(slist.get()), nullptr);

  // 2. Insert all keys except the last in interleaved order via H5SL_insert,
  // then insert the last via H5SL_add to exercise both insertion entry points.
  std::vector<size_t> insert_order;
  insert_order.reserve(sorted_keys.size() - 1);
  for (size_t lo = 0, hi = sorted_keys.size() - 2; lo <= hi;) {
    insert_order.push_back(hi);
    if (lo < hi) {
      insert_order.push_back(lo);
    }
    ++lo;
    if (hi == 0) {
      break;
    }
    --hi;
  }
  for (size_t idx : insert_order) {
    ASSERT_GE(H5SL_insert(slist.get(), &items[idx], ToKeyPtr(sorted_keys[idx])),
              0);
  }
  const size_t last_idx = sorted_keys.size() - 1;
  H5SL_node_t* added_node =
      H5SL_add(slist.get(), &items[last_idx], ToKeyPtr(sorted_keys[last_idx]));
  ASSERT_NE(added_node, nullptr);
  EXPECT_EQ(H5SL_item(added_node), &items[last_idx]);
  EXPECT_EQ(H5SL_count(slist.get()), sorted_keys.size());

  // 3. Duplicate key rejection via both H5SL_insert and H5SL_add.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5SL_insert(slist.get(), &items[0], ToKeyPtr(sorted_keys[0])), 0);
    EXPECT_EQ(H5SL_add(slist.get(), &items[last_idx],
                       ToKeyPtr(sorted_keys[last_idx])),
              nullptr);
    EXPECT_LT(H5SL_insert(slist.get(), &items[sorted_keys.size() / 2],
                          ToKeyPtr(sorted_keys[sorted_keys.size() / 2])),
              0);
  }
  EXPECT_EQ(H5SL_count(slist.get()), sorted_keys.size());

  // 4. Exact-match queries (search, less, greater, find, below, above).
  for (size_t i = 0; i < sorted_keys.size(); ++i) {
    const void* key_ptr = ToKeyPtr(sorted_keys[i]);
    EXPECT_EQ(H5SL_search(slist.get(), key_ptr), &items[i]);
    EXPECT_EQ(H5SL_less(slist.get(), key_ptr), &items[i]);
    EXPECT_EQ(H5SL_greater(slist.get(), key_ptr), &items[i]);

    H5SL_node_t* found = H5SL_find(slist.get(), key_ptr);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(H5SL_item(found), &items[i]);
    EXPECT_EQ(H5SL_below(slist.get(), key_ptr), found);
    EXPECT_EQ(H5SL_above(slist.get(), key_ptr), found);
  }

  // 5. Out-of-bounds queries (below_min, between_keys, above_max).
  EXPECT_EQ(H5SL_search(slist.get(), ToKeyPtr(below_min)), nullptr);
  EXPECT_EQ(H5SL_find(slist.get(), ToKeyPtr(below_min)), nullptr);
  EXPECT_EQ(H5SL_less(slist.get(), ToKeyPtr(below_min)), nullptr);
  EXPECT_EQ(H5SL_below(slist.get(), ToKeyPtr(below_min)), nullptr);
  EXPECT_EQ(H5SL_greater(slist.get(), ToKeyPtr(below_min)), &items[0]);
  EXPECT_EQ(H5SL_above(slist.get(), ToKeyPtr(below_min)),
            H5SL_first(slist.get()));

  for (size_t i = 0; i < between_keys.size(); ++i) {
    const void* key_ptr = ToKeyPtr(between_keys[i]);
    EXPECT_EQ(H5SL_search(slist.get(), key_ptr), nullptr);
    EXPECT_EQ(H5SL_find(slist.get(), key_ptr), nullptr);
    EXPECT_EQ(H5SL_less(slist.get(), key_ptr), &items[i]);
    EXPECT_EQ(H5SL_greater(slist.get(), key_ptr), &items[i + 1]);

    H5SL_node_t* below = H5SL_below(slist.get(), key_ptr);
    H5SL_node_t* above = H5SL_above(slist.get(), key_ptr);
    ASSERT_NE(below, nullptr);
    ASSERT_NE(above, nullptr);
    EXPECT_EQ(H5SL_item(below), &items[i]);
    EXPECT_EQ(H5SL_item(above), &items[i + 1]);
    EXPECT_EQ(H5SL_next(below), above);
    EXPECT_EQ(H5SL_prev(above), below);
  }

  EXPECT_EQ(H5SL_search(slist.get(), ToKeyPtr(above_max)), nullptr);
  EXPECT_EQ(H5SL_find(slist.get(), ToKeyPtr(above_max)), nullptr);
  EXPECT_EQ(H5SL_less(slist.get(), ToKeyPtr(above_max)), &items[last_idx]);
  EXPECT_EQ(H5SL_below(slist.get(), ToKeyPtr(above_max)),
            H5SL_last(slist.get()));
  EXPECT_EQ(H5SL_greater(slist.get(), ToKeyPtr(above_max)), nullptr);
  EXPECT_EQ(H5SL_above(slist.get(), ToKeyPtr(above_max)), nullptr);

  // 6. Forward and backward iteration via H5SL_first/next and H5SL_last/prev.
  std::vector<size_t> seen_forward;
  for (H5SL_node_t* n = H5SL_first(slist.get()); n != nullptr;
       n = H5SL_next(n)) {
    seen_forward.push_back(*static_cast<size_t*>(H5SL_item(n)));
  }
  EXPECT_EQ(seen_forward, items);

  std::vector<size_t> seen_backward;
  for (H5SL_node_t* n = H5SL_last(slist.get()); n != nullptr;
       n = H5SL_prev(n)) {
    seen_backward.push_back(*static_cast<size_t*>(H5SL_item(n)));
  }
  std::reverse(seen_backward.begin(), seen_backward.end());
  EXPECT_EQ(seen_backward, items);

  // 7. Missing-key removals return nullptr; existing removals remove in
  // middle/first/last order and leave an empty list.
  EXPECT_EQ(H5SL_remove(slist.get(), ToKeyPtr(below_min)), nullptr);
  EXPECT_EQ(H5SL_remove(slist.get(), ToKeyPtr(between_keys[0])), nullptr);
  EXPECT_EQ(H5SL_remove(slist.get(), ToKeyPtr(above_max)), nullptr);
  EXPECT_EQ(H5SL_count(slist.get()), sorted_keys.size());

  const size_t mid = sorted_keys.size() / 2;
  EXPECT_EQ(H5SL_remove(slist.get(), ToKeyPtr(sorted_keys[mid])), &items[mid]);
  EXPECT_EQ(H5SL_remove(slist.get(), ToKeyPtr(sorted_keys[mid])), nullptr);
  EXPECT_EQ(H5SL_remove(slist.get(), ToKeyPtr(sorted_keys[last_idx])),
            &items[last_idx]);
  EXPECT_EQ(H5SL_remove(slist.get(), ToKeyPtr(sorted_keys[0])), &items[0]);
  for (size_t i = 1; i < last_idx; ++i) {
    if (i == mid) {
      continue;
    }
    EXPECT_EQ(H5SL_remove(slist.get(), ToKeyPtr(sorted_keys[i])), &items[i]);
  }
  EXPECT_EQ(H5SL_count(slist.get()), 0u);
  EXPECT_EQ(H5SL_remove(slist.get(), ToKeyPtr(sorted_keys[0])), nullptr);
  EXPECT_EQ(H5SL_first(slist.get()), nullptr);
  EXPECT_EQ(H5SL_last(slist.get()), nullptr);
}

TEST_F(H5SLTest, IntKeyType) {
  constexpr auto kKeys = std::to_array<int>({-30, -10, 0, 10, 20, 30, 40, 50});
  constexpr auto kBetween = std::to_array<int>({-20, -5, 5, 15, 25, 35, 45});
  ExerciseKeyType<int>(H5SL_TYPE_INT, nullptr, kKeys, -40, kBetween, 60);
}

TEST_F(H5SLTest, HaddrKeyType) {
  constexpr auto kKeys =
      std::to_array<haddr_t>({10, 20, 30, 40, 50, 60, 70, 80});
  constexpr auto kBetween =
      std::to_array<haddr_t>({15, 25, 35, 45, 55, 65, 75});
  ExerciseKeyType<haddr_t>(H5SL_TYPE_HADDR, nullptr, kKeys, 5, kBetween, 90);
}

TEST_F(H5SLTest, HsizeKeyType) {
  constexpr auto kKeys =
      std::to_array<hsize_t>({100, 200, 300, 400, 500, 600, 700, 800});
  constexpr auto kBetween =
      std::to_array<hsize_t>({150, 250, 350, 450, 550, 650, 750});
  ExerciseKeyType<hsize_t>(H5SL_TYPE_HSIZE, nullptr, kKeys, 50, kBetween, 900);
}

TEST_F(H5SLTest, UnsignedKeyType) {
  constexpr auto kKeys =
      std::to_array<unsigned>({10u, 20u, 30u, 40u, 50u, 60u, 70u, 80u});
  constexpr auto kBetween =
      std::to_array<unsigned>({15u, 25u, 35u, 45u, 55u, 65u, 75u});
  ExerciseKeyType<unsigned>(H5SL_TYPE_UNSIGNED, nullptr, kKeys, 5u, kBetween,
                            90u);
}

TEST_F(H5SLTest, SizeKeyType) {
  constexpr auto kKeys =
      std::to_array<size_t>({10, 20, 30, 40, 50, 60, 70, 80});
  constexpr auto kBetween = std::to_array<size_t>({15, 25, 35, 45, 55, 65, 75});
  ExerciseKeyType<size_t>(H5SL_TYPE_SIZE, nullptr, kKeys, 5, kBetween, 90);
}

TEST_F(H5SLTest, HidKeyType) {
  constexpr auto kKeys = std::to_array<hid_t>({10, 20, 30, 40, 50, 60, 70, 80});
  constexpr auto kBetween = std::to_array<hid_t>({15, 25, 35, 45, 55, 65, 75});
  ExerciseKeyType<hid_t>(H5SL_TYPE_HID, nullptr, kKeys, 5, kBetween, 90);
}

TEST_F(H5SLTest, ObjKeyTypeComparesFilenoThenAddr) {
  // Include both distinct fileno values and identical fileno with distinct
  // addr values to cover both arms of H5SL_LOCATE_OBJ_CMP and OBJ_EQ.
  constexpr auto kKeys = std::to_array<H5_obj_t>({
      {.fileno = 1, .addr = 100},
      {.fileno = 1, .addr = 200},
      {.fileno = 2, .addr = 10},
      {.fileno = 2, .addr = 30},
      {.fileno = 2, .addr = 50},
      {.fileno = 3, .addr = 10},
      {.fileno = 4, .addr = 200},
  });
  constexpr auto kBetween = std::to_array<H5_obj_t>({
      {.fileno = 1, .addr = 150},
      {.fileno = 1, .addr = 300},
      {.fileno = 2, .addr = 20},
      {.fileno = 2, .addr = 40},
      {.fileno = 2, .addr = 100},
      {.fileno = 3, .addr = 999},
  });
  ExerciseKeyType<H5_obj_t>(H5SL_TYPE_OBJ, nullptr, kKeys,
                            H5_obj_t{.fileno = 1, .addr = 50}, kBetween,
                            H5_obj_t{.fileno = 5, .addr = 0});
}

TEST_F(H5SLTest, GenericKeyTypeUsesComparator) {
  constexpr auto kKeys = std::to_array<CustomKey>({
      {.major = 1, .minor = 10},
      {.major = 1, .minor = 20},
      {.major = 2, .minor = 5},
      {.major = 2, .minor = 15},
      {.major = 3, .minor = 10},
      {.major = 4, .minor = 20},
  });
  constexpr auto kBetween = std::to_array<CustomKey>({
      {.major = 1, .minor = 15},
      {.major = 1, .minor = 99},
      {.major = 2, .minor = 10},
      {.major = 2, .minor = 50},
      {.major = 3, .minor = 99},
  });
  ExerciseKeyType<CustomKey>(H5SL_TYPE_GENERIC, CompareCustomKey, kKeys,
                             CustomKey{.major = 0, .minor = 99}, kBetween,
                             CustomKey{.major = 9, .minor = 0});
}

TEST_F(H5SLTest, StringKeyTypeOrdersByHashThenStrcmpOnCollisions) {
  // djb2 (hash * 33 + c): changing char i by +1 and char i+1 by -33 preserves
  // the exact 32-bit hash. Thus "ad" ('a'=97, 'd'=100), "bC" ('b'=98, 'C'=67),
  // and "c\"" ('c'=99, '"'=34) share one hashval and order lexicographically
  // via strcmp when hashes match (likewise "aad", "abC", "ac\"").
  ASSERT_EQ(H5_hash_string("ad"), H5_hash_string("bC"));
  ASSERT_EQ(H5_hash_string("bC"), H5_hash_string("c\""));
  ASSERT_EQ(H5_hash_string("aad"), H5_hash_string("abC"));
  ASSERT_EQ(H5_hash_string("abC"), H5_hash_string("ac\""));

  const std::vector<std::string> keys = {
      "a", "m", "z", "ad", "c\"", "aad", "ac\"",
  };
  const std::vector<std::string> between = {
      "b", "t", "aa", "bC", "zz", "abC",
  };
  const std::string below_min = "A";
  const std::string above_max = "zzz";

  auto sl_less = [](const std::string& a, const std::string& b) {
    const uint32_t ha = H5_hash_string(a.c_str());
    const uint32_t hb = H5_hash_string(b.c_str());
    return ha != hb ? (ha < hb) : (a < b);
  };
  ASSERT_TRUE(sl_less(below_min, keys.front()));
  for (size_t i = 0; i < between.size(); ++i) {
    ASSERT_TRUE(sl_less(keys[i], between[i]));
    ASSERT_TRUE(sl_less(between[i], keys[i + 1]));
  }
  ASSERT_TRUE(sl_less(keys.back(), above_max));

  ExerciseKeyType<std::string>(H5SL_TYPE_STR, nullptr, keys, below_min, between,
                               above_max);
}

TEST_F(H5SLTest, LargeListGrowthAndRemovalPatterns) {
  // 1500 nodes exceed 2^10, forcing H5SL_GROW to promote nodes across >= 9
  // levels and grow the global factory array H5SL_fac_g from 1 -> 2 -> 4 -> 8.
  constexpr int kCount = 1500;
  std::vector<int> keys(kCount);
  std::vector<int> items(kCount);
  for (int i = 0; i < kCount; ++i) {
    keys[i] = i * 2;
    items[i] = i + 1000;
  }

  // 1. Ascending insert -> descending remove (tests tail growth & shrinking).
  {
    ScopedSkipList slist = MakeSkipList(H5SL_TYPE_INT);
    ASSERT_NE(slist, nullptr);
    for (int i = 0; i < kCount; ++i) {
      ASSERT_GE(H5SL_insert(slist.get(), &items[i], &keys[i]), 0);
    }
    EXPECT_EQ(H5SL_count(slist.get()), static_cast<size_t>(kCount));

    // Probe odd missing keys across the populated multi-level tree.
    for (int probe = -1; probe <= kCount * 2 + 1; probe += 98) {
      EXPECT_EQ(H5SL_search(slist.get(), &probe), nullptr);
    }

    for (int i = kCount - 1; i >= 0; --i) {
      EXPECT_EQ(H5SL_remove(slist.get(), &keys[i]), &items[i]);
    }
    EXPECT_EQ(H5SL_count(slist.get()), 0u);
    EXPECT_EQ(H5SL_remove_first(slist.get()), nullptr);
  }

  // 2. Descending insert -> H5SL_remove_first until empty (exercises front
  // gap demotion, promotion from next gap, and header shrinking).
  {
    ScopedSkipList slist = MakeSkipList(H5SL_TYPE_INT);
    ASSERT_NE(slist, nullptr);
    for (int i = kCount - 1; i >= 0; --i) {
      ASSERT_GE(H5SL_insert(slist.get(), &items[i], &keys[i]), 0);
    }
    for (int i = 0; i < kCount; ++i) {
      EXPECT_EQ(H5SL_remove_first(slist.get()), &items[i]);
    }
    EXPECT_EQ(H5SL_count(slist.get()), 0u);
    EXPECT_EQ(H5SL_remove_first(slist.get()), nullptr);
  }

  // 3. Deterministic pseudo-random insertion and deletion order (exercises
  // non-first-gap borrows, merges, level>0 swap-with-backward removals, and
  // missing-key probes at intermediate heights).
  {
    ScopedSkipList slist = MakeSkipList(H5SL_TYPE_INT);
    ASSERT_NE(slist, nullptr);

    std::vector<int> perm(kCount);
    std::iota(perm.begin(), perm.end(), 0);
    // Deterministic Fisher-Yates using LCG so runs are 100% reproducible.
    uint32_t rng = 0x12345678u;
    auto next_rand = [&rng]() -> uint32_t {
      rng = rng * 1664525u + 1013904223u;
      return rng;
    };
    for (int i = kCount - 1; i > 0; --i) {
      std::swap(perm[i], perm[next_rand() % static_cast<uint32_t>(i + 1)]);
    }
    for (int idx : perm) {
      ASSERT_GE(H5SL_insert(slist.get(), &items[idx], &keys[idx]), 0);
    }
    EXPECT_EQ(H5SL_count(slist.get()), static_cast<size_t>(kCount));

    // Verify full sorted order through H5SL_first/next before shuffled delete.
    int expected = 0;
    for (H5SL_node_t* n = H5SL_first(slist.get()); n != nullptr;
         n = H5SL_next(n)) {
      ASSERT_EQ(*static_cast<int*>(H5SL_item(n)), items[expected]);
      ++expected;
    }
    EXPECT_EQ(expected, kCount);

    // Shuffle again for deletion order and interleave missing-key removals.
    for (int i = kCount - 1; i > 0; --i) {
      std::swap(perm[i], perm[next_rand() % static_cast<uint32_t>(i + 1)]);
    }
    for (size_t step = 0; step < perm.size(); ++step) {
      if (step % 17 == 0) {
        int missing = keys[perm[step]] + 1;
        EXPECT_EQ(H5SL_remove(slist.get(), &missing), nullptr);
      }
      const int idx = perm[step];
      EXPECT_EQ(H5SL_remove(slist.get(), &keys[idx]), &items[idx]);
    }
    EXPECT_EQ(H5SL_count(slist.get()), 0u);
  }

  // 4. Ascending insert -> H5SL_remove_first until empty (exercises the
  // next-gap >= 2 promotion path in H5SL_remove_first when the first gap
  // becomes empty and borrows from the following 2/3-node gap).
  {
    ScopedSkipList slist = MakeSkipList(H5SL_TYPE_INT);
    ASSERT_NE(slist, nullptr);
    for (int i = 0; i < kCount; ++i) {
      ASSERT_GE(H5SL_insert(slist.get(), &items[i], &keys[i]), 0);
    }
    for (int i = 0; i < kCount; ++i) {
      EXPECT_EQ(H5SL_remove_first(slist.get()), &items[i]);
    }
    EXPECT_EQ(H5SL_count(slist.get()), 0u);
    EXPECT_EQ(H5SL_remove_first(slist.get()), nullptr);
  }
}

TEST_F(H5SLTest, SingleElementAndEmptyStringKeyBoundary) {
  // Single-element integer list boundary checks.
  {
    ScopedSkipList slist = MakeSkipList(H5SL_TYPE_INT);
    ASSERT_NE(slist, nullptr);
    int key = 10;
    int item = 99;
    ASSERT_GE(H5SL_insert(slist.get(), &item, &key), 0);
    EXPECT_EQ(H5SL_count(slist.get()), 1u);

    H5SL_node_t* first = H5SL_first(slist.get());
    H5SL_node_t* last = H5SL_last(slist.get());
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first, last);
    EXPECT_EQ(H5SL_next(first), nullptr);
    EXPECT_EQ(H5SL_prev(first), nullptr);

    int below = 5;
    int above = 15;
    EXPECT_EQ(H5SL_less(slist.get(), &below), nullptr);
    EXPECT_EQ(H5SL_less(slist.get(), &key), &item);
    EXPECT_EQ(H5SL_less(slist.get(), &above), &item);
    EXPECT_EQ(H5SL_greater(slist.get(), &below), &item);
    EXPECT_EQ(H5SL_greater(slist.get(), &key), &item);
    EXPECT_EQ(H5SL_greater(slist.get(), &above), nullptr);

    EXPECT_EQ(H5SL_remove_first(slist.get()), &item);
    EXPECT_EQ(H5SL_count(slist.get()), 0u);
    EXPECT_EQ(H5SL_first(slist.get()), nullptr);
    EXPECT_EQ(H5SL_last(slist.get()), nullptr);
  }

  // Empty C-string key in H5SL_TYPE_STR.
  {
    ScopedSkipList slist = MakeSkipList(H5SL_TYPE_STR);
    ASSERT_NE(slist, nullptr);
    const std::string empty_key;
    const std::string non_empty_key = "a";
    int item_empty = 1;
    int item_a = 2;
    ASSERT_GE(H5SL_insert(slist.get(), &item_a, non_empty_key.c_str()), 0);
    ASSERT_GE(H5SL_insert(slist.get(), &item_empty, empty_key.c_str()), 0);
    EXPECT_EQ(H5SL_search(slist.get(), empty_key.c_str()), &item_empty);
    H5SL_node_t* first = H5SL_first(slist.get());
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(H5SL_item(first), &item_empty);
    EXPECT_EQ(H5SL_remove(slist.get(), empty_key.c_str()), &item_empty);
    EXPECT_EQ(H5SL_search(slist.get(), empty_key.c_str()), nullptr);
    EXPECT_EQ(H5SL_search(slist.get(), non_empty_key.c_str()), &item_a);
  }
}

TEST_F(H5SLTest, IterateShortCircuitAndDeleteDuringIteration) {
  ScopedSkipList slist = MakeSkipList(H5SL_TYPE_INT);
  ASSERT_NE(slist, nullptr);

  constexpr int kCount = 24;
  std::array<int, kCount> keys{};
  std::array<int, kCount> items{};
  for (int i = 0; i < kCount; ++i) {
    keys[i] = i * 10;
    items[i] = i;
    ASSERT_GE(H5SL_insert(slist.get(), &items[i], &keys[i]), 0);
  }

  // 1. Full iteration (callback returns 0).
  struct IterState {
    std::vector<int> visited;
    int stop_after = -1;
    herr_t stop_code = 0;
    H5SL_t* delete_even_from = nullptr;
  };

  auto collect_cb = [](void* item, void* /*key*/, void* op_data) -> herr_t {
    auto* st = static_cast<IterState*>(op_data);
    st->visited.push_back(*static_cast<int*>(item));
    if (st->stop_after > 0 &&
        static_cast<int>(st->visited.size()) == st->stop_after) {
      return st->stop_code;
    }
    return 0;
  };

  IterState full_state;
  EXPECT_EQ(H5SL_iterate(slist.get(), collect_cb, &full_state), 0);
  EXPECT_EQ(full_state.visited.size(), static_cast<size_t>(kCount));
  EXPECT_EQ(full_state.visited.front(), 0);
  EXPECT_EQ(full_state.visited.back(), kCount - 1);

  // 2. Positive short-circuit.
  IterState pos_state{.stop_after = 5, .stop_code = 42};
  EXPECT_EQ(H5SL_iterate(slist.get(), collect_cb, &pos_state), 42);
  EXPECT_THAT(pos_state.visited, ElementsAre(0, 1, 2, 3, 4));

  // 3. Negative short-circuit.
  IterState neg_state{.stop_after = 3, .stop_code = -7};
  EXPECT_EQ(H5SL_iterate(slist.get(), collect_cb, &neg_state), -7);
  EXPECT_THAT(neg_state.visited, ElementsAre(0, 1, 2));

  // 4. Safe deletion of the current node during iteration.
  auto delete_even_cb = [](void* item, void* key, void* op_data) -> herr_t {
    auto* st = static_cast<IterState*>(op_data);
    const int val = *static_cast<int*>(item);
    st->visited.push_back(val);
    if (val % 2 == 0) {
      void* removed = H5SL_remove(st->delete_even_from, key);
      if (removed != item) {
        return -1;
      }
    }
    return 0;
  };
  IterState del_state{.delete_even_from = slist.get()};
  EXPECT_EQ(H5SL_iterate(slist.get(), delete_even_cb, &del_state), 0);
  EXPECT_EQ(del_state.visited.size(), static_cast<size_t>(kCount));
  EXPECT_EQ(H5SL_count(slist.get()), static_cast<size_t>(kCount / 2));

  // Confirm only odd entries remain in ascending order.
  IterState remaining;
  EXPECT_EQ(H5SL_iterate(slist.get(), collect_cb, &remaining), 0);
  EXPECT_THAT(remaining.visited,
              ElementsAre(1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23));

  // 5. Iteration over an empty list after release returns 0 with 0 visits.
  ASSERT_GE(H5SL_release(slist.get()), 0);
  IterState empty_state;
  EXPECT_EQ(H5SL_iterate(slist.get(), collect_cb, &empty_state), 0);
  EXPECT_THAT(empty_state.visited, IsEmpty());
}

TEST_F(H5SLTest, ReleaseFreeDestroyAndCloseLifecycle) {
  constexpr int kCount = 40;
  std::array<int, kCount> keys{};
  std::array<int, kCount> items{};
  for (int i = 0; i < kCount; ++i) {
    keys[i] = i;
    items[i] = i * 3;
  }

  // 1. H5SL_release removes all nodes (including shrinking an elevated header)
  // while keeping the skip list reusable.
  ScopedSkipList slist = MakeSkipList(H5SL_TYPE_INT);
  ASSERT_NE(slist, nullptr);
  for (int i = 0; i < kCount; ++i) {
    ASSERT_GE(H5SL_insert(slist.get(), &items[i], &keys[i]), 0);
  }
  ASSERT_GE(H5SL_release(slist.get()), 0);
  EXPECT_EQ(H5SL_count(slist.get()), 0u);
  EXPECT_EQ(H5SL_first(slist.get()), nullptr);
  EXPECT_EQ(H5SL_last(slist.get()), nullptr);

  // Re-insert into the released list and verify H5SL_free invokes callback
  // for every element (even if callback returns non-zero, H5SL_free ignores it
  // and frees all nodes).
  for (int i = 0; i < 10; ++i) {
    ASSERT_GE(H5SL_insert(slist.get(), &items[i], &keys[i]), 0);
  }
  struct FreeLog {
    std::vector<int> freed_items;
    herr_t return_code = 0;
  };
  auto free_cb = [](void* item, void* /*key*/, void* op_data) -> herr_t {
    auto* log = static_cast<FreeLog*>(op_data);
    log->freed_items.push_back(*static_cast<int*>(item));
    return log->return_code;
  };
  FreeLog free_log{.return_code = -1};
  ASSERT_GE(H5SL_free(slist.get(), free_cb, &free_log), 0);
  EXPECT_EQ(H5SL_count(slist.get()), 0u);
  EXPECT_THAT(free_log.freed_items,
              ElementsAre(0, 3, 6, 9, 12, 15, 18, 21, 24, 27));

  // 2. H5SL_free with nullptr callback acts like release.
  ASSERT_GE(H5SL_insert(slist.get(), &items[0], &keys[0]), 0);
  ASSERT_GE(H5SL_free(slist.get(), nullptr, nullptr), 0);
  EXPECT_EQ(H5SL_count(slist.get()), 0u);

  // 3. H5SL_destroy frees all remaining items via callback and frees slist.
  for (int i = 0; i < 5; ++i) {
    ASSERT_GE(H5SL_insert(slist.get(), &items[i], &keys[i]), 0);
  }
  FreeLog destroy_log;
  EXPECT_GE(H5SL_destroy(slist.release(), free_cb, &destroy_log), 0);
  EXPECT_THAT(destroy_log.freed_items, ElementsAre(0, 3, 6, 9, 12));

  // 4. H5SL_destroy with nullptr callback on empty list.
  ScopedSkipList empty_list = MakeSkipList(H5SL_TYPE_INT);
  ASSERT_NE(empty_list, nullptr);
  EXPECT_GE(H5SL_destroy(empty_list.release(), nullptr, nullptr), 0);

  // 5. H5SL_close on populated list frees all nodes without invoking callbacks.
  ScopedSkipList populated = MakeSkipList(H5SL_TYPE_INT);
  ASSERT_NE(populated, nullptr);
  for (int i = 0; i < kCount; ++i) {
    ASSERT_GE(H5SL_insert(populated.get(), &items[i], &keys[i]), 0);
  }
  EXPECT_GE(H5SL_close(populated.release()), 0);
}

TEST_F(H5SLTest, PackageShutdownAndReinitialization) {
  absl::Cleanup reopen = [] { H5open(); };

  // Build and close a large skip list so multiple H5SL_fac_g factories exist.
  {
    ScopedSkipList slist = MakeSkipList(H5SL_TYPE_INT);
    ASSERT_NE(slist, nullptr);
    constexpr int kCount = 300;
    std::array<int, kCount> keys{};
    for (int i = 0; i < kCount; ++i) {
      keys[i] = i;
      ASSERT_GE(H5SL_insert(slist.get(), &keys[i], &keys[i]), 0);
    }
  }

  // H5close() runs H5_term_library(), calling H5SL_term_package() until it
  // returns 0 and clears H5_PKG_INIT_VAR.
  ASSERT_GE(H5close(), 0);
  EXPECT_EQ(H5SL_term_package(), 0);

  // Re-open HDF5 and confirm skip list factories lazily re-initialize.
  ASSERT_GE(H5open(), 0);
  ScopedSkipList slist = MakeSkipList(H5SL_TYPE_INT);
  ASSERT_NE(slist, nullptr);
  int key = 77;
  int item = 88;
  ASSERT_GE(H5SL_insert(slist.get(), &item, &key), 0);
  EXPECT_EQ(H5SL_search(slist.get(), &key), &item);
}

}  // namespace
}  // namespace hdf5
