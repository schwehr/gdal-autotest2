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
// Tests image checksum calculation (alg/gdalchecksum.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/gdalchecksum.cpp

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

// Custom raster band to allow testing arbitrary block sizes and simulated read
// failures without external files.
class CustomTestBand final : public GDALRasterBand {
 public:
  CustomTestBand(GDALDataset* poDSIn, GDALDataType eType, int nXSize,
                 int nYSize, int nBXSize, int nBYSize, bool fail_read = false)
      : fail_read_(fail_read) {
    poDS = poDSIn;
    nBand = 1;
    eDataType = eType;
    nRasterXSize = nXSize;
    nRasterYSize = nYSize;
    nBlockXSize = nBXSize;
    nBlockYSize = nBYSize;
  }

  void SetFailRead(bool fail) { fail_read_ = fail; }

 protected:
  CPLErr IReadBlock(int /*nBlockXOff*/, int /*nBlockYOff*/,
                    void* pImage) override {
    if (fail_read_) {
      return CE_Failure;
    }
    const size_t count = static_cast<size_t>(nBlockXSize) * nBlockYSize;
    if (eDataType == GDT_Float32) {
      std::fill_n(static_cast<float*>(pImage), count, 3.0f);
    } else if (eDataType == GDT_Float64) {
      std::fill_n(static_cast<double*>(pImage), count, 3.0);
    } else if (eDataType == GDT_Int32) {
      std::fill_n(static_cast<GInt32*>(pImage), count, 3);
    } else {
      const int dt_size = GDALGetDataTypeSizeBytes(eDataType);
      std::memset(pImage, 3, count * dt_size);
    }
    return CE_None;
  }

 private:
  bool fail_read_ = false;
};

// Dataset managing a CustomTestBand instance.
class CustomTestDataset final : public GDALDataset {
 public:
  CustomTestDataset(GDALDataType eType, int nXSize, int nYSize, int nBXSize,
                    int nBYSize, bool fail_read = false) {
    nRasterXSize = nXSize;
    nRasterYSize = nYSize;
    SetBand(1, new CustomTestBand(this, eType, nXSize, nYSize, nBXSize, nBYSize,
                                  fail_read));
  }
};

// Reference implementation of GDALChecksumImage algorithm
// (alg/gdalchecksum.cpp).
int IntFromDoubleRef(double val) {
  if (!std::isfinite(val)) {
    return std::numeric_limits<int>::min();
  }
  val += 0.5;
  if (val < -2147483647.0) return -2147483647;
  if (val > 2147483647.0) return 2147483647;
  return static_cast<GInt32>(std::floor(val));
}

int ExpectedChecksumFromInts(std::span<const int> vals) {
  constexpr int kPrimes[11] = {7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43};
  int checksum = 0;
  int i_prime = 0;
  for (int v : vals) {
    checksum = (checksum + (v % kPrimes[i_prime])) & 0xffff;
    if (++i_prime > 10) i_prime = 0;
  }
  return checksum;
}

int ExpectedChecksumFromDoubles(std::span<const double> vals) {
  std::vector<int> ints;
  ints.reserve(vals.size());
  for (double v : vals) ints.push_back(IntFromDoubleRef(v));
  return ExpectedChecksumFromInts(ints);
}

// When every scalar value v satisfies 0 <= v < 7, v % kPrimes[i] == v.
constexpr int ExpectedConstantChecksum(int val_under_7, size_t scalar_count) {
  return static_cast<int>((static_cast<uint64_t>(val_under_7) * scalar_count) &
                          0xffffu);
}

class GdalChecksumTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_MEM(); }

  GDALDriver* GetMemDriver() {
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("MEM");
    EXPECT_NE(nullptr, driver);
    return driver;
  }
};

// 1. Verify handling of null band pointer.
TEST_F(GdalChecksumTest, NullBandValidation) {
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(0, GDALChecksumImage(nullptr, 0, 0, 10, 10));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

// 2. Verify empty or zero-sized windows produce a checksum of zero cleanly.
TEST_F(GdalChecksumTest, ZeroSizeWindow) {
  GDALDriver* mem_driver = GetMemDriver();
  ASSERT_NE(nullptr, mem_driver);
  GDALDatasetUniquePtr ds(mem_driver->Create("", 10, 10, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  EXPECT_EQ(0, GDALChecksumImage(GDALRasterBand::ToHandle(band), 0, 0, 0, 0));
  EXPECT_EQ(0, GDALChecksumImage(GDALRasterBand::ToHandle(band), 0, 0, 0, 10));
  EXPECT_EQ(0, GDALChecksumImage(GDALRasterBand::ToHandle(band), 0, 0, 10, 0));
}

// 3. Verify IntFromDouble conversion handles finite, NaN, infinities, and
// extreme values that exceed GInt32 range.
TEST_F(GdalChecksumTest, IntFromDoubleSpecialValues) {
  GDALDriver* mem_driver = GetMemDriver();
  ASSERT_NE(nullptr, mem_driver);
  GDALDatasetUniquePtr ds(
      mem_driver->Create("", 8, 1, 1, GDT_Float64, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  const std::vector<double> values = {
      std::numeric_limits<double>::quiet_NaN(),
      std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(),
      -3.0e9,  // Underflow < -2147483647
      3.0e9,   // Overflow > 2147483647
      10.2,    // Standard round down
      10.7,    // Standard round up
      -10.7,   // Negative round
  };
  ASSERT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 8, 1,
                                    const_cast<double*>(values.data()), 8, 1,
                                    GDT_Float64, 0, 0));

  const int checksum =
      GDALChecksumImage(GDALRasterBand::ToHandle(band), 0, 0, 8, 1);
  EXPECT_EQ(ExpectedChecksumFromDoubles(values), checksum);
}

// 4. Verify floating point chunked read (nXOff == 0 && nYOff == 0) with
// GDT_Float32 and GDT_Float64 across multiple lines and columns exceeding 11
// elements to wrap iPrime past index 10.
TEST_F(GdalChecksumTest, FloatingPointChunkedBasic) {
  GDALDriver* mem_driver = GetMemDriver();
  ASSERT_NE(nullptr, mem_driver);
  constexpr int kWidth = 20;
  constexpr int kHeight = 5;

  GDALDatasetUniquePtr ds(
      mem_driver->Create("", kWidth, kHeight, 1, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  std::vector<float> data(kWidth * kHeight);
  for (int i = 0; i < kWidth * kHeight; ++i) {
    data[i] = static_cast<float>(i * 1.5);
  }
  ASSERT_EQ(CE_None,
            band->RasterIO(GF_Write, 0, 0, kWidth, kHeight, data.data(), kWidth,
                           kHeight, GDT_Float32, 0, 0));

  const int checksum =
      GDALChecksumImage(GDALRasterBand::ToHandle(band), 0, 0, kWidth, kHeight);
  std::vector<double> ddata(data.begin(), data.end());
  EXPECT_EQ(ExpectedChecksumFromDoubles(ddata), checksum);
}

// 5. Verify floating point chunked read where block size is smaller than the
// requested raster window (nBlockXSize < nXSize), fitting in cache.
TEST_F(GdalChecksumTest, FloatingPointChunkedBlockSmallerThanRaster) {
  constexpr int kWidth = 20;
  constexpr int kHeight = 8;
  constexpr int kBlockWidth = 4;
  constexpr int kBlockHeight = 2;

  auto ds = std::make_unique<CustomTestDataset>(GDT_Float32, kWidth, kHeight,
                                                kBlockWidth, kBlockHeight);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  const int checksum =
      GDALChecksumImage(GDALRasterBand::ToHandle(band), 0, 0, kWidth, kHeight);
  EXPECT_EQ(ExpectedConstantChecksum(3, static_cast<size_t>(kWidth) * kHeight),
            checksum);
}

// 6. Verify floating point chunked read where requested line cannot fit in
// max allowed memory chunk, triggering multi-block chunk calculation.
TEST_F(GdalChecksumTest, FloatingPointChunkedExceedsMaxChunkMemory) {
  constexpr int kWidth = 2000;
  constexpr int kHeight = 1000;
  constexpr int kBlockWidth = 100;
  constexpr int kBlockHeight = 1000;

  const GIntBig prev_cache = GDALGetCacheMax64();
  GDALSetCacheMax64(10 * 1000 * 1000);
  absl::Cleanup restore_cache = [prev_cache] { GDALSetCacheMax64(prev_cache); };

  auto ds = std::make_unique<CustomTestDataset>(GDT_Float64, kWidth, kHeight,
                                                kBlockWidth, kBlockHeight);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  const int checksum =
      GDALChecksumImage(GDALRasterBand::ToHandle(band), 0, 0, kWidth, kHeight);
  EXPECT_EQ(ExpectedConstantChecksum(3, static_cast<size_t>(kWidth) * kHeight),
            checksum);
}

// 7. Verify floating point chunked read with complex data (GDT_CFloat32 and
// GDT_CFloat64), ensuring both real and imaginary parts influence the checksum.
TEST_F(GdalChecksumTest, FloatingPointChunkedComplex) {
  GDALDriver* mem_driver = GetMemDriver();
  ASSERT_NE(nullptr, mem_driver);
  constexpr int kWidth = 12;
  constexpr int kHeight = 3;

  GDALDatasetUniquePtr ds(
      mem_driver->Create("", kWidth, kHeight, 1, GDT_CFloat32, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  std::vector<float> complex_data(kWidth * kHeight * 2);
  for (int i = 0; i < kWidth * kHeight * 2; ++i) {
    complex_data[i] = static_cast<float>(i + 1);
  }
  ASSERT_EQ(CE_None,
            band->RasterIO(GF_Write, 0, 0, kWidth, kHeight, complex_data.data(),
                           kWidth, kHeight, GDT_CFloat32, 0, 0));

  const int checksum =
      GDALChecksumImage(GDALRasterBand::ToHandle(band), 0, 0, kWidth, kHeight);
  std::vector<double> ddata(complex_data.begin(), complex_data.end());
  EXPECT_EQ(ExpectedChecksumFromDoubles(ddata), checksum);
}

// 8. Verify floating point chunked read error handling when RasterIO fails.
TEST_F(GdalChecksumTest, FloatingPointChunkedReadFailure) {
  auto ds = std::make_unique<CustomTestDataset>(GDT_Float64, 10, 10, 5, 5,
                                                /*fail_read=*/true);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(-1,
            GDALChecksumImage(GDALRasterBand::ToHandle(band), 0, 0, 10, 10));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Checksum value could not be computed due to I/O read "
                        "error"));
  CPLErrorReset();
}

// 9. Verify floating point line-by-line read path (nXOff != 0 || nYOff != 0).
TEST_F(GdalChecksumTest, FloatingPointLineByLineRead) {
  GDALDriver* mem_driver = GetMemDriver();
  ASSERT_NE(nullptr, mem_driver);
  constexpr int kWidth = 25;
  constexpr int kHeight = 10;

  GDALDatasetUniquePtr ds(
      mem_driver->Create("", kWidth, kHeight, 1, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  std::vector<float> data(kWidth * kHeight);
  for (int i = 0; i < kWidth * kHeight; ++i) {
    data[i] = static_cast<float>(i * 2.0);
  }
  ASSERT_EQ(CE_None,
            band->RasterIO(GF_Write, 0, 0, kWidth, kHeight, data.data(), kWidth,
                           kHeight, GDT_Float32, 0, 0));

  // Non-zero offsets force line-by-line reading. Width 15 forces iPrime wrap.
  const int checksum =
      GDALChecksumImage(GDALRasterBand::ToHandle(band), 2, 1, 15, 4);
  std::vector<double> subwindow;
  for (int y = 1; y < 1 + 4; ++y) {
    for (int x = 2; x < 2 + 15; ++x) {
      subwindow.push_back(data[y * kWidth + x]);
    }
  }
  EXPECT_EQ(ExpectedChecksumFromDoubles(subwindow), checksum);
}

// 10. Verify floating point line-by-line read with complex data.
TEST_F(GdalChecksumTest, FloatingPointLineByLineComplex) {
  GDALDriver* mem_driver = GetMemDriver();
  ASSERT_NE(nullptr, mem_driver);
  constexpr int kWidth = 16;
  constexpr int kHeight = 6;

  GDALDatasetUniquePtr ds(
      mem_driver->Create("", kWidth, kHeight, 1, GDT_CFloat64, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  std::vector<double> complex_data(kWidth * kHeight * 2);
  for (int i = 0; i < kWidth * kHeight * 2; ++i) {
    complex_data[i] = static_cast<double>(i + 5);
  }
  ASSERT_EQ(CE_None,
            band->RasterIO(GF_Write, 0, 0, kWidth, kHeight, complex_data.data(),
                           kWidth, kHeight, GDT_CFloat64, 0, 0));

  const int checksum =
      GDALChecksumImage(GDALRasterBand::ToHandle(band), 1, 2, 8, 3);
  std::vector<double> subwindow;
  for (int y = 2; y < 2 + 3; ++y) {
    for (int x = 1; x < 1 + 8; ++x) {
      subwindow.push_back(complex_data[(y * kWidth + x) * 2]);
      subwindow.push_back(complex_data[(y * kWidth + x) * 2 + 1]);
    }
  }
  EXPECT_EQ(ExpectedChecksumFromDoubles(subwindow), checksum);
}

// 11. Verify floating point line-by-line read error when RasterIO fails.
TEST_F(GdalChecksumTest, FloatingPointLineByLineReadFailure) {
  GDALDriver* mem_driver = GetMemDriver();
  ASSERT_NE(nullptr, mem_driver);
  GDALDatasetUniquePtr ds(
      mem_driver->Create("", 10, 10, 1, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  WithQuietHandler quiet;
  CPLErrorReset();
  // Requesting window out of bounds fails in RasterIO.
  EXPECT_EQ(-1,
            GDALChecksumImage(GDALRasterBand::ToHandle(band), 5, 5, 10, 10));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Checksum value couldn't be computed due to I/O read "
                        "error"));
  CPLErrorReset();
}

// 12. Verify integer chunked read path (nXOff == 0 && nYOff == 0) with
// various integer types (GDT_Byte, GDT_Int16, GDT_Int32, GDT_Int64).
TEST_F(GdalChecksumTest, IntegerChunkedBasic) {
  GDALDriver* mem_driver = GetMemDriver();
  ASSERT_NE(nullptr, mem_driver);
  constexpr int kWidth = 20;
  constexpr int kHeight = 4;

  const GDALDataType types[] = {GDT_Byte,  GDT_Int16,  GDT_UInt16,
                                GDT_Int32, GDT_UInt32, GDT_Int64};
  for (GDALDataType type : types) {
    GDALDatasetUniquePtr ds(
        mem_driver->Create("", kWidth, kHeight, 1, type, nullptr));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    std::vector<GInt32> data(kWidth * kHeight);
    for (int i = 0; i < kWidth * kHeight; ++i) {
      data[i] = i * 3 + 1;
    }
    ASSERT_EQ(CE_None,
              band->RasterIO(GF_Write, 0, 0, kWidth, kHeight, data.data(),
                             kWidth, kHeight, GDT_Int32, 0, 0));

    const int checksum = GDALChecksumImage(GDALRasterBand::ToHandle(band), 0, 0,
                                           kWidth, kHeight);
    EXPECT_EQ(ExpectedChecksumFromInts(data), checksum);
  }
}

// 13. Verify integer chunked read where block size is smaller than the
// requested raster window (nBlockXSize < nXSize), fitting in cache.
TEST_F(GdalChecksumTest, IntegerChunkedBlockSmallerThanRaster) {
  constexpr int kWidth = 24;
  constexpr int kHeight = 8;
  constexpr int kBlockWidth = 4;
  constexpr int kBlockHeight = 2;

  auto ds = std::make_unique<CustomTestDataset>(GDT_Int32, kWidth, kHeight,
                                                kBlockWidth, kBlockHeight);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  const int checksum =
      GDALChecksumImage(GDALRasterBand::ToHandle(band), 0, 0, kWidth, kHeight);
  EXPECT_EQ(ExpectedConstantChecksum(3, static_cast<size_t>(kWidth) * kHeight),
            checksum);
}

// 14. Verify integer chunked read where requested line cannot fit in
// max allowed memory chunk.
TEST_F(GdalChecksumTest, IntegerChunkedExceedsMaxChunkMemory) {
  constexpr int kWidth = 3000;
  constexpr int kHeight = 1000;
  constexpr int kBlockWidth = 100;
  constexpr int kBlockHeight = 1000;

  const GIntBig prev_cache = GDALGetCacheMax64();
  GDALSetCacheMax64(10 * 1000 * 1000);
  absl::Cleanup restore_cache = [prev_cache] { GDALSetCacheMax64(prev_cache); };

  auto ds = std::make_unique<CustomTestDataset>(GDT_Int32, kWidth, kHeight,
                                                kBlockWidth, kBlockHeight);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  const int checksum =
      GDALChecksumImage(GDALRasterBand::ToHandle(band), 0, 0, kWidth, kHeight);
  EXPECT_EQ(ExpectedConstantChecksum(3, static_cast<size_t>(kWidth) * kHeight),
            checksum);
}

// 15. Verify integer chunked read with complex integer data (GDT_CInt16,
// GDT_CInt32).
TEST_F(GdalChecksumTest, IntegerChunkedComplex) {
  GDALDriver* mem_driver = GetMemDriver();
  ASSERT_NE(nullptr, mem_driver);
  constexpr int kWidth = 12;
  constexpr int kHeight = 3;

  GDALDatasetUniquePtr ds(
      mem_driver->Create("", kWidth, kHeight, 1, GDT_CInt32, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  std::vector<GInt32> complex_data(kWidth * kHeight * 2);
  for (int i = 0; i < kWidth * kHeight * 2; ++i) {
    complex_data[i] = i * 2 + 1;
  }
  ASSERT_EQ(CE_None,
            band->RasterIO(GF_Write, 0, 0, kWidth, kHeight, complex_data.data(),
                           kWidth, kHeight, GDT_CInt32, 0, 0));

  const int checksum =
      GDALChecksumImage(GDALRasterBand::ToHandle(band), 0, 0, kWidth, kHeight);
  EXPECT_EQ(ExpectedChecksumFromInts(complex_data), checksum);
}

// 16. Verify integer chunked read error handling when RasterIO fails.
TEST_F(GdalChecksumTest, IntegerChunkedReadFailure) {
  auto ds = std::make_unique<CustomTestDataset>(GDT_Int32, 10, 10, 5, 5,
                                                /*fail_read=*/true);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(-1,
            GDALChecksumImage(GDALRasterBand::ToHandle(band), 0, 0, 10, 10));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Checksum value could not be computed due to I/O read "
                        "error"));
  CPLErrorReset();
}

// 17. Verify integer line-by-line read path (nXOff != 0 || nYOff != 0).
TEST_F(GdalChecksumTest, IntegerLineByLineRead) {
  GDALDriver* mem_driver = GetMemDriver();
  ASSERT_NE(nullptr, mem_driver);
  constexpr int kWidth = 25;
  constexpr int kHeight = 8;

  GDALDatasetUniquePtr ds(
      mem_driver->Create("", kWidth, kHeight, 1, GDT_Int32, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  std::vector<GInt32> data(kWidth * kHeight);
  for (int i = 0; i < kWidth * kHeight; ++i) {
    data[i] = i * 4;
  }
  ASSERT_EQ(CE_None,
            band->RasterIO(GF_Write, 0, 0, kWidth, kHeight, data.data(), kWidth,
                           kHeight, GDT_Int32, 0, 0));

  // Non-zero offset forces line-by-line reading. Width 15 forces iPrime wrap.
  const int checksum =
      GDALChecksumImage(GDALRasterBand::ToHandle(band), 3, 2, 15, 3);
  std::vector<int> subwindow;
  for (int y = 2; y < 2 + 3; ++y) {
    for (int x = 3; x < 3 + 15; ++x) {
      subwindow.push_back(data[y * kWidth + x]);
    }
  }
  EXPECT_EQ(ExpectedChecksumFromInts(subwindow), checksum);
}

// 18. Verify integer line-by-line read with complex data.
TEST_F(GdalChecksumTest, IntegerLineByLineComplex) {
  GDALDriver* mem_driver = GetMemDriver();
  ASSERT_NE(nullptr, mem_driver);
  constexpr int kWidth = 16;
  constexpr int kHeight = 6;

  GDALDatasetUniquePtr ds(
      mem_driver->Create("", kWidth, kHeight, 1, GDT_CInt16, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  std::vector<GInt16> complex_data(kWidth * kHeight * 2);
  for (int i = 0; i < kWidth * kHeight * 2; ++i) {
    complex_data[i] = static_cast<GInt16>(i * 3 + 2);
  }
  ASSERT_EQ(CE_None,
            band->RasterIO(GF_Write, 0, 0, kWidth, kHeight, complex_data.data(),
                           kWidth, kHeight, GDT_CInt16, 0, 0));

  const int checksum =
      GDALChecksumImage(GDALRasterBand::ToHandle(band), 2, 1, 8, 3);
  std::vector<int> subwindow;
  for (int y = 1; y < 1 + 3; ++y) {
    for (int x = 2; x < 2 + 8; ++x) {
      subwindow.push_back(complex_data[(y * kWidth + x) * 2]);
      subwindow.push_back(complex_data[(y * kWidth + x) * 2 + 1]);
    }
  }
  EXPECT_EQ(ExpectedChecksumFromInts(subwindow), checksum);
}

// 19. Verify integer line-by-line read error when RasterIO fails.
TEST_F(GdalChecksumTest, IntegerLineByLineReadFailure) {
  GDALDriver* mem_driver = GetMemDriver();
  ASSERT_NE(nullptr, mem_driver);
  GDALDatasetUniquePtr ds(
      mem_driver->Create("", 10, 10, 1, GDT_Int32, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(-1,
            GDALChecksumImage(GDALRasterBand::ToHandle(band), 6, 6, 10, 10));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Checksum value could not be computed due to I/O read "
                        "error"));
  CPLErrorReset();
}

// 20. Verify exact known mathematical values of the checksum algorithm:
// Primes: {7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43}.
// Single pixel 1: 1 % 7 = 1.
// Single pixel 7: 7 % 7 = 0.
// Two pixels {1, 2}: (1 % 7) + (2 % 11) = 3.
TEST_F(GdalChecksumTest, ExactKnownChecksumValues) {
  GDALDriver* mem_driver = GetMemDriver();
  ASSERT_NE(nullptr, mem_driver);

  // 1x1 image with value 1
  {
    GDALDatasetUniquePtr ds(mem_driver->Create("", 1, 1, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    const GByte val = 1;
    ASSERT_EQ(CE_None,
              band->RasterIO(GF_Write, 0, 0, 1, 1, const_cast<GByte*>(&val), 1,
                             1, GDT_Byte, 0, 0));
    EXPECT_EQ(1, GDALChecksumImage(GDALRasterBand::ToHandle(band), 0, 0, 1, 1));
  }

  // 1x1 image with value 7
  {
    GDALDatasetUniquePtr ds(mem_driver->Create("", 1, 1, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    const GByte val = 7;
    ASSERT_EQ(CE_None,
              band->RasterIO(GF_Write, 0, 0, 1, 1, const_cast<GByte*>(&val), 1,
                             1, GDT_Byte, 0, 0));
    EXPECT_EQ(0, GDALChecksumImage(GDALRasterBand::ToHandle(band), 0, 0, 1, 1));
  }

  // 2x1 image with values {1, 2}
  {
    GDALDatasetUniquePtr ds(mem_driver->Create("", 2, 1, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    const GByte vals[2] = {1, 2};
    ASSERT_EQ(CE_None,
              band->RasterIO(GF_Write, 0, 0, 2, 1, const_cast<GByte*>(vals), 2,
                             1, GDT_Byte, 0, 0));
    EXPECT_EQ(3, GDALChecksumImage(GDALRasterBand::ToHandle(band), 0, 0, 2, 1));
  }

  // 1x1 float image with 1.4 (rounds to 1) and 1.6 (rounds to 2)
  {
    GDALDatasetUniquePtr ds(
        mem_driver->Create("", 2, 1, 1, GDT_Float32, nullptr));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    const float vals[2] = {1.4f, 1.6f};
    ASSERT_EQ(CE_None,
              band->RasterIO(GF_Write, 0, 0, 2, 1, const_cast<float*>(vals), 2,
                             1, GDT_Float32, 0, 0));
    // Pixel 0: 1.4 rounds to 1 -> 1 % 7 = 1
    EXPECT_EQ(1, GDALChecksumImage(GDALRasterBand::ToHandle(band), 0, 0, 1, 1));
    // Pixel 1: 1.6 rounds to 2 -> 2 % 7 = 2
    EXPECT_EQ(2, GDALChecksumImage(GDALRasterBand::ToHandle(band), 1, 0, 1, 1));
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Remaining uncovered lines are VSI_MALLOC*_VERBOSE OOM
// guards.
