// Copyright 2026 Google Inc. All Rights Reserved.
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
// Tests gcore/gdalorienteddataset.cpp.

#include "third_party/gdal/gcore/gdalorienteddataset.h"

#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"

namespace {

TEST(GdalorienteddatasetTest, AllOrientationsAndDataTypes) {
  GDALAllRegister();
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  const GDALOrientedDataset::Origin origins[] = {
      GDALOrientedDataset::Origin::TOP_LEFT,
      GDALOrientedDataset::Origin::TOP_RIGHT,
      GDALOrientedDataset::Origin::BOT_RIGHT,
      GDALOrientedDataset::Origin::BOT_LEFT,
      GDALOrientedDataset::Origin::LEFT_TOP,
      GDALOrientedDataset::Origin::RIGHT_TOP,
      GDALOrientedDataset::Origin::RIGHT_BOT,
      GDALOrientedDataset::Origin::LEFT_BOT,
  };

  // Test with both 1-byte (GDT_Byte) and 2-byte (GDT_UInt16) data types to
  // cover both branches of FlipLineHorizontally().
  for (GDALDataType dt : {GDT_Byte, GDT_UInt16}) {
    std::unique_ptr<GDALDataset> src_ds(
        mem_driver->Create("", 4, 3, 1, dt, nullptr));
    ASSERT_NE(nullptr, src_ds);
    src_ds->GetRasterBand(1)->SetColorInterpretation(GCI_GrayIndex);
    std::vector<GByte> init_data(4 * 3 * GDALGetDataTypeSizeBytes(dt), 1);
    for (size_t i = 0; i < init_data.size(); ++i) {
      init_data[i] = static_cast<GByte>((i + 1) & 0xFF);
    }
    ASSERT_EQ(CE_None, src_ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 4, 3, init_data.data(), 4, 3, dt, 0,
                           0, nullptr));

    for (GDALOrientedDataset::Origin origin : origins) {
      GDALOrientedDataset oriented_ds(src_ds.get(), origin);
      GDALRasterBand* band = oriented_ds.GetRasterBand(1);
      ASSERT_NE(nullptr, band);
      EXPECT_EQ(GCI_GrayIndex, band->GetColorInterpretation());

      std::vector<GByte> out_buf(oriented_ds.GetRasterXSize() *
                                     oriented_ds.GetRasterYSize() *
                                     GDALGetDataTypeSizeBytes(dt),
                                 0);
      EXPECT_EQ(CE_None, band->RasterIO(
                             GF_Read, 0, 0, oriented_ds.GetRasterXSize(),
                             oriented_ds.GetRasterYSize(), out_buf.data(),
                             oriented_ds.GetRasterXSize(),
                             oriented_ds.GetRasterYSize(), dt, 0, 0, nullptr));
    }
  }
}

TEST(GdalorienteddatasetTest, UniquePtrCtorAndMetadata) {
  GDALAllRegister();
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
