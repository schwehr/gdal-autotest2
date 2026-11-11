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

// Unit tests for //third_party/netcdf/libdispatch/nchashmap.c.
//
// Exercises allocation/prime sizing, CRC64 key hashing, CRUD operations,
// linear-probe collisions/wraparound, tombstone saturation/reuse, rehashing,
// NC_hashmapdeactivate, and debug dump helpers.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/nchashmap.c

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"

extern "C" {
#include "third_party/netcdf/include/nchashmap.h"
int NC_hashmapdeactivate(NC_hashmap* map, uintptr_t data);
void printhashmapstats(NC_hashmap* hm);
void printhashmap(NC_hashmap* hm);
}  // extern "C"

namespace netcdf {
namespace {

struct HashmapDeleter {
  void operator()(NC_hashmap* hm) const { NC_hashmapfree(hm); }
};

using ScopedHashmap = std::unique_ptr<NC_hashmap, HashmapDeleter>;

constexpr size_t kMinTableSize = 131U;
constexpr size_t kHashBits = sizeof(nchashkey_t) * 8;
static_assert(kHashBits <= 32, "Expected 32-bit nchashkey_t");

std::string ApplyBitMask(const std::string& base, size_t num_bits,
                         uint64_t mask) {
  std::string out = base;
  for (size_t i = 0; i < num_bits; ++i) {
    if ((mask & (uint64_t{1} << i)) != 0) {
      const size_t byte_idx = i / 4;
      const unsigned bit_mask = 1U << (i % 4);
      out[byte_idx] = static_cast<char>(
          static_cast<unsigned char>(out[byte_idx]) ^ bit_mask);
    }
  }
  return out;
}

struct HashLinearBasis {
  std::string base;
  size_t num_bits = 0;
  nchashkey_t base_hash = 0;
  std::array<nchashkey_t, kHashBits> basis_vec{};
  std::array<uint64_t, kHashBits> basis_comb{};
  std::vector<uint64_t> null_masks;
};

// Builds a GF(2) linear basis for NC_hashmapkey() over printable ASCII keys of
// length `key_len` (modifying bits 0..3 of `'a'`, keeping bytes in '`'..'o').
HashLinearBasis BuildHashLinearBasis(size_t key_len) {
  HashLinearBasis hb;
  hb.base = std::string(key_len, 'a');
  hb.num_bits = std::min<size_t>(key_len * 4, 63);
  hb.base_hash = NC_hashmapkey(hb.base.data(), key_len);

  for (size_t i = 0; i < hb.num_bits; ++i) {
    const std::string perturbed =
        ApplyBitMask(hb.base, hb.num_bits, uint64_t{1} << i);
    nchashkey_t v = NC_hashmapkey(perturbed.data(), key_len) ^ hb.base_hash;
    uint64_t comb = uint64_t{1} << i;

    for (size_t b = 0; b < kHashBits; ++b) {
      if (((v >> b) & nchashkey_t{1}) == 0) {
        continue;
      }
      if (hb.basis_vec[b] != 0) {
        v ^= hb.basis_vec[b];
        comb ^= hb.basis_comb[b];
      } else {
        hb.basis_vec[b] = v;
        hb.basis_comb[b] = comb;
        comb = 0;
        break;
      }
    }
    if (v == 0 && comb != 0) {
      hb.null_masks.push_back(comb);
    }
  }
  return hb;
}

std::vector<std::string> FindCollidingKeys(size_t key_len, size_t needed) {
  const HashLinearBasis hb = BuildHashLinearBasis(key_len);
  std::vector<std::string> keys;
  keys.push_back(hb.base);
  for (size_t m = 0; m < hb.null_masks.size() && keys.size() < needed; ++m) {
    keys.push_back(ApplyBitMask(hb.base, hb.num_bits, hb.null_masks[m]));
  }
  return keys;
}

std::optional<std::string> FindKeyWithHash(size_t key_len,
                                           nchashkey_t target_hash) {
  const HashLinearBasis hb = BuildHashLinearBasis(key_len);
  nchashkey_t diff = target_hash ^ hb.base_hash;
  uint64_t sol_mask = 0;
  for (size_t b = 0; b < kHashBits; ++b) {
    if (((diff >> b) & nchashkey_t{1}) != 0) {
      if (hb.basis_vec[b] == 0) {
        return std::nullopt;
      }
      diff ^= hb.basis_vec[b];
      sol_mask ^= hb.basis_comb[b];
    }
  }
  return ApplyBitMask(hb.base, hb.num_bits, sol_mask);
}

TEST(NchashmapTest, NewHandlesDefaultMediumLargeAndOverflowSizes) {
  for (const size_t small_size : {size_t{0}, size_t{1}, size_t{130}}) {
    SCOPED_TRACE(small_size);
    ScopedHashmap hm(NC_hashmapnew(small_size));
    ASSERT_NE(nullptr, hm);
    EXPECT_EQ(kMinTableSize, hm->alloc);
    EXPECT_EQ(0U, hm->active);
    EXPECT_EQ(0U, NC_hashmapcount(hm.get()));
    EXPECT_NE(nullptr, hm->table);
  }

  // startsize == 131 hits the >= MINTABLESIZE branch and binary-searches lower.
  {
    ScopedHashmap hm(NC_hashmapnew(131));
    ASSERT_NE(nullptr, hm);
    EXPECT_EQ(179U, hm->alloc);
  }

  // startsize == 131000 hits the upper midpoint branch in findPrimeGreaterThan.
  {
    ScopedHashmap hm(NC_hashmapnew(131000));
    ASSERT_NE(nullptr, hm);
    EXPECT_EQ(174673U, hm->alloc);
  }

  // Above largest NC_primes entry (180503), exercises nextPrime() for even
  // (150000*4/3 == 200000) and odd (150001*4/3 == 200001) targets.
  for (const size_t large_size : {size_t{150000}, size_t{150001}}) {
    SCOPED_TRACE(large_size);
    ScopedHashmap hm(NC_hashmapnew(large_size));
    ASSERT_NE(nullptr, hm);
    EXPECT_GT(hm->alloc, 200000U);
    EXPECT_EQ(1, NC_hashmapadd(hm.get(), 42, "probe", 5));
    uintptr_t val = 0;
    EXPECT_EQ(1, NC_hashmapget(hm.get(), "probe", 5, &val));
    EXPECT_EQ(42U, val);
  }

  // Overflow guard: startsize > SIZE_MAX / 4 returns NULL.
  EXPECT_EQ(nullptr, NC_hashmapnew(std::numeric_limits<size_t>::max()));
  EXPECT_EQ(nullptr,
            NC_hashmapnew((std::numeric_limits<size_t>::max() / 4) + 1));

  // Target >= 0xFFFFFFFF after 4/3 scaling exceeds NC_primes and returns NULL.
  if constexpr (std::numeric_limits<size_t>::max() / 4 >= 0xFFFFFFFFULL) {
    EXPECT_EQ(nullptr, NC_hashmapnew(size_t{0xFFFFFFFFU}));
  }

  // NC_hashmapfree(NULL) is a safe no-op returning 1.
  EXPECT_EQ(1, NC_hashmapfree(nullptr));
}

TEST(NchashmapTest, HashmapkeyHashesNormalPrefixAndEmbeddedNulKeys) {
  const nchashkey_t h1 = NC_hashmapkey("temperature", 11);
  const nchashkey_t h2 = NC_hashmapkey("temperature", 11);
  EXPECT_EQ(h1, h2);

  EXPECT_NE(NC_hashmapkey("temp", 4), NC_hashmapkey("temp2", 5));

  constexpr char kBinA[] = {'a', 'b', '\0', 'c'};
  constexpr char kBinB[] = {'a', 'b', '\0', 'd'};
  EXPECT_NE(NC_hashmapkey(kBinA, sizeof(kBinA)),
            NC_hashmapkey(kBinB, sizeof(kBinB)));
  EXPECT_EQ(nchashkey_t{0}, NC_hashmapkey("", 0));
}

TEST(NchashmapTest, RejectsInvalidArgumentsAndEmptyMapLookups) {
  ScopedHashmap hm(NC_hashmapnew(0));
  ASSERT_NE(nullptr, hm);

  uintptr_t val = 9876;
  EXPECT_EQ(0, NC_hashmapadd(hm.get(), 1, nullptr, 3));
  EXPECT_EQ(0, NC_hashmapadd(hm.get(), 1, "abc", 0));

  EXPECT_EQ(0, NC_hashmapget(hm.get(), nullptr, 3, &val));
  EXPECT_EQ(0, NC_hashmapget(hm.get(), "abc", 0, &val));
  EXPECT_EQ(0, NC_hashmapget(hm.get(), "abc", 3, &val));
  EXPECT_EQ(9876U, val);

  EXPECT_EQ(0, NC_hashmapremove(hm.get(), nullptr, 3, &val));
  EXPECT_EQ(0, NC_hashmapremove(hm.get(), "abc", 0, &val));
  EXPECT_EQ(0, NC_hashmapremove(hm.get(), "abc", 3, &val));

  EXPECT_EQ(0, NC_hashmapsetdata(hm.get(), nullptr, 3, 1));
  EXPECT_EQ(0, NC_hashmapsetdata(hm.get(), "abc", 0, 1));
  EXPECT_EQ(0, NC_hashmapsetdata(nullptr, "abc", 3, 1));
  EXPECT_EQ(0, NC_hashmapsetdata(hm.get(), "abc", 3, 1));

  EXPECT_EQ(0, NC_hashmapdeactivate(hm.get(), 1));
  EXPECT_EQ(0U, NC_hashmapcount(hm.get()));
}

TEST(NchashmapTest, AddGetSetdataRemoveOverwriteAndIthRoundTrip) {
  ScopedHashmap hm(NC_hashmapnew(0));
  ASSERT_NE(nullptr, hm);

  // Verify caller key buffer mutation does not corrupt internal key copy.
  std::string mutable_key = "latitude";
  ASSERT_EQ(
      1, NC_hashmapadd(hm.get(), 101, mutable_key.data(), mutable_key.size()));
  mutable_key[0] = 'X';

  uintptr_t val = 0;
  EXPECT_EQ(1, NC_hashmapget(hm.get(), "latitude", 8, &val));
  EXPECT_EQ(101U, val);
  EXPECT_EQ(0, NC_hashmapget(hm.get(), "Xatitude", 8, &val));
  EXPECT_EQ(1, NC_hashmapget(hm.get(), "latitude", 8, nullptr));

  // Binary keys containing embedded NUL bytes are compared by exact keysize.
  constexpr char kBin1[] = {'k', '\0', '1'};
  constexpr char kBin2[] = {'k', '\0', '2'};
  ASSERT_EQ(1, NC_hashmapadd(hm.get(), 201, kBin1, sizeof(kBin1)));
  ASSERT_EQ(1, NC_hashmapadd(hm.get(), 202, kBin2, sizeof(kBin2)));
  ASSERT_EQ(1, NC_hashmapadd(hm.get(), 203, "k", 1));
  EXPECT_EQ(4U, NC_hashmapcount(hm.get()));

  // Overwrite via NC_hashmapadd and update via NC_hashmapsetdata.
  EXPECT_EQ(1, NC_hashmapadd(hm.get(), 102, "latitude", 8));
  EXPECT_EQ(4U, NC_hashmapcount(hm.get()));
  EXPECT_EQ(1, NC_hashmapget(hm.get(), "latitude", 8, &val));
  EXPECT_EQ(102U, val);

  EXPECT_EQ(1, NC_hashmapsetdata(hm.get(), kBin1, sizeof(kBin1), 999));
  EXPECT_EQ(1, NC_hashmapget(hm.get(), kBin1, sizeof(kBin1), &val));
  EXPECT_EQ(999U, val);

  // Remove with datap != nullptr, with datap == nullptr, and double-remove.
  uintptr_t removed = 0;
  EXPECT_EQ(1, NC_hashmapremove(hm.get(), kBin2, sizeof(kBin2), &removed));
  EXPECT_EQ(202U, removed);
  EXPECT_EQ(0, NC_hashmapremove(hm.get(), kBin2, sizeof(kBin2), &removed));
  EXPECT_EQ(1, NC_hashmapremove(hm.get(), "k", 1, nullptr));
  EXPECT_EQ(2U, NC_hashmapcount(hm.get()));

  // Missing key lookup when active > 0 and empty slots remain.
  EXPECT_EQ(0, NC_hashmapget(hm.get(), "absent", 6, &val));

  // NC_hashmapith parameter validation and full table scan.
  const char* key_out = nullptr;
  EXPECT_EQ(NC_EINVAL, NC_hashmapith(nullptr, 0, &val, &key_out));
  EXPECT_EQ(NC_EINVAL, NC_hashmapith(hm.get(), hm->alloc, &val, &key_out));
  EXPECT_EQ(NC_EINVAL, NC_hashmapith(hm.get(), hm->alloc + 7, &val, &key_out));

  size_t active_seen = 0;
  bool saw_latitude = false;
  for (size_t i = 0; i < hm->alloc; ++i) {
    ASSERT_EQ(NC_NOERR, NC_hashmapith(hm.get(), i, nullptr, nullptr));
    val = 7777;
    key_out = "sentinel";
    ASSERT_EQ(NC_NOERR, NC_hashmapith(hm.get(), i, &val, &key_out));
    if (key_out != nullptr) {
      ++active_seen;
      if (std::string_view(key_out) == "latitude") {
        saw_latitude = true;
        EXPECT_EQ(102U, val);
      }
    } else {
      EXPECT_EQ(0U, val);
    }
  }
  EXPECT_EQ(2U, active_seen);
  EXPECT_TRUE(saw_latitude);
}

TEST(NchashmapTest, HandlesExactHashCollisionsTombstoneProbingAndWraparound) {
  // Part A: Same 32-bit hashkey AND same keysize (10) -> exercises memcmp()
  // mismatch during linear probing and consecutive DELETED tombstones.
  const std::vector<std::string> colliding = FindCollidingKeys(10, 4);
  ASSERT_EQ(4U, colliding.size());
  const nchashkey_t shared_hash = NC_hashmapkey(colliding[0].data(), 10);
  for (size_t i = 1; i < colliding.size(); ++i) {
    ASSERT_EQ(shared_hash, NC_hashmapkey(colliding[i].data(), 10));
    for (size_t j = 0; j < i; ++j) {
      ASSERT_NE(colliding[j], colliding[i]);
    }
  }

  ScopedHashmap hm(NC_hashmapnew(0));
  ASSERT_NE(nullptr, hm);

  for (size_t i = 0; i < colliding.size(); ++i) {
    ASSERT_EQ(1, NC_hashmapadd(hm.get(), 100 + i, colliding[i].data(),
                               colliding[i].size()));
  }
  EXPECT_EQ(4U, NC_hashmapcount(hm.get()));

  // Remove first two keys to leave two consecutive DELETED slots in the chain.
  uintptr_t val = 0;
  ASSERT_EQ(1, NC_hashmapremove(hm.get(), colliding[0].data(), 10, &val));
  EXPECT_EQ(100U, val);
  ASSERT_EQ(1, NC_hashmapremove(hm.get(), colliding[1].data(), 10, &val));
  EXPECT_EQ(101U, val);

  // Probe past both DELETED slots and an ACTIVE hash-colliding key.
  EXPECT_EQ(0, NC_hashmapget(hm.get(), colliding[0].data(), 10, &val));
  EXPECT_EQ(1, NC_hashmapget(hm.get(), colliding[3].data(), 10, &val));
  EXPECT_EQ(103U, val);
  EXPECT_EQ(1, NC_hashmapsetdata(hm.get(), colliding[2].data(), 10, 502));
  EXPECT_EQ(1, NC_hashmapget(hm.get(), colliding[2].data(), 10, &val));
  EXPECT_EQ(502U, val);

  // Part B: Same hashkey but different keysize (11) exercises keysize check.
  const std::optional<std::string> diff_len = FindKeyWithHash(11, shared_hash);
  if (diff_len.has_value()) {
    ASSERT_EQ(shared_hash, NC_hashmapkey(diff_len->data(), diff_len->size()));
    ASSERT_EQ(1,
              NC_hashmapadd(hm.get(), 600, diff_len->data(), diff_len->size()));
    EXPECT_EQ(
        1, NC_hashmapget(hm.get(), diff_len->data(), diff_len->size(), &val));
    EXPECT_EQ(600U, val);
    EXPECT_EQ(1, NC_hashmapget(hm.get(), colliding[3].data(), 10, &val));
    EXPECT_EQ(103U, val);
  }

  // Part C: Linear probing wraparound from index (alloc - 1) to 0.
  ScopedHashmap wrap_hm(NC_hashmapnew(0));
  ASSERT_NE(nullptr, wrap_hm);
  std::vector<std::string> last_slot_keys;
  for (int n = 0; n < 5000 && last_slot_keys.size() < 2; ++n) {
    std::string candidate = absl::StrFormat("wrap_%d", n);
    if (NC_hashmapkey(candidate.data(), candidate.size()) % wrap_hm->alloc ==
        wrap_hm->alloc - 1) {
      last_slot_keys.push_back(std::move(candidate));
    }
  }
  ASSERT_EQ(2U, last_slot_keys.size());
  ASSERT_EQ(1, NC_hashmapadd(wrap_hm.get(), 11, last_slot_keys[0].data(),
                             last_slot_keys[0].size()));
  ASSERT_EQ(1, NC_hashmapadd(wrap_hm.get(), 22, last_slot_keys[1].data(),
                             last_slot_keys[1].size()));

  const char* slot_key = nullptr;
  ASSERT_EQ(NC_NOERR,
            NC_hashmapith(wrap_hm.get(), wrap_hm->alloc - 1, &val, &slot_key));
  EXPECT_EQ(11U, val);
  EXPECT_STREQ(last_slot_keys[0].c_str(), slot_key);

  ASSERT_EQ(NC_NOERR, NC_hashmapith(wrap_hm.get(), 0, &val, &slot_key));
  EXPECT_EQ(22U, val);
  EXPECT_STREQ(last_slot_keys[1].c_str(), slot_key);

  ASSERT_EQ(1, NC_hashmapremove(wrap_hm.get(), last_slot_keys[0].data(),
                                last_slot_keys[0].size(), nullptr));
  EXPECT_EQ(1, NC_hashmapget(wrap_hm.get(), last_slot_keys[1].data(),
                             last_slot_keys[1].size(), &val));
  EXPECT_EQ(22U, val);
}

TEST(NchashmapTest, TombstoneSaturatedTableHandlesMissesAndReusesDeletedSlot) {
  ScopedHashmap hm(NC_hashmapnew(0));
  ASSERT_NE(nullptr, hm);
  ASSERT_EQ(kMinTableSize, hm->alloc);

  ASSERT_EQ(1, NC_hashmapadd(hm.get(), 999, "anchor", 6));

  // Add and remove (alloc - 1) distinct keys while active <= 2 so rehash never
  // triggers and locate() fills every remaining EMPTY slot before turning it
  // into DELETED. Result: 1 ACTIVE slot, (alloc - 1) DELETED slots, 0 EMPTY.
  for (size_t i = 1; i < hm->alloc; ++i) {
    const std::string key = absl::StrFormat("tomb_%zu", i);
    ASSERT_EQ(1, NC_hashmapadd(hm.get(), i, key.data(), key.size()));
    uintptr_t removed = 0;
    ASSERT_EQ(1, NC_hashmapremove(hm.get(), key.data(), key.size(), &removed));
    EXPECT_EQ(i, removed);
  }
  ASSERT_EQ(1U, NC_hashmapcount(hm.get()));
  for (size_t i = 0; i < hm->alloc; ++i) {
    EXPECT_NE(0, hm->table[i].flags);
  }

  // With zero EMPTY slots, locate(..., deletedok=0) returns 0 for missing keys.
  uintptr_t val = 0;
  EXPECT_EQ(0, NC_hashmapget(hm.get(), "missing", 7, &val));
  EXPECT_EQ(0, NC_hashmapremove(hm.get(), "missing", 7, &val));
  EXPECT_EQ(0, NC_hashmapsetdata(hm.get(), "missing", 7, 123));

  EXPECT_EQ(1, NC_hashmapget(hm.get(), "anchor", 6, &val));
  EXPECT_EQ(999U, val);

  // Inserting a new key with deletedok=1 reuses the first DELETED tombstone.
  EXPECT_EQ(1, NC_hashmapadd(hm.get(), 777, "reused", 6));
  EXPECT_EQ(2U, NC_hashmapcount(hm.get()));
  EXPECT_EQ(1, NC_hashmapget(hm.get(), "reused", 6, &val));
  EXPECT_EQ(777U, val);
}

TEST(NchashmapTest, RehashesAcrossGrowthThresholdsPreservingActiveEntries) {
  ScopedHashmap hm(NC_hashmapnew(0));
  ASSERT_NE(nullptr, hm);
  EXPECT_EQ(131U, hm->alloc);

  // Populate 90 entries and delete 10 so rehash encounters DELETED + ACTIVE.
  for (size_t i = 0; i < 90; ++i) {
    const std::string key = absl::StrFormat("item_%zu", i);
    ASSERT_EQ(1, NC_hashmapadd(hm.get(), 1000 + i, key.data(), key.size()));
  }
  for (size_t i = 0; i < 10; ++i) {
    const std::string key = absl::StrFormat("item_%zu", i);
    ASSERT_EQ(1, NC_hashmapremove(hm.get(), key.data(), key.size(), nullptr));
  }
  EXPECT_EQ(80U, NC_hashmapcount(hm.get()));

  // Adding up to item_108 pushes active past 98 and triggers rehash at 99.
  for (size_t i = 90; i < 600; ++i) {
    const std::string key = absl::StrFormat("item_%zu", i);
    ASSERT_EQ(1, NC_hashmapadd(hm.get(), 1000 + i, key.data(), key.size()));
  }
  EXPECT_GT(hm->alloc, 500U);
  EXPECT_EQ(590U, NC_hashmapcount(hm.get()));

  for (size_t i = 0; i < 10; ++i) {
    const std::string key = absl::StrFormat("item_%zu", i);
    uintptr_t val = 0;
    EXPECT_EQ(0, NC_hashmapget(hm.get(), key.data(), key.size(), &val));
  }
  for (size_t i = 10; i < 600; ++i) {
    const std::string key = absl::StrFormat("item_%zu", i);
    uintptr_t val = 0;
    ASSERT_EQ(1, NC_hashmapget(hm.get(), key.data(), key.size(), &val));
    EXPECT_EQ(1000 + i, val);
  }
}

TEST(NchashmapTest, DeactivateRemovesMatchingDataEntriesOnly) {
  ScopedHashmap hm(NC_hashmapnew(0));
  ASSERT_NE(nullptr, hm);

  ASSERT_EQ(1, NC_hashmapadd(hm.get(), 10, "a", 1));
  ASSERT_EQ(1, NC_hashmapadd(hm.get(), 20, "b", 1));
  ASSERT_EQ(1, NC_hashmapadd(hm.get(), 20, "c", 1));
  ASSERT_EQ(1, NC_hashmapadd(hm.get(), 30, "d", 1));

  EXPECT_EQ(0, NC_hashmapdeactivate(hm.get(), 999));
  EXPECT_EQ(4U, NC_hashmapcount(hm.get()));

  EXPECT_EQ(1, NC_hashmapdeactivate(hm.get(), 20));
  EXPECT_EQ(1, NC_hashmapdeactivate(hm.get(), 20));
  EXPECT_EQ(0, NC_hashmapdeactivate(hm.get(), 20));
  EXPECT_EQ(2U, NC_hashmapcount(hm.get()));

  uintptr_t val = 0;
  EXPECT_EQ(0, NC_hashmapget(hm.get(), "b", 1, &val));
  EXPECT_EQ(0, NC_hashmapget(hm.get(), "c", 1, &val));
  EXPECT_EQ(1, NC_hashmapget(hm.get(), "a", 1, &val));
  EXPECT_EQ(10U, val);
  EXPECT_EQ(1, NC_hashmapget(hm.get(), "d", 1, &val));
  EXPECT_EQ(30U, val);
}

TEST(NchashmapTest, DebugPrintHelpersReportStatsEntriesAndMalformedGuard) {
  constexpr std::string_view kArgs[] = {"nchashmap_print"};
  const CliResult res = RunCliInForkedChild(
      [](int /*argc*/, char** /*argv*/) -> int {
        printhashmap(nullptr);

        ScopedHashmap hm(NC_hashmapnew(0));
        if (hm == nullptr) {
          return 1;
        }
        if (NC_hashmapadd(hm.get(), 1, "alpha", 5) != 1 ||
            NC_hashmapadd(hm.get(), 2, "beta", 4) != 1 ||
            NC_hashmapadd(hm.get(), 3, "gamma", 5) != 1 ||
            NC_hashmapremove(hm.get(), "beta", 4, nullptr) != 1) {
          return 2;
        }
        printhashmapstats(hm.get());
        printhashmap(hm.get());

        ScopedHashmap large(NC_hashmapnew(3500));
        if (large == nullptr) {
          return 3;
        }
        printhashmap(large.get());
        return 0;
      },
      kArgs);

  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code);
  EXPECT_THAT(res.stderr_output, ::testing::HasSubstr("NULL"));
  EXPECT_THAT(res.stderr_output,
              ::testing::HasSubstr("hashmap: alloc=131 active=2 maxchain="));
  EXPECT_THAT(res.stderr_output, ::testing::HasSubstr("flags=ACTIVE"));
  EXPECT_THAT(res.stderr_output, ::testing::HasSubstr("flags=DELETED"));
  EXPECT_THAT(res.stderr_output, ::testing::HasSubstr("flags=EMPTY"));
  EXPECT_THAT(res.stderr_output, ::testing::HasSubstr("..."));
  EXPECT_THAT(res.stderr_output, ::testing::HasSubstr("MALFORMED"));
}

}  // namespace
}  // namespace netcdf
