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

#include "third_party/gdal/apps/gdalalg_main.h"

#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

// Mock algorithm for pipeline simulation.
class MockPipelineAlgorithm final : public GDALAlgorithm {
 public:
  static constexpr const char* NAME = "pipeline";
  static constexpr const char* DESCRIPTION = "Mock pipeline algorithm";
  static constexpr const char* HELP_URL = "";

  static std::vector<std::string> GetAliasesStatic() { return {}; }

  MockPipelineAlgorithm() : GDALAlgorithm(NAME, DESCRIPTION, HELP_URL) {}

  bool ParseCommandLineArguments(
      const std::vector<std::string>& args) override {
    if (simulate_raster_vector_error) {
      CPLError(CE_Failure, CPLE_AppDefined,
               "Dataset has both raster and vector content");
      return false;
    }
    return should_succeed;
  }

  std::string GetUsageForCLI(bool shortUsage,
                             const UsageOptions& usageOptions) const override {
    return "Mock pipeline usage";
  }

  static inline bool should_succeed = true;
  static inline bool simulate_raster_vector_error = false;

 protected:
  bool RunImpl(GDALProgressFunc, void*) override { return true; }
};

// Mock algorithm for info simulation.
class MockInfoAlgorithm final : public GDALAlgorithm {
 public:
  static constexpr const char* NAME = "info";
  static constexpr const char* DESCRIPTION = "Mock info algorithm";
  static constexpr const char* HELP_URL = "";

  static std::vector<std::string> GetAliasesStatic() { return {}; }

  MockInfoAlgorithm() : GDALAlgorithm(NAME, DESCRIPTION, HELP_URL) {
    if (add_layer_arg) {
      AddArg("layer", 0, "Layer name", &layer_value_);
    }
  }

  bool ParseCommandLineArguments(
      const std::vector<std::string>& args) override {
    if (simulate_raster_vector_error) {
      CPLError(CE_Failure, CPLE_AppDefined,
               "Dataset has both raster and vector content");
      return false;
    }
    return should_succeed;
  }

  std::string GetUsageForCLI(bool shortUsage,
                             const UsageOptions& usageOptions) const override {
    return "Mock info usage";
  }

  static inline bool should_succeed = true;
  static inline bool simulate_raster_vector_error = false;
  static inline bool add_layer_arg = false;

 protected:
  bool RunImpl(GDALProgressFunc, void*) override { return true; }

 private:
  std::string layer_value_;
};

// Mock algorithm for sub-algorithm testing and typo detection.
class MockSubAlgorithm final : public GDALAlgorithm {
 public:
  static constexpr const char* NAME = "mocksubcommand";
  static constexpr const char* DESCRIPTION = "Mock subcommand";
  static constexpr const char* HELP_URL = "";

  static std::vector<std::string> GetAliasesStatic() { return {}; }

  MockSubAlgorithm() : GDALAlgorithm(NAME, DESCRIPTION, HELP_URL) {}

 protected:
  bool RunImpl(GDALProgressFunc, void*) override { return true; }
};

class GdalalgMainTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    auto& registry = GDALGlobalAlgorithmRegistry::GetSingleton();
    registry.Register<MockSubAlgorithm>();
    registry.Register<MockPipelineAlgorithm>();
    registry.Register<MockInfoAlgorithm>();
  }

  void SetUp() override { ResetMocks(); }
  void TearDown() override { ResetMocks(); }

  static void ResetMocks() {
    MockPipelineAlgorithm::should_succeed = true;
    MockPipelineAlgorithm::simulate_raster_vector_error = false;
    MockInfoAlgorithm::should_succeed = true;
    MockInfoAlgorithm::simulate_raster_vector_error = false;
    MockInfoAlgorithm::add_layer_arg = false;
  }
};

TEST_F(GdalalgMainTest, BasicPropertiesAndUsage) {
  GDALMainAlgorithm alg;
  EXPECT_STREQ("gdal", alg.GetName().c_str());
  EXPECT_STREQ("Main gdal entry point.", alg.GetDescription().c_str());
  EXPECT_STREQ("/programs/index.html", alg.GetHelpURL().c_str());

  const std::string usage = alg.GetUsageForCLI(/*shortUsage=*/false, {});
  EXPECT_NE(std::string::npos, usage.find("gdal <FILENAME>"));
}

TEST_F(GdalalgMainTest, VersionAndDriversArguments) {
  {
    GDALMainAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments({"--version"}));
  }

  {
    GDALMainAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments({"--drivers"}));
    EXPECT_TRUE(alg.Run());
    const std::string output =
        alg.GetArg(GDAL_ARG_NAME_OUTPUT_STRING)->Get<std::string>();
    EXPECT_FALSE(output.empty());
  }

  {
    GDALMainAlgorithm alg;
    EXPECT_TRUE(alg.Run());
  }
}

TEST_F(GdalalgMainTest, DoubleDashOptions) {
  GDALMainAlgorithm alg;
  EXPECT_TRUE(alg.ParseCommandLineArguments({"--help"}));

  GDALMainAlgorithm alg_bad;
  {
    WithQuietHandler quiet;
    EXPECT_FALSE(alg_bad.ParseCommandLineArguments({"--unknown_option_xyz"}));
  }
}

TEST_F(GdalalgMainTest, SubcommandAndTypoSuggestion) {
  // Valid subcommand.
  {
    GDALMainAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments({"mocksubcommand"}));
  }

  // Subcommand typo triggering "Do you mean 'mocksubcommand'?"
  {
    GDALMainAlgorithm alg;
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.ParseCommandLineArguments({"mocksubcommane"}));
  }
}

TEST_F(GdalalgMainTest, ShortestFormPipelineRead) {
  // Normal success with vector path.
  {
    MockPipelineAlgorithm::should_succeed = true;
    MockPipelineAlgorithm::simulate_raster_vector_error = false;
    GDALMainAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments({"read", "input.tif"}));
    EXPECT_EQ("Mock pipeline usage",
              alg.GetUsageForCLI(/*shortUsage=*/false, {}));
  }

  // Failure with raster/vector error message suppresses usage.
  {
    CPLErrorReset();
    MockPipelineAlgorithm::should_succeed = false;
    MockPipelineAlgorithm::simulate_raster_vector_error = true;
    GDALMainAlgorithm alg;
    {
      WithQuietHandler quiet;
      EXPECT_FALSE(alg.ParseCommandLineArguments({"read", "input.tif"}));
    }
    EXPECT_TRUE(alg.GetUsageForCLI(/*shortUsage=*/false, {}).empty());
  }

  // Normal failure falls through.
  {
    CPLErrorReset();
    MockPipelineAlgorithm::should_succeed = false;
    MockPipelineAlgorithm::simulate_raster_vector_error = false;
    GDALMainAlgorithm alg;
    {
      WithQuietHandler quiet;
      EXPECT_FALSE(alg.ParseCommandLineArguments({"read", "input.tif"}));
    }
  }

  // Only "read" without second arg does not trigger pipeline branch.
  {
    GDALMainAlgorithm alg;
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.ParseCommandLineArguments({"read"}));
  }
}

TEST_F(GdalalgMainTest, ShortestFormInfoDirectFile) {
  const std::string test_dir = "/vsimem/gdalalg_main_test/shortest_info";
  ASSERT_EQ(0, VSIMkdir(test_dir.c_str(), 0755));
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string file_path = test_dir + "/test.tif";
  VSILFILE* fp = VSIFOpenL(file_path.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  ASSERT_EQ(0, VSIFCloseL(fp));

  // Raster path (no "layer" arg in info algorithm).
  {
    MockInfoAlgorithm::add_layer_arg = false;
    MockInfoAlgorithm::should_succeed = true;
    MockInfoAlgorithm::simulate_raster_vector_error = false;
    GDALMainAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments({file_path}));
    EXPECT_EQ("Mock info usage", alg.GetUsageForCLI(/*shortUsage=*/false, {}));
  }

  // Vector path ("layer" arg present in info algorithm).
  {
    MockInfoAlgorithm::add_layer_arg = true;
    MockInfoAlgorithm::should_succeed = true;
    MockInfoAlgorithm::simulate_raster_vector_error = false;
    GDALMainAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments({file_path}));
    EXPECT_EQ("Mock info usage", alg.GetUsageForCLI(/*shortUsage=*/false, {}));
  }

  // Raster/vector error message suppresses usage.
  {
    CPLErrorReset();
    MockInfoAlgorithm::add_layer_arg = false;
    MockInfoAlgorithm::should_succeed = false;
    MockInfoAlgorithm::simulate_raster_vector_error = true;
    GDALMainAlgorithm alg;
    {
      WithQuietHandler quiet;
      EXPECT_FALSE(alg.ParseCommandLineArguments({file_path}));
    }
    EXPECT_TRUE(alg.GetUsageForCLI(/*shortUsage=*/false, {}).empty());
  }

  // Normal failure falls through.
  {
    CPLErrorReset();
    MockInfoAlgorithm::add_layer_arg = false;
    MockInfoAlgorithm::should_succeed = false;
    MockInfoAlgorithm::simulate_raster_vector_error = false;
    GDALMainAlgorithm alg;
    {
      WithQuietHandler quiet;
      EXPECT_FALSE(alg.ParseCommandLineArguments({file_path}));
    }
  }

  // Non-existent file falls through.
  {
    GDALMainAlgorithm alg;
    WithQuietHandler quiet;
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({test_dir + "/nonexistent.tif"}));
  }
}

}  // namespace
}  // namespace autotest2
