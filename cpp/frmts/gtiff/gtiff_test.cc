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
// Tests geotiff raster driver.

#include <stddef.h>
#include <memory>
#include <string>
#include <vector>

#include "logging.h"
#include "gmock.h"
#include "gunit.h"
#include "third_party/absl/memory/memory.h"
#include "autotest2/cpp/util/error_handler.h"
#include "autotest2/cpp/util/vsimem.h"
#include "gcore/gdal.h"
#include "gcore/gdal_frmts.h"
#include "gcore/gdal_priv.h"
#include "port/cpl_conv.h"
#include "port/cpl_error.h"
#include "port/cpl_port.h"

namespace autotest2 {
namespace {

constexpr char kDriverName[] = "GTiff";

class GTiffTest : public ::testing::Test {
 public:
  std::unique_ptr<GDALDataset> OpenReadOnly(const string& filename) {
    auto open_info = gtl::MakeUnique<GDALOpenInfo>(filename.c_str(),
                                                   GDAL_OF_READONLY, nullptr);
    auto dataset = absl::WrapUnique(driver_->pfnOpen(open_info.get()));
    return dataset;
  }

 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    driver_ = GetGDALDriverManager()->GetDriverByName(kDriverName);
    CHECK_NOTNULL(driver_);
  }

  GDALDriver* driver_;
};

TEST_F(GTiffTest, ReadSimplest) {
  constexpr char kFilename[] = "/vsimem/read_simplest.tif";

  // 1x1 without any extra metadata.
  const char data[] =
      "\x49\x49\x2a\x00\x08\x00\x00\x00\x0b\x00\x00\x01\x03\x00\x01\x00\x00\x00"
      "\x01\x00\x00\x00\x01\x01\x03\x00\x01\x00\x00\x00\x01\x00\x00\x00\x02\x01"
      "\x03\x00\x01\x00\x00\x00\x08\x00\x00\x00\x03\x01\x03\x00\x01\x00\x00\x00"
      "\x01\x00\x00\x00\x06\x01\x03\x00\x01\x00\x00\x00\x01\x00\x00\x00\x11\x01"
      "\x04\x00\x01\x00\x00\x00\x92\x00\x00\x00\x15\x01\x03\x00\x01\x00\x00\x00"
      "\x01\x00\x00\x00\x16\x01\x03\x00\x01\x00\x00\x00\x01\x00\x00\x00\x17\x01"
      "\x04\x00\x01\x00\x00\x00\x01\x00\x00\x00\x1c\x01\x03\x00\x01\x00\x00\x00"
      "\x01\x00\x00\x00\x53\x01\x03\x00\x01\x00\x00\x00\x01\x00\x00\x00\x00\x00"
      "\x00\x00\x00";
  const string data2(reinterpret_cast<const char*>(data), CPL_ARRAYSIZE(data));
  autotest2::VsiMemTempWrapper wrapper(kFilename, data2);

  auto dataset = OpenReadOnly(kFilename);
  EXPECT_STREQ("", CPLGetLastErrorMsg());
  EXPECT_EQ(CPLE_None, CPLGetLastErrorType());
  ASSERT_NE(nullptr, dataset);

  int width = dataset->GetRasterXSize();
  int height = dataset->GetRasterYSize();
  size_t size = width * height;

  EXPECT_EQ(1, width);
  EXPECT_EQ(1, height);
  ASSERT_EQ(1, dataset->GetRasterCount());

  auto band = dataset->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  // Set buf to 1 as the tif contains zero.
  std::vector<unsigned char> buf(size, 1);
  EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, width, height, &buf[0], 1, 1,
                                    GDT_Byte, 1, 1, nullptr));
  EXPECT_EQ(0, buf[0]);
}

// Write with the fewest possible features.
TEST_F(GTiffTest, CreateSimplest) {
  constexpr char kFilename[] = "/vsimem/create_simplest.tif";
  int width = 1;
  int height = 2;
  size_t size = width * height;
  int num_bands = 1;
  GDALDataType data_type = GDT_Byte;
  {
    char** options = nullptr;
    auto dataset = gtl::WrapUnique<GDALDataset>(driver_->Create(
        kFilename, width, height, num_bands, data_type, options));
    ASSERT_NE(nullptr, dataset);

    std::vector<unsigned char> buf(size, 0);

    auto band = dataset->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    ASSERT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, width, height, &buf[0],
                                      width, height, data_type, 0, 0, nullptr));
  }

  auto dataset = OpenReadOnly(kFilename);

  EXPECT_STREQ("", CPLGetLastErrorMsg());
  EXPECT_EQ(CPLE_None, CPLGetLastErrorType());
  ASSERT_NE(nullptr, dataset);

  EXPECT_EQ(1, dataset->GetRasterCount());
  EXPECT_EQ(1, dataset->GetRasterXSize());
  EXPECT_EQ(2, dataset->GetRasterYSize());

  auto band = dataset->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  // Set buf to 1 as the tif contains zeros.
  std::vector<unsigned char> buf(size, 1);
  EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, width, height, &buf[0], 2, 1,
                                    GDT_Byte, 1, 1, nullptr));
  EXPECT_EQ(0, buf[0]);
  EXPECT_EQ(0, buf[1]);
}

TEST(GTiffBad, TooManyBlocks) {
  // Image with more than 2G blocks in a single band.
  // Based on tiff_read_toomanyblocks().
  WithQuietHandler error_handler;
  constexpr char kFilename[] = "/vsimem/too_many_blocks.tif";
  const char data[] =
      "\x49\x49\x2a\x00\x08\x00\x00\x00\x0c\x00\x00\x01\x03\x00\x01\x00\x00\x00"
      "\x60\xea\x00\x00\x01\x01\x03\x00\x01\x00\x00\x00\x60\xea\x00\x00\x02\x01"
      "\x03\x00\x01\x00\x00\x00\x08\x00\x00\x00\x03\x01\x03\x00\x01\x00\x00\x00"
      "\x01\x00\x00\x00\x06\x01\x03\x00\x01\x00\x00\x00\x01\x00\x00\x00\x15\x01"
      "\x03\x00\x01\x00\x00\x00\x01\x00\x00\x00\x1c\x01\x03\x00\x01\x00\x00\x00"
      "\x01\x00\x00\x00\x42\x01\x03\x00\x01\x00\x00\x00\x01\x00\x00\x00\x43\x01"
      "\x03\x00\x01\x00\x00\x00\x01\x00\x00\x00\x44\x01\x04\x00\x01\x00\x00\x00"
      "\x00\x00\x00\x00\x45\x01\x04\x00\x01\x00\x00\x00\x00\x00\x00\x00\x53\x01"
      "\x03\x00\x01\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00";
  const string data2(reinterpret_cast<const char*>(data), CPL_ARRAYSIZE(data));
  autotest2::VsiMemTempWrapper wrapper(kFilename, data2);
  auto open_info = gtl::MakeUnique<GDALOpenInfo>(kFilename,
                                                 GDAL_OF_READONLY, nullptr);

  GDALDriver *driver_ = GetGDALDriverManager()->GetDriverByName(kDriverName);

  auto dataset = absl::WrapUnique(driver_->pfnOpen(open_info.get()));
  EXPECT_EQ(nullptr, dataset);
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("TileOffsets"));
}

TEST(GTiffBad, TooManyBlocksSeparate) {
  // Image with more than 2G blocks for all bands.
  // Based on tiff_read_toomanyblocks_separate().
  WithQuietHandler error_handler;
  constexpr char kFilename[] = "/vsimem/too_many_blocks_separate.tif";
  const char data[] =
      "\x49\x49\x2a\x00\x08\x00\x00\x00\x0c\x00\x00\x01\x03\x00\x01\x00\x00"
      "\x00\xff\x7f\x00\x00\x01\x01\x03\x00\x01\x00\x00\x00\xff\xff\x00\x00"
      "\x02\x01\x03\x00\x01\x00\x00\x00\x08\x00\x00\x00\x03\x01\x03\x00\x01"
      "\x00\x00\x00\x01\x00\x00\x00\x06\x01\x03\x00\x01\x00\x00\x00\x01\x00"
      "\x00\x00\x15\x01\x03\x00\x01\x00\x00\x00\x02\x00\x00\x00\x1c\x01\x03"
      "\x00\x01\x00\x00\x00\x02\x00\x00\x00\x42\x01\x03\x00\x01\x00\x00\x00"
      "\x01\x00\x00\x00\x43\x01\x03\x00\x01\x00\x00\x00\x01\x00\x00\x00\x44"
      "\x01\x04\x00\x01\x00\x00\x00\x9e\x00\x00\x00\x45\x01\x04\x00\x01\x00"
      "\x00\x00\x00\x01\x00\x00\x53\x01\x03\x00\x01\x00\x00\x00\x01\x00\x00"
      "\x00\x00\x00\x00\x00\x73\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00";
  const string data2(reinterpret_cast<const char*>(data), CPL_ARRAYSIZE(data));
  autotest2::VsiMemTempWrapper wrapper(kFilename, data2);
  auto open_info = gtl::MakeUnique<GDALOpenInfo>(kFilename,
                                                 GDAL_OF_READONLY, nullptr);

  GDALDriver *driver_ = GetGDALDriverManager()->GetDriverByName(kDriverName);

  auto dataset = absl::WrapUnique(driver_->pfnOpen(open_info.get()));
  EXPECT_EQ(nullptr, dataset);
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("TileOffsets"));
}

}  // namespace
}  // namespace autotest2
