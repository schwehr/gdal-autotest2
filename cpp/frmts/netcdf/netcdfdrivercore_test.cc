// Copyright 2026 Google Inc. All Rights Reserved.
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

#include "third_party/gdal/frmts/netcdf/netcdfdrivercore.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdalsubdatasetinfo.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

class NetCDFDriverCoreTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALAllRegister(); }
};

TEST_F(NetCDFDriverCoreTest, DriverSetCommonMetadata) {
  GDALDriver driver;
  netCDFDriverSetCommonMetadata(&driver);

  EXPECT_STREQ("netCDF", driver.GetDescription());
  EXPECT_STREQ("YES", driver.GetMetadataItem(GDAL_DCAP_RASTER));
  EXPECT_STREQ("YES", driver.GetMetadataItem(GDAL_DCAP_VECTOR));
  EXPECT_STREQ("Network Common Data Format",
               driver.GetMetadataItem(GDAL_DMD_LONGNAME));
  EXPECT_STREQ("nc", driver.GetMetadataItem(GDAL_DMD_EXTENSION));
  EXPECT_NE(nullptr, driver.pfnIdentify);
  EXPECT_NE(nullptr, driver.pfnGetSubdatasetInfoFunc);
}

TEST_F(NetCDFDriverCoreTest, IdentifyFormat) {
  // Prefix NETCDF:
  {
    GDALOpenInfo open_info("NETCDF:foo.nc:var", GDAL_OF_READONLY, nullptr);
    EXPECT_EQ(NCDF_FORMAT_UNKNOWN,
              netCDFIdentifyFormat(&open_info, /*bCheckExt=*/true));
  }

  // Header too short (< 4 bytes)
  {
    const char* vsimem_path = "/vsimem/short.nc";
    VSIFCloseL(VSIFileFromMemBuffer(
        vsimem_path, reinterpret_cast<GByte*>(const_cast<char*>("AB")), 2,
        FALSE));
    GDALOpenInfo open_info(vsimem_path, GDAL_OF_READONLY, nullptr);
    EXPECT_EQ(NCDF_FORMAT_NONE,
              netCDFIdentifyFormat(&open_info, /*bCheckExt=*/true));
    VSIUnlink(vsimem_path);
  }

  // Classic NetCDF (CDF1)
  {
    const char* vsimem_path = "/vsimem/classic.nc";
    const char data[] = "CDF\001extra_header_bytes";
    VSIFCloseL(VSIFileFromMemBuffer(
        vsimem_path, reinterpret_cast<GByte*>(const_cast<char*>(data)),
        sizeof(data), FALSE));
    GDALOpenInfo open_info(vsimem_path, GDAL_OF_READONLY, nullptr);
    EXPECT_EQ(NCDF_FORMAT_NC,
              netCDFIdentifyFormat(&open_info, /*bCheckExt=*/true));
    VSIUnlink(vsimem_path);
  }

  // 64-bit offset NetCDF (CDF2)
  {
    const char* vsimem_path = "/vsimem/offset64.nc2";
    const char data[] = "CDF\002extra_header_bytes";
    VSIFCloseL(VSIFileFromMemBuffer(
        vsimem_path, reinterpret_cast<GByte*>(const_cast<char*>(data)),
        sizeof(data), FALSE));
    GDALOpenInfo open_info(vsimem_path, GDAL_OF_READONLY, nullptr);
    EXPECT_EQ(NCDF_FORMAT_NC2,
              netCDFIdentifyFormat(&open_info, /*bCheckExt=*/true));
    VSIUnlink(vsimem_path);
  }

  // HDF5 signature at offset 0 with .nc extension
  {
    const char* vsimem_path = "/vsimem/nc4.nc";
    const char sig[] = "\211HDF\r\n\032\n";
    VSIFCloseL(VSIFileFromMemBuffer(
        vsimem_path, reinterpret_cast<GByte*>(const_cast<char*>(sig)), 8,
        FALSE));
    GDALOpenInfo open_info(vsimem_path, GDAL_OF_READONLY, nullptr);
    EXPECT_EQ(NCDF_FORMAT_NC4,
              netCDFIdentifyFormat(&open_info, /*bCheckExt=*/true));
    EXPECT_EQ(NCDF_FORMAT_NC4,
              netCDFIdentifyFormat(&open_info, /*bCheckExt=*/false));
    VSIUnlink(vsimem_path);
  }

  // HDF5 signature at offset 0 with .h5 extension (delegates to HDF5 when
  // bCheckExt=true)
  {
    const char* vsimem_path = "/vsimem/test.h5";
    const char sig[] = "\211HDF\r\n\032\n";
    VSIFCloseL(VSIFileFromMemBuffer(
        vsimem_path, reinterpret_cast<GByte*>(const_cast<char*>(sig)), 8,
        FALSE));
    GDALOpenInfo open_info(vsimem_path, GDAL_OF_READONLY, nullptr);
    ASSERT_NE(nullptr, GDALGetDriverByName("HDF5"));
    EXPECT_EQ(NCDF_FORMAT_HDF5,
              netCDFIdentifyFormat(&open_info, /*bCheckExt=*/true));
    EXPECT_EQ(NCDF_FORMAT_NC4,
              netCDFIdentifyFormat(&open_info, /*bCheckExt=*/false));
    VSIUnlink(vsimem_path);
  }

  // HDF5 signature at offset 0 with single allowed driver "netCDF"
  {
    const char* vsimem_path = "/vsimem/forced_netcdf.h5";
    const char sig[] = "\211HDF\r\n\032\n";
    VSIFCloseL(VSIFileFromMemBuffer(
        vsimem_path, reinterpret_cast<GByte*>(const_cast<char*>(sig)), 8,
        FALSE));
    const char* const allowed_drivers[] = {"netCDF", nullptr};
    GDALOpenInfo open_info(vsimem_path, GDAL_OF_READONLY, nullptr);
    open_info.papszAllowedDrivers = allowed_drivers;
    EXPECT_EQ(NCDF_FORMAT_NC4,
              netCDFIdentifyFormat(&open_info, /*bCheckExt=*/true));
    VSIUnlink(vsimem_path);
  }

  // HDF5 signature at offset 512 in buffer
  {
    const char* vsimem_path = "/vsimem/offset512.nc";
    std::vector<char> buf(600, 0);
    const char sig[] = "\211HDF\r\n\032\n";
    memcpy(&buf[512], sig, 8);
    VSIFCloseL(VSIFileFromMemBuffer(
        vsimem_path, reinterpret_cast<GByte*>(buf.data()), buf.size(), FALSE));
    GDALOpenInfo open_info(vsimem_path, GDAL_OF_READONLY, nullptr);
    EXPECT_EQ(NCDF_FORMAT_NC4,
              netCDFIdentifyFormat(&open_info, /*bCheckExt=*/true));
    VSIUnlink(vsimem_path);
  }

  // HDF5 signature at offset 1024 (requiring file seek)
  {
    const char* vsimem_path = "/vsimem/offset1024.nc";
    std::vector<char> buf(2048, 0);
    const char sig[] = "\211HDF\r\n\032\n";
    memcpy(&buf[1024], sig, 8);
    VSIFCloseL(VSIFileFromMemBuffer(
        vsimem_path, reinterpret_cast<GByte*>(buf.data()), buf.size(), FALSE));
    GDALOpenInfo open_info(vsimem_path, GDAL_OF_READONLY, nullptr);
    EXPECT_EQ(NCDF_FORMAT_NC4,
              netCDFIdentifyFormat(&open_info, /*bCheckExt=*/true));
    VSIUnlink(vsimem_path);
  }

  // HDF4 signature "\016\003\023\001". This build registers the HDF4 GDAL
  // driver (FRMT_hdf4) but libnetcdf has no HDF4 support (NETCDF_HAS_HDF4 is
  // not defined), so the file is delegated to the HDF4 driver when extension
  // checking is on and rejected outright otherwise.
  {
    const char* vsimem_path = "/vsimem/test.hdf";
    const char sig[] = "\016\003\023\001extra";
    VSIFCloseL(VSIFileFromMemBuffer(
        vsimem_path, reinterpret_cast<GByte*>(const_cast<char*>(sig)),
        sizeof(sig), FALSE));
    ASSERT_NE(nullptr, GDALGetDriverByName("HDF4"));
    GDALOpenInfo open_info(vsimem_path, GDAL_OF_READONLY, nullptr);
    EXPECT_EQ(NCDF_FORMAT_HDF4,
              netCDFIdentifyFormat(&open_info, /*bCheckExt=*/true));
    EXPECT_EQ(NCDF_FORMAT_NONE,
              netCDFIdentifyFormat(&open_info, /*bCheckExt=*/false));

    // Restricting to the netCDF driver does not change the outcome without
    // NETCDF_HAS_HDF4: the HDF4 driver availability check still wins.
    const char* const allowed_drivers[] = {"netCDF", nullptr};
    GDALOpenInfo open_info_netcdf(vsimem_path, GDAL_OF_READONLY, nullptr);
    open_info_netcdf.papszAllowedDrivers = allowed_drivers;
    EXPECT_EQ(NCDF_FORMAT_HDF4,
              netCDFIdentifyFormat(&open_info_netcdf, /*bCheckExt=*/true));
    VSIUnlink(vsimem_path);
  }

  // Unrecognized file
  {
    const char* vsimem_path = "/vsimem/unrecognized.bin";
    const char data[] = "XXXXYYYYZZZZ";
    VSIFCloseL(VSIFileFromMemBuffer(
        vsimem_path, reinterpret_cast<GByte*>(const_cast<char*>(data)),
        sizeof(data), FALSE));
    GDALOpenInfo open_info(vsimem_path, GDAL_OF_READONLY, nullptr);
    EXPECT_EQ(NCDF_FORMAT_NONE,
              netCDFIdentifyFormat(&open_info, /*bCheckExt=*/true));
    VSIUnlink(vsimem_path);
  }
}

TEST_F(NetCDFDriverCoreTest, DatasetIdentify) {
  GDALDriver driver;
  netCDFDriverSetCommonMetadata(&driver);
  ASSERT_NE(nullptr, driver.pfnIdentify);

  // NETCDF: prefix
  {
    GDALOpenInfo open_info("NETCDF:some_file:var", GDAL_OF_READONLY, nullptr);
    EXPECT_TRUE(driver.pfnIdentify(&open_info));
  }

  // NetCDF classic
  {
    const char* vsimem_path = "/vsimem/ident_classic.nc";
    const char data[] = "CDF\001header";
    VSIFCloseL(VSIFileFromMemBuffer(
        vsimem_path, reinterpret_cast<GByte*>(const_cast<char*>(data)),
        sizeof(data), FALSE));
    GDALOpenInfo open_info(vsimem_path, GDAL_OF_READONLY, nullptr);
    EXPECT_TRUE(driver.pfnIdentify(&open_info));
    VSIUnlink(vsimem_path);
  }

  // Not netCDF
  {
    const char* vsimem_path = "/vsimem/ident_other.bin";
    const char data[] = "UNKNOWN_DATA";
    VSIFCloseL(VSIFileFromMemBuffer(
        vsimem_path, reinterpret_cast<GByte*>(const_cast<char*>(data)),
        sizeof(data), FALSE));
    GDALOpenInfo open_info(vsimem_path, GDAL_OF_READONLY, nullptr);
    EXPECT_FALSE(driver.pfnIdentify(&open_info));
    VSIUnlink(vsimem_path);
  }
}

TEST_F(NetCDFDriverCoreTest, SubdatasetInfo) {
  GDALDriver driver;
  netCDFDriverSetCommonMetadata(&driver);
  ASSERT_NE(nullptr, driver.pfnGetSubdatasetInfoFunc);

  // Not NETCDF: prefix
  EXPECT_EQ(nullptr, driver.pfnGetSubdatasetInfoFunc("foo.nc"));

  // Not enough parts
  EXPECT_EQ(nullptr, driver.pfnGetSubdatasetInfoFunc("NETCDF:foo.nc"));

  // Standard 3 parts
  {
    std::unique_ptr<GDALSubdatasetInfo> info(
        driver.pfnGetSubdatasetInfoFunc("NETCDF:foo.nc:var"));
    ASSERT_NE(nullptr, info);
    EXPECT_EQ("foo.nc", info->GetPathComponent());
    EXPECT_EQ("var", info->GetSubdatasetComponent());
  }

  // Quoted parts
  {
    std::unique_ptr<GDALSubdatasetInfo> info(
        driver.pfnGetSubdatasetInfoFunc("NETCDF:\"foo.nc\":\"var\""));
    ASSERT_NE(nullptr, info);
    EXPECT_EQ("foo.nc", info->GetPathComponent());
    EXPECT_EQ("var", info->GetSubdatasetComponent());
  }

  // With drive letter: NETCDF:C:\foo\bar.nc:var
  {
    std::unique_ptr<GDALSubdatasetInfo> info(
        driver.pfnGetSubdatasetInfoFunc("NETCDF:C:\\foo\\bar.nc:var"));
    ASSERT_NE(nullptr, info);
    EXPECT_EQ("C:\\foo\\bar.nc", info->GetPathComponent());
    EXPECT_EQ("var", info->GetSubdatasetComponent());
  }

  // With /vsicurl/http protocol:
  // NETCDF:/vsicurl/http://example.com/foo.nc:var:extra
  {
    std::unique_ptr<GDALSubdatasetInfo> info(driver.pfnGetSubdatasetInfoFunc(
        "NETCDF:/vsicurl/http://example.com/foo.nc:var:extra"));
    ASSERT_NE(nullptr, info);
    EXPECT_EQ("/vsicurl/http://example.com/foo.nc", info->GetPathComponent());
    EXPECT_EQ("var:extra", info->GetSubdatasetComponent());
  }
}

}  // namespace
}  // namespace autotest2
