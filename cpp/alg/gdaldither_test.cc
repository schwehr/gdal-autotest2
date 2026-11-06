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
// Tests Floyd-Steinberg dithering from 24-bit RGB to 8-bit pseudo-color
// (alg/gdaldither.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/gdaldither.cpp

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/alg/gdal_alg_priv.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_colortable.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

// Custom raster band to simulate read and write failures.
class FailableRasterBand final : public GDALRasterBand {
 public:
  FailableRasterBand(GDALDataset* poDSIn, int nXSize, int nYSize) {
    poDS = poDSIn;
    nBand = 1;
    eDataType = GDT_Byte;
    nRasterXSize = nXSize;
    nRasterYSize = nYSize;
    nBlockXSize = nXSize;
    nBlockYSize = 1;
  }

  void SetFailRead(bool fail) { fail_read_ = fail; }
  void SetFailWrite(bool fail) { fail_write_ = fail; }
  void SetFillValue(GByte val) { fill_value_ = val; }

  CPLErr IRasterIO(GDALRWFlag eRWFlag, int /*nXOff*/, int /*nYOff*/,
                   int /*nXSize*/, int /*nYSize*/, void* pData, int nBufXSize,
                   int nBufYSize, GDALDataType eBufType,
                   GSpacing /*nPixelSpace*/, GSpacing /*nLineSpace*/,
                   GDALRasterIOExtraArg* /*psExtraArg*/) override {
    if (eRWFlag == GF_Read) {
      if (fail_read_) {
        CPLError(CE_Failure, CPLE_AppDefined, "Simulated read failure");
        return CE_Failure;
      }
      if (eBufType == GDT_Byte && pData != nullptr) {
        std::memset(pData, fill_value_,
                    static_cast<size_t>(nBufXSize) * nBufYSize);
      }
      return CE_None;
    }
    if (eRWFlag == GF_Write) {
      if (fail_write_) {
        CPLError(CE_Failure, CPLE_AppDefined, "Simulated write failure");
        return CE_Failure;
      }
      return CE_None;
    }
    return CE_Failure;
  }

  CPLErr IReadBlock(int /*nBlockXOff*/, int /*nBlockYOff*/,
                    void* pImage) override {
    if (fail_read_) {
      CPLError(CE_Failure, CPLE_AppDefined, "Simulated read failure");
      return CE_Failure;
    }
    std::memset(pImage, fill_value_, nBlockXSize);
    return CE_None;
  }

  CPLErr IWriteBlock(int /*nBlockXOff*/, int /*nBlockYOff*/,
                     void* /*pImage*/) override {
    if (fail_write_) {
      CPLError(CE_Failure, CPLE_AppDefined, "Simulated write failure");
      return CE_Failure;
    }
    return CE_None;
  }

 private:
  bool fail_read_ = false;
  bool fail_write_ = false;
  GByte fill_value_ = 0;
};

// Dataset managing a FailableRasterBand.
class FailableDataset final : public GDALDataset {
 public:
  FailableDataset(int nXSize, int nYSize) {
    nRasterXSize = nXSize;
    nRasterYSize = nYSize;
    SetBand(1, new FailableRasterBand(this, nXSize, nYSize));
  }

  FailableRasterBand* GetFailableBand() {
    return static_cast<FailableRasterBand*>(GetRasterBand(1));
  }
};

// Creates an in-memory dataset using the MEM driver.
GDALDatasetUniquePtr CreateMemDataset(int width, int height, int bands = 1,
                                      GDALDataType type = GDT_Byte) {
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  if (mem_driver == nullptr) {
    return nullptr;
  }
  return GDALDatasetUniquePtr(
      mem_driver->Create("", width, height, bands, type, nullptr));
}

// Creates a color table with a specified number of entries spread across RGB.
std::unique_ptr<GDALColorTable> CreateColorTable(int num_colors) {
  auto table = std::make_unique<GDALColorTable>(GPI_RGB);
  for (int i = 0; i < num_colors; ++i) {
    GDALColorEntry entry;
    entry.c1 = static_cast<short>((i * 37) % 256);
    entry.c2 = static_cast<short>((i * 73) % 256);
    entry.c3 = static_cast<short>((i * 109) % 256);
    entry.c4 = 255;
    table->SetColorEntry(i, &entry);
  }
  return table;
}

// Creates a standard 2-color black and white color table.
std::unique_ptr<GDALColorTable> CreateBlackAndWhiteColorTable() {
  auto table = std::make_unique<GDALColorTable>(GPI_RGB);
  GDALColorEntry black = {0, 0, 0, 255};
  GDALColorEntry white = {255, 255, 255, 255};
  table->SetColorEntry(0, &black);
  table->SetColorEntry(1, &white);
  return table;
}

// Creates an 8-color table containing black, red, green, blue, yellow, cyan,
// magenta, and white.
std::unique_ptr<GDALColorTable> CreateEightColorTable() {
  auto table = std::make_unique<GDALColorTable>(GPI_RGB);
  constexpr std::array<GDALColorEntry, 8> kColors = {{
      {0, 0, 0, 255},        // Black
      {255, 0, 0, 255},      // Red
      {0, 255, 0, 255},      // Green
      {0, 0, 255, 255},      // Blue
      {255, 255, 0, 255},    // Yellow
      {0, 255, 255, 255},    // Cyan
      {255, 0, 255, 255},    // Magenta
      {255, 255, 255, 255},  // White
  }};
  for (size_t i = 0; i < kColors.size(); ++i) {
    table->SetColorEntry(static_cast<int>(i), &kColors[i]);
  }
  return table;
}

class GdalDitherTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_MEM(); }
};

// 1. Validation / Null Pointer Checks
TEST_F(GdalDitherTest, NullPointers) {
  auto rgb_ds = CreateMemDataset(4, 4, 3);
  ASSERT_NE(nullptr, rgb_ds);
  auto target_ds = CreateMemDataset(4, 4, 1);
  ASSERT_NE(nullptr, target_ds);
  auto ct = CreateColorTable(2);

  GDALRasterBandH r = rgb_ds->GetRasterBand(1);
  GDALRasterBandH g = rgb_ds->GetRasterBand(2);
  GDALRasterBandH b = rgb_ds->GetRasterBand(3);
  GDALRasterBandH target = target_ds->GetRasterBand(1);
  GDALColorTableH table = ct.get();

  // Null Red band
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALDitherRGB2PCT(nullptr, g, b, target, table,
                                            nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Null Green band
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALDitherRGB2PCT(r, nullptr, b, target, table,
                                            nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Null Blue band
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALDitherRGB2PCT(r, g, nullptr, target, table,
                                            nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Null Target band
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALDitherRGB2PCT(r, g, b, nullptr, table, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Null Color table
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALDitherRGB2PCT(r, g, b, target, nullptr, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

// 2. Band Dimension Mismatch Checks
TEST_F(GdalDitherTest, SourceDimensionMismatch) {
  auto r_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, r_ds);
  auto target_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, target_ds);
  auto ct = CreateColorTable(4);

  // Green X mismatch
  {
    auto g_ds = CreateMemDataset(11, 10, 1);
    auto b_ds = CreateMemDataset(10, 10, 1);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(
        CE_Failure,
        GDALDitherRGB2PCT(r_ds->GetRasterBand(1), g_ds->GetRasterBand(1),
                          b_ds->GetRasterBand(1), target_ds->GetRasterBand(1),
                          ct.get(), nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Green or blue band doesn't match size of red band"));
    CPLErrorReset();
  }

  // Green Y mismatch
  {
    auto g_ds = CreateMemDataset(10, 12, 1);
    auto b_ds = CreateMemDataset(10, 10, 1);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(
        CE_Failure,
        GDALDitherRGB2PCT(r_ds->GetRasterBand(1), g_ds->GetRasterBand(1),
                          b_ds->GetRasterBand(1), target_ds->GetRasterBand(1),
                          ct.get(), nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Green or blue band doesn't match size of red band"));
    CPLErrorReset();
  }

  // Blue X mismatch
  {
    auto g_ds = CreateMemDataset(10, 10, 1);
    auto b_ds = CreateMemDataset(12, 10, 1);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(
        CE_Failure,
        GDALDitherRGB2PCT(r_ds->GetRasterBand(1), g_ds->GetRasterBand(1),
                          b_ds->GetRasterBand(1), target_ds->GetRasterBand(1),
                          ct.get(), nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Green or blue band doesn't match size of red band"));
    CPLErrorReset();
  }

  // Blue Y mismatch
  {
    auto g_ds = CreateMemDataset(10, 10, 1);
    auto b_ds = CreateMemDataset(10, 13, 1);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(
        CE_Failure,
        GDALDitherRGB2PCT(r_ds->GetRasterBand(1), g_ds->GetRasterBand(1),
                          b_ds->GetRasterBand(1), target_ds->GetRasterBand(1),
                          ct.get(), nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Green or blue band doesn't match size of red band"));
    CPLErrorReset();
  }

  // Target X mismatch
  {
    auto g_ds = CreateMemDataset(10, 10, 1);
    auto b_ds = CreateMemDataset(10, 10, 1);
    auto bad_target = CreateMemDataset(14, 10, 1);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(
        CE_Failure,
        GDALDitherRGB2PCT(r_ds->GetRasterBand(1), g_ds->GetRasterBand(1),
                          b_ds->GetRasterBand(1), bad_target->GetRasterBand(1),
                          ct.get(), nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Target band doesn't match size of source bands"));
    CPLErrorReset();
  }

  // Target Y mismatch
  {
    auto g_ds = CreateMemDataset(10, 10, 1);
    auto b_ds = CreateMemDataset(10, 10, 1);
    auto bad_target = CreateMemDataset(10, 15, 1);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(
        CE_Failure,
        GDALDitherRGB2PCT(r_ds->GetRasterBand(1), g_ds->GetRasterBand(1),
                          b_ds->GetRasterBand(1), bad_target->GetRasterBand(1),
                          ct.get(), nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Target band doesn't match size of source bands"));
    CPLErrorReset();
  }
}

// 3. Color Table Size Validation
TEST_F(GdalDitherTest, EmptyColorTable) {
  auto rgb_ds = CreateMemDataset(4, 4, 3);
  ASSERT_NE(nullptr, rgb_ds);
  auto target_ds = CreateMemDataset(4, 4, 1);
  ASSERT_NE(nullptr, target_ds);
  GDALColorTable empty_table(GPI_RGB);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(
      CE_Failure,
      GDALDitherRGB2PCT(rgb_ds->GetRasterBand(1), rgb_ds->GetRasterBand(2),
                        rgb_ds->GetRasterBand(3), target_ds->GetRasterBand(1),
                        &empty_table, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Color table must not be empty"));
  CPLErrorReset();
}

TEST_F(GdalDitherTest, ColorTableTooManyEntries) {
  auto rgb_ds = CreateMemDataset(4, 4, 3);
  ASSERT_NE(nullptr, rgb_ds);
  auto target_ds = CreateMemDataset(4, 4, 1);
  ASSERT_NE(nullptr, target_ds);
  auto ct = CreateColorTable(257);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(
      CE_Failure,
      GDALDitherRGB2PCT(rgb_ds->GetRasterBand(1), rgb_ds->GetRasterBand(2),
                        rgb_ds->GetRasterBand(3), target_ds->GetRasterBand(1),
                        ct.get(), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Color table cannot have more than 256 entries"));
  CPLErrorReset();
}

// 4. Progress Callback Checks
TEST_F(GdalDitherTest, ProgressCallbackNullSucceeds) {
  auto rgb_ds = CreateMemDataset(4, 4, 3);
  ASSERT_NE(nullptr, rgb_ds);
  auto target_ds = CreateMemDataset(4, 4, 1);
  ASSERT_NE(nullptr, target_ds);
  auto ct = CreateBlackAndWhiteColorTable();

  EXPECT_EQ(CE_None, GDALDitherRGB2PCT(
                         rgb_ds->GetRasterBand(1), rgb_ds->GetRasterBand(2),
                         rgb_ds->GetRasterBand(3), target_ds->GetRasterBand(1),
                         ct.get(), nullptr, nullptr));
}

TEST_F(GdalDitherTest, ProgressInterruptedFirstScanline) {
  auto rgb_ds = CreateMemDataset(4, 4, 3);
  ASSERT_NE(nullptr, rgb_ds);
  auto target_ds = CreateMemDataset(4, 4, 1);
  ASSERT_NE(nullptr, target_ds);
  auto ct = CreateBlackAndWhiteColorTable();

  auto cancel_callback = [](double /*dfComplete*/, const char* /*pszMessage*/,
                            void* /*pProgressArg*/) -> int { return 0; };

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(
      CE_Failure,
      GDALDitherRGB2PCT(rgb_ds->GetRasterBand(1), rgb_ds->GetRasterBand(2),
                        rgb_ds->GetRasterBand(3), target_ds->GetRasterBand(1),
                        ct.get(), cancel_callback, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_UserInterrupt, CPLGetLastErrorNo());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("User Terminated"));
  CPLErrorReset();
}

TEST_F(GdalDitherTest, ProgressInterruptedMidway) {
  auto rgb_ds = CreateMemDataset(4, 4, 3);
  ASSERT_NE(nullptr, rgb_ds);
  auto target_ds = CreateMemDataset(4, 4, 1);
  ASSERT_NE(nullptr, target_ds);
  auto ct = CreateBlackAndWhiteColorTable();

  int call_count = 0;
  auto cancel_on_second = [](double /*dfComplete*/, const char* /*pszMessage*/,
                             void* pProgressArg) -> int {
    int* count = static_cast<int*>(pProgressArg);
    (*count)++;
    return (*count <= 1) ? 1 : 0;
  };

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(
      CE_Failure,
      GDALDitherRGB2PCT(rgb_ds->GetRasterBand(1), rgb_ds->GetRasterBand(2),
                        rgb_ds->GetRasterBand(3), target_ds->GetRasterBand(1),
                        ct.get(), cancel_on_second, &call_count));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_UserInterrupt, CPLGetLastErrorNo());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("User Terminated"));
  EXPECT_EQ(2, call_count);
  CPLErrorReset();
}

TEST_F(GdalDitherTest, ProgressTracking) {
  auto rgb_ds = CreateMemDataset(4, 4, 3);
  ASSERT_NE(nullptr, rgb_ds);
  auto target_ds = CreateMemDataset(4, 4, 1);
  ASSERT_NE(nullptr, target_ds);
  auto ct = CreateBlackAndWhiteColorTable();

  std::vector<double> progress_values;
  auto track_progress = [](double dfComplete, const char* /*pszMessage*/,
                           void* pProgressArg) -> int {
    auto* values = static_cast<std::vector<double>*>(pProgressArg);
    values->push_back(dfComplete);
    return 1;
  };

  EXPECT_EQ(CE_None, GDALDitherRGB2PCT(
                         rgb_ds->GetRasterBand(1), rgb_ds->GetRasterBand(2),
                         rgb_ds->GetRasterBand(3), target_ds->GetRasterBand(1),
                         ct.get(), track_progress, &progress_values));

  // 4 scanlines + final 1.0 progress report = 5 calls
  ASSERT_EQ(5, progress_values.size());
  EXPECT_DOUBLE_EQ(0.0, progress_values[0]);
  EXPECT_DOUBLE_EQ(0.25, progress_values[1]);
  EXPECT_DOUBLE_EQ(0.5, progress_values[2]);
  EXPECT_DOUBLE_EQ(0.75, progress_values[3]);
  EXPECT_DOUBLE_EQ(1.0, progress_values[4]);
}

// 5. Simulated Read and Write Failures
TEST_F(GdalDitherTest, FailReadRedBand) {
  auto r_ds = std::make_unique<FailableDataset>(4, 4);
  auto g_ds = std::make_unique<FailableDataset>(4, 4);
  auto b_ds = std::make_unique<FailableDataset>(4, 4);
  auto target_ds = std::make_unique<FailableDataset>(4, 4);
  auto ct = CreateBlackAndWhiteColorTable();

  r_ds->GetFailableBand()->SetFailRead(true);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GDALDitherRGB2PCT(
                            r_ds->GetRasterBand(1), g_ds->GetRasterBand(1),
                            b_ds->GetRasterBand(1), target_ds->GetRasterBand(1),
                            ct.get(), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST_F(GdalDitherTest, FailReadGreenBand) {
  auto r_ds = std::make_unique<FailableDataset>(4, 4);
  auto g_ds = std::make_unique<FailableDataset>(4, 4);
  auto b_ds = std::make_unique<FailableDataset>(4, 4);
  auto target_ds = std::make_unique<FailableDataset>(4, 4);
  auto ct = CreateBlackAndWhiteColorTable();

  g_ds->GetFailableBand()->SetFailRead(true);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GDALDitherRGB2PCT(
                            r_ds->GetRasterBand(1), g_ds->GetRasterBand(1),
                            b_ds->GetRasterBand(1), target_ds->GetRasterBand(1),
                            ct.get(), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST_F(GdalDitherTest, FailReadBlueBand) {
  auto r_ds = std::make_unique<FailableDataset>(4, 4);
  auto g_ds = std::make_unique<FailableDataset>(4, 4);
  auto b_ds = std::make_unique<FailableDataset>(4, 4);
  auto target_ds = std::make_unique<FailableDataset>(4, 4);
  auto ct = CreateBlackAndWhiteColorTable();

  b_ds->GetFailableBand()->SetFailRead(true);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GDALDitherRGB2PCT(
                            r_ds->GetRasterBand(1), g_ds->GetRasterBand(1),
                            b_ds->GetRasterBand(1), target_ds->GetRasterBand(1),
                            ct.get(), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST_F(GdalDitherTest, FailWriteTargetBand) {
  auto r_ds = std::make_unique<FailableDataset>(4, 4);
  auto g_ds = std::make_unique<FailableDataset>(4, 4);
  auto b_ds = std::make_unique<FailableDataset>(4, 4);
  auto target_ds = std::make_unique<FailableDataset>(4, 4);
  auto ct = CreateBlackAndWhiteColorTable();

  target_ds->GetFailableBand()->SetFailWrite(true);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GDALDitherRGB2PCT(
                            r_ds->GetRasterBand(1), g_ds->GetRasterBand(1),
                            b_ds->GetRasterBand(1), target_ds->GetRasterBand(1),
                            ct.get(), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

// 6. Various Color Table Sizes and SSE2 Multiple-of-8 Padding
TEST_F(GdalDitherTest, ColorTableSizesAndPadding) {
  constexpr std::array<int, 7> kColorCounts = {1, 2, 7, 8, 254, 255, 256};
  for (int num_colors : kColorCounts) {
    auto rgb_ds = CreateMemDataset(4, 4, 3);
    ASSERT_NE(nullptr, rgb_ds);
    auto target_ds = CreateMemDataset(4, 4, 1);
    ASSERT_NE(nullptr, target_ds);
    auto ct = CreateColorTable(num_colors);

    EXPECT_EQ(
        CE_None,
        GDALDitherRGB2PCT(rgb_ds->GetRasterBand(1), rgb_ds->GetRasterBand(2),
                          rgb_ds->GetRasterBand(3), target_ds->GetRasterBand(1),
                          ct.get(), nullptr, nullptr));

    std::vector<GByte> output(16, 255);
    EXPECT_EQ(CE_None,
              target_ds->GetRasterBand(1)->RasterIO(
                  GF_Read, 0, 0, 4, 4, output.data(), 4, 4, GDT_Byte, 0, 0));
    for (GByte pixel : output) {
      EXPECT_LT(pixel, num_colors);
    }
  }
}

// 7. Various Image Dimensions
TEST_F(GdalDitherTest, ImageDimensions) {
  struct Dimension {
    int width;
    int height;
  };
  constexpr std::array<Dimension, 5> kDimensions = {{
      {1, 1},    // Single pixel
      {1, 16},   // Narrow column
      {16, 1},   // Wide row
      {8, 8},    // Small square
      {32, 32},  // Larger square
  }};

  auto ct = CreateEightColorTable();

  for (const auto& dim : kDimensions) {
    auto rgb_ds = CreateMemDataset(dim.width, dim.height, 3);
    ASSERT_NE(nullptr, rgb_ds);
    auto target_ds = CreateMemDataset(dim.width, dim.height, 1);
    ASSERT_NE(nullptr, target_ds);

    EXPECT_EQ(
        CE_None,
        GDALDitherRGB2PCT(rgb_ds->GetRasterBand(1), rgb_ds->GetRasterBand(2),
                          rgb_ds->GetRasterBand(3), target_ds->GetRasterBand(1),
                          ct.get(), nullptr, nullptr));

    const size_t total_pixels = static_cast<size_t>(dim.width) * dim.height;
    std::vector<GByte> output(total_pixels, 255);
    EXPECT_EQ(CE_None, target_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, dim.width, dim.height, output.data(),
                           dim.width, dim.height, GDT_Byte, 0, 0));
    for (GByte pixel : output) {
      EXPECT_LT(pixel, 8);
    }
  }
}

// 8. Deterministic Dithering and Error Diffusion Verification
TEST_F(GdalDitherTest, DeterministicValuesBlackWhiteAndGray) {
  constexpr int kWidth = 4;
  constexpr int kHeight = 4;
  auto ct = CreateBlackAndWhiteColorTable();

  // All black image -> all 0s
  {
    auto rgb_ds = CreateMemDataset(kWidth, kHeight, 3);
    ASSERT_NE(nullptr, rgb_ds);
    auto target_ds = CreateMemDataset(kWidth, kHeight, 1);
    ASSERT_NE(nullptr, target_ds);

    std::vector<GByte> black_data(kWidth * kHeight, 0);
    EXPECT_EQ(CE_None, rgb_ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, kWidth, kHeight, black_data.data(),
                           kWidth, kHeight, GDT_Byte, 0, 0));
    EXPECT_EQ(CE_None, rgb_ds->GetRasterBand(2)->RasterIO(
                           GF_Write, 0, 0, kWidth, kHeight, black_data.data(),
                           kWidth, kHeight, GDT_Byte, 0, 0));
    EXPECT_EQ(CE_None, rgb_ds->GetRasterBand(3)->RasterIO(
                           GF_Write, 0, 0, kWidth, kHeight, black_data.data(),
                           kWidth, kHeight, GDT_Byte, 0, 0));

    EXPECT_EQ(
        CE_None,
        GDALDitherRGB2PCT(rgb_ds->GetRasterBand(1), rgb_ds->GetRasterBand(2),
                          rgb_ds->GetRasterBand(3), target_ds->GetRasterBand(1),
                          ct.get(), nullptr, nullptr));

    std::vector<GByte> output(kWidth * kHeight, 255);
    EXPECT_EQ(CE_None, target_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, kWidth, kHeight, output.data(),
                           kWidth, kHeight, GDT_Byte, 0, 0));
    for (GByte pixel : output) {
      EXPECT_EQ(0, pixel);
    }
  }

  // All white image -> all 1s
  {
    auto rgb_ds = CreateMemDataset(kWidth, kHeight, 3);
    ASSERT_NE(nullptr, rgb_ds);
    auto target_ds = CreateMemDataset(kWidth, kHeight, 1);
    ASSERT_NE(nullptr, target_ds);

    std::vector<GByte> white_data(kWidth * kHeight, 255);
    EXPECT_EQ(CE_None, rgb_ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, kWidth, kHeight, white_data.data(),
                           kWidth, kHeight, GDT_Byte, 0, 0));
    EXPECT_EQ(CE_None, rgb_ds->GetRasterBand(2)->RasterIO(
                           GF_Write, 0, 0, kWidth, kHeight, white_data.data(),
                           kWidth, kHeight, GDT_Byte, 0, 0));
    EXPECT_EQ(CE_None, rgb_ds->GetRasterBand(3)->RasterIO(
                           GF_Write, 0, 0, kWidth, kHeight, white_data.data(),
                           kWidth, kHeight, GDT_Byte, 0, 0));

    EXPECT_EQ(
        CE_None,
        GDALDitherRGB2PCT(rgb_ds->GetRasterBand(1), rgb_ds->GetRasterBand(2),
                          rgb_ds->GetRasterBand(3), target_ds->GetRasterBand(1),
                          ct.get(), nullptr, nullptr));

    std::vector<GByte> output(kWidth * kHeight, 0);
    EXPECT_EQ(CE_None, target_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, kWidth, kHeight, output.data(),
                           kWidth, kHeight, GDT_Byte, 0, 0));
    for (GByte pixel : output) {
      EXPECT_EQ(1, pixel);
    }
  }

  // Uniform 50% gray image -> dithering produces both 0s and 1s
  {
    auto rgb_ds = CreateMemDataset(kWidth, kHeight, 3);
    ASSERT_NE(nullptr, rgb_ds);
    auto target_ds = CreateMemDataset(kWidth, kHeight, 1);
    ASSERT_NE(nullptr, target_ds);

    std::vector<GByte> gray_data(kWidth * kHeight, 128);
    EXPECT_EQ(CE_None, rgb_ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, kWidth, kHeight, gray_data.data(),
                           kWidth, kHeight, GDT_Byte, 0, 0));
    EXPECT_EQ(CE_None, rgb_ds->GetRasterBand(2)->RasterIO(
                           GF_Write, 0, 0, kWidth, kHeight, gray_data.data(),
                           kWidth, kHeight, GDT_Byte, 0, 0));
    EXPECT_EQ(CE_None, rgb_ds->GetRasterBand(3)->RasterIO(
                           GF_Write, 0, 0, kWidth, kHeight, gray_data.data(),
                           kWidth, kHeight, GDT_Byte, 0, 0));

    EXPECT_EQ(
        CE_None,
        GDALDitherRGB2PCT(rgb_ds->GetRasterBand(1), rgb_ds->GetRasterBand(2),
                          rgb_ds->GetRasterBand(3), target_ds->GetRasterBand(1),
                          ct.get(), nullptr, nullptr));

    std::vector<GByte> output(kWidth * kHeight, 255);
    EXPECT_EQ(CE_None, target_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, kWidth, kHeight, output.data(),
                           kWidth, kHeight, GDT_Byte, 0, 0));

    int count0 = 0;
    int count1 = 0;
    for (GByte pixel : output) {
      if (pixel == 0) count0++;
      if (pixel == 1) count1++;
    }
    EXPECT_GT(count0, 0);
    EXPECT_GT(count1, 0);
    EXPECT_EQ(kWidth * kHeight, count0 + count1);
  }
}

// 9. GDALDitherRGB2PCTInternal: No Dithering (bDither = FALSE)
TEST_F(GdalDitherTest, InternalNoDither) {
  constexpr int kWidth = 4;
  constexpr int kHeight = 4;
  auto rgb_ds = CreateMemDataset(kWidth, kHeight, 3);
  ASSERT_NE(nullptr, rgb_ds);
  auto target_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, target_ds);
  auto ct = CreateBlackAndWhiteColorTable();

  // Uniform 50% gray image with bDither = FALSE maps uniformly to nearest color
  // without error diffusion.
  std::vector<GByte> gray_data(kWidth * kHeight, 128);
  EXPECT_EQ(CE_None, rgb_ds->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, kWidth, kHeight, gray_data.data(),
                         kWidth, kHeight, GDT_Byte, 0, 0));
  EXPECT_EQ(CE_None, rgb_ds->GetRasterBand(2)->RasterIO(
                         GF_Write, 0, 0, kWidth, kHeight, gray_data.data(),
                         kWidth, kHeight, GDT_Byte, 0, 0));
  EXPECT_EQ(CE_None, rgb_ds->GetRasterBand(3)->RasterIO(
                         GF_Write, 0, 0, kWidth, kHeight, gray_data.data(),
                         kWidth, kHeight, GDT_Byte, 0, 0));

  EXPECT_EQ(CE_None, GDALDitherRGB2PCTInternal(
                         rgb_ds->GetRasterBand(1), rgb_ds->GetRasterBand(2),
                         rgb_ds->GetRasterBand(3), target_ds->GetRasterBand(1),
                         ct.get(), /*nBits=*/5, /*pasDynamicColorMap=*/nullptr,
                         /*bDither=*/FALSE, nullptr, nullptr));

  std::vector<GByte> output(kWidth * kHeight, 255);
  EXPECT_EQ(CE_None, target_ds->GetRasterBand(1)->RasterIO(
                         GF_Read, 0, 0, kWidth, kHeight, output.data(), kWidth,
                         kHeight, GDT_Byte, 0, 0));

  // Without dithering, all pixels map to the same nearest entry (1 for 128).
  for (GByte pixel : output) {
    EXPECT_EQ(output[0], pixel);
  }
}

// 10. GDALDitherRGB2PCTInternal: Hash Table Dynamic Color Map (nBits = 8, Small
// Image)
TEST_F(GdalDitherTest, InternalDynamicColorMapSmallImageHash) {
  constexpr int kWidth = 8;
  constexpr int kHeight = 2;
  auto rgb_ds = CreateMemDataset(kWidth, kHeight, 3);
  ASSERT_NE(nullptr, rgb_ds);
  auto target_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, target_ds);
  auto ct = CreateEightColorTable();

  // Colors designed to exercise all hash table branches:
  // PRIME_FOR_65536 is 98317.
  // Group 1: 4 colors hashing to index 1000 % 98317
  // - Color 1 (code 1000): (232, 3, 0) -> Slot 1
  // - Color 2 (code 1000 + 98317 = 99317): (245, 131, 1) -> Slot 2
  // - Color 3 (code 1000 + 2*98317 = 197634): (2, 4, 3) -> Slot 3
  // - Color 4 (code 1000 + 3*98317 = 295951): (15, 132, 4) -> Collision probe
  // (+257 to 1257)
  //
  // Group 2: 4 colors hashing to index 98300 % 98317 (near PRIME_FOR_65536)
  // - Color 5 (code 98300): (252, 127, 1) -> Slot 1
  // - Color 6 (code 98300 + 98317 = 196617): (9, 0, 3) -> Slot 2
  // - Color 7 (code 98300 + 2*98317 = 294934): (22, 128, 4) -> Slot 3
  // - Color 8 (code 98300 + 3*98317 = 393251): (35, 0, 6) -> Collision probe
  // (98300 + 257 = 98557 >= 98317 -> wraps to 240)
  struct RGBPixel {
    GByte r, g, b;
  };
  constexpr std::array<RGBPixel, 8> kTestColors = {{
      {232, 3, 0},    // Group 1, Slot 1
      {245, 131, 1},  // Group 1, Slot 2
      {2, 4, 3},      // Group 1, Slot 3
      {15, 132, 4},   // Group 1, Collision step
      {252, 127, 1},  // Group 2, Slot 1
      {9, 0, 3},      // Group 2, Slot 2
      {22, 128, 4},   // Group 2, Slot 3
      {35, 0, 6},     // Group 2, Wrap-around step
  }};

  std::vector<GByte> red_plane(kWidth * kHeight);
  std::vector<GByte> green_plane(kWidth * kHeight);
  std::vector<GByte> blue_plane(kWidth * kHeight);

  // Line 0 populates hash table slots and triggers collision steps.
  // Line 1 repeats the exact same colors, testing hash table hit branches.
  for (int row = 0; row < kHeight; ++row) {
    for (int col = 0; col < kWidth; ++col) {
      const size_t idx = static_cast<size_t>(row) * kWidth + col;
      red_plane[idx] = kTestColors[col].r;
      green_plane[idx] = kTestColors[col].g;
      blue_plane[idx] = kTestColors[col].b;
    }
  }

  EXPECT_EQ(CE_None, rgb_ds->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, kWidth, kHeight, red_plane.data(),
                         kWidth, kHeight, GDT_Byte, 0, 0));
  EXPECT_EQ(CE_None, rgb_ds->GetRasterBand(2)->RasterIO(
                         GF_Write, 0, 0, kWidth, kHeight, green_plane.data(),
                         kWidth, kHeight, GDT_Byte, 0, 0));
  EXPECT_EQ(CE_None, rgb_ds->GetRasterBand(3)->RasterIO(
                         GF_Write, 0, 0, kWidth, kHeight, blue_plane.data(),
                         kWidth, kHeight, GDT_Byte, 0, 0));

  std::vector<int8_t> dynamic_buffer(MEDIAN_CUT_AND_DITHER_BUFFER_SIZE_65536);
  GInt16* dynamic_map = reinterpret_cast<GInt16*>(dynamic_buffer.data());

  EXPECT_EQ(CE_None,
            GDALDitherRGB2PCTInternal(
                rgb_ds->GetRasterBand(1), rgb_ds->GetRasterBand(2),
                rgb_ds->GetRasterBand(3), target_ds->GetRasterBand(1), ct.get(),
                /*nBits=*/8, dynamic_map, /*bDither=*/FALSE, nullptr, nullptr));

  std::vector<GByte> output(kWidth * kHeight, 255);
  EXPECT_EQ(CE_None, target_ds->GetRasterBand(1)->RasterIO(
                         GF_Read, 0, 0, kWidth, kHeight, output.data(), kWidth,
                         kHeight, GDT_Byte, 0, 0));

  // Verify that row 0 and row 1 produce identical results for the same colors.
  for (int col = 0; col < kWidth; ++col) {
    EXPECT_EQ(output[col], output[kWidth + col]);
    EXPECT_LT(output[col], 8);
  }
}

// 11. GDALDitherRGB2PCTInternal: Direct Array Dynamic Color Map (nBits = 7)
TEST_F(GdalDitherTest, InternalDynamicColorMapDirectArray) {
  constexpr int kWidth = 4;
  constexpr int kHeight = 2;
  auto rgb_ds = CreateMemDataset(kWidth, kHeight, 3);
  ASSERT_NE(nullptr, rgb_ds);
  auto target_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, target_ds);
  auto ct = CreateEightColorTable();

  // Repeating color (100, 100, 100) tests both the uncached (*psIndex < 0)
  // and cached (*psIndex >= 0) paths.
  std::vector<GByte> red_plane(kWidth * kHeight, 100);
  std::vector<GByte> green_plane(kWidth * kHeight, 100);
  std::vector<GByte> blue_plane(kWidth * kHeight, 100);

  EXPECT_EQ(CE_None, rgb_ds->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, kWidth, kHeight, red_plane.data(),
                         kWidth, kHeight, GDT_Byte, 0, 0));
  EXPECT_EQ(CE_None, rgb_ds->GetRasterBand(2)->RasterIO(
                         GF_Write, 0, 0, kWidth, kHeight, green_plane.data(),
                         kWidth, kHeight, GDT_Byte, 0, 0));
  EXPECT_EQ(CE_None, rgb_ds->GetRasterBand(3)->RasterIO(
                         GF_Write, 0, 0, kWidth, kHeight, blue_plane.data(),
                         kWidth, kHeight, GDT_Byte, 0, 0));

  std::vector<GInt16> direct_map(256 * 256 * 256);

  // Test without dithering
  EXPECT_EQ(CE_None, GDALDitherRGB2PCTInternal(
                         rgb_ds->GetRasterBand(1), rgb_ds->GetRasterBand(2),
                         rgb_ds->GetRasterBand(3), target_ds->GetRasterBand(1),
                         ct.get(), /*nBits=*/7, direct_map.data(),
                         /*bDither=*/FALSE, nullptr, nullptr));

  std::vector<GByte> output(kWidth * kHeight, 255);
  EXPECT_EQ(CE_None, target_ds->GetRasterBand(1)->RasterIO(
                         GF_Read, 0, 0, kWidth, kHeight, output.data(), kWidth,
                         kHeight, GDT_Byte, 0, 0));

  for (GByte pixel : output) {
    EXPECT_EQ(output[0], pixel);
    EXPECT_LT(pixel, 8);
  }

  // Test with dithering
  EXPECT_EQ(CE_None, GDALDitherRGB2PCTInternal(
                         rgb_ds->GetRasterBand(1), rgb_ds->GetRasterBand(2),
                         rgb_ds->GetRasterBand(3), target_ds->GetRasterBand(1),
                         ct.get(), /*nBits=*/7, direct_map.data(),
                         /*bDither=*/TRUE, nullptr, nullptr));

  EXPECT_EQ(CE_None, target_ds->GetRasterBand(1)->RasterIO(
                         GF_Read, 0, 0, kWidth, kHeight, output.data(), kWidth,
                         kHeight, GDT_Byte, 0, 0));
  for (GByte pixel : output) {
    EXPECT_LT(pixel, 8);
  }
}

// 12. GDALDitherRGB2PCTInternal: Cube Resolution (nBits = 4 and nBits = 6)
TEST_F(GdalDitherTest, InternalCubeResolution) {
  constexpr int kWidth = 4;
  constexpr int kHeight = 4;
  auto rgb_ds = CreateMemDataset(kWidth, kHeight, 3);
  ASSERT_NE(nullptr, rgb_ds);
  auto target_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, target_ds);
  auto ct = CreateEightColorTable();

  // Test nBits = 4 (16^3 cube)
  EXPECT_EQ(CE_None, GDALDitherRGB2PCTInternal(
                         rgb_ds->GetRasterBand(1), rgb_ds->GetRasterBand(2),
                         rgb_ds->GetRasterBand(3), target_ds->GetRasterBand(1),
                         ct.get(), /*nBits=*/4, /*pasDynamicColorMap=*/nullptr,
                         /*bDither=*/TRUE, nullptr, nullptr));

  // Test nBits = 6 (64^3 cube)
  EXPECT_EQ(CE_None, GDALDitherRGB2PCTInternal(
                         rgb_ds->GetRasterBand(1), rgb_ds->GetRasterBand(2),
                         rgb_ds->GetRasterBand(3), target_ds->GetRasterBand(1),
                         ct.get(), /*nBits=*/6, /*pasDynamicColorMap=*/nullptr,
                         /*bDither=*/TRUE, nullptr, nullptr));
}

// 13. Extreme Error Clamping (testing std::max(0, std::min(255, ...)))
TEST_F(GdalDitherTest, ExtremeErrorClamping) {
  constexpr int kWidth = 8;
  constexpr int kHeight = 4;
  auto rgb_ds = CreateMemDataset(kWidth, kHeight, 3);
  ASSERT_NE(nullptr, rgb_ds);
  auto target_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, target_ds);
  auto ct = CreateBlackAndWhiteColorTable();

  // Alternating 0 and 255 creates large error swings that push unconstrained
  // values below 0 and above 255.
  std::vector<GByte> pattern(kWidth * kHeight);
  for (size_t i = 0; i < pattern.size(); ++i) {
    pattern[i] = (i % 2 == 0) ? 0 : 255;
  }

  EXPECT_EQ(CE_None, rgb_ds->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, kWidth, kHeight, pattern.data(),
                         kWidth, kHeight, GDT_Byte, 0, 0));
  EXPECT_EQ(CE_None, rgb_ds->GetRasterBand(2)->RasterIO(
                         GF_Write, 0, 0, kWidth, kHeight, pattern.data(),
                         kWidth, kHeight, GDT_Byte, 0, 0));
  EXPECT_EQ(CE_None, rgb_ds->GetRasterBand(3)->RasterIO(
                         GF_Write, 0, 0, kWidth, kHeight, pattern.data(),
                         kWidth, kHeight, GDT_Byte, 0, 0));

  EXPECT_EQ(CE_None, GDALDitherRGB2PCT(
                         rgb_ds->GetRasterBand(1), rgb_ds->GetRasterBand(2),
                         rgb_ds->GetRasterBand(3), target_ds->GetRasterBand(1),
                         ct.get(), nullptr, nullptr));

  std::vector<GByte> output(kWidth * kHeight, 255);
  EXPECT_EQ(CE_None, target_ds->GetRasterBand(1)->RasterIO(
                         GF_Read, 0, 0, kWidth, kHeight, output.data(), kWidth,
                         kHeight, GDT_Byte, 0, 0));

  for (GByte pixel : output) {
    EXPECT_LT(pixel, 2);
  }
}

// 14. FindNearestColor: Test All Eight Sub-Indices in SIMD Block
TEST_F(GdalDitherTest, FindNearestColorEightSubIndices) {
  constexpr int kWidth = 8;
  constexpr int kHeight = 1;
  auto rgb_ds = CreateMemDataset(kWidth, kHeight, 3);
  ASSERT_NE(nullptr, rgb_ds);
  auto target_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, target_ds);

  auto ct = std::make_unique<GDALColorTable>(GPI_RGB);
  std::vector<GByte> red_plane(kWidth);
  std::vector<GByte> green_plane(kWidth, 0);
  std::vector<GByte> blue_plane(kWidth, 0);

  // Define 8 distinct red levels, each uniquely matching one entry 0..7.
  for (int i = 0; i < kWidth; ++i) {
    const short r = static_cast<short>(i * 32);
    GDALColorEntry entry = {r, 0, 0, 255};
    ct->SetColorEntry(i, &entry);
    red_plane[i] = static_cast<GByte>(r);
  }

  EXPECT_EQ(CE_None, rgb_ds->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, kWidth, kHeight, red_plane.data(),
                         kWidth, kHeight, GDT_Byte, 0, 0));
  EXPECT_EQ(CE_None, rgb_ds->GetRasterBand(2)->RasterIO(
                         GF_Write, 0, 0, kWidth, kHeight, green_plane.data(),
                         kWidth, kHeight, GDT_Byte, 0, 0));
  EXPECT_EQ(CE_None, rgb_ds->GetRasterBand(3)->RasterIO(
                         GF_Write, 0, 0, kWidth, kHeight, blue_plane.data(),
                         kWidth, kHeight, GDT_Byte, 0, 0));

  EXPECT_EQ(CE_None, GDALDitherRGB2PCTInternal(
                         rgb_ds->GetRasterBand(1), rgb_ds->GetRasterBand(2),
                         rgb_ds->GetRasterBand(3), target_ds->GetRasterBand(1),
                         ct.get(), /*nBits=*/8, /*pasDynamicColorMap=*/nullptr,
                         /*bDither=*/FALSE, nullptr, nullptr));

  std::vector<GByte> output(kWidth, 255);
  EXPECT_EQ(CE_None, target_ds->GetRasterBand(1)->RasterIO(
                         GF_Read, 0, 0, kWidth, kHeight, output.data(), kWidth,
                         kHeight, GDT_Byte, 0, 0));

  // Verify that each pixel mapped to its corresponding exact color entry.
  for (int i = 0; i < kWidth; ++i) {
    EXPECT_EQ(i, output[i]);
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Remaining uncovered branches are OOM guards and I/O
// failure paths.
