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
// Tests for GDALVectorSQLAlgorithm (apps/gdalalg_vector_sql.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_sql.cpp

#include "third_party/gdal/apps/gdalalg_vector_sql.h"

#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_vector_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
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

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_sql_test";

class GdalalgVectorSqlTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    RegisterOGRGeoPackage();
    RegisterOGRSQLite();
    RegisterOGRGeoJSON();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

void CreatePointsGpkg(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);
  OGRLayer* lyr = ds->CreateLayer("pts", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, lyr);
  OGRFieldDefn fld("val", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&fld));

  for (int i = 1; i <= 3; ++i) {
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    feat->SetField("val", i * 10);
    auto pt = std::make_unique<OGRPoint>(static_cast<double>(i),
                                         static_cast<double>(i));
    ASSERT_EQ(OGRERR_NONE, feat->SetGeometryDirectly(pt.release()));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  }
}

TEST_F(GdalalgVectorSqlTest, Properties) {
  GDALVectorSQLAlgorithmStandalone alg;
  EXPECT_EQ("sql", alg.GetName());
  EXPECT_NE(nullptr, alg.GetArg("sql"));
  EXPECT_NE(nullptr, alg.GetArg("output-layer"));
  EXPECT_NE(nullptr, alg.GetArg("dialect"));
}

TEST_F(GdalalgVectorSqlTest, SingleStatementWithOutputLayerAndDialect) {
  const std::string in_gpkg = std::string(kTestDir) + "/in.gpkg";
  const std::string out_gpkg = std::string(kTestDir) + "/out.gpkg";
  const std::string sql_file = std::string(kTestDir) + "/query.sql";
  ASSERT_NO_FATAL_FAILURE(CreatePointsGpkg(in_gpkg));
  VsiMemTempWrapper sql_wrapper(
      sql_file, "-- SQL comment\nSELECT * FROM pts WHERE val >= 20;\n");

  GDALVectorSQLAlgorithmStandalone alg;
  ASSERT_TRUE(alg.ParseCommandLineArguments(
      {"-i", in_gpkg, "-o", out_gpkg, "-f", "GPKG", "--sql", "@" + sql_file,
       "--output-layer", "filtered", "--dialect", "OGRSQL"}));
  EXPECT_TRUE(alg.Run());
  ASSERT_TRUE(alg.Finalize());

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, out_ds);
  EXPECT_EQ(1, out_ds->GetLayerCount());
  OGRLayer* lyr = out_ds->GetLayerByName("filtered");
  ASSERT_NE(nullptr, lyr);
  EXPECT_EQ(2, lyr->GetFeatureCount());
}

TEST_F(GdalalgVectorSqlTest, MultiStatementDatasetAndAutoNaming) {
  const std::string in_gpkg = std::string(kTestDir) + "/multi_in.gpkg";
  const std::string out_gpkg = std::string(kTestDir) + "/multi_out.gpkg";
  const std::string out_named = std::string(kTestDir) + "/multi_named.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreatePointsGpkg(in_gpkg));

  // 1. Multiple SELECT statements without --output-layer trigger suffix
  // deduplication and ProxiedSQLLayer pooling.
  {
    GDALVectorSQLAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-i", in_gpkg, "-o", out_gpkg, "-f", "GPKG", "--sql",
         "SELECT * FROM pts WHERE val = 10", "--sql",
         "SELECT * FROM pts WHERE val = 30"}));
    EXPECT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(2, out_ds->GetLayerCount());
    OGRLayer* l0 = out_ds->GetLayer(0);
    OGRLayer* l1 = out_ds->GetLayer(1);
    ASSERT_NE(nullptr, l0);
    ASSERT_NE(nullptr, l1);
    EXPECT_EQ(1, l0->GetFeatureCount());
    EXPECT_EQ(1, l1->GetFeatureCount());
  }

  // 2. Multiple SELECT statements with explicit --output-layer names and
  // SQLITE dialect.
  {
    GDALVectorSQLAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-i", in_gpkg, "-o", out_named, "-f", "GPKG", "--dialect", "SQLITE",
         "--sql", "SELECT * FROM pts WHERE val = 10", "--sql",
         "SELECT * FROM pts WHERE val = 20", "--output-layer", "first",
         "--output-layer", "second"}));
    EXPECT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> out_ds(GDALDataset::Open(
        out_named.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, out_ds);
    EXPECT_NE(nullptr, out_ds->GetLayerByName("first"));
    EXPECT_NE(nullptr, out_ds->GetLayerByName("second"));
  }
}

TEST_F(GdalalgVectorSqlTest, InPlaceUpdateAndNoOutputModes) {
  const std::string gpkg = std::string(kTestDir) + "/inplace.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreatePointsGpkg(gpkg));

  // 1. In-place statement without result set (CREATE TABLE).
  {
    GDALVectorSQLAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-i", gpkg, "--update", "--sql",
         "CREATE TABLE extra (id INTEGER PRIMARY KEY, name TEXT)"}));
    EXPECT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());
  }

  // 2. Statement returning a result set without --output emits a warning.
  {
    GDALVectorSQLAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-i", gpkg, "--sql", "SELECT * FROM pts"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_TRUE(alg.Run());
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("returned a result set"));
    CPLErrorReset();
  }

  // 3. Same with --quiet suppresses the warning.
  {
    GDALVectorSQLAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-i", gpkg, "--quiet", "--sql", "SELECT * FROM pts"}));
    CPLErrorReset();
    EXPECT_TRUE(alg.Run());
    EXPECT_EQ(CE_None, CPLGetLastErrorType());
  }

  // 4. Failing statement without --update suggests 'update' argument.
  {
    GDALVectorSQLAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-i", gpkg, "--sql", "DROP TABLE nonexistent_table"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("'update' argument"));
    CPLErrorReset();
  }

  // 5. Failing statement with --update omits 'update' suggestion.
  {
    GDALVectorSQLAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-i", gpkg, "--update", "--sql", "DROP TABLE nonexistent_table"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(std::string::npos,
              std::string(CPLGetLastErrorMsg()).find("'update' argument"));
    CPLErrorReset();
  }
}

TEST_F(GdalalgVectorSqlTest, OutputErrorPathsAndPipelineUsage) {
  const std::string in_gpkg = std::string(kTestDir) + "/err_in.gpkg";
  const std::string out_gpkg = std::string(kTestDir) + "/err_out.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreatePointsGpkg(in_gpkg));

  // 1. Mismatched --output-layer count vs --sql count.
  {
    GDALVectorSQLAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-i", in_gpkg, "-o", out_gpkg, "-f", "GPKG", "--sql",
         "SELECT * FROM pts", "--sql", "SELECT * FROM pts", "--output-layer",
         "only_one"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("as many layer names"));
    CPLErrorReset();
  }

  // 2. Single-statement SQL returning no result layer when output is requested.
  {
    GDALVectorSQLAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-i", in_gpkg, "-o", out_gpkg, "-f", "GPKG", "--dialect", "SQLITE",
         "--sql", "PRAGMA cache_size = 2000"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("did not result in a result layer"));
    CPLErrorReset();
  }

  // 3. Multi-statement SQL where a statement produces no result layer.
  {
    GDALVectorSQLAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-i", in_gpkg, "-o", out_gpkg, "-f", "GPKG", "--dialect", "SQLITE",
         "--sql", "SELECT * FROM pts", "--sql", "PRAGMA cache_size = 2000"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("did not result in a result layer"));
    CPLErrorReset();
  }

  // 4. Multi-statement SQL with syntax error.
  {
    GDALVectorSQLAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-i", in_gpkg, "-o", out_gpkg, "-f", "GPKG", "--sql",
         "SELECT * FROM pts", "--sql", "SELECT * FROM missing_tbl"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }

  // 5. Pipeline usage with positional SQL arguments and streaming output.
  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", in_gpkg, "!", "sql", "SELECT * FROM pts",
         "SELECT * FROM pts WHERE val > 10", "-l", "all_pts", "-l", "big_pts",
         "!", "write", out_gpkg, "-f", "GPKG", "--overwrite"}));
    EXPECT_TRUE(pipeline.Run());
    ASSERT_TRUE(pipeline.Finalize());

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    ASSERT_NE(nullptr, ds->GetLayerByName("all_pts"));
    ASSERT_NE(nullptr, ds->GetLayerByName("big_pts"));
    EXPECT_EQ(3, ds->GetLayerByName("all_pts")->GetFeatureCount());
    EXPECT_EQ(2, ds->GetLayerByName("big_pts")->GetFeatureCount());
  }
}

}  // namespace
}  // namespace autotest2
