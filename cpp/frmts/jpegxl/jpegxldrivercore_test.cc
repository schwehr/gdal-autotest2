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
// Tests for JPEG-XL driver core functions (jpegxldrivercore.cpp).
//
// Note: DeclareDeferredJPEGXLPlugin() is gated on #ifdef PLUGIN_FILENAME, which
// is not defined in the static google3 build. In addition, multithreading
// support (HAVE_JXL_THREADS) is disabled in google3, while box API
// (HAVE_JXL_BOX_API) and extra channels (HAVE_JxlEncoderInitExtraChannelInfo)
// are enabled.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/jpegxl/jpegxldrivercore.cpp
//   https://gdal.org/drivers/raster/jpegxl.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/jpegxl.py

#include "third_party/gdal/frmts/jpegxl/jpegxldrivercore.h"

#include <array>
#include <format>  // NOLINT
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
using ::testing::Not;
using ::testing::NotNull;

constexpr char kVsiDir[] = "/vsimem/jpegxldrivercore_test";

constexpr auto kJxlContainerSignature = std::to_array<char>({
    0x00, 0x00, 0x00, 0x0C, 'J', 'X', 'L', ' ', 0x0D, 0x0A,
    static_cast<char>(0x87), 0x0A,
});
static_assert(kJxlContainerSignature.size() == 12);

struct ContainerSignatureCase {
  std::string_view name;
  std::string_view header;
  bool expected;
};

constexpr ContainerSignatureCase kContainerSignatureCases[] = {
    {
        .name = "exact_12_bytes",
        .header = std::string_view(kJxlContainerSignature.data(),
                                   kJxlContainerSignature.size()),
        .expected = true,
    },
    {
        .name = "12_bytes_with_extra_payload",
        .header = std::string_view(
            "\x00\x00\x00\x0CJXL \x0D\x0A\x87\x0A\x00\x00\x00\x08" "ftyp", 20),
        .expected = true,
    },
    {
        .name = "truncated_11_bytes",
        .header = std::string_view(kJxlContainerSignature.data(), 11),
        .expected = false,
    },
    {
        .name = "truncated_2_bytes",
        .header = std::string_view(kJxlContainerSignature.data(), 2),
        .expected = false,
    },
    {
        .name = "empty",
        .header = std::string_view("", 0),
        .expected = false,
    },
    {
        .name = "mismatch_last_byte",
        .header = std::string_view("\x00\x00\x00\x0CJXL \x0D\x0A\x87\x0B", 12),
        .expected = false,
    },
    {
        .name = "mismatch_magic",
        .header = std::string_view("\x00\x00\x00\x0CPNG \x0D\x0A\x87\x0A", 12),
        .expected = false,
    },
};

struct MetadataCase {
  const char* key;
  const char* expected;
};

constexpr MetadataCase kMetadataCases[] = {
    {.key = GDAL_DCAP_RASTER, .expected = "YES"},
    {.key = GDAL_DMD_LONGNAME, .expected = "JPEG-XL"},
    {.key = GDAL_DMD_HELPTOPIC, .expected = "drivers/raster/jpegxl.html"},
    {.key = GDAL_DMD_EXTENSION, .expected = "jxl"},
    {.key = GDAL_DMD_MIMETYPE, .expected = "image/jxl"},
    {.key = GDAL_DMD_CREATIONDATATYPES, .expected = "Byte UInt16 Float32"},
    {.key = GDAL_DCAP_VIRTUALIO, .expected = "YES"},
    {.key = "JXL_ENCODER_SUPPORT_EXTRA_CHANNELS", .expected = "YES"},
    {.key = GDAL_DCAP_OPEN, .expected = "YES"},
    {.key = GDAL_DCAP_CREATECOPY, .expected = "YES"},
};

struct IdentifyCase {
  std::string_view filename;
  std::string_view header;
  int expected;
};

constexpr IdentifyCase kIdentifyCases[] = {
    {
        .filename = "test.jxl",
        .header = "arbitrary content for jxl extension",
        .expected = TRUE,
    },
    {
        .filename = "test.JXL",
        .header = "case insensitive check",
        .expected = TRUE,
    },
    {
        .filename = "raw_codestream.bin",
        .header = std::string_view("\xff\x0a\x00\x00", 4),
        .expected = GDAL_IDENTIFY_UNKNOWN,
    },
    {
        .filename = "raw_codestream_2bytes.bin",
        .header = std::string_view("\xff\x0a", 2),
        .expected = GDAL_IDENTIFY_UNKNOWN,
    },
    {
        .filename = "container.bin",
        .header = std::string_view(kJxlContainerSignature.data(),
                                   kJxlContainerSignature.size()),
        .expected = TRUE,
    },
    {
        .filename = "single_byte_ff.bin",
        .header = std::string_view("\xff", 1),
        .expected = FALSE,
    },
    {
        .filename = "unrelated.bin",
        .header = "some random non-jxl data",
        .expected = FALSE,
    },
    {
        .filename = "empty.bin",
        .header = "",
        .expected = FALSE,
    },
};

TEST(JpegxlDriverCoreTest, IsJPEGXLContainer) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };

  for (const auto& tc : kContainerSignatureCases) {
    SCOPED_TRACE(tc.name);
    const std::string path = std::format("{}/{}.bin", kVsiDir, tc.name);
    VsiMemTempWrapper file(path, std::string(tc.header));
    GDALOpenInfo info(path.c_str(), GDAL_OF_READONLY, nullptr);
    EXPECT_EQ(tc.expected, IsJPEGXLContainer(&info));
  }
}

TEST(JpegxlDriverCoreTest, SetCommonMetadata) {
  GDALDriver driver;
  JPEGXLDriverSetCommonMetadata(&driver);

  EXPECT_STREQ("JPEGXL", driver.GetDescription());
  for (const auto& tc : kMetadataCases) {
    SCOPED_TRACE(tc.key);
    EXPECT_STREQ(tc.expected, driver.GetMetadataItem(tc.key));
  }

  const char* open_options = driver.GetMetadataItem(GDAL_DMD_OPENOPTIONLIST);
  ASSERT_THAT(open_options, NotNull());
  EXPECT_THAT(open_options, HasSubstr("APPLY_ORIENTATION"));

  const char* creation_options =
      driver.GetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST);
  ASSERT_THAT(creation_options, NotNull());
  EXPECT_THAT(creation_options, HasSubstr("LOSSLESS"));
  EXPECT_THAT(creation_options, HasSubstr("EFFORT"));
  EXPECT_THAT(creation_options, HasSubstr("DISTANCE"));
  EXPECT_THAT(creation_options, HasSubstr("ALPHA_DISTANCE"));
  EXPECT_THAT(creation_options, HasSubstr("QUALITY"));
  EXPECT_THAT(creation_options, HasSubstr("NBITS"));
  EXPECT_THAT(creation_options, HasSubstr("SOURCE_ICC_PROFILE"));
  EXPECT_THAT(creation_options, HasSubstr("COMPRESS_BOXES"));
  // Multithreading option NUM_THREADS is disabled in google3 static build.
  EXPECT_THAT(creation_options, Not(HasSubstr("NUM_THREADS")));

  EXPECT_THAT(driver.pfnIdentify, NotNull());
}

TEST(JpegxlDriverCoreTest, IdentifyDoesNotExist) {
  GDALOpenInfo missing("/does_not_exist", GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(nullptr, missing.fpL);

  GDALDriver driver;
  JPEGXLDriverSetCommonMetadata(&driver);
  ASSERT_THAT(driver.pfnIdentify, NotNull());
  EXPECT_EQ(FALSE, driver.pfnIdentify(&missing));
}

TEST(JpegxlDriverCoreTest, IdentifyCases) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };

  GDALDriver driver;
  JPEGXLDriverSetCommonMetadata(&driver);
  ASSERT_THAT(driver.pfnIdentify, NotNull());

  for (const auto& tc : kIdentifyCases) {
    SCOPED_TRACE(tc.filename);
    const std::string path = std::format("{}/{}", kVsiDir, tc.filename);
    VsiMemTempWrapper file(path, std::string(tc.header));
    GDALOpenInfo info(path.c_str(), GDAL_OF_READONLY, nullptr);
    EXPECT_EQ(tc.expected, driver.pfnIdentify(&info));
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - DeclareDeferredJPEGXLPlugin is gated on #ifdef
// PLUGIN_FILENAME, so it cannot be tested in google3 static builds.
