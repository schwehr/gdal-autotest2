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
// Unit tests for ogrparquetlayer.cpp (OGRParquetLayer and OGRParquetLayerBase).
//
// Build caveat in google3: GDAL_USE_ARROWDATASET is not defined in
// //third_party/gdal/BUILD, so OGRParquetDatasetLayer paths are compiled out.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/parquet/ogrparquetlayer.cpp
//   https://gdal.org/drivers/vector/parquet.html

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "arrow/api.h"
#include "arrow/array/array_dict.h"
#include "arrow/io/memory.h"
#include "arrow/util/key_value_metadata.h"
#include "parquet/column_writer.h"
#include "parquet/file_writer.h"
#include "parquet/schema.h"
#include "parquet/types.h"
#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/ogr/ogrsf_frmts/parquet/parquet_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory_closer.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_recordbatch.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/ogr/ogrsf_frmts/parquet/ogr_parquet.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/ogrparquetlayer_test";

class OgrParquetLayerTest : public ::testing::Test {
 protected:
  void SetUp() override { RegisterOGRParquet(); }
};

std::shared_ptr<arrow::Table> MakeMultiTypeTable(
    const std::shared_ptr<const arrow::KeyValueMetadata>& kv = nullptr) {
  arrow::BooleanBuilder b_bool;
  arrow::Int32Builder b_i32;
  arrow::UInt32Builder b_u32;
  arrow::Int16Builder b_i16;
  arrow::Int64Builder b_i64;
  arrow::FloatBuilder b_f32;
  arrow::DoubleBuilder b_f64;
  arrow::StringBuilder b_str;
  arrow::TimestampBuilder b_ts(arrow::timestamp(arrow::TimeUnit::MILLI, "UTC"),
                               arrow::default_memory_pool());
  arrow::BinaryBuilder b_wkb;

  auto sb_a = std::make_shared<arrow::Int32Builder>();
  auto sb_b = std::make_shared<arrow::DoubleBuilder>();
  auto st_type = arrow::struct_(
      {arrow::field("a", arrow::int32()), arrow::field("b", arrow::float64())});
  arrow::StructBuilder b_struct(st_type, arrow::default_memory_pool(),
                                {sb_a, sb_b});

  arrow::MapBuilder b_map(arrow::default_memory_pool(),
                          std::make_shared<arrow::StringBuilder>(),
                          std::make_shared<arrow::Int32Builder>());

  arrow::StringDictionaryBuilder b_dict(arrow::default_memory_pool());

  for (int i = 0; i < 6; ++i) {
    EXPECT_TRUE(b_bool.Append(i % 2 == 0).ok());
    if (i == 4) {
      EXPECT_TRUE(b_i32.AppendNull().ok());
      EXPECT_TRUE(b_str.AppendNull().ok());
    } else {
      EXPECT_TRUE(b_i32.Append(i * 10 - 20).ok());
      EXPECT_TRUE(b_str.Append("val_" + std::to_string(i)).ok());
    }
    EXPECT_TRUE(b_u32.Append(static_cast<uint32_t>(3000000000U + i)).ok());
    EXPECT_TRUE(b_i16.Append(static_cast<int16_t>(-100 + i * 50)).ok());
    EXPECT_TRUE(b_i64.Append(100000000000LL + i).ok());
    EXPECT_TRUE(b_f32.Append(1.5f + i).ok());
    EXPECT_TRUE(b_f64.Append(2.5 + i).ok());
    EXPECT_TRUE(b_ts.Append(1700000000000LL + i * 1000LL).ok());
    EXPECT_TRUE(b_wkb.Append(MakePointWkb(i * 10.0, i * 10.0)).ok());

    EXPECT_TRUE(b_struct.Append().ok());
    EXPECT_TRUE(sb_a->Append(i).ok());
    EXPECT_TRUE(sb_b->Append(i * 0.5).ok());

    EXPECT_TRUE(b_map.Append().ok());
    EXPECT_TRUE(static_cast<arrow::StringBuilder*>(b_map.key_builder())
                    ->Append("k")
                    .ok());
    EXPECT_TRUE(static_cast<arrow::Int32Builder*>(b_map.item_builder())
                    ->Append(i)
                    .ok());

    EXPECT_TRUE(b_dict.Append(i % 2 == 0 ? "even" : "odd").ok());
  }

  std::shared_ptr<arrow::Array> c_bool, c_i32, c_u32, c_i16, c_i64, c_f32,
      c_f64, c_str, c_ts, c_wkb, c_struct, c_map, c_dict;
  EXPECT_TRUE(b_bool.Finish(&c_bool).ok());
  EXPECT_TRUE(b_i32.Finish(&c_i32).ok());
  EXPECT_TRUE(b_u32.Finish(&c_u32).ok());
  EXPECT_TRUE(b_i16.Finish(&c_i16).ok());
  EXPECT_TRUE(b_i64.Finish(&c_i64).ok());
  EXPECT_TRUE(b_f32.Finish(&c_f32).ok());
  EXPECT_TRUE(b_f64.Finish(&c_f64).ok());
  EXPECT_TRUE(b_str.Finish(&c_str).ok());
  EXPECT_TRUE(b_ts.Finish(&c_ts).ok());
  EXPECT_TRUE(b_wkb.Finish(&c_wkb).ok());
  EXPECT_TRUE(b_struct.Finish(&c_struct).ok());
  EXPECT_TRUE(b_map.Finish(&c_map).ok());
  EXPECT_TRUE(b_dict.Finish(&c_dict).ok());

  auto schema = arrow::schema(
      {arrow::field("b", arrow::boolean()), arrow::field("i32", arrow::int32()),
       arrow::field("u32", arrow::uint32()),
       arrow::field("i16", arrow::int16()), arrow::field("i64", arrow::int64()),
       arrow::field("f32", arrow::float32()),
       arrow::field("f64", arrow::float64()), arrow::field("s", arrow::utf8()),
       arrow::field("ts", arrow::timestamp(arrow::TimeUnit::MILLI, "UTC")),
       arrow::field("st", st_type),
       arrow::field("mp", arrow::map(arrow::utf8(), arrow::int32())),
       arrow::field("dict", arrow::dictionary(arrow::int8(), arrow::utf8())),
       arrow::field("geom", arrow::binary())},
      kv);

  return arrow::Table::Make(
      schema, {c_bool, c_i32, c_u32, c_i16, c_i64, c_f32, c_f64, c_str, c_ts,
               c_struct, c_map, c_dict, c_wkb});
}

TEST_F(OgrParquetLayerTest, GeoMetadataParsingAndCrsOptions) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  // 1. Invalid JSON 'geo' emits warning.
  {
    constexpr char kPath[] = "/vsimem/ogrparquetlayer_test/bad_geo.parquet";
    auto kv = std::make_shared<arrow::KeyValueMetadata>();
    kv->Append("geo", "{not valid json");
    ASSERT_TRUE(WriteArrowTableToVsiMem(kPath, MakeMultiTypeTable(kv)));

    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = OpenParquetDataset(kPath);
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot parse 'geo' metadata"));
    CPLErrorReset();
  }

  // 2. Old GeoParquet 0.1.0 with string WKT CRS + unknown version +
  // computed WKB geometry type + edges=spherical.
  {
    constexpr char kPath[] = "/vsimem/ogrparquetlayer_test/v010.parquet";
    OGRSpatialReference srs;
    srs.importFromEPSG(4326);
    char* wkt_raw = nullptr;
    ASSERT_EQ(OGRERR_NONE, srs.exportToWkt(&wkt_raw));
    CPLMemoryCloser<char> wkt_closer(wkt_raw);
    const std::string wkt(wkt_closer.get());

    CPLJSONObject root;
    root.Add("version", "0.0.1");
    root.Add("primary_column", "geom");
    CPLJSONObject cols;
    CPLJSONObject col;
    col.Add("encoding", "WKB");
    col.Add("crs", wkt);
    col.Add("edges", "spherical");
    cols.Add("geom", col);
    root.Add("columns", cols);

    auto kv = std::make_shared<arrow::KeyValueMetadata>();
    kv->Append("geo", root.Format(CPLJSONObject::PrettyFormat::Plain));
    ASSERT_TRUE(WriteArrowTableToVsiMem(kPath, MakeMultiTypeTable(kv)));

    auto ds = OpenParquetDataset(kPath);
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(wkbPoint, lyr->GetGeomType());
    EXPECT_STREQ("SPHERICAL", lyr->GetMetadataItem("EDGES"));
    ASSERT_NE(nullptr, lyr->GetSpatialRef());
    EXPECT_STREQ("4326", lyr->GetSpatialRef()->GetAuthorityCode(nullptr));
  }

  // 3. Multi-geometry_types array promotion (LineString + MultiLineString ->
  // MultiLineString), invalid WKT CRS fallback, and CRS=... open option.
  {
    constexpr char kPath[] = "/vsimem/ogrparquetlayer_test/geom_types.parquet";
    CPLJSONObject root;
    root.Add("version", "1.1.0");
    root.Add("primary_column", "geom");
    CPLJSONObject cols;
    CPLJSONObject col;
    col.Add("encoding", "WKB");
    col.Add("crs", "INVALID_WKT_CRS");
    CPLJSONArray gtypes;
    gtypes.Add("LineString Z");
    gtypes.Add("MultiLineString");
    col.Add("geometry_types", gtypes);
    cols.Add("geom", col);
    root.Add("columns", cols);

    auto kv = std::make_shared<arrow::KeyValueMetadata>();
    kv->Append("geo", root.Format(CPLJSONObject::PrettyFormat::Plain));
    ASSERT_TRUE(WriteArrowTableToVsiMem(kPath, MakeMultiTypeTable(kv)));

    CPLStringList open_opts;
    open_opts.SetNameValue("CRS", "EPSG:32631");
    auto ds = OpenParquetDataset(kPath, GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                 open_opts.List());
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(wkbMultiLineString25D, lyr->GetGeomType());
    ASSERT_NE(nullptr, lyr->GetSpatialRef());
    EXPECT_STREQ("32631", lyr->GetSpatialRef()->GetAuthorityCode(nullptr));
  }

  // 4. Autodetect WKB and WKT columns from GEOM_POSSIBLE_NAMES without 'geo'
  // metadata, with CRS open option.
  {
    constexpr char kPath[] =
        "/vsimem/ogrparquetlayer_test/heuristic_wkt.parquet";
    arrow::StringBuilder wkt_builder;
    ASSERT_TRUE(wkt_builder.AppendValues({"POINT (1 2)", "POINT (3 4)"}).ok());
    std::shared_ptr<arrow::Array> wkt_arr;
    ASSERT_TRUE(wkt_builder.Finish(&wkt_arr).ok());
    auto table = arrow::Table::Make(
        arrow::schema({arrow::field("my_wkt", arrow::utf8())}), {wkt_arr});
    ASSERT_TRUE(WriteArrowTableToVsiMem(kPath, table));

    CPLStringList open_opts;
    open_opts.SetNameValue("GEOM_POSSIBLE_NAMES", "my_wkt");
    open_opts.SetNameValue("CRS", "EPSG:4326");
    auto ds = OpenParquetDataset(kPath, GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                 open_opts.List());
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_STREQ("my_wkt", lyr->GetGeometryColumn());
    EXPECT_EQ(wkbUnknown, lyr->GetGeomType());
    std::unique_ptr<OGRFeature> feat(lyr->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    ASSERT_NE(nullptr, feat->GetGeometryRef());
  }
}

TEST_F(OgrParquetLayerTest, FilteringRandomAccessStatisticsAndMetadata) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };
  constexpr char kPath[] = "/vsimem/ogrparquetlayer_test/multi_rg.parquet";
  // chunk_size=2 produces 3 row groups across 6 rows.
  ASSERT_TRUE(WriteArrowTableToVsiMem(kPath, MakeMultiTypeTable(),
                                      /*chunk_size=*/2));

  auto ds = OpenParquetDataset(kPath);
  ASSERT_NE(nullptr, ds);
  OGRLayer* lyr = ds->GetLayer(0);
  ASSERT_NE(nullptr, lyr);

  // 1. Domains on dictionary field.
  const auto domains = ds->GetFieldDomainNames();
  EXPECT_FALSE(domains.empty());
  const OGRFieldDomain* dom = ds->GetFieldDomain("dictDomain");
  ASSERT_NE(nullptr, dom);

  // 2. Capabilities & Metadata queries (_PARQUET_, _PARQUET_METADATA_,
  // _GDAL_CREATION_OPTIONS_).
  EXPECT_EQ(TRUE, lyr->TestCapability(OLCFastFeatureCount));
  EXPECT_EQ(TRUE, lyr->TestCapability(OLCIgnoreFields));
  EXPECT_EQ(TRUE, lyr->TestCapability(OLCFastSetNextByIndex));
  EXPECT_STREQ("3", lyr->GetMetadataItem("NUM_ROW_GROUPS", "_PARQUET_"));
  EXPECT_NE(nullptr, lyr->GetMetadataItem("CREATOR", "_PARQUET_"));
  EXPECT_STREQ("2",
               lyr->GetMetadataItem("ROW_GROUPS[0].NUM_ROWS", "_PARQUET_"));
  EXPECT_EQ(nullptr,
            lyr->GetMetadataItem("ROW_GROUPS[99].NUM_ROWS", "_PARQUET_"));
  EXPECT_NE(nullptr, lyr->GetMetadataItem(
                         "ROW_GROUPS[0].COLUMNS[0].COMPRESSION", "_PARQUET_"));
  EXPECT_EQ(nullptr, lyr->GetMetadataItem(
                         "ROW_GROUPS[99].COLUMNS[0].COMPRESSION", "_PARQUET_"));
  EXPECT_EQ(nullptr, lyr->GetMetadataItem("UNKNOWN", "_PARQUET_"));
  EXPECT_NE(nullptr,
            lyr->GetMetadataItem("ARROW:schema", "_PARQUET_METADATA_"));
  EXPECT_EQ(nullptr, lyr->GetMetadataItem("NO_SUCH_KEY", "_PARQUET_METADATA_"));
  CSLConstList pm = lyr->GetMetadata("_PARQUET_METADATA_");
  EXPECT_GT(CSLCount(pm), 0);
  CSLConstList gco = lyr->GetMetadata("_GDAL_CREATION_OPTIONS_");
  EXPECT_EQ(0, CSLCount(gco));

  // 3. GetFeature random access across row groups & bounds.
  EXPECT_EQ(nullptr, lyr->GetFeature(-1));
  EXPECT_EQ(nullptr, lyr->GetFeature(100));
  {
    std::unique_ptr<OGRFeature> f3(lyr->GetFeature(3));
    ASSERT_NE(nullptr, f3);
    EXPECT_EQ(3, f3->GetFID());
    EXPECT_EQ(10, f3->GetFieldAsInteger("i32"));
  }

  // 4. SetNextByIndex out-of-range and valid positions (both multi-batch and
  // single-batch).
  EXPECT_EQ(OGRERR_NON_EXISTING_FEATURE, lyr->SetNextByIndex(-1));
  EXPECT_EQ(OGRERR_NON_EXISTING_FEATURE, lyr->SetNextByIndex(99));
  EXPECT_EQ(OGRERR_NONE, lyr->SetNextByIndex(3));
  {
    std::unique_ptr<OGRFeature> f(lyr->GetNextFeature());
    ASSERT_NE(nullptr, f);
    EXPECT_EQ(3, f->GetFID());
  }

  // 5. Attribute filter operators hitting every IsConstraintPossible branch +
  // FID pruning + IS NULL / IS NOT NULL + row-group remapping.
  struct FilterCase {
    const char* expr;
    GIntBig expected_count;
  };
  constexpr FilterCase kFilterCases[] = {
      {.expr = "b = 1", .expected_count = 3},
      {.expr = "i32 = 30", .expected_count = 1},
      {.expr = "i32 != 30", .expected_count = 4},
      {.expr = "i32 < -15", .expected_count = 1},
      {.expr = "i32 <= -20", .expected_count = 1},
      {.expr = "i32 > 25", .expected_count = 1},
      {.expr = "i32 >= 30", .expected_count = 1},
      {.expr = "i32 IS NULL", .expected_count = 1},
      {.expr = "i32 IS NOT NULL", .expected_count = 5},
      {.expr = "u32 = 3000000005", .expected_count = 1},
      {.expr = "i16 = -100", .expected_count = 1},
      {.expr = "i64 = 100000000005", .expected_count = 1},
      {.expr = "f32 = 6.5", .expected_count = 1},
      {.expr = "f64 > 5.0", .expected_count = 3},
      {.expr = "s = 'val_5'", .expected_count = 1},
      {.expr = "FID = 4", .expected_count = 1},
      {.expr = "ts >= '2023/11/14 22:13:20+00'", .expected_count = 6},
  };
  for (const auto& tc : kFilterCases) {
    ASSERT_EQ(OGRERR_NONE, lyr->SetAttributeFilter(tc.expr)) << tc.expr;
    EXPECT_EQ(tc.expected_count, lyr->GetFeatureCount(/*bForce=*/1)) << tc.expr;
  }
  ASSERT_EQ(OGRERR_NONE, lyr->SetAttributeFilter(nullptr));

  // 6. Ignored fields covering struct members, map, geometry, and restoration.
  const char* ignore_some[] = {"st.a", "mp", nullptr};
  ASSERT_EQ(OGRERR_NONE, lyr->SetIgnoredFields(ignore_some));
  lyr->ResetReading();
  {
    std::unique_ptr<OGRFeature> f0(lyr->GetNextFeature());
    ASSERT_NE(nullptr, f0);
    EXPECT_FALSE(
        f0->IsFieldSetAndNotNull(lyr->GetLayerDefn()->GetFieldIndex("st.a")));
    EXPECT_FALSE(
        f0->IsFieldSetAndNotNull(lyr->GetLayerDefn()->GetFieldIndex("mp")));
    EXPECT_TRUE(
        f0->IsFieldSetAndNotNull(lyr->GetLayerDefn()->GetFieldIndex("st.b")));
  }
  ASSERT_EQ(OGRERR_NONE, lyr->SetIgnoredFields(nullptr));

  // 7. GetArrowStream with explicit MAX_FEATURES_IN_BATCH.
  {
    struct ArrowArrayStream stream{};
    absl::Cleanup rel_stream = [&stream] {
      if (stream.release) stream.release(&stream);
    };
    CPLStringList opts;
    opts.SetNameValue("MAX_FEATURES_IN_BATCH", "2");
    ASSERT_TRUE(lyr->GetArrowStream(&stream, opts.List()));
    struct ArrowArray batch{};
    absl::Cleanup rel_batch = [&batch] {
      if (batch.release) batch.release(&batch);
    };
    ASSERT_EQ(0, stream.get_next(&stream, &batch));
    EXPECT_EQ(2, batch.length);
  }
}

TEST_F(OgrParquetLayerTest, NativeParquetGeoTypesAndOvertureCovering) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  // 1. Write a Parquet GEOMETRY file using USE_PARQUET_GEO_TYPES=ONLY with
  // multiple row groups to cover m_geoStatsWithBBOXAvailable, FastGetExtent,
  // FastSpatialFilter geo_statistics pruning, and OLCFastGetExtent3D.
  constexpr char kGeoStatsPath[] =
      "/vsimem/ogrparquetlayer_test/geostats.parquet";
  {
    VSIMkdirRecursive(kTestDir, 0755);
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("Parquet");
    ASSERT_NE(nullptr, drv);
    GDALDatasetUniquePtr wr(
        drv->Create(kGeoStatsPath, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, wr);
    CPLStringList opts;
    opts.SetNameValue("USE_PARQUET_GEO_TYPES", "ONLY");
    opts.SetNameValue("ROW_GROUP_SIZE", "2");
    OGRSpatialReference srs;
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    srs.importFromEPSG(4326);
    OGRLayer* lyr = wr->CreateLayer("gs", &srs, wkbPoint25D, opts.List());
    ASSERT_NE(nullptr, lyr);
    for (int i = 0; i < 4; ++i) {
      OGRFeature f(lyr->GetLayerDefn());
      auto pt = std::make_unique<OGRPoint>(i * 10.0, i * 10.0, i * 5.0);
      f.SetGeometryDirectly(pt.release());
      ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(&f));
    }
    EXPECT_EQ(CE_None, wr->Close());
  }
  {
    auto ds = OpenParquetDataset(kGeoStatsPath);
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(TRUE, lyr->TestCapability(OLCFastSpatialFilter));
    EXPECT_EQ(TRUE, lyr->TestCapability(OLCFastGetExtent));
    EXPECT_EQ(TRUE, lyr->TestCapability(OLCFastGetExtent3D));
    OGREnvelope3D ext3d;
    EXPECT_EQ(OGRERR_NONE, lyr->GetExtent3D(0, &ext3d, /*bForce=*/FALSE));
    EXPECT_DOUBLE_EQ(0.0, ext3d.MinZ);
    EXPECT_DOUBLE_EQ(15.0, ext3d.MaxZ);

    // Filter matching only row group 1.
    lyr->SetSpatialFilterRect(20.0, 20.0, 30.0, 30.0);
    EXPECT_EQ(2, lyr->GetFeatureCount(/*bForce=*/1));
    // Non-intersecting filter prunes all row groups.
    lyr->SetSpatialFilterRect(500.0, 500.0, 600.0, 600.0);
    EXPECT_EQ(nullptr, lyr->GetNextFeature());
  }

  // 2. Overture-style implicit bbox covering detection (bbox.minx..maxy and
  // bbox.xmin..ymax without 'covering' metadata).
  for (bool alpha_naming : {true, false}) {
    const std::string path =
        std::string(kTestDir) +
        (alpha_naming ? "/ov_alpha.parquet" : "/ov_beta.parquet");
    auto bbox_type = arrow::struct_({
        arrow::field(alpha_naming ? "minx" : "xmin", arrow::float32()),
        arrow::field(alpha_naming ? "miny" : "ymin", arrow::float32()),
        arrow::field(alpha_naming ? "maxx" : "xmax", arrow::float32()),
        arrow::field(alpha_naming ? "maxy" : "ymax", arrow::float32()),
    });
    auto x0 = std::make_shared<arrow::FloatBuilder>();
    auto y0 = std::make_shared<arrow::FloatBuilder>();
    auto x1 = std::make_shared<arrow::FloatBuilder>();
    auto y1 = std::make_shared<arrow::FloatBuilder>();
    arrow::StructBuilder sb(bbox_type, arrow::default_memory_pool(),
                            {x0, y0, x1, y1});
    ASSERT_TRUE(sb.Append().ok());
    ASSERT_TRUE(x0->Append(1.0f).ok());
    ASSERT_TRUE(y0->Append(2.0f).ok());
    ASSERT_TRUE(x1->Append(3.0f).ok());
    ASSERT_TRUE(y1->Append(4.0f).ok());
    arrow::BinaryBuilder wkb_b;
    ASSERT_TRUE(wkb_b.Append(MakePointWkb(2.0, 3.0)).ok());
    std::shared_ptr<arrow::Array> bbox_arr, geom_arr;
    ASSERT_TRUE(sb.Finish(&bbox_arr).ok());
    ASSERT_TRUE(wkb_b.Finish(&geom_arr).ok());

    auto kv = std::make_shared<arrow::KeyValueMetadata>();
    kv->Append("geo",
               "{\"version\":\"1.0.0\",\"primary_column\":\"geometry\","
               "\"columns\":{\"geometry\":{\"encoding\":\"WKB\","
               "\"geometry_types\":[\"Point\"]}}}");
    auto schema = arrow::schema({arrow::field("geometry", arrow::binary()),
                                 arrow::field("bbox", bbox_type)},
                                kv);
    ASSERT_TRUE(WriteArrowTableToVsiMem(
        path.c_str(), arrow::Table::Make(schema, {geom_arr, bbox_arr})));

    auto ds = OpenParquetDataset(path.c_str());
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(TRUE, lyr->TestCapability(OLCFastGetExtent));
    EXPECT_EQ(TRUE, lyr->TestCapability(OLCFastSpatialFilter));
    OGREnvelope env;
    EXPECT_EQ(OGRERR_NONE, lyr->GetExtent(&env, /*bForce=*/FALSE));
    EXPECT_DOUBLE_EQ(1.0, env.MinX);
    EXPECT_DOUBLE_EQ(3.0, env.MaxX);
  }
}

TEST_F(OgrParquetLayerTest, GeoArrowStructFilterAndExplicitInt64Fid) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  // 1. GeoArrow struct spatial filter & SetIgnoredFields with bbox column.
  constexpr char kGaPath[] = "/vsimem/ogrparquetlayer_test/ga_point.parquet";
  {
    VSIMkdirRecursive(kTestDir, 0755);
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("Parquet");
    ASSERT_NE(nullptr, drv);
    GDALDatasetUniquePtr wr(
        drv->Create(kGaPath, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, wr);
    CPLStringList opts;
    opts.SetNameValue("GEOMETRY_ENCODING", "GEOARROW");
    opts.SetNameValue("FID", "fid64");
    opts.SetNameValue("ROW_GROUP_SIZE", "2");
    OGRLayer* lyr = wr->CreateLayer("ga", nullptr, wkbPoint, opts.List());
    ASSERT_NE(nullptr, lyr);
    OGRFieldDefn f_id("v", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_id));
    for (int i = 0; i < 4; ++i) {
      OGRFeature f(lyr->GetLayerDefn());
      f.SetFID(1000LL + i);
      f.SetField(0, i);
      auto pt = std::make_unique<OGRPoint>(i * 5.0, i * 5.0);
      f.SetGeometryDirectly(pt.release());
      ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(&f));
    }
    EXPECT_EQ(CE_None, wr->Close());
  }

  {
    auto ds = OpenParquetDataset(kGaPath);
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(TRUE, lyr->TestCapability(OLCFastSpatialFilter));

    // Ignore attribute column while keeping geometry for spatial filtering.
    const char* ign[] = {"v", nullptr};
    ASSERT_EQ(OGRERR_NONE, lyr->SetIgnoredFields(ign));
    lyr->SetSpatialFilterRect(9.0, 9.0, 16.0, 16.0);
    EXPECT_EQ(2, lyr->GetFeatureCount(/*bForce=*/1));
    lyr->SetSpatialFilter(nullptr);
    ASSERT_EQ(OGRERR_NONE, lyr->SetIgnoredFields(nullptr));

    std::unique_ptr<OGRFeature> f(lyr->GetFeature(1002));
    ASSERT_NE(nullptr, f);
    EXPECT_EQ(2, f->GetFieldAsInteger("v"));
    EXPECT_EQ(nullptr, lyr->GetFeature(9999));
  }
}

TEST_F(OgrParquetLayerTest, NativeGeographyProjJsonEpochAndBatchSeek) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  // 1. GeoParquet CRS as PROJJSON object + epoch in 'geo' metadata.
  {
    constexpr char kPath[] =
        "/vsimem/ogrparquetlayer_test/projjson_epoch.parquet";
    OGRSpatialReference srs;
    srs.importFromEPSG(32631);
    char* pj_raw = nullptr;
    ASSERT_EQ(OGRERR_NONE, srs.exportToPROJJSON(&pj_raw, nullptr));
    CPLMemoryCloser<char> pj_closer(pj_raw);
    const std::string pj_str(pj_closer.get());

    CPLJSONDocument pj_doc;
    ASSERT_TRUE(pj_doc.LoadMemory(pj_str));
    CPLJSONObject root;
    root.Add("version", "1.1.0");
    root.Add("primary_column", "geom");
    CPLJSONObject cols;
    CPLJSONObject col;
    col.Add("encoding", "WKB");
    col.Add("crs", pj_doc.GetRoot());
    col.Add("epoch", 2021.5);
    CPLJSONArray gtypes;
    gtypes.Add("Point");
    col.Add("geometry_types", gtypes);
    cols.Add("geom", col);
    root.Add("columns", cols);

    auto kv = std::make_shared<arrow::KeyValueMetadata>();
    kv->Append("geo", root.Format(CPLJSONObject::PrettyFormat::Plain));
    ASSERT_TRUE(WriteArrowTableToVsiMem(kPath, MakeMultiTypeTable(kv)));

    auto ds = OpenParquetDataset(kPath);
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    ASSERT_NE(nullptr, lyr->GetSpatialRef());
    EXPECT_STREQ("32631", lyr->GetSpatialRef()->GetAuthorityCode(nullptr));
    EXPECT_DOUBLE_EQ(2021.5, lyr->GetSpatialRef()->GetCoordinateEpoch());
  }

  // 2. Native Parquet GeographyLogicalType with projjson: file metadata lookup
  // + srid: prefix + missing projjson: key fallback.
  {
    constexpr char kPath[] =
        "/vsimem/ogrparquetlayer_test/native_geography.parquet";
    OGRSpatialReference srs;
    srs.importFromEPSG(4326);
    char* pj_raw = nullptr;
    ASSERT_EQ(OGRERR_NONE, srs.exportToPROJJSON(&pj_raw, nullptr));
    CPLMemoryCloser<char> pj_closer(pj_raw);
    const std::string pj_str(pj_closer.get());

    auto out_res = arrow::io::BufferOutputStream::Create();
    ASSERT_TRUE(out_res.ok());
    auto out = *out_res;

    parquet::schema::NodeVector fields;
    fields.push_back(parquet::schema::PrimitiveNode::Make(
        "geog", parquet::Repetition::OPTIONAL,
        parquet::GeographyLogicalType::Make(
            "projjson:custom_crs",
            parquet::LogicalType::EdgeInterpolationAlgorithm::SPHERICAL),
        parquet::Type::BYTE_ARRAY));
    fields.push_back(parquet::schema::PrimitiveNode::Make(
        "geom_srid", parquet::Repetition::OPTIONAL,
        parquet::GeometryLogicalType::Make("srid:32631"),
        parquet::Type::BYTE_ARRAY));
    fields.push_back(parquet::schema::PrimitiveNode::Make(
        "geom_missing_pj", parquet::Repetition::OPTIONAL,
        parquet::GeographyLogicalType::Make(
            "projjson:does_not_exist",
            parquet::LogicalType::EdgeInterpolationAlgorithm::SPHERICAL),
        parquet::Type::BYTE_ARRAY));
    auto schema = std::static_pointer_cast<parquet::schema::GroupNode>(
        parquet::schema::GroupNode::Make(
            "schema", parquet::Repetition::REQUIRED, fields));

    auto kv = std::make_shared<arrow::KeyValueMetadata>();
    kv->Append("custom_crs", pj_str);

    auto props = parquet::WriterProperties::Builder().build();
    auto fw = parquet::ParquetFileWriter::Open(out, schema, props, kv);
    auto* rg = fw->AppendRowGroup();
    const std::string pt_wkb = MakePointWkb(2.0, 48.0);
    const std::string line_wkb = [] {
      OGRLineString ls;
      ls.addPoint(0.0, 0.0);
      ls.addPoint(1.0, 1.0);
      std::string buf(ls.WkbSize(), '\0');
      ls.exportToWkb(reinterpret_cast<unsigned char*>(buf.data()));
      return buf;
    }();
    for (int col_idx = 0; col_idx < 3; ++col_idx) {
      auto* w = static_cast<parquet::ByteArrayWriter*>(rg->NextColumn());
      const std::string& v = (col_idx == 1) ? line_wkb : pt_wkb;
      const int16_t def = 1;
      parquet::ByteArray ba;
      ba.len = static_cast<uint32_t>(v.size());
      ba.ptr = reinterpret_cast<const uint8_t*>(v.data());
      w->WriteBatch(1, &def, nullptr, &ba);
    }
    fw->Close();
    auto buf_res = out->Finish();
    ASSERT_TRUE(buf_res.ok());
    ASSERT_TRUE(WriteBytesToVsiMem(
        kPath,
        std::string_view(reinterpret_cast<const char*>((*buf_res)->data()),
                         (*buf_res)->size())));

    auto ds = OpenParquetDataset(kPath);
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(3, lyr->GetLayerDefn()->GetGeomFieldCount());
    EXPECT_STREQ("SPHERICAL", lyr->GetMetadataItem("EDGES"));
    ASSERT_NE(nullptr, lyr->GetSpatialRef());
    EXPECT_STREQ("4326", lyr->GetSpatialRef()->GetAuthorityCode(nullptr));
    const auto* fdef1 = lyr->GetLayerDefn()->GetGeomFieldDefn(1);
    ASSERT_NE(nullptr, fdef1->GetSpatialRef());
    EXPECT_STREQ("32631", fdef1->GetSpatialRef()->GetAuthorityCode(nullptr));
    EXPECT_EQ(wkbLineString,
              lyr->GetLayerDefn()->GetGeomFieldDefn(1)->GetType());
    EXPECT_EQ(nullptr,
              lyr->GetLayerDefn()->GetGeomFieldDefn(2)->GetSpatialRef());
  }

  // 3. Old geoarrow.wkb extension on raw binary column without logical type.
  {
    constexpr char kPath[] =
        "/vsimem/ogrparquetlayer_test/legacy_geoarrow_wkb.parquet";
    arrow::BinaryBuilder wkb_b;
    ASSERT_TRUE(wkb_b.Append(MakePointWkb(1.0, 2.0)).ok());
    std::shared_ptr<arrow::Array> wkb_a;
    ASSERT_TRUE(wkb_b.Finish(&wkb_a).ok());
    auto fld = arrow::field(
        "g", arrow::binary(),
        arrow::key_value_metadata({"ARROW:extension:name"}, {"geoarrow.wkb"}));
    auto tbl = arrow::Table::Make(arrow::schema({fld}), {wkb_a});
    ASSERT_TRUE(WriteArrowTableToVsiMem(kPath, tbl));

    auto ds = OpenParquetDataset(kPath);
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(wkbPoint, lyr->GetGeomType());
  }

  // 4. Multi-batch SetNextByIndex within single row group via
  // OGR_PARQUET_BATCH_SIZE=2, single-batch SetNextByIndex, and GetArrowStream
  // clamping (0 and INT_MAX).
  {
    constexpr char kPath[] = "/vsimem/ogrparquetlayer_test/batch_seek.parquet";
    ASSERT_TRUE(CreateSampleParquetViaGdal(kPath, /*row_count=*/6,
                                           /*row_group_size=*/6));
    {
      CPLConfigOptionSetter batch_sz("OGR_PARQUET_BATCH_SIZE", "2", false);
      auto ds = OpenParquetDataset(kPath);
      ASSERT_NE(nullptr, ds);
      OGRLayer* lyr = ds->GetLayer(0);
      ASSERT_NE(nullptr, lyr);
      EXPECT_EQ(OGRERR_NONE, lyr->SetNextByIndex(5));
      std::unique_ptr<OGRFeature> f(lyr->GetNextFeature());
      ASSERT_NE(nullptr, f);
      EXPECT_EQ(5, f->GetFID());

      // When m_bSingleBatch is false, MAX_FEATURES_IN_BATCH="0" clamps to 1.
      lyr->ResetReading();
      struct ArrowArrayStream stream{};
      absl::Cleanup rel_stream = [&stream] {
        if (stream.release) stream.release(&stream);
      };
      CPLStringList opts;
      opts.SetNameValue("MAX_FEATURES_IN_BATCH", "0");
      ASSERT_TRUE(lyr->GetArrowStream(&stream, opts.List()));
      struct ArrowArray batch{};
      absl::Cleanup rel_batch = [&batch] {
        if (batch.release) batch.release(&batch);
      };
      ASSERT_EQ(0, stream.get_next(&stream, &batch));
      EXPECT_EQ(1, batch.length);
    }
    {
      auto ds = OpenParquetDataset(kPath);
      ASSERT_NE(nullptr, ds);
      OGRLayer* lyr = ds->GetLayer(0);
      ASSERT_NE(nullptr, lyr);
      EXPECT_EQ(OGRERR_NONE, lyr->SetNextByIndex(2));
      std::unique_ptr<OGRFeature> f(lyr->GetNextFeature());
      ASSERT_NE(nullptr, f);
      EXPECT_EQ(2, f->GetFID());

      // Exercise MAX_FEATURES_IN_BATCH=INT_MAX clamp in GetArrowStream().
      struct ArrowArrayStream stream{};
      absl::Cleanup rel_stream = [&stream] {
        if (stream.release) stream.release(&stream);
      };
      CPLStringList opts;
      opts.SetNameValue("MAX_FEATURES_IN_BATCH", "2147483647");
      ASSERT_TRUE(lyr->GetArrowStream(&stream, opts.List()));
      struct ArrowArray batch{};
      absl::Cleanup rel_batch = [&batch] {
        if (batch.release) batch.release(&batch);
      };
      ASSERT_EQ(0, stream.get_next(&stream, &batch));
      EXPECT_EQ(6, batch.length);
    }
  }
}

TEST_F(OgrParquetLayerTest, FidInt32IgnoreStructBBoxHelperAndStats) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  // Build table with INT32 FID, struct field, i64, f32, timestamp, string,
  // Overture bbox struct, and point WKB geometry across 2 row groups.
  arrow::Int32Builder fid_b, a_b;
  arrow::Int64Builder i64_b;
  arrow::FloatBuilder f32_b;
  arrow::StringBuilder b_b, s_b;
  arrow::TimestampBuilder ts_b(arrow::timestamp(arrow::TimeUnit::MILLI, "UTC"),
                               arrow::default_memory_pool());
  arrow::FloatBuilder xmin_b, ymin_b, xmax_b, ymax_b;
  arrow::BinaryBuilder geom_b;
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(fid_b.Append(500 + i).ok());
    ASSERT_TRUE(a_b.Append(i).ok());
    ASSERT_TRUE(b_b.Append("b_" + std::to_string(i)).ok());
    ASSERT_TRUE(i64_b.Append(10000000000LL + i).ok());
    ASSERT_TRUE(f32_b.Append(static_cast<float>(i) * 1.5f).ok());
    ASSERT_TRUE(ts_b.Append(1700000000000LL + i * 1000LL).ok());
    ASSERT_TRUE(s_b.Append("s_" + std::to_string(i)).ok());
    const float x = static_cast<float>(i) * 10.0f;
    const float y = static_cast<float>(i) * 10.0f;
    ASSERT_TRUE(xmin_b.Append(x).ok());
    ASSERT_TRUE(ymin_b.Append(y).ok());
    ASSERT_TRUE(xmax_b.Append(x + 1.0f).ok());
    ASSERT_TRUE(ymax_b.Append(y + 1.0f).ok());
    ASSERT_TRUE(geom_b.Append(MakePointWkb(x, y)).ok());
  }

  std::shared_ptr<arrow::Array> fid_a, a_arr, b_arr, i64_a, f32_a, ts_a, s_a,
      xmin_a, ymin_a, xmax_a, ymax_a, geom_a;
  ASSERT_TRUE(fid_b.Finish(&fid_a).ok());
  ASSERT_TRUE(a_b.Finish(&a_arr).ok());
  ASSERT_TRUE(b_b.Finish(&b_arr).ok());
  ASSERT_TRUE(i64_b.Finish(&i64_a).ok());
  ASSERT_TRUE(f32_b.Finish(&f32_a).ok());
  ASSERT_TRUE(ts_b.Finish(&ts_a).ok());
  ASSERT_TRUE(s_b.Finish(&s_a).ok());
  ASSERT_TRUE(xmin_b.Finish(&xmin_a).ok());
  ASSERT_TRUE(ymin_b.Finish(&ymin_a).ok());
  ASSERT_TRUE(xmax_b.Finish(&xmax_a).ok());
  ASSERT_TRUE(ymax_b.Finish(&ymax_a).ok());
  ASSERT_TRUE(geom_b.Finish(&geom_a).ok());

  auto grp_type = arrow::struct_(
      {arrow::field("a", arrow::int32()), arrow::field("b", arrow::utf8())});
  auto grp_res = arrow::StructArray::Make({a_arr, b_arr}, grp_type->fields());
  ASSERT_TRUE(grp_res.ok()) << grp_res.status().ToString();
  std::shared_ptr<arrow::Array> grp_a = *grp_res;
  auto bbox_type = arrow::struct_({arrow::field("xmin", arrow::float32()),
                                   arrow::field("ymin", arrow::float32()),
                                   arrow::field("xmax", arrow::float32()),
                                   arrow::field("ymax", arrow::float32())});
  auto bbox_res = arrow::StructArray::Make({xmin_a, ymin_a, xmax_a, ymax_a},
                                           bbox_type->fields());
  ASSERT_TRUE(bbox_res.ok()) << bbox_res.status().ToString();
  std::shared_ptr<arrow::Array> bbox_a = *bbox_res;

  auto kv = std::make_shared<arrow::KeyValueMetadata>();
  kv->Append("gdal:schema", "{\"fid\":\"fid32\"}");
  kv->Append("geo",
             "{\"version\":\"1.1.0\",\"primary_column\":\"geometry\","
             "\"columns\":{\"geometry\":{\"encoding\":\"WKB\","
             "\"geometry_types\":[\"Point\"],"
             "\"covering\":{\"bbox\":{"
             "\"xmin\":[\"bbox\",\"xmin\"],"
             "\"ymin\":[\"bbox\",\"ymin\"],"
             "\"xmax\":[\"bbox\",\"xmax\"],"
             "\"ymax\":[\"bbox\",\"ymax\"]}}}}}");
  auto schema = arrow::schema(
      {arrow::field("fid32", arrow::int32()), arrow::field("grp", grp_type),
       arrow::field("i64", arrow::int64()),
       arrow::field("f32", arrow::float32()),
       arrow::field("ts", arrow::timestamp(arrow::TimeUnit::MILLI, "UTC")),
       arrow::field("s", arrow::utf8()), arrow::field("bbox", bbox_type),
       arrow::field("geometry", arrow::binary())},
      kv);
  auto tbl = arrow::Table::Make(
      schema, {fid_a, grp_a, i64_a, f32_a, ts_a, s_a, bbox_a, geom_a});

  constexpr char kStatsPath[] =
      "/vsimem/ogrparquetlayer_test/fid32_stats.parquet";
  constexpr char kNoStatsPath[] =
      "/vsimem/ogrparquetlayer_test/fid32_nostats.parquet";
  ASSERT_TRUE(WriteArrowTableToVsiMem(kStatsPath, tbl, /*chunk_size=*/2));
  ASSERT_TRUE(WriteArrowTableToVsiMem(
      kNoStatsPath, tbl, /*chunk_size=*/2,
      parquet::WriterProperties::Builder().disable_statistics()->build()));

  {
    auto ds = OpenParquetDataset(kStatsPath);
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(4, lyr->GetFeatureCount(/*bForce=*/1));

    // Exercise GeomColsBBOXParquet().
    auto* pq_lyr = dynamic_cast<OGRParquetLayer*>(lyr);
    ASSERT_NE(nullptr, pq_lyr);
    int x0 = -1, y0 = -1, x1 = -1, y1 = -1;
    EXPECT_TRUE(pq_lyr->GeomColsBBOXParquet(0, x0, y0, x1, y1));
    EXPECT_GE(x0, 0);
    EXPECT_GE(y0, 0);
    EXPECT_GE(x1, 0);
    EXPECT_GE(y1, 0);
    EXPECT_FALSE(pq_lyr->GeomColsBBOXParquet(1, x0, y0, x1, y1));

    // INT32 FID lookup + ignored struct/geometry columns.
    const char* ign[] = {"grp.a", "grp.b", "geometry", nullptr};
    ASSERT_EQ(OGRERR_NONE, lyr->SetIgnoredFields(ign));
    {
      std::unique_ptr<OGRFeature> f(lyr->GetFeature(502));
      ASSERT_NE(nullptr, f);
      EXPECT_EQ(502, f->GetFID());
      EXPECT_EQ(nullptr, f->GetGeometryRef());
    }
    ASSERT_EQ(OGRERR_NONE, lyr->SetIgnoredFields(nullptr));

    // Trigger positive + all 4 bbox early-prune comparisons on Overture bbox.
    lyr->SetSpatialFilterRect(9.0, 9.0, 21.0, 21.0);
    EXPECT_EQ(2, lyr->GetFeatureCount(/*bForce=*/1));
    lyr->SetSpatialFilterRect(-20.0, -20.0, -5.0, 50.0);
    EXPECT_EQ(0, lyr->GetFeatureCount(/*bForce=*/1));
    lyr->SetSpatialFilterRect(-5.0, -20.0, 50.0, -5.0);
    EXPECT_EQ(0, lyr->GetFeatureCount(/*bForce=*/1));
    lyr->SetSpatialFilterRect(40.0, 0.0, 60.0, 60.0);
    EXPECT_EQ(0, lyr->GetFeatureCount(/*bForce=*/1));
    lyr->SetSpatialFilterRect(0.0, 40.0, 60.0, 60.0);
    EXPECT_EQ(0, lyr->GetFeatureCount(/*bForce=*/1));
    lyr->SetSpatialFilter(nullptr);

    // Trigger GetMinMaxForOGRField on timestamp, i64, f32, s via fast SQL
    // MIN/MAX optimization in OGRParquetDataset::ExecuteSQL and assert values.
    {
      OGRLayer* res = ds->ExecuteSQL(
          "SELECT MIN(i64), MAX(i64), MIN(f32), MAX(f32), MIN(s), MAX(s), "
          "MIN(ts), MAX(ts) FROM fid32_stats",
          nullptr, nullptr);
      ASSERT_NE(nullptr, res);
      absl::Cleanup rel = [&ds, res] { ds->ReleaseResultSet(res); };
      std::unique_ptr<OGRFeature> row(res->GetNextFeature());
      ASSERT_NE(nullptr, row);
      EXPECT_EQ(10000000000LL, row->GetFieldAsInteger64(0));
      EXPECT_EQ(10000000003LL, row->GetFieldAsInteger64(1));
      EXPECT_DOUBLE_EQ(0.0, row->GetFieldAsDouble(2));
      EXPECT_DOUBLE_EQ(4.5, row->GetFieldAsDouble(3));
      EXPECT_STREQ("s_0", row->GetFieldAsString(4));
      EXPECT_STREQ("s_3", row->GetFieldAsString(5));
      EXPECT_TRUE(row->IsFieldSetAndNotNull(6));
      EXPECT_TRUE(row->IsFieldSetAndNotNull(7));
    }
  }

  // Trigger missing-statistics fallbacks cleanly.
  {
    auto ds = OpenParquetDataset(kNoStatsPath);
    ASSERT_NE(nullptr, ds);
    OGRLayer* res = ds->ExecuteSQL(
        "SELECT MIN(i64), MAX(i64) FROM fid32_nostats", nullptr, nullptr);
    ASSERT_NE(nullptr, res);
    absl::Cleanup rel = [&ds, res] { ds->ReleaseResultSet(res); };
    std::unique_ptr<OGRFeature> row(res->GetNextFeature());
    ASSERT_NE(nullptr, row);
    EXPECT_EQ(10000000000LL, row->GetFieldAsInteger64(0));
    EXPECT_EQ(10000000003LL, row->GetFieldAsInteger64(1));
  }
}

// TODO: b/335317901 - Cover remaining OGRParquetLayer branches
// (OGR_USE_ARROWDATASET ArrowWkb metadata paths, antimeridian wraparound
// geostatistics, and Parquet reader ReadNext() I/O failure propagation).

}  // namespace
}  // namespace autotest2
