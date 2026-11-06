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
// Tests raster interpolation at point (alg/gdal_interpolateatpoint.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/gdal_interpolateatpoint.cpp

#include "third_party/gdal/alg/gdal_interpolateatpoint.h"

#include <complex>
#include <cstdint>
#include <memory>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

GDALDatasetUniquePtr CreateMemDataset(int width, int height, int bands = 1,
                                      GDALDataType type = GDT_Float64) {
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

TEST(GdalInterpolateAtPointTest, OutOfBoundsCoordinates) {
  GDALDatasetUniquePtr ds = CreateMemDataset(4, 4, 1, GDT_Float64);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  const std::vector<double> input(16, 1.0);
  WriteBandData(band, input);

  std::unique_ptr<DoublePointsCache> cache;
  double real_val = 0.0;
  double imag_val = 0.0;

  // 1. Negative X coordinate.
  EXPECT_FALSE(GDALInterpolateAtPoint(band, GRIORA_Bilinear, cache, -0.1, 2.0,
                                      &real_val, &imag_val));

  // 2. Negative Y coordinate.
  EXPECT_FALSE(GDALInterpolateAtPoint(band, GRIORA_Bilinear, cache, 2.0, -0.1,
                                      &real_val, &imag_val));

  // 3. X coordinate exceeds raster width.
  EXPECT_FALSE(GDALInterpolateAtPoint(band, GRIORA_Bilinear, cache, 4.1, 2.0,
                                      &real_val, &imag_val));

  // 4. Y coordinate exceeds raster height.
  EXPECT_FALSE(GDALInterpolateAtPoint(band, GRIORA_Bilinear, cache, 2.0, 4.1,
                                      &real_val, &imag_val));
}

TEST(GdalInterpolateAtPointTest, NearestNeighbourEdgeCoordinates) {
  GDALDatasetUniquePtr ds = CreateMemDataset(4, 4, 1, GDT_Float64);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  std::vector<double> input(16);
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      input[y * 4 + x] = static_cast<double>((y * 4 + x) * 10);
    }
  }
  WriteBandData(band, input);

  std::unique_ptr<DoublePointsCache> cache;
  double real_val = 0.0;
  double imag_val = 0.0;

  // 1. Coordinate exactly at right edge: dfXIn == 4.0.
  // NearestNeighbour shifts dfXIn -= 0.25 into column 3.
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_NearestNeighbour, cache, 4.0,
                                     2.0, &real_val, &imag_val));
  EXPECT_DOUBLE_EQ(110.0, real_val);  // row 2, col 3 -> (2*4+3)*10 = 110.0

  // 2. Coordinate exactly at bottom edge: dfYIn == 4.0.
  // NearestNeighbour shifts dfYIn -= 0.25 into row 3.
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_NearestNeighbour, cache, 2.0,
                                     4.0, &real_val, &imag_val));
  EXPECT_DOUBLE_EQ(140.0, real_val);  // row 3, col 2 -> (3*4+2)*10 = 140.0

  // 3. Coordinate at bottom-right corner: (4.0, 4.0).
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_NearestNeighbour, cache, 4.0,
                                     4.0, &real_val, &imag_val));
  EXPECT_DOUBLE_EQ(150.0, real_val);  // row 3, col 3 -> (3*4+3)*10 = 150.0

  // 4. Coordinate just beyond epsilon (4.0 + 1e-4) fails out of bounds.
  EXPECT_FALSE(GDALInterpolateAtPoint(band, GRIORA_NearestNeighbour, cache,
                                      4.0001, 2.0, &real_val, &imag_val));
}

TEST(GdalInterpolateAtPointTest, AlgorithmDowngradeSmallRasters) {
  // 1. 3x3 raster (< 4): CubicSpline and Cubic downgrade to Bilinear.
  {
    GDALDatasetUniquePtr ds = CreateMemDataset(3, 3, 1, GDT_Float64);
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    const std::vector<double> input(9, 15.0);
    WriteBandData(band, input);

    std::unique_ptr<DoublePointsCache> cache;
    double real_val = 0.0;
    double imag_val = 0.0;

    EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_CubicSpline, cache, 1.5,
                                       1.5, &real_val, &imag_val));
    EXPECT_DOUBLE_EQ(15.0, real_val);

    EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_Cubic, cache, 1.5, 1.5,
                                       &real_val, &imag_val));
    EXPECT_DOUBLE_EQ(15.0, real_val);
  }

  // 2. 1x1 raster (< 2): Bilinear downgrades to NearestNeighbour.
  {
    GDALDatasetUniquePtr ds = CreateMemDataset(1, 1, 1, GDT_Float64);
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    const std::vector<double> input = {42.0};
    WriteBandData(band, input);

    std::unique_ptr<DoublePointsCache> cache;
    double real_val = 0.0;
    double imag_val = 0.0;

    EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_Bilinear, cache, 0.5, 0.5,
                                       &real_val, &imag_val));
    EXPECT_DOUBLE_EQ(42.0, real_val);

    // CubicSpline on 1x1 downgrades to Bilinear, then to NearestNeighbour.
    EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_CubicSpline, cache, 0.5,
                                       0.5, &real_val, &imag_val));
    EXPECT_DOUBLE_EQ(42.0, real_val);

    EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_Cubic, cache, 0.5, 0.5,
                                       &real_val, &imag_val));
    EXPECT_DOUBLE_EQ(42.0, real_val);
  }

  // 3. Asymmetric small rasters (3x5 and 5x3).
  {
    GDALDatasetUniquePtr ds_x_small = CreateMemDataset(3, 5, 1, GDT_Float64);
    ASSERT_NE(nullptr, ds_x_small);
    GDALRasterBand* band_x = ds_x_small->GetRasterBand(1);
    ASSERT_NE(nullptr, band_x);
    const std::vector<double> input_x(15, 20.0);
    WriteBandData(band_x, input_x);

    std::unique_ptr<DoublePointsCache> cache;
    double real_val = 0.0;
    double imag_val = 0.0;
    EXPECT_TRUE(GDALInterpolateAtPoint(band_x, GRIORA_CubicSpline, cache, 1.5,
                                       2.5, &real_val, &imag_val));
    EXPECT_DOUBLE_EQ(20.0, real_val);

    GDALDatasetUniquePtr ds_y_small = CreateMemDataset(5, 3, 1, GDT_Float64);
    ASSERT_NE(nullptr, ds_y_small);
    GDALRasterBand* band_y = ds_y_small->GetRasterBand(1);
    ASSERT_NE(nullptr, band_y);
    const std::vector<double> input_y(15, 30.0);
    WriteBandData(band_y, input_y);
    cache.reset();

    EXPECT_TRUE(GDALInterpolateAtPoint(band_y, GRIORA_CubicSpline, cache, 2.5,
                                       1.5, &real_val, &imag_val));
    EXPECT_DOUBLE_EQ(30.0, real_val);
  }
}

TEST(GdalInterpolateAtPointTest, BorderDraggingAndCorrection) {
  GDALDatasetUniquePtr ds = CreateMemDataset(10, 10, 1, GDT_Float64);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  std::vector<double> input(100);
  for (int y = 0; y < 10; ++y) {
    for (int x = 0; x < 10; ++x) {
      input[y * 10 + x] = y * 10.0 + x;
    }
  }
  WriteBandData(band, input);

  std::unique_ptr<DoublePointsCache> cache;
  double real_val = 0.0;
  double imag_val = 0.0;

  // 1. Upper-left corner (0.1, 0.1): triggers dOutOfBorder < 0 for X and Y.
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_CubicSpline, cache, 0.1, 0.1,
                                     &real_val, &imag_val));
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_Cubic, cache, 0.1, 0.1,
                                     &real_val, &imag_val));
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_Bilinear, cache, 0.1, 0.1,
                                     &real_val, &imag_val));

  // 2. Lower-right corner (9.9, 9.9): triggers dOutOfBorder > 0 for X and Y.
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_CubicSpline, cache, 9.9, 9.9,
                                     &real_val, &imag_val));
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_Cubic, cache, 9.9, 9.9,
                                     &real_val, &imag_val));
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_Bilinear, cache, 9.9, 9.9,
                                     &real_val, &imag_val));

  // 3. Upper-right corner (9.9, 0.1): X > 0, Y < 0 out-of-border.
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_CubicSpline, cache, 9.9, 0.1,
                                     &real_val, &imag_val));

  // 4. Lower-left corner (0.1, 9.9): X < 0, Y > 0 out-of-border.
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_CubicSpline, cache, 0.1, 9.9,
                                     &real_val, &imag_val));
}

TEST(GdalInterpolateAtPointTest, AllAlgorithmsRealBand) {
  GDALDatasetUniquePtr ds = CreateMemDataset(8, 8, 1, GDT_Float64);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  // Linear ramp: f(x, y) = 10.0 + 2.0 * x + 3.0 * y.
  std::vector<double> input(64);
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      input[y * 8 + x] = 10.0 + 2.0 * x + 3.0 * y;
    }
  }
  WriteBandData(band, input);

  std::unique_ptr<DoublePointsCache> cache;
  double real_val = 0.0;
  double imag_val = 0.0;

  // 1. NearestNeighbour at (2.5, 3.5): center of pixel (2, 3) -> 10 + 4 + 9
  // = 23.
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_NearestNeighbour, cache, 2.5,
                                     3.5, &real_val, &imag_val));
  EXPECT_DOUBLE_EQ(23.0, real_val);
  EXPECT_DOUBLE_EQ(0.0, imag_val);

  // 2. Bilinear at (2.5, 3.5).
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_Bilinear, cache, 2.5, 3.5,
                                     &real_val, &imag_val));
  EXPECT_DOUBLE_EQ(23.0, real_val);
  EXPECT_DOUBLE_EQ(0.0, imag_val);

  // 3. Cubic at (2.5, 3.5).
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_Cubic, cache, 2.5, 3.5,
                                     &real_val, &imag_val));
  EXPECT_NEAR(23.0, real_val, 0.05);
  EXPECT_DOUBLE_EQ(0.0, imag_val);

  // 4. CubicSpline at (2.5, 3.5).
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_CubicSpline, cache, 2.5, 3.5,
                                     &real_val, &imag_val));
  EXPECT_NEAR(23.0, real_val, 0.05);
  EXPECT_DOUBLE_EQ(0.0, imag_val);

  // 5. Fallback algorithm (GRIORA_Average falls to else / 1x1 window).
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_Average, cache, 2.5, 3.5,
                                     &real_val, &imag_val));
  EXPECT_DOUBLE_EQ(23.0, real_val);
  EXPECT_DOUBLE_EQ(0.0, imag_val);
}

TEST(GdalInterpolateAtPointTest, RealBandNullImag) {
  GDALDatasetUniquePtr ds = CreateMemDataset(4, 4, 1, GDT_Float64);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  const std::vector<double> input(16, 50.0);
  WriteBandData(band, input);

  std::unique_ptr<DoublePointsCache> cache;
  double real_val = 0.0;

  // Passing pdfOutputImag == nullptr should not crash.
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_Bilinear, cache, 1.5, 1.5,
                                     &real_val, nullptr));
  EXPECT_DOUBLE_EQ(50.0, real_val);
}

TEST(GdalInterpolateAtPointTest, NoDataHandlingRealBand) {
  GDALDatasetUniquePtr ds = CreateMemDataset(4, 4, 1, GDT_Float64);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  EXPECT_EQ(CE_None, band->SetNoDataValue(-9999.0));

  std::vector<double> input(16, 10.0);
  input[1 * 4 + 1] = -9999.0;  // Pixel (1, 1) is nodata.
  WriteBandData(band, input);

  std::unique_ptr<DoublePointsCache> cache;
  double real_val = 0.0;
  double imag_val = 0.0;

  // 1. NearestNeighbour on nodata pixel returns false.
  EXPECT_FALSE(GDALInterpolateAtPoint(band, GRIORA_NearestNeighbour, cache, 1.5,
                                      1.5, &real_val, &imag_val));
  // NearestNeighbour on valid pixel returns true.
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_NearestNeighbour, cache, 0.5,
                                     0.5, &real_val, &imag_val));
  EXPECT_DOUBLE_EQ(10.0, real_val);

  // 2. Bilinear where 2x2 window touches nodata returns false.
  EXPECT_FALSE(GDALInterpolateAtPoint(band, GRIORA_Bilinear, cache, 1.5, 1.5,
                                      &real_val, &imag_val));
  // Bilinear where 2x2 window does not touch nodata returns true.
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_Bilinear, cache, 3.5, 3.5,
                                     &real_val, &imag_val));
  EXPECT_DOUBLE_EQ(10.0, real_val);

  // 3. CubicSpline skips nodata sample and averages remaining valid samples.
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_CubicSpline, cache, 1.5, 1.5,
                                     &real_val, &imag_val));
  EXPECT_DOUBLE_EQ(10.0, real_val);

  // 4. Cubic at (1.5, 1.5) has delta=(0,0) where the only non-zero weight is
  // at the nodata sample, so dfSumWeight == 0.0 and it returns false.
  EXPECT_FALSE(GDALInterpolateAtPoint(band, GRIORA_Cubic, cache, 1.5, 1.5,
                                      &real_val, &imag_val));
  // Off-center query at (1.7, 1.7) has non-zero weights on valid neighboring
  // pixels and succeeds.
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_Cubic, cache, 1.7, 1.7,
                                     &real_val, &imag_val));
  EXPECT_DOUBLE_EQ(10.0, real_val);

  // 5. When all 16 samples are nodata, Cubic and CubicSpline return false.
  const std::vector<double> all_nodata(16, -9999.0);
  WriteBandData(band, all_nodata);
  cache.reset();

  EXPECT_FALSE(GDALInterpolateAtPoint(band, GRIORA_CubicSpline, cache, 1.5, 1.5,
                                      &real_val, &imag_val));
  EXPECT_FALSE(GDALInterpolateAtPoint(band, GRIORA_Cubic, cache, 1.5, 1.5,
                                      &real_val, &imag_val));
}

TEST(GdalInterpolateAtPointTest, ComplexBandAllAlgorithms) {
  GDALDatasetUniquePtr ds = CreateMemDataset(8, 8, 1, GDT_CFloat64);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  std::vector<std::complex<double>> input(64);
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      input[y * 8 + x] = std::complex<double>(10.0 + 2.0 * x, 20.0 + 3.0 * y);
    }
  }
  WriteBandData(band, input);

  std::unique_ptr<DoublePointsCache> cache;
  double real_val = 0.0;
  double imag_val = 0.0;

  // 1. NearestNeighbour.
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_NearestNeighbour, cache, 2.5,
                                     3.5, &real_val, &imag_val));
  EXPECT_DOUBLE_EQ(14.0, real_val);  // x=2 -> 10 + 4 = 14
  EXPECT_DOUBLE_EQ(29.0, imag_val);  // y=3 -> 20 + 9 = 29

  // 2. Bilinear.
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_Bilinear, cache, 2.5, 3.5,
                                     &real_val, &imag_val));
  EXPECT_DOUBLE_EQ(14.0, real_val);
  EXPECT_DOUBLE_EQ(29.0, imag_val);

  // 3. Cubic.
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_Cubic, cache, 2.5, 3.5,
                                     &real_val, &imag_val));
  EXPECT_NEAR(14.0, real_val, 0.05);
  EXPECT_NEAR(29.0, imag_val, 0.05);

  // 4. CubicSpline.
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_CubicSpline, cache, 2.5, 3.5,
                                     &real_val, &imag_val));
  EXPECT_NEAR(14.0, real_val, 0.05);
  EXPECT_NEAR(29.0, imag_val, 0.05);

  // 5. Complex with null imag_val pointer.
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_Bilinear, cache, 2.5, 3.5,
                                     &real_val, nullptr));
  EXPECT_DOUBLE_EQ(14.0, real_val);
}

TEST(GdalInterpolateAtPointTest, ComplexBandNoData) {
  GDALDatasetUniquePtr ds = CreateMemDataset(4, 4, 1, GDT_CFloat64);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  EXPECT_EQ(CE_None, band->SetNoDataValue(-9999.0));

  std::vector<std::complex<double>> input(16, std::complex<double>(10.0, 20.0));
  input[1 * 4 + 1] = std::complex<double>(-9999.0, 0.0);  // Nodata real part.
  WriteBandData(band, input);

  std::unique_ptr<DoublePointsCache> cache;
  double real_val = 0.0;
  double imag_val = 0.0;

  // 1. NearestNeighbour on nodata returns false.
  EXPECT_FALSE(GDALInterpolateAtPoint(band, GRIORA_NearestNeighbour, cache, 1.5,
                                      1.5, &real_val, &imag_val));
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_NearestNeighbour, cache, 0.5,
                                     0.5, &real_val, &imag_val));
  EXPECT_DOUBLE_EQ(10.0, real_val);
  EXPECT_DOUBLE_EQ(20.0, imag_val);

  // 2. Bilinear touching nodata returns false.
  EXPECT_FALSE(GDALInterpolateAtPoint(band, GRIORA_Bilinear, cache, 1.5, 1.5,
                                      &real_val, &imag_val));
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_Bilinear, cache, 3.5, 3.5,
                                     &real_val, &imag_val));
  EXPECT_DOUBLE_EQ(10.0, real_val);
  EXPECT_DOUBLE_EQ(20.0, imag_val);

  // 3. CubicSpline skips nodata.
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_CubicSpline, cache, 1.5, 1.5,
                                     &real_val, &imag_val));
  EXPECT_DOUBLE_EQ(10.0, real_val);
  EXPECT_DOUBLE_EQ(20.0, imag_val);

  // 4. All nodata returns false.
  const std::vector<std::complex<double>> all_nodata(
      16, std::complex<double>(-9999.0, 0.0));
  WriteBandData(band, all_nodata);
  cache.reset();

  EXPECT_FALSE(GDALInterpolateAtPoint(band, GRIORA_CubicSpline, cache, 1.5, 1.5,
                                      &real_val, &imag_val));
}

TEST(GdalInterpolateAtPointTest, CacheReuseAndWrapper) {
  GDALDatasetUniquePtr ds = CreateMemDataset(4, 4, 1, GDT_Float64);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  const std::vector<double> input(16, 75.0);
  WriteBandData(band, input);

  GDALDoublePointsCache cache_wrapper;
  EXPECT_EQ(nullptr, cache_wrapper.cache);

  double real_val = 0.0;
  double imag_val = 0.0;

  // First call creates the cache.
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_Bilinear, cache_wrapper.cache,
                                     1.5, 1.5, &real_val, &imag_val));
  EXPECT_DOUBLE_EQ(75.0, real_val);
  ASSERT_NE(nullptr, cache_wrapper.cache);

  DoublePointsCache* raw_cache = cache_wrapper.cache.get();

  // Second call reuses the cache.
  EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_Bilinear, cache_wrapper.cache,
                                     1.5, 1.5, &real_val, &imag_val));
  EXPECT_DOUBLE_EQ(75.0, real_val);
  EXPECT_EQ(raw_cache, cache_wrapper.cache.get());
}

TEST(GdalInterpolateAtPointTest, BlockCrossingMultipleIterations) {
  // Test multi-block extraction across the 64-pixel block boundary.
  // 1. Real band (128x128).
  {
    GDALDatasetUniquePtr ds = CreateMemDataset(128, 128, 1, GDT_Float64);
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    std::vector<double> input(128 * 128);
    for (int y = 0; y < 128; ++y) {
      for (int x = 0; x < 128; ++x) {
        input[y * 128 + x] = y * 128.0 + x;
      }
    }
    WriteBandData(band, input);

    std::unique_ptr<DoublePointsCache> cache;
    double real_val = 0.0;
    double imag_val = 0.0;

    // Sampling at (63.8, 63.8) spans blocks (0,0), (1,0), (0,1), and (1,1).
    EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_Bilinear, cache, 63.8, 63.8,
                                       &real_val, &imag_val));
    EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_CubicSpline, cache, 63.8,
                                       63.8, &real_val, &imag_val));
  }

  // 2. Complex band (128x128).
  {
    GDALDatasetUniquePtr ds = CreateMemDataset(128, 128, 1, GDT_CFloat64);
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    std::vector<std::complex<double>> input(128 * 128);
    for (int y = 0; y < 128; ++y) {
      for (int x = 0; x < 128; ++x) {
        input[y * 128 + x] = std::complex<double>(y * 128.0 + x, x * 128.0 + y);
      }
    }
    WriteBandData(band, input);

    std::unique_ptr<DoublePointsCache> cache;
    double real_val = 0.0;
    double imag_val = 0.0;

    EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_Bilinear, cache, 63.8, 63.8,
                                       &real_val, &imag_val));
    EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_CubicSpline, cache, 63.8,
                                       63.8, &real_val, &imag_val));
  }
}

TEST(GdalInterpolateAtPointTest, Float32AndCFloat32Bands) {
  // 1. GDT_Float32.
  {
    GDALDatasetUniquePtr ds = CreateMemDataset(4, 4, 1, GDT_Float32);
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    const std::vector<float> input(16, 25.0f);
    WriteBandData(band, input);

    std::unique_ptr<DoublePointsCache> cache;
    double real_val = 0.0;
    double imag_val = 0.0;

    EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_Bilinear, cache, 1.5, 1.5,
                                       &real_val, &imag_val));
    EXPECT_DOUBLE_EQ(25.0, real_val);
  }

  // 2. GDT_CFloat32.
  {
    GDALDatasetUniquePtr ds = CreateMemDataset(4, 4, 1, GDT_CFloat32);
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    const std::vector<std::complex<float>> input(
        16, std::complex<float>(30.0f, 40.0f));
    WriteBandData(band, input);

    std::unique_ptr<DoublePointsCache> cache;
    double real_val = 0.0;
    double imag_val = 0.0;

    EXPECT_TRUE(GDALInterpolateAtPoint(band, GRIORA_Bilinear, cache, 1.5, 1.5,
                                       &real_val, &imag_val));
    EXPECT_FLOAT_EQ(30.0f, static_cast<float>(real_val));
    EXPECT_FLOAT_EQ(40.0f, static_cast<float>(imag_val));
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Remaining uncovered lines are defensive
// RasterIO/block-read error guards.
