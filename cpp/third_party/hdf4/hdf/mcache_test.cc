#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"

#ifdef SUCCEED
#undef SUCCEED
#endif
#ifdef FAIL
#undef FAIL
#endif

#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/herr_priv.h"
#include "third_party/hdf4/hdf/src/hqueue_priv.h"
#include "third_party/hdf4/hdf/src/mcache_priv.h"

namespace {

struct MockStorage {
  int pagesize = 64;
  int npages = 4;
  std::vector<std::vector<uint8_t>> pages;
  bool fail_pgin = false;
  bool fail_pgout = false;
  int pgin_calls = 0;
  int pgout_calls = 0;

  explicit MockStorage(int ps = 64, int np = 4)
      : pagesize(ps), npages(np), pages(np, std::vector<uint8_t>(ps, 0)) {
    for (int i = 0; i < np; ++i) {
      std::memset(pages[i].data(), i + 1, ps);
    }
  }
};

int32 MockPgin(void* cookie, int32 pgno, void* page) {
  auto* storage = static_cast<MockStorage*>(cookie);
  storage->pgin_calls++;
  if (storage->fail_pgin) {
    // Upstream HDF4 bug in mcache_get: when pgin fails, the newly allocated
    // bucket is never added to the lru list and is leaked by mcache_get.
    // Free the bucket buffer here to avoid memory leaks during test.
    std::free(static_cast<char*>(page) - sizeof(BKT));
    return FAIL;
  }
  if (pgno < 0 || pgno >= storage->npages) {
    return FAIL;
  }
  std::memcpy(page, storage->pages[pgno].data(), storage->pagesize);
  return SUCCEED;
}

int32 MockPgout(void* cookie, int32 pgno, const void* page) {
  auto* storage = static_cast<MockStorage*>(cookie);
  storage->pgout_calls++;
  if (storage->fail_pgout) {
    return FAIL;
  }
  if (pgno < 0 || pgno >= storage->npages) {
    return FAIL;
  }
  std::memcpy(storage->pages[pgno].data(), page, storage->pagesize);
  return SUCCEED;
}

class McacheTest : public ::testing::Test {
 protected:
  void SetUp() override { HEclear(); }
  void TearDown() override { HEclear(); }
};

TEST_F(McacheTest, NullPointersHandledSafely) {
  EXPECT_EQ(0, mcache_get_npages(nullptr));
  EXPECT_EQ(0, mcache_get_maxcache(nullptr));
  EXPECT_EQ(0, mcache_set_maxcache(nullptr, 10));
  EXPECT_EQ(0, mcache_get_pagesize(nullptr));
  EXPECT_EQ(nullptr, mcache_get(nullptr, 1, 0));
  EXPECT_EQ(FAIL, mcache_put(nullptr, nullptr, 0));
  EXPECT_EQ(FAIL, mcache_close(nullptr));
  EXPECT_EQ(FAIL, mcache_sync(nullptr));
}

TEST_F(McacheTest, OpenDefaultsAndAccessors) {
  MCACHE* mp = mcache_open(nullptr, 1, 0, 0, 5, 0);
  ASSERT_NE(nullptr, mp);

  EXPECT_EQ(DEF_PAGESIZE, mcache_get_pagesize(mp));
  EXPECT_EQ(DEF_MAXCACHE, mcache_get_maxcache(mp));
  EXPECT_EQ(5, mcache_get_npages(mp));

  EXPECT_EQ(10, mcache_set_maxcache(mp, 10));
  EXPECT_EQ(10, mcache_get_maxcache(mp));

  EXPECT_EQ(5, mcache_set_maxcache(mp, 5));
  EXPECT_EQ(5, mcache_get_maxcache(mp));

  EXPECT_EQ(SUCCEED, mcache_close(mp));
}

TEST_F(McacheTest, OpenWithFlagsNonZero) {
  // flags = 1 denotes pages do not exist on disk initially (eflags = 0).
  MCACHE* mp = mcache_open(nullptr, 1, 64, 2, 3, 1);
  ASSERT_NE(nullptr, mp);

  // Getting a page when eflags == 0 should not call pgin, but allocate a new
  // list elem.
  void* page = mcache_get(mp, 1, 0);
  ASSERT_NE(nullptr, page);
  EXPECT_EQ(SUCCEED, mcache_put(mp, page, 0));

  EXPECT_EQ(SUCCEED, mcache_close(mp));
}

TEST_F(McacheTest, GetInvalidPageNumber) {
  MCACHE* mp = mcache_open(nullptr, 1, 64, 2, 3, 0);
  ASSERT_NE(nullptr, mp);

  EXPECT_EQ(nullptr, mcache_get(mp, 4, 0));

  EXPECT_EQ(SUCCEED, mcache_close(mp));
}

TEST_F(McacheTest, ReadWriteAndCacheHit) {
  MockStorage storage(64, 4);
  MCACHE* mp = mcache_open(nullptr, 1, 64, 2, 4, 0);
  ASSERT_NE(nullptr, mp);
  mcache_filter(mp, MockPgin, MockPgout, &storage);

  // Get page 1 (1-based page number) -> calls MockPgin for chunk 0.
  void* p1 = mcache_get(mp, 1, 0);
  ASSERT_NE(nullptr, p1);
  EXPECT_EQ(1, storage.pgin_calls);
  EXPECT_EQ(1, static_cast<uint8_t*>(p1)[0]);

  // Repeated get on page 1 -> cache hit, no extra pgin call.
  void* p1_again = mcache_get(mp, 1, 0);
  EXPECT_EQ(p1, p1_again);
  EXPECT_EQ(1, storage.pgin_calls);

  // Modify page 1 and put it back dirty.
  static_cast<uint8_t*>(p1)[0] = 42;
  EXPECT_EQ(SUCCEED, mcache_put(mp, p1, MCACHE_DIRTY));

  // Sync to storage.
  EXPECT_EQ(SUCCEED, mcache_sync(mp));
  EXPECT_EQ(1, storage.pgout_calls);
  EXPECT_EQ(42, storage.pages[0][0]);

  EXPECT_EQ(SUCCEED, mcache_close(mp));
}

TEST_F(McacheTest, LruEvictionCleanAndDirty) {
  MockStorage storage(64, 4);
  MCACHE* mp = mcache_open(nullptr, 1, 64, 2, 4, 0);
  ASSERT_NE(nullptr, mp);
  mcache_filter(mp, MockPgin, MockPgout, &storage);

  // Max cache is 2. Fill both slots.
  void* p1 = mcache_get(mp, 1, 0);
  ASSERT_NE(nullptr, p1);
  EXPECT_EQ(SUCCEED, mcache_put(mp, p1, 0));  // Unpinned, clean

  void* p2 = mcache_get(mp, 2, 0);
  ASSERT_NE(nullptr, p2);
  EXPECT_EQ(SUCCEED, mcache_put(mp, p2, MCACHE_DIRTY));  // Unpinned, dirty

  // Getting page 3 evicts page 1 (oldest unpinned clean page).
  void* p3 = mcache_get(mp, 3, 0);
  ASSERT_NE(nullptr, p3);
  EXPECT_EQ(SUCCEED, mcache_put(mp, p3, 0));

  // Getting page 4 evicts page 2 (dirty page), which triggers write to storage.
  EXPECT_EQ(0, storage.pgout_calls);
  void* p4 = mcache_get(mp, 4, 0);
  ASSERT_NE(nullptr, p4);
  EXPECT_EQ(1, storage.pgout_calls);
  EXPECT_EQ(SUCCEED, mcache_put(mp, p4, 0));

  EXPECT_EQ(SUCCEED, mcache_close(mp));
}

TEST_F(McacheTest, CacheGrowsWhenAllPagesPinned) {
  MockStorage storage(64, 4);
  MCACHE* mp = mcache_open(nullptr, 1, 64, 2, 4, 0);
  ASSERT_NE(nullptr, mp);
  mcache_filter(mp, MockPgin, MockPgout, &storage);

  // Pin both pages (by getting without putting).
  void* p1 = mcache_get(mp, 1, 0);
  ASSERT_NE(nullptr, p1);
  void* p2 = mcache_get(mp, 2, 0);
  ASSERT_NE(nullptr, p2);

  // Getting page 3 cannot evict p1 or p2 because both are pinned.
  // Cache grows beyond maxcache.
  void* p3 = mcache_get(mp, 3, 0);
  ASSERT_NE(nullptr, p3);

  EXPECT_EQ(SUCCEED, mcache_put(mp, p1, 0));
  EXPECT_EQ(SUCCEED, mcache_put(mp, p2, 0));
  EXPECT_EQ(SUCCEED, mcache_put(mp, p3, 0));

  EXPECT_EQ(SUCCEED, mcache_close(mp));
}

TEST_F(McacheTest, FilterErrorPaths) {
  // Case 1: Missing pgout filter.
  MockStorage storage(64, 4);
  MCACHE* mp1 = mcache_open(nullptr, 1, 64, 2, 4, 0);
  ASSERT_NE(nullptr, mp1);
  mcache_filter(mp1, MockPgin, nullptr, &storage);
  void* p1 = mcache_get(mp1, 1, 0);
  ASSERT_NE(nullptr, p1);
  EXPECT_EQ(SUCCEED, mcache_put(mp1, p1, MCACHE_DIRTY));
  EXPECT_EQ(FAIL, mcache_sync(mp1));
  EXPECT_EQ(SUCCEED, mcache_close(mp1));

  // Case 2: Failing pgout filter during sync.
  storage.fail_pgout = true;
  MCACHE* mp2 = mcache_open(nullptr, 1, 64, 2, 4, 0);
  ASSERT_NE(nullptr, mp2);
  mcache_filter(mp2, MockPgin, MockPgout, &storage);
  void* p2 = mcache_get(mp2, 1, 0);
  ASSERT_NE(nullptr, p2);
  EXPECT_EQ(SUCCEED, mcache_put(mp2, p2, MCACHE_DIRTY));
  EXPECT_EQ(FAIL, mcache_sync(mp2));
  EXPECT_EQ(SUCCEED, mcache_close(mp2));

  // Case 3: Failing pgin filter during mcache_get.
  storage.fail_pgout = false;
  storage.fail_pgin = true;
  MCACHE* mp3 = mcache_open(nullptr, 1, 64, 2, 4, 0);
  ASSERT_NE(nullptr, mp3);
  mcache_filter(mp3, MockPgin, MockPgout, &storage);
  EXPECT_EQ(nullptr, mcache_get(mp3, 1, 0));
  EXPECT_EQ(SUCCEED, mcache_close(mp3));
}

TEST_F(McacheTest, EvictionFlushFailure) {
  MockStorage storage(64, 4);
  // Max cache 1.
  MCACHE* mp = mcache_open(nullptr, 1, 64, 1, 4, 0);
  ASSERT_NE(nullptr, mp);
  mcache_filter(mp, MockPgin, MockPgout, &storage);

  void* p1 = mcache_get(mp, 1, 0);
  ASSERT_NE(nullptr, p1);
  EXPECT_EQ(SUCCEED, mcache_put(mp, p1, MCACHE_DIRTY));

  // Make pgout fail when mcache_bkt attempts to flush p1 during eviction.
  storage.fail_pgout = true;
  EXPECT_EQ(nullptr, mcache_get(mp, 2, 0));

  // Upstream bug in mcache_bkt: on write failure during eviction, free(bp)
  // is executed without unlinking bp from mp->lqh and mp->hqh.
  // Re-initialize queue heads to avoid use-after-free during mcache_close.
  mp->lqh.cqh_first = reinterpret_cast<BKT*>(&mp->lqh);
  mp->lqh.cqh_last = reinterpret_cast<BKT*>(&mp->lqh);
  for (int entry = 0; entry < HASHSIZE; ++entry) {
    mp->hqh[entry].cqh_first = reinterpret_cast<BKT*>(&mp->hqh[entry]);
    mp->hqh[entry].cqh_last = reinterpret_cast<BKT*>(&mp->hqh[entry]);
  }

  EXPECT_EQ(SUCCEED, mcache_close(mp));
}

}  // namespace
