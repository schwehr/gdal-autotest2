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
// Tests for Kerchunk VSI utility functions (vsikerchunk.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/zarr/vsikerchunk.cpp

#include "third_party/gdal/frmts/zarr/vsikerchunk.h"

#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace autotest2 {
namespace {

class FakeRemoteFilesystemHandler : public VSIFilesystemHandler {
 public:
  bool IsLocal(const char* /*pszPath*/) const override { return false; }
  VSIVirtualHandleUniquePtr Open(const char* /*pszFilename*/,
                                 const char* /*pszAccess*/, bool /*bSetError*/,
                                 CSLConstList /*papszOptions*/) override {
    return nullptr;
  }
  int Stat(const char* /*pszFilename*/, VSIStatBufL* /*pStatBuf*/,
           int /*nFlags*/) override {
    return -1;
  }
};

TEST(VsikerchunkTest, InstallAndCleanCache) {
  VSIInstallKerchunkFileSystems();
  EXPECT_NE(VSIFileManager::GetHandler("/"),
            VSIFileManager::GetHandler("/vsikerchunk_json_ref/"));
  EXPECT_NE(VSIFileManager::GetHandler("/"),
            VSIFileManager::GetHandler("/vsikerchunk_json_ref_cached/"));
  EXPECT_NE(VSIFileManager::GetHandler("/"),
            VSIFileManager::GetHandler("/vsikerchunk_parquet_ref/"));
  VSIKerchunkFileSystemsCleanCache();
}

TEST(VsikerchunkTest, MorphURIToVSIPathWithVsiURIs) {
  EXPECT_EQ("/vsis3/bucket/key",
            VSIKerchunkMorphURIToVSIPath("s3://bucket/key", ""));
  EXPECT_EQ("/vsigs/bucket/key",
            VSIKerchunkMorphURIToVSIPath("gs://bucket/key", ""));
  EXPECT_EQ("/vsicurl/http://example.com/test",
            VSIKerchunkMorphURIToVSIPath("http://example.com/test", ""));
  EXPECT_EQ("/vsicurl/https://example.com/test",
            VSIKerchunkMorphURIToVSIPath("https://example.com/test", ""));
}

TEST(VsikerchunkTest, MorphURIToVSIPathWithRelativePath) {
  EXPECT_EQ("/path/to/root/sub/file.bin",
            VSIKerchunkMorphURIToVSIPath("sub/file.bin", "/path/to/root"));
  EXPECT_EQ("sub/file.bin", VSIKerchunkMorphURIToVSIPath("sub/file.bin", ""));
}

TEST(VsikerchunkTest, MorphURIToVSIPathLocalFromRemoteDefaultDisabled) {
  constexpr char kRemotePrefix[] = "/vsi_test_remote_default/";
  FakeRemoteFilesystemHandler handler;
  VSIFileManager::InstallHandler(kRemotePrefix, &handler);
  absl::Cleanup remove_handler = [kRemotePrefix] {
    VSIFileManager::RemoveHandler(kRemotePrefix);
  };

  const CPLConfigOptionSetter opt_unset(
      "GDAL_ALLOW_REMOTE_RESOURCE_TO_ACCESS_LOCAL_FILE", nullptr, false);

  const ::WithQuietHandler quiet;
  CPLErrorReset();
  const std::string result = VSIKerchunkMorphURIToVSIPath(
      "/local/path/file.bin", "/vsi_test_remote_default/root");
  EXPECT_EQ("", result);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST(VsikerchunkTest, MorphURIToVSIPathLocalFromRemoteExplicitlyDisabled) {
  constexpr char kRemotePrefix[] = "/vsi_test_remote_disabled/";
  FakeRemoteFilesystemHandler handler;
  VSIFileManager::InstallHandler(kRemotePrefix, &handler);
  absl::Cleanup remove_handler = [kRemotePrefix] {
    VSIFileManager::RemoveHandler(kRemotePrefix);
  };

  const CPLConfigOptionSetter opt_no(
      "GDAL_ALLOW_REMOTE_RESOURCE_TO_ACCESS_LOCAL_FILE", "NO", false);

  const ::WithQuietHandler quiet;
  CPLErrorReset();
  const std::string result = VSIKerchunkMorphURIToVSIPath(
      "/local/path/file.bin", "/vsi_test_remote_disabled/root");
  EXPECT_EQ("", result);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST(VsikerchunkTest, MorphURIToVSIPathLocalFromRemoteExplicitlyAllowed) {
  constexpr char kRemotePrefix[] = "/vsi_test_remote_allowed/";
  FakeRemoteFilesystemHandler handler;
  VSIFileManager::InstallHandler(kRemotePrefix, &handler);
  absl::Cleanup remove_handler = [kRemotePrefix] {
    VSIFileManager::RemoveHandler(kRemotePrefix);
  };

  const CPLConfigOptionSetter opt_yes(
      "GDAL_ALLOW_REMOTE_RESOURCE_TO_ACCESS_LOCAL_FILE", "YES", false);

  const std::string result = VSIKerchunkMorphURIToVSIPath(
      "/local/path/file.bin", "/vsi_test_remote_allowed/root");
  EXPECT_EQ("/local/path/file.bin", result);
}

TEST(VsikerchunkTest, MorphURIToVSIPathPassthrough) {
  EXPECT_EQ("/nonexistent/file.bin",
            VSIKerchunkMorphURIToVSIPath("/nonexistent/file.bin",
                                         "/nonexistent/root"));
  EXPECT_EQ("foo://bar",
            VSIKerchunkMorphURIToVSIPath("foo://bar", "/nonexistent/root"));
}

}  // namespace
}  // namespace autotest2
