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
// Tests for GDALVSIAlgorithm (apps/gdalalg_vsi.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vsi.cpp
//   https://gdal.org/programs/gdal_vsi.html

#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::Contains;
using ::testing::HasSubstr;
using ::testing::UnorderedElementsAre;

constexpr char kTestDir[] = "/vsimem/gdalalg_vsi_test";

void WriteVsimemFile(const std::string& path, const std::string& content) {
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
  ASSERT_EQ(content.size(), VSIFWriteL(content.data(), 1, content.size(), fp));
}

class GdalalgVsiTest : public ::testing::Test {
 protected:
  void SetUp() override {
    VSIRmdirRecursive(kTestDir);
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgVsiTest, BasicPropertiesSubAlgorithmsAndUsage) {
  auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("vsi");
  ASSERT_NE(nullptr, alg);
  EXPECT_EQ("vsi", alg->GetName());
  EXPECT_EQ("GDAL Virtual System Interface (VSI) commands.",
            alg->GetDescription());
  EXPECT_EQ("/programs/gdal_vsi.html", alg->GetHelpURL());
  EXPECT_TRUE(alg->HasSubAlgorithms());

  EXPECT_THAT(
      alg->GetSubAlgorithmNames(),
      UnorderedElementsAre("copy", "delete", "list", "move", "sync", "sozip"));

  const std::string short_usage = alg->GetUsageForCLI(/*shortUsage=*/true, {});
  EXPECT_THAT(short_usage, HasSubstr("copy"));
  EXPECT_THAT(short_usage, HasSubstr("list"));

  const std::string full_usage = alg->GetUsageForCLI(/*shortUsage=*/false, {});
  EXPECT_THAT(full_usage, HasSubstr("delete"));
  EXPECT_THAT(full_usage, HasSubstr("sozip"));

  const std::string json_usage = alg->GetUsageAsJSON();
  EXPECT_THAT(json_usage, HasSubstr("GDAL Virtual System Interface"));
  EXPECT_THAT(json_usage, HasSubstr("\"sync\""));

  std::vector<std::string> ac_root;
  EXPECT_THAT(alg->GetAutoComplete(ac_root, /*lastWordIsComplete=*/true,
                                   /*showAllOptions=*/false),
              Contains("list"));

  std::vector<std::string> ac_sub = {"list", "--"};
  EXPECT_THAT(alg->GetAutoComplete(ac_sub, /*lastWordIsComplete=*/false,
                                   /*showAllOptions=*/false),
              Contains("--long-listing"));
}

TEST_F(GdalalgVsiTest, DirectRunAndUnknownSubcommand) {
  {
    auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("vsi");
    ASSERT_NE(nullptr, alg);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg->Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        HasSubstr("The Run() method should not be called directly on the "
                  "\"gdal vsi\" program."));
    CPLErrorReset();
  }

  {
    auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("vsi");
    ASSERT_NE(nullptr, alg);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg->ParseCommandLineArguments({"unknown_subcmd"}));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST_F(GdalalgVsiTest, ExecuteSubcommands) {
  const std::string src_file = std::string(kTestDir) + "/src.txt";
  const std::string copy_file = std::string(kTestDir) + "/copy.txt";
  const std::string moved_file = std::string(kTestDir) + "/moved.txt";
  ASSERT_NO_FATAL_FAILURE(WriteVsimemFile(src_file, "hello vsi"));

  // 1. list
  {
    auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("vsi");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments({"list", src_file}));
    ASSERT_TRUE(alg->Run());
    const auto* out_arg =
        alg->GetActualAlgorithm().GetArg(GDAL_ARG_NAME_OUTPUT_STRING);
    ASSERT_NE(nullptr, out_arg);
    EXPECT_THAT(out_arg->Get<std::string>(), HasSubstr(src_file));
  }

  // 2. copy
  {
    auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("vsi");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments({"copy", src_file, copy_file}));
    ASSERT_TRUE(alg->Run());
    VSIStatBufL sStat;
    EXPECT_EQ(0, VSIStatL(copy_file.c_str(), &sStat));
  }

  // 3. move
  {
    auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("vsi");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(
        alg->ParseCommandLineArguments({"move", copy_file, moved_file}));
    ASSERT_TRUE(alg->Run());
    VSIStatBufL sStat;
    EXPECT_NE(0, VSIStatL(copy_file.c_str(), &sStat));
    EXPECT_EQ(0, VSIStatL(moved_file.c_str(), &sStat));
  }

  // 4. delete
  {
    auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("vsi");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments({"delete", moved_file}));
    ASSERT_TRUE(alg->Run());
    VSIStatBufL sStat;
    EXPECT_NE(0, VSIStatL(moved_file.c_str(), &sStat));
  }
}

}  // namespace
}  // namespace autotest2
