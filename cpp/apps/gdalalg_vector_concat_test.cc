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
// Tests for GDALVectorConcatAlgorithm (apps/gdalalg_vector_concat.cpp and
// apps/gdalalg_vector_concat.h).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_concat.cpp

#include "third_party/gdal/apps/gdalalg_vector_concat.h"

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/gdalalg_vector_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_concat_test";

void CreatePointGpkg(const std::string& path, const std::string& layer_name,
                     int epsg, double x, double y,
                     const std::vector<std::string>& extra_fields) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRSpatialReference srs;
  OGRSpatialReference* srs_ptr = nullptr;
  if (epsg > 0) {
    ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(epsg));
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    srs_ptr = &srs;
  }

  OGRLayer* lyr =
      ds->CreateLayer(layer_name.c_str(), srs_ptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, lyr);
  for (const std::string& f : extra_fields) {
    OGRFieldDefn fld(f.c_str(), OFTString);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&fld));
  }

  auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
  for (const std::string& f : extra_fields) {
    feat->SetField(f.c_str(), (layer_name + "_" + f).c_str());
  }
  auto pt = std::make_unique<OGRPoint>(x, y);
  feat->SetGeometryDirectly(pt.release());
  ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
}

void RegisterDrivers() {
  GDALRegister_MEM();
  RegisterOGRGeoPackage();
  RegisterOGRGeoJSON();
}

TEST(GdalalgVectorConcatTest, MergeSingleStackModesAndFieldStrategy) {
  RegisterDrivers();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  const std::string g1 = std::string(kTestDir) + "/g1.gpkg";
  const std::string g2 = std::string(kTestDir) + "/g2.gpkg";
  ASSERT_NO_FATAL_FAILURE(
      CreatePointGpkg(g1, "pts", 4326, 1.0, 2.0, {"common", "only1"}));
  ASSERT_NO_FATAL_FAILURE(
      CreatePointGpkg(g2, "pts", 4326, 3.0, 4.0, {"common", "only2"}));

  // 1. Default merge-per-layer-name + union field strategy + standalone GPKG
  // write.
  const std::string out_merge = std::string(kTestDir) + "/merged.gpkg";
  {
    GDALVectorConcatAlgorithmStandalone alg;
    EXPECT_EQ("concat", alg.GetName());
    EXPECT_FALSE(alg.GetDescription().empty());
    EXPECT_FALSE(alg.GetHelpURL().empty());

    ASSERT_TRUE(alg.ParseCommandLineArguments({g1, g2, out_merge}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        out_merge.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(1, ds->GetLayerCount());
    OGRLayer* lyr = ds->GetLayer(0);
    EXPECT_EQ(2, lyr->GetFeatureCount());
    EXPECT_GE(lyr->GetLayerDefn()->GetFieldIndex("only1"), 0);
    EXPECT_GE(lyr->GetLayerDefn()->GetFieldIndex("only2"), 0);
  }

  // 2. Single mode with intersection field strategy and source-layer-field.
  {
    GDALVectorConcatAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--mode=single", "--field-strategy=intersection",
         "--source-layer-field-name=src_lyr",
         "--source-layer-field-content={DS_BASENAME}_{DS_INDEX}_{LAYER_NAME}",
         g1, g2, "unused"}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(1, out_ds->GetLayerCount());
    EXPECT_EQ(1, out_ds->TestCapability(ODsCCurveGeometries));
    EXPECT_EQ(1, out_ds->TestCapability(ODsCMeasuredGeometries));
    EXPECT_EQ(1, out_ds->TestCapability(ODsCZGeometries));
    EXPECT_EQ(0, out_ds->TestCapability("NonExistentCap"));
    EXPECT_EQ(nullptr, out_ds->GetLayer(-1));
    EXPECT_EQ(nullptr, out_ds->GetLayer(99));

    OGRLayer* lyr = out_ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_STREQ("merged", lyr->GetName());
    EXPECT_GE(lyr->GetLayerDefn()->GetFieldIndex("src_lyr"), 0);
    EXPECT_GE(lyr->GetLayerDefn()->GetFieldIndex("common"), 0);
    EXPECT_EQ(-1, lyr->GetLayerDefn()->GetFieldIndex("only1"));
  }

  // 3. Stack mode with placeholders (both matching and non-matching basename).
  const std::string same_name = std::string(kTestDir) + "/same.gpkg";
  ASSERT_NO_FATAL_FAILURE(
      CreatePointGpkg(same_name, "same", 4326, 0.0, 0.0, {}));
  {
    GDALVectorConcatAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--mode=stack",
         "--output-layer={AUTO_NAME}_{DS_NAME}_{LAYER_INDEX}",
         "--source-layer-field-content={AUTO_NAME}", g1, same_name, "unused"}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(2, out_ds->GetLayerCount());
  }
}

TEST(GdalalgVectorConcatTest, ReprojectionLayerFilterAndPool) {
  RegisterDrivers();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  const std::string g_4326 = std::string(kTestDir) + "/wgs84.gpkg";
  const std::string g_3857 = std::string(kTestDir) + "/merc.gpkg";
  const std::string g_nosrs = std::string(kTestDir) + "/nosrs.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreatePointGpkg(g_4326, "pts", 4326, 2.0, 48.0, {}));
  ASSERT_NO_FATAL_FAILURE(
      CreatePointGpkg(g_3857, "pts", 3857, 222638.98, 6106854.83, {}));
  ASSERT_NO_FATAL_FAILURE(CreatePointGpkg(g_nosrs, "pts", 0, 1.0, 2.0, {}));

  // 1. Reproject mixed CRS inputs to EPSG:3857.
  {
    GDALVectorConcatAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dst-crs=EPSG:3857", "--input-layer=pts", g_4326,
         g_3857, "unused"}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* lyr = out_ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(2, lyr->GetFeatureCount());
  }

  // 2. --src-crs overrides missing CRS on input layer.
  {
    GDALVectorConcatAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--src-crs=EPSG:4326", "--dst-crs=EPSG:3857", g_nosrs,
         "unused"}));
    EXPECT_TRUE(alg.Run());
  }

  // 3. Pooled opening when GDAL_VECTOR_CONCAT_MAX_OPENED_DATASETS=1 with 3 DS.
  {
    CPLConfigOptionSetter max_open_setter(
        "GDAL_VECTOR_CONCAT_MAX_OPENED_DATASETS", "1",
        /*bSetOnlyIfUndefined=*/false);
    GDALVectorConcatAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", g_4326, g_3857, g_nosrs, "unused"}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* lyr = out_ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    int count = 0;
    std::unique_ptr<OGRFeature> feat;
    while ((feat.reset(lyr->GetNextFeature())), feat != nullptr) {
      ++count;
    }
    EXPECT_EQ(3, count);
  }

  // 4. Non-pooled second-pass open when nonOpenedDSCount == 1 (2 DS, max = 1).
  {
    CPLConfigOptionSetter max_open_setter(
        "GDAL_VECTOR_CONCAT_MAX_OPENED_DATASETS", "1",
        /*bSetOnlyIfUndefined=*/false);
    GDALVectorConcatAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", g_4326, g_3857, "unused"}));
    EXPECT_TRUE(alg.Run());
  }

  // 5. Inside GDALVectorPipelineAlgorithm.
  const std::string pipe_out = std::string(kTestDir) + "/pipe.gpkg";
  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"concat", g_4326, g_3857, "--dst-crs=EPSG:4326", "!", "write",
         pipe_out, "--overwrite"}));
    EXPECT_TRUE(pipeline.Run());
    EXPECT_TRUE(pipeline.Finalize());
  }
  VSIStatBufL stat_buf;
  EXPECT_EQ(0, VSIStatL(pipe_out.c_str(), &stat_buf));
}

TEST(GdalalgVectorConcatTest, ErrorPaths) {
  RegisterDrivers();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  const std::string g_nosrs = std::string(kTestDir) + "/err_nosrs.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreatePointGpkg(g_nosrs, "pts", 0, 1.0, 2.0, {}));

  // 1. --output-layer disallowed in mode=merge-per-layer-name.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALVectorConcatAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--mode=merge-per-layer-name", "--output-layer=bad",
         g_nosrs, "unused"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("cannot be specified"));
    CPLErrorReset();
  }

  // 2. --dst-crs without --src-crs on layer with no SRS fails.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALVectorConcatAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dst-crs=EPSG:4326", g_nosrs, "unused"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("has no spatial reference system"));
    CPLErrorReset();
  }

  // 3. Missing input file fails.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALVectorConcatAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", std::string(kTestDir) + "/missing.gpkg", "unused"}));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
