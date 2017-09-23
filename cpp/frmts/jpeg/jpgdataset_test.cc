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
// Tests JPEG raster driver.
//
// See also:
//   http://www.gdal.org/frmt_jpeg.html
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/gdrivers/jpeg.py

#include "frmts/jpeg/jpgdataset.h"
#include "port/cpl_port.h"

#include "file/base/path.h"
#include "gmock.h"
#include "gunit.h"
#include "third_party/absl/memory/memory.h"
#include "autotest2/cpp/util/error_handler.h"
#include "autotest2/cpp/util/vsimem.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"
#include "util/gtl/cleanup.h"

namespace autotest2 {
namespace {

TEST(JpegIdentifyTest, IdentifyDoesNotExist) {
  auto open_info = gtl::MakeUnique<GDALOpenInfo>("/does_not_exist",
                                                 GDAL_OF_READONLY, nullptr);
  ASSERT_NE(nullptr, open_info);
  EXPECT_EQ(FALSE, JPGDataset::Identify(open_info.get()));
}

TEST(JpegIdentifyTest, Subfile) {
  auto open_info = gtl::MakeUnique<GDALOpenInfo>("JPEG_SUBFILE:/does_not_exist",
                                                 GDAL_OF_READONLY, nullptr);
  ASSERT_NE(nullptr, open_info);
  EXPECT_EQ(TRUE, JPGDataset::Identify(open_info.get()));
}

constexpr char kIdentifyMinimum[] = "\xff\xd8\xff\xee\xff\x0e\x00\x64\x41";

TEST(JpegIdentifyTest, MagicOnly) {
  const char kFilename[] = "/vsimem/header-only.jpg";
  const string data2(reinterpret_cast<const char *>(kIdentifyMinimum),
                     CPL_ARRAYSIZE(kIdentifyMinimum));
  autotest2::VsiMemTempWrapper wrapper(kFilename, data2);
  auto open_info =
      gtl::MakeUnique<GDALOpenInfo>(kFilename, GDAL_OF_READONLY, nullptr);
  ASSERT_NE(nullptr, open_info);
  EXPECT_EQ(TRUE, JPGDataset::Identify(open_info.get()));
}

// 1x1 white pixel.
const char kOne[] =
    "\xff\xd8\xff\xe0\x00\x10\x4a\x46\x49\x46\x00\x01\x01\x01\x00\x48\x00\x48"
    "\x00\x00\xff\xdb\x00\x43\x00\x08\x06\x06\x07\x06\x05\x08\x07\x07\x07\x09"
    "\x09\x08\x0a\x0c\x14\x0d\x0c\x0b\x0b\x0c\x19\x12\x13\x0f\x14\x1d\x1a\x1f"
    "\x1e\x1d\x1a\x1c\x1c\x20\x24\x2e\x27\x20\x22\x2c\x23\x1c\x1c\x28\x37\x29"
    "\x2c\x30\x31\x34\x34\x34\x1f\x27\x39\x3d\x38\x32\x3c\x2e\x33\x34\x32\xff"
    "\xc0\x00\x0b\x08\x00\x01\x00\x01\x01\x01\x11\x00\xff\xc4\x00\x14\x00\x01"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x08\xff\xc4"
    "\x00\x14\x10\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\xff\xda\x00\x08\x01\x01\x00\x00\x3f\x00\x3f\xbf\xff\xd9";

TEST(JpegDatasetCommonTest, One) {
  const char kFilename[] = "/vsimem/one.jpg";
  const string data(reinterpret_cast<const char *>(kOne), CPL_ARRAYSIZE(kOne));
  autotest2::VsiMemTempWrapper wrapper(kFilename, data);
  auto open_info =
      gtl::MakeUnique<GDALOpenInfo>(kFilename, GDAL_OF_READONLY, nullptr);
  ASSERT_NE(nullptr, open_info);
  auto src = absl::WrapUnique(JPGDatasetCommon::Open(open_info.get()));
  ASSERT_NE(nullptr, src);
  double geo_transform[6] = {};
  src->GetGeoTransform(geo_transform);
  EXPECT_THAT(geo_transform, testing::ElementsAre(0, 1, 0, 0, 0, 1));
  EXPECT_EQ(0, src->GetGCPCount());
  EXPECT_EQ(1, src->GetRasterXSize());
  EXPECT_EQ(1, src->GetRasterYSize());
  EXPECT_EQ(1, src->GetRasterCount());

  // No file level metadata.
  EXPECT_EQ(nullptr, src->GetMetadata());

  GDALRasterBand *band = src->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  int block_xsize = 0;
  int block_ysize = 0;
  band->GetBlockSize(&block_xsize, &block_ysize);
  EXPECT_EQ(1, block_xsize);
  EXPECT_EQ(1, block_ysize);
  EXPECT_EQ(GDT_Byte, band->GetRasterDataType());
  EXPECT_EQ(GCI_GrayIndex, band->GetColorInterpretation());

  double minmax[2] = {0.0, 0.0};
  ASSERT_EQ(CE_None, band->ComputeRasterMinMax(false, minmax));
  EXPECT_THAT(minmax, testing::ElementsAre(0, 0));

  // band owns metadata.
  char **metadata_list = band->GetMetadata();
  EXPECT_EQ(nullptr, metadata_list);
}

const char kDatastreamContainsNoImage[] =
    "\xff\xd8\xff\x00\x00\x00\x00\x00\x00\x00\x0d\x64\x3d";

TEST(JpegDatasetCommonTest, NoImage) {
  const char kFilename[] = "/vsimem/no-image.jpg";
  const string data(reinterpret_cast<const char *>(kDatastreamContainsNoImage),
                    CPL_ARRAYSIZE(kDatastreamContainsNoImage));
  autotest2::VsiMemTempWrapper wrapper(kFilename, data);
  auto open_info =
      gtl::MakeUnique<GDALOpenInfo>(kFilename, GDAL_OF_READONLY, nullptr);
  ASSERT_NE(nullptr, open_info);

  WithQuietHandler error_handler;

  auto src = absl::WrapUnique(JPGDatasetCommon::Open(open_info.get()));
  ASSERT_EQ(nullptr, src);
  // Expect these two issues:
  //   libjpeg: Premature end of JPEG file
  //   libjpeg: JPEG datastream contains no image
  // Only JERR_NO_IMAGE from libjpeg will remain.
  EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(2, CPLGetErrorCounter());
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("contains no image"));
}

TEST(JpegDatasetCommonTest, b64482898_CorruptICCProfile) {
  // poc-9c3b06aec99bca6a6c945a7b8ee15e81a9472d243009684ef84a6bfddaf61393
  constexpr GByte kData[] =
      "\xff\xd8\xff\xc1\x00\x14\x08\x6a\x70\x67\x04\x04\x04\x34\x00\x00\x14\x00"
      "\x00\x21\x00\xc1\x14\x00\xff\xda\x00\x0e\x04\x04\x28\x00\x00\x00\x00\xc1"
      "\xda\xff\xff\xff\xff\xe2\x00\x0f\x49\x43\x43\x5f\x50\x52\x4f\x46\x49\x4c"
      "\x45\x00\x01\x01\x2f\x00";
  constexpr char kFilename[] = "/vsimem/a.jpg";
  auto data = const_cast<GByte *>(kData);

  // Do not take ownership.
  VSILFILE *f =
      VSIFileFromMemBuffer(kFilename, data, CPL_ARRAYSIZE(kData), FALSE);
  auto closer = gtl::MakeCleanup([f] { VSIFCloseL(f); });

  auto open_info =
      gtl::MakeUnique<GDALOpenInfo>(kFilename, GDAL_OF_READONLY, nullptr);
  ASSERT_NE(nullptr, open_info);

  auto src = absl::WrapUnique(JPGDatasetCommon::Open(open_info.get()));
  ASSERT_NE(nullptr, src);
  // EXPECT_EQ(0, CPLGetErrorCounter());

  WithQuietHandler error_handler;

  // This hits an issue with the corrupt ICCProfile.
  CSLDestroy(src->GetMetadataDomainList());

  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_LT(0, CPLGetErrorCounter());
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("nICCChunkLength"));
}

// TODO(schwehr): Port jpeg_28 to test writing EXIF and GPS tags.

}  // namespace
}  // namespace autotest2
