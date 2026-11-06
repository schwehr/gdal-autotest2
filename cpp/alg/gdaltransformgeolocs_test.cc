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
// Tests GDALTransformGeolocations (alg/gdaltransformgeolocs.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/gdaltransformgeolocs.cpp

#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::DoubleEq;
using ::testing::ElementsAre;

int OffsetTransformer(void* pTransformArg, int bDstToSrc, int nPointCount,
                      double* x, double* y, double* z, int* panSuccess) {
  const double offset =
      pTransformArg ? *static_cast<double*>(pTransformArg) : 10.0;
  for (int i = 0; i < nPointCount; ++i) {
    if (bDstToSrc) {
      x[i] -= offset;
      y[i] -= offset * 2.0;
      if (z) z[i] -= offset * 3.0;
    } else {
      x[i] += offset;
      y[i] += offset * 2.0;
      if (z) z[i] += offset * 3.0;
    }
    if (panSuccess) panSuccess[i] = TRUE;
  }
  return TRUE;
}

int ProgressTracker(double dfComplete, const char* /* pszMessage */,
                    void* pProgressArg) {
  if (pProgressArg) {
    auto* calls = static_cast<std::vector<double>*>(pProgressArg);
    calls->push_back(dfComplete);
  }
  return TRUE;
}

TEST(GdalTransformGeolocsTest, NullBandsValidation) {
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GDALTransformGeolocations(nullptr, nullptr, nullptr,
                                                  OffsetTransformer, nullptr,
                                                  nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST(GdalTransformGeolocsTest, NullYBandValidation) {
  GDALRegister_MEM();
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  GDALDatasetUniquePtr ds(
      mem_driver->Create("", 4, 3, 1, GDT_Float64, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALRasterBandH hXBand = GDALRasterBand::ToHandle(ds->GetRasterBand(1));

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GDALTransformGeolocations(hXBand, nullptr, nullptr,
                                                  OffsetTransformer, nullptr,
                                                  nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST(GdalTransformGeolocsTest, SizeMismatchXYBands) {
  GDALRegister_MEM();
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  GDALDatasetUniquePtr ds1(
      mem_driver->Create("", 4, 3, 1, GDT_Float64, nullptr));
  GDALDatasetUniquePtr ds2(
      mem_driver->Create("", 5, 3, 1, GDT_Float64, nullptr));
  ASSERT_NE(nullptr, ds1);
  ASSERT_NE(nullptr, ds2);

  GDALRasterBandH hXBand = GDALRasterBand::ToHandle(ds1->GetRasterBand(1));
  GDALRasterBandH hYBand = GDALRasterBand::ToHandle(ds2->GetRasterBand(1));

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GDALTransformGeolocations(hXBand, hYBand, nullptr,
                                                  OffsetTransformer, nullptr,
                                                  nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST(GdalTransformGeolocsTest, SizeMismatchXZBands) {
  GDALRegister_MEM();
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  GDALDatasetUniquePtr ds1(
      mem_driver->Create("", 4, 3, 1, GDT_Float64, nullptr));
  GDALDatasetUniquePtr ds2(
      mem_driver->Create("", 4, 3, 1, GDT_Float64, nullptr));
  GDALDatasetUniquePtr ds3(
      mem_driver->Create("", 4, 4, 1, GDT_Float64, nullptr));
  ASSERT_NE(nullptr, ds1);
  ASSERT_NE(nullptr, ds2);
  ASSERT_NE(nullptr, ds3);

  GDALRasterBandH hXBand = GDALRasterBand::ToHandle(ds1->GetRasterBand(1));
  GDALRasterBandH hYBand = GDALRasterBand::ToHandle(ds2->GetRasterBand(1));
  GDALRasterBandH hZBand = GDALRasterBand::ToHandle(ds3->GetRasterBand(1));

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALTransformGeolocations(hXBand, hYBand, hZBand, OffsetTransformer,
                                      nullptr, nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST(GdalTransformGeolocsTest, Transform2DSuccessWithNullProgress) {
  GDALRegister_MEM();
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  constexpr int kXSize = 3;
  constexpr int kYSize = 2;
  GDALDatasetUniquePtr ds(
      mem_driver->Create("", kXSize, kYSize, 2, GDT_Float64, nullptr));
  ASSERT_NE(nullptr, ds);

  GDALRasterBand* band_x = ds->GetRasterBand(1);
  GDALRasterBand* band_y = ds->GetRasterBand(2);

  std::vector<double> x_init = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
  std::vector<double> y_init = {10.0, 20.0, 30.0, 40.0, 50.0, 60.0};
  ASSERT_EQ(CE_None,
            band_x->RasterIO(GF_Write, 0, 0, kXSize, kYSize, x_init.data(),
                             kXSize, kYSize, GDT_Float64, 0, 0, nullptr));
  ASSERT_EQ(CE_None,
            band_y->RasterIO(GF_Write, 0, 0, kXSize, kYSize, y_init.data(),
                             kXSize, kYSize, GDT_Float64, 0, 0, nullptr));

  double offset = 5.0;
  EXPECT_EQ(CE_None,
            GDALTransformGeolocations(GDALRasterBand::ToHandle(band_x),
                                      GDALRasterBand::ToHandle(band_y), nullptr,
                                      OffsetTransformer, &offset, nullptr,
                                      nullptr, nullptr));

  std::vector<double> x_out(kXSize * kYSize, 0.0);
  std::vector<double> y_out(kXSize * kYSize, 0.0);
  ASSERT_EQ(CE_None,
            band_x->RasterIO(GF_Read, 0, 0, kXSize, kYSize, x_out.data(),
                             kXSize, kYSize, GDT_Float64, 0, 0, nullptr));
  ASSERT_EQ(CE_None,
            band_y->RasterIO(GF_Read, 0, 0, kXSize, kYSize, y_out.data(),
                             kXSize, kYSize, GDT_Float64, 0, 0, nullptr));

  EXPECT_THAT(x_out, ElementsAre(6.0, 7.0, 8.0, 9.0, 10.0, 11.0));
  EXPECT_THAT(y_out, ElementsAre(20.0, 30.0, 40.0, 50.0, 60.0, 70.0));
}

TEST(GdalTransformGeolocsTest, Transform3DSuccessWithProgress) {
  GDALRegister_MEM();
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  constexpr int kXSize = 2;
  constexpr int kYSize = 2;
  GDALDatasetUniquePtr ds(
      mem_driver->Create("", kXSize, kYSize, 3, GDT_Float64, nullptr));
  ASSERT_NE(nullptr, ds);

  GDALRasterBand* band_x = ds->GetRasterBand(1);
  GDALRasterBand* band_y = ds->GetRasterBand(2);
  GDALRasterBand* band_z = ds->GetRasterBand(3);

  std::vector<double> x_init = {1.0, 2.0, 3.0, 4.0};
  std::vector<double> y_init = {10.0, 20.0, 30.0, 40.0};
  std::vector<double> z_init = {100.0, 200.0, 300.0, 400.0};
  ASSERT_EQ(CE_None,
            band_x->RasterIO(GF_Write, 0, 0, kXSize, kYSize, x_init.data(),
                             kXSize, kYSize, GDT_Float64, 0, 0, nullptr));
  ASSERT_EQ(CE_None,
            band_y->RasterIO(GF_Write, 0, 0, kXSize, kYSize, y_init.data(),
                             kXSize, kYSize, GDT_Float64, 0, 0, nullptr));
  ASSERT_EQ(CE_None,
            band_z->RasterIO(GF_Write, 0, 0, kXSize, kYSize, z_init.data(),
                             kXSize, kYSize, GDT_Float64, 0, 0, nullptr));

  std::vector<double> progress_reports;
  double offset = 1.0;
  EXPECT_EQ(CE_None, GDALTransformGeolocations(
                         GDALRasterBand::ToHandle(band_x),
                         GDALRasterBand::ToHandle(band_y),
                         GDALRasterBand::ToHandle(band_z), OffsetTransformer,
                         &offset, ProgressTracker, &progress_reports, nullptr));

  EXPECT_FALSE(progress_reports.empty());
  EXPECT_DOUBLE_EQ(0.0, progress_reports.front());
  EXPECT_DOUBLE_EQ(1.0, progress_reports.back());

  std::vector<double> x_out(4, 0.0);
  std::vector<double> y_out(4, 0.0);
  std::vector<double> z_out(4, 0.0);
  ASSERT_EQ(CE_None,
            band_x->RasterIO(GF_Read, 0, 0, kXSize, kYSize, x_out.data(),
                             kXSize, kYSize, GDT_Float64, 0, 0, nullptr));
  ASSERT_EQ(CE_None,
            band_y->RasterIO(GF_Read, 0, 0, kXSize, kYSize, y_out.data(),
                             kXSize, kYSize, GDT_Float64, 0, 0, nullptr));
  ASSERT_EQ(CE_None,
            band_z->RasterIO(GF_Read, 0, 0, kXSize, kYSize, z_out.data(),
                             kXSize, kYSize, GDT_Float64, 0, 0, nullptr));

  EXPECT_THAT(x_out, ElementsAre(2.0, 3.0, 4.0, 5.0));
  EXPECT_THAT(y_out, ElementsAre(12.0, 22.0, 32.0, 42.0));
  EXPECT_THAT(z_out, ElementsAre(103.0, 203.0, 303.0, 403.0));
}

}  // namespace
}  // namespace autotest2
