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
// Unit tests for ogrparquetdataset.cpp (OGRParquetDataset).
//
// Build caveat in google3: GDAL_USE_ARROWDATASET is not defined in
// //third_party/gdal/BUILD, so partitioned directory datasets and
// OGRParquetDatasetLayer are compiled out.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/parquet/ogrparquetdataset.cpp
//   https://gdal.org/drivers/vector/parquet.html

#include <filesystem>  // NOLINT
#include <memory>
#include <string>

#include "arrow/api.h"
#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/ogr/ogrsf_frmts/parquet/parquet_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/ogr/ogrsf_frmts/parquet/ogr_parquet.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/ogrparquetdataset_test";

class OgrParquetDatasetTest : public ::testing::Test {
 protected:
  void SetUp() override { RegisterOGRParquet(); }
};

TEST_F(OgrParquetDatasetTest, LayerAccessAndCapabilities) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };
  constexpr char kPath[] = "/vsimem/ogrparquetdataset_test/caps.parquet";
  ASSERT_TRUE(CreateSampleParquetViaGdal(kPath, /*row_count=*/6,
                                         /*row_group_size=*/3,
                                         /*with_fid_column=*/true));

  auto ds = OpenParquetDataset(kPath);
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(1, ds->GetLayerCount());
  EXPECT_NE(nullptr, ds->GetLayer(0));
  EXPECT_EQ(nullptr, ds->GetLayer(-1));
  EXPECT_EQ(nullptr, ds->GetLayer(1));
  EXPECT_NE(nullptr, ds->GetLayerByName("caps"));
  EXPECT_EQ(nullptr, ds->GetLayerByName("nonexistent"));

  EXPECT_EQ(TRUE, ds->TestCapability(ODsCZGeometries));
  EXPECT_EQ(TRUE, ds->TestCapability(ODsCMeasuredGeometries));
  EXPECT_EQ(FALSE, ds->TestCapability(ODsCCreateLayer));
  EXPECT_EQ(CE_None, ds->Close());
}

TEST_F(OgrParquetDatasetTest, CreateReaderLayerErrors) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };
  VSIMkdirRecursive(kTestDir, 0755);

  OGRParquetDataset ds;
  VSILFILE* fp = nullptr;

  // 1. Missing /vsimem/ file returns nullptr cleanly.
  EXPECT_EQ(nullptr,
            ds.CreateReaderLayer("/vsimem/ogrparquetdataset_test/no.parquet",
                                 fp, nullptr));

  // 2. Missing local filesystem file returns error from arrow::ReadableFile.
  {
    const std::string missing_posix =
        (std::filesystem::path(::testing::TempDir()) / "missing_sub" /
         "file.parquet")
            .string();
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              ds.CreateReaderLayer(missing_posix.c_str(), fp, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 3. Invalid Parquet bytes fail FileReaderBuilder::Open().
  constexpr char kBadPath[] = "/vsimem/ogrparquetdataset_test/bad.parquet";
  ASSERT_TRUE(WriteBytesToVsiMem(kBadPath, "PAR1bad_bytes_not_parquetPAR1"));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds.CreateReaderLayer(kBadPath, fp, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("FileReaderBuilder::Open() failed"));
    CPLErrorReset();
  }

  // 4. Truncated Parquet header/footer size fails FileReaderBuilder::Open().
  constexpr char kSmallPath[] = "/vsimem/ogrparquetdataset_test/small.parquet";
  ASSERT_TRUE(WriteBytesToVsiMem(kSmallPath, "PAR1____PAR1"));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    VSILFILE* fp_in = VSIFOpenL(kSmallPath, "rb");
    ASSERT_NE(nullptr, fp_in);
    EXPECT_EQ(nullptr, ds.CreateReaderLayer(kSmallPath, fp_in, nullptr));
    EXPECT_EQ(nullptr, fp_in);  // CreateReaderLayer took ownership of fp_in.
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 5. Explicit batch size & threads config options on a valid file.
  constexpr char kGoodPath[] = "/vsimem/ogrparquetdataset_test/good.parquet";
  ASSERT_TRUE(CreateSampleParquetViaGdal(kGoodPath, /*row_count=*/4,
                                         /*row_group_size=*/2));
  CPLConfigOptionSetter batch_opt("OGR_PARQUET_BATCH_SIZE", "2", false);
  CPLConfigOptionSetter thread_opt("OGR_PARQUET_USE_THREADS", "YES", false);
  CPLConfigOptionSetter vsi_opt("OGR_PARQUET_USE_VSI", "YES", false);
  VSILFILE* fp_good = nullptr;
  auto layer = ds.CreateReaderLayer(kGoodPath, fp_good, nullptr);
  ASSERT_NE(nullptr, layer);
  EXPECT_EQ(4, layer->GetFeatureCount(/*bForce=*/1));
}

TEST_F(OgrParquetDatasetTest, ExecuteSqlMinMaxCountOptimizations) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };
  constexpr char kPath[] = "/vsimem/ogrparquetdataset_test/stats.parquet";
  ASSERT_TRUE(CreateSampleParquetViaGdal(kPath, /*row_count=*/6,
                                         /*row_group_size=*/2,
                                         /*with_fid_column=*/true));

  auto ds = OpenParquetDataset(kPath);
  ASSERT_NE(nullptr, ds);

  // 1. Optimized MIN/MAX/COUNT across multiple row groups (including FID and
  // regular columns, with aliases).
  {
    OGRLayer* sql_lyr = ds->ExecuteSQL(
        "SELECT MIN(id), MAX(id) AS max_id, COUNT(name), MIN(val), MAX(val), "
        "MIN(fid), MAX(fid) FROM stats",
        nullptr, nullptr);
    ASSERT_NE(nullptr, sql_lyr);
    absl::Cleanup release = [&ds, sql_lyr] { ds->ReleaseResultSet(sql_lyr); };

    std::unique_ptr<OGRFeature> feat(sql_lyr->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(0, feat->GetFieldAsInteger("MIN_id"));
    EXPECT_EQ(5, feat->GetFieldAsInteger("max_id"));
    // rows 0, 1, 3, 4 have non-null name (i % 3 != 2 => rows 2, 5 are null)
    EXPECT_EQ(4, feat->GetFieldAsInteger64("COUNT_name"));
    EXPECT_DOUBLE_EQ(0.0, feat->GetFieldAsDouble("MIN_val"));
    EXPECT_DOUBLE_EQ(7.5, feat->GetFieldAsDouble("MAX_val"));
    EXPECT_EQ(1, feat->GetFieldAsInteger64("MIN_fid"));
    EXPECT_EQ(51, feat->GetFieldAsInteger64("MAX_fid"));
    EXPECT_EQ(nullptr, sql_lyr->GetNextFeature());
  }

  // 2. Fallback when Arrow distinct_count() is 0 with non-null values.
  {
    OGRLayer* sql_lyr =
        ds->ExecuteSQL("SELECT COUNT(DISTINCT id) FROM stats", nullptr, "");
    ASSERT_NE(nullptr, sql_lyr);
    absl::Cleanup release = [&ds, sql_lyr] { ds->ReleaseResultSet(sql_lyr); };
    std::unique_ptr<OGRFeature> feat(sql_lyr->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(6, feat->GetFieldAsInteger64(0));
  }

  // 3. Fallbacks: WHERE filter, ORDER BY, AVG, CAST/target_type, missing table
  // or column, invalid SQL syntax, non-OGRSQL dialect.
  {
    OGRLayer* sql_lyr = ds->ExecuteSQL(
        "SELECT MIN(id) FROM stats WHERE id >= 2", nullptr, "OGRSQL");
    ASSERT_NE(nullptr, sql_lyr);
    absl::Cleanup release = [&ds, sql_lyr] { ds->ReleaseResultSet(sql_lyr); };
    std::unique_ptr<OGRFeature> feat(sql_lyr->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(2, feat->GetFieldAsInteger(0));
  }
  {
    OGRLayer* sql_lyr =
        ds->ExecuteSQL("SELECT COUNT(*) FROM stats", nullptr, nullptr);
    ASSERT_NE(nullptr, sql_lyr);
    absl::Cleanup release = [&ds, sql_lyr] { ds->ReleaseResultSet(sql_lyr); };
    std::unique_ptr<OGRFeature> feat(sql_lyr->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(6, feat->GetFieldAsInteger64(0));
  }
  {
    OGRLayer* sql_lyr = ds->ExecuteSQL(
        "SELECT DISTINCT id FROM stats ORDER BY id", nullptr, "");
    ASSERT_NE(nullptr, sql_lyr);
    absl::Cleanup release = [&ds, sql_lyr] { ds->ReleaseResultSet(sql_lyr); };
    std::unique_ptr<OGRFeature> feat(sql_lyr->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(0, feat->GetFieldAsInteger(0));
  }
  {
    OGRLayer* sql_lyr =
        ds->ExecuteSQL("SELECT MIN(id) FROM stats", nullptr, "SQLITE");
    ASSERT_NE(nullptr, sql_lyr);
    absl::Cleanup release = [&ds, sql_lyr] { ds->ReleaseResultSet(sql_lyr); };
    std::unique_ptr<OGRFeature> feat(sql_lyr->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(0, feat->GetFieldAsInteger(0));
  }

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds->ExecuteSQL("SELECT FROM", nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              ds->ExecuteSQL("SELECT MIN(id) FROM missing_tbl", nullptr, ""));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds->ExecuteSQL("SELECT MIN(missing_col) FROM stats",
                                      nullptr, ""));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST_F(OgrParquetDatasetTest, ExecuteSqlStatsUnavailableOrUnmapped) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  // 1. Dataset with STATISTICS=NO and empty row groups -> falls back cleanly.
  constexpr char kNoStatsPath[] =
      "/vsimem/ogrparquetdataset_test/nostats.parquet";
  {
    VSIMkdirRecursive(kTestDir, 0755);
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("Parquet");
    ASSERT_NE(nullptr, drv);
    GDALDatasetUniquePtr wr(
        drv->Create(kNoStatsPath, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, wr);
    CPLStringList opts;
    opts.SetNameValue("STATISTICS", "NO");
    OGRLayer* lyr = nullptr;
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      lyr = wr->CreateLayer("nostats", nullptr, wkbNone, opts.List());
      CPLErrorReset();
    }
    ASSERT_NE(nullptr, lyr);
    OGRFieldDefn fld("x", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&fld));
    OGRFeature f(lyr->GetLayerDefn());
    f.SetField(0, 42);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(&f));
    EXPECT_EQ(CE_None, wr->Close());
  }
  {
    auto ds = OpenParquetDataset(kNoStatsPath);
    ASSERT_NE(nullptr, ds);
    OGRLayer* sql_lyr = ds->ExecuteSQL(
        "SELECT MIN(x), MAX(x), COUNT(x) FROM nostats", nullptr, nullptr);
    ASSERT_NE(nullptr, sql_lyr);
    absl::Cleanup release = [&ds, sql_lyr] { ds->ReleaseResultSet(sql_lyr); };
    std::unique_ptr<OGRFeature> feat(sql_lyr->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(42, feat->GetFieldAsInteger(0));
    EXPECT_EQ(42, feat->GetFieldAsInteger(1));
    EXPECT_EQ(1, feat->GetFieldAsInteger64(2));
  }

  // 2. Empty table (0 row groups) still handles MIN/MAX/COUNT cleanly.
  constexpr char kEmptyPath[] = "/vsimem/ogrparquetdataset_test/empty.parquet";
  auto schema = arrow::schema({arrow::field("v", arrow::int32())});
  auto empty_arr_res = arrow::MakeArrayOfNull(arrow::int32(), 0);
  ASSERT_TRUE(empty_arr_res.ok());
  auto empty_table = arrow::Table::Make(schema, {*empty_arr_res});
  ASSERT_TRUE(WriteArrowTableToVsiMem(kEmptyPath, empty_table));
  {
    auto ds = OpenParquetDataset(kEmptyPath);
    ASSERT_NE(nullptr, ds);
    OGRLayer* sql_lyr =
        ds->ExecuteSQL("SELECT MIN(v) FROM empty", nullptr, nullptr);
    ASSERT_NE(nullptr, sql_lyr);
    absl::Cleanup release = [&ds, sql_lyr] { ds->ReleaseResultSet(sql_lyr); };
    std::unique_ptr<OGRFeature> feat(sql_lyr->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_FALSE(feat->IsFieldSetAndNotNull(0));
  }

  // 3. List field COUNT(arr) exercises generic SQL fallback when a nested
  // column cannot be answered from Parquet metadata stats.
  auto list_type = arrow::list(arrow::int32());
  auto list_schema = arrow::schema({arrow::field("arr", list_type)});
  arrow::ListBuilder lb(arrow::default_memory_pool(),
                        std::make_shared<arrow::Int32Builder>());
  ASSERT_TRUE(lb.Append().ok());
  ASSERT_TRUE(
      static_cast<arrow::Int32Builder*>(lb.value_builder())->Append(42).ok());
  std::shared_ptr<arrow::Array> list_arr;
  ASSERT_TRUE(lb.Finish(&list_arr).ok());
  auto list_table = arrow::Table::Make(list_schema, {list_arr});
  constexpr char kListPath[] = "/vsimem/ogrparquetdataset_test/list.parquet";
  ASSERT_TRUE(WriteArrowTableToVsiMem(kListPath, list_table));
  {
    auto ds = OpenParquetDataset(kListPath);
    ASSERT_NE(nullptr, ds);
    OGRLayer* sql_lyr =
        ds->ExecuteSQL("SELECT COUNT(arr) FROM list", nullptr, nullptr);
    ASSERT_NE(nullptr, sql_lyr);
    absl::Cleanup release = [&ds, sql_lyr] { ds->ReleaseResultSet(sql_lyr); };
    std::unique_ptr<OGRFeature> feat(sql_lyr->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(1, feat->GetFieldAsInteger64(0));
  }
}

// TODO: b/335317901 - Cover remaining defensive fallback branches in
// OGRParquetDataset (ARROW_DATASET paths, VSI file open edge errors, and
// SQL MIN/MAX non-column fallback paths).

}  // namespace
}  // namespace autotest2
