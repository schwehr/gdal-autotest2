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
// Tests for GDALVSIListAlgorithm (apps/gdalalg_vsi_list.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vsi_list.cpp

#include "third_party/gdal/apps/gdalalg_vsi_list.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_error.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

constexpr char kWorkingDir[] = "/vsimem/gdalalg_vsi_list_test";
constexpr char kFailPrefix[] = "/vsilist_fail_test/";

std::string GetOutput(const GDALVSIListAlgorithm& alg) {
  const auto* arg = alg.GetArg(GDAL_ARG_NAME_OUTPUT_STRING);
  return arg ? arg->Get<std::string>() : "";
}

class MockDir final : public VSIDIR {
 public:
  explicit MockDir(std::vector<VSIDIREntry> entries)
      : entries_(std::move(entries)) {}

  const VSIDIREntry* NextDirEntry() override {
    if (idx_ < entries_.size()) {
      return &entries_[idx_++];
    }
    return nullptr;
  }

 private:
  std::vector<VSIDIREntry> entries_;
  size_t idx_ = 0;
};

class CustomListFilesystemHandler : public VSIFilesystemHandler {
 public:
  VSIVirtualHandleUniquePtr Open(const char* /* pszFilename */,
                                 const char* /* pszAccess */,
                                 bool /* bSetError */,
                                 CSLConstList /* papszOptions */) override {
    return nullptr;
  }

  int Stat(const char* pszFilename, VSIStatBufL* pStatBuf,
           int /* nFlags */) override {
    std::string_view name(pszFilename);
    if (name.find("vsi_error") != std::string_view::npos) {
      VSIError(VSIE_AccessDenied, "Access denied");
      return -1;
    }
    if (name.find("mock_dir") != std::string_view::npos) {
      pStatBuf->st_mode = S_IFDIR;
      return 0;
    }
    return -1;
  }

  VSIDIR* OpenDir(const char* pszPath, int /* nRecurseDepth */,
                  const char* const* /* papszOptions */) override {
    if (std::string_view(pszPath).find("mock_dir") != std::string_view::npos) {
      std::vector<VSIDIREntry> entries(2);
      entries[0].pszName = CPLStrdup("sub_unknown_dir");
      entries[0].nMode = S_IFDIR;
      entries[0].bModeKnown = false;
      entries[0].bMTimeKnown = false;

      entries[1].pszName = CPLStrdup("file_unknown_mode.txt");
      entries[1].nMode = S_IFREG;
      entries[1].bModeKnown = false;
      entries[1].bMTimeKnown = true;
      entries[1].nMTime = 1600000000;

      return new MockDir(std::move(entries));
    }
    return nullptr;
  }
};

TEST(GdalalgVsiListTest, BasicPropertiesAndAliases) {
  GDALVSIListAlgorithm alg;
  EXPECT_EQ("list", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_FALSE(alg.GetHelpURL().empty());
  EXPECT_THAT(GDALVSIListAlgorithm::GetAliasesStatic(), ElementsAre("ls"));
}

TEST(GdalalgVsiListTest, MissingArguments) {
  WithQuietHandler quiet;
  GDALVSIListAlgorithm alg;
  EXPECT_FALSE(alg.ParseCommandLineArguments({}));
}

TEST(GdalalgVsiListTest, SingleFileListing) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kWorkingDir); };
  ASSERT_EQ(0, VSIMkdir(kWorkingDir, 0755));

  const std::string file = std::string(kWorkingDir) + "/single.txt";
  VSILFILE* fp = VSIFOpenL(file.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
  EXPECT_EQ(5, VSIFWriteL("12345", 1, 5, fp));
  std::move(fp_closer).Invoke();

  // 1. Short text listing
  {
    GDALVSIListAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments({file}));
    EXPECT_TRUE(alg.Run());
    EXPECT_THAT(GetOutput(alg), HasSubstr(file));
  }

  // 2. Long text listing
  {
    GDALVSIListAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments({"-l", file}));
    EXPECT_TRUE(alg.Run());
    EXPECT_THAT(GetOutput(alg), HasSubstr("5"));
    EXPECT_THAT(GetOutput(alg), HasSubstr(file));
  }

  // 3. Short JSON listing
  {
    GDALVSIListAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments({"-f", "json", file}));
    EXPECT_TRUE(alg.Run());
    EXPECT_THAT(GetOutput(alg), HasSubstr(file));
  }

  // 4. Long JSON listing
  {
    GDALVSIListAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments({"-f", "json", "-l", file}));
    EXPECT_TRUE(alg.Run());
    EXPECT_THAT(GetOutput(alg), HasSubstr("\"name\":"));
    EXPECT_THAT(GetOutput(alg), HasSubstr("\"type\": \"file\""));
    EXPECT_THAT(GetOutput(alg), HasSubstr("\"size\": 5"));
  }

  // 5. Stdout listing
  {
    GDALVSIListAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments({"--stdout", file}));
    EXPECT_TRUE(alg.Run());
  }
}

TEST(GdalalgVsiListTest, DirectoryListingModes) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kWorkingDir); };
  ASSERT_EQ(0, VSIMkdir(kWorkingDir, 0755));

  const std::string dir = std::string(kWorkingDir) + "/dir";
  const std::string sub1 = dir + "/sub1";
  const std::string sub2 = dir + "/sub2";
  ASSERT_EQ(0, VSIMkdir(dir.c_str(), 0755));
  ASSERT_EQ(0, VSIMkdir(sub1.c_str(), 0755));
  ASSERT_EQ(0, VSIMkdir(sub2.c_str(), 0755));

  const std::string f1 = sub1 + "/f1.txt";
  const std::string f2 = sub2 + "/f2.txt";
  VSILFILE* fp1 = VSIFOpenL(f1.c_str(), "wb");
  ASSERT_NE(nullptr, fp1);
  EXPECT_EQ(3, VSIFWriteL("foo", 1, 3, fp1));
  EXPECT_EQ(0, VSIFCloseL(fp1));

  VSILFILE* fp2 = VSIFOpenL(f2.c_str(), "wb");
  ASSERT_NE(nullptr, fp2);
  EXPECT_EQ(3, VSIFWriteL("bar", 1, 3, fp2));
  EXPECT_EQ(0, VSIFCloseL(fp2));

  // 1. Text recursive long listing
  {
    GDALVSIListAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments({"-R", "-l", dir}));
    EXPECT_TRUE(alg.Run());
    EXPECT_THAT(GetOutput(alg), HasSubstr("sub1"));
    EXPECT_THAT(GetOutput(alg), HasSubstr("sub2"));
  }

  // 2. JSON tree recursive listing
  {
    GDALVSIListAlgorithm alg;
    EXPECT_TRUE(
        alg.ParseCommandLineArguments({"-f", "json", "-R", "--tree", dir}));
    EXPECT_TRUE(alg.Run());
    EXPECT_THAT(GetOutput(alg), HasSubstr("\"entries\": ["));
  }

  // 3. JSON tree recursive long listing
  {
    GDALVSIListAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "json", "-R", "-l", "--tree", dir}));
    EXPECT_TRUE(alg.Run());
    EXPECT_THAT(GetOutput(alg), HasSubstr("\"name\":"));
    EXPECT_THAT(GetOutput(alg), HasSubstr("\"entries\": ["));
  }

  // 4. Depth limit
  {
    GDALVSIListAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments({"-R", "--depth", "1", dir}));
    EXPECT_TRUE(alg.Run());
  }

  // 5. Absolute path
  {
    GDALVSIListAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments({"--abs", dir}));
    EXPECT_TRUE(alg.Run());
    EXPECT_THAT(GetOutput(alg), HasSubstr(dir));
  }
}

TEST(GdalalgVsiListTest, RelativePathWithAbsOption) {
  // Uses "." to exercise CPLGetCurrentDir() in GDALVSIListAlgorithm.
  {
    GDALVSIListAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments({"--abs", "."}));
    EXPECT_TRUE(alg.Run());
  }
}

TEST(GdalalgVsiListTest, NonExistentSources) {
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALVSIListAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments(
        {"/vsimem/gdalalg_vsi_list_test/non_existent.bin"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("does not exist or cannot be accessed"));
    CPLErrorReset();
  }
}

TEST(GdalalgVsiListTest, CustomHandlerUnknownModesAndErrors) {
  CustomListFilesystemHandler handler;
  VSIFileManager::InstallHandler(kFailPrefix, &handler);
  absl::Cleanup remove_handler = [] {
    VSIFileManager::RemoveHandler(kFailPrefix);
  };

  // VSI error path
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALVSIListAlgorithm alg;
    EXPECT_TRUE(
        alg.ParseCommandLineArguments({"/vsilist_fail_test/vsi_error"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("cannot be accessed"));
    CPLErrorReset();
  }

  // Mock directory with bModeKnown = false (both dir and file)
  {
    GDALVSIListAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "text", "-l", "/vsilist_fail_test/mock_dir"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_THAT(GetOutput(alg), HasSubstr("dr-xr-xr-x"));
    EXPECT_THAT(GetOutput(alg), HasSubstr("-r--r--r--"));
  }

  // Mock directory with JSON long listing
  {
    GDALVSIListAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "json", "-l", "/vsilist_fail_test/mock_dir"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_THAT(GetOutput(alg), HasSubstr("last_modification_date"));
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover remaining unreachable/defensive branches in
// apps/gdalalg_vsi_list.cpp.
