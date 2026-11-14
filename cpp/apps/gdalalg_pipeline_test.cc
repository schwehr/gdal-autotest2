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
// Tests for GDALPipelineAlgorithm and GDALPipelineStepAlgorithm
// (apps/gdalalg_pipeline.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_pipeline.cpp
//   https://gdal.org/programs/gdal_pipeline.html

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/gdalalg_raster_compare.h"
#include "third_party/gdal/apps/gdalalg_raster_convert.h"
#include "third_party/gdal/apps/gdalalg_raster_info.h"
#include "third_party/gdal/apps/gdalalg_raster_set_type.h"
#include "third_party/gdal/apps/gdalalg_raster_sieve.h"
#include "third_party/gdal/apps/gdalalg_raster_slope.h"
#include "third_party/gdal/apps/gdalalg_vector_buffer.h"
#include "third_party/gdal/apps/gdalalg_vector_convert.h"
#include "third_party/gdal/apps/gdalalg_vector_info.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
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

constexpr char kTestDir[] = "/vsimem/gdalalg_pipeline_test";

int RecordingProgress(double /*complete*/, const char* /*message*/,
                      void* data) {
  int* count = static_cast<int*>(data);
  if (count != nullptr) {
    ++(*count);
  }
  return 1;
}

void CreateTinyTiff(const std::string& path, uint8_t fill = 42) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  ds->SetSpatialRef(&srs);
  double gt[6] = {0.0, 0.01, 0.0, 1.0, 0.0, -0.01};
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));
  std::vector<uint8_t> pixels(16, fill);
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 4, pixels.data(),
                                           4, 4, GDT_Byte, 0, 0, nullptr));
}

void WriteVsimemFile(const std::string& path, const std::string& content) {
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
  ASSERT_EQ(content.size(), VSIFWriteL(content.data(), 1, content.size(), fp));
}

void CreateTinyGeoJson(const std::string& path) {
  ASSERT_NO_FATAL_FAILURE(WriteVsimemFile(path,
                                          R"json({
  "type": "FeatureCollection",
  "features": [
    {
      "type": "Feature",
      "geometry": { "type": "Point", "coordinates": [1.0, 2.0] },
      "properties": { "name": "pt1" }
    }
  ]
})json"));
}

class GdalalgPipelineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GDALG();
    GDALRegister_GTiff();
    GDALRegister_MEM();
    GDALRegister_VRT();
    RegisterOGRGeoJSON();
    RegisterOGRGeoPackage();
    VSIRmdirRecursive(kTestDir);
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
    CPLSetConfigOption("CPL_TMPDIR", kTestDir);
  }

  void TearDown() override {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(kTestDir);
  }
};

TEST_F(GdalalgPipelineTest, BasicPropertiesAliasesUsageAndAutoComplete) {
  auto alg =
      GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("pipeline");
  ASSERT_NE(nullptr, alg);
  EXPECT_EQ("pipeline", alg->GetName());
  EXPECT_EQ("Process a dataset applying several steps.", alg->GetDescription());
  EXPECT_EQ("/programs/gdal_pipeline.html", alg->GetHelpURL());

  auto proj_alias =
      GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("+pipeline");
  ASSERT_NE(nullptr, proj_alias);
  EXPECT_EQ("pipeline", proj_alias->GetName());

  const std::string short_usage = alg->GetUsageForCLI(/*shortUsage=*/true, {});
  EXPECT_THAT(short_usage, HasSubstr("Usage:"));

  const std::string full_usage = alg->GetUsageForCLI(/*shortUsage=*/false, {});
  EXPECT_THAT(full_usage, HasSubstr("Potential steps are:"));
  EXPECT_THAT(full_usage, HasSubstr("read"));
  EXPECT_THAT(full_usage, HasSubstr("write"));

  const std::string json_usage = alg->GetUsageAsJSON();
  EXPECT_THAT(json_usage, HasSubstr("pipeline_algorithms"));

  std::vector<std::string> ac_first;
  EXPECT_THAT(alg->GetAutoComplete(ac_first, /*lastWordIsComplete=*/true,
                                   /*showAllOptions=*/false),
              Contains("read"));

  const std::string in_tif = std::string(kTestDir) + "/ac.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(in_tif));
  std::vector<std::string> ac_step = {"read", in_tif, "!", ""};
  EXPECT_THAT(alg->GetAutoComplete(ac_step, /*lastWordIsComplete=*/true,
                                   /*showAllOptions=*/false),
              Contains("write"));
}

TEST_F(GdalalgPipelineTest, HelpDocCategories) {
  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("pipeline");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments({"--help-doc=main"}));
    const std::string usage = alg->GetUsageForCLI(/*shortUsage=*/false, {});
    EXPECT_THAT(usage, HasSubstr("<PIPELINE>"));
    EXPECT_EQ(std::string::npos, usage.find("Potential steps are:"));
  }

  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("pipeline");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments({"--help-doc=set-type"}));
    const std::string usage = alg->GetUsageForCLI(/*shortUsage=*/false, {});
    EXPECT_THAT(usage, HasSubstr("set-type"));
  }

  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("pipeline");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(
        alg->ParseCommandLineArguments({"--help-doc=nonexistent_step_xyz"}));
    const std::string usage = alg->GetUsageForCLI(/*shortUsage=*/false, {});
    EXPECT_THAT(usage, HasSubstr("ERROR: unknown pipeline step"));
  }
}

TEST_F(GdalalgPipelineTest, DirectRunAndErrorBranches) {
  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("pipeline");
    ASSERT_NE(nullptr, alg);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg->Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("'pipeline' argument not set"));
    CPLErrorReset();
  }

  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("pipeline");
    ASSERT_NE(nullptr, alg);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg->ParseCommandLineArguments({"nonexistent_step_xyz"}));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST_F(GdalalgPipelineTest, ExecuteRasterVectorMixedAndNestedPipelines) {
  const std::string in_tif = std::string(kTestDir) + "/in.tif";
  const std::string out_tif = std::string(kTestDir) + "/out.tif";
  const std::string side_tif = std::string(kTestDir) + "/side.tif";
  const std::string in_geojson = std::string(kTestDir) + "/in.geojson";
  const std::string out_gpkg = std::string(kTestDir) + "/out.gpkg";
  const std::string out_poly = std::string(kTestDir) + "/poly.geojson";
  const std::string out_rast = std::string(kTestDir) + "/rast.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(in_tif));
  ASSERT_NO_FATAL_FAILURE(CreateTinyGeoJson(in_geojson));

  // 1. Raster pipeline
  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("pipeline");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments(
        {"read", in_tif, "!", "set-type", "--datatype", "UInt16", "!", "write",
         out_tif, "--overwrite"}));
    ASSERT_TRUE(alg->Run());
    ASSERT_TRUE(alg->Finalize());

    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(GDT_UInt16, out_ds->GetRasterBand(1)->GetRasterDataType());
  }

  // 2. Vector pipeline
  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("pipeline");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments({"read", in_geojson, "!",
                                                "buffer", "0.5", "!", "write",
                                                out_gpkg, "--overwrite"}));
    ASSERT_TRUE(alg->Run());
    ASSERT_TRUE(alg->Finalize());

    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, out_ds);
    ASSERT_EQ(1, out_ds->GetLayerCount());
    EXPECT_EQ(1, out_ds->GetLayer(0)->GetFeatureCount());
  }

  // 3. Mixed raster -> vector pipeline
  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("pipeline");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(
        alg->ParseCommandLineArguments({"read", in_tif, "!", "polygonize", "!",
                                        "write", out_poly, "--overwrite"}));
    ASSERT_TRUE(alg->Run());
    ASSERT_TRUE(alg->Finalize());

    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_poly.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, out_ds);
    ASSERT_EQ(1, out_ds->GetLayerCount());
  }

  // 4. Mixed vector -> raster pipeline
  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("pipeline");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments(
        {"read", in_geojson, "!", "rasterize", "--extent", "0,0,4,4", "--size",
         "4,4", "!", "write", out_rast, "--overwrite"}));
    ASSERT_TRUE(alg->Run());
    ASSERT_TRUE(alg->Finalize());

    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_rast.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(4, out_ds->GetRasterXSize());
  }

  // 5. Nested pipeline with tee
  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("pipeline");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments(
        {"read", in_tif, "!", "tee", "[", "write", side_tif, "--overwrite", "]",
         "!", "info"}));
    alg->SetCalledFromCommandLine();
    ASSERT_TRUE(alg->Run());
    ASSERT_TRUE(alg->Finalize());
    const auto* out_arg = alg->GetArg(GDAL_ARG_NAME_OUTPUT_STRING);
    ASSERT_NE(nullptr, out_arg);
    EXPECT_THAT(out_arg->Get<std::string>(), HasSubstr(in_tif));
    VSIStatBufL sStat;
    EXPECT_EQ(0, VSIStatL(side_tif.c_str(), &sStat));
  }
}

TEST_F(GdalalgPipelineTest, StandaloneStepAlgorithmPaths) {
  const std::string in_tif = std::string(kTestDir) + "/step_in.tif";
  const std::string ref_tif = std::string(kTestDir) + "/step_ref.tif";
  const std::string out_tif = std::string(kTestDir) + "/step_out.tif";
  const std::string out_vrt = std::string(kTestDir) + "/step_out.vrt";
  const std::string out_gdalg = std::string(kTestDir) + "/step_out.gdalg.json";
  const std::string in_geojson = std::string(kTestDir) + "/step_in.geojson";
  const std::string out_gpkg = std::string(kTestDir) + "/step_out.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(in_tif, 42));
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(ref_tif, 99));
  ASSERT_NO_FATAL_FAILURE(CreateTinyGeoJson(in_geojson));

  // 1. Standalone step where CanHandleNextStep(writeAlg) is true + progress.
  {
    GDALRasterConvertAlgorithm alg(/*standalone=*/true);
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({in_tif, out_tif, "--overwrite"}));
    int progress_calls = 0;
    ASSERT_TRUE(alg.Run(RecordingProgress, &progress_calls));
    EXPECT_GT(progress_calls, 0);
    ASSERT_TRUE(alg.Finalize());
  }

  // 2. Standalone step where writeAlg executes separately + progress.
  {
    GDALRasterSetTypeAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--datatype", "UInt16", in_tif, out_tif, "--overwrite"}));
    int progress_calls = 0;
    ASSERT_TRUE(alg.Run(RecordingProgress, &progress_calls));
    EXPECT_GT(progress_calls, 0);
    ASSERT_TRUE(alg.Finalize());
  }

  // 3. Standalone non-natively-streaming step + progress.
  {
    GDALRasterSieveAlgorithmStandalone alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({in_tif, out_tif, "--overwrite"}));
    int progress_calls = 0;
    ASSERT_TRUE(alg.Run(RecordingProgress, &progress_calls));
    EXPECT_GT(progress_calls, 0);
    ASSERT_TRUE(alg.Finalize());
  }

  // 4. Standalone compare step + progress.
  {
    GDALRasterCompareAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({ref_tif, in_tif}));
    int progress_calls = 0;
    ASSERT_TRUE(alg.Run(RecordingProgress, &progress_calls));
    EXPECT_FALSE(alg.GetOutputString().empty());
    ASSERT_TRUE(alg.Finalize());
  }

  // 5. Standalone raster info and SetInputDataset.
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(in_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    GDALRasterInfoAlgorithmStandalone alg;
    alg.SetInputDataset(ds.get());
    ASSERT_TRUE(alg.Run());
    EXPECT_THAT(alg.GetOutputString(),
                HasSubstr("\"driverShortName\":\"GTiff\""));
    ASSERT_TRUE(alg.Finalize());
  }

  // 6. Standalone vector info.
  {
    GDALVectorInfoAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({in_geojson}));
    ASSERT_TRUE(alg.Run());
    EXPECT_THAT(alg.GetOutputString(), HasSubstr("GeoJSON"));
    ASSERT_TRUE(alg.Finalize());
  }

  // 7. VRT output disallowed when !m_outputVRTCompatible.
  {
    GDALRasterSlopeAlgorithmStandalone alg;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.ParseCommandLineArguments({in_tif, out_vrt}));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("VRT output is not supported"));
    CPLErrorReset();
  }

  // 8. Standalone GDALG output and stream output.
  {
    GDALRasterConvertAlgorithm alg(/*standalone=*/true);
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({in_tif, out_gdalg, "--overwrite"}));
    ASSERT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());
    VSIStatBufL sStat;
    EXPECT_EQ(0, VSIStatL(out_gdalg.c_str(), &sStat));
  }

  {
    GDALRasterConvertAlgorithm alg(/*standalone=*/true);
    ASSERT_TRUE(alg.ParseCommandLineArguments({"-f", "stream", in_tif, ""}));
    alg.SetExecutionForStreamedOutput();
    ASSERT_TRUE(alg.Run());
    EXPECT_NE(nullptr, alg.GetOutputDataset().GetDatasetRef());
    ASSERT_TRUE(alg.Finalize());
  }

  // 9. Vector standalone step with --upsert (updates existing GPKG).
  {
    GDALVectorConvertAlgorithm alg_create;
    ASSERT_TRUE(alg_create.ParseCommandLineArguments(
        {in_geojson, out_gpkg, "--overwrite"}));
    ASSERT_TRUE(alg_create.Run());
    ASSERT_TRUE(alg_create.Finalize());

    GDALVectorConvertAlgorithm alg_upsert;
    ASSERT_TRUE(alg_upsert.ParseCommandLineArguments(
        {"--upsert", in_geojson, out_gpkg}));
    ASSERT_TRUE(alg_upsert.Run());
    ASSERT_TRUE(alg_upsert.Finalize());
  }
}

}  // namespace
}  // namespace autotest2
