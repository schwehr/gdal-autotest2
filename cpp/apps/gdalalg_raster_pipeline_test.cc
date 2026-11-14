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
// Tests for GDALRasterPipelineAlgorithm and base step algorithms
// (apps/gdalalg_raster_pipeline.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_pipeline.cpp

#include "third_party/gdal/apps/gdalalg_raster_pipeline.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_pipeline_test";

class TestStepAlgorithm final : public GDALRasterPipelineStepAlgorithm {
 public:
  TestStepAlgorithm()
      : GDALRasterPipelineStepAlgorithm("test-step", "Test step", "", false) {
    AddOutputFormatArg(&m_format);
  }

  void ExposeSetOutputVRTCompatible(bool b) { SetOutputVRTCompatible(b); }

 protected:
  bool RunStep(GDALPipelineStepRunContext&) override { return true; }
};

class TestNonStreamingAlgorithm final
    : public GDALRasterPipelineNonNativelyStreamingAlgorithm {
 public:
  TestNonStreamingAlgorithm()
      : GDALRasterPipelineNonNativelyStreamingAlgorithm(
            "test-non-streaming", "Test non-streaming", "", false) {}

  bool ExposeIsNativelyStreamingCompatible() const {
    return IsNativelyStreamingCompatible();
  }

  std::unique_ptr<GDALDataset> ExposeCreateTemporaryDataset(
      int width, int height, int bands, GDALDataType dt, bool tiled,
      GDALDataset* src_for_meta, bool copy_meta) {
    return CreateTemporaryDataset(width, height, bands, dt, tiled, src_for_meta,
                                  copy_meta);
  }

  std::unique_ptr<GDALDataset> ExposeCreateTemporaryCopy(
      GDALDataset* src, int single_band, bool tiled, GDALProgressFunc progress,
      void* progress_data) {
    return CreateTemporaryCopy(this, src, single_band, tiled, progress,
                               progress_data);
  }

 protected:
  bool RunStep(GDALPipelineStepRunContext&) override { return true; }
};

class GdalalgRasterPipelineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    GDALRegister_MEM();
    GDALRegister_VRT();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
    CPLSetConfigOption("CPL_TMPDIR", kTestDir);
  }

  void TearDown() override {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    CPLSetConfigOption("GDAL_RASTER_PIPELINE_USE_GTIFF_FOR_TEMP_DATASET",
                       nullptr);
    VSIRmdirRecursive(kTestDir);
  }
};

void CreateTinyTiff(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  ASSERT_EQ(CE_None, ds->SetSpatialRef(&srs));
  GDALGeoTransform gt{0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));
  std::array<uint8_t, 16> data{};
  data.fill(7);
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 4, data.data(), 4,
                                           4, GDT_Byte, 0, 0, nullptr));
}

TEST_F(GdalalgRasterPipelineTest, PipelinePropertiesAndMixedRegistration) {
  GDALRasterPipelineAlgorithm alg;
  EXPECT_EQ("pipeline", alg.GetName());
  EXPECT_EQ(GDAL_OF_RASTER, alg.GetInputType());
  EXPECT_EQ(GDAL_OF_RASTER, alg.GetOutputType());

  GDALRasterPipelineAlgorithm mixed_alg(/*openForMixedRasterVector=*/true);
  EXPECT_EQ(GDAL_OF_RASTER, mixed_alg.GetInputType());

  GDALRasterAlgorithmStepRegistry reg;
  GDALRasterPipelineAlgorithm::RegisterAlgorithms(reg,
                                                  /*forMixedPipeline=*/true);
  EXPECT_NE(nullptr, reg.Instantiate("read-raster"));
  EXPECT_NE(nullptr, reg.Instantiate("write-raster"));
}

TEST_F(GdalalgRasterPipelineTest, GetUsageForCliVariants) {
  GDALAlgorithm::UsageOptions opts;
  opts.isPipelineMain = false;

  // 1. Short usage.
  {
    GDALRasterPipelineAlgorithm alg;
    const std::string short_usage = alg.GetUsageForCLI(true, opts);
    EXPECT_THAT(short_usage, HasSubstr("Usage:"));
    EXPECT_EQ(std::string::npos, short_usage.find("Potential steps are:"));
  }

  // 2. Full usage listing all steps.
  {
    GDALRasterPipelineAlgorithm alg;
    const std::string full_usage = alg.GetUsageForCLI(false, opts);
    EXPECT_THAT(full_usage, HasSubstr("Potential steps are:"));
    EXPECT_THAT(full_usage, HasSubstr("read"));
    EXPECT_THAT(full_usage, HasSubstr("write"));
  }

  // 3. --help-doc=main.
  {
    GDALRasterPipelineAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--help-doc=main"}));
    const std::string main_doc = alg.GetUsageForCLI(false, opts);
    EXPECT_THAT(main_doc, HasSubstr("<PIPELINE> is of the form:"));
    EXPECT_EQ(std::string::npos, main_doc.find("Potential steps are:"));
  }

  // 4. --help-doc=clip (valid step).
  {
    GDALRasterPipelineAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--help-doc=clip"}));
    const std::string clip_doc = alg.GetUsageForCLI(false, opts);
    EXPECT_THAT(clip_doc, HasSubstr("clip"));
  }

  // 5. --help-doc=nonexistent (unknown step).
  {
    GDALRasterPipelineAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--help-doc=nonexistent_step"}));
    const std::string err_doc = alg.GetUsageForCLI(false, opts);
    EXPECT_THAT(err_doc, HasSubstr("unknown pipeline step"));
  }
}

TEST_F(GdalalgRasterPipelineTest, EndToEndAndNestedPipeline) {
  const std::string in_tif = std::string(kTestDir) + "/in.tif";
  const std::string tee_tif = std::string(kTestDir) + "/tee.tif";
  const std::string out_tif = std::string(kTestDir) + "/out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(in_tif));

  GDALRasterPipelineAlgorithm pipeline;
  ASSERT_TRUE(pipeline.ParseCommandLineArguments(
      {"read",     in_tif,       "!",          "tee",         "[",     "write",
       tee_tif,    "-f",         "GTiff",      "--overwrite", "]",     "!",
       "set-type", "--datatype", "UInt16",     "!",           "write", out_tif,
       "-f",       "GTiff",      "--overwrite"}));
  EXPECT_TRUE(pipeline.Run());
  ASSERT_TRUE(pipeline.Finalize());

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, out_ds);
  EXPECT_EQ(GDT_UInt16, out_ds->GetRasterBand(1)->GetRasterDataType());
}

TEST_F(GdalalgRasterPipelineTest, StepSetOutputVrtCompatible) {
  TestStepAlgorithm step;
  step.ExposeSetOutputVRTCompatible(true);
  step.ExposeSetOutputVRTCompatible(false);
  auto* fmt_arg = step.GetArg(GDAL_ARG_NAME_OUTPUT_FORMAT);
  ASSERT_NE(nullptr, fmt_arg);
}

TEST_F(GdalalgRasterPipelineTest, NonNativelyStreamingTemporaryDatasetAndCopy) {
  TestNonStreamingAlgorithm alg;
  EXPECT_FALSE(alg.ExposeIsNativelyStreamingCompatible());

  // Build source MEM dataset with SRS, GeoTransform, GCPs, and metadata.
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  std::unique_ptr<GDALDataset> src(
      mem_drv->Create("src_mem", 4, 4, 2, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src);
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  src->SetSpatialRef(&srs);
  GDALGeoTransform gt{10.0, 1.0, 0.0, 20.0, 0.0, -1.0};
  src->SetGeoTransform(gt);
  src->SetMetadataItem("TEST_KEY", "TEST_VAL");

  GDAL_GCP gcp;
  GDALInitGCPs(1, &gcp);
  absl::Cleanup free_gcp = [&gcp] { GDALDeinitGCPs(1, &gcp); };
  gcp.dfGCPPixel = 0.0;
  gcp.dfGCPLine = 0.0;
  gcp.dfGCPX = 10.0;
  gcp.dfGCPY = 20.0;
  ASSERT_EQ(CE_None, src->SetGCPs(1, &gcp, &srs));

  // 1. MEM temporary dataset & copy (default when RAM is sufficient).
  {
    const CPLConfigOptionSetter use_gtiff(
        "GDAL_RASTER_PIPELINE_USE_GTIFF_FOR_TEMP_DATASET", "NO", false);
    auto tmp_ds = alg.ExposeCreateTemporaryDataset(
        4, 4, 1, GDT_Byte, /*tiled=*/true, src.get(), /*copy_meta=*/true);
    ASSERT_NE(nullptr, tmp_ds);
    EXPECT_STREQ("TEST_VAL", tmp_ds->GetMetadataItem("TEST_KEY"));
    EXPECT_EQ(1, tmp_ds->GetGCPCount());

    auto tmp_no_meta = alg.ExposeCreateTemporaryDataset(
        4, 4, 1, GDT_Byte, /*tiled=*/false, nullptr, /*copy_meta=*/false);
    ASSERT_NE(nullptr, tmp_no_meta);

    bool progress_called = false;
    auto progress_cb = [](double, const char*, void* data) -> int {
      *static_cast<bool*>(data) = true;
      return 1;
    };
    auto copy_all = alg.ExposeCreateTemporaryCopy(src.get(), /*single_band=*/0,
                                                  /*tiled=*/false, progress_cb,
                                                  &progress_called);
    ASSERT_NE(nullptr, copy_all);
    EXPECT_EQ(2, copy_all->GetRasterCount());
    EXPECT_TRUE(progress_called);

    auto copy_single = alg.ExposeCreateTemporaryCopy(
        src.get(), /*single_band=*/2, /*tiled=*/true, nullptr, nullptr);
    ASSERT_NE(nullptr, copy_single);
    EXPECT_EQ(1, copy_single->GetRasterCount());
  }

  // 2. GTiff on-disk temporary dataset & copy.
  {
    const CPLConfigOptionSetter use_gtiff(
        "GDAL_RASTER_PIPELINE_USE_GTIFF_FOR_TEMP_DATASET", "YES", false);
    auto tmp_gtiff = alg.ExposeCreateTemporaryDataset(
        4, 4, 1, GDT_Byte, /*tiled=*/true, src.get(), /*copy_meta=*/false);
    ASSERT_NE(nullptr, tmp_gtiff);
    EXPECT_EQ(nullptr, tmp_gtiff->GetMetadataItem("TEST_KEY"));

    auto copy_gtiff = alg.ExposeCreateTemporaryCopy(
        src.get(), /*single_band=*/1, /*tiled=*/true, nullptr, nullptr);
    ASSERT_NE(nullptr, copy_gtiff);
    EXPECT_EQ(1, copy_gtiff->GetRasterCount());
  }

  // 3. Invalid single_band causes CreateTemporaryCopy to report error.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto bad_copy = alg.ExposeCreateTemporaryCopy(
        src.get(), /*single_band=*/99, /*tiled=*/false, nullptr, nullptr);
    EXPECT_EQ(nullptr, bad_copy);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Failed to create temporary dataset"));
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
