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

#include <cstdint>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

class GTiffBitmapBandTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALAllRegister();
    driver_ = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(driver_, nullptr);
  }

  void TearDown() override { CPLErrorReset(); }

  GDALDriver* driver_ = nullptr;
};

// Tests 1-bit MINISBLACK bitmap band (default synthesized black/white table).
TEST_F(GTiffBitmapBandTest, MinIsBlackAndColorInterp) {
  constexpr char kFilename[] = "/vsimem/bitmap_minisblack.tif";
  absl::Cleanup cleanup = [&]() { VSIUnlink(kFilename); };

  char** options = nullptr;
  options = CSLSetNameValue(options, "NBITS", "1");
  GDALDataset* ds = driver_->Create(kFilename, 16, 16, 1, GDT_Byte, options);
  CSLDestroy(options);
  ASSERT_NE(ds, nullptr);

  std::vector<uint8_t> data(16 * 16, 0);
  data[0] = 1;
  ASSERT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 16, 16, data.data(),
                                           16, 16, GDT_Byte, 0, 0, nullptr),
            CE_None);
  GDALClose(ds);

  GDALDataset* read_ds =
      static_cast<GDALDataset*>(GDALOpen(kFilename, GA_Update));
  ASSERT_NE(read_ds, nullptr);
  absl::Cleanup close_read_ds = [&]() { GDALClose(read_ds); };

  GDALRasterBand* band = read_ds->GetRasterBand(1);
  ASSERT_NE(band, nullptr);
  EXPECT_EQ(band->GetRasterDataType(), GDT_Byte);
  EXPECT_EQ(band->GetColorInterpretation(), GCI_PaletteIndex);

  GDALColorTable* ct = band->GetColorTable();
  ASSERT_NE(ct, nullptr);
  ASSERT_EQ(ct->GetColorEntryCount(), 2);
  EXPECT_EQ(ct->GetColorEntry(0)->c1, 0);
  EXPECT_EQ(ct->GetColorEntry(1)->c1, 255);

  // SetColorInterpretation with matching value and non-matching value.
  EXPECT_EQ(band->SetColorInterpretation(GCI_PaletteIndex), CE_None);
  EXPECT_EQ(band->SetColorInterpretation(GCI_GrayIndex), CE_None);
}

// Tests 1-bit MINISWHITE bitmap band (clones dataset-level inverted palette).
TEST_F(GTiffBitmapBandTest, MinIsWhiteClonesDatasetColorTable) {
  constexpr char kFilename[] = "/vsimem/bitmap_miniswhite.tif";
  absl::Cleanup cleanup = [&]() { VSIUnlink(kFilename); };

  char** options = nullptr;
  options = CSLSetNameValue(options, "NBITS", "1");
  options = CSLSetNameValue(options, "PHOTOMETRIC", "MINISWHITE");
  GDALDataset* ds = driver_->Create(kFilename, 16, 16, 1, GDT_Byte, options);
  CSLDestroy(options);
  ASSERT_NE(ds, nullptr);
  GDALClose(ds);

  GDALDataset* read_ds =
      static_cast<GDALDataset*>(GDALOpen(kFilename, GA_ReadOnly));
  ASSERT_NE(read_ds, nullptr);
  absl::Cleanup close_read_ds = [&]() { GDALClose(read_ds); };

  GDALRasterBand* band = read_ds->GetRasterBand(1);
  ASSERT_NE(band, nullptr);
  GDALColorTable* ct = band->GetColorTable();
  ASSERT_NE(ct, nullptr);
  ASSERT_EQ(ct->GetColorEntryCount(), 2);
  EXPECT_EQ(ct->GetColorEntry(0)->c1, 255);
  EXPECT_EQ(ct->GetColorEntry(1)->c1, 0);
}

// Tests internal mask bitmap band where m_bPromoteTo8Bits is true.
TEST_F(GTiffBitmapBandTest, InternalMaskPromoteTo8Bits) {
  constexpr char kFilename[] = "/vsimem/bitmap_internal_mask.tif";
  absl::Cleanup cleanup = [&]() { VSIUnlink(kFilename); };

  CPLConfigOptionSetter internal_mask("GDAL_TIFF_INTERNAL_MASK", "YES", false);
  GDALDataset* ds = driver_->Create(kFilename, 16, 16, 1, GDT_Byte, nullptr);
  ASSERT_NE(ds, nullptr);
  ASSERT_EQ(ds->CreateMaskBand(GMF_PER_DATASET), CE_None);

  GDALRasterBand* mask_band = ds->GetRasterBand(1)->GetMaskBand();
  ASSERT_NE(mask_band, nullptr);
  EXPECT_EQ(mask_band->GetColorInterpretation(), GCI_Undefined);
  EXPECT_EQ(mask_band->GetColorTable(), nullptr);
  GDALClose(ds);

  GDALDataset* read_ds =
      static_cast<GDALDataset*>(GDALOpen(kFilename, GA_ReadOnly));
  ASSERT_NE(read_ds, nullptr);
  absl::Cleanup close_read_ds = [&]() { GDALClose(read_ds); };

  GDALRasterBand* read_mask = read_ds->GetRasterBand(1)->GetMaskBand();
  ASSERT_NE(read_mask, nullptr);
  EXPECT_EQ(read_mask->GetColorInterpretation(), GCI_Undefined);
  EXPECT_EQ(read_mask->GetColorTable(), nullptr);
}

}  // namespace
}  // namespace autotest2
