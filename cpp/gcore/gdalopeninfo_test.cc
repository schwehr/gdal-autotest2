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
// Tests GDALOpenInfo (gcore/gdalopeninfo.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdalopeninfo.cpp

#include <cstddef>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_openinfo.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

// Internal helper symbols in gcore/gdalopeninfo.cpp not exported by a public
// header; keep in sync with third_party/gdal/gcore/gdalopeninfo.cpp.
void GDALOpenInfoDeclareFileNotToOpen(const char* pszFilename,
                                      const GByte* pabyHeader,
                                      int nHeaderBytes);
void GDALOpenInfoUnDeclareFileNotToOpen(const char* pszFilename);

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::UnorderedElementsAre;

constexpr char kTestDir[] = "/vsimem/gdalopeninfo_test";
constexpr char kFile1[] = "/vsimem/gdalopeninfo_test/test1.bin";
constexpr char kFile2[] = "/vsimem/gdalopeninfo_test/test2.bin";
constexpr char kZipPath[] = "/vsimem/gdalopeninfo_test/valid.zip";

TEST(GdalopeninfoTest, FileNotToOpenAndOpenInfoVariants) {
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };

  VsiMemTempWrapper file1(kFile1, std::string(2048, 'A'));
  VsiMemTempWrapper file2(kFile2, "hello");

  // 1. DeclareFileNotToOpen (including refcount > 1) and
  // UnDeclareFileNotToOpen.
  {
    constexpr std::string_view kFakeHdr = "SQLite format 3";
    GDALOpenInfoDeclareFileNotToOpen(
        kFile1, reinterpret_cast<const GByte*>(kFakeHdr.data()),
        static_cast<int>(kFakeHdr.size()));
    absl::Cleanup undeclare1 = [] {
      GDALOpenInfoUnDeclareFileNotToOpen(kFile1);
    };
    GDALOpenInfoDeclareFileNotToOpen(
        kFile1, reinterpret_cast<const GByte*>(kFakeHdr.data()),
        static_cast<int>(kFakeHdr.size()));
    absl::Cleanup undeclare2 = [] {
      GDALOpenInfoUnDeclareFileNotToOpen(kFile1);
    };

    GDALOpenInfo info_fnto(kFile1, GDAL_OF_READONLY);
    EXPECT_TRUE(info_fnto.bStatOK);
    EXPECT_EQ(nullptr, info_fnto.fpL);
    ASSERT_NE(nullptr, info_fnto.pabyHeader);
    EXPECT_STREQ("SQLite format 3",
                 reinterpret_cast<const char*>(info_fnto.pabyHeader));
    EXPECT_FALSE(info_fnto.TryToIngest(4096));

    // Opening a different file while pMapFNTO is non-null hits
    // oIter == pMapFNTO->end().
    GDALOpenInfo info_other(kFile2, GDAL_OF_READONLY);
    EXPECT_TRUE(info_other.bStatOK);
  }

  // 2. Normal file open + TryToIngest (both when all bytes already read and
  // when more bytes are available), plus GDAL_INGESTED_BYTES_AT_OPEN < 1024 and
  // > 10MB.
  {
    {
      CPLConfigOptionSetter opt_small("GDAL_INGESTED_BYTES_AT_OPEN", "512",
                                      /*bSetOnlyIfUndefined=*/false);
      GDALOpenInfo info(kFile1, GDAL_OF_READONLY);
      EXPECT_TRUE(info.bStatOK);
      EXPECT_FALSE(info.bIsDirectory);
      ASSERT_NE(nullptr, info.fpL);
      EXPECT_EQ(1024, info.nHeaderBytes);
      EXPECT_TRUE(info.TryToIngest(2048));
      EXPECT_EQ(2048, info.nHeaderBytes);
      // Second TryToIngest when nHeaderBytes < nHeaderBytesTried returns TRUE
      // early.
      EXPECT_TRUE(info.TryToIngest(4096));
    }

    {
      CPLConfigOptionSetter opt_large("GDAL_INGESTED_BYTES_AT_OPEN", "20000000",
                                      /*bSetOnlyIfUndefined=*/false);
      GDALOpenInfo info_large(kFile2, GDAL_OF_READONLY);
      EXPECT_EQ(5, info_large.nHeaderBytes);
    }
  }

  // 3. Sibling files: lazy loading, StealSiblingFiles, explicit siblings, and
  // GDAL_DISABLE_READDIR_ON_OPEN=EMPTY_DIR / YES.
  {
    {
      CPLConfigOptionSetter opt_limit("GDAL_READDIR_LIMIT_ON_OPEN", "1",
                                      /*bSetOnlyIfUndefined=*/false);
      GDALOpenInfo info_limit(kFile1, GDAL_OF_READONLY);
      // With 2 files in dir and limit=1, VSIReadDirEx exceeds limit and returns
      // nullptr.
      EXPECT_EQ(nullptr, info_limit.GetSiblingFiles());
    }

    GDALOpenInfo info_siblings(kFile1, GDAL_OF_READONLY);
    StringListPtr stolen(info_siblings.StealSiblingFiles(), CSLDestroy);
    EXPECT_TRUE(info_siblings.AreSiblingFilesLoaded());
    EXPECT_THAT(CslToVector(stolen.get()),
                UnorderedElementsAre("test1.bin", "test2.bin"));
    EXPECT_EQ(nullptr, info_siblings.GetSiblingFiles());

    const char* explicit_siblings[] = {"test2.bin", nullptr};
    GDALOpenInfo info_explicit(kFile1, GDAL_OF_READONLY, explicit_siblings);
    EXPECT_TRUE(info_explicit.AreSiblingFilesLoaded());
    ASSERT_NE(nullptr, info_explicit.GetSiblingFiles());
    EXPECT_THAT(CslToVector(info_explicit.GetSiblingFiles()),
                ElementsAre("test2.bin"));

    {
      CPLConfigOptionSetter opt_empty("GDAL_DISABLE_READDIR_ON_OPEN",
                                      "EMPTY_DIR",
                                      /*bSetOnlyIfUndefined=*/false);
      GDALOpenInfo info_empty_dir(kFile1, GDAL_OF_READONLY);
      EXPECT_TRUE(info_empty_dir.AreSiblingFilesLoaded());
      ASSERT_NE(nullptr, info_empty_dir.GetSiblingFiles());
      EXPECT_THAT(CslToVector(info_empty_dir.GetSiblingFiles()),
                  ElementsAre("test1.bin"));
    }

    {
      CPLConfigOptionSetter opt_disable("GDAL_DISABLE_READDIR_ON_OPEN", "YES",
                                        /*bSetOnlyIfUndefined=*/false);
      GDALOpenInfo info_disable(kFile1, GDAL_OF_READONLY);
      EXPECT_TRUE(info_disable.AreSiblingFilesLoaded());
      EXPECT_EQ(nullptr, info_disable.GetSiblingFiles());
    }
  }

  // 4. Directory, MVT:/vsi, /vsizip/, /vsicurl/, and IsSingleAllowedDriver.
  {
    GDALOpenInfo info_dir(kTestDir, GDAL_OF_READONLY);
    EXPECT_TRUE(info_dir.bStatOK);
    EXPECT_TRUE(info_dir.bIsDirectory);

    GDALOpenInfo info_mvt("MVT:/vsimem/gdalopeninfo_test/foo",
                          GDAL_OF_READONLY);
    EXPECT_FALSE(info_mvt.bStatOK);

    {
      WithQuietHandler quiet;
      CPLErrorReset();
      GDALOpenInfo info_zip("/vsizip//vsimem/gdalopeninfo_test/nonexistent.zip",
                            GDAL_OF_READONLY | GDAL_OF_VERBOSE_ERROR);
      EXPECT_FALSE(info_zip.bStatOK);
      CPLErrorReset();
    }

    // Valid zip archive on /vsimem/ to hit bPotentialDirectory VSIStatExL == 0.
    // Note: a zip with > 1 file is treated as a directory by /vsizip/.
    void* h_zip = CPLCreateZip(kZipPath, nullptr);
    ASSERT_NE(nullptr, h_zip);
    absl::Cleanup close_zip = [&] {
      if (h_zip != nullptr) {
        CPLCloseZip(h_zip);
      }
    };
    EXPECT_EQ(CE_None, CPLCreateFileInZip(h_zip, "dummy1.txt", nullptr));
    EXPECT_EQ(CE_None, CPLWriteFileInZip(h_zip, "abc", 3));
    EXPECT_EQ(CE_None, CPLCloseFileInZip(h_zip));
    EXPECT_EQ(CE_None, CPLCreateFileInZip(h_zip, "dummy2.txt", nullptr));
    EXPECT_EQ(CE_None, CPLWriteFileInZip(h_zip, "def", 3));
    EXPECT_EQ(CE_None, CPLCloseFileInZip(h_zip));
    EXPECT_EQ(CE_None, CPLCloseZip(h_zip));
    h_zip = nullptr;

    GDALOpenInfo info_valid_zip("/vsizip//vsimem/gdalopeninfo_test/valid.zip",
                                GDAL_OF_READONLY);
    EXPECT_TRUE(info_valid_zip.bStatOK);
    EXPECT_TRUE(info_valid_zip.bIsDirectory);

    // Curl is compiled out in google3; /vsicurl/ stat fails cleanly.
    GDALOpenInfo info_curl("/vsicurl/http://127.0.0.1:1/nonexistent.tif",
                           GDAL_OF_READONLY);
    EXPECT_FALSE(info_curl.bStatOK);

    const char* single_drv[] = {"GTiff", nullptr};
    const char* multi_drv[] = {"GTiff", "MEM", nullptr};
    info_dir.papszAllowedDrivers = single_drv;
    EXPECT_TRUE(info_dir.IsSingleAllowedDriver("GTiff"));
    EXPECT_FALSE(info_dir.IsSingleAllowedDriver("MEM"));
    info_dir.papszAllowedDrivers = multi_drv;
    EXPECT_FALSE(info_dir.IsSingleAllowedDriver("GTiff"));
  }
}

}  // namespace
}  // namespace autotest2
