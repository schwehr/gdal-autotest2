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
// Tests 1-bit GeoTIFF bitmap raster band color table and internal mask
// behavior (gtiffbitmapband.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/gtiff/gtiffbitmapband.cpp

#include <cstdint>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
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
    GDALRegister_GTiff();
    CPLErrorReset();
    driver_ = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, driver_);
  }

  void TearDown() override { CPLErrorReset(); }

  GDALDriver* driver_ = nullptr;
};

// Tests 1-bit MINISBLACK bitmap band (default synthesized black/white table).
TEST_F(GTiffBitmapBandTest, MinIsBlackAndColorInterp) {
  constexpr char kFilename[] = "/vsimem/bitmap_minisblack.tif";
  absl::Cleanup cleanup = [&]() { VSIUnlink(kFilename); };

  {
    CPLStringList options;
    options.AddNameValue("NBITS", "1");
    GDALDatasetUniquePtr ds(
        driver_->Create(kFilename, 16, 16, 1, GDT_Byte, options.List()));
    ASSERT_NE(nullptr, ds);

    std::vector<uint8_t> data(16 * 16, 0);
    data[0] = 1;
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 16, 16,
                                                      data.data(), 16, 16,
                                                      GDT_Byte, 0, 0, nullptr));
  }

  GDALDatasetUniquePtr read_ds(
      GDALDataset::Open(kFilename, GDAL_OF_RASTER | GDAL_OF_UPDATE));
  ASSERT_NE(nullptr, read_ds);

  GDALRasterBand* band = read_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  EXPECT_EQ(GDT_Byte, band->GetRasterDataType());
  EXPECT_EQ(GCI_PaletteIndex, band->GetColorInterpretation());

  GDALColorTable* ct = band->GetColorTable();
  ASSERT_NE(nullptr, ct);
  ASSERT_EQ(2, ct->GetColorEntryCount());
  EXPECT_EQ(0, ct->GetColorEntry(0)->c1);
  EXPECT_EQ(255, ct->GetColorEntry(1)->c1);

  // SetColorInterpretation with matching value and non-matching value.
  EXPECT_EQ(CE_None, band->SetColorInterpretation(GCI_PaletteIndex));
  EXPECT_EQ(CE_None, band->SetColorInterpretation(GCI_GrayIndex));
}

// Tests 1-bit MINISWHITE bitmap band (clones dataset-level inverted palette).
TEST_F(GTiffBitmapBandTest, MinIsWhiteClonesDatasetColorTable) {
  constexpr char kFilename[] = "/vsimem/bitmap_miniswhite.tif";
  absl::Cleanup cleanup = [&]() { VSIUnlink(kFilename); };

  {
    CPLStringList options;
    options.AddNameValue("NBITS", "1");
    options.AddNameValue("PHOTOMETRIC", "MINISWHITE");
    GDALDatasetUniquePtr ds(
        driver_->Create(kFilename, 16, 16, 1, GDT_Byte, options.List()));
    ASSERT_NE(nullptr, ds);
  }

  GDALDatasetUniquePtr read_ds(
      GDALDataset::Open(kFilename, GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, read_ds);

  GDALRasterBand* band = read_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  GDALColorTable* ct = band->GetColorTable();
  ASSERT_NE(nullptr, ct);
  ASSERT_EQ(2, ct->GetColorEntryCount());
  EXPECT_EQ(255, ct->GetColorEntry(0)->c1);
  EXPECT_EQ(0, ct->GetColorEntry(1)->c1);
}

// Tests internal mask bitmap band where m_bPromoteTo8Bits is true.
TEST_F(GTiffBitmapBandTest, InternalMaskPromoteTo8Bits) {
  constexpr char kFilename[] = "/vsimem/bitmap_internal_mask.tif";
  absl::Cleanup cleanup = [&]() { VSIUnlink(kFilename); };

  CPLConfigOptionSetter internal_mask("GDAL_TIFF_INTERNAL_MASK", "YES", false);
  {
    GDALDatasetUniquePtr ds(
        driver_->Create(kFilename, 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(CE_None, ds->CreateMaskBand(GMF_PER_DATASET));

    GDALRasterBand* mask_band = ds->GetRasterBand(1)->GetMaskBand();
    ASSERT_NE(nullptr, mask_band);
    EXPECT_EQ(GCI_Undefined, mask_band->GetColorInterpretation());
    EXPECT_EQ(nullptr, mask_band->GetColorTable());
  }

  GDALDatasetUniquePtr read_ds(
      GDALDataset::Open(kFilename, GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, read_ds);

  GDALRasterBand* read_mask = read_ds->GetRasterBand(1)->GetMaskBand();
  ASSERT_NE(nullptr, read_mask);
  EXPECT_EQ(GCI_Undefined, read_mask->GetColorInterpretation());
  EXPECT_EQ(nullptr, read_mask->GetColorTable());
}

// TODO: b/335317901 - Cover remaining defensive nullptr/error branches in
// gtiffbitmapband.cpp (~91.2% coverage today).
}  // namespace
}  // namespace autotest2
