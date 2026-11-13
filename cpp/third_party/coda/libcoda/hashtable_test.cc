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
// Tests CODA string-to-index hash tables (libcoda/hashtable.c).
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/hashtable.c
//   https://github.com/stcorp/coda/blob/master/libcoda/hashtable.h

#include "third_party/stcorp_coda/libcoda/hashtable.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"

namespace stcorp_coda {
namespace {

// Mirrors strhash / strcasehash in libcoda/hashtable.c so tests can synthesize
// keys that deterministically collide in both initial (size=32) and resized
// (size=64) buckets and exercise wrap-around and multi-step probing.
unsigned long ComputeStrHash(std::string_view str, bool case_sensitive) {
  unsigned long hash = 0;
  for (const char ch : str) {
    auto lc = static_cast<unsigned char>(ch);
    if (!case_sensitive && lc >= 'A' && lc <= 'Z') {
      lc = static_cast<unsigned char>(lc + 32);
    }
    hash = (hash * 0xF4243UL) ^ lc;
  }
  return hash;
}

std::vector<std::string> FindKeysWithLowHashBits(size_t count, size_t key_len,
                                                 unsigned long mask,
                                                 unsigned long target_bits) {
  std::vector<std::string> result;
  if (key_len == 0) {
    return result;
  }
  result.reserve(count);
  std::string current(key_len, 'a');
  while (result.size() < count) {
    if ((ComputeStrHash(current, /*case_sensitive=*/true) & mask) ==
        target_bits) {
      result.push_back(current);
    }
    bool advanced = false;
    for (size_t pos = key_len; pos > 0; --pos) {
      if (current[pos - 1] < 'z') {
        ++current[pos - 1];
        advanced = true;
        break;
      }
      current[pos - 1] = 'a';
    }
    if (!advanced) {
      break;
    }
  }
  return result;
}

std::string ToUpper(std::string_view sv) {
  std::string out(sv);
  for (char& ch : out) {
    if (ch >= 'a' && ch <= 'z') {
      ch = static_cast<char>(ch - ('a' - 'A'));
    }
  }
  return out;
}

TEST(HashtableTest, DeleteNullAndEmptyTablesAreSafeAndMissLookups) {
  hashtable_delete(nullptr);

  for (const int case_sensitive : {0, 1}) {
    ScopedHashtable ht(hashtable_new(case_sensitive));
    ASSERT_NE(nullptr, ht.get());
    EXPECT_EQ(-1, hashtable_get_index_from_name(ht.get(), "missing"));
    EXPECT_EQ(-1, hashtable_get_index_from_name(ht.get(), ""));
    EXPECT_EQ(-1, hashtable_get_index_from_name_n(ht.get(), "missing", 7));
    EXPECT_EQ(-1, hashtable_get_index_from_name_n(ht.get(), "", 0));
  }
}

TEST(HashtableTest, AddAndLookupCaseSensitiveKeys) {
  constexpr std::array<const char*, 6> kKeys = {
      "", "alpha", "Alpha", "ALPHA", "a@Z[z", "\xC0\xFF",
  };
  ScopedHashtable ht(hashtable_new(/*case_sensitive=*/1));
  ASSERT_NE(nullptr, ht.get());

  for (size_t i = 0; i < kKeys.size(); ++i) {
    EXPECT_EQ(0, hashtable_add_name(ht.get(), kKeys[i])) << "i=" << i;
  }

  // Exact duplicate is rejected without disturbing stored indices.
  EXPECT_EQ(-1, hashtable_add_name(ht.get(), "Alpha"));
  EXPECT_EQ(-1, hashtable_add_name(ht.get(), ""));

  for (size_t i = 0; i < kKeys.size(); ++i) {
    EXPECT_EQ(static_cast<long>(i),
              hashtable_get_index_from_name(ht.get(), kKeys[i]));
  }

  EXPECT_EQ(-1, hashtable_get_index_from_name(ht.get(), "aLPHA"));
  EXPECT_EQ(-1, hashtable_get_index_from_name(ht.get(), "ALPHA_"));
  EXPECT_EQ(-1, hashtable_get_index_from_name(ht.get(), "beta"));
}

TEST(HashtableTest, AddAndLookupCaseInsensitiveKeysAndAsciiBoundaries) {
  constexpr std::array<const char*, 4> kKeys = {
      "Alpha",
      "",
      "@AZ[az",
      "\xC0key",
  };
  ScopedHashtable ht(hashtable_new(/*case_sensitive=*/0));
  ASSERT_NE(nullptr, ht.get());

  for (size_t i = 0; i < kKeys.size(); ++i) {
    EXPECT_EQ(0, hashtable_add_name(ht.get(), kKeys[i])) << "i=" << i;
  }

  // Case-insensitive duplicates are rejected.
  EXPECT_EQ(-1, hashtable_add_name(ht.get(), "alpha"));
  EXPECT_EQ(-1, hashtable_add_name(ht.get(), "@az[AZ"));
  EXPECT_EQ(-1, hashtable_add_name(ht.get(), ""));

  struct LookupCase {
    const char* query;
    long expected_index;
  };
  // '@' (0x40 = 'A'-1) and '[' (0x5B = 'Z'+1) must not fold into '`' / '{'.
  constexpr LookupCase kLookupCases[] = {
      {.query = "Alpha", .expected_index = 0},
      {.query = "alpha", .expected_index = 0},
      {.query = "ALPHA", .expected_index = 0},
      {.query = "aLpHa", .expected_index = 0},
      {.query = "", .expected_index = 1},
      {.query = "@AZ[az", .expected_index = 2},
      {.query = "@az[AZ", .expected_index = 2},
      {.query = "`az{AZ", .expected_index = -1},
      {.query = "@az{AZ", .expected_index = -1},
      {.query = "`AZ[az", .expected_index = -1},
      {.query = "\xC0key", .expected_index = 3},
      {.query = "\xC0KEY", .expected_index = 3},
      {.query = "missing", .expected_index = -1},
  };
  for (const LookupCase& tc : kLookupCases) {
    EXPECT_EQ(tc.expected_index,
              hashtable_get_index_from_name(ht.get(), tc.query))
        << "query=" << tc.query;
  }
}

TEST(HashtableTest, LookupByLengthHandlesSlicesZeroLongAndNonNulBuffers) {
  constexpr std::array<const char*, 3> kKeys = {
      "",
      "alpha",
      "alphabetic",
  };
  constexpr std::array<char, 5> kNonNulLower = {'a', 'l', 'p', 'h', 'a'};
  constexpr std::array<char, 5> kNonNulMixed = {'A', 'l', 'p', 'h', 'a'};

  for (const int case_sensitive : {0, 1}) {
    ScopedHashtable ht(hashtable_new(case_sensitive));
    ASSERT_NE(nullptr, ht.get());
    for (const char* key : kKeys) {
      ASSERT_EQ(0, hashtable_add_name(ht.get(), key));
    }

    // Zero-length lookup matches empty key regardless of buffer content.
    EXPECT_EQ(0, hashtable_get_index_from_name_n(ht.get(), "", 0));
    EXPECT_EQ(0, hashtable_get_index_from_name_n(ht.get(), "alpha", 0));

    // Prefix slice of a longer string and non-NUL-terminated buffer.
    EXPECT_EQ(1, hashtable_get_index_from_name_n(ht.get(), "alpha_extra", 5));
    EXPECT_EQ(1, hashtable_get_index_from_name_n(
                     ht.get(), kNonNulLower.data(),
                     static_cast<int>(kNonNulLower.size())));
    EXPECT_EQ(
        case_sensitive ? -1 : 1,
        hashtable_get_index_from_name_n(ht.get(), kNonNulMixed.data(),
                                        static_cast<int>(kNonNulMixed.size())));

    // Length exceeding strlen stops at NUL and mismatches stored name_length.
    EXPECT_EQ(-1, hashtable_get_index_from_name_n(ht.get(), "alpha", 10));
    EXPECT_EQ(-1, hashtable_get_index_from_name_n(ht.get(), "alpha", 3));

    EXPECT_EQ(2, hashtable_get_index_from_name_n(ht.get(), "alphabetic", 10));
    EXPECT_EQ(case_sensitive ? -1 : 2,
              hashtable_get_index_from_name_n(ht.get(), "ALPHABETIC", 10));
  }
}

TEST(HashtableTest, InsertNameShiftsSubsequentIndicesAndKeepsOrderOnDuplicate) {
  for (const int case_sensitive : {0, 1}) {
    ScopedHashtable ht(hashtable_new(case_sensitive));
    ASSERT_NE(nullptr, ht.get());

    // 1. Insert at start, start again, middle, and end (index == used).
    ASSERT_EQ(0, hashtable_insert_name(ht.get(), 0, "c"));
    ASSERT_EQ(0, hashtable_insert_name(ht.get(), 0, "a"));
    ASSERT_EQ(0, hashtable_insert_name(ht.get(), 1, "b"));
    ASSERT_EQ(0, hashtable_insert_name(ht.get(), 3, "d"));

    EXPECT_EQ(0, hashtable_get_index_from_name(ht.get(), "a"));
    EXPECT_EQ(1, hashtable_get_index_from_name(ht.get(), "b"));
    EXPECT_EQ(2, hashtable_get_index_from_name(ht.get(), "c"));
    EXPECT_EQ(3, hashtable_get_index_from_name(ht.get(), "d"));

    // 2. Duplicate at index < used must fail without shifting existing indices.
    EXPECT_EQ(-1, hashtable_insert_name(ht.get(), 0, "b"));
    if (case_sensitive == 0) {
      EXPECT_EQ(-1, hashtable_insert_name(ht.get(), 0, "B"));
    }
    EXPECT_EQ(0, hashtable_get_index_from_name(ht.get(), "a"));
    EXPECT_EQ(1, hashtable_get_index_from_name(ht.get(), "b"));
    EXPECT_EQ(2, hashtable_get_index_from_name(ht.get(), "c"));
    EXPECT_EQ(3, hashtable_get_index_from_name(ht.get(), "d"));
    EXPECT_EQ(-1, hashtable_get_index_from_name_n(ht.get(), "a", 0));
  }
}

TEST(HashtableTest, CollisionsExerciseMultiStepProbingAndWrapAround) {
  // Bucket 0 with bits 5..8 == 0 forces step=1 and i < step wrap-around (0->31)
  // followed by multi-step non-wrapping probes (31->30->29). Bucket 20 forces
  // i >= step (20->19->18). Length-3 keys in bucket 0 test length mismatch.
  const std::vector<std::string> bucket0_len4 =
      FindKeysWithLowHashBits(4, 4, 0x1FFUL, 0UL);
  const std::vector<std::string> bucket0_len3 =
      FindKeysWithLowHashBits(2, 3, 0x1FUL, 0UL);
  const std::vector<std::string> bucket20_len4 =
      FindKeysWithLowHashBits(4, 4, 0x1FFUL, 20UL);
  ASSERT_EQ(4u, bucket0_len4.size());
  ASSERT_EQ(2u, bucket0_len3.size());
  ASSERT_EQ(4u, bucket20_len4.size());

  // Keep all inserted key strings in `inserted` (declared before `ht` so
  // `ht` is destroyed while all key buffers remain alive).
  std::vector<std::string> inserted = {
      bucket0_len4[0],  bucket0_len4[1],  bucket0_len4[2],  bucket0_len3[0],
      bucket20_len4[0], bucket20_len4[1], bucket20_len4[2],
  };
  const std::vector<std::string> uninserted_collisions = {
      bucket0_len4[3],
      bucket0_len3[1],
      bucket20_len4[3],
  };

  for (const int case_sensitive : {0, 1}) {
    ScopedHashtable ht(hashtable_new(case_sensitive));
    ASSERT_NE(nullptr, ht.get());

    for (size_t i = 0; i < inserted.size(); ++i) {
      ASSERT_EQ(0, hashtable_add_name(ht.get(), inserted[i].c_str()))
          << "i=" << i;
    }

    // Duplicate detected after multiple probe steps.
    EXPECT_EQ(-1, hashtable_add_name(ht.get(), inserted[2].c_str()));
    EXPECT_EQ(-1, hashtable_add_name(ht.get(), inserted[6].c_str()));

    for (size_t i = 0; i < inserted.size(); ++i) {
      const long expected = static_cast<long>(i);
      EXPECT_EQ(expected,
                hashtable_get_index_from_name(ht.get(), inserted[i].c_str()));
      EXPECT_EQ(expected, hashtable_get_index_from_name_n(
                              ht.get(), inserted[i].data(),
                              static_cast<int>(inserted[i].size())));
      if (case_sensitive == 0) {
        const std::string upper = ToUpper(inserted[i]);
        EXPECT_EQ(expected,
                  hashtable_get_index_from_name(ht.get(), upper.c_str()));
        EXPECT_EQ(expected,
                  hashtable_get_index_from_name_n(
                      ht.get(), upper.data(), static_cast<int>(upper.size())));
      }
    }

    // Colliding keys that were never inserted must probe and return -1.
    for (const std::string& miss : uninserted_collisions) {
      EXPECT_EQ(-1, hashtable_get_index_from_name(ht.get(), miss.c_str()));
      EXPECT_EQ(-1, hashtable_get_index_from_name_n(
                        ht.get(), miss.data(), static_cast<int>(miss.size())));
    }
  }
}

TEST(HashtableTest, ResizeAcrossMultiplePowersRehashesCollisionsAndShifts) {
  // Four keys in bucket 0 with hash & 0x1FF == 0 also collide in the resized
  // size=64 table (step=1, j < step wrap 0->63 and multi-step 63->62->61).
  // Four keys in bucket 20 with hash & 0x1FF == 20 test j >= step in resize.
  const std::vector<std::string> bucket0_len4 =
      FindKeysWithLowHashBits(4, 4, 0x1FFUL, 0UL);
  const std::vector<std::string> bucket20_len4 =
      FindKeysWithLowHashBits(4, 4, 0x1FFUL, 20UL);
  ASSERT_EQ(4u, bucket0_len4.size());
  ASSERT_EQ(4u, bucket20_len4.size());

  // 16 initial keys (triggers resize on the 17th unique insert) + 26 more
  // keys = 42 total keys, crossing both 16 (32->64) and 32 (64->128) resizes.
  std::vector<std::string> keys;
  keys.reserve(42);
  for (const std::string& k : bucket0_len4) {
    keys.push_back(k);
  }
  for (const std::string& k : bucket20_len4) {
    keys.push_back(k);
  }
  for (int i = 0; i < 34; ++i) {
    keys.push_back("item_" + std::to_string(i));
  }
  ASSERT_EQ(42u, keys.size());

  for (const int case_sensitive : {0, 1}) {
    ScopedHashtable ht(hashtable_new(case_sensitive));
    ASSERT_NE(nullptr, ht.get());

    // 1. Fill to the first half-full boundary (used == 16 == size >> 1).
    for (size_t i = 0; i < 16; ++i) {
      ASSERT_EQ(0, hashtable_add_name(ht.get(), keys[i].c_str())) << "i=" << i;
    }

    // 2. Duplicate at used == 16 must fail without triggering resize or shift.
    EXPECT_EQ(-1, hashtable_insert_name(ht.get(), 0, keys[2].c_str()));

    // 3. Insert key[16] at index 0: triggers 32->64 resize and shifts 0..15.
    ASSERT_EQ(0, hashtable_insert_name(ht.get(), 0, keys[16].c_str()));
    EXPECT_EQ(0, hashtable_get_index_from_name(ht.get(), keys[16].c_str()));
    for (size_t i = 0; i < 16; ++i) {
      EXPECT_EQ(static_cast<long>(i + 1),
                hashtable_get_index_from_name(ht.get(), keys[i].c_str()));
    }

    // 4. Add remaining keys[17..41] (crosses used == 32 -> 64->128 resize).
    for (size_t i = 17; i < keys.size(); ++i) {
      ASSERT_EQ(0, hashtable_add_name(ht.get(), keys[i].c_str())) << "i=" << i;
    }

    // 5. Verify every key's exact final index via both lookup functions.
    auto expected_index_for = [](size_t pos) -> long {
      if (pos == 16) {
        return 0;
      }
      if (pos < 16) {
        return static_cast<long>(pos + 1);
      }
      return static_cast<long>(pos);
    };
    for (size_t i = 0; i < keys.size(); ++i) {
      const long expected = expected_index_for(i);
      EXPECT_EQ(expected,
                hashtable_get_index_from_name(ht.get(), keys[i].c_str()))
          << "i=" << i;
      EXPECT_EQ(expected,
                hashtable_get_index_from_name_n(
                    ht.get(), keys[i].data(), static_cast<int>(keys[i].size())))
          << "i=" << i;
      if (case_sensitive == 0) {
        const std::string upper = ToUpper(keys[i]);
        EXPECT_EQ(expected,
                  hashtable_get_index_from_name(ht.get(), upper.c_str()))
            << "i=" << i;
      }
    }
  }
}

// TODO(schwehr): 99.6% line coverage in libcoda/hashtable.c (224/225); only
// line 133 (calloc OOM branch) is missed.

}  // namespace
}  // namespace stcorp_coda
