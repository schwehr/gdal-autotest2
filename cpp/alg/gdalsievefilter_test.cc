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
// Tests sieve filter (alg/gdalsievefilter.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/gdalsievefilter.cpp

#include <cstdint>
#include <memory>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_progress.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

GDALDatasetUniquePtr CreateMemDataset(int width, int height, int bands = 1,
                                      GDALDataType type = GDT_Byte) {
  GDALRegister_MEM();
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  if (mem_driver == nullptr) {
    return nullptr;
  }
  return GDALDatasetUniquePtr(
      mem_driver->Create("", width, height, bands, type, nullptr));
}

template <typename T>
void WriteBandData(GDALRasterBand* band, const std::vector<T>& data) {
  const int x_size = band->GetXSize();
  const int y_size = band->GetYSize();
  const GDALDataType type = band->GetRasterDataType();
  EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, x_size, y_size,
                                    const_cast<T*>(data.data()), x_size, y_size,
                                    type, 0, 0));
}

template <typename T>
std::vector<T> ReadBandData(GDALRasterBand* band) {
  const int x_size = band->GetXSize();
  const int y_size = band->GetYSize();
  const GDALDataType type = band->GetRasterDataType();
  std::vector<T> data(static_cast<size_t>(x_size) * y_size);
  EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, x_size, y_size, data.data(),
                                    x_size, y_size, type, 0, 0));
  return data;
}

int CancelProgressPass1(double /*dfComplete*/, const char* /*pszMessage*/,
                        void* /*pProgressArg*/) {
  return FALSE;
}

int CancelProgressPass2(double dfComplete, const char* /*pszMessage*/,
                        void* /*pProgressArg*/) {
  if (dfComplete > 0.25) {
    return FALSE;
  }
  return TRUE;
}

int CancelProgressPass3(double dfComplete, const char* /*pszMessage*/,
                        void* /*pProgressArg*/) {
  if (dfComplete > 0.5) {
    return FALSE;
  }
  return TRUE;
}

struct ProgressTracker {
  std::vector<double> records;
};

int RecordProgress(double dfComplete, const char* /*pszMessage*/,
                   void* pProgressArg) {
  auto* tracker = static_cast<ProgressTracker*>(pProgressArg);
  tracker->records.push_back(dfComplete);
  return TRUE;
}

TEST(GdalSieveFilterTest, NullPointers) {
  GDALDatasetUniquePtr ds = CreateMemDataset(3, 3, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  // 1. Null source band.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALSieveFilter(nullptr, nullptr, band, 2, 4, nullptr,
                                          nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 2. Null destination band.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALSieveFilter(band, nullptr, nullptr, 2, 4, nullptr,
                                          nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST(GdalSieveFilterTest, NullProgress) {
  GDALDatasetUniquePtr ds = CreateMemDataset(3, 3, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  const std::vector<uint8_t> input = {
      10, 10, 10, 10, 20, 10, 10, 10, 10,
  };
  WriteBandData(band, input);

  EXPECT_EQ(CE_None, GDALSieveFilter(band, nullptr, band, 2, 4, nullptr,
                                     nullptr, nullptr));

  const std::vector<uint8_t> expected = {
      10, 10, 10, 10, 10, 10, 10, 10, 10,
  };
  EXPECT_EQ(expected, ReadBandData<uint8_t>(band));
}

TEST(GdalSieveFilterTest, ProgressInterruptPass1) {
  GDALDatasetUniquePtr ds = CreateMemDataset(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  const std::vector<uint8_t> input(16, 5);
  WriteBandData(band, input);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GDALSieveFilter(band, nullptr, band, 2, 4, nullptr,
                                        CancelProgressPass1, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("User terminated"));
  CPLErrorReset();
}

TEST(GdalSieveFilterTest, ProgressInterruptPass2) {
  GDALDatasetUniquePtr ds = CreateMemDataset(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  const std::vector<uint8_t> input(16, 5);
  WriteBandData(band, input);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GDALSieveFilter(band, nullptr, band, 2, 4, nullptr,
                                        CancelProgressPass2, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("User terminated"));
  CPLErrorReset();
}

TEST(GdalSieveFilterTest, ProgressInterruptPass3) {
  GDALDatasetUniquePtr ds = CreateMemDataset(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  const std::vector<uint8_t> input(16, 5);
  WriteBandData(band, input);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GDALSieveFilter(band, nullptr, band, 2, 4, nullptr,
                                        CancelProgressPass3, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("User terminated"));
  CPLErrorReset();
}

TEST(GdalSieveFilterTest, ProgressReportingSuccessful) {
  GDALDatasetUniquePtr ds = CreateMemDataset(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  const std::vector<uint8_t> input(16, 10);
  WriteBandData(band, input);

  ProgressTracker tracker;
  EXPECT_EQ(CE_None, GDALSieveFilter(band, nullptr, band, 2, 4, nullptr,
                                     RecordProgress, &tracker));
  EXPECT_FALSE(tracker.records.empty());
  EXPECT_DOUBLE_EQ(1.0, tracker.records.back());
}

TEST(GdalSieveFilterTest, AllPixelsMaskedInPlace) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(3, 3, 1, GDT_Byte);
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* src_band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, src_band);

  GDALDatasetUniquePtr mask_ds = CreateMemDataset(3, 3, 1, GDT_Byte);
  ASSERT_NE(nullptr, mask_ds);
  GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, mask_band);

  const std::vector<uint8_t> input = {
      1, 2, 3, 4, 5, 6, 7, 8, 9,
  };
  WriteBandData(src_band, input);

  const std::vector<uint8_t> mask(9, 0);  // All masked out.
  WriteBandData(mask_band, mask);

  EXPECT_EQ(CE_None, GDALSieveFilter(src_band, mask_band, src_band, 2, 4,
                                     nullptr, nullptr, nullptr));

  // In-place: data remains unchanged.
  EXPECT_EQ(input, ReadBandData<uint8_t>(src_band));
}

TEST(GdalSieveFilterTest, AllPixelsMaskedSeparateDst) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(3, 3, 1, GDT_Byte);
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* src_band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, src_band);

  GDALDatasetUniquePtr dst_ds = CreateMemDataset(3, 3, 1, GDT_Byte);
  ASSERT_NE(nullptr, dst_ds);
  GDALRasterBand* dst_band = dst_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, dst_band);

  GDALDatasetUniquePtr mask_ds = CreateMemDataset(3, 3, 1, GDT_Byte);
  ASSERT_NE(nullptr, mask_ds);
  GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, mask_band);

  const std::vector<uint8_t> input = {
      1, 2, 3, 4, 5, 6, 7, 8, 9,
  };
  WriteBandData(src_band, input);

  const std::vector<uint8_t> zeros(9, 0);
  WriteBandData(dst_band, zeros);
  WriteBandData(mask_band, zeros);  // All masked out.

  EXPECT_EQ(CE_None, GDALSieveFilter(src_band, mask_band, dst_band, 2, 4,
                                     nullptr, nullptr, nullptr));

  // Separate destination: whole raster is copied.
  EXPECT_EQ(input, ReadBandData<uint8_t>(dst_band));
}

TEST(GdalSieveFilterTest, MaskBandReadFailure) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* src_band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, src_band);

  // Mask band with only 1 row, causing GDALRasterIO to fail on row 1.
  GDALDatasetUniquePtr mask_ds = CreateMemDataset(4, 1, 1, GDT_Byte);
  ASSERT_NE(nullptr, mask_ds);
  GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, mask_band);
  std::vector<uint8_t> mask_row0(4, 1);
  EXPECT_EQ(CE_None, mask_band->RasterIO(GF_Write, 0, 0, 4, 1, mask_row0.data(),
                                         4, 1, GDT_Byte, 0, 0, nullptr));

  GDALDatasetUniquePtr dst_ds = CreateMemDataset(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, dst_ds);
  GDALRasterBand* dst_band = dst_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, dst_band);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GDALSieveFilter(src_band, mask_band, dst_band, 2, 4,
                                        nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST(GdalSieveFilterTest, BasicSinglePixelSieveInPlace) {
  GDALDatasetUniquePtr ds = CreateMemDataset(5, 5, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  std::vector<uint8_t> input(25, 10);
  input[2 * 5 + 2] = 20;  // Center pixel is a 1-pixel island of value 20.
  WriteBandData(band, input);

  EXPECT_EQ(CE_None, GDALSieveFilter(band, nullptr, band, 2, 4, nullptr,
                                     nullptr, nullptr));

  const std::vector<uint8_t> expected(25, 10);
  EXPECT_EQ(expected, ReadBandData<uint8_t>(band));
}

TEST(GdalSieveFilterTest, BasicSinglePixelSieveSeparateDst) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(5, 5, 1, GDT_Byte);
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* src_band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, src_band);

  GDALDatasetUniquePtr dst_ds = CreateMemDataset(5, 5, 1, GDT_Byte);
  ASSERT_NE(nullptr, dst_ds);
  GDALRasterBand* dst_band = dst_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, dst_band);

  std::vector<uint8_t> input(25, 10);
  input[2 * 5 + 2] = 20;
  WriteBandData(src_band, input);

  EXPECT_EQ(CE_None, GDALSieveFilter(src_band, nullptr, dst_band, 2, 4, nullptr,
                                     nullptr, nullptr));

  // Source band remains unchanged.
  EXPECT_EQ(input, ReadBandData<uint8_t>(src_band));

  // Destination band has sieved center pixel.
  const std::vector<uint8_t> expected(25, 10);
  EXPECT_EQ(expected, ReadBandData<uint8_t>(dst_band));
}

TEST(GdalSieveFilterTest, Connectivity4vs8) {
  // 1. Test 4-connectedness: diagonal pixels form separate polygons of size 1.
  {
    GDALDatasetUniquePtr ds = CreateMemDataset(3, 3, 1, GDT_Byte);
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    const std::vector<uint8_t> input = {
        20, 10, 10, 10, 20, 10, 10, 10, 10,
    };
    WriteBandData(band, input);

    EXPECT_EQ(CE_None, GDALSieveFilter(band, nullptr, band, 2, 4, nullptr,
                                       nullptr, nullptr));

    const std::vector<uint8_t> expected = {
        10, 10, 10, 10, 10, 10, 10, 10, 10,
    };
    EXPECT_EQ(expected, ReadBandData<uint8_t>(band));
  }

  // 2. Test 8-connectedness: diagonal pixels connect into 1 polygon of size 2.
  {
    GDALDatasetUniquePtr ds = CreateMemDataset(3, 3, 1, GDT_Byte);
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    const std::vector<uint8_t> input = {
        20, 10, 10, 10, 20, 10, 10, 10, 10,
    };
    WriteBandData(band, input);

    EXPECT_EQ(CE_None, GDALSieveFilter(band, nullptr, band, 2, 8, nullptr,
                                       nullptr, nullptr));

    // Size is 2 >= threshold (2), so not sieved away.
    const std::vector<uint8_t> expected = {
        20, 10, 10, 10, 20, 10, 10, 10, 10,
    };
    EXPECT_EQ(expected, ReadBandData<uint8_t>(band));
  }
}

TEST(GdalSieveFilterTest, Connectedness8Diagonals) {
  GDALDatasetUniquePtr ds = CreateMemDataset(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  const std::vector<uint8_t> input = {
      10, 20, 10, 30, 40, 50, 60, 10, 10, 70, 80, 10, 10, 10, 10, 10,
  };
  WriteBandData(band, input);

  EXPECT_EQ(CE_None, GDALSieveFilter(band, nullptr, band, 2, 8, nullptr,
                                     nullptr, nullptr));

  const std::vector<uint8_t> output = ReadBandData<uint8_t>(band);
  EXPECT_EQ(std::vector<uint8_t>(16, 10), output);
}

TEST(GdalSieveFilterTest, MergedPolygonFragments) {
  GDALDatasetUniquePtr ds = CreateMemDataset(3, 2, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  // U-shaped polygon:
  // Row 0 has (0,0)=20 and (2,0)=20 separated by (1,0)=10.
  // Row 1 has all 20s, merging (0,0) and (2,0) into one polygon of size 5.
  const std::vector<uint8_t> input = {
      20, 10, 20, 20, 20, 20,
  };
  WriteBandData(band, input);

  EXPECT_EQ(CE_None, GDALSieveFilter(band, nullptr, band, 2, 4, nullptr,
                                     nullptr, nullptr));

  // The 1-pixel island of 10 is merged into 20.
  const std::vector<uint8_t> expected = {
      20, 20, 20, 20, 20, 20,
  };
  EXPECT_EQ(expected, ReadBandData<uint8_t>(band));
}

TEST(GdalSieveFilterTest, IsolatedSmallPolygonWithMask) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(3, 3, 1, GDT_Byte);
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* src_band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, src_band);

  GDALDatasetUniquePtr mask_ds = CreateMemDataset(3, 3, 1, GDT_Byte);
  ASSERT_NE(nullptr, mask_ds);
  GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, mask_band);

  const std::vector<uint8_t> input = {
      10, 10, 10, 10, 50, 10, 10, 10, 10,
  };
  WriteBandData(src_band, input);

  // Only the center pixel is valid; all surrounding pixels are nodata.
  const std::vector<uint8_t> mask = {
      0, 0, 0, 0, 255, 0, 0, 0, 0,
  };
  WriteBandData(mask_band, mask);

  EXPECT_EQ(CE_None, GDALSieveFilter(src_band, mask_band, src_band, 2, 4,
                                     nullptr, nullptr, nullptr));

  // Isolated center pixel has no valid neighbours; remains 50.
  const std::vector<uint8_t> expected = {
      10, 10, 10, 10, 50, 10, 10, 10, 10,
  };
  EXPECT_EQ(expected, ReadBandData<uint8_t>(src_band));
}

TEST(GdalSieveFilterTest, MultiHopChain) {
  // A (size 1, val 10) touches B (size 2, val 20).
  // B touches C (size >= 10, val 30).
  // Threshold = 5.
  // Row 0: 10, 20, 20, 30, 30
  // Row 1:  M,  M,  M, 30, 30
  // Row 2: 30, 30, 30, 30, 30
  // Row 3: 30, 30, 30, 30, 30
  // Row 4: 30, 30, 30, 30, 30
  GDALDatasetUniquePtr src_ds = CreateMemDataset(5, 5, 1, GDT_Byte);
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* src_band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, src_band);

  GDALDatasetUniquePtr mask_ds = CreateMemDataset(5, 5, 1, GDT_Byte);
  ASSERT_NE(nullptr, mask_ds);
  GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, mask_band);

  std::vector<uint8_t> input(25, 30);
  input[0] = 10;
  input[1] = 20;
  input[2] = 20;
  WriteBandData(src_band, input);

  std::vector<uint8_t> mask(25, 255);
  mask[5] = 0;
  mask[6] = 0;
  mask[7] = 0;
  WriteBandData(mask_band, mask);

  EXPECT_EQ(CE_None, GDALSieveFilter(src_band, mask_band, src_band, 5, 4,
                                     nullptr, nullptr, nullptr));

  const std::vector<uint8_t> output = ReadBandData<uint8_t>(src_band);
  // (0,0) and (1,0) and (2,0) should have merged into 30.
  EXPECT_EQ(30, output[0]);
  EXPECT_EQ(30, output[1]);
  EXPECT_EQ(30, output[2]);
}

TEST(GdalSieveFilterTest, CycleUnmergableSmallPolygons) {
  // Two small polygons touch only each other, surrounded by borders.
  // Threshold = 5.
  GDALDatasetUniquePtr ds = CreateMemDataset(2, 1, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  const std::vector<uint8_t> input = {10, 20};
  WriteBandData(band, input);

  EXPECT_EQ(CE_None, GDALSieveFilter(band, nullptr, band, 5, 4, nullptr,
                                     nullptr, nullptr));

  // Neither polygon can reach threshold 5, so neither is altered.
  EXPECT_EQ(input, ReadBandData<uint8_t>(band));
}

TEST(GdalSieveFilterTest, CompareNeighbourUpdates) {
  GDALDatasetUniquePtr ds = CreateMemDataset(5, 3, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  // Row 0 has polygon 30 of size 5.
  // Row 1 has polygon 10 of size 5.
  // Row 2 has 20 (size 1), 40 (size 1), 50 (size 3).
  // Pixel 40 touches 10 (size 5), 20 (size 1), and 50 (size 3).
  const std::vector<uint8_t> input = {
      30, 30, 30, 30, 30, 10, 10, 10, 10, 10, 20, 40, 50, 50, 50,
  };
  WriteBandData(band, input);

  EXPECT_EQ(CE_None, GDALSieveFilter(band, nullptr, band, 2, 4, nullptr,
                                     nullptr, nullptr));

  const std::vector<uint8_t> output = ReadBandData<uint8_t>(band);
  // Pixel 40 (size 1) largest neighbour is 10 (size 5).
  EXPECT_EQ(10, output[11]);
}

TEST(GdalSieveFilterTest, VariousDataTypes) {
  // 1. Int16 band.
  {
    GDALDatasetUniquePtr ds = CreateMemDataset(3, 3, 1, GDT_Int16);
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    const std::vector<int16_t> input = {
        1000, 1000, 1000, 1000, 2000, 1000, 1000, 1000, 1000,
    };
    WriteBandData(band, input);

    EXPECT_EQ(CE_None, GDALSieveFilter(band, nullptr, band, 2, 4, nullptr,
                                       nullptr, nullptr));

    const std::vector<int16_t> expected(9, 1000);
    EXPECT_EQ(expected, ReadBandData<int16_t>(band));
  }

  // 2. Int32 band.
  {
    GDALDatasetUniquePtr ds = CreateMemDataset(3, 3, 1, GDT_Int32);
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    const std::vector<int32_t> input = {
        50000, 50000, 50000, 50000, 90000, 50000, 50000, 50000, 50000,
    };
    WriteBandData(band, input);

    EXPECT_EQ(CE_None, GDALSieveFilter(band, nullptr, band, 2, 4, nullptr,
                                       nullptr, nullptr));

    const std::vector<int32_t> expected(9, 50000);
    EXPECT_EQ(expected, ReadBandData<int32_t>(band));
  }

  // 3. Float32 band.
  {
    GDALDatasetUniquePtr ds = CreateMemDataset(3, 3, 1, GDT_Float32);
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    const std::vector<float> input = {
        10.0f, 10.0f, 10.0f, 10.0f, 20.0f, 10.0f, 10.0f, 10.0f, 10.0f,
    };
    WriteBandData(band, input);

    EXPECT_EQ(CE_None, GDALSieveFilter(band, nullptr, band, 2, 4, nullptr,
                                       nullptr, nullptr));

    const std::vector<float> expected(9, 10.0f);
    EXPECT_EQ(expected, ReadBandData<float>(band));
  }
}

TEST(GdalSieveFilterTest, ThresholdZeroAndOne) {
  GDALDatasetUniquePtr ds = CreateMemDataset(3, 3, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  const std::vector<uint8_t> input = {
      10, 10, 10, 10, 20, 10, 10, 10, 10,
  };
  WriteBandData(band, input);

  // Threshold 0: no polygons smaller than 0.
  EXPECT_EQ(CE_None, GDALSieveFilter(band, nullptr, band, 0, 4, nullptr,
                                     nullptr, nullptr));
  EXPECT_EQ(input, ReadBandData<uint8_t>(band));

  // Threshold 1: no polygons smaller than 1.
  EXPECT_EQ(CE_None, GDALSieveFilter(band, nullptr, band, 1, 4, nullptr,
                                     nullptr, nullptr));
  EXPECT_EQ(input, ReadBandData<uint8_t>(band));
}

TEST(GdalSieveFilterTest, SinglePixelRaster) {
  GDALDatasetUniquePtr ds = CreateMemDataset(1, 1, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  const std::vector<uint8_t> input = {42};
  WriteBandData(band, input);

  EXPECT_EQ(CE_None, GDALSieveFilter(band, nullptr, band, 2, 4, nullptr,
                                     nullptr, nullptr));

  // Isolated 1-pixel raster has no neighbours; remains 42.
  EXPECT_EQ(input, ReadBandData<uint8_t>(band));
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Sieve filter 32-bit polygon allocation and edge
// conditions.
