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
// Tests the GDAL WEBP driver identification and core metadata
// (webpdrivercore.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/webp/webpdrivercore.cpp
//   https://gdal.org/drivers/raster/webp.html

#include "third_party/gdal/frmts/webp/webpdrivercore.h"

#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using namespace std::string_view_literals;

struct IdentifyCase {
  std::string_view name;
  std::string_view header;
  int expected_identify;
};

TEST(WebpDriverCoreTest, IdentifyDoesNotExist) {
  GDALOpenInfo open_info("/does_not_exist", GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(FALSE, WEBPDriverIdentify(&open_info));
}

TEST(WebpDriverCoreTest, IdentifyHeaderVariants) {
  constexpr IdentifyCase kIdentifyCases[] = {
      // Too short headers (< 20 bytes)
      {.name = "empty", .header = ""sv, .expected_identify = FALSE},
      {.name = "four_bytes", .header = "RIFF"sv, .expected_identify = FALSE},
      {.name = "twelve_bytes",
       .header = "RIFF\0\0\0\0WEBP"sv,
       .expected_identify = FALSE},
      {.name = "nineteen_bytes",
       .header = "RIFF\0\0\0\0WEBPVP8 \0\0\0"sv,
       .expected_identify = FALSE},
      // 20-byte valid headers
      {.name = "valid_vp8_lossy",
       .header = "RIFF\x20\0\0\0WEBPVP8 \0\0\0\0"sv,
       .expected_identify = TRUE},
      {.name = "valid_vp8l_lossless",
       .header = "RIFF\x20\0\0\0WEBPVP8L\0\0\0\0"sv,
       .expected_identify = TRUE},
      {.name = "valid_vp8x_extended",
       .header = "RIFF\x20\0\0\0WEBPVP8X\0\0\0\0"sv,
       .expected_identify = TRUE},
      // 20-byte invalid headers
      {.name = "invalid_riff_magic",
       .header = "RIFX\x20\0\0\0WEBPVP8 \0\0\0\0"sv,
       .expected_identify = FALSE},
      {.name = "invalid_webp_magic",
       .header = "RIFF\x20\0\0\0WAVEfmt \0\0\0\0"sv,
       .expected_identify = FALSE},
      {.name = "invalid_chunk_fourcc",
       .header = "RIFF\x20\0\0\0WEBPVP8?\0\0\0\0"sv,
       .expected_identify = FALSE},
      {.name = "unknown_chunk_fourcc",
       .header = "RIFF\x20\0\0\0WEBPXYZ \0\0\0\0"sv,
       .expected_identify = FALSE},
  };

  for (const auto &tc : kIdentifyCases) {
    const std::string filename =
        std::string("/vsimem/webpdrivercore_test/header_") +
        std::string(tc.name);
    VsiMemTempWrapper wrapper(filename, std::string(tc.header));
    GDALOpenInfo open_info(filename.c_str(), GDAL_OF_READONLY, nullptr);
    EXPECT_EQ(tc.expected_identify, WEBPDriverIdentify(&open_info))
        << "Failed for case: " << tc.name;
  }
}

TEST(WebpDriverCoreTest, SetCommonMetadata) {
  GDALDriver driver;
  WEBPDriverSetCommonMetadata(&driver);

  EXPECT_STREQ("WEBP", driver.GetDescription());
  EXPECT_STREQ("YES", driver.GetMetadataItem(GDAL_DCAP_RASTER));
  EXPECT_STREQ("WEBP", driver.GetMetadataItem(GDAL_DMD_LONGNAME));
  EXPECT_STREQ("drivers/raster/webp.html",
               driver.GetMetadataItem(GDAL_DMD_HELPTOPIC));
  EXPECT_STREQ("webp", driver.GetMetadataItem(GDAL_DMD_EXTENSION));
  EXPECT_STREQ("image/webp", driver.GetMetadataItem(GDAL_DMD_MIMETYPE));
  EXPECT_STREQ("Byte", driver.GetMetadataItem(GDAL_DMD_CREATIONDATATYPES));
  EXPECT_STREQ("YES", driver.GetMetadataItem(GDAL_DCAP_VIRTUALIO));
  EXPECT_STREQ("YES", driver.GetMetadataItem(
                          GDAL_DCAP_CREATE_ONLY_VISIBLE_AT_CLOSE_TIME));
  EXPECT_STREQ("YES", driver.GetMetadataItem(GDAL_DCAP_OPEN));
  EXPECT_STREQ("YES", driver.GetMetadataItem(GDAL_DCAP_CREATECOPY));

  const char *creation_options =
      driver.GetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST);
  ASSERT_NE(nullptr, creation_options);
  EXPECT_THAT(creation_options, HasSubstr("<CreationOptionList>"));

  constexpr std::string_view kCreationOptions[] = {
      "QUALITY",         "LOSSLESS",         "LOSSLESS_COPY",
      "PRESET",          "TARGETSIZE",       "PSNR",
      "METHOD",          "SEGMENTS",         "SNS_STRENGTH",
      "FILTER_STRENGTH", "FILTER_SHARPNESS", "FILTER_TYPE",
      "AUTOFILTER",      "PASS",             "PREPROCESSING",
      "PARTITIONS",      "PARTITION_LIMIT",  "EXACT",
      "WORLDFILE",
  };
  for (std::string_view opt : kCreationOptions) {
    EXPECT_THAT(creation_options,
                HasSubstr(std::string("name='").append(opt).append("'")));
  }

  ASSERT_NE(nullptr, driver.pfnIdentify);
  EXPECT_EQ(WEBPDriverIdentify, driver.pfnIdentify);
}

}  // namespace
}  // namespace autotest2
