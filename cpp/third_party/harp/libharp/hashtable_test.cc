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
// Unit tests for libharp's internal string-to-index hashtable
// (libharp/hashtable.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/hashtable.c
//   https://github.com/stcorp/harp/blob/master/libharp/hashtable.h

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"

extern "C" {
#include "third_party/stcorp_harp/libharp/hashtable.h"
}  // extern "C"

namespace stcorp_harp {
namespace {

struct HashtableDeleter {
  void operator()(hashtable* table) const { hashtable_delete(table); }
};
using ScopedHashtable = std::unique_ptr<hashtable, HashtableDeleter>;

TEST(HashtableTest, EmptyTableLookupsAndDelete) {
  hashtable_delete(nullptr);

  // An allocated but unpopulated table has count == NULL.
  ScopedHashtable empty_cs(hashtable_new(/*case_sensitive=*/1));
  ASSERT_NE(nullptr, empty_cs.get());
  EXPECT_EQ(-1L, hashtable_get_index_from_name(empty_cs.get(), "alpha"));
  EXPECT_EQ(-1L, hashtable_get_index_from_name_n(empty_cs.get(), "alpha", 5));

  ScopedHashtable empty_ci(hashtable_new(/*case_sensitive=*/0));
  ASSERT_NE(nullptr, empty_ci.get());
  EXPECT_EQ(-1L, hashtable_get_index_from_name(empty_ci.get(), "alpha"));
  EXPECT_EQ(-1L, hashtable_get_index_from_name_n(empty_ci.get(), "alpha", 5));
}

TEST(HashtableTest, CaseSensitiveAddLookupAndDuplicates) {
  ScopedHashtable table(hashtable_new(/*case_sensitive=*/1));
  ASSERT_NE(nullptr, table.get());

  EXPECT_EQ(0, hashtable_add_name(table.get(), "Alpha"));
  EXPECT_EQ(0, hashtable_add_name(table.get(), "alpha"));
  EXPECT_EQ(0, hashtable_add_name(table.get(), "beta"));
  // Duplicate exact key fails; different case succeeds.
  EXPECT_EQ(-1, hashtable_add_name(table.get(), "Alpha"));
  EXPECT_EQ(-1, hashtable_add_name(table.get(), "alpha"));

  EXPECT_EQ(0L, hashtable_get_index_from_name(table.get(), "Alpha"));
  EXPECT_EQ(1L, hashtable_get_index_from_name(table.get(), "alpha"));
  EXPECT_EQ(2L, hashtable_get_index_from_name(table.get(), "beta"));
  EXPECT_EQ(-1L, hashtable_get_index_from_name(table.get(), "ALPHA"));
  EXPECT_EQ(-1L, hashtable_get_index_from_name(table.get(), "gamma"));

  // Length-bounded lookup matches prefixes without NUL termination and rejects
  // mismatched lengths.
  EXPECT_EQ(0L,
            hashtable_get_index_from_name_n(table.get(), "Alpha_suffix", 5));
  EXPECT_EQ(1L,
            hashtable_get_index_from_name_n(table.get(), "alpha_suffix", 5));
  EXPECT_EQ(-1L, hashtable_get_index_from_name_n(table.get(), "Alpha", 4));
  EXPECT_EQ(-1L, hashtable_get_index_from_name_n(table.get(), "Alpha", 10));
}

TEST(HashtableTest, CaseInsensitiveAddLookupAndDuplicates) {
  ScopedHashtable table(hashtable_new(/*case_sensitive=*/0));
  ASSERT_NE(nullptr, table.get());

  EXPECT_EQ(0, hashtable_add_name(table.get(), "Ozone_Density"));
  EXPECT_EQ(0, hashtable_add_name(table.get(), "Temperature"));
  // Different casing of an existing key is treated as a duplicate.
  EXPECT_EQ(-1, hashtable_add_name(table.get(), "ozone_density"));
  EXPECT_EQ(-1, hashtable_add_name(table.get(), "TEMPERATURE"));

  EXPECT_EQ(0L, hashtable_get_index_from_name(table.get(), "OZONE_DENSITY"));
  EXPECT_EQ(1L, hashtable_get_index_from_name(table.get(), "temperature"));
  EXPECT_EQ(-1L, hashtable_get_index_from_name(table.get(), "pressure"));

  EXPECT_EQ(
      0L, hashtable_get_index_from_name_n(table.get(), "oZoNe_DeNsItY_2", 13));
  EXPECT_EQ(-1L, hashtable_get_index_from_name_n(table.get(), "ozone", 5));
  EXPECT_EQ(-1L,
            hashtable_get_index_from_name_n(table.get(), "temperature", 20));
}

TEST(HashtableTest, InsertAtMiddleIndexShiftsSubsequentIndices) {
  ScopedHashtable table(hashtable_new(/*case_sensitive=*/1));
  ASSERT_NE(nullptr, table.get());

  ASSERT_EQ(0, hashtable_add_name(table.get(), "first"));
  ASSERT_EQ(0, hashtable_add_name(table.get(), "third"));
  ASSERT_EQ(0, hashtable_add_name(table.get(), "fourth"));

  // Insert at index 1; "third" and "fourth" must shift to 2 and 3.
  EXPECT_EQ(0, hashtable_insert_name(table.get(), 1, "second"));
  // Insert at index 0; all existing keys shift by +1.
  EXPECT_EQ(0, hashtable_insert_name(table.get(), 0, "zeroth"));
  // Duplicate insert fails without altering existing indices.
  EXPECT_EQ(-1, hashtable_insert_name(table.get(), 2, "second"));

  EXPECT_EQ(0L, hashtable_get_index_from_name(table.get(), "zeroth"));
  EXPECT_EQ(1L, hashtable_get_index_from_name(table.get(), "first"));
  EXPECT_EQ(2L, hashtable_get_index_from_name(table.get(), "second"));
  EXPECT_EQ(3L, hashtable_get_index_from_name(table.get(), "third"));
  EXPECT_EQ(4L, hashtable_get_index_from_name(table.get(), "fourth"));
}

TEST(HashtableTest, ResizeAndCollisionProbingAcrossManyKeys) {
  // Initial table size is 32 and resizes when used == size / 2 (16, 32, 64).
  // Inserting 100 keys in both modes exercises multiple resizes, rehash
  // collision chains, and non-empty probe loops on missing lookups.
  for (int case_sensitive : {0, 1}) {
    ScopedHashtable table(hashtable_new(case_sensitive));
    ASSERT_NE(nullptr, table.get());

    std::vector<std::string> keys;
    keys.reserve(100);
    for (int i = 0; i < 100; ++i) {
      keys.push_back("Key_Number_" + std::to_string(i));
    }

    for (int i = 0; i < 100; ++i) {
      ASSERT_EQ(0, hashtable_add_name(table.get(), keys[i].c_str()));
    }
    for (int i = 0; i < 100; ++i) {
      EXPECT_EQ(-1, hashtable_add_name(table.get(), keys[i].c_str()));
      EXPECT_EQ(static_cast<long>(i),
                hashtable_get_index_from_name(table.get(), keys[i].c_str()));
      const std::string padded = keys[i] + "_extra";
      EXPECT_EQ(static_cast<long>(i), hashtable_get_index_from_name_n(
                                          table.get(), padded.c_str(),
                                          static_cast<int>(keys[i].size())));
    }

    // Probe a large set of absent keys so both empty-slot and collision-chain
    // misses are exercised for both lookup functions.
    for (int i = 100; i < 200; ++i) {
      const std::string missing = "Key_Number_" + std::to_string(i);
      EXPECT_EQ(-1L,
                hashtable_get_index_from_name(table.get(), missing.c_str()));
      EXPECT_EQ(-1L, hashtable_get_index_from_name_n(
                         table.get(), missing.c_str(),
                         static_cast<int>(missing.size())));
    }
  }
}

TEST(HashtableTest, ExplicitHashBucketCollisions) {
  // With initial size 32 (mask = 0x1f), keys whose hash values share the low
  // 5 bits collide in the first bucket and force step probing.
  // For hash = hash * 0xF4243 ^ c:
  // Single-character strings "A" (0x41) and "a" (0x61) differ by 0x20, so
  // in case-sensitive mode their hashes share the exact same low 5 bits!
  ScopedHashtable cs_table(hashtable_new(/*case_sensitive=*/1));
  ASSERT_NE(nullptr, cs_table.get());
  ASSERT_EQ(0, hashtable_add_name(cs_table.get(), "A"));
  ASSERT_EQ(0, hashtable_add_name(cs_table.get(), "a"));
  // A 3rd key with the same low 5 bits exercises multi-step probing;
  // ASCII 0x81 (unsigned char 129) also has low 5 bits == 1.
  constexpr char kHighByte[] = {static_cast<char>(0x81), '\0'};
  ASSERT_EQ(0, hashtable_add_name(cs_table.get(), kHighByte));

  EXPECT_EQ(0L, hashtable_get_index_from_name(cs_table.get(), "A"));
  EXPECT_EQ(1L, hashtable_get_index_from_name(cs_table.get(), "a"));
  EXPECT_EQ(2L, hashtable_get_index_from_name(cs_table.get(), kHighByte));

  // Absent key with the same low 5 bits (0x21 = '!') traverses the collision
  // chain and exits with -1.
  EXPECT_EQ(-1L, hashtable_get_index_from_name(cs_table.get(), "!"));
  EXPECT_EQ(-1L, hashtable_get_index_from_name_n(cs_table.get(), "!", 1));
  EXPECT_EQ(1L, hashtable_get_index_from_name_n(cs_table.get(), "ab", 1));
  // Same low 5 bits but different length ("!!" length 2 vs "!" length 1).
  constexpr char kTwoA[] = {static_cast<char>(0xa1), '\0'};
  EXPECT_EQ(-1L, hashtable_get_index_from_name(cs_table.get(), kTwoA));
}

}  // namespace
}  // namespace stcorp_harp
