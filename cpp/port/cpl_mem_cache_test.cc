// Test LRUCache11
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_mem_cache.h
//
// Copyright 2024 Google Inc. All Rights Reserved.
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

#include <features.h>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/port/cpl_mem_cache.h"

namespace autotest2 {
namespace {

TEST(LruCache11, InitializationAndProperties) {
  lru11::Cache<int, int> cache(2, 1);

  EXPECT_EQ(0, cache.size());
  EXPECT_TRUE(cache.empty());
  EXPECT_EQ(2, cache.getMaxSize());
  EXPECT_EQ(1, cache.getElasticity());
  EXPECT_EQ(3, cache.getMaxAllowedSize());
}

TEST(LruCache11, SuccessfulRetrieval) {
  lru11::Cache<int, int> cache(2, 1);

  cache.insert(0, 1);

  EXPECT_EQ(1, cache.size());
  EXPECT_FALSE(cache.empty());
  EXPECT_TRUE(cache.contains(0));

  int val;
  EXPECT_TRUE(cache.tryGet(0, val));
  EXPECT_EQ(1, val);

  EXPECT_EQ(1, cache.get(0));
  EXPECT_EQ(1, cache.getCopy(0));
}

TEST(LruCache11, SuccessfulRetrievalByPointer) {
  lru11::Cache<int, int> cache(2, 1);

  cache.insert(0, 1);

  int *ptr = cache.getPtr(0);
  ASSERT_NE(nullptr, ptr);
  EXPECT_EQ(1, *ptr);
}

TEST(LruCache11, TryGetReturnsFalseWhenKeyMissing) {
  lru11::Cache<int, int> cache(2, 1);

  int val;

  EXPECT_FALSE(cache.tryGet(0, val));
}

TEST(LruCache11, GetPtrReturnsNullptrWhenKeyMissing) {
  lru11::Cache<int, int> cache(2, 1);

  EXPECT_EQ(nullptr, cache.getPtr(0));
}

TEST(LruCache11, GetThrowsExceptionWhenKeyMissing) {
  lru11::Cache<int, int> cache(2, 1);

  EXPECT_THROW({ cache.get(0); }, lru11::KeyNotFound);
}

TEST(LruCache11, RemoveReturnsFalseWhenKeyMissing) {
  lru11::Cache<int, int> cache(2, 1);

  EXPECT_FALSE(cache.remove(0));
}

TEST(LruCache11, ContainsReturnsFalseWhenKeyMissing) {
  lru11::Cache<int, int> cache(2, 1);

  EXPECT_FALSE(cache.contains(0));
}

TEST(LruCache11, RemoveAndRecycleOldestEntryReturnsFalseWhenKeyMissing) {
  lru11::Cache<int, int> cache(2, 1);

  int out;
  EXPECT_FALSE(cache.removeAndRecycleOldestEntry(out));
}

TEST(LruCache11, Cwalk) {
  lru11::Cache<int, int> cache(2, 1);

  cache.insert(0, 1);

  bool visited = false;

  auto lambda = [&visited](const lru11::KeyValuePair<int, int>&kv) {
    if (kv.key == 0 && kv.value == 1) {
      visited = true;
    }
  };
  cache.cwalk(lambda);

  EXPECT_TRUE(visited);
}

TEST(LruCache11, RemoveAndRecycleOldestEntry) {
  lru11::Cache<int, int> cache(2, 1);

  cache.insert(0, 1);
  cache.insert(1, 2);

  int out = -1;
  EXPECT_TRUE(cache.removeAndRecycleOldestEntry(out));
  EXPECT_EQ(1, out);
}

TEST(LruCache11, OverwritingValues) {
  lru11::Cache<int, int> cache(2, 1);

  cache.insert(0, 1);
  cache.insert(0, 2);

  EXPECT_EQ(2, cache.get(0));
  EXPECT_EQ(1, cache.size());
}

TEST(LruCache11, Size) {
  lru11::Cache<int, int> cache(2, 1);

  cache.insert(0, 1);
  cache.insert(1, 1);
  cache.insert(2, 1);

  EXPECT_EQ(3, cache.size());
}

TEST(LruCache11, Remove) {
  lru11::Cache<int, int> cache(2, 1);

  cache.insert(0, 1);
  cache.insert(1, 1);

  EXPECT_EQ(2, cache.size());
  EXPECT_TRUE(cache.remove(0));
  EXPECT_EQ(1, cache.size());
  EXPECT_FALSE(cache.contains(0));
}

TEST(LruCache11, OldestEntryDropsOutOfCache) {
  lru11::Cache<int, int> cache(2, 0);

  cache.insert(0, 1);
  cache.insert(1, 1);
  cache.insert(2, 1);

  EXPECT_EQ(2, cache.size());
  EXPECT_FALSE(cache.contains(0));
}

TEST(LruCache11, OldestEntryDropsOutOfCacheWithElasticity) {
  lru11::Cache<int, int> cache(2, 1);

  cache.insert(0, 1);
  cache.insert(1, 1);
  cache.insert(2, 1);

  // Still within elasticity limit, no evictions yet
  EXPECT_EQ(3, cache.size());
  EXPECT_TRUE(cache.contains(0));
  EXPECT_TRUE(cache.contains(1));
  EXPECT_TRUE(cache.contains(2));

  // Hit elasticity limit, starts to evict
  cache.insert(3, 1);

  EXPECT_EQ(2, cache.size());
  EXPECT_FALSE(cache.contains(0));
  EXPECT_FALSE(cache.contains(1));
  EXPECT_TRUE(cache.contains(2));
  EXPECT_TRUE(cache.contains(3));
}

TEST(LruCache11, CacheDoesNotUseCopyConstructorCopyAssignment) {
  struct NoCopyObj {
    int test_v;

    NoCopyObj(int v): test_v(v) {};

    NoCopyObj(const NoCopyObj&) : test_v(-1) {
      ADD_FAILURE();
    };

    NoCopyObj& operator=(const NoCopyObj&) {
      ADD_FAILURE();
      return *this;
    }

    NoCopyObj(NoCopyObj&&) = default;
    NoCopyObj& operator=(NoCopyObj&&) = default;
  };

  lru11::Cache<int, NoCopyObj> cache(2, 0);

  EXPECT_EQ(0, cache.insert(0, NoCopyObj(0)).test_v);

  {
    NoCopyObj *ptr = cache.getPtr(0);
    EXPECT_EQ(0, ptr->test_v);
  }

  EXPECT_EQ(1, cache.insert(1, NoCopyObj(1)).test_v);
  EXPECT_EQ(2, cache.insert(2, NoCopyObj(2)).test_v);

  {
    NoCopyObj out(-1);
    cache.removeAndRecycleOldestEntry(out);
    EXPECT_EQ(1, out.test_v);
  }
}

}  // namespace
}  // namespace autotest2
