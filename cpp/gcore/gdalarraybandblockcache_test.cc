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
// Tests GDALArrayBandBlockCache (gcore/gdalarraybandblockcache.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdalarraybandblockcache.cpp

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_abstractbandblockcache.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

class ArrayCacheTestBand final : public GDALRasterBand {
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
    std::memset(image, 3, static_cast<size_t>(nBlockXSize) * nBlockYSize);
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

class ArrayCacheTestDataset final : public GDALDataset {
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

TEST(GdalArrayBandBlockCacheTest, NonSubblockingAndSubblockingModes) {
  CPLConfigOptionSetter block_cache_opt("GDAL_BAND_BLOCK_CACHE", "ARRAY",
                                        /*bSetOnlyIfUndefined=*/false);

  // 1. Non-subblocking mode: nBlocksPerRow < 32 (16x16 with 4x4 blocks).
  // 2. Subblocking mode: nBlocksPerRow >= 32 (160x160 with 4x4 blocks).
  constexpr std::array<std::pair<int, int>, 4> kCoords = {
      {{0, 0}, {1, 0}, {2, 1}, {3, 1}}};
  for (int raster_size : {16, 160}) {
    ArrayCacheTestDataset ds(raster_size, raster_size, 4, 4);
    ArrayCacheTestBand* band = ds.GetTestBand();
    ASSERT_NE(nullptr, band);

    // Before cache is initialized, TryGetLockedBlockRef returns nullptr and
    // FlushBlock returns CE_Failure.
    EXPECT_EQ(nullptr, band->TryGetLockedBlockRef(0, 0));
    EXPECT_EQ(CE_Failure, band->FlushBlock(0, 0, TRUE));

    for (const auto& [bx, by] : kCoords) {
      GDALRasterBlock* b = band->GetLockedBlockRef(bx, by, TRUE);
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

    // Touch (0, 0) via TryGetLockedBlockRef (which calls TakeLock -> Touch(),
    // making (0, 0) MRU) and FlushCacheBlock() evicts the oldest remaining
    // dirty block (2, 1), triggering an IWriteBlock.
    GDALRasterBlock* touch_block = band->TryGetLockedBlockRef(0, 0);
    ASSERT_NE(nullptr, touch_block);
    touch_block->DropLock();
    GDALRasterBlock::FlushCacheBlock();
    EXPECT_EQ(nullptr, band->TryGetLockedBlockRef(2, 1));
    EXPECT_EQ(2, band->write_count());

    // FlushCache with write failure on remaining dirty blocks (0, 0) and (3,
    // 1).
    band->SetFailWrite(true);
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, band->FlushCache(false));
      CPLErrorReset();
    }

    // Standalone cache instance to exercise IsInitOK() directly.
    std::unique_ptr<GDALAbstractBandBlockCache> direct(
        GDALArrayBandBlockCacheCreate(band));
    ASSERT_NE(nullptr, direct);
    EXPECT_TRUE(direct->Init());
    EXPECT_TRUE(direct->IsInitOK());
  }
}

}  // namespace
}  // namespace autotest2
