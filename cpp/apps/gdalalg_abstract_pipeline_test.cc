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
// Tests for GDALAbstractPipelineAlgorithm and GDALPipelineStepAlgorithm
// (apps/gdalalg_abstract_pipeline.cpp and apps/gdalalg_abstract_pipeline.h).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_abstract_pipeline.cpp

#include "third_party/gdal/apps/gdalalg_abstract_pipeline.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_raster_pipeline.h"
#include "third_party/gdal/apps/gdalalg_vector_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::Contains;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;

constexpr char kTestDir[] = "/vsimem/gdalalg_abstract_pipeline_test";

void WriteVsimemFile(const std::string& path, const std::string& content) {
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  const size_t written = VSIFWriteL(content.data(), 1, content.size(), fp);
  ASSERT_EQ(0, VSIFCloseL(fp));
  ASSERT_EQ(content.size(), written);
}

void CreateTinyTiff(const std::string& path, bool georeferenced = true) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  if (georeferenced) {
    OGRSpatialReference srs;
    ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
    ds->SetSpatialRef(&srs);
    double gt[6] = {0.0, 0.01, 0.0, 1.0, 0.0, -0.01};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));
  }
  std::vector<uint8_t> pixels(16, 42);
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 4, pixels.data(),
                                           4, 4, GDT_Byte, 0, 0, nullptr));
}

void CreateTinyGeoJson(const std::string& path) {
  WriteVsimemFile(path,
                  R"json({
  "type": "FeatureCollection",
  "features": [
    {
      "type": "Feature",
      "geometry": { "type": "Point", "coordinates": [1.0, 2.0] },
      "properties": { "id": 1 }
    }
  ]
})json");
}

// Custom pipeline step that acts as a first step producing a MEM raster.
class DummyReadStep final : public GDALPipelineStepAlgorithm {
 public:
  static constexpr const char* NAME = "dummy-read";
  static constexpr const char* DESCRIPTION = "Dummy read step";
  static constexpr const char* HELP_URL = "";

  DummyReadStep()
      : GDALPipelineStepAlgorithm(NAME, DESCRIPTION, HELP_URL,
                                  ConstructorOptions()) {
    AddArg("input", 0, "Input", &m_inputDataset, GDAL_OF_RASTER)
        .SetPositional();
  }

  int GetInputType() const override { return GDAL_OF_RASTER; }
  int GetOutputType() const override { return GDAL_OF_RASTER; }

 protected:
  bool CanBeFirstStep() const override { return true; }

  bool RunStep(GDALPipelineStepRunContext&) override {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
    if (!drv) return false;
    auto ds = std::unique_ptr<GDALDataset>(
        drv->Create("", 2, 2, 1, GDT_Byte, nullptr));
    if (!ds) return false;
    m_outputDataset.Set(std::move(ds));
    return true;
  }
};

// Custom pipeline step that fails to produce an output dataset.
class NoOutputDatasetStep final : public GDALPipelineStepAlgorithm {
 public:
  static constexpr const char* NAME = "no-output";
  static constexpr const char* DESCRIPTION = "Step returning no dataset";
  static constexpr const char* HELP_URL = "";

  NoOutputDatasetStep()
      : GDALPipelineStepAlgorithm(NAME, DESCRIPTION, HELP_URL,
                                  ConstructorOptions()) {
    AddArg("nested", 0, "Nested dataset", &m_nested, GDAL_OF_RASTER)
        .SetPositional();
  }

  int GetInputType() const override { return GDAL_OF_RASTER; }
  int GetOutputType() const override { return GDAL_OF_RASTER; }

 protected:
  bool CanBeFirstStep() const override { return true; }
  bool CanBeLastStep() const override { return false; }

  bool RunStep(GDALPipelineStepRunContext&) override { return true; }

 private:
  GDALArgDatasetValue m_nested{};
};

// Custom pipeline step with a stdout arg so RunStep allows empty output
// dataset, letting BuildNestedPipeline hit its nullptr output dataset guard.
class NoOutputStdoutStep final : public GDALPipelineStepAlgorithm {
 public:
  static constexpr const char* NAME = "no-output-stdout";
  static constexpr const char* DESCRIPTION = "Step with stdout arg";
  static constexpr const char* HELP_URL = "";

  NoOutputStdoutStep()
      : GDALPipelineStepAlgorithm(NAME, DESCRIPTION, HELP_URL,
                                  ConstructorOptions()) {
    AddArg(GDAL_ARG_NAME_STDOUT, 0, "Stdout", &m_stdout);
  }

  int GetInputType() const override { return GDAL_OF_RASTER; }
  int GetOutputType() const override { return GDAL_OF_RASTER; }

 protected:
  bool CanBeFirstStep() const override { return true; }
  bool CanBeMiddleStep() const override { return true; }
  bool CanBeLastStep() const override { return false; }

  bool RunStep(GDALPipelineStepRunContext&) override { return true; }
};

// Custom step that is neither first nor last nor middle, to exercise the
// CheckFirstAndLastStep intermediate error path.
class RestrictedStep final : public GDALPipelineStepAlgorithm {
 public:
  static constexpr const char* NAME = "restricted";
  static constexpr const char* DESCRIPTION = "Restricted step";
  static constexpr const char* HELP_URL = "";

  RestrictedStep()
      : GDALPipelineStepAlgorithm(NAME, DESCRIPTION, HELP_URL,
                                  ConstructorOptions()) {}

  int GetInputType() const override { return GDAL_OF_RASTER; }
  int GetOutputType() const override { return GDAL_OF_RASTER; }

 protected:
  bool CanBeFirstStep() const override { return false; }
  bool CanBeMiddleStep() const override { return false; }
  bool CanBeLastStep() const override { return false; }

  bool RunStep(GDALPipelineStepRunContext&) override { return true; }
};

// Custom step that produces an output string.
class DummyInfoStep final : public GDALPipelineStepAlgorithm {
 public:
  static constexpr const char* NAME = "dummy-info";
  static constexpr const char* DESCRIPTION = "Dummy info step";
  static constexpr const char* HELP_URL = "";

  DummyInfoStep()
      : GDALPipelineStepAlgorithm(NAME, DESCRIPTION, HELP_URL,
                                  ConstructorOptions()) {
    AddOutputStringArg(&m_output);
  }

  int GetInputType() const override { return GDAL_OF_RASTER; }
  int GetOutputType() const override { return GDAL_OF_RASTER; }

  bool HasOutputString() const override { return true; }

 protected:
  bool CanBeLastStep() const override { return true; }

  bool RunStep(GDALPipelineStepRunContext&) override {
    m_output = "dummy-info-output";
    return true;
  }
};

// Minimal concrete subclass of GDALAbstractPipelineAlgorithm for protected API
// and synthetic step error paths.
class TestConcretePipelineAlgorithm final
    : public GDALAbstractPipelineAlgorithm {
 public:
  static constexpr const char* NAME = "test-pipeline";
  static constexpr const char* DESCRIPTION = "Test concrete pipeline";
  static constexpr const char* HELP_URL = "";

  TestConcretePipelineAlgorithm()
      : GDALAbstractPipelineAlgorithm(
            NAME, DESCRIPTION, HELP_URL,
            ConstructorOptions().SetStandaloneStep(false)) {
    AddArg("pipeline", 0, "Pipeline", &m_pipeline).SetPositional();
    AddOutputStringArg(&m_output);
    m_stepRegistry.Register<DummyReadStep>();
    m_stepRegistry.Register<NoOutputDatasetStep>();
    m_stepRegistry.Register<NoOutputStdoutStep>();
    m_stepRegistry.Register<RestrictedStep>();
    m_stepRegistry.Register<DummyInfoStep>();
  }

  int GetInputType() const override { return GDAL_OF_RASTER; }
  int GetOutputType() const override { return GDAL_OF_RASTER; }

  using GDALAbstractPipelineAlgorithm::GetStepAlg;
  using GDALAbstractPipelineAlgorithm::HasOutputString;
  using GDALAbstractPipelineAlgorithm::IsReadSpecificArgument;
  using GDALAbstractPipelineAlgorithm::IsWriteSpecificArgument;

 protected:
  GDALAlgorithmRegistry& GetStepRegistry() override { return m_stepRegistry; }
  const GDALAlgorithmRegistry& GetStepRegistry() const override {
    return m_stepRegistry;
  }

 private:
  std::unique_ptr<GDALAbstractPipelineAlgorithm> CreateNestedPipeline()
      const override {
    return std::make_unique<TestConcretePipelineAlgorithm>();
  }

  GDALAlgorithmRegistry m_stepRegistry{};
};

std::unique_ptr<GDALAbstractPipelineAlgorithm> MakeGenericPipeline() {
  auto alg =
      GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("pipeline");
  return std::unique_ptr<GDALAbstractPipelineAlgorithm>(
      cpl::down_cast<GDALAbstractPipelineAlgorithm*>(alg.release()));
}

class GdalalgAbstractPipelineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    GDALRegister_VRT();
    GDALRegister_GDALG();
    GDALRegister_PNG();
    RegisterOGRGeoJSON();
    RegisterOGRGeoPackage();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
    tmpdir_setter_ = std::make_unique<CPLConfigOptionSetter>(
        "CPL_TMPDIR", kTestDir, /*bSetOnlyIfUndefined=*/false);
  }

  void TearDown() override {
    tmpdir_setter_.reset();
    VSIRmdirRecursive(kTestDir);
  }

  std::unique_ptr<CPLConfigOptionSetter> tmpdir_setter_;
};

TEST_F(GdalalgAbstractPipelineTest, ConcreteSubclassProtectedHelpersAndSteps) {
  EXPECT_TRUE(TestConcretePipelineAlgorithm::IsReadSpecificArgument(
      GDAL_ARG_NAME_INPUT));
  EXPECT_TRUE(TestConcretePipelineAlgorithm::IsReadSpecificArgument(
      GDAL_ARG_NAME_INPUT_FORMAT));
  EXPECT_TRUE(TestConcretePipelineAlgorithm::IsReadSpecificArgument(
      GDAL_ARG_NAME_OPEN_OPTION));
  EXPECT_TRUE(TestConcretePipelineAlgorithm::IsReadSpecificArgument(
      GDAL_ARG_NAME_INPUT_LAYER));
  EXPECT_FALSE(
      TestConcretePipelineAlgorithm::IsReadSpecificArgument("unknown"));

  EXPECT_TRUE(TestConcretePipelineAlgorithm::IsWriteSpecificArgument(
      GDAL_ARG_NAME_OUTPUT));
  EXPECT_TRUE(TestConcretePipelineAlgorithm::IsWriteSpecificArgument(
      GDAL_ARG_NAME_OUTPUT_FORMAT));
  EXPECT_TRUE(TestConcretePipelineAlgorithm::IsWriteSpecificArgument(
      GDAL_ARG_NAME_OVERWRITE));
  EXPECT_FALSE(
      TestConcretePipelineAlgorithm::IsWriteSpecificArgument("unknown"));

  {
    TestConcretePipelineAlgorithm alg;
    EXPECT_NE(nullptr, alg.GetStepAlg("dummy-read"));
    EXPECT_EQ(nullptr, alg.GetStepAlg("does-not-exist"));
    EXPECT_FALSE(alg.HasOutputString());
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"dummy-read", "!", "dummy-info"}));
    EXPECT_TRUE(alg.HasOutputString());
    EXPECT_TRUE(alg.Run());
    EXPECT_EQ("dummy-info-output", alg.GetOutputString());
    EXPECT_TRUE(alg.Finalize());
  }

  // Restricted intermediate step fails CheckFirstAndLastStep.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    TestConcretePipelineAlgorithm alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"dummy-read", "!", "restricted", "!", "dummy-info"}));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("is not allowed as an intermediate step"));
    CPLErrorReset();
  }

  // Step that fails to produce an output dataset fails during Run().
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    TestConcretePipelineAlgorithm alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"no-output", "!", "dummy-info"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("failed to produce an output dataset"));
    CPLErrorReset();
  }

  // Nested pipeline whose step fails to produce an output dataset.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    TestConcretePipelineAlgorithm alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"no-output", "[", "no-output", "]", "!", "dummy-info"}));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("failed to produce an output dataset"));
    CPLErrorReset();
  }

  // Nested pipeline that succeeds via stdout arg without an output dataset.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    TestConcretePipelineAlgorithm alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"no-output", "[", "no-output-stdout", "]", "!", "dummy-info"}));
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        HasSubstr("Nested pipeline does not generate an output dataset"));
    CPLErrorReset();
  }

  // Empty 'pipeline' arg fails Run().
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    TestConcretePipelineAlgorithm alg;
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("'pipeline' argument not set"));
    CPLErrorReset();
  }
}

TEST_F(GdalalgAbstractPipelineTest, RasterPipelineBasicHelpUsageAndJson) {
  const std::string in_tif = std::string(kTestDir) + "/in.tif";
  const std::string out_tif = std::string(kTestDir) + "/out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(in_tif));

  {
    GDALRasterPipelineAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments({"--help"}));
    EXPECT_FALSE(alg.GetUsageAsJSON().empty());
  }
  {
    GDALRasterPipelineAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments({"--json-usage"}));
  }
  {
    GDALRasterPipelineAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments({"--help-doc=read"}));
  }

  // Pipeline string passed via single "read ..." argument or --pipeline.
  {
    GDALRasterPipelineAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"read " + in_tif + " ! write " + out_tif + " --overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    GDALRasterPipelineAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--pipeline", "read " + in_tif +
                           " ! scale --src-min 0 --src-max 255 ! write " +
                           out_tif + " --overwrite"}));
    EXPECT_TRUE(alg.Run());
  }
  {
    GDALRasterPipelineAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--pipeline=read " + in_tif + " ! info"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_THAT(alg.GetOutputString(), HasSubstr("in.tif"));
  }

  // Called twice fails.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"read", in_tif, "!", "info"}));
    EXPECT_FALSE(alg.ParseCommandLineArguments({"read", in_tif, "!", "info"}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("can only be called once"));
    CPLErrorReset();
  }
}

TEST_F(GdalalgAbstractPipelineTest, StepOrderingAndBracketErrors) {
  const std::string in_tif = std::string(kTestDir) + "/order.tif";
  const std::string out_tif = std::string(kTestDir) + "/order_out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(in_tif));

  // Unknown step.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments({"nonexistent_step"}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("unknown step name"));
    CPLErrorReset();
  }

  // First step cannot be a middle/last step.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments({"info"}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("First step should be"));
    CPLErrorReset();
  }

  // Read-like step inside tee inner pipeline is forbidden (validated when tee
  // runs its nested pipeline).
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"read", in_tif, "!", "tee", "[", "read", in_tif, "]", "!", "info"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("No read-like step"));
    CPLErrorReset();
  }

  // Write-like step inside mosaic nested pipeline is forbidden.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm alg;
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"mosaic", "[", "read", in_tif, "!",
                                       "write", out_tif, "]", "!", "info"}));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("must not be a write-like step"));
    CPLErrorReset();
  }

  // Read as last step.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm alg;
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"read", in_tif, "!", "read", in_tif}));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("is only allowed as a first step"));
    CPLErrorReset();
  }

  // CLI mode requires >= 2 steps and last step must be write-like.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm alg;
    alg.SetCalledFromCommandLine();
    EXPECT_FALSE(alg.ParseCommandLineArguments({"read", in_tif}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("At least 2 steps"));
    CPLErrorReset();
  }
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm alg;
    alg.SetCalledFromCommandLine();
    EXPECT_FALSE(alg.ParseCommandLineArguments({"read", in_tif, "!", "scale"}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Last step should be"));
    CPLErrorReset();
  }
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm alg;
    alg.SetCalledFromCommandLine();
    EXPECT_FALSE(alg.ParseCommandLineArguments({"read", in_tif, "--help=?"}));
    CPLErrorReset();
  }

  // CLI help for specific step and empty steps.
  {
    GDALRasterPipelineAlgorithm alg;
    alg.SetCalledFromCommandLine();
    EXPECT_TRUE(alg.ParseCommandLineArguments({"read", "--help"}));
    EXPECT_TRUE(alg.Run());
  }
  {
    GDALRasterPipelineAlgorithm alg;
    alg.SetCalledFromCommandLine();
    EXPECT_TRUE(alg.ParseCommandLineArguments({"!", "--help"}));
  }

  // Bracket errors.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments({"["}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Open bracket must be placed"));
    CPLErrorReset();
  }
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments({"read", in_tif, "]"}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Closing bracket found"));
    CPLErrorReset();
  }
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm alg;
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"mosaic", "[", "read", in_tif}));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Open bracket has no matching closing bracket"));
    CPLErrorReset();
  }
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm alg;
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"mosaic", "[", "mosaic", "[", "mosaic",
                                       "[", "read", in_tif, "]", "]", "]"}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Too many nested pipelines"));
    CPLErrorReset();
  }

  // Intermediate read-like or write-like step triggers CheckFirstAndLastStep
  // diagnostics (note: CheckFirstAndLastStep emits CE_Failure and continues,
  // so capture messages via CPLPushErrorHandlerEx before subsequent resets).
  auto capture_errors = [](const auto& run_fn) {
    std::vector<std::string> errs;
    auto handler = [](CPLErr, CPLErrorNum, const char* msg) {
      auto* out =
          static_cast<std::vector<std::string>*>(CPLGetErrorHandlerUserData());
      if (msg) out->emplace_back(msg);
    };
    CPLPushErrorHandlerEx(handler, &errs);
    run_fn();
    CPLPopErrorHandler();
    CPLErrorReset();
    return errs;
  };
  {
    const auto errs = capture_errors([&] {
      GDALRasterPipelineAlgorithm alg;
      (void)alg.ParseCommandLineArguments(
          {"read", in_tif, "!", "read", in_tif, "!", "info"});
    });
    EXPECT_THAT(errs, Contains(HasSubstr("Only first step can be 'read'")));
  }
  {
    const auto errs = capture_errors([&] {
      GDALRasterPipelineAlgorithm alg;
      (void)alg.ParseCommandLineArguments(
          {"read", in_tif, "!", "write", out_tif, "!", "info"});
    });
    EXPECT_THAT(errs, Contains(HasSubstr("Only last step can be 'write'")));
  }

  // Empty step list in non-CLI mode fails.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments({"!"}));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("At least one step must be provided"));
    CPLErrorReset();
  }

  // CLI mode with >= 2 steps where last step cannot be last and help '=?'
  // is requested.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm alg;
    alg.SetCalledFromCommandLine();
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"read", in_tif, "!", "scale", "--help=?"}));
    CPLErrorReset();
  }

  // PROJ syntax unknown step name.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm alg;
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"+gdal=nonexistent_proj_step"}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("unknown step name"));
    CPLErrorReset();
  }
}

TEST_F(GdalalgAbstractPipelineTest, NestedPipelinesProjSyntaxAndTee) {
  const std::string in_tif = std::string(kTestDir) + "/nest.tif";
  const std::string side_tif = std::string(kTestDir) + "/side.tif";
  const std::string out_tif = std::string(kTestDir) + "/nest_out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(in_tif));

  // Nested pipeline inside clip --like (including double nesting level 2 < 3).
  {
    GDALRasterPipelineAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"read", in_tif, "!",    "clip",   "--like", "[",          "read",
         in_tif, "!",    "clip", "--like", "[",      "read",       in_tif,
         "]",    "]",    "|",    "write",  out_tif,  "--overwrite"}));
    EXPECT_TRUE(alg.Run());
  }

  // Duplicate identical nested pipeline in tee triggers error.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"read", in_tif, "!", "tee", "[", "write", side_tif, "--overwrite", "]",
         "[", "write", side_tif, "--overwrite", "]", "!", "info"}));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Another identical nested pipeline exists"));
    CPLErrorReset();
  }

  // PROJ nostalgia syntax (+step, +gdal=..., +overwrite) and /vsistdout/.
  {
    GDALRasterPipelineAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"+gdal=read", in_tif, "+step", "+write", out_tif, "+overwrite"}));
    EXPECT_TRUE(alg.Run());
  }

  // --progress and --quiet flags.
  {
    GDALRasterPipelineAlgorithm alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--progress", "--quiet", "read", in_tif,
                                       "!", "write", out_tif, "--overwrite"}));
    int progress_calls = 0;
    auto cb = [](double, const char*, void* p) -> int {
      ++(*static_cast<int*>(p));
      return TRUE;
    };
    EXPECT_TRUE(alg.Run(cb, &progress_calls));
    EXPECT_GT(progress_calls, 0);
  }

  // /vsistdout/ automatically sets quiet.
  {
    GDALRasterPipelineAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"read", in_tif, "!", "write", "/vsistdout/", "-f", "GTiff"}));
    const GDALAlgorithmArg* quiet_arg = alg.GetArg(GDAL_ARG_NAME_QUIET);
    ASSERT_NE(nullptr, quiet_arg);
    EXPECT_TRUE(quiet_arg->Get<bool>());
  }

  // Step implementing CanHandleNextStep() (reproject -> write).
  {
    const std::string reproj_tif = std::string(kTestDir) + "/reproj_out.tif";
    GDALRasterPipelineAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"read", in_tif, "!", "reproject", "--dst-crs", "EPSG:3857", "!",
         "write", reproj_tif, "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  // CLI mode with HasOutputString() disables progress callback in RunStep().
  {
    GDALRasterPipelineAlgorithm alg;
    alg.SetCalledFromCommandLine();
    ASSERT_TRUE(alg.ParseCommandLineArguments({"read", in_tif, "!", "info"}));
    int calls = 0;
    auto cb = [](double, const char*, void* p) -> int {
      ++(*static_cast<int*>(p));
      return TRUE;
    };
    ::testing::internal::CaptureStdout();
    EXPECT_TRUE(alg.Run(cb, &calls));
    (void)::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, calls);
  }
}

TEST_F(GdalalgAbstractPipelineTest, GenericMixedPipelineAndSubAlgorithms) {
  const std::string in_tif = std::string(kTestDir) + "/mix.tif";
  const std::string out_tif = std::string(kTestDir) + "/mix_out.tif";
  const std::string in_geojson = std::string(kTestDir) + "/mix.geojson";
  const std::string out_gpkg = std::string(kTestDir) + "/mix_out.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(in_tif));
  ASSERT_NO_FATAL_FAILURE(CreateTinyGeoJson(in_geojson));

  // Raster mixed pipeline.
  {
    auto alg = MakeGenericPipeline();
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments(
        {"read", in_tif, "!", "scale", "--src-min", "0", "--src-max", "255",
         "!", "write", out_tif, "--overwrite"}));
    EXPECT_TRUE(alg->Run());
  }

  // Vector mixed pipeline with sub-algorithm (grid average) or tee/buffer.
  {
    auto alg = MakeGenericPipeline();
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments({"read", in_geojson, "!",
                                                "buffer", "1.0", "!", "write",
                                                out_gpkg, "--overwrite"}));
    EXPECT_TRUE(alg->Run());
  }

  // Sub-algorithm in pipeline (grid average) + unknown sub-algorithm error.
  {
    auto alg = MakeGenericPipeline();
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments(
        {"read", in_geojson, "!", "grid", "average", "--zfield", "id", "--size",
         "4,4", "!", "write", out_tif, "--overwrite"}));
    EXPECT_TRUE(alg->Run());
  }
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    auto alg = MakeGenericPipeline();
    ASSERT_NE(nullptr, alg);
    EXPECT_FALSE(alg->ParseCommandLineArguments(
        {"read", in_geojson, "!", "grid", "no_such_subalg"}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("unknown sub-algorithm"));
    CPLErrorReset();
  }

  // Type mismatch error: raster read into vector buffer step.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    auto alg = MakeGenericPipeline();
    ASSERT_NE(nullptr, alg);
    EXPECT_FALSE(
        alg->ParseCommandLineArguments({"read", in_tif, "!", "buffer", "1.0"}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("expects a vector input"));
    CPLErrorReset();
  }

  // Error replay when both raster and vector step parse fail on invalid flag.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    auto alg = MakeGenericPipeline();
    ASSERT_NE(nullptr, alg);
    EXPECT_FALSE(alg->ParseCommandLineArguments(
        {"read", "/vsimem/does_not_exist_xyz.foo", "!", "info"}));
    CPLErrorReset();
  }
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    auto alg = MakeGenericPipeline();
    ASSERT_NE(nullptr, alg);
    EXPECT_FALSE(alg->ParseCommandLineArguments(
        {"read", in_tif, "!", "scale", "--unknown_opt_xyz"}));
    CPLErrorReset();
  }
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    auto alg = MakeGenericPipeline();
    ASSERT_NE(nullptr, alg);
    EXPECT_FALSE(alg->ParseCommandLineArguments(
        {"read", in_geojson, "!", "buffer", "--unknown_opt_xyz"}));
    CPLErrorReset();
  }

  // Upstream bug in GDALAbstractPipelineAlgorithm: generic pipeline parses dual
  // steps like 'clip' once (consuming $$nested_pipeline_...$$ from
  // m_oMapDatasetNameToDataset) and then re-instantiates 'clip-raster' without
  // preserving m_oMapDatasetNameToDataset. Test non-tee nested bracket via
  // GDALRasterPipelineAlgorithm and dual-step re-instantiation via
  // MakeGenericPipeline().
  {
    GDALRasterPipelineAlgorithm alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"read", in_tif, "!", "clip", "--like",
                                       "[", "read", in_tif, "]", "!", "info"}));
    EXPECT_TRUE(alg.Run());
  }
  {
    auto alg = MakeGenericPipeline();
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(
        alg->ParseCommandLineArguments({"read", in_tif, "!", "clip", "--bbox",
                                        "0.01,0.97,0.03,0.99", "!", "info"}));
    EXPECT_TRUE(alg->Run());
  }
  {
    const std::string side_tif = std::string(kTestDir) + "/mix_tee.tif";
    auto alg = MakeGenericPipeline();
    ASSERT_NE(nullptr, alg);
    alg->SetCalledFromCommandLine();
    ASSERT_TRUE(alg->ParseCommandLineArguments(
        {"read", in_tif, "!", "tee", "[", "write", side_tif, "--overwrite", "]",
         "!", "write", out_tif, "--overwrite"}));
    EXPECT_TRUE(alg->Run());
    EXPECT_TRUE(alg->Finalize());
  }
}

TEST_F(GdalalgAbstractPipelineTest, VectorOgrSchemaOptimization) {
  const std::string in_geojson = std::string(kTestDir) + "/schema.geojson";
  ASSERT_NO_FATAL_FAILURE(CreateTinyGeoJson(in_geojson));

  GDALVectorPipelineAlgorithm alg;
  ASSERT_TRUE(alg.ParseCommandLineArguments(
      {"read", in_geojson, "!", "set-field-type", "--field-name", "id",
       "--field-type", "String", "!", "info"}));
  EXPECT_TRUE(alg.Run());
  EXPECT_THAT(alg.GetOutputString(), HasSubstr("String"));
}

TEST_F(GdalalgAbstractPipelineTest, SaveGdalgVrtStreamedAndExistingJson) {
  const std::string in_tif = std::string(kTestDir) + "/gdalg_in.tif";
  const std::string out_tif = std::string(kTestDir) + "/gdalg_out.tif";
  const std::string gdalg_path = std::string(kTestDir) + "/pipe.gdalg.json";
  const std::string bad_json = std::string(kTestDir) + "/bad.gdalg.json";
  const std::string out_vrt = std::string(kTestDir) + "/too_long.vrt";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(in_tif));

  // Save .gdalg.json (including non-streaming step warning from sieve).
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm alg;
    alg.SetCallPath({"gdal", "raster", "pipeline"});
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"read", in_tif, "!", "sieve", "!", "scale", "--src-min", "0",
         "--src-max", "90", "!", "write", gdalg_path}));
    EXPECT_TRUE(alg.Run());
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Existing .gdalg.json without --overwrite fails; with --overwrite succeeds.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"read", in_tif, "!", "write", gdalg_path}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("already exists"));
    CPLErrorReset();
  }
  {
    GDALRasterPipelineAlgorithm alg;
    alg.SetCallPath({"gdal", "raster", "pipeline"});
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"read", in_tif, "!", "scale", "--src-min", "0", "--src-max", "255",
         "!", "scale", "--src-min", "0", "--src-max", "255", "!", "write",
         gdalg_path, "--overwrite"}));
    EXPECT_TRUE(alg.Run());
  }

  // Run existing .gdalg.json with overrides (ambiguous and unambiguous) via
  // GDALPipelineAlgorithm, which allows arbitrary long-name override args.
  {
    auto alg = MakeGenericPipeline();
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments(
        {"--pipeline", gdalg_path, "--input", in_tif, "--output", out_tif,
         "--overwrite", "--scale[0].src-min", "1", "--scale[1].src-max=200"}));
    EXPECT_TRUE(alg->Run());
    EXPECT_TRUE(alg->Finalize());
  }

  // Non-existent .json pipeline fails.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {std::string(kTestDir) + "/no_such_pipe.gdalg.json"}));
    CPLErrorReset();
  }

  // Existing .gdalg.json with streamed write args, --arg=val, and positional
  // input arg overridden by user.
  {
    const std::string stream_gdalg =
        std::string(kTestDir) + "/stream_pipe.gdalg.json";
    ASSERT_NO_FATAL_FAILURE(WriteVsimemFile(
        stream_gdalg,
        "{\"type\":\"gdal_streamed_alg\",\"command_line\":\"gdal raster "
        "pipeline read " +
            in_tif +
            " ! scale --src-min=0 --src-max=255 ! write "
            "--output-format=stream streamed_dataset\"}"));
    auto alg = MakeGenericPipeline();
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments(
        {"--pipeline", stream_gdalg, "--input", in_tif, "--src-min", "1",
         "--output", out_tif, "--overwrite"}));
    EXPECT_TRUE(alg->Run());
    EXPECT_TRUE(alg->Finalize());
  }

  // Ambiguous argument across 2 identical steps fails.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    auto alg = MakeGenericPipeline();
    ASSERT_NE(nullptr, alg);
    EXPECT_FALSE(alg->ParseCommandLineArguments({"--pipeline=" + gdalg_path,
                                                 "--src-min=1", "--output",
                                                 out_tif, "--overwrite"}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Ambiguous argument name"));
    CPLErrorReset();
  }

  // Ambiguous step name across 2 identical steps fails.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    auto alg = MakeGenericPipeline();
    ASSERT_NE(nullptr, alg);
    EXPECT_FALSE(alg->ParseCommandLineArguments(
        {"--pipeline=" + gdalg_path, "--scale.src-min=1", "--output", out_tif,
         "--overwrite"}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Qualify it as"));
    CPLErrorReset();
  }

  // Non-existent arg / invalid format / non-existent step in JSON override.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    auto alg = MakeGenericPipeline();
    ASSERT_NE(nullptr, alg);
    EXPECT_FALSE(alg->ParseCommandLineArguments({"--pipeline=" + gdalg_path,
                                                 "--no-such-arg=1", "--output",
                                                 out_tif, "--overwrite"}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("No step in the pipeline"));
    CPLErrorReset();
  }
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    auto alg = MakeGenericPipeline();
    ASSERT_NE(nullptr, alg);
    EXPECT_FALSE(
        alg->ParseCommandLineArguments({"--pipeline=" + gdalg_path, "--a.b.c=1",
                                        "--output", out_tif, "--overwrite"}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid argument name"));
    CPLErrorReset();
  }
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    auto alg = MakeGenericPipeline();
    ASSERT_NE(nullptr, alg);
    EXPECT_FALSE(alg->ParseCommandLineArguments({"--pipeline=" + gdalg_path,
                                                 "--nostep.arg=1", "--output",
                                                 out_tif, "--overwrite"}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("non-existing"));
    CPLErrorReset();
  }

  // Existing JSON missing command_line fails.
  ASSERT_NO_FATAL_FAILURE(
      WriteVsimemFile(bad_json, R"json({"type": "gdal_streamed_alg"})json"));
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments({"--pipeline=" + bad_json}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("command_line missing"));
    CPLErrorReset();
  }

  // VRT output with > 3 steps fails; VRT with non-VRT-compatible step fails.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"read", in_tif, "!", "scale",
                                               "!", "scale", "!", "write",
                                               out_vrt, "--overwrite"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("more than 3 steps"));
    CPLErrorReset();
  }
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"read", in_tif, "!", "slope", "!", "write", out_vrt, "--overwrite"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("VRT output is not supported"));
    CPLErrorReset();
  }

  // Streamed output restrictions.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm alg;
    alg.SetExecutionForStreamedOutput();
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"read", in_tif, "!", "write", out_tif, "--overwrite"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("--format stream"));
    CPLErrorReset();
  }
  {
    const std::string side_tif = std::string(kTestDir) + "/stream_side.tif";
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm alg;
    alg.SetExecutionForStreamedOutput();
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"read", in_tif, "!", "tee", "[", "write", side_tif, "--overwrite", "]",
         "!", "write", "streamed", "-f", "stream"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("not allowed in stream execution"));
    CPLErrorReset();
  }
}

TEST_F(GdalalgAbstractPipelineTest, TileMultiThreadingAndAutoComplete) {
  const std::string in_tif = std::string(kTestDir) + "/tile_in.tif";
  const std::string tile_dir = std::string(kTestDir) + "/tiles";
  const std::string gdalg_path = std::string(kTestDir) + "/ac.gdalg.json";
  const std::string in_geojson = std::string(kTestDir) + "/ac.geojson";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(in_tif));
  ASSERT_NO_FATAL_FAILURE(CreateTinyGeoJson(in_geojson));

  // Tile multi-threading with non-MEM file serializes prior steps to GDALG.
  {
    GDALRasterPipelineAlgorithm alg;
    alg.SetCallPath({"gdal", "raster", "pipeline"});
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"read", in_tif, "!", "scale", "--src-min", "0", "--src-max", "255",
         "!", "tile", tile_dir, "--num-threads", "2", "--parallel-method",
         "thread", "--min-zoom", "0", "--max-zoom", "0"}));
    EXPECT_TRUE(alg.Run());
  }

  // Tile multi-threading with MEM input dataset fails.
  {
    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_NE(nullptr, mem_drv);
    GDALDatasetUniquePtr mem_ds(
        mem_drv->Create("", 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, mem_ds);
    OGRSpatialReference srs;
    ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
    mem_ds->SetSpatialRef(&srs);
    double gt[6] = {0.0, 0.01, 0.0, 1.0, 0.0, -0.01};
    ASSERT_EQ(CE_None, mem_ds->SetGeoTransform(gt));

    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm alg;
    alg.SetInputDataset(mem_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"read", "!", "scale", "!", "tile", tile_dir, "--num-threads", "2",
         "--parallel-method", "thread"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("non-materialized dataset"));
    CPLErrorReset();
  }

  // GetAutoComplete on step names, after '!', in generic pipeline, and JSON.
  {
    GDALRasterPipelineAlgorithm alg;
    std::vector<std::string> args = {"r"};
    EXPECT_THAT(alg.GetAutoComplete(args, false, false), Contains("read"));
  }
  {
    GDALRasterPipelineAlgorithm alg;
    std::vector<std::string> args = {"read"};
    EXPECT_THAT(alg.GetAutoComplete(args, false, false), IsEmpty());
  }
  {
    GDALRasterPipelineAlgorithm alg;
    std::vector<std::string> args = {"read", in_tif, "!"};
    EXPECT_THAT(alg.GetAutoComplete(args, false, false), Contains("scale"));
  }
  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("pipeline");
    ASSERT_NE(nullptr, alg);
    std::vector<std::string> args = {"read", in_geojson, "!", "buffer", "--"};
    EXPECT_THAT(alg->GetAutoComplete(args, false, false), Not(IsEmpty()));
  }
  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("pipeline");
    ASSERT_NE(nullptr, alg);
    std::vector<std::string> args = {"read", in_tif,  "!", "slope",
                                     "!",    "scale", "--"};
    EXPECT_THAT(alg->GetAutoComplete(args, false, false), Not(IsEmpty()));
  }
  {
    GDALRasterPipelineAlgorithm alg;
    alg.SetCallPath({"gdal", "raster", "pipeline"});
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"read", in_tif, "!", "scale", "!",
                                       "write", gdalg_path, "--overwrite"}));
    ASSERT_TRUE(alg.Run());

    GDALRasterPipelineAlgorithm ac1;
    std::vector<std::string> args1 = {"--pipeline=" + gdalg_path, "--"};
    EXPECT_THAT(ac1.GetAutoComplete(args1, false, false),
                Contains("--scale.src-min="));

    GDALRasterPipelineAlgorithm ac2;
    std::vector<std::string> args2 = {"--pipeline=" + gdalg_path,
                                      "--output-format="};
    EXPECT_THAT(ac2.GetAutoComplete(args2, false, false), Contains("GTiff"));
  }
  {
    const std::string ac2_gdalg = std::string(kTestDir) + "/ac2.gdalg.json";
    GDALRasterPipelineAlgorithm alg;
    alg.SetCallPath({"gdal", "raster", "pipeline"});
    ASSERT_TRUE(alg.ParseCommandLineArguments({"read", in_tif, "!", "scale",
                                               "!", "scale", "!", "write",
                                               ac2_gdalg, "--overwrite"}));
    ASSERT_TRUE(alg.Run());

    GDALRasterPipelineAlgorithm ac1;
    std::vector<std::string> args1 = {"--pipeline=" + ac2_gdalg, "--"};
    EXPECT_THAT(ac1.GetAutoComplete(args1, false, false),
                Contains("--scale[0].src-min="));

    GDALRasterPipelineAlgorithm ac2;
    std::vector<std::string> args2 = {"--pipeline=" + ac2_gdalg,
                                      "--scale[0].output-data-type="};
    EXPECT_THAT(ac2.GetAutoComplete(args2, false, false), Contains("Byte"));

    GDALRasterPipelineAlgorithm ac3;
    std::vector<std::string> args3 = {"--pipeline=" + ac2_gdalg,
                                      "--input-format="};
    EXPECT_THAT(ac3.GetAutoComplete(args3, false, false), Contains("GTiff"));
  }
  {
    GDALRasterPipelineAlgorithm alg;
    std::vector<std::string> args = {"mosaic", "[", "read", in_tif};
    (void)alg.GetAutoComplete(args, false, false);
  }
  {
    auto alg = MakeGenericPipeline();
    ASSERT_NE(nullptr, alg);
    std::vector<std::string> args = {"read", in_tif, "!", "scale", "!", "w"};
    EXPECT_THAT(alg->GetAutoComplete(args, false, false), Contains("write"));
  }
  {
    auto alg = MakeGenericPipeline();
    ASSERT_NE(nullptr, alg);
    std::vector<std::string> args = {"read", in_geojson, "!", "buffer",
                                     "1.0",  "!",        "w"};
    EXPECT_THAT(alg->GetAutoComplete(args, false, false), Contains("write"));
  }
  {
    auto alg = MakeGenericPipeline();
    ASSERT_NE(nullptr, alg);
    std::vector<std::string> args = {"read", in_tif, "!", "sc"};
    EXPECT_THAT(alg->GetAutoComplete(args, false, false), Contains("scale"));
  }
  {
    auto alg = MakeGenericPipeline();
    ASSERT_NE(nullptr, alg);
    std::vector<std::string> args = {"read", in_tif,  "!", "sieve",
                                     "!",    "scale", "--"};
    EXPECT_THAT(alg->GetAutoComplete(args, false, false), Not(IsEmpty()));
  }
  {
    auto alg = MakeGenericPipeline();
    ASSERT_NE(nullptr, alg);
    std::vector<std::string> args = {"read", "/vsimem/no_such_ac_xyz.foo", "!",
                                     "clip", "--"};
    EXPECT_THAT(alg->GetAutoComplete(args, false, false), Not(IsEmpty()));
  }
}

}  // namespace
}  // namespace autotest2
