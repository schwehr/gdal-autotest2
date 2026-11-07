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
// Tests atomic group and atom ID management (hdf/src/atom.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/atom.c

#include <cstdint>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

// GoogleTest's gtest.h defines SUCCEED() and FAIL() as function-like macros.
// Undefine them before including HDF4 headers so hdf.h can define its constants
// SUCCEED (0) and FAIL (-1) without macro redefinition errors under no_modules.
#ifdef SUCCEED
#undef SUCCEED
#endif
#ifdef FAIL
#undef FAIL
#endif

#include "third_party/hdf4/hdf/src/atom_priv.h"
#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/herr.h"

namespace hdf4 {
namespace {

class AtomTest : public ::testing::Test {
 protected:
  void TearDown() override {
    // HAshutdown cleans up process-global atom_group_list and atom_free_list.
    HAshutdown();
  }
};

TEST_F(AtomTest, HAinitGroupValidatesInputs) {
  // Invalid groups.
  EXPECT_EQ(FAIL, HAinit_group(BADGROUP, 8));
  EXPECT_EQ(FAIL, HAinit_group(MAXGROUP, 8));
  EXPECT_EQ(FAIL, HAinit_group(static_cast<group_t>(-2), 8));
  EXPECT_EQ(FAIL, HAinit_group(static_cast<group_t>(100), 8));

  // Zero hash size.
  EXPECT_EQ(FAIL, HAinit_group(FIDGROUP, 0));

  // Non-power-of-2 hash sizes.
  EXPECT_EQ(FAIL, HAinit_group(FIDGROUP, 3));
  EXPECT_EQ(FAIL, HAinit_group(FIDGROUP, 6));
  EXPECT_EQ(FAIL, HAinit_group(FIDGROUP, 7));
  EXPECT_EQ(FAIL, HAinit_group(FIDGROUP, 10));
}

TEST_F(AtomTest, HAinitGroupAndDestroyGroupReferenceCounting) {
  constexpr group_t kGroup = FIDGROUP;

  // First initialization.
  ASSERT_EQ(SUCCEED, HAinit_group(kGroup, 16));

  // Second initialization increments count.
  EXPECT_EQ(SUCCEED, HAinit_group(kGroup, 16));

  // First destroy decrements count without destroying the group.
  EXPECT_EQ(SUCCEED, HAdestroy_group(kGroup));

  // Group is still valid and can register atoms.
  int dummy = 42;
  atom_t atm = HAregister_atom(kGroup, &dummy);
  ASSERT_NE(FAIL, atm);
  EXPECT_EQ(&dummy, HAatom_object(atm));
  EXPECT_EQ(&dummy, HAremove_atom(atm));

  // Second destroy completely cleans up the group.
  EXPECT_EQ(SUCCEED, HAdestroy_group(kGroup));

  // Subsequent destroy on uninitialized group fails.
  EXPECT_EQ(FAIL, HAdestroy_group(kGroup));

  // Destroy on invalid groups fails.
  EXPECT_EQ(FAIL, HAdestroy_group(BADGROUP));
  EXPECT_EQ(FAIL, HAdestroy_group(MAXGROUP));
}

TEST_F(AtomTest, HAregisterAtomValidatesGroup) {
  int dummy = 1;
  // Invalid group.
  EXPECT_EQ(FAIL, HAregister_atom(BADGROUP, &dummy));
  EXPECT_EQ(FAIL, HAregister_atom(MAXGROUP, &dummy));

  // Valid group, but uninitialized.
  EXPECT_EQ(FAIL, HAregister_atom(AIDGROUP, &dummy));
}

TEST_F(AtomTest, HAatomGroupRetrievesGroupFromAtom) {
  constexpr group_t kGroup = VGIDGROUP;
  ASSERT_EQ(SUCCEED, HAinit_group(kGroup, 8));
  absl::Cleanup clean_group = [] {
    EXPECT_EQ(SUCCEED, HAdestroy_group(kGroup));
  };

  int dummy = 100;
  atom_t atm = HAregister_atom(kGroup, &dummy);
  ASSERT_NE(FAIL, atm);
  absl::Cleanup clean_atom = [atm] { EXPECT_NE(nullptr, HAremove_atom(atm)); };

  EXPECT_EQ(kGroup, HAatom_group(atm));

  // Invalid atom value with out-of-range group.
  EXPECT_EQ(BADGROUP, HAatom_group(static_cast<atom_t>(-1)));
}

TEST_F(AtomTest, HAatomObjectExercisesAllCachePositionsAndMisses) {
  constexpr group_t kGroup = VSIDGROUP;
  ASSERT_EQ(SUCCEED, HAinit_group(kGroup, 8));
  absl::Cleanup clean_group = [] {
    EXPECT_EQ(SUCCEED, HAdestroy_group(kGroup));
  };

  int val0 = 0, val1 = 1, val2 = 2, val3 = 3, val4 = 4;
  atom_t a0 = HAregister_atom(kGroup, &val0);
  atom_t a1 = HAregister_atom(kGroup, &val1);
  atom_t a2 = HAregister_atom(kGroup, &val2);
  atom_t a3 = HAregister_atom(kGroup, &val3);
  atom_t a4 = HAregister_atom(kGroup, &val4);
  ASSERT_NE(FAIL, a0);
  ASSERT_NE(FAIL, a1);
  ASSERT_NE(FAIL, a2);
  ASSERT_NE(FAIL, a3);
  ASSERT_NE(FAIL, a4);

  absl::Cleanup clean_atoms = [a0, a1, a2, a3, a4] {
    HAremove_atom(a0);
    HAremove_atom(a1);
    HAremove_atom(a2);
    HAremove_atom(a3);
    HAremove_atom(a4);
  };

  // 1. Initial lookups miss the cache, call HAIatom_object, and populate
  // cache[3].
  EXPECT_EQ(&val0, HAatom_object(a0));
  EXPECT_EQ(&val1, HAatom_object(a1));
  EXPECT_EQ(&val2, HAatom_object(a2));
  EXPECT_EQ(&val3, HAatom_object(a3));

  // 2. Querying elements triggers cache hits across index 0, 1, 2, 3 and swaps:
  // Query a3 (should hit near index 3 and swap).
  EXPECT_EQ(&val3, HAatom_object(a3));
  // Query a2.
  EXPECT_EQ(&val2, HAatom_object(a2));
  // Query a1.
  EXPECT_EQ(&val1, HAatom_object(a1));
  // Query a0 (cache miss or shifted, pulls into cache).
  EXPECT_EQ(&val0, HAatom_object(a0));

  // Repeated query hits cache[0].
  EXPECT_EQ(&val0, HAatom_object(a0));

  // 3. Lookup on non-existent atom returns nullptr.
  atom_t invalid_atm = a4 + 999;
  EXPECT_EQ(nullptr, HAatom_object(invalid_atm));
}

TEST_F(AtomTest, HAremoveAtomHandlesListPositionsAndCacheClearing) {
  // Use hash_size 1 so all registered atoms collide into the same bucket
  // (hash_loc = 0).
  constexpr group_t kGroup = GRIDGROUP;
  ASSERT_EQ(SUCCEED, HAinit_group(kGroup, 1));
  absl::Cleanup clean_group = [] {
    EXPECT_EQ(SUCCEED, HAdestroy_group(kGroup));
  };

  int obj1 = 10, obj2 = 20, obj3 = 30;
  // HAregister_atom prepends to grp_ptr->atom_list[hash_loc].
  // Order in list: atm3 -> atm2 -> atm1.
  atom_t atm1 = HAregister_atom(kGroup, &obj1);
  atom_t atm2 = HAregister_atom(kGroup, &obj2);
  atom_t atm3 = HAregister_atom(kGroup, &obj3);
  ASSERT_NE(FAIL, atm1);
  ASSERT_NE(FAIL, atm2);
  ASSERT_NE(FAIL, atm3);

  // Prime the cache with atm2.
  EXPECT_EQ(&obj2, HAatom_object(atm2));

  // 1. Remove middle element (last_atm != nullptr): atm2.
  EXPECT_EQ(&obj2, HAremove_atom(atm2));
  // After removal, querying atm2 returns nullptr.
  EXPECT_EQ(nullptr, HAatom_object(atm2));

  // 2. Remove head element (last_atm == nullptr): atm3.
  EXPECT_EQ(&obj3, HAremove_atom(atm3));
  EXPECT_EQ(nullptr, HAatom_object(atm3));

  // 3. Remove tail element: atm1.
  EXPECT_EQ(&obj1, HAremove_atom(atm1));
  EXPECT_EQ(nullptr, HAatom_object(atm1));

  // 4. Removing already removed atom fails.
  EXPECT_EQ(nullptr, HAremove_atom(atm1));

  // 5. Invalid arguments to HAremove_atom.
  EXPECT_EQ(nullptr, HAremove_atom(static_cast<atom_t>(-1)));
}

TEST_F(AtomTest, FreeListReuseInGetAtomNode) {
  constexpr group_t kGroup = RIIDGROUP;
  ASSERT_EQ(SUCCEED, HAinit_group(kGroup, 4));
  absl::Cleanup clean_group = [] {
    EXPECT_EQ(SUCCEED, HAdestroy_group(kGroup));
  };

  int val = 99;
  atom_t a1 = HAregister_atom(kGroup, &val);
  ASSERT_NE(FAIL, a1);

  // Removing a1 pushes its node onto atom_free_list.
  EXPECT_EQ(&val, HAremove_atom(a1));

  // Registering another atom reuses the node from atom_free_list.
  atom_t a2 = HAregister_atom(kGroup, &val);
  ASSERT_NE(FAIL, a2);
  EXPECT_EQ(&val, HAatom_object(a2));
  EXPECT_EQ(&val, HAremove_atom(a2));
}

TEST_F(AtomTest, HAsearchAtomFindsMatchingAndNonMatchingObjects) {
  constexpr group_t kGroup = BITIDGROUP;
  ASSERT_EQ(SUCCEED, HAinit_group(kGroup, 4));
  absl::Cleanup clean_group = [] {
    EXPECT_EQ(SUCCEED, HAdestroy_group(kGroup));
  };

  struct Item {
    int id;
    const char* name;
  };
  Item item1{.id = 10, .name = "first"};
  Item item2{.id = 20, .name = "second"};
  Item item3{.id = 30, .name = "third"};

  atom_t a1 = HAregister_atom(kGroup, &item1);
  atom_t a2 = HAregister_atom(kGroup, &item2);
  atom_t a3 = HAregister_atom(kGroup, &item3);
  ASSERT_NE(FAIL, a1);
  ASSERT_NE(FAIL, a2);
  ASSERT_NE(FAIL, a3);

  absl::Cleanup clean_atoms = [a1, a2, a3] {
    HAremove_atom(a1);
    HAremove_atom(a2);
    HAremove_atom(a3);
  };

  auto match_by_id = [](const void* obj, const void* key) -> int {
    const auto* it = static_cast<const Item*>(obj);
    const int target_id = *static_cast<const int*>(key);
    return it->id == target_id ? 1 : 0;
  };

  // Search for existing ID 20.
  int target_20 = 20;
  void* found = HAsearch_atom(kGroup, match_by_id, &target_20);
  EXPECT_EQ(&item2, found);

  // Search for non-existent ID 99.
  int target_99 = 99;
  EXPECT_EQ(nullptr, HAsearch_atom(kGroup, match_by_id, &target_99));

  // Search on invalid group.
  EXPECT_EQ(nullptr, HAsearch_atom(BADGROUP, match_by_id, &target_20));
  EXPECT_EQ(nullptr, HAsearch_atom(MAXGROUP, match_by_id, &target_20));
  // Search on uninitialized group.
  EXPECT_EQ(nullptr, HAsearch_atom(ANIDGROUP, match_by_id, &target_20));
}

TEST_F(AtomTest, HAshutdownCleansUpAllBuffers) {
  // Initialize and populate a group.
  ASSERT_EQ(SUCCEED, HAinit_group(ANIDGROUP, 4));
  int dummy = 123;
  atom_t atm = HAregister_atom(ANIDGROUP, &dummy);
  ASSERT_NE(FAIL, atm);

  // Remove atom to release node to atom_free_list.
  EXPECT_EQ(&dummy, HAremove_atom(atm));

  // Shutdown frees groups and free list.
  EXPECT_EQ(SUCCEED, HAshutdown());

  // Subsequent call on group fails because it was shut down.
  EXPECT_EQ(nullptr, HAatom_object(atm));

  // Idempotent shutdown.
  EXPECT_EQ(SUCCEED, HAshutdown());

  // Re-initialization after shutdown succeeds.
  ASSERT_EQ(SUCCEED, HAinit_group(ANIDGROUP, 4));
  atom_t atm2 = HAregister_atom(ANIDGROUP, &dummy);
  ASSERT_NE(FAIL, atm2);
  EXPECT_EQ(&dummy, HAatom_object(atm2));
  EXPECT_EQ(&dummy, HAremove_atom(atm2));
  EXPECT_EQ(SUCCEED, HAdestroy_group(ANIDGROUP));
}

}  // namespace
}  // namespace hdf4
