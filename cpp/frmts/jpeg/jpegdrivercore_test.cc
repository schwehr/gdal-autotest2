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
// Tests for JPEG driver identification and common metadata
// (jpegdrivercore.cpp). Note: google3 builds with libjpeg-turbo where
// D_LOSSLESS_SUPPORTED is defined, so JPEGDriverIdentify accepts lossless JPEG
// markers (JPEGDatasetIsJPEGLS is tested directly here) and
// DeclareDeferredJPEGPlugin (PLUGIN_FILENAME) is not compiled.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/jpeg/jpegdrivercore.cpp
//   https://gdal.org/drivers/raster/jpeg.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/jpeg.py

#include "third_party/gdal/frmts/jpeg/jpegdrivercore.h"

#include <array>
#include <cstdint>
#include <format>  // NOLINT
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_openinfo.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kVsiDir[] = "/vsimem/jpegdrivercore_test";

constexpr auto kValidJpegPrefix = std::to_array<char>(
    {'\xff', '\xd8', '\xff', '\xe0', 0x00, 0x04, 0x00, 0x00, '\xff', '\xd9'});
static_assert(kValidJpegPrefix.size() == 10);

constexpr std::string_view kValidHeaderView(kValidJpegPrefix.data(),
                                            kValidJpegPrefix.size());

struct IdentifyCase {
  std::string_view filename;
  std::string_view header;
  bool expected;
};

constexpr IdentifyCase kIdentifyCases[] = {
    {.filename = "empty.jpg", .header = "", .expected = false},
    {.filename = "short_9_bytes.jpg",
     .header = std::string_view("\xff\xd8\xff\xe0\x00\x04\x00\x00\xff", 9),
     .expected = false},
    {.filename = "bad_byte0.jpg",
     .header = std::string_view("\x00\xd8\xff\xe0\x00\x04\x00\x00\xff\xd9", 10),
     .expected = false},
    {.filename = "bad_byte1.jpg",
     .header = std::string_view("\xff\x00\xff\xe0\x00\x04\x00\x00\xff\xd9", 10),
     .expected = false},
    {.filename = "bad_byte2.jpg",
     .header = std::string_view("\xff\xd8\x00\xe0\x00\x04\x00\x00\xff\xd9", 10),
     .expected = false},
    {.filename = "valid_10_bytes.jpg",
     .header = kValidHeaderView,
     .expected = true},
    {.filename = "upper_ext.JPEG",
     .header = kValidHeaderView,
     .expected = true},
    // SRTM HGT files may accidentally start with FF D8 FF and must be rejected.
    {.filename = "N65E039.hgt", .header = kValidHeaderView, .expected = false},
    {.filename = "N65E039.HGT.GZ",
     .header = kValidHeaderView,
     .expected = false},
    {.filename = "N65E039.hgt.zip",
     .header = kValidHeaderView,
     .expected = false},
};

struct JpegLsCase {
  std::string_view name;
  std::string_view header;
  bool expected;
};

constexpr JpegLsCase kJpegLsCases[] = {
    {.name = "empty", .header = "", .expected = false},
    {.name = "short_9_bytes",
     .header = std::string_view("\xff\xd8\xff\xf7\x00\x08\x00\x00\x00", 9),
     .expected = false},
    {.name = "bad_soi_byte0",
     .header = std::string_view("\x00\xd8\xff\xf7\x00\x08\x00\x00\x00\x00", 10),
     .expected = false},
    {.name = "bad_soi_byte1",
     .header = std::string_view("\xff\x00\xff\xf7\x00\x08\x00\x00\x00\x00", 10),
     .expected = false},
    {.name = "missing_marker_prefix",
     .header = std::string_view("\xff\xd8\x00\xf7\x00\x08\x00\x00\x00\x00", 10),
     .expected = false},
    {.name = "start_of_scan_0xDA",
     .header = std::string_view("\xff\xd8\xff\xda\x00\x08\x00\x00\x00\x00", 10),
     .expected = false},
    {.name = "jpegls_0xF7",
     .header = std::string_view("\xff\xd8\xff\xf7\x00\x08\x00\x00\x00\x00", 10),
     .expected = true},
    {.name = "jpegls_ext_0xF8",
     .header = std::string_view("\xff\xd8\xff\xf8\x00\x08\x00\x00\x00\x00", 10),
     .expected = true},
    {.name = "sof3_lossless_huffman_0xC3",
     .header = std::string_view("\xff\xd8\xff\xc3\x00\x08\x00\x00\x00\x00", 10),
     .expected = true},
    {.name = "sof7_diff_lossless_huffman_0xC7",
     .header = std::string_view("\xff\xd8\xff\xc7\x00\x08\x00\x00\x00\x00", 10),
     .expected = true},
    {.name = "sof11_lossless_arith_0xCB",
     .header = std::string_view("\xff\xd8\xff\xcb\x00\x08\x00\x00\x00\x00", 10),
     .expected = true},
    {.name = "sof15_diff_lossless_arith_0xCF",
     .header = std::string_view("\xff\xd8\xff\xcf\x00\x08\x00\x00\x00\x00", 10),
     .expected = true},
    // Skip a 4-byte APP0 segment (offset 2..7) and find SOF3 (0xC3) at
    // offset 8.
    {.name = "after_app0_segment",
     .header = std::string_view(
         "\xff\xd8\xff\xe0\x00\x04\x4a\x46\xff\xc3\x00\x08\x00\x00", 14),
     .expected = true},
    // Skip APP0 segment and hit baseline SOF0 (0xC0) with no room left ->
    // false.
    {.name = "baseline_sof0_end_of_header",
     .header = std::string_view(
         "\xff\xd8\xff\xe0\x00\x04\x4a\x46\xff\xc0\x00\x08\x00\x00", 14),
     .expected = false},
};

struct MetadataCase {
  const char* key;
  const char* expected;
};

constexpr MetadataCase kMetadataCases[] = {
    {.key = GDAL_DCAP_RASTER, .expected = "YES"},
    {.key = GDAL_DMD_LONGNAME, .expected = "JPEG JFIF"},
    {.key = GDAL_DMD_HELPTOPIC, .expected = "drivers/raster/jpeg.html"},
    {.key = GDAL_DMD_EXTENSION, .expected = "jpg"},
    {.key = GDAL_DMD_EXTENSIONS, .expected = "jpg jpeg"},
    {.key = GDAL_DMD_MIMETYPE, .expected = "image/jpeg"},
    {.key = GDAL_DMD_CREATIONDATATYPES, .expected = "Byte"},
    {.key = GDAL_DCAP_VIRTUALIO, .expected = "YES"},
    {.key = GDAL_DCAP_CREATE_ONLY_VISIBLE_AT_CLOSE_TIME, .expected = "YES"},
    {.key = GDAL_DCAP_OPEN, .expected = "YES"},
    {.key = GDAL_DCAP_CREATECOPY, .expected = "YES"},
};

TEST(JpegDriverCoreTest, IdentifyDoesNotExist) {
  GDALOpenInfo missing("/does_not_exist", GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(FALSE, JPEGDriverIdentify(&missing));
}

TEST(JpegDriverCoreTest, IdentifySubfilePrefixes) {
  // 1. JPEG_SUBFILE: is matched case-insensitively.
  GDALOpenInfo subfile_upper("JPEG_SUBFILE:1,2,/does_not_exist",
                             GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(TRUE, JPEGDriverIdentify(&subfile_upper));

  GDALOpenInfo subfile_lower("jpeg_subfile:1,2,/does_not_exist",
                             GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(TRUE, JPEGDriverIdentify(&subfile_lower));

  // 2. JPEG: is matched case-sensitively.
  GDALOpenInfo subdataset_upper("JPEG:/does_not_exist:sub", GDAL_OF_READONLY,
                                nullptr);
  EXPECT_EQ(TRUE, JPEGDriverIdentify(&subdataset_upper));

  GDALOpenInfo subdataset_lower("jpeg:/does_not_exist:sub", GDAL_OF_READONLY,
                                nullptr);
  EXPECT_EQ(FALSE, JPEGDriverIdentify(&subdataset_lower));
}

TEST(JpegDriverCoreTest, IdentifyHeaderVariants) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };

  for (const auto& tc : kIdentifyCases) {
    SCOPED_TRACE(tc.filename);
    const std::string path = std::format("{}/{}", kVsiDir, tc.filename);
    VsiMemTempWrapper file(path, std::string(tc.header));
    GDALOpenInfo info(path.c_str(), GDAL_OF_READONLY, nullptr);
    EXPECT_EQ(tc.expected ? TRUE : FALSE, JPEGDriverIdentify(&info));
  }
}

TEST(JpegDriverCoreTest, DatasetIsJpegLsHeaderVariants) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };

  for (const auto& tc : kJpegLsCases) {
    SCOPED_TRACE(tc.name);
    const std::string path = std::format("{}/ls_{}.jpg", kVsiDir, tc.name);
    VsiMemTempWrapper file(path, std::string(tc.header));
    GDALOpenInfo info(path.c_str(), GDAL_OF_READONLY, nullptr);
    EXPECT_EQ(tc.expected, JPEGDatasetIsJPEGLS(&info));
  }

  // Exercise 16-bit big-endian segment length calculation (hi*256 + lo) by
  // skipping a 260-byte APP0 segment before encountering 0xF7 (JPEG-LS).
  std::vector<uint8_t> large_app0(270, 0x00);
  large_app0[0] = 0xFF;
  large_app0[1] = 0xD8;
  large_app0[2] = 0xFF;
  large_app0[3] = 0xE0;
  large_app0[4] = 0x01;  // Length high byte: 1 * 256 + 4 = 260
  large_app0[5] = 0x04;
  large_app0[264] = 0xFF;
  large_app0[265] = 0xF7;  // JPEG-LS marker at offset 264 (2 + 2 + 260)
  const std::string large_path = std::format("{}/ls_large_app0.jpg", kVsiDir);
  VsiMemTempWrapper large_file(
      large_path, std::string(reinterpret_cast<const char*>(large_app0.data()),
                              large_app0.size()));
  GDALOpenInfo large_info(large_path.c_str(), GDAL_OF_READONLY, nullptr);
  EXPECT_TRUE(JPEGDatasetIsJPEGLS(&large_info));
}

TEST(JpegDriverCoreTest, SetCommonMetadata) {
  GDALDriver driver;
  JPEGDriverSetCommonMetadata(&driver);

  EXPECT_STREQ("JPEG", driver.GetDescription());
  for (const auto& tc : kMetadataCases) {
    SCOPED_TRACE(tc.key);
    EXPECT_STREQ(tc.expected, driver.GetMetadataItem(tc.key));
  }
  const char* open_options = driver.GetMetadataItem(GDAL_DMD_OPENOPTIONLIST);
  ASSERT_NE(nullptr, open_options);
  EXPECT_THAT(open_options, HasSubstr("USE_INTERNAL_OVERVIEWS"));
  EXPECT_THAT(open_options, HasSubstr("APPLY_ORIENTATION"));
  EXPECT_STREQ("YES",
               driver.GetMetadataItem("LOSSLESS_JPEG_SUPPORTED", "JPEG"));
  EXPECT_EQ(JPEGDriverIdentify, driver.pfnIdentify);
}

}  // namespace
}  // namespace autotest2
