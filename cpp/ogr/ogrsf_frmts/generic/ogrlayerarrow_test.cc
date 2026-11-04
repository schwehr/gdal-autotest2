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

// Tests for OGRLayer Arrow C Data Interface support.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/generic/ogrlayerarrow.cpp
// http://google3/third_party/gdal/ogr/ogrsf_frmts/generic/ogrlayerarrow.cpp

#include "third_party/gdal/ogr/ogrsf_frmts/generic/ogrlayerarrow.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/mem/memdataset.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_recordbatch.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/generic/ograrrowarrayhelper.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

class ArrowTestLayer : public OGRLayer {
 public:
  using OGRLayer::CanPostFilterArrowArray;
  using OGRLayer::CreateSchemaForWKBGeometryColumn;
  using OGRLayer::GetLastErrorArrowArrayStream;
  using OGRLayer::PostFilterArrowArray;
  using OGRLayer::ReleaseArray;
  using OGRLayer::ReleaseSchema;
  using OGRLayer::ReleaseStream;
  using OGRLayer::StaticGetArrowSchema;
  using OGRLayer::StaticGetNextArrowArray;

  static bool TestCanPostFilter(const OGRLayer* lyr,
                                const struct ArrowSchema* schema) {
    auto fn = &ArrowTestLayer::CanPostFilterArrowArray;
    return (lyr->*fn)(schema);
  }

  static void TestPostFilter(const OGRLayer* lyr,
                             const struct ArrowSchema* schema,
                             struct ArrowArray* array,
                             CSLConstList options = nullptr) {
    auto fn = &ArrowTestLayer::PostFilterArrowArray;
    (lyr->*fn)(schema, array, options);
  }
};

// Helper to create an in-memory layer populated with diverse data types
std::unique_ptr<GDALDataset> CreatePopulatedDataset() {
  auto ds = std::make_unique<MEMDataset>();

  // Add coded field domain
  {
    std::string failure_reason;
    std::vector<OGRCodedValue> valid_codes;
    OGRCodedValue cv1{CPLStrdup("1"), CPLStrdup("one")};
    valid_codes.push_back(cv1);
    OGRCodedValue cv2{CPLStrdup("2"), nullptr};
    valid_codes.push_back(cv2);
    OGRCodedValue cv3{CPLStrdup("3"), CPLStrdup("three")};
    valid_codes.push_back(cv3);
    auto valid_dom = std::make_unique<OGRCodedFieldDomain>(
        "valid_domain", "desc", OFTInteger, OFSTNone, std::move(valid_codes));
    EXPECT_TRUE(ds->AddFieldDomain(std::move(valid_dom), failure_reason));
  }

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  OGRLayer* lyr = ds->CreateLayer("test_arrow", nullptr, wkbNone);
  OGRGeomFieldDefn geom_fld(OGRLayer::DEFAULT_ARROW_GEOMETRY_NAME, wkbPoint);
  geom_fld.SetSpatialRef(&srs);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateGeomField(&geom_fld));

  OGRFieldDefn f_int("f_int", OFTInteger);
  f_int.SetDomainName("valid_domain");
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_int));

  OGRFieldDefn f_int64("f_int64", OFTInteger64);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_int64));

  OGRFieldDefn f_real("f_real", OFTReal);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_real));

  OGRFieldDefn f_str("f_str", OFTString);
  f_str.SetAlternativeName("alt_str");
  f_str.SetComment("comment_str");
  f_str.SetDefault("'default_val'");
  f_str.SetUnique(true);
  f_str.SetWidth(32);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_str));

  OGRFieldDefn f_bin("f_bin", OFTBinary);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_bin));

  OGRFieldDefn f_bin_fixed("f_bin_fixed", OFTBinary);
  f_bin_fixed.SetWidth(4);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_bin_fixed));

  OGRFieldDefn f_date("f_date", OFTDate);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_date));

  OGRFieldDefn f_time("f_time", OFTTime);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_time));

  OGRFieldDefn f_dt("f_dt", OFTDateTime);
  f_dt.SetTZFlag(OGR_TZFLAG_UTC);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_dt));

  OGRFieldDefn f_dt_local("f_dt_local", OFTDateTime);
  f_dt_local.SetTZFlag(OGR_TZFLAG_LOCALTIME);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_dt_local));

  OGRFieldDefn f_dt_mixed("f_dt_mixed", OFTDateTime);
  f_dt_mixed.SetTZFlag(OGR_TZFLAG_MIXED_TZ);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_dt_mixed));

  OGRFieldDefn f_dt_tz8("f_dt_tz8", OFTDateTime);
  f_dt_tz8.SetTZFlag(108);  // +02:00
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_dt_tz8));

  OGRFieldDefn f_bool("f_bool", OFTInteger);
  f_bool.SetSubType(OFSTBoolean);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_bool));

  OGRFieldDefn f_int16("f_int16", OFTInteger);
  f_int16.SetSubType(OFSTInt16);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_int16));

  OGRFieldDefn f_float32("f_float32", OFTReal);
  f_float32.SetSubType(OFSTFloat32);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_float32));

  OGRFieldDefn f_json("f_json", OFTString);
  f_json.SetSubType(OFSTJSON);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_json));

  OGRFieldDefn f_uuid("f_uuid", OFTString);
  f_uuid.SetSubType(OFSTUUID);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_uuid));

  OGRFieldDefn f_intlist("f_intlist", OFTIntegerList);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_intlist));

  OGRFieldDefn f_boollist("f_boollist", OFTIntegerList);
  f_boollist.SetSubType(OFSTBoolean);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_boollist));

  OGRFieldDefn f_int16list("f_int16list", OFTIntegerList);
  f_int16list.SetSubType(OFSTInt16);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_int16list));

  OGRFieldDefn f_int64list("f_int64list", OFTInteger64List);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_int64list));

  OGRFieldDefn f_reallist("f_reallist", OFTRealList);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_reallist));

  OGRFieldDefn f_float32list("f_float32list", OFTRealList);
  f_float32list.SetSubType(OFSTFloat32);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_float32list));

  OGRFieldDefn f_strlist("f_strlist", OFTStringList);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_strlist));

  // Feature 1: full non-null values
  {
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    feat->SetFID(1);
    feat->SetField("f_int", 1);
    feat->SetField("f_int64", static_cast<GIntBig>(1234567890123LL));
    feat->SetField("f_real", 3.14159);
    feat->SetField("f_str", "hello");
    GByte bin_data[] = {0x01, 0x02, 0x03};
    feat->SetField(feat->GetFieldIndex("f_bin"), 3, bin_data);
    GByte fixed_bin[] = {0x11, 0x22, 0x33, 0x44};
    feat->SetField(feat->GetFieldIndex("f_bin_fixed"), 4, fixed_bin);
    feat->SetField("f_date", 2026, 1, 15, 0, 0, 0, 0);
    feat->SetField("f_time", 0, 0, 0, 12, 30, 45, 0);
    feat->SetField("f_dt", 2026, 1, 15, 12, 30, 45, 100);        // timezone UTC
    feat->SetField("f_dt_local", 2026, 1, 15, 12, 30, 45, 1);    // localtime
    feat->SetField("f_dt_mixed", 2026, 1, 15, 12, 30, 45, 108);  // mixed
    feat->SetField("f_dt_tz8", 2026, 1, 15, 12, 30, 45, 108);    // +02:00
    feat->SetField("f_bool", 1);
    feat->SetField("f_int16", 100);
    feat->SetField("f_float32", 1.5);
    feat->SetField("f_json", "{\"key\": 123}");
    feat->SetField("f_uuid", "123e4567-e89b-12d3-a456-426614174000");
    int int_list[] = {1, 2, 3};
    feat->SetField("f_intlist", 3, int_list);
    int bool_list[] = {1, 0, 1};
    feat->SetField("f_boollist", 3, bool_list);
    int int16_list[] = {10, 20};
    feat->SetField("f_int16list", 2, int16_list);
    GIntBig int64_list[] = {10000000000LL, 20000000000LL};
    feat->SetField("f_int64list", 2, int64_list);
    double real_list[] = {1.1, 2.2};
    feat->SetField("f_reallist", 2, real_list);
    double float32_list[] = {1.5, 2.5};
    feat->SetField("f_float32list", 2, float32_list);
    CPLStringList str_list;
    str_list.AddString("a");
    str_list.AddString("b");
    feat->SetField("f_strlist", str_list.List());

    auto pt = std::make_unique<OGRPoint>(10.0, 20.0);
    feat->SetGeometryDirectly(pt.release());

    EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  }

  // Feature 2: null / unset values
  {
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    feat->SetFID(2);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  }

  // Feature 3: partial values with padded fixed binary (< 4 bytes)
  {
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    feat->SetFID(3);
    GByte short_bin[] = {0xAA, 0xBB};
    feat->SetField(feat->GetFieldIndex("f_bin_fixed"), 2,
                   short_bin);  // 2 bytes into width 4 field -> padding
    auto pt = std::make_unique<OGRPoint>(30.0, 40.0);
    feat->SetGeometryDirectly(pt.release());
    EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  }

  return ds;
}

// ---------------------------------------------------------------------------
// 1. Metadata and Helper Tests
// ---------------------------------------------------------------------------

TEST(OGRLayerArrowTest, ParseArrowMetadata) {
  // Buffer with 0 count
  int32_t zero_count = 0;
  auto map_empty =
      OGRParseArrowMetadata(reinterpret_cast<const char*>(&zero_count));
  EXPECT_TRUE(map_empty.empty());

  // Valid metadata: 2 pairs
  std::string meta_buf;
  int32_t count = 2;
  meta_buf.append(reinterpret_cast<const char*>(&count), sizeof(count));

  std::string k1 = "key1";
  int32_t k1_len = static_cast<int32_t>(k1.size());
  meta_buf.append(reinterpret_cast<const char*>(&k1_len), sizeof(k1_len));
  meta_buf.append(k1);

  std::string v1 = "val1";
  int32_t v1_len = static_cast<int32_t>(v1.size());
  meta_buf.append(reinterpret_cast<const char*>(&v1_len), sizeof(v1_len));
  meta_buf.append(v1);

  std::string k2 = ARROW_EXTENSION_NAME_KEY;
  int32_t k2_len = static_cast<int32_t>(k2.size());
  meta_buf.append(reinterpret_cast<const char*>(&k2_len), sizeof(k2_len));
  meta_buf.append(k2);

  std::string v2 = EXTENSION_NAME_OGC_WKB;
  int32_t v2_len = static_cast<int32_t>(v2.size());
  meta_buf.append(reinterpret_cast<const char*>(&v2_len), sizeof(v2_len));
  meta_buf.append(v2);

  auto parsed = OGRParseArrowMetadata(meta_buf.data());
  EXPECT_EQ(2, parsed.size());
  EXPECT_EQ("val1", parsed["key1"]);
  EXPECT_EQ(EXTENSION_NAME_OGC_WKB, parsed[ARROW_EXTENSION_NAME_KEY]);
}

// ---------------------------------------------------------------------------
// 2. Stream Generation and Querying Across Options
// ---------------------------------------------------------------------------

TEST(OGRLayerArrowTest, GetArrowStreamDefaultAndOptions) {
  auto ds = CreatePopulatedDataset();
  OGRLayer* lyr = ds->GetLayer(0);

  // Default stream
  {
    OGRArrowArrayStream stream;
    EXPECT_TRUE(lyr->GetArrowStream(stream.get(), nullptr));

    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    EXPECT_EQ(0, stream.get_schema(&schema));
    EXPECT_STREQ("+s", schema.format);
    EXPECT_GT(schema.n_children, 0);

    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    EXPECT_EQ(0, stream.get_next(&array));
    EXPECT_EQ(3, array.length);

    if (array.release) array.release(&array);
    if (schema.release) schema.release(&schema);
  }

  // Stream with options: INCLUDE_FID=NO, MAX_FEATURES_IN_BATCH=1
  {
    OGRArrowArrayStream stream;
    CPLStringList opt;
    opt.AddString("INCLUDE_FID=NO");
    opt.AddString("MAX_FEATURES_IN_BATCH=1");
    EXPECT_TRUE(lyr->GetArrowStream(stream.get(), opt.List()));

    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    EXPECT_EQ(0, stream.get_schema(&schema));

    for (int i = 0; i < 3; ++i) {
      struct ArrowArray array;
      memset(&array, 0, sizeof(array));
      EXPECT_EQ(0, stream.get_next(&array));
      EXPECT_EQ(1, array.length);
      if (array.release) array.release(&array);
    }

    // End of stream
    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    EXPECT_EQ(0, stream.get_next(&array));
    EXPECT_EQ(0, array.length);
    if (array.release) array.release(&array);

    if (schema.release) schema.release(&schema);
  }

  // Geometry encodings: WKT, GEOARROW, GEOARROW_INTERLEAVED
  for (const char* encoding : {"WKT", "GEOARROW", "GEOARROW_INTERLEAVED"}) {
    OGRArrowArrayStream stream;
    CPLStringList opt;
    opt.SetNameValue("GEOMETRY_ENCODING", encoding);
    EXPECT_TRUE(lyr->GetArrowStream(stream.get(), opt.List()));

    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    EXPECT_EQ(0, stream.get_schema(&schema));

    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    EXPECT_EQ(0, stream.get_next(&array));
    EXPECT_EQ(3, array.length);

    if (array.release) array.release(&array);
    if (schema.release) schema.release(&schema);
  }

  // Stream with GEOMETRY_METADATA_ENCODING options
  for (const char* enc : {"OGC", "GEOARROW", "INVALID"}) {
    OGRArrowArrayStream stream;
    CPLStringList opt;
    opt.SetNameValue("GEOMETRY_METADATA_ENCODING", enc);
    EXPECT_TRUE(lyr->GetArrowStream(stream.get(), opt.List()));
    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    EXPECT_EQ(0, stream.get_schema(&schema));
    if (schema.release) schema.release(&schema);
  }

  // Timezone options: unknown, +02:00, UTC, and DATETIME_AS_STRING
  for (const char* tz : {"unknown", "+02:00", "UTC", "invalid_tz"}) {
    OGRArrowArrayStream stream;
    CPLStringList opt;
    opt.SetNameValue("TIMEZONE", tz);
    opt.SetNameValue("DATETIME_AS_STRING", "YES");
    EXPECT_TRUE(lyr->GetArrowStream(stream.get(), opt.List()));

    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    EXPECT_EQ(0, stream.get_schema(&schema));

    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    EXPECT_EQ(0, stream.get_next(&array));
    EXPECT_EQ(3, array.length);

    if (array.release) array.release(&array);
    if (schema.release) schema.release(&schema);
  }

  // Timezone options without DATETIME_AS_STRING
  for (const char* tz : {"unknown", "+02:00", "UTC"}) {
    OGRArrowArrayStream stream;
    CPLStringList opt;
    opt.SetNameValue("TIMEZONE", tz);
    EXPECT_TRUE(lyr->GetArrowStream(stream.get(), opt.List()));

    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    EXPECT_EQ(0, stream.get_schema(&schema));

    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    EXPECT_EQ(0, stream.get_next(&array));
    EXPECT_EQ(3, array.length);

    if (array.release) array.release(&array);
    if (schema.release) schema.release(&schema);
  }

  // Fast FID filtering on stream
  {
    lyr->SetAttributeFilter("FID = 1");
    OGRArrowArrayStream stream;
    EXPECT_TRUE(lyr->GetArrowStream(stream.get(), nullptr));
    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    EXPECT_EQ(0, stream.get_next(&array));
    EXPECT_EQ(1, array.length);
    if (array.release) array.release(&array);
    lyr->SetAttributeFilter(nullptr);
  }
  {
    lyr->SetAttributeFilter("FID IN (1, 3)");
    OGRArrowArrayStream stream;
    EXPECT_TRUE(lyr->GetArrowStream(stream.get(), nullptr));
    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    EXPECT_EQ(0, stream.get_next(&array));
    EXPECT_EQ(2, array.length);
    if (array.release) array.release(&array);
    lyr->SetAttributeFilter(nullptr);
  }

  // Duplicate stream error & GetLastError
  {
    OGRArrowArrayStream s1;
    EXPECT_TRUE(lyr->GetArrowStream(s1.get(), nullptr));
    OGRArrowArrayStream s2;
    EXPECT_FALSE(lyr->GetArrowStream(s2.get(), nullptr));
    EXPECT_NE(nullptr, s1.get()->get_last_error(s1.get()));
  }

  // Stream used after layer destruction
  {
    struct ArrowArrayStream orphaned_stream;
    memset(&orphaned_stream, 0, sizeof(orphaned_stream));
    {
      auto tmp_ds = std::make_unique<MEMDataset>();
      OGRLayer* tmp_lyr = tmp_ds->CreateLayer("tmp", nullptr, wkbNone);
      EXPECT_TRUE(tmp_lyr->GetArrowStream(&orphaned_stream, nullptr));
    }
    struct ArrowSchema orphaned_schema;
    memset(&orphaned_schema, 0, sizeof(orphaned_schema));
    EXPECT_EQ(EINVAL,
              orphaned_stream.get_schema(&orphaned_stream, &orphaned_schema));
    struct ArrowArray orphaned_array;
    memset(&orphaned_array, 0, sizeof(orphaned_array));
    EXPECT_EQ(EINVAL,
              orphaned_stream.get_next(&orphaned_stream, &orphaned_array));
    if (orphaned_stream.release) orphaned_stream.release(&orphaned_stream);
  }

  // Non-nullable geometry layer
  {
    auto geom_ds = std::make_unique<MEMDataset>();
    OGRLayer* geom_lyr = geom_ds->CreateLayer("geom_layer", nullptr, wkbNone);
    OGRGeomFieldDefn gfld(OGRLayer::DEFAULT_ARROW_GEOMETRY_NAME, wkbPoint);
    gfld.SetNullable(false);
    EXPECT_EQ(OGRERR_NONE, geom_lyr->CreateGeomField(&gfld));
    auto feat_empty = std::make_unique<OGRFeature>(geom_lyr->GetLayerDefn());
    auto pt = std::make_unique<OGRPoint>(1.0, 2.0);
    feat_empty->SetGeometryDirectly(pt.release());
    EXPECT_EQ(OGRERR_NONE, geom_lyr->CreateFeature(feat_empty.get()));

    OGRArrowArrayStream stream;
    EXPECT_TRUE(geom_lyr->GetArrowStream(stream.get(), nullptr));
    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    EXPECT_EQ(0, stream.get_next(&array));
    EXPECT_EQ(1, array.length);
    if (array.release) array.release(&array);
  }

  // CreateSchemaForWKBGeometryColumn direct call
  {
    OGRGeomFieldDefn gfld("my_geom", wkbPoint);
    auto* geom_schema = ArrowTestLayer::CreateSchemaForWKBGeometryColumn(
        &gfld, "Z", "INVALID_EXT");
    ASSERT_NE(nullptr, geom_schema);
    if (geom_schema->release) geom_schema->release(geom_schema);
    CPLFree(geom_schema);
  }

  // C API: OGR_L_GetArrowStream
  {
    struct ArrowArrayStream c_stream;
    memset(&c_stream, 0, sizeof(c_stream));
    EXPECT_TRUE(OGR_L_GetArrowStream(lyr, &c_stream, nullptr));
    if (c_stream.release) {
      c_stream.release(&c_stream);
    }
    EXPECT_FALSE(OGR_L_GetArrowStream(nullptr, &c_stream, nullptr));
  }

  // ReleaseSchema and ReleaseArray via OGRLayer methods
  {
    struct ArrowSchema dummy_schema;
    memset(&dummy_schema, 0, sizeof(dummy_schema));
    dummy_schema.format = const_cast<char*>("i");
    dummy_schema.release = [](struct ArrowSchema* s) { s->release = nullptr; };
    ArrowTestLayer::ReleaseSchema(&dummy_schema);
    EXPECT_EQ(nullptr, dummy_schema.release);

    struct ArrowArray dummy_array;
    memset(&dummy_array, 0, sizeof(dummy_array));
    dummy_array.release = [](struct ArrowArray* a) { a->release = nullptr; };
    ArrowTestLayer::ReleaseArray(&dummy_array);
    EXPECT_EQ(nullptr, dummy_array.release);
  }

  OSRCleanup();
}

// ---------------------------------------------------------------------------
// 3. Schema and Array Cloning
// ---------------------------------------------------------------------------

TEST(OGRLayerArrowTest, CloneArrowSchemaAndArray) {
  auto ds = CreatePopulatedDataset();
  OGRLayer* lyr = ds->GetLayer(0);

  OGRArrowArrayStream stream;
  EXPECT_TRUE(lyr->GetArrowStream(stream.get(), nullptr));

  struct ArrowSchema schema;
  memset(&schema, 0, sizeof(schema));
  EXPECT_EQ(0, stream.get_schema(&schema));

  struct ArrowArray array;
  memset(&array, 0, sizeof(array));
  EXPECT_EQ(0, stream.get_next(&array));

  // Clone Schema
  struct ArrowSchema cloned_schema;
  memset(&cloned_schema, 0, sizeof(cloned_schema));
  EXPECT_TRUE(OGRCloneArrowSchema(&schema, &cloned_schema));
  EXPECT_STREQ(schema.format, cloned_schema.format);
  EXPECT_EQ(schema.n_children, cloned_schema.n_children);

  // Clone Array
  struct ArrowArray cloned_array;
  memset(&cloned_array, 0, sizeof(cloned_array));
  EXPECT_TRUE(OGRCloneArrowArray(&schema, &array, &cloned_array));
  EXPECT_EQ(array.length, cloned_array.length);
  EXPECT_EQ(array.n_children, cloned_array.n_children);

  // Cleanup
  if (cloned_array.release) cloned_array.release(&cloned_array);
  if (cloned_schema.release) cloned_schema.release(&cloned_schema);
  if (array.release) array.release(&array);
  if (schema.release) schema.release(&schema);

  OSRCleanup();
}

// ---------------------------------------------------------------------------
// 4. Filtering and PostFilterArrowArray
// ---------------------------------------------------------------------------

TEST(OGRLayerArrowTest, PostFilterArrowArray) {
  auto ds = CreatePopulatedDataset();
  OGRLayer* lyr = ds->GetLayer(0);

  OGRArrowArrayStream stream;
  EXPECT_TRUE(lyr->GetArrowStream(stream.get(), nullptr));

  struct ArrowSchema schema;
  memset(&schema, 0, sizeof(schema));
  EXPECT_EQ(0, stream.get_schema(&schema));

  struct ArrowArray array;
  memset(&array, 0, sizeof(array));
  EXPECT_EQ(0, stream.get_next(&array));

  EXPECT_TRUE(ArrowTestLayer::TestCanPostFilter(lyr, &schema));

  // Set spatial filter: rectangle covering feature 1 (10, 20)
  lyr->SetSpatialFilterRect(0, 0, 50, 50);

  struct ArrowArray filtered_array;
  memset(&filtered_array, 0, sizeof(filtered_array));
  EXPECT_TRUE(OGRCloneArrowArray(&schema, &array, &filtered_array));
  ArrowTestLayer::TestPostFilter(lyr, &schema, &filtered_array);

  EXPECT_EQ(2, filtered_array.length);  // Feature 1 and 3 matched

  if (filtered_array.release) filtered_array.release(&filtered_array);

  // Set attribute filter
  lyr->SetSpatialFilter(nullptr);
  lyr->SetAttributeFilter("f_int = 1");

  struct ArrowArray attr_filtered;
  memset(&attr_filtered, 0, sizeof(attr_filtered));
  EXPECT_TRUE(OGRCloneArrowArray(&schema, &array, &attr_filtered));
  ArrowTestLayer::TestPostFilter(lyr, &schema, &attr_filtered);
  EXPECT_EQ(1, attr_filtered.length);

  if (attr_filtered.release) attr_filtered.release(&attr_filtered);
  if (array.release) array.release(&array);
  if (schema.release) schema.release(&schema);

  OSRCleanup();
}

// ---------------------------------------------------------------------------
// 5. IsArrowSchemaSupported and CreateFieldFromArrowSchema
// ---------------------------------------------------------------------------

TEST(OGRLayerArrowTest, IsArrowSchemaSupportedAndCreateField) {
  auto ds = std::make_unique<MEMDataset>();
  OGRLayer* lyr = ds->CreateLayer("target_layer", nullptr, wkbNone);

  // Helper to test a format string for field creation
  auto TestFieldFormat = [&](const char* name, const char* format) {
    struct ArrowSchema s;
    memset(&s, 0, sizeof(s));
    s.name = const_cast<char*>(name);
    s.format = const_cast<char*>(format);
    s.release = [](struct ArrowSchema*) {};

    EXPECT_TRUE(lyr->CreateFieldFromArrowSchema(&s, nullptr));
    if (s.release) s.release(&s);
  };

  TestFieldFormat("col_bool", "b");
  TestFieldFormat("col_int8", "c");
  TestFieldFormat("col_uint8", "C");
  TestFieldFormat("col_int16", "s");
  TestFieldFormat("col_uint16", "S");
  TestFieldFormat("col_int32", "i");
  TestFieldFormat("col_uint32", "I");
  TestFieldFormat("col_int64", "l");
  TestFieldFormat("col_uint64", "L");
  TestFieldFormat("col_float", "f");
  TestFieldFormat("col_double", "g");
  TestFieldFormat("col_string", "u");
  TestFieldFormat("col_largestring", "U");
  TestFieldFormat("col_binary", "z");
  TestFieldFormat("col_largebinary", "Z");
  TestFieldFormat("col_fixedbinary", "w:16");
  TestFieldFormat("col_date", "tdD");
  TestFieldFormat("col_time", "ttm");
  TestFieldFormat("col_timestamp", "tsu:");

  // Verify created fields
  const OGRFeatureDefn* defn = lyr->GetLayerDefn();
  EXPECT_NE(-1, defn->GetFieldIndex("col_bool"));
  EXPECT_NE(-1, defn->GetFieldIndex("col_int32"));
  EXPECT_NE(-1, defn->GetFieldIndex("col_string"));
  EXPECT_NE(-1, defn->GetFieldIndex("col_timestamp"));

  // IsArrowSchemaSupported on struct
  {
    struct ArrowSchema struct_schema;
    memset(&struct_schema, 0, sizeof(struct_schema));
    struct_schema.format = const_cast<char*>("+s");
    struct_schema.name = const_cast<char*>("");
    struct ArrowSchema child_col;
    memset(&child_col, 0, sizeof(child_col));
    child_col.format = const_cast<char*>("i");
    child_col.name = const_cast<char*>("val");
    struct ArrowSchema* children[1] = {&child_col};
    struct_schema.children = children;
    struct_schema.n_children = 1;

    std::string err_msg;
    EXPECT_TRUE(lyr->IsArrowSchemaSupported(&struct_schema, nullptr, err_msg));
    char* c_err = nullptr;
    EXPECT_TRUE(
        OGR_L_IsArrowSchemaSupported(lyr, &struct_schema, nullptr, &c_err));
    if (c_err) VSIFree(c_err);
  }

  // Unsupported format strings
  for (const char* bad_fmt :
       {"unsupported_type_xyz", "d", "d:50,2", "d:invalid"}) {
    struct ArrowSchema bad_s;
    memset(&bad_s, 0, sizeof(bad_s));
    bad_s.name = const_cast<char*>("bad");
    bad_s.format = const_cast<char*>(bad_fmt);
    bad_s.release = [](struct ArrowSchema*) {};
    EXPECT_FALSE(lyr->CreateFieldFromArrowSchema(&bad_s, nullptr));
  }

  // C API check with null handle
  struct ArrowSchema dummy_s;
  memset(&dummy_s, 0, sizeof(dummy_s));
  dummy_s.format = const_cast<char*>("i");
  EXPECT_FALSE(
      OGR_L_IsArrowSchemaSupported(nullptr, &dummy_s, nullptr, nullptr));
  EXPECT_FALSE(OGR_L_CreateFieldFromArrowSchema(nullptr, &dummy_s, nullptr));
}

// ---------------------------------------------------------------------------
// 6. WriteArrowBatch Round-Trip
// ---------------------------------------------------------------------------

TEST(OGRLayerArrowTest, WriteArrowBatchRoundTrip) {
  auto src_ds = CreatePopulatedDataset();
  OGRLayer* src_lyr = src_ds->GetLayer(0);

  OGRArrowArrayStream stream;
  EXPECT_TRUE(src_lyr->GetArrowStream(stream.get(), nullptr));

  struct ArrowSchema schema;
  memset(&schema, 0, sizeof(schema));
  EXPECT_EQ(0, stream.get_schema(&schema));

  struct ArrowArray array;
  memset(&array, 0, sizeof(array));
  EXPECT_EQ(0, stream.get_next(&array));

  // Write batch into target layer
  auto dest_ds = std::make_unique<MEMDataset>();
  OGRLayer* dest_lyr = dest_ds->CreateLayer("dest_layer", nullptr, wkbPoint);

  for (int64_t i = 0; i < schema.n_children; ++i) {
    const char* name = schema.children[i]->name;
    if (strcmp(name, "OGC_FID") != 0 &&
        strcmp(name, OGRLayer::DEFAULT_ARROW_GEOMETRY_NAME) != 0) {
      EXPECT_TRUE(
          dest_lyr->CreateFieldFromArrowSchema(schema.children[i], nullptr));
    }
  }

  struct ArrowArray array_clone;
  memset(&array_clone, 0, sizeof(array_clone));
  EXPECT_TRUE(OGRCloneArrowArray(&schema, &array, &array_clone));

  // C API test
  EXPECT_TRUE(OGR_L_WriteArrowBatch(dest_lyr, &schema, &array_clone, nullptr));
  EXPECT_EQ(3, dest_lyr->GetFeatureCount(true));
  dest_lyr->ResetReading();
  OGRFeature* f0 = dest_lyr->GetNextFeature();
  ASSERT_NE(nullptr, f0);
  EXPECT_EQ(1234567890123LL, f0->GetFieldAsInteger64("f_int64"));
  EXPECT_STREQ("hello", f0->GetFieldAsString("f_str"));
  ASSERT_NE(nullptr, f0->GetGeometryRef());
  OGRFeature::DestroyFeature(f0);

  // Write again via C++ API
  EXPECT_TRUE(dest_lyr->WriteArrowBatch(&schema, &array, nullptr));
  EXPECT_EQ(6, dest_lyr->GetFeatureCount(true));

  // Null handle checks
  EXPECT_FALSE(OGR_L_WriteArrowBatch(nullptr, &schema, &array, nullptr));

  if (array_clone.release) array_clone.release(&array_clone);
  if (array.release) array.release(&array);
  if (schema.release) schema.release(&schema);

  OSRCleanup();
}

// ---------------------------------------------------------------------------
// 7. Comprehensive Synthetic Batch Builder Covering All Arrow Formats
// ---------------------------------------------------------------------------

class ComprehensiveBatchBuilder {
 public:
  std::vector<void*> allocations_;
  struct ArrowSchema root_schema_{};
  struct ArrowArray root_array_{};
  std::vector<struct ArrowSchema*> schema_children_;
  std::vector<struct ArrowArray*> array_children_;

  ComprehensiveBatchBuilder() {
    memset(&root_schema_, 0, sizeof(root_schema_));
    memset(&root_array_, 0, sizeof(root_array_));
    root_schema_.format = AllocString("+s");
    root_schema_.name = AllocString("");
    root_schema_.release = [](struct ArrowSchema* s) { s->release = nullptr; };
    root_array_.length = 3;
    root_array_.null_count = 0;
    root_array_.release = [](struct ArrowArray* a) { a->release = nullptr; };
  }

  ~ComprehensiveBatchBuilder() {
    for (void* p : allocations_) {
      CPLFree(p);
    }
  }

  void* AllocBytes(size_t n) {
    void* p = CPLCalloc(1, n ? n : 1);
    allocations_.push_back(p);
    return p;
  }

  char* AllocString(const char* str) {
    char* p = CPLStrdup(str);
    allocations_.push_back(p);
    return p;
  }

  uint8_t* AllocValidity3() {
    auto* p = static_cast<uint8_t*>(AllocBytes(1));
    p[0] = 0x05;  // Row 0 valid, Row 1 null, Row 2 valid
    return p;
  }

  struct ArrowSchema* AllocSchema(const char* name, const char* format) {
    auto* s = static_cast<struct ArrowSchema*>(
        AllocBytes(sizeof(struct ArrowSchema)));
    s->name = AllocString(name);
    s->format = AllocString(format);
    s->flags = ARROW_FLAG_NULLABLE;
    s->release = [](struct ArrowSchema* sch) { sch->release = nullptr; };
    return s;
  }

  struct ArrowArray* AllocArray(int64_t length, int64_t null_count) {
    auto* a =
        static_cast<struct ArrowArray*>(AllocBytes(sizeof(struct ArrowArray)));
    a->length = length;
    a->null_count = null_count;
    a->release = [](struct ArrowArray* arr) { arr->release = nullptr; };
    return a;
  }

  template <typename T>
  void AddPrimitiveColumn(const char* name, const char* format, T v0, T v2) {
    auto* s = AllocSchema(name, format);
    auto* a = AllocArray(3, 1);
    a->n_buffers = 2;
    a->buffers = static_cast<const void**>(AllocBytes(2 * sizeof(void*)));
    a->buffers[0] = AllocValidity3();
    auto* vals = static_cast<T*>(AllocBytes(3 * sizeof(T)));
    vals[0] = v0;
    vals[1] = T{};
    vals[2] = v2;
    a->buffers[1] = vals;
    schema_children_.push_back(s);
    array_children_.push_back(a);
  }

  void AddBoolColumn(const char* name) {
    auto* s = AllocSchema(name, "b");
    auto* a = AllocArray(3, 1);
    a->n_buffers = 2;
    a->buffers = static_cast<const void**>(AllocBytes(2 * sizeof(void*)));
    a->buffers[0] = AllocValidity3();
    auto* vals = static_cast<uint8_t*>(AllocBytes(1));
    vals[0] = 0x05;  // bits 0=1, 1=0, 2=1
    a->buffers[1] = vals;
    schema_children_.push_back(s);
    array_children_.push_back(a);
  }

  void AddStringColumn(const char* name, const char* format, const char* s0,
                       const char* s2) {
    auto* s = AllocSchema(name, format);
    auto* a = AllocArray(3, 1);
    a->n_buffers = 3;
    a->buffers = static_cast<const void**>(AllocBytes(3 * sizeof(void*)));
    a->buffers[0] = AllocValidity3();
    size_t l0 = strlen(s0);
    size_t l2 = strlen(s2);
    size_t total_len = l0 + l2;
    char* data = static_cast<char*>(AllocBytes(total_len + 1));
    memcpy(data, s0, l0);
    memcpy(data + l0, s2, l2);
    a->buffers[2] = data;

    if (strcmp(format, "u") == 0 || strcmp(format, "z") == 0) {
      auto* offsets = static_cast<uint32_t*>(AllocBytes(4 * sizeof(uint32_t)));
      offsets[0] = 0;
      offsets[1] = static_cast<uint32_t>(l0);
      offsets[2] = static_cast<uint32_t>(l0);
      offsets[3] = static_cast<uint32_t>(total_len);
      a->buffers[1] = offsets;
    } else {
      auto* offsets = static_cast<uint64_t*>(AllocBytes(4 * sizeof(uint64_t)));
      offsets[0] = 0;
      offsets[1] = static_cast<uint64_t>(l0);
      offsets[2] = static_cast<uint64_t>(l0);
      offsets[3] = static_cast<uint64_t>(total_len);
      a->buffers[1] = offsets;
    }
    schema_children_.push_back(s);
    array_children_.push_back(a);
  }

  void AddFixedWidthBinaryColumn(const char* name, int width, const uint8_t* b0,
                                 const uint8_t* b2) {
    std::string fmt = CPLSPrintf("w:%d", width);
    auto* s = AllocSchema(name, fmt.c_str());
    auto* a = AllocArray(3, 1);
    a->n_buffers = 2;
    a->buffers = static_cast<const void**>(AllocBytes(2 * sizeof(void*)));
    a->buffers[0] = AllocValidity3();
    auto* data = static_cast<uint8_t*>(AllocBytes(3 * width));
    memcpy(data, b0, width);
    memcpy(data + 2 * width, b2, width);
    a->buffers[1] = data;
    schema_children_.push_back(s);
    array_children_.push_back(a);
  }

  void AddDecimalColumn(const char* name, const char* format, int width_bytes,
                        int64_t v0, int64_t v2) {
    auto* s = AllocSchema(name, format);
    auto* a = AllocArray(3, 1);
    a->n_buffers = 2;
    a->buffers = static_cast<const void**>(AllocBytes(2 * sizeof(void*)));
    a->buffers[0] = AllocValidity3();
    auto* data = static_cast<uint8_t*>(AllocBytes(3 * width_bytes));
    memcpy(data, &v0, sizeof(v0));
    memcpy(data + 2 * width_bytes, &v2, sizeof(v2));
    a->buffers[1] = data;
    schema_children_.push_back(s);
    array_children_.push_back(a);
  }

  template <typename T>
  void AddListColumn(const char* name, const char* list_format,
                     const char* child_format, T v0, T v2) {
    auto* s = AllocSchema(name, list_format);
    auto* a = AllocArray(3, 1);
    s->n_children = 1;
    s->children = static_cast<struct ArrowSchema**>(
        AllocBytes(sizeof(struct ArrowSchema*)));
    s->children[0] = AllocSchema("item", child_format);

    a->n_children = 1;
    a->children = static_cast<struct ArrowArray**>(
        AllocBytes(sizeof(struct ArrowArray*)));

    if (strcmp(list_format, "+w:1") == 0) {
      a->n_buffers = 1;
      a->buffers = static_cast<const void**>(AllocBytes(sizeof(void*)));
      a->buffers[0] = AllocValidity3();

      auto* child_arr = AllocArray(3, 1);
      child_arr->n_buffers = 2;
      child_arr->buffers =
          static_cast<const void**>(AllocBytes(2 * sizeof(void*)));
      child_arr->buffers[0] = AllocValidity3();
      auto* vals = static_cast<T*>(AllocBytes(3 * sizeof(T)));
      vals[0] = v0;
      vals[1] = T{};
      vals[2] = v2;
      child_arr->buffers[1] = vals;
      a->children[0] = child_arr;
    } else {
      a->n_buffers = 2;
      a->buffers = static_cast<const void**>(AllocBytes(2 * sizeof(void*)));
      a->buffers[0] = AllocValidity3();

      if (strcmp(list_format, "+l") == 0) {
        auto* offsets =
            static_cast<uint32_t*>(AllocBytes(4 * sizeof(uint32_t)));
        offsets[0] = 0;
        offsets[1] = 1;
        offsets[2] = 1;
        offsets[3] = 2;
        a->buffers[1] = offsets;
      } else {
        auto* offsets =
            static_cast<uint64_t*>(AllocBytes(4 * sizeof(uint64_t)));
        offsets[0] = 0;
        offsets[1] = 1;
        offsets[2] = 1;
        offsets[3] = 2;
        a->buffers[1] = offsets;
      }

      auto* child_arr = AllocArray(2, 0);
      child_arr->n_buffers = 2;
      child_arr->buffers =
          static_cast<const void**>(AllocBytes(2 * sizeof(void*)));
      child_arr->buffers[0] = nullptr;
      auto* vals = static_cast<T*>(AllocBytes(2 * sizeof(T)));
      vals[0] = v0;
      vals[1] = v2;
      child_arr->buffers[1] = vals;
      a->children[0] = child_arr;
    }
    schema_children_.push_back(s);
    array_children_.push_back(a);
  }

  std::pair<struct ArrowSchema*, struct ArrowArray*> CreateChildBool(
      const char* name, int len) {
    auto* s = AllocSchema(name, "b");
    auto* a = AllocArray(len, 0);
    a->n_buffers = 2;
    a->buffers = static_cast<const void**>(AllocBytes(2 * sizeof(void*)));
    a->buffers[0] = nullptr;
    auto* vals = static_cast<uint8_t*>(AllocBytes((len + 7) / 8));
    vals[0] = 0x05;
    a->buffers[1] = vals;
    return {s, a};
  }

  template <typename T>
  std::pair<struct ArrowSchema*, struct ArrowArray*> CreateChildPrimitive(
      const char* name, const char* format, T v0, T v1, int len) {
    auto* s = AllocSchema(name, format);
    auto* a = AllocArray(len, 0);
    a->n_buffers = 2;
    a->buffers = static_cast<const void**>(AllocBytes(2 * sizeof(void*)));
    a->buffers[0] = nullptr;
    auto* vals = static_cast<T*>(AllocBytes(len * sizeof(T)));
    vals[0] = v0;
    if (len > 1) vals[1] = v1;
    if (len > 2) vals[2] = v0;
    a->buffers[1] = vals;
    return {s, a};
  }

  std::pair<struct ArrowSchema*, struct ArrowArray*> CreateChildString(
      const char* name, const char* format, const char* str, int len) {
    auto* s = AllocSchema(name, format);
    auto* a = AllocArray(len, 0);
    a->n_buffers = 3;
    a->buffers = static_cast<const void**>(AllocBytes(3 * sizeof(void*)));
    a->buffers[0] = nullptr;
    size_t slen = strlen(str);
    size_t total = slen * len;
    char* data = static_cast<char*>(AllocBytes(total + 1));
    for (int i = 0; i < len; ++i) {
      memcpy(data + i * slen, str, slen);
    }
    a->buffers[2] = data;
    if (strcmp(format, "u") == 0 || strcmp(format, "z") == 0) {
      auto* offsets =
          static_cast<uint32_t*>(AllocBytes((len + 1) * sizeof(uint32_t)));
      for (int i = 0; i <= len; ++i)
        offsets[i] = static_cast<uint32_t>(i * slen);
      a->buffers[1] = offsets;
    } else {
      auto* offsets =
          static_cast<uint64_t*>(AllocBytes((len + 1) * sizeof(uint64_t)));
      for (int i = 0; i <= len; ++i)
        offsets[i] = static_cast<uint64_t>(i * slen);
      a->buffers[1] = offsets;
    }
    return {s, a};
  }

  std::pair<struct ArrowSchema*, struct ArrowArray*>
  CreateChildFixedWidthBinary(const char* name, int width, const uint8_t* bytes,
                              int len) {
    std::string fmt = CPLSPrintf("w:%d", width);
    auto* s = AllocSchema(name, fmt.c_str());
    auto* a = AllocArray(len, 0);
    a->n_buffers = 2;
    a->buffers = static_cast<const void**>(AllocBytes(2 * sizeof(void*)));
    a->buffers[0] = nullptr;
    auto* data = static_cast<uint8_t*>(AllocBytes(len * width));
    for (int i = 0; i < len; ++i) memcpy(data + i * width, bytes, width);
    a->buffers[1] = data;
    return {s, a};
  }

  std::pair<struct ArrowSchema*, struct ArrowArray*> CreateChildDecimal(
      const char* name, const char* format, int width_bytes, int64_t val,
      int len) {
    auto* s = AllocSchema(name, format);
    auto* a = AllocArray(len, 0);
    a->n_buffers = 2;
    a->buffers = static_cast<const void**>(AllocBytes(2 * sizeof(void*)));
    a->buffers[0] = nullptr;
    auto* data = static_cast<uint8_t*>(AllocBytes(len * width_bytes));
    for (int i = 0; i < len; ++i)
      memcpy(data + i * width_bytes, &val, sizeof(val));
    a->buffers[1] = data;
    return {s, a};
  }

  std::pair<struct ArrowSchema*, struct ArrowArray*> CreateChildWithNull(
      const char* name, const char* format, int len) {
    auto* s = AllocSchema(name, format);
    auto* a = AllocArray(len, 1);
    a->n_buffers = 2;
    a->buffers = static_cast<const void**>(AllocBytes(2 * sizeof(void*)));
    auto* validity = static_cast<uint8_t*>(AllocBytes((len + 7) / 8));
    validity[0] = 0x06;  // bit 0=0 (null), bit 1=1 (valid)
    a->buffers[0] = validity;
    auto* vals = static_cast<int32_t*>(AllocBytes(len * sizeof(int32_t)));
    vals[0] = 0;
    if (len > 1) vals[1] = 42;
    if (len > 2) vals[2] = 84;
    a->buffers[1] = vals;
    return {s, a};
  }

  template <typename T>
  std::pair<struct ArrowSchema*, struct ArrowArray*> CreateChildListWithNull(
      const char* name, const char* list_format, const char* child_format,
      T val, int len) {
    auto* s = AllocSchema(name, list_format);
    s->n_children = 1;
    s->children = static_cast<struct ArrowSchema**>(
        AllocBytes(sizeof(struct ArrowSchema*)));
    s->children[0] = AllocSchema("item", child_format);

    auto* a = AllocArray(len, 0);
    a->n_children = 1;
    a->children = static_cast<struct ArrowArray**>(
        AllocBytes(sizeof(struct ArrowArray*)));

    int total_items = len * 2;
    auto* child_arr = AllocArray(total_items, 1);
    child_arr->n_buffers = 2;
    child_arr->buffers =
        static_cast<const void**>(AllocBytes(2 * sizeof(void*)));
    auto* validity = static_cast<uint8_t*>(AllocBytes((total_items + 7) / 8));
    validity[0] = 0x02;  // item 0 is null, item 1 is valid
    child_arr->buffers[0] = validity;
    auto* vals = static_cast<T*>(AllocBytes(total_items * sizeof(T)));
    for (int i = 0; i < total_items; ++i) vals[i] = val;
    child_arr->buffers[1] = vals;
    a->children[0] = child_arr;

    if (strcmp(list_format, "+w:2") == 0) {
      a->n_buffers = 1;
      a->buffers = static_cast<const void**>(AllocBytes(sizeof(void*)));
      a->buffers[0] = nullptr;
    } else if (strcmp(list_format, "+l") == 0) {
      a->n_buffers = 2;
      a->buffers = static_cast<const void**>(AllocBytes(2 * sizeof(void*)));
      a->buffers[0] = nullptr;
      auto* offsets =
          static_cast<uint32_t*>(AllocBytes((len + 1) * sizeof(uint32_t)));
      for (int i = 0; i <= len; ++i) offsets[i] = static_cast<uint32_t>(i * 2);
      a->buffers[1] = offsets;
    } else {
      a->n_buffers = 2;
      a->buffers = static_cast<const void**>(AllocBytes(2 * sizeof(void*)));
      a->buffers[0] = nullptr;
      auto* offsets =
          static_cast<uint64_t*>(AllocBytes((len + 1) * sizeof(uint64_t)));
      for (int i = 0; i <= len; ++i) offsets[i] = static_cast<uint64_t>(i * 2);
      a->buffers[1] = offsets;
    }
    return {s, a};
  }

  std::pair<struct ArrowSchema*, struct ArrowArray*> CreateChildListFromItem(
      const char* name, const char* list_format,
      std::pair<struct ArrowSchema*, struct ArrowArray*> child_item, int len) {
    auto* s = AllocSchema(name, list_format);
    s->n_children = 1;
    s->children = static_cast<struct ArrowSchema**>(
        AllocBytes(sizeof(struct ArrowSchema*)));
    s->children[0] = child_item.first;

    auto* a = AllocArray(len, 0);
    a->n_children = 1;
    a->children = static_cast<struct ArrowArray**>(
        AllocBytes(sizeof(struct ArrowArray*)));
    a->children[0] = child_item.second;

    if (strcmp(list_format, "+w:2") == 0) {
      a->n_buffers = 1;
      a->buffers = static_cast<const void**>(AllocBytes(sizeof(void*)));
      a->buffers[0] = nullptr;
    } else {
      a->n_buffers = 2;
      a->buffers = static_cast<const void**>(AllocBytes(2 * sizeof(void*)));
      a->buffers[0] = nullptr;
      if (strcmp(list_format, "+l") == 0) {
        auto* offsets =
            static_cast<uint32_t*>(AllocBytes((len + 1) * sizeof(uint32_t)));
        for (int i = 0; i <= len; ++i)
          offsets[i] = static_cast<uint32_t>(i * 2);
        a->buffers[1] = offsets;
      } else {
        auto* offsets =
            static_cast<uint64_t*>(AllocBytes((len + 1) * sizeof(uint64_t)));
        for (int i = 0; i <= len; ++i)
          offsets[i] = static_cast<uint64_t>(i * 2);
        a->buffers[1] = offsets;
      }
    }
    return {s, a};
  }

  void AddStructListColumn(const char* name, const char* list_format) {
    int child_len = (strcmp(list_format, "+w:1") == 0) ? 3 : 2;
    auto* s = AllocSchema(name, list_format);
    auto* a = AllocArray(3, 1);
    if (strcmp(list_format, "+w:1") == 0) {
      a->n_buffers = 1;
      a->buffers = static_cast<const void**>(AllocBytes(sizeof(void*)));
      a->buffers[0] = AllocValidity3();
    } else {
      a->n_buffers = 2;
      a->buffers = static_cast<const void**>(AllocBytes(2 * sizeof(void*)));
      a->buffers[0] = AllocValidity3();
      if (strcmp(list_format, "+l") == 0) {
        auto* offsets =
            static_cast<uint32_t*>(AllocBytes(4 * sizeof(uint32_t)));
        offsets[0] = 0;
        offsets[1] = 1;
        offsets[2] = 1;
        offsets[3] = 2;
        a->buffers[1] = offsets;
      } else {
        auto* offsets =
            static_cast<uint64_t*>(AllocBytes(4 * sizeof(uint64_t)));
        offsets[0] = 0;
        offsets[1] = 1;
        offsets[2] = 1;
        offsets[3] = 2;
        a->buffers[1] = offsets;
      }
    }

    auto* struct_schema = AllocSchema("item", "+s");
    auto* struct_array = AllocArray(child_len, 0);
    struct_array->n_buffers = 1;
    struct_array->buffers =
        static_cast<const void**>(AllocBytes(sizeof(void*)));
    struct_array->buffers[0] = nullptr;

    std::vector<std::pair<struct ArrowSchema*, struct ArrowArray*>> children;
    children.push_back(CreateChildBool("s_bool", child_len));
    children.push_back(
        CreateChildPrimitive<int8_t>("s_int8", "c", 1, 2, child_len));
    children.push_back(
        CreateChildPrimitive<uint8_t>("s_uint8", "C", 3, 4, child_len));
    children.push_back(
        CreateChildPrimitive<int16_t>("s_int16", "s", 5, 6, child_len));
    children.push_back(
        CreateChildPrimitive<uint16_t>("s_uint16", "S", 7, 8, child_len));
    children.push_back(
        CreateChildPrimitive<int32_t>("s_int32", "i", 9, 10, child_len));
    children.push_back(
        CreateChildPrimitive<uint32_t>("s_uint32", "I", 11, 12, child_len));
    children.push_back(
        CreateChildPrimitive<int64_t>("s_int64", "l", 13, 14, child_len));
    children.push_back(
        CreateChildPrimitive<uint64_t>("s_uint64", "L", 15, 16, child_len));
    children.push_back(CreateChildPrimitive<uint16_t>("s_f16", "e", 0x3c00,
                                                      0x4000, child_len));
    children.push_back(
        CreateChildPrimitive<float>("s_f32", "f", 1.5f, 2.5f, child_len));
    children.push_back(
        CreateChildPrimitive<double>("s_f64", "g", 3.5, 4.5, child_len));
    children.push_back(CreateChildString("s_utf8", "u", "hello", child_len));
    children.push_back(
        CreateChildString("s_large_utf8", "U", "world", child_len));
    children.push_back(CreateChildString("s_bin", "z", "\x01\x02", child_len));
    children.push_back(
        CreateChildString("s_large_bin", "Z", "\x03\x04", child_len));
    uint8_t bin4[] = {1, 2, 3, 4};
    children.push_back(CreateChildFixedWidthBinary("s_w4", 4, bin4, child_len));
    children.push_back(
        CreateChildDecimal("s_dec", "d:10,2", 16, 12345, child_len));
    children.push_back(CreateChildWithNull("s_null", "i", child_len));
    children.push_back(
        CreateChildListWithNull<int32_t>("s_list", "+l", "i", 77, child_len));
    children.push_back(CreateChildListWithNull<int32_t>("s_largelist", "+L",
                                                        "i", 88, child_len));
    children.push_back(CreateChildListWithNull<int32_t>("s_fixedlist", "+w:2",
                                                        "i", 99, child_len));

    // Diverse child lists to exercise AddToArray for all types
    children.push_back(CreateChildListFromItem(
        "s_list_b", "+l", CreateChildBool("item", child_len * 2), child_len));
    children.push_back(CreateChildListFromItem(
        "s_list_c", "+l",
        CreateChildPrimitive<int8_t>("item", "c", 1, 2, child_len * 2),
        child_len));
    children.push_back(CreateChildListFromItem(
        "s_list_C", "+l",
        CreateChildPrimitive<uint8_t>("item", "C", 3, 4, child_len * 2),
        child_len));
    children.push_back(CreateChildListFromItem(
        "s_list_s", "+l",
        CreateChildPrimitive<int16_t>("item", "s", 5, 6, child_len * 2),
        child_len));
    children.push_back(CreateChildListFromItem(
        "s_list_S", "+l",
        CreateChildPrimitive<uint16_t>("item", "S", 7, 8, child_len * 2),
        child_len));
    children.push_back(CreateChildListFromItem(
        "s_list_I", "+l",
        CreateChildPrimitive<uint32_t>("item", "I", 11, 12, child_len * 2),
        child_len));
    children.push_back(CreateChildListFromItem(
        "s_list_l", "+l",
        CreateChildPrimitive<int64_t>("item", "l", 13, 14, child_len * 2),
        child_len));
    children.push_back(CreateChildListFromItem(
        "s_list_L", "+l",
        CreateChildPrimitive<uint64_t>("item", "L", 15, 16, child_len * 2),
        child_len));
    children.push_back(
        CreateChildListFromItem("s_list_e", "+l",
                                CreateChildPrimitive<uint16_t>(
                                    "item", "e", 0x3c00, 0x4000, child_len * 2),
                                child_len));
    children.push_back(CreateChildListFromItem(
        "s_list_f", "+l",
        CreateChildPrimitive<float>("item", "f", 1.5f, 2.5f, child_len * 2),
        child_len));
    children.push_back(CreateChildListFromItem(
        "s_list_g", "+l",
        CreateChildPrimitive<double>("item", "g", 3.5, 4.5, child_len * 2),
        child_len));
    children.push_back(CreateChildListFromItem(
        "s_list_u", "+l",
        CreateChildString("item", "u", "hello", child_len * 2), child_len));
    children.push_back(CreateChildListFromItem(
        "s_list_U", "+l",
        CreateChildString("item", "U", "world", child_len * 2), child_len));
    children.push_back(CreateChildListFromItem(
        "s_list_z", "+l",
        CreateChildString("item", "z", "\x01\x02", child_len * 2), child_len));
    children.push_back(CreateChildListFromItem(
        "s_list_Z", "+l",
        CreateChildString("item", "Z", "\x03\x04", child_len * 2), child_len));
    uint8_t bin4_list[] = {1, 2, 3, 4};
    children.push_back(CreateChildListFromItem(
        "s_list_w4", "+l",
        CreateChildFixedWidthBinary("item", 4, bin4_list, child_len * 2),
        child_len));
    children.push_back(CreateChildListFromItem(
        "s_list_dec", "+l",
        CreateChildDecimal("item", "d:10,2", 16, 12345, child_len * 2),
        child_len));

    // Nested struct
    {
      auto* nest_s = AllocSchema("s_struct", "+s");
      auto* nest_a = AllocArray(child_len, 0);
      nest_a->n_buffers = 1;
      nest_a->buffers = static_cast<const void**>(AllocBytes(sizeof(void*)));
      nest_a->buffers[0] = nullptr;
      auto nest_child =
          CreateChildPrimitive<int32_t>("inner_i", "i", 123, 456, child_len);
      nest_s->n_children = 1;
      nest_s->children = static_cast<struct ArrowSchema**>(
          AllocBytes(sizeof(struct ArrowSchema*)));
      nest_s->children[0] = nest_child.first;
      nest_a->n_children = 1;
      nest_a->children = static_cast<struct ArrowArray**>(
          AllocBytes(sizeof(struct ArrowArray*)));
      nest_a->children[0] = nest_child.second;
      children.push_back({nest_s, nest_a});
    }

    // Nested map
    {
      auto* map_s = AllocSchema("s_map", "+m");
      auto* map_a = AllocArray(child_len, 0);
      map_a->n_buffers = 2;
      map_a->buffers = static_cast<const void**>(AllocBytes(2 * sizeof(void*)));
      map_a->buffers[0] = nullptr;
      auto* offsets = static_cast<uint32_t*>(
          AllocBytes((child_len + 1) * sizeof(uint32_t)));
      for (int i = 0; i <= child_len; ++i)
        offsets[i] = static_cast<uint32_t>(i);
      map_a->buffers[1] = offsets;

      map_s->n_children = 1;
      map_s->children = static_cast<struct ArrowSchema**>(
          AllocBytes(sizeof(struct ArrowSchema*)));
      auto* entries_s = AllocSchema("entries", "+s");
      map_s->children[0] = entries_s;

      map_a->n_children = 1;
      map_a->children = static_cast<struct ArrowArray**>(
          AllocBytes(sizeof(struct ArrowArray*)));
      auto* entries_a = AllocArray(child_len, 0);
      entries_a->n_buffers = 1;
      entries_a->buffers = static_cast<const void**>(AllocBytes(sizeof(void*)));
      entries_a->buffers[0] = nullptr;
      map_a->children[0] = entries_a;

      entries_s->n_children = 2;
      entries_s->children = static_cast<struct ArrowSchema**>(
          AllocBytes(2 * sizeof(struct ArrowSchema*)));
      entries_s->children[0] = AllocSchema("k", "u");
      entries_s->children[1] = AllocSchema("v", "i");

      entries_a->n_children = 2;
      entries_a->children = static_cast<struct ArrowArray**>(
          AllocBytes(2 * sizeof(struct ArrowArray*)));
      auto k_child = CreateChildString("k", "u", "key", child_len);
      auto v_child =
          CreateChildPrimitive<int32_t>("v", "i", 555, 666, child_len);
      entries_a->children[0] = k_child.second;
      entries_a->children[1] = v_child.second;
      children.push_back({map_s, map_a});
    }

    struct_schema->n_children = static_cast<int64_t>(children.size());
    struct_schema->children = static_cast<struct ArrowSchema**>(
        AllocBytes(children.size() * sizeof(struct ArrowSchema*)));
    struct_array->n_children = static_cast<int64_t>(children.size());
    struct_array->children = static_cast<struct ArrowArray**>(
        AllocBytes(children.size() * sizeof(struct ArrowArray*)));
    for (size_t i = 0; i < children.size(); ++i) {
      struct_schema->children[i] = children[i].first;
      struct_array->children[i] = children[i].second;
    }

    s->n_children = 1;
    s->children = static_cast<struct ArrowSchema**>(
        AllocBytes(sizeof(struct ArrowSchema*)));
    s->children[0] = struct_schema;
    a->n_children = 1;
    a->children = static_cast<struct ArrowArray**>(
        AllocBytes(sizeof(struct ArrowArray*)));
    a->children[0] = struct_array;

    schema_children_.push_back(s);
    array_children_.push_back(a);
  }

  void AddTopStructColumn(const char* name) {
    auto* s = AllocSchema(name, "+s");
    auto* a = AllocArray(3, 1);
    a->n_buffers = 1;
    a->buffers = static_cast<const void**>(AllocBytes(sizeof(void*)));
    a->buffers[0] = AllocValidity3();

    s->n_children = 2;
    s->children = static_cast<struct ArrowSchema**>(
        AllocBytes(2 * sizeof(struct ArrowSchema*)));
    s->children[0] = AllocSchema("sub_i", "i");
    s->children[1] = AllocSchema("sub_u", "u");

    a->n_children = 2;
    a->children = static_cast<struct ArrowArray**>(
        AllocBytes(2 * sizeof(struct ArrowArray*)));

    auto sub_i = CreateChildPrimitive<int32_t>("sub_i", "i", 100, 200, 3);
    auto sub_u = CreateChildString("sub_u", "u", "struct_str", 3);
    a->children[0] = sub_i.second;
    a->children[1] = sub_u.second;

    schema_children_.push_back(s);
    array_children_.push_back(a);
  }

  void AddStringListColumn(const char* name, const char* list_format,
                           const char* child_format) {
    auto* s = AllocSchema(name, list_format);
    s->n_children = 1;
    s->children = static_cast<struct ArrowSchema**>(
        AllocBytes(sizeof(struct ArrowSchema*)));
    s->children[0] = AllocSchema("item", child_format);

    auto* a = AllocArray(3, 1);
    a->n_children = 1;
    a->children = static_cast<struct ArrowArray**>(
        AllocBytes(sizeof(struct ArrowArray*)));

    if (strcmp(list_format, "+w:1") == 0) {
      a->n_buffers = 1;
      a->buffers = static_cast<const void**>(AllocBytes(sizeof(void*)));
      a->buffers[0] = AllocValidity3();

      auto child = CreateChildString("item", child_format, "elem", 3);
      child.second->buffers[0] = AllocValidity3();
      child.second->null_count = 1;
      a->children[0] = child.second;
    } else {
      a->n_buffers = 2;
      a->buffers = static_cast<const void**>(AllocBytes(2 * sizeof(void*)));
      a->buffers[0] = AllocValidity3();

      if (strcmp(list_format, "+l") == 0) {
        auto* offsets =
            static_cast<uint32_t*>(AllocBytes(4 * sizeof(uint32_t)));
        offsets[0] = 0;
        offsets[1] = 1;
        offsets[2] = 1;
        offsets[3] = 2;
        a->buffers[1] = offsets;
      } else {
        auto* offsets =
            static_cast<uint64_t*>(AllocBytes(4 * sizeof(uint64_t)));
        offsets[0] = 0;
        offsets[1] = 1;
        offsets[2] = 1;
        offsets[3] = 2;
        a->buffers[1] = offsets;
      }

      auto child = CreateChildString("item", child_format, "elem", 2);
      a->children[0] = child.second;
    }
    schema_children_.push_back(s);
    array_children_.push_back(a);
  }

  void AddDecimalListColumn(const char* name, const char* list_format,
                            const char* dec_format, int width_bytes) {
    auto* s = AllocSchema(name, list_format);
    s->n_children = 1;
    s->children = static_cast<struct ArrowSchema**>(
        AllocBytes(sizeof(struct ArrowSchema*)));
    s->children[0] = AllocSchema("item", dec_format);

    auto* a = AllocArray(3, 1);
    a->n_children = 1;
    a->children = static_cast<struct ArrowArray**>(
        AllocBytes(sizeof(struct ArrowArray*)));

    if (strcmp(list_format, "+w:1") == 0) {
      a->n_buffers = 1;
      a->buffers = static_cast<const void**>(AllocBytes(sizeof(void*)));
      a->buffers[0] = AllocValidity3();

      auto child =
          CreateChildDecimal("item", dec_format, width_bytes, 12345, 3);
      child.second->buffers[0] = AllocValidity3();
      child.second->null_count = 1;
      a->children[0] = child.second;
    } else {
      a->n_buffers = 2;
      a->buffers = static_cast<const void**>(AllocBytes(2 * sizeof(void*)));
      a->buffers[0] = AllocValidity3();

      if (strcmp(list_format, "+l") == 0) {
        auto* offsets =
            static_cast<uint32_t*>(AllocBytes(4 * sizeof(uint32_t)));
        offsets[0] = 0;
        offsets[1] = 1;
        offsets[2] = 1;
        offsets[3] = 2;
        a->buffers[1] = offsets;
      } else {
        auto* offsets =
            static_cast<uint64_t*>(AllocBytes(4 * sizeof(uint64_t)));
        offsets[0] = 0;
        offsets[1] = 1;
        offsets[2] = 1;
        offsets[3] = 2;
        a->buffers[1] = offsets;
      }

      auto child =
          CreateChildDecimal("item", dec_format, width_bytes, 12345, 2);
      a->children[0] = child.second;
    }
    schema_children_.push_back(s);
    array_children_.push_back(a);
  }

  void AddMapColumn(const char* name) {
    auto* s = AllocSchema(name, "+m");
    auto* a = AllocArray(3, 1);
    a->n_buffers = 2;
    a->buffers = static_cast<const void**>(AllocBytes(2 * sizeof(void*)));
    a->buffers[0] = AllocValidity3();
    auto* offsets = static_cast<uint32_t*>(AllocBytes(4 * sizeof(uint32_t)));
    offsets[0] = 0;
    offsets[1] = 1;
    offsets[2] = 1;
    offsets[3] = 2;
    a->buffers[1] = offsets;

    s->n_children = 1;
    s->children = static_cast<struct ArrowSchema**>(
        AllocBytes(sizeof(struct ArrowSchema*)));
    auto* entry_schema = AllocSchema("entries", "+s");
    s->children[0] = entry_schema;

    a->n_children = 1;
    a->children = static_cast<struct ArrowArray**>(
        AllocBytes(sizeof(struct ArrowArray*)));
    auto* entry_array = AllocArray(2, 0);
    entry_array->n_buffers = 1;
    entry_array->buffers = static_cast<const void**>(AllocBytes(sizeof(void*)));
    entry_array->buffers[0] = nullptr;
    a->children[0] = entry_array;

    entry_schema->n_children = 2;
    entry_schema->children = static_cast<struct ArrowSchema**>(
        AllocBytes(2 * sizeof(struct ArrowSchema*)));
    auto* k_schema = AllocSchema("keys", "u");
    auto* v_schema = AllocSchema("values", "i");
    entry_schema->children[0] = k_schema;
    entry_schema->children[1] = v_schema;

    entry_array->n_children = 2;
    entry_array->children = static_cast<struct ArrowArray**>(
        AllocBytes(2 * sizeof(struct ArrowArray*)));

    auto* k_arr = AllocArray(2, 0);
    k_arr->n_buffers = 3;
    k_arr->buffers = static_cast<const void**>(AllocBytes(3 * sizeof(void*)));
    k_arr->buffers[0] = nullptr;
    auto* k_offsets = static_cast<uint32_t*>(AllocBytes(3 * sizeof(uint32_t)));
    k_offsets[0] = 0;
    k_offsets[1] = 2;
    k_offsets[2] = 4;
    k_arr->buffers[1] = k_offsets;
    k_arr->buffers[2] = AllocString("k1k2");
    entry_array->children[0] = k_arr;

    auto* v_arr = AllocArray(2, 0);
    v_arr->n_buffers = 2;
    v_arr->buffers = static_cast<const void**>(AllocBytes(2 * sizeof(void*)));
    v_arr->buffers[0] = nullptr;
    auto* v_vals = static_cast<int32_t*>(AllocBytes(2 * sizeof(int32_t)));
    v_vals[0] = 100;
    v_vals[1] = 200;
    v_arr->buffers[1] = v_vals;
    entry_array->children[1] = v_arr;

    schema_children_.push_back(s);
    array_children_.push_back(a);
  }

  void AddDictionaryColumn(const char* name, const char* index_format) {
    auto* s = AllocSchema(name, index_format);
    auto* a = AllocArray(3, 1);
    a->n_buffers = 2;
    a->buffers = static_cast<const void**>(AllocBytes(2 * sizeof(void*)));
    a->buffers[0] = AllocValidity3();

    int64_t idx_bytes = 1;
    if (strcmp(index_format, "s") == 0 || strcmp(index_format, "S") == 0) {
      idx_bytes = 2;
    } else if (strcmp(index_format, "i") == 0 ||
               strcmp(index_format, "I") == 0) {
      idx_bytes = 4;
    } else if (strcmp(index_format, "l") == 0 ||
               strcmp(index_format, "L") == 0) {
      idx_bytes = 8;
    }
    auto* idx_data = static_cast<uint8_t*>(AllocBytes(3 * idx_bytes));
    if (idx_bytes == 1) {
      idx_data[0] = 0;
      idx_data[2] = 1;
    } else if (idx_bytes == 2) {
      reinterpret_cast<int16_t*>(idx_data)[0] = 0;
      reinterpret_cast<int16_t*>(idx_data)[2] = 1;
    } else if (idx_bytes == 4) {
      reinterpret_cast<int32_t*>(idx_data)[0] = 0;
      reinterpret_cast<int32_t*>(idx_data)[2] = 1;
    } else {
      reinterpret_cast<int64_t*>(idx_data)[0] = 0;
      reinterpret_cast<int64_t*>(idx_data)[2] = 1;
    }
    a->buffers[1] = idx_data;

    auto* dict_schema = AllocSchema("dict", "u");
    auto* dict_array = AllocArray(2, 0);
    dict_array->n_buffers = 3;
    dict_array->buffers =
        static_cast<const void**>(AllocBytes(3 * sizeof(void*)));
    dict_array->buffers[0] = nullptr;
    auto* dict_offsets =
        static_cast<uint32_t*>(AllocBytes(3 * sizeof(uint32_t)));
    dict_offsets[0] = 0;
    dict_offsets[1] = 3;
    dict_offsets[2] = 6;
    dict_array->buffers[1] = dict_offsets;
    dict_array->buffers[2] = AllocString("catdog");

    s->dictionary = dict_schema;
    a->dictionary = dict_array;

    schema_children_.push_back(s);
    array_children_.push_back(a);
  }

  void AddWKBGeometryColumn(const char* name, const char* format,
                            const char* ext_name) {
    auto* s = AllocSchema(name, format);
    std::string k = ARROW_EXTENSION_NAME_KEY;
    int32_t k_len = static_cast<int32_t>(k.size());
    std::string v = ext_name;
    int32_t v_len = static_cast<int32_t>(v.size());
    int32_t count = 1;
    size_t meta_size =
        sizeof(count) + sizeof(k_len) + k.size() + sizeof(v_len) + v.size();
    char* meta = static_cast<char*>(AllocBytes(meta_size));
    char* ptr = meta;
    memcpy(ptr, &count, sizeof(count));
    ptr += sizeof(count);
    memcpy(ptr, &k_len, sizeof(k_len));
    ptr += sizeof(k_len);
    memcpy(ptr, k.data(), k.size());
    ptr += k.size();
    memcpy(ptr, &v_len, sizeof(v_len));
    ptr += sizeof(v_len);
    memcpy(ptr, v.data(), v.size());
    s->metadata = meta;

    OGRPoint pt0(10.0, 20.0);
    OGRPoint pt2(30.0, 40.0);
    size_t wkb_size0 = pt0.WkbSize();
    size_t wkb_size2 = pt2.WkbSize();
    std::vector<GByte> wkb0(wkb_size0);
    std::vector<GByte> wkb2(wkb_size2);
    static_cast<OGRGeometry&>(pt0).exportToWkb(wkbNDR, wkb0.data(),
                                               wkbVariantIso);
    static_cast<OGRGeometry&>(pt2).exportToWkb(wkbNDR, wkb2.data(),
                                               wkbVariantIso);

    auto* a = AllocArray(3, 1);
    a->n_buffers = 3;
    a->buffers = static_cast<const void**>(AllocBytes(3 * sizeof(void*)));
    a->buffers[0] = AllocValidity3();

    size_t total_wkb = wkb_size0 + wkb_size2;
    GByte* wkb_data = static_cast<GByte*>(AllocBytes(total_wkb));
    memcpy(wkb_data, wkb0.data(), wkb_size0);
    memcpy(wkb_data + wkb_size0, wkb2.data(), wkb_size2);
    a->buffers[2] = wkb_data;

    if (strcmp(format, "z") == 0) {
      auto* offsets = static_cast<uint32_t*>(AllocBytes(4 * sizeof(uint32_t)));
      offsets[0] = 0;
      offsets[1] = static_cast<uint32_t>(wkb_size0);
      offsets[2] = static_cast<uint32_t>(wkb_size0);
      offsets[3] = static_cast<uint32_t>(total_wkb);
      a->buffers[1] = offsets;
    } else {
      auto* offsets = static_cast<uint64_t*>(AllocBytes(4 * sizeof(uint64_t)));
      offsets[0] = 0;
      offsets[1] = static_cast<uint64_t>(wkb_size0);
      offsets[2] = static_cast<uint64_t>(wkb_size0);
      offsets[3] = static_cast<uint64_t>(total_wkb);
      a->buffers[1] = offsets;
    }

    schema_children_.push_back(s);
    array_children_.push_back(a);
  }

  void Finalize() {
    int n = static_cast<int>(schema_children_.size());
    root_schema_.n_children = n;
    root_schema_.children = static_cast<struct ArrowSchema**>(
        AllocBytes(n * sizeof(struct ArrowSchema*)));
    for (int i = 0; i < n; ++i) {
      root_schema_.children[i] = schema_children_[i];
    }

    root_array_.n_children = n;
    root_array_.children = static_cast<struct ArrowArray**>(
        AllocBytes(n * sizeof(struct ArrowArray*)));
    for (int i = 0; i < n; ++i) {
      root_array_.children[i] = array_children_[i];
    }
  }
};

TEST(OGRLayerArrowTest, ComprehensiveArrowFormats) {
  ComprehensiveBatchBuilder b;

  // Scalar columns
  b.AddBoolColumn("col_b");
  b.AddPrimitiveColumn<int8_t>("col_int8", "c", 10, 20);
  b.AddPrimitiveColumn<uint8_t>("col_uint8", "C", 11, 21);
  b.AddPrimitiveColumn<int16_t>("col_int16", "s", 100, 200);
  b.AddPrimitiveColumn<uint16_t>("col_uint16", "S", 101, 201);
  b.AddPrimitiveColumn<int32_t>("col_int32", "i", 1000, 2000);
  b.AddPrimitiveColumn<uint32_t>("col_uint32", "I", 1001, 2001);
  b.AddPrimitiveColumn<int64_t>("col_int64", "l", 100000, 200000);
  b.AddPrimitiveColumn<uint64_t>("col_uint64", "L", 100001, 200001);
  b.AddPrimitiveColumn<uint16_t>("col_e", "e", 0x3c00, 0x4000);  // float16
  b.AddPrimitiveColumn<float>("col_f", "f", 1.5f, 2.5f);
  b.AddPrimitiveColumn<double>("col_g", "g", 1.25, 2.75);
  b.AddStringColumn("col_utf8", "u", "cat", "dog");
  b.AddStringColumn("col_large_utf8", "U", "alpha", "beta");
  b.AddStringColumn("col_bin", "z", "\x01\x02", "\x03\x04");
  b.AddStringColumn("col_large_bin", "Z", "\x05\x06", "\x07\x08");

  uint8_t bin4_0[] = {1, 2, 3, 4};
  uint8_t bin4_2[] = {5, 6, 7, 8};
  b.AddFixedWidthBinaryColumn("col_w4", 4, bin4_0, bin4_2);
  b.AddDecimalColumn("col_d128", "d:10,2", 16, 12345, 67890);
  b.AddDecimalColumn("col_d256", "d:10,2,256", 32, 54321, 98765);

  // Temporal columns
  b.AddPrimitiveColumn<int32_t>("col_tdD", "tdD", 19000, 19500);
  b.AddPrimitiveColumn<int64_t>("col_tdm", "tdm", 1600000000000LL,
                                1700000000000LL);
  b.AddPrimitiveColumn<int32_t>("col_tts", "tts", 3661, 7200);
  b.AddPrimitiveColumn<int32_t>("col_ttm", "ttm", 3661500, 7200250);
  b.AddPrimitiveColumn<int64_t>("col_ttu", "ttu", 3661500000LL, 7200250000LL);
  b.AddPrimitiveColumn<int64_t>("col_ttn", "ttn", 3661500000000LL,
                                7200250000000LL);
  b.AddPrimitiveColumn<int64_t>("col_tss", "tss:", 1600000000LL, 1700000000LL);
  b.AddPrimitiveColumn<int64_t>("col_tss_utc", "tss:UTC", 1600000000LL,
                                1700000000LL);
  b.AddPrimitiveColumn<int64_t>("col_tsm_tz", "tsm:+02:00", 1600000000000LL,
                                1700000000000LL);
  b.AddPrimitiveColumn<int64_t>("col_tsu_tz", "tsu:-05:30", 1600000000000000LL,
                                1700000000000000LL);
  b.AddPrimitiveColumn<int64_t>("col_tsn_utc", "tsn:UTC", 1600000000000000000LL,
                                1700000000000000000LL);

  // List columns
  b.AddListColumn<int32_t>("col_list_int32", "+l", "i", 10, 20);
  b.AddListColumn<int32_t>("col_largelist_int32", "+L", "i", 10, 20);
  b.AddListColumn<int32_t>("col_fixedlist_int32", "+w:1", "i", 10, 20);

  b.AddListColumn<int64_t>("col_list_int64", "+l", "l", 100LL, 200LL);
  b.AddListColumn<int64_t>("col_largelist_int64", "+L", "l", 100LL, 200LL);
  b.AddListColumn<int64_t>("col_fixedlist_int64", "+w:1", "l", 100LL, 200LL);

  b.AddListColumn<double>("col_list_double", "+l", "g", 1.5, 2.5);
  b.AddListColumn<double>("col_largelist_double", "+L", "g", 1.5, 2.5);
  b.AddListColumn<double>("col_fixedlist_double", "+w:1", "g", 1.5, 2.5);

  b.AddListColumn<float>("col_list_float", "+l", "f", 1.1f, 2.2f);
  b.AddListColumn<int16_t>("col_list_int16", "+l", "s", 5, 15);
  b.AddListColumn<uint8_t>("col_list_bool", "+l", "b", 1, 0);

  // Additional List columns
  b.AddListColumn<int8_t>("col_list_int8", "+l", "c", 1, 2);
  b.AddListColumn<int8_t>("col_fixedlist_int8", "+w:1", "c", 1, 2);
  b.AddListColumn<uint8_t>("col_list_uint8", "+l", "C", 1, 2);
  b.AddListColumn<uint8_t>("col_fixedlist_uint8", "+w:1", "C", 1, 2);
  b.AddListColumn<uint16_t>("col_list_uint16", "+l", "S", 1, 2);
  b.AddListColumn<uint16_t>("col_fixedlist_uint16", "+w:1", "S", 1, 2);
  b.AddListColumn<uint32_t>("col_list_uint32", "+l", "I", 1, 2);
  b.AddListColumn<uint32_t>("col_fixedlist_uint32", "+w:1", "I", 1, 2);
  b.AddListColumn<uint64_t>("col_list_uint64", "+l", "L", 1, 2);
  b.AddListColumn<uint64_t>("col_fixedlist_uint64", "+w:1", "L", 1, 2);

  b.AddListColumn<uint16_t>("col_list_float16", "+l", "e", 0x3c00, 0x4000);
  b.AddListColumn<uint16_t>("col_largelist_float16", "+L", "e", 0x3c00, 0x4000);
  b.AddListColumn<uint16_t>("col_fixedlist_float16", "+w:1", "e", 0x3c00,
                            0x4000);

  b.AddListColumn<float>("col_largelist_float", "+L", "f", 1.1f, 2.2f);
  b.AddListColumn<float>("col_fixedlist_float", "+w:1", "f", 1.1f, 2.2f);
  b.AddListColumn<int16_t>("col_largelist_int16", "+L", "s", 5, 15);
  b.AddListColumn<int16_t>("col_fixedlist_int16", "+w:1", "s", 5, 15);
  b.AddListColumn<uint8_t>("col_largelist_bool", "+L", "b", 1, 0);
  b.AddListColumn<uint8_t>("col_fixedlist_bool", "+w:1", "b", 1, 0);

  // String and Decimal list columns
  b.AddStringListColumn("col_list_utf8", "+l", "u");
  b.AddStringListColumn("col_largelist_utf8", "+L", "u");
  b.AddStringListColumn("col_fixedlist_utf8", "+w:1", "u");
  b.AddStringListColumn("col_list_large_utf8", "+l", "U");
  b.AddStringListColumn("col_largelist_large_utf8", "+L", "U");
  b.AddStringListColumn("col_fixedlist_large_utf8", "+w:1", "U");

  b.AddDecimalListColumn("col_list_dec", "+l", "d:10,2", 16);
  b.AddDecimalListColumn("col_largelist_dec", "+L", "d:10,2", 16);
  b.AddDecimalListColumn("col_fixedlist_dec", "+w:1", "d:10,2", 16);

  // Struct list and top struct columns
  b.AddStructListColumn("col_struct_list", "+l");
  b.AddStructListColumn("col_large_struct_list", "+L");
  b.AddStructListColumn("col_fixed_struct_list", "+w:1");
  b.AddTopStructColumn("col_top_struct");

  // Map and Dictionary columns
  b.AddMapColumn("col_map");
  b.AddDictionaryColumn("col_dict_int8", "c");
  b.AddDictionaryColumn("col_dict_uint8", "C");
  b.AddDictionaryColumn("col_dict_int16", "s");
  b.AddDictionaryColumn("col_dict_uint16", "S");
  b.AddDictionaryColumn("col_dict_int32", "i");
  b.AddDictionaryColumn("col_dict_uint32", "I");
  b.AddDictionaryColumn("col_dict_int64", "l");
  b.AddDictionaryColumn("col_dict_uint64", "L");

  // Geometry and FID columns
  b.AddWKBGeometryColumn("wkb_geometry", "z", EXTENSION_NAME_OGC_WKB);
  b.AddPrimitiveColumn<int32_t>("FID", "i", 10, 20);

  b.Finalize();

  // 1. Clone testing
  struct ArrowSchema cloned_s;
  memset(&cloned_s, 0, sizeof(cloned_s));
  EXPECT_TRUE(OGRCloneArrowSchema(&b.root_schema_, &cloned_s));

  struct ArrowArray cloned_a;
  memset(&cloned_a, 0, sizeof(cloned_a));
  EXPECT_TRUE(OGRCloneArrowArray(&b.root_schema_, &b.root_array_, &cloned_a));
  EXPECT_EQ(3, cloned_a.length);
  EXPECT_GT(cloned_s.n_children, 0);

  // Non-zero offset clone testing
  {
    struct ArrowArray offset_arr;
    memset(&offset_arr, 0, sizeof(offset_arr));
    cloned_a.offset = 1;
    cloned_a.length = 2;
    EXPECT_TRUE(OGRCloneArrowArray(&cloned_s, &cloned_a, &offset_arr));
    EXPECT_EQ(2, offset_arr.length);
    if (offset_arr.release) offset_arr.release(&offset_arr);
    cloned_a.offset = 0;
    cloned_a.length = 3;
  }

  // 2. IsArrowSchemaSupported and CreateFieldFromArrowSchema
  auto dest_native_ds = std::make_unique<MEMDataset>();
  OGRLayer* dest_native_lyr =
      dest_native_ds->CreateLayer("dest_native", nullptr, wkbPoint);
  std::string root_schema_err;
  EXPECT_TRUE(dest_native_lyr->IsArrowSchemaSupported(&cloned_s, nullptr,
                                                      root_schema_err));
  for (int64_t i = 0; i < cloned_s.n_children; ++i) {
    const auto child = cloned_s.children[i];
    if (strcmp(child->name, "FID") != 0 &&
        strcmp(child->name, "wkb_geometry") != 0) {
      EXPECT_TRUE(dest_native_lyr->CreateFieldFromArrowSchema(child, nullptr));
    }
  }

  // 3. WriteArrowBatch into native layer
  {
    struct ArrowArray write_a1;
    memset(&write_a1, 0, sizeof(write_a1));
    EXPECT_TRUE(OGRCloneArrowArray(&cloned_s, &cloned_a, &write_a1));
    CPLStringList write_opt;
    write_opt.SetNameValue("FID", "FID");
    write_opt.SetNameValue("GEOMETRY_NAME", "wkb_geometry");
    EXPECT_TRUE(dest_native_lyr->WriteArrowBatch(&cloned_s, &write_a1,
                                                 write_opt.List()));
    EXPECT_EQ(3, dest_native_lyr->GetFeatureCount(true));
    if (write_a1.release) write_a1.release(&write_a1);
  }

  // 4. WriteArrowBatch into all-string layer (exercises
  // SetFieldForOtherFormats)
  {
    auto dest_str_ds = std::make_unique<MEMDataset>();
    OGRLayer* dest_str_lyr =
        dest_str_ds->CreateLayer("dest_str", nullptr, wkbPoint);
    for (int64_t i = 0; i < cloned_s.n_children; ++i) {
      if (strcmp(cloned_s.children[i]->name, "wkb_geometry") != 0) {
        if (strcmp(cloned_s.children[i]->format, "+s") == 0) {
          for (int64_t j = 0; j < cloned_s.children[i]->n_children; ++j) {
            std::string n = std::string(cloned_s.children[i]->name) + "." +
                            cloned_s.children[i]->children[j]->name;
            OGRFieldDefn fld(n.c_str(), OFTString);
            EXPECT_EQ(OGRERR_NONE, dest_str_lyr->CreateField(&fld));
          }
        } else {
          OGRFieldDefn fld(cloned_s.children[i]->name, OFTString);
          EXPECT_EQ(OGRERR_NONE, dest_str_lyr->CreateField(&fld));
        }
      }
    }
    struct ArrowArray write_a2;
    memset(&write_a2, 0, sizeof(write_a2));
    EXPECT_TRUE(OGRCloneArrowArray(&cloned_s, &cloned_a, &write_a2));
    CPLStringList write_opt;
    write_opt.SetNameValue("GEOMETRY_NAME", "wkb_geometry");
    EXPECT_TRUE(
        dest_str_lyr->WriteArrowBatch(&cloned_s, &write_a2, write_opt.List()));
    EXPECT_EQ(3, dest_str_lyr->GetFeatureCount(true));
    if (write_a2.release) write_a2.release(&write_a2);
  }

  // 5. PostFilterArrowArray check
  {
    std::string attr_filter;
    for (int i = 0; i < dest_native_lyr->GetLayerDefn()->GetFieldCount(); ++i) {
      const char* fld_name =
          dest_native_lyr->GetLayerDefn()->GetFieldDefn(i)->GetNameRef();
      if (STARTS_WITH(fld_name, "col_list_") ||
          STARTS_WITH(fld_name, "col_largelist_") ||
          STARTS_WITH(fld_name, "col_fixedlist_") ||
          STARTS_WITH(fld_name, "col_struct_list") ||
          STARTS_WITH(fld_name, "col_large_struct_list") ||
          STARTS_WITH(fld_name, "col_fixed_struct_list") ||
          STARTS_WITH(fld_name, "col_top_struct") ||
          STARTS_WITH(fld_name, "col_dict_") ||
          strcmp(fld_name, "col_map") == 0) {
        continue;
      }
      if (!attr_filter.empty()) attr_filter += " OR ";
      attr_filter += fld_name;
      attr_filter += " IS NOT NULL";
    }
    attr_filter = "(" + attr_filter + ") AND col_int32 = 1000";
    dest_native_lyr->SetAttributeFilter(attr_filter.c_str());

    struct ArrowArray filter_a;
    memset(&filter_a, 0, sizeof(filter_a));
    EXPECT_TRUE(OGRCloneArrowArray(&cloned_s, &cloned_a, &filter_a));
    EXPECT_TRUE(ArrowTestLayer::TestCanPostFilter(dest_native_lyr, &cloned_s));
    ArrowTestLayer::TestPostFilter(dest_native_lyr, &cloned_s, &filter_a);
    EXPECT_EQ(1, filter_a.length);
    if (filter_a.release) filter_a.release(&filter_a);
    dest_native_lyr->SetAttributeFilter(nullptr);
  }

  // FID filter with BASE_SEQUENTIAL_FID
  {
    dest_native_lyr->SetAttributeFilter("FID = 10");
    struct ArrowArray fid_a;
    memset(&fid_a, 0, sizeof(fid_a));
    EXPECT_TRUE(OGRCloneArrowArray(&cloned_s, &cloned_a, &fid_a));
    CPLStringList post_opt;
    post_opt.SetNameValue("BASE_SEQUENTIAL_FID", "10");
    ArrowTestLayer::TestPostFilter(dest_native_lyr, &cloned_s, &fid_a,
                                   post_opt.List());
    EXPECT_EQ(1, fid_a.length);
    if (fid_a.release) fid_a.release(&fid_a);

    // Early return optimization for FID out of range
    dest_native_lyr->SetAttributeFilter("FID = 9999");
    memset(&fid_a, 0, sizeof(fid_a));
    EXPECT_TRUE(OGRCloneArrowArray(&cloned_s, &cloned_a, &fid_a));
    ArrowTestLayer::TestPostFilter(dest_native_lyr, &cloned_s, &fid_a,
                                   post_opt.List());
    EXPECT_EQ(0, fid_a.length);
    if (fid_a.release) fid_a.release(&fid_a);
    dest_native_lyr->SetAttributeFilter(nullptr);
  }

  if (cloned_a.release) cloned_a.release(&cloned_a);
  if (cloned_s.release) cloned_s.release(&cloned_s);

  OSRCleanup();
}

class UnpreservedFidLayer : public OGRMemLayer {
  bool force_large_fid_;

 public:
  explicit UnpreservedFidLayer(bool force_large_fid)
      : OGRMemLayer("unpreserved_fid", nullptr, wkbNone),
        force_large_fid_(force_large_fid) {
    OGRFieldDefn fld("val", OFTInteger);
    EXPECT_EQ(OGRERR_NONE, CreateField(&fld));
  }

  OGRErr ICreateFeature(OGRFeature* poFeature) override {
    if (force_large_fid_) {
      poFeature->SetFID(3000000000LL);  // > int32_t::max()
    } else {
      poFeature->SetFID(999);  // Different from input FID (e.g. 10)
    }
    return OGRERR_NONE;
  }
};

class CustomDriverDataset : public MEMDataset {
 public:
  GDALDriver driver_{};
  CustomDriverDataset() {
    driver_.SetMetadataItem(GDAL_DMD_CREATIONFIELDDATATYPES, "String");
    poDriver = &driver_;
  }
};

class CustomDriverLayer : public OGRMemLayer {
  GDALDataset* ds_ = nullptr;
  std::string fid_col_{};

 public:
  CustomDriverLayer(const char* name, GDALDataset* ds, const char* fid_col = "")
      : OGRMemLayer(name, nullptr, wkbNone), ds_(ds), fid_col_(fid_col) {}

  GDALDataset* GetDataset() override { return ds_; }
  const char* GetFIDColumn() const override { return fid_col_.c_str(); }

  OGRErr CreateField(const OGRFieldDefn* poField,
                     int bApproxOK = TRUE) override {
    if (std::string(poField->GetNameRef()) == "col_rename") {
      OGRFieldDefn renamed(*poField);
      renamed.SetName("col_rename_renamed");
      return OGRMemLayer::CreateField(&renamed, bApproxOK);
    }
    return OGRMemLayer::CreateField(poField, bApproxOK);
  }
};

TEST(OGRLayerArrowTest, WriteArrowBatchOptionsAndErrors) {
  WithQuietHandler quiet_handler;
  CPLErrorReset();
  // 1. FID preservation options
  {
    ComprehensiveBatchBuilder b;
    b.AddPrimitiveColumn<int32_t>("FID", "i", 10, 20);
    b.AddPrimitiveColumn<int32_t>("val", "i", 1, 2);
    b.Finalize();

    struct ArrowSchema s{};
    struct ArrowArray a{};
    EXPECT_TRUE(OGRCloneArrowSchema(&b.root_schema_, &s));
    EXPECT_TRUE(OGRCloneArrowArray(&b.root_schema_, &b.root_array_, &a));

    UnpreservedFidLayer layer_warn(false);
    CPLStringList opt_warn;
    opt_warn.SetNameValue("FID", "FID");
    opt_warn.SetNameValue("IF_FID_NOT_PRESERVED", "WARNING");
    struct ArrowArray a_warn;
    EXPECT_TRUE(OGRCloneArrowArray(&s, &a, &a_warn));
    EXPECT_TRUE(layer_warn.WriteArrowBatch(&s, &a_warn, opt_warn.List()));
    if (a_warn.release) a_warn.release(&a_warn);

    UnpreservedFidLayer layer_err(false);
    CPLStringList opt_err;
    opt_err.SetNameValue("FID", "FID");
    opt_err.SetNameValue("IF_FID_NOT_PRESERVED", "ERROR");
    struct ArrowArray a_err;
    EXPECT_TRUE(OGRCloneArrowArray(&s, &a, &a_err));
    EXPECT_FALSE(layer_err.WriteArrowBatch(&s, &a_err, opt_err.List()));
    if (a_err.release) a_err.release(&a_err);

    // FID > int32 max with format "i"
    UnpreservedFidLayer layer_large(true);
    struct ArrowArray a_large;
    EXPECT_TRUE(OGRCloneArrowArray(&s, &a, &a_large));
    CPLStringList opt_fid;
    opt_fid.SetNameValue("FID", "FID");
    EXPECT_TRUE(layer_large.WriteArrowBatch(&s, &a_large, opt_fid.List()));
    if (a_large.release) a_large.release(&a_large);

    if (a.release) a.release(&a);
    if (s.release) s.release(&s);
  }

  // 64-bit FID column
  {
    ComprehensiveBatchBuilder b;
    b.AddPrimitiveColumn<int64_t>("FID", "l", 100LL, 200LL);
    b.AddPrimitiveColumn<int32_t>("val", "i", 1, 2);
    b.Finalize();

    struct ArrowSchema s{};
    struct ArrowArray a{};
    EXPECT_TRUE(OGRCloneArrowSchema(&b.root_schema_, &s));
    EXPECT_TRUE(OGRCloneArrowArray(&b.root_schema_, &b.root_array_, &a));

    UnpreservedFidLayer layer_fid64(true);
    CPLStringList opt_fid;
    opt_fid.SetNameValue("FID", "FID");
    EXPECT_TRUE(layer_fid64.WriteArrowBatch(&s, &a, opt_fid.List()));

    if (a.release) a.release(&a);
    if (s.release) s.release(&s);
  }

  // 2. Lossy field conversions
  {
    CPLStringList opt_err;
    opt_err.SetNameValue("IF_FIELD_NOT_PRESERVED", "ERROR");

    // Int64 -> Int32 lossy
    {
      auto ds = std::make_unique<MEMDataset>();
      auto* lyr = ds->CreateLayer("test", nullptr, wkbNone);
      OGRFieldDefn fld("num", OFTInteger);
      EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&fld));

      ComprehensiveBatchBuilder b;
      b.AddPrimitiveColumn<int64_t>("num", "l", 10000000000LL, 20000000000LL);
      b.Finalize();
      EXPECT_FALSE(lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_,
                                        opt_err.List()));
    }

    // Real -> Int32 lossy
    {
      auto ds = std::make_unique<MEMDataset>();
      auto* lyr = ds->CreateLayer("test", nullptr, wkbNone);
      OGRFieldDefn fld("num", OFTInteger);
      EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&fld));

      ComprehensiveBatchBuilder b;
      b.AddPrimitiveColumn<double>("num", "g", 3.14, 2.71);
      b.Finalize();
      EXPECT_FALSE(lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_,
                                        opt_err.List()));
    }

    // Real -> Int64 lossy
    {
      auto ds = std::make_unique<MEMDataset>();
      auto* lyr = ds->CreateLayer("test", nullptr, wkbNone);
      OGRFieldDefn fld("num", OFTInteger64);
      EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&fld));

      ComprehensiveBatchBuilder b;
      b.AddPrimitiveColumn<double>("num", "g", 1.5, 2.5);
      b.Finalize();
      EXPECT_FALSE(lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_,
                                        opt_err.List()));
    }

    // Int64 -> Real lossy
    {
      auto ds = std::make_unique<MEMDataset>();
      auto* lyr = ds->CreateLayer("test", nullptr, wkbNone);
      OGRFieldDefn fld("num", OFTReal);
      EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&fld));

      ComprehensiveBatchBuilder b;
      b.AddPrimitiveColumn<int64_t>("num", "l", 9223372036854775800LL,
                                    9223372036854775800LL);
      b.Finalize();
      EXPECT_FALSE(lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_,
                                        opt_err.List()));
    }

    // Non-lossy conversions
    {
      auto ds = std::make_unique<MEMDataset>();
      auto* lyr = ds->CreateLayer("test", nullptr, wkbNone);
      OGRFieldDefn fld1("num", OFTInteger64);
      EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&fld1));
      OGRFieldDefn fld2("dt", OFTDateTime);
      EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&fld2));

      ComprehensiveBatchBuilder b;
      b.AddPrimitiveColumn<int32_t>("num", "i", 42, 84);
      b.AddStringColumn("dt", "u", "2023-05-01T12:34:56Z",
                        "2023-05-02T12:34:56Z");
      b.Finalize();
      EXPECT_TRUE(lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_,
                                       opt_err.List()));
    }
  }

  // 3. BuildOGRFieldInfo error paths
  {
    auto ds = std::make_unique<MEMDataset>();
    auto* lyr = ds->CreateLayer("test", nullptr, wkbNone);
    OGRFieldDefn fld_date("fld_date", OFTDate);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&fld_date));
    OGRFieldDefn fld_int("fld_int", OFTInteger);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&fld_int));

    // Root not struct
    {
      ComprehensiveBatchBuilder b;
      b.root_schema_.format = b.AllocString("i");
      b.Finalize();
      EXPECT_FALSE(
          lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_, nullptr));
    }

    // Children count mismatch
    {
      ComprehensiveBatchBuilder b;
      b.AddPrimitiveColumn<int32_t>("col1", "i", 1, 2);
      b.Finalize();
      b.root_schema_.n_children = 2;
      EXPECT_FALSE(
          lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_, nullptr));
      b.root_schema_.n_children = 1;
    }

    // Dictionary parent not integer
    {
      ComprehensiveBatchBuilder b;
      b.AddDictionaryColumn("col_dict_bad", "f");
      b.Finalize();
      EXPECT_FALSE(
          lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_, nullptr));
    }

    // FID column format invalid
    {
      ComprehensiveBatchBuilder b;
      b.AddStringColumn("FID", "u", "1", "2");
      b.Finalize();
      CPLStringList opt;
      opt.SetNameValue("FID", "FID");
      EXPECT_FALSE(
          lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_, opt.List()));
    }

    // Geom column format invalid
    {
      ComprehensiveBatchBuilder b;
      b.AddPrimitiveColumn<int32_t>("geom", "i", 1, 2);
      b.Finalize();
      CPLStringList opt;
      opt.SetNameValue("GEOMETRY_NAME", "geom");
      EXPECT_FALSE(
          lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_, opt.List()));
    }

    // Geom column not found on wkbNone layer (GEOMETRY_NAME)
    {
      ComprehensiveBatchBuilder b;
      b.AddStringColumn("geom", "z", "\x01", "\x02");
      b.Finalize();
      CPLStringList opt;
      opt.SetNameValue("GEOMETRY_NAME", "geom");
      EXPECT_FALSE(
          lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_, opt.List()));
    }

    // Geom column not found on wkbNone layer (ARROW:extension:name metadata)
    {
      ComprehensiveBatchBuilder b;
      b.AddWKBGeometryColumn("geom", "z", EXTENSION_NAME_OGC_WKB);
      b.Finalize();
      EXPECT_FALSE(
          lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_, nullptr));
    }

    // Field not found in layer
    {
      ComprehensiveBatchBuilder b;
      b.AddPrimitiveColumn<int32_t>("not_in_layer", "i", 1, 2);
      b.Finalize();
      EXPECT_FALSE(
          lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_, nullptr));
    }

    // Type mismatches:
    // Arrow "i" vs OFTDate
    {
      ComprehensiveBatchBuilder b;
      b.AddPrimitiveColumn<int32_t>("fld_date", "i", 1, 2);
      b.Finalize();
      EXPECT_FALSE(
          lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_, nullptr));
    }
    // Arrow "+m" vs OFTInteger
    {
      ComprehensiveBatchBuilder b;
      b.AddMapColumn("fld_int");
      b.Finalize();
      EXPECT_FALSE(
          lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_, nullptr));
    }
    // Arrow "tss:" vs OFTInteger
    {
      ComprehensiveBatchBuilder b;
      b.AddPrimitiveColumn<int64_t>("fld_int", "tss:", 100, 200);
      b.Finalize();
      EXPECT_FALSE(
          lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_, nullptr));
    }
    // Arrow "w:4" vs OFTInteger
    {
      ComprehensiveBatchBuilder b;
      uint8_t d[] = {1, 2, 3, 4};
      b.AddFixedWidthBinaryColumn("fld_int", 4, d, d);
      b.Finalize();
      EXPECT_FALSE(
          lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_, nullptr));
    }
    // Arrow "+l" of "i" vs OFTInteger
    {
      ComprehensiveBatchBuilder b;
      b.AddListColumn<int32_t>("fld_int", "+l", "i", 1, 2);
      b.Finalize();
      EXPECT_FALSE(
          lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_, nullptr));
    }
    // Arrow "+l" of "d:10,2" vs OFTInteger
    {
      ComprehensiveBatchBuilder b;
      b.AddDecimalListColumn("fld_int", "+l", "d:10,2", 16);
      b.Finalize();
      EXPECT_FALSE(
          lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_, nullptr));
    }
    // Arrow "+l" of "d:bad" vs OFTInteger
    {
      ComprehensiveBatchBuilder b;
      b.AddDecimalListColumn("fld_int", "+l", "d:bad", 16);
      b.Finalize();
      EXPECT_FALSE(
          lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_, nullptr));
    }
    // Arrow "+l" of "d:50,2" vs OFTInteger
    {
      ComprehensiveBatchBuilder b;
      b.AddDecimalListColumn("fld_int", "+l", "d:50,2", 16);
      b.Finalize();
      EXPECT_FALSE(
          lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_, nullptr));
    }
    // Arrow "+l" of "+s" vs OFTInteger
    {
      ComprehensiveBatchBuilder b;
      b.AddStructListColumn("fld_int", "+l");
      b.Finalize();
      EXPECT_FALSE(
          lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_, nullptr));
    }
    // Arrow "+l" of unsupported child
    {
      ComprehensiveBatchBuilder b;
      b.AddPrimitiveColumn<int32_t>("fld_int", "+l", 1, 2);
      b.schema_children_.back()->n_children = 1;
      b.schema_children_.back()->children = static_cast<struct ArrowSchema**>(
          b.AllocBytes(sizeof(struct ArrowSchema*)));
      b.schema_children_.back()->children[0] =
          b.AllocSchema("item", "invalid_fmt");
      b.Finalize();
      EXPECT_FALSE(
          lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_, nullptr));
    }
    // Arrow "d:bad" vs OFTDate
    {
      ComprehensiveBatchBuilder b;
      b.AddDecimalColumn("fld_date", "d:bad", 16, 1, 2);
      b.Finalize();
      EXPECT_FALSE(
          lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_, nullptr));
    }
    // Arrow "d:50,2" vs OFTDate
    {
      ComprehensiveBatchBuilder b;
      b.AddDecimalColumn("fld_date", "d:50,2", 16, 1, 2);
      b.Finalize();
      EXPECT_FALSE(
          lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_, nullptr));
    }
    // Arrow "d:10,2" vs OFTDate
    {
      ComprehensiveBatchBuilder b;
      b.AddDecimalColumn("fld_date", "d:10,2", 16, 1, 2);
      b.Finalize();
      EXPECT_FALSE(
          lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_, nullptr));
    }
    // Arrow unsupported format
    {
      ComprehensiveBatchBuilder b;
      b.AddPrimitiveColumn<int32_t>("fld_date", "unsupported_fmt", 1, 2);
      b.Finalize();
      EXPECT_FALSE(
          lyr->WriteArrowBatch(&b.root_schema_, &b.root_array_, nullptr));
    }
  }

  // 4. Native types metadata and renamed field in CreateField
  {
    auto custom_ds = std::make_unique<CustomDriverDataset>();
    CustomDriverLayer custom_lyr("custom_layer", custom_ds.get());

    ComprehensiveBatchBuilder b;
    b.AddPrimitiveColumn<int32_t>("col_int32", "i", 10, 20);
    b.AddListColumn<int32_t>("col_intlist", "+l", "i", 1, 2);
    b.AddPrimitiveColumn<int32_t>("col_rename", "i", 100, 200);
    b.Finalize();

    for (int64_t i = 0; i < b.root_schema_.n_children; ++i) {
      EXPECT_TRUE(custom_lyr.CreateFieldFromArrowSchema(
          b.root_schema_.children[i], nullptr));
    }
    const auto* defn = custom_lyr.GetLayerDefn();
    EXPECT_EQ(OFTString,
              defn->GetFieldDefn(defn->GetFieldIndex("col_int32"))->GetType());
    EXPECT_EQ(
        OFTString,
        defn->GetFieldDefn(defn->GetFieldIndex("col_intlist"))->GetType());
    EXPECT_EQ(
        OFSTJSON,
        defn->GetFieldDefn(defn->GetFieldIndex("col_intlist"))->GetSubType());
    EXPECT_NE(-1, defn->GetFieldIndex("col_rename_renamed"));

    EXPECT_TRUE(
        custom_lyr.WriteArrowBatch(&b.root_schema_, &b.root_array_, nullptr));
    EXPECT_EQ(3, custom_lyr.GetFeatureCount(true));
  }

  // 5. PostFilterArrowArray with FIDColumn and no BASE_SEQUENTIAL_FID
  {
    // A: Int32 FID column
    {
      CustomDriverLayer lyr_fid("fid_i", nullptr, "FID");
      lyr_fid.SetAttributeFilter("FID = 10");

      ComprehensiveBatchBuilder b;
      b.AddPrimitiveColumn<int32_t>("FID", "i", 10, 20);
      b.Finalize();

      struct ArrowArray a_filter;
      memset(&a_filter, 0, sizeof(a_filter));
      EXPECT_TRUE(
          OGRCloneArrowArray(&b.root_schema_, &b.root_array_, &a_filter));
      ArrowTestLayer::TestPostFilter(&lyr_fid, &b.root_schema_, &a_filter,
                                     nullptr);
      EXPECT_EQ(1, a_filter.length);
      if (a_filter.release) a_filter.release(&a_filter);
    }

    // B: Int64 FID column
    {
      CustomDriverLayer lyr_fid64("fid_l", nullptr, "FID");
      lyr_fid64.SetAttributeFilter("FID = 100");

      ComprehensiveBatchBuilder b;
      b.AddPrimitiveColumn<int64_t>("FID", "l", 100LL, 200LL);
      b.Finalize();

      struct ArrowArray a_filter;
      memset(&a_filter, 0, sizeof(a_filter));
      EXPECT_TRUE(
          OGRCloneArrowArray(&b.root_schema_, &b.root_array_, &a_filter));
      ArrowTestLayer::TestPostFilter(&lyr_fid64, &b.root_schema_, &a_filter,
                                     nullptr);
      EXPECT_EQ(1, a_filter.length);
      if (a_filter.release) a_filter.release(&a_filter);
    }

    // C: Nested struct FID column ("top.FID") with null parent struct
    {
      CustomDriverLayer lyr_nested("fid_nested", nullptr, "top.FID");
      lyr_nested.SetAttributeFilter("FID = 10");

      ComprehensiveBatchBuilder b;
      auto* top_s = b.AllocSchema("top", "+s");
      top_s->n_children = 1;
      top_s->children = static_cast<struct ArrowSchema**>(
          b.AllocBytes(sizeof(struct ArrowSchema*)));
      top_s->children[0] = b.AllocSchema("FID", "i");

      auto* top_a = b.AllocArray(3, 1);
      top_a->n_buffers = 1;
      top_a->buffers = static_cast<const void**>(b.AllocBytes(sizeof(void*)));
      top_a->buffers[0] = b.AllocValidity3();
      top_a->n_children = 1;
      top_a->children = static_cast<struct ArrowArray**>(
          b.AllocBytes(sizeof(struct ArrowArray*)));

      auto fid_child = b.CreateChildPrimitive<int32_t>("FID", "i", 10, 20, 3);
      top_a->children[0] = fid_child.second;

      b.schema_children_.push_back(top_s);
      b.array_children_.push_back(top_a);
      b.Finalize();

      struct ArrowArray a_filter;
      memset(&a_filter, 0, sizeof(a_filter));
      EXPECT_TRUE(
          OGRCloneArrowArray(&b.root_schema_, &b.root_array_, &a_filter));
      ArrowTestLayer::TestPostFilter(&lyr_nested, &b.root_schema_, &a_filter,
                                     nullptr);
      EXPECT_EQ(2, a_filter.length);
      if (a_filter.release) a_filter.release(&a_filter);
    }

    // D: Unknown FID column (error path)
    {
      CustomDriverLayer lyr_bad("bad_fid", nullptr, "nonexistent_fid");
      lyr_bad.SetAttributeFilter("FID = 10");

      ComprehensiveBatchBuilder b;
      b.AddPrimitiveColumn<int32_t>("val", "i", 1, 2);
      b.Finalize();

      struct ArrowArray a_filter;
      memset(&a_filter, 0, sizeof(a_filter));
      EXPECT_TRUE(
          OGRCloneArrowArray(&b.root_schema_, &b.root_array_, &a_filter));
      ArrowTestLayer::TestPostFilter(&lyr_bad, &b.root_schema_, &a_filter,
                                     nullptr);
      if (a_filter.release) a_filter.release(&a_filter);
    }

    // E: Nested struct field in attribute filter with null parent struct
    {
      CustomDriverLayer lyr_nested_fld("nested_fld", nullptr, "");
      OGRFieldDefn fld("top.sub_val", OFTInteger);
      EXPECT_EQ(OGRERR_NONE, lyr_nested_fld.CreateField(&fld));
      lyr_nested_fld.SetAttributeFilter("top.sub_val = 10");

      ComprehensiveBatchBuilder b;
      auto* top_s = b.AllocSchema("top", "+s");
      top_s->n_children = 1;
      top_s->children = static_cast<struct ArrowSchema**>(
          b.AllocBytes(sizeof(struct ArrowSchema*)));
      top_s->children[0] = b.AllocSchema("sub_val", "i");

      auto* top_a = b.AllocArray(3, 1);
      top_a->n_buffers = 1;
      top_a->buffers = static_cast<const void**>(b.AllocBytes(sizeof(void*)));
      top_a->buffers[0] = b.AllocValidity3();
      top_a->n_children = 1;
      top_a->children = static_cast<struct ArrowArray**>(
          b.AllocBytes(sizeof(struct ArrowArray*)));

      auto val_child =
          b.CreateChildPrimitive<int32_t>("sub_val", "i", 10, 20, 3);
      top_a->children[0] = val_child.second;

      b.schema_children_.push_back(top_s);
      b.array_children_.push_back(top_a);
      b.Finalize();

      struct ArrowArray a_filter;
      memset(&a_filter, 0, sizeof(a_filter));
      EXPECT_TRUE(
          OGRCloneArrowArray(&b.root_schema_, &b.root_array_, &a_filter));
      ArrowTestLayer::TestPostFilter(&lyr_nested_fld, &b.root_schema_,
                                     &a_filter, nullptr);
      EXPECT_EQ(2, a_filter.length);
      if (a_filter.release) a_filter.release(&a_filter);
    }
  }
}

TEST(OGRLayerArrowTest, CreateFieldFromArrowSchemaMetadataAndErrors) {
  WithQuietHandler quiet_handler;
  CPLErrorReset();
  auto ds = CreatePopulatedDataset();
  OGRLayer* lyr = ds->GetLayer(0);

  ComprehensiveBatchBuilder b;

  // Helper to build metadata
  auto make_meta =
      [&b](const std::vector<std::pair<std::string, std::string>>& kvs)
      -> const char* {
    int32_t count = static_cast<int32_t>(kvs.size());
    std::string buf;
    buf.append(reinterpret_cast<const char*>(&count), sizeof(count));
    for (const auto& [k, v] : kvs) {
      int32_t klen = static_cast<int32_t>(k.size());
      int32_t vlen = static_cast<int32_t>(v.size());
      buf.append(reinterpret_cast<const char*>(&klen), sizeof(klen));
      buf.append(k);
      buf.append(reinterpret_cast<const char*>(&vlen), sizeof(vlen));
      buf.append(v);
    }
    char* p = static_cast<char*>(b.AllocBytes(buf.size()));
    memcpy(p, buf.data(), buf.size());
    return p;
  };

  // 1. All metadata keys on CreateFieldFromArrowSchema
  {
    const char* meta_buf = make_meta({
        {"GDAL:OGR:type", "Real"},
        {"GDAL:OGR:alternative_name", "alt_fld"},
        {"GDAL:OGR:comment", "test comment"},
        {"GDAL:OGR:default", "3.14"},
        {"GDAL:OGR:subtype", "Float32"},
        {"GDAL:OGR:width", "15"},
        {"GDAL:OGR:unique", "true"},
        {"GDAL:OGR:domain_name", "valid_domain"},
        {ARROW_EXTENSION_NAME_KEY, EXTENSION_NAME_ARROW_JSON},
        {"unknown_metadata_key", "val"},
    });

    auto* s = b.AllocSchema("meta_field", "g");
    s->metadata = meta_buf;
    EXPECT_TRUE(lyr->CreateFieldFromArrowSchema(s, nullptr));

    int idx = lyr->GetLayerDefn()->GetFieldIndex("meta_field");
    EXPECT_GE(idx, 0);
    auto* fld = lyr->GetLayerDefn()->GetFieldDefn(idx);
    EXPECT_STREQ("alt_fld", fld->GetAlternativeNameRef());
    EXPECT_STREQ("test comment", fld->GetComment().c_str());
    EXPECT_STREQ("3.14", fld->GetDefault());
    EXPECT_EQ(15, fld->GetWidth());
    EXPECT_TRUE(fld->IsUnique());
    EXPECT_STREQ("valid_domain", fld->GetDomainName().c_str());
    EXPECT_EQ(OFSTFloat32, fld->GetSubType());
  }

  // Extensions: google:sqlType:json and unknown extension
  {
    const char* meta_bq = make_meta({
        {ARROW_EXTENSION_NAME_KEY, "google:sqlType:json"},
    });
    auto* s_bq = b.AllocSchema("bq_json", "u");
    s_bq->metadata = meta_bq;
    EXPECT_TRUE(lyr->CreateFieldFromArrowSchema(s_bq, nullptr));
    int idx_bq = lyr->GetLayerDefn()->GetFieldIndex("bq_json");
    EXPECT_GE(idx_bq, 0);
    EXPECT_EQ(OFSTJSON,
              lyr->GetLayerDefn()->GetFieldDefn(idx_bq)->GetSubType());

    const char* meta_json = make_meta({
        {ARROW_EXTENSION_NAME_KEY, EXTENSION_NAME_ARROW_JSON},
    });
    auto* s_json = b.AllocSchema("arrow_json", "u");
    s_json->metadata = meta_json;
    EXPECT_TRUE(lyr->CreateFieldFromArrowSchema(s_json, nullptr));
    int idx_json = lyr->GetLayerDefn()->GetFieldIndex("arrow_json");
    EXPECT_GE(idx_json, 0);
    EXPECT_EQ(OFSTJSON,
              lyr->GetLayerDefn()->GetFieldDefn(idx_json)->GetSubType());

    const char* meta_unk = make_meta({
        {ARROW_EXTENSION_NAME_KEY, "custom:unknown"},
    });
    auto* s_unk = b.AllocSchema("unk_ext", "u");
    s_unk->metadata = meta_unk;
    EXPECT_TRUE(lyr->CreateFieldFromArrowSchema(s_unk, nullptr));
  }

  // C API: OGR_L_CreateFieldFromArrowSchema
  {
    auto* s = b.AllocSchema("c_api_fld", "i");
    EXPECT_TRUE(OGR_L_CreateFieldFromArrowSchema(
        reinterpret_cast<OGRLayerH>(lyr), s, nullptr));
    EXPECT_FALSE(OGR_L_CreateFieldFromArrowSchema(nullptr, s, nullptr));
    EXPECT_FALSE(OGR_L_CreateFieldFromArrowSchema(
        reinterpret_cast<OGRLayerH>(lyr), nullptr, nullptr));
  }

  // Decimal field creation and error paths in CreateFieldFromArrowSchema
  {
    auto* s_dec = b.AllocSchema("fld_dec", "d:10,2");
    EXPECT_TRUE(lyr->CreateFieldFromArrowSchema(s_dec, nullptr));

    auto* s_dec_list = b.AllocSchema("fld_dec_list", "+l");
    s_dec_list->n_children = 1;
    s_dec_list->children = static_cast<struct ArrowSchema**>(
        b.AllocBytes(sizeof(struct ArrowSchema*)));
    s_dec_list->children[0] = b.AllocSchema("item", "d:10,2");
    EXPECT_TRUE(lyr->CreateFieldFromArrowSchema(s_dec_list, nullptr));

    // Decimal bad format errors
    auto* s_dec_bad = b.AllocSchema("fld_dec_bad", "d:bad");
    EXPECT_FALSE(lyr->CreateFieldFromArrowSchema(s_dec_bad, nullptr));

    auto* s_dec_50 = b.AllocSchema("fld_dec_50", "d:50,2");
    EXPECT_FALSE(lyr->CreateFieldFromArrowSchema(s_dec_50, nullptr));

    auto* s_dec_list_bad = b.AllocSchema("fld_dec_lbad", "+l");
    s_dec_list_bad->n_children = 1;
    s_dec_list_bad->children = static_cast<struct ArrowSchema**>(
        b.AllocBytes(sizeof(struct ArrowSchema*)));
    s_dec_list_bad->children[0] = b.AllocSchema("item", "d:bad");
    EXPECT_FALSE(lyr->CreateFieldFromArrowSchema(s_dec_list_bad, nullptr));

    auto* s_dec_list_50 = b.AllocSchema("fld_dec_l50", "+l");
    s_dec_list_50->n_children = 1;
    s_dec_list_50->children = static_cast<struct ArrowSchema**>(
        b.AllocBytes(sizeof(struct ArrowSchema*)));
    s_dec_list_50->children[0] = b.AllocSchema("item", "d:50,2");
    EXPECT_FALSE(lyr->CreateFieldFromArrowSchema(s_dec_list_50, nullptr));

    // Unsupported list child
    auto* s_unsupp_list = b.AllocSchema("fld_unsupp_list", "+l");
    s_unsupp_list->n_children = 1;
    s_unsupp_list->children = static_cast<struct ArrowSchema**>(
        b.AllocBytes(sizeof(struct ArrowSchema*)));
    s_unsupp_list->children[0] = b.AllocSchema("item", "invalid_fmt");
    EXPECT_FALSE(lyr->CreateFieldFromArrowSchema(s_unsupp_list, nullptr));

    // Unsupported field format
    auto* s_unsupp = b.AllocSchema("fld_unsupp", "invalid_fmt");
    EXPECT_FALSE(lyr->CreateFieldFromArrowSchema(s_unsupp, nullptr));
  }

  // IsArrowSchemaSupported and C API OGR_L_IsArrowSchemaSupported
  {
    std::string err;

    // Root not struct
    auto* s_non_struct = b.AllocSchema("not_struct", "i");
    EXPECT_FALSE(lyr->IsArrowSchemaSupported(s_non_struct, nullptr, err));

    // Root struct with various supported & unsupported fields
    auto* s_root = b.AllocSchema("", "+s");
    s_root->n_children = 8;
    s_root->children = static_cast<struct ArrowSchema**>(
        b.AllocBytes(8 * sizeof(struct ArrowSchema*)));

    // 0: bad dict
    auto* s_bad_dict = b.AllocSchema("bad_dict", "f");
    s_bad_dict->dictionary = b.AllocSchema("dict", "u");
    s_root->children[0] = s_bad_dict;

    // 1: bad decimal
    s_root->children[1] = b.AllocSchema("bad_dec", "d:bad");

    // 2: decimal > 38
    s_root->children[2] = b.AllocSchema("bad_dec50", "d:50,2");

    // 3: bad list decimal
    auto* s_l_dec_bad = b.AllocSchema("l_dec_bad", "+l");
    s_l_dec_bad->n_children = 1;
    s_l_dec_bad->children = static_cast<struct ArrowSchema**>(
        b.AllocBytes(sizeof(struct ArrowSchema*)));
    s_l_dec_bad->children[0] = b.AllocSchema("item", "d:bad");
    s_root->children[3] = s_l_dec_bad;

    // 4: list decimal > 38
    auto* s_l_dec_50 = b.AllocSchema("l_dec_50", "+l");
    s_l_dec_50->n_children = 1;
    s_l_dec_50->children = static_cast<struct ArrowSchema**>(
        b.AllocBytes(sizeof(struct ArrowSchema*)));
    s_l_dec_50->children[0] = b.AllocSchema("item", "d:50,2");
    s_root->children[4] = s_l_dec_50;

    // 5: bad list child
    auto* s_l_bad = b.AllocSchema("l_bad", "+l");
    s_l_bad->n_children = 1;
    s_l_bad->children = static_cast<struct ArrowSchema**>(
        b.AllocBytes(sizeof(struct ArrowSchema*)));
    s_l_bad->children[0] = b.AllocSchema("item", "invalid_fmt");
    s_root->children[5] = s_l_bad;

    // 6: bad map
    auto* s_map_bad = b.AllocSchema("map_bad", "+m");
    s_map_bad->n_children = 1;
    s_map_bad->children = static_cast<struct ArrowSchema**>(
        b.AllocBytes(sizeof(struct ArrowSchema*)));
    auto* map_struct = b.AllocSchema("entries", "+s");
    map_struct->n_children = 2;
    map_struct->children = static_cast<struct ArrowSchema**>(
        b.AllocBytes(2 * sizeof(struct ArrowSchema*)));
    map_struct->children[0] = b.AllocSchema("k", "i");  // bad key format
    map_struct->children[1] = b.AllocSchema("v", "i");
    s_map_bad->children[0] = map_struct;
    s_root->children[6] = s_map_bad;

    // 7: unsupported format
    s_root->children[7] = b.AllocSchema("unsupp", "invalid_fmt");

    EXPECT_FALSE(lyr->IsArrowSchemaSupported(s_root, nullptr, err));
    EXPECT_FALSE(err.empty());

    // C API: OGR_L_IsArrowSchemaSupported
    char* pszErrorMsg = nullptr;
    EXPECT_FALSE(OGR_L_IsArrowSchemaSupported(reinterpret_cast<OGRLayerH>(lyr),
                                              s_root, nullptr, &pszErrorMsg));
    EXPECT_NE(nullptr, pszErrorMsg);
    VSIFree(pszErrorMsg);

    // C API null validation
    EXPECT_FALSE(
        OGR_L_IsArrowSchemaSupported(nullptr, s_root, nullptr, nullptr));
    EXPECT_FALSE(OGR_L_IsArrowSchemaSupported(reinterpret_cast<OGRLayerH>(lyr),
                                              nullptr, nullptr, nullptr));
  }
  CPLErrorReset();
}

// TODO: b/335317901 - Cover remaining error branches in ogrlayerarrow.cpp.

}  // namespace
}  // namespace autotest2
