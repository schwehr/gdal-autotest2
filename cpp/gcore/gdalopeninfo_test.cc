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
//
// Tests gcore/gdalopeninfo.cpp.

#include <cstring>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

void GDALOpenInfoDeclareFileNotToOpen(const char* pszFilename,
                                      const GByte* pabyHeader,
                                      int nHeaderBytes);
void GDALOpenInfoUnDeclareFileNotToOpen(const char* pszFilename);

namespace {

TEST(GdalopeninfoTest, FileNotToOpenAndOpenInfoVariants) {
  const std::string dir = "/vsimem/openinfo_test_dir";
  const std::string file1 = dir + "/test1.bin";
  const std::string file2 = dir + "/test2.bin";
  VSIMkdir(dir.c_str(), 0755);

  const std::string content(2048, 'A');
  VSILFILE* fp = VSIFOpenL(file1.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  VSIFWriteL(content.data(), 1, content.size(), fp);
  VSIFCloseL(fp);

  fp = VSIFOpenL(file2.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  VSIFWriteL("hello", 1, 5, fp);
  VSIFCloseL(fp);

  // 1. DeclareFileNotToOpen (including refcount > 1) and
  // UnDeclareFileNotToOpen.
  const GByte fake_hdr[] = "SQLite format 3";
  GDALOpenInfoDeclareFileNotToOpen(file1.c_str(), fake_hdr, sizeof(fake_hdr));
  GDALOpenInfoDeclareFileNotToOpen(file1.c_str(), fake_hdr, sizeof(fake_hdr));
  {
    GDALOpenInfo info_fnto(file1.c_str(), GDAL_OF_READONLY);
    EXPECT_TRUE(info_fnto.bStatOK);
    EXPECT_EQ(nullptr, info_fnto.fpL);
    ASSERT_NE(nullptr, info_fnto.pabyHeader);
    EXPECT_STREQ("SQLite format 3",
                 reinterpret_cast<const char*>(info_fnto.pabyHeader));
    EXPECT_FALSE(info_fnto.TryToIngest(4096));

    // Opening a DIFFERENT file while pMapFNTO is non-null hits oIter ==
    // pMapFNTO->end().
    GDALOpenInfo info_other(file2.c_str(), GDAL_OF_READONLY);
    EXPECT_TRUE(info_other.bStatOK);
  }
  GDALOpenInfoUnDeclareFileNotToOpen(file1.c_str());
  GDALOpenInfoUnDeclareFileNotToOpen(file1.c_str());

  // 2. Normal file open + TryToIngest (both when all bytes already read and
  // when more bytes are available), plus GDAL_INGESTED_BYTES_AT_OPEN < 1024 and
  // > 10MB.
  {
    CPLSetThreadLocalConfigOption("GDAL_INGESTED_BYTES_AT_OPEN", "512");
    GDALOpenInfo info(file1.c_str(), GDAL_OF_READONLY);
    CPLSetThreadLocalConfigOption("GDAL_INGESTED_BYTES_AT_OPEN", nullptr);
    EXPECT_TRUE(info.bStatOK);
    EXPECT_FALSE(info.bIsDirectory);
    ASSERT_NE(nullptr, info.fpL);
    EXPECT_EQ(1024, info.nHeaderBytes);
    EXPECT_TRUE(info.TryToIngest(2048));
    EXPECT_EQ(2048, info.nHeaderBytes);
    // Second TryToIngest when nHeaderBytes < nHeaderBytesTried returns TRUE
    // early.
    EXPECT_TRUE(info.TryToIngest(4096));

    CPLSetThreadLocalConfigOption("GDAL_INGESTED_BYTES_AT_OPEN", "20000000");
    GDALOpenInfo info_large(file2.c_str(), GDAL_OF_READONLY);
    CPLSetThreadLocalConfigOption("GDAL_INGESTED_BYTES_AT_OPEN", nullptr);
    EXPECT_EQ(5, info_large.nHeaderBytes);
  }

  // 3. Sibling files: lazy loading, StealSiblingFiles, explicit siblings, and
  // GDAL_DISABLE_READDIR_ON_OPEN=EMPTY_DIR / YES.
  {
    // Clear VSISiblingFiles cache if any, and test
    // GetSiblingFiles/StealSiblingFiles.
    CPLSetThreadLocalConfigOption("GDAL_READDIR_LIMIT_ON_OPEN", "1");
    GDALOpenInfo info_limit(file1.c_str(), GDAL_OF_READONLY);
    // With 2 files in dir and limit=1, VSIReadDirEx exceeds limit and returns
    // nullptr.
    EXPECT_EQ(nullptr, info_limit.GetSiblingFiles());
    CPLSetThreadLocalConfigOption("GDAL_READDIR_LIMIT_ON_OPEN", nullptr);

    GDALOpenInfo info_siblings(file1.c_str(), GDAL_OF_READONLY);
    char** stolen = info_siblings.StealSiblingFiles();
    EXPECT_TRUE(info_siblings.AreSiblingFilesLoaded());
    CSLDestroy(stolen);

    const char* explicit_siblings[] = {"test2.bin", nullptr};
    GDALOpenInfo info_explicit(file1.c_str(), GDAL_OF_READONLY,
                               explicit_siblings);
    EXPECT_TRUE(info_explicit.AreSiblingFilesLoaded());
    ASSERT_NE(nullptr, info_explicit.GetSiblingFiles());

    CPLSetThreadLocalConfigOption("GDAL_DISABLE_READDIR_ON_OPEN", "EMPTY_DIR");
    GDALOpenInfo info_empty_dir(file1.c_str(), GDAL_OF_READONLY);
    EXPECT_TRUE(info_empty_dir.AreSiblingFilesLoaded());
    ASSERT_NE(nullptr, info_empty_dir.GetSiblingFiles());

    CPLSetThreadLocalConfigOption("GDAL_DISABLE_READDIR_ON_OPEN", "YES");
    GDALOpenInfo info_disable(file1.c_str(), GDAL_OF_READONLY);
    EXPECT_TRUE(info_disable.AreSiblingFilesLoaded());
    EXPECT_EQ(nullptr, info_disable.GetSiblingFiles());
    CPLSetThreadLocalConfigOption("GDAL_DISABLE_READDIR_ON_OPEN", nullptr);
  }

  // 4. Directory, MVT:/vsi, /vsizip/, and IsSingleAllowedDriver.
  {
    GDALOpenInfo info_dir(dir.c_str(), GDAL_OF_READONLY);
    EXPECT_TRUE(info_dir.bStatOK);
    EXPECT_TRUE(info_dir.bIsDirectory);

    GDALOpenInfo info_mvt("MVT:/vsimem/foo", GDAL_OF_READONLY);
    EXPECT_FALSE(info_mvt.bStatOK);

    GDALOpenInfo info_zip("/vsizip//vsimem/nonexistent.zip",
                          GDAL_OF_READONLY | GDAL_OF_VERBOSE_ERROR);
    EXPECT_FALSE(info_zip.bStatOK);

    // Valid zip archive on /vsimem/ to hit bPotentialDirectory VSIStatExL == 0.
    // Note: a zip with > 1 file is treated as a directory by /vsizip/.
    const std::string zip_path = dir + "/valid.zip";
    void* h_zip = CPLCreateZip(zip_path.c_str(), nullptr);
    ASSERT_NE(nullptr, h_zip);
    EXPECT_EQ(CE_None, CPLCreateFileInZip(h_zip, "dummy1.txt", nullptr));
    EXPECT_EQ(CE_None, CPLWriteFileInZip(h_zip, "abc", 3));
    EXPECT_EQ(CE_None, CPLCloseFileInZip(h_zip));
    EXPECT_EQ(CE_None, CPLCreateFileInZip(h_zip, "dummy2.txt", nullptr));
    EXPECT_EQ(CE_None, CPLWriteFileInZip(h_zip, "def", 3));
    EXPECT_EQ(CE_None, CPLCloseFileInZip(h_zip));
    CPLCloseZip(h_zip);
    GDALOpenInfo info_valid_zip(("/vsizip/" + zip_path).c_str(),
                                GDAL_OF_READONLY);
    EXPECT_TRUE(info_valid_zip.bStatOK);
    EXPECT_TRUE(info_valid_zip.bIsDirectory);
    VSIUnlink(zip_path.c_str());

    GDALOpenInfo info_curl("/vsicurl/http://127.0.0.1:1/nonexistent.tif",
                           GDAL_OF_READONLY);

    const char* single_drv[] = {"GTiff", nullptr};
    const char* multi_drv[] = {"GTiff", "MEM", nullptr};
    info_dir.papszAllowedDrivers = single_drv;
    EXPECT_TRUE(info_dir.IsSingleAllowedDriver("GTiff"));
    EXPECT_FALSE(info_dir.IsSingleAllowedDriver("MEM"));
    info_dir.papszAllowedDrivers = multi_drv;
    EXPECT_FALSE(info_dir.IsSingleAllowedDriver("GTiff"));
  }

  VSIUnlink(file1.c_str());
  VSIUnlink(file2.c_str());
  VSIRmdir(dir.c_str());
}

}  // namespace
