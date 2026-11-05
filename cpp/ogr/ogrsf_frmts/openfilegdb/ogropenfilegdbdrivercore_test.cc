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
// Tests core driver identify and metadata for OpenFileGDB
// (ogropenfilegdbdrivercore.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/ogropenfilegdbdrivercore.cpp

#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/ogropenfilegdbdrivercore.h"

#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_minizip_zip.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/ogropenfilegdbdrivercore_test";

TEST(OgrOpenFileGdbDriverCoreTest, SetCommonMetadataAndDriverIdentifyCallback) {
  GDALDriver driver;
  OGROpenFileGDBDriverSetCommonMetadata(&driver);

  EXPECT_STREQ("OpenFileGDB", driver.GetDescription());
  EXPECT_STREQ("YES", driver.GetMetadataItem(GDAL_DCAP_RASTER));
  EXPECT_STREQ("YES", driver.GetMetadataItem(GDAL_DCAP_VECTOR));
  EXPECT_STREQ("gdb", driver.GetMetadataItem(GDAL_DMD_EXTENSION));
  EXPECT_STREQ("YES", driver.GetMetadataItem(GDAL_DCAP_NOTNULL_FIELDS));
  EXPECT_STREQ(
      "YES", driver.GetMetadataItem(GDAL_DCAP_HONOR_GEOM_COORDINATE_PRECISION));
  ASSERT_NE(nullptr, driver.pfnIdentify);

  // Test the 1-argument pfnIdentify callback
  GDALOpenInfo open_info("OpenFileGDB:/vsimem/foo.gdb:layer", GDAL_OF_RASTER);
  EXPECT_EQ(GDAL_IDENTIFY_TRUE, driver.pfnIdentify(&open_info));
}

TEST(OgrOpenFileGdbDriverCoreTest, IdentifyPrefixAndExtensions) {
  {
    GDALOpenInfo open_info("OpenFileGDB:/vsimem/foo.gdb:layer", GDAL_OF_RASTER);
    const char* filename = open_info.pszFilename;
    EXPECT_EQ(GDAL_IDENTIFY_TRUE,
              OGROpenFileGDBDriverIdentify(&open_info, filename));
  }
  {
    GDALOpenInfo open_info("/vsimem/test.gdb.zip", GDAL_OF_VECTOR);
    const char* filename = open_info.pszFilename;
    EXPECT_EQ(GDAL_IDENTIFY_TRUE,
              OGROpenFileGDBDriverIdentify(&open_info, filename));
  }
  {
    GDALOpenInfo open_info("/vsimem/test.gdb.tar", GDAL_OF_VECTOR);
    const char* filename = open_info.pszFilename;
    EXPECT_EQ(GDAL_IDENTIFY_TRUE,
              OGROpenFileGDBDriverIdentify(&open_info, filename));
  }
  {
    GDALOpenInfo open_info("/vsimem/sample_gdb.zip", GDAL_OF_VECTOR);
    const char* filename = open_info.pszFilename;
    EXPECT_EQ(GDAL_IDENTIFY_TRUE,
              OGROpenFileGDBDriverIdentify(&open_info, filename));
  }
  {
    GDALOpenInfo open_info("/vsimem/sample_GDB.zip", GDAL_OF_VECTOR);
    const char* filename = open_info.pszFilename;
    EXPECT_EQ(GDAL_IDENTIFY_TRUE,
              OGROpenFileGDBDriverIdentify(&open_info, filename));
  }
  {
    GDALOpenInfo open_info("/vsimem/a00000001.gdbtable", GDAL_OF_VECTOR);
    const char* filename = open_info.pszFilename;
    EXPECT_EQ(GDAL_IDENTIFY_TRUE,
              OGROpenFileGDBDriverIdentify(&open_info, filename));
  }
  {
    GDALOpenInfo open_info("/vsimem/unrelated.txt", GDAL_OF_VECTOR);
    const char* filename = open_info.pszFilename;
    EXPECT_EQ(GDAL_IDENTIFY_FALSE,
              OGROpenFileGDBDriverIdentify(&open_info, filename));
  }
  {
    GDALOpenInfo open_info(".", GDAL_OF_VECTOR);
    const char* filename = open_info.pszFilename;
    EXPECT_EQ(GDAL_IDENTIFY_FALSE,
              OGROpenFileGDBDriverIdentify(&open_info, filename));
  }
}

TEST(OgrOpenFileGdbDriverCoreTest, IdentifyGdbDirectoryAndNonDirectory) {
  absl::Cleanup cleanup_root = [] { VSIRmdirRecursive(kTestDir); };
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));

  // Non-existent directory
  {
    const std::string path = std::string(kTestDir) + "/missing.gdb";
    GDALOpenInfo open_info(path.c_str(), GDAL_OF_VECTOR);
    const char* filename = open_info.pszFilename;
    EXPECT_EQ(GDAL_IDENTIFY_FALSE,
              OGROpenFileGDBDriverIdentify(&open_info, filename));
  }

  // Regular file with .gdb extension (not a directory)
  {
    const std::string path = std::string(kTestDir) + "/regular_file.gdb";
    VsiMemTempWrapper file(path, "content");
    GDALOpenInfo open_info(path.c_str(), GDAL_OF_VECTOR);
    const char* filename = open_info.pszFilename;
    EXPECT_EQ(GDAL_IDENTIFY_FALSE,
              OGROpenFileGDBDriverIdentify(&open_info, filename));
  }

  // vsicurl github URL path (stat check branch)
  {
    GDALOpenInfo open_info("/vsicurl/https://github.com/org/repo/test.gdb",
                           GDAL_OF_VECTOR);
    const char* filename = open_info.pszFilename;
    EXPECT_EQ(GDAL_IDENTIFY_FALSE,
              OGROpenFileGDBDriverIdentify(&open_info, filename));
  }

  // Valid directory with .gdb
  const std::string gdb_dir = std::string(kTestDir) + "/valid.gdb";
  ASSERT_EQ(0, VSIMkdir(gdb_dir.c_str(), 0755));

  // Vector mode: recognized as GDB directory
  {
    GDALOpenInfo open_info(gdb_dir.c_str(), GDAL_OF_VECTOR);
    const char* filename = open_info.pszFilename;
    EXPECT_EQ(GDAL_IDENTIFY_TRUE,
              OGROpenFileGDBDriverIdentify(&open_info, filename));
  }
  // With trailing slash
  {
    const std::string gdb_dir_slash = gdb_dir + "/";
    GDALOpenInfo open_info(gdb_dir_slash.c_str(), GDAL_OF_VECTOR);
    const char* filename = open_info.pszFilename;
    EXPECT_EQ(GDAL_IDENTIFY_TRUE,
              OGROpenFileGDBDriverIdentify(&open_info, filename));
  }

  // Raster mode on directory without raster index files
  {
    const std::string cat_file = gdb_dir + "/a00000001.gdbtable";
    VsiMemTempWrapper cat_wrapper(cat_file, "dummy");
    GDALOpenInfo open_info(gdb_dir.c_str(), GDAL_OF_RASTER);
    const char* filename = open_info.pszFilename;
    EXPECT_EQ(GDAL_IDENTIFY_FALSE,
              OGROpenFileGDBDriverIdentify(&open_info, filename));
  }

  // Raster mode on directory with all 4 raster index files
  {
    const std::string f1 = gdb_dir + "/a00000009.band_index.atx";
    const std::string f2 = gdb_dir + "/a00000009.blk_key_index.atx";
    const std::string f3 = gdb_dir + "/a00000009.col_index.atx";
    const std::string f4 = gdb_dir + "/a00000009.row_index.atx";
    VsiMemTempWrapper w1(f1, "x");
    VsiMemTempWrapper w2(f2, "x");
    VsiMemTempWrapper w3(f3, "x");
    VsiMemTempWrapper w4(f4, "x");

    GDALOpenInfo open_info(gdb_dir.c_str(), GDAL_OF_RASTER);
    const char* filename = open_info.pszFilename;
    EXPECT_EQ(GDAL_IDENTIFY_TRUE,
              OGROpenFileGDBDriverIdentify(&open_info, filename));
  }
}

TEST(OgrOpenFileGdbDriverCoreTest, IdentifyVsiZipDirectory) {
  absl::Cleanup cleanup_root = [] { VSIRmdirRecursive(kTestDir); };
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));

  // Create zip file without a00000001.gdbtable (must have > 1 file so /vsizip/
  // treats it as a directory)
  const std::string zip1_path = std::string(kTestDir) + "/plain.zip";
  void* h_zip1 = CPLCreateZip(zip1_path.c_str(), nullptr);
  ASSERT_NE(nullptr, h_zip1);
  EXPECT_EQ(CE_None, CPLCreateFileInZip(h_zip1, "file1.txt", nullptr));
  EXPECT_EQ(CE_None, CPLWriteFileInZip(h_zip1, "abc", 3));
  EXPECT_EQ(CE_None, CPLCloseFileInZip(h_zip1));
  EXPECT_EQ(CE_None, CPLCreateFileInZip(h_zip1, "file2.txt", nullptr));
  EXPECT_EQ(CE_None, CPLWriteFileInZip(h_zip1, "def", 3));
  EXPECT_EQ(CE_None, CPLCloseFileInZip(h_zip1));
  EXPECT_EQ(CE_None, CPLCloseZip(h_zip1));

  const std::string zip1_dir = std::string("/vsizip/") + zip1_path;
  {
    GDALOpenInfo open_info(zip1_dir.c_str(), GDAL_OF_VECTOR);
    EXPECT_TRUE(open_info.bStatOK);
    EXPECT_TRUE(open_info.bIsDirectory);
    const char* filename = open_info.pszFilename;
    EXPECT_EQ(GDAL_IDENTIFY_FALSE,
              OGROpenFileGDBDriverIdentify(&open_info, filename));
  }

  // Create zip file with a00000001.gdbtable and a second file
  const std::string zip2_path = std::string(kTestDir) + "/with_cat.zip";
  void* h_zip2 = CPLCreateZip(zip2_path.c_str(), nullptr);
  ASSERT_NE(nullptr, h_zip2);
  EXPECT_EQ(CE_None, CPLCreateFileInZip(h_zip2, "a00000001.gdbtable", nullptr));
  EXPECT_EQ(CE_None, CPLWriteFileInZip(h_zip2, "cat", 3));
  EXPECT_EQ(CE_None, CPLCloseFileInZip(h_zip2));
  EXPECT_EQ(CE_None, CPLCreateFileInZip(h_zip2, "file2.txt", nullptr));
  EXPECT_EQ(CE_None, CPLWriteFileInZip(h_zip2, "xyz", 3));
  EXPECT_EQ(CE_None, CPLCloseFileInZip(h_zip2));
  EXPECT_EQ(CE_None, CPLCloseZip(h_zip2));

  const std::string zip2_dir = std::string("/vsizip/") + zip2_path;
  {
    GDALOpenInfo open_info(zip2_dir.c_str(), GDAL_OF_VECTOR);
    EXPECT_TRUE(open_info.bStatOK);
    EXPECT_TRUE(open_info.bIsDirectory);
    const char* filename = open_info.pszFilename;
    EXPECT_EQ(GDAL_IDENTIFY_TRUE,
              OGROpenFileGDBDriverIdentify(&open_info, filename));
  }
}

}  // namespace
}  // namespace autotest2
