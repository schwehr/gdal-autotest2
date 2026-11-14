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
// Tests for GDALVectorMakePointAlgorithm (apps/gdalalg_vector_make_point.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_make_point.cpp

#include "third_party/gdal/apps/gdalalg_vector_make_point.h"

#include <memory>
#include <string>

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
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_make_point_test";

std::unique_ptr<GDALDataset> CreateTableMemDataset(
    bool with_string_coords = false) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!drv) return nullptr;
  std::unique_ptr<GDALDataset> ds(
      drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  if (!ds) return nullptr;

  OGRLayer* lyr = ds->CreateLayer("tbl", nullptr, wkbPoint, nullptr);
  if (!lyr) return nullptr;

  OGRFieldDefn fx("x", with_string_coords ? OFTString : OFTReal);
  OGRFieldDefn fy("y", with_string_coords ? OFTString : OFTInteger);
  OGRFieldDefn fz("z", OFTReal);
  OGRFieldDefn fm("m", OFTReal);
  OGRFieldDefn fdate("dt", OFTDate);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&fx));
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&fy));
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&fz));
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&fm));
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&fdate));

  auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
  if (with_string_coords) {
    feat->SetField("x", "  12.5  ");
    feat->SetField("y", " 34.25 ");
  } else {
    feat->SetField("x", 10.5);
    feat->SetField("y", 20);
  }
  feat->SetField("z", 100.0);
  feat->SetField("m", 5.5);
  feat->SetField("dt", 2026, 1, 1, 0, 0, 0, 0);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  return ds;
}

class GdalalgVectorMakePointTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    RegisterOGRGeoPackage();
    RegisterOGRCSV();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgVectorMakePointTest, StandaloneProperties) {
  GDALVectorMakePointAlgorithmStandalone alg;
  EXPECT_EQ("make-point", alg.GetName());
  EXPECT_EQ("Create point geometries from attribute fields",
            alg.GetDescription());
}

TEST_F(GdalalgVectorMakePointTest, NumericAndStringCoordinatesAndDimensions) {
  auto num_ds = CreateTableMemDataset(/*with_string_coords=*/false);
  ASSERT_NE(nullptr, num_ds);

  // 1. 2D Point + --dst-crs.
  {
    GDALVectorMakePointAlgorithm alg;
    alg.GetInputDatasets().emplace_back(num_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--x", "x", "--y", "y", "--dst-crs", "EPSG:4326"}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* lyr = out_ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(wkbPoint, lyr->GetGeomType());
    EXPECT_TRUE(lyr->TestCapability(OLCRandomRead));
    ASSERT_NE(nullptr, lyr->GetSpatialRef());
    EXPECT_STREQ("4326", lyr->GetSpatialRef()->GetAuthorityCode(nullptr));

    auto feat = std::unique_ptr<OGRFeature>(lyr->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    const OGRPoint* pt = feat->GetGeometryRef()->toPoint();
    ASSERT_NE(nullptr, pt);
    EXPECT_DOUBLE_EQ(10.5, pt->getX());
    EXPECT_DOUBLE_EQ(20.0, pt->getY());
    EXPECT_FALSE(pt->Is3D());
    EXPECT_FALSE(pt->IsMeasured());
  }

  // 2. PointZ.
  num_ds->GetLayer(0)->ResetReading();
  {
    GDALVectorMakePointAlgorithm alg;
    alg.GetInputDatasets().emplace_back(num_ds.get());
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--x", "x", "--y", "y", "--z", "z"}));
    ASSERT_TRUE(alg.Run());
    OGRLayer* lyr = alg.GetOutputDataset().GetDatasetRef()->GetLayer(0);
    EXPECT_EQ(wkbPoint25D, lyr->GetGeomType());
    auto feat = std::unique_ptr<OGRFeature>(lyr->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    const OGRPoint* pt = feat->GetGeometryRef()->toPoint();
    EXPECT_TRUE(pt->Is3D());
    EXPECT_FALSE(pt->IsMeasured());
    EXPECT_DOUBLE_EQ(100.0, pt->getZ());
  }

  // 3. PointM.
  num_ds->GetLayer(0)->ResetReading();
  {
    GDALVectorMakePointAlgorithm alg;
    alg.GetInputDatasets().emplace_back(num_ds.get());
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--x", "x", "--y", "y", "--m", "m"}));
    ASSERT_TRUE(alg.Run());
    OGRLayer* lyr = alg.GetOutputDataset().GetDatasetRef()->GetLayer(0);
    EXPECT_EQ(wkbPointM, lyr->GetGeomType());
    auto feat = std::unique_ptr<OGRFeature>(lyr->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    const OGRPoint* pt = feat->GetGeometryRef()->toPoint();
    EXPECT_FALSE(pt->Is3D());
    EXPECT_TRUE(pt->IsMeasured());
    EXPECT_DOUBLE_EQ(5.5, pt->getM());
  }

  // 4. PointZM + string coordinates with whitespace.
  auto str_ds = CreateTableMemDataset(/*with_string_coords=*/true);
  ASSERT_NE(nullptr, str_ds);
  {
    GDALVectorMakePointAlgorithm alg;
    alg.GetInputDatasets().emplace_back(str_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--x", "x", "--y", "y", "--z", "z", "--m", "m"}));
    ASSERT_TRUE(alg.Run());
    OGRLayer* lyr = alg.GetOutputDataset().GetDatasetRef()->GetLayer(0);
    EXPECT_EQ(wkbPointZM, lyr->GetGeomType());
    auto feat = std::unique_ptr<OGRFeature>(lyr->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    const OGRPoint* pt = feat->GetGeometryRef()->toPoint();
    EXPECT_TRUE(pt->Is3D());
    EXPECT_TRUE(pt->IsMeasured());
    EXPECT_DOUBLE_EQ(12.5, pt->getX());
    EXPECT_DOUBLE_EQ(34.25, pt->getY());
    EXPECT_DOUBLE_EQ(100.0, pt->getZ());
    EXPECT_DOUBLE_EQ(5.5, pt->getM());
  }
}

TEST_F(GdalalgVectorMakePointTest, StandaloneCsvToGpkg) {
  const std::string in_csv = std::string(kTestDir) + "/pts.csv";
  const std::string out_gpkg = std::string(kTestDir) + "/pts.gpkg";
  {
    VSILFILE* fp = VSIFOpenL(in_csv.c_str(), "wb");
    ASSERT_NE(nullptr, fp);
    const std::string csv = "lon,lat,label\n1.5,2.5,alpha\n";
    EXPECT_EQ(csv.size(), VSIFWriteL(csv.data(), 1, csv.size(), fp));
    EXPECT_EQ(0, VSIFCloseL(fp));
  }

  {
    GDALVectorMakePointAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--x", "lon", "--y", "lat",
                                               "--dst-crs", "EPSG:4326", in_csv,
                                               out_gpkg, "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR));
  ASSERT_NE(nullptr, out_ds);
  auto feat =
      std::unique_ptr<OGRFeature>(out_ds->GetLayer(0)->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  const OGRPoint* pt = feat->GetGeometryRef()->toPoint();
  ASSERT_NE(nullptr, pt);
  EXPECT_DOUBLE_EQ(1.5, pt->getX());
  EXPECT_DOUBLE_EQ(2.5, pt->getY());
}

TEST_F(GdalalgVectorMakePointTest, ErrorPaths) {
  // 1. Dataset with 0 layers fails in Run().
  {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_NE(nullptr, drv);
    std::unique_ptr<GDALDataset> empty_ds(
        drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, empty_ds);

    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorMakePointAlgorithm alg;
    alg.GetInputDatasets().emplace_back(empty_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--x", "x", "--y", "y"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_NE(CE_None, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 2. Invalid --dst-crs fails at parse time or Run().
  auto num_ds = CreateTableMemDataset(false);
  ASSERT_NE(nullptr, num_ds);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorMakePointAlgorithm alg;
    alg.GetInputDatasets().emplace_back(num_ds.get());
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--x", "x", "--y", "y", "--dst-crs", "NOT_A_CRS"}));
    CPLErrorReset();
  }

  // 3. Missing X, Y, Z, or M field name emits error and FailTranslation().
  for (const std::vector<std::string>& args :
       std::vector<std::vector<std::string>>{
           {"--x", "no_x", "--y", "y"},
           {"--x", "x", "--y", "no_y"},
           {"--x", "x", "--y", "y", "--z", "no_z"},
           {"--x", "x", "--y", "y", "--m", "no_m"},
           {"--x", "dt", "--y", "y"},
       }) {
    num_ds->GetLayer(0)->ResetReading();
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorMakePointAlgorithm alg;
    alg.GetInputDatasets().emplace_back(num_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(args));
    ASSERT_TRUE(alg.Run());
    EXPECT_NE(CE_None, CPLGetLastErrorType());
    EXPECT_EQ(
        nullptr,
        alg.GetOutputDataset().GetDatasetRef()->GetLayer(0)->GetNextFeature());
    CPLErrorReset();
  }

  // 4. Invalid non-numeric string value in string coordinate field.
  {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_NE(nullptr, drv);
    std::unique_ptr<GDALDataset> bad_str_ds(
        drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* lyr = bad_str_ds->CreateLayer("tbl", nullptr, wkbNone, nullptr);
    OGRFieldDefn fx("x", OFTString);
    OGRFieldDefn fy("y", OFTString);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&fx));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&fy));
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    feat->SetField("x", "not_a_number");
    feat->SetField("y", "2.0");
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));

    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorMakePointAlgorithm alg;
    alg.GetInputDatasets().emplace_back(bad_str_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--x", "x", "--y", "y"}));
    ASSERT_TRUE(alg.Run());
    EXPECT_EQ(
        nullptr,
        alg.GetOutputDataset().GetDatasetRef()->GetLayer(0)->GetNextFeature());
    EXPECT_NE(CE_None, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
