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
// Tests gcore/gdalnodatavaluesmaskband.cpp.

#include <cstring>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"

namespace {

class PartialBlockBand : public GDALRasterBand {
 public:
  PartialBlockBand(GDALDataset* ds, int band_num, bool fail_read)
      : fail_read_(fail_read) {
    poDS = ds;
    nBand = band_num;
    nRasterXSize = 3;
    nRasterYSize = 3;
    nBlockXSize = 4;
    nBlockYSize = 4;
    eDataType = GDT_Byte;
  }

 protected:
  CPLErr IReadBlock(int x_off, int y_off, void* image) override {
    if (fail_read_) {
      return CE_Failure;
    }
    memset(image, 5, nBlockXSize * nBlockYSize);
    return CE_None;
  }

 private:
  bool fail_read_;
};

class PartialBlockDataset : public GDALDataset {
 public:
  explicit PartialBlockDataset(bool fail_read) {
    nRasterXSize = 3;
    nRasterYSize = 3;
    SetMetadataItem("NODATA_VALUES", "5 5");
    SetBand(1, new PartialBlockBand(this, 1, fail_read));
    SetBand(2, new PartialBlockBand(this, 2, fail_read));
  }
};

TEST(GdalnodatavaluesmaskbandTest, AllDataTypes) {
  GDALAllRegister();
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  const GDALDataType types[] = {GDT_Byte,    GDT_UInt16,  GDT_Int16,
                                GDT_Float32, GDT_Float64, GDT_Int64};
  for (GDALDataType dt : types) {
    std::unique_ptr<GDALDataset> ds(
        mem_driver->Create("", 4, 1, 2, dt, nullptr));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(CE_None, ds->SetMetadataItem("NODATA_VALUES", "10 20"));

    // Pixel 0: (10, 20) -> matches nodata in both bands -> mask = 0
    // Pixel 1: (10, 99) -> matches only band 1 -> mask = 255
    // Pixel 2: (99, 20) -> matches only band 2 -> mask = 255
    // Pixel 3: (1, 2)   -> matches neither -> mask = 255
    double b1[4] = {10.0, 10.0, 99.0, 1.0};
    double b2[4] = {20.0, 99.0, 20.0, 2.0};
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 1, b1, 4, 1,
                                             GDT_Float64, 0, 0, nullptr));
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(2)->RasterIO(GF_Write, 0, 0, 4, 1, b2, 4, 1,
                                             GDT_Float64, 0, 0, nullptr));

    GDALNoDataValuesMaskBand mask_band(ds.get());
    std::vector<GByte> mask(4, 123);
    EXPECT_EQ(CE_None, mask_band.ReadBlock(0, 0, mask.data()));
    EXPECT_EQ(0, mask[0]);
    EXPECT_EQ(255, mask[1]);
    EXPECT_EQ(255, mask[2]);
    EXPECT_EQ(255, mask[3]);

    // Write attempt triggers EmitErrorMessageIfWriteNotSupported.
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(CE_Failure, mask_band.Fill(0));
    CPLPopErrorHandler();
  }
}

TEST(GdalnodatavaluesmaskbandTest, PartialBlock) {
  // Partial block (3x3 raster with 4x4 block size).
  PartialBlockDataset ok_ds(/*fail_read=*/false);
  GDALNoDataValuesMaskBand ok_mask(&ok_ds);
  std::vector<GByte> buf(16, 255);
  EXPECT_EQ(CE_None, ok_mask.ReadBlock(0, 0, buf.data()));
  EXPECT_EQ(0, buf[0]);
}

}  // namespace
