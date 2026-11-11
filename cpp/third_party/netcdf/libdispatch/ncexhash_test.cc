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

// Unit tests for //third_party/netcdf/libdispatch/ncexhash.c.
//
// Covers extendible hashing directory creation, binary search, leaf splits
// with and without directory doubling, key updates/removals, iterator guards,
// CRC64 key generation, split error unwind, and debug print helpers.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/ncexhash.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
// clang-format on

extern "C" {
#include "third_party/netcdf/include/ncexhash.h"
}  // extern "C"

namespace netcdf {
namespace {

struct NcExhashmapDeleter {
  void operator()(NCexhashmap* map) const { ncexhashmapfree(map); }
};
using ScopedExhashmap = std::unique_ptr<NCexhashmap, NcExhashmapDeleter>;

TEST(NcexhashTest, NewInqAndFreeHandleBoundaryInputs) {
  ncexhashmapfree(nullptr);
  EXPECT_EQ(0, ncexhashmaplength(static_cast<NCexhashmap*>(nullptr)));
  EXPECT_EQ(NC_EINVAL, ncexhashinqmap(nullptr, nullptr, nullptr, nullptr,
                                      nullptr, nullptr));

  // Requested leaf sizes < 2 clamp to MINLEAFLEN (2).
  for (const int req_len : {-4, 0, 1}) {
    SCOPED_TRACE(req_len);
    ScopedExhashmap map(ncexhashnew(req_len));
    ASSERT_NE(nullptr, map.get());

    int leaflen = 0;
    int depth = 0;
    int nactive = -1;
    int uid = 0;
    int walking = -1;
    EXPECT_EQ(NC_NOERR, ncexhashinqmap(map.get(), &leaflen, &depth, &nactive,
                                       &uid, &walking));
    EXPECT_EQ(2, leaflen);
    EXPECT_EQ(1, depth);
    EXPECT_EQ(0, nactive);
    EXPECT_EQ(2, uid);
    EXPECT_EQ(0, walking);
    EXPECT_EQ(0, ncexhashcount(map.get()));
    EXPECT_EQ(0, ncexhashmaplength(map.get()));
    EXPECT_EQ(NC_NOERR, ncexhashinqmap(map.get(), nullptr, nullptr, nullptr,
                                       nullptr, nullptr));
  }

  ScopedExhashmap larger(ncexhashnew(8));
  ASSERT_NE(nullptr, larger.get());
  int leaflen = 0;
  EXPECT_EQ(NC_NOERR, ncexhashinqmap(larger.get(), &leaflen, nullptr, nullptr,
                                     nullptr, nullptr));
  EXPECT_EQ(8, leaflen);
}

TEST(NcexhashTest, PutGetUpdateAndBinarySearchBoundaries) {
  ScopedExhashmap map(ncexhashnew(8));
  ASSERT_NE(nullptr, map.get());

  uintptr_t out = 999;
  // Lookup on empty leaf (n == 0).
  EXPECT_EQ(NC_ENOTFOUND, ncexhashget(map.get(), 0x10ULL << 56, &out));

  // Insert keys in non-monotonic order into the MSB=0 leaf so exhashnewentry
  // hits both tail-append and entry-shift paths and exbinsearch hits both odd
  // and even midpoint branches.
  constexpr std::array<ncexhashkey_t, 7> kKeys = {
      0x40ULL << 56, 0x20ULL << 56, 0x60ULL << 56, 0x10ULL << 56,
      0x30ULL << 56, 0x50ULL << 56, 0x70ULL << 56,
  };
  for (size_t i = 0; i < kKeys.size(); ++i) {
    ASSERT_EQ(NC_NOERR,
              ncexhashput(map.get(), kKeys[i], static_cast<uintptr_t>(i + 10)));
  }
  EXPECT_EQ(7, ncexhashcount(map.get()));

  for (size_t i = 0; i < kKeys.size(); ++i) {
    out = 0;
    EXPECT_EQ(NC_NOERR, ncexhashget(map.get(), kKeys[i], &out));
    EXPECT_EQ(static_cast<uintptr_t>(i + 10), out);
    EXPECT_EQ(NC_NOERR, ncexhashget(map.get(), kKeys[i], nullptr));
  }

  // Misses smaller than min, between elements, and larger than max.
  EXPECT_EQ(NC_ENOTFOUND, ncexhashget(map.get(), 0x05ULL << 56, &out));
  EXPECT_EQ(NC_ENOTFOUND, ncexhashget(map.get(), 0x25ULL << 56, &out));
  EXPECT_EQ(NC_ENOTFOUND, ncexhashget(map.get(), 0x75ULL << 56, &out));

  // Overwriting an existing key via ncexhashput updates data without adding.
  EXPECT_EQ(NC_NOERR, ncexhashput(map.get(), kKeys[3], 4242));
  EXPECT_EQ(7, ncexhashcount(map.get()));
  EXPECT_EQ(NC_NOERR, ncexhashget(map.get(), kKeys[3], &out));
  EXPECT_EQ(uintptr_t{4242}, out);
}

TEST(NcexhashTest, SplitsAndDirectoryDoublingHandleDensePrefixesAndManyKeys) {
  ScopedExhashmap map(ncexhashnew(2));
  ASSERT_NE(nullptr, map.get());

  // Keys sharing top bits force repeated directory doubling (leaf->depth >
  // map->depth) before diverging.
  constexpr std::array<ncexhashkey_t, 4> kDenseZeroPrefix = {
      0x00ULL << 56,
      0x01ULL << 56,
      0x02ULL << 56,
      0x10ULL << 56,
  };
  for (size_t i = 0; i < kDenseZeroPrefix.size(); ++i) {
    ASSERT_EQ(NC_NOERR, ncexhashput(map.get(), kDenseZeroPrefix[i], i + 1));
  }

  int depth = 0;
  ASSERT_EQ(NC_NOERR, ncexhashinqmap(map.get(), nullptr, &depth, nullptr,
                                     nullptr, nullptr));
  ASSERT_EQ(7, depth);

  // Keys in the upper half split a leaf whose local depth is still below
  // map->depth (leaf->depth <= map->depth, no directory doubling). Because
  // exhashsplit() assigns directory entries with (i % 2 == 1) to the new leaf,
  // at map->depth == 7 the two existing keys must differ at bit (64 - 7) == 57
  // (0x80ULL << 56 vs. 0x82ULL << 56) so they separate without doubling.
  const int depth_before = depth;
  ASSERT_EQ(NC_NOERR, ncexhashput(map.get(), 0x80ULL << 56, 101));
  ASSERT_EQ(NC_NOERR, ncexhashput(map.get(), 0x82ULL << 56, 102));
  ASSERT_EQ(NC_NOERR, ncexhashput(map.get(), 0xA0ULL << 56, 103));
  ASSERT_EQ(NC_NOERR, ncexhashinqmap(map.get(), nullptr, &depth, nullptr,
                                     nullptr, nullptr));
  EXPECT_EQ(depth_before, depth);

  std::map<ncexhashkey_t, uintptr_t> expected;
  for (size_t i = 0; i < kDenseZeroPrefix.size(); ++i) {
    expected[kDenseZeroPrefix[i]] = i + 1;
  }
  expected[0x80ULL << 56] = 101;
  expected[0x82ULL << 56] = 102;
  expected[0xA0ULL << 56] = 103;

  EXPECT_EQ(static_cast<int>(expected.size()), ncexhashcount(map.get()));
  for (const auto& [hk, val] : expected) {
    uintptr_t got = 0;
    EXPECT_EQ(NC_NOERR, ncexhashget(map.get(), hk, &got));
    EXPECT_EQ(val, got);
  }

  // Insert 64 CRC64-hashed keys into a dedicated map (keeping synthetic
  // all-zero-tail keys separate so hashed keys never hit an unsplitable pair)
  // and verify every lookup succeeds.
  ScopedExhashmap hashed_map(ncexhashnew(4));
  ASSERT_NE(nullptr, hashed_map.get());
  std::map<ncexhashkey_t, uintptr_t> hashed_expected;
  for (int i = 0; i < 64; ++i) {
    const std::string key = "exhash_item_" + std::to_string(i);
    const ncexhashkey_t hk = ncexhashkey(
        reinterpret_cast<const unsigned char*>(key.data()), key.size());
    const uintptr_t val = static_cast<uintptr_t>(1000 + i);
    ASSERT_EQ(NC_NOERR, ncexhashput(hashed_map.get(), hk, val));
    hashed_expected[hk] = val;
  }

  EXPECT_EQ(static_cast<int>(hashed_expected.size()),
            ncexhashcount(hashed_map.get()));
  for (const auto& [hk, val] : hashed_expected) {
    uintptr_t got = 0;
    EXPECT_EQ(NC_NOERR, ncexhashget(hashed_map.get(), hk, &got));
    EXPECT_EQ(val, got);
  }
}

TEST(NcexhashTest, SetDataAndRemoveUpdateAndCompactLeaves) {
  ScopedExhashmap map(ncexhashnew(4));
  ASSERT_NE(nullptr, map.get());

  uintptr_t old_val = 0;
  EXPECT_EQ(NC_ENOTFOUND,
            ncexhashsetdata(map.get(), 0x10ULL << 56, 5, &old_val));
  EXPECT_EQ(NC_ENOTFOUND, ncexhashremove(map.get(), 0x10ULL << 56, &old_val));

  constexpr std::array<ncexhashkey_t, 4> kKeys = {
      0x10ULL << 56,
      0x20ULL << 56,
      0x30ULL << 56,
      0x40ULL << 56,
  };
  for (size_t i = 0; i < kKeys.size(); ++i) {
    ASSERT_EQ(NC_NOERR, ncexhashput(map.get(), kKeys[i], i + 100));
  }

  // Update data with non-null and null olddatap.
  EXPECT_EQ(NC_NOERR, ncexhashsetdata(map.get(), kKeys[1], 777, &old_val));
  EXPECT_EQ(uintptr_t{101}, old_val);
  EXPECT_EQ(NC_NOERR, ncexhashsetdata(map.get(), kKeys[1], 888, nullptr));
  uintptr_t cur = 0;
  EXPECT_EQ(NC_NOERR, ncexhashget(map.get(), kKeys[1], &cur));
  EXPECT_EQ(uintptr_t{888}, cur);

  // Remove first, middle, and last entry; also test datap == nullptr.
  uintptr_t removed = 0;
  EXPECT_EQ(NC_NOERR, ncexhashremove(map.get(), kKeys[0], &removed));
  EXPECT_EQ(uintptr_t{100}, removed);
  EXPECT_EQ(NC_NOERR, ncexhashremove(map.get(), kKeys[2], nullptr));
  EXPECT_EQ(NC_NOERR, ncexhashremove(map.get(), kKeys[3], &removed));
  EXPECT_EQ(uintptr_t{103}, removed);
  EXPECT_EQ(1, ncexhashcount(map.get()));

  EXPECT_EQ(NC_ENOTFOUND, ncexhashget(map.get(), kKeys[0], &cur));
  EXPECT_EQ(NC_NOERR, ncexhashget(map.get(), kKeys[1], &cur));
  EXPECT_EQ(uintptr_t{888}, cur);
}

TEST(NcexhashTest, IterateWalksEntriesAndBlocksConcurrentMutations) {
  ScopedExhashmap map(ncexhashnew(2));
  ASSERT_NE(nullptr, map.get());

  // Empty map iteration returns NC_ERANGE immediately and clears walking state.
  ncexhashkey_t k = 0;
  uintptr_t v = 0;
  EXPECT_EQ(NC_ERANGE, ncexhashiterate(map.get(), &k, &v));

  std::map<ncexhashkey_t, uintptr_t> expected;
  for (int i = 0; i < 10; ++i) {
    const ncexhashkey_t hk = static_cast<ncexhashkey_t>(i) << 59;
    ASSERT_EQ(NC_NOERR, ncexhashput(map.get(), hk, static_cast<uintptr_t>(i)));
    expected[hk] = static_cast<uintptr_t>(i);
  }

  std::map<ncexhashkey_t, uintptr_t> visited;
  bool checked_walking_guards = false;
  while (true) {
    const int stat = ncexhashiterate(map.get(), &k, &v);
    if (stat == NC_ERANGE) {
      break;
    }
    ASSERT_EQ(NC_NOERR, stat);
    visited[k] = v;

    if (!checked_walking_guards) {
      checked_walking_guards = true;
      int walking = 0;
      EXPECT_EQ(NC_NOERR, ncexhashinqmap(map.get(), nullptr, nullptr, nullptr,
                                         nullptr, &walking));
      EXPECT_EQ(1, walking);
      EXPECT_EQ(NC_EPERM, ncexhashput(map.get(), 1ULL, 1));
      EXPECT_EQ(NC_EPERM, ncexhashremove(map.get(), k, nullptr));
      EXPECT_EQ(NC_EPERM, ncexhashsetdata(map.get(), k, 999, nullptr));
      // Exercise walking branch inside ncexhashprint().
      ncexhashprint(map.get());
    }
  }
  EXPECT_EQ(expected, visited);

  int walking = -1;
  EXPECT_EQ(NC_NOERR, ncexhashinqmap(map.get(), nullptr, nullptr, nullptr,
                                     nullptr, &walking));
  EXPECT_EQ(0, walking);

  // Iterate with null out-pointers as well.
  int count = 0;
  while (ncexhashiterate(map.get(), nullptr, nullptr) == NC_NOERR) {
    ++count;
  }
  EXPECT_EQ(10, count);
}

TEST(NcexhashTest, HashKeyComputesDeterministicCrc64) {
  constexpr char kAlpha[] = "alpha";
  constexpr char kBeta[] = "beta";
  const ncexhashkey_t h_alpha1 = ncexhashkey(
      reinterpret_cast<const unsigned char*>(kAlpha), std::strlen(kAlpha));
  const ncexhashkey_t h_alpha2 = ncexhashkey(
      reinterpret_cast<const unsigned char*>(kAlpha), std::strlen(kAlpha));
  const ncexhashkey_t h_beta = ncexhashkey(
      reinterpret_cast<const unsigned char*>(kBeta), std::strlen(kBeta));
  const ncexhashkey_t h_empty =
      ncexhashkey(reinterpret_cast<const unsigned char*>(""), 0);

  EXPECT_EQ(h_alpha1, h_alpha2);
  EXPECT_NE(h_alpha1, h_beta);
  EXPECT_EQ(0ULL, h_empty);
}

TEST(NcexhashTest, SplitDetectsCorruptDuplicateKeyAndUnwindsCleanly) {
  ScopedExhashmap map(ncexhashnew(2));
  ASSERT_NE(nullptr, map.get());

  // Seed a single entry in directory[0] and simulate a duplicate hashkey in
  // the leaf to exercise the NC_EINTERNAL detection and unwind path in
  // exhashsplit().
  ASSERT_EQ(NC_NOERR, ncexhashput(map.get(), 0ULL, 11));
  NCexleaf* leaf0 = map->directory[0];
  ASSERT_NE(nullptr, leaf0);
  ASSERT_EQ(1, leaf0->active);
  leaf0->entries[1] = leaf0->entries[0];
  leaf0->active = 2;
  map->nactive = 2;

  EXPECT_EQ(NC_EINTERNAL, ncexhashput(map.get(), 1ULL << 60, 22));
}

TEST(NcexhashTest, DebugPrintAndStatsHelpersRunCleanly) {
  ncexhashprint(nullptr);
  EXPECT_STREQ("", ncexbinstr(0ULL, 0));
  EXPECT_STREQ("1010", ncexbinstr(0xAULL, 4));

  ScopedExhashmap map(ncexhashnew(2));
  ASSERT_NE(nullptr, map.get());
  // Print empty map first to cover zero-entry leaf formatting.
  ncexhashprint(map.get());

  // Split both initial leaves so every active leaf has depth >= 1 (otherwise
  // ncexhashprint / ncexhashprintleaf calls MSB(hkey, 0) which shifts by 64).
  ASSERT_EQ(NC_NOERR, ncexhashput(map.get(), 0x00ULL << 56, 1));
  ASSERT_EQ(NC_NOERR, ncexhashput(map.get(), 0x40ULL << 56, 2));
  ASSERT_EQ(NC_NOERR, ncexhashput(map.get(), 0x60ULL << 56, 3));
  ASSERT_EQ(NC_NOERR, ncexhashput(map.get(), 0x80ULL << 56, 4));
  ASSERT_EQ(NC_NOERR, ncexhashput(map.get(), 0xC0ULL << 56, 5));
  ASSERT_EQ(NC_NOERR, ncexhashput(map.get(), 0xE0ULL << 56, 6));

  ncexhashprint(map.get());
  ncexhashprintdir(map.get(), map->directory);

  // Locate a leaf with at least 2 entries to exercise index == 0 and index > 0.
  NCexleaf* target_leaf = map->leaves;
  for (NCexleaf* leaf = map->leaves; leaf != nullptr; leaf = leaf->next) {
    if (leaf->active >= 2) {
      target_leaf = leaf;
      break;
    }
  }
  ASSERT_NE(nullptr, target_leaf);
  ASSERT_GE(target_leaf->active, 1);
  ncexhashprintleaf(map.get(), target_leaf);
  ncexhashprintentry(map.get(), &target_leaf->entries[0]);

  ncexhashprintstats(map.get());

  // Temporarily skew nactive to cover mismatch warning branch, then restore.
  const int saved_nactive = map->nactive;
  map->nactive = saved_nactive + 1;
  ncexhashprintstats(map.get());
  map->nactive = saved_nactive;
}

}  // namespace
}  // namespace netcdf
