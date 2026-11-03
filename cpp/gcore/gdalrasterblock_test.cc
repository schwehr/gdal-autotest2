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
// Tests GDALRasterBlock lifecycle and global block cache management
// (gdalrasterblock.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdalrasterblock.cpp

#include <climits>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

class CustomWriteBand final : public GDALRasterBand {
 public:
  CustomWriteBand(GDALDataset* ds, int band, bool fail_write = false)
      : m_fail_write(fail_write) {
    poDS = ds;
    nBand = band;
    nRasterXSize = 100;
    nRasterYSize = 1;
    eDataType = GDT_Byte;
    nBlockXSize = 1;
    nBlockYSize = 1;
  }
  CPLErr IReadBlock(int, int, void* pImage) override {
    *static_cast<GByte*>(pImage) = 0;
    return CE_None;
  }
  CPLErr IWriteBlock(int, int, void*) override {
    return m_fail_write ? CE_Failure : CE_None;
  }

 private:
  bool m_fail_write = false;
};

class CustomWriteDataset final : public GDALDataset {
 public:
  explicit CustomWriteDataset(bool fail_write = false) {
    nRasterXSize = 100;
    nRasterYSize = 1;
    eAccess = GA_Update;
    SetBand(1, new CustomWriteBand(this, 1, fail_write));
  }
};

TEST(GdalRasterBlockTest, CacheSizingAndBlockLifecycle) {
  // Set invalid GDAL_CACHEMAX before first GDALGetCacheMax64() call.
  CPLConfigOptionSetter opt_cachemax("GDAL_CACHEMAX", "INVALID_SIZE",
                                     /*bSetOnlyIfUndefined=*/false);
  CPLConfigOptionSetter opt_lock_type("GDAL_RB_LOCK_TYPE", "INVALID_LOCK",
                                      /*bSetOnlyIfUndefined=*/false);
  CPLConfigOptionSetter opt_debug_cache("GDAL_DEBUG_BLOCK_CACHE", "YES",
                                        /*bSetOnlyIfUndefined=*/false);
  CPLConfigOptionSetter opt_sleep1("GDAL_RB_FLUSHBLOCK_SLEEP_AFTER_DROP_LOCK",
                                   "0.0001", /*bSetOnlyIfUndefined=*/false);
  CPLConfigOptionSetter opt_sleep2("GDAL_RB_FLUSHBLOCK_SLEEP_AFTER_RB_LOCK",
                                   "0.0001", /*bSetOnlyIfUndefined=*/false);
  CPLConfigOptionSetter opt_sleep3("GDAL_RB_INTERNALIZE_SLEEP_AFTER_DROP_LOCK",
                                   "0.0001", /*bSetOnlyIfUndefined=*/false);
  CPLConfigOptionSetter opt_sleep4(
      "GDAL_RB_INTERNALIZE_SLEEP_AFTER_DETACH_BEFORE_WRITE", "0.0001",
      /*bSetOnlyIfUndefined=*/false);
  CPLConfigOptionSetter opt_sleep5("GDAL_RB_TRYGET_SLEEP_AFTER_TAKE_LOCK",
                                   "0.0001", /*bSetOnlyIfUndefined=*/false);

  GIntBig orig_max = 0;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    orig_max = GDALGetCacheMax64();
    CPLErrorReset();
  }
  EXPECT_GT(orig_max, 0);
  absl::Cleanup restore_cache_max = [orig_max] { GDALSetCacheMax64(orig_max); };

  // Test > INT_MAX truncation in GDALGetCacheMax().
  GDALSetCacheMax64(static_cast<GIntBig>(INT_MAX) + 1024);
  EXPECT_EQ(INT_MAX, GDALGetCacheMax());

  GDALSetCacheMax(10 * 1024 * 1024);
  EXPECT_EQ(10 * 1024 * 1024, GDALGetCacheMax());

  // Dummy 2-arg constructor (used for hashset lookup).
  GDALRasterBlock lookup_block(3, 4);
  EXPECT_EQ(3, lookup_block.GetXOff());
  EXPECT_EQ(4, lookup_block.GetYOff());
  EXPECT_EQ(CE_None, lookup_block.Write());  // clean block

  // DestroyRBMutex and re-initialize via GDALRasterBlock constructor.
  GDALRasterBlock::DestroyRBMutex();
  CustomWriteDataset ds1(false);
  CustomWriteDataset ds2(true);  // fails write
  GDALRasterBand* b1 = ds1.GetRasterBand(1);
  GDALRasterBand* b2 = ds2.GetRasterBand(1);
  ASSERT_NE(nullptr, b1);
  ASSERT_NE(nullptr, b2);

  // Create and internalize > 64 blocks on b1 so Internalize's 64-block batch
  // loop triggers.
  for (int x = 0; x < 70; ++x) {
    GDALRasterBlock* rb = b1->GetLockedBlockRef(x, 0, TRUE);
    ASSERT_NE(nullptr, rb);
    if (x < 3) {
      rb->MarkDirty();
      rb->MarkClean();
      rb->MarkDirty();
    }
    rb->Touch();
    rb->DropLock();
  }
  EXPECT_GT(GDALGetCacheUsed(), 0);
  EXPECT_GT(GDALGetCacheUsed64(), 0);

  // Also create dirty blocks on ds2 (another dataset).
  for (int x = 0; x < 3; ++x) {
    GDALRasterBlock* rb = b2->GetLockedBlockRef(x, 0, TRUE);
    ASSERT_NE(nullptr, rb);
    rb->MarkDirty();
    rb->DropLock();
  }

  // Test TakeLock on a valid block.
  GDALRasterBlock* rb0 = b1->TryGetLockedBlockRef(10, 0);
  if (rb0 != nullptr) {
    rb0->DropLock();
  }

  // Test EnterDisableDirtyBlockFlush / LeaveDisableDirtyBlockFlush.
  GDALRasterBlock::EnterDisableDirtyBlockFlush();
  GDALRasterBlock::FlushDirtyBlocks();  // should not flush while disabled
  GDALRasterBlock::LeaveDisableDirtyBlockFlush();

  // Trigger eviction during Internalize by setting cache max very small (e.g.,
  // 100 bytes). First flush all clean blocks on b1 while cache max is still
  // large, so ds2's dirty blocks stay in cache.
  EXPECT_EQ(CE_None, b1->FlushCache(false));
  GDALSetCacheMax64(100);
  // Now allocate new blocks on b1 while ONLY ds2 has dirty blocks -> evicts
  // dirty blocks of another dataset (failing write) and recycles blocks from
  // the band free list via RecycleFor.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    for (int x = 70; x < 80; ++x) {
      GDALRasterBlock* rb = b1->GetLockedBlockRef(x, 0, TRUE);
      ASSERT_NE(nullptr, rb);
      rb->MarkDirty();
      rb->DropLock();
    }
    CPLErrorReset();
  }

  // Also test the fallback branch where the oldest dirty block of another
  // dataset is locked.
  GDALSetCacheMax64(10 * 1024 * 1024);
  EXPECT_EQ(CE_None, b1->FlushCache(false));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_None, b2->FlushCache(false));
    CPLErrorReset();
  }
  GDALRasterBlock* ds2_b0 = b2->GetLockedBlockRef(0, 0, TRUE);
  ASSERT_NE(nullptr, ds2_b0);
  absl::Cleanup drop_ds2_b0 = [&ds2_b0] {
    if (ds2_b0 != nullptr) {
      ds2_b0->DropLock();
    }
  };
  ds2_b0->MarkDirty();  // keep ds2_b0 locked (nLockCount == 1) and oldest
  GDALRasterBlock* ds2_b1 = b2->GetLockedBlockRef(1, 0, TRUE);
  ASSERT_NE(nullptr, ds2_b1);
  ds2_b1->MarkDirty();
  ds2_b1->DropLock();  // unlocked dirty block of another dataset
  // Now shrink cache max so Internalize must evict, sees ds2_b0 first (locked),
  // and falls back to scanning for ds2_b1.
  GDALSetCacheMax64(100);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterBlock* rb = b1->GetLockedBlockRef(80, 0, TRUE);
    ASSERT_NE(nullptr, rb);
    rb->DropLock();
    CPLErrorReset();
  }
  ds2_b0->DropLock();
  ds2_b0 = nullptr;

  // Lock all remaining blocks in cache and call GDALSetCacheMax(0) to hit the
  // `break` in GDALSetCacheMax64.
  {
    GDALRasterBlock* locked_rb = b1->GetLockedBlockRef(74, 0, TRUE);
    ASSERT_NE(nullptr, locked_rb);
    absl::Cleanup drop_locked_rb = [locked_rb] { locked_rb->DropLock(); };
    // Flush everything else while locked_rb is held.
    GDALSetCacheMax(0);
  }

  // Flush dirty blocks explicitly.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterBlock::FlushDirtyBlocks();
    CPLErrorReset();
  }
  GDALRasterBlock::Verify();

  // Direct test of Write() on dummy block with poBand == nullptr.
  lookup_block.MarkDirty();
  EXPECT_EQ(CE_Failure, lookup_block.Write());

  // Direct test of TakeLock when nLockCount == -1, and
  // DropLockForRemovalFromStorage.
  GDALRasterBlock direct_block(b1, 0, 0);
  // Simulate a block being evicted (nLockCount == -1):
  direct_block.DropLock();                    // 0 -> -1
  EXPECT_EQ(FALSE, direct_block.TakeLock());  // -1 -> 0 -> -1 and returns FALSE
  EXPECT_EQ(FALSE, direct_block.DropLockForRemovalFromStorage());
  direct_block
      .AddLock();  // -1 -> 0 so destructor assertion nLockCount <= 0 holds
}

}  // namespace
}  // namespace autotest2
