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
// Tests Line Of Sight algorithm (alg/los.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/los.cpp

#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"

namespace autotest2 {
namespace {

TEST(LosTest, NullBandValidation) {
  WithQuietHandler quiet;
  CPLErrorReset();
  int x_int = -1;
  int y_int = -1;
  EXPECT_FALSE(GDALIsLineOfSightVisible(nullptr, 0, 0, 10.0, 1, 1, 10.0, &x_int,
                                        &y_int, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST(LosTest, SameStartAndEndPoint) {
  GDALRegister_MEM();
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  GDALDatasetUniquePtr ds(
      mem_driver->Create("", 3, 3, 1, GDT_Float64, nullptr));
  ASSERT_NE(nullptr, ds);

  GDALRasterBand* band = ds->GetRasterBand(1);
  std::vector<double> dem(9, 5.0);
  ASSERT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 3, 3, dem.data(), 3, 3,
                                    GDT_Float64, 0, 0, nullptr));

  GDALRasterBandH hBand = GDALRasterBand::ToHandle(band);
  int x_int = -999;
  int y_int = -999;
  // Above terrain:
  EXPECT_TRUE(GDALIsLineOfSightVisible(hBand, 1, 1, 10.0, 1, 1, 10.0, &x_int,
                                       &y_int, nullptr));
  // Below terrain at start:
  EXPECT_FALSE(GDALIsLineOfSightVisible(hBand, 1, 1, 4.0, 1, 1, 10.0, &x_int,
                                        &y_int, nullptr));
  EXPECT_EQ(1, x_int);
  EXPECT_EQ(1, y_int);

  // Below terrain at end:
  EXPECT_FALSE(GDALIsLineOfSightVisible(hBand, 1, 1, 10.0, 1, 1, 4.0, &x_int,
                                        &y_int, nullptr));
  EXPECT_EQ(1, x_int);
  EXPECT_EQ(1, y_int);
}

TEST(LosTest, VerticalLineSight) {
  GDALRegister_MEM();
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  // 1x5 grid
  GDALDatasetUniquePtr ds(
      mem_driver->Create("", 1, 5, 1, GDT_Float64, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);

  // Elevation: 0, 10 (peak), 0, 0, 0
  std::vector<double> dem = {0.0, 10.0, 0.0, 0.0, 0.0};
  ASSERT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 1, 5, dem.data(), 1, 5,
                                    GDT_Float64, 0, 0, nullptr));

  GDALRasterBandH hBand = GDALRasterBand::ToHandle(band);
  int x_int = -1;
  int y_int = -1;

  // yA < yB, blocked by peak at y=1 (ray from z=5 to z=5)
  EXPECT_FALSE(GDALIsLineOfSightVisible(hBand, 0, 0, 5.0, 0, 4, 5.0, &x_int,
                                        &y_int, nullptr));
  EXPECT_EQ(0, x_int);
  EXPECT_EQ(1, y_int);

  // yA < yB, clear above peak (ray from z=15 to z=15)
  EXPECT_TRUE(GDALIsLineOfSightVisible(hBand, 0, 0, 15.0, 0, 4, 15.0, &x_int,
                                       &y_int, nullptr));

  // yA > yB, blocked by peak at y=1 (ray from y=4 to y=0)
  EXPECT_FALSE(GDALIsLineOfSightVisible(hBand, 0, 4, 5.0, 0, 0, 5.0, &x_int,
                                        &y_int, nullptr));
  EXPECT_EQ(0, x_int);
  EXPECT_EQ(1, y_int);

  // yA > yB, clear above peak
  EXPECT_TRUE(GDALIsLineOfSightVisible(hBand, 0, 4, 15.0, 0, 0, 15.0, nullptr,
                                       nullptr, nullptr));
}

TEST(LosTest, HorizontalLineSight) {
  GDALRegister_MEM();
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  // 5x1 grid
  GDALDatasetUniquePtr ds(
      mem_driver->Create("", 5, 1, 1, GDT_Float64, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);

  // Elevation: 0, 0, 20 (mountain at x=2), 0, 0
  std::vector<double> dem = {0.0, 0.0, 20.0, 0.0, 0.0};
  ASSERT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 5, 1, dem.data(), 5, 1,
                                    GDT_Float64, 0, 0, nullptr));

  GDALRasterBandH hBand = GDALRasterBand::ToHandle(band);
  int x_int = -1;
  int y_int = -1;

  // xA < xB, blocked by mountain at x=2
  EXPECT_FALSE(GDALIsLineOfSightVisible(hBand, 0, 0, 10.0, 4, 0, 10.0, &x_int,
                                        &y_int, nullptr));
  EXPECT_EQ(2, x_int);
  EXPECT_EQ(0, y_int);

  // xA < xB, clear above mountain
  EXPECT_TRUE(GDALIsLineOfSightVisible(hBand, 0, 0, 25.0, 4, 0, 25.0, &x_int,
                                       &y_int, nullptr));

  // xA > xB, blocked
  EXPECT_FALSE(GDALIsLineOfSightVisible(hBand, 4, 0, 10.0, 0, 0, 10.0, &x_int,
                                        &y_int, nullptr));
  EXPECT_EQ(2, x_int);
  EXPECT_EQ(0, y_int);

  // xA > xB, clear
  EXPECT_TRUE(GDALIsLineOfSightVisible(hBand, 4, 0, 25.0, 0, 0, 25.0, nullptr,
                                       nullptr, nullptr));
}

TEST(LosTest, BresenhamDiagonalSightAllOctants) {
  GDALRegister_MEM();
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  // 10x10 grid with a peak at (5, 5) elevation 50.0, rest 0.0
  GDALDatasetUniquePtr ds(
      mem_driver->Create("", 10, 10, 1, GDT_Float64, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);

  std::vector<double> dem(100, 0.0);
  dem[5 * 10 + 5] = 50.0;
  ASSERT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 10, 10, dem.data(), 10, 10,
                                    GDT_Float64, 0, 0, nullptr));

  GDALRasterBandH hBand = GDALRasterBand::ToHandle(band);
  int x_int = -1;
  int y_int = -1;

  // 1. dx >= dy, x2 >= x1, y2 >= y1: from (1, 3) to (9, 7) passing near/through
  // (5, 5) Blocked at low z
  EXPECT_FALSE(GDALIsLineOfSightVisible(hBand, 1, 3, 10.0, 9, 7, 10.0, &x_int,
                                        &y_int, nullptr));
  EXPECT_EQ(5, x_int);
  EXPECT_EQ(5, y_int);
  // Clear at high z
  EXPECT_TRUE(GDALIsLineOfSightVisible(hBand, 1, 3, 60.0, 9, 7, 60.0, &x_int,
                                       &y_int, nullptr));

  // 2. dx >= dy, x2 < x1, y2 < y1: reverse from (9, 7) to (1, 3)
  EXPECT_FALSE(GDALIsLineOfSightVisible(hBand, 9, 7, 10.0, 1, 3, 10.0, &x_int,
                                        &y_int, nullptr));
  EXPECT_EQ(5, x_int);
  EXPECT_EQ(5, y_int);
  EXPECT_TRUE(GDALIsLineOfSightVisible(hBand, 9, 7, 60.0, 1, 3, 60.0, &x_int,
                                       &y_int, nullptr));

  // 3. dx < dy, y2 >= y1, x2 < x1: steep line from (7, 1) to (3, 9)
  EXPECT_FALSE(GDALIsLineOfSightVisible(hBand, 7, 1, 10.0, 3, 9, 10.0, &x_int,
                                        &y_int, nullptr));
  EXPECT_EQ(5, x_int);
  EXPECT_EQ(5, y_int);
  EXPECT_TRUE(GDALIsLineOfSightVisible(hBand, 7, 1, 60.0, 3, 9, 60.0, &x_int,
                                       &y_int, nullptr));

  // 4. dx < dy, y2 < y1, x2 >= x1: steep line from (3, 9) to (7, 1)
  EXPECT_FALSE(GDALIsLineOfSightVisible(hBand, 3, 9, 10.0, 7, 1, 10.0, &x_int,
                                        &y_int, nullptr));
  EXPECT_EQ(5, x_int);
  EXPECT_EQ(5, y_int);
  EXPECT_TRUE(GDALIsLineOfSightVisible(hBand, 3, 9, 60.0, 7, 1, 60.0, &x_int,
                                       &y_int, nullptr));
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Remaining 1 uncovered line in los.cpp is an unreachable
// defensive guard.
