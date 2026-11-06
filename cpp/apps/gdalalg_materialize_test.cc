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
// Tests for materialize pipeline step (apps/gdalalg_materialize.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_materialize.cpp

#include "third_party/gdal/apps/gdalalg_materialize.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/apps/gdalalg_pipeline_step_stub.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::NotNull;

class GdalalgMaterializeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CPLSetConfigOption("CPL_TMPDIR", "/vsimem/gdalalg_materialize_tmp");
    GDALRegister_MEM();
    GDALRegister_GTiff();
    RegisterOGRSQLite();
    RegisterOGRGeoPackage();
  }

  void TearDown() override {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive("/vsimem/gdalalg_materialize_tmp");
  }
};

bool RunStep(GDALPipelineStepAlgorithm* alg,
             GDALProgressFunc pfnProgress = nullptr,
             void* progress_data = nullptr) {
  GDALPipelineStepRunContext ctxt;
  ctxt.m_pfnProgress = pfnProgress;
  ctxt.m_pProgressData = progress_data;
  return alg->RunStep(ctxt);
}

std::unique_ptr<GDALDataset> CreateRasterMemDataset(int width = 10,
                                                    int height = 10) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!drv) return nullptr;
  auto ds = std::unique_ptr<GDALDataset>(
      drv->Create("", width, height, 1, GDT_Byte, nullptr));
  if (ds && ds->GetRasterBand(1)) {
    std::vector<uint8_t> data(width * height, 42);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, width, height, data.data(), width,
                           height, GDT_Byte, 0, 0, nullptr));
  }
  return ds;
}

std::unique_ptr<GDALDataset> CreateVectorMemDataset(
    bool with_list_field = false, bool with_second_geom_field = false) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!drv) return nullptr;
  auto ds = std::unique_ptr<GDALDataset>(
      drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  if (!ds) return nullptr;
  auto* lyr = ds->CreateLayer("test_layer", nullptr, wkbPoint, nullptr);
  if (!lyr) return nullptr;

  OGRFieldDefn field_id("id", OFTInteger);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&field_id));

  if (with_list_field) {
    OGRFieldDefn field_list("tags", OFTStringList);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&field_list));
  }

  if (with_second_geom_field) {
    OGRGeomFieldDefn geom2("geom2", wkbPolygon);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateGeomField(&geom2));
  }

  auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
  feat->SetField("id", 1);
  if (with_list_field) {
    const char* const tags[] = {"a", "b", nullptr};
    feat->SetField("tags", tags);
  }
  EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  return ds;
}

TEST_F(GdalalgMaterializeTest, RasterStepProperties) {
  auto alg = std::make_unique<GDALMaterializeRasterAlgorithm>();
  ASSERT_THAT(alg, NotNull());
  EXPECT_EQ("materialize", alg->GetName());
  EXPECT_FALSE(alg->IsNativelyStreamingCompatible());
  EXPECT_FALSE(alg->CanBeMiddleStep());
  EXPECT_FALSE(alg->CanBeLastStep());
  EXPECT_FALSE(alg->GeneratesFilesFromUserInput());

  alg->ParseCommandLineArguments({"-o", "/vsimem/materialize_test/out.tif"});
  EXPECT_TRUE(alg->GeneratesFilesFromUserInput());
}

TEST_F(GdalalgMaterializeTest, RasterMaterializeMem) {
  auto src_ds = CreateRasterMemDataset();
  ASSERT_THAT(src_ds, NotNull());

  auto alg = std::make_unique<GDALMaterializeRasterAlgorithm>();
  alg->GetInputDatasets().emplace_back(src_ds.get());
  alg->SetFormat("MEM");

  EXPECT_TRUE(RunStep(alg.get()));
  auto* out_ds = alg->GetOutputDataset().GetDatasetRef();
  ASSERT_THAT(out_ds, NotNull());
  EXPECT_EQ(10, out_ds->GetRasterXSize());
  EXPECT_EQ(10, out_ds->GetRasterYSize());
  EXPECT_EQ(1, out_ds->GetRasterCount());
}

TEST_F(GdalalgMaterializeTest, RasterMaterializeGTiffAutoDelete) {
  auto src_ds = CreateRasterMemDataset();
  ASSERT_THAT(src_ds, NotNull());

  auto alg = std::make_unique<GDALMaterializeRasterAlgorithm>();
  alg->GetInputDatasets().emplace_back(src_ds.get());

  EXPECT_TRUE(RunStep(alg.get()));
  auto* out_ds = alg->GetOutputDataset().GetDatasetRef();
  ASSERT_THAT(out_ds, NotNull());
  EXPECT_EQ(10, out_ds->GetRasterXSize());
  EXPECT_EQ(10, out_ds->GetRasterYSize());
  EXPECT_EQ(1, out_ds->GetRasterCount());
}

TEST_F(GdalalgMaterializeTest, RasterMaterializeGTiffReopenRequired) {
  constexpr char kDir[] = "/vsimem/gdalalg_materialize_reopen";
  constexpr char kOutTif[] = "/vsimem/gdalalg_materialize_reopen/reopen.tif";
  absl::Cleanup clean_dir = [kDir] { VSIRmdirRecursive(kDir); };

  auto* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_THAT(drv, NotNull());
  drv->SetMetadataItem(GDAL_DCAP_REOPEN_AFTER_WRITE_REQUIRED, "YES");
  absl::Cleanup reset_meta = [drv] {
    drv->SetMetadataItem(GDAL_DCAP_REOPEN_AFTER_WRITE_REQUIRED, nullptr);
  };

  auto src_ds = CreateRasterMemDataset();
  ASSERT_THAT(src_ds, NotNull());

  auto alg = std::make_unique<GDALMaterializeRasterAlgorithm>();
  alg->GetInputDatasets().emplace_back(src_ds.get());
  alg->ParseCommandLineArguments({"-o", kOutTif});

  EXPECT_TRUE(RunStep(alg.get()));
  auto* out_ds = alg->GetOutputDataset().GetDatasetRef();
  ASSERT_THAT(out_ds, NotNull());
  EXPECT_EQ(10, out_ds->GetRasterXSize());
}

TEST_F(GdalalgMaterializeTest, RasterMaterializeGTiffExplicitOptions) {
  constexpr char kDir[] = "/vsimem/gdalalg_materialize_raster";
  constexpr char kOutTif[] = "/vsimem/gdalalg_materialize_raster/explicit.tif";
  absl::Cleanup clean_dir = [kDir] { VSIRmdirRecursive(kDir); };

  auto src_ds = CreateRasterMemDataset();
  ASSERT_THAT(src_ds, NotNull());

  auto alg = std::make_unique<GDALMaterializeRasterAlgorithm>();
  alg->GetInputDatasets().emplace_back(src_ds.get());
  alg->SetFormat("GTiff");
  alg->GetCreationOptions().push_back("TILED=NO");
  alg->GetCreationOptions().push_back("COPY_SRC_OVERVIEWS=NO");
  alg->GetCreationOptions().push_back("COMPRESS=NONE");
  alg->ParseCommandLineArguments({"-o", kOutTif});

  EXPECT_TRUE(RunStep(alg.get()));
  auto* out_ds = alg->GetOutputDataset().GetDatasetRef();
  ASSERT_THAT(out_ds, NotNull());
  EXPECT_EQ(10, out_ds->GetRasterXSize());
}

TEST_F(GdalalgMaterializeTest, RasterMaterializeUnknownDriverFails) {
  auto src_ds = CreateRasterMemDataset();
  ASSERT_THAT(src_ds, NotNull());

  auto alg = std::make_unique<GDALMaterializeRasterAlgorithm>();
  alg->GetInputDatasets().emplace_back(src_ds.get());
  alg->SetFormat("NON_EXISTENT_DRIVER");

  WithQuietHandler quiet;
  EXPECT_FALSE(RunStep(alg.get()));
}

TEST_F(GdalalgMaterializeTest, VectorStepProperties) {
  auto alg = std::make_unique<GDALMaterializeVectorAlgorithm>();
  ASSERT_THAT(alg, NotNull());
  EXPECT_EQ("materialize", alg->GetName());
  EXPECT_FALSE(alg->IsNativelyStreamingCompatible());
  EXPECT_FALSE(alg->CanBeMiddleStep());
  EXPECT_FALSE(alg->CanBeLastStep());
  EXPECT_FALSE(alg->GeneratesFilesFromUserInput());

  alg->ParseCommandLineArguments({"-o", "/vsimem/materialize_test/out.gpkg"});
  EXPECT_TRUE(alg->GeneratesFilesFromUserInput());
}

TEST_F(GdalalgMaterializeTest, VectorMaterializeMem) {
  auto src_ds = CreateVectorMemDataset();
  ASSERT_THAT(src_ds, NotNull());

  auto alg = std::make_unique<GDALMaterializeVectorAlgorithm>();
  alg->GetInputDatasets().emplace_back(src_ds.get());
  alg->SetFormat("MEM");

  EXPECT_TRUE(RunStep(alg.get()));
  auto* out_ds = alg->GetOutputDataset().GetDatasetRef();
  ASSERT_THAT(out_ds, NotNull());
  EXPECT_EQ(1, out_ds->GetLayerCount());
}

TEST_F(GdalalgMaterializeTest, VectorMaterializeDefaultGpkgAutoDelete) {
  auto src_ds = CreateVectorMemDataset();
  ASSERT_THAT(src_ds, NotNull());

  auto alg = std::make_unique<GDALMaterializeVectorAlgorithm>();
  alg->GetInputDatasets().emplace_back(src_ds.get());

  EXPECT_TRUE(RunStep(alg.get()));
  auto* out_ds = alg->GetOutputDataset().GetDatasetRef();
  ASSERT_THAT(out_ds, NotNull());
  EXPECT_EQ(1, out_ds->GetLayerCount());
}

TEST_F(GdalalgMaterializeTest, VectorMaterializeDefaultGpkgReopenRequired) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_THAT(drv, NotNull());
  drv->SetMetadataItem(GDAL_DCAP_REOPEN_AFTER_WRITE_REQUIRED, "YES");
  absl::Cleanup reset_meta = [drv] {
    drv->SetMetadataItem(GDAL_DCAP_REOPEN_AFTER_WRITE_REQUIRED, nullptr);
  };

  auto src_ds = CreateVectorMemDataset();
  ASSERT_THAT(src_ds, NotNull());

  auto alg = std::make_unique<GDALMaterializeVectorAlgorithm>();
  alg->GetInputDatasets().emplace_back(src_ds.get());

  EXPECT_TRUE(RunStep(alg.get()));
  auto* out_ds = alg->GetOutputDataset().GetDatasetRef();
  ASSERT_THAT(out_ds, NotNull());
  EXPECT_EQ(1, out_ds->GetLayerCount());
}

TEST_F(GdalalgMaterializeTest, VectorMaterializeDefaultSqliteListField) {
  auto src_ds = CreateVectorMemDataset(/*with_list_field=*/true);
  ASSERT_THAT(src_ds, NotNull());

  auto alg = std::make_unique<GDALMaterializeVectorAlgorithm>();
  alg->GetInputDatasets().emplace_back(src_ds.get());

  EXPECT_TRUE(RunStep(alg.get()));
  auto* out_ds = alg->GetOutputDataset().GetDatasetRef();
  ASSERT_THAT(out_ds, NotNull());
  EXPECT_EQ(1, out_ds->GetLayerCount());
}

TEST_F(GdalalgMaterializeTest, VectorMaterializeDefaultSqliteSecondGeomField) {
  auto src_ds = CreateVectorMemDataset(/*with_list_field=*/false,
                                       /*with_second_geom_field=*/true);
  ASSERT_THAT(src_ds, NotNull());

  auto alg = std::make_unique<GDALMaterializeVectorAlgorithm>();
  alg->GetInputDatasets().emplace_back(src_ds.get());

  EXPECT_TRUE(RunStep(alg.get()));
  auto* out_ds = alg->GetOutputDataset().GetDatasetRef();
  ASSERT_THAT(out_ds, NotNull());
  EXPECT_EQ(1, out_ds->GetLayerCount());
}

TEST_F(GdalalgMaterializeTest, VectorMaterializeExplicitOptionsAndProgress) {
  constexpr char kDir[] = "/vsimem/gdalalg_materialize_vector";
  constexpr char kOutGpkg[] =
      "/vsimem/gdalalg_materialize_vector/explicit.gpkg";
  absl::Cleanup clean_dir = [kDir] { VSIRmdirRecursive(kDir); };

  auto src_ds = CreateVectorMemDataset();
  ASSERT_THAT(src_ds, NotNull());

  auto alg = std::make_unique<GDALMaterializeVectorAlgorithm>();
  alg->GetInputDatasets().emplace_back(src_ds.get());
  alg->SetOverwrite(true);
  alg->SetFormat("GPKG");
  alg->GetCreationOptions().push_back("ADD_GPKG_OGR_CONTENTS=YES");
  alg->GetLayerCreationOptions().push_back("SPATIAL_INDEX=NO");
  alg->ParseCommandLineArguments({"-o", kOutGpkg});

  auto custom_progress = [](double, const char*, void*) -> int { return 1; };
  EXPECT_TRUE(RunStep(alg.get(), custom_progress, nullptr));

  auto* out_ds = alg->GetOutputDataset().GetDatasetRef();
  ASSERT_THAT(out_ds, NotNull());
  EXPECT_EQ(1, out_ds->GetLayerCount());
}

TEST_F(GdalalgMaterializeTest, VectorMaterializeSqliteExplicitSpatialite) {
  constexpr char kDir[] = "/vsimem/gdalalg_materialize_sqlite";
  constexpr char kOutSqlite[] =
      "/vsimem/gdalalg_materialize_sqlite/explicit.sqlite";
  absl::Cleanup clean_dir = [kDir] { VSIRmdirRecursive(kDir); };

  auto src_ds = CreateVectorMemDataset();
  ASSERT_THAT(src_ds, NotNull());

  auto alg = std::make_unique<GDALMaterializeVectorAlgorithm>();
  alg->GetInputDatasets().emplace_back(src_ds.get());
  alg->SetFormat("SQLite");
  alg->GetCreationOptions().push_back("SPATIALITE=NO");
  alg->ParseCommandLineArguments({"-o", kOutSqlite});

  EXPECT_TRUE(RunStep(alg.get()));
  auto* out_ds = alg->GetOutputDataset().GetDatasetRef();
  ASSERT_THAT(out_ds, NotNull());
}

TEST_F(GdalalgMaterializeTest, VectorMaterializeUnknownDriverFails) {
  auto src_ds = CreateVectorMemDataset();
  ASSERT_THAT(src_ds, NotNull());

  auto alg = std::make_unique<GDALMaterializeVectorAlgorithm>();
  alg->GetInputDatasets().emplace_back(src_ds.get());
  alg->SetFormat("NON_EXISTENT_VECTOR_DRIVER");

  WithQuietHandler quiet;
  EXPECT_FALSE(RunStep(alg.get()));
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover remaining unreachable/defensive branches in
// apps/gdalalg_materialize.cpp.
