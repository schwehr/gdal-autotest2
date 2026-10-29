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
// Tests gcore/gdalhashsetbandblockcache.cpp.

#include <cstring>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_abstractbandblockcache.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"

namespace {

class HashSetTestBand : public GDALRasterBand {
 public:
  HashSetTestBand(GDALDataset* ds, int width, int height) {
    poDS = ds;
    nBand = 1;
    nRasterXSize = width;
    nRasterYSize = height;
    nBlockXSize = 4;
    nBlockYSize = 4;
    eDataType = GDT_Byte;
    eAccess = GA_Update;
  }

  void SetFailWrite(bool fail) { fail_write_ = fail; }
  int write_count() const { return write_count_; }

 protected:
  CPLErr IReadBlock(int x_off, int y_off, void* image) override {
    memset(image, 7, nBlockXSize * nBlockYSize);
    return CE_None;
  }

  CPLErr IWriteBlock(int x_off, int y_off, void* image) override {
    ++write_count_;
    return fail_write_ ? CE_Failure : CE_None;
  }

 private:
  bool fail_write_ = false;
  int write_count_ = 0;
};

class HashSetTestDataset : public GDALDataset {
 public:
  HashSetTestDataset() {
    nRasterXSize = 16;
    nRasterYSize = 16;
    eAccess = GA_Update;
    SetBand(1, new HashSetTestBand(this, 16, 16));
  }

  HashSetTestBand* GetTestBand() {
    return static_cast<HashSetTestBand*>(GetRasterBand(1));
  }
};

TEST(GdalhashsetbandblockcacheTest, BandAndDirectCacheOperations) {
  CPLSetThreadLocalConfigOption("GDAL_BAND_BLOCK_CACHE", "HASHSET");
  HashSetTestDataset ds;
  HashSetTestBand* band = ds.GetTestBand();

  // Trigger Init(), IsInitOK(), CreateBlock(), AdoptBlock(), and
  // TryGetLockedBlockRef() via GDALRasterBand::GetLockedBlockRef() across
  // multiple (x, y) coordinates so BlockComparator (< y, > y, == y && < x) is
  // exercised.
  int coords[4][2] = {{1, 1}, {0, 0}, {2, 1}, {0, 2}};
  for (const auto& c : coords) {
    GDALRasterBlock* block = band->GetLockedBlockRef(c[0], c[1], TRUE);
    ASSERT_NE(nullptr, block);
    block->MarkDirty();
    block->DropLock();
  }

  // Fetching an already-cached block hits TryGetLockedBlockRef -> TakeLock.
  GDALRasterBlock* cached = band->GetLockedBlockRef(1, 1, FALSE);
  ASSERT_NE(nullptr, cached);
  cached->DropLock();

  // Flush a non-existent block and an existing dirty block.
  EXPECT_EQ(CE_None, band->FlushBlock(3, 3, TRUE));
  EXPECT_EQ(CE_None, band->FlushBlock(1, 1, TRUE));
  EXPECT_EQ(1, band->write_count());

  // UnreferenceBlock is called when a block is evicted from the global block
  // cache list via FlushCacheBlock().
  GDALRasterBlock* to_evict = band->GetLockedBlockRef(0, 0, FALSE);
  ASSERT_NE(nullptr, to_evict);
  to_evict->MarkClean();
  to_evict->DropLock();
  GDALRasterBlock::FlushCacheBlock();

  // FlushCache with a failing write to exercise error propagation.
  band->SetFailWrite(true);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(CE_Failure, band->FlushCache(false));
  CPLPopErrorHandler();

  // Also directly exercise IsInitOK() on a standalone instance.
  std::unique_ptr<GDALAbstractBandBlockCache> direct_cache(
      GDALHashSetBandBlockCacheCreate(band));
  ASSERT_NE(nullptr, direct_cache);
  EXPECT_TRUE(direct_cache->Init());
  EXPECT_TRUE(direct_cache->IsInitOK());

  CPLSetThreadLocalConfigOption("GDAL_BAND_BLOCK_CACHE", nullptr);
}

}  // namespace
