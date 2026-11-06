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
// Tests for GDALVSISyncAlgorithm (apps/gdalalg_vsi_sync.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vsi_sync.cpp

#include "third_party/gdal/apps/gdalalg_vsi_sync.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_error.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kWorkingDir[] = "/vsimem/gdalalg_vsi_sync_test";
constexpr char kFailPrefix[] = "/vsisync_fail_test/";

class CustomFailFilesystemHandler : public VSIFilesystemHandler {
 public:
  VSIVirtualHandleUniquePtr Open(const char* /* pszFilename */,
                                 const char* /* pszAccess */,
                                 bool /* bSetError */,
                                 CSLConstList /* papszOptions */) override {
    return nullptr;
  }

  int Stat(const char* pszFilename, VSIStatBufL* pStatBuf,
           int /* nFlags */) override {
    if (std::string_view(pszFilename).find("vsi_error") !=
        std::string_view::npos) {
      VSIError(VSIE_AccessDenied, "Access denied");
      return -1;
    }
    if (std::string_view(pszFilename).find("cannot_sync") !=
        std::string_view::npos) {
      pStatBuf->st_mode = S_IFREG;
      return 0;
    }
    return -1;
  }

  bool Sync(const char* /* pszSource */, const char* /* pszTarget */,
            const char* const* /* papszOptions */,
            GDALProgressFunc /* pProgressFunc */, void* /* pProgressData */,
            char*** /* ppapszOutputs */) override {
    return false;
  }
};

TEST(GdalalgVsiSyncTest, BasicProperties) {
  GDALVSISyncAlgorithm alg;
  EXPECT_EQ("sync", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_FALSE(alg.GetHelpURL().empty());
}

TEST(GdalalgVsiSyncTest, MissingArguments) {
  WithQuietHandler quiet;
  GDALVSISyncAlgorithm alg;
  EXPECT_FALSE(alg.ParseCommandLineArguments({}));
  EXPECT_FALSE(alg.ParseCommandLineArguments({"source_only"}));
}

TEST(GdalalgVsiSyncTest, SyncFileSuccess) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kWorkingDir); };
  ASSERT_EQ(0, VSIMkdir(kWorkingDir, 0755));

  const std::string src = std::string(kWorkingDir) + "/src.txt";
  const std::string dst = std::string(kWorkingDir) + "/dst.txt";

  VSILFILE* fp = VSIFOpenL(src.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
  EXPECT_EQ(5, VSIFWriteL("sync!", 1, 5, fp));
  std::move(fp_closer).Invoke();

  GDALVSISyncAlgorithm alg;
  EXPECT_TRUE(alg.ParseCommandLineArguments({src, dst}));
  EXPECT_TRUE(alg.Run());

  VSIStatBufL sStat;
  EXPECT_EQ(0, VSIStatL(dst.c_str(), &sStat));
  EXPECT_EQ(5, sStat.st_size);
}

TEST(GdalalgVsiSyncTest, SyncWithOptions) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kWorkingDir); };
  ASSERT_EQ(0, VSIMkdir(kWorkingDir, 0755));

  const std::string src_dir = std::string(kWorkingDir) + "/src_dir";
  const std::string dst_dir = std::string(kWorkingDir) + "/dst_dir";
  ASSERT_EQ(0, VSIMkdir(src_dir.c_str(), 0755));
  ASSERT_EQ(0, VSIMkdir(dst_dir.c_str(), 0755));

  const std::string src_file = src_dir + "/f.txt";
  VSILFILE* fp = VSIFOpenL(src_file.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
  EXPECT_EQ(4, VSIFWriteL("test", 1, 4, fp));
  std::move(fp_closer).Invoke();

  GDALVSISyncAlgorithm alg;
  EXPECT_TRUE(alg.ParseCommandLineArguments(
      {"-r", "--strategy", "overwrite", "-j", "2", src_dir + "/", dst_dir}));
  EXPECT_TRUE(alg.Run());

  const std::string dst_file = dst_dir + "/f.txt";
  VSIStatBufL sStat;
  EXPECT_EQ(0, VSIStatL(dst_file.c_str(), &sStat));
}

TEST(GdalalgVsiSyncTest, NonExistentSource) {
  CPLErrorReset();
  GDALVSISyncAlgorithm alg;
  EXPECT_TRUE(alg.ParseCommandLineArguments(
      {"/vsimem/gdalalg_vsi_sync_test/non_existent.bin",
       "/vsimem/gdalalg_vsi_sync_test/dst.bin"}));
  {
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.Run());
  }
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("does not exist or cannot be accessed"));
  CPLErrorReset();
}

TEST(GdalalgVsiSyncTest, ErrorPathsWithCustomHandler) {
  CustomFailFilesystemHandler handler;
  VSIFileManager::InstallHandler(kFailPrefix, &handler);
  absl::Cleanup remove_handler = [] {
    VSIFileManager::RemoveHandler(kFailPrefix);
  };

  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALVSISyncAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments(
        {"/vsisync_fail_test/vsi_error", "/vsisync_fail_test/dst"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("cannot be accessed"));
    CPLErrorReset();
  }

  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALVSISyncAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments(
        {"/vsisync_fail_test/cannot_sync", "/vsisync_fail_test/dst"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("could not be synchronised with"));
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
