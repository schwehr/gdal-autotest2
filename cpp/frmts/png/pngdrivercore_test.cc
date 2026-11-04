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
// Tests for pngdrivercore (pngdrivercore.cpp).
//
// Caveats:
//   - DeclareDeferredPNGPlugin() is guarded by #ifdef PLUGIN_FILENAME, which
//     is not defined in Google3 builds.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/png/pngdrivercore.cpp
//   https://gdal.org/drivers/raster/png.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/png.py

#include "third_party/gdal/frmts/png/pngdrivercore.h"

#include <array>
#include <format>
#include <string>
#include <string_view>

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
using namespace std::string_view_literals;

constexpr char kVsiDir[] = "/vsimem/pngdrivercore_test";
constexpr auto kBinaryPngSignature = std::to_array<char>(
    {'\x89', 'P', 'N', 'G', '\x0d', '\x0a', '\x1a', '\x0a'});
static_assert(kBinaryPngSignature.size() == 8);

struct HeaderCase {
  std::string_view filename;
  std::string_view header;
  bool expected;
};

constexpr HeaderCase kHeaderCases[] = {
    {.filename = "empty.png", .header = "", .expected = false},
    {.filename = "short.png",
     .header = std::string_view(kBinaryPngSignature.data(), 7),
     .expected = false},
    {.filename = "exact.png",
     .header = std::string_view(kBinaryPngSignature.data(),
                                kBinaryPngSignature.size()),
     .expected = true},
    {.filename = "exact_with_extra.png",
     .header = "\x89PNG\x0d\x0a\x1a\x0a\x00\x00\x00\x0dIHDR"sv,
     .expected = true},
    {.filename = "mismatched.png",
     .header = "NOT_A_PN",
     .expected = false},
};

struct MetadataCase {
  const char* key;
  const char* expected;
};

constexpr MetadataCase kMetadataCases[] = {
    {.key = GDAL_DCAP_RASTER, .expected = "YES"},
    {.key = GDAL_DMD_LONGNAME, .expected = "Portable Network Graphics"},
    {.key = GDAL_DMD_HELPTOPIC, .expected = "drivers/raster/png.html"},
    {.key = GDAL_DMD_EXTENSION, .expected = "png"},
    {.key = GDAL_DMD_MIMETYPE, .expected = "image/png"},
    {.key = GDAL_DMD_CREATIONDATATYPES, .expected = "Byte UInt16"},
    {.key = GDAL_DCAP_VIRTUALIO, .expected = "YES"},
    {.key = GDAL_DCAP_CREATE_ONLY_VISIBLE_AT_CLOSE_TIME, .expected = "YES"},
    {.key = GDAL_DCAP_OPEN, .expected = "YES"},
    {.key = GDAL_DCAP_CREATECOPY, .expected = "YES"},
};

constexpr std::string_view kExpectedOptions[] = {
    "WORLDFILE",
    "ZLEVEL",
    "SOURCE_ICC_PROFILE",
    "SOURCE_ICC_PROFILE_NAME",
    "SOURCE_PRIMARIES_RED",
    "SOURCE_PRIMARIES_GREEN",
    "SOURCE_PRIMARIES_BLUE",
    "SOURCE_WHITEPOINT",
    "PNG_GAMMA",
    "TITLE",
    "DESCRIPTION",
    "COPYRIGHT",
    "COMMENT",
    "WRITE_METADATA_AS_TEXT",
    "NBITS",
};

TEST(PngDriverCoreTest, IdentifyDoesNotExist) {
  GDALOpenInfo missing("/does_not_exist", GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(FALSE, PNGDriverIdentify(&missing));
}

TEST(PngDriverCoreTest, IdentifyHeaderVariants) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };

  for (const auto& tc : kHeaderCases) {
    SCOPED_TRACE(tc.filename);
    const std::string path = std::format("{}/{}", kVsiDir, tc.filename);
    VsiMemTempWrapper file(path, std::string(tc.header));
    GDALOpenInfo info(path.c_str(), GDAL_OF_READONLY, nullptr);
    EXPECT_EQ(tc.expected ? TRUE : FALSE, PNGDriverIdentify(&info));
  }
}

TEST(PngDriverCoreTest, SetCommonMetadata) {
  GDALDriver driver;
  PNGDriverSetCommonMetadata(&driver);

  EXPECT_STREQ("PNG", driver.GetDescription());
  for (const auto& tc : kMetadataCases) {
    SCOPED_TRACE(tc.key);
    EXPECT_STREQ(tc.expected, driver.GetMetadataItem(tc.key));
  }
  const char* creation_options =
      driver.GetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST);
  ASSERT_NE(nullptr, creation_options);
  for (const auto opt : kExpectedOptions) {
    EXPECT_THAT(creation_options, HasSubstr(opt));
  }
  EXPECT_EQ(PNGDriverIdentify, driver.pfnIdentify);
}

// TODO: b/335317901 - DeclareDeferredPNGPlugin() is compiled out in Google3
// (requires PLUGIN_FILENAME definition). Add plugin loader test if dynamic
// driver loading is enabled in the future.

}  // namespace
}  // namespace autotest2
