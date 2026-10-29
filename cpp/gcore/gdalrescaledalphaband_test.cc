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
// Tests gcore/gdalrescaledalphaband.cpp.

#include <cstring>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"

namespace {

class MockUInt16Band : public GDALRasterBand {
 public:
  MockUInt16Band(GDALDataset* ds, int width, int height, int block_x,
                 int block_y, bool fail_read = false)
      : fail_read_(fail_read), data_(width * height, 0) {
    poDS = ds;
    nBand = 1;
    nRasterXSize = width;
    nRasterYSize = height;
    nBlockXSize = block_x;
    nBlockYSize = block_y;
    eDataType = GDT_UInt16;
  }

  void SetPixel(int x, int y, GUInt16 val) {
    data_[y * nRasterXSize + x] = val;
  }

 protected:
  CPLErr IReadBlock(int x_block, int y_block, void* image) override {
    if (fail_read_) {
      return CE_Failure;
    }
    GUInt16* dst = static_cast<GUInt16*>(image);
    for (int y = 0; y < nBlockYSize; ++y) {
      int src_y = y_block * nBlockYSize + y;
      for (int x = 0; x < nBlockXSize; ++x) {
        int src_x = x_block * nBlockXSize + x;
        if (src_x < nRasterXSize && src_y < nRasterYSize) {
          dst[y * nBlockXSize + x] = data_[src_y * nRasterXSize + src_x];
        } else {
          dst[y * nBlockXSize + x] = 0;
        }
      }
    }
    return CE_None;
  }

 private:
  bool fail_read_;
  std::vector<GUInt16> data_;
};

class DummyDataset : public GDALDataset {
 public:
  DummyDataset(int width, int height, int block_x, int block_y,
               bool fail_read = false) {
    nRasterXSize = width;
    nRasterYSize = height;
    SetBand(1, new MockUInt16Band(this, width, height, block_x, block_y,
                                  fail_read));
  }
  MockUInt16Band* GetMockBand() {
    return static_cast<MockUInt16Band*>(GetRasterBand(1));
  }
};

TEST(GdalrescaledalphabandTest, ReadBlockAndRasterIO) {
  DummyDataset ds(5, 5, 4, 4);
  MockUInt16Band* parent = ds.GetMockBand();
  parent->SetPixel(0, 0, 0);
  parent->SetPixel(1, 0, 1);      // 1..256 -> rescaled to 1
  parent->SetPixel(2, 0, 256);    // 1..256 -> rescaled to 1
  parent->SetPixel(3, 0, 32768);  // > 256 -> (32768 * 255) / 65535 = 127
  parent->SetPixel(4, 4, 65535);  // -> 255

  GDALRescaledAlphaBand alpha(parent);
  EXPECT_EQ(5, alpha.GetXSize());
  EXPECT_EQ(5, alpha.GetYSize());
  EXPECT_EQ(GDT_Byte, alpha.GetRasterDataType());

  // Full block (0, 0) of size 4x4 via ReadBlock -> IReadBlock.
  std::vector<GByte> block(16, 99);
  EXPECT_EQ(CE_None, alpha.ReadBlock(0, 0, block.data()));
  EXPECT_EQ(0, block[0]);
  EXPECT_EQ(1, block[1]);
  EXPECT_EQ(1, block[2]);
  EXPECT_EQ(127, block[3]);

  // Partial edge block (1, 1) covering pixel (4, 4).
  std::vector<GByte> edge_block(16, 0);
  EXPECT_EQ(CE_None, alpha.ReadBlock(1, 1, edge_block.data()));
  EXPECT_EQ(255, edge_block[0]);

  // Fast path IRasterIO (GDT_Byte, 1:1 size, pixel_space = 1).
  std::vector<GByte> fast_buf(5, 0);
  EXPECT_EQ(CE_None, alpha.RasterIO(GF_Read, 0, 0, 5, 1, fast_buf.data(), 5, 1,
                                    GDT_Byte, 1, 5, nullptr));
  EXPECT_EQ(0, fast_buf[0]);
  EXPECT_EQ(1, fast_buf[1]);
  EXPECT_EQ(1, fast_buf[2]);
  EXPECT_EQ(127, fast_buf[3]);

  // Fallback path IRasterIO (eBufType != GDT_Byte, e.g., GDT_UInt16).
  std::vector<GUInt16> slow_buf(5, 0);
  EXPECT_EQ(CE_None, alpha.RasterIO(GF_Read, 0, 0, 5, 1, slow_buf.data(), 5, 1,
                                    GDT_UInt16, 0, 0, nullptr));
  EXPECT_EQ(0, slow_buf[0]);
  EXPECT_EQ(1, slow_buf[1]);
  EXPECT_EQ(127, slow_buf[3]);

  // Write attempt emits error via EmitErrorMessageIfWriteNotSupported.
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(CE_Failure, alpha.Fill(0));
  EXPECT_EQ(CE_Failure, alpha.RasterIO(GF_Write, 0, 0, 5, 1, fast_buf.data(), 5,
                                       1, GDT_Byte, 1, 5, nullptr));
  CPLPopErrorHandler();
}

TEST(GdalrescaledalphabandTest, ParentReadErrorPropagates) {
  DummyDataset ds(4, 4, 4, 4, /*fail_read=*/true);
  GDALRescaledAlphaBand alpha(ds.GetMockBand());
  std::vector<GByte> buf(4, 0);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(CE_Failure, alpha.RasterIO(GF_Read, 0, 0, 4, 1, buf.data(), 4, 1,
                                       GDT_Byte, 1, 4, nullptr));
  CPLPopErrorHandler();
}

}  // namespace
