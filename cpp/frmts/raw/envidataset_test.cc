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
// Tests for the ENVI .hdr labelled raw raster format driver.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/raw/envidataset.cpp
//   https://gdal.org/drivers/raster/envi.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/envi.py

#include "third_party/gdal/frmts/raw/envidataset.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/matchers.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::autotest2::IsSameAs;
using ::testing::ElementsAre;
using ::testing::HasSubstr;

void WriteVsiBytes(const char* path, const std::vector<GByte>& data) {
  VSILFILE* fp = VSIFOpenL(path, "wb");
  ASSERT_NE(nullptr, fp);
  if (!data.empty()) {
    EXPECT_EQ(data.size(), VSIFWriteL(data.data(), 1, data.size(), fp));
  }
  EXPECT_EQ(0, VSIFCloseL(fp));
}

void WriteVsiText(const char* path, std::string_view text) {
  VSILFILE* fp = VSIFOpenL(path, "wt");
  ASSERT_NE(nullptr, fp);
  if (!text.empty()) {
    EXPECT_EQ(text.size(), VSIFWriteL(text.data(), 1, text.size(), fp));
  }
  EXPECT_EQ(0, VSIFCloseL(fp));
}

class VsiDir {
 public:
  explicit VsiDir(std::string_view base_name)
      : dir_(std::format("/vsimem/envidataset_test/{}", base_name)) {}

  ~VsiDir() { VSIRmdirRecursive(dir_.c_str()); }

  std::string file(std::string_view filename) const {
    return std::format("{}/{}", dir_, filename);
  }

  const std::string& path() const { return dir_; }

 private:
  std::string dir_;
};

class EnviDatasetTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_ENVI(); }
};

// -----------------------------------------------------------------------------
// Driver Registration & Driver Metadata
// -----------------------------------------------------------------------------

TEST_F(EnviDatasetTest, DriverRegistrationAndMetadata) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("ENVI");
  ASSERT_NE(nullptr, drv);

  EXPECT_STREQ("ENVI", drv->GetDescription());
  EXPECT_STREQ("YES", drv->GetMetadataItem(GDAL_DCAP_RASTER));
  EXPECT_STREQ("YES", drv->GetMetadataItem(GDAL_DCAP_UPDATE));
  EXPECT_STREQ("YES", drv->GetMetadataItem(GDAL_DCAP_VIRTUALIO));
  EXPECT_STREQ("ENVI .hdr Labelled",
               drv->GetMetadataItem(GDAL_DMD_LONGNAME));

  // Early return branch when already registered
  GDALRegister_ENVI();
}

// -----------------------------------------------------------------------------
// Open: Error Handling, Header Parsing, and Validation Edge Cases
// -----------------------------------------------------------------------------

TEST_F(EnviDatasetTest, IdentifyDoesNotExistAndInvalidOpen) {
  const VsiDir dir("invalid_open");

  // 1. Non-existent file
  {
    GDALOpenInfo info("/vsimem/envidataset_test/nonexistent.dat", GA_ReadOnly);
    EXPECT_EQ(nullptr, ENVIDataset::Open(&info));
  }

  // 2. File too short (< 2 bytes)
  {
    const std::string data_path = dir.file("short.dat");
    WriteVsiBytes(data_path.c_str(), {0x01});
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    EXPECT_EQ(nullptr, ENVIDataset::Open(&info));
  }

  // 3. .zarr extension rejection (unless single allowed driver)
  {
    const std::string zarr_path = dir.file("test.zarr");
    const std::string zarr_hdr = dir.file("test.zarr.hdr");
    WriteVsiBytes(zarr_path.c_str(), {0x01, 0x02});
    WriteVsiText(zarr_hdr.c_str(),
                 "ENVI\nsamples = 1\nlines = 2\nbands = 1\ndata type = 1\n");
    GDALOpenInfo info(zarr_path.c_str(), GA_ReadOnly);
    EXPECT_EQ(nullptr, ENVIDataset::Open(&info));

    const char* const allowed_drivers[] = {"ENVI", nullptr};
    std::unique_ptr<GDALDataset> ds(
        static_cast<GDALDataset*>(GDALOpenEx(zarr_path.c_str(),
                                             GDAL_OF_RASTER | GDAL_OF_READONLY,
                                             allowed_drivers, nullptr, nullptr)));
    EXPECT_NE(nullptr, ds);
  }

  // 4. Opening .hdr directly produces an explanatory error
  {
    const std::string hdr_path = dir.file("direct.hdr");
    WriteVsiText(hdr_path.c_str(), "ENVI\nsamples = 2\nlines = 2\nbands = 1\n");
    GDALOpenInfo info(hdr_path.c_str(), GA_ReadOnly);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ENVIDataset::Open(&info));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("ENVI header file"));
  }

  // 5. Opening .sta directly produces an explanatory error
  {
    const std::string sta_path = dir.file("direct.sta");
    WriteVsiBytes(sta_path.c_str(), std::vector<GByte>(64, 0));
    const std::string hdr_path = dir.file("direct.hdr");
    WriteVsiText(hdr_path.c_str(), "ENVI\nsamples = 2\nlines = 2\nbands = 1\n");
    GDALOpenInfo info(sta_path.c_str(), GA_ReadOnly);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ENVIDataset::Open(&info));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("ENVI statistics file"));
  }

  // 6. Header without "ENVI" magic word
  {
    const std::string data_path = dir.file("not_envi.dat");
    const std::string hdr_path = dir.file("not_envi.hdr");
    WriteVsiBytes(data_path.c_str(), {0x00, 0x00});
    WriteVsiText(hdr_path.c_str(), "NOPE\nsamples = 1\nlines = 1\nbands = 1\n");
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    EXPECT_EQ(nullptr, ENVIDataset::Open(&info));
  }

  // 7. Header too short (< 4 bytes)
  {
    const std::string data_path = dir.file("short_hdr.dat");
    const std::string hdr_path = dir.file("short_hdr.hdr");
    WriteVsiBytes(data_path.c_str(), {0x00, 0x00});
    WriteVsiText(hdr_path.c_str(), "ENV");
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    EXPECT_EQ(nullptr, ENVIDataset::Open(&info));
  }

  // 8. Missing required dimensions (samples, lines, bands)
  {
    const std::string data_path = dir.file("missing_dims.dat");
    const std::string hdr_path = dir.file("missing_dims.hdr");
    WriteVsiBytes(data_path.c_str(), {0x00, 0x00});
    WriteVsiText(hdr_path.c_str(), "ENVI\nfile type = ENVI Standard\n");
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ENVIDataset::Open(&info));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("keywords appears to be missing"));
  }

  // 9. Dimension overflow & clamping:
  // lines > INT_MAX emits warning and clamps
  {
    const std::string data_path = dir.file("lines_overflow.dat");
    const std::string hdr_path = dir.file("lines_overflow.hdr");
    WriteVsiBytes(data_path.c_str(), {0x00, 0x00});
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 1\nlines = 3000000000\nbands = 1\n");
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(std::numeric_limits<int>::max(), ds->GetRasterYSize());
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Limiting number of lines"));
  }

  // samples > INT_MAX fails
  {
    const std::string data_path = dir.file("samples_overflow.dat");
    const std::string hdr_path = dir.file("samples_overflow.hdr");
    WriteVsiBytes(data_path.c_str(), {0x00, 0x00});
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 3000000000\nlines = 1\nbands = 1\n");
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ENVIDataset::Open(&info));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot handle samples="));
  }

  // bands > INT_MAX fails
  {
    const std::string data_path = dir.file("bands_overflow.dat");
    const std::string hdr_path = dir.file("bands_overflow.hdr");
    WriteVsiBytes(data_path.c_str(), {0x00, 0x00});
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 1\nlines = 1\nbands = 3000000000\n");
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ENVIDataset::Open(&info));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot handle bands="));
  }

  // 10. Unsupported file types: "envi meta file", "envi virtual mosaic", "envi spectral library"
  {
    constexpr std::string_view kUnsupportedTypes[] = {
        "envi meta file", "envi virtual mosaic", "envi spectral library"};
    for (const auto& ftype : kUnsupportedTypes) {
      const std::string base = std::format("unsupported_{}", ftype);
      std::string safe_name = base;
      for (char& c : safe_name) {
        if (c == ' ') c = '_';
      }
      const std::string data_path = dir.file(safe_name + ".dat");
      const std::string hdr_path = dir.file(safe_name + ".hdr");
      WriteVsiBytes(data_path.c_str(), {0x00, 0x00});
      WriteVsiText(hdr_path.c_str(),
                   std::format("ENVI\nsamples = 1\nlines = 1\nbands = 1\nfile type = {}\n",
                               ftype));
      GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(nullptr, ENVIDataset::Open(&info));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("invalid file type"));
    }
  }

  // 11. Unrecognised data type
  {
    const std::string data_path = dir.file("bad_dtype.dat");
    const std::string hdr_path = dir.file("bad_dtype.hdr");
    WriteVsiBytes(data_path.c_str(), {0x00, 0x00});
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 1\nlines = 1\nbands = 1\ndata type = 99\n");
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ENVIDataset::Open(&info));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("recognised by the GDAL ENVI driver"));
  }

  // 12. ReadHeader with spaces in key, lines without '=', multiline values, and comments
  {
    const std::string data_path = dir.file("header_parsing.dat");
    const std::string hdr_path = dir.file("header_parsing.hdr");
    WriteVsiBytes(data_path.c_str(), {0x00, 0x00, 0x00, 0x00});
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\n"
                 "; Comment line\n"
                 "   leading spaces key   =   value with spaces   \n"
                 "multiline key = {\n"
                 "  part1,\n"
                 "  part2\n"
                 "}\n"
                 "samples = 2\n"
                 "lines = 2\n"
                 "bands = 1\n"
                 "data type = 1\n");
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    EXPECT_STREQ("value with spaces",
                 ds->GetMetadataItem("leading_spaces_key", "ENVI"));
  }
}

// -----------------------------------------------------------------------------
// Header Extension and Sibling File Discovery
// -----------------------------------------------------------------------------

TEST_F(EnviDatasetTest, HeaderExtensionAndSiblingDiscovery) {
  const VsiDir dir("hdr_discovery");

  // 1. Appended .hdr
  {
    const std::string data_path = dir.file("appended.bin");
    const std::string hdr_path = dir.file("appended.bin.hdr");
    WriteVsiBytes(data_path.c_str(), {0x01, 0x02});
    WriteVsiText(hdr_path.c_str(), "ENVI\nsamples = 1\nlines = 2\nbands = 1\n");
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(1, ds->GetRasterXSize());
    EXPECT_EQ(2, ds->GetRasterYSize());
  }

  // 2. Replaced .hdr
  {
    const std::string data_path = dir.file("replaced.bin");
    const std::string hdr_path = dir.file("replaced.hdr");
    WriteVsiBytes(data_path.c_str(), {0x01, 0x02});
    WriteVsiText(hdr_path.c_str(), "ENVI\nsamples = 2\nlines = 1\nbands = 1\n");
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(2, ds->GetRasterXSize());
    EXPECT_EQ(1, ds->GetRasterYSize());
  }

  // 3. Appended .HDR (uppercase)
  {
    const std::string data_path = dir.file("upper_app.bin");
    const std::string hdr_path = dir.file("upper_app.bin.HDR");
    WriteVsiBytes(data_path.c_str(), {0x01, 0x02});
    WriteVsiText(hdr_path.c_str(), "ENVI\nsamples = 1\nlines = 2\nbands = 1\n");
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(1, ds->GetRasterXSize());
  }

  // 4. Replaced .HDR (uppercase)
  {
    const std::string data_path = dir.file("upper_rep.bin");
    const std::string hdr_path = dir.file("upper_rep.HDR");
    WriteVsiBytes(data_path.c_str(), {0x01, 0x02});
    WriteVsiText(hdr_path.c_str(), "ENVI\nsamples = 1\nlines = 2\nbands = 1\n");
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(1, ds->GetRasterXSize());
  }

  // 5. With SiblingFiles list
  {
    const std::string data_path = dir.file("sibling_test.bin");
    const std::string hdr_path = dir.file("sibling_test.hdr");
    WriteVsiBytes(data_path.c_str(), {0x01, 0x02});
    WriteVsiText(hdr_path.c_str(), "ENVI\nsamples = 1\nlines = 2\nbands = 1\n");

    const char* sibling_files[] = {"sibling_test.bin", "sibling_test.hdr", nullptr};
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly, sibling_files);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
  }

  // 6. With SiblingFiles list (appended header)
  {
    const std::string data_path = dir.file("sibling_app.bin");
    const std::string hdr_path = dir.file("sibling_app.bin.hdr");
    WriteVsiBytes(data_path.c_str(), {0x01, 0x02});
    WriteVsiText(hdr_path.c_str(), "ENVI\nsamples = 1\nlines = 2\nbands = 1\n");

    const char* sibling_files[] = {"sibling_app.bin", "sibling_app.bin.hdr", nullptr};
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly, sibling_files);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
  }
}

// -----------------------------------------------------------------------------
// All 11 Supported Data Types and Endianness
// -----------------------------------------------------------------------------

TEST_F(EnviDatasetTest, AllDataTypesAndByteOrders) {
  const VsiDir dir("data_types");

  struct DataTypeCase {
    int envi_type;
    GDALDataType expected_gdt;
    int bytes_per_pixel;
  };

  constexpr DataTypeCase kCases[] = {
      {.envi_type = 1, .expected_gdt = GDT_Byte, .bytes_per_pixel = 1},
      {.envi_type = 2, .expected_gdt = GDT_Int16, .bytes_per_pixel = 2},
      {.envi_type = 3, .expected_gdt = GDT_Int32, .bytes_per_pixel = 4},
      {.envi_type = 4, .expected_gdt = GDT_Float32, .bytes_per_pixel = 4},
      {.envi_type = 5, .expected_gdt = GDT_Float64, .bytes_per_pixel = 8},
      {.envi_type = 6, .expected_gdt = GDT_CFloat32, .bytes_per_pixel = 8},
      {.envi_type = 9, .expected_gdt = GDT_CFloat64, .bytes_per_pixel = 16},
      {.envi_type = 12, .expected_gdt = GDT_UInt16, .bytes_per_pixel = 2},
      {.envi_type = 13, .expected_gdt = GDT_UInt32, .bytes_per_pixel = 4},
      {.envi_type = 14, .expected_gdt = GDT_Int64, .bytes_per_pixel = 8},
      {.envi_type = 15, .expected_gdt = GDT_UInt64, .bytes_per_pixel = 8},
  };

  for (const auto& c : kCases) {
    for (int byte_order : {0, 1}) {
      const std::string name =
          std::format("type_{}_bo_{}", c.envi_type, byte_order);
      const std::string data_path = dir.file(name + ".bin");
      const std::string hdr_path = dir.file(name + ".hdr");

      const size_t file_size = static_cast<size_t>(c.bytes_per_pixel) * 4;
      WriteVsiBytes(data_path.c_str(), std::vector<GByte>(file_size, 0x42));
      WriteVsiText(
          hdr_path.c_str(),
          std::format(
              "ENVI\nsamples = 2\nlines = 2\nbands = 1\ndata type = {}\nbyte order = {}\n",
              c.envi_type, byte_order));

      GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
      std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
      ASSERT_NE(nullptr, ds) << "Failed for ENVI type " << c.envi_type;
      ASSERT_EQ(1, ds->GetRasterCount());
      EXPECT_EQ(c.expected_gdt, ds->GetRasterBand(1)->GetRasterDataType());

      // Test Create() with this data type
      GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("ENVI");
      ASSERT_NE(nullptr, drv);
      const std::string create_path = dir.file("create_" + name + ".bin");
      GDALDatasetUniquePtr created_ds(
          drv->Create(create_path.c_str(), 2, 2, 1, c.expected_gdt, nullptr));
      ASSERT_NE(nullptr, created_ds);
      created_ds.reset();

      // Reopen created dataset to verify
      GDALOpenInfo created_info(create_path.c_str(), GA_ReadOnly);
      std::unique_ptr<GDALDataset> reopened(ENVIDataset::Open(&created_info));
      ASSERT_NE(nullptr, reopened);
      EXPECT_EQ(c.expected_gdt, reopened->GetRasterBand(1)->GetRasterDataType());
    }
  }

  // Create with unsupported data type (e.g. GDT_CInt16) emits error
  {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("ENVI");
    ASSERT_NE(nullptr, drv);
    WithQuietHandler quiet;
    CPLErrorReset();
    const std::string bad_create = dir.file("bad_create.bin");
    GDALDatasetUniquePtr created_ds(
        drv->Create(bad_create.c_str(), 2, 2, 1, GDT_CInt16, nullptr));
    EXPECT_NE(nullptr, created_ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("illegal data type"));
  }
}

// -----------------------------------------------------------------------------
// Interleaves, Major Frame Offsets, Layout and Compression
// -----------------------------------------------------------------------------

TEST_F(EnviDatasetTest, InterleavesMajorFrameOffsetsAndCompression) {
  const VsiDir dir("interleaves");

  // 1. BIL, BIP, BSQ, and unknown fallback
  constexpr struct {
    const char* interleave_str;
    const char* expected_structure;
  } kInterleaveCases[] = {
      {"bil", "LINE"},
      {"bip", "PIXEL"},
      {"bsq", "BAND"},
      {"unknown_interleave", "BAND"},
  };

  for (const auto& c : kInterleaveCases) {
    const std::string name = std::format("int_{}", c.interleave_str);
    const std::string data_path = dir.file(name + ".bin");
    const std::string hdr_path = dir.file(name + ".hdr");

    WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0x11));
    WriteVsiText(
        hdr_path.c_str(),
        std::format("ENVI\nsamples = 2\nlines = 2\nbands = 2\ndata type = 1\ninterleave = {}\n",
                    c.interleave_str));

    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    EXPECT_STREQ(c.expected_structure,
                 ds->GetMetadataItem("INTERLEAVE", "IMAGE_STRUCTURE"));

    GDALDataset::RawBinaryLayout layout;
    EXPECT_TRUE(ds->GetRawBinaryLayout(layout));
    EXPECT_STREQ(ds->GetDescription(), layout.osRawFilename.c_str());
  }

  // 2. Integer overflow in offset calculation (tested without file size check)
  {
    const std::string data_path = dir.file("overflow_bil.bin");
    const std::string hdr_path = dir.file("overflow_bil.hdr");
    WriteVsiBytes(data_path.c_str(), {0x00, 0x00});
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 1000000000\nlines = 1\nbands = 10\ndata type = 4\ninterleave = bil\n");

    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ENVIDataset::Open(&info, /*bFileSizeCheck=*/false));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Int overflow occurred"));
  }
  {
    const std::string data_path = dir.file("overflow_bip.bin");
    const std::string hdr_path = dir.file("overflow_bip.hdr");
    WriteVsiBytes(data_path.c_str(), {0x00, 0x00});
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 1000000000\nlines = 1\nbands = 10\ndata type = 4\ninterleave = bip\n");

    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ENVIDataset::Open(&info, /*bFileSizeCheck=*/false));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Int overflow occurred"));
  }
  {
    const std::string data_path = dir.file("overflow_bsq.bin");
    const std::string hdr_path = dir.file("overflow_bsq.hdr");
    WriteVsiBytes(data_path.c_str(), {0x00, 0x00});
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 1000000000\nlines = 1\nbands = 1\ndata type = 4\ninterleave = bsq\n");

    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ENVIDataset::Open(&info, /*bFileSizeCheck=*/false));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Int overflow occurred"));
  }

  // 3. Major frame offsets (both valid 2-element list and 1-element list)
  {
    const std::string data_path = dir.file("mfo_2.bin");
    const std::string hdr_path = dir.file("mfo_2.hdr");
    std::vector<GByte> raw_bytes(64, 0);
    raw_bytes[4] = 10;
    raw_bytes[5] = 20;
    raw_bytes[14] = 30;
    raw_bytes[15] = 40;
    WriteVsiBytes(data_path.c_str(), raw_bytes);
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 2\nlines = 2\nbands = 1\ndata type = 1\n"
                 "major frame offsets = {4, 4}\n");

    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    std::vector<GByte> read_buf(4, 0);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 2, 2, read_buf.data(), 2, 2,
                           GDT_Byte, 0, 0, nullptr));
    EXPECT_THAT(read_buf, ElementsAre(10, 20, 30, 40));
  }
  {
    const std::string data_path = dir.file("mfo_1.bin");
    const std::string hdr_path = dir.file("mfo_1.hdr");
    WriteVsiBytes(data_path.c_str(), std::vector<GByte>(64, 0));
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 2\nlines = 2\nbands = 1\ndata type = 1\n"
                 "major frame offsets = {4}\n");

    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
  }

  // 4. Compressed files (file_compression = 1)
  {
    const std::string data_path = dir.file("compressed.bin");
    const std::string hdr_path = dir.file("compressed.hdr");
    WriteVsiBytes(data_path.c_str(), {0x00, 0x00});
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 1\nlines = 1\nbands = 1\nfile compression = 1\n");

    // In update mode, compressed file is rejected
    GDALOpenInfo info_update(data_path.c_str(), GA_Update);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ENVIDataset::Open(&info_update));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot open compressed file in update mode"));
  }
}

// -----------------------------------------------------------------------------
// Band Metadata, Wavelengths, Classes, and Color Tables
// -----------------------------------------------------------------------------

TEST_F(EnviDatasetTest, BandMetadataWavelengthsClassesAndColorTable) {
  const VsiDir dir("metadata_classes");

  // 1. Wavelengths, FWHM, and Units
  constexpr struct {
    const char* unit;
    const char* expected_wl;
    const char* expected_fwhm;
  } kWLUnits[] = {
      {"Micrometers", "0.550", "0.020"},
      {"um", "0.550", "0.020"},
      {"Nanometers", "0.001", "0.000"},
      {"nm", "0.001", "0.000"},
      {"Millimeters", "550.000", "20.000"},
      {"mm", "550.000", "20.000"},
      {"Unknown", nullptr, nullptr},
      {"Index", nullptr, nullptr},
      {"Custom", nullptr, nullptr},
  };

  for (const auto& u : kWLUnits) {
    const std::string name = std::format("wl_{}", u.unit);
    const std::string data_path = dir.file(name + ".bin");
    const std::string hdr_path = dir.file(name + ".hdr");

    WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
    WriteVsiText(
        hdr_path.c_str(),
        std::format("ENVI\nsamples = 2\nlines = 2\nbands = 2\ndata type = 1\n"
                    "band names = {{Band Red, Band Green}}\n"
                    "wavelength = {{0.55, 0.65}}\n"
                    "fwhm = {{0.02, 0.03}}\n"
                    "wavelength units = {}\n",
                    u.unit));

    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);

    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    EXPECT_STREQ("0.55", band->GetMetadataItem("wavelength"));

    if (u.expected_wl) {
      EXPECT_STREQ(u.expected_wl,
                   band->GetMetadataItem("CENTRAL_WAVELENGTH_UM", "IMAGERY"));
      EXPECT_STREQ(u.expected_fwhm,
                   band->GetMetadataItem("FWHM_UM", "IMAGERY"));
    }
  }

  // 2. Wavelength without band names
  {
    const std::string data_path = dir.file("wl_no_names.bin");
    const std::string hdr_path = dir.file("wl_no_names.hdr");
    WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 2\nlines = 2\nbands = 1\ndata type = 1\n"
                 "wavelength = {0.55}\n"
                 "wavelength units = Micrometers\n");

    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    EXPECT_STREQ("0.55 Micrometers", ds->GetRasterBand(1)->GetDescription());
  }

  // 3. Default bands RGB and Gray
  {
    const std::string data_path = dir.file("default_rgb.bin");
    const std::string hdr_path = dir.file("default_rgb.hdr");
    WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 2\nlines = 2\nbands = 3\ndata type = 1\n"
                 "default bands = {3, 2, 1}\n");

    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(GCI_RedBand, ds->GetRasterBand(3)->GetColorInterpretation());
    EXPECT_EQ(GCI_GreenBand, ds->GetRasterBand(2)->GetColorInterpretation());
    EXPECT_EQ(GCI_BlueBand, ds->GetRasterBand(1)->GetColorInterpretation());
  }
  {
    const std::string data_path = dir.file("default_gray.bin");
    const std::string hdr_path = dir.file("default_gray.hdr");
    WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 2\nlines = 2\nbands = 2\ndata type = 1\n"
                 "default bands = {2}\n");

    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(GCI_GrayIndex, ds->GetRasterBand(2)->GetColorInterpretation());
  }

  // 4. Data offset, gain, and ignore values
  {
    const std::string data_path = dir.file("offset_gain_nodata.bin");
    const std::string hdr_path = dir.file("offset_gain_nodata.hdr");
    WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 2\nlines = 2\nbands = 2\ndata type = 1\n"
                 "data offset values = {10.5, 20.5}\n"
                 "data gain values = {2.0, 3.0}\n"
                 "data ignore value = -9999\n");

    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);

    int has_val = 0;
    EXPECT_DOUBLE_EQ(10.5, ds->GetRasterBand(1)->GetOffset(&has_val));
    EXPECT_TRUE(has_val);
    EXPECT_DOUBLE_EQ(2.0, ds->GetRasterBand(1)->GetScale(&has_val));
    EXPECT_TRUE(has_val);
    EXPECT_DOUBLE_EQ(-9999.0, ds->GetRasterBand(1)->GetNoDataValue(&has_val));
    EXPECT_TRUE(has_val);
    EXPECT_DOUBLE_EQ(-9999.0, ds->GetRasterBand(2)->GetNoDataValue(&has_val));
    EXPECT_TRUE(has_val);
  }

  // 5. Classes and Colormap
  {
    const std::string data_path = dir.file("classes_colormap.bin");
    const std::string hdr_path = dir.file("classes_colormap.hdr");
    WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 2\nlines = 2\nbands = 1\ndata type = 1\n"
                 "classes = 2\n"
                 "class names = {Water, Land}\n"
                 "class lookup = {0, 0, 255, 0, 255, 0}\n");

    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);

    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    EXPECT_EQ(GCI_PaletteIndex, band->GetColorInterpretation());

    char** cats = band->GetCategoryNames();
    ASSERT_NE(nullptr, cats);
    EXPECT_EQ(2, CSLCount(cats));
    EXPECT_STREQ("Water", cats[0]);
    EXPECT_STREQ("Land", cats[1]);

    const GDALColorTable* ct = band->GetColorTable();
    ASSERT_NE(nullptr, ct);
    EXPECT_EQ(2, ct->GetColorEntryCount());
    const GDALColorEntry* e0 = ct->GetColorEntry(0);
    ASSERT_NE(nullptr, e0);
    EXPECT_EQ(0, e0->c1);
    EXPECT_EQ(0, e0->c2);
    EXPECT_EQ(255, e0->c3);

    // GetFileList test
    StringListPtr files(ds->GetFileList(), CSLDestroy);
    ASSERT_NE(nullptr, files);
    EXPECT_EQ(2, CSLCount(files.get()));
  }
}

// -----------------------------------------------------------------------------
// Update Mode, Band Setters, FlushCache, and File Expansion
// -----------------------------------------------------------------------------

TEST_F(EnviDatasetTest, UpdateModeBandSettersAndFlushCache) {
  const VsiDir dir("update_mode");

  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("ENVI");
  ASSERT_NE(nullptr, drv);

  // 1. Create with options: BIP, SUFFIX=ADD, @BYTE_ORDER=BIG_ENDIAN
  const std::string img_path = dir.file("update_bip.bin");
  const char* options[] = {"INTERLEAVE=BIP", "SUFFIX=ADD",
                           "@BYTE_ORDER=BIG_ENDIAN", nullptr};
  {
    GDALDatasetUniquePtr created(
        drv->Create(img_path.c_str(), 4, 4, 3, GDT_Byte, const_cast<char**>(options)));
    ASSERT_NE(nullptr, created);

    // Band description
    created->GetRasterBand(1)->SetDescription("Red Channel");
    created->GetRasterBand(2)->SetDescription("Green Channel");
    // Band 3 empty description to test fallback to "Band 3"
    created->GetRasterBand(3)->SetDescription("");

    // Set dataset description
    created->SetDescription("My Test ENVI Image");

    // Color interpretation
    created->GetRasterBand(1)->SetColorInterpretation(GCI_RedBand);
    created->GetRasterBand(2)->SetColorInterpretation(GCI_GreenBand);
    created->GetRasterBand(3)->SetColorInterpretation(GCI_BlueBand);

    // Offset and scale
    created->GetRasterBand(1)->SetOffset(1.5);
    created->GetRasterBand(1)->SetScale(0.5);

    // NoData values: same value
    created->GetRasterBand(1)->SetNoDataValue(0.0);
    created->GetRasterBand(2)->SetNoDataValue(0.0);

    // Different nodata value triggering warning
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      created->GetRasterBand(1)->SetNoDataValue(255.0);
      EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("is different from nodata value"));
    }

    // Set > 5 categories to test modulo-5 newline formatting in FlushCache
    const char* cat_names[] = {"c1", "c2", "c3", "c4", "c5", "c6", nullptr};
    created->GetRasterBand(1)->SetCategoryNames(const_cast<char**>(cat_names));

    GDALColorTable ct;
    for (int i = 0; i < 6; ++i) {
      GDALColorEntry ent = {static_cast<short>(i * 10), static_cast<short>(i * 20),
                            static_cast<short>(i * 30), 255};
      ct.SetColorEntry(i, &ent);
    }
    created->GetRasterBand(1)->SetColorTable(&ct);

    // Metadata in ENVI domain
    created->SetMetadataItem("custom_key", "custom_val", "ENVI");
    created->SetMetadataItem("sensor_type", "Airborne", "ENVI");

    // Explicit FlushCache
    EXPECT_EQ(CE_None, created->FlushCache());

    // When closed, bFillFile extends file to expected size: 4 * 4 * 3 * 1 = 48 bytes
  }

  // Check file size after close
  VSIStatBufL stat_buf;
  ASSERT_EQ(0, VSIStatL(img_path.c_str(), &stat_buf));
  EXPECT_EQ(48, stat_buf.st_size);

  // Reopen and verify persisted header fields
  {
    GDALOpenInfo info(img_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);

    EXPECT_STREQ("{My Test ENVI Image}", ds->GetMetadataItem("description", "ENVI"));
    EXPECT_STREQ("Red Channel", ds->GetRasterBand(1)->GetDescription());
    EXPECT_STREQ("Band 3", ds->GetRasterBand(3)->GetDescription());
    EXPECT_STREQ("custom_val", ds->GetMetadataItem("custom_key", "ENVI"));
    EXPECT_STREQ("Airborne", ds->GetMetadataItem("sensor_type", "ENVI"));

    int has_val = 0;
    EXPECT_DOUBLE_EQ(1.5, ds->GetRasterBand(1)->GetOffset(&has_val));
    EXPECT_DOUBLE_EQ(0.5, ds->GetRasterBand(1)->GetScale(&has_val));
  }

  // 2. Create with options: BIL and BSQ
  {
    const std::string bil_path = dir.file("update_bil.bin");
    const char* bil_opts[] = {"INTERLEAVE=BIL", "@BYTE_ORDER=LITTLE_ENDIAN", nullptr};
    GDALDatasetUniquePtr bil_ds(
        drv->Create(bil_path.c_str(), 2, 2, 2, GDT_Int16, const_cast<char**>(bil_opts)));
    ASSERT_NE(nullptr, bil_ds);
    bil_ds.reset();

    VSIStatBufL bil_stat;
    ASSERT_EQ(0, VSIStatL(bil_path.c_str(), &bil_stat));
    EXPECT_EQ(2 * 2 * 2 * 2, bil_stat.st_size);
  }
}

// -----------------------------------------------------------------------------
// Statistics (.sta) File Parsing
// -----------------------------------------------------------------------------

TEST_F(EnviDatasetTest, StatisticsFileParsing) {
  const VsiDir dir("stats_parsing");

  // 1. Valid float statistics file
  {
    const std::string data_path = dir.file("float_stats.bin");
    const std::string hdr_path = dir.file("float_stats.hdr");
    const std::string sta_path = dir.file("float_stats.sta");

    WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 2\nlines = 2\nbands = 2\ndata type = 1\n");

    // Construct big-endian float .sta file
    // Header has 10 int32s: header[0] = 1111838282 (MSB), header[3] = nb = 2
    std::vector<GByte> sta_data(512, 0);
    uint32_t magic = 1111838282;
    CPL_MSBPTR32(&magic);
    std::memcpy(&sta_data[0], &magic, 4);

    uint32_t nb = 2;
    CPL_MSBPTR32(&nb);
    std::memcpy(&sta_data[12], &nb, 4);

    // Offset at 40 + (nb + 1) * 4 = 40 + 12 = 52
    uint32_t offset = 0;
    CPL_MSBPTR32(&offset);
    std::memcpy(&sta_data[52], &offset, 4);

    // Seek position: 40 + (nb + 1) * 8 + lOffset + nb = 40 + 24 + 0 + 2 = 66
    // float stats: 2 * 4 * 4 = 32 bytes (min[0,1], max[0,1], mean[0,1], std[0,1])
    float fvals[8] = {1.0f, 2.0f, 10.0f, 20.0f, 5.0f, 11.0f, 2.0f, 4.0f};
    for (int i = 0; i < 8; ++i) {
      uint32_t val = std::bit_cast<uint32_t>(fvals[i]);
      CPL_MSBPTR32(&val);
      std::memcpy(&sta_data[66 + i * 4], &val, 4);
    }

    WriteVsiBytes(sta_path.c_str(), sta_data);

    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);

    double min = 0, max = 0, mean = 0, stddev = 0;
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->GetStatistics(FALSE, FALSE, &min, &max, &mean, &stddev));
    EXPECT_DOUBLE_EQ(1.0, min);
    EXPECT_DOUBLE_EQ(10.0, max);
    EXPECT_DOUBLE_EQ(5.0, mean);
    EXPECT_DOUBLE_EQ(2.0, stddev);

    StringListPtr file_list(ds->GetFileList(), CSLDestroy);
    ASSERT_NE(nullptr, file_list);
    EXPECT_EQ(3, CSLCount(file_list.get()));
  }

  // 2. Valid double statistics file
  {
    const std::string data_path = dir.file("double_stats.bin");
    const std::string hdr_path = dir.file("double_stats.hdr");
    const std::string sta_path = dir.file("double_stats.sta");

    WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 2\nlines = 2\nbands = 1\ndata type = 1\n");

    std::vector<GByte> sta_data(512, 0);
    uint32_t magic = 0;  // Double statistics
    std::memcpy(&sta_data[0], &magic, 4);

    uint32_t nb = 1;
    CPL_MSBPTR32(&nb);
    std::memcpy(&sta_data[12], &nb, 4);

    // Offset at 40 + (nb + 1) * 4 = 40 + 8 = 48
    uint32_t offset = 0;
    std::memcpy(&sta_data[48], &offset, 4);

    // Seek position: 40 + (nb + 1) * 8 + 0 + 1 = 40 + 16 + 1 = 57
    // double stats: 1 * 4 * 8 = 32 bytes (min, max, mean, std)
    double dvals[4] = {10.0, 50.0, 30.0, 5.0};
    for (int i = 0; i < 4; ++i) {
      uint64_t val = std::bit_cast<uint64_t>(dvals[i]);
      CPL_MSBPTR64(&val);
      std::memcpy(&sta_data[57 + i * 8], &val, 8);
    }

    WriteVsiBytes(sta_path.c_str(), sta_data);

    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);

    double min = 0, max = 0, mean = 0, stddev = 0;
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->GetStatistics(FALSE, FALSE, &min, &max, &mean, &stddev));
    EXPECT_DOUBLE_EQ(10.0, min);
    EXPECT_DOUBLE_EQ(50.0, max);
    EXPECT_DOUBLE_EQ(30.0, mean);
    EXPECT_DOUBLE_EQ(5.0, stddev);
  }

  // 3. Truncated .sta file (< 40 bytes)
  {
    const std::string data_path = dir.file("trunc_stats.bin");
    const std::string hdr_path = dir.file("trunc_stats.hdr");
    const std::string sta_path = dir.file("trunc_stats.sta");

    WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 2\nlines = 2\nbands = 1\ndata type = 1\n");
    WriteVsiBytes(sta_path.c_str(), {0x00, 0x01, 0x02});

    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
  }
}

// -----------------------------------------------------------------------------
// Map Info, Geotransform, Projections, Datums, and Ellipsoids
// -----------------------------------------------------------------------------

TEST_F(EnviDatasetTest, MapInfoAndProjectionsReadAndWrite) {
  const VsiDir dir("projections");

  // 1. ProcessMapinfo with < 7 fields returns false
  {
    const std::string data_path = dir.file("short_mapinfo.bin");
    const std::string hdr_path = dir.file("short_mapinfo.hdr");
    WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 2\nlines = 2\nbands = 1\ndata type = 1\n"
                 "map info = {Arbitrary, 1, 1}\n");

    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt;
    EXPECT_EQ(CE_Failure, ds->GetGeoTransform(gt));
  }

  // 2. Arbitrary with rotation=45 and rotation=180 (bUpsideDown)
  {
    const std::string data_path = dir.file("rot_45.bin");
    const std::string hdr_path = dir.file("rot_45.hdr");
    WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 2\nlines = 2\nbands = 1\ndata type = 1\n"
                 "map info = {Arbitrary, 1, 1, 100.0, 200.0, 10.0, 10.0, rotation=45}\n");

    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt;
    EXPECT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(100.0, gt[0]);
    EXPECT_DOUBLE_EQ(200.0, gt[3]);
  }
  {
    const std::string data_path = dir.file("rot_180.bin");
    const std::string hdr_path = dir.file("rot_180.hdr");
    WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 2\nlines = 2\nbands = 1\ndata type = 1\n"
                 "map info = {Arbitrary, 1, 1, 100.0, 200.0, 10.0, 10.0, rotation=180}\n");

    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt;
    EXPECT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(10.0, gt[1]);
    EXPECT_DOUBLE_EQ(10.0, gt[5]);
  }

  // 3. UTM North and South with datum
  {
    const std::string data_path = dir.file("utm_north.bin");
    const std::string hdr_path = dir.file("utm_north.hdr");
    WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 2\nlines = 2\nbands = 1\ndata type = 1\n"
                 "map info = {UTM, 1, 1, 500000, 4000000, 30, 30, 11, North, WGS-84}\n");

    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    const OGRSpatialReference* srs = ds->GetSpatialRef();
    ASSERT_NE(nullptr, srs);
    int bNorth = 0;
    EXPECT_EQ(11, srs->GetUTMZone(&bNorth));
    EXPECT_TRUE(bNorth);
  }
  {
    const std::string data_path = dir.file("utm_south.bin");
    const std::string hdr_path = dir.file("utm_south.hdr");
    WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 2\nlines = 2\nbands = 1\ndata type = 1\n"
                 "map info = {UTM, 1, 1, 500000, 4000000, 30, 30, 12, South}\n");

    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    const OGRSpatialReference* srs = ds->GetSpatialRef();
    ASSERT_NE(nullptr, srs);
    int bNorth = 1;
    EXPECT_EQ(12, srs->GetUTMZone(&bNorth));
    EXPECT_FALSE(bNorth);
  }

  // 4. State Plane NAD 27 and NAD 83 with ITTVISToUSGSZone branches
  {
    // New zone 101
    const std::string data_path = dir.file("sp_nad27.bin");
    const std::string hdr_path = dir.file("sp_nad27.hdr");
    WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 2\nlines = 2\nbands = 1\ndata type = 1\n"
                 "map info = {State Plane (NAD 27), 1, 1, 500000, 4000000, 10, 10, 101}\n");

    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    EXPECT_NE(nullptr, ds->GetSpatialRef());
  }
  {
    // Old zone 3126 -> converted to 102
    const std::string data_path = dir.file("sp_nad83.bin");
    const std::string hdr_path = dir.file("sp_nad83.hdr");
    WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 2\nlines = 2\nbands = 1\ndata type = 1\n"
                 "map info = {State Plane (NAD 83), 1, 1, 500000, 4000000, 10, 10, 3126}\n");

    WithQuietHandler quiet;
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    EXPECT_NE(nullptr, ds->GetSpatialRef());
  }
  {
    // Zone not in list -> 99999
    const std::string data_path = dir.file("sp_unknown.bin");
    const std::string hdr_path = dir.file("sp_unknown.hdr");
    WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 2\nlines = 2\nbands = 1\ndata type = 1\n"
                 "map info = {State Plane (NAD 83), 1, 1, 500000, 4000000, 10, 10, 99999}\n");

    WithQuietHandler quiet;
    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
  }

  // 5. Geographic Lat/Lon and Angular Units: Radians, Degrees, Minutes, Seconds
  {
    constexpr struct {
      const char* unit;
      double factor;
    } kAngularCases[] = {
        {"Radians", 1.0},
        {"Degrees", 1.0},
        {"Minutes", 60.0},
        {"Seconds", 3600.0},
    };
    for (const auto& a : kAngularCases) {
      const std::string name = std::format("geo_{}", a.unit);
      const std::string data_path = dir.file(name + ".bin");
      const std::string hdr_path = dir.file(name + ".hdr");
      WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
      WriteVsiText(
          hdr_path.c_str(),
          std::format("ENVI\nsamples = 2\nlines = 2\nbands = 1\ndata type = 1\n"
                      "map info = {{Geographic Lat/Lon, 1, 1, -120.0, 38.0, 0.01, 0.01, WGS-84, units={}}}\n",
                      a.unit));

      GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
      std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
      ASSERT_NE(nullptr, ds);
      GDALGeoTransform gt;
      EXPECT_EQ(CE_None, ds->GetGeoTransform(gt));
      EXPECT_NEAR(-120.0 / a.factor, gt[0], 1e-7);
    }
  }

  // 6. Linear Units: Feet, Meters, Km, Yards, Miles, Nautical Miles
  {
    constexpr std::string_view kLinearUnits[] = {
        "Feet", "Meters", "Km", "Yards", "Miles", "Nautical Miles"};
    for (const auto& unit : kLinearUnits) {
      const std::string name = std::format("linear_{}", unit);
      std::string safe_name = name;
      for (char& c : safe_name) {
        if (c == ' ') c = '_';
      }
      const std::string data_path = dir.file(safe_name + ".bin");
      const std::string hdr_path = dir.file(safe_name + ".hdr");
      WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
      WriteVsiText(
          hdr_path.c_str(),
          std::format("ENVI\nsamples = 2\nlines = 2\nbands = 1\ndata type = 1\n"
                      "map info = {{UTM, 1, 1, 500000, 4000000, 10, 10, 11, North, WGS-84, units={}}}\n",
                      unit));

      GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
      std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
      ASSERT_NE(nullptr, ds);
    }
  }

  // 7. All datums in SetENVIDatum
  {
    constexpr const char* kDatums[] = {
        "WGS-84", "WGS-72", "North America 1983", "North America 1927",
        "NAD27", "NAD-27", "European 1950", "Ordnance Survey of Great Britain '36",
        "SAD-69/Brazil", "Geocentric Datum of Australia 1994", "Australian Geodetic 1984",
        "Nouvelle Triangulation Francaise IGN", "GRS 80", "Airy",
        "Australian National", "Bessel 1841", "Clark 1866", "UnrecognizedDatum"};

    int idx = 0;
    for (const char* datum : kDatums) {
      const std::string name = std::format("datum_{}", idx++);
      const std::string data_path = dir.file(name + ".bin");
      const std::string hdr_path = dir.file(name + ".hdr");
      WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
      WriteVsiText(
          hdr_path.c_str(),
          std::format("ENVI\nsamples = 2\nlines = 2\nbands = 1\ndata type = 1\n"
                      "map info = {{UTM, 1, 1, 500000, 4000000, 10, 10, 11, North, {}}}\n",
                      datum));

      WithQuietHandler quiet;
      CPLErrorReset();
      GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
      std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
      ASSERT_NE(nullptr, ds);
      EXPECT_NE(nullptr, ds->GetSpatialRef());
    }
  }

  // 8. SetENVIEllipse via projection info with numeric ellipse parameters
  {
    // Sphere: a == b
    {
      const std::string data_path = dir.file("ellipse_sphere.bin");
      const std::string hdr_path = dir.file("ellipse_sphere.hdr");
      WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
      WriteVsiText(
          hdr_path.c_str(),
          "ENVI\nsamples = 2\nlines = 2\nbands = 1\ndata type = 1\n"
          "map info = {Custom, 1, 1, 0, 0, 1, 1}\n"
          "projection info = {3, 6378137, 6378137, 0, 0, 0, 0, 1, 12345, Transverse Mercator}\n");

      GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
      std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
      ASSERT_NE(nullptr, ds);
    }
    // Ellipsoid: a != b
    {
      const std::string data_path = dir.file("ellipse_oblate.bin");
      const std::string hdr_path = dir.file("ellipse_oblate.hdr");
      WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
      WriteVsiText(
          hdr_path.c_str(),
          "ENVI\nsamples = 2\nlines = 2\nbands = 1\ndata type = 1\n"
          "map info = {Custom, 1, 1, 0, 0, 1, 1}\n"
          "projection info = {3, 6378137, 6356752, 0, 0, 0, 0, 1, 12345, Transverse Mercator}\n");

      GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
      std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
      ASSERT_NE(nullptr, ds);
    }
  }

  // 9. All 10 projection_info types in ProcessMapinfo
  {
    constexpr struct {
      int proj_code;
      const char* name;
      const char* pi_str;
    } kProjCases[] = {
        {3, "TM", "3, 6378137, 6356752, 0.0, -120.0, 500000, 0, 0.9996, WGS-84"},
        {4, "LCC", "4, 6378137, 6356752, 30.0, -120.0, 500000, 0, 33.0, 45.0, WGS-84"},
        {5, "HOM2PNO", "5, 6378137, 6356752, 4.0, 2.0, 114.0, 6.0, 116.0, 0, 0, 1.0, WGS-84"},
        {6, "HOM", "6, 6378137, 6356752, 4.0, 115.0, 53.0, 0, 0, 1.0, WGS-84"},
        {7, "Stereo", "7, 6378137, 6356752, 90.0, 0.0, 0, 0, 1.0, WGS-84"},
        {9, "Albers", "9, 6378137, 6356752, 23.0, -96.0, 0, 0, 29.5, 45.5, WGS-84"},
        {10, "Polyconic", "10, 6378137, 6356752, 0.0, -75.0, 0, 0, WGS-84"},
        {11, "LAEA", "11, 6378137, 6356752, 45.0, -100.0, 0, 0, WGS-84"},
        {12, "AE", "12, 6378137, 6356752, 45.0, -100.0, 0, 0, WGS-84"},
        {31, "PolarStereo", "31, 6378137, 6356752, 90.0, 0.0, 0, 0, WGS-84"},
    };

    for (const auto& p : kProjCases) {
      const std::string name = std::format("proj_{}", p.name);
      const std::string data_path = dir.file(name + ".bin");
      const std::string hdr_path = dir.file(name + ".hdr");
      WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
      WriteVsiText(
          hdr_path.c_str(),
          std::format("ENVI\nsamples = 2\nlines = 2\nbands = 1\ndata type = 1\n"
                      "map info = {{Custom, 1, 1, 500000, 4000000, 10, 10}}\n"
                      "projection info = {{{}}}\n",
                      p.pi_str));

      GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
      std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
      ASSERT_NE(nullptr, ds);
      EXPECT_NE(nullptr, ds->GetSpatialRef());
    }
  }

  // 10. WriteProjectionInfo in GA_Update mode:
  // Testing all projections written by WriteProjectionInfo:
  // UTM, Geographic, NZMG, TM, LCC, HOM A, HOM B, Stereo, Albers, Polyconic,
  // LAEA, Azimuthal Equidistant, Polar Stereographic, and unrecognized
  {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("ENVI");
    ASSERT_NE(nullptr, drv);

    struct WriteProjCase {
      const char* name;
      int epsg_or_setup;
    };

    constexpr WriteProjCase kWPCases[] = {
        {"utm_n", 32611},      // UTM 11N
        {"utm_s", 32711},      // UTM 11S
        {"wgs84", 4326},       // Geographic WGS-84
        {"wgs72", 4322},       // Geographic WGS-72
        {"nad83", 4269},       // Geographic NAD 83
        {"nad27", 4267},       // Geographic NAD 27
        {"nzmg", 27200},       // New Zealand Map Grid (39)
        {"tm", 27700},         // Transverse Mercator (3)
        {"lcc", 2154},         // Lambert Conformal Conic (4)
        {"albers", 5070},      // Albers Equal Area (9)
        {"laea", 3035},        // Lambert Azimuthal Equal Area (11)
        {"polar_stereo", 3031} // Polar Stereographic (31)
    };

    for (const auto& w : kWPCases) {
      const std::string img_path = dir.file(std::format("write_proj_{}.bin", w.name));
      GDALDatasetUniquePtr created(
          drv->Create(img_path.c_str(), 2, 2, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, created);

      OGRSpatialReference srs;
      EXPECT_EQ(OGRERR_NONE, srs.importFromEPSG(w.epsg_or_setup));
      EXPECT_EQ(CE_None, created->SetSpatialRef(&srs));

      GDALGeoTransform gt = {100000.0, 10.0, 0.0, 500000.0, 0.0, -10.0};
      EXPECT_EQ(CE_None, created->SetGeoTransform(gt));

      EXPECT_EQ(CE_None, created->FlushCache());
      created.reset();

      // Reopen and verify spatial reference
      GDALOpenInfo info(img_path.c_str(), GA_ReadOnly);
      std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
      ASSERT_NE(nullptr, ds);
      EXPECT_NE(nullptr, ds->GetSpatialRef());
    }

    // Additional projections configured via PROJECTION strings:
    // HOM A (Two point natural origin), HOM B, Stereographic, Polyconic, Azimuthal Equidistant
    {
      const std::string img_path = dir.file("write_proj_hom_a.bin");
      GDALDatasetUniquePtr created(
          drv->Create(img_path.c_str(), 2, 2, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, created);

      OGRSpatialReference srs;
      srs.SetWellKnownGeogCS("WGS84");
      EXPECT_EQ(OGRERR_NONE,
                srs.SetHOM2PNO(4.0, 2.0, 114.0, 6.0, 116.0, 1.0, 0.0, 0.0));
      EXPECT_EQ(CE_None, created->SetSpatialRef(&srs));
      GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
      EXPECT_EQ(CE_None, created->SetGeoTransform(gt));
      created.reset();
    }
    {
      const std::string img_path = dir.file("write_proj_hom_b.bin");
      GDALDatasetUniquePtr created(
          drv->Create(img_path.c_str(), 2, 2, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, created);

      OGRSpatialReference srs;
      srs.SetWellKnownGeogCS("WGS84");
      EXPECT_EQ(OGRERR_NONE,
                srs.SetHOM(4.0, 115.0, 53.0, 0.0, 1.0, 0.0, 0.0));
      EXPECT_EQ(CE_None, created->SetSpatialRef(&srs));
      GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
      EXPECT_EQ(CE_None, created->SetGeoTransform(gt));
      created.reset();
    }
    {
      const std::string img_path = dir.file("write_proj_stereo.bin");
      GDALDatasetUniquePtr created(
          drv->Create(img_path.c_str(), 2, 2, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, created);

      OGRSpatialReference srs;
      srs.SetWellKnownGeogCS("WGS84");
      EXPECT_EQ(OGRERR_NONE, srs.SetStereographic(90.0, 0.0, 1.0, 0.0, 0.0));
      EXPECT_EQ(CE_None, created->SetSpatialRef(&srs));
      GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
      EXPECT_EQ(CE_None, created->SetGeoTransform(gt));
      created.reset();
    }
    {
      const std::string img_path = dir.file("write_proj_polyconic.bin");
      GDALDatasetUniquePtr created(
          drv->Create(img_path.c_str(), 2, 2, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, created);

      OGRSpatialReference srs;
      srs.SetWellKnownGeogCS("WGS84");
      EXPECT_EQ(OGRERR_NONE, srs.SetPolyconic(0.0, -75.0, 0.0, 0.0));
      EXPECT_EQ(CE_None, created->SetSpatialRef(&srs));
      GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
      EXPECT_EQ(CE_None, created->SetGeoTransform(gt));
      created.reset();
    }
    {
      const std::string img_path = dir.file("write_proj_ae.bin");
      GDALDatasetUniquePtr created(
          drv->Create(img_path.c_str(), 2, 2, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, created);

      OGRSpatialReference srs;
      srs.SetWellKnownGeogCS("WGS84");
      EXPECT_EQ(OGRERR_NONE, srs.SetAE(45.0, -100.0, 0.0, 0.0));
      EXPECT_EQ(CE_None, created->SetSpatialRef(&srs));
      GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
      EXPECT_EQ(CE_None, created->SetGeoTransform(gt));
      created.reset();
    }
    {
      // Fallback projection: Mercator_1SP
      const std::string img_path = dir.file("write_proj_mercator.bin");
      GDALDatasetUniquePtr created(
          drv->Create(img_path.c_str(), 2, 2, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, created);

      OGRSpatialReference srs;
      srs.SetWellKnownGeogCS("WGS84");
      srs.SetMercator(0.0, 0.0, 1.0, 0.0, 0.0);
      created->SetSpatialRef(&srs);
      GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
      created->SetGeoTransform(gt);
      created.reset();
    }

    // Write with rotation=180 and with sheared matrix (warning)
    {
      const std::string img_path = dir.file("write_rot_180.bin");
      GDALDatasetUniquePtr created(
          drv->Create(img_path.c_str(), 2, 2, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, created);
      // gt[1] > 0, gt[5] > 0 -> rotation=180
      GDALGeoTransform gt = {100.0, 5.0, 0.0, 200.0, 0.0, 5.0};
      created->SetGeoTransform(gt);
      created.reset();
    }
    {
      const std::string img_path = dir.file("write_shear.bin");
      GDALDatasetUniquePtr created(
          drv->Create(img_path.c_str(), 2, 2, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, created);
      // Non-rotational terms: dfRotation1 != dfRotation2
      GDALGeoTransform gt = {100.0, 5.0, 2.0, 200.0, 1.0, -5.0};
      created->SetGeoTransform(gt);
      WithQuietHandler quiet;
      CPLErrorReset();
      created->FlushCache();
      EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("non rotational terms"));
    }
  }
}

// -----------------------------------------------------------------------------
// RPC Info Read and Write
// -----------------------------------------------------------------------------

TEST_F(EnviDatasetTest, RpcAndGeoPointsReadAndWrite) {
  const VsiDir dir("rpc_and_gcps");

  // 1. ProcessRPCinfo with < 90 fields is ignored
  {
    const std::string data_path = dir.file("rpc_short.bin");
    const std::string hdr_path = dir.file("rpc_short.hdr");
    WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 2\nlines = 2\nbands = 1\ndata type = 1\n"
                 "rpc info = {1.0, 2.0, 3.0}\n");

    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(nullptr, ds->GetMetadataItem("LINE_OFF", "RPC"));
  }

  // 2. ProcessRPCinfo with 93 fields and chipping offsets
  {
    const std::string data_path = dir.file("rpc_93.bin");
    const std::string hdr_path = dir.file("rpc_93.hdr");
    WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));

    std::string rpc_body = "1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0";
    for (int i = 0; i < 80; ++i) {
      rpc_body += ", 0.001";
    }
    // TILE_ROW_OFFSET=10, TILE_COL_OFFSET=20, ENVI_RPC_EMULATION=1
    rpc_body += ", 10, 20, 1";

    WriteVsiText(
        hdr_path.c_str(),
        std::format("ENVI\nsamples = 100\nlines = 100\nbands = 1\ndata type = 1\n"
                    "rpc info = {{{}}}\n",
                    rpc_body));

    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);

    EXPECT_STREQ("1", ds->GetMetadataItem("LINE_OFF", "RPC"));
    EXPECT_STREQ("2", ds->GetMetadataItem("SAMP_OFF", "RPC"));
    EXPECT_STREQ("10", ds->GetMetadataItem("TILE_ROW_OFFSET", "RPC"));
    EXPECT_STREQ("20", ds->GetMetadataItem("TILE_COL_OFFSET", "RPC"));
    EXPECT_STREQ("1", ds->GetMetadataItem("ICHIP_SCALE_FACTOR"));
    EXPECT_STREQ("0.5", ds->GetMetadataItem("ICHIP_OP_ROW_11"));
    EXPECT_STREQ("10.5", ds->GetMetadataItem("ICHIP_FI_ROW_11"));
  }

  // 3. WriteRpcInfo via SetMetadata in RPC domain
  {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("ENVI");
    ASSERT_NE(nullptr, drv);

    const std::string img_path = dir.file("write_rpc.bin");
    GDALDatasetUniquePtr created(
        drv->Create(img_path.c_str(), 10, 10, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, created);

    created->SetMetadataItem("LINE_OFF", "500", "RPC");
    created->SetMetadataItem("SAMP_OFF", "500", "RPC");
    created->SetMetadataItem("LAT_OFF", "35.0", "RPC");
    created->SetMetadataItem("LONG_OFF", "-120.0", "RPC");
    created->SetMetadataItem("HEIGHT_OFF", "100.0", "RPC");
    created->SetMetadataItem("LINE_SCALE", "1000", "RPC");
    created->SetMetadataItem("SAMP_SCALE", "1000", "RPC");
    created->SetMetadataItem("LAT_SCALE", "1.0", "RPC");
    created->SetMetadataItem("LONG_SCALE", "1.0", "RPC");
    created->SetMetadataItem("HEIGHT_SCALE", "500.0", "RPC");

    std::string twenty_coeffs = "0.0 0.001 -0.002 0.0 0.0 0.0 0.0 0.0 0.0 0.0 "
                                "0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0";
    created->SetMetadataItem("LINE_NUM_COEFF", twenty_coeffs.c_str(), "RPC");
    created->SetMetadataItem("LINE_DEN_COEFF", twenty_coeffs.c_str(), "RPC");
    created->SetMetadataItem("SAMP_NUM_COEFF", twenty_coeffs.c_str(), "RPC");
    created->SetMetadataItem("SAMP_DEN_COEFF", twenty_coeffs.c_str(), "RPC");

    created->SetMetadataItem("TILE_ROW_OFFSET", "0", "RPC");
    created->SetMetadataItem("TILE_COL_OFFSET", "0", "RPC");
    created->SetMetadataItem("ENVI_RPC_EMULATION", "0", "RPC");

    EXPECT_EQ(CE_None, created->FlushCache());
    created.reset();

    // Reopen and verify RPC info was written and reread
    GDALOpenInfo info(img_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    EXPECT_STREQ("500", ds->GetMetadataItem("LINE_OFF", "RPC"));
  }

  // 4. ProcessGeoPoints and WritePseudoGcpInfo
  {
    // Reading geo_points from .hdr
    const std::string data_path = dir.file("gcps.bin");
    const std::string hdr_path = dir.file("gcps.hdr");
    WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
    WriteVsiText(
        hdr_path.c_str(),
        "ENVI\nsamples = 10\nlines = 10\nbands = 1\ndata type = 1\n"
        "geo points = {\n"
        "  1.0, 1.0, 38.0, -120.0,\n"
        "  10.0, 1.0, 38.0, -119.0,\n"
        "  10.0, 10.0, 37.0, -119.0,\n"
        "  1.0, 10.0, 37.0, -120.0\n"
        "}\n");

    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);

    EXPECT_EQ(4, ds->GetGCPCount());
    const GDAL_GCP* gcps = ds->GetGCPs();
    ASSERT_NE(nullptr, gcps);
    // ENVI 1-based convention subtracted 1: 1.0 -> 0.0, 10.0 -> 9.0
    EXPECT_DOUBLE_EQ(0.0, gcps[0].dfGCPPixel);
    EXPECT_DOUBLE_EQ(0.0, gcps[0].dfGCPLine);
    EXPECT_DOUBLE_EQ(38.0, gcps[0].dfGCPY);
    EXPECT_DOUBLE_EQ(-120.0, gcps[0].dfGCPX);

    EXPECT_DOUBLE_EQ(9.0, gcps[1].dfGCPPixel);
    EXPECT_DOUBLE_EQ(0.0, gcps[1].dfGCPLine);
  }

  // 5. Setting GCPs in update mode and writing to header
  {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("ENVI");
    ASSERT_NE(nullptr, drv);

    const std::string img_path = dir.file("write_gcps.bin");
    GDALDatasetUniquePtr created(
        drv->Create(img_path.c_str(), 10, 10, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, created);

    GDAL_GCP gcp_list[4];
    GDALInitGCPs(4, gcp_list);
    absl::Cleanup deinit_gcps = [&] { GDALDeinitGCPs(4, gcp_list); };
    gcp_list[0].dfGCPPixel = 0.0;
    gcp_list[0].dfGCPLine = 0.0;
    gcp_list[0].dfGCPX = -120.0;
    gcp_list[0].dfGCPY = 38.0;

    gcp_list[1].dfGCPPixel = 9.0;
    gcp_list[1].dfGCPLine = 0.0;
    gcp_list[1].dfGCPX = -119.0;
    gcp_list[1].dfGCPY = 38.0;

    gcp_list[2].dfGCPPixel = 9.0;
    gcp_list[2].dfGCPLine = 9.0;
    gcp_list[2].dfGCPX = -119.0;
    gcp_list[2].dfGCPY = 37.0;

    gcp_list[3].dfGCPPixel = 0.0;
    gcp_list[3].dfGCPLine = 9.0;
    gcp_list[3].dfGCPX = -120.0;
    gcp_list[3].dfGCPY = 37.0;

    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    EXPECT_EQ(CE_None, created->SetGCPs(4, gcp_list, &srs));

    EXPECT_EQ(CE_None, created->FlushCache());
    created.reset();

    // Reopen and verify GCPs
    GDALOpenInfo info(img_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(4, ds->GetGCPCount());
  }
}

// -----------------------------------------------------------------------------
// Helper and Boundary Condition Edge Cases
// -----------------------------------------------------------------------------

TEST_F(EnviDatasetTest, BoundaryConditionsAndErrorHandling) {
  const VsiDir dir("edge_cases");

  // 1. Create file error (empty or invalid path)
  {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("ENVI");
    ASSERT_NE(nullptr, drv);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, drv->Create("", 2, 2, 1, GDT_Byte, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 2. SetNoDataValue NaN comparison
  {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("ENVI");
    ASSERT_NE(nullptr, drv);

    const std::string img_path = dir.file("nan_nodata.bin");
    GDALDatasetUniquePtr created(
        drv->Create(img_path.c_str(), 2, 2, 2, GDT_Float32, nullptr));
    ASSERT_NE(nullptr, created);

    WithQuietHandler quiet;
    CPLErrorReset();
    const double nan_val = std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(CE_None, created->GetRasterBand(1)->SetNoDataValue(nan_val));
    // Both NaN values should not trigger warning
    EXPECT_EQ(CE_None, created->GetRasterBand(2)->SetNoDataValue(nan_val));
    EXPECT_EQ(CE_None, CPLGetLastErrorType());
    created.reset();
  }

  // 3. ProcessGeoPoints with count not multiple of 4
  {
    const std::string data_path = dir.file("bad_gcps.bin");
    const std::string hdr_path = dir.file("bad_gcps.hdr");
    WriteVsiBytes(data_path.c_str(), std::vector<GByte>(16, 0));
    WriteVsiText(hdr_path.c_str(),
                 "ENVI\nsamples = 2\nlines = 2\nbands = 1\ndata type = 1\n"
                 "geo points = {1.0, 2.0, 3.0}\n");

    GDALOpenInfo info(data_path.c_str(), GA_ReadOnly);
    std::unique_ptr<GDALDataset> ds(ENVIDataset::Open(&info));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(0, ds->GetGCPCount());
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover remaining I/O failure branches.
