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
// Tests Kakadu JPEG 2000 driver core identification and metadata registration
// (jp2kakdrivercore.cpp).
//
// See also:
//   https://gdal.org/drivers/raster/jp2kak.html
//   https://github.com/OSGeo/gdal/blob/master/frmts/jp2kak/jp2kakdrivercore.cpp

#include "third_party/gdal/frmts/jp2kak/jp2kakdrivercore.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::StartsWith;

std::string BytesToString(std::span<const uint8_t> bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

int Identify(const char* path) {
  GDALOpenInfo open_info(path, GDAL_OF_READONLY);
  return JP2KAKDatasetIdentify(&open_info);
}

TEST(Jp2kakdrivercoreTest, SetCommonMetadata) {
  GDALDriver driver;
  JP2KAKDriverSetCommonMetadata(&driver);

  EXPECT_STREQ(DRIVER_NAME, driver.GetDescription());
  EXPECT_STREQ("YES", driver.GetMetadataItem(GDAL_DCAP_RASTER));
  EXPECT_STREQ("YES", driver.GetMetadataItem(GDAL_DCAP_VECTOR));
  EXPECT_THAT(driver.GetMetadataItem(GDAL_DMD_LONGNAME),
              StartsWith("JPEG-2000 (based on Kakadu "));
  EXPECT_STREQ("drivers/raster/jp2kak.html",
               driver.GetMetadataItem(GDAL_DMD_HELPTOPIC));
  EXPECT_STREQ("Byte Int16 UInt16 Int32 UInt32",
               driver.GetMetadataItem(GDAL_DMD_CREATIONDATATYPES));
  EXPECT_STREQ("image/jp2", driver.GetMetadataItem(GDAL_DMD_MIMETYPE));
  EXPECT_STREQ("jp2 j2k", driver.GetMetadataItem(GDAL_DMD_EXTENSION));
  EXPECT_STREQ("YES", driver.GetMetadataItem(GDAL_DCAP_VIRTUALIO));
  EXPECT_STREQ("YES", driver.GetMetadataItem(GDAL_DCAP_OPEN));
  EXPECT_STREQ("YES", driver.GetMetadataItem(GDAL_DCAP_CREATECOPY));
  EXPECT_EQ(&JP2KAKDatasetIdentify, driver.pfnIdentify);

  const char* open_options = driver.GetMetadataItem(GDAL_DMD_OPENOPTIONLIST);
  ASSERT_NE(nullptr, open_options);
  EXPECT_THAT(open_options, HasSubstr("1BIT_ALPHA_PROMOTION"));
  EXPECT_THAT(open_options, HasSubstr("OPEN_REMOTE_GML"));
  EXPECT_THAT(open_options, HasSubstr("GEOREF_SOURCES"));

  const char* creation_options =
      driver.GetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST);
  ASSERT_NE(nullptr, creation_options);
  EXPECT_THAT(creation_options, HasSubstr("CODEC"));
  EXPECT_THAT(creation_options, HasSubstr("QUALITY"));
  EXPECT_THAT(creation_options, HasSubstr("BLOCKXSIZE"));
  EXPECT_THAT(creation_options, HasSubstr("BLOCKYSIZE"));
  EXPECT_THAT(creation_options, HasSubstr("GMLJP2V2_DEF"));
  EXPECT_THAT(creation_options, HasSubstr("ROI"));
  EXPECT_THAT(creation_options, HasSubstr("RATE"));
  EXPECT_THAT(creation_options, HasSubstr("Creversible"));
}

TEST(Jp2kakdrivercoreTest, IdentifyDoesNotExistAndShortFiles) {
  EXPECT_EQ(FALSE,
            Identify("/vsimem/jp2kakdrivercore_test/does_not_exist.jp2"));

  constexpr char kEmptyPath[] = "/vsimem/jp2kakdrivercore_test/empty.jp2";
  VsiMemTempWrapper empty_file(kEmptyPath, "");
  EXPECT_EQ(FALSE, Identify(kEmptyPath));

  // 11 bytes (< sizeof(jp2_header) == 12), even with JPC magic prefix.
  constexpr std::array<uint8_t, 11> kShortJpc = {
      0xff, 0x4f, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  constexpr char kShortPath[] = "/vsimem/jp2kakdrivercore_test/short.jpc";
  VsiMemTempWrapper short_file(kShortPath, BytesToString(kShortJpc));
  EXPECT_EQ(FALSE, Identify(kShortPath));
}

TEST(Jp2kakdrivercoreTest, IdentifyShortHeaderSpecialPrefixes) {
  struct PrefixCase {
    const char* filename;
    int expected;
  };
  constexpr PrefixCase kCases[] = {
      // USE_JPIP is disabled in google3 builds, so JPIP/HTTP URLs return FALSE.
      {.filename = "http://localhost/a.jp2", .expected = FALSE},
      {.filename = "https://example.test/a.JP2", .expected = FALSE},
      {.filename = "jpip://localhost/a.jp2", .expected = FALSE},
      // Non-jp2 extension on HTTP URL also returns FALSE.
      {.filename = "http://localhost/a.tif", .expected = FALSE},
      // J2K_SUBFILE: prefix is recognized case-insensitively before header I/O.
      {.filename = "J2K_SUBFILE:0,100,/vsimem/container.bin", .expected = TRUE},
      {.filename = "j2k_subfile:128,512,/vsimem/container.bin",
       .expected = TRUE},
      // Unrecognized virtual prefix without a backing file returns FALSE.
      {.filename = "J2K_UNKNOWN:0,100,/vsimem/container.bin",
       .expected = FALSE},
  };

  for (const PrefixCase& tc : kCases) {
    EXPECT_EQ(tc.expected, Identify(tc.filename)) << tc.filename;
  }
}

TEST(Jp2kakdrivercoreTest, IdentifyJp2Signature) {
  // Exact 12-byte JP2 signature box header matches regardless of extension.
  constexpr std::array<uint8_t, 12> kExactJp2 = {
      0x00, 0x00, 0x00, 0x0c, 0x6a, 0x50, 0x20, 0x20, 0x0d, 0x0a, 0x87, 0x0a};
  static_assert(kExactJp2.size() == sizeof(jp2_header));

  constexpr const char* kPaths[] = {
      "/vsimem/jp2kakdrivercore_test/no_extension",
      "/vsimem/jp2kakdrivercore_test/image.jp2",
      "/vsimem/jp2kakdrivercore_test/image.jpx",
      "/vsimem/jp2kakdrivercore_test/image.arbitrary",
  };
  const std::string payload = BytesToString(kExactJp2);

  for (const char* path : kPaths) {
    VsiMemTempWrapper wrapper(path, payload);
    EXPECT_EQ(TRUE, Identify(path)) << path;
  }
}

TEST(Jp2kakdrivercoreTest, IdentifyJpcSignatureAndExtensions) {
  // 16-byte buffer starting with JPC SOC + SIZ markers (0xFF 0x4F 0xFF 0x51).
  constexpr std::array<uint8_t, 16> kJpcBytes = {
      0xff, 0x4f, 0xff, 0x51, 0x00, 0x2f, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10};
  const std::string jpc_payload = BytesToString(kJpcBytes);

  constexpr const char* kValidExts[] = {"jpc", "JPC", "j2k", "J2K",
                                        "jp2", "JP2", "jpx", "JPX",
                                        "j2c", "J2C", "jhc", "JHC"};
  for (const char* ext : kValidExts) {
    const std::string path =
        std::format("/vsimem/jp2kakdrivercore_test/codestream.{}", ext);
    VsiMemTempWrapper wrapper(path, jpc_payload);
    EXPECT_EQ(TRUE, Identify(path.c_str())) << path;
  }

  // /vsisubfile/ paths containing "vsisubfile" match JPC headers even with
  // unrelated file extensions.
  constexpr char kBackingPath[] = "/vsimem/jp2kakdrivercore_test/backing.bin";
  VsiMemTempWrapper backing(kBackingPath, jpc_payload);
  const std::string vsisubfile_path =
      std::format("/vsisubfile/0_16,{}", kBackingPath);
  EXPECT_EQ(TRUE, Identify(vsisubfile_path.c_str()));

  // JPC header with non-JPEG2000 extension and no "vsisubfile" returns FALSE.
  constexpr char kBadExtPath[] = "/vsimem/jp2kakdrivercore_test/codestream.tif";
  VsiMemTempWrapper bad_ext(kBadExtPath, jpc_payload);
  EXPECT_EQ(FALSE, Identify(kBadExtPath));

  // Non-JPEG2000 content (>= 12 bytes) with .jp2 extension returns FALSE.
  constexpr char kGarbagePath[] = "/vsimem/jp2kakdrivercore_test/garbage.jp2";
  VsiMemTempWrapper garbage(kGarbagePath, "NOT_JPEG2000_DATA");
  EXPECT_EQ(FALSE, Identify(kGarbagePath));
}

}  // namespace
}  // namespace autotest2
