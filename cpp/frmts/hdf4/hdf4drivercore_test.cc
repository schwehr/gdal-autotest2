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

#include "third_party/gdal/frmts/hdf4/hdf4drivercore.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdalsubdatasetinfo.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

TEST(Hdf4DriverCoreTest, IdentifyNonExistentAndShortFiles) {
  GDALOpenInfo missing_info("/vsimem/does_not_exist.hdf", GDAL_OF_READONLY);
  EXPECT_EQ(HDF4DatasetIdentify(&missing_info), FALSE);

  const char kEmptyFile[] = "/vsimem/hdf4drivercore_empty.hdf";
  VsiMemTempWrapper empty_file(kEmptyFile, "");
  GDALOpenInfo empty_info(kEmptyFile, GDAL_OF_READONLY);
  EXPECT_EQ(HDF4DatasetIdentify(&empty_info), FALSE);

  const char kShortFile[] = "/vsimem/hdf4drivercore_short.hdf";
  VsiMemTempWrapper short_file(kShortFile, std::string("\x0e\x03\x13", 3));
  GDALOpenInfo short_info(kShortFile, GDAL_OF_READONLY);
  EXPECT_EQ(HDF4DatasetIdentify(&short_info), FALSE);
}

TEST(Hdf4DriverCoreTest, IdentifyMagicHeader) {
  const char kNonHdf4[] = "/vsimem/hdf4drivercore_other.hdf";
  VsiMemTempWrapper non_hdf4(kNonHdf4, "NOTHDF");
  GDALOpenInfo non_hdf4_info(kNonHdf4, GDAL_OF_READONLY);
  EXPECT_EQ(HDF4DatasetIdentify(&non_hdf4_info), FALSE);

  const char kValidHdf4[] = "/vsimem/hdf4drivercore_valid.hdf";
  VsiMemTempWrapper valid_hdf4(kValidHdf4,
                               std::string("\x0e\x03\x13\x01\x00\x00", 6));
  GDALOpenInfo valid_hdf4_info(kValidHdf4, GDAL_OF_READONLY);
  EXPECT_EQ(HDF4DatasetIdentify(&valid_hdf4_info), TRUE);
}

TEST(Hdf4DriverCoreTest, ImageDatasetIdentify) {
  const char* kMatchingNames[] = {
      "HDF4_SDS:UNKNOWN:/vsimem/test.hdf:0",
      "hdf4_sds:unknown:/vsimem/test.hdf:0",
      "HDF4_GR:/vsimem/test.hdf:0",
      "hdf4_gr:/vsimem/test.hdf:0",
      "HDF4_GD:/vsimem/test.hdf:Grid1:Field1",
      "hdf4_gd:/vsimem/test.hdf:Grid1:Field1",
      "HDF4_EOS:EOS_SWATH:/vsimem/test.hdf:Swath1:Field1",
      "hdf4_eos:eos_grid:/vsimem/test.hdf:Grid1:Field1",
  };
  for (const char* name : kMatchingNames) {
    GDALOpenInfo open_info(name, GDAL_OF_READONLY);
    EXPECT_EQ(HDF4ImageDatasetIdentify(&open_info), TRUE) << name;
  }

  const char* kNonMatchingNames[] = {
      "/vsimem/test.hdf",
      "HDF4_OTHER:/vsimem/test.hdf:0",
      "NETCDF:/vsimem/test.nc:var",
      "",
  };
  for (const char* name : kNonMatchingNames) {
    GDALOpenInfo open_info(name, GDAL_OF_READONLY);
    EXPECT_EQ(HDF4ImageDatasetIdentify(&open_info), FALSE) << name;
  }
}

TEST(Hdf4DriverCoreTest, SetCommonMetadata) {
  GDALDriver hdf4_driver;
  HDF4DriverSetCommonMetadata(&hdf4_driver);
  EXPECT_STREQ(hdf4_driver.GetDescription(), HDF4_DRIVER_NAME);
  EXPECT_STREQ(hdf4_driver.GetMetadataItem(GDAL_DCAP_RASTER), "YES");
  EXPECT_STREQ(hdf4_driver.GetMetadataItem(GDAL_DMD_LONGNAME),
               "Hierarchical Data Format Release 4");
  EXPECT_STREQ(hdf4_driver.GetMetadataItem(GDAL_DMD_HELPTOPIC),
               "drivers/raster/hdf4.html");
  EXPECT_STREQ(hdf4_driver.GetMetadataItem(GDAL_DMD_EXTENSION), "hdf");
  EXPECT_STREQ(hdf4_driver.GetMetadataItem(GDAL_DMD_SUBDATASETS), "YES");
  EXPECT_STREQ(hdf4_driver.GetMetadataItem(GDAL_DCAP_MULTIDIM_RASTER), "YES");
  EXPECT_NE(hdf4_driver.GetMetadataItem(GDAL_DMD_OPENOPTIONLIST), nullptr);
  EXPECT_EQ(hdf4_driver.pfnIdentify, HDF4DatasetIdentify);
  EXPECT_STREQ(hdf4_driver.GetMetadataItem(GDAL_DCAP_OPEN), "YES");
  EXPECT_NE(hdf4_driver.pfnGetSubdatasetInfoFunc, nullptr);

  GDALDriver hdf4_image_driver;
  HDF4ImageDriverSetCommonMetadata(&hdf4_image_driver);
  EXPECT_STREQ(hdf4_image_driver.GetDescription(), HDF4_IMAGE_DRIVER_NAME);
  EXPECT_STREQ(hdf4_image_driver.GetMetadataItem(GDAL_DCAP_RASTER), "YES");
  EXPECT_STREQ(hdf4_image_driver.GetMetadataItem(GDAL_DMD_LONGNAME),
               "HDF4 Dataset");
  EXPECT_STREQ(hdf4_image_driver.GetMetadataItem(GDAL_DMD_HELPTOPIC),
               "drivers/raster/hdf4.html");
  EXPECT_STREQ(hdf4_image_driver.GetMetadataItem(GDAL_DMD_CREATIONDATATYPES),
               "Byte Int8 Int16 UInt16 Int32 UInt32 Float32 Float64");
  EXPECT_NE(hdf4_image_driver.GetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST),
            nullptr);
  EXPECT_STREQ(hdf4_image_driver.GetMetadataItem(GDAL_DCAP_OPEN), "YES");
  EXPECT_EQ(hdf4_image_driver.pfnIdentify, HDF4ImageDatasetIdentify);
  EXPECT_STREQ(hdf4_image_driver.GetMetadataItem(GDAL_DCAP_CREATE), "YES");
}

TEST(Hdf4DriverCoreTest, GetSubdatasetInfo) {
  GDALDriver driver;
  HDF4DriverSetCommonMetadata(&driver);
  ASSERT_NE(driver.pfnGetSubdatasetInfoFunc, nullptr);

  auto parse = [&driver](const char* filename) {
    return std::unique_ptr<GDALSubdatasetInfo>(
        driver.pfnGetSubdatasetInfoFunc(filename));
  };

  // Invalid / incomplete subdataset descriptors.
  EXPECT_EQ(parse("/vsimem/plain.hdf"), nullptr);
  EXPECT_EQ(parse("HDF4_GR:/vsimem/plain.hdf:0"), nullptr);
  EXPECT_EQ(parse("HDF4_SDS:ONLY_TWO"), nullptr);
  EXPECT_EQ(parse("HDF4_SDS:UNKNOWN:/vsimem/no_subdataset.hdf"), nullptr);
  EXPECT_EQ(parse("HDF4_SDS:UNKNOWN:C:/missing_subdataset.hdf"), nullptr);

  // Standard Unix path with single subdataset token.
  {
    auto info = parse("HDF4_SDS:UNKNOWN:/vsimem/test.hdf:0");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->GetPathComponent(), "/vsimem/test.hdf");
    EXPECT_EQ(info->GetSubdatasetComponent(), "0");
    EXPECT_EQ(info->ModifyPathComponent("/vsimem/other.hdf"),
              "HDF4_SDS:UNKNOWN:/vsimem/other.hdf:0");
  }

  // Case-insensitive prefix and multi-colon subdataset component.
  {
    auto info = parse("hdf4_eos:EOS_GRID:/vsimem/test.hdf:Grid1:Field:Sub");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->GetPathComponent(), "/vsimem/test.hdf");
    EXPECT_EQ(info->GetSubdatasetComponent(), "Grid1:Field:Sub");
  }

  // Windows drive letter (1-char unquoted, forward and backslash).
  {
    auto info = parse("HDF4_SDS:UNKNOWN:C:/data/test.hdf:1");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->GetPathComponent(), "C:/data/test.hdf");
    EXPECT_EQ(info->GetSubdatasetComponent(), "1");
  }
  {
    auto info = parse("HDF4_SDS:UNKNOWN:D:\\data\\test.hdf:2");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->GetPathComponent(), "D:\\data\\test.hdf");
    EXPECT_EQ(info->GetSubdatasetComponent(), "2");
  }

  // Windows drive letter with leading quote (2-char token before colon).
  {
    auto info = parse("HDF4_SDS:UNKNOWN:\"C:/data/test.hdf\":3");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->GetPathComponent(), "C:/data/test.hdf");
    EXPECT_EQ(info->GetSubdatasetComponent(), "3");
  }

  // /vsicurl/ protocol path containing a colon.
  {
    auto info =
        parse("HDF4_SDS:UNKNOWN:/vsicurl/https://example.com/test.hdf:4");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->GetPathComponent(),
              "/vsicurl/https://example.com/test.hdf");
    EXPECT_EQ(info->GetSubdatasetComponent(), "4");
  }

  // Registered driver manager lookup via GDALGetSubdatasetInfo.
  GDALAllRegister();
  GDALSubdatasetInfoH h_info =
      GDALGetSubdatasetInfo("HDF4_EOS:EOS_SWATH:/vsimem/swath.hdf:Swath1:Band");
  ASSERT_NE(h_info, nullptr);
  char* path_comp = GDALSubdatasetInfoGetPathComponent(h_info);
  EXPECT_STREQ(path_comp, "/vsimem/swath.hdf");
  CPLFree(path_comp);
  GDALDestroySubdatasetInfo(h_info);
}

}  // namespace
}  // namespace autotest2
