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
// Tests median cut algorithm (alg/gdalmediancut.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/gdalmediancut.cpp

#include <algorithm>
#include <cstddef>
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
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_progress.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

// Custom raster band to support simulated read failures and custom dimensions
// without allocating heavy in-memory buffers.
class CustomTestBand final : public GDALRasterBand {
 public:
  CustomTestBand(int nXSize, int nYSize, bool fail_read = false)
      : fail_read_(fail_read) {
    poDS = nullptr;
    nBand = 1;
    eDataType = GDT_Byte;
    nRasterXSize = nXSize;
    nRasterYSize = nYSize;
    nBlockXSize = nXSize;
    nBlockYSize = 1;
  }

  void SetFailRead(bool fail) { fail_read_ = fail; }

 protected:
  CPLErr IReadBlock(int /*nBlockXOff*/, int /*nBlockYOff*/,
                    void* pImage) override {
    if (fail_read_) {
      return CE_Failure;
    }
    if (pImage != nullptr && nBlockXSize > 0) {
      std::memset(pImage, 0, nBlockXSize);
    }
    return CE_None;
  }

  CPLErr IRasterIO(GDALRWFlag /*eRWFlag*/, int /*nXOff*/, int /*nYOff*/,
                   int /*nXSize*/, int /*nYSize*/, void* pData, int nBufXSize,
                   int nBufYSize, GDALDataType /*eBufType*/,
                   GSpacing /*nPixelSpace*/, GSpacing /*nLineSpace*/,
                   GDALRasterIOExtraArg* /*psExtraArg*/) override {
    if (fail_read_) {
      return CE_Failure;
    }
    if (pData != nullptr && nBufXSize > 0 && nBufYSize > 0) {
      std::memset(pData, 0, static_cast<size_t>(nBufXSize) * nBufYSize);
    }
    return CE_None;
  }

 private:
  bool fail_read_ = false;
};

class GdalMedianCutTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_MEM(); }

  static GDALDatasetUniquePtr CreateRgbDataset(int width, int height) {
    GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
    if (mem_driver == nullptr) {
      return nullptr;
    }
    return GDALDatasetUniquePtr(
        mem_driver->Create("", width, height, 3, GDT_Byte, nullptr));
  }

  static bool WriteBands(GDALDataset* ds, const std::vector<uint8_t>& red,
                         const std::vector<uint8_t>& green,
                         const std::vector<uint8_t>& blue) {
    const int width = ds->GetRasterXSize();
    const int height = ds->GetRasterYSize();
    const size_t expected_size = static_cast<size_t>(width) * height;
    if (red.size() != expected_size || green.size() != expected_size ||
        blue.size() != expected_size) {
      return false;
    }
    if (ds->GetRasterBand(1)->RasterIO(
            GF_Write, 0, 0, width, height,
            const_cast<void*>(static_cast<const void*>(red.data())), width,
            height, GDT_Byte, 0, 0) != CE_None) {
      return false;
    }
    if (ds->GetRasterBand(2)->RasterIO(
            GF_Write, 0, 0, width, height,
            const_cast<void*>(static_cast<const void*>(green.data())), width,
            height, GDT_Byte, 0, 0) != CE_None) {
      return false;
    }
    if (ds->GetRasterBand(3)->RasterIO(
            GF_Write, 0, 0, width, height,
            const_cast<void*>(static_cast<const void*>(blue.data())), width,
            height, GDT_Byte, 0, 0) != CE_None) {
      return false;
    }
    return true;
  }
};

TEST_F(GdalMedianCutTest, NullRedBandReturnsFailure) {
  GDALDatasetUniquePtr ds = CreateRgbDataset(4, 4);
  ASSERT_NE(nullptr, ds);
  GDALColorTable ct;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALComputeMedianCutPCT(
                nullptr, GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(3)),
                /*pfnIncludePixel=*/nullptr, 256, GDALColorTable::ToHandle(&ct),
                /*pfnProgress=*/nullptr, /*pProgressArg=*/nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST_F(GdalMedianCutTest, NullGreenBandReturnsFailure) {
  GDALDatasetUniquePtr ds = CreateRgbDataset(4, 4);
  ASSERT_NE(nullptr, ds);
  GDALColorTable ct;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALComputeMedianCutPCT(
                GDALRasterBand::ToHandle(ds->GetRasterBand(1)), nullptr,
                GDALRasterBand::ToHandle(ds->GetRasterBand(3)),
                /*pfnIncludePixel=*/nullptr, 256, GDALColorTable::ToHandle(&ct),
                /*pfnProgress=*/nullptr, /*pProgressArg=*/nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST_F(GdalMedianCutTest, NullBlueBandReturnsFailure) {
  GDALDatasetUniquePtr ds = CreateRgbDataset(4, 4);
  ASSERT_NE(nullptr, ds);
  GDALColorTable ct;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALComputeMedianCutPCT(
                GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(2)), nullptr,
                /*pfnIncludePixel=*/nullptr, 256, GDALColorTable::ToHandle(&ct),
                /*pfnProgress=*/nullptr, /*pProgressArg=*/nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST_F(GdalMedianCutTest, InternalNullRedBandReturnsFailure) {
  GDALDatasetUniquePtr ds = CreateRgbDataset(4, 4);
  ASSERT_NE(nullptr, ds);
  GDALColorTable ct;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALComputeMedianCutPCTInternal<GUInt32>(
                nullptr, GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(3)), nullptr,
                nullptr, nullptr, nullptr, 256, 5, nullptr,
                GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST_F(GdalMedianCutTest, ZeroYSizeReturnsFailure) {
  CustomTestBand red(10, 0);
  CustomTestBand green(10, 0);
  CustomTestBand blue(10, 0);
  GDALColorTable ct;
  EXPECT_EQ(
      CE_Failure,
      GDALComputeMedianCutPCT(
          GDALRasterBand::ToHandle(&red), GDALRasterBand::ToHandle(&green),
          GDALRasterBand::ToHandle(&blue), /*pfnIncludePixel=*/nullptr, 256,
          GDALColorTable::ToHandle(&ct), /*pfnProgress=*/nullptr,
          /*pProgressArg=*/nullptr));
}

TEST_F(GdalMedianCutTest, LargeImageDimensionsDispatchesToGUIntBig) {
  CustomTestBand red(65536, 65536);
  CustomTestBand green(65536, 65536);
  CustomTestBand blue(65536, 65536);
  GDALColorTable ct;
  auto cancel_callback = [](double /*dfComplete*/, const char* /*pszMessage*/,
                            void* /*pProgressArg*/) -> int { return FALSE; };
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(
      CE_Failure,
      GDALComputeMedianCutPCT(
          GDALRasterBand::ToHandle(&red), GDALRasterBand::ToHandle(&green),
          GDALRasterBand::ToHandle(&blue), /*pfnIncludePixel=*/nullptr, 256,
          GDALColorTable::ToHandle(&ct), cancel_callback,
          /*pProgressArg=*/nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_UserInterrupt, CPLGetLastErrorNo());
  CPLErrorReset();
}

TEST_F(GdalMedianCutTest, LargeImageDimensionWarningWithGUInt32) {
  CustomTestBand red(65536, 65536);
  CustomTestBand green(65536, 65536);
  CustomTestBand blue(65536, 65536);
  GDALColorTable ct;
  std::string warning_before_cancel;
  auto cancel_callback = [](double /*dfComplete*/, const char* /*pszMessage*/,
                            void* pProgressArg) -> int {
    if (pProgressArg) {
      *static_cast<std::string*>(pProgressArg) = CPLGetLastErrorMsg();
    }
    return FALSE;
  };
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(
      CE_Failure,
      GDALComputeMedianCutPCTInternal<GUInt32>(
          GDALRasterBand::ToHandle(&red), GDALRasterBand::ToHandle(&green),
          GDALRasterBand::ToHandle(&blue), nullptr, nullptr, nullptr, nullptr,
          256, 5, nullptr, GDALColorTable::ToHandle(&ct), cancel_callback,
          &warning_before_cancel));
  EXPECT_THAT(warning_before_cancel,
              HasSubstr("not called with large enough type"));
  CPLErrorReset();
}

TEST_F(GdalMedianCutTest, GreenXDimensionMismatchReturnsFailure) {
  GDALDatasetUniquePtr red_ds = CreateRgbDataset(4, 4);
  GDALDatasetUniquePtr green_ds = CreateRgbDataset(5, 4);
  GDALDatasetUniquePtr blue_ds = CreateRgbDataset(4, 4);
  ASSERT_NE(nullptr, red_ds);
  ASSERT_NE(nullptr, green_ds);
  ASSERT_NE(nullptr, blue_ds);
  GDALColorTable ct;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALComputeMedianCutPCT(
                GDALRasterBand::ToHandle(red_ds->GetRasterBand(1)),
                GDALRasterBand::ToHandle(green_ds->GetRasterBand(1)),
                GDALRasterBand::ToHandle(blue_ds->GetRasterBand(1)), nullptr,
                256, GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Green or blue band doesn't match"));
  CPLErrorReset();
}

TEST_F(GdalMedianCutTest, GreenYDimensionMismatchReturnsFailure) {
  GDALDatasetUniquePtr red_ds = CreateRgbDataset(4, 4);
  GDALDatasetUniquePtr green_ds = CreateRgbDataset(4, 5);
  GDALDatasetUniquePtr blue_ds = CreateRgbDataset(4, 4);
  ASSERT_NE(nullptr, red_ds);
  ASSERT_NE(nullptr, green_ds);
  ASSERT_NE(nullptr, blue_ds);
  GDALColorTable ct;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALComputeMedianCutPCT(
                GDALRasterBand::ToHandle(red_ds->GetRasterBand(1)),
                GDALRasterBand::ToHandle(green_ds->GetRasterBand(1)),
                GDALRasterBand::ToHandle(blue_ds->GetRasterBand(1)), nullptr,
                256, GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Green or blue band doesn't match"));
  CPLErrorReset();
}

TEST_F(GdalMedianCutTest, BlueXDimensionMismatchReturnsFailure) {
  GDALDatasetUniquePtr red_ds = CreateRgbDataset(4, 4);
  GDALDatasetUniquePtr green_ds = CreateRgbDataset(4, 4);
  GDALDatasetUniquePtr blue_ds = CreateRgbDataset(5, 4);
  ASSERT_NE(nullptr, red_ds);
  ASSERT_NE(nullptr, green_ds);
  ASSERT_NE(nullptr, blue_ds);
  GDALColorTable ct;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALComputeMedianCutPCT(
                GDALRasterBand::ToHandle(red_ds->GetRasterBand(1)),
                GDALRasterBand::ToHandle(green_ds->GetRasterBand(1)),
                GDALRasterBand::ToHandle(blue_ds->GetRasterBand(1)), nullptr,
                256, GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Green or blue band doesn't match"));
  CPLErrorReset();
}

TEST_F(GdalMedianCutTest, BlueYDimensionMismatchReturnsFailure) {
  GDALDatasetUniquePtr red_ds = CreateRgbDataset(4, 4);
  GDALDatasetUniquePtr green_ds = CreateRgbDataset(4, 4);
  GDALDatasetUniquePtr blue_ds = CreateRgbDataset(4, 5);
  ASSERT_NE(nullptr, red_ds);
  ASSERT_NE(nullptr, green_ds);
  ASSERT_NE(nullptr, blue_ds);
  GDALColorTable ct;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALComputeMedianCutPCT(
                GDALRasterBand::ToHandle(red_ds->GetRasterBand(1)),
                GDALRasterBand::ToHandle(green_ds->GetRasterBand(1)),
                GDALRasterBand::ToHandle(blue_ds->GetRasterBand(1)), nullptr,
                256, GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Green or blue band doesn't match"));
  CPLErrorReset();
}

TEST_F(GdalMedianCutTest, UnsupportedIncludePixelCallbackReturnsFailure) {
  GDALDatasetUniquePtr ds = CreateRgbDataset(4, 4);
  ASSERT_NE(nullptr, ds);
  GDALColorTable ct;
  auto include_pixel = [](int /*x*/, int /*y*/, void* /*arg*/) -> int {
    return 1;
  };
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALComputeMedianCutPCT(
                GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(3)), include_pixel,
                256, GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("doesn't currently support pfnIncludePixel"));
  CPLErrorReset();
}

TEST_F(GdalMedianCutTest, ZeroColorCountReturnsFailure) {
  GDALDatasetUniquePtr ds = CreateRgbDataset(4, 4);
  ASSERT_NE(nullptr, ds);
  GDALColorTable ct;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALComputeMedianCutPCT(
                GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(3)), nullptr, 0,
                GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("strictly greater than 1"));
  CPLErrorReset();
}

TEST_F(GdalMedianCutTest, NegativeColorCountReturnsFailure) {
  GDALDatasetUniquePtr ds = CreateRgbDataset(4, 4);
  ASSERT_NE(nullptr, ds);
  GDALColorTable ct;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALComputeMedianCutPCT(
                GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(3)), nullptr, -1,
                GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("strictly greater than 1"));
  CPLErrorReset();
}

TEST_F(GdalMedianCutTest, TooLargeColorCountReturnsFailure) {
  GDALDatasetUniquePtr ds = CreateRgbDataset(4, 4);
  ASSERT_NE(nullptr, ds);
  GDALColorTable ct;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALComputeMedianCutPCT(
                GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(3)), nullptr, 257,
                GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("lesser than or equal to 256"));
  CPLErrorReset();
}

TEST_F(GdalMedianCutTest, RedBandReadFailureReturnsFailure) {
  CustomTestBand red(4, 4, /*fail_read=*/true);
  CustomTestBand green(4, 4, /*fail_read=*/false);
  CustomTestBand blue(4, 4, /*fail_read=*/false);
  GDALColorTable ct;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GDALComputeMedianCutPCT(
                            GDALRasterBand::ToHandle(&red),
                            GDALRasterBand::ToHandle(&green),
                            GDALRasterBand::ToHandle(&blue), nullptr, 16,
                            GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  CPLErrorReset();
}

TEST_F(GdalMedianCutTest, GreenBandReadFailureReturnsFailure) {
  CustomTestBand red(4, 4, /*fail_read=*/false);
  CustomTestBand green(4, 4, /*fail_read=*/true);
  CustomTestBand blue(4, 4, /*fail_read=*/false);
  GDALColorTable ct;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GDALComputeMedianCutPCT(
                            GDALRasterBand::ToHandle(&red),
                            GDALRasterBand::ToHandle(&green),
                            GDALRasterBand::ToHandle(&blue), nullptr, 16,
                            GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  CPLErrorReset();
}

TEST_F(GdalMedianCutTest, BlueBandReadFailureReturnsFailure) {
  CustomTestBand red(4, 4, /*fail_read=*/false);
  CustomTestBand green(4, 4, /*fail_read=*/false);
  CustomTestBand blue(4, 4, /*fail_read=*/true);
  GDALColorTable ct;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GDALComputeMedianCutPCT(
                            GDALRasterBand::ToHandle(&red),
                            GDALRasterBand::ToHandle(&green),
                            GDALRasterBand::ToHandle(&blue), nullptr, 16,
                            GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  CPLErrorReset();
}

TEST_F(GdalMedianCutTest, ProgressCallbackCancellationDuringHistogram) {
  GDALDatasetUniquePtr ds = CreateRgbDataset(4, 4);
  ASSERT_NE(nullptr, ds);
  GDALColorTable ct;
  auto cancel_on_line2 = [](double dfComplete, const char* /*pszMessage*/,
                            void* /*pProgressArg*/) -> int {
    return dfComplete < 0.4 ? TRUE : FALSE;
  };
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALComputeMedianCutPCT(
                GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(3)), nullptr, 16,
                GDALColorTable::ToHandle(&ct), cancel_on_line2, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_UserInterrupt, CPLGetLastErrorNo());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("User Terminated"));
  CPLErrorReset();
}

TEST_F(GdalMedianCutTest, ProgressCallbackCancellationAtCompletion) {
  GDALDatasetUniquePtr ds = CreateRgbDataset(4, 4);
  ASSERT_NE(nullptr, ds);
  GDALColorTable ct;
  auto cancel_at_end = [](double dfComplete, const char* /*pszMessage*/,
                          void* /*pProgressArg*/) -> int {
    return dfComplete < 1.0 ? TRUE : FALSE;
  };
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALComputeMedianCutPCT(
                GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(3)), nullptr, 16,
                GDALColorTable::ToHandle(&ct), cancel_at_end, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_UserInterrupt, CPLGetLastErrorNo());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("User Terminated"));
  CPLErrorReset();
}

TEST_F(GdalMedianCutTest, ProgressCallbackTracksProgress) {
  GDALDatasetUniquePtr ds = CreateRgbDataset(4, 4);
  ASSERT_NE(nullptr, ds);
  GDALColorTable ct;
  int progress_call_count = 0;
  auto track_progress = [](double /*dfComplete*/, const char* /*pszMessage*/,
                           void* pProgressArg) -> int {
    int* count = static_cast<int*>(pProgressArg);
    (*count)++;
    return TRUE;
  };
  EXPECT_EQ(CE_None, GDALComputeMedianCutPCT(
                         GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                         GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                         GDALRasterBand::ToHandle(ds->GetRasterBand(3)),
                         nullptr, 16, GDALColorTable::ToHandle(&ct),
                         track_progress, &progress_call_count));
  // 4 scanlines + 1 final completion call = 5 calls
  EXPECT_EQ(5, progress_call_count);
}

TEST_F(GdalMedianCutTest, SingleUniformColor) {
  constexpr int kWidth = 4;
  constexpr int kHeight = 4;
  GDALDatasetUniquePtr ds = CreateRgbDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> red(kWidth * kHeight, 128);
  std::vector<uint8_t> green(kWidth * kHeight, 64);
  std::vector<uint8_t> blue(kWidth * kHeight, 32);
  ASSERT_TRUE(WriteBands(ds.get(), red, green, blue));

  GDALColorTable ct;
  EXPECT_EQ(CE_None,
            GDALComputeMedianCutPCT(
                GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(3)), nullptr, 16,
                GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(1, ct.GetColorEntryCount());
  const GDALColorEntry* entry = ct.GetColorEntry(0);
  ASSERT_NE(nullptr, entry);
  EXPECT_NEAR(128, entry->c1, 8);
  EXPECT_NEAR(64, entry->c2, 8);
  EXPECT_NEAR(32, entry->c3, 8);
  EXPECT_EQ(255, entry->c4);
}

TEST_F(GdalMedianCutTest, TwoDistinctColorsReturnsTwoEntries) {
  constexpr int kWidth = 4;
  constexpr int kHeight = 4;
  GDALDatasetUniquePtr ds = CreateRgbDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> red(kWidth * kHeight, 0);
  std::vector<uint8_t> green(kWidth * kHeight, 0);
  std::vector<uint8_t> blue(kWidth * kHeight, 0);
  for (int i = 8; i < 16; ++i) {
    red[i] = 255;
    green[i] = 255;
    blue[i] = 255;
  }
  ASSERT_TRUE(WriteBands(ds.get(), red, green, blue));

  GDALColorTable ct;
  EXPECT_EQ(CE_None,
            GDALComputeMedianCutPCT(
                GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(3)), nullptr, 16,
                GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(2, ct.GetColorEntryCount());
}

TEST_F(GdalMedianCutTest,
       AsymmetricColorDistributionTriggersFirstBinIncrement) {
  constexpr int kWidth = 10;
  constexpr int kHeight = 10;
  GDALDatasetUniquePtr ds = CreateRgbDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> red(kWidth * kHeight, 0);
  std::vector<uint8_t> green(kWidth * kHeight, 0);
  std::vector<uint8_t> blue(kWidth * kHeight, 0);
  for (int i = 90; i < 100; ++i) {
    red[i] = 240;
  }
  ASSERT_TRUE(WriteBands(ds.get(), red, green, blue));

  GDALColorTable ct;
  EXPECT_EQ(CE_None,
            GDALComputeMedianCutPCT(
                GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(3)), nullptr, 4,
                GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(2, ct.GetColorEntryCount());
}

TEST_F(GdalMedianCutTest, MultipleDistinctColorsSixteen) {
  constexpr int kWidth = 16;
  constexpr int kHeight = 16;
  GDALDatasetUniquePtr ds = CreateRgbDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> red(kWidth * kHeight);
  std::vector<uint8_t> green(kWidth * kHeight);
  std::vector<uint8_t> blue(kWidth * kHeight);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const int idx = y * kWidth + x;
      red[idx] = static_cast<uint8_t>((x * 17) % 256);
      green[idx] = static_cast<uint8_t>((y * 17) % 256);
      blue[idx] = static_cast<uint8_t>((x * y * 5) % 256);
    }
  }
  ASSERT_TRUE(WriteBands(ds.get(), red, green, blue));

  GDALColorTable ct;
  EXPECT_EQ(CE_None,
            GDALComputeMedianCutPCT(
                GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(3)), nullptr, 16,
                GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(16, ct.GetColorEntryCount());
  for (int i = 0; i < 16; ++i) {
    const GDALColorEntry* entry = ct.GetColorEntry(i);
    ASSERT_NE(nullptr, entry);
    EXPECT_EQ(255, entry->c4);
  }
}

TEST_F(GdalMedianCutTest, MultipleDistinctColorsTwoHundredFiftySix) {
  constexpr int kWidth = 32;
  constexpr int kHeight = 32;
  GDALDatasetUniquePtr ds = CreateRgbDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> red(kWidth * kHeight);
  std::vector<uint8_t> green(kWidth * kHeight);
  std::vector<uint8_t> blue(kWidth * kHeight);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const int idx = y * kWidth + x;
      red[idx] = static_cast<uint8_t>((x * 8) % 256);
      green[idx] = static_cast<uint8_t>((y * 8) % 256);
      blue[idx] = static_cast<uint8_t>(((x + y) * 4) % 256);
    }
  }
  ASSERT_TRUE(WriteBands(ds.get(), red, green, blue));

  GDALColorTable ct;
  EXPECT_EQ(CE_None,
            GDALComputeMedianCutPCT(
                GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(3)), nullptr, 256,
                GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(256, ct.GetColorEntryCount());
}

TEST_F(GdalMedianCutTest, SmoothGradientLargeImageMultiScanline) {
  constexpr int kWidth = 256;
  constexpr int kHeight = 256;
  GDALDatasetUniquePtr ds = CreateRgbDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> red(kWidth * kHeight);
  std::vector<uint8_t> green(kWidth * kHeight);
  std::vector<uint8_t> blue(kWidth * kHeight);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const int idx = y * kWidth + x;
      red[idx] = static_cast<uint8_t>(x);
      green[idx] = static_cast<uint8_t>(y);
      blue[idx] = static_cast<uint8_t>((x + y) / 2);
    }
  }
  ASSERT_TRUE(WriteBands(ds.get(), red, green, blue));

  GDALColorTable ct;
  EXPECT_EQ(CE_None,
            GDALComputeMedianCutPCT(
                GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(3)), nullptr, 256,
                GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(256, ct.GetColorEntryCount());
}

TEST_F(GdalMedianCutTest, RedAxisDominanceFullHistogram) {
  constexpr int kWidth = 32;
  constexpr int kHeight = 1;
  GDALDatasetUniquePtr ds = CreateRgbDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> red(kWidth);
  std::vector<uint8_t> green(kWidth, 0);
  std::vector<uint8_t> blue(kWidth, 0);
  for (int i = 0; i < kWidth; ++i) {
    red[i] = static_cast<uint8_t>(i * 8);
  }
  ASSERT_TRUE(WriteBands(ds.get(), red, green, blue));

  GDALColorTable ct;
  EXPECT_EQ(CE_None,
            GDALComputeMedianCutPCT(
                GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(3)), nullptr, 8,
                GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(8, ct.GetColorEntryCount());
}

TEST_F(GdalMedianCutTest, GreenAxisDominanceFullHistogram) {
  constexpr int kWidth = 32;
  constexpr int kHeight = 1;
  GDALDatasetUniquePtr ds = CreateRgbDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> red(kWidth, 0);
  std::vector<uint8_t> green(kWidth);
  std::vector<uint8_t> blue(kWidth, 0);
  for (int i = 0; i < kWidth; ++i) {
    green[i] = static_cast<uint8_t>(i * 8);
  }
  ASSERT_TRUE(WriteBands(ds.get(), red, green, blue));

  GDALColorTable ct;
  EXPECT_EQ(CE_None,
            GDALComputeMedianCutPCT(
                GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(3)), nullptr, 8,
                GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(8, ct.GetColorEntryCount());
}

TEST_F(GdalMedianCutTest, BlueAxisDominanceFullHistogram) {
  constexpr int kWidth = 32;
  constexpr int kHeight = 1;
  GDALDatasetUniquePtr ds = CreateRgbDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> red(kWidth, 0);
  std::vector<uint8_t> green(kWidth, 0);
  std::vector<uint8_t> blue(kWidth);
  for (int i = 0; i < kWidth; ++i) {
    blue[i] = static_cast<uint8_t>(i * 8);
  }
  ASSERT_TRUE(WriteBands(ds.get(), red, green, blue));

  GDALColorTable ct;
  EXPECT_EQ(CE_None,
            GDALComputeMedianCutPCT(
                GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(3)), nullptr, 8,
                GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(8, ct.GetColorEntryCount());
}

TEST_F(GdalMedianCutTest, InternalEightBitExactColorsEarlyExit) {
  constexpr int kWidth = 3;
  constexpr int kHeight = 1;
  GDALDatasetUniquePtr ds = CreateRgbDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> red = {10, 40, 70};
  std::vector<uint8_t> green = {20, 50, 80};
  std::vector<uint8_t> blue = {30, 60, 90};
  ASSERT_TRUE(WriteBands(ds.get(), red, green, blue));

  GDALColorTable ct;
  EXPECT_EQ(CE_None, GDALComputeMedianCutPCTInternal<GUInt32>(
                         GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                         GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                         GDALRasterBand::ToHandle(ds->GetRasterBand(3)),
                         nullptr, nullptr, nullptr, nullptr, 16, 8, nullptr,
                         GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(3, ct.GetColorEntryCount());
  const GDALColorEntry* e0 = ct.GetColorEntry(0);
  ASSERT_NE(nullptr, e0);
  EXPECT_EQ(10, e0->c1);
  EXPECT_EQ(20, e0->c2);
  EXPECT_EQ(30, e0->c3);
  EXPECT_EQ(255, e0->c4);

  const GDALColorEntry* e1 = ct.GetColorEntry(1);
  ASSERT_NE(nullptr, e1);
  EXPECT_EQ(40, e1->c1);
  EXPECT_EQ(50, e1->c2);
  EXPECT_EQ(60, e1->c3);

  const GDALColorEntry* e2 = ct.GetColorEntry(2);
  ASSERT_NE(nullptr, e2);
  EXPECT_EQ(70, e2->c1);
  EXPECT_EQ(80, e2->c2);
  EXPECT_EQ(90, e2->c3);
}

TEST_F(GdalMedianCutTest, PreallocatedHistogramFourBits) {
  constexpr int kWidth = 16;
  constexpr int kHeight = 16;
  GDALDatasetUniquePtr ds = CreateRgbDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> red(kWidth * kHeight, 100);
  std::vector<uint8_t> green(kWidth * kHeight, 150);
  std::vector<uint8_t> blue(kWidth * kHeight, 200);
  for (int i = 0; i < (kWidth * kHeight) / 2; ++i) {
    red[i] = 10;
    green[i] = 20;
    blue[i] = 30;
  }
  ASSERT_TRUE(WriteBands(ds.get(), red, green, blue));

  // nBits = 4 -> nCLevelsCube = 16^3 = 4096
  std::vector<GUInt32> hist(4096, 0);
  GDALColorTable ct;
  EXPECT_EQ(CE_None, GDALComputeMedianCutPCTInternal<GUInt32>(
                         GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                         GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                         GDALRasterBand::ToHandle(ds->GetRasterBand(3)),
                         nullptr, nullptr, nullptr, nullptr, 8, 4, hist.data(),
                         GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_GE(ct.GetColorEntryCount(), 1);
}

TEST_F(GdalMedianCutTest, HashHistogramRedAxis) {
  constexpr int kWidth = 8;
  constexpr int kHeight = 1;
  GDALDatasetUniquePtr ds = CreateRgbDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> red = {0, 30, 60, 90, 120, 150, 180, 210};
  std::vector<uint8_t> green(kWidth, 0);
  std::vector<uint8_t> blue(kWidth, 0);
  ASSERT_TRUE(WriteBands(ds.get(), red, green, blue));

  std::vector<uint8_t> hash_buf(MEDIAN_CUT_AND_DITHER_BUFFER_SIZE_65536);
  GDALColorTable ct;
  EXPECT_EQ(CE_None, GDALComputeMedianCutPCTInternal<GUInt32>(
                         GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                         GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                         GDALRasterBand::ToHandle(ds->GetRasterBand(3)),
                         nullptr, nullptr, nullptr, nullptr, 4, 8,
                         reinterpret_cast<GUInt32*>(hash_buf.data()),
                         GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(4, ct.GetColorEntryCount());
}

TEST_F(GdalMedianCutTest, HashHistogramGreenAxis) {
  constexpr int kWidth = 8;
  constexpr int kHeight = 1;
  GDALDatasetUniquePtr ds = CreateRgbDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> red(kWidth, 0);
  std::vector<uint8_t> green = {0, 30, 60, 90, 120, 150, 180, 210};
  std::vector<uint8_t> blue(kWidth, 0);
  ASSERT_TRUE(WriteBands(ds.get(), red, green, blue));

  std::vector<uint8_t> hash_buf(MEDIAN_CUT_AND_DITHER_BUFFER_SIZE_65536);
  GDALColorTable ct;
  EXPECT_EQ(CE_None, GDALComputeMedianCutPCTInternal<GUInt32>(
                         GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                         GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                         GDALRasterBand::ToHandle(ds->GetRasterBand(3)),
                         nullptr, nullptr, nullptr, nullptr, 4, 8,
                         reinterpret_cast<GUInt32*>(hash_buf.data()),
                         GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(4, ct.GetColorEntryCount());
}

TEST_F(GdalMedianCutTest, HashHistogramBlueAxis) {
  constexpr int kWidth = 8;
  constexpr int kHeight = 1;
  GDALDatasetUniquePtr ds = CreateRgbDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> red(kWidth, 0);
  std::vector<uint8_t> green(kWidth, 0);
  std::vector<uint8_t> blue = {0, 30, 60, 90, 120, 150, 180, 210};
  ASSERT_TRUE(WriteBands(ds.get(), red, green, blue));

  std::vector<uint8_t> hash_buf(MEDIAN_CUT_AND_DITHER_BUFFER_SIZE_65536);
  GDALColorTable ct;
  EXPECT_EQ(CE_None, GDALComputeMedianCutPCTInternal<GUInt32>(
                         GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                         GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                         GDALRasterBand::ToHandle(ds->GetRasterBand(3)),
                         nullptr, nullptr, nullptr, nullptr, 4, 8,
                         reinterpret_cast<GUInt32*>(hash_buf.data()),
                         GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(4, ct.GetColorEntryCount());
}

TEST_F(GdalMedianCutTest, HashHistogramCollisionsAndProbing) {
  // Test 4 colors that share identical code % PRIME_FOR_65536 (98317):
  // C1 = 10: r=10, g=0, b=0
  // C2 = 98327 (10 + 98317): r=23, g=128, b=1
  // C3 = 196644 (10 + 2*98317): r=36, g=0, b=3
  // C4 = 294961 (10 + 3*98317): r=49, g=128, b=4
  // Along with repetitions and distinct colors to exercise slots and probing.
  constexpr int kWidth = 12;
  constexpr int kHeight = 1;
  GDALDatasetUniquePtr ds = CreateRgbDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> red = {10, 23, 36,  49,  10,  23,
                              36, 49, 100, 150, 200, 250};
  std::vector<uint8_t> green = {0, 128, 0, 128, 0, 128, 0, 128, 50, 60, 70, 80};
  std::vector<uint8_t> blue = {0, 1, 3, 4, 0, 1, 3, 4, 10, 20, 30, 40};
  ASSERT_TRUE(WriteBands(ds.get(), red, green, blue));

  std::vector<uint8_t> hash_buf(MEDIAN_CUT_AND_DITHER_BUFFER_SIZE_65536);
  GDALColorTable ct;
  EXPECT_EQ(CE_None, GDALComputeMedianCutPCTInternal<GUInt32>(
                         GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                         GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                         GDALRasterBand::ToHandle(ds->GetRasterBand(3)),
                         nullptr, nullptr, nullptr, nullptr, 4, 8,
                         reinterpret_cast<GUInt32*>(hash_buf.data()),
                         GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(4, ct.GetColorEntryCount());
}

TEST_F(GdalMedianCutTest, BandPointersRedAxis) {
  constexpr int kWidth = 8;
  constexpr int kHeight = 1;
  GDALDatasetUniquePtr ds = CreateRgbDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> red = {0, 30, 60, 90, 120, 150, 180, 240};
  std::vector<uint8_t> green(kWidth, 0);
  std::vector<uint8_t> blue(kWidth, 0);
  ASSERT_TRUE(WriteBands(ds.get(), red, green, blue));

  std::vector<uint8_t> hash_buf(MEDIAN_CUT_AND_DITHER_BUFFER_SIZE_65536);
  GDALColorTable ct;
  EXPECT_EQ(CE_None, GDALComputeMedianCutPCTInternal<GUInt32>(
                         GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                         GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                         GDALRasterBand::ToHandle(ds->GetRasterBand(3)),
                         red.data(), green.data(), blue.data(), nullptr, 4, 8,
                         reinterpret_cast<GUInt32*>(hash_buf.data()),
                         GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(4, ct.GetColorEntryCount());
}

TEST_F(GdalMedianCutTest, BandPointersGreenAxis) {
  constexpr int kWidth = 8;
  constexpr int kHeight = 1;
  GDALDatasetUniquePtr ds = CreateRgbDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> red(kWidth, 0);
  std::vector<uint8_t> green = {0, 30, 60, 90, 120, 150, 180, 240};
  std::vector<uint8_t> blue(kWidth, 0);
  ASSERT_TRUE(WriteBands(ds.get(), red, green, blue));

  std::vector<uint8_t> hash_buf(MEDIAN_CUT_AND_DITHER_BUFFER_SIZE_65536);
  GDALColorTable ct;
  EXPECT_EQ(CE_None, GDALComputeMedianCutPCTInternal<GUInt32>(
                         GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                         GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                         GDALRasterBand::ToHandle(ds->GetRasterBand(3)),
                         red.data(), green.data(), blue.data(), nullptr, 4, 8,
                         reinterpret_cast<GUInt32*>(hash_buf.data()),
                         GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(4, ct.GetColorEntryCount());
}

TEST_F(GdalMedianCutTest, BandPointersBlueAxis) {
  constexpr int kWidth = 8;
  constexpr int kHeight = 1;
  GDALDatasetUniquePtr ds = CreateRgbDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> red(kWidth, 0);
  std::vector<uint8_t> green(kWidth, 0);
  std::vector<uint8_t> blue = {0, 30, 60, 90, 120, 150, 180, 240};
  ASSERT_TRUE(WriteBands(ds.get(), red, green, blue));

  std::vector<uint8_t> hash_buf(MEDIAN_CUT_AND_DITHER_BUFFER_SIZE_65536);
  GDALColorTable ct;
  EXPECT_EQ(CE_None, GDALComputeMedianCutPCTInternal<GUInt32>(
                         GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                         GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                         GDALRasterBand::ToHandle(ds->GetRasterBand(3)),
                         red.data(), green.data(), blue.data(), nullptr, 4, 8,
                         reinterpret_cast<GUInt32*>(hash_buf.data()),
                         GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(4, ct.GetColorEntryCount());
}

TEST_F(GdalMedianCutTest, GUIntBigInstantiation) {
  constexpr int kWidth = 8;
  constexpr int kHeight = 1;
  GDALDatasetUniquePtr ds = CreateRgbDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> red = {0, 30, 60, 90, 120, 150, 180, 210};
  std::vector<uint8_t> green = {10, 40, 70, 100, 130, 160, 190, 220};
  std::vector<uint8_t> blue = {20, 50, 80, 110, 140, 170, 200, 230};
  ASSERT_TRUE(WriteBands(ds.get(), red, green, blue));

  GDALColorTable ct;
  EXPECT_EQ(CE_None, GDALComputeMedianCutPCTInternal<GUIntBig>(
                         GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                         GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                         GDALRasterBand::ToHandle(ds->GetRasterBand(3)),
                         nullptr, nullptr, nullptr, nullptr, 4, 5, nullptr,
                         GDALColorTable::ToHandle(&ct), nullptr, nullptr));
  EXPECT_EQ(4, ct.GetColorEntryCount());

  std::vector<GUIntBig> hist_big(4096, 0);
  GDALColorTable ct2;
  EXPECT_EQ(CE_None,
            GDALComputeMedianCutPCTInternal<GUIntBig>(
                GDALRasterBand::ToHandle(ds->GetRasterBand(1)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(2)),
                GDALRasterBand::ToHandle(ds->GetRasterBand(3)), nullptr,
                nullptr, nullptr, nullptr, 4, 4, hist_big.data(),
                GDALColorTable::ToHandle(&ct2), nullptr, nullptr));
  EXPECT_EQ(4, ct2.GetColorEntryCount());
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Remaining uncovered branches are OOM guards and histogram
// edge paths.
