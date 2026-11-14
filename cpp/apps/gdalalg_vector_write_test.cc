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
// Tests for GDALVectorWriteAlgorithm (apps/gdalalg_vector_write.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_write.cpp

#include "third_party/gdal/apps/gdalalg_vector_write.h"

#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
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
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_write_test";

using ::testing::HasSubstr;

GDALDatasetUniquePtr CreatePointGpkg(const std::string& path, int id_start = 1,
                                     const char* val = "v1",
                                     bool with_unique_id = false) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  if (!drv) return nullptr;
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  if (!ds) return nullptr;

  OGRLayer* lyr = ds->CreateLayer("pts", nullptr, wkbPoint, nullptr);
  if (!lyr) return nullptr;
  if (with_unique_id) {
    OGRFieldDefn id_fld("id", OFTInteger);
    id_fld.SetUnique(true);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&id_fld));
  }
  OGRFieldDefn fld("tag", OFTString);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&fld));

  for (int i = 0; i < 2; ++i) {
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    feat->SetFID(id_start + i);
    if (with_unique_id) {
      feat->SetField("id", id_start + i);
    }
    feat->SetField("tag", val);
    auto pt = std::make_unique<OGRPoint>(static_cast<double>(i), 1.0);
    feat->SetGeometryDirectly(pt.release());
    EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  }
  return ds;
}

class GdalalgVectorWriteTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    RegisterOGRGeoPackage();
    RegisterOGRGeoJSON();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgVectorWriteTest, Properties) {
  GDALVectorWriteAlgorithm alg;
  EXPECT_EQ("write", alg.GetName());
  EXPECT_EQ("Write a vector dataset.", alg.GetDescription());
  EXPECT_TRUE(alg.CanBeLastStep());
  EXPECT_TRUE(alg.GeneratesFilesFromUserInput());
  EXPECT_FALSE(alg.IsNativelyStreamingCompatible());
}

TEST_F(GdalalgVectorWriteTest, StreamAndWriteOptions) {
  const std::string in_gpkg = std::string(kTestDir) + "/in.gpkg";
  const std::string out_gpkg = std::string(kTestDir) + "/out.gpkg";
  ASSERT_NE(nullptr, CreatePointGpkg(in_gpkg));

  // 1. Stream output format returns input dataset directly.
  {
    GDALVectorPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", in_gpkg, "!", "write", "--output-format", "stream", ""}));
    EXPECT_TRUE(pipe.Run());
    GDALDataset* ds = pipe.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(2, ds->GetLayer(0)->GetFeatureCount());
  }

  // 2. Write with format, creation-option, layer-creation-option,
  //    output-layer, skip-errors, and progress callback.
  {
    GDALVectorPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", in_gpkg, "!", "write", "--output-format", "GPKG",
         "--creation-option", "ADD_GPKG_OGR_CONTENTS=YES",
         "--layer-creation-option", "SPATIAL_INDEX=NO", "--output-layer",
         "pts_out", "--skip-errors", "--overwrite", out_gpkg}));
    auto progress = [](double, const char*, void*) -> int { return 1; };
    EXPECT_TRUE(pipe.Run(progress, nullptr));
    EXPECT_TRUE(pipe.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR));
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* lyr = out_ds->GetLayerByName("pts_out");
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(2, lyr->GetFeatureCount());
  }
}

TEST_F(GdalalgVectorWriteTest, AppendOverwriteLayerAndUpsert) {
  const std::string in_gpkg = std::string(kTestDir) + "/u_in.gpkg";
  const std::string out_gpkg = std::string(kTestDir) + "/u_out.gpkg";
  ASSERT_NE(nullptr, CreatePointGpkg(in_gpkg, 1, "initial"));

  // Initial write.
  {
    GDALVectorPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", in_gpkg, "!", "write", out_gpkg, "--overwrite"}));
    EXPECT_TRUE(pipe.Run());
    EXPECT_TRUE(pipe.Finalize());
  }

  // 1. --append.
  {
    GDALVectorPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", in_gpkg, "!", "write", "--append", out_gpkg}));
    EXPECT_TRUE(pipe.Run());
    EXPECT_TRUE(pipe.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR));
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(4, out_ds->GetLayerByName("pts")->GetFeatureCount());
  }

  // 2. --overwrite-layer.
  {
    GDALVectorPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", in_gpkg, "!", "write", "--overwrite-layer", out_gpkg}));
    EXPECT_TRUE(pipe.Run());
    EXPECT_TRUE(pipe.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR));
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(2, out_ds->GetLayerByName("pts")->GetFeatureCount());
  }

  // 3. --upsert updates existing UNIQUE id=2 and inserts id=3.
  const std::string uniq_in_gpkg = std::string(kTestDir) + "/uniq_in.gpkg";
  const std::string uniq_upd_gpkg = std::string(kTestDir) + "/uniq_upd.gpkg";
  const std::string upsert_out_gpkg =
      std::string(kTestDir) + "/upsert_out.gpkg";
  ASSERT_NE(nullptr, CreatePointGpkg(uniq_in_gpkg, 1, "initial",
                                     /*with_unique_id=*/true));
  ASSERT_NE(nullptr, CreatePointGpkg(uniq_upd_gpkg, 2, "updated",
                                     /*with_unique_id=*/true));
  {
    GDALVectorPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", uniq_in_gpkg, "!", "write", upsert_out_gpkg, "--overwrite"}));
    EXPECT_TRUE(pipe.Run());
    EXPECT_TRUE(pipe.Finalize());
  }
  {
    GDALVectorPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", uniq_upd_gpkg, "!", "write", "--upsert", upsert_out_gpkg}));
    EXPECT_TRUE(pipe.Run());
    EXPECT_TRUE(pipe.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(upsert_out_gpkg.c_str(), GDAL_OF_VECTOR));
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* lyr = out_ds->GetLayerByName("pts");
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(3, lyr->GetFeatureCount());
    auto f2 = std::unique_ptr<OGRFeature>(lyr->GetFeature(2));
    ASSERT_NE(nullptr, f2);
    EXPECT_EQ(2, f2->GetFieldAsInteger("id"));
    EXPECT_STREQ("updated", f2->GetFieldAsString("tag"));
  }
}

TEST_F(GdalalgVectorWriteTest, ErrorPaths) {
  const std::string in_gpkg = std::string(kTestDir) + "/err_in.gpkg";
  const std::string out_gpkg = std::string(kTestDir) + "/err_out.gpkg";
  ASSERT_NE(nullptr, CreatePointGpkg(in_gpkg));
  ASSERT_NE(nullptr, CreatePointGpkg(out_gpkg));

  // Existing output without --overwrite fails validation at parse time.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorPipelineAlgorithm pipe;
    EXPECT_FALSE(pipe.ParseCommandLineArguments(
        {"read", in_gpkg, "!", "write", out_gpkg}));
    EXPECT_NE(CE_None, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("already exists"));
    CPLErrorReset();
  }

  // Invalid / unknown output driver extension fails.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", in_gpkg, "!", "write",
         std::string(kTestDir) + "/bad.unknown_ext"}));
    EXPECT_FALSE(pipe.Run());
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
