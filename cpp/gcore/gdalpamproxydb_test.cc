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
// Tests gcore/gdalpamproxydb.cpp.

#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal_pam.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace {

class FailingPamProxyHandle final : public VSIVirtualHandle {
 public:
  explicit FailingPamProxyHandle(int mode) : mode_(mode) {}
  int Seek(vsi_l_offset offset, int whence) override { return 0; }
  vsi_l_offset Tell() override { return 0; }
  size_t Read(void* buffer, size_t size, size_t count) override { return 0; }
  size_t Write(const void* buffer, size_t size, size_t count) override {
    ++write_calls_;
    if (mode_ == 1 && write_calls_ == 1) {
      return 0;  // Fail header write
    }
    if (mode_ == 2 && write_calls_ >= 2) {
      return 0;  // Fail entry write
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
    if (strstr(filename, ".lck") != nullptr) {
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

  // Without GDAL_PAM_PROXY_DIR configured, returns nullptr.
  CPLSetThreadLocalConfigOption("GDAL_PAM_PROXY_DIR", nullptr);
  EXPECT_EQ(nullptr, PamGetProxy("/vsimem/some_file.tif"));
  EXPECT_EQ(nullptr, PamAllocateProxy("/vsimem/some_file.tif"));
  PamCleanProxyDB();

  const std::string proxy_dir = "/vsimem/pam_proxy_test_dir";
  VSIMkdir(proxy_dir.c_str(), 0755);
  CPLSetThreadLocalConfigOption("GDAL_PAM_PROXY_DIR", proxy_dir.c_str());

  // Initially not in DB.
  EXPECT_EQ(nullptr, PamGetProxy("/vsimem/file1.tif"));

  // Allocate standard file, :::OVR file, .gmac file, and very long path (> 220
  // chars).
  const char* p1 = PamAllocateProxy("/vsimem/file1.tif");
  ASSERT_NE(nullptr, p1);
  EXPECT_STREQ(p1, PamGetProxy("/vsimem/file1.tif"));

  const char* p2 = PamAllocateProxy(":::OVR/vsimem/file1.tif");
  ASSERT_NE(nullptr, p2);

  const char* p3 = PamAllocateProxy("/vsimem/file1.gmac");
  ASSERT_NE(nullptr, p3);

  std::string long_path = "/vsimem/";
  for (int i = 0; i < 25; ++i) {
    long_path += "verylongsubdir/";
  }
  long_path += "special?chars*file.tif";
  const char* p4 = PamAllocateProxy(long_path.c_str());
  ASSERT_NE(nullptr, p4);

  // Clean in-memory DB and reload from the saved gdal_pam_proxy.dat file!
  std::string saved_p1(p1);
  PamCleanProxyDB();
  const char* reloaded_p1 = PamGetProxy("/vsimem/file1.tif");
  ASSERT_NE(nullptr, reloaded_p1);
  EXPECT_EQ(saved_p1, std::string(reloaded_p1));

  // Corrupt the gdal_pam_proxy.dat file header and test LoadDB error path.
  PamCleanProxyDB();
  const std::string db_file = proxy_dir + "/gdal_pam_proxy.dat";
  VSILFILE* fp = VSIFOpenL(db_file.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  VSIFWriteL("SHORT_CORRUPT_HEADER", 1, 20, fp);
  VSIFCloseL(fp);

  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(nullptr, PamGetProxy("/vsimem/file1.tif"));
  CPLPopErrorHandler();

  // Truncated gdal_pam_proxy.dat with valid 100-byte header + original name
  // without proxy name (hits iNext == nBufLength break in LoadDB).
  PamCleanProxyDB();
  fp = VSIFOpenL(db_file.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  std::string hdr(100, ' ');
  hdr.replace(0, 19, "GDAL_PROXY        1");
  VSIFWriteL(hdr.data(), 1, 100, fp);
  VSIFWriteL("unterminated_original", 1, 21, fp);
  VSIFCloseL(fp);
  EXPECT_EQ(nullptr, PamGetProxy("unterminated_original"));

  // SaveDB failure when osDBName is a directory (VSIFOpenL("w") returns
  // nullptr) and when proxy_dir has max_size=50 on /vsimem/ (VSIFWriteL header
  // fails).
  PamCleanProxyDB();
  const std::string fail_open_dir = "/vsimem/pam_proxy_fail_open";
  VSIMkdir(fail_open_dir.c_str(), 0755);
  VSIMkdir((fail_open_dir + "/gdal_pam_proxy.dat").c_str(), 0755);
  CPLSetThreadLocalConfigOption("GDAL_PAM_PROXY_DIR", fail_open_dir.c_str());
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_NE(nullptr, PamAllocateProxy("/vsimem/file_fail_open.tif"));
  CPLPopErrorHandler();
  VSIRmdir((fail_open_dir + "/gdal_pam_proxy.dat").c_str());
  VSIRmdir(fail_open_dir.c_str());

  // SaveDB write failures (header write fail, entry write fail, close fail)
  // via custom VSIFilesystemHandler.
  for (int mode : {1, 2, 3}) {
    PamCleanProxyDB();
    auto* handler = new FailingPamProxyFSHandler(mode);
    VSIFileManager::InstallHandler("/vsipamfail/", handler);
    CPLSetThreadLocalConfigOption("GDAL_PAM_PROXY_DIR", "/vsipamfail");
    CPLPushErrorHandler(CPLQuietErrorHandler);
    PamAllocateProxy("/vsimem/fail_test.tif");
    CPLPopErrorHandler();
    PamCleanProxyDB();
    VSIFileManager::RemoveHandler("/vsipamfail/");
    delete handler;
  }

  PamCleanProxyDB();
  CPLSetThreadLocalConfigOption("GDAL_PAM_PROXY_DIR", nullptr);
  VSIUnlink(db_file.c_str());
  VSIRmdir(proxy_dir.c_str());
}

}  // namespace
