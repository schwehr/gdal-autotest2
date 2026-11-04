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
// Tests EHDR raw raster driver (ehdrdataset.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/raw/ehdrdataset.cpp
//   https://gdal.org/drivers/raster/ehdr.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/ehdr.py

#include "third_party/gdal/frmts/raw/ehdrdataset.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <format>      // NOLINT
#include <limits>
#include <memory>
#include <span>  // NOLINT
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/matchers.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_geotransform.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::autotest2::IsSameAs;
using ::testing::ElementsAre;
using ::testing::HasSubstr;

constexpr char kBaseDir[] = "/vsimem/ehdrdataset_test";

class ScopedVsiDir {
 public:
  explicit ScopedVsiDir(std::string_view subname)
      : dir_(std::format("{}/{}", kBaseDir, subname)) {
    VSIMkdirRecursive(dir_.c_str(), 0777);
  }
  ScopedVsiDir(const ScopedVsiDir&) = delete;
  ScopedVsiDir& operator=(const ScopedVsiDir&) = delete;
  ~ScopedVsiDir() { VSIRmdirRecursive(dir_.c_str()); }

  std::string file(std::string_view name) const {
    return std::format("{}/{}", dir_, name);
  }

 private:
  std::string dir_;
};

void WriteVsiBytes(const char* path, std::span<const GByte> bytes) {
  VSILFILE* fp = VSIFOpenL(path, "wb");
  ASSERT_NE(nullptr, fp) << path;
  absl::Cleanup close_fp = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
  if (!bytes.empty()) {
    ASSERT_EQ(bytes.size(), VSIFWriteL(bytes.data(), 1, bytes.size(), fp));
  }
}

void WriteVsiText(const char* path, std::string_view text) {
  WriteVsiBytes(
      path, std::span<const GByte>(reinterpret_cast<const GByte*>(text.data()),
                                   text.size()));
}

class EhdrDatasetTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_EHdr(); }
};

// -----------------------------------------------------------------------------
// Driver Registration
// -----------------------------------------------------------------------------

TEST_F(EhdrDatasetTest, DriverRegistrationAndMetadata) {
  GDALRegister_EHdr();
  GDALRegister_EHdr();  // Repeat call for early return branch.

  GDALDriverManager* mgr = GetGDALDriverManager();
  ASSERT_NE(nullptr, mgr);

  GDALDriver* drv = mgr->GetDriverByName("EHdr");
  ASSERT_NE(nullptr, drv);

  EXPECT_STREQ("YES", drv->GetMetadataItem(GDAL_DCAP_RASTER));
  EXPECT_STREQ("ESRI .hdr Labelled", drv->GetMetadataItem(GDAL_DMD_LONGNAME));
  EXPECT_STREQ("drivers/raster/ehdr.html",
               drv->GetMetadataItem(GDAL_DMD_HELPTOPIC));
  EXPECT_STREQ("bil", drv->GetMetadataItem(GDAL_DMD_EXTENSION));
  EXPECT_STREQ("Byte Int8 Int16 UInt16 Int32 UInt32 Float32",
               drv->GetMetadataItem(GDAL_DMD_CREATIONDATATYPES));
  EXPECT_STREQ("YES", drv->GetMetadataItem(GDAL_DCAP_UPDATE));
  EXPECT_STREQ("GeoTransform SRS NoData RasterValues",
               drv->GetMetadataItem(GDAL_DMD_UPDATE_ITEMS));
  EXPECT_STREQ("YES", drv->GetMetadataItem(GDAL_DCAP_VIRTUALIO));
  EXPECT_THAT(drv->GetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST),
              HasSubstr("NBITS"));
}

// -----------------------------------------------------------------------------
// Open Rejections and Edge Cases
// -----------------------------------------------------------------------------

TEST_F(EhdrDatasetTest, OpenDoesNotExist) {
  auto open_info = std::make_unique<GDALOpenInfo>("/does_not_exist",
                                                  GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(nullptr, EHdrDataset::Open(open_info.get()));
}

TEST_F(EhdrDatasetTest, OpenShortFileOrZarrRejected) {
  ScopedVsiDir dir("short_file");
  const std::string short_path = dir.file("short.bil");
  WriteVsiText(short_path.c_str(), "x");  // Only 1 byte (< 2)

  GDALOpenInfo info1(short_path.c_str(), GA_ReadOnly);
  EXPECT_EQ(nullptr, EHdrDataset::Open(&info1));

  // .zarr extension rejection when not single allowed driver
  const std::string zarr_path = dir.file("test.zarr");
  const std::string zarr_hdr = dir.file("test.hdr");
  WriteVsiText(zarr_path.c_str(), "xx");
  WriteVsiText(zarr_hdr.c_str(), "nrows 1\nncols 2\nnbands 1\nnbits 8\n");
  GDALOpenInfo info2(zarr_path.c_str(), GA_ReadOnly);
  EXPECT_EQ(nullptr, EHdrDataset::Open(&info2));

  const char* const allowed_drivers[] = {"EHdr", nullptr};
  std::unique_ptr<GDALDataset> ds(
      static_cast<GDALDataset*>(GDALOpenEx(zarr_path.c_str(),
                                           GDAL_OF_RASTER | GDAL_OF_READONLY,
                                           allowed_drivers, nullptr, nullptr)));
  EXPECT_NE(nullptr, ds);
}

TEST_F(EhdrDatasetTest, OpenSelectedHdrFailsWithExplanation) {
  ScopedVsiDir dir("open_hdr");
  const std::string hdr_path = dir.file("sample.hdr");
  WriteVsiText(hdr_path.c_str(),
               "nrows 10\n"
               "ncols 10\n"
               "nbands 1\n"
               "nbits 8\n");

  WithQuietHandler quiet;
  CPLErrorReset();
  GDALOpenInfo info(hdr_path.c_str(), GA_ReadOnly);
  EXPECT_EQ(nullptr, EHdrDataset::Open(&info));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("The selected file is an ESRI BIL header file"));
}

TEST_F(EhdrDatasetTest, OpenInvalidHeaders) {
  ScopedVsiDir dir("invalid_hdr");
  const std::string data_path = dir.file("invalid.bil");
  const std::string hdr_path = dir.file("invalid.hdr");
  WriteVsiBytes(data_path.c_str(), std::vector<GByte>(100, 0));

  // Missing nrows/ncols
  WriteVsiText(hdr_path.c_str(), "nbands 1\n");
  GDALOpenInfo info1(data_path.c_str(), GA_ReadOnly);
  EXPECT_EQ(nullptr, EHdrDataset::Open(&info1));

  // Invalid dimensions (0 cols)
  WriteVsiText(hdr_path.c_str(), "nrows 10\nncols 0\n");
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALOpenInfo info2(data_path.c_str(), GA_ReadOnly);
    EXPECT_EQ(nullptr, EHdrDataset::Open(&info2));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // Unsupported NBITS
  WriteVsiText(hdr_path.c_str(), "nrows 10\nncols 10\nnbits 12\n");
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALOpenInfo info3(data_path.c_str(), GA_ReadOnly);
    EXPECT_EQ(nullptr, EHdrDataset::Open(&info3));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("EHdr driver does not support 12 NBITS value"));
  }

  // Sibling files lookup: no matching .hdr
  const char* const siblings[] = {"invalid.bil", "other.txt", nullptr};
  GDALOpenInfo info_siblings(data_path.c_str(), GDAL_OF_READONLY, siblings);
  EXPECT_EQ(nullptr, EHdrDataset::Open(&info_siblings));

  // Integer overflow in BSQ layout offset calculation
  WriteVsiText(hdr_path.c_str(),
               "nrows 10\nncols 2147483647\nnbands 1\nnbits 16\nlayout BSQ\n");
  {
    WithQuietHandler quiet_overflow;
    CPLErrorReset();
    GDALOpenInfo info_bsq(data_path.c_str(), GA_ReadOnly);
    EXPECT_EQ(nullptr, EHdrDataset::Open(&info_bsq, /*bFileSizeCheck=*/false));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Int overflow occurred"));
  }

  // Integer overflow in BIL layout offset calculation
  WriteVsiText(hdr_path.c_str(),
               "nrows 10\nncols 2147483647\nnbands 2\nnbits 8\nlayout BIL\n");
  {
    WithQuietHandler quiet_overflow;
    CPLErrorReset();
    GDALOpenInfo info_bil(data_path.c_str(), GA_ReadOnly);
    EXPECT_EQ(nullptr, EHdrDataset::Open(&info_bil, /*bFileSizeCheck=*/false));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Int overflow occurred"));
  }
}

TEST_F(EhdrDatasetTest, OpenWithSiblingFilesSuccess) {
  ScopedVsiDir dir("siblings_ok");
  const std::string data_path = dir.file("test.bil");
  const std::string hdr_path = dir.file("test.hdr");
  WriteVsiBytes(data_path.c_str(), std::vector<GByte>(100, 42));
  WriteVsiText(hdr_path.c_str(),
               "nrows 10\n"
               "ncols 10\n"
               "nbands 1\n"
               "nbits 8\n");

  const char* const siblings[] = {"test.bil", "test.hdr", nullptr};
  GDALOpenInfo info(data_path.c_str(), GDAL_OF_READONLY, siblings);
  std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(10, ds->GetRasterXSize());
  EXPECT_EQ(10, ds->GetRasterYSize());
}

TEST_F(EhdrDatasetTest, OpenSrcExtensionWithSchHeader) {
  ScopedVsiDir dir("src_sch");
  // 7 chars stem starting with e/w and having n/s at pos 4: e100n40
  const std::string data_path = dir.file("e100n40.src");
  const std::string sch_path = dir.file("e100n40.sch");
  WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
  WriteVsiText(sch_path.c_str(),
               "nrows 4\n"
               "ncols 4\n"
               "nbits 8\n");

  GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
  std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(4, ds->GetRasterXSize());
  EXPECT_EQ(4, ds->GetRasterYSize());
}

// -----------------------------------------------------------------------------
// Pixel Type & NBITS Inference
// -----------------------------------------------------------------------------

TEST_F(EhdrDatasetTest, InferDataTypeFromFilesizeAndPixelType) {
  ScopedVsiDir dir("infer_type");
  const std::string data_path = dir.file("data.bil");
  const std::string hdr_path = dir.file("data.hdr");

  // 1. Filesize > 8 bytes per pixel -> failure
  WriteVsiBytes(data_path.c_str(), std::vector<GByte>(100, 0));
  WriteVsiText(hdr_path.c_str(), "nrows 2\nncols 2\nnbands 1\n");
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    EXPECT_EQ(nullptr, EHdrDataset::Open(&info));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("cannot infer NBITS value"));
  }

  // 2. 4 bytes per pixel -> Float32
  WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));  // 2*2*4 = 16
  {
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(1, ds->GetRasterCount());
    EXPECT_EQ(GDT_Float32, ds->GetRasterBand(1)->GetRasterDataType());
  }

  // 3. 2 bytes per pixel -> UInt16
  WriteVsiBytes(data_path.c_str(), std::vector<GByte>(8, 0));  // 2*2*2 = 8
  {
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(1, ds->GetRasterCount());
    EXPECT_EQ(GDT_UInt16, ds->GetRasterBand(1)->GetRasterDataType());
  }

  // 4. Negative nodata with chPixelType == 'N' -> signed (Int16)
  WriteVsiText(hdr_path.c_str(),
               "nrows 2\nncols 2\nnbands 1\nNODATA -9999\n");
  {
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(1, ds->GetRasterCount());
    EXPECT_EQ(GDT_Int16, ds->GetRasterBand(1)->GetRasterDataType());
  }

  // 5. Short file with .flt extension -> Float32
  const std::string flt_data = dir.file("test.flt");
  const std::string flt_hdr = dir.file("test.hdr");
  WriteVsiBytes(flt_data.c_str(), std::vector<GByte>(2, 0));
  WriteVsiText(flt_hdr.c_str(), "nrows 2\nncols 2\nnbands 1\n");
  {
    GDALOpenInfo info(flt_data.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info, false));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(1, ds->GetRasterCount());
    EXPECT_EQ(GDT_Float32, ds->GetRasterBand(1)->GetRasterDataType());
  }

  // 6. Short file with .bil extension -> Byte
  const std::string bil_data = dir.file("test_zero.bil");
  const std::string bil_hdr = dir.file("test_zero.hdr");
  WriteVsiBytes(bil_data.c_str(), std::vector<GByte>(2, 0));
  WriteVsiText(bil_hdr.c_str(), "nrows 2\nncols 2\nnbands 1\n");
  {
    GDALOpenInfo info(bil_data.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info, false));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(1, ds->GetRasterCount());
    EXPECT_EQ(GDT_Byte, ds->GetRasterBand(1)->GetRasterDataType());
  }
}

TEST_F(EhdrDatasetTest, ExplicitNbitsAndPixelTypes) {
  ScopedVsiDir dir("explicit_types");
  const std::string data_path = dir.file("sample.bil");
  const std::string hdr_path = dir.file("sample.hdr");

  struct TestCase {
    const char* hdr_content;
    GDALDataType expected_type;
  };

  constexpr TestCase kCases[] = {
      {.hdr_content = "nrows 2\nncols 2\nnbands 1\nnbits 16\npixeltype S\n",
       .expected_type = GDT_Int16},
      {.hdr_content = "nrows 2\nncols 2\nnbands 1\nnbits 16\npixeltype U\n",
       .expected_type = GDT_UInt16},
      {.hdr_content = "nrows 2\nncols 2\nnbands 1\nnbits 32\npixeltype S\n",
       .expected_type = GDT_Int32},
      {.hdr_content = "nrows 2\nncols 2\nnbands 1\nnbits 32\npixeltype F\n",
       .expected_type = GDT_Float32},
      {.hdr_content = "nrows 2\nncols 2\nnbands 1\nnbits 32\npixeltype U\n",
       .expected_type = GDT_UInt32},
      {.hdr_content = "nrows 2\nncols 2\nnbands 1\nnbits 8\npixeltype S\n",
       .expected_type = GDT_Int8},
      {.hdr_content = "nrows 2\nncols 2\nnbands 1\nnbits 8\npixeltype U\n",
       .expected_type = GDT_Byte},
  };

  WriteVsiBytes(data_path.c_str(), std::vector<GByte>(64, 0));

  for (const auto& tc : kCases) {
    WriteVsiText(hdr_path.c_str(), tc.hdr_content);
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info, false));
    ASSERT_NE(nullptr, ds) << tc.hdr_content;
    ASSERT_EQ(1, ds->GetRasterCount());
    EXPECT_EQ(tc.expected_type, ds->GetRasterBand(1)->GetRasterDataType());
  }
}

// -----------------------------------------------------------------------------
// Layouts (BIP, BSQ, BIL) and Byte Orders
// -----------------------------------------------------------------------------

TEST_F(EhdrDatasetTest, LayoutsAndByteOrders) {
  ScopedVsiDir dir("layouts");
  const std::string data_path = dir.file("layout.bil");
  const std::string hdr_path = dir.file("layout.hdr");
  WriteVsiBytes(data_path.c_str(), std::vector<GByte>(100, 0));

  for (const char* layout : {"BIP", "BSQ", "BIL"}) {
    for (char bo : {'I', 'L', 'M'}) {
      std::string hdr = std::format(
          "nrows 2\nncols 2\nnbands 2\nnbits 8\nlayout {}\nbyteorder {}\n",
          layout, bo);
      WriteVsiText(hdr_path.c_str(), hdr);

      GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
      std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info, false));
      ASSERT_NE(nullptr, ds) << hdr;
      EXPECT_EQ(2, ds->GetRasterCount());
    }
  }

  // Integer overflow on BIP layout
  std::string overflow_hdr =
      std::format("nrows 2\nncols {}\nnbands 1000\nnbits 32\nlayout BIP\n",
                  std::numeric_limits<int>::max() / 100);
  WriteVsiText(hdr_path.c_str(), overflow_hdr);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    EXPECT_EQ(nullptr, EHdrDataset::Open(&info, false));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Int overflow"));
  }
}

// -----------------------------------------------------------------------------
// Sub-Byte (NBITS < 8) Support: Reading and Writing Blocks
// -----------------------------------------------------------------------------

TEST_F(EhdrDatasetTest, SubByteRasterBandReadWrite) {
  ScopedVsiDir dir("sub_byte");
  const std::string data_path = dir.file("4bit.bil");
  const std::string hdr_path = dir.file("4bit.hdr");

  // 4 rows, 4 cols, 2 bands, 4 bits per pixel.
  // Row bytes = (4 * 4 + 7) / 8 = 2 bytes per band per row.
  // 4 rows * 2 bytes * 2 bands = 16 bytes.
  std::vector<GByte> raw_data(16, 0);
  // Band 1 row 0: pixels 0x1, 0x2, 0x3, 0x4 -> byte0=0x12, byte1=0x34
  raw_data[0] = 0x12;
  raw_data[1] = 0x34;
  // Band 2 row 0: pixels 0x5, 0x6, 0x7, 0x8 -> byte2=0x56, byte3=0x78
  raw_data[2] = 0x56;
  raw_data[3] = 0x78;
  WriteVsiBytes(data_path.c_str(), raw_data);

  WriteVsiText(hdr_path.c_str(),
               "nrows 4\n"
               "ncols 4\n"
               "nbands 2\n"
               "nbits 4\n"
               "skipbytes 0\n"
               "bandrowbytes 2\n"
               "totalrowbytes 4\n");

  // Open dataset to act as parent for EHdrRasterBand
  GDALOpenInfo info(data_path.c_str(), GA_Update);
  std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info, false));
  ASSERT_NE(nullptr, ds);

  VSILFILE* fp = VSIFOpenL(data_path.c_str(), "rb+");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup close_fp = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };

  // 1. Direct sub-byte band construction with nBits = 4
  EHdrRasterBand band1(ds.get(), 1, fp, 0, 1, 4, GDT_Byte,
                       RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN, 4);
  ASSERT_TRUE(band1.IsValid());
  band1.SetAccess(GA_Update);
  EXPECT_STREQ("4", band1.GetMetadataItem("NBITS", "IMAGE_STRUCTURE"));

  EHdrRasterBand band2(ds.get(), 2, fp, 0, 1, 4, GDT_Byte,
                       RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN, 4);
  ASSERT_TRUE(band2.IsValid());

  std::vector<GByte> pixels(4);
  ASSERT_EQ(CE_None, band1.RasterIO(GF_Read, 0, 0, 4, 1, pixels.data(), 4, 1,
                                     GDT_Byte, 0, 0, nullptr));
  EXPECT_THAT(pixels, ElementsAre(0x1, 0x2, 0x3, 0x4));

  ASSERT_EQ(CE_None, band2.RasterIO(GF_Read, 0, 0, 4, 1, pixels.data(), 4, 1,
                                     GDT_Byte, 0, 0, nullptr));
  EXPECT_THAT(pixels, ElementsAre(0x5, 0x6, 0x7, 0x8));

  // Exercise IRasterIO fallback paths with GDT_Int16
  std::vector<int16_t> int16_pixels(4);
  EXPECT_EQ(CE_None, band1.RasterIO(GF_Read, 0, 0, 4, 1, int16_pixels.data(),
                                    4, 1, GDT_Int16, 0, 0, nullptr));
  EXPECT_THAT(int16_pixels, ElementsAre(1, 2, 3, 4));

  // 2. Write test: change pixels in band 1 row 0 to 0xA, 0xB, 0xC, 0xD
  pixels = {0xA, 0xB, 0xC, 0xD};
  ASSERT_EQ(CE_None, band1.RasterIO(GF_Write, 0, 0, 4, 1, pixels.data(), 4,
                                     1, GDT_Byte, 0, 0, nullptr));
  EXPECT_EQ(CE_None, band1.FlushCache(false));

  // 3. Verify write persisted
  EHdrRasterBand verify_band1(ds.get(), 1, fp, 0, 1, 4, GDT_Byte,
                              RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN, 4);
  ASSERT_TRUE(verify_band1.IsValid());
  std::vector<GByte> read_pixels(4);
  ASSERT_EQ(CE_None,
            verify_band1.RasterIO(GF_Read, 0, 0, 4, 1, read_pixels.data(), 4, 1,
                                  GDT_Byte, 0, 0, nullptr));
  EXPECT_THAT(read_pixels, ElementsAre(0xA, 0xB, 0xC, 0xD));

  // 4. Sub-byte constructor error paths
  // 4a. skipbytes < 0 causes Open() to fail because band 1 offset is negative
  {
    WriteVsiText(hdr_path.c_str(),
                 "nrows 4\nncols 4\nnbands 2\nnbits 4\nskipbytes -5\n");
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALOpenInfo bad_info(data_path.c_str(), GA_ReadOnly);
    EXPECT_EQ(nullptr, EHdrDataset::Open(&bad_info, false));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 4b. bandrowbytes and totalrowbytes negative error paths in EHdrRasterBand
  for (const char* bad_key : {"bandrowbytes -1", "totalrowbytes -1"}) {
    std::string bad_hdr =
        std::format("nrows 4\nncols 4\nnbands 2\nnbits 8\n{}\n", bad_key);
    WriteVsiText(hdr_path.c_str(), bad_hdr);
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALOpenInfo bad_info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> bad_ds(EHdrDataset::Open(&bad_info, false));
    ASSERT_NE(nullptr, bad_ds);
    EHdrRasterBand bad_band(bad_ds.get(), 2, fp, 0, 1, 4, GDT_Byte,
                            RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN, 4);
    EXPECT_FALSE(bad_band.IsValid());
  }
}

// -----------------------------------------------------------------------------
// Geotransform and Spatial Reference
// -----------------------------------------------------------------------------

TEST_F(EhdrDatasetTest, GeotransformKeywordsAndWorldFile) {
  ScopedVsiDir dir("geo");
  const std::string data_path = dir.file("grid.bil");
  const std::string hdr_path = dir.file("grid.hdr");
  WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));

  // 1. Center transform (ulxmap, ulymap, xdim, ydim)
  WriteVsiText(hdr_path.c_str(),
               "nrows 4\n"
               "ncols 4\n"
               "nbands 1\n"
               "nbits 8\n"
               "ulxmap 100.5\n"
               "ulymap 200.5\n"
               "xdim 10.0\n"
               "ydim 20.0\n");
  {
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt;
    EXPECT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_NEAR(95.5, gt[0], 1e-5);
    EXPECT_NEAR(10.0, gt[1], 1e-5);
    EXPECT_DOUBLE_EQ(0.0, gt[2]);
    EXPECT_NEAR(210.5, gt[3], 1e-5);
    EXPECT_DOUBLE_EQ(0.0, gt[4]);
    EXPECT_NEAR(-20.0, gt[5], 1e-5);
  }

  // 2. Corner transform (xllcorner, yllcorner, cellsize)
  WriteVsiText(hdr_path.c_str(),
               "nrows 4\n"
               "ncols 4\n"
               "nbands 1\n"
               "nbits 8\n"
               "xllcorner 50.0\n"
               "yllcorner 100.0\n"
               "cellsize 5.0\n");
  {
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt;
    EXPECT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(50.0, gt[0]);
    EXPECT_DOUBLE_EQ(5.0, gt[1]);
    EXPECT_DOUBLE_EQ(0.0, gt[2]);
    EXPECT_DOUBLE_EQ(120.0, gt[3]);  // yll + nrows * cellsize = 100 + 4*5
    EXPECT_DOUBLE_EQ(0.0, gt[4]);
    EXPECT_DOUBLE_EQ(-5.0, gt[5]);
  }

  // 3. Center transform with xllcenter / yllcenter
  WriteVsiText(hdr_path.c_str(),
               "nrows 4\n"
               "ncols 4\n"
               "nbands 1\n"
               "nbits 8\n"
               "xllcenter 52.5\n"
               "yllcenter 102.5\n"
               "cellsize 5.0\n");
  {
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt;
    EXPECT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(50.0, gt[0]);
    EXPECT_DOUBLE_EQ(120.0, gt[3]);
  }

  // 4. Worldfile fallback (.blw and .wld)
  WriteVsiText(hdr_path.c_str(), "nrows 4\nncols 4\nnbands 1\nnbits 8\n");
  const std::string blw_path = dir.file("grid.blw");
  WriteVsiText(blw_path.c_str(), "2.0\n0.0\n0.0\n-2.0\n10.0\n20.0\n");
  {
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt;
    EXPECT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(9.0, gt[0]);
    EXPECT_DOUBLE_EQ(2.0, gt[1]);
    EXPECT_DOUBLE_EQ(21.0, gt[3]);
    EXPECT_DOUBLE_EQ(-2.0, gt[5]);
  }

  // 5. Fallback to .wld when .blw is absent
  EXPECT_EQ(0, VSIUnlink(blw_path.c_str()));
  const std::string wld_path = dir.file("grid.wld");
  WriteVsiText(wld_path.c_str(), "3.0\n0.0\n0.0\n-3.0\n30.0\n40.0\n");
  {
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt;
    EXPECT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(28.5, gt[0]);
    EXPECT_DOUBLE_EQ(3.0, gt[1]);
    EXPECT_DOUBLE_EQ(41.5, gt[3]);
    EXPECT_DOUBLE_EQ(-3.0, gt[5]);
  }
}

TEST_F(EhdrDatasetTest, SetGeoTransformAndResetKeyValue) {
  ScopedVsiDir dir("set_gt");
  const std::string data_path = dir.file("mod.bil");
  const std::string hdr_path = dir.file("mod.hdr");
  WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
  WriteVsiText(hdr_path.c_str(),
               "nrows 4\nncols 4\nnbands 1\nnbits 8\n"
               "ulxmap 1.0\nulymap 2.0\nxdim 1.0\nydim 1.0\n");

  {
    GDALOpenInfo info(data_path.c_str(), GA_Update);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
    ASSERT_NE(nullptr, ds);

    // Rotated transform delegates to Pam
    GDALGeoTransform rot_gt = {0.0, 1.0, 0.5, 0.0, 0.5, -1.0};
    EXPECT_EQ(CE_None, ds->SetGeoTransform(rot_gt));

    // Non-rotated transform strips old keys and sets ULXMAP etc.
    GDALGeoTransform new_gt = {100.0, 2.0, 0.0, 200.0, 0.0, -3.0};
    EXPECT_EQ(CE_None, ds->SetGeoTransform(new_gt));
    // Set again to test in-place update in ResetKeyValue
    GDALGeoTransform update_gt = {105.0, 2.0, 0.0, 205.0, 0.0, -3.0};
    EXPECT_EQ(CE_None, ds->SetGeoTransform(update_gt));
  }

  // Verify updated .hdr on reopen
  {
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt;
    EXPECT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(105.0, gt[0]);
    EXPECT_DOUBLE_EQ(2.0, gt[1]);
    EXPECT_DOUBLE_EQ(205.0, gt[3]);
    EXPECT_DOUBLE_EQ(-3.0, gt[5]);
  }
}

TEST_F(EhdrDatasetTest, PrjFileAndArcSecondsConversion) {
  ScopedVsiDir dir("prj");
  const std::string data_path = dir.file("geo.bil");
  const std::string hdr_path = dir.file("geo.hdr");
  const std::string prj_path = dir.file("geo.prj");
  WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
  WriteVsiText(hdr_path.c_str(),
               "nrows 4\nncols 4\nnbands 1\nnbits 8\n"
               "ulxmap 3600.0\nulymap 7200.0\nxdim 36.0\nydim 36.0\n");

  // Geographic WKT with "Units DS" (arc-seconds)
  WriteVsiText(prj_path.c_str(),
               "GEOGCS[\"GCS_WGS_1984\",DATUM[\"D_WGS_1984\",SPHEROID[\"WGS_"
               "1984\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT["
               "\"Degree\",0.0174532925199433]]\n"
               "Units DS\n");

  {
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    const OGRSpatialReference* srs = ds->GetSpatialRef();
    ASSERT_NE(nullptr, srs);
    EXPECT_TRUE(srs->IsGeographic());

    // Coordinates divided by 3600.0
    GDALGeoTransform gt;
    EXPECT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_NEAR(3600.0 / 3600.0 - 0.5 * (36.0 / 3600.0), gt[0], 1e-5);
  }

  // Test SetSpatialRef writing out .prj
  {
    GDALOpenInfo info(data_path.c_str(), GA_Update);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
    ASSERT_NE(nullptr, ds);

    OGRSpatialReference utm;
    utm.SetUTM(11, TRUE);
    utm.SetWellKnownGeogCS("WGS84");
    EXPECT_EQ(CE_None, ds->SetSpatialRef(&utm));
    ASSERT_NE(nullptr, ds->GetSpatialRef());
    EXPECT_THAT(*ds->GetSpatialRef(), IsSameAs(utm));
    // Clear SRS
    EXPECT_EQ(CE_None, ds->SetSpatialRef(nullptr));
    // File list includes .prj
    StringListPtr file_list(ds->GetFileList(), CSLDestroy);
    std::vector<std::string> files = CslToVector(file_list.get());
    EXPECT_THAT(files, ::testing::Contains(prj_path));
  }

  // Invalid .prj file causes OGRSpatialReference to be cleared
  WriteVsiText(prj_path.c_str(), "INVALID_PRJ_CONTENT\n");
  {
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(nullptr, ds->GetSpatialRef());
  }
}

TEST_F(EhdrDatasetTest, InternalProjectionInHdr) {
  ScopedVsiDir dir("internal_proj");
  const std::string data_path = dir.file("worldclim.bil");
  const std::string hdr_path = dir.file("worldclim.hdr");
  WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));

  WriteVsiText(hdr_path.c_str(),
               "nrows 4\nncols 4\nnbands 1\nnbits 8\n"
               "Projection GEOGRAPHIC\n"
               "Datum WGS84\n");

  GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
  std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
  ASSERT_NE(nullptr, ds);
  ASSERT_NE(nullptr, ds->GetSpatialRef());
  EXPECT_TRUE(ds->GetSpatialRef()->IsGeographic());
}

TEST_F(EhdrDatasetTest, SpdfImageRepParsing) {
  ScopedVsiDir dir("spdf");
  const std::string data_path = dir.file("spdf_test.bil");
  const std::string hdr_path = dir.file("spdf_test.hdr");
  const std::string rep_path = dir.file("spdf_test.rep");

  WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
  WriteVsiText(hdr_path.c_str(), "nrows 4\nncols 4\nnbands 1\nnbits 8\n");

  // 1. UTM North SPDF rep file with stx, clr
  const std::string stx_path = dir.file("spdf_test.stx");
  const std::string clr_path = dir.file("spdf_test.clr");
  WriteVsiText(stx_path.c_str(), "1 0.0 255.0 128.0 10.0\n");
  WriteVsiText(clr_path.c_str(), "0 0 0 0\n");
  WriteVsiText(rep_path.c_str(),
               "PROJ_ID \"UTM\"\n"
               "PROJ_ZONE \"31 Nord\"\n"
               "HORIZ_DATUM \"WGS 84\"\n"
               "MAP_NUMBER \"MAP_123\"\n"
               "PRODUCTION_DATE 2026-01-01\n");

  {
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
    ASSERT_NE(nullptr, ds);

    const OGRSpatialReference* srs = ds->GetSpatialRef();
    ASSERT_NE(nullptr, srs);
    int bNorth = FALSE;
    EXPECT_EQ(31, srs->GetUTMZone(&bNorth));
    EXPECT_TRUE(bNorth);
    EXPECT_STREQ("MAP_123", ds->GetMetadataItem("SPDF_MAP_NUMBER"));
    EXPECT_STREQ("2026-01-01", ds->GetMetadataItem("SPDF_PRODUCTION_DATE"));

    // File list includes .rep, .stx, .clr
    char** file_list = ds->GetFileList();
    std::vector<std::string> files = CslToVector(file_list);
    CSLDestroy(file_list);
    EXPECT_THAT(files, ::testing::Contains(rep_path));
    EXPECT_THAT(files, ::testing::Contains(stx_path));
    EXPECT_THAT(files, ::testing::Contains(clr_path));
  }

  // 2. UTM South
  WriteVsiText(rep_path.c_str(),
               "PROJ_ID \"UTM\"\n"
               "PROJ_ZONE \"31 Sud\"\n"
               "HORIZ_DATUM \"WGS84\"\n");
  {
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    const OGRSpatialReference* srs = ds->GetSpatialRef();
    ASSERT_NE(nullptr, srs);
    int bNorth = TRUE;
    EXPECT_EQ(31, srs->GetUTMZone(&bNorth));
    EXPECT_FALSE(bNorth);
  }

  // 3. FR-MINDEF syntax
  WriteVsiText(rep_path.c_str(),
               "PROJ_ID \"UTM\"\n"
               "PROJ_CODE \"FR-MINDEF-A31N\"\n"
               "HORIZ_DATUM \"WGS84\"\n");
  {
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    const OGRSpatialReference* srs = ds->GetSpatialRef();
    ASSERT_NE(nullptr, srs);
    int bNorth = FALSE;
    EXPECT_EQ(31, srs->GetUTMZone(&bNorth));
    EXPECT_TRUE(bNorth);
  }

  // 4. Incomplete SPDF triggers warning
  WriteVsiText(rep_path.c_str(), "PROJ_ID \"UTM\"\nPROJ_ZONE \"31 Nord\"\n");
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Cannot retrieve projection from IMAGE.REP"));
    EXPECT_EQ(nullptr, ds->GetSpatialRef());
  }

  // 5. Upper directory search for IMAGE.REP
  {
    const std::string sub_bil = dir.file("subdir/imspatio.bil");
    const std::string upper_rep = dir.file("image.rep");
    WriteVsiBytes(sub_bil.c_str(), std::vector<GByte>(16, 0));
    WriteVsiText(upper_rep.c_str(), "dummy\n");
    EXPECT_EQ(upper_rep, EHdrDataset::GetImageRepFilename(sub_bil.c_str()));
    EXPECT_EQ(0, VSIUnlink(upper_rep.c_str()));

    // Search stopping at 'image' directory
    const std::string image_bil = dir.file("image/imspatio.bil");
    WriteVsiBytes(image_bil.c_str(), std::vector<GByte>(16, 0));
    EXPECT_STREQ("", EHdrDataset::GetImageRepFilename(image_bil.c_str()).c_str());

    // Relative path handling
    EXPECT_STREQ("", EHdrDataset::GetImageRepFilename("imspatio.bil").c_str());
  }
}

// -----------------------------------------------------------------------------
// Color Table (.clr) and Raster Attribute Table (.rat)
// -----------------------------------------------------------------------------

TEST_F(EhdrDatasetTest, ColorTableAndRatHandling) {
  ScopedVsiDir dir("color");
  const std::string data_path = dir.file("palette.bil");
  const std::string hdr_path = dir.file("palette.hdr");
  const std::string clr_path = dir.file("palette.clr");
  WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
  WriteVsiText(hdr_path.c_str(), "nrows 4\nncols 4\nnbands 1\nnbits 8\n");

  // .clr file with comments and entries
  WriteVsiText(clr_path.c_str(),
               "# Comment line\n"
               "! Another comment\n"
               "0 255 0 0\n"
               "1 0 255 0\n"
               "-1 0 0 255\n");  // Negative value creates RAT

  {
    GDALOpenInfo info(data_path.c_str(), GA_Update);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
    ASSERT_NE(nullptr, ds);

    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    EXPECT_EQ(GCI_PaletteIndex, band->GetColorInterpretation());

    GDALColorTable* ct = band->GetColorTable();
    ASSERT_NE(nullptr, ct);
    const GDALColorEntry* entry0 = ct->GetColorEntry(0);
    ASSERT_NE(nullptr, entry0);
    EXPECT_EQ(255, entry0->c1);

    GDALRasterAttributeTable* rat = band->GetDefaultRAT();
    ASSERT_NE(nullptr, rat);
    EXPECT_EQ(4, rat->GetColumnCount());

    // 1. Rewrite CLR via SetDefaultRAT (covers RewriteCLR's if (poRAT) branch)
    GDALDefaultRasterAttributeTable valid_rat;
    valid_rat.CreateColumn("Value", GFT_Integer, GFU_Generic);
    valid_rat.CreateColumn("Red", GFT_Integer, GFU_Red);
    valid_rat.CreateColumn("Green", GFT_Integer, GFU_Green);
    valid_rat.CreateColumn("Blue", GFT_Integer, GFU_Blue);
    valid_rat.SetValue(0, 0, -1);
    valid_rat.SetValue(0, 1, 11);
    valid_rat.SetValue(0, 2, 22);
    valid_rat.SetValue(0, 3, 33);
    EXPECT_EQ(CE_None, band->SetDefaultRAT(&valid_rat));
  }

  // 2. Verify rewritten RAT, then clear RAT and set new ColorTable
  {
    GDALOpenInfo info(data_path.c_str(), GA_Update);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
    ASSERT_NE(nullptr, ds);

    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    GDALRasterAttributeTable* rat = band->GetDefaultRAT();
    ASSERT_NE(nullptr, rat);
    EXPECT_EQ(11, rat->GetValueAsInt(0, 1));

    // Clear RAT so poTable is written by RewriteCLR
    EXPECT_EQ(CE_None, band->SetDefaultRAT(nullptr));
    GDALColorTable new_ct;
    GDALColorEntry e = {10, 20, 30, 255};
    new_ct.SetColorEntry(0, &e);
    EXPECT_EQ(CE_None, band->SetColorTable(&new_ct));
  }

  // 3. Verify rewritten .clr color table, test invalid RAT error, and unlink .clr
  {
    GDALOpenInfo info(data_path.c_str(), GA_Update);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
    ASSERT_NE(nullptr, ds);

    GDALRasterBand* band = ds->GetRasterBand(1);
    GDALColorTable* ct = band->GetColorTable();
    ASSERT_NE(nullptr, ct);
    const GDALColorEntry* entry0 = ct->GetColorEntry(0);
    ASSERT_NE(nullptr, entry0);
    EXPECT_EQ(10, entry0->c1);

    // Test SetDefaultRAT with invalid column count
    GDALDefaultRasterAttributeTable bad_rat;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, band->SetDefaultRAT(&bad_rat));

    // Clear color table and RAT
    EXPECT_EQ(CE_None, band->SetColorTable(nullptr));
    EXPECT_EQ(CE_None, band->SetDefaultRAT(nullptr));
  }

  VSIStatBufL sStat;
  EXPECT_NE(0, VSIStatL(clr_path.c_str(), &sStat));
}

// -----------------------------------------------------------------------------
// Statistics (.stx) and Metadata
// -----------------------------------------------------------------------------

TEST_F(EhdrDatasetTest, StatisticsStxReadAndWrite) {
  ScopedVsiDir dir("stats");
  const std::string data_path = dir.file("stats.bil");
  const std::string hdr_path = dir.file("stats.hdr");
  const std::string stx_path = dir.file("stats.stx");

  WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
  WriteVsiText(hdr_path.c_str(),
               "nrows 4\nncols 4\nnbands 1\nnbits 8\n"
               "NODATA 255\n");

  // Band 1: min max mean stddev stretchmin stretchmax
  WriteVsiText(stx_path.c_str(), "1 10.0 50.0 30.0 5.0 12.0 48.0\n");

  {
    GDALOpenInfo info(data_path.c_str(), GA_Update);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
    ASSERT_NE(nullptr, ds);

    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    int bSuccess = 0;
    EXPECT_DOUBLE_EQ(10.0, band->GetMinimum(&bSuccess));
    EXPECT_TRUE(bSuccess);
    EXPECT_DOUBLE_EQ(50.0, band->GetMaximum(&bSuccess));
    EXPECT_TRUE(bSuccess);

    double min = 0, max = 0, mean = 0, stddev = 0;
    EXPECT_EQ(CE_None,
              band->GetStatistics(FALSE, FALSE, &min, &max, &mean, &stddev));
    EXPECT_DOUBLE_EQ(10.0, min);
    EXPECT_DOUBLE_EQ(50.0, max);
    EXPECT_DOUBLE_EQ(30.0, mean);
    EXPECT_DOUBLE_EQ(5.0, stddev);

    EXPECT_STREQ("12.0", band->GetMetadataItem("STRETCHMIN", "RENDERING_HINTS"));
    EXPECT_STREQ("48.0", band->GetMetadataItem("STRETCHMAX", "RENDERING_HINTS"));

    // Set new statistics
    EXPECT_EQ(CE_None, band->SetStatistics(5.0, 60.0, 32.0, 6.0));
    // Calling SetStatistics with identical values returns CE_None early
    EXPECT_EQ(CE_None, band->SetStatistics(5.0, 60.0, 32.0, 6.0));

    // Test SetStatistics when STATISTICS_MINIMUM is set in metadata
    band->SetMetadataItem("STATISTICS_MINIMUM", "5.0");
    EXPECT_EQ(CE_None, band->SetStatistics(4.0, 61.0, 32.0, 6.0));
    EXPECT_EQ(nullptr, band->GetMetadataItem("STATISTICS_MINIMUM"));

    // Test SetStatistics when STATISTICS_APPROXIMATE is set:
    // delegates to RawRasterBand::SetStatistics without rewriting .stx
    band->SetMetadataItem("STATISTICS_APPROXIMATE", "YES");
    EXPECT_EQ(CE_None, band->SetStatistics(3.0, 62.0, 32.0, 6.0));
    EXPECT_STREQ("3", band->GetMetadataItem("STATISTICS_MINIMUM"));
    EXPECT_STREQ("62", band->GetMetadataItem("STATISTICS_MAXIMUM"));
  }

  // Verify rewritten statistics in .stx (remains 4.0 and 61.0 since approximate stats skip RewriteSTX)
  {
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    int bSuccess = 0;
    EXPECT_DOUBLE_EQ(4.0, band->GetMinimum(&bSuccess));
    EXPECT_DOUBLE_EQ(61.0, band->GetMaximum(&bSuccess));
  }

  // Min == NoData in .stx -> ignored
  WriteVsiText(stx_path.c_str(), "1 255.0 255.0 255.0 0.0\n");
  {
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    int bSuccess = 0;
    band->GetMinimum(&bSuccess);
    EXPECT_FALSE(bSuccess);
  }

  // Test GetStatistics on a dataset without an initial .stx file
  {
    const std::string nostx_data = dir.file("nostx.bil");
    const std::string nostx_hdr = dir.file("nostx.hdr");
    WriteVsiBytes(nostx_data.c_str(), std::vector<GByte>(16, 7));
    WriteVsiText(nostx_hdr.c_str(), "nrows 4\nncols 4\nnbands 1\nnbits 8\n");

    GDALOpenInfo info(nostx_data.c_str(), GA_Update);
    std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    double min = 0, max = 0, mean = 0, stddev = 0;
    EXPECT_EQ(CE_None,
              band->GetStatistics(FALSE, TRUE, &min, &max, &mean, &stddev));
    EXPECT_DOUBLE_EQ(7.0, min);
    EXPECT_DOUBLE_EQ(7.0, max);
    // Cached stats return CE_None immediately
    EXPECT_EQ(CE_None,
              band->GetStatistics(FALSE, FALSE, &min, &max, &mean, &stddev));
  }
}

TEST_F(EhdrDatasetTest, MinValueMaxValueInHdr) {
  ScopedVsiDir dir("minmax_hdr");
  const std::string data_path = dir.file("minmax.bil");
  const std::string hdr_path = dir.file("minmax.hdr");
  WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
  WriteVsiText(hdr_path.c_str(),
               "nrows 4\nncols 4\nnbands 1\nnbits 8\n"
               "MinValue 1.5\nMaxValue 9.5\n");

  GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
  std::unique_ptr<GDALDataset> ds(EHdrDataset::Open(&info));
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  int bSuccess = 0;
  EXPECT_DOUBLE_EQ(1.5, band->GetMinimum(&bSuccess));
  EXPECT_TRUE(bSuccess);
  EXPECT_DOUBLE_EQ(9.5, band->GetMaximum(&bSuccess));
  EXPECT_TRUE(bSuccess);
}

// -----------------------------------------------------------------------------
// Create and CreateCopy
// -----------------------------------------------------------------------------

TEST_F(EhdrDatasetTest, CreateDatasetSuccessAndErrors) {
  ScopedVsiDir dir("create");
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("EHdr");
  ASSERT_NE(nullptr, drv);

  // 1. Errors on invalid parameters
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, drv->Create(dir.file("bad1.bil").c_str(), 10, 10, 0,
                                   GDT_Byte, nullptr));
    EXPECT_EQ(nullptr, drv->Create(dir.file("bad2.bil").c_str(), 10, 10, 1,
                                   GDT_Float64, nullptr));
    const char* const nbits_zero[] = {"NBITS=0", nullptr};
    EXPECT_EQ(nullptr, drv->Create(dir.file("bad3.bil").c_str(), 10, 10, 1,
                                   GDT_Byte, const_cast<char**>(nbits_zero)));
    EXPECT_EQ(nullptr, drv->Create(dir.file("bad4.bil").c_str(), 0, 10, 1,
                                   GDT_Byte, nullptr));
    EXPECT_EQ(nullptr, drv->Create(dir.file("bad5.bil").c_str(), 10, -1, 1,
                                   GDT_Byte, nullptr));
    EXPECT_EQ(nullptr, drv->Create("", 5, 5, 1, GDT_Byte, nullptr));
  }

  // 2. Successful creation for various types
  const GDALDataType kTypes[] = {GDT_Byte,   GDT_Int8,  GDT_Int16,
                                 GDT_UInt16, GDT_Int32, GDT_UInt32,
                                 GDT_Float32};
  for (GDALDataType type : kTypes) {
    const std::string name =
        std::format("create_{}.bil", GDALGetDataTypeName(type));
    std::unique_ptr<GDALDataset> ds(
        drv->Create(dir.file(name).c_str(), 5, 5, 1, type, nullptr));
    ASSERT_NE(nullptr, ds) << GDALGetDataTypeName(type);
    EXPECT_EQ(5, ds->GetRasterXSize());
    EXPECT_EQ(5, ds->GetRasterYSize());
    EXPECT_EQ(type, ds->GetRasterBand(1)->GetRasterDataType());
  }

  // 3. Signed byte creation via PIXELTYPE=SIGNEDBYTE
  const char* const signed_byte_opt[] = {"PIXELTYPE=SIGNEDBYTE", nullptr};
  {
    std::unique_ptr<GDALDataset> ds(
        drv->Create(dir.file("signed_byte.bil").c_str(), 5, 5, 1, GDT_Byte,
                    const_cast<char**>(signed_byte_opt)));
    ASSERT_NE(nullptr, ds);
  }
}

TEST_F(EhdrDatasetTest, CreateCopyRoundTrip) {
  ScopedVsiDir dir("create_copy");
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("EHdr");
  ASSERT_NE(nullptr, drv);

  // Source EHdr dataset
  std::unique_ptr<GDALDataset> src(
      drv->Create(dir.file("src.bil").c_str(), 10, 10, 2, GDT_Int16, nullptr));
  ASSERT_NE(nullptr, src);

  GDALGeoTransform gt = {10.0, 1.0, 0.0, 20.0, 0.0, -1.0};
  src->SetGeoTransform(gt);
  OGRSpatialReference srs;
  srs.SetWellKnownGeogCS("WGS84");
  src->SetSpatialRef(&srs);

  std::vector<int16_t> line(10, 42);
  EXPECT_EQ(CE_None, src->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, 10, 1, line.data(), 10, 1, GDT_Int16,
                         0, 0, nullptr));

  // CreateCopy to EHdr
  const std::string dst_path = dir.file("copy.bil");
  std::unique_ptr<GDALDataset> dst(
      drv->CreateCopy(dst_path.c_str(), src.get(), FALSE, nullptr, nullptr,
                      nullptr));
  ASSERT_NE(nullptr, dst);
  dst.reset();  // Flush to disk

  // Verify reopened dataset
  GDALOpenInfo info(dst_path.c_str(), GA_ReadOnly);
  std::unique_ptr<GDALDataset> reopen(EHdrDataset::Open(&info));
  ASSERT_NE(nullptr, reopen);
  EXPECT_EQ(10, reopen->GetRasterXSize());
  EXPECT_EQ(10, reopen->GetRasterYSize());
  EXPECT_EQ(2, reopen->GetRasterCount());
  ASSERT_NE(nullptr, reopen->GetSpatialRef());
  EXPECT_THAT(*reopen->GetSpatialRef(), IsSameAs(srs));

  std::vector<int16_t> read_line(10);
  EXPECT_EQ(CE_None, reopen->GetRasterBand(1)->RasterIO(
                         GF_Read, 0, 0, 10, 1, read_line.data(), 10, 1,
                         GDT_Int16, 0, 0, nullptr));
  EXPECT_EQ(42, read_line[0]);

  // CreateCopy passing NBITS and PIXELTYPE from GDT_Byte dataset
  {
    std::unique_ptr<GDALDataset> byte_src(drv->Create(
        dir.file("byte_src.bil").c_str(), 5, 5, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, byte_src);
    byte_src->GetRasterBand(1)->SetMetadataItem("NBITS", "4",
                                                "IMAGE_STRUCTURE");
    byte_src->GetRasterBand(1)->SetMetadataItem("PIXELTYPE", "SIGNEDBYTE",
                                                "IMAGE_STRUCTURE");
    std::unique_ptr<GDALDataset> byte_copy(
        drv->CreateCopy(dir.file("byte_copy.bil").c_str(), byte_src.get(),
                        FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, byte_copy);
  }

  // 0-band source rejection
  EHdrDataset empty_src;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(nullptr,
            drv->CreateCopy(dir.file("bad_copy.bil").c_str(), &empty_src, FALSE,
                            nullptr, nullptr, nullptr));
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover remaining I/O error branches.
