// Copyright 2017 Google Inc. All Rights Reserved.
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
// Tests Kakadu JPEG2000 raster driver.
//
// See also:
//   http://www.gdal.org/frmt_jp2kak.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/jpipkak.py

// TODO(schwehr): Try these:
//   CPLSetConfigOption("JP2KAK_THREADS", "0");
//   CPLSetConfigOption("JP2KAK_THREADS", "1");
//   CPLSetConfigOption("JP2KAK_THREADS", "2");
//   CPLSetConfigOption("JP2KAK_THREADS", "-1");

#include <stddef.h>

#include <memory>
#include <string>

#include "commandlineflags_declare.h"
#include "file/base/path.h"
#include "gmock.h"
#include "googletest.h"
#include "gunit.h"
#include "third_party/absl/flags/flag.h"
#include "autotest2/cpp/util/matchers.h"
#include "gcore/gdal.h"
#include "gcore/gdal_frmts.h"
#include "gcore/gdal_priv.h"
#include "ogr/ogr_core.h"
#include "ogr/ogr_spatialref.h"
#include "port/cpl_conv.h"
#include "port/cpl_error.h"

namespace autotest2 {
namespace {

const char kTestData[] =
    "/google3/third_party/gdal/autotest2/cpp/frmts/jp2kak/testdata/";

class Jp2kakTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_JP2KAK(); }
};

TEST_F(Jp2kakTest, Srs) {
  const std::string filepath = absl::GetFlag(FLAGS_test_srcdir) +
                               std::string(kTestData) + "byte_point.jp2";
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
      << "See http://spatialreference.org/ref/epsg/" << epsg;
}

TEST_F(Jp2kakTest, Basics) {
  const std::string filepath =
      file::JoinPath(absl::GetFlag(FLAGS_test_srcdir), std::string(kTestData),
                     "byte_point.jp2");
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

}  // namespace
}  // namespace autotest2
