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
// Tests for GDALVectorPipelineAlgorithm and vector pipeline dataset/layer
// helpers (apps/gdalalg_vector_pipeline.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_pipeline.cpp

#include "third_party/gdal/apps/gdalalg_vector_pipeline.h"

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::Contains;
using ::testing::HasSubstr;
using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_pipeline_test";

constexpr char kPointsGeoJson[] = R"json({
  "type": "FeatureCollection",
  "features": [
    {
      "type": "Feature",
      "geometry": { "type": "Point", "coordinates": [1.0, 2.0] },
      "properties": { "id": 1 }
    },
    {
      "type": "Feature",
      "geometry": { "type": "Point", "coordinates": [3.0, 4.0] },
      "properties": { "id": 2 }
    }
  ]
})json";

// Test subclass exercising GDALVectorPipelineOutputLayer (skipping, splitting,
// and FailTranslation()).
class TestTranslateLayer : public GDALVectorPipelineOutputLayer {
 public:
  explicit TestTranslateLayer(OGRLayer& src, bool fail_on_second = false)
      : GDALVectorPipelineOutputLayer(src), fail_on_second_(fail_on_second) {}

  const OGRFeatureDefn* GetLayerDefn() const override {
    return m_srcLayer.GetLayerDefn();
  }

  int TestCapability(const char* pszCap) const override {
    return m_srcLayer.TestCapability(pszCap);
  }

  void TranslateFeature(
      std::unique_ptr<OGRFeature> src_feat,
      std::vector<std::unique_ptr<OGRFeature>>& out_feats) override {
    const int id = src_feat->GetFieldAsInteger("id");
    if (fail_on_second_ && id == 2) {
      FailTranslation();
      return;
    }
    if (id == 1) {
      // Emit two copies for id == 1 to exercise pending-feature buffering.
      out_feats.emplace_back(src_feat->Clone());
      out_feats.push_back(std::move(src_feat));
    }
    // Skip id != 1 (when not failing) to exercise loop continuation.
  }

 private:
  bool fail_on_second_;
};

// Test subclass exercising GDALVectorNonStreamingAlgorithmDataset.
class TestNonStreamingDataset final
    : public GDALVectorNonStreamingAlgorithmDataset {
 public:
  bool Process(OGRLayer& src_layer, OGRLayer& dst_layer) override {
    src_layer.ResetReading();
    while (auto feat =
               std::unique_ptr<OGRFeature>(src_layer.GetNextFeature())) {
      if (dst_layer.CreateFeature(feat.get()) != OGRERR_NONE) {
        return false;
      }
    }
    return true;
  }
};

class GdalalgVectorPipelineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    RegisterOGRGeoJSON();
    RegisterOGRGeoPackage();
    VSIRmdirRecursive(kTestDir);
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgVectorPipelineTest, UsageHelpDocAndAutoComplete) {
  GDALVectorPipelineAlgorithm alg;
  EXPECT_EQ("pipeline", alg.GetName());
  EXPECT_EQ(GDAL_OF_VECTOR, alg.GetInputType());
  EXPECT_EQ(GDAL_OF_VECTOR, alg.GetOutputType());

  // 1. Short and full CLI usage + JSON usage.
  const GDALAlgorithm::UsageOptions usage_opts;
  EXPECT_THAT(alg.GetUsageForCLI(/*shortUsage=*/true, usage_opts),
              HasSubstr("Usage:"));
  const std::string full_usage =
      alg.GetUsageForCLI(/*shortUsage=*/false, usage_opts);
  EXPECT_THAT(full_usage, HasSubstr("Potential steps are:"));
  EXPECT_THAT(full_usage, HasSubstr("filter"));
  EXPECT_THAT(alg.GetUsageAsJSON(), HasSubstr("pipeline_algorithms"));

  // 2. --help-doc=main, valid step, and unknown step.
  {
    GDALVectorPipelineAlgorithm help_main;
    ASSERT_TRUE(help_main.ParseCommandLineArguments({"--help-doc=main"}));
    EXPECT_THAT(help_main.GetUsageForCLI(false, usage_opts),
                HasSubstr("<PIPELINE>"));
  }
  {
    GDALVectorPipelineAlgorithm help_step;
    ASSERT_TRUE(help_step.ParseCommandLineArguments({"--help-doc=filter"}));
    EXPECT_THAT(help_step.GetUsageForCLI(false, usage_opts),
                HasSubstr("filter"));
  }
  {
    GDALVectorPipelineAlgorithm help_unknown;
    ASSERT_TRUE(
        help_unknown.ParseCommandLineArguments({"--help-doc=no_such_step"}));
    EXPECT_THAT(help_unknown.GetUsageForCLI(false, usage_opts),
                HasSubstr("unknown pipeline step"));
  }

  // 3. Autocomplete on step arguments.
  std::vector<std::string> ac_args = {"read", "in.geojson", "!", "filter", "-"};
  const auto suggestions = alg.GetAutoComplete(ac_args, false, false);
  EXPECT_THAT(suggestions, Contains("--where"));
}

TEST_F(GdalalgVectorPipelineTest, RegisterAlgorithmsForMixedPipeline) {
  GDALVectorAlgorithmStepRegistry mixed_registry;
  GDALVectorPipelineAlgorithm::RegisterAlgorithms(mixed_registry,
                                                  /*forMixedPipeline=*/true);
  EXPECT_THAT(mixed_registry.GetNames(), Contains("read-vector"));
  EXPECT_THAT(mixed_registry.GetNames(), Contains("write-vector"));
  EXPECT_THAT(mixed_registry.GetNames(), Contains("filter"));
}

TEST_F(GdalalgVectorPipelineTest, RunPipelineAndNestedPipeline) {
  const std::string in_json = std::string(kTestDir) + "/in.geojson";
  const std::string side_gpkg = std::string(kTestDir) + "/side.gpkg";
  const std::string out_gpkg = std::string(kTestDir) + "/out.gpkg";
  VsiMemTempWrapper in_file(in_json, kPointsGeoJson);

  // 1. Multi-step pipeline with tee (nested pipeline), filter, reproject, and
  //    write. (tee runs before filter because GDALTeeStepAlgorithmBase's
  //    filter restoration clears non-empty attribute filters.)
  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments({
        "read",        in_json,     "!",           "tee",   "[",
        "write",       side_gpkg,   "--overwrite", "]",     "!",
        "filter",      "--where",   "id = 2",      "!",     "reproject",
        "--dst-crs",   "EPSG:3857", "!",           "write", out_gpkg,
        "--overwrite",
    }));
    EXPECT_TRUE(pipeline.Run());
    EXPECT_TRUE(pipeline.Finalize());

    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(1, out_ds->GetLayer(0)->GetFeatureCount());

    std::unique_ptr<GDALDataset> side_ds(GDALDataset::Open(
        side_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_THAT(side_ds, NotNull());
    EXPECT_EQ(2, side_ds->GetLayer(0)->GetFeatureCount());
  }

  // 2. Pipeline ending in info produces output string.
  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", in_json, "!", "info", "--output-format", "json"}));
    EXPECT_TRUE(pipeline.Run());
    const std::string output = pipeline.GetOutputString();
    EXPECT_THAT(output, HasSubstr("\"driverShortName\":\"GeoJSON\""));
    EXPECT_THAT(output, HasSubstr("\"featureCount\":2"));
  }
}

TEST_F(GdalalgVectorPipelineTest, OutputDatasetOutputLayerAndPassthrough) {
  const std::string in_json = std::string(kTestDir) + "/layer_src.geojson";
  VsiMemTempWrapper in_file(in_json, kPointsGeoJson);
  std::unique_ptr<GDALDataset> src_ds(
      GDALDataset::Open(in_json.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_THAT(src_ds, NotNull());
  OGRLayer* src_lyr = src_ds->GetLayer(0);
  ASSERT_THAT(src_lyr, NotNull());

  // 1. GDALVectorOutputDataset + GDALVectorPipelinePassthroughLayer.
  {
    GDALVectorOutputDataset out_ds;
    EXPECT_EQ(0, out_ds.TestCapability(ODsCCreateLayer));
    out_ds.AddLayer(
        std::make_unique<GDALVectorPipelinePassthroughLayer>(*src_lyr));
    EXPECT_EQ(1, out_ds.GetLayerCount());
    const OGRLayer* lyr = out_ds.GetLayer(0);
    ASSERT_THAT(lyr, NotNull());
    EXPECT_EQ(src_lyr->GetLayerDefn(), lyr->GetLayerDefn());
    EXPECT_EQ(src_lyr->TestCapability(OLCStringsAsUTF8),
              lyr->TestCapability(OLCStringsAsUTF8));

    auto* mut_lyr = const_cast<OGRLayer*>(lyr);
    int count = 0;
    while (auto f = std::unique_ptr<OGRFeature>(mut_lyr->GetNextFeature())) {
      ++count;
    }
    EXPECT_EQ(2, count);
    mut_lyr->ResetReading();
    auto first = std::unique_ptr<OGRFeature>(mut_lyr->GetNextFeature());
    ASSERT_THAT(first, NotNull());
  }

  // 2. GDALVectorPipelineOutputDataset + TestTranslateLayer.
  {
    GDALVectorPipelineOutputDataset pipe_ds(*src_ds);
    pipe_ds.AddLayer(*src_lyr, std::make_unique<TestTranslateLayer>(*src_lyr));
    EXPECT_EQ(1, pipe_ds.GetLayerCount());
    EXPECT_EQ(nullptr, pipe_ds.GetLayer(-1));
    EXPECT_EQ(nullptr, pipe_ds.GetLayer(1));
    ASSERT_THAT(pipe_ds.GetLayer(0), NotNull());
    EXPECT_EQ(src_ds->TestCapability(ODsCRandomLayerRead),
              pipe_ds.TestCapability(ODsCRandomLayerRead));
    EXPECT_FALSE(pipe_ds.TestCapability(ODsCCreateLayer));

    OGRLayer* belonging = nullptr;
    double pct = 0.0;
    auto f1 = std::unique_ptr<OGRFeature>(
        pipe_ds.GetNextFeature(&belonging, &pct, nullptr, nullptr));
    ASSERT_THAT(f1, NotNull());
    EXPECT_EQ(pipe_ds.GetLayer(0), belonging);
    auto f2 = std::unique_ptr<OGRFeature>(
        pipe_ds.GetNextFeature(&belonging, &pct, nullptr, nullptr));
    ASSERT_THAT(f2, NotNull());
    // Feature id == 2 was skipped, so next is nullptr.
    EXPECT_EQ(nullptr,
              pipe_ds.GetNextFeature(&belonging, &pct, nullptr, nullptr));

    pipe_ds.ResetReading();
    auto f_again = std::unique_ptr<OGRFeature>(
        pipe_ds.GetNextFeature(&belonging, &pct, nullptr, nullptr));
    ASSERT_THAT(f_again, NotNull());
  }

  // 3. Test FailTranslation() on GDALVectorPipelineOutputLayer.
  {
    src_lyr->ResetReading();
    TestTranslateLayer fail_lyr(*src_lyr, /*fail_on_second=*/true);
    auto f1 = std::unique_ptr<OGRFeature>(fail_lyr.GetNextFeature());
    ASSERT_THAT(f1, NotNull());
    auto f2 = std::unique_ptr<OGRFeature>(fail_lyr.GetNextFeature());
    ASSERT_THAT(f2, NotNull());
    EXPECT_EQ(nullptr, fail_lyr.GetNextFeature());
  }
}

TEST_F(GdalalgVectorPipelineTest, NonStreamingAlgorithmDataset) {
  const std::string in_json = std::string(kTestDir) + "/non_stream.geojson";
  VsiMemTempWrapper in_file(in_json, kPointsGeoJson);
  std::unique_ptr<GDALDataset> src_ds(
      GDALDataset::Open(in_json.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_THAT(src_ds, NotNull());
  OGRLayer* src_lyr = src_ds->GetLayer(0);
  ASSERT_THAT(src_lyr, NotNull());

  TestNonStreamingDataset non_streaming;
  EXPECT_TRUE(non_streaming.AddProcessedLayer(*src_lyr));
  non_streaming.AddPassThroughLayer(*src_lyr);
  EXPECT_EQ(2, non_streaming.GetLayerCount());
  EXPECT_EQ(nullptr, non_streaming.GetLayer(-1));
  EXPECT_EQ(nullptr, non_streaming.GetLayer(2));

  OGRLayer* processed = non_streaming.GetLayer(0);
  ASSERT_THAT(processed, NotNull());
  EXPECT_EQ(2, processed->GetFeatureCount());
  EXPECT_THAT(non_streaming.GetLayer(1), NotNull());
  EXPECT_FALSE(non_streaming.TestCapability(ODsCCreateLayer));
  EXPECT_FALSE(non_streaming.TestCapability(ODsCDeleteLayer));
  EXPECT_FALSE(non_streaming.TestCapability("UNSUPPORTED_CAP"));
}

}  // namespace
}  // namespace autotest2
