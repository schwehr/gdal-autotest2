// Copyright 2020 Google Inc. All Rights Reserved.
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
// Tests AIGRID/AIG (Arc/Info Binary Grid) raster driver.
//
// See also:
//   https://gdal.org/drivers/raster/aig.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/aigrid.py

#include <filesystem>  // NOLINT
#include <memory>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/memory/memory.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"

namespace autotest2 {
namespace {

const std::filesystem::path kTestDataPath(
    "google3/third_party/gdal/autotest2/cpp/frmts/aigrid/testdata");

std::filesystem::path GetTestFilePath(const std::string_view filename) {
  return std::filesystem::path(::testing::SrcDir()) / kTestDataPath / filename;
}

class AigridTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_AIGrid(); }
};

TEST_F(AigridTest, Basics) {
  const auto filepath = GetTestFilePath("abc3x1");
  auto src = absl::WrapUnique(static_cast<GDALDataset *>(
      GDALOpenEx(filepath.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                 nullptr, nullptr)));
  ASSERT_NE(nullptr, src);
  double geo_transform[6] = {};
  src->GetGeoTransform(geo_transform);
  EXPECT_THAT(
      geo_transform,
      testing::ElementsAre(-0.5, 1, 0, 0.5, 0, -1));
  EXPECT_EQ(0, src->GetGCPCount());
  EXPECT_EQ(3, src->GetRasterXSize());
  EXPECT_EQ(1, src->GetRasterYSize());
  EXPECT_EQ(1, src->GetRasterCount());

  GDALRasterBand *band = src->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  int block_xsize = 0;
  int block_ysize = 0;
  band->GetBlockSize(&block_xsize, &block_ysize);
  EXPECT_EQ(256, block_xsize);
  EXPECT_EQ(4, block_ysize);
  EXPECT_EQ(GDT_Byte, band->GetRasterDataType());
  EXPECT_EQ(GCI_PaletteIndex, band->GetColorInterpretation());

  double minmax[2] = {0.0, 0.0};
  ASSERT_EQ(CE_None, band->ComputeRasterMinMax(false, minmax));
  EXPECT_THAT(minmax, testing::ElementsAre(0, 2));

  EXPECT_EQ(0, band->GetOverviewCount());
  EXPECT_NE(nullptr, band->GetColorTable());

  // TODO(schwehr): Test more.
}

}  // namespace
}  // namespace autotest2
