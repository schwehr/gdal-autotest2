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
// Tests GRIB raster driver.
//
// See also:
//   http://www.gdal.org/frmt_grib.html
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/gdrivers/grib.py

#include <stddef.h>
#include <memory>
#include <string>

#include "base/commandlineflags_declare.h"
#include "file/base/path.h"
#include "gmock.h"
#include "gunit.h"
#include "third_party/absl/memory/memory.h"
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
    "autotest2/cpp/frmts/grib/testdata/";

class GribTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_GRIB(); }
};

TEST_F(GribTest, Basics) {
  const string filepath = file::JoinPath(
      FLAGS_test_srcdir, string(kTestData),
      "regular_latlon_surface_constant.grib2");
  auto src = absl::WrapUnique(static_cast<GDALDataset *>(
      GDALOpenEx(filepath.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                 nullptr, nullptr)));
  ASSERT_NE(nullptr, src);
  double geo_transform[6] = {};
  src->GetGeoTransform(geo_transform);
  EXPECT_THAT(
      geo_transform,
      testing::ElementsAre(-1, 2, 0, 61, 0, -2));
  EXPECT_EQ(0, src->GetGCPCount());
  EXPECT_EQ(16, src->GetRasterXSize());
  EXPECT_EQ(31, src->GetRasterYSize());
  EXPECT_EQ(1, src->GetRasterCount());

  GDALRasterBand *band = src->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  int block_xsize = 0;
  int block_ysize = 0;
  band->GetBlockSize(&block_xsize, &block_ysize);
  EXPECT_EQ(16, block_xsize);
  EXPECT_EQ(1, block_ysize);
  EXPECT_EQ(GDT_Float64, band->GetRasterDataType());
  EXPECT_EQ(GCI_Undefined, band->GetColorInterpretation());

  double minmax[2] = {0.0, 0.0};
  ASSERT_EQ(CE_None, band->ComputeRasterMinMax(false, minmax));
  EXPECT_THAT(minmax, testing::ElementsAre(-272.15, -272.15));

  EXPECT_EQ(0, band->GetOverviewCount());
  EXPECT_EQ(nullptr, band->GetColorTable());
}

}  // namespace
}  // namespace autotest2
