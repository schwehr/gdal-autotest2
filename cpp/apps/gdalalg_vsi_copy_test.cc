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
// Tests for GDALVSICopyAlgorithm (apps/gdalalg_vsi_copy.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vsi_copy.cpp

#include "third_party/gdal/apps/gdalalg_vsi_copy.h"

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

using ::testing::ElementsAre;
using ::testing::HasSubstr;

constexpr char kWorkingDir[] = "/vsimem/gdalalg_vsi_copy_test";
constexpr char kFailPrefix[] = "/vsicopy_fail_test/";

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
    if (std::string_view(pszFilename).find("regular_file") !=
        std::string_view::npos) {
      pStatBuf->st_mode = S_IFREG;
      pStatBuf->st_size = 10;
      return 0;
    }
    return -1;
  }

  int CopyFile(const char* /* pszSource */, const char* /* pszTarget */,
               VSILFILE* /* fpSource */, vsi_l_offset /* nSourceSize */,
               const char* const* /* papszOptions */,
               GDALProgressFunc /* pProgressFunc */,
               void* /* pProgressData */) override {
    return -1;
  }
};

TEST(GdalalgVsiCopyTest, BasicPropertiesAndAliases) {
  GDALVSICopyAlgorithm alg;
  EXPECT_EQ("copy", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_FALSE(alg.GetHelpURL().empty());
  EXPECT_THAT(GDALVSICopyAlgorithm::GetAliasesStatic(), ElementsAre("cp"));
}

TEST(GdalalgVsiCopyTest, MissingArguments) {
  WithQuietHandler quiet;
  GDALVSICopyAlgorithm alg;
  EXPECT_FALSE(alg.ParseCommandLineArguments({}));
  EXPECT_FALSE(alg.ParseCommandLineArguments({"source_only"}));
}

TEST(GdalalgVsiCopyTest, CopySingleFileSuccess) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kWorkingDir); };
  ASSERT_EQ(0, VSIMkdir(kWorkingDir, 0755));

  const std::string src = std::string(kWorkingDir) + "/src.txt";
  const std::string dst = std::string(kWorkingDir) + "/dst.txt";

  VSILFILE* fp = VSIFOpenL(src.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
  EXPECT_EQ(5, VSIFWriteL("hello", 1, 5, fp));
  std::move(fp_closer).Invoke();

  GDALVSICopyAlgorithm alg;
  EXPECT_TRUE(alg.ParseCommandLineArguments({src, dst}));
  EXPECT_TRUE(alg.Run());

  VSIStatBufL sStat;
  EXPECT_EQ(0, VSIStatL(dst.c_str(), &sStat));
  EXPECT_EQ(5, sStat.st_size);
}

TEST(GdalalgVsiCopyTest, CopySingleFileToDirectoryDestination) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kWorkingDir); };
  ASSERT_EQ(0, VSIMkdir(kWorkingDir, 0755));

  const std::string src = std::string(kWorkingDir) + "/file.txt";
  const std::string dst_dir = std::string(kWorkingDir) + "/dst_dir/";

  VSILFILE* fp = VSIFOpenL(src.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
  EXPECT_EQ(4, VSIFWriteL("data", 1, 4, fp));
  std::move(fp_closer).Invoke();

  ASSERT_EQ(0, VSIMkdir(dst_dir.c_str(), 0755));

  GDALVSICopyAlgorithm alg;
  EXPECT_TRUE(alg.ParseCommandLineArguments({src, dst_dir}));
  EXPECT_TRUE(alg.Run());

  const std::string expected_dst =
      std::string(kWorkingDir) + "/dst_dir/file.txt";
  VSIStatBufL sStat;
  EXPECT_EQ(0, VSIStatL(expected_dst.c_str(), &sStat));
  EXPECT_EQ(4, sStat.st_size);
}

TEST(GdalalgVsiCopyTest, CopyDirectoryWithoutRecursiveFails) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kWorkingDir); };
  ASSERT_EQ(0, VSIMkdir(kWorkingDir, 0755));

  const std::string dir = std::string(kWorkingDir) + "/a_dir";
  ASSERT_EQ(0, VSIMkdir(dir.c_str(), 0755));

  CPLErrorReset();
  GDALVSICopyAlgorithm alg;
  EXPECT_TRUE(
      alg.ParseCommandLineArguments({dir, std::string(kWorkingDir) + "/dst"}));
  {
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.Run());
  }
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("is a directory. Use -r/--recursive"));
  CPLErrorReset();
}

TEST(GdalalgVsiCopyTest, CopyRecursiveSuccessWithProgressAndTrailingSlashes) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kWorkingDir); };
  ASSERT_EQ(0, VSIMkdir(kWorkingDir, 0755));

  const std::string src_dir = std::string(kWorkingDir) + "/src_tree";
  const std::string sub_dir = src_dir + "/sub";
  ASSERT_EQ(0, VSIMkdir(src_dir.c_str(), 0755));
  ASSERT_EQ(0, VSIMkdir(sub_dir.c_str(), 0755));

  const std::string f1 = src_dir + "/f1.txt";
  const std::string f2 = sub_dir + "/f2.txt";

  VSILFILE* fp1 = VSIFOpenL(f1.c_str(), "wb");
  ASSERT_NE(nullptr, fp1);
  EXPECT_EQ(3, VSIFWriteL("abc", 1, 3, fp1));
  EXPECT_EQ(0, VSIFCloseL(fp1));

  VSILFILE* fp2 = VSIFOpenL(f2.c_str(), "wb");
  ASSERT_NE(nullptr, fp2);
  EXPECT_EQ(4, VSIFWriteL("defg", 1, 4, fp2));
  EXPECT_EQ(0, VSIFCloseL(fp2));

  const std::string dst_dir = std::string(kWorkingDir) + "/dst_tree";
  ASSERT_EQ(0, VSIMkdir(dst_dir.c_str(), 0755));

  // Trailing slash on both source and destination triggers lines 90 and 112
  GDALVSICopyAlgorithm alg;
  EXPECT_TRUE(alg.ParseCommandLineArguments(
      {"--progress", "-r", src_dir + "/", dst_dir + "/"}));
  EXPECT_TRUE(alg.Run());

  VSIStatBufL sStat;
  const std::string copied_f1 = dst_dir + "/src_tree/f1.txt";
  const std::string copied_f2 = dst_dir + "/src_tree/sub/f2.txt";
  EXPECT_EQ(0, VSIStatL(copied_f1.c_str(), &sStat));
  EXPECT_EQ(0, VSIStatL(copied_f2.c_str(), &sStat));
}

TEST(GdalalgVsiCopyTest, CopyWildcardNonRecursive) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kWorkingDir); };
  ASSERT_EQ(0, VSIMkdir(kWorkingDir, 0755));

  const std::string src_dir = std::string(kWorkingDir) + "/wild_src";
  const std::string sub_dir = src_dir + "/inner_dir";
  const std::string dst_dir = std::string(kWorkingDir) + "/wild_dst";
  ASSERT_EQ(0, VSIMkdir(src_dir.c_str(), 0755));
  ASSERT_EQ(0, VSIMkdir(sub_dir.c_str(), 0755));
  ASSERT_EQ(0, VSIMkdir(dst_dir.c_str(), 0755));

  const std::string f1 = src_dir + "/file.txt";
  VSILFILE* fp = VSIFOpenL(f1.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  EXPECT_EQ(2, VSIFWriteL("hi", 1, 2, fp));
  EXPECT_EQ(0, VSIFCloseL(fp));

  GDALVSICopyAlgorithm alg;
  EXPECT_TRUE(alg.ParseCommandLineArguments({src_dir + "/*", dst_dir}));
  EXPECT_TRUE(alg.Run());

  VSIStatBufL sStat;
  EXPECT_EQ(0, VSIStatL((dst_dir + "/file.txt").c_str(), &sStat));
  EXPECT_EQ(0, VSIStatL((dst_dir + "/inner_dir").c_str(), &sStat));
}

TEST(GdalalgVsiCopyTest, CopyRecursiveOnRegularFileNotADirectory) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kWorkingDir); };
  ASSERT_EQ(0, VSIMkdir(kWorkingDir, 0755));

  const std::string file = std::string(kWorkingDir) + "/regular.txt";
  VSILFILE* fp = VSIFOpenL(file.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  EXPECT_EQ(2, VSIFWriteL("hi", 1, 2, fp));
  EXPECT_EQ(0, VSIFCloseL(fp));

  // Passing a regular file with -r: VSIOpenDir fails inside CopyRecursive
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALVSICopyAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments(
        {"-r", file, std::string(kWorkingDir) + "/dst"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("is not a directory or cannot be opened"));
    CPLErrorReset();
  }

  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALVSICopyAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments(
        {"--skip-errors", "-r", file, std::string(kWorkingDir) + "/dst"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("is not a directory or cannot be opened"));
    CPLErrorReset();
  }
}

TEST(GdalalgVsiCopyTest, ProgressCancellationAtStartAndDuringCopy) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kWorkingDir); };
  ASSERT_EQ(0, VSIMkdir(kWorkingDir, 0755));

  const std::string src_dir = std::string(kWorkingDir) + "/cancel_src";
  const std::string sub_dir = src_dir + "/sub";
  ASSERT_EQ(0, VSIMkdir(src_dir.c_str(), 0755));
  ASSERT_EQ(0, VSIMkdir(sub_dir.c_str(), 0755));
  const std::string f1 = src_dir + "/file.txt";
  VSILFILE* fp = VSIFOpenL(f1.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  EXPECT_EQ(1, VSIFWriteL("x", 1, 1, fp));
  EXPECT_EQ(0, VSIFCloseL(fp));

  // 1. Cancel at initial listing
  {
    GDALVSICopyAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments(
        {"-r", src_dir, std::string(kWorkingDir) + "/cancel_dst1"}));
    auto cancel_at_start =
        [](double /* dfComplete */, const char* /* pszMessage */,
           void* /* pProgressData */) -> int { return FALSE; };
    EXPECT_FALSE(alg.Run(cancel_at_start, nullptr));
  }

  // 2. Cancel during directory progress
  {
    const std::string dir_only = std::string(kWorkingDir) + "/dir_only";
    const std::string dir_sub = dir_only + "/sub";
    ASSERT_EQ(0, VSIMkdir(dir_only.c_str(), 0755));
    ASSERT_EQ(0, VSIMkdir(dir_sub.c_str(), 0755));

    GDALVSICopyAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments(
        {"-r", dir_only, std::string(kWorkingDir) + "/cancel_dst2"}));
    auto cancel_on_subdir = [](double dfComplete, const char* /* pszMessage */,
                               void* /* pProgressData */) -> int {
      if (dfComplete > 0.0) {
        return FALSE;
      }
      return TRUE;
    };
    EXPECT_FALSE(alg.Run(cancel_on_subdir, nullptr));
  }
}

TEST(GdalalgVsiCopyTest, NonExistentSources) {
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALVSICopyAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments(
        {"/vsimem/gdalalg_vsi_copy_test/non_existent.bin", "/vsimem/dst.bin"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("cannot be accessed"));
    CPLErrorReset();
  }

  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALVSICopyAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments(
        {"-r", "/vsimem/gdalalg_vsi_copy_test/non_existent_dir",
         "/vsimem/dst"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("cannot be accessed"));
    CPLErrorReset();
  }

  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALVSICopyAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments(
        {"/vsimem/gdalalg_vsi_copy_test/non_existent_wild/*", "/vsimem/dst"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("cannot be accessed"));
    CPLErrorReset();
  }
}

TEST(GdalalgVsiCopyTest, ErrorPathsWithCustomHandler) {
  CustomFailFilesystemHandler handler;
  VSIFileManager::InstallHandler(kFailPrefix, &handler);
  absl::Cleanup remove_handler = [] {
    VSIFileManager::RemoveHandler(kFailPrefix);
  };

  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALVSICopyAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments(
        {"/vsicopy_fail_test/vsi_error", "/vsicopy_fail_test/dst"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("cannot be accessed"));
    CPLErrorReset();
  }

  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALVSICopyAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments(
        {"/vsicopy_fail_test/regular_file", "/vsicopy_fail_test/dst"}));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }

  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALVSICopyAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments(
        {"--skip-errors", "/vsicopy_fail_test/regular_file",
         "/vsicopy_fail_test/dst"}));
    EXPECT_TRUE(alg.Run());
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover remaining unreachable/defensive branches in
// apps/gdalalg_vsi_copy.cpp.
