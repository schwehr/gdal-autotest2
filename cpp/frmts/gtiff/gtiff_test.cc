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
// Tests geotiff raster driver.

#include <stddef.h>

#include <filesystem>  // NOLINT
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/logging.h"
#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/memory/memory.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_openinfo.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

const std::filesystem::path kTestDataPath(
    "google3/third_party/gdal/autotest2/cpp/frmts/gtiff/testdata");

std::filesystem::path GetTestFilePath(const std::string_view filename) {
  return std::filesystem::path(::testing::SrcDir()) / kTestDataPath / filename;
}

constexpr char kDriverName[] = "GTiff";

class GTiffTest : public ::testing::Test {
 public:
  std::unique_ptr<GDALDataset> OpenReadOnly(const std::string& filename) {
    auto open_info = std::make_unique<GDALOpenInfo>(filename.c_str(),
                                                    GDAL_OF_READONLY, nullptr);
    auto dataset = absl::WrapUnique(driver_->pfnOpen(open_info.get()));
    return dataset;
  }

 protected:
  void SetUp() override {
    CPLErrorReset();

    GDALRegister_GTiff();
    driver_ = GetGDALDriverManager()->GetDriverByName(kDriverName);
    CHECK(driver_ != nullptr);
  }

  void TearDown() override { CPLErrorReset(); }

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
  const std::string data2(reinterpret_cast<const char*>(data),
                          CPL_ARRAYSIZE(data));
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
    auto dataset = absl::WrapUnique<GDALDataset>(driver_->Create(
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

TEST_F(GTiffTest, CreateCopyPreservesMaskForBandInterleavedCog) {
  constexpr char kFilename[] = "/vsimem/create_copy_with_mask.tif";
  constexpr int kWidth = 2;
  constexpr int kHeight = 2;
  constexpr size_t kSize = kWidth * kHeight;
  constexpr int kNumBands = 1;

  {
    char** options = nullptr;
    options = CSLSetNameValue(options, "COPY_SRC_OVERVIEWS", "YES");
    options = CSLSetNameValue(options, "INTERLEAVE", "BAND");
    auto dataset = absl::WrapUnique<GDALDataset>(driver_->Create(
        kFilename, kWidth, kHeight, kNumBands, GDT_Byte, options));
    ASSERT_TRUE(dataset != nullptr);

    std::vector<unsigned char> raster_buf(kSize, 0);
    ASSERT_EQ(dataset->RasterIO(GF_Write, 0, 0, kWidth, kHeight, &raster_buf[0],
                                kWidth, kHeight, GDT_Byte, kNumBands, nullptr,
                                0, 0, 0),
              CE_None);
    {
      const CPLConfigOptionSetter internalMaskSetter("GDAL_TIFF_INTERNAL_MASK",
                                                     "YES", false);

      ASSERT_EQ(dataset->CreateMaskBand(GMF_PER_DATASET), CE_None);
      auto mask_band = dataset->GetRasterBand(1)->GetMaskBand();
      ASSERT_TRUE(mask_band != nullptr);
      std::vector<unsigned char> mask_buf = {0, 255, 255, 255};
      ASSERT_EQ(
          mask_band->RasterIO(GF_Write, 0, 0, kWidth, kHeight, &mask_buf[0],
                              kWidth, kHeight, GDT_Byte, 0, 0),
          CE_None);
    }

    auto copy_dataset = absl::WrapUnique<GDALDataset>(driver_->CreateCopy(
        kFilename, dataset.get(), true, options, nullptr, nullptr));
    ASSERT_TRUE(copy_dataset != nullptr);

    CSLDestroy(options);
  }

  auto dataset = OpenReadOnly(kFilename);

  ASSERT_EQ(dataset->GetRasterCount(), kNumBands);
  ASSERT_EQ(dataset->GetRasterXSize(), kWidth);
  ASSERT_EQ(dataset->GetRasterYSize(), kHeight);

  auto band = dataset->GetRasterBand(1);
  ASSERT_NE(band->GetMaskFlags() & GMF_PER_DATASET, 0);
  auto mask_band = band->GetMaskBand();
  ASSERT_TRUE(mask_band != nullptr);
  std::vector<unsigned char> mask_buf(kSize, 0);
  ASSERT_EQ(mask_band->RasterIO(GF_Read, 0, 0, kWidth, kHeight, &mask_buf[0],
                                kWidth, kHeight, GDT_Byte, 0, 0),
            CE_None);
  EXPECT_EQ(mask_buf[0], 0);
  EXPECT_EQ(mask_buf[1], 255);
  EXPECT_EQ(mask_buf[2], 255);
  EXPECT_EQ(mask_buf[3], 255);

  VSIUnlink(kFilename);
}

TEST_F(GTiffTest, Overviews) {
  const auto filepath = GetTestFilePath("with-overviews.tif");
  const auto dataset = OpenReadOnly(filepath);
  ASSERT_NE(nullptr, dataset);

  EXPECT_EQ(1, dataset->GetRasterCount());

  const auto band = dataset->GetRasterBand(1);
  EXPECT_EQ(3, band->GetOverviewCount());

  // Note that bands are indexed starting at 1, but overviews start at 0.
  const auto overview0 = band->GetOverview(0);
  ASSERT_NE(nullptr, overview0);
  EXPECT_EQ(250, overview0->GetXSize());
  EXPECT_EQ(125, overview0->GetYSize());
  const auto overview2 = band->GetOverview(2);
  ASSERT_NE(nullptr, overview2);
  EXPECT_EQ(63, overview2->GetXSize());
  EXPECT_EQ(32, overview2->GetYSize());
  EXPECT_EQ(nullptr, band->GetOverview(3));
}

TEST_F(GTiffTest, OverviewFile) {
  // Tiff file plus a separate overview sidecar.
  const auto filepath = GetTestFilePath("with-overview-file.tif");
  const auto dataset = OpenReadOnly(filepath);
  ASSERT_NE(nullptr, dataset);

  char** filelist_tmp = dataset->GetFileList();
  auto filelist = autotest2::CslToVector(filelist_tmp);
  CSLDestroy(filelist_tmp);
  EXPECT_THAT(filelist, testing::ElementsAre(
                            testing::EndsWith("with-overview-file.tif"),
                            testing::EndsWith("with-overview-file.tif.ovr")));

  EXPECT_EQ(1, dataset->GetRasterCount());

  const auto band = dataset->GetRasterBand(1);
  EXPECT_EQ(1, band->GetOverviewCount());

  const auto overview0 = band->GetOverview(0);
  ASSERT_NE(nullptr, overview0);
  EXPECT_EQ(63, overview0->GetXSize());
  EXPECT_EQ(32, overview0->GetYSize());
  EXPECT_EQ(nullptr, band->GetOverview(1));
}

class GTiffBadTest : public GTiffTest {};

TEST_F(GTiffBadTest, TooManyBlocks) {
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
  const std::string data2(reinterpret_cast<const char*>(data),
                          CPL_ARRAYSIZE(data));
  autotest2::VsiMemTempWrapper wrapper(kFilename, data2);
  auto open_info =
      std::make_unique<GDALOpenInfo>(kFilename, GDAL_OF_READONLY, nullptr);

  auto dataset = absl::WrapUnique(driver_->pfnOpen(open_info.get()));
  EXPECT_EQ(nullptr, dataset);
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("Too many blocks"));
}

TEST_F(GTiffBadTest, TooManyBlocksSeparate) {
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
  const std::string data2(reinterpret_cast<const char*>(data),
                          CPL_ARRAYSIZE(data));
  autotest2::VsiMemTempWrapper wrapper(kFilename, data2);
  auto open_info =
      std::make_unique<GDALOpenInfo>(kFilename, GDAL_OF_READONLY, nullptr);

  auto dataset = absl::WrapUnique(driver_->pfnOpen(open_info.get()));
  EXPECT_EQ(nullptr, dataset);
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("Too many blocks"));
}

}  // namespace
}  // namespace autotest2
