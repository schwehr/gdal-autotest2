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
// Tests oriented dataset wrapper (gcore/gdalorienteddataset.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdalorienteddataset.cpp

#include "third_party/gdal/gcore/gdalorienteddataset.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAreArray;

constexpr int kSrcWidth = 4;
constexpr int kSrcHeight = 3;

constexpr auto kOrigins = std::to_array<GDALOrientedDataset::Origin>({
    GDALOrientedDataset::Origin::TOP_LEFT,
    GDALOrientedDataset::Origin::TOP_RIGHT,
    GDALOrientedDataset::Origin::BOT_RIGHT,
    GDALOrientedDataset::Origin::BOT_LEFT,
    GDALOrientedDataset::Origin::LEFT_TOP,
    GDALOrientedDataset::Origin::RIGHT_TOP,
    GDALOrientedDataset::Origin::RIGHT_BOT,
    GDALOrientedDataset::Origin::LEFT_BOT,
});

// Expected 1-based source pixel indices after applying each EXIF orientation
// transform to a 4x3 raster containing {1..12} in row-major order.
constexpr std::array<std::array<int, 12>, 8> kExpectedPixelOrder = {{
    {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12},  // TOP_LEFT (4x3)
    {4, 3, 2, 1, 8, 7, 6, 5, 12, 11, 10, 9},  // TOP_RIGHT (4x3)
    {12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1},  // BOT_RIGHT (4x3)
    {9, 10, 11, 12, 5, 6, 7, 8, 1, 2, 3, 4},  // BOT_LEFT (4x3)
    {1, 5, 9, 2, 6, 10, 3, 7, 11, 4, 8, 12},  // LEFT_TOP (3x4)
    {9, 5, 1, 10, 6, 2, 11, 7, 3, 12, 8, 4},  // RIGHT_TOP (3x4)
    {12, 8, 4, 11, 7, 3, 10, 6, 2, 9, 5, 1},  // RIGHT_BOT (3x4)
    {4, 8, 12, 3, 7, 11, 2, 6, 10, 1, 5, 9},  // LEFT_BOT (3x4)
}};

TEST(GdalOrientedDatasetTest, AllOrientationsAndDataTypes) {
  GDALRegister_MEM();
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  // Test both 1-byte (GDT_Byte) and 2-byte (GDT_UInt16) data types to cover
  // both branches of FlipLineHorizontally() and pixel-byte transposes.
  for (GDALDataType dt : {GDT_Byte, GDT_UInt16}) {
    SCOPED_TRACE(GDALGetDataTypeName(dt));
    GDALDatasetUniquePtr src_ds(
        mem_driver->Create("", kSrcWidth, kSrcHeight, 1, dt, nullptr));
    ASSERT_NE(nullptr, src_ds);
    src_ds->GetRasterBand(1)->SetColorInterpretation(GCI_GrayIndex);

    if (dt == GDT_Byte) {
      constexpr auto kSrcBytes =
          std::to_array<GByte>({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});
      ASSERT_EQ(CE_None, src_ds->GetRasterBand(1)->RasterIO(
                             GF_Write, 0, 0, kSrcWidth, kSrcHeight,
                             const_cast<GByte*>(kSrcBytes.data()), kSrcWidth,
                             kSrcHeight, GDT_Byte, 0, 0, nullptr));
    } else {
      constexpr auto kSrcU16 =
          std::to_array<uint16_t>({1001, 1002, 1003, 1004, 1005, 1006, 1007,
                                   1008, 1009, 1010, 1011, 1012});
      ASSERT_EQ(CE_None, src_ds->GetRasterBand(1)->RasterIO(
                             GF_Write, 0, 0, kSrcWidth, kSrcHeight,
                             const_cast<uint16_t*>(kSrcU16.data()), kSrcWidth,
                             kSrcHeight, GDT_UInt16, 0, 0, nullptr));
    }

    for (size_t idx = 0; idx < kOrigins.size(); ++idx) {
      GDALOrientedDataset oriented_ds(src_ds.get(), kOrigins[idx]);
      GDALRasterBand* band = oriented_ds.GetRasterBand(1);
      ASSERT_NE(nullptr, band);
      EXPECT_EQ(GCI_GrayIndex, band->GetColorInterpretation());
      const int out_w = oriented_ds.GetRasterXSize();
      const int out_h = oriented_ds.GetRasterYSize();
      EXPECT_EQ(12, out_w * out_h);

      if (dt == GDT_Byte) {
        std::array<GByte, 12> expected{};
        for (size_t i = 0; i < 12; ++i) {
          expected[i] = static_cast<GByte>(kExpectedPixelOrder[idx][i]);
        }
        std::vector<GByte> out_buf(12, 0);
        ASSERT_EQ(CE_None,
                  band->RasterIO(GF_Read, 0, 0, out_w, out_h, out_buf.data(),
                                 out_w, out_h, GDT_Byte, 0, 0, nullptr));
        EXPECT_THAT(out_buf, ElementsAreArray(expected));
      } else {
        std::array<uint16_t, 12> expected{};
        for (size_t i = 0; i < 12; ++i) {
          expected[i] =
              static_cast<uint16_t>(1000 + kExpectedPixelOrder[idx][i]);
        }
        std::vector<uint16_t> out_buf(12, 0);
        ASSERT_EQ(CE_None,
                  band->RasterIO(GF_Read, 0, 0, out_w, out_h, out_buf.data(),
                                 out_w, out_h, GDT_UInt16, 0, 0, nullptr));
        EXPECT_THAT(out_buf, ElementsAreArray(expected));
      }
    }
  }
}

TEST(GdalOrientedDatasetTest, UniquePtrCtorAndMetadata) {
  GDALRegister_MEM();
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  std::unique_ptr<GDALDataset> src_ds(
      mem_driver->Create("", 2, 2, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);
  src_ds->SetMetadataItem("EXIF_Orientation", "6", "");
  src_ds->SetMetadataItem("OTHER", "VAL", "");
  src_ds->SetMetadataItem("EXIF_Orientation", "6", "EXIF");
  src_ds->SetMetadataItem("CUSTOM_K", "CUSTOM_V", "CUSTOM_DOM");

  GDALOrientedDataset oriented_ds(std::move(src_ds),
                                  GDALOrientedDataset::Origin::RIGHT_TOP);
  EXPECT_EQ(nullptr, oriented_ds.GetMetadataItem("EXIF_Orientation", ""));
  EXPECT_STREQ("6",
               oriented_ds.GetMetadataItem("original_EXIF_Orientation", ""));
  EXPECT_STREQ("VAL", oriented_ds.GetMetadataItem("OTHER", ""));

  EXPECT_EQ(nullptr, oriented_ds.GetMetadataItem("EXIF_Orientation", "EXIF"));
  EXPECT_STREQ(
      "6", oriented_ds.GetMetadataItem("original_EXIF_Orientation", "EXIF"));

  EXPECT_STREQ("CUSTOM_V",
               oriented_ds.GetMetadataItem("CUSTOM_K", "CUSTOM_DOM"));
}

}  // namespace
}  // namespace autotest2
