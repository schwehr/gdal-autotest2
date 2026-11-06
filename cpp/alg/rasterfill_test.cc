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
// Tests rasterfill (alg/rasterfill.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/rasterfill.cpp

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

// Returns default fill options directing temporary work files to MEM driver.
CPLStringList GetDefaultFillOptions() {
  CPLStringList options;
  options.SetNameValue("TEMP_FILE_DRIVER", "MEM");
  return options;
}

// Helper to create a single-band MEM dataset of given dimensions and data type.
GDALDatasetUniquePtr CreateMemDataset(int width, int height,
                                      GDALDataType type = GDT_Float32) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("MEM");
  if (driver == nullptr) {
    return nullptr;
  }
  return GDALDatasetUniquePtr(
      driver->Create("", width, height, 1, type, nullptr));
}

// Custom raster band simulating read/write failures for error branch coverage.
class FaultyRasterBand final : public GDALRasterBand {
 public:
  FaultyRasterBand(int x_size, int y_size, GDALDataType type,
                   bool fail_on_read = false, bool fail_on_write = false)
      : fail_on_read_(fail_on_read), fail_on_write_(fail_on_write) {
    poDS = nullptr;
    nBand = 1;
    eDataType = type;
    nRasterXSize = x_size;
    nRasterYSize = y_size;
    nBlockXSize = std::max(1, x_size);
    nBlockYSize = 1;
    eAccess = GA_Update;
  }

  void SetFailOnRead(bool fail) { fail_on_read_ = fail; }
  void SetFailOnWrite(bool fail) { fail_on_write_ = fail; }
  void SetFailAfterNReads(int n) {
    fail_after_n_reads_ = n;
    read_count_ = 0;
  }
  void SetFailAfterNWrites(int n) {
    fail_after_n_writes_ = n;
    write_count_ = 0;
  }

 protected:
  CPLErr IReadBlock(int /*nBlockXOff*/, int /*nBlockYOff*/,
                    void* /*pImage*/) override {
    return CE_Failure;
  }

  CPLErr IRasterIO(GDALRWFlag eRWFlag, int /*nXOff*/, int /*nYOff*/, int nXSize,
                   int nYSize, void* pData, int /*nBufXSize*/,
                   int /*nBufYSize*/, GDALDataType eBufType,
                   GSpacing /*nPixelSpace*/, GSpacing /*nLineSpace*/,
                   GDALRasterIOExtraArg* /*psExtraArg*/) override {
    if (eRWFlag == GF_Read) {
      read_count_++;
      if (fail_on_read_ ||
          (fail_after_n_reads_ >= 0 && read_count_ > fail_after_n_reads_)) {
        CPLError(CE_Failure, CPLE_AppDefined, "Simulated read failure");
        return CE_Failure;
      }
      const int dt_size = GDALGetDataTypeSizeBytes(eBufType);
      std::memset(pData, 0, static_cast<size_t>(nXSize) * nYSize * dt_size);
      return CE_None;
    }
    if (eRWFlag == GF_Write) {
      write_count_++;
      if (fail_on_write_ ||
          (fail_after_n_writes_ >= 0 && write_count_ > fail_after_n_writes_)) {
        CPLError(CE_Failure, CPLE_AppDefined, "Simulated write failure");
        return CE_Failure;
      }
      return CE_None;
    }
    return CE_None;
  }

 private:
  bool fail_on_read_ = false;
  bool fail_on_write_ = false;
  int fail_after_n_reads_ = -1;
  int fail_after_n_writes_ = -1;
  int read_count_ = 0;
  int write_count_ = 0;
};

class RasterFillTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_MEM(); }
};

// 1. Verify handling of null target band pointer.
TEST_F(RasterFillTest, NullTargetBand) {
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GDALFillNodata(nullptr, nullptr, 0.0, 0, 0, nullptr,
                                       nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

// 2. Verify error handling for invalid interpolation option.
TEST_F(RasterFillTest, InvalidInterpolationOption) {
  GDALDatasetUniquePtr ds = CreateMemDataset(3, 3);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  CPLStringList options = GetDefaultFillOptions();
  options.SetNameValue("INTERPOLATION", "UNSUPPORTED_METHOD");

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALFillNodata(GDALRasterBand::ToHandle(band), nullptr, 0.0, 0, 0,
                           options.List(), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Unsupported interpolation method"));
  CPLErrorReset();
}

// 3. Verify error handling when TEMP_FILE_DRIVER specifies an unregistered
// driver.
TEST_F(RasterFillTest, UnregisteredTempFileDriver) {
  GDALDatasetUniquePtr ds = CreateMemDataset(3, 3);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  CPLStringList options;
  options.SetNameValue("TEMP_FILE_DRIVER", "NONEXISTENT_DRIVER_XYZ");

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALFillNodata(GDALRasterBand::ToHandle(band), nullptr, 0.0, 0, 0,
                           options.List(), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("driver is not registered"));
  CPLErrorReset();
}

// 4. Verify default TEMP_FILE_DRIVER (GTiff) error when GTiff is not
// registered.
TEST_F(RasterFillTest, DefaultTempFileDriverGTiffUnregistered) {
  GDALDatasetUniquePtr ds = CreateMemDataset(3, 3);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GDALFillNodata(GDALRasterBand::ToHandle(band), nullptr,
                                       0.0, 0, 0, nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("driver is not registered"));
  CPLErrorReset();
}

// 5. Verify failure when work Y dataset creation fails.
TEST_F(RasterFillTest, FailingTempDriverYWorkDatasetFailure) {
  GDALDriver failing_driver;
  failing_driver.SetDescription("FAILING_TEMP_DRIVER_Y");
  failing_driver.SetMetadataItem(GDAL_DCAP_RASTER, "YES");
  failing_driver.SetMetadataItem(GDAL_DCAP_CREATE, "YES");
  failing_driver.pfnCreate = [](const char*, int, int, int, GDALDataType,
                                char**) -> GDALDataset* { return nullptr; };
  GetGDALDriverManager()->RegisterDriver(&failing_driver);
  absl::Cleanup deregister = [&]() {
    GetGDALDriverManager()->DeregisterDriver(&failing_driver);
  };

  auto band = std::make_unique<FaultyRasterBand>(4, 4, GDT_Byte);
  CPLStringList options;
  options.SetNameValue("TEMP_FILE_DRIVER", "FAILING_TEMP_DRIVER_Y");

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALFillNodata(GDALRasterBand::ToHandle(band.get()), nullptr, 0.0,
                           0, 0, options.List(), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Could not create Y index work file"));
  CPLErrorReset();
}

// 6. Verify failure when temporary mask work dataset creation fails.
TEST_F(RasterFillTest, FailingTempDriverMaskWorkDatasetFailure) {
  GDALDriver failing_driver;
  failing_driver.SetDescription("FAILING_TEMP_DRIVER_MASK");
  failing_driver.SetMetadataItem(GDAL_DCAP_RASTER, "YES");
  failing_driver.SetMetadataItem(GDAL_DCAP_CREATE, "YES");
  failing_driver.pfnCreate = [](const char*, int, int, int, GDALDataType,
                                char**) -> GDALDataset* { return nullptr; };
  GetGDALDriverManager()->RegisterDriver(&failing_driver);
  absl::Cleanup deregister = [&]() {
    GetGDALDriverManager()->DeregisterDriver(&failing_driver);
  };

  auto target_band = std::make_unique<FaultyRasterBand>(4, 4, GDT_Byte);
  auto mask_band = std::make_unique<FaultyRasterBand>(4, 4, GDT_Byte);
  CPLStringList options;
  options.SetNameValue("TEMP_FILE_DRIVER", "FAILING_TEMP_DRIVER_MASK");

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALFillNodata(GDALRasterBand::ToHandle(target_band.get()),
                           GDALRasterBand::ToHandle(mask_band.get()), 0.0, 0, 1,
                           options.List(), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Could not create poTmpMaskDS work file"));
  CPLErrorReset();
}

// 7. Verify failure when copying mask band fails during smoothing setup.
TEST_F(RasterFillTest, MaskBandCopyWholeRasterFailure) {
  GDALDatasetUniquePtr target_ds = CreateMemDataset(3, 3);
  ASSERT_NE(nullptr, target_ds);
  GDALRasterBand* target_band = target_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, target_band);

  auto faulty_mask = std::make_unique<FaultyRasterBand>(
      3, 3, GDT_Byte, /*fail_on_read=*/true, /*fail_on_write=*/false);
  CPLStringList options = GetDefaultFillOptions();

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALFillNodata(GDALRasterBand::ToHandle(target_band),
                           GDALRasterBand::ToHandle(faulty_mask.get()), 0.0, 0,
                           1, options.List(), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

// 8. Verify cancellation in progress callback at start of filling.
TEST_F(RasterFillTest, ProgressCancelInitial) {
  GDALDatasetUniquePtr ds = CreateMemDataset(3, 3);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  CPLStringList options = GetDefaultFillOptions();

  auto progress_cb = [](double /*dfComplete*/, const char* /*pszMessage*/,
                        void* /*pData*/) -> int { return FALSE; };

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALFillNodata(GDALRasterBand::ToHandle(band), nullptr, 0.0, 0, 0,
                           options.List(), progress_cb, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_UserInterrupt, CPLGetLastErrorNo());
  CPLErrorReset();
}

// 9. Verify cancellation during the top-to-bottom pass.
TEST_F(RasterFillTest, ProgressCancelTopDown) {
  GDALDatasetUniquePtr ds = CreateMemDataset(10, 10);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  CPLStringList options = GetDefaultFillOptions();

  auto progress_cb = [](double dfComplete, const char* pszMessage,
                        void* /*pData*/) -> int {
    if (pszMessage != nullptr && std::string_view(pszMessage) == "Filling..." &&
        dfComplete >= 0.1) {
      return FALSE;
    }
    return TRUE;
  };

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALFillNodata(GDALRasterBand::ToHandle(band), nullptr, 0.0, 0, 0,
                           options.List(), progress_cb, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_UserInterrupt, CPLGetLastErrorNo());
  CPLErrorReset();
}

// 10. Verify cancellation during the bottom-to-top pass.
TEST_F(RasterFillTest, ProgressCancelBottomUp) {
  GDALDatasetUniquePtr ds = CreateMemDataset(10, 10);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  CPLStringList options = GetDefaultFillOptions();

  auto progress_cb = [](double dfComplete, const char* pszMessage,
                        void* /*pData*/) -> int {
    if (pszMessage != nullptr && std::string_view(pszMessage) == "Filling..." &&
        dfComplete >= 0.6) {
      return FALSE;
    }
    return TRUE;
  };

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALFillNodata(GDALRasterBand::ToHandle(band), nullptr, 0.0, 0, 0,
                           options.List(), progress_cb, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_UserInterrupt, CPLGetLastErrorNo());
  CPLErrorReset();
}

// 11. Verify cancellation at the start of the smoothing filter pass.
TEST_F(RasterFillTest, ProgressCancelSmoothingInitial) {
  GDALDatasetUniquePtr ds = CreateMemDataset(10, 10);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  CPLStringList options = GetDefaultFillOptions();

  auto progress_cb = [](double /*dfComplete*/, const char* pszMessage,
                        void* /*pData*/) -> int {
    if (pszMessage != nullptr &&
        std::string_view(pszMessage) == "Smoothing Filter...") {
      return FALSE;
    }
    return TRUE;
  };

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALFillNodata(GDALRasterBand::ToHandle(band), nullptr, 0.0, 0, 1,
                           options.List(), progress_cb, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_UserInterrupt, CPLGetLastErrorNo());
  CPLErrorReset();
}

// 12. Verify cancellation during smoothing iterations.
TEST_F(RasterFillTest, ProgressCancelSmoothingIteration) {
  GDALDatasetUniquePtr ds = CreateMemDataset(10, 10);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  CPLStringList options = GetDefaultFillOptions();

  auto progress_cb = [](double dfComplete, const char* pszMessage,
                        void* /*pData*/) -> int {
    if (pszMessage != nullptr &&
        std::string_view(pszMessage) == "Smoothing Filter..." &&
        dfComplete >= 0.95) {
      return FALSE;
    }
    return TRUE;
  };

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALFillNodata(GDALRasterBand::ToHandle(band), nullptr, 0.0, 0, 2,
                           options.List(), progress_cb, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_UserInterrupt, CPLGetLastErrorNo());
  CPLErrorReset();
}

// 13. Verify smoothing with invalid/empty smoothing iterations.
TEST_F(RasterFillTest, ZeroSmoothingIterationsSucceeds) {
  GDALDatasetUniquePtr ds = CreateMemDataset(4, 4);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  CPLStringList options = GetDefaultFillOptions();
  EXPECT_EQ(CE_None,
            GDALFillNodata(GDALRasterBand::ToHandle(band), nullptr, 0.0, 0, 0,
                           options.List(), nullptr, nullptr));
}

// 14. Verify failure when creating the XY value work file with GDT_Unknown.
TEST_F(RasterFillTest, UnknownDataTypeFailsXYValueDataset) {
  auto band = std::make_unique<FaultyRasterBand>(3, 3, GDT_Unknown);
  CPLStringList options = GetDefaultFillOptions();

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALFillNodata(GDALRasterBand::ToHandle(band.get()), nullptr, 0.0,
                           0, 0, options.List(), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Could not create XY value work file"));
  CPLErrorReset();
}

// 15. Verify error handling when reading from target band fails.
TEST_F(RasterFillTest, RasterIOReadFailureTarget) {
  auto band = std::make_unique<FaultyRasterBand>(
      3, 3, GDT_Float32, /*fail_on_read=*/true, /*fail_on_write=*/false);
  CPLStringList options = GetDefaultFillOptions();

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALFillNodata(GDALRasterBand::ToHandle(band.get()), nullptr, 0.0,
                           0, 0, options.List(), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

// 16. Verify error handling when reading from mask band fails.
TEST_F(RasterFillTest, RasterIOReadFailureMask) {
  GDALDatasetUniquePtr target_ds = CreateMemDataset(3, 3);
  ASSERT_NE(nullptr, target_ds);
  GDALRasterBand* target_band = target_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, target_band);

  auto mask_band = std::make_unique<FaultyRasterBand>(
      3, 3, GDT_Byte, /*fail_on_read=*/true, /*fail_on_write=*/false);
  CPLStringList options = GetDefaultFillOptions();

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALFillNodata(GDALRasterBand::ToHandle(target_band),
                           GDALRasterBand::ToHandle(mask_band.get()), 0.0, 0, 0,
                           options.List(), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

// 17. Verify error handling when writing to target band fails.
TEST_F(RasterFillTest, RasterIOWriteFailureTarget) {
  auto band = std::make_unique<FaultyRasterBand>(
      3, 3, GDT_Float32, /*fail_on_read=*/false, /*fail_on_write=*/true);
  CPLStringList options = GetDefaultFillOptions();

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALFillNodata(GDALRasterBand::ToHandle(band.get()), nullptr, 0.0,
                           0, 0, options.List(), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

// 18. Verify error handling when reading from target band fails during
// smoothing.
TEST_F(RasterFillTest, RasterIOReadFailureInMultiFilter) {
  auto band = std::make_unique<FaultyRasterBand>(3, 3, GDT_Float32);
  band->SetFailAfterNReads(6);
  CPLStringList options = GetDefaultFillOptions();

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALFillNodata(GDALRasterBand::ToHandle(band.get()), nullptr, 0.0,
                           0, 1, options.List(), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

// 19. Verify error handling when writing to target band fails during smoothing.
TEST_F(RasterFillTest, RasterIOWriteFailureInMultiFilter) {
  auto band = std::make_unique<FaultyRasterBand>(3, 3, GDT_Float32);
  band->SetFailAfterNWrites(3);
  CPLStringList options = GetDefaultFillOptions();

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALFillNodata(GDALRasterBand::ToHandle(band.get()), nullptr, 0.0,
                           0, 1, options.List(), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

// 20. Verify error handling when reading from target band fails on bottom-up
// pass.
TEST_F(RasterFillTest, RasterIOReadFailureBottomUp) {
  auto band = std::make_unique<FaultyRasterBand>(3, 3, GDT_Float32);
  band->SetFailAfterNReads(3);
  CPLStringList options = GetDefaultFillOptions();

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALFillNodata(GDALRasterBand::ToHandle(band.get()), nullptr, 0.0,
                           0, 0, options.List(), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

// 21. Basic 3x3 nodata hole interpolation using inverse distance weighting.
TEST_F(RasterFillTest, BasicFill3x3InvDistCenterHole) {
  constexpr int kWidth = 3;
  constexpr int kHeight = 3;
  GDALDatasetUniquePtr target_ds = CreateMemDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, target_ds);
  GDALRasterBand* target_band = target_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, target_band);

  GDALDatasetUniquePtr mask_ds = CreateMemDataset(kWidth, kHeight, GDT_Byte);
  ASSERT_NE(nullptr, mask_ds);
  GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, mask_band);

  std::vector<float> target_data = {
      10.0f, 10.0f, 10.0f, 10.0f, 0.0f, 10.0f, 10.0f, 10.0f, 10.0f,
  };
  std::vector<uint8_t> mask_data = {
      255, 255, 255, 255, 0, 255, 255, 255, 255,
  };

  ASSERT_EQ(CE_None, target_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                           target_data.data(), kWidth, kHeight,
                                           GDT_Float32, 0, 0));
  ASSERT_EQ(CE_None, mask_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                         mask_data.data(), kWidth, kHeight,
                                         GDT_Byte, 0, 0));

  CPLStringList options = GetDefaultFillOptions();
  EXPECT_EQ(CE_None, GDALFillNodata(GDALRasterBand::ToHandle(target_band),
                                    GDALRasterBand::ToHandle(mask_band), 0.0, 0,
                                    0, options.List(), nullptr, nullptr));

  std::vector<float> result(kWidth * kHeight);
  ASSERT_EQ(CE_None,
            target_band->RasterIO(GF_Read, 0, 0, kWidth, kHeight, result.data(),
                                  kWidth, kHeight, GDT_Float32, 0, 0));
  EXPECT_NEAR(10.0f, result[4], 1e-4f);
}

// 22. 3x3 nodata hole with 4 different neighbors to test exact inverse distance
// weighting.
TEST_F(RasterFillTest, BasicFill3x3InvDistWeightedAverage) {
  constexpr int kWidth = 3;
  constexpr int kHeight = 3;
  GDALDatasetUniquePtr target_ds = CreateMemDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, target_ds);
  GDALRasterBand* target_band = target_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, target_band);

  GDALDatasetUniquePtr mask_ds = CreateMemDataset(kWidth, kHeight, GDT_Byte);
  ASSERT_NE(nullptr, mask_ds);
  GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, mask_band);

  std::vector<float> target_data = {
      0.0f, 10.0f, 0.0f, 20.0f, 0.0f, 40.0f, 0.0f, 30.0f, 0.0f,
  };
  std::vector<uint8_t> mask_data = {
      0, 255, 0, 255, 0, 255, 0, 255, 0,
  };

  ASSERT_EQ(CE_None, target_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                           target_data.data(), kWidth, kHeight,
                                           GDT_Float32, 0, 0));
  ASSERT_EQ(CE_None, mask_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                         mask_data.data(), kWidth, kHeight,
                                         GDT_Byte, 0, 0));

  CPLStringList options = GetDefaultFillOptions();
  EXPECT_EQ(CE_None, GDALFillNodata(GDALRasterBand::ToHandle(target_band),
                                    GDALRasterBand::ToHandle(mask_band), 2.0, 0,
                                    0, options.List(), nullptr, nullptr));

  std::vector<float> result(kWidth * kHeight);
  ASSERT_EQ(CE_None,
            target_band->RasterIO(GF_Read, 0, 0, kWidth, kHeight, result.data(),
                                  kWidth, kHeight, GDT_Float32, 0, 0));
  // 3 quadrants found at dist 1.0 (top (1,0)=10, bottom (1,2)=30, right
  // (2,1)=40): (10 + 30 + 40) / 3 = 80/3 = 26.6667
  EXPECT_NEAR(80.0f / 3.0f, result[4], 1e-4f);
}

// 23. 3x3 nodata hole interpolation using nearest neighbor.
TEST_F(RasterFillTest, BasicFill3x3Nearest) {
  constexpr int kWidth = 3;
  constexpr int kHeight = 3;
  GDALDatasetUniquePtr target_ds = CreateMemDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, target_ds);
  GDALRasterBand* target_band = target_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, target_band);

  GDALDatasetUniquePtr mask_ds = CreateMemDataset(kWidth, kHeight, GDT_Byte);
  ASSERT_NE(nullptr, mask_ds);
  GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, mask_band);

  std::vector<float> target_data(9, 0.0f);
  target_data[1] = 42.0f;  // Pixel (1, 0) top neighbor
  std::vector<uint8_t> mask_data(9, 0);
  mask_data[1] = 255;  // Only top neighbor is valid

  ASSERT_EQ(CE_None, target_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                           target_data.data(), kWidth, kHeight,
                                           GDT_Float32, 0, 0));
  ASSERT_EQ(CE_None, mask_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                         mask_data.data(), kWidth, kHeight,
                                         GDT_Byte, 0, 0));

  CPLStringList options = GetDefaultFillOptions();
  options.SetNameValue("INTERPOLATION", "NEAREST");

  EXPECT_EQ(CE_None, GDALFillNodata(GDALRasterBand::ToHandle(target_band),
                                    GDALRasterBand::ToHandle(mask_band), 2.0, 0,
                                    0, options.List(), nullptr, nullptr));

  std::vector<float> result(kWidth * kHeight);
  ASSERT_EQ(CE_None,
            target_band->RasterIO(GF_Read, 0, 0, kWidth, kHeight, result.data(),
                                  kWidth, kHeight, GDT_Float32, 0, 0));
  EXPECT_NEAR(42.0f, result[4], 1e-4f);
}

// 24. No mask band provided; internally uses band nodata mask band.
TEST_F(RasterFillTest, BasicFill3x3NoMaskBandUsesDatasetNodata) {
  constexpr int kWidth = 3;
  constexpr int kHeight = 3;
  GDALDatasetUniquePtr ds = CreateMemDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  ASSERT_EQ(CE_None, band->SetNoDataValue(-9999.0));

  std::vector<float> target_data = {
      50.0f, 50.0f, 50.0f, 50.0f, -9999.0f, 50.0f, 50.0f, 50.0f, 50.0f,
  };
  ASSERT_EQ(CE_None,
            band->RasterIO(GF_Write, 0, 0, kWidth, kHeight, target_data.data(),
                           kWidth, kHeight, GDT_Float32, 0, 0));

  CPLStringList options = GetDefaultFillOptions();
  EXPECT_EQ(CE_None,
            GDALFillNodata(GDALRasterBand::ToHandle(band), nullptr, 0.0, 0, 0,
                           options.List(), nullptr, nullptr));

  std::vector<float> result(kWidth * kHeight);
  ASSERT_EQ(CE_None,
            band->RasterIO(GF_Read, 0, 0, kWidth, kHeight, result.data(),
                           kWidth, kHeight, GDT_Float32, 0, 0));
  EXPECT_NEAR(50.0f, result[4], 1e-4f);
}

// 25. Compare smoothing iterations 0, 1, and 2 on a 5x5 raster.
TEST_F(RasterFillTest, SmoothingIterationsZeroOneTwo) {
  constexpr int kSize = 5;
  for (int smoothing_iters : {0, 1, 2}) {
    GDALDatasetUniquePtr target_ds = CreateMemDataset(kSize, kSize);
    ASSERT_NE(nullptr, target_ds);
    GDALRasterBand* target_band = target_ds->GetRasterBand(1);
    ASSERT_NE(nullptr, target_band);

    GDALDatasetUniquePtr mask_ds = CreateMemDataset(kSize, kSize, GDT_Byte);
    ASSERT_NE(nullptr, mask_ds);
    GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
    ASSERT_NE(nullptr, mask_band);

    std::vector<float> target_data(kSize * kSize, 0.0f);
    std::vector<uint8_t> mask_data(kSize * kSize, 0);

    for (int y = 0; y < kSize; ++y) {
      for (int x = 0; x < kSize; ++x) {
        if (x == 0 || x == kSize - 1 || y == 0 || y == kSize - 1) {
          target_data[y * kSize + x] = 100.0f;
          mask_data[y * kSize + x] = 255;
        }
      }
    }

    ASSERT_EQ(CE_None, target_band->RasterIO(GF_Write, 0, 0, kSize, kSize,
                                             target_data.data(), kSize, kSize,
                                             GDT_Float32, 0, 0));
    ASSERT_EQ(CE_None, mask_band->RasterIO(GF_Write, 0, 0, kSize, kSize,
                                           mask_data.data(), kSize, kSize,
                                           GDT_Byte, 0, 0));

    CPLStringList options = GetDefaultFillOptions();
    EXPECT_EQ(CE_None, GDALFillNodata(GDALRasterBand::ToHandle(target_band),
                                      GDALRasterBand::ToHandle(mask_band), 0.0,
                                      0, smoothing_iters, options.List(),
                                      nullptr, nullptr));

    std::vector<float> result(kSize * kSize);
    ASSERT_EQ(CE_None,
              target_band->RasterIO(GF_Read, 0, 0, kSize, kSize, result.data(),
                                    kSize, kSize, GDT_Float32, 0, 0));
    // Center pixel (2, 2) is filled with 100.0f
    EXPECT_NEAR(100.0f, result[2 * kSize + 2], 1e-4f);
    // Boundary pixels remain 100.0f
    EXPECT_FLOAT_EQ(100.0f, result[0]);
  }
}

// 26. Smoothing with null mask band triggers GDALFlushRasterCache.
TEST_F(RasterFillTest, SmoothingWithNullMaskFlushesCache) {
  constexpr int kSize = 5;
  GDALDatasetUniquePtr ds = CreateMemDataset(kSize, kSize);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  ASSERT_EQ(CE_None, band->SetNoDataValue(-9999.0));

  std::vector<float> target_data(kSize * kSize, 50.0f);
  target_data[2 * kSize + 2] = -9999.0f;

  ASSERT_EQ(CE_None,
            band->RasterIO(GF_Write, 0, 0, kSize, kSize, target_data.data(),
                           kSize, kSize, GDT_Float32, 0, 0));

  CPLStringList options = GetDefaultFillOptions();
  EXPECT_EQ(CE_None,
            GDALFillNodata(GDALRasterBand::ToHandle(band), nullptr, 0.0, 0, 1,
                           options.List(), nullptr, nullptr));

  std::vector<float> result(kSize * kSize);
  ASSERT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, kSize, kSize, result.data(),
                                    kSize, kSize, GDT_Float32, 0, 0));
  EXPECT_NEAR(50.0f, result[2 * kSize + 2], 1e-4f);
}

// 27. Search distance limits: small distance prevents filling distant holes.
TEST_F(RasterFillTest, SearchDistancesSmallAndLarge) {
  constexpr int kWidth = 7;
  constexpr int kHeight = 1;

  // Small search distance: distant pixels remain unfilled.
  {
    GDALDatasetUniquePtr target_ds = CreateMemDataset(kWidth, kHeight);
    ASSERT_NE(nullptr, target_ds);
    GDALRasterBand* target_band = target_ds->GetRasterBand(1);
    ASSERT_NE(nullptr, target_band);

    GDALDatasetUniquePtr mask_ds = CreateMemDataset(kWidth, kHeight, GDT_Byte);
    ASSERT_NE(nullptr, mask_ds);
    GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
    ASSERT_NE(nullptr, mask_band);

    std::vector<float> target_data = {10.0f, 0.0f, 0.0f, 0.0f,
                                      0.0f,  0.0f, 20.0f};
    std::vector<uint8_t> mask_data = {255, 0, 0, 0, 0, 0, 255};

    ASSERT_EQ(CE_None, target_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                             target_data.data(), kWidth,
                                             kHeight, GDT_Float32, 0, 0));
    ASSERT_EQ(CE_None, mask_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                           mask_data.data(), kWidth, kHeight,
                                           GDT_Byte, 0, 0));

    CPLStringList options = GetDefaultFillOptions();
    EXPECT_EQ(CE_None, GDALFillNodata(GDALRasterBand::ToHandle(target_band),
                                      GDALRasterBand::ToHandle(mask_band), 1.0,
                                      0, 0, options.List(), nullptr, nullptr));

    std::vector<float> result(kWidth);
    ASSERT_EQ(CE_None, target_band->RasterIO(GF_Read, 0, 0, kWidth, kHeight,
                                             result.data(), kWidth, kHeight,
                                             GDT_Float32, 0, 0));
    EXPECT_NEAR(10.0f, result[1], 1e-4f);
    EXPECT_FLOAT_EQ(0.0f, result[3]);
  }

  // Large search distance: all pixels are filled.
  {
    GDALDatasetUniquePtr target_ds = CreateMemDataset(kWidth, kHeight);
    ASSERT_NE(nullptr, target_ds);
    GDALRasterBand* target_band = target_ds->GetRasterBand(1);
    ASSERT_NE(nullptr, target_band);

    GDALDatasetUniquePtr mask_ds = CreateMemDataset(kWidth, kHeight, GDT_Byte);
    ASSERT_NE(nullptr, mask_ds);
    GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
    ASSERT_NE(nullptr, mask_band);

    std::vector<float> target_data = {10.0f, 0.0f, 0.0f, 0.0f,
                                      0.0f,  0.0f, 20.0f};
    std::vector<uint8_t> mask_data = {255, 0, 0, 0, 0, 0, 255};

    ASSERT_EQ(CE_None, target_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                             target_data.data(), kWidth,
                                             kHeight, GDT_Float32, 0, 0));
    ASSERT_EQ(CE_None, mask_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                           mask_data.data(), kWidth, kHeight,
                                           GDT_Byte, 0, 0));

    CPLStringList options = GetDefaultFillOptions();
    EXPECT_EQ(CE_None, GDALFillNodata(GDALRasterBand::ToHandle(target_band),
                                      GDALRasterBand::ToHandle(mask_band), 5.0,
                                      0, 0, options.List(), nullptr, nullptr));

    std::vector<float> result(kWidth);
    ASSERT_EQ(CE_None, target_band->RasterIO(GF_Read, 0, 0, kWidth, kHeight,
                                             result.data(), kWidth, kHeight,
                                             GDT_Float32, 0, 0));
    EXPECT_NEAR(15.0f, result[3], 1e-4f);
  }
}

// 28. Search steps >= 4 triggers periodic max search distance recomputation.
TEST_F(RasterFillTest, Step4MaxSearchDistRecompute) {
  constexpr int kSize = 15;
  GDALDatasetUniquePtr target_ds = CreateMemDataset(kSize, kSize);
  ASSERT_NE(nullptr, target_ds);
  GDALRasterBand* target_band = target_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, target_band);

  GDALDatasetUniquePtr mask_ds = CreateMemDataset(kSize, kSize, GDT_Byte);
  ASSERT_NE(nullptr, mask_ds);
  GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, mask_band);

  std::vector<float> target_data(kSize * kSize, 0.0f);
  std::vector<uint8_t> mask_data(kSize * kSize, 0);

  for (int y = 0; y < kSize; ++y) {
    for (int x = 0; x < kSize; ++x) {
      if (x < 3 || x >= 12 || y < 3 || y >= 12) {
        target_data[y * kSize + x] = 50.0f;
        mask_data[y * kSize + x] = 255;
      }
    }
  }

  ASSERT_EQ(CE_None, target_band->RasterIO(GF_Write, 0, 0, kSize, kSize,
                                           target_data.data(), kSize, kSize,
                                           GDT_Float32, 0, 0));
  ASSERT_EQ(CE_None,
            mask_band->RasterIO(GF_Write, 0, 0, kSize, kSize, mask_data.data(),
                                kSize, kSize, GDT_Byte, 0, 0));

  CPLStringList options = GetDefaultFillOptions();
  EXPECT_EQ(CE_None, GDALFillNodata(GDALRasterBand::ToHandle(target_band),
                                    GDALRasterBand::ToHandle(mask_band), 0.0, 0,
                                    0, options.List(), nullptr, nullptr));

  std::vector<float> result(kSize * kSize);
  ASSERT_EQ(CE_None,
            target_band->RasterIO(GF_Read, 0, 0, kSize, kSize, result.data(),
                                  kSize, kSize, GDT_Float32, 0, 0));
  EXPECT_NEAR(50.0f, result[7 * kSize + 7], 1e-4f);
}

// 29. Raster width > 65533 exercises GDT_UInt32 Y work buffer and nodata
// 4000002.
TEST_F(RasterFillTest, LargeRasterDimensionsX65534) {
  constexpr int kWidth = 65534;
  constexpr int kHeight = 1;
  GDALDatasetUniquePtr target_ds = CreateMemDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, target_ds);
  GDALRasterBand* target_band = target_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, target_band);

  GDALDatasetUniquePtr mask_ds = CreateMemDataset(kWidth, kHeight, GDT_Byte);
  ASSERT_NE(nullptr, mask_ds);
  GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, mask_band);

  float init_target[3] = {10.0f, 0.0f, 30.0f};
  uint8_t init_mask[3] = {255, 0, 255};

  ASSERT_EQ(CE_None, target_band->RasterIO(GF_Write, 0, 0, 3, 1, init_target, 3,
                                           1, GDT_Float32, 0, 0));
  ASSERT_EQ(CE_None, mask_band->RasterIO(GF_Write, 0, 0, 3, 1, init_mask, 3, 1,
                                         GDT_Byte, 0, 0));

  CPLStringList options = GetDefaultFillOptions();
  EXPECT_EQ(CE_None, GDALFillNodata(GDALRasterBand::ToHandle(target_band),
                                    GDALRasterBand::ToHandle(mask_band), 2.0, 0,
                                    0, options.List(), nullptr, nullptr));

  float filled_val = 0.0f;
  ASSERT_EQ(CE_None, target_band->RasterIO(GF_Read, 1, 0, 1, 1, &filled_val, 1,
                                           1, GDT_Float32, 0, 0));
  EXPECT_NEAR(20.0f, filled_val, 1e-4f);
}

// 30. Raster height > 65533 exercises GDT_UInt32 Y work buffer and nodata
// 4000002.
TEST_F(RasterFillTest, LargeRasterDimensionsY65534) {
  constexpr int kWidth = 1;
  constexpr int kHeight = 65534;
  GDALDatasetUniquePtr target_ds = CreateMemDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, target_ds);
  GDALRasterBand* target_band = target_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, target_band);

  GDALDatasetUniquePtr mask_ds = CreateMemDataset(kWidth, kHeight, GDT_Byte);
  ASSERT_NE(nullptr, mask_ds);
  GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, mask_band);

  float init_target[3] = {10.0f, 0.0f, 30.0f};
  uint8_t init_mask[3] = {255, 0, 255};

  ASSERT_EQ(CE_None, target_band->RasterIO(GF_Write, 0, 0, 1, 3, init_target, 1,
                                           3, GDT_Float32, 0, 0));
  ASSERT_EQ(CE_None, mask_band->RasterIO(GF_Write, 0, 0, 1, 3, init_mask, 1, 3,
                                         GDT_Byte, 0, 0));

  CPLStringList options = GetDefaultFillOptions();
  EXPECT_EQ(CE_None, GDALFillNodata(GDALRasterBand::ToHandle(target_band),
                                    GDALRasterBand::ToHandle(mask_band), 2.0, 0,
                                    0, options.List(), nullptr, nullptr));

  float filled_val = 0.0f;
  ASSERT_EQ(CE_None, target_band->RasterIO(GF_Read, 0, 1, 1, 1, &filled_val, 1,
                                           1, GDT_Float32, 0, 0));
  EXPECT_NEAR(20.0f, filled_val, 1e-4f);
}

// 31. NODATA option in NEAREST interpolation ignores neighbor matching NODATA.
TEST_F(RasterFillTest, NoDataOptionNearest) {
  constexpr int kWidth = 3;
  constexpr int kHeight = 3;
  GDALDatasetUniquePtr target_ds = CreateMemDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, target_ds);
  GDALRasterBand* target_band = target_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, target_band);

  GDALDatasetUniquePtr mask_ds = CreateMemDataset(kWidth, kHeight, GDT_Byte);
  ASSERT_NE(nullptr, mask_ds);
  GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, mask_band);

  std::vector<float> target_data = {
      0.0f, 999.0f, 0.0f, 0.0f, 0.0f, 42.0f, 0.0f, 0.0f, 0.0f,
  };
  std::vector<uint8_t> mask_data = {
      0, 255, 0, 0, 0, 255, 0, 0, 0,
  };

  ASSERT_EQ(CE_None, target_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                           target_data.data(), kWidth, kHeight,
                                           GDT_Float32, 0, 0));
  ASSERT_EQ(CE_None, mask_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                         mask_data.data(), kWidth, kHeight,
                                         GDT_Byte, 0, 0));

  CPLStringList options = GetDefaultFillOptions();
  options.SetNameValue("INTERPOLATION", "NEAREST");
  options.SetNameValue("NODATA", "999");

  EXPECT_EQ(CE_None, GDALFillNodata(GDALRasterBand::ToHandle(target_band),
                                    GDALRasterBand::ToHandle(mask_band), 2.0, 0,
                                    0, options.List(), nullptr, nullptr));

  std::vector<float> result(kWidth * kHeight);
  ASSERT_EQ(CE_None,
            target_band->RasterIO(GF_Read, 0, 0, kWidth, kHeight, result.data(),
                                  kWidth, kHeight, GDT_Float32, 0, 0));
  EXPECT_NEAR(42.0f, result[4], 1e-4f);
}

// 32. NODATA option in NEAREST interpolation when all neighbors match NODATA.
TEST_F(RasterFillTest, NoDataOptionNearestAllNoData) {
  constexpr int kWidth = 3;
  constexpr int kHeight = 3;
  GDALDatasetUniquePtr target_ds = CreateMemDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, target_ds);
  GDALRasterBand* target_band = target_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, target_band);

  GDALDatasetUniquePtr mask_ds = CreateMemDataset(kWidth, kHeight, GDT_Byte);
  ASSERT_NE(nullptr, mask_ds);
  GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, mask_band);

  std::vector<float> target_data(9, 999.0f);
  target_data[4] = 0.0f;
  std::vector<uint8_t> mask_data(9, 255);
  mask_data[4] = 0;

  ASSERT_EQ(CE_None, target_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                           target_data.data(), kWidth, kHeight,
                                           GDT_Float32, 0, 0));
  ASSERT_EQ(CE_None, mask_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                         mask_data.data(), kWidth, kHeight,
                                         GDT_Byte, 0, 0));

  CPLStringList options = GetDefaultFillOptions();
  options.SetNameValue("INTERPOLATION", "NEAREST");
  options.SetNameValue("NODATA", "999");

  EXPECT_EQ(CE_None, GDALFillNodata(GDALRasterBand::ToHandle(target_band),
                                    GDALRasterBand::ToHandle(mask_band), 2.0, 0,
                                    0, options.List(), nullptr, nullptr));

  std::vector<float> result(kWidth * kHeight);
  ASSERT_EQ(CE_None,
            target_band->RasterIO(GF_Read, 0, 0, kWidth, kHeight, result.data(),
                                  kWidth, kHeight, GDT_Float32, 0, 0));
  EXPECT_NEAR(999.0f, result[4], 1e-4f);
}

// 33. NODATA option in INV_DIST interpolation when all neighbors match NODATA.
TEST_F(RasterFillTest, NoDataOptionInvDistAllNoData) {
  constexpr int kWidth = 3;
  constexpr int kHeight = 3;
  GDALDatasetUniquePtr target_ds = CreateMemDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, target_ds);
  GDALRasterBand* target_band = target_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, target_band);

  GDALDatasetUniquePtr mask_ds = CreateMemDataset(kWidth, kHeight, GDT_Byte);
  ASSERT_NE(nullptr, mask_ds);
  GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, mask_band);

  std::vector<float> target_data(9, 999.0f);
  target_data[4] = 0.0f;
  std::vector<uint8_t> mask_data(9, 255);
  mask_data[4] = 0;

  ASSERT_EQ(CE_None, target_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                           target_data.data(), kWidth, kHeight,
                                           GDT_Float32, 0, 0));
  ASSERT_EQ(CE_None, mask_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                         mask_data.data(), kWidth, kHeight,
                                         GDT_Byte, 0, 0));

  CPLStringList options = GetDefaultFillOptions();
  options.SetNameValue("INTERPOLATION", "INV_DIST");
  options.SetNameValue("NODATA", "999");

  EXPECT_EQ(CE_None, GDALFillNodata(GDALRasterBand::ToHandle(target_band),
                                    GDALRasterBand::ToHandle(mask_band), 2.0, 0,
                                    0, options.List(), nullptr, nullptr));

  std::vector<float> result(kWidth * kHeight);
  ASSERT_EQ(CE_None,
            target_band->RasterIO(GF_Read, 0, 0, kWidth, kHeight, result.data(),
                                  kWidth, kHeight, GDT_Float32, 0, 0));
  EXPECT_NEAR(999.0f, result[4], 1e-4f);
}

// 34. Data type coverage: GDT_Byte raster band.
TEST_F(RasterFillTest, DataTypesByte) {
  constexpr int kWidth = 3;
  constexpr int kHeight = 3;
  GDALDatasetUniquePtr target_ds = CreateMemDataset(kWidth, kHeight, GDT_Byte);
  ASSERT_NE(nullptr, target_ds);
  GDALRasterBand* target_band = target_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, target_band);

  GDALDatasetUniquePtr mask_ds = CreateMemDataset(kWidth, kHeight, GDT_Byte);
  ASSERT_NE(nullptr, mask_ds);
  GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, mask_band);

  std::vector<uint8_t> target_data = {
      10, 10, 10, 10, 0, 30, 30, 30, 30,
  };
  std::vector<uint8_t> mask_data = {
      255, 255, 255, 255, 0, 255, 255, 255, 255,
  };

  ASSERT_EQ(CE_None, target_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                           target_data.data(), kWidth, kHeight,
                                           GDT_Byte, 0, 0));
  ASSERT_EQ(CE_None, mask_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                         mask_data.data(), kWidth, kHeight,
                                         GDT_Byte, 0, 0));

  CPLStringList options = GetDefaultFillOptions();
  EXPECT_EQ(CE_None, GDALFillNodata(GDALRasterBand::ToHandle(target_band),
                                    GDALRasterBand::ToHandle(mask_band), 2.0, 0,
                                    0, options.List(), nullptr, nullptr));

  std::vector<uint8_t> result(9);
  ASSERT_EQ(CE_None,
            target_band->RasterIO(GF_Read, 0, 0, kWidth, kHeight, result.data(),
                                  kWidth, kHeight, GDT_Byte, 0, 0));
  // 4 quadrants with weights (1, 1, 1, 1/sqrt(2)):
  // (10*1 + 30*(2 + 1/sqrt(2))) / (3 + 1/sqrt(2)) = 24.605 -> rounds to 25.
  EXPECT_EQ(25, result[4]);
}

// 35. Data type coverage: GDT_Float64 raster band.
TEST_F(RasterFillTest, DataTypesFloat64) {
  constexpr int kWidth = 3;
  constexpr int kHeight = 3;
  GDALDatasetUniquePtr target_ds =
      CreateMemDataset(kWidth, kHeight, GDT_Float64);
  ASSERT_NE(nullptr, target_ds);
  GDALRasterBand* target_band = target_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, target_band);

  GDALDatasetUniquePtr mask_ds = CreateMemDataset(kWidth, kHeight, GDT_Byte);
  ASSERT_NE(nullptr, mask_ds);
  GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, mask_band);

  std::vector<double> target_data = {
      100.25, 100.25, 100.25, 100.25, 0.0, 200.75, 200.75, 200.75, 200.75,
  };
  std::vector<uint8_t> mask_data = {
      255, 255, 255, 255, 0, 255, 255, 255, 255,
  };

  ASSERT_EQ(CE_None, target_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                           target_data.data(), kWidth, kHeight,
                                           GDT_Float64, 0, 0));
  ASSERT_EQ(CE_None, mask_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                         mask_data.data(), kWidth, kHeight,
                                         GDT_Byte, 0, 0));

  CPLStringList options = GetDefaultFillOptions();
  EXPECT_EQ(CE_None, GDALFillNodata(GDALRasterBand::ToHandle(target_band),
                                    GDALRasterBand::ToHandle(mask_band), 2.0, 0,
                                    0, options.List(), nullptr, nullptr));

  std::vector<double> result(9);
  ASSERT_EQ(CE_None,
            target_band->RasterIO(GF_Read, 0, 0, kWidth, kHeight, result.data(),
                                  kWidth, kHeight, GDT_Float64, 0, 0));
  // 4 quadrants with weights (1, 1, 1, 1/sqrt(2)):
  // (100.25*1 + 200.75*(2 + 1/sqrt(2))) / (3 + 1/sqrt(2)) = 173.6399f
  EXPECT_NEAR(173.6399, result[4], 1e-3);
}

// 36. Data type coverage: GDT_Int16 raster band with negative values.
TEST_F(RasterFillTest, DataTypesInt16) {
  constexpr int kWidth = 3;
  constexpr int kHeight = 3;
  GDALDatasetUniquePtr target_ds = CreateMemDataset(kWidth, kHeight, GDT_Int16);
  ASSERT_NE(nullptr, target_ds);
  GDALRasterBand* target_band = target_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, target_band);

  GDALDatasetUniquePtr mask_ds = CreateMemDataset(kWidth, kHeight, GDT_Byte);
  ASSERT_NE(nullptr, mask_ds);
  GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, mask_band);

  std::vector<int16_t> target_data = {
      -100, -100, -100, -100, 0, -200, -200, -200, -200,
  };
  std::vector<uint8_t> mask_data = {
      255, 255, 255, 255, 0, 255, 255, 255, 255,
  };

  ASSERT_EQ(CE_None, target_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                           target_data.data(), kWidth, kHeight,
                                           GDT_Int16, 0, 0));
  ASSERT_EQ(CE_None, mask_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                         mask_data.data(), kWidth, kHeight,
                                         GDT_Byte, 0, 0));

  CPLStringList options = GetDefaultFillOptions();
  EXPECT_EQ(CE_None, GDALFillNodata(GDALRasterBand::ToHandle(target_band),
                                    GDALRasterBand::ToHandle(mask_band), 2.0, 0,
                                    0, options.List(), nullptr, nullptr));

  std::vector<int16_t> result(9);
  ASSERT_EQ(CE_None,
            target_band->RasterIO(GF_Read, 0, 0, kWidth, kHeight, result.data(),
                                  kWidth, kHeight, GDT_Int16, 0, 0));
  // 4 quadrants with weights (1, 1, 1, 1/sqrt(2)):
  // (-100*1 + -200*(2 + 1/sqrt(2))) / (3 + 1/sqrt(2)) = -173.047 -> -173
  EXPECT_EQ(-173, result[4]);
}

// 37. Edge clipping and corner pixel interpolation.
TEST_F(RasterFillTest, EdgeClippingAndCorners) {
  constexpr int kWidth = 4;
  constexpr int kHeight = 4;
  GDALDatasetUniquePtr target_ds = CreateMemDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, target_ds);
  GDALRasterBand* target_band = target_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, target_band);

  GDALDatasetUniquePtr mask_ds = CreateMemDataset(kWidth, kHeight, GDT_Byte);
  ASSERT_NE(nullptr, mask_ds);
  GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, mask_band);

  std::vector<float> target_data(16, 50.0f);
  std::vector<uint8_t> mask_data(16, 255);

  // Mark all 4 corners and selected edges as nodata.
  mask_data[0] = 0;                       // (0, 0)
  mask_data[kWidth - 1] = 0;              // (3, 0)
  mask_data[(kHeight - 1) * kWidth] = 0;  // (0, 3)
  mask_data[15] = 0;                      // (3, 3)
  mask_data[1] = 0;                       // (1, 0) top edge
  mask_data[kWidth] = 0;                  // (0, 1) left edge
  mask_data[2 * kWidth + 3] = 0;          // (3, 2) right edge
  mask_data[14] = 0;                      // (2, 3) bottom edge

  ASSERT_EQ(CE_None, target_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                           target_data.data(), kWidth, kHeight,
                                           GDT_Float32, 0, 0));
  ASSERT_EQ(CE_None, mask_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                         mask_data.data(), kWidth, kHeight,
                                         GDT_Byte, 0, 0));

  CPLStringList options = GetDefaultFillOptions();
  EXPECT_EQ(CE_None, GDALFillNodata(GDALRasterBand::ToHandle(target_band),
                                    GDALRasterBand::ToHandle(mask_band), 0.0, 0,
                                    0, options.List(), nullptr, nullptr));

  std::vector<float> result(16);
  ASSERT_EQ(CE_None,
            target_band->RasterIO(GF_Read, 0, 0, kWidth, kHeight, result.data(),
                                  kWidth, kHeight, GDT_Float32, 0, 0));
  EXPECT_NEAR(50.0f, result[0], 1e-4f);
  EXPECT_NEAR(50.0f, result[3], 1e-4f);
  EXPECT_NEAR(50.0f, result[12], 1e-4f);
  EXPECT_NEAR(50.0f, result[15], 1e-4f);
}

// 38. Entire raster is nodata; completes cleanly with no source values.
TEST_F(RasterFillTest, AllNoDataRaster) {
  constexpr int kWidth = 4;
  constexpr int kHeight = 4;
  GDALDatasetUniquePtr target_ds = CreateMemDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, target_ds);
  GDALRasterBand* target_band = target_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, target_band);

  GDALDatasetUniquePtr mask_ds = CreateMemDataset(kWidth, kHeight, GDT_Byte);
  ASSERT_NE(nullptr, mask_ds);
  GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, mask_band);

  std::vector<float> target_data(16, 0.0f);
  std::vector<uint8_t> mask_data(16, 0);

  ASSERT_EQ(CE_None, target_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                           target_data.data(), kWidth, kHeight,
                                           GDT_Float32, 0, 0));
  ASSERT_EQ(CE_None, mask_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                         mask_data.data(), kWidth, kHeight,
                                         GDT_Byte, 0, 0));

  CPLStringList options = GetDefaultFillOptions();
  EXPECT_EQ(CE_None, GDALFillNodata(GDALRasterBand::ToHandle(target_band),
                                    GDALRasterBand::ToHandle(mask_band), 2.0, 0,
                                    0, options.List(), nullptr, nullptr));
}

// 39. Entire raster is valid; early exit in interpolation loop without
// modification.
TEST_F(RasterFillTest, AllValidRaster) {
  constexpr int kWidth = 4;
  constexpr int kHeight = 4;
  GDALDatasetUniquePtr target_ds = CreateMemDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, target_ds);
  GDALRasterBand* target_band = target_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, target_band);

  GDALDatasetUniquePtr mask_ds = CreateMemDataset(kWidth, kHeight, GDT_Byte);
  ASSERT_NE(nullptr, mask_ds);
  GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, mask_band);

  std::vector<float> target_data(16, 77.0f);
  std::vector<uint8_t> mask_data(16, 255);

  ASSERT_EQ(CE_None, target_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                           target_data.data(), kWidth, kHeight,
                                           GDT_Float32, 0, 0));
  ASSERT_EQ(CE_None, mask_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                         mask_data.data(), kWidth, kHeight,
                                         GDT_Byte, 0, 0));

  CPLStringList options = GetDefaultFillOptions();
  EXPECT_EQ(CE_None, GDALFillNodata(GDALRasterBand::ToHandle(target_band),
                                    GDALRasterBand::ToHandle(mask_band), 0.0, 0,
                                    0, options.List(), nullptr, nullptr));

  std::vector<float> result(16);
  ASSERT_EQ(CE_None,
            target_band->RasterIO(GF_Read, 0, 0, kWidth, kHeight, result.data(),
                                  kWidth, kHeight, GDT_Float32, 0, 0));
  EXPECT_FLOAT_EQ(77.0f, result[0]);
  EXPECT_FLOAT_EQ(77.0f, result[15]);
}

// 40. Top-down pass column exceeding max search distance resets panThisY to
// nNoDataVal.
TEST_F(RasterFillTest, TopDownColumnExceedingSearchDist) {
  constexpr int kWidth = 1;
  constexpr int kHeight = 4;
  GDALDatasetUniquePtr target_ds = CreateMemDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, target_ds);
  GDALRasterBand* target_band = target_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, target_band);

  GDALDatasetUniquePtr mask_ds = CreateMemDataset(kWidth, kHeight, GDT_Byte);
  ASSERT_NE(nullptr, mask_ds);
  GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, mask_band);

  std::vector<float> target_data = {10.0f, 0.0f, 0.0f, 0.0f};
  std::vector<uint8_t> mask_data = {255, 0, 0, 0};

  ASSERT_EQ(CE_None, target_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                           target_data.data(), kWidth, kHeight,
                                           GDT_Float32, 0, 0));
  ASSERT_EQ(CE_None, mask_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                         mask_data.data(), kWidth, kHeight,
                                         GDT_Byte, 0, 0));

  CPLStringList options = GetDefaultFillOptions();
  EXPECT_EQ(CE_None, GDALFillNodata(GDALRasterBand::ToHandle(target_band),
                                    GDALRasterBand::ToHandle(mask_band), 1.0, 0,
                                    0, options.List(), nullptr, nullptr));

  std::vector<float> result(4);
  ASSERT_EQ(CE_None,
            target_band->RasterIO(GF_Read, 0, 0, kWidth, kHeight, result.data(),
                                  kWidth, kHeight, GDT_Float32, 0, 0));
  EXPECT_NEAR(10.0f, result[1], 1e-4f);
  EXPECT_FLOAT_EQ(0.0f, result[3]);
}

// 41. Bottom-up pass column exceeding max search distance resets panThisY to
// nNoDataVal.
TEST_F(RasterFillTest, BottomUpColumnExceedingSearchDist) {
  constexpr int kWidth = 1;
  constexpr int kHeight = 4;
  GDALDatasetUniquePtr target_ds = CreateMemDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, target_ds);
  GDALRasterBand* target_band = target_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, target_band);

  GDALDatasetUniquePtr mask_ds = CreateMemDataset(kWidth, kHeight, GDT_Byte);
  ASSERT_NE(nullptr, mask_ds);
  GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, mask_band);

  std::vector<float> target_data = {0.0f, 0.0f, 0.0f, 10.0f};
  std::vector<uint8_t> mask_data = {0, 0, 0, 255};

  ASSERT_EQ(CE_None, target_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                           target_data.data(), kWidth, kHeight,
                                           GDT_Float32, 0, 0));
  ASSERT_EQ(CE_None, mask_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                         mask_data.data(), kWidth, kHeight,
                                         GDT_Byte, 0, 0));

  CPLStringList options = GetDefaultFillOptions();
  EXPECT_EQ(CE_None, GDALFillNodata(GDALRasterBand::ToHandle(target_band),
                                    GDALRasterBand::ToHandle(mask_band), 1.0, 0,
                                    0, options.List(), nullptr, nullptr));

  std::vector<float> result(4);
  ASSERT_EQ(CE_None,
            target_band->RasterIO(GF_Read, 0, 0, kWidth, kHeight, result.data(),
                                  kWidth, kHeight, GDT_Float32, 0, 0));
  EXPECT_NEAR(10.0f, result[2], 1e-4f);
  EXPECT_FLOAT_EQ(0.0f, result[0]);
}

// 42. GDALFilterLine comprehensive coverage over all 3x3 neighbor masks and
// scanlines.
TEST_F(RasterFillTest, FilterLineCoverageAllNeighbors) {
  constexpr int kWidth = 5;
  constexpr int kHeight = 5;
  GDALDatasetUniquePtr target_ds = CreateMemDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, target_ds);
  GDALRasterBand* target_band = target_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, target_band);

  GDALDatasetUniquePtr mask_ds = CreateMemDataset(kWidth, kHeight, GDT_Byte);
  ASSERT_NE(nullptr, mask_ds);
  GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, mask_band);

  std::vector<float> target_data(25, 0.0f);
  std::vector<uint8_t> mask_data(25, 0);

  for (int x = 0; x < kWidth; ++x) {
    target_data[x] = 10.0f;
    mask_data[x] = 255;
    target_data[4 * kWidth + x] = 30.0f;
    mask_data[4 * kWidth + x] = 255;
  }

  ASSERT_EQ(CE_None, target_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                           target_data.data(), kWidth, kHeight,
                                           GDT_Float32, 0, 0));
  ASSERT_EQ(CE_None, mask_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                         mask_data.data(), kWidth, kHeight,
                                         GDT_Byte, 0, 0));

  CPLStringList options = GetDefaultFillOptions();
  EXPECT_EQ(CE_None, GDALFillNodata(GDALRasterBand::ToHandle(target_band),
                                    GDALRasterBand::ToHandle(mask_band), 0.0, 0,
                                    2, options.List(), nullptr, nullptr));

  std::vector<float> result(25);
  ASSERT_EQ(CE_None,
            target_band->RasterIO(GF_Read, 0, 0, kWidth, kHeight, result.data(),
                                  kWidth, kHeight, GDT_Float32, 0, 0));
  EXPECT_NEAR(20.0f, result[2 * kWidth + 2], 1.0f);
}

// 43. Custom progress callback reporting success and tracking invocations.
TEST_F(RasterFillTest, CustomProgressReportingSuccess) {
  constexpr int kWidth = 5;
  constexpr int kHeight = 5;
  GDALDatasetUniquePtr ds = CreateMemDataset(kWidth, kHeight);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  CPLStringList options = GetDefaultFillOptions();

  int progress_calls = 0;
  auto progress_cb = [](double /*dfComplete*/, const char* /*pszMessage*/,
                        void* pData) -> int {
    auto* calls = static_cast<int*>(pData);
    (*calls)++;
    return TRUE;
  };

  EXPECT_EQ(CE_None,
            GDALFillNodata(GDALRasterBand::ToHandle(band), nullptr, 0.0, 0, 1,
                           options.List(), progress_cb, &progress_calls));
  EXPECT_GT(progress_calls, 0);
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Remaining uncovered branches are temporary file/OOM
// failure guards.
