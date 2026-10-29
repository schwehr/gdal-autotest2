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
// Tests gcore/gdalarraybandblockcache.cpp.

#include <cstring>
#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_abstractbandblockcache.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"

namespace {

class ArrayCacheTestBand : public GDALRasterBand {
 public:
  ArrayCacheTestBand(GDALDataset* ds, int width, int height, int block_x,
                     int block_y) {
    poDS = ds;
    nBand = 1;
    nRasterXSize = width;
    nRasterYSize = height;
    nBlockXSize = block_x;
    nBlockYSize = block_y;
    eDataType = GDT_Byte;
    eAccess = GA_Update;
  }

  void SetFailWrite(bool fail) { fail_write_ = fail; }
  int write_count() const { return write_count_; }

 protected:
  CPLErr IReadBlock(int x_off, int y_off, void* image) override {
    memset(image, 3, nBlockXSize * nBlockYSize);
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

class ArrayCacheTestDataset : public GDALDataset {
 public:
  ArrayCacheTestDataset(int width, int height, int block_x, int block_y) {
    nRasterXSize = width;
    nRasterYSize = height;
    eAccess = GA_Update;
    SetBand(1, new ArrayCacheTestBand(this, width, height, block_x, block_y));
  }

  ArrayCacheTestBand* GetTestBand() {
    return static_cast<ArrayCacheTestBand*>(GetRasterBand(1));
  }
};

TEST(GdalarraybandblockcacheTest, NonSubblockingAndSubblockingModes) {
  CPLSetThreadLocalConfigOption("GDAL_BAND_BLOCK_CACHE", "ARRAY");

  // 1. Non-subblocking mode: nBlocksPerRow < 32 (e.g., 16x16 with 4x4 blocks ->
  // 4x4 blocks).
  // 2. Subblocking mode: nBlocksPerRow >= 32 (e.g., 160x160 with 4x4 blocks ->
  // 40x40 blocks).
  for (int raster_size : {16, 160}) {
    ArrayCacheTestDataset ds(raster_size, raster_size, 4, 4);
    ArrayCacheTestBand* band = ds.GetTestBand();

    // Before cache is initialized, TryGetLockedBlockRef returns nullptr and
    // FlushBlock returns CE_Failure.
    EXPECT_EQ(nullptr, band->TryGetLockedBlockRef(0, 0));
    EXPECT_EQ(CE_Failure, band->FlushBlock(0, 0, TRUE));

    int coords[4][2] = {{0, 0}, {1, 0}, {2, 1}, {3, 1}};
    for (const auto& c : coords) {
      GDALRasterBlock* b = band->GetLockedBlockRef(c[0], c[1], TRUE);
      ASSERT_NE(nullptr, b);
      b->MarkDirty();
      b->DropLock();
    }

    // Missing block after cache is initialized.
    EXPECT_EQ(nullptr, band->TryGetLockedBlockRef(3, 3));
    EXPECT_EQ(CE_None, band->FlushBlock(3, 3, TRUE));

    // Hit cached block via TryGetLockedBlockRef.
    GDALRasterBlock* cached = band->TryGetLockedBlockRef(1, 0);
    ASSERT_NE(nullptr, cached);
    cached->DropLock();

    // Flush single dirty block.
    EXPECT_EQ(CE_None, band->FlushBlock(1, 0, TRUE));
    EXPECT_EQ(1, band->write_count());

    // Evict one clean block via FlushCacheBlock -> UnreferenceBlock.
    GDALRasterBlock* to_evict = band->TryGetLockedBlockRef(0, 0);
    ASSERT_NE(nullptr, to_evict);
    to_evict->MarkClean();
    to_evict->DropLock();
    GDALRasterBlock::FlushCacheBlock();

    // FlushCache with write failure on remaining dirty blocks (2, 1) and (3,
    // 1).
    band->SetFailWrite(true);
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(CE_Failure, band->FlushCache(false));
    CPLPopErrorHandler();

    // Standalone cache instance to exercise IsInitOK() directly.
    std::unique_ptr<GDALAbstractBandBlockCache> direct(
        GDALArrayBandBlockCacheCreate(band));
    ASSERT_NE(nullptr, direct);
    EXPECT_TRUE(direct->Init());
    EXPECT_TRUE(direct->IsInitOK());
  }

  CPLSetThreadLocalConfigOption("GDAL_BAND_BLOCK_CACHE", nullptr);
}

}  // namespace
