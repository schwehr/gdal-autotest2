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
// Tests for GDALVectorOutputAbstractAlgorithm
// (apps/gdalalg_vector_output_abstract.cpp and
// apps/gdalalg_vector_output_abstract.h).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_output_abstract.cpp

#include "third_party/gdal/apps/gdalalg_vector_output_abstract.h"

#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_output_abstract_test";

// Minimal non-deletable layer & dataset to cover DeleteLayer failure.
class DummyLayer final : public OGRLayer {
 public:
  explicit DummyLayer(const char* name)
      : defn_(OGRFeatureDefn::CreateFeatureDefn(name)) {
    defn_->Reference();
    SetDescription(name);
  }
  ~DummyLayer() override { defn_->Release(); }

  const OGRFeatureDefn* GetLayerDefn() const override { return defn_; }
  void ResetReading() override {}
  OGRFeature* GetNextFeature() override { return nullptr; }
  int TestCapability(const char*) const override { return FALSE; }

 private:
  OGRFeatureDefn* defn_;
};

class NonDeletableLayerDataset final : public GDALDataset {
 public:
  explicit NonDeletableLayerDataset(const char* layer_name)
      : layer_(std::make_unique<DummyLayer>(layer_name)) {}

  int GetLayerCount() const override { return 1; }
  const OGRLayer* GetLayer(int idx) const override {
    return idx == 0 ? layer_.get() : nullptr;
  }

 private:
  std::unique_ptr<DummyLayer> layer_;
};

// Concrete test subclass exposing protected GDALVectorOutputAbstractAlgorithm
// helpers.
class TestVectorOutputAlgorithm final
    : public GDALVectorOutputAbstractAlgorithm {
 public:
  explicit TestVectorOutputAlgorithm(const std::string& default_layer_name = "")
      : GDALVectorOutputAbstractAlgorithm("test-vec-out", "Test", "") {
    m_outputLayerName = default_layer_name;
    AddAllOutputArgs();
  }

  void SetCreateDummyLayerIfMissing(bool v) { create_layer_if_missing_ = v; }
  const std::string& OutputLayerName() const { return m_outputLayerName; }

 protected:
  bool RunImpl(GDALProgressFunc, void*) override {
    SetupOutputDatasetRet ret = SetupOutputDataset();
    if (!ret.outDS) {
      return false;
    }
    if (!SetDefaultOutputLayerNameIfNeeded(ret.outDS)) {
      return false;
    }
    if (!ret.layer && create_layer_if_missing_) {
      ret.layer = ret.outDS->CreateLayer(m_outputLayerName.c_str(), nullptr,
                                         wkbPoint, nullptr);
      if (!ret.layer) {
        return false;
      }
    }
    if (ret.newDS) {
      m_outputDataset.Set(std::move(ret.newDS));
    }
    return true;
  }

 private:
  bool create_layer_if_missing_ = true;
};

TEST(GdalalgVectorOutputAbstractTest, CreateGpkgShpAndDefaultLayerName) {
  GDALRegister_MEM();
  RegisterOGRGeoPackage();
  RegisterOGRShape();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  // 1. Default output layer name initialized before AddAllOutputArgs().
  const std::string gpkg = std::string(kTestDir) + "/out.gpkg";
  {
    TestVectorOutputAlgorithm alg("preset_layer");
    ASSERT_TRUE(alg.ParseCommandLineArguments({gpkg}));
    EXPECT_TRUE(alg.Run());
    EXPECT_EQ("preset_layer", alg.OutputLayerName());
    EXPECT_TRUE(alg.Finalize());
  }

  // 2. Shapefile derives layer name from basename automatically.
  const std::string shp = std::string(kTestDir) + "/my_shp.shp";
  {
    TestVectorOutputAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({shp}));
    EXPECT_TRUE(alg.Run());
    EXPECT_EQ("my_shp", alg.OutputLayerName());
    EXPECT_TRUE(alg.Finalize());
  }

  // 3. Re-opening existing single-layer GPKG derives layer name for --append.
  {
    TestVectorOutputAlgorithm alg;
    alg.SetCreateDummyLayerIfMissing(false);
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--append", gpkg}));
    EXPECT_TRUE(alg.GetArg(GDAL_ARG_NAME_UPDATE)->Get<bool>());
    EXPECT_TRUE(alg.Run());
    EXPECT_EQ("preset_layer", alg.OutputLayerName());
    EXPECT_TRUE(alg.Finalize());
  }

  // 4. --overwrite-layer deletes and recreates existing layer.
  {
    TestVectorOutputAlgorithm alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--update", "--overwrite-layer",
                                       "--output-layer=preset_layer", gpkg}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
}

TEST(GdalalgVectorOutputAbstractTest, ExistingDatasetObjectAndErrorPaths) {
  GDALRegister_MEM();
  RegisterOGRGeoPackage();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  // 1. Output dataset passed as existing MEM dataset object.
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr mem_ds(
      mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, mem_ds);
  ASSERT_NE(nullptr,
            mem_ds->CreateLayer("existing", nullptr, wkbPoint, nullptr));

  // Existing layer without --overwrite-layer or --append fails.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    TestVectorOutputAlgorithm alg;
    alg.GetArg(GDAL_ARG_NAME_OUTPUT)->Set(mem_ds.get());
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("already exists"));
    CPLErrorReset();
  }

  // Missing layer with --append or --overwrite-layer fails.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    TestVectorOutputAlgorithm alg;
    alg.GetArg(GDAL_ARG_NAME_OUTPUT)->Set(mem_ds.get());
    alg.GetArg(GDAL_ARG_NAME_OUTPUT_LAYER)->Set("missing_layer");
    alg.GetArg(GDAL_ARG_NAME_APPEND)->Set(true);
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot find layer"));
    CPLErrorReset();
  }

  // MEM dataset without layer name fails SetDefaultOutputLayerNameIfNeeded().
  {
    GDALDatasetUniquePtr empty_mem(
        mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    CPLErrorReset();
    WithQuietHandler quiet;
    TestVectorOutputAlgorithm alg;
    alg.GetArg(GDAL_ARG_NAME_OUTPUT)->Set(empty_mem.get());
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Argument 'layer'"));
    CPLErrorReset();
  }

  // Non-deletable layer with --overwrite-layer fails DeleteLayer check.
  {
    NonDeletableLayerDataset non_del_ds("locked");
    CPLErrorReset();
    WithQuietHandler quiet;
    TestVectorOutputAlgorithm alg;
    alg.GetArg(GDAL_ARG_NAME_OUTPUT)->Set(&non_del_ds);
    alg.GetArg(GDAL_ARG_NAME_OVERWRITE_LAYER)->Set(true);
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot delete layer"));
    CPLErrorReset();
  }

  // Unknown extension cannot guess driver.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    TestVectorOutputAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {std::string(kTestDir) + "/out.unknown_ext"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot guess driver"));
    CPLErrorReset();
  }

  // Driver Create() failure on invalid path.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    TestVectorOutputAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "GPKG", "/nonexistent_parent_xyz/out.gpkg"}));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
