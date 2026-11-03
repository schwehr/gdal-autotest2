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
// Tests GDAL PAM proxy database (gcore/gdalpamproxydb.cpp).
//
// Build caveat in google3: //third_party/gdal/BUILD compiles with PAM_ENABLED
// undefined, so GDAL_PAM_ENABLED defaults to "NO" unless overridden via
// CPLConfigOptionSetter.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdalpamproxydb.cpp

#include <cstddef>
#include <cstring>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal_pam.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdalpamproxydb_test";
constexpr char kProxyDir[] = "/vsimem/gdalpamproxydb_test/proxy";
constexpr char kDbFile[] =
    "/vsimem/gdalpamproxydb_test/proxy/gdal_pam_proxy.dat";
constexpr char kFailOpenDir[] = "/vsimem/gdalpamproxydb_test/fail_open";
constexpr char kFailOpenDbDir[] =
    "/vsimem/gdalpamproxydb_test/fail_open/gdal_pam_proxy.dat";
constexpr char kFailFsPrefix[] = "/vsipamproxydb_test_fail/";
constexpr char kFailFsDir[] = "/vsipamproxydb_test_fail";

class FailingPamProxyHandle final : public VSIVirtualHandle {
 public:
  explicit FailingPamProxyHandle(int mode) : mode_(mode) {}
  int Seek(vsi_l_offset offset, int whence) override { return 0; }
  vsi_l_offset Tell() override { return 0; }
  size_t Read(void* buffer, size_t size, size_t count) override { return 0; }
  size_t Write(const void* buffer, size_t size, size_t count) override {
    ++write_calls_;
    if (mode_ == 1 && write_calls_ == 1) {
      return 0;  // Fail header write.
    }
    if (mode_ == 2 && write_calls_ >= 2) {
      return 0;  // Fail entry write.
    }
    return count;
  }
  void ClearErr() override {}
  int Eof() override { return 0; }
  int Error() override { return 0; }
  int Close() override { return mode_ == 3 ? -1 : 0; }

 private:
  int mode_ = 0;
  int write_calls_ = 0;
};

class FailingPamProxyFSHandler final : public VSIFilesystemHandler {
 public:
  explicit FailingPamProxyFSHandler(int mode) : mode_(mode) {}
  VSIVirtualHandleUniquePtr Open(const char* filename, const char* access,
                                 bool set_error,
                                 CSLConstList options) override {
    if (std::strstr(filename, ".lck") != nullptr) {
      return VSIVirtualHandleUniquePtr(new FailingPamProxyHandle(0));
    }
    return VSIVirtualHandleUniquePtr(new FailingPamProxyHandle(mode_));
  }
  int Stat(const char* filename, VSIStatBufL* stat_buf, int flags) override {
    return -1;
  }
  int Unlink(const char* filename) override { return 0; }

 private:
  int mode_ = 0;
};

TEST(GdalpamproxydbTest, AllocateGetLoadAndCleanProxyDB) {
  PamCleanProxyDB();
  absl::Cleanup clean_db = [] { PamCleanProxyDB(); };

  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup_vsimem = [] { VSIRmdirRecursive(kTestDir); };

  // Without GDAL_PAM_PROXY_DIR configured, returns nullptr.
  {
    CPLConfigOptionSetter unset_proxy_dir("GDAL_PAM_PROXY_DIR", nullptr,
                                          /*bSetOnlyIfUndefined=*/false);
    EXPECT_EQ(nullptr,
              PamGetProxy("/vsimem/gdalpamproxydb_test/some_file.tif"));
    EXPECT_EQ(nullptr,
              PamAllocateProxy("/vsimem/gdalpamproxydb_test/some_file.tif"));
    PamCleanProxyDB();
  }

  ASSERT_EQ(0, VSIMkdir(kProxyDir, 0755));
  {
    CPLConfigOptionSetter proxy_dir_opt("GDAL_PAM_PROXY_DIR", kProxyDir,
                                        /*bSetOnlyIfUndefined=*/false);

    // Initially not in DB.
    EXPECT_EQ(nullptr, PamGetProxy("/vsimem/gdalpamproxydb_test/file1.tif"));

    // Allocate standard file, :::OVR file, .gmac file, and very long path
    // (> 220 chars).
    const char* p1 = PamAllocateProxy("/vsimem/gdalpamproxydb_test/file1.tif");
    ASSERT_NE(nullptr, p1);
    EXPECT_STREQ(p1, PamGetProxy("/vsimem/gdalpamproxydb_test/file1.tif"));

    const char* p2 =
        PamAllocateProxy(":::OVR/vsimem/gdalpamproxydb_test/file1.tif");
    ASSERT_NE(nullptr, p2);

    const char* p3 = PamAllocateProxy("/vsimem/gdalpamproxydb_test/file1.gmac");
    ASSERT_NE(nullptr, p3);

    std::string long_path = "/vsimem/gdalpamproxydb_test/";
    for (int i = 0; i < 25; ++i) {
      long_path += "verylongsubdir/";
    }
    long_path += "special?chars*file.tif";
    const char* p4 = PamAllocateProxy(long_path.c_str());
    ASSERT_NE(nullptr, p4);

    // Clean in-memory DB and reload from the saved gdal_pam_proxy.dat file.
    const std::string saved_p1(p1);
    PamCleanProxyDB();
    const char* reloaded_p1 =
        PamGetProxy("/vsimem/gdalpamproxydb_test/file1.tif");
    ASSERT_NE(nullptr, reloaded_p1);
    EXPECT_EQ(saved_p1, reloaded_p1);

    // Corrupt the gdal_pam_proxy.dat file header and test LoadDB error path.
    PamCleanProxyDB();
    {
      VsiMemTempWrapper corrupt_db(kDbFile, "SHORT_CORRUPT_HEADER");
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(nullptr, PamGetProxy("/vsimem/gdalpamproxydb_test/file1.tif"));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      CPLErrorReset();
    }

    // Truncated gdal_pam_proxy.dat with valid 100-byte header + original name
    // without proxy name (hits iNext == nBufLength break in LoadDB).
    PamCleanProxyDB();
    {
      std::string truncated_db(100, ' ');
      truncated_db.replace(0, 19, "GDAL_PROXY        1");
      truncated_db += "unterminated_original";
      VsiMemTempWrapper truncated_file(kDbFile, truncated_db);
      EXPECT_EQ(nullptr, PamGetProxy("unterminated_original"));
    }
  }

  // SaveDB failure when osDBName is a directory (VSIFOpenL("w") returns
  // nullptr).
  PamCleanProxyDB();
  ASSERT_EQ(0, VSIMkdir(kFailOpenDir, 0755));
  ASSERT_EQ(0, VSIMkdir(kFailOpenDbDir, 0755));
  {
    CPLConfigOptionSetter fail_dir_opt("GDAL_PAM_PROXY_DIR", kFailOpenDir,
                                       /*bSetOnlyIfUndefined=*/false);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_NE(nullptr, PamAllocateProxy(
                           "/vsimem/gdalpamproxydb_test/file_fail_open.tif"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // SaveDB write/close failures (header write fail, entry write fail, close
  // fail) via custom VSIFilesystemHandler.
  for (int mode : {1, 2, 3}) {
    PamCleanProxyDB();
    auto handler = std::make_unique<FailingPamProxyFSHandler>(mode);
    VSIFileManager::InstallHandler(kFailFsPrefix, handler.get());
    absl::Cleanup remove_handler = [] {
      VSIFileManager::RemoveHandler(kFailFsPrefix);
    };
    CPLConfigOptionSetter fail_fs_opt("GDAL_PAM_PROXY_DIR", kFailFsDir,
                                      /*bSetOnlyIfUndefined=*/false);
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_NE(nullptr,
                PamAllocateProxy("/vsimem/gdalpamproxydb_test/fail_test.tif"));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      CPLErrorReset();
    }
    PamCleanProxyDB();
  }
}

}  // namespace
}  // namespace autotest2
