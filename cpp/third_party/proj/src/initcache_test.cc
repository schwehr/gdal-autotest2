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
// Unit tests for PROJ init-file parameter list cache and paralist cloning
// (proj/src/initcache.cpp). Note: initcache uses process-global state protected
// by pj_acquire_lock(), so tests explicitly clear the cache via absl::Cleanup
// to stay order-independent.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/initcache.cpp

#include <initializer_list>
#include <memory>
#include <string>
#include <thread>  // NOLINT
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/in_memory_file_api.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

struct ParalistDeleter {
  PJ_CONTEXT* ctx;
  void operator()(paralist* p) const noexcept {
    if (p != nullptr) {
      free_params(ctx, p, 0);
    }
  }
};

using ScopedParalist = std::unique_ptr<paralist, ParalistDeleter>;

ScopedParalist MakeParalist(PJ_CONTEXT* ctx,
                            std::initializer_list<const char*> items) {
  paralist* head = nullptr;
  paralist* tail = nullptr;
  for (const char* item : items) {
    paralist* node = pj_mkparam(item);
    if (node == nullptr) {
      free_params(ctx, head, 0);
      return ScopedParalist(nullptr, ParalistDeleter{ctx});
    }
    if (head == nullptr) {
      head = node;
    } else {
      tail->next = node;
    }
    tail = node;
  }
  return ScopedParalist(head, ParalistDeleter{ctx});
}

TEST(InitcacheTest, ClearWhenEmptyIsSafeNoOp) {
  pj_clear_initcache();
  absl::Cleanup clear_cache = [] { pj_clear_initcache(); };
  // Second clear exercises cache_alloc == 0 early exit.
  pj_clear_initcache();

  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedParalist missing(pj_search_initcache("missing:key"),
                         ParalistDeleter{ctx.get()});
  EXPECT_EQ(nullptr, missing.get());
}

TEST(InitcacheTest, CloneParalistHandlesNullSingleAndMultiNodeLists) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedParalist null_clone(pj_clone_paralist(nullptr),
                            ParalistDeleter{ctx.get()});
  EXPECT_EQ(nullptr, null_clone.get());

  ScopedParalist single = MakeParalist(ctx.get(), {"+proj=merc"});
  ASSERT_NE(nullptr, single.get());
  single->used = 1;
  ScopedParalist single_clone(pj_clone_paralist(single.get()),
                              ParalistDeleter{ctx.get()});
  ASSERT_NE(nullptr, single_clone.get());
  EXPECT_NE(single.get(), single_clone.get());
  EXPECT_STREQ("proj=merc", single_clone->param);
  EXPECT_EQ(0, single_clone->used);
  EXPECT_EQ(nullptr, single_clone->next);

  ScopedParalist multi =
      MakeParalist(ctx.get(), {"+proj=utm", "+zone=32", "+ellps=WGS84"});
  ASSERT_NE(nullptr, multi.get());
  multi->used = 1;
  multi->next->used = 1;
  ScopedParalist multi_clone(pj_clone_paralist(multi.get()),
                             ParalistDeleter{ctx.get()});
  ASSERT_NE(nullptr, multi_clone.get());
  ASSERT_NE(nullptr, multi_clone->next);
  ASSERT_NE(nullptr, multi_clone->next->next);
  EXPECT_EQ(nullptr, multi_clone->next->next->next);
  EXPECT_STREQ("proj=utm", multi_clone->param);
  EXPECT_STREQ("zone=32", multi_clone->next->param);
  EXPECT_STREQ("ellps=WGS84", multi_clone->next->next->param);
  EXPECT_EQ(0, multi_clone->used);
  EXPECT_EQ(0, multi_clone->next->used);

  // Destroying original list must not invalidate the deep-cloned list.
  multi.reset();
  EXPECT_STREQ("ellps=WGS84", multi_clone->next->next->param);
}

TEST(InitcacheTest, InsertSearchGrowAndClearRoundTrip) {
  pj_clear_initcache();
  absl::Cleanup clear_cache = [] { pj_clear_initcache(); };

  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Insert 20 entries (> initial cache_alloc of 15) to trigger growth.
  constexpr int kEntryCount = 20;
  for (int i = 0; i < kEntryCount; ++i) {
    const std::string key = "testinit:entry_" + std::to_string(i);
    const std::string zone = "zone=" + std::to_string(i + 1);
    ScopedParalist entry = MakeParalist(ctx.get(), {"proj=utm", zone.c_str()});
    ASSERT_NE(nullptr, entry.get());
    pj_insert_initcache(key.c_str(), entry.get());
  }

  // 2. Verify lookup hits across pre- and post-growth boundaries and misses.
  for (int i : {0, 14, 15, 19}) {
    const std::string key = "testinit:entry_" + std::to_string(i);
    const std::string expected_zone = "zone=" + std::to_string(i + 1);
    ScopedParalist found(pj_search_initcache(key.c_str()),
                         ParalistDeleter{ctx.get()});
    ASSERT_NE(nullptr, found.get()) << key;
    ASSERT_NE(nullptr, found->next) << key;
    EXPECT_STREQ("proj=utm", found->param);
    EXPECT_STREQ(expected_zone.c_str(), found->next->param);
  }

  ScopedParalist miss(pj_search_initcache("testinit:nonexistent"),
                      ParalistDeleter{ctx.get()});
  EXPECT_EQ(nullptr, miss.get());

  // 3. Verify searched paralists are independent clones.
  ScopedParalist hit_a(pj_search_initcache("testinit:entry_0"),
                       ParalistDeleter{ctx.get()});
  ScopedParalist hit_b(pj_search_initcache("testinit:entry_0"),
                       ParalistDeleter{ctx.get()});
  ASSERT_NE(nullptr, hit_a.get());
  ASSERT_NE(nullptr, hit_b.get());
  EXPECT_NE(hit_a.get(), hit_b.get());
  hit_a->used = 1;
  EXPECT_EQ(0, hit_b->used);

  // 4. Clearing removes all entries.
  pj_clear_initcache();
  ScopedParalist after_clear(pj_search_initcache("testinit:entry_0"),
                             ParalistDeleter{ctx.get()});
  EXPECT_EQ(nullptr, after_clear.get());
}

TEST(InitcacheTest, ExpandInitViaInMemoryFileSystemPopulatesAndHitsCache) {
  pj_clear_initcache();
  absl::Cleanup clear_cache = [] { pj_clear_initcache(); };

  ScopedContext ctx1 = MakeContext();
  ASSERT_NE(nullptr, ctx1.get());
  InMemoryFileSystem fs;
  fs.AddTextFile("/mem/custom_init",
                 "<wgs_merc> +proj=merc +ellps=WGS84 +no_defs <>\n");
  ASSERT_TRUE(fs.Install(ctx1.get()));

  // 1. Initial expansion reads from virtual init file and caches the paralist.
  ScopedParalist init_arg1 =
      MakeParalist(ctx1.get(), {"init=/mem/custom_init:wgs_merc"});
  ASSERT_NE(nullptr, init_arg1.get());
  paralist* expanded1 = pj_expand_init(ctx1.get(), init_arg1.get());
  ASSERT_EQ(init_arg1.get(), expanded1);
  ASSERT_NE(nullptr, expanded1->next);
  EXPECT_STREQ("proj=merc", expanded1->next->param);

  // 2. Subsequent expansion on a separate context without the file hits cache.
  ScopedContext ctx2 = MakeContext();
  ASSERT_NE(nullptr, ctx2.get());
  ScopedParalist init_arg2 =
      MakeParalist(ctx2.get(), {"init=/mem/custom_init:wgs_merc"});
  ASSERT_NE(nullptr, init_arg2.get());
  paralist* expanded2 = pj_expand_init(ctx2.get(), init_arg2.get());
  ASSERT_EQ(init_arg2.get(), expanded2);
  ASSERT_NE(nullptr, expanded2->next);
  EXPECT_STREQ("proj=merc", expanded2->next->param);
}

TEST(InitcacheTest, ConcurrentInsertAndSearchAreThreadSafe) {
  pj_clear_initcache();
  // Declare cleanup before worker threads so all threads join prior to clear.
  absl::Cleanup clear_cache = [] { pj_clear_initcache(); };

  constexpr int kNumThreads = 4;
  constexpr int kKeysPerThread = 6;
  {
    std::vector<std::jthread> threads;
    threads.reserve(kNumThreads);
    for (int tid = 0; tid < kNumThreads; ++tid) {
      threads.emplace_back([tid] {
        ScopedContext tctx = MakeContext();
        ASSERT_NE(nullptr, tctx.get());
        for (int j = 0; j < kKeysPerThread; ++j) {
          const std::string key =
              "thread_" + std::to_string(tid) + ":item_" + std::to_string(j);
          const std::string val = "zone=" + std::to_string(tid * 10 + j + 1);
          ScopedParalist pl =
              MakeParalist(tctx.get(), {"proj=utm", val.c_str()});
          ASSERT_NE(nullptr, pl.get());
          pj_insert_initcache(key.c_str(), pl.get());

          ScopedParalist found(pj_search_initcache(key.c_str()),
                               ParalistDeleter{tctx.get()});
          ASSERT_NE(nullptr, found.get());
          ASSERT_NE(nullptr, found->next);
          EXPECT_STREQ(val.c_str(), found->next->param);
        }
      });
    }
  }

  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  for (int tid = 0; tid < kNumThreads; ++tid) {
    for (int j = 0; j < kKeysPerThread; ++j) {
      const std::string key =
          "thread_" + std::to_string(tid) + ":item_" + std::to_string(j);
      ScopedParalist found(pj_search_initcache(key.c_str()),
                           ParalistDeleter{ctx.get()});
      EXPECT_NE(nullptr, found.get()) << key;
    }
  }
}

}  // namespace
}  // namespace proj
