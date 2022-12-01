// Copyright 2018 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Test the HDF5 Image raster driver.
//
// See also:
//   https://www.gdal.org/frmt_hdf5.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/hdf5.py

#include <memory>
#include <string>

#include "logging.h"
#include "file/base/path.h"
#include "gmock.h"
#include "googletest.h"
#include "gunit.h"
#include "third_party/absl/flags/flag.h"
#include "third_party/absl/memory/memory.h"
#include "third_party/absl/strings/substitute.h"
#include "autotest2/cpp/util/error_handler.h"
#include "gcore/gdal.h"
#include "gcore/gdal_frmts.h"
#include "gcore/gdal_priv.h"
#include "ogr/ogr_core.h"
#include "ogr/ogr_spatialref.h"
#include "port/cpl_conv.h"
#include "port/cpl_error.h"

namespace autotest2 {
namespace {

constexpr char kHdf5ImageDriver[] = "HDF5Image";
constexpr char kTestData[] =
    "/google3/third_party/gdal/autotest2/cpp/frmts/hdf5/testdata/";

class Hdf5ImageTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    CPLSetErrorHandler(CPLGoogleLogErrorHandler);
    // Explicitly avoiding GDALRegister_HDF5().
    GDALRegister_HDF5Image();
  }

  static void TearDownTestSuite() {
    auto driver_handle = GDALGetDriverByName(kHdf5ImageDriver);
    CHECK_NE(nullptr, driver_handle);
    GDALDeregisterDriver(driver_handle);
  }
};

TEST_F(Hdf5ImageTest, ReadSimpleNoHdf5Prefix) {
  const std::string filepath = file::JoinPath(
      absl::GetFlag(FLAGS_test_srcdir), std::string(kTestData), "u8be.h5");
  auto src = absl::WrapUnique(static_cast<GDALDataset *>(
      GDALOpenEx(filepath.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                 nullptr, nullptr)));
  ASSERT_EQ(nullptr, src);
}

TEST_F(Hdf5ImageTest, ReadSimple) {
  const std::string filepath = file::JoinPath(
      absl::GetFlag(FLAGS_test_srcdir), std::string(kTestData), "u8be.h5");
  // See: gdalinfo u8be.h5 -listmdd -mdd DERIVED_SUBDATASETS
  const std::string source = absl::Substitute("HDF5:$0://TestArray", filepath);
  auto src = absl::WrapUnique(static_cast<GDALDataset *>(
      GDALOpenEx(source.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                 nullptr, nullptr)));

  ASSERT_NE(nullptr, src);

  double geo_transform[6] = {};
  src->GetGeoTransform(geo_transform);
  EXPECT_THAT(geo_transform, testing::ElementsAre(0, 1, 0, 0, 0, 1));
  EXPECT_EQ(0, src->GetGCPCount());
  EXPECT_EQ(5, src->GetRasterXSize());
  EXPECT_EQ(6, src->GetRasterYSize());
  EXPECT_EQ(1, src->GetRasterCount());

  GDALRasterBand *band = src->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  int block_xsize = 0;
  int block_ysize = 0;
  band->GetBlockSize(&block_xsize, &block_ysize);
  EXPECT_EQ(5, block_xsize);
  EXPECT_EQ(1, block_ysize);
  EXPECT_EQ(GDT_Byte, band->GetRasterDataType());
  EXPECT_EQ(GCI_Undefined, band->GetColorInterpretation());

  double minmax[2] = {0.0, 0.0};
  // Force accessing the actual band data by setting approximately ok to false.
  ASSERT_EQ(CE_None, band->ComputeRasterMinMax(false /* approx_ok */, minmax));
  EXPECT_THAT(minmax, testing::ElementsAre(0.0, 9.0));

  EXPECT_EQ(0, band->GetOverviewCount());
  EXPECT_EQ(nullptr, band->GetColorTable());
}

TEST_F(Hdf5ImageTest, ReadSzip) {
  const std::string filepath =
      file::JoinPath(absl::GetFlag(FLAGS_test_srcdir), std::string(kTestData),
                     "PROBAV_S1_TOC_X11Y11_20180916_100M_V101.HDF5");
  const std::string source =
      absl::Substitute("HDF5:$0://LEVEL3/GEOMETRY/SAA", filepath);
  auto src = absl::WrapUnique(static_cast<GDALDataset *>(
      GDALOpenEx(source.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                 nullptr, nullptr)));
  ASSERT_NE(nullptr, src);

  double geo_transform[6] = {};
  src->GetGeoTransform(geo_transform);
  EXPECT_THAT(geo_transform, testing::ElementsAre(0, 1, 0, 0, 0, 1));
  EXPECT_EQ(0, src->GetGCPCount());
  EXPECT_EQ(10080, src->GetRasterXSize());
  EXPECT_EQ(10080, src->GetRasterYSize());
  EXPECT_EQ(1, src->GetRasterCount());

  GDALRasterBand *band = src->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  int block_xsize = 0;
  int block_ysize = 0;
  band->GetBlockSize(&block_xsize, &block_ysize);
  EXPECT_EQ(10080, block_xsize);
  EXPECT_EQ(1, block_ysize);
  EXPECT_EQ(GDT_Byte, band->GetRasterDataType());
  EXPECT_EQ(GCI_Undefined, band->GetColorInterpretation());

  double minmax[2] = {0.0, 255.0};
  // Force accessing the actual band data by setting approximately ok to false.
  ASSERT_EQ(CE_None, band->ComputeRasterMinMax(false /* approx_ok */, minmax));
  EXPECT_THAT(minmax, testing::ElementsAre(27.0, 255.0));

  EXPECT_EQ(0, band->GetOverviewCount());
  EXPECT_EQ(nullptr, band->GetColorTable());
}

}  // namespace
}  // namespace autotest2
