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
// Tests for GDALVSIDeleteAlgorithm (apps/gdalalg_vsi_delete.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vsi_delete.cpp

#include "third_party/gdal/apps/gdalalg_vsi_delete.h"

#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_error.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kFileSuccessDir[] =
    "/vsimem/gdalalg_vsi_delete_test/file_success";
constexpr char kEmptyDir[] = "/vsimem/gdalalg_vsi_delete_test/empty_dir";
constexpr char kRecDir[] = "/vsimem/gdalalg_vsi_delete_test/rec_dir";
constexpr char kRecDirUpperR[] =
    "/vsimem/gdalalg_vsi_delete_test/rec_dir_upper_r";
constexpr char kFailPrefix[] = "/vsidelete_fail_test/";

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
    if (std::string_view(pszFilename).find("cannot_delete") !=
        std::string_view::npos) {
      pStatBuf->st_mode = S_IFREG;
      return 0;
    }
    return -1;
  }

  int Unlink(const char* /* pszFilename */) override { return -1; }
};

TEST(GdalalgVsiDeleteTest, BasicProperties) {
  GDALVSIDeleteAlgorithm alg;
  EXPECT_EQ("delete", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_FALSE(alg.GetHelpURL().empty());
}

TEST(GdalalgVsiDeleteTest, MissingArgumentFails) {
  WithQuietHandler quiet;
  GDALVSIDeleteAlgorithm alg;
  EXPECT_FALSE(alg.ParseCommandLineArguments({}));
}

TEST(GdalalgVsiDeleteTest, DeleteFileSuccess) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kFileSuccessDir); };
  ASSERT_EQ(0, VSIMkdir(kFileSuccessDir, 0755));

  const std::string file = std::string(kFileSuccessDir) + "/test.bin";
  VSIStatBufL sStat;
  VSILFILE* fp = VSIFOpenL(file.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  ASSERT_EQ(0, VSIFCloseL(fp));
  ASSERT_EQ(0, VSIStatL(file.c_str(), &sStat));

  GDALVSIDeleteAlgorithm alg;
  EXPECT_TRUE(alg.ParseCommandLineArguments({file}));
  EXPECT_TRUE(alg.Run());
  EXPECT_NE(0, VSIStatL(file.c_str(), &sStat));
}

TEST(GdalalgVsiDeleteTest, DeleteNonExistentFile) {
  CPLErrorReset();
  GDALVSIDeleteAlgorithm alg;
  EXPECT_TRUE(alg.ParseCommandLineArguments(
      {"/vsimem/gdalalg_vsi_delete_test/non_existent.bin"}));
  {
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.Run());
  }
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("does not exist"));
  CPLErrorReset();
}

TEST(GdalalgVsiDeleteTest, DeleteEmptyDirectoryNonRecursive) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kEmptyDir); };
  ASSERT_EQ(0, VSIMkdir(kEmptyDir, 0755));

  VSIStatBufL sStat;
  ASSERT_EQ(0, VSIStatL(kEmptyDir, &sStat));

  GDALVSIDeleteAlgorithm alg;
  EXPECT_TRUE(alg.ParseCommandLineArguments({kEmptyDir}));
  EXPECT_TRUE(alg.Run());
  EXPECT_NE(0, VSIStatL(kEmptyDir, &sStat));
}

TEST(GdalalgVsiDeleteTest, DeleteDirectoryRecursive) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kRecDir); };

  ASSERT_EQ(0, VSIMkdir(kRecDir, 0755));
  const std::string sub = std::string(kRecDir) + "/sub";
  ASSERT_EQ(0, VSIMkdir(sub.c_str(), 0755));
  const std::string file = sub + "/test.txt";
  VSILFILE* fp = VSIFOpenL(file.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  ASSERT_EQ(0, VSIFCloseL(fp));

  VSIStatBufL sStat;
  ASSERT_EQ(0, VSIStatL(kRecDir, &sStat));

  GDALVSIDeleteAlgorithm alg;
  EXPECT_TRUE(alg.ParseCommandLineArguments({"-r", kRecDir}));
  EXPECT_TRUE(alg.Run());
  EXPECT_NE(0, VSIStatL(kRecDir, &sStat));
}

TEST(GdalalgVsiDeleteTest, DeleteDirectoryRecursiveUpperR) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kRecDirUpperR); };

  ASSERT_EQ(0, VSIMkdir(kRecDirUpperR, 0755));
  VSIStatBufL sStat;
  ASSERT_EQ(0, VSIStatL(kRecDirUpperR, &sStat));

  GDALVSIDeleteAlgorithm alg;
  EXPECT_TRUE(alg.ParseCommandLineArguments({"-R", kRecDirUpperR}));
  EXPECT_TRUE(alg.Run());
  EXPECT_NE(0, VSIStatL(kRecDirUpperR, &sStat));
}

TEST(GdalalgVsiDeleteTest, ErrorPathsWithCustomHandler) {
  CustomFailFilesystemHandler handler;
  VSIFileManager::InstallHandler(kFailPrefix, &handler);
  absl::Cleanup remove_handler = [] {
    VSIFileManager::RemoveHandler(kFailPrefix);
  };

  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALVSIDeleteAlgorithm alg;
    EXPECT_TRUE(
        alg.ParseCommandLineArguments({"/vsidelete_fail_test/vsi_error"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("cannot be accessed"));
    CPLErrorReset();
  }

  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALVSIDeleteAlgorithm alg;
    EXPECT_TRUE(
        alg.ParseCommandLineArguments({"/vsidelete_fail_test/cannot_delete"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot delete"));
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
