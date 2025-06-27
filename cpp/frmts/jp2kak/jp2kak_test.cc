// Copyright 2017 Google Inc. All Rights Reserved.
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
// Tests Kakadu JPEG2000 raster driver.
//
// See also:
//   https://gdal.org/drivers/raster/jp2kak.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/jpipkak.py

// TODO(schwehr): Try these:
//   CPLSetConfigOption("JP2KAK_THREADS", "0");
//   CPLSetConfigOption("JP2KAK_THREADS", "1");
//   CPLSetConfigOption("JP2KAK_THREADS", "2");
//   CPLSetConfigOption("JP2KAK_THREADS", "-1");

#include <stddef.h>

#include <filesystem>  // NOLINT
#include <memory>
#include <string>
#include <string_view>

#include "gmock.h"
#include "gunit.h"
#include "autotest2/cpp/util/error_handler.h"
#include "autotest2/cpp/util/matchers.h"
#include "gcore/gdal.h"
#include "gcore/gdal_frmts.h"
#include "gcore/gdal_priv.h"
#include "ogr/ogr_core.h"
#include "ogr/ogr_spatialref.h"
#include "port/cpl_error.h"
#include "port/cpl_port.h"
#include "port/cpl_vsi.h"

namespace autotest2 {
namespace {

const std::filesystem::path kTestDataPath(
    "google3/third_party/gdal/autotest2/cpp/frmts/jp2kak/testdata/");

std::filesystem::path GetTestFilePath(std::string_view filename) {
  return std::filesystem::path(::testing::SrcDir()) / kTestDataPath / filename;
}

class Jp2kakTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_JP2KAK();
    GDALRegister_MEM();
  }
};

TEST_F(Jp2kakTest, Srs) {
  const auto filepath = GetTestFilePath("byte_point.jp2");
  std::unique_ptr<GDALDataset> src(static_cast<GDALDataset *>(
      GDALOpenEx(filepath.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                 nullptr, nullptr)));
  EXPECT_STREQ("", CPLGetLastErrorMsg());
  EXPECT_EQ(CPLE_None, CPLGetLastErrorType());
  ASSERT_NE(nullptr, src);

  const std::string projection(src->GetProjectionRef());

  OGRSpatialReference src_srs(projection.c_str());
  OGRSpatialReference expected_srs;
  const int epsg = 32611;
  ASSERT_EQ(OGRERR_NONE, expected_srs.importFromEPSG(epsg))
      << "Failed to load epsg: " << epsg;
  EXPECT_THAT(src_srs, IsSameAs(expected_srs))
      << "Failed for EPSG: " << epsg << "\n"
      << "See https://spatialreference.org/ref/epsg/" << epsg << "/";
}

TEST_F(Jp2kakTest, Basics) {
  const auto filepath = GetTestFilePath("byte_point.jp2");
  std::unique_ptr<GDALDataset> src(static_cast<GDALDataset *>(
      GDALOpenEx(filepath.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                 nullptr, nullptr)));
  ASSERT_NE(nullptr, src);
  double geo_transform[6] = {};
  src->GetGeoTransform(geo_transform);
  EXPECT_THAT(
      geo_transform,
      testing::ElementsAre(440690.0, 60.0, 0.0, 3.75135e+06, 0.0, -60.0));
  EXPECT_EQ(0, src->GetGCPCount());
  EXPECT_EQ(20, src->GetRasterXSize());
  EXPECT_EQ(20, src->GetRasterYSize());
  EXPECT_EQ(1, src->GetRasterCount());

  GDALRasterBand *band = src->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  int block_xsize = 0;
  int block_ysize = 0;
  band->GetBlockSize(&block_xsize, &block_ysize);
  EXPECT_EQ(20, block_xsize);
  EXPECT_EQ(20, block_ysize);
  EXPECT_EQ(GDT_Byte, band->GetRasterDataType());
  EXPECT_EQ(GCI_GrayIndex, band->GetColorInterpretation());

  double minmax[2] = {0.0, 0.0};
  ASSERT_EQ(CE_None, band->ComputeRasterMinMax(false, minmax));
  EXPECT_THAT(minmax, testing::ElementsAre(75.0, 255.0));

  EXPECT_EQ(0, band->GetOverviewCount());
  EXPECT_EQ(nullptr, band->GetColorTable());
}

// Verify that creating a new jp2 file is not supported by this driver.
TEST(Jp2kakdatasetTest, CreateFails) {
  const char kFilename[] = "/vsimem/create_fails.jp2";
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("JP2KAK");
  ASSERT_NE(driver, nullptr);

  GDALDataset* dataset = nullptr;
  {
    WithQuietHandler error_handler;
    dataset = driver->Create(kFilename, 3, 2, 1, GDT_Byte, nullptr);
  }
  EXPECT_EQ(dataset, nullptr);
  EXPECT_EQ(CPLGetLastErrorType(), CE_Failure);
  EXPECT_STREQ(
      "GDALDriver::Create() ... no create method implemented for this format.",
      CPLGetLastErrorMsg());

  VSIStatBufL stat_buf{};
  EXPECT_EQ(VSIStatL(kFilename, &stat_buf), -1);
}

TEST_F(Jp2kakTest, CreateCopy) {
  const char kOutputFilename[] = "/vsimem/output.jp2";

  // Create a small in-memory dataset.
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(mem_driver, nullptr);
  GDALDataset* mem_dataset = mem_driver->Create("", 3, 2, 1, GDT_Byte, nullptr);
  ASSERT_NE(mem_dataset, nullptr);

  GDALRasterBand* band = mem_dataset->GetRasterBand(1);
  ASSERT_NE(band, nullptr);

  const unsigned char data[] = {4, 5, 6, 7, 8, 9};
  ASSERT_EQ(
      band->RasterIO(GF_Write, 0, 0, 3, 2, const_cast<unsigned char*>(data), 3,
                     2, GDT_Byte, 0, 0),
      CE_None);

  // Create a JP2K dataset using CreateCopy.
  GDALDriver* jp2kak_driver = GetGDALDriverManager()->GetDriverByName("JP2KAK");
  ASSERT_NE(jp2kak_driver, nullptr);
  GDALDataset* jp2kak_dataset = jp2kak_driver->CreateCopy(
      kOutputFilename, mem_dataset, /*bStrict=*/FALSE, /*papszOptions=*/nullptr,
      /*pfnProgress=*/nullptr, /*pProgressData=*/nullptr);
  ASSERT_NE(jp2kak_dataset, nullptr);

  // Clean up
  GDALClose(mem_dataset);
  GDALClose(jp2kak_dataset);
  VSIUnlink(kOutputFilename);
}

}  // namespace
}  // namespace autotest2
