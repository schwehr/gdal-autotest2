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

// Unit tests for //third_party/netcdf/libdispatch/ncxcache.c.
//
// Exercises cache creation/destruction (default and custom leaf sizes),
// hash-key derivation, insertion, lookup, LRU touch/eviction ordering, removal,
// error paths (null cache, missing keys, put failure during map iteration),
// and ncxcacheprint debug formatting inside a forked child.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/ncxcache.c

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/temp_file.h"

extern "C" {
#include "third_party/netcdf/include/ncexhash.h"
#include "third_party/netcdf/include/ncxcache.h"
}  // extern "C"

namespace netcdf {
namespace {

struct NcXcacheDeleter {
  void operator()(NCxcache* cache) const { ncxcachefree(cache); }
};

using ScopedNcXcache = std::unique_ptr<NCxcache, NcXcacheDeleter>;

// NCXUSER is enabled in ncxcache.h, so NCxnode must be the first member of
// cached objects.
struct CacheEntry {
  NCxnode node{};
  int id = 0;
};

static_assert(offsetof(CacheEntry, node) == 0,
              "NCxnode must be at offset 0 under NCXUSER");

TEST(NcxcacheTest, NullCacheAndEmptyCacheBehavior) {
  ncxcachefree(nullptr);
  EXPECT_EQ(NC_NOERR, ncxcachenew(0, nullptr));

  EXPECT_EQ(0, ncxcachecount(static_cast<NCxcache*>(nullptr)));
  EXPECT_EQ(nullptr, ncxcachefirst(nullptr));
  EXPECT_EQ(nullptr, ncxcachelast(nullptr));

  void* out = nullptr;
  CacheEntry dummy{};
  EXPECT_EQ(NC_EINVAL, ncxcachelookup(nullptr, 1ULL, &out));
  EXPECT_EQ(NC_EINVAL, ncxcachetouch(nullptr, 1ULL));
  EXPECT_EQ(NC_EINVAL, ncxcacheinsert(nullptr, 1ULL, &dummy));
  EXPECT_EQ(NC_EINVAL, ncxcacheremove(nullptr, 1ULL, &out));

  NCxcache* raw_cache = nullptr;
  ASSERT_EQ(NC_NOERR, ncxcachenew(0, &raw_cache));
  ScopedNcXcache cache(raw_cache);
  ASSERT_NE(nullptr, cache.get());

  EXPECT_EQ(0, ncxcachecount(cache.get()));
  EXPECT_EQ(nullptr, ncxcachefirst(cache.get()));
  EXPECT_EQ(nullptr, ncxcachelast(cache.get()));
  EXPECT_EQ(NC_ENOOBJECT, ncxcachelookup(cache.get(), 42ULL, &out));
  EXPECT_EQ(NC_ENOOBJECT, ncxcachetouch(cache.get(), 42ULL));
  EXPECT_EQ(NC_ENOOBJECT, ncxcacheremove(cache.get(), 42ULL, &out));
}

TEST(NcxcacheTest, KeyGenerationIsDeterministicAndLengthSensitive) {
  constexpr char kAlpha[] = "dataset_alpha";
  constexpr char kBeta[] = "dataset_beta";
  const ncexhashkey_t h1 = ncxcachekey(kAlpha, sizeof(kAlpha) - 1);
  const ncexhashkey_t h2 = ncxcachekey(kAlpha, sizeof(kAlpha) - 1);
  const ncexhashkey_t h3 = ncxcachekey(kAlpha, sizeof(kAlpha) - 2);
  const ncexhashkey_t h4 = ncxcachekey(kBeta, sizeof(kBeta) - 1);

  EXPECT_EQ(h1, h2);
  EXPECT_NE(h1, h3);
  EXPECT_NE(h1, h4);
}

TEST(NcxcacheTest, InsertLookupTouchAndRemoveMaintainLruOrder) {
  CacheEntry a{.id = 1};
  CacheEntry b{.id = 2};
  CacheEntry c{.id = 3};
  NCxcache* raw_cache = nullptr;
  ASSERT_EQ(NC_NOERR, ncxcachenew(4, &raw_cache));
  ScopedNcXcache cache(raw_cache);
  ASSERT_NE(nullptr, cache.get());

  const ncexhashkey_t ka = ncxcachekey("a", 1);
  const ncexhashkey_t kb = ncxcachekey("b", 1);
  const ncexhashkey_t kc = ncxcachekey("c", 1);

  ASSERT_EQ(NC_NOERR, ncxcacheinsert(cache.get(), ka, &a));
  EXPECT_EQ(1, ncxcachecount(cache.get()));
  EXPECT_EQ(&a, ncxcachefirst(cache.get()));
  EXPECT_EQ(&a, ncxcachelast(cache.get()));

  ASSERT_EQ(NC_NOERR, ncxcacheinsert(cache.get(), kb, &b));
  ASSERT_EQ(NC_NOERR, ncxcacheinsert(cache.get(), kc, &c));
  EXPECT_EQ(3, ncxcachecount(cache.get()));
  // LRU order from first to last: c -> b -> a.
  EXPECT_EQ(&c, ncxcachefirst(cache.get()));
  EXPECT_EQ(&a, ncxcachelast(cache.get()));

  void* looked_up = nullptr;
  ASSERT_EQ(NC_NOERR, ncxcachelookup(cache.get(), kb, &looked_up));
  EXPECT_EQ(&b, looked_up);
  EXPECT_EQ(NC_NOERR, ncxcachelookup(cache.get(), kb, nullptr));

  // Touch tail element 'a' -> order becomes a -> c -> b.
  ASSERT_EQ(NC_NOERR, ncxcachetouch(cache.get(), ka));
  EXPECT_EQ(&a, ncxcachefirst(cache.get()));
  EXPECT_EQ(&b, ncxcachelast(cache.get()));

  // Touching already-first element preserves order.
  ASSERT_EQ(NC_NOERR, ncxcachetouch(cache.get(), ka));
  EXPECT_EQ(&a, ncxcachefirst(cache.get()));
  EXPECT_EQ(&b, ncxcachelast(cache.get()));

  // Touch middle element 'c' -> order becomes c -> a -> b.
  ASSERT_EQ(NC_NOERR, ncxcachetouch(cache.get(), kc));
  EXPECT_EQ(&c, ncxcachefirst(cache.get()));
  EXPECT_EQ(&b, ncxcachelast(cache.get()));

  // Remove middle 'a', then first 'c', then last 'b'.
  void* removed = nullptr;
  ASSERT_EQ(NC_NOERR, ncxcacheremove(cache.get(), ka, &removed));
  EXPECT_EQ(&a, removed);
  EXPECT_EQ(2, ncxcachecount(cache.get()));
  EXPECT_EQ(&c, ncxcachefirst(cache.get()));
  EXPECT_EQ(&b, ncxcachelast(cache.get()));

  ASSERT_EQ(NC_NOERR, ncxcacheremove(cache.get(), kc, nullptr));
  EXPECT_EQ(1, ncxcachecount(cache.get()));
  EXPECT_EQ(&b, ncxcachefirst(cache.get()));
  EXPECT_EQ(&b, ncxcachelast(cache.get()));

  ASSERT_EQ(NC_NOERR, ncxcacheremove(cache.get(), kb, &removed));
  EXPECT_EQ(&b, removed);
  EXPECT_EQ(0, ncxcachecount(cache.get()));
  EXPECT_EQ(nullptr, ncxcachefirst(cache.get()));
  EXPECT_EQ(nullptr, ncxcachelast(cache.get()));
  EXPECT_EQ(NC_ENOOBJECT, ncxcachelookup(cache.get(), kb, &looked_up));
}

TEST(NcxcacheTest, HandlesManyInsertionsAndMapPutFailureDuringIteration) {
  constexpr int kCount = 24;
  std::vector<CacheEntry> entries(kCount);
  std::vector<ncexhashkey_t> keys(kCount);
  NCxcache* raw_cache = nullptr;
  ASSERT_EQ(NC_NOERR, ncxcachenew(2, &raw_cache));
  ScopedNcXcache cache(raw_cache);
  ASSERT_NE(nullptr, cache.get());

  for (int i = 0; i < kCount; ++i) {
    entries[i].id = i;
    const std::string name = "entry_" + std::to_string(i);
    keys[i] = ncxcachekey(name.data(), name.size());
    ASSERT_EQ(NC_NOERR, ncxcacheinsert(cache.get(), keys[i], &entries[i]));
  }
  EXPECT_EQ(kCount, ncxcachecount(cache.get()));
  EXPECT_EQ(&entries.back(), ncxcachefirst(cache.get()));
  EXPECT_EQ(&entries.front(), ncxcachelast(cache.get()));

  for (int i = 0; i < kCount; ++i) {
    void* out = nullptr;
    ASSERT_EQ(NC_NOERR, ncxcachelookup(cache.get(), keys[i], &out));
    EXPECT_EQ(&entries[i], out);
  }

  // Starting an iteration on cache->map causes ncexhashput to return NC_EPERM,
  // exercising ncxcacheinsert's error exit path cleanly.
  ncexhashkey_t iter_key = 0;
  uintptr_t iter_val = 0;
  ASSERT_EQ(NC_NOERR, ncexhashiterate(cache->map, &iter_key, &iter_val));

  CacheEntry blocked{.id = 999};
  const ncexhashkey_t blocked_key = ncxcachekey("blocked", 7);
  EXPECT_EQ(NC_EPERM, ncxcacheinsert(cache.get(), blocked_key, &blocked));
  EXPECT_EQ(kCount, ncxcachecount(cache.get()));

  // Drain iterator back to idle before lookup/destruction.
  while (ncexhashiterate(cache->map, &iter_key, &iter_val) == NC_NOERR) {
  }
  EXPECT_EQ(NC_ENOOBJECT, ncxcachelookup(cache.get(), blocked_key, nullptr));
}

TEST(NcxcacheTest, PrintFormatsEmptyAndMultiEntryCaches) {
  TempFile print_log("ncxcache_print.log");
  ASSERT_TRUE(RunInForkedChild([&]() -> bool {
    if (std::freopen(print_log.c_str(), "w", stderr) == nullptr) {
      return false;
    }
    // Split both initial leaves so every active leaf has depth >= 1 (otherwise
    // ncexhashprint inside ncxcacheprint calls MSB(hkey, 0), which shifts by
    // 64).
    std::vector<CacheEntry> entries(6);
    constexpr ncexhashkey_t kKeys[6] = {
        0x00ULL << 56, 0x40ULL << 56, 0x60ULL << 56,
        0x80ULL << 56, 0xC0ULL << 56, 0xE0ULL << 56,
    };
    NCxcache* raw = nullptr;
    if (ncxcachenew(2, &raw) != NC_NOERR || raw == nullptr) {
      return false;
    }
    ScopedNcXcache cache(raw);
    ncxcacheprint(cache.get());

    for (int i = 0; i < 6; ++i) {
      entries[i].id = i + 1;
      if (ncxcacheinsert(cache.get(), kKeys[i], &entries[i]) != NC_NOERR) {
        return false;
      }
    }
    ncxcacheprint(cache.get());
    std::fflush(stderr);
    return true;
  }));

  std::ifstream in(print_log.path());
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string printed = ss.str();
  EXPECT_THAT(printed, ::testing::HasSubstr("NCxcache: lru={}\n"));
  EXPECT_THAT(printed, ::testing::HasSubstr(","));
}

}  // namespace
}  // namespace netcdf
