// Copyright 2026 Google Inc. All Rights Reserved.
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
// Tests gcore/gdalabstractbandblockcache.cpp.

#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_abstractbandblockcache.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"

namespace {

class TestBandBlockCache : public GDALAbstractBandBlockCache {
 public:
  explicit TestBandBlockCache(GDALRasterBand* band)
      : GDALAbstractBandBlockCache(band) {}

  ~TestBandBlockCache() override { FlushCache(); }

  bool Init() override { return true; }
  bool IsInitOK() override { return true; }
  CPLErr FlushCache() override {
    FreeDanglingBlocks();
    WaitCompletionPendingTasks();
    return CE_None;
  }
  CPLErr AdoptBlock(GDALRasterBlock* block) override { return CE_None; }
  GDALRasterBlock* TryGetLockedBlockRef(int x_off, int y_off) override {
    return nullptr;
  }
  CPLErr UnreferenceBlock(GDALRasterBlock* block) override {
    UnreferenceBlockBase();
    return CE_None;
  }
  CPLErr FlushBlock(int x_off, int y_off, int write_dirty) override {
    return CE_None;
  }

  void CallFreeDanglingBlocks() { FreeDanglingBlocks(); }
  void CallStartLog() { StartDirtyBlockFlushingLog(); }
  void CallUpdateLog() { UpdateDirtyBlockFlushingLog(); }
  void CallEndLog() { EndDirtyBlockFlushingLog(); }

  void SetInitialDirtyBlocksForLog(int count) {
    m_nInitialDirtyBlocksInFlushCache = count;
    m_nLastTick = -1;
  }
};

TEST(GdalabstractbandblockcacheTest, BlockRecyclingAndDirtyBlockLogging) {
  GDALAllRegister();
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);
  std::unique_ptr<GDALDataset> ds(
      mem_driver->Create("", 8, 8, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);

  TestBandBlockCache cache(ds->GetRasterBand(1));

  // Create a new block, unreference it, add to free list, and recycle it via
  // CreateBlock.
  GDALRasterBlock* b1 = cache.CreateBlock(0, 0);
  ASSERT_NE(nullptr, b1);
  EXPECT_EQ(CE_None, cache.UnreferenceBlock(b1));
  cache.AddBlockToFreeList(b1);

  // Recycling from free list returns the same block reinitialized for (1, 0).
  GDALRasterBlock* b2 = cache.CreateBlock(1, 0);
  EXPECT_EQ(b1, b2);
  EXPECT_EQ(1, b2->GetXOff());
  EXPECT_EQ(0, b2->GetYOff());

  // Put it back on the free list and free dangling blocks explicitly.
  EXPECT_EQ(CE_None, cache.UnreferenceBlock(b2));
  cache.AddBlockToFreeList(b2);
  cache.CallFreeDanglingBlocks();

  // WaitCompletionPendingTasks when counter is 0 returns immediately.
  cache.WaitCompletionPendingTasks();

  // Dirty block counter and log progress.
  EXPECT_FALSE(cache.HasDirtyBlocks());
  cache.IncDirtyBlocks(2);
  EXPECT_TRUE(cache.HasDirtyBlocks());

  // Enable CPL_DEBUG so StartDirtyBlockFlushingLog initializes progress.
  CPLSetThreadLocalConfigOption("CPL_DEBUG", "ON");
  CPLSetThreadLocalConfigOption("GDAL_REPORT_DIRTY_BLOCK_FLUSHING", nullptr);
  cache.CallStartLog();
  // Even if WITHOUT_CPLDEBUG is compiled in, also set
  // m_nInitialDirtyBlocksInFlushCache directly so UpdateDirtyBlockFlushingLog
  // is guaranteed to execute all ticks.
  cache.SetInitialDirtyBlocksForLog(2);
  cache.IncDirtyBlocks(-1);
  cache.CallUpdateLog();
  cache.IncDirtyBlocks(-1);
  cache.CallUpdateLog();
  cache.CallEndLog();
  CPLSetThreadLocalConfigOption("CPL_DEBUG", nullptr);

  EXPECT_FALSE(cache.HasDirtyBlocks());
}

}  // namespace
