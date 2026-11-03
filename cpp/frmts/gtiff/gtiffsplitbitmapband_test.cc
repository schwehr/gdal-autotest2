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
// Tests 1-bit split scanline bitmap band decoding, rewinding, coverage status,
// write rejection, and CCITTFAX4 EOF short-circuiting
// (gtiffsplitbitmapband.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/gtiff/gtiffsplitbitmapband.cpp

#include <cstdint>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

class GTiffSplitBitmapBandTest : public ::testing::Test {
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

// Tests normal 1-bit split bitmap band operations: read, advance, rewind,
// coverage status, and write rejection.
TEST_F(GTiffSplitBitmapBandTest, ReadRewindCoverageAndWriteReject) {
  constexpr char kFilename[] = "/vsimem/split_bitmap.tif";
  absl::Cleanup cleanup = [&]() { VSIUnlink(kFilename); };

  constexpr int kWidth = 16;
  constexpr int kHeight = 2001;

  {
    CPLStringList options;
    options.AddNameValue("NBITS", "1");
    options.AddNameValue("COMPRESS", "PACKBITS");
    options.AddNameValue("BLOCKYSIZE", "2001");
    GDALDatasetUniquePtr ds(driver_->Create(kFilename, kWidth, kHeight, 1,
                                            GDT_Byte, options.List()));
    ASSERT_NE(nullptr, ds);

    std::vector<uint8_t> full_img(kWidth * kHeight, 0);
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        full_img[y * kWidth + x] = static_cast<uint8_t>((x + y) % 2);
      }
    }
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, kWidth, kHeight, full_img.data(),
                           kWidth, kHeight, GDT_Byte, 0, 0, nullptr));
  }

  GDALDatasetUniquePtr read_ds(
      GDALDataset::Open(kFilename, GDAL_OF_RASTER | GDAL_OF_UPDATE));
  ASSERT_NE(nullptr, read_ds);

  GDALRasterBand* band = read_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  int block_x = 0;
  int block_y = 0;
  band->GetBlockSize(&block_x, &block_y);
  ASSERT_EQ(kWidth, block_x);
  ASSERT_EQ(1, block_y);

  int status = band->GetDataCoverageStatus(0, 0, 1, 1, 0, nullptr);
  EXPECT_NE(0, status & GDAL_DATA_COVERAGE_STATUS_DATA);

  // WriteBlock delegates to GTiffSplitBitmapBand::IWriteBlock and fails.
  std::vector<uint8_t> buf(kWidth, 0);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure, band->WriteBlock(0, 0, buf.data()));
    CPLErrorReset();
  }

  // Read line 0 (unpacks both 0 and 1 bits).
  ASSERT_EQ(CE_None, band->ReadBlock(0, 0, buf.data()));
  for (int x = 0; x < kWidth; ++x) {
    EXPECT_EQ(static_cast<uint8_t>(x % 2), buf[x]) << "x=" << x;
  }

  // Advance to line 5.
  ASSERT_EQ(CE_None, band->ReadBlock(0, 5, buf.data()));
  for (int x = 0; x < kWidth; ++x) {
    EXPECT_EQ(static_cast<uint8_t>((x + 5) % 2), buf[x]) << "x=" << x;
  }

  // Rewind to line 2.
  ASSERT_EQ(CE_None, band->ReadBlock(0, 2, buf.data()));
  for (int x = 0; x < kWidth; ++x) {
    EXPECT_EQ(static_cast<uint8_t>((x + 2) % 2), buf[x]) << "x=" << x;
  }
}

// Tests CCITTFAX4 Premature EOF handling and m_nLastLineValid short-circuit.
TEST_F(GTiffSplitBitmapBandTest, FaxPrematureEofShortCircuitsSubsequentLines) {
  constexpr char kFilename[] = "/vsimem/split_bitmap_fax_eof.tif";
  absl::Cleanup cleanup = [&]() { VSIUnlink(kFilename); };

  constexpr int kWidth = 64;
  constexpr int kHeight = 2001;

  {
    CPLStringList options;
    options.AddNameValue("NBITS", "1");
    options.AddNameValue("COMPRESS", "CCITTFAX4");
    options.AddNameValue("BLOCKYSIZE", "2001");
    GDALDatasetUniquePtr ds(driver_->Create(kFilename, kWidth, kHeight, 1,
                                            GDT_Byte, options.List()));
    ASSERT_NE(nullptr, ds);

    std::vector<uint8_t> full_img(kWidth * kHeight, 0);
    for (size_t i = 0; i < full_img.size(); ++i) {
      full_img[i] = static_cast<uint8_t>(((i * 37) ^ (i >> 3)) & 1);
    }
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, kWidth, kHeight, full_img.data(),
                           kWidth, kHeight, GDT_Byte, 0, 0, nullptr));
  }

  GDALDatasetUniquePtr read_ds(
      GDALDataset::Open(kFilename, GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, read_ds);

  GDALRasterBand* band = read_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  int block_x = 0;
  int block_y = 0;
  band->GetBlockSize(&block_x, &block_y);
  ASSERT_EQ(1, block_y);

  // Truncate the CCITTFAX4 strip in memory so Fax4Decode emits "Premature EOF".
  VSIStatBufL st;
  ASSERT_EQ(0, VSIStatL(kFilename, &st));
  ASSERT_GT(st.st_size, 1000u);

  {
    VSILFILE* fp = VSIFOpenL(kFilename, "r+");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup close_fp = [&]() {
      if (fp != nullptr) VSIFCloseL(fp);
    };
    ASSERT_EQ(0, VSIFTruncateL(fp, st.st_size / 2));
    ASSERT_EQ(0, VSIFCloseL(fp));
    fp = nullptr;
  }

  WithQuietHandler quiet;
  std::vector<uint8_t> buf(kWidth, 0);
  // Reading near the end hits Premature EOF and sets m_nLastLineValid.
  EXPECT_EQ(CE_Failure, band->ReadBlock(0, 1990, buf.data()));
  // Subsequent read past m_nLastLineValid immediately returns CE_Failure.
  EXPECT_EQ(CE_Failure, band->ReadBlock(0, 1995, buf.data()));
  CPLErrorReset();
}

// TODO: b/335317901 - Cover remaining defensive I/O failure branches in
// gtiffsplitbitmapband.cpp (~91.7% coverage today).
}  // namespace
}  // namespace autotest2
