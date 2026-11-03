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
// Tests for gribdrivercore (gribdrivercore.cpp).
// Note: google3 builds GDAL without ENABLE_TDLP (TDLP identification disabled).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/grib/gribdrivercore.cpp
//   https://gdal.org/drivers/raster/grib.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/grib.py

#include "third_party/gdal/frmts/grib/gribdrivercore.h"

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

constexpr char kVsiDir[] = "/vsimem/gribdrivercore_test";
constexpr auto kBinaryGribHeader =
    std::to_array<char>({'G', 'R', 'I', 'B', 0x00, 0x00, 0x00, 0x02});
static_assert(kBinaryGribHeader.size() == 8);

struct HeaderCase {
  std::string_view filename;
  std::string_view header;
  bool expected;
};

constexpr HeaderCase kHeaderCases[] = {
    {.filename = "empty.grb2", .header = "", .expected = false},
    {.filename = "short.grb2", .header = "GRIB123", .expected = false},
    {.filename = "std.grb2",
     .header =
         std::string_view(kBinaryGribHeader.data(), kBinaryGribHeader.size()),
     .expected = true},
    {.filename = "lower.grb2", .header = "grib0002", .expected = true},
    {.filename = "offset.grb2",
     .header = "PREAMBLE_GrIb_DATA",
     .expected = true},
    // "GRIB" right at the end of a 9-byte buffer (index 5 < 9 - 3).
    {.filename = "at_end.grb2", .header = "12345GRIB", .expected = true},
    // Truncated "GRI" at the end of an 8-byte buffer should not match.
    {.filename = "truncated.grb2", .header = "12345GRI", .expected = false},
    {.filename = "unrelated.bin",
     .header = "NETCDF_OR_TIFF_HEADER",
     .expected = false},
};

struct MetadataCase {
  const char* key;
  const char* expected;
};

constexpr MetadataCase kMetadataCases[] = {
    {.key = GDAL_DCAP_RASTER, .expected = "YES"},
    {.key = GDAL_DCAP_MULTIDIM_RASTER, .expected = "YES"},
    {.key = GDAL_DMD_LONGNAME, .expected = "GRIdded Binary (.grb, .grb2)"},
    {.key = GDAL_DMD_HELPTOPIC, .expected = "drivers/raster/grib.html"},
    {.key = GDAL_DMD_EXTENSIONS, .expected = "grb grb2 grib2"},
    {.key = GDAL_DCAP_VIRTUALIO, .expected = "YES"},
    {.key = GDAL_DCAP_CREATE_SUBDATASETS, .expected = "YES"},
    {.key = GDAL_DMD_CREATIONDATATYPES,
     .expected = "Byte UInt16 Int16 UInt32 Int32 Float32 Float64"},
    {.key = GDAL_DCAP_OPEN, .expected = "YES"},
    {.key = GDAL_DCAP_CREATECOPY, .expected = "YES"},
};

TEST(GribDriverCoreTest, IdentifyDoesNotExist) {
  GDALOpenInfo missing("/does_not_exist", GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(FALSE, GRIBDriverIdentify(&missing));
}

TEST(GribDriverCoreTest, IdentifyHeaderVariants) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };

  for (const auto& tc : kHeaderCases) {
    SCOPED_TRACE(tc.filename);
    const std::string path = std::format("{}/{}", kVsiDir, tc.filename);
    VsiMemTempWrapper file(path, std::string(tc.header));
    GDALOpenInfo info(path.c_str(), GDAL_OF_READONLY, nullptr);
    EXPECT_EQ(tc.expected ? TRUE : FALSE, GRIBDriverIdentify(&info));
  }
}

TEST(GribDriverCoreTest, SetCommonMetadata) {
  GDALDriver driver;
  GRIBDriverSetCommonMetadata(&driver);

  EXPECT_STREQ("GRIB", driver.GetDescription());
  for (const auto& tc : kMetadataCases) {
    SCOPED_TRACE(tc.key);
    EXPECT_STREQ(tc.expected, driver.GetMetadataItem(tc.key));
  }
  EXPECT_THAT(driver.GetMetadataItem(GDAL_DMD_OPENOPTIONLIST),
              HasSubstr("USE_IDX"));
  EXPECT_EQ(GRIBDriverIdentify, driver.pfnIdentify);
}

}  // namespace
}  // namespace autotest2
