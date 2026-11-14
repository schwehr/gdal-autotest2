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
// Tests for "partition" step of "vector pipeline"
// (apps/gdalalg_vector_partition.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_partition.cpp

#include "third_party/gdal/apps/gdalalg_vector_partition.h"

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_vector_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_partition_test";
constexpr char kTmpDir[] = "/vsimem/gdalalg_vector_partition_tmp";

class GdalalgVectorPartitionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
    ASSERT_EQ(0, VSIMkdir(kTmpDir, 0755));
    CPLSetConfigOption("CPL_TMPDIR", kTmpDir);
    GDALRegister_MEM();
    RegisterOGRGML();
    RegisterOGRGeoJSON();
    RegisterOGRGeoPackage();
    RegisterOGRSQLite();
    RegisterOGRShape();
  }

  void TearDown() override {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(kTestDir);
    VSIRmdirRecursive(kTmpDir);
    CPLErrorReset();
  }
};

std::unique_ptr<GDALDataset> CreateRichMemDataset(bool with_lists,
                                                  bool with_time = true) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!drv) return nullptr;
  auto ds = std::unique_ptr<GDALDataset>(
      drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  if (!ds) return nullptr;

  OGRLayer* lyr = ds->CreateLayer("items", nullptr, wkbPoint, nullptr);
  if (!lyr) return nullptr;

  auto add_field = [lyr](const char* name, OGRFieldType type) {
    OGRFieldDefn fd(name, type);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&fd));
  };
  add_field("part_str", OFTString);
  add_field("part_int", OFTInteger);
  add_field("part_int64", OFTInteger64);
  add_field("dbl_val", OFTReal);
  add_field("bin_val", OFTBinary);
  if (with_time) {
    add_field("time_val", OFTTime);
  }
  add_field("date_val", OFTDate);
  add_field("dt_val", OFTDateTime);
  if (with_lists) {
    add_field("int_list", OFTIntegerList);
    add_field("int64_list", OFTInteger64List);
    add_field("real_list", OFTRealList);
    add_field("str_list", OFTStringList);
  }

  struct RowSpec {
    const char* s;
    int i;
    GIntBig i64;
    bool is_null;
  };
  constexpr RowSpec kRows[] = {
      {.s = "alpha/beta", .i = 1, .i64 = 10000000000LL, .is_null = false},
      {.s = "a=b:c", .i = 2, .i64 = 20000000000LL, .is_null = false},
      {.s = nullptr, .i = 3, .i64 = 30000000000LL, .is_null = true},
      {.s = "alpha/beta", .i = 1, .i64 = 10000000000LL, .is_null = false},
  };
  for (const auto& row : kRows) {
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    if (!row.is_null) {
      feat->SetField("part_str", row.s);
    }
    feat->SetField("part_int", row.i);
    feat->SetField("part_int64", row.i64);
    feat->SetField("dbl_val", 3.14);
    const GByte bytes[2] = {0x01, 0x02};
    feat->SetField(feat->GetFieldIndex("bin_val"), 2, bytes);
    if (with_time) {
      feat->SetField("time_val", 2025, 1, 2, 12, 30, 0.0f, 0);
    }
    feat->SetField("date_val", 2025, 1, 2, 0, 0, 0.0f, 0);
    feat->SetField("dt_val", 2025, 1, 2, 12, 30, 0.0f, 0);
    if (with_lists) {
      const int il[2] = {1, 2};
      const GIntBig i64l[2] = {10, 20};
      const double rl[2] = {1.5, 2.5};
      const char* const sl[3] = {"x", "y", nullptr};
      feat->SetField("int_list", 2, il);
      feat->SetField("int64_list", 2, i64l);
      feat->SetField("real_list", 2, rl);
      feat->SetField("str_list", sl);
    }
    auto pt = std::make_unique<OGRPoint>(1.0, 2.0);
    feat->SetGeometryDirectly(pt.release());
    EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  }
  return ds;
}

TEST_F(GdalalgVectorPartitionTest, PatternAndMaxFileSizeValidation) {
  auto mem_ds = CreateRichMemDataset(/*with_lists=*/false);
  ASSERT_THAT(mem_ds, NotNull());

  struct PatternErr {
    const char* pattern;
    const char* msg;
  };
  constexpr PatternErr kPatternErrors[] = {
      {.pattern = "part_no_percent", .msg = "Missing '%' character"},
      {.pattern = "part_%d_%d", .msg = "A single '%' character"},
      {.pattern = "part_%s", .msg = "part number specification"},
      {.pattern = "part_%11d", .msg = "should be in [1,10] range"},
  };
  for (const auto& tc : kPatternErrors) {
    GDALVectorPartitionAlgorithm alg;
    alg.GetInputDatasets().emplace_back(mem_ds.get());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--field", "part_str", "--pattern", tc.pattern, "-o",
         std::string(kTestDir) + "/out"}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr(tc.msg));
  }

  // Invalid max-file-size and < 1MB max-file-size.
  {
    GDALVectorPartitionAlgorithm alg;
    alg.GetInputDatasets().emplace_back(mem_ds.get());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--field", "part_str", "--max-file-size", "notanumber", "-o",
         std::string(kTestDir) + "/out"}));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Invalid value for max-file-size"));
  }
  {
    GDALVectorPartitionAlgorithm alg;
    alg.GetInputDatasets().emplace_back(mem_ds.get());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--field", "part_str", "--max-file-size", "512KB", "-o",
         std::string(kTestDir) + "/out"}));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("max-file-size should be at least one MB"));
  }
}

TEST_F(GdalalgVectorPartitionTest, DriverAndFieldErrors) {
  auto mem_ds = CreateRichMemDataset(/*with_lists=*/false);
  ASSERT_THAT(mem_ds, NotNull());

  // 1. MEM source without --output-format cannot infer extension.
  {
    GDALVectorPartitionAlgorithm alg;
    alg.GetInputDatasets().emplace_back(mem_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--field", "part_str", "-o", std::string(kTestDir) + "/err1"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot infer output format"));
  }

  // 2. Explicit --output-format MEM has no file extension.
  {
    GDALVectorPartitionAlgorithm alg;
    alg.GetInputDatasets().emplace_back(mem_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--field", "part_str", "-o", std::string(kTestDir) + "/err2"}));
    GDALAlgorithmArg* fmt_arg = alg.GetArg(GDAL_ARG_NAME_OUTPUT_FORMAT);
    ASSERT_THAT(fmt_arg, NotNull());
    fmt_arg->Set("MEM");
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("no known file extension"));
  }

  // 3. GML does not support --append.
  {
    GDALVectorPartitionAlgorithm alg;
    alg.GetInputDatasets().emplace_back(mem_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--field", "part_str", "-f", "GML", "--append", "-o",
         std::string(kTestDir) + "/err3"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("does not support update"));
  }

  // 4. Missing field name.
  {
    GDALVectorPartitionAlgorithm alg;
    alg.GetInputDatasets().emplace_back(mem_ds.get());
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--field", "missing_field", "-f", "GPKG",
                                       "-o", std::string(kTestDir) + "/err4"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot find field"));
  }

  // 5. Unsupported partitioning field type (OFTReal).
  {
    GDALVectorPartitionAlgorithm alg;
    alg.GetInputDatasets().emplace_back(mem_ds.get());
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--field", "dbl_val", "-f", "GPKG", "-o",
                                       std::string(kTestDir) + "/err5"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("not valid for partitioning"));
  }
}

TEST_F(GdalalgVectorPartitionTest, HiveGeoJsonMultiPassAndAttributeFilter) {
  auto mem_ds = CreateRichMemDataset(/*with_lists=*/true);
  ASSERT_THAT(mem_ds, NotNull());
  const std::string out_dir = std::string(kTestDir) + "/gml_hive";

  // Single-field GML (does not support append) with max-cache-size=1
  // (triggers multi-pass), max-file-size=1MB (triggers GetEstimatedFeatureSize
  // on all field types), and custom pattern with suffix.
  mem_ds->GetLayer(0)->SetAttributeFilter("part_int >= 1");
  {
    GDALVectorPartitionAlgorithm alg;
    alg.GetInputDatasets().emplace_back(mem_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--field", "part_str", "-f", "GML", "--scheme", "hive", "--pattern",
         "p_%03d_end", "--max-cache-size", "1", "--max-file-size", "1MB",
         "--omit-partitioned-field", "-o", out_dir}));
    EXPECT_TRUE(alg.Run());
  }

  // Verify default hive partition for NULL and percent-encoded key.
  VSIStatBufL st;
  EXPECT_EQ(0, VSIStatL((out_dir + "/items/part_str=__HIVE_DEFAULT_PARTITION__/"
                                   "p_001_end.gml")
                            .c_str(),
                        &st));
  EXPECT_EQ(
      0,
      VSIStatL((out_dir + "/items/part_str=alpha%2Fbeta/p_001_end.gml").c_str(),
               &st));

  // Multi-field GeoJSON partition with flat scheme and custom pattern.
  const std::string flat_dir = std::string(kTestDir) + "/geojson_flat";
  mem_ds->GetLayer(0)->SetAttributeFilter(nullptr);
  {
    GDALVectorPartitionAlgorithm alg;
    alg.GetInputDatasets().emplace_back(mem_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--field", "part_str,part_int", "-f", "GeoJSON", "--scheme", "flat",
         "--pattern", "{LAYER_NAME}_{FIELD_VALUE}_%2d", "--max-cache-size",
         "10", "-o", flat_dir}));
    EXPECT_TRUE(alg.Run());
  }
  EXPECT_EQ(0, VSIStatL((flat_dir + "/items___NULL___3_ 1.json").c_str(), &st));
}

TEST_F(GdalalgVectorPartitionTest, GpkgAppendOverwriteSafetyAndFeatureLimit) {
  // GeoPackage stores OFTTime as TEXT (OFTString), so omit time_val when
  // testing --append schema matching against an existing GeoPackage partition.
  auto mem_ds = CreateRichMemDataset(/*with_lists=*/false, /*with_time=*/false);
  ASSERT_THAT(mem_ds, NotNull());
  const std::string out_dir = std::string(kTestDir) + "/gpkg_hive";

  // 1. Initial partition by part_int64 with feature-limit=1 and progress.
  {
    GDALVectorPartitionAlgorithm alg;
    alg.GetInputDatasets().emplace_back(mem_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--field", "part_int64", "-f", "GPKG", "--feature-limit", "1",
         "--transaction-size", "1", "-o", out_dir}));
    bool progress_called = false;
    auto progress_cb = [](double, const char*, void* user_data) -> int {
      *static_cast<bool*>(user_data) = true;
      return 1;
    };
    EXPECT_TRUE(alg.Run(progress_cb, &progress_called));
    EXPECT_TRUE(progress_called);
  }

  VSIStatBufL st;
  const std::string part2 =
      out_dir + "/items/part_int64=10000000000/part_0000000002.gpkg";
  EXPECT_EQ(0, VSIStatL(part2.c_str(), &st));

  // 2. Existing directory without --overwrite or --append fails.
  {
    GDALVectorPartitionAlgorithm alg;
    alg.GetInputDatasets().emplace_back(mem_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--field", "part_int64", "-f", "GPKG", "-o", out_dir}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("already exists"));
  }

  // 3. Append with feature-limit=2 opens existing part_0000000002 (count=1 <
  // 2), adds 1 feature, and rolls over when needed.
  {
    GDALVectorPartitionAlgorithm alg;
    alg.GetInputDatasets().emplace_back(mem_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--field", "part_int64", "-f", "GPKG", "--append", "--feature-limit",
         "2", "-o", out_dir}));
    EXPECT_TRUE(alg.Run());
  }

  // 4. Append with mismatched feature definition fails.
  {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_THAT(drv, NotNull());
    std::unique_ptr<GDALDataset> diff_ds(
        drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* lyr = diff_ds->CreateLayer("items", nullptr, wkbPoint, nullptr);
    OGRFieldDefn fd("part_int64", OFTInteger64);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&fd));
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    feat->SetField("part_int64", static_cast<GIntBig>(10000000000LL));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));

    GDALVectorPartitionAlgorithm alg;
    alg.GetInputDatasets().emplace_back(diff_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--field", "part_int64", "-f", "GPKG", "--append", "-o", out_dir}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("does not have the same feature definition"));
  }

  // 5. --overwrite on valid hive directory succeeds.
  {
    GDALVectorPartitionAlgorithm alg;
    alg.GetInputDatasets().emplace_back(mem_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--field", "part_int64", "-f", "GPKG", "--overwrite", "-o", out_dir}));
    EXPECT_TRUE(alg.Run());
  }

  // 6. --overwrite safety check rejects non-partition directory (hive & flat).
  const std::string bad_hive = std::string(kTestDir) + "/bad_hive";
  ASSERT_EQ(0, VSIMkdir(bad_hive.c_str(), 0755));
  ASSERT_EQ(0, VSIMkdir((bad_hive + "/not_a_layer").c_str(), 0755));
  ASSERT_EQ(0, VSIMkdir((bad_hive + "/not_a_layer/no_equals").c_str(), 0755));
  {
    GDALVectorPartitionAlgorithm alg;
    alg.GetInputDatasets().emplace_back(mem_ds.get());
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--field", "part_int64", "-f", "GPKG",
                                       "--overwrite", "-o", bad_hive}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Rejecting removing"));
  }

  const std::string bad_flat = std::string(kTestDir) + "/bad_flat";
  ASSERT_EQ(0, VSIMkdir(bad_flat.c_str(), 0755));
  ASSERT_EQ(0, VSIMkdir((bad_flat + "/subdir").c_str(), 0755));
  {
    GDALVectorPartitionAlgorithm alg;
    alg.GetInputDatasets().emplace_back(mem_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--field", "part_int64", "-f",
                                               "GPKG", "--scheme", "flat",
                                               "--overwrite", "-o", bad_flat}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Rejecting removing"));
  }
}

TEST_F(GdalalgVectorPartitionTest,
       MaxFileSizeRolloverAndSkipErrorsAndInterrupt) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_THAT(drv, NotNull());

  // 1. Trigger max-file-size rollover on GPKG (binary format + spatial index).
  {
    std::unique_ptr<GDALDataset> big_ds(
        drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* lyr = big_ds->CreateLayer("big", nullptr, wkbPoint, nullptr);
    OGRFieldDefn fd1("k", OFTInteger);
    OGRFieldDefn fd2("payload", OFTString);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&fd1));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&fd2));
    const std::string payload(2048, 'x');
    for (int i = 0; i < 600; ++i) {
      auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
      feat->SetField("k", 1);
      feat->SetField("payload", payload.c_str());
      auto pt = std::make_unique<OGRPoint>(1.0, 2.0);
      feat->SetGeometryDirectly(pt.release());
      ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
    }

    const std::string out_dir = std::string(kTestDir) + "/gpkg_maxsize";
    GDALVectorPartitionAlgorithm alg;
    alg.GetInputDatasets().emplace_back(big_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--field", "k", "-f", "GPKG",
                                               "--max-file-size", "1MB", "-o",
                                               out_dir}));
    EXPECT_TRUE(alg.Run());
    VSIStatBufL st;
    EXPECT_EQ(
        0, VSIStatL((out_dir + "/big/k=1/part_0000000002.gpkg").c_str(), &st));
  }

  // 2. Incompatible geometry triggers CreateFeature failure; test fail vs
  // --skip-errors (with ESRI Shapefile, which rejects OGRPoint in a polygon
  // shapefile without SQLite transaction cleanup conflicts), and progress
  // interruption on GPKG.
  {
    std::unique_ptr<GDALDataset> bad_ds(
        drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* lyr = bad_ds->CreateLayer("poly", nullptr, wkbPolygon, nullptr);
    OGRFieldDefn fd("k", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&fd));
    {
      auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
      feat->SetField("k", 7);
      auto poly = std::make_unique<OGRPolygon>();
      auto ring = std::make_unique<OGRLinearRing>();
      ring->addPoint(0.0, 0.0);
      ring->addPoint(1.0, 0.0);
      ring->addPoint(1.0, 1.0);
      ring->addPoint(0.0, 1.0);
      ring->addPoint(0.0, 0.0);
      poly->addRingDirectly(ring.release());
      feat->SetGeometryDirectly(poly.release());
      ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
    }
    {
      auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
      feat->SetField("k", 7);
      auto pt = std::make_unique<OGRPoint>(1.0, 2.0);
      feat->SetGeometryDirectly(pt.release());
      ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
    }

    {
      GDALVectorPartitionAlgorithm alg;
      alg.GetInputDatasets().emplace_back(bad_ds.get());
      ASSERT_TRUE(alg.ParseCommandLineArguments(
          {"--field", "k", "-f", "ESRI Shapefile", "-o",
           std::string(kTestDir) + "/bad_fail"}));
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_FALSE(alg.Run());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot insert feature"));
    }
    {
      GDALVectorPartitionAlgorithm alg;
      alg.GetInputDatasets().emplace_back(bad_ds.get());
      ASSERT_TRUE(alg.ParseCommandLineArguments(
          {"--field", "k", "-f", "ESRI Shapefile", "--skip-errors", "-o",
           std::string(kTestDir) + "/bad_skip"}));
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_TRUE(alg.Run());
    }
    {
      std::unique_ptr<GDALDataset> valid_ds(
          drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
      OGRLayer* vlyr = valid_ds->CreateLayer("pts", nullptr, wkbPoint, nullptr);
      ASSERT_EQ(OGRERR_NONE, vlyr->CreateField(&fd));
      for (int i = 0; i < 2; ++i) {
        auto feat = std::make_unique<OGRFeature>(vlyr->GetLayerDefn());
        feat->SetField("k", 7);
        ASSERT_EQ(OGRERR_NONE, vlyr->CreateFeature(feat.get()));
      }
      GDALVectorPartitionAlgorithm alg;
      alg.GetInputDatasets().emplace_back(valid_ds.get());
      ASSERT_TRUE(alg.ParseCommandLineArguments(
          {"--field", "k", "-f", "GPKG", "-o",
           std::string(kTestDir) + "/interrupt"}));
      auto cancel_cb = [](double, const char*, void*) -> int { return 0; };
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_FALSE(alg.Run(cancel_cb, nullptr));
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Interrupted by user"));
    }
  }
}

TEST_F(GdalalgVectorPartitionTest, StandaloneAndPipelineWithMultiGeom) {
  const std::string in_gpkg = std::string(kTestDir) + "/in_multi.gpkg";
  {
    GDALDriver* gpkg_drv = GetGDALDriverManager()->GetDriverByName("GPKG");
    ASSERT_THAT(gpkg_drv, NotNull());
    std::unique_ptr<GDALDataset> ds(
        gpkg_drv->Create(in_gpkg.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_THAT(ds, NotNull());
    OGRLayer* lyr = ds->CreateLayer("pts", nullptr, wkbPoint, nullptr);
    ASSERT_THAT(lyr, NotNull());
    OGRFieldDefn fd("grp", OFTString);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&fd));
    for (int i = 0; i < 3; ++i) {
      auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
      feat->SetField("grp", i == 0 ? "A" : "B");
      auto pt = std::make_unique<OGRPoint>(1.0, 2.0);
      feat->SetGeometryDirectly(pt.release());
      ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
    }
  }

  const std::string standalone_out = std::string(kTestDir) + "/standalone_out";
  {
    GDALVectorPartitionAlgorithmStandalone alg;
    EXPECT_EQ("partition", alg.GetName());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--field", "grp", in_gpkg, standalone_out}));
    EXPECT_TRUE(alg.Run());
  }

  const std::string pipe_out = std::string(kTestDir) + "/pipe_out";
  GDALVectorPipelineAlgorithm pipeline;
  ASSERT_TRUE(pipeline.ParseCommandLineArguments(
      {"read", in_gpkg, "!", "filter", "--where", "grp = 'B'", "!", "partition",
       "--field", "grp", "-f", "GeoJSON", "-o", pipe_out}));
  EXPECT_TRUE(pipeline.Run());
  EXPECT_TRUE(pipeline.Finalize());
  VSIStatBufL st;
  EXPECT_EQ(
      0, VSIStatL((pipe_out + "/pts/grp=B/part_0000000001.json").c_str(), &st));
  EXPECT_NE(
      0, VSIStatL((pipe_out + "/pts/grp=A/part_0000000001.json").c_str(), &st));
}

}  // namespace
}  // namespace autotest2
